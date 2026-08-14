#include "arena.h"
#include "intrusive_list.h"
#include "pool.h"
#include "tracking_allocator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <list>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
volatile std::uint64_t g_sink = 0;

struct SmallObject {
    std::uint64_t values[4];
};

struct IntrusiveNode {
    microalloc::IntrusiveListNode link;
    std::uint64_t value;
};

using IntrusiveNodeList = microalloc::IntrusiveList<IntrusiveNode, &IntrusiveNode::link>;

struct ParticleAoS {
    float x;
    float y;
    float z;
    float vx;
    float vy;
    float vz;
    float temperature;
    float mass;
    float radius;
    float charge;
    float color_r;
    float color_g;
    float color_b;
    float color_a;
};

struct ParticleSoA {
    std::vector<float> x;
    std::vector<float> y;
    std::vector<float> z;
    std::vector<float> vx;
    std::vector<float> vy;
    std::vector<float> vz;
    std::vector<float> temperature;
    std::vector<float> mass;
    std::vector<float> radius;
    std::vector<float> charge;
    std::vector<float> color_r;
    std::vector<float> color_g;
    std::vector<float> color_b;
    std::vector<float> color_a;
};

template <typename Func>
double median_ms(Func&& func, int iterations = 9, int warmup = 1) {
    for (int i = 0; i < warmup; ++i) {
        func();
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        func();
        const auto stop = Clock::now();
        const std::chrono::duration<double, std::milli> elapsed = stop - start;
        samples.push_back(elapsed.count());
    }

    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2U];
}

void print_header(const std::string& title) {
    std::cout << "\n=== " << title << " ===\n";
}

void print_two_way_table(const std::string& baseline_name,
                         double baseline_ms,
                         const std::string& candidate_name,
                         double candidate_ms) {
    const double speedup = baseline_ms / candidate_ms;

    std::cout << std::left << std::setw(28) << "Variant"
              << std::right << std::setw(14) << "Median ms"
              << std::setw(14) << "Speedup" << '\n';
    std::cout << std::string(56, '-') << '\n';
    std::cout << std::left << std::setw(28) << baseline_name
              << std::right << std::setw(14) << std::fixed << std::setprecision(3) << baseline_ms
              << std::setw(14) << "1.00x" << '\n';
    std::cout << std::left << std::setw(28) << candidate_name
              << std::right << std::setw(14) << std::fixed << std::setprecision(3) << candidate_ms
              << std::setw(12) << std::fixed << std::setprecision(2) << speedup << "x\n";
}

void benchmark_small_allocations() {
    print_header("Many small allocations: new/delete vs pool vs arena");

    constexpr std::size_t count = 200000;

    const double new_delete_ms = median_ms([&]() {
        std::vector<SmallObject*> pointers;
        pointers.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            SmallObject* object = new SmallObject{};
            object->values[0] = static_cast<std::uint64_t>(i);
            pointers.push_back(object);
        }

        for (SmallObject* object : pointers) {
            g_sink += object->values[0];
            delete object;
        }
    });

    const double pool_ms = median_ms([&]() {
        microalloc::FixedPool<sizeof(SmallObject), alignof(SmallObject)> pool(1024);
        std::vector<SmallObject*> pointers;
        pointers.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            void* storage = pool.allocate();
            auto* object = new (storage) SmallObject{};
            object->values[0] = static_cast<std::uint64_t>(i);
            pointers.push_back(object);
        }

        for (SmallObject* object : pointers) {
            g_sink += object->values[0];
            object->~SmallObject();
            pool.deallocate(object);
        }
    });

    const double arena_ms = median_ms([&]() {
        microalloc::Arena arena(64 * 1024);
        std::vector<SmallObject*> pointers;
        pointers.reserve(count);

        for (std::size_t i = 0; i < count; ++i) {
            auto* object = static_cast<SmallObject*>(arena.allocate(sizeof(SmallObject), alignof(SmallObject)));
            object->values[0] = static_cast<std::uint64_t>(i);
            pointers.push_back(object);
        }

        for (SmallObject* object : pointers) {
            g_sink += object->values[0];
        }

        arena.reset();
    });

    std::cout << std::left << std::setw(28) << "Variant"
              << std::right << std::setw(14) << "Median ms"
              << std::setw(14) << "Vs new" << '\n';
    std::cout << std::string(56, '-') << '\n';
    std::cout << std::left << std::setw(28) << "new/delete"
              << std::right << std::setw(14) << std::fixed << std::setprecision(3) << new_delete_ms
              << std::setw(14) << "1.00x" << '\n';
    std::cout << std::left << std::setw(28) << "pool"
              << std::right << std::setw(14) << pool_ms
              << std::setw(12) << std::fixed << std::setprecision(2) << (new_delete_ms / pool_ms) << "x\n";
    std::cout << std::left << std::setw(28) << "arena"
              << std::right << std::setw(14) << arena_ms
              << std::setw(12) << std::fixed << std::setprecision(2) << (new_delete_ms / arena_ms) << "x\n";
}

