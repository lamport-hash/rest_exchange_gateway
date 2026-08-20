#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "util/free_port.hpp"

#include <crow_all.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {

TEST_CASE("crow /health and /echo served in-process and driven by cpp-httplib")
{
    crow::SimpleApp app;

    CROW_ROUTE(app, "/health")
    ([] {
        crow::json::wvalue body;
        body["status"] = "ok";
        return crow::response(body);
    });

    CROW_ROUTE(app, "/echo").methods(crow::HTTPMethod::POST)([](const crow::request& a_req) {
        const auto parsed = nlohmann::json::parse(a_req.body, nullptr, false);
        if (parsed.is_discarded()) {
            return crow::response(400);
        }
        crow::json::wvalue body;
        body["echo"] = parsed.dump();
        return crow::response(body);
    });

    const std::uint16_t port = gateway::testing::pick_free_port();
    auto server = app.port(port).concurrency(1).loglevel(crow::LogLevel::Warning).run_async();
    app.wait_for_server_start();

    httplib::Client client("127.0.0.1", static_cast<int>(port));

    SUBCASE("GET /health returns status ok")
    {
        const auto res = client.Get("/health");
        REQUIRE(res != nullptr);
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        CHECK(body["status"] == "ok");
    }

    SUBCASE("POST /echo parses and returns the JSON body")
    {
        const auto res = client.Post("/echo", R"({"hello":"gateway"})", "application/json");
        REQUIRE(res != nullptr);
        CHECK(res->status == 200);
        const auto body = nlohmann::json::parse(res->body);
        CHECK(body["echo"] == R"({"hello":"gateway"})");
    }

    SUBCASE("POST /echo with malformed JSON is rejected")
    {
        const auto res = client.Post("/echo", "not json", "application/json");
        REQUIRE(res != nullptr);
        CHECK(res->status == 400);
    }

    app.stop();
    server.wait();
}

} // namespace
