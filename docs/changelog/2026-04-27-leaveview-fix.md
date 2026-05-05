# CHANGELOG Fragment — LeaveView 修复 + 滑步问题

## [2026-04-27] — v0.2.1 — 客户端 LeaveView 处理 + 滑步修复

> **核心主题**：客户端补上 SC_LEAVE_VIEW 消息处理，远程玩家离开视野时销毁节点，消除"滑步 + 拽回"现象。

### 🐛 FIX: 客户端未处理 LeaveView — 远程玩家永不销毁

**问题**：服务端在 AOI 跨格时正确发送 `SC_LEAVE_VIEW` 包，但客户端 `GameManager` 的消息路由中完全没有处理 `ScLeaveView (MsgID=2006)`，导致：

1. 远程玩家离开视野后客户端仍然保留其 `CampPlayer` 节点
2. 客户端在无新数据时继续按最后速度 Dead Reckoning（滑步）
3. 服务端仍向该"已离开"的接收者发 `SC_MOVE_NTF`，客户端收到后强制拽回（瞬移）
4. 形成「滑步 → 拽回 → 滑步」的循环

**修复**：客户端两处改动

**`GameManager.cs`：**
- 新增 `OnLeaveView` 事件：`Action<ulong>`
- 新增 `HandleLeaveView()` 反序列化并触发事件
- `OnClientMessage()` switch 中添加 `MsgID.ScLeaveView` 路由

**`CampScene.cs`：**
- `_Ready()` 中订阅 `gm.OnLeaveView += OnLeaveView`
- 新增 `OnLeaveView()` → CallDeferred → `HandleLeaveView()`
- `HandleLeaveView()` 实现：
  1. 从 `_players` 字典查找
  2. `RemoveChild` + `QueueFree` 释放节点
  3. 从字典中移除
- `_ExitTree()` 中反订阅
