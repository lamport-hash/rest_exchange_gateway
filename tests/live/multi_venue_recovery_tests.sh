#!/usr/bin/env bash
# LIVE dual-venue failure/recovery suite — real venues, NO MOCKS.
#
# Usage: tests/live/multi_venue_recovery_tests.sh [path-to-config]
#
# Complements tests/live/live_func_tests.sh (single venue, one graceful
# restart, one dead venue) with two harder multi-venue scenarios against
# ONE gateway instance configured for BOTH venues:
#
# A. hard crash + restart with two live orders
#    - place one resting order on OKX and one on Binance (both live)
#    - SIGKILL the gateway mid-flight (no graceful shutdown, no venue
#      cleanup) and prove the REST API is really gone (conn refused)
#    - restart on the same config + event log: both orders recovered,
#      reconciled against BOTH venues, still live with the SAME
#      exchangeOrderIds; clientOrderId retries replay idempotently
#    - amend + cancel both orders through the recovered gateway (OKX
#      amend keeps the venue id, Binance cancelReplace mints a new one)
#
# B. venue isolation outage + recovery
#    - second instance: OKX pointed at a really closed port (conn
#      refused, no mock), Binance section untouched (live)
#    - OKX place -> transport retry budget exhausted -> 502; the intent
#      stays pending (GET 200, no exchangeOrderId); same-id retry -> 202
#      pending (the gateway never re-sends an unacked place)
#    - while OKX is dead, a Binance place on the same instance -> 201
#      live (per-venue isolation: one venue's outage must not affect
#      the other)
#    - kill, restore the LIVE OKX section (same port + SAME event log),
#      restart: startup reconcile resolves the pending order against
#      real OKX (51603 order-not-exist) -> rejected venue_absent; the
#      Binance order is untouched (live, same venue id) and cancelable
#
# Cost model: every order rests at REST_PX far from the mid (no fills
# by design), locking ~6.5 USDT per venue transiently; everything is
# canceled or terminal by the end, so net demo-fund consumption is ~0
# (still hard-checked, both venues, 1000 USDT cap).
#
# Prerequisites: config/gateway.json (gitignored) with BOTH valid venue
# demo sections (okx + binance) and internet access.
#
# Usage (host or inside the dev container — the script re-execs itself
# in the container when started on the host):
#   tests/live/multi_venue_recovery_tests.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

CONFIG_ARG="${1:-$ROOT/config/gateway.json.secret}"

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
    exec docker compose exec -T dev bash tests/live/multi_venue_recovery_tests.sh "$CONTAINER_CONFIG_ARG"
fi

cd "$ROOT"
GATEWAY_BIN="$ROOT/build/release/gateway"
GW_PORT=18090   # scenario A instance (both venues live)
ISO_PORT=18091  # scenario B instance (okx dead, binance live)
WORK="$(mktemp -d /tmp/live_dual.XXXXXX)"
GATEWAY_PID=""
ISO_PID=""
PASS=0
FAIL=0

