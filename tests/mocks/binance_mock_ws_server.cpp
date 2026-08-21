#include "mocks/binance_mock_ws_server.hpp"

#include "exchange/binance/binance_signer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <utility>

namespace gateway::testing {

namespace {

using exchange::binance::BinanceConfig;

constexpr long long kScaleFactor = 100000000; // 1e8, Binance-style decimals

auto string_field(const nlohmann::json& a_node, const char* a_name) -> std::string
{
    const auto it = a_node.find(a_name);
    return it != a_node.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

auto scaled(const std::string& a_decimal) -> long long
{
    const auto point = a_decimal.find('.');
    const long long whole =
        std::stoll(point == std::string::npos ? a_decimal : a_decimal.substr(0, point));
    long long fraction = 0;
    if (point != std::string::npos) {
        std::string digits = a_decimal.substr(point + 1);
        while (digits.size() < 8) {
            digits += '0';
        }
        digits = digits.substr(0, 8);
        fraction = std::stoll(digits);
    }
    return whole * kScaleFactor + fraction;
}

// Binance renders decimals with 8 fractional digits, trailing zeros kept.
auto format8(long long a_scaled) -> std::string
{
    const bool negative = a_scaled < 0;
    const long long magnitude = negative ? -a_scaled : a_scaled;
    std::string whole = std::to_string(magnitude / kScaleFactor);
    std::string fraction = std::to_string(magnitude % kScaleFactor);
    while (fraction.size() < 8) {
        fraction.insert(fraction.begin(), '0');
    }
    std::string text = whole + "." + fraction;
    return negative ? "-" + text : text;
}

// Wide intermediate for scaled products: 1e8 * 1e8 overflows long long.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
using WideMul = __int128;
#pragma GCC diagnostic pop

auto scaled_product(long long a_lhs, long long a_rhs) -> long long
{
    return static_cast<long long>(static_cast<WideMul>(a_lhs) * a_rhs / kScaleFactor);
}

auto now_ms() -> long long
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

auto error_body(int a_code, const std::string& a_msg) -> nlohmann::json
{
    return nlohmann::json{{"code", a_code}, {"msg", a_msg}};
}

auto error_response(const nlohmann::json& a_request, int a_status, int a_code,
                    const std::string& a_msg) -> std::string
{
    const nlohmann::json frame{{"id", a_request.value("id", 0)},
                               {"status", a_status},
                               {"error", error_body(a_code, a_msg)}};
    return frame.dump();
}

auto ok_response(const nlohmann::json& a_request, const nlohmann::json& a_result) -> std::string
{
    const nlohmann::json frame{
        {"id", a_request.value("id", 0)}, {"status", 200}, {"result", a_result}};
    return frame.dump();
}

} // namespace

BinanceMockWsServer::BinanceMockWsServer(BinanceConfig a_client_config, std::string a_path)
    : client_config_(std::move(a_client_config)), path_(std::move(a_path))
{}

BinanceMockWsServer::~BinanceMockWsServer()
{
    stop();
}

void BinanceMockWsServer::start()
{
    auto server = std::make_unique<httplib::Server>();
    register_routes(*server);
    const int bound = server->bind_to_any_port("127.0.0.1");
    if (bound < 0) {
        throw std::runtime_error("BinanceMockWsServer: failed to bind");
    }
    port_ = bound;
    server_ = std::move(server);
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
    running_ = true;
}

void BinanceMockWsServer::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    {
        const std::lock_guard lock(mutex_);
        for (Session* session : sessions_) {
            if (session->socket != nullptr) {
                session->socket->close(); // unblocks the handler read loops
            }
        }
    }
    server_->stop();
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    server_.reset();
}

void BinanceMockWsServer::restart_on_same_port()
{
    stop();
    auto server = std::make_unique<httplib::Server>();
    register_routes(*server);
    if (!server->bind_to_port("127.0.0.1", port_)) {
        throw std::runtime_error("BinanceMockWsServer: failed to rebind port " +
                                 std::to_string(port_));
    }
    server_ = std::move(server);
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
    running_ = true;
}

auto BinanceMockWsServer::port() const -> std::uint16_t
{
    return static_cast<std::uint16_t>(port_);
}

void BinanceMockWsServer::set_fill_mode(FillMode a_mode)
{
    const std::lock_guard lock(mutex_);
    fill_mode_ = a_mode;
}

void BinanceMockWsServer::apply_fill(const std::string& a_client_order_id, const std::string& a_qty,
                                     const std::string& a_px)
{
    const std::lock_guard lock(mutex_);
    const auto it = orders_.find(a_client_order_id);
    if (it == orders_.end()) {
        return;
    }
    MockOrder& order = it->second;
    const long long qty = scaled(a_qty);
    const long long px = scaled(a_px);
    order.executed_scaled = std::min(order.orig_scaled, order.executed_scaled + qty);
    order.quote_scaled += scaled_product(qty, px);
    order.status = order.executed_scaled >= order.orig_scaled ? "FILLED" : "PARTIALLY_FILLED";
}

void BinanceMockWsServer::set_drop_next_response()
{
    const std::lock_guard lock(mutex_);
    drop_next_response_ = true;
}

void BinanceMockWsServer::set_delay_next_response(unsigned a_ms)
{
    const std::lock_guard lock(mutex_);
    delay_next_ms_ = a_ms;
}

void BinanceMockWsServer::set_ignore_signature(bool a_ignore)
{
    const std::lock_guard lock(mutex_);
    ignore_signature_ = a_ignore;
}

void BinanceMockWsServer::set_drop_next_updates(int a_count)
{
    const std::lock_guard lock(mutex_);
    drop_next_updates_ = a_count;
}

void BinanceMockWsServer::set_duplicate_next_update()
{
    const std::lock_guard lock(mutex_);
    duplicate_next_update_ = true;
}

void BinanceMockWsServer::push_execution_report(const std::string& a_client_order_id)
{
    nlohmann::json payload;
    {
        const std::lock_guard lock(mutex_);
        const auto it = orders_.find(a_client_order_id);
        if (it == orders_.end()) {
            return;
        }
        const MockOrder& order = it->second;
        const auto execution_type = order.status == "NEW" ? "NEW" : "TRADE";
        payload = nlohmann::json{
            {"subscriptionId", 0},
            {"event",
             nlohmann::json{{"e", "executionReport"},
                            {"E", now_ms()},
                            {"s", order.symbol},
                            {"c", order.client_order_id},
                            {"S", order.side},
                            {"o", order.type},
                            {"f", order.time_in_force},
                            {"q", format8(order.orig_scaled)},
                            {"p", order.price},
                            {"x", execution_type},
                            {"X", order.status},
                            {"r", "NONE"},
                            {"i", order.order_id},
                            {"l", "0.00000000"},
                            {"z", format8(order.executed_scaled)},
                            {"L", "0.00000000"},
                            {"Z", format8(order.quote_scaled)},
                            {"w", order.status == "NEW" || order.status == "PARTIALLY_FILLED"},
                            {"T", now_ms()}}}};
    }
    const std::string frame = payload.dump();

    const std::lock_guard lock(mutex_);
    if (drop_next_updates_ > 0) {
        --drop_next_updates_;
        return;
    }
    for (Session* session : sessions_) {
        if (session->subscribed && session->socket != nullptr) {
            session->socket->send(frame);
            if (duplicate_next_update_) {
                session->socket->send(frame);
            }
        }
    }
    duplicate_next_update_ = false;
}

void BinanceMockWsServer::push_raw_frame(const std::string& a_frame)
{
    const std::lock_guard lock(mutex_);
    for (Session* session : sessions_) {
        if (session->subscribed && session->socket != nullptr) {
            session->socket->send(a_frame);
        }
    }
}

void BinanceMockWsServer::kill_connections()
{
    const std::lock_guard lock(mutex_);
    for (Session* session : sessions_) {
        if (session->socket != nullptr) {
            session->socket->close(httplib::ws::CloseStatus::Normal, "mock kill");
        }
    }
}

auto BinanceMockWsServer::stats() const -> Stats
{
    const std::lock_guard lock(mutex_);
    return stats_;
}

auto BinanceMockWsServer::wait_for_subscriber(int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this] { return stats_.any_subscribed; });
}

