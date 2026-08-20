#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/event_log.hpp"
#include "core/oms.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace gateway;

/// Scripted in-memory ExchangeConnector: every verb dispatches to a
/// mutable std::function so tests can script outcomes per scenario.
class FakeConnector final : public ExchangeConnector
{
  public:
    std::function<Result<OrderPlacement>(const OrderRequest&)> place_impl =
        [](const OrderRequest& a_request) {
            return OrderPlacement{.client_order_id = a_request.client_order_id,
                                  .exchange_order_id = "ord-" + a_request.client_order_id};
        };
    std::function<Result<OrderPlacement>(const CancelRequest&)> cancel_impl =
        [](const CancelRequest& a_request) {
            return OrderPlacement{.client_order_id = a_request.client_order_id,
                                  .exchange_order_id = "ord-" + a_request.client_order_id};
        };
    std::function<Result<OrderPlacement>(const AmendRequest&)> amend_impl =
        [](const AmendRequest& a_request) {
            return OrderPlacement{.client_order_id = a_request.client_order_id,
                                  .exchange_order_id = "ord-" + a_request.client_order_id};
        };
    std::function<Result<std::optional<OrderSnapshot>>(const OrderQuery&)> get_impl =
        [](const OrderQuery&) -> Result<std::optional<OrderSnapshot>> {
        return Result<std::optional<OrderSnapshot>>{std::optional<OrderSnapshot>{std::nullopt}};
    };
    std::function<Result<std::vector<OrderSnapshot>>()> open_impl =
        []() -> Result<std::vector<OrderSnapshot>> { return std::vector<OrderSnapshot>{}; };

    std::vector<OrderRequest> placed;
    std::vector<CancelRequest> cancels;
    std::vector<AmendRequest> amends;

    [[nodiscard]] auto place_order(const OrderRequest& a_request) -> Result<OrderPlacement> override
    {
        placed.push_back(a_request);
        return place_impl(a_request);
    }
    [[nodiscard]] auto cancel_order(const CancelRequest& a_request) -> Result<OrderPlacement> override
    {
        cancels.push_back(a_request);
        return cancel_impl(a_request);
    }
    [[nodiscard]] auto amend_order(const AmendRequest& a_request) -> Result<OrderPlacement> override
    {
        amends.push_back(a_request);
        return amend_impl(a_request);
    }
    [[nodiscard]] auto get_order(const OrderQuery& a_query)
        -> Result<std::optional<OrderSnapshot>> override
    {
        return get_impl(a_query);
    }
    [[nodiscard]] auto get_open_orders() -> Result<std::vector<OrderSnapshot>> override
    {
        return open_impl();
    }
    void set_execution_report_handler(
        std::function<void(const ExecutionReport&)> a_handler) override
    {
        report_handler = std::move(a_handler);
    }
    void set_connectivity_handler(std::function<void(bool)> a_handler) override
    {
        connectivity_handler = std::move(a_handler);
    }
    void start() override {}
    void stop() override {}

    std::function<void(const ExecutionReport&)> report_handler;
    std::function<void(bool)> connectivity_handler;
};

auto temp_log_path(const char* a_name) -> std::filesystem::path
{
    return std::filesystem::temp_directory_path() /
           ("gateway_oms_test_" + std::string(a_name) + ".jsonl");
}

auto buy_request(const std::string& a_id = "gw1", const std::string& a_qty = "1") -> OrderRequest
{
    return OrderRequest{.client_order_id = a_id,
                        .instrument_id = "BTC-USDT",
                        .side = Side::Buy,
                        .type = OrderType::Limit,
                        .price = "50000",
                        .quantity = a_qty,
                        .time_in_force = ""};
}

auto report(const std::string& a_id, OrderState a_state, const std::string& a_filled,
            const std::string& a_avg = "") -> ExecutionReport
{
    return ExecutionReport{.client_order_id = a_id,
                           .exchange_order_id = "ord-" + a_id,
                           .state = a_state,
                           .side = Side::Buy,
                           .filled_quantity = a_filled,
                           .average_fill_price = a_avg};
}

auto risk_with_position(const std::string& a_max_position) -> RiskConfig
{
    auto parsed = risk_config_from_json(nlohmann::json::parse(
        R"({"instruments":{"BTC-USDT":{"maxPosition":")" + a_max_position + R"("}}})"));
    REQUIRE(parsed.is_ok());
    return parsed.value();
}

// ---------------------------------------------------------------- place ----

