# EJIT 诊断功能说明

> 基于 `djq/ejit_dev_spec4` 分支。面向 EJIT 集成方与现场调测人员，仅说明各诊断命令 / API 的**作用**与**使用方法**。

EJIT 的诊断能力按“何时生效”分为三类：

- **构建期开关**（CMake / `build.sh`）：编译运行库时决定哪些诊断代码被编入，决定生产构建能否做到零开销。
- **AOT 编译期诊断**（`clang -mllvm` / `-W`）：编译含 `ejit_entry` 的用户代码（`-fembed-bitcode`）时，由 AOT pass / clang Sema 发出，经 `stderr` 或 clang 诊断通道输出。**与运行时日志级别、`EJIT_DIAG_ENABLE` 相互独立**——它们在 AOT 编译期就已完成，不受运行时设置影响。
- **运行时诊断**（C API + 日志级别）：程序运行过程中调用，动态调整日志级别、读取统计、转储 IR/ASM、内省注册表等。

运行时诊断输出走平台日志通道：裸金属 / SRE 构建走平台提供的 `SRE_printf()`，宿主构建走 `std::printf`；AOT 编译期诊断走 `stderr`（pass 的 `errs()`）或 clang 诊断（`-W`）。诊断代码全部位于冷路径（注册、编译、诊断调用），且默认可在编译期完全裁掉，因此生产构建可做到零开销。

---

## 1. 构建期诊断开关

| 开关 | 默认 | 作用 | 启用方式 |
|------|------|------|----------|
| `EJIT_DIAG_ENABLE` | `OFF` | 打开运行时诊断日志（`EJIT_DIAG*` 宏）。关闭时所有日志宏展开为空，零开销。 | CMake：`-DEJIT_DIAG_ENABLE=ON` |
| `EJIT_SRE_DIAG` | `OFF` | 诊断输出改走平台 `SRE_printf()`（而非 `std::printf`）。裸金属 / SRE 目标必开。 | CMake：`-DEJIT_SRE_DIAG=ON` |
| `EJIT_STATS_ENABLE` | `OFF` | 嵌入 taskpool 逐调用统计计数器（cache 命中等）。关闭时计数自增被裁掉，`ejit_taskpool_get_stats()` 报全零；**热点路径有跨核原子开销**，仅调测打开。 | `build.sh --stats` 或 `-DEJIT_STATS_ENABLE=ON` |
| `EJIT_DUMP_ASM` | `OFF` | 在启用后端裁剪（`EJIT_TRIM_LLVM_BACKEND`）时，重新打开文本汇编发射，使运行时 IR/ASM 转储能产出汇编。 | CMake：`-DEJIT_DUMP_ASM=ON` |
| `EJIT_WRAPPER_TIMING_REPORT_EVERY` | `100000` | AOT wrapper 计时的周期性汇总打印间隔（每 N 次调用打印一行汇总）。设为 `0` 关闭周期输出。 | 编译期宏：`-DEJIT_WRAPPER_TIMING_REPORT_EVERY=10000` |

> 代码池另有 `EJIT_SRE_CODE_POOL`（代码池总开关，§4.3 统计的前提）、`EJIT_FIXED_CODE_POOL`（固定段模式，额外产出 W^X 转换日志，见 §4.3）、`EJIT_CODE_POOL_4K_SEAL`（4K 粒度封固，影响计数粒度）三个**功能开关**也会门控诊断输出，详见 `EJIT_SRE_CODE_POOL.md`。
>
> **一键全开**：CMake 预设 `ejit-minimal-aarch64_be` 已将 `EJIT_DIAG_ENABLE`、`EJIT_SRE_DIAG`、`EJIT_DUMP_ASM`、`EJIT_STATS_ENABLE` 全部置 `ON`，适合带板调测。
>
> ```bash
> cmake --preset ejit-minimal-aarch64_be
> ```

---

## 2. AOT 编译期诊断

