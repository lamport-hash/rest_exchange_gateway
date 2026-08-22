#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/okx_mock_server.hpp"
#include "mocks/okx_mock_ws_server.hpp"
#include "util/free_port.hpp"

#include "core/event_log.hpp"
#include "core/oms.hpp"
#include "exchange/okx/okx_connector.hpp"
#include "exchange/okx/okx_rest_client.hpp"
#include "rest/order_routes.hpp"

#include <crow_all.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using gateway::testing::OkxMockServer;
using gateway::testing::OkxMockWsServer;
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

auto okx_ws_config(const OkxMockWsServer& a_ws) -> OkxWsConfig
{
    return OkxWsConfig{.enabled = true,
                       .host = "127.0.0.1",
                       .port = static_cast<int>(a_ws.port()),
                       .use_tls = false,
                       .path = "/ws/v5/private",
                       .ping_interval = std::chrono::milliseconds{20000},
                       .max_missed_pongs = 2};
}

/// Scriptable in-memory stand-in for a second venue, registered as
/// "binance" so REST-level venue routing can be tested without any
/// exchange-specific code.
class FakeVenueConnector final : public ExchangeConnector
{
  public:
    std::function<Result<OrderPlacement>(const OrderRequest&)> place_impl =
        [](const OrderRequest& a_request) {
            return OrderPlacement{.client_order_id = a_request.client_order_id,
                                  .exchange_order_id = "fake-" + a_request.client_order_id};
        };
    std::function<Result<OrderPlacement>(const CancelRequest&)> cancel_impl =
        [](const CancelRequest& a_request) {
            return OrderPlacement{.client_order_id = a_request.client_order_id,
                                  .exchange_order_id = "fake-" + a_request.client_order_id};
        };
    std::function<Result<OrderPlacement>(const AmendRequest&)> amend_impl =
        [](const AmendRequest& a_request) {
            return OrderPlacement{.client_order_id = a_request.client_order_id,
                                  .exchange_order_id = "fake-" + a_request.client_order_id};
        };
    std::function<Result<std::optional<OrderSnapshot>>(const OrderQuery&)> get_impl =
        [](const OrderQuery&) -> Result<std::optional<OrderSnapshot>> {
        return Result<std::optional<OrderSnapshot>>{std::optional<OrderSnapshot>{std::nullopt}};
    };
    std::function<Result<std::vector<OrderSnapshot>>()> open_impl =
        []() -> Result<std::vector<OrderSnapshot>> { return std::vector<OrderSnapshot>{}; };
    std::vector<OrderRequest> placed;
    std::vector<AmendRequest> amends;
    std::vector<CancelRequest> cancels;

    [[nodiscard]] auto place_order(const OrderRequest& a_request) -> Result<OrderPlacement> override
    {
        placed.push_back(a_request);
        return place_impl(a_request);
    }
    [[nodiscard]] auto
    cancel_order(const CancelRequest& a_request) -> Result<OrderPlacement> override
    {
        cancels.push_back(a_request);
        return cancel_impl(a_request);
    }
    [[nodiscard]] auto amend_order(const AmendRequest& a_request) -> Result<OrderPlacement> override
    {
        amends.push_back(a_request);
        return amend_impl(a_request);
    }
    [[nodiscard]] auto
    get_order(const OrderQuery& a_query) -> Result<std::optional<OrderSnapshot>> override
    {
        return get_impl(a_query);
    }
    [[nodiscard]] auto get_open_orders() -> Result<std::vector<OrderSnapshot>> override
    {
        return open_impl();
    }
    void
    set_execution_report_handler(std::function<void(const ExecutionReport&)> a_handler) override
    {
        report_handler = std::move(a_handler);
    }
    void set_connectivity_handler(std::function<void(bool)> a_handler) override
    {
        connectivity_handler = std::move(a_handler);
    }
    void start() override {}
    void stop() override {}

    std::function<void(const ExecutionReport&)> report_handler;
    std::function<void(bool)> connectivity_handler;
};

/// Crow + OMS + OkxConnector (venue "okx") + FakeVenueConnector (venue
/// "binance") against in-process mock OKX (REST + WS).
class GatewayFixture
{
  public:
    struct Options
    {
        bool with_persistence = false;
        gateway::RiskConfig risk{};
    };
    using PreStartHook =
        std::function<void(crow::SimpleApp&, OkxConnector&, OkxMockServer&, OkxMockWsServer&)>;

