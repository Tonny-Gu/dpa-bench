#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#define CUDA_CHECK(call)                                                        \
    do {                                                                        \
        cudaError_t err__ = (call);                                             \
        if (err__ != cudaSuccess) {                                             \
            fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__,   \
                    cudaGetErrorString(err__));                                 \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

#ifndef HAVE_CUDA_MEMCPY_BATCH
#if defined(CUDART_VERSION) && CUDART_VERSION >= 13000
#define HAVE_CUDA_MEMCPY_BATCH 1
#else
#define HAVE_CUDA_MEMCPY_BATCH 0
#endif
#endif

constexpr size_t KiB = 1024ULL;
constexpr size_t MiB = 1024ULL * KiB;
constexpr size_t GiB = 1024ULL * MiB;

constexpr size_t DEFAULT_POOL_SIZE = 4ULL * GiB;
constexpr size_t DEFAULT_COPY_SIZE = 1ULL * GiB;
constexpr int DEFAULT_WARMUP_RUNS = 3;
constexpr int DEFAULT_BENCHMARK_RUNS = 5;

struct Options {
    int srcDevice = 0;
    int dstDevice = 1;
    size_t poolSize = DEFAULT_POOL_SIZE;
    size_t copySize = DEFAULT_COPY_SIZE;
    int warmupRuns = DEFAULT_WARMUP_RUNS;
    int benchmarkRuns = DEFAULT_BENCHMARK_RUNS;
    uint64_t seed = 42;
};

struct BenchmarkResult {
    size_t elemSize = 0;
    size_t numCopies = 0;
    double bandwidthGBps = 0.0;
    double timeMs = 0.0;
    bool valid = false;
};

static void usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  --src <id>        Source GPU id (default: 0)\n");
    printf("  --dst <id>        Destination GPU id (default: 1)\n");
    printf("  --pool-mb <MB>    Pool size per GPU in MiB (default: 4096)\n");
    printf("  --copy-mb <MB>    Total copied bytes per run in MiB (default: 1024)\n");
    printf("  --warmup <N>      Warmup runs (default: 3)\n");
    printf("  --runs <N>        Benchmark runs (default: 5)\n");
    printf("  --seed <N>        RNG seed (default: 42)\n");
}

static uint64_t parseU64(const char *argName, const char *value) {
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 0);
    if (end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", argName, value);
        exit(EXIT_FAILURE);
    }
    return static_cast<uint64_t>(parsed);
}