本节诊断在 `clang -fembed-bitcode` 编译用户代码时由 AOT pass / Sema 发出，走 `stderr`（`errs()`）或 clang 诊断通道，经 `-mllvm` 标志或 `-W` 诊断组控制。**不受 `EJIT_DIAG_ENABLE`、运行时日志级别影响**——它们在 AOT 编译期即完成。lit 测试用 `2>&1` 捕获（输出在 stderr，不在 IR `.ll` 里）。

### 2.1 Sema 诊断（`-Wembedded-jit`）

EJIT 在 clang Sema 阶段发出以下告警，归入诊断组 `embedded-jit`（即 `-Wembedded-jit`，可用 `-Wno-embedded-jit` 整组关闭）：

| 告警 | 触发条件 | 文本 |
|------|----------|------|
| `warn_ejit_always_inline_conflict` | 对 `ejit_entry` / `ejit_period_lc` 函数标注 `always_inline`。这些函数必须保持非内联才能在内联器中存活供 JIT 特化（CodeGen/PASS3 会强制加 `noinline`），用户 `always_inline` 与之冲突，Sema 告警并忽略 `always_inline`。 | `'always_inline' is incompatible with %0, which must remain out-of-line so it survives the inliner for JIT specialization; ignoring 'always_inline'`（`%0` = `ejit_entry` 或 `ejit_period_lc`） |
| `warn_ejit_may_const_modified_without_lc` | 在**未**标注 `ejit_period_lc` 的函数中写 `ejit_may_const` 字段。`may_const` 字段只允许在时间窗切换（lifecycle）时变更，普通函数写入会破坏特化一致性。 | `modifying ejit_may_const field %0 of %1 without ejit_period_lc attribute`（`%0` = 字段名，`%1` = 持有该结构体的变量或其父记录） |

> 历史的 `-ejit-noinline-entry` 标志已移除：noinline 现由 Sema/CodeGen 在 `ejit_entry` / `ejit_period_lc` 上自动强制，无需也不再接受用户开关。

### 2.2 PASS1 特化价值诊断

由 `EJitRegisterBitcodePass`（PASS1）在提取位码（`preOptimizeBitcode` 之后、全局 extern 化之前）上运行 `runSpecializationDiagnostic` 发出，经 `-mllvm` 标志控制。

**特化闭包**（诊断推理的范围）：`ejit_entry` 自身 + 经**直接调用**可达的全部已定义非 intrinsic 函数（不动点传播）。**外部声明调用与间接调用（函数指针）不计入**——JIT 无法内联它们，故其 `may_const` 读不会进入该 entry 的特化。这使 #1 是 sound 的（无误报）。

| 标志 | 默认 | 类别 | 触发条件 |
|------|------|------|----------|
| `-mllvm -ejit-warn-no-specialization` | **on** | #1 告警 | entry 的特化闭包内**无任何** `!ejit.may_const` load（特化无收益，考虑移除 `ejit_entry`） |
| `-mllvm -ejit-warn-unused-dim` | **on** | #2 告警 | 声明了 `ejit_period_arr_ind("P")` 但闭包**从不索引** `ejit_period_arr("P")`（死维度） |
| `-mllvm -ejit-report-mayconst` | **off** | info 报告 | 报告每个 entry 闭包内 `may_const` 读总数 K 及其中在循环内的 J |

**输出文本**（走 `errs()`）：

```
#1  EJit warning: ejit_entry function '<name>' reads no ejit_may_const field in its specialization closure; no JIT specialization value, consider removing ejit_entry
#2  EJit warning: ejit_entry function '<name>' declares ejit_period_arr_ind('<P>') but its specialization closure never indexes an ejit_period_arr('<P>'); unused specialization dimension, consider removing it
info EJit info: ejit_entry function '<name>': <K> ejit_may_const read[s] (<J> in loops)   # K==1 时 read 无 s
```

