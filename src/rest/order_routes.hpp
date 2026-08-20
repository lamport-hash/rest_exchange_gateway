#pragma once

#include "gateway/exchange_connector.hpp"

#include <crow_all.h>

namespace gateway::rest {

/// Register POST/GET/DELETE /orders and the exception backstop on a_app.
/// a_connector must outlive the app. All error responses use the structured
/// schema {"error": {"code", "reason", "clientOrderId"}}; exceptions raised
/// inside handlers are converted to structured 500 responses and never
/// cross the REST boundary.
void register_order_routes(crow::SimpleApp& a_app, ExchangeConnector& a_connector);

} // namespace gateway::rest
