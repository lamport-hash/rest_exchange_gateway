#!/usr/bin/env bash
# Place a small limit order through the gateway, show its state, cancel it,
# and show the final state. Runs against the gateway started by
# examples/run_gateway.sh (default http://127.0.0.1:8080 inside the dev
# container).
#
# Usage: examples/place_and_cancel.sh [port] [instrument] [price] [quantity]
#   port        gateway REST port        (default 8080)
#   instrument  OKX instrument id        (default BTC-USDT)
#   price       limit price              (default 10000 — far from market so
#                                         the order stays live)
#   quantity    order size               (default 0.0001 — fits the OKX demo
#                                         wallet)
#
# clientOrderId must be 1-32 alphanumeric characters (OKX rule, enforced by
# the gateway), so a timestamp suffix is used for uniqueness.

set -euo pipefail

cd "$(dirname "$0")/.."

PORT="${1:-8080}"
INSTRUMENT="${2:-BTC-USDT}"
PRICE="${3:-10000}"
QUANTITY="${4:-0.0001}"
CLIENT_ORDER_ID="gw$(date +%s%N | tail -c 16)" # last 16 digits of ns clock

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

echo "== placing limit order: buy ${QUANTITY} ${INSTRUMENT} @ ${PRICE} (clientOrderId: ${CLIENT_ORDER_ID})"
PLACE=$(http POST /orders "{\"clientOrderId\":\"${CLIENT_ORDER_ID}\",\"venue\":\"OKX\",\"symbol\":\"${INSTRUMENT}\",\"side\":\"buy\",\"type\":\"limit\",\"price\":\"${PRICE}\",\"quantity\":\"${QUANTITY}\"}")
PLACE_STATUS="${PLACE##*$'\n'}"
show "${PLACE}" "place"
if [ "${PLACE_STATUS}" != "201" ]; then
    echo "error: placement failed" >&2
    exit 1
fi

show "$(http GET "/orders/${CLIENT_ORDER_ID}")" "status before cancel"
show "$(http DELETE "/orders/${CLIENT_ORDER_ID}")" "cancel"
show "$(http GET "/orders/${CLIENT_ORDER_ID}")" "status after cancel"
