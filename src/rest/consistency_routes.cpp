#include "rest/consistency_routes.hpp"

#include "rest/route_support.hpp"

#include <exception>

namespace gateway::rest {

void register_consistency_routes(crow::SimpleApp& a_app, const ConsistencyCache& a_cache)
{
    // ---- GET /consistency: latest audit result --------------------------
    // Read-only mirror of the periodic consistency audit (and the drained
    // unknown-leg notes it reported). The audit itself never repairs: it
    // only observes; reconciliation remains the healing path.
    CROW_ROUTE(a_app, "/consistency")
    ([&a_cache]() -> crow::response {
        try {
            return json_response(200, a_cache.get());
        } catch (...) {
            return internal_error_response();
        }
    });
}

} // namespace gateway::rest
