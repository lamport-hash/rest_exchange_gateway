#include "core/event_log.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace gateway {

EventLog::EventLog(std::filesystem::path a_path) : path_(std::move(a_path))
{
    out_.open(path_, std::ios::app);
}

auto EventLog::path() const -> const std::filesystem::path&
{
    return path_;
}

auto EventLog::append(const nlohmann::json& a_event) -> std::optional<Error>
{
    if (!out_.is_open()) {
        return Error{"io", "event log is not open: " + path_.string()};
    }
    out_ << a_event.dump() << '\n';
    out_.flush();
    if (!out_.good()) {
        out_.clear();
        return Error{"io", "failed to append to event log: " + path_.string()};
    }
    return std::nullopt;
}

auto EventLog::replay(const std::filesystem::path& a_path,
                      const std::function<void(const nlohmann::json&)>& a_sink)
    -> Result<ReplayStats>
{
    if (!std::filesystem::exists(a_path)) {
        return ReplayStats{};
    }

    std::ifstream input(a_path, std::ios::binary);
    if (!input.is_open()) {
        return Error{"io", "cannot open event log for replay: " + a_path.string()};
    }
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    if (input.bad()) {
        return Error{"io", "failed while reading event log: " + a_path.string()};
    }

    ReplayStats stats;
    /// Offset just past the last accepted line; a torn tail is truncated
    /// back to this point.
    std::size_t good_offset = 0;
    std::size_t line_number = 0;
    std::size_t position = 0;
    while (position < content.size()) {
        const std::size_t newline = content.find('\n', position);
        ++line_number;
        if (newline == std::string::npos) {
            // Unterminated tail: an event only counts once its line was
            // completely written (terminated). Dropping a fully-parseable
            // tail is safe — the venue-side reconciliation repairs any
            // state the lost event carried.
            stats.tail_truncated = true;
            break;
        }
        const std::string_view line =
            std::string_view{content}.substr(position, newline - position);
        const auto event = nlohmann::json::parse(line, nullptr, false);
        if (event.is_discarded() || !event.is_object()) {
            return Error{"persistence", "corrupt event log: line " + std::to_string(line_number) +
                                            " of " + a_path.string() + " is not a JSON object"};
        }
        a_sink(event);
        ++stats.events;
        position = newline + 1;
        good_offset = position;
    }

    if (stats.tail_truncated) {
        std::error_code error_code;
        std::filesystem::resize_file(a_path, good_offset, error_code);
        if (error_code) {
            return Error{"io", "cannot truncate torn event log tail: " + error_code.message()};
        }
    }
    return stats;
}

} // namespace gateway
