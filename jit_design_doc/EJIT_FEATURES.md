# EJIT 功能说明

> 支持功能发布说明书 · 面向集成方。
>
> 说明本版本 EJIT 提供的功能、每个功能如何使用、支持的平台、集成接口与使用约束。本文不涉及如何构建 EJIT 运行库本身；某些功能需要你的 EJIT 运行库已启用对应支持（如固定代码段、inline cache、逐调用统计），若不确定请联系 EJIT 提供方。

---

## 1. 概述

EJIT（EmbeddedJIT）是面向嵌入式场景的 JIT 编译系统，核心思想是**时间窗常量特化**：

- 在一段声明的**时间窗**（period）内，部分运行时数据被视作编译期常量。
- JIT 据此为函数产生**特化版本**：相关分支被折叠、内存读被替换为字面常量、死代码被消除。
- 时间窗关闭后，执行回退到 AOT 版本。

所有 JIT 失败均回退 AOT，不会导致崩溃。

### 集成步骤总览

1. **标注属性**：在 C/C++ 源码中给函数 / 全局 / 字段 / 参数加 ejit 属性（§4）。
2. **编译**：用 `clang -fembed-bitcode` 编译；按需加 `-mllvm` 选项开启各功能（§6、§9）。
3. **链接**：按用到的功能在链接脚本里放置对应段 / 符号（§7）。
4. **运行**：`ejit_init()` 初始化、`ejit_activate()` 激活时间窗，调用 `ejit_entry` 函数即获特化（§11）。

---

## 2. 支持的目标平台与环境

### 2.1 JIT 代码生成架构

| 架构 | 支持情况 |
|------|----------|
| AArch64（小端） | 支持，JIT 代码生成目标 |
| AArch64 大端（`aarch64_be`） | 支持，生产上版目标 |
| x86 | 作为宿主开发 / 测试目标（本机运行） |

公共 ABI 结构体使用定宽标量字段，在 `aarch64_be` 目标与宿主读取器之间保持稳定。

### 2.2 运行环境

同一套运行库既可在 Linux 宿主上编译运行（真实 pthread、`steady_clock` 计时、`mprotect` 风格封固），也可在裸金属 / SRE 目标上运行（freestanding，无 pthread、无 C++ 线程 / 标准库）。集成方通常在宿主上开发调试，部署 freestanding 构建到单板。

### 2.3 裸金属 / SRE 平台需提供的接口

裸金属环境下 EJIT 以 freestanding 方式运行，以下 `extern "C"` 符号由**集成方平台实现**（EJIT 只声明不定义，缺失则链接报错；宿主构建有 weak 空实现兜底）：

| 平台符号 | 作用 | 被哪些功能使用 |
|----------|------|----------------|
| `SRE_printf` | 所有诊断日志输出 | 日志 / 内省 |
| `SRE_CycleCountGet64` | 周期计数时间戳 | wrapper 计时（§6.7） |
| `SRE_MemDbgAlloc` | 代码池原始内存分配 | 代码内存池（§6.3） |
| `enable_ex` | 页封固为 RX（W^X 执行） | 代码内存池（§6.3） |
| `enable_rw` | 页置为 RW（须清除执行权限） | 固定代码段（§6.3） |
| `split_2m_to_4k` | 2MiB 池拆分为 4KiB 页 | 代码内存池 4K 封固（§6.3） |
| `SRE_TaskCreate` / `SRE_TaskDelete` / `SRE_TaskDelay` | 异步编译 worker 任务的创建 / 删除 / 延时 | 异步编译（§6.2） |

跨核共享 taskpool 的共享状态置于核间共享内存（见 §7.5），并绑定到平台提供的“当前核”接口。

---

## 3. 标注属性

集成方在源码中用以下 6 个属性声明特化意图。头文件 `EJitRuntime.h` 提供推荐的**宏形式**（等价于底层 `__attribute__` 拼写）；在包含该头文件前定义 `EJIT_DISABLE` 可把所有标注编译为空，便于 A/B 对比或移植到非 clang 编译器。

