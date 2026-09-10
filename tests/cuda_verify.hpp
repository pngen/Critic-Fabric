// Critic Fabric — accelerator verification entry points.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_TEST_CUDA_VERIFY_HPP
#define CRITIC_FABRIC_TEST_CUDA_VERIFY_HPP

#include <cstdint>

extern "C" {

// All functions return zero on success or a CUDA runtime error code otherwise.
int critic_fabric_cuda_device_count(int* count);
int critic_fabric_cuda_device_info(char* name, int name_capacity, int* major, int* minor,
                                   unsigned long long* total_bytes);
int critic_fabric_cuda_memory(unsigned long long* free_bytes, unsigned long long* total_bytes);
int critic_fabric_cuda_verify(const std::uint8_t* host_input, std::uint64_t length,
                              unsigned long long* host_output, char* error, int error_capacity);

}  // extern "C"

#endif  // CRITIC_FABRIC_TEST_CUDA_VERIFY_HPP
