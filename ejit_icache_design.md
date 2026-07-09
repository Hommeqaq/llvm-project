# EJIT Per-Function Inline Cache - v1 Design & Benefit Summary

> v1 范围:**简单 + 性能**。单槽 per-function HP 缓存,不重入,惰性 HP-gated free,
> flag 默认关。覆盖共享 taskpool。

## v1 实现状态(已完成)

- **AOT(EJitWrapperGen)**:`-ejit-inline-cache` flag(默认关)在 `jit_entry` 与
  `jit_call` 之间插入 `jit_icache` 探针块 + `jit_icache_dispatch`(命中直接 call,
  无 `release_read`)。`<=2` 维用 `ejit_icache_try_Nd`,`>2` 维用 generic。幂等。
  ✅ lit 测试 `ejit-wrapper-gen-icache.ll`。
- **运行时(EJitSharedTaskPool)**:per-core BSS 表 `gEJitICache[MAX_CORES][FUNC_SLOTS]`
  (默认 8×64,可调),`icacheTry`(纯 load,version+enabled+generation+code-sharing 门)
  + `icacheFill`(publish 快照)。`compile_or_get[_Nd]` 成功路径填 icache(含 taskpool
  命中路径,否则冷 icache 永不填充)。✅ 单元测试 2 个(命中/失效/单态/范围;安全门)。
- **回收安全(v1)**:**安全门**——wire `setReleaser` 时 `icacheReclamationSafe_=false`,
  `icacheTry` 恒 MISS、`icacheFill` no-op,杜绝 UAF。v1 生产不 wire releaser(代码不
  释放)→ 门不触发 → icache 全开。**HP-scan retire(惰性 HP-gated free)为后续增量**,
  落地后把门翻回 true,即可在"能 free"下开 icache。
- **验证**:lit 31 pass/5 XFAIL(无回归);shared taskpool 单元 45 pass/24 pre-existing
  fail(stats 编译开关问题,与本次无关);`build_release_x86` LLVMEJIT + opt + 测试全绿。

## 后续增量(未做)

- **HP-scan retire**:在 `cachePublish` 的 `releaseFn_` 调用点(及 stale-compile 拒
  绝点)前插入 `retireWithIcache(oldFn)`:扫描所有核 icache 槽,被钉则入 pending(自旋
  锁 + 定长数组,裸核兼容),无人钉才释放;每次 retire 顺带 drain pending。落地后移除
  安全门(`icacheReclamationSafe_` 恒 true)。这是"能 free 下开 icache"的完整方案。
- per-core 表扩容 / 多态槽(交替 dims)/ 非共享 `EJitTaskPool` 覆盖。

---


## 1. 目标

消除 `ejit_entry` 命中的主导单次调用成本:跨核 `readers_` RMW 对(`fetchAdd`+
`fetchSub`)与 bucket slot 扫描。命中时直接调用特化码,**无 read-token、无 slot
扫描、无 `release_read`**。

## 2. 锁定的 v1 设计

### 2.1 安全前提

- **不重入**:中断不在同核重入 `ejit_entry` -> 调用期间本核 icache 槽稳定 ->
  单槽 HP 即可,无需 `activeDepth`/re-check,热路径纯 load。
- **环境可 free**:用 HP(槽钉住 fnPtr)+ 惰性 retire 保证不 UAF;version 检查
  保证不执行过期特化。两者正交,都必需。

### 2.2 数据结构

```c
struct EJitICacheSlot {          // per-core, 写者仅本核
  uint32_t valid, numDims;
  uint32_t dimTypes[4], instances[4], versions[4];
  void    *fnPtr;                // 钉住的代码指针(release 存 / acquire 读)
};  // 表: EJitICacheSlot[ numCores ][ funcIndex数 ]  (按注册数密集索引)

// retire 管线
struct EJitRetiredNode { void *fn; uint32_t epoch; ... };  // pendingFree 链表
```

### 2.3 热路径(命中,纯 load,无 RMW)

```
ejit_icache_try(funcIndex, dims, numDims, &outFn):
  s = &icache[coreId][funcIndex]
  P = load_acquire(s->fnPtr)
  if P==null or !s->valid: return MISS
  for i in numDims:                       // 正确性: version + dims
    cv = load_acquire(version[ s->dimTypes[i] ][ s->instances[i] ])
    if s->versions[i]!=cv or s->instances[i]!=dims[i].instance: return MISS
  *outFn = P; return HIT
// 调用方: call outFn(args); (无需 release_read,无需 ejit_icache_exit)
```

### 2.4 命中 miss 与 fill

miss -> 原 `compile_or_get`(不动)。成功返回时(命中或新编译),运行时**填本核
icache 槽**:`{valid=1, dims, dimTypes, versions=current, fnPtr}`(release 存)。

### 2.5 回收(惰性,HP-gated)

```
retire(oldFn):  加入 pendingFree; drainPending():
  for fn in pendingFree:
    if 任一核 icache 槽 == fn:  延迟(槽钉住)      // HP 扫描
    else:                       free(fn)          // 无人钉 -> 释放
```
- retire 触发:taskpool 覆盖持有 oldFn 的槽(同身份重编译或 bucket 满淘汰)。
- free 门控:HP 扫描无持有者。在途调用必伴随槽持有 -> 不会在用期间 free。
- v1 生产 `releaseFn_` 未 wire -> 无 free 发生 -> HP 扫描平凡安全;wire 后即生效。
- **安全门**:若 `setReleaser` 已 wire 但未走 `ejit_icache_safe_release`,
  `ejit_icache_try` 恒返回 MISS(icache 自禁用)。