auto BinanceMockWsServer::wait_for_places(int a_count, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this, a_count] { return stats_.places >= a_count; });
}

auto BinanceMockWsServer::wait_for_amends(int a_count, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this, a_count] { return stats_.amends >= a_count; });
}

auto BinanceMockWsServer::wait_for_cancels(int a_count, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this, a_count] { return stats_.cancels >= a_count; });
}

auto BinanceMockWsServer::wait_for_connections(int a_count, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this, a_count] { return stats_.connections >= a_count; });
}

auto BinanceMockWsServer::wait_for_text(const std::string& a_needle, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms}, [this, &a_needle] {
        return std::any_of(stats_.received.begin(), stats_.received.end(),
                           [&a_needle](const std::string& a_frame) {
                               return a_frame.find(a_needle) != std::string::npos;
                           });
    });
}

auto BinanceMockWsServer::check_signed(const nlohmann::json& a_params)
    -> std::unique_ptr<std::string>
{
    if (ignore_signature_) {
        return nullptr;
    }
    const std::string api_key = string_field(a_params, "apiKey");
    if (api_key != client_config_.api_key) {
        return std::make_unique<std::string>("-2014:API-key format invalid (mock).");
    }
    nlohmann::json unsigned_params = a_params;
    unsigned_params.erase("signature");
    const std::string expected =
        exchange::binance::sign_params(unsigned_params, client_config_.secret_key);
    if (string_field(a_params, "signature") != expected) {
        return std::make_unique<std::string>("-1022:Signature for this request is not valid.");
    }
    const long long timestamp =
        a_params.contains("timestamp") && a_params["timestamp"].is_number_integer()
            ? a_params["timestamp"].get<long long>()
            : 0;
    const long long recv_window =
        a_params.contains("recvWindow") && a_params["recvWindow"].is_number_integer()
            ? a_params["recvWindow"].get<long long>()
            : 5000;
    if (std::llabs(now_ms() - timestamp) > recv_window) {
        return std::make_unique<std::string>(
            "-1021:Timestamp for this request was outside of the recvWindow.");
    }
    return nullptr;
}

