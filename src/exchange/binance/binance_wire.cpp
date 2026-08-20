#include "exchange/binance/binance_wire.hpp"

#include <array>
#include <utility>

namespace gateway::exchange::binance {

namespace {

auto side_text(Side a_side) -> const char*
{
    return a_side == Side::Buy ? "BUY" : "SELL";
}

auto type_text(OrderType a_type) -> const char*
{
    return a_type == OrderType::Limit ? "LIMIT" : "MARKET";
}

auto string_field(const nlohmann::json& a_node, const char* a_name) -> std::string
{
    const auto it = a_node.find(a_name);
    if (it != a_node.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return {};
}

auto number_field(const nlohmann::json& a_node, const char* a_name) -> std::string
{
    const auto it = a_node.find(a_name);
    if (it == a_node.end()) {
        return {};
    }
    if (it->is_number_integer()) {
        return std::to_string(it->get<long long>());
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return {};
}

auto require_decimal(const char* a_what, const std::string& a_value) -> std::optional<Error>
{
    if (a_value.empty()) {
        return Error{"protocol", std::string{a_what} + " is required"};
    }
    return std::nullopt;
}

} // namespace

auto build_place_params(const BinancePlaceRequest& a_request) -> Result<nlohmann::json>
{
    if (a_request.client_order_id.empty() || a_request.symbol.empty()) {
        return Error{"protocol", "place requires clientOrderId and symbol"};
    }
    if (const auto error = require_decimal("quantity", a_request.quantity)) {
        return *error;
    }
    if (a_request.type == OrderType::Limit) {
        if (const auto error = require_decimal("price", a_request.price)) {
            return *error;
        }
        if (a_request.time_in_force.empty()) {
            return Error{"protocol", "LIMIT orders require timeInForce"};
        }
    }

    nlohmann::json params{{"symbol", a_request.symbol},
                          {"side", side_text(a_request.side)},
                          {"type", type_text(a_request.type)},
                          {"quantity", a_request.quantity},
                          {"newClientOrderId", a_request.client_order_id},
                          {"newOrderRespType", "RESULT"}};
    if (a_request.type == OrderType::Limit) {
        params["price"] = a_request.price;
        params["timeInForce"] = a_request.time_in_force;
    }
    return params;
}

auto build_cancel_params(const BinanceCancelRequest& a_request) -> Result<nlohmann::json>
{
    if (a_request.client_order_id.empty() || a_request.symbol.empty()) {
        return Error{"protocol", "cancel requires clientOrderId and symbol"};
    }
    return nlohmann::json{{"symbol", a_request.symbol},
                          {"origClientOrderId", a_request.client_order_id}};
}

auto build_cancel_replace_params(const BinanceAmendRequest& a_request) -> Result<nlohmann::json>
{
    if (a_request.client_order_id.empty() || a_request.symbol.empty()) {
        return Error{"protocol", "amend requires clientOrderId and symbol"};
    }
    if (const auto error = require_decimal("quantity", a_request.quantity)) {
        return *error;
    }
    if (a_request.type == OrderType::Limit) {
        if (const auto error = require_decimal("price", a_request.price)) {
            return *error;
        }
        if (a_request.time_in_force.empty()) {
            return Error{"protocol", "LIMIT orders require timeInForce"};
        }
    }

    nlohmann::json params{{"symbol", a_request.symbol},
                          {"cancelReplaceMode", "STOP_ON_FAILURE"},
                          {"cancelOrigClientOrderId", a_request.client_order_id},
                          {"side", side_text(a_request.side)},
                          {"type", type_text(a_request.type)},
                          {"quantity", a_request.quantity},
                          {"newClientOrderId", a_request.client_order_id},
                          {"newOrderRespType", "RESULT"}};
    if (a_request.type == OrderType::Limit) {
        params["price"] = a_request.price;
        params["timeInForce"] = a_request.time_in_force;
    }
    return params;
}

auto build_order_status_params(const BinanceOrderQuery& a_query) -> nlohmann::json
{
    return nlohmann::json{{"symbol", a_query.symbol},
                          {"origClientOrderId", a_query.client_order_id}};
}

auto build_open_orders_params() -> nlohmann::json
{
    return nlohmann::json::object();
}

auto parse_order_ack(const nlohmann::json& a_result) -> BinanceOrderAck
{
    return BinanceOrderAck{.order_id = number_field(a_result, "orderId"),
                           .client_order_id = string_field(a_result, "clientOrderId"),
                           .status = string_field(a_result, "status"),
                           .executed_qty = string_field(a_result, "executedQty")};
}

auto parse_order_info(const nlohmann::json& a_result) -> Result<BinanceOrderInfo>
{
    if (!a_result.is_object()) {
        return Error{"protocol", "order result is not a JSON object"};
    }
    return BinanceOrderInfo{.order_id = number_field(a_result, "orderId"),
                            .client_order_id = string_field(a_result, "clientOrderId"),
                            .symbol = string_field(a_result, "symbol"),
                            .status = string_field(a_result, "status"),
                            .side = string_field(a_result, "side"),
                            .type = string_field(a_result, "type"),
                            .price = string_field(a_result, "price"),
                            .orig_qty = string_field(a_result, "origQty"),
                            .executed_qty = string_field(a_result, "executedQty"),
                            .cummulative_quote_qty = string_field(a_result, "cummulativeQuoteQty")};
}

auto parse_replace_result(const nlohmann::json& a_result) -> Result<BinanceReplaceResult>
{
    if (!a_result.is_object()) {
        return Error{"protocol", "cancelReplace result is not a JSON object"};
    }
    const auto cancel = a_result.find("cancelResponse");
    const auto placed = a_result.find("newOrderResponse");
    if (cancel == a_result.end() || !cancel->is_object() || placed == a_result.end() ||
        !placed->is_object()) {
        return Error{"protocol", "cancelReplace result lacks cancel/new order legs"};
    }
    auto canceled = parse_order_info(*cancel);
    if (!canceled.is_ok()) {
        return canceled.error();
    }
    return BinanceReplaceResult{.cancel_result = string_field(a_result, "cancelResult"),
                                .new_order_result = string_field(a_result, "newOrderResult"),
                                .canceled = std::move(canceled.value()),
                                .replacement = parse_order_ack(*placed)};
}

auto map_binance_state(std::string_view a_status) -> std::optional<OrderState>
{
    if (a_status == "NEW" || a_status == "PENDING_NEW") {
        return OrderState::Live;
    }
    if (a_status == "PARTIALLY_FILLED") {
        return OrderState::PartiallyFilled;
    }
    if (a_status == "FILLED") {
        return OrderState::Filled;
    }
    if (a_status == "CANCELED" || a_status == "PENDING_CANCEL" || a_status == "EXPIRED" ||
        a_status == "EXPIRED_IN_MATCH") {
        return OrderState::Canceled;
    }
    if (a_status == "REJECTED") {
        return OrderState::Rejected;
    }
    return std::nullopt;
}

auto map_binance_side(std::string_view a_side) -> std::optional<Side>
{
    if (a_side == "BUY") {
        return Side::Buy;
    }
    if (a_side == "SELL") {
        return Side::Sell;
    }
    return std::nullopt;
}

auto map_binance_type(std::string_view a_type) -> std::optional<OrderType>
{
    if (a_type == "LIMIT") {
        return OrderType::Limit;
    }
    if (a_type == "MARKET") {
        return OrderType::Market;
    }
    return std::nullopt;
}

namespace {

/// Quote assets recognized by the reverse translation, longest-first.
constexpr std::array<std::string_view, 12> kQuoteAssets{
    "USDT", "FDUSD", "USDC", "TUSD", "BUSD", "DAI", "BTC", "ETH", "BNB", "EUR", "TRY", "GBP"};

} // namespace

auto SymbolTranslator::to_wire(const std::string& a_gateway_symbol) -> std::string
{
    std::string wire;
    wire.reserve(a_gateway_symbol.size());
    for (const char c : a_gateway_symbol) {
        if (c != '-') {
            wire += c;
        }
    }
    gateway_to_wire_[a_gateway_symbol] = wire;
    wire_to_gateway_[wire] = a_gateway_symbol;
    return wire;
}

auto SymbolTranslator::to_gateway(std::string_view a_wire_symbol) -> std::string
{
    const std::string wire{a_wire_symbol};
    if (const auto it = wire_to_gateway_.find(wire); it != wire_to_gateway_.end()) {
        return it->second;
    }
    for (const std::string_view quote : kQuoteAssets) {
        if (wire.size() > quote.size() + 1 &&
            wire.compare(wire.size() - quote.size(), quote.size(), quote) == 0) {
            std::string gateway = wire.substr(0, wire.size() - quote.size());
            gateway += '-';
            gateway += quote;
            wire_to_gateway_[wire] = gateway;
            gateway_to_wire_[gateway] = wire;
            return gateway;
        }
    }
    wire_to_gateway_[wire] = wire;
    gateway_to_wire_[wire] = wire;
    return wire;
}

} // namespace gateway::exchange::binance
