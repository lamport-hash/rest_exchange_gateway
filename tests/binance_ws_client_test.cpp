#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/binance_mock_ws_server.hpp"

#include "exchange/binance/binance_ws_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace {

using gateway::exchange::binance::BinanceConfig;
using gateway::exchange::binance::BinanceFeedEvent;
using gateway::exchange::binance::BinanceFeedEventType;
using gateway::exchange::binance::BinanceWsClient;
using gateway::testing::BinanceMockWsServer;

struct EventLog
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<BinanceFeedEvent> events;

    void record(BinanceFeedEvent a_event)
    {
        {
            const std::lock_guard lock(mutex);
            events.push_back(std::move(a_event));
        }
        cv.notify_all();
    }

    [[nodiscard]] auto count(BinanceFeedEventType a_type) -> int
    {
        const std::lock_guard lock(mutex);
        return count_locked(a_type);
    }

    [[nodiscard]] auto wait_for_count(BinanceFeedEventType a_type, int a_count,
                                      int a_timeout_ms) -> bool
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                           [&] { return count_locked(a_type) >= a_count; });
    }

  private:
    [[nodiscard]] auto count_locked(BinanceFeedEventType a_type) const -> int
    {
        int total = 0;
        for (const auto& event : events) {
            total += event.type == a_type ? 1 : 0;
        }
        return total;
    }
};

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

    [[nodiscard]] auto size() -> std::size_t
    {
        const std::lock_guard lock(mutex);
        return reports.size();
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
                         .request_timeout = std::chrono::milliseconds{500},
                         .retry = gateway::RetryPolicy{}};
}

class WsFixture
{
  public:
    WsFixture()
    {
        server_ =
            std::make_unique<BinanceMockWsServer>(BinanceConfig{.api_key = "test-key",
                                                                .secret_key = "test-secret",
                                                                .host = "127.0.0.1",
                                                                .retry = gateway::RetryPolicy{}});
        server_->start();
        client_ = std::make_unique<BinanceWsClient>(fast_config(*server_));
        client_->set_report_handler(
            [this](const gateway::ExecutionReport& a_report) { reports_.record(a_report); });
        client_->set_event_handler(
            [this](const BinanceFeedEvent& a_event) { events_.record(a_event); });
    }

    ~WsFixture()
    {
        client_->stop();
        server_->stop();
    }

    std::unique_ptr<BinanceMockWsServer> server_;
    std::unique_ptr<BinanceWsClient> client_;
    EventLog events_;
    ReportSink reports_;
};

auto place_params() -> nlohmann::json
{
    return nlohmann::json{{"symbol", "BTCUSDT"},         {"side", "BUY"},    {"type", "LIMIT"},
                          {"timeInForce", "GTC"},        {"price", "50000"}, {"quantity", "0.1"},
                          {"newClientOrderId", "ws0001"}};
}

} // namespace

TEST_CASE_FIXTURE(WsFixture, "connect, subscribe the user data stream, and place an order")
{
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));
    REQUIRE(server_->wait_for_subscriber(5000));

    const auto result = client_->call_signed("order.place", place_params());
    REQUIRE(result.is_ok());
    CHECK(result.value()["clientOrderId"] == "ws0001");
    CHECK(result.value()["status"] == "NEW");
    CHECK(result.value().contains("orderId"));
    CHECK(server_->stats().places == 1);
}

TEST_CASE("a feed event handler may issue synchronous venue calls")
{
    // Regression: connectivity handlers run reconciliation (get_open_orders
    // etc.). When events were delivered on the reader loop, such a handler
    // deadlocked until request_timeout because only the blocked reader
    // could dispatch its response. Events are now delivered on a notifier
    // thread, so the handler's call must succeed promptly.
    BinanceMockWsServer server{BinanceConfig{.api_key = "test-key",
                                             .secret_key = "test-secret",
                                             .host = "127.0.0.1",
                                             .retry = gateway::RetryPolicy{}}};
    server.start();
    auto client = std::make_unique<BinanceWsClient>(fast_config(server));

    std::mutex mutex;
    std::condition_variable cv;
    bool handler_done = false;
    client->set_event_handler([&](const BinanceFeedEvent& a_event) {
        if (a_event.type != BinanceFeedEventType::Connected) {
            return;
        }
        const auto result = client->call_signed("openOrders.status",
                                                nlohmann::json{{"symbol", "BTCUSDT"}});
        if (!result.is_ok()) {
            return; // leave handler_done false: the test asserts success
        }
        const std::lock_guard lock(mutex);
        handler_done = true;
        cv.notify_all();
    });
    client->start();

    bool ok = false;
    {
        std::unique_lock lock(mutex);
        ok = cv.wait_for(lock, std::chrono::milliseconds{3000}, [&] { return handler_done; });
    }
    CHECK(ok); // the handler's synchronous venue call completed (no timeout)
    client->stop();
    server.stop();
}

