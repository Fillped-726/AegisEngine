---
title: AegisEngine ECS 重构计划
date: 2026-04-29
status: draft
supersedes: docs/archived/combat-component-design.md
---

# ECS 重构计划：Player/NPC 去 Actor 化

## 一、为什么做

### 当前问题

**NpcActor 是披着 Actor 外衣的纯数据结构。**

```cpp
// npc_actor.cpp L51-53:
void NpcActor::handle_message(ActorMessage *msg) {
    // 空实现——从不接收消息
}
```

NpcActor 继承了 Actor 的完整基础设施（MPSC 队列、调度槽位、ActorRegistry 注册、ObjectPool 分配），但实际用到的只有 `x/y/hp/is_dead/bt_tree`。这是开销浪费：

- 每个 NPC 浪费：`head_`(8B) + `tail_`(8B) + `is_scheduled_`(8B) + 缓存行对齐填充 ≈ 64B+ 的 Actor 元数据
- ActorRegistry 容量 65536，NPC 多时会挤占 Player 的槽位
- `unordered_map<uint64_t, NpcActor*>` 散落堆内存，遍历 NPC 是 Cache Miss

**战斗逻辑的 if-else 膨胀。**

`OnHandleSkillCast` 和 `OnHandleEnter` 等函数里反复判断 target/caster 是 Player 还是 NPC，后续加宠物/召唤物/中立单位会指数级恶化。

**PlayerActor 的问题类似但程度不同。**

PlayerActor 确实在用 Actor 的完整能力（消息队列、网络连接、dispatch_msg），但它是**唯一需要这些能力**的实体。把 Player 和 NPC 放在同一个继承体系里，反而限制了各自的发展。

### ECS 方案的核心变化

```
当前:     PlayerActor(Actor)    NpcActor(Actor)
              │                      │
              └────── SceneActor ─────┘
                    (两个 unordered_map + 双路 if-else)

ECS:      SceneActor (唯一 Actor, 持有所有逻辑)
              │
              ├─ PlayerEntities[]  (纯数据组件)
              ├─ NpcEntities[]     (纯数据组件)
              ├─ ConnectionMap     (uid → Connection, 独立管理)
              └─ Systems:
                   ├─ CombatSystem  (统一处理伤害/治疗/Buff)
                   ├─ MoveSystem    (位置更新 + AOI)
                   ├─ AISystem      (NPC 行为树驱动)
                   └─ SyncSystem    (DirtyFlag → 序列化 → 广播)
```

收益：
1. 消除 NpcActor 的 Actor 开销
2. 战斗逻辑不再区分 Player/NPC
3. 内存连续布局（Cache 友好）
4. 按 Tick 遍历实体，不依赖 Actor 调度

## 二、新架构总览

### 2.1 Entity 定义

```cpp
// Entity 是轻量级 ID，不包含数据
using EntityId = uint64_t;

// 组件：纯 POD，无虚函数，无继承
struct HealthComponent {
    int32_t max_hp;
    int32_t current_hp;
    bool is_dead = false;
};

struct PositionComponent {
    float x, y;
    float speed = 0.0f;
    float direction = 0.0f;
    bool is_moving = false;
};

struct PlayerTag {
    // 标记型组件，无数据
};

struct NpcTag {
    int32_t npc_type_id;  // → 查 ConfigManager
    bool ai_active = false;
    // BT::Tree 不连续存储，用单独的 vector<unique_ptr<BT::Tree>>
};

// 即将到来的战斗组件（后续实现）
// struct SkillComponent { ... };
// struct BuffComponent { ... };
```

### 2.2 SceneActor 持有 SOA

```cpp
class SceneActor : public PooledActor<SceneActor, 128, 32> {
    // 不再用两个 unordered_map
    // Player 用 vector (固定数量，随机访问为主)
    // HP 和 Pos 分开存储，System 只遍历自己需要的字段

    // --- Player 数据 (连续内存) ---
    std::vector<EntityId> player_ids_;

    // 组件分离存储 (SOA 布局)
    // 一个 System 只遍历自己需要的组件，不污染 Cache
    std::vector<HealthComponent> player_health_;
    std::vector<PositionComponent> player_pos_;

    // --- NPC 数据 (连续内存) ---
    std::vector<EntityId> npc_ids_;
    std::vector<HealthComponent> npc_health_;
    std::vector<PositionComponent> npc_pos_;
    std::vector<NpcTag> npc_tags_;
    // BT::Tree 放在单独的 vector 里（非 POD，需要 unique_ptr）
    std::vector<std::unique_ptr<BT::Tree>> npc_bt_trees_;

    // --- 网络连接 (独立管理，不在 Entity 上) ---
    std::unordered_map<uint64_t, std::shared_ptr<net::Connection>> connections_;
    // 快速反向查找: EntityId → Connection
    // Entity 退出场景时，Connection 由 SceneActor 持有
};
```

