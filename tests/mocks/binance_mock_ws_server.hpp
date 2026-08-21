#pragma once

#include "exchange/binance/binance_config.hpp"

#include <httplib.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gateway::testing {

/// In-process mock of the Binance Spot WebSocket API (testnet-shaped),
/// built from the official docs. Deterministic: fills happen only when
/// the test scripts them; no timers; no randomness.
///
/// Served methods:
/// - userDataStream.subscribe.signature: verifies apiKey + HMAC signature
///   (recomputed from the received params) like the venue does
/// - order.place: creates an order (status NEW); duplicate open
///   clientOrderId -> error -4116; bad signature -> -1022; unknown
///   apiKey -> -2014; timestamps outside recvWindow -> -1021
/// - order.cancel: unknown/terminal target -> -2011
/// - order.cancelReplace (STOP_ON_FAILURE): cancel + replacement place;
///   unknown/terminal target -> -2011
/// - order.status: unknown order -> -2013; otherwise a status payload
/// - openOrders.status: every non-terminal order
///
/// Scripting: fills via apply_fill()/set_fill_mode(Full); pushes via
/// push_execution_report() (subscription-gated, drop/duplicate control);
/// set_drop_next_response() processes a request but loses its response
/// (outcome-unknown drills); kill_connections()/restart_on_same_port()
/// kill the transport; set_delay_next_response() delays one reply.
///
/// Every received frame is recorded for assertions.
class BinanceMockWsServer
{
  public:
    explicit BinanceMockWsServer(exchange::binance::BinanceConfig a_client_config,
                                 std::string a_path = "/ws-api/v3");
    ~BinanceMockWsServer();

    BinanceMockWsServer(const BinanceMockWsServer&) = delete;
    auto operator=(const BinanceMockWsServer&) -> BinanceMockWsServer& = delete;

    void start();
    void stop();
    /// stop() + start() binding the SAME port (endpoint dies abruptly and
    /// comes back; clients see connection failures in between).
    void restart_on_same_port();

    [[nodiscard]] auto port() const -> std::uint16_t;

    enum class FillMode
    {
        None,
        Full,
    };

    // ---- scripting (thread-safe) ----
    void set_fill_mode(FillMode a_mode);
    /// Fill a_client_order_id by a_qty at a_px ("0" qty = no-op); status
    /// becomes PARTIALLY_FILLED or FILLED when fully executed.
    void apply_fill(const std::string& a_client_order_id, const std::string& a_qty,
                    const std::string& a_px);
    /// Process the next request normally but drop its response: the
    /// outcome happened, the acknowledgement is lost (one-shot).
    void set_drop_next_response();
    /// Delay the next response by a_ms (still delivered).
    void set_delay_next_response(unsigned a_ms);
    /// Skip apiKey/signature/timestamp verification (auth drills off).
    void set_ignore_signature(bool a_ignore);
    /// Silently lose the next a_count pushed execution reports.
    void set_drop_next_updates(int a_count);
    /// Deliver the next pushed execution report twice.
    void set_duplicate_next_update();
    /// Push one executionReport event for a_client_order_id to every
    /// subscribed session.
    void push_execution_report(const std::string& a_client_order_id);
    /// Push a raw JSON frame to every subscribed session (handcrafted
    /// venue events for parser tests).
    void push_raw_frame(const std::string& a_frame);
    /// Close every session with a clean WS close frame.
    void kill_connections();

    // ---- observation (thread-safe snapshots) ----
    struct Stats
    {
        int connections = 0;
        int places = 0;
        int cancels = 0;
        int amends = 0;
        int status_queries = 0;
        int open_orders_queries = 0;
        int subscribes = 0;
        int signature_failures = 0;
        int responses_dropped = 0;
        bool any_subscribed = false;
        std::vector<std::string> received; // client -> server frames
    };
    [[nodiscard]] auto stats() const -> Stats;

    [[nodiscard]] auto wait_for_subscriber(int a_timeout_ms) const -> bool;
    [[nodiscard]] auto wait_for_places(int a_count, int a_timeout_ms) const -> bool;
    [[nodiscard]] auto wait_for_amends(int a_count, int a_timeout_ms) const -> bool;
    [[nodiscard]] auto wait_for_cancels(int a_count, int a_timeout_ms) const -> bool;
    [[nodiscard]] auto wait_for_connections(int a_count, int a_timeout_ms) const -> bool;
    /// Block until a frame containing a_needle was received.
    [[nodiscard]] auto wait_for_text(const std::string& a_needle, int a_timeout_ms) const -> bool;

  private:
    struct MockOrder
    {
        long long order_id = 0;
        std::string client_order_id;
        std::string symbol; // wire spelling (BTCUSDT)
        std::string side;   // BUY/SELL
        std::string type;   // LIMIT/MARKET
        std::string price;
        std::string time_in_force;
        long long orig_scaled = 0; // quantity * 1e8
        long long executed_scaled = 0;
        long long quote_scaled = 0; // cummulativeQuoteQty * 1e8
        std::string status;         // NEW / PARTIALLY_FILLED / FILLED / CANCELED
    };

    struct Session
    {
        httplib::ws::WebSocket* socket = nullptr;
        bool subscribed = false;
    };

    void register_routes(httplib::Server& a_server);
    void handle_frame(httplib::ws::WebSocket& a_ws, Session& a_session,
                      const nlohmann::json& a_request);
    /// Verify SIGNED params; returns an error response body when the
    /// check fails, nullptr otherwise.
    [[nodiscard]] auto check_signed(const nlohmann::json& a_params) -> std::unique_ptr<std::string>;
    [[nodiscard]] auto order_status_payload(const MockOrder& a_order) const -> nlohmann::json;
    [[nodiscard]] auto order_result_payload(const MockOrder& a_order) const -> nlohmann::json;
    auto handle_place(const nlohmann::json& a_params) -> nlohmann::json;
    auto handle_cancel(const nlohmann::json& a_params) -> nlohmann::json;
    auto handle_cancel_replace(const nlohmann::json& a_params) -> nlohmann::json;
    auto handle_order_status(const nlohmann::json& a_params) -> nlohmann::json;
    auto handle_open_orders(const nlohmann::json& a_params) -> nlohmann::json;
    [[nodiscard]] auto find_order(const std::string& a_client_order_id) -> MockOrder*;
    [[nodiscard]] auto next_order_id() -> long long;
    auto fill_if_mode(MockOrder& a_order) -> void;
    static auto response_frame(const nlohmann::json& a_request, int a_status,
                               const nlohmann::json& a_body) -> std::string;

    exchange::binance::BinanceConfig client_config_;
    std::string path_;

    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    bool running_ = false;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<Session*> sessions_;
    Stats stats_;
    FillMode fill_mode_ = FillMode::None;
    bool drop_next_response_ = false;
    unsigned delay_next_ms_ = 0;
    bool ignore_signature_ = false;
    int drop_next_updates_ = 0;
    bool duplicate_next_update_ = false;
    std::unordered_map<std::string, MockOrder> orders_;
    long long order_counter_ = 0;
};

} // namespace gateway::testing
