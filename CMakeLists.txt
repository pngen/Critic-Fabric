cmake_minimum_required(VERSION 3.24)

project(CriticFabric
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral C++20 reference runtime for authorizing and auditing critique, verification and evidence-backed assessment"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CRITIC_FABRIC_BUILD_TESTS "Build the Critic Fabric test suite" ON)
option(CRITIC_FABRIC_BUILD_EXAMPLES "Build the Critic Fabric examples" ON)
option(CRITIC_FABRIC_BUILD_BENCHMARKS "Build the Critic Fabric benchmark" ON)
option(CRITIC_FABRIC_BUILD_CUDA_PROOF "Build the optional CUDA-backed critic proof" OFF)
option(CRITIC_FABRIC_ENABLE_ASAN "Build with AddressSanitizer" OFF)

if(MSVC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Applies the project's strict warning contract to one first-party target.
function(critic_fabric_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

add_library(critic_fabric
    src/coordinator.cpp
    src/digest.cpp
    src/enums.cpp
    src/evaluation.cpp
    src/explain.cpp
    src/lifecycle.cpp
    src/limits.cpp
    src/model.cpp
    src/persistence.cpp
    src/platform.cpp
    src/policy.cpp
    src/protocol.cpp
    src/result.cpp
    src/serialization.cpp
    src/transport.cpp
    src/worker.cpp)
add_library(CriticFabric::critic_fabric ALIAS critic_fabric)

target_include_directories(critic_fabric
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(critic_fabric PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(critic_fabric PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(critic_fabric PUBLIC ws2_32)
endif()

critic_fabric_strict_warnings(critic_fabric)

if(CRITIC_FABRIC_ENABLE_ASAN)
    if(MSVC)
        target_compile_options(critic_fabric PUBLIC /fsanitize=address)
        target_link_options(critic_fabric PUBLIC /INCREMENTAL:NO)
    else()
        target_compile_options(critic_fabric PUBLIC -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(critic_fabric PUBLIC -fsanitize=address)
    endif()
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------

function(critic_fabric_executable target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE CriticFabric::critic_fabric)
    critic_fabric_strict_warnings(${target})
endfunction()

critic_fabric_executable(critic_coordinator apps/critic_coordinator.cpp)
critic_fabric_executable(critic_worker apps/critic_worker.cpp)
critic_fabric_executable(critic_controller apps/critic_controller.cpp)
critic_fabric_executable(critic_inspect apps/critic_inspect.cpp)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(CRITIC_FABRIC_BUILD_EXAMPLES)
    critic_fabric_executable(example_single_critic examples/single_critic.cpp)
    critic_fabric_executable(example_multi_critic examples/multi_critic.cpp)
    critic_fabric_executable(example_disagreement examples/disagreement.cpp)
    critic_fabric_executable(example_quorum_and_abstention examples/quorum_and_abstention.cpp)
    critic_fabric_executable(example_target_supersession examples/target_supersession.cpp)
    critic_fabric_executable(example_critic_replacement examples/critic_replacement.cpp)
    critic_fabric_executable(example_challenge_rebuttal examples/challenge_rebuttal.cpp)
    critic_fabric_executable(example_cancellation examples/cancellation.cpp)
    critic_fabric_executable(example_recovery examples/recovery.cpp)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(CRITIC_FABRIC_BUILD_BENCHMARKS)
    critic_fabric_executable(critic_bench bench/critic_bench.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(CRITIC_FABRIC_BUILD_TESTS)
    enable_testing()

    add_executable(critic_fabric_tests
        tests/test_core.cpp
        tests/test_coordinator.cpp
        tests/test_hardening.cpp
        tests/test_concurrency.cpp
        tests/test_support.cpp)
    target_link_libraries(critic_fabric_tests PRIVATE CriticFabric::critic_fabric)
    target_include_directories(critic_fabric_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    critic_fabric_strict_warnings(critic_fabric_tests)
    add_test(NAME critic_fabric_tests COMMAND critic_fabric_tests)

    add_executable(critic_fabric_tests_multiprocess
        tests/test_multiprocess.cpp
        tests/child_process.cpp
        tests/test_support.cpp)
    target_link_libraries(critic_fabric_tests_multiprocess PRIVATE CriticFabric::critic_fabric)
    target_include_directories(critic_fabric_tests_multiprocess PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    critic_fabric_strict_warnings(critic_fabric_tests_multiprocess)
    add_test(NAME critic_fabric_tests_multiprocess COMMAND critic_fabric_tests_multiprocess)
    set_tests_properties(critic_fabric_tests_multiprocess PROPERTIES
        ENVIRONMENT "CRITIC_FABRIC_BIN_DIR=$<TARGET_FILE_DIR:critic_coordinator>")
endif()

# ---------------------------------------------------------------------------
# Optional CUDA-backed critic proof
# ---------------------------------------------------------------------------

if(CRITIC_FABRIC_BUILD_CUDA_PROOF)
    # The accelerator kernel is compiled directly with nvcc and linked into a
    # plain C++ executable. Using CMake's CUDA language support here would fold
    # the host compiler's "/Fd" and "/FS" options into a single comma separated
    # -Xcompiler argument whose trailing empty element nvcc mistakes for a
    # second input file, so the kernel is built with an explicit command line and
    # the governance half of the proof stays ordinary C++.
    find_package(CUDAToolkit QUIET)
    find_program(CRITIC_FABRIC_NVCC nvcc HINTS "${CUDAToolkit_BIN_DIR}")
    if(CUDAToolkit_FOUND AND CRITIC_FABRIC_NVCC)
        # The kernel is built for the compute capability of the installed device.
        set(CRITIC_FABRIC_CUDA_ARCH "sm_75")
        find_program(CRITIC_FABRIC_NVIDIA_SMI nvidia-smi)
        if(CRITIC_FABRIC_NVIDIA_SMI)
            execute_process(
                COMMAND ${CRITIC_FABRIC_NVIDIA_SMI} --query-gpu=compute_cap --format=csv,noheader
                OUTPUT_VARIABLE CRITIC_FABRIC_CUDA_CAPABILITY
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE CRITIC_FABRIC_CUDA_CAPABILITY_RESULT)
            if(CRITIC_FABRIC_CUDA_CAPABILITY_RESULT EQUAL 0)
                string(REPLACE "." "" CRITIC_FABRIC_CUDA_ARCH_DIGITS "${CRITIC_FABRIC_CUDA_CAPABILITY}")
                string(STRIP "${CRITIC_FABRIC_CUDA_ARCH_DIGITS}" CRITIC_FABRIC_CUDA_ARCH_DIGITS)
                if(CRITIC_FABRIC_CUDA_ARCH_DIGITS MATCHES "^[0-9]+$")
                    set(CRITIC_FABRIC_CUDA_ARCH "sm_${CRITIC_FABRIC_CUDA_ARCH_DIGITS}")
                endif()
            endif()
        endif()
        message(STATUS "Critic Fabric accelerator proof targets ${CRITIC_FABRIC_CUDA_ARCH}")

        set(CRITIC_FABRIC_CUDA_KERNEL_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/cuda_verify_kernel.obj)
        add_custom_command(
            OUTPUT ${CRITIC_FABRIC_CUDA_KERNEL_OBJECT}
            COMMAND ${CRITIC_FABRIC_NVCC}
                    -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda_verify_kernel.cu
                    -o ${CRITIC_FABRIC_CUDA_KERNEL_OBJECT}
                    -std=c++17 -O2 -arch=${CRITIC_FABRIC_CUDA_ARCH}
                    $<$<CONFIG:Debug>:-Xcompiler=/MDd>$<$<NOT:$<CONFIG:Debug>>:-Xcompiler=/MD>
                    -I${CMAKE_CURRENT_SOURCE_DIR}/tests
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda_verify_kernel.cu
            COMMENT "Compiling the CUDA verification kernel"
            VERBATIM)
        add_custom_target(critic_cuda_kernel DEPENDS ${CRITIC_FABRIC_CUDA_KERNEL_OBJECT})

        add_executable(critic_cuda_tests
            tests/test_cuda.cpp
            tests/test_support.cpp
            ${CRITIC_FABRIC_CUDA_KERNEL_OBJECT})
        add_dependencies(critic_cuda_tests critic_cuda_kernel)
        target_link_libraries(critic_cuda_tests PRIVATE
            CriticFabric::critic_fabric
            CUDA::cudart)
        target_include_directories(critic_cuda_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        critic_fabric_strict_warnings(critic_cuda_tests)
        if(CRITIC_FABRIC_BUILD_TESTS)
            add_test(NAME critic_fabric_tests_cuda COMMAND critic_cuda_tests)
        endif()
    else()
        message(STATUS "CUDA toolkit not found; the accelerator proof is not built")
    endif()
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS critic_fabric
    EXPORT CriticFabricTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/critic_fabric
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(CRITIC_FABRIC_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/CriticFabric)

install(EXPORT CriticFabricTargets
    FILE CriticFabricTargets.cmake
    NAMESPACE CriticFabric::
    DESTINATION ${CRITIC_FABRIC_CMAKE_DIR})

configure_package_config_file(
    cmake/CriticFabricConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CriticFabricConfig.cmake
    INSTALL_DESTINATION ${CRITIC_FABRIC_CMAKE_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CriticFabricConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/CriticFabricConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/CriticFabricConfigVersion.cmake
    DESTINATION ${CRITIC_FABRIC_CMAKE_DIR})
