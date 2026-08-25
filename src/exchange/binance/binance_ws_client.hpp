#pragma once

#include "exchange/binance/binance_config.hpp"
#include "exchange/binance/binance_signer.hpp"
#include "gateway/exchange_connector.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

namespace httplib::ws {
class WebSocketClient;
} // namespace httplib::ws

namespace gateway::exchange::binance {

enum class BinanceFeedEventType
{
    Connecting,
    Connected,
    Disconnected,
    Stopped,
    /// A venue message could not be normalized and was skipped; the feed
    /// stays up (detail carries the payload).
    ProtocolWarning
};

struct BinanceFeedEvent
{
    BinanceFeedEventType type = BinanceFeedEventType::Connecting;
    std::string detail;
};

/// Milliseconds since the Unix epoch (SIGNED request timestamps).
using UnixMsProvider = std::function<long long()>;

[[nodiscard]] auto real_unix_ms() -> long long;

/// Client for the Binance Spot WebSocket API
/// (wss://ws-api.testnet.binance.vision/ws-api/v3 on testnet).
///
/// Protocol (official Binance Spot WebSocket API docs):
/// - one JSON request per text frame: {"id": N, "method": "...",
///   "params": {...}}; responses echo the id with {"status": 200, "result"}
///   or {"status": 4xx/5xx, "error": {"code", "msg"}}
/// - SIGNED methods carry apiKey/timestamp/recvWindow/signature params
///   (HMAC-SHA256 hex over the alphabetically sorted query string)
/// - execution updates: the account's User Data Stream is subscribed on
///   the SAME connection via "userDataStream.subscribe.signature";
///   executionReport events arrive as {"subscriptionId", "event": {...}}
///   frames interleaved with request responses
/// - server pings every ~20s are answered by the WS layer automatically;
///   the client's own protocol heartbeat (wsPingIntervalSec /
///   wsMaxMissedPongs) closes a silent connection after too many missed
///   pongs so a half-open socket cannot stall trading for minutes
/// - on disconnect: pending requests fail with "transport" (outcome
///   unknown), the supervisor reconnects with backoff + jitter and
///   re-subscribes the User Data Stream; the backoff resets to the
///   initial value after every healthy session (the venue was proven
///   reachable, so one early failure must not pin it at max backoff)
///
/// Threading: call() may be invoked from any thread (one request frame per
/// call, sends serialized); feed events are delivered on a dedicated
/// notifier thread so handlers may issue synchronous venue calls (e.g. a
/// connectivity handler running reconciliation) without blocking the
/// reader loop that would have to dispatch their responses. Execution
/// reports are delivered on the supervisor/reader thread (they must stay
/// ordered with the stream and are expected to be cheap). Handlers must
/// be installed before start().
class BinanceWsClient final
{
  public:
    using ReportHandler = std::function<void(const ExecutionReport&)>;
    using EventHandler = std::function<void(const BinanceFeedEvent&)>;

    explicit BinanceWsClient(BinanceConfig a_config, UnixMsProvider a_timestamp = nullptr);
    ~BinanceWsClient();

    BinanceWsClient(const BinanceWsClient&) = delete;
    auto operator=(const BinanceWsClient&) -> BinanceWsClient& = delete;

    void set_report_handler(ReportHandler a_handler);
    void set_event_handler(EventHandler a_handler);

    /// Start the supervisor thread (idempotent).
    void start();

    /// Stop the supervisor, close the active session and join all threads
    /// (idempotent).
    void stop();

    [[nodiscard]] auto is_running() const -> bool;

    /// Send one WS-API request and wait for its correlated response.
    /// Returns the "result" payload on {"status": 200}. Errors:
    /// - "transport": no connection, send failed, response timeout, or the
    ///   session died mid-request (outcome unknown — resolve before retry)
    /// - "venue:<code>": definitive 4xx rejection (error.code)
    /// - "protocol": malformed response frame
    /// - 5xx responses and venue errors -1006/-1007 are returned as
    ///   "transport": the venue documents that execution status is then
    ///   unknown, not failed.
    [[nodiscard]] auto call(const std::string& a_method,
                            const nlohmann::json& a_params) -> Result<nlohmann::json>;

