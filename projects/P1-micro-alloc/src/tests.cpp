#include "arena.h"
#include "intrusive_list.h"
#include "pool.h"
#include "tracking_allocator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_passes = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                   \
    do {                                                                                              \
        if (!(expr)) {                                                                                \
            std::cerr << "CHECK failed: " #expr << " at " << __FILE__ << ':' << __LINE__ << '\n'; \
            return false;                                                                             \
        }                                                                                             \
    } while (false)

#define TEST(name)                                                                                 \
    bool name();                                                                                   \
    struct name##_registrar {                                                                      \
        name##_registrar() { test_registry().push_back(TestCase{#name, &name}); }                 \
    } name##_registrar_instance;                                                                   \
    bool name()

struct TestCase {
    const char* name;
    bool (*fn)();
};

std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> tests;
    return tests;
}

TEST(test_arena_alignment_correctness) {
    microalloc::Arena arena(512);
    constexpr std::array<std::size_t, 8> alignments{{1, 2, 4, 8, 16, 32, 64, 128}};

    for (std::size_t alignment : alignments) {
        for (std::size_t size = 1; size <= 97; ++size) {
            void* pointer = arena.allocate(size, alignment);
            CHECK((reinterpret_cast<std::uintptr_t>(pointer) % alignment) == 0U);
        }
    }

    return true;
}

TEST(test_arena_block_chaining) {
    microalloc::Arena arena(64);
    for (int i = 0; i < 32; ++i) {
        static_cast<void>(arena.allocate(32, 8));
    }
    CHECK(arena.block_count() > 1U);
    return true;
}

TEST(test_arena_reset_reuse) {
    microalloc::Arena arena(128);
    void* first = arena.allocate(24, 16);
    static_cast<void>(arena.allocate(48, 8));
    arena.reset();
    void* after_reset = arena.allocate(24, 16);
    CHECK(first == after_reset);
    return true;
}

TEST(test_pool_reuses_freed_chunks_in_lifo_order) {
    microalloc::FixedPool<32, alignof(std::max_align_t), true> pool(4);
    void* a = pool.allocate();
    void* b = pool.allocate();
    void* c = pool.allocate();
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(c != nullptr);

    pool.deallocate(b);
    pool.deallocate(a);

    void* d = pool.allocate();
    void* e = pool.allocate();
    CHECK(d == a);
    CHECK(e == b);
    return true;
}

struct ListItem {
    microalloc::IntrusiveListNode link;
    int value;
};

using ListType = microalloc::IntrusiveList<ListItem, &ListItem::link>;

TEST(test_intrusive_list_insert_erase_iterate) {
    ListItem a{{}, 1};
    ListItem b{{}, 2};
    ListItem c{{}, 3};

    ListType list;
    list.push_back(a);
    list.push_back(c);
    auto it = list.insert(list.end(), b);
    CHECK(list.size() == 3U);

    std::vector<int> values;
    for (const ListItem& item : list) {
        values.push_back(item.value);
    }
    CHECK((values == std::vector<int>{1, 3, 2}));

    list.erase(it);
    values.clear();
    for (const ListItem& item : list) {
        values.push_back(item.value);
    }
    CHECK((values == std::vector<int>{1, 3}));
    CHECK(list.front().value == 1);
    CHECK(list.back().value == 3);
    return true;
}

TEST(test_tracking_allocator_counts) {
    microalloc::TrackingAllocator<int> alloc;
    auto state = alloc.state();

    {
        std::vector<int, microalloc::TrackingAllocator<int>> values(alloc);
        for (int i = 0; i < 128; ++i) {
            values.push_back(i);
        }
        CHECK(state->allocation_count >= 1U);
        CHECK(state->peak_bytes >= values.capacity() * sizeof(int));
    }

    CHECK(state->deallocation_count >= 1U);
    CHECK(state->current_bytes == 0U);
    CHECK(state->total_allocated_bytes >= state->total_deallocated_bytes);
    return true;
}

TEST(test_vector_reserve_does_exactly_one_allocation) {
    microalloc::TrackingAllocator<int> alloc;
    auto state = alloc.state();

    {
        std::vector<int, microalloc::TrackingAllocator<int>> values(alloc);
        values.reserve(256);
        for (int i = 0; i < 256; ++i) {
            values.push_back(i);
        }
        CHECK(state->allocation_count == 1U);
        CHECK(state->deallocation_count == 0U);
    }

    CHECK(state->allocation_count == 1U);
    CHECK(state->deallocation_count == 1U);
    CHECK(state->current_bytes == 0U);
    return true;
}

TEST(test_arena_allocator_works_with_vector) {
    microalloc::Arena arena(1024);
    microalloc::ArenaAllocator<int> alloc(arena);
    std::vector<int, microalloc::ArenaAllocator<int>> values(alloc);
    values.reserve(64);
    for (int i = 0; i < 64; ++i) {
        values.push_back(i * 2);
    }
    CHECK(values.size() == 64U);
    CHECK(values[10] == 20);
    return true;
}

} // namespace

int main() {
    std::cout << "Running microalloc tests...\n";

    for (const TestCase& test : test_registry()) {
        try {
            const bool ok = test.fn();
            if (ok) {
                ++g_passes;
                std::cout << "[PASS] " << test.name << '\n';
            } else {
                ++g_failures;
                std::cout << "[FAIL] " << test.name << '\n';
            }
        } catch (const std::exception& ex) {
            ++g_failures;
            std::cout << "[EXCEPTION] " << test.name << ": " << ex.what() << '\n';
        } catch (...) {
            ++g_failures;
            std::cout << "[EXCEPTION] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << "\nSummary: " << g_passes << " passed, " << g_failures << " failed.\n";
    return (g_failures == 0) ? 0 : 1;
}
