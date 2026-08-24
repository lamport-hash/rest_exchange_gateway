#pragma once

#include "core/event_log.hpp"
#include "core/order_state.hpp"
#include "core/risk.hpp"
#include "gateway/exchange_connector.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gateway {

/// The gateway's unified view of one client order (the registry entry).
struct OrderRecord
{
    std::string client_order_id;
    std::string symbol;
    /// Venue key ("okx", "binance") the order is routed to.
    std::string venue;
    std::string exchange_order_id;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    std::string time_in_force = "GTC";
    /// Current (post-amend) values as decimal strings.
    std::string price;
    std::string quantity;
    /// Lifecycle state. Pending = the place was staged and sent but the
    /// venue has not acknowledged it (gateway-local; the exchangeOrderId
    /// is unknown/empty). Resolved by the venue ack, an execution report
    /// racing the ack, or restart reconciliation.
    OrderState state = OrderState::Pending;
    /// Monotonic high-water mark of reported fills.
    std::string filled_quantity = "0";
    std::string average_fill_price;
    /// Full venue-side lifecycle of this clientOrderId: every exchange
    /// order id it ever had (place + each amend; cancelReplace venues
    /// like Binance issue a NEW id per amend, in-place venues like OKX
    /// keep one). exchange_order_id is always the last entry. Reports
    /// for superseded ids may only contribute fills, never state.
    std::vector<std::string> exchange_order_ids;
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

/// Last-traded price of an instrument as served by one venue. venue is
/// the RESOLVED venue key (filled in when the request omitted it).
struct PriceQuote
{
    std::string venue;
    std::string price;
};

struct ReconcileReport
{
    int adopted = 0;           // venue-live orders adopted into the registry
    int updated = 0;           // fresher fills/fields applied
    int terminal_resolved = 0; // registry orders found terminal on the venue
    int absent_rejected = 0;   // venue conclusively does not know them -> Rejected
    int unresolved = 0;        // venue unreachable; entries left untouched
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
/// - venue routing: one connector per venue ("okx", "binance", ...); each
///   record remembers its venue and every follow-up routes through it
/// - strict idempotency: once a clientOrderId is known, places with it
///   replay the recorded outcome (identical ack, pending record, or
///   identical rejection)
/// - the pending stage: a place that passed risk is PERSISTED
///   (place_submitted) and recorded Pending BEFORE the venue call; the
///   venue ack, a racing execution report or restart reconciliation then
///   resolves it forward (Live/PartiallyFilled/Filled/Rejected). Pending
///   orders cannot be canceled or amended (the venue has not accepted
///   them) and count toward risk projections like working orders
/// - execution-report arbitration through the order state machine:
///   duplicates, out-of-order and REST-vs-WS races never regress state
///   (filled quantity is a monotonic high-water mark); reports from every
///   venue feed the same registry keyed by the globally-unique clientOrderId
/// - pre-trade risk checks before any venue routing (per instrument
///   across venues: limits span both books conservatively)
/// - append-only persistence of every applied event + startup replay
/// - startup/reconnect reconciliation with every venue
///
/// Concurrency: mutex_ guards the registry (and the raced-reports
/// buffer) only; venue (connector) calls are made WITHOUT holding it. A
/// venue feed thread applying execution reports therefore never waits
/// behind venue I/O (on WS-API venues like Binance the same connection
/// carries request responses, so a blocked feed thread would stall the
/// client's own ack). Consequences, handled explicitly:
/// - execution reports racing an in-flight place are buffered per
///   clientOrderId (raced_reports_) and applied right after the place
///   outcome lands, keeping the log replay-correct
///   (place_submitted -> place_accepted -> state)
/// - a concurrent duplicate place of an in-flight clientOrderId replays
///   the Pending registry record instead of sending a second venue
///   request
/// - pre-trade risk projects positions across the registry (Pending
///   entries included — they are working orders-to-be)
/// Registry mutations re-validate under the lock (state machine guards
/// illegal transitions); cancel/amend snapshot what they need before the
/// venue call and re-apply to the (possibly progressed) record after.
class OrderManagementSystem
{
  public:
    /// a_connectors: venue key -> connector (keys are lower-case venue
    /// names, e.g. {"okx", &okx}, {"binance", &binance}); must not be
    /// empty. a_default_venue is used when a place request carries no
    /// venue. a_log may be nullptr (persistence disabled; recovery then
    /// relies entirely on venue reconciliation).
    OrderManagementSystem(std::map<std::string, ExchangeConnector*> a_connectors, EventLog* a_log,
                          RiskConfig a_risk, std::string a_default_venue = "okx");

