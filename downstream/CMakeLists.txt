cmake_minimum_required(VERSION 3.24)

project(CriticFabricDownstream LANGUAGES CXX)

# This project deliberately lives outside the Critic Fabric source tree. It
# consumes the installed package through find_package and never compiles a
# Critic Fabric source file directly.

find_package(CriticFabric 1.0 REQUIRED CONFIG)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE CriticFabric::critic_fabric)

if(MSVC)
    target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc)
else()
    target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
