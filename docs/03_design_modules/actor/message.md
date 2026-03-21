# Aegis Actor 框架：核心消息系统 (Message System) 技术文档

## 1. 架构概述与设计哲学

本文件定义了 Aegis Actor 框架底层的消息流转协议与数据结构。作为一个面向高并发、低延迟服务端（如游戏服务器或网关）的底层组件，其设计严格遵循**零成本抽象（Zero-cost Abstraction）和极致内存友好**的原则。

核心设计亮点：

- **无虚函数开销：** 彻底剔除 `virtual` 关键字，压缩对象体积，提升 CPU Cache 命中率。
    
- **静态多态：** 广泛运用 CRTP（奇异递归模板模式）解决无虚析构带来的多态释放问题。
    
- **异构生命周期管理：** 统一 `finalize()` 接口，无缝兼容普通消息的按需析构与高频网络消息的对象池复用。
    
- **现代 C++ 赋能：** 深度集成 C++20 协程（Coroutine）、Promise 模型以及 Concepts 编译期约束。
    

---

## 2. 核心模块解析

## 2.1 基础消息头：侵入式无锁队列节点

C++

```
struct ActorMessage {
    std::atomic<ActorMessage *> next{nullptr};
    uint8_t type_id = MSG_TYPE_BASE;
    ~ActorMessage() = default; 
};
```

- **侵入式设计 (Intrusive)：** 内置 `atomic<ActorMessage*> next` 指针。消息对象本身即是 MPSC（多生产者单消费者）无锁队列的节点，避免了将消息压入队列时产生额外的堆内存分配（如 `std::queue` 底层的 Node 分配）。
    
- **内存对齐与去虚化：** 强制去除 `virtual` 析构函数，省去 8 字节的 vptr 开销。配合 `uint8_t` 的类型枚举，使基础头保持极简且严格内存对齐，极其契合高频的内存池分配。
    

## 2.2 静态多态与安全回收：CRTP 模板机制

由于基类 `ActorMessage` 没有虚析构函数，直接 `delete ActorMessage*` 会导致派生类资源泄漏。框架通过 `BasicMessage` 模板结合 CRTP 完美破局：

C++

```
template <typename Derived, uint8_t TypeId>
struct BasicMessage : public ActorMessage {
    BasicMessage() { type_id = TypeId; }
    void finalize() {
        static_assert(sizeof(Derived) > 0, "Derived must be a complete type");
        delete static_cast<Derived *>(this);
    }
};
```

- **编译期向下转型：** 框架在调度处理完消息后，统一调用 `finalize()`。由于 CRTP 在编译期已知具体的子类类型 `Derived`，通过 `static_cast` 强转后 `delete`，可精确调用子类的析构函数。
    
- **安全防御：** 内部的 `static_assert` 拦截了前置声明导致的未定义行为（UB），将运行时内存泄漏风险扼杀在编译期。
    

## 2.3 高频/特殊消息的异构处理

消息系统根据业务特征，提供了两种特殊的生命周期与流转方式：

- **NetworkMessage (网络消息与对象池)：**
    
    - **痛点：** 网关面临海量网络包，频繁 `new/delete` 会引发内存碎片和极高的分配延迟。
        
    - **实现：** 不继承 `BasicMessage`，直接重写 `finalize()`。处理完毕后不执行 `delete`，而是调用 `NetworkMessagePool::instance().release(this)`，将自己放回全局无锁对象池。实现高频流量下的零内存分配。
        
- **RpcMessage (协程与异步化)：**
    
    - **实现：** 内部持有 `mutable std::promise<ResT> promise`。
        
    - **机制：** 允许发送方 Actor 挂起当前协程等待回包。接收方处理完后调用 `Reply()` 设置 promise 值，直接唤醒发送方的协程。彻底消除跨 Actor 通信的回调地狱（Callback Hell）。
        

## 2.4 架构级安全：C++20 Concept 契约

C++

```
template <typename T>
concept FinalizableMessage = std::derived_from<T, ActorMessage> && requires(T m) {
    { m.finalize() } -> std::same_as<void>;
};
```

利用 C++20 的 Concepts，在编译期强制要求所有进入 Actor 邮箱的自定义消息类型必须满足：

1. 继承自 `ActorMessage`。
    
2. 实现了 `void finalize()` 方法。 这保证了无论是对象池回收还是直接销毁，系统永远不会遗漏清理逻辑，从语法层面实现了架构规范的强约束。