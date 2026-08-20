#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/okx_mock_server.hpp"
#include "mocks/okx_mock_ws_server.hpp"
#include "util/free_port.hpp"

#include "exchange/okx/okx_connector.hpp"
#include "gateway/exchange_connector.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace {

using gateway::testing::OkxMockServer;
using gateway::testing::OkxMockWsServer;
using namespace gateway;
using namespace gateway::exchange::okx;

auto base_config() -> OkxConfig
{
    OkxConfig config{.api_key = "test-key",
                     .secret_key = "test-secret",
                     .passphrase = "test-pass",
                     .host = "127.0.0.1",
                     .port = 0,
                     .use_tls = false,
                     .demo_trading = true,
                     .retry = gateway::RetryPolicy{},
                     .ws = gateway::exchange::okx::OkxWsConfig{}};
    // fast retries so fault-injection tests stay snappy
    config.retry.initial_backoff = std::chrono::milliseconds{10};
    config.retry.max_backoff = std::chrono::milliseconds{30};
    config.retry.jitter = 0.0;
    config.retry.budget = std::chrono::milliseconds{2000};
    return config;
}

auto limit_buy() -> OrderRequest
{
    return OrderRequest{.client_order_id = "gw0001",
                        .instrument_id = "BTC-USDT",
                        .side = Side::Buy,
                        .type = OrderType::Limit,
                        .price = "50000",
                        .quantity = "0.001",
                        .time_in_force = ""};
}

auto make_connector(const OkxMockServer& a_server) -> OkxConnector
{
    auto config = base_config();
    config.port = static_cast<int>(a_server.port());
    return OkxConnector(config, [] { return std::string("2026-08-20T10:00:00.000Z"); });
}

auto count_recorded(const OkxMockServer& a_server, const std::string& a_method,
                    const std::string& a_target_prefix) -> int
{
    int total = 0;
    for (const auto& request : a_server.recorded_requests()) {
        if (request.method == a_method && request.target.rfind(a_target_prefix, 0) == 0) {
            ++total;
        }
    }
    return total;
}

TEST_CASE("place then get through the ExchangeConnector interface")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    const auto placement = connector_interface.place_order(limit_buy());
    REQUIRE(placement.is_ok());
    CHECK(placement.value().client_order_id == "gw0001");
    CHECK(placement.value().exchange_order_id == "mock-1");

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Live);
    CHECK(snapshot.value()->price == "50000");
    CHECK(snapshot.value()->quantity == "0.001");
    CHECK(snapshot.value()->filled_quantity == "0");
    CHECK(snapshot.value()->average_fill_price.empty());
}

TEST_CASE("partial fill surfaces as PartiallyFilled snapshot")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    server.apply_fill("gw0001", "0.0004", "49999.5");

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::PartiallyFilled);
    CHECK(snapshot.value()->instrument_id == "BTC-USDT");
    CHECK(snapshot.value()->side == Side::Buy);
    CHECK(snapshot.value()->type == OrderType::Limit);
    CHECK(snapshot.value()->filled_quantity == "0.0004");
    CHECK(snapshot.value()->average_fill_price == "49999.5");
}

TEST_CASE("get_open_orders returns normalized pending snapshots")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    server.apply_fill("gw0001", "0.0004", "49999.5");

    const auto open = connector_interface.get_open_orders();
    REQUIRE(open.is_ok());
    REQUIRE(open.value().size() == 1);
    const auto& snapshot = open.value().front();
    CHECK(snapshot.client_order_id == "gw0001");
    CHECK(snapshot.exchange_order_id == "mock-1");
    CHECK(snapshot.instrument_id == "BTC-USDT");
    CHECK(snapshot.state == OrderState::PartiallyFilled);
    CHECK(snapshot.side == Side::Buy);
    CHECK(snapshot.type == OrderType::Limit);
    CHECK(snapshot.filled_quantity == "0.0004");

    // a fully filled order disappears from the pending listing
    server.apply_fill("gw0001", "0.0006", "50000");
    const auto after_fill = connector_interface.get_open_orders();
    REQUIRE(after_fill.is_ok());
    CHECK(after_fill.value().empty());
}

TEST_CASE("cancel transitions the snapshot to Canceled")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    const auto cancel = connector_interface.cancel_order(CancelRequest{"gw0001", "BTC-USDT"});
    REQUIRE(cancel.is_ok());
    CHECK(cancel.value().exchange_order_id == "mock-1");

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Canceled);
}

