#include "exchange/okx/okx_rest_client.hpp"

#include "core/clock.hpp"
#include "exchange/okx/okx_signer.hpp"

#include <httplib.h>

#include <utility>

namespace gateway::exchange::okx {

namespace {
constexpr const char* kPathPlace = "/api/v5/trade/order";
constexpr const char* kPathCancel = "/api/v5/trade/cancel-order";
constexpr const char* kPathAmend = "/api/v5/trade/amend-order";
constexpr const char* kPathOrderInfo = "/api/v5/trade/order";
constexpr const char* kPathOrdersPending = "/api/v5/trade/orders-pending";
constexpr const char* kPathTicker = "/api/v5/market/ticker";
constexpr const char* kPathDemoAdjustBalance = "/api/v5/account/demo-adjust-balance";

auto string_field(const nlohmann::json& a_node, const char* a_name) -> std::optional<std::string>
{
    const auto it = a_node.find(a_name);
    if (it != a_node.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return std::nullopt;
}

/// Validate the OKX envelope ({code, msg, data}); non-"0" codes become
/// "venue:<code>" errors. Shared by signed and public requests.
auto parse_envelope(const std::string& a_body, const std::string& a_path) -> Result<nlohmann::json>
{
    const auto envelope = nlohmann::json::parse(a_body, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) {
        return Error{"protocol", "response is not a JSON object: " + a_body};
    }

    const auto code = string_field(envelope, "code");
    if (!code.has_value()) {
        return Error{"protocol", "envelope missing string field \"code\""};
    }
    if (*code != "0") {
        const auto msg = string_field(envelope, "msg").value_or("");
        std::string detail = msg;
        if (envelope.contains("data") && envelope.at("data").is_array() &&
            !envelope.at("data").empty() && envelope.at("data").front().is_object()) {
            const auto& front = envelope.at("data").front();
            detail += " [" + string_field(front, "sCode").value_or("") + ": " +
                      string_field(front, "sMsg").value_or("") + "]";
        }
        return Error{"venue:" + *code, "OKX rejected " + a_path + ": " + detail};
    }
    if (!envelope.contains("data") || !envelope.at("data").is_array()) {
        return Error{"protocol", "envelope missing \"data\" array"};
    }
    return envelope;
}

template <typename ClientT>
auto perform_request(const OkxConfig& a_config, std::string_view a_method,
                     const std::string& a_path, const httplib::Headers& a_headers,
                     const std::string& a_body) -> httplib::Result
{
    ClientT client(a_config.host, a_config.port);
    client.set_connection_timeout(0, a_config.rest_connect_timeout_ms * 1000);
    client.set_read_timeout(0, a_config.rest_read_timeout_ms * 1000);
    if (a_method == "GET") {
        return client.Get(a_path, a_headers);
    }
    return client.Post(a_path, a_headers, a_body, "application/json");
}

auto ws_config_from_json(const nlohmann::json& a_node,
                         OkxWsConfig a_defaults) -> Result<OkxWsConfig>
{
    if (!a_node.is_object()) {
        return Error{"protocol", "okx ws config must be a JSON object"};
    }

    OkxWsConfig config = a_defaults;
    if (const auto enabled = a_node.find("enabled"); enabled != a_node.end()) {
        if (!enabled->is_boolean()) {
            return Error{"protocol", "okx ws enabled must be a boolean"};
        }
        config.enabled = enabled->get<bool>();
    }
    if (const auto host = string_field(a_node, "host")) {
        config.host = *host;
    }
    if (const auto port = a_node.find("port"); port != a_node.end()) {
        if (!port->is_number_unsigned() || port->get<std::uint64_t>() < 1 ||
            port->get<std::uint64_t>() > 65535) {
            return Error{"protocol", "okx ws port must be an unsigned number in [1, 65535]"};
        }
        config.port = static_cast<int>(port->get<std::uint64_t>());
    }
    if (const auto use_tls = a_node.find("useTls"); use_tls != a_node.end()) {
        if (!use_tls->is_boolean()) {
            return Error{"protocol", "okx ws useTls must be a boolean"};
        }
        config.use_tls = use_tls->get<bool>();
    }
    if (const auto path = string_field(a_node, "path")) {
        config.path = *path;
    }
    if (const auto interval = a_node.find("pingIntervalMs"); interval != a_node.end()) {
        if (!interval->is_number_unsigned() || interval->get<std::uint64_t>() < 1 ||
            interval->get<std::uint64_t>() > 600000) {
            return Error{"protocol", "okx ws pingIntervalMs must be in [1, 600000]"};
        }
        config.ping_interval =
            std::chrono::milliseconds{static_cast<long long>(interval->get<std::uint64_t>())};
    }
    if (const auto missed = a_node.find("maxMissedPongs"); missed != a_node.end()) {
        if (!missed->is_number_unsigned() || missed->get<std::uint64_t>() < 1 ||
            missed->get<std::uint64_t>() > 100) {
            return Error{"protocol", "okx ws maxMissedPongs must be in [1, 100]"};
        }
        config.max_missed_pongs = static_cast<int>(missed->get<std::uint64_t>());
    }
    return config;
}
} // namespace

auto okx_config_from_json(const nlohmann::json& a_section) -> Result<OkxConfig>
{
    if (!a_section.is_object()) {
        return Error{"protocol", "okx config section must be a JSON object"};
    }

    std::string missing;
    const auto require = [&a_section, &missing](const char* a_name) -> std::optional<std::string> {
        if (const auto value = string_field(a_section, a_name)) {
            return value;
        }
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += a_name;
        return std::nullopt;
    };

    OkxConfig config;
    const auto api_key = require("apiKey");
    const auto secret_key = require("secretKey");
    const auto passphrase = require("passphrase");
    if (!missing.empty()) {
        return Error{"protocol", "okx config missing or non-string field(s): " + missing};
    }
    config.api_key = std::move(*api_key);
    config.secret_key = std::move(*secret_key);
    config.passphrase = std::move(*passphrase);

    if (const auto host = string_field(a_section, "host")) {
        config.host = *host;
    }
    const auto port = a_section.find("port");
    if (port != a_section.end()) {
        if (!port->is_number_unsigned()) {
            return Error{"protocol", "okx port must be an unsigned number"};
        }
        config.port = static_cast<int>(port->get<std::uint64_t>());
    }
    const auto use_tls = a_section.find("useTls");
    if (use_tls != a_section.end()) {
        if (!use_tls->is_boolean()) {
            return Error{"protocol", "okx useTls must be a boolean"};
        }
        config.use_tls = use_tls->get<bool>();
    }
    const auto demo = a_section.find("demoTrading");
    if (demo != a_section.end()) {
        if (!demo->is_boolean()) {
            return Error{"protocol", "okx demoTrading must be a boolean"};
        }
        config.demo_trading = demo->get<bool>();
    }

    const auto timeouts = a_section.find("restConnectTimeoutMs");
    if (timeouts != a_section.end()) {
        if (!timeouts->is_number_unsigned() || timeouts->get<std::uint64_t>() < 1 ||
            timeouts->get<std::uint64_t>() > 600000) {
            return Error{"protocol", "okx restConnectTimeoutMs must be in [1, 600000]"};
        }
        config.rest_connect_timeout_ms = static_cast<int>(timeouts->get<std::uint64_t>());
    }
    const auto read_timeout = a_section.find("restReadTimeoutMs");
    if (read_timeout != a_section.end()) {
        if (!read_timeout->is_number_unsigned() || read_timeout->get<std::uint64_t>() < 1 ||
            read_timeout->get<std::uint64_t>() > 600000) {
            return Error{"protocol", "okx restReadTimeoutMs must be in [1, 600000]"};
        }
        config.rest_read_timeout_ms = static_cast<int>(read_timeout->get<std::uint64_t>());
    }

    if (const auto retry = a_section.find("retry"); retry != a_section.end()) {
        const auto policy = retry_policy_from_json(*retry, config.retry);
        if (!policy.is_ok()) {
            return policy.error();
        }
        config.retry = policy.value();
    }
    if (const auto ws = a_section.find("ws"); ws != a_section.end()) {
        const auto parsed = ws_config_from_json(*ws, config.ws);
        if (!parsed.is_ok()) {
            return parsed.error();
        }
        config.ws = parsed.value();
    }
    return config;
}

OkxRestClient::OkxRestClient(OkxConfig a_config, TimestampProvider a_timestamp)
    : config_(std::move(a_config)),
      timestamp_(a_timestamp ? std::move(a_timestamp) : TimestampProvider{&utc_now_iso_ms})
{}

auto OkxRestClient::signed_request_raw(const char* a_method, const std::string& a_path,
                                       const std::string& a_body) const
    -> Result<std::pair<int, std::string>>
{
    const std::string timestamp = timestamp_();

    httplib::Headers headers{
        {"OK-ACCESS-KEY", config_.api_key},
        {"OK-ACCESS-SIGN", sign_request(timestamp, a_method, a_path, a_body, config_.secret_key)},
        {"OK-ACCESS-TIMESTAMP", timestamp},
        {"OK-ACCESS-PASSPHRASE", config_.passphrase}};
    if (config_.demo_trading) {
        headers.emplace("x-simulated-trading", "1");
    }

    const auto result =
        config_.use_tls
            ? perform_request<httplib::SSLClient>(config_, a_method, a_path, headers, a_body)
            : perform_request<httplib::Client>(config_, a_method, a_path, headers, a_body);
    if (result == nullptr) {
        return Error{"transport", "network failure talking to " + config_.host};
    }
    return std::make_pair(result->status, result->body);
}

auto OkxRestClient::signed_request(const char* a_method, const std::string& a_path,
                                   const std::string& a_body) const -> Result<nlohmann::json>
{
    const auto raw = signed_request_raw(a_method, a_path, a_body);
    if (!raw.is_ok()) {
        return raw.error();
    }
    if (raw.value().first != 200) {
        return Error{"transport", "unexpected HTTP status " + std::to_string(raw.value().first) +
                                      " from " + a_path};
    }
    return parse_envelope(raw.value().second, a_path);
}
auto OkxRestClient::place_order(const OkxPlaceRequest& a_request) const -> Result<OkxOrderAck>
{
    const auto envelope = signed_request("POST", kPathPlace, to_json(a_request).dump());
    if (!envelope.is_ok()) {
        return envelope.error();
    }
    const auto& data = envelope.value().at("data");
    if (data.empty() || !data.front().is_object()) {
        return Error{"protocol", "place-order data array is empty"};
    }
    return parse_order_ack(data.front());
}

auto OkxRestClient::cancel_order(const OkxCxlRequest& a_request) const -> Result<OkxOrderAck>
{
    const auto envelope = signed_request("POST", kPathCancel, to_json(a_request).dump());
    if (!envelope.is_ok()) {
        return envelope.error();
    }
    const auto& data = envelope.value().at("data");
    if (data.empty() || !data.front().is_object()) {
        return Error{"protocol", "cancel-order data array is empty"};
    }
    return parse_order_ack(data.front());
}

auto OkxRestClient::amend_order(const OkxAmendRequest& a_request) const -> Result<OkxOrderAck>
{
    const auto body = to_json(a_request);
    if (!body.is_ok()) {
        return body.error();
    }
    const auto envelope = signed_request("POST", kPathAmend, body.value().dump());
    if (!envelope.is_ok()) {
        return envelope.error();
    }
    const auto& data = envelope.value().at("data");
    if (data.empty() || !data.front().is_object()) {
        return Error{"protocol", "amend-order data array is empty"};
    }
    return parse_order_ack(data.front());
}

auto OkxRestClient::get_order(const OkxQuery& a_query) const -> Result<std::optional<OkxOrderInfo>>
{
    const std::string path = std::string(kPathOrderInfo) + "?" + to_query(a_query);
    const auto envelope = signed_request("GET", path, "");
    if (!envelope.is_ok()) {
        return envelope.error();
    }
    const auto& data = envelope.value().at("data");
    if (data.empty()) {
        return std::optional<OkxOrderInfo>{std::nullopt};
    }
    if (!data.front().is_object()) {
        return Error{"protocol", "order-info data[0] is not an object"};
    }
    return std::optional<OkxOrderInfo>{parse_order_info(data.front())};
}

auto OkxRestClient::get_orders_pending() const -> Result<std::vector<OkxOrderInfo>>
{
    const auto envelope = signed_request("GET", kPathOrdersPending, "");
    if (!envelope.is_ok()) {
        return envelope.error();
    }
    const auto& data = envelope.value().at("data");
    if (!data.is_array()) {
        return Error{"protocol", "orders-pending data is not an array"};
    }
    std::vector<OkxOrderInfo> pending;
    pending.reserve(data.size());
    for (const auto& item : data) {
        if (!item.is_object()) {
            return Error{"protocol", "orders-pending data item is not an object"};
        }
        pending.push_back(parse_order_info(item));
    }
    return pending;
}

auto OkxRestClient::adjust_demo_balance(const OkxDemoBalanceRequest& a_request) const
    -> Result<nlohmann::json>
{
    // Unlike the trade endpoints, this venue reports business rejections
    // with a non-200 HTTP status (e.g. 400 + envelope code 51000/59693):
    // parse the envelope before declaring a transport failure so those
    // surface as "venue:<code>", not 502.
    const auto raw = signed_request_raw("POST", kPathDemoAdjustBalance, to_json(a_request).dump());
    if (!raw.is_ok()) {
        return raw.error();
    }
    const auto& [status, body] = raw.value();
    if (status != 200) {
        const auto envelope = parse_envelope(body, kPathDemoAdjustBalance);
        if (!envelope.is_ok() && envelope.error().code.rfind("venue:", 0) == 0) {
            return envelope.error();
        }
        return Error{"transport", "unexpected HTTP status " + std::to_string(status) + " from " +
                                      std::string{kPathDemoAdjustBalance}};
    }
    const auto envelope = parse_envelope(body, kPathDemoAdjustBalance);
    if (!envelope.is_ok()) {
        return envelope.error();
    }
    return envelope.value().at("data");
}

auto OkxRestClient::get_ticker(const std::string& a_instrument_id) const -> Result<std::string>
{
    // Public market-data endpoint: no OK-ACCESS-* headers, no signing.
    const std::string path = std::string(kPathTicker) + "?instId=" + a_instrument_id;
    const auto result = config_.use_tls
                            ? perform_request<httplib::SSLClient>(config_, "GET", path, {}, "")
                            : perform_request<httplib::Client>(config_, "GET", path, {}, "");
    if (result == nullptr) {
        return Error{"transport", "network failure talking to " + config_.host};
    }
    if (result->status != 200) {
        return Error{"transport",
                     "unexpected HTTP status " + std::to_string(result->status) + " from " + path};
    }
    const auto envelope = parse_envelope(result->body, kPathTicker);
    if (!envelope.is_ok()) {
        return envelope.error();
    }
    const auto& data = envelope.value().at("data");
    if (data.empty() || !data.front().is_object()) {
        return Error{"protocol", "ticker data[0] is missing or not an object"};
    }
    const auto last = string_field(data.front(), "last");
    if (!last.has_value()) {
        return Error{"protocol", "ticker data[0] missing string field \"last\""};
    }
    return *last;
}

} // namespace gateway::exchange::okx
