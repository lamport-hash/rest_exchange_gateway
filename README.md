# rest_exchange_gateway

A REST gateway exposing **one unified order API over two exchanges**
(OKX and Binance). Clients speak a single schema; adapters translate to
each venue's native protocol and normalize everything back. C++20, Crow,
cpp-httplib, nlohmann/json, doctest.

Spec: `doc/project-spec.md` — Architecture: `doc/project-archi.md` —
Plan: `doc/implementation-plan.md` / `implement-todos.md`.
API reference + diagrams: `doc/api.md` — code review vs spec:
`doc/code-review.md` — remaining work: `doc/feature-backlog.md`.

## Architecture

```
            ┌────────────────────────────────────────────────┐
Client ─HTTP│ Crow REST layer (src/rest/)                     │
            │  one schema, one error format, zero venue code  │
            ├────────────────────────────────────────────────┤
            │ OMS core (src/core/)                            │
            │  clientOrderId registry + venue routing         │
            │  order state machine · pre-trade risk           │
            │  append-only event log (restart recovery)       │
            ├──────────────────┬─────────────────────────────┤
            │ ExchangeConnector│ (include/gateway/) — the ONLY │
            │ seam: place/cancel/amend/get/open-orders +      │
            │ execution-report & connectivity callbacks       │
            ├──────────────────┴─────────────────────────────┤
            │ OKX adapter (src/exchange/okx/)                │
            │  REST order entry + WS private orders feed      │
            ├────────────────────────────────────────────────┤
            │ Binance adapter (src/exchange/binance/)         │
            │  WS-API order entry + user-data-stream reports  │
            └────────────────────────────────────────────────┘
```

- Exchange-specific code lives **only** inside its adapter directory and is
  never referenced above `ExchangeConnector` (`include/gateway/exchange_connector.hpp`).
  The composition root (`apps/gateway_main.cpp`) is the single place that
  instantiates concrete connectors and registers them with the OMS
  (`{"okx", …}, {"binance", …}`).
- The OMS owns all state: one registry keyed by the globally unique
  `clientOrderId`; every record carries its venue so cancels, amends and
  reconciliation route correctly.

## Common order schema

```json
POST /orders
{
  "clientOrderId": "order-123",      // 1-32 alphanumeric, gateway-enforced
  "venue": "OKX | BINANCE",          // optional; default from config
  "symbol": "BTC-USDT",              // gateway spelling everywhere
  "side": "buy | sell",
  "type": "limit | market",
  "price": "30000",                  // required for limit, rejected for market
  "quantity": "0.1",
  "timeInForce": "GTC | IOC | FOK"   // optional, limit orders only
}
```

- Clients never send exchange-specific fields; unknown fields are rejected
  with a 400 that names them.
- Symbols stay `BTC-USDT` at the REST boundary; the Binance adapter
  translates to `BTCUSDT` on the wire and back on every response
  (`SymbolTranslator`, memoized with a quote-suffix fallback).
- Normalized states: `live`, `partially_filled`, `filled`, `canceled`,
  `rejected` — venue spellings map through explicit per-adapter tables
  (Binance `EXPIRED`/`EXPIRED_IN_MATCH`/`PENDING_CANCEL` → canceled, etc.).
- Full REST surface: `POST /orders`, `GET /orders` (registry listing),
  `GET /orders/{id}`, `DELETE /orders/{id}`
  (idempotent), `PUT /orders/{id}` (amend), `GET /price/{symbol}`
  (last-traded price, `?venue=`), `GET /health`.
  Errors are always `{"error":{"code","reason","clientOrderId"}}`.
  Endpoint-by-endpoint reference: `doc/api.md`.

## Adapter design (OKX vs Binance)

The two adapters implement the same interface over fundamentally different
transports, and both pair **request execution** with **resolve-then-retry
semantics** so an unknown outcome is resolved by lookup before any re-send:

