#include "exchange/okx/okx_connector.hpp"

#include "core/clock.hpp"
#include "core/retry.hpp"
#include "exchange/okx/okx_resilient.hpp"
#include "exchange/okx/okx_ws_client.hpp"

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

OkxConnector::OkxConnector(OkxConfig a_config, OkxRestClient::TimestampProvider a_timestamp)
    : config_(a_config),
      timestamp_provider_(a_timestamp ? std::move(a_timestamp)
                                      : OkxRestClient::TimestampProvider{&utc_now_iso_ms}),
      client_(std::move(a_config), timestamp_provider_), retry_clock_(real_retry_clock())
{}

OkxConnector::~OkxConnector()
{
    stop();
}

auto OkxConnector::place_order(const OrderRequest& a_request) -> Result<OrderPlacement>
{
    const auto ack = resilient_place(client_, to_wire(a_request), config_.retry, retry_clock_);
    if (!ack.is_ok()) {
        return ack.error();
    }
    return ack_to_placement(ack.value());
}

auto OkxConnector::cancel_order(const CancelRequest& a_request) -> Result<OrderPlacement>
{
    const auto ack = resilient_cancel(
        client_,
        OkxCxlRequest{.inst_id = a_request.instrument_id, .cl_ord_id = a_request.client_order_id},
        config_.retry, retry_clock_);
    if (!ack.is_ok()) {
        return ack.error();
    }
    return ack_to_placement(ack.value());
}

auto OkxConnector::amend_order(const AmendRequest& a_request) -> Result<OrderPlacement>
{
    const auto ack = resilient_amend(client_,
                                     OkxAmendRequest{.inst_id = a_request.instrument_id,
                                                     .cl_ord_id = a_request.client_order_id,
                                                     .new_px = a_request.new_price,
                                                     .new_sz = a_request.new_quantity},
                                     config_.retry, retry_clock_);
    if (!ack.is_ok()) {
        return ack.error();
    }
    return ack_to_placement(ack.value());
}

auto OkxConnector::get_order(const OrderQuery& a_query) -> Result<std::optional<OrderSnapshot>>
{
    const OkxQuery wire_query{.inst_id = a_query.instrument_id,
                              .cl_ord_id = a_query.client_order_id};
    const auto attempt = [this, &wire_query]() -> Result<std::optional<OrderSnapshot>> {
        const auto info = client_.get_order(wire_query);
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
    };
    // A GET is side-effect free: transport failures are retried directly.
    return with_retries<std::optional<OrderSnapshot>>(
        config_.retry, retry_clock_, attempt,
        [](const Error& a_error) { return a_error.code == "transport"; });
}

void OkxConnector::set_execution_report_handler(
    std::function<void(const ExecutionReport&)> a_handler)
{
    const std::lock_guard lock(handler_mutex_);
    execution_report_handler_ = std::move(a_handler);
}

void OkxConnector::forward_report(const ExecutionReport& a_report)
{
    std::function<void(const ExecutionReport&)> handler;
    {
        const std::lock_guard lock(handler_mutex_);
        handler = execution_report_handler_;
    }
    if (handler) {
        handler(a_report);
    }
}

void OkxConnector::start()
{
    if (!config_.ws.enabled || feed_) {
        return;
    }
    auto feed = std::make_unique<OkxOrdersFeed>(config_, timestamp_provider_);
    feed->set_report_handler([this](const ExecutionReport& a_report) { forward_report(a_report); });
    feed->start();
    feed_ = std::move(feed);
}

void OkxConnector::stop()
{
    if (feed_) {
        feed_->stop();
        feed_.reset();
    }
}

} // namespace gateway::exchange::okx