    /// SIGNED variant: adds apiKey/recvWindow/timestamp and the HMAC
    /// signature to a_params before sending (SIGNED request security).
    /// On venue:-1021 (timestamp outside recvWindow) it syncs the venue
    /// clock once via the public "time" method, stores the offset and
    /// re-signs the SAME request once (a -1021 reject happens before
    /// execution, so a re-sign can never duplicate an order; a re-send
    /// could). A -1021 persisting after the re-sign is returned as
    /// "protocol" (skew unrecoverable).
    [[nodiscard]] auto call_signed(const std::string& a_method,
                                   const nlohmann::json& a_params) -> Result<nlohmann::json>;

  private:
    struct Pending
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<nlohmann::json> response; // full response object
        bool failed = false;                    // session died / send failed
    };

    /// Result of one connect -> subscribe -> read cycle.
    struct SessionOutcome
    {
        /// Why the session ended (empty when stopped by the caller).
        std::string failure;
        /// Subscribed AND proven alive (a response served after the
        /// subscribe, or a subscribed uptime >= 30s). A healthy session
        /// resets the reconnect backoff: the venue was reachable, so the
        /// next failure should start at initialBackoffMs again.
        bool healthy = false;
    };

    void emit(BinanceFeedEventType a_type, std::string a_detail);
    void run(std::stop_token a_stop);
    /// Deliver queued feed events to the handler (never blocks the
    /// reader loop; see the threading note above).
    void pump_events(std::stop_token a_stop);
    /// One connect -> subscribe -> read cycle.
    [[nodiscard]] auto run_session(std::stop_token a_stop) -> SessionOutcome;

    /// Send a frame on the active session. False when there is no session
    /// or the send failed.
    [[nodiscard]] auto send_frame(const std::string& a_frame) -> bool;

    /// Route one decoded response/event frame. Returns true when the
    /// frame completed a pending request response (session-liveness
    /// signal for the reconnect backoff reset).
    [[nodiscard]] auto dispatch_message(const std::string& a_message) -> bool;

    /// Stamp apiKey/recvWindow/timestamp (shifted by the learned
    /// venue-clock offset) and the HMAC signature onto a_params. Erases
    /// any previous signature first, so re-signing the same object is
    /// safe.
    void apply_signature(nlohmann::json& a_params) const;

    /// Sync the venue clock via the public "time" method from a caller
    /// thread (response dispatched by the read loop). Stores the
    /// serverTime - localTime offset for subsequent signatures.
    [[nodiscard]] auto sync_server_time() -> Result<long long>;

    /// Best-effort clock sync executed on the session thread before the
    /// user data stream subscribe: sends a "time" request and reads the
    /// reply inline (the read loop has not started yet). False when the
    /// venue did not answer usable serverTime; the session proceeds with
    /// whatever offset is already stored.
    [[nodiscard]] auto inline_time_sync() -> bool;

    /// Complete/fail every pending request (session ended).
    void fail_all_pending();

    /// Deliver a normalized executionReport event.
    void handle_user_event(const nlohmann::json& a_event);

    /// Non-empty when dispatch saw a session-terminating venue event
    /// (serverShutdown / eventStreamTerminated). Session-thread only.
    std::string terminate_reason_;

    BinanceConfig config_;
    UnixMsProvider timestamp_;

    /// serverTime - localTime offset (ms), learned from the venue "time"
    /// method on -1021 recovery and at session start. Atomic because
    /// call_signed runs on arbitrary caller threads while the session
    /// thread may refresh it.
    std::atomic<long long> server_time_offset_ms_{0};

    ReportHandler report_handler_;
    EventHandler event_handler_;

    std::jthread supervisor_;
    std::jthread event_notifier_;
    std::atomic<bool> running_{false};

    /// Event hand-off queue: emit() (reader loop) enqueues, the notifier
    /// thread drains. Guards event_handler_ as well.
    std::mutex event_mutex_;
    std::condition_variable event_cv_;
    std::deque<BinanceFeedEvent> event_queue_;

    /// Active session (supervisor-owned lifetime; sends from other threads
    /// hold this mutex so the pointer is never used mid-swap). The inner
    /// WebSocket write lock serializes the frame itself.
    std::mutex client_mutex_;
    std::unique_ptr<httplib::ws::WebSocketClient> client_;

    std::mutex pending_mutex_;
    std::unordered_map<long long, std::shared_ptr<Pending>> pending_;
    long long next_id_ = 1;
};

} // namespace gateway::exchange::binance
