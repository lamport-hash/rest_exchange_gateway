#pragma once

#include "gateway/result.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gateway {

/// Order side, normalized across venues.
enum class Side
{
    Buy,
    Sell
};

/// Supported order types (venue-specific types are intentionally rejected).
enum class OrderType
{
    Limit,
    Market
};

/// Normalized order lifecycle state. Illegal transitions are a concern of
/// the order state machine (later phase); connectors only report snapshots.
enum class OrderState
{
    Live,
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected
};

/// Client-facing order request. price/quantity are decimal strings carried
/// verbatim to the venue (no float conversion).
struct OrderRequest
{
    std::string client_order_id;
    std::string instrument_id;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    std::string price;
    std::string quantity;
};

struct CancelRequest
{
    std::string client_order_id;
    std::string instrument_id;
};

struct OrderQuery
{
    std::string client_order_id;
    std::string instrument_id;
};

struct AmendRequest
{
    std::string client_order_id;
    std::string instrument_id;
    std::optional<std::string> new_price;
    std::optional<std::string> new_quantity;
};

/// Acknowledgement of place/cancel/amend: venue accepted the request.
struct OrderPlacement
{
    std::string client_order_id;
    std::string exchange_order_id;
};

/// Full normalized snapshot of an order as reported by the venue.
struct OrderSnapshot
{
    std::string client_order_id;
    std::string exchange_order_id;
    OrderState state = OrderState::Live;
    std::string price;
    std::string quantity;
    std::string filled_quantity;
    std::string average_fill_price;
};

/// Incremental execution update (WebSocket phase); delivered off the
/// connector's worker threads.
struct ExecutionReport
{
    std::string client_order_id;
    std::string exchange_order_id;
    OrderState state = OrderState::Live;
    std::string filled_quantity;
    std::string average_fill_price;
};

/// Venue-agnostic trading interface implemented by every exchange adapter.
/// All methods are synchronous in phase 1 (REST) and may fail with Result
/// errors: "transport" (network), "protocol" (malformed venue data),
/// "venue:<code>" (venue rejection). Thread-safety: implementations must be
/// safe to call from the REST worker threads.
class ExchangeConnector
{
  public:
    virtual ~ExchangeConnector() = default;

  protected:
    ExchangeConnector() = default;

  public:
    ExchangeConnector(const ExchangeConnector&) = delete;
    auto operator=(const ExchangeConnector&) -> ExchangeConnector& = delete;

    /// Place a new order. Returns the venue's order id mapping on success.
    [[nodiscard]] virtual auto
    place_order(const OrderRequest& a_request) -> Result<OrderPlacement> = 0;

    /// Cancel an open order.
    [[nodiscard]] virtual auto
    cancel_order(const CancelRequest& a_request) -> Result<OrderPlacement> = 0;

    /// Amend an open order (price and/or quantity). Venues without a native
    /// amend emulate it in their adapter.
    [[nodiscard]] virtual auto
    amend_order(const AmendRequest& a_request) -> Result<OrderPlacement> = 0;

    /// Fetch the current snapshot. std::nullopt means the venue does not
    /// know the order (never placed, or aged out).
    [[nodiscard]] virtual auto
    get_order(const OrderQuery& a_query) -> Result<std::optional<OrderSnapshot>> = 0;

    /// Register the execution-report sink (WebSocket-driven updates).
    virtual void
    set_execution_report_handler(std::function<void(const ExecutionReport&)> a_handler) = 0;
};

/// Lower-case name of a state for logging/responses, e.g. "partially_filled".
[[nodiscard]] inline auto to_string(OrderState a_state) -> std::string_view
{
    switch (a_state) {
    case OrderState::Live:
        return "live";
    case OrderState::PartiallyFilled:
        return "partially_filled";
    case OrderState::Filled:
        return "filled";
    case OrderState::Canceled:
        return "canceled";
    case OrderState::Rejected:
        return "rejected";
    }
    return "unknown";
}

} // namespace gateway
