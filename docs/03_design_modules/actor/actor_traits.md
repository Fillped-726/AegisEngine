# Aegis Engine: Actor Traits (生命周期策略)

### 1. 模块定义 (What & Why)

- **一句话定位：** `actor_traits` 定义了一组 **策略模板类 (`Policy Classes`)**，用于剥离 Actor 的 **"业务逻辑"** 与 **"内存管理逻辑"**。
    
- **设计初衷 (Motivation)：**
    
    - **消除重复代码：** 如果没有这个模块，每个具体的 Actor（如 `PlayerActor`, `BulletActor`）都需要自己写一遍 `ObjectPool::acquire()` 和 `release()` 的逻辑。
        
    - **灵活切换：** 有些 Actor（如单例管理器）适合用 `new/delete`，有些（如子弹）必须用对象池。通过继承不同的 Trait，一键切换内存策略，无需修改业务代码。
        

### 2. 核心技术决策 (Key Decisions & Trade-offs)

#### A. 静态多态 (CRTP) 的应用

- **代码特征：** `class PooledActor : public Actor`，其中 `Derived` 是模板参数。
    
- **决策：** 使用 **CRTP (Curiously Recurring Template Pattern)**。
    
- **理由 (面试高频)：**
    
    - **类型感知：** `Actor` 基类不知道具体的子类是谁。但 `PooledActor<T>` 知道 `T` 是谁。这使得我们可以在基类中调用 `ObjectPool<T>::instance()`，而不需要在每个子类里写死。
        
    - **性能：** 所有的工厂方法 `create` 和销毁逻辑都是编译期生成的，没有运行时开销。
        

#### B. 工厂方法 (Static Factory)

- **决策：** 强制通过 `create()` 静态方法创建实例，而不是直接 `new`。
    
- **理由：**
    
    - **统一入口：** 调用者不需要关心这个 Actor 是从堆上分配的，还是从池子里捞出来的。
        
    - **参数完美转发：** 使用 `Args&&... args` 和 `std::forward`，支持任意形式的构造函数参数。
        

### 3. 关键实现细节 (Implementation Deep Dive)

#### A. `PooledActor` 的内存闭环

```
template <typename Derived, size_t PoolSize, ...>
class PooledActor : public Actor {
    // 1. 定义专属池子
    using PoolType = ObjectPool<Derived, PoolSize, ...>;

    // 2. 借出 (Factory)
    static Derived *create(...) {
        return PoolType::instance().acquire(...);
    }

    // 3. 归还 (Virtual Finalize)
    void finalize() override {
        // 必须强转回子类指针，因为 ObjectPool 存的是 Derived
        PoolType::instance().release(static_cast<Derived *>(this));
    }
};
```

- **解析：**
    
    - 这里的 `finalize` 覆盖了 `Actor` 基类的纯虚函数。
        
    - 当 `Actor::process_batch` 检测到 `MSG_TYPE_DESTROY` 时，它不直接 `delete`，而是调用 `finalize()`。
        
    - `finalize` 内部将 `this` 指针（基类指针）转回 `Derived*`，然后扔回对应的对象池。**这是实现“自动回收”的关键桥梁。**
        

#### B. `SimpleActor` 的兜底策略

- 对于 `RoomManager` 或 `GlobalSettings` 这种全服只有一个的单例 Actor，开一个 1000 大小的对象池是纯粹的浪费。
    
- `SimpleActor` 提供标准的 `new/delete` 封装，接口与 `PooledActor` 保持一致（都有 `create` 和 `finalize`），使得上层代码（如 Registry）可以用统一的模板逻辑处理它们。
    

### 4. 踩坑与难点 (Challenges & Solutions)

#### 难点 1：虚析构与对象池的冲突

- **问题：** 如果直接 `delete actor`，会触发析构函数并释放内存给 OS。但对于对象池，我们**不希望释放内存**，只希望调用析构函数（清理资源）并把内存块还给池子。
    
- **解决：**
    
    - 禁止外部直接调用 `delete actor`（可以通过将析构函数设为 `protected` 来强制，但为了方便这里没做）。
        
    - 引入 `finalize()` 语义。`finalize` 负责“归还”这个动作。对于 `SimpleActor`，归还=delete；对于 `PooledActor`，归还=release。
        

#### 难点 2：类型安全

- **风险：** `static_cast<Derived*>(this)` 是强制转换。如果继承关系写错了（例如 `class A : public PooledActor<B>`），会导致未定义行为。
    
- **解决：** CRTP 的标准写法虽然无法完全禁止这种错误，但现代编译器通常会给出警告。在 `SimpleActor` 中加了 `static_assert` 检查类型完整性。
    

### 5. 性能复杂度 (Complexity)

- **PooledActor:**
    
    - 创建/销毁：**O(1)** (对象池操作)。
        
    - 内存开销：`PoolSize * sizeof(Derived)` (启动时预分配)。
        
- **SimpleActor:**
    
    - 创建/销毁：**Unspecified** (取决于 OS 的 malloc/free，可能有锁，可能有碎片)。
        

### 6. 面试模拟 (Interview Q&A)

**Q: 这里的 CRTP 有什么作用？**

**A:** CRTP 在这里主要用于实现**静态多态的内存管理**。`Actor` 基类是通用的，不知道具体的子类类型。通过 `class MyActor : public PooledActor<MyActor>`，我们将 `MyActor` 的类型信息“注入”到了父类中。这样，父类中的 `finalize` 函数就能知道应该去哪个类型的 `ObjectPool`（例如 `ObjectPool<MyActor>`）归还自己。

**Q: 为什么需要 `finalize` 函数？为什么不直接在析构函数里归还池子？**

**A:** 这是一个经典的陷阱。

1. 如果在析构函数里调用 `pool.release(this)`，那么当 `delete actor` 发生时，析构函数执行，对象被还给池子，**紧接着** `delete` 运算符会将这块内存释放给操作系统。结果就是池子里存了一个悬垂指针。
    
2. 我们需要的是一个**替代 delete 的动作**。`finalize` 就是这个动作：它告诉系统“我结束了”，然后由具体的策略决定是 `delete`（还给 OS）还是 `release`（还给池子）。
    

**Q: `PoolSize` 作为模板参数有什么好处？**

**A:** 不同的 Actor 数量级差异巨大。`PlayerActor` 可能需要 5000 个，而 `BulletActor` 可能需要 100,000 个。将 PoolSize 参数化，可以在编译期为每种 Actor 生成专属大小的池子，避免了“大材小用”或“不够用”的情况，实现了静态配置的灵活性。