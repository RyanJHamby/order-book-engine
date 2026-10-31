---
id: reject-duplicate-order-id
goal: OrderBook::add_order ignores an order whose id is already resting on the book, so no un-cancellable ghost order can be created; ids of filled or cancelled orders can be reused.
acceptance:
  - cmake -S orderbook-engine -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target orderbook_duplicate_id_tests && ./build/tests/orderbook_duplicate_id_tests
  - cmake -S orderbook-engine -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build/tests --output-on-failure
scope:
  - orderbook-engine/src/orderbook.cpp
  - orderbook-engine/include/orderbook.hpp
  - orderbook-engine/tests/CMakeLists.txt
non_goals:
  - do not change any assertion in orderbook-engine/tests/test_duplicate_order_id.cpp; the only test-side change is deleting the `set_tests_properties(DuplicateOrderIdTests PROPERTIES WILL_FAIL TRUE)` line in tests/CMakeLists.txt (the second acceptance command fails while WILL_FAIL remains and the work is done)
  - do not weaken or delete any existing test; the full ctest run in acceptance command 2 must stay green
  - do not change the Order/Fill structs, add a return value or error code to add_order, or add exceptions (the release build uses -fno-exceptions)
  - do not add validation for zero quantity, negative price, NaN price or other bad input; that is a separate decision
  - do not quote or update any benchmark number; the extra hash lookup may shift the README latency figures, and re-measuring is the owner's job via `pow run`
risk_tier: 2
max_runs: 3
autonomy: inherit
status: ready
issue: 6
---

## The gap (found by reading the tests and `src/orderbook.cpp`, then confirmed with a 10-line program)
`add_order` records `order_index_[incoming.id]` for any resting order without checking whether that id is already present.
`tests/test_edge_cases.cpp` ("OrderBookIdenticalOrders") rests five orders at the same price with different ids but nothing
exercises a repeated id. Behaviour today: add BUY id=1 @100 x10, then BUY id=1 @101 x5 gives `bid_count() == 2`; the first
`cancel_order(1)` removes only the second (the index points to 101); the second `cancel_order(1)` returns false while the
original 100 x10 order is still on the book and still matchable. That is a ghost order a caller cannot remove.

## Behaviour to implement
At the top of `OrderBook::add_order`: if `order.id` is currently resting (present in `order_index_`), return without matching,
resting, producing fills, or consuming a timestamp. Everything else is unchanged.
- "Resting" means in `order_index_`: a partially filled resting order still counts.
- An id whose order was fully filled (as resting or on arrival) or cancelled is free and may be used again.
- The rule applies across sides: a SELL with the id of a resting BUY is ignored.
- Silent ignore is the chosen design (the signature returns void and the hot path avoids exceptions). If the owner prefers a
  bool return, that is a separate spec.

## How the test is wired (ctest + GTest; this repo has no pytest or Catch2)
`orderbook-engine/tests/test_duplicate_order_id.cpp` is built into its own executable `orderbook_duplicate_id_tests` and
registered as CTest test `DuplicateOrderIdTests` with `WILL_FAIL TRUE`, so the full suite is green today. Acceptance command 1
runs the executable directly (ignores WILL_FAIL): fails today, passes when done. Acceptance command 2 builds everything and runs
all of ctest from `build/tests` (note: the CMake `enable_testing()` is in `tests/`, so run ctest with `--test-dir build/tests`):
passes today thanks to WILL_FAIL, fails once the feature works until the WILL_FAIL line is removed, then passes. Keep the
dedicated target; do not fold the file into `orderbook_tests`.

## Hard facts
- `cancel_order` uses `order_index_` -> `{price, side}`; the book is `std::map<double, std::deque<Order>>` per side.
- Tests need exceptions on; the main target builds with `-fno-exceptions -fno-rtti` and `-O3 -flto`.
- Run from the repo root. Build dir `build/` is git-ignored.

## Log
- 2026-10-30 draft (provisional handshake; repo not onboarded to autopilot yet)
- 2026-10-31 ready, issue #6 (handshake was provisional; repo not yet onboarded with ap init; waiting for the owner's agent-ready label)