TEST_CASE("place records the accepted order and replays it idempotently")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};

    const auto first = oms.place(buy_request());
    REQUIRE(first.is_ok());
    CHECK_FALSE(first.value().replayed);
    CHECK(first.value().record.state == OrderState::Live);
    CHECK(first.value().record.exchange_order_id == "ord-gw1");
    CHECK(first.value().record.time_in_force == "GTC");
    REQUIRE(connector.placed.size() == 1);

    // strict idempotency: same clientOrderId replays the recorded outcome
    // without touching the venue again
    const auto second = oms.place(buy_request());
    REQUIRE(second.is_ok());
    CHECK(second.value().replayed);
    CHECK(second.value().record.exchange_order_id == first.value().record.exchange_order_id);
    REQUIRE(connector.placed.size() == 1);

    // even a DIFFERENT payload with the same clientOrderId replays (the
    // clientOrderId is the idempotency key)
    auto other = buy_request();
    other.quantity = "999";
    const auto third = oms.place(other);
    REQUIRE(third.is_ok());
    CHECK(third.value().replayed);
    REQUIRE(connector.placed.size() == 1);
}

TEST_CASE("place after a terminal outcome still replays the recorded outcome")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};

    REQUIRE(oms.place(buy_request()).is_ok());
    REQUIRE(oms.cancel("gw1").is_ok());

    const auto replayed = oms.place(buy_request());
    REQUIRE(replayed.is_ok());
    CHECK(replayed.value().replayed);
    CHECK(replayed.value().record.state == OrderState::Canceled);
    REQUIRE(connector.placed.size() == 1); // never re-sent
}

TEST_CASE("risk rejections are recorded and replayed")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, risk_with_position("1")};

    const auto rejected = oms.place(buy_request("gw1", "5"));
    REQUIRE_FALSE(rejected.is_ok());
    CHECK(rejected.error().code == "risk_max_position");
    REQUIRE(connector.placed.empty()); // never routed to the venue

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::Rejected);
    REQUIRE(record.value().rejection.has_value());
    CHECK(record.value().rejection->code == "risk_max_position");

    // retrying the same clientOrderId replays the same rejection
    const auto replayed = oms.place(buy_request("gw1", "5"));
    REQUIRE_FALSE(replayed.is_ok());
    CHECK(replayed.error().code == "risk_max_position");
    REQUIRE(connector.placed.empty());
}

TEST_CASE("definitive venue rejections are recorded and replayed")
{
    FakeConnector connector;
    connector.place_impl = [](const OrderRequest&) -> Result<OrderPlacement> {
        return Error{"venue:51001", "Instrument ID does not exist"};
    };
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};

    const auto rejected = oms.place(buy_request());
    REQUIRE_FALSE(rejected.is_ok());
    CHECK(rejected.error().code == "venue:51001");

    const auto replayed = oms.place(buy_request());
    REQUIRE_FALSE(replayed.is_ok());
    CHECK(replayed.error().code == "venue:51001");
    REQUIRE(connector.placed.size() == 1); // the venue saw it exactly once
    CHECK(oms.query("gw1").value().state == OrderState::Rejected);
}

TEST_CASE("transport-unresolved places record nothing; retry reaches the venue")
{
    FakeConnector connector;
    int attempts = 0;
    connector.place_impl = [&attempts](const OrderRequest&) -> Result<OrderPlacement> {
        ++attempts;
        if (attempts == 1) {
            return Error{"transport", "unresolved"};
        }
        return OrderPlacement{.client_order_id = "gw1", .exchange_order_id = "ord-gw1"};
    };
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};

    const auto failed = oms.place(buy_request());
    REQUIRE_FALSE(failed.is_ok());
    CHECK(failed.error().code == "transport");
    CHECK(oms.stats().known_orders == 0);

    const auto retried = oms.place(buy_request());
    REQUIRE(retried.is_ok());
    CHECK(retried.value().record.exchange_order_id == "ord-gw1");
}

// ------------------------------------------------------- execution feeds ----

TEST_CASE("execution reports advance state and dedupe exactly")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());

    const auto partial = report("gw1", OrderState::PartiallyFilled, "0.4", "49999.5");
    oms.on_execution_report(partial);
    oms.on_execution_report(partial); // exact duplicate

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::PartiallyFilled);
    CHECK(record.value().filled_quantity == "0.4");
    CHECK(record.value().average_fill_price == "49999.5");
    CHECK(oms.stats().reports_applied == 1);
    CHECK(oms.stats().reports_stale == 1);
}

