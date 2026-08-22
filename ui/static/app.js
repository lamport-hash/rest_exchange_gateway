"use strict";

const state = {
  tests: [],
  runsByTest: {},   // test_id -> latest run record
  openLogs: new Set(), // test ids whose log row is expanded
  orders: [],
  lastOrdersOk: false,
};

const $ = (sel) => document.querySelector(sel);

async function fetchJson(url, opts) {
  const res = await fetch(url, opts);
  return res.json();
}

/* ------------------------------------------------------------------- tabs -- */

function initTabs() {
  document.querySelectorAll("#tabs .tab").forEach((btn) => {
    btn.onclick = () => {
      document.querySelectorAll("#tabs .tab").forEach((b) => b.classList.remove("active"));
      document.querySelectorAll(".tab-panel").forEach((p) => p.classList.remove("active"));
      btn.classList.add("active");
      $(`#tab-${btn.dataset.tab}`).classList.add("active");
      if (btn.dataset.tab === "diagrams") renderDiagrams(); // lazy, once
    };
  });
}

/* ---------------------------------------------------------------- panels -- */

function fmtDuration(seconds) {
  if (seconds == null) return "";
  if (seconds < 60) return `${seconds.toFixed(1)}s`;
  const m = Math.floor(seconds / 60);
  return `${m}m${Math.round(seconds % 60)}s`;
}

function renderGateway(data) {
  const badge = $("#gateway-badge");
  if (data.connected) {
    badge.textContent = "gateway: connected";
    badge.className = "badge ok";
  } else {
    badge.textContent = "gateway: unreachable";
    badge.className = "badge bad";
  }
  const feed = $("#feed-badge");
  if (data.feed_ok === true) {
    feed.textContent = "feed: subscribed";
    feed.className = "badge ok";
  } else if (data.feed_ok === false) {
    feed.textContent = "feed: disconnected";
    feed.className = "badge bad";
  } else {
    feed.textContent = "feed: no events yet";
    feed.className = "badge unknown";
  }
  const h = data.health || {};
  $("#om-stats").textContent =
    `knownOrders ${h.knownOrders ?? "–"}  ·  reportsApplied ${h.reportsApplied ?? "–"}  ·  reportsStale ${h.reportsStale ?? "–"}`;
  $("#clock").textContent = new Date().toLocaleTimeString();

  const ticker = $("#event-ticker");
  ticker.textContent = (data.events || []).slice(-6).map((e) => {
    const parts = [e.event];
    if (e.clientOrderId) parts.push(e.clientOrderId);
    if (e.state) parts.push(e.state);
    if (e.event === "reconcile") {
      parts.push(`adopted:${e.adopted ?? 0} unresolved:${e.unresolved ?? 0}`);
    }
    return parts.join(" ");
  }).reverse().join("   |   ") || "–";
}

function renderOrders(data) {
  const tbody = $("#orders-table tbody");
  const orders = data.orders || [];
  state.orders = orders;
  state.lastOrdersOk = !data.error;
  $("#orders-empty").style.display = orders.length ? "none" : "block";
  $("#orders-count").textContent = orders.length ? `${orders.length} order(s)` : "";
  tbody.innerHTML = "";
  for (const o of orders) {
    const tr = document.createElement("tr");
    const cells = [
      o.clientOrderId, o.venue, o.symbol, o.side, o.type,
      o.price ?? "–", o.quantity ?? "–", o.filledQuantity ?? "0",
      o.averageFillPrice || "–", o.exchangeOrderId || "–",
    ];
    for (const c of cells) {
      const td = document.createElement("td");
      td.textContent = c;
      tr.appendChild(td);
    }
    const stateTd = document.createElement("td");
    const span = document.createElement("span");
    span.className = `state ${o.state}`;
    span.textContent = o.state;
    stateTd.appendChild(span);
    tr.appendChild(stateTd);
    tbody.appendChild(tr);
  }
  renderStateChart();
}

/* ------------------------------------------------------ state visualization -- */

const STATES = ["live", "partially_filled", "filled", "canceled", "rejected"];

