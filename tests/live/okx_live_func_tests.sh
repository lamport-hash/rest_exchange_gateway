#!/usr/bin/env bash
# LIVE functional test suite — real OKX demo trading, NO MOCKS.
#
# Exercises every gateway feature that involves a real connection to the
# exchange (phases 1-3): signed REST place/amend/cancel, OMS idempotency
# (clientOrderId replay), common-schema validation (exchange-field
# rejection), pre-trade risk limits, structured error mapping (400/404/
# 409/502), the private WebSocket orders feed (login, subscribe,
# execution reports on place/amend/cancel/fill), real fills, persistence
# + restart recovery, venue reconciliation with adoption of orders placed
# directly on the venue, and the transport-failure path (venue
# unreachable -> retry budget exhausted -> 502) against a really closed
# port.
#
# Not covered here (impossible against the live venue; covered by the
# deterministic rigs in tests/ and tests/blackbox/):
# - WS server-side disconnect / reconnect (we cannot kill wspap.okx.com)
# - fault injection (dropped acks / delayed responses)
#
# Prerequisites: config/gateway.json (gitignored) with valid OKX demo
# credentials and demo funds; internet access. The suite cancels or
# leaves canceled every order it creates and spends a tiny amount of
# demo funds (one aggressive limit buy of ~0.00005 BTC).
#
# Usage (host or inside the dev container — the script re-execs itself in
# the container when started on the host):
#   tests/live/okx_live_func_tests.sh [path-to-config]
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONFIG_ARG="${1:-$ROOT/config/gateway.json}"

# ---- run inside the dev container when started on the host ---------------
if [ ! -f /.dockerenv ]; then
    cd "$ROOT"
    docker compose up -d dev >/dev/null 2>&1 || docker-compose up -d dev >/dev/null 2>&1
    exec docker compose exec -T dev bash tests/live/okx_live_func_tests.sh "$CONFIG_ARG"
fi

cd "$ROOT"
GATEWAY_BIN="$ROOT/build/release/gateway"
GW_PORT=18090
DEAD_GW_PORT=18091
WORK="$(mktemp -d /tmp/okx_live.XXXXXX)"
GATEWAY_PID=""
DEAD_GATEWAY_PID=""
PASS=0
FAIL=0

cleanup() {
    [ -n "$GATEWAY_PID" ] && kill "$GATEWAY_PID" 2>/dev/null
    [ -n "$DEAD_GATEWAY_PID" ] && kill "$DEAD_GATEWAY_PID" 2>/dev/null
    wait 2>/dev/null
    if [ "$FAIL" -gt 0 ]; then
        echo "artifacts kept in $WORK (gateway.log, configs, order log)"
    else
        rm -rf "$WORK"
    fi
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

start_gateway() { # config log
    "$GATEWAY_BIN" "$1" >"$2" 2>&1 &
    GATEWAY_PID=$!
    for _ in $(seq 1 150); do
        gw GET /health && [ "$STATUS" = "200" ] && return 0
        sleep 0.2
    done
    return 1
}
stop_gateway() {
    [ -n "$GATEWAY_PID" ] && kill "$GATEWAY_PID" 2>/dev/null
    wait "$GATEWAY_PID" 2>/dev/null
    GATEWAY_PID=""
    sleep 0.5
}

gw() { # METHOD path [json-body] -> sets STATUS and BODY (live instance)
    if [ $# -gt 2 ]; then
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -m 25 -X "$1" \
            -H 'Content-Type: application/json' -d "$3" \
            "http://127.0.0.1:${GW_PORT}$2")"
    else
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -m 25 -X "$1" \
            "http://127.0.0.1:${GW_PORT}$2")"
    fi
    BODY="$(cat "$WORK/body" 2>/dev/null)"
}
gw_dead() { # METHOD path [json-body] -> sets STATUS and BODY (dead venue)
    if [ $# -gt 2 ]; then
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -m 30 -X "$1" \
            -H 'Content-Type: application/json' -d "$3" \
            "http://127.0.0.1:${DEAD_GW_PORT}$2")"
    else
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -m 30 -X "$1" \
            "http://127.0.0.1:${DEAD_GW_PORT}$2")"
    fi
    BODY="$(cat "$WORK/body" 2>/dev/null)"
}
json_field() { # body field -> value of "field":"value"
    printf '%s' "$1" | grep -o "\"$2\":\"[^\"]*\"" | head -1 | cut -d'"' -f4
}
json_flag() { # body field -> "true"/"false" for boolean fields
    printf '%s' "$1" | grep -o "\"$2\":\(true\|false\)" | head -1 | cut -d: -f2
}
file_contains() { # description file pattern
    if grep -q "$3" "$2" 2>/dev/null; then
        ok "$1"
    else
        bad "$1 (pattern [$3] not in $2)"
    fi
}
wait_log() { # pattern timeout_seconds
    local deadline=$((SECONDS + $2))
    while [ $SECONDS -lt $deadline ]; do
        if grep -q "$1" "$WORK/gateway.log" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}
