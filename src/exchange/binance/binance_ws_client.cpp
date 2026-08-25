#include "exchange/binance/binance_ws_client.hpp"

#include "core/decimal.hpp"
#include "core/retry.hpp"
#include "exchange/binance/binance_wire.hpp"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <random>
#include <utility>

namespace gateway::exchange::binance {

namespace {

/// A subscribed session alive at least this long counts as healthy even
/// without served requests (quiet but functional link).
constexpr auto kHealthySessionSeconds = std::chrono::seconds{30};

/// Wait for a_duration, but wake up as soon as a_stop is requested.
void interruptible_sleep(std::chrono::milliseconds a_duration, std::stop_token a_stop)
{
    std::condition_variable_any cv;
    std::mutex mutex;
    std::stop_callback wake_on_stop{a_stop, [&cv] { cv.notify_all(); }};
    std::unique_lock lock(mutex);
    cv.wait_for(lock, a_duration, [&a_stop] { return a_stop.stop_requested(); });
}

/// Average fill price = cummulativeQuoteQty / cumulative filled quantity
/// (docs note), exact at up to 8 fractional digits. Empty when nothing
/// filled yet.
auto average_price(const std::string& a_quote_qty, const std::string& a_filled) -> std::string
{
    if (a_quote_qty.empty() || a_filled.empty()) {
        return {};
    }
    const auto quote = parse_decimal(a_quote_qty);
    const auto filled = parse_decimal(a_filled);
    if (!quote.is_ok() || !filled.is_ok() || is_zero(filled.value())) {
        return {};
    }
    const auto price = div(quote.value(), filled.value(), kMaxDecimalScale);
    if (!price.is_ok()) {
        return {};
    }
    return decimal_to_string(price.value());
}

/// True when a result is the venue's -1021 (timestamp outside the
/// recvWindow): the local clock disagrees with the venue clock.
auto is_clock_skew_reject(const Result<nlohmann::json>& a_result) -> bool
{
    return !a_result.is_ok() && a_result.error().code == "venue:-1021";
}

/// Integer serverTime out of a "time" method RESULT payload.
auto server_time_of(const nlohmann::json& a_result) -> std::optional<long long>
{
    const auto server_time = a_result.find("serverTime");
    if (server_time == a_result.end() || !server_time->is_number_integer()) {
        return std::nullopt;
    }
    return server_time->get<long long>();
}

} // namespace

auto real_unix_ms() -> long long
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

BinanceWsClient::BinanceWsClient(BinanceConfig a_config, UnixMsProvider a_timestamp)
    : config_(std::move(a_config)),
      timestamp_(std::move(a_timestamp) ? a_timestamp : UnixMsProvider{&real_unix_ms})
{}

BinanceWsClient::~BinanceWsClient()
{
    stop();
}

void BinanceWsClient::set_report_handler(ReportHandler a_handler)
{
    report_handler_ = std::move(a_handler);
}

void BinanceWsClient::set_event_handler(EventHandler a_handler)
{
    const std::lock_guard lock(event_mutex_);
    event_handler_ = std::move(a_handler);
}

void BinanceWsClient::emit(BinanceFeedEventType a_type, std::string a_detail)
{
    {
        const std::lock_guard lock(event_mutex_);
        event_queue_.push_back(BinanceFeedEvent{a_type, std::move(a_detail)});
    }
    event_cv_.notify_all();
}

void BinanceWsClient::pump_events(std::stop_token a_stop)
{
    std::unique_lock lock(event_mutex_);
    while (true) {
        event_cv_.wait(lock, [&] { return a_stop.stop_requested() || !event_queue_.empty(); });
        while (!event_queue_.empty()) {
            BinanceFeedEvent event = std::move(event_queue_.front());
            event_queue_.pop_front();
            EventHandler handler = event_handler_; // copy: handlers may be reset mid-delivery
            lock.unlock();
            if (handler) {
                handler(event);
            }
            lock.lock();
        }
        if (a_stop.stop_requested()) {
            break;
        }
    }
}

void BinanceWsClient::start()
{
    const bool already = running_.exchange(true);
    if (already) {
        return;
    }
    event_notifier_ = std::jthread([this](std::stop_token stop) { pump_events(std::move(stop)); });
    supervisor_ = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

void BinanceWsClient::stop()
{
    if (supervisor_.joinable()) {
        supervisor_.request_stop();
        {
            const std::lock_guard lock(client_mutex_);
            if (client_) {
                client_->close(httplib::ws::CloseStatus::Normal, "client stopped");
            }
        }
        supervisor_.join();
    }
    // join the notifier last so it drains every event the supervisor
    // emitted during shutdown (including the final Stopped)
    if (event_notifier_.joinable()) {
        event_notifier_.request_stop();
        {
            const std::lock_guard lock(event_mutex_);
            event_cv_.notify_all();
        }
        event_notifier_.join();
    }
    running_ = false;
    fail_all_pending();
}

auto BinanceWsClient::is_running() const -> bool
{
    return running_;
}

auto BinanceWsClient::send_frame(const std::string& a_frame) -> bool
{
    const std::lock_guard lock(client_mutex_);
    if (!client_) {
        return false;
    }
    return client_->send(a_frame);
}

auto BinanceWsClient::call(const std::string& a_method,
                           const nlohmann::json& a_params) -> Result<nlohmann::json>
{
    const long long id = [this] {
        const std::lock_guard lock(pending_mutex_);
        return next_id_++;
    }();

    auto pending = std::make_shared<Pending>();
    {
        const std::lock_guard lock(pending_mutex_);
        pending_[id] = pending;
    }

    const nlohmann::json frame{{"id", id}, {"method", a_method}, {"params", a_params}};
    if (!send_frame(frame.dump())) {
        const std::lock_guard lock(pending_mutex_);
        pending_.erase(id);
        return Error{"transport", "cannot send " + a_method + ": no connection"};
    }

    bool timed_out = false;
    {
        std::unique_lock lock(pending->mutex);
        const auto deadline = std::chrono::steady_clock::now() + config_.request_timeout;
        pending->cv.wait_until(lock, deadline, [&pending] {
            return pending->response.has_value() || pending->failed;
        });
        if (!pending->response.has_value() && !pending->failed) {
            timed_out = true; // venue-side outcome unknown
        }
    }
    {
        const std::lock_guard lock(pending_mutex_);
        pending_.erase(id);
    }

    if (timed_out) {
        return Error{"transport",
                     a_method + " response timed out (outcome unknown, resolve before retry)"};
    }
    if (pending->failed) {
        return Error{"transport",
                     a_method + " failed: connection lost (outcome unknown, resolve before retry)"};
    }

    const auto& response = *pending->response;
    const auto status = response.find("status");
    if (status == response.end() || !status->is_number_integer()) {
        return Error{"protocol", "response without integer status: " + response.dump()};
    }
    const auto http_status = status->get<int>();
    if (http_status == 200) {
        const auto result = response.find("result");
        if (result == response.end()) {
            return Error{"protocol", "successful response without result: " + response.dump()};
        }
        return *result;
    }
    // 5xx: execution status unknown per the docs — treat like transport so
    // callers resolve the true outcome instead of assuming failure.
    if (http_status >= 500) {
        return Error{"transport", "venue " + std::to_string(http_status) +
                                      " (outcome unknown, resolve before retry)"};
    }
    const auto error = response.find("error");
    std::string code = "0";
    std::string message = "venue rejected the request";
    if (error != response.end() && error->is_object()) {
        code = std::to_string(error->value("code", 0));
        message = error->value("msg", message);
    }
    // -1006 (UNEXPECTED_RESP) / -1007 (TIMEOUT): per the docs the request
    // may still have been executed — the outcome is unknown, exactly like
    // a 5xx or a dropped response. Map to "transport" regardless of the
    // HTTP status so resolve-then-retry runs (order.status before any
    // re-send) instead of a definitive, possibly false rejection.
    if (code == "-1006" || code == "-1007") {
        return Error{"transport", "venue error " + code + " (" + message +
                                      ", outcome unknown, resolve before retry)"};
    }
    return Error{"venue:" + code, message};
}

auto BinanceWsClient::call_signed(const std::string& a_method,
                                  const nlohmann::json& a_params) -> Result<nlohmann::json>
{
    nlohmann::json signed_params = a_params;
    apply_signature(signed_params);
    auto result = call(a_method, signed_params);
    if (!is_clock_skew_reject(result)) {
        return result;
    }
    // -1021: sync the venue clock and re-sign the SAME request once. A
    // -1021 rejection happens before execution, so re-signing can never
    // duplicate an order (unlike a blind re-send, which could).
    const auto synced = sync_server_time();
    if (!synced.is_ok()) {
        return result; // keep the definitive venue:-1021 error
    }
    apply_signature(signed_params); // re-sign with the fresh offset
    result = call(a_method, signed_params);
    if (is_clock_skew_reject(result)) {
        return Error{"protocol",
                     "clock skew unrecoverable: -1021 persists after time sync and one re-sign"};
    }
    return result;
}

void BinanceWsClient::apply_signature(nlohmann::json& a_params) const
{
    a_params["apiKey"] = config_.api_key;
    a_params["recvWindow"] = config_.recv_window_ms;
    a_params["timestamp"] =
        timestamp_() + server_time_offset_ms_.load(std::memory_order_relaxed);
    a_params.erase("signature"); // never sign the signature itself
    a_params["signature"] = sign_params(a_params, config_.secret_key);
}

auto BinanceWsClient::sync_server_time() -> Result<long long>
{
    const auto result = call("time", nlohmann::json::object());
    if (!result.is_ok()) {
        return result.error();
    }
    const auto server_time = server_time_of(result.value());
    if (!server_time.has_value()) {
        return Error{"protocol", "time response without integer serverTime: " +
                                     result.value().dump()};
    }
    const long long offset = *server_time - timestamp_();
    server_time_offset_ms_.store(offset, std::memory_order_relaxed);
    return offset;
}

auto BinanceWsClient::inline_time_sync() -> bool
{
    const long long id = [this] {
        const std::lock_guard lock(pending_mutex_);
        return next_id_++;
    }();
    const nlohmann::json frame{{"id", id}, {"method", "time"}, {"params", nlohmann::json::object()}};
    if (!client_->send(frame.dump())) {
        return false;
    }
    std::string message;
    if (client_->read(message) != httplib::ws::ReadResult::Text) {
        return false;
    }
    const auto parsed = nlohmann::json::parse(message, nullptr, false);
    const auto response_id = parsed.find("id");
    const auto result = parsed.find("result");
    if (parsed.is_discarded() || response_id == parsed.end() ||
        !response_id->is_number_integer() || response_id->get<long long>() != id ||
        result == parsed.end() || !result->is_object()) {
        return false;
    }
    const auto server_time = server_time_of(*result);
    if (!server_time.has_value()) {
        return false;
    }
    server_time_offset_ms_.store(*server_time - timestamp_(), std::memory_order_relaxed);
    return true;
}

void BinanceWsClient::fail_all_pending()
{
    std::vector<std::shared_ptr<Pending>> waiters;
    {
        const std::lock_guard lock(pending_mutex_);
        waiters.reserve(pending_.size());
        for (auto& [id, pending] : pending_) {
            waiters.push_back(pending);
        }
        pending_.clear();
    }
    for (auto& pending : waiters) {
        const std::lock_guard lock(pending->mutex);
        pending->failed = true;
        pending->cv.notify_all();
    }
}

void BinanceWsClient::handle_user_event(const nlohmann::json& a_event)
{
    if (!a_event.is_object()) {
        emit(BinanceFeedEventType::ProtocolWarning, "user event is not an object");
        return;
    }
    const std::string type = a_event.value("e", std::string{});
    if (type != "executionReport") {
        // outboundAccountPosition / balanceUpdate / listStatus / ... are
        // legitimate events this gateway does not consume.
        return;
    }
    // "c" carries the clientOrderId of the CURRENT action: for cancels
    // (and cancelReplace's canceled leg) the venue puts its auto-generated
    // cancel id there and the ORIGINAL clientOrderId in "C" — prefer it so
    // reports stay keyed by the gateway's clientOrderId.
    std::string client_order_id = a_event.value("C", std::string{});
    if (client_order_id.empty()) {
        client_order_id = a_event.value("c", std::string{});
    }
    if (getenv("GATEWAY_BINANCE_DUMP_EVENTS") != nullptr) {
        std::cerr << "[binance-raw-event] " << a_event.dump() << '\n';
    }
    if (client_order_id.empty()) {
        emit(BinanceFeedEventType::ProtocolWarning, "executionReport without clientOrderId");
        return;
    }
    const std::string status = a_event.value("X", std::string{});
    const auto state = map_binance_state(status);
    if (!state.has_value()) {
        emit(BinanceFeedEventType::ProtocolWarning,
             "unknown Binance order status \"" + status + "\"; report skipped");
        return;
    }
    const auto side = map_binance_side(a_event.value("S", std::string{}));
    if (!side.has_value()) {
        emit(BinanceFeedEventType::ProtocolWarning, "unknown Binance order side \"" +
                                                        a_event.value("S", std::string{}) +
                                                        "\"; report skipped");
        return;
    }
    if (report_handler_) {
        report_handler_(ExecutionReport{
            .client_order_id = client_order_id,
            .exchange_order_id = std::to_string(a_event.value("i", 0LL)),
            .state = *state,
            .side = *side,
            .filled_quantity = a_event.value("z", std::string{}),
            .average_fill_price =
                average_price(a_event.value("Z", std::string{}), a_event.value("z", std::string{})),
        });
    }
}

auto BinanceWsClient::dispatch_message(const std::string& a_message) -> bool
{
    const auto parsed = nlohmann::json::parse(a_message, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        emit(BinanceFeedEventType::ProtocolWarning, "non-JSON text frame skipped");
        return false;
    }

    const auto event = parsed.find("event");
    if (event != parsed.end() && event->is_object()) {
        const std::string type = event->value("e", std::string{});
        if (type == "serverShutdown") {
            terminate_reason_ = "venue announced serverShutdown";
            return false;
        }
        if (type == "eventStreamTerminated") {
            terminate_reason_ = "venue terminated the user data stream";
            return false;
        }
        handle_user_event(*event);
        return false;
    }

    const auto id = parsed.find("id");
    if (id != parsed.end() && id->is_number_integer()) {
        const long long request_id = id->get<long long>();
        std::shared_ptr<Pending> pending;
        {
            const std::lock_guard lock(pending_mutex_);
            const auto it = pending_.find(request_id);
            if (it != pending_.end()) {
                pending = it->second;
                pending_.erase(it);
            }
        }
        if (pending) {
            const std::lock_guard lock(pending->mutex);
            pending->response = parsed;
            pending->cv.notify_all();
            return true;
        }
        // Unknown ids (late responses to requests that already timed out)
        // are dropped: the caller already moved on to resolution.
        return false;
    }

    emit(BinanceFeedEventType::ProtocolWarning, "unrecognized venue frame skipped: " + a_message);
    return false;
}

auto BinanceWsClient::run_session(std::stop_token a_stop) -> SessionOutcome
{
    const std::string url = (config_.use_tls ? "wss://" : "ws://") + config_.host + ":" +
                            std::to_string(config_.port) + config_.path;
    emit(BinanceFeedEventType::Connecting, url);
    terminate_reason_.clear();

    auto client = std::make_unique<httplib::ws::WebSocketClient>(url);
    client->set_connection_timeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(config_.request_timeout) / 4);
    // Protocol-level liveness: pings on this same connection; after
    // ws_max_missed_pongs unanswered ones the watchdog closes the silent
    // socket (half-open detection), which unblocks the read loop and runs
    // the normal reconnect + re-subscribe path. httplib's default (0
    // missed pongs) disables the check entirely, leaving a silent socket
    // undetected until the 300s OS read timeout.
    client->set_websocket_ping_interval(config_.ws_ping_interval_s);
    client->set_websocket_max_missed_pongs(config_.ws_max_missed_pongs);
    // Bound the blocked read: the watchdog flips the WebSocket state, but
    // a fully silent wire never delivers the bytes that would unblock
    // recv() (the close frame may be held by a dead path forever). A read
    // deadline slightly beyond the watchdog window guarantees the read
    // loop observes the death. Healthy links reset the deadline
    // continuously (venue pings, pongs to our pings, events), so only
    // true silence ever trips it.
    client->set_read_timeout(static_cast<time_t>(config_.ws_ping_interval_s) *
                                     (config_.ws_max_missed_pongs + 1) +
                                 config_.ws_ping_interval_s,
                             0);

    {
        const std::lock_guard lock(client_mutex_);
        client_ = std::move(client);
    }

    const SessionOutcome outcome = [this, &a_stop, &url]() -> SessionOutcome {
        const auto connected = client_->connect();
        if (!connected) {
            return SessionOutcome{.failure = "connect failed to " + url + ": error " +
                                                 std::to_string(static_cast<int>(
                                                     connected.error()))};
        }

        // ---- subscribe the account's User Data Stream on this session ----
        // Best-effort clock sync first: with a drifted local clock the
        // SIGNED subscribe below would be rejected -1021 on every
        // attempt, dead-looping reconnects. The learned offset is reused
        // by every subsequent signed call (and refreshed on -1021).
        (void)inline_time_sync();
        nlohmann::json params = nlohmann::json::object();
        apply_signature(params);
        const long long subscribe_id = [this] {
            const std::lock_guard lock(pending_mutex_);
            return next_id_++;
        }();
        auto subscribe_pending = std::make_shared<Pending>();
        {
            const std::lock_guard lock(pending_mutex_);
            pending_[subscribe_id] = subscribe_pending;
        }
        const nlohmann::json subscribe_frame{{"id", subscribe_id},
                                             {"method", "userDataStream.subscribe.signature"},
                                             {"params", params}};
        if (!client_->send(subscribe_frame.dump())) {
            const std::lock_guard lock(pending_mutex_);
            pending_.erase(subscribe_id);
            return SessionOutcome{.failure = "failed to send user data stream subscription"};
        }

        // ---- read loop: responses, events, and the subscribe outcome ----
        bool subscribed = false;
        bool served_after_subscribe = false;
        auto subscribed_at = std::chrono::steady_clock::now();
        std::string reason;
        while (!a_stop.stop_requested()) {
            std::string message;
            if (client_->read(message) != httplib::ws::ReadResult::Text) {
                reason = a_stop.stop_requested() ? std::string{} : std::string{"connection lost"};
                break;
            }
            const bool served_response = dispatch_message(message);
            if (!terminate_reason_.empty()) {
                reason = terminate_reason_;
                break;
            }

            if (!subscribed) {
                std::unique_lock lock(subscribe_pending->mutex);
                if (subscribe_pending->response.has_value()) {
                    const auto& response = *subscribe_pending->response;
                    const int status = response.value("status", 0);
                    if (status == 200) {
                        subscribed = true;
                        subscribed_at = std::chrono::steady_clock::now();
                        emit(BinanceFeedEventType::Connected,
                             "user data stream subscribed (subscriptionId " +
                                 std::to_string(
                                     response.value("result", nlohmann::json::object())
                                         .value("subscriptionId", 0LL)) +
                                 ")");
                    } else {
                        reason = "user data stream subscription rejected: " + response.dump();
                        break;
                    }
                } else if (subscribe_pending->failed) {
                    reason = "connection lost during user data stream subscription";
                    break;
                }
            } else if (served_response) {
                served_after_subscribe = true;
            }
        }
        {
            const std::lock_guard lock(pending_mutex_);
            pending_.erase(subscribe_id);
        }
        return SessionOutcome{
            .failure = reason,
            .healthy = subscribed &&
                       (served_after_subscribe ||
                        std::chrono::steady_clock::now() - subscribed_at >=
                            kHealthySessionSeconds)};
    }();

    {
        const std::lock_guard lock(client_mutex_);
        if (client_) {
            client_->close();
            client_.reset();
        }
    }
    fail_all_pending();
    return outcome;
}

void BinanceWsClient::run(std::stop_token a_stop)
{
    unsigned connect_attempt = 0;
    while (!a_stop.stop_requested()) {
        const SessionOutcome outcome = run_session(a_stop);
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
        emit(BinanceFeedEventType::Disconnected, outcome.failure);

        static thread_local std::mt19937_64 engine{std::random_device{}()};
        std::uniform_real_distribution<double> uniform{0.0, 1.0};
        const auto raw = backoff_for(config_.retry, static_cast<int>(connect_attempt));
        const auto delay = apply_jitter(config_.retry, raw, uniform(engine));
        interruptible_sleep(delay, a_stop);
    }
    emit(BinanceFeedEventType::Stopped, "feed stopped");
}

} // namespace gateway::exchange::binance
