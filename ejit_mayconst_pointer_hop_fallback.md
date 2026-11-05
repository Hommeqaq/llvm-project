# EJIT may_const 多跳指针折叠:两跳链设计提案

- **日期**:2026-11-04
- **基线**:`ejit_dev_spec5` @ `700234661017`;机制依赖 PR #252(上下文保持/常量发现)与 PR #256(锚定 witness 轻量折叠)的既有基建;与 PR #255(非指针全局嵌套数组元数据)**互补零冲突**——#255 明确把指针元素全局排除在外
- **性质**:设计提案,未实施;实施另立 PR,届时以本文档为准

## 1. 背景:场景与断点

板上幸存 may_const 站点中的"多跳指针"形态:

```c
extern StructType *arr[A][B];                          // 注册 period 数组全局(数组 of 指针)
typedef struct { ...; Basic *e[MAX_N]; } StructType;   // e 未标注
/* Basic 内 field1 已标 ejit_may_const */

v = arr[dim1][ejit_free_dim_n % MAX_INDEX]->e[x].field1;
```

链路三段:

```
跳1:arr[dim1][free%MAX]   槽在注册数组里 → period 契约已担保,零标注
跳2:p->e[x]               槽在所指对象内部 → 无担保            ← 断点
终点:q->field1            Basic 字段已标注,marker 已在,折不了
```

