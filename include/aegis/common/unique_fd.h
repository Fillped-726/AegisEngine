// include/aegis/common/unique_fd.h
#pragma once

#include <unistd.h> // [DEPENDENCY: POSIX unistd (close)]
#include <utility>
#include <algorithm>

namespace aegis::common
{
    // [INTENT: Strict RAII ownership manager for generic POSIX file descriptors]
    class UniqueFd
    {
    public:
        // [STATE_MUTATION: Adopt raw fd integer]
        explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}

        // [STATE_MUTATION: Deterministic OS resource release]
        ~UniqueFd()
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
            }
        }

        // [CONSTRAINT: Non-copyable; enforce unique ownership]
        UniqueFd(const UniqueFd &) = delete;
        UniqueFd &operator=(const UniqueFd &) = delete;

        // [STATE_MUTATION: Ownership transfer; invalidate source via std::exchange]
        UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

        // [STATE_MUTATION: Clean existing resource; adopt source payload]
        UniqueFd &operator=(UniqueFd &&other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        int get() const noexcept { return fd_; }

        // [STATE_MUTATION: Yield ownership; bypass dtor cleanup]
        [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

        explicit operator bool() const noexcept { return fd_ >= 0; }

        // [STATE_MUTATION: Conditional active resource cleanup and new payload adoption]
        // [INTENT: Aliasing/self-assignment guard prevents use-after-close]
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
        // [STATE: Raw OS handle]
        int fd_ = -1;
    };

} // namespace aegis::common