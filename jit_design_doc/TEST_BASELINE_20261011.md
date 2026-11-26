# EJIT 测试基线（重构前）— 2026-10-11

HEAD: `e09df8420627` (ejit_dev_spec5)
构建: `build_release_x86` (Release, x86)

## 基线配置（关键！两个非默认开关）

```
cmake -S llvm -B build_release_x86 \
  -DEJIT_SRE_SHARED_CODE_POINTERS=ON \   # EJITSharedTaskPoolTests 的 22 个 peer/4K 用例需要
  -DEJIT_STATS_ENABLE=ON                 # 集成测试断言 asyncCompiles/cacheHits 计数器需要
```

其余沿用缓存默认：SHARED_TASKPOOL=ON、4K_SEAL=ON、CODE_POOL/BATCHED/NO_RECLAIM/VP/DIAG=OFF。
另需：`ninja split-file opt clang lld`（lit 依赖，之前未构建导致 opt 陈旧 9/10 vs 源码 9/22）。

## 结果

| 套件 | 结果 |
|---|---|
| EJITTests (gtest) | **199/199** |
| EJITCodePoolTests | **62/62** |
| EJITTaskPoolTests | **91/91** |
| EJITSharedTaskPoolTests | **158/158**（ON 配置下 4 个 `#ifndef` 负向用例按设计不编译；OFF 配置时 22 个 peer 用例必失败） |
| AOT pass lit | **51 通过 + 4 XFAIL**（55） |
| clang lit (ejit_*) | **19/20** |
| 集成 ejit_test | **24 通过 / 4 失败** |

## 已知红（重构期间视为基线，不算新回归）

1. **`clang/test/CodeGen/EJIT_FREE_DIM.c`**（lit）— bound_ptr 元数据 ABI 形状漂移：测试期望 5 操作数 `!ejit_bound_ptr` 节点，codegen 发射不同形状（122f16cf 多 bound 指针 backport 改了描述符格式未更新该测试）
2. **`ejit_volatile_test`**（集成）— 3 个入口（volatile/atomic/union）**编译真实失败**（failed=3, readyEntries=0），来自 8/28 merge #192，早于 PR233/所有 backport
3. **`ejit_closure_slim_test`**（集成）— "no JIT entries (silent AOT fallback)"，同期
4. **`ejit_pgo_test`**（集成）— "Tier-1 compile did not happen (compiles=0)"，同期
5. **`ejit_trace_test`**（集成）— 同期
6. **`ejit_vp_multicore_test.c:316`**（编译错误，被 stale object 掩盖）— `ejit_vp_stats_t` 在 VP=OFF 构建下不存在，测试缺 `#ifdef` 门控（与 EJitPgoTest 同类问题）

## 已修（本机工作树，未提交）

- `llvm/unittests/ExecutionEngine/EJIT/EJitPgoTest.cpp` — `realOrcStressCodeReady/Flush` 与
  `RealOrcTwentyFunctionWorkerThrottleKeepsHeartbeatAlive` 加 `#ifdef EJIT_SRE_CODE_POOL` 门控
  （HEAD 上 CODE_POOL=OFF 构建必炸：引擎方法 `isCodeReady/flushPendingCode` 是 CODE_POOL 门控的，测试裸调）

## 排查记录（为什么会误判很多失败）

- `EJIT_STATS_ENABLE=OFF` 时 `EJIT_STAT_INC` 编译成空操作 → `asyncCompiles` 恒 0 →
  10 个集成测试"silent AOT"假失败。`readyEntries` 是真实槽位计数，不受影响。
- gtest 目标（EJITTaskPoolTests/EJITSharedTaskPoolTests）在 CMake 里显式定义 EJIT_STATS_ENABLE，
  所以 gtest 绿而集成红——同一棵树两套口径。
- gdb 断点会扰动 worker/主线程时序，使 runCompile 的版本 checkpoint 全部丢弃——
  观察者效应，勿据 gdb 会话断定"compile 从未发生"。
