#pragma once

#include "core/decimal.hpp"
#include "core/retry.hpp"
#include "exchange/binance/binance_config.hpp"
#include "exchange/binance/binance_signer.hpp"
#include "exchange/binance/binance_wire.hpp"
#include "gateway/result.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gateway::exchange::binance {

namespace detail {

/// Numeric equality of a gateway decimal string and a venue-formatted one
/// ("23416.1" == "23416.10000000"). True for empty a_requested (field not
/// part of the comparison).
inline auto value_matches(const std::string& a_requested, const std::string& a_reported) -> bool
{
    if (a_requested.empty()) {
        return true;
    }
    if (a_requested == a_reported) {
        return true;
    }
    const auto lhs = parse_decimal(a_requested);
    const auto rhs = parse_decimal(a_reported);
    return lhs.is_ok() && rhs.is_ok() && compare(lhs.value(), rhs.value()) == 0;
}

} // namespace detail

/// Typed operations over BinanceWsClient: build + call + parse. The raw
/// call layer (BinanceWsClient::call_signed) adds apiKey/timestamp and
/// the HMAC signature. No retry logic here (that is what the resilient
/// wrappers add).
class BinanceApi
{
  public:
    /// a_call: (method, params) -> "result" payload (Result<json>); the
    /// implementation is expected to sign the request.
    /// a_public_call: same shape for NONE-security methods (market data)
    /// that must NOT carry signature params.
    using RawCall =
        std::function<Result<nlohmann::json>(const std::string&, const nlohmann::json&)>;

    explicit BinanceApi(RawCall a_call, RawCall a_public_call = nullptr)
        : call_(std::move(a_call)), public_call_(std::move(a_public_call))
    {}

    /// "order.place". Errors: "transport", "protocol", "venue:<code>".
    [[nodiscard]] auto place(const BinancePlaceRequest& a_request) -> Result<BinanceOrderAck>;

    /// "order.cancel".
    [[nodiscard]] auto cancel(const BinanceCancelRequest& a_request) -> Result<BinanceOrderInfo>;

    /// "order.cancelReplace" (STOP_ON_FAILURE). Both legs succeeded or an
    /// error is returned.
    [[nodiscard]] auto
    cancel_replace(const BinanceAmendRequest& a_request) -> Result<BinanceReplaceResult>;

    /// "order.status". std::nullopt when the venue does not know the
    /// order (venue:-2013) — the documented "order does not exist" reply.
    [[nodiscard]] auto
    get_order(const BinanceOrderQuery& a_query) -> Result<std::optional<BinanceOrderInfo>>;

    /// "openOrders.status" (all symbols).
    [[nodiscard]] auto get_open_orders() -> Result<std::vector<BinanceOrderInfo>>;

    /// "ticker.price" (public, unsigned): last-traded price of a WIRE
    /// symbol (e.g. "BTCUSDT"), verbatim decimal string.
    [[nodiscard]] auto get_price(const std::string& a_wire_symbol) -> Result<std::string>;

  private:
    [[nodiscard]] auto invoke(const std::string& a_method,
                              const nlohmann::json& a_params) -> Result<nlohmann::json>;

    RawCall call_;
    RawCall public_call_;
};

/// Outcome of an order.status lookup used to resolve unknown outcomes.
enum class BinanceLookupOutcome
{
    Found,
    /// The venue definitively does not know the order (-2013).
    Absent,
    /// Inconclusive (transport kept failing / protocol error).
    Inconclusive
};

struct BinanceLookup
{
    BinanceLookupOutcome outcome = BinanceLookupOutcome::Inconclusive;
    BinanceOrderInfo info;
    std::optional<Error> error;
};