| 宏形式（推荐） | 标注对象 | 含义 |
|----------------|----------|------|
| `ejit_entry` | 函数 | 该函数是 JIT 特化候选；编译器在其入口插入分发 wrapper 并嵌入其位码 |
| `ejit_period(name)` | 非数组全局变量 | 将该全局归入名为 `name` 的时间窗；`"static"` 表示整个 JIT 运行期恒定（始终激活，无需 `ejit_activate`） |
| `ejit_period_arr(name)` | 数组 / 指针全局变量 | 声明一个多实例时间窗数组；同名数组共享激活索引 |
| `ejit_period_arr_ind(name)` | 函数参数 | 标记该参数为选择 `name` 窗口哪个实例的运行时下标（特化维度） |
| `ejit_may_const` | 结构体字段 | 该字段在所属全局处于激活窗口时可被视作编译期常量 |
| `ejit_period_lc(name)` | 函数 | 该函数会修改窗口内数据；编译器在其入口插入 `ejit_deactivate`、出口插入 `ejit_activate`，保证写入期间不使用陈旧特化 |

> 因 `static` 是 C 关键字，时间窗名以字符串传入：`ejit_period(static)`。

### 3.1 `ejit_may_const`（字段）

```c
struct CellConfig {
    ejit_may_const uint32_t cellType;   /* 激活窗口内被当作常量折入特化 */
    uint32_t trafficLoad;               /* 普通字段，不特化 */
};
```

支持整型、布尔、浮点、嵌套结构体字段及上述类型的数组。`volatile` 字段永不视为常量。对 `may_const` 字段取无 `const` 限定符的地址会告警。

### 3.2 `ejit_period(name)`（标量全局）

```c
ejit_period(static) struct BoardConfig g_boardCfg;   /* "static" 窗口：JIT 运行期不变 */
```

仅用于非数组全局；数组须用 `ejit_period_arr`。一个变量不能同时带 `ejit_period` 与 `ejit_period_arr`。本版本标量窗仅支持 `"static"`。

### 3.3 `ejit_period_arr(name)`（数组 / 指针全局）

```c
ejit_period_arr(cell) struct CellConfig g_cellCfg[16];   /* 16 个独立实例 */
ejit_period_arr(cell) struct CellConfig *g_cellPtr;       /* 指针形式：集成方自行赋值与越界检查 */
```

数组长度须 ≤ 100；全程序最多 1024 个 `ejit_period_arr` 数组。多个数组可共用同一 `name`，`ejit_activate(name, idx)` 一次性激活该 `name` 下所有数组的第 `idx` 个实例。指针形式无静态长度限制，由集成方保证越界安全。

### 3.4 `ejit_period_arr_ind(name)`（参数维度）

```c
ejit_entry
void process_cell(ejit_period_arr_ind(cell) uint8_t cellIdx) { ... }
```

参数须为整型；每个函数最多 4 个此类参数（故编译 API 提供 `_0d.._4d` 变体）；每个维度最多关联 1024 个数组。

### 3.5 `ejit_entry`（JIT 函数）

```c
ejit_entry void process_board(void) { ... }
```

约束：不可递归；隐式依赖 `"static"` 窗口；对数组窗的依赖须经 `ejit_period_arr_ind` 声明；**不可标注 `always_inline`**（该函数须在内联器中存活以供 JIT 特化，冲突时 Sema 告警并忽略 `always_inline`）。异步模式下首次调用走 AOT 回退，故副作用须幂等；非幂等函数请用同步模式。

### 3.6 `ejit_period_lc(name)`（生命周期守卫）

```c
ejit_period_lc(cell)
void update_cell_config(ejit_period_arr_ind(cell) uint8_t cellIdx) {
    g_cellCfg[cellIdx].cellType = 0xFD;   /* 入口 deactivate(cell,idx)，出口 activate(cell,idx) */
}
```

可叠加（如 `ejit_period_lc(cell) ejit_period_lc(trp)`）；须配对同名的 `ejit_period_arr_ind(name)` 参数；同样不可 `always_inline`。

---

## 4. 特化模型

1. `ejit_entry` 标记待特化函数；编译器嵌入其位码并在入口插分发 stub。
2. **时间窗**（period）是命名、可激活的作用域，覆盖一组全局。`"static"` 窗口始终激活；自定义窗（`ejit_period_arr`）管理“稳定一段时间后变更”的数据。
3. `ejit_may_const` 标记的字段在窗口激活时，其当前值被 JIT 在编译期读出并折入特化。
4. `ejit_period_arr` / `ejit_period_arr_ind` 构成多实例维度：`ejit_period_arr(cell) struct CellConfig g[16]` 声明 16 个实例；`ejit_period_arr_ind(cell) uint8_t idx` 表示“本次调用针对实例 `idx` 特化”。
5. `ejit_activate("cell", 5)` 激活 `cell` 窗口的实例 5；此后对 `idx==5` 的 `ejit_entry` 调用解析到一个把 `g[5]` 中 `may_const` 字段折为字面常量的特化版本。`ejit_deactivate("cell", 5)` 失效该特化的缓存条目，下次调用重编译。
6. `ejit_period_lc` 函数在写入前后 bracket deactivate/activate，确保写入中途不使用陈旧特化。