    explicit GatewayFixture() : GatewayFixture(Options{}, nullptr) {}

    explicit GatewayFixture(Options a_options, PreStartHook a_pre_start = nullptr)
    {
        server_ = std::make_unique<OkxMockServer>(base_config());
        server_->start();
        ws_server_ = std::make_unique<OkxMockWsServer>(base_config());
        ws_server_->start();

        if (a_options.with_persistence) {
            log_path_ = std::filesystem::temp_directory_path() /
                        ("gateway_rest_test_" + std::to_string(++instance_counter_) + ".jsonl");
            std::filesystem::remove(log_path_);
            event_log_ = std::make_unique<gateway::EventLog>(log_path_);
        }

        auto config = base_config();
        config.port = static_cast<int>(server_->port());
        config.ws = okx_ws_config(*ws_server_);
        connector_ = std::make_unique<OkxConnector>(
            config, [] { return std::string("2026-08-20T10:00:00.000Z"); });

        oms_ = std::make_unique<gateway::OrderManagementSystem>(
            std::map<std::string, gateway::ExchangeConnector*>{{"okx", connector_.get()},
                                                               {"binance", &fake_binance_}},
            event_log_ ? event_log_.get() : nullptr, a_options.risk);
        connector_->set_execution_report_handler(
            [this](const ExecutionReport& a_report) { oms_->on_execution_report(a_report); });

        gateway::rest::register_order_routes(app_, *oms_);

        if (a_pre_start) {
            a_pre_start(app_, *connector_, *server_, *ws_server_);
        }

        connector_->start();
        const auto reconcile = oms_->reconcile();
        (void)reconcile;

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
        connector_->stop();
    }

    GatewayFixture(const GatewayFixture&) = delete;
    auto operator=(const GatewayFixture&) -> GatewayFixture& = delete;

    [[nodiscard]] auto client() -> httplib::Client& { return *client_; }
    [[nodiscard]] auto mock() -> OkxMockServer& { return *server_; }
    [[nodiscard]] auto ws() -> OkxMockWsServer& { return *ws_server_; }
    [[nodiscard]] auto binance() -> FakeVenueConnector& { return fake_binance_; }
    [[nodiscard]] auto log_path() const -> const std::filesystem::path& { return log_path_; }

    /// Push an orders-channel update for a client order id and its state.
    void push_update(const std::string& a_id, const std::string& a_state, const std::string& a_fill,
                     const std::string& a_avg = "50000")
    {
        REQUIRE(ws().wait_for_subscriber(5000));
        ws().push_orders_update(nlohmann::json{{"instId", "BTC-USDT"},
                                               {"ordId", "mock-1"},
                                               {"clOrdId", a_id},
                                               {"state", a_state},
                                               {"side", "buy"},
                                               {"px", "50000"},
                                               {"sz", "0.001"},
                                               {"accFillSz", a_fill},
                                               {"avgPx", a_avg}});
    }

    /// Poll the registry view until the order reaches a_state.
    [[nodiscard]] auto wait_for_state(const std::string& a_id, const std::string& a_state,
                                      int a_timeout_ms = 5000) -> bool
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{a_timeout_ms};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto res = client().Get("/orders/" + a_id);
            if (res != nullptr && res->status == 200) {
                const auto body = nlohmann::json::parse(res->body);
                if (body["state"] == a_state) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    [[nodiscard]] static auto next_log_path() -> std::filesystem::path
    {
        static int counter = 0;
        return std::filesystem::temp_directory_path() /
               ("gateway_rest_drill_" + std::to_string(++counter) + ".jsonl");
    }

  private:
    static inline int instance_counter_ = 0;
    std::filesystem::path log_path_{};
    FakeVenueConnector fake_binance_;
    std::unique_ptr<OkxMockServer> server_;
    std::unique_ptr<OkxMockWsServer> ws_server_;
    std::unique_ptr<gateway::EventLog> event_log_;
    std::unique_ptr<OkxConnector> connector_;
    std::unique_ptr<gateway::OrderManagementSystem> oms_;
    crow::SimpleApp app_;
    std::future<void> server_future_;
    std::unique_ptr<httplib::Client> client_;
};

auto error_body(const httplib::Result& a_result) -> nlohmann::json
{
    REQUIRE(a_result != nullptr);
    return nlohmann::json::parse(a_result->body);
}

constexpr const char* kPlaceBody =
    R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})";

} // namespace

