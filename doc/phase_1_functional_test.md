# Phase 1 Functional Test — Live OKX Demo Trading (todo 1.6)

Validation of the phase 1 slice (place / status / cancel through the
gateway) against the **OKX demo trading** environment (`x-simulated-trading:
1`). Live-run counterpart of the deterministic mock tests; the plan allows
this as the one exception to "no live connectivity in tests"
(`doc/implementation-plan.md` §1, connectivity decision).

- Date: 2026-08-20
- Result: **PASS**
- Commit: `8959826` (fixes) + `470d967` (scripts)
- Venue: `https://www.okx.com` (demo), instrument `BTC-USDT`, SPOT / `cash`

## 1. Prerequisites

Host: Docker Engine + Docker Compose v2 only. Everything else runs in the
dev container (Ubuntu 24.04 image: gcc 13, cmake, ninja, ccache, curl,
libssl).

Credentials: an OKX **demo** API key triple (apiKey / secretKey / passphrase)
with trade permission for SPOT. Demo funds must cover the order notional
(the demo wallet used here had ~22 USDT; the test buys 0.0001 BTC @ 10000 ≈
1 USDT notional).

## 2. Environment setup

```bash
# build dev image + start long-running container (first time)
docker compose build dev
docker compose up -d dev
```

Create the gitignored live config (copy `config/gateway.example.json`,
fill in the demo credentials):

```bash
cp config/gateway.example.json config/gateway.json
# edit config/gateway.json:
# {
#     "rest":  { "port": 8080 },
#     "okx": {
#         "apiKey":      "<OKX demo API key>",
#         "secretKey":   "<OKX demo secret key>",
#         "passphrase":  "<OKX demo passphrase>",
#         "host":        "www.okx.com",
#         "port":        443,
#         "useTls":      true,
#         "demoTrading": true
#     }
# }
```

## 3. Launch the gateway

```bash
./examples/run_gateway.sh config/gateway.json
```

Parameters: `[config_path]` (default `config/gateway.json`). The script
starts the dev container, builds `build/release/gateway` if missing, kills
any previous instance, launches the binary, and polls `GET /health` until
the gateway answers (10 s budget).

## 4. Functional test

### 4.1 Scripted sequence (place → status → cancel → status)

```bash
./examples/place_and_cancel.sh 8080 BTC-USDT 10000 0.0001
```

Parameters: `[port] [instrument] [price] [quantity]` — defaults
`8080 BTC-USDT 10000 0.0001` (price far below market so the order stays
`live`; size fits the demo wallet). The script generates an alphanumeric
`clientOrderId` (`gw` + nanosecond-clock digits — OKX rejects non-alphanumeric
IDs, gateway enforces 1–32 alphanum).

Expected output (observed):

```
== placing limit order: buy 0.0001 BTC-USDT @ 10000 (clientOrderId: gw226286376153943)
--- place [HTTP 201]
{ "clientOrderId": "gw226286376153943", "exchangeOrderId": "3849494857125384192" }
--- status before cancel [HTTP 200]
{ ..., "filledQuantity": "0", "price": "10000", "quantity": "0.0001", "state": "live" }
--- cancel [HTTP 200]
{ "clientOrderId": "gw226286376153943", "exchangeOrderId": "3849494857125384192" }
--- status after cancel [HTTP 200]
{ ..., "state": "canceled" }
```

### 4.2 Manual curl equivalents (run inside the container)

