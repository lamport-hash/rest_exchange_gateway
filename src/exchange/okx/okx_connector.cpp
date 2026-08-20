#include "exchange/okx/okx_connector.hpp"

#include <utility>

namespace gateway::exchange::okx {

namespace {
auto ack_to_placement(const OkxOrderAck& a_ack) -> Result<OrderPlacement>
{
    if (!a_ack.s_code.empty() && a_ack.s_code != "0") {
        return Error{"venue:" + a_ack.s_code, "OKX rejected request: " + a_ack.s_msg};
    }
    return OrderPlacement{.client_order_id = a_ack.cl_ord_id, .exchange_order_id = a_ack.ord_id};
}

auto to_wire(const OrderRequest& a_request) -> OkxPlaceRequest
{
    return OkxPlaceRequest{.cl_ord_id = a_request.client_order_id,
                           .inst_id = a_request.instrument_id,
                           .side = a_request.side == Side::Buy ? "buy" : "sell",
                           .ord_type = a_request.type == OrderType::Limit ? "limit" : "market",
                           .px = a_request.price,
                           .sz = a_request.quantity};
}

auto to_snapshot(const OkxOrderInfo& a_info) -> Result<OrderSnapshot>
{
    const auto state = map_okx_state(a_info.state);
    if (!state.has_value()) {
        return Error{"protocol", "unknown OKX order state: " + a_info.state};
    }
    return OrderSnapshot{.client_order_id = a_info.cl_ord_id,
                         .exchange_order_id = a_info.ord_id,
                         .state = *state,
                         .price = a_info.px,
                         .quantity = a_info.sz,
                         .filled_quantity = a_info.acc_fill_sz,
                         .average_fill_price = a_info.avg_px};
}
} // namespace

auto map_okx_state(std::string_view a_state) -> std::optional<OrderState>
{
    if (a_state == "live") {
        return OrderState::Live;
    }
    if (a_state == "partially_filled") {
        return OrderState::PartiallyFilled;
    }
    if (a_state == "filled") {
        return OrderState::Filled;
    }
    if (a_state == "canceled") {
        return OrderState::Canceled;
    }
    return std::nullopt;
}

OkxConnector::OkxConnector(OkxConfig a_config, OkxRestClient::TimestampProvider a_timestamp)
    : client_(std::move(a_config), std::move(a_timestamp))
{}

auto OkxConnector::place_order(const OrderRequest& a_request) -> Result<OrderPlacement>
{
    const auto ack = client_.place_order(to_wire(a_request));
    if (!ack.is_ok()) {
        return ack.error();
    }
    return ack_to_placement(ack.value());
}

auto OkxConnector::cancel_order(const CancelRequest& a_request) -> Result<OrderPlacement>
{
    const auto ack = client_.cancel_order(
        OkxCxlRequest{.inst_id = a_request.instrument_id, .cl_ord_id = a_request.client_order_id});
    if (!ack.is_ok()) {
        return ack.error();
    }
    return ack_to_placement(ack.value());
}

auto OkxConnector::amend_order(const AmendRequest& a_request) -> Result<OrderPlacement>
{
    const auto body = client_.amend_order(OkxAmendRequest{.inst_id = a_request.instrument_id,
                                                          .cl_ord_id = a_request.client_order_id,
                                                          .new_px = a_request.new_price,
                                                          .new_sz = a_request.new_quantity});
    if (!body.is_ok()) {
        return body.error();
    }
    return ack_to_placement(body.value());
}

auto OkxConnector::get_order(const OrderQuery& a_query) -> Result<std::optional<OrderSnapshot>>
{
    const auto info = client_.get_order(
        OkxQuery{.inst_id = a_query.instrument_id, .cl_ord_id = a_query.client_order_id});
    if (!info.is_ok()) {
        return info.error();
    }
    if (!info.value().has_value()) {
        return Result<std::optional<OrderSnapshot>>{std::optional<OrderSnapshot>{std::nullopt}};
    }
    const auto snapshot = to_snapshot(*info.value());
    if (!snapshot.is_ok()) {
        return snapshot.error();
    }
    return Result<std::optional<OrderSnapshot>>{std::optional<OrderSnapshot>{snapshot.value()}};
}

void OkxConnector::set_execution_report_handler(
    std::function<void(const ExecutionReport&)> a_handler)
{
    execution_report_handler_ = std::move(a_handler);
}

} // namespace gateway::exchange::okx