TEST_CASE_FIXTURE(WsFixture, "venue rejections carry venue:<code> and 5xx become transport")
{
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));

    SUBCASE("unknown order status query is venue:-2013")
    {
        const auto result = client_->call_signed(
            "order.status", nlohmann::json{{"symbol", "BTCUSDT"}, {"origClientOrderId", "none"}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:-2013");
    }
    SUBCASE("malformed method is a venue error")
    {
        const auto result = client_->call_signed("bogus.method", nlohmann::json::object());
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code.rfind("venue:", 0) == 0);
    }
    SUBCASE("a tampered signature is rejected with -1022")
    {
        auto params = place_params();
        params["apiKey"] = "test-key";
        params["recvWindow"] = 60000;
        params["timestamp"] = gateway::exchange::binance::real_unix_ms();
        params["signature"] = "deadbeef";
        const auto result = client_->call("order.place", params);
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:-1022");
    }
}

TEST_CASE_FIXTURE(WsFixture, "executionReport events interleave with pending requests")
{
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));
    REQUIRE(server_->wait_for_subscriber(5000));
    REQUIRE(client_->call_signed("order.place", place_params()).is_ok());

    // Delay the next (status) reply so the report certainly lands while a
    // request is outstanding; both must be handled independently.
    server_->apply_fill("ws0001", "0.05", "50000");
    server_->set_delay_next_response(200);
    server_->push_execution_report("ws0001");

    const auto status = client_->call_signed(
        "order.status", nlohmann::json{{"symbol", "BTCUSDT"}, {"origClientOrderId", "ws0001"}});
    REQUIRE(status.is_ok());
    CHECK(status.value()["status"] == "PARTIALLY_FILLED");

    REQUIRE(reports_.wait_for(1, 5000));
    const auto& report = reports_.latest();
    CHECK(report.client_order_id == "ws0001");
    CHECK(report.state == gateway::OrderState::PartiallyFilled);
    CHECK(report.filled_quantity == "0.05000000");
    // average price = cummulativeQuoteQty / executedQty = 2500 / 0.05
    CHECK(report.average_fill_price == "50000");
}

TEST_CASE_FIXTURE(WsFixture, "cancel reports are keyed by the original clientOrderId")
{
    // Regression: on CANCELED executionReports the venue puts its
    // auto-generated cancel id in "c" and the ORIGINAL clientOrderId in
    // "C"; the normalized report must stay keyed by the original.
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));
    REQUIRE(server_->wait_for_subscriber(5000));

    server_->push_raw_frame(
        R"({"subscriptionId":0,"event":{"e":"executionReport","E":1,"s":"BTCUSDT",)"
        R"("c":"gAoVEW9zYFs8jreejCDBR6","C":"ws0001","S":"BUY","o":"LIMIT",)"
        R"("X":"CANCELED","i":42,"z":"0.00000000"}})");
    REQUIRE(reports_.wait_for(1, 5000));
    const auto& report = reports_.latest();
    CHECK(report.client_order_id == "ws0001");
    CHECK(report.state == gateway::OrderState::Canceled);
}

TEST_CASE_FIXTURE(WsFixture, "duplicate execution reports are delivered verbatim"){
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));
    REQUIRE(server_->wait_for_subscriber(5000));
    REQUIRE(client_->call_signed("order.place", place_params()).is_ok());

    server_->set_duplicate_next_update();
    server_->push_execution_report("ws0001");
    REQUIRE(reports_.wait_for(2, 5000));
    CHECK(reports_.size() >= 2);
}

TEST_CASE_FIXTURE(WsFixture, "a dropped response times out as transport (outcome unknown)")
{
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));

    server_->set_drop_next_response();
    const auto result = client_->call_signed("order.place", place_params());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "transport");
    // the order DID land on the venue
    CHECK(server_->stats().places == 1);
    CHECK(server_->stats().responses_dropped == 1);
}

TEST_CASE_FIXTURE(WsFixture, "requests before start fail fast as transport")
{
    const auto result = client_->call_signed("order.place", place_params());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "transport");
}

TEST_CASE_FIXTURE(WsFixture, "disconnects fail pending requests and trigger a reconnect")
{
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));
    REQUIRE(server_->wait_for_connections(1, 5000));

    // hold one request in flight and kill the connection mid-flight (the
    // call blocks its thread, so the killer must be launched first)
    server_->set_delay_next_response(300);
    std::thread killer([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        server_->kill_connections();
    });
    killer.detach();

    const auto status = client_->call_signed(
        "order.status", nlohmann::json{{"symbol", "BTCUSDT"}, {"origClientOrderId", "none"}});

    REQUIRE_FALSE(status.is_ok());
    CHECK(status.error().code == "transport");
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Disconnected, 1, 5000));

    // supervisor reconnects and re-subscribes the user data stream
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 2, 10000));
    REQUIRE(server_->wait_for_connections(2, 10000));
    CHECK(server_->stats().subscribes >= 2);
}

TEST_CASE_FIXTURE(WsFixture, "an abruptly dead endpoint is retried until it returns")
{
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));

    server_->kill_connections();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Disconnected, 1, 5000));

    server_->restart_on_same_port();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 2, 15000));
    REQUIRE(server_->wait_for_connections(2, 15000));
    REQUIRE(server_->wait_for_subscriber(15000));
}

TEST_CASE_FIXTURE(WsFixture, "stop is idempotent and joins the supervisor")
{
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));
    client_->stop();
    client_->stop();
    CHECK(client_->is_running() == false);
}