wait_state() { # clientOrderId state timeout_seconds -> sets STATE
    local deadline=$((SECONDS + $3))
    STATE=""
    while [ $SECONDS -lt $deadline ]; do
        gw GET "/orders/$1"
        STATE="$(json_field "$BODY" state)"
        [ "$STATE" = "$2" ] && return 0
        sleep 0.5
    done
    return 1
}
new_id() { # tag -> unique alphanumeric clientOrderId (OKX rule)
    printf 'L%s%s' "$(date +%s%N | tail -c 11)" "$1"
}
place_body() { # clientOrderId price quantity [extra-json-fields]
    printf '{"clientOrderId":"%s","symbol":"BTC-USDT","side":"buy","type":"limit","price":"%s","quantity":"%s"%s}' \
        "$1" "$2" "$3" "${4:-}"
}

# ---- preflight ------------------------------------------------------------
if [ ! -f "$CONFIG_ARG" ]; then
    echo "error: live config $CONFIG_ARG not found" >&2
    echo "       copy config/gateway.example.json -> config/gateway.json and fill in OKX demo credentials" >&2
    exit 1
fi
if grep -q 'YOUR_OKX' "$CONFIG_ARG"; then
    echo "error: $CONFIG_ARG still contains placeholder credentials" >&2
    exit 1
fi

if [ ! -x "$GATEWAY_BIN" ]; then
    echo "== building release gateway =="
    cmake --preset release >/dev/null && cmake --build --preset release --target gateway >/dev/null || {
        echo "build failed" >&2
        exit 1
    }
fi

# Derived config: same credentials/venue as the user's config, private
# REST port, a fresh persistence log, and risk limits that allow the
# suite's small orders but reject one designated oversized order.
python3 - "$CONFIG_ARG" "$WORK/gateway.json" "$GW_PORT" "$WORK/orders.jsonl" <<'EOF'
import json, sys
src, dst, port, log = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
cfg = json.load(open(src))
cfg["rest"]["port"] = int(port)
cfg["persistence"] = {"logPath": log}
cfg["risk"] = {"default": {"maxQty": "1", "maxNotional": "100000", "maxPosition": "1"}}
json.dump(cfg, open(dst, "w"), indent=2)
EOF

# ---- 1-17: main gateway instance ------------------------------------------
echo "== starting gateway against LIVE OKX demo (rest :$GW_PORT, private WS) =="
start_gateway "$WORK/gateway.json" "$WORK/gateway.log" || {
    echo "gateway did not become healthy: $(tail -5 "$WORK/gateway.log")" >&2
    exit 1
}

# ---- 1. health ------------------------------------------------------------
echo "== 1. health =="
gw GET /health
assert_eq "GET /health -> 200" "200" "$STATUS"
assert_contains "health body ok" "$BODY" '"status":"ok"'
assert_contains "health exposes OMS stats" "$BODY" '"knownOrders"'

# ---- 2. place + get round-trip --------------------------------------------
echo "== 2. signed REST place, far from market =="
# The orders channel does not replay events missed before the subscription:
# wait until the feed is connected (the 2nd reconcile line comes from the
# connectivity handler firing on WS connect) before the first place.
WAITED=0
until [ "$(grep -c '"event":"reconcile"' "$WORK/gateway.log" 2>/dev/null)" -ge 2 ] ||
    [ "$WAITED" -ge 30 ]; do
    sleep 0.5
    WAITED=$((WAITED + 1))
