# 在你的 tests/CMakeLists.txt 中添加：

add_executable(test_client test_client.cpp)

# 链接 Protobuf 库
target_link_libraries(test_client 
    PRIVATE 
    AegisProto 
)

# 确保能找到 login.pb.h
target_include_directories(test_client PRIVATE AegisProto)