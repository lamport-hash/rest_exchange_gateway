#pragma once

#include "core/event_log.hpp"
#include "core/latency.hpp"
#include "core/order_state.hpp"
#include "core/risk.hpp"
#include "gateway/exchange_connector.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gateway {

/// One version of a client order on its venue: the exchange order id the
/// venue issued for the initial place (version 1) or for an amend
/// (version N+1). CancelReplace venues (Binance) mint a NEW id per amend;
/// in-place amend venues (OKX) keep the id and still advance the version.
/// Reports for the current leg drive the full lifecycle; reports for
/// superseded legs contribute fills only.
struct OrderLeg
{
    std::uint64_t version = 1;
    std::string exchange_order_id;
};

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
    /// Full venue-side lifecycle of this clientOrderId: every leg the
    /// place and each amend created (cancelReplace venues mint a new
    /// exchange order id per amend; in-place venues repeat theirs with an
    /// incremented version). legs are ordered by version 1..N with no
    /// gaps; the last leg is current (== exchange_order_id). Reports for
    /// non-current legs may only contribute fills, never state.
    std::vector<OrderLeg> legs;
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
    /// Reports buffered because a place/amend venue call was open (they
    /// apply, in arrival order, right after that call's outcome lands).
    std::uint64_t reports_buffered = 0;
    /// Exchange order ids adopted from reports/snapshots the leg table
    /// did not know (a lost amend ack): each adoption is surfaced by
    /// audit().
    std::uint64_t legs_discovered = 0;
    std::uint64_t log_write_failures = 0;
};

/// One finding of the periodic consistency audit. check is a stable
/// machine-readable key (see audit()); detail is human-oriented.
struct ConsistencyAlert
{
    std::string client_order_id;
    std::string check;
    std::string detail;
};

/// Result of one non-mutating consistency pass (see audit()).
struct AuditReport
{
    std::vector<ConsistencyAlert> alerts;
    std::size_t orders_checked = 0;
    std::size_t venue_lookups = 0;
    std::size_t lookup_failures = 0;
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
/// - execution reports racing an in-flight place or amend are buffered
///   per clientOrderId (raced_reports_) and applied, in arrival order,
///   right after the outcome lands — for amends this keeps the old leg's
///   CANCELED from terminalizing the record before the replacement leg
///   exists, and keeps the log replay-correct
///   (place_submitted -> place_accepted -> state; amended -> state)
/// - a concurrent duplicate place of an in-flight clientOrderId replays
///   the Pending registry record instead of sending a second venue
///   request; a concurrent amend against an in-flight clientOrderId is
///   rejected "order_busy" (one mutating venue call per order)
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
    /// relies entirely on venue reconciliation). a_latency may be nullptr
    /// (latency tracking disabled); when set, place-send and
    /// fill-to-state-update measurements are appended to it.
    OrderManagementSystem(std::map<std::string, ExchangeConnector*> a_connectors, EventLog* a_log,
                          RiskConfig a_risk, std::string a_default_venue = "okx",
                          LatencyLog* a_latency = nullptr);

    OrderManagementSystem(const OrderManagementSystem&) = delete;
    auto operator=(const OrderManagementSystem&) -> OrderManagementSystem& = delete;

    /// Registered venue keys (for REST-layer validation).
    [[nodiscard]] auto venues() const -> std::vector<std::string>;

    /// Default venue key (used when a request omits the venue).
    [[nodiscard]] auto default_venue() const -> std::string;

    /// Read the shared latency clock (monotonic nanoseconds); returns
    /// kNoLatencyStamp when tracking is disabled. The REST layer uses
    /// this to stamp the request on handler entry and hand the value to
    /// place().
    [[nodiscard]] auto latency_now() const -> std::int64_t;

    /// Place a new order on a_venue (empty -> default venue).
    /// a_rest_hit_ns: REST-handler-entry timestamp from
    /// latency_now() (kNoLatencyStamp when the caller has none, e.g.
    /// internal/recovery paths); with tracking enabled the
    /// place-send measurements are logged just before the venue call.
    /// Paths:
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
    [[nodiscard]] auto place(const OrderRequest& a_request, std::string_view a_venue = {},
                             std::int64_t a_rest_hit_ns = kNoLatencyStamp) -> Result<PlaceOutcome>;

    /// Cancel; idempotent (already-canceled returns the record).
    /// Errors: "not_found", "order_pending" (venue has not acked yet —
    /// there is nothing to cancel; no venue call), "order_terminal",
    /// venue/transport codes.
    [[nodiscard]] auto cancel(std::string_view a_client_order_id) -> Result<OrderRecord>;

    /// Amend price and/or quantity (risk checks re-run on the new values).
    /// Errors: "not_found", "order_pending" (venue has not acked yet),
    /// "order_busy" (another place/amend for this clientOrderId is in
    /// flight), "order_terminal", "protocol" (nothing to change),
    /// venue/transport codes.
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

