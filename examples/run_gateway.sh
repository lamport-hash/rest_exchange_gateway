#!/usr/bin/env bash
# Build (if needed) and launch the gateway inside the Docker dev container.
#
# Usage: examples/run_gateway.sh [config_path]
#   config_path  path to the gateway config (default: config/gateway.json,
#                gitignored — copy from config/gateway.example.json and fill
#                in OKX demo credentials)
#
# Requires only Docker Engine + Docker Compose v2 on the host; everything
# else (curl, toolchain) lives in the dev image. Source is bind-mounted, so
# host edits are picked up on the next run.

set -euo pipefail

cd "$(dirname "$0")/.."

CONFIG="${1:-config/gateway.json}"
PORT=$(python3 -c "import json;print(json.load(open('${CONFIG}'))['rest']['port'])" 2>/dev/null ||
    grep -o '"port"[[:space:]]*:[[:space:]]*[0-9]*' "${CONFIG}" | head -1 | grep -o '[0-9]*$')

if [ ! -f "${CONFIG}" ]; then
    echo "error: config ${CONFIG} not found (copy config/gateway.example.json and fill in credentials)" >&2
    exit 1
fi
if [ -z "${PORT}" ]; then
    echo "error: cannot read rest.port from ${CONFIG}" >&2
    exit 1
fi

echo "==> starting dev container"
docker compose up -d dev

if ! docker compose exec -T dev test -x build/release/gateway; then
    echo "==> release binary missing, building (first build takes a while)"
    docker compose exec -T dev cmake --preset release >/dev/null
    docker compose exec -T dev cmake --build --preset release
fi

echo "==> stopping any previously running gateway"
docker compose exec -T dev pkill -x gateway 2>/dev/null || true
sleep 1

echo "==> launching gateway (config: ${CONFIG}, port: ${PORT})"
docker compose exec -d dev ./build/release/gateway "${CONFIG}"

echo -n "==> waiting for /health"
for _ in $(seq 1 50); do
    if docker compose exec -T dev curl -sf "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
        echo
        echo "gateway is up: http://127.0.0.1:${PORT} (inside the container)"
        echo "try: examples/place_and_cancel.sh"
        exit 0
    fi
    echo -n "."
    sleep 0.2
done
echo
echo "error: gateway did not become healthy within 10s" >&2
exit 1
