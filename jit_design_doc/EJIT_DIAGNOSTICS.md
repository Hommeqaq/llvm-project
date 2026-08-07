# EJIT 诊断与调试指南

> 面向已将 EJIT 集成进自己二进制的集成方。说明上版调试时如何用 EJIT 提供的诊断手段定位问题。
>
> 本文不涉及如何构建 EJIT 运行库本身。若某诊断能力调用后无输出或返回 `EJIT_ERR_DISABLED`，说明你链接的 EJIT 运行库未启用该能力--向 EJIT 提供方确认即可。

EJIT 的诊断手段按“何时用到”分两类：

- **运行时**（上版调试期）：二进制运行过程中，通过 C API 动态调整日志级别、读取统计、转储 IR/ASM、内省注册表。这是上板排障的主要手段（§1–§8）。
- **编译期**（开发构建期）：`clang` 编译你标注了 ejit 属性的源码时发出的告警 / 报告（§9）。它们在你构建含 `ejit_entry` 的代码时就已产出，帮你提前发现“特化没收益”“维度声明错”等问题。

### 现象 -> 先看哪里

| 现象 | 先看 |
|------|------|
| 注册表 / 特化参数对不对 | §4 注册表与元数据内省 |
| 某个 `ejit_entry` 没收益 / 没被特化 | §9.2 编译期特化价值诊断 + §4 `ejit_print_func_meta` |
| 该编译的没编译 / 编译失败 | §6 错误报告 + §1 日志 + §2.2 taskpool 统计 |
| 命中率低 / 性能差 | §2 统计 + §5 wrapper 计时 |
| 代码内存趋紧 / 耗尽 | §2.3 代码池统计 |
| AArch64 上分支超范围 / 走了 stub | §8 JITLink 分支重定位诊断 |
| 想看某函数特化后的 IR / 汇编 | §3 JIT IR / ASM 转储 |

---

## 1. 运行时日志

运行时日志分四级，可在不重新编译的情况下动态调整。

| 级别 | 值 | 作用 |
|------|----|------|
| `EJIT_LOG_OFF` | 0 | 不输出 |
| `EJIT_LOG_INFO` | 1 | 关键事件（默认）：init/shutdown、编译 begin/OK/FAIL、cache MISS、激活、错误、注册消费摘要 |
| `EJIT_LOG_VERBOSE` | 2 | 逐项细节：每次首次注册、逐函数 struct-field 统计、逐次 `compile_or_get`、taskpool 请求 |
| `EJIT_LOG_DEBUG` | 3 | 内部机理：幂等注册跳过、逐 load 替换失败、staging 内部、funcMeta 缓存 |

```c
void ejit_set_log_level(ejit_log_level_t level);   // 立即生效，影响后续所有日志输出
ejit_log_level_t ejit_get_log_level(void);          // 查询当前级别
```

```c
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

ejit_set_log_level(EJIT_LOG_INFO);     // 生产环境降低日志量
ejit_set_log_level(EJIT_LOG_VERBOSE);  // 排查问题时提升到细节级
```

> 若调用后无任何日志输出，说明你的 EJIT 运行库未启用诊断日志输出，向提供方确认。

---

## 2. 运行时统计

### 2.1 旧版 LRU cache 统计

```c
typedef struct {
  size_t entryCount;      // cache 中条目数
  size_t totalCodeSize;   // 已缓存代码总字节
  size_t maxSize;         // cache 容量上限
  uint64_t hits;          // 命中次数
  uint64_t misses;        // 未命中次数
  uint64_t evictions;     // 淘汰次数
} ejit_stats_t;

ejit_status_t ejit_get_stats(ejit_stats_t *stats);
```

**作用**：查询旧版 LRU `EJitCache` 的命中 / 未命中 / 淘汰与内存占用。

### 2.2 SRE taskpool 统计