done
if [ "$(grep -c '"event":"reconcile"' "$WORK/gateway.log" 2>/dev/null)" -ge 2 ]; then
    ok "WS feed connected before the first place (${WAITED}s)"
else
    bad "WS feed did not connect within 30s (subscription may miss reports)"
fi
ID_A="$(new_id A)"
gw POST /orders "$(place_body "$ID_A" 10000 0.0001)"
assert_eq "POST /orders -> 201" "201" "$STATUS"
ORD_A="$(json_field "$BODY" exchangeOrderId)"
[ -n "$ORD_A" ] && ok "venue assigned exchangeOrderId ($ORD_A)" || bad "no exchangeOrderId"
assert_eq "state is live" "live" "$(json_field "$BODY" state)"
gw GET "/orders/$ID_A"
assert_eq "GET /orders -> 200" "200" "$STATUS"
assert_eq "state is live" "live" "$(json_field "$BODY" state)"
assert_eq "symbol echoed" "BTC-USDT" "$(json_field "$BODY" symbol)"

# ---- 3. WS orders feed: report on placement -------------------------------
echo "== 3. private WS: execution report for placement =="
if wait_log "\"clientOrderId\":\"$ID_A\".*\"state\":\"live\"" 25; then
    ok "WS report (live) reached the gateway log"
else
    bad "no WS execution report for $ID_A (feed login/subscribe broken?)"
fi

# ---- 4. idempotent client retry -------------------------------------------
echo "== 4. same clientOrderId twice -> same outcome =="
gw POST /orders "$(place_body "$ID_A" 10000 0.0001)"
assert_eq "re-POST same clientOrderId -> 201" "201" "$STATUS"
assert_eq "replayed=true (registry replay)" "true" "$(json_flag "$BODY" replayed)"
assert_eq "same exchangeOrderId as first place" "$ORD_A" "$(json_field "$BODY" exchangeOrderId)"

# ---- 5. amend (live venue amend) ------------------------------------------
echo "== 5. PUT amend price =="
gw PUT "/orders/$ID_A" '{"price":"10001"}'
assert_eq "PUT amend -> 200" "200" "$STATUS"
gw GET "/orders/$ID_A"
assert_eq "amended price visible" "10001" "$(json_field "$BODY" price)"
gw PUT "/orders/$ID_A" '{"price":"10002","quantity":"0.0002"}'
assert_eq "PUT amend px+qty -> 200" "200" "$STATUS"
gw GET "/orders/$ID_A"
assert_eq "amended quantity visible" "0.0002" "$(json_field "$BODY" quantity)"
assert_eq "amended price visible" "10002" "$(json_field "$BODY" price)"

# ---- 6. cancel ------------------------------------------------------------
echo "== 6. cancel =="
gw DELETE "/orders/$ID_A"
assert_eq "DELETE /orders -> 200" "200" "$STATUS"
assert_eq "same exchangeOrderId in cancel ack" "$ORD_A" "$(json_field "$BODY" exchangeOrderId)"
gw GET "/orders/$ID_A"
assert_eq "state is canceled" "canceled" "$(json_field "$BODY" state)"

# ---- 7. WS orders feed: report on cancel ----------------------------------
echo "== 7. private WS: execution report for cancel =="
if wait_log "\"clientOrderId\":\"$ID_A\".*\"state\":\"canceled\"" 25; then
    ok "WS report (canceled) reached the gateway log"
else
    bad "no WS execution report for canceled $ID_A"
fi

# ---- 8. idempotent cancel -------------------------------------------------
echo "== 8. cancel twice -> idempotent =="
gw DELETE "/orders/$ID_A"
assert_eq "second DELETE -> 200 (registry replay)" "200" "$STATUS"
assert_eq "same exchangeOrderId" "$ORD_A" "$(json_field "$BODY" exchangeOrderId)"