    OrderManagementSystem(const OrderManagementSystem&) = delete;
    auto operator=(const OrderManagementSystem&) -> OrderManagementSystem& = delete;

    /// Registered venue keys (for REST-layer validation).
    [[nodiscard]] auto venues() const -> std::vector<std::string>;

    /// Default venue key (used when a request omits the venue).
    [[nodiscard]] auto default_venue() const -> std::string;

    /// Place a new order on a_venue (empty -> default venue). Paths:
    /// - known clientOrderId: replayed outcome (record + replayed=true),
    ///   or the recorded rejection as an error. A still-unacked entry
    ///   (Pending — its venue call is in flight, or a previous attempt
    ///   was transport-unresolved) replays the pending record instead of
    ///   re-sending to the venue
    /// - unknown venue: "invalid_request"
    /// - risk rejection: recorded Rejected (code risk_*)
    /// - venue accepted: recorded Live
    /// - definitive venue rejection: recorded Rejected (code venue:*)
    /// - transport/unresolved: the record stays Pending (place_submitted
    ///   was already persisted; reconcile or an execution report resolves
    ///   it) and the transport error is returned
    /// - persistence failure of the pre-send place_submitted event: the
    ///   order is NOT sent ("persistence" error, nothing recorded); after
    ///   venue acceptance a persistence failure is reported but the
    ///   outcome is recorded anyway (client retry replays it)
    [[nodiscard]] auto place(const OrderRequest& a_request,
                             std::string_view a_venue = {}) -> Result<PlaceOutcome>;

    /// Cancel; idempotent (already-canceled returns the record).
    /// Errors: "not_found", "order_pending" (venue has not acked yet —
    /// there is nothing to cancel; no venue call), "order_terminal",
    /// venue/transport codes.
    [[nodiscard]] auto cancel(std::string_view a_client_order_id) -> Result<OrderRecord>;

    /// Amend price and/or quantity (risk checks re-run on the new values).
    /// Errors: "not_found", "order_pending" (venue has not acked yet),
    /// "order_terminal", "protocol" (nothing to change), venue/transport
    /// codes.
    [[nodiscard]] auto amend(const AmendCommand& a_command) -> Result<OrderRecord>;

    /// Registry lookup (copy). Errors: "not_found". Serves the REST GET.
    [[nodiscard]] auto query(std::string_view a_client_order_id) -> Result<OrderRecord>;

    /// Registry snapshot (copies), sorted by clientOrderId for a stable
    /// listing. Serves the REST collection GET.
    [[nodiscard]] auto all_orders() -> std::vector<OrderRecord>;

    /// Last-traded price of a_symbol from a_venue (empty -> default
    /// venue). Pure venue read (public market data) — no registry state,
    /// no registry lock. Errors: "invalid_request" (unknown venue) plus
    /// the connector's transport/protocol/venue codes.
    [[nodiscard]] auto get_price(const std::string& a_symbol,
                                 std::string_view a_venue = {}) -> Result<PriceQuote>;

    [[nodiscard]] auto stats() const -> OmsStats;

    /// Active pre-trade risk limits (copy taken under the lock). Serves
    /// the read-only GET /risk surface; limits are fixed by the config
    /// file at startup and never mutated at runtime.
    [[nodiscard]] auto risk_config() const -> RiskConfig;

    /// Execution-report sink (connector worker threads).
    void on_execution_report(const ExecutionReport& a_report);

    /// Replay the persistence log into the registry (startup).
    /// Errors: "io"/"persistence" (corrupt log — startup should fail).
    [[nodiscard]] auto load_from_log() -> Result<EventLog::ReplayStats>;

