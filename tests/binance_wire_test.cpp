#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "exchange/binance/binance_wire.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace gateway::exchange::binance;
using gateway::OrderState;
using gateway::OrderType;
using gateway::Side;

auto place_limit() -> BinancePlaceRequest
{
    return BinancePlaceRequest{.client_order_id = "gw0001",
                               .symbol = "BTCUSDT",
                               .side = Side::Sell,
                               .type = OrderType::Limit,
                               .price = "23416.1",
                               .quantity = "0.00847",
                               .time_in_force = "GTC"};
}

} // namespace

TEST_CASE("build_place_params builds a signed-ready LIMIT payload")
{
    const auto params = build_place_params(place_limit());
    REQUIRE(params.is_ok());
    const auto& value = params.value();
    CHECK(value["symbol"] == "BTCUSDT");
    CHECK(value["side"] == "SELL");
    CHECK(value["type"] == "LIMIT");
    CHECK(value["timeInForce"] == "GTC");
    CHECK(value["price"] == "23416.1");
    CHECK(value["quantity"] == "0.00847");
    CHECK(value["newClientOrderId"] == "gw0001");
    CHECK(value["newOrderRespType"] == "RESULT");
    CHECK_FALSE(value.contains("apiKey")); // signing is centralized
}

TEST_CASE("build_place_params omits price and timeInForce for MARKET")
{
    auto request = place_limit();
    request.type = OrderType::Market;
    request.price.clear();
    request.time_in_force.clear();
    const auto params = build_place_params(request);
    REQUIRE(params.is_ok());
    CHECK(params.value()["type"] == "MARKET");
    CHECK_FALSE(params.value().contains("price"));
    CHECK_FALSE(params.value().contains("timeInForce"));
}

TEST_CASE("build_place_params rejects malformed requests")
{
    SUBCASE("missing quantity")
    {
        auto request = place_limit();
        request.quantity.clear();
        CHECK_FALSE(build_place_params(request).is_ok());
    }
    SUBCASE("LIMIT without price")
    {
        auto request = place_limit();
        request.price.clear();
        CHECK_FALSE(build_place_params(request).is_ok());
    }
    SUBCASE("LIMIT without timeInForce")
    {
        auto request = place_limit();
        request.time_in_force.clear();
        CHECK_FALSE(build_place_params(request).is_ok());
    }
    SUBCASE("missing clientOrderId")
    {
        auto request = place_limit();
        request.client_order_id.clear();
        CHECK_FALSE(build_place_params(request).is_ok());
    }
}

TEST_CASE("build_cancel_params targets origClientOrderId")
{
    const auto params =
        build_cancel_params(BinanceCancelRequest{.client_order_id = "gw0002", .symbol = "BTCUSDT"});
    REQUIRE(params.is_ok());
    CHECK(params.value()["symbol"] == "BTCUSDT");
    CHECK(params.value()["origClientOrderId"] == "gw0002");
    CHECK(build_cancel_params(BinanceCancelRequest{.client_order_id = "", .symbol = "BTCUSDT"})
              .is_ok() == false);
}

TEST_CASE("build_cancel_replace_params keeps the clientOrderId and uses STOP_ON_FAILURE")
{
    const BinanceAmendRequest amend{.client_order_id = "gw0003",
                                    .symbol = "BTCUSDT",
                                    .side = Side::Buy,
                                    .type = OrderType::Limit,
                                    .price = "31000",
                                    .quantity = "0.2",
                                    .time_in_force = "GTC"};
    const auto params = build_cancel_replace_params(amend);
    REQUIRE(params.is_ok());
    CHECK(params.value()["cancelReplaceMode"] == "STOP_ON_FAILURE");
    CHECK(params.value()["cancelOrigClientOrderId"] == "gw0003");
    CHECK(params.value()["newClientOrderId"] == "gw0003");
    CHECK(params.value()["price"] == "31000");
    CHECK(params.value()["quantity"] == "0.2");
    CHECK(params.value()["side"] == "BUY");
}

TEST_CASE("build_order_status_params queries by origClientOrderId")
{
    const auto params = build_order_status_params(
        BinanceOrderQuery{.client_order_id = "gw0004", .symbol = "ETHBTC"});
    CHECK(params["symbol"] == "ETHBTC");
    CHECK(params["origClientOrderId"] == "gw0004");
    CHECK(build_open_orders_params().is_object());
    CHECK(build_open_orders_params().empty());
}

