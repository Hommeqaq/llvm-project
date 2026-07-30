# EJIT Cross-TU Inlining 设计文档

**版本**: 1.1
**日期**: 2026-07-30
**关联**: SPEC4.md, PLAN4.md, PASS1_EJitRegisterBitcode.md

---

## 1. 概述

### 1.1 背景

当前 `ejit_entry` 函数调用其他 TU 的子函数时，PASS1 在编译期运行，只能看到当前 TU。跨 TU 的子函数在 bitcode 中表现为 `declare external` 声明，PASS1 无法将其内联。JIT 编译时仍有跨函数调用开销。

之前的方案是开启全局 ThinLTO，但 ThinLTO 会对整个程序生效——AOT 代码也被跨模块优化，增加了编译时间并影响了 AOT 代码的确定性。

### 1.2 目标

通过新选项 `-fejit-cross-inline`，让**只有** `__ejit_bitcode` 获得跨 TU 内联优化，AOT 代码走普通编译流程。

### 1.3 核心思路

将 PASS1 拆为两阶段：

- **编译期**：完整 Module IR 嵌入 `.ejit_cross` ELF section（不提取闭包，不生成 `__ejit_bitcode`）
- **链接期**：读取所有 `.ejit_cross` section → 合并 → 内联跨 TU 子函数 → 生成最终的 `__ejit_bitcode`

---

## 2. 整体流程

```
┌─────────────────────────────────────────────────────────────────┐
│  编译期 (-fejit-cross-inline -c)                                │
│                                                                 │
│  clang → IR → PASS1(cross-inline 模式)                         │
│                  │                                              │
│                  └─ 完整 Module 序列化为 bitcode                 │
│                     embedBitcodeInSection( ".ejit_cross" )      │
│                     不生成 @__ejit_bitcode                      │
│                     不生成 ejit_auto_register                   │
│                  │                                              │
│                  ↓                                              │
│              tu_a.o (含 .ejit_cross section)                    │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  链接期 (-fejit-cross-inline *.o -o output)                     │
│  (-r 部分链接和最终链接处理逻辑完全相同)                          │
│                                                                 │
│  链接期 (-fejit-cross-inline *.o -o output, 需 -fuse-ld=lld)     │
│  (-r 部分链接和最终链接处理逻辑完全相同)                          │
│                                                                 │
│  ld.lld (唯一 owner, 仅在 --ejit-cross-inline 时执行一次):       │
│    1. 扫描所有输入 .o, 提取 .ejit_cross section                 │
│    2. parseBitcodeFile → 每个 section 解析为一个 Module          │
│    3. llvm::Linker::linkInModule() → 合并所有 Module            │
│    4. 找 ejit_entry → computeTransitiveClosure (保守引用收集)    │
│    5. AlwaysInliner + ModuleInlinerWrapperPass → 真实内联跨 TU  │
│       普通函数 (成本模型驱动, 非仅 always_inline)               │
│    6. reAnnotateMayConst (复用 EJitCommon.h canonical resolver)  │
│    7. 对每个 ejit_entry 单独:                                   │
│       CloneModule → computeTransitiveClosure → trim             │
│       → verifyModule → 独立 @__ejit_bitcode_<name>             │
│    8. generateRegistryTable (registry 全部常量属于 TmpM;         │
│       external func/global 在 TmpM 内建 declaration)            │
│    9. verifyModule(TmpM) → 失败则链接失败                       │
│   10. WriteBitcodeToFile → 临时 .bc; 链接后由 Driver 清理        │
│   11. lld 链接 (原始 .o + 临时 .bc), 丢弃原始 .ejit_cross        │
│                                                                 │
│  任一阶段失败 → 链接失败 (绝不静默 fallback 到 AOT)             │
│  输出: output 中无 .ejit_cross, 只有一套 @__ejit_bitcode         │
└─────────────────────────────────────────────────────────────────┘
```

### 2.1 `-r` 部分链接行为

```
clang -fejit-cross-inline -r tu_a.o tu_b.o -o combined.o
  → combined.o 中只有 @__ejit_bitcode, 无 .ejit_cross

clang -fejit-cross-inline combined.o tu_c.o -o app
  → 读取 tu_c.o 的 .ejit_cross + combined.o 的 @__ejit_bitcode
  → 合并处理 → 生成新的 @__ejit_bitcode
```

---

## 3. 编译期详细设计

### 3.1 新增 flag

**文件**: `llvm/lib/Passes/PassBuilderPipelines.cpp`

