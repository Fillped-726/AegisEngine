/**
 * @file rpc_channel.h
 * @brief Sender-side RPC transport abstraction (Seastar-like design).
 *
 * RpcChannel defines the pure-virtual interface for sending RPC messages
 * to a target. Subclasses implement the actual transport:
 *
 *   - LocalMpscChannel:  dispatch via thread-local MPSC queue (in-process Actor)
 *   - TcpNetworkChannel: serialize and send over io_uring TCP (cross-server)
 *
 * Usage (future):
 *   auto *ch = RpcChannel::get_or_create(target_id);
 *   co_await ch->send_message(msg_id, data, len);
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

#include "aegis/core/task.h"

namespace aegis::rpc
{
    // ── RPC 目标标识 ──
    // 用于屏蔽本地 ActorID 与远程 server_id+actor_id 的差异
    struct TargetID
    {
        uint64_t raw = 0;

        bool is_valid() const { return raw != 0; }
        bool operator==(const TargetID &o) const { return raw == o.raw; }
        bool operator!=(const TargetID &o) const { return raw != o.raw; }
    };

    // ── RPC 发送端抽象 ──
    /**
     * @brief Pure-virtual channel for sending RPC payloads.
     *
     * The caller holds a TargetID (opaque handle) and calls send_message().
     * The concrete implementation decides whether to:
     *   - push into a lock-free MPSC queue (local intra-process)
     *   - serialize + submit io_uring write (inter-process TCP)
     *   - route via some future transport
     */
    class RpcChannel
    {
    public:
        virtual ~RpcChannel() = default;

        /**
         * @brief Send a type-erased RPC message blob to the target.
         * @param target  Opaque target identifier.
         * @param msg_id  Message type ID (opaque to the channel).
         * @param data    Serialized payload (may be nullptr if len==0).
         * @param len     Payload length in bytes.
         * @return Task<void> that completes when the message has been
         *         delivered (local) or enqueued for send (network).
         *
         * Semantics are fire-and-forget from the caller's perspective;
         * request-response correlation is managed at a higher layer.
         */
        virtual core::Task<void> send_message(TargetID target,
                                               uint32_t msg_id,
                                               const void *data,
                                               size_t len) = 0;

        /**
         * @brief Create or retrieve a channel for the given target.
         *
         * Future factory: the implementation can decide the transport
         * based on the target address space (local ActorID vs remote).
         */
        static std::shared_ptr<RpcChannel> get_or_create(const TargetID &target);
    };

} // namespace aegis::rpc