cleanup() {
    [ -n "$GATEWAY_PID" ] && kill "$GATEWAY_PID" 2>/dev/null
    [ -n "$ISO_PID" ] && kill "$ISO_PID" 2>/dev/null
    wait 2>/dev/null
    if [ "$FAIL" -gt 0 ]; then
        echo "artifacts kept in $WORK (gateway.log, iso_gateway.log, configs, order logs)"
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

_gw() { # port METHOD path [json-body] -> sets STATUS and BODY
    if [ $# -gt 3 ]; then
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -m 30 -X "$2" \
            -H 'Content-Type: application/json' -d "$4" \
            "http://127.0.0.1:$1$3")"
    else
        STATUS="$(curl -s -o "$WORK/body" -w '%{http_code}' -m 30 -X "$2" \
            "http://127.0.0.1:$1$3")"
    fi
    BODY="$(cat "$WORK/body" 2>/dev/null)"
}
gw() { _gw "$GW_PORT" "$@"; }
gw_iso() { _gw "$ISO_PORT" "$@"; }
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
wait_reconcile() { # log-file min-count timeout-seconds
    # Every venue WS connect fires one reconcile line (plus one at
    # startup): counting them is how the suite knows a feed is REALLY
    # connected — Binance ws-api needs several seconds after startup and
    # orders placed before the session is up fail with a transport 502.
    local deadline=$((SECONDS + $3))
    while [ $SECONDS -lt $deadline ]; do
        [ "$(grep -c '"event":"reconcile"' "$1" 2>/dev/null)" -ge "$2" ] && return 0
        sleep 0.5
    done
    return 1
}
wait_state() { # which-gw clientOrderId state timeout_seconds -> sets STATE
    local deadline=$((SECONDS + $4))
    STATE=""
    while [ $SECONDS -lt $deadline ]; do
        if [ "$1" = "iso" ]; then
            gw_iso GET "/orders/$2"
        else
            gw GET "/orders/$2"
        fi
        STATE="$(json_field "$BODY" state)"
        [ "$STATE" = "$3" ] && return 0
        sleep 0.5
    done
    return 1
}
new_id() { # tag -> unique alphanumeric clientOrderId (OKX/Binance rule)
    printf 'D%s%s' "$(date +%s%N | tail -c 11)" "$1"
}
place_body() { # clientOrderId venue price quantity
    printf '{"clientOrderId":"%s","venue":"%s","symbol":"BTC-USDT","side":"buy","type":"limit","price":"%s","quantity":"%s"}' \
        "$1" "$2" "$3" "$4"
}

# ---- shared parameters (same cost model as live_func_tests.sh) -----------
BTC_USD="70000"  # hardcoded theoretical BTC price for the budget math
REST_PX="65000"  # resting price (~0.8x an ~80k market; far from the mid)
BASE_QTY="0.0001" # ~6.5 USDT locked per resting order; above venue minimums

start_gateway() { # config log pid-var -> starts and waits for /health
    local __pidvar="$3"
    "$GATEWAY_BIN" "$1" >>"$2" 2>&1 &
    eval "$__pidvar=\$!"
    local pid
    eval "pid=\$$__pidvar"
    for _ in $(seq 1 150); do
        curl -s -o /dev/null -m 5 "http://127.0.0.1:$4/health" && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
stop_gateway() { # pid-var (graceful)
    local __pidvar="$1"
    local pid
    eval "pid=\$$__pidvar"
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    eval "$__pidvar=''"
    sleep 0.5
}

# ---- preflight -------------------------------------------------------------
if [ ! -f "$CONFIG_ARG" ]; then
    echo "error: live config $CONFIG_ARG not found" >&2
    echo "       copy config/gateway.example.json -> config/gateway.json and fill in" >&2
    echo "       BOTH venue demo sections (okx + binance): this suite needs both" >&2
    exit 1
fi
if grep -q 'YOUR_OKX\|YOUR_BINANCE' "$CONFIG_ARG"; then
    echo "error: $CONFIG_ARG still contains placeholder credentials" >&2
    exit 1
fi
if ! python3 - "$CONFIG_ARG" <<'EOF'
import json, sys
cfg = json.load(open(sys.argv[1]))
for venue in ("okx", "binance"):
    section = cfg.get(venue)
    if not isinstance(section, dict) or not section.get("apiKey"):
        print(f"error: config has no complete '{venue}' section; "
              "this suite needs BOTH venues configured")
        sys.exit(1)
EOF
then
    exit 1
fi

# Free USDT + BTC per venue (stdout: "usdt btc"). Used for the start/end
# budget guard across both venues.
venue_balance() { # venue out-file
    if [ "$1" = "binance" ]; then
        python3 - "$CONFIG_ARG" >"$2" <<'EOF'
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
        python3 - "$CONFIG_ARG" >"$2" <<'EOF'
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
for V in okx binance; do
    venue_balance "$V" "$WORK/balance_start_$V" || {
        echo "error: cannot read the $V account balance for the budget guard" >&2
        exit 1
    }
done
echo "== budget guard: start free okx $(cat "$WORK/balance_start_okx") /" \
    "binance $(cat "$WORK/balance_start_binance") (cap ${BUDGET_MAX_USDT} USDT) =="

# Binance PERCENT_PRICE_BY_SIDE accepts buy prices in [0.5x avg, 2x avg]:
# REST_PX must sit inside that window around the live testnet price.
LAST="$(curl -s -m 10 'https://testnet.binance.vision/api/v3/ticker/price?symbol=BTCUSDT' |
    grep -o '"price":"[0-9.]*"' | head -1 | cut -d'"' -f4)"
if [ -z "$LAST" ]; then
    echo "error: cannot fetch BTCUSDT ticker from the testnet; aborting" >&2
    exit 1
fi
if ! python3 -c "exit(0 if float('$LAST') * 0.5 <= $REST_PX <= float('$LAST') * 2 else 1)"; then
    echo "error: BTCUSDT at $LAST puts REST_PX $REST_PX outside the" \
        "[0.5x, 2x] PERCENT_PRICE_BY_SIDE band; update REST_PX/BTC_USD" >&2
    exit 1
fi

if [ ! -x "$GATEWAY_BIN" ]; then
    echo "== building release gateway =="
    cmake --preset release >/dev/null && cmake --build --preset release --target gateway >/dev/null || {
        echo "build failed" >&2
        exit 1
    }
fi

# Derived config helper: both venues from the user's config, private REST
# port, a fresh persistence log, and risk limits that allow the suite's
# small resting orders.
derive_config() { # out-file port persistence-log [poison-okx]
    python3 - "$CONFIG_ARG" "$1" "$2" "$3" "${4:-}" <<'EOF'
import json, sys
src, dst, port, log, poison = sys.argv[1:6]
cfg = json.load(open(src))
cfg["rest"]["port"] = int(port)
cfg["persistence"] = {"logPath": log}
cfg["risk"] = {"default": {"maxQty": "1", "maxNotional": "100000",
                           "maxPosition": "10"}}
if poison:
    dead = cfg["okx"]
    dead["host"] = "127.0.0.1"  # nothing listens there: real connection refused
    dead["port"] = 9
    dead["useTls"] = False
    dead["restConnectTimeoutMs"] = 300
    dead["restReadTimeoutMs"] = 300
    dead["retry"] = {"maxAttempts": 3, "initialBackoffMs": 50, "maxBackoffMs": 200,
                     "multiplier": 2.0, "jitter": 0.0, "budgetMs": 1500}
    dead["ws"] = {"enabled": False}
json.dump(cfg, open(dst, "w"), indent=2)
EOF
}

# ============================================================================
# Scenario A: hard crash + restart with two live orders (one per venue)
# ============================================================================
echo "== A. dual-venue hard crash: 2 live orders, SIGKILL, restart, recover =="
derive_config "$WORK/gateway.json" "$GW_PORT" "$WORK/orders.jsonl"

echo "== A1. starting gateway against LIVE OKX + BINANCE (rest :$GW_PORT) =="
start_gateway "$WORK/gateway.json" "$WORK/gateway.log" GATEWAY_PID "$GW_PORT" || {
    echo "gateway did not become healthy: $(tail -5 "$WORK/gateway.log")" >&2
    exit 1
}
gw GET /health
assert_eq "GET /health -> 200" "200" "$STATUS"
assert_contains "health body ok" "$BODY" '"status":"ok"'

# The execution feeds do not replay events missed before the subscription:
# wait until BOTH venue feeds connected (startup reconcile + one reconcile
# line per venue WS connect) before the first place.
WAITED=0
until [ "$(grep -c '"event":"reconcile"' "$WORK/gateway.log" 2>/dev/null)" -ge 3 ] ||
    [ "$WAITED" -ge 30 ]; do
    sleep 0.5
    WAITED=$((WAITED + 1))
done
if [ "$(grep -c '"event":"reconcile"' "$WORK/gateway.log" 2>/dev/null)" -ge 3 ]; then
    ok "both venue feeds connected before the first place (${WAITED}s)"
else
    bad "both venue feeds did not connect within 30s (subscription may miss reports)"
fi

echo "== A2. one resting order per venue =="
ID_OKX="$(new_id OA)"
gw POST /orders "$(place_body "$ID_OKX" okx "$REST_PX" "$BASE_QTY")"
assert_eq "POST okx order -> 201" "201" "$STATUS"
[ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
ORD_OKX="$(json_field "$BODY" exchangeOrderId)"
[ -n "$ORD_OKX" ] && ok "okx assigned exchangeOrderId ($ORD_OKX)" || bad "no okx exchangeOrderId"
assert_eq "okx order state is live" "live" "$(json_field "$BODY" state)"

ID_BN="$(new_id OB)"
gw POST /orders "$(place_body "$ID_BN" binance "$REST_PX" "$BASE_QTY")"
assert_eq "POST binance order -> 201" "201" "$STATUS"
[ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
ORD_BN="$(json_field "$BODY" exchangeOrderId)"
[ -n "$ORD_BN" ] && ok "binance assigned exchangeOrderId ($ORD_BN)" || bad "no binance exchangeOrderId"
assert_eq "binance order state is live" "live" "$(json_field "$BODY" state)"

if wait_log "\"clientOrderId\":\"$ID_OKX\".*\"state\":\"live\"" 25; then
    ok "okx WS report (live) reached the gateway log"
else
    bad "no WS execution report for okx $ID_OKX"
fi
if wait_log "\"clientOrderId\":\"$ID_BN\".*\"state\":\"live\"" 25; then
    ok "binance WS report (live) reached the gateway log"
else
    bad "no WS execution report for binance $ID_BN"
fi

echo "== A3. SIGKILL the gateway mid-flight (2 orders still live at the venues) =="
kill -9 "$GATEWAY_PID" 2>/dev/null
wait "$GATEWAY_PID" 2>/dev/null
GATEWAY_PID=""
sleep 0.5
gw GET /health
assert_eq "REST API gone after SIGKILL (conn refused)" "000" "$STATUS"

echo "== A4. restart on the same config + event log =="
start_gateway "$WORK/gateway.json" "$WORK/gateway.log" GATEWAY_PID "$GW_PORT" || {
    echo "gateway did not restart cleanly: $(tail -5 "$WORK/gateway.log")" >&2
    exit 1
}
file_contains "startup replay logged" "$WORK/gateway.log" 'recovered'
file_contains "startup reconciliation logged" "$WORK/gateway.log" '"event":"reconcile"'

echo "== A5. both orders recovered, reconciled against both venues =="
gw GET "/orders/$ID_OKX"
assert_eq "okx order survived the crash (from log)" "200" "$STATUS"
assert_eq "okx state live after restart" "live" "$(json_field "$BODY" state)"
assert_eq "okx same exchangeOrderId after restart" "$ORD_OKX" "$(json_field "$BODY" exchangeOrderId)"
gw GET "/orders/$ID_BN"
assert_eq "binance order survived the crash (from log)" "200" "$STATUS"
assert_eq "binance state live after restart" "live" "$(json_field "$BODY" state)"
assert_eq "binance same exchangeOrderId after restart" "$ORD_BN" "$(json_field "$BODY" exchangeOrderId)"

echo "== A6. clientOrderId retries replay idempotently across the crash =="
gw POST /orders "$(place_body "$ID_OKX" okx "$REST_PX" "$BASE_QTY")"
assert_eq "re-POST okx id -> 201" "201" "$STATUS"
assert_eq "okx replayed=true" "true" "$(json_flag "$BODY" replayed)"
assert_eq "okx same exchangeOrderId as first place" "$ORD_OKX" "$(json_field "$BODY" exchangeOrderId)"
gw POST /orders "$(place_body "$ID_BN" binance "$REST_PX" "$BASE_QTY")"
assert_eq "re-POST binance id -> 201" "201" "$STATUS"
assert_eq "binance replayed=true" "true" "$(json_flag "$BODY" replayed)"
assert_eq "binance same exchangeOrderId as first place" "$ORD_BN" "$(json_field "$BODY" exchangeOrderId)"

echo "== A7. amend + cancel both orders through the recovered gateway =="
gw PUT "/orders/$ID_OKX" '{"price":"10001"}'
assert_eq "PUT amend okx -> 200" "200" "$STATUS"
[ "$STATUS" != "200" ] && echo "   venue reason: $(json_field "$BODY" reason)"
gw GET "/orders/$ID_OKX"
assert_eq "okx amended price visible" "10001" "$(json_field "$BODY" price)"
assert_eq "okx in-place amend kept the exchangeOrderId" "$ORD_OKX" "$(json_field "$BODY" exchangeOrderId)"
AMEND_BN="$(python3 -c "print(f'{float('$REST_PX') + 2:.2f}')")"
gw PUT "/orders/$ID_BN" "{\"price\":\"$AMEND_BN\"}"
assert_eq "PUT amend binance -> 200" "200" "$STATUS"
[ "$STATUS" != "200" ] && echo "   venue reason: $(json_field "$BODY" reason)"
gw GET "/orders/$ID_BN"
assert_eq "binance amended price visible" "$AMEND_BN" "$(json_field "$BODY" price)"
assert_ne "binance cancelReplace amend assigned a NEW exchangeOrderId" "$ORD_BN" "$(json_field "$BODY" exchangeOrderId)"
ORD_BN="$(json_field "$BODY" exchangeOrderId)"

gw DELETE "/orders/$ID_OKX"
assert_eq "DELETE okx order -> 200" "200" "$STATUS"
assert_eq "same okx exchangeOrderId in cancel ack" "$ORD_OKX" "$(json_field "$BODY" exchangeOrderId)"
gw DELETE "/orders/$ID_BN"
assert_eq "DELETE binance order -> 200" "200" "$STATUS"
assert_eq "same binance exchangeOrderId in cancel ack" "$ORD_BN" "$(json_field "$BODY" exchangeOrderId)"
if wait_state main "$ID_OKX" canceled 25; then
    ok "okx order reached state canceled"
else
    bad "okx order did not reach canceled (state=$STATE)"
fi
if wait_state main "$ID_BN" canceled 25; then
    ok "binance order reached state canceled"
else
    bad "binance order did not reach canceled (state=$STATE)"
fi
if wait_log "\"clientOrderId\":\"$ID_OKX\".*\"state\":\"canceled\"" 25; then
    ok "okx WS report (canceled) reached the gateway log"
else
    bad "no WS execution report for canceled okx $ID_OKX"
fi
if wait_log "\"clientOrderId\":\"$ID_BN\".*\"state\":\"canceled\"" 25; then
    ok "binance WS report (canceled) reached the gateway log"
else
    bad "no WS execution report for canceled binance $ID_BN"
fi
stop_gateway GATEWAY_PID

# ============================================================================
# Scenario B: venue isolation outage + recovery (okx dead, binance live)
# ============================================================================
echo "== B. venue isolation: okx dead (real closed port), binance live =="
derive_config "$WORK/isolation_dead.json" "$ISO_PORT" "$WORK/orders_iso.jsonl" poison

echo "== B1. starting isolation gateway (rest :$ISO_PORT) =="
start_gateway "$WORK/isolation_dead.json" "$WORK/iso_gateway.log" ISO_PID "$ISO_PORT" || {
    echo "isolation gateway did not become healthy: $(tail -5 "$WORK/iso_gateway.log")" >&2
    exit 1
}
gw_iso GET /health
assert_eq "isolation gateway GET /health -> 200" "200" "$STATUS"
# The okx feed is disabled here; the binance ws-api session needs a few
# seconds to come up (a place before that fails with a transport 502).
# Startup reconcile + the binance WS-connect reconcile = 2 lines.
if wait_reconcile "$WORK/iso_gateway.log" 2 30; then
    ok "binance feed connected on the isolation instance"
else
    bad "binance feed did not connect within 30s on the isolation instance"
fi

echo "== B2. okx place while okx is unreachable -> 502, intent stays pending =="
ID_D="$(new_id BD)"
T0="$SECONDS"
gw_iso POST /orders "$(place_body "$ID_D" okx "$REST_PX" "$BASE_QTY")"
assert_eq "POST okx while unreachable -> 502" "502" "$STATUS"
assert_contains "structured venue_unavailable error" "$BODY" '"code":"venue_unavailable"'
ELAPSED=$((SECONDS - T0))
[ "$ELAPSED" -le 15 ] && ok "okx retry budget honored (failed in ${ELAPSED}s)" ||
    bad "okx place took ${ELAPSED}s to fail (budget not honored?)"
gw_iso GET "/orders/$ID_D"
assert_eq "unacked okx intent visible via GET" "200" "$STATUS"
assert_eq "unacked okx intent state is pending" "pending" "$(json_field "$BODY" state)"
assert_eq "unacked okx intent carries no exchangeOrderId" "" "$(json_field "$BODY" exchangeOrderId)"
gw_iso POST /orders "$(place_body "$ID_D" okx "$REST_PX" "$BASE_QTY")"
assert_eq "same-id okx retry while unacked -> 202" "202" "$STATUS"
assert_eq "same-id retry state is pending" "pending" "$(json_field "$BODY" state)"
assert_eq "same-id retry carries no exchangeOrderId" "" "$(json_field "$BODY" exchangeOrderId)"

echo "== B3. binance still fully live on the same instance =="
ID_IB="$(new_id BE)"
gw_iso POST /orders "$(place_body "$ID_IB" binance "$REST_PX" "$BASE_QTY")"
assert_eq "POST binance while okx is dead -> 201" "201" "$STATUS"
[ "$STATUS" != "201" ] && echo "   venue reason: $(json_field "$BODY" reason)"
ORD_IB="$(json_field "$BODY" exchangeOrderId)"
[ -n "$ORD_IB" ] && ok "binance assigned exchangeOrderId ($ORD_IB)" || bad "no binance exchangeOrderId"
assert_eq "binance order state is live (isolation held)" "live" "$(json_field "$BODY" state)"

echo "== B4. restore the LIVE okx section (same port + SAME event log), restart =="
RECON_BASELINE="$(grep -c '"event":"reconcile"' "$WORK/iso_gateway.log" 2>/dev/null || true)"
stop_gateway ISO_PID
derive_config "$WORK/isolation_live.json" "$ISO_PORT" "$WORK/orders_iso.jsonl"
start_gateway "$WORK/isolation_live.json" "$WORK/iso_gateway.log" ISO_PID "$ISO_PORT" || {
    echo "gateway did not restart cleanly: $(tail -5 "$WORK/iso_gateway.log")" >&2
    exit 1
}
file_contains "iso restart replay logged" "$WORK/iso_gateway.log" 'recovered'
file_contains "iso startup reconciliation logged" "$WORK/iso_gateway.log" '"event":"reconcile"'
# Both feeds (okx re-enabled + binance) must be connected before the
# cancel in B6 goes out over the binance ws-api session.
if wait_reconcile "$WORK/iso_gateway.log" "$((RECON_BASELINE + 2))" 30; then
    ok "both feeds reconnected after the okx recovery"
else
    bad "both feeds did not reconnect within 30s after the okx recovery"
fi

echo "== B5. pending okx order resolved against live okx -> rejected venue_absent =="
if wait_state iso "$ID_D" rejected 30; then
    ok "pending okx order resolved to rejected after okx recovery"
else
    bad "pending okx order did not resolve within 30s (state=$STATE)"
fi
gw_iso GET "/orders/$ID_D"
assert_contains "rejection reason is venue_absent" "$BODY" '"code":"venue_absent"'

echo "== B6. binance order untouched by the okx outage + recovery =="
gw_iso GET "/orders/$ID_IB"
assert_eq "binance order visible after restart" "200" "$STATUS"
assert_eq "binance order state live after restart" "live" "$(json_field "$BODY" state)"
assert_eq "binance order kept its exchangeOrderId" "$ORD_IB" "$(json_field "$BODY" exchangeOrderId)"
gw_iso DELETE "/orders/$ID_IB"
assert_eq "binance order cancelable through the gateway" "200" "$STATUS"
gw_iso DELETE "/orders/$ID_D"
assert_eq "DELETE rejected okx order -> 409 order_terminal" "409" "$STATUS"
assert_contains "order_terminal code" "$BODY" '"code":"order_terminal"'
stop_gateway ISO_PID

# ---- budget guard: net demo-fund consumption of this run --------------------
echo "== budget check (cap ${BUDGET_MAX_USDT} USDT, BTC valued at $BTC_USD) =="
TOTAL_OK="0"
for V in okx binance; do
    if venue_balance "$V" "$WORK/balance_end_$V"; then
        START_USDT="$(awk '{print $1}' "$WORK/balance_start_$V")"
        START_BTC="$(awk '{print $2}' "$WORK/balance_start_$V")"
        END_USDT="$(awk '{print $1}' "$WORK/balance_end_$V")"
        END_BTC="$(awk '{print $2}' "$WORK/balance_end_$V")"
        SPENT="$(python3 - "$START_USDT" "$START_BTC" "$END_USDT" "$END_BTC" "$BTC_USD" <<'EOF'
import sys
start_usdt, start_btc, end_usdt, end_btc, btc_usd = map(float, sys.argv[1:6])
usdt_spent = start_usdt - end_usdt
btc_net = end_btc - start_btc
print(f"{max(usdt_spent, 0.0) + btc_net * btc_usd:.2f}")
EOF
        )"
        echo "   $V: USDT $START_USDT -> $END_USDT, BTC $START_BTC -> $END_BTC (charged $SPENT USDT)"
        TOTAL_OK="$(python3 -c "print(f'{float('$TOTAL_OK') + float('$SPENT'):.2f}')")"
    else
        bad "cannot re-read the $V balance for the budget check"
    fi
done
if python3 -c "exit(0 if float('$TOTAL_OK') < $BUDGET_MAX_USDT else 1)"; then
    ok "net consumption $TOTAL_OK USDT < ${BUDGET_MAX_USDT} budget"
else
    bad "net consumption $TOTAL_OK USDT exceeds the ${BUDGET_MAX_USDT} budget"
fi

# ---- summary ----------------------------------------------------------------
echo
echo "== live dual-venue recovery suite: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
