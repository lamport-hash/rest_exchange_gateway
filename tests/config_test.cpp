#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/config.hpp"
#include "exchange/binance/binance_config.hpp"
#include "exchange/okx/okx_rest_client.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

class TempConfigFile
{
  public:
    explicit TempConfigFile(const std::string& a_content)
        : path_(std::filesystem::temp_directory_path() /
                ("gateway_config_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter_++) + ".json"))
    {
        std::ofstream file(path_);
        file << a_content;
    }

    ~TempConfigFile()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

  private:
    static inline int counter_ = 0;
    std::filesystem::path path_;
};

TEST_CASE("load_config accepts a full valid config")
{
    const TempConfigFile file{R"({
        "rest": {"port": 8080},
        "okx": {"apiKey": "k", "secretKey": "s", "passphrase": "p", "demoTrading": true}
    })"};
    const auto result = gateway::load_config(file.path());
    REQUIRE(result.is_ok());
    CHECK(result.value().rest_port == 8080);
    CHECK(result.value().okx.at("apiKey") == "k");
}

TEST_CASE("load_config parses persistence and risk sections")
{
    const TempConfigFile file{R"({
        "persistence": {"logPath": "data/orders.jsonl"},
        "risk": {
            "default": {"maxQty": "10", "maxNotional": "1000000", "maxPosition": "5"},
            "instruments": {"BTC-USDT": {"maxQty": "1"}}
        }
    })"};
    const auto result = gateway::load_config(file.path());
    REQUIRE(result.is_ok());
    REQUIRE(result.value().persistence_log.has_value());
    CHECK(result.value().persistence_log->string() == "data/orders.jsonl");
    const auto limits = result.value().risk.limits_for("BTC-USDT");
    REQUIRE(limits.has_value());
    CHECK(limits->max_qty == "1");
    CHECK(result.value().risk.limits_for("ETH-USDT")->max_qty == "10");
}

