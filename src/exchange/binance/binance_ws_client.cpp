#include "exchange/binance/binance_ws_client.hpp"

#include "core/decimal.hpp"
#include "core/retry.hpp"
#include "exchange/binance/binance_wire.hpp"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <random>
#include <utility>

namespace gateway::exchange::binance {

namespace {

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
    event_handler_ = std::move(a_handler);
}

void BinanceWsClient::emit(BinanceFeedEventType a_type, std::string a_detail) const
{
    if (event_handler_) {
        event_handler_(BinanceFeedEvent{a_type, std::move(a_detail)});
    }
}

void BinanceWsClient::start()
{
    const bool already = running_.exchange(true);
    if (already) {
        return;
    }
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
    return Error{"venue:" + code, message};
}

auto BinanceWsClient::call_signed(const std::string& a_method,
                                  const nlohmann::json& a_params) -> Result<nlohmann::json>
{
    nlohmann::json signed_params = a_params;
    signed_params["apiKey"] = config_.api_key;
    signed_params["recvWindow"] = config_.recv_window_ms;
    signed_params["timestamp"] = timestamp_();
    signed_params["signature"] = sign_params(signed_params, config_.secret_key);
    return call(a_method, signed_params);
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
    const std::string client_order_id = a_event.value("c", std::string{});
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

void BinanceWsClient::dispatch_message(const std::string& a_message)
{
    const auto parsed = nlohmann::json::parse(a_message, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        emit(BinanceFeedEventType::ProtocolWarning, "non-JSON text frame skipped");
        return;
    }

    const auto event = parsed.find("event");
    if (event != parsed.end() && event->is_object()) {
        const std::string type = event->value("e", std::string{});
        if (type == "serverShutdown") {
            terminate_reason_ = "venue announced serverShutdown";
            return;
        }
        if (type == "eventStreamTerminated") {
            terminate_reason_ = "venue terminated the user data stream";
            return;
        }
        handle_user_event(*event);
        return;
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
        }
        // Unknown ids (late responses to requests that already timed out)
        // are dropped: the caller already moved on to resolution.
        return;
    }

    emit(BinanceFeedEventType::ProtocolWarning, "unrecognized venue frame skipped: " + a_message);
}

auto BinanceWsClient::run_session(std::stop_token a_stop) -> std::string
{
    const std::string url = (config_.use_tls ? "wss://" : "ws://") + config_.host + ":" +
                            std::to_string(config_.port) + config_.path;
    emit(BinanceFeedEventType::Connecting, url);
    terminate_reason_.clear();

    auto client = std::make_unique<httplib::ws::WebSocketClient>(url);
    client->set_connection_timeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(config_.request_timeout) / 4);
    // Keep the WS-layer heartbeat: Binance answers protocol pongs and the
    // server's own pings are auto-answered by the read loop.

    {
        const std::lock_guard lock(client_mutex_);
        client_ = std::move(client);
    }

    const std::string failure = [this, &a_stop, &url]() -> std::string {
        const auto connected = client_->connect();
        if (!connected) {
            return "connect failed to " + url + ": error " +
                   std::to_string(static_cast<int>(connected.error()));
        }

        // ---- subscribe the account's User Data Stream on this session ----
        nlohmann::json params{{"apiKey", config_.api_key}, {"timestamp", timestamp_()}};
        params["signature"] = sign_params(params, config_.secret_key);
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
            return "failed to send user data stream subscription";
        }

        // ---- read loop: responses, events, and the subscribe outcome ----
        bool subscribed = false;
        std::string reason;
        while (!a_stop.stop_requested()) {
            std::string message;
            if (client_->read(message) != httplib::ws::ReadResult::Text) {
                reason = a_stop.stop_requested() ? std::string{} : std::string{"connection lost"};
                break;
            }
            dispatch_message(message);
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
                        emit(BinanceFeedEventType::Connected,
                             "user data stream subscribed (subscriptionId " +
                                 std::to_string(response.value("result", nlohmann::json::object())
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
            }
        }
        {
            const std::lock_guard lock(pending_mutex_);
            pending_.erase(subscribe_id);
        }
        return reason;
    }();

    {
        const std::lock_guard lock(client_mutex_);
        if (client_) {
            client_->close();
            client_.reset();
        }
    }
    fail_all_pending();
    return failure;
}

void BinanceWsClient::run(std::stop_token a_stop)
{
    unsigned connect_attempt = 0;
    while (!a_stop.stop_requested()) {
        const std::string failure = run_session(a_stop);
        if (a_stop.stop_requested()) {
            break;
        }
        ++connect_attempt;
        emit(BinanceFeedEventType::Disconnected, failure);

        static thread_local std::mt19937_64 engine{std::random_device{}()};
        std::uniform_real_distribution<double> uniform{0.0, 1.0};
        const auto raw = backoff_for(config_.retry, static_cast<int>(connect_attempt));
        const auto delay = apply_jitter(config_.retry, raw, uniform(engine));
        interruptible_sleep(delay, a_stop);
    }
    emit(BinanceFeedEventType::Stopped, "feed stopped");
}

} // namespace gateway::exchange::binance
