# Exchange-Connected Feature Test — Live Venue Suites (OKX / Binance)

How to run every gateway feature that involves a **real connection to the
exchange**, with **no mocks**. Companion documents:

- `doc/phase_1_functional_test.md` — first live-demo validation (manual curl)
- `doc/phase_2_functional_test.md` — deterministic black-box rig (mock venue)
- this document — the **live** suites: real OKX demo and Binance spot
  testnet trading end to end, driven through one venue-agnostic script

## 1. What is tested live

Everything in the gateway that talks to a venue (phases 1–3). The REST
API under test is exchange-agnostic, so the suite is one parameterized
script — `tests/live/live_func_tests.sh okx|binance` — with per-venue
facts (filters, price bands, amend semantics) isolated in a parameter
table at the top of the script.

| Area | Feature | Live evidence checked |
|---|---|---|
| Order place | OKX signed REST POST `/api/v5/trade/order`; Binance signed WS-API `order.place` | HTTP 201, venue `exchangeOrderId`, state `live` |
| Amend | OKX in-place amend (same `exchangeOrderId`); Binance `order.cancelReplace` (NEW `exchangeOrderId`, same clientOrderId) | amended price/quantity visible; amend keeps/replaces the id per venue |
| Cancel | OKX REST cancel; Binance WS-API `order.cancel` | state `canceled`, idempotent re-cancel |
| WS feed | OKX private WS (login, subscribe `orders` SPOT, keepalive); Binance user-data stream on the WS-API connection | `execution_report` log lines for place/amend/cancel/fill |
| OMS | clientOrderId idempotency, replayed outcomes, `order_terminal` on cancel-after-fill, replace-lifecycle arbitration (Binance) | `replayed:true`, 409 `order_terminal` |
| Schema | common-schema enforcement, exchange-field rejection, venue whitelist, timeInForce rules | 400 `invalid_request` |
| Risk | pre-trade maxQty/maxNotional/maxPosition (gateway-side, both venues) | 400 `risk_max_*` |
| Venue errors | OKX 51020 (min order amount), insufficient funds; Binance NOTIONAL filter, insufficient balance | 409 `venue_rejected` |
| Real fill | marketable limit buy (+0.3% over last) on both venues | state `filled`, non-zero `filledQuantity`, `averageFillPrice`, WS `filled` report |
| Market order | market sell on both venues (base-ccy size, above the ~5 USDT market minimum) | 201 → `filled`, WS report |
| Persistence | JSONL event log, restart replay, idempotency across restart | order survives gateway restart, `replayed:true` after restart |
| Recovery | startup reconciliation: adopt orders placed directly on the venue (signed request outside the gateway) | `"adopted":≥1` reconcile line, adopted order cancelable |
| Transport | venue unreachable (real closed port `127.0.0.1:9`, no mock) | 502 `venue_unavailable` within the retry budget |

Not testable against the live venue (covered deterministically elsewhere):

- WS server-side disconnect/reconnect and fault injection (dropped/delayed
  responses) — `tests/blackbox/phase2_client_tests.sh` (mock venue)
- exhaustive state-machine/risk/OMS unit behavior — `ctest` (21 suites)

## 2. Prerequisites

- Docker Engine + Docker Compose v2 (host); the dev container provides the
  toolchain, curl and python3.
- `config/gateway.json` (gitignored): copy `config/gateway.example.json`
  and fill in credentials for the venue under test:
  - **OKX demo** — apiKey/secretKey/passphrase with SPOT trade permission
    (`x-simulated-trading`);
  - **Binance spot testnet** — apiKey/secretKey from
    testnet.binance.vision (the suite validates them with a signed
    read-only account query before starting).
- Demo funds: the suite spends a few USDT (one marketable limit buy of
  0.00011 BTC, then sells 0.0001 BTC back via a market order) and leaves
  no live orders behind. On Binance the position-limit test additionally
  locks ~0.13 BTC * far-price (~5600 USDT) while it runs, then cancels.
- Internet access to `www.okx.com` / `wspap.okx.com:8443` (OKX) and
  `ws-api.testnet.binance.vision` / `testnet.binance.vision` (Binance).

Venue facts the parameter table encodes (live-verified):

- OKX: BTC-USDT far-from-market price 10000 accepted; market minimum
  ~5 USDT; amend keeps the exchangeOrderId.