TEST_CASE("load_config rejects malformed persistence and risk sections")
{
    const TempConfigFile bad_persistence{R"({"persistence": {"logPath": 5}})"};
    CHECK_FALSE(gateway::load_config(bad_persistence.path()).is_ok());

    const TempConfigFile missing_path{R"({"persistence": {}})"};
    CHECK_FALSE(gateway::load_config(missing_path.path()).is_ok());

    const TempConfigFile bad_risk{R"({"risk": {"default": {"maxQty": "soon"}}})"};
    const auto result = gateway::load_config(bad_risk.path());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("a config without persistence or risk keeps them disabled/unlimited")
{
    const TempConfigFile file{R"({"okx": {}})"};
    const auto result = gateway::load_config(file.path());
    REQUIRE(result.is_ok());
    CHECK_FALSE(result.value().persistence_log.has_value());
    CHECK_FALSE(result.value().risk.limits_for("BTC-USDT").has_value());
}

TEST_CASE("load_config applies defaults for a minimal config")
{
    const TempConfigFile file{R"({"okx": {}})"};
    const auto result = gateway::load_config(file.path());
    REQUIRE(result.is_ok());
    CHECK(result.value().rest_port == 8080);
    CHECK(result.value().okx.is_object());
}

TEST_CASE("load_config rejects an empty file")
{
    const TempConfigFile file{""};
    const auto result = gateway::load_config(file.path());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("load_config rejects malformed JSON")
{
    const TempConfigFile file{R"({"rest": {"port": }})"};
    const auto result = gateway::load_config(file.path());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("load_config rejects a non-object root")
{
    const TempConfigFile file{R"([1, 2, 3])"};
    const auto result = gateway::load_config(file.path());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("load_config rejects out-of-range rest port")
{
    const TempConfigFile file{R"({"rest": {"port": 65536}})"};
    const auto result = gateway::load_config(file.path());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("load_config rejects non-numeric rest port")
{
    const TempConfigFile file{R"({"rest": {"port": "8080"}})"};
    const auto result = gateway::load_config(file.path());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("load_config rejects non-object okx section")
{
    const TempConfigFile file{R"({"okx": 5})"};
    const auto result = gateway::load_config(file.path());
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("load_config reports missing files as io errors")
{
    const auto result = gateway::load_config("/nonexistent/gateway_config.json");
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "io");
}

TEST_CASE("load_config parses the audit interval with a 30s default")
{
    const TempConfigFile without{R"({"okx": {}})"};
    const auto defaults = gateway::load_config(without.path());
    REQUIRE(defaults.is_ok());
    CHECK(defaults.value().audit_interval_ms == 30000);

    const TempConfigFile zero{R"({"audit": {"intervalMs": 0}})"};
    const auto disabled = gateway::load_config(zero.path());
    REQUIRE(disabled.is_ok());
    CHECK(disabled.value().audit_interval_ms == 0);

    const TempConfigFile custom{R"({"audit": {"intervalMs": 5000}})"};
    const auto parsed = gateway::load_config(custom.path());
    REQUIRE(parsed.is_ok());
    CHECK(parsed.value().audit_interval_ms == 5000);
}

TEST_CASE("load_config rejects malformed audit sections")
{
    const TempConfigFile missing{R"({"audit": {}})"};
    const auto no_interval = gateway::load_config(missing.path());
    REQUIRE_FALSE(no_interval.is_ok());
    CHECK(no_interval.error().code == "protocol");

    const TempConfigFile stringy{R"({"audit": {"intervalMs": "30000"}})"};
    CHECK_FALSE(gateway::load_config(stringy.path()).is_ok());

    const TempConfigFile negative{R"({"audit": {"intervalMs": -1}})"};
    CHECK_FALSE(gateway::load_config(negative.path()).is_ok());

    const TempConfigFile huge{R"({"audit": {"intervalMs": 86400001}})"};
    const auto out_of_range = gateway::load_config(huge.path());
    REQUIRE_FALSE(out_of_range.is_ok());
    CHECK(out_of_range.error().code == "protocol");

    const TempConfigFile not_object{R"({"audit": 30000})"};
    CHECK_FALSE(gateway::load_config(not_object.path()).is_ok());
}

TEST_CASE("load_config parses the optional latency log section")
{
    const TempConfigFile with{R"({"latency": {"logPath": "data/latency.jsonl"}})"};
    const auto parsed = gateway::load_config(with.path());
    REQUIRE(parsed.is_ok());
    REQUIRE(parsed.value().latency_log.has_value());
    CHECK(parsed.value().latency_log->string() == "data/latency.jsonl");

    const TempConfigFile without{R"({"okx": {}})"};
    const auto defaults = gateway::load_config(without.path());
    REQUIRE(defaults.is_ok());
    CHECK_FALSE(defaults.value().latency_log.has_value());
}

TEST_CASE("load_config rejects malformed latency sections")
{
    const TempConfigFile not_object{R"({"latency": "data/latency.jsonl"})"};
    CHECK_FALSE(gateway::load_config(not_object.path()).is_ok());

    const TempConfigFile missing_path{R"({"latency": {}})"};
    CHECK_FALSE(gateway::load_config(missing_path.path()).is_ok());

    const TempConfigFile wrong_type{R"({"latency": {"logPath": 5}})"};
    CHECK_FALSE(gateway::load_config(wrong_type.path()).is_ok());
}

TEST_CASE("okx_config_from_json accepts a complete section")
{
    const auto section = nlohmann::json::parse(R"({
        "apiKey": "key", "secretKey": "secret", "passphrase": "pass",
        "host": "127.0.0.1", "port": 8080, "useTls": false, "demoTrading": true
    })");
    const auto result = gateway::exchange::okx::okx_config_from_json(section);
    REQUIRE(result.is_ok());
    const auto& config = result.value();
    CHECK(config.api_key == "key");
    CHECK(config.secret_key == "secret");
    CHECK(config.passphrase == "pass");
    CHECK(config.host == "127.0.0.1");
    CHECK(config.port == 8080);
    CHECK(config.use_tls == false);
    CHECK(config.demo_trading == true);
}

TEST_CASE("okx_config_from_json applies production defaults")
{
    const auto section =
        nlohmann::json::parse(R"({"apiKey":"k","secretKey":"s","passphrase":"p"})");
    const auto result = gateway::exchange::okx::okx_config_from_json(section);
    REQUIRE(result.is_ok());
    CHECK(result.value().host == "www.okx.com");
    CHECK(result.value().port == 443);
    CHECK(result.value().use_tls);
    CHECK_FALSE(result.value().demo_trading);
}

TEST_CASE("okx_config_from_json lists every missing credential")
{
    const auto section = nlohmann::json::parse(R"({"apiKey": 5})");
    const auto result = gateway::exchange::okx::okx_config_from_json(section);
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
    CHECK(result.error().message.find("secretKey") != std::string::npos);
    CHECK(result.error().message.find("passphrase") != std::string::npos);
    CHECK(result.error().message.find("apiKey") != std::string::npos);
}

TEST_CASE("okx_config_from_json rejects wrong types")
{
    const auto bad_port =
        nlohmann::json::parse(R"({"apiKey":"k","secretKey":"s","passphrase":"p","port":"443"})");
    const auto bad_tls =
        nlohmann::json::parse(R"({"apiKey":"k","secretKey":"s","passphrase":"p","useTls":"yes"})");
    const auto bad_demo =
        nlohmann::json::parse(R"({"apiKey":"k","secretKey":"s","passphrase":"p","demoTrading":1})");

    for (const auto* section : {&bad_port, &bad_tls, &bad_demo}) {
        const auto result = gateway::exchange::okx::okx_config_from_json(*section);
        REQUIRE_FALSE(result.is_ok());
        CHECK(result.error().code == "protocol");
    }
}

TEST_CASE("okx_config_from_json rejects non-object sections")
{
    const auto section = nlohmann::json::parse(R"("nope")");
    const auto result = gateway::exchange::okx::okx_config_from_json(section);
    REQUIRE_FALSE(result.is_ok());
    CHECK(result.error().code == "protocol");
}

TEST_CASE("okx_config_from_json parses the retry section")
{
    const auto section = nlohmann::json::parse(R"({
        "apiKey": "k", "secretKey": "s", "passphrase": "p",
        "retry": {"maxAttempts": 5, "initialBackoffMs": 50, "maxBackoffMs": 700,
                  "multiplier": 3.0, "jitter": 0.25, "budgetMs": 9000}
    })");
    const auto result = gateway::exchange::okx::okx_config_from_json(section);
    REQUIRE(result.is_ok());
    const auto& retry = result.value().retry;
    CHECK(retry.max_attempts == 5);
    CHECK(retry.initial_backoff == std::chrono::milliseconds{50});
    CHECK(retry.max_backoff == std::chrono::milliseconds{700});
    CHECK(retry.multiplier == 3.0);
    CHECK(retry.jitter == 0.25);
    CHECK(retry.budget == std::chrono::milliseconds{9000});
}

TEST_CASE("okx retry defaults apply without a retry section")
{
    const auto section =
        nlohmann::json::parse(R"({"apiKey":"k","secretKey":"s","passphrase":"p"})");
    const auto result = gateway::exchange::okx::okx_config_from_json(section);
    REQUIRE(result.is_ok());
    const auto& retry = result.value().retry;
    CHECK(retry.max_attempts == 3);
    CHECK(retry.initial_backoff == std::chrono::milliseconds{100});
    CHECK(retry.max_backoff == std::chrono::milliseconds{2000});
    CHECK(retry.budget == std::chrono::milliseconds{5000});
}

TEST_CASE("okx retry section validation errors surface")
{
    const auto parse = [](const std::string& a_retry) {
        return gateway::exchange::okx::okx_config_from_json(nlohmann::json::parse(
            R"({"apiKey":"k","secretKey":"s","passphrase":"p","retry":)" + a_retry + "}"));
    };
    CHECK_FALSE(parse(R"({"maxAttempts":0})").is_ok());
    CHECK_FALSE(parse(R"({"jitter":2.0})").is_ok());
    CHECK_FALSE(parse(R"([1])").is_ok());
}

TEST_CASE("okx_config_from_json parses the ws section")
{
    const auto section = nlohmann::json::parse(R"({
        "apiKey": "k", "secretKey": "s", "passphrase": "p",
        "ws": {"enabled": false, "host": "127.0.0.1", "port": 9001, "useTls": false,
               "path": "/ws/v5/private", "pingIntervalMs": 15000, "maxMissedPongs": 3}
    })");
    const auto result = gateway::exchange::okx::okx_config_from_json(section);
    REQUIRE(result.is_ok());
    const auto& ws = result.value().ws;
    CHECK_FALSE(ws.enabled);
    CHECK(ws.host == "127.0.0.1");
    CHECK(ws.port == 9001);
    CHECK_FALSE(ws.use_tls);
    CHECK(ws.path == "/ws/v5/private");
    CHECK(ws.ping_interval == std::chrono::milliseconds{15000});
    CHECK(ws.max_missed_pongs == 3);
}

TEST_CASE("okx ws defaults point at the production private endpoint")
{
    const auto section =
        nlohmann::json::parse(R"({"apiKey":"k","secretKey":"s","passphrase":"p"})");
    const auto result = gateway::exchange::okx::okx_config_from_json(section);
    REQUIRE(result.is_ok());
    const auto& ws = result.value().ws;
    CHECK(ws.enabled);
    CHECK(ws.host == "ws.okx.com");
    CHECK(ws.port == 8443);
    CHECK(ws.use_tls);
    CHECK(ws.path == "/ws/v5/private");
    CHECK(ws.ping_interval == std::chrono::milliseconds{20000});
    CHECK(ws.max_missed_pongs == 2);
}

TEST_CASE("okx ws section validation errors surface")
{
    const auto parse = [](const std::string& a_ws) {
        return gateway::exchange::okx::okx_config_from_json(nlohmann::json::parse(
            R"({"apiKey":"k","secretKey":"s","passphrase":"p","ws":)" + a_ws + "}"));
    };
    CHECK_FALSE(parse(R"({"enabled":"yes"})").is_ok());
    CHECK_FALSE(parse(R"({"port":0})").is_ok());
    CHECK_FALSE(parse(R"({"port":70000})").is_ok());
    CHECK_FALSE(parse(R"({"useTls":1})").is_ok());
    CHECK_FALSE(parse(R"({"pingIntervalMs":0})").is_ok());
    CHECK_FALSE(parse(R"({"maxMissedPongs":0})").is_ok());
    CHECK_FALSE(parse(R"("nope")").is_ok());
}

TEST_CASE("okx rest timeout overrides are validated")
{
    const auto parse = [](const std::string& a_extra) {
        return gateway::exchange::okx::okx_config_from_json(nlohmann::json::parse(
            R"({"apiKey":"k","secretKey":"s","passphrase":"p",)" + a_extra + "}"));
    };
    const auto ok = parse(R"("restConnectTimeoutMs":250,"restReadTimeoutMs":300)");
    REQUIRE(ok.is_ok());
    CHECK(ok.value().rest_connect_timeout_ms == 250);
    CHECK(ok.value().rest_read_timeout_ms == 300);

    CHECK_FALSE(parse(R"("restReadTimeoutMs":0)").is_ok());
    CHECK_FALSE(parse(R"("restReadTimeoutMs":"500")").is_ok());
    CHECK_FALSE(parse(R"("restConnectTimeoutMs":-1)").is_ok());
}

TEST_CASE("binance ws watchdog defaults enable the liveness check")
{
    // Regression: httplib defaults to max_missed_pongs = 0, which
    // DISABLES pong-timeout detection — a silent socket then stalls
    // until the 300s OS read timeout. The gateway default must keep the
    // watchdog armed.
    const auto section =
        nlohmann::json::parse(R"({"apiKey":"k","secretKey":"s"})");
    const auto result = gateway::exchange::binance::binance_config_from_json(section);
    REQUIRE(result.is_ok());
    CHECK(result.value().ws_ping_interval_s == 20);
    CHECK(result.value().ws_max_missed_pongs == 2);
}

TEST_CASE("binance ws watchdog overrides are parsed and validated")
{
    const auto parse = [](const std::string& a_extra) {
        return gateway::exchange::binance::binance_config_from_json(nlohmann::json::parse(
            R"({"apiKey":"k","secretKey":"s",)" + a_extra + "}"));
    };
    const auto ok = parse(R"("wsPingIntervalSec":5,"wsMaxMissedPongs":4)");
    REQUIRE(ok.is_ok());
    CHECK(ok.value().ws_ping_interval_s == 5);
    CHECK(ok.value().ws_max_missed_pongs == 4);

    CHECK_FALSE(parse(R"("wsPingIntervalSec":0)").is_ok());
    CHECK_FALSE(parse(R"("wsPingIntervalSec":301)").is_ok());
    CHECK_FALSE(parse(R"("wsMaxMissedPongs":0)").is_ok());
    CHECK_FALSE(parse(R"("wsMaxMissedPongs":"2")").is_ok());
}

} // namespace
