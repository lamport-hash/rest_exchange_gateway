# Code Review — vs doc/project-spec.md

Date: 2026-08-22 · Tree: commit `015d676` + working changes reviewed here
(see §5 "Fixed during this review"). Method: full read of `src/rest/`,
`src/core/`, both adapters, `apps/gateway_main.cpp`; findings verified
against the code, not inferred. Companion docs: `doc/api.md` (API
reference + diagrams), `doc/feature-backlog.md` (prioritized remaining
work).

## 1. Spec-compliance matrix

| Spec requirement | Status | Notes |
|---|---|---|
| Single exchange-agnostic REST API, common schema | ✅ | strict field allowlist rejects exchange-specific fields (`order_routes.cpp:256`, `:416`) |
| Client never sends exchange-specific fields | ✅ | 400 names the offending fields |
| OKX: REST entry/cancel/amend + WS updates | ✅ | /api/v5/trade/* + private `orders` channel |
| Binance: WS submission + WS execution updates | ✅ | WS-API order.place/cancel/cancelReplace + user-data stream on the same connection |
| Adapters isolated behind common interface | ✅ | `ExchangeConnector` (`include/gateway/exchange_connector.hpp`); zero venue code above it except the composition root |
| New order (Market/Limit), Cancel, Amend | ✅ | POST/DELETE/PUT + registry listing GET |
| Normalized states + explicit mapping | ✅ | explicit per-adapter mapping tables (`okx_wire.cpp:21`, `binance_wire.cpp:182`), fail-closed on unknown venue states |
| clientOrderId → exchangeOrderId mapping | ✅ | plus full multi-id lifecycle for cancelReplace amends (`oms.hpp:38`) |
| Out-of-order WS, duplicates, REST-vs-WS races | ✅ | state machine + monotonic fill HWM + in-flight report buffering (`oms.cpp:561-618`) |
| Idempotent client retries | ✅ | registry replay incl. rejection replay; venue-side resolve-then-retry for transport-unresolved places |
| Append-only persistence + restart recovery | ✅ | JSONL, flush-per-append, torn-tail-safe replay (`event_log.cpp:59`) — no fsync (power-loss window) |
| Risk: max qty / max notional / position | ✅/⚠️ | exact decimals; **maxNotional skipped for market orders** (no price feed; documented `risk.hpp:49`) |
| WS disconnect/reconnect handling | ✅/⚠️ | backoff+jitter, re-subscribe, reconnect→reconcile; Binance pong-liveness never enabled (§3.B6) |
| Restart with live orders (reconcile) | ✅/⚠️ | adopt/refresh/absent-reject; OKX open orders **single page ≤100** |
| README / tests / examples deliverables | ✅ | 21 ctest suites, blackbox rig (44), live suites (85/venue), curl examples |

Overall: **spec-compliant** for everything the spec marks required; gaps are
documented trade-offs or listed in the backlog.

## 2. Findings — severity-ordered

Fixed items reference §5; everything else is open (see backlog).

### Critical

| # | Finding | Location | Status |
|---|---|---|---|
| C1 | `SymbolTranslator` data race: `to_wire`/`to_gateway` mutate two unordered_maps unlocked; called from 2 Crow workers + feed notifier thread — violates the interface thread-safety contract, real UB | `binance_wire.cpp:242-265`, contract at `exchange_connector.hpp:118` | **FIXED** (§5) |
| C2 | Unguarded `okx_connector` deref: config with an okx section lacking `apiKey` (e.g. binance-only deployment) → `bad_optional_access`/UB at startup | `gateway_main.cpp:135,161,170,191` | **FIXED** (§5) |

### High (open)

| # | Finding | Location |
|---|---|---|
| H1 | OKX REST: every non-200 (401, 400, 429…) is classified `transport` (retryable) — persistent auth failure burns the retry budget and surfaces as "cannot reach the venue"; 4xx should be `protocol`/venue | `okx_rest_client.cpp:208-211` |
| H2 | OKX WS: login/subscribe ack parsing uses throwing `json::value(key, default)` — a type-confused ack (key present, non-string) throws across the supervisor `jthread` → `std::terminate` | `okx_ws_client.cpp:226-227,245` |
| H3 | Binance: `-1021` (timestamp outside recvWindow) is terminal — clock skew > recvWindow makes every signed request fail permanently; no serverTime sync/re-sign retry | `binance_ws_client.cpp:225-232` |
| H4 | Binance: `-1006`/`-1007` ("execution status unknown, query it") not mapped to `transport` when delivered with 4xx status → bypasses resolve-then-retry | `binance_ws_client.cpp:221-224` |
| H5 | Binance: pong-timeout liveness never enabled (httplib default 0 = disabled, read timeout 300 s) — half-open connection stalls feed + all trading for up to ~5 min | `binance_ws_client.hpp:61`, `httplib.h:21490` |
| H6 | No TLS server-cert verification on OKX `SSLClient` (MITM exposure; spec excludes security but this is transport-integrity, not key management) | `okx_rest_client.cpp:33-35` |

### Medium (open)

| # | Finding | Location |
|---|---|---|
| M1 | Binance reconnect backoff never resets (`connect_attempt` monotone) — an early failure makes the next 24 h rotation reconnect at max backoff | `binance_ws_client.cpp:469-482` |
| M2 | OKX reconcile: open orders fetched as a single page (≤100, no `after` cursor); unnormalizable pending orders silently skipped | `okx_rest_client.cpp:299-318`, `okx_connector.cpp:162-172` |
| M3 | `venue:-4116` (duplicate open clientOrderId) is not in official Spot error docs — resolve path exercised only by the mock; a real duplicate may surface a different code as a definitive rejection | `binance_resilient.hpp:135-152` |
| M4 | Amend landing check is string-equality — venue numeric normalization (`"30000"` vs `"30000.0000"`) causes redundant (benign) re-sends | `okx_resilient.hpp:196-204` |
| M5 | In-flight duplicate place returns optimistic `201 live` with empty exchangeOrderId before the venue acks; a later rejection is only visible via GET/retry | `oms.cpp:287-292` |
| M6 | Reconcile "venue absent → Rejected" can mislabel an order that filled-and-archived at the venue while the gateway was down (acknowledged approximation) | `oms.cpp:759-773` |
| M7 | `clientOrderId` charset (alphanumeric only) rejects the spec's own example `"order-123"` — stricter than both venues | `order_routes.cpp:65-78` |
| M8 | `is_decimal` accepts zero — `quantity:"0"` passes REST validation ("positive decimal" message overpromises); relies on risk limits or the venue to catch it | `order_routes.cpp:46-63` |
| M9 | Risk-reject path returns raw `"io"` on log failure (→ 500 `internal`) vs the accepted path's `"persistence"` — inconsistent client-facing code | `oms.cpp:330-336` vs `:378-383`, `order_routes.cpp:155-158` |
| M10 | `EventLog` ctor doesn't verify the file opened — failures surface only at first append; `replay` loads the whole file into RAM | `event_log.cpp:11-14` |
| M11 | `concurrency(2)`: two slow venue calls occupy both Crow workers — `/health` and all REST stall; no backpressure | `gateway_main.cpp:188` |
| M12 | OKX implicitly enabled when the config has no okx section (`okx.empty()` → defaults) — surprising for binance-only deployments | `gateway_main.cpp:56` |

### Low (open)

- OKX `ProtocolWarning`s silently discarded — no logging hook (`okx_connector.cpp:229-231`); Binance logs them to stderr (inconsistent).
- Binance `get_open_orders` drops unparseable items without a warning (`binance_connector.cpp:157-162`).
- Binance snapshot avg fill price always empty — ignores available `cummulative_quote_qty` (`binance_connector.cpp:36`).
- `/health` hides `reports_unknown` and `log_write_failures` (present in `OmsStats`).
- `Result::value()/error()` use `assert` — vanish under NDEBUG (`result.hpp:38-53`).
- OKX WS login uses the wall clock directly; injected `timestamp_` member ignored (testability gap) (`okx_ws_client.cpp:210`).
- OMS error message hardcodes "(configured: okx, binance)" regardless of actual connectors (`oms.cpp:296`).
- Binance `stop()` races the supervisor's unlocked read/send (`binance_ws_client.cpp:125-129`) — relies on httplib internals.
- Stale counts in `doc/phase_2_functional_test.md` (32→44 assertions, 12→21 suites).
- Several `*.secret` files in the tree root (`brian_test_key.secret`, `okx-test.secret`, …) — verify gitignore intent.

## 3. Adapter annexes

### 3.A OKX (`src/exchange/okx/`)

Clean layering: wire → signer → REST client → resolve-then-retry policy →
WS feed → connector. Signing is doc-faithful (base64 HMAC over
`ts+method+path+body`, `OK-ACCESS-*`, demo header). Resolve-then-retry is
genuinely careful: transport failure → `GET /trade/order`; found →
synthesized ack; conclusively absent (51603) → identical re-send;
inconclusive → fail `transport` **without** re-send. Duplicate clOrdId
(51000) resolves to the existing order's ack. Cancel-of-canceled →
idempotent success; cancel-of-filled → rejection.

Spec gaps: **orders channel only — no trades channel** (fills surface as
cumulative `accFillSz`/`avgPx`, no per-trade data); no sequence/gap
detection (missing messages are only recovered by reconnect-reconcile).

### 3.B Binance (`src/exchange/binance/`)

Doc-faithful WS-API usage: one connection for requests + user-data stream
(`userDataStream.subscribe.signature` — no listenKey lifecycle needed),
id-correlated JSON frames, HMAC-hex over sorted `name=value&…` (official
test vector), `STOP_ON_FAILURE` cancelReplace as amend emulation with
stable clientOrderId. Reconnect: backoff+jitter, re-subscribe, pending
requests fail `transport` (outcome unknown) → resolved by the
resolve-then-retry engine (`-2013` absence, `-4116` duplicate,
`-2011` idempotent cancel). Notifier thread decouples feed delivery from
venue-I/O-doing connectivity handlers (documented regression fix).

Open defects: H3–H5, M1, M3 above; amend-resolution matches only
price+origQty (an amend to identical values can misreport; post-replace
`order.status` is ambiguous between the canceled original and the live
replacement sharing one clientOrderId).

## 4. Core/REST assessment

The OMS is the strongest part: single-mutex registry with venue I/O
deliberately outside the lock, in-flight staging that buffers reports
racing the ack, log-then-registry ordering, and one transition table
arbitrating every observation source. The REST layer is strict
(allowlists, structured errors, no exception crossing the boundary).
Design debt is documented honestly in the README's trade-offs section.
The state machine's `→ Rejected` edges from live states are reserved for
reconciliation only — venue reports can never "un-place" an order.

## 5. Fixed during this review

1. **C1** — `SymbolTranslator` now guards both memo maps with a
   `mutable std::mutex` (`binance_wire.hpp:166`,
   `binance_wire.cpp:233-268`); concurrent-hammer test added
   (`binance_wire_test.cpp`, 8 threads × 500 iterations, green under
   ASan+UBSan).
2. **C2** — all four `okx_connector` uses in `gateway_main.cpp` guarded
   with `has_value()` (mirrors the binance pattern).
3. WIP `GET /orders` listing (registry snapshot, sorted) reviewed:
   registration is sound (`/orders` vs `/orders/<string>` are distinct
   Crow trie paths; an earlier "duplicate registration crash" suspicion
   was **disproven** — HEAD already used same-URL different-method routes
   green). Endpoint kept and documented in `doc/api.md`.

Verification: `ctest --preset debug` (ASan+UBSan) **21/21** and
`ctest --preset release` **21/21**, in the Docker dev container, including
the new race test and the GET /orders integration tests.
