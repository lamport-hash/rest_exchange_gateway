#pragma once

#include "exchange/okx/okx_rest_client.hpp"
#include "gateway/exchange_connector.hpp"

#include <functional>
#include <optional>
#include <string_view>

namespace gateway::exchange::okx {

/// Map an OKX order state string to the normalized OrderState.
/// Returns std::nullopt for unknown/unsupported states.
[[nodiscard]] auto map_okx_state(std::string_view a_state) -> std::optional<OrderState>;

/// OKX implementation of gateway::ExchangeConnector (REST-based, phase 1).
class OkxConnector final : public ExchangeConnector
{
  public:
    explicit OkxConnector(OkxConfig a_config,
                          OkxRestClient::TimestampProvider a_timestamp = nullptr);

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

  private:
    OkxRestClient client_;
    std::function<void(const ExecutionReport&)> execution_report_handler_;
};

} // namespace gateway::exchange::okx