- Binance: `PERCENT_PRICE_BY_SIDE` band [0.5x avg, 2x avg] forces
  ticker-derived prices (0.55x last for resting orders); NOTIONAL filter
  min 5 USDT forces qty >= 0.0002 at that price; cancelReplace amend
  issues a NEW exchangeOrderId; cancel reports are keyed by the ORIGINAL
  clientOrderId (venue puts its cancel id in `c`, the original in `C`).

## 3. How to run

```bash
tests/live/live_func_tests.sh okx                     # OKX demo, or
tests/live/live_func_tests.sh binance                 # Binance testnet
docker compose exec dev tests/live/live_func_tests.sh okx
```

`tests/live/okx_live_func_tests.sh` remains as a thin wrapper for
`... okx`. Optional second argument: path to the live config (default
`config/gateway.json.secret`). The script re-execs itself inside the dev
container when started on the host, builds `build/release/gateway` when
missing, and derives two throw-away configs in a temp dir:

- main instance: same credentials/venues as the user config, REST port
  18090, fresh persistence log, risk limits tuned per venue (maxQty 1;
  OKX maxNotional 100000 / maxPosition 1, Binance maxNotional 1000000 /
  maxPosition 0.25) that allow the suite's small orders but reject one
  designated oversized order per check;
- "dead venue" instance: REST port 18091, the venue under test pointed
  at `127.0.0.1:9` (a real closed port — connection refused, no mock),
  WS disabled, tight retry budget.

Every order created is canceled or ends canceled/filled; on failure the
work dir (gateway log, configs, order log) is kept and its path printed.

## 4. Test sequence