TEST_CASE("amend updates price through the interface")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    const auto amend = connector_interface.amend_order(
        AmendRequest{"gw0001", "BTC-USDT", std::string("51000"), std::nullopt, Side::Buy,
                     OrderType::Limit, "GTC"});
    REQUIRE(amend.is_ok());

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->price == "51000");
}

TEST_CASE("side and type map to OKX wire values")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    OrderRequest market_sell = limit_buy();
    market_sell.client_order_id = "gw0002";
    market_sell.side = Side::Sell;
    market_sell.type = OrderType::Market;
    market_sell.price.clear();
    REQUIRE(connector_interface.place_order(market_sell).is_ok());

    const auto recorded = server.recorded_requests();
    REQUIRE(recorded.size() == 1);
    const auto body = nlohmann::json::parse(recorded.front().body);
    CHECK(body.at("side") == "sell");
    CHECK(body.at("ordType") == "market");
    CHECK_FALSE(body.contains("px"));

    OrderRequest limit = limit_buy();
    limit.client_order_id = "gw0003";
    limit.side = Side::Sell;
    REQUIRE(connector_interface.place_order(limit).is_ok());
    CHECK(nlohmann::json::parse(server.recorded_requests().back().body).at("side") == "sell");
}

TEST_CASE("get_order: unknown order is a conclusive absence (nullopt), not an error")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    // OKX reports "51603 The order does not exist"; the venue-agnostic
    // contract maps conclusive absence to std::nullopt so the core treats
    // every exchange identically (Binance -2013 behaves the same way).
    const auto snapshot = connector_interface.get_order(OrderQuery{"ghost", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value() == false);
}

TEST_CASE("venue errors pass through the interface unchanged")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    const auto result = connector_interface.cancel_order(CancelRequest{"nope", "BTC-USDT"});
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "venue:51016");
}

TEST_CASE("amend with no changes is rejected before the network")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    const auto result = connector_interface.amend_order(AmendRequest{
        "gw0001", "BTC-USDT", std::nullopt, std::nullopt, Side::Buy, OrderType::Limit, "GTC"});
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
    CHECK(server.recorded_requests().empty());
}

TEST_CASE("execution report handler can be installed and replaced")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);

    int calls = 0;
    connector.set_execution_report_handler([&calls](const ExecutionReport&) { ++calls; });
    connector.set_execution_report_handler([](const ExecutionReport&) {});
    CHECK(calls == 0);
}

TEST_CASE("map_okx_state covers documented states and rejects unknowns")
{
    CHECK(map_okx_state("live") == OrderState::Live);
    CHECK(map_okx_state("partially_filled") == OrderState::PartiallyFilled);
    CHECK(map_okx_state("filled") == OrderState::Filled);
    CHECK(map_okx_state("canceled") == OrderState::Canceled);
    CHECK_FALSE(map_okx_state("marginal").has_value());
    CHECK_FALSE(map_okx_state("").has_value());
}

TEST_CASE("to_string renders every OrderState")
{
    CHECK(to_string(OrderState::Live) == "live");
    CHECK(to_string(OrderState::PartiallyFilled) == "partially_filled");
    CHECK(to_string(OrderState::Filled) == "filled");
    CHECK(to_string(OrderState::Canceled) == "canceled");
    CHECK(to_string(OrderState::Rejected) == "rejected");
}

TEST_CASE("dropped place response resolves via lookup: exactly one live order")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    server.drop_next_request();
    const auto placement = connector_interface.place_order(limit_buy());
    REQUIRE(placement.is_ok());
    CHECK(placement.value().exchange_order_id == "mock-1");

    // the dropped place was re-sent exactly once, after a lookup in between
    CHECK(count_recorded(server, "POST", "/api/v5/trade/order") == 2);
    CHECK(count_recorded(server, "GET", "/api/v5/trade/order") == 1);
    const auto recorded = server.recorded_requests();
    REQUIRE(recorded.size() == 3);
    CHECK(recorded[0].method == "POST");
    CHECK(recorded[1].method == "GET");
    CHECK(recorded[2].method == "POST");

    // no double-applied order: the venue holds exactly one live gw0001
    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Live);
    CHECK(snapshot.value()->exchange_order_id == "mock-1");
}