function renderStateChart() {
  const chart = $("#state-chart");
  if (!chart) return;
  const counts = Object.fromEntries(STATES.map((s) => [s, 0]));
  let total = 0;
  for (const o of state.orders) {
    if (counts[o.state] != null) counts[o.state] += 1;
    total += 1;
  }
  $("#states-empty").style.display = total ? "none" : "block";
  $("#states-total").textContent = total ? `${total} order(s)` : "";
  chart.innerHTML = "";
  for (const s of STATES) {
    const n = counts[s];
    const frac = total ? (100 * n) / total : 0;
    const row = document.createElement("div");
    row.className = "chart-row";
    row.innerHTML =
      `<span class="chart-label"><span class="state ${s}">${s}</span></span>` +
      `<div class="chart-bar"><div class="chart-fill ${s}" style="width:${frac}%"></div></div>` +
      `<span class="chart-value mono">${n}</span>`;
    chart.appendChild(row);
  }
}

/* ------------------------------------------------------------ api playground -- */

const ENDPOINTS = [
  {
    id: "place", label: "POST /orders — place order", method: "POST", path: "/orders",
    description: "Place a limit or market order. clientOrderId is the idempotency key: retrying a known id replays its recorded outcome verbatim.",
    fields: [
      ["clientOrderId", "required", "1–32 chars, [A-Za-z0-9] only — idempotency key"],
      ["venue", "optional", "OKX | BINANCE (case-insensitive); default from config"],
      ["symbol", "required", "gateway spelling everywhere: BTC-USDT"],
      ["side", "required", "buy | sell"],
      ["type", "required", "limit | market"],
      ["price", "conditional", "required for limit, forbidden for market; plain decimal"],
      ["quantity", "required", "plain decimal"],
      ["timeInForce", "optional", "GTC | IOC | FOK — limit orders only"],
    ],
    body: JSON.stringify({
      clientOrderId: "ui0001", venue: "OKX", symbol: "BTC-USDT",
      side: "buy", type: "limit", price: "30000", quantity: "0.01", timeInForce: "GTC",
    }, null, 2),
    example: `201 {"clientOrderId":"ui0001","exchangeOrderId":"12569099453","symbol":"BTC-USDT",
     "venue":"OKX","state":"live","replayed":false}`,
  },
  {
    id: "list", label: "GET /orders — list", method: "GET", path: "/orders",
    description: "Full registry snapshot sorted by clientOrderId (no pagination yet).",
    fields: [],
    body: null,
    example: `200 {"orders":[ { …order record… }, … ]}`,
  },
  {
    id: "status", label: "GET /orders/{id} — status", method: "GET", path: "/orders/ui0001",
    description: "Served from the local registry (WS-fed + reconcile) — no venue round-trip.",
    fields: [],
    body: null,
    example: `200 {"clientOrderId":"ui0001","state":"partially_filled","filledQuantity":"0.04", …}`,
  },
  {
    id: "amend", label: "PUT /orders/{id} — amend", method: "PUT", path: "/orders/ui0001",
    description: "Amend price and/or quantity (null = unchanged; at least one required). Risk checks re-run. Binance emulates with cancel+replace; clientOrderId stays stable.",
    fields: [
      ["price", "optional", "new price — plain decimal"],
      ["quantity", "optional", "new quantity — plain decimal"],
    ],
    body: JSON.stringify({ price: "30100", quantity: "0.02" }, null, 2),
    example: `200 {"clientOrderId":"ui0001","exchangeOrderId":"…","state":"live","price":"30100","quantity":"0.02"}`,
  },
  {
    id: "cancel", label: "DELETE /orders/{id} — cancel", method: "DELETE", path: "/orders/ui0001",
    description: "Idempotent: canceling an already-canceled order returns 200 again without a venue call.",
    fields: [],
    body: null,
    example: `200 {"clientOrderId":"ui0001","state":"canceled", …}`,
  },
  {
    id: "health", label: "GET /health — liveness", method: "GET", path: "/health",
    description: "Registry stats; reportsStale counts duplicate/out-of-order execution reports safely discarded.",
    fields: [],
    body: null,
    example: `200 {"status":"ok","knownOrders":3,"reportsApplied":12,"reportsStale":2}`,
  },
];

