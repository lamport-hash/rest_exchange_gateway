#pragma once

#include "exchange/okx/okx_rest_client.hpp"

#include <httplib.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gateway::testing {

/// In-process mock of the OKX v5 trade REST endpoints, built from the
/// official docs (POST /api/v5/trade/order, cancel-order, amend-order and
/// GET /api/v5/trade/order). Deterministic: fills happen only when the
/// test script calls apply_fill()/set_fill_mode(); no timers, no randomness.
///
/// Behavior notes:
/// - Auth: recomputes the OK-ACCESS-SIGN HMAC with the configured secret and
///   rejects mismatches/missing headers with envelope code "50102".
/// - Unknown instrument -> "51001"; unknown order -> "51016" (cancel/amend)
///   or "51603" (get, like live OKX); terminal order cancels/amends ->
///   "51017"; validation failures -> "51000"; clOrdId must be alphanumeric
///   up to 32 chars (like live OKX).
/// - Every received request is recorded for assertions
///   (headers, raw target, body).
class OkxMockServer
{
  public:
    struct RecordedRequest
    {
        std::string method;
        std::string target;
        std::string body;
        httplib::Headers headers;
    };

    enum class FillMode
    {
        None,
        Full,
    };

    explicit OkxMockServer(exchange::okx::OkxConfig a_client_config);
    ~OkxMockServer();

    OkxMockServer(const OkxMockServer&) = delete;
    auto operator=(const OkxMockServer&) -> OkxMockServer& = delete;

    void start();
    void stop();
    /// stop() + start() binding the SAME port (simulates the venue dying
    /// abruptly and coming back; clients see connection failures in between).
    void restart_on_same_port();
    [[nodiscard]] auto port() const -> std::uint16_t;

    /// Scripting API (thread-safe).
    void set_fill_mode(FillMode a_mode);
    void apply_fill(const std::string& a_cl_ord_id, const std::string& a_qty,
                    const std::string& a_px);
    /// GET /api/v5/market/ticker serves this instrument at this price
    /// (defaults BTC-USDT @ 50000). Any other instId is rejected with
    /// "51001" like live OKX.
    void set_ticker(const std::string& a_inst_id, const std::string& a_last_price);
    /// Seed the demo balance of a currency (POST
    /// /api/v5/account/demo-adjust-balance state); currencies never
    /// touched start at 0.
    void set_demo_balance(const std::string& a_ccy, const std::string& a_amt);
    /// Script the remaining daily demo-increase quota (live default 3;
    /// reduce requests do not consume it).
    void set_demo_increase_quota(int a_remaining);
    /// Current demo balance of a currency as a plain decimal string;
    /// "" when the currency was never adjusted/seeded.
    [[nodiscard]] auto demo_balance(const std::string& a_ccy) const -> std::string;
    /// One-shot fault injection: the next request gets a raw response instead
    /// of the normal envelope.
    void set_next_raw_response(int a_status, std::string a_body);
    /// One-shot fault injection: the next request is dropped mid-response
    /// (partial chunked body, then the connection is closed) — the client
    /// observes a transport failure.
    void drop_next_request();
    /// One-shot fault injection: the next request is PROCESSED normally, but
    /// its success response is dropped mid-body (the outcome did happen, the
    /// acknowledgement is lost).
    void drop_next_response();
    /// One-shot fault injection: the next request's response is delayed by
    /// a_ms milliseconds (still delivered normally afterwards).
    void delay_next_request(unsigned a_ms);

    /// Recorded requests, in arrival order.
    [[nodiscard]] auto recorded_requests() const -> std::vector<RecordedRequest>;

  private:
    struct MockOrder
    {
        std::string ord_id;
        std::string cl_ord_id;
        std::string inst_id;
        std::string side;
        std::string ord_type;
        std::string px;
        std::string sz;
        std::string state;
        std::string acc_fill_sz;
        std::string avg_px;
        long long acc_scaled = 0;
        long long quote_scaled = 0;
    };

    void register_routes(httplib::Server& a_server);

    /// Record the request and apply one-shot fault scripting (raw response /
    /// drop / delay). Returns true when the response has already been set
    /// (fault applied) and the caller must not process the request further.
    auto begin_request(const httplib::Request& a_req, std::string_view a_body,
                       httplib::Response& a_res) -> bool;
    /// Deliver a success response, honoring drop_next_response() (the
    /// request was processed; its acknowledgement is then dropped).
    void respond_success(httplib::Response& a_res, const std::string& a_body);
    [[nodiscard]] auto check_auth(const httplib::Request& a_req,
                                  std::string_view a_body) const -> std::optional<std::string>;
    [[nodiscard]] auto find_order(const std::string& a_cl_ord_id) const -> std::optional<MockOrder>;
    void record(const httplib::Request& a_req, std::string_view a_body);
    [[nodiscard]] auto next_ord_id() -> std::string;

    exchange::okx::OkxConfig client_config_;
    FillMode fill_mode_ = FillMode::None;
    std::string ticker_inst_id_ = "BTC-USDT";
    std::string ticker_last_ = "50000";
    int raw_status_ = 0;
    std::string raw_body_;
    int drop_next_ = 0;
    bool drop_next_response_ = false;
    unsigned delay_next_ms_ = 0;
    std::unordered_map<std::string, MockOrder> orders_;
    /// Demo balances by currency, scaled by 1e8 (demo-adjust-balance).
    std::unordered_map<std::string, long long> demo_balances_;
    /// Remaining daily demo-increase quota (live default: 3/day).
    int demo_increase_quota_ = 3;
    std::vector<RecordedRequest> recorded_;
    long long ord_counter_ = 0;
    mutable std::mutex mutex_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    bool running_ = false;
};

} // namespace gateway::testing
