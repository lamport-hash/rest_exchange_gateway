#pragma once

#include <string_view>

namespace gateway::exchange::okx {

/// Base64-encoded HMAC-SHA256 over the OKX pre-hash string
/// timestamp + method + requestPath(incl. query string) + body, keyed with
/// a_secret. Deterministic and allocation-light; safe for empty body/secret.
[[nodiscard]] auto sign_request(std::string_view a_timestamp, std::string_view a_method,
                                std::string_view a_path, std::string_view a_body,
                                std::string_view a_secret) -> std::string;

/// Signature for the private WebSocket login message: HMAC-SHA256 over
/// timestamp + "GET" + "/users/self/verify", keyed with a_secret (OKX v5
/// WebSocket docs). Same encoding as sign_request.
[[nodiscard]] auto sign_ws_login(std::string_view a_timestamp,
                                 std::string_view a_secret) -> std::string;

} // namespace gateway::exchange::okx
