#pragma once

#include "gateway/result.hpp"

#include <crow_all.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace gateway::rest {

/// Helpers shared by the route files: JSON responses, the structured
/// error envelope {"error":{code,reason,clientOrderId}}, strict field
/// allowlisting and decimal validation.

inline auto json_response(int a_status, const nlohmann::json& a_body) -> crow::response
{
    crow::response res{a_status, a_body.dump()};
    res.set_header("Content-Type", "application/json");
    return res;
}

inline auto error_response(int a_status, std::string_view a_code, std::string_view a_reason,
                           std::string_view a_client_order_id) -> crow::response
{
    const nlohmann::json body = {
        {"error", {{"code", a_code}, {"reason", a_reason}, {"clientOrderId", a_client_order_id}}}};
    return json_response(a_status, body);
}

inline auto internal_error_response(std::string_view a_client_order_id = "") -> crow::response
{
    return error_response(500, "internal", "unexpected server error", a_client_order_id);
}

/// Plain non-negative decimal (no sign, no exponent, at most one dot,
/// digits on both sides). "0" passes; business positivity is the
/// venue's/model's call.
inline auto is_decimal(std::string_view a_text) -> bool
{
    if (a_text.empty() || a_text.front() == '.') {
        return false;
    }
    bool dot_seen = false;
    for (const char c : a_text) {
        if (c == '.') {
            if (dot_seen) {
                return false;
            }
            dot_seen = true;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return a_text.back() != '.';
}

/// Reject unknown fields: the client must not send exchange-specific
/// parameters (spec: "Client must not send exchange-specific fields").
inline auto unknown_fields(const nlohmann::json& a_body,
                           std::initializer_list<std::string_view> a_allowed) -> std::string
{
    std::string unknown;
    for (auto it = a_body.begin(); it != a_body.end(); ++it) {
        const bool allowed =
            std::find(a_allowed.begin(), a_allowed.end(), it.key()) != a_allowed.end();
        if (!allowed) {
            if (!unknown.empty()) {
                unknown += ", ";
            }
            unknown += it.key();
        }
    }
    return unknown;
}

inline auto string_field(const nlohmann::json& a_body, const char* a_name) -> std::string
{
    const auto it = a_body.find(a_name);
    if (it != a_body.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return {};
}

inline auto optional_string_field(const nlohmann::json& a_body,
                                  const char* a_name) -> std::optional<std::string>
{
    const auto it = a_body.find(a_name);
    if (it == a_body.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

inline auto parse_json_body(const crow::request& a_request,
                            crow::response& a_response) -> std::optional<nlohmann::json>
{
    const auto body = nlohmann::json::parse(a_request.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        a_response =
            error_response(400, "invalid_request", "request body must be a JSON object", "");
        return std::nullopt;
    }
    return body;
}

/// Structured mapping of OMS/connector error codes to HTTP responses.
inline auto map_error(const Error& a_error, std::string_view a_client_order_id) -> crow::response
{
    if (a_error.code == "invalid_request") {
        return error_response(400, "invalid_request", a_error.message, a_client_order_id);
    }
    if (a_error.code == "not_found" || a_error.code == "venue:51016" ||
        a_error.code == "venue:-2013" || a_error.code == "venue:51603") {
        return error_response(404, "not_found", a_error.message, a_client_order_id);
    }
    if (a_error.code == "order_terminal") {
        return error_response(409, "order_terminal", a_error.message, a_client_order_id);
    }
    if (a_error.code == "order_pending") {
        return error_response(409, "order_pending", a_error.message, a_client_order_id);
    }
    if (a_error.code.rfind("risk_", 0) == 0) {
        // 400 (not 422): Crow v1.2.0 only emits status codes it knows and
        // rewrites unknown ones to 500; the machine-readable risk_* code
        // carries the semantics.
        return error_response(400, a_error.code, a_error.message, a_client_order_id);
    }
    if (a_error.code == "venue_absent") {
        return error_response(409, "venue_rejected", a_error.message, a_client_order_id);
    }
    if (a_error.code == "transport") {
        return error_response(502, "venue_unavailable", "cannot reach the venue",
                              a_client_order_id);
    }
    if (a_error.code.rfind("venue:", 0) == 0) {
        return error_response(409, "venue_rejected", a_error.message, a_client_order_id);
    }
    if (a_error.code == "persistence") {
        return error_response(500, "persistence", a_error.message, a_client_order_id);
    }
    return error_response(500, "internal", a_error.message, a_client_order_id);
}

} // namespace gateway::rest
