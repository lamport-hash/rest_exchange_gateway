#include "mocks/okx_mock_server.hpp"

#include "core/clock.hpp"
#include "exchange/okx/okx_signer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gateway::testing {

namespace {
constexpr long long kScale = 100000000LL;

auto parse_decimal_scaled(const std::string& a_text) -> long long
{
    if (a_text.empty() || !std::isdigit(static_cast<unsigned char>(a_text.front()))) {
        throw std::runtime_error("mock: not a decimal: " + a_text);
    }
    long long whole = 0;
    long long frac = 0;
    long long frac_digits = 0;
    bool in_frac = false;
    for (const char c : a_text) {
        if (c == '.') {
            if (in_frac) {
                throw std::runtime_error("mock: not a decimal: " + a_text);
            }
            in_frac = true;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            throw std::runtime_error("mock: not a decimal: " + a_text);
        }
        if (in_frac) {
            if (frac_digits >= 8) {
                throw std::runtime_error("mock: more than 8 decimal places: " + a_text);
            }
            frac = frac * 10 + (c - '0');
            ++frac_digits;
        } else {
            whole = whole * 10 + (c - '0');
        }
    }
    for (long long i = frac_digits; i < 8; ++i) {
        frac *= 10;
    }
    return whole * kScale + frac;
}

auto scaled_to_decimal(long long a_scaled) -> std::string
{
    const long long whole = a_scaled / kScale;
    long long frac = a_scaled % kScale;
    if (frac == 0) {
        return std::to_string(whole);
    }
    std::string frac_str(8, '0');
    for (int i = 7; i >= 0; --i) {
        frac_str[static_cast<std::size_t>(i)] = static_cast<char>('0' + frac % 10);
        frac /= 10;
    }
    while (frac_str.back() == '0') {
        frac_str.pop_back();
    }
    return std::to_string(whole) + "." + frac_str;
}

auto envelope_ok() -> std::string
{
    return R"({"code":"0","msg":"","data":[)";
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

auto mul_div_scaled(long long a_a, long long a_b) -> long long
{
    const __int128 product = static_cast<__int128>(a_a) * a_b;
    return static_cast<long long>(product / kScale);
}

auto mul_div_scaled(long long a_a, long long a_b, long long a_c) -> long long
{
    const __int128 product = static_cast<__int128>(a_a) * a_b;
    return static_cast<long long>(product / a_c);
}

#pragma GCC diagnostic pop

auto envelope_error(const std::string& a_code, const std::string& a_msg) -> std::string
{
    nlohmann::json body = {{"code", a_code}, {"msg", a_msg}, {"data", nlohmann::json::array()}};
    return body.dump();
}

auto ack_item(const std::string& a_ord_id, const std::string& a_cl_ord_id) -> std::string
{
    nlohmann::json item = {
        {"ordId", a_ord_id}, {"clOrdId", a_cl_ord_id}, {"sCode", "0"}, {"sMsg", ""}};
    return item.dump();
}
} // namespace

OkxMockServer::OkxMockServer(exchange::okx::OkxConfig a_client_config)
    : client_config_(std::move(a_client_config))
{
    register_routes();
}

OkxMockServer::~OkxMockServer()
{
    stop();
}

void OkxMockServer::start()
{
    const int bound = server_.bind_to_any_port("127.0.0.1");
    if (bound < 0) {
        throw std::runtime_error("OkxMockServer: failed to bind");
    }
    port_ = static_cast<std::uint16_t>(bound);
    running_ = true;
    server_thread_ = std::thread([this] { server_.listen_after_bind(); });
}

void OkxMockServer::stop()
{
    if (running_) {
        server_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        running_ = false;
    }
}

auto OkxMockServer::port() const -> std::uint16_t
{
    return static_cast<std::uint16_t>(port_);
}

void OkxMockServer::set_fill_mode(FillMode a_mode)
{
    const std::lock_guard lock(mutex_);
    fill_mode_ = a_mode;
}

void OkxMockServer::apply_fill(const std::string& a_cl_ord_id, const std::string& a_qty,
                               const std::string& a_px)
{
    const std::lock_guard lock(mutex_);
    auto it = orders_.find(a_cl_ord_id);
    if (it == orders_.end()) {
        throw std::runtime_error("mock: apply_fill on unknown order " + a_cl_ord_id);
    }
    MockOrder& order = it->second;
    if (order.state != "live" && order.state != "partially_filled") {
        throw std::runtime_error("mock: apply_fill on terminal order " + a_cl_ord_id);
    }
    const long long qty = parse_decimal_scaled(a_qty);
    const long long px = parse_decimal_scaled(a_px);
    const long long sz = parse_decimal_scaled(order.sz);
    if (qty <= 0 || order.acc_scaled + qty > sz) {
        throw std::runtime_error("mock: fill quantity " + a_qty + " does not fit order " +
                                 a_cl_ord_id);
    }
    order.acc_scaled += qty;
    order.quote_scaled += mul_div_scaled(qty, px);
    order.acc_fill_sz = scaled_to_decimal(order.acc_scaled);
    order.avg_px = scaled_to_decimal(mul_div_scaled(order.quote_scaled, kScale, order.acc_scaled));
    order.state = order.acc_scaled == sz ? "filled" : "partially_filled";
}

void OkxMockServer::set_next_raw_response(int a_status, std::string a_body)
{
    const std::lock_guard lock(mutex_);
    raw_status_ = a_status;
    raw_body_ = std::move(a_body);
}

auto OkxMockServer::recorded_requests() const -> std::vector<RecordedRequest>
{
    const std::lock_guard lock(mutex_);
    return recorded_;
}

auto OkxMockServer::check_auth(const httplib::Request& a_req,
                               std::string_view a_body) const -> std::optional<std::string>
{
    const auto missing_or = [&a_req](const char* a_name) -> bool {
        return !a_req.has_header(a_name);
    };
    if (missing_or("OK-ACCESS-KEY") || missing_or("OK-ACCESS-SIGN") ||
        missing_or("OK-ACCESS-TIMESTAMP") || missing_or("OK-ACCESS-PASSPHRASE")) {
        return envelope_error("50102", "Invalid Sign: missing OK-ACCESS header(s)");
    }
    const auto expected =
        exchange::okx::sign_request(a_req.get_header_value("OK-ACCESS-TIMESTAMP"), a_req.method,
                                    a_req.target, a_body, client_config_.secret_key);
    if (expected != a_req.get_header_value("OK-ACCESS-SIGN")) {
        return envelope_error("50102", "Invalid Sign");
    }
    return std::nullopt;
}

auto OkxMockServer::find_order(const std::string& a_cl_ord_id) const -> std::optional<MockOrder>
{
    const auto it = orders_.find(a_cl_ord_id);
    if (it == orders_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void OkxMockServer::record(const httplib::Request& a_req, std::string_view a_body)
{
    recorded_.push_back(
        RecordedRequest{a_req.method, a_req.target, std::string(a_body), a_req.headers});
}

auto OkxMockServer::next_ord_id() -> std::string
{
    return "mock-" + std::to_string(++ord_counter_);
}

void OkxMockServer::register_routes()
{
    server_.Post("/api/v5/trade/order", [this](const httplib::Request& req,
                                               httplib::Response& res) {
        std::string body = req.body;
        std::optional<std::string> error;

        {
            const std::lock_guard lock(mutex_);
            record(req, body);
            if (raw_status_ != 0) {
                res.status = raw_status_;
                res.set_content(raw_body_, "application/json");
                raw_status_ = 0;
                return;
            }
        }

        error = check_auth(req, body);
        if (error) {
            res.set_content(*error, "application/json");
            return;
        }

        const auto json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            res.set_content(envelope_error("51000", "Parameter error: body is not a JSON object"),
                            "application/json");
            return;
        }
        const auto str = [&json](const char* a_name) {
            const auto it = json.find(a_name);
            return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
        };
        const std::string cl_ord_id = str("clOrdId");
        const std::string inst_id = str("instId");
        const std::string side = str("side");
        const std::string ord_type = str("ordType");
        const std::string px = str("px");
        const std::string sz = str("sz");

        const auto valid_cl_ord_id = [](const std::string& a_id) {
            if (a_id.empty() || a_id.size() > 32) {
                return false;
            }
            return std::all_of(a_id.begin(), a_id.end(),
                               [](unsigned char a_c) { return std::isalnum(a_c) != 0; });
        };
        if (!valid_cl_ord_id(cl_ord_id)) {
            res.set_content(envelope_error("51000", "Parameter clOrdId error"), "application/json");
            return;
        }
        if (cl_ord_id.empty() || inst_id.empty() || sz.empty()) {
            res.set_content(envelope_error("51000", "Parameter error: clOrdId/instId/sz required"),
                            "application/json");
            return;
        }
        if (side != "buy" && side != "sell") {
            res.set_content(envelope_error("51040", "Invalid order side"), "application/json");
            return;
        }
        if (ord_type != "limit" && ord_type != "market") {
            res.set_content(envelope_error("51010", "Unsupported order type"), "application/json");
            return;
        }
        if (inst_id != "BTC-USDT") {
            res.set_content(envelope_error("51001", "Instrument ID does not exist"),
                            "application/json");
            return;
        }

        {
            const std::lock_guard lock(mutex_);
            if (const auto it = orders_.find(cl_ord_id);
                it != orders_.end() &&
                (it->second.state != "canceled" && it->second.state != "filled")) {
                res.set_content(
                    envelope_error("51000",
                                   "Parameter error: duplicate active clOrdId " + cl_ord_id),
                    "application/json");
                return;
            }

            MockOrder order{.ord_id = next_ord_id(),
                            .cl_ord_id = cl_ord_id,
                            .inst_id = inst_id,
                            .side = side,
                            .ord_type = ord_type,
                            .px = px,
                            .sz = sz,
                            .state = "live",
                            .acc_fill_sz = "0",
                            .avg_px = ""};
            if (fill_mode_ == FillMode::Full) {
                order.state = "filled";
                order.acc_scaled = parse_decimal_scaled(sz);
                order.acc_fill_sz = sz;
                order.avg_px = px;
                order.quote_scaled = mul_div_scaled(order.acc_scaled, parse_decimal_scaled(px));
            }
            orders_[cl_ord_id] = order;
            const std::string ord_id = order.ord_id;
            res.set_content(envelope_ok() + ack_item(ord_id, cl_ord_id) + "]}", "application/json");
        }
    });

    server_.Post("/api/v5/trade/cancel-order", [this](const httplib::Request& req,
                                                      httplib::Response& res) {
        std::string body = req.body;
        {
            const std::lock_guard lock(mutex_);
            record(req, body);
            if (raw_status_ != 0) {
                res.status = raw_status_;
                res.set_content(raw_body_, "application/json");
                raw_status_ = 0;
                return;
            }
        }

        const auto error = check_auth(req, body);
        if (error) {
            res.set_content(*error, "application/json");
            return;
        }

        const auto json = nlohmann::json::parse(body, nullptr, false);
        const auto str = [&json](const char* a_name) {
            const auto it = json.find(a_name);
            return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
        };
        const std::string cl_ord_id = str("clOrdId");

        const std::lock_guard lock(mutex_);
        const auto it = orders_.find(cl_ord_id);
        if (it == orders_.end()) {
            res.set_content(envelope_error("51016", "Order does not exist"), "application/json");
            return;
        }
        if (it->second.state == "canceled" || it->second.state == "filled") {
            res.set_content(envelope_error("51017", "Order status is done"), "application/json");
            return;
        }
        it->second.state = "canceled";
        res.set_content(envelope_ok() + ack_item(it->second.ord_id, cl_ord_id) + "]}",
                        "application/json");
    });

    server_.Post("/api/v5/trade/amend-order", [this](const httplib::Request& req,
                                                     httplib::Response& res) {
        std::string body = req.body;
        {
            const std::lock_guard lock(mutex_);
            record(req, body);
            if (raw_status_ != 0) {
                res.status = raw_status_;
                res.set_content(raw_body_, "application/json");
                raw_status_ = 0;
                return;
            }
        }

        const auto error = check_auth(req, body);
        if (error) {
            res.set_content(*error, "application/json");
            return;
        }

        const auto json = nlohmann::json::parse(body, nullptr, false);
        const auto str = [&json](const char* a_name) {
            const auto it = json.find(a_name);
            return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
        };
        const std::string cl_ord_id = str("clOrdId");
        const std::string new_px = str("newPx");
        const std::string new_sz = str("newSz");

        const std::lock_guard lock(mutex_);
        const auto it = orders_.find(cl_ord_id);
        if (it == orders_.end()) {
            res.set_content(envelope_error("51016", "Order does not exist"), "application/json");
            return;
        }
        if (it->second.state == "canceled" || it->second.state == "filled") {
            res.set_content(envelope_error("51017", "Order status is done"), "application/json");
            return;
        }
        if (!new_px.empty()) {
            it->second.px = new_px;
        }
        if (!new_sz.empty()) {
            it->second.sz = new_sz;
        }
        res.set_content(envelope_ok() + ack_item(it->second.ord_id, cl_ord_id) + "]}",
                        "application/json");
    });

    server_.Get("/api/v5/trade/order", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string body;
        {
            const std::lock_guard lock(mutex_);
            record(req, body);
            if (raw_status_ != 0) {
                res.status = raw_status_;
                res.set_content(raw_body_, "application/json");
                raw_status_ = 0;
                return;
            }
        }

        const auto error = check_auth(req, body);
        if (error) {
            res.set_content(*error, "application/json");
            return;
        }

        const std::string inst_id = req.get_param_value("instId");
        const std::string cl_ord_id = req.get_param_value("clOrdId");
        if (inst_id.empty() || cl_ord_id.empty()) {
            res.set_content(envelope_error("51000", "Parameter error: instId/clOrdId"),
                            "application/json");
            return;
        }

        const std::lock_guard lock(mutex_);
        const auto order = find_order(cl_ord_id);
        if (!order.has_value()) {
            res.set_content(envelope_error("51603", "Order does not exist"), "application/json");
            return;
        }
        if (order->inst_id != inst_id) {
            res.set_content(envelope_ok() + "]}", "application/json");
            return;
        }
        nlohmann::json item = {{"ordId", order->ord_id},   {"clOrdId", order->cl_ord_id},
                               {"instId", order->inst_id}, {"state", order->state},
                               {"side", order->side},      {"ordType", order->ord_type},
                               {"px", order->px},          {"sz", order->sz},
                               {"avgPx", order->avg_px},   {"accFillSz", order->acc_fill_sz}};
        res.set_content(envelope_ok() + item.dump() + "]}", "application/json");
    });
}

} // namespace gateway::testing
