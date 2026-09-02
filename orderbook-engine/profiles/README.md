# profiles/

Captured `perf` output from this repo's own EC2 benchmark run, kept as
concrete evidence rather than a description of what the tools produce.

- `orderbook_ec2.perf_script.txt` — captured on a `c6i.large` (Intel Xeon
  Ice Lake) On-Demand instance via `scripts/cloud_init.sh`:
  ```
  perf record -F 999 -g --call-graph dwarf -o perf.data -- ./latency_benchmark
  perf script -i perf.data > orderbook_ec2.perf_script.txt
  ```
  Raw `perf script` text — speedscope.app imports it directly (drag it in,
  or use the `#profileURL=` link in the main README to load it straight
  from GitHub with full interactivity: zoom, search, a "sandwich" view
  grouping by function regardless of call path). `perf` only exists on
  Linux, so this can't be regenerated locally on macOS — see
  `scripts/run_benchmark.sh` to capture a fresh one on EC2.

  What it shows: samples land inside `OrderBook::add_order` →
  `std::map<double, std::deque<Order>>::operator[]` →
  `_Rb_tree_insert_and_rebalance` — the price-level map's red-black tree
  insertion is real, visible cost, alongside some `do_anonymous_page`
  kernel time from first-touch page faults on the order pool. See the main
  README's Performance section for what this implies about the gap between
  per-op latency and sustained pipeline throughput.