**举例**：基站 `process_cell(cellIdx)` 读 `g_cellCfg[cellIdx].cellType` 并分支。无窗口时跑通用 AOT 代码。`ejit_activate("cell", 3)` 后，首次 `process_cell(3)` 触发 JIT：读出 `g_cellCfg[3].cellType`（假设 `0xFD`），把 `if (cellType == 0xFD)` 折成恒真，缓存特化函数指针。后续 `process_cell(3)` 直接跳到缓存特化。调用 `process_cell(7)`（不同实例）解析到不同特化--多版本 inline cache 按 `(维度, 实例)` 身份各存一槽。

---

## 5. 功能与使用

每个功能给出：**选项**（编译你的 ejit 代码时传给 clang 的 `-mllvm` 标志）、**链接脚本**（你的链接脚本需放置的段 / 符号）、**属性 / API**、**平台回调**。所有 `-mllvm` 标志汇总见 §9；链接脚本细节见 §7。

### 5.1 时间窗特化（核心）

- **选项**：无需任何 `-mllvm`；属性 + `clang -fembed-bitcode -O2` 即触发。
- **链接脚本**：裸金属需放注册表段（§7.1）；宿主可选。
- **属性 / API**：`ejit_entry` 等属性（§3）+ `ejit_init()` / `ejit_activate()` / `ejit_deactivate()`。
- **平台回调**：无。

### 5.2 异步编译 / taskpool 调度

异步模式下 cache 未命中时把编译请求入队到单一后台 worker，立即返回 `EJIT_PENDING`，调用方本次走 AOT 回退，下次命中新鲜特化；对相同特化的并发请求去重。同步模式在未命中时于调用线程内联编译。

- **选项**：无 AOT 期标志（已退役的 `-ejit-wrapper-async` 已移除）。模式运行时可切：`ejit_set_compile_mode(EJIT_COMPILE_ASYNC)` / `EJIT_COMPILE_SYNC`，或 init 时设 `cfg.compileMode`。
- **链接脚本**：跨核共享 taskpool 需放共享段（§7.5）。
- **属性 / API**：`ejit_taskpool_compile_or_get()` + `ejit_taskpool_release_read()`（wrapper 自动调用）；`ejit_taskpool_get_stats()` 观察队列 / 去重 / 失败。
- **平台回调**：`SRE_TaskCreate` / `SRE_TaskDelete` / `SRE_TaskDelay`（创建 / 删除 / 延时 worker）。

### 5.3 代码内存池（W^X、4K 封固、固定代码段）

JIT 代码从受管内存池分配，写时 RW、封固时 RX（严格 W^X）；4K 封固模式下每页代码就绪即单独封固。**固定代码段模式**把代码池放在链接脚本固定的 `.text.ejit` 区域，给 JIT 稳定地址（±128MiB 内可直 `bl`/`adrp` 到 AOT）。

- **选项**：无 AOT 期标志。是否启用代码池 / 4K 封固 / 固定段取决于你链接的运行库（向提供方确认）。
- **链接脚本**：固定代码段模式需放 `.text.ejit` + `__ejit_code_start`/`__ejit_code_end`（§7.2）。
- **属性 / API**：`ejit_get_code_pool_stats()` / `ejit_print_code_pool_stats()` 监控占用与耗尽；代码内存预算由 `cfg.maxCodeMemory` 配置。
- **平台回调**：`SRE_MemDbgAlloc`（原始分配）、`enable_ex`（封固 RX）、`split_2m_to_4k`（4K 拆页）；固定代码段模式另需 `enable_rw`（写前置 RW，**缺失即链接报错**）。

### 5.4 inline cache（多版本、per-core）

特化编译后函数指针发布到 per-function、direct-indexed inline-cache 槽（`@__ejit_icache_fn_<name>`，按维度 shaping）。开启后命中路径是一次原子 load + 判空 + 尾调用，无 `ejit_*` 调用。