flag 必须放在 `PassBuilderPipelines.cpp`（`LLVMPasses` 库）而非 `EJitPassOptions.cpp`，因为 clang 解析 `cl::opt` 早于 `LLVMEmbeddedJIT` 的静态初始化。

```cpp
cl::opt<bool> EJitCrossInline(
    "ejit-cross-inline", cl::init(false), cl::Hidden,
    cl::desc("Defer bitcode extraction to link time for cross-TU inlining"));
```

flag 值通过 `EJitRegisterBitcodePass(bool CrossInline)` 构造函数传入。

### 3.2 PASS1 分支逻辑

**文件**: `llvm/lib/Transforms/EmbeddedJIT/EJitRegisterBitcode.cpp`

```cpp
struct EJitRegisterBitcodePass {
  bool CrossInline = false;
  // ...
};

PreservedAnalyses run(Module &M, ...) {
  if (CrossInline) {
    embedBitcodeInSection(M, ".ejit_cross");
    return PreservedAnalyses::none();
  }
  // 原有逻辑不变 ...
}
```

### 3.3 embedBitcodeInSection

```cpp
static void embedBitcodeInSection(Module &M, const std::string &BC,
                                    StringRef SectionName) {
  LLVMContext &Ctx = M.getContext();
  auto *ArrTy = ArrayType::get(Type::getInt8Ty(Ctx), BC.size());
  auto *Const = ConstantDataArray::get(Ctx, StringRef(BC.data(), BC.size()));
  auto *GV = new GlobalVariable(M, ArrTy, true, GlobalValue::InternalLinkage,
                                Const, "__ejit_cross_module");
  GV->setSection(SectionName);
  GV->setAlignment(Align(1));
  // 加入 llvm.compiler.used 防止编译器丢弃此 unreferenced global，
  // 但允许链接器 GC（--gc-sections）。ld.lld 在 cross-link 处理后仅对
  // EJitCrossLinkResult::consumedFiles 中的输入主动丢弃此 section。
  appendToCompilerUsed(M, {GV});
}
```

### 3.4 与现有 PASS 顺序的关系

PASS1 (cross-inline 模式) 仍在 `buildPerModuleDefaultPipeline` 的最前面运行。
不生成 wrapper — 由后续的 PASS3 (EJitWrapperGenPass) 正常生成。
PASS2 (period 注册) 不受影响。

### 3.5 Clang Driver 编译选项

**文件**: `clang/include/clang/Driver/Options.td`

```
def fejit_cross_inline : Flag<["-"], "fejit-cross-inline">,
  HelpText<"Enable cross-TU inlining for EJIT entry functions">,
  Visibility<[ClangOption, CC1Option]>;
```

**文件**: `clang/lib/Driver/ToolChains/Clang.cpp`

在编译 Job 构造时（`ConstructJob`）传递：

```cpp
if (Args.hasArg(options::OPT_fejit_cross_inline)) {
  CmdArgs.push_back("-mllvm");
  CmdArgs.push_back("-ejit-cross-inline");
}
```

---

## 4. 链接期详细设计

### 4.1 核心函数

唯一实现文件：**`lld/ELF/EJitCrossLink.cpp`**（clang driver 复制版已删除）

```cpp
/// 链接期跨 TU 内联处理。
/// 从 lld 实际选中的目标文件（ctx.objectFiles）中提取 .ejit_cross section，
/// 合并、内联、生成 registry，写入临时 .bc，并返回路径及被精确消费的输入集合。
/// SelectedObjects 的每个 MemoryBufferRef 标识必须是 canonical ObjFile::getName()。
/// 无 .ejit_cross 时返回空结果；检测后的任何失败返回 Error。
Expected<EJitCrossLinkResult>
runEJitCrossLink(ArrayRef<MemoryBufferRef> SelectedObjects,
                 StringRef TargetTriple, StringRef SaveTempsPrefix = {});
```

> **调用时机（archive member selection）**：`runEJitCrossLink` 在 lld 完成符号解析
> **之后**运行（`addWrappedSymbols` 之后、`initSectionsAndLocalSyms` 之前），此时
> `ctx.objectFiles` 恰好是被链接选中的目标文件集合——包含被实际拉入链接的 archive
> member、`-l` 输入与 linker-script 输入。因此只处理被选中的 member，不重新实现一套
> ELF 符号选择，未被选中的 member 既不进入 composite 也不进入 registry。早期版本在
> `parseFiles` 之前无条件合并 archive 内所有带 `.ejit_cross` 的 member，语义错误且会
> 拖慢链接，已被移除。


### 4.2 处理流程