### 2.3 Systems

```cpp
// 所有 System 都是 SceneActor 的私有方法，在 OnTick() 中按序调用

void SceneActor::SystemCombat(EntityId caster_id, EntityId target_id, uint32_t skill_id);
// 统一接口，不区分 Player/NPC

void SceneActor::SystemMove(EntityId eid, float nx, float ny, float dir, float speed);
// 更新位置 + AOI 跨格检测

void SceneActor::SystemSync();
// 遍历所有脏实体，序列化 + AOI 广播

void SceneActor::SystemAI();
// 遍历 npc_ai_active_ = true 的 NPC，驱动 BT tick

void SceneActor::SystemCleanup();
// 统一处理死亡实体（回收资源前，确认没有后续引用）
```

## 三、改动清单

### Step 1: 新建组件数据结构

**文件**：`include/aegis/game/components/health_component.h`
          `include/aegis/game/components/position_component.h`
          `include/aegis/game/components/entity_tags.h`

内容：POD 结构体，无方法（或只有简单的 getter/inline 方法）。

BT 相关：NpcActor 持有 BT::Tree 的方式需要保留，因为 BT.CPP 的 Tree 对象不能简单搬进 vector。用 `vector<unique_ptr<BT::Tree>>` 或者 NPC 的 bt_tree 按 id 索引。

### Step 2: 改造 SceneActor

**文件**：`include/aegis/game/scene_actor.h` + `src/game/scene_actor.cpp`

变化：
- 移除 `unordered_map<uint64_t, PlayerActor*> actors_` 和 `unordered_map<uint64_t, NpcActor*> npcs_`
- 新增 `player_ids_ / player_health_ / player_pos_` 等 SOA 字段
- 新增 `npc_ids_ / npc_health_ / npc_pos_ / npc_tags_ / npc_bt_trees_` 等 SOA 字段
- 新增 `connections_` map（EntityId → Connection）
- 新增 `EntityId AddPlayer(std::shared_ptr<Connection>, float x, float y);`
- 新增 `EntityId AddNpc(float x, float y, int32_t npc_type_id);`
- 新增 `void RemoveEntity(EntityId eid);`

**关键接口变更**：
- 移除 `GetPlayer(uint64_t actorId)`（外部依赖方：SyncManager, ai_nodes.cpp 的 FindPlayerNode/MoveTowardsNode）
- 新增 `Entity* GetEntity(uint64_t eid)` 返回 a entity 指针？不——SOA 下没有"实体对象"。改成：
  ```cpp
  // 查询接口通过 EntityId 返回组件引用
  HealthComponent* GetHealth(EntityId eid);
  PositionComponent* GetPosition(EntityId eid);
  ```
  或者直接让 Systems 方法做各种操作。

### Step 3: 改造 SyncManager

**文件**：`include/aegis/game/sync_manager.h`

当前 SyncManager 模板方法接受 `PlayerActor*` 列表。改造后：
- SyncManager 直接接受 PositionComponent 引用和 DirtyFlag，不再依赖 PlayerActor 类型
- 或者 SyncManager 逻辑直接内联到 SceneActor::SystemSync 中

选择：**SyncManager 内联到 SceneActor**。因为 SOA 后 SyncManager 的逻辑变得极其简单（遍历 player_pos_ 的 DirtyFlag+序列化），单独类的收益不大。SyncManager 取消。

### Step 4: 移除 NpcActor

**文件**：删除 `include/aegis/game/npc_actor.h` + `src/game/npc_actor.cpp`

NpcActor 的能力全部移到 SceneActor 的 SystemAI 中：
- 行为树创建（BT::BehaviorTreeFactory 单例保留）
- `OnTick()` 逻辑移到 `SceneActor::SystemAI()`
- `TakeDamage()` 移到 `CombatSystem` 里

