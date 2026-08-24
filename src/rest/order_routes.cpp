#include "rest/order_routes.hpp"

#include "rest/route_support.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <string>
#include <string_view>

namespace gateway::rest {

namespace {

constexpr std::size_t kMaxClientOrderIdLength = 32;

auto to_lower(std::string_view a_text) -> std::string
{
    std::string lower{a_text};
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
    return lower;
}

auto is_valid_client_order_id(std::string_view a_id) -> bool
{
    if (a_id.empty() || a_id.size() > kMaxClientOrderIdLength) {
        return false;
    }
    for (const char c : a_id) {
        const bool alnum =
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alnum) {
            return false;
        }
    }
    return true;
}

auto parse_side(std::string_view a_text) -> std::optional<Side>
{
    const auto lower = to_lower(a_text);
    if (lower == "buy") {
        return Side::Buy;
    }
    if (lower == "sell") {
        return Side::Sell;
    }
    return std::nullopt;
}

auto parse_order_type(std::string_view a_text) -> std::optional<OrderType>
{
    const auto lower = to_lower(a_text);
    if (lower == "limit") {
        return OrderType::Limit;
    }
    if (lower == "market") {
        return OrderType::Market;
    }
    return std::nullopt;
}

auto parse_time_in_force(std::string_view a_text) -> std::optional<std::string>
{
    auto upper = to_lower(a_text);
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (upper == "GTC" || upper == "IOC" || upper == "FOK") {
        return upper;
    }
    return std::nullopt;
}

auto side_to_string(Side a_side) -> std::string_view
{
    return a_side == Side::Buy ? "buy" : "sell";
}

auto type_to_string(OrderType a_type) -> std::string_view
{
    return a_type == OrderType::Limit ? "limit" : "market";
}

auto record_json(const OrderRecord& a_record) -> nlohmann::json
{
    nlohmann::json body = {{"clientOrderId", a_record.client_order_id},
                           {"exchangeOrderId", a_record.exchange_order_id},
                           {"symbol", a_record.symbol},
                           {"venue", a_record.venue},
                           {"side", side_to_string(a_record.side)},
                           {"type", type_to_string(a_record.type)},
                           {"timeInForce", a_record.time_in_force},
                           {"state", to_string(a_record.state)},
                           {"price", a_record.price},
                           {"quantity", a_record.quantity},
                           {"filledQuantity", a_record.filled_quantity},
                           {"averageFillPrice", a_record.average_fill_price}};
    if (a_record.rejection.has_value()) {
        // state "rejected": the recorded reason replayed to retries
        // (risk_* code or venue rejection).
        body["rejection"] = {{"code", a_record.rejection->code},
                             {"reason", a_record.rejection->message}};
    }
    return body;
}

} // namespace

