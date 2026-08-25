#include "mocks/okx_mock_ws_server.hpp"

#include "exchange/okx/okx_signer.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace gateway::testing {

namespace {
auto string_field(const nlohmann::json& a_node, const char* a_name) -> std::string
{
    const auto it = a_node.find(a_name);
    return it != a_node.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

auto login_reply_ok() -> std::string
{
    return R"({"event":"login","code":"0","msg":""})";
}

auto login_reply_error(const std::string& a_detail) -> std::string
{
    const nlohmann::json body = {{"event", "error"}, {"code", "60009"}, {"msg", a_detail}};
    return body.dump();
}

auto subscribe_reply(const nlohmann::json& a_arg) -> std::string
{
    const nlohmann::json body = {{"event", "subscribe"}, {"arg", a_arg}};
    return body.dump();
}

/// Live OKX reply for an orders subscription without instType.
auto subscribe_error_reply() -> std::string
{
    const nlohmann::json body = {{"event", "error"},
                                 {"code", "60018"},
                                 {"msg", "Subscribe failed, wrong URL or channel:orders doesn't "
                                         "exist. Please use the correct URL, channel and "
                                         "parameters referring to API document."}};
    return body.dump();
}
} // namespace

OkxMockWsServer::OkxMockWsServer(exchange::okx::OkxConfig a_client_config, std::string a_path)
    : client_config_(std::move(a_client_config)), path_(std::move(a_path))
{}

OkxMockWsServer::~OkxMockWsServer()
{
    stop();
}

void OkxMockWsServer::start()
{
    auto server = std::make_unique<httplib::Server>();
    register_routes(*server);
    const int bound = server->bind_to_any_port("127.0.0.1");
    if (bound < 0) {
        throw std::runtime_error("OkxMockWsServer: failed to bind");
    }
    port_ = bound;
    server_ = std::move(server);
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
    running_ = true;
}

void OkxMockWsServer::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    {
        const std::lock_guard lock(mutex_);
        for (Session* session : sessions_) {
            if (session->socket != nullptr) {
                session->socket->close(); // unblocks the handler read loops
            }
        }
    }
    server_->stop();
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    server_.reset();
}

void OkxMockWsServer::restart_on_same_port()
{
    stop();
    auto server = std::make_unique<httplib::Server>();
    register_routes(*server);
    if (!server->bind_to_port("127.0.0.1", port_)) {
        throw std::runtime_error("OkxMockWsServer: failed to rebind port " + std::to_string(port_));
    }
    server_ = std::move(server);
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
    running_ = true;
}

auto OkxMockWsServer::port() const -> std::uint16_t
{
    return static_cast<std::uint16_t>(port_);
}

void OkxMockWsServer::set_login_should_fail(bool a_fail)
{
    const std::lock_guard lock(mutex_);
    login_should_fail_ = a_fail;
}

void OkxMockWsServer::set_raw_login_ack(std::string a_reply)
{
    const std::lock_guard lock(mutex_);
    raw_login_ack_ = std::move(a_reply);
}

void OkxMockWsServer::set_raw_subscribe_ack(std::string a_reply)
{
    const std::lock_guard lock(mutex_);
    raw_subscribe_ack_ = std::move(a_reply);
}

void OkxMockWsServer::set_ignore_pings(bool a_ignore)
{
    const std::lock_guard lock(mutex_);
    ignore_pings_ = a_ignore;
}

void OkxMockWsServer::set_drop_next_updates(int a_count)
{
    const std::lock_guard lock(mutex_);
    drop_next_ = a_count;
}

void OkxMockWsServer::set_duplicate_next_update()
{
    const std::lock_guard lock(mutex_);
    duplicate_next_ = true;
}

void OkxMockWsServer::push_orders_update(const nlohmann::json& a_item)
{
    const nlohmann::json message = {{"arg", nlohmann::json{{"channel", "orders"}}},
                                    {"data", nlohmann::json::array({a_item})}};
    const std::string payload = message.dump();

    const std::lock_guard lock(mutex_);
    if (drop_next_ > 0) {
        --drop_next_;
        return;
    }
    for (Session* session : sessions_) {
        if (session->subscribed && session->socket != nullptr) {
            session->socket->send(payload);
            if (duplicate_next_) {
                session->socket->send(payload);
            }
        }
    }
    if (duplicate_next_) {
        duplicate_next_ = false;
    }
    ++stats_.pushes_delivered;
}

void OkxMockWsServer::send_text(const std::string& a_text)
{
    const std::lock_guard lock(mutex_);
    for (Session* session : sessions_) {
        if (session->socket != nullptr) {
            session->socket->send(a_text);
        }
    }
}

void OkxMockWsServer::kill_connections()
{
    const std::lock_guard lock(mutex_);
    for (Session* session : sessions_) {
        if (session->socket != nullptr) {
            session->socket->close(httplib::ws::CloseStatus::Normal, "mock kill");
        }
    }
}

auto OkxMockWsServer::stats() const -> Stats
{
    const std::lock_guard lock(mutex_);
    return stats_;
}

auto OkxMockWsServer::wait_for_subscriber(int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this] { return stats_.any_subscribed; });
}

auto OkxMockWsServer::last_login_timestamp() const -> std::string
{
    const std::lock_guard lock(mutex_);
    return last_login_timestamp_;
}

