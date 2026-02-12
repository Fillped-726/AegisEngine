project(gate_server)

# 包含路径：核心库 + 本服务的头文件
include_directories(
    ${CMAKE_SOURCE_DIR}/../  # 允许引用 aegis 核心库
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/logic # 允许引用逻辑层头文件
)

# 收集源文件：包括 server 核心和 logic 业务
file(GLOB_RECURSE SRC_FILES 
    "src/*.cpp" 
    "src/logic/*.cpp"
)

add_executable(gate_server ${SRC_FILES})

# 链接核心库 (假设你的核心库叫 AegisEngine)
target_link_libraries(gate_server PRIVATE 
    AegisEngine
    AegisProto
    common_jemalloc # 链接 jemalloc 接口库，确保可执行文件也能使用 jemalloc
)