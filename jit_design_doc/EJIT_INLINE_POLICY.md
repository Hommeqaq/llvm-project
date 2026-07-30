# EJIT 链接期内联策略与诊断 设计文档

**版本**: 1.0
**日期**: 2026-07-30
**关联**: EJIT_CROSS_TU_INLINE.md, PASS1_EJitRegisterBitcode.md
**入口**: `lld/ELF/EJitCrossLink.cpp::runRealInliner`

---

## 1. 背景与目标

`-fejit-cross-inline` 已实现跨 TU 合并与内联（链接期 `runRealInliner` =
`AlwaysInlinerPass` + `ModuleInlinerWrapperPass`）。当前内联完全交给 LLVM 默认
成本模型，存在两个不足：

1. **无 PGO 时热/冷分支不被显式驱动**。标准 inliner 无 profile summary 时：
   - 认 `cold` 函数属性（阈值 45）和本地 BFI 的 2%-cold / 525-hot 启发式；
   - **不认 `hot` 函数属性**（`isFunctionEntryHot` 先查 `hasProfileSummary()`）；
   - **不认 `!prof` 热边**达到 3000 hot-callsite 阈值（只给本地 525）。
2. **无可观测性**。哪些子函数因何种原因没被内联，链接日志里看不到。

**目标**：让 ejit_entry 子树的内联由**热/冷分支**驱动、**可配置阈值**、并打印
每个未内联 call site 的**原始原因**。传递性（子函数的子函数）由 CGSCC `InlinerPass`
的迭代保证。

---

## 2. 需求

| 编号 | 需求 |
|---|---|
| R1 | 迭代/传递内联 ejit_entry 子树（CGSCC 自身迭代，策略每层生效） |
| R2 | 热分支内联、冷分支不内联（无 PGO，靠 IR 信号分类） |
| R3 | 可配置：冷/热边界 + 内联阈值（warm/hot/cold） |
| R4 | 诊断：开=打印每个未内联 call site 的原始原因 + 汇总；关=静默 |
| 约束 | 仅作用于 EJIT 合成模块；不影响 AOT；`always_inline` 强制、`noinline` 强制跳过；默认行为可回退 |

---

## 3. 设计

### 3.1 架构：自定义 InlineAdvisor（不重造内联器）

`ModuleInlinerWrapperPass` 不接受预构造 advisor，但可在 `MPM.run` 前向
`ModuleAnalysisManager` 注册 `PluginInlineAdvisorAnalysis`，其工厂返回我们的
`EJitInlineAdvisor`。`InlinerPass` 负责真正的 `InlineFunction` 与 CGSCC 迭代
（传递性天然得到），advisor 只做**决策 + 原因记录**。

```
runRealInliner(M):
  若 ejit-inline-policy == off: 走原逻辑 (AlwaysInliner + 默认 ModuleInliner) 返回
  LAM/FAM/CGAM/MAM 注册分析 (含 BFI/BPI/PSI)
  MPM.addPass(LowerExpectIntrinsicPass)     # @llvm.expect -> !prof，关键
  MPM.addPass(AlwaysInlinerPass)            # always_inline，不走 advisor，不进诊断
  MAM.registerPass(PluginInlineAdvisorAnalysis(makeEJitAdvisorFactory(...)))
  MPM.addPass(ModuleInlinerWrapperPass(InlineParams{ ComputeFullInlineCost=true, ... }))
  MPM.addPass(cleanup: Promote/InstCombine/SimplifyCFG)
  MPM.run(M, MAM)
  flush 诊断 (按 ejit-inline-diag 开关)
```

`AlwaysInlinerPass` 独立处理 `always_inline`（直接 `InlineFunction`），不经 advisor，
故不进诊断——符合"诊断只针对未内联"的定位（`always_inline` 总被内联）。

### 3.2 EJitInlineAdvisor / EJitInlineAdvice

`EJitInlineAdvisor::getAdviceImpl(CallBase &CB)`：
1. 取 callee。声明（无 body）-> 记 `unavailable definition`，返回 not-recommended。
2. 间接调用 -> 记 `indirect call`，返回 not-recommended。
3. 调 `getInlineCost(CB, Params, TTI, BFI, PSI, ...)`（`ComputeFullInlineCost=true`）：
   - `isAlways()` -> recommend inline。
   - `isNever()` -> 记 LLVM 原始原因 `getReason()`，返回 not-recommended。
   - 否则得 cost 值 -> 按 zone 选阈值比较（见 3.3）。
