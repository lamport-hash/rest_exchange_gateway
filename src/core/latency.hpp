#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string_view>

namespace gateway {

/// Sentinel for "no timestamp was taken" (tracking disabled or the
/// measurement point never ran). Real clocks return non-negative
/// nanoseconds, so -1 is unambiguous.
inline constexpr std::int64_t kNoLatencyStamp = -1;

/// Append-only JSON-lines latency log. One line per completed
/// measurement:
///   {"type":"latency","phase":"...","clientOrderId":"...",
///    "startNs":N,"endNs":N,"elapsedNs":N}
/// flushed on append. The clock is injectable so tests are
/// deterministic (mocked time); production uses the monotonic
/// steady clock (latency is a duration, never a wall-clock
/// difference).
///
/// Phases written by the OMS:
///  - "place_send_rest":  REST POST /orders handler entry ->
///                        just before the venue place call
///  - "place_send_oms":   OMS place() entry ->
///                        just before the venue place call
///  - "fill_state_update": execution report received ->
///                        registry state updated and persisted
///
/// Thread-safety: now() reads the clock (must be thread-safe);
/// measure() serializes appends with a mutex. Write failures are
/// silently dropped (latency metrics must never fail a trade path).
class LatencyLog
{
  public:
    /// Monotonic clock in nanoseconds.
    using Clock = std::function<std::int64_t()>;

    /// Open (create if missing) a_path for appending. The parent
    /// directory must exist. Uses the monotonic steady clock.
    explicit LatencyLog(std::filesystem::path a_path);

    /// Same, with an injectable clock (tests mock time; the clock must
    /// be thread-safe).
    LatencyLog(std::filesystem::path a_path, Clock a_clock);

    LatencyLog(const LatencyLog&) = delete;
    auto operator=(const LatencyLog&) -> LatencyLog& = delete;
    LatencyLog(LatencyLog&&) = delete;
    auto operator=(LatencyLog&&) -> LatencyLog& = delete;
    ~LatencyLog() = default;

    /// Read the clock (monotonic nanoseconds). The single time
    /// source shared by every latency measurement point.
    [[nodiscard]] auto now() const -> std::int64_t;

    /// Append one measurement line and flush. a_end_ns < a_start_ns
    /// (clock anomaly) is dropped rather than logged as a negative
    /// duration.
    void measure(std::string_view a_client_order_id, std::string_view a_phase,
                 std::int64_t a_start_ns, std::int64_t a_end_ns);

    [[nodiscard]] auto path() const -> const std::filesystem::path&;

    /// Production default: steady_clock nanoseconds (monotonic).
    [[nodiscard]] static auto default_clock() -> Clock;

  private:
    std::filesystem::path path_;
    Clock clock_;
    /// Guards out_ (measure may run on REST and venue feed threads).
    mutable std::mutex mutex_;
    std::ofstream out_;
};

} // namespace gateway