# ---- 9. unknown order -----------------------------------------------------
echo "== 9. unknown order -> 404 =="
ID_X="$(new_id X)"
gw GET "/orders/$ID_X"
assert_eq "GET unknown -> 404" "404" "$STATUS"
assert_contains "structured not_found error" "$BODY" '"code":"not_found"'

# ---- 10. cancel unknown order --------------------------------------------
echo "== 10. cancel unknown order -> 404 =="
gw DELETE "/orders/$ID_X"
assert_eq "DELETE unknown -> 404" "404" "$STATUS"
assert_contains "structured not_found error" "$BODY" '"code":"not_found"'

# ---- 11. gateway-side validation ------------------------------------------
echo "== 11. request validation (no venue round-trip) =="
gw POST /orders '{"clientOrderId":"live-bad!id","symbol":"BTC-USDT","side":"buy","type":"limit","price":"10000","quantity":"0.0001"}'
assert_eq "non-alphanumeric clientOrderId -> 400" "400" "$STATUS"
assert_contains "invalid_request error" "$BODY" '"code":"invalid_request"'
gw POST /orders '{"clientOrderId":"'"$(new_id S)"'","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"10000","quantity":"0.0001"}'
assert_eq "exchange-specific field (instrumentId) -> 400" "400" "$STATUS"
assert_contains "field rejection reason" "$BODY" 'exchange-specific fields are rejected'
gw POST /orders '{"clientOrderId":"'"$(new_id V)"'","symbol":"BTC-USDT","venue":"binance","side":"buy","type":"limit","price":"10000","quantity":"0.0001"}'
assert_eq "unsupported venue -> 400" "400" "$STATUS"
gw POST /orders '{"clientOrderId":"'"$(new_id T)"'","symbol":"BTC-USDT","side":"buy","type":"market","quantity":"0.00001","timeInForce":"GTC"}'
assert_eq "timeInForce on market order -> 400" "400" "$STATUS"

# ---- 12. pre-trade risk limits --------------------------------------------
echo "== 12. pre-trade risk rejection (gateway-side) =="
gw POST /orders "$(place_body "$(new_id R)" 10000 2)"
assert_eq "quantity above maxQty -> 400" "400" "$STATUS"
assert_contains "risk_max_qty code" "$BODY" '"code":"risk_max_qty"'

# ---- 13. venue rejections (live OKX error codes) --------------------------
echo "== 13. venue rejections -> 409 venue_rejected =="
LAST="$(curl -s -m 10 'https://www.okx.com/api/v5/market/ticker?instId=BTC-USDT' |
    grep -o '"last":"[0-9.]*"' | head -1 | cut -d'"' -f4)"
gw POST /orders "$(place_body "$(new_id M)" 10000 0.00001)"
assert_eq "below minimum order amount -> 409" "409" "$STATUS"
assert_contains "venue_rejected error (51020)" "$BODY" '51020'
if [ -z "$LAST" ]; then
    bad "cannot fetch BTC-USDT ticker for the price-band tests"
    FUNDS_PX="70000"
else
    # inside the venue buy price band (51137), notional under the risk cap,
    # notional far above the demo wallet -> insufficient funds
    FUNDS_PX="$(python3 -c "print(int(float('$LAST') * 0.99))")"
fi
gw POST /orders "$(place_body "$(new_id F)" "$FUNDS_PX" 1)"
assert_eq "insufficient funds -> 409" "409" "$STATUS"
assert_contains "venue_rejected error" "$BODY" '"code":"venue_rejected"'

# ---- 14. real fill through the feed ---------------------------------------
echo "== 14. aggressive limit buy fills on the venue =="
if [ -z "$LAST" ]; then
    bad "cannot fetch BTC-USDT ticker for the aggressive-price test"
