"use strict";

const state = {
  orders: [],
  lastOrdersOk: false,
  priceSymbol: "BTC-USDT",
  priceVenue: "",
  orderFlow: null,  // cached /api/spec/order-flow matrix
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

/* ------------------------------------------------------------- price feed -- */

async function refreshPrice() {
  const line = $("#price-line");
  if (!line) return;
  const params = new URLSearchParams({ symbol: state.priceSymbol });
  if (state.priceVenue) params.set("venue", state.priceVenue);
  try {
    const data = await fetchJson(`/api/price?${params}`);
    if (data.error) {
      line.textContent = data.error;
      line.className = "mono bad";
      return;
    }
    line.textContent = `${data.symbol} @ ${data.venue}: ${data.price}`;
    line.className = "mono ok";
  } catch {
    line.textContent = "–";
    line.className = "mono muted";
  }
}

function initPriceControls() {
  const symbol = $("#price-symbol");
  const venue = $("#price-venue");
  if (!symbol || !venue) return;
  symbol.oninput = () => {
    state.priceSymbol = symbol.value.trim() || "BTC-USDT";
    refreshPrice();
  };
  venue.onchange = () => {
    state.priceVenue = venue.value;
    refreshPrice();
  };
}

/* ---------------------------------------------------------------- panels -- */

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
  renderRejections();
}

/* ------------------------------------------------------ state visualization -- */

const STATES = ["pending", "live", "partially_filled", "filled", "canceled", "rejected"];

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
  {
    id: "price", label: "GET /price/{symbol} — last price", method: "GET", path: "/price/BTC-USDT",
    description: "Last-traded price of a pair (public venue market data). Optional ?venue= routes; default venue when absent.",
    fields: [
      ["venue (query)", "optional", "OKX | BINANCE via ?venue=…; default from config"],
    ],
    body: null,
    example: `200 {"symbol":"BTC-USDT","venue":"OKX","price":"61750.5"}`,
  },
  {
    id: "risk", label: "GET /risk — active pre-trade limits", method: "GET", path: "/risk",
    description: "Read-only view of the pre-trade risk limits, fixed by the config file at startup. " +
      "An instrument entry replaces the defaults wholesale; an empty string disables that check for the scope. " +
      "Rejected orders carry their reason in GET /orders records as rejection:{code,reason}.",
    fields: [],
    body: null,
    example: `200 {"default":{"maxQty":"10","maxNotional":"1000000","maxPosition":"10"},
     "instruments":{"BTC-USDT":{"maxQty":"5","maxNotional":"","maxPosition":"2"}}}`,
  },
  {
    id: "okx-demo-balance", label: "POST /venue/okx/demo-adjust-balance — OKX demo balance",
    method: "POST", path: "/venue/okx/demo-adjust-balance",
    description: "Venue-specific passthrough to OKX POST /api/v5/account/demo-adjust-balance " +
      "(demo trading accounts only; 400 when okx.demoTrading is off). Currencies BTC/ETH/USDT/OKB; " +
      "increase quota 3/day (UTC). One signed request, never retried.",
    fields: [
      ["type", "required", "increase | reduce (venue spelling)"],
      ["adjustments", "required", "non-empty array of {ccy, amt}; duplicate ccys rejected; amt plain decimal"],
    ],
    body: JSON.stringify({ type: "increase", adjustments: [{ ccy: "USDT", amt: "5000" }] }, null, 2),
    example: `200 {"venue":"okx","type":"increase","adjustments":[{"ccy":"USDT","amt":"5000"}],"data":[…]}`,
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

/* -------------------------------------------------------- okx demo balance -- */

function initBalanceControls() {
  const send = $("#bal-send");
  if (!send) return;
  send.onclick = async () => {
    const statusEl = $("#bal-status");
    const pre = $("#bal-response");
    statusEl.textContent = "…";
    statusEl.className = "badge unknown";
    $("#bal-latency").textContent = "";
    try {
      const data = await fetchJson("/api/okx/demo-balance", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          type: $("#bal-type").value,
          ccy: $("#bal-ccy").value.trim(),
          amt: $("#bal-amt").value.trim(),
        }),
      });
      if (!data.ok && data.status == null) {
        statusEl.textContent = "unreachable";
        statusEl.className = "badge bad";
        $("#bal-latency").textContent = `${data.latency_ms} ms`;
        pre.textContent = data.error;
        return;
      }
      statusEl.textContent = `HTTP ${data.status}`;
      statusEl.className = `badge ${data.status < 300 ? "ok" : data.status < 500 ? "warn" : "bad"}`;
      $("#bal-latency").textContent = `${data.latency_ms} ms`;
      const text = typeof data.body === "string" ? data.body : JSON.stringify(data.body, null, 2);
      pre.innerHTML = highlightJson(text);
    } catch (e) {
      statusEl.textContent = "error";
      statusEl.className = "badge bad";
      pre.textContent = String(e);
    }
  };
}