- **关闭默认 on 的告警**：`-mllvm -ejit-warn-no-specialization=false`（同理 `-ejit-warn-unused-dim=false`）。
- **info 报告**仅报告、不 gating。理由：`may_const` 个数是特化价值的 poor proxy——单次 `may_const` 读若在热循环或喂给分支即为高收益，故唯一 sound 的阈值是 0（即 #1），N>0 会误报最高价值场景；判断权留给用户。
- **循环检测**用轻量 CFG 自环判定（`isOnCfgCycle`：BB 能否经非空路径到达自己），**不依赖 `LoopInfo`/`PassBuilder`**，以避免给 `LLVMEmbeddedJIT` 共享库引入 `Passes`/`Analysis` 链接依赖。对自然循环与 `LoopInfo` 一致，且接受不可归约循环，对 info 信号足够。
- **相关 lit 测试**：`llvm/test/Transforms/EmbeddedJIT/ejit-diag-no-specialization.ll`、`ejit-diag-unused-dim.ll`、`ejit-diag-report-mayconst.ll`。

**PASS5 对称检查（始终开启，无标志）**：`EJitAotModulePass::runDiagnosticCheck` 在 entry 函数体**引用**了 `ejit_period_arr("P")` 但**未声明** `ejit_period_arr_ind("P")` 时告警，与 #2（声明但不用）反向对称：

```
EJit warning: function '<name>' references ejit_period_arr '<P>' but it is not declared via ejit_period_arr_ind
```

> #2 与 PASS5 的差异：#2 在**特化闭包**上传播（fixpoint，含被调函数），用 `rootGlobal` 走 GEP/bitcast 链解析全局；PASS5 只看 **entry 自身函数体**，用 `stripPointerCasts()` 解析。两者方向相反、互补。相关 lit 测试：`llvm/test/Transforms/EmbeddedJIT/ejit-aot-module-diagnostic.ll`。

### 2.3 提取位码转储

```bash
clang -fembed-bitcode ... -mllvm -ejit-dump-bitcode-dir=/tmp/ejit_bc process.c
```

**作用**：在 AOT 编译期把每个 TU 提取出的 EmbeddedJIT 位码（`.bc` + `.ll`）落盘到指定目录，文件名含 PID + 模块名，便于并行 `-j` 构建不冲突。用于调试符号提取 / 闭包计算结果。默认空（关闭）。

---

## 3. 运行时日志级别

运行时日志分四级，可在不重新编译的情况下动态调整。**前提**：构建时已打开 `EJIT_DIAG_ENABLE`（见 §1）。

| 级别 | 值 | 作用 |
|------|----|------|
| `EJIT_LOG_OFF` | 0 | 不输出 |
| `EJIT_LOG_INFO` | 1 | 关键事件（默认）：init/shutdown、编译 begin/OK/FAIL、cache MISS、激活、错误、注册消费摘要 |
| `EJIT_LOG_VERBOSE` | 2 | 逐项细节：每次首次注册、逐函数 struct-field 统计、逐次 `compile_or_get`、taskpool 请求 |
| `EJIT_LOG_DEBUG` | 3 | 内部机理：幂等注册跳过、逐 load 替换失败、staging 内部、funcMeta 缓存 |

### API

```c
void ejit_set_log_level(ejit_log_level_t level);   // 立即生效，影响后续所有 EJIT_DIAG* 输出
ejit_log_level_t ejit_get_log_level(void);          // 查询当前级别
```

### 使用方法

```c
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

// 生产环境降低日志量
ejit_set_log_level(EJIT_LOG_INFO);

// 排查问题时提升到细节级
ejit_set_log_level(EJIT_LOG_VERBOSE);
```

---

## 4. 统计信息

### 4.1 旧版 LRU cache 统计

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

**作用**：查询旧版 LRU `EJitCache` 的命中 / 未命中 / 淘汰与内存占用。无构建开关要求，始终可用。

