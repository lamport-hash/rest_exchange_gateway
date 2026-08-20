#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/config.hpp"
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

} // namespace
