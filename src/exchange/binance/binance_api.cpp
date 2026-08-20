#include "exchange/binance/binance_resilient.hpp"

namespace gateway::exchange::binance {

auto BinanceApi::invoke(const std::string& a_method,
                        const nlohmann::json& a_params) -> Result<nlohmann::json>
{
    // Signing happens inside the raw call (BinanceWsClient::call_signed),
    // which owns the credentials and the clock.
    return call_(a_method, a_params);
}

auto BinanceApi::place(const BinancePlaceRequest& a_request) -> Result<BinanceOrderAck>
{
    const auto params = build_place_params(a_request);
    if (!params.is_ok()) {
        return params.error();
    }
    const auto result = invoke("order.place", params.value());
    if (!result.is_ok()) {
        return result.error();
    }
    return parse_order_ack(result.value());
}

auto BinanceApi::cancel(const BinanceCancelRequest& a_request) -> Result<BinanceOrderInfo>
{
    const auto params = build_cancel_params(a_request);
    if (!params.is_ok()) {
        return params.error();
    }
    const auto result = invoke("order.cancel", params.value());
    if (!result.is_ok()) {
        return result.error();
    }
    auto info_result = parse_order_info(result.value());
    if (!info_result.is_ok()) {
        return info_result.error();
    }
    auto info = info_result.value();
    // order.cancel reports the canceled order under origClientOrderId;
    // "clientOrderId" carries the (auto-generated) cancelNewClientOrderId.
    info.client_order_id = a_request.client_order_id;
    return info;
}

auto BinanceApi::cancel_replace(const BinanceAmendRequest& a_request)
    -> Result<BinanceReplaceResult>
{
    const auto params = build_cancel_replace_params(a_request);
    if (!params.is_ok()) {
        return params.error();
    }
    const auto result = invoke("order.cancelReplace", params.value());
    if (!result.is_ok()) {
        return result.error();
    }
    auto replace_result = parse_replace_result(result.value());
    if (!replace_result.is_ok()) {
        return replace_result.error();
    }
    auto replace = replace_result.value();
    replace.canceled.client_order_id = a_request.client_order_id;
    return replace;
}

auto BinanceApi::get_order(const BinanceOrderQuery& a_query)
    -> Result<std::optional<BinanceOrderInfo>>
{
    const auto result = invoke("order.status", build_order_status_params(a_query));
    if (!result.is_ok()) {
        if (result.error().code == "venue:-2013") {
            return Result<std::optional<BinanceOrderInfo>>{
                std::optional<BinanceOrderInfo>{std::nullopt}};
        }
        return result.error();
    }
    auto info = parse_order_info(result.value());
    if (!info.is_ok()) {
        return info.error();
    }
    return Result<std::optional<BinanceOrderInfo>>{
        std::optional<BinanceOrderInfo>{std::move(info.value())}};
}

auto BinanceApi::get_open_orders() -> Result<std::vector<BinanceOrderInfo>>
{
    const auto result = invoke("openOrders.status", build_open_orders_params());
    if (!result.is_ok()) {
        return result.error();
    }
    if (!result.value().is_array()) {
        return Error{"protocol", "openOrders.status result is not an array"};
    }
    std::vector<BinanceOrderInfo> orders;
    orders.reserve(result.value().size());
    for (const auto& item : result.value()) {
        auto info = parse_order_info(item);
        if (info.is_ok()) {
            orders.push_back(std::move(info.value()));
        }
        // Items that cannot be parsed are skipped: reconciliation is best
        // effort and must not fail because of one exotic venue order.
    }
    return orders;
}

} // namespace gateway::exchange::binance
