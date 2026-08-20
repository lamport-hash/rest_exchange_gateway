# Implementation Plan — rest_exchange_gateway

Status: **approved, not yet implemented** (decisions confirmed by user). Architecture: `doc/project-archi.md`; rules: `doc/general-guidelines.md`, `doc/code-guidelines.md`; behavior: `doc/project-spec.md`; testing: `test/testing-guidelines.md`.

## 1. Locked Decisions

| Area | Decision |
|---|---|
| Build system | CMake (apt, ≥3.28) + Ninja; `CMakePresets.json` with `debug` (ASan+UBSan) and `release` presets |
| Compiler | GCC 13.3 (g++ Ubuntu 24.04), C++20 only; `-Wall -Wextra -Wpedantic -Werror` |
| REST server | **Crow** (amalgamated `crow_all.h`, `CROW_ENABLE_SSL` via OpenSSL) |
| HTTP client | **cpp-httplib** (latest; OKX REST over TLS) |
| WebSocket client | **cpp-httplib** (its recent WS support) for OKX and Binance feeds |
| JSON | **nlohmann/json** (single header) |
| Tests | **doctest** (single header), run via `ctest` |
| Error handling | `Result<T>` (C++20 equivalent of `std::expected`, which is C++23 — not allowed per code-guidelines §1) |
| Formatting | `std::format` (GCC 13 supports it); `std::print` is C++23 → forbidden |
| Connectivity | Real demo environments: OKX demo trading (`x-simulated-trading: 1` header) and Binance Spot testnet (`testnet.binance.vision`); credentials via config file (insecure by design per spec — discuss secure handling in README). All tests mock the network. |
| Dependency delivery | Vendored single headers in `third_party/`; no vcpkg/conan available in this environment |

### Guideline overrides (user-approved)

- **Boost**: code-guidelines discourage Boost, but Crow requires Boost.Asio. Install `libboost-dev` + `libboost-system-dev` via apt (build-time dependency only, no Boost usage in our code). Fallback if this blocks: standalone Asio single header + `CROW_USE_BOOST=0`.
- Crow bundles RapidJSON inside `crow_all.h` for its own JSON macros; our code uses nlohmann/json exclusively.

## 2. Architecture Recap

Layers (details in `doc/project-archi.md`):

```
Client → Crow REST layer (src/rest/) → core (src/core/: state machine, risk, OMS, persistence)
                                        → IExchange adapters (src/exchange/okx/, src/exchange/binance/)
```

Layout per code-guidelines §2: `include/gateway/` (public headers), `src/{core,rest,exchange/<venue>/}`, `tests/`.

## 3. Implementation Steps (in order)

1. **Scaffolding**: apt install cmake/ninja; vendor `third_party/` headers (crow_all.h, httplib.h, json.hpp, doctest.h); root `CMakeLists.txt` + presets; `.clang-format` (per code-guidelines §5). Smoke test: build a minimal Crow app serving `/health` to validate the toolchain and all four dependencies before any domain code.
2. **Core types** (`src/core/`, public in `include/gateway/`): strong types (`OrderId`, `Price`, `Qty`, `Timestamp`), common schema structs, `OrderState` enum class, `Result<T>`, JSON config loader, structured logging. Unit tests.
3. **Order state machine**: explicit transition table (New/Live → PartiallyFilled → Filled; Canceled; Rejected); illegal transitions rejected. Exhaustive unit tests.
4. **Risk engine**: config-driven max qty / max notional per instrument, approximate per-instrument position limit from fills; clear reject reasons. Unit tests.
5. **Persistence**: append-only event log + startup replay. Replay tests.
6. **OMS registry**: `clientOrderId → exchangeOrderId` map, idempotent client retries, duplicate/out-of-order execution-report handling feeding the state machine. Unit tests incl. duplicate reports and REST-vs-WS races.
7. **Crow REST layer** (`src/rest/`): routes `POST /orders`, `GET /orders/{clientOrderId}`, `DELETE /orders/{clientOrderId}`, amend (`PUT /orders/{clientOrderId}`), `GET /health`; structured JSON errors (`{error: {code, reason, clientOrderId}}`); exceptions never cross the boundary (Crow error handler → structured 500). Integration-style tests: run Crow on an ephemeral port inside the test binary, drive it with cpp-httplib client.
8. **`IExchange` interface**: place/cancel/amend, execution-report subscription, reconcile snapshot. Exchange code confined to `src/exchange/<venue>/`.
9. **OKX adapter**: signed REST (`/api/v5/trade/*`: HMAC-SHA256 base64 of `timestamp+method+path+body`, `OK-ACCESS-*` headers, demo header), WS orders channel; symbol passthrough (`BTC-USDT`).
10. **Binance adapter**: WS-API `order.place` / `order.cancel` (signed requests, testnet `wss://testnet.binance.vision/ws-api/v3`), `cancelReplace` as amend emulation, symbol translation (`BTC-USDT` ↔ `BTCUSDT`), execution reports from user-data stream.
11. **Recovery orchestration**: startup = replay log → reconcile with OKX open orders → re-establish Binance WS state; WS disconnect/reconnect handling. Integration tests with simulator harness.
12. **Deliverables**: `examples/` curl scripts (place/cancel/amend on both venues), README rewrite (architecture, common schema, adapter design, idempotency/retry strategy, trade-offs & limitations), final pass: clang-format + full `ctest` in both presets.

Every step: functions get normal-case + dangerous-edge-case tests per `doc/general-guidelines.md`; consult user before architectural or core-object changes.

## 4. Verification

- `ctest --preset debug` (ASan+UBSan) and `--preset release` must be green before any step is considered done.
- Integration-style tests use an in-process **simulator harness** (scriptable fake REST/WS exchanges) — no live connectivity in tests, per `test/testing-guidelines.md`.

## 5. Risks & Fallbacks

| Risk | Fallback |
|---|---|
| cpp-httplib WS support is recent | Small hand-written RFC 6455 client over OpenSSL |
| Crow + Boost.Asio friction in this environment | Standalone Asio header + `CROW_USE_BOOST=0`; last resort: cpp-httplib server (already vendored) |
| Binance testnet WS-API needs user-provided API key/secret | Mocked in tests; live runs require keys in config |
| OKX demo trading needs demo credentials | Same as above |
