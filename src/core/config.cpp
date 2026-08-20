#include "core/config.hpp"

#include <fstream>

namespace gateway {

namespace {
constexpr std::uint16_t kMinPort = 1;
constexpr std::uint16_t kMaxPort = 65535;
} // namespace

auto load_config(const std::filesystem::path& a_path) -> Result<GatewayConfig>
{
    std::ifstream file(a_path);
    if (!file.is_open()) {
        return Error{"io", "cannot open config file: " + a_path.string()};
    }

    const auto root = nlohmann::json::parse(file, nullptr, false);
    if (root.is_discarded()) {
        return Error{"protocol", "config file is not valid JSON: " + a_path.string()};
    }
    if (!root.is_object()) {
        return Error{"protocol", "config root must be a JSON object"};
    }

    GatewayConfig config;

    if (root.contains("rest")) {
        const auto& rest = root.at("rest");
        if (!rest.is_object() || !rest.contains("port") || !rest.at("port").is_number_unsigned()) {
            return Error{"protocol",
                         "rest section must be an object with unsigned numeric \"port\""};
        }
        const auto port = rest.at("port").get<std::uint64_t>();
        if (port < kMinPort || port > kMaxPort) {
            return Error{"protocol", "rest.port is out of range [1, 65535]"};
        }
        config.rest_port = static_cast<std::uint16_t>(port);
    }

    if (root.contains("persistence")) {
        const auto& persistence = root.at("persistence");
        if (!persistence.is_object() || !persistence.contains("logPath") ||
            !persistence.at("logPath").is_string()) {
            return Error{"protocol",
                         "persistence section must be an object with string \"logPath\""};
        }
        config.persistence_log = persistence.at("logPath").get<std::string>();
    }

    if (root.contains("risk")) {
        const auto risk = risk_config_from_json(root.at("risk"));
        if (!risk.is_ok()) {
            return risk.error();
        }
        config.risk = risk.value();
    }

    if (root.contains("okx")) {
        if (!root.at("okx").is_object()) {
            return Error{"protocol", "okx section must be a JSON object"};
        }
        config.okx = root.at("okx");
    }

    if (root.contains("binance")) {
        if (!root.at("binance").is_object()) {
            return Error{"protocol", "binance section must be a JSON object"};
        }
        config.binance = root.at("binance");
    }

    if (root.contains("defaultVenue")) {
        if (!root.at("defaultVenue").is_string()) {
            return Error{"protocol", "defaultVenue must be a string"};
        }
        config.default_venue = root.at("defaultVenue").get<std::string>();
    }

    return config;
}

} // namespace gateway
