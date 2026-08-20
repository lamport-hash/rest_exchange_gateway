#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/retry.hpp"

#include <chrono>
#include <deque>
#include <vector>

namespace {

using gateway::Error;
using gateway::RetryPolicy;

/// Deterministic clock: now() only advances when sleep() is called; every
/// sleep is recorded; random01 pops scripted values.
struct FakeClock
{
    std::chrono::steady_clock::time_point now{std::chrono::milliseconds{1000}};
    std::vector<std::chrono::milliseconds> sleeps;
    double random_value = 0.0;
    std::deque<double> random_values{};

    [[nodiscard]] auto clock() const -> gateway::RetryClock
    {
        return gateway::RetryClock{.now = [this] { return now; },
                                   .sleep =
                                       [this](std::chrono::milliseconds a_duration) {
                                           const_cast<FakeClock*>(this)->sleeps.push_back(
                                               a_duration);
                                           const_cast<FakeClock*>(this)->now += a_duration;
                                       },
                                   .random01 =
                                       [this] {
                                           if (random_values.empty()) {
                                               return random_value;
                                           }
                                           const double value = random_values.front();
                                           const_cast<FakeClock*>(this)->random_values.pop_front();
                                           return value;
                                       }};
    }
};

auto test_policy() -> RetryPolicy
{
    return RetryPolicy{.max_attempts = 3,
                       .initial_backoff = std::chrono::milliseconds{100},
                       .max_backoff = std::chrono::milliseconds{400},
                       .multiplier = 2.0,
                       .jitter = 0.5,
                       .budget = std::chrono::milliseconds{5000}};
}

auto transport_error() -> Error
{
    return Error{"transport", "network failure"};
}

auto ok() -> gateway::Result<int>
{
    return 42;
}

TEST_CASE("with_retries returns the first success without sleeping")
{
    FakeClock fake;
    const auto policy = test_policy();
    int attempts = 0;

    const auto result = gateway::with_retries<int>(
        policy, fake.clock(),
        [&attempts]() -> gateway::Result<int> {
            ++attempts;
            return ok();
        },
        [](const Error&) { return true; });

    REQUIRE(result.is_ok());
    CHECK(result.value() == 42);
    CHECK(attempts == 1);
    CHECK(fake.sleeps.empty());
}

TEST_CASE("with_retries retries a transport error and succeeds on attempt two")
{
    FakeClock fake;
    const auto policy = test_policy();
    int attempts = 0;

    const auto result = gateway::with_retries<int>(
        policy, fake.clock(),
        [&attempts]() -> gateway::Result<int> {
            ++attempts;
            if (attempts == 1) {
                return transport_error();
            }
            return ok();
        },
        [](const Error& a_error) { return a_error.code == "transport"; });

    REQUIRE(result.is_ok());
    CHECK(attempts == 2);
    REQUIRE(fake.sleeps.size() == 1);
    // random01 = 0.0 -> no jitter applied -> full initial backoff
    CHECK(fake.sleeps.front() == std::chrono::milliseconds{100});
}

TEST_CASE("jitter scales the backoff down to [delay*(1-jitter), delay]")
{
    auto policy = test_policy();
    policy.max_attempts = 2; // exactly one backoff sleep per run
    const std::vector<std::pair<double, int>> cases{{0.0, 100}, {0.5, 75}, {1.0, 50}};
    for (const auto& [u01, expected_ms] : cases) {
        FakeClock fake;
        fake.random_value = u01;
        int attempts = 0;
        const auto result = gateway::with_retries<int>(
            policy, fake.clock(),
            [&attempts]() -> gateway::Result<int> {
                ++attempts;
                return transport_error();
            },
            [](const Error&) { return true; });
        (void)result;
        REQUIRE(fake.sleeps.size() == 1);
        CHECK(fake.sleeps.front() == std::chrono::milliseconds{expected_ms});
    }
}

TEST_CASE("non-retryable errors are returned immediately without sleeping")
{
    const FakeClock fake;
    const auto policy = test_policy();
    int attempts = 0;

    const auto result = gateway::with_retries<int>(
        policy, fake.clock(),
        [&attempts]() -> gateway::Result<int> {
            ++attempts;
            return Error{"venue:51001", "Instrument ID does not exist"};
        },
        [](const Error& a_error) { return a_error.code == "transport"; });

    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "venue:51001");
    CHECK(attempts == 1);
    CHECK(fake.sleeps.empty());
}

