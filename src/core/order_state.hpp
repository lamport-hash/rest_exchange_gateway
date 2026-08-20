#pragma once

#include "gateway/exchange_connector.hpp"

#include <array>
#include <utility>

namespace gateway {

/// Explicit legal-transition table of the normalized order lifecycle.
///
/// Semantics:
/// - An order is born Live (venue ack) or Rejected (pre-trade risk
///   rejection, definitive venue rejection, or conclusively unknown to
///   the venue after a restart).
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
inline constexpr std::array<std::pair<OrderState, OrderState>, 9> kLegalTransitions{{
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
[[nodiscard]] inline auto apply_transition(OrderState& a_state, OrderState a_next)
    -> TransitionResult
{
    if (!can_transition(a_state, a_next)) {
        return TransitionResult::Rejected;
    }
    a_state = a_next;
    return TransitionResult::Applied;
}

} // namespace gateway