TEST_CASE("out-of-order reports never regress state or fills")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());

    oms.on_execution_report(report("gw1", OrderState::PartiallyFilled, "0.4", "49999.5"));
    // an older "live, nothing filled" report arrives late
    oms.on_execution_report(report("gw1", OrderState::Live, "0"));

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::PartiallyFilled);
    CHECK(record.value().filled_quantity == "0.4");
    CHECK(oms.stats().reports_stale == 1);
}

TEST_CASE("REST-vs-WS race: fill report wins over later stale snapshot data")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());

    // WS delivers the fill...
    oms.on_execution_report(report("gw1", OrderState::Filled, "1", "50000"));
    // ...then a REST snapshot taken BEFORE the fill is applied (a report
    // with the same information would race identically)
    oms.on_execution_report(report("gw1", OrderState::Live, "0"));

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::Filled);
    CHECK(record.value().filled_quantity == "1");
}

TEST_CASE("terminal orders ignore state regression but keep late fill data")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());

    oms.on_execution_report(report("gw1", OrderState::Canceled, "0"));
    oms.on_execution_report(report("gw1", OrderState::Live, "1")); // illegal: Canceled -> Live

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::Canceled); // state never regressed
    CHECK(record.value().filled_quantity == "1"); // fill information is kept
}

TEST_CASE("reports for unknown orders are counted, not applied")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    oms.on_execution_report(report("ghost", OrderState::Filled, "1"));
    CHECK(oms.stats().reports_unknown == 1);
    CHECK_FALSE(oms.query("ghost").is_ok());
}

TEST_CASE("late fill data still lands after the state went terminal")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());

    // venue canceled a partially filled order; the fill report races in
    // after the cancel snapshot
    oms.on_execution_report(report("gw1", OrderState::Canceled, "0.2", "49999"));
    oms.on_execution_report(report("gw1", OrderState::PartiallyFilled, "0.2", "49999"));

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::Canceled); // state kept
    CHECK(record.value().filled_quantity == "0.2");      // fill kept
}

// -------------------------------------------------------- cancel / amend ----

TEST_CASE("cancel is idempotent and rejects terminal orders clearly")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};

    const auto missing = oms.cancel("gw1");
    REQUIRE_FALSE(missing.is_ok());
    CHECK(missing.error().code == "not_found");

    REQUIRE(oms.place(buy_request()).is_ok());
    const auto canceled = oms.cancel("gw1");
    REQUIRE(canceled.is_ok());
    CHECK(canceled.value().state == OrderState::Canceled);

    const auto again = oms.cancel("gw1");
    REQUIRE(again.is_ok());
    CHECK(again.value().state == OrderState::Canceled);
    REQUIRE(connector.cancels.size() == 1); // venue asked once

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::Canceled);
}

TEST_CASE("cancel of a filled order is order_terminal, not a venue call")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());
    oms.on_execution_report(report("gw1", OrderState::Filled, "1"));

    const auto result = oms.cancel("gw1");
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "order_terminal");
    CHECK(connector.cancels.empty());
}

TEST_CASE("amend updates the record after the venue accepts")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());

    const auto amended = oms.amend(
        AmendCommand{"gw1", std::optional<std::string>{"51000"}, std::optional<std::string>{"2"}});
    REQUIRE(amended.is_ok());
    CHECK(amended.value().price == "51000");
    CHECK(amended.value().quantity == "2");
    REQUIRE(connector.amends.size() == 1);
    CHECK(connector.amends.front().new_price.value() == "51000");

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().price == "51000");
    CHECK(record.value().quantity == "2");
}

TEST_CASE("amend validates its inputs and the order's liveness")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};

    const auto nothing = oms.amend(AmendCommand{"gw1", std::nullopt, std::nullopt});
    REQUIRE_FALSE(nothing.is_ok());
    CHECK(nothing.error().code == "protocol");

    const auto missing = oms.amend(AmendCommand{"ghost", std::optional<std::string>{"1"},
                                                std::nullopt});
    REQUIRE_FALSE(missing.is_ok());
    CHECK(missing.error().code == "not_found");

    REQUIRE(oms.place(buy_request()).is_ok());
    REQUIRE(oms.cancel("gw1").is_ok());
    const auto terminal = oms.amend(
        AmendCommand{"gw1", std::optional<std::string>{"1"}, std::nullopt});
    REQUIRE_FALSE(terminal.is_ok());
    CHECK(terminal.error().code == "order_terminal");
    CHECK(connector.amends.empty());
}