    /// Reconcile the registry with every venue (startup and WS
    /// reconnect): adopt venue-live orders missing locally, refresh
    /// fills, resolve non-terminal entries (absent -> Rejected,
    /// unreachable -> keep).
    [[nodiscard]] auto reconcile() -> ReconcileReport;

  private:
    /// Connector for a venue key; nullptr for unknown venues.
    [[nodiscard]] auto connector_for(const std::string& a_venue) const -> ExchangeConnector*;

    /// Connector that owns a record (venue field, default fallback).
    [[nodiscard]] auto connector_for(const OrderRecord& a_record) const -> ExchangeConnector*;

    /// Apply one observation (WS report or REST snapshot). Returns true
    /// when something changed. Transitions go through the state machine
    /// (illegal/stale ones are discarded); filled_quantity only moves
    /// forward; snapshot price/quantity refresh the record when present.
    /// a_apply_lifecycle=false (reports for superseded exchange order
    /// ids): only the monotonic fill high-water mark is applied.
    auto apply_observation(OrderRecord& a_record, OrderState a_state, std::string_view a_filled,
                           std::string_view a_avg_price, std::string_view a_price = "",
                           std::string_view a_quantity = "", bool a_apply_lifecycle = true) -> bool;

    /// Apply one execution report to a record (stats + persistence
    /// included). The first observation resolving a Pending record
    /// backfills the venue's exchangeOrderId (a known id is never
    /// overwritten — superseded legs are arbitrated by the caller via
    /// a_apply_lifecycle=false).
    void apply_report(OrderRecord& a_record, const ExecutionReport& a_report,
                      bool a_apply_lifecycle = true);

    /// Adopt/reject/... helpers shared by reconcile and replay.
    void record_from_snapshot(const OrderSnapshot& a_snapshot, const std::string& a_venue,
                              bool a_adopted);
    auto lookup(std::string_view a_client_order_id)
        -> std::unordered_map<std::string, OrderRecord>::iterator;

    /// Worst-case signed position for a_symbol if every working order
    /// (plus the candidate amount) fully filled. a_replace non-null: the
    /// candidate replaces that record's contribution (amend); null: the
    /// candidate is a fresh order (place — it is not in the registry yet:
    /// the dedup check in the same critical section guarantees it). Sums
    /// across venues: the same instrument traded on two venues is one net
    /// position. Pending entries are non-terminal registry records and
    /// count at their full quantity (a place whose venue call is still
    /// open is already exposure).
    auto projected_position(const std::string& a_symbol, const OrderRecord* a_replace,
                            Side a_candidate_side,
                            const std::string& a_candidate_qty) -> std::string;

    /// Append an event; counts failures (venue truth already applied).
    void append_event(const nlohmann::json& a_event);

    /// Append or fail: for request paths where the caller surfaces the
    /// error to the client (registry is updated regardless — the venue
    /// already accepted the action).
    auto append_or_error(const nlohmann::json& a_event) -> std::optional<Error>;

    void apply_log_event(const nlohmann::json& a_event);

    /// Register a venue order id on the record: current moves to a_id,
    /// the full lifecycle (exchange_order_ids) accumulates. No-op for an
    /// empty id or one that is already current.
    void note_exchange_id(OrderRecord& a_record, const std::string& a_id);

    std::map<std::string, ExchangeConnector*> connectors_;
    std::string default_venue_;
    EventLog* log_;
    RiskConfig risk_;
    mutable std::mutex mutex_;
    /// The registry: every known clientOrderId. Pending entries are
    /// places whose venue call has not resolved yet — pending lives HERE,
    /// not in a side map.
    std::unordered_map<std::string, OrderRecord> orders_;

    /// Execution reports that raced a place whose venue call is still
    /// open (guarded by mutex_), keyed by clientOrderId. Purely a buffer:
    /// it holds no order state. Reports drain (and persist) right after
    /// the place outcome lands, keeping the log replay-correct
    /// (place_submitted -> place_accepted -> state). The entry is erased
    /// when the place resolves — later reports apply to the registry
    /// record directly.
    std::unordered_map<std::string, std::vector<ExecutionReport>> raced_reports_;

    OmsStats stats_;
};

} // namespace gateway
