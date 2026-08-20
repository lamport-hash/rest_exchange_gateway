#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace gateway {

/// Gateway-wide configuration loaded from a JSON file. Venue sections are
/// kept as raw JSON; each exchange adapter parses its own section
/// (keeps src/core free of exchange-specific knowledge).
struct GatewayConfig
{
    std::uint16_t rest_port = 8080;
    nlohmann::json okx = nlohmann::json::object();
};

/// Load and structurally validate the gateway configuration.
/// Errors: "io" (unreadable file), "protocol" (malformed JSON / wrong types).
/// Postcondition: on is_ok() the okx member is an object (possibly empty).
[[nodiscard]] auto load_config(const std::filesystem::path& a_path) -> Result<GatewayConfig>;

} // namespace gateway