### Step 5: 改造 PlayerActor

**文件**：`include/aegis/game/playerActor.h` + `handler_loader.cpp` + 所有引用处

PlayerActor 不再继承 `PooledActor<PlayerActor>`。
- 移除 Actor 基类：不再有 handle_message、消息队列、ActorRegistry 注册
- 移除 ObjectPool：不再通过 `PlayerActor::create()` 创建
- 保留 `send_packet` / `send_buffer`：但需要 Connection 引用

**PlayerActor 的新形态**：

```cpp
// PlayerActor 降级为 SceneActor 内的 Entity 表示
// 不再有独立的 Actor 文件

// 在 SceneActor 内部:
EntityId SceneActor::AddPlayer(
    std::shared_ptr<net::Connection> conn,
    float x, float y)
{
    EntityId eid = next_entity_id_++;
    player_ids_.push_back(eid);
    player_health_.push_back(HealthComponent{100, 100, false});
    player_pos_.push_back(PositionComponent{x, y, 0, 0, false});
    connections_[eid] = std::move(conn);

    // EntityId → index 映射，用于快速查找
    player_index_[eid] = player_ids_.size() - 1;
    return eid;
}
```

**消息路由的变更**：

当前流程：
```
Client → Dispatcher → PlayerActor::handle_message → dispatch_msg(SceneActor)
```

改造后：
```
Client → Dispatcher → ??? (不再有 PlayerActor::handle_message)
```

解决方案：
- Dispatcher 注册的 handler 里，`Actor*` 参数原来是 PlayerActor，现在变成 SceneActor
- 但 Dispatcher 当前的设计是：handler(actor, proto_msg)，actor 是收到消息的 Actor
- 网络消息目前是 GateServer 收到后路由到 PlayerActor 的
- **改造方案**：消息到场景的流转需要重新设计

这部分的详细方案见下文"4.3 消息路由改造"。

### Step 6: 改造 AI Nodes

**文件**：`include/aegis/game/ai_nodes.h` + `src/game/ai_nodes.cpp`

当前 BT 节点通过 blackboard 拿到 `NpcActor*` 和 `SceneActor*`。改造后：
- blackboard 存 `(EntityId, SceneActor*)` 而不是 `NpcActor*`
- BT 节点通过 SceneActor 的查询接口操作 Entity
- `FindPlayerNode`：遍历 `scene_->player_pos_` 查最近玩家
- `MoveTowardsNode`：通过 `scene_->GetPosition(eid)->x/y` 移动
- `AttackPlayerNode`：调用 `scene_->SystemCombat(...)`

### Step 7: 改造 AiNodes 的 AttackPlayerNode 中的 static 问题

当前代码 L151 有一个 `static auto last_attack_time`——多个 NPC 共享同一个 CD 变量。这是一个 bug（所有 NPC 共享攻击 CD），重构时一并修复：把 CD 移到 NpcTag 或单独的 AIStateComponent。

### Step 8: 改造 GameApp 和创建流程

当前：
```cpp
auto *player = PlayerActor::create(id, conn);
// ...
auto *scene = registry.create_actor<SceneActor>(...);
// ...
dispatch_msg(scene, new SceneEnterMsg(player->id(), ...));
```

改造后：
```cpp
// 在 login handler 中
EntityId eid = scene->AddPlayer(conn, spawn_x, spawn_y);
// 直接通知 scene，不再需要 SceneEnterMsg
```

创建逻辑简化，因为 Entity 不是 Actor，不需要 `dispatch_msg`。

## 四、关键设计决策

### 4.1 SOA vs AOS

选择 SOA（Struct of Arrays）而不是 AOS（Array of Structs）：

```cpp
// AOS（传统 ECS）
struct Entity {
    HealthComponent health;
    PositionComponent pos;
    PlayerTag tag;
};
std::vector<Entity> entities;

// SOA（本方案）
std::vector<HealthComponent> player_health_;
std::vector<PositionComponent> player_pos_;
```

理由：
- CombatSystem 只遍历 health_，不会污染 Cache 加载不需要的 pos 或 bt_tree
- AI 系统只遍历 npc_tags_ 和 npc_bt_trees_
- 实际遍历的是视线范围（AOI 可见实体），不是全量，SOA 优势不如全量 ECS 大，但零成本

### 4.2 EntityId 的设计

