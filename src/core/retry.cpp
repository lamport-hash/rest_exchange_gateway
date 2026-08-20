#include "core/retry.hpp"

#include <algorithm>
#include <random>
#include <thread>

namespace gateway {

namespace {
constexpr int kMaxAttemptsLimit = 100;
/// Upper bound for any duration field (1 hour in milliseconds).
constexpr std::uint64_t kMaxDurationMs = 3600000ULL;

auto unsigned_ms_field(const nlohmann::json& a_node, const char* a_name,
                       std::chrono::milliseconds a_default, std::string& a_error,
                       std::chrono::milliseconds& a_out) -> bool
{
    const auto it = a_node.find(a_name);
    if (it == a_node.end()) {
        a_out = a_default;
        return true;
    }
    if (!it->is_number_unsigned()) {
        a_error = std::string("retry field \"") + a_name + "\" must be an unsigned number";
        return false;
    }
    const std::uint64_t value = it->get<std::uint64_t>();
    if (value > kMaxDurationMs) {
        a_error = std::string("retry field \"") + a_name + "\" is out of range";
        return false;
    }
    a_out = std::chrono::milliseconds{static_cast<long long>(value)};
    return true;
}
} // namespace

auto real_retry_clock() -> RetryClock
{
    return RetryClock{
        .now = [] { return std::chrono::steady_clock::now(); },
        .sleep =
            [](std::chrono::milliseconds a_duration) { std::this_thread::sleep_for(a_duration); },
        .random01 =
            [] {
                static thread_local std::mt19937_64 engine{std::random_device{}()};
                std::uniform_real_distribution<double> uniform{0.0, 1.0};
                return uniform(engine);
            },
    };
}

auto retry_policy_from_json(const nlohmann::json& a_node,
                            const RetryPolicy& a_defaults) -> Result<RetryPolicy>
{
    if (!a_node.is_object()) {
        return Error{"protocol", "retry config must be a JSON object"};
    }

    RetryPolicy policy = a_defaults;
    std::string error;

    if (const auto it = a_node.find("maxAttempts"); it != a_node.end()) {
        if (!it->is_number_unsigned()) {
            return Error{"protocol", "retry field \"maxAttempts\" must be an unsigned number"};
        }
        const auto value = it->get<std::uint64_t>();
        if (value < 1 || value > kMaxAttemptsLimit) {
            return Error{"protocol", "retry field \"maxAttempts\" must be in [1, 100]"};
        }
        policy.max_attempts = static_cast<int>(value);
    }

    std::chrono::milliseconds initial{};
    std::chrono::milliseconds max_backoff{};
    std::chrono::milliseconds budget{};
    if (!unsigned_ms_field(a_node, "initialBackoffMs", a_defaults.initial_backoff, error,
                           initial) ||
        !unsigned_ms_field(a_node, "maxBackoffMs", a_defaults.max_backoff, error, max_backoff) ||
        !unsigned_ms_field(a_node, "budgetMs", a_defaults.budget, error, budget)) {
        return Error{"protocol", error};
    }
    if (max_backoff < initial) {
        return Error{"protocol", "retry maxBackoffMs must be >= initialBackoffMs"};
    }
    policy.initial_backoff = initial;
    policy.max_backoff = max_backoff;
    policy.budget = budget;

    if (const auto it = a_node.find("multiplier"); it != a_node.end()) {
        if (!it->is_number()) {
            return Error{"protocol", "retry field \"multiplier\" must be a number"};
        }
        const double value = it->get<double>();
        if (!(value >= 1.0)) {
            return Error{"protocol", "retry field \"multiplier\" must be >= 1.0"};
        }
        policy.multiplier = value;
    }
    if (const auto it = a_node.find("jitter"); it != a_node.end()) {
        if (!it->is_number()) {
            return Error{"protocol", "retry field \"jitter\" must be a number"};
        }
        const double value = it->get<double>();
        if (!(value >= 0.0 && value <= 1.0)) {
            return Error{"protocol", "retry field \"jitter\" must be in [0, 1]"};
        }
        policy.jitter = value;
    }

    return policy;
}

auto backoff_for(const RetryPolicy& a_policy, int a_retry_index) -> std::chrono::milliseconds
{
    const int index = std::max(a_retry_index, 1);
    double raw = static_cast<double>(a_policy.initial_backoff.count());
    for (int i = 1; i < index; ++i) {
        raw *= a_policy.multiplier;
        if (raw >= static_cast<double>(a_policy.max_backoff.count())) {
            return a_policy.max_backoff;
        }
    }
    const auto capped =
        std::min(std::chrono::milliseconds{static_cast<long long>(raw)}, a_policy.max_backoff);
    return std::max(capped, std::chrono::milliseconds{0});
}

auto apply_jitter(const RetryPolicy& a_policy, std::chrono::milliseconds a_raw,
                  double a_u01) -> std::chrono::milliseconds
{
    const double clamped = std::clamp(a_u01, 0.0, 0.999999);
    const double factor = 1.0 - a_policy.jitter * clamped;
    const auto jittered = std::chrono::duration_cast<std::chrono::milliseconds>(a_raw * factor);
    return std::max(jittered, std::chrono::milliseconds{0});
}

} // namespace gateway
