#include <benchmark/benchmark.h>
#include "zerokv/config.h"
#include "zerokv/worker.h"
#include "zerokv/memory.h"

#include <vector>
#include <cstring>

using namespace axon;

static void BenchmarkMemoryRegistration(benchmark::State& state) {
    auto config = Config::builder()
        .set_transport("tcp")
        .build();
    auto ctx = Context::create(config);

    size_t size = state.range(0);
    std::vector<char> buffer(size, 0);

    for (auto _ : state) {
        auto region = MemoryRegion::register_mem(ctx, buffer.data(), size);
        benchmark::DoNotOptimize(region);
    }
}
BENCHMARK(BenchmarkMemoryRegistration)->Range(4096, 64 * 1024 * 1024);

static void BenchmarkMultipleWorkers(benchmark::State& state) {
    auto config = Config::builder()
        .set_transport("tcp")
        .build();
    auto ctx = Context::create(config);

    size_t num_workers = state.range(0);

    for (auto _ : state) {
        std::vector<Worker::Ptr> workers;
        workers.reserve(num_workers);
        for (size_t i = 0; i < num_workers; ++i) {
            workers.push_back(Worker::create(ctx, i));
        }
        benchmark::DoNotOptimize(workers);
    }
}
BENCHMARK(BenchmarkMultipleWorkers)->Range(1, 64);

static void BenchmarkTagConstruction(benchmark::State& state) {
    for (auto _ : state) {
        Tag tag = make_tag(state.range(0), state.range(1));
        benchmark::DoNotOptimize(tag);
    }
}
BENCHMARK(BenchmarkTagConstruction)
    ->Args({1, 42})
    ->Args({0xFFFF, 0x1234});

BENCHMARK_MAIN();
