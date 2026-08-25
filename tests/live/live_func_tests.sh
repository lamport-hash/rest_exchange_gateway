#!/usr/bin/env bash
# LIVE functional test suite — real venue trading, NO MOCKS.
#
# Usage: tests/live/live_func_tests.sh okx|binance [path-to-config]
#
# One venue-agnostic suite: the REST API under test is exchange-agnostic,
# so every section drives the same client schema and only the "venue"
# field (and a few venue facts: filters, bands, semantics) changes.
#
# Exercises every gateway feature that involves a real connection to the
# exchange (phases 1-3): signed order place/amend/cancel (limit + market;
# REST on OKX, WS-API on Binance), OMS idempotency (clientOrderId
# replay), common-schema validation (exchange-field rejection), pre-trade
# risk limits (maxQty, maxNotional, maxPosition), structured error
# mapping (400/404/409/502), the private execution feed (login,
# subscribe, execution reports on place/amend/cancel/fill), real fills,
# the amend-to-cross-mid race (three resting orders amended across the
# mid: full instant fills racing the amend acks -> state filled),
# persistence + restart recovery, venue reconciliation with adoption of
# orders placed directly on the venue, and the transport-failure path
# (venue unreachable -> retry budget exhausted -> 502) against a really
# closed port.
#
# Venue facts baked into the parameters below:
# - Cost model: BTC is hardcoded at BTC_USD (70000) for quantity
#   derivation; every suite order is sized so the WHOLE run consumes
#   well under 1000 USDT of demo funds (hard-checked at the end).
# - Resting price REST_PX (65000, ~0.8x a ~80k market) stays inside the
#   Binance PERCENT_PRICE_BY_SIDE band [0.5x avg, 2x avg]; a preflight
#   guard aborts if the live market drifted outside that assumption.
# - Crossing price is last * 1.01 (aggressive, instant fill) for both
#   the single-fill section and the amend-to-cross section.
# - OKX demo: amend keeps the exchangeOrderId.
# - Binance spot testnet: BTCUSDT, NOTIONAL filter (min 5 USDT) forces
#   BASE_QTY such that qty * REST_PX > 5; amend is cancelReplace
#   emulation -> NEW exchangeOrderId, same clientOrderId.
#
# Not covered here (impossible against the live venue; covered by the
# deterministic rigs in tests/ and tests/blackbox/):
# - WS server-side disconnect / reconnect (we cannot kill the venue)
# - fault injection (dropped acks / delayed responses)
# - Binance insufficient-balance rejection inside tight risk caps
#   (buy-side; the sell-side variant IS covered here — see section 13)
#
# Prerequisites: config/gateway.json (gitignored) with valid venue demo
# credentials and demo funds (Binance testnet: >= ~20 USDT and a little
# BTC); internet access. The suite cancels or leaves canceled every
# order it creates. Budget: every order is sized off a hardcoded 70000
# USDT/BTC (BASE_QTY = 7/70000 BTC); the whole run buys ~4 x BASE_QTY
# BTC (~28 USDT) and sells the wallet back in section 15, transient
# working-order locks stay under ~70 USDT, and a start/end balance
# check HARD-FAILS the run if net consumption reaches 1000 USDT.
#
# Usage (host or inside the dev container — the script re-execs itself in
# the container when started on the host):
#   tests/live/live_func_tests.sh okx
#   tests/live/live_func_tests.sh binance
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

VENUE="${1:-}"
case "$VENUE" in
    okx | binance) ;;
    *)
        echo "usage: $0 okx|binance [path-to-config]" >&2
        exit 1
        ;;
esac
CONFIG_ARG="${2:-$ROOT/config/gateway.json.secret}"

