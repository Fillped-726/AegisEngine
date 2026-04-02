function(aegis_add_benchmark name)
    add_executable(${name} ${name}.cpp)
    # 因为 AegisEngine 已经是 PUBLIC 继承了，这里只需要 Link 即可，
    # Include 路径和 C++20 会自动传递过来！
    target_link_libraries(${name} PRIVATE 
        AegisEngine
        common_jemalloc # 链接 jemalloc 接口库，确保测试也能使用 jemalloc
        benchmark::benchmark_main
    )
    
    # 只有这个宏可能需要单独配（因为测试可能需要 TRACE 级别，而 Release 库不需要）
    target_compile_definitions(${name} PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
endfunction()

aegis_add_benchmark(benchmark_echo)
# aegis_add_benchmark(benchmark_scheduler)
aegis_add_benchmark(benchmark_object_pool)
aegis_add_benchmark(benchmark_robot)
aegis_add_benchmark(bench_client)
# aegis_add_benchmark(benchmark_packet_pool)
aegis_add_benchmark(bm_ws_queue)
aegis_add_benchmark(bm_spinlock)
# aegis_add_benchmark(bm_aoi_grid)
aegis_add_benchmark(bm_packet)
