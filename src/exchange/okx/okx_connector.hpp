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

    void
    set_execution_report_handler(std::function<void(const ExecutionReport&)> a_handler) override;

    void start() override;
    void stop() override;

  private:
    void forward_report(const ExecutionReport& a_report);

    OkxConfig config_;
    OkxRestClient::TimestampProvider timestamp_provider_;
    OkxRestClient client_;
    RetryClock retry_clock_;
    std::mutex handler_mutex_;
    std::function<void(const ExecutionReport&)> execution_report_handler_;
    std::unique_ptr<OkxOrdersFeed> feed_;
};

} // namespace gateway::exchange::okx