| | OKX (`src/exchange/okx/`) | Binance (`src/exchange/binance/`) |
|---|---|---|
| Order entry | signed REST `POST /api/v5/trade/*` (HMAC-SHA256 **base64**, `OK-ACCESS-*` headers, demo header `x-simulated-trading: 1`) | signed WS-API requests `order.place` / `order.cancel` / `order.cancelReplace` (HMAC-SHA256 **hex** over the alphabetically sorted `name=value&…` payload) |
| Amend | native `POST /api/v5/trade/amend-order` (same orderId) | emulated with `order.cancelReplace` (`STOP_ON_FAILURE`): same clientOrderId, **new** exchange orderId; the OMS records the replacement id |
| Execution updates | private WS `orders` channel (login → subscribe → text ping/pong keepalive) | User Data Stream `executionReport` events subscribed **on the same WS-API connection** (`userDataStream.subscribe.signature`) |
| Unknown outcome | transport failure → `GET /api/v5/trade/order`; found → synthesized ack; conclusively absent → identical re-send; unresolved → fail `transport` **without** re-send | response timeout / disconnect / 5xx → `order.status`; same three-way resolution (`venue:-2013` = conclusive absence; duplicate-open-clientOrderId `-4116` → resolve to the existing order) |
| Reconnect | backoff+jitter, re-login, re-subscribe; watchdog closes silent connections | backoff+jitter, re-subscribe user-data stream; pending requests fail `transport`; late responses dropped by request-id |
| Idempotent cancel | cancel-of-canceled resolves to success; cancel-of-filled is rejected | same, via `order.status` after `-2011` |

Both feeds surface connectivity to the core, which **reconciles after every
reconnect** (missed fills are recovered, not guessed).

## Idempotency and retry strategy

Three layers, each with one job:

1. **Client idempotency (OMS).** A known `clientOrderId` replays its recorded
   outcome verbatim — identical ack or identical rejection — with no venue
   call. Risk-rejected and venue-rejected places are recorded as terminal
   `rejected` and replay deterministically. Only transport-unresolved places
   record nothing and let the client retry safely (layer 3).
2. **Report arbitration (state machine).** Duplicate, out-of-order and
   REST-vs-WS racing observations all funnel through one explicit
   transition table; illegal transitions are discarded and filled quantity
   is a monotonic high-water mark, so no report can regress an order.
3. **Resolve-then-retry (adapters).** Backoff+jitter with a wall-clock
   budget for pure transport failures; for actions with side effects the
   adapter first *resolves the true outcome* via a point query and only
   re-sends when the venue conclusively never saw the request. There is no
   code path that can double-place an order.

**Restart recovery:** startup replays the append-only JSONL log (torn-tail
safe), then reconciles with *every* venue: venue-live orders missing locally
are adopted, fills refreshed, non-terminal entries resolved (venue-absent →
terminal `rejected`; unreachable → kept, warned). Binance's execution state
is re-established by the user-data-stream subscription on the fresh
connection.

## Pre-trade risk

Config-driven per instrument (`risk.default` + `risk.instruments`), exact
fixed-point arithmetic (`src/core/decimal.cpp`, no floats):

- `maxQty` — order size cap
- `maxNotional` — price × quantity cap (skipped for market orders: no price
  feed pre-trade — documented limitation)
- `maxPosition` — worst-case |position| if every working order fully fills
  (fills so far + outstanding, signed by side, **summed across venues**)

Rejections return 400 with machine-readable `risk_*` codes and a
human-readable reason; amends re-run the checks against the amended values.

## Security discussion (credentials)

The spec explicitly scopes out secure key management: API keys/secrets are
read from a JSON config file in plain text, matching the "insecure by
design" allowance. For production, the defensible minimum would be: secrets
from a KMS/Vault with short-lived tokens, per-venue IP allow-lists, read of
secrets at startup only (never re-read), structured logs that never emit
keys or signatures (currently true), and separate trade-only keys with
withdrawals disabled (true for both OKX demo and Binance testnet keys).

## Testing