static Options parseArgs(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", arg);
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
        const char *value = argv[++i];
        if (std::strcmp(arg, "--src") == 0) {
            opt.srcDevice = static_cast<int>(parseU64(arg, value));
        } else if (std::strcmp(arg, "--dst") == 0) {
            opt.dstDevice = static_cast<int>(parseU64(arg, value));
        } else if (std::strcmp(arg, "--pool-mb") == 0) {
            opt.poolSize = static_cast<size_t>(parseU64(arg, value)) * MiB;
        } else if (std::strcmp(arg, "--copy-mb") == 0) {
            opt.copySize = static_cast<size_t>(parseU64(arg, value)) * MiB;
        } else if (std::strcmp(arg, "--warmup") == 0) {
            opt.warmupRuns = static_cast<int>(parseU64(arg, value));
        } else if (std::strcmp(arg, "--runs") == 0) {
            opt.benchmarkRuns = static_cast<int>(parseU64(arg, value));
        } else if (std::strcmp(arg, "--seed") == 0) {
            opt.seed = parseU64(arg, value);
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (opt.srcDevice == opt.dstDevice) {
        fprintf(stderr, "Source and destination GPUs must be different.\n");
        exit(EXIT_FAILURE);
    }
    if (opt.poolSize == 0 || opt.copySize == 0 || opt.copySize > opt.poolSize) {
        fprintf(stderr, "Require 0 < copy size <= pool size.\n");
        exit(EXIT_FAILURE);
    }
    if (opt.warmupRuns < 0 || opt.benchmarkRuns <= 0) {
        fprintf(stderr, "Require warmup >= 0 and runs > 0.\n");
        exit(EXIT_FAILURE);
    }
    return opt;
}

static void formatSize(size_t bytes, double *value, const char **unit) {
    *value = static_cast<double>(bytes);
    *unit = "B";
    if (bytes >= GiB && bytes % GiB == 0) {
        *value = static_cast<double>(bytes) / GiB;
        *unit = "GB";
    } else if (bytes >= MiB && bytes % MiB == 0) {
        *value = static_cast<double>(bytes) / MiB;
        *unit = "MB";
    } else if (bytes >= KiB && bytes % KiB == 0) {
        *value = static_cast<double>(bytes) / KiB;
        *unit = "KB";
    }
}

static void generateRandomCopyPlan(size_t poolSize, size_t elemSize, size_t totalCopySize,
                                   std::vector<size_t> &srcOffsets,
                                   std::vector<size_t> &dstOffsets,
                                   std::mt19937_64 &rng) {
    const size_t numCopies = totalCopySize / elemSize;
    srcOffsets.resize(numCopies);
    dstOffsets.resize(numCopies);

    const size_t alignment = 64;
    const size_t maxOffset = poolSize - elemSize;
    std::uniform_int_distribution<size_t> dist(0, maxOffset / alignment);

    for (size_t i = 0; i < numCopies; ++i) {
        srcOffsets[i] = dist(rng) * alignment;
        dstOffsets[i] = dist(rng) * alignment;
    }

    std::vector<size_t> indices(numCopies);
    std::iota(indices.begin(), indices.end(), 0);
    std::shuffle(indices.begin(), indices.end(), rng);

    std::vector<size_t> shuffledSrc(numCopies), shuffledDst(numCopies);
    for (size_t i = 0; i < numCopies; ++i) {
        shuffledSrc[i] = srcOffsets[indices[i]];
        shuffledDst[i] = dstOffsets[indices[i]];
    }
    srcOffsets = std::move(shuffledSrc);
    dstOffsets = std::move(shuffledDst);
}

static void preparePointers(void *srcPool, void *dstPool, size_t elemSize, size_t copySize,
                            size_t poolSize, std::vector<void *> &srcs,
                            std::vector<void *> &dsts, std::vector<size_t> &srcOffsets,
                            std::vector<size_t> &dstOffsets, std::mt19937_64 &rng) {
    generateRandomCopyPlan(poolSize, elemSize, copySize, srcOffsets, dstOffsets, rng);
    const size_t numCopies = copySize / elemSize;
    srcs.resize(numCopies);
    dsts.resize(numCopies);
    for (size_t i = 0; i < numCopies; ++i) {
        srcs[i] = static_cast<char *>(srcPool) + srcOffsets[i];
        dsts[i] = static_cast<char *>(dstPool) + dstOffsets[i];
    }
}

static BenchmarkResult runP2PAsyncBenchmark(void *srcPool, void *dstPool, int srcDevice,
                                            int dstDevice, size_t poolSize, size_t copySize,
                                            size_t elemSize, cudaStream_t stream,
                                            int warmupRuns, int benchmarkRuns,
                                            std::mt19937_64 &rng) {
    CUDA_CHECK(cudaSetDevice(dstDevice));

    std::vector<size_t> srcOffsets, dstOffsets;
    std::vector<void *> srcs, dsts;
    preparePointers(srcPool, dstPool, elemSize, copySize, poolSize, srcs, dsts, srcOffsets,
                    dstOffsets, rng);

    const size_t numCopies = copySize / elemSize;
    for (int w = 0; w < warmupRuns; ++w) {
        for (size_t i = 0; i < numCopies; ++i) {
            CUDA_CHECK(cudaMemcpyPeerAsync(dsts[i], dstDevice, srcs[i], srcDevice, elemSize,
                                           stream));
        }
        CUDA_CHECK(cudaStreamSynchronize(stream));
    }

    cudaEvent_t start, stop;
    CUDA_CHECK(cudaEventCreate(&start));
    CUDA_CHECK(cudaEventCreate(&stop));

    double totalTimeMs = 0.0;
    for (int r = 0; r < benchmarkRuns; ++r) {
        preparePointers(srcPool, dstPool, elemSize, copySize, poolSize, srcs, dsts,
                        srcOffsets, dstOffsets, rng);

        CUDA_CHECK(cudaEventRecord(start, stream));
        for (size_t i = 0; i < numCopies; ++i) {
            CUDA_CHECK(cudaMemcpyPeerAsync(dsts[i], dstDevice, srcs[i], srcDevice, elemSize,
                                           stream));
        }
        CUDA_CHECK(cudaEventRecord(stop, stream));
        CUDA_CHECK(cudaStreamSynchronize(stream));

        float ms = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));
        totalTimeMs += ms;
    }

    CUDA_CHECK(cudaEventDestroy(start));
    CUDA_CHECK(cudaEventDestroy(stop));

    const double avgTimeMs = totalTimeMs / benchmarkRuns;
    const double bandwidthGBps = (copySize / 1e9) / (avgTimeMs / 1000.0);
    return {elemSize, numCopies, bandwidthGBps, avgTimeMs, true};
}