4. zone 阈值比较：cost <= threshold -> recommend；否则记原因 `cost=N exceeds threshold=M`，not-recommend。

`EJitInlineAdvice`（继承 `DefaultInlineAdvice`）携带 caller/callee/loc/zone/cost/
threshold/预计算原因，覆写：
- `recordUnattemptedInliningImpl()`：flush 预计算原因（cost/cold/no-body/...）。
- `recordUnsuccessfulInliningImpl(const InlineResult&)`：flush `Result.getFailureReason()`
  （InlineFunction 时的合法性失败，覆盖预计算原因）。
- `recordInliningImpl()` / `recordInliningWithCalleeDeletedImpl()`：成功，不记录
  （诊断只记未内联）。

### 3.3 热/冷分类器（无 PGO，显式有序检查）

`classifyZone` 返回 `{Zone, Evidence}`，Evidence 直接写进诊断。按优先级判定：

1. callee `cold` 属性 或 caller `cold` 属性 -> **COLD**（`cold function attribute`）。
2. `!prof` 支配边：沿 dominator tree 上溯，对每个支配该 BB 的条件分支用
   `ProfDataUtils::extractBranchWeights` 读权重；通向该 BB 的边权重
   ≤ `ejit-inline-cold-cutoff`(默认 1) -> **COLD**（`cold branch (edge weight W <= cutoff C)`）；
   ≥ `ejit-inline-hot-cutoff`(默认 2000，对齐 `__builtin_expect` 2000:1) 且远大于兄弟边 -> **HOT**。
3. unreachable/noreturn 路径：call 所在 BB 以 `unreachable` 结尾（或 invoke
   normal dest 如此）-> **COLD**（`unreachable path`）。
4. callee `hot` 属性 或 `inlinehint`(C `inline`) -> **HOT**（标准 inliner 无 PGO
   不认 `hot`，我们认）。
5. **必走（must-execute）**：call 所在 BB 的块频率 ≥ caller 入口频率（每次进入函数都执行，
   或循环放大）-> **HOT**（`must-execute (block freq >= entry freq)`）。这是无标注直线 call 的
   "必走"路径。`__builtin_expect` 的 likely 分支块频率 ≈ 0.9995 入口（< 入口），不算必走，
   但已被第 2 步 `!prof` 热边判 HOT，二者不冲突。
6. 否则 **WARM**。

zone -> 策略：HOT 用 `ejit-inline-hot-threshold`、WARM 用 `ejit-inline-threshold`；
**COLD 永不内联**（不管 cost），除非 `always_inline`（它在 `getInlineCost` 阶段已返回 Always，
先于 zone 判定）。`noinline` 由 `getInlineCost` 返回 Never（属性短路），不进 zone 判定。

### 3.4 信号保留

cross-inline 模式 PASS1 嵌入**完整 module** bitcode，`!prof`/函数属性都保留。
但 `__builtin_expect` 先降为 `@llvm.expect`，再由 `LowerExpectIntrinsicPass` 转
`!prof`；`runRealInliner` 原 pipeline 不含该 pass。**必须在 inliner 前加
`LowerExpectIntrinsicPass`**，否则 `!prof` 不可见、3.3 的边权重分类失效。

### 3.5 诊断

运行时 `cl::opt` 开关（不依赖编译期 `EJIT_DIAG_ENABLE`，便于随时开）。advisor 把
每个决策存入列表，inliner 跑完后按开关 flush。输出走 lld stderr。

格式（原始原因 + 判定证据，不自定义分类）：
```
[ejit-inline] <caller> -> <callee> at <file:line>: not inlined (cold flow (cold branch (edge weight 1 <= cutoff 1)))
[ejit-inline] <caller> -> <callee> at <file:line>: not inlined (noinline function attribute)
[ejit-inline] <caller> -> <callee> at <file:line>: not inlined (cost=480 exceeds threshold=225 (zone=warm))
[ejit-inline] summary: considered=N inlined=M not_inlined=K
```
合法性失败原因直接取自 `InlineResult::getFailureReason()`；成本原因带 zone 与阈值；
冷流原因显式写 `cold flow (<证据>)`。

