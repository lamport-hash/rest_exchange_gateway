#!/usr/bin/env bash
# Phase 2 black-box client test suite.
#
# Acts exactly like an external client: every check drives the REAL gateway
# binary over HTTP (curl), against a REAL fake venue process (mock_okx_env),
# both started locally by this script. Faults are injected through the mock's
# control plane; venue-side effects are asserted through /stats and the
# gateway's execution-report log. No in-process harness involved.
#
# Points covered (implement-todos.md, phase 2):
#   1  health
#   2  place / get round-trip
#   3  idempotent client retries (same clientOrderId twice -> same order)
#   4  place processed but ack dropped -> resolved via lookup, no re-send
#   5  place dropped entirely -> safe re-send
#   6  response slower than the read timeout -> retry succeeds
#   7  idempotent cancel (cancel twice -> both succeed)
#   8  cancel processed but ack dropped -> no re-send
#   9  WS orders channel: execution reports land in the gateway log
#   10 WS killed mid-stream -> reconnect + resubscribe -> updates flow again
#   11 venue death -> 502 (order stays pending) -> reconcile rejects venue_absent
#
# Usage: tests/blackbox/phase2_client_tests.sh   (expects curl + the built
#        release binaries; builds them when missing)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATEWAY_BIN="$ROOT/build/release/gateway"
MOCK_ENV_BIN="$ROOT/build/release/mock_okx_env"

CONTROL_PORT=18080
GW_PORT=18081
WORK="$(mktemp -d)"
GATEWAY_PID=""
MOCK_PID=""
PASS=0
FAIL=0