static BenchmarkResult runP2PBatchBenchmark(void *srcPool, void *dstPool, int srcDevice,
                                            int dstDevice, size_t poolSize, size_t copySize,
                                            size_t elemSize, cudaStream_t stream,
                                            int warmupRuns, int benchmarkRuns,
                                            std::mt19937_64 &rng) {
#if HAVE_CUDA_MEMCPY_BATCH
    CUDA_CHECK(cudaSetDevice(dstDevice));

    std::vector<size_t> srcOffsets, dstOffsets;
    std::vector<void *> rawSrcs, rawDsts;
    preparePointers(srcPool, dstPool, elemSize, copySize, poolSize, rawSrcs, rawDsts,
                    srcOffsets, dstOffsets, rng);

    const size_t numCopies = copySize / elemSize;
    std::vector<const void *> srcs(numCopies);
    std::vector<const void *> dsts(numCopies);
    std::vector<size_t> sizes(numCopies, elemSize);
    for (size_t i = 0; i < numCopies; ++i) {
        srcs[i] = rawSrcs[i];
        dsts[i] = rawDsts[i];
    }

    cudaMemcpyAttributes attr = {};
    attr.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
    attr.srcLocHint.type = cudaMemLocationTypeDevice;
    attr.srcLocHint.id = srcDevice;
    attr.dstLocHint.type = cudaMemLocationTypeDevice;
    attr.dstLocHint.id = dstDevice;
    size_t attrsIdx = 0;

    for (int w = 0; w < warmupRuns; ++w) {
        CUDA_CHECK(cudaMemcpyBatchAsync(dsts.data(), srcs.data(), sizes.data(), numCopies,
                                        &attr, &attrsIdx, 1, stream));
        CUDA_CHECK(cudaStreamSynchronize(stream));
    }

    cudaEvent_t start, stop;
    CUDA_CHECK(cudaEventCreate(&start));
    CUDA_CHECK(cudaEventCreate(&stop));

    double totalTimeMs = 0.0;
    for (int r = 0; r < benchmarkRuns; ++r) {
        preparePointers(srcPool, dstPool, elemSize, copySize, poolSize, rawSrcs, rawDsts,
                        srcOffsets, dstOffsets, rng);
        for (size_t i = 0; i < numCopies; ++i) {
            srcs[i] = rawSrcs[i];
            dsts[i] = rawDsts[i];
        }

        CUDA_CHECK(cudaEventRecord(start, stream));
        CUDA_CHECK(cudaMemcpyBatchAsync(dsts.data(), srcs.data(), sizes.data(), numCopies,
                                        &attr, &attrsIdx, 1, stream));
        CUDA_CHECK(cudaEventRecord(stop, stream));
        CUDA_CHECK(cudaStreamSynchronize(stream));

        float ms = 0.0f;
        CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));
        totalTimeMs += ms;
    }

    CUDA_CHECK(cudaEventDestroy(start));
    CUDA_CHECK(cudaEventDestroy(stop));

    const double avgTimeMs = totalTimeMs / benchmarkRuns;
    const double bandwidthGBps = (copySize / 1e9) / (avgTimeMs / 1000.0);
    return {elemSize, numCopies, bandwidthGBps, avgTimeMs, true};
#else
    (void)srcPool;
    (void)dstPool;
    (void)srcDevice;
    (void)dstDevice;
    (void)poolSize;
    (void)stream;
    (void)warmupRuns;
    (void)benchmarkRuns;
    (void)rng;
    return {elemSize, copySize / elemSize, 0.0, 0.0, false};
#endif
}

