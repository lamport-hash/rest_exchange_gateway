#include "core/latency.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <utility>

namespace gateway {

LatencyLog::LatencyLog(std::filesystem::path a_path)
    : LatencyLog(std::move(a_path), default_clock())
{}

LatencyLog::LatencyLog(std::filesystem::path a_path, Clock a_clock)
    : path_(std::move(a_path)), clock_(std::move(a_clock)), out_(path_, std::ios::app)
{
    if (!clock_) {
        clock_ = default_clock();
    }
}

auto LatencyLog::now() const -> std::int64_t
{
    return clock_();
}

void LatencyLog::measure(std::string_view a_client_order_id, std::string_view a_phase,
                         std::int64_t a_start_ns, std::int64_t a_end_ns)
{
    if (a_start_ns < 0 || a_end_ns < a_start_ns) {
        return;
    }
    const nlohmann::json line = {
        {"type", "latency"},     {"phase", a_phase},  {"clientOrderId", a_client_order_id},
        {"startNs", a_start_ns}, {"endNs", a_end_ns}, {"elapsedNs", a_end_ns - a_start_ns}};
    const std::lock_guard lock(mutex_);
    out_ << line.dump() << '\n';
    out_.flush();
}

auto LatencyLog::path() const -> const std::filesystem::path&
{
    return path_;
}

auto LatencyLog::default_clock() -> Clock
{
    return [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
}

} // namespace gateway