function highlightJson(text) {
  const esc = text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  // single pass: string | literal | number — whichever group matched wins,
  // so numbers inside strings stay untouched (no lookbehind: older Safari)
  return esc.replace(
    /("(?:[^"\\]|\\.)*")|\b(true|false|null)\b|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)/g,
    (_m, str, lit, num) => {
      if (str) return `<span class="j-key">${str}</span>`;
      if (lit) return `<span class="j-lit">${lit}</span>`;
      return `<span class="j-num">${num}</span>`;
    },
  );
}

function renderEndpoints() {
  const select = $("#pg-endpoint");
  for (const ep of ENDPOINTS) {
    const opt = document.createElement("option");
    opt.value = ep.id;
    opt.textContent = ep.label;
    select.appendChild(opt);
  }
  const applyEndpoint = () => {
    const ep = ENDPOINTS.find((e) => e.id === select.value);
    $("#pg-method").value = ep.method;
    $("#pg-path").value = ep.path;
    $("#pg-body").value = ep.body ?? "";
    $("#pg-body").disabled = !ep.body && (ep.method === "GET" || ep.method === "DELETE");
    $("#pg-example").textContent = ep.example;
  };
  select.onchange = applyEndpoint;
  applyEndpoint();

  $("#pg-send").onclick = async () => {
    const statusEl = $("#pg-status");
    const pre = $("#pg-response");
    statusEl.textContent = "…";
    statusEl.className = "badge unknown";
    $("#pg-latency").textContent = "";
    try {
      const data = await fetchJson("/api/proxy", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          method: $("#pg-method").value,
          path: $("#pg-path").value,
          body: $("#pg-body").value || null,
        }),
      });
      if (!data.ok) {
        statusEl.textContent = "unreachable";
        statusEl.className = "badge bad";
        $("#pg-latency").textContent = `${data.latency_ms} ms`;
        pre.textContent = data.error;
        return;
      }
      statusEl.textContent = `HTTP ${data.status}`;
      statusEl.className = `badge ${data.status < 300 ? "ok" : data.status < 500 ? "warn" : "bad"}`;
      $("#pg-latency").textContent = `${data.latency_ms} ms`;
      const text = typeof data.body === "string" ? data.body : JSON.stringify(data.body, null, 2);
      pre.innerHTML = highlightJson(text);
    } catch (e) {
      statusEl.textContent = "error";
      statusEl.className = "badge bad";
      pre.textContent = String(e);
    }
  };

  const cards = $("#endpoint-cards");
  for (const ep of ENDPOINTS) {
    const card = document.createElement("div");
    card.className = "endpoint-card";
    const head = document.createElement("div");
    head.className = "endpoint-head";
    const chip = document.createElement("span");
    chip.className = `http-chip ${ep.method}`;
    chip.textContent = ep.method;
    const pathEl = document.createElement("span");
    pathEl.className = "mono";
    pathEl.textContent = " " + ep.path.replace("ui0001", "{clientOrderId}");
    head.appendChild(chip);
    head.appendChild(pathEl);
    card.appendChild(head);
    const desc = document.createElement("div");
    desc.className = "muted endpoint-desc";
    desc.textContent = ep.description;
    card.appendChild(desc);
    if (ep.fields.length) {
      const table = document.createElement("table");
      table.className = "fields";
      table.innerHTML = "<thead><tr><th>field</th><th>req/opt</th><th>rules</th></tr></thead>";
      const tb = document.createElement("tbody");
      for (const [f, r, note] of ep.fields) {
        const tr = document.createElement("tr");
        tr.innerHTML =
          `<td class="mono">${f}</td><td>${r}</td><td class="muted">${note}</td>`;
        tb.appendChild(tr);
      }
      table.appendChild(tb);
      card.appendChild(table);
    }
    const ex = document.createElement("pre");
    ex.className = "json";
    ex.textContent = ep.example;
    card.appendChild(ex);
    cards.appendChild(card);
  }
}

