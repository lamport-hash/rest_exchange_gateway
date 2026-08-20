#pragma once

#include "exchange/okx/okx_rest_client.hpp"

#include <httplib.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace gateway::testing {

/// In-process mock of the OKX v5 private WebSocket (orders channel), built
/// from the official docs. Deterministic: nothing is pushed unless the test
/// scripts it; no timers. Covers:
/// - handshake: records headers (x-simulated-trading check) and target path
/// - login: verifies apiKey/passphrase and the WS login signature
///   (HMAC-SHA256 over timestamp + "GET" + "/users/self/verify"); failure is
///   scriptable via set_login_should_fail()
/// - subscribe/unsubscribe of the "orders" channel (acks like the venue)
/// - application-level keepalive: answers text "ping" with "pong"
///   (can be silenced with set_ignore_pings() to starve the client watchdog);
///   send_text("ping") lets tests check the client answers "pong"
/// - scripted pushes: push_orders_update() delivers
///   {"arg":{"channel":"orders"},"data":[item]} to every subscribed session;
///   set_drop_next_updates() loses messages, set_duplicate_next_update()
///   delivers one push twice
/// - kill_connections() closes every session (client observes disconnect);
///   stop() + restart_on_same_port() simulates the whole endpoint dying
///   abruptly (TCP EOF without WS close frames) and coming back.
class OkxMockWsServer
{
  public:
    explicit OkxMockWsServer(exchange::okx::OkxConfig a_client_config,
                             std::string a_path = "/ws/v5/private");
    ~OkxMockWsServer();

    OkxMockWsServer(const OkxMockWsServer&) = delete;
    auto operator=(const OkxMockWsServer&) -> OkxMockWsServer& = delete;

    /// Bind to an ephemeral port and serve.
    void start();
    /// Stop serving and close every connection.
    void stop();
    /// stop() + start() binding the SAME port (simulates an abruptly killed
    /// endpoint; the client sees EOF without WS close frames).
    void restart_on_same_port();

    [[nodiscard]] auto port() const -> std::uint16_t;

    // ---- scripting (thread-safe) ----
    void set_login_should_fail(bool a_fail);
    void set_ignore_pings(bool a_ignore);
    /// Silently lose the next a_count pushed updates.
    void set_drop_next_updates(int a_count);
    /// Deliver the next pushed update twice (duplicate execution report).
    void set_duplicate_next_update();
    /// Push one orders-channel data item to every subscribed session.
    void push_orders_update(const nlohmann::json& a_item);
    /// Send a raw text frame to every session (e.g. "ping").
    void send_text(const std::string& a_text);
    /// Close every session with a clean WS close frame.
    void kill_connections();

    // ---- observation (thread-safe snapshots) ----
    struct Stats
    {
        int connections = 0; // completed handshakes
        int logins_ok = 0;
        int logins_failed = 0;
        int subscribed_sessions = 0;
        bool any_subscribed = false;
        std::vector<std::string> received;          // client -> server texts
        std::vector<std::string> handshake_targets; // request targets
        bool saw_demo_header = false;
        int pushes_delivered = 0;
    };
    [[nodiscard]] auto stats() const -> Stats;

    /// Block until at least one session subscribed (or timeout). False on
    /// timeout.
    [[nodiscard]] auto wait_for_subscriber(int a_timeout_ms) const -> bool;
    /// Timestamp string of the last login attempt (empty before any).
    [[nodiscard]] auto last_login_timestamp() const -> std::string;
    /// Block until stats().received grows to a_size items.
    [[nodiscard]] auto wait_for_received(std::size_t a_size, int a_timeout_ms) const -> bool;
    /// Block until a_text was received from any session.
    [[nodiscard]] auto wait_for_text(const std::string& a_text, int a_timeout_ms) const -> bool;
    /// Block until a_count failed logins were observed.
    [[nodiscard]] auto wait_for_logins_failed(int a_count, int a_timeout_ms) const -> bool;
    /// Block until stats().connections reaches a_count (reconnect checks).
    [[nodiscard]] auto wait_for_connections(int a_count, int a_timeout_ms) const -> bool;

  private:
    struct Session
    {
        httplib::ws::WebSocket* socket = nullptr;
        bool authenticated = false;
        bool subscribed = false;
    };

    void serve_on(int a_port);
    void register_routes(httplib::Server& a_server);
    void handle_text(httplib::ws::WebSocket& a_ws, Session& a_session, const std::string& a_text);

    exchange::okx::OkxConfig client_config_;
    std::string path_;

    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    bool running_ = false;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<Session*> sessions_; // guarded; only touched under mutex
    Stats stats_;
    bool login_should_fail_ = false;
    bool ignore_pings_ = false;
    int drop_next_ = 0;
    bool duplicate_next_ = false;
    std::string last_login_timestamp_;
};

} // namespace gateway::testing
