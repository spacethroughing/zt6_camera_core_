cmake_minimum_required(VERSION 3.16)
# 核心部署包：仅编译 CLI 和 Web，测试保留在完整开发工作空间。
project(zt6_camera_console VERSION 2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
add_library(zt6_core cpp/core.cpp)
target_include_directories(zt6_core PUBLIC cpp third_party)
target_compile_definitions(zt6_core PRIVATE ZT6_WORKSPACE="${CMAKE_SOURCE_DIR}")
# ZT6 的 Boa 文件服务混用 CRLF/LF 终止响应头，需启用库提供的兼容选项。
# PUBLIC 保证核心与 Web 采用相同配置，避免头文件内联实现不一致。
target_compile_definitions(zt6_core PUBLIC CPPHTTPLIB_ALLOW_LF_AS_LINE_TERMINATOR)
target_link_libraries(zt6_core PUBLIC Threads::Threads)
target_compile_options(zt6_core PRIVATE -Wall -Wextra -Wpedantic)
add_executable(zt6 cpp/main.cpp)
target_link_libraries(zt6 PRIVATE zt6_core)
add_executable(zt6-web cpp/web.cpp)
target_link_libraries(zt6-web PRIVATE zt6_core)
