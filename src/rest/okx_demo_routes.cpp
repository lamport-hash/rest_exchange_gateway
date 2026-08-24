#include "rest/okx_demo_routes.hpp"

#include "rest/route_support.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>

namespace gateway::rest {

namespace {

/// Parse the request body into the OKX wire request; std::nullopt means
/// a_shape_error was filled with the 400 response to return.
auto parse_adjustment_body(const nlohmann::json& a_body,
                           gateway::exchange::okx::OkxDemoBalanceRequest& a_request,
                           crow::response& a_shape_error) -> bool
{
    using gateway::exchange::okx::OkxDemoBalanceAdjustment;

    if (const auto unknown = unknown_fields(a_body, {"type", "adjustments"}); !unknown.empty()) {
        a_shape_error =
            error_response(400, "invalid_request", "unsupported field(s): " + unknown, "");
        return false;
    }

    a_request.type = string_field(a_body, "type");
    if (a_request.type != "increase" && a_request.type != "reduce") {
        a_shape_error =
            error_response(400, "invalid_request", "type must be \"increase\" or \"reduce\"", "");
        return false;
    }

    const auto adjustments = a_body.find("adjustments");
    if (adjustments == a_body.end() || !adjustments->is_array() || adjustments->empty()) {
        a_shape_error = error_response(400, "invalid_request",
                                       "adjustments must be a non-empty array of {ccy, amt}", "");
        return false;
    }
    a_request.adjustments.reserve(adjustments->size());
    for (auto it = adjustments->begin(); it != adjustments->end(); ++it) {
        const std::string index = "adjustments[" + std::to_string(it - adjustments->begin()) + "]";
        if (!it->is_object()) {
            a_shape_error =
                error_response(400, "invalid_request", index + " must be an object", "");
            return false;
        }
        if (const auto unknown = unknown_fields(*it, {"ccy", "amt"}); !unknown.empty()) {
            a_shape_error = error_response(400, "invalid_request",
                                           index + ": unsupported field(s): " + unknown, "");
            return false;
        }
        OkxDemoBalanceAdjustment adjustment;
        adjustment.ccy = string_field(*it, "ccy");
        if (adjustment.ccy.empty()) {
            a_shape_error = error_response(400, "invalid_request", index + ".ccy is required", "");
            return false;
        }
        adjustment.amt = string_field(*it, "amt");
        if (adjustment.amt.empty() || !is_decimal(adjustment.amt)) {
            a_shape_error = error_response(400, "invalid_request",
                                           index + ".amt must be a positive decimal", "");
            return false;
        }
        a_request.adjustments.push_back(std::move(adjustment));
    }
    return true;
}

} // namespace

void register_okx_demo_routes(crow::SimpleApp& a_app, gateway::exchange::okx::OkxConnector& a_okx)
{
    // ---- POST /venue/okx/demo-adjust-balance -----------------------------
    // Venue-specific passthrough: shape validation here, business rules
    // (known currency, adjustment limits, sufficient balance for a
    // reduce) at the venue — its rejection surfaces as 409 venue_rejected.
    CROW_ROUTE(a_app, "/venue/okx/demo-adjust-balance")
        .methods(crow::HTTPMethod::POST)([&a_okx](const crow::request& a_req) -> crow::response {
            try {
                crow::response parse_error;
                const auto body = parse_json_body(a_req, parse_error);
                if (!body.has_value()) {
                    return parse_error;
                }

                gateway::exchange::okx::OkxDemoBalanceRequest request;
                crow::response shape_error;
                if (!parse_adjustment_body(*body, request, shape_error)) {
                    return shape_error;
                }

                const auto data = a_okx.adjust_demo_balance(request);
                if (!data.is_ok()) {
                    return map_error(data.error(), "");
                }

                nlohmann::json adjustments = nlohmann::json::array();
                for (const auto& adjustment : request.adjustments) {
                    adjustments.push_back({{"ccy", adjustment.ccy}, {"amt", adjustment.amt}});
                }
                const nlohmann::json response = {{"venue", "okx"},
                                                 {"type", request.type},
                                                 {"adjustments", std::move(adjustments)},
                                                 {"data", data.value()}};
                return json_response(200, response);
            } catch (...) {
                return internal_error_response();
            }
        });
}

} // namespace gateway::rest
