#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/okx_mock_server.hpp"
#include "util/free_port.hpp"

#include "exchange/okx/okx_connector.hpp"
#include "rest/order_routes.hpp"

#include <crow_all.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using gateway::testing::OkxMockServer;
using namespace gateway;
using namespace gateway::exchange::okx;

auto base_config() -> OkxConfig
{
    return OkxConfig{.api_key = "test-key",
                     .secret_key = "test-secret",
                     .passphrase = "test-pass",
                     .host = "127.0.0.1",
                     .port = 0,
                     .use_tls = false,
                     .demo_trading = true,
                     .retry = gateway::RetryPolicy{},
                     .ws = gateway::exchange::okx::OkxWsConfig{}};
}

class GatewayFixture
{
  public:
    using PreStartHook = std::function<void(crow::SimpleApp&, OkxMockServer&)>;

    explicit GatewayFixture(PreStartHook a_pre_start = nullptr) : server_(base_config())
    {
        server_.start();
        auto config = base_config();
        config.port = static_cast<int>(server_.port());
        connector_ = std::make_unique<OkxConnector>(
            config, [] { return std::string("2026-08-20T10:00:00.000Z"); });
        gateway::rest::register_order_routes(app_, *connector_);

        CROW_ROUTE(app_, "/health")
        ([] {
            crow::json::wvalue body;
            body["status"] = "ok";
            return crow::response(body);
        });

        if (a_pre_start) {
            a_pre_start(app_, server_);
        }

        const auto port = gateway::testing::pick_free_port();
        server_future_ =
            app_.port(port).concurrency(1).loglevel(crow::LogLevel::Warning).run_async();
        app_.wait_for_server_start();
        client_ = std::make_unique<httplib::Client>("127.0.0.1", static_cast<int>(port));
    }

    ~GatewayFixture()
    {
        app_.stop();
        server_future_.wait();
    }

    [[nodiscard]] auto client() -> httplib::Client& { return *client_; }

    [[nodiscard]] auto mock() -> OkxMockServer& { return server_; }

    [[nodiscard]] auto app() -> crow::SimpleApp& { return app_; }

  private:
    OkxMockServer server_;
    std::unique_ptr<OkxConnector> connector_;
    crow::SimpleApp app_;
    std::future<void> server_future_;
    std::unique_ptr<httplib::Client> client_;
};

auto error_body(const httplib::Result& a_result) -> nlohmann::json
{
    REQUIRE(a_result != nullptr);
    return nlohmann::json::parse(a_result->body);
}

TEST_CASE("place and fetch an order end-to-end")
{
    GatewayFixture fixture;

    const auto place = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})",
        "application/json");
    REQUIRE(place != nullptr);
    REQUIRE(place->status == 201);
    const auto placed = nlohmann::json::parse(place->body);
    CHECK(placed["clientOrderId"] == "gw0001");
    CHECK(placed["exchangeOrderId"] == "mock-1");

    const auto get = fixture.client().Get("/orders/gw0001?instrumentId=BTC-USDT");
    REQUIRE(get != nullptr);
    REQUIRE(get->status == 200);
    const auto snapshot = nlohmann::json::parse(get->body);
    CHECK(snapshot["state"] == "live");
    CHECK(snapshot["price"] == "50000");
    CHECK(snapshot["quantity"] == "0.001");
    CHECK(snapshot["filledQuantity"] == "0");
}

TEST_CASE("partial fill is visible through the gateway")
{
    GatewayFixture fixture;

    REQUIRE(
        fixture.client()
            .Post(
                "/orders",
                R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})",
                "application/json")
            ->status == 201);
    fixture.mock().apply_fill("gw0001", "0.0004", "49999.5");

    const auto get = fixture.client().Get("/orders/gw0001?instrumentId=BTC-USDT");
    REQUIRE(get != nullptr);
    REQUIRE(get->status == 200);
    const auto snapshot = nlohmann::json::parse(get->body);
    CHECK(snapshot["state"] == "partially_filled");
    CHECK(snapshot["filledQuantity"] == "0.0004");
    CHECK(snapshot["averageFillPrice"] == "49999.5");
}

TEST_CASE("cancel an order through the gateway")
{
    GatewayFixture fixture;

    REQUIRE(
        fixture.client()
            .Post(
                "/orders",
                R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})",
                "application/json")
            ->status == 201);

    const auto cancel = fixture.client().Delete("/orders/gw0001?instrumentId=BTC-USDT");
    REQUIRE(cancel != nullptr);
    REQUIRE(cancel->status == 200);
    CHECK(nlohmann::json::parse(cancel->body)["exchangeOrderId"] == "mock-1");

    const auto get = fixture.client().Get("/orders/gw0001?instrumentId=BTC-USDT");
    REQUIRE(get != nullptr);
    CHECK(nlohmann::json::parse(get->body)["state"] == "canceled");
}

