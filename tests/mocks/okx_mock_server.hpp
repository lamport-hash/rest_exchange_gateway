#pragma once

#include "exchange/okx/okx_rest_client.hpp"

#include <httplib.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

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
    [[nodiscard]] auto port() const -> std::uint16_t;

    /// Scripting API (thread-safe).
    void set_fill_mode(FillMode a_mode);
    void apply_fill(const std::string& a_cl_ord_id, const std::string& a_qty,
                    const std::string& a_px);
    /// One-shot fault injection: the next request gets a raw response instead
    /// of the normal envelope.
    void set_next_raw_response(int a_status, std::string a_body);

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

    void register_routes();

    [[nodiscard]] auto check_auth(const httplib::Request& a_req,
                                  std::string_view a_body) const -> std::optional<std::string>;
    [[nodiscard]] auto find_order(const std::string& a_cl_ord_id) const -> std::optional<MockOrder>;
    void record(const httplib::Request& a_req, std::string_view a_body);
    [[nodiscard]] auto next_ord_id() -> std::string;

    exchange::okx::OkxConfig client_config_;
    FillMode fill_mode_ = FillMode::None;
    int raw_status_ = 0;
    std::string raw_body_;
    std::unordered_map<std::string, MockOrder> orders_;
    std::vector<RecordedRequest> recorded_;
    long long ord_counter_ = 0;
    mutable std::mutex mutex_;
    httplib::Server server_;
    std::thread server_thread_;
    int port_ = 0;
    bool running_ = false;
};

} // namespace gateway::testing