- `ctest --preset debug` (ASan+UBSan) and `--preset release` must both be
  green: **21 suites** — state-machine matrix, decimal/risk vectors, OMS
  (idempotency, dedup, races, recovery, reconciliation), per-adapter wire
  codecs (signer vectors straight from the official docs), and full
  connector behavior against **in-process mock venues** built from the
  official API docs (`tests/mocks/`): fault injection covers dropped
  requests, lost acks (outcome unknown), delayed responses, duplicate and
  dropped reports, WS kills and endpoint death/restart.
- Black-box client rig (`tests/blackbox/`, 44 assertions) drives the real
  gateway binary over HTTP against a standalone fake OKX venue.
- Live venue suites (`tests/live/live_func_tests.sh okx|binance`,
  85 assertions per venue, no mocks) and runbook
  (`doc/exchanges_func.md`).

All network-dependent behavior is mocked; CI never needs connectivity.

## Requirements

All installation, compilation, and testing happens **inside Docker** — nothing
needs to be installed on the host except:

- Docker Engine (with `docker` CLI)
- Docker Compose v2 (`docker compose`)

The dev image (Ubuntu 24.04) provides: gcc 13 (C++20), cmake, ninja, ccache,
clang-format, libboost-dev, libboost-system-dev, libssl-dev, zlib1g-dev.

## Development workflow (Docker)

Build the dev image and start the long-running dev container:

```bash
docker compose build dev
docker compose up -d dev
```

Compile / test inside the container (source is bind-mounted, so edits on the
host are picked up immediately; `build/` and the ccache directory live in
named volumes and survive container rebuilds):

```bash
docker compose exec dev bash                # interactive shell
# --- or one-shot commands ---
docker compose exec dev cmake --preset debug
docker compose exec dev cmake --build --preset debug
docker compose exec dev ctest --preset debug

docker compose exec dev cmake --preset release
docker compose exec dev cmake --build --preset release
docker compose exec dev ctest --preset release
```

- `debug` preset: `-O0 -g` + AddressSanitizer + UndefinedBehaviorSanitizer
- `release` preset: optimized build
- Both: C++20, `-Wall -Wextra -Wpedantic -Werror`, Ninja, ccache

Format code:

```bash
docker compose exec dev clang-format -i src/**/*.{hpp,cpp} tests/**/*.hpp tests/**/*.cpp apps/*.cpp
```

## Running the gateway

Needs `config/gateway.json` (copy `config/gateway.example.json`, fill in
venue credentials; the file is gitignored). Each venue section is optional
but at least one must be present — with both, the gateway routes on the
`venue` field and uses `defaultVenue` when it is omitted:

```bash
examples/run_gateway.sh                      # or manually:
docker compose exec dev ./build/release/gateway config/gateway.json
```

- OKX: demo trading (`x-simulated-trading: 1`) against `www.okx.com`
- Binance: Spot **testnet** (`wss://ws-api.testnet.binance.vision/ws-api/v3`)

Exercise both venues through the single API (place, amend, cancel on each,
plus a default-venue place):

```bash
examples/place_amend_cancel_both_venues.sh
examples/place_and_cancel.sh                 # single-venue variant
```

Black-box client suite against the real binary + fake venue:

```bash
tests/blackbox/run_docker_client.sh          # 44 assertions, ~10 seconds
```

Live venue suites (need real demo/testnet credentials in the config):

```bash
tests/live/live_func_tests.sh okx            # OKX demo trading
tests/live/live_func_tests.sh binance        # Binance spot testnet
                                             # runbook: doc/exchanges_func.md
```

## Monitor UI + test launcher (compose apps)

Besides the `dev` container, compose runs the gateway and a small web UI as
apps sharing the same image, source tree, and build volume:

```bash
docker compose up -d                         # dev + gateway + ui + edge
# UI:      http://<host>:8090
# Gateway: http://<host>:8080  (config/gateway.json.secret)
```

