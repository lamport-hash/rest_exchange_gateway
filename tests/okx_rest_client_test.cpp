#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/okx_mock_server.hpp"
#include "util/free_port.hpp"

#include "exchange/okx/okx_rest_client.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace {

using gateway::testing::OkxMockServer;
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

auto config_for(const OkxMockServer& a_server, bool a_demo = true) -> OkxConfig
{
    auto config = base_config();
    config.port = static_cast<int>(a_server.port());
    config.demo_trading = a_demo;
    return config;
}

auto fixed_clock_client(const OkxConfig& a_config) -> OkxRestClient
{
    return OkxRestClient(a_config, [] { return std::string("2026-08-20T10:00:00.000Z"); });
}

auto limit_buy() -> OkxPlaceRequest
{
    return OkxPlaceRequest{.cl_ord_id = "gw0001",
                           .inst_id = "BTC-USDT",
                           .side = "buy",
                           .ord_type = "limit",
                           .px = "50000",
                           .sz = "0.001",
                           .td_if = ""};
}

TEST_CASE("place and fetch a live order (normal path)")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    const auto ack = client.place_order(limit_buy());
    REQUIRE(ack.is_ok());
    CHECK(ack.value().ord_id == "mock-1");
    CHECK(ack.value().cl_ord_id == "gw0001");
    CHECK(ack.value().s_code == "0");

    const auto info = client.get_order(OkxQuery{"BTC-USDT", "gw0001"});
    REQUIRE(info.is_ok());
    REQUIRE(info.value().has_value());
    CHECK(info.value()->state == "live");
    CHECK(info.value()->acc_fill_sz == "0");
    CHECK(info.value()->avg_px.empty());
    CHECK(info.value()->px == "50000");
    CHECK(info.value()->sz == "0.001");

    const auto recorded = server.recorded_requests();
    REQUIRE(recorded.size() == 2);
    CHECK(recorded.front().method == "POST");
    CHECK(recorded.front().target == "/api/v5/trade/order");
    CHECK(recorded.front().headers.find("OK-ACCESS-KEY")->second == "test-key");
    CHECK(recorded.front().headers.find("OK-ACCESS-TIMESTAMP")->second ==
          "2026-08-20T10:00:00.000Z");
    CHECK(recorded.front().headers.count("x-simulated-trading") == 1);
    CHECK(recorded.back().target.find("/api/v5/trade/order?instId=BTC-USDT&clOrdId=gw0001") == 0);
}

TEST_CASE("place sends exactly one Content-Type header")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    REQUIRE(client.place_order(limit_buy()).is_ok());
    const auto recorded = server.recorded_requests();
    REQUIRE(recorded.size() == 1);
    CHECK(recorded.front().headers.count("Content-Type") == 1);
    CHECK(recorded.front().headers.find("Content-Type")->second == "application/json");
}

TEST_CASE("full auto-fill on place")
{
    OkxMockServer server(base_config());
    server.set_fill_mode(OkxMockServer::FillMode::Full);
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    REQUIRE(client.place_order(limit_buy()).is_ok());

    const auto info = client.get_order(OkxQuery{"BTC-USDT", "gw0001"});
    REQUIRE(info.is_ok());
    REQUIRE(info.value().has_value());
    CHECK(info.value()->state == "filled");
    CHECK(info.value()->acc_fill_sz == "0.001");
    CHECK(info.value()->avg_px == "50000");
}

TEST_CASE("scripted partial fill reports partially_filled with weighted average")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    OkxPlaceRequest request = limit_buy();
    request.sz = "1";
    REQUIRE(client.place_order(request).is_ok());

    server.apply_fill("gw0001", "0.4", "100");
    const auto partial = client.get_order(OkxQuery{"BTC-USDT", "gw0001"});
    REQUIRE(partial.is_ok());
    REQUIRE(partial.value().has_value());
    CHECK(partial.value()->state == "partially_filled");
    CHECK(partial.value()->acc_fill_sz == "0.4");
    CHECK(partial.value()->avg_px == "100");

    server.apply_fill("gw0001", "0.6", "200");
    const auto done = client.get_order(OkxQuery{"BTC-USDT", "gw0001"});
    REQUIRE(done.is_ok());
    REQUIRE(done.value().has_value());
    CHECK(done.value()->state == "filled");
    CHECK(done.value()->acc_fill_sz == "1");
    CHECK(done.value()->avg_px == "160");
}

