#include "core/oms.hpp"

#include "core/decimal.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace gateway {

namespace {

auto side_to_string(Side a_side) -> std::string_view
{
    return a_side == Side::Buy ? "buy" : "sell";
}

auto parse_side(std::string_view a_text) -> std::optional<Side>
{
    if (a_text == "buy") {
        return Side::Buy;
    }
    if (a_text == "sell") {
        return Side::Sell;
    }
    return std::nullopt;
}

auto type_to_string(OrderType a_type) -> std::string_view
{
    return a_type == OrderType::Limit ? "limit" : "market";
}

auto parse_type(std::string_view a_text) -> std::optional<OrderType>
{
    if (a_text == "limit") {
        return OrderType::Limit;
    }
    if (a_text == "market") {
        return OrderType::Market;
    }
    return std::nullopt;
}

auto state_to_string(OrderState a_state) -> std::string_view
{
    return to_string(a_state);
}

auto parse_state(std::string_view a_text) -> std::optional<OrderState>
{
    if (a_text == "live") {
        return OrderState::Live;
    }
    if (a_text == "partially_filled") {
        return OrderState::PartiallyFilled;
    }
    if (a_text == "filled") {
        return OrderState::Filled;
    }
    if (a_text == "canceled") {
        return OrderState::Canceled;
    }
    if (a_text == "rejected") {
        return OrderState::Rejected;
    }
    return std::nullopt;
}

auto event_string(const nlohmann::json& a_event, const char* a_name) -> std::optional<std::string>
{
    const auto it = a_event.find(a_name);
    if (it != a_event.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return std::nullopt;
}

/// The pre-send intent record: persisted BEFORE the venue call so a
/// crash between send and ack still leaves a Pending entry that startup
/// replay restores and reconciliation resolves.
auto place_submitted_event(const OrderRecord& a_record) -> nlohmann::json
{
    return {{"type", "place_submitted"},
            {"clientOrderId", a_record.client_order_id},
            {"symbol", a_record.symbol},
            {"venue", a_record.venue},
            {"side", side_to_string(a_record.side)},
            {"orderType", type_to_string(a_record.type)},
            {"price", a_record.price},
            {"quantity", a_record.quantity},
            {"timeInForce", a_record.time_in_force}};
}

auto place_accepted_event(const OrderRecord& a_record) -> nlohmann::json
{
    return {{"type", "place_accepted"},
            {"clientOrderId", a_record.client_order_id},
            {"symbol", a_record.symbol},
            {"venue", a_record.venue},
            {"side", side_to_string(a_record.side)},
            {"orderType", type_to_string(a_record.type)},
            {"price", a_record.price},
            {"quantity", a_record.quantity},
            {"timeInForce", a_record.time_in_force},
            {"exchangeOrderId", a_record.exchange_order_id},
            {"version", a_record.legs.empty() ? 1 : a_record.legs.back().version}};
}

auto adopted_event(const OrderRecord& a_record) -> nlohmann::json
{
    nlohmann::json event = place_accepted_event(a_record);
    event["type"] = "adopted";
    event["state"] = state_to_string(a_record.state);
    event["filledQuantity"] = a_record.filled_quantity;
    event["averageFillPrice"] = a_record.average_fill_price;
    return event;
}

auto rejected_event(const OrderRecord& a_record) -> nlohmann::json
{
    return {{"type", "rejected"},
            {"clientOrderId", a_record.client_order_id},
            {"symbol", a_record.symbol},
            {"venue", a_record.venue},
            {"code", a_record.rejection.value_or(Error{}).code},
            {"reason", a_record.rejection.value_or(Error{}).message}};
}

auto amended_event(const OrderRecord& a_record) -> nlohmann::json
{
    return {{"type", "amended"},
            {"clientOrderId", a_record.client_order_id},
            {"price", a_record.price},
            {"quantity", a_record.quantity},
            {"state", state_to_string(a_record.state)},
            {"exchangeOrderId", a_record.exchange_order_id},
            {"version", a_record.legs.empty() ? 1 : a_record.legs.back().version}};
}

auto state_event(const OrderRecord& a_record) -> nlohmann::json
{
    return {{"type", "state"},
            {"clientOrderId", a_record.client_order_id},
            {"state", state_to_string(a_record.state)},
            {"filledQuantity", a_record.filled_quantity},
            {"averageFillPrice", a_record.average_fill_price},
            {"exchangeOrderId", a_record.exchange_order_id},
            {"version", a_record.legs.empty() ? 1 : a_record.legs.back().version}};
}

} // namespace

OrderManagementSystem::OrderManagementSystem(std::map<std::string, ExchangeConnector*> a_connectors,
                                             EventLog* a_log, RiskConfig a_risk,
                                             std::string a_default_venue)
    : connectors_(std::move(a_connectors)), default_venue_(std::move(a_default_venue)), log_(a_log),
      risk_(std::move(a_risk))
{}

auto OrderManagementSystem::connector_for(const std::string& a_venue) const -> ExchangeConnector*
{
    const auto it = connectors_.find(a_venue);
    return it == connectors_.end() ? nullptr : it->second;
}

auto OrderManagementSystem::connector_for(const OrderRecord& a_record) const -> ExchangeConnector*
{
    if (!a_record.venue.empty()) {
        return connector_for(a_record.venue);
    }
    return connector_for(default_venue_); // records from pre-venue logs
}

auto OrderManagementSystem::venues() const -> std::vector<std::string>
{
    std::vector<std::string> keys;
    keys.reserve(connectors_.size());
    for (const auto& [venue, connector] : connectors_) {
        keys.push_back(venue);
    }
    return keys;
}

auto OrderManagementSystem::default_venue() const -> std::string
{
    return default_venue_;
}

auto OrderManagementSystem::lookup(std::string_view a_client_order_id)
    -> std::unordered_map<std::string, OrderRecord>::iterator
{
    return orders_.find(std::string{a_client_order_id});
}

auto OrderManagementSystem::stats() const -> OmsStats
{
    const std::lock_guard lock(mutex_);
    OmsStats stats = stats_;
    stats.known_orders = orders_.size();
    return stats;
}

auto OrderManagementSystem::risk_config() const -> RiskConfig
{
    const std::lock_guard lock(mutex_);
    return risk_;
}

void OrderManagementSystem::append_event(const nlohmann::json& a_event)
{
    if (log_ == nullptr) {
        return;
    }
    if (log_->append(a_event).has_value()) {
        ++stats_.log_write_failures;
    }
}

auto OrderManagementSystem::append_or_error(const nlohmann::json& a_event) -> std::optional<Error>
{
    if (log_ == nullptr) {
        return std::nullopt;
    }
    return log_->append(a_event);
}

auto OrderManagementSystem::projected_position(const std::string& a_symbol,
                                               const OrderRecord* a_replace, Side a_candidate_side,
                                               const std::string& a_candidate_qty) -> std::string
{
    Decimal projected{};

    const auto add_in = [&projected](const Decimal& a_amount) {
        const auto sum = add(projected, a_amount);
        if (sum.is_ok()) {
            projected = sum.value();
        }
    };
    const auto signed_amount = [](Side a_side, const Decimal& a_magnitude) {
        return a_side == Side::Sell ? negate(a_magnitude) : a_magnitude;
    };

    for (const auto& [id, record] : orders_) {
        if (record.symbol != a_symbol) {
            continue;
        }
        // already-executed quantity counts regardless of terminal state
        // (a filled or cancel-after-partial order did execute)
        const auto filled =
            parse_decimal(record.filled_quantity.empty() ? "0" : record.filled_quantity);
        if (filled.is_ok() && !is_zero(filled.value())) {
            add_in(signed_amount(record.side, filled.value()));
        }
        // outstanding quantity of working orders may still execute.
        // Pending entries are non-terminal records and count in full: a
        // place whose venue call is open (or was transport-unresolved) is
        // already exposure — the venue may accept it at any moment.
        if (is_terminal(record.state) ||
            (a_replace != nullptr && id == a_replace->client_order_id)) {
            continue;
        }
        const auto quantity = parse_decimal(record.quantity);
        if (quantity.is_ok()) {
            add_in(signed_amount(record.side, sub_clamped_zero(quantity.value(), filled.value())));
        }
    }

    // the candidate order itself, replacing a_replace's contribution
    const auto candidate_qty = parse_decimal(a_candidate_qty);
    if (candidate_qty.is_ok()) {
        const auto candidate_filled = parse_decimal(
            a_replace != nullptr && !a_replace->filled_quantity.empty() ? a_replace->filled_quantity
                                                                        : "0");
        if (candidate_filled.is_ok()) {
            add_in(signed_amount(a_candidate_side, sub_clamped_zero(candidate_qty.value(),
                                                                    candidate_filled.value())));
        }
    }
    return decimal_to_string(projected);
}

auto OrderManagementSystem::place(const OrderRequest& a_request,
                                  std::string_view a_venue) -> Result<PlaceOutcome>
{
    std::string venue{a_venue.empty() ? default_venue_ : std::string{a_venue}};

    ExchangeConnector* connector = nullptr;
    { // ---- critical section 1: dedup, risk, stage the pending record ----
        const std::lock_guard lock(mutex_);

        if (const auto existing = lookup(a_request.client_order_id); existing != orders_.end()) {
            if (existing->second.rejection.has_value()) {
                return existing->second.rejection.value();
            }
            // Replay the recorded outcome — including a still-Pending
            // entry (its venue call is in flight, or a previous attempt
            // was transport-unresolved): a duplicate must never re-send.
            return PlaceOutcome{.record = existing->second, .replayed = true};
        }

        connector = connector_for(venue);
        if (connector == nullptr) {
            return Error{"invalid_request",
                         "unsupported venue \"" + venue + "\" (configured: okx, binance)"};
        }

        // ---- pre-trade risk ----
        OrderRecord candidate{.client_order_id = a_request.client_order_id,
                              .symbol = a_request.instrument_id,
                              .venue = venue,
                              .exchange_order_id = "",
                              .side = a_request.side,
                              .type = a_request.type,
                              .time_in_force = a_request.time_in_force.empty()
                                                   ? std::string{"GTC"}
                                                   : a_request.time_in_force,
                              .price = a_request.price,
                              .quantity = a_request.quantity,
                               .state = OrderState::Pending,
                               .filled_quantity = "0",
                               .average_fill_price = "",
                               .legs = {},
                               .adopted = false,
                               .rejection = std::nullopt};
        const auto limits = risk_.limits_for(a_request.instrument_id);
        if (limits.has_value()) {
            const RiskOrder risk_order{.side = a_request.side,
                                       .price = a_request.price,
                                       .quantity = a_request.quantity,
                                       .projected_position =
                                           projected_position(a_request.instrument_id, nullptr,
                                                              a_request.side, a_request.quantity)};
            if (const auto rejection = check_risk(limits, a_request.instrument_id, risk_order)) {
                candidate.state = OrderState::Rejected;
                candidate.rejection = *rejection;
                const auto persist_error = append_or_error(rejected_event(candidate));
                orders_[candidate.client_order_id] = candidate;
                if (persist_error.has_value()) {
                    ++stats_.log_write_failures;
                    return *persist_error;
                }
                return *rejection;
            }
        }

        // ---- born Pending: persist the intent BEFORE the venue call ----
        if (const auto persist_error = append_or_error(place_submitted_event(candidate));
            persist_error.has_value()) {
            // Nothing was sent to the venue: abort without recording.
            // Sending after a lost place_submitted would create an order
            // no restart can attribute to this clientOrderId.
            ++stats_.log_write_failures;
            return Error{"persistence",
                         persist_error->message + " (order was not sent to the venue)"};
        }
        orders_[candidate.client_order_id] = std::move(candidate);
        raced_reports_.try_emplace(a_request.client_order_id);
    }

    // ---- venue routing (NO lock: a venue feed thread applying execution
    // reports must never wait behind venue I/O — on WS-API venues the
    // same connection carries this very response) ----
    const auto placement = connector->place_order(a_request);

    { // ---- critical section 2: apply the outcome, drain raced reports ----
        const std::lock_guard lock(mutex_);
        const auto raced = drain_raced_reports(a_request.client_order_id);
        const auto it = lookup(a_request.client_order_id);
        if (it == orders_.end()) {
            return Error{"internal", "pending place vanished for " + a_request.client_order_id};
        }
        OrderRecord& record = it->second;

        if (!placement.is_ok()) {
            if (placement.error().code != "transport") {
                // definitive venue rejection: terminal Rejected, replayed to
                // idempotent retries
                if (apply_transition(record.state, OrderState::Rejected) ==
                    TransitionResult::Applied) {
                    record.rejection = placement.error();
                    const auto persist_error = append_or_error(rejected_event(record));
                    if (persist_error.has_value()) {
                        ++stats_.log_write_failures;
                    }
                }
                return placement.error();
            }
            // transport/unresolved: the record stays Pending (the intent
            // is persisted; reconcile or an execution report resolves
            // it). Reports that raced the failure are real observations
            // about the venue-side outcome: apply them.
            for (const auto& entry : raced) {
                apply_arbitrated_report(record, entry);
            }
            return placement.error();
        }

        note_exchange_id(record, placement.value().exchange_order_id);
        // The venue acked: Pending -> Live. The record cannot have
        // advanced meanwhile (racing reports were buffered, reconcile
        // skips ids with an open venue call); the state machine still
        // guards the move.
        (void)apply_transition(record.state, OrderState::Live);
        const auto persist_error = append_or_error(place_accepted_event(record));
        if (persist_error.has_value()) {
            ++stats_.log_write_failures;
            return Error{"persistence", persist_error->message +
                                            " (order was accepted by the venue; retry replays it)"};
        }
        // reports that raced the venue ack apply after the place_accepted
        // event exists, keeping the log replay-correct; the first applied
        // observation may jump Pending -> PartiallyFilled/Filled
        for (const auto& entry : raced) {
            apply_arbitrated_report(record, entry);
        }
        return PlaceOutcome{.record = record, .replayed = false};
    }
}

auto OrderManagementSystem::cancel(std::string_view a_client_order_id) -> Result<OrderRecord>
{
    CancelRequest wire;
    ExchangeConnector* connector = nullptr;
    { // ---- snapshot + validate under the lock ----
        const std::lock_guard lock(mutex_);

        const auto it = lookup(a_client_order_id);
        if (it == orders_.end()) {
            return Error{"not_found", "unknown clientOrderId " + std::string{a_client_order_id}};
        }
        const OrderRecord& record = it->second;
        if (record.state == OrderState::Pending) {
            // The venue has not accepted the order: there is nothing to
            // cancel. Never a venue call.
            return Error{"order_pending", "order " + record.client_order_id +
                                              " is pending venue acknowledgement; "
                                              "cancel after the venue acknowledges it"};
        }
        if (record.state == OrderState::Canceled) {
            return record; // idempotent cancel
        }
        if (is_terminal(record.state)) {
            return Error{"order_terminal", "order " + record.client_order_id + " is " +
                                               std::string{state_to_string(record.state)}};
        }
        connector = connector_for(record);
        if (connector == nullptr) {
            return Error{"internal", "no connector for venue \"" + record.venue + "\""};
        }
        wire = CancelRequest{record.client_order_id, record.symbol};
    }

    // ---- venue I/O without the lock ----
    const auto placement = connector->cancel_order(wire);

    { // ---- apply to the (possibly progressed) record ----
        const std::lock_guard lock(mutex_);
        const auto it = lookup(a_client_order_id);
        if (it == orders_.end()) {
            return Error{"not_found", "unknown clientOrderId " + std::string{a_client_order_id}};
        }
        OrderRecord& record = it->second;
        if (!placement.is_ok()) {
            return placement.error();
        }
        note_exchange_id(record, placement.value().exchange_order_id);
        // A successful venue cancel is definitive for a working order; the
        // Live/PartiallyFilled -> Canceled steps are legal by precondition.
        // A fill that raced the cancel request is honored: the state
        // machine discards the terminal->Canceled regression.
        if (apply_transition(record.state, OrderState::Canceled) == TransitionResult::Applied) {
            append_event(state_event(record));
        }
        return record;
    }
}

auto OrderManagementSystem::amend(const AmendCommand& a_command) -> Result<OrderRecord>
{
    if (!a_command.new_price.has_value() && !a_command.new_quantity.has_value()) {
        return Error{"protocol", "amend requires a new price and/or quantity"};
    }

    AmendRequest wire;
    ExchangeConnector* connector = nullptr;
    { // ---- snapshot + validate + risk under the lock ----
        const std::lock_guard lock(mutex_);

        const auto it = lookup(a_command.client_order_id);
        if (it == orders_.end()) {
            return Error{"not_found", "unknown clientOrderId " + a_command.client_order_id};
        }
        const OrderRecord& record = it->second;
        if (record.state == OrderState::Pending) {
            // The venue has not accepted the order: amending an order the
            // venue may never see (or is still resolving) is ambiguous.
            // Never a venue call.
            return Error{"order_pending", "order " + record.client_order_id +
                                              " is pending venue acknowledgement; "
                                              "amend after the venue acknowledges it"};
        }
        if (is_terminal(record.state)) {
            return Error{"order_terminal", "order " + record.client_order_id + " is " +
                                               std::string{state_to_string(record.state)}};
        }
        connector = connector_for(record);
        if (connector == nullptr) {
            return Error{"internal", "no connector for venue \"" + record.venue + "\""};
        }

        const std::string new_price = a_command.new_price.value_or(record.price);
        const std::string new_quantity = a_command.new_quantity.value_or(record.quantity);

        const auto limits = risk_.limits_for(record.symbol);
        if (limits.has_value()) {
            const RiskOrder risk_order{.side = record.side,
                                       .price = new_price,
                                       .quantity = new_quantity,
                                       .projected_position = projected_position(
                                           record.symbol, &record, record.side, new_quantity)};
            if (const auto rejection = check_risk(limits, record.symbol, risk_order)) {
                return *rejection;
            }
        }

        wire = AmendRequest{.client_order_id = record.client_order_id,
                            .instrument_id = record.symbol,
                            .new_price = new_price,
                            .new_quantity = new_quantity,
                            .side = record.side,
                            .type = record.type,
                            .time_in_force = record.time_in_force};

        // Register the raced-reports buffer BEFORE the venue call: on
        // cancelReplace venues (Binance) the old leg's CANCELED and the
        // replacement's NEW/FILLED reports can all arrive on the feed
        // thread before the amend response returns. Buffered here, they
        // apply (and persist) AFTER the amended event installs the new
        // leg — the old leg arbitrates as superseded (fills only), the
        // replacement drives the lifecycle. A concurrent place/amend of
        // the same order is rejected instead: one mutating venue call.
        if (!raced_reports_.try_emplace(a_command.client_order_id).second) {
            return Error{"order_busy",
                         "a place/amend for " + a_command.client_order_id +
                             " is still in flight; retry once it resolves"};
        }
    }

    // ---- venue I/O without the lock ----
    const auto placement = connector->amend_order(wire);

    { // ---- apply to the (definitely unchanged) record, drain raced reports ----
        const std::lock_guard lock(mutex_);
        const auto raced = drain_raced_reports(a_command.client_order_id);
        const auto it = lookup(a_command.client_order_id);
        if (it == orders_.end()) {
            return Error{"not_found", "unknown clientOrderId " + a_command.client_order_id};
        }
        OrderRecord& record = it->second;

        if (!placement.is_ok()) {
            // No new leg is installed: the amend did not verifiably land.
            // A buffered report carrying an UNKNOWN id proves the
            // replacement actually went live (a lost amend ack): adopt the
            // newest such id BEFORE applying the batch, so the old leg's
            // terminal CANCELED arbitrates as superseded instead of
            // terminalizing the record the live replacement continues.
            if (!record.legs.empty()) {
                std::string newest_unknown;
                for (const auto& entry : raced) {
                    if (!entry.exchange_order_id.empty() &&
                        !leg_index_of(record, entry.exchange_order_id).has_value()) {
                        newest_unknown = entry.exchange_order_id;
                    }
                }
                if (!newest_unknown.empty()) {
                    note_exchange_id(record, newest_unknown);
                    ++stats_.legs_discovered;
                    note_discovered_leg(record.client_order_id, newest_unknown);
                }
            }
            // Buffered reports are real venue observations and still apply
            // under the (possibly just extended) leg table: the old leg's
            // own reports arbitrate normally, the adopted replacement
            // drives the lifecycle.
            for (const auto& entry : raced) {
                apply_arbitrated_report(record, entry);
            }
            return placement.error();
        }

        record.price = *wire.new_price;
        record.quantity = *wire.new_quantity;
        // The venue acked the amend: append the new leg. CancelReplace
        // venues (Binance) carry a NEW exchangeOrderId; in-place amend
        // venues (OKX) echo the existing one — the version still
        // advances. The record was Live/PartiallyFilled throughout (all
        // racing reports were buffered), so no resurrection is needed.
        append_amend_leg(record, placement.value().exchange_order_id);
        const auto persist_error = append_or_error(amended_event(record));
        if (persist_error.has_value()) {
            ++stats_.log_write_failures;
            return Error{"persistence", persist_error->message +
                                            " (the amend was accepted by the venue; "
                                            "reconcile or an execution report converges it)"};
        }
        // reports that raced the venue ack apply after the amended event
        // exists, keeping the log replay-correct (amended -> state): the
        // old leg contributes fills only, the new leg drives the state
        // (an immediate full fill lands Filled, not a zombie Live).
        for (const auto& entry : raced) {
            apply_arbitrated_report(record, entry);
        }
        return record;
    }
}

auto OrderManagementSystem::query(std::string_view a_client_order_id) -> Result<OrderRecord>
{
    const std::lock_guard lock(mutex_);
    const auto it = lookup(a_client_order_id);
    if (it == orders_.end()) {
        return Error{"not_found", "unknown clientOrderId " + std::string{a_client_order_id}};
    }
    return it->second;
}

auto OrderManagementSystem::all_orders() -> std::vector<OrderRecord>
{
    const std::lock_guard lock(mutex_);
    std::vector<OrderRecord> records;
    records.reserve(orders_.size());
    for (const auto& [id, record] : orders_) {
        records.push_back(record);
    }
    std::sort(records.begin(), records.end(),
              [](const OrderRecord& a_lhs, const OrderRecord& a_rhs) {
                  return a_lhs.client_order_id < a_rhs.client_order_id;
              });
    return records;
}

auto OrderManagementSystem::get_price(const std::string& a_symbol,
                                      std::string_view a_venue) -> Result<PriceQuote>
{
    const std::string venue{a_venue.empty() ? default_venue_ : std::string{a_venue}};
    const auto connector = connector_for(venue);
    if (connector == nullptr) {
        std::string names;
        for (const auto& [key, entry] : connectors_) {
            if (!names.empty()) {
                names += ", ";
            }
            names += key;
        }
        return Error{"invalid_request",
                     "unsupported venue \"" + venue + "\" (configured: " + names + ")"};
    }
    const auto price = connector->get_price(a_symbol);
    if (!price.is_ok()) {
        return price.error();
    }
    return PriceQuote{.venue = venue, .price = price.value()};
}

auto OrderManagementSystem::apply_observation(OrderRecord& a_record, OrderState a_state,
                                              std::string_view a_filled,
                                              std::string_view a_avg_price,
                                              std::string_view a_price, std::string_view a_quantity,
                                              bool a_apply_lifecycle) -> bool
{
    bool changed = false;

    if (!a_filled.empty()) {
        const auto reported = parse_decimal(a_filled);
        const auto current =
            parse_decimal(a_record.filled_quantity.empty() ? "0" : a_record.filled_quantity);
        if (reported.is_ok() && current.is_ok() && compare(reported.value(), current.value()) > 0) {
            a_record.filled_quantity = std::string{a_filled};
            if (!a_avg_price.empty()) {
                a_record.average_fill_price = std::string{a_avg_price};
            }
            changed = true;
        }
    }

    if (a_apply_lifecycle) {
        if (a_state != a_record.state) {
            if (apply_transition(a_record.state, a_state) == TransitionResult::Applied) {
                changed = true;
            }
        }

        // Snapshots (REST / reconciliation) are authoritative for the current
        // price/quantity; WS reports leave them untouched (empty view here).
        if (!a_price.empty() && a_price != a_record.price) {
            a_record.price = std::string{a_price};
            changed = true;
        }
        if (!a_quantity.empty() && a_quantity != a_record.quantity) {
            a_record.quantity = std::string{a_quantity};
            changed = true;
        }
    }
    return changed;
}

void OrderManagementSystem::note_exchange_id(OrderRecord& a_record, const std::string& a_id)
{
    if (a_id.empty() || a_id == a_record.exchange_order_id) {
        return;
    }
    a_record.exchange_order_id = a_id;
    // backfill/adoption path: the observed id is newest known truth
    a_record.legs.push_back(
        OrderLeg{.version = a_record.legs.size() + 1, .exchange_order_id = a_id});
}

void OrderManagementSystem::append_amend_leg(OrderRecord& a_record, const std::string& a_id)
{
    if (a_id.empty()) {
        return; // defensive: an ack without an id leaves the leg table alone
    }
    // The amend ack always advances the version, even when the venue
    // echoes the current id (in-place amend venues): the leg table is the
    // amend ledger first, the id mapping second.
    a_record.legs.push_back(
        OrderLeg{.version = a_record.legs.size() + 1, .exchange_order_id = a_id});
    a_record.exchange_order_id = a_id;
}

auto OrderManagementSystem::leg_index_of(const OrderRecord& a_record, const std::string& a_id) const
    -> std::optional<std::size_t>
{
    std::optional<std::size_t> match;
    for (std::size_t index = 0; index < a_record.legs.size(); ++index) {
        if (a_record.legs[index].exchange_order_id == a_id) {
            match = index; // keep scanning: the NEWEST matching leg wins
        }
    }
    return match;
}

void OrderManagementSystem::note_discovered_leg(std::string a_client_order_id,
                                                std::string a_exchange_order_id)
{
    constexpr std::size_t kMaxDiscoveredLegNotes = 256;
    discovered_leg_notes_.emplace_back(std::move(a_client_order_id),
                                       std::move(a_exchange_order_id));
    while (discovered_leg_notes_.size() > kMaxDiscoveredLegNotes) {
        discovered_leg_notes_.pop_front();
    }
}

auto OrderManagementSystem::drain_raced_reports(const std::string& a_client_order_id)
    -> std::vector<ExecutionReport>
{
    std::vector<ExecutionReport> raced;
    if (const auto flying = raced_reports_.find(a_client_order_id);
        flying != raced_reports_.end()) {
        raced = std::move(flying->second);
        raced_reports_.erase(flying);
    }
    return raced;
}

void OrderManagementSystem::apply_report(OrderRecord& a_record, const ExecutionReport& a_report,
                                         bool a_apply_lifecycle)
{
    // The first observation resolving a Pending record proves the venue
    // saw the order: learn its exchangeOrderId (backfill only — a known
    // id reflects the amend lifecycle and is never overwritten here).
    if (a_record.exchange_order_id.empty()) {
        note_exchange_id(a_record, a_report.exchange_order_id);
    }
    if (apply_observation(a_record, a_report.state, a_report.filled_quantity,
                          a_report.average_fill_price, "", "", a_apply_lifecycle)) {
        ++stats_.reports_applied;
        append_event(state_event(a_record));
    } else {
        ++stats_.reports_stale;
    }
}

void OrderManagementSystem::apply_arbitrated_report(OrderRecord& a_record,
                                                    const ExecutionReport& a_report)
{
    // Venue-lifecycle arbitration by the leg table. cancelReplace venues
    // (Binance) emit reports for BOTH legs of an amend under the same
    // clientOrderId: the replaced order's CANCELED must not terminalize
    // the live replacement. Rules (no report is ignored):
    // - report id empty, current leg, or unknown to the table: full
    //   lifecycle application (an unknown id means the venue knows a leg
    //   we don't — a lost amend ack — and is adopted as the newest leg,
    //   counted and surfaced by audit())
    // - report id belongs to a superseded leg: fills still count (real
    //   executions, monotonic high-water mark), state/price/quantity
    //   do not
    bool apply_lifecycle = true;
    if (!a_report.exchange_order_id.empty() && !a_record.legs.empty()) {
        const auto match = leg_index_of(a_record, a_report.exchange_order_id);
        if (!match.has_value()) {
            note_exchange_id(a_record, a_report.exchange_order_id);
            ++stats_.legs_discovered;
            note_discovered_leg(a_record.client_order_id, a_report.exchange_order_id);
        } else if (*match + 1 < a_record.legs.size()) {
            apply_lifecycle = false;
        }
    }
    apply_report(a_record, a_report, apply_lifecycle);
}

void OrderManagementSystem::on_execution_report(const ExecutionReport& a_report)
{
    const std::lock_guard lock(mutex_);
    // A place or amend whose venue outcome has not landed yet: buffer the
    // report so it applies (and persists) right after that outcome, in
    // replay-correct order (place_submitted -> place_accepted -> state;
    // amended -> state) and under the then-current leg table.
    if (const auto flying = raced_reports_.find(a_report.client_order_id);
        flying != raced_reports_.end()) {
        flying->second.push_back(a_report);
        ++stats_.reports_buffered;
        return;
    }
    const auto it = lookup(a_report.client_order_id);
    if (it == orders_.end()) {
        ++stats_.reports_unknown;
        return;
    }
    apply_arbitrated_report(it->second, a_report);
}

void OrderManagementSystem::record_from_snapshot(const OrderSnapshot& a_snapshot,
                                                 const std::string& a_venue, bool a_adopted)
{
    OrderRecord record{.client_order_id = a_snapshot.client_order_id,
                       .symbol = a_snapshot.instrument_id,
                       .venue = a_venue,
                       .exchange_order_id = a_snapshot.exchange_order_id,
                       .side = a_snapshot.side,
                       .type = a_snapshot.type,
                       .time_in_force = "GTC",
                       .price = a_snapshot.price,
                       .quantity = a_snapshot.quantity,
                       .state = a_snapshot.state,
                        .filled_quantity =
                            a_snapshot.filled_quantity.empty() ? "0" : a_snapshot.filled_quantity,
                        .average_fill_price = a_snapshot.average_fill_price,
                        .legs = {},
                        .adopted = a_adopted,
                        .rejection = std::nullopt};
    if (!a_snapshot.exchange_order_id.empty()) {
        record.legs.push_back(OrderLeg{.version = 1,
                                       .exchange_order_id = a_snapshot.exchange_order_id});
    }
    if (a_adopted) {
        append_event(adopted_event(record));
    } else {
        append_event(place_accepted_event(record));
    }
    orders_[record.client_order_id] = std::move(record);
}

auto OrderManagementSystem::reconcile() -> ReconcileReport
{
    ReconcileReport report;

    // ---- Phase A: per venue, adopt venue-live orders missing locally ----
    // Venue listings happen WITHOUT the lock (slow I/O must not block
    // report application); adoption decisions re-check under it.
    for (const auto& [venue, connector] : connectors_) {
        const auto pending = connector->get_open_orders();
        if (!pending.is_ok()) {
            report.pending_listing_failed = true;
            continue;
        }
        const std::lock_guard lock(mutex_);
        for (const auto& snapshot : pending.value()) {
            if (snapshot.client_order_id.empty() ||
                raced_reports_.find(snapshot.client_order_id) != raced_reports_.end()) {
                // a place whose venue call is still open owns this id: its
                // own ack/report path records the outcome (adopting or
                // updating here would race it)
                continue;
            }
            const auto it = lookup(snapshot.client_order_id);
            if (it == orders_.end()) {
                record_from_snapshot(snapshot, venue, true);
                ++report.adopted;
            } else {
                // a snapshot resolving a Pending entry carries the
                // venue's id: backfill it once (known ids are never
                // overwritten — they follow the amend lifecycle)
                bool changed = false;
                if (it->second.exchange_order_id.empty() && !snapshot.exchange_order_id.empty()) {
                    note_exchange_id(it->second, snapshot.exchange_order_id);
                    changed = true;
                }
                if (apply_observation(it->second, snapshot.state, snapshot.filled_quantity,
                                      snapshot.average_fill_price, snapshot.price,
                                      snapshot.quantity)) {
                    changed = true;
                }
                if (changed) {
                    ++report.updated;
                    append_event(state_event(it->second));
                }
            }
        }
    }

    // ---- Phase B: resolve every non-terminal registry entry ----
    // Snapshot the worklist under the lock; per-order venue lookups and
    // their application each take the lock separately. Ids whose venue
    // call is still open are skipped: their own place path records the
    // outcome (a snapshot applied here could precede the place_accepted
    // event and break replay ordering).
    struct WorkItem
    {
        std::string id;
        std::string venue;
        OrderQuery query;
    };
    std::vector<WorkItem> open;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [id, record] : orders_) {
            if (!is_terminal(record.state) && raced_reports_.find(id) == raced_reports_.end()) {
                const std::string& venue = record.venue.empty() ? default_venue_ : record.venue;
                open.push_back(WorkItem{id, venue, OrderQuery{id, record.symbol}});
            }
        }
    }
    for (const auto& item : open) {
        const auto connector = connector_for(item.venue);
        if (connector == nullptr) {
            ++report.unresolved;
            continue;
        }
        const auto snapshot = connector->get_order(item.query);
        const std::lock_guard lock(mutex_);
        const auto it = lookup(item.id);
        if (it == orders_.end()) {
            continue; // removed while unlocked (nothing erases today; defensive)
        }
        OrderRecord& record = it->second;
        if (snapshot.is_ok() && snapshot.value().has_value()) {
            bool changed = false;
            if (record.exchange_order_id.empty() && !snapshot.value()->exchange_order_id.empty()) {
                note_exchange_id(record, snapshot.value()->exchange_order_id);
                changed = true;
            } else if (!snapshot.value()->exchange_order_id.empty() &&
                       !leg_index_of(record, snapshot.value()->exchange_order_id).has_value()) {
                // The venue snapshot carries an id no leg knows: an amend
                // whose ack never resolved (transport-unresolved). Adopt
                // it as the newest leg so reports arbitrate correctly.
                note_exchange_id(record, snapshot.value()->exchange_order_id);
                ++stats_.legs_discovered;
                note_discovered_leg(record.client_order_id, snapshot.value()->exchange_order_id);
                changed = true;
            }
            if (apply_observation(record, snapshot.value()->state,
                                  snapshot.value()->filled_quantity,
                                  snapshot.value()->average_fill_price, snapshot.value()->price,
                                  snapshot.value()->quantity)) {
                changed = true;
            }
            if (changed) {
                append_event(state_event(record));
                if (is_terminal(record.state)) {
                    ++report.terminal_resolved;
                } else {
                    ++report.updated;
                }
            }
            continue;
        }
        // nullopt (venue-agnostic "order does not exist") plus the OKX
        // legacy codes are a conclusive absence; transport/protocol errors
        // stay unresolved.
        const bool absent = snapshot.is_ok() || (snapshot.error().code == "venue:51603" ||
                                                 snapshot.error().code == "venue:51016");
        if (absent) {
            // The venue conclusively does not know the order: it can never
            // fill; terminal Rejected keeps the clientOrderId deterministic.
            if (apply_transition(record.state, OrderState::Rejected) == TransitionResult::Applied) {
                record.rejection =
                    Error{"venue_absent", "order is unknown to the venue after reconciliation"};
                append_event(state_event(record));
                append_event(rejected_event(record));
            }
            ++report.absent_rejected;
        } else {
            // Unreachable venue (or inconclusive lookup): keep the entry
            // as-is; later reports or reconciliations correct it.
            ++report.unresolved;
        }
    }
    return report;
}

