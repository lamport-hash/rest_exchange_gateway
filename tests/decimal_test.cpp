#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/decimal.hpp"

#include <string>

namespace {

using namespace gateway;

auto parse_ok(const std::string& a_text) -> Decimal
{
    const auto result = parse_decimal(a_text);
    REQUIRE(result.is_ok());
    return result.value();
}

} // namespace

TEST_CASE("parse_decimal accepts plain decimals and normalizes scale")
{
    CHECK(parse_ok("0").unscaled == 0);
    CHECK(parse_ok("0").scale == 0);
    CHECK(parse_ok("50000").unscaled == 50000);
    CHECK(parse_ok("0.001").unscaled == 1);
    CHECK(parse_ok("0.001").scale == 3);
    CHECK(parse_ok("49999.50").unscaled == 499995);
    CHECK(parse_ok("49999.50").scale == 1);
    CHECK(parse_ok("0.00000001").scale == 8);
    // trailing zeros are stripped: 1.2300 == 1.23
    CHECK(decimal_to_string(parse_ok("1.2300")) == "1.23");
    CHECK(decimal_to_string(parse_ok("0.0")) == "0");
    CHECK(decimal_to_string(parse_ok("000123")) == "123");
}

TEST_CASE("parse_decimal rejects malformed input")
{
    const auto rejects = [](const std::string& a_text) {
        const auto result = parse_decimal(a_text);
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "protocol");
    };
    rejects("");
    rejects(".");
    rejects("1.");
    rejects(".5");
    rejects("1..5");
    rejects("1.2.3");
    rejects("-1");
    rejects("+1");
    rejects("1e5");
    rejects("abc");
    rejects("1 000");
    rejects("1,5");
    rejects("1.000000001"); // 9 fractional digits
    rejects("99999999999999999999"); // > 64 bits
}

TEST_CASE("parse_signed_decimal accepts one leading minus only")
{
    const auto negative = parse_signed_decimal("-1.5");
    REQUIRE(negative.is_ok());
    CHECK(decimal_to_string(negative.value()) == "-1.5");
    CHECK(compare(negative.value(), parse_ok("0")) == -1);

    for (const std::string bad : {"", "-", "--1", "+1", "-.5", "-1.", "x"}) {
        CHECK_FALSE(parse_signed_decimal(bad).is_ok());
    }
    CHECK(parse_signed_decimal("2.50").is_ok());
}

TEST_CASE("compare is scale-insensitive and signed")
{
    CHECK(compare(parse_ok("1.0"), parse_ok("1")) == 0);
    CHECK(compare(parse_ok("0.001"), parse_ok("0.0010")) == 0);
    CHECK(compare(parse_ok("0.001"), parse_ok("0.002")) == -1);
    CHECK(compare(parse_ok("2"), parse_ok("1.99999999")) == 1);
    CHECK(compare(negate(parse_ok("2")), parse_ok("1")) == -1);
    CHECK(compare(parse_ok("0"), negate(parse_ok("0"))) == 0);
}

TEST_CASE("add and sub are exact across different scales")
{
    CHECK(decimal_to_string(add(parse_ok("0.001"), parse_ok("0.002")).value()) == "0.003");
    CHECK(decimal_to_string(add(parse_ok("1.5"), parse_ok("2")).value()) == "3.5");
    CHECK(decimal_to_string(sub(parse_ok("1"), parse_ok("0.00000001")).value()) ==
          "0.99999999");
    CHECK(decimal_to_string(sub(parse_ok("0.5"), parse_ok("2")).value()) == "-1.5");
    CHECK(decimal_to_string(add(negate(parse_ok("0.5")), parse_ok("0.25")).value()) ==
          "-0.25");
}

TEST_CASE("sub_clamped_zero never goes negative")
{
    CHECK(decimal_to_string(sub_clamped_zero(parse_ok("1"), parse_ok("0.25"))) == "0.75");
    CHECK(decimal_to_string(sub_clamped_zero(parse_ok("0.25"), parse_ok("1"))) == "0");
    CHECK(decimal_to_string(sub_clamped_zero(parse_ok("1"), parse_ok("1.0"))) == "0");
}

TEST_CASE("mul is exact and carries combined scale")
{
    const auto notional = mul(parse_ok("50000"), parse_ok("0.001"));
    REQUIRE(notional.is_ok());
    CHECK(decimal_to_string(notional.value()) == "50");
    CHECK(decimal_to_string(mul(parse_ok("0.1"), parse_ok("0.2")).value()) == "0.02");
    CHECK(decimal_to_string(mul(parse_ok("49999.5"), parse_ok("0.0004")).value()) ==
          "19.9998");
    CHECK(decimal_to_string(mul(negate(parse_ok("3")), parse_ok("2")).value()) == "-6");
}

TEST_CASE("arithmetic reports overflow instead of wrapping")
{
    const auto huge = parse_ok("999999999999999");
    const auto big = mul(huge, huge);
    REQUIRE_FALSE(big.is_ok());
    CHECK(big.error().code == "protocol");

    const auto lhs = parse_ok("9200000000000000000");
    const auto sum = add(lhs, lhs);
    REQUIRE_FALSE(sum.is_ok());

    const auto diff = sub(negate(lhs), lhs);
    REQUIRE_FALSE(diff.is_ok());
}

TEST_CASE("decimal_to_string pads small fractional values")
{
    CHECK(decimal_to_string(Decimal{.unscaled = 5, .scale = 2}) == "0.05");
    CHECK(decimal_to_string(Decimal{.unscaled = -5, .scale = 2}) == "-0.05");
    CHECK(decimal_to_string(Decimal{.unscaled = -123, .scale = 1}) == "-12.3");
    CHECK(decimal_to_string(Decimal{.unscaled = 123, .scale = 4}) == "0.0123");
}