```
runEJitCrossLink(SelectedObjects, TargetTriple):

  1. 扫描 + 提取 + 合并（合并为一次遍历）:
     for each MemoryBufferRef in SelectedObjects:   // 只遍历被选中的目标文件
       ObjectFile::createObjectFile()
       找到 .ejit_cross section:
         parseBitcodeFile(section contents) → Module
         Composite 为空 → std::move；否则 Linker::linkInModule
       consumedFiles += buffer identifier            // == ObjFile::getName()
     无 .ejit_cross → return EJitCrossLinkResult{}

  2. 闭包计算 (union):
     collectEntryFunctions(Composite) → EntryFuncs
     computeTransitiveClosure(EntryFuncs) → ClosureFuncs + ClosureGlobals + ClosureIndirect
     扫描 operand、常量表达式/聚合、alias/ifunc、全局 initializer
     deleteBody/setInitializer(nullptr) + GlobalDCE 安全裁剪

  3. 内联跨 TU 子函数:
     将 JIT-only composite definitions 标记 dso_local
     AlwaysInlinerPass + ModuleInlinerWrapperPass (成本模型)

  4. 轻量 cleanup + metadata 恢复:
     Promote → InstCombine → SimplifyCFG
     reAnnotateMayConst(Composite)

  5. 对每个 ejit_entry 单独提取闭包 + 序列化 (closure-only clone，见 4.6):
     for each EntryFunc:
       computeTransitiveClosure({EntryFunc}) → SrcFuncs/SrcGlobals/SrcIndirect  // 在 Composite 上
       PerFuncModule = CloneModule(Composite, VMap, ShouldClone=闭包成员)        // 只克隆闭包定义
       擦除闭包外的 ifunc（CloneModule 对 ifunc 不遵守 ShouldClone 回调）
       trimToClosure → GlobalDCE 清理残留声明
       externalize mutable globals
       collectExternalSymbols → 在 TmpModule 中建立稳定 declaration
       verifyModule(*PerFuncModule)
       serializeToBitcode(*PerFuncModule) → BC

  6. 生成临时 Module (含 N 个 @__ejit_bitcode_<name> + 单一 registry):
     TmpModule = new Module("ejit_cross_link", Ctx)
     复制 DataLayout + TargetTriple
     for each EntryFunc + its BC:
       embedBitcode(TmpModule, BC, "__ejit_bitcode_" + funcName)
     generateRegistryTable(TmpModule, entries, external symbols)
     verifyModule(TmpModule)

  7. 写入临时 .bc:
     WriteBitcodeToFile(TmpModule) → 临时 .bc 文件

  8. return {tmpfile path, consumed input files}
```

> 每个阶段都包裹在 `llvm::TimeTraceScope("EJitCross:*")` 中（`DetectScan`、
> `ParseBitcode`、`MergeModule`、`EntryScan`、`UnionClosure`、`Inliner`、
> `PerEntryExtraction`、`PerEntryClosure`、`PerEntryClone`、`PerEntryTrim`、
> `PerEntryVerify`、`PerEntrySerialize`、`RegistryGen`）。这些 scope 只在
> `ld.lld --time-trace` 初始化 profiler 时才产生记录；关闭 time trace 时每个 scope
> 仅多一次 TimeTraceScope 的 inactive-profiler 分支，不产生任何记录或输出（并非
> 编译期完全消除）。使用方法见 4.7。


### 4.3 与 PASS1 共享的语义

cross-link 的闭包、内联和 registry 生成属于 lld，不再复制一份 clang/PASS1
实现。`may_const` 指针形态解析直接复用 `EJitCommon.h` 中的
`ejitMayConstFieldOffset` 与 `ejitAccessFitsMayConstField`，保证与运行时优化 pass
使用同一套保守规则。

### 4.4 Driver 集成点（PR #102 hardening: ld.lld 为唯一 owner）

为保证跨 TU 合并/注册**恰好执行一次**，`ld.lld` 是唯一的 cross-link owner；clang
driver 自身**不再**执行任何合并。协议如下：

1. **clang driver**: `clang/lib/Driver/ToolChains/Gnu.cpp` — `Linker::ConstructJob`
   中，当 `-fejit-cross-inline` 生效时用 `ToolChain::GetLinkerPath(&LinkerIsLLD)`
   判断链接器：
   - 若不是 lld → `err_drv_argument_only_allowed_with` 硬报错（`-fejit-cross-inline`
     只允许配合 `-fuse-ld=lld`）。绝不生成 GNU ld 无法消费的 `.bc`，也绝不把巨大的
     `.ejit_cross` 静默留在最终产物。
   - 若是 lld → 仅向链接命令行追加 `--ejit-cross-inline`，把处理交给 lld。
