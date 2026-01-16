function(aegis_add_test name)
    add_executable(${name} ${name}.cpp)
    # 因为 AegisEngine 已经是 PUBLIC 继承了，这里只需要 Link 即可，
    # Include 路径和 C++20 会自动传递过来！
    target_link_libraries(${name} PRIVATE AegisEngine)
    
    # 只有这个宏可能需要单独配（因为测试可能需要 TRACE 级别，而 Release 库不需要）
    target_compile_definitions(${name} PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
endfunction()
aegis_add_test(test_client)
aegis_add_test(test_hierarchy_timer)
aegis_add_test(test_actor_sleep)
aegis_add_test(test_aoi_grid)
aegis_add_test(test_scene_move)