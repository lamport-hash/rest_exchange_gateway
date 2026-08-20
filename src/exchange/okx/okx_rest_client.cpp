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

auto string_field(const nlohmann::json& a_node, const char* a_name) -> std::optional<std::string>
{
    const auto it = a_node.find(a_name);
    if (it != a_node.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return std::nullopt;
}

template <typename ClientT>
auto perform_request(const OkxConfig& a_config, std::string_view a_method,
                     const std::string& a_path, const httplib::Headers& a_headers,
                     const std::string& a_body) -> httplib::Result
{
    ClientT client(a_config.host, a_config.port);
    client.set_connection_timeout(5);
    client.set_read_timeout(5);
    if (a_method == "GET") {
        return client.Get(a_path, a_headers);
    }
    return client.Post(a_path, a_headers, a_body, "application/json");
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
    return config;
}

OkxRestClient::OkxRestClient(OkxConfig a_config, TimestampProvider a_timestamp)
    : config_(std::move(a_config)),
      timestamp_(a_timestamp ? std::move(a_timestamp) : TimestampProvider{&utc_now_iso_ms})
{}

auto OkxRestClient::signed_request(const char* a_method, const std::string& a_path,
                                   const std::string& a_body) const -> Result<nlohmann::json>
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
    if (result->status != 200) {
        return Error{"transport", "unexpected HTTP status " + std::to_string(result->status) +
                                      " from " + a_path};
    }

    const auto envelope = nlohmann::json::parse(result->body, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) {
        return Error{"protocol", "response is not a JSON object: " + result->body};
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
        return Error{"venue:" + *code, "OKX rejected " + std::string(a_path) + ": " + detail};
    }
    if (!envelope.contains("data") || !envelope.at("data").is_array()) {
        return Error{"protocol", "envelope missing \"data\" array"};
    }
    return envelope;
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

} // namespace gateway::exchange::okx