2. **ld.lld**: `lld/ELF/Driver.cpp` — `LinkerDriver::link()` 中，**仅当
   `--ejit-cross-inline` 存在**（`ctx.arg.ejitCrossInline`）时调用
   `runEJitCrossLink`。调用点在**符号解析完成之后**（`addWrappedSymbols` 之后、
   `initSectionsAndLocalSyms` 之前），传入 `ctx.objectFiles`（被选中的目标文件）
   的 `MemoryBufferRef`：
   - 返回 `Expected<EJitCrossLinkResult>`：`Error` → `ErrAlways(ctx)` 令链接失败；
     空 `tempPath` → 无 `.ejit_cross`（正常跳过，默认行为完全不变）；非空 →
     临时 `.bc` 路径和被精确消费的输入文件集合。
   - `consumedFiles` 用 `ObjFile::getName()`（即 buffer identifier）标识，`InputFiles.cpp`
     只丢弃 `ctx.ejitCrossConsumedFiles` 中列出的输入的 `.ejit_cross` section。因为
     只有被选中的目标文件被处理并消费，任何仍带未处理 `.ejit_cross` 的**被选中**
     输入都会被 `rejectUnprocessedEJitCross` 硬报错，绝不静默丢弃。
   - 生成的 registry 是 bitcode：用 `addFile` 加入 `files` 后立即
     `parseFile(ctx, ...)` 解析，使其进入 `ctx.bitcodeFiles`，随后由既有的 LTO
     步骤（`compileBitcodeFiles`）编译为携带 `.ejit_bitcode` 的目标文件。它对 AOT
     符号的引用绑定到已选中的定义；由于在符号解析之后加入，绝不会再拉入新的 member。
   - 临时 `.bc` 被 `parseFile` 读入内存后立即 `sys::fs::remove` 删除。

> **旧实现的问题（本轮修复）**：早期 `runEJitCrossLink` 在 `parseFiles` 之前扫描
> `OPT_INPUT` 路径，用一个**全局** `HasCross` 布尔在 archive member 循环里 `break`。
> 只要更早的普通目标文件已把 `HasCross` 置真，后续 archive 就只检查第一个 member，
> 后面 member 里的 ejit_entry 会被漏掉，链接以 "no ejit_entry" 失败。同时它无条件
> 合并 archive 内**所有**带 `.ejit_cross` 的 member（含未被链接选中的），既污染
> registry 又拖慢链接。改为符号解析后遍历 `ctx.objectFiles` 后，这两个问题都从
> 结构上消除，`-l` / linker-script 输入也自然被支持。


> clang 复制版 `clang/lib/Driver/EJitCrossLink.cpp` 已删除，因此实现只剩 lld 一份，
> 不存在两份实现漂移（Area 6）。`.ejit_cross` 里的 may_const 语义直接复用
> `EJitCommon.h` 的 `ejitMayConstFieldOffset` / `ejitAccessFitsMayConstField`
> canonical resolver，不再维护过时的简化版。

```
# clang 驱动（必须使用 lld）
clang -fejit-cross-inline -fuse-ld=lld -r tu_a.o tu_b.o -o combined.o

# 直接用 ld.lld
ld.lld --ejit-cross-inline -r tu_a.o tu_b.o -o combined.o
```

### 4.5 每函数独立 bitcode 设计

**为什么不共用一份 `@__ejit_bitcode`？**

如果所有 ejit_entry 共用一份 bitcode，JIT 编译 `jit_board_check` 时需要 parse 整个 Module（含其他 3 个函数的全部代码和跨 TU 内联来的代码），浪费 parse 时间和内存。

**方案**：对每个 ejit_entry 单独提取闭包 + 独立序列化：

```llvm
@__ejit_bitcode_board = internal constant [N] <jid_board_check + closure>  ; 只含自己的闭包
@__ejit_bitcode_cell  = internal constant [M] <jid_cell_check + closure>
@__ejit_bitcode_chain = internal constant [K] <jid_chain_check + closure>
@__ejit_bitcode_prior = internal constant [L] <jid_priority + closure>
```

**JIT 编译时**：按 funcIdx 取出对应那份 bitcode → parse → `getFunction(funcName)` → 编译。Module 里只含当前函数 + 依赖，无冗余。

**运行时无需改动**：`EJitModuleLoader` 本来按 funcName 独立存储，每个 entry 有自己的 data 指针。多函数共用同一份 bitcode 是当前行为，改成独立后运行时完全无感。

```cpp
// EJitModuleLoader 内部 (已有逻辑，无需修改)
struct BitcodeEntry {
  std::string funcName;
  const uint8_t *data;   // 各指向不同的 @__ejit_bitcode_xxx
  size_t size;
};
```

