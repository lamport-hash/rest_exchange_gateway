#pragma once

#include "core/retry.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <stop_token>
#include <string>
#include <utility>

namespace gateway::exchange {

/// A subscribed session alive at least this long counts as healthy even
/// without inbound traffic (quiet but functional link).
inline constexpr std::chrono::seconds kHealthySessionSeconds{30};

/// Wait for a_duration, but wake up as soon as a_stop is requested.
inline void interruptible_sleep(std::chrono::milliseconds a_duration, std::stop_token a_stop)
{
    std::condition_variable_any cv;
    std::mutex mutex;
    std::stop_callback wake_on_stop{a_stop, [&cv] { cv.notify_all(); }};
    std::unique_lock lock(mutex);
    cv.wait_for(lock, a_duration, [&a_stop] { return a_stop.stop_requested(); });
}

/// Result of one connect -> authenticate -> subscribe -> read cycle,
/// shared by the venue feed supervisors.
struct FeedSessionOutcome
{
    /// Why the session ended (empty when stopped by the caller).
    std::string failure;
    /// Subscribed AND proven alive (venue-specific liveness signal, or a
    /// subscribed uptime >= kHealthySessionSeconds). A healthy session
    /// resets the reconnect backoff.
    bool healthy = false;
};

/// Reconnect supervisor loop shared by the venue feeds: run one session
/// after another, reset the backoff counter after every healthy session
/// (the venue was proven reachable, so one early failure must not pin
/// the backoff at max), and sleep between sessions with exponential
/// backoff + jitter, interruptibly. a_session runs one cycle and returns
/// its outcome; a_on_disconnected receives each session's failure
/// detail. Returns when a_stop is requested; the final Stopped event is
/// left to the caller.
template <typename SessionFn, typename DisconnectedFn>
void reconnect_loop(std::stop_token a_stop, const RetryPolicy& a_policy, SessionFn&& a_session,
                    DisconnectedFn&& a_on_disconnected)
{
    unsigned connect_attempt = 0;
    while (!a_stop.stop_requested()) {
        const FeedSessionOutcome outcome = a_session();
        if (a_stop.stop_requested()) {
            break;
        }
        if (outcome.healthy) {
            // the venue was reachable and responsive this session: the
            // next failure is a fresh incident, not the tail of the last
            // one — start the backoff over instead of climbing to max
            connect_attempt = 0;
        }
        ++connect_attempt;
        a_on_disconnected(outcome.failure);

        static thread_local std::mt19937_64 engine{std::random_device{}()};
        std::uniform_real_distribution<double> uniform{0.0, 1.0};
        const auto raw = backoff_for(a_policy, static_cast<int>(connect_attempt));
        const auto delay = apply_jitter(a_policy, raw, uniform(engine));
        interruptible_sleep(delay, a_stop);
    }
}

} // namespace gateway::exchange
