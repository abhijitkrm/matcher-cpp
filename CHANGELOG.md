# Changelog

## Unreleased

- New hot-path layout (`../matcher/docs/LAYOUT.md`): resting orders live
  inline in 128-byte per-level chunks (unrolled FIFO, dead slot = qty 0)
  instead of a pooled record + intrusive prev/next list; matching reads
  makers sequentially and a cancel touches one chunk.
- `OrderMap`: 1-byte tag array + `{key, val}` entries; absent-id checks
  touch only tags. `replace` skips the re-probe when no entry moved.
- `OrderBook::apply_batch()`: same events as `apply()` per command, plus a
  look-ahead prefetch pipeline once the book outgrows cache;
  `set_prefetch_threshold()`; `prefault()`.
- `matcherbench --mode latency|throughput|scale` (BENCH.md v2), `-flto`,
  optional PGO (`scripts/pgo.sh`, `-DMATCHER_PGO_PROFILE`).
- Aggregate (`--mode scale`, 8 shards, M1): 112M ops/s on the mixed
  workload, 305M on a 1k-order book.
- Golden suite also checks `apply_batch` (pipeline forced on) against
  `apply` for every vector.

## v0.2.0 — 2026-10-10

- Journals and snapshots (`spec/JOURNAL.md`): command/event journal,
  `matcher-snap/1` snapshots, restore; `matcherrun`, `matcherrecover`,
  `matchersnap`; `matcherfuzz` differential-fuzz harness.
- `engine: true` golden vectors (multi-symbol `Engine`).
- Vector `edge/042_dense_map_churn` (43 golden vectors).
- Fix: `OrderMap::remove` dropped live keys during backward-shift deletion.
- `Engine` builds a book only for a new symbol (it built one per command).
- The price ladder finds the next best price through a summary bitmap,
  and an emptied side resets at once. A side whose last level emptied
  used to scan the whole ladder: W3 at 1M ops (the book drains) ran at
  0.64M and now runs at 15.3M orders/s. Output unchanged; a randomized
  test checks the cursor against a sorted reference.

## v0.1.0 — 2026-09-24

Initial release.

- Header-only C++20 `OrderBook` + `Engine`, `Sink` concept seam
- Limit/Market, New/Cancel/Replace, GTC/IOC/FOK/Post-Only
- FIFO price-time priority, maker-price execution
- Pooled orders, intrusive FIFO levels, bitmap ladder index,
  `std::map` fallback, enum-dispatched index
- CMake install + `matcher::matcher` package target
- 41 golden vectors passing — byte-identical to matcher-rust and matcher-go
- Zero dependencies
