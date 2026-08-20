#include "core/oms.hpp"

#include "core/decimal.hpp"

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
            {"exchangeOrderId", a_record.exchange_order_id}};
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
            {"quantity", a_record.quantity}};
}

auto state_event(const OrderRecord& a_record) -> nlohmann::json
{
    return {{"type", "state"},
            {"clientOrderId", a_record.client_order_id},
            {"state", state_to_string(a_record.state)},
            {"filledQuantity", a_record.filled_quantity},
            {"averageFillPrice", a_record.average_fill_price}};
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
        // outstanding quantity of working orders may still execute
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
    const std::lock_guard lock(mutex_);

    if (const auto existing = lookup(a_request.client_order_id); existing != orders_.end()) {
        if (existing->second.rejection.has_value()) {
            return existing->second.rejection.value();
        }
        return PlaceOutcome{.record = existing->second, .replayed = true};
    }

    std::string venue{a_venue.empty() ? default_venue_ : a_venue};
    ExchangeConnector* connector = connector_for(venue);
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
                          .state = OrderState::Live,
                          .filled_quantity = "0",
                          .average_fill_price = "",
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

    // ---- venue routing ----
    const auto placement = connector->place_order(a_request);
    if (!placement.is_ok()) {
        if (placement.error().code != "transport") {
            // definitive venue rejection: terminal Rejected, replayed to
            // idempotent retries
            candidate.state = OrderState::Rejected;
            candidate.rejection = placement.error();
            const auto persist_error = append_or_error(rejected_event(candidate));
            orders_[candidate.client_order_id] = candidate;
            if (persist_error.has_value()) {
                ++stats_.log_write_failures;
            }
        }
        return placement.error();
    }

    candidate.exchange_order_id = placement.value().exchange_order_id;
    const auto persist_error = append_or_error(place_accepted_event(candidate));
    orders_[candidate.client_order_id] = candidate;
    if (persist_error.has_value()) {
        ++stats_.log_write_failures;
        return Error{"persistence", persist_error->message +
                                        " (order was accepted by the venue; retry replays it)"};
    }
    return PlaceOutcome{.record = candidate, .replayed = false};
}

auto OrderManagementSystem::cancel(std::string_view a_client_order_id) -> Result<OrderRecord>
{
    const std::lock_guard lock(mutex_);

    const auto it = lookup(a_client_order_id);
    if (it == orders_.end()) {
        return Error{"not_found", "unknown clientOrderId " + std::string{a_client_order_id}};
    }
    OrderRecord& record = it->second;
    if (record.state == OrderState::Canceled) {
        return record; // idempotent cancel
    }
    if (is_terminal(record.state)) {
        return Error{"order_terminal", "order " + record.client_order_id + " is " +
                                           std::string{state_to_string(record.state)}};
    }

    ExchangeConnector* connector = connector_for(record);
    if (connector == nullptr) {
        return Error{"internal", "no connector for venue \"" + record.venue + "\""};
    }

    const auto placement =
        connector->cancel_order(CancelRequest{record.client_order_id, record.symbol});
    if (!placement.is_ok()) {
        return placement.error();
    }
    if (!record.exchange_order_id.empty() && !placement.value().exchange_order_id.empty()) {
        record.exchange_order_id = placement.value().exchange_order_id;
    }
    // A successful venue cancel is definitive for a working order; the
    // Live/PartiallyFilled -> Canceled steps are legal by precondition.
    if (apply_transition(record.state, OrderState::Canceled) == TransitionResult::Applied) {
        append_event(state_event(record));
    }
    return record;
}

