"use strict";

const state = {
  tests: [],
  runsByTest: {},   // test_id -> latest run record
  openLogs: new Set(), // test ids whose log row is expanded
};

const $ = (sel) => document.querySelector(sel);

async function fetchJson(url, opts) {
  const res = await fetch(url, opts);
  return res.json();
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

let logRefreshTimer = null;

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
