#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/event_log.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace gateway;

auto temp_log_path(const char* a_name) -> std::filesystem::path
{
    return std::filesystem::temp_directory_path() /
           ("gateway_event_log_test_" + std::string(a_name) + ".jsonl");
}

auto write_file(const std::filesystem::path& a_path, const std::string& a_content)
{
    std::ofstream file(a_path, std::ios::trunc | std::ios::binary);
    file << a_content;
}

auto read_file(const std::filesystem::path& a_path) -> std::string
{
    std::ifstream file(a_path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

auto replayed_events(const std::filesystem::path& a_path)
    -> Result<EventLog::ReplayStats>
{
    // the sink copies events; tests inspect them via a lambda capture
    // instead, this helper only checks success
    return EventLog::replay(a_path, [](const nlohmann::json&) {});
}

} // namespace

TEST_CASE("append then replay round-trips events in order")
{
    const auto path = temp_log_path("roundtrip");
    std::filesystem::remove(path);
    {
        EventLog log{path};
        REQUIRE(log.append({{"type", "place_accepted"}, {"clientOrderId", "a"}}) ==
                 std::nullopt);
        REQUIRE(log.append({{"type", "state"}, {"clientOrderId", "a"}, {"state", "filled"}}) ==
                 std::nullopt);
    }

    std::vector<nlohmann::json> events;
    const auto stats = EventLog::replay(path, [&events](const nlohmann::json& a_event) {
        events.push_back(a_event);
    });
    REQUIRE(stats.is_ok());
    CHECK(stats.value().events == 2);
    CHECK_FALSE(stats.value().tail_truncated);
    REQUIRE(events.size() == 2);
    CHECK(events[0]["type"] == "place_accepted");
    CHECK(events[1]["state"] == "filled");
    std::filesystem::remove(path);
}

TEST_CASE("a second EventLog appends without destroying earlier lines")
{
    const auto path = temp_log_path("append_twice");
    std::filesystem::remove(path);
    {
        EventLog first{path};
        REQUIRE(first.append({{"type", "a"}}) == std::nullopt);
    }
    {
        EventLog second{path};
        REQUIRE(second.append({{"type", "b"}}) == std::nullopt);
    }
    const auto stats = replayed_events(path);
    REQUIRE(stats.is_ok());
    CHECK(stats.value().events == 2);
    std::filesystem::remove(path);
}

TEST_CASE("replay of a missing file is an empty log")
{
    const auto path = temp_log_path("missing");
    std::filesystem::remove(path);
    const auto stats = replayed_events(path);
    REQUIRE(stats.is_ok());
    CHECK(stats.value().events == 0);
    CHECK_FALSE(stats.value().tail_truncated);
}

TEST_CASE("a torn final line is dropped and the file is truncated")
{
    const auto path = temp_log_path("torn_tail");
    const std::string good =
        R"({"type":"place_accepted","clientOrderId":"a"})" "\n"
        R"({"type":"state","clientOrderId":"a","state":"filled"})" "\n";

    SUBCASE("partial JSON tail")
    {
        write_file(path, good + R"({"type":"amended","clientOr)");
        std::vector<nlohmann::json> events;
        const auto stats = EventLog::replay(
            path, [&events](const nlohmann::json& a_event) { events.push_back(a_event); });
        REQUIRE(stats.is_ok());
        CHECK(stats.value().events == 2);
        CHECK(stats.value().tail_truncated);
        // the file is now exactly the two good lines...
        CHECK(read_file(path) == good);
        // ...and new appends produce clean lines
        {
            EventLog log{path};
            REQUIRE(log.append({{"type", "amended"}}) == std::nullopt);
        }
        const auto after = replayed_events(path);
        REQUIRE(after.is_ok());
        CHECK(after.value().events == 3);
        CHECK_FALSE(after.value().tail_truncated);
    }

    SUBCASE("complete JSON without its terminating newline is also a torn tail")
    {
        write_file(path, good + R"({"type":"amended","clientOrderId":"a"})");
        const auto stats = replayed_events(path);
        REQUIRE(stats.is_ok());
        CHECK(stats.value().events == 2);
        CHECK(stats.value().tail_truncated);
        CHECK(read_file(path) == good);
    }

    std::filesystem::remove(path);
}

TEST_CASE("a corrupt line in the middle fails the replay")
{
    const auto path = temp_log_path("corrupt_middle");
    write_file(path,
               R"({"type":"a"})" "\n"
               "this is not json" "\n"
               R"({"type":"b"})" "\n");
    bool sink_called = false;
    const auto stats = EventLog::replay(path, [&sink_called](const nlohmann::json&) {
        sink_called = true;
    });
    REQUIRE_FALSE(stats.is_ok());
    CHECK(stats.error().code == "persistence");
    CHECK(sink_called); // earlier lines were delivered before the failure
    std::filesystem::remove(path);
}

TEST_CASE("a parsable non-object line is corruption, not an event")
{
    const auto path = temp_log_path("array_line");
    write_file(path, R"({"type":"a"})" "\n[1,2,3]\n");
    const auto stats = replayed_events(path);
    REQUIRE_FALSE(stats.is_ok());
    CHECK(stats.error().code == "persistence");
    std::filesystem::remove(path);
}

TEST_CASE("an empty file replays zero events without truncation")
{
    const auto path = temp_log_path("empty");
    write_file(path, "");
    const auto stats = replayed_events(path);
    REQUIRE(stats.is_ok());
    CHECK(stats.value().events == 0);
    CHECK_FALSE(stats.value().tail_truncated);
    std::filesystem::remove(path);
}

TEST_CASE("append reports io errors for an unwritable path")
{
    const auto path = std::filesystem::temp_directory_path() / "no_such_dir" / "log.jsonl";
    EventLog log{path};
    const auto error = log.append({{"type", "a"}});
    REQUIRE(error.has_value());
    CHECK(error->code == "io");
}
