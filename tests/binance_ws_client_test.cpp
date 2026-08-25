#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/binance_mock_ws_server.hpp"

#include "exchange/binance/binance_ws_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

auto count_method_frames(const BinanceMockWsServer& a_server, const char* a_method) -> int
{
    const std::string needle = std::string{"\"method\":\""} + a_method + "\"";
    int total = 0;
    for (const auto& frame : a_server.stats().received) {
        total += frame.find(needle) != std::string::npos ? 1 : 0;
    }
    return total;
}

/// A dumb TCP proxy that can freeze forwarding in both directions: the
/// established connections stay up but no bytes move — a half-open,
/// silent socket from the client's point of view. This is the only way
/// to drill the WS pong watchdog against the in-process mock: the mock's
/// ws.read() auto-answers protocol pings inside the blocked read, so a
/// "frozen handler" cannot silence them; silencing must happen on the
/// wire.
class SilentProxy
{
  public:
    SilentProxy() = default;
    ~SilentProxy() { stop(); }
    SilentProxy(const SilentProxy&) = delete;
    auto operator=(const SilentProxy&) -> SilentProxy& = delete;

    void start(std::uint16_t a_target_port)
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            return;
        }
        socklen_t len = sizeof addr;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        {
            const std::lock_guard lock(state_mutex_);
            running_ = true;
        }
        acceptor_ = std::thread([this, a_target_port] { accept_loop(a_target_port); });
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return port_; }

    /// Freeze (true) / resume (false) byte forwarding in both directions.
    void set_silent(bool a_silent)
    {
        {
            const std::lock_guard lock(state_mutex_);
            silent_ = a_silent;
        }
        state_cv_.notify_all();
    }

    void stop()
    {
        {
            const std::lock_guard lock(state_mutex_);
            running_ = false;
        }
        state_cv_.notify_all();
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (acceptor_.joinable()) {
            acceptor_.join();
        }
        std::vector<int> fds;
        std::vector<std::thread> pumps;
        {
            const std::lock_guard lock(state_mutex_);
            fds = std::move(open_fds_);
            pumps = std::move(pumps_);
        }
        // shutdown unblocks pumps blocked in recv
        for (int fd : fds) {
            ::shutdown(fd, SHUT_RDWR);
        }
        for (auto& pump : pumps) {
            if (pump.joinable()) {
                pump.join();
            }
        }
        for (int fd : fds) {
            ::close(fd);
        }
    }

  private:
    void accept_loop(std::uint16_t a_target_port)
    {
        while (true) {
            {
                const std::lock_guard lock(state_mutex_);
                if (!running_) {
                    return;
                }
            }
            const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
            if (client_fd < 0) {
                return;
            }
            const int upstream_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(a_target_port);
            if (upstream_fd < 0 ||
                ::connect(upstream_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
                ::close(client_fd);
                if (upstream_fd >= 0) {
                    ::close(upstream_fd);
                }
                continue;
            }
            const std::lock_guard lock(state_mutex_);
            open_fds_.push_back(client_fd);
            open_fds_.push_back(upstream_fd);
            pumps_.emplace_back([this, client_fd, upstream_fd] { pump(client_fd, upstream_fd); });
            pumps_.emplace_back([this, client_fd, upstream_fd] { pump(upstream_fd, client_fd); });
        }
    }

    void pump(int a_from, int a_to)
    {
        char buffer[4096];
        while (true) {
            {
                std::unique_lock lock(state_mutex_);
                state_cv_.wait(lock, [this] { return !silent_ || !running_; });
                if (!running_) {
                    return;
                }
            }
            const ssize_t received = ::recv(a_from, buffer, sizeof buffer, 0);
            if (received <= 0) {
                return;
            }
            {
                // bytes that arrived just as silence began are held too
                std::unique_lock lock(state_mutex_);
                state_cv_.wait(lock, [this] { return !silent_ || !running_; });
                if (!running_) {
                    return;
                }
                ssize_t sent = 0;
                while (sent < received) {
                    const ssize_t n = ::send(a_to, buffer + sent,
                                             static_cast<size_t>(received - sent), MSG_NOSIGNAL);
                    if (n <= 0) {
                        return;
                    }
                    sent += n;
                }
            }
        }
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread acceptor_;
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool running_ = false;
    bool silent_ = false;
    std::vector<int> open_fds_;
    std::vector<std::thread> pumps_;
};

} // namespace

