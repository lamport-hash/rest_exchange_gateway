# Architecture — rest_exchange_gateway

Exchange-agnostic order REST gateway routing a unified client schema to OKX and Binance. Source of truth: `doc/project-spec.md`; C++ rules: `doc/code-guidelines.md`.

## Layers

```mermaid
flowchart TB
    C([Client])

    subgraph REST["REST layer (src/rest/)"]
        R["Crow routes: POST/GET/DELETE/PUT /orders, GET /health<br/>one schema, one error envelope, zero venue code"]
    end

    subgraph CORE["OMS core (src/core/)"]
        OMS["OrderManagementSystem<br/>clientOrderId registry + venue routing"]
        SM["Order state machine<br/>(explicit transition table)"]
        RK["Pre-trade risk<br/>maxQty / maxNotional / maxPosition"]
        LOG["EventLog<br/>append-only JSONL"]
    end

    subgraph SEAM["ExchangeConnector (include/gateway/)"]
        IC["place / cancel / amend / get / open-orders<br/>execution-report + connectivity callbacks"]
    end

    subgraph OKX["OKX adapter (src/exchange/okx/)"]
        OKXR["signed REST /api/v5/trade/*"]
        OKXW["WS private orders channel"]
    end

    subgraph BIN["Binance adapter (src/exchange/binance/)"]
        BINW["WS-API order.place/cancel/cancelReplace"]
        BINU["user-data stream executionReport"]
    end

    V1([OKX])
    V2([Binance])

    C -->|HTTP| R --> OMS
    OMS --- SM --- RK
    OMS --> LOG
    OMS -->|venue-routed| IC
    IC --> OKXR --> V1
    OKXW -->|execution reports| IC
    IC --> BINW --> V2
    BINU -->|execution reports| IC
    IC -->|normalized reports / connectivity| OMS
```

1. **REST layer** (`src/rest/`, public headers in `include/gateway/`)
   - Single client-facing API; identical request/response schema regardless of venue.
   - Clients never send exchange-specific fields.
2. **Core** (`src/core/`)
   - Unified order model + order state machine.
   - `clientOrderId → exchangeOrderId` mapping.
   - Pre-trade risk checks: max order size per instrument, max notional per order, per-instrument position limit (approximate). Rejections return clear reasons.
   - Append-only persistence log for restart recovery.
3. **Exchange adapters** (`src/exchange/<venue>/`)
   - Isolated behind one common interface (`IExchange` or concept-based).
   - OKX: REST for order entry / cancel / amend; WebSocket for order and trade updates.
   - Binance: WebSocket-based order submission; WebSocket subscriptions for execution updates.

## Rules

- Exchange-specific code lives only inside its adapter subdirectory and is never referenced from the REST layer.
- Common order schema: `clientOrderId`, `venue` (OKX | BINANCE), `symbol`, `side`, `type` (Market/Limit), `price`, `quantity`, `timeInForce`.
- Normalized execution states: New/Live, Partially Filled, Filled, Canceled, Rejected. Mapping between client-level concepts and exchange-specific semantics must be explicit.

## Robustness Requirements

- Handle out-of-order WebSocket messages, duplicate execution reports, and REST vs WebSocket race conditions.
- Idempotent with respect to client retries.
- WebSocket disconnect/reconnect handling.
- On startup: reload persisted orders, reconcile with OKX open orders, re-establish Binance WebSocket state.

Diagrams: architecture above; order state machine, place idempotency
sequence and restart-recovery sequence in `doc/api.md`.

## External APIs (implementation must be based on the official docs)

- OKX API v5: https://my.okx.com/docs-v5/en/
- Binance Spot WebSocket API: https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

## Out of Scope (per spec)

- Secure auth/key management (discussion only), UI/frontend, full exchange feature coverage, ultra-low-latency optimization.

## Project Layout

```
include/gateway/          # Public headers (the 1-REST surface)
src/
  core/                   # Shared types, config, logging, metrics
  exchange/               # One subdirectory per exchange (okx/, binance/, ...)
  rest/                   # Unified REST handlers
tests/
```
