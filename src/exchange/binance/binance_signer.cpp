#include "exchange/binance/binance_signer.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <map>
#include <string_view>

namespace gateway::exchange::binance {

namespace {

constexpr std::size_t kSha256Len = 32;

auto hex_encode(const unsigned char* a_data, std::size_t a_len) -> std::string
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(a_len * 2);
    for (std::size_t i = 0; i < a_len; ++i) {
        out[2 * i] = kDigits[a_data[i] >> 4];
        out[2 * i + 1] = kDigits[a_data[i] & 0x0F];
    }
    return out;
}

/// Text form of a param value: strings verbatim, integers without padding.
/// Floating point values are refused (the gateway signs exact decimal
/// strings, never floats).
auto value_text(const nlohmann::json& a_value) -> std::string
{
    if (a_value.is_string()) {
        return a_value.get<std::string>();
    }
    if (a_value.is_number_integer()) {
        return std::to_string(a_value.get<long long>());
    }
    if (a_value.is_boolean()) {
        return a_value.get<bool>() ? "true" : "false";
    }
    return {};
}

} // namespace

auto signature_payload(const nlohmann::json& a_params) -> std::string
{
    std::map<std::string, std::string> sorted;
    for (auto it = a_params.begin(); it != a_params.end(); ++it) {
        if (it.key() == "signature") {
            continue;
        }
        sorted[it.key()] = value_text(it.value());
    }

    std::string payload;
    for (const auto& [name, value] : sorted) {
        if (!payload.empty()) {
            payload += '&';
        }
        payload += name;
        payload += '=';
        payload += value;
    }
    return payload;
}

auto sign_params(const nlohmann::json& a_params, std::string_view a_secret) -> std::string
{
    const std::string payload = signature_payload(a_params);

    std::array<unsigned char, kSha256Len> mac{};
    unsigned int mac_len = 0;
    const unsigned char* result =
        HMAC(EVP_sha256(), a_secret.data(), static_cast<int>(a_secret.size()),
             reinterpret_cast<const unsigned char*>(payload.data()),
             static_cast<int>(payload.size()), mac.data(), &mac_len);
    if (result == nullptr || mac_len != kSha256Len) {
        return {};
    }
    return hex_encode(mac.data(), mac_len);
}

} // namespace gateway::exchange::binance
