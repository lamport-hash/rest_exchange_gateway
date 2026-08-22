"""Monitor + test-launcher backend for the REST exchange gateway.

Served by the compose "ui" service (uvicorn ui.app:app). Talks to the
compose "gateway" service over HTTP and runs the repo's test suites
(ctest / blackbox / live venue scripts) as serialized background jobs.

Reads of the gateway config touch only the REST port and venue names —
credentials are never read or exposed by this process.
"""

from __future__ import annotations

import json
import os
import queue
import re
import signal
import subprocess
import threading
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
RUNS_DIR = ROOT / "data" / "ui-runs"
GATEWAY_URL = os.environ.get("GATEWAY_URL", "http://gateway:8080")
STDOUT_LOG = ROOT / "data" / "gateway-stdout.jsonl"

UNIT_TIMEOUT_S = 900
LIVE_TIMEOUT_S = 1800

# ---------------------------------------------------------------- catalog ---

UNIT_SUITES: dict[str, str] = {
    "smoke_test": "Build/boot smoke: linkage and basic invariants",
    "result_test": "Result<T> value/error type used across the gateway",
    "retry_test": "Retry budget: exponential backoff + jitter (venue unreachable → 502)",
    "decimal_test": "Exact decimal parse/compare for money fields (no float drift)",
    "order_state_test": "Order state machine transition table (illegal transitions rejected)",
    "event_log_test": "Append-only JSONL persistence: torn-tail safety, replay",
    "risk_test": "Pre-trade risk: maxQty / maxNotional / maxPosition",
    "oms_test": "OMS: idempotency, duplicate/out-of-order reports, REST-vs-WS races",
    "clock_test": "Epoch vs ISO timestamps (OKX WS login 60004 fix)",
    "config_test": "Config parsing/validation (venues, risk, persistence)",
    "okx_signer_test": "OKX HMAC-SHA256 signing against documentation vectors",
    "okx_wire_test": "OKX wire translation + common-schema enforcement",
    "okx_resilient_test": "Connector resilience: dropped acks, timeouts, reconnect",
    "okx_rest_client_test": "OKX REST client: place/cancel/amend, venue error mapping",
    "okx_connector_test": "OKX adapter end-to-end against the mock venue",
    "okx_ws_client_test": "OKX private WS feed: login, subscribe, keepalive, reconnect",
    "rest_api_test": "REST API layer: schema validation, error mapping, GET /orders listing",
    "binance_signer_test": "Binance HMAC signing (official documentation vector)",
    "binance_wire_test": "Binance wire translation, symbol BTC-USDT <-> BTCUSDT",
    "binance_ws_client_test": "Binance WS-API client: notifier thread, cancel-report keying",
    "binance_connector_test": "Binance adapter: WS place/cancel, cancelReplace amend",
}

SPECIAL_TESTS: list[dict[str, Any]] = [
    {
        "id": "blackbox",
        "name": "blackbox (phase2_client_tests)",
        "kind": "blackbox",
        "description": "Black-box client suite: real gateway vs scripted mock venue, "
        "fault injection, restart recovery (44 assertions)",
        "timeout_s": 600,
        "needs_confirm": False,
        "command": ["bash", "tests/blackbox/phase2_client_tests.sh"],
    },
    {
        "id": "live_okx",
        "name": "live okx (demo trading)",
        "kind": "live",
        "description": "Real OKX demo venue: signed place/amend/cancel, WS reports, "
        "real fills, restart recovery, adoption (85 assertions, ~2-3 min, "
        "spends small demo funds)",
        "timeout_s": LIVE_TIMEOUT_S,
        "needs_confirm": True,
        "command": ["bash", "tests/live/live_func_tests.sh", "okx"],
    },
    {
        "id": "live_binance",
        "name": "live binance (spot testnet)",
        "kind": "live",
        "description": "Real Binance spot testnet: WS-API orders, cancelReplace amend, "
        "fills, restart recovery, adoption (85 assertions, ~2-3 min, "
        "spends small testnet funds)",
        "timeout_s": LIVE_TIMEOUT_S,
        "needs_confirm": True,
        "command": ["bash", "tests/live/live_func_tests.sh", "binance"],
    },
]


def catalog() -> list[dict[str, Any]]:
    tests: list[dict[str, Any]] = []
    for preset in ("debug", "release"):
        for name, description in UNIT_SUITES.items():
            tests.append(
                {
                    "id": f"{preset}:{name}",
                    "name": name,
                    "kind": "unit",
                    "preset": preset,
                    "description": description,
                    "timeout_s": UNIT_TIMEOUT_S,
                    "needs_confirm": False,
                    "command": [
                        "bash",
                        "-lc",
                        f"cmake --preset {preset} >/dev/null "
                        f"&& cmake --build --preset {preset} "
                        f"&& ctest --preset {preset} -R '^{name}$'",
                    ],
                }
            )
    tests.extend(SPECIAL_TESTS)
    return tests