### 4.6 每函数闭包提取的复杂度与 closure-only clone

**原始实现（`entry × Composite` 克隆）**：每个 ejit_entry 都
`CloneModule(*Composite)` 复制**整个** composite，再删除闭包以外的 ~99% 内容。
per-entry 代价 ∝ composite 大小，总代价 ∝ `entry 数 × composite 大小`。当每个
entry 只依赖自身的小闭包、而 composite 包含所有 entry 的闭包时，这呈**二次**增长
（benchmark 中 entry 数翻倍 → 墙钟约 4 倍）。

**closure-only clone（本轮）**：先在 composite 上算出该 entry 的传递闭包，再用
`CloneModule(M, VMap, ShouldCloneDefinition)` 的**选择性回调**只克隆闭包内的函数/
全局的**定义**，其余一律克隆为**声明**（无函数体/无 initializer，代价低），最后
`trimToClosure` 的 `GlobalDCE` 清除残留声明。这与 `llvm-extract` 采用的抽取方式
相同，复用 LLVM 结构化 clone API，不手写不完整 clone。

关键语义处理：

- **alias / ifunc 纳入闭包跟踪**：`computeTransitiveClosure` 额外产出
  `ClosureIndirect`（被引用的 `GlobalAlias`/`GlobalIFunc`）。回调对 alias/ifunc
  按闭包成员判定，而非一律返回 `true`。原因：`GlobalDCE` **不会**删除
  externally-visible 的未引用 alias/ifunc，若一律克隆会把死符号泄漏进每个 per-entry
  模块。被闭包引用的 alias 仍以 alias 定义保留（JIT 解析到 aliasee），未引用的则退化
  为声明并被 `GlobalDCE` 删除。
- **ifunc 的特殊处理**：`CloneModule` 对 `GlobalIFunc` **不**咨询 `ShouldCloneDefinition`
  回调，总是把 ifunc 物化为定义并指向其 resolver。若某 entry 不引用该 ifunc，其
  resolver 只会被克隆为声明，导致 "ifunc resolver must be a definition" 的非法模块。
  因此 clone 之后显式擦除闭包外的 ifunc（通过 `VMap` 定位），只保留被引用的合法 ifunc。
- **闭包引用形态**：`call`/`invoke`/`callbr`（均为 `CallBase`，callee 是 operand）、
  `bitcast`/ConstantExpr callee、常量聚合中的函数地址、函数指针表（global initializer）、
  `GlobalAlias`/`GlobalIFunc`、personality、comdat、metadata；mutable global externalize、
  const global 保留、external func/global 注册、`may_const`/`ejit.metadata` 均保留。
  见 `lld/test/ELF/ejit-cross-inline-refs.ll`。

**输出等价性**：closure-only clone 的 per-entry bitcode 与旧的 full-clone+trim 路径
**大小逐字节一致、反汇编 IR 一致**（语义相同），但**原始 bitcode 字节并非 bit-identical**
——full-clone-then-trim 与 closure-only-clone 会留下不同的内部 value ordering，序列化
为不同字节。因此正确的说法是「size + 反汇编 IR 一致」，而非「byte-for-byte identical」。
`lld/utils/ejit-cross-link-bench.py --mode=compare` 会同时报告原始字节 hash（不等）与
归一化 IR hash（相等）。

**尚存限制**：`CloneModule` 仍会为整个 composite 的每个 GlobalValue 创建一个声明
骨架（`PerEntryClone`），且 per-entry `GlobalDCE`（`PerEntryTrim`）会遍历整份克隆。
两者仍是 `O(entry 数 × composite 符号数)`，在 256 entry 的 trace 中合计占
`PerEntryExtraction` 的绝大部分。要做到真正 `O(闭包)`，需要一个**真正的 closure-only
module builder**（用 `IRMover`/`CloneFunctionInto` 直接把闭包搬进新模块），但那会
偏离 `llvm-extract` 惯用法，且需自行正确处理 module flags、named metadata、comdat、
alias/ifunc 等，风险较高，故本轮不实现，列为后续工作。

### 4.7 time-trace 使用方法

```
# 采集各阶段耗时（关闭 granularity 阈值以捕获亚毫秒阶段）
ld.lld --ejit-cross-inline --time-trace=trace.json --time-trace-granularity=0 \
       -shared a.o b.o -o out.so
# 用 Chrome about:tracing 或脚本聚合 "EJitCross:*" 事件
```