- **选项**：`-mllvm -ejit-inline-cache`（默认 off）。
- **链接脚本**：无（槽符号是内部全局，自动注册）。**要求运行库已启用跨核共享代码指针支持**（向提供方确认）。
- **属性 / API**：自动注册，无需手动。每维槽数 `D`（默认 16，2 的幂）须 AOT 与运行库一致；维度数 > 4 为编译错误。
- **平台回调**：无。

### 5.5 分派器聚簇

把 frame-less icache 分派器（每个约 16-48 字节）聚到专用 `.text.ejit_dispatch` 段，提升 I-cache 空间局部性、降 iTLB 压力。

- **选项**：`-mllvm -ejit-dispatcher-cluster`（默认 off）。仅在 `-ejit-inline-cache` 开启时有意义。
- **链接脚本**：放 `.text.ejit_dispatch` 段（§7.3）。
- **属性 / API / 平台回调**：无。

### 5.6 Miss 函数冷隔离

把 icache 未命中时执行的 AOT 回退体（MissFn，可能数 KB 冷代码）标记为 `cold`，使后端放入 `.text.unlikely`，避免冷代码污染热 I-cache。

- **选项**：`-mllvm -ejit-missfn-cold`（默认 off）。仅在 `-ejit-inline-cache` 开启时有意义（MissFn 只在 wrapper 拆分时存在）。
- **链接脚本**：确保链接脚本放置了 `.text.unlikely` 段（§7.4）。
- **属性 / API / 平台回调**：无。

### 5.7 wrapper 计时

在 `ejit_entry` wrapper 中插计时探针，测 taskpool 查找 / 间接 JIT 调用 / 读令牌释放各段耗时。运行库按固定间隔（默认每 100000 次）聚合打印一行汇总。

- **选项**：`-mllvm -ejit-wrapper-timing`（默认 off）。
- **链接脚本**：无。
- **属性 / API**：探针由 wrapper 自动调用 `ejit_taskpool_trace_now()` / `ejit_taskpool_trace_wrapper()`，一般无需手动调用。
- **平台回调**：`SRE_CycleCountGet64`（时间戳；宿主用 `steady_clock` 纳秒）。

### 5.8 定维快路径

对维度数 ≤ 2 的 entry 生成定维快路径调用（`ejit_taskpool_compile_or_get_Nd`，参数走寄存器、无栈溢出）；>2 维走通用入口。

- **选项**：`-mllvm -ejit-wrapper-fixed-dim-entry`（默认 **on**）。
- **链接脚本 / 属性 / API / 平台回调**：无（运行库内部快路径函数，集成方无操作）。

### 5.9 L0 分派 cache

per-core L0 分派 cache 在热点路径上前置于 taskpool 查找；在 `ejit_invalidate` / deactivate / cache 退役时失效。

- **选项 / 链接脚本**：无（随共享 taskpool 运行库提供）。
- **属性 / API**：`ejit_invalidate(periodName, idx)` 主动退役陈旧特化。
- **平台回调**：无。

### 5.10 IR/ASM 转储与诊断

详见《EJIT 诊断与调试指南》。编译期诊断标志（`-ejit-warn-*` / `-ejit-report-mayconst` / `-ejit-dump-bitcode-dir`，见 §9）；运行时转储 / 内省 / 日志 / 统计 API。

---

## 6. 链接脚本集成

EJIT 在你的二进制中引入若干段 / 符号，需在你的链接脚本中放置。模板见 `llvm/lib/ExecutionEngine/EJIT/ejit_registry.ld`（含 inline 与 INSERT 两种形式）。**裸金属构建下这些符号为强符号，缺失即链接报错**；宿主构建下为 weak，缺失则对应范围为空。

### 6.1 注册表段：`.ejit_bitcode` / `.ejit_period` + 边界符号

PASS1/PASS2/PASS3 把每个 TU 的注册条目（`ejit_reg_entry_t`，40 字节、8 字节对齐）以私有数组形式发到这两段；运行时遍历 `[__start_ejit_bitcode, __stop_ejit_bitcode)` 与 `[__start_ejit_period, __stop_ejit_period)`。段名带前导点（非合法 C 标识符），链接器不会自动合成边界符号，**必须由链接脚本定义**。

**inline 形式**（贴进你现有的 `.rodata` 输出段）：

