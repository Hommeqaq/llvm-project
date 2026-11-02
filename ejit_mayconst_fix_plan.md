# EJIT may_const 指针型 period 折叠:方向与决策建议

- **日期**:2026-11-02
- **基线**:`ejit_dev_spec5` @ `700234661017`(含 PR #249 `ejit_pointer_period_lookup`、PR #250 NEON vectorization)
- **性质**:方向与决策建议,不含实施

## 背景

板上 dump 的特化后 IR 中,6 处 `!ejit.may_const` load 幸存,根因分四类:

| 类别 | 站点数 | 根因 |
|---|---|---|
| free_dim witness 被拒 | 3 | 指针型 period(全局指针,pointee 物化为 inttoptr)的绝对地址折叠路径对 witness 下标一律拒绝 |
| 坐标系错位 | 与上同类 | 该路径拿"距 pointee 的总偏移"直接比对"元素内部坐标"的字段表,常量下标 k>0 永不匹配(仅 slot 0 碰巧对上) |
| 业务标注缺失 | 2 | `void*` 参数无法标 `ejit_bound_ptr`(Sema 硬拒绝);slot 来源字段未标 may_const |
| 双重间接 | 1 | 地址需两跳指针读,担保机制(三种)均不覆盖 |

前两类是 EJIT 侧问题,也是本文焦点;后两类属业务标注,分派不变(见决策建议 4)。

## 场景(实测 IR 形状)

```llvm
; 形状 A:结构体 GEP,witness 下标
%7  = urem i16 %1, 5
%9  = getelementptr %struct.TestPara, ptr inttoptr(i64 ... to ptr), i64 %8
%10 = load i64, ptr %9, !ejit.may_const ...

; 形状 B:i8 GEP,步长内嵌在乘法常数(优化后 GEP 类型已无步长信息)
%15 = getelementptr i8, ptr inttoptr(i64 ... to ptr), i64 (urem(%3,5) * 1240 + 45)
%17 = load i8, ptr %16, !ejit.may_const ...

; 形状 C:bound_ptr 参数派生常量下标(slot 先折成常数 c,地址 = (c%5)*184 + 14)
```

## spec5 现状(PR #249 已落地的机制)

- **布局元数据 `ejit_pointer_layout`**:clang 对指针型 period 全局发射,含元素步长(sizeof pointee)与各标注字段的 {偏移, 宽度} 区间。
- **bounds 两个来源**:显式注册(`ejit_register_pointer_period_bounds`,登记 base+size);分配器发现(应用在 ejit_init 前注册 `ejit_set_allocation_size_lookup` 回调,运行时对当前 pointee 查分配大小)。显式优先。
- **strict 路径**(有 bounds 时):witness 放行;要求 entry 携带 period 身份契约;分配范围内 containment;GEP 链逐级在界内;步长归一;字段区间含宽度防护(access-fit)。
- **legacy 路径**(无 bounds):维持原常量下标精确匹配,witness 与常量 k>0 均不折。
- 配套测试已随 PR 合入:专项单测(EJitPointerPeriodTest)+ 布局发射 clang lit。

## 方向与决策建议

1. **witness 站点的解锁条件**:strict 路径要求 bounds 与 entry 契约**两者齐备**。
   - bounds 是应用侧行动:显式注册,或注册 allocation-size 回调走发现——均非默认,需部署侧决策。
   - 契约是既有缺口:free_dim 参数按设计不产生 period_arr_ind,目标 entry 若无匹配 period 身份,strict 路径进不去。建议逐站点核实 entry 元数据,缺失则业务补标注,或评估下述轻量路径。
2. **可选轻量路径(决策点)**:无 bounds 时按"元素 0 精确匹配"放行 witness。安全论证:字段表偏移严格小于元素尺寸,witness=0 求值下只有元素 0 的标注字段可能精确命中——读的字节与现有"常量下标 0"路径完全相同,无需 bounds、无需契约门。建议仅当"分配大小不可知且 entry 契约难补"的站点确实存在时再立项,避免与 strict 模型形成双轨。
3. **常量下标 k>0**:显式 bounds + 布局元数据即折。无 bounds 场景维持不折(保守可接受);若未来放开,可用"常量链折叠读址 == 原程序运行时读址"的论证——布局元数据已含步长,机制上只差归一一步。
4. **业务侧分派不变**:`void*` 参数改类型化结构体指针 + 标 bound;双重间接数据扁平化或中间指针标注化;slot 字段语义窗口常量则补 may_const 标注。
5. **验证方向**:板上六站点 may_const 是否消失;同对象常量下标是否随之折叠;端到端贯通性(应用注册 bounds/回调 → 特化 → 折叠)。机制本身的单测/lit 已随 PR #249 覆盖,增量验证聚焦站点级与部署级。