/// Retry order.status until conclusive (transport retried per a_policy;
/// venue:-2013 conclusive Absence; other errors conclusive failure).
template <typename ApiT>
[[nodiscard]] auto binance_lookup(ApiT& a_api, const BinanceOrderQuery& a_query,
                                  const RetryPolicy& a_policy,
                                  const RetryClock& a_clock) -> BinanceLookup
{
    const auto attempt = [&a_api, &a_query]() -> Result<BinanceLookup> {
        auto result = a_api.get_order(a_query);
        if (result.is_ok()) {
            if (result.value().has_value()) {
                return BinanceLookup{BinanceLookupOutcome::Found, std::move(*result.value()),
                                     std::nullopt};
            }
            return BinanceLookup{BinanceLookupOutcome::Absent, BinanceOrderInfo{}, std::nullopt};
        }
        if (result.error().code == "transport") {
            return result.error();
        }
        return BinanceLookup{BinanceLookupOutcome::Inconclusive, BinanceOrderInfo{},
                             result.error()};
    };

    auto resolved =
        with_retries<BinanceLookup>(a_policy, a_clock, attempt, [](const Error& a_error) {
            return a_error.code == "transport";
        });
    if (resolved.is_ok()) {
        return resolved.value();
    }
    return BinanceLookup{BinanceLookupOutcome::Inconclusive, BinanceOrderInfo{}, resolved.error()};
}

/// Place with resolve-then-retry semantics (mirrors the OKX engine):
/// - transport failure (outcome unknown): resolve via order.status; found
///   -> synthesized ack (no re-send); conclusively absent -> identical
///   re-send; unresolved -> "transport" without re-send (no double-place)
/// - venue:-4116 (duplicate clientOrderId, e.g. an earlier unresolved
///   place actually landed): resolve and return the existing order's ack
/// - other venue/protocol errors are definitive and returned unchanged.
template <typename ApiT>
[[nodiscard]] auto binance_resilient_place(ApiT& a_api, const BinancePlaceRequest& a_request,
                                           const RetryPolicy& a_policy,
                                           const RetryClock& a_clock) -> Result<BinanceOrderAck>
{
    const BinanceOrderQuery query{a_request.client_order_id, a_request.symbol};
    bool safe_to_resend = false;

    const auto attempt = [&]() -> Result<BinanceOrderAck> {
        safe_to_resend = false;
        auto result = a_api.place(a_request);
        if (result.is_ok()) {
            return result;
        }
        const auto& error = result.error();
        if (error.code == "transport" || error.code == "venue:-4116") {
            const auto lookup = binance_lookup(a_api, query, a_policy, a_clock);
            if (lookup.outcome == BinanceLookupOutcome::Found) {
                return BinanceOrderAck{.order_id = lookup.info.order_id,
                                       .client_order_id = lookup.info.client_order_id,
                                       .status = lookup.info.status,
                                       .executed_qty = lookup.info.executed_qty};
            }
            if (error.code == "venue:-4116") {
                // Definitively absent duplicate: a genuine venue problem.
                return result;
            }
            if (lookup.outcome == BinanceLookupOutcome::Absent) {
                safe_to_resend = true;
                return result;
            }
            return Error{"transport", "place outcome unresolved for clientOrderId " +
                                          a_request.client_order_id + ": " +
                                          lookup.error.value_or(error).message};
        }
        return result;
    };

    return with_retries<BinanceOrderAck>(
        a_policy, a_clock, attempt, [&safe_to_resend](const Error&) { return safe_to_resend; });
}

