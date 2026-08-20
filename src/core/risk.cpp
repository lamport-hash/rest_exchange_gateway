#include "core/risk.hpp"

#include "core/decimal.hpp"

#include <string>
#include <utility>

namespace gateway {

namespace {

auto limits_from_json(const nlohmann::json& a_node) -> Result<InstrumentRiskLimits>
{
    if (!a_node.is_object()) {
        return Error{"protocol", "risk limits block must be a JSON object"};
    }
    InstrumentRiskLimits limits;
    const auto read = [&a_node](const char* a_name, std::string& a_target) -> std::optional<Error> {
        const auto it = a_node.find(a_name);
        if (it == a_node.end() || it->is_null()) {
            return std::nullopt;
        }
        if (!it->is_string()) {
            return Error{"protocol", std::string("risk field ") + a_name + " must be a string"};
        }
        a_target = it->get<std::string>();
        if (a_target.empty()) {
            return std::nullopt; // explicitly empty = check disabled
        }
        const auto value = parse_decimal(a_target);
        if (!value.is_ok()) {
            return Error{"protocol",
                         std::string("risk field ") + a_name + " is not a decimal: " + a_target};
        }
        return std::nullopt;
    };

    if (const auto error = read("maxQty", limits.max_qty)) {
        return *error;
    }
    if (const auto error = read("maxNotional", limits.max_notional)) {
        return *error;
    }
    if (const auto error = read("maxPosition", limits.max_position)) {
        return *error;
    }
    return limits;
}

} // namespace

auto RiskConfig::limits_for(std::string_view a_symbol) const -> std::optional<InstrumentRiskLimits>
{
    const auto it = instruments.find(std::string{a_symbol});
    if (it != instruments.end()) {
        return it->second;
    }
    return defaults;
}

auto risk_config_from_json(const nlohmann::json& a_node) -> Result<RiskConfig>
{
    if (!a_node.is_object()) {
        return Error{"protocol", "risk section must be a JSON object"};
    }
    if (a_node.empty()) {
        return RiskConfig{}; // explicitly unlimited
    }

    RiskConfig config;
    if (const auto defaults = a_node.find("default"); defaults != a_node.end()) {
        auto parsed = limits_from_json(*defaults);
        if (!parsed.is_ok()) {
            return parsed.error();
        }
        config.defaults = std::move(parsed.value());
    }
    if (const auto instruments = a_node.find("instruments"); instruments != a_node.end()) {
        if (!instruments->is_object()) {
            return Error{"protocol", "risk.instruments must be a JSON object"};
        }
        for (auto it = instruments->begin(); it != instruments->end(); ++it) {
            auto parsed = limits_from_json(it.value());
            if (!parsed.is_ok()) {
                return parsed.error();
            }
            config.instruments[it.key()] = std::move(parsed.value());
        }
    }
    return config;
}

auto check_risk(const std::optional<InstrumentRiskLimits>& a_limits, std::string_view a_symbol,
                const RiskOrder& a_order) -> std::optional<Error>
{
    if (!a_limits.has_value()) {
        return std::nullopt; // instrument unlimited
    }

    const auto invalid = [](const char* a_what, const std::string& a_value) {
        return Error{"risk_invalid_value",
                     std::string(a_what) + " is not a valid decimal: " + a_value};
    };

    auto quantity = parse_decimal(a_order.quantity);
    if (!quantity.is_ok()) {
        return invalid("quantity", a_order.quantity);
    }

    if (!a_limits->max_qty.empty()) {
        auto max_qty = parse_decimal(a_limits->max_qty);
        if (!max_qty.is_ok()) {
            return invalid("maxQty", a_limits->max_qty);
        }
        if (compare(quantity.value(), max_qty.value()) > 0) {
            return Error{"risk_max_qty", "quantity " + a_order.quantity + " exceeds maxQty " +
                                             a_limits->max_qty + " for " + std::string{a_symbol}};
        }
    }

    if (!a_limits->max_notional.empty() && !a_order.price.empty()) {
        auto price = parse_decimal(a_order.price);
        if (!price.is_ok()) {
            return invalid("price", a_order.price);
        }
        auto max_notional = parse_decimal(a_limits->max_notional);
        if (!max_notional.is_ok()) {
            return invalid("maxNotional", a_limits->max_notional);
        }
        const auto notional = mul(price.value(), quantity.value());
        if (!notional.is_ok()) {
            return Error{"risk_invalid_value", "notional computation overflowed"};
        }
        if (compare(notional.value(), max_notional.value()) > 0) {
            return Error{"risk_max_notional", "notional " + decimal_to_string(notional.value()) +
                                                  " (price " + a_order.price + " x quantity " +
                                                  a_order.quantity + ") exceeds maxNotional " +
                                                  a_limits->max_notional + " for " +
                                                  std::string{a_symbol}};
        }
    }

    if (!a_limits->max_position.empty()) {
        auto position = parse_signed_decimal(a_order.projected_position);
        if (!position.is_ok()) {
            return invalid("projectedPosition", a_order.projected_position);
        }
        auto max_position = parse_decimal(a_limits->max_position);
        if (!max_position.is_ok()) {
            return invalid("maxPosition", a_limits->max_position);
        }
        const auto magnitude =
            position.value().unscaled < 0 ? negate(position.value()) : position.value();
        if (compare(magnitude, max_position.value()) > 0) {
            return Error{"risk_max_position",
                         "projected position " + decimal_to_string(position.value()) +
                             " exceeds maxPosition ±" + a_limits->max_position + " for " +
                             std::string{a_symbol}};
        }
    }
    return std::nullopt;
}

} // namespace gateway