cleanup() {
    [ -n "$GATEWAY_PID" ] && kill "$GATEWAY_PID" 2>/dev/null
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

ok() {
    PASS=$((PASS + 1))
    echo "  PASS: $1"
}
bad() {
    FAIL=$((FAIL + 1))
    echo "  FAIL: $1"
}
assert_eq() { # description expected actual
    if [ "$2" = "$3" ]; then
        ok "$1"
    else
        bad "$1 (expected [$2] got [$3])"
    fi
}
assert_contains() { # description haystack needle
    if printf '%s' "$2" | grep -q "$3"; then
        ok "$1"
    else
        bad "$1 (missing [$3] in [$(printf '%s' "$2" | head -c 200)])"
    fi
}

# ---- helpers -------------------------------------------------------------
control() { # subpath [data]
    if [ $# -gt 1 ]; then
        curl -s -X POST --data "$2" "http://127.0.0.1:$CONTROL_PORT$1"
    else
        curl -s -X POST "http://127.0.0.1:$CONTROL_PORT$1"
    fi
}
stat_of() { # field
    curl -s "http://127.0.0.1:$CONTROL_PORT/stats" | grep "^$1=" | cut -d= -f2
}
wait_stat() { # field operator value timeout_seconds
    local deadline=$((SECONDS + $4))
    while [ $SECONDS -lt $deadline ]; do
        local current
        current="$(stat_of "$1")"
        if [ "$current" "$2" "$3" ]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}
wait_log() { # pattern timeout_seconds
    local deadline=$((SECONDS + $2))
    while [ $SECONDS -lt $deadline ]; do
        if grep -q "$1" "$WORK/gateway.log" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}
gw() { # METHOD path [json-body] -> sets STATUS and BODY
    if [ $# -gt 2 ]; then
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -X "$1" \
            -H 'Content-Type: application/json' -d "$3" \
            "http://127.0.0.1:$GW_PORT$2")"
    else
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -X "$1" \
            "http://127.0.0.1:$GW_PORT$2")"
    fi
    BODY="$(cat "$WORK/body")"
}
json_field() { # body field -> value of "field":"value"
    printf '%s' "$1" | grep -o "\"$2\":\"[^\"]*\"" | head -1 | cut -d'"' -f4
}
place_body() { # clientOrderId [quantity] -> request JSON
    if [ $# -gt 1 ]; then
        printf '{"clientOrderId":"%s","venue":"OKX","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"%s"}' "$1" "$2"
    else
        printf '{"clientOrderId":"%s","venue":"OKX","symbol":"BTC-USDT","side":"buy","type":"limit","price":"50000","quantity":"0.001"}' "$1"
    fi
}

# ---- build (if needed) ----------------------------------------------------
if [ ! -x "$GATEWAY_BIN" ] || [ ! -x "$MOCK_ENV_BIN" ]; then
    echo "== building release binaries =="
    (cd "$ROOT" && cmake --preset release >/dev/null &&
        cmake --build --preset release --target gateway mock_okx_env >/dev/null) ||
        {
            echo "build failed"
            exit 1
        }
fi

# ---- start the fake venue -------------------------------------------------
echo "== starting mock OKX venue (REST + WS + control plane) =="
"$MOCK_ENV_BIN" "$CONTROL_PORT" >"$WORK/mock_env.log" 2>&1 &
MOCK_PID=$!

for _ in $(seq 1 50); do
    STATUS_JSON="$(curl -s "http://127.0.0.1:$CONTROL_PORT/status" 2>/dev/null)"
    [ -n "$STATUS_JSON" ] && break
    sleep 0.1
done
[ -n "${STATUS_JSON:-}" ] || {
    echo "mock env did not start: $(cat "$WORK/mock_env.log")"
    exit 1
}
REST_PORT="$(printf '%s' "$STATUS_JSON" | grep '^rest_port=' | cut -d= -f2)"
WS_PORT="$(printf '%s' "$STATUS_JSON" | grep '^ws_port=' | cut -d= -f2)"
echo "   venue rest_port=$REST_PORT ws_port=$WS_PORT"

# ---- start the gateway ----------------------------------------------------
cat >"$WORK/gateway.json" <<EOF
{
    "rest": {"port": $GW_PORT},
    "persistence": {"logPath": "$WORK/orders.jsonl"},
    "risk": {"instruments": {"BTC-USDT": {"maxQty": "0.01", "maxNotional": "100000"}}},
    "okx": {
        "apiKey": "blackbox-key",
        "secretKey": "blackbox-secret",
        "passphrase": "blackbox-pass",
        "host": "127.0.0.1",
        "port": $REST_PORT,
        "useTls": false,
        "demoTrading": true,
        "restConnectTimeoutMs": 1000,
        "restReadTimeoutMs": 400,
        "retry": {"maxAttempts": 4, "initialBackoffMs": 20, "maxBackoffMs": 60,
                  "multiplier": 2.0, "jitter": 0.0, "budgetMs": 3000},
        "ws": {"enabled": true, "host": "127.0.0.1", "port": $WS_PORT,
               "useTls": false, "path": "/ws/v5/private",
               "pingIntervalMs": 200, "maxMissedPongs": 5}
    }
}
EOF

echo "== starting gateway =="
"$GATEWAY_BIN" "$WORK/gateway.json" >"$WORK/gateway.log" 2>&1 &
GATEWAY_PID=$!

for _ in $(seq 1 50); do
    gw GET /health && [ "$STATUS" = "200" ] && break
    sleep 0.1
done

# Wait for the execution feed's first connect: it fires a reconcile whose
# per-order lookups would otherwise race the venue-traffic assertions below.
for _ in $(seq 1 100); do
    [ "$(grep -c '"event":"reconcile"' "$WORK/gateway.log" 2>/dev/null || true)" -ge 2 ] && break
    sleep 0.1
done

# ---- 1. health ------------------------------------------------------------
echo "== 1. health =="
gw GET /health
assert_eq "GET /health -> 200" "200" "$STATUS"
assert_contains "health body ok" "$BODY" '"status":"ok"'

# ---- 2. place + get round-trip -------------------------------------------
echo "== 2. place and fetch an order =="
gw POST /orders "$(place_body gwT02)"
assert_eq "POST /orders -> 201" "201" "$STATUS"
assert_eq "exchangeOrderId assigned" "mock-1" "$(json_field "$BODY" exchangeOrderId)"
gw GET "/orders/gwT02"
assert_eq "GET /orders/gwT02 -> 200" "200" "$STATUS"
assert_eq "state is live" "live" "$(json_field "$BODY" state)"

# ---- 3. idempotent client retry ------------------------------------------
echo "== 3. same clientOrderId twice -> same outcome (strict idempotency) =="
P0="$(stat_of rest_place)"
G0="$(stat_of rest_get)"
gw POST /orders "$(place_body gwT03)"
assert_eq "first place -> 201" "201" "$STATUS"
ORD1="$(json_field "$BODY" exchangeOrderId)"
gw POST /orders "$(place_body gwT03)"
assert_eq "retry place -> 201 (replayed)" "201" "$STATUS"
assert_eq "replayed flag set" "true" "$(printf '%s' "$BODY" | grep -o '"replayed":[a-z]*' | cut -d: -f2)"
assert_eq "same exchangeOrderId" "$ORD1" "$(json_field "$BODY" exchangeOrderId)"
assert_eq "venue saw exactly 1 place" "$((P0 + 1))" "$(stat_of rest_place)"
assert_eq "venue saw no resolving lookup" "$G0" "$(stat_of rest_get)"

# ---- 4. place processed, acknowledgement dropped --------------------------
echo "== 4. place ack dropped -> lookup resolution, no re-send =="
P0="$(stat_of rest_place)"
G0="$(stat_of rest_get)"
control /fault/drop-next-response >/dev/null
gw POST /orders "$(place_body gwT04)"
assert_eq "place -> 201 despite dropped ack" "201" "$STATUS"
assert_eq "no re-send (place count +1 only)" "$((P0 + 1))" "$(stat_of rest_place)"
assert_eq "resolved via one lookup" "$((G0 + 1))" "$(stat_of rest_get)"
gw GET "/orders/gwT04"
assert_eq "exactly one live order at the venue" "live" "$(json_field "$BODY" state)"

# ---- 5. place dropped entirely -------------------------------------------
echo "== 5. place dropped -> safe re-send =="
P0="$(stat_of rest_place)"
control /fault/drop-next-request >/dev/null
gw POST /orders "$(place_body gwT05)"
assert_eq "place -> 201 after retry" "201" "$STATUS"
assert_eq "re-sent once (place count +2)" "$((P0 + 2))" "$(stat_of rest_place)"
gw GET "/orders/gwT05"
assert_eq "order is live" "live" "$(json_field "$BODY" state)"

# ---- 6. response slower than the read timeout -----------------------------
echo "== 6. delayed venue response -> timeout -> retry =="
control /fault/delay-next '{"ms":800}' >/dev/null # > 400ms read timeout
gw POST /orders "$(place_body gwT06)"
assert_eq "place -> 201 via retry" "201" "$STATUS"
gw GET "/orders/gwT06"
assert_eq "order is live" "live" "$(json_field "$BODY" state)"

# ---- 7. idempotent cancel --------------------------------------------------
echo "== 7. cancel twice -> both succeed =="
gw DELETE "/orders/gwT02"
assert_eq "first cancel -> 200" "200" "$STATUS"
gw DELETE "/orders/gwT02"
assert_eq "second cancel -> 200 (idempotent)" "200" "$STATUS"
gw GET "/orders/gwT02"
assert_eq "order canceled at the venue" "canceled" "$(json_field "$BODY" state)"

# ---- 8. cancel processed, ack dropped -------------------------------------
echo "== 8. cancel ack dropped -> no re-send =="
C0="$(stat_of rest_cancel)"
control /fault/drop-next-response >/dev/null
gw DELETE "/orders/gwT03"
assert_eq "cancel -> 200 despite dropped ack" "200" "$STATUS"
assert_eq "no re-send (cancel count +1 only)" "$((C0 + 1))" "$(stat_of rest_cancel)"

# ---- 9. WS execution reports ----------------------------------------------
echo "== 9. WS orders channel -> execution reports logged =="
wait_stat ws_subscribed -ge 1 10 || bad "gateway never subscribed to the orders channel"
control /ws/push '{"instId":"BTC-USDT","ordId":"mock-3","clOrdId":"gwT04","state":"partially_filled","side":"buy","px":"50000","sz":"0.001","accFillSz":"0.0005","avgPx":"50000"}' >/dev/null
if wait_log '"clientOrderId":"gwT04".*"state":"partially_filled"' 10; then
    ok "execution report (partially_filled) reached the gateway log"
else
    bad "no execution report for gwT04 in gateway log"
fi

# ---- 10. WS killed mid-stream ----------------------------------------------
echo "== 10. WS disconnect -> reconnect -> resubscribe =="
CONNS0="$(stat_of ws_connections)"
control /ws/kill >/dev/null
if wait_stat ws_connections -ge "$((CONNS0 + 1))" 10 &&
    wait_stat ws_logins_ok -ge 2 10 &&
    wait_stat ws_subscribed -ge 1 10; then
    ok "feed reconnected, re-logged-in and re-subscribed"
else
    bad "feed did not reconnect after kill"
fi
control /ws/push '{"instId":"BTC-USDT","ordId":"mock-3","clOrdId":"gwT04","state":"filled","side":"buy","px":"50000","sz":"0.001","accFillSz":"0.001","avgPx":"50000"}' >/dev/null
if wait_log '"clientOrderId":"gwT04".*"state":"filled"' 10; then
    ok "updates flow again after reconnect"
else
    bad "no execution report after reconnect"
fi

# ---- 11. venue death and recovery ------------------------------------------
# Since the Pending state (c2289cc): a place whose venue call fails on
# transport keeps its born-Pending record (intent persisted, visible via
# GET) and same-id retries replay 202 {state:pending} — the gateway never
# re-sends an unacked place itself. The next WS-reconnect reconcile looks
# the id up: conclusively absent (mock answers OKX 51603) -> terminal
# Rejected venue_absent. A fresh clientOrderId places normally again.
echo "== 11. venue death -> 502 -> pending replay -> reconcile rejects -> fresh id =="
control /rest/stop >/dev/null
gw POST /orders "$(place_body gwT11)"
assert_eq "place -> 502 while venue is dead" "502" "$STATUS"
assert_contains "structured error" "$BODY" '"code":"venue_unavailable"'
control /rest/start >/dev/null
gw POST /orders "$(place_body gwT11)"
assert_eq "same-id retry while unacked -> 202" "202" "$STATUS"
assert_eq "unacked replay state is pending" "pending" "$(json_field "$BODY" state)"
assert_eq "unacked replay carries no exchangeOrderId" "" "$(json_field "$BODY" exchangeOrderId)"
gw GET "/orders/gwT11"
assert_eq "order visible as pending after recovery" "pending" "$(json_field "$BODY" state)"
# WS reconnect triggers the reconcile that resolves the never-delivered order
control /ws/kill >/dev/null
ST="pending"
for _ in $(seq 1 100); do
    gw GET "/orders/gwT11"
    ST="$(json_field "$BODY" state)"
    [ "$ST" = "rejected" ] && break
    sleep 0.1
done
assert_eq "reconcile resolves venue-absent order to rejected" "rejected" "$ST"
assert_contains "rejection reason is venue_absent" "$BODY" '"code":"venue_absent"'
gw POST /orders "$(place_body gwT11b)"
assert_eq "fresh id -> 201 after venue recovery" "201" "$STATUS"
gw GET "/orders/gwT11b"
assert_eq "order is live after recovery" "live" "$(json_field "$BODY" state)"

# ---- 12. amend via PUT -------------------------------------------------------
echo "== 12. amend an order (PUT /orders/{id}) =="
gw PUT /orders/gwT05 '{"price":"49000","quantity":"0.002"}'
assert_eq "PUT /orders/gwT05 -> 200" "200" "$STATUS"
assert_eq "amended price visible" "49000" "$(json_field "$BODY" price)"
gw GET /orders/gwT05
assert_eq "GET after amend shows new price" "49000" "$(json_field "$BODY" price)"

# ---- 13. pre-trade risk ------------------------------------------------------
echo "== 13. risk rejects oversized orders without touching the venue =="
P0="$(stat_of rest_place)"
gw POST /orders "$(place_body gwT13 0.1)"
assert_eq "oversized place -> 400" "400" "$STATUS"
assert_contains "risk reject code" "$BODY" '"code":"risk_max_qty"'
assert_eq "venue untouched" "$P0" "$(stat_of rest_place)"

# ---- 14. restart recovery ----------------------------------------------------
echo "== 14. gateway restart -> replay + reconcile =="
P0="$(stat_of rest_place)"
kill "$GATEWAY_PID" 2>/dev/null
wait "$GATEWAY_PID" 2>/dev/null
"$GATEWAY_BIN" "$WORK/gateway.json" >>"$WORK/gateway.log" 2>&1 &
GATEWAY_PID=$!
for _ in $(seq 1 100); do
    gw GET /health && [ "$STATUS" = "200" ] && break
    sleep 0.1
done
gw GET /orders/gwT05
assert_eq "GET /orders/gwT05 -> 200 after restart" "200" "$STATUS"
assert_eq "order state survives restart" "live" "$(json_field "$BODY" state)"
assert_eq "amended price survived restart" "49000" "$(json_field "$BODY" price)"
gw POST /orders "$(place_body gwT05)"
assert_eq "place replay after restart -> 201" "201" "$STATUS"
assert_eq "venue saw no extra place" "$P0" "$(stat_of rest_place)"

# ---- summary ----------------------------------------------------------------
echo
echo "== black-box client suite: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