TEST_CASE("place and fetch an order end-to-end through the OMS")
{
    GatewayFixture fixture;

    const auto place = fixture.client().Post("/orders", kPlaceBody, "application/json");
    REQUIRE(place != nullptr);
    REQUIRE(place->status == 201);
    const auto placed = nlohmann::json::parse(place->body);
    CHECK(placed["clientOrderId"] == "gw0001");
    CHECK(placed["exchangeOrderId"] == "mock-1");
    CHECK(placed["state"] == "live");
    CHECK(placed["symbol"] == "BTC-USDT");
    CHECK(placed["replayed"] == false);

    const auto get = fixture.client().Get("/orders/gw0001");
    REQUIRE(get != nullptr);
    REQUIRE(get->status == 200);
    const auto snapshot = nlohmann::json::parse(get->body);
    CHECK(snapshot["state"] == "live");
    CHECK(snapshot["side"] == "buy");
    CHECK(snapshot["type"] == "limit");
    CHECK(snapshot["timeInForce"] == "GTC");
    CHECK(snapshot["price"] == "50000");
    CHECK(snapshot["quantity"] == "0.001");
    CHECK(snapshot["filledQuantity"] == "0");

    // GET is served from the registry: the venue sees no order-info
    // lookups (only the startup reconcile's orders-pending listing)
    const auto recorded = fixture.mock().recorded_requests();
    const auto gets = std::count_if(
        recorded.begin(), recorded.end(), [](const OkxMockServer::RecordedRequest& a_request) {
            return a_request.method == "GET" &&
                   a_request.target.rfind("/api/v5/trade/order?", 0) == 0;
        });
    CHECK(gets == 0);
}

TEST_CASE("GET /orders lists the registry sorted by clientOrderId")
{
    GatewayFixture fixture;

    const auto empty = fixture.client().Get("/orders");
    REQUIRE(empty != nullptr);
    REQUIRE(empty->status == 200);
    const auto empty_body = nlohmann::json::parse(empty->body);
    REQUIRE(empty_body["orders"].is_array());
    CHECK(empty_body["orders"].empty());

    REQUIRE(
        fixture.client()
            .Post(
                "/orders",
                R"({"clientOrderId":"gw0002","symbol":"BTC-USDT","side":"sell","type":"limit","price":"51000","quantity":"0.002"})",
                "application/json")
            ->status == 201);
    REQUIRE(fixture.client().Post("/orders", kPlaceBody, "application/json")->status == 201);
    REQUIRE(
        fixture.client()
            .Post(
                "/orders",
                R"({"clientOrderId":"gw0003","venue":"binance","symbol":"BTC-USDT","side":"buy","type":"limit","price":"49000","quantity":"0.003"})",
                "application/json")
            ->status == 201);

    const auto list = fixture.client().Get("/orders");
    REQUIRE(list != nullptr);
    REQUIRE(list->status == 200);
    const auto body = nlohmann::json::parse(list->body);
    const auto& orders = body["orders"];
    REQUIRE(orders.size() == 3);
    // sorted by clientOrderId regardless of placement order
    CHECK(orders[0]["clientOrderId"] == "gw0001");
    CHECK(orders[1]["clientOrderId"] == "gw0002");
    CHECK(orders[2]["clientOrderId"] == "gw0003");
    // full record shape, same as the per-id GET
    CHECK(orders[0]["exchangeOrderId"] == "mock-2"); // venue ids follow placement order
    CHECK(orders[0]["state"] == "live");
    CHECK(orders[0]["symbol"] == "BTC-USDT");
    CHECK(orders[0]["venue"] == "okx");
    CHECK(orders[0]["side"] == "buy");
    CHECK(orders[0]["price"] == "50000");
    CHECK(orders[0]["quantity"] == "0.001");
    CHECK(orders[0]["filledQuantity"] == "0");
    CHECK(orders[2]["venue"] == "binance");

    // the listing is a snapshot of live registry state
    fixture.push_update("gw0001", "canceled", "0");
    REQUIRE(fixture.wait_for_state("gw0001", "canceled"));
    const auto after = nlohmann::json::parse(fixture.client().Get("/orders")->body);
    CHECK(after["orders"].size() == 3);
    CHECK(after["orders"][0]["state"] == "canceled");
}

