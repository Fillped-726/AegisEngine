option(AEGIS_USE_TSAN "Enable ThreadSanitizer for concurrency tests" OFF)
function(aegis_add_test name)
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
    # 如果开启了 TSan 开关
    if(AEGIS_USE_TSAN)
        message(STATUS "ThreadSanitizer enabled for ${name}")
        # 增加 -fno-pie -no-pie 固定内存布局，防止 ASLR 冲突
        target_compile_options(${name} PRIVATE -fsanitize=thread -g -O1 -fno-pie)
        target_link_options(${name} PRIVATE -fsanitize=thread -no-pie)
    endif()
endfunction()
aegis_add_test(test_architecture)
# aegis_add_test(test_hierarchy_timer)
# aegis_add_test(test_actor_sleep)
# aegis_add_test(test_aoi_grid)
aegis_add_test(test_work_stealing_queue)
aegis_add_test(test_sanity)