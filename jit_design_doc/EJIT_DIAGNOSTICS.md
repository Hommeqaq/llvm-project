# EJIT 诊断功能说明

> 基于 `djq/ejit_dev_spec4` 分支。面向 EJIT 集成方与现场调测人员，仅说明各诊断命令 / API 的**作用**与**使用方法**。

EJIT 的诊断能力分为两层：

- **构建期开关**：通过 CMake / `build.sh` 在编译时打开，决定哪些诊断代码被编入运行库。
- **运行时 C API**：在程序运行过程中调用，动态调整日志级别、读取统计、转储 IR/ASM、内省注册表等。

所有诊断输出都走平台日志通道：裸金属 / SRE 构建走平台提供的 `SRE_printf()`，宿主构建走 `std::printf`。诊断代码全部位于冷路径（注册、编译、诊断调用），且默认可在编译期完全裁掉，因此生产构建可做到零开销。

---

## 1. 构建期诊断开关

| 开关 | 默认 | 作用 | 启用方式 |
|------|------|------|----------|
| `EJIT_DIAG_ENABLE` | `OFF` | 打开运行时诊断日志（`EJIT_DIAG*` 宏）。关闭时所有日志宏展开为空，零开销。 | CMake：`-DEJIT_DIAG_ENABLE=ON` |
| `EJIT_SRE_DIAG` | `OFF` | 诊断输出改走平台 `SRE_printf()`（而非 `std::printf`）。裸金属 / SRE 目标必开。 | CMake：`-DEJIT_SRE_DIAG=ON` |
| `EJIT_STATS_ENABLE` | `OFF` | 嵌入 taskpool 逐调用统计计数器（cache 命中等）。关闭时计数自增被裁掉，`ejit_taskpool_get_stats()` 报全零；**热点路径有跨核原子开销**，仅调测打开。 | `build.sh --stats` 或 `-DEJIT_STATS_ENABLE=ON` |
| `EJIT_DUMP_ASM` | `OFF` | 在启用后端裁剪（`EJIT_TRIM_LLVM_BACKEND`）时，重新打开文本汇编发射，使运行时 IR/ASM 转储能产出汇编。 | CMake：`-DEJIT_DUMP_ASM=ON` |
| `EJIT_WRAPPER_TIMING_REPORT_EVERY` | `100000` | AOT wrapper 计时的周期性汇总打印间隔（每 N 次调用打印一行汇总）。设为 `0` 关闭周期输出。 | 编译期宏：`-DEJIT_WRAPPER_TIMING_REPORT_EVERY=10000` |

> **一键全开**：CMake 预设 `ejit-minimal-aarch64_be` 已将 `EJIT_DIAG_ENABLE`、`EJIT_SRE_DIAG`、`EJIT_DUMP_ASM`、`EJIT_STATS_ENABLE` 全部置 `ON`，适合带板调测。
>
> ```bash
> cmake --preset ejit-minimal-aarch64_be
> ```

---

## 2. 运行时日志级别

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

## 3. 统计信息

### 3.1 旧版 LRU cache 统计

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

### 3.2 SRE taskpool 统计

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
  uint64_t instanceDisabledPreActivate; // 首次激活前命中的子集（init→activate 窗口）
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

**作用**：描述 taskpool 的 cache / 去重 / 队列流水线运行状况，定位"为何没编译 / 为何编译慢 / 队列是否拥塞"。

> **注意**：逐调用计数器（`cacheHits` 等）需要构建期打开 `EJIT_STATS_ENABLE`（见 §1），否则 `get_stats()` 返回全零；`print_compiled()`、`get_worker_core()`、`pending_count()` 不依赖该开关。

### 3.3 代码池统计

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

---

## 4. JIT IR / ASM 转储

提供两种互补的转储方式：**文件式**（一次性、全量、便于离线分析）与**内存捕获式**（运行时按名过滤、经平台日志回读）。

### 4.1 文件式 IR 转储（`dumpJITDir`）

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

### 4.2 内存捕获式 IR+ASM 转储

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

## 5. 注册表与元数据内省

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
| `ejit_print_active` | 列出每个注册 period 下当前激活的 (period, cell)。静态变量视为恒激活。用于诊断"某 period 实例为何编译 / 为何没编译"。 |
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

## 6. Wrapper 计时

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

## 7. 错误报告

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

## 8. 缓存诊断

```c
void ejit_clear_cache(void);
void ejit_invalidate(const char *periodName, uint8_t cellIdx);
```

**作用**：

- `ejit_clear_cache`：清空整个 JIT cache，强制后续调用全部重新编译。用于验证可重现性或释放代码内存。
- `ejit_invalidate`：仅失效指定 period / cell 实例的缓存条目，用于定向重编译。

---

## 9. 常用诊断流程

### 9.1 带板首次调测（bring-up）

1. 用 `ejit-minimal-aarch64_be` 预设构建（诊断全开），或 `build.sh --stats`。
2. `ejit_init()` 后调用 `ejit_print_registry()` 确认 AOT 注册表正确。
3. `ejit_print_version()` 记录构建标识，与源码版本对齐。
4. 默认 `EJIT_LOG_INFO` 即可观察 init / 编译 / cache MISS 等关键事件；必要时 `ejit_set_log_level(EJIT_LOG_VERBOSE)`。

### 9.2 性能 / 命中率诊断

1. 构建 `--stats`，运行后 `ejit_taskpool_get_stats()` 读取 `cacheHits` / `asyncCompiles` / `alreadyPending` / `queueFull` 等。
2. `ejit_taskpool_print_compiled()` 查看实际编译了哪些特化。
3. 若 wrapper 开销可疑，对 AOT 端加 `-mllvm -ejit-wrapper-timing`，观察周期汇总中查找 / 调用 / 释放各段耗时。
4. `ejit_get_code_pool_stats()` 监控代码内存是否趋近耗尽（`usedBytes` vs `reservedBytes`）。

### 9.3 现场问题定位

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

## 附录 B：构建期诊断开关速查

| 开关 | 关闭时影响 | 推荐场景 |
|------|-----------|----------|
| `EJIT_DIAG_ENABLE` | 所有运行时日志静默 | 调测 ON，生产 OFF |
| `EJIT_SRE_DIAG` | 日志走 `std::printf` | 裸金属 / SRE 目标必开 |
| `EJIT_STATS_ENABLE` | taskpool 逐调用计数报零 | 调测 ON（有热点原子开销），生产 OFF |
| `EJIT_DUMP_ASM` | IR/ASM 转储无汇编 | 需要汇编转储时 ON |
| `-mllvm -ejit-wrapper-timing` | wrapper 无计时探针 | 分析 wrapper 开销时启用 |

> 所有诊断 API 的头文件：`llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h`（公共 C ABI）。
