# Changelog

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