TEST_CASE("amends re-run risk against the projected position")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, risk_with_position("1")};

    REQUIRE(oms.place(buy_request("gw1", "1")).is_ok());
    const auto grow = oms.amend(AmendCommand{"gw1", std::nullopt,
                                             std::optional<std::string>{"3"}});
    REQUIRE_FALSE(grow.is_ok());
    CHECK(grow.error().code == "risk_max_position");
    CHECK(connector.amends.empty());

    const auto shrink = oms.amend(AmendCommand{"gw1", std::nullopt,
                                               std::optional<std::string>{"0.5"}});
    REQUIRE(shrink.is_ok());
    CHECK(shrink.value().quantity == "0.5");
}

TEST_CASE("hedged buy+sell exposure nets out in the position projection")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, risk_with_position("1")};

    REQUIRE(oms.place(buy_request("b1", "1")).is_ok());
    auto sell = buy_request("s1", "1");
    sell.side = Side::Sell;
    const auto hedged = oms.place(sell);
    REQUIRE(hedged.is_ok()); // +1 buy and -1 sell net to zero

    auto extra = buy_request("b2", "2");
    const auto over = oms.place(extra);
    REQUIRE_FALSE(over.is_ok());
    CHECK(over.error().code == "risk_max_position"); // net 0 + 2 breaches the limit
}

// ---------------------------------------------------------- persistence ----

TEST_CASE("a full lifecycle persists and replays into an identical registry")
{
    const auto path = temp_log_path("lifecycle");
    std::filesystem::remove(path);

    std::string exchange_order_id;
    {
        FakeConnector connector;
        EventLog log{path};
        OrderManagementSystem oms{connector, &log, RiskConfig{}};

        REQUIRE(oms.place(buy_request("gw1", "1")).is_ok());
        oms.on_execution_report(report("gw1", OrderState::PartiallyFilled, "0.4", "49999.5"));
        REQUIRE(oms.amend(AmendCommand{"gw1", std::optional<std::string>{"48000"},
                                       std::optional<std::string>{"2"}})
                    .is_ok());
        oms.on_execution_report(report("gw1", OrderState::Filled, "2", "48100"));
        exchange_order_id = oms.query("gw1").value().exchange_order_id;
    }

    FakeConnector connector;
    EventLog log{path};
    OrderManagementSystem recovered{connector, &log, RiskConfig{}};
    const auto stats = recovered.load_from_log();
    REQUIRE(stats.is_ok());
    CHECK(stats.value().events > 0);
    CHECK_FALSE(stats.value().tail_truncated);

    const auto after = recovered.query("gw1");
    REQUIRE(after.is_ok());
    CHECK(after.value().state == OrderState::Filled);
    CHECK(after.value().filled_quantity == "2");
    CHECK(after.value().average_fill_price == "48100");
    CHECK(after.value().price == "48000");
    CHECK(after.value().quantity == "2");
    CHECK(after.value().side == Side::Buy);
    CHECK(after.value().exchange_order_id == exchange_order_id);
}

TEST_CASE("rejections persist and replay as deterministic rejections")
{
    const auto path = temp_log_path("rejected");
    std::filesystem::remove(path);

    {
        FakeConnector connector;
        EventLog log{path};
        OrderManagementSystem oms{connector, &log, risk_with_position("1")};
        const auto rejected = oms.place(buy_request("gw1", "5"));
        REQUIRE_FALSE(rejected.is_ok());
    }

    FakeConnector connector;
    EventLog log{path};
    OrderManagementSystem recovered{connector, &log, RiskConfig{}};
    REQUIRE(recovered.load_from_log().is_ok());

    const auto replayed = recovered.place(buy_request("gw1", "5"));
    REQUIRE_FALSE(replayed.is_ok());
    CHECK(replayed.error().code == "risk_max_position");
    CHECK(connector.placed.empty());
}

TEST_CASE("load_from_log survives a torn tail")
{
    const auto path = temp_log_path("torn");
    std::filesystem::remove(path);
    {
        FakeConnector connector;
        EventLog log{path};
        OrderManagementSystem oms{connector, &log, RiskConfig{}};
        REQUIRE(oms.place(buy_request("gw1")).is_ok());
    }
    {
        std::ofstream file(path, std::ios::app | std::ios::binary);
        file << R"({"type":"state","clientOrderId":"gw1","sta)";
    }

    FakeConnector connector;
    EventLog log{path};
    OrderManagementSystem recovered{connector, &log, RiskConfig{}};
    const auto stats = recovered.load_from_log();
    REQUIRE(stats.is_ok());
    CHECK(stats.value().tail_truncated);
    const auto record = recovered.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::Live);
}

