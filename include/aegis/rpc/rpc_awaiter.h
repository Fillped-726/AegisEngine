/**
 * @file rpc_awaiter.h
 * @brief Coroutine awaitable for async RPC — replaces blocking future.get().
 *
 * Migrated from aegis/game/rpc_awaiter.h → aegis/rpc/rpc_awaiter.h.
 * Namespace changed from aegis::core → aegis::rpc.
 *
 * Replaces:
 *   auto future = rpc_msg->promise.get_future();
 *   dispatch_msg(target, rpc_msg);
 *   AssignCampRes res = future.get();   // BLOCKS WORKER THREAD!
 *
 * With:
 *   auto *rpc_msg = new RPCAssignCampMsg(req);
 *   rpc_msg->set_rpc_meta(0, player->id());
 *   AssignCampRes res = co_await RpcCall<AssignCampRes>(room_mgr_id, rpc_msg);
 *
 * How it works:
 *   RpcCall takes an rpc_id_setter callback. When await_suspend generates
 *   the rpc_id, it calls the setter to inject it into the RpcMessage.
 */
#pragma once

#include <coroutine>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <memory>
#include <stdexcept>

#include "aegis/core/task.h"
#include "aegis/core/actor.h"
#include "aegis/core/worker.h"
#include "aegis/core/actor_registry.h"
#include "aegis/rpc/rpc_message.h"
#include "aegis/common/aegisLog.h"
#include "aegis/common/actor_utils.h"

namespace aegis::rpc
{
    constexpr uint32_t kRpcDefaultTimeoutMs = 5000;

    // ── 每个 Worker 的 RPC 挂起管理器 ──
    class RpcManager
    {
    public:
        struct Entry
        {
            std::coroutine_handle<> handle;
            void **result_slot = nullptr;
            uint32_t timer_id = 0;
        };

        static RpcManager &instance()
        {
            thread_local RpcManager mgr;
            return mgr;
        }

        void register_pending(RpcId id, std::coroutine_handle<> handle,
                              void **slot, uint32_t timer_id)
        {
            pending_[id] = {handle, slot, timer_id};
        }

        std::coroutine_handle<> consume(RpcId id, void *result_ptr)
        {
            auto it = pending_.find(id);
            if (it == pending_.end())
                return nullptr;

            auto handle = it->second.handle;
            if (it->second.result_slot)
                *it->second.result_slot = result_ptr;

            if (it->second.timer_id != 0 && core::t_current_worker)
                core::t_current_worker->time_wheel().cancel_timer(it->second.timer_id);

            pending_.erase(it);
            return handle;
        }

        void on_timeout(RpcId id)
        {
            auto it = pending_.find(id);
            if (it == pending_.end()) return;

            auto handle = it->second.handle;
            if (it->second.result_slot)
                *it->second.result_slot = nullptr;
            pending_.erase(it);

            if (handle) handle.resume();
        }

        RpcId next_id()
        {
            constexpr uint64_t WORKER_SHIFT = 48;
            uint64_t id = counter_.fetch_add(1, std::memory_order_relaxed);
            return (static_cast<uint64_t>(core::Worker::get_current_id()) << WORKER_SHIFT) | id;
        }

    private:
        std::unordered_map<RpcId, Entry> pending_;
        std::atomic<uint64_t> counter_{1};
    };

    // ── RpcAwaiter ──
    template <typename ResT>
    class RpcAwaiter
    {
    public:
        using RpcIdSetter = std::function<void(uint64_t)>;

        RpcAwaiter(core::ActorID target, core::ActorMessage *msg,
                   RpcIdSetter rpc_id_setter,
                   uint32_t timeout_ms = kRpcDefaultTimeoutMs)
            : target_id_(target), msg_(msg),
              rpc_id_setter_(std::move(rpc_id_setter)),
              timeout_ms_(timeout_ms) {}

        ~RpcAwaiter()
        {
            if (msg_ && !dispatched_)
                delete msg_;
        }

        bool await_ready() { return false; }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!msg_)
            {
                timed_out_ = true;
                return false;
            }

            rpc_id_ = RpcManager::instance().next_id();

            // 把 rpc_id 写回 RpcMessage（通过 setter 回调）
            if (rpc_id_setter_)
                rpc_id_setter_(rpc_id_);

            RpcId id = rpc_id_;
            uint32_t timer_id = 0;
            if (core::t_current_worker)
            {
                timer_id = core::t_current_worker->time_wheel().add_timer(
                    timeout_ms_, [id]()
                    { RpcManager::instance().on_timeout(id); });
            }

            void **slot = reinterpret_cast<void **>(&result_);
            RpcManager::instance().register_pending(rpc_id_, handle, slot, timer_id);

            auto *target = core::ActorRegistry::instance().get(target_id_);
            if (!target)
            {
                timed_out_ = true;
                return false;
            }

            dispatched_ = true;
            core::dispatch_msg(target, msg_);
            msg_ = nullptr;
            return true;
        }

        ResT await_resume()
        {
            if (timed_out_ || result_ == nullptr)
                throw std::runtime_error("RPC timed out");

            auto *typed_result = static_cast<ResT *>(result_);
            ResT r = std::move(*typed_result);
            delete typed_result;
            result_ = nullptr;
            return r;
        }

        RpcId rpc_id() const { return rpc_id_; }

    private:
        core::ActorID target_id_;
        core::ActorMessage *msg_ = nullptr;
        RpcIdSetter rpc_id_setter_;
        uint32_t timeout_ms_;
        RpcId rpc_id_ = 0;
        bool dispatched_ = false;
        void *result_ = nullptr;
        bool timed_out_ = false;
    };

    // ── 便捷函数 ──
    template <typename ResT, typename MsgT>
    RpcAwaiter<ResT> RpcCall(core::ActorID target, MsgT *msg,
                              uint32_t timeout_ms = kRpcDefaultTimeoutMs)
    {
        // setter 回调：把 rpc_id 写入 RpcMessage
        typename RpcAwaiter<ResT>::RpcIdSetter setter;
        if constexpr (requires { msg->set_rpc_id(uint64_t{}); })
        {
            setter = [msg](uint64_t id)
            {
                msg->set_rpc_id(id);
            };
        }

        return RpcAwaiter<ResT>(target, static_cast<core::ActorMessage *>(msg),
                                 std::move(setter), timeout_ms);
    }

} // namespace aegis::rpc
