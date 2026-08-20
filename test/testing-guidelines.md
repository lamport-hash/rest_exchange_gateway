# Testing Guidelines

Mandatory rules (from `doc/general-guidelines.md`, `doc/code-guidelines.md` §7, `doc/project-spec.md`):

- Every function must have a test illustrating the normal use case.
- For every function, list dangerous edge cases and test them whenever the behavior is not trivial.
- Every public API and every exchange adapter must have unit tests.
- Unit tests for the order state machine are a required deliverable.
- A small number of integration-style tests (or a test harness) is required.

## Style

- Deterministic tests only: mock network and time; never depend on live exchange connectivity.
- Run with AddressSanitizer + UndefinedBehaviorSanitizer in debug/CI (per `doc/code-guidelines.md` §1).
- No test framework is chosen yet — decide with the user when the build system is set up.

## Gateway-Specific Edge Cases to Cover

Derived from `doc/project-spec.md` (these are evaluation criteria, not optional):

- Order state machine: New/Live → Partially Filled → Filled; Canceled; Rejected (illegal transitions rejected).
- Out-of-order WebSocket messages.
- Duplicate execution reports (no double application).
- REST vs WebSocket race conditions.
- Client retry idempotency (same `clientOrderId` twice → same outcome).
- Pre-trade risk rejections: max order size, max notional, position limit.
- Restart recovery: reload persisted orders, reconcile with OKX open orders, re-establish Binance WebSocket state.
- WebSocket disconnect/reconnect.

## Stretch Goals (nice to have)

- Property-based tests for order state transitions.
