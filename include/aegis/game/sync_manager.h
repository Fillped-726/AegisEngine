/**
 * @file sync_manager.h
 * @brief Dirty-driven state sync with AOI cross-grid detection and batch broadcast.
 */
#pragma once

#include <vector>
#include <unordered_set>
#include <cstdint>
#include <memory>
#include <functional>

#include "aegis/game/playerActor.h"
#include "aegis/game/aoi_grid.h"
#include "aegis/common/aegisLog.h"
// 包含你刚刚新增的 protobuf 定义
#include "cs_battle.pb.h"

namespace aegis::core
{
    /**
     * @brief Dirty-driven state synchronization engine.
     * 
     * Tracks dirty players, computes AOI cross-grid changes (enter/leave),
     * aggregates per-receiver SCMoveNtfBatch protobuf messages,
     * and invokes callbacks for batched broadcast delivery.
     * 
     * Template Tick() decouples sync logic from delivery mechanism.
     */
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
            if (dirty_set_.insert(player->id().raw).second)
            {
                dirty_players_.push_back(player);
            }
        }

        void RemoveDirtyPlayer(PlayerActor *player)
        {
            dirty_set_.erase(player->id().raw);
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

            // [SYNC] 开始 Tick，有多少脏玩家
            aegis::Log::instance().debug("[SYNC][TickStart] {} dirty players (dirty_count={})",
                dirty_players_.size(), dirty_set_.size());

            // ==========================================
            // 阶段 1：计算视野变更 (Enter / Leave)
            // ==========================================
            for (PlayerActor *mover : dirty_players_)
            {
                if (!mover->IsDirty(PlayerActor::DIRTY_POS))
                    continue;

                std::vector<uint64_t> enterIds;
                std::vector<uint64_t> leaveIds;

                uint32_t old_grid = mover->get_aoi_grid_index();
                uint32_t new_grid = aoi.Move(mover->id().raw, mover->get_aoi_grid_index(),
                                             mover->GetX(), mover->GetY(),
                                             enterIds, leaveIds);

                if (new_grid != (uint32_t)-1)
                {
                    mover->set_aoi_grid_index(new_grid);
                    // [SYNC] AOI 跨格
                    if (old_grid != new_grid)
                    {
                        aegis::Log::instance().debug("[SYNC][AOI] Player {} grid: {}→{}, enters={}, leaves={}",
                            mover->id().raw, old_grid, new_grid, enterIds.size(), leaveIds.size());
                    }
                    // 只有真跨格了，才回调给 SceneActor 处理视野包
                    if (!enterIds.empty() || !leaveIds.empty())
                    {
                        onEnterLeave(mover, enterIds, leaveIds);
                    }
                }
                else if (old_grid != (uint32_t)-1)
                {
                    // 没有跨格但移动了后续阶段2会发batch
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

                // [SYNC] 该脏玩家的观众列表
                aegis::Log::instance().debug("[SYNC][Phase2] Mover {} at ({:.2f},{:.2f}) grid={}, neighbors={}",
                    mover->id().raw, mover->GetX(), mover->GetY(),
                    mover->get_aoi_grid_index(), neighbors.size());

                for (uint64_t neighborActorId : neighbors)
                {
                    if (neighborActorId == mover->id().raw)
                        continue; // 不发给自己

                    // 将 mover 的移动信息塞入 receiver 的专属 batch 中
                    auto *move_info = receiver_batches[neighborActorId].add_moves();
                    move_info->set_entity_id(mover->id().raw); // 使用 ActorID，与 EnterView 一致
                    move_info->mutable_pos()->set_x(mover->GetX());
                    move_info->mutable_pos()->set_y(mover->GetY());
                    move_info->set_direction(mover->GetDir()); // 使用 mover 的实际朝向，修复鬼畜转头
                    move_info->set_speed(mover->GetSpeed());
                    move_info->set_is_moving(mover->IsMoving());
                }
            }

            // ==========================================
            // 阶段 3：执行批量发送
            // ==========================================
            int total_targets = 0;
            int total_moves = 0;
            for (const auto &[receiverActorId, batch] : receiver_batches)
            {
                if (batch.moves_size() > 0)
                {
                    total_targets++;
                    total_moves += batch.moves_size();
                    // [SYNC] 每个接收者的广播信息
                    aegis::Log::instance().debug("[SYNC][Phase3] SendBatch to receiver={}, moves_in_batch={}",
                        receiverActorId, batch.moves_size());

                    // 整个 Tick 中，对某个玩家的所有移动更新只序列化一次！
                    auto shared_buf = std::make_shared<std::string>(batch.SerializeAsString());
                    onSendBatch(receiverActorId, shared_buf);
                }
            }
            // [SYNC] Tick 汇总
            aegis::Log::instance().debug("[SYNC][TickEnd] Broadcast: {} targets, {} moves total",
                total_targets, total_moves);

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