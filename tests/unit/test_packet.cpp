#include <gtest/gtest.h>
#include <cstring>
#include "aegis/net/packet.h"

using namespace aegis::net;

// 1. SBO 基础测试：小内存分配
TEST(PacketTest, AllocSmallBuffer)
{
    Packet p;
    // 分配 100 字节，小于 kSmallBufferSize (1024)
    p.alloc(100);

    EXPECT_EQ(p.size(), 100);
    // 验证是否写在栈上 (无法直接访问 private，但可以通过地址距离判断，或者信任逻辑)
    // 这里我们主要验证功能
    std::memset(p.mutable_data(), 'A', 100);
    EXPECT_EQ(p.data()[0], 'A');
    EXPECT_EQ(p.data()[99], 'A');
}

// 2. 堆内存测试：大内存分配
TEST(PacketTest, AllocHeapBuffer)
{
    Packet p;
    size_t large_size = 2048; // > 1024
    p.alloc(large_size);

    EXPECT_EQ(p.size(), large_size);

    // 填充数据测试
    char *ptr = p.mutable_data();
    ptr[0] = 'H';
    ptr[large_size - 1] = 'E';

    EXPECT_EQ(p.data()[0], 'H');
    EXPECT_EQ(p.data()[large_size - 1], 'E');
}

// 3. SBO 自动扩容测试 (Stack -> Heap)
TEST(PacketTest, GrowFromStackToHeap)
{
    Packet p;
    p.alloc(512); // Stack
    std::memset(p.mutable_data(), 'S', 512);

    // 记录旧地址
    const char *old_ptr = p.data();

    // 扩容到 2048 (Heap)
    p.alloc(2048);

    EXPECT_EQ(p.size(), 2048);
    // 地址应该变了
    EXPECT_NE(p.data(), old_ptr);
    // 旧数据应该还在
    EXPECT_EQ(p.data()[0], 'S');
    EXPECT_EQ(p.data()[511], 'S');
}

// 4. 拷贝构造测试 (Deep Copy)
TEST(PacketTest, CopyConstructor)
{
    // Case A: Stack Copy
    Packet p1;
    p1.alloc(10);
    std::memcpy(p1.mutable_data(), "123456789", 10);

    Packet p2 = p1; // Copy
    EXPECT_EQ(p2.size(), 10);
    EXPECT_EQ(std::memcmp(p2.data(), p1.data(), 10), 0);
    // 地址必须不同 (Deep Copy)
    EXPECT_NE(p2.data(), p1.data());

    // Case B: Heap Copy
    Packet p3;
    p3.alloc(2000);
    p3.mutable_data()[0] = 'X';

    Packet p4 = p3;
    EXPECT_EQ(p4.size(), 2000);
    EXPECT_EQ(p4.data()[0], 'X');
    EXPECT_NE(p4.data(), p3.data()); // 必须是深拷贝
}

// 5. 移动语义测试 (Ownership Transfer)
TEST(PacketTest, MoveSemantics)
{
    // Case A: Heap Move (Should steal pointer)
    Packet p1;
    p1.alloc(2000);
    char *p1_ptr = p1.mutable_data();
    p1_ptr[0] = 'Z';

    Packet p2 = std::move(p1);

    // p2 应该接管了 p1 的堆内存地址
    EXPECT_EQ(p2.size(), 2000);
    EXPECT_EQ(p2.data(), p1_ptr); // 指针地址应该不变！
    EXPECT_EQ(p2.data()[0], 'Z');

    // p1 应该被重置
    EXPECT_EQ(p1.size(), 0);

    // Case B: Stack Move (Should memcpy)
    Packet p3;
    p3.alloc(100);
    p3.mutable_data()[0] = 'A';
    const char *p3_old_ptr = p3.data();

    Packet p4 = std::move(p3);
    EXPECT_EQ(p4.size(), 100);
    EXPECT_EQ(p4.data()[0], 'A');
    // 对于 Stack move，p4 的 data() 指向 p4 自己的栈，肯定不等于 p3 的栈地址
    EXPECT_NE(p4.data(), p3_old_ptr);
}

// 6. MsgID Endian 测试
TEST(PacketTest, MsgIDEndian)
{
    Packet p;
    p.alloc(4); // Only header

    uint32_t id = 0x12345678;

    // 手动按 Big Endian 写入
    // 0x12, 0x34, 0x56, 0x78
    unsigned char *raw = (unsigned char *)p.mutable_data();
    raw[0] = 0x12;
    raw[1] = 0x34;
    raw[2] = 0x56;
    raw[3] = 0x78;

    // msg_id() 应该能正确转回 Host Byte Order
    // 如果是 Little Endian 机器 (x86/ARM)，它会做 swap
    // 0x12345678 (Big) -> 0x12345678 (Host Value)
    EXPECT_EQ(p.msg_id(), 0x12345678);
}