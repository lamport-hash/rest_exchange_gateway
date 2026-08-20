# Phase 2 Functional Test — Black-Box Client Rig (todos 2.1–2.4)

Validation of the phase 2 resilience slice (retry policy, WS orders feed,
resolve-then-retry idempotency, fault recovery) from **outside** the codebase:
a Docker container acting as a plain HTTP client drives the real gateway
binary against a scriptable fake OKX venue. Fully deterministic — no live
connectivity (unlike the phase 1 live-demo exception).

- Date: 2026-08-20
- Result: **PASS** (32 assertions)
- Commits: `0aa5758` (phase 2) + `3a9fd0d` (rig)

## 1. Rig architecture

`tests/blackbox/run_docker_client.sh` (host) starts a one-shot client
container that launches and wires three processes:

    client container ──curl──▶ gateway (real binary, REST :18081)
                                   │ REST + private WS
                                   ▼
                          mock_okx_env (fake OKX venue)
                                   ▲ fault-control plane (HTTP :18080)

- `mock_okx_env` — in-process mocks as a standalone binary: OKX REST trade
  endpoints, private WS (login/subscribe/orders push), plus a plain-HTTP
  control plane: `/fault/drop-next-request`, `/fault/drop-next-response`,
  `/fault/delay-next`, `/ws/push`, `/ws/kill`, `/ws/restart`,
  `/rest/stop|start`, `/stats` (key=value).
- Venue-side effects asserted via `/stats` (exact REST request counts prove
  no double-applied orders); gateway-side via its execution-report log.

## 2. How to run

```bash
tests/blackbox/run_docker_client.sh                              # host
docker compose exec dev tests/blackbox/phase2_client_tests.sh    # or
```

Observed: `== black-box client suite: 32 passed, 0 failed ==` (~10 s).

## 3. Coverage (phase 2 acceptance points)

| # | Point (todo) | Scenario | Expected / observed |
|---|---|---|---|
| 1 | — | `GET /health` | 200 `{"status":"ok"}` |
| 2 | — | place + get round-trip | 201, state `live` |
| 3 | 2.3 | same `clientOrderId` placed twice | both 201, same `exchangeOrderId`; venue saw 2 places + exactly 1 resolving lookup |
| 4 | 2.3 | place processed, ack dropped | 201 via lookup, **no re-send** (place count +1) |
| 5 | 2.1/2.3 | place dropped mid-response | 201 via safe re-send (place count +2) |
| 6 | 2.1 | venue response 800 ms > 400 ms read timeout | read timeout → retry → 201 |
| 7 | 2.3 | cancel twice | both 200 (idempotent), venue state `canceled` |
| 8 | 2.3 | cancel processed, ack dropped | 200 via lookup, no re-send (cancel count +1) |
| 9 | 2.2 | WS orders-channel push | execution report line in gateway log |
| 10 | 2.2 | WS killed mid-stream | reconnect → re-login → re-subscribe → updates flow |
| 11 | 2.4 | venue death → client call → venue reborn | 502 `venue_unavailable`, then 201 + `live` |

No lost or double-applied orders anywhere (checked by exact `/stats`
deltas after every fault).

## 4. Deterministic regression suite

```bash
docker compose exec dev ctest --preset debug     # ASan+UBSan
docker compose exec dev ctest --preset release
```

Observed: `100% tests passed, 0 tests failed out of 12` in both.

## 5. Findings fixed during this test

| # | Bug | Symptom | Fix |
|---|-----|---------|-----|
| 1 | `register_routes` used null member `server_` after refactor | `mock_okx_env` segfault at startup | handlers registered on the `a_server` parameter |
| 2 | gateway log not flushed | external client could not tail execution reports | `std::flush` on report lines + startup banner |
| 3 | test 10 pushed before resubscription completed | fill lost after reconnect (rig race, not a gateway bug) | script waits for `ws_subscribed >= 1` before pushing |

## 6. Reproduce checklist

```bash
tests/blackbox/run_docker_client.sh
docker compose exec dev ctest --preset debug
docker compose exec dev ctest --preset release
```
