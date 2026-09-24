# Contributing

The contract that keeps every implementation honest is `spec/` + `vectors/`:

- **Semantics changes** start in `spec/SPEC.md`, then get a golden vector
  (`vectors/**.cmd.jsonl`) plus its canonical `.evt.jsonl`, then land in every
  implementation. Ports are ports — no language-specific behavior.
- **Verify**: `./scripts/verify.sh` must pass (all implementations replay every
  vector byte-identically).
- **Rust**: `cargo fmt`, `cargo clippy --all-targets -- -D warnings`,
  `cargo test`. No `unsafe` — the crate is `#![forbid(unsafe_code)]`.
- **Go**: `gofmt`, `go vet`, `go test ./...`.
- **C++**: C++20, `-Wall -Wextra` clean, `ctest`. Header-only — keep the hot
  path inlinable; no virtual dispatch in match loops.
- **Benchmarks**: use `tools/vectorgen` workloads, follow `spec/BENCH.md`,
  report environment (CPU/OS/flags). No unattributed numbers.

## Adding a language port

1. Read `spec/SPEC.md` + `spec/SCHEMA.md`.
2. Mirror the reference decomposition: pooled orders → intrusive FIFO levels →
   bitmap ladder (+ tree fallback) → open-addressed id map → book → engine →
   sink seam.
3. Write a golden runner that emits canonical `*.evt.jsonl` and diffs against
   `vectors/`. Byte-identical is the bar — then benchmarks.
