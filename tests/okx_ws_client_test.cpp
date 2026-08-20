#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/okx_mock_ws_server.hpp"

#include "exchange/okx/okx_ws_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace {

using gateway::exchange::okx::FeedEvent;
using gateway::exchange::okx::FeedEventType;
using gateway::exchange::okx::OkxConfig;
using gateway::exchange::okx::OkxOrdersFeed;
using gateway::testing::OkxMockWsServer;

struct EventLog
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<FeedEvent> events;

    void record(FeedEvent a_event)
    {
        {
            const std::lock_guard lock(mutex);
            events.push_back(std::move(a_event));
        }
        cv.notify_all();
    }

    [[nodiscard]] auto count(FeedEventType a_type) -> int
    {
        const std::lock_guard lock(mutex);
        return count_locked(a_type);
    }

    [[nodiscard]] auto wait_for_count(FeedEventType a_type, int a_count, int a_timeout_ms) -> bool
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                           [&] { return count_locked(a_type) >= a_count; });
    }

  private:
    [[nodiscard]] auto count_locked(FeedEventType a_type) const -> int
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
};

auto base_config() -> OkxConfig
{
    return OkxConfig{.api_key = "test-key",
                     .secret_key = "test-secret",
                     .passphrase = "test-pass",
                     .host = "127.0.0.1",
                     .port = 0,
                     .use_tls = false,
                     .demo_trading = true,
                     .retry = gateway::RetryPolicy{},
                     .ws = gateway::exchange::okx::OkxWsConfig{}};
}

/// Fast, deterministic-ish feed config: sub-second pings, tiny backoffs.
auto feed_config(const OkxMockWsServer& a_server, OkxConfig a_base) -> OkxConfig
{
    a_base.ws.enabled = true;
    a_base.ws.host = "127.0.0.1";
    a_base.ws.port = static_cast<int>(a_server.port());
    a_base.ws.use_tls = false;
    a_base.ws.path = "/ws/v5/private";
    a_base.ws.ping_interval = std::chrono::milliseconds{60};
    a_base.ws.max_missed_pongs = 2;
    a_base.retry.initial_backoff = std::chrono::milliseconds{20};
    a_base.retry.max_backoff = std::chrono::milliseconds{60};
    a_base.retry.jitter = 0.0;
    return a_base;
}

auto order_item(const std::string& a_state, const std::string& a_fill = "0",
                const std::string& a_px = "") -> nlohmann::json
{
    return nlohmann::json{
        {"instId", "BTC-USDT"}, {"ordId", "ord-1"}, {"clOrdId", "gw1"}, {"state", a_state},
        {"side", "buy"},        {"px", "50000"},    {"sz", "1"},        {"accFillSz", a_fill},
        {"avgPx", a_px}};
}

TEST_CASE("feed logs in, subscribes and normalizes orders updates")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};

    ReportSink sink;
    EventLog events;
    feed.set_report_handler(
        [&sink](const gateway::ExecutionReport& a_report) { sink.record(a_report); });
    feed.set_event_handler([&events](const FeedEvent& a_event) { events.record(a_event); });
    feed.start();

    REQUIRE(feed.is_running());
    REQUIRE(server.wait_for_subscriber(5000));

    const auto stats = server.stats();
    CHECK(stats.logins_ok == 1);
    CHECK(stats.logins_failed == 0);
    CHECK(stats.handshake_targets.front() == "/ws/v5/private");
    CHECK(stats.saw_demo_header);
    CHECK(events.wait_for_count(FeedEventType::Connected, 1, 5000));

    server.push_orders_update(order_item("live"));
    REQUIRE(sink.wait_for(1, 5000));
    {
        const std::lock_guard lock(sink.mutex);
        const auto& report = sink.reports.front();
        CHECK(report.client_order_id == "gw1");
        CHECK(report.exchange_order_id == "ord-1");
        CHECK(report.state == gateway::OrderState::Live);
        CHECK(report.side == gateway::Side::Buy);
        CHECK(report.filled_quantity == "0");
    }

    server.push_orders_update(order_item("partially_filled", "0.4", "49999.5"));
    server.push_orders_update(order_item("filled", "1", "50000"));
    REQUIRE(sink.wait_for(3, 5000));

    feed.stop();
    feed.stop(); // idempotent
    CHECK_FALSE(feed.is_running());
    CHECK(events.wait_for_count(FeedEventType::Stopped, 1, 5000));
}

TEST_CASE("feed answers server pings with pong")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    EventLog events;
    feed.set_event_handler([&events](const FeedEvent& a_event) { events.record(a_event); });
    feed.start();
    REQUIRE(server.wait_for_subscriber(5000));

    server.send_text("ping");
    REQUIRE(server.wait_for_text("pong", 5000));

    feed.stop();
}