/* --------------------------------------------------------------- diagrams -- */

let diagramsDone = false;

async function renderDiagrams() {
  if (diagramsDone) return;
  diagramsDone = true;
  const nodes = document.querySelectorAll(".mermaid-src");
  if (typeof window.mermaid === "undefined") {
    // vendored mermaid.min.js missing — the raw sources stay visible as <pre>
    nodes.forEach((n) => n.classList.add("mermaid-fallback"));
    return;
  }
  try {
    window.mermaid.initialize({ startOnLoad: false, theme: "dark", securityLevel: "strict" });
    for (const node of nodes) {
      const { svg } = await window.mermaid.render(`m-${node.id}`, node.textContent);
      const holder = document.createElement("div");
      holder.className = "diagram";
      holder.innerHTML = svg;
      node.replaceWith(holder);
    }
  } catch {
    nodes.forEach((n) => n.classList.add("mermaid-fallback"));
  }
}

/* ----------------------------------------------------------------- tests -- */

function runClass(run) {
  if (!run) return "";
  if (run.status === "passed" || run.status === "failed") return run.status;
  return run.status; // queued | running | timeout
}

function renderTests() {
  const tbody = $("#tests-table tbody");
  tbody.innerHTML = "";
  const groups = [
    { key: "unit-debug", label: "unit tests — debug preset (ASan+UBSan)", match: (t) => t.kind === "unit" && t.preset === "debug" },
    { key: "unit-release", label: "unit tests — release preset", match: (t) => t.kind === "unit" && t.preset === "release" },
    { key: "blackbox", label: "black-box (mock venue, deterministic)", match: (t) => t.kind === "blackbox" },
    { key: "live", label: "live venues (real demo/testnet funds — confirm required)", match: (t) => t.kind === "live" },
  ];
  for (const group of groups) {
    const tests = state.tests.filter(group.match);
    if (!tests.length) continue;
    const head = document.createElement("tr");
    head.className = "kind-group";
    head.innerHTML = `<td colspan="8">${group.label}</td>`;
    tbody.appendChild(head);
    for (const t of tests) {
      tbody.appendChild(renderTestRow(t));
      if (state.openLogs.has(t.id)) {
        tbody.appendChild(renderLogRow(t));
      }
    }
  }
}

function renderTestRow(t) {
  const run = state.runsByTest[t.id];
  const tr = document.createElement("tr");
  tr.dataset.testId = t.id;

  const runTd = document.createElement("td");
  const btn = document.createElement("button");
  btn.className = "run";
  btn.textContent = "▶ run";
  const busy = run && (run.status === "running" || run.status === "queued");
  btn.disabled = Boolean(busy);
  btn.onclick = () => launch(t);
  runTd.appendChild(btn);
  tr.appendChild(runTd);

  const nameTd = document.createElement("td");
  nameTd.className = "mono";
  nameTd.textContent = t.name;
  tr.appendChild(nameTd);

  const presetTd = document.createElement("td");
  presetTd.textContent = t.preset || "–";
  tr.appendChild(presetTd);

  const kindTd = document.createElement("td");
  kindTd.textContent = t.kind;
  tr.appendChild(kindTd);

  const descTd = document.createElement("td");
  descTd.className = "muted";
  descTd.textContent = t.description;
  tr.appendChild(descTd);

  const outTd = document.createElement("td");
  outTd.className = "col-outcome";
  if (run) {
    const span = document.createElement("span");
    span.className = `outcome ${runClass(run)}`;
    span.textContent = run.status;
    outTd.appendChild(span);
    if (run.summary) {
      const s = document.createElement("div");
      s.className = "summary";
      s.textContent = run.summary;
      outTd.appendChild(s);
    }
  } else {
    outTd.innerHTML = '<span class="muted">never run</span>';
  }
  tr.appendChild(outTd);

  const durTd = document.createElement("td");
  if (run && run.finished_at) {
    durTd.textContent = fmtDuration(run.finished_at - run.started_at);
  } else if (run && run.status === "running") {
    durTd.textContent = fmtDuration((Date.now() / 1000) - run.started_at) + "…";
  }
  tr.appendChild(durTd);

  const logTd = document.createElement("td");
  const logBtn = document.createElement("button");
  logBtn.textContent = state.openLogs.has(t.id) ? "hide" : "log";
  logBtn.disabled = !run;
  logBtn.onclick = () => {
    if (state.openLogs.has(t.id)) state.openLogs.delete(t.id);
    else state.openLogs.add(t.id);
    renderTests();
  };
  logTd.appendChild(logBtn);
  tr.appendChild(logTd);

  return tr;
}

