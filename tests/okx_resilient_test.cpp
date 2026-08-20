#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/retry.hpp"
#include "exchange/okx/okx_resilient.hpp"

#include <chrono>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using gateway::Error;
using gateway::RetryPolicy;
using gateway::exchange::okx::OkxAmendRequest;
using gateway::exchange::okx::OkxCxlRequest;
using gateway::exchange::okx::OkxOrderAck;
using gateway::exchange::okx::OkxOrderInfo;
using gateway::exchange::okx::OkxPlaceRequest;
using gateway::exchange::okx::OkxQuery;
using AckResult = gateway::Result<OkxOrderAck>;

/// Scripted stand-in for OkxRestClient: every response comes from a queue.
struct ScriptedClient
{
    std::deque<gateway::Result<OkxOrderAck>> place_replies;
    std::deque<gateway::Result<OkxOrderAck>> cancel_replies;
    std::deque<gateway::Result<OkxOrderAck>> amend_replies;
    std::deque<gateway::Result<std::optional<OkxOrderInfo>>> get_replies;

    std::vector<OkxPlaceRequest> placed;
    std::vector<OkxCxlRequest> canceled;
    std::vector<OkxAmendRequest> amended;
    std::vector<OkxQuery> queried;

    auto place_order(const OkxPlaceRequest& a_request) -> gateway::Result<OkxOrderAck>
    {
        placed.push_back(a_request);
        return pop(place_replies, ack());
    }

    auto cancel_order(const OkxCxlRequest& a_request) -> gateway::Result<OkxOrderAck>
    {
        canceled.push_back(a_request);
        return pop(cancel_replies, ack());
    }

    auto amend_order(const OkxAmendRequest& a_request) -> gateway::Result<OkxOrderAck>
    {
        amended.push_back(a_request);
        return pop(amend_replies, ack());
    }

    auto get_order(const OkxQuery& a_query) -> gateway::Result<std::optional<OkxOrderInfo>>
    {
        queried.push_back(a_query);
        return pop(get_replies, std::optional<OkxOrderInfo>{std::nullopt});
    }

  private:
    static auto ack() -> OkxOrderAck
    {
        return OkxOrderAck{.ord_id = "ord-1", .cl_ord_id = "gw1", .s_code = "0", .s_msg = ""};
    }

    template <typename T, typename Fallback>
    static auto pop(std::deque<gateway::Result<T>>& a_queue,
                    Fallback a_fallback) -> gateway::Result<T>
    {
        if (a_queue.empty()) {
            return a_fallback;
        }
        const auto front = a_queue.front();
        a_queue.pop_front();
        return front;
    }
};

struct NullClock
{
    std::vector<std::chrono::milliseconds> sleeps;

    [[nodiscard]] auto clock() const -> gateway::RetryClock
    {
        return gateway::RetryClock{.now = [] { return std::chrono::steady_clock::time_point{}; },
                                   .sleep =
                                       [this](std::chrono::milliseconds a_duration) {
                                           const_cast<NullClock*>(this)->sleeps.push_back(
                                               a_duration);
                                       },
                                   .random01 = [] { return 0.0; }};
    }
};

auto policy() -> RetryPolicy
{
    return RetryPolicy{.max_attempts = 3,
                       .initial_backoff = std::chrono::milliseconds{1},
                       .max_backoff = std::chrono::milliseconds{2},
                       .multiplier = 2.0,
                       .jitter = 0.0,
                       .budget = std::chrono::milliseconds{1000}};
}

auto transport() -> Error
{
    return Error{"transport", "timeout"};
}

auto info(const std::string& a_state, const std::string& a_px = "50000",
          const std::string& a_sz = "1") -> OkxOrderInfo
{
    return OkxOrderInfo{.ord_id = "ord-1",
                        .cl_ord_id = "gw1",
                        .state = a_state,
                        .side = "buy",
                        .ord_type = "limit",
                        .px = a_px,
                        .sz = a_sz,
                        .avg_px = "",
                        .acc_fill_sz = "0"};
}

