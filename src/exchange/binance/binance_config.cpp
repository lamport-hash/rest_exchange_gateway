#include "exchange/binance/binance_config.hpp"

#include <string>
#include <vector>

namespace gateway::exchange::binance {

auto binance_config_from_json(const nlohmann::json& a_section) -> Result<BinanceConfig>
{
    if (!a_section.is_object()) {
        return Error{"protocol", "binance section must be a JSON object"};
    }

    std::vector<std::string> problems;
    BinanceConfig config;

    const auto read_string = [&a_section, &problems](const char* a_name, std::string& a_target,
                                                     bool a_required) {
        const auto it = a_section.find(a_name);
        if (it == a_section.end() || it->is_null()) {
            if (a_required) {
                problems.push_back(std::string{"missing "} + a_name);
            }
            return;
        }
        if (!it->is_string()) {
            problems.push_back(std::string{a_name} + " must be a string");
            return;
        }
        a_target = it->get<std::string>();
    };

    read_string("apiKey", config.api_key, true);
    read_string("secretKey", config.secret_key, true);
    read_string("host", config.host, false);
    read_string("path", config.path, false);

    if (const auto it = a_section.find("port"); it != a_section.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<long long>() < 1 || it->get<long long>() > 65535) {
            problems.push_back("port must be an integer in [1, 65535]");
        } else {
            config.port = static_cast<int>(it->get<long long>());
        }
    }
    if (const auto it = a_section.find("useTls"); it != a_section.end() && !it->is_null()) {
        if (!it->is_boolean()) {
            problems.push_back("useTls must be a boolean");
        } else {
            config.use_tls = it->get<bool>();
        }
    }
    if (const auto it = a_section.find("recvWindowMs"); it != a_section.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<long long>() < 0 || it->get<long long>() > 60000) {
            problems.push_back("recvWindowMs must be an integer in [0, 60000]");
        } else {
            config.recv_window_ms = it->get<long long>();
        }
    }
    if (const auto it = a_section.find("requestTimeoutMs");
        it != a_section.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<long long>() <= 0) {
            problems.push_back("requestTimeoutMs must be a positive integer");
        } else {
            config.request_timeout = std::chrono::milliseconds{it->get<long long>()};
        }
    }
    if (const auto it = a_section.find("wsPingIntervalSec");
        it != a_section.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<long long>() < 1 || it->get<long long>() > 300) {
            problems.push_back("wsPingIntervalSec must be an integer in [1, 300]");
        } else {
            config.ws_ping_interval_s = static_cast<int>(it->get<long long>());
        }
    }
    if (const auto it = a_section.find("wsMaxMissedPongs");
        it != a_section.end() && !it->is_null()) {
        if (!it->is_number_integer() || it->get<long long>() < 1 || it->get<long long>() > 100) {
            problems.push_back("wsMaxMissedPongs must be an integer in [1, 100]");
        } else {
            config.ws_max_missed_pongs = static_cast<int>(it->get<long long>());
        }
    }
    if (const auto it = a_section.find("retry"); it != a_section.end() && !it->is_null()) {
        auto retry = retry_policy_from_json(*it, config.retry);
        if (!retry.is_ok()) {
            return retry.error();
        }
        config.retry = retry.value();
    }

    if (!problems.empty()) {
        std::string message = "invalid binance config:";
        for (const auto& problem : problems) {
            message += " ";
            message += problem;
            message += ";";
        }
        return Error{"protocol", message};
    }
    return config;
}

} // namespace gateway::exchange::binance
