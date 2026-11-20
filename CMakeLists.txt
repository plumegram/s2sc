cmake_minimum_required(VERSION 3.21)
project(s2sc LANGUAGES CXX)

# One predictable binary location regardless of the generator: multi-config
# (Visual Studio) appends a per-config directory, single-config (Ninja) does not.
# Setting the per-config variables explicitly flattens both to build/bin.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY                ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG          ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE        ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(pc)