TEST_CASE("feed sends application-level pings to stay alive")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    feed.start();
    REQUIRE(server.wait_for_subscriber(5000));

    // With a 60ms interval the client must send "ping" within 300ms.
    REQUIRE(server.wait_for_text("ping", 5000));

    feed.stop();
}

TEST_CASE("feed reconnects, re-logs-in and re-subscribes after a clean kill")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    ReportSink sink;
    EventLog events;
    feed.set_report_handler(
        [&sink](const gateway::ExecutionReport& a_report) { sink.record(a_report); });
    feed.set_event_handler([&events](const FeedEvent& a_event) { events.record(a_event); });
    feed.start();
    REQUIRE(server.wait_for_subscriber(5000));
    CHECK(events.wait_for_count(FeedEventType::Connected, 1, 5000));

    server.push_orders_update(order_item("live"));
    REQUIRE(sink.wait_for(1, 5000));

    server.kill_connections();
    CHECK(events.wait_for_count(FeedEventType::Disconnected, 1, 5000));

    REQUIRE(server.wait_for_connections(2, 5000));
    REQUIRE(server.wait_for_received(3, 5000)); // second login+subscribe
    CHECK(events.wait_for_count(FeedEventType::Connected, 2, 5000));

    // updates flow again after the reconnect
    server.push_orders_update(order_item("canceled"));
    REQUIRE(sink.wait_for(2, 5000));
    {
        const std::lock_guard lock(sink.mutex);
        CHECK(sink.reports.back().state == gateway::OrderState::Canceled);
    }

    const auto stats = server.stats();
    CHECK(stats.logins_ok == 2);

    feed.stop();
}

TEST_CASE("feed survives the endpoint dying abruptly and coming back")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    ReportSink sink;
    EventLog events;
    feed.set_report_handler(
        [&sink](const gateway::ExecutionReport& a_report) { sink.record(a_report); });
    feed.set_event_handler([&events](const FeedEvent& a_event) { events.record(a_event); });
    feed.start();
    REQUIRE(server.wait_for_subscriber(5000));

    server.stop(); // abrupt: TCP EOF without WS close frames
    CHECK(events.wait_for_count(FeedEventType::Disconnected, 1, 5000));

    server.restart_on_same_port();
    REQUIRE(server.wait_for_connections(2, 5000));
    CHECK(events.wait_for_count(FeedEventType::Connected, 2, 5000));

    server.push_orders_update(order_item("live"));
    REQUIRE(sink.wait_for(1, 5000));

    feed.stop();
}

TEST_CASE("the watchdog closes a silent connection and reconnects")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    EventLog events;
    feed.set_event_handler([&events](const FeedEvent& a_event) { events.record(a_event); });
    feed.start();
    REQUIRE(server.wait_for_subscriber(5000));

    // Silence the server (no pong, no data). The feed must notice within
    // (max_missed_pongs + 1) * ping_interval, close, and reconnect.
    server.set_ignore_pings(true);
    CHECK(events.wait_for_count(FeedEventType::Disconnected, 1, 5000));

    server.set_ignore_pings(false);
    REQUIRE(server.wait_for_connections(2, 5000));
    CHECK(events.wait_for_count(FeedEventType::Connected, 2, 5000));

    feed.stop();
}

TEST_CASE("wrong credentials keep the feed retrying without Connected")
{
    OkxMockWsServer server(base_config());
    server.set_login_should_fail(true);
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    EventLog events;
    feed.set_event_handler([&events](const FeedEvent& a_event) { events.record(a_event); });
    feed.start();

    // at least two attempts (backoff loop alive) but never Connected
    REQUIRE(events.wait_for_count(FeedEventType::Connecting, 2, 5000));
    REQUIRE(server.wait_for_logins_failed(2, 5000));
    CHECK(events.count(FeedEventType::Connected) == 0);

    feed.stop();
}

TEST_CASE("duplicate and out-of-order updates are forwarded verbatim")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    ReportSink sink;
    feed.set_report_handler(
        [&sink](const gateway::ExecutionReport& a_report) { sink.record(a_report); });
    feed.start();
    REQUIRE(server.wait_for_subscriber(5000));

    SUBCASE("duplicate execution reports are both delivered")
    {
        server.set_duplicate_next_update();
        server.push_orders_update(order_item("filled", "1", "50000"));
        REQUIRE(sink.wait_for(2, 5000));
    }

    SUBCASE("out-of-order states arrive in wire order")
    {
        // wire order: filled, then a stale partially_filled
        server.push_orders_update(order_item("filled", "1", "50000"));
        server.push_orders_update(order_item("partially_filled", "0.4", "49999.5"));
        REQUIRE(sink.wait_for(2, 5000));
        const std::lock_guard lock(sink.mutex);
        CHECK(sink.reports[0].state == gateway::OrderState::Filled);
        CHECK(sink.reports[1].state == gateway::OrderState::PartiallyFilled);
    }

    feed.stop();
}