/// Cancel with resolve-then-retry semantics:
/// - transport failure: resolve; already CANCELED -> synthesized success
///   (idempotent); still working -> safe re-send; conclusively absent ->
///   definitive venue:-2013; unresolved -> "transport" without re-send
/// - venue:-2011 (unknown order / already done): resolve; CANCELED ->
///   idempotent success; any other terminal state is a legitimate
///   rejection; inconclusive -> the original error.
template <typename ApiT>
[[nodiscard]] auto binance_resilient_cancel(ApiT& a_api, const BinanceCancelRequest& a_request,
                                            const RetryPolicy& a_policy,
                                            const RetryClock& a_clock) -> Result<BinanceOrderInfo>
{
    const BinanceOrderQuery query{a_request.client_order_id, a_request.symbol};
    bool safe_to_resend = false;

    const auto synthesized = [](const BinanceOrderInfo& a_info) -> Result<BinanceOrderInfo> {
        return a_info;
    };

    const auto attempt = [&]() -> Result<BinanceOrderInfo> {
        safe_to_resend = false;
        auto result = a_api.cancel(a_request);
        if (result.is_ok()) {
            return result;
        }
        const auto& error = result.error();
        if (error.code == "transport") {
            const auto lookup = binance_lookup(a_api, query, a_policy, a_clock);
            if (lookup.outcome == BinanceLookupOutcome::Found) {
                if (lookup.info.status == "CANCELED") {
                    return synthesized(lookup.info);
                }
                if (lookup.info.status == "NEW" || lookup.info.status == "PARTIALLY_FILLED" ||
                    lookup.info.status == "PENDING_CANCEL") {
                    safe_to_resend = true;
                    return result;
                }
                // Filled/expired: a cancel can never succeed.
                return Error{"venue:-2011", "order " + a_request.client_order_id + " is already " +
                                                lookup.info.status};
            }
            if (lookup.outcome == BinanceLookupOutcome::Absent) {
                return Error{"venue:-2013", "cancel target not found on the venue"};
            }
            return Error{"transport", "cancel outcome unresolved for clientOrderId " +
                                          a_request.client_order_id + ": " +
                                          lookup.error.value_or(error).message};
        }
        if (error.code == "venue:-2011") {
            const auto lookup = binance_lookup(a_api, query, a_policy, a_clock);
            if (lookup.outcome == BinanceLookupOutcome::Found && lookup.info.status == "CANCELED") {
                return synthesized(lookup.info);
            }
            return result;
        }
        return result;
    };

    return with_retries<BinanceOrderInfo>(
        a_policy, a_clock, attempt, [&safe_to_resend](const Error&) { return safe_to_resend; });
}

/// Amend (cancelReplace) with resolve-then-retry semantics:
/// - transport failure: resolve; when the current order already carries
///   every requested new value the amend landed -> synthesized success;
///   when the original order is still untouched -> safe re-send; when the
///   original is gone but no replacement exists (cancel landed, place
///   leg's fate unknown) -> definitive error naming the situation;
///   absent everywhere -> definitive venue:-2013; unresolved -> transport
/// - venue/protocol errors are definitive and returned unchanged.
template <typename ApiT>
[[nodiscard]] auto binance_resilient_amend(ApiT& a_api, const BinanceAmendRequest& a_request,
                                           const RetryPolicy& a_policy,
                                           const RetryClock& a_clock) -> Result<BinanceOrderAck>
{
    const BinanceOrderQuery query{a_request.client_order_id, a_request.symbol};
    bool safe_to_resend = false;

    const auto matches_request = [](const BinanceOrderInfo& a_info,
                                    const BinanceAmendRequest& a_request) {
        return detail::value_matches(a_request.price, a_info.price) &&
               detail::value_matches(a_request.quantity, a_info.orig_qty);
    };

    const auto attempt = [&]() -> Result<BinanceOrderAck> {
        safe_to_resend = false;
        auto result = a_api.cancel_replace(a_request);
        if (result.is_ok()) {
            return result.value().replacement;
        }
        const auto& error = result.error();
        if (error.code != "transport") {
            return result.error();
        }
        const auto lookup = binance_lookup(a_api, query, a_policy, a_clock);
        if (lookup.outcome == BinanceLookupOutcome::Found) {
            if (matches_request(lookup.info, a_request)) {
                // The replacement order is live under the same clientOrderId.
                return BinanceOrderAck{.order_id = lookup.info.order_id,
                                       .client_order_id = lookup.info.client_order_id,
                                       .status = lookup.info.status,
                                       .executed_qty = lookup.info.executed_qty};
            }
            const auto& status = lookup.info.status;
            if (status == "NEW" || status == "PARTIALLY_FILLED" || status == "PENDING_CANCEL") {
                safe_to_resend = true; // cancelReplace never landed
                return result.error();
            }
            return Error{"venue:-2011", "amend outcome partially unknown for clientOrderId " +
                                            a_request.client_order_id + ": original order is " +
                                            status + " and no replacement is live; " +
                                            "re-issue as a new order if needed"};
        }
        if (lookup.outcome == BinanceLookupOutcome::Absent) {
            return Error{"venue:-2013", "amend target not found on the venue"};
        }
        return Error{"transport", "amend outcome unresolved for clientOrderId " +
                                      a_request.client_order_id + ": " +
                                      lookup.error.value_or(error).message};
    };

    return with_retries<BinanceOrderAck>(
        a_policy, a_clock, attempt, [&safe_to_resend](const Error&) { return safe_to_resend; });
}

} // namespace gateway::exchange::binance