不带 `--time-trace` 时不产生任何 trace 文件，也无控制台输出（见
`lld/test/ELF/ejit-cross-inline-time-trace.ll`）。scope 名称是固定短字符串，
per-entry 的函数名放在 TimeTraceScope 的 detail 参数里，不会把不受控超长字符串
放进 scope 名。

### 4.8 production benchmark 方法与 before/after 数据

`lld/utils/ejit-cross-link-bench.py` 生成可复现输入（每个 entry 一条 `noinline`
helper 链 + 私有全局；composite 含所有 entry 的闭包），并分三种**互不污染**的模式：

- `--mode=production`：不开 `--save-temps`、不开 `--time-trace`，测量墙钟与 peak RSS。
- `--mode=trace`：只开 `--time-trace`，给出阶段占比（其墙钟**不**作为生产结果）。
- `--mode=compare`：只开 `--save-temps`，比较 per-entry bitcode 大小与 hash。

复现命令（baseline = `dong/ejit_dev_spec4` 基线 full-clone 的 `ld.lld`）：

```
lld/utils/ejit-cross-link-bench.py --mode=all \
  --baseline-bin /path/to/baseline/bin \
  --optimized-bin build_release_x86/bin \
  --entries 1,8,32,64,128,256
```

**production 墙钟 / peak RSS（helpers=20，globals=6，best-of-3，x86 Release+asserts）**：

| entries | baseline wall | optimized wall | speedup | baseline RSS | optimized RSS |
|--------:|--------------:|---------------:|--------:|-------------:|--------------:|
| 1       | 0.017 s       | 0.017 s        | ~1.0×   | 54.3 MB      | 54.5 MB       |
| 8       | 0.030 s       | 0.026 s        | 1.17×   | 55.3 MB      | 55.6 MB       |
| 32      | 0.151 s       | 0.071 s        | 2.12×   | 58.9 MB      | 59.9 MB       |
| 64      | 0.509 s       | 0.176 s        | 2.89×   | 65.4 MB      | 65.7 MB       |
| 128     | 1.967 s       | 0.536 s        | 3.67×   | 76.6 MB      | 77.4 MB       |
| 256     | 8.323 s       | 1.961 s        | 4.24×   | 99.8 MB      | 100.4 MB      |

peak RSS 基本持平（优化不以内存换时间）。

**compare（输出保持）**：所有规模下 per-entry bitcode **大小逐条一致**，原始字节
hash 不等（内部 value ordering 差异），归一化 IR hash 相等 → 语义/文本一致。

**trace（256 entry 阶段占比）**：`PerEntryClone` ≈ 1105 ms、`PerEntryTrim` ≈ 565 ms
合计占 `PerEntryExtraction`（≈ 1813 ms）的绝大部分，与 4.6 的尚存限制一致。

---


## 5. 构建依赖

### 5.1 LLVMEmbeddedJIT (CMakeLists.txt)

```cmake
add_llvm_component_library(LLVMEmbeddedJIT
  EJitPassOptions.cpp
  EJitRegisterBitcode.cpp
  ...

  LINK_COMPONENTS
  Core
  IRReader
  Support
  TransformUtils
  BitWriter
  BitReader      # 新增: parseBitcodeFile (链接期)
  Object         # 新增: 读取 ELF section
  Linker         # 新增: linkInModule
  )
```

### 5.2 clangDriver

clang driver 只负责校验选中的 linker 并转发 `--ejit-cross-inline`；不再链接
BitReader/Object/Linker 等 cross-link 实现依赖。

---

## 6. 与其他模块的交互

| 模块 | 影响 |
|---|---|
| **PASS3 (WrapperGen)** | 不受影响 — wrapper 仍正常生成 |
| **PASS2 (RegisterPeriod)** | 不受影响 |
| **运行时 (libLLVMEJIT)** | 不受影响 — `ejit_init` 仍通过 `ejit_register_bitcode` 加载 `@__ejit_bitcode` |
| **JIT 编译** | 受益 — bitcode 中跨 TU 子函数已内联，PASS6 可以追到 may_const load |
| **默认编译路径** | 不受影响 — 不传 `-fejit-cross-inline` 时行为完全不变 |

---

## 7. 错误处理 & 边界情况

### 7.1 错误处理（PR #102: 检测到 .ejit_cross 后绝不静默吞错）

区分「输入没有 `.ejit_cross`」（正常跳过）与「已检测到 `.ejit_cross` 但处理失败」
（硬失败）。`runEJitCrossLink` 返回 `Expected<EJitCrossLinkResult>`，每个错误都带
**输入文件名 + 处理阶段 + 底层 llvm::Error** 文本，由 `ld.lld` 转成致命链接错误。

