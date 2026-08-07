# EJIT 功能说明

> 支持功能发布说明书 · 面向集成方。
>
> 说明本版本 EJIT 提供的功能、支持的平台、集成接口与使用约束。本文不涉及如何构建 EJIT 运行库本身；若需要某项能力（如逐调用统计、汇编转储、固定代码段）的运行库，请联系 EJIT 提供方获取。

---

## 1. 概述

EJIT（EmbeddedJIT）是面向嵌入式场景的 JIT 编译系统，核心思想是**时间窗常量特化**：

- 在一段声明的**时间窗**（period）内，部分运行时数据被视作编译期常量。
- JIT 据此为函数产生**特化版本**：相关分支被折叠、内存读被替换为字面常量、死代码被消除。
- 时间窗关闭后，执行回退到 AOT 版本。

集成方通过在 C/C++ 源码中标注 ejit 属性来声明特化意图，链接 EJIT 运行库，在运行时激活时间窗即可获得特化收益。所有 JIT 失败均回退 AOT，不会导致崩溃。

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

裸金属环境下 EJIT 以 freestanding 方式运行，以下 `extern "C"` 符号由**集成方平台实现**（EJIT 只声明不定义，缺失则链接报错）：

| 平台符号 | 作用 |
|----------|------|
| `SRE_printf` | 所有诊断日志输出 |
| `SRE_CycleCountGet64` | wrapper 计时用的周期计数时间戳 |
| `SRE_MemDbgAlloc` | 代码内存池的原始内存分配 |
| `enable_ex` | 将页封固为 RX（W^X 执行） |
| `enable_rw` | 将页置为 RW（须清除执行权限） |
| `split_2m_to_4k` | 将 2MiB 池拆分为 4KiB 页（4K 封固模式） |
| `SRE_TaskCreate` / `SRE_TaskDelete` / `SRE_TaskDelay` | 异步编译 worker 任务的创建 / 删除 / 延时 |

宿主构建下这些符号有 weak 空实现兜底，同一份代码可在 Linux 下直接构建运行用于测试。跨核共享 taskpool 的共享状态置于核间共享内存，并绑定到平台提供的“当前核”接口。

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

支持整型、布尔、浮点、嵌套结构体字段及上述类型的数组。`volatile` 字段永不视为常量。对 `may_const` 字段取无 `const` 限定符的地址会告警（非 const 指针允许在 `ejit_period_lc` 外写入，破坏时间窗常量假设）。

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

约束：不可递归；隐式依赖 `"static"` 窗口；对数组窗的依赖须经 `ejit_period_arr_ind` 声明；**不可标注 `always_inline`**（该函数须在内联器中存活以供 JIT 特化，冲突时 Sema 告警并忽略 `always_inline`）。异步模式下首次调用走 AOT 回退，故副作用须幂等（同参同效）；非幂等函数请用同步模式。

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

属性如何协同产生特化：

1. `ejit_entry` 标记待特化函数；编译器嵌入其位码并在入口插分发 stub。
2. **时间窗**（period）是命名、可激活的作用域，覆盖一组全局。`"static"` 窗口始终激活（运行期不变的数据）；自定义窗（`ejit_period_arr`）管理“稳定一段时间后变更”的数据。
3. `ejit_may_const` 标记的字段在窗口激活时，其当前值被 JIT 在编译期读出并折入特化。
4. `ejit_period_arr` / `ejit_period_arr_ind` 构成多实例维度：`ejit_period_arr(cell) struct CellConfig g[16]` 声明 16 个实例；`ejit_period_arr_ind(cell) uint8_t idx` 表示“本次调用针对实例 `idx` 特化”。
5. `ejit_activate("cell", 5)` 激活 `cell` 窗口的实例 5；此后对 `idx==5` 的 `ejit_entry` 调用解析到一个把 `g[5]` 中 `may_const` 字段折为字面常量的特化版本。`ejit_deactivate("cell", 5)` 失效该特化的缓存条目，下次调用重编译。
6. `ejit_period_lc` 函数在写入前后 bracket deactivate/activate，确保写入中途不使用陈旧特化。

