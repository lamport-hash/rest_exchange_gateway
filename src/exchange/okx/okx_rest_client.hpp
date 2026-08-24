#pragma once

#include "core/retry.hpp"
#include "exchange/okx/okx_wire.hpp"
#include "gateway/result.hpp"

#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

namespace gateway::exchange::okx {

/// Private WebSocket (orders channel) connection parameters.
struct OkxWsConfig
{
    bool enabled = true;
    std::string host = "ws.okx.com";
    int port = 8443;
    bool use_tls = true;
    std::string path = "/ws/v5/private";
    /// OKX disconnects idle connections after ~30s of silence; the feed sends
    /// the application-level text "ping" every ping_interval.
    std::chrono::milliseconds ping_interval{20000};
    /// Close the connection after this many ping intervals without any
    /// inbound message (pong or data); triggers reconnect.
    int max_missed_pongs = 2;
};

struct OkxConfig
{
    std::string api_key;
    std::string secret_key;
    std::string passphrase;
    std::string host = "www.okx.com";
    int port = 443;
    bool use_tls = true;
    bool demo_trading = false;
    int rest_connect_timeout_ms = 5000;
    int rest_read_timeout_ms = 5000;
    /// REST retry policy for transport failures, also used to resolve
    /// unknown-outcome place/cancel/amend and to back off WebSocket
    /// reconnects (attempts/budget are shared, backoff fields only).
    RetryPolicy retry;
    OkxWsConfig ws;
};

/// Parse an OkxConfig from the "okx" config section. Requires apiKey,
/// secretKey and passphrase; host/port/useTls/demoTrading/rest timeouts and
/// the "retry"/"ws" sub-objects are optional.
/// Errors: "protocol" with the list of missing/invalid fields.
[[nodiscard]] auto okx_config_from_json(const nlohmann::json& a_section) -> Result<OkxConfig>;

/// Thin raw client for the OKX v5 trade REST endpoints (no retry, no
/// abstraction — that lives above this layer).
class OkxRestClient
{
  public:
    using TimestampProvider = std::function<std::string()>;

    explicit OkxRestClient(OkxConfig a_config, TimestampProvider a_timestamp = nullptr);

    /// POST /api/v5/trade/order. Error codes: "transport", "protocol",
    /// "venue:<sCode>".
    [[nodiscard]] auto place_order(const OkxPlaceRequest& a_request) const -> Result<OkxOrderAck>;

    /// POST /api/v5/trade/cancel-order.
    [[nodiscard]] auto cancel_order(const OkxCxlRequest& a_request) const -> Result<OkxOrderAck>;

    /// POST /api/v5/trade/amend-order.
    [[nodiscard]] auto amend_order(const OkxAmendRequest& a_request) const -> Result<OkxOrderAck>;

    /// GET /api/v5/trade/order. A well-formed reply with an empty data
    /// array yields std::nullopt (unknown order for the venue); an unknown
    /// order is normally reported by OKX as venue error "51603".
    [[nodiscard]] auto
    get_order(const OkxQuery& a_query) const -> Result<std::optional<OkxOrderInfo>>;

    /// GET /api/v5/trade/orders-pending: every currently open order
    /// (single page; OKX returns up to 100 — documented limitation).
    [[nodiscard]] auto get_orders_pending() const -> Result<std::vector<OkxOrderInfo>>;

    /// GET /api/v5/market/ticker (public, unsigned): last-traded price of
    /// an instrument, verbatim decimal string.
    [[nodiscard]] auto get_ticker(const std::string& a_instrument_id) const -> Result<std::string>;

    /// POST /api/v5/account/demo-adjust-balance (demo trading accounts
    /// only): increase/reduce the demo account balance per currency
    /// (type "increase"|"reduce"). Unlike the trade endpoints, the venue
    /// reports business rejections with a non-200 status + error
    /// envelope; those are surfaced as "venue:<code>". Returns the
    /// venue's envelope "data" array verbatim ({remainCnt, totalCnt,
    /// details}). Error codes: "transport", "protocol", "venue:<sCode>".
    [[nodiscard]] auto
    adjust_demo_balance(const OkxDemoBalanceRequest& a_request) const -> Result<nlohmann::json>;

  private:
    /// Signed request returning the raw HTTP status + body (no envelope
    /// validation).
    [[nodiscard]] auto
    signed_request_raw(const char* a_method, const std::string& a_path,
                       const std::string& a_body) const -> Result<std::pair<int, std::string>>;

    [[nodiscard]] auto signed_request(const char* a_method, const std::string& a_path,
                                      const std::string& a_body) const -> Result<nlohmann::json>;

    OkxConfig config_;
    TimestampProvider timestamp_;
};

} // namespace gateway::exchange::okx
