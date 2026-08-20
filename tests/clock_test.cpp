#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/clock.hpp"

#include <chrono>
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

TEST_CASE("utc_now_epoch_ms returns OKX WS login timestamp format")
{
    const std::string stamp = gateway::utc_now_epoch_ms();
    const std::regex pattern(R"(^\d{10}\.\d{3}$)");
    CAPTURE(stamp);
    CHECK(std::regex_match(stamp, pattern));
}

TEST_CASE("utc_now_epoch_ms advances and stays near the wall clock")
{
    const std::string first = gateway::utc_now_epoch_ms();
    const std::string second = gateway::utc_now_epoch_ms();
    CHECK(std::stoll(first) <= std::stoll(second));
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto now_secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    CHECK(std::stoll(first) <= now_secs + 1);
    CHECK(std::stoll(first) >= now_secs - 5);
}

} // namespace