```c
typedef struct {
  uint64_t cacheHits;                   // 命中 taskpool cache 的调用数
  uint64_t asyncCompiles;               // worker 成功编译数
  uint64_t asyncEnqueues;               // 入队请求数
  uint64_t alreadyPending;              // 被合并的重复提交
  uint64_t queueFull;                   // 队列满被拒的入队
  uint64_t compileFailed;               // 失败 / 取消 / 丢弃的编译
  uint64_t publishFailed;               // 无法进入 cache 的结果
  uint64_t instanceDisabled;            // 实例禁用快速路径命中
  uint64_t instanceDisabledPreActivate; // 首次激活前命中的子集（init->activate 窗口）
  uint32_t readyEntries;                // 存活 ready cache 条目
  uint32_t pendingEntries;              // 存活 in-flight 去重槽
  uint32_t queueApproxSize;             // 近似异步队列深度
  uint32_t reserved;                    // 保留，恒为 0
} ejit_taskpool_stats_t;

ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
void          ejit_taskpool_print_stats(void);   // 人可读形式经日志输出
void          ejit_taskpool_print_compiled(void);// 列出所有已编译特化（funcIdx/name/dims/fn）
uint32_t      ejit_taskpool_get_worker_core(void);// 返回 worker 运行所在核
unsigned      ejit_taskpool_pending_count(void); // 在途数量
```

**作用**：定位“为何没编译 / 为何编译慢 / 队列是否拥塞”。看 `cacheHits` 判断命中率，`alreadyPending` 判断重复提交合并，`queueFull`/`queueApproxSize` 判断拥塞，`compileFailed`/`publishFailed` 判断编译失败。

> 若 `cacheHits` 等逐调用计数全为零，说明运行库未启用逐调用统计（这些计数有热点路径开销，默认关闭）；`print_compiled()`、`get_worker_core()`、`pending_count()` 不受此影响，始终可用。

### 2.3 代码池统计

```c
typedef struct ejit_code_pool_stats_t {
  uint64_t poolCount;           // 已创建的 2MiB 池总数
  uint64_t sealedCount;         // 当前已封存（RX）的池
  uint64_t activeCount;         // 仍可写（RW）的池
  uint64_t usedBytes;           // 各池 bump 偏移之和
  uint64_t reservedBytes;       // 各池容量之和
  uint64_t wastedBytes;         // 已封存池尾部未用字节
  uint64_t sealInvocations;     // enable_ex 成功次数（4K 模式按 4K 页计）
  uint64_t splitInvocations;    // split_2m_to_4k 成功次数（4K 模式）
  uint64_t finalizedRangeCount; // 记录的可执行区间数
} ejit_code_pool_stats_t;

ejit_status_t ejit_get_code_pool_stats(ejit_code_pool_stats_t *out);
void          ejit_print_code_pool_stats(void);
```

**作用**：监控 JIT 代码内存占用与是否趋近耗尽（`usedBytes` vs `reservedBytes`）。返回值：`EJIT_OK` 成功；`EJIT_ERR_NOT_ACTIVE` 运行时未初始化；`EJIT_ERR_INVALID_PARAM` 入参为空；`EJIT_ERR_DISABLED` 运行库未含代码池支持。

> **固定代码段模式**：若你的运行库使用固定代码段模式（代码池位于链接脚本固定的 `.text.ejit` 区域，给 JIT 稳定地址），每次页状态转换会在 **INFO 级**打印 `enableRwRange` 系列日志（`begin` / `OK` / `FAIL`（未归属 / 跨池 / `enable_rw` 失败）/ `rollback`（部分失败时回退 RW->RX）），用于诊断 W^X 页状态转换。封固粒度（4K 页 vs 整 2MiB 池）影响 `sealInvocations` / `splitInvocations` 的计数粒度。

---

## 3. JIT IR / ASM 转储

提供两种互补的转储方式：**文件式**（一次性、全量、便于离线分析）与**内存捕获式**（运行时按名过滤、经日志回读）。

### 3.1 文件式 IR 转储（`dumpJITDir`）

把每个特化的 JIT 优化后 LLVM IR（`.ll`）落盘，文件名 `<funcName>_<cacheKey>.ll`，便于离线 diff / 审查。通过 `ejit_init()` 配置项启用：

```c
ejit_config_t cfg = {};
cfg.compileMode  = EJIT_COMPILE_ASYNC;
cfg.optLevel     = EJIT_OPT_L2;
cfg.enableLogger = true;
cfg.dumpJITDir   = "/tmp/ejit_ir";   // 非空即开启
ejit_init(&cfg);
```