auto OrderManagementSystem::load_from_log() -> Result<EventLog::ReplayStats>
{
    if (log_ == nullptr) {
        return EventLog::ReplayStats{};
    }
    const std::lock_guard lock(mutex_);
    return EventLog::replay(log_->path(),
                            [this](const nlohmann::json& a_event) { apply_log_event(a_event); });
}

void OrderManagementSystem::apply_log_event(const nlohmann::json& a_event)
{
    const auto type = event_string(a_event, "type");
    const auto client_order_id = event_string(a_event, "clientOrderId");
    if (!type.has_value() || !client_order_id.has_value()) {
        return; // replay is schema-validated defensively: skip junk
    }
    const auto event_version = [&a_event]() -> std::uint64_t {
        const auto it = a_event.find("version");
        if (it != a_event.end() && it->is_number_unsigned()) {
            return it->get<std::uint64_t>();
        }
        return 0; // legacy event: no explicit version
    }();

    if (*type == "place_submitted" || *type == "place_accepted" || *type == "adopted") {
        const auto symbol = event_string(a_event, "symbol").value_or("");
        const auto venue = event_string(a_event, "venue").value_or(default_venue_);
        // place_submitted carries no exchangeOrderId (no ack yet); when a
        // place_accepted follows in the log it overwrites this record.
        const auto exchange_order_id = *type == "place_submitted"
                                           ? std::string{}
                                           : event_string(a_event, "exchangeOrderId").value_or("");
        const auto side =
            parse_side(event_string(a_event, "side").value_or("buy")).value_or(Side::Buy);
        const auto order_type = parse_type(event_string(a_event, "orderType").value_or("limit"))
                                    .value_or(OrderType::Limit);
        OrderRecord record{.client_order_id = *client_order_id,
                           .symbol = symbol,
                           .venue = venue,
                           .exchange_order_id = exchange_order_id,
                           .side = side,
                           .type = order_type,
                           .time_in_force = event_string(a_event, "timeInForce").value_or("GTC"),
                           .price = event_string(a_event, "price").value_or(""),
                           .quantity = event_string(a_event, "quantity").value_or(""),
                           .state = OrderState::Live,
                           .filled_quantity = "0",
                           .average_fill_price = "",
                           .legs = {},
                           .adopted = *type == "adopted",
                           .rejection = std::nullopt};
        if (*type == "place_submitted") {
            // torn write / crash before the venue ack: the entry replays
            // Pending and startup reconciliation resolves it (venue
            // snapshot forward, conclusive absence -> Rejected,
            // unreachable -> kept pending).
            record.state = OrderState::Pending;
        }
        if (!exchange_order_id.empty()) {
            record.legs.push_back(OrderLeg{.version = event_version == 0 ? 1 : event_version,
                                               .exchange_order_id = exchange_order_id});
        }
        if (*type == "adopted") {
            record.state = parse_state(event_string(a_event, "state").value_or("live"))
                               .value_or(OrderState::Live);
            record.filled_quantity = event_string(a_event, "filledQuantity").value_or("0");
            record.average_fill_price = event_string(a_event, "averageFillPrice").value_or("");
        }
        orders_[*client_order_id] = std::move(record);
        return;
    }

    if (*type == "rejected") {
        OrderRecord& record = orders_[*client_order_id];
        record.client_order_id = *client_order_id;
        record.state = OrderState::Rejected;
        record.symbol = event_string(a_event, "symbol").value_or(record.symbol);
        record.venue = event_string(a_event, "venue")
                           .value_or(record.venue.empty() ? default_venue_ : record.venue);
        record.rejection = Error{event_string(a_event, "code").value_or("rejected"),
                                 event_string(a_event, "reason").value_or("")};
        return;
    }

    if (*type == "amended") {
        const auto it = lookup(*client_order_id);
        if (it == orders_.end()) {
            return;
        }
        const auto price = event_string(a_event, "price");
        const auto quantity = event_string(a_event, "quantity");
        if (price.has_value() && !price->empty()) {
            it->second.price = *price;
        }
        if (quantity.has_value() && !quantity->empty()) {
            it->second.quantity = *quantity;
        }
        const auto exchange_order_id = event_string(a_event, "exchangeOrderId").value_or("");
        if (event_version == 0) {
            // Legacy event (pre versioned legs): infer the append and
            // keep the historic Canceled->Live resurrection — old logs
            // contain the amend-race shape the runtime no longer writes.
            note_exchange_id(it->second, exchange_order_id);
            const auto state = parse_state(event_string(a_event, "state").value_or(""));
            if (state.has_value() && it->second.state == OrderState::Canceled &&
                *state == OrderState::Live) {
                it->second.state = OrderState::Live;
            }
            return;
        }
        // Versioned replay: in-order logs append leg after leg; a repeat
        // of a known version updates it (idempotent re-reads).
        if (event_version <= it->second.legs.size()) {
            auto& leg = it->second.legs[event_version - 1];
            if (!exchange_order_id.empty()) {
                leg.exchange_order_id = exchange_order_id;
            }
        } else {
            it->second.legs.push_back(OrderLeg{.version = event_version,
                                               .exchange_order_id = exchange_order_id});
        }
        if (!exchange_order_id.empty()) {
            it->second.exchange_order_id = exchange_order_id;
        }
        return;
    }

    if (*type == "state") {
        const auto it = lookup(*client_order_id);
        if (it == orders_.end()) {
            return;
        }
        const auto state = parse_state(event_string(a_event, "state").value_or(""));
        if (!state.has_value()) {
            return;
        }
        apply_observation(it->second, *state, event_string(a_event, "filledQuantity").value_or(""),
                          event_string(a_event, "averageFillPrice").value_or(""));
        // events written after a Pending entry was resolved by an
        // observation carry the venue's id; backfill it once (older
        // logs have no such field)
        if (it->second.exchange_order_id.empty()) {
            note_exchange_id(it->second, event_string(a_event, "exchangeOrderId").value_or(""));
        }
        return;
    }
}