# ---- run inside the dev container when started on the host ---------------
if [ ! -f /.dockerenv ]; then
    cd "$ROOT"
    docker compose up -d dev >/dev/null 2>&1 || docker-compose up -d dev >/dev/null 2>&1
    # docker-compose.yml mounts the repo at /workspace/rest_exchange_gateway;
    # translate host paths under $ROOT so the preflight check finds the config
    CONTAINER_ROOT=/workspace/rest_exchange_gateway
    CONTAINER_CONFIG_ARG="$CONFIG_ARG"
    case "$CONFIG_ARG" in
        "$ROOT"/*) CONTAINER_CONFIG_ARG="$CONTAINER_ROOT/${CONFIG_ARG#"$ROOT"/}" ;;
    esac
    exec docker compose exec -T dev bash tests/live/live_func_tests.sh "$VENUE" "$CONTAINER_CONFIG_ARG"
fi

cd "$ROOT"
GATEWAY_BIN="$ROOT/build/release/gateway"
GW_PORT=18090
DEAD_GW_PORT=18091
WORK="$(mktemp -d /tmp/live_${VENUE}.XXXXXX)"
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
assert_ne() { # description not-expected actual
    if [ "$2" != "$3" ]; then
        ok "$1"
    else
        bad "$1 (got the value it must not be [$2])"
    fi
}
assert_contains() { # description haystack needle
    if printf '%s' "$2" | grep -q -- "$3"; then
        ok "$1"
    else
        bad "$1 (missing [$3] in [$(printf '%s' "$2" | head -c 200)])"
    fi
}

start_gateway() { # config log (appends: later restarts keep earlier evidence)
    "$GATEWAY_BIN" "$1" >>"$2" 2>&1 &
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
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -m 30 -X "$1" \
            -H 'Content-Type: application/json' -d "$3" \
            "http://127.0.0.1:${GW_PORT}$2")"
    else
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -m 30 -X "$1" \
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
eq_num() { # numeric-equality test for venue-echoed decimals (0.0001 vs 0.00010000)
    python3 -c "import sys; sys.exit(0 if float('$1') == float('$2') else 1)"
}
assert_num_eq() { # description expected actual (numeric compare)
    if eq_num "$2" "$3"; then
        ok "$1"
    else
        bad "$1 (expected [$2] got [$3])"
    fi
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
new_id() { # tag -> unique alphanumeric clientOrderId (OKX/Binance rule)
    printf 'L%s%s' "$(date +%s%N | tail -c 11)" "$1"
}
place_body() { # clientOrderId price quantity [extra-json-fields]
    printf '{"clientOrderId":"%s","venue":"%s","symbol":"BTC-USDT","side":"buy","type":"limit","price":"%s","quantity":"%s"%s}' \
        "$1" "$VENUE" "$2" "$3" "${4:-}"
}

# ---- venue parameters -------------------------------------------------------
# Shared cost model (both venues): BTC valued at BTC_USD for sizing, all
# orders priced off REST_PX (resting) or last*CROSS_FACTOR (crossing).
BTC_USD="70000"     # hardcoded theoretical BTC price for quantity math
REST_PX="65000"     # resting price (~0.8x an ~80k market; far from mid)
# BASE_QTY: ~7 USDT notional at the theoretical price (7 / 70000); at
# REST_PX it is 6.5 USDT — above Binance's 5-USDT NOTIONAL minimum and
# above OKX BTC-USDT minSz, while every fill costs pennies.
BASE_QTY="0.0001"
# Dynamic, per-venue (see the case table): the crossing price is
# last * CROSS_FACTOR (+1 tick) — aggressive, instant fill.
CROSS_FACTOR=""

case "$VENUE" in
    okx)
        VENUE_ID="OKX"
        TICKER_URL='https://www.okx.com/api/v5/market/ticker?instId=BTC-USDT'
        TICKER_FIELD='"last":"[0-9.]*"'
        # OKX rejects BUY prices above ~last*1.005 (error 51137 "highest
        # price limit for the buy leg"), so +1% can never rest there;
        # +0.4% still crosses the tight demo book instantly.
        CROSS_FACTOR="1.004"
        RISK_MAX_NOTIONAL="100000"
        # headroom above the funds candidate (qty 1) so reconciliation
        # adopting stray resting venue orders cannot flip that test to a
        # gateway-side rejection
        RISK_MAX_POSITION="1.1"
        AMEND_QTY="0.0002" # ~14 USDT at the theoretical price
        POS_QTY="0.6" # projected 0.6 + 0.6 = 1.2 > maxPosition 1.1
        POS_PX="100" # locks 0.6 * 100 = 60 USDT on the venue
        NOTIONAL_PX="150000" # 150000 * 1 > maxNotional (gateway-side)
        FUNDS_QTY="1" # buy ~78k USDT > demo wallet -> venue rejection
        FUNDS_FACTOR="0.99" # inside the venue buy band, above the wallet
        MINORD_PX="10000" # far outside the OKX buy band -> 51020
        MINORD_MATCH="51020"
        ;;
    binance)
        VENUE_ID="BINANCE"
        TICKER_URL='https://testnet.binance.vision/api/v3/ticker/price?symbol=BTCUSDT'
        TICKER_FIELD='"price":"[0-9.]*"'
        # Binance's PERCENT_PRICE_BY_SIDE allows buy prices up to 2x avg,
        # so the full +1% crossing price is fine here.
        CROSS_FACTOR="1.01"
        RISK_MAX_NOTIONAL="1000000"
        # Tight position cap keeps the position-test lock at ~6.5 USDT
        # (BASE_QTY * REST_PX) instead of thousands.
        RISK_MAX_POSITION="0.002"
        AMEND_QTY="0.0002" # ~13 USDT at REST_PX (above the 5-USDT floor)
        POS_QTY="0.002" # candidate: working BASE_QTY + 0.002 > 0.002 cap
        POS_PX="$REST_PX" # working order locks BASE_QTY * 65000 = 6.5 USDT
        NOTIONAL_PX="1500000" # 1500000 * 1 > maxNotional (gateway-side;
        # the notional check fires before the position check, so qty 1 is
        # fine even under the tight 0.002 position cap)
        # funds test is sell-side (see section 13): sells more BTC than
        # the wallet holds -> venue -2010, no quote ccy locked at all
        FUNDS_OVER="0.0005"
        # the numeric venue code is not mapped into the REST error body;
        # the filter name is (Binance: "Filter failure: NOTIONAL")
        MINORD_PX="$REST_PX" # 0.00001 * 65000 = 0.65 USDT < 5 -> NOTIONAL
        MINORD_MATCH="Filter failure: NOTIONAL"
        ;;
esac

# ---- preflight ------------------------------------------------------------
if [ ! -f "$CONFIG_ARG" ]; then
    echo "error: live config $CONFIG_ARG not found" >&2
    echo "       copy config/gateway.example.json -> config/gateway.json and fill in venue demo credentials" >&2
    exit 1
fi
if grep -q 'YOUR_OKX\|YOUR_BINANCE' "$CONFIG_ARG"; then
    echo "error: $CONFIG_ARG still contains placeholder credentials" >&2
    exit 1
fi

# Free USDT + BTC of the account (stdout: "usdt btc"). Used for the
# start/end budget guard and to size the sell-side tests dynamically so
# successive suite runs never leak locked funds.
venue_balance() { # out-file
    if [ "$VENUE" = "binance" ]; then
        python3 - "$CONFIG_ARG" >"$1" <<'EOF'
import hashlib, hmac, json, sys, time, urllib.request
cfg = json.load(open(sys.argv[1]))["binance"]
q = f"timestamp={int(time.time()*1000)}&recvWindow=10000"
sig = hmac.new(cfg["secretKey"].encode(), q.encode(), hashlib.sha256).hexdigest()
req = urllib.request.Request(
    f"https://testnet.binance.vision/api/v3/account?{q}&signature={sig}",
    headers={"X-MBX-APIKEY": cfg["apiKey"], "User-Agent": "curl/8"})
bal = {b["asset"]: float(b["free"]) for b in json.load(urllib.request.urlopen(req, timeout=10))["balances"]}
print(f"{bal.get('USDT', 0):.8f} {bal.get('BTC', 0):.8f}")
EOF
    else
        python3 - "$CONFIG_ARG" >"$1" <<'EOF'
import base64, datetime, hashlib, hmac, json, sys, urllib.request
cfg = json.load(open(sys.argv[1]))["okx"]
path = "/api/v5/account/balance?ccy=BTC,USDT"
ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"
sign = base64.b64encode(hmac.new(cfg["secretKey"].encode(),
                                 (ts + "GET" + path).encode(), hashlib.sha256).digest()).decode()
req = urllib.request.Request("https://www.okx.com" + path, headers={
    "OK-ACCESS-KEY": cfg["apiKey"], "OK-ACCESS-SIGN": sign,
    "OK-ACCESS-TIMESTAMP": ts, "OK-ACCESS-PASSPHRASE": cfg["passphrase"],
    "x-simulated-trading": "1", "User-Agent": "Mozilla/5.0"})
data = json.load(urllib.request.urlopen(req, timeout=10))["data"][0]["details"]
bal = {d["ccy"]: float(d["availBal"] or 0) for d in data}
print(f"{bal.get('USDT', 0):.8f} {bal.get('BTC', 0):.8f}")
EOF
    fi
}

BUDGET_MAX_USDT="1000"
venue_balance "$WORK/balance_start" || {
    echo "error: cannot read the $VENUE account balance for the budget guard" >&2
    exit 1
}
START_USDT="$(awk '{print $1}' "$WORK/balance_start")"
START_BTC="$(awk '{print $2}' "$WORK/balance_start")"
echo "== budget guard: start free $START_USDT USDT / $START_BTC BTC (cap ${BUDGET_MAX_USDT} USDT) =="

if [ "$VENUE" = "binance" ]; then
    # validate the testnet keys before wasting a gateway start (signed, read-only)
    python3 - "$CONFIG_ARG" <<'EOF' || exit 1
import hashlib, hmac, json, sys, time, urllib.request, urllib.error
cfg = json.load(open(sys.argv[1]))["binance"]
q = f"timestamp={int(time.time()*1000)}&recvWindow=10000"
sig = hmac.new(cfg["secretKey"].encode(), q.encode(), hashlib.sha256).hexdigest()
req = urllib.request.Request(
    f"https://testnet.binance.vision/api/v3/account?{q}&signature={sig}",
    headers={"X-MBX-APIKEY": cfg["apiKey"], "User-Agent": "curl/8"})
try:
    acct = json.load(urllib.request.urlopen(req, timeout=10))
except urllib.error.HTTPError as e:
    print(f"error: binance testnet keys rejected: {e.read().decode()[:120]}")
    sys.exit(1)
except Exception as e:
    print(f"warning: cannot validate binance keys up front ({e}); continuing")
    sys.exit(0)
bal = {b["asset"]: float(b["free"]) for b in acct["balances"]}
if bal.get("USDT", 0) < 20:
    print(f"warning: only {bal.get('USDT', 0)} USDT free on the testnet; "
          "the fill tests need ~20+ USDT")
if bal.get("BTC", 0) < 0.0002:
    print(f"warning: only {bal.get('BTC', 0)} BTC free on the testnet; "
          "the sell-side funds test needs a little BTC (it self-skips otherwise)")
EOF
fi

if [ ! -x "$GATEWAY_BIN" ]; then
    echo "== building release gateway =="
    cmake --preset release >/dev/null && cmake --build --preset release --target gateway >/dev/null || {
        echo "build failed" >&2
        exit 1
    }
fi

# Derived config: same credentials/venues as the user's config, private
# REST port, a fresh persistence log, and risk limits that allow the
# suite's small orders but reject one designated oversized order per
# check (maxNotional / maxPosition are venue-tuned; see the parameter
# table above).
python3 - "$CONFIG_ARG" "$WORK/gateway.json" "$GW_PORT" "$WORK/orders.jsonl" \
    "$RISK_MAX_NOTIONAL" "$RISK_MAX_POSITION" <<'EOF'
import json, sys
src, dst, port, log, max_notional, max_position = sys.argv[1:7]
cfg = json.load(open(src))
cfg["rest"]["port"] = int(port)
cfg["persistence"] = {"logPath": log}
cfg["risk"] = {"default": {"maxQty": "1", "maxNotional": max_notional,
                           "maxPosition": max_position}}
json.dump(cfg, open(dst, "w"), indent=2)
EOF

# ---- 1-18: main gateway instance ------------------------------------------
echo "== starting gateway against LIVE $VENUE_ID (rest :$GW_PORT, private WS) =="
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
echo "== 2. signed order place, far from market =="
# The execution feed does not replay events missed before the subscription:
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
LAST="$(curl -s -m 10 "$TICKER_URL" | grep -o "$TICKER_FIELD" | head -1 | cut -d'"' -f4)"
if [ "$VENUE" = "binance" ]; then
    # every derived price below depends on the band -> hard requirement
    if [ -z "$LAST" ]; then
        echo "error: cannot fetch BTCUSDT ticker from the testnet; aborting" >&2
        exit 1
    fi
    # PERCENT_PRICE_BY_SIDE accepts buy prices in [0.5x avg, 2x avg]:
    # REST_PX (65000) must sit inside that window around the live price.
    if ! python3 -c "exit(0 if float('$LAST') * 0.5 <= $REST_PX <= float('$LAST') * 2 else 1)"; then
        echo "error: BTCUSDT at $LAST puts REST_PX $REST_PX outside the" \
            "[0.5x, 2x] PERCENT_PRICE_BY_SIDE band; update REST_PX/BTC_USD" >&2
        exit 1
    fi
fi
ID_A="$(new_id A)"
gw POST /orders "$(place_body "$ID_A" "$REST_PX" "$BASE_QTY")"
assert_eq "POST /orders -> 201" "201" "$STATUS"
[ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
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
gw POST /orders "$(place_body "$ID_A" "$REST_PX" "$BASE_QTY")"
assert_eq "re-POST same clientOrderId -> 201" "201" "$STATUS"
assert_eq "replayed=true (registry replay)" "true" "$(json_flag "$BODY" replayed)"
assert_eq "same exchangeOrderId as first place" "$ORD_A" "$(json_field "$BODY" exchangeOrderId)"

# ---- 5. amend (live venue amend) ------------------------------------------
echo "== 5. PUT amend price =="
if [ "$VENUE" = "binance" ]; then
    # amend is cancelReplace emulation: the replacement keeps the
    # clientOrderId but receives a NEW exchangeOrderId
    AMEND1="$(python3 -c "print(f'{float('$REST_PX') + 1:.2f}')")"
    AMEND2="$(python3 -c "print(f'{float('$REST_PX') + 2:.2f}')")"
    gw PUT "/orders/$ID_A" "{\"price\":\"$AMEND1\"}"
    assert_eq "PUT amend -> 200" "200" "$STATUS"
    gw GET "/orders/$ID_A"
    assert_eq "amended price visible" "$AMEND1" "$(json_field "$BODY" price)"
    assert_ne "cancelReplace amend assigned a NEW exchangeOrderId" "$ORD_A" "$(json_field "$BODY" exchangeOrderId)"
    ORD_A="$(json_field "$BODY" exchangeOrderId)"
    gw PUT "/orders/$ID_A" "{\"price\":\"$AMEND2\",\"quantity\":\"$AMEND_QTY\"}"
    assert_eq "PUT amend px+qty -> 200" "200" "$STATUS"
    gw GET "/orders/$ID_A"
    assert_num_eq "amended quantity visible" "$AMEND_QTY" "$(json_field "$BODY" quantity)"
    assert_num_eq "amended price visible" "$AMEND2" "$(json_field "$BODY" price)"
    ORD_A="$(json_field "$BODY" exchangeOrderId)" # px+qty amend replaced the id again
else
    gw PUT "/orders/$ID_A" '{"price":"10001"}'
    assert_eq "PUT amend -> 200" "200" "$STATUS"
    gw GET "/orders/$ID_A"
    assert_eq "amended price visible" "10001" "$(json_field "$BODY" price)"
    assert_eq "amend kept the exchangeOrderId" "$ORD_A" "$(json_field "$BODY" exchangeOrderId)"
    gw PUT "/orders/$ID_A" '{"price":"10002","quantity":"0.0002"}'
    assert_eq "PUT amend px+qty -> 200" "200" "$STATUS"
    gw GET "/orders/$ID_A"
    assert_eq "amended quantity visible" "0.0002" "$(json_field "$BODY" quantity)"
    assert_eq "amended price visible" "10002" "$(json_field "$BODY" price)"
fi

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

# ---- 10. cancel / amend unknown order -------------------------------------
echo "== 10. cancel / amend unknown order -> 404 =="
gw DELETE "/orders/$ID_X"
assert_eq "DELETE unknown -> 404" "404" "$STATUS"
assert_contains "structured not_found error" "$BODY" '"code":"not_found"'
gw PUT "/orders/$ID_X" '{"price":"10001"}'
assert_eq "amend unknown -> 404" "404" "$STATUS"
assert_contains "structured not_found error (amend)" "$BODY" '"code":"not_found"'

# ---- 11. gateway-side validation ------------------------------------------
echo "== 11. request validation (no venue round-trip) =="
gw POST /orders '{"clientOrderId":"live-bad!id","venue":"'"$VENUE"'","symbol":"BTC-USDT","side":"buy","type":"limit","price":"10000","quantity":"0.0001"}'
assert_eq "non-alphanumeric clientOrderId -> 400" "400" "$STATUS"
assert_contains "invalid_request error" "$BODY" '"code":"invalid_request"'
gw POST /orders '{"clientOrderId":"'"$(new_id S)"'","instrumentId":"BTC-USDT","side":"buy","type":"limit","price":"10000","quantity":"0.0001"}'
assert_eq "exchange-specific field (instrumentId) -> 400" "400" "$STATUS"
assert_contains "field rejection reason" "$BODY" 'exchange-specific fields are rejected'
gw POST /orders '{"clientOrderId":"'"$(new_id V)"'","symbol":"BTC-USDT","venue":"kraken","side":"buy","type":"limit","price":"10000","quantity":"0.0001"}'
assert_eq "unsupported venue -> 400" "400" "$STATUS"
gw POST /orders '{"clientOrderId":"'"$(new_id T)"'","symbol":"BTC-USDT","side":"buy","type":"market","quantity":"0.00001","timeInForce":"GTC"}'
assert_eq "timeInForce on market order -> 400" "400" "$STATUS"

# ---- 12. pre-trade risk limits --------------------------------------------
echo "== 12. pre-trade risk rejection (gateway-side) =="
gw POST /orders "$(place_body "$(new_id R)" "$REST_PX" 2)"
assert_eq "quantity above maxQty -> 400" "400" "$STATUS"
assert_contains "risk_max_qty code" "$BODY" '"code":"risk_max_qty"'
gw POST /orders "$(place_body "$(new_id N)" "$NOTIONAL_PX" 1)"
assert_eq "notional above maxNotional -> 400" "400" "$STATUS"
assert_contains "risk_max_notional code" "$BODY" '"code":"risk_max_notional"'
# projected position: a working buy plus a candidate buy projects above
# maxPosition (gateway counts working + filled qty). The working order
# is the suite's standard cheap order (BASE_QTY at REST_PX — locks ~6.5
# USDT); only the candidate carries the oversized quantity, so rejected
# orders lock nothing and the working lock is canceled right after.
ID_P="$(new_id P)"
if [ "$VENUE" = "okx" ]; then
    # keeps the historic shape: both legs POS_QTY, the working one parked
    # at a near-zero price so it locks only ~60 USDT of quote ccy.
    gw POST /orders "$(place_body "$ID_P" "$POS_PX" "$POS_QTY")"
    assert_eq "position test order placed -> 201" "201" "$STATUS"
    [ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
    gw POST /orders "$(place_body "$(new_id Q)" "$POS_PX" "$POS_QTY")"
else
    # working = BASE_QTY at REST_PX (locks ~6.5 USDT); candidate POS_QTY
    # projects BASE_QTY + POS_QTY > the 0.002 cap.
    gw POST /orders "$(place_body "$ID_P" "$POS_PX" "$BASE_QTY")"
    assert_eq "position test order placed -> 201" "201" "$STATUS"
    [ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
    gw POST /orders "$(place_body "$(new_id Q)" "$POS_PX" "$POS_QTY")"
fi
assert_eq "projected position above maxPosition -> 400" "400" "$STATUS"
assert_contains "risk_max_position code" "$BODY" '"code":"risk_max_position"'
gw DELETE "/orders/$ID_P"
assert_eq "cleanup: cancel position test order" "200" "$STATUS"

# ---- 13. venue rejections (live venue error codes) ------------------------
echo "== 13. venue rejections -> 409 venue_rejected =="
gw POST /orders "$(place_body "$(new_id M)" "$MINORD_PX" 0.00001)"
assert_eq "venue filter rejection -> 409" "409" "$STATUS"
assert_contains "venue_rejected error ($MINORD_MATCH)" "$BODY" "$MINORD_MATCH"
if [ "$VENUE" = "okx" ]; then
    # buy-side: qty 1 at ~0.99x last is inside the band and risk caps
    # but far above the demo wallet -> venue 51008-style rejection.
    if [ -z "$LAST" ]; then
        bad "cannot fetch BTC-USDT ticker for the funds test"
        FUNDS_PX="70000"
    else
        FUNDS_PX="$(python3 -c "print(int(float('$LAST') * $FUNDS_FACTOR))")"
    fi
    gw POST /orders "$(place_body "$(new_id F)" "$FUNDS_PX" "$FUNDS_QTY")"
    assert_eq "insufficient funds -> 409" "409" "$STATUS"
    assert_contains "venue_rejected error" "$BODY" '"code":"venue_rejected"'
else
    # sell-side: market-sell more BTC than the wallet holds (free + the
    # small overshoot) -> venue -2010 insufficient balance. A sell locks
    # no quote ccy and the tight 0.002 position cap still passes while
    # free BTC stays below ~0.0015; otherwise skip (the next full run
    # sells the wallet back down and re-enables it).
    FREE_BTC="$(python3 -c "print(f'{float('$START_BTC'):.8f}')" 2>/dev/null || echo 0)"
    SELL_QTY="$(python3 -c "print(f'{float('$FREE_BTC') + $FUNDS_OVER:.5f}')")"
    if python3 -c "exit(0 if float('$FREE_BTC') <= 0.0015 else 1)"; then
        gw POST /orders '{"clientOrderId":"'"$(new_id F)"'","venue":"'"$VENUE"'","symbol":"BTC-USDT","side":"sell","type":"market","quantity":"'"$SELL_QTY"'"}'
        assert_eq "insufficient BTC (sell-side) -> 409" "409" "$STATUS"
        assert_contains "venue_rejected error" "$BODY" '"code":"venue_rejected"'
    else
        echo "  note: skipping sell-side funds test (free BTC $FREE_BTC > 0.0015;"
        echo "        covered again once the wallet is below that - OKX run covers"
        echo "        the funds path buy-side in the meantime)"
    fi
fi

# ---- 14. real fill through the feed ---------------------------------------
echo "== 14. aggressive limit buy fills on the venue =="
if [ -z "$LAST" ]; then
    bad "cannot fetch BTC-USDT ticker for the aggressive-price test"
else
    AGGRESSIVE="$(python3 -c "print(int(float('$LAST') * $CROSS_FACTOR) + 1)")"
    echo "   last=$LAST -> aggressive px=$AGGRESSIVE (buy $BASE_QTY, ~7 USDT of demo funds)"
    ID_C="$(new_id C)"
    gw POST /orders "$(place_body "$ID_C" "$AGGRESSIVE" "$BASE_QTY")"
    assert_eq "marketable limit -> 201" "201" "$STATUS"
    [ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
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
    gw PUT "/orders/$ID_C" '{"price":"10001"}'
    assert_eq "amend of filled order -> 409 order_terminal" "409" "$STATUS"
    assert_contains "order_terminal code (amend)" "$BODY" '"code":"order_terminal"'
fi

# ---- 14b. three far orders amended across the mid fill instantly ----------
echo "== 14b. amend-to-cross-mid: 3 resting orders fill instantly =="
# The regression rig for the amend/fill race: each amend crosses the mid
# so the venue fully executes the replacement while the amend response
# is still in flight (old leg CANCELED + replacement NEW + replacement
# FILLED racing the ack). Every order must settle in state filled with
# its full fill — never a zombie live-with-full-fill — and the
# consistency audit must stay silent about them.
if [ -z "$LAST" ]; then
    bad "skipping amend-to-cross test (no ticker, section 14 did not run)"
else
    CROSS="$(python3 -c "print(int(float('$LAST') * $CROSS_FACTOR) + 1)")"
    echo "   last=$LAST -> crossing px=$CROSS (3 buys of $BASE_QTY)"
    ID_X1="$(new_id XA)"
    ID_X2="$(new_id XB)"
    ID_X3="$(new_id XC)"
    ORD_X1="" ; ORD_X2="" ; ORD_X3=""
    for TAG in 1 2 3; do
        eval "ID=\$ID_X$TAG"
        gw POST /orders "$(place_body "$ID" "$REST_PX" "$BASE_QTY")"
        assert_eq "resting place $ID -> 201" "201" "$STATUS"
        [ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
        eval "ORD_X$TAG=\"\$(json_field \"\$BODY\" exchangeOrderId)\""
    done

    for TAG in 1 2 3; do
        eval "ID=\$ID_X$TAG"
        gw PUT "/orders/$ID" "{\"price\":\"$CROSS\"}"
        assert_eq "amend $ID across the mid -> 200" "200" "$STATUS"
        [ "$STATUS" != "200" ] && echo "   venue reason: $(json_field "$BODY" reason)"
        echo "   amend $ID ack state: $(json_field "$BODY" state)"
    done

    for TAG in 1 2 3; do
        eval "ID=\$ID_X$TAG"
        if wait_state "$ID" filled 25; then
            ok "$ID reached state filled after the crossing amend"
        else
            bad "$ID did not fill within 25s (state=$STATE)"
        fi
        gw GET "/orders/$ID"
        assert_num_eq "$ID filledQuantity == quantity" "$BASE_QTY" \
            "$(json_field "$BODY" filledQuantity)"
        [ -n "$(json_field "$BODY" averageFillPrice)" ] &&
            ok "$ID averageFillPrice=$(json_field "$BODY" averageFillPrice)" ||
            bad "$ID has no averageFillPrice"
        eval "ORD=\$ORD_X$TAG"
        if [ "$VENUE" = "binance" ]; then
            assert_ne "$ID cancelReplace minted a new venue id" "$ORD" \
                "$(json_field "$BODY" exchangeOrderId)"
        else
            assert_eq "$ID in-place amend kept the venue id" "$ORD" \
                "$(json_field "$BODY" exchangeOrderId)"
        fi
    done

    gw GET /consistency
    assert_eq "GET /consistency -> 200" "200" "$STATUS"
    if printf '%s' "$BODY" | grep -Eq "\"clientOrderId\":\"($ID_X1|$ID_X2|$ID_X3)\""; then
        bad "consistency alerts reference the crossing orders: $BODY"
    else
        ok "no consistency alerts reference the crossing orders"
    fi
fi

# ---- 15. market order happy path -------------------------------------------
echo "== 15. market order fills on the venue =="
# Sells the BTC bought in sections 14 + 14b back: the quantity is read
# from the venue wallet (floored to the 0.00001 lot step, minimum the
# venue's ~5 USDT notional floor, capped at SELL_MAX so an accumulated
# demo wallet is never dumped — and maxQty can never trip) so the
# account's BTC returns to ~dust after every run: repeated runs neither
# accumulate BTC nor starve Binance's sell-side funds test of section
# 13. Market sell quantity is base ccy on both venues.
SELL_MAX="0.001"
SELL_ALL_QTY=""
if venue_balance "$WORK/balance_mid"; then
    FREE_BTC_NOW="$(awk '{print $2}' "$WORK/balance_mid")"
    SELL_ALL_QTY="$(python3 -c "
free = float('$FREE_BTC_NOW')
qty = min(int(free * 1e5) / 1e5, float('$SELL_MAX'))  # lot step + cap
print(f'{qty:.5f}' if qty >= 0.0001 else '')")"
    [ -n "$SELL_ALL_QTY" ] &&
        echo "   wallet has $FREE_BTC_NOW BTC -> market selling $SELL_ALL_QTY (cap $SELL_MAX)" ||
        echo "   wallet BTC $FREE_BTC_NOW below the ~5 USDT notional floor"
fi
if [ -z "$SELL_ALL_QTY" ]; then
    # wallet query failed or dust: this run's buys (4 x BASE_QTY minus
    # fees) still cover the historical fallback quantity
    SELL_ALL_QTY="0.0003"
    echo "   falling back to fixed market sell of $SELL_ALL_QTY BTC"
fi
ID_MKT="$(new_id K)"
gw POST /orders '{"clientOrderId":"'"$ID_MKT"'","venue":"'"$VENUE"'","symbol":"BTC-USDT","side":"sell","type":"market","quantity":"'"$SELL_ALL_QTY"'"}'
assert_eq "market sell -> 201" "201" "$STATUS"
[ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
if wait_state "$ID_MKT" filled 25; then
    ok "market sell reached state filled at the venue"
    gw GET "/orders/$ID_MKT"
    assert_num_eq "market filledQuantity" "$SELL_ALL_QTY" "$(json_field "$BODY" filledQuantity)"
    [ -n "$(json_field "$BODY" averageFillPrice)" ] &&
        ok "market averageFillPrice=$(json_field "$BODY" averageFillPrice)" ||
        bad "no averageFillPrice on market fill"
else
    bad "market sell did not fill within 25s (state=$STATE)"
fi
if wait_log "\"clientOrderId\":\"$ID_MKT\".*\"state\":\"filled\"" 25; then
    ok "WS report (filled) reached the gateway log for the market order"
else
    bad "no WS execution report for market order $ID_MKT"
fi
gw DELETE "/orders/$ID_MKT"
if [ "$STATUS" = "200" ] || [ "$STATUS" = "409" ]; then
    ok "market order left terminal/canceled (DELETE -> $STATUS)"
else
    bad "unexpected market cleanup status $STATUS"
fi

# ---- 16. persistence + restart recovery ------------------------------------
echo "== 16. restart recovery from the event log =="
ID_R="$(new_id R)"
gw POST /orders "$(place_body "$ID_R" "$REST_PX" "$BASE_QTY")"
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
gw POST /orders "$(place_body "$ID_R" "$REST_PX" "$BASE_QTY")"
assert_eq "place retry after restart -> 201 replayed" "201" "$STATUS"
assert_eq "replayed=true across restart" "true" "$(json_flag "$BODY" replayed)"

# ---- 17. adoption of a venue-placed order ----------------------------------
echo "== 17. reconcile adopts an order placed directly on the venue =="
ID_V="$(new_id V)"
if [ "$VENUE" = "binance" ]; then
    VENUE_PX="$(python3 -c "print(f'{float('$REST_PX'):.2f}')")"
    python3 - "$CONFIG_ARG" "$ID_V" "$VENUE_PX" <<'EOF' || bad "direct venue place failed (python helper)"
import hashlib, hmac, json, sys, time, urllib.request, urllib.error
cfg = json.load(open(sys.argv[1]))["binance"]
cl, px = sys.argv[2], sys.argv[3]
params = [("symbol", "BTCUSDT"), ("side", "BUY"), ("type", "LIMIT"),
          ("timeInForce", "GTC"), ("quantity", "0.0002"), ("price", px),
          ("newClientOrderId", cl), ("recvWindow", "10000"),
          ("timestamp", str(int(time.time() * 1000)))]
query = "&".join(f"{k}={v}" for k, v in params)
sig = hmac.new(cfg["secretKey"].encode(), query.encode(), hashlib.sha256).hexdigest()
req = urllib.request.Request(
    f"https://testnet.binance.vision/api/v3/order?{query}&signature={sig}",
    method="POST", headers={"X-MBX-APIKEY": cfg["apiKey"], "User-Agent": "curl/8"})
try:
    reply = json.load(urllib.request.urlopen(req, timeout=10))
    assert reply["clientOrderId"] == cl, reply
    print("venue orderId:", reply["orderId"])
except urllib.error.HTTPError as e:
    print("venue place failed:", e.read().decode()[:150])
    sys.exit(1)
EOF
else
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
fi
stop_gateway
start_gateway "$WORK/gateway.json" "$WORK/gateway.log" || {
    echo "gateway did not restart cleanly" >&2
    exit 1
}
# Binance openOrders listings fail while the WS session is still coming
# up ("pendingListingFailed"); adoption lands on a later reconcile, so
# allow up to 30s for the adopted event.
ADOPT_DEADLINE=$((SECONDS + 30))
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

# ---- 18. feed still alive at the end of the suite --------------------------
echo "== 18. WS feed sustained connectivity (end of suite) =="
ID_E="$(new_id E)"
gw POST /orders "$(place_body "$ID_E" "$REST_PX" "$BASE_QTY")"
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

# ---- 18b. OKX demo-trading balance adjustment (demo accounts only) ------
if [ "$VENUE" = "okx" ] && python3 -c "import json,sys; sys.exit(0 if json.load(open('$WORK/gateway.json')).get('okx',{}).get('demoTrading') else 1)"; then
    echo "== 18b. OKX demo balance adjustment (demo-adjust-balance) =="
    gw POST /venue/okx/demo-adjust-balance \
        '{"type":"increase","adjustments":[{"ccy":"USDT","amt":"1"}]}'
    if [ "$STATUS" = "200" ]; then
        assert_eq "demo balance increase -> 200" "200" "$STATUS"
        assert_eq "venue echoed" "okx" "$(json_field "$BODY" venue)"
    elif [ "$STATUS" = "409" ] && printf '%s' "$BODY" | grep -q '59691'; then
        # OKX caps demo increases at 3/day (UTC) per account — and this
        # suite consumes one per run. The quota error must still surface
        # with its venue code (never an empty reason); the increase
        # itself is re-validated any day with quota left.
        assert_contains "quota exhaustion surfaces the venue code" "$BODY" '59691'
        echo "  note: daily increase quota exhausted (0/3 left; resets 00:00 UTC)"
    else
        bad "demo balance increase -> 200 (expected [200] got [$STATUS])"
        bad "venue echoed (got [$(json_field "$BODY" venue)])"
    fi
    gw POST /venue/okx/demo-adjust-balance \
        '{"type":"reduce","adjustments":[{"ccy":"USDT","amt":"1"}]}'
    assert_eq "demo balance reduce (restored) -> 200" "200" "$STATUS"
    gw POST /venue/okx/demo-adjust-balance \
        '{"type":"banana","adjustments":[{"ccy":"USDT","amt":"1"}]}'
    assert_eq "invalid type rejected gateway-side -> 400" "400" "$STATUS"
    gw POST /venue/okx/demo-adjust-balance \
        '{"type":"increase","adjustments":[{"ccy":"USDT","amt":"-5"}]}'
    assert_eq "negative amt rejected gateway-side -> 400" "400" "$STATUS"
else
    echo "== 18b. demo balance adjustment skipped (okx demo trading only) =="
fi
stop_gateway

# ---- 19. venue unreachable: retry budget -> 502 ----------------------------
echo "== 19. $VENUE unreachable -> 502 venue_unavailable (real closed port, no mock) =="
python3 - "$CONFIG_ARG" "$WORK/dead.json" "$DEAD_GW_PORT" "$VENUE" <<'EOF'
import json, sys
src, dst, port, venue = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
cfg = json.load(open(src))
cfg["rest"]["port"] = int(port)
dead = cfg[venue]
dead["host"] = "127.0.0.1"  # nothing listens there: real connection refused
dead["port"] = 9
dead["useTls"] = False
if venue == "okx":
    dead["restConnectTimeoutMs"] = 300
    dead["restReadTimeoutMs"] = 300
    dead["retry"] = {"maxAttempts": 3, "initialBackoffMs": 50, "maxBackoffMs": 200,
                     "multiplier": 2.0, "jitter": 0.0, "budgetMs": 1500}
    dead["ws"] = {"enabled": False}
else:
    dead["requestTimeoutMs"] = 300
    dead["retry"] = {"maxAttempts": 3, "initialBackoffMs": 50, "maxBackoffMs": 200,
                     "multiplier": 2.0, "jitter": 0.0, "budgetMs": 1500}
json.dump(cfg, open(dst, "w"), indent=2)
EOF
"$GATEWAY_BIN" "$WORK/dead.json" >"$WORK/dead_gateway.log" 2>&1 &
DEAD_GATEWAY_PID=$!
for _ in $(seq 1 100); do
    gw_dead GET /health && [ "$STATUS" = "200" ] && break
    sleep 0.2
done
T0="$SECONDS"
gw_dead POST /orders "$(place_body "$(new_id D)" "$REST_PX" 0.0001)"
assert_eq "POST while venue unreachable -> 502" "502" "$STATUS"
assert_contains "structured venue_unavailable error" "$BODY" '"code":"venue_unavailable"'
ELAPSED=$((SECONDS - T0))
[ "$ELAPSED" -le 15 ] && ok "retry budget honored (failed in ${ELAPSED}s)" ||
    bad "took ${ELAPSED}s to fail (budget not honored?)"
kill "$DEAD_GATEWAY_PID" 2>/dev/null
DEAD_GATEWAY_PID=""

# ---- budget guard: net demo-fund consumption of this run ------------------
echo "== budget check (cap ${BUDGET_MAX_USDT} USDT, BTC valued at $BTC_USD) =="
if venue_balance "$WORK/balance_end"; then
    END_USDT="$(awk '{print $1}' "$WORK/balance_end")"
    END_BTC="$(awk '{print $2}' "$WORK/balance_end")"
    BUDGET_TOTAL="$(python3 - "$START_USDT" "$START_BTC" "$END_USDT" "$END_BTC" "$BTC_USD" <<'EOF'
import sys
start_usdt, start_btc, end_usdt, end_btc, btc_usd = map(float, sys.argv[1:6])
usdt_spent = start_usdt - end_usdt
btc_net = end_btc - start_btc
print(f"{max(usdt_spent, 0.0) + btc_net * btc_usd:.2f}")
EOF
)"
    python3 - "$START_USDT" "$START_BTC" "$END_USDT" "$END_BTC" "$BTC_USD" <<'EOF'
import sys
start_usdt, start_btc, end_usdt, end_btc, btc_usd = map(float, sys.argv[1:6])
print(f"   USDT: {start_usdt:.2f} -> {end_usdt:.2f} (spent {start_usdt - end_usdt:.2f})")
print(f"   BTC : {start_btc:.8f} -> {end_btc:.8f} (net {end_btc - start_btc:+.8f}"
      f" = {abs(end_btc - start_btc) * btc_usd:.2f} USDT)")
EOF
    if python3 -c "exit(0 if float('$BUDGET_TOTAL') < $BUDGET_MAX_USDT else 1)"; then
        ok "net consumption $BUDGET_TOTAL USDT < ${BUDGET_MAX_USDT} budget"
    else
        bad "net consumption $BUDGET_TOTAL USDT exceeds the ${BUDGET_MAX_USDT} budget"
    fi
else
    bad "cannot re-read the $VENUE balance for the budget check"
fi

# ---- summary ----------------------------------------------------------------
echo
echo "== live $VENUE suite: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
