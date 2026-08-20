#include "rest/order_routes.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <string_view>

namespace gateway::rest {

namespace {
auto json_response(int a_status, const nlohmann::json& a_body) -> crow::response
{
    crow::response res{a_status, a_body.dump()};
    res.set_header("Content-Type", "application/json");
    return res;
}

auto error_response(int a_status, std::string_view a_code, std::string_view a_reason,
                    std::string_view a_client_order_id) -> crow::response
{
    const nlohmann::json body = {
        {"error", {{"code", a_code}, {"reason", a_reason}, {"clientOrderId", a_client_order_id}}}};
    return json_response(a_status, body);
}

auto internal_error_response(std::string_view a_client_order_id = "") -> crow::response
{
    return error_response(500, "internal", "unexpected server error", a_client_order_id);
}

auto is_decimal(std::string_view a_text) -> bool
{
    if (a_text.empty() || a_text.front() == '.') {
        return false;
    }
    bool dot_seen = false;
    for (const char c : a_text) {
        if (c == '.') {
            if (dot_seen) {
                return false;
            }
            dot_seen = true;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return a_text.back() != '.';
}

auto parse_side(std::string_view a_text) -> std::optional<Side>
{
    if (a_text == "buy") {
        return Side::Buy;
    }
    if (a_text == "sell") {
        return Side::Sell;
    }
    return std::nullopt;
}

auto parse_order_type(std::string_view a_text) -> std::optional<OrderType>
{
    if (a_text == "limit") {
        return OrderType::Limit;
    }
    if (a_text == "market") {
        return OrderType::Market;
    }
    return std::nullopt;
}

auto map_connector_error(const Error& a_error, std::string_view a_client_order_id) -> crow::response
{
    if (a_error.code == "venue:51016") {
        return error_response(404, "not_found", "order does not exist on the venue",
                              a_client_order_id);
    }
    if (a_error.code == "transport") {
        return error_response(502, "venue_unavailable", "cannot reach the venue",
                              a_client_order_id);
    }
    if (a_error.code.rfind("venue:", 0) == 0) {
        return error_response(409, "venue_rejected", a_error.message, a_client_order_id);
    }
    return error_response(500, "internal", a_error.message, a_client_order_id);
}

auto instrument_id_from_query(const crow::request& a_req) -> std::optional<std::string>
{
    const char* value = a_req.url_params.get("instrumentId");
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
}
} // namespace

void register_order_routes(crow::SimpleApp& a_app, ExchangeConnector& a_connector)
{
    a_app.exception_handler([](crow::response& a_res) { a_res = internal_error_response(); });

    CROW_ROUTE(a_app, "/orders")
        .methods(
            crow::HTTPMethod::POST)([&a_connector](const crow::request& a_req) -> crow::response {
            try {
                const auto body = nlohmann::json::parse(a_req.body, nullptr, false);
                if (body.is_discarded() || !body.is_object()) {
                    return error_response(400, "invalid_request",
                                          "request body must be a JSON object", "");
                }

                const auto string_field = [&body](const char* a_name) -> std::string {
                    const auto it = body.find(a_name);
                    return it != body.end() && it->is_string() ? it->get<std::string>()
                                                               : std::string{};
                };

                OrderRequest request;
                request.client_order_id = string_field("clientOrderId");
                request.instrument_id = string_field("instrumentId");
                request.price = string_field("price");
                request.quantity = string_field("quantity");

                if (request.client_order_id.empty()) {
                    return error_response(400, "invalid_request", "clientOrderId is required", "");
                }
                if (request.instrument_id.empty()) {
                    return error_response(400, "invalid_request", "instrumentId is required",
                                          request.client_order_id);
                }
                const auto side = parse_side(string_field("side"));
                if (!side.has_value()) {
                    return error_response(400, "invalid_request", "side must be buy or sell",
                                          request.client_order_id);
                }
                request.side = *side;
                const auto type = parse_order_type(string_field("type"));
                if (!type.has_value()) {
                    return error_response(400, "invalid_request", "type must be limit or market",
                                          request.client_order_id);
                }
                request.type = *type;
                if (request.quantity.empty() || !is_decimal(request.quantity)) {
                    return error_response(400, "invalid_request",
                                          "quantity must be a positive decimal",
                                          request.client_order_id);
                }
                if (!request.price.empty() && !is_decimal(request.price)) {
                    return error_response(400, "invalid_request",
                                          "price must be a positive decimal",
                                          request.client_order_id);
                }
                if (request.type == OrderType::Limit && request.price.empty()) {
                    return error_response(400, "invalid_request",
                                          "price is required for limit orders",
                                          request.client_order_id);
                }
                if (request.type == OrderType::Market && !request.price.empty()) {
                    return error_response(400, "invalid_request",
                                          "price is not allowed for market orders",
                                          request.client_order_id);
                }

                const auto placement = a_connector.place_order(request);
                if (!placement.is_ok()) {
                    return map_connector_error(placement.error(), request.client_order_id);
                }
                const nlohmann::json response = {
                    {"clientOrderId", placement.value().client_order_id},
                    {"exchangeOrderId", placement.value().exchange_order_id}};
                return json_response(201, response);
            } catch (...) {
                return internal_error_response();
            }
        });

    CROW_ROUTE(a_app, "/orders/<string>")
        .methods(crow::HTTPMethod::GET)(
            [&a_connector](const crow::request& a_req,
                           const std::string& a_client_order_id) -> crow::response {
                try {
                    const auto instrument_id = instrument_id_from_query(a_req);
                    if (!instrument_id.has_value()) {
                        return error_response(400, "invalid_request",
                                              "instrumentId query parameter is required",
                                              a_client_order_id);
                    }

                    const auto snapshot =
                        a_connector.get_order(OrderQuery{a_client_order_id, *instrument_id});
                    if (!snapshot.is_ok()) {
                        return map_connector_error(snapshot.error(), a_client_order_id);
                    }
                    if (!snapshot.value().has_value()) {
                        return error_response(404, "not_found", "order does not exist on the venue",
                                              a_client_order_id);
                    }

                    const auto& order = *snapshot.value();
                    const nlohmann::json response = {
                        {"clientOrderId", order.client_order_id},
                        {"exchangeOrderId", order.exchange_order_id},
                        {"state", to_string(order.state)},
                        {"price", order.price},
                        {"quantity", order.quantity},
                        {"filledQuantity", order.filled_quantity},
                        {"averageFillPrice", order.average_fill_price}};
                    return json_response(200, response);
                } catch (...) {
                    return internal_error_response(a_client_order_id);
                }
            });

    CROW_ROUTE(a_app, "/orders/<string>")
        .methods(crow::HTTPMethod::DELETE)(
            [&a_connector](const crow::request& a_req,
                           const std::string& a_client_order_id) -> crow::response {
                try {
                    const auto instrument_id = instrument_id_from_query(a_req);
                    if (!instrument_id.has_value()) {
                        return error_response(400, "invalid_request",
                                              "instrumentId query parameter is required",
                                              a_client_order_id);
                    }

                    const auto cancel =
                        a_connector.cancel_order(CancelRequest{a_client_order_id, *instrument_id});
                    if (!cancel.is_ok()) {
                        return map_connector_error(cancel.error(), a_client_order_id);
                    }
                    const nlohmann::json response = {
                        {"clientOrderId", cancel.value().client_order_id},
                        {"exchangeOrderId", cancel.value().exchange_order_id}};
                    return json_response(200, response);
                } catch (...) {
                    return internal_error_response(a_client_order_id);
                }
            });
}

} // namespace gateway::rest
