// SPDX-License-Identifier: GPL-2.0
// Optional hardware test: retain one CUDA allocation while a sibling is removed.
// Build: nvcc -O2 hotplug-cuda-smoke.cu -o hotplug-cuda-smoke
#include <cuda_runtime.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define CUDA(call) do { cudaError_t e = (call); if (e != cudaSuccess) { \
    std::fprintf(stderr, "%s: %s\n", #call, cudaGetErrorString(e)); return 1; \
} } while (0)

__global__ void fill(unsigned *data, size_t count, unsigned seed)
{
    size_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) data[i] = (unsigned)i ^ seed;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s DOMAIN:BUS:DEV.FUNCTION SECONDS(1..300)\n", argv[0]);
        return 2;
    }
    char *end;
    long seconds = std::strtol(argv[2], &end, 10);
    if (*end || seconds < 1 || seconds > 300) return 2;
    int device;
    CUDA(cudaDeviceGetByPCIBusId(&device, argv[1]));
    CUDA(cudaSetDevice(device));
    constexpr size_t count = 8 * 1024 * 1024;
    unsigned *buffer;
    CUDA(cudaMalloc(&buffer, count * sizeof(unsigned)));
    std::vector<unsigned> host(count);
    auto start = std::chrono::steady_clock::now();
    unsigned iterations = 0;
    do {
        unsigned seed = 0x5a730000u ^ iterations;
        fill<<<(count + 255) / 256, 256>>>(buffer, count, seed);
        CUDA(cudaGetLastError());
        CUDA(cudaMemcpy(host.data(), buffer, count * sizeof(unsigned), cudaMemcpyDeviceToHost));
        for (size_t i = 0; i < count; i++)
            if (host[i] != ((unsigned)i ^ seed)) {
                std::fprintf(stderr, "Mismatch at %zu iteration %u\n", i, iterations);
                return 1;
            }
        if (!(iterations % 50)) {
            std::printf("PASS %s iteration %u (32 MiB checked, context retained)\n", argv[1], iterations);
            std::fflush(stdout);
        }
        iterations++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds));
    CUDA(cudaFree(buffer));
    std::printf("PASS %s %u iterations over %ld seconds\n", argv[1], iterations, seconds);
    return 0;
}
