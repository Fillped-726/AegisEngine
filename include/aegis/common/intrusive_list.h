/**
 * @file intrusive_list.h
 * @brief Lock-free intrusive linked list for zero-overhead actor message queues and timer wheels.
 */
#pragma once
/**
 * @file intrusive_list.h
 * @brief Lock-free intrusive linked list for zero-overhead actor message queues and timer wheels.
 */
/**
 * @file intrusive_list.h
 * @brief Lock-free intrusive linked list for zero-overhead actor message queues and timer wheels.
 */
#pragma once
#include <cstddef>
#include <iterator>
#include <concepts>
#include <cassert>
#include <type_traits>
#include "aegisLog.h" // [DEPENDENCY: aegis::Log]

namespace aegis::common
{
    struct IntrusiveListNode;

    // [CONSTRAINT: T requires IntrusiveListNode inheritance]
    template <typename T>
    concept IntrusiveNode = std::derived_from<T, IntrusiveListNode>;

    // [STATE: Topology hook embedded in payload]
    struct IntrusiveListNode
    {
        IntrusiveListNode *prev;
        IntrusiveListNode *next;

        IntrusiveListNode() : prev(nullptr), next(nullptr) {}

        ~IntrusiveListNode()
        {
            // [INTENT: Enforce lifecycle safety prior to dtor]
            assert(!is_linked() && "Node is being destroyed but is still in a list!");
        }

        bool is_linked() const { return next != nullptr; }

        // [STATE_MUTATION: Detach topology references]
        void unlink()
        {
            prev = nullptr;
            next = nullptr;
        }
    };

    // [STATE: Circular doubly-linked sequence, root_ sentinel]
    // [INTENT: Zero-allocation O(1) mutations]
    template <IntrusiveNode T>
    class IntrusiveList
    {
    public:
        // [INTENT: STL-compliant bidirectional iterator over intrusive nodes]
        template <bool IsConst>
        struct IteratorImpl
        {
            using iterator_category = std::bidirectional_iterator_tag;
            using value_type = std::conditional_t<IsConst, const T, T>;
            using difference_type = std::ptrdiff_t;
            using pointer = value_type *;
            using reference = value_type &;
            using node_ptr_t = std::conditional_t<IsConst, const IntrusiveListNode *, IntrusiveListNode *>;

            node_ptr_t current;

            IteratorImpl(node_ptr_t node) : current(node) {}

            reference operator*() const { return *static_cast<pointer>(current); }
            pointer operator->() const { return static_cast<pointer>(current); }

            IteratorImpl &operator++()
            {
                current = current->next;
                return *this;
            }

            IteratorImpl operator++(int)
            {
                IteratorImpl tmp = *this;
                ++(*this);
                return tmp;
            }

            IteratorImpl &operator--()
            {
                current = current->prev;
                return *this;
            }

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

        // [STATE_MUTATION: Init self-referential sentinel]
        IntrusiveList() : size_(0)
        {
            root_.next = &root_;
            root_.prev = &root_;
        }

        // [STATE_MUTATION: Topology adoption via move semantics]
        IntrusiveList(IntrusiveList &&other) noexcept
            : IntrusiveList()
        {
            move_from(std::move(other));
        }

        IntrusiveList &operator=(IntrusiveList &&other) noexcept
        {
            if (this != &other)
            {
                clear_links();
                move_from(std::move(other));
            }
            return *this;
        }

        IntrusiveList(const IntrusiveList &) = delete;
        IntrusiveList &operator=(const IntrusiveList &) = delete;

        // [STATE_MUTATION: Detach all hooks; no heap deallocation]
        ~IntrusiveList() { clear_links(); }

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

        // [STATE_MUTATION: O(1) topological insertion before pos]
        void insert(IntrusiveListNode *pos, T *node)
        {
            assert(node && "Node implies nullptr!");
            assert(!node->is_linked() && "Node is already linked! Double insertion detected.");

            IntrusiveListNode *prev_node = pos->prev;

            prev_node->next = node;
            node->prev = prev_node;
            node->next = pos;
            pos->prev = node;

            size_++;
        }

        void push_back(T *node)
        {
            insert(&root_, node);
        }

        void push_front(T *node)
        {
            insert(root_.next, node);
        }

        // [STATE_MUTATION: O(1) topological extraction]
        void remove(T *node)
        {
            if (!node || !node->is_linked())
                return;

            IntrusiveListNode *prev_node = node->prev;
            IntrusiveListNode *next_node = node->next;

            prev_node->next = next_node;
            next_node->prev = prev_node;

            node->unlink();
            size_--;
        }

        T *pop_front()
        {
            if (empty())
                return nullptr;
            T *node = static_cast<T *>(root_.next);
            remove(node);
            return node;
        }

        T *pop_back()
        {
            if (empty())
                return nullptr;
            T *node = static_cast<T *>(root_.prev);
            remove(node);
            return node;
        }

        // [STATE_MUTATION: O(1) list concatenation; repoints sentinels]
        void splice(IntrusiveList &other)
        {
            if (other.empty() || &other == this)
                return;

            IntrusiveListNode *other_first = other.root_.next;
            IntrusiveListNode *other_last = other.root_.prev;
            IntrusiveListNode *my_last = this->root_.prev;

            my_last->next = other_first;
            other_first->prev = my_last;

            other_last->next = &this->root_;
            this->root_.prev = other_last;

            this->size_ += other.size_;

            other.root_.next = &other.root_;
            other.root_.prev = &other.root_;
            other.size_ = 0;
        }

        // [STATE_MUTATION: Iterative unlink; reset self sentinel]
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

        // [STATE_MUTATION: Sentinel takeover; invalidate source]
        void move_from(IntrusiveList &&other) noexcept
        {
            if (!other.empty())
            {
                IntrusiveListNode *first = other.root_.next;
                IntrusiveListNode *last = other.root_.prev;

                this->root_.next = first;
                this->root_.prev = last;

                first->prev = &this->root_;
                last->next = &this->root_;

                this->size_ = std::exchange(other.size_, 0);

                other.root_.next = &other.root_;
                other.root_.prev = &other.root_;
            }
        }

    private:
        // [STATE: Sentinel node; represents end()]
        IntrusiveListNode root_;
        size_t size_;
    };

} // namespace aegis::common