TEST_CASE("cancel a live order then observe canceled state")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    REQUIRE(client.place_order(limit_buy()).is_ok());
    const auto cancel = client.cancel_order(OkxCxlRequest{"BTC-USDT", "gw0001"});
    REQUIRE(cancel.is_ok());
    CHECK(cancel.value().s_code == "0");

    const auto info = client.get_order(OkxQuery{"BTC-USDT", "gw0001"});
    REQUIRE(info.is_ok());
    REQUIRE(info.value().has_value());
    CHECK(info.value()->state == "canceled");
}

TEST_CASE("orders-pending lists only open orders")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    auto second = limit_buy();
    second.cl_ord_id = "gw0002";
    REQUIRE(client.place_order(limit_buy()).is_ok());
    REQUIRE(client.place_order(second).is_ok());

    auto pending = client.get_orders_pending();
    REQUIRE(pending.is_ok());
    REQUIRE(pending.value().size() == 2);

    // one order fully fills, one is canceled -> pending becomes empty
    server.apply_fill("gw0001", "0.001", "50000");
    REQUIRE(client.cancel_order(OkxCxlRequest{"BTC-USDT", "gw0002"}).is_ok());
    pending = client.get_orders_pending();
    REQUIRE(pending.is_ok());
    CHECK(pending.value().empty());

    // pending items carry the same fields as order-info
    auto third = limit_buy();
    third.cl_ord_id = "gw0003";
    REQUIRE(client.place_order(third).is_ok());
    server.apply_fill("gw0003", "0.0004", "49999.5");
    pending = client.get_orders_pending();
    REQUIRE(pending.is_ok());
    REQUIRE(pending.value().size() == 1);
    CHECK(pending.value().front().cl_ord_id == "gw0003");
    CHECK(pending.value().front().inst_id == "BTC-USDT");
    CHECK(pending.value().front().state == "partially_filled");
    CHECK(pending.value().front().acc_fill_sz == "0.0004");
}

TEST_CASE("venue error codes surface as Result errors")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    SUBCASE("cancel an unknown order")
    {
        const auto result = client.cancel_order(OkxCxlRequest{"BTC-USDT", "nope"});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51016");
    }

    SUBCASE("cancel a filled order")
    {
        server.set_fill_mode(OkxMockServer::FillMode::Full);
        REQUIRE(client.place_order(limit_buy()).is_ok());
        const auto result = client.cancel_order(OkxCxlRequest{"BTC-USDT", "gw0001"});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51017");
    }

    SUBCASE("duplicate active client order id")
    {
        REQUIRE(client.place_order(limit_buy()).is_ok());
        const auto result = client.place_order(limit_buy());
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51000");
        CHECK(result.error().message.find("duplicate") != std::string::npos);
    }

    SUBCASE("non-alphanumeric client order id")
    {
        OkxPlaceRequest request = limit_buy();
        request.cl_ord_id = "gw-0001";
        const auto result = client.place_order(request);
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51000");
    }

    SUBCASE("unknown instrument")
    {
        OkxPlaceRequest request = limit_buy();
        request.inst_id = "DOGE-USDT";
        const auto result = client.place_order(request);
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51001");
    }

    SUBCASE("invalid side")
    {
        OkxPlaceRequest request = limit_buy();
        request.side = "hodl";
        const auto result = client.place_order(request);
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51040");
    }
}

TEST_CASE("order-info surfaces venue error 51603 for unknown orders")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    const auto result = client.get_order(OkxQuery{"BTC-USDT", "ghost"});
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "venue:51603");
    CHECK(result.error().message.find("does not exist") != std::string::npos);
}