auto BinanceMockWsServer::order_status_payload(const MockOrder& a_order) const -> nlohmann::json
{
    const long long time = now_ms();
    return nlohmann::json{
        {"symbol", a_order.symbol},
        {"orderId", a_order.order_id},
        {"orderListId", -1},
        {"clientOrderId", a_order.client_order_id},
        {"price", a_order.price},
        {"origQty", format8(a_order.orig_scaled)},
        {"executedQty", format8(a_order.executed_scaled)},
        {"cummulativeQuoteQty", format8(a_order.quote_scaled)},
        {"status", a_order.status},
        {"timeInForce", a_order.time_in_force},
        {"type", a_order.type},
        {"side", a_order.side},
        {"stopPrice", "0.00000000"},
        {"icebergQty", "0.00000000"},
        {"time", time},
        {"updateTime", time},
        {"isWorking", a_order.status == "NEW" || a_order.status == "PARTIALLY_FILLED"},
        {"workingTime", time},
        {"origQuoteOrderQty", "0.00000000"},
        {"selfTradePreventionMode", "NONE"}};
}

auto BinanceMockWsServer::order_result_payload(const MockOrder& a_order) const -> nlohmann::json
{
    return nlohmann::json{{"symbol", a_order.symbol},
                          {"orderId", a_order.order_id},
                          {"orderListId", -1},
                          {"clientOrderId", a_order.client_order_id},
                          {"transactTime", now_ms()},
                          {"price", a_order.price},
                          {"origQty", format8(a_order.orig_scaled)},
                          {"executedQty", format8(a_order.executed_scaled)},
                          {"origQuoteOrderQty", "0.00000000"},
                          {"cummulativeQuoteQty", format8(a_order.quote_scaled)},
                          {"status", a_order.status},
                          {"timeInForce", a_order.time_in_force},
                          {"type", a_order.type},
                          {"side", a_order.side},
                          {"workingTime", now_ms()},
                          {"selfTradePreventionMode", "NONE"}};
}

auto BinanceMockWsServer::find_order(const std::string& a_client_order_id) -> MockOrder*
{
    const auto it = orders_.find(a_client_order_id);
    return it == orders_.end() ? nullptr : &it->second;
}

auto BinanceMockWsServer::next_order_id() -> long long
{
    return ++order_counter_;
}

void BinanceMockWsServer::fill_if_mode(MockOrder& a_order)
{
    if (fill_mode_ != FillMode::Full) {
        return;
    }
    const long long remaining = a_order.orig_scaled - a_order.executed_scaled;
    const long long price = a_order.price.empty() ? 0 : scaled(a_order.price);
    a_order.executed_scaled += remaining;
    a_order.quote_scaled += scaled_product(remaining, price);
    a_order.status = "FILLED";
}