### 3.2 内存捕获式 IR+ASM 转储

```c
void ejit_dump_func(const char *name);  // 按名开启捕获；name="*" 捕获全部；NULL/"" 关闭后续捕获
void ejit_dump_all(bool enable);        // 等价于 ejit_dump_func("*")（enable=true 时）
void ejit_print_dumped(const char *name);// 经日志打印已保存的 IR+ASM；NULL/"" 打印本核全部
```

运行时按函数名过滤捕获下一次 JIT 编译产生的**优化后 IR 与汇编**，再经日志逐行回读。适合现场定位某个函数的特化结果。

> - 捕获为**精确名匹配**，`"*"` 例外（捕获全部）。
> - 完整 IR/ASM 负载保留在 worker 核本地，不拷入共享 taskpool 内存。
> - 若回读内容只有 IR、没有汇编，说明运行库未启用汇编发射，向提供方确认。

```c
ejit_dump_func("process_cell");   // 只捕获感兴趣函数的下一次特化
// ... 触发该函数的 JIT 编译 ...
ejit_print_dumped("process_cell"); // 回读 IR+ASM

ejit_dump_all(true);              // 捕获所有特化
```

---

## 4. 注册表与元数据内省

```c
void ejit_print_registry(void);             // 打印全部已注册项
void ejit_print_func_meta(const char *name);// 打印某函数的 !ejit.metadata
void ejit_print_active(void);               // 打印当前激活的时间窗实例
void ejit_print_version(void);              // 打印运行库构建标识
```

| API | 作用 |
|-----|------|
| `ejit_print_registry` | 列出所有已注册 bitcode（funcIdx / name / size）、period 数组（period / var / base / size）、静态变量（var / addr），以及 funcIndex / lifecycle 计数。用于验证 AOT 注册是否正确填充运行时。 |
| `ejit_print_func_meta` | 解析并打印 `name` 的 `!ejit.metadata`：是否为 `ejit_entry`、其 `period_arr_ind` 参数槽、period 数组、`may_const` 字段偏移。用于诊断特化参数绑定与常量替换资格。 |
| `ejit_print_active` | 列出每个注册 period 下当前激活的 (period, cell)。静态变量视为恒激活。用于诊断“某 period 实例为何编译 / 为何没编译”。 |
| `ejit_print_version` | 打印运行库构建标识：LLVM 发行版本号与 llvm-project 源码的 git commit + 分支。**无需初始化运行时、不受日志级别门控**，便于将现场设备行为与确切源码版本对应。 |

```c
ejit_init(&cfg);
ejit_print_registry();                // 确认注册表
ejit_print_func_meta("process_cell"); // 查看某函数的特化元数据
ejit_print_active();                  // 查看当前激活实例
ejit_print_version();                 // 可在 init 前后任意时刻调用
```

---

## 5. Wrapper 计时

在 AOT 生成的 `ejit_entry` wrapper 中插入计时探针，测量 taskpool 查找、间接 JIT 调用、读令牌释放各段耗时，定位 wrapper 开销。运行库自动按固定间隔（默认每 100000 次调用）聚合打印一行汇总，避免日志刷屏。

**启用**：计时探针需在编译你的 ejit 代码时开启：

```bash
clang -fembed-bitcode ... -mllvm -ejit-wrapper-timing process.c
```

运行时聚合 API 由插桩 wrapper 自动调用，一般无需手动调用：

```c
uint64_t ejit_taskpool_trace_now(void);   // 取当前时间戳
void ejit_taskpool_trace_wrapper(uint32_t funcIndex, uint32_t status,
                                 void *fnPtr, uint32_t bucketIndex,
                                 uint64_t tBeforeLookup,
                                 uint64_t tAfterLookup,
                                 uint64_t tAfterFn,
                                 uint64_t tAfterRelease);
```

> 仅当 wrapper 以 `-mllvm -ejit-wrapper-timing` 构建时才会产生计时；时间戳单位由平台决定。

---

## 6. 错误报告

```c
typedef struct {
  int  code;
  char message[256];
  char funcName[128];
} ejit_error_t;

const ejit_error_t *ejit_get_last_error(void);
```

