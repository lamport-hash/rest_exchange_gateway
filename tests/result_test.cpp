#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/result.hpp"

#include <string>
#include <utility>

namespace {

TEST_CASE("Result holds a value for the success case")
{
    const gateway::Result<int> result = 42;
    CHECK(result.is_ok());
    CHECK(result.value() == 42);
    CHECK(result.value_or(7) == 42);
}

TEST_CASE("Result holds an error for the failure case")
{
    const gateway::Result<int> result = gateway::Error{"transport", "connection refused"};
    CHECK_FALSE(result.is_ok());
    CHECK(result.error() == gateway::Error{"transport", "connection refused"});
    CHECK(result.value_or(7) == 7);
}

TEST_CASE("errors compare by code and message")
{
    CHECK(gateway::Error{"a", "b"} == gateway::Error{"a", "b"});
    CHECK(gateway::Error{"a", "b"} != gateway::Error{"a", "c"});
    CHECK(gateway::Error{"a", "b"} != gateway::Error{"c", "b"});
}

TEST_CASE("Result works with move-only and non-default types")
{
    gateway::Result<std::string> result = std::string("payload");
    CHECK(result.is_ok());
    CHECK(result.value() == "payload");

    const auto moved = std::move(result).value();
    CHECK(moved == "payload");
}

TEST_CASE("Result deduction guides pick the value type")
{
    gateway::Result from_error = gateway::Error{"protocol", "bad"};
    static_assert(std::is_same_v<decltype(from_error), gateway::Result<gateway::Error>>);
    CHECK_FALSE(from_error.is_ok());

    gateway::Result from_value = 42;
    static_assert(std::is_same_v<decltype(from_value), gateway::Result<int>>);
    CHECK(from_value.value() == 42);
}

} // namespace