auto BinanceMockWsServer::handle_place(const nlohmann::json& a_params) -> nlohmann::json
{
    ++stats_.places;
    MockOrder order;
    order.client_order_id = string_field(a_params, "newClientOrderId");
    order.symbol = string_field(a_params, "symbol");
    order.side = string_field(a_params, "side");
    order.type = string_field(a_params, "type");
    order.price = string_field(a_params, "price");
    order.time_in_force = string_field(a_params, "timeInForce").empty()
                              ? std::string{"GTC"}
                              : string_field(a_params, "timeInForce");
    const std::string quantity = string_field(a_params, "quantity");

    if (order.client_order_id.empty() || order.symbol.empty() || quantity.empty() ||
        (order.type == "LIMIT" && order.price.empty())) {
        return error_body(-1102, "Mandatory parameter missing (mock validation).");
    }
    if (const MockOrder* existing = find_order(order.client_order_id);
        existing != nullptr && existing->status != "FILLED" && existing->status != "CANCELED") {
        // like the venue: a duplicate clientOrderId is rejected while the
        // previous order is still open
        return error_body(-4116, "ClientOrderId is duplicated.");
    }

    order.order_id = next_order_id();
    order.orig_scaled = scaled(quantity);
    order.status = "NEW";
    fill_if_mode(order);
    const nlohmann::json result = order_result_payload(order);
    orders_[order.client_order_id] = order;
    return result;
}

auto BinanceMockWsServer::handle_cancel(const nlohmann::json& a_params) -> nlohmann::json
{
    ++stats_.cancels;
    MockOrder* order = find_order(string_field(a_params, "origClientOrderId"));
    if (order == nullptr || order->status == "FILLED" || order->status == "CANCELED") {
        return error_body(-2011, "Unknown order sent.");
    }
    order->status = "CANCELED";
    return order_status_payload(*order);
}

auto BinanceMockWsServer::handle_cancel_replace(const nlohmann::json& a_params) -> nlohmann::json
{
    ++stats_.amends;
    const std::string target = string_field(a_params, "cancelOrigClientOrderId");
    MockOrder* order = find_order(target);
    if (order == nullptr || order->status == "FILLED" || order->status == "CANCELED") {
        return error_body(-2011, "Unknown order sent.");
    }

    order->status = "CANCELED";
    const nlohmann::json canceled = order_status_payload(*order);

    MockOrder replacement;
    replacement.client_order_id = string_field(a_params, "newClientOrderId");
    if (replacement.client_order_id.empty()) {
        replacement.client_order_id = target;
    }
    replacement.symbol = string_field(a_params, "symbol");
    replacement.side = string_field(a_params, "side");
    replacement.type = string_field(a_params, "type");
    replacement.price = string_field(a_params, "price");
    replacement.time_in_force = string_field(a_params, "timeInForce").empty()
                                    ? std::string{"GTC"}
                                    : string_field(a_params, "timeInForce");
    replacement.orig_scaled = scaled(string_field(a_params, "quantity"));
    replacement.order_id = next_order_id();
    replacement.status = "NEW";
    fill_if_mode(replacement);
    const nlohmann::json placed = order_result_payload(replacement);
    orders_[replacement.client_order_id] = replacement;

    return nlohmann::json{{"cancelResult", "SUCCESS"},
                          {"newOrderResult", "SUCCESS"},
                          {"cancelResponse", canceled},
                          {"newOrderResponse", placed}};
}

auto BinanceMockWsServer::handle_order_status(const nlohmann::json& a_params) -> nlohmann::json
{
    ++stats_.status_queries;
    const MockOrder* order = find_order(string_field(a_params, "origClientOrderId"));
    if (order == nullptr) {
        return error_body(-2013, "Order does not exist.");
    }
    return order_status_payload(*order);
}

auto BinanceMockWsServer::handle_open_orders(const nlohmann::json& /*a_params*/) -> nlohmann::json
{
    ++stats_.open_orders_queries;
    nlohmann::json open = nlohmann::json::array();
    for (const auto& [id, order] : orders_) {
        if (order.status == "NEW" || order.status == "PARTIALLY_FILLED") {
            open.push_back(order_status_payload(order));
        }
    }
    return open;
}