TEST_CASE("a corrupt mid-file log fails load_from_log")
{
    const auto path = temp_log_path("corrupt");
    std::filesystem::remove(path);
    {
        FakeConnector connector;
        EventLog log{path};
        OrderManagementSystem oms{connector, &log, RiskConfig{}};
        REQUIRE(oms.place(buy_request("gw1")).is_ok());
    }
    {
        std::ofstream file(path, std::ios::app | std::ios::binary);
        file << "garbage not json\n";
    }

    FakeConnector connector;
    EventLog log{path};
    OrderManagementSystem recovered{connector, &log, RiskConfig{}};
    const auto stats = recovered.load_from_log();
    REQUIRE_FALSE(stats.is_ok());
    CHECK(stats.error().code == "persistence");
}

// ----------------------------------------------------------- reconcile ----

TEST_CASE("reconcile adopts venue-live orders missing from the registry")
{
    const auto path = temp_log_path("adopt");
    std::filesystem::remove(path);

    FakeConnector connector;
    const OrderSnapshot venue_order{.client_order_id = "venueonly",
                                    .exchange_order_id = "ord-77",
                                    .instrument_id = "BTC-USDT",
                                    .state = OrderState::PartiallyFilled,
                                    .side = Side::Sell,
                                    .type = OrderType::Limit,
                                    .price = "49000",
                                    .quantity = "3",
                                    .filled_quantity = "1",
                                    .average_fill_price = "48999"};
    connector.open_impl = [&venue_order]() -> Result<std::vector<OrderSnapshot>> {
        return std::vector<OrderSnapshot>{venue_order};
    };
    connector.get_impl = [&venue_order](const OrderQuery& a_query)
        -> Result<std::optional<OrderSnapshot>> {
        if (a_query.client_order_id == "venueonly") {
            return Result<std::optional<OrderSnapshot>>{
                std::optional<OrderSnapshot>{venue_order}};
        }
        return Error{"venue:51603", "Order does not exist"};
    };
    EventLog log{path};
    OrderManagementSystem oms{connector, &log, RiskConfig{}};

    const auto report = oms.reconcile();
    CHECK(report.adopted == 1);

    const auto record = oms.query("venueonly");
    REQUIRE(record.is_ok());
    CHECK(record.value().adopted);
    CHECK(record.value().state == OrderState::PartiallyFilled);
    CHECK(record.value().side == Side::Sell);
    CHECK(record.value().filled_quantity == "1");

    // the adoption itself persists: a restart learns it from the log
    OrderManagementSystem restarted{connector, &log, RiskConfig{}};
    REQUIRE(restarted.load_from_log().is_ok());
    const auto replayed = restarted.query("venueonly");
    REQUIRE(replayed.is_ok());
    CHECK(replayed.value().adopted);
    CHECK(replayed.value().state == OrderState::PartiallyFilled);
}

TEST_CASE("reconcile resolves, rejects-absent, and keeps live entries")
{
    FakeConnector connector;
    EventLog log{temp_log_path("resolve")};
    std::filesystem::remove(log.path());
    OrderManagementSystem oms{connector, &log, RiskConfig{}};

    REQUIRE(oms.place(buy_request("filled1")).is_ok());
    REQUIRE(oms.place(buy_request("absent1")).is_ok());
    REQUIRE(oms.place(buy_request("stale1")).is_ok());

    connector.get_impl = [](const OrderQuery& a_query)
        -> Result<std::optional<OrderSnapshot>> {
        if (a_query.client_order_id == "filled1") {
            return Result<std::optional<OrderSnapshot>>{std::optional<OrderSnapshot>{
                OrderSnapshot{.client_order_id = "filled1",
                              .exchange_order_id = "ord-filled1",
                              .instrument_id = "BTC-USDT",
                              .state = OrderState::Filled,
                              .side = Side::Buy,
                              .type = OrderType::Limit,
                              .price = "50000",
                              .quantity = "1",
                              .filled_quantity = "1",
                              .average_fill_price = "50000"}}};
        }
        if (a_query.client_order_id == "absent1") {
            return Error{"venue:51603", "Order does not exist"};
        }
        return Error{"transport", "venue unreachable"};
    };

    const auto report = oms.reconcile();
    CHECK(report.terminal_resolved == 1);
    CHECK(report.absent_rejected == 1);
    CHECK(report.unresolved == 1);

    const auto filled = oms.query("filled1");
    REQUIRE(filled.is_ok());
    CHECK(filled.value().state == OrderState::Filled);

    const auto absent = oms.query("absent1");
    REQUIRE(absent.is_ok());
    CHECK(absent.value().state == OrderState::Rejected);
    REQUIRE(absent.value().rejection.has_value());
    CHECK(absent.value().rejection->code == "venue_absent");

    const auto stale = oms.query("stale1");
    REQUIRE(stale.is_ok());
    CHECK(stale.value().state == OrderState::Live); // kept, not guessed
}

