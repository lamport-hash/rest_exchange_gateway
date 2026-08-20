#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "exchange/okx/okx_signer.hpp"

#include <string>

namespace {

using gateway::exchange::okx::sign_request;

TEST_CASE("sign_request matches openssl-generated HMAC vectors (normal cases)")
{
    CHECK(sign_request("2026-08-20T10:00:00.000Z", "GET",
                       "/api/v5/trade/order?instId=BTC-USDT&clOrdId=gw0001", "",
                       "22582BD0CFF14C41EDBF1AB98506286D") ==
          "wgWpMOc0b32OTt4ttNe9DD32Dk+Z/eor9JN7ArnIPGU=");

    CHECK(
        sign_request(
            "2026-08-20T10:05:30.250Z", "POST", "/api/v5/trade/order",
            R"({"instId":"BTC-USDT","tdMode":"cash","side":"buy","ordType":"limit","px":"50000","sz":"0.001","clOrdId":"gw0001"})",
            "my-api-secret") == "UgxlWhTv+SexlUq7iBAo9/zsjMRXsibfdUp9zRWTFXU=");
}

TEST_CASE("sign_request handles edge-case inputs")
{
    CHECK(sign_request("1970-01-01T00:00:00.000Z", "GET", "/", "", "s3cr3t-K") ==
          "Bzoo0KzvWKq+SUIEmt/Q5Ef7i59WhBW1OjoS3S1y85Q=");

    CHECK(sign_request("2026-08-20T10:00:00.000Z", "GET", "/api/v5/account/balance", "", "") ==
          "ETZmY5wGi9NRYqHeyC8jHd091twsFHC+t7zXhgiPeDk=");
}

TEST_CASE("sign_request is deterministic and keyed")
{
    const auto first = sign_request("t", "GET", "/p", "b", "secret-one");
    const auto second = sign_request("t", "GET", "/p", "b", "secret-one");
    CHECK(first == second);

    const auto other_key = sign_request("t", "GET", "/p", "b", "secret-two");
    CHECK(first != other_key);
}

TEST_CASE("sign_request output is always 44-char base64")
{
    const auto sig = sign_request("ts", "POST", "/x", "body", "k");
    CHECK(sig.size() == 44);
    CHECK(sig.back() == '=');
}

TEST_CASE("sign_ws_login is the documented /users/self/verify signature")
{
    using gateway::exchange::okx::sign_ws_login;
    // sign_ws_login(ts, secret) == sign_request(ts, "GET", "/users/self/verify", "", secret)
    CHECK(sign_ws_login("2026-08-20T10:00:00.000Z", "22582BD0CFF14C41EDBF1AB98506286D") ==
          sign_request("2026-08-20T10:00:00.000Z", "GET", "/users/self/verify", "",
                       "22582BD0CFF14C41EDBF1AB98506286D"));
    CHECK_FALSE(sign_ws_login("ts", "a").empty());
    CHECK(sign_ws_login("ts", "a").size() == 44);
}

} // namespace
