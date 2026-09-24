# Contributing

The contract that keeps this library honest is `spec/` + `vectors/` (vendored
from the [matcher spec repo](https://github.com/abhijitkrm/matcher)):

- **Semantics changes** start upstream in `spec/SPEC.md` plus a golden vector
  (`vectors/**.cmd.jsonl`) with its canonical `.evt.jsonl`. The implementation
  must emit that stream byte-identically — in every index mode the vector
  declares.
- **Verify**: `cmake -B build && cmake --build build && ctest --test-dir build`
  replays all golden vectors.
- **Style**: C++20, `-Wall -Wextra` clean, zero dependencies. Header-only —
  keep the hot path inlinable, no virtual dispatch in match loops.
- **Performance**: pooled orders, intrusive levels, direct-indexed ladder,
  enum-dispatched index. Benchmarks use `tools/vectorgen` workloads per
  `spec/BENCH.md` — report CPU/OS/flags, no unattributed numbers.
