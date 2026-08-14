#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace microalloc {

struct TrackingAllocatorState {
    // These counters intentionally track allocations for the original value_type that created the state.
    // Standard containers may rebind allocators for internal helper objects; those are real allocations,
    // but they would distort experiments whose goal is to count element-buffer reallocations.
    std::size_t allocation_count = 0;
    std::size_t deallocation_count = 0;
    std::size_t current_bytes = 0;
    std::size_t peak_bytes = 0;
    std::size_t total_allocated_bytes = 0;
    std::size_t total_deallocated_bytes = 0;
    const void* tracked_type_tag = nullptr;

    void reset() noexcept {
        allocation_count = 0;
        deallocation_count = 0;
        current_bytes = 0;
        peak_bytes = 0;
        total_allocated_bytes = 0;
        total_deallocated_bytes = 0;
    }
};

// A tracking allocator is useful because allocator instances are copied by containers.
// If each copy owned separate counters, the measurements would be misleading.
// Therefore all allocator copies share one heap-allocated state object.
template <typename T>
class TrackingAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::false_type;

    template <typename U>
    struct rebind {
        using other = TrackingAllocator<U>;
    };

    TrackingAllocator() : state_(std::make_shared<TrackingAllocatorState>()) {
        state_->tracked_type_tag = type_tag();
    }

    explicit TrackingAllocator(std::shared_ptr<TrackingAllocatorState> state) : state_(std::move(state)) {
        if (state_->tracked_type_tag == nullptr) {
            state_->tracked_type_tag = type_tag();
        }
    }

    template <typename U>
    TrackingAllocator(const TrackingAllocator<U>& other) noexcept : state_(other.state()) {}

    T* allocate(std::size_t n) {
        if (n > max_size()) {
            throw std::bad_array_new_length();
        }

        const std::size_t bytes = n * sizeof(T);
        if (is_tracked_type()) {
            state_->allocation_count += 1;
            state_->current_bytes += bytes;
            state_->total_allocated_bytes += bytes;
            if (state_->current_bytes > state_->peak_bytes) {
                state_->peak_bytes = state_->current_bytes;
            }
        }

        return static_cast<T*>(::operator new(bytes));
    }

    void deallocate(T* pointer, std::size_t n) noexcept {
        const std::size_t bytes = n * sizeof(T);
        if (is_tracked_type()) {
            state_->deallocation_count += 1;
            state_->current_bytes -= bytes;
            state_->total_deallocated_bytes += bytes;
        }
        ::operator delete(pointer);
    }

    std::size_t max_size() const noexcept {
        return static_cast<std::size_t>(-1) / sizeof(T);
    }

    std::shared_ptr<TrackingAllocatorState> state() const noexcept { return state_; }

    template <typename U>
    bool operator==(const TrackingAllocator<U>& other) const noexcept {
        return state_ == other.state();
    }

    template <typename U>
    bool operator!=(const TrackingAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    template <typename>
    friend class TrackingAllocator;

    static const void* type_tag() noexcept {
        static const int tag = 0;
        return &tag;
    }

    bool is_tracked_type() const noexcept {
        return state_->tracked_type_tag == type_tag();
    }

    std::shared_ptr<TrackingAllocatorState> state_;
};

} // namespace microalloc
