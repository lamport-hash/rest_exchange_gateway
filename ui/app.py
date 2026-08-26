"""Monitor backend for the REST exchange gateway.

Served by the compose "ui" service (uvicorn ui.app:app). Talks to the
compose "gateway" service over HTTP.

Reads of the gateway config touch only the REST port and venue names —
credentials are never read or exposed by this process.
"""

from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

ROOT = Path(__file__).resolve().parent.parent
GATEWAY_URL = os.environ.get("GATEWAY_URL", "http://gateway:8080")
STDOUT_LOG = ROOT / "data" / "gateway-stdout.jsonl"

# ------------------------------------------------------------ gateway reads ---


def _gateway_get(path: str, timeout: float = 2.0) -> tuple[int, Any]:
    with urllib.request.urlopen(GATEWAY_URL + path, timeout=timeout) as resp:  # noqa: S310
        return resp.status, json.loads(resp.read().decode())


def _tail_events(limit: int = 40) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    try:
        with STDOUT_LOG.open("rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            fh.seek(max(0, size - 256 * 1024))
            for line in fh.read().decode(errors="replace").splitlines():
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(entry, dict) and "event" in entry:
                    events.append(entry)
    except OSError:
        pass
    return events[-limit:]


# ------------------------------------------------------------------- app ----

app = FastAPI(title="gateway monitor", docs_url=None, redoc_url=None, openapi_url=None)
app.mount("/static", StaticFiles(directory=str(ROOT / "ui" / "static")), name="static")


@app.get("/")
def index() -> FileResponse:
    return FileResponse(ROOT / "ui" / "static" / "index.html")


@app.get("/api/gateway/status")
def gateway_status() -> JSONResponse:
    health = None
    connected = False
    try:
        status_code, health = _gateway_get("/health")
        connected = status_code == 200
    except Exception:
        pass
    events = _tail_events()
    feed_ok = None
    for entry in reversed(events):
        if entry.get("event") == "reconcile":
            feed_ok = True
            break
        if entry.get("event") == "feed_disconnected":
            feed_ok = False
            break
    return JSONResponse(
        {
            "connected": connected,
            "health": health,
            "feed_ok": feed_ok,
            "events": events[-12:],
            "gateway_url": GATEWAY_URL,
        }
    )


@app.get("/api/orders")
def orders() -> JSONResponse:
    try:
        _, body = _gateway_get("/orders", timeout=4.0)
        return JSONResponse({"orders": body.get("orders", [])})
    except Exception as exc:
        return JSONResponse({"orders": [], "error": str(exc)}, status_code=200)


@app.get("/api/price")
def price(symbol: str = "BTC-USDT", venue: str | None = None) -> JSONResponse:
    """Last-traded price of a pair, served by the gateway (Monitor tab)."""
    path = "/price/" + urllib.parse.quote(symbol)
    if venue:
        path += "?venue=" + urllib.parse.quote(venue)
    try:
        _, body = _gateway_get(path, timeout=4.0)
        return JSONResponse(body)
    except Exception as exc:
        return JSONResponse({"error": str(exc)}, status_code=200)


@app.get("/api/risk")
def risk() -> JSONResponse:
    """Active pre-trade risk limits served by the gateway (Risk tab).

    Read-only: limits are fixed by the config file at gateway startup.
    """
    try:
        _, body = _gateway_get("/risk", timeout=4.0)
        return JSONResponse(body)
    except Exception as exc:
        return JSONResponse({"error": str(exc)}, status_code=200)


# --------------------------------------------------- spec 3.1 order flow ---

# Traceability matrix for doc/project-spec.md §3.1 "Supported Order Flow".
# Suite names are static references to the repo's covering suites (ctest
# suite names, blackbox, live venue ids). Implementation refs are
# file:line into the current tree.
ORDER_FLOW_SPEC: dict[str, Any] = {
    "spec": {
        "section": "3.1",
        "title": "Supported Order Flow",
        "source": "doc/project-spec.md",
    },
    "requirements": [
        {
            "id": "new-limit",
            "requirement": "New order — Limit",
            "implementation": [
                {"ref": "src/core/oms.cpp:278", "what": "OMS::place — risk checks, idempotent replay, raced-report arbitration"},
                {"ref": "src/rest/order_routes.cpp:121", "what": "POST /orders (type limit|market, price required)"},
                {"ref": "include/gateway/exchange_connector.hpp:134", "what": "ExchangeConnector::place_order seam"},
            ],
            "tests": ["oms_test", "okx_connector_test", "binance_connector_test", "rest_api_test", "blackbox", "live_okx", "live_binance"],
        },
        {
            "id": "new-market",
            "requirement": "New order — Market",
            "implementation": [
                {"ref": "src/rest/order_routes.cpp:55", "what": "parse_order_type — market: price forbidden"},
                {"ref": "include/gateway/exchange_connector.hpp:21", "what": "OrderType::Market"},
            ],
            "tests": ["rest_api_test", "okx_wire_test", "binance_wire_test", "live_okx", "live_binance"],
        },
        {
            "id": "cancel",
            "requirement": "Cancel order",
            "implementation": [
                {"ref": "src/core/oms.cpp:404", "what": "OMS::cancel — idempotent, order_terminal refusal"},
                {"ref": "src/rest/order_routes.cpp:268", "what": "DELETE /orders/{clientOrderId}"},
                {"ref": "include/gateway/exchange_connector.hpp:138", "what": "ExchangeConnector::cancel_order seam"},
            ],
            "tests": ["oms_test", "okx_connector_test", "binance_connector_test", "rest_api_test", "blackbox", "live_okx", "live_binance"],
        },
        {
            "id": "amend",
            "requirement": "Replace / amend order",
            "implementation": [
                {"ref": "src/core/oms.cpp:455", "what": "OMS::amend — re-runs risk, arbitrates cancelReplace legs"},
                {"ref": "src/rest/order_routes.cpp:289", "what": "PUT /orders/{clientOrderId} (price and/or quantity)"},
                {"ref": "include/gateway/exchange_connector.hpp:143", "what": "ExchangeConnector::amend_order seam (per-venue semantics documented)"},
            ],
            "tests": ["oms_test", "okx_rest_client_test", "okx_connector_test", "binance_connector_test", "rest_api_test", "blackbox", "live_okx", "live_binance"],
        },
        {
            "id": "state-pending",
            "requirement": "Normalized state: Pending (gateway-local)",
            "implementation": [
                {"ref": "src/core/oms.cpp:347", "what": "place() records Pending + persists place_submitted BEFORE the venue call"},
                {"ref": "src/core/order_state.hpp:16", "what": "Pending resolves forward only (ack/fill/reject); Pending -> Canceled illegal"},
                {"ref": "src/core/oms.cpp:436", "what": "order_pending refusal — cancel/amend of an unacked order never reaches the venue"},
                {"ref": "src/rest/order_routes.cpp:251", "what": "unacked place: 202 {state:pending, exchangeOrderId:\"\"}, duplicate POSTs replay it"},
            ],
            "tests": ["order_state_test", "oms_test", "rest_api_test"],
        },
        {
            "id": "state-live",
            "requirement": "Normalized state: New / Live",
            "implementation": [
                {"ref": "include/gateway/exchange_connector.hpp:35", "what": "OrderState enum — the six normalized states"},
                {"ref": "src/core/order_state.hpp:35", "what": "kLegalTransitions — explicit legal-transition table"},
            ],
            "tests": ["order_state_test", "oms_test", "okx_wire_test", "binance_wire_test"],
        },
        {
            "id": "state-partial",
            "requirement": "Normalized state: Partially Filled",
            "implementation": [
                {"ref": "src/core/oms.cpp:582", "what": "apply_observation — monotonic fill high-water mark"},
                {"ref": "src/core/order_state.hpp:35", "what": "transition table (partial -> partial/filled/canceled legal)"},
            ],
            "tests": ["order_state_test", "oms_test", "okx_connector_test"],
        },
        {
            "id": "state-filled",
            "requirement": "Normalized state: Filled (terminal)",
            "implementation": [
                {"ref": "src/core/order_state.hpp:41", "what": "is_terminal — Filled/Canceled/Rejected end the lifecycle"},
            ],
            "tests": ["order_state_test", "oms_test", "rest_api_test"],
        },
        {
            "id": "state-canceled",
            "requirement": "Normalized state: Canceled (terminal)",
            "implementation": [
                {"ref": "src/core/oms.cpp:404", "what": "OMS::cancel — cancel ack transitions to Canceled, fills retained"},
            ],
            "tests": ["order_state_test", "oms_test", "rest_api_test"],
        },
        {
            "id": "state-rejected",
            "requirement": "Normalized state: Rejected (terminal)",
            "implementation": [
                {"ref": "src/core/oms.cpp:333", "what": "risk rejection recorded on the candidate, replayed to retries"},
                {"ref": "src/core/oms.cpp:368", "what": "definitive venue rejection recorded, replayed to retries"},
            ],
            "tests": ["oms_test", "risk_test", "rest_api_test"],
        },
        {
            "id": "explicit-mapping",
            "requirement": "Client <-> exchange semantics mapping is explicit",
            "implementation": [
                {"ref": "src/exchange/okx/okx_wire.cpp:21", "what": "map_okx_state — OKX wire values -> OrderState"},
                {"ref": "src/exchange/binance/binance_wire.cpp:199", "what": "map_binance_state — Binance statuses -> OrderState"},
                {"ref": "src/exchange/okx/okx_wire.cpp:38", "what": "map_okx_side — buy|sell <-> venue spelling"},
                {"ref": "src/exchange/binance/binance_wire.cpp:220", "what": "map_binance_side — buy|sell <-> BUY|SELL"},
            ],
            "tests": ["okx_wire_test", "binance_wire_test", "okx_connector_test", "binance_connector_test"],
        },
    ],
    # The explicit client-concept -> venue-semantics tables (spec: mapping
    # "must be explicit"). Mirrors the map_*_state functions verbatim.
    "flow_mapping": [
        {
            "flow": "New order (limit / market)",
            "client": "POST /orders — type limit|market, side buy|sell",
            "okx": "POST /api/v5/trade/order — ordType limit|market, side buy|sell",
            "binance": "WS-API order.place — type LIMIT|MARKET, side BUY|SELL",
        },
        {
            "flow": "Cancel order",
            "client": "DELETE /orders/{clientOrderId}",
            "okx": "POST /api/v5/trade/cancel-order — keyed by clOrdId",
            "binance": "WS-API order.cancel — keyed by newClientOrderId",
        },
        {
            "flow": "Replace / amend order",
            "client": "PUT /orders/{clientOrderId} — price and/or quantity, id stays stable",
            "okx": "POST /api/v5/trade/amend-order — same ordId, newPx/newSz in place",
            "binance": "WS-API order.cancelReplace (STOP_ON_FAILURE) — new exchange id, clientOrderId stays stable",
        },
    ],
    "state_mapping": [
        {
            "state": "pending",
            "meaning": "place sent, venue has not acked — gateway-local; unacked POST replays 202 until a venue observation or reconcile resolves it",
            "okx": "no wire state — venue reports never carry it",
            "binance": "no wire state — venue reports never carry it",
        },
        {
            "state": "live",
            "meaning": "New / Live — venue acked, working on the book",
            "okx": "live",
            "binance": "NEW, PENDING_NEW",
        },
        {
            "state": "partially_filled",
            "meaning": "some quantity executed, remainder working",
            "okx": "partially_filled",
            "binance": "PARTIALLY_FILLED",
        },
        {
            "state": "filled",
            "meaning": "full quantity executed — terminal",
            "okx": "filled",
            "binance": "FILLED",
        },
        {
            "state": "canceled",
            "meaning": "canceled (client or venue), remainder gone — terminal",
            "okx": "canceled",
            "binance": "CANCELED, PENDING_CANCEL, EXPIRED, EXPIRED_IN_MATCH",
        },
        {
            "state": "rejected",
            "meaning": "refused before resting (risk or venue) — terminal",
            "okx": "no wire state — definitive venue error envelope (sCode) at place time",
            "binance": "REJECTED",
        },
    ],
    "mapping_note": "Unknown venue values map to nullopt — the report is discarded "
    "(counted in /health reportsStale), never an error and never a wrong state.",
}


@app.get("/api/spec/order-flow")
def spec_order_flow() -> JSONResponse:
    """Spec 3.1 traceability matrix (Order flow tab): requirements,
    implementation refs and covering suites."""
    return JSONResponse(ORDER_FLOW_SPEC)


class _DemoBalanceRequest(BaseModel):
    type: str
    ccy: str
    amt: str


@app.post("/api/okx/demo-balance")
def okx_demo_balance(req: _DemoBalanceRequest) -> JSONResponse:
    """Forward one OKX demo-balance adjustment to the gateway (Balance tab).

    The gateway validates the shape again and refuses with 400 when OKX
    demo trading is disabled; venue rejections come back as 409.
    """
    if req.type not in ("increase", "reduce"):
        raise HTTPException(status_code=400, detail="type must be increase or reduce")
    if not req.ccy.strip():
        raise HTTPException(status_code=400, detail="ccy is required")
    payload = json.dumps(
        {"type": req.type, "adjustments": [{"ccy": req.ccy.strip(), "amt": req.amt.strip()}]}
    ).encode()
    request = urllib.request.Request(  # noqa: S310 - fixed gateway base URL
        GATEWAY_URL + "/venue/okx/demo-adjust-balance", data=payload, method="POST",
        headers={"Content-Type": "application/json"},
    )
    started = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=15.0) as resp:
            status, raw = resp.status, resp.read().decode(errors="replace")
    except urllib.error.HTTPError as exc:
        status, raw = exc.code, exc.read().decode(errors="replace")
    except Exception as exc:
        return JSONResponse(
            {"ok": False, "status": None,
             "latency_ms": round((time.perf_counter() - started) * 1000),
             "error": f"gateway unreachable: {exc}"}
        )
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        parsed = raw
    return JSONResponse(
        {"ok": status < 300, "status": status,
         "latency_ms": round((time.perf_counter() - started) * 1000),
         "body": parsed}
    )


