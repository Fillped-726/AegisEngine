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
    constexpr size_t kPacketMsgHeader = 4;

    // [STATE: Small Buffer Optimization (SBO) threshold]
    constexpr size_t kSmallBufferSize = 1024;

    static constexpr size_t kMaxRetainSize = 64 * 1024;

    // [INTENT: SBO-backed network payload container; minimizes heap allocations]
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

        // [STATE_MUTATION: Serialize header + Protobuf body to active buffer]
        template <ProtobufMessage T>
        void pack_into(uint32_t msg_id, const T &msg)
        {
            size_t body_size = msg.ByteSizeLong();
            size_t total_size = kPacketMsgHeader + body_size;

            alloc(total_size);

            // [INTENT: Enforce network byte order (Big Endian)]
            uint32_t net_id = (std::endian::native == std::endian::big)
                                  ? msg_id
                                  : __builtin_bswap32(msg_id);
            std::memcpy(data_, &net_id, kPacketMsgHeader);

            if (body_size > 0)
            {
                msg.SerializeToArray(data_ + kPacketMsgHeader, static_cast<int>(body_size));
            }
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

        size_t size_ = 0;
        size_t capacity_ = kSmallBufferSize;

        // [STATE_MUTATION: Heap allocation/expansion]
        void grow(size_t new_cap);
        void free_heap();
        void copy_from(const Packet &other);
        void move_from(Packet &&other);
    };

} // namespace aegis::net