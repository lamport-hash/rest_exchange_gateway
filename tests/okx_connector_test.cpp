#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "mocks/okx_mock_server.hpp"

#include "exchange/okx/okx_connector.hpp"
#include "gateway/exchange_connector.hpp"

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
                     .demo_trading = true};
}

auto limit_buy() -> OrderRequest
{
    return OrderRequest{.client_order_id = "gw-0001",
                        .instrument_id = "BTC-USDT",
                        .side = Side::Buy,
                        .type = OrderType::Limit,
                        .price = "50000",
                        .quantity = "0.001"};
}

auto make_connector(const OkxMockServer& a_server) -> OkxConnector
{
    auto config = base_config();
    config.port = static_cast<int>(a_server.port());
    return OkxConnector(config, [] { return std::string("2026-08-20T10:00:00.000Z"); });
}

TEST_CASE("place then get through the ExchangeConnector interface")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    const auto placement = connector_interface.place_order(limit_buy());
    REQUIRE(placement.is_ok());
    CHECK(placement.value().client_order_id == "gw-0001");
    CHECK(placement.value().exchange_order_id == "mock-1");

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw-0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Live);
    CHECK(snapshot.value()->price == "50000");
    CHECK(snapshot.value()->quantity == "0.001");
    CHECK(snapshot.value()->filled_quantity == "0");
    CHECK(snapshot.value()->average_fill_price.empty());
}

TEST_CASE("partial fill surfaces as PartiallyFilled snapshot")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    server.apply_fill("gw-0001", "0.0004", "49999.5");

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw-0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::PartiallyFilled);
    CHECK(snapshot.value()->filled_quantity == "0.0004");
    CHECK(snapshot.value()->average_fill_price == "49999.5");
}

TEST_CASE("cancel transitions the snapshot to Canceled")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    const auto cancel = connector_interface.cancel_order(CancelRequest{"gw-0001", "BTC-USDT"});
    REQUIRE(cancel.is_ok());
    CHECK(cancel.value().exchange_order_id == "mock-1");

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw-0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->state == OrderState::Canceled);
}

TEST_CASE("amend updates price through the interface")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    REQUIRE(connector_interface.place_order(limit_buy()).is_ok());
    const auto amend = connector_interface.amend_order(
        AmendRequest{"gw-0001", "BTC-USDT", std::string("51000"), std::nullopt});
    REQUIRE(amend.is_ok());

    const auto snapshot = connector_interface.get_order(OrderQuery{"gw-0001", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    REQUIRE(snapshot.value().has_value());
    CHECK(snapshot.value()->price == "51000");
}

TEST_CASE("side and type map to OKX wire values")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    OrderRequest market_sell = limit_buy();
    market_sell.client_order_id = "gw-0002";
    market_sell.side = Side::Sell;
    market_sell.type = OrderType::Market;
    market_sell.price.clear();
    REQUIRE(connector_interface.place_order(market_sell).is_ok());

    const auto recorded = server.recorded_requests();
    REQUIRE(recorded.size() == 1);
    const auto body = nlohmann::json::parse(recorded.front().body);
    CHECK(body.at("side") == "sell");
    CHECK(body.at("ordType") == "market");
    CHECK_FALSE(body.contains("px"));

    OrderRequest limit = limit_buy();
    limit.client_order_id = "gw-0003";
    limit.side = Side::Sell;
    REQUIRE(connector_interface.place_order(limit).is_ok());
    CHECK(nlohmann::json::parse(server.recorded_requests().back().body).at("side") == "sell");
}

TEST_CASE("get_order returns nullopt for orders the venue never saw")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    const auto snapshot = connector_interface.get_order(OrderQuery{"ghost", "BTC-USDT"});
    REQUIRE(snapshot.is_ok());
    CHECK_FALSE(snapshot.value().has_value());
}

TEST_CASE("venue errors pass through the interface unchanged")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    const auto result = connector_interface.cancel_order(CancelRequest{"nope", "BTC-USDT"});
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "venue:51016");
}

TEST_CASE("amend with no changes is rejected before the network")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);
    ExchangeConnector& connector_interface = connector;

    const auto result = connector_interface.amend_order(
        AmendRequest{"gw-0001", "BTC-USDT", std::nullopt, std::nullopt});
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
    CHECK(server.recorded_requests().empty());
}

TEST_CASE("execution report handler can be installed and replaced")
{
    OkxMockServer server(base_config());
    server.start();
    OkxConnector connector = make_connector(server);

    int calls = 0;
    connector.set_execution_report_handler([&calls](const ExecutionReport&) { ++calls; });
    connector.set_execution_report_handler([](const ExecutionReport&) {});
    CHECK(calls == 0);
}

TEST_CASE("map_okx_state covers documented states and rejects unknowns")
{
    CHECK(map_okx_state("live") == OrderState::Live);
    CHECK(map_okx_state("partially_filled") == OrderState::PartiallyFilled);
    CHECK(map_okx_state("filled") == OrderState::Filled);
    CHECK(map_okx_state("canceled") == OrderState::Canceled);
    CHECK_FALSE(map_okx_state("marginal").has_value());
    CHECK_FALSE(map_okx_state("").has_value());
}

TEST_CASE("to_string renders every OrderState")
{
    CHECK(to_string(OrderState::Live) == "live");
    CHECK(to_string(OrderState::PartiallyFilled) == "partially_filled");
    CHECK(to_string(OrderState::Filled) == "filled");
    CHECK(to_string(OrderState::Canceled) == "canceled");
    CHECK(to_string(OrderState::Rejected) == "rejected");
}

} // namespace
