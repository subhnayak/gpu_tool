#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace microalloc {

// FixedPool serves one chunk size very efficiently.
// This is the opposite of the general heap's flexibility:
// we restrict the problem (same-sized chunks), so the allocator can be very cheap.
//
// The free list is "intrusive": when a chunk is free, we reuse the chunk's own storage to hold
// the next pointer. That sounds scary at first, but it is a standard low-level technique.
// Once the original object has been destroyed, its bytes are just raw storage again.
//
// About aliasing / union-style reasoning:
// - while a user object is alive, those bytes belong to that object type,
// - after the caller destroys the object, the lifetime of that object is over,
// - at that point we may treat the storage as untyped memory and write a FreeNode into it,
// - when allocate() returns the slot again, placement-new starts the lifetime of the next object.
//
// In other words, we are not pretending one live object is two types at once.
// The storage changes role over time: live object -> raw bytes / free-list node -> live object.
template <std::size_t ChunkSize,
          std::size_t ChunkAlignment = alignof(std::max_align_t),
          bool DebugPoison = false>
class FixedPool {
public:
    static_assert(ChunkSize > 0, "ChunkSize must be non-zero");
    static_assert((ChunkAlignment & (ChunkAlignment - 1U)) == 0U, "ChunkAlignment must be a power of two");

private:
    struct FreeNode {
        FreeNode* next;
    };

public:
    static constexpr std::size_t effective_chunk_size = (ChunkSize > sizeof(FreeNode)) ? ChunkSize : sizeof(FreeNode);
    static constexpr std::size_t stride = (effective_chunk_size + (ChunkAlignment - 1U)) & ~(ChunkAlignment - 1U);

    static_assert(sizeof(FreeNode) <= effective_chunk_size, "Free list node must fit into a chunk");

    explicit FixedPool(std::size_t chunks_per_page = 256)
        : chunks_per_page_(std::max<std::size_t>(chunks_per_page, 1U)), pages_(nullptr), free_list_(nullptr), page_count_(0) {}

    FixedPool(const FixedPool&) = delete;
    FixedPool& operator=(const FixedPool&) = delete;

    ~FixedPool() { release_all(); }

    void* allocate() {
        if (free_list_ == nullptr) {
            add_page();
        }

        FreeNode* node = free_list_;
        free_list_ = free_list_->next;
        return node;
    }

    void deallocate(void* pointer) noexcept {
        if (pointer == nullptr) {
            return;
        }

        if constexpr (DebugPoison) {
            // Poisoning helps catch use-after-free in non-sanitized debug sessions.
            // We write the pattern first, then overwrite the first bytes with the next pointer.
            std::memset(pointer, 0xDD, stride);
        }

        auto* node = static_cast<FreeNode*>(pointer);
        node->next = free_list_;
        free_list_ = node;
    }

    void reset() noexcept {
        free_list_ = nullptr;
        for (Page* page = pages_; page != nullptr; page = page->next) {
            link_page_chunks(*page);
        }
    }

    std::size_t page_count() const noexcept { return page_count_; }
    std::size_t chunk_size() const noexcept { return effective_chunk_size; }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        static_assert(sizeof(T) <= ChunkSize, "Type does not fit in this pool chunk size");
        static_assert(alignof(T) <= ChunkAlignment, "Type alignment exceeds pool alignment");
        void* storage = allocate();
        return new (storage) T(std::forward<Args>(args)...);
    }

    template <typename T>
    void destroy(T* object) noexcept {
        if (object != nullptr) {
            object->~T();
            deallocate(object);
        }
    }

private:
    struct Page {
        Page* next;
        unsigned char* memory;
    };

    void add_page() {
        auto* page = new Page{pages_, nullptr};
        page->memory = static_cast<unsigned char*>(::operator new(stride * chunks_per_page_, std::align_val_t(ChunkAlignment)));
        pages_ = page;
        ++page_count_;
        link_page_chunks(*page);
    }

    void link_page_chunks(Page& page) noexcept {
        for (std::size_t i = 0; i < chunks_per_page_; ++i) {
            unsigned char* chunk = page.memory + (i * stride);
            auto* node = reinterpret_cast<FreeNode*>(chunk);
            node->next = free_list_;
            free_list_ = node;
        }
    }

    void release_all() noexcept {
        Page* page = pages_;
        while (page != nullptr) {
            Page* next = page->next;
            ::operator delete(page->memory, std::align_val_t(ChunkAlignment));
            delete page;
            page = next;
        }

        pages_ = nullptr;
        free_list_ = nullptr;
        page_count_ = 0;
    }

    std::size_t chunks_per_page_;
    Page* pages_;
    FreeNode* free_list_;
    std::size_t page_count_;
};

} // namespace microalloc

