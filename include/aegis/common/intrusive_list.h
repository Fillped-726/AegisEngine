#pragma once

#include <cstddef>
#include <iterator>
#include <concepts>
#include <cassert>
#include <type_traits>
#include "aegisLog.h" // 集成日志系统

namespace aegis::common
{
    struct IntrusiveListNode;

    // C++20 Concept: 强制 T 必须派生自 IntrusiveListNode
    template <typename T>
    concept IntrusiveNode = std::derived_from<T, IntrusiveListNode>;

    // 1. 定义通用钩子基类
    struct IntrusiveListNode
    {
        IntrusiveListNode *prev;
        IntrusiveListNode *next;

        IntrusiveListNode() : prev(nullptr), next(nullptr) {}

        ~IntrusiveListNode()
        {
            assert(!is_linked() && "Node is being destroyed but is still in a list!");
        }

        bool is_linked() const { return next != nullptr; }

        // 重置状态
        void unlink()
        {
            prev = nullptr;
            next = nullptr;
        }
    };

    /**
     * @brief 带哨兵的侵入式循环双向链表
     * @details
     * 1. 总是包含一个 root_ 哨兵节点。
     * 2. 空表时：root_.next == &root_, root_.prev == &root_
     * 3. 这里的 end() 迭代器指向 &root_，而不是 nullptr。
     */
    template <IntrusiveNode T>
    class IntrusiveList
    {
    public:
        // --- 迭代器定义 (支持 const 和 non-const) ---
        template <bool IsConst>
        struct IteratorImpl
        {
            using iterator_category = std::bidirectional_iterator_tag; // 升级为双向迭代器
            using value_type = std::conditional_t<IsConst, const T, T>;
            using difference_type = std::ptrdiff_t;
            using pointer = value_type *;
            using reference = value_type &;
            using node_ptr_t = std::conditional_t<IsConst, const IntrusiveListNode *, IntrusiveListNode *>;

            node_ptr_t current;

            IteratorImpl(node_ptr_t node) : current(node) {}

            reference operator*() const { return *static_cast<pointer>(current); }
            pointer operator->() const { return static_cast<pointer>(current); }

            // 前置 ++
            IteratorImpl &operator++()
            {
                current = current->next;
                return *this;
            }

            // 后置 ++
            IteratorImpl operator++(int)
            {
                IteratorImpl tmp = *this;
                ++(*this);
                return tmp;
            }

            // 前置 -- (双向链表特性)
            IteratorImpl &operator--()
            {
                current = current->prev;
                return *this;
            }

            // 后置 -- (返回旧值)
            IteratorImpl operator--(int)
            {
                IteratorImpl tmp = *this;
                --(*this);
                return tmp;
            }

            bool operator!=(const IteratorImpl &other) const { return current != other.current; }
            bool operator==(const IteratorImpl &other) const { return current == other.current; }
        };

        using iterator = IteratorImpl<false>;
        using const_iterator = IteratorImpl<true>;

        // --- 构造与析构 ---
        IntrusiveList() : size_(0)
        {
            root_.next = &root_;
            root_.prev = &root_;
        }

        // 移动构造
        // 构造函数初始化列表先把自己初始化为空
        IntrusiveList(IntrusiveList &&other) noexcept
            : IntrusiveList() // 委托给默认构造函数（C++11），先把自己初始化好
        {
            move_from(std::move(other));
        }

        IntrusiveList &operator=(IntrusiveList &&other) noexcept
        {
            // 1. 防止自我赋值 (a = std::move(a))
            if (this != &other)
            {
                // 2. [关键] 先清理自己的旧数据！
                // 如果是智能指针管理生命周期，这里可能需要 delete
                // 如果是纯侵入式链表（不管理生命周期），这里只是解绑
                clear_links();

                move_from(std::move(other));
            }
            return *this;
        }

        // 禁止拷贝
        IntrusiveList(const IntrusiveList &) = delete;
        IntrusiveList &operator=(const IntrusiveList &) = delete;

        // 析构：侵入式链表通常不负责 delete 节点，只负责断开连接
        // 但为了安全，可以在析构时将所有节点的钩子置空
        ~IntrusiveList() { clear_links(); }

