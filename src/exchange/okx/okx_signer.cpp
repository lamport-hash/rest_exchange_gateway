#include "exchange/okx/okx_signer.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <string>

namespace gateway::exchange::okx {

namespace {
constexpr std::size_t kSha256Len = 32;
constexpr std::size_t kBase64Len = 4 * ((kSha256Len + 2) / 3);

auto base64_encode(const unsigned char* a_data, std::size_t a_len) -> std::string
{
    std::string out(kBase64Len, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), a_data,
                                        static_cast<int>(a_len));
    out.resize(static_cast<std::size_t>(written));
    return out;
}
} // namespace

auto sign_request(std::string_view a_timestamp, std::string_view a_method, std::string_view a_path,
                  std::string_view a_body, std::string_view a_secret) -> std::string
{
    std::string prehash;
    prehash.reserve(a_timestamp.size() + a_method.size() + a_path.size() + a_body.size());
    prehash.append(a_timestamp);
    prehash.append(a_method);
    prehash.append(a_path);
    prehash.append(a_body);

    std::array<unsigned char, kSha256Len> mac{};
    unsigned int mac_len = 0;
    const unsigned char* result =
        HMAC(EVP_sha256(), a_secret.data(), static_cast<int>(a_secret.size()),
             reinterpret_cast<const unsigned char*>(prehash.data()),
             static_cast<int>(prehash.size()), mac.data(), &mac_len);
    if (result == nullptr || mac_len != kSha256Len) {
        return {};
    }
    return base64_encode(mac.data(), mac_len);
}

} // namespace gateway::exchange::okx