### 4.2 SRE taskpool 统计

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
void          ejit_taskpool_print_stats(void);   // 人可读形式经平台日志输出
void          ejit_taskpool_print_compiled(void);// 列出所有已编译特化（funcIdx/name/dims/fn）
uint32_t      ejit_taskpool_get_worker_core(void);// 返回 worker 运行所在核
unsigned      ejit_taskpool_pending_count(void); // 在途数量
```

**作用**：描述 taskpool 的 cache / 去重 / 队列流水线运行状况，定位“为何没编译 / 为何编译慢 / 队列是否拥塞”。

> **注意**：逐调用计数器（`cacheHits` 等）需要构建期打开 `EJIT_STATS_ENABLE`（见 §1），否则 `get_stats()` 返回全零；`print_compiled()`、`get_worker_core()`、`pending_count()` 不依赖该开关。

### 4.3 代码池统计

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

**作用**：监控嵌入式代码内存的占用与耗尽情况。返回值：`EJIT_OK` 成功；`EJIT_ERR_NOT_ACTIVE` 运行时未初始化；`EJIT_ERR_INVALID_PARAM` 入参为空；`EJIT_ERR_DISABLED` 构建未含 `EJIT_SRE_CODE_POOL`。代码池统计为冷路径诊断，**始终编译**，不受 `EJIT_STATS_ENABLE` 影响。

> **固定代码池模式**（`EJIT_SRE_CODE_POOL=ON` + `EJIT_FIXED_CODE_POOL=ON`，详见 `EJIT_SRE_CODE_POOL.md`）：代码池改用链接脚本固定区域 `[__ejit_code_start, __ejit_code_end)`（`.text.ejit` 代码段），给 JIT 稳定地址（±128MiB 内可直 `bl`/`adrp` 到 AOT）。每个 slab 写前 `enable_rw`（RX->RW）、finalize `enable_ex`（RW->RX）。此时：
> - 每次页状态转换经 `EJIT_DIAG` 在 **INFO 级**打印 `enableRwRange` 系列日志（`begin` / `OK` / `FAIL`（未归属 / 跨池 / `enable_rw` 失败）/ `rollback`（部分失败时回退 RW->RX）），用于诊断 W^X 页状态转换。成功 `enable_rw` 次数仅在内部 `Stats` 累计，不暴露进上面的公共 `ejit_code_pool_stats_t`。
>
> `EJIT_CODE_POOL_4K_SEAL`（默认 ON）决定封固粒度，影响 `sealInvocations` / `splitInvocations` 的计数粒度。

---

## 5. JIT IR / ASM 转储

提供两种互补的转储方式：**文件式**（一次性、全量、便于离线分析）与**内存捕获式**（运行时按名过滤、经平台日志回读）。

### 5.1 文件式 IR 转储（`dumpJITDir`）

**作用**：把每个特化的 JIT 优化后 LLVM IR（`.ll`）落盘，文件名 `<funcName>_<cacheKey>.ll`，便于离线 diff / 审查。

通过 `ejit_init()` 的配置项启用：

```c
ejit_config_t cfg = {};
cfg.compileMode  = EJIT_COMPILE_ASYNC;
cfg.optLevel     = EJIT_OPT_L2;
cfg.enableLogger = true;
cfg.dumpJITDir   = "/tmp/ejit_ir";   // 非空即开启
ejit_init(&cfg);
```

### 5.2 内存捕获式 IR+ASM 转储

```c
void ejit_dump_func(const char *name);  // 按名开启捕获；name="*" 捕获全部；NULL/"" 关闭后续捕获
void ejit_dump_all(bool enable);        // 等价于 ejit_dump_func("*")（enable=true 时）
void ejit_print_dumped(const char *name);// 经平台日志打印已保存的 IR+ASM；NULL/"" 打印本核全部
```

**作用**：运行时按函数名过滤捕获下一次 JIT 编译产生的**优化后 IR 与汇编**，再经平台日志逐行回读。适合现场定位某个函数的特化结果。

> - 捕获为**精确名匹配**，`"*"` 例外（捕获全部）。
> - 完整 IR/ASM 负载保留在 worker 核本地，不拷入共享 taskpool 内存。
> - 要在目标上拿到汇编，需构建期打开 `EJIT_DUMP_ASM`（见 §1）。

### 使用方法

```c
// 只捕获感兴趣函数的下一次特化
ejit_dump_func("process_cell");
// ... 触发该函数的 JIT 编译 ...
ejit_print_dumped("process_cell");   // 经平台日志回读 IR+ASM

