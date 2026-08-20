#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "exchange/binance/binance_signer.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace {

using gateway::exchange::binance::sign_params;
using gateway::exchange::binance::signature_payload;

// Official HMAC signing example from the Binance Spot WebSocket API docs.
constexpr auto kDocApiKey = "vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A";
constexpr auto kDocSecret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
const std::string kDocSignature =
    "aa1b5712c094bc4e57c05a1a5c1fd8d88dcd628338ea863fec7b88e59fe2db24";

auto doc_params() -> nlohmann::json
{
    // insertion order deliberately different from the sorted payload:
    // the signature must be order-independent
    return nlohmann::json{{"symbol", "BTCUSDT"},      {"side", "SELL"},
                          {"type", "LIMIT"},          {"timeInForce", "GTC"},
                          {"quantity", "0.01000000"}, {"price", "52000.00"},
                          {"recvWindow", 100},        {"timestamp", 1645423376532},
                          {"apiKey", kDocApiKey}};
}

} // namespace

TEST_CASE("sign_params reproduces the official documentation vector")
{
    CHECK(sign_params(doc_params(), kDocSecret) == kDocSignature);
}

TEST_CASE("signature_payload sorts params alphabetically and joins with &")
{
    CHECK(signature_payload(doc_params()) ==
          "apiKey=vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A"
          "&price=52000.00&quantity=0.01000000&recvWindow=100&side=SELL&symbol=BTCUSDT"
          "&timeInForce=GTC&timestamp=1645423376532&type=LIMIT");
}

TEST_CASE("the signature parameter itself is excluded from the payload")
{
    auto params = doc_params();
    params["signature"] = "should-not-be-signed";
    CHECK(signature_payload(params) == signature_payload(doc_params()));
}

TEST_CASE("empty params sign to the HMAC of an empty payload")
{
    const std::string empty = sign_params(nlohmann::json::object(), "secret");
    CHECK_FALSE(empty.empty());
    // determinism
    CHECK(empty == sign_params(nlohmann::json::object(), "secret"));
    // changes with the secret
    CHECK(empty != sign_params(nlohmann::json::object(), "other"));
}

TEST_CASE("signing is deterministic and key-dependent")
{
    const auto first = sign_params(doc_params(), kDocSecret);
    const auto second = sign_params(doc_params(), kDocSecret);
    CHECK(first == second);
    CHECK(first != sign_params(doc_params(), "wrong-secret"));
}