- **`edge` service** — the only component with published ports
  (8080/8090): Caddy drops (`403`) every request whose source IP is not
  in the allowlist, then reverse-proxies to `gateway`/`ui`, which are
  otherwise compose-internal. The allowlist lives in the gitignored
  `.env` (`ALLOWED_IPS=1.2.3.4 5.6.7.8`, space-separated); change it
  with `docker compose up -d edge` afterwards. Private ranges
  (`172.16.0.0/12`, `127.0.0.1`) stay allowed so host-side curl,
  `examples/*.sh` and the test rigs keep working. Traffic is plain
  HTTP by choice (no domain → no clean TLS) — fine for demo/testnet
  use; for anything sensitive put TLS + auth in front (Caddy
  `basic_auth` + `tls internal`, or a domain + Let's Encrypt).

- **`gateway` service** — builds `build/release/gateway` on first start
  (`tools/run-gateway-app.sh`), then runs it with `config/gateway.json.secret`;
  stdout is mirrored to `data/gateway-stdout.jsonl` for the UI event ticker.
  Stop it and the edge (`docker compose stop gateway edge`) before
  `examples/run_gateway.sh` to avoid competing for port 8080.
- **`ui` service** — FastAPI backend (`ui/app.py`) + one static page
  (`ui/static/`), organized in tabs: **Monitor** (health badge, WS feed
  state derived from `reconcile` / `feed_disconnected` events, OMS stats,
  event ticker, orders table refreshed every 2s), **API** (endpoint
  reference with field rules and curl examples from `doc/api.md`, a live
  playground that proxies hand-crafted requests to the gateway
  (`POST /api/proxy`) with status/latency/JSON-highlighted responses, and
  an order-state distribution chart), **Balance** (OKX demo-trading
  balance adjustments through the gateway), **Risk** (read-only view of
  the active pre-trade limits via `GET /risk` — defaults plus
  per-instrument overrides, fixed by the config file at startup — and the
  recent `risk_*` rejections with their recorded reasons from the order
  registry), **Diagrams** (architecture, order
  state machine, place idempotency and restart-recovery mermaid diagrams,
  rendered offline from a vendored `mermaid.min.js`), and **Tests** — all
  21 ctest suites in both presets (debug = ASan+UBSan), the black-box
  rig, and the live venue suites, each with a Run button, live log tail,
  parsed outcome summary and duration. Tests execute in the UI container,
  one at a time (the suites claim fixed ports); live rows ask for
  confirmation because they spend demo/testnet funds. Run history
  persists under `data/ui-runs/`.

Stop everything (named volumes `build/` and `ccache/` are kept):

```bash
docker compose down
```

## Trade-offs and known limitations

- **Single OMS mutex.** Venue calls are made under the registry lock — a
  slow venue delays other orders. Deliberate: no re-validation races, and
  the REST layer only ever waits on this one mutex. Hot-path lock-free
  structures were out of scope (spec: no ultra-low-latency target).
- **Binance amend is cancel+replace**, not an in-place amend: the exchange
  orderId changes and the old order's fills are not carried over
  (partially-filled amends re-quote the remaining size as a fresh order).
  The unified `clientOrderId` keeps the client view coherent.
- **Market-order notional check skipped** (no price feed pre-trade).
- **Position limit is approximate** (worst-case projection, no netting
  against balances), and cross-venue by design — conservative by summing.
- **openOrders pagination**: OKX pending orders are fetched as a single
  page (≤100, documented); Binance returns all.
- **Symbol translation is heuristic for unseen manual orders** on Binance
  (longest quote-suffix match); gateway-placed orders are always memoized
  exactly.
- **24h Binance connection lifetime** and venue `serverShutdown` events are
  handled by unbounded reconnect with backoff, not graceful pre-emptive
  rotation.
- **Credentials in plain-text config** — see the security discussion above.
- `std::print`/`std::expected` (C++23) are deliberately avoided; the
  codebase is strict C++20 with a local `Result<T>`.
