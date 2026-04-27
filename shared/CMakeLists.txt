cmake_minimum_required(VERSION 3.20)
project(AegisShared)

find_package(Protobuf CONFIG REQUIRED)

# 1. 明确定义 PROTO 所在的根目录
# 因为你的 tree 显示文件在 shared/proto/*.proto
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")

# 2. 收集所有 .proto 文件
file(GLOB PROTO_FILES "${PROTO_DIR}/*.proto")

# 3. 定义静态库
add_library(AegisProto STATIC ${PROTO_FILES})

set_target_properties(AegisProto PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 4. 核心修复：使用 IMPORT_DIRS 强制对齐 -I 路径
set(PROTO_IMPORT_DIRS "${PROTO_DIR}")

# 4.1 生成消息代码（仅 protobuf，无 gRPC）
protobuf_generate(
    TARGET AegisProto 
    LANGUAGE cpp
    IMPORT_DIRS ${PROTO_IMPORT_DIRS}
)

# 5. 配置包含路径 (供其他模块使用)
target_include_directories(AegisProto
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

# 6. 链接依赖
target_link_libraries(AegisProto
    PUBLIC
    protobuf::libprotobuf
)