void benchmark_linked_structures() {
    print_header("Large linked structure: std::list vs intrusive_list in arena");

    constexpr std::size_t count = 200000;

    const double std_list_ms = median_ms([&]() {
        std::list<std::uint64_t> values;
        for (std::size_t i = 0; i < count; ++i) {
            values.push_back(static_cast<std::uint64_t>(i));
        }

        std::uint64_t sum = 0;
        for (std::uint64_t value : values) {
            sum += value;
        }
        g_sink += sum;
    });

    const double intrusive_ms = median_ms([&]() {
        microalloc::Arena arena(128 * 1024);
        IntrusiveNodeList values;

        for (std::size_t i = 0; i < count; ++i) {
            IntrusiveNode* node = arena.create<IntrusiveNode>();
            node->value = static_cast<std::uint64_t>(i);
            values.push_back(*node);
        }

        std::uint64_t sum = 0;
        for (const IntrusiveNode& node : values) {
            sum += node.value;
        }
        g_sink += sum;
        arena.reset();
    });

    print_two_way_table("std::list", std_list_ms, "intrusive_list + arena", intrusive_ms);
}

void benchmark_aos_vs_soa() {
    print_header("Cache behaviour: AoS vs SoA traversal");

    constexpr std::size_t count = 1 << 20;
    std::vector<ParticleAoS> aos(count);
    ParticleSoA soa;
    soa.x.resize(count);
    soa.y.resize(count);
    soa.z.resize(count);
    soa.vx.resize(count);
    soa.vy.resize(count);
    soa.vz.resize(count);
    soa.temperature.resize(count);
    soa.mass.resize(count);
    soa.radius.resize(count);
    soa.charge.resize(count);
    soa.color_r.resize(count);
    soa.color_g.resize(count);
    soa.color_b.resize(count);
    soa.color_a.resize(count);

    std::mt19937 rng(42U);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (std::size_t i = 0; i < count; ++i) {
        aos[i] = ParticleAoS{dist(rng), dist(rng), dist(rng), dist(rng), dist(rng), dist(rng),
                             dist(rng), dist(rng), dist(rng), dist(rng), dist(rng), dist(rng),
                             dist(rng), dist(rng)};
        soa.x[i] = aos[i].x;
        soa.y[i] = aos[i].y;
        soa.z[i] = aos[i].z;
        soa.vx[i] = aos[i].vx;
        soa.vy[i] = aos[i].vy;
        soa.vz[i] = aos[i].vz;
        soa.temperature[i] = aos[i].temperature;
        soa.mass[i] = aos[i].mass;
        soa.radius[i] = aos[i].radius;
        soa.charge[i] = aos[i].charge;
        soa.color_r[i] = aos[i].color_r;
        soa.color_g[i] = aos[i].color_g;
        soa.color_b[i] = aos[i].color_b;
        soa.color_a[i] = aos[i].color_a;
    }

    const double aos_ms = median_ms([&]() {
        double sum = 0.0;
        for (std::size_t repeat = 0; repeat < 8; ++repeat) {
            for (const ParticleAoS& p : aos) {
                sum += static_cast<double>(p.x);
            }
        }
        g_sink += static_cast<std::uint64_t>(sum);
    });

    const double soa_ms = median_ms([&]() {
        double sum = 0.0;
        for (std::size_t repeat = 0; repeat < 8; ++repeat) {
            for (std::size_t i = 0; i < count; ++i) {
                sum += static_cast<double>(soa.x[i]);
            }
        }
        g_sink += static_cast<std::uint64_t>(sum);
    });

    print_two_way_table("AoS", aos_ms, "SoA", soa_ms);
}

