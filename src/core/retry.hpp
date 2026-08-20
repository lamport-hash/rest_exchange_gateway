#pragma once

#include "gateway/result.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <concepts>
#include <functional>
#include <utility>

namespace gateway {

/// Venue-agnostic retry behavior for outbound requests.
/// - max_attempts: total attempts including the first one (>= 1)
/// - budget: wall-clock deadline; a retry attempt is only started while
///   now() < start + budget (an in-flight attempt is never preempted)
/// - backoff before retry k (k >= 1): min(initial * multiplier^k, max_backoff),
///   then jitter-scaled down to [delay * (1 - jitter), delay]
///   (jitter = 1 means full jitter: uniform in [0, delay]).
/// Precondition on a_jitter and a_multiplier: 0 <= jitter <= 1, multiplier >= 1
/// (enforced by retry_policy_from_json; direct constructors must respect it).
struct RetryPolicy
{
    int max_attempts = 3;
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{2000};
    double multiplier = 2.0;
    double jitter = 0.5;
    std::chrono::milliseconds budget{5000};
};

/// Injectable time, sleep and randomness source so retry logic is fully
/// deterministic in tests. Production wiring: real_retry_clock().
struct RetryClock
{
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
    /// Uniform double in [0, 1); used to apply jitter.
    std::function<double()> random01;
};

/// Real steady clock, real sleep, randomly seeded engine.
[[nodiscard]] auto real_retry_clock() -> RetryClock;

/// Parse a retry policy from a JSON object; missing fields keep a_defaults.
/// Recognized fields: maxAttempts, initialBackoffMs, maxBackoffMs, multiplier,
/// jitter, budgetMs. Errors: "protocol" (wrong types or out-of-range values;
/// maxBackoffMs must be >= initialBackoffMs). Unknown fields are ignored.
[[nodiscard]] auto retry_policy_from_json(const nlohmann::json& a_node,
                                          const RetryPolicy& a_defaults) -> Result<RetryPolicy>;

/// Backoff before retry a_retry_index (1-based: the first retry is index 1),
/// capped at max_backoff, before jitter. Non-positive indices use index 1.
[[nodiscard]] auto backoff_for(const RetryPolicy& a_policy,
                               int a_retry_index) -> std::chrono::milliseconds;

/// Scale a_raw down with jitter factor from a_u01 in [0,1):
/// result in [a_raw * (1 - jitter), a_raw]. Shared by the REST retry driver
/// and the WebSocket reconnect loop so jitter semantics stay identical.
[[nodiscard]] auto apply_jitter(const RetryPolicy& a_policy, std::chrono::milliseconds a_raw,
                                double a_u01) -> std::chrono::milliseconds;

/// Drive a_attempt according to a_policy. a_retryable classifies an error as
/// retryable; other errors (and successes) are returned immediately. Backoff
/// sleeps are truncated so they never extend past the budget deadline, and a
/// retry is only started when the previous failure happened before the
/// deadline. The last attempt is returned unchanged when attempts or budget
/// are exhausted.
template <typename T, typename AttemptFn, typename RetryableFn>
    requires std::invocable<AttemptFn> &&
                 std::convertible_to<std::invoke_result_t<AttemptFn>, Result<T>> &&
                 std::invocable<RetryableFn, const Error&> &&
                 std::convertible_to<std::invoke_result_t<RetryableFn, const Error&>, bool>
[[nodiscard]] auto with_retries(const RetryPolicy& a_policy, const RetryClock& a_clock,
                                AttemptFn a_attempt, RetryableFn a_retryable) -> Result<T>
{
    const auto deadline = a_clock.now() + a_policy.budget;
    for (int attempt_index = 0; attempt_index < a_policy.max_attempts; ++attempt_index) {
        Result<T> result = a_attempt();
        const bool last_attempt = attempt_index + 1 >= a_policy.max_attempts;
        if (result.is_ok() || last_attempt || !a_retryable(result.error())) {
            return result;
        }
        const auto failed_at = a_clock.now();
        if (failed_at >= deadline) {
            return result;
        }
        const auto raw = backoff_for(a_policy, attempt_index + 1);
        auto delay = apply_jitter(a_policy, raw, a_clock.random01());
        const auto remaining = deadline - failed_at;
        if (delay > remaining) {
            delay = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        }
        a_clock.sleep(delay);
    }
    return Error{"protocol", "retry driver exhausted attempts"};
}

} // namespace gateway