**举例**：基站 `process_cell(cellIdx)` 读 `g_cellCfg[cellIdx].cellType` 并分支。无窗口时跑通用 AOT 代码。`ejit_activate("cell", 3)` 后，首次 `process_cell(3)` 触发 JIT：读出 `g_cellCfg[3].cellType`（假设 `0xFD`），把 `if (cellType == 0xFD)` 折成恒真，缓存特化函数指针。后续 `process_cell(3)` 直接跳到缓存特化。调用 `process_cell(7)`（不同实例）解析到不同特化（或触发另一次编译）--多版本 inline cache 按 `(维度, 实例)` 身份各存一槽。

---

## 5. 运行时能力

### 5.1 异步编译 / taskpool 调度

异步模式（`EJIT_COMPILE_ASYNC`）下，cache 未命中时把编译请求入队到单一后台 worker，立即返回 `EJIT_PENDING`--调用方本次走 AOT 回退，下次调用即命中新鲜发布的特化。taskpool 对并发的相同请求（相同 funcIndex + 维度身份）去重，故对同一特化的并发调用只编译一次。队列满、去重满、发布失败等状况可通过统计 API 观察。每实例可单独禁用快速路径。同步模式（`EJIT_COMPILE_SYNC`）在未命中时于调用线程内联编译。

### 5.2 代码内存池（W^X、4K 封固、固定代码段）

JIT 代码从受管内存池分配（默认 2MiB slab，或固定代码段模式下位于链接脚本固定的 `.text.ejit` 区域，给 JIT 稳定地址、±128MiB 内可直 `bl`/`adrp` 到 AOT）。内存写时 RW、封固时 RX--严格 W^X（写时清执行权限，执行时清写权限）。4K 封固模式下 2MiB slab 拆为 4KiB 页，每页代码就绪即单独封固，缩短可写窗口。占用可通过 `ejit_get_code_pool_stats()` 查询；代码内存预算由 `ejit_config_t.maxCodeMemory` 配置。

### 5.3 inline cache（多版本、per-core）

特化编译后，其函数指针发布到按函数维度 shaping 的 per-function、direct-indexed inline-cache 槽（`@__ejit_icache_fn_<name>`）。AOT wrapper 命中路径直接读该槽（GEP + 原子 load + 判空 + 间接 call），无 `ejit_*` 调用，故首次编译后的分派是一次 load + branch。cache 是多版本的：按 `(instanceId…)` 身份各存一槽，使同一函数以不同维度值调用时各取正确特化。cache 状态 per-core（每核须自行封固其可执行映射）。

### 5.4 L0 分派 cache

per-core L0 分派 cache 在热点路径上前置于 taskpool 查找；在 `ejit_invalidate` / deactivate / cache 退役时失效。

### 5.5 IR/ASM 转储与注册表内省

可转储优化后 JIT IR（`dumpJITDir` 落盘或 `ejit_dump_func` 内存捕获），并内省注册表（`ejit_print_registry` / `ejit_print_func_meta` / `ejit_print_active` / `ejit_print_version`）。详见《EJIT 诊断与调试指南》。

---

## 6. 公共 C API

头文件：`llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h`（`extern "C"`）。以下按能力分组列出主要接口。

### 6.1 类型与枚举

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

### 6.2 生命周期

```c
ejit_status_t ejit_init(const ejit_config_t *config);
void          ejit_shutdown(void);
ejit_status_t ejit_activate(const char *periodName, uint8_t cellIdx);     // 激活 name 下所有数组的第 idx 实例
ejit_status_t ejit_deactivate(const char *periodName, uint8_t cellIdx);   // 失效；依赖该实例的缓存条目退役
ejit_status_t ejit_activate_all(const char *periodName);
ejit_status_t ejit_deactivate_all(const char *periodName);
bool          ejit_is_active(const char *periodName, uint8_t cellIdx);
```

激活以 name + index 为键（无数组指针维度）；对较小的数组，越界的 `cellIdx` 在该数组上静默跳过。

### 6.3 编译（统一入口 + 定维快路径）

```c
ejit_status_t ejit_taskpool_compile_or_get(uint32_t funcIndex,
                                           const ejit_dim_pair_t *dims, uint32_t numDims,
                                           void **outFn, uint32_t *outBucket);
// 定维变体：_0d / _1d / _2d / _3d / _4d（最多 4 维；更多维用上面通用入口）
void ejit_taskpool_release_read(uint32_t bucketIndex);    // 用完 *outFn 后必须调用（释放读令牌）
void ejit_taskpool_set_instance_enabled(uint32_t dimType, uint32_t instanceId, uint32_t enabled);
unsigned ejit_taskpool_pending_count(void);
```

