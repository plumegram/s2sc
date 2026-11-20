cmake_minimum_required(VERSION 3.21)
project(s2sc_agent CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(s2sc_agent src/main.cpp)
target_include_directories(s2sc_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

# Fully static C++ runtime: the binary must run from /data/local/tmp with no
# libc++_shared.so shipped alongside.
target_compile_options(s2sc_agent PRIVATE -O2 -fno-exceptions -fno-rtti -Wall -Wextra)
target_link_libraries(s2sc_agent PRIVATE log)
