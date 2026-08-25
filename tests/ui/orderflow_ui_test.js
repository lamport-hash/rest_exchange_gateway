"use strict";
// jsdom harness for the Order flow tab (spec 3.1 traceability UI).
//
// Loads the real index.html + app.js from ui/static/ into jsdom, stubs
// fetch with scripted /api/tests, /api/runs and /api/spec/order-flow
// payloads, then asserts on the rendered DOM. The spec matrix and the
// test catalog are imported from ui/app.py itself (python3, single
// source of truth) — the suite fails if the matrix references a suite
// missing from the catalog or if the rendering logic drifts.
//
// Run:  cd tests/ui && npm install && npm test
// (or:  node orderflow_ui_test.js)

const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");
const { JSDOM } = require("jsdom");

const ROOT = path.resolve(__dirname, "..", "..");
const html = fs.readFileSync(path.join(ROOT, "ui", "static", "index.html"), "utf8");
const appJs = fs.readFileSync(path.join(ROOT, "ui", "static", "app.js"), "utf8");

// ---- real backend data: ORDER_FLOW_SPEC + CATALOG from ui/app.py ----
const appPy = path.join(ROOT, "ui", "app.py");
const dump = spawnSync(
  "python3",
  ["-c", [
    "import importlib.util, json, sys",
    `spec = importlib.util.spec_from_file_location("uiapp", ${JSON.stringify(appPy)})`,
    "mod = importlib.util.module_from_spec(spec)",
    "spec.loader.exec_module(mod)",
    'json.dump({"spec": mod.ORDER_FLOW_SPEC, "tests": mod.CATALOG}, sys.stdout)',
  ].join("\n")],
  { encoding: "utf8" },
);
if (dump.status !== 0) {
  console.error(`cannot load ui/app.py data (python3 + fastapi required):\n${dump.stderr}`);
  process.exit(1);
}
const backend = JSON.parse(dump.stdout);
const specPayload = backend.spec;
const TESTS = backend.tests;

// ---- deterministic mock run history (exercises every status branch) ----
// oms both presets passed, blackbox failed, order_state passed,
// okx_connector running -> requirement rows land on covered / failing /
// running / not run respectively.
const RUNS = [
  { id: "3", test_id: "debug:oms_test", status: "passed" },
  { id: "4", test_id: "release:oms_test", status: "passed" },
  { id: "5", test_id: "blackbox", status: "failed" },
  { id: "6", test_id: "debug:order_state_test", status: "passed" },
  { id: "7", test_id: "debug:okx_connector_test", status: "running" },
];

// ---- catalog cross-check: every matrix suite must exist in the catalog ----
const unitNames = new Set(TESTS.filter((t) => t.kind === "unit").map((t) => t.name));
const specialIds = new Set(TESTS.filter((t) => t.kind !== "unit").map((t) => t.id));
const unknown = [...new Set(
  specPayload.requirements.flatMap((r) => r.tests),
)].filter((s) => !unitNames.has(s) && !specialIds.has(s));

const dom = new JSDOM(html, { url: "http://ui/", pretendToBeVisual: true, runScripts: "outside-only" });
const { window } = dom;
window.alert = () => {};
window.confirm = () => false;
window.fetch = async (url) => ({
  json: async () => {
    if (url === "/api/tests") return { tests: TESTS };
    if (url === "/api/runs") return { runs: RUNS };
    if (url === "/api/spec/order-flow") return specPayload;
    if (url === "/api/gateway/status") return { connected: false, health: {}, events: [] };
    if (url === "/api/orders") return { orders: [] };
    if (url.startsWith("/api/risk") || url.startsWith("/api/price")) return { error: "n/a" };
    throw new Error(`unexpected fetch ${url}`);
  },
});
window.mermaid = undefined; // the diagrams lazy hook only fires on click

let failed = 0;
const check = (name, cond) => {
  console.log(`${cond ? "PASS" : "FAIL"}: ${name}`);
  if (!cond) failed += 1;
};

window.eval(appJs);

