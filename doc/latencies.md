# Gateway Latency Measurements

Measured latency of the gateway's own processing windows (venue network
time excluded). Produced by `tests/latency_bench_test` — report-only,
asserts measurement plumbing, never speed.

## Measured windows

Three phases, logged by the OMS through `LatencyLog`
(`src/core/latency.hpp`) on the monotonic steady clock:

| Phase | Window | Contains |
|---|---|---|
| `place_send_rest` | REST `POST /orders` handler entry → just before the venue place call | JSON parse, field validation, venue resolution, pre-trade risk projection, Pending staging, (optional) `place_submitted` persist |
| `place_send_oms` | OMS `place()` entry → just before the venue place call | dedup lookup, pre-trade risk projection, Pending staging, (optional) persist |
| `fill_state_update` | execution report received (`on_execution_report`) → registry state updated and the `state` event persisted | leg-table arbitration, state-machine transition, fill high-water mark, persistence |

Not measured (by design): Crow's socket/HTTP wire parsing (before the
handler-entry stamp), the venue round trip itself, and the venue feed's
delivery of the report. The benchmark venue is an instant in-memory
connector, so windows contain gateway processing only.

## Results — release build, 2026-08-26

Environment: 4-core Linux, AMD EPYC-Rome, `build/release` (-O3, no
sanitizers), commit `55bf249`. 500 measured orders per variant (50 HTTP
warmup, 100 preloaded terminal registry orders so the risk projection
scans a representative registry; risk enabled with always-passing
limits). Values in **microseconds**; representative single run, stable
across repeated runs (p50 varies by roughly ±20%, p99 by ±40%).

| Variant | Phase | p50 | p95 | p99 | max | mean |
|---|---|---|---|---|---|---|
| no-persistence | `place_send_rest` | 28.2 | 46.2 | 60.0 | 66.8 | 29.8 |
| no-persistence | `place_send_oms` | 21.9 | 37.5 | 50.3 | 54.9 | 22.6 |
| no-persistence | `fill_state_update` | 6.0 | 8.0 | 22.9 | 31.1 | 6.4 |
| persistence | `place_send_rest` | 37.3 | 63.7 | 75.7 | 98.4 | 40.4 |
| persistence | `place_send_oms` | 29.8 | 54.2 | 67.9 | 90.3 | 32.9 |
| persistence | `fill_state_update` | 11.2 | 17.9 | 29.2 | 41.0 | 12.4 |

### Reading the numbers

- **Budget**: all phases sit two orders of magnitude under a "few
  milliseconds" budget; worst observed max across runs ~230 µs.
- **REST vs OMS seam**: the ~6–8 µs difference between
  `place_send_rest` and `place_send_oms` is the REST layer's JSON parse
  + field validation; the OMS core (dedup, risk, staging) is ~22 µs at
  p50.
- **Persistence cost**: the append+flush `EventLog` write inside the
  window adds ~6–12 µs (place_submitted before the send, state event on
  fill) on this tmpfs-backed filesystem.
- **Fill path**: the cheapest window (~6 µs p50) — the execution-report
  path takes no venue I/O and only the registry mutex.
- **Debug builds are not representative**: ASan+UBSan instrumentation
  runs ~50× slower (place ~1.7 ms p50). That is sanitizer overhead, not
  gateway cost; latency figures must always be taken from release
  builds.

## Reproducing

```sh
cmake --build build/release --target latency_bench_test
./build/release/latency_bench_test
```

Also registered in ctest (`latency_bench_test`, ~0.35 s), where it only
verifies that every measured order produced exactly one measurement per
phase — it never asserts wall-clock budgets (deterministic-testing
rule: numbers are reported, CI machines vary).

Runtime latency logging in the live gateway: set
`"latency": {"logPath": "data/latency.jsonl"}` in the config (absent =
disabled); each line is
`{"type":"latency","phase","clientOrderId","startNs","endNs","elapsedNs"}`.