既有机制(legacy / strict / #256 轻量)都要求地址**锚定到注册对象的根**;跳 2 的槽在所指对象内部,三种担保(注册存储 / 结构体字段标注 / bound_ptr 参数)均不覆盖 → 全链不折。这是 IR_3 家族"点内跳"的具体实例。

## 2. 为什么仅给 e 标注不够(必要但不充分)

给 `e` 加 `ejit_may_const` 后:`p->e[x]` 的 load 自动带 marker(clang 数组退化路径传播字段标注,终点 marker 本就在)。但三个机制缺口仍在:

| 缺口 | 现状 |
|---|---|
| **求解器** | Pattern 3(`tryReplaceIndirect`)只认**一跳**,且不带 Assumed——跳 1 的 `free%MAX` 是运行时下标,槽偏移算不出,保守拒绝;终点的链根又是 `e[x]` 这条运行时 load,所有要求常量根的路径全部出局 |
| **元数据** | 2D 数组 of 指针全局连裸表都为空(unwrap 只剥一层数组);`ejit_pointer_layout` 门控只认指针型全局 |
| **终点授权** | Basic 对象的地址不在任何注册对象内,现有路径锚不住 |

标注解决的是**担保**(跳 2 槽的窗口稳定性,无替代来源);本文档解决**机制与凭据**。

## 3. 设计

### 3.1 跳跃求解器(补 Pattern 3 的三处局限:无 witness / 只认 GV 直根 / 无逐跳授权)

新 walker 挂在 run() 模式链末端(Pattern 3 之后)并纳入 #256 的 PendingFields 重试;对终点 may_const load 的指针链**逐跳解析**,中间指针值仅编译期内部使用、不写回 IR:

- **跳 1(注册槽跳)**:槽地址 = `resolveBase`(数组全局)+ `computeAbsoluteGEPOffset`(**带 Assumed**,witness=0 求值、poison 语义照旧)→ containment(数组大小来自 IR 类型)→ `createConstantFromMemory` 式读槽得 StructType* 值(原语已存在)。
- **跳 2(标注字段跳)**:槽地址 = 跳 1 指针值 + offsetof(e) + x×指针宽(x 经 Assumed 求值)→ 授权两钥匙:**该跳 load 自带 marker** + 槽偏移落在 StructType 布局的 e 字段区间内 → 读槽得 Basic* 值;**此跳 load 同时被折叠为 IntToPtr 常量**(见决策 3)。
- **终点**:地址 = Basic* + offsetof(field1) → 授权两钥匙:marker + 偏移落在 e 区间**内嵌的 pointee 布局**(Basic 字段区间)且宽度足够 → 读值冻结。

**边界(复用 #256 全套)**:不救援 strict 拒绝;歧义拒绝(多候选跳);逐跳拒绝原因诊断(`hop-out-of-registered-object` / `hop-field-unauthorized` / `hop-depth-exceeded` 等);volatile/atomic 照旧拒绝;store 保持活参数。

### 3.2 元数据三动作(CGEJIT.cpp,`emitEjitGlobalMetadata`)

1. **unwrap 修复**:剥数组到非数组元素为止(1D/2D/N-D 指针数组均可识别)。
2. **布局门控扩展**:`isPointerType()` → "解包后元素为指针"的数组全局也发 `ejit_pointer_layout`;步长 = sizeof(pointee 记录体);e 的逐元素槽区间由 `collectPointerPeriodFields` 现有数组展开覆盖(标注指针数组字段自身区间已被记录,现成)。
3. **pointee 布局内嵌递归**:标注指针(数组)字段的布局条目**内嵌**其目标记录体(Basic)的字段区间(嵌套子节点,按位置关联、不做类型名匹配;深度一层,预算对齐 4096 风格)。

## 4. 已对齐决策(2026-11-04)

| 决策 | 结论 |
|---|---|
| `e` 标注 | **接受**——全设计唯一业务源码改动(一行);无它则跳 2 不可靠(指针可被窗口期内重绑,冻结值失真) |
| 跳深 | **两跳起步**(常量上限,机制按可扩展设计) |
| 中间 load | **也折叠**为 IntToPtr 常量(标注所许可;审计口径干净;终点折叠不依赖它) |
| 交付 | 本文档先行,实施另立 |

## 5. 安全模型与契约条款

| 环节 | 担保 | 来源 |
|---|---|---|
| 跳 1 槽稳定 | period 注册契约 | 既有 |
| 跳 1 下标代表性 | free_dim 跨维同值契约 | 既有家族 |
| **跳 2 槽稳定** | **e 标注 = "存于此字段的指针值窗口期稳定"(指针字段标注语义推广)** | **新增条款** |
| 逐跳指针有效性 | 所选元素前缀对编译 worker 可读、已初始化(#256 部署契约的逐跳延伸) | 新条款 |
| 终点值稳定 | field1 的 may_const | 既有 |

## 6. 实施与验证方向(供评审与后续实施)

| 文件 | 改动 |
|---|---|
| `clang/lib/CodeGen/CGEJIT.cpp` | unwrap 修复 + 门控扩展 + pointee 内嵌递归(~100-150 行) |
| `clang/test/CodeGen/`(新 lit) | 数组 of 指针全局的布局发射(2D、e 槽区间、内嵌 pointee) |
| `llvm/lib/ExecutionEngine/EJIT/EJitStructFieldPass.cpp` | 跳跃求解器 + 逐跳授权 + 诊断 + run() 挂点(~200-280 行) |
| `llvm/unittests/ExecutionEngine/EJIT/EJitPointerPeriodTest.cpp` | 两跳链测试矩阵(~500-700 行) |
| `ejit_test/`(新 C 源复现件)+ 规格文档 | 照 `ejit_pointer_witness_source_repro.c` 模式;规格含指针字段标注语义与逐跳契约 |

测试矩阵要点:两跳链正例(跳 1 下标 witness/常量、x 常量/witness 各组合;LE/BE;L2/L3;三 Tier);负例(e 未标注拒、终点偏移越界拒、宽 load 拒、跳深超限拒、歧义拒、strict 拒不救援);store 保活;真实 ORC 管线端到端。验证:EJITTests 基线对照 + C 源复现 + 板上验收(站点 may_const 消失、输出一致、审计增量符合预期)。

## 7. 诚实声明

- **信任链逐跳延长**:每跳一次指针有效性假设,错误标注的爆炸半径随之增大(与"休眠标注激活"同理);`e` 标错的指针字段从无害变有害。
- **中间 load 折叠的 IR 暴露**:IntToPtr 常量进入 IR(与 #249 根物化同暴露级别)。
- 跳深两跳;更深形态(点内再嵌指针)将来另议。
- 实施前置:先合 #252、#256;`e` 标注须随业务源码更新(重编位码)。
- 部署契约新增逐跳条款——业务须能为"所选元素前缀可读"背书(对静态配置数据天然成立)。

## 附:白话摘要

现在的机制只认"地址从注册对象一条算术链直达";`arr[..][..]->e[x]->field1` 中间隔着两次"从内存里读指针",第二次(存在结构体字段里的指针)没有任何担保,所以整条链折不了。本提案做三件事:让求解器能**逐跳解析**(每跳算出槽地址、编译期读出指针值、继续算,中间值不写进代码);给"数组 of 指针"全局补上布局元数据;每一跳用**两把钥匙**放行(那一步的 load 带 marker + 偏移落在标注字段区间内)。业务侧只需一行:给 `e` 标上 may_const——含义推广为"存在这个字段里的指针,窗口期内不变"。装好后,这类两跳链的 may_const 自动折叠,零登记、零部署配置。