void OrderManagementSystem::audit_record_local(const OrderRecord& a_record,
                                               std::vector<ConsistencyAlert>& a_alerts) const
{
    // Leg chain: versions must be a contiguous 1..N sequence.
    for (std::size_t index = 0; index < a_record.legs.size(); ++index) {
        if (a_record.legs[index].version != index + 1) {
            a_alerts.push_back(ConsistencyAlert{
                a_record.client_order_id,
                "version_gap",
                "leg " + std::to_string(index + 1) + " carries version " +
                    std::to_string(a_record.legs[index].version) + "; expected a contiguous 1.." +
                    std::to_string(a_record.legs.size()) + " chain"});
            break;
        }
    }
    if (!a_record.legs.empty() &&
        a_record.legs.back().exchange_order_id != a_record.exchange_order_id) {
        a_alerts.push_back(ConsistencyAlert{
            a_record.client_order_id,
            "version_gap",
            "current exchangeOrderId " + a_record.exchange_order_id + " != newest leg id " +
                a_record.legs.back().exchange_order_id});
    }

    // Fill high-water mark vs lifecycle state.
    const auto filled = parse_decimal(a_record.filled_quantity.empty() ? "0"
                                                                       : a_record.filled_quantity);
    const auto quantity = parse_decimal(a_record.quantity);
    if (!filled.is_ok() || !quantity.is_ok() || is_zero(quantity.value())) {
        return;
    }
    const auto relation = compare(filled.value(), quantity.value());
    if (a_record.state != OrderState::Filled && relation >= 0) {
        a_alerts.push_back(ConsistencyAlert{
            a_record.client_order_id,
            "fill_state_mismatch",
            "filledQuantity " + a_record.filled_quantity + " reached quantity " +
                a_record.quantity + " but state is " + std::string{to_string(a_record.state)}});
    } else if (a_record.state == OrderState::Filled && relation < 0) {
        a_alerts.push_back(ConsistencyAlert{
            a_record.client_order_id,
            "fill_state_mismatch",
            "state is filled but filledQuantity " + a_record.filled_quantity +
                " is below quantity " + a_record.quantity});
    }
}