| 场景 | 策略 |
|---|---|
| 无 .ejit_cross section 的链接 | 返回空结果，正常链接（默认行为不变） |
| 普通非对象参数或不含该 section 的输入 | 跳过，非错误 |
| **被选中的 archive 成员 / `-l` / linker-script 输入含 .ejit_cross** | 正常处理（在符号解析后遍历 `ctx.objectFiles`）；未被选中的成员不处理 |
| **bitcode 解析失败 / section 读取失败** | **链接失败**（带文件名+阶段+Error） |
| **`Linker::linkInModule` 合并冲突** | **链接失败**（检查返回值） |
| 合并后无 ejit_entry | **链接失败**（已承诺有 .ejit_cross，却无可注册入口） |
| **per-entry / registry `verifyModule` 失败** | **链接失败**，不写出任何 bitcode |
| **临时 .bc 创建 / 写入失败** | **链接失败**（删除半成品临时文件） |

> 旧实现在上述多数场景 `continue` 或返回空串静默 fallback 到 AOT；现已全部改为
> 通过 `Error` 传播到链接器用户。clang 与 lld 两条入口的错误语义一致（clang 仅在
> 非 lld 链接器时报错，其余交给 lld）。

### 7.2 与 `-flto` 冲突

`-flto` / `-flto=thin` 和 `-fejit-cross-inline` 是两套独立的跨模块机制，同时使用会导致：
- 两套机制都做内联，结果不确定
- LTO 的 bitcode section (`.llvmbc`) 和 `.ejit_cross` 共存，语义混乱

**策略**：clang driver 检测到同时出现时直接报错。

```cpp
if (Args.hasArg(options::OPT_fejit_cross_inline) &&
    Args.hasArg(options::OPT_flto_EQ)) {
  C.getDriver().Diag(clang::diag::err_drv_argument_not_allowed_with)
      << "-fejit-cross-inline" << "-flto";
}
```

### 7.3 `.ejit_cross` section 自动清除

Cross-link 返回精确的 `consumedFiles`，Driver 写入
`ctx.ejitCrossConsumedFiles`。`InputFiles.cpp` 只有在当前 input identifier 命中该
集合且 section 名为 `.ejit_cross` 时才设为 `InputSection::discarded`：
- 已处理的原始 `.o` section 不进入输出，`-r` 和最终链接都生效；
- archive、`-l` 或 linker script 引入的 member 仍由 lld 原生符号解析选择；
- provisional registry 若引用新的 lazy archive symbol，Driver 会提取对应 member，
  重新扩展 composite，直到 `ctx.objectFiles` 不再增长；只有 fixed point 的最终
  registry 会进入 LTO，因此不会遗留未处理的 `.ejit_cross` 或重复 registry。

### 7.4 被调用的 TU 也需要 `-fejit-cross-inline`

只有 `ejit_entry` 所在 TU 用该 flag 是不够的——`child_func()` 定义的 TU 也必须用，否则链接期看不到它的 IR。

```
clang -fejit-cross-inline -c tu_a.c -o tu_a.o   # ejit_entry 在此
clang -fejit-cross-inline -c tu_b.c -o tu_b.o   # child_func 在此, 必须
```

---

## 8. 与 ThinLTO/FullLTO 方案对比

| 维度 | ThinLTO/FullLTO | `-fejit-cross-inline` |
|---|---|---|
| AOT 代码影响 | 全局跨模块优化 | 无影响 |
| 编译时间 | 增加 (summary + 多阶段) | 仅编译期多一次序列化 |
| 链接时间 | 增加 (LTO backend) | 增加 (合并 + 内联) |
| JIT bitcode 质量 | 跨模块内联 ✓ | 跨模块内联 ✓ |
| `!ejit.may_const` | 需要 noinline 保护 | 天然保留 (完整 Module) |
| 构建系统改动 | clang flag | 同 |
| `-r` 兼容性 | 复杂 | 简单 (标准 ELF section) |

---

## 9. 文件改动清单

PR #102 hardening 后（ld.lld 为唯一 owner）：

