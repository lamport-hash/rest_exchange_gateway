#pragma once

#include "gateway/exchange_connector.hpp"

#include <array>
#include <utility>

namespace gateway {

/// Explicit legal-transition table of the normalized order lifecycle.
///
/// Semantics:
/// - An order is born Pending (place staged/sent, venue has not acked) or
///   Rejected (pre-trade risk rejection). Pending is gateway-local: venue
///   observations never carry it.
/// - Pending resolves forward only: to Live once the venue acks
///   (exchangeOrderId known), directly to PartiallyFilled/Filled when an
///   execution report races the ack (skipping Live), or to Rejected on a
///   definitive venue rejection / restart reconciliation proving the
///   venue never saw the order. Pending -> Canceled is ILLEGAL (you
///   cannot cancel what the venue has not accepted).
/// - Live/PartiallyFilled move forward to PartiallyFilled/Filled/Canceled;
///   they may also be moved to Rejected only by reconciliation (the venue
///   no longer knows the order) — no venue execution report ever carries
///   Rejected for an accepted order.
/// - Filled/Canceled/Rejected are terminal: no outgoing transitions.
/// - PartiallyFilled -> Live is a regression: rejected as stale.
///
/// Every (from, to) pair not listed is illegal. Callers observing venue
/// data (REST snapshots, WS execution reports) must reject illegal
/// transitions instead of applying them — that is exactly the
/// out-of-order / duplicate / REST-vs-WS race arbitration: late or
/// duplicated observations against a terminal or further-advanced state
/// simply do not apply.
inline constexpr std::array<std::pair<OrderState, OrderState>, 14> kLegalTransitions{{
    {OrderState::Pending, OrderState::Pending},
    {OrderState::Pending, OrderState::Live},
    {OrderState::Pending, OrderState::PartiallyFilled},
    {OrderState::Pending, OrderState::Filled},
    {OrderState::Pending, OrderState::Rejected},
    {OrderState::Live, OrderState::Live},
    {OrderState::Live, OrderState::PartiallyFilled},
    {OrderState::Live, OrderState::Filled},
    {OrderState::Live, OrderState::Canceled},
    {OrderState::Live, OrderState::Rejected},
    {OrderState::PartiallyFilled, OrderState::PartiallyFilled},
    {OrderState::PartiallyFilled, OrderState::Filled},
    {OrderState::PartiallyFilled, OrderState::Canceled},
    {OrderState::PartiallyFilled, OrderState::Rejected},
}};

[[nodiscard]] constexpr auto is_terminal(OrderState a_state) -> bool
{
    return a_state == OrderState::Filled || a_state == OrderState::Canceled ||
           a_state == OrderState::Rejected;
}

/// True when (a_from -> a_to) appears in kLegalTransitions.
[[nodiscard]] constexpr auto can_transition(OrderState a_from, OrderState a_to) -> bool
{
    for (const auto& transition : kLegalTransitions) {
        if (transition.first == a_from && transition.second == a_to) {
            return true;
        }
    }
    return false;
}

enum class TransitionResult
{
    Applied,
    /// Illegal transition: a_state is unchanged; the observation was
    /// stale, duplicated or out of order and must be discarded.
    Rejected
};

/// Attempt to move a_state to a_next following the explicit table.
[[nodiscard]] inline auto apply_transition(OrderState& a_state,
                                           OrderState a_next) -> TransitionResult
{
    if (!can_transition(a_state, a_next)) {
        return TransitionResult::Rejected;
    }
    a_state = a_next;
    return TransitionResult::Applied;
}

} // namespace gateway
