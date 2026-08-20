#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/okx_mock_server.hpp"
#include "util/free_port.hpp"

#include "exchange/okx/okx_rest_client.hpp"

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
                     .demo_trading = true};
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
    return OkxPlaceRequest{.cl_ord_id = "gw-0001",
                           .inst_id = "BTC-USDT",
                           .side = "buy",
                           .ord_type = "limit",
                           .px = "50000",
                           .sz = "0.001"};
}

TEST_CASE("place and fetch a live order (normal path)")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    const auto ack = client.place_order(limit_buy());
    REQUIRE(ack.is_ok());
    CHECK(ack.value().ord_id == "mock-1");
    CHECK(ack.value().cl_ord_id == "gw-0001");
    CHECK(ack.value().s_code == "0");

    const auto info = client.get_order(OkxQuery{"BTC-USDT", "gw-0001"});
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
    CHECK(recorded.back().target.find("/api/v5/trade/order-info?instId=BTC-USDT&clOrdId=gw-0001") ==
          0);
}

TEST_CASE("full auto-fill on place")
{
    OkxMockServer server(base_config());
    server.set_fill_mode(OkxMockServer::FillMode::Full);
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    REQUIRE(client.place_order(limit_buy()).is_ok());

    const auto info = client.get_order(OkxQuery{"BTC-USDT", "gw-0001"});
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

    server.apply_fill("gw-0001", "0.4", "100");
    const auto partial = client.get_order(OkxQuery{"BTC-USDT", "gw-0001"});
    REQUIRE(partial.is_ok());
    REQUIRE(partial.value().has_value());
    CHECK(partial.value()->state == "partially_filled");
    CHECK(partial.value()->acc_fill_sz == "0.4");
    CHECK(partial.value()->avg_px == "100");

    server.apply_fill("gw-0001", "0.6", "200");
    const auto done = client.get_order(OkxQuery{"BTC-USDT", "gw-0001"});
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
    const auto cancel = client.cancel_order(OkxCxlRequest{"BTC-USDT", "gw-0001"});
    REQUIRE(cancel.is_ok());
    CHECK(cancel.value().s_code == "0");

    const auto info = client.get_order(OkxQuery{"BTC-USDT", "gw-0001"});
    REQUIRE(info.is_ok());
    REQUIRE(info.value().has_value());
    CHECK(info.value()->state == "canceled");
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
        const auto result = client.cancel_order(OkxCxlRequest{"BTC-USDT", "gw-0001"});
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

TEST_CASE("order-info returns nullopt for unknown orders")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    const auto result = client.get_order(OkxQuery{"BTC-USDT", "ghost"});
    REQUIRE(result.is_ok());
    CHECK_FALSE(result.value().has_value());
}

TEST_CASE("amend updates a live order")
{
    OkxMockServer server(base_config());
    server.start();
    const auto client = fixed_clock_client(config_for(server));

    REQUIRE(client.place_order(limit_buy()).is_ok());
    const auto amend = client.amend_order(OkxAmendRequest{
        .inst_id = "BTC-USDT", .cl_ord_id = "gw-0001", .new_px = "51000", .new_sz = std::nullopt});
    REQUIRE(amend.is_ok());
    CHECK(amend.value().s_code == "0");

    const auto info = client.get_order(OkxQuery{"BTC-USDT", "gw-0001"});
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
        client.amend_order(OkxAmendRequest{"BTC-USDT", "gw-0001", std::nullopt, std::nullopt});
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
            client.amend_order(OkxAmendRequest{"BTC-USDT", "gw-0001", "51000", std::nullopt});
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

} // namespace
