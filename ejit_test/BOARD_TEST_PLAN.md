# EJIT 板上测试方案

验证 **dso_local 实验**（消除外部全局变量的数据 GOT 开销）与 **enable_ex cache-sync 修复**（补 I-cache 同步）在 aarch64_be 裸核生产场景下的效果。

- 分支：`worktree-ejit-verify-combined`（含 cache-sync 修复 + 3 个多核 demo）。
- 目标 preset：`llvm/CMakePresets.json` 的 `ejit-minimal-aarch64_be`（异步 + 共享池 + 跨核代码指针 + freestanding + 4K seal + BE）。

---

## 1. 两个构建配置

dso_local 实验在 `llvm/lib/ExecutionEngine/EJIT/EJitOrcEngine.cpp::loadBitcodeModule` 里（commit `33754cfdc82e`），是 `if (TT.isAArch64() && TT.isOSBinFormatELF())` 内的一个 `#if 0` 块。

| Config | `#if 0` 改成 | dso_local 清除 | codegen | 含义 |
|--------|-------------|---------------|---------|------|
| **A**（基线） | **`#if 1`** | 启用（`setDSOLocal(false)` 执行） | GOT/PLT 间接 | 旧行为，有 GOT 开销 |
| **B**（实验） | 保持 `#if 0` | 禁用 | ADRP/BL 直接 | 消除 GOT |

两个构建都带 cache-sync 修复（分支已含）。板测时：先 `#if 1` 构建 A 跑一轮，再改回 `#if 0` 构建 B 跑一轮。

---

## 2. 测试用例

| # | 用例 | 入口（无 main） | 输入全局 | 验证 |
|---|------|----------------|---------|------|
| 1 | `ejit_jit_verify_test` | `test_ejit_jit_verify` | `g_ci` / `g_ti` / `g_ci2` | JIT 正确性：编译/cache hit/分支折叠/多维/重编译（+间接验不溢出） |
| 2 | `ejit_got_probe_test` | `test_ejit_got_probe` | `g_ci` | 不溢出 + `:got:` dump + AOT-vs-JIT |
| 3 | `ejit_perf_aot_vs_jit_test` | `test_ejit_perf` | `g_ci` | perf_globals(GOT 重) vs perf_fold(特化收益) AOT-vs-JIT |
| 4 | run15.log `--wrap` 桩 | 产品测量（非 demo） | 真负载调用参数 | DlschCcScheduler 等 AOT-vs-JIT cycle |

前 3 个是多核 demo（板上跑），第 4 个是真负载 `--wrap` 桩。

### 统一结构（前 3 个 demo）
- 无 `main`，入口 `int test_ejit_xxx(uint8_t,uint8_t,uint8_t,uint8_t)`（4 个 args 全忽略），RTOS 在**每个核**上调用。
- 输入：文件级全局 `g_ci`（等），裸核环境调用前设。
- 三段式：`init_data` + `call_init_array_functions` + `ejit_init`（选举 worker）→ 分流（worker 核 `idle_forever`）→ 非-worker 核（activate/派发/`wait_for_compile`/验证/测量/`idle_forever`）。
- `SRE_printf` + `SRE_CycleCountGet64` + 共享 `ejit_bench_helpers.h`。
- 异步下用 `wait_for_compile` 等 worker 编译完再验 JIT 结果（不被 AOT fallback 蒙）。

---

## 3. 测试顺序

递进逻辑：**正确性 → GOT 验证 → 性能 → 真负载**。每个用例 Config A 先、B 后（A 建基线，B 看改进）。

### Phase 1：Config A（基线，GOT 存在）
1. **`ejit_jit_verify_test`**（A）-- 期望 PASS。验 JIT 基础设施 + cache-sync 修复。**地基，不过其它没意义。**
2. **`ejit_got_probe_test`**（A）-- 期望 `:got:` ~6、PASS。建立"GOT 存在"基线。
3. **`ejit_perf_aot_vs_jit_test`**（A）-- 期望 perf_globals JIT 更慢（GOT）、perf_fold JIT 更快（特化）。建立 perf 基线。

### Phase 2：Config B（实验，GOT 消除）
4. **`ejit_jit_verify_test`**（B）-- 期望 PASS。**关键 gate：FAIL（compileFailed>0）= 重定位溢出（slab 超 adrp 范围），停止后续，回退 dso_local 清除或走同址化。** 多跑几次（slab 动态）。
5. **`ejit_got_probe_test`**（B）-- 期望 `:got:` 0、PASS。验 GOT 真消除。
6. **`ejit_perf_aot_vs_jit_test`**（B）-- 期望 perf_globals JIT 回升、perf_fold 仍快。验性能恢复。

### Phase 3：真负载
7. **run15.log `--wrap` 桩**（A then B）-- `DlschCcScheduler` delta：A ~1700c → B ~600c（非 GOT 残余）。真负载确认。