TEST_CASE("attempts are exhausted after max_attempts with capped growing backoff")
{
    FakeClock fake;
    auto policy = test_policy();
    policy.max_attempts = 5;
    fake.random_value = 0.0; // deterministic: full backoff every time
    int attempts = 0;

    const auto result = gateway::with_retries<int>(
        policy, fake.clock(),
        [&attempts]() -> gateway::Result<int> {
            ++attempts;
            return transport_error();
        },
        [](const Error&) { return true; });

    REQUIRE_FALSE(result.is_ok());
    CHECK(attempts == 5);
    // backoffs for retries 1..4: 100, 200, 400 (capped), 400
    REQUIRE(fake.sleeps.size() == 4);
    CHECK(fake.sleeps[0] == std::chrono::milliseconds{100});
    CHECK(fake.sleeps[1] == std::chrono::milliseconds{200});
    CHECK(fake.sleeps[2] == std::chrono::milliseconds{400});
    CHECK(fake.sleeps[3] == std::chrono::milliseconds{400});
}

TEST_CASE("the budget deadline stops retries even with attempts left")
{
    FakeClock fake;
    auto policy = test_policy();
    policy.max_attempts = 10;
    policy.initial_backoff = std::chrono::milliseconds{1000};
    policy.max_backoff = std::chrono::milliseconds{1000};
    policy.budget = std::chrono::milliseconds{300};
    fake.random_value = 0.0;
    int attempts = 0;

    const auto result = gateway::with_retries<int>(
        policy, fake.clock(),
        [&attempts]() -> gateway::Result<int> {
            ++attempts;
            return transport_error();
        },
        [](const Error&) { return true; });

    REQUIRE_FALSE(result.is_ok());
    // sleep truncated to the remaining budget, then the next failure happens
    // past the deadline and no further retry starts.
    CHECK(attempts == 2);
    REQUIRE(fake.sleeps.size() == 1);
    CHECK(fake.sleeps[0] == std::chrono::milliseconds{300});
}

TEST_CASE("budget exactly reached counts as exhausted")
{
    FakeClock fake;
    auto policy = test_policy();
    policy.budget = std::chrono::milliseconds{0};
    fake.random_value = 0.0;
    int attempts = 0;

    const auto result = gateway::with_retries<int>(
        policy, fake.clock(),
        [&attempts]() -> gateway::Result<int> {
            ++attempts;
            return transport_error();
        },
        [](const Error&) { return true; });

    REQUIRE_FALSE(result.is_ok());
    CHECK(attempts == 1);
    CHECK(fake.sleeps.empty());
}

TEST_CASE("max_attempts of 1 performs exactly one attempt")
{
    FakeClock fake;
    auto policy = test_policy();
    policy.max_attempts = 1;
    int attempts = 0;

    const auto result = gateway::with_retries<int>(
        policy, fake.clock(),
        [&attempts]() -> gateway::Result<int> {
            ++attempts;
            return transport_error();
        },
        [](const Error&) { return true; });

    REQUIRE_FALSE(result.is_ok());
    CHECK(attempts == 1);
    CHECK(fake.sleeps.empty());
}

TEST_CASE("backoff_for grows exponentially and caps at max_backoff")
{
    auto policy = test_policy();
    policy.initial_backoff = std::chrono::milliseconds{100};
    policy.max_backoff = std::chrono::milliseconds{400};
    policy.multiplier = 2.0;

    CHECK(gateway::backoff_for(policy, 1) == std::chrono::milliseconds{100});
    CHECK(gateway::backoff_for(policy, 2) == std::chrono::milliseconds{200});
    CHECK(gateway::backoff_for(policy, 3) == std::chrono::milliseconds{400});
    CHECK(gateway::backoff_for(policy, 4) == std::chrono::milliseconds{400});
    CHECK(gateway::backoff_for(policy, 100) == std::chrono::milliseconds{400});
    // non-positive indices are clamped to the first retry
    CHECK(gateway::backoff_for(policy, 0) == std::chrono::milliseconds{100});
    CHECK(gateway::backoff_for(policy, -3) == std::chrono::milliseconds{100});
}