TEST_CASE("amend updates a live order")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    REQUIRE(client.place_order(limit_buy()).is_ok());
    const auto amend = client.amend_order(OkxAmendRequest{
        .inst_id = "BTC-USDT", .cl_ord_id = "gw0001", .new_px = "51000", .new_sz = std::nullopt});
    REQUIRE(amend.is_ok());
    CHECK(amend.value().s_code == "0");

    const auto info = client.get_order(OkxQuery{"BTC-USDT", "gw0001"});
    REQUIRE(info.is_ok());
    REQUIRE(info.value().has_value());
    CHECK(info.value()->px == "51000");
}

TEST_CASE("amend with no changes fails client-side without touching the network")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    const auto result =
        client.amend_order(OkxAmendRequest{"BTC-USDT", "gw0001", std::nullopt, std::nullopt});
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
    CHECK(server.recorded_requests().empty());
}

TEST_CASE("amend on unknown or terminal orders is rejected by the venue")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    SUBCASE("unknown order")
    {
        const auto result =
            client.amend_order(OkxAmendRequest{"BTC-USDT", "ghost", "51000", std::nullopt});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51016");
    }

    SUBCASE("filled order")
    {
        server.set_fill_mode(OkxMockServer::FillMode::Full);
        REQUIRE(client.place_order(limit_buy()).is_ok());
        const auto result =
            client.amend_order(OkxAmendRequest{"BTC-USDT", "gw0001", "51000", std::nullopt});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51017");
    }
}

TEST_CASE("a wrong secret is rejected with signature error 50102")
{
    OkxMockServer server(base_config());
    server.start();
    auto config = config_for(server);
    config.secret_key = "wrong-secret";
    const auto client = fixed_clock_client(config);

    const auto result = client.place_order(limit_buy());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "venue:50102");
}

TEST_CASE("demo header is only sent when demo trading is enabled")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server, false));

    REQUIRE(client.place_order(limit_buy()).is_ok());
    const auto recorded = server.recorded_requests();
    REQUIRE(recorded.size() == 1);
    CHECK(recorded.front().headers.count("x-simulated-trading") == 0);
}

TEST_CASE("transport failures produce a transport error")
{
    const auto unused_port = gateway::testing::pick_free_port();
    auto config = base_config();
    config.port = static_cast<int>(unused_port);
    const auto client = fixed_clock_client(config);

    const auto result = client.place_order(limit_buy());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "transport");
}

TEST_CASE("malformed venue responses produce protocol errors")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    SUBCASE("non-JSON body with HTTP 200")
    {
        server.set_next_raw_response(200, "[not json");
        const auto result = client.place_order(limit_buy());
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "protocol");
    }

    SUBCASE("HTTP 500")
    {
        server.set_next_raw_response(500, R"({"oops":1})");
        const auto result = client.place_order(limit_buy());
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
        CHECK(result.error().message.find("500") != std::string::npos);
    }
}

TEST_CASE("get_ticker returns the last price of a public instrument")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    server.set_ticker("BTC-USDT", "61750.5");
    const auto price = client.get_ticker("BTC-USDT");
    REQUIRE(price.is_ok());
    CHECK(price.value() == "61750.5");

    // public market data: the request must not carry auth headers
    const auto recorded = server.recorded_requests().back();
    CHECK(recorded.method == "GET");
    CHECK(recorded.target == "/api/v5/market/ticker?instId=BTC-USDT");
    bool has_auth_header = false;
    for (const auto& [name, value] : recorded.headers) {
        if (name.rfind("OK-ACCESS-", 0) == 0) {
            has_auth_header = true;
        }
    }
    CHECK_FALSE(has_auth_header);
}

TEST_CASE("adjust_demo_balance posts the documented body (normal path)")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
        .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "5000"}}});
    REQUIRE(result.is_ok());
    REQUIRE(result.value().is_array());
    REQUIRE(result.value().size() == 1);
    // documented shape: {remainCnt, totalCnt, details:[{ccy, amt, bal}]}
    CHECK(result.value().at(0).at("remainCnt") == "2");
    CHECK(result.value().at(0).at("totalCnt") == "3");
    CHECK(result.value().at(0).at("details").at(0).at("ccy") == "USDT");
    CHECK(result.value().at(0).at("details").at(0).at("amt") == "5000");
    CHECK(result.value().at(0).at("details").at(0).at("bal") == "5000");
    CHECK(server.demo_balance("USDT") == "5000");

    // signed demo request: path, body and simulated-trading header
    const auto recorded = server.recorded_requests();
    REQUIRE(recorded.size() == 1);
    CHECK(recorded.front().method == "POST");
    CHECK(recorded.front().target == "/api/v5/account/demo-adjust-balance");
    CHECK(recorded.front().headers.count("x-simulated-trading") == 1);
    const auto body = nlohmann::json::parse(recorded.front().body);
    CHECK(body.at("type") == "increase");
    CHECK(body.at("adjustments").at(0).at("ccy") == "USDT");
    CHECK(body.at("adjustments").at(0).at("amt") == "5000");
}

