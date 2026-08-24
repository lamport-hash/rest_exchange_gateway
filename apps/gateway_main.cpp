#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/event_log.hpp"
#include "core/oms.hpp"
#include "exchange/binance/binance_config.hpp"
#include "exchange/binance/binance_connector.hpp"
#include "exchange/okx/okx_connector.hpp"
#include "exchange/okx/okx_rest_client.hpp"
#include "gateway/exchange_connector.hpp"
#include "rest/okx_demo_routes.hpp"
#include "rest/order_routes.hpp"
#include "rest/risk_routes.hpp"

#include <crow_all.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace {

auto reconcile_summary_line(const gateway::ReconcileReport& a_report) -> nlohmann::json
{
    return {{"ts", gateway::utc_now_iso_ms()},
            {"event", "reconcile"},
            {"adopted", a_report.adopted},
            {"updated", a_report.updated},
            {"terminalResolved", a_report.terminal_resolved},
            {"absentRejected", a_report.absent_rejected},
            {"unresolved", a_report.unresolved},
            {"pendingListingFailed", a_report.pending_listing_failed}};
}

} // namespace

auto main(int a_argc, char* a_argv[]) -> int
{
    const std::string config_path = a_argc > 1 ? a_argv[1] : "config/gateway.json";

    const auto config = gateway::load_config(config_path);
    if (!config.is_ok()) {
        std::cerr << std::format("failed to load config {}: [{}] {}\n", config_path,
                                 config.error().code, config.error().message);
        return 1;
    }

    // ---- venue connectors (composition root: the only place that knows
    // concrete exchanges; everything below sees ExchangeConnector) ----
    std::map<std::string, gateway::ExchangeConnector*> connectors;
    std::optional<gateway::exchange::okx::OkxConnector> okx_connector;
    std::optional<gateway::exchange::binance::BinanceConnector> binance_connector;

    if (config.value().okx.contains("apiKey") || config.value().okx.empty()) {
        const auto okx_config = gateway::exchange::okx::okx_config_from_json(config.value().okx);
        if (!okx_config.is_ok()) {
            std::cerr << std::format("invalid okx config: [{}] {}\n", okx_config.error().code,
                                     okx_config.error().message);
            return 1;
        }
        okx_connector.emplace(okx_config.value());
        connectors.emplace("okx", &okx_connector.value());
    }
    if (config.value().binance.contains("apiKey")) {
        const auto binance_config =
            gateway::exchange::binance::binance_config_from_json(config.value().binance);
        if (!binance_config.is_ok()) {
            std::cerr << std::format("invalid binance config: [{}] {}\n",
                                     binance_config.error().code, binance_config.error().message);
            return 1;
        }
        binance_connector.emplace(binance_config.value());
        connectors.emplace("binance", &binance_connector.value());
    }
    if (connectors.empty()) {
        std::cerr << "error: no venue is configured (need an okx or binance section)\n";
        return 1;
    }
    std::string default_venue = config.value().default_venue;
    if (connectors.find(default_venue) == connectors.end()) {
        default_venue = connectors.begin()->first;
        std::cerr << "warning: defaultVenue \"" << config.value().default_venue
                  << "\" is not configured; using \"" << default_venue << "\"\n";
    }

    // persistence log (optional): parent directory must exist
    std::optional<gateway::EventLog> event_log;
    if (config.value().persistence_log.has_value()) {
        const auto parent = config.value().persistence_log->parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec); // best effort
        }
        event_log.emplace(*config.value().persistence_log);
    } else {
        std::cerr << "warning: persistence is disabled (no persistence.logPath in config); "
                     "recovery relies on venue reconciliation\n";
    }
    if (config.value().risk.limits_for("BTC-USDT") == std::nullopt &&
        !config.value().risk.defaults.has_value() && config.value().risk.instruments.empty()) {
        std::cerr << "warning: no risk limits configured; pre-trade checks are disabled\n";
    }

    gateway::OrderManagementSystem oms{connectors,
                                       event_log.has_value() ? &event_log.value() : nullptr,
                                       config.value().risk, default_venue};

    // startup recovery: replay the local log before serving traffic
    const auto replayed = oms.load_from_log();
    if (!replayed.is_ok()) {
        std::cerr << std::format("cannot replay order log {}: [{}] {}\n",
                                 event_log.has_value() ? event_log->path().string() : "",
                                 replayed.error().code, replayed.error().message);
        return 1;
    }
    if (replayed.value().tail_truncated) {
        std::cerr << "warning: order log had a torn tail; it was truncated to the last "
                     "complete event\n";
    }
    std::cout << std::format("recovered {} events from the order log\n", replayed.value().events)
              << std::flush;

    const auto execution_report_line = [](const gateway::ExecutionReport& a_report) {
        const nlohmann::json line = {{"ts", gateway::utc_now_iso_ms()},
                                     {"event", "execution_report"},
                                     {"clientOrderId", a_report.client_order_id},
                                     {"exchangeOrderId", a_report.exchange_order_id},
                                     {"state", gateway::to_string(a_report.state)},
                                     {"filledQuantity", a_report.filled_quantity},
                                     {"averageFillPrice", a_report.average_fill_price}};
        std::cout << line.dump() << '\n' << std::flush;
    };
    if (okx_connector.has_value()) {
        okx_connector->set_execution_report_handler(
            [&oms, &execution_report_line](const gateway::ExecutionReport& a_report) {
                oms.on_execution_report(a_report);
                execution_report_line(a_report);
            });
    }
    if (binance_connector.has_value()) {
        binance_connector->set_execution_report_handler(
            [&oms, &execution_report_line](const gateway::ExecutionReport& a_report) {
                oms.on_execution_report(a_report);
                execution_report_line(a_report);
            });
    }

    // a (re)established execution feed may have missed updates: reconcile
    const auto install_connectivity = [&oms](gateway::ExchangeConnector& a_connector) {
        a_connector.set_connectivity_handler([&oms](bool a_connected) {
            if (!a_connected) {
                const nlohmann::json line = {{"ts", gateway::utc_now_iso_ms()},
                                             {"event", "feed_disconnected"}};
                std::cout << line.dump() << '\n' << std::flush;
                return;
            }
            const auto report = oms.reconcile();
            std::cout << reconcile_summary_line(report).dump() << '\n' << std::flush;
        });
    };
    if (okx_connector.has_value()) {
        install_connectivity(okx_connector.value());
    }
    if (binance_connector.has_value()) {
        install_connectivity(binance_connector.value());
    }

    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, oms);
    gateway::rest::register_risk_routes(app, oms);
    if (okx_connector.has_value()) {
        // OKX-only demo-trading surface; absent when OKX is not configured.
        gateway::rest::register_okx_demo_routes(app, okx_connector.value());
    }

    const auto start_venue = [](gateway::ExchangeConnector& a_connector) { a_connector.start(); };
    if (okx_connector.has_value()) {
        start_venue(okx_connector.value());
    }
    if (binance_connector.has_value()) {
        start_venue(binance_connector.value());
    }
    const auto startup_reconcile = oms.reconcile();
    std::cout << reconcile_summary_line(startup_reconcile).dump() << '\n' << std::flush;

    std::string venue_names;
    for (const auto& venue : oms.venues()) {
        if (!venue_names.empty()) {
            venue_names += ", ";
        }
        venue_names += venue;
    }
    std::cout << std::format(
                     "rest_exchange_gateway listening on port {} (venues: {}; default: {})\n",
                     config.value().rest_port, venue_names, default_venue)
              << std::flush;
    app.port(config.value().rest_port).concurrency(2).loglevel(crow::LogLevel::Warning).run();

    const auto stop_venue = [](gateway::ExchangeConnector& a_connector) { a_connector.stop(); };
    if (okx_connector.has_value()) {
        stop_venue(okx_connector.value());
    }
    if (binance_connector.has_value()) {
        stop_venue(binance_connector.value());
    }
    return 0;
}