返回语义：`EJIT_OK` -> 命中，`*outFn` 可用；`EJIT_PENDING` -> 异步编译已提交，`*outFn` 为 NULL（本次走 AOT 回退）；负值 `EJIT_ERR_*` -> 失败。同步模式未命中时阻塞内联编译。

### 6.4 运行时配置

```c
void                ejit_set_compile_mode(ejit_compile_mode_t mode);
ejit_compile_mode_t ejit_get_compile_mode(void);
```

### 6.5 符号注册（裸金属手工注册用；通常由编译器自动生成）

```c
void ejit_register_symbol(const char *name, void *addr);
void ejit_register_lifecycle(const char *lifecycleName, uint32_t *slotOut);
void ejit_register_funcindex(const char *funcName, uint32_t *slotOut);
void ejit_register_icache_slot(const char *funcName, void *slot, uint32_t numDims);
```

### 6.6 诊断与内省

日志级别、统计、转储、注册表内省、错误报告、缓存失效等接口详见《EJIT 诊断与调试指南》。

---

## 7. 运行时配置

```c
typedef struct {
  ejit_compile_mode_t compileMode;   // SYNC / ASYNC
  ejit_opt_level_t    optLevel;      // L1 / L2 / L3
  size_t maxCodeMemory;              // JIT 代码内存预算
  size_t maxDataMemory;              // JIT 数据内存预算
  size_t maxCacheEntries;            // cache 条目数上限
  size_t maxCacheSize;               // cache 字节上限
  bool   enableLogger;               // 是否启用运行时日志（零初始化为 false，须显式置 true）
  bool   forceStaticRegistry;        // 强制静态注册表路径（跳过构造函数）
  const char *dumpJITDir;            // 非空 -> 把优化后 IR(.ll) 落盘到此目录
} ejit_config_t;
```

> `ejit_config_t` 字段无默认值：`ejit_config_t cfg = {};` 得到 `enableLogger = false`、`compileMode = SYNC`、`optLevel` 未指定。需显式设置关心的字段后再 `ejit_init(&cfg)`。

典型优化级别：L1（轻量常量折叠 / 死代码消除）、L2（默认，含分支折叠、内联、CFG 化简）、L3（更激进，含循环展开，注意控制函数代码膨胀）。

---

## 8. 使用约束与限制

- **编译器**：仅支持 clang（属性与位码嵌入为 clang 专有）。
- **递归**：`ejit_entry` 函数不可递归。
- **`always_inline`**：`ejit_entry` 与 `ejit_period_lc` 函数不可 `always_inline`（告警并丢弃）。历史的 `-ejit-noinline-entry` 标志已移除。
- **`may_const`**：仅支持整型 / 布尔 / 浮点 / 嵌套结构体 / 上述数组字段；`volatile` 字段永不视为常量；在 `ejit_period_lc` 外修改 `may_const` 字段会告警。
- **`ejit_period_lc`**：须配对同名 `ejit_period_arr_ind` 参数。
- **跨 TU 内联**：不支持。`ejit_entry` 应只调用内部（`static`）函数；ThinLTO/FullLTO 跨模块内联已回退移除。
- **数组规模**：`ejit_period_arr` 数组 ≤ 100 元素；全程序 ≤ 1024 个此类数组。指针形式无静态上限（集成方保证越界安全）。
- **维度数**：每函数最多 4 个 `ejit_period_arr_ind` 参数；每维度最多关联 1024 个数组。
- **并发模型**：主线程跑用户代码；异步编译在单一后台 worker 上运行。异步模式下 `ejit_entry` 副作用须幂等（首次走 AOT 回退）。
- **函数指针调用**：间接 / 函数指针调用不做特化（仅直接 `ejit_entry` 调用特化）。
- **内存预算**：JIT 代码内存受 `maxCodeMemory` 约束、cache 受 `maxCacheEntries`/`maxCacheSize` 约束；耗尽时运行库淘汰 / 回退 AOT，不崩溃。
- **激活粒度**：`ejit_activate(name, idx)` 以 name + index 为键，无数组指针级激活。

---

## 9. 端到端示例

