# 定义所有 proto 文件
file(GLOB_RECURSE PROTO_FILES "internal/*.proto" "client/*.proto")

# 生成 C++ 源文件
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# 创建协议库 (静态库)
add_library(AegisProto STATIC ${PROTO_SRCS} ${PROTO_HDRS})

# 设置头文件包含路径 (这样其他模块 include 时不用加 weird path)
target_include_directories(AegisProto PUBLIC 
    ${CMAKE_CURRENT_BINARY_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

# 链接 Protobuf 库
target_link_libraries(AegisProto PUBLIC ${Protobuf_LIBRARIES})

# 打印信息
message(STATUS "Protocol Buffers targets setup done.")