TEST_CASE("execution reports from the venue feed keep the registry current")
{
    GatewayFixture fixture;

    REQUIRE(fixture.client().Post("/orders", kPlaceBody, "application/json")->status == 201);
    fixture.push_update("gw0001", "partially_filled", "0.0004", "49999.5");
    REQUIRE(fixture.wait_for_state("gw0001", "partially_filled"));

    const auto get = fixture.client().Get("/orders/gw0001");
    REQUIRE(get->status == 200);
    const auto snapshot = nlohmann::json::parse(get->body);
    CHECK(snapshot["filledQuantity"] == "0.0004");
    CHECK(snapshot["averageFillPrice"] == "49999.5");

    // a stale duplicate of the earlier report changes nothing
    fixture.push_update("gw0001", "partially_filled", "0.0004", "49999.5");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto again = nlohmann::json::parse(fixture.client().Get("/orders/gw0001")->body);
    CHECK(again["filledQuantity"] == "0.0004");
    CHECK(again["state"] == "partially_filled");
}

TEST_CASE("cancel an order through the gateway")
{
    GatewayFixture fixture;

    REQUIRE(fixture.client().Post("/orders", kPlaceBody, "application/json")->status == 201);

    const auto cancel = fixture.client().Delete("/orders/gw0001");
    REQUIRE(cancel != nullptr);
    REQUIRE(cancel->status == 200);
    CHECK(nlohmann::json::parse(cancel->body)["exchangeOrderId"] == "mock-1");
    CHECK(nlohmann::json::parse(cancel->body)["state"] == "canceled");

    const auto get = fixture.client().Get("/orders/gw0001");
    REQUIRE(get->status == 200);
    CHECK(nlohmann::json::parse(get->body)["state"] == "canceled");

    // second cancel is idempotent at the gateway: same outcome, no venue call
    const auto recorded_before = fixture.mock().recorded_requests();
    const auto cancels_before = std::count_if(
        recorded_before.begin(), recorded_before.end(),
        [](const OkxMockServer::RecordedRequest& a_request) {
            return a_request.method == "POST" && a_request.target == "/api/v5/trade/cancel-order";
        });
    const auto second = fixture.client().Delete("/orders/gw0001");
    REQUIRE(second->status == 200);
    CHECK(nlohmann::json::parse(second->body)["state"] == "canceled");
    const auto recorded_after = fixture.mock().recorded_requests();
    const auto cancels_after = std::count_if(
        recorded_after.begin(), recorded_after.end(),
        [](const OkxMockServer::RecordedRequest& a_request) {
            return a_request.method == "POST" && a_request.target == "/api/v5/trade/cancel-order";
        });
    CHECK(cancels_after == cancels_before);
}

TEST_CASE("amend an order via PUT /orders/{id}")
{
    GatewayFixture fixture;

    REQUIRE(fixture.client().Post("/orders", kPlaceBody, "application/json")->status == 201);

    const auto amend = fixture.client().Put(
        "/orders/gw0001", R"({"price":"49000","quantity":"0.002"})", "application/json");
    REQUIRE(amend != nullptr);
    REQUIRE(amend->status == 200);
    const auto body = nlohmann::json::parse(amend->body);
    CHECK(body["price"] == "49000");
    CHECK(body["quantity"] == "0.002");
    CHECK(body["state"] == "live");

    const auto get = nlohmann::json::parse(fixture.client().Get("/orders/gw0001")->body);
    CHECK(get["price"] == "49000");
    CHECK(get["quantity"] == "0.002");

    SUBCASE("amend with no fields is a 400")
    {
        const auto empty = fixture.client().Put("/orders/gw0001", "{}", "application/json");
        REQUIRE(empty->status == 400);
        CHECK(error_body(empty)["error"]["code"] == "invalid_request");
    }

    SUBCASE("amend of an unknown order is a 404")
    {
        const auto missing =
            fixture.client().Put("/orders/ghost", R"({"price":"1"})", "application/json");
        REQUIRE(missing->status == 404);
        CHECK(error_body(missing)["error"]["code"] == "not_found");
    }

    SUBCASE("amend of a terminal order is a 409")
    {
        REQUIRE(fixture.client().Delete("/orders/gw0001")->status == 200);
        const auto terminal =
            fixture.client().Put("/orders/gw0001", R"({"price":"1"})", "application/json");
        REQUIRE(terminal->status == 409);
        CHECK(error_body(terminal)["error"]["code"] == "order_terminal");
    }
}

