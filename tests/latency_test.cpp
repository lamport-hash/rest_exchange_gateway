#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "util/free_port.hpp"

#include "core/latency.hpp"
#include "core/oms.hpp"
#include "rest/order_routes.hpp"

#include <crow_all.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace gateway;

/// Deterministic time source: every read advances the clock by one
/// kStepNs, so successive reads form the fixed sequence kStepNs,
/// 2*kStepNs, 3*kStepNs, ... Measurements between two reads are then
/// exactly kStepNs regardless of real execution speed (testing
/// guidelines: mock time, never depend on wall-clock latency).
constexpr std::int64_t kStepNs = 1000;

class FakeClock
{
  public:
    std::atomic<std::int64_t> base{0};
};

auto fake_clock(FakeClock& a_clock) -> LatencyLog::Clock
{
    return [&a_clock] { return a_clock.base.fetch_add(kStepNs) + kStepNs; };
}

/// Scripted in-memory ExchangeConnector (same pattern as oms_test):
/// place acks "ord-<clientOrderId>"; everything else is a stub.
class FakeConnector final : public ExchangeConnector
{
  public:
    std::function<Result<OrderPlacement>(const OrderRequest&)> place_impl =
        [](const OrderRequest& a_request) {
            return OrderPlacement{.client_order_id = a_request.client_order_id,
                                  .exchange_order_id = "ord-" + a_request.client_order_id};
        };

    std::vector<OrderRequest> placed;

    [[nodiscard]] auto place_order(const OrderRequest& a_request) -> Result<OrderPlacement> override
    {
        placed.push_back(a_request);
        return place_impl(a_request);
    }
    [[nodiscard]] auto cancel_order(const CancelRequest&) -> Result<OrderPlacement> override
    {
        return Error{"not_found", "unsupported in this fixture"};
    }
    [[nodiscard]] auto amend_order(const AmendRequest&) -> Result<OrderPlacement> override
    {
        return Error{"not_found", "unsupported in this fixture"};
    }
    [[nodiscard]] auto get_order(const OrderQuery&) -> Result<std::optional<OrderSnapshot>> override
    {
        return Result<std::optional<OrderSnapshot>>{std::optional<OrderSnapshot>{std::nullopt}};
    }
    [[nodiscard]] auto get_open_orders() -> Result<std::vector<OrderSnapshot>> override
    {
        return std::vector<OrderSnapshot>{};
    }
    [[nodiscard]] auto get_price(const std::string&) -> Result<std::string> override
    {
        return Result<std::string>{std::string{"stub-price"}};
    }
    void
    set_execution_report_handler(std::function<void(const ExecutionReport&)> a_handler) override
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

auto temp_latency_path(const char* a_name) -> std::filesystem::path
{
    return std::filesystem::temp_directory_path() /
           ("gateway_latency_test_" + std::string(a_name) + ".jsonl");
}

auto buy_request(const std::string& a_id = "gw1") -> OrderRequest
{
    return OrderRequest{.client_order_id = a_id,
                        .instrument_id = "BTC-USDT",
                        .side = Side::Buy,
                        .type = OrderType::Limit,
                        .price = "50000",
                        .quantity = "1",
                        .time_in_force = ""};
}

auto fill_report(const std::string& a_id, OrderState a_state,
                 const std::string& a_filled) -> ExecutionReport
{
    return ExecutionReport{.client_order_id = a_id,
                           .exchange_order_id = "ord-" + a_id,
                           .state = a_state,
                           .side = Side::Buy,
                           .filled_quantity = a_filled,
                           .average_fill_price = "50000"};
}

/// Parse the latency log: one JSON object per non-empty line.
auto read_latency_lines(const std::filesystem::path& a_path) -> std::vector<nlohmann::json>
{
    std::vector<nlohmann::json> lines;
    std::ifstream file(a_path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(nlohmann::json::parse(line));
        }
    }
    return lines;
}

constexpr const char* kPlaceBody =
    R"({"clientOrderId":"gw1","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"1"})";

// --------------------------------------------------------------- test 1 ----

