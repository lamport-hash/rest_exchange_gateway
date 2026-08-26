#include "exchange/okx/okx_ws_client.hpp"

#include "core/clock.hpp"
#include "exchange/okx/okx_signer.hpp"
#include "exchange/okx/okx_wire.hpp"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

namespace gateway::exchange::okx {

namespace {

auto now_ms() -> long long
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Type-safe string field access: the value when a_name exists and is a
/// string, empty otherwise. Unlike json::value(key, default) this never
/// throws on a type-confused field (e.g. "code": 0) — a throw would
/// escape the supervisor jthread and terminate the process.
auto checked_string(const nlohmann::json& a_node, const char* a_name) -> std::string
{
    const auto it = a_node.find(a_name);
    return it != a_node.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

/// Sender-side keepalive: every ping_interval send the text "ping" (OKX
/// replies "pong"); if no inbound message at all arrived for more than
/// (max_missed_pongs + 1) * ping_interval the connection is declared dead
/// and closed, which unblocks the reader and triggers a reconnect.
void run_pinger(httplib::ws::WebSocketClient& a_client, const OkxWsConfig& a_config,
                std::stop_token a_stop, const std::atomic<long long>& a_last_inbound_ms,
                std::atomic<bool>& a_watchdog_fired)
{
    const long long patience_ms =
        (a_config.ping_interval.count()) * (a_config.max_missed_pongs + 1);
    while (!a_stop.stop_requested()) {
        interruptible_sleep(a_config.ping_interval, a_stop);
        if (a_stop.stop_requested()) {
            return;
        }
        if (now_ms() - a_last_inbound_ms.load(std::memory_order_relaxed) > patience_ms) {
            a_watchdog_fired.store(true, std::memory_order_relaxed);
            a_client.close(httplib::ws::CloseStatus::GoingAway, "watchdog: no inbound traffic");
            return;
        }
        if (!a_client.send("ping")) {
            return; // reader observes the failure and ends the session
        }
    }
}

} // namespace

auto ws_host_for(const OkxConfig& a_config) -> std::string
{
    if (a_config.demo_trading && a_config.ws.host == "ws.okx.com") {
        return "wspap.okx.com";
    }
    return a_config.ws.host;
}

OkxOrdersFeed::OkxOrdersFeed(OkxConfig a_config, OkxRestClient::TimestampProvider a_timestamp)
    : config_(std::move(a_config)),
      timestamp_(a_timestamp ? std::move(a_timestamp)
                             : OkxRestClient::TimestampProvider{&utc_now_iso_ms})
{}

OkxOrdersFeed::~OkxOrdersFeed()
{
    stop();
}

void OkxOrdersFeed::set_report_handler(ReportHandler a_handler)
{
    report_handler_ = std::move(a_handler);
}

void OkxOrdersFeed::set_event_handler(EventHandler a_handler)
{
    event_handler_ = std::move(a_handler);
}

void OkxOrdersFeed::emit(FeedEventType a_type, std::string a_detail) const
{
    if (event_handler_) {
        event_handler_(FeedEvent{a_type, std::move(a_detail)});
    }
}

void OkxOrdersFeed::start()
{
    if (!config_.ws.enabled) {
        return;
    }
    const bool already = running_.exchange(true);
    if (already) {
        return;
    }
    supervisor_ = std::jthread([this](std::stop_token stop) { run(std::move(stop)); });
}

void OkxOrdersFeed::stop()
{
    if (supervisor_.joinable()) {
        supervisor_.request_stop();
        {
            const std::lock_guard lock(session_mutex_);
            if (close_active_session_) {
                close_active_session_();
            }
        }
        supervisor_.join();
    }
    running_ = false;
}

auto OkxOrdersFeed::is_running() const -> bool
{
    return running_;
}

void OkxOrdersFeed::dispatch_orders_message(const nlohmann::json& a_message)
{
    const auto arg = a_message.find("arg");
    const auto data = a_message.find("data");
    if (arg == a_message.end() || !arg->is_object() || data == a_message.end() ||
        !data->is_array()) {
        emit(FeedEventType::ProtocolWarning, "malformed orders message: " + a_message.dump());
        return;
    }

    for (const auto& item : *data) {
        if (!item.is_object()) {
            emit(FeedEventType::ProtocolWarning, "orders data item is not an object");
            continue;
        }
        const OkxOrderInfo info = parse_order_info(item);
        const auto state = map_okx_state(info.state);
        if (!state.has_value()) {
            emit(FeedEventType::ProtocolWarning,
                 "unknown OKX order state \"" + info.state + "\"; item skipped");
            continue;
        }
        if (info.cl_ord_id.empty()) {
            emit(FeedEventType::ProtocolWarning, "orders item without clOrdId; skipped");
            continue;
        }
        const auto side = map_okx_side(info.side);
        if (!side.has_value()) {
            emit(FeedEventType::ProtocolWarning,
                 "unknown OKX order side \"" + info.side + "\"; item skipped");
            continue;
        }
        if (report_handler_) {
            report_handler_(ExecutionReport{.client_order_id = info.cl_ord_id,
                                            .exchange_order_id = info.ord_id,
                                            .state = *state,
                                            .side = *side,
                                            .filled_quantity = info.acc_fill_sz,
                                            .average_fill_price = info.avg_px});
        }
    }
}

auto OkxOrdersFeed::run_session(std::stop_token a_stop) -> SessionOutcome
{
    // Demo-trading credentials only authenticate against the demo WS host;
    // the production host rejects them with 50101.
    const std::string host = ws_host_for(config_);
    const std::string url = (config_.ws.use_tls ? "wss://" : "ws://") + host + ":" +
                            std::to_string(config_.ws.port) + config_.ws.path;
    emit(FeedEventType::Connecting, url);

    httplib::Headers headers;
    if (config_.demo_trading) {
        headers.emplace("x-simulated-trading", "1");
    }
    httplib::ws::WebSocketClient client(url, headers);
    client.set_connection_timeout(0, config_.rest_connect_timeout_ms * 1000);
    // Protocol-level heartbeat disabled: OKX keepalive is the application-
    // level text "ping"/"pong" handled by run_pinger.
    client.set_websocket_ping_interval(0);

    {
        const std::lock_guard lock(session_mutex_);
        close_active_session_ = [&client] { client.close(); };
    }

    const SessionOutcome outcome = [this, &client, &a_stop, &url]() -> SessionOutcome {
        const auto connected = client.connect();
        if (!connected) {
            return SessionOutcome{.failure = "connect failed to " + url + ": error " +
                                                 std::to_string(static_cast<int>(
                                                     connected.error()))};
        }

        // login (epoch seconds.millis timestamp — the WS login rejects
        // ISO 8601 with error 60004)
        const std::string timestamp = gateway::utc_now_epoch_ms();
        const nlohmann::json login = {
            {"op", "login"},
            {"args",
             nlohmann::json::array({{{"apiKey", config_.api_key},
                                     {"passphrase", config_.passphrase},
                                     {"timestamp", timestamp},
                                     {"sign", sign_ws_login(timestamp, config_.secret_key)}}})}};
        if (!client.send(login.dump())) {
            return SessionOutcome{.failure = "failed to send login request"};
        }
        std::string reply;
        if (client.read(reply) != httplib::ws::ReadResult::Text) {
            return SessionOutcome{.failure = "connection closed waiting for login ack"};
        }
        const auto login_ack = nlohmann::json::parse(reply, nullptr, false);
        if (!login_ack.is_object() || checked_string(login_ack, "event") != "login" ||
            checked_string(login_ack, "code") != "0") {
            return SessionOutcome{.failure = "login rejected: " + reply};
        }

        // subscribe to the orders channel. Live OKX rejects a bare
        // {"channel":"orders"} with 60018: the private orders channel
        // requires instType (the gateway trades SPOT / tdMode cash).
        const nlohmann::json subscribe = {
            {"op", "subscribe"},
            {"args", nlohmann::json::array({{{"channel", "orders"}, {"instType", "SPOT"}}})}};
        if (!client.send(subscribe.dump())) {
            return SessionOutcome{.failure = "failed to send subscribe request"};
        }
        std::string sub_reply;
        if (client.read(sub_reply) != httplib::ws::ReadResult::Text) {
            return SessionOutcome{.failure = "connection closed waiting for subscribe ack"};
        }
        const auto sub_ack = nlohmann::json::parse(sub_reply, nullptr, false);
        if (!sub_ack.is_object() || checked_string(sub_ack, "event") != "subscribe") {
            return SessionOutcome{.failure = "subscribe rejected: " + sub_reply};
        }

        emit(FeedEventType::Connected, "orders channel subscribed");
        const auto subscribed_at = std::chrono::steady_clock::now();
        bool inbound_after_subscribe = false;

        // ---- reader + keepalive ----
        std::atomic<long long> last_inbound_ms{now_ms()};
        std::atomic<bool> watchdog_fired{false};
        std::jthread pinger{[&client, this, &a_stop, &last_inbound_ms,
                             &watchdog_fired](std::stop_token ping_stop) {
            run_pinger(client, config_.ws, std::move(ping_stop), last_inbound_ms, watchdog_fired);
        }};

        std::string reason;
        while (!a_stop.stop_requested()) {
            std::string message;
            const auto result = client.read(message);
            if (result != httplib::ws::ReadResult::Text) {
                reason = a_stop.stop_requested()
                             ? std::string{}
                             : (watchdog_fired.load(std::memory_order_relaxed)
                                    ? std::string{"watchdog closed a silent connection"}
                                    : std::string{"connection lost"});
                break;
            }
            last_inbound_ms.store(now_ms(), std::memory_order_relaxed);
            inbound_after_subscribe = true; // any frame (pong included) proves life

            if (message == "pong") {
                continue;
            }
            if (message == "ping") {
                client.send("pong");
                continue;
            }

            const auto parsed = nlohmann::json::parse(message, nullptr, false);
            if (parsed.is_discarded()) {
                emit(FeedEventType::ProtocolWarning, "non-JSON text frame skipped");
                continue;
            }
            const auto event = parsed.find("event");
            if (event != parsed.end() && event->is_string()) {
                if (*event == "error") {
                    emit(FeedEventType::ProtocolWarning, "venue error event: " + message);
                }
                // late login/subscribe acks and pongs carried as events are
                // informational; nothing to do.
                continue;
            }
            dispatch_orders_message(parsed);
        }

        pinger.request_stop();
        client.close();
        pinger.join();
        return SessionOutcome{
            .failure = reason,
            .healthy = inbound_after_subscribe ||
                       std::chrono::steady_clock::now() - subscribed_at >=
                           kHealthySessionSeconds};
    }();

    {
        const std::lock_guard lock(session_mutex_);
        close_active_session_ = nullptr;
    }
    return outcome;
}

void OkxOrdersFeed::run(std::stop_token a_stop)
{
    reconnect_loop(
        a_stop, config_.retry, [this, a_stop] { return run_session(a_stop); },
        [this](const std::string& a_failure) { emit(FeedEventType::Disconnected, a_failure); });
    emit(FeedEventType::Stopped, "feed stopped");
}

} // namespace gateway::exchange::okx