void OrderManagementSystem::audit_record_vs_snapshot(
    const OrderRecord& a_record, const OrderSnapshot& a_snapshot,
    std::vector<ConsistencyAlert>& a_alerts) const
{
    if (!a_snapshot.exchange_order_id.empty() &&
        !leg_index_of(a_record, a_snapshot.exchange_order_id).has_value()) {
        a_alerts.push_back(ConsistencyAlert{
            a_record.client_order_id,
            "unknown_leg_report",
            "venue snapshot carries exchangeOrderId " + a_snapshot.exchange_order_id +
                " absent from the leg table (reconcile adopts it on the next pass)"});
    }
    if (a_snapshot.state != a_record.state) {
        a_alerts.push_back(ConsistencyAlert{
            a_record.client_order_id,
            "state_mismatch",
            "local " + std::string{to_string(a_record.state)} + ", venue " +
                std::string{to_string(a_snapshot.state)}});
    }
    const auto compare_decimal = [](const std::string& a_local, const std::string& a_venue)
        -> std::optional<int> {
        const auto local = parse_decimal(a_local.empty() ? "0" : a_local);
        const auto venue = parse_decimal(a_venue.empty() ? "0" : a_venue);
        if (!local.is_ok() || !venue.is_ok()) {
            return std::nullopt; // unparseable values are not evidence of drift
        }
        return compare(local.value(), venue.value());
    };
    if (const auto relation = compare_decimal(a_record.filled_quantity,
                                              a_snapshot.filled_quantity);
        relation.has_value() && *relation != 0) {
        a_alerts.push_back(ConsistencyAlert{
            a_record.client_order_id,
            "fill_mismatch",
            "local filledQuantity " + a_record.filled_quantity + ", venue " +
                a_snapshot.filled_quantity});
    }
    if (const auto relation = compare_decimal(a_record.price, a_snapshot.price);
        relation.has_value() && *relation != 0) {
        a_alerts.push_back(ConsistencyAlert{a_record.client_order_id, "price_qty_mismatch",
                                            "local price " + a_record.price + ", venue " +
                                                a_snapshot.price});
    }
    if (const auto relation = compare_decimal(a_record.quantity, a_snapshot.quantity);
        relation.has_value() && *relation != 0) {
        a_alerts.push_back(ConsistencyAlert{a_record.client_order_id, "price_qty_mismatch",
                                            "local quantity " + a_record.quantity + ", venue " +
                                                a_snapshot.quantity});
    }
}

