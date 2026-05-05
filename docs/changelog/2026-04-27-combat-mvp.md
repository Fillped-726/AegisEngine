# CHANGELOG Fragment — MVP 战斗表现闭环

## [2026-04-27] — v0.4.0 — 战斗 MVP

> **核心主题**：打通从客户端攻击输入 → 服务端校验 + 伤害计算 → 客户端血条/飘字/死亡的完整闭环。

### 🆕 NEW: 客户端攻击输入

`CampPlayer.cs` `HandleAttackInput()`：
- 监听鼠标左键 / 空格键
- 0.5s 攻击 CD 防止连点
- 遍历同场景所有 `CampPlayer` 找到最近的作为目标
- 调用 `GameManager.SendAttack(targetId, skillId=1)`
- 服务端 `OnHandleSkillCast` 负责距离校验与伤害计算（现有逻辑不变）

### 🆕 NEW: SC_DAMAGE_NTF 消息处理

`GameManager.cs`：
- 新增 `OnPlayerTakeDamage` 事件：`Action<ulong targetId, ulong attackerId, int damage, int currentHp>`
- 新增 `SendAttack(ulong targetId, uint skillId)` 发送 `CS_SKILL_CAST_REQ`
- 消息路由注册 `MsgID.ScDamageNtf` → `HandleDamageNtf()` → 触发事件

### 🆕 NEW: 远程玩家血条显示与伤害飘字

`CampPlayer.cs`：
- `_Ready()` 中创建 HPBar（ProgressBar，宽30高4，在头顶）
- 血条颜色随血量渐变：绿 → 橙 → 红
- `TakeDamage()` 方法：更新 `_hpBar.Value`、颜色
- `SpawnDamageText(int damage)`：创建 Label，向上飘 40px + 渐隐 0.8s，然后 QueueFree

### 🆕 NEW: 死亡表现

`CampPlayer.cs` `Die()`：
- 隐藏 Sprite、Collision、HPBar、NameLabel
- 显示 💀 死亡图标
- `_isDead = true` → `_Process` 跳过所有逻辑

### 🆕 NEW: CampScene 伤害事件订阅

`CampScene.cs`：
- `_Ready` 订阅 `gm.OnPlayerTakeDamage += OnPlayerTakeDamage`
- `OnPlayerTakeDamage`：按 targetId 查找 `_players[targetId]` → `TakeDamage(damage, currentHp)`
- `_ExitTree` 反订阅

---

## [2026-04-27] — v0.4.1 — 攻击范围修复 + 视觉改进

### 🐛 FIX: 攻击范围过小（必须重合才能攻击）

**问题**：`OnHandleSkillCast` 中 `skill_range = 2.0f`，容忍 `×1.2` = `2.4` 单位。这是在旧地图 `cellSize=10` 时代设的值。现在玩家移动速度 200px/s，2.4 单位 ≈ 像素，几乎必须完全重合。

**修复**：`skill_range` 改为 `60.0f`，容忍后 `72.0` 单位，匹配近战视觉上的攻击距离。

### 🎨 REFACTOR: 玩家视觉改进

旧版：单一绿色/红色 ColorRect 方块 + 方向指示点。

新版：代码驱动的多部件角色视觉：
- **头部**：肤色矩形 + 眼睛（两点）
- **身体**：本地玩家绿色，远程玩家红色
- **腿**：深色小矩形 ×2
- **武器指示器**：短棍 + 尖端，挂在角色侧面，跟随朝向翻转
- **朝向翻转**：整个 Sprite 水平翻转 + 武器 pivot 挪到另一侧

### 📐 TWEAK: 头顶 HPBar 缩小

- 尺寸 `30×4` → `18×2`，带圆角
- 透明度调低 (`0.8 → 0.6`)，不抢眼

### 🔥 REMOVE: HUD 中不更新的 HPBar

左上角 HPBar 从未连接到血量数据，已移除。血量显示统一到每个 `CampPlayer` 头顶的细血条。
