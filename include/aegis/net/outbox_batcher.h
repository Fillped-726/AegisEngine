#pragma once

#include <vector>
#include <deque>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <limits.h>
#include "aegis/net/packet.h"
#include "aegis/net/packetPool.h"

namespace aegis::net
{

    /**
     * @brief 零拷贝视图构建器
     * 职责：将 Packet 队列转换为 writev/sendmsg 可用的 iovec 数组。
     * 关键：管理临时 Header 的生命周期，避免悬空指针。
     */
    class OutboxBatcher
    {
    public:
        // Linux 默认 IOV_MAX 通常是 1024，这里保守取值，
        // 既避免栈溢出（虽然是在堆上），也避免单次 syscall 太大导致内核处理延迟。
        static constexpr size_t BATCH_LIMIT = 64;

        OutboxBatcher()
        {
            // 预分配内存，避免运行时 realloc 带来的抖动
            // 每个 Packet 需要 2 个 iovec (1 Header + 1 Body)
            iovecs_.reserve(BATCH_LIMIT * 2);
            header_cache_.reserve(BATCH_LIMIT);
        }

        /**
         * @brief 构建 scatter-gather 视图
         * @param queue 待发送的数据包队列
         * @return size_t 本次 Batch 实际处理了多少个 Packet
         */
        size_t prepare_batch(const std::vector<PooledPacket> &queue)
        {
            // 1. Reset (并不是释放内存，只是重置指针，极快)
            iovecs_.clear();
            header_cache_.clear();
            packet_end_indices_.clear();

            consumed_iov_index_ = 0; // 重置游标
            size_t count = 0;

            // 2. Iterate & Build
            for (const auto &pkt : queue)
            {
                // 安全检查：防止超过内核单次 writev 的限制
                // 预留 2 个槽位给当前的 header 和 body
                if (count >= BATCH_LIMIT || iovecs_.size() + 2 > 1024)
                {
                    break;
                }

                // --- 陷阱回避区域 Start ---

                // [Critical] Header 生命周期管理
                // 错误做法：uint32_t len = htonl(...) -> iov_base = &len;
                // 原因：len 是局部变量，循环结束后地址无效，内核读取时就是乱码。

                uint32_t body_len = static_cast<uint32_t>(pkt->size());
                uint32_t net_len = htonl(body_len);

                // 正确做法：存入成员变量 vector 中，确保生命周期覆盖到 syscall 结束
                header_cache_.push_back(net_len);

                // --- 陷阱回避区域 End ---

                // 3. Construct iovec [Header]
                struct iovec iov_h;
                iov_h.iov_base = &header_cache_.back(); // 指向 cache 中的地址
                iov_h.iov_len = sizeof(uint32_t);       // 4 bytes
                iovecs_.push_back(iov_h);

                // 4. Construct iovec [Body]
                if (body_len > 0)
                {
                    struct iovec iov_b;
                    // [Zero Copy] 直接指向 Packet 内部的 payload 内存
                    iov_b.iov_base = const_cast<char *>(pkt->data());
                    iov_b.iov_len = body_len;
                    iovecs_.push_back(iov_b);
                }
                packet_end_indices_.push_back(iovecs_.size()); // 记录当前 Packet 结束的 iovec 索引
                count++;
            }

            return count; // 告诉调用者，我们处理了队列头部的 count 个包
        }

        /**
         * @brief 核心：处理部分写入 (Partial Write)
         * @param written_bytes 内核实际发送的字节数
         * @return size_t 已经完全发送完毕的 Packet 数量 (用于调用者 pop_front)
         */
        size_t advance(size_t written_bytes)
        {
            size_t packets_completed = 0;

            while (written_bytes > 0 && consumed_iov_index_ < iovecs_.size())
            {
                struct iovec &iov = iovecs_[consumed_iov_index_];

                if (written_bytes >= iov.iov_len)
                {
                    // Case A: 当前 iovec 被完全发送
                    written_bytes -= iov.iov_len;
                    consumed_iov_index_++; // 游标后移，不发生内存移动
                }
                else
                {
                    // Case B: 当前 iovec 发了一半 (Partial Write 终点)
                    // 修改指针和长度，等待下一次 syscall
                    // 注意：iov_base 是 void*，需强转 char* 进行算术运算
                    iov.iov_base = static_cast<char *>(iov.iov_base) + written_bytes;
                    iov.iov_len -= written_bytes;
                    written_bytes = 0;
                    // 此时 consumed_iov_index_ 不动，下次还从这里发
                }

                // check if we completed any packets
                // 检查 packet_end_indices_ 队列头部
                // 如果当前游标已经越过了某个 Packet 的结束边界，说明该 Packet 发完了
                while (!packet_end_indices_.empty() &&
                       consumed_iov_index_ >= packet_end_indices_.front())
                {
                    packets_completed++;
                    // 这里的 pop 是 vector::erase(begin)，因为这里只是记录索引的
                    // int 数组，且每次只有几十个，开销可忽略。
                    // 追求极致可以使用 deque 或双指针，但在 BATCH_LIMIT=64 下无所谓。
                    packet_end_indices_.erase(packet_end_indices_.begin());
                }
            }

            return packets_completed;
        }

        // Accessors for Syscall
        const struct iovec *iov_data() const { return iovecs_.data(); }
        int iov_count() const { return static_cast<int>(iovecs_.size()); }

        // 辅助：计算本次 Batch 总共要发多少字节（用于判断 Partial Write）
        size_t total_bytes() const
        {
            size_t bytes = 0;
            for (const auto &iov : iovecs_)
            {
                bytes += iov.iov_len;
            }
            return bytes;
        }

        std::span<struct iovec> remaining_iovecs()
        {
            if (consumed_iov_index_ >= iovecs_.size())
                return {};
            return {iovecs_.data() + consumed_iov_index_, iovecs_.size() - consumed_iov_index_};
        }

        bool is_empty() const { return consumed_iov_index_ >= iovecs_.size(); }

    private:
        // 真正传给内核的结构体数组
        std::vector<struct iovec> iovecs_;
        // [核心] 这里的 buffer 是专门为了让 iovec 有地方指！
        // 因为 Packet 里只有 Body，没有现成的 Big-Endian Header。
        std::vector<uint32_t> header_cache_;
        std::vector<size_t> packet_end_indices_; // 单调递增的索引
        size_t consumed_iov_index_ = 0;          // [Crucial] 游标
    };
}