auto OrderManagementSystem::audit() -> AuditReport
{
    AuditReport report;

    struct WorkItem
    {
        std::string id;
        std::string venue;
        OrderQuery query;
    };
    std::vector<WorkItem> open;

    { // ---- local invariants + worklist snapshot under the lock ----
        const std::lock_guard lock(mutex_);
        report.orders_checked = orders_.size();
        for (const auto& [id, record] : orders_) {
            audit_record_local(record, report.alerts);
            // ids with an open place/amend venue call own themselves: an
            // in-flight amend's snapshot could legally differ from the
            // not-yet-applied outcome
            if (!is_terminal(record.state) && raced_reports_.find(id) == raced_reports_.end()) {
                const std::string& venue = record.venue.empty() ? default_venue_ : record.venue;
                open.push_back(WorkItem{id, venue, OrderQuery{id, record.symbol}});
            }
        }
        for (const auto& [client_order_id, exchange_order_id] : discovered_leg_notes_) {
            report.alerts.push_back(ConsistencyAlert{
                client_order_id,
                "unknown_leg_report",
                "execution report carried exchangeOrderId " + exchange_order_id +
                    " absent from the leg table; adopted as a new leg (lost amend ack?)"});
        }
        discovered_leg_notes_.clear();
    }

    // ---- venue truth per non-terminal order, lock only for comparing ----
    for (const auto& item : open) {
        const auto connector = connector_for(item.venue);
        if (connector == nullptr) {
            ++report.lookup_failures;
            report.alerts.push_back(ConsistencyAlert{item.id, "lookup_failed",
                                                     "no connector for venue \"" + item.venue +
                                                         "\""});
            continue;
        }
        const auto snapshot = connector->get_order(item.query);
        ++report.venue_lookups;

        const std::lock_guard lock(mutex_);
        const auto it = lookup(item.id);
        if (it == orders_.end()) {
            continue; // removed while unlocked (nothing erases today; defensive)
        }
        if (!snapshot.is_ok()) {
            ++report.lookup_failures;
            report.alerts.push_back(ConsistencyAlert{
                item.id, "lookup_failed",
                snapshot.error().code + ": " + snapshot.error().message});
            continue;
        }
        if (!snapshot.value().has_value()) {
            report.alerts.push_back(ConsistencyAlert{
                item.id,
                "venue_unknown",
                "non-terminal locally (" + std::string{to_string(it->second.state)} +
                    ") but the venue does not know the order"});
            continue;
        }
        audit_record_vs_snapshot(it->second, *snapshot.value(), report.alerts);
    }
    return report;
}

} // namespace gateway
