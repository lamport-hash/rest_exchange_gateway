#pragma once

#include "gateway/exchange_connector.hpp"
#include "gateway/result.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace gateway::exchange::okx {

struct OkxPlaceRequest
{
    std::string cl_ord_id;
    std::string inst_id;
    std::string side;
    std::string ord_type;
    std::string px;
    std::string sz;
    /// Optional time in force ("GTC"/"IOC"/"FOK"); empty = venue default.
    std::string td_if;
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

/// One currency adjustment of POST /api/v5/account/demo-adjust-balance.
struct OkxDemoBalanceAdjustment
{
    /// Currency, e.g. "USDT".
    std::string ccy;
    /// Plain decimal string carried verbatim to the venue.
    std::string amt;
};

/// POST /api/v5/account/demo-adjust-balance request body (demo trading
/// accounts only; supported currencies BTC/ETH/USDT/OKB, increases are
/// quota-limited by the venue).
struct OkxDemoBalanceRequest
{
    /// "increase" or "reduce" (venue spelling).
    std::string type;
    std::vector<OkxDemoBalanceAdjustment> adjustments;
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
    std::string inst_id;
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
/// /api/v5/trade/order.
[[nodiscard]] auto to_query(const OkxQuery& a_query) -> std::string;

/// POST /api/v5/account/demo-adjust-balance request body.
[[nodiscard]] auto to_json(const OkxDemoBalanceRequest& a_request) -> nlohmann::json;

/// Parse one element of the envelope "data" array of trade endpoints.
/// Missing optional fields default to empty strings; type mismatches are
/// tolerated the same way (OKX returns string-typed fields only).
[[nodiscard]] auto parse_order_ack(const nlohmann::json& a_item) -> OkxOrderAck;

/// Parse one element of the envelope "data" array of order-info.
[[nodiscard]] auto parse_order_info(const nlohmann::json& a_item) -> OkxOrderInfo;

/// Percent-encode for query components (RFC 3986 unreserved set kept as-is).
[[nodiscard]] auto url_encode(std::string_view a_value) -> std::string;

/// Map an OKX order state string ("live", "partially_filled", "filled",
/// "canceled") to the normalized OrderState. Returns std::nullopt for
/// unknown/unsupported states. Shared by the REST codec and the WS feed.
[[nodiscard]] auto map_okx_state(std::string_view a_state) -> std::optional<OrderState>;

/// Map an OKX side string ("buy"/"sell") to the normalized Side.
/// std::nullopt for anything else. Shared by the REST codec and the WS feed.
[[nodiscard]] auto map_okx_side(std::string_view a_side) -> std::optional<Side>;

} // namespace gateway::exchange::okx
