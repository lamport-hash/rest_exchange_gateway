#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "exchange/okx/okx_wire.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace {

using namespace gateway::exchange::okx;

TEST_CASE("to_json builds the place-order body with all fields")
{
    const OkxPlaceRequest request{.cl_ord_id = "gw-0001",
                                  .inst_id = "BTC-USDT",
                                  .side = "buy",
                                  .ord_type = "limit",
                                  .px = "50000",
                                  .sz = "0.001",
                                  .td_if = ""};
    const auto json = to_json(request);
    CHECK(json.at("clOrdId") == "gw-0001");
    CHECK(json.at("instId") == "BTC-USDT");
    CHECK(json.at("tdMode") == "cash");
    CHECK(json.at("side") == "buy");
    CHECK(json.at("ordType") == "limit");
    CHECK(json.at("px") == "50000");
    CHECK(json.at("sz") == "0.001");
}

TEST_CASE("to_json omits px for market orders")
{
    const OkxPlaceRequest market{.cl_ord_id = "gw-m",
                                 .inst_id = "BTC-USDT",
                                 .side = "buy",
                                 .ord_type = "market",
                                 .px = "",
                                 .sz = "0.001",
                                 .td_if = ""};
    const auto json = to_json(market);
    CHECK_FALSE(json.contains("px"));
    CHECK(json.at("sz") == "0.001");
    CHECK(json.at("ordType") == "market");
}

TEST_CASE("to_json carries tdIf only when set")
{
    OkxPlaceRequest request{.cl_ord_id = "gw-ioc",
                            .inst_id = "BTC-USDT",
                            .side = "buy",
                            .ord_type = "limit",
                            .px = "50000",
                            .sz = "0.001",
                            .td_if = "IOC"};
    auto json = to_json(request);
    CHECK(json.at("tdIf") == "IOC");

    request.td_if.clear();
    json = to_json(request);
    CHECK_FALSE(json.contains("tdIf"));
}

TEST_CASE("to_json builds the cancel-order body")
{
    const OkxCxlRequest request{.inst_id = "BTC-USDT", .cl_ord_id = "gw-0001"};
    const auto json = to_json(request);
    REQUIRE(json.size() == 2);
    CHECK(json.at("instId") == "BTC-USDT");
    CHECK(json.at("clOrdId") == "gw-0001");
}

TEST_CASE("to_json amend body includes only provided fields")
{
    const OkxAmendRequest px_only{
        .inst_id = "BTC-USDT", .cl_ord_id = "gw-0001", .new_px = "51000", .new_sz = std::nullopt};
    const auto px_json = to_json(px_only);
    REQUIRE(px_json.is_ok());
    CHECK(px_json.value().at("newPx") == "51000");
    CHECK_FALSE(px_json.value().contains("newSz"));

    const OkxAmendRequest sz_only{
        .inst_id = "BTC-USDT", .cl_ord_id = "gw-0001", .new_px = std::nullopt, .new_sz = "0.002"};
    const auto sz_json = to_json(sz_only);
    REQUIRE(sz_json.is_ok());
    CHECK(sz_json.value().at("newSz") == "0.002");
    CHECK_FALSE(sz_json.value().contains("newPx"));
}