setTimeout(() => {
  const $ = (sel) => window.document.querySelector(sel);

  check("matrix references only catalog suites", unknown.length === 0);

  // tab wiring
  const btn = window.document.querySelector('#tabs .tab[data-tab="orderflow"]');
  check("nav button exists", btn && btn.textContent === "Order flow");
  check("panel exists", $("#tab-orderflow") != null);
  btn.click();
  check("tab activates its panel", $("#tab-orderflow").classList.contains("active"));

  // requirement rows + run-history join
  const rows = [...window.document.querySelectorAll("#orderflow-table tbody tr")];
  check(`${specPayload.requirements.length} requirement rows`,
        rows.length === specPayload.requirements.length);
  const byFirstCell = (label) => rows.find((r) => r.querySelector("td").textContent === label);

  const newLimit = byFirstCell("New order — Limit");
  check("new-limit row present", newLimit != null);
  const chips = [...newLimit.querySelectorAll(".suite-chip")];
  // 4 unit suites x2 presets + blackbox + live_okx + live_binance
  check("new-limit has 11 suite chips", chips.length === 11);
  check("passed preset chip marked",
        chips.some((c) => c.textContent.includes("release") && c.classList.contains("passed")));
  check("failed blackbox chip marked",
        chips.some((c) => c.textContent === "blackbox ✗" && c.classList.contains("failed")));
  check("unrun live chip muted",
        chips.some((c) => c.textContent === "live_okx" && !c.classList.contains("passed")));
  check("new-limit failing (blackbox failed)",
        newLimit.querySelector(".badge").textContent === "failing");

  const newMarket = byFirstCell("New order — Market");
  check("new-market not run (no covering suite has a run)",
        newMarket.querySelector(".badge").textContent === "not run");

  const stateLive = byFirstCell("Normalized state: New / Live");
  check("state-live covered (all executed suites passed)",
        stateLive.querySelector(".badge").textContent === "covered");

  const statePending = byFirstCell("Normalized state: Pending (gateway-local)");
  check("state-pending row present + covered (order_state/oms passed)",
        statePending != null && statePending.querySelector(".badge").textContent === "covered");
  check("state-pending impl refs cite the born-Pending staging",
        statePending.textContent.includes("place_submitted") &&
        statePending.textContent.includes("BEFORE the venue call"));

  const explicit = byFirstCell("Client <-> exchange semantics mapping is explicit");
  check("explicit-mapping running… (okx_connector_test running, none failed)",
        explicit.querySelector(".badge").textContent === "running…");

  const rollup = $("#orderflow-status");
  check("rollup badge counts the 3 failing rows",
        rollup.textContent === "3 requirement(s) failing" && rollup.className.includes("bad"));

  // implementation refs rendered with file:line
  check("impl ref rendered",
        newLimit.querySelector(".impl-list .mono").textContent === "src/core/oms.cpp:278");

  // explicit mapping tables
  const flowRows = [...window.document.querySelectorAll("#flowmap-table tbody tr")];
  check(`${specPayload.flow_mapping.length} flow-mapping rows`,
        flowRows.length === specPayload.flow_mapping.length);
  check("flow amend row mentions cancelReplace",
        flowRows.some((r) => r.textContent.includes("cancelReplace")));
  const stateRows = [...window.document.querySelectorAll("#statemap-table tbody tr")];
  check(`${specPayload.state_mapping.length} state-mapping rows`,
        stateRows.length === specPayload.state_mapping.length);
  check("canceled row lists EXPIRED_IN_MATCH",
        stateRows.some((r) => r.textContent.includes("EXPIRED_IN_MATCH")));
  check("pending row first — gateway-local, never on the wire",
        stateRows[0].querySelector(".state.pending") != null &&
        stateRows[0].textContent.includes("gateway-local") &&
        stateRows[0].textContent.includes("never carry it"));
  check("state chips reuse .state classes", stateRows[1].querySelector(".state.live") != null);
  check("mapping note set", $("#statemap-note").textContent.includes("nullopt"));

  console.log(failed ? `\n${failed} check(s) FAILED` : "\nall checks passed");
  process.exit(failed ? 1 : 0);
}, 800);
