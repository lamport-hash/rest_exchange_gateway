#include "core/decimal.hpp"

#include <limits>
#include <string>

namespace gateway {

namespace {

constexpr long long kLlMax = std::numeric_limits<long long>::max();
constexpr long long kLlMin = std::numeric_limits<long long>::min();

// __int128 is a GCC extension; hide its spelling behind an alias so the
// rest of the TU stays -Wpedantic clean (same pattern as the OKX mock).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
using Wide = __int128;
#pragma GCC diagnostic pop

/// 10^a_exponent for a_exponent in [0, kMaxInternalScale].
auto pow10(int a_exponent) -> Wide
{
    Wide value = 1;
    for (int i = 0; i < a_exponent; ++i) {
        value *= 10;
    }
    return value;
}

/// Rescale both operands to their common (max) scale. Products below stay
/// within Wide: |unscaled| <= 9.3e18 and the factor is <= 1e16.
auto aligned(const Decimal& a_lhs, const Decimal& a_rhs) -> std::pair<Wide, Wide>
{
    const int scale = std::max(a_lhs.scale, a_rhs.scale);
    return {static_cast<Wide>(a_lhs.unscaled) * pow10(scale - a_lhs.scale),
            static_cast<Wide>(a_rhs.unscaled) * pow10(scale - a_rhs.scale)};
}

auto fits_ll(Wide a_value) -> bool
{
    return a_value <= static_cast<Wide>(kLlMax) && a_value >= static_cast<Wide>(kLlMin);
}

auto normalized(Decimal a_value) -> Decimal
{
    while (a_value.scale > 0 && a_value.unscaled % 10 == 0) {
        a_value.unscaled /= 10;
        --a_value.scale;
    }
    if (a_value.unscaled == 0) {
        a_value.scale = 0;
    }
    return a_value;
}

auto overflow_error(const char* a_op) -> Error
{
    return Error{"protocol", std::string("decimal ") + a_op + " overflow"};
}

} // namespace

auto parse_decimal(std::string_view a_text) -> Result<Decimal>
{
    if (!a_text.empty() && a_text.front() == '-') {
        return Error{"protocol", "negative values are not accepted here: " + std::string(a_text)};
    }
    return parse_signed_decimal(a_text);
}

auto parse_signed_decimal(std::string_view a_text) -> Result<Decimal>
{
    const auto invalid = [&a_text] {
        return Error{"protocol", "invalid decimal string: " + std::string(a_text)};
    };

    if (a_text.empty()) {
        return invalid();
    }
    bool negative = false;
    if (a_text.front() == '-') {
        negative = true;
        a_text.remove_prefix(1);
    }
    if (a_text.empty() || a_text.front() == '.' || a_text.back() == '.') {
        return invalid();
    }

    long long unscaled = 0;
    int scale = 0;
    bool in_fraction = false;
    bool any_digit = false;
    for (const char c : a_text) {
        if (c == '.') {
            if (in_fraction) {
                return invalid();
            }
            in_fraction = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return invalid();
        }
        if (in_fraction && scale >= kMaxDecimalScale) {
            return Error{"protocol", "more than " + std::to_string(kMaxDecimalScale) +
                                         " fractional digits: " + std::string(a_text)};
        }
        const int digit = c - '0';
        if (__builtin_mul_overflow(unscaled, 10, &unscaled) ||
            __builtin_add_overflow(unscaled, digit, &unscaled)) {
            return overflow_error("parse");
        }
        any_digit = true;
        if (in_fraction) {
            ++scale;
        }
    }
    if (!any_digit) {
        return invalid();
    }
    return normalized(Decimal{.unscaled = negative ? -unscaled : unscaled, .scale = scale});
}

auto decimal_to_string(const Decimal& a_value) -> std::string
{
    if (a_value.scale <= 0) {
        return std::to_string(a_value.unscaled);
    }
    const bool negative = a_value.unscaled < 0;
    const unsigned long long magnitude =
        negative ? 0ULL - static_cast<unsigned long long>(a_value.unscaled)
                 : static_cast<unsigned long long>(a_value.unscaled);
    std::string digits = std::to_string(magnitude);
    while (static_cast<int>(digits.size()) <= a_value.scale) {
        digits.insert(digits.begin(), '0');
    }
    const auto split = static_cast<int>(digits.size()) - a_value.scale;
    std::string text = digits.substr(0, static_cast<std::size_t>(split)) + "." +
                       digits.substr(static_cast<std::size_t>(split));
    if (negative) {
        text.insert(text.begin(), '-');
    }
    return text;
}

auto compare(const Decimal& a_lhs, const Decimal& a_rhs) -> int
{
    const auto [lhs, rhs] = aligned(a_lhs, a_rhs);
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

auto add(const Decimal& a_lhs, const Decimal& a_rhs) -> Result<Decimal>
{
    const auto [lhs, rhs] = aligned(a_lhs, a_rhs);
    const Wide sum = lhs + rhs;
    if (!fits_ll(sum)) {
        return overflow_error("add");
    }
    return normalized(Decimal{.unscaled = static_cast<long long>(sum),
                              .scale = std::max(a_lhs.scale, a_rhs.scale)});
}

auto sub(const Decimal& a_lhs, const Decimal& a_rhs) -> Result<Decimal>
{
    const auto [lhs, rhs] = aligned(a_lhs, a_rhs);
    const Wide difference = lhs - rhs;
    if (!fits_ll(difference)) {
        return overflow_error("sub");
    }
    return normalized(Decimal{.unscaled = static_cast<long long>(difference),
                              .scale = std::max(a_lhs.scale, a_rhs.scale)});
}

auto sub_clamped_zero(const Decimal& a_lhs, const Decimal& a_rhs) -> Decimal
{
    const auto [lhs, rhs] = aligned(a_lhs, a_rhs);
    if (lhs <= rhs) {
        return Decimal{};
    }
    return normalized(Decimal{.unscaled = static_cast<long long>(lhs - rhs),
                              .scale = std::max(a_lhs.scale, a_rhs.scale)});
}

auto mul(const Decimal& a_lhs, const Decimal& a_rhs) -> Result<Decimal>
{
    Wide product = static_cast<Wide>(a_lhs.unscaled) * a_rhs.unscaled;
    if (!fits_ll(product)) {
        return overflow_error("mul");
    }
    return normalized(
        Decimal{.unscaled = static_cast<long long>(product), .scale = a_lhs.scale + a_rhs.scale});
}

auto div(const Decimal& a_lhs, const Decimal& a_rhs, int a_max_scale) -> Result<Decimal>
{
    if (a_rhs.unscaled == 0) {
        return Error{"protocol", "decimal div by zero"};
    }
    if (a_max_scale < 0 || a_max_scale > kMaxInternalScale) {
        return Error{"protocol", "decimal div scale out of range"};
    }

    // numerator = lhs scaled to (max(lhs.scale, rhs.scale) + a_max_scale)
    // fractional digits so the integer quotient carries a_max_scale extra
    // digits; Wide holds it: |lhs.unscaled| * 10^16 <= 9.3e34 < 1.7e38.
    const int common = std::max(a_lhs.scale, a_rhs.scale);
    const Wide scaled_lhs =
        static_cast<Wide>(a_lhs.unscaled) * pow10(common - a_lhs.scale + a_max_scale);
    const Wide scaled_rhs = static_cast<Wide>(a_rhs.unscaled) * pow10(common - a_rhs.scale);

    const bool negative = (scaled_lhs < 0) != (scaled_rhs < 0);
    const Wide lhs_magnitude = scaled_lhs < 0 ? -scaled_lhs : scaled_lhs;
    const Wide rhs_magnitude = scaled_rhs < 0 ? -scaled_rhs : scaled_rhs;

    Wide quotient = lhs_magnitude / rhs_magnitude;
    const Wide remainder = lhs_magnitude % rhs_magnitude;
    // round half away from zero
    if (remainder * 2 >= rhs_magnitude) {
        ++quotient;
    }
    if (!fits_ll(negative ? -quotient : quotient)) {
        return overflow_error("div");
    }
    return normalized(Decimal{.unscaled = static_cast<long long>(negative ? -quotient : quotient),
                              .scale = a_max_scale});
}

} // namespace gateway