// 捕获所有特化
ejit_dump_all(true);
```

---

## 6. 注册表与元数据内省

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
| `ejit_print_version` | 打印运行库构建标识：LLVM 发行版本号（major.minor.patch）与 llvm-project 源码的 git commit + 分支。commit 在构建期捕获，即使增量重建也跟踪源码 HEAD。**无需初始化运行时、不受日志级别门控**，便于将现场设备行为与确切源码版本对应。 |

### 使用方法

```c
ejit_init(&cfg);
ejit_print_registry();                // 确认注册表
ejit_print_func_meta("process_cell"); // 查看某函数的特化元数据
ejit_print_active();                  // 查看当前激活实例
ejit_print_version();                 // 可在 init 前后任意时刻调用
```

---

## 7. Wrapper 计时

**作用**：在 AOT 生成的 `ejit_entry` wrapper 中插入计时探针，测量 taskpool 查找、间接 JIT 调用、读令牌释放各段耗时，定位 wrapper 开销。运行时按 `EJIT_WRAPPER_TIMING_REPORT_EVERY`（默认 100000）次调用聚合打印一行汇总，避免日志刷屏。

### 启用方式

计时探针由 AOT 端 LLVM pass 选项控制，编译含 `ejit_entry` 的源码时传入：

```bash
clang -fembed-bitcode ... -mllvm -ejit-wrapper-timing process.c
```

### 运行时聚合 API（由插桩 wrapper 自动调用，一般不手动调用）

```c
uint64_t ejit_taskpool_trace_now(void);   // 取当前时间戳（SRE 用 SRE_CycleCountGet64，宿主用 steady_clock 纳秒）
void ejit_taskpool_trace_wrapper(uint32_t funcIndex, uint32_t status,
                                 void *fnPtr, uint32_t bucketIndex,
                                 uint64_t tBeforeLookup,
                                 uint64_t tAfterLookup,
                                 uint64_t tAfterFn,
                                 uint64_t tAfterRelease);
```

> 仅当 wrapper 以 `-mllvm -ejit-wrapper-timing` 构建时才会调用上述 API；运行时负责聚合与周期打印。时间戳单位由平台决定。

---

## 8. 错误报告

```c
typedef struct {
  int  code;
  char message[256];
  char funcName[128];
} ejit_error_t;

