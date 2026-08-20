#pragma once

#include "core/event_log.hpp"
#include "core/order_state.hpp"
#include "core/risk.hpp"
#include "gateway/exchange_connector.hpp"

#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gateway {

/// The gateway's unified view of one client order (the registry entry).
struct OrderRecord
{
    std::string client_order_id;
    std::string symbol;
    std::string exchange_order_id;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    std::string time_in_force = "GTC";
    /// Current (post-amend) values as decimal strings.
    std::string price;
    std::string quantity;
    OrderState state = OrderState::Live;
    /// Monotonic high-water mark of reported fills.
    std::string filled_quantity = "0";
    std::string average_fill_price;
    /// Discovered live on the venue during (re)start reconciliation.
    bool adopted = false;
    /// Set when state == Rejected: the recorded rejection replayed to
    /// idempotent place retries (risk/venue rejection, or "venue does not
    /// know this order").
    std::optional<Error> rejection;
};

struct PlaceOutcome
{
    OrderRecord record;
    /// true when the outcome was served from the registry (idempotent
    /// replay of a clientOrderId the gateway has already seen).
    bool replayed = false;
};

struct AmendCommand
{
    std::string client_order_id;
    std::optional<std::string> new_price;
    std::optional<std::string> new_quantity;
};

struct ReconcileReport
{
    int adopted = 0;             // venue-live orders adopted into the registry
    int updated = 0;             // fresher fills/fields applied
    int terminal_resolved = 0;   // registry orders found terminal on the venue
    int absent_rejected = 0;     // venue conclusively does not know them -> Rejected
    int unresolved = 0;          // venue unreachable; entries left untouched
    bool pending_listing_failed = false;
};

struct OmsStats
{
    std::size_t known_orders = 0;
    std::uint64_t reports_applied = 0;
    /// Duplicate/out-of-order/racing observations correctly discarded.
    std::uint64_t reports_stale = 0;
    /// Reports about clientOrderIds this gateway never placed.
    std::uint64_t reports_unknown = 0;
    std::uint64_t log_write_failures = 0;
};

/// Order Management System: the single owner of order state.
///
/// Responsibilities:
/// - clientOrderId registry (clientOrderId -> exchangeOrderId and full record)
/// - strict idempotency: once a clientOrderId is known, places with it
///   replay the recorded outcome (identical ack or identical rejection)
/// - execution-report arbitration through the order state machine:
///   duplicates, out-of-order and REST-vs-WS races never regress state
///   (filled quantity is a monotonic high-water mark)
/// - pre-trade risk checks before any venue routing
/// - append-only persistence of every applied event + startup replay
/// - startup/reconnect reconciliation with the venue
///
/// Concurrency: all state is guarded by one mutex. Venue (connector) calls
/// are made while holding it, so a slow venue call delays other OMS
/// operations — a deliberate trade for simplicity and correctness (no
/// re-validation races); the REST layer stays responsive because it only
/// waits on this mutex.
class OrderManagementSystem
{
  public:
    /// a_log may be nullptr (persistence disabled; recovery then relies
    /// entirely on venue reconciliation).
    OrderManagementSystem(ExchangeConnector& a_connector, EventLog* a_log, RiskConfig a_risk);

    OrderManagementSystem(const OrderManagementSystem&) = delete;
    auto operator=(const OrderManagementSystem&) -> OrderManagementSystem& = delete;

    /// Place a new order. Paths:
    /// - known clientOrderId: replayed outcome (record + replayed=true),
    ///   or the recorded rejection as an error
    /// - risk rejection: recorded Rejected (code risk_*)
    /// - venue accepted: recorded Live
    /// - definitive venue rejection: recorded Rejected (code venue:*)
    /// - transport/unresolved: nothing recorded (the venue-side engine
    ///   already resolved as far as it could; the client may retry)
    /// - persistence failure after venue acceptance: recorded anyway and
    ///   reported as "persistence" (client retry replays the outcome)
    [[nodiscard]] auto place(const OrderRequest& a_request) -> Result<PlaceOutcome>;

    /// Cancel; idempotent (already-canceled returns the record).
    /// Errors: "not_found", "order_terminal", venue/transport codes.
    [[nodiscard]] auto cancel(std::string_view a_client_order_id) -> Result<OrderRecord>;

    /// Amend price and/or quantity (risk checks re-run on the new values).
    /// Errors: "not_found", "order_terminal", "protocol" (nothing to
    /// change), venue/transport codes.
    [[nodiscard]] auto amend(const AmendCommand& a_command) -> Result<OrderRecord>;

    /// Registry lookup (copy). Errors: "not_found". Serves the REST GET.
    [[nodiscard]] auto query(std::string_view a_client_order_id) -> Result<OrderRecord>;

    [[nodiscard]] auto stats() const -> OmsStats;

    /// Execution-report sink (connector worker threads).
    void on_execution_report(const ExecutionReport& a_report);

    /// Replay the persistence log into the registry (startup).
    /// Errors: "io"/"persistence" (corrupt log — startup should fail).
    [[nodiscard]] auto load_from_log() -> Result<EventLog::ReplayStats>;

    /// Reconcile the registry with the venue (startup and WS reconnect):
    /// adopt venue-live orders missing locally, refresh fills, resolve
    /// non-terminal entries (absent -> Rejected, unreachable -> keep).
    [[nodiscard]] auto reconcile() -> ReconcileReport;

  private:
    /// Apply one observation (WS report or REST snapshot). Returns true
    /// when something changed. Transitions go through the state machine
    /// (illegal/stale ones are discarded); filled_quantity only moves
    /// forward; snapshot price/quantity refresh the record when present.
    auto apply_observation(OrderRecord& a_record, OrderState a_state,
                           std::string_view a_filled, std::string_view a_avg_price,
                           std::string_view a_price = "", std::string_view a_quantity = "")
        -> bool;

    /// Adopt/reject/... helpers shared by reconcile and replay.
    void record_from_snapshot(const OrderSnapshot& a_snapshot, bool a_adopted);
    auto lookup(std::string_view a_client_order_id) -> std::unordered_map<
        std::string, OrderRecord>::iterator;

    /// Worst-case signed position for a_symbol if every working order
    /// (plus the candidate amount) fully filled. a_replace non-null: the
    /// candidate replaces that record's contribution (amend); null: the
    /// candidate is a fresh order (place).
    auto projected_position(const std::string& a_symbol, const OrderRecord* a_replace,
                            Side a_candidate_side, const std::string& a_candidate_qty)
        -> std::string;

    /// Append an event; counts failures (venue truth already applied).
    void append_event(const nlohmann::json& a_event);

    /// Append or fail: for request paths where the caller surfaces the
    /// error to the client (registry is updated regardless — the venue
    /// already accepted the action).
    auto append_or_error(const nlohmann::json& a_event) -> std::optional<Error>;

    void apply_log_event(const nlohmann::json& a_event);

    ExchangeConnector& connector_;
    EventLog* log_;
    RiskConfig risk_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, OrderRecord> orders_;
    OmsStats stats_;
};

} // namespace gateway