```ld
.rodata : ALIGN(8)
{
  *(.rodata .rodata.*)
  . = ALIGN(8);
  __start_ejit_bitcode = .;
  KEEP(*(.ejit_bitcode))
  __stop_ejit_bitcode = .;
  . = ALIGN(8);
  __start_ejit_period = .;
  KEEP(*(.ejit_period))
  __stop_ejit_period = .;
  . = ALIGN(8);
} > FLASH
```

**INSERT 形式**（不能改 vendor 的 `SECTIONS` 时，单独成文件追加）：

```ld
SECTIONS
{
  .ejit_registry : ALIGN(8)
  {
    . = ALIGN(8);
    __start_ejit_bitcode = .;
    KEEP(*(.ejit_bitcode))
    __stop_ejit_bitcode = .;
    . = ALIGN(8);
    __start_ejit_period = .;
    KEEP(*(.ejit_period))
    __stop_ejit_period = .;
    . = ALIGN(8);
  }
} INSERT AFTER .rodata;
```

要点：
- `KEEP()` 防止 `--gc-sections` 丢弃表（它们只被链接符号引用，看似未用）。
- 静态（非 PIE）裸金属镜像下指针字段在链接期解析为绝对地址，表为只读，放 `.rodata`/flash 无 RAM 开销；PIE 下编译器发为可写（带重定位），须放 `.data`（用 `INSERT AFTER .data`）。
- 用自动注册（§8）时此块可选；用手工 / 静态注册时**必需**。

### 6.2 固定代码段：`.text.ejit` + `__ejit_code_start` / `__ejit_code_end`

固定代码段模式下，运行时读这两个符号、把起点上对齐到 2MiB、从 `[对齐起点, __ejit_code_end)` 切 2MiB 对齐的池（替代 `SRE_MemDbgAlloc`）。

```ld
.text.ejit : ALIGN(4096)
{
  __ejit_code_start = .;
  . = __ejit_code_start + 16M;
  __ejit_code_end = .;
} > CODE       /* 代码段，近 .text（±128MiB 内 bl/adrp 直达 AOT） */
```

要点：
- 段只需 4KiB 对齐，2MiB 对齐由运行时负责（`enable_ex`/`split_2m_to_4k` 要求 2MiB 对齐基址）。`[__ejit_code_start, 对齐起点)` 是浪费（≤~2MiB），故按 16M 配置约得 ~14M 可用、~7 个池。
- 放 CODE 区、在 `.text` 的 ±128MiB 内，JIT 代码才能直 `bl`/`adrp` 到 AOT。区域加载时 RX；运行时写前 `enable_rw`（RX->RW）、finalize `enable_ex`（RW->RX）。
- 区域大小由链接脚本定（`+ 16M`），**无运行库配置项**。用 `readelf -SW/-lW` 校验，确认 `__ejit_code_end > __ejit_code_start`。
- 仅在最终可执行链接处用 INSERT（`clang -r` 中间产物不可靠）。

### 6.3 分派器聚簇段：`.text.ejit_dispatch`

`-ejit-dispatcher-cluster` 时，分派器被发到此段：

```ld
.text.ejit_dispatch : ALIGN(64)
{
  KEEP(*(.text.ejit_dispatch))
} > CODE
```

`ALIGN(64)` 匹配典型 L1 cache 行，避免 false sharing；`KEEP()` 防 `--gc-sections` 丢弃。放 CODE 区、近 `.text`。

### 6.4 Miss 函数冷段：`.text.unlikely`

`-ejit-missfn-cold` 时，MissFn 带 `cold` 属性，后端放入 `.text.unlikely`。你的链接脚本须放置该段，冷代码才会被隔离：

```ld
.text.unlikely : { *(.text.unlikely .text.unlikely.*) } > CODE
```

未提及该段的链接脚本可能把冷代码放任意位置或报错。未开 `-ejit-missfn-cold` 时 MissFn 走函数正常段，无隔离。

### 6.5 共享 taskpool 段

跨核共享 taskpool 的共享状态置于一个可配置段（生产预设常用 `.mc_shared`，段名由运行库构建决定，须匹配）。须放**核间共享内存**：

```ld
.mc_shared : ALIGN(64) { KEEP(*(.mc_shared)) } > SHARED_MEM
```

`KEEP()` 因共享状态只被运行时引用。段名须与你链接的运行库一致（向提供方确认）。

---