/* ------------------------------------------------------------------- risk -- */

function renderRisk(data) {
  const tbody = $("#risk-table tbody");
  if (!tbody) return;
  const badge = $("#risk-status");
  tbody.innerHTML = "";
  if (data.error) {
    badge.textContent = "unreachable";
    badge.className = "badge bad";
    $("#risk-empty").style.display = "none";
    return;
  }
  const def = data.default;
  const instruments = data.instruments || {};
  const symbols = Object.keys(instruments).sort();
  const noneConfigured = !def && symbols.length === 0;
  $("#risk-empty").style.display = noneConfigured ? "block" : "none";
  badge.textContent = noneConfigured ? "checks disabled" : "enforced";
  badge.className = `badge ${noneConfigured ? "warn" : "ok"}`;

  const limitCell = (value) => {
    const td = document.createElement("td");
    td.textContent = value || "—";
    td.className = value ? "mono" : "muted";
    return td;
  };
  const rows = [];
  if (def) rows.push(["default (every instrument)", def]);
  for (const s of symbols) rows.push([s, instruments[s]]);
  for (const [scope, limits] of rows) {
    const tr = document.createElement("tr");
    const scopeTd = document.createElement("td");
    scopeTd.textContent = scope;
    tr.appendChild(scopeTd);
    tr.appendChild(limitCell(limits.maxQty));
    tr.appendChild(limitCell(limits.maxNotional));
    tr.appendChild(limitCell(limits.maxPosition));
    tbody.appendChild(tr);
  }
}

function renderRejections() {
  const tbody = $("#rejections-table tbody");
  if (!tbody) return;
  const rejected = state.orders.filter(
    (o) => o.state === "rejected" && o.rejection && String(o.rejection.code).startsWith("risk_"));
  tbody.innerHTML = "";
  $("#rejections-empty").style.display = rejected.length ? "none" : "block";
  $("#rejections-count").textContent = rejected.length ? `${rejected.length} risk-rejected order(s)` : "";
  for (const o of rejected) {
    const tr = document.createElement("tr");
    for (const c of [o.clientOrderId, o.venue, o.symbol, o.side, o.quantity]) {
      const td = document.createElement("td");
      td.textContent = c;
      tr.appendChild(td);
    }
    const codeTd = document.createElement("td");
    const chip = document.createElement("span");
    chip.className = "state rejected";
    chip.textContent = o.rejection.code;
    codeTd.appendChild(chip);
    tr.appendChild(codeTd);
    const reasonTd = document.createElement("td");
    reasonTd.className = "muted";
    reasonTd.textContent = o.rejection.reason;
    tr.appendChild(reasonTd);
    tbody.appendChild(tr);
  }
}

/* ------------------------------------------------------------- order flow -- */

