#include "aegis/net/packet.h"

namespace aegis::net
{

    Packet::Packet(const Packet &other)
    {
        copy_from(other);
    }

    Packet &Packet::operator=(const Packet &other)
    {
        if (this != &other)
            copy_from(other);
        return *this;
    }

    Packet::Packet(Packet &&other) noexcept
    {
        move_from(std::move(other));
    }

    Packet &Packet::operator=(Packet &&other) noexcept
    {
        if (this != &other)
            move_from(std::move(other));
        return *this;
    }

    Packet::~Packet()
    {
        free_heap();
    }

    uint32_t Packet::msg_id() const
    {
        if (size_ < kPacketMsgHeader)
            return 0;

        uint32_t net_id;
        // 此时 data_ 指向栈或者堆，对调用者透明
        std::memcpy(&net_id, data_, 4);

        if constexpr (std::endian::native == std::endian::big)
        {
            return net_id;
        }
        else
        {
            return __builtin_bswap32(net_id);
        }
    }

    const char *Packet::data() const
    {
        return data_;
    }

    size_t Packet::size() const
    {
        return size_;
    }

    void Packet::alloc(size_t req_size)
    {
        // 如果请求大小超过当前容量
        if (req_size > capacity_)
        {
            size_t new_cap = std::max(req_size, capacity_ * 2);
            grow(new_cap);
        }
        // 仅仅调整逻辑大小，不涉及内存清零 (避免 resize 的性能损耗)
        size_ = req_size;
    }

    char *Packet::mutable_data()
    {
        return data_;
    }

    // --- Private Helper Methods ---

    void Packet::reset()
    {
        size_ = 0; // 逻辑清空

        if (heap_buf_ && capacity_ > kMaxRetainSize)
        {
            free_heap();
            data_ = stack_buf_;
            capacity_ = kSmallBufferSize;
        }
    }

    void Packet::grow(size_t new_cap)
    {
        char *new_mem = new char[new_cap];

        // 如果有旧数据，拷贝过来
        if (size_ > 0)
        {
            std::memcpy(new_mem, data_, size_);
        }

        // 如果之前已经在堆上，释放旧堆
        free_heap();

        heap_buf_ = new_mem;
        data_ = heap_buf_;
        capacity_ = new_cap;
    }

    void Packet::free_heap()
    {
        if (heap_buf_)
        {
            delete[] heap_buf_;
            heap_buf_ = nullptr;
        }
    }

    void Packet::copy_from(const Packet &other)
    {
        alloc(other.size_); // 确保空间足够
        std::memcpy(data_, other.data_, other.size_);
    }

    void Packet::move_from(Packet &&other)
    {
        // 先清理自己
        free_heap();

        size_ = other.size_;
        capacity_ = other.capacity_;

        if (other.heap_buf_)
        {
            // 如果对方是堆内存，直接偷指针 (这是移动语义的精髓)
            heap_buf_ = other.heap_buf_;
            data_ = heap_buf_;

            // 置空对方
            other.heap_buf_ = nullptr;
            other.data_ = other.stack_buf_; // 让对方回到安全状态
            other.size_ = 0;
            other.capacity_ = kSmallBufferSize;
        }
        else
        {
            // 如果对方是栈内存，必须拷贝 (栈内存无法"移动"所有权，只能拷贝数据)
            // 但因为是在栈上，且必然小于 kSmallBufferSize，由于 Cache 命中率高，这次 memcpy 极快
            std::memcpy(stack_buf_, other.stack_buf_, other.size_);
            data_ = stack_buf_;
            heap_buf_ = nullptr;
        }
    }

} // namespace aegis::net