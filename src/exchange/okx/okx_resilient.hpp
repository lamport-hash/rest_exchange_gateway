#pragma once

#include "core/retry.hpp"
#include "exchange/okx/okx_wire.hpp"
#include "gateway/result.hpp"

#include <optional>
#include <string>
#include <utility>

namespace gateway::exchange::okx {

/// Outcome of an order lookup (GET /api/v5/trade/order) used to resolve
/// unknown-outcome situations.
enum class LookupOutcome
{
    Found,
    /// The venue definitively does not know the order (51603 or empty data).
    Absent,
    /// The lookup could not reach a conclusion within the retry policy
    /// (transport kept failing, or a non-retryable protocol error).
    Inconclusive
};

struct LookupResult
{
    LookupOutcome outcome = LookupOutcome::Inconclusive;
    /// Valid only when outcome == Found.
    OkxOrderInfo info;
    /// Error detail when outcome == Inconclusive.
    std::optional<Error> error;
};

/// Retry the order lookup until it is conclusive. Transport errors are
/// retried per a_policy; venue:51603 and an empty data array are conclusive
/// Absence; protocol and other venue errors are conclusive failures of the
/// lookup itself (Inconclusive, with the error attached).
template <typename ClientT>
[[nodiscard]] auto lookup_order(ClientT& a_client, const OkxQuery& a_query,
                                const RetryPolicy& a_policy,
                                const RetryClock& a_clock) -> LookupResult
{
    const auto attempt = [&a_client, &a_query]() -> Result<LookupResult> {
        const auto result = a_client.get_order(a_query);
        if (result.is_ok()) {
            if (result.value().has_value()) {
                return LookupResult{LookupOutcome::Found, *result.value(), std::nullopt};
            }
            return LookupResult{LookupOutcome::Absent, OkxOrderInfo{}, std::nullopt};
        }
        const auto& error = result.error();
        if (error.code == "venue:51603") {
            return LookupResult{LookupOutcome::Absent, OkxOrderInfo{}, std::nullopt};
        }
        if (error.code == "transport") {
            return error;
        }
        return LookupResult{LookupOutcome::Inconclusive, OkxOrderInfo{}, error};
    };

    auto resolved =
        with_retries<LookupResult>(a_policy, a_clock, attempt, [](const Error& a_error) {
            return a_error.code == "transport";
        });
    if (resolved.is_ok()) {
        return resolved.value();
    }
    return LookupResult{LookupOutcome::Inconclusive, OkxOrderInfo{}, resolved.error()};
}

/// Ack synthesized from an order found via lookup (used when a
/// place/cancel/amend actually landed but its acknowledgement was lost, or
/// when a retried place collided with the venue's duplicate-clOrdId check).
[[nodiscard]] inline auto synthesized_ack(const OkxOrderInfo& a_info) -> OkxOrderAck
{
    return OkxOrderAck{
        .ord_id = a_info.ord_id, .cl_ord_id = a_info.cl_ord_id, .s_code = "0", .s_msg = ""};
}

/// Place with resolve-then-retry semantics:
/// - transport failure (outcome unknown): resolve via lookup; if the order
///   exists, return its ack (no re-send); if conclusively absent, re-send the
///   identical place (same clOrdId); if unresolved, fail with "transport"
///   without re-sending (a re-send could double-place).
/// - venue:51000 (duplicate active clOrdId, e.g. a client retry): resolve via
///   lookup and return the existing order's ack — the second identical place
///   yields the same outcome as the first.
/// - all other venue/protocol errors are definitive and returned unchanged.
template <typename ClientT>
[[nodiscard]] auto resilient_place(ClientT& a_client, const OkxPlaceRequest& a_request,
                                   const RetryPolicy& a_policy,
                                   const RetryClock& a_clock) -> Result<OkxOrderAck>
{
    const OkxQuery query{a_request.inst_id, a_request.cl_ord_id};
    bool safe_to_resend = false;

    const auto attempt = [&]() -> Result<OkxOrderAck> {
        safe_to_resend = false;
        auto result = a_client.place_order(a_request);
        if (result.is_ok()) {
            return result;
        }
        const auto& error = result.error();
        if (error.code == "transport" || error.code == "venue:51000") {
            const LookupResult lookup = lookup_order(a_client, query, a_policy, a_clock);
            if (lookup.outcome == LookupOutcome::Found) {
                return synthesized_ack(lookup.info);
            }
            if (error.code == "venue:51000") {
                // Definitively absent: a genuine parameter error, not a duplicate.
                return result;
            }
            if (lookup.outcome == LookupOutcome::Absent) {
                safe_to_resend = true;
                return result;
            }
            return Error{"transport", "place outcome unresolved for clOrdId " +
                                          a_request.cl_ord_id + ": " +
                                          lookup.error.value_or(error).message};
        }
        return result;
    };

    return with_retries<OkxOrderAck>(a_policy, a_clock, attempt,
                                     [&safe_to_resend](const Error&) { return safe_to_resend; });
}

/// Cancel with resolve-then-retry semantics:
/// - transport failure: resolve via lookup; already canceled -> synthesized
///   success (idempotent); still live/partially filled -> safe re-send;
///   conclusively absent -> definitive venue:51603; unresolved -> "transport"
///   without re-send (a cancel re-send is harmless, but an unreachable venue
///   cannot succeed either; the client may retry).
/// - venue:51017 (order already done): canceled -> idempotent success; any
///   other terminal state (e.g. filled) is a legitimate rejection.
/// - venue:51016 / venue:51603 / other errors are definitive.
template <typename ClientT>
[[nodiscard]] auto resilient_cancel(ClientT& a_client, const OkxCxlRequest& a_request,
                                    const RetryPolicy& a_policy,
                                    const RetryClock& a_clock) -> Result<OkxOrderAck>
{
    const OkxQuery query{a_request.inst_id, a_request.cl_ord_id};
    bool safe_to_resend = false;

    const auto attempt = [&]() -> Result<OkxOrderAck> {
        safe_to_resend = false;
        auto result = a_client.cancel_order(a_request);
        if (result.is_ok()) {
            return result;
        }
        const auto& error = result.error();
        if (error.code == "transport") {
            const LookupResult lookup = lookup_order(a_client, query, a_policy, a_clock);
            if (lookup.outcome == LookupOutcome::Found) {
                if (lookup.info.state == "canceled") {
                    return synthesized_ack(lookup.info);
                }
                safe_to_resend = true; // cancel did not land on a live order
                return result;
            }
            if (lookup.outcome == LookupOutcome::Absent) {
                return Error{"venue:51603", "cancel target not found on the venue"};
            }
            return Error{"transport", "cancel outcome unresolved for clOrdId " +
                                          a_request.cl_ord_id + ": " +
                                          lookup.error.value_or(error).message};
        }
        if (error.code == "venue:51017") {
            const LookupResult lookup = lookup_order(a_client, query, a_policy, a_clock);
            if (lookup.outcome == LookupOutcome::Found && lookup.info.state == "canceled") {
                return synthesized_ack(lookup.info);
            }
            return result;
        }
        return result;
    };

    return with_retries<OkxOrderAck>(a_policy, a_clock, attempt,
                                     [&safe_to_resend](const Error&) { return safe_to_resend; });
}

/// Amend with resolve-then-retry semantics:
/// - transport failure: resolve via lookup; when the snapshot already matches
///   every requested new value the amend landed -> synthesized success;
///   otherwise re-send (amending towards the same values is idempotent in
///   effect); absent -> definitive venue:51603; unresolved -> "transport".
/// - venue/protocol errors are definitive and returned unchanged.
template <typename ClientT>
[[nodiscard]] auto resilient_amend(ClientT& a_client, const OkxAmendRequest& a_request,
                                   const RetryPolicy& a_policy,
                                   const RetryClock& a_clock) -> Result<OkxOrderAck>
{
    const OkxQuery query{a_request.inst_id, a_request.cl_ord_id};
    bool safe_to_resend = false;

    const auto matches_request = [](const OkxOrderInfo& a_info, const OkxAmendRequest& a_request) {
        if (a_request.new_px.has_value() && a_info.px != *a_request.new_px) {
            return false;
        }
        if (a_request.new_sz.has_value() && a_info.sz != *a_request.new_sz) {
            return false;
        }
        return true;
    };

    const auto attempt = [&]() -> Result<OkxOrderAck> {
        safe_to_resend = false;
        auto result = a_client.amend_order(a_request);
        if (result.is_ok()) {
            return result;
        }
        const auto& error = result.error();
        if (error.code == "transport") {
            const LookupResult lookup = lookup_order(a_client, query, a_policy, a_clock);
            if (lookup.outcome == LookupOutcome::Found) {
                if (matches_request(lookup.info, a_request)) {
                    return synthesized_ack(lookup.info);
                }
                safe_to_resend = true;
                return result;
            }
            if (lookup.outcome == LookupOutcome::Absent) {
                return Error{"venue:51603", "amend target not found on the venue"};
            }
            return Error{"transport", "amend outcome unresolved for clOrdId " +
                                          a_request.cl_ord_id + ": " +
                                          lookup.error.value_or(error).message};
        }
        return result;
    };

    return with_retries<OkxOrderAck>(a_policy, a_clock, attempt,
                                     [&safe_to_resend](const Error&) { return safe_to_resend; });
}

} // namespace gateway::exchange::okx
