/**
 * @file packetPool.h
 * @brief ObjectPool specialization for Packet, plus RAII PooledPacket alias.
 */
#pragma once
#include "aegis/common/objectPool.h" // [DEPENDENCY: aegis::core::ObjectPool]
#include "aegis/net/packet.h"        // [DEPENDENCY: aegis::net::Packet]

namespace aegis::net
{
    // [CONSTRAINT: Enforce minimum Small Buffer Optimization (SBO) capacity]
    static_assert(sizeof(Packet) >= 1024, "Packet SBO size is too small!");

    // [CONSTRAINT: Enforce strict ABI memory layout and padding alignment]
    static_assert(sizeof(Packet) == 1072, "ABI Alert: Packet size is not 1056. Check padding/alignas!");

    // [STATE: Singleton pool definition; L2 capacity=100k, TLS L1 batch=128]
    using PacketPool = aegis::core::ObjectPool<Packet, 100000, 128>;

    // [STATE: RAII auto-release smart pointer alias]
    using PooledPacket = PacketPool::Ptr;
}