```cpp
using EntityId = uint64_t;

// EntityId 的低 32 位是 serial number（递增分配）
// 高 32 位是 generation（防悬挂引用）
// 或者直接用递增 ID，场景内保证唯一
```

当前 ActorID 的 `uint64_t raw` 可以复用，EntityId 保持与 ActorID 的 `raw` 字段兼容。
这样 protobuf 中的 `entity_id` 字段不用改类型。

EntityId 由 SceneActor 内部递增分配，不再经过 ActorRegistry。

### 4.3 消息路由改造（最关键的部分）

当前 Dispatcher 注册的 handler 签名：
```cpp
register_handler<CSSkillCastReq>(ids::CS_SKILL_CAST_REQ,
    [](Actor *actor, const CSSkillCastReq &req) -> Task<void> {
        auto player = static_cast<PlayerActor*>(actor);
        // actor 是 PlayerActor
    });
```

actor 参数来自网络层：GateServer 收到包后，根据 fd 找到对应的 PlayerActor，然后调用 `player->push(pkt)`，最终 `PlayerActor::handle_message` 回调 Dispatcher。

改造方案：

**方案 A：Dispatcher 直接路由到 SceneActor**

handler 不再收到 PlayerActor*，而是收到 SceneActor* + EntityId。

```cpp
// 方案 A1: 在消息原型中附带 EntityId
message CSSkillCastReq {
    // 不再需要，因为 SceneActor 知道 EntityId
    uint64_t entity_id = 1; // SceneActor 填充
    uint32_t skill_id = 2;
    uint64_t target_id = 3;
    // ...
}

// handler 中:
register_handler<CSSkillCastReq>(..., [](Actor *actor, const CSSkillCastReq &req) {
    auto *scene = static_cast<SceneActor*>(actor);
    scene->SystemCombat(req.entity_id(), req.target_id(), req.skill_id());
});
```

问题：网络包到达 GateServer，它需要把包发给哪个 Actor？目前的机制是 fd → PlayerActor。改造后需要 fd → SceneActor（或者 GateServer 多维护一层 fd → EntityId → SceneActor 的映射）。

**方案 B：SceneActor 持有 Connection，GateServer 直接路由到 SceneActor**

GateServer 维护 `fd → (scene_actor_id, entity_id)` 映射。
收到消息后直接 `dispatch_msg(scene, pkt)`，SceneActor 的 handle_message 解析后找 entity_id。

**推荐方案 B**，因为它对 Dispatcher 的改动最小。Dispatcher 注册的 handler 不变，只是 actor 类型从 PlayerActor 变成 SceneActor，handler 内部通过 entity_id 定位。

需要：
1. SceneActor 增加 `handle_message` 中对网络消息的处理（当前只有场景消息）
2. GateServer 的会话层：在玩家登录/进入场景时，记录 `fd → (SceneActorID, EntityId)`
3. 每个 protobuf 消息加上 entity_id 字段，SceneActor 用来确定是哪个 Entity 的操作

### 4.4 死亡与清理

ECS 方案自然地解决了"A 打死 B，C 的火球接着打 B"的问题：

```cpp
void SceneActor::CombatSystem(EntityId caster_eid, EntityId target_eid, uint32_t skill_id) {
    auto *health = GetHealth(target_eid);
    if (!health || health->is_dead) return; // 已经死了，忽略

    health->current_hp -= damage;
    if (health->current_hp <= 0) {
        health->is_dead = true;
        // 只标记，不移除
    }
}

void SceneActor::SystemCleanup() {
    // Tick 末尾统一清理
    // 遍历所有玩家，回收死亡的
    for (size_t i = 0; i < player_ids_.size(); ) {
        if (player_health_[i].is_dead) {
            RemovePlayerAt(i);
            continue;
        }
        i++;
    }
    // 同样处理 NPC
}
```

期间任何其他 System 访问到死亡实体，通过 `is_dead` 判断即可，不会 Crash。

### 4.5 行为树与 SOA 的适配

BT::Tree 不是 trivially copyable/relocatable，不能放 `vector<BT::Tree>`。方案：