TEST_CASE("place processed but its acknowledgement dropped resolves without re-send")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    server.drop_next_response();
    const auto placement = connector_interface.place_order(limit_buy());
    REQUIRE(placement.is_ok());
    CHECK(placement.value().exchange_order_id == "mock-1");

    // the order exists at the venue (lookup found it), so it was never re-sent
    CHECK(count_recorded(server, "POST", "/api/v5/trade/order") == 1);
    CHECK(count_recorded(server, "GET", "/api/v5/trade/order") == 1);

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Live);
}

TEST_CASE("response slower than the read timeout retries the place")
{
    OkxMockServer server(base_config());
    server.start();
    auto config = base_config();
    config.port = static_cast<int>(server.port());
    config.rest_read_timeout_ms = 150;
    OkxConnector connector{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};
    ExchangeConnector& connector_interface = connector;

    server.delay_next_request(400); // beyond the 150ms read timeout
    const auto placement = connector_interface.place_order(limit_buy());
    REQUIRE(placement.is_ok());
    CHECK(placement.value().exchange_order_id == "mock-1");
    CHECK(count_recorded(server, "POST", "/api/v5/trade/order") == 2);
}

TEST_CASE("the same clientOrderId placed twice returns the same order")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    const auto first = connector_interface.place_order(limit_buy());
    REQUIRE(first.is_ok());
    const auto second = connector_interface.place_order(limit_buy());
    REQUIRE(second.is_ok());
    CHECK(second.value().exchange_order_id == first.value().exchange_order_id);
    CHECK(count_recorded(server, "POST", "/api/v5/trade/order") == 2);
    CHECK(count_recorded(server, "GET", "/api/v5/trade/order") == 1);
}

TEST_CASE("cancel is idempotent: cancelling twice both succeed")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());

    const auto first = connector_interface.cancel_order(CancelRequest{"gw0001", "BTC-USDT"});
    REQUIRE(first.is_ok());
    const auto second = connector_interface.cancel_order(CancelRequest{"gw0001", "BTC-USDT"});
    REQUIRE(second.is_ok());
    CHECK(second.value().exchange_order_id == "mock-1");

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Canceled);
}

TEST_CASE("cancel with a filled order stays a venue rejection")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    server.apply_fill("gw0001", "0.001", "50000");

    const auto cancel = connector_interface.cancel_order(CancelRequest{"gw0001", "BTC-USDT"});
    REQUIRE_FALSE(cancel.is_ok());
    CHECK(cancel.error().code == "venue:51017");
}

TEST_CASE("a dropped cancel request is re-sent safely")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    server.drop_next_request();
    const auto cancel = connector_interface.cancel_order(CancelRequest{"gw0001", "BTC-USDT"});
    REQUIRE(cancel.is_ok());

    // the dropped cancel was never processed; lookup saw the order live and
    // the cancel was re-sent once
    CHECK(count_recorded(server, "POST", "/api/v5/trade/cancel-order") == 2);
    CHECK(count_recorded(server, "GET", "/api/v5/trade/order") == 1);

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Canceled);
}

TEST_CASE("cancel processed but its acknowledgement dropped resolves without re-send")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    server.drop_next_response();
    const auto cancel = connector_interface.cancel_order(CancelRequest{"gw0001", "BTC-USDT"});
    REQUIRE(cancel.is_ok());
    CHECK(cancel.value().exchange_order_id == "mock-1");

    // first cancel landed (lookup sees canceled), so it was never re-sent
    CHECK(count_recorded(server, "POST", "/api/v5/trade/cancel-order") == 1);
    CHECK(count_recorded(server, "GET", "/api/v5/trade/order") == 1);
}

TEST_CASE("dropped amend response re-sends until the price matches")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    server.drop_next_request();
    const auto amend = connector_interface.amend_order(
        AmendRequest{"gw0001", "BTC-USDT", std::string("51000"), std::nullopt, Side::Buy,
                     OrderType::Limit, "GTC"});
    REQUIRE(amend.is_ok());

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->price == "51000");
    CHECK(count_recorded(server, "POST", "/api/v5/trade/amend-order") == 2);
}

TEST_CASE("get_order transparently retries a dropped response")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    server.drop_next_request();
    const auto snapshot = connector_interface.get_order(OrderQuery{"gw0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Live);
    CHECK(count_recorded(server, "GET", "/api/v5/trade/order") == 2);
}

TEST_CASE("venue unreachable: place fails with transport and never double-places")
{
    const auto dead_port = gateway::testing::pick_free_port();
    auto config = base_config();
    config.port = static_cast<int>(dead_port);
    OkxConnector connector{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};
    ExchangeConnector& connector_interface = connector;

    const auto placement = connector_interface.place_order(limit_buy());
    REQUIRE_FALSE(placement.is_ok());
    CHECK(placement.error().code == "transport");
}

struct ReportLatch
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ExecutionReport> reports;

    void record(const ExecutionReport& a_report)
    {
        {
            const std::lock_guard lock(mutex);
            reports.push_back(a_report);
        }
        cv.notify_all();
    }

    [[nodiscard]] auto wait_for(std::size_t a_count, int a_timeout_ms) -> bool
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                           [&] { return reports.size() >= a_count; });
    }
};

TEST_CASE("start/stop control the orders feed end-to-end")
{
    OkxMockServer rest_server(base_config());
    rest_server.start();
    OkxMockWsServer ws_server(base_config());
    ws_server.start();

    auto config = base_config();
    config.port = static_cast<int>(rest_server.port());
    config.ws.enabled = true;
    config.ws.host = "127.0.0.1";
    config.ws.port = static_cast<int>(ws_server.port());
    config.ws.use_tls = false;
    config.ws.path = "/ws/v5/private";
    config.ws.ping_interval = std::chrono::milliseconds{60};
    OkxConnector connector{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};

    ReportLatch latch;
    connector.set_execution_report_handler(
        [&latch](const ExecutionReport& a_report) { latch.record(a_report); });
    connector.start();
    connector.start(); // idempotent

    REQUIRE(ws_server.wait_for_subscriber(5000));

    ws_server.push_orders_update(nlohmann::json{{"instId", "BTC-USDT"},
                                                {"ordId", "ord-1"},
                                                {"clOrdId", "gw0001"},
                                                {"state", "partially_filled"},
                                                {"side", "buy"},
                                                {"px", "50000"},
                                                {"sz", "1"},
                                                {"accFillSz", "0.4"},
                                                {"avgPx", "49999.5"}});
    REQUIRE(latch.wait_for(1, 5000));
    {
        const std::lock_guard lock(latch.mutex);
        CHECK(latch.reports.front().client_order_id == "gw0001");
        CHECK(latch.reports.front().state == OrderState::PartiallyFilled);
        CHECK(latch.reports.front().filled_quantity == "0.4");
    }

    connector.stop();
    connector.stop(); // idempotent
}

TEST_CASE("connectivity handler reflects the feed lifecycle")
{
    OkxMockServer rest_server(base_config());
    rest_server.start();
    OkxMockWsServer ws_server(base_config());
    ws_server.start();

    auto config = base_config();
    config.port = static_cast<int>(rest_server.port());
    config.ws.enabled = true;
    config.ws.host = "127.0.0.1";
    config.ws.port = static_cast<int>(ws_server.port());
    config.ws.use_tls = false;
    config.ws.path = "/ws/v5/private";
    config.ws.ping_interval = std::chrono::milliseconds{60};
    OkxConnector connector{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};

    // latch recording the sequence of connectivity transitions
    struct Latch
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<bool> events;

        void record(bool a_up)
        {
            {
                const std::lock_guard lock(mutex);
                events.push_back(a_up);
            }
            cv.notify_all();
        }

        [[nodiscard]] auto wait_for_count(std::size_t a_count, int a_timeout_ms) -> bool
        {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                               [&] { return events.size() >= a_count; });
        }
    } latch;

    connector.set_connectivity_handler([&latch](bool a_up) { latch.record(a_up); });
    connector.start();
    REQUIRE(ws_server.wait_for_subscriber(5000));
    REQUIRE(latch.wait_for_count(1, 5000));
    {
        const std::lock_guard lock(latch.mutex);
        CHECK(latch.events.front() == true); // connected
    }

    ws_server.kill_connections();
    REQUIRE(latch.wait_for_count(2, 5000));
    {
        const std::lock_guard lock(latch.mutex);
        CHECK(latch.events[1] == false); // dropped
    }

    connector.stop();
}

TEST_CASE("start is a no-op when the ws feed is disabled")
{
    OkxMockServer server(base_config());
    server.start();
    auto config = base_config();
    config.port = static_cast<int>(server.port());
    config.ws.enabled = false;
    OkxConnector connector{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};
    connector.start();
    connector.stop();
    CHECK(server.recorded_requests().empty());
}

} // namespace
