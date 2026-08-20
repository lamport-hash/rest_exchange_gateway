#pragma once

#include <string>

namespace gateway {

/// Current UTC time formatted as ISO 8601 with milliseconds,
/// e.g. "2026-08-20T10:00:00.123Z" (OK-ACCESS-TIMESTAMP format).
[[nodiscard]] auto utc_now_iso_ms() -> std::string;

} // namespace gateway