TEST_CASE("-1021 clock skew: sync, re-sign once, and reuse the learned offset")
{
    using gateway::exchange::binance::real_unix_ms;

    BinanceMockWsServer server{BinanceConfig{.api_key = "test-key",
                                             .secret_key = "test-secret",
                                             .host = "127.0.0.1",
                                             .retry = gateway::RetryPolicy{}}};
    server.start();
    auto config = fast_config(server);
    config.recv_window_ms = 5000;

    // state objects are declared BEFORE the client: on an assertion
    // failure doctest unwinds without calling client.stop(), so the
    // client (and its threads) must be destroyed first.
    // The client clock drifts MID-SESSION (skew starts at zero so the
    // session subscribes cleanly, then jumps 120s into the past).
    std::atomic<long long> skew_ms{0};
    EventLog events;
    BinanceWsClient client{config, [&skew_ms] { return real_unix_ms() - skew_ms.load(); }};
    client.set_event_handler([&events](const BinanceFeedEvent& a_event) { events.record(a_event); });
    client.start();
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));

    // drift: the next signed call's timestamp is 120s stale -> -1021
    skew_ms.store(120000);
    const auto result = client.call_signed("order.place", place_params());
    REQUIRE(result.is_ok());
    // the -1021 attempt was rejected BEFORE execution (not counted as a
    // place); the re-signed request is the only one that landed
    CHECK(server.stats().places == 1);
    // one session-start sync + exactly one recovery sync
    CHECK(count_method_frames(server, "time") == 2);

    // the learned offset is REUSED: another skewed call succeeds without
    // any further time sync
    const auto again = client.call_signed(
        "order.status", nlohmann::json{{"symbol", "BTCUSDT"}, {"origClientOrderId", "ws0001"}});
    REQUIRE(again.is_ok());
    CHECK(count_method_frames(server, "time") == 2); // no new sync

    client.stop();
    server.stop();
}

TEST_CASE("-1021 persisting after sync and one re-sign is a protocol error")
{
    using gateway::exchange::binance::real_unix_ms;

    BinanceMockWsServer server{BinanceConfig{.api_key = "test-key",
                                             .secret_key = "test-secret",
                                             .host = "127.0.0.1",
                                             .retry = gateway::RetryPolicy{}}};
    server.start();
    auto config = fast_config(server);
    config.recv_window_ms = 5000;

    // the venue's "time" starts honest (clean session), then starts
    // lying 10 minutes behind while the client clock goes 2 minutes
    // stale: the freshly learned offset stays wrong, so the single
    // re-sign hits -1021 again -> unrecoverable skew, not a re-send loop
    EventLog events;
    std::atomic<long long> skew_ms{0};
    BinanceWsClient client{config, [&skew_ms] { return real_unix_ms() - skew_ms.load(); }};
    client.set_event_handler([&events](const BinanceFeedEvent& a_event) { events.record(a_event); });
    client.start();
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));

    server.set_time_offset(-600000);
    skew_ms.store(120000);

    const auto result = client.call_signed("order.place", place_params());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
    CHECK(result.error().message.find("clock skew") != std::string::npos);
    // exactly two place frames on the wire (original + one re-sign),
    // none of which executed, and a single recovery sync
    CHECK(count_method_frames(server, "order.place") == 2);
    CHECK(server.stats().places == 0);
    CHECK(count_method_frames(server, "time") == 2);

    client.stop();
    server.stop();
}

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

TEST_CASE_FIXTURE(WsFixture, "venue -1006/-1007 are transport (outcome unknown)")
{
    // Regression: a 4xx-wrapped -1006/-1007 used to be a definitive
    // venue:<code> rejection although the venue documents that execution
    // status is unknown for these codes.
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));

    SUBCASE("-1006 unexpected response")
    {
        server_->set_next_place_unknown_outcome(-1006);
        const auto result = client_->call_signed("order.place", place_params());
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
        CHECK(server_->stats().places == 1); // the order DID land
    }
    SUBCASE("-1007 order service timeout")
    {
        server_->set_next_place_unknown_outcome(-1007);
        const auto result = client_->call_signed("order.place", place_params());
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
        CHECK(server_->stats().places == 1);
    }
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

