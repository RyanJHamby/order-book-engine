set shell := ["bash", "-c"]

# run the unit tests (builds first)
test:
	cd orderbook-engine && ./scripts/run_tests.sh

# build and run the latency benchmark
bench:
	cd orderbook-engine && ./scripts/build_and_run_benchmark.sh

# thread-sanitizer build and tests (separate build-tsan/ tree)
tsan:
	cd orderbook-engine && ./scripts/run_tsan.sh