const ejit_error_t *ejit_get_last_error(void);
```

**作用**：返回最近一次错误的指针（code / message / funcName），底层为预分配环形缓冲（最多 32 条），日志时刻无动态分配。

配套配置项 `enableLogger`（`ejit_config_t` 的 `bool` 字段）控制错误日志器是否启用。注意该 C 结构体字段**无默认值**：零初始化的 `ejit_config_t` 会得到 `enableLogger = false`，需显式置 `true` 才会记录错误：

```c
ejit_config_t cfg = {};
cfg.enableLogger = true;   // 必须显式设置；零初始化结构体为 false
ejit_init(&cfg);
```

### 使用方法

```c
if (ejit_taskpool_compile_or_get(...) != EJIT_OK) {
  const ejit_error_t *e = ejit_get_last_error();
  if (e) printf("EJIT error %d: %s (%s)\n", e->code, e->message, e->funcName);
}
```

---

## 9. 缓存诊断

```c
void ejit_clear_cache(void);
void ejit_invalidate(const char *periodName, uint8_t cellIdx);
```

**作用**：

- `ejit_clear_cache`：清空整个 JIT cache，强制后续调用全部重新编译。用于验证可重现性或释放代码内存。
- `ejit_invalidate`：仅失效指定 period / cell 实例的缓存条目，用于定向重编译。

---

## 10. JITLink 分支重定位诊断（AArch64）

**作用**：诊断 AArch64 / aarch64_be 上 JIT 代码的分支重定位是否落在 `B/BL` 的 ±128MiB 直跳范围内，超范围时是否被 stub 化（`ADRP x16; LDR x16,[x16]; BR x16` 经 `$__GOT` 间接跳），以及 stub 化的边能否被松弛回直跳。仅 AArch64 目标生效。

**启用**：构建期 `EJIT_DIAG_ENABLE=ON`（见 §1）+ 运行时日志级别 ≥ `EJIT_LOG_INFO`（默认即 INFO）。由 `EJitLinkDiagPlugin` / `EJitLinkOptimizationPlugin`（挂载在 `ObjectLinkingLayer` 上）在每次链接 AArch64 graph 后输出。诊断审计体（`EJitLinkDiagPlugin`）`#ifdef EJIT_DIAG_ENABLE` 门控，关闭时为空操作；松弛 pass（`EJitLinkOptimizationPlugin`）始终运行，但其汇总日志同样经 `EJIT_DIAG` 宏，在 `EJIT_DIAG_ENABLE` 关闭时为空。

**INFO 级输出**（每个链接的 graph，经 `EJIT_DIAG` 宏带 `[EJIT]` 前缀）：

- **relax 汇总**：`relaxAArch64BranchStubs: graph=<g> Branch26PCRel: <n> total, <n> stubbed (chain-mismatch=<n> unresolved=<n> out-of-range=<n>), <n> relaxed` —— stub 化边的跳过原因分解。
- **`[STUBBED]` 审计**：每条被 stub 化的分支重定位一行（带两空格缩进），`  [STUBBED] <bl/b/...> @0x<addr> -> stub@0x<addr> (ADRP x16; LDR x16,[x16]; BR x16) -> $__GOT -> <target> @0x<addr> | direct dist=<n> (<EXCEEDS/within> +-128MB)`。
- **linkdiag 汇总**：`linkdiag: graph=<g> summary: <n> stubbed (<n> exceed +-128MB), <n> direct`。

**VERBOSE 级**追加：graph 头 `linkdiag: graph=<g> triple=<triple> -- branch relocation audit`，以及逐条直跳重定位（带两空格缩进）`  [direct ] <reloc> @0x<addr> -> <target> @0x<addr> | dist=<n>`。

用于定位“为何某 call 没被优化成直跳 / 为何走了 stub / stub 是否被松弛”。

---

## 11. 常用诊断流程

### 11.1 AOT 编译期：特化价值审查

1. 正常 `clang -fembed-bitcode` 编译。默认即可看到 #1 / #2 告警（闭包无 `may_const` / 死维度）与 PASS5 的引用未声明告警。
2. 评估特化收益：`-mllvm -ejit-report-mayconst` 查看每个 entry 闭包内 `may_const` 读总数及循环内占比。
3. 排查符号提取：`-mllvm -ejit-dump-bitcode-dir=/tmp/ejit_bc` 落盘提取位码，对照闭包预期。
4. `-Wembedded-jit` 确认无 `always_inline` 冲突、无非法 `may_const` 写。

### 11.2 带板首次调测（bring-up）

1. 用 `ejit-minimal-aarch64_be` 预设构建（诊断全开），或 `build.sh --stats`。
2. `ejit_init()` 后调用 `ejit_print_registry()` 确认 AOT 注册表正确。
3. `ejit_print_version()` 记录构建标识，与源码版本对齐。
4. 默认 `EJIT_LOG_INFO` 即可观察 init / 编译 / cache MISS 等关键事件；必要时 `ejit_set_log_level(EJIT_LOG_VERBOSE)`。

