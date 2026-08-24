#include "mocks/okx_mock_server.hpp"

#include "core/clock.hpp"
#include "exchange/okx/okx_signer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <thread>
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

/// Documented 59693 message shape (insufficient transferable balance).
auto currency_insufficient_message(const std::string& a_ccy) -> std::string
{
    return a_ccy + " transferable balance insufficient. Some funds are occupied by open orders "
                   "or positions. Please cancel orders or close positions and try again";
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
{}

OkxMockServer::~OkxMockServer()
{
    stop();
}

void OkxMockServer::start()
{
    auto server = std::make_unique<httplib::Server>();
    register_routes(*server);
    const int bound = server->bind_to_any_port("127.0.0.1");
    if (bound < 0) {
        throw std::runtime_error("OkxMockServer: failed to bind");
    }
    port_ = bound;
    server_ = std::move(server);
    running_ = true;
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
}

void OkxMockServer::stop()
{
    if (running_) {
        server_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        server_.reset();
        running_ = false;
    }
}

void OkxMockServer::restart_on_same_port()
{
    stop();
    auto server = std::make_unique<httplib::Server>();
    register_routes(*server);
    if (!server->bind_to_port("127.0.0.1", port_)) {
        throw std::runtime_error("OkxMockServer: failed to rebind port " + std::to_string(port_));
    }
    server_ = std::move(server);
    running_ = true;
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
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

void OkxMockServer::set_ticker(const std::string& a_inst_id, const std::string& a_last_price)
{
    const std::lock_guard lock(mutex_);
    ticker_inst_id_ = a_inst_id;
    ticker_last_ = a_last_price;
}

void OkxMockServer::set_demo_balance(const std::string& a_ccy, const std::string& a_amt)
{
    const std::lock_guard lock(mutex_);
    demo_balances_[a_ccy] = parse_decimal_scaled(a_amt);
}

void OkxMockServer::set_demo_increase_quota(int a_remaining)
{
    const std::lock_guard lock(mutex_);
    demo_increase_quota_ = a_remaining;
}

auto OkxMockServer::demo_balance(const std::string& a_ccy) const -> std::string
{
    const std::lock_guard lock(mutex_);
    const auto it = demo_balances_.find(a_ccy);
    if (it == demo_balances_.end()) {
        return {};
    }
    return scaled_to_decimal(it->second);
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

void OkxMockServer::drop_next_request()
{
    const std::lock_guard lock(mutex_);
    ++drop_next_;
}

void OkxMockServer::drop_next_response()
{
    const std::lock_guard lock(mutex_);
    drop_next_response_ = true;
}

void OkxMockServer::respond_success(httplib::Response& a_res, const std::string& a_body)
{
    bool drop = false;
    {
        const std::lock_guard lock(mutex_);
        drop = drop_next_response_;
        drop_next_response_ = false;
    }
    if (drop) {
        a_res.status = 200;
        a_res.set_chunked_content_provider("application/json",
                                           [](std::uint64_t, httplib::DataSink& a_sink) -> bool {
                                               a_sink.write("{\"partial\":", 11);
                                               return false; // abort: connection closed mid-body
                                           });
        return;
    }
    a_res.set_content(a_body, "application/json");
}

void OkxMockServer::delay_next_request(unsigned a_ms)
{
    const std::lock_guard lock(mutex_);
    delay_next_ms_ = a_ms;
}

auto OkxMockServer::begin_request(const httplib::Request& a_req, std::string_view a_body,
                                  httplib::Response& a_res) -> bool
{
    unsigned delay_ms = 0;
    bool drop = false;
    {
        const std::lock_guard lock(mutex_);
        record(a_req, a_body);
        if (raw_status_ != 0) {
            a_res.status = raw_status_;
            a_res.set_content(raw_body_, "application/json");
            raw_status_ = 0;
            return true;
        }
        if (drop_next_ > 0) {
            --drop_next_;
            drop = true;
        }
        if (delay_next_ms_ > 0) {
            delay_ms = delay_next_ms_;
            delay_next_ms_ = 0;
        }
    }

    if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{delay_ms});
    }
    if (drop) {
        a_res.status = 200;
        a_res.set_chunked_content_provider("application/json",
                                           [](std::uint64_t, httplib::DataSink& a_sink) -> bool {
                                               a_sink.write("{\"partial\":", 11);
                                               return false; // abort: connection closed mid-body
                                           });
        return true;
    }
    return false;
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

void OkxMockServer::register_routes(httplib::Server& a_server)
{
    a_server.Post("/api/v5/trade/order", [this](const httplib::Request& req,
                                                httplib::Response& res) {
        const std::string body = req.body;
        if (begin_request(req, body, res)) {
            return;
        }

        const auto auth_error = check_auth(req, body);
        if (auth_error) {
            res.set_content(*auth_error, "application/json");
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
        const std::string td_if = str("tdIf");

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
        // tdIf is only applicable to limit orders and only accepts the
        // documented values (live OKX rejects anything else with 51000).
        if (!td_if.empty()) {
            if (ord_type != "limit" || (td_if != "GTC" && td_if != "IOC" && td_if != "FOK")) {
                res.set_content(envelope_error("51000", "Parameter tdIf error"),
                                "application/json");
                return;
            }
        }
        if (inst_id != "BTC-USDT") {
            res.set_content(envelope_error("51001", "Instrument ID does not exist"),
                            "application/json");
            return;
        }

        std::string reply;
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
            reply = envelope_ok() + ack_item(ord_id, cl_ord_id) + "]}";
        }
        respond_success(res, reply);
    });

    a_server.Post(
        "/api/v5/trade/cancel-order", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string body = req.body;
            if (begin_request(req, body, res)) {
                return;
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

            std::string reply;
            {
                const std::lock_guard lock(mutex_);
                const auto it = orders_.find(cl_ord_id);
                if (it == orders_.end()) {
                    res.set_content(envelope_error("51016", "Order does not exist"),
                                    "application/json");
                    return;
                }
                if (it->second.state == "canceled" || it->second.state == "filled") {
                    res.set_content(envelope_error("51017", "Order status is done"),
                                    "application/json");
                    return;
                }
                it->second.state = "canceled";
                reply = envelope_ok() + ack_item(it->second.ord_id, cl_ord_id) + "]}";
            }
            respond_success(res, reply);
        });

    a_server.Post(
        "/api/v5/trade/amend-order", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string body = req.body;
            if (begin_request(req, body, res)) {
                return;
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

            std::string reply;
            {
                const std::lock_guard lock(mutex_);
                const auto it = orders_.find(cl_ord_id);
                if (it == orders_.end()) {
                    res.set_content(envelope_error("51016", "Order does not exist"),
                                    "application/json");
                    return;
                }
                if (it->second.state == "canceled" || it->second.state == "filled") {
                    res.set_content(envelope_error("51017", "Order status is done"),
                                    "application/json");
                    return;
                }
                if (!new_px.empty()) {
                    it->second.px = new_px;
                }
                if (!new_sz.empty()) {
                    it->second.sz = new_sz;
                }
                reply = envelope_ok() + ack_item(it->second.ord_id, cl_ord_id) + "]}";
            }
            respond_success(res, reply);
        });

    a_server.Get("/api/v5/trade/order", [this](const httplib::Request& req,
                                               httplib::Response& res) {
        const std::string body;
        if (begin_request(req, body, res)) {
            return;
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

    // GET /api/v5/trade/orders-pending: every open (live or partially
    // filled) order, newest first — same item shape as order-info.
    a_server.Get("/api/v5/trade/orders-pending",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     if (begin_request(req, "", res)) {
                         return;
                     }

                     const auto error = check_auth(req, "");
                     if (error) {
                         res.set_content(*error, "application/json");
                         return;
                     }

                     const std::lock_guard lock(mutex_);
                     nlohmann::json data = nlohmann::json::array();
                     for (const auto& entry : orders_) {
                         const MockOrder& order = entry.second;
                         if (order.state == "live" || order.state == "partially_filled") {
                             data.push_back(nlohmann::json{{"ordId", order.ord_id},
                                                           {"clOrdId", order.cl_ord_id},
                                                           {"instId", order.inst_id},
                                                           {"state", order.state},
                                                           {"side", order.side},
                                                           {"ordType", order.ord_type},
                                                           {"px", order.px},
                                                           {"sz", order.sz},
                                                           {"avgPx", order.avg_px},
                                                           {"accFillSz", order.acc_fill_sz}});
                         }
                     }
                     const nlohmann::json body = {{"code", "0"}, {"msg", ""}, {"data", data}};
                     res.set_content(body.dump(), "application/json");
                 });

    // POST /api/v5/account/demo-adjust-balance: demo-account-only balance
    // adjustment, mirroring the live venue's documented behavior:
    // - type "increase"|"reduce"; supported currencies BTC/ETH/USDT/OKB;
    //   duplicate ccys rejected; all-or-nothing validation
    // - per-request increase caps: BTC 1, ETH 1, USDT 5000, OKB 100
    // - increases consume a 3-per-day quota (reduce does not)
    // - business rejections arrive as HTTP 400 + envelope (observed live:
    //   400 + 51000 "Parameter type error"), unlike the trade endpoints
    //   which use HTTP 200 + envelope
    // - success data[0] = {remainCnt, totalCnt, details:[{ccy, amt, bal}]}
    a_server.Post("/api/v5/account/demo-adjust-balance", [this](const httplib::Request& req,
                                                                httplib::Response& res) {
        const std::string body = req.body;
        if (begin_request(req, body, res)) {
            return;
        }

        const auto auth_error = check_auth(req, body);
        if (auth_error) {
            res.set_content(*auth_error, "application/json");
            return;
        }

        const auto reject = [&res](const std::string& a_code, const std::string& a_msg) {
            res.status = 400;
            res.set_content(envelope_error(a_code, a_msg), "application/json");
        };

        const auto json = nlohmann::json::parse(body, nullptr, false);
        if (json.is_discarded() || !json.is_object()) {
            reject("51000", "Parameter error: body is not a JSON object");
            return;
        }
        const auto str = [&json](const char* a_name) {
            const auto it = json.find(a_name);
            return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
        };
        const std::string type = str("type");
        if (type != "increase" && type != "reduce") {
            reject("51000", "Parameter type error");
            return;
        }
        const auto adjustments = json.find("adjustments");
        if (adjustments == json.end() || !adjustments->is_array() || adjustments->empty()) {
            reject("51000", "Parameter error: adjustments");
            return;
        }

        // Validate every adjustment before applying any (docs:
        // all-or-nothing).
        std::vector<std::pair<std::string, long long>> parsed;
        parsed.reserve(adjustments->size());
        for (const auto& item : *adjustments) {
            if (!item.is_object()) {
                reject("51000", "Parameter error: adjustment");
                return;
            }
            const auto ccy = item.find("ccy");
            const auto amt = item.find("amt");
            if (ccy == item.end() || !ccy->is_string() || ccy->get<std::string>().empty() ||
                amt == item.end() || !amt->is_string()) {
                reject("51000", "Parameter error: ccy/amt");
                return;
            }
            long long scaled = 0;
            try {
                scaled = parse_decimal_scaled(amt->get<std::string>());
            } catch (const std::runtime_error&) {
                reject("51000", "Parameter error: amt");
                return;
            }
            const std::string currency = ccy->get<std::string>();
            static const std::unordered_map<std::string, long long> kCurrencies = {
                {"BTC", 1 * kScale},
                {"ETH", 1 * kScale},
                {"USDT", 5000 * kScale},
                {"OKB", 100 * kScale}};
            const auto supported = kCurrencies.find(currency);
            if (supported == kCurrencies.end()) {
                reject("51000", "Parameter error: unsupported ccy " + currency);
                return;
            }
            if (type == "increase" && scaled > supported->second) {
                reject("51000", "Parameter error: amt exceeds the increase cap");
                return;
            }
            for (const auto& entry : parsed) {
                if (entry.first == currency) {
                    reject("51000", "Parameter error: duplicate ccy " + currency);
                    return;
                }
            }
            parsed.emplace_back(currency, scaled);
        }

        std::string reply;
        {
            const std::lock_guard lock(mutex_);
            if (type == "reduce") {
                for (const auto& [ccy, scaled] : parsed) {
                    const auto it = demo_balances_.find(ccy);
                    const long long balance = it == demo_balances_.end() ? 0 : it->second;
                    if (scaled > balance) {
                        reject("59693", currency_insufficient_message(ccy));
                        return;
                    }
                }
            } else if (demo_increase_quota_ <= 0) {
                reject("50011", "Rate limit: daily demo increase quota exhausted");
                return;
            }
            nlohmann::json details = nlohmann::json::array();
            for (const auto& [ccy, scaled] : parsed) {
                long long& balance = demo_balances_[ccy];
                balance += type == "increase" ? scaled : -scaled;
                details.push_back({{"ccy", ccy},
                                   {"amt", scaled_to_decimal(scaled)},
                                   {"bal", scaled_to_decimal(balance)}});
            }
            if (type == "increase") {
                --demo_increase_quota_;
            }
            nlohmann::json item = {{"remainCnt", std::to_string(demo_increase_quota_)},
                                   {"totalCnt", "3"},
                                   {"details", std::move(details)}};
            reply = envelope_ok() + item.dump() + "]}";
        }
        respond_success(res, reply);
    });

    // GET /api/v5/market/ticker: PUBLIC market data (no auth headers);
    // serves the scripted instrument/price, rejects others with 51001
    // like live OKX ("Instrument ID does not exist").
    a_server.Get("/api/v5/market/ticker", [this](const httplib::Request& req,
                                                 httplib::Response& res) {
        if (begin_request(req, "", res)) {
            return;
        }

        const std::string inst_id = req.get_param_value("instId");
        if (inst_id.empty()) {
            res.set_content(envelope_error("51000", "Parameter error: instId"), "application/json");
            return;
        }

        const std::lock_guard lock(mutex_);
        if (inst_id != ticker_inst_id_) {
            res.set_content(envelope_error("51001", "Instrument ID does not exist"),
                            "application/json");
            return;
        }
        nlohmann::json item = {{"instId", ticker_inst_id_},
                               {"last", ticker_last_},
                               {"askPx", ticker_last_},
                               {"bidPx", ticker_last_},
                               {"ts", "1700000000000"}};
        res.set_content(envelope_ok() + item.dump() + "]}", "application/json");
    });
}

} // namespace gateway::testing
