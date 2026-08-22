#include "exchange/binance/binance_connector.hpp"

#include <iostream>
#include <utility>

namespace gateway::exchange::binance {

namespace {

/// Full snapshot normalization; fails closed on unknown state/side/type so
/// corrupt venue data is surfaced instead of guessed.
auto to_snapshot(const BinanceOrderInfo& a_info,
                 SymbolTranslator& a_symbols) -> Result<OrderSnapshot>
{
    const auto state = map_binance_state(a_info.status);
    const auto side = map_binance_side(a_info.side);
    const auto type = map_binance_type(a_info.type);
    if (!state.has_value()) {
        return Error{"protocol", "unknown Binance order status: " + a_info.status};
    }
    if (!side.has_value()) {
        return Error{"protocol", "unknown Binance order side: " + a_info.side};
    }
    if (!type.has_value()) {
        return Error{"protocol", "unknown Binance order type: " + a_info.type};
    }
    return OrderSnapshot{.client_order_id = a_info.client_order_id,
                         .exchange_order_id = a_info.order_id,
                         .instrument_id = a_symbols.to_gateway(a_info.symbol),
                         .state = *state,
                         .side = *side,
                         .type = *type,
                         .price = a_info.price,
                         .quantity = a_info.orig_qty,
                         .filled_quantity = a_info.executed_qty,
                         .average_fill_price = {}};
}

auto placement_from(const BinanceOrderAck& a_ack) -> Result<OrderPlacement>
{
    if (a_ack.order_id.empty()) {
        return Error{"protocol", "venue ack without orderId"};
    }
    return OrderPlacement{.client_order_id = a_ack.client_order_id,
                          .exchange_order_id = a_ack.order_id};
}

} // namespace

BinanceConnector::BinanceConnector(BinanceConfig a_config, UnixMsProvider a_timestamp)
    : config_(a_config),
      timestamp_provider_(a_timestamp ? std::move(a_timestamp) : UnixMsProvider{&real_unix_ms}),
      retry_clock_(real_retry_clock()), ws_(std::move(a_config), timestamp_provider_),
      api_([this](const std::string& a_method,
                  const nlohmann::json& a_params) { return ws_.call_signed(a_method, a_params); },
           // Public (NONE-security) methods like ticker.price go out
           // unsigned on the same session.
           [this](const std::string& a_method, const nlohmann::json& a_params) {
               return ws_.call(a_method, a_params);
           })
{}

BinanceConnector::~BinanceConnector()
{
    stop();
}

auto BinanceConnector::place_order(const OrderRequest& a_request) -> Result<OrderPlacement>
{
    const BinancePlaceRequest wire{.client_order_id = a_request.client_order_id,
                                   .symbol = symbols_.to_wire(a_request.instrument_id),
                                   .side = a_request.side,
                                   .type = a_request.type,
                                   .price = a_request.price,
                                   .quantity = a_request.quantity,
                                   .time_in_force = a_request.time_in_force.empty()
                                                        ? std::string{"GTC"}
                                                        : a_request.time_in_force};
    const auto ack = binance_resilient_place(api_, wire, config_.retry, retry_clock_);
    if (!ack.is_ok()) {
        return ack.error();
    }
    return placement_from(ack.value());
}

auto BinanceConnector::cancel_order(const CancelRequest& a_request) -> Result<OrderPlacement>
{
    const BinanceCancelRequest wire{.client_order_id = a_request.client_order_id,
                                    .symbol = symbols_.to_wire(a_request.instrument_id)};
    const auto info = binance_resilient_cancel(api_, wire, config_.retry, retry_clock_);
    if (!info.is_ok()) {
        return info.error();
    }
    return placement_from(BinanceOrderAck{.order_id = info.value().order_id,
                                          .client_order_id = info.value().client_order_id,
                                          .status = info.value().status,
                                          .executed_qty = info.value().executed_qty});
}

auto BinanceConnector::amend_order(const AmendRequest& a_request) -> Result<OrderPlacement>
{
    if (!a_request.new_price.has_value() || !a_request.new_quantity.has_value() ||
        !a_request.side.has_value() || !a_request.type.has_value()) {
        return Error{"protocol", "Binance amend (cancelReplace) needs the full replacement: price, "
                                 "quantity, side and type of the resulting order"};
    }
    const BinanceAmendRequest wire{.client_order_id = a_request.client_order_id,
                                   .symbol = symbols_.to_wire(a_request.instrument_id),
                                   .side = *a_request.side,
                                   .type = *a_request.type,
                                   .price = *a_request.new_price,
                                   .quantity = *a_request.new_quantity,
                                   .time_in_force = a_request.time_in_force.empty()
                                                        ? std::string{"GTC"}
                                                        : a_request.time_in_force};
    const auto ack = binance_resilient_amend(api_, wire, config_.retry, retry_clock_);
    if (!ack.is_ok()) {
        return ack.error();
    }
    return placement_from(ack.value());
}

auto BinanceConnector::get_order(const OrderQuery& a_query) -> Result<std::optional<OrderSnapshot>>
{
    const BinanceOrderQuery wire{.client_order_id = a_query.client_order_id,
                                 .symbol = symbols_.to_wire(a_query.instrument_id)};
    const auto attempt = [this, &wire]() -> Result<std::optional<OrderSnapshot>> {
        const auto info = api_.get_order(wire);
        if (!info.is_ok()) {
            return info.error();
        }
        if (!info.value().has_value()) {
            return Result<std::optional<OrderSnapshot>>{std::optional<OrderSnapshot>{std::nullopt}};
        }
        const auto snapshot = to_snapshot(*info.value(), symbols_);
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

auto BinanceConnector::get_open_orders() -> Result<std::vector<OrderSnapshot>>
{
    const auto attempt = [this]() -> Result<std::vector<BinanceOrderInfo>> {
        return api_.get_open_orders();
    };
    const auto orders = with_retries<std::vector<BinanceOrderInfo>>(
        config_.retry, retry_clock_, attempt,
        [](const Error& a_error) { return a_error.code == "transport"; });
    if (!orders.is_ok()) {
        return orders.error();
    }

    std::vector<OrderSnapshot> snapshots;
    snapshots.reserve(orders.value().size());
    for (const auto& info : orders.value()) {
        auto snapshot = to_snapshot(info, symbols_);
        if (snapshot.is_ok()) {
            snapshots.push_back(std::move(snapshot.value()));
        }
    }
    return snapshots;
}

auto BinanceConnector::get_price(const std::string& a_instrument_id) -> Result<std::string>
{
    // Read-only public market data: plain transport retries (no side
    // effects, resolve-then-retry unnecessary), shared venue policy.
    const std::string wire_symbol = symbols_.to_wire(a_instrument_id);
    const auto attempt = [this, &wire_symbol]() -> Result<std::string> {
        return api_.get_price(wire_symbol);
    };
    return with_retries<std::string>(
        config_.retry, retry_clock_, attempt,
        [](const Error& a_error) { return a_error.code == "transport"; });
}

void BinanceConnector::set_execution_report_handler(
    std::function<void(const ExecutionReport&)> a_handler)
{
    const std::lock_guard lock(handler_mutex_);
    execution_report_handler_ = std::move(a_handler);
}

void BinanceConnector::set_connectivity_handler(std::function<void(bool)> a_handler)
{
    const std::lock_guard lock(handler_mutex_);
    connectivity_handler_ = std::move(a_handler);
}

void BinanceConnector::forward_connectivity(bool a_connected)
{
    std::function<void(bool)> handler;
    {
        const std::lock_guard lock(handler_mutex_);
        handler = connectivity_handler_;
    }
    if (handler) {
        handler(a_connected);
    }
}

void BinanceConnector::forward_report(const ExecutionReport& a_report)
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

void BinanceConnector::start()
{
    if (started_) {
        return;
    }
    started_ = true;
    ws_.set_report_handler([this](const ExecutionReport& a_report) { forward_report(a_report); });
    ws_.set_event_handler([this](const BinanceFeedEvent& a_event) {
        switch (a_event.type) {
        case BinanceFeedEventType::Connected:
            forward_connectivity(true);
            break;
        case BinanceFeedEventType::Disconnected:
        case BinanceFeedEventType::Stopped:
            forward_connectivity(false);
            break;
        case BinanceFeedEventType::Connecting:
            break;
        case BinanceFeedEventType::ProtocolWarning:
            // venue frames this gateway cannot normalize must be visible
            // in operations, not silently dropped
            std::cerr << "binance feed warning: " << a_event.detail << '\n';
            break;
        }
    });
    ws_.start();
}

void BinanceConnector::stop()
{
    if (started_) {
        ws_.stop();
        started_ = false;
        forward_connectivity(false);
    }
}

} // namespace gateway::exchange::binance
