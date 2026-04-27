/**
 * @file packet.h
 * @brief SBO-backed network packet container with big-endian wire protocol.
 *
 * Wire format: [4B Length BigEndian][4B MsgID BigEndian][Protobuf Body]
 * Length = 4 + BodySize. SBO threshold = 1024 bytes.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <bit> // [DEPENDENCY: C++20 std::endian]
#include <algorithm>
#include <array>
#include <cassert>

#include "aegis/common/aegisLog.h" // [DEPENDENCY: aegis::Log]

namespace google::protobuf
{
    class Message;
}

// [CONSTRAINT: T requires google::protobuf::Message base]
template <typename T>
concept ProtobufMessage = std::is_base_of_v<google::protobuf::Message, T>;

namespace aegis::net
{
    // ── 帧层常量 (网络帧, OutboxBatcher/read_packet 处理) ──
    constexpr size_t kFrameMagicSize = 4;
    constexpr size_t kFrameLengthSize = 4;
    constexpr size_t kFrameHeaderSize = kFrameMagicSize + kFrameLengthSize; // 8

    // ── 包层常量 (Packet 内部) ──
    constexpr size_t kPacketSeqIdSize = 4;
    constexpr size_t kPacketMsgIdSize = 4;
    constexpr size_t kPacketMsgHeader = kPacketSeqIdSize + kPacketMsgIdSize; // 8

    // 魔数 'AEGS' = 0x41454753
    constexpr uint32_t kAegisMagic = 0x41454753;

    // [STATE: Small Buffer Optimization (SBO) threshold]
    constexpr size_t kSmallBufferSize = 1024;

    static constexpr size_t kMaxRetainSize = 64 * 1024;

    // [INTENT: SBO-backed network payload container; minimizes heap allocations]
    /**
     * @brief SBO-backed network packet container.
     *
     * Wire protocol: [4B BigEndian Length][4B BigEndian MsgID][Protobuf Body]
     * Small Buffer Optimization: 1024B stack buffer, heap fallback for larger payloads.
     * Thread-safe design for pooled allocation (ObjectPool<Packet>).
     */
    class Packet
    {
    public:
        Packet() = default;

        // [STATE_MUTATION: SBO-aware deep copy]
        Packet(const Packet &other);
        Packet &operator=(const Packet &other);

        // [STATE_MUTATION: SBO-aware ownership transfer]
        Packet(Packet &&other) noexcept;
        Packet &operator=(Packet &&other) noexcept;

        ~Packet();

        // [INTENT: Read big-endian header]
        [[nodiscard]] uint32_t msg_id() const;

        // [INTENT: Read big-endian seq_id]
        [[nodiscard]] uint32_t seq_id() const;

        // [INTENT: 设置 seq_id，用于响应方回填]
        void set_seq_id(uint32_t seq_id);

        [[nodiscard]] const char *data() const;
        [[nodiscard]] size_t size() const;

        // [STATE_MUTATION: Resolve stack vs heap allocation strategy]
        void alloc(size_t req_size);

        char *mutable_data();

        // [INTENT: Extract body to Protobuf object; bypass header]
        template <ProtobufMessage T>
        bool parse(T &msg) const
        {
            if (size_ <= kPacketMsgHeader)
                return false;

            const void *body_ptr = data_ + kPacketMsgHeader;
            int body_len = static_cast<int>(size_ - kPacketMsgHeader);

            if (body_len == 0)
                return true;

            return msg.ParseFromArray(body_ptr, body_len);
        }

        // [STATE_MUTATION: Serialize SeqID + MsgID + Protobuf body to active buffer]
        template <ProtobufMessage T>
        void pack_into(uint32_t msg_id, uint32_t seq_id, const T &msg)
        {
            size_t body_size = msg.ByteSizeLong();
            size_t total_size = kPacketMsgHeader + body_size;

            alloc(total_size);

            // 写入 SeqID (Big Endian)
            uint32_t net_seq = (std::endian::native == std::endian::big)
                                   ? seq_id
                                   : __builtin_bswap32(seq_id);
            std::memcpy(data_, &net_seq, kPacketSeqIdSize);

            // 写入 MsgID (Big Endian)
            uint32_t net_id = (std::endian::native == std::endian::big)
                                  ? msg_id
                                  : __builtin_bswap32(msg_id);
            std::memcpy(data_ + kPacketSeqIdSize, &net_id, kPacketMsgIdSize);

            if (body_size > 0)
            {
                msg.SerializeToArray(data_ + kPacketMsgHeader, static_cast<int>(body_size));
            }

            // 记录 seq_id 供后续响应时回填
            seq_id_ = seq_id;
        }

        // [STATE_MUTATION: Reclaim memory; reset to SBO state]
        void reset();

    private:
        // [STATE: SBO hot memory tier]
        alignas(std::max_align_t) char stack_buf_[kSmallBufferSize];

        // [STATE: SBO cold memory tier]
        char *heap_buf_ = nullptr;

        // [STATE: Active tier pointer alias]
        char *data_ = stack_buf_;

        // [STATE: 最近一次 pack_into/set_seq_id 使用的 SeqID，供响应方回填]
        uint32_t seq_id_ = 0;

        size_t size_ = 0;
        size_t capacity_ = kSmallBufferSize;

        // [STATE_MUTATION: Heap allocation/expansion]
        void grow(size_t new_cap);
        void free_heap();
        void copy_from(const Packet &other);
        void move_from(Packet &&other);
    };

} // namespace aegis::net