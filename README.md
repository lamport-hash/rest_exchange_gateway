# rest_exchange_gateway

REST gateway exposing one unified API over multiple cryptocurrency exchanges
(OKX first, Binance later). C++20, Crow, cpp-httplib, nlohmann/json, doctest.

Spec: `doc/project-spec.md` — Architecture: `doc/project-archi.md` —
Plan: `doc/implementation-plan.md` / `implement-todos.md`.

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

Run the gateway (needs `config/gateway.json` — copy `config/gateway.example.json`
and fill in venue credentials; the file is gitignored):

```bash
examples/run_gateway.sh                      # or manually:
docker compose exec dev ./build/release/gateway config/gateway.json
```

Place then cancel a small demo limit order through the gateway:

```bash
examples/place_and_cancel.sh                 # optional: port instrument price qty
```

Black-box Phase 2 client suite — a one-shot Docker container acting as an
external client: it starts a scriptable fake OKX venue (`mock_okx_env`:
REST + private WS + HTTP fault-control plane) plus the real gateway binary,
then drives every resilience point over HTTP with curl (retry on dropped /
delayed responses, idempotent place/cancel retries, no double-applied
orders, WS execution reports, disconnect/reconnect, venue death/recovery):

```bash
tests/blackbox/run_docker_client.sh          # 32 assertions, ~10 seconds
# or directly inside the dev container:
docker compose exec dev tests/blackbox/phase2_client_tests.sh
```

REST surface (phase 1, OKX backend):

- `POST /orders` — `{"clientOrderId","instrumentId","side":"buy|sell","type":"limit|market","price","quantity"}` → 201
- `GET /orders/{clientOrderId}?instrumentId=BTC-USDT` → order snapshot
- `DELETE /orders/{clientOrderId}?instrumentId=BTC-USDT` → cancel
- `GET /health`
- Errors: `{"error":{"code","reason","clientOrderId"}}` (400 invalid request,
  404 not found, 409 venue rejected, 502 venue unreachable, 500 internal)

Stop everything (named volumes `build/` and `ccache/` are kept):

```bash
docker compose down
```

A production multi-stage build for the final binary will be added later.
