# CHANGELOG — AegisEngine 更新日志

> 格式：[日期] · [分类: 新增/重构/修复/测试] · 简述
>
> 分类标记:
> - 🆕 **NEW** — 新增功能
> - 🔧 **REFACTOR** — 重构改进
> - 🐛 **FIX** — Bug 修复
> - 🧪 **TEST** — 测试适配
> - 📝 **DOC** — 文档更新

---

## [2026-04-27] — v0.2.0 — AOI 重构 + 坐标系统一 + 同步修复

> **核心主题**：将 AOI 网格从仅支持 `[0, max]` 的正坐标范围，重构为支持任意 `[min, max]` 的浮点坐标范围。同步统一客户端-服务端出生点，修复方向数据丢失导致的鬼畜转头。

### 🔧 REFACTOR: AOIGrid 支持负数坐标

**动机**：ARPG 的地图逻辑中心应当在 `(0,0)`，而非角落。旧版 `getIndexUnsafe()` 用 `std::clamp` 把负数钳到 0，导致玩家走入地图左/下边界时 AOI 计算完全失效。

**变更**：

- **构造函数签名**：`(width, height, cellSize)` → `(minX, minY, maxX, maxY, cellSize)`
- **新增字段**：
  - `minX_ / minY_` — 地图最小坐标
  - `offsetX_ / offsetY_` — 内部偏移（`-minX` / `-minY`），将世界坐标映射到非负索引空间
  - `width_ / height_` — 不再直接传参，由 `max - min` 计算得出
- **`getIndexUnsafe()` 行为变更**：
  - 先应用偏移 `tx = x + offsetX_`，再做非负判断
  - 越界坐标**不再 clamp**，统一返回 `(uint32_t)-1`（为 0 的语义由调用方处理）
- **`isValidPos()`** 同步更新为偏移坐标系判断
- **`reset()`** 同步改签名

**地图配置变更**：
| 参数 | 旧值 | 新值 |
|------|------|------|
| `map_width` | 500.0f | 2000.0f |
| `map_height` | 500.0f | 2000.0f |
| `cell_size` | 10.0f | 256.0f |
| 逻辑范围 | `[0, 500]` | `[-1000, 1000]` |
| 网格数 | 50×50 = 2500 | 8×8 = **64** |
| 9宫格物理跨度 | 30×30 | **768×768** |

### 🔧 REFACTOR: SceneActor 适配新签名

- 构造/destroy 签名同步改为 `(minX, minY, maxX, maxY, cellSize)`
- RoomManager 中 2 处创建 SceneActor 的硬编码值同步更新

### 🐛 FIX: 出生点统一

| 场景 | 旧坐标 | 新坐标 |
|------|--------|--------|
| Login（主城） | `(250, 250)` | `(0, 0)` |
| 创建营地 | `player->GetX/Y()`（门把手传递） | `(0, 0)` |
| 加入营地 | `player->GetX/Y()` | `(0, 0)` |
| 客户端 CampScene | `Vector2.Zero` | `Vector2.Zero`（未改变） |
| 服务端 NPC 出生 | `map_width*0.5f` | `0.0f` |

### 🐛 FIX: 去除 MoveHandler 重复 SetPos — Dirty Flag 冗余

**问题**：`handler_loader.cpp` 的 Move 处理器中，Gate 线程先执行了 `player->SetPos(newX, newY)` **然后又** 将 `SceneMoveMsg` 投递到 SceneActor，后者在 `OnHandleMove()` 再次执行 `SetPos()`。这违反了 Actor 模型的**状态变更只能在自己的线程上下文**执行的铁律。

**修复**：删除 MoveHandler 中的 `player->SetPos(...)` 调用。SceneActor::OnHandleMove 仍然是唯一执行坐标状态变更的地方。

### 🐛 FIX: SyncManager direction 字段 — 鬼畜转头

**问题**：`SyncManager::Tick()` 中 `move_info->set_direction(0.0f)` 写死了方向为 0，导致客户端收到方向永远指向右侧，插值时发生「鬼畜转头」。

**修复**：改为 `mover->GetDir()`，读取 PlayerActor 存储的实际朝向角。

### 🆕 NEW: PlayerActor 方向角字段

- 新增 `direction_` 字段（float，弧度，atan2 语义）
- `SetMoveState()` 新增 `float direction` 参数
- 新增 `GetDir()` public 接口
- `reset()` 中将 `direction_` 初始化为 0.0f

### 🧪 TEST: 适配新地图配置

所有依赖 AOIGrid 旧签名的测试文件已更新：

| 文件 | 旧值 | 新值 |
|------|------|------|
| `tests/unit/test_sync.cpp` | `AOIGrid(500,500,10)` | `AOIGrid(0,0,500,500,10)` |
| `tests/unit/test_ai.cpp` | `AOIGrid(100,100,10)` | `AOIGrid(-1000,-1000,1000,1000,256)` |
| `tests/unit/test_room_manager.cpp` | `(500,500,10)` | `(-1000,-1000,1000,1000,256)` |
| `tests/integration/test_architecture.cpp` | `(500,500,10)` | `(-1000,-1000,1000,1000,256)` |
| `tests/benchmark/bm_aoi_grid.cpp` | `AOIGrid(1000,1000,30)` | `AOIGrid(0,0,1000,1000,30)` |

AI 测试坐标同步迁移到新地图范围（NPC `500,500`、Player `-500,500` 等保证 9 宫格外/内测试有效性）。

---

## ANCHOR: 更新日志模板

```markdown
## [YYYY-MM-DD] — vX.Y.Z — 标题

> 简短描述

### 🆕 NEW: 标题

**动机**：
**变更**：
**风险**：

### 🔧 REFACTOR: 标题

**动机**：
**变更**：
**影响范围**：

### 🐛 FIX: 标题

**问题**：
**根因**：
**修复**：

### 🧪 TEST: 标题
```
