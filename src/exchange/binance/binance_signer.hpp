#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace gateway::exchange::binance {

/// Signature payload for SIGNED requests: all params except "signature",
/// sorted alphabetically by name, formatted as "name=value" pairs joined
/// with "&" (official Binance HMAC signing example).
[[nodiscard]] auto signature_payload(const nlohmann::json& a_params) -> std::string;

/// Hex-encoded (lower-case) HMAC-SHA256 of the signature payload, keyed
/// with a_secret — the "signature" parameter of SIGNED WS-API requests.
/// Deterministic; safe for empty payload/secret.
[[nodiscard]] auto sign_params(const nlohmann::json& a_params,
                               std::string_view a_secret) -> std::string;

} // namespace gateway::exchange::binance
