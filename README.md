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
docker compose exec dev ./build/release/gateway config/gateway.json
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
