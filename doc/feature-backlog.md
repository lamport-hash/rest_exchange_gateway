# Feature Backlog — rest_exchange_gateway

Prioritized remaining work, distilled from the 2026-08-22 code review
(`doc/code-review.md`). Nothing here blocks the spec's required
deliverables — those are met (see the compliance matrix there).

## P1 — robustness fixes (small, high value)

- [x] OKX REST: classify 4xx as `protocol`/venue, not `transport` — a
      persistent auth failure currently burns the retry budget and looks
      like a network outage (`okx_rest_client.cpp:208`)
      *(done 2026-08-24: 5xx/429 stay transport; other 4xx parse the OKX
      envelope → `venue:<code>`, else `protocol`)*
- [x] OKX WS: replace throwing `json::value(key, default)` in login/
      subscribe ack parsing — a type-confused ack terminates the process
      (`okx_ws_client.cpp:226,245`)
      *(done 2026-08-24: non-throwing `checked_string`, session-fails and
      reconnects instead)*
- [x] Binance: handle `-1021` clock skew — serverTime offset sync and/or
      re-sign retry; today skew > recvWindow kills every signed request
      (`binance_ws_client.cpp:225`)
      *(done 2026-08-24: best-effort `time` sync at session start + sync/
      re-sign-once per signed call; persisting skew → `protocol`)*
- [x] Binance: map `-1006`/`-7` ("execution status unknown") to
      `transport` so they enter resolve-then-retry (`binance_ws_client.cpp:221`)
      *(done 2026-08-24)*
- [x] Binance: enable WS pong-timeout liveness (httplib
      `CPPHTTPLIB_WEBSOCKET_MAX_MISSED_PONGS > 0`) — half-open
      connections currently stall trading up to the 300 s read timeout
      *(done 2026-08-24: `wsPingIntervalSec=20` / `wsMaxMissedPongs=2`
      defaults + a bounded read deadline so a fully silent wire is
      observed)*
- [x] Binance: reset reconnect backoff after a healthy session
      (`connect_attempt` is monotone today) (`binance_ws_client.cpp:469`)
      *(done 2026-08-24: healthy = subscribed AND (response served OR
      30 s uptime); OKX got the same reset)*
- [ ] TLS server-cert verification on OKX `SSLClient`
      (`okx_rest_client.cpp:33`)
      *(verified 2026-08-24: premise false — the vendored httplib already
      defaults `server_certificate_verification_ = true` and nothing
      disables it; only an opt-out knob could be added, no robustness
      gain)*
- [x] Binance: verify `-4116` against the real Spot testnet (documented
      only in the mock); make the duplicate-resolve path tolerant of the
      actual code the venue sends
      *(done 2026-08-24 mock-side: resolve triggers on `-4116`, `-2010`
      or a duplicate/already-exists message; **live testnet verification
      of the actual code still open**)*

## P2 — spec deviations / semantics polish

- [ ] Widen `clientOrderId` charset to `[A-Za-z0-9-_]` (the spec's own
      example `order-123` is rejected today) — requires OMS/venue check
      (OKX clOrdId is alphanumeric-only, so map or validate per venue)
- [ ] OKX trades channel subscription (per-trade fills; today cumulative
      only via the orders channel)
- [ ] OKX open-orders pagination (`after` cursor) — reconcile sees only
      the first 100 pending orders
- [ ] Market-order notional check via a price snapshot (skipped today,
      documented limitation)
- [ ] Numeric (not string) amend-landing comparison on OKX to avoid
      redundant re-sends (`okx_resilient.hpp:196`)
- [x] In-flight duplicate place: return 202 or wait for the ack instead of
      the optimistic `201 live` with empty exchangeOrderId (`oms.cpp:287`)
      — DONE 2026-08-24: `OrderState::Pending` (gateway-local unacked
      stage, `place_submitted` persisted before the venue call, 202
      replays, `order_pending` 409 on cancel/amend, reconcile resolves)
- [ ] Consistent persistence-failure code (`persistence` on both paths;
      today the risk-reject path can surface `io` → 500 `internal`)
- [ ] `EventLog`: fail fast in the ctor if the file can't be opened;
      stream replay instead of whole-file read
- [ ] OKX enabled only when its config section is present (implicit
      default-enabled surprises binance-only deployments)
- [ ] `/health`: expose `reports_unknown` and `log_write_failures`

## P3 — stretch goals (spec §7, all unstarted)

- [ ] Rate-limit awareness per exchange (Binance `rateLimits` /
      `retryAfter`; OKX headers) — at minimum stop hammering on 429
- [ ] Sequence-number tracking + gap detection on WS feeds (Binance
      executionReports carry `u`/`s` transact sequences) — today gaps are
      only healed by reconnect-reconcile
- [ ] Backpressure between WS ingestion and venue calls beyond the
      `concurrency(2)` pool (two slow venue calls stall /health)
- [ ] Client-facing WebSocket order-update endpoint
- [ ] Property-based tests for the state machine / risk decimals
      (rapid-check-style or doctest-based generators)

## Hygiene

- [ ] Refresh stale counts in `doc/phase_2_functional_test.md`
      (32→44 blackbox assertions, 12→21 suites)
- [ ] Stray `*.secret` files in the tree root are gitignored — consider
      moving real credentials out of the repo entirely
- [ ] Consistent `ProtocolWarning` logging across adapters (OKX discards,
      Binance logs to stderr)
