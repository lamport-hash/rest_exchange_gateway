#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/clock.hpp"

#include <regex>
#include <string>

namespace {

TEST_CASE("utc_now_iso_ms returns OKX timestamp format")
{
    const std::string stamp = gateway::utc_now_iso_ms();
    const std::regex pattern(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$)");
    CAPTURE(stamp);
    CHECK(std::regex_match(stamp, pattern));
    CHECK(stamp.size() == 24);
}

TEST_CASE("utc_now_iso_ms is monotonic within one second of wall time")
{
    const std::string first = gateway::utc_now_iso_ms();
    const std::string second = gateway::utc_now_iso_ms();
    CHECK(first.substr(0, 19) <= second.substr(0, 19));
}

} // namespace