TEST_CASE("dropped updates are simply not delivered (loss is visible)")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    ReportSink sink;
    feed.set_report_handler(
        [&sink](const gateway::ExecutionReport& a_report) { sink.record(a_report); });
    feed.start();
    REQUIRE(server.wait_for_subscriber(5000));

    server.set_drop_next_updates(1);
    server.push_orders_update(order_item("live"));                 // lost by the venue
    server.push_orders_update(order_item("filled", "1", "50000")); // delivered

    REQUIRE(sink.wait_for(1, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds{120});
    const std::lock_guard lock(sink.mutex);
    REQUIRE(sink.reports.size() == 1);
    CHECK(sink.reports.front().state == gateway::OrderState::Filled);

    feed.stop();
}

TEST_CASE("unnormalizable updates raise a ProtocolWarning instead of killing the feed")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    ReportSink sink;
    EventLog events;
    feed.set_report_handler(
        [&sink](const gateway::ExecutionReport& a_report) { sink.record(a_report); });
    feed.set_event_handler([&events](const FeedEvent& a_event) { events.record(a_event); });
    feed.start();
    REQUIRE(server.wait_for_subscriber(5000));
    CHECK(events.wait_for_count(FeedEventType::Connected, 1, 5000));

    SUBCASE("unknown state")
    {
        server.push_orders_update(order_item("marginal"));
        REQUIRE(events.wait_for_count(FeedEventType::ProtocolWarning, 1, 5000));
    }

    SUBCASE("missing clOrdId")
    {
        auto item = order_item("live");
        item.erase("clOrdId");
        server.push_orders_update(item);
        REQUIRE(events.wait_for_count(FeedEventType::ProtocolWarning, 1, 5000));
    }

    SUBCASE("unknown side")
    {
        auto item = order_item("live");
        item["side"] = "short";
        server.push_orders_update(item);
        REQUIRE(events.wait_for_count(FeedEventType::ProtocolWarning, 1, 5000));
    }

    // the feed itself stays connected and keeps delivering good updates
    server.push_orders_update(order_item("live"));
    REQUIRE(sink.wait_for(1, 5000));
    CHECK(events.count(FeedEventType::Disconnected) == 0);

    feed.stop();
}

TEST_CASE("a disabled ws config makes start a no-op")
{
    OkxMockWsServer server(base_config());
    server.start();
    auto config = feed_config(server, base_config());
    config.ws.enabled = false;
    OkxOrdersFeed feed{config};
    feed.start();
    CHECK_FALSE(feed.is_running());
    feed.stop();
}

TEST_CASE("ws_host_for routes demo credentials to the demo WS host")
{
    auto config = base_config();
    config.demo_trading = true;
    config.ws.host = "ws.okx.com";
    CHECK(gateway::exchange::okx::ws_host_for(config) == "wspap.okx.com");

    config.demo_trading = false;
    CHECK(gateway::exchange::okx::ws_host_for(config) == "ws.okx.com");

    config.demo_trading = true;
    config.ws.host = "custom.host.example";
    CHECK(gateway::exchange::okx::ws_host_for(config) == "custom.host.example");

    config.ws.host = "wspap.okx.com";
    CHECK(gateway::exchange::okx::ws_host_for(config) == "wspap.okx.com");
}

TEST_CASE("the feed logs in with an epoch timestamp, not ISO 8601")
{
    OkxMockWsServer server(base_config());
    server.start();
    OkxOrdersFeed feed{feed_config(server, base_config())};
    feed.start();

    REQUIRE(server.wait_for_subscriber(5000));
    const auto login_timestamp = server.last_login_timestamp();
    CAPTURE(login_timestamp);
    CHECK(std::regex_match(login_timestamp, std::regex(R"(^\d{10}\.\d{3}$)")));

    feed.stop();
}

TEST_CASE("wrong login signature is rejected by the mock (harness self-check)")
{
    OkxMockWsServer server(base_config());
    server.start();
    auto config = feed_config(server, base_config());
    config.secret_key = "wrong-secret";
    OkxOrdersFeed feed{config};
    EventLog events;
    feed.set_event_handler([&events](const FeedEvent& a_event) { events.record(a_event); });
    feed.start();

    REQUIRE(events.wait_for_count(FeedEventType::Connecting, 2, 5000));
    REQUIRE(server.wait_for_logins_failed(2, 5000));
    CHECK(events.count(FeedEventType::Connected) == 0);

    feed.stop();
}

} // namespace
