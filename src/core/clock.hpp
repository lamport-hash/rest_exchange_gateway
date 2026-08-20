#pragma once

#include <string>

namespace gateway {

/// Current UTC time formatted as ISO 8601 with milliseconds,
/// e.g. "2026-08-20T10:00:00.123Z" (OK-ACCESS-TIMESTAMP format).
[[nodiscard]] auto utc_now_iso_ms() -> std::string;

/// Current UTC time as epoch seconds with milliseconds,
/// e.g. "1787235344.678" (OKX WebSocket login timestamp format — the
/// private WS login rejects ISO 8601 with error 60004).
[[nodiscard]] auto utc_now_epoch_ms() -> std::string;

} // namespace gateway
