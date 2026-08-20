#pragma once

#include "exchange/okx/okx_wire.hpp"
#include "gateway/result.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace gateway::exchange::okx {

struct OkxConfig
{
    std::string api_key;
    std::string secret_key;
    std::string passphrase;
    std::string host = "www.okx.com";
    int port = 443;
    bool use_tls = true;
    bool demo_trading = false;
};

/// Parse an OkxConfig from the "okx" config section. Requires apiKey,
/// secretKey and passphrase; host/port/useTls/demoTrading are optional.
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

    /// GET /api/v5/trade/order-info. A well-formed reply with an empty data
    /// array yields std::nullopt (unknown order for the venue).
    [[nodiscard]] auto
    get_order(const OkxQuery& a_query) const -> Result<std::optional<OkxOrderInfo>>;

  private:
    [[nodiscard]] auto signed_request(const char* a_method, const std::string& a_path,
                                      const std::string& a_body) const -> Result<nlohmann::json>;

    OkxConfig config_;
    TimestampProvider timestamp_;
};

} // namespace gateway::exchange::okx
