#include "exchange/okx/okx_wire.hpp"

#include <string_view>

namespace gateway::exchange::okx {

namespace {
auto field_string(const nlohmann::json& a_item, const char* a_name) -> std::string
{
    const auto it = a_item.find(a_name);
    if (it == a_item.end() || it->is_null()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    return it->dump();
}
} // namespace

auto map_okx_state(std::string_view a_state) -> std::optional<OrderState>
{
    if (a_state == "live") {
        return OrderState::Live;
    }
    if (a_state == "partially_filled") {
        return OrderState::PartiallyFilled;
    }
    if (a_state == "filled") {
        return OrderState::Filled;
    }
    if (a_state == "canceled") {
        return OrderState::Canceled;
    }
    return std::nullopt;
}

auto to_json(const OkxPlaceRequest& a_request) -> nlohmann::json
{
    nlohmann::json body = {{"clOrdId", a_request.cl_ord_id},
                           {"instId", a_request.inst_id},
                           {"tdMode", a_request.td_mode},
                           {"side", a_request.side},
                           {"ordType", a_request.ord_type},
                           {"px", a_request.px},
                           {"sz", a_request.sz}};
    if (a_request.px.empty()) {
        body.erase("px");
    }
    return body;
}

auto to_json(const OkxCxlRequest& a_request) -> nlohmann::json
{
    return {{"instId", a_request.inst_id}, {"clOrdId", a_request.cl_ord_id}};
}

auto to_json(const OkxAmendRequest& a_request) -> Result<nlohmann::json>
{
    if (!a_request.new_px && !a_request.new_sz) {
        return Error{"protocol", "amend requires newPx and/or newSz"};
    }
    nlohmann::json body = {{"instId", a_request.inst_id}, {"clOrdId", a_request.cl_ord_id}};
    if (a_request.new_px) {
        body["newPx"] = *a_request.new_px;
    }
    if (a_request.new_sz) {
        body["newSz"] = *a_request.new_sz;
    }
    return body;
}

auto to_query(const OkxQuery& a_query) -> std::string
{
    return "instId=" + url_encode(a_query.inst_id) + "&clOrdId=" + url_encode(a_query.cl_ord_id);
}

auto parse_order_ack(const nlohmann::json& a_item) -> OkxOrderAck
{
    return {.ord_id = field_string(a_item, "ordId"),
            .cl_ord_id = field_string(a_item, "clOrdId"),
            .s_code = field_string(a_item, "sCode"),
            .s_msg = field_string(a_item, "sMsg")};
}

auto parse_order_info(const nlohmann::json& a_item) -> OkxOrderInfo
{
    return {.ord_id = field_string(a_item, "ordId"),
            .cl_ord_id = field_string(a_item, "clOrdId"),
            .state = field_string(a_item, "state"),
            .side = field_string(a_item, "side"),
            .ord_type = field_string(a_item, "ordType"),
            .px = field_string(a_item, "px"),
            .sz = field_string(a_item, "sz"),
            .avg_px = field_string(a_item, "avgPx"),
            .acc_fill_sz = field_string(a_item, "accFillSz")};
}

auto url_encode(std::string_view a_value) -> std::string
{
    static constexpr auto kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(a_value.size());
    for (const unsigned char c : a_value) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

} // namespace gateway::exchange::okx
