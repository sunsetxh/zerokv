#include <benchmark/benchmark.h>
#include "p2p/config.h"
#include "p2p/worker.h"
#include "p2p/endpoint.h"

#include <vector>
#include <cstring>

using namespace p2p;

static void BenchmarkCreateContext(benchmark::State& state) {
    auto config = Config::builder()
        .set_transport("tcp")
        .build();

    for (auto _ : state) {
        auto ctx = Context::create(config);
        benchmark::DoNotOptimize(ctx);
    }
}
BENCHMARK(BenchmarkCreateContext);

static void BenchmarkCreateWorker(benchmark::State& state) {
    auto config = Config::builder()
        .set_transport("tcp")
        .build();
    auto ctx = Context::create(config);

    for (auto _ : state) {
        auto worker = Worker::create(ctx);
        benchmark::DoNotOptimize(worker);
    }
}
BENCHMARK(BenchmarkCreateWorker);

static void BenchmarkWorkerProgress(benchmark::State& state) {
    auto config = Config::builder()
        .set_transport("tcp")
        .build();
    auto ctx = Context::create(config);
    auto worker = Worker::create(ctx);

    for (auto _ : state) {
        worker->progress();
    }
}
BENCHMARK(BenchmarkWorkerProgress);

static void BenchmarkMemoryAllocation(benchmark::State& state) {
    auto config = Config::builder()
        .set_transport("tcp")
        .build();
    auto ctx = Context::create(config);

    size_t size = state.range(0);

    for (auto _ : state) {
        auto region = MemoryRegion::allocate(ctx, size);
        benchmark::DoNotOptimize(region);
    }
}
BENCHMARK(BenchmarkMemoryAllocation)->Range(1024, 64 * 1024 * 1024);

static void BenchmarkWorkerAddress(benchmark::State& state) {
    auto config = Config::builder()
        .set_transport("tcp")
        .build();
    auto ctx = Context::create(config);
    auto worker = Worker::create(ctx);

    for (auto _ : state) {
        auto addr = worker->address();
        benchmark::DoNotOptimize(addr);
    }
}
BENCHMARK(BenchmarkWorkerAddress);

BENCHMARK_MAIN();
