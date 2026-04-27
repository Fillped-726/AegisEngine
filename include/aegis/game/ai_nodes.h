/**
 * @file ai_nodes.h
 * @brief BehaviorTree.CPP v4 custom node definitions for NPC AI behaviors.
 */
#pragma once

#include <behaviortree_cpp/behavior_tree.h>
#include <behaviortree_cpp/bt_factory.h>

namespace aegis::core
{
    class NpcActor;
    class SceneActor;

    // 注册所有自定义 AI 节点
    void RegisterNpcAiNodes(BT::BehaviorTreeFactory &factory);

    // ----------------------------------------------------
    // 1. 索敌节点：寻找最近的玩家
    // ----------------------------------------------------
    class FindPlayerNode : public BT::SyncActionNode
    {
    public:
        FindPlayerNode(const std::string &name, const BT::NodeConfig &config);
        static BT::PortsList providedPorts();
        BT::NodeStatus tick() override;
    };

    // ----------------------------------------------------
    // 2. 移动节点：向目标玩家移动
    // ----------------------------------------------------
    class MoveTowardsNode : public BT::StatefulActionNode
    {
    public:
        MoveTowardsNode(const std::string &name, const BT::NodeConfig &config);
        static BT::PortsList providedPorts();
        BT::NodeStatus onStart() override;
        BT::NodeStatus onRunning() override;
        void onHalted() override;
    };

    // ----------------------------------------------------
    // 3. 攻击节点：对目标玩家发起攻击
    // ----------------------------------------------------
    class AttackPlayerNode : public BT::SyncActionNode
    {
    public:
        AttackPlayerNode(const std::string &name, const BT::NodeConfig &config);
        static BT::PortsList providedPorts();
        BT::NodeStatus tick() override;
    };

} // namespace aegis::core