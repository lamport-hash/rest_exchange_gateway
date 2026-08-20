#pragma once

#include "gateway/result.hpp"

#include <string>
#include <string_view>

namespace gateway {

/// Exact fixed-point decimal used for prices, quantities, notionals and
/// positions. Values are unscaled * 10^scale with at most
/// kMaxDecimalScale fractional digits on input; arithmetic may carry up
/// to kMaxInternalScale digits. Magnitudes must fit a signed 64-bit
/// integer — anything beyond that is an explicit error, never a silent
/// rounding. Domain includes negatives (signed positions); parsing
/// rejects signs (client inputs are non-negative by schema).
struct Decimal
{
    long long unscaled = 0;
    int scale = 0;
};

inline constexpr int kMaxDecimalScale = 8;
inline constexpr int kMaxInternalScale = 16;

/// Parse a plain decimal string ("0", "50000", "0.001", "49999.50").
/// Errors ("protocol"): empty input, sign characters, stray characters,
/// missing digits on either side of '.', more than kMaxDecimalScale
/// fractional digits, 64-bit overflow. "0.50" normalizes to "0.5".
[[nodiscard]] auto parse_decimal(std::string_view a_text) -> Result<Decimal>;

/// Like parse_decimal but tolerates one leading '-' (signed positions).
/// "+", "--1" and "-" alone are still errors.
[[nodiscard]] auto parse_signed_decimal(std::string_view a_text) -> Result<Decimal>;

/// Canonical text form (trailing fractional zeros stripped, "-0" -> "0").
[[nodiscard]] auto decimal_to_string(const Decimal& a_value) -> std::string;

/// Three-way compare: -1 when a_lhs < a_rhs, 0 when equal, 1 when greater.
[[nodiscard]] auto compare(const Decimal& a_lhs, const Decimal& a_rhs) -> int;

[[nodiscard]] inline auto is_zero(const Decimal& a_value) -> bool
{
    return a_value.unscaled == 0;
}

[[nodiscard]] inline auto negate(const Decimal& a_value) -> Decimal
{
    return Decimal{.unscaled = -a_value.unscaled, .scale = a_value.scale};
}

/// Exact sum. Error ("protocol") on 64-bit overflow after scale alignment.
[[nodiscard]] auto add(const Decimal& a_lhs, const Decimal& a_rhs) -> Result<Decimal>;

/// Exact difference (a_lhs - a_rhs). Error on overflow.
[[nodiscard]] auto sub(const Decimal& a_lhs, const Decimal& a_rhs) -> Result<Decimal>;

/// max(0, a_lhs - a_rhs); never errors (clamps instead of going negative).
[[nodiscard]] auto sub_clamped_zero(const Decimal& a_lhs, const Decimal& a_rhs) -> Decimal;

/// Exact product (result scale = sum of scales, then normalized).
/// Error on 64-bit overflow.
[[nodiscard]] auto mul(const Decimal& a_lhs, const Decimal& a_rhs) -> Result<Decimal>;

/// Exact division a_lhs / a_rhs computed at up to a_max_scale fractional
/// digits, rounding half away from zero at the cut, then normalized.
/// Errors ("protocol"): a_rhs is zero, or the result does not fit 64 bits
/// at a_max_scale (e.g. tiny divisors).
[[nodiscard]] auto div(const Decimal& a_lhs, const Decimal& a_rhs,
                       int a_max_scale) -> Result<Decimal>;

} // namespace gateway
