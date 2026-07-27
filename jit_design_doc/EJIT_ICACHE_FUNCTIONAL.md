# EJIT Inline Cache 功能说明与数据报告

> 面向**使用者**：功能说明、使用方法、性能数据、逐维汇编。
> 目标平台：裸核 aarch64_be。技术设计见 `EJIT_ICACHE_MULTIVERSION.md`。

## 1. 功能简介

EJIT Inline Cache 为 `ejit_entry` 函数提供**多版本内联缓存**：首次按维度身份（identity）解析并编译特化后，后续同身份调用**直接尾调用特化代码**，跳过 taskpool 的 `compile_or_get` / `readers` 令牌 / 桶槽扫描 / `release_read`，把每次命中调用压到 4–9 cycle。

**特性**

- **多版本**：按 `ejit_dim` 参数值索引 `[D]^numDims` 数组，每个身份独立槽，互不覆盖。
- **frame-less 命中路径**：探针 + 尾调用，无栈帧、无运行时调用。
- **零运行时调用**：命中路径不调任何 runtime 函数，直接读自己的全局槽。
- **安全**：生产环境下 JIT 代码不释放，缓存指针永不悬空。

## 2. 适用场景

- 裸核 aarch64_be。
- 按 `ejit_dim` 身份多态派发的 `ejit_entry`（如按 cell / carrier / layer 等维度特化）。
- 命中率高的热路径（首次 miss 后稳定命中）。

## 3. 使用方法

你只需要**编译业务代码**--无需构建 EJIT 运行时（ejit.a），无需配置 CMake / preset。EJIT 工具链（预编译的 clang + 运行时）已由平台提供，运行时已链接进系统，你只编译自己的业务代码。

### 3.1 标注业务代码

```c
/* 1) 周期数组全局：ejit_period_arr("name") */
__attribute__((ejit_period_arr("cell"))) struct CellConfig cellCfg[16];

/* 2) ejit_entry 函数 + 维度参数：ejit_period_arr_ind("name")（别名 ejit_dim） */
__attribute__((ejit_entry))
int process_cell(__attribute__((ejit_period_arr_ind("cell"))) int ci) {
    return cellCfg[ci].cellType;   /* may_const 字段触发特化 */
}
```

- 维度参数可用别名 `ejit_dim`：`__attribute__((ejit_dim("cell"))) int ci`。
- 多维：函数加多个维度参数，每个标 `ejit_period_arr_ind` / `ejit_dim`；**最左参数 = dim0**（索引顺序）。

### 3.2 编译业务代码

用 EJIT 工具链编译业务代码时，加**一个**选项开启 icache（默认关闭）：

```
-mllvm -ejit-inline-cache
```

把它加到你正常的编译命令里，与 `--target=aarch64_be-...`、`-Os` 等常规选项并列即可。运行时由平台预构建，你不需要构建或链接它。

如需测量命中开销（wrapper-timing 自动报告），额外加 `-mllvm -ejit-wrapper-timing`。

### 3.3 约束

- 维度数 ≤ 4，超限**编译报错**。
- 每个维度参数值 **< 8**（`D=8`），且为整数类型。**无运行时边界检查**--依赖标注保证取值范围。
- 维度名非空、同一函数内不重复。

## 4. 工作机制

1. **首次调用某身份**（如 `ci=3`）：miss -> 尾调用 MissFn -> 编译特化 -> 把函数指针写入该身份的槽 -> 执行。
2. **后续同身份调用**（`ci=3`）：直接读槽 -> 非空 -> 尾调用该特化。无 taskpool、无 `release_read`。
3. **不同身份**（`ci=5`）：各自命中自己的槽，互不影响。

每身份的槽在首次解析后写入，后续只读（特化不变，覆写无害）。每核私有，各自填各自读，无跨核写竞争。

## 5. 性能数据

命中路径实测 cycle（frame-less，`-Os`，aarch64_be，`D=8`）：

| ejit_dim 维度数 | hit 路径实测 cycle | LLVM 指令数 | ILP 收益 |
|---|---|---|---|
| 0 | **4** | 4 | 0（1:1） |
| 1 | **5** | 5 | 0（1:1） |
| 2 | **6** | 7 | -1（sxtw 与 adrp 重叠） |
| 3 | **8** | 9 | -1 |
| 4 | **9** | 11 | -2 |

- 命中路径 = 探针（移位索引 + load + 判空）+ 尾调用，无栈帧、无 call、无 `release_read`、无跨核 RMW。
- 2–4 维的实测 cycle **比指令数还少**：`sxtw`（ALU 符号扩展）与 `adrp`（PC 相对寻址，有访存延迟）并行执行，被流水线重叠。维度越高，索引链越长，能重叠的越多。
- 对比未开启 icache / 首次 miss 的 taskpool 路径（`compile_or_get` + `readers` 跨核 RMW + 桶扫描 + 间接 call + `release_read`），命中路径压到 **4–9 cycle**。
- 对比 v2 单态（~7–8 cycle，有帧 + ldar）：0–3 维**优于或持平 v2**，4 维仅 +1~2 cycle。多版本的代价几乎为零。

## 6. 注意事项

