---
version: alpha
name: AegisEngine — 战斗组件框架设计方案
description: 基于 DESIGN.md 第三章的数据驱动轻量化组件架构，设计 HealthComponent、SkillComponent、BuffComponent 的详细接口和对接方案。
---

## 一、设计目标

1. 所有伤害/回血/Buff 走统一入口，不再零散实现
2. 技能和 Buff 用 proto 配置，加新技能不改 C++ 代码
3. PlayerActor 和 NpcActor 共用同一套组件
4. 最小可行：支持「攻击→伤害→Buff→死亡」完整流程

## 二、组件架构

```
Entity（PlayerActor / NpcActor）
├── HealthComponent
│   ├── 属性：max_hp, current_hp, is_dead
│   ├── 方法：TakeDamage(), Heal(), IsDead(), GetHp(), GetMaxHp()
│   └── 事件：OnHpChanged(int32_t old_hp, int32_t new_hp)
│
├── SkillComponent
│   ├── 属性：技能表、冷却时间
│   ├── 方法：CanCast(), CastSkill(), TickCooldowns()
│   └── 配置：SkillConfig（proto）
│
└── BuffComponent
    ├── 属性：活跃 Buff 列表
    ├── 方法：AddBuff(), RemoveBuff(), TickBuffs(), GetAttributeMod()
    └── 配置：BuffConfig（proto）
```

## 三、Proto 配置定义

新增 `shared/proto/config.proto`：

```proto
syntax = "proto3";
package aegis.config;

// 技能配置
message SkillConfig {
    uint32 skill_id = 1;
    string name = 2;
    int32 base_damage = 3;          // 基础伤害
    float cooldown = 4;             // 冷却时间（秒）
    float range = 5;                // 攻击范围
    uint32 target_type = 6;         // 0=任意, 1=敌方, 2=友方
    repeated BuffApply self_buffs = 7;    // 对自己施加的 buff
    repeated BuffApply target_buffs = 8;  // 对目标施加的 buff
}

// Buff 应用（技能配置的子字段）
message BuffApply {
    uint32 buff_id = 1;
    float duration = 2;             // 持续时间（秒）
    map<string, float> attribute_mods = 3; // 属性修正
}

// Buff 定义
message BuffConfig {
    uint32 buff_id = 1;
    string name = 2;
    float duration = 3;             // 默认持续时间
    bool is_debuff = 4;             // 是否为负面效果
    map<string, float> attribute_mods = 5; // 属性修正
}
```

## 四、C++ 组件实现

### HealthComponent

```cpp
// include/aegis/game/components/health_component.h
class HealthComponent {
public:
    HealthComponent(int32_t max_hp);

    // 核心逻辑
    void TakeDamage(int32_t damage, uint64_t attacker_id);
    void Heal(int32_t amount);

    // 查询
    bool IsDead() const { return is_dead_; }
    int32_t GetHp() const { return current_hp_; }
    int32_t GetMaxHp() const { return max_hp_; }
    float GetHpPercent() const;

    // 回调（由外部设置，用于广播 SCDamageNtf 等）
    std::function<void(int32_t old_hp, int32_t new_hp, uint64_t attacker_id)> OnHpChanged;

    // 重置（对象池复用）
    void Reset(int32_t max_hp);

private:
    int32_t max_hp_;
    int32_t current_hp_;
    bool is_dead_ = false;
};
```

### SkillComponent

```cpp
// include/aegis/game/components/skill_component.h
class SkillComponent {
public:
    SkillComponent();

    // 加载技能配置（启动时从 proto 文件读取）
    void LoadSkills(const std::vector<SkillConfig> &skills);

    // 查询
    bool CanCast(uint32_t skill_id, int64_t now) const;
    const SkillConfig* GetSkill(uint32_t skill_id) const;

    // 释放技能（返回伤害数值，0 表示失败）
    struct CastResult {
        bool success = false;
        int32_t damage = 0;
        std::vector<BuffApply> self_buffs;
        std::vector<BuffApply> target_buffs;
    };
    CastResult CastSkill(uint32_t skill_id, int64_t now);

    // Tick 冷却
    void TickCooldowns(int64_t now);

    // 重置
    void Reset();

private:
    std::unordered_map<uint32_t, SkillConfig> skill_configs_;
    std::unordered_map<uint32_t, int64_t> cooldowns_; // skill_id → 就绪时间戳(ms)
};
```

### BuffComponent