# ------------------------------------------------------------- api playground ---

_PROXY_METHODS = {"GET", "POST", "PUT", "DELETE"}


class _ProxyRequest(BaseModel):
    method: str
    path: str
    body: str | None = None


_PROXY_PATH_PREFIXES = (
    "/orders",
    "/price",
    "/risk",
    "/health",
    "/consistency",
    "/venue/okx/demo-adjust-balance",
)


@app.post("/api/proxy")
def proxy(req: _ProxyRequest) -> JSONResponse:
    """Forward one hand-crafted request to the gateway (API playground).

    Only the gateway's own REST surface is reachable: the path must start
    with a known route prefix, and only idempotent-safe methods plus the
    documented order verbs are allowed.
    """
    method = req.method.upper()
    if method not in _PROXY_METHODS:
        raise HTTPException(status_code=400, detail=f"method {method} not allowed")
    path = req.path
    if not path.startswith("/") or ".." in path or "://" in path:
        raise HTTPException(status_code=400, detail="path must be an absolute gateway path")
    if not path.startswith(_PROXY_PATH_PREFIXES):
        raise HTTPException(
            status_code=400,
            detail="path must target /orders, /price, /risk, /health, /consistency"
            " or /venue/okx/demo-adjust-balance",
        )

    data = None
    if method in ("POST", "PUT"):
        if req.body:
            try:
                data = json.dumps(json.loads(req.body)).encode()
            except json.JSONDecodeError as exc:
                raise HTTPException(status_code=400, detail=f"body is not valid JSON: {exc}")
    elif req.body:
        raise HTTPException(status_code=400, detail=f"{method} must not carry a body")

    request = urllib.request.Request(  # noqa: S310 - fixed gateway base URL
        GATEWAY_URL + path, data=data, method=method,
        headers={"Content-Type": "application/json"},
    )
    started = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=15.0) as resp:
            status, raw = resp.status, resp.read().decode(errors="replace")
    except urllib.error.HTTPError as exc:
        status, raw = exc.code, exc.read().decode(errors="replace")
    except Exception as exc:
        return JSONResponse(
            {"ok": False, "status": None, "latency_ms": round((time.perf_counter() - started) * 1000),
             "error": f"gateway unreachable: {exc}"}
        )
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        parsed = raw
    return JSONResponse(
        {"ok": True, "status": status, "latency_ms": round((time.perf_counter() - started) * 1000),
         "body": parsed}
    )

