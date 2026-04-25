/**
 * @file scopeGuard.h
 * @brief RAII scope guard macro for exception-safe cleanup.
 */
#pragma once

// [INTENT: RAII wrapper for deterministic deferred execution/scope cleanup]
template <typename F>
class ScopeGuard
{
public:
    // [STATE_MUTATION: Bind callable payload]
    explicit ScopeGuard(F &&f) : func_(std::forward<F>(f)) {}

    // [INTENT: Trigger callable payload on scope exit]
    ~ScopeGuard() { func_(); }

    // [CONSTRAINT: Strict non-copyable semantics]
    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;

private:
    // [STATE: Callable payload]
    F func_;
};

// [INTENT: Factory helper for template type deduction/CTAD]
template <typename F>
ScopeGuard<F> make_scope_guard(F &&f)
{
    return ScopeGuard<F>(std::forward<F>(f));
}