## 7. 注册：自动 vs 手工

**自动注册（默认）**：编译时（`-mllvm -enable-ejit-global-ctors=true`，默认）把 `ejit_auto_register` 加入 `llvm.global_ctors`（优先级 65535），启动时自动注册全部条目。此路径**不需要**链接脚本边界符号**（§6.1 可选）。**

**手工 / 静态注册（裸金属无构造函数时）**：

1. 编译时 `-mllvm -enable-ejit-global-ctors=false`，只发 `.ejit_bitcode`/`.ejit_period` 段表。
2. 链接脚本加注册表边界符号（§6.1）--**必需**。
3. init 前 `cfg.forceStaticRegistry = true`（或依赖构造数据为空的回退）。
4. 需 JIT 解析的外部符号（裸金属无 `dlsym`）：调用 `ejit_register_symbol(name, addr)` 注册地址。

运行时遍历段表处理条目类型：bitcode、period 数组、静态变量、符号、lifecycle、funcindex、icache 槽。

---

## 8. 编译期选项总览

以下 `-mllvm` 标志在编译你的 ejit 代码（`clang -fembed-bitcode`）时传入。均为 `cl::Hidden`（不在 `--help`），需显式指定。无 clang `-f` 前端标志。

| 标志 | 默认 | 作用 |
|------|------|------|
| `-mllvm -enable-ejit-bitcode` | true | PASS1 提取并嵌入 `ejit_entry` 位码；关则无 JIT |
| `-mllvm -enable-ejit-aot` | true | 后期 AOT pass（wrapper / period 注册等）；关则 `ejit_entry` 不被包装 |
| `-mllvm -enable-ejit-global-ctors` | true | 生成 `llvm.global_ctors` 自动注册；裸金属无构造函数时关（见 §7） |
| `-mllvm -ejit-wrapper-fixed-dim-entry` | true | 维度 ≤2 的 entry 生成定维快路径调用（§5.8） |
| `-mllvm -ejit-inline-cache` | off | 发多版本 inline cache 探针（§5.4） |
| `-mllvm -ejit-dispatcher-cluster` | off | 分派器聚到 `.text.ejit_dispatch`（§5.5，须配合 inline-cache） |
| `-mllvm -ejit-missfn-cold` | off | MissFn 标 cold 放 `.text.unlikely`（§5.6，须配合 inline-cache） |
| `-mllvm -ejit-wrapper-timing` | off | wrapper 计时探针（§5.7） |
| `-mllvm -ejit-dump-bitcode-dir=<dir>` | off | AOT 期把提取位码（`.bc`+`.ll`）落盘 |
| `-mllvm -ejit-warn-no-specialization` | on | 闭包无 `may_const` 告警（诊断） |
| `-mllvm -ejit-warn-unused-dim` | on | 死维度告警（诊断） |
| `-mllvm -ejit-warn-few-mayconst=<N>` | 0(off) | 闭包 `may_const` 读数 < N 告警（opt-in 阈值；如 `=4` 告警 0..3 读） |
| `-mllvm -ejit-report-mayconst` | off | 每 entry `may_const` 读数（总数 / 循环内）info 报告（诊断） |

> 诊断类标志的输出与使用见《EJIT 诊断与调试指南》。Sema 告警组 `-Wembedded-jit` 用 `-Wno-embedded-jit` 关闭。

---

## 9. 公共 C API

头文件：`llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h`（`extern "C"`）。

### 9.1 类型与枚举

```c
typedef enum { EJIT_OK=0, EJIT_ERR_INVALID_PARAM=-1, EJIT_ERR_NOT_ACTIVE=-2,
  EJIT_ERR_COMPILE_FAILED=-3, EJIT_ERR_CACHE_FULL=-4, EJIT_ERR_MEMORY=-5,
  EJIT_ERR_BITCODE_NOT_FOUND=-6, EJIT_ERR_QUEUE_FULL=-7, EJIT_ERR_DEDUP_FULL=-8,
  EJIT_ERR_DISABLED=-9, EJIT_ERR_INSTANCE_DISABLED=-10, EJIT_PENDING=1 } ejit_status_t;
typedef enum { EJIT_COMPILE_SYNC=0, EJIT_COMPILE_ASYNC=1 } ejit_compile_mode_t;
typedef enum { EJIT_OPT_L1=1, EJIT_OPT_L2=2, EJIT_OPT_L3=3 } ejit_opt_level_t;
typedef struct { uint32_t dimType; uint32_t instanceId; } ejit_dim_pair_t;
typedef struct { int code; char message[256]; char funcName[128]; } ejit_error_t;
```

