#pragma once

#include "core/risk.hpp"
#include "gateway/result.hpp"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>

namespace gateway {

/// Gateway-wide configuration loaded from a JSON file. Venue sections are
/// kept as raw JSON; each exchange adapter parses its own section
/// (keeps src/core free of exchange-specific knowledge).
struct GatewayConfig
{
    std::uint16_t rest_port = 8080;
    /// Append-only order event log; std::nullopt disables persistence
    /// (recovery then relies entirely on venue reconciliation).
    std::optional<std::filesystem::path> persistence_log;
    /// Pre-trade risk limits; empty = unlimited (warned at startup).
    RiskConfig risk;
    /// Venue used when a client order omits the venue field.
    std::string default_venue = "okx";
    /// Raw venue sections; each exchange adapter parses its own (keeps
    /// src/core free of exchange-specific knowledge). A section present
    /// but empty {} means "venue enabled with default parameters".
    nlohmann::json okx = nlohmann::json::object();
    nlohmann::json binance = nlohmann::json::object();
};

/// Load and structurally validate the gateway configuration.
/// Errors: "io" (unreadable file), "protocol" (malformed JSON / wrong types).
/// Postcondition: on is_ok() the okx member is an object (possibly empty).
[[nodiscard]] auto load_config(const std::filesystem::path& a_path) -> Result<GatewayConfig>;

} // namespace gateway
