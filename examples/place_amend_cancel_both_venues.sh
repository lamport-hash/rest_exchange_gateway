#!/usr/bin/env bash
# Exercise the unified API across BOTH venues: place on each, amend, cancel,
# and observe the unified snapshots. The client schema is identical for both
# venues — only the "venue" field changes; symbols stay in gateway spelling
# (BTC-USDT) and are translated per adapter (BTCUSDT on Binance).
#
# Requires a gateway configured with both an "okx" and a "binance" section
# (see config/gateway.example.json) and started via examples/run_gateway.sh.
#
# Usage: examples/place_amend_cancel_both_venues.sh [port]
#
# clientOrderId must be 1-32 alphanumeric characters (enforced by the
# gateway), so a timestamp suffix keeps ids unique per run.

set -euo pipefail

cd "$(dirname "$0")/.."

PORT="${1:-8080}"
ID_OKX="ok$(date +%s%N | tail -c 16)"
ID_BINANCE="bi$(date +%s%N | tail -c 16)"

http() { # method path [data]
    local method="$1" path="$2" data="${3:-}"
    if [ -n "${data}" ]; then
        docker compose exec -T dev curl -s -w "\n%{http_code}" -X "${method}" \
            "http://127.0.0.1:${PORT}${path}" \
            -H 'Content-Type: application/json' -d "${data}"
    else
        docker compose exec -T dev curl -s -w "\n%{http_code}" -X "${method}" \
            "http://127.0.0.1:${PORT}${path}"
    fi
}

show() { # label raw_response (body\nstatus)
    local body status
    body="${1%$'\n'*}"
    status="${1##*$'\n'}"
    echo "--- ${2} [HTTP ${status}]"
    echo "${body}" | python3 -m json.tool 2>/dev/null || echo "${body}"
}

expect() { # expected_status raw_response label
    if [ "${1##*$'\n'}" != "${2}" ]; then
        echo "error: ${3} did not return HTTP ${2}" >&2
        exit 1
    fi
}

echo "== OKX: place / amend / cancel =="
PLACE=$(http POST /orders "{\"clientOrderId\":\"${ID_OKX}\",\"venue\":\"OKX\",\"symbol\":\"BTC-USDT\",\"side\":\"buy\",\"type\":\"limit\",\"price\":\"10000\",\"quantity\":\"0.0001\"}")
show "${PLACE}" "place (venue OKX)"
expect "${PLACE}" 201 "OKX place"

AMEND=$(http PUT "/orders/${ID_OKX}" '{"price":"10001","quantity":"0.0002"}')
show "${AMEND}" "amend (price + quantity)"
expect "${AMEND}" 200 "OKX amend"

CANCEL=$(http DELETE "/orders/${ID_OKX}")
show "${CANCEL}" "cancel"
expect "${CANCEL}" 200 "OKX cancel"

echo
echo "== Binance: place / amend (cancelReplace emulation) / cancel =="
PLACE=$(http POST /orders "{\"clientOrderId\":\"${ID_BINANCE}\",\"venue\":\"BINANCE\",\"symbol\":\"BTC-USDT\",\"side\":\"buy\",\"type\":\"limit\",\"price\":\"10000\",\"quantity\":\"0.0001\",\"timeInForce\":\"GTC\"}")
show "${PLACE}" "place (venue BINANCE, symbol translated to BTCUSDT)"
expect "${PLACE}" 201 "Binance place"

AMEND=$(http PUT "/orders/${ID_BINANCE}" '{"price":"10001"}')
show "${AMEND}" "amend (venue replaces the order; same clientOrderId, NEW exchangeOrderId)"
expect "${AMEND}" 200 "Binance amend"

CANCEL=$(http DELETE "/orders/${ID_BINANCE}")
show "${CANCEL}" "cancel"
expect "${CANCEL}" 200 "Binance cancel"

echo
echo "== venue omitted: the configured default venue is used =="
DEFAULT_ID="df$(date +%s%N | tail -c 16)"
PLACE=$(http POST /orders "{\"clientOrderId\":\"${DEFAULT_ID}\",\"symbol\":\"BTC-USDT\",\"side\":\"buy\",\"type\":\"limit\",\"price\":\"10000\",\"quantity\":\"0.0001\"}")
show "${PLACE}" "place (no venue field)"
expect "${PLACE}" 201 "default-venue place"
http DELETE "/orders/${DEFAULT_ID}" >/dev/null

echo
echo "== both orders in the unified registry =="
show "$(http GET "/orders/${ID_OKX}")" "OKX order final state"
show "$(http GET "/orders/${ID_BINANCE}")" "Binance order final state"