TEST_CASE("validation errors use the structured error schema")
{
    GatewayFixture fixture;

    SUBCASE("malformed JSON body")
    {
        const auto res = fixture.client().Post("/orders", "not json", "application/json");
        REQUIRE(res->status == 400);
        const auto error = error_body(res)["error"];
        CHECK(error["code"] == "invalid_request");
        CHECK(error.contains("reason"));
        CHECK(error.contains("clientOrderId"));
    }

    SUBCASE("missing instrumentId")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","side":"buy","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("bad side")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"hodl","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("bad price format")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"cheap","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("limit without price")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("market with price")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"market","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("negative quantity")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"-2"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("hyphenated clientOrderId")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw-0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("clientOrderId longer than 32 characters")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("missing instrumentId query on GET")
    {
        const auto res = fixture.client().Get("/orders/gw0001");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }
}

TEST_CASE("venue errors map to structured responses")
{
    GatewayFixture fixture;

    SUBCASE("a retried place with the same clientOrderId yields the same order")
    {
        const auto body =
            R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})";
        const auto first = fixture.client().Post("/orders", body, "application/json");
        REQUIRE(first->status == 201);
        const auto second = fixture.client().Post("/orders", body, "application/json");
        REQUIRE(second->status == 201);
        const auto first_json = nlohmann::json::parse(first->body);
        const auto second_json = nlohmann::json::parse(second->body);
        CHECK(second_json["exchangeOrderId"] == first_json["exchangeOrderId"]);
    }

    SUBCASE("unknown order on GET becomes 404 not_found")
    {
        const auto res = fixture.client().Get("/orders/ghost?instrumentId=BTC-USDT");
        REQUIRE(res->status == 404);
        const auto error = error_body(res)["error"];
        CHECK(error["code"] == "not_found");
        CHECK(error["clientOrderId"] == "ghost");
    }

    SUBCASE("unknown order on DELETE becomes 404 not_found")
    {
        const auto res = fixture.client().Delete("/orders/ghost?instrumentId=BTC-USDT");
        REQUIRE(res->status == 404);
        CHECK(error_body(res)["error"]["code"] == "not_found");
    }

    SUBCASE("cancelling twice both succeed (idempotent cancel)")
    {
        const auto place = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})",
            "application/json");
        REQUIRE(place->status == 201);

        const auto first = fixture.client().Delete("/orders/gw0001?instrumentId=BTC-USDT");
        REQUIRE(first->status == 200);
        const auto second = fixture.client().Delete("/orders/gw0001?instrumentId=BTC-USDT");
        REQUIRE(second->status == 200);
        CHECK(nlohmann::json::parse(second->body)["exchangeOrderId"] == "mock-1");
    }
}

TEST_CASE("a dropped venue response is retried transparently behind the REST API")
{
    GatewayFixture fixture;
    fixture.mock().drop_next_request();

    const auto res = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})",
        "application/json");
    REQUIRE(res->status == 201);
    CHECK(nlohmann::json::parse(res->body)["exchangeOrderId"] == "mock-1");

    // the venue ends up with exactly one live order (no double placement)
    const auto get = fixture.client().Get("/orders/gw0001?instrumentId=BTC-USDT");
    REQUIRE(get->status == 200);
    CHECK(nlohmann::json::parse(get->body)["state"] == "live");
}

TEST_CASE("venue connectivity problems become 502 venue_unavailable")
{
    const auto dead_port = gateway::testing::pick_free_port();
    auto config = base_config();
    config.port = static_cast<int>(dead_port);
    OkxConnector connector{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};

    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, connector);
    const auto port = gateway::testing::pick_free_port();
    auto future = app.port(port).concurrency(1).loglevel(crow::LogLevel::Warning).run_async();
    app.wait_for_server_start();
    httplib::Client client{"127.0.0.1", static_cast<int>(port)};

    const auto res = client.Post(
        "/orders",
        R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})",
        "application/json");
    REQUIRE(res->status == 502);
    CHECK(error_body(res)["error"]["code"] == "venue_unavailable");

    app.stop();
    future.wait();
}

TEST_CASE("malformed venue payloads become 500 internal")
{
    GatewayFixture fixture;
    fixture.mock().set_next_raw_response(200, "[not json");

    const auto res = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})",
        "application/json");
    REQUIRE(res->status == 500);
    CHECK(error_body(res)["error"]["code"] == "internal");
}

TEST_CASE("handler exceptions never cross the boundary")
{
    GatewayFixture fixture{[](crow::SimpleApp& a_app, OkxMockServer&) {
        CROW_ROUTE(a_app, "/boom")([]() -> crow::response { throw std::runtime_error("kaboom"); });
    }};

    const auto res = fixture.client().Get("/boom");
    REQUIRE(res->status == 500);
    CHECK(error_body(res)["error"]["code"] == "internal");
}

TEST_CASE("health endpoint answers ok")
{
    GatewayFixture fixture;
    const auto res = fixture.client().Get("/health");
    REQUIRE(res->status == 200);
    CHECK(nlohmann::json::parse(res->body)["status"] == "ok");
}

TEST_CASE("market orders reach the venue without a price field")
{
    GatewayFixture fixture;

    const auto res = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0001","instrumentId":"BTC-USDT","side":"sell","type":"market","quantity":"0.001"})",
        "application/json");
    REQUIRE(res->status == 201);

    const auto recorded = fixture.mock().recorded_requests();
    REQUIRE(recorded.size() == 1);
    const auto body = nlohmann::json::parse(recorded.front().body);
    CHECK(body["ordType"] == "market");
    CHECK(body["side"] == "sell");
    CHECK_FALSE(body.contains("px"));
}

} // namespace