TEST_CASE("to_json rejects an amend request that changes nothing")
{
    const OkxAmendRequest empty{.inst_id = "BTC-USDT",
                                .cl_ord_id = "gw-0001",
                                .new_px = std::nullopt,
                                .new_sz = std::nullopt};
    const auto result = to_json(empty);
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("to_query encodes both parameters")
{
    const OkxQuery query{.inst_id = "BTC-USDT", .cl_ord_id = "gw-0001"};
    CHECK(to_query(query) == "instId=BTC-USDT&clOrdId=gw-0001");
}

TEST_CASE("to_json builds the demo-balance body with one adjustment")
{
    const OkxDemoBalanceRequest request{.type = "increase",
                                        .adjustments = {OkxDemoBalanceAdjustment{"USDT", "5000"}}};
    const auto json = to_json(request);
    REQUIRE(json.size() == 2);
    CHECK(json.at("type") == "increase");
    REQUIRE(json.at("adjustments").is_array());
    REQUIRE(json.at("adjustments").size() == 1);
    CHECK(json.at("adjustments").at(0).at("ccy") == "USDT");
    CHECK(json.at("adjustments").at(0).at("amt") == "5000");
}

TEST_CASE("to_json keeps multiple adjustments in order")
{
    const OkxDemoBalanceRequest request{.type = "reduce",
                                        .adjustments = {OkxDemoBalanceAdjustment{"USDT", "100.5"},
                                                        OkxDemoBalanceAdjustment{"BTC", "0.001"}}};
    const auto json = to_json(request);
    CHECK(json.at("type") == "reduce");
    REQUIRE(json.at("adjustments").size() == 2);
    CHECK(json.at("adjustments").at(0).at("ccy") == "USDT");
    CHECK(json.at("adjustments").at(0).at("amt") == "100.5");
    CHECK(json.at("adjustments").at(1).at("ccy") == "BTC");
    CHECK(json.at("adjustments").at(1).at("amt") == "0.001");
}

TEST_CASE("to_json demo-balance body carries decimals verbatim")
{
    // no float round-trip: trailing zeros and precision are preserved
    const OkxDemoBalanceRequest request{
        .type = "increase", .adjustments = {OkxDemoBalanceAdjustment{"USDT", "0.100000000"}}};
    CHECK(to_json(request).at("adjustments").at(0).at("amt") == "0.100000000");
}

TEST_CASE("to_query percent-encodes reserved characters")
{
    const OkxQuery query{.inst_id = "BTC USDT/1", .cl_ord_id = "a+b&c=d"};
    CHECK(to_query(query) == "instId=BTC%20USDT%2F1&clOrdId=a%2Bb%26c%3Dd");
}

TEST_CASE("parse_order_ack reads a documented OKX ack")
{
    const auto item = nlohmann::json::parse(R"({
        "ordId": "647348870051397632", "clOrdId": "gw-0001", "sCode": "0", "sMsg": "Order placed"
    })");
    const auto ack = parse_order_ack(item);
    CHECK(ack.ord_id == "647348870051397632");
    CHECK(ack.cl_ord_id == "gw-0001");
    CHECK(ack.s_code == "0");
    CHECK(ack.s_msg == "Order placed");
}

TEST_CASE("parse_order_ack tolerates missing fields")
{
    const auto ack = parse_order_ack(nlohmann::json::parse(R"({"ordId":"1"})"));
    CHECK(ack.ord_id == "1");
    CHECK(ack.cl_ord_id.empty());
    CHECK(ack.s_code.empty());
    CHECK(ack.s_msg.empty());
}

TEST_CASE("parse_order_info reads a live order and a partially filled order")
{
    const auto live = parse_order_info(nlohmann::json::parse(R"({
        "ordId": "1", "clOrdId": "gw-0001", "state": "live", "side": "buy", "ordType": "limit",
        "px": "50000", "sz": "0.001", "avgPx": "", "accFillSz": "0"
    })"));
    CHECK(live.state == "live");
    CHECK(live.avg_px.empty());

    const auto partial = parse_order_info(nlohmann::json::parse(R"({
        "ordId": "1", "clOrdId": "gw-0001", "state": "partially_filled", "side": "buy",
        "ordType": "limit", "px": "50000", "sz": "0.001", "avgPx": "49999.5", "accFillSz": "0.0005"
    })"));
    CHECK(partial.state == "partially_filled");
    CHECK(partial.avg_px == "49999.5");
    CHECK(partial.acc_fill_sz == "0.0005");
}

TEST_CASE("parse_order_info tolerates null and non-string fields")
{
    const auto info = parse_order_info(
        nlohmann::json::parse(R"({"ordId":"1","state":null,"sz":5,"avgPx":"1.5"})"));
    CHECK(info.state.empty());
    CHECK(info.sz == "5");
    CHECK(info.avg_px == "1.5");
}

TEST_CASE("map_okx_side maps documented values and rejects unknowns")
{
    CHECK(map_okx_side("buy") == gateway::Side::Buy);
    CHECK(map_okx_side("sell") == gateway::Side::Sell);
    CHECK_FALSE(map_okx_side("BUY").has_value());
    CHECK_FALSE(map_okx_side("short").has_value());
    CHECK_FALSE(map_okx_side("").has_value());
}

TEST_CASE("url_encode keeps unreserved characters and encodes everything else")
{
    CHECK(url_encode("AZaz09-_.~") == "AZaz09-_.~");
    CHECK(url_encode("a b/c?d#e") == "a%20b%2Fc%3Fd%23e");
    CHECK(url_encode("") == "");
}

} // namespace