    /// One non-mutating consistency pass. NEVER repairs anything (healing
    /// stays with reconcile): it compares the registry against its own
    /// invariants and against venue truth, and returns alerts:
    /// - fill_state_mismatch: fills reached the quantity but the state is
    ///   not Filled (or the state is Filled with fills below quantity)
    /// - version_gap: the leg table is not a contiguous 1..N chain
    /// - unknown_leg_report: a report/snapshot carried an exchange order
    ///   id no leg knew (lost amend ack); it was adopted as a new leg
    /// - state_mismatch / fill_mismatch / price_qty_mismatch: local view
    ///   differs from the venue snapshot of a non-terminal order
    /// - venue_unknown: non-terminal locally, unknown to the venue
    /// - lookup_failed: the venue lookup itself failed (counted, alerted)
    /// Orders whose place/amend venue call is open are skipped (their own
    /// ack path owns them). Venue I/O happens without the registry lock.
    [[nodiscard]] auto audit() -> AuditReport;

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

    /// Apply one execution report arbitrated by the leg table: reports
    /// for the current leg drive the full lifecycle, reports for
    /// superseded legs contribute fills only, and a report whose id no
    /// leg knows is adopted as a new leg (lost amend ack) and counted in
    /// stats_.legs_discovered. Mutex_ must be held.
    void apply_arbitrated_report(OrderRecord& a_record, const ExecutionReport& a_report);

    /// Move out and drop the raced-reports buffer of a_client_order_id
    /// (place/amend outcome application drains it). Mutex_ must be held.
    auto drain_raced_reports(const std::string& a_client_order_id) -> std::vector<ExecutionReport>;

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
    /// the full lifecycle (legs) accumulates. No-op for an empty id or
    /// one that is already current. Amends do NOT use this (they always
    /// append a leg, even when the venue echoes the current id: the
    /// version advances).
    void note_exchange_id(OrderRecord& a_record, const std::string& a_id);

    /// Append the amend leg: version = last + 1 with the venue's id (new
    /// on cancelReplace venues, repeated on in-place venues) and make it
    /// current. Mutex_ must be held.
    void append_amend_leg(OrderRecord& a_record, const std::string& a_id);

    /// Highest leg index whose exchange order id equals a_id; a_id not
    /// found -> a_id absent from the leg table (returns nullopt).
    [[nodiscard]] auto leg_index_of(const OrderRecord& a_record,
                                    const std::string& a_id) const -> std::optional<std::size_t>;

    /// Remember an adopted unknown exchange order id for the next audit()
    /// (bounded queue: the newest discoveries survive an audit backlog).
    void note_discovered_leg(std::string a_client_order_id, std::string a_exchange_order_id);

    /// Local invariant checks of one record (fill vs state, leg version
    /// chain); appends alerts. Mutex_ must be held.
    void audit_record_local(const OrderRecord& a_record,
                            std::vector<ConsistencyAlert>& a_alerts) const;

    /// Venue-truth checks of one record against a snapshot (state, fills,
    /// price/quantity, unknown snapshot id); appends alerts.
    void audit_record_vs_snapshot(const OrderRecord& a_record, const OrderSnapshot& a_snapshot,
                                  std::vector<ConsistencyAlert>& a_alerts) const;

    std::map<std::string, ExchangeConnector*> connectors_;
    std::string default_venue_;
    EventLog* log_;
    /// Optional latency measurement sink (nullptr = disabled).
    LatencyLog* latency_;
    RiskConfig risk_;
    mutable std::mutex mutex_;
    /// The registry: every known clientOrderId. Pending entries are
    /// places whose venue call has not resolved yet — pending lives HERE,
    /// not in a side map.
    std::unordered_map<std::string, OrderRecord> orders_;

    /// Execution reports that raced a place or amend whose venue call is
    /// still open (guarded by mutex_), keyed by clientOrderId. Purely a
    /// buffer: it holds no order state. Reports drain (and persist) right
    /// after the venue outcome lands, keeping the log replay-correct
    /// (place_submitted -> place_accepted -> state; amended -> state).
    /// The entry is erased when the place/amend resolves — later reports
    /// apply to the registry record directly. An entry existing for an id
    /// also marks its venue call open: reconcile and audit skip it, and a
    /// second mutating request (amend) is rejected "order_busy".
    std::unordered_map<std::string, std::vector<ExecutionReport>> raced_reports_;

    /// Unknown exchange order ids adopted since the last audit() pass,
    /// drained into that audit's alerts. Bounded (newest kept).
    std::deque<std::pair<std::string, std::string>> discovered_leg_notes_;

    OmsStats stats_;
};

} // namespace gateway
