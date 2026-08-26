#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "util/free_port.hpp"

#include "core/event_log.hpp"
#include "core/latency.hpp"
#include "core/oms.hpp"
#include "rest/order_routes.hpp"

#include <crow_all.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace gateway;

/// Real-clock, report-only latency benchmark of the two windows the
/// LatencyLog already instruments in the OMS (no assertions on speed —
/// CI machines vary; the numbers are printed for humans):
///   place_send_rest    POST /orders handler entry -> just before the
///                      venue place call (JSON parse + validation +
///                      risk projection + pending staging [+ persist])
///   place_send_oms     OMS place() entry -> just before the venue call
///   fill_state_update  execution report received -> registry updated
///                      and the state event persisted
/// The venue itself is an instant in-memory connector, so the windows
/// contain gateway processing only. Real HTTP is exercised end to end
/// (in-process Crow server + loopback client); Crow's socket/HTTP
/// parsing happens before the handler-entry stamp and is excluded by
/// design. Determinism rules (testing-guidelines) still hold for what
/// IS asserted: exactly one measurement per measured order per phase.
constexpr int kPreloadedOrders = 100;
constexpr int kWarmupOrders = 50;
constexpr int kMeasuredOrders = 500;

/// Instant in-memory venue: the place acks synchronously.
class FakeConnector final : public ExchangeConnector
{
  public:
    [[nodiscard]] auto place_order(const OrderRequest& a_request) -> Result<OrderPlacement> override
    {
        return OrderPlacement{.client_order_id = a_request.client_order_id,
                              .exchange_order_id = "ord-" + a_request.client_order_id};
    }
    [[nodiscard]] auto cancel_order(const CancelRequest&) -> Result<OrderPlacement> override
    {
        return Error{"not_found", "unsupported in this benchmark"};
    }
    [[nodiscard]] auto amend_order(const AmendRequest&) -> Result<OrderPlacement> override
    {
        return Error{"not_found", "unsupported in this benchmark"};
    }
    [[nodiscard]] auto get_order(const OrderQuery&) -> Result<std::optional<OrderSnapshot>> override
    {
        return Result<std::optional<OrderSnapshot>>{std::optional<OrderSnapshot>{std::nullopt}};
    }
    [[nodiscard]] auto get_open_orders() -> Result<std::vector<OrderSnapshot>> override
    {
        return std::vector<OrderSnapshot>{};
    }
    [[nodiscard]] auto get_price(const std::string&) -> Result<std::string> override
    {
        return Result<std::string>{std::string{"stub-price"}};
    }
    void set_execution_report_handler(std::function<void(const ExecutionReport&)>) override {}
    void set_connectivity_handler(std::function<void(bool)>) override {}
    void start() override {}
    void stop() override {}
};

auto temp_path(const char* a_name) -> std::filesystem::path
{
    return std::filesystem::temp_directory_path() /
           (std::string{"gateway_latency_bench_"} + a_name);
}

auto place_request(const std::string& a_id) -> OrderRequest
{
    return OrderRequest{.client_order_id = a_id,
                        .instrument_id = "BTC-USDT",
                        .side = Side::Buy,
                        .type = OrderType::Limit,
                        .price = "50000",
                        .quantity = "1",
                        .time_in_force = ""};
}

auto place_body(const std::string& a_id) -> std::string
{
    return R"({"clientOrderId":")" + a_id +
           R"(","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"1"})";
}

auto fill_report(const std::string& a_id) -> ExecutionReport
{
    return ExecutionReport{.client_order_id = a_id,
                           .exchange_order_id = "ord-" + a_id,
                           .state = OrderState::Filled,
                           .side = Side::Buy,
                           .filled_quantity = "1",
                           .average_fill_price = "50000"};
}

struct PhaseStats
{
    std::size_t count = 0;
    std::int64_t p50_ns = 0;
    std::int64_t p95_ns = 0;
    std::int64_t p99_ns = 0;
    std::int64_t max_ns = 0;
    double mean_ns = 0.0;
};

/// Nearest-rank percentile of an ascending-sorted sample.
auto percentile(const std::vector<std::int64_t>& a_sorted, double a_p) -> std::int64_t
{
    const auto rank =
        static_cast<std::size_t>(std::ceil(a_p / 100.0 * static_cast<double>(a_sorted.size())));
    return a_sorted[std::clamp(rank, std::size_t{1}, a_sorted.size()) - 1];
}

auto stats_of(std::vector<std::int64_t> a_samples) -> PhaseStats
{
    std::sort(a_samples.begin(), a_samples.end());
    PhaseStats out;
    out.count = a_samples.size();
    if (a_samples.empty()) {
        return out;
    }
    out.p50_ns = percentile(a_samples, 50);
    out.p95_ns = percentile(a_samples, 95);
    out.p99_ns = percentile(a_samples, 99);
    out.max_ns = a_samples.back();
    double sum = 0.0;
    for (const auto sample : a_samples) {
        sum += static_cast<double>(sample);
    }
    out.mean_ns = sum / static_cast<double>(a_samples.size());
    return out;
}

auto read_lines(const std::filesystem::path& a_path) -> std::vector<nlohmann::json>
{
    std::vector<nlohmann::json> lines;
    std::ifstream file(a_path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(nlohmann::json::parse(line));
        }
    }
    return lines;
}