TEST_CASE("reconcile tolerates a failing pending listing")
{
    FakeConnector connector;
    connector.open_impl = []() -> Result<std::vector<OrderSnapshot>> {
        return Error{"transport", "orders-pending unreachable"};
    };
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());

    connector.get_impl = [](const OrderQuery&) -> Result<std::optional<OrderSnapshot>> {
        return Error{"transport", "down"};
    };
    const auto report = oms.reconcile();
    CHECK(report.pending_listing_failed);
    CHECK(report.unresolved == 1);
    CHECK(oms.query("gw1").value().state == OrderState::Live);
}

// ------------------------------------------------------- restart drill ----

TEST_CASE("restart drill: replay + reconcile reconstructs in-flight fills")
{
    const auto path = temp_log_path("drill");
    std::filesystem::remove(path);

    // instance 1: place an order, then "crash" before seeing any fill
    {
        FakeConnector connector;
        EventLog log{path};
        OrderManagementSystem oms{connector, &log, RiskConfig{}};
        REQUIRE(oms.place(buy_request("gw1", "2")).is_ok());
    }

    // while down, the venue partially fills the order and knows it live
    FakeConnector connector;
    const OrderSnapshot venue_state{.client_order_id = "gw1",
                                    .exchange_order_id = "ord-gw1",
                                    .instrument_id = "BTC-USDT",
                                    .state = OrderState::PartiallyFilled,
                                    .side = Side::Buy,
                                    .type = OrderType::Limit,
                                    .price = "50000",
                                    .quantity = "2",
                                    .filled_quantity = "0.7",
                                    .average_fill_price = "49999.5"};
    connector.open_impl = [&venue_state]() -> Result<std::vector<OrderSnapshot>> {
        return std::vector<OrderSnapshot>{venue_state};
    };
    connector.get_impl = [&venue_state](const OrderQuery& a_query)
        -> Result<std::optional<OrderSnapshot>> {
        if (a_query.client_order_id == "gw1") {
            return Result<std::optional<OrderSnapshot>>{
                std::optional<OrderSnapshot>{venue_state}};
        }
        return Error{"venue:51603", "Order does not exist"};
    };

    // instance 2: replay the log, then reconcile with the venue
    EventLog log{path};
    OrderManagementSystem oms{connector, &log, RiskConfig{}};
    REQUIRE(oms.load_from_log().is_ok());
    const auto report = oms.reconcile();
    CHECK(report.updated >= 1);

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::PartiallyFilled);
    CHECK(record.value().filled_quantity == "0.7");
    CHECK(record.value().average_fill_price == "49999.5");
    CHECK_FALSE(record.value().adopted);

    // strict idempotency survived the restart
    const auto replayed = oms.place(buy_request("gw1", "2"));
    REQUIRE(replayed.is_ok());
    CHECK(replayed.value().replayed);
    CHECK(connector.placed.empty());
}

TEST_CASE("stats expose registry size and arbitration counters")
{
    FakeConnector connector;
    OrderManagementSystem oms{connector, nullptr, RiskConfig{}};
    REQUIRE(oms.place(buy_request()).is_ok());

    oms.on_execution_report(report("gw1", OrderState::Live, "0"));
    oms.on_execution_report(report("nope", OrderState::Live, "0"));

    const auto stats = oms.stats();
    CHECK(stats.known_orders == 1);
    CHECK(stats.reports_unknown == 1);
    CHECK((stats.reports_applied + stats.reports_stale) == 1);
    CHECK(stats.log_write_failures == 0);
}

} // namespace
