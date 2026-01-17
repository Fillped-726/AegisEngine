// include/aegis/common/unique_fd.h
#pragma once

#include <unistd.h>
#include <utility>
#include <algorithm> // for std::swap if needed

namespace aegis::common
{
    /**
     * @brief 遵循 RAII 原则的通用文件描述符包装器
     * 适用于 socket, timerfd, eventfd, signalfd, file 等所有 Linux fd
     */
    class UniqueFd
    {
    public:
        explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}

        ~UniqueFd()
        {
            if (fd_ >= 0)
            {
                // 生产环境建议：虽然析构函数不抛异常，但可以记录 close 失败的 Log
                // 特别是 EINTR 或 EIO
                ::close(fd_);
            }
        }

        // 禁止拷贝
        UniqueFd(const UniqueFd &) = delete;
        UniqueFd &operator=(const UniqueFd &) = delete;

        // 移动构造
        UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

        // 移动赋值
        UniqueFd &operator=(UniqueFd &&other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        int get() const noexcept { return fd_; }

        [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

        explicit operator bool() const noexcept { return fd_ >= 0; }

        /**
         * @brief 重置 FD
         * @note [Fix] 增加了自赋值检查。
         * 如果不检查，调用 fd.reset(fd.get()) 会导致 fd 被 close，但成员变量依然持有旧值，
         * 造成 Double Close 或 Use-After-Close。
         */
        void reset(int new_fd = -1) noexcept
        {
            if (fd_ == new_fd)
                return;

            if (fd_ >= 0)
            {
                ::close(fd_);
            }
            fd_ = new_fd;
        }

    private:
        int fd_ = -1;
    };

} // namespace aegis::common