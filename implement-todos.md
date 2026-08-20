# Implementation Todos — rest_exchange_gateway

Walking-skeleton approach: real OKX code first, abstractions extracted from
working code, second exchange last. Source of truth: doc/project-spec.md.

## Ground rules (every phase)
- [ ] Fully code each feature when built — no stubs or TODOs left behind
- [ ] Every function: normal-case test + dangerous edge-case tests
      (doc/general-guidelines.md)
- [ ] Deterministic tests run against in-process mock exchanges built from the
      official OKX v5 / Binance docs — never live exchanges
- [ ] `ctest --preset debug` (ASan+UBSan) AND `--preset release` green to close a phase
- [ ] Consult user before architectural or core-object changes

## Phase 1 — Raw OKX REST client → ExchangeConnector → thin gateway slice
- [ ] 1.1 Scaffolding: CMake + Ninja, debug/release presets (C++20,
      -Wall -Wextra -Wpedantic -Werror), third_party/ (crow_all.h, httplib.h,
      json.hpp, doctest.h), .clang-format; Crow /health smoke test
- [ ] 1.2 Raw OKX REST client (no abstraction yet): HMAC-SHA256 signing,
      OK-ACCESS-* headers, x-simulated-trading demo header; place / cancel /
      get-order against /api/v5/trade/*
- [ ] 1.3 Mock OKX (in-process, from official v5 docs): trade/order,
      trade/cancel-order, trade/order-info; error envelope (code/sMsg);
      basic partial-fill engine; deterministic unit tests for the client
- [ ] 1.4 ExchangeConnector thin interface extracted from the working client:
      place/cancel/amend/get + execution-report callback; OKXConnector
      implements it; exchange code confined below the interface
- [ ] 1.5 Thin Crow slice wired end-to-end: POST /orders, GET /orders/{id},
      DELETE /orders/{id} → OKXConnector; structured JSON errors
- [ ] 1.6 Live validation on OKX demo trading (credentials in config)
      Acceptance: place/cancel/status through the gateway over HTTP on OKX
      demo; mock tests green in both presets

## Phase 2 — Resilience for the OKX connector
- [ ] 2.1 Exponential backoff + jitter retry policy for REST (configurable
      caps, per-request budget)
- [ ] 2.2 OKX WebSocket orders channel: subscribe, normalize updates,
      disconnect detection, reconnect with backoff, resubscribe, ping/pong
- [ ] 2.3 Unknown-outcome handling: place timeout → get-order resolution;
      idempotent clientOrderId retries (same request twice → same outcome)
- [ ] 2.4 Fault-injection tests on mock OKX: dropped, delayed, duplicate and
      out-of-order messages; WS killed mid-stream
      Acceptance: connectivity failures → gateway recovers, no lost or
      double-applied orders; both presets green

## Phase 3 — Order state machine, OMS, recovery, full REST surface
- [ ] 3.1 Normalized OrderState machine (Live, PartiallyFilled, Filled,
      Canceled, Rejected) with explicit OKX mapping table; illegal transitions
      rejected; exhaustive unit tests
- [ ] 3.2 OMS registry: clientOrderId→exchangeOrderId map, duplicate-report
      dedup, out-of-order arbitration, REST-vs-WS race handling
- [ ] 3.3 Append-only persistence log + startup replay (truncated-tail safety)
- [ ] 3.4 Startup reconciliation with OKX open orders (orders-pending);
      restart-with-live-orders drill
- [ ] 3.5 Pre-trade risk checks: max qty per instrument, max notional,
      approximate position limit; clear reject reasons to the client
- [ ] 3.6 Complete client-facing REST API: PUT /orders/{id} (amend),
      GET /health, full error schema {error:{code,reason,clientOrderId}};
      reject exchange-specific fields; integration tests (ephemeral-port Crow
      driven by cpp-httplib)
      Acceptance: restart/reconcile drills pass on mock; state-machine tests
      exhaustive; both presets green

## Phase 4 — Second exchange (Binance WS) + truly agnostic gateway
- [ ] 4.1 Mock Binance (in-process, from official docs): WS-API order.place /
      order.cancel, executionReport stream, fault injection
- [ ] 4.2 BinanceConnector fully coded: signed WS-API requests (testnet
      wss://testnet.binance.vision/ws-api/v3), execution reports from
      user-data stream, symbol translation BTC-USDT↔BTCUSDT, amend emulation
      via cancelReplace
- [ ] 4.3 Venue-agnostic gateway: routing on `venue` field, per-venue
      config/limits, zero exchange code above the connector interface;
      startup re-establishes Binance WS state
- [ ] 4.4 Deliverables: README (architecture, common schema, adapter design,
      idempotency/retry strategy, auth-security discussion, trade-offs &
      limitations); examples/ curl scripts for both venues; final pass:
      clang-format whole tree + full ctest both presets
- [ ] 4.5 Stretch (optional): rate-limit awareness per exchange, sequence/gap
      detection, backpressure, property-based state-machine tests
      Acceptance: all project-spec deliverables present; both venues work
      through the single unified API
