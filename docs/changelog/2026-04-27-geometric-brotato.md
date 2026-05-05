# CHANGELOG Fragment — 几何土豆兄弟

## [2026-04-27] — v0.5.0 — 联机几何土豆兄弟 MVP

> **核心主题**：彻底移除占位素材，纯几何体角色 + 自动索敌 + 定时开火 + 激光表现，打通联机割草闭环。

### 🔧 REFACTOR: 角色视觉几何化

`CampPlayer.cs`：
- **清理**：删除旧版头/身/腿/武器等多部件拼装，统一为 40x40 方块
- **颜色**：本地玩家天蓝 `#4A90E2`，远程玩家/ NPC 番茄红 `#FF6B6B`
- **血条**：40x4 细条，位于头顶，圆角无背景

### 🆕 NEW: 自动索敌雷达

`CampPlayer._Ready()`：
- `Area2D` + `CircleShape2D` 半径 250，挂在本地玩家的节点树下
- `Timer` 0.5 秒循环定时器，`Autostart = true`

`OnAttackTimerTimeout()`：
- `_radar.GetOverlappingBodies()` 获取所有碰撞体重叠的 `CampPlayer`
- 排除自己 + 排除已死亡，找最近目标
- 发包 `SendAttack(targetId, 1)` + 本地激光表现

### 🆕 NEW: Line2D 激光

`SpawnLaser(ulong targetId)`：
- 创建 `Line2D` 节点：起点=本地玩家中心，终点=目标中心
- 亮黄色 `#FFF01A`，宽度 4，抗锯齿
- `Tween` 在 0.2s 内宽度从 4→0，然后 `QueueFree`

### 🆕 NEW: 受击闪红特效

`TakeDamage()`：
- 方块瞬间变白 → 0.1s Tween 恢复原色
- 同步更新血条、伤害飘字、死亡处理

### 🔧 FIX: 攻击范围匹配雷达

`scene_actor.cpp`：`skill_range = 200.0f`（容忍 `×1.2 = 240`），覆盖客户端雷达 250 半径。