else
    AGGRESSIVE="$(python3 -c "print(int(float('$LAST') * 1.003) + 1)")"
    echo "   last=$LAST -> aggressive px=$AGGRESSIVE (buy 0.00005, spends a few USDT of demo funds)"
    ID_C="$(new_id C)"
    gw POST /orders "$(place_body "$ID_C" "$AGGRESSIVE" 0.00005)"
    assert_eq "marketable limit -> 201" "201" "$STATUS"
    if wait_state "$ID_C" filled 25; then
        ok "order reached state filled at the venue"
        gw GET "/orders/$ID_C"
        FILLED_QTY="$(json_field "$BODY" filledQuantity)"
        [ -n "$FILLED_QTY" ] && [ "$FILLED_QTY" != "0" ] &&
            ok "filledQuantity=$FILLED_QTY (non-zero)" || bad "filledQuantity is empty/zero"
        [ -n "$(json_field "$BODY" averageFillPrice)" ] &&
            ok "averageFillPrice=$(json_field "$BODY" averageFillPrice)" || bad "no averageFillPrice"
    else
        bad "order did not fill within 25s (state=$STATE)"
    fi
    if wait_log "\"clientOrderId\":\"$ID_C\".*\"state\":\"filled\"" 25; then
        ok "WS report (filled) reached the gateway log"
    else
        bad "no WS execution report for filled $ID_C"
    fi
    gw DELETE "/orders/$ID_C"
    assert_eq "cancel of filled order -> 409 order_terminal" "409" "$STATUS"
    assert_contains "order_terminal code" "$BODY" '"code":"order_terminal"'
fi

# ---- 15. persistence + restart recovery ------------------------------------
echo "== 15. restart recovery from the event log =="
ID_R="$(new_id R)"
gw POST /orders "$(place_body "$ID_R" 10000 0.0001)"
assert_eq "place before restart -> 201" "201" "$STATUS"
ORD_R="$(json_field "$BODY" exchangeOrderId)"
stop_gateway
start_gateway "$WORK/gateway.json" "$WORK/gateway.log" || {
    echo "gateway did not restart cleanly" >&2
    exit 1
}
file_contains "startup replay logged" "$WORK/gateway.log" 'recovered'
file_contains "startup reconciliation logged" "$WORK/gateway.log" '"event":"reconcile"'
gw GET "/orders/$ID_R"
assert_eq "order survived restart (from log)" "200" "$STATUS"
assert_eq "same exchangeOrderId after restart" "$ORD_R" "$(json_field "$BODY" exchangeOrderId)"
assert_eq "state live after restart" "live" "$(json_field "$BODY" state)"
gw POST /orders "$(place_body "$ID_R" 10000 0.0001)"
assert_eq "place retry after restart -> 201 replayed" "201" "$STATUS"
assert_eq "replayed=true across restart" "true" "$(json_flag "$BODY" replayed)"

# ---- 16. adoption of a venue-placed order ----------------------------------
echo "== 16. reconcile adopts an order placed directly on the venue =="
ID_V="$(new_id V)"
python3 - "$CONFIG_ARG" "$ID_V" <<'EOF' || bad "direct venue place failed (python helper)"
import base64, datetime, hashlib, hmac, json, sys, urllib.request
cfg = json.load(open(sys.argv[1]))["okx"]
cl = sys.argv[2]
ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"
body = json.dumps({"clOrdId": cl, "instId": "BTC-USDT", "tdMode": "cash",
                   "side": "buy", "ordType": "limit", "px": "10000", "sz": "0.0001"})
sign = base64.b64encode(hmac.new(cfg["secretKey"].encode(),
                                 (ts + "POST" + "/api/v5/trade/order" + body).encode(),
                                 hashlib.sha256).digest()).decode()
req = urllib.request.Request("https://www.okx.com/api/v5/trade/order", data=body.encode())
req.add_header("OK-ACCESS-KEY", cfg["apiKey"]); req.add_header("OK-ACCESS-SIGN", sign)
req.add_header("OK-ACCESS-TIMESTAMP", ts); req.add_header("OK-ACCESS-PASSPHRASE", cfg["passphrase"])
req.add_header("x-simulated-trading", "1"); req.add_header("User-Agent", "Mozilla/5.0")
req.add_header("Content-Type", "application/json")
reply = json.load(urllib.request.urlopen(req, timeout=10))
assert reply["code"] == "0" and reply["data"][0]["sCode"] == "0", reply
print("venue ordId:", reply["data"][0]["ordId"])
EOF
stop_gateway
start_gateway "$WORK/gateway.json" "$WORK/gateway.log" || {
    echo "gateway did not restart cleanly" >&2
    exit 1
}
ADOPT_DEADLINE=$((SECONDS + 15))
until grep -q "\\"clientOrderId\\":\\"$ID_V\\".*\\"type\\":\\"adopted\\"" "$WORK/orders.jsonl" 2>/dev/null ||
    [ $SECONDS -ge "$ADOPT_DEADLINE" ]; do
    sleep 0.5