static void printComparisonResults(const char *testName,
                                   const std::vector<BenchmarkResult> &asyncResults,
                                   const std::vector<BenchmarkResult> &batchResults) {
    printf("\n========== %s ==========\n", testName);
    printf("%-12s %-12s %-14s %-14s %-14s %-14s %-10s\n", "Elem Size",
           "Num Copies", "Async (ms)", "Batch (ms)", "Async (GB/s)", "Batch (GB/s)",
           "Speedup");
    printf("--------------------------------------------------------------------------------------------\n");

    for (size_t i = 0; i < asyncResults.size(); ++i) {
        const auto &async = asyncResults[i];
        const auto &batch = batchResults[i];

        double size = 0.0;
        const char *unit = nullptr;
        formatSize(async.elemSize, &size, &unit);

        if (batch.valid) {
            const double speedup = async.timeMs / batch.timeMs;
            printf("%-9.0f%-3s %-12zu %-14.2f %-14.2f %-14.2f %-14.2f %-10.2fx\n",
                   size, unit, async.numCopies, async.timeMs, batch.timeMs,
                   async.bandwidthGBps, batch.bandwidthGBps, speedup);
        } else {
            printf("%-9.0f%-3s %-12zu %-14.2f %-14s %-14.2f %-14s %-10s\n", size,
                   unit, async.numCopies, async.timeMs, "N/A", async.bandwidthGBps, "N/A",
                   "N/A");
        }
    }
}

static void enablePeerAccess(int device, int peer) {
    int canAccess = 0;
    CUDA_CHECK(cudaDeviceCanAccessPeer(&canAccess, device, peer));
    if (!canAccess) {
        fprintf(stderr, "GPU %d cannot access peer GPU %d.\n", device, peer);
        exit(EXIT_FAILURE);
    }

    CUDA_CHECK(cudaSetDevice(device));
    cudaError_t err = cudaDeviceEnablePeerAccess(peer, 0);
    if (err == cudaErrorPeerAccessAlreadyEnabled) {
        (void)cudaGetLastError();
        return;
    }
    CUDA_CHECK(err);
}

static void *allocatePool(int device, size_t bytes) {
    CUDA_CHECK(cudaSetDevice(device));
    void *ptr = nullptr;
    CUDA_CHECK(cudaMalloc(&ptr, bytes));
    CUDA_CHECK(cudaMemset(ptr, 0, bytes));
    return ptr;
}

static cudaStream_t createStreamOnDevice(int device) {
    CUDA_CHECK(cudaSetDevice(device));
    cudaStream_t stream = nullptr;
    CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    return stream;
}

static void destroyStreamOnDevice(int device, cudaStream_t stream) {
    CUDA_CHECK(cudaSetDevice(device));
    CUDA_CHECK(cudaStreamDestroy(stream));
}

static void freePool(int device, void *ptr) {
    CUDA_CHECK(cudaSetDevice(device));
    CUDA_CHECK(cudaFree(ptr));
}

static std::vector<size_t> makeElemSizes(size_t copySize) {
    const std::vector<size_t> candidates = {1 * KiB,  4 * KiB,  16 * KiB,
                                            64 * KiB, 256 * KiB, 1 * MiB,
                                            4 * MiB,  16 * MiB, 32 * MiB};
    std::vector<size_t> elemSizes;
    for (size_t size : candidates) {
        if (size <= copySize) {
            elemSizes.push_back(size);
        }
    }
    return elemSizes;
}

static void runDirection(const char *label, void *srcPool, void *dstPool, int srcDevice,
                         int dstDevice, const Options &opt,
                         const std::vector<size_t> &elemSizes, std::mt19937_64 &rng) {
    cudaStream_t stream = createStreamOnDevice(dstDevice);

    printf("\nRunning %s cudaMemcpyPeerAsync benchmarks...\n", label);
    std::vector<BenchmarkResult> asyncResults;
    for (size_t elemSize : elemSizes) {
        printf("  Async element size: %zu bytes (%zu copies)...\n", elemSize,
               opt.copySize / elemSize);
        asyncResults.push_back(runP2PAsyncBenchmark(srcPool, dstPool, srcDevice, dstDevice,
                                                    opt.poolSize, opt.copySize, elemSize,
                                                    stream, opt.warmupRuns,
                                                    opt.benchmarkRuns, rng));
    }

    printf("\nRunning %s cudaMemcpyBatchAsync benchmarks...\n", label);
    if (!HAVE_CUDA_MEMCPY_BATCH) {
        printf("  Skipped: cudaMemcpyBatchAsync is not available in this CUDA runtime header.\n");
    }
    std::vector<BenchmarkResult> batchResults;
    for (size_t elemSize : elemSizes) {
        if (HAVE_CUDA_MEMCPY_BATCH) {
            printf("  Batch element size: %zu bytes (%zu copies)...\n", elemSize,
                   opt.copySize / elemSize);
        }
        batchResults.push_back(runP2PBatchBenchmark(srcPool, dstPool, srcDevice, dstDevice,
                                                    opt.poolSize, opt.copySize, elemSize,
                                                    stream, opt.warmupRuns,
                                                    opt.benchmarkRuns, rng));
    }

    printComparisonResults(label, asyncResults, batchResults);
    destroyStreamOnDevice(dstDevice, stream);
}

