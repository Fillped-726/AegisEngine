// ai_nodes.cpp
#include "aegis/core/ai_nodes.h"
#include "aegis/core/npc_actor.h"
#include "aegis/core/scene_actor.h"
#include "aegis/common/aegisLog.h"

namespace aegis::core
{
    void RegisterNpcAiNodes(BT::BehaviorTreeFactory &factory)
    {
        factory.registerNodeType<FindPlayerNode>("FindPlayer");
        factory.registerNodeType<MoveTowardsNode>("MoveTowards");
        factory.registerNodeType<AttackPlayerNode>("AttackPlayer");
    }

    // ====================================================================
    // FindPlayerNode 实现 (KR3: 索敌)
    // ====================================================================
    FindPlayerNode::FindPlayerNode(const std::string &name, const BT::NodeConfig &config)
        : BT::SyncActionNode(name, config) {}

    BT::PortsList FindPlayerNode::providedPorts()
    {
        return {BT::OutputPort<uint64_t>("target_id")};
    }

    BT::NodeStatus FindPlayerNode::tick()
    {
        NpcActor *npc = nullptr;
        if (!config().blackboard->get("npc_ptr", npc))
        {
            return BT::NodeStatus::FAILURE;
        }

        SceneActor *scene = npc->GetScene();
        if (!scene)
        {
            return BT::NodeStatus::FAILURE;
        }

        std::vector<uint64_t> view_entities;
        scene->GetAoi().GetViewEntityIds(npc->get_aoi_grid_index(), view_entities);

        uint64_t closest_player_id = 0;
        float min_dist_sq = 9999999.0f;

        // 寻找视野内最近的玩家
        for (uint64_t vid : view_entities)
        {
            auto *player = scene->GetPlayer(vid);
            if (player && !player->IsDead())
            {
                float dx = player->GetX() - npc->GetX();
                float dy = player->GetY() - npc->GetY();
                float dist_sq = dx * dx + dy * dy;

                if (dist_sq < min_dist_sq)
                {
                    min_dist_sq = dist_sq;
                    closest_player_id = vid;
                }
            }
        }

        if (closest_player_id != 0)
        {
            setOutput("target_id", closest_player_id);
            // common::Log::instance().debug("[AI] NPC {} Found Target {}", npc->id().raw, closest_player_id);
            return BT::NodeStatus::SUCCESS;
        }

        return BT::NodeStatus::FAILURE;
    }

    // ====================================================================
    // MoveTowardsNode 实现 (KR3: 遇阻/距离停止)
    // ====================================================================
    MoveTowardsNode::MoveTowardsNode(const std::string &name, const BT::NodeConfig &config)
        : BT::StatefulActionNode(name, config) {}

    BT::PortsList MoveTowardsNode::providedPorts()
    {
        return {BT::InputPort<uint64_t>("target_id")};
    }

    BT::NodeStatus MoveTowardsNode::onStart() { return onRunning(); }

    BT::NodeStatus MoveTowardsNode::onRunning()
    {
        NpcActor *npc = nullptr;
        config().blackboard->get("npc_ptr", npc);
        SceneActor *scene = npc->GetScene();

        auto target_id_res = getInput<uint64_t>("target_id");
        if (!target_id_res || !scene)
            return BT::NodeStatus::FAILURE;

        auto *player = scene->GetPlayer(target_id_res.value());
        if (!player || player->IsDead())
            return BT::NodeStatus::FAILURE; // 目标丢失

        float dx = player->GetX() - npc->GetX();
        float dy = player->GetY() - npc->GetY();
        float dist_sq = dx * dx + dy * dy;

        const float ATTACK_RANGE = 2.0f; // 假设攻击距离是 2.0

        // 遇阻停止（本期弱化：如果距离小于攻击距离，视为到达目标位置，停止移动并返回 SUCCESS）
        if (dist_sq <= ATTACK_RANGE * ATTACK_RANGE)
        {
            return BT::NodeStatus::SUCCESS;
        }

        // 简单平移逻辑 (向目标方向移动一步)
        float dist = std::sqrt(dist_sq);
        float speed = 0.5f; // NPC 移动速度
        float new_x = npc->GetX() + (dx / dist) * speed;
        float new_y = npc->GetY() + (dy / dist) * speed;

        npc->SetPos(new_x, new_y);
        // 这里应有：更新 AOI Grid 的逻辑 scene->GetAoi().Move(...)

        return BT::NodeStatus::RUNNING; // 还没走到，下一帧继续执行此节点
    }

    void MoveTowardsNode::onHalted() {}

    // ====================================================================
    // AttackPlayerNode 实现 (KR3: 攻击)
    // ====================================================================
    AttackPlayerNode::AttackPlayerNode(const std::string &name, const BT::NodeConfig &config)
        : BT::SyncActionNode(name, config) {}

    BT::PortsList AttackPlayerNode::providedPorts()
    {
        return {BT::InputPort<uint64_t>("target_id")};
    }

    BT::NodeStatus AttackPlayerNode::tick()
    {
        NpcActor *npc = nullptr;
        config().blackboard->get("npc_ptr", npc);
        SceneActor *scene = npc->GetScene(); // 使用安全的获取方式

        auto target_id_res = getInput<uint64_t>("target_id");
        if (!target_id_res || !scene)
            return BT::NodeStatus::FAILURE;

        // 【⚠️ 高能预警：防 DDOS 简易 CD 控制】
        // 因为 OnTick 是 50ms 跑一次，如果不加 CD，NPC 一秒钟会向客户端发送 20 次攻击包！
        static auto last_attack_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_attack_time).count() < 1000)
        {
            return BT::NodeStatus::SUCCESS; // 攻击 CD 中，假装成功并跳过
        }
        last_attack_time = now;

        // 构建真实的技能释放消息，投递给当前场景处理
        // 注意：因为我们在同一个 Scene 的 Tick 线程里，直接 handle_message 是绝对线程安全的
        SceneSkillCastMsg msg(npc->id(), 1, target_id_res.value(), 0.0f, 0.0f); // 技能坐标可以在 SceneActor 里根据目标玩家位置计算

        scene->handle_message(&msg);

        return BT::NodeStatus::SUCCESS;
    }
}