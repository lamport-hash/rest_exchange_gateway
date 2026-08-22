#!/usr/bin/env bash
# Entrypoint of the compose "gateway" app service: build the release
# gateway when missing, then exec it with the live config. Stdout is
# mirrored to data/gateway-stdout.jsonl so the monitor UI can tail
# venue feed events (execution_report / reconcile / feed_disconnected).
set -euo pipefail

ROOT=/workspace/rest_exchange_gateway
cd "$ROOT"

CONFIG="${GATEWAY_CONFIG:-$ROOT/config/gateway.json.secret}"
if [ ! -f "$CONFIG" ]; then
    echo "gateway-app: config $CONFIG missing (copy config/gateway.example.json and fill credentials)" >&2
    exit 1
fi

if [ ! -x "$ROOT/build/release/gateway" ]; then
    echo "gateway-app: building release gateway (first start)..."
    cmake --preset release >/dev/null
    cmake --build --preset release --target gateway
fi

mkdir -p "$ROOT/data"
exec > >(tee -a "$ROOT/data/gateway-stdout.jsonl") 2>&1
exec "$ROOT/build/release/gateway" "$CONFIG"