function renderLogRow(t) {
  const tr = document.createElement("tr");
  tr.className = "log-row";
  const td = document.createElement("td");
  td.colSpan = 8;
  const pre = document.createElement("pre");
  pre.className = "log";
  pre.dataset.testId = t.id;
  pre.textContent = "loading…";
  td.appendChild(pre);
  tr.appendChild(td);
  refreshLog(t.id, pre);
  return tr;
}

async function refreshLog(testId, pre) {
  const run = state.runsByTest[testId];
  if (!run) return;
  try {
    const data = await fetchJson(`/api/runs/${run.id}/log?tail=12000`);
    pre.textContent = data.log || "(empty)";
    pre.scrollTop = pre.scrollHeight;
  } catch {
    pre.textContent = "(no log yet)";
  }
}

async function launch(t) {
  if (t.needs_confirm) {
    const ok = confirm(
      `${t.name}\n\nThis runs against the REAL venue and spends small demo/testnet funds.\n` +
      "It needs internet access and valid credentials in config/gateway.json.secret.\n\nProceed?"
    );
    if (!ok) return;
  }
  try {
    const data = await fetchJson(`/api/tests/${encodeURIComponent(t.id)}/run`, { method: "POST" });
    if (data.run_id) {
      state.runsByTest[t.id] = {
        id: data.run_id, test_id: t.id, status: "queued",
        started_at: Date.now() / 1000, finished_at: null, summary: null,
      };
      renderTests();
    } else if (data.detail) {
      alert(`cannot start: ${data.detail}`);
    }
  } catch (e) {
    alert(`cannot start: ${e}`);
  }
}

/* ---------------------------------------------------------------- refresh -- */

async function refreshRuns() {
  const data = await fetchJson("/api/runs");
  const byTest = {};
  for (const run of data.runs || []) {
    const prev = byTest[run.test_id];
    if (!prev || Number(run.id) > Number(prev.id)) byTest[run.test_id] = run;
  }
  state.runsByTest = byTest;
  renderTests();
  // keep live logs scrolling while their test runs
  document.querySelectorAll("pre.log").forEach((pre) => {
    const run = state.runsByTest[pre.dataset.testId];
    if (run && (run.status === "running" || run.status === "queued")) {
      refreshLog(pre.dataset.testId, pre);
    }
  });
}

async function refreshAll() {
  try { renderGateway(await fetchJson("/api/gateway/status")); } catch { /* gateway down */ }
  try { renderOrders(await fetchJson("/api/orders")); } catch { /* ignore */ }
  try { await refreshRuns(); } catch { /* ignore */ }
}

async function init() {
  initTabs();
  renderEndpoints();
  const data = await fetchJson("/api/tests");
  state.tests = data.tests || [];
  document.querySelectorAll(".runall button").forEach((btn) => {
    btn.onclick = async () => {
      const group = btn.dataset.group;
      const targets = state.tests.filter((t) =>
        group === "blackbox" ? t.kind === "blackbox" : t.preset === group);
      for (const t of targets) {
        if (t.needs_confirm) continue; // live suites stay explicit
        await fetchJson(`/api/tests/${encodeURIComponent(t.id)}/run`, { method: "POST" });
      }
      await refreshRuns();
    };
  });
  await refreshAll();
  setInterval(refreshAll, 2000);
}

init();
