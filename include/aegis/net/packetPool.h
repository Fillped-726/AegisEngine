#pragma once
#include "aegis/common/objectPool.h"
#include "aegis/net/packet.h"

namespace aegis::net
{
    // 定义 PacketPool
    // 全局最大 10万个包，每个线程本地缓存 128 个
    using PacketPool = aegis::core::ObjectPool<Packet, 100000, 128>;
    using PooledPacket = PacketPool::Ptr;
}
