#include "aegis/common/aegisLog.h"
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <iostream>

namespace aegis
{

    Log::Log()
    {
        try
        {
            // 创建一个同步的控制台 Logger 作为兜底
            // 这样如果不调用 init_config，至少能在控制台看到日志
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(spdlog::level::trace); // 默认允许所有级别

            logger_ = std::make_shared<spdlog::logger>("console_default", console_sink);

            // 设置默认格式
            logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

            // 全局注册（可选，方便 spdlog 原生宏查找）
            spdlog::register_logger(logger_);
            logger_->set_level(spdlog::level::debug);
        }
        catch (const spdlog::spdlog_ex &ex)
        {
            std::cerr << "Log construction failed: " << ex.what() << std::endl;
        }
    }

    Log::~Log()
    {
        spdlog::shutdown();
    }

    void Log::init_config(const std::string &log_path, const std::string &app_name)
    {
        try
        {
            // 1. 初始化线程池 (8192 队列长度, 1个后台线程)
            spdlog::init_thread_pool(8192, 1);

            // 2. 准备 Sinks
            std::vector<spdlog::sink_ptr> sinks;

            // Sink A: 控制台 (带颜色)
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sinks.push_back(console_sink);

            // Sink B: 文件 (10MB * 5)
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_path, 1024 * 1024 * 10, 5);
            sinks.push_back(file_sink);

            // 3. 创建异步 Logger
            // 注意：这里会覆盖构造函数里创建的默认 logger_
            auto async_logger = std::make_shared<spdlog::async_logger>(
                app_name,
                sinks.begin(), sinks.end(),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::block); // 队列满则阻塞

            // 4. 接管全局 logger_
            // 先注销旧的，防止名字冲突
            spdlog::drop(app_name);
            spdlog::drop("console_default");

            logger_ = async_logger;
            spdlog::register_logger(logger_);

            // 5. 配置格式和级别
            logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [t:%t] [%^%l%$] [%s:%#] %v");
            logger_->set_level(spdlog::level::trace); // 确保开发阶段能看到 output
            logger_->flush_on(spdlog::level::err);

            // 打印一条确认日志
            // 注意：这里不需要用 LOG.info，因为我们在类内部，直接调 logger_
            logger_->info("AegisLog initialized successfully via init_config. Path: {}", log_path);
        }
        catch (const spdlog::spdlog_ex &ex)
        {
            // 初始化失败时打印到标准错误，不要吞掉异常
            std::cerr << "Log init failed: " << ex.what() << std::endl;
        }
    }

    void Log::set_level(spdlog::level::level_enum level)
    {
        // 1. 设置我们封装的 logger 实例级别
        if (logger_)
        {
            logger_->set_level(level);

            // 2. 只有当级别设置为 trace/debug 时，才强制 flush，否则保持高性能
            if (level <= spdlog::level::debug)
            {
                logger_->flush_on(level);
            }
            else
            {
                // 高负载模式下（Warn/Error），减少 flush 频率
                logger_->flush_on(spdlog::level::err);
            }
        }

        // 3. 同时更新 spdlog 的全局级别（这会影响所有未手动设置级别的 logger）
        spdlog::set_level(level);
    }

} // namespace aegis