---

## 4. 选项（cl::opt，lld 经 -mllvm 解析）

与 `ejit-cross-inline` / `UseInlineAdvisor` / `PreInlineThreshold` 同机制。用户侧
`clang -fejit-cross-inline -Wl,-mllvm,-<opt>=<val>` 或
`ld.lld --ejit-cross-inline -mllvm -<opt>=<val>`。

| 选项 | 类型 | 默认 | 语义 |
|---|---|---|---|
| `ejit-inline-policy` | `off\|on` | `on` | `off`=原逻辑（AlwaysInliner+默认ModuleInliner，无 advisor 无诊断）；`on`=EJIT 热/冷 advisor |
| `ejit-inline-threshold` | int | 225 | WARM 区阈值 |
| `ejit-inline-hot-threshold` | int | 3000 | HOT 区阈值（必走 / 热分支 / hot 属性 / inlinehint） |
| `ejit-inline-cold-cutoff` | int | 1 | 通向 call 的分支边权重 ≤ 此值 -> COLD（永不内联） |
| `ejit-inline-hot-cutoff` | int | 2000 | 该边权重 ≥ 此值（相对兄弟边） -> HOT |
| `ejit-inline-diag` | bool | false | 开=打印每个未内联 call site 原始原因 + 汇总；关=静默 |
| `ejit-inline-fast-reject-margin` | int | 2 | 快速早退：Hot/Warm callee 指令数 > 区阈值×此值 时跳过 CallAnalyzer 直接判不内联（0=关闭）。保守，极少数高折叠函数可能被误拒 |
| `ejit-inline-verify` | bool | false | 逐 entry `verifyModule`（默认关以提速；registry 模块始终校验） |

COLD 不是阈值选项：冷流（cold 属性 / `!prof` 冷边 / unreachable）一律不内联，除 `always_inline`。

### 4.1 链接速度

advisor 决策顺序刻意把廉价检查排在昂贵的 `getInlineCost`（CallAnalyzer 全量遍历 callee）之前：
1. 间接/无定义、`noinline`/`always_inline` 属性 -> 直接判定，不跑 CallAnalyzer。
2. `classifyZone`（DT + BFI，廉价）-> 冷流直接不内联，不跑 CallAnalyzer。
3. `ejit-inline-fast-reject-margin` 早退超大 callee，不跑 CallAnalyzer。
4. 仅 Hot/Warm 且未早退的 callee 才跑 `getInlineCost`（`ComputeFullInlineCost=true`）。

加上 `ejit-inline-verify` 默认关（逐 entry 校验挪到按需），多 entry / 大合成模块时链接明显更快。
用 `--time-trace` 可看 `EJitCross:Inliner` / `PerEntryExtraction` 各阶段耗时定位下一步。

---

## 5. 文件改动

| 文件 | 改动 |
|---|---|
| `lld/ELF/EJitCrossLink.cpp` | cl::opt 选项；`EJitInlineAdvisor`/`EJitInlineAdvice`；热/冷分类器；诊断 sink；`runRealInliner` 接入 `LowerExpectIntrinsicPass` + `PluginInlineAdvisorAnalysis` |
| `lld/ELF/CMakeLists.txt` | 确认链接 InlineAdvisor/Analysis/IPO 组件（lld/ELF 已含，按需补） |
| `lld/test/ELF/ejit-cross-inline-policy.ll` | 新建：验证 hot/cold 内联行为 + 诊断输出 |

---

## 6. 风险与验证

- **`getInlineCost` 依赖大量分析**（TTI/BFI/PSI/ORE/AAResults）：需在 advisor 工厂
  里正确从 FAM 取，镜像 `DefaultInlineAdvisor::getAdviceImpl`。
- **`PluginInlineAdvisorAnalysis` 时机**：必须在 `MPM.run` 前注册到 MAM。
- **`!prof` 可见性**：依赖 `LowerExpectIntrinsicPass` 先跑（3.4）。
- **验证**：lit 测试用 `__builtin_expect` 构造热/冷分支，断言热分支 call 被内联、
  冷分支 call 保留，并 `-mllvm -ejit-inline-diag` 检查原因行。
