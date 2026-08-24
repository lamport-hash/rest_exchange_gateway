#include "rest/risk_routes.hpp"

#include "rest/route_support.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <utility>

namespace gateway::rest {

namespace {

/// One limits block: decimal strings as active in the engine; an empty
/// string means that check is disabled for the scope.
auto limits_json(const InstrumentRiskLimits& a_limits) -> nlohmann::json
{
    return {{"maxQty", a_limits.max_qty},
            {"maxNotional", a_limits.max_notional},
            {"maxPosition", a_limits.max_position}};
}

} // namespace

void register_risk_routes(crow::SimpleApp& a_app, OrderManagementSystem& a_oms)
{
    // ---- GET /risk: active pre-trade limits ------------------------------
    // Read-only: limits are fixed by the config file at startup. Avenues
    // that would mutate them at runtime do not exist by design.
    CROW_ROUTE(a_app, "/risk")
    ([&a_oms]() -> crow::response {
        try {
            const RiskConfig risk = a_oms.risk_config();
            nlohmann::json instruments = nlohmann::json::object();
            for (const auto& [symbol, limits] : risk.instruments) {
                instruments[symbol] = limits_json(limits);
            }
            const nlohmann::json body = {
                {"default",
                 risk.defaults.has_value() ? limits_json(*risk.defaults) : nlohmann::json()},
                {"instruments", std::move(instruments)}};
            return json_response(200, body);
        } catch (...) {
            return internal_error_response();
        }
    });
}

} // namespace gateway::rest
