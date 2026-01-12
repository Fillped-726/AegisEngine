# AegisEngine/third_party/CMakeLists.txt



# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# 1. moodycamel::ConcurrentQueue 配置 (手动封装)
# ---------------------------------------------------------------------------
# 因为这个库是 Header-Only 且其自带的 CMake 可能包含我们不需要的东西
# 我们直接创建一个 "接口库 (Interface Library)"
# ---------------------------------------------------------------------------

add_library(ConcurrentQueue INTERFACE)

# 指定头文件位置 (使用 BUILD_INTERFACE 生成表达式)
# 这样在编译时，CMake 会自动找到 third_party/concurrentqueue 目录
target_include_directories(ConcurrentQueue INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/concurrentqueue>
)

# 标记为 SYSTEM，这样编译器会忽略这个库内部的 Warning，防止干扰视线
target_include_directories(ConcurrentQueue SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/concurrentqueue>
)

# 创建一个别名，方便外部使用 (譬如用 moodycamel::ConcurrentQueue 引用)
add_library(moodycamel::ConcurrentQueue ALIAS ConcurrentQueue)

message(STATUS "Third-party: ConcurrentQueue added (Header-only).")