TEST_CASE("strict idempotency: retrying a known clientOrderId replays the outcome")
{
    GatewayFixture fixture;

    const auto first = fixture.client().Post("/orders", kPlaceBody, "application/json");
    REQUIRE(first->status == 201);
    const auto second = fixture.client().Post("/orders", kPlaceBody, "application/json");
    REQUIRE(second->status == 201);
    const auto first_json = nlohmann::json::parse(first->body);
    const auto second_json = nlohmann::json::parse(second->body);
    CHECK(second_json["exchangeOrderId"] == first_json["exchangeOrderId"]);
    CHECK(second_json["replayed"] == true);

    // the venue saw exactly one place
    const auto recorded = fixture.mock().recorded_requests();
    const auto places = std::count_if(
        recorded.begin(), recorded.end(), [](const OkxMockServer::RecordedRequest& a_request) {
            return a_request.method == "POST" && a_request.target == "/api/v5/trade/order";
        });
    CHECK(places == 1);

    // after cancel, the same place still replays the recorded outcome
    REQUIRE(fixture.client().Delete("/orders/gw0001")->status == 200);
    const auto third = fixture.client().Post("/orders", kPlaceBody, "application/json");
    REQUIRE(third->status == 201);
    CHECK(nlohmann::json::parse(third->body)["state"] == "canceled");
    CHECK(nlohmann::json::parse(third->body)["replayed"] == true);
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

    SUBCASE("exchange-specific fields are rejected")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1","instId":"BTC-USDT","tdMode":"cash"})",
            "application/json");
        REQUIRE(res->status == 400);
        const auto error = error_body(res)["error"];
        CHECK(error["code"] == "invalid_request");
        const std::string reason = error["reason"];
        CHECK(reason.find("instId") != std::string::npos);
        CHECK(reason.find("tdMode") != std::string::npos);
    }

    SUBCASE("unknown venue is rejected with the supported list")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","venue":"bybit","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
        CHECK(error_body(res)["error"]["reason"].get<std::string>().find("BINANCE, OKX") !=
              std::string::npos);
    }

    SUBCASE("venue binance routes to the binance connector")
    {
        const auto okx_requests_before = fixture.mock().recorded_requests().size();
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","venue":"BINANCE","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 201);
        const auto body = nlohmann::json::parse(res->body);
        CHECK(body["venue"] == "binance");
        CHECK(body["exchangeOrderId"] == "fake-gw0001");
        REQUIRE(fixture.binance().placed.size() == 1);
        // OKX saw no new traffic (the fixture startup reconcile already ran)
        CHECK(fixture.mock().recorded_requests().size() == okx_requests_before);
    }

    SUBCASE("cancel and amend route through the record's venue")
    {
        REQUIRE(
            fixture.client()
                .Post(
                    "/orders",
                    R"({"clientOrderId":"gw0001","venue":"binance","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})",
                    "application/json")
                ->status == 201);
        REQUIRE(fixture.client().Delete("/orders/gw0001")->status == 200);
        CHECK(fixture.binance().cancels.size() == 1);

        REQUIRE(
            fixture.client()
                .Post(
                    "/orders",
                    R"({"clientOrderId":"gw0002","venue":"binance","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})",
                    "application/json")
                ->status == 201);
        const auto amend = fixture.client().Put("/orders/gw0002", R"({"price":"2","quantity":"2"})",
                                                "application/json");
        REQUIRE(amend->status == 200);
        REQUIRE(fixture.binance().amends.size() == 1);
        CHECK(fixture.binance().amends.front().side == Side::Buy);
        CHECK(fixture.binance().amends.front().time_in_force == "GTC");
    }

    SUBCASE("venue okx is accepted case-insensitively")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","venue":"Okx","symbol":"BTC-USDT","side":"BUY","type":"LIMIT","price":"50000","quantity":"0.001"})",
            "application/json");
        REQUIRE(res->status == 201);
    }

    SUBCASE("missing symbol")
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
            R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"hodl","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("bad price format")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"cheap","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("limit without price")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("market with price")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"market","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("negative quantity")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"-2"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("hyphenated clientOrderId")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw-0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("clientOrderId longer than 32 characters")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("bad timeInForce")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"1","quantity":"1","timeInForce":"GTX"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }

    SUBCASE("timeInForce on a market order")
    {
        const auto res = fixture.client().Post(
            "/orders",
            R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"market","quantity":"1","timeInForce":"IOC"})",
            "application/json");
        REQUIRE(res->status == 400);
        CHECK(error_body(res)["error"]["code"] == "invalid_request");
    }
}

