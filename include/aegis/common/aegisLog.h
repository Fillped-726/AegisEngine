/**
 * @file aegisLog.h
 * @brief Singleton logging facade wrapping spdlog.
 */
#pragma once

#include <string>
#include <memory>
#include <source_location>
#include <format>
#include <spdlog/spdlog.h>

namespace aegis
{
    // [INTENT: Aggregate format string + call-site metadata at compile-time]
    // [DEPENDENCY: std::format_string, std::source_location]
    template <typename... Args>
    struct LogFormat
    {
        std::format_string<Args...> fmt;
        std::source_location loc;

        template <typename T>
        consteval LogFormat(const T &s, const std::source_location &l = std::source_location::current())
            : fmt(s), loc(l) {}
    };

    // [INTENT: Singleton facade for spdlog backend]
    // [STATE: logger_ shared pointer]
    class Log
    {
    public:
        // [INTENT: Thread-safe lazy initialization (Meyers Singleton)]
        static Log &instance()
        {
            static Log instance;
            return instance;
        }

        Log(const Log &) = delete;
        Log &operator=(const Log &) = delete;

        // [STATE_MUTATION: Initialize/reconfigure spdlog sinks]
        void init_config(const std::string &log_path, const std::string &app_name);

        // [STATE_MUTATION: Filter level update]
        void set_level(spdlog::level::level_enum level);

        // [INTENT: Type-safe dispatch with automatic location capture]
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
        Log();
        ~Log();

        std::shared_ptr<spdlog::logger> logger_;

        // [INTENT: Bridge C++20 location/format to spdlog native types]
        // [PERF: Pre-formats string via std::format before spdlog handoff]
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

            spdlog::source_loc spd_loc{loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
            std::string msg = std::format(fmt, std::forward<Args>(args)...);

            logger_->log(spd_loc, lvl, msg);
        }
    };

} // namespace aegis