# matcher-cpp

[![ci](https://github.com/abhijitkrm/matcher-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/abhijitkrm/matcher-cpp/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue.svg)](LICENSE-MIT)

Deterministic FIFO limit order book and matching engine core — header-only
C++20, a port of the reference [Rust implementation](https://github.com/abhijitkrm/matcher-rust).

Single-writer book per symbol, commands in, monotonically sequenced events out.
All I/O hangs off the `Sink` concept; there is no networking, persistence, or
clock dependence in the core. Zero dependencies.

```cpp
#include <matcher/matcher.hpp>
```

```cpp
matcher::OrderBook book(matcher::BookConfig{});
matcher::VecSink sink;

book.apply(matcher::Command::new_limit(1, matcher::Side::Ask, 100, 10,
                                       matcher::Tif::Gtc), sink);
book.apply(matcher::Command::new_limit(2, matcher::Side::Bid, 100, 4,
                                       matcher::Tif::Gtc), sink);
// order 2 filled 4 @100 against order 1 and closed; order 1 keeps 6 resting.
```

## Use it

Header-only, C++20 — add `include/` to your include path, or via CMake:

```cmake
add_subdirectory(matcher-cpp)
target_link_libraries(your_target PRIVATE matcher)
# or: cmake --install build, then find_package(matcher) → matcher::matcher
```

## Features

- Limit + Market orders, New / Cancel / Replace
- GTC, IOC, FOK, Post-Only
- FIFO price-time priority, maker-price execution, partial fills, sweeps
- Pooled orders, intrusive FIFO price levels, bitmap ladder index with
  `std::map` fallback for unbounded prices
- Enum-dispatched index — no virtual dispatch on the hot path
- Thin multi-symbol `Engine` router
- Deterministic event streams — byte-identical to the
  [Rust](https://github.com/abhijitkrm/matcher-rust) and
  [Go](https://github.com/abhijitkrm/matcher-go) implementations, verified
  against the shared golden vector corpus (`vectors/`)

## Layout

```
include/matcher/   headers (book, engine, types, sink, detail/internals)
tests/golden.cpp   golden vector runner
bench/             matcherbench harness
vectors/           shared golden corpus (spec repo: github.com/abhijitkrm/matcher)
spec/              semantics contract (SPEC.md, SCHEMA.md, BENCH.md)
tools/             vectorgen — deterministic workload generator (Rust tool)
```

## Test & bench

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # 41 golden vectors

# benchmark — vectorgen is a small Rust tool (see spec/BENCH.md)
mkdir -p bench/corpora
cargo run --release --manifest-path tools/vectorgen/Cargo.toml -- \
  --workload w2 --n 200000 --setup-n 100000 --out bench/corpora/w2
./build/matcherbench bench/corpora/w2
```

## License

MIT OR Apache-2.0