TEST_CASE("a valid timeInForce is forwarded to the venue")
{
    GatewayFixture fixture;

    const auto res = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001","timeInForce":"IOC"})",
        "application/json");
    REQUIRE(res->status == 201);

    const auto recorded = fixture.mock().recorded_requests();
    const auto place_it = std::find_if(
        recorded.begin(), recorded.end(), [](const OkxMockServer::RecordedRequest& a_request) {
            return a_request.method == "POST" && a_request.target == "/api/v5/trade/order";
        });
    REQUIRE(place_it != recorded.end());
    const auto body = nlohmann::json::parse(place_it->body);
    CHECK(body["tdIf"] == "IOC");
}

TEST_CASE("risk rejections return 400 with risk_* codes and replay deterministically")
{
    auto risk = risk_config_from_json(nlohmann::json::parse(
        R"({"instruments":{"BTC-USDT":{"maxQty":"0.01","maxNotional":"100"}}})"));
    REQUIRE(risk.is_ok());
    GatewayFixture fixture{
        GatewayFixture::Options{.with_persistence = false, .risk = risk.value()}};

    const auto too_big = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.005"})",
        "application/json");
    REQUIRE(too_big->status == 400);
    const auto error = error_body(too_big)["error"];
    CHECK(error["code"] == "risk_max_notional");
    const std::string reason = error["reason"];
    CHECK(reason.find("5000") != std::string::npos);
    CHECK(error["clientOrderId"] == "gw0001");
    // the rejected order never reached the venue (reconcile's read-only
    // orders-pending listing may still appear in the log)
    const auto recorded = fixture.mock().recorded_requests();
    const auto writes = std::count_if(
        recorded.begin(), recorded.end(),
        [](const OkxMockServer::RecordedRequest& a_request) { return a_request.method == "POST"; });
    CHECK(writes == 0);

    // retry replays the same rejection
    const auto retry = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.005"})",
        "application/json");
    REQUIRE(retry->status == 400);
    CHECK(error_body(retry)["error"]["code"] == "risk_max_notional");

    // a compliant order still passes
    const auto fine = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0002","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"})",
        "application/json");
    REQUIRE(fine->status == 201);
}

TEST_CASE("venue errors map to structured responses")
{
    GatewayFixture fixture;

    SUBCASE("unknown order on GET becomes 404 not_found (registry miss)")
    {
        const auto res = fixture.client().Get("/orders/ghost");
        REQUIRE(res->status == 404);
        const auto error = error_body(res)["error"];
        CHECK(error["code"] == "not_found");
        CHECK(error["clientOrderId"] == "ghost");
    }

    SUBCASE("unknown order on DELETE becomes 404 not_found")
    {
        const auto res = fixture.client().Delete("/orders/ghost");
        REQUIRE(res->status == 404);
        CHECK(error_body(res)["error"]["code"] == "not_found");
    }

    SUBCASE("venue rejection of a place is 409 and replays deterministically")
    {
        const auto body =
            R"({"clientOrderId":"gw0001","symbol":"DOGE-USDT","side":"buy","type":"limit","price":"1","quantity":"1"})";
        const auto first = fixture.client().Post("/orders", body, "application/json");
        REQUIRE(first->status == 409);
        CHECK(error_body(first)["error"]["code"] == "venue_rejected");
        const auto retry = fixture.client().Post("/orders", body, "application/json");
        REQUIRE(retry->status == 409);
        CHECK(error_body(retry)["error"]["code"] == "venue_rejected");
    }
}

