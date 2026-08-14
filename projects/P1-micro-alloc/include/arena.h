#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace microalloc {

// Arena allocators are intentionally simple:
// - allocate from a large block by "bumping" a pointer forward,
// - never free individual objects,
// - reclaim everything at once with reset().
//
// This is a great fit when many objects share the same lifetime:
// one parser pass, one HTTP request, one simulation frame, one compiler phase, etc.
// The trade-off is deliberate: you give up per-object free so allocation becomes extremely cheap.
class Arena {
public:
    explicit Arena(std::size_t block_size = 64 * 1024)
        : block_size_(std::max<std::size_t>(block_size, 64U)), head_(nullptr), current_(nullptr), block_count_(0) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    ~Arena() { release_all(); }

    // Allocate raw storage with a requested alignment.
    // We use manual pointer arithmetic instead of storing one allocator object per allocation.
    // That means allocation cost is mostly a few integer operations and one bounds check.
    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
        if (bytes == 0) {
            bytes = 1;
        }

        if (alignment == 0) {
            alignment = 1;
        }

        assert((alignment & (alignment - 1U)) == 0U && "alignment must be a power of two");

        if (current_ == nullptr) {
            add_block(bytes, alignment);
        }

        Block* block = current_;
        for (;;) {
            if (void* result = try_allocate_from_block(*block, bytes, alignment)) {
                current_ = block;
                return result;
            }

            if (block->next == nullptr) {
                add_block(bytes, alignment);
                block = current_;
            } else {
                block = block->next;
            }
        }
    }

    // reset() keeps the already allocated blocks but marks all space as reusable.
    // This is a common arena design choice:
    // - very fast repeated reuse,
    // - no need to return to the general heap between requests/frames,
    // - stable pointers become invalid by convention once reset() is called.
    void reset() noexcept {
        for (Block* block = head_; block != nullptr; block = block->next) {
            block->used = 0;
        }
        current_ = head_;
    }

    std::size_t block_count() const noexcept { return block_count_; }
    std::size_t default_block_size() const noexcept { return block_size_; }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* storage = allocate(sizeof(T), alignof(T));
        return new (storage) T(std::forward<Args>(args)...);
    }

    template <typename T>
    void destroy(T* object) noexcept {
        if (object != nullptr) {
            object->~T();
        }
    }

private:
    struct Block {
        Block* next;
        std::size_t capacity;
        std::size_t used;
    };

    static std::uintptr_t align_up(std::uintptr_t value, std::size_t alignment) noexcept {
        const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment - 1U);
        return (value + mask) & ~mask;
    }

    static unsigned char* data_begin(Block* block) noexcept {
        return reinterpret_cast<unsigned char*>(block + 1);
    }

    static void* try_allocate_from_block(Block& block, std::size_t bytes, std::size_t alignment) noexcept {
        unsigned char* begin = data_begin(&block);
        std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(begin + block.used);
        std::uintptr_t aligned = align_up(raw, alignment);
        std::size_t padding = static_cast<std::size_t>(aligned - raw);

        if (block.used + padding + bytes > block.capacity) {
            return nullptr;
        }

        block.used += padding + bytes;
        return reinterpret_cast<void*>(aligned);
    }

    void add_block(std::size_t bytes, std::size_t alignment) {
        // Extra alignment slack matters. Even if the raw block begins at an inconvenient address,
        // there must still be enough room to move forward to the next aligned boundary.
        const std::size_t minimum_capacity = bytes + alignment;
        const std::size_t capacity = std::max(block_size_, minimum_capacity);
        const std::size_t total_bytes = sizeof(Block) + capacity;

        void* raw = ::operator new(total_bytes);
        Block* block = new (raw) Block{nullptr, capacity, 0};

        if (head_ == nullptr) {
            head_ = block;
            current_ = block;
        } else {
            Block* tail = head_;
            while (tail->next != nullptr) {
                tail = tail->next;
            }
            tail->next = block;
            current_ = block;
        }

        ++block_count_;
    }

    void release_all() noexcept {
        Block* block = head_;
        while (block != nullptr) {
            Block* next = block->next;
            ::operator delete(block);
            block = next;
        }

        head_ = nullptr;
        current_ = nullptr;
        block_count_ = 0;
    }

    std::size_t block_size_;
    Block* head_;
    Block* current_;
    std::size_t block_count_;
};

// std::allocator-compatible adapter.
// The adapter holds a pointer to an external Arena because standard containers copy allocators.
// All copies must still refer to the same backing arena so that allocations share one lifetime domain.
template <typename T>
class ArenaAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::false_type;

    template <typename U>
    struct rebind {
        using other = ArenaAllocator<U>;
    };

    ArenaAllocator() noexcept : arena_(nullptr) {}
    explicit ArenaAllocator(Arena& arena) noexcept : arena_(&arena) {}

    template <typename U>
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena_(other.arena()) {}

    T* allocate(std::size_t n) {
        if (arena_ == nullptr) {
            throw std::bad_alloc();
        }

        if (n > max_size()) {
            throw std::bad_array_new_length();
        }

        return static_cast<T*>(arena_->allocate(n * sizeof(T), alignof(T)));
    }

    void deallocate(T*, std::size_t) noexcept {
        // Intentionally a no-op.
        // Containers still call deallocate(), but the arena frees memory only on reset()/destruction.
    }

    std::size_t max_size() const noexcept {
        return static_cast<std::size_t>(-1) / sizeof(T);
    }

    Arena* arena() const noexcept { return arena_; }

    template <typename U>
    bool operator==(const ArenaAllocator<U>& other) const noexcept {
        return arena_ == other.arena();
    }

    template <typename U>
    bool operator!=(const ArenaAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    template <typename>
    friend class ArenaAllocator;

    Arena* arena_;
};

} // namespace microalloc