function renderOrderFlow() {
  const tbody = $("#orderflow-table tbody");
  if (!tbody || !state.orderFlow || state.orderFlow.error) return;
  const spec = state.orderFlow;
  tbody.innerHTML = "";
  for (const req of spec.requirements) {
    const tr = document.createElement("tr");

    const reqTd = document.createElement("td");
    reqTd.textContent = req.requirement;
    tr.appendChild(reqTd);

    const implTd = document.createElement("td");
    const implList = document.createElement("div");
    implList.className = "impl-list";
    for (const entry of req.implementation) {
      const line = document.createElement("div");
      const ref = document.createElement("span");
      ref.className = "mono";
      ref.textContent = entry.ref;
      line.appendChild(ref);
      const what = document.createElement("span");
      what.className = "muted";
      what.textContent = ` — ${entry.what}`;
      line.appendChild(what);
      implList.appendChild(line);
    }
    implTd.appendChild(implList);
    tr.appendChild(implTd);

    const testsTd = document.createElement("td");
    const chips = document.createElement("div");
    chips.className = "suite-chips";
    for (const name of req.tests) {
      const chip = document.createElement("span");
      chip.className = "suite-chip";
      chip.textContent = name;
      chips.appendChild(chip);
    }
    if (!req.tests.length) chips.textContent = "–";
    testsTd.appendChild(chips);
    tr.appendChild(testsTd);

    tbody.appendChild(tr);
  }

  const flowTbody = $("#flowmap-table tbody");
  flowTbody.innerHTML = "";
  for (const row of spec.flow_mapping) {
    const tr = document.createElement("tr");
    const flowTd = document.createElement("td");
    flowTd.textContent = row.flow;
    tr.appendChild(flowTd);
    for (const cell of [row.client, row.okx, row.binance]) {
      const td = document.createElement("td");
      td.className = "mono";
      td.textContent = cell;
      tr.appendChild(td);
    }
    flowTbody.appendChild(tr);
  }

  const stateTbody = $("#statemap-table tbody");
  stateTbody.innerHTML = "";
  for (const row of spec.state_mapping) {
    const tr = document.createElement("tr");
    const stateTd = document.createElement("td");
    const chip = document.createElement("span");
    chip.className = `state ${row.state}`;
    chip.textContent = row.state;
    stateTd.appendChild(chip);
    tr.appendChild(stateTd);
    const meaningTd = document.createElement("td");
    meaningTd.textContent = row.meaning;
    tr.appendChild(meaningTd);
    for (const cell of [row.okx, row.binance]) {
      const td = document.createElement("td");
      td.className = "mono";
      td.textContent = cell;
      tr.appendChild(td);
    }
    stateTbody.appendChild(tr);
  }
  $("#statemap-note").textContent = spec.mapping_note;
}

async function ensureOrderFlowSpec() {
  if (state.orderFlow) return;
  try {
    state.orderFlow = await fetchJson("/api/spec/order-flow");
  } catch {
    state.orderFlow = { error: "unavailable" };
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
  } catch {
    nodes.forEach((n) => n.classList.add("mermaid-fallback"));
    return;
  }
  for (const node of nodes) {
    const holder = document.createElement("div");
    holder.className = "diagram";
    try {
      node.after(holder);
      // Passing the holder as render container keeps mermaid's temp/error
      // elements inside it — a parse failure can never leak a "Syntax
      // error ... mermaid version" block into the page body.
      const { svg } = await window.mermaid.render(`m-${node.id}`, node.textContent, holder);
      holder.innerHTML = svg;
      node.remove();
    } catch {
      // keep the raw source visible instead of a broken/blank diagram
      holder.remove();
      node.classList.add("mermaid-fallback");
    }
  }
}

/* ---------------------------------------------------------------- refresh -- */

async function refreshAll() {
  try { renderGateway(await fetchJson("/api/gateway/status")); } catch { /* gateway down */ }
  try { renderOrders(await fetchJson("/api/orders")); } catch { /* ignore */ }
  try { renderRisk(await fetchJson("/api/risk")); } catch { /* ignore */ }
  try { await ensureOrderFlowSpec(); renderOrderFlow(); } catch { /* ignore */ }
  try { await refreshPrice(); } catch { /* ignore */ }
}

async function init() {
  initTabs();
  initPriceControls();
  initBalanceControls();
  renderEndpoints();
  await refreshAll();
  setInterval(refreshAll, 2000);
}

init();
