#pragma once

#include "core/retry.hpp"
#include "gateway/result.hpp"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

namespace gateway::exchange::binance {

/// Connection + behavior configuration for the Binance Spot WS-API
/// adapter. Defaults point at the Spot testnet
/// (wss://ws-api.testnet.binance.vision/ws-api/v3); production is
/// wss://ws-api.binance.com:443/ws-api/v3.
struct BinanceConfig
{
    std::string api_key;
    std::string secret_key;
    std::string host = "ws-api.testnet.binance.vision";
    int port = 443;
    bool use_tls = true;
    std::string path = "/ws-api/v3";
    /// SIGNED request recvWindow (docs: <= 60000; small recommended).
    long long recv_window_ms = 5000;
    /// How long the client waits for a WS-API response before declaring
    /// the outcome unknown ("transport"). Venue-side processing timeout
    /// is 10s, so this must be strictly larger.
    std::chrono::milliseconds request_timeout{12000};
    /// WS protocol-level liveness: client pings every ws_ping_interval_s
    /// seconds; after ws_max_missed_pongs unanswered pings the silent
    /// socket is closed and the reconnect + re-subscribe + reconcile
    /// path runs. Without it a half-open TCP connection stalls trading
    /// until the OS read timeout (httplib default: 300s).
    int ws_ping_interval_s = 20;
    int ws_max_missed_pongs = 2;
    /// Retry policy for transport failures (also backs off reconnects).
    RetryPolicy retry;
};

/// Parse a BinanceConfig from the "binance" config section. Requires
/// apiKey and secretKey; host/port/useTls/path/recvWindowMs/
/// requestTimeoutMs/wsPingIntervalSec/wsMaxMissedPongs and the "retry"
/// sub-object are optional. Errors: "protocol" with the list of
/// missing/invalid fields.
[[nodiscard]] auto
binance_config_from_json(const nlohmann::json& a_section) -> Result<BinanceConfig>;

} // namespace gateway::exchange::binance
