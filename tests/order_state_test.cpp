#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/order_state.hpp"

#include <vector>

namespace {

using namespace gateway;

auto all_states() -> std::vector<OrderState>
{
    return {OrderState::Pending, OrderState::Live,     OrderState::PartiallyFilled,
            OrderState::Filled,  OrderState::Canceled, OrderState::Rejected};
}

} // namespace

TEST_CASE("the transition table is exactly the approved 6x6 lifecycle matrix")
{
    // Expected matrix (rows: from, columns: to), P = pending, L = live,
    // PF = partially_filled, F = filled, C = canceled, R = rejected:
    //      P   L   PF  F   C   R
    //  P   y   y   y   y   n   y     pending resolves forward; pending ->
    //  L   n   y   y   y   y   y     canceled is illegal (the venue has
    //  PF  n   n   y   y   y   y     not accepted the order, so there is
    //  F   n   n   n   n   n   n     nothing to cancel). No state ever
    //  C   n   n   n   n   n   n     regresses to pending.
    //  R   n   n   n   n   n   n     terminal states never move
    const auto expected = [](OrderState a_from, OrderState a_to) {
        switch (a_from) {
        case OrderState::Pending:
            return a_to != OrderState::Canceled;
        case OrderState::Live:
            return a_to != OrderState::Pending;
        case OrderState::PartiallyFilled:
            return a_to != OrderState::Live && a_to != OrderState::Pending;
        case OrderState::Filled:
        case OrderState::Canceled:
        case OrderState::Rejected:
            return false;
        }
        return false;
    };

    int legal_count = 0;
    for (const auto from : all_states()) {
        for (const auto to : all_states()) {
            const bool legal = can_transition(from, to);
            CHECK(legal == expected(from, to));
            legal_count += legal ? 1 : 0;
        }
    }
    CHECK(legal_count == 14);
    CHECK(kLegalTransitions.size() == 14);
}

TEST_CASE("is_terminal marks exactly the terminal states")
{
    CHECK_FALSE(is_terminal(OrderState::Pending));
    CHECK_FALSE(is_terminal(OrderState::Live));
    CHECK_FALSE(is_terminal(OrderState::PartiallyFilled));
    CHECK(is_terminal(OrderState::Filled));
    CHECK(is_terminal(OrderState::Canceled));
    CHECK(is_terminal(OrderState::Rejected));
}

TEST_CASE("apply_transition mutates on success and refuses illegal moves")
{
    OrderState state = OrderState::Live;
    CHECK(apply_transition(state, OrderState::PartiallyFilled) == TransitionResult::Applied);
    CHECK(state == OrderState::PartiallyFilled);

    // regression to Live is stale -> rejected, state untouched
    CHECK(apply_transition(state, OrderState::Live) == TransitionResult::Rejected);
    CHECK(state == OrderState::PartiallyFilled);

    CHECK(apply_transition(state, OrderState::Filled) == TransitionResult::Applied);
    CHECK(state == OrderState::Filled);

    // terminal: nothing leaves Filled (not even a same-state repeat)
    for (const auto to : all_states()) {
        CHECK(apply_transition(state, to) == TransitionResult::Rejected);
    }
    CHECK(state == OrderState::Filled);
}

TEST_CASE("duplicate same-state observations are legal no-ops")
{
    OrderState state = OrderState::Pending;
    CHECK(apply_transition(state, OrderState::Pending) == TransitionResult::Applied);
    CHECK(state == OrderState::Pending);

    state = OrderState::Live;
    CHECK(apply_transition(state, OrderState::Live) == TransitionResult::Applied);
    CHECK(state == OrderState::Live);

    state = OrderState::PartiallyFilled;
    CHECK(apply_transition(state, OrderState::PartiallyFilled) == TransitionResult::Applied);
    CHECK(state == OrderState::PartiallyFilled);
}

TEST_CASE("pending resolves through every legal path and never cancels")
{
    // ack -> live -> the usual lifecycle
    OrderState ack_path = OrderState::Pending;
    CHECK(apply_transition(ack_path, OrderState::Live) == TransitionResult::Applied);
    CHECK(apply_transition(ack_path, OrderState::PartiallyFilled) == TransitionResult::Applied);
    CHECK(apply_transition(ack_path, OrderState::Filled) == TransitionResult::Applied);

    // a fill racing the ack skips Live
    OrderState raced_fill = OrderState::Pending;
    CHECK(apply_transition(raced_fill, OrderState::PartiallyFilled) == TransitionResult::Applied);
    CHECK(apply_transition(raced_fill, OrderState::Filled) == TransitionResult::Applied);

    OrderState raced_full_fill = OrderState::Pending;
    CHECK(apply_transition(raced_full_fill, OrderState::Filled) == TransitionResult::Applied);

    // definitive venue reject / restart proves the venue never saw it
    OrderState rejected = OrderState::Pending;
    CHECK(apply_transition(rejected, OrderState::Rejected) == TransitionResult::Applied);

    // pending -> canceled is illegal: no venue call can acknowledge it
    OrderState no_cancel = OrderState::Pending;
    CHECK(apply_transition(no_cancel, OrderState::Canceled) == TransitionResult::Rejected);
    CHECK(no_cancel == OrderState::Pending);

    // live-then-canceled and direct fill remain walkable
    OrderState direct_cancel = OrderState::Live;
    CHECK(apply_transition(direct_cancel, OrderState::Canceled) == TransitionResult::Applied);
    OrderState direct_fill = OrderState::Live;
    CHECK(apply_transition(direct_fill, OrderState::Filled) == TransitionResult::Applied);
}
