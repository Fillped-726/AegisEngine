cmake_minimum_required(VERSION 3.20)

project(AegisEngine 
    VERSION 0.1.0 
    LANGUAGES CXX C
)

# ==========================================
# 1. 全局设置
# ==========================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF) 

# 输出目录
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ==========================================
# 自动探测并开启 ASan (由 Preset 触发)
# ==========================================
option(ENABLE_ASAN "Enable AddressSanitizer for debugging" OFF)

if(ENABLE_ASAN)
    message(STATUS "🚀 [Aegis] AddressSanitizer ENABLED")
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)
    endif()
endif()

# ==========================================
# 2. 依赖查找 (vcpkg 托管一切)
# ==========================================
# 核心原则：只负责“找”，不负责“下”和“编”

find_package(behaviortree_cpp CONFIG REQUIRED)
# --- Log & Format ---
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# --- gRPC & Protobuf (最复杂的依赖被 vcpkg 完美解决) ---
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# --- Concurrent Queue ---
# vcpkg 会提供 moodycamel::ConcurrentQueue target
find_package(concurrentqueue CONFIG REQUIRED) 

# 1. 既然 target 里的路径不可靠，我们直接去文件系统里找 concurrentqueue.h 到底在哪
# HINTS 指引 CMake 去 vcpkg 安装目录里找
find_path(CONCURRENTQUEUE_INCLUDE_DIR
    NAMES "concurrentqueue.h"
    PATHS "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
    PATH_SUFFIXES "concurrentqueue/moodycamel" "concurrentqueue" "moodycamel"
    NO_DEFAULT_PATH
)

# 如果上面的没找到，尝试在默认路径再找一次
if(NOT CONCURRENTQUEUE_INCLUDE_DIR)
    find_path(CONCURRENTQUEUE_INCLUDE_DIR NAMES "concurrentqueue.h")
endif()

if(CONCURRENTQUEUE_INCLUDE_DIR)
    message(STATUS "✅ Force-located concurrentqueue.h at: ${CONCURRENTQUEUE_INCLUDE_DIR}")
    
    # 2. 创建一个垫片库
    if(NOT TARGET moodycamel_shim)
        add_library(moodycamel_shim INTERFACE)
    endif()

    # 3. 核心：直接把找到的这个绝对路径加进去！
    # 无论它藏多深，这下编译器一定能看见了
    target_include_directories(moodycamel_shim INTERFACE "${CONCURRENTQUEUE_INCLUDE_DIR}")

    # 4. 链接到底层库 (为了获取 lib 依赖)
    if(TARGET concurrentqueue::concurrentqueue)
        target_link_libraries(moodycamel_shim INTERFACE concurrentqueue::concurrentqueue)
    elseif(TARGET concurrentqueue)
        target_link_libraries(moodycamel_shim INTERFACE concurrentqueue)
    endif()

    # 5. 创建/更新别名
    if(NOT TARGET moodycamel::ConcurrentQueue)
        add_library(moodycamel::ConcurrentQueue ALIAS moodycamel_shim)
    endif()
else()
    message(FATAL_ERROR "❌ Could not find concurrentqueue.h in any vcpkg directory! Please check installation.")
endif()

find_package(PkgConfig REQUIRED)

# 使用 PkgConfig 查找 liburing
# IMPORTED_TARGET 参数非常重要，它会自动创建一个 PkgConfig::liburing 的目标
pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)

if(liburing_FOUND)
    message(STATUS "✅ Found liburing via PkgConfig")
else()
    message(FATAL_ERROR "❌ liburing not found! Please check vcpkg install.")
endif()
pkg_check_modules(JEMALLOC jemalloc)

if(JEMALLOC_FOUND)
    message(STATUS "✅ Jemalloc found via vcpkg/system")
    add_library(common_jemalloc INTERFACE)
    target_link_libraries(common_jemalloc INTERFACE ${JEMALLOC_LIBRARIES})
    target_include_directories(common_jemalloc INTERFACE ${JEMALLOC_INCLUDE_DIRS})
    target_compile_definitions(common_jemalloc INTERFACE USE_JEMALLOC)
else()
    message(WARNING "⚠️ Jemalloc NOT found! Falling back to system malloc.")
    add_library(common_jemalloc INTERFACE)
endif()

# --- GoogleTest (仅在测试开启时查找) ---
# vcpkg 默认安装 gtest，我们只需要在测试部分 link 即可

# ==========================================
# 3. 业务模块构建
# ==========================================
# 编译器警告优化
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 添加子目录
add_subdirectory(shared)      
add_subdirectory(src)         
add_subdirectory(services)    

# ==========================================
# 4. 测试模块
# ==========================================
enable_testing()

# 这里查找 GTest，vcpkg 已经准备好了
find_package(GTest CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_ORIG ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_subdirectory(tests/benchmark) 
add_subdirectory(tests/integration)
add_subdirectory(tests/unit)

