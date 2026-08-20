#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/risk.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace {

using namespace gateway;

auto unlimited() -> std::optional<InstrumentRiskLimits>
{
    return std::nullopt;
}

auto limits(std::string a_qty = "", std::string a_notional = "",
            std::string a_position = "") -> std::optional<InstrumentRiskLimits>
{
    return InstrumentRiskLimits{.max_qty = std::move(a_qty),
                                .max_notional = std::move(a_notional),
                                .max_position = std::move(a_position)};
}

auto order(std::string a_qty, std::string a_px = "50000",
           std::string a_position = "0") -> RiskOrder
{
    return RiskOrder{
        .side = Side::Buy, .price = std::move(a_px), .quantity = std::move(a_qty),
        .projected_position = std::move(a_position)};
}

} // namespace

TEST_CASE("risk_config_from_json parses defaults and per-instrument limits")
{
    const auto parsed = risk_config_from_json(nlohmann::json::parse(R"({
        "default": {"maxQty": "10", "maxNotional": "1000000", "maxPosition": "5"},
        "instruments": {"BTC-USDT": {"maxQty": "1", "maxPosition": "0.5"}}
    })"));
    REQUIRE(parsed.is_ok());
    const auto& config = parsed.value();

    REQUIRE(config.defaults.has_value());
    CHECK(config.defaults->max_qty == "10");
    CHECK(config.defaults->max_notional == "1000000");
    CHECK(config.defaults->max_position == "5");

    const auto btc = config.limits_for("BTC-USDT");
    REQUIRE(btc.has_value());
    CHECK(btc->max_qty == "1");           // instrument entry replaces defaults
    CHECK(btc->max_notional.empty());     // ... wholesale, not merged
    CHECK(btc->max_position == "0.5");

    const auto eth = config.limits_for("ETH-USDT");
    REQUIRE(eth.has_value());
    CHECK(eth->max_qty == "10");          // falls back to defaults
    CHECK(config.limits_for("SOL-USDT").has_value()); // defaults apply everywhere
}

TEST_CASE("risk_config_from_json accepts empty and partial sections")
{
    const auto empty = risk_config_from_json(nlohmann::json::object());
    REQUIRE(empty.is_ok());
    CHECK_FALSE(empty.value().limits_for("BTC-USDT").has_value());

    const auto partial = risk_config_from_json(
        nlohmann::json::parse(R"({"default":{"maxQty":"2"}})"));
    REQUIRE(partial.is_ok());
    const auto limits_btc = partial.value().limits_for("BTC-USDT");
    REQUIRE(limits_btc.has_value());
    CHECK(limits_btc->max_qty == "2");
    CHECK(limits_btc->max_notional.empty());
}

TEST_CASE("risk_config_from_json rejects malformed sections")
{
    const auto rejects = [](const std::string& a_json) {
        const auto parsed = risk_config_from_json(nlohmann::json::parse(a_json, nullptr, false));
        REQUIRE_FALSE(parsed.is_ok());
        CHECK(parsed.error().code == "protocol");
    };
    rejects(R"({"default":{"maxQty":"abc"}})");
    rejects(R"({"default":{"maxQty":10}})");
    rejects(R"({"default":"nope"})");
    rejects(R"({"instruments":{"BTC-USDT":{"maxPosition":"1..2"}}})");
    rejects(R"({"instruments":[]})");
    rejects(R"([1,2,3])");
}

TEST_CASE("no limits means every order passes")
{
    CHECK(check_risk(unlimited(), "BTC-USDT", order("1000000")).has_value() == false);
}

TEST_CASE("maxQty rejects oversized quantities and passes the boundary")
{
    const auto rule = limits("1");
    CHECK(check_risk(rule, "BTC-USDT", order("1.00001"))->code == "risk_max_qty");
    CHECK_FALSE(check_risk(rule, "BTC-USDT", order("1")).has_value());
    CHECK_FALSE(check_risk(rule, "BTC-USDT", order("0.5")).has_value());
}

TEST_CASE("maxNotional rejects expensive orders and is skipped for market orders")
{
    const auto rule = limits("", "1000");
    // 50000 * 0.001 = 50 -> fine; 50000 * 0.1 = 5000 -> rejected
    CHECK_FALSE(check_risk(rule, "BTC-USDT", order("0.001")).has_value());
    const auto rejection = check_risk(rule, "BTC-USDT", order("0.1"));
    REQUIRE(rejection.has_value());
    CHECK(rejection->code == "risk_max_notional");
    CHECK(rejection->message.find("5000") != std::string::npos);

    // market orders carry no price: the notional check is skipped
    CHECK_FALSE(check_risk(rule, "BTC-USDT", order("100", "")).has_value());
}

TEST_CASE("maxPosition is enforced symmetrically on both directions")
{
    const auto rule = limits("", "", "2");
    CHECK_FALSE(check_risk(rule, "BTC-USDT", order("1", "50000", "1")).has_value());
    CHECK_FALSE(check_risk(rule, "BTC-USDT", order("1", "50000", "-1.5")).has_value());
    CHECK(check_risk(rule, "BTC-USDT", order("1", "50000", "2.5"))->code ==
          "risk_max_position");
    CHECK(check_risk(rule, "BTC-USDT", order("1", "50000", "-2.0001"))->code ==
          "risk_max_position");
    CHECK(check_risk(rule, "BTC-USDT", order("1", "50000", "-2")).has_value() == false);
}

TEST_CASE("non-decimal order values fail closed")
{
    const auto qty_rule = limits("1");
    CHECK(check_risk(qty_rule, "BTC-USDT", order("lots"))->code == "risk_invalid_value");

    const auto notional_rule = limits("", "1000");
    CHECK(check_risk(notional_rule, "BTC-USDT", order("1", "cheap"))->code ==
          "risk_invalid_value");

    const auto position_rule = limits("", "", "2");
    CHECK(check_risk(position_rule, "BTC-USDT", order("1", "50000", "none"))->code ==
          "risk_invalid_value");
}

TEST_CASE("reject messages name the instrument and the numbers involved")
{
    const auto rule = limits("0.5");
    const auto rejection = check_risk(rule, "BTC-USDT", order("10"));
    REQUIRE(rejection.has_value());
    CHECK(rejection->message.find("BTC-USDT") != std::string::npos);
    CHECK(rejection->message.find("10") != std::string::npos);
    CHECK(rejection->message.find("0.5") != std::string::npos);
}
