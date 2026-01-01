cmake_minimum_required(VERSION 3.20)

# 项目元数据
project(AegisEngine 
    VERSION 0.1.0 
    LANGUAGES CXX C
)

# ==========================================
# 1. 全局设置 (Global Settings)
# ==========================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # 确保只使用标准 C++，不依赖 GCC 特性

# 输出目录设置 (bin/ 放可执行, lib/ 放库)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 开启所有警告 (这对工业级项目很重要)
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ==========================================
# 2. 依赖管理 (Dependencies via FetchContent)
# ==========================================
include(FetchContent)

message(STATUS "Downloading Dependencies... This may take a while.")

# 2.1 spdlog (高性能日志)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.12.0
)
FetchContent_MakeAvailable(spdlog)

# 2.2 liburing (异步IO核心)
# 注意：在 Linux 上通常推荐用 apt install liburing-dev
# 这里为了确保版本统一，我们尝试查找系统库，找不到再报错
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)
if(LIBURING_FOUND)
    message(STATUS "Found liburing: ${LIBURING_VERSION}")
else()
    message(FATAL_ERROR "liburing not found! Please run: sudo apt install liburing-dev")
endif()

# 2.3 Protobuf (序列化)
# Protobuf 编译很慢，通常建议找系统安装的。
find_package(Protobuf REQUIRED)
if(Protobuf_FOUND)
    message(STATUS "Found Protobuf: ${Protobuf_VERSION}")
else()
    message(FATAL_ERROR "Protobuf not found! Please run: sudo apt install protobuf-compiler libprotobuf-dev")
endif()

# ==========================================
# 3. 添加子模块 (Subdirectories)
# ==========================================
add_subdirectory(protocols) # 先生成协议代码
add_subdirectory(src)       # 再编译核心库
add_subdirectory(services)  # 最后编译业务服务
# add_subdirectory(tests)   # 稍后开启