TEST_CASE("place: REST hit to venue send is measured and logged")
{
    const auto log_path = temp_latency_path("place_send");
    std::filesystem::remove(log_path);

    FakeClock clock;
    FakeConnector connector;
    LatencyLog latency{log_path, fake_clock(clock)};
    OrderManagementSystem oms{{{"okx", &connector}}, nullptr, RiskConfig{}, "okx", &latency};

    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, oms);
    const auto port = gateway::testing::pick_free_port();
    auto server = app.port(static_cast<int>(port))
                      .concurrency(1)
                      .loglevel(crow::LogLevel::Warning)
                      .run_async();
    app.wait_for_server_start();

    httplib::Client client{"127.0.0.1", static_cast<int>(port)};
    const auto response = client.Post("/orders", kPlaceBody, "application/json");

    // The normal case: the place reached the venue and was acked.
    REQUIRE(response != nullptr);
    REQUIRE(response->status == 201);
    REQUIRE(connector.placed.size() == 1);

    // Dangerous edge: a duplicate POST (same clientOrderId) is replayed
    // from the registry — nothing is re-sent, so no new measurement may
    // appear (a second latency line would double-count the place).
    const auto duplicate = client.Post("/orders", kPlaceBody, "application/json");
    REQUIRE(duplicate != nullptr);
    CHECK(duplicate->status == 201);
    CHECK(connector.placed.size() == 1);

    app.stop();
    server.wait();

    // Clock reads in causal order: rest_hit (handler entry), oms_entry
    // (place()), send (just before the venue call) -> 1000/2000/3000.
    // The duplicate re-reads the clock for its handler-entry stamp but
    // replays before any send, adding no lines.
    const auto lines = read_latency_lines(log_path);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0]["type"] == "latency");
    CHECK(lines[0]["phase"] == "place_send_rest");
    CHECK(lines[0]["clientOrderId"] == "gw1");
    CHECK(lines[0]["startNs"] == 1000);
    CHECK(lines[0]["endNs"] == 3000);
    CHECK(lines[0]["elapsedNs"] == 2000);
    CHECK(lines[1]["phase"] == "place_send_oms");
    CHECK(lines[1]["startNs"] == 2000);
    CHECK(lines[1]["endNs"] == 3000);
    CHECK(lines[1]["elapsedNs"] == 1000);

    std::filesystem::remove(log_path);
}

TEST_CASE("place: requests that never reach the venue log no place-send latency")
{
    const auto log_path = temp_latency_path("place_nosend");
    std::filesystem::remove(log_path);

    FakeClock clock;
    FakeConnector connector;
    LatencyLog latency{log_path, fake_clock(clock)};
    // maxQty 1: the "gw2" order below (quantity 99999) is rejected by
    // pre-trade risk and never reaches the venue.
    const auto risk = risk_config_from_json(
        nlohmann::json::parse(R"({"instruments":{"BTC-USDT":{"maxQty":"1"}}})"));
    REQUIRE(risk.is_ok());
    OrderManagementSystem oms{{{"okx", &connector}}, nullptr, risk.value(), "okx", &latency};

    // A malformed body: rejected by REST validation, no venue call.
    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, oms);
    const auto port = gateway::testing::pick_free_port();
    auto server = app.port(static_cast<int>(port))
                      .concurrency(1)
                      .loglevel(crow::LogLevel::Warning)
                      .run_async();
    app.wait_for_server_start();

    httplib::Client client{"127.0.0.1", static_cast<int>(port)};
    const auto invalid = client.Post("/orders", R"({"clientOrderId":"!!"})", "application/json");
    // A risk-rejected place: recorded, never sent.
    const auto risked = client.Post(
        "/orders",
        R"({"clientOrderId":"gw2","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"99999"})",
        "application/json");

    app.stop();
    server.wait();

    CHECK(invalid != nullptr);
    CHECK(invalid->status == 400);
    CHECK(risked != nullptr);
    CHECK(risked->status == 400);
    CHECK(connector.placed.empty());
    CHECK(read_latency_lines(log_path).empty());

    std::filesystem::remove(log_path);
}

// --------------------------------------------------------------- test 2 ----