### 11.3 性能 / 命中率诊断

1. 构建 `--stats`，运行后 `ejit_taskpool_get_stats()` 读取 `cacheHits` / `asyncCompiles` / `alreadyPending` / `queueFull` 等。
2. `ejit_taskpool_print_compiled()` 查看实际编译了哪些特化。
3. 若 wrapper 开销可疑，对 AOT 端加 `-mllvm -ejit-wrapper-timing`，观察周期汇总中查找 / 调用 / 释放各段耗时。
4. `ejit_get_code_pool_stats()` 监控代码内存是否趋近耗尽（`usedBytes` vs `reservedBytes`）。
5. AArch64 上若怀疑分支超范围，`EJIT_LOG_INFO` 观察 §10 的 relax / `[STUBBED]` / linkdiag 汇总。

### 11.4 现场问题定位

1. `ejit_set_log_level(EJIT_LOG_DEBUG)` 恢复完整细节。
2. `ejit_print_active()` 确认相关 period 实例是否处于激活态。
3. `ejit_print_func_meta(name)` 确认特化参数绑定与 `may_const` 资格。
4. `ejit_dump_func(name)` 捕获并 `ejit_print_dumped(name)` 回读该函数的 IR+ASM，对照预期。
5. `ejit_get_last_error()` 读取失败原因。

---

## 附录 A：状态码

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

## 附录 B：构建期与编译期诊断开关速查

### B.1 构建期（CMake）开关

| 开关 | 关闭时影响 | 推荐场景 |
|------|-----------|----------|
| `EJIT_DIAG_ENABLE` | 所有运行时日志静默 | 调测 ON，生产 OFF |
| `EJIT_SRE_DIAG` | 日志走 `std::printf` | 裸金属 / SRE 目标必开 |
| `EJIT_STATS_ENABLE` | taskpool 逐调用计数报零 | 调测 ON（有热点原子开销），生产 OFF |
| `EJIT_DUMP_ASM` | IR/ASM 转储无汇编 | 需要汇编转储时 ON |
| `EJIT_WRAPPER_TIMING_REPORT_EVERY` | wrapper 计时周期汇总间隔（默认 100000，0 关闭） | 调整汇总频率 |
| `EJIT_SRE_CODE_POOL` | 代码池统计返回 `EJIT_ERR_DISABLED` | 用代码池时 ON |
| `EJIT_FIXED_CODE_POOL` | 无固定段 W^X 转换日志（`enableRwRange`） | 需稳定 JIT 地址 + W^X 诊断时 ON |
| `EJIT_CODE_POOL_4K_SEAL` | 回退整 2MiB 池封固，计数粒度变粗 | 默认 ON（4K 粒度） |

### B.2 AOT 编译期标志（`-mllvm` / `-W`，编译含 `ejit_entry` 的源码时传入）

| 标志 | 默认 | 作用 |
|------|------|------|
| `-Wembedded-jit` | on | Sema 告警组：`always_inline` 冲突、非法 `may_const` 写（§2.1）。`-Wno-embedded-jit` 关闭 |
| `-mllvm -ejit-warn-no-specialization` | on | #1 闭包无 `may_const` 告警（§2.2） |
| `-mllvm -ejit-warn-unused-dim` | on | #2 死维度告警（§2.2） |
| `-mllvm -ejit-report-mayconst` | off | `may_const` 读计数 info 报告（§2.2） |
| `-mllvm -ejit-dump-bitcode-dir=<dir>` | off | AOT 期提取位码转储（§2.3） |
| `-mllvm -ejit-wrapper-timing` | off | wrapper 计时探针（§7） |

> 所有运行时诊断 API 的头文件：`llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h`（公共 C ABI）。AOT 编译期诊断分别由 clang Sema（`clang/lib/Sema/SemaEJIT.cpp`）与 PASS1/PASS5（`llvm/lib/Transforms/EmbeddedJIT/`）实现。
