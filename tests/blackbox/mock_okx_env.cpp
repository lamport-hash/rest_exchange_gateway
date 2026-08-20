// Standalone black-box fake OKX venue for the Phase 2 client test script.
// Composes the in-process mocks (REST + private WebSocket) with a plain-HTTP
// control plane (key=value responses, so the driving script needs only curl).
//
//   mock_okx_env [control_port]        (default 18080)
//
// Control endpoints (all on 127.0.0.1):
//   GET  /status                       -> rest_port=N, ws_port=N (one per line)
//   GET  /stats                        -> counters, see below
//   POST /fault/drop-next-request      -> next REST request dropped mid-response
//   POST /fault/drop-next-response     -> next REST success processed, ack dropped
//   POST /fault/delay-next  {"ms":N}   -> next REST response delayed by N ms
//   POST /rest/stop | /rest/start      -> venue death / rebirth on the same port
//   POST /ws/push       <orders item>  -> push one orders-channel data item
//   POST /ws/kill                      -> clean-close every WS session
//   POST /ws/restart                   -> kill endpoint abruptly, rebind same port
//   POST /exit                         -> shut everything down
//
// /stats fields:
//   rest_place rest_get rest_cancel rest_amend rest_total
//   ws_connections ws_logins_ok ws_logins_failed ws_subscribed ws_pushes
#include "mocks/okx_mock_server.hpp"
#include "mocks/okx_mock_ws_server.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using gateway::testing::OkxMockServer;
using gateway::testing::OkxMockWsServer;

auto venue_config() -> gateway::exchange::okx::OkxConfig
{
    return gateway::exchange::okx::OkxConfig{.api_key = "blackbox-key",
                                             .secret_key = "blackbox-secret",
                                             .passphrase = "blackbox-pass",
                                             .host = "127.0.0.1",
                                             .port = 0,
                                             .use_tls = false,
                                             .demo_trading = true,
                                             .retry = gateway::RetryPolicy{},
                                             .ws = gateway::exchange::okx::OkxWsConfig{}};
}

auto text_response(httplib::Response& a_res, const std::string& a_body) -> void
{
    a_res.status = 200;
    a_res.set_content(a_body, "text/plain");
}

auto count_target(const std::vector<OkxMockServer::RecordedRequest>& a_recorded,
                  const std::string& a_method, const std::string& a_target) -> int
{
    int total = 0;
    for (const auto& request : a_recorded) {
        if (request.method == a_method && request.target == a_target) {
            ++total;
        }
    }
    return total;
}

auto count_target_prefix(const std::vector<OkxMockServer::RecordedRequest>& a_recorded,
                         const std::string& a_method, const std::string& a_prefix) -> int
{
    int total = 0;
    for (const auto& request : a_recorded) {
        if (request.method == a_method && request.target.rfind(a_prefix, 0) == 0) {
            ++total;
        }
    }
    return total;
}

} // namespace

auto main(int a_argc, char* a_argv[]) -> int
{
    int control_port = 18080;
    if (a_argc > 1) {
        control_port = std::stoi(a_argv[1]);
    }

    const auto config = venue_config();
    OkxMockServer rest{config};
    rest.start();
    OkxMockWsServer ws{config};
    ws.start();

    httplib::Server control;

    control.Get("/status", [&rest, &ws](const httplib::Request&, httplib::Response& res) {
        text_response(res, "rest_port=" + std::to_string(rest.port()) + "\n" +
                               "ws_port=" + std::to_string(ws.port()) + "\n");
    });

    control.Get("/stats", [&rest, &ws](const httplib::Request&, httplib::Response& res) {
        const auto recorded = rest.recorded_requests();
        const auto ws_stats = ws.stats();
        std::string body;
        body +=
            "rest_place=" + std::to_string(count_target(recorded, "POST", "/api/v5/trade/order")) +
            "\n";
        body += "rest_get=" +
                std::to_string(count_target_prefix(recorded, "GET", "/api/v5/trade/order?")) + "\n";
        body += "rest_cancel=" +
                std::to_string(count_target(recorded, "POST", "/api/v5/trade/cancel-order")) + "\n";
        body += "rest_amend=" +
                std::to_string(count_target(recorded, "POST", "/api/v5/trade/amend-order")) + "\n";
        body += "rest_total=" + std::to_string(recorded.size()) + "\n";
        body += "ws_connections=" + std::to_string(ws_stats.connections) + "\n";
        body += "ws_logins_ok=" + std::to_string(ws_stats.logins_ok) + "\n";
        body += "ws_logins_failed=" + std::to_string(ws_stats.logins_failed) + "\n";
        body += "ws_subscribed=" + std::to_string(ws_stats.subscribed_sessions) + "\n";
        body += "ws_pushes=" + std::to_string(ws_stats.pushes_delivered) + "\n";
        text_response(res, body);
    });

    control.Post("/fault/drop-next-request",
                 [&rest](const httplib::Request&, httplib::Response& res) {
                     rest.drop_next_request();
                     text_response(res, "ok=1\n");
                 });
    control.Post("/fault/drop-next-response",
                 [&rest](const httplib::Request&, httplib::Response& res) {
                     rest.drop_next_response();
                     text_response(res, "ok=1\n");
                 });
    control.Post("/fault/delay-next", [&rest](const httplib::Request& req, httplib::Response& res) {
        const auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("ms") || !body.at("ms").is_number_unsigned()) {
            res.status = 400;
            res.set_content("expected {\"ms\": <number>}", "text/plain");
            return;
        }
        rest.delay_next_request(static_cast<unsigned>(body.at("ms").get<std::uint64_t>()));
        text_response(res, "ok=1\n");
    });

    control.Post("/rest/stop", [&rest](const httplib::Request&, httplib::Response& res) {
        rest.stop();
        text_response(res, "ok=1\n");
    });
    control.Post("/rest/start", [&rest](const httplib::Request&, httplib::Response& res) {
        try {
            rest.restart_on_same_port();
            text_response(res, "ok=1\n");
        } catch (const std::runtime_error& a_error) {
            res.status = 500;
            res.set_content(a_error.what(), "text/plain");
        }
    });

    control.Post("/ws/push", [&ws](const httplib::Request& req, httplib::Response& res) {
        const auto item = nlohmann::json::parse(req.body, nullptr, false);
        if (item.is_discarded() || !item.is_object()) {
            res.status = 400;
            res.set_content("expected an orders-channel data item object", "text/plain");
            return;
        }
        ws.push_orders_update(item);
        text_response(res, "ok=1\n");
    });
    control.Post("/ws/kill", [&ws](const httplib::Request&, httplib::Response& res) {
        ws.kill_connections();
        text_response(res, "ok=1\n");
    });
    control.Post("/ws/restart", [&ws](const httplib::Request&, httplib::Response& res) {
        try {
            ws.restart_on_same_port();
            text_response(res, "ok=1\n");
        } catch (const std::runtime_error& a_error) {
            res.status = 500;
            res.set_content(a_error.what(), "text/plain");
        }
    });

    control.Post("/exit", [&control](const httplib::Request&, httplib::Response& res) {
        text_response(res, "ok=1\n");
        // stop() must not run on a handler thread (it joins worker threads)
        std::thread([&control] { control.stop(); }).detach();
    });

    std::cout << "mock_okx_env ready control_port=" << control_port << " rest_port=" << rest.port()
              << " ws_port=" << ws.port() << '\n'
              << std::flush;

    control.listen("127.0.0.1", control_port);

    rest.stop();
    ws.stop();
    return 0;
}