void register_order_routes(crow::SimpleApp& a_app, OrderManagementSystem& a_oms)
{
    a_app.exception_handler([](crow::response& a_res) { a_res = internal_error_response(); });

    // ---- /orders: POST place, GET registry listing ----------------------
    // One rule with both methods (Crow's trie rejects a duplicate URL) —
    // the handler dispatches on the request method.
    CROW_ROUTE(a_app, "/orders")
        .methods(crow::HTTPMethod::POST,
                 crow::HTTPMethod::GET)([&a_oms](const crow::request& a_req) -> crow::response {
            try {
                if (a_req.method == crow::HTTPMethod::GET) {
                    nlohmann::json orders = nlohmann::json::array();
                    for (const auto& record : a_oms.all_orders()) {
                        orders.push_back(record_json(record));
                    }
                    return json_response(200, {{"orders", std::move(orders)}});
                }

                crow::response parse_error;
                const auto body = parse_json_body(a_req, parse_error);
                if (!body.has_value()) {
                    return parse_error;
                }

                if (const auto unknown =
                        unknown_fields(*body, {"clientOrderId", "venue", "symbol", "side", "type",
                                               "price", "quantity", "timeInForce"});
                    !unknown.empty()) {
                    return error_response(
                        400, "invalid_request",
                        "unsupported field(s): " + unknown +
                            " (exchange-specific fields are rejected; use the common schema)",
                        string_field(*body, "clientOrderId"));
                }

                OrderRequest request;
                request.client_order_id = string_field(*body, "clientOrderId");
                if (request.client_order_id.empty()) {
                    return error_response(400, "invalid_request", "clientOrderId is required", "");
                }
                if (!is_valid_client_order_id(request.client_order_id)) {
                    return error_response(400, "invalid_request",
                                          "clientOrderId must be 1-32 alphanumeric characters",
                                          request.client_order_id);
                }
                std::string venue; // empty -> OMS default venue
                if (const auto venue_field = optional_string_field(*body, "venue")) {
                    venue = to_lower(*venue_field);
                    const auto supported = a_oms.venues();
                    if (std::find(supported.begin(), supported.end(), venue) == supported.end()) {
                        std::string names;
                        for (const auto& candidate : supported) {
                            if (!names.empty()) {
                                names += ", ";
                            }
                            for (const char c : candidate) {
                                names +=
                                    static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            }
                        }
                        return error_response(400, "invalid_request",
                                              "unsupported venue \"" + *venue_field +
                                                  "\" (supported: " + names + ")",
                                              request.client_order_id);
                    }
                }
                request.instrument_id = string_field(*body, "symbol");
                if (request.instrument_id.empty()) {
                    return error_response(400, "invalid_request", "symbol is required",
                                          request.client_order_id);
                }
                const auto side = parse_side(string_field(*body, "side"));
                if (!side.has_value()) {
                    return error_response(400, "invalid_request", "side must be buy or sell",
                                          request.client_order_id);
                }
                request.side = *side;
                const auto type = parse_order_type(string_field(*body, "type"));
                if (!type.has_value()) {
                    return error_response(400, "invalid_request", "type must be limit or market",
                                          request.client_order_id);
                }
                request.type = *type;
                if (const auto tif = optional_string_field(*body, "timeInForce")) {
                    const auto normalized = parse_time_in_force(*tif);
                    if (!normalized.has_value()) {
                        return error_response(400, "invalid_request",
                                              "timeInForce must be GTC, IOC or FOK",
                                              request.client_order_id);
                    }
                    if (request.type == OrderType::Market) {
                        return error_response(400, "invalid_request",
                                              "timeInForce only applies to limit orders",
                                              request.client_order_id);
                    }
                    request.time_in_force = *normalized;
                }
                request.price = string_field(*body, "price");
                request.quantity = string_field(*body, "quantity");
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

                const auto outcome = a_oms.place(request, venue);
                if (!outcome.is_ok()) {
                    return map_error(outcome.error(), request.client_order_id);
                }
                nlohmann::json response = {
                    {"clientOrderId", outcome.value().record.client_order_id},
                    {"exchangeOrderId", outcome.value().record.exchange_order_id},
                    {"symbol", outcome.value().record.symbol},
                    {"venue", outcome.value().record.venue},
                    {"state", to_string(outcome.value().record.state)},
                    {"replayed", outcome.value().replayed}};
                // Still unacked (a duplicate of an in-flight place, or a
                // replay of a transport-unresolved one): 202 Accepted —
                // the venue has not acknowledged the order, there is no
                // exchangeOrderId yet. Once acked, replays return 201
                // with the recorded record.
                const int status = outcome.value().record.state == OrderState::Pending ? 202 : 201;
                return json_response(status, response);
            } catch (...) {
                return internal_error_response();
            }
        });

    // ---- GET /orders/{clientOrderId}: registry view ---------------------
    CROW_ROUTE(a_app, "/orders/<string>")
        .methods(crow::HTTPMethod::GET)(
            [&a_oms](const crow::request&, const std::string& a_client_order_id) -> crow::response {
                try {
                    const auto record = a_oms.query(a_client_order_id);
                    if (!record.is_ok()) {
                        return map_error(record.error(), a_client_order_id);
                    }
                    return json_response(200, record_json(record.value()));
                } catch (...) {
                    return internal_error_response(a_client_order_id);
                }
            });

    // ---- DELETE /orders/{clientOrderId}: cancel -------------------------
    CROW_ROUTE(a_app, "/orders/<string>")
        .methods(crow::HTTPMethod::DELETE)(
            [&a_oms](const crow::request&, const std::string& a_client_order_id) -> crow::response {
                try {
                    const auto record = a_oms.cancel(a_client_order_id);
                    if (!record.is_ok()) {
                        return map_error(record.error(), a_client_order_id);
                    }
                    const nlohmann::json response = {
                        {"clientOrderId", record.value().client_order_id},
                        {"exchangeOrderId", record.value().exchange_order_id},
                        {"symbol", record.value().symbol},
                        {"venue", record.value().venue},
                        {"state", to_string(record.value().state)}};
                    return json_response(200, response);
                } catch (...) {
                    return internal_error_response(a_client_order_id);
                }
            });

    // ---- PUT /orders/{clientOrderId}: amend -----------------------------
    CROW_ROUTE(a_app, "/orders/<string>")
        .methods(crow::HTTPMethod::PUT)(
            [&a_oms](const crow::request& a_req,
                     const std::string& a_client_order_id) -> crow::response {
                try {
                    crow::response parse_error;
                    const auto body = parse_json_body(a_req, parse_error);
                    if (!body.has_value()) {
                        return parse_error;
                    }
                    if (const auto unknown = unknown_fields(*body, {"price", "quantity"});
                        !unknown.empty()) {
                        return error_response(400, "invalid_request",
                                              "unsupported field(s): " + unknown +
                                                  " (amend accepts price and/or quantity)",
                                              a_client_order_id);
                    }

                    AmendCommand command;
                    command.client_order_id = a_client_order_id;
                    command.new_price = optional_string_field(*body, "price");
                    command.new_quantity = optional_string_field(*body, "quantity");
                    if (!command.new_price.has_value() && !command.new_quantity.has_value()) {
                        return error_response(400, "invalid_request",
                                              "amend requires price and/or quantity",
                                              a_client_order_id);
                    }
                    if (command.new_price.has_value() &&
                        (command.new_price->empty() || !is_decimal(*command.new_price))) {
                        return error_response(400, "invalid_request",
                                              "price must be a positive decimal",
                                              a_client_order_id);
                    }
                    if (command.new_quantity.has_value() &&
                        (command.new_quantity->empty() || !is_decimal(*command.new_quantity))) {
                        return error_response(400, "invalid_request",
                                              "quantity must be a positive decimal",
                                              a_client_order_id);
                    }

                    const auto record = a_oms.amend(command);
                    if (!record.is_ok()) {
                        return map_error(record.error(), a_client_order_id);
                    }
                    const nlohmann::json response = {
                        {"clientOrderId", record.value().client_order_id},
                        {"exchangeOrderId", record.value().exchange_order_id},
                        {"symbol", record.value().symbol},
                        {"state", to_string(record.value().state)},
                        {"price", record.value().price},
                        {"quantity", record.value().quantity}};
                    return json_response(200, response);
                } catch (...) {
                    return internal_error_response(a_client_order_id);
                }
            });

    // ---- GET /health -----------------------------------------------------
    CROW_ROUTE(a_app, "/health")
    ([&a_oms]() -> crow::response {
        const auto stats = a_oms.stats();
        const nlohmann::json body = {{"status", "ok"},
                                     {"knownOrders", stats.known_orders},
                                     {"reportsApplied", stats.reports_applied},
                                     {"reportsStale", stats.reports_stale}};
        return json_response(200, body);
    });

    // ---- GET /price/{symbol}?venue=... ----------------------------------
    // Public market data passthrough: last-traded price of a pair. The
    // optional venue query parameter routes (default venue when absent,
    // like POST /orders). Errors follow the shared error envelope.
    CROW_ROUTE(a_app, "/price/<string>")
    ([&a_oms](const crow::request& a_req, const std::string& a_symbol) -> crow::response {
        try {
            std::string venue;
            if (const char* venue_param = a_req.url_params.get("venue")) {
                venue = to_lower(venue_param);
            }
            const auto quote = a_oms.get_price(a_symbol, venue);
            if (!quote.is_ok()) {
                return map_error(quote.error(), "");
            }
            const nlohmann::json body = {{"symbol", a_symbol},
                                         {"venue", quote.value().venue},
                                         {"price", quote.value().price}};
            return json_response(200, body);
        } catch (...) {
            return internal_error_response();
        }
    });
}

} // namespace gateway::rest