auto OrderManagementSystem::amend(const AmendCommand& a_command) -> Result<OrderRecord>
{
    if (!a_command.new_price.has_value() && !a_command.new_quantity.has_value()) {
        return Error{"protocol", "amend requires a new price and/or quantity"};
    }

    const std::lock_guard lock(mutex_);

    const auto it = lookup(a_command.client_order_id);
    if (it == orders_.end()) {
        return Error{"not_found", "unknown clientOrderId " + a_command.client_order_id};
    }
    OrderRecord& record = it->second;
    if (is_terminal(record.state)) {
        return Error{"order_terminal", "order " + record.client_order_id + " is " +
                                           std::string{state_to_string(record.state)}};
    }
    ExchangeConnector* connector = connector_for(record);
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

    const auto placement =
        connector->amend_order(AmendRequest{.client_order_id = record.client_order_id,
                                            .instrument_id = record.symbol,
                                            .new_price = new_price,
                                            .new_quantity = new_quantity,
                                            .side = record.side,
                                            .type = record.type,
                                            .time_in_force = record.time_in_force});
    if (!placement.is_ok()) {
        return placement.error();
    }
    record.price = new_price;
    record.quantity = new_quantity;
    // Venues whose amend is a cancel+replace (Binance) issue a NEW
    // exchangeOrderId for the replacement; in-place amend venues (OKX)
    // echo the existing one.
    if (!placement.value().exchange_order_id.empty()) {
        record.exchange_order_id = placement.value().exchange_order_id;
    }
    append_event(amended_event(record));
    return record;
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

auto OrderManagementSystem::apply_observation(OrderRecord& a_record, OrderState a_state,
                                              std::string_view a_filled,
                                              std::string_view a_avg_price,
                                              std::string_view a_price,
                                              std::string_view a_quantity) -> bool
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
    return changed;
}

void OrderManagementSystem::on_execution_report(const ExecutionReport& a_report)
{
    const std::lock_guard lock(mutex_);
    const auto it = lookup(a_report.client_order_id);
    if (it == orders_.end()) {
        ++stats_.reports_unknown;
        return;
    }
    if (apply_observation(it->second, a_report.state, a_report.filled_quantity,
                          a_report.average_fill_price)) {
        ++stats_.reports_applied;
        append_event(state_event(it->second));
    } else {
        ++stats_.reports_stale;
    }
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
                       .adopted = a_adopted,
                       .rejection = std::nullopt};
    if (a_adopted) {
        append_event(adopted_event(record));
    } else {
        append_event(place_accepted_event(record));
    }
    orders_[record.client_order_id] = std::move(record);
}

auto OrderManagementSystem::reconcile() -> ReconcileReport
{
    const std::lock_guard lock(mutex_);
    ReconcileReport report;

    // ---- Phase A: per venue, adopt venue-live orders missing locally ----
    for (const auto& [venue, connector] : connectors_) {
        const auto pending = connector->get_open_orders();
        if (!pending.is_ok()) {
            report.pending_listing_failed = true;
            continue;
        }
        for (const auto& snapshot : pending.value()) {
            if (snapshot.client_order_id.empty()) {
                continue;
            }
            const auto it = lookup(snapshot.client_order_id);
            if (it == orders_.end()) {
                record_from_snapshot(snapshot, venue, true);
                ++report.adopted;
            } else if (apply_observation(it->second, snapshot.state, snapshot.filled_quantity,
                                         snapshot.average_fill_price, snapshot.price,
                                         snapshot.quantity)) {
                ++report.updated;
                append_event(state_event(it->second));
            }
        }
    }

    // ---- Phase B: resolve every non-terminal registry entry ----
    std::vector<std::pair<std::string, std::string>> open;
    for (const auto& [id, record] : orders_) {
        if (!is_terminal(record.state)) {
            open.emplace_back(id, record.venue);
        }
    }
    for (const auto& [id, venue] : open) {
        OrderRecord& record = orders_.at(id);
        ExchangeConnector* connector = connector_for(record);
        if (connector == nullptr) {
            ++report.unresolved;
            continue;
        }
        const auto snapshot = connector->get_order(OrderQuery{id, record.symbol});
        if (snapshot.is_ok() && snapshot.value().has_value()) {
            if (apply_observation(record, snapshot.value()->state,
                                  snapshot.value()->filled_quantity,
                                  snapshot.value()->average_fill_price, snapshot.value()->price,
                                  snapshot.value()->quantity)) {
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

    if (*type == "place_accepted" || *type == "adopted") {
        const auto symbol = event_string(a_event, "symbol").value_or("");
        const auto venue = event_string(a_event, "venue").value_or(default_venue_);
        const auto exchange_order_id = event_string(a_event, "exchangeOrderId").value_or("");
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
                           .adopted = *type == "adopted",
                           .rejection = std::nullopt};
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
        return;
    }
}

} // namespace gateway