```cpp
// NPC 的 bt_tree 与 Component 分开存储
// 按 NPC 在 npc_ids_ 中的 index 索引
std::vector<std::unique_ptr<BT::Tree>> npc_bt_trees_;

void SceneActor::SystemAI() {
    for (size_t i = 0; i < npc_ids_.size(); i++) {
        if (!npc_tags_[i].ai_active) continue;
        if (npc_health_[i].is_dead) continue;

        // 更新 blackboard 中的 npc_index（代替 NpcActor*）
        auto &tree = *npc_bt_trees_[i];
        tree.blackboard()->set<size_t>("npc_index", i);
        tree.tickExactlyOnce();
    }
}
```

或者用 parallel array：npc_bt_trees_ 长度和 npc_ids_ 一致，`npc_bt_trees_[i]` 对应 `npc_ids_[i]` 的 BT。

### 4.6 写入 Proto（原 PlayerActor::WriteToProto）

当前 PlayerActor 有 `WriteToProto(PBPlayerInfo*)` 方法。ECS 方案下：

```cpp
// SceneActor 提供
void SceneActor::WriteEntityToProto(EntityId eid, PBPlayerInfo* proto) {
    auto idx = GetPlayerIndex(eid);  // O(1) via hash map
    proto->set_entity_id(eid);
    proto->mutable_pos()->set_x(player_pos_[idx].x);
    proto->mutable_pos()->set_y(player_pos_[idx].y);
    proto->set_hp(player_health_[idx].current_hp);
    // ...
}
```

删除 PlayerActor.h 中的 `WriteToProto` 和 `send_packet` / `send_buffer`（这些移到 SceneActor，或通过 ConnectionMap 提供）。

## 五、工作量与步骤

### Phase 1: 数据结构 + SceneActor 改造（核心）

| 步骤 | 内容 | 涉及文件 |
|------|------|----------|
| 1.1 | 创建 Component 头文件（Health/Position/Tags） | 3 new files |
| 1.2 | SceneActor 改为 SOA 结构 | scene_actor.h + .cpp |
| 1.3 | 添加 AddPlayer/AddNpc/RemoveEntity | scene_actor.cpp |
| 1.4 | 添加 SystemCombat/SystemMove/SystemSync/SystemAI/SystemCleanup | scene_actor.cpp |
| 1.5 | 修改 OnTick() 调用 Systems | scene_actor.cpp |
| 1.6 | 删除 NpcActor 文件 | delete 2 files |

### Phase 2: PlayerActor 改造 + 消息路由

| 步骤 | 内容 | 涉及文件 |
|------|------|----------|
| 2.1 | PlayerActor 去 Actor 化 | playerActor.h |
| 2.2 | GateServer 层添加 fd→SceneActor 映射 | gate_server 相关 |
| 2.3 | 改造 all handler（LoginReq, CreateCamp, Move, SkillCast 等） | handler_loader.cpp |
| 2.4 | SceneEnter 等消息类型改为直接调用 SceneActor 方法 | 删除 SceneEnterMsg 等 |

### Phase 3: AI + SyncManager 改造

| 步骤 | 内容 | 涉及文件 |
|------|------|----------|
| 3.1 | SyncManager 逻辑内联到 SceneActor::SystemSync | 删除 sync_manager.* |
| 3.2 | BT 节点改为通过 SceneActor 接口操作 | ai_nodes.h + .cpp |
| 3.3 | 修复 static CD bug | ai_nodes.cpp |

### Phase 4: GameApp + RoomManager 适配

| 步骤 | 内容 | 涉及文件 |
|------|------|----------|
| 4.1 | GameApp 创建流程改为 SceneActor.AddPlayer | game_app.cpp |
| 4.2 | RoomManager 中 SceneCreate/Dungeon 适配 | room_manager.cpp |

### Phase 5: 测试 + 清理

| 步骤 | 内容 |
|------|------|
| 5.1 | 更新单元测试 |
| 5.2 | 确认编译通过 |
| 5.3 | 联调验证移动/战斗/进入场景全流程 |
| 5.4 | 删除废弃文件：npc_actor.h/cpp, sync_manager.h/cpp, combat-component-design.md |

## 六、不处理（后续迭代）

- AttributeType 枚举化 + flat array（跟 ECS 正交，后续单独做）
- SkillComponent / BuffComponent 的具体实现（当前文档只搭 SOA 架子）
- ConfigManager 享元模式（独立优化项）

## 七、与废案的关系

原 `combat-component-design.md` 假设 PlayerActor/NpcActor 继承 Actor 并持有 Component。
本计划将其完全取代，原文档已移入 `docs/archived/`。