返回最近一次错误的指针（code / message / funcName），底层为预分配环形缓冲（最多 32 条），无动态分配。

配套配置项 `enableLogger`（`ejit_config_t` 的 `bool` 字段）控制错误日志器是否启用。注意该 C 结构体字段**无默认值**：零初始化的 `ejit_config_t` 会得到 `enableLogger = false`，需显式置 `true` 才会记录错误：

```c
ejit_config_t cfg = {};
cfg.enableLogger = true;   // 必须显式设置；零初始化结构体为 false
ejit_init(&cfg);
```

```c
if (ejit_taskpool_compile_or_get(...) != EJIT_OK) {
  const ejit_error_t *e = ejit_get_last_error();
  if (e) printf("EJIT error %d: %s (%s)\n", e->code, e->message, e->funcName);
}
```

---

## 7. 缓存失效

```c
void ejit_clear_cache(void);
void ejit_invalidate(const char *periodName, uint8_t cellIdx);
```

- `ejit_clear_cache`：清空整个 JIT cache，强制后续调用全部重新编译。用于验证可重现性或释放代码内存。
- `ejit_invalidate`：仅失效指定 period / cell 实例的缓存条目，用于定向重编译。

---

## 8. JITLink 分支重定位诊断（AArch64）

**作用**：诊断 AArch64 / aarch64_be 上 JIT 代码的分支重定位是否落在 `B/BL` 的 ±128MiB 直跳范围内，超范围时是否被 stub 化（`ADRP x16; LDR x16,[x16]; BR x16` 经 `$__GOT` 间接跳），以及 stub 化的边能否被松弛回直跳。仅 AArch64 目标生效。

**启用**：需运行库启用诊断日志 + 运行时日志级别 ≥ `EJIT_LOG_INFO`（默认即 INFO）。由 JITLink 诊断 / 优化插件在每次链接 AArch64 graph 后输出。

**INFO 级输出**（每个链接的 graph，带 `[EJIT]` 前缀）：

- **relax 汇总**：`relaxAArch64BranchStubs: graph=<g> Branch26PCRel: <n> total, <n> stubbed (chain-mismatch=<n> unresolved=<n> out-of-range=<n>), <n> relaxed` -- stub 化边的跳过原因分解。
- **`[STUBBED]` 审计**：每条被 stub 化的分支重定位一行（带两空格缩进），`  [STUBBED] <bl/b/...> @0x<addr> -> stub@0x<addr> (ADRP x16; LDR x16,[x16]; BR x16) -> $__GOT -> <target> @0x<addr> | direct dist=<n> (<EXCEEDS/within> +-128MB)`。
- **linkdiag 汇总**：`linkdiag: graph=<g> summary: <n> stubbed (<n> exceed +-128MB), <n> direct`。

**VERBOSE 级**追加：graph 头 `linkdiag: graph=<g> triple=<triple> -- branch relocation audit`，以及逐条直跳重定位（带两空格缩进）`  [direct ] <reloc> @0x<addr> -> <target> @0x<addr> | dist=<n>`。

用于定位“为何某 call 没被优化成直跳 / 为何走了 stub / stub 是否被松弛”。

---

## 9. 编译 ejit 代码时的诊断

以下诊断在你用 `clang -fembed-bitcode` 编译标注了 ejit 属性的源码时发出，走 `stderr` 或 clang 诊断通道，经 `-mllvm` 标志或 `-W` 诊断组控制。**不受运行时日志级别影响**--它们在编译期即完成。lit 测试用 `2>&1` 捕获（输出在 stderr，不在 IR `.ll` 里）。

### 9.1 Sema 诊断（`-Wembedded-jit`）

clang Sema 阶段发出以下告警，归入诊断组 `embedded-jit`（即 `-Wembedded-jit`，可用 `-Wno-embedded-jit` 整组关闭）：