### 9.2 生命周期

```c
ejit_status_t ejit_init(const ejit_config_t *config);
void          ejit_shutdown(void);
ejit_status_t ejit_activate(const char *periodName, uint8_t cellIdx);
ejit_status_t ejit_deactivate(const char *periodName, uint8_t cellIdx);
ejit_status_t ejit_activate_all(const char *periodName);
ejit_status_t ejit_deactivate_all(const char *periodName);
bool          ejit_is_active(const char *periodName, uint8_t cellIdx);
```

激活以 name + index 为键（无数组指针维度）；对较小的数组，越界 `cellIdx` 在该数组上静默跳过。

### 9.3 编译（统一入口 + 定维快路径）

```c
ejit_status_t ejit_taskpool_compile_or_get(uint32_t funcIndex,
                                           const ejit_dim_pair_t *dims, uint32_t numDims,
                                           void **outFn, uint32_t *outBucket);
// 定维变体：_0d / _1d / _2d / _3d / _4d（最多 4 维；更多维用上面通用入口）
void ejit_taskpool_release_read(uint32_t bucketIndex);    // 用完 *outFn 后必须调用
void ejit_taskpool_set_instance_enabled(uint32_t dimType, uint32_t instanceId, uint32_t enabled);
unsigned ejit_taskpool_pending_count(void);
```

返回语义：`EJIT_OK` -> 命中；`EJIT_PENDING` -> 异步编译已提交，`*outFn` 为 NULL（本次走 AOT 回退）；负值 -> 失败。同步模式未命中时阻塞内联编译。这些通常由 wrapper 自动调用。

### 9.4 运行时配置与符号注册

```c
void                ejit_set_compile_mode(ejit_compile_mode_t mode);
ejit_compile_mode_t ejit_get_compile_mode(void);
void ejit_register_symbol(const char *name, void *addr);                    // 裸金属 JIT 符号解析
void ejit_register_lifecycle(const char *lifecycleName, uint32_t *slotOut); // 通常编译器自动生成
void ejit_register_funcindex(const char *funcName, uint32_t *slotOut);
void ejit_register_icache_slot(const char *funcName, void *slot, uint32_t numDims);
```

### 9.5 诊断与内省

日志级别、统计、转储、注册表内省、错误报告、缓存失效等接口详见《EJIT 诊断与调试指南》。

---

## 10. 运行时配置

```c
typedef struct {
  ejit_compile_mode_t compileMode;   // SYNC / ASYNC
  ejit_opt_level_t    optLevel;      // L1 / L2 / L3
  size_t maxCodeMemory;              // JIT 代码内存预算
  size_t maxDataMemory;              // JIT 数据内存预算
  size_t maxCacheEntries;            // cache 条目数上限
  size_t maxCacheSize;               // cache 字节上限
  bool   enableLogger;               // 启用运行时日志（零初始化为 false，须显式置 true）
  bool   forceStaticRegistry;        // 强制静态注册表路径（跳过构造函数，见 §7）
  const char *dumpJITDir;            // 非空 -> 把优化后 IR(.ll) 落盘到此目录
} ejit_config_t;
```

> `ejit_config_t` 字段无默认值：`ejit_config_t cfg = {};` 得到 `enableLogger = false`、`compileMode = SYNC`。需显式设置关心的字段后再 `ejit_init(&cfg)`。

优化级别：L1（轻量常量折叠 / 死代码消除）、L2（默认，含分支折叠、内联、CFG 化简）、L3（更激进，含循环展开，注意控制函数代码膨胀）。

---

## 11. 端到端示例