auto place_request() -> OkxPlaceRequest
{
    return OkxPlaceRequest{.cl_ord_id = "gw1",
                           .inst_id = "BTC-USDT",
                           .side = "buy",
                           .ord_type = "limit",
                           .px = "50000",
                           .sz = "1"};
}

auto found(const OkxOrderInfo& a_info) -> gateway::Result<std::optional<OkxOrderInfo>>
{
    return std::optional<OkxOrderInfo>{a_info};
}

auto absent() -> gateway::Result<std::optional<OkxOrderInfo>>
{
    return std::optional<OkxOrderInfo>{std::nullopt};
}

TEST_SUITE("resilient_place")
{

    TEST_CASE("a successful place is returned without any lookup")
    {
        ScriptedClient client;
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_place(client, place_request(),
                                                                    policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(result.value().ord_id == "ord-1");
        CHECK(result.value().s_code == "0");
        CHECK(client.placed.size() == 1);
        CHECK(client.queried.empty());
    }

    TEST_CASE("transport failure with the order found resolves without re-sending")
    {
        ScriptedClient client;
        client.place_replies.push_back(transport());
        client.get_replies.push_back(found(info("live")));
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_place(client, place_request(),
                                                                    policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(result.value().ord_id == "ord-1");
        CHECK(result.value().s_code == "0");
        CHECK(client.placed.size() == 1); // never re-sent
        CHECK(client.queried.size() == 1);
    }

    TEST_CASE("transport failure with the order conclusively absent re-sends safely")
    {
        ScriptedClient client;
        client.place_replies.push_back(transport());
        client.place_replies.push_back(AckResult{OkxOrderAck{"ord-2", "gw1", "0", ""}});
        client.get_replies.push_back(absent());
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_place(client, place_request(),
                                                                    policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(result.value().ord_id == "ord-2");
        CHECK(client.placed.size() == 2);
        CHECK(client.queried.size() == 1);
    }

    TEST_CASE("unresolved lookup never re-sends (no double-place risk)")
    {
        ScriptedClient client;
        client.place_replies.push_back(transport());
        // lookup keeps failing with transport -> inconclusive
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport());
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_place(client, place_request(),
                                                                    policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
        CHECK(result.error().message.find("unresolved") != std::string::npos);
        CHECK(client.placed.size() == 1); // the critical assertion: no re-send
    }

    TEST_CASE("duplicate clOrdId rejection resolves to the existing order")
    {
        ScriptedClient client;
        client.place_replies.push_back(Error{"venue:51000", "duplicate active clOrdId"});
        client.get_replies.push_back(found(info("live")));
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_place(client, place_request(),
                                                                    policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(result.value().ord_id == "ord-1"); // the original order, not an error
        CHECK(client.placed.size() == 1);
    }

    TEST_CASE("a genuine parameter error surfaces unchanged")
    {
        ScriptedClient client;
        client.place_replies.push_back(Error{"venue:51000", "Parameter clOrdId error"});
        client.get_replies.push_back(absent()); // no such order -> not a duplicate
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_place(client, place_request(),
                                                                    policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51000");
    }

    TEST_CASE("venue rejections other than 51000 are definitive")
    {
        ScriptedClient client;
        client.place_replies.push_back(Error{"venue:51001", "Instrument ID does not exist"});
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_place(client, place_request(),
                                                                    policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51001");
        CHECK(client.queried.empty());
    }

    TEST_CASE("re-send attempts are bounded by max_attempts")
    {
        ScriptedClient client;
        for (int i = 0; i < 10; ++i) {
            client.place_replies.push_back(transport());
            client.get_replies.push_back(absent());
        }
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_place(client, place_request(),
                                                                    policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
        CHECK(client.placed.size() == 3); // max_attempts
    }

} // TEST_SUITE

TEST_SUITE("resilient_cancel")
{

    auto cancel_request() -> OkxCxlRequest
    {
        return OkxCxlRequest{.inst_id = "BTC-USDT", .cl_ord_id = "gw1"};
    }

    TEST_CASE("a successful cancel is returned without any lookup")
    {
        ScriptedClient client;
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_cancel(client, cancel_request(),
                                                                     policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(client.canceled.size() == 1);
        CHECK(client.queried.empty());
    }

    TEST_CASE("transport failure with the order already canceled resolves idempotently")
    {
        ScriptedClient client;
        client.cancel_replies.push_back(transport());
        client.get_replies.push_back(found(info("canceled")));
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_cancel(client, cancel_request(),
                                                                     policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(result.value().ord_id == "ord-1");
        CHECK(client.canceled.size() == 1); // never re-sent
    }

    TEST_CASE("transport failure with a live order re-sends the cancel")
    {
        ScriptedClient client;
        client.cancel_replies.push_back(transport());
        client.cancel_replies.push_back(AckResult{OkxOrderAck{"ord-1", "gw1", "0", ""}});
        client.get_replies.push_back(found(info("live")));
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_cancel(client, cancel_request(),
                                                                     policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(client.canceled.size() == 2);
    }

    TEST_CASE("transport failure with the order absent is a definitive not-found")
    {
        ScriptedClient client;
        client.cancel_replies.push_back(transport());
        client.get_replies.push_back(absent());
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_cancel(client, cancel_request(),
                                                                     policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51603");
    }

    TEST_CASE("cancel of an already-canceled order (venue 51017) is idempotent success")
    {
        ScriptedClient client;
        client.cancel_replies.push_back(Error{"venue:51017", "Order status is done"});
        client.get_replies.push_back(found(info("canceled")));
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_cancel(client, cancel_request(),
                                                                     policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(result.value().ord_id == "ord-1");
        CHECK(client.canceled.size() == 1);
    }

    TEST_CASE("cancel of a filled order (venue 51017) stays a rejection")
    {
        ScriptedClient client;
        client.cancel_replies.push_back(Error{"venue:51017", "Order status is done"});
        client.get_replies.push_back(found(info("filled")));
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_cancel(client, cancel_request(),
                                                                     policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51017");
    }

    TEST_CASE("unknown-order cancel (venue 51016) passes through unchanged")
    {
        ScriptedClient client;
        client.cancel_replies.push_back(Error{"venue:51016", "Order does not exist"});
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_cancel(client, cancel_request(),
                                                                     policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51016");
        CHECK(client.queried.empty());
    }

    TEST_CASE("unresolved cancel lookup reports transport without re-sending")
    {
        ScriptedClient client;
        client.cancel_replies.push_back(transport());
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport());
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_cancel(client, cancel_request(),
                                                                     policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
        CHECK(client.canceled.size() == 1);
    }

} // TEST_SUITE

TEST_SUITE("resilient_amend")
{

    auto amend_request(const std::string& a_px, const std::string& a_sz = "") -> OkxAmendRequest
    {
        return OkxAmendRequest{.inst_id = "BTC-USDT",
                               .cl_ord_id = "gw1",
                               .new_px = a_px.empty() ? std::optional<std::string>{std::nullopt}
                                                      : std::optional<std::string>{a_px},
                               .new_sz = a_sz.empty() ? std::optional<std::string>{std::nullopt}
                                                      : std::optional<std::string>{a_sz}};
    }

    TEST_CASE("a successful amend is returned without any lookup")
    {
        ScriptedClient client;
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_amend(client, amend_request("51000"),
                                                                    policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(client.amended.size() == 1);
        CHECK(client.queried.empty());
    }

    TEST_CASE("transport failure with the snapshot already updated resolves without re-send")
    {
        ScriptedClient client;
        client.amend_replies.push_back(transport());
        client.get_replies.push_back(found(info("live", "51000")));
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_amend(client, amend_request("51000"),
                                                                    policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(result.value().ord_id == "ord-1");
        CHECK(client.amended.size() == 1);
    }

    TEST_CASE("transport failure with an old snapshot re-sends the same amend")
    {
        ScriptedClient client;
        client.amend_replies.push_back(transport());
        client.amend_replies.push_back(AckResult{OkxOrderAck{"ord-1", "gw1", "0", ""}});
        client.get_replies.push_back(found(info("live", "50000"))); // old price
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_amend(client, amend_request("51000"),
                                                                    policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(client.amended.size() == 2);
        // the re-send is identical (idempotent in effect)
        CHECK(client.amended[0].new_px == client.amended[1].new_px);
    }

    TEST_CASE("amend matches only the requested fields")
    {
        ScriptedClient client;
        client.amend_replies.push_back(transport());
        // size amended, price untouched: only new_sz requested
        client.get_replies.push_back(found(info("live", "50000", "2")));
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_amend(client, amend_request("", "2"),
                                                                    policy(), clock.clock());

        REQUIRE(result.is_ok());
        CHECK(client.amended.size() == 1);
    }

    TEST_CASE("transport failure with the order absent is a definitive not-found")
    {
        ScriptedClient client;
        client.amend_replies.push_back(transport());
        client.get_replies.push_back(absent());
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_amend(client, amend_request("51000"),
                                                                    policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51603");
    }

    TEST_CASE("unresolved amend lookup reports transport without re-sending")
    {
        ScriptedClient client;
        client.amend_replies.push_back(transport());
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport());
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_amend(client, amend_request("51000"),
                                                                    policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
        CHECK(client.amended.size() == 1);
    }

    TEST_CASE("venue rejections are definitive")
    {
        ScriptedClient client;
        client.amend_replies.push_back(Error{"venue:51017", "Order status is done"});
        const NullClock clock;

        const auto result = gateway::exchange::okx::resilient_amend(client, amend_request("51000"),
                                                                    policy(), clock.clock());

        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51017");
        CHECK(client.queried.empty());
    }

} // TEST_SUITE

TEST_SUITE("lookup_order")
{

    TEST_CASE("lookup retries transport failures and concludes")
    {
        ScriptedClient client;
        client.get_replies.push_back(transport());
        client.get_replies.push_back(found(info("live")));
        const NullClock clock;

        const auto lookup = gateway::exchange::okx::lookup_order(
            client, OkxQuery{"BTC-USDT", "gw1"}, policy(), clock.clock());

        CHECK(lookup.outcome == gateway::exchange::okx::LookupOutcome::Found);
        CHECK(lookup.info.ord_id == "ord-1");
        CHECK(client.queried.size() == 2);
    }

    TEST_CASE("venue 51603 and empty data both mean absent")
    {
        const NullClock clock;
        ScriptedClient missing;
        missing.get_replies.push_back(Error{"venue:51603", "Order does not exist"});
        const auto lookup_missing = gateway::exchange::okx::lookup_order(
            missing, OkxQuery{"BTC-USDT", "gw1"}, policy(), clock.clock());
        CHECK(lookup_missing.outcome == gateway::exchange::okx::LookupOutcome::Absent);

        ScriptedClient empty;
        empty.get_replies.push_back(absent());
        const auto lookup_empty = gateway::exchange::okx::lookup_order(
            empty, OkxQuery{"BTC-USDT", "gw1"}, policy(), clock.clock());
        CHECK(lookup_empty.outcome == gateway::exchange::okx::LookupOutcome::Absent);
    }

    TEST_CASE("protocol errors are conclusive without retrying")
    {
        ScriptedClient client;
        client.get_replies.push_back(Error{"protocol", "not json"});
        const NullClock clock;

        const auto lookup = gateway::exchange::okx::lookup_order(
            client, OkxQuery{"BTC-USDT", "gw1"}, policy(), clock.clock());

        CHECK(lookup.outcome == gateway::exchange::okx::LookupOutcome::Inconclusive);
        CHECK(lookup.error.has_value());
        CHECK(client.queried.size() == 1);
    }

    TEST_CASE("exhausted transport retries end inconclusive")
    {
        ScriptedClient client;
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport());
        client.get_replies.push_back(transport()); // beyond max_attempts
        const NullClock clock;

        const auto lookup = gateway::exchange::okx::lookup_order(
            client, OkxQuery{"BTC-USDT", "gw1"}, policy(), clock.clock());

        CHECK(lookup.outcome == gateway::exchange::okx::LookupOutcome::Inconclusive);
        CHECK(client.queried.size() == 3);
    }

} // TEST_SUITE

} // namespace