| 告警 | 触发条件 | 文本 |
|------|----------|------|
| `warn_ejit_always_inline_conflict` | 对 `ejit_entry` / `ejit_period_lc` 函数标注 `always_inline`。这些函数必须保持非内联才能在内联器中存活供 JIT 特化，用户 `always_inline` 与之冲突，Sema 告警并忽略 `always_inline`。 | `'always_inline' is incompatible with %0, which must remain out-of-line so it survives the inliner for JIT specialization; ignoring 'always_inline'`（`%0` = `ejit_entry` 或 `ejit_period_lc`） |
| `warn_ejit_may_const_modified_without_lc` | 在**未**标注 `ejit_period_lc` 的函数中写 `ejit_may_const` 字段。`may_const` 字段只允许在时间窗切换（lifecycle）时变更，普通函数写入会破坏特化一致性。 | `modifying ejit_may_const field %0 of %1 without ejit_period_lc attribute`（`%0` = 字段名，`%1` = 持有该结构体的变量或其父记录） |

### 9.2 特化价值诊断

由 EJIT 的 AOT 位码提取 pass（PASS1）在提取位码上运行 `runSpecializationDiagnostic` 发出，经 `-mllvm` 标志控制。

**特化闭包**（诊断推理的范围）：`ejit_entry` 自身 + 经**直接调用**可达的全部已定义非 intrinsic 函数（不动点传播）。**外部声明调用与间接调用（函数指针）不计入**--JIT 无法内联它们，故其 `may_const` 读不会进入该 entry 的特化。这使 #1 是 sound 的（无误报）。

| 标志 | 默认 | 类别 | 触发条件 |
|------|------|------|----------|
| `-mllvm -ejit-warn-no-specialization` | **on** | #1 告警 | entry 的特化闭包内**无任何** `!ejit.may_const` load（特化无收益，考虑移除 `ejit_entry`） |
| `-mllvm -ejit-warn-unused-dim` | **on** | #2 告警 | 声明了 `ejit_period_arr_ind("P")` 但闭包**从不索引** `ejit_period_arr("P")`（死维度） |
| `-mllvm -ejit-report-mayconst` | **off** | info 报告 | 报告每个 entry 闭包内 `may_const` 读总数 K 及其中在循环内的 J |
| `-mllvm -ejit-warn-few-mayconst=<N>` | `0`（off） | opt-in 告警 | entry 闭包内 `may_const` 读数 **< N** 时告警（如 `=4` 告警 0..3 读）。默认关闭 |

**输出文本**（走 `stderr`）：

```
#1    EJit warning: ejit_entry function '<name>' reads no ejit_may_const field in its specialization closure; no JIT specialization value, consider removing ejit_entry
#2    EJit warning: ejit_entry function '<name>' declares ejit_period_arr_ind('<P>') but its specialization closure never indexes an ejit_period_arr('<P>'); unused specialization dimension, consider removing it
info  EJit info: ejit_entry function '<name>': <K> ejit_may_const read[s] (<J> in loops)   # K==1 时 read 无 s
few   EJit warning: ejit_entry function '<name>' has only <K> ejit_may_const read[s] in its specialization closure (threshold: <N>); low specialization surface, consider adding more may-const fields   # K==1 时 read 无 s
```

- **关闭默认 on 的告警**：`-mllvm -ejit-warn-no-specialization=false`（同理 `-ejit-warn-unused-dim=false`）。
- **info 报告**仅报告、不 gating。理由：`may_const` 个数是特化价值的 poor proxy--单次 `may_const` 读若在热循环或喂给分支即为高收益，故唯一 sound 的默认阈值是 0（即 #1），N>0 会误报最高价值场景；判断权留给用户。
- **`-ejit-warn-few-mayconst=N`** 是 opt-in 的个数阈值告警，默认关闭。它不与 #1 冲突：#1（零=确定无价值）是默认 sound 基线，本标志供想对“特化面较窄”做更严格审查的用户按需开启（低计数只表示可折入的少，是否真的配置不当取决于这些读 gate 了什么，需人工判断）。

**PASS5 对称检查（始终开启，无标志）**：AOT 后期 pass 在 entry 函数体**引用**了 `ejit_period_arr("P")` 但**未声明** `ejit_period_arr_ind("P")` 时告警，与 #2（声明但不用）反向对称：

```
EJit warning: function '<name>' references ejit_period_arr '<P>' but it is not declared via ejit_period_arr_ind
```

> #2 在**特化闭包**上传播（含被调函数）；PASS5 只看 **entry 自身函数体**。两者方向相反、互补。

### 9.3 提取位码转储