```c
#include <stdint.h>
#include <stdio.h>
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

struct BoardConfig { ejit_may_const uint32_t boardType; uint32_t xx; };
struct CellConfig  { ejit_may_const uint32_t cellType; uint32_t trafficLoad; };

ejit_period(static)   struct BoardConfig g_boardCfg;
ejit_period_arr(cell) struct CellConfig  g_cellCfg[16];

ejit_entry
void process_cell(ejit_period_arr_ind(cell) uint8_t cellIdx) {
    if (g_boardCfg.boardType == 0x01) {
        if (g_cellCfg[cellIdx].cellType == 0xFD)
            g_cellCfg[cellIdx].trafficLoad += 10;
        else
            g_cellCfg[cellIdx].trafficLoad += 20;
    }
}

ejit_period_lc(cell)
void update_cell_config(ejit_period_arr_ind(cell) uint8_t cellIdx) {
    g_cellCfg[cellIdx].cellType = 0xFD;
}

int main(int argc, char **argv) {
    uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;

    ejit_config_t cfg = {};
    cfg.compileMode  = EJIT_COMPILE_ASYNC;
    cfg.optLevel     = EJIT_OPT_L2;
    cfg.enableLogger = true;
    ejit_init(&cfg);

    ejit_activate("cell", ci);   /* 激活实例 ci，g_cellCfg[ci].cellType 可作 JIT 常量 */

    process_cell(ci);            /* 首次：未命中 -> JIT 编译特化（异步模式本次走 AOT） */
    process_cell(ci);            /* 后续：经 inline-cache 直接分派 */

    update_cell_config(ci);      /* 入口 deactivate、出口 activate，退役旧特化 */
    process_cell(ci);            /* 按新 cellType 重编译 */

    ejit_shutdown();
    return 0;
}
```

编译（含 inline cache + miss 冷隔离）：

```bash
clang -fembed-bitcode -O2 \
      -mllvm -ejit-inline-cache -mllvm -ejit-missfn-cold \
      process.c
```

链接脚本（裸金属）须含：注册表段（§6.1）、`.text.ejit_dispatch`（§6.3，若用聚簇）、`.text.unlikely`（§6.4，若用 miss 冷隔离）、固定代码段（§6.2，若用固定段）、共享段（§6.5，若用共享 taskpool）。

---

## 12. 使用约束与限制

- **编译器**：仅支持 clang（属性与位码嵌入为 clang 专有）。
- **递归**：`ejit_entry` 函数不可递归。
- **`always_inline`**：`ejit_entry` 与 `ejit_period_lc` 函数不可 `always_inline`（告警并丢弃）。
- **`may_const`**：仅整型 / 布尔 / 浮点 / 嵌套结构体 / 上述数组字段；`volatile` 永不视为常量；在 `ejit_period_lc` 外修改会告警。
- **`ejit_period_lc`**：须配对同名 `ejit_period_arr_ind` 参数。
- **跨 TU 内联**：不支持。`ejit_entry` 应只调用内部（`static`）函数。
- **数组规模**：`ejit_period_arr` 数组 ≤ 100 元素；全程序 ≤ 1024 个。指针形式无静态上限（集成方保证越界安全）。
- **维度数**：每函数最多 4 个 `ejit_period_arr_ind` 参数。
- **inline cache**：要求运行库启用跨核共享代码指针支持；每维槽数 `D` 须 AOT 与运行库一致；维度数 > 4 为编译错误。
- **固定代码段**：要求平台提供 `enable_rw`；区域大小由链接脚本定（无运行库配置项）。
- **并发模型**：主线程跑用户代码；异步编译在单一后台 worker 上运行。异步模式下 `ejit_entry` 副作用须幂等（首次走 AOT 回退）。
- **函数指针调用**：间接 / 函数指针调用不做特化（仅直接 `ejit_entry` 调用特化）。
- **内存预算**：JIT 代码内存受 `maxCodeMemory` 约束、cache 受 `maxCacheEntries`/`maxCacheSize` 约束；耗尽时淘汰 / 回退 AOT，不崩溃。

---

## 附录：相关文档

| 文档 | 内容 |
|------|------|
| `EJIT_DIAGNOSTICS.md` | 诊断与上板调试指南（日志、统计、转储、编译期诊断、排查流程） |
| `ejit_registry.ld`（运行库源码内） | 链接脚本片段模板（inline + INSERT） |
| `CLANG_ATTR_DESIGN.md` | ejit 属性的 TableGen / Sema / CodeGen 设计 |
| `SPEC4.md` | EJIT 总体设计（SPEC4） |
| `EJIT_SRE_CODE_POOL.md` | 代码内存池设计（W^X / 4K 封固 / 固定代码段） |
| `EJIT_SRE_TASKPOOL.md` | taskpool 编译调度设计 |
| `EJIT_ICACHE_MULTIVERSION.md` / `EJIT_ICACHE_ANALYSIS.md` | 多版本 inline cache 设计与分析 |
