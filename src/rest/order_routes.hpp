#pragma once

#include "core/oms.hpp"

#include <crow_all.h>

namespace gateway::rest {

/// Register the complete client-facing API on a_app:
///   POST   /orders            place (common schema, strict field allowlist)
///   GET    /orders            registry listing (all orders, by clientOrderId)
///   GET    /orders/{id}       registry view (OMS state)
///   DELETE /orders/{id}       cancel (idempotent)
///   PUT    /orders/{id}       amend (price and/or quantity)
///   GET    /price/{symbol}    last-traded price (?venue=..., default venue)
///   GET    /health            liveness + registry size
/// a_oms must outlive the app. All error responses use the structured
/// schema {"error": {"code", "reason", "clientOrderId"}}; exceptions
/// raised inside handlers are converted to structured 500 responses and
/// never cross the REST boundary.
void register_order_routes(crow::SimpleApp& a_app, OrderManagementSystem& a_oms);

} // namespace gateway::rest
