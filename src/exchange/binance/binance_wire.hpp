#pragma once

#include "gateway/exchange_connector.hpp"
#include "gateway/result.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gateway::exchange::binance {

/// Order placement in Binance wire terms. price is empty for market
/// orders; time_in_force is "GTC"/"IOC"/"FOK" (empty = omit, venue
/// default). symbol uses the WIRE spelling (e.g. "BTCUSDT"); the
/// BTC-USDT <-> BTCUSDT translation lives in the connector
/// (SymbolTranslator).
struct BinancePlaceRequest
{
    std::string client_order_id;
    std::string symbol;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    std::string price;
    std::string quantity;
    std::string time_in_force;
};

struct BinanceCancelRequest
{
    std::string client_order_id;
    std::string symbol;
};

/// Amend emulation: one cancel + one replacement place (order.cancelReplace).
/// The replacement keeps the same clientOrderId (allowed by the venue) so
/// the gateway-level identity is stable while the venue orderId changes.
struct BinanceAmendRequest
{
    std::string client_order_id;
    std::string symbol;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    std::string price;    // full replacement price
    std::string quantity; // full replacement quantity
    std::string time_in_force;
};

struct BinanceOrderQuery
{
    std::string client_order_id;
    std::string symbol;
};

/// Normalized "order.place" ACK/RESULT payload.
struct BinanceOrderAck
{
    std::string order_id;
    std::string client_order_id;
    /// Wire status ("NEW", "FILLED", ...); empty when the response omitted
    /// it (pure ACK responses carry no status).
    std::string status;
    std::string executed_qty;
};

/// Normalized order.status / openOrders / cancel / cancelReplace leg.
struct BinanceOrderInfo
{
    std::string order_id;
    std::string client_order_id;
    /// Gateway-canonical symbol (e.g. "BTC-USDT"), reverse-translated.
    std::string symbol;
    std::string status;
    std::string side; // "BUY"/"SELL"
    std::string type; // "LIMIT"/"MARKET"
    std::string price;
    std::string orig_qty;
    std::string executed_qty;
    std::string cummulative_quote_qty;
};

/// Result of order.cancelReplace: both legs.
struct BinanceReplaceResult
{
    /// Wire "cancelResult" / "newOrderResult" ("SUCCESS"/"FAILURE"/...).
    std::string cancel_result;
    std::string new_order_result;
    BinanceOrderInfo canceled;
    BinanceOrderAck replacement;
};

// ---- request building --------------------------------------------------
// Builders produce the method params only; apiKey/timestamp/recvWindow/
// signature are added centrally by the signed API layer.

/// "order.place" params. LIMIT orders require price and timeInForce.
[[nodiscard]] auto
build_place_params(const BinancePlaceRequest& a_request) -> Result<nlohmann::json>;

/// "order.cancel" params.
[[nodiscard]] auto
build_cancel_params(const BinanceCancelRequest& a_request) -> Result<nlohmann::json>;

/// "order.cancelReplace" params (STOP_ON_FAILURE: never place the
/// replacement when the cancel failed — deterministic amend semantics).
[[nodiscard]] auto
build_cancel_replace_params(const BinanceAmendRequest& a_request) -> Result<nlohmann::json>;

/// "order.status" params.
[[nodiscard]] auto build_order_status_params(const BinanceOrderQuery& a_query) -> nlohmann::json;

/// "openOrders.status" params (all symbols: reconciliation adopts
/// everything the account has working).
[[nodiscard]] auto build_open_orders_params() -> nlohmann::json;

// ---- response parsing ---------------------------------------------------

/// Parse a successful "order.place"-style result object into an ack.
/// Tolerant of missing optional fields (ACK responses carry only ids).
[[nodiscard]] auto parse_order_ack(const nlohmann::json& a_result) -> BinanceOrderAck;

/// Parse an order-status-shaped result object. Returns an error Result for
/// non-object payloads.
[[nodiscard]] auto parse_order_info(const nlohmann::json& a_result) -> Result<BinanceOrderInfo>;

/// Parse a successful "order.cancelReplace" result.
[[nodiscard]] auto
parse_replace_result(const nlohmann::json& a_result) -> Result<BinanceReplaceResult>;

/// Map a Binance order status to the normalized OrderState. Returns
/// std::nullopt for unknown statuses (fail closed).
///   NEW, PENDING_NEW -> Live
///   PARTIALLY_FILLED -> PartiallyFilled
///   FILLED           -> Filled
///   CANCELED, PENDING_CANCEL, EXPIRED, EXPIRED_IN_MATCH -> Canceled
///     (PENDING_CANCEL is transient: the final CANCELED report follows;
///      EXPIRED = unfilled IOC/FOK remainder expired — accepted-then-ended)
///   REJECTED         -> Rejected
[[nodiscard]] auto map_binance_state(std::string_view a_status) -> std::optional<OrderState>;

/// Map "BUY"/"SELL" to the normalized Side; std::nullopt otherwise.
[[nodiscard]] auto map_binance_side(std::string_view a_side) -> std::optional<Side>;

/// Map "LIMIT"/"MARKET" to the normalized OrderType; std::nullopt otherwise.
[[nodiscard]] auto map_binance_type(std::string_view a_type) -> std::optional<OrderType>;

// ---- symbol translation -------------------------------------------------

/// Bidirectional BTC-USDT <-> BTCUSDT translation. Forward translations
/// are memoized; reverse translation prefers the memo, then splits the
/// longest known quote-asset suffix ("BTCUSDT" -> "BTC-USDT"), then passes
/// the symbol through unchanged (documented limitation: genuinely
/// ambiguous symbols are only produced by manual venue orders).
class SymbolTranslator
{
  public:
    /// Gateway -> wire ("BTC-USDT" -> "BTCUSDT"); registers the pair.
    [[nodiscard]] auto to_wire(const std::string& a_gateway_symbol) -> std::string;

    /// Wire -> gateway; memo first, then quote-suffix heuristic, then
    /// passthrough. Registers whatever it returns, so results stay stable.
    [[nodiscard]] auto to_gateway(std::string_view a_wire_symbol) -> std::string;

  private:
    /// Guards both memo maps: the connector calls from concurrent REST
    /// workers and the feed notifier thread (interface thread-safety
    /// contract, exchange_connector.hpp).
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> gateway_to_wire_;
    std::unordered_map<std::string, std::string> wire_to_gateway_;
};

} // namespace gateway::exchange::binance