```bash
clang -fembed-bitcode ... -mllvm -ejit-dump-bitcode-dir=/tmp/ejit_bc process.c
```

在编译期把每个 TU 提取出的 EmbeddedJIT 位码（`.bc` + `.ll`）落盘到指定目录，文件名含 PID + 模块名，便于并行 `-j` 构建不冲突。用于调试符号提取 / 闭包计算结果。默认空（关闭）。

---

## 10. 常见问题排查

### 10.1 首次上板（bring-up）

1. `ejit_init()` 后调用 `ejit_print_version()` 记录运行库构建标识，与预期源码版本对齐。
2. `ejit_print_registry()` 确认 AOT 注册表正确（bitcode / period 数组 / 静态变量都到位）。
3. 默认 `EJIT_LOG_INFO` 观察 init / 编译 / cache MISS 等关键事件；必要时 `ejit_set_log_level(EJIT_LOG_VERBOSE)`。

### 10.2 某个 `ejit_entry` 没收益 / 没被特化

1. 编译期先看 §9.2：#1 告警（闭包无 `may_const`）说明该 entry 特化无收益，应移除 `ejit_entry` 或补 `may_const` 标注；`-mllvm -ejit-report-mayconst` 看 `may_const` 读总数及循环内占比，评估收益。
2. 运行时 `ejit_print_func_meta(name)` 确认特化参数绑定与 `may_const` 资格。
3. `ejit_print_active()` 确认相关 period 实例是否处于激活态。

### 10.3 该编译的没编译 / 编译失败

1. `ejit_get_last_error()` 读失败原因（code / message / funcName）。
2. `EJIT_LOG_VERBOSE` 看逐次 `compile_or_get` 与 taskpool 请求。
3. `ejit_taskpool_get_stats()` 看 `compileFailed` / `publishFailed` / `queueFull` / `queueApproxSize`，判断是失败、被拒还是拥塞。
4. `ejit_taskpool_print_compiled()` 看实际编译了哪些特化。

### 10.4 命中率低 / 性能差

1. `ejit_taskpool_get_stats()` 读 `cacheHits` / `asyncCompiles` / `alreadyPending`。
2. 若 wrapper 开销可疑，对 ejit 代码加 `-mllvm -ejit-wrapper-timing`，观察周期汇总中查找 / 调用 / 释放各段耗时。
3. `ejit_get_stats()` 看旧版 LRU 的命中 / 淘汰。

### 10.5 代码内存趋紧 / 耗尽

1. `ejit_get_code_pool_stats()` 看 `usedBytes` vs `reservedBytes`、`wastedBytes`、`poolCount`。
2. 固定代码段模式下观察 §2.3 的 `enableRwRange` 日志是否有 `FAIL` / `rollback`。
3. 必要时 `ejit_clear_cache()` 释放代码内存（会强制重编译）。

### 10.6 AArch64 分支超范围

`EJIT_LOG_INFO` 观察 §8 的 relax / `[STUBBED]` / linkdiag 汇总：`out-of-range` / `exceed +-128MB` 计数非零即存在超范围分支走了 stub（间接跳），影响性能。VERBOSE 可看每条直跳 / stub 的距离。

### 10.7 想看某函数特化结果

`ejit_dump_func(name)` 捕获下一次特化，`ejit_print_dumped(name)` 回读 IR+ASM，对照预期。

---

## 附录：状态码

```c
EJIT_OK                  =  0
EJIT_PENDING             =  1
EJIT_ERR_INVALID_PARAM   = -1
EJIT_ERR_NOT_ACTIVE      = -2
EJIT_ERR_COMPILE_FAILED  = -3
EJIT_ERR_CACHE_FULL      = -4
EJIT_ERR_MEMORY          = -5
EJIT_ERR_BITCODE_NOT_FOUND = -6
EJIT_ERR_QUEUE_FULL      = -7
EJIT_ERR_DEDUP_FULL      = -8
EJIT_ERR_DISABLED        = -9
EJIT_ERR_INSTANCE_DISABLED = -10
```

> 所有运行时诊断 API 的头文件：`llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h`（公共 C ABI）。编译期诊断由 clang Sema 与 EJIT AOT pass 实现。
