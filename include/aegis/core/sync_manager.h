#pragma once

#include <vector>
#include <unordered_set>
#include <cstdint>
#include <memory>
#include <functional>

#include "aegis/core/playerActor.h"
#include "aegis/core/aoi_grid.h"
// 包含你刚刚新增的 protobuf 定义
#include "cs_battle.pb.h"

namespace aegis::core
{
    class SyncManager
    {
    public:
        SyncManager()
        {
            // 面试高光：预分配内存，避免 Tick 过程中的动态扩容开销
            dirty_players_.reserve(1024);
        }
        ~SyncManager() = default;

        void AddDirtyPlayer(PlayerActor *player)
        {
            if (dirty_set_.insert(player->get_player_id()).second)
            {
                dirty_players_.push_back(player);
            }
        }

        void RemoveDirtyPlayer(PlayerActor *player)
        {
            dirty_set_.erase(player->get_player_id());
            // 简单移除，实际项目中可使用侵入式链表做到 O(1) 移除
            std::erase(dirty_players_, player);
        }

        /**
         * @brief 核心 Tick 驱动
         * @param aoi 场景的 AOI 实例
         * @param onEnterLeave 回调：处理跨越网格时的视野变更
         * @param onSendBatch 回调：处理最终的批量发包
         */
        template <typename EnterLeaveFunc, typename SendBatchFunc>
        void Tick(AOIGrid &aoi, EnterLeaveFunc &&onEnterLeave, SendBatchFunc &&onSendBatch)
        {
            if (dirty_players_.empty())
                return;

            // ==========================================
            // 阶段 1：计算视野变更 (Enter / Leave)
            // ==========================================
            for (PlayerActor *mover : dirty_players_)
            {
                if (!mover->IsDirty(PlayerActor::DIRTY_POS))
                    continue;

                std::vector<uint64_t> enterIds;
                std::vector<uint64_t> leaveIds;

                uint32_t new_grid = aoi.Move(mover->id().raw, mover->get_aoi_grid_index(),
                                             mover->GetX(), mover->GetY(),
                                             enterIds, leaveIds);

                if (new_grid != (uint32_t)-1)
                {
                    mover->set_aoi_grid_index(new_grid);
                    // 只有真跨格了，才回调给 SceneActor 处理视野包
                    if (!enterIds.empty() || !leaveIds.empty())
                    {
                        onEnterLeave(mover, enterIds, leaveIds);
                    }
                }
            }

            // ==========================================
            // 阶段 2：聚合打包与广播 (状态同步)
            // ==========================================
            // 映射：接收者的 ActorID -> 他将看到的移动包
            std::unordered_map<uint64_t, aegis::cs::battle::SCMoveNtfBatch> receiver_batches;

            for (PlayerActor *mover : dirty_players_)
            {
                if (!mover->IsDirty(PlayerActor::DIRTY_POS))
                    continue;

                // 获取该玩家周围 9 宫格的观众
                std::vector<uint64_t> neighbors;
                aoi.GetViewEntityIds(mover->get_aoi_grid_index(), neighbors);

                for (uint64_t neighborActorId : neighbors)
                {
                    if (neighborActorId == mover->id().raw)
                        continue; // 不发给自己

                    // 将 mover 的移动信息塞入 receiver 的专属 batch 中
                    auto *move_info = receiver_batches[neighborActorId].add_moves();
                    move_info->set_entity_id(mover->get_player_id());
                    move_info->mutable_pos()->set_x(mover->GetX());
                    move_info->mutable_pos()->set_y(mover->GetY());
                }
            }

            // ==========================================
            // 阶段 3：执行批量发送
            // ==========================================
            for (const auto &[receiverActorId, batch] : receiver_batches)
            {
                if (batch.moves_size() > 0)
                {
                    // 整个 Tick 中，对某个玩家的所有移动更新只序列化一次！
                    auto shared_buf = std::make_shared<std::string>(batch.SerializeAsString());
                    onSendBatch(receiverActorId, shared_buf);
                }
            }

            // ==========================================
            // 阶段 4：清理标记
            // ==========================================
            for (PlayerActor *player : dirty_players_)
            {
                player->ClearDirty();
            }
            dirty_players_.clear();
            dirty_set_.clear();
        }

    private:
        std::vector<PlayerActor *> dirty_players_;
        std::unordered_set<uint64_t> dirty_set_;
    };

} // namespace aegis::core