// Critic Fabric — CUDA verification kernel for the optional accelerator proof.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This translation unit contains only CUDA work. It is compiled directly with
// nvcc and linked into a plain C++ test executable, so the governance logic of
// the proof stays in ordinary C++ and no CMake CUDA language flags are involved.
#include <cuda_runtime.h>

#include <cstdint>
#include <cstring>

namespace {

// The reduction is a sum of per-position contributions, so the total is
// independent of the order in which threads and blocks execute. That is what
// makes an exact CPU parity comparison possible: a per-thread recurrence over
// the payload would depend on thread scheduling and could not be reproduced on
// the host.
__global__ void verify_kernel(const std::uint8_t* input, std::uint64_t length,
                              unsigned long long* output) {
    const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    const std::uint64_t stride = static_cast<std::uint64_t>(gridDim.x) * blockDim.x;
    std::uint64_t partial = 0;
    for (std::uint64_t position = index; position < length; position += stride) {
        partial += static_cast<std::uint64_t>(input[position]) * 131u + position + 7u;
    }
    atomicAdd(output, static_cast<unsigned long long>(partial));
}

void copy_error(char* buffer, int capacity, cudaError_t error) {
    if (buffer == nullptr || capacity <= 0) {
        return;
    }
    const char* text = cudaGetErrorString(error);
    const std::size_t length = std::strlen(text);
    const std::size_t bounded = length < static_cast<std::size_t>(capacity - 1)
                                    ? length
                                    : static_cast<std::size_t>(capacity - 1);
    std::memcpy(buffer, text, bounded);
    buffer[bounded] = '\0';
}

}  // namespace

extern "C" int critic_fabric_cuda_device_count(int* count) {
    if (count == nullptr) {
        return static_cast<int>(cudaErrorInvalidValue);
    }
    const cudaError_t status = cudaGetDeviceCount(count);
    if (status != cudaSuccess) {
        return static_cast<int>(status);
    }
    return 0;
}

extern "C" int critic_fabric_cuda_device_info(char* name, int name_capacity, int* major, int* minor,
                                              unsigned long long* total_bytes) {
    cudaDeviceProp properties;
    std::memset(&properties, 0, sizeof(properties));
    const cudaError_t status = cudaGetDeviceProperties(&properties, 0);
    if (status != cudaSuccess) {
        return static_cast<int>(status);
    }
    if (name != nullptr && name_capacity > 0) {
        const std::size_t length = std::strlen(properties.name);
        const std::size_t bounded = length < static_cast<std::size_t>(name_capacity - 1)
                                        ? length
                                        : static_cast<std::size_t>(name_capacity - 1);
        std::memcpy(name, properties.name, bounded);
        name[bounded] = '\0';
    }
    if (major != nullptr) {
        *major = properties.major;
    }
    if (minor != nullptr) {
        *minor = properties.minor;
    }
    if (total_bytes != nullptr) {
        *total_bytes = static_cast<unsigned long long>(properties.totalGlobalMem);
    }
    return 0;
}

extern "C" int critic_fabric_cuda_memory(unsigned long long* free_bytes,
                                         unsigned long long* total_bytes) {
    std::size_t free_value = 0;
    std::size_t total_value = 0;
    const cudaError_t status = cudaMemGetInfo(&free_value, &total_value);
    if (status != cudaSuccess) {
        return static_cast<int>(status);
    }
    if (free_bytes != nullptr) {
        *free_bytes = static_cast<unsigned long long>(free_value);
    }
    if (total_bytes != nullptr) {
        *total_bytes = static_cast<unsigned long long>(total_value);
    }
    return 0;
}

// Performs the complete accelerator sequence: allocate, upload, execute,
// synchronise, download, and release. Returns zero only when every step
// succeeded and the device result was written.
extern "C" int critic_fabric_cuda_verify(const std::uint8_t* host_input, std::uint64_t length,
                                         unsigned long long* host_output, char* error,
                                         int error_capacity) {
    if (host_input == nullptr || host_output == nullptr || length == 0) {
        copy_error(error, error_capacity, cudaErrorInvalidValue);
        return static_cast<int>(cudaErrorInvalidValue);
    }

    std::uint8_t* device_input = nullptr;
    unsigned long long* device_output = nullptr;
    cudaError_t status = cudaMalloc(reinterpret_cast<void**>(&device_input), static_cast<std::size_t>(length));
    if (status != cudaSuccess) {
        copy_error(error, error_capacity, status);
        return static_cast<int>(status);
    }
    status = cudaMalloc(reinterpret_cast<void**>(&device_output), sizeof(unsigned long long));
    if (status != cudaSuccess) {
        cudaFree(device_input);
        copy_error(error, error_capacity, status);
        return static_cast<int>(status);
    }
    status = cudaMemset(device_output, 0, sizeof(unsigned long long));
    if (status == cudaSuccess) {
        status = cudaMemcpy(device_input, host_input, static_cast<std::size_t>(length),
                            cudaMemcpyHostToDevice);
    }
    if (status == cudaSuccess) {
        const unsigned int threads = 256;
        const unsigned int blocks = 128;
        verify_kernel<<<blocks, threads>>>(device_input, length, device_output);
        status = cudaGetLastError();
    }
    if (status == cudaSuccess) {
        status = cudaDeviceSynchronize();
    }
    if (status == cudaSuccess) {
        status = cudaMemcpy(host_output, device_output, sizeof(unsigned long long),
                            cudaMemcpyDeviceToHost);
    }

    const cudaError_t release_input = cudaFree(device_input);
    const cudaError_t release_output = cudaFree(device_output);
    const cudaError_t synchronise = cudaDeviceSynchronize();
    if (status == cudaSuccess && release_input != cudaSuccess) {
        status = release_input;
    }
    if (status == cudaSuccess && release_output != cudaSuccess) {
        status = release_output;
    }
    if (status == cudaSuccess && synchronise != cudaSuccess) {
        status = synchronise;
    }
    if (status != cudaSuccess) {
        copy_error(error, error_capacity, status);
        return static_cast<int>(status);
    }
    return 0;
}
