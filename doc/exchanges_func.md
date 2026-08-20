# Exchange-Connected Feature Test — Live OKX Demo Suite

How to run every gateway feature that involves a **real connection to the
exchange**, with **no mocks**. Companion documents:

- `doc/phase_1_functional_test.md` — first live-demo validation (manual curl)
- `doc/phase_2_functional_test.md` — deterministic black-box rig (mock venue)
- this document — the **live** suite: real OKX demo trading end to end

## 1. What is tested live

Everything in the gateway that talks to OKX (phases 1–3):

| Area | Feature | Live evidence checked |
|---|---|---|
| REST place | signed POST `/api/v5/trade/order` (HMAC over ts+method+path+body, `x-simulated-trading: 1`) | HTTP 201, venue `exchangeOrderId`, state `live` |
| REST status | signed GET `/api/v5/trade/order` | state/filled fields served through the OMS registry |
| REST amend | signed POST `/api/v5/trade/amend-order` via `PUT /orders/{id}` | amended price/quantity visible |
| REST cancel | signed POST `/api/v5/trade/cancel-order` | state `canceled`, idempotent re-cancel |
| WS feed | private WS `wss://wspap.okx.com:8443/ws/v5/private`: login (epoch timestamp), subscribe `orders` with `instType`, keepalive | `execution_report` log lines for place/amend/cancel/fill |
| OMS | clientOrderId idempotency, replayed outcomes, `order_terminal` on cancel-after-fill | `replayed:true`, 409 `order_terminal` |
| Schema | common-schema enforcement, exchange-field rejection, venue whitelist, timeInForce rules | 400 `invalid_request` |
| Risk | pre-trade maxQty/maxNotional/maxPosition (gateway-side) | 400 `risk_max_qty` |
| Venue errors | live OKX codes 51020 (min order amount), 51008-family insufficient funds | 409 `venue_rejected` |
| Real fill | marketable limit buy (+20% over last traded price) | state `filled`, non-zero `filledQuantity`, `averageFillPrice`, WS `filled` report |
| Persistence | JSONL event log, restart replay, idempotency across restart | order survives gateway restart, `replayed:true` after restart |
| Recovery | startup reconciliation: adopt orders placed directly on the venue (signed REST outside the gateway) | `"adopted":≥1` reconcile line, adopted order cancelable |
| Transport | venue unreachable (real closed port `127.0.0.1:9`, no mock) | 502 `venue_unavailable` within the retry budget |

Not testable against the live venue (covered deterministically elsewhere):

- WS server-side disconnect/reconnect and fault injection (dropped/delayed
  responses) — `tests/blackbox/phase2_client_tests.sh` (mock venue)
- exhaustive state-machine/risk/OMS unit behavior — `ctest` (17 suites)

## 2. Prerequisites

- Docker Engine + Docker Compose v2 (host); the dev container provides the
  toolchain, curl and python3.
- `config/gateway.json` (gitignored): copy `config/gateway.example.json`
  and fill in **OKX demo** credentials (apiKey/secretKey/passphrase with
  SPOT trade permission).
- Demo funds: the suite spends a few USDT (one marketable limit buy of
  0.00005 BTC) and leaves no live orders behind.
- Internet access to `www.okx.com` and `wspap.okx.com:8443`.

## 3. How to run

```bash
tests/live/okx_live_func_tests.sh                 # from the host, or
docker compose exec dev tests/live/okx_live_func_tests.sh
```

Optional argument: path to the live config (default
`config/gateway.json`). The script re-execs itself inside the dev
container when started on the host, builds `build/release/gateway` when
missing, and derives two throw-away configs in a temp dir:

- main instance: same credentials/venue as the user config, REST port
  18090, fresh persistence log, risk limits (maxQty 1 BTC) that allow the
  suite's small orders but reject one designated oversized order;
- "dead venue" instance: REST port 18091, okx host `127.0.0.1:9` (a real
  closed port — connection refused, no mock), WS disabled, tight retry
  budget.

Every order created is canceled or ends canceled/filled; on failure the
work dir (gateway log, configs, order log) is kept and its path printed.

## 4. Test sequence

| # | Section | Assertions (summary) |
|---|---|---|
| 1 | health | 200, `status:ok`, OMS stats fields |
| 2 | place + get | waits for the WS feed to be connected (2nd `reconcile` line) — the orders channel does not replay events missed before the subscription; then 201 + `exchangeOrderId`; GET 200 `live`, symbol echo |
| 3 | WS report (place) | `execution_report` line, state `live` |
| 4 | idempotent retry | re-POST → 201, `replayed:true`, same `exchangeOrderId` |
| 5 | amend | PUT price → 200; PUT price+qty → 200; GET shows new values |
| 6 | cancel | 200, same `exchangeOrderId`; GET `canceled` |
| 7 | WS report (cancel) | `execution_report` line, state `canceled` |
| 8 | idempotent cancel | second DELETE → 200 (registry replay) |
| 9–10 | unknown order | GET/DELETE → 404 `not_found` (no venue call) |
| 11 | validation | bad clientOrderId, `instrumentId` field, `venue:"binance"`, timeInForce-on-market → 400 |
| 12 | risk | qty > maxQty → 400 `risk_max_qty` |
| 13 | venue rejections | 51020 (below min order amount), insufficient funds → 409 `venue_rejected` |
| 14 | real fill | marketable limit buy → `filled`, non-zero `filledQuantity`, `averageFillPrice`, WS `filled` report; cancel-after-fill → 409 `order_terminal` |
| 15 | restart recovery | order survives restart (event-log replay + reconcile); retry after restart → `replayed:true` |
| 16 | adoption | order placed directly on the venue via signed REST (outside the gateway) is adopted by reconcile (persisted `adopted` event) and cancelable through the gateway |
| 17 | feed longevity | place/cancel reports still flow at end of suite (keepalive held) |
| 18 | dead venue | place → 502 `venue_unavailable` within budget (real closed port) |

Expected result (observed 2026-08-20 after the fixes in §5):
**68 passed, 0 failed** in ~2–3 minutes.

Run-to-run determinism notes (races the suite already handles internally):

- the first place waits for the WS subscription (OKX pushes no backlog);
- the fill test prices the order +0.3% over last — OKX rejects buys
  above the venue price band with `51137` (a +20% "aggressive" price is
  rejected, not filled);
- adoption is asserted via the persisted `adopted` event (with wait)
  because the adopt can land in either the startup or the
  WS-reconnect reconcile, whichever runs first.

## 5. Live findings fixed during this test

| # | Bug | Symptom (live) | Fix |
|---|---|---|---|
| 1 | WS login used the REST ISO-8601 timestamp | OKX `60004 Invalid timestamp` on every login; feed reconnect-flooded | epoch `secs.millis` login timestamp (`utc_now_epoch_ms`, src/core/clock.cpp) |
| 2 | Demo credentials sent to the production WS host | OKX `50101 APIKey does not match current environment` | `ws_host_for()`: `ws.okx.com` → `wspap.okx.com` when demoTrading (src/exchange/okx/okx_ws_client.cpp) |
| 3 | Subscribed `{"channel":"orders"}` without parameters | OKX `60018 ... channel:orders doesn't exist` | subscribe `orders` with `instType:"SPOT"` (verified live: bare channel and `instId` are both rejected) |
| 4 | Duplicate clientOrderId: live OKX answers `51016`, code only handled `51000` | 409 instead of idempotent 201 | resolved by the phase 3 OMS: clientOrderId replay serves the recorded outcome before any venue call |
| 5 | Cancel of already-canceled/unknown: live OKX answers `51400`, code handled only `51017` | 409 instead of idempotent 200/404 | same OMS-layer resolution (registry replay / `not_found`) |

Notes:

- The in-process mock (tests/mocks/) was aligned with the live behavior
  found in #3 (orders subscribe without instType now fails with 60018) and
  now records the login timestamp so the epoch format is unit-tested
  (tests/okx_ws_client_test.cpp, tests/clock_test.cpp).
- Finding #1 caused a reconnect flood (hundreds of connections) against
  ws.okx.com, which in turn made the OKX **demo** environment throttle all
  trade POST endpoints with HTTP 503 / error `50001 Service temporarily
  unavailable` for tens of minutes. Keep the WS credentials/host correct
  and the flood cannot recur; if the suite ever hits 502-everywhere with
  GETs still working, this venue-side throttle is the cause — wait it out.

## 6. Reproduce checklist

```bash
docker compose up -d dev
cp config/gateway.example.json config/gateway.json   # + fill demo credentials
tests/live/okx_live_func_tests.sh
docker compose exec dev ctest --preset debug && docker compose exec dev ctest --preset release
```

Verification state (2026-08-20): live suite 68/68, `ctest` 17/17 in both
presets (debug = ASan+UBSan), black-box rig 44/44.