```bash
# place a limit buy (alphanumeric clientOrderId!)
docker compose exec dev curl -s -w "\nHTTP %{http_code}\n" -X POST http://127.0.0.1:8080/orders \
    -H 'Content-Type: application/json' \
    -d '{"clientOrderId":"gwlive001","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"10000","quantity":"0.0001"}'
# → 201 {"clientOrderId":"gwlive001","exchangeOrderId":"<19-digit ordId>"}

# order status
docker compose exec dev curl -s -w "\nHTTP %{http_code}\n" \
    "http://127.0.0.1:8080/orders/gwlive001?instrumentId=BTC-USDT"
# → 200 {... "state":"live" ...}

# cancel
docker compose exec dev curl -s -w "\nHTTP %{http_code}\n" -X DELETE \
    "http://127.0.0.1:8080/orders/gwlive001?instrumentId=BTC-USDT"
# → 200 {"clientOrderId":"gwlive001","exchangeOrderId":"..."}

# status after cancel
docker compose exec dev curl -s -w "\nHTTP %{http_code}\n" \
    "http://127.0.0.1:8080/orders/gwlive001?instrumentId=BTC-USDT"
# → 200 {... "state":"canceled" ...}

# health
docker compose exec dev curl -s -w "\nHTTP %{http_code}\n" http://127.0.0.1:8080/health
# → 200 {"status":"ok"}
```

### 4.3 Error paths checked live

```bash
# unknown order → 404 (venue envelope code 51603 mapped to not_found)
docker compose exec dev curl -s -w "\nHTTP %{http_code}\n" \
    "http://127.0.0.1:8080/orders/gwnope123?instrumentId=BTC-USDT"
# → 404 {"error":{"clientOrderId":"gwnope123","code":"not_found","reason":"order does not exist on the venue"}}

# hyphenated clientOrderId → 400 (gateway-side validation; live OKX rejects with 51000)
docker compose exec dev curl -s -w "\nHTTP %{http_code}\n" -X POST http://127.0.0.1:8080/orders \
    -H 'Content-Type: application/json' \
    -d '{"clientOrderId":"gw-live-1","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"10000","quantity":"0.0001"}'
# → 400 {"error":{"code":"invalid_request","reason":"clientOrderId must be 1-32 alphanumeric characters",...}}

# insufficient demo funds (e.g. qty 0.01 @ 10000 = 100 USDT) → 409 venue_rejected
# → 409 {"error":{"code":"venue_rejected","reason":"... [51008: Order failed. Your available USDT balance is insufficient ...]"}}
```

## 5. Deterministic regression suite (acceptance requirement)

Both presets must be green to close the phase (mocked network only):

```bash
docker compose exec dev cmake --preset debug   && docker compose exec dev cmake --build --preset debug   && docker compose exec dev ctest --preset debug
docker compose exec dev cmake --preset release && docker compose exec dev cmake --build --preset release && docker compose exec dev ctest --preset release
```

Observed: `100% tests passed, 0 tests failed out of 9` in both
(debug = ASan+UBSan).

## 6. Findings fixed during this test

| # | Bug | Symptom | Fix |
|---|-----|---------|-----|
| 1 | Duplicate `Content-Type` header (httplib `set_header` emplaces) | OKX `50006 Incorrect Content-Type`, HTTP 400 → gateway 502 | header passed only via `Post()` content-type arg (src/exchange/okx/okx_rest_client.cpp) |
| 2 | Wrong status endpoint `/api/v5/trade/order-info` (does not exist) | HTTP 404 → gateway 502 on every GET | correct endpoint is `GET /api/v5/trade/order` |
| 3 | Non-alphanumeric `clientOrderId` passed through | live OKX `51000 Parameter clOrdId error` | gateway validates 1–32 alphanum; mock aligned |
| 4 | Unknown order on live OKX = envelope `51603` (not `51016`, not empty data) | would surface as 409/500 | mapped to 404 `not_found`; mock returns `51603` |

Cleanup: any stray orders left on the demo account (e.g. from probing)
were canceled manually via the OKX API; the scripted sequence cancels its
own order.

## 7. Reproduce checklist

```bash
docker compose build dev && docker compose up -d dev
cp config/gateway.example.json config/gateway.json   # + fill demo credentials
./examples/run_gateway.sh config/gateway.json
./examples/place_and_cancel.sh 8080 BTC-USDT 10000 0.0001
docker compose exec dev ctest --preset debug
docker compose exec dev ctest --preset release
```