TEST_CASE("the pong watchdog closes a half-open silent connection and reconnects")
{
    // Regression: httplib's default (max_missed_pongs = 0) disables the
    // pong-timeout check, so a silently dead socket stalled the read
    // loop until the ~300s OS read timeout. With a 1s ping interval and
    // 2 missed pongs the watchdog must close the connection in ~3s and
    // the supervisor must re-subscribe once the venue talks again.
    BinanceMockWsServer server{BinanceConfig{.api_key = "test-key",
                                             .secret_key = "test-secret",
                                             .host = "127.0.0.1",
                                             .retry = gateway::RetryPolicy{}}};
    server.start();
    SilentProxy proxy;
    proxy.start(server.port());
    REQUIRE(proxy.port() != 0);

    auto config = fast_config(server);
    config.port = static_cast<int>(proxy.port()); // through the proxy
    config.ws_ping_interval_s = 1;
    config.ws_max_missed_pongs = 2;
    config.retry.initial_backoff = std::chrono::milliseconds{50};
    config.retry.max_backoff = std::chrono::milliseconds{200};
    config.retry.jitter = 0.0;

    EventLog events;
    BinanceWsClient client{config};
    client.set_event_handler([&events](const BinanceFeedEvent& a_event) { events.record(a_event); });
    client.start();
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));
    REQUIRE(server.wait_for_subscriber(5000));

    // freeze the wire: the TCP connections stay established but no
    // bytes move — the client's pings go unanswered (half-open socket)
    proxy.set_silent(true);
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Disconnected, 1, 10000));

    proxy.set_silent(false);
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Connected, 2, 15000));
    CHECK(server.stats().subscribes >= 2); // user data stream re-subscribed

    client.stop();
    server.stop();
    proxy.stop();
}

TEST_CASE("a healthy session resets the reconnect backoff")
{
    // Regression: connect_attempt was monotone, so failures accumulated
    // during an early outage pinned every later reconnect near max
    // backoff. After a session has been subscribed and served a request,
    // the next failure must start at the initial backoff again.
    BinanceMockWsServer server{BinanceConfig{.api_key = "test-key",
                                             .secret_key = "test-secret",
                                             .host = "127.0.0.1",
                                             .retry = gateway::RetryPolicy{}}};
    server.start();
    auto config = fast_config(server);
    config.retry.initial_backoff = std::chrono::milliseconds{100};
    config.retry.max_backoff = std::chrono::milliseconds{60000};
    config.retry.jitter = 0.0;

    EventLog events;
    BinanceWsClient client{config};
    client.set_event_handler([&events](const BinanceFeedEvent& a_event) { events.record(a_event); });

    // outage: six failed connect attempts accumulate backoff (without the
    // fix the next delay after the healthy session would be >= 6.4s)
    server.stop();
    client.start();
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Connecting, 6, 15000));

    // venue returns; the session connects and serves one request
    server.restart_on_same_port();
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Connected, 1, 15000));
    const auto status = client.call_signed(
        "order.status", nlohmann::json{{"symbol", "BTCUSDT"}, {"origClientOrderId", "none"}});
    CHECK_FALSE(status.is_ok()); // venue:-2013, but a SERVED response: healthy

    server.kill_connections();
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Disconnected, 1, 5000));

    // healthy session -> the next backoff is the initial one (~200ms),
    // not the accumulated one (>= 6.4s)
    REQUIRE(events.wait_for_count(BinanceFeedEventType::Connected, 2, 5000));

    client.stop();
    server.stop();
}

TEST_CASE_FIXTURE(WsFixture, "stop is idempotent and joins the supervisor")
{
    client_->start();
    REQUIRE(events_.wait_for_count(BinanceFeedEventType::Connected, 1, 5000));
    client_->stop();
    client_->stop();
    CHECK(client_->is_running() == false);
}
