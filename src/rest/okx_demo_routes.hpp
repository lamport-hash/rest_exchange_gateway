#pragma once

#include "exchange/okx/okx_connector.hpp"

#include <crow_all.h>

namespace gateway::rest {

/// Register the OKX demo-trading balance route on a_app:
///   POST /venue/okx/demo-adjust-balance
///     {"type": "increase"|"reduce",
///      "adjustments": [{"ccy": "USDT", "amt": "5000"}, ...]}
/// OKX-specific passthrough (deliberately outside the venue-agnostic
/// /orders schema): the gateway only validates the request shape and
/// forwards it as a signed POST /api/v5/account/demo-adjust-balance;
/// business rules (supported currencies BTC/ETH/USDT/OKB, per-request
/// increase caps, 3-per-day increase quota) stay at the venue.
/// Rejected with 400 invalid_request when OKX demo trading is disabled.
/// Error responses follow the shared {"error":{code,reason,clientOrderId}}
/// envelope (clientOrderId empty — no order is involved).
/// a_okx must outlive the app.
void register_okx_demo_routes(crow::SimpleApp& a_app, gateway::exchange::okx::OkxConnector& a_okx);

} // namespace gateway::rest