TEST_CASE("apply_jitter bounds")
{
    auto policy = test_policy();
    policy.jitter = 0.5;
    const auto raw = std::chrono::milliseconds{200};
    CHECK(gateway::apply_jitter(policy, raw, 0.0) == std::chrono::milliseconds{200});
    CHECK(gateway::apply_jitter(policy, raw, 0.5) == std::chrono::milliseconds{150});
    CHECK(gateway::apply_jitter(policy, raw, 1.0) == std::chrono::milliseconds{100});
    CHECK(gateway::apply_jitter(policy, raw, 7.0) == std::chrono::milliseconds{100});

    policy.jitter = 0.0;
    CHECK(gateway::apply_jitter(policy, raw, 0.9) == std::chrono::milliseconds{200});

    policy.jitter = 1.0;
    CHECK(gateway::apply_jitter(policy, raw, 0.999999) == std::chrono::milliseconds{0});
}

TEST_CASE("retry_policy_from_json parses a complete policy")
{
    const auto node = nlohmann::json::parse(
        R"({"maxAttempts":5,"initialBackoffMs":50,"maxBackoffMs":700,"multiplier":3.0,"jitter":0.25,"budgetMs":9000})");
    const auto result = gateway::retry_policy_from_json(node, RetryPolicy{});
    REQUIRE(result.is_ok());
    const auto& policy = result.value();
    CHECK(policy.max_attempts == 5);
    CHECK(policy.initial_backoff == std::chrono::milliseconds{50});
    CHECK(policy.max_backoff == std::chrono::milliseconds{700});
    CHECK(policy.multiplier == 3.0);
    CHECK(policy.jitter == 0.25);
    CHECK(policy.budget == std::chrono::milliseconds{9000});
}

TEST_CASE("retry_policy_from_json keeps defaults for missing fields")
{
    const auto result = gateway::retry_policy_from_json(
        nlohmann::json::parse(R"({"maxAttempts":2})"), test_policy());
    REQUIRE(result.is_ok());
    CHECK(result.value().max_attempts == 2);
    CHECK(result.value().initial_backoff == std::chrono::milliseconds{100});
    CHECK(result.value().budget == std::chrono::milliseconds{5000});
}

TEST_CASE("retry_policy_from_json rejects invalid values")
{
    const auto defaults = RetryPolicy{};
    const auto parse = [&defaults](const std::string& a_text) {
        return gateway::retry_policy_from_json(nlohmann::json::parse(a_text), defaults);
    };

    CHECK_FALSE(parse(R"({"maxAttempts":0})").is_ok());
    CHECK_FALSE(parse(R"({"maxAttempts":101})").is_ok());
    CHECK_FALSE(parse(R"({"maxAttempts":2.5})").is_ok());
    CHECK_FALSE(parse(R"({"maxAttempts":"3"})").is_ok());
    CHECK_FALSE(parse(R"({"multiplier":0.5})").is_ok());
    CHECK_FALSE(parse(R"({"multiplier":"2"})").is_ok());
    CHECK_FALSE(parse(R"({"jitter":-0.1})").is_ok());
    CHECK_FALSE(parse(R"({"jitter":1.5})").is_ok());
    CHECK_FALSE(parse(R"({"initialBackoffMs":-5})").is_ok());
    CHECK_FALSE(parse(R"({"initialBackoffMs":"100"})").is_ok());
    // maxBackoffMs below initialBackoffMs is contradictory
    CHECK_FALSE(parse(R"({"initialBackoffMs":500,"maxBackoffMs":100})").is_ok());
    CHECK_FALSE(parse(R"([1,2])").is_ok());

    // unknown fields are tolerated (consistent with the okx section parsing)
    CHECK(parse(R"({"somethingElse":true})").is_ok());
}

TEST_CASE("real_retry_clock is wired and random01 stays in [0,1)")
{
    const auto clock = gateway::real_retry_clock();
    const auto first = clock.now();
    const auto second = clock.now();
    CHECK(second >= first);
    for (int i = 0; i < 100; ++i) {
        const double value = clock.random01();
        CHECK(value >= 0.0);
        CHECK(value < 1.0);
    }
}

} // namespace
