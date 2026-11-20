cmake_minimum_required(VERSION 3.21)
project(s2sc_pc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE NOMINMAX)
target_link_libraries(imgui PUBLIC d3d11 dwmapi)

add_executable(s2sc WIN32
    src/main.cpp
    src/app.cpp
    src/ui.cpp
    src/adb.cpp
    src/agent_client.cpp
    src/proc_mem.cpp
    src/skin_db.cpp
    src/config.cpp
)
target_include_directories(s2sc PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../shared)
target_compile_definitions(s2sc PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(s2sc PRIVATE imgui ws2_32 d3d11)

if (MSVC)
    # /utf-8: the UI contains UTF-8 glyphs (● ○) that MSVC would otherwise
    # reinterpret in the system codepage.
    target_compile_options(s2sc PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
    target_compile_options(imgui PRIVATE /W0)
endif()