| # | Section | Assertions (summary) |
|---|---|---|
| 1 | health | 200, `status:ok`, OMS stats fields |
| 2 | place + get | waits for the WS feed to be connected (2nd `reconcile` line) — the feed does not replay events missed before the subscription; then 201 + `exchangeOrderId`; GET 200 `live`, symbol echo |
| 3 | WS report (place) | `execution_report` line, state `live` |
| 4 | idempotent retry | re-POST → 201, `replayed:true`, same `exchangeOrderId` |
| 5 | amend | PUT price → 200; PUT price+qty → 200; GET shows new values; Binance asserts a NEW `exchangeOrderId` (cancelReplace), OKX asserts the same one |
| 6 | cancel | 200, same `exchangeOrderId` (Binance: the latest replacement's); GET `canceled` |
| 7 | WS report (cancel) | `execution_report` line, state `canceled` |
| 8 | idempotent cancel | second DELETE → 200 (registry replay) |
| 9–10 | unknown order | GET/DELETE/PUT → 404 `not_found` (no venue call) |
| 11 | validation | bad clientOrderId, `instrumentId` field, `venue:"kraken"`, timeInForce-on-market → 400 |
| 12 | risk | qty > maxQty, notional > maxNotional, projected position > maxPosition → 400 `risk_max_*` |
| 13 | venue rejections | OKX 51020 / Binance NOTIONAL filter, insufficient funds → 409 `venue_rejected` |
| 14 | real fill | marketable limit buy → `filled`, non-zero `filledQuantity`, `averageFillPrice`, WS `filled` report; cancel/amend-after-fill → 409 `order_terminal` |
| 15 | market order | market sell → `filled`, exact `filledQuantity`, `averageFillPrice`, WS report |
| 16 | restart recovery | order survives restart (event-log replay + reconcile); retry after restart → `replayed:true` |
| 17 | adoption | order placed directly on the venue via a signed request (outside the gateway) is adopted by reconcile (persisted `adopted` event) and cancelable through the gateway |
| 18 | feed longevity | place/cancel reports still flow at end of suite (keepalive held) |
| 19 | dead venue | place → 502 `venue_unavailable` within budget (real closed port) |

Expected result (observed 2026-08-21): **85 passed, 0 failed** per venue
in ~2–3 minutes.

Run-to-run determinism notes (races the suite already handles internally):

- the first place waits for the WS subscription (neither venue pushes a
  backlog);
- the fill test prices the order +0.3% over last — OKX rejects buys
  above the venue price band with `51137`, Binance enforces the 2x
  `PERCENT_PRICE_BY_SIDE` ceiling;
- adoption is asserted via the persisted `adopted` event (with wait,
  30s on Binance) because the adopt can land in a later reconcile while
  the venue session finishes subscribing.

## 5. Live findings fixed during these tests

| # | Bug | Symptom (live) | Fix |
|---|---|---|---|
| 1 | WS login used the REST ISO-8601 timestamp | OKX `60004 Invalid timestamp` on every login; feed reconnect-flooded | epoch `secs.millis` login timestamp (`utc_now_epoch_ms`, src/core/clock.cpp) |
| 2 | Demo credentials sent to the production WS host | OKX `50101 APIKey does not match current environment` | `ws_host_for()`: `ws.okx.com` → `wspap.okx.com` when demoTrading (src/exchange/okx/okx_ws_client.cpp) |
| 3 | Subscribed `{"channel":"orders"}` without parameters | OKX `60018 ... channel:orders doesn't exist` | subscribe `orders` with `instType:"SPOT"` (verified live: bare channel and `instId` are both rejected) |
| 4 | Duplicate clientOrderId: live OKX answers `51016`, code only handled `51000` | 409 instead of idempotent 201 | resolved by the phase 3 OMS: clientOrderId replay serves the recorded outcome before any venue call |
| 5 | Cancel of already-canceled/unknown: live OKX answers `51400`, code handled only `51017` | 409 instead of idempotent 200/404 | same OMS-layer resolution (registry replay / `not_found`) |
| 6 | Binance place required explicit `timeInForce` (amend already defaulted to GTC; OKX venue-defaults) | 500 `internal` "LIMIT orders require timeInForce" | place path defaults empty timeInForce to GTC (src/exchange/binance/binance_connector.cpp) |
| 7 | Connectivity handlers ran reconciliation on the Binance WS reader thread; reconcile's `openOrders.status` call waited for a response only that thread could dispatch | every place → 502 while orders landed on the venue; `pendingListingFailed`; disconnect storms | feed events are delivered on a dedicated notifier thread (src/exchange/binance/binance_ws_client.cpp) |
| 8 | OMS held its mutex across venue I/O; the Binance reader applying the racing executionReport (which precedes the API response frame) blocked behind it | place timed out (12s + 12s resolve) → 502 while the order was live on the venue | OMS lock-scope refactor: venue I/O outside the lock, in-flight staging buffers raced reports (src/core/oms.cpp) |
| 9 | Cancel reports keyed by the venue's auto-generated cancel id (`c`), original id in `C` | cancel `execution_report` lines under random ids; OMS `reports_unknown` | report normalization prefers the original clientOrderId in `C` (src/exchange/binance/binance_ws_client.cpp) |
| 10 | cancelReplace emits CANCELED (old leg) + NEW (replacement) under the same clientOrderId; the old leg's CANCELED terminalized the record | second amend → 409 `order_terminal` while the replacement was live | full exchange-order-id lifecycle per record; superseded legs contribute fills only, never state (src/core/oms.cpp) |

Notes:

- The in-process mock (tests/mocks/) was aligned with the live behavior
  found in #3 (orders subscribe without instType now fails with 60018) and
  now records the login timestamp so the epoch format is unit-tested
  (tests/okx_ws_client_test.cpp, tests/clock_test.cpp). Findings #7–#10
  each gained a deterministic regression test
  (tests/binance_ws_client_test.cpp, tests/binance_connector_test.cpp,
  tests/oms_test.cpp).
- Finding #1 caused a reconnect flood (hundreds of connections) against
  ws.okx.com, which in turn made the OKX **demo** environment throttle all
  trade POST endpoints with HTTP 503 / error `50001 Service temporarily
  unavailable` for tens of minutes. Keep the WS credentials/host correct
  and the flood cannot recur; if the suite ever hits 502-everywhere with
  GETs still working, this venue-side throttle is the cause — wait it out.
- The Binance REST host `api.testnet.binance.vision` does not resolve on
  every network; `testnet.binance.vision` (which proxies `/api/v3`) is
  used for the suite's direct signed REST helper instead. The gateway
  itself only needs `ws-api.testnet.binance.vision`.

## 6. Reproduce checklist

```bash
docker compose up -d dev
cp config/gateway.example.json config/gateway.json   # + fill venue credentials
tests/live/live_func_tests.sh okx
tests/live/live_func_tests.sh binance
docker compose exec dev ctest --preset debug && docker compose exec dev ctest --preset release
```

Verification state (2026-08-21): live suites 85/85 per venue (OKX demo,
Binance spot testnet), `ctest` 21/21 suites in both presets (debug =
ASan+UBSan), black-box rig 50/50.
