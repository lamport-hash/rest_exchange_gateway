# Code Design Guidelines — C++20 Exchange Gateway

**Target**: High-reliability, low-latency REST gateway that presents a single unified API to multiple cryptocurrency/spot/derivatives exchanges.

These rules are mandatory for all new code. Prefer clarity and correctness over cleverness. Follow the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/) unless this docume
nt overrides them.

## 1. Language & Toolchain

- C++20 only (no C++23 features yet).
- Mandatory: `-Wall -Wextra -Wpedantic -Werror`, AddressSanitizer + UndefinedBehaviorSanitizer in debug/CI.
- Prefer standard library + carefully vetted header-only libraries. Avoid Boost unless there is a clear, measured benefit.
- Modules are allowed where the build system supports them; otherwise classic headers + include guards / `#pragma once`.

## 2. Project Layout

```
include/gateway/          # Public headers (the 1-REST surface)
src/
  core/                   # Shared types, config, logging, metrics
  exchange/               # One subdirectory per exchange (binance/, bybit/, ...)
  rest/                   # Unified REST handlers
tests/
```

- Public headers live under `include/gateway/`.
- Implementation details never leak into public headers.
- One logical component = one `.hpp` + one `.cpp` (or header-only when appropriate).


## 3. Naming

| Entity | Style | Example | Notes / C++20 Context |
|---|---|---|---|
| Namespaces | snake_case | gateway::exchange | Keep names short. Use nested namespaces to group modules. |
| Types / Classes | PascalCase | OrderRequest, BinanceClient | Applies to class, struct, union, and using type aliases. |
| Concepts | camelCase | Arithmetic, isStrategy | Use camelCase to distinguish C++20 concepts clearly from types. |
| Template Parameters | PascalCase | T, StrategyType, Alloc | Use single letters for generic types, or full PascalCase for clarity. |
| Functions / Methods | snake_case | place_order(), on_fill() | Do not use get_ prefixes for basic property accessors (e.g., rect.width()). |
| Async Functions | snake_case + _async | fetch_data_async() | Mandatory suffix for functions returning coroutine tasks or std::future. |
| Function Parameters | a_ prefix | a_order_id, a_timeout | Explicitly identifies an incoming Argument. Prevents variable shadowing. |
| Local Variables | snake_case | order_id, is_connected | Plain, clean naming without any type or scope prefixes. |
| Member Variables | trailing underscore | last_seq_, socket_ | Identifies encapsulated state. Safe for text searching (_ filter). |
| Enums (Scoped) | PascalCase | OrderSide::Buy, Status::Idle | Always use enum class. Never use SCREAMING_SNAKE for enum values. |
| Constants (constexpr) | kPascalCase | kMaxRetries, kDefaultTimeout | Identifies compile-time constants safely without relying on macros. |
| Macros | SCREAMING_SNAKE | GATEWAY_ASSERT | Reserved exclusively for preprocessor macros. High-alert visual styling. |


## 4. Core Design Principles

### Abstraction
- Single unified interface for all exchanges (`IExchange` or concept-based).
- Exchange-specific code lives only inside its own subdirectory and is never referenced from the public REST layer.
- Prefer composition + concepts over deep inheritance.

### Types & Safety
- Prefer `std::string_view`, `std::span`, `std::optional`, `std::expected` (or equivalent) over raw pointers and out-parameters.
- Strong types for IDs, prices, quantities, timestamps (`OrderId`, `Price`, `Qty`, `Timestamp`).
- `[[nodiscard]]` on all functions that return a value that should not be ignored.
- No raw `new`/`delete`. Use RAII (`std::unique_ptr`, `std::shared_ptr` only when shared ownership is required).
- Prefer value semantics and move semantics. Avoid unnecessary copies in hot paths.

### Error Handling
- Prefer `std::expected` / Result types for expected failures.
- Exceptions only for truly exceptional / unrecoverable situations.
- Never let exceptions cross the public REST boundary; convert to structured error responses.

### Concurrency & Latency
- Prefer lock-free or carefully scoped locks, with lock guard. Document every mutex.
- Use `std::jthread`, `std::stop_token`, and structured concurrency patterns.
- Hot paths must be allocation-free after start-up (pre-allocate, use arenas / object pools where measured beneficial).
- Prefer `std::atomic` with explicit memory orders when lock-free is required.

### Modern C++20 Features (encouraged)
- Concepts for constraints.
- `std::format` / `std::print` instead of iostreams for logging and messages.
- Ranges and views where they improve clarity without measurable cost.
- Coroutines only for clearly asynchronous I/O flows (document why).
- Designated initializers for aggregates.
- `constexpr` / `consteval` aggressively for configuration and compile-time checks.

## 5. Style & Formatting

- clang-format (LLVM or Google style, project `.clang-format` is authoritative).
- 4-space indentation, no tabs.
- Braces on the same line for control structures, next line for functions/classes (Allman for types, K&R for control).
- Maximum line length 100 characters.
- One statement per line.
- Prefer early returns / guard clauses over deep nesting.

## 6. Headers & Includes

- Self-contained headers.
- `#pragma once`
- Include order: corresponding header → project headers → third-party → standard library
- Prefer forward declarations in headers; include only what is necessary.
- No `using namespace` in headers. Limited `using` declarations in `.cpp` files are acceptable.

## 7. Testing & Observability

- Every public API and every exchange adapter must have unit tests.
- Prefer deterministic tests; mock network / time.
- Structured logging (JSON or key-value) with clear levels. Never log secrets or full order payloads in production.
- Metrics (latency histograms, error rates, exchange-specific counters) are first-class citizens.

## 8. Performance Rules of Thumb

1. Measure first. Never optimise without data.
2. Prefer simple, correct code. Complexity is only justified by measured gains.
3. Cache-friendly data layouts (SoA vs AoS decided by profiling).
4. Avoid virtual calls in the hottest paths; prefer static polymorphism (CRTP / concepts) when needed.

## 9. Forbidden / Strongly Discouraged

- Raw owning pointers.
- `std::endl` (use `'\n'`).
- Implicit conversions that lose information.
- Global mutable state.
- Magic numbers (use named constants).
- Copy-paste of exchange-specific logic — extract common patterns.

## 10. Documentation

- Public headers must have clear, concise comments describing pre/post-conditions and error behaviour.
- Prefer self-documenting code over long comments.
- Use `///` for Doxygen-style public API documentation.

---

**When in doubt**: ask user , list the solutions that is the simpliest to express, hardest to misuse, and still meets the latency/reliability requirements of a production exchange gateway.