```c
#include <stdint.h>
#include <stdio.h>
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"   /* 宏 + C ABI */

/* 1. 带 may_const 字段的结构体（窗口内当作常量）。 */
struct BoardConfig { ejit_may_const uint32_t boardType; uint32_t xx; };
struct CellConfig  { ejit_may_const uint32_t cellType; uint32_t trafficLoad; };

/* 2. 全局归入时间窗。"static" 窗口始终激活。 */
ejit_period(static)   struct BoardConfig g_boardCfg;
ejit_period_arr(cell) struct CellConfig  g_cellCfg[16];

/* 3. ejit_entry：隐式依赖 "static"；cellIdx 选择特化哪个 cell 实例。 */
ejit_entry
void process_cell(ejit_period_arr_ind(cell) uint8_t cellIdx) {
    if (g_boardCfg.boardType == 0x01) {            /* boardType 折为常量 */
        if (g_cellCfg[cellIdx].cellType == 0xFD)   /* cellType 针对本 idx 折为常量 */
            g_cellCfg[cellIdx].trafficLoad += 10;
        else
            g_cellCfg[cellIdx].trafficLoad += 20;
    }
}

/* 4. 生命周期守卫：入口 deactivate(cell,idx)，出口 activate(cell,idx)。 */
ejit_period_lc(cell)
void update_cell_config(ejit_period_arr_ind(cell) uint8_t cellIdx) {
    g_cellCfg[cellIdx].cellType = 0xFD;
}

int main(int argc, char **argv) {
    uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;

    /* 5. 初始化运行库。 */
    ejit_config_t cfg = {};
    cfg.compileMode  = EJIT_COMPILE_ASYNC;
    cfg.optLevel     = EJIT_OPT_L2;
    cfg.enableLogger = true;
    ejit_init(&cfg);

    /* 6. 激活 cell 窗口的实例 ci：g_cellCfg[ci].cellType 现可作 JIT 常量。 */
    ejit_activate("cell", ci);

    /* 7. 首次调用：未命中 -> JIT 编译 (process_cell, cell=ci) 特化，
          折入 g_boardCfg.boardType 与 g_cellCfg[ci].cellType，封固 RX，
          发布到 inline-cache。异步模式下本次走 AOT 回退，下次命中。 */
    process_cell(ci);

    /* 8. 后续调用：经 (cell=ci) 的 inline-cache 槽直接 load + 间接 call。 */
    process_cell(ci);

    /* 9. 修改 cell 配置：ejit_period_lc 包装入口 deactivate、出口 activate，
          退役旧特化；下次 process_cell(ci) 按新 cellType 重编译。 */
    update_cell_config(ci);
    process_cell(ci);

    ejit_shutdown();
    return 0;
}
```

**运行期发生了什么**：步骤 7 触发特化--JIT 加载嵌入的 `process_cell` 位码，把 `ejit_period_arr_ind(cell)` 参数与 `may_const` 字段读（`g_boardCfg.boardType`、`g_cellCfg[ci].cellType`）替换为当前运行时值，跑配置的优化流水线（默认 L2：常量传播、死代码消除、分支折叠、内联、CFG 化简），生成 AArch64 机器码写入 W^X 代码池，封固 RX，把函数指针发布到 `(cell=ci)` 的 inline-cache 槽。步骤 8 经该槽直接分派。步骤 9 的修改经 deactivate/reactivate 失效旧特化，随后调用按新常量重编译。

---

## 附录：相关文档

| 文档 | 内容 |
|------|------|
| `EJIT_DIAGNOSTICS.md` | 诊断与上板调试指南（日志、统计、转储、编译期诊断、排查流程） |
| `CLANG_ATTR_DESIGN.md` | ejit 属性的 TableGen / Sema / CodeGen 设计 |
| `SPEC4.md` | EJIT 总体设计（SPEC4） |
| `PASS1_EJitRegisterBitcode.md` / `PASS2_EJitRegisterPeriod.md` | AOT pass 设计 |
| `EJIT_SRE_CODE_POOL.md` | 代码内存池设计（W^X / 4K 封固 / 固定代码段） |
| `EJIT_SRE_TASKPOOL.md` | taskpool 编译调度设计 |
| `EJIT_ICACHE_MULTIVERSION.md` | 多版本 inline cache 设计 |