---

## 4. 多核 bring-up（每个用例）

1. **一个核先跑** `test_ejit_xxx` → `ejit_init` 选举/创建 shared compile worker（`ejit_taskpool_get_worker_core()` 返回 worker 核）。
2. **其余核**（非 worker）跑 `test_ejit_xxx`。
3. worker 核检测 `core==workerCore` → `idle_forever("worker")`（等编译任务）；非-worker 核跑测量 → `idle_forever("benchmark-complete")`。

---

## 5. 输入设置

调用入口前，由裸核环境设好全局：

| 用例 | 全局 | 范围 | 说明 |
|------|------|------|------|
| ejit_jit_verify_test | `g_ci` | 0–15 | 主 cellIdx |
| | `g_ti` | 0–7 | trpIdx（多维） |
| | `g_ci2` | 0–15 | 第二 cellIdx（重编译测试） |
| ejit_got_probe_test | `g_ci` | 0–3 | g_probeArr 4 元素 |
| ejit_perf_aot_vs_jit_test | `g_ci` | 0–3 | g_pgPeriod/g_pfCfg 4 元素 |

数据由各 demo 的 `init_data()` 内部设置，无需外部输入数据。

---

## 6. 收集日志 & 期望结果

### 用例 1：`ejit_jit_verify_test`
- **日志**：`[BENCH][core=N]` 行 + 最终 `PASS/FAIL` + `stats`（ready/hits/compiles/compileFailed）。
- **期望**：A、B 都 PASS；`compileFailed=0`。
- **判定**：B 下 FAIL=溢出。

### 用例 2：`ejit_got_probe_test`
- **日志**：Step 1 overflow PASS/FAIL + `compileFailed`；Step 2 `dump ASM` 段（数 `:got:`）；Step 3 AOT/JIT cyc/call。
- **期望**：
  - Step 1：A、B 都 PASS（不溢出）。
  - Step 2 `:got:` 数：**A ~6，B 0**（GOT 消除证据）。
  - Step 3：A JIT 慢（GOT），B 回升。

### 用例 3：`ejit_perf_aot_vs_jit_test`
- **日志**：`perf_globals` + `perf_fold` 的 AOT/JIT cyc/call + verdict + stats。
- **期望**：
  - `perf_globals`：A JIT 更慢，B 回升持平。
  - `perf_fold`：A、B 都 JIT 更快（特化赢，n=64 放大）。

### 用例 4：run15.log `--wrap`
- **日志**：`DlschCcScheduler`（+ `DlschInitUserSchTypeStaticInfo`、`DlschUsrSchCalcOneTbMcsAndTBSize`）的 cycle 表（调用前/调用后/fn_call_avg/get_fn_avg）。
- **期望**：`调用后−调用前` delta：**A ~1700c，B ~600c**（非 GOT 残余）。下降 ~1000c = 真负载上 GOT 消除。

---

## 7. 判定标准 & gate

| 检查 | 通过条件 | 失败含义 |
|------|---------|---------|
| JIT 正确性（用例 1） | A、B 都 PASS，`compileFailed=0` | cache-sync 修复问题 / JIT 基础设施问题 |
| 不溢出（用例 1、2 在 B） | B 下 PASS，多跑几次都过 | slab 超 adrp 范围 → A 路线不稳，回退 dso_local 清除或同址化 |
| GOT 消除（用例 2） | `:got:` A~6 → B 0 | dso_local 实验未生效 |
| demo 性能（用例 3） | perf_globals B 回升、perf_fold 始终 JIT 快 | 特化收益未兑现 / 还有其它开销 |
| 真负载性能（用例 4） | DlschCcScheduler delta A~1700c → B~600c | 真负载上 GOT 未消 / 有 ~600c 非 GOT 残余（call stub 等） |

**关键 gate**：Phase 2 第 4 步（Config B 的 `ejit_jit_verify_test`）FAIL = 重定位溢出，**停止后续**，回退 dso_local 清除（`#if 1`）或走 slab 同址化路线。

---

## 8. 注意事项

- **slab 动态**（`SRE_MemAlloc`，运行间 1.5–2.2GiB 偏离 `.text`）：Config B 的用例 1、2**多跑几次**，确认没有某次 slab 落到 >4GiB 导致溢出。偶发溢出 → A 路线不稳。
- **cache-sync 修复必带**：不带的话 JIT 可能间歇执行 stale 指令，污染用例 2/3/4 的数据。合并分支已含。
- **计时口径**：demo 用 `SRE_CycleCountGet64`（= `CNTPCT_EL0` × ratio，wall-cycle，对齐 run15.log）。
- **输出**：`SRE_printf` 走平台日志，`[BENCH][core=N]` 前缀按核区分。
- **用例 3、4 互补**：3 是受控小 demo（隔离 GOT 和特化收益），4 是真负载（确认收益在产品函数上兑现）。