void BinanceMockWsServer::handle_frame(httplib::ws::WebSocket& a_ws, Session& a_session,
                                       const nlohmann::json& a_request)
{
    const std::string method = string_field(a_request, "method");
    const auto params_it = a_request.find("params");
    const nlohmann::json params = params_it != a_request.end() && params_it->is_object()
                                      ? *params_it
                                      : nlohmann::json::object();

    bool drop_response = false;
    unsigned delay_ms = 0;
    {
        const std::lock_guard lock(mutex_);
        drop_response = drop_next_response_;
        drop_next_response_ = false;
        delay_ms = delay_next_ms_;
        delay_next_ms_ = 0;
    }

    // userDataStream.subscribe.signature is SIGNED too (apiKey/timestamp/
    // signature params), so it runs through the same auth check.
    if (auto auth_error = check_signed(params)) {
        const auto separator = auth_error->find(':');
        const int code = std::stoi(auth_error->substr(0, separator));
        const std::string msg = auth_error->substr(separator + 1);
        {
            const std::lock_guard lock(mutex_);
            ++stats_.signature_failures;
            cv_.notify_all();
        }
        a_ws.send(error_response(a_request, 400, code, msg));
        return;
    }

    std::string reply;
    if (method == "userDataStream.subscribe.signature") {
        const std::lock_guard lock(mutex_);
        a_session.subscribed = true;
        stats_.any_subscribed = true;
        ++stats_.subscribes;
        cv_.notify_all();
        reply = ok_response(a_request, nlohmann::json{{"subscriptionId", 0}});
    } else if (method == "order.place") {
        const std::lock_guard lock(mutex_);
        const auto result = handle_place(params);
        reply = result.contains("code") ? error_response(a_request, 400, result["code"].get<int>(),
                                                         result["msg"].get<std::string>())
                                        : ok_response(a_request, result);
    } else if (method == "order.cancel") {
        const std::lock_guard lock(mutex_);
        const auto result = handle_cancel(params);
        reply = result.contains("code") ? error_response(a_request, 400, result["code"].get<int>(),
                                                         result["msg"].get<std::string>())
                                        : ok_response(a_request, result);
    } else if (method == "order.cancelReplace") {
        const std::lock_guard lock(mutex_);
        const auto result = handle_cancel_replace(params);
        reply = result.contains("code") ? error_response(a_request, 400, result["code"].get<int>(),
                                                         result["msg"].get<std::string>())
                                        : ok_response(a_request, result);
    } else if (method == "order.status") {
        const std::lock_guard lock(mutex_);
        const auto result = handle_order_status(params);
        reply = result.contains("code") ? error_response(a_request, 400, result["code"].get<int>(),
                                                         result["msg"].get<std::string>())
                                        : ok_response(a_request, result);
    } else if (method == "openOrders.status") {
        const std::lock_guard lock(mutex_);
        reply = ok_response(a_request, handle_open_orders(params));
    } else {
        reply = error_response(a_request, 400, -1003,
                               "Invalid method: " + (method.empty() ? "(none)" : method));
    }

    if (drop_response) {
        const std::lock_guard lock(mutex_);
        ++stats_.responses_dropped;
        cv_.notify_all();
        return; // the outcome happened; the acknowledgement is lost
    }
    if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{delay_ms});
    }
    a_ws.send(reply);
}

void BinanceMockWsServer::register_routes(httplib::Server& a_server)
{
    a_server.WebSocket(path_, [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
        Session session{.socket = &ws, .subscribed = false};
        {
            const std::lock_guard lock(mutex_);
            sessions_.push_back(&session);
            ++stats_.connections;
            cv_.notify_all();
        }

        while (true) {
            std::string text;
            if (ws.read(text) != httplib::ws::ReadResult::Text) {
                break;
            }
            {
                const std::lock_guard lock(mutex_);
                stats_.received.push_back(text);
                cv_.notify_all();
            }
            const auto request = nlohmann::json::parse(text, nullptr, false);
            if (request.is_discarded() || !request.is_object()) {
                continue;
            }
            handle_frame(ws, session, request);
        }

        const std::lock_guard lock(mutex_);
        const auto it = std::find(sessions_.begin(), sessions_.end(), &session);
        if (it != sessions_.end()) {
            sessions_.erase(it);
        }
        cv_.notify_all();
    });
}

} // namespace gateway::testing