struct VectorGrowthMeasurement {
    double median_ms;
    std::size_t allocations;
    std::size_t peak_bytes;
};

VectorGrowthMeasurement measure_vector_growth(bool use_reserve) {
    constexpr std::size_t count = 200000;

    microalloc::TrackingAllocator<int> sample_allocator;
    auto sample_state = sample_allocator.state();
    {
        std::vector<int, microalloc::TrackingAllocator<int>> sample(sample_allocator);
        if (use_reserve) {
            sample.reserve(count);
        }
        for (std::size_t i = 0; i < count; ++i) {
            sample.push_back(static_cast<int>(i));
        }
        g_sink += static_cast<std::uint64_t>(sample.back());
    }

    const double time_ms = median_ms([&]() {
        microalloc::TrackingAllocator<int> allocator;
        std::vector<int, microalloc::TrackingAllocator<int>> values(allocator);
        if (use_reserve) {
            values.reserve(count);
        }
        for (std::size_t i = 0; i < count; ++i) {
            values.push_back(static_cast<int>(i));
        }
        g_sink += static_cast<std::uint64_t>(values.back());
    });

    return VectorGrowthMeasurement{time_ms, sample_state->allocation_count, sample_state->peak_bytes};
}

void benchmark_vector_growth() {
    print_header("std::vector growth: without reserve vs with reserve");

    const VectorGrowthMeasurement no_reserve = measure_vector_growth(false);
    const VectorGrowthMeasurement with_reserve = measure_vector_growth(true);

    std::cout << std::left << std::setw(24) << "Variant"
              << std::right << std::setw(14) << "Median ms"
              << std::setw(16) << "Alloc count"
              << std::setw(16) << "Peak bytes"
              << std::setw(14) << "Speedup" << '\n';
    std::cout << std::string(84, '-') << '\n';
    std::cout << std::left << std::setw(24) << "vector push_back"
              << std::right << std::setw(14) << std::fixed << std::setprecision(3) << no_reserve.median_ms
              << std::setw(16) << no_reserve.allocations
              << std::setw(16) << no_reserve.peak_bytes
              << std::setw(14) << "1.00x" << '\n';
    std::cout << std::left << std::setw(24) << "vector reserve + push"
              << std::right << std::setw(14) << with_reserve.median_ms
              << std::setw(16) << with_reserve.allocations
              << std::setw(16) << with_reserve.peak_bytes
              << std::setw(12) << std::fixed << std::setprecision(2) << (no_reserve.median_ms / with_reserve.median_ms) << "x\n";
}

} // namespace

int main() {
    std::cout << "microalloc benchmark harness\n";
    std::cout << "All results are medians after a warmup. Faster variants show speedup ratios above 1.0x.\n";

    benchmark_small_allocations();
    benchmark_linked_structures();
    benchmark_aos_vs_soa();
    benchmark_vector_growth();

    std::cout << "\nIgnore the sink value; it only prevents the optimizer from deleting the work: " << g_sink << "\n";
    return 0;
}

