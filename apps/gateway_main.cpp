#include "core/config.hpp"
#include "exchange/okx/okx_connector.hpp"
#include "rest/order_routes.hpp"

#include <crow_all.h>

#include <format>
#include <iostream>
#include <string>

auto main(int a_argc, char* a_argv[]) -> int
{
    const std::string config_path = a_argc > 1 ? a_argv[1] : "config/gateway.json";

    const auto config = gateway::load_config(config_path);
    if (!config.is_ok()) {
        std::cerr << std::format("failed to load config {}: [{}] {}\n", config_path,
                                 config.error().code, config.error().message);
        return 1;
    }

    const auto okx_config = gateway::exchange::okx::okx_config_from_json(config.value().okx);
    if (!okx_config.is_ok()) {
        std::cerr << std::format("invalid okx config: [{}] {}\n", okx_config.error().code,
                                 okx_config.error().message);
        return 1;
    }

    const gateway::exchange::okx::OkxConfig venue_config = okx_config.value();
    gateway::exchange::okx::OkxConnector connector{venue_config};

    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, connector);

    CROW_ROUTE(app, "/health")
    ([] {
        crow::json::wvalue body;
        body["status"] = "ok";
        return crow::response(body);
    });

    std::cout << std::format("rest_exchange_gateway listening on port {}\n",
                             config.value().rest_port);
    app.port(config.value().rest_port).concurrency(2).loglevel(crow::LogLevel::Warning).run();
    return 0;
}
