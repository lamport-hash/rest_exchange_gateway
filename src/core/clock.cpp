#include "core/clock.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace gateway {

namespace {
constexpr std::size_t kIsoLen = 24;
}

auto utc_now_iso_ms() -> std::string
{
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm tm_utc{};
    gmtime_r(&secs, &tm_utc);

    std::string out(kIsoLen, '\0');
    const int written =
        std::snprintf(out.data(), out.size() + 1, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour,
                      tm_utc.tm_min, tm_utc.tm_sec, static_cast<int>(ms));
    out.resize(static_cast<std::size_t>(written));
    return out;
}

auto utc_now_epoch_ms() -> std::string
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 1000;
    char frac[8];
    std::snprintf(frac, sizeof(frac), ".%03d", static_cast<int>(ms));
    return std::to_string(secs) + frac;
}

} // namespace gateway
