#pragma once

#include "core/retry.hpp"
#include "exchange/binance/binance_config.hpp"
#include "exchange/binance/binance_resilient.hpp"
#include "exchange/binance/binance_wire.hpp"
#include "exchange/binance/binance_ws_client.hpp"
#include "gateway/exchange_connector.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace gateway::exchange::binance {

/// Binance implementation of gateway::ExchangeConnector.
/// - every synchronous operation is a signed WS-API request over one
///   connection (order.place / order.cancel / order.cancelReplace /
///   order.status / openOrders.status)
/// - place/cancel/amend use resolve-then-retry semantics: unknown
///   outcomes (transport failures, 5xx, response timeouts) are resolved
///   via order.status before any re-send
/// - amend is emulated with cancelReplace (STOP_ON_FAILURE); the
///   replacement keeps the clientOrderId and receives a NEW orderId
/// - execution reports come from the account's User Data Stream,
///   subscribed on the same connection; a reconnect re-subscribes it
/// - symbols are translated BTC-USDT <-> BTCUSDT in both directions
class BinanceConnector final : public ExchangeConnector
{
  public:
    explicit BinanceConnector(BinanceConfig a_config, UnixMsProvider a_timestamp = nullptr);
    ~BinanceConnector() override;

    BinanceConnector(const BinanceConnector&) = delete;
    auto operator=(const BinanceConnector&) -> BinanceConnector& = delete;

    [[nodiscard]] auto
    place_order(const OrderRequest& a_request) -> Result<OrderPlacement> override;
    [[nodiscard]] auto
    cancel_order(const CancelRequest& a_request) -> Result<OrderPlacement> override;
    [[nodiscard]] auto
    amend_order(const AmendRequest& a_request) -> Result<OrderPlacement> override;
    [[nodiscard]] auto
    get_order(const OrderQuery& a_query) -> Result<std::optional<OrderSnapshot>> override;
    [[nodiscard]] auto get_open_orders() -> Result<std::vector<OrderSnapshot>> override;

    void
    set_execution_report_handler(std::function<void(const ExecutionReport&)> a_handler) override;
    void set_connectivity_handler(std::function<void(bool)> a_handler) override;

    void start() override;
    void stop() override;

  private:
    void forward_report(const ExecutionReport& a_report);
    void forward_connectivity(bool a_connected);

    BinanceConfig config_;
    UnixMsProvider timestamp_provider_;
    RetryClock retry_clock_;
    SymbolTranslator symbols_;

    std::mutex handler_mutex_;
    std::function<void(const ExecutionReport&)> execution_report_handler_;
    std::function<void(bool)> connectivity_handler_;

    BinanceWsClient ws_;
    BinanceApi api_;
    bool started_ = false;
};

} // namespace gateway::exchange::binance