| 文件 | 改动 |
|---|---|
| `clang/include/clang/Driver/Options.td` | `-fejit-cross-inline` |
| `clang/lib/Driver/ToolChains/Clang.cpp` | 编译选项传递 + `-flto` 冲突检测（不变） |
| `clang/lib/Driver/ToolChains/Gnu.cpp` | link 阶段：检测非 lld → 报错；lld → 追加 `--ejit-cross-inline`（不再自行合并） |
| ~~`clang/lib/Driver/EJitCrossLink.cpp` / `.h`~~ | **已删除**（去重：实现只剩 lld 一份） |
| `clang/lib/Driver/CMakeLists.txt` | 移除 `EJitCrossLink.cpp` 及其专用 LINK_COMPONENTS |
| `lld/ELF/EJitCrossLink.cpp` | 链接期处理核心（唯一实现）：`Expected<EJitCrossLinkResult>`、保守闭包、真实 inliner、TmpM 内 registry + `verifyModule`、错误传播、临时文件清理。**本轮**：改为遍历选中的 `MemoryBufferRef`；closure-only 选择性 `CloneModule` + alias/ifunc 闭包跟踪 + 擦除闭包外 ifunc；返回 registry 强引用的 external symbol；每阶段 `TimeTraceScope` |
| `lld/ELF/EJitCrossLink.h` | API 声明改为 `runEJitCrossLink(ArrayRef<MemoryBufferRef> SelectedObjects, ...)`，结果附带 `requiredSymbols` |
| `lld/ELF/Options.td` | `--ejit-cross-inline`（gate，默认关闭时零扫描） |
| `lld/ELF/Config.h` | `Config::ejitCrossInline`（arg）+ `Ctx::ejitCrossConsumedFiles`（精确消费集合） |
| `lld/ELF/Driver.cpp` | 解析 flag；**本轮**：调用点移到符号解析之后、遍历 `ctx.objectFiles`；根据 provisional registry 的 `requiredSymbols` 迭代提取 lazy archive member 至 fixed point，最后一份 registry 经 `addFile`+`parseFile` 进入 LTO |
| `lld/ELF/InputFiles.cpp` | 仅丢弃精确匹配已消费输入的 `.ejit_cross`（不变） |
| `lld/test/ELF/ejit-cross-inline.ll` | 生产路径测试；**本轮**更新 archive/`-l` 用例为需显式选中 member |
| `lld/test/ELF/ejit-cross-inline-time-trace.ll` | **新建**：`--time-trace` 下所有阶段 scope 存在；不传时不产生 trace 文件 |
| `lld/test/ELF/ejit-cross-inline-archive-select.ll` | **新建**：archive member selection（HasCross 顺序 bug；只处理选中 member；冲突符号；顺序无关；registry 引发 late extraction 的 fixed-point 回归） |
| `lld/test/ELF/ejit-cross-inline-refs.ll` | **新建**：引用形态覆盖（call/invoke/personality/fptr 表/常量聚合/bitcast/alias(live+dead)/ifunc/shared+dead helper/mutable+const global/external 注册） |
| `lld/utils/ejit-cross-link-bench.py` | **新建**：production/trace/compare 三模式 benchmark，支持 baseline/optimized before/after |
| `jit_design_doc/EJIT_CROSS_TU_INLINE.md` | 本文档：复杂度、closure-only 算法、archive 选择语义、time-trace、benchmark 与 before/after 数据、尚存限制 |

### 9.1 闭包支持的引用形态（Area 4）

`computeTransitiveClosure` / `collectFromConstant` 统一、保守地收集引用：扫描所有
指令 operand，穿透 `stripPointerCasts` / `ConstantExpr` / `ConstantAggregate`，解析
`GlobalAlias` 与 `GlobalIFunc` 目标，扫描已保留全局的 initializer（函数指针表）。
`CallInst` / `InvokeInst` / `CallBrInst`（均为 `CallBase`，callee 是 operand）自然覆盖，
bitcast 后的 callee 也覆盖。运行时传入的真正间接目标无法静态推断，间接调用保持不变；
module-owned 函数表目标由 initializer 扫描覆盖。trim 先 `deleteBody`，再由 `GlobalDCE`
安全回收，Debug/Release 都不会断言或留下 dangling reference。

**本轮补充**：`computeTransitiveClosure` 额外产出 `ClosureIndirect`（被引用的
alias/ifunc），供 closure-only clone 判定是否保留 alias/ifunc 定义（见 4.6）。

---

## 10. 未来扩展

- ~~支持从 archive (.a) 中提取 `.ejit_cross`~~ **已实现**（符号解析后遍历选中 member）
- 真正的 closure-only module builder（`IRMover`/`CloneFunctionInto`），把 per-entry
  `PerEntryClone`/`PerEntryTrim` 从 `O(entry×composite符号)` 降到 `O(闭包)`（见 4.6 风险）
- 增量链接：缓存已处理的跨 TU bitcode 避免重复合并
- `-fejit-cross-inline=thin` 变体：利用 ThinLTO summary 实现按需导入

---

*文档版本: 2.0（cross-TU per-entry extraction 优化 + archive member selection）*
*创建日期: 2026-07-29*