auto OkxMockWsServer::wait_for_received(std::size_t a_size, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this, a_size] { return stats_.received.size() >= a_size; });
}

auto OkxMockWsServer::wait_for_text(const std::string& a_text, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms}, [this, &a_text] {
        return std::find(stats_.received.begin(), stats_.received.end(), a_text) !=
               stats_.received.end();
    });
}

auto OkxMockWsServer::wait_for_logins_failed(int a_count, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this, a_count] { return stats_.logins_failed >= a_count; });
}

auto OkxMockWsServer::wait_for_connections(int a_count, int a_timeout_ms) const -> bool
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds{a_timeout_ms},
                        [this, a_count] { return stats_.connections >= a_count; });
}

void OkxMockWsServer::handle_text(httplib::ws::WebSocket& a_ws, Session& a_session,
                                  const std::string& a_text)
{
    bool ignore_pings = false;
    {
        const std::lock_guard lock(mutex_);
        stats_.received.push_back(a_text);
        ignore_pings = ignore_pings_;
        cv_.notify_all();
    }

    if (a_text == "ping") {
        if (!ignore_pings) {
            a_ws.send("pong");
        }
        return;
    }

    const auto message = nlohmann::json::parse(a_text, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        return;
    }
    const std::string op = string_field(message, "op");

    if (op == "login") {
        const auto args = message.find("args");
        std::string detail = "malformed login";
        bool ok = false;
        if (args != message.end() && args->is_array() && !args->empty() &&
            args->front().is_object()) {
            const auto& entry = args->front();
            const std::string api_key = string_field(entry, "apiKey");
            const std::string passphrase = string_field(entry, "passphrase");
            const std::string timestamp = string_field(entry, "timestamp");
            const std::string sign = string_field(entry, "sign");
            const std::string expected =
                exchange::okx::sign_ws_login(timestamp, client_config_.secret_key);
            ok = !login_should_fail_ && api_key == client_config_.api_key &&
                 passphrase == client_config_.passphrase && !timestamp.empty() && sign == expected;
            detail = ok ? std::string{} : "Invalid sign/credentials (mock)";
            last_login_timestamp_ = timestamp;
        }
        const std::lock_guard lock(mutex_);
        if (ok) {
            a_session.authenticated = true;
            ++stats_.logins_ok;
        } else {
            ++stats_.logins_failed;
        }
        if (!raw_login_ack_.empty()) {
            a_ws.send(std::move(raw_login_ack_)); // one-shot scripted ack
            raw_login_ack_.clear();
            cv_.notify_all();
            return;
        }
        cv_.notify_all();
        a_ws.send(ok ? login_reply_ok() : login_reply_error(detail));
        return;
    }

    if (op == "subscribe" || op == "unsubscribe") {
        const auto args = message.find("args");
        if (args == message.end() || !args->is_array() || args->empty() ||
            !args->front().is_object()) {
            a_ws.send(login_reply_error("malformed subscribe"));
            return;
        }
        const auto& arg = args->front();
        // Live OKX (verified on the demo environment) rejects an orders
        // subscription without instType with error 60018; instId is not
        // a valid subscribe parameter for this channel either.
        const bool is_orders =
            string_field(arg, "channel") == "orders" && !string_field(arg, "instType").empty();
        const std::lock_guard lock(mutex_);
        if (op == "subscribe" && is_orders) {
            a_session.subscribed = true;
        } else {
            a_session.subscribed = false;
        }
        stats_.any_subscribed = stats_.any_subscribed || a_session.subscribed;
        int subscribed_count = 0;
        for (const Session* session : sessions_) {
            subscribed_count += session->subscribed ? 1 : 0;
        }
        stats_.subscribed_sessions = subscribed_count;
        cv_.notify_all();
        if (!raw_subscribe_ack_.empty()) {
            a_ws.send(std::move(raw_subscribe_ack_)); // one-shot scripted ack
            raw_subscribe_ack_.clear();
            return;
        }
        if (string_field(arg, "channel") == "orders" && !is_orders) {
            a_ws.send(subscribe_error_reply());
            return;
        }
        a_ws.send(subscribe_reply(arg));
        return;
    }
}

void OkxMockWsServer::register_routes(httplib::Server& a_server)
{
    a_server.WebSocket(path_, [this](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        Session session{.socket = &ws, .authenticated = false, .subscribed = false};
        {
            const std::lock_guard lock(mutex_);
            sessions_.push_back(&session);
            ++stats_.connections;
            stats_.handshake_targets.push_back(req.target);
            stats_.saw_demo_header =
                stats_.saw_demo_header || req.has_header("x-simulated-trading");
            cv_.notify_all();
        }

        while (true) {
            std::string text;
            if (ws.read(text) != httplib::ws::ReadResult::Text) {
                break;
            }
            handle_text(ws, session, text);
        }

        // Deregister under the same mutex that senders use, so no scripted
        // push can touch the socket after this point (it is destroyed when
        // the handler returns).
        const std::lock_guard lock(mutex_);
        const auto it = std::find(sessions_.begin(), sessions_.end(), &session);
        if (it != sessions_.end()) {
            sessions_.erase(it);
        }
        int subscribed_count = 0;
        for (const Session* entry : sessions_) {
            subscribed_count += entry->subscribed ? 1 : 0;
        }
        stats_.subscribed_sessions = subscribed_count;
        cv_.notify_all();
    });
}

} // namespace gateway::testing