- **维度参数值必须 < 8**：越界无运行时检查，依赖标注正确。
- **冷启动**：每身份首次调用走 miss（编译 + 填槽），有一次编译开销；之后同身份稳定命中。
- **运行时**：icache 在标准 EJIT 运行时下可用（平台已提供）。若运行时配置了代码回收，icache 会自动关闭、退化为 taskpool 路径，不影响正确性。

## 7. FAQ

**Q：加了 `-mllvm -ejit-inline-cache` 但没命中？**
A：检查--维度参数值是否 < 8；函数是否标了 `ejit_entry` 且维度参数标了 `ejit_period_arr_ind`/`ejit_dim`；周期数组全局是否标了 `ejit_period_arr`。

**Q：维度数 > 4？**
A：编译报错；该函数无法用 icache，需拆分或降维。

**Q：多个核同时调用同一身份？**
A：每核私有槽，各自填各自读，无跨核写竞争；命中路径无跨核 RMW。

**Q：特化会被释放吗？缓存指针会悬空吗？**
A：生产环境 JIT 代码不释放，缓存指针安全。若运行时启用代码回收，icache 自动关闭（退化为 taskpool 路径）。

---

## 附录 A. 逐维度汇编数据报告

> 生成方法：`opt -passes=ejit-wrapper-gen -ejit-inline-cache -S`（LLVM 21）生成 wrapper IR，`clang-18 --target=aarch64_be-linux-gnu -Os -S` 降至 aarch64_be 汇编。`D = EJIT_ICACHE_DIM_SIZE = 8`（power-of-2）。

### 架构：Lever B（frame-less wrapper + noinline MissFn）

wrapper（`ejit_entry`）是**无帧命中路径**：探针（GEP + `ldr` + `cbz`）+ 两个尾调用（`br spec` 命中 / `br MissFn` miss）。慢路径（funcidx 守卫 + `compile_or_get` + dispatch + AOT 体）在 per-function `noinline MissFn` 中，自带栈帧。miss 罕见，尾调用开销可忽略。

### 各维度命中路径汇编（`-Os`，入口 -> `br spec`）

#### 0 维（标量槽，4 cycle）
```asm
adrp  x8, @__ejit_icache_fn_<name>
ldr   x1, [x8, :lo12:@__ejit_icache_fn_<name>]   ; :lo12: 折进 ldr
cbz   x1, .miss
br    x1
```

#### 1 维（5 cycle）
```asm
adrp  x8, @__ejit_icache_fn_<name>
add   x8, x8, :lo12:@__ejit_icache_fn_<name>
ldr   x1, [x8, w0, sxtw #3]    ; dim0*8, sxtw 折进 ldr 寻址
cbz   x1, .miss
br    x1
```

#### 2 维（6 cycle）
```asm
sxtw  x8, w0                   ; dim0 符号扩展（与 adrp 并行 -> ILP 隐藏）
adrp  x9, @__ejit_icache_fn_<name>
add   x9, x9, :lo12:@__ejit_icache_fn_<name>
add   x8, x9, x8, lsl #6       ; + dim0*64  (D*8 = 8<<3 = <<6)
ldr   x2, [x8, w1, sxtw #3]    ; + dim1*8,  sxtw 折进 ldr
cbz   x2, .miss
br    x2
```

#### 3 维（8 cycle）
```asm
sxtw  x8, w0
adrp  x10, @__ejit_icache_fn_<name>
add   x10, x10, :lo12:@__ejit_icache_fn_<name>
sxtw  x9, w1
add   x8, x10, x8, lsl #9      ; dim0*512
add   x8, x8, x9, lsl #6       ; dim1*64
ldr   x3, [x8, w2, sxtw #3]    ; dim2*8
cbz   x3, .miss
br    x3
```

#### 4 维（9 cycle）
```asm
sxtw  x8, w0
adrp  x10, @__ejit_icache_fn_<name>
add   x10, x10, :lo12:@__ejit_icache_fn_<name>
sxtw  x9, w1
add   x8, x10, x8, lsl #12     ; dim0*4096
sxtw  x10, w2
add   x8, x8, x9, lsl #9       ; dim1*512
add   x8, x8, x10, lsl #6      ; dim2*64
ldr   x4, [x8, w3, sxtw #3]    ; dim3*8
cbz   x4, .miss
br    x4
```

### 指令结构拆解

每维（非末维）= 1 `sxtw` + 1 `add lsl#N`（2 条）；末维的 `sxtw` 折进 `ldr` 寻址（1 条）。
- `adrp` + `add :lo12:` = 基址（2 条；0 维把 `add` 折进 `ldr` -> 1 条）。
- `cbz` + `br` = 判空 + 尾调用（2 条）。
- `.miss` = `b <name>_miss`（冷路径，不在命中计数内）。

移位量 `#N` = `3 + 3*(numDims-1-i)`（D=8, cell 8B）：1 维 `#3`；2 维 `#6,#3`；3 维 `#9,#6,#3`；4 维 `#12,#9,#6,#3`。纯移位，无乘法（D power-of-2）。

### ILP 分析

`sxtw`（ALU 操作）与 `adrp`（PC 相对访存，有延迟）可并行。2–4 维中，非末维的 `sxtw` 被隐藏在 `adrp` 延迟后 -> 实测 cycle 比指令数少 1~2。维度越高，索引链越长，可重叠的 `sxtw` 越多（4 维省 2 条）。
