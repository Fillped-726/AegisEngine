#pragma once

#include <string>
#include <memory>
#include <source_location> // C++20 核心特性
#include <format>          // C++20 格式化库
#include <spdlog/spdlog.h> 

namespace aegis
{

    template <typename... Args>
    struct LogFormat
    {
        std::format_string<Args...> fmt;
        std::source_location loc;

        template <typename T>
        consteval LogFormat(const T &s, const std::source_location &l = std::source_location::current())
            : fmt(s), loc(l) {}
    };

    class Log
    {
    public:
        // 删除静态 init 方法，改为获取单例实例
        static Log &instance()
        {
            static Log instance; // Meyers Singleton: 首次调用时初始化，线程安全
            return instance;
        }

        // 禁止拷贝和赋值
        Log(const Log &) = delete;
        Log &operator=(const Log &) = delete;

        // 初始化配置 (非必须，若不调用则使用默认配置)
        void init_config(const std::string &log_path, const std::string &app_name);

        // 动态设置日志级别
        // 用法: aegis::Log::instance().set_level(spdlog::level::warn);
        void set_level(spdlog::level::level_enum level);

        // 核心接口：使用模板处理任意参数，完全替代宏
        // loc 参数会自动获取调用者的文件名和行号
        template <typename... Args>
        void info(LogFormat<std::type_identity_t<Args>...> format, Args &&...args)
        {
            log_payload(spdlog::level::info, format.loc, format.fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void error(LogFormat<std::type_identity_t<Args>...> format, Args &&...args)
        {
            log_payload(spdlog::level::err, format.loc, format.fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void debug(LogFormat<std::type_identity_t<Args>...> format, Args &&...args)
        {
            log_payload(spdlog::level::debug, format.loc, format.fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void warn(LogFormat<std::type_identity_t<Args>...> format, Args &&...args)
        {
            log_payload(spdlog::level::warn, format.loc, format.fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void critical(LogFormat<std::type_identity_t<Args>...> format, Args &&...args)
        {
            log_payload(spdlog::level::critical, format.loc, format.fmt, std::forward<Args>(args)...);
        }

    private:
        Log(); // 构造函数私有化
        ~Log();

        std::shared_ptr<spdlog::logger> logger_;

        // 内部实现函数，将 C++20 的 source_location 转换为 spdlog 的 source_loc
        template <typename... Args>
        void log_payload(spdlog::level::level_enum lvl, const std::source_location &loc,
                         std::format_string<Args...> fmt, Args &&...args)
        {
            if (!logger_)
                return;

            if (!logger_->should_log(lvl))
            {
                return;
            }

            // 构建 spdlog 需要的 source_loc
            spdlog::source_loc spd_loc{loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};

            // 使用 C++20 std::format 格式化字符串 (比 spdlog 自带的 fmt 库编译更快且标准)
            // 注意：spdlog 自身支持 fmt 库，这里我们预先 format 成 string 传入
            // 为了极致性能，这里可以优化为直接传递给 spdlog，但 std::format 是标准趋势
            std::string msg = std::format(fmt, std::forward<Args>(args)...);

            logger_->log(spd_loc, lvl, msg);
        }
    };

} // namespace aegis