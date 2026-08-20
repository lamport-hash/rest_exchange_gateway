#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/binance_mock_ws_server.hpp"

#include "exchange/binance/binance_connector.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using gateway::exchange::binance::BinanceConfig;
using gateway::exchange::binance::BinanceConnector;
using gateway::testing::BinanceMockWsServer;

struct ReportSink
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<gateway::ExecutionReport> reports;

    void record(const gateway::ExecutionReport& a_report)
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

    [[nodiscard]] auto latest() -> gateway::ExecutionReport
    {
        const std::lock_guard lock(mutex);
        return reports.back();
    }
};

auto fast_config(const BinanceMockWsServer& a_server) -> BinanceConfig
{
    return BinanceConfig{.api_key = "test-key",
                         .secret_key = "test-secret",
                         .host = "127.0.0.1",
                         .port = static_cast<int>(a_server.port()),
                         .use_tls = false,
                         .path = "/ws-api/v3",
                         .recv_window_ms = 60000,
                         .request_timeout = std::chrono::milliseconds{400},
                         .retry = gateway::RetryPolicy{}};
}

class ConnectorFixture
{
  public:
    ConnectorFixture()
    {
        server_ =
            std::make_unique<BinanceMockWsServer>(BinanceConfig{.api_key = "test-key",
                                                                .secret_key = "test-secret",
                                                                .host = "127.0.0.1",
                                                                .retry = gateway::RetryPolicy{}});
        server_->start();
        connector_ = std::make_unique<BinanceConnector>(fast_config(*server_));
        connector_->set_execution_report_handler(
            [this](const gateway::ExecutionReport& a_report) { reports_.record(a_report); });
    }

    ~ConnectorFixture()
    {
        connector_->stop();
        server_->stop();
    }

    auto place_request(const std::string& a_id) -> gateway::OrderRequest
    {
        return gateway::OrderRequest{.client_order_id = a_id,
                                     .instrument_id = "BTC-USDT",
                                     .side = gateway::Side::Buy,
                                     .type = gateway::OrderType::Limit,
                                     .price = "50000",
                                     .quantity = "0.1",
                                     .time_in_force = "GTC"};
    }

    std::unique_ptr<BinanceMockWsServer> server_;
    std::unique_ptr<BinanceConnector> connector_;
    ReportSink reports_;
};

} // namespace

TEST_CASE_FIXTURE(ConnectorFixture, "place maps the venue ack into a placement")
{
    connector_->start();
    const auto placement = connector_->place_order(place_request("co0001"));
    REQUIRE(placement.is_ok());
    CHECK(placement.value().client_order_id == "co0001");
    CHECK_FALSE(placement.value().exchange_order_id.empty());
    CHECK(server_->wait_for_places(1, 5000));
}

TEST_CASE_FIXTURE(ConnectorFixture, "venue rejections pass through with venue codes")
{
    connector_->start();
    REQUIRE(connector_->place_order(place_request("co0002")).is_ok());

    SUBCASE("cancel of an unknown order is a definitive venue:-2011")
    {
        const auto unknown = connector_->cancel_order(gateway::CancelRequest{"nope", "BTC-USDT"});
        REQUIRE_FALSE(unknown.is_ok());
        CHECK(unknown.error().code == "venue:-2011");
    }
    SUBCASE("cancel of an already-canceled order resolves idempotently")
    {
        REQUIRE(connector_->cancel_order(gateway::CancelRequest{"co0002", "BTC-USDT"}).is_ok());
        const auto again = connector_->cancel_order(gateway::CancelRequest{"co0002", "BTC-USDT"});
        REQUIRE(again.is_ok()); // -2011 resolved via order.status -> CANCELED
    }
}

TEST_CASE_FIXTURE(ConnectorFixture, "get_order: found snapshots, absent nullopt")
{
    connector_->start();
    REQUIRE(connector_->place_order(place_request("co0003")).is_ok());

    const auto found = connector_->get_order(gateway::OrderQuery{"co0003", "BTC-USDT"});
    REQUIRE(found.is_ok());
    REQUIRE(found.value().has_value());
    CHECK(found.value()->client_order_id == "co0003");
    CHECK(found.value()->instrument_id == "BTC-USDT"); // reverse-translated
    CHECK(found.value()->state == gateway::OrderState::Live);
    CHECK(found.value()->price == "50000"); // gateway spelling preserved

    const auto absent = connector_->get_order(gateway::OrderQuery{"never-placed", "BTC-USDT"});
    REQUIRE(absent.is_ok());
    CHECK(absent.value().has_value() == false);
}

TEST_CASE_FIXTURE(ConnectorFixture, "get_open_orders lists and reverse-translates symbols")
{
    connector_->start();
    REQUIRE(connector_->place_order(place_request("co0004")).is_ok());
    REQUIRE(connector_->place_order(place_request("co0005")).is_ok());

    const auto open = connector_->get_open_orders();
    REQUIRE(open.is_ok());
    CHECK(open.value().size() == 2);
    for (const auto& snapshot : open.value()) {
        CHECK(snapshot.instrument_id == "BTC-USDT");
        CHECK(snapshot.state == gateway::OrderState::Live);
    }
}

TEST_CASE_FIXTURE(ConnectorFixture, "amend emulates via cancelReplace and reports the new id")
{
    connector_->start();
    const auto placed = connector_->place_order(place_request("co0006"));
    REQUIRE(placed.is_ok());
    const std::string old_id = placed.value().exchange_order_id;

    const auto amended =
        connector_->amend_order(gateway::AmendRequest{.client_order_id = "co0006",
                                                      .instrument_id = "BTC-USDT",
                                                      .new_price = std::string{"48000"},
                                                      .new_quantity = std::string{"0.2"},
                                                      .side = gateway::Side::Buy,
                                                      .type = gateway::OrderType::Limit,
                                                      .time_in_force = "GTC"});
    REQUIRE(amended.is_ok());
    CHECK(amended.value().client_order_id == "co0006");
    CHECK_FALSE(amended.value().exchange_order_id.empty());
    CHECK(amended.value().exchange_order_id != old_id); // replacement order
    CHECK(server_->wait_for_amends(1, 5000));

    // the original is canceled, the replacement is live under the same id
    const auto snapshot = connector_->get_order(gateway::OrderQuery{"co0006", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == gateway::OrderState::Live);
    CHECK(snapshot.value()->quantity == "0.20000000");
}

TEST_CASE_FIXTURE(ConnectorFixture, "amend without replacement attributes is a protocol error")
{
    connector_->start();
    REQUIRE(connector_->place_order(place_request("co0007")).is_ok());
    const auto amended =
        connector_->amend_order(gateway::AmendRequest{.client_order_id = "co0007",
                                                      .instrument_id = "BTC-USDT",
                                                      .new_price = std::string{"48000"},
                                                      .new_quantity = std::string{"0.2"},
                                                      .side = std::nullopt,
                                                      .type = std::nullopt,
                                                      .time_in_force = ""});
    REQUIRE_FALSE(amended.is_ok());
    CHECK(amended.error().code == "protocol");
}

TEST_CASE_FIXTURE(ConnectorFixture, "execution reports are normalized and forwarded")
{
    connector_->start();
    REQUIRE(connector_->place_order(place_request("co0008")).is_ok());

    server_->apply_fill("co0008", "0.05", "51000");
    server_->push_execution_report("co0008");
    REQUIRE(reports_.wait_for(1, 5000));
    const auto& report = reports_.latest();
    CHECK(report.client_order_id == "co0008");
    CHECK(report.state == gateway::OrderState::PartiallyFilled);
    CHECK(report.side == gateway::Side::Buy);
    CHECK(report.filled_quantity == "0.05000000");
    CHECK(report.average_fill_price == "51000");
}

TEST_CASE_FIXTURE(ConnectorFixture, "a place whose ack is lost resolves to the landed order")
{
    connector_->start();
    server_->set_drop_next_response(); // place processed, ack lost

    const auto placement = connector_->place_order(place_request("co0009"));
    REQUIRE(placement.is_ok()); // resolved via order.status, never re-sent
    CHECK(placement.value().exchange_order_id.empty() == false);

    // exactly one venue place despite the lost ack + resolution
    REQUIRE(server_->wait_for_places(1, 5000));
    CHECK(server_->stats().places == 1);
    CHECK(server_->stats().status_queries >= 1);
}

TEST_CASE_FIXTURE(ConnectorFixture, "a cancel whose ack is lost resolves idempotently")
{
    connector_->start();
    REQUIRE(connector_->place_order(place_request("co0010")).is_ok());

    server_->set_drop_next_response();
    const auto canceled = connector_->cancel_order(gateway::CancelRequest{"co0010", "BTC-USDT"});
    REQUIRE(canceled.is_ok());
    CHECK(server_->stats().cancels == 1);
    CHECK(server_->stats().status_queries >= 1);
}

TEST_CASE_FIXTURE(ConnectorFixture, "an amend whose ack is lost resolves to the replacement")
{
    connector_->start();
    REQUIRE(connector_->place_order(place_request("co0011")).is_ok());

    server_->set_drop_next_response(); // cancelReplace processed, ack lost
    const auto amended =
        connector_->amend_order(gateway::AmendRequest{.client_order_id = "co0011",
                                                      .instrument_id = "BTC-USDT",
                                                      .new_price = std::string{"49000"},
                                                      .new_quantity = std::string{"0.1"},
                                                      .side = gateway::Side::Buy,
                                                      .type = gateway::OrderType::Limit,
                                                      .time_in_force = "GTC"});
    REQUIRE(amended.is_ok()); // found the live replacement under the same id
    CHECK(amended.value().client_order_id == "co0011");

    REQUIRE(server_->wait_for_amends(1, 5000));
    CHECK(server_->stats().amends == 1);

    const auto snapshot = connector_->get_order(gateway::OrderQuery{"co0011", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->price == "49000"); // numerically equal to 49000.00000000
    CHECK(snapshot.value()->state == gateway::OrderState::Live);
}

TEST_CASE_FIXTURE(ConnectorFixture, "duplicate clientOrderId places resolve to the existing order")
{
    connector_->start();
    const auto first = connector_->place_order(place_request("co0012"));
    REQUIRE(first.is_ok());

    // a second place with the same clientOrderId while the first is open
    // is rejected -4116 by the venue and resolved to the existing order
    const auto second = connector_->place_order(place_request("co0012"));
    REQUIRE(second.is_ok());
    CHECK(second.value().exchange_order_id == first.value().exchange_order_id);
    CHECK(server_->stats().places == 2); // two sends, one live order
}

TEST_CASE_FIXTURE(ConnectorFixture, "connectivity events reflect the session lifecycle")
{
    struct Counters
    {
        std::mutex mutex;
        std::condition_variable cv;
        int connected = 0;
        int disconnected = 0;
    };
    // shared_ptr: the handler runs on connector-owned threads that can
    // outlive this stack until the fixture tears the connector down.
    const auto counters = std::make_shared<Counters>();
    connector_->set_connectivity_handler([counters](bool a_up) {
        {
            const std::lock_guard lock(counters->mutex);
            a_up ? ++counters->connected : ++counters->disconnected;
        }
        counters->cv.notify_all();
    });

    connector_->start();
    {
        std::unique_lock lock(counters->mutex);
        REQUIRE(counters->cv.wait_for(lock, std::chrono::milliseconds{5000},
                                      [&] { return counters->connected >= 1; }));
    }

    server_->kill_connections();
    {
        std::unique_lock lock(counters->mutex);
        // reconnect after backoff fires Connected again
        REQUIRE(counters->cv.wait_for(lock, std::chrono::milliseconds{15000}, [&] {
            return counters->connected >= 2 && counters->disconnected >= 1;
        }));
    }

    connector_->stop(); // join the supervisor before Counters goes away
}
