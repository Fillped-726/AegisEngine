function(aegis_add_unit name)
    add_executable(${name} ${name}.cpp)
    # 因为 AegisEngine 已经是 PUBLIC 继承了，这里只需要 Link 即可，
    # Include 路径和 C++20 会自动传递过来！
    target_link_libraries(${name} PRIVATE 
    AegisEngine
    GTest::gtest_main
    common_jemalloc # 链接 jemalloc 接口库，确保测试也能使用 jemalloc
    )
    
    # 只有这个宏可能需要单独配（因为测试可能需要 TRACE 级别，而 Release 库不需要）
    target_compile_definitions(${name} PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
endfunction()


aegis_add_unit(test_object_pool)
aegis_add_unit(test_packet)
aegis_add_unit(test_acceptor)
aegis_add_unit(test_batcher)
aegis_add_unit(test_socket)
aegis_add_unit(test_env_connection)
