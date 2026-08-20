# Speeding up the dev loop

Ideas (original notes):

- Ninja + parallel jobs
- ccache
- mold / lld
- PCH
- once the APIs are stable between components, establish code frontiers
  (set of tests related to a component) => run only the tests of a single
  component if API/signatures are not changed.

## Current state vs. ideas (assessed 2026-08-20)

| Idea                | Status                      | Evidence                                                                       |
| ------------------- | --------------------------- | ------------------------------------------------------------------------------ |
| Ninja + parallel    | done already                | `CMakePresets.json` generator=Ninja; 4 cores in the dev container               |
| ccache              | wired but underperforming   | launcher set, persistent volume; stats: 8% hits, 0 direct hits, 92 misses       |
| mold / lld          | absent                      | not installed; default `ld.bfd`; 13 link targets each pulling Boost/OpenSSL     |
| PCH                 | absent                      | `crow_all.h` parsed in 5 TUs, `httplib.h` in 4, `json.hpp` in 9                 |
| Component frontiers | not formalized              | no test LABELS / scoped presets; every iteration runs all 9 tests               |

## Approved plan (not yet implemented)

### 1. ccache tuning — `Dockerfile`

```dockerfile
ENV CCACHE_BASEDIR=/workspace/rest_exchange_gateway
ENV CCACHE_DEPEND=1
ENV CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime
```

Keep existing `CCACHE_DIR=/ccache` and the 5G cap. No CMake changes needed.
Expected effect: direct hits instead of preprocess-only hits; container
rebuilds and preset switches become near-instant.

### 2. lld — `Dockerfile` + `CMakeLists.txt`

- Dockerfile: add `lld` to the apt install list.
- CMakeLists: option `GATEWAY_USE_LLD` (default ON when the compiler is
  GNU/Clang) -> `add_link_options(-fuse-ld=lld)`.
- Expected effect: 5-10x faster links across the 13 binaries; this dominates
  the edit-one-file -> relink-test-binaries loop.

### 3. Third-party PCH only — `CMakeLists.txt`

```cmake
target_precompile_headers(gateway_third_party INTERFACE
    <crow_all.h> <httplib.h> <nlohmann/json.hpp>)
```

Never project headers (APIs still moving in phase 1). Expected 40-60% cut
per Crow TU. Crow PCH needs ~1GB RAM to precompile — fine on 4 cores.

### 4. Test frontiers via labels + presets — `CMakeLists.txt` + `CMakePresets.json`

- `set_tests_properties(... PROPERTIES LABELS ...)`:
  - `core`: result, clock, config
  - `okx`: okx_signer, okx_wire, okx_rest_client, okx_connector
  - `rest`: rest_api
  - `smoke`: smoke_test
- New testPresets `debug-okx`, `debug-rest` (and release twins) using a
  `tests` filter, e.g. `ctest --preset debug-okx` runs only that component.
- Inner loop: scoped preset while iterating on one component; the full
  dual-preset suite (`ctest --preset debug` AND `--preset release`) remains
  the phase-close gate (see implement-todos.md ground rules).

Ninja already scopes compilation to changed components; this scopes test
runtime, matching the "run only the tests of a single component" idea.

## Verification (when implementing)

1. Rebuild the dev image (`docker compose build dev`).
2. Clean configure + build of both presets.
3. `ccache --show-stats` after the second build: expect direct hits > 0.
4. Time a touch-rebuild of `okx_wire.cpp` + relink of `rest_api_test`
   before/after.
5. Full `ctest --preset debug` and `--preset release` green; scoped presets
   run only their component's tests.

Notes: debug (ASan) and release have different flags -> separate cache
entries are expected; the first build per preset always misses.