int main(int argc, char **argv) {
    const Options opt = parseArgs(argc, argv);

    int deviceCount = 0;
    CUDA_CHECK(cudaGetDeviceCount(&deviceCount));
    if (opt.srcDevice < 0 || opt.srcDevice >= deviceCount || opt.dstDevice < 0 ||
        opt.dstDevice >= deviceCount) {
        fprintf(stderr, "Invalid GPU ids: src=%d dst=%d, device count=%d\n", opt.srcDevice,
                opt.dstDevice, deviceCount);
        return EXIT_FAILURE;
    }

    cudaDeviceProp srcProp = {}, dstProp = {};
    CUDA_CHECK(cudaGetDeviceProperties(&srcProp, opt.srcDevice));
    CUDA_CHECK(cudaGetDeviceProperties(&dstProp, opt.dstDevice));

    double poolValue = 0.0, copyValue = 0.0;
    const char *poolUnit = nullptr, *copyUnit = nullptr;
    formatSize(opt.poolSize, &poolValue, &poolUnit);
    formatSize(opt.copySize, &copyValue, &copyUnit);

    printf("CUDA P2P memcpy benchmark\n");
    printf("Source GPU: %d (%s)\n", opt.srcDevice, srcProp.name);
    printf("Destination GPU: %d (%s)\n", opt.dstDevice, dstProp.name);
    printf("Pool size per GPU: %.0f %s, copy size per run: %.0f %s\n", poolValue,
           poolUnit, copyValue, copyUnit);
    printf("Element size range: 1KB - 32MB\n");
    printf("Warmup runs: %d, benchmark runs: %d\n", opt.warmupRuns, opt.benchmarkRuns);
    printf("CUDART_VERSION: %d\n", CUDART_VERSION);
    printf("cudaMemcpyBatchAsync: %s\n\n", HAVE_CUDA_MEMCPY_BATCH ? "enabled" : "not compiled");

    enablePeerAccess(opt.srcDevice, opt.dstDevice);
    enablePeerAccess(opt.dstDevice, opt.srcDevice);

    printf("Allocating %.0f %s on GPU %d...\n", poolValue, poolUnit, opt.srcDevice);
    void *srcPool = allocatePool(opt.srcDevice, opt.poolSize);
    printf("Allocating %.0f %s on GPU %d...\n", poolValue, poolUnit, opt.dstDevice);
    void *dstPool = allocatePool(opt.dstDevice, opt.poolSize);

    const std::vector<size_t> elemSizes = makeElemSizes(opt.copySize);
    std::mt19937_64 rng(opt.seed);

    char forwardLabel[32] = {};
    char reverseLabel[32] = {};
    std::snprintf(forwardLabel, sizeof(forwardLabel), "GPU%d-to-GPU%d", opt.srcDevice,
                  opt.dstDevice);
    std::snprintf(reverseLabel, sizeof(reverseLabel), "GPU%d-to-GPU%d", opt.dstDevice,
                  opt.srcDevice);

    runDirection(forwardLabel, srcPool, dstPool, opt.srcDevice, opt.dstDevice, opt,
                 elemSizes, rng);
    runDirection(reverseLabel, dstPool, srcPool, opt.dstDevice, opt.srcDevice, opt,
                 elemSizes, rng);

    printf("\nCleaning up...\n");
    freePool(opt.srcDevice, srcPool);
    freePool(opt.dstDevice, dstPool);
    printf("Done.\n");
    return EXIT_SUCCESS;
}