CATALOG = catalog()
BY_ID = {t["id"]: t for t in CATALOG}

# ------------------------------------------------------------- run history ---

_runs_lock = threading.Lock()
_runs: dict[str, dict[str, Any]] = {}  # id -> run record (also persisted)
_next_id = 1


def _load_history() -> None:
    global _next_id
    meta = RUNS_DIR / "runs.json"
    if meta.exists():
        try:
            history = json.loads(meta.read_text())
            for run in history:
                _runs[run["id"]] = run
                try:
                    _next_id = max(_next_id, int(run["id"]) + 1)
                except ValueError:
                    pass
        except (json.JSONDecodeError, KeyError):
            pass


def _persist_history() -> None:
    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    keep = sorted(_runs.values(), key=lambda r: r.get("started_at", 0))[-200:]
    (RUNS_DIR / "runs.json").write_text(json.dumps(keep, indent=1))


def _parse_summary(text: str) -> str | None:
    tail = text[-4000:]
    for pattern in (
        r"== (?:black-box client|live \S+ suite): (\d+) passed, (\d+) failed ==",
        r"(\d+)% tests passed, (\d+) tests failed out of (\d+)",
    ):
        m = re.search(pattern, tail)
        if m:
            return m.group(0).strip()
    return None


def _worker() -> None:
    while True:
        job = _queue.get()
        if job is None:
            break
        run_id, test = job
        log_path = RUNS_DIR / f"{run_id}.log"
        summary = None
        status = "failed"
        try:
            RUNS_DIR.mkdir(parents=True, exist_ok=True)
            with log_path.open("wb") as log:
                proc = subprocess.Popen(  # noqa: S603 - fixed commands from the catalog
                    test["command"],
                    cwd=str(ROOT),
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                with _runs_lock:
                    _runs[run_id]["pid"] = proc.pid
                    _runs[run_id]["status"] = "running"
                    _persist_history()
                try:
                    proc.wait(timeout=test["timeout_s"])
                    status = "passed" if proc.returncode == 0 else "failed"
                except subprocess.TimeoutExpired:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                    proc.wait()
                    status = "timeout"
            summary = _parse_summary(log_path.read_text(errors="replace"))
        except Exception as exc:  # pragma: no cover - defensive
            status = "failed"
            summary = f"launcher error: {exc}"
        finally:
            with _runs_lock:
                _runs[run_id]["status"] = status
                _runs[run_id]["finished_at"] = time.time()
                _runs[run_id]["summary"] = summary
                _persist_history()
            _queue.task_done()


_queue: Any = None
_worker_started = False


def _ensure_worker() -> None:
    global _queue, _worker_started
    if not _worker_started:
        _queue = queue.Queue()
        threading.Thread(target=_worker, daemon=True).start()
        _worker_started = True


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


@app.get("/api/tests")
def tests() -> JSONResponse:
    return JSONResponse({"tests": CATALOG})


@app.post("/api/tests/{test_id}/run")
def run_test(test_id: str) -> JSONResponse:
    test = BY_ID.get(test_id)
    if test is None:
        raise HTTPException(status_code=404, detail="unknown test id")
    if test["kind"] == "live" and not (ROOT / "config" / "gateway.json.secret").exists():
        raise HTTPException(status_code=400, detail="config/gateway.json.secret missing")
    _ensure_worker()
    global _next_id
    with _runs_lock:
        run_id = str(_next_id)
        _next_id += 1
        _runs[run_id] = {
            "id": run_id,
            "test_id": test_id,
            "test_name": test["name"],
            "kind": test["kind"],
            "preset": test.get("preset"),
            "status": "queued",
            "started_at": time.time(),
            "finished_at": None,
            "summary": None,
        }
        _persist_history()
    _queue.put((run_id, test))
    return JSONResponse({"run_id": run_id})


@app.get("/api/runs")
def runs() -> JSONResponse:
    with _runs_lock:
        items = sorted(_runs.values(), key=lambda r: r["id"], reverse=True)
    return JSONResponse({"runs": items})


@app.get("/api/runs/{run_id}/log")
def run_log(run_id: str, tail: int = 8000) -> JSONResponse:
    log_path = RUNS_DIR / f"{run_id}.log"
    if not log_path.exists():
        raise HTTPException(status_code=404, detail="no log")
    text = log_path.read_text(errors="replace")
    return JSONResponse({"log": text[-tail:]})


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


# ------------------------------------------------------------- api playground ---

_PROXY_METHODS = {"GET", "POST", "PUT", "DELETE"}


class _ProxyRequest(BaseModel):
    method: str
    path: str
    body: str | None = None


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
    if not path.startswith(("/orders", "/health", "/price")):
        raise HTTPException(status_code=400, detail="path must target /orders, /price or /health")

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


_load_history()
