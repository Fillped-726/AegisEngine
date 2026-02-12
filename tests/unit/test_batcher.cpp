#include <gtest/gtest.h>
#include <vector>
#include <memory>
#include <cstring>
#include <arpa/inet.h>
#include "aegis/net/outbox_batcher.h"
#include "aegis/net/packet.h"

using namespace aegis::net;

// [修改 1] 按照你的要求，测试 unique_ptr 场景
using PacketPtr = std::unique_ptr<Packet>;

class OutboxBatcherTest : public ::testing::Test
{
protected:
    OutboxBatcher batcher;
    std::vector<PacketPtr> queue;

    // 辅助：创建一个指定大小和内容的 SBO 包 (返回 unique_ptr)
    PacketPtr make_pkt(size_t size, char content)
    {
        // Packet::create 应该返回 unique_ptr 或者我们直接 make_unique
        // 假设 Packet 类有 alloc 方法
        auto p = std::make_unique<Packet>();
        p->alloc(size);
        if (size > 0)
        {
            std::memset(p->mutable_data(), content, size);
        }
        return p;
    }

    void SetUp() override
    {
        queue.clear();
    }
};

TEST_F(OutboxBatcherTest, HandleEmptyQueue)
{
    size_t count = batcher.prepare_batch(queue);
    EXPECT_EQ(count, 0);
    EXPECT_TRUE(batcher.remaining_iovecs().empty());
}

// SBO 栈内存测试
TEST_F(OutboxBatcherTest, BatchesSmallPacket_StackMemory)
{
    size_t small_size = 100;
    auto pkt = make_pkt(small_size, 'S');

    // [技巧] 因为 unique_ptr 马上要被 move 进队列，pkt 会变空
    // 所以必须提前保存裸指针用于后续验证
    const char *original_ptr = pkt->data();

    // [修改 2] 必须使用 std::move
    queue.push_back(std::move(pkt));

    size_t count = batcher.prepare_batch(queue);
    EXPECT_EQ(count, 1);

    auto iovs = batcher.remaining_iovecs();
    ASSERT_EQ(iovs.size(), 2);

    // 验证 Header
    EXPECT_EQ(iovs[0].iov_len, 4);
    uint32_t header_val = *reinterpret_cast<uint32_t *>(iovs[0].iov_base);
    EXPECT_EQ(header_val, htonl(small_size));

    // 验证 Body (指向栈地址)
    EXPECT_EQ(iovs[1].iov_len, small_size);
    EXPECT_EQ(iovs[1].iov_base, original_ptr); // 验证地址未变
}

// SBO 堆内存测试
TEST_F(OutboxBatcherTest, BatchesLargePacket_HeapMemory)
{
    size_t large_size = 2048; // Heap
    auto pkt = make_pkt(large_size, 'H');

    const char *original_ptr = pkt->data(); // 保存裸指针

    queue.push_back(std::move(pkt)); // 移交所有权

    batcher.prepare_batch(queue);
    auto iovs = batcher.remaining_iovecs();

    EXPECT_EQ(iovs[1].iov_len, large_size);
    EXPECT_EQ(iovs[1].iov_base, original_ptr); // 验证地址未变
}

// 混合测试
TEST_F(OutboxBatcherTest, MixStackAndHeapPackets)
{
    auto p1 = make_pkt(10, 'A');
    auto p2 = make_pkt(4096, 'B');
    auto p3 = make_pkt(20, 'C');

    // 提前保存指针
    const char *ptr2 = p2->data();

    // 依次移动
    queue.push_back(std::move(p1));
    queue.push_back(std::move(p2));
    queue.push_back(std::move(p3));

    size_t count = batcher.prepare_batch(queue);
    EXPECT_EQ(count, 3);

    auto iovs = batcher.remaining_iovecs();
    ASSERT_EQ(iovs.size(), 6);

    // 检查第2个包
    EXPECT_EQ(iovs[3].iov_len, 4096);
    EXPECT_EQ(iovs[3].iov_base, ptr2);
}

// Advance 测试 - 部分发送
TEST_F(OutboxBatcherTest, AdvancePartialBody)
{
    auto pkt = make_pkt(100, 'Y');
    const char *raw_ptr = pkt->data();

    queue.push_back(std::move(pkt));
    batcher.prepare_batch(queue);

    // 发送 Header(4) + Body(50) = 54 字节
    size_t completed = batcher.advance(54);

    EXPECT_EQ(completed, 0);
    auto iovs = batcher.remaining_iovecs();

    ASSERT_EQ(iovs.size(), 1); // Header 没了

    // Body 还剩 50
    EXPECT_EQ(iovs[0].iov_len, 50);
    // 指针偏移验证
    EXPECT_EQ(iovs[0].iov_base, raw_ptr + 50);
}

// 跨包发送
TEST_F(OutboxBatcherTest, AdvanceMultiplePackets)
{
    queue.push_back(make_pkt(10, '1'));
    queue.push_back(make_pkt(20, '2'));

    batcher.prepare_batch(queue);

    // 14 + 24 = 38
    size_t completed = batcher.advance(38);

    EXPECT_EQ(completed, 2);
    EXPECT_TRUE(batcher.remaining_iovecs().empty());
}