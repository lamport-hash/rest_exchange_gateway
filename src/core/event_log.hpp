#pragma once

#include "gateway/result.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

namespace gateway {

/// Append-only JSON-lines event log backing restart recovery. Each event
/// is one line, flushed on append. Crash safety relies on the replay
/// rule: a torn (unterminated or unparsable) FINAL line is discarded and
/// the file is truncated back to the last good offset, while a malformed
/// line followed by valid data is real corruption and fails the replay.
///
/// Event schema (written and replayed by the OMS; "ts" is informational):
///  - {"type":"place_submitted","clientOrderId","symbol","venue","side",
///     "orderType","price","quantity","timeInForce"}
///     — persisted BEFORE the venue call; a log ending here (no
///     place_accepted/rejected follow-up) replays as a Pending entry that
///     reconciliation resolves
///  - {"type":"place_accepted","clientOrderId","symbol","side","orderType",
///     "price","quantity","timeInForce","exchangeOrderId"}
///  - {"type":"adopted", <place_accepted fields>,"state","filledQuantity",
///     "averageFillPrice"}        — venue-live order adopted at (re)start
///  - {"type":"rejected","clientOrderId","symbol","code","reason"}
///  - {"type":"amended","clientOrderId","price","quantity"}
///  - {"type":"state","clientOrderId","state","filledQuantity",
///     "averageFillPrice"}
class EventLog
{
  public:
    struct ReplayStats
    {
        std::size_t events = 0;
        /// A torn tail line was dropped and the file truncated.
        bool tail_truncated = false;
    };

    /// Open (create if missing) a_path for appending. The parent
    /// directory must exist. Errors: "io".
    explicit EventLog(std::filesystem::path a_path);

    EventLog(const EventLog&) = delete;
    auto operator=(const EventLog&) -> EventLog& = delete;
    EventLog(EventLog&&) = default;
    auto operator=(EventLog&&) -> EventLog& = default;
    ~EventLog() = default;

    /// Append one event object as a single line and flush.
    /// Errors: "io" (the event is not written).
    [[nodiscard]] auto append(const nlohmann::json& a_event) -> std::optional<Error>;

    [[nodiscard]] auto path() const -> const std::filesystem::path&;

    /// Feed every complete, parsable event to a_sink in file order.
    /// A missing file is an empty log (not an error). Rules:
    /// - unterminated final segment: dropped as a torn write (an event
    ///   counts only when its line was fully written and terminated);
    ///   stats.tail_truncated is set and the file is resized back to the
    ///   last accepted offset so appends stay clean;
    /// - unparsable or non-object COMPLETE line: "persistence" error
    ///   (mid-file corruption — fail rather than guess).
    [[nodiscard]] static auto
    replay(const std::filesystem::path& a_path,
           const std::function<void(const nlohmann::json&)>& a_sink) -> Result<ReplayStats>;

  private:
    std::filesystem::path path_;
    std::ofstream out_;
};

} // namespace gateway