```cpp
// include/aegis/game/components/buff_component.h
struct ActiveBuff {
    uint32_t buff_id;
    int64_t end_time;      // 结束时间戳(ms)
    float duration;        // 原始持续时间
    std::unordered_map<std::string, float> attribute_mods;

    bool IsExpired(int64_t now) const { return now >= end_time; }
};

class BuffComponent {
public:
    BuffComponent();

    // 加载 Buff 定义
    void LoadBuffDefs(const std::vector<BuffConfig> &defs);

    // 添加 Buff
    void AddBuff(uint32_t buff_id, float duration, int64_t now);

    // 移除 Buff
    void RemoveBuff(uint32_t buff_id);

    // Tick（移除过期 Buff）
    void TickBuffs(int64_t now);

    // 查询属性修正值（示例：移速修正、伤害修正）
    float GetAttributeMod(const std::string &attr_name) const;

    // 是否有指定 Buff
    bool HasBuff(uint32_t buff_id) const;

    // 重置
    void Reset();

    // 回调
    std::function<void(uint32_t buff_id, bool added)> OnBuffChanged;

private:
    std::unordered_map<uint32_t, BuffConfig> buff_defs_;
    std::vector<ActiveBuff> active_buffs_;
};
```

## 五、对接方案

### 5.1 PlayerActor 对接

PlayerActor 持有三个组件实例：

```cpp
class PlayerActor : public PooledActor<PlayerActor> {
    // ... 现有逻辑 ...

    // 新增组件
    HealthComponent health_;
    SkillComponent skills_;
    BuffComponent buffs_;

    // 初始化时加载技能配置
    void InitCombat();
};
```

**TakeDamage 流程变更：**
1. `PlayerActor::TakeDamage()` 不再直接扣 `hp_`，改为：
   ```cpp
   void PlayerActor::TakeDamage(int32_t damage, uint64_t attacker_id) {
       health_.TakeDamage(damage, attacker_id);
   }
   ```
2. `HealthComponent::OnHpChanged` 回调由 SceneActor 设置，用于触发 `SCDamageNtf` 广播

### 5.2 NpcActor 对接

NpcActor 同样持有三个组件：

```cpp
class NpcActor : public PooledActor<NpcActor, 256, 64> {
    // ... 现有逻辑 ...

    HealthComponent health_{DefaultMaxHp};
    SkillComponent skills_;
    BuffComponent buffs_;
};
```

NPC 的 `TakeDamage()` 改为委托到 `health_`。

### 5.3 SceneActor 对接

`OnHandleSkillCast` 中的伤害计算改为：

```cpp
void SceneActor::OnHandleSkillCast(SceneSkillCastMsg *msg) {
    // 1. 获取施法者（Player 或 NPC）
    // 2. 获取目标（Player 或 NPC）
    // 3. 验证技能可用（skill_component->CanCast）
    // 4. 计算伤害（skill_component->CastSkill）
    // 5. 目标受到伤害（target_health.TakeDamage）
    // 6. 应用 Buff
    // 7. 广播 SCDamageNtf
    // 8. 如果目标死亡，触发死亡逻辑
}
```

### 5.4 配置加载时机

服务端启动时，从 `config/` 目录加载所有 `SkillConfig` 和 `BuffConfig`（用 protobuf text format 或二进制）。

```cpp
// 在 GameApp::init() 中加载
auto skill_configs = LoadSkillConfigs("config/skills.bin");
auto buff_configs = LoadBuffConfigs("config/buffs.bin");

// 当 PlayerActor 或 NpcActor 创建时，调用 InitCombat() 传入配置
```

## 六、工作量与优先级

| 步骤 | 内容 | 预计时间 |
|------|------|----------|
| 1 | 定义 config.proto | 30min |
| 2 | 实现 HealthComponent | 1h |
| 3 | 实现 SkillComponent | 1.5h |
| 4 | 实现 BuffComponent | 1h |
| 5 | PlayerActor 对接 | 1h |
| 6 | NpcActor 对接 | 0.5h |
| 7 | SceneActor 对接（重写 OnHandleSkillCast） | 1h |
| 8 | 编译+联调 | 1h |
| 总计 | | ~7.5h |

## 七、遗留问题（本轮不处理）

1. 属性系统（攻击力、防御力、暴击率等）——当前硬编码伤害值
2. 多技能切换——当前只有技能 1（普攻）
3. 技能动画/特效——客户端播放
4. 掉落系统——怪物死亡后掉物品
5. 技能升级系统
