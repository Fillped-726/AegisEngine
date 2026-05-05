// npc_actor.cpp
#include "aegis/game/npc_actor.h"
#include "aegis/game/ai_nodes.h" // 引入节点注册
#include "aegis/common/aegisLog.h"

namespace aegis::core
{
    // 定义一个静态工厂，避免每个 NPC 创建一次（节省开销）
    static BT::BehaviorTreeFactory *GetAiFactory()
    {
        static BT::BehaviorTreeFactory factory;
        static bool initialized = false;
        if (!initialized)
        {
            RegisterNpcAiNodes(factory);
            initialized = true;
        }
        return &factory;
    }

    // 简单的行为树 XML：以 Sequence 串联：找人 -> 移动 -> 攻击
    const char *xml_text = R"(
    <root BTCPP_format="4">
        <BehaviorTree ID="MainTree">
            <Sequence>
                <FindPlayer   target_id="{attack_target}" />
                <MoveTowards  target_id="{attack_target}" />
                <AttackPlayer target_id="{attack_target}" />
            </Sequence>
        </BehaviorTree>
    </root>
    )";

    void NpcActor::reset(ActorID self_id, float x, float y)
    {
        base_reset(self_id, ActorID(0));
        x_ = x;
        y_ = y;
        is_ai_active_ = true; // 暂且设为 true，方便测试

        // 1. 初始化黑板，把自己的指针存进去
        auto blackboard = BT::Blackboard::create();
        blackboard->set<NpcActor *>("npc_ptr", this);
        blackboard->set<SceneActor *>("scene_ptr", scene_);

        // 2. 从工厂加载行为树
        auto factory = GetAiFactory();
        tree_ = factory->createTreeFromText(xml_text, blackboard);
    }

    void NpcActor::handle_message(ActorMessage *msg)
    {
    }

    void NpcActor::OnTick()
    {
        if (!is_ai_active_)
            return;

        // 驱动行为树运行一遍
        tree_.tickExactlyOnce();
    }

    void NpcActor::TakeDamage(int32_t damage)
    {
        if (is_dead_ || damage <= 0)
            return;

        hp_ -= damage;
        if (hp_ <= 0)
        {
            hp_ = 0;
            is_dead_ = true;
        }
    }
}