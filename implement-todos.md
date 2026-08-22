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
- [x] 1.1 Scaffolding: CMake + Ninja, debug/release presets (C++20,
      -Wall -Wextra -Wpedantic -Werror), third_party/ (crow_all.h, httplib.h,
      json.hpp, doctest.h), .clang-format; Crow /health smoke test
      (Docker dev environment added: Dockerfile + docker-compose.yml, all
      build/test runs inside the container)
- [x] 1.2 Raw OKX REST client (no abstraction yet): HMAC-SHA256 signing,
      OK-ACCESS-* headers, x-simulated-trading demo header; place / cancel /
      get-order against /api/v5/trade/*
- [x] 1.3 Mock OKX (in-process, from official v5 docs): trade/order,
      trade/cancel-order, trade/order-info; error envelope (code/sMsg);
      basic partial-fill engine; deterministic unit tests for the client
      (amend-order route included)
- [x] 1.4 ExchangeConnector thin interface extracted from the working client:
      place/cancel/amend/get + execution-report callback; OKXConnector
      implements it; exchange code confined below the interface
- [x] 1.5 Thin Crow slice wired end-to-end: POST /orders, GET /orders/{id},
      DELETE /orders/{id} → OKXConnector; structured JSON errors
- [x] 1.6 Live validation on OKX demo trading (credentials in config)
      Acceptance: place/cancel/status through the gateway over HTTP on OKX
      demo; mock tests green in both presets
      (2026-08-20: pass — POST/GET/DELETE /orders + /health against
      www.okx.com demo; three live-behavior bugs found and fixed: duplicated
      Content-Type header from httplib emplace semantics, wrong order-info
      path — correct endpoint is GET /api/v5/trade/order, and clOrdId must be
      1-32 alphanumeric; mock aligned with live behavior incl. 51603)

## Phase 2 — Resilience for the OKX connector
- [x] 2.1 Exponential backoff + jitter retry policy for REST (configurable
      caps, per-request budget)
      (src/core/retry.{hpp,cpp}: RetryPolicy + injectable RetryClock;
      "okx":{"retry":{...}} config; attempts, wall-clock budget, jitter
      bounds; shared by REST verbs, unknown-outcome resolution and WS
      reconnect backoff)
- [x] 2.2 OKX WebSocket orders channel: subscribe, normalize updates,
      disconnect detection, reconnect with backoff, resubscribe, ping/pong
      (src/exchange/okx/okx_ws_client.{hpp,cpp}: login via WS signature,
      "orders" channel, app-level text ping/pong, inbound-traffic watchdog,
      unbounded reconnect with backoff+jitter, re-login+resubscribe;
      ExchangeConnector gained start()/stop(); execution reports normalized
      and forwarded to the handler; gateway_main logs them as JSON lines)
- [x] 2.3 Unknown-outcome handling: place timeout → get-order resolution;
      idempotent clientOrderId retries (same request twice → same outcome)
      (src/exchange/okx/okx_resilient.hpp: resolve-then-retry engine —
      transport failure → lookup: found → synthesized ack, absent → safe
      re-send, unresolved → transport error without re-send (no
      double-place); venue 51000 duplicate-clOrdId → existing order's ack;
      cancel of canceled order → idempotent success; cancel of filled →
      rejection; amend matched against snapshot; GET retries transport
      directly)
- [x] 2.4 Fault-injection tests on mock OKX: dropped, delayed, duplicate and
      out-of-order messages; WS killed mid-stream
      (OkxMockServer: drop_next_request / drop_next_response (processed but
      ack lost) / delay_next_request; OkxMockWsServer: login/subscribe acks,
      scripted pushes, duplicate pushes, dropped updates, kill_connections,
      abrupt endpoint death + restart_on_same_port, ping silence; 3 new test
      binaries: retry_test, okx_resilient_test, okx_ws_client_test +
      connector/rest fault suites — 12 tests total)
      Acceptance: connectivity failures → gateway recovers, no lost or
      double-applied orders; both presets green
      (2026-08-20: pass — ctest debug (ASan+UBSan) and release 12/12)
      (2026-08-20: black-box client suite added — tests/blackbox/
      run_docker_client.sh launches a one-shot docker client container that
      starts mock_okx_env (standalone fake venue: REST+WS+fault control
      plane) + the real gateway binary and asserts all phase-2 points over
      HTTP; 32 assertions green)

## Phase 3 — Order state machine, OMS, recovery, full REST surface
- [x] 3.1 Normalized OrderState machine (Live, PartiallyFilled, Filled,
      Canceled, Rejected) with explicit OKX mapping table; illegal transitions
      rejected; exhaustive unit tests
      (src/core/order_state.hpp: explicit 9-entry transition table +
      is_terminal/can_transition/apply_transition; OKX mapping in
      okx_wire.cpp map_okx_state/map_okx_side + connector snapshot mapping;
      order_state_test verifies the full 5x5 matrix)
- [x] 3.2 OMS registry: clientOrderId→exchangeOrderId map, duplicate-report
      dedup, out-of-order arbitration, REST-vs-WS race handling
      (src/core/oms.{hpp,cpp}: single-mutex registry; duplicate/stale/racing
      observations discarded by the state machine while filled quantity
      stays a monotonic high-water mark; reports_applied/stale/unknown
      stats; strict gateway-level idempotency — a known clientOrderId
      replays its recorded outcome (ack or rejection) without venue calls;
      venue-rejected and risk-rejected places are recorded as terminal
      Rejected and replay deterministically; transport-unresolved places
      record nothing and let the venue-side engine resolve retries)
- [x] 3.3 Append-only persistence log + startup replay (truncated-tail safety)
      (src/core/event_log.{hpp,cpp}: JSONL, flush on append; replay drops an
      unterminated tail and truncates the file back to the last complete
      event, while mid-file corruption fails startup; OMS persists
      place_accepted/adopted/rejected/amended/state events and rebuilds the
      registry on load_from_log)
- [x] 3.4 Startup reconciliation with OKX open orders (orders-pending);
      restart-with-live-orders drill
      (ExchangeConnector::get_open_orders() + orders-pending REST endpoint;
      reconcile() adopts venue-live orders missing locally, refreshes fills,
      resolves non-terminal entries; venue-absent → terminal Rejected
      (venue_absent), unreachable → kept Live + warned; reconciliation also
      runs on every WS reconnect via set_connectivity_handler; drills in
      oms_test, rest_api_test and blackbox point 14)
- [x] 3.5 Pre-trade risk checks: max qty per instrument, max notional,
      approximate position limit; clear reject reasons to the client
      (src/core/risk.{hpp,cpp} + src/core/decimal.{hpp,cpp} exact scaled
      arithmetic; risk config {"default","instruments"} in gateway config;
      worst-case projection = executed fills + outstanding of working
      orders + candidate, signed by side (hedges net out); notional skipped
      for market orders (no price feed) — documented; REST rejects with 400
      + machine-readable risk_* codes (Crow v1.2.0 cannot emit 422); amend
      re-runs the checks against the amended quantity)
- [x] 3.6 Complete client-facing REST API: PUT /orders/{id} (amend),
      GET /health, full error schema {error:{code,reason,clientOrderId}};
      reject exchange-specific fields; integration tests (ephemeral-port Crow
      driven by cpp-httplib)
      (spec schema: symbol + optional venue ("OKX" only until phase 4) +
      optional timeInForce GTC/IOC/FOK → OKX tdIf (limit orders only);
      strict field allowlist on POST and PUT; GET served from the OMS
      registry (WS-fed), DELETE idempotent, PUT amend; /health carries
      registry stats; blackbox suite extended to 44 assertions incl. amend,
      risk rejection and a kill/restart recovery drill)
      Acceptance: restart/reconcile drills pass on mock; state-machine tests
      exhaustive; both presets green
      (2026-08-20: pass — ctest debug (ASan+UBSan) and release 17/17
      (decimal, order_state, event_log, risk, oms + updated okx/rest
      suites); black-box client suite 44/44)

## Phase 4 — Second exchange (Binance WS) + truly agnostic gateway
- [x] 4.1 Mock Binance (in-process, from official docs): WS-API order.place /
      order.cancel, executionReport stream, fault injection
      (tests/mocks/binance_mock_ws_server.{hpp,cpp}: one JSON request per
      text frame with id correlation, SIGNED param verification incl.
      recvWindow freshness (-1022/-2014/-1021), duplicate-open-clientOrderId
      -4116, cancelReplace, openOrders.status; scripting: lost acks
      (set_drop_next_response — outcome unknown), delayed replies, fills,
      dropped/duplicated execution reports, kill_connections + abrupt
      endpoint death/restart)
- [x] 4.2 BinanceConnector fully coded: signed WS-API requests (testnet
      wss://ws-api.testnet.binance.vision/ws-api/v3), execution reports from
      user-data stream, symbol translation BTC-USDT↔BTCUSDT, amend emulation
      via cancelReplace
      (src/exchange/binance/: signer — HMAC-SHA256 hex over the sorted
      name=value payload, doc-vector tested; wire codecs + explicit state
      mapping (EXPIRED/EXPIRED_IN_MATCH/PENDING_CANCEL→canceled); config
      with recvWindow/requestTimeout/retry; BinanceWsClient — id-correlated
      request/response over one connection, user-data stream subscribed on
      the same session (userDataStream.subscribe.signature), serverShutdown/
      eventStreamTerminated handling, backoff+jitter reconnect with
      re-subscribe, pending requests fail transport (outcome unknown), 5xx
      → transport per docs; resolve-then-retry place/cancel/amend mirroring
      the OKX engine (venue:-2013 conclusive absence, -4116 duplicate
      resolve, -2011 idempotent cancel resolution, cancelReplace partial
      outcome surfaced); BinanceConnector over the unchanged
      ExchangeConnector interface)
- [x] 4.3 Venue-agnostic gateway: routing on `venue` field, per-venue
      config/limits, zero exchange code above the connector interface;
      startup re-establishes Binance WS state
      (OMS holds map<venue, ExchangeConnector*>; OrderRecord/events persist
      the venue; cancel/amend/reconcile route via the record; reconcile
      iterates every venue; risk projection sums instruments across venues;
      ExchangeConnector::AmendRequest gained optional side/type/timeInForce
      for cancel+replace venues (user-approved); OkxConnector::get_order now
      normalizes conclusive absence (51603) to nullopt like Binance -2013;
      REST: venue validated against configured venues (case-insensitive,
      listed in the error), responses carry "venue"; config: "binance"
      section + "defaultVenue"; gateway_main is the composition root)
- [x] 4.4 Deliverables: README (architecture, common schema, adapter design,
      idempotency/retry strategy, auth-security discussion, trade-offs &
      limitations); examples/ curl scripts for both venues
      (place_amend_cancel_both_venues.sh + place_and_cancel.sh); final pass:
      clang-format whole tree + full ctest both presets
      (21/21 suites green debug+release; black-box suite 44/44 — plus a rig
      fix: wait for the feed's first-connect reconcile before the venue-
      traffic assertions, which used to race and flake)
- [ ] 4.5 Stretch (optional): rate-limit awareness per exchange, sequence/gap
      detection, backpressure, property-based state-machine tests
      Acceptance: all project-spec deliverables present; both venues work
      through the single unified API
      (2026-08-20: met for OKX (live demo + mock suites); Binance verified
      against the doc-faithful mock (testnet needs user keys in config)

## Post-delivery review (2026-08-22)
- [x] Full code review vs spec: `doc/code-review.md` (compliance matrix +
      severity-ordered findings; 2 critical bugs found and fixed:
      SymbolTranslator data race, unguarded okx_connector optional deref)
- [x] API reference + mermaid diagrams: `doc/api.md` (endpoints, schemas,
      error envelope, state machine, place idempotency + restart-recovery
      sequences); architecture diagram in `doc/project-archi.md`
- [x] GET /orders registry listing finished and documented (sorted
      snapshot, no pagination yet — see backlog)
- [x] Prioritized remaining work: `doc/feature-backlog.md` (P1 robustness
      fixes, P2 spec deviations, P3 stretch goals, hygiene)
      Verification: ctest debug (ASan+UBSan) 21/21 + release 21/21