TEST_CASE("adjust_demo_balance applies increases and reduces exactly once")
{
    OkxMockServer server(base_config());
    server.set_demo_balance("USDT", "100");
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    REQUIRE(client
                .adjust_demo_balance(OkxDemoBalanceRequest{
                    .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "50"}}})
                .is_ok());
    CHECK(server.demo_balance("USDT") == "150");
    CHECK(client
              .adjust_demo_balance(OkxDemoBalanceRequest{
                  .type = "reduce", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "20"}}})
              .is_ok());
    CHECK(server.demo_balance("USDT") == "130");
}

TEST_CASE("adjust_demo_balance error paths")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    SUBCASE("reduce below the balance is a venue error (HTTP 400 + envelope)")
    {
        const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
            .type = "reduce", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "5000"}}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:59693");
        CHECK(result.error().message.find("insufficient") != std::string::npos);
        // nothing was applied
        CHECK(server.demo_balance("USDT").empty());
    }

    SUBCASE("exhausted daily increase quota is a venue rate-limit error")
    {
        server.set_demo_increase_quota(0);
        const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
            .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "1"}}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:50011");
        CHECK(server.demo_balance("USDT").empty());
    }

    SUBCASE("over-cap increase is a venue parameter error")
    {
        const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
            .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "5001"}}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51000");
    }

    SUBCASE("unsupported currency is a venue parameter error")
    {
        const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
            .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"DOGE", "1"}}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51000");
    }

    SUBCASE("unknown type is a venue parameter error")
    {
        const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
            .type = "banana", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "1"}}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "venue:51000");
        CHECK(result.error().message.find("Parameter type error") != std::string::npos);
    }

    SUBCASE("malformed venue response is a protocol error")
    {
        server.set_next_raw_response(200, "[not json");
        const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
            .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "1"}}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "protocol");
    }

    SUBCASE("non-200 without a venue envelope stays a transport error")
    {
        server.set_next_raw_response(500, R"({"oops":1})");
        const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
            .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "1"}}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
        CHECK(result.error().message.find("500") != std::string::npos);
    }

    SUBCASE("network failure is a transport error")
    {
        server.drop_next_request();
        const auto result = client.adjust_demo_balance(OkxDemoBalanceRequest{
            .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "1"}}});
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "transport");
    }
}

TEST_CASE("get_ticker error paths")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    SUBCASE("unknown instrument is a venue error")
    {
        const auto price = client.get_ticker("XYZ-USDT");
        REQUIRE_FALSE(price.is_ok());
        CHECK(price.error().code == "venue:51001");
    }
    SUBCASE("malformed payload is a protocol error")
    {
        server.set_next_raw_response(200, "[not json");
        CHECK(client.get_ticker("BTC-USDT").error().code == "protocol");
    }
    SUBCASE("empty data array is a protocol error")
    {
        server.set_next_raw_response(200, R"({"code":"0","msg":"","data":[]})");
        CHECK(client.get_ticker("BTC-USDT").error().code == "protocol");
    }
    SUBCASE("data without a string last field is a protocol error")
    {
        server.set_next_raw_response(200,
                                     R"({"code":"0","msg":"","data":[{"instId":"BTC-USDT"}]})");
        CHECK(client.get_ticker("BTC-USDT").error().code == "protocol");
    }
    SUBCASE("network failure is a transport error")
    {
        server.drop_next_request();
        CHECK(client.get_ticker("BTC-USDT").error().code == "transport");
    }
}

} // namespace
