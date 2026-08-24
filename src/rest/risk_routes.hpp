#pragma once

#include "core/oms.hpp"

#include <crow_all.h>

namespace gateway::rest {

/// Register the read-only pre-trade risk surface on a_app:
///   GET /risk   active limits (config-file snapshot taken at startup)
/// The body mirrors the config "risk" section: an optional "default"
/// block plus "instruments" entries; an entry replaces the defaults
/// wholesale. Every limits field is a decimal string; an empty string
/// means that check is disabled for the scope, "default": null means
/// no defaults are configured. a_oms must outlive the app; error
/// responses use the shared envelope {"error": {code, reason,
/// clientOrderId}}.
void register_risk_routes(crow::SimpleApp& a_app, OrderManagementSystem& a_oms);

} // namespace gateway::rest
