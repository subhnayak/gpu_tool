#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <type_traits>

namespace microalloc {

// The node that gets embedded directly into user objects.
// Intrusive containers matter when you need to avoid a second allocation just to hold links:
// job schedulers, GUI trees, game-engine entity lists, OS kernels, allocators, and tooling code
// often already control the object layout and want the links inside that object.
struct IntrusiveListNode {
    IntrusiveListNode* prev;
    IntrusiveListNode* next;

    IntrusiveListNode() noexcept : prev(this), next(this) {}
};

template <typename T, IntrusiveListNode T::* Member>
class IntrusiveList {
    static_assert(std::is_standard_layout<T>::value,
                  "IntrusiveList uses pointer arithmetic from the embedded node; T must be standard-layout");

private:
    template <bool IsConst>
    class Iterator {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = typename std::conditional<IsConst, const T*, T*>::type;
        using reference = typename std::conditional<IsConst, const T&, T&>::type;

        Iterator() noexcept : node_(nullptr) {}
        explicit Iterator(IntrusiveListNode* node) noexcept : node_(node) {}

        template <bool OtherConst, typename = typename std::enable_if<IsConst || !OtherConst>::type>
        Iterator(const Iterator<OtherConst>& other) noexcept : node_(other.node_) {}

        reference operator*() const { return *value_from_node(node_); }
        pointer operator->() const { return value_from_node(node_); }

        Iterator& operator++() noexcept {
            node_ = node_->next;
            return *this;
        }

        Iterator operator++(int) noexcept {
            Iterator copy(*this);
            ++(*this);
            return copy;
        }

        Iterator& operator--() noexcept {
            node_ = node_->prev;
            return *this;
        }

        Iterator operator--(int) noexcept {
            Iterator copy(*this);
            --(*this);
            return copy;
        }

        friend bool operator==(const Iterator& lhs, const Iterator& rhs) noexcept { return lhs.node_ == rhs.node_; }
        friend bool operator!=(const Iterator& lhs, const Iterator& rhs) noexcept { return !(lhs == rhs); }

    private:
        friend class IntrusiveList;

        static pointer value_from_node(IntrusiveListNode* node) noexcept {
            return IntrusiveList::value_from_node(node);
        }

        IntrusiveListNode* node_;
    };

public:
    using value_type = T;
    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    IntrusiveList() noexcept : sentinel_(), size_(0) {}

    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }

    iterator begin() noexcept { return iterator(sentinel_.next); }
    iterator end() noexcept { return iterator(&sentinel_); }
    const_iterator begin() const noexcept { return const_iterator(sentinel_.next); }
    const_iterator end() const noexcept { return const_iterator(const_cast<IntrusiveListNode*>(&sentinel_)); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    T& front() noexcept {
        assert(!empty());
        return *value_from_node(sentinel_.next);
    }

    T& back() noexcept {
        assert(!empty());
        return *value_from_node(sentinel_.prev);
    }

    void push_front(T& value) noexcept { insert(begin(), value); }
    void push_back(T& value) noexcept { insert(end(), value); }

    iterator insert(iterator position, T& value) noexcept {
        IntrusiveListNode* node = node_from_value(value);
        assert(!is_linked(*node) && "node is already linked into a list");

        IntrusiveListNode* before = position.node_->prev;
        IntrusiveListNode* after = position.node_;

        before->next = node;
        node->prev = before;
        node->next = after;
        after->prev = node;

        ++size_;
        return iterator(node);
    }

    iterator erase(iterator position) noexcept {
        IntrusiveListNode* node = position.node_;
        assert(node != &sentinel_ && "cannot erase end() sentinel");

        IntrusiveListNode* before = node->prev;
        IntrusiveListNode* after = node->next;
        before->next = after;
        after->prev = before;

        node->next = node;
        node->prev = node;
        --size_;
        return iterator(after);
    }

    void clear() noexcept {
        while (!empty()) {
            erase(begin());
        }
    }

private:
    static constexpr std::ptrdiff_t member_offset() noexcept {
        return reinterpret_cast<std::ptrdiff_t>(&(reinterpret_cast<T const volatile*>(0)->*Member));
    }

    static T* value_from_node(IntrusiveListNode* node) noexcept {
        auto* bytes = reinterpret_cast<unsigned char*>(node);
        return reinterpret_cast<T*>(bytes - member_offset());
    }

    static const T* value_from_node(const IntrusiveListNode* node) noexcept {
        auto* bytes = reinterpret_cast<const unsigned char*>(node);
        return reinterpret_cast<const T*>(bytes - member_offset());
    }

    static IntrusiveListNode* node_from_value(T& value) noexcept {
        return &(value.*Member);
    }

    static bool is_linked(const IntrusiveListNode& node) noexcept {
        return !(node.prev == &node && node.next == &node);
    }

    IntrusiveListNode sentinel_;
    std::size_t size_;
};

} // namespace microalloc
