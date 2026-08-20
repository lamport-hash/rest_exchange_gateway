#pragma once

#include "gateway/exchange_connector.hpp"
#include "gateway/result.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace gateway {

/// Per-instrument pre-trade limits. Values are decimal strings; an empty
/// field disables that specific check for the instrument.
struct InstrumentRiskLimits
{
    std::string max_qty;
    std::string max_notional;
    std::string max_position;
};

/// Config-driven risk limits: an optional defaults block plus explicit
/// per-instrument entries (an entry replaces the defaults wholesale).
struct RiskConfig
{
    std::optional<InstrumentRiskLimits> defaults;
    std::map<std::string, InstrumentRiskLimits> instruments;

    /// Limits for a_symbol: the explicit entry when present, otherwise
    /// the defaults; std::nullopt when the instrument is unlimited.
    [[nodiscard]] auto limits_for(std::string_view a_symbol) const
        -> std::optional<InstrumentRiskLimits>;
};

/// Parse the "risk" config section:
/// {"default": {"maxQty","maxNotional","maxPosition"},
///  "instruments": {"BTC-USDT": {...}}}
/// All fields are optional decimal strings; unknown keys are ignored.
/// Errors: "protocol" (wrong types, non-decimal values, empty section).
/// An EMPTY object is valid and means "no limits configured".
[[nodiscard]] auto risk_config_from_json(const nlohmann::json& a_node) -> Result<RiskConfig>;

/// Candidate order as seen by the risk engine.
struct RiskOrder
{
    Side side = Side::Buy;
    /// Limit price; empty for market orders (notional check skipped —
    /// documented limitation: no price feed is available pre-trade).
    std::string price;
    /// Full order quantity (decimal string).
    std::string quantity;
    /// Worst-case signed position AFTER this order fills completely:
    /// signed(filled) across the instrument plus signed(outstanding) of
    /// every working order including this one (decimal string).
    std::string projected_position;
};

/// Evaluate the pre-trade checks. Returns std::nullopt when the order is
/// acceptable; otherwise a rejection with codes:
/// - "risk_max_qty": quantity above maxQty
/// - "risk_max_notional": price * quantity above maxNotional
/// - "risk_max_position": |projected_position| above maxPosition
/// - "risk_invalid_value": a value that should be a decimal is not
[[nodiscard]] auto check_risk(const std::optional<InstrumentRiskLimits>& a_limits,
                              std::string_view a_symbol, const RiskOrder& a_order)
    -> std::optional<Error>;

} // namespace gateway