TEST_CASE("parse_order_ack reads the documented RESULT payload")
{
    const nlohmann::json result = nlohmann::json::parse(R"({
        "symbol": "BTCUSDT", "orderId": 12569099453, "orderListId": -1,
        "clientOrderId": "gw0001", "transactTime": 1660801715639,
        "price": "23416.10000000", "origQty": "0.00847000",
        "executedQty": "0.00000000", "status": "NEW",
        "timeInForce": "GTC", "type": "LIMIT", "side": "SELL"})");
    const auto ack = parse_order_ack(result);
    CHECK(ack.order_id == "12569099453");
    CHECK(ack.client_order_id == "gw0001");
    CHECK(ack.status == "NEW");
    CHECK(ack.executed_qty == "0.00000000");
}

TEST_CASE("parse_order_ack tolerates a pure ACK payload")
{
    const nlohmann::json result = nlohmann::json::parse(
        R"({"symbol":"BTCUSDT","orderId":7,"orderListId":-1,"clientOrderId":"gw0002","transactTime":1})");
    const auto ack = parse_order_ack(result);
    CHECK(ack.order_id == "7");
    CHECK(ack.status.empty());
}

TEST_CASE("parse_order_info reads status payloads and rejects non-objects")
{
    const nlohmann::json result = nlohmann::json::parse(R"({
        "symbol": "BTCUSDT", "orderId": 42, "clientOrderId": "gw0005",
        "price": "31000.00000000", "origQty": "0.20000000",
        "executedQty": "0.10000000", "cummulativeQuoteQty": "3100.00000000",
        "status": "PARTIALLY_FILLED", "timeInForce": "GTC",
        "type": "LIMIT", "side": "BUY"})");
    const auto info = parse_order_info(result);
    REQUIRE(info.is_ok());
    CHECK(info.value().order_id == "42");
    CHECK(info.value().symbol == "BTCUSDT");
    CHECK(info.value().status == "PARTIALLY_FILLED");
    CHECK(info.value().orig_qty == "0.20000000");
    CHECK(info.value().executed_qty == "0.10000000");
    CHECK(info.value().cummulative_quote_qty == "3100.00000000");

    CHECK(parse_order_info(nlohmann::json::array()).is_ok() == false);
}

TEST_CASE("parse_replace_result reads both cancelReplace legs")
{
    const nlohmann::json result = nlohmann::json::parse(R"({
        "cancelResult": "SUCCESS", "newOrderResult": "SUCCESS",
        "cancelResponse": {
            "symbol": "BTCUSDT", "origClientOrderId": "gw0006", "orderId": 10,
            "status": "CANCELED", "executedQty": "0.00001000"
        },
        "newOrderResponse": {
            "symbol": "BTCUSDT", "clientOrderId": "gw0006", "orderId": 11,
            "status": "NEW", "price": "32000", "origQty": "0.10000000"
        }})");
    const auto replace = parse_replace_result(result);
    REQUIRE(replace.is_ok());
    CHECK(replace.value().cancel_result == "SUCCESS");
    CHECK(replace.value().new_order_result == "SUCCESS");
    CHECK(replace.value().canceled.order_id == "10");
    CHECK(replace.value().canceled.status == "CANCELED");
    CHECK(replace.value().replacement.order_id == "11");
    CHECK(replace.value().replacement.status == "NEW");

    SUBCASE("missing legs are a protocol error")
    {
        const nlohmann::json partial{{"cancelResult", "SUCCESS"}};
        CHECK(parse_replace_result(partial).is_ok() == false);
    }
}

TEST_CASE("map_binance_state covers the documented statuses")
{
    CHECK(map_binance_state("NEW") == OrderState::Live);
    CHECK(map_binance_state("PENDING_NEW") == OrderState::Live);
    CHECK(map_binance_state("PARTIALLY_FILLED") == OrderState::PartiallyFilled);
    CHECK(map_binance_state("FILLED") == OrderState::Filled);
    CHECK(map_binance_state("CANCELED") == OrderState::Canceled);
    CHECK(map_binance_state("PENDING_CANCEL") == OrderState::Canceled);
    CHECK(map_binance_state("EXPIRED") == OrderState::Canceled);
    CHECK(map_binance_state("EXPIRED_IN_MATCH") == OrderState::Canceled);
    CHECK(map_binance_state("REJECTED") == OrderState::Rejected);
    CHECK(map_binance_state("").has_value() == false);
    CHECK(map_binance_state("SOMETHING_NEW").has_value() == false);
    CHECK(map_binance_state("partially_filled").has_value() == false); // case-sensitive
}

