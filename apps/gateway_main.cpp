#include "core/clock.hpp"
#include "core/config.hpp"
#include "exchange/okx/okx_connector.hpp"
#include "gateway/exchange_connector.hpp"
#include "rest/order_routes.hpp"

#include <crow_all.h>
#include <nlohmann/json.hpp>

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

    connector.set_execution_report_handler([](const gateway::ExecutionReport& a_report) {
        const nlohmann::json line = {{"ts", gateway::utc_now_iso_ms()},
                                     {"event", "execution_report"},
                                     {"clientOrderId", a_report.client_order_id},
                                     {"exchangeOrderId", a_report.exchange_order_id},
                                     {"state", gateway::to_string(a_report.state)},
                                     {"filledQuantity", a_report.filled_quantity},
                                     {"averageFillPrice", a_report.average_fill_price}};
        std::cout << line.dump() << '\n' << std::flush;
    });

    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, connector);

    CROW_ROUTE(app, "/health")
    ([] {
        crow::json::wvalue body;
        body["status"] = "ok";
        return crow::response(body);
    });

    connector.start();
    std::cout << std::format("rest_exchange_gateway listening on port {} (okx feed {})\n",
                             config.value().rest_port,
                             venue_config.ws.enabled ? "enabled" : "disabled")
              << std::flush;
    app.port(config.value().rest_port).concurrency(2).loglevel(crow::LogLevel::Warning).run();

    connector.stop();
    return 0;
}