        // --- 标准容器接口 ---
        iterator begin() { return iterator(&root_.next); }
        iterator end() { return iterator(&root_); }

        const_iterator begin() const { return const_iterator(&root_.next); }
        const_iterator end() const { return const_iterator(&root_); }
        const_iterator cbegin() const { return const_iterator(&root_.next); }
        const_iterator cend() const { return const_iterator(&root_); }

        bool empty() const { return root_.next == &root_; }
        size_t size() const { return size_; }

        T *front() { return empty() ? nullptr : static_cast<T *>(root_.next); }
        T *back() { return empty() ? nullptr : static_cast<T *>(root_.prev); }

        // --- 核心操作 ---

        // 内部通用插入：在 pos 之前插入 node
        void insert(IntrusiveListNode *pos, T *node)
        {
            // 1. 硬核防御：Release 模式下完全消失，Debug 下拦截错误
            assert(node && "Node implies nullptr!");
            assert(!node->is_linked() && "Node is already linked! Double insertion detected.");

            IntrusiveListNode *prev_node = pos->prev;

            // 核心指针操作：无 if 判断
            prev_node->next = node;
            node->prev = prev_node;
            node->next = pos;
            pos->prev = node;

            size_++;
        }

        // O(1) 尾插
        void push_back(T *node)
        {
            insert(&root_, node);
        }

        // O(1) 头插
        void push_front(T *node)
        {
            insert(root_.next, node);
        }

        // O(1) 移除任意节点
        void remove(T *node)
        {
            if (!node || !node->is_linked())
                return;

            // 核心指针操作：无 if 判断
            // 即使链表只有这一个节点，node->next 和 node->prev 也会指向 root_，逻辑依然成立
            IntrusiveListNode *prev_node = node->prev;
            IntrusiveListNode *next_node = node->next;

            prev_node->next = next_node;
            next_node->prev = prev_node;

            node->unlink();
            size_--;
        }

        // O(1) 弹出头部
        T *pop_front()
        {
            if (empty())
                return nullptr;
            T *node = static_cast<T *>(root_.next);
            remove(node);
            return node;
        }

        // O(1) 弹出尾部
        T *pop_back()
        {
            if (empty())
                return nullptr;
            T *node = static_cast<T *>(root_.prev);
            remove(node);
            return node;
        }

        // 拼接链表
        void splice(IntrusiveList &other)
        {
            if (other.empty() || &other == this)
                return;

            IntrusiveListNode *other_first = other.root_.next;
            IntrusiveListNode *other_last = other.root_.prev;
            IntrusiveListNode *my_last = this->root_.prev;

            // 1. 把 other 的整段 挂到 this 的尾部
            my_last->next = other_first;
            other_first->prev = my_last;

            other_last->next = &this->root_;
            this->root_.prev = other_last;

            // 2. 更新大小
            this->size_ += other.size_;

            // 3. 重置 other
            other.root_.next = &other.root_;
            other.root_.prev = &other.root_;
            other.size_ = 0;
        }

        // 清理链表关系（不 delete 节点）
        void clear_links()
        {
            IntrusiveListNode *curr = root_.next;
            while (curr != &root_)
            {
                IntrusiveListNode *next = curr->next;
                curr->unlink();
                curr = next;
            }
            root_.next = &root_;
            root_.prev = &root_;
            size_ = 0;
        }
        void move_from(IntrusiveList &&other) noexcept
        {
            if (!other.empty())
            {
                // 1. 拿到对方的货 (First & Last)
                IntrusiveListNode *first = other.root_.next;
                IntrusiveListNode *last = other.root_.prev;

                // 2. 挂到自己名下
                this->root_.next = first;
                this->root_.prev = last;

                // 3. 告诉货（节点），新老板是我
                first->prev = &this->root_;
                last->next = &this->root_;

                // 4. 偷取 size，同时顺手把对方的 size 置 0 (一行代码搞定)
                this->size_ = std::exchange(other.size_, 0);

                // 5. 修复对方的哨兵（恢复成空表状态）
                other.root_.next = &other.root_;
                other.root_.prev = &other.root_;
            }
        }

    private:
        IntrusiveListNode root_;
        size_t size_;
    };

} // namespace aegis::common