TEST_CASE("map_binance_side and map_binance_type")
{
    CHECK(map_binance_side("BUY") == Side::Buy);
    CHECK(map_binance_side("SELL") == Side::Sell);
    CHECK(map_binance_side("buy").has_value() == false);
    CHECK(map_binance_type("LIMIT") == OrderType::Limit);
    CHECK(map_binance_type("MARKET") == OrderType::Market);
    CHECK(map_binance_type("LIMIT_MAKER").has_value() == false);
}

TEST_CASE("SymbolTranslator converts gateway and wire symbols both ways")
{
    SymbolTranslator symbols;
    CHECK(symbols.to_wire("BTC-USDT") == "BTCUSDT");
    CHECK(symbols.to_wire("ETH-BTC") == "ETHBTC");

    SUBCASE("memoized reverse translation round-trips")
    {
        CHECK(symbols.to_gateway("BTCUSDT") == "BTC-USDT");
        CHECK(symbols.to_gateway("ETHBTC") == "ETH-BTC");
    }
    SUBCASE("unseen wire symbols split at the longest quote suffix")
    {
        CHECK(symbols.to_gateway("SOLUSDT") == "SOL-USDT");
        CHECK(symbols.to_gateway("ETHBTC") == "ETH-BTC");
        CHECK(symbols.to_gateway("BNBFDUSD") == "BNB-FDUSD");
        CHECK(symbols.to_gateway("USDCUSDT") == "USDC-USDT");
    }
    SUBCASE("a wire symbol with no known suffix passes through unchanged")
    {
        CHECK(symbols.to_gateway("XYZABCQ") == "XYZABCQ");
    }
    SUBCASE("memo takes precedence over the heuristic")
    {
        // The gateway placed BTC-USDT; a manual BTCUSDT ambiguity cannot
        // arise here, but ETHBTC must never re-split once memoized.
        (void)symbols.to_wire("ETH-BTC");
        CHECK(symbols.to_gateway("ETHBTC") == "ETH-BTC");
    }
}

TEST_CASE("ticker.price params and result parsing")
{
    const auto params = build_ticker_price_params("BTCUSDT");
    CHECK(params == nlohmann::json{{"symbol", "BTCUSDT"}});

    const nlohmann::json result =
        nlohmann::json{{"symbol", "BTCUSDT"}, {"price", "61000.01000000"}};
    const auto price = parse_ticker_price(result);
    REQUIRE(price.is_ok());
    CHECK(price.value() == "61000.01000000");

    SUBCASE("missing price field is a protocol error")
    {
        CHECK_FALSE(parse_ticker_price(nlohmann::json{{"symbol", "BTCUSDT"}}).is_ok());
    }
    SUBCASE("non-object payload is a protocol error")
    {
        CHECK_FALSE(parse_ticker_price(nlohmann::json::array()).is_ok());
    }
}

TEST_CASE("SymbolTranslator is safe under concurrent REST and feed traffic")
{
    // The connector calls from crow workers (to_wire) and the feed notifier
    // thread (to_gateway) simultaneously; the memo maps must not corrupt.
    SymbolTranslator symbols;
    constexpr int kThreadsPerDirection = 4;
    constexpr int kIterations = 500;

    const auto hammer_wire = [&symbols](int a_offset) {
        for (int i = 0; i < kIterations; ++i) {
            const std::string base = std::string("SYM") + std::to_string(a_offset);
            (void)symbols.to_wire(base + "-USDT");
        }
    };
    const auto hammer_gateway = [&symbols](int a_offset) {
        // Memo (from a racing to_wire) and the quote-suffix heuristic both
        // split identically, so the result is stable regardless of ordering.
        for (int i = 0; i < kIterations; ++i) {
            const std::string wire = std::string("SYM") + std::to_string(a_offset) + "USDT";
            const auto expected = std::string("SYM") + std::to_string(a_offset) + "-USDT";
            if (symbols.to_gateway(wire) != expected) {
                return false;
            }
        }
        return true;
    };

    std::vector<std::thread> threads;
    std::atomic<bool> consistent{true};
    for (int t = 0; t < kThreadsPerDirection; ++t) {
        threads.emplace_back([&, t] { hammer_wire(t); });
        threads.emplace_back([&, t] {
            if (!hammer_gateway(t)) {
                consistent = false;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(consistent.load());
    // After the hammer, every direction must be stable.
    CHECK(symbols.to_wire("SYM0-USDT") == "SYM0USDT");
    CHECK(symbols.to_gateway("SYM1USDT") == "SYM1-USDT");
}
