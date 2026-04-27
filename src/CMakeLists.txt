# 收集源文件
file(GLOB_RECURSE SRC_CORE "core/*.cpp")
file(GLOB_RECURSE SRC_GAME "game/*.cpp")
file(GLOB_RECURSE SRC_NET "net/*.cpp")
file(GLOB_RECURSE SRC_COMMON "common/*.cpp")

# 创建核心库 (AegisEngine)
add_library(AegisEngine SHARED 
    ${SRC_CORE} 
    ${SRC_GAME}
    ${SRC_NET}
    ${SRC_COMMON}
)

# 设置头文件搜索路径
target_include_directories(AegisEngine PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
)

# 链接依赖
target_link_libraries(AegisEngine PUBLIC 
    spdlog::spdlog
    PkgConfig::liburing
    AegisProto # 链接上面定义的协议库
    moodycamel::ConcurrentQueue
    common_jemalloc # 链接上面定义的 jemalloc 接口库
    BT::behaviortree_cpp
)

# 链接 liburing 的头文件路径 (如果是 pkg-config 找出来的)
target_include_directories(AegisEngine PRIVATE ${LIBURING_INCLUDE_DIRS})

message(STATUS "AegisEngine Core setup done.")