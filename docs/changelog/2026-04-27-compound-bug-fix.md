# CHANGELOG Fragment — EnterView 信息补全 + 场景切换消息丢失修复 + 远程玩家碰撞禁用

## [2026-04-27] — v0.3.0 — 复合 Bug 修复

> **核心主题**：修复"后进入看不到已有实例"、"进入视野从原点瞬移"、"模型互推"三个复合 bug。

### 🐛 FIX: 场景切换期间 EnterView 消息丢失 → 后进入者看不到已有玩家

**问题**：客户端创建/加入营地后，`ChangeSceneToFile` 异步切换场景。服务端在 `SceneActor::OnHandleEnter` 中立即发送了 `SC_ENTER_VIEW`（包含周围玩家的快照）。但此时 `CampScene` 还在加载中，`OnEnterView` 事件尚未订阅，导致消息丢失。

表现：第二个进入营地的玩家永远看不到第一个玩家，直到双方移动触发 AOI 跨格重新触发 Enter/Leave。

**修复**——两端配合：

**`GameManager.cs`**：
- 新增 `_pendingEnterView` 缓存字段
- `HandleEnterView()` 中先缓存到 `_pendingEnterView`，再触发事件
- 新增 `ConsumePendingEnterView()` 方法供场景在初始化完成后拉取缓存

**`CampScene.cs`**：
- `_Ready()` 末尾调用 `gm.ConsumePendingEnterView()`
- 此时场景已加载完毕、事件已订阅、本地玩家已创建，可安全消费

### 🐛 FIX: EnterView 不携带速度/方向 → 进入视野时从原点瞬移

**问题**：`EntityViewInfo` struct 只有 `uid + x + y + entity_type`，没有 `direction/speed/is_moving`。客户端创建远程玩家后，只能等第一包 `SC_MOVE_NTF` 才触发插值，期间玩家显示在 (0,0) 或错误位置。

**修复**·服务端：
- `common.proto` —— `PBPlayerInfo` 新增 `speed(9)` 和 `is_moving(10)` 字段
- `EntityViewInfo` —— 新增 `direction/speed/is_moving` 字段（带默认值）
- `packet_builder.cpp` —— `BuildEnterView` 序列化完整状态
- `scene_actor.cpp` —— `OnHandleEnter` 和 `ProcessAoiEnterLeave` 中构造 `EntityViewInfo` 时实时读取玩家移动状态

**修复**·客户端：
- `CampScene.HandleEnterView()` —— 创建远程玩家后立即调用 `SyncPosition()`，传入速度/方向/移动状态
- 消除"先出现在 0,0 → 下一帧瞬移到目标位置"的窗口

### 🐛 FIX: 远程玩家物理碰撞阻挡本地玩家 → 模型互推

**问题**：`CampPlayer` 是 `CharacterBody2D`，`CollisionLayer = 1` 且 `CollisionMask = 1`。远程玩家的 `CollisionShape2D` 虽不参与 `MoveAndSlide()`，但本地玩家的 `MoveAndSlide()` 会被远程玩家的碰撞体阻挡，导致"推不动"。

**修复**：
```csharp
CollisionLayer = _isLocalPlayer ? 1u : 0u;
CollisionMask = _isLocalPlayer ? 1u : 0u;
```
- 本地玩家：Layer=1, Mask=1，正常物理碰撞
- 远程玩家：Layer=0, Mask=0，不参与任何碰撞
- 客户端 proto 重新生成（`Common.cs/CsBattle.cs/CsLobby.cs/Ids.cs`）