TEST_CASE("a dropped venue response is retried transparently behind the REST API")
{
    GatewayFixture fixture;
    fixture.mock().drop_next_request();

    const auto res = fixture.client().Post("/orders", kPlaceBody, "application/json");
    REQUIRE(res->status == 201);
    CHECK(nlohmann::json::parse(res->body)["exchangeOrderId"] == "mock-1");

    const auto get = fixture.client().Get("/orders/gw0001");
    REQUIRE(get->status == 200);
    CHECK(nlohmann::json::parse(get->body)["state"] == "live");
}

TEST_CASE("venue connectivity problems become 502 venue_unavailable")
{
    const auto dead_port = gateway::testing::pick_free_port();
    auto config = base_config();
    config.port = static_cast<int>(dead_port);
    config.ws.enabled = false;
    OkxConnector connector{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};
    gateway::OrderManagementSystem oms{{{"okx", &connector}}, nullptr, RiskConfig{}};

    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, oms);
    const auto port = gateway::testing::pick_free_port();
    auto future = app.port(port).concurrency(1).loglevel(crow::LogLevel::Warning).run_async();
    app.wait_for_server_start();
    httplib::Client client{"127.0.0.1", static_cast<int>(port)};

    const auto res = client.Post("/orders", kPlaceBody, "application/json");
    REQUIRE(res->status == 502);
    CHECK(error_body(res)["error"]["code"] == "venue_unavailable");

    app.stop();
    future.wait();
}

TEST_CASE("malformed venue payloads become 500 internal")
{
    GatewayFixture fixture;
    fixture.mock().set_next_raw_response(200, "[not json");

    const auto res = fixture.client().Post("/orders", kPlaceBody, "application/json");
    REQUIRE(res->status == 500);
    CHECK(error_body(res)["error"]["code"] == "internal");
}

TEST_CASE("handler exceptions never cross the boundary")
{
    GatewayFixture fixture{GatewayFixture::Options{}, [](crow::SimpleApp& a_app, OkxConnector&,
                                                         OkxMockServer&, OkxMockWsServer&) {
                               CROW_ROUTE(a_app, "/boom")
                               ([]() -> crow::response { throw std::runtime_error("kaboom"); });
                           }};

    const auto res = fixture.client().Get("/boom");
    REQUIRE(res->status == 500);
    CHECK(error_body(res)["error"]["code"] == "internal");
}

TEST_CASE("health endpoint answers ok with registry stats")
{
    GatewayFixture fixture;
    REQUIRE(fixture.client().Post("/orders", kPlaceBody, "application/json")->status == 201);

    const auto res = fixture.client().Get("/health");
    REQUIRE(res->status == 200);
    const auto body = nlohmann::json::parse(res->body);
    CHECK(body["status"] == "ok");
    CHECK(body["knownOrders"] == 1);
}

TEST_CASE("market orders reach the venue without a price field")
{
    GatewayFixture fixture;

    const auto res = fixture.client().Post(
        "/orders",
        R"({"clientOrderId":"gw0001","symbol":"BTC-USDT","side":"sell","type":"market","quantity":"0.001"})",
        "application/json");
    REQUIRE(res->status == 201);

    const auto recorded = fixture.mock().recorded_requests();
    const auto place_it = std::find_if(
        recorded.begin(), recorded.end(), [](const OkxMockServer::RecordedRequest& a_request) {
            return a_request.method == "POST" && a_request.target == "/api/v5/trade/order";
        });
    REQUIRE(place_it != recorded.end());
    const auto body = nlohmann::json::parse(place_it->body);
    CHECK(body["ordType"] == "market");
    CHECK(body["side"] == "sell");
    CHECK_FALSE(body.contains("px"));
}