done
if grep -q "\"clientOrderId\":\"$ID_V\".*\"type\":\"adopted\"" "$WORK/orders.jsonl" 2>/dev/null; then
    ok "reconcile adopted the venue-placed order (adopted event persisted)"
else
    bad "no adopted event for $ID_V in the persistence log"
fi
gw GET "/orders/$ID_V"
assert_eq "adopted order visible in the registry" "200" "$STATUS"
assert_eq "adopted order state live" "live" "$(json_field "$BODY" state)"
gw DELETE "/orders/$ID_V"
assert_eq "adopted order cancelable through the gateway" "200" "$STATUS"
gw DELETE "/orders/$ID_R"
assert_eq "cleanup: cancel pre-restart order" "200" "$STATUS"

# ---- 17. feed still alive at the end of the suite --------------------------
echo "== 17. WS feed sustained connectivity (end of suite) =="
ID_E="$(new_id E)"
gw POST /orders "$(place_body "$ID_E" 10000 0.0001)"
assert_eq "final place -> 201" "201" "$STATUS"
if wait_log "\"clientOrderId\":\"$ID_E\".*\"state\":\"live\"" 25; then
    ok "WS reports still flow at end of suite (keepalive held)"
else
    bad "WS feed stopped delivering reports before end of suite"
fi
gw DELETE "/orders/$ID_E"
assert_eq "final cancel -> 200" "200" "$STATUS"
if wait_log "\"clientOrderId\":\"$ID_E\".*\"state\":\"canceled\"" 25; then
    ok "final WS cancel report received"
else
    bad "no WS cancel report for $ID_E"
fi
stop_gateway

# ---- 18. venue unreachable: retry budget -> 502 ----------------------------
echo "== 18. venue unreachable -> 502 venue_unavailable (real closed port, no mock) =="
python3 - "$CONFIG_ARG" "$WORK/dead.json" "$DEAD_GW_PORT" <<'EOF'
import json, sys
src, dst, port = sys.argv[1], sys.argv[2], sys.argv[3]
cfg = json.load(open(src))
cfg["rest"]["port"] = int(port)
okx = cfg["okx"]
okx["host"] = "127.0.0.1"   # nothing listens there: real connection refused
okx["port"] = 9
okx["useTls"] = False
okx["restConnectTimeoutMs"] = 300
okx["restReadTimeoutMs"] = 300
okx["retry"] = {"maxAttempts": 3, "initialBackoffMs": 50, "maxBackoffMs": 200,
                "multiplier": 2.0, "jitter": 0.0, "budgetMs": 1500}
okx["ws"] = {"enabled": False}
json.dump(cfg, open(dst, "w"), indent=2)
EOF
"$GATEWAY_BIN" "$WORK/dead.json" >"$WORK/dead_gateway.log" 2>&1 &
DEAD_GATEWAY_PID=$!
for _ in $(seq 1 100); do
    gw_dead GET /health && [ "$STATUS" = "200" ] && break
    sleep 0.2
done
T0="$SECONDS"
gw_dead POST /orders "$(place_body "$(new_id D)" 10000 0.0001)"
assert_eq "POST while venue unreachable -> 502" "502" "$STATUS"
assert_contains "structured venue_unavailable error" "$BODY" '"code":"venue_unavailable"'
ELAPSED=$((SECONDS - T0))
[ "$ELAPSED" -le 15 ] && ok "retry budget honored (failed in ${ELAPSED}s)" ||
    bad "took ${ELAPSED}s to fail (budget not honored?)"
kill "$DEAD_GATEWAY_PID" 2>/dev/null
DEAD_GATEWAY_PID=""

# ---- summary ----------------------------------------------------------------
echo
echo "== live OKX demo suite: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
