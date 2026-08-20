#pragma once

#include "exchange/okx/okx_rest_client.hpp"
#include "gateway/exchange_connector.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <string>
#include <thread>

namespace gateway::exchange::okx {

/// Effective private-WS host for a_config: demo credentials only
/// authenticate on the demo host (wspap.okx.com); when demo trading is
/// enabled and the configured host is the production default, the demo
/// host is used instead. Explicit non-default hosts pass through.
[[nodiscard]] auto ws_host_for(const OkxConfig& a_config) -> std::string;

enum class FeedEventType
{
    Connecting,
    Connected,
    Disconnected,
    Stopped,
    /// A venue message could not be normalized and was skipped (the feed
    /// stays up; the skipped payload is in the event detail).
    ProtocolWarning
};

struct FeedEvent
{
    FeedEventType type = FeedEventType::Connecting;
    std::string detail;
};

/// Client for the OKX v5 private WebSocket "orders" channel.
///
/// Protocol (official OKX v5 docs):
/// - connect to wss://ws.okx.com:8443/ws/v5/private (x-simulated-trading: 1
///   handshake header in demo mode)
/// - login with HMAC-SHA256(timestamp + "GET" + "/users/self/verify")
/// - subscribe {"op":"subscribe","args":[{"channel":"orders"}]} (all
///   instruments; every report carries its own instId)
/// - keepalive: send the text "ping" every ping_interval, answer server
///   "ping" with "pong"; a connection with no inbound message for more than
///   (max_missed_pongs + 1) * ping_interval is closed and reconnected
/// - on any disconnect: reconnect with exponential backoff + jitter (the
///   retry section of OkxConfig; unbounded attempts), re-login and
///   re-subscribe. Missed fills are reconciled in phase 3.
///
/// Threading: reports are delivered on the session reader thread, feed
/// events on the supervisor thread. Handlers must be installed before
/// start() and must not be re-registered afterwards.
class OkxOrdersFeed final
{
  public:
    using ReportHandler = std::function<void(const ExecutionReport&)>;
    using EventHandler = std::function<void(const FeedEvent&)>;

    explicit OkxOrdersFeed(OkxConfig a_config,
                           OkxRestClient::TimestampProvider a_timestamp = nullptr);
    ~OkxOrdersFeed();

    OkxOrdersFeed(const OkxOrdersFeed&) = delete;
    auto operator=(const OkxOrdersFeed&) -> OkxOrdersFeed& = delete;

    void set_report_handler(ReportHandler a_handler);
    void set_event_handler(EventHandler a_handler);

    /// Start the supervisor thread (idempotent; no-op when ws is disabled).
    void start();

    /// Stop the supervisor, close the active session and join all threads
    /// (idempotent).
    void stop();

    [[nodiscard]] auto is_running() const -> bool;

  private:
    void emit(FeedEventType a_type, std::string a_detail) const;
    void run(std::stop_token a_stop);

    /// One connect -> login -> subscribe -> read cycle. Returns the failure
    /// reason that ended the session (empty when stopped by the caller).
    [[nodiscard]] auto run_session(std::stop_token a_stop) -> std::string;

    /// Normalize one orders-channel message and deliver execution reports.
    void dispatch_orders_message(const nlohmann::json& a_message);

    OkxConfig config_;
    OkxRestClient::TimestampProvider timestamp_;
    ReportHandler report_handler_;
    EventHandler event_handler_;
    std::jthread supervisor_;
    std::atomic<bool> running_{false};

    /// Closes the active session's connection to unblock a reader stuck in
    /// read(); guarded by session_mutex_. Set/cleared by the supervisor at
    /// session boundaries, invoked by stop().
    std::mutex session_mutex_;
    std::function<void()> close_active_session_;
};

} // namespace gateway::exchange::okx