TEST_CASE("venue-live orders unknown to the gateway are adopted at startup")
{
    // a pre-start hook places an order directly at the venue, bypassing
    // the gateway, then partially fills it
    GatewayFixture fixture{GatewayFixture::Options{}, [](crow::SimpleApp&, OkxConnector&,
                                                         OkxMockServer& a_rest, OkxMockWsServer&) {
                               auto config = base_config();
                               config.port = static_cast<int>(a_rest.port());
                               const OkxRestClient direct{
                                   config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};
                               REQUIRE(direct
                                           .place_order(OkxPlaceRequest{.cl_ord_id = "external1",
                                                                        .inst_id = "BTC-USDT",
                                                                        .side = "buy",
                                                                        .ord_type = "limit",
                                                                        .px = "50000",
                                                                        .sz = "0.002",
                                                                        .td_if = ""})
                                           .is_ok());
                               a_rest.apply_fill("external1", "0.0005", "49999.5");
                           }};

    // startup reconciliation adopted it into the registry
    const auto get = fixture.client().Get("/orders/external1");
    REQUIRE(get->status == 200);
    const auto snapshot = nlohmann::json::parse(get->body);
    CHECK(snapshot["state"] == "partially_filled");
    CHECK(snapshot["filledQuantity"] == "0.0005");
    CHECK(snapshot["averageFillPrice"] == "49999.5");
    CHECK(snapshot["price"] == "50000");
    CHECK(snapshot["quantity"] == "0.002");

    // and the gateway can cancel it like any other order
    const auto cancel = fixture.client().Delete("/orders/external1");
    REQUIRE(cancel->status == 200);
    CHECK(nlohmann::json::parse(cancel->body)["state"] == "canceled");
}

TEST_CASE("restart drill: a new gateway instance recovers from the log + venue")
{
    std::optional<std::filesystem::path> log_path;
    {
        GatewayFixture first{GatewayFixture::Options{.with_persistence = true}};
        log_path = first.log_path();
        REQUIRE(first.client().Post("/orders", kPlaceBody, "application/json")->status == 201);
        first.push_update("gw0001", "partially_filled", "0.0004", "49999.5");
        REQUIRE(first.wait_for_state("gw0001", "partially_filled"));
    }

    // the venue keeps its state across the "restart" (new mock bound to
    // the same story: the order lives there, partially filled)
    OkxMockServer venue{base_config()};
    venue.start();
    {
        auto config = base_config();
        config.port = static_cast<int>(venue.port());
        const OkxRestClient direct{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};
        REQUIRE(direct
                    .place_order(OkxPlaceRequest{.cl_ord_id = "gw0001",
                                                 .inst_id = "BTC-USDT",
                                                 .side = "buy",
                                                 .ord_type = "limit",
                                                 .px = "50000",
                                                 .sz = "0.001",
                                                 .td_if = ""})
                    .is_ok());
        venue.apply_fill("gw0001", "0.0004", "49999.5");
    }

    OkxMockWsServer ws{base_config()};
    ws.start();
    auto config = base_config();
    config.port = static_cast<int>(venue.port());
    config.ws = okx_ws_config(ws);
    OkxConnector connector{config, [] { return std::string("2026-08-20T10:00:00.000Z"); }};
    gateway::EventLog log{*log_path};
    gateway::OrderManagementSystem oms{{{"okx", &connector}}, &log, RiskConfig{}};
    REQUIRE(oms.load_from_log().is_ok());
    connector.set_execution_report_handler(
        [&oms](const ExecutionReport& a_report) { oms.on_execution_report(a_report); });

    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, oms);
    connector.start();
    const auto reconcile = oms.reconcile();
    CHECK(reconcile.absent_rejected == 0);
    CHECK(reconcile.unresolved == 0);

    const auto port = gateway::testing::pick_free_port();
    auto future = app.port(port).concurrency(1).loglevel(crow::LogLevel::Warning).run_async();
    app.wait_for_server_start();
    httplib::Client client{"127.0.0.1", static_cast<int>(port)};

    const auto get = client.Get("/orders/gw0001");
    REQUIRE(get->status == 200);
    const auto snapshot = nlohmann::json::parse(get->body);
    CHECK(snapshot["state"] == "partially_filled");
    CHECK(snapshot["filledQuantity"] == "0.0004");
    CHECK(snapshot["averageFillPrice"] == "49999.5");

    // idempotency survived: a place replay does not hit the venue
    const auto replayed = client.Post("/orders", kPlaceBody, "application/json");
    REQUIRE(replayed->status == 201);
    CHECK(nlohmann::json::parse(replayed->body)["replayed"] == true);

    app.stop();
    future.wait();
    connector.stop();
}