### 2.6 AOT 改动(EJitWrapperGen)

```
jit_entry   -> jit_icache (funcidx 有效) | jit_fallback
jit_icache  : call ejit_icache_try[_Nd](funcIdx, dims..., &outFn)
              HIT -> jit_icache_dispatch ; MISS -> jit_call
jit_icache_dispatch: call outFn(args); ret   (无 release_read)
jit_call    : [不变] compile_or_get(...); 成功则运行时填 icache
jit_dispatch: [不变] 间接 call + release_read + ret
jit_fallback: [不变] AOT 体
```
flag `-ejit-inline-cache`(AOT+运行时,默认关)控制生成。

## 3. 收益

### 3.1 命中前后对比(每次 `ejit_entry` 调用)

| 开销项 | 当前(命中) | v1 icache(命中) |
|---|---|---|
| C 函数调用 | 2 次(`compile_or_get`+`release_read`) | **1 次**(`ejit_icache_try`) |
| **跨核 RMW** | **2 次**(`readers_` fetchAdd+fetchSub) | **0** |
| bucket `writeFlag` acquire | 2 次 | 0 |
| slot 扫描 | 最多 16 槽 × 数次 load | 0 |
| `generation` acquire | 1 | 0 |
| `version[dt][inst]` acquire | N | N(同,不可避免,read-mostly) |
| `fnPtr` acquire | 1 | 1 |
| funcidx/dimtype load | 1+N | 1+N(同) |
| 间接 call | 1 | 1 |

砍掉:1 次 C 调用、**2 次跨核 RMW**、2 次 writeFlag acquire、整段 slot 扫描、
generation load。保留:version 加载(正确性必需,read-mostly 无争用)、fnPtr/
funcidx/dimtype 加载、间接 call。

### 3.2 多核扩展性(最大收益)

`readers_` 每 bucket 一个计数器(32 bucket,`alignas(64)`),命中同 bucket 的核
在这一字上 `fetchAdd/fetchSub` 串行化(`EJitStats.h:11-13` 点名的多核主导成本)。
v1 改 per-core 私有槽 -> 读 token 跨核 RMW 对消失 -> 核间不为读串行;version 数组
read-mostly -> cache line 共享只读无弹跳。**命中吞吐随核数近线性扩展**。

### 3.3 单核延迟

少 1 次 C 调用 + 砍 slot 扫描(命中不再遍历最多 16 槽)+ 砍 writeFlag 双检查。
命中延迟下降且更稳定(无扫描最坏情况)。

### 3.4 miss 路径无回归

miss 仍走原 `compile_or_get`+`readers_`;icache 仅在前加一次廉价探针(N 次 version
load+比较)。稳态以命中为主,可忽略。不引入编译/回退回归。

### 3.5 内存代价

per-core 表:`numCores × funcIndex数 × ~64B`。8 核 × 200 `ejit_entry` ≈ 100KB
(按注册数密集索引)。冷路径 retire HP 扫描 = `numCores × funcIndex数` load,retire
稀有,可忽略。

## 4. v1 不做的(换简单+性能)

- 不做 `activeDepth`/re-check(依赖不重入)。
- 不做周期性 sweep(接受死身份滞留码,受历史特化基数上界)。
- 不做多态槽(单态;交替 dims 抖动时退化为 miss)。
- 不动非共享 `EJitTaskPool`(只覆盖共享 taskpool)。
- flag 默认关,验证后开。

## 5. 安全保证

1. **不执行过期特化**:icache version 检查 miss,taskpool `cacheLookup` version
   检查也 miss。失效后新调用不走旧 fnPtr。
2. **不 UAF**:fnPtr 被某核槽持有时,HP 扫描必延迟释放;槽仅 miss-refill 时被本核
   覆盖(不重入保证调用期间稳定)。在途调用期间 fnPtr 必被钉,不 free。
3. **职责分离**:taskpool 决定 retire 谁(publish 捕获 oldFn);icache 决定何时能
   free(HP 扫描无持有者)。

## 6. 配置

| Flag / define | 默认 | 作用 |
|---|---|---|
| `-ejit-inline-cache` (AOT+runtime) | off | 生成/启用 icache |
| `EJIT_ICACHE_RECLAMATION_SAFE` | off | wire releaser 时必须开,否则 icache 自禁用 |
| `-ejit-icache-stats` | off | per-core icache 计数(隐含 `EJIT_STATS_ENABLE`) |

## 7. 实施顺序

1. 运行时:icache 表 + `ejit_icache_try/_Nd` + fill-on-publish + stats(共享池)。
2. 单元测试(命中/miss/version/dims)。
3. AOT:`EJitWrapperGen` 的 `jit_icache` 块 + lit 测试。
4. 集成测试 + 性能测量。
5. 回收:wire `retire` + HP 扫描 + 安全门。
6. 验证后开默认。
