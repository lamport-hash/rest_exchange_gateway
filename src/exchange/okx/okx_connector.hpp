#pragma once

#include "exchange/okx/okx_rest_client.hpp"
#include "gateway/exchange_connector.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace gateway::exchange::okx {

class OkxOrdersFeed;

/// OKX implementation of gateway::ExchangeConnector.
/// - place/cancel/amend use resolve-then-retry semantics (transport failures
///   are resolved via order lookup before any re-send; identical client
///   retries with the same clOrdId resolve to the existing order)
/// - start()/stop() control the private WebSocket orders feed; execution
///   reports are delivered through set_execution_report_handler.
class OkxConnector final : public ExchangeConnector
{
  public:
    explicit OkxConnector(OkxConfig a_config,
                          OkxRestClient::TimestampProvider a_timestamp = nullptr);
    ~OkxConnector() override;

    OkxConnector(const OkxConnector&) = delete;
    auto operator=(const OkxConnector&) -> OkxConnector& = delete;

    [[nodiscard]] auto
    place_order(const OrderRequest& a_request) -> Result<OrderPlacement> override;
    [[nodiscard]] auto
    cancel_order(const CancelRequest& a_request) -> Result<OrderPlacement> override;
    [[nodiscard]] auto
    amend_order(const AmendRequest& a_request) -> Result<OrderPlacement> override;
    [[nodiscard]] auto
    get_order(const OrderQuery& a_query) -> Result<std::optional<OrderSnapshot>> override;
    [[nodiscard]] auto get_open_orders() -> Result<std::vector<OrderSnapshot>> override;
    [[nodiscard]] auto
    get_price(const std::string& a_instrument_id) -> Result<std::string> override;

    /// OKX-specific helper outside ExchangeConnector: POST
    /// /api/v5/account/demo-adjust-balance. Fails fast with
    /// "invalid_request" when demo trading is disabled (the venue
    /// endpoint only exists for demo accounts). Non-idempotent: exactly
    /// one attempt, never retried — a retried increase/reduce could
    /// double-apply. Returns the venue's data array verbatim.
    [[nodiscard]] auto
    adjust_demo_balance(const OkxDemoBalanceRequest& a_request) -> Result<nlohmann::json>;

    void
    set_execution_report_handler(std::function<void(const ExecutionReport&)> a_handler) override;
    void set_connectivity_handler(std::function<void(bool)> a_handler) override;

    void start() override;
    void stop() override;

  private:
    void forward_report(const ExecutionReport& a_report);
    void forward_connectivity(bool a_connected);

    OkxConfig config_;
    OkxRestClient::TimestampProvider timestamp_provider_;
    OkxRestClient client_;
    RetryClock retry_clock_;
    std::mutex handler_mutex_;
    std::function<void(const ExecutionReport&)> execution_report_handler_;
    std::function<void(bool)> connectivity_handler_;
    std::unique_ptr<OkxOrdersFeed> feed_;
};

} // namespace gateway::exchange::okx
