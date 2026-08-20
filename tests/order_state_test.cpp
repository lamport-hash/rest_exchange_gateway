#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/order_state.hpp"

#include <vector>

namespace {

using namespace gateway;

auto all_states() -> std::vector<OrderState>
{
    return {OrderState::Live, OrderState::PartiallyFilled, OrderState::Filled, OrderState::Canceled,
            OrderState::Rejected};
}

} // namespace

TEST_CASE("the transition table is exactly the approved lifecycle")
{
    // Expected matrix, derived from the spec lifecycle:
    //   live -> {live, partially_filled, filled, canceled, rejected}
    //   partially_filled -> {partially_filled, filled, canceled, rejected}
    //   terminal states have no outgoing transitions.
    const auto expected = [](OrderState a_from, OrderState a_to) {
        switch (a_from) {
        case OrderState::Live:
            return true;
        case OrderState::PartiallyFilled:
            return a_to != OrderState::Live;
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
    CHECK(legal_count == 9);
    CHECK(kLegalTransitions.size() == 9);
}

TEST_CASE("is_terminal marks exactly the terminal states")
{
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
    OrderState state = OrderState::Live;
    CHECK(apply_transition(state, OrderState::Live) == TransitionResult::Applied);
    CHECK(state == OrderState::Live);

    state = OrderState::PartiallyFilled;
    CHECK(apply_transition(state, OrderState::PartiallyFilled) == TransitionResult::Applied);
    CHECK(state == OrderState::PartiallyFilled);
}

TEST_CASE("the full happy lifecycle and the cancel lifecycles are walkable")
{
    OrderState fill_path = OrderState::Live;
    CHECK(apply_transition(fill_path, OrderState::PartiallyFilled) == TransitionResult::Applied);
    CHECK(apply_transition(fill_path, OrderState::Filled) == TransitionResult::Applied);

    OrderState cancel_after_partial = OrderState::Live;
    CHECK(apply_transition(cancel_after_partial, OrderState::PartiallyFilled) ==
          TransitionResult::Applied);
    CHECK(apply_transition(cancel_after_partial, OrderState::Canceled) ==
          TransitionResult::Applied);

    OrderState direct_fill = OrderState::Live;
    CHECK(apply_transition(direct_fill, OrderState::Filled) == TransitionResult::Applied);

    OrderState direct_cancel = OrderState::Live;
    CHECK(apply_transition(direct_cancel, OrderState::Canceled) == TransitionResult::Applied);
}
