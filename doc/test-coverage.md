# Test Coverage Matrix

Maps each requirement in `doc/project-spec.md` to the tests that cover it.

Test locations:

- `tests/live/live_func_tests.sh okx|binance` — one venue-parameterized live
  functional suite (no mocks) against the real OKX demo and Binance spot
  testnet environments. Section numbers below refer to the `== N. ... ==`
  headers in that script; assertions are identical for both venues.
  (`tests/live/okx_live_func_tests.sh` is a thin wrapper for `... okx`.)
- `tests/*_test.cpp` — deterministic unit tests (mock servers in `tests/mocks/`).
- `tests/blackbox/` — blackbox client tests against a mock OKX environment.

## Spec coverage

| Spec requirement | Live suite | Unit / blackbox tests |
|---|---|---|
| Exchange-agnostic REST schema | 2, 11 | `rest_api_test.cpp` |
| Client must not send exchange-specific fields (rejected) | 11 | `rest_api_test.cpp` |
| New order — Limit | 2, 15, 17 | `okx_connector_test.cpp`, blackbox |
| New order — Market (happy path) | 15 | `rest_api_test.cpp`, `okx_wire_test.cpp` |
| Cancel order | 6, 8, 18 | `okx_connector_test.cpp`, blackbox |
| Replace / amend order | 5 | `okx_connector_test.cpp`, blackbox |
| Amend unknown order -> 404 | 10 | `rest_api_test.cpp` |
| Amend terminal order -> 409 | 14 | `rest_api_test.cpp` |
| States: live / canceled / filled / rejected | 2, 6, 14, 15, 13 | `oms_test.cpp`, `order_state_test.cpp` |
| State: partially filled | — (1) | `oms_test.cpp` |
| clientOrderId -> exchangeOrderId mapping | 2, 4, 15 | `oms_test.cpp` |
| Out-of-order WS messages | — (1) | `oms_test.cpp`, `order_state_test.cpp` |
| Duplicate execution reports | 4, 8 (2) | `oms_test.cpp` |
| REST vs WebSocket race conditions | — (1) | `okx_resilient_test.cpp` |
| Idempotent client retries (place / cancel / across restart) | 4, 8, 16 | `oms_test.cpp` |
| Risk: max order size per instrument | 12 | `risk_test.cpp` |
| Risk: max notional per order | 12 | `risk_test.cpp` |
| Risk: per-instrument position limit | 12 | `risk_test.cpp` |
| Rejected orders return clear reasons | 11, 12, 13 | `risk_test.cpp`, `rest_api_test.cpp` |
| Persistence (append-only log) | 16 | `event_log_test.cpp` |
| Restart recovery: reload persisted orders | 16 | `event_log_test.cpp` |
| Reconcile with OKX open orders (adoption) | 17 | — |
| WS orders feed: login, subscribe, reports | 2, 3, 7, 14, 15, 18 | `okx_ws_client_test.cpp` |
| Real fills (fill quantity / average price) | 14, 15 | — |
| WS disconnects and reconnects | — (1) | `okx_resilient_test.cpp`, `okx_ws_client_test.cpp`, `binance_ws_client_test.cpp`, mocks |
| Venue unreachable -> retry budget -> 502 | 19 (both venues) | `retry_test.cpp`, `okx_resilient_test.cpp`, `binance_ws_client_test.cpp` |
| Binance adapter: WS submission, cancelReplace amend, user-data stream | 1–19 (binance) | `binance_connector_test.cpp`, `binance_ws_client_test.cpp`, `binance_wire_test.cpp`, `binance_signer_test.cpp`, mocks |
| Amend semantics per venue (same id vs new id) | 5 (both venues) | `binance_connector_test.cpp` |
| Cancel reports keyed by the original clientOrderId | 7 (binance) | `binance_ws_client_test.cpp` |
| Replace-lifecycle arbitration (superseded exchange ids) | 5–8 (binance) | `oms_test.cpp` |

Notes:

1. Not testable against the live venue (would require killing or fault-injecting the venue's servers, or non-deterministic partial fills). Covered by the deterministic rigs in `tests/` and `tests/blackbox/`.
2. The live suite exercises idempotent replay of client requests; duplicate WS report *handling* is covered deterministically in `oms_test.cpp`.

## Unit test scope (summary)

| Area | Tests |
|---|---|
| Order state machine | `order_state_test.cpp`, `oms_test.cpp` |
| Signing | `okx_signer_test.cpp`, `binance_signer_test.cpp` |
| Wire translation | `okx_wire_test.cpp`, `binance_wire_test.cpp` |
| Exchange clients | `okx_rest_client_test.cpp`, `okx_ws_client_test.cpp`, `binance_ws_client_test.cpp` |
| Resilience / retry | `okx_resilient_test.cpp`, `retry_test.cpp` |
| REST API layer | `rest_api_test.cpp` |
| Risk checks | `risk_test.cpp` |
| Persistence | `event_log_test.cpp` |
| Utilities | `decimal_test.cpp`, `result_test.cpp`, `clock_test.cpp`, `config_test.cpp` |
| Build / smoke | `smoke_test.cpp` |
