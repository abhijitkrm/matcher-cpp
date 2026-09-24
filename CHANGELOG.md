# Changelog

## v0.1.0 — 2025-01-XX

Initial release.

- Header-only C++20 `OrderBook` + `Engine`, `Sink` concept seam
- Limit/Market, New/Cancel/Replace, GTC/IOC/FOK/Post-Only
- FIFO price-time priority, maker-price execution
- Pooled orders, intrusive FIFO levels, bitmap ladder index,
  `std::map` fallback, enum-dispatched index
- CMake install + `matcher::matcher` package target
- 41 golden vectors passing — byte-identical to matcher-rust and matcher-go
- Zero dependencies