/// One full benchmark run. Registry hygiene for honest numbers:
/// - 'p' prefix: kPreloadedOrders terminal orders preloaded directly
///   through the OMS, so the pre-trade risk projection inside every
///   measured place pays a steady, representative registry scan
/// - 'w' prefix: HTTP warmup (allocator, Crow routing, connection)
/// - 'm' prefix: the measured 500 — the only ids aggregated
auto run_variant(bool a_with_persistence, const std::filesystem::path& a_latency_path,
                 const std::filesystem::path& a_event_path) -> std::map<std::string, PhaseStats>
{
    std::filesystem::remove(a_latency_path);
    std::filesystem::remove(a_event_path);

    // Limits every candidate passes: the risk step RUNS (projection
    // over the registry) but never rejects.
    const auto risk = risk_config_from_json(nlohmann::json::parse(
        R"({"instruments":{"BTC-USDT":{"maxQty":"1000","maxNotional":"100000000","maxPosition":"1000000"}}})"));
    REQUIRE(risk.is_ok());

    FakeConnector connector;
    std::optional<EventLog> event_log;
    if (a_with_persistence) {
        event_log.emplace(a_event_path);
    }
    LatencyLog latency{a_latency_path}; // production steady clock, no mocking
    OrderManagementSystem oms{{{"okx", &connector}},
                              event_log.has_value() ? &event_log.value() : nullptr,
                              risk.value(),
                              "okx",
                              &latency};

    for (int i = 0; i < kPreloadedOrders; ++i) {
        const auto id = "p" + std::to_string(i);
        REQUIRE(oms.place(place_request(id)).is_ok());
        oms.on_execution_report(fill_report(id));
    }

    crow::SimpleApp app;
    gateway::rest::register_order_routes(app, oms);
    const auto port = gateway::testing::pick_free_port();
    auto server = app.port(static_cast<int>(port))
                      .concurrency(1)
                      .loglevel(crow::LogLevel::Warning)
                      .run_async();
    app.wait_for_server_start();
    httplib::Client client{"127.0.0.1", static_cast<int>(port)};

    const auto post_place = [&](const std::string& a_id) {
        const auto res = client.Post("/orders", place_body(a_id), "application/json");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 201);
    };

    for (int i = 0; i < kWarmupOrders; ++i) {
        post_place("w" + std::to_string(i));
    }
    for (int i = 0; i < kMeasuredOrders; ++i) {
        const auto id = "m" + std::to_string(i);
        post_place(id);
        // the fill drives the same path a venue feed thread would
        oms.on_execution_report(fill_report(id));
    }

    app.stop();
    server.wait();

    std::map<std::string, std::vector<std::int64_t>> samples;
    for (const auto& line : read_lines(a_latency_path)) {
        const auto id = line.at("clientOrderId").get<std::string>();
        if (id.empty() || id.front() != 'm') {
            continue;
        }
        samples[line.at("phase").get<std::string>()].push_back(
            line.at("elapsedNs").get<std::int64_t>());
    }

    std::map<std::string, PhaseStats> result;
    for (auto& [phase, values] : samples) {
        result.emplace(phase, stats_of(std::move(values)));
    }

    // Speed-independent plumbing checks: every measured order produced
    // exactly one measurement of each phase.
    REQUIRE(result.count("place_send_rest") == 1);
    REQUIRE(result.count("place_send_oms") == 1);
    REQUIRE(result.count("fill_state_update") == 1);
    CHECK(result.at("place_send_rest").count == static_cast<std::size_t>(kMeasuredOrders));
    CHECK(result.at("place_send_oms").count == static_cast<std::size_t>(kMeasuredOrders));
    CHECK(result.at("fill_state_update").count == static_cast<std::size_t>(kMeasuredOrders));

    std::filesystem::remove(a_latency_path);
    std::filesystem::remove(a_event_path);
    return result;
}

auto micros(std::int64_t a_ns) -> double
{
    return static_cast<double>(a_ns) / 1000.0;
}

void print_rows(const std::string& a_variant, const std::map<std::string, PhaseStats>& a_stats)
{
    for (const auto& [phase, stats] : a_stats) {
        std::cout << std::format("{:<14} {:<18} {:>9.1f} {:>9.1f} {:>9.1f} {:>9.1f} {:>9.1f}\n",
                                 a_variant, phase, micros(stats.p50_ns), micros(stats.p95_ns),
                                 micros(stats.p99_ns), micros(stats.max_ns),
                                 stats.mean_ns / 1000.0);
    }
}

} // namespace

TEST_CASE("latency benchmark: REST hit -> venue send and fill -> state update (report-only)")
{
    std::cout << std::format(
        "gateway latency benchmark — real steady clock, microseconds; {} measured orders per "
        "variant ({} warmup, {} preloaded registry orders, risk enabled, instant venue)\n",
        kMeasuredOrders, kWarmupOrders, kPreloadedOrders);
    std::cout << std::format("{:<14} {:<18} {:>9} {:>9} {:>9} {:>9} {:>9}\n", "variant", "phase",
                             "p50", "p95", "p99", "max", "mean");

    const auto no_persistence =
        run_variant(false, temp_path("nopers_latency.jsonl"), temp_path("nopers_events.jsonl"));
    print_rows("no-persistence", no_persistence);

    const auto persistence =
        run_variant(true, temp_path("pers_latency.jsonl"), temp_path("pers_events.jsonl"));
    print_rows("persistence", persistence);
}