TEST_CASE("fill: execution report received to registry state updated is measured and logged")
{
    const auto log_path = temp_latency_path("fill_state");
    std::filesystem::remove(log_path);

    FakeClock clock;
    FakeConnector connector;
    LatencyLog latency{log_path, fake_clock(clock)};
    OrderManagementSystem oms{{{"okx", &connector}}, nullptr, RiskConfig{}, "okx", &latency};

    const auto placed = oms.place(buy_request());
    REQUIRE(placed.is_ok());

    // The fill: received (report entry) then applied (state Filled and
    // persisted) — clock reads 3000/4000 -> exactly one kStepNs.
    oms.on_execution_report(fill_report("gw1", OrderState::Filled, "1"));

    const auto record = oms.query("gw1");
    REQUIRE(record.is_ok());
    CHECK(record.value().state == OrderState::Filled);
    CHECK(record.value().filled_quantity == "1");

    const auto lines = read_latency_lines(log_path);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0]["phase"] == "place_send_oms"); // internal place: no REST stamp
    CHECK(lines[1]["type"] == "latency");
    CHECK(lines[1]["phase"] == "fill_state_update");
    CHECK(lines[1]["clientOrderId"] == "gw1");
    CHECK(lines[1]["startNs"] == 3000);
    CHECK(lines[1]["endNs"] == 4000);
    CHECK(lines[1]["elapsedNs"] == 1000);

    // Dangerous edges — none of these update state, so none may log a
    // fill_state_update line (a line would measure an update that
    // never happened):
    // a duplicate fill (same high-water mark) is stale...
    oms.on_execution_report(fill_report("gw1", OrderState::Filled, "1"));
    // ...and an unknown clientOrderId is counted, not applied.
    oms.on_execution_report(fill_report("stranger", OrderState::Filled, "1"));

    CHECK(oms.stats().reports_stale == 1);
    CHECK(oms.stats().reports_unknown == 1);
    CHECK(read_latency_lines(log_path).size() == 2);

    std::filesystem::remove(log_path);
}

// --------------------------------------------------------------- test 3 ----

TEST_CASE("the latency log records one JSONL measurement per line, in order")
{
    const auto log_path = temp_latency_path("log_format");
    std::filesystem::remove(log_path);

    FakeClock clock;
    LatencyLog latency{log_path, fake_clock(clock)};

    // Normal case: two measurements, two flushed lines, each a
    // self-contained JSON object with the full schema. (Reads are
    // sequenced into named locals: function argument evaluation order
    // is unspecified, and a reversed start/end would be dropped by the
    // clock-anomaly guard under test below.)
    const auto place_start = latency.now();
    const auto place_end = latency.now();
    latency.measure("gw1", "place_send_rest", place_start, place_end);
    const auto fill_start = latency.now();
    const auto fill_end = latency.now();
    latency.measure("gw1", "fill_state_update", fill_start, fill_end);

    const auto lines = read_latency_lines(log_path);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0]["phase"] == "place_send_rest");
    CHECK(lines[0]["startNs"] == 1000);
    CHECK(lines[0]["endNs"] == 2000);
    CHECK(lines[0]["elapsedNs"] ==
          lines[0]["endNs"].get<std::int64_t>() - lines[0]["startNs"].get<std::int64_t>());
    CHECK(lines[1]["phase"] == "fill_state_update");
    CHECK(lines[1]["startNs"] == 3000);
    CHECK(lines[1]["endNs"] == 4000);
    CHECK(lines[1]["elapsedNs"] == 1000);
    CHECK(latency.path() == log_path);

    // Dangerous edges: a clock anomaly (end before start) and a
    // missing stamp (kNoLatencyStamp) are dropped, never logged as a
    // negative or nonsense duration.
    latency.measure("gw1", "place_send_rest", 5000, 4000);
    latency.measure("gw1", "place_send_rest", kNoLatencyStamp, 6000);
    CHECK(read_latency_lines(log_path).size() == 2);

    // Latency tracking disabled (nullptr sink): the trade paths work
    // and nothing is measured or written.
    const auto disabled_path = temp_latency_path("disabled");
    std::filesystem::remove(disabled_path);
    FakeConnector connector;
    OrderManagementSystem plain_oms{{{"okx", &connector}}, nullptr, RiskConfig{}};
    CHECK(plain_oms.latency_now() == kNoLatencyStamp);
    const auto outcome = plain_oms.place(buy_request());
    REQUIRE(outcome.is_ok());
    plain_oms.on_execution_report(fill_report("gw1", OrderState::Filled, "1"));
    CHECK(plain_oms.query("gw1").value().state == OrderState::Filled);
    CHECK_FALSE(std::filesystem::exists(disabled_path));

    std::filesystem::remove(log_path);
    std::filesystem::remove(disabled_path);
}

} // namespace
