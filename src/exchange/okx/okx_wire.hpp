#pragma once

#include "core/result.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace gateway::exchange::okx {

struct OkxPlaceRequest
{
    std::string cl_ord_id;
    std::string inst_id;
    std::string side;
    std::string ord_type;
    std::string px;
    std::string sz;
    std::string td_mode = "cash";
};

struct OkxCxlRequest
{
    std::string inst_id;
    std::string cl_ord_id;
};

struct OkxAmendRequest
{
    std::string inst_id;
    std::string cl_ord_id;
    std::optional<std::string> new_px;
    std::optional<std::string> new_sz;
};

struct OkxQuery
{
    std::string inst_id;
    std::string cl_ord_id;
};

struct OkxOrderAck
{
    std::string ord_id;
    std::string cl_ord_id;
    std::string s_code;
    std::string s_msg;
};

struct OkxOrderInfo
{
    std::string ord_id;
    std::string cl_ord_id;
    std::string state;
    std::string side;
    std::string ord_type;
    std::string px;
    std::string sz;
    std::string avg_px;
    std::string acc_fill_sz;
};

/// POST /api/v5/trade/order request body.
[[nodiscard]] auto to_json(const OkxPlaceRequest& a_request) -> nlohmann::json;

/// POST /api/v5/trade/cancel-order request body.
[[nodiscard]] auto to_json(const OkxCxlRequest& a_request) -> nlohmann::json;

/// POST /api/v5/trade/amend-order request body; requires at least one of
/// new_px / new_sz (returns an error Result otherwise).
[[nodiscard]] auto to_json(const OkxAmendRequest& a_request) -> Result<nlohmann::json>;

/// URL-encoded query string (without leading '?') for GET
/// /api/v5/trade/order-info.
[[nodiscard]] auto to_query(const OkxQuery& a_query) -> std::string;

/// Parse one element of the envelope "data" array of trade endpoints.
/// Missing optional fields default to empty strings; type mismatches are
/// tolerated the same way (OKX returns string-typed fields only).
[[nodiscard]] auto parse_order_ack(const nlohmann::json& a_item) -> OkxOrderAck;

/// Parse one element of the envelope "data" array of order-info.
[[nodiscard]] auto parse_order_info(const nlohmann::json& a_item) -> OkxOrderInfo;

/// Percent-encode for query components (RFC 3986 unreserved set kept as-is).
[[nodiscard]] auto url_encode(std::string_view a_value) -> std::string;

} // namespace gateway::exchange::okx
