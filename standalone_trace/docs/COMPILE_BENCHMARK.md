# Compile Performance Benchmarks

NVDLA design (1M signals, 4.3M endpoints), 15 GB RAM machine, swap flushed before each run.

## Baseline

Commit `494b78b` — no mimalloc, no string_view interning, no symbol path cache.

| Config | Build Graph | Wall Time | User Time | System Time | Peak RSS |
|---|---|---|---|---|---|
| (no `--low-mem`) | 87.6s | 142s | 49s | 93s | 5.5 GB |

## c2435ac — mimalloc + string_view interning + symbol path cache

| Config | Build Graph | Wall Time | User Time | System Time | Peak RSS |
|---|---|---|---|---|---|
| **without `--low-mem`** | 29.2s | **44.1s** | 39.2s | 5.5s | 5.35 GB |
| with `--low-mem` | 58.8s | 1:16 | 66.5s | 10.3s | 4.35 GB |

Note: the commit message claimed 46s wall time with 4.6 GB peak RSS. That benchmark was run
without `--low-mem` (confirmed by back-to-back comparison). The commit message was misleading.

## 677b77d + uncommitted — allocation elimination round

Changes on top of c2435ac: `from_chars` integer parsing, `string_view` SymbolPathLess,
`MergeEndpointBitRangesInPlace` fast path, clock/reset name detection without allocation,
sort+unique skip for small vectors, explicit move semantics.

| Config | Build Graph | Wall Time | User Time | System Time | Peak RSS |
|---|---|---|---|---|---|
| **without `--low-mem`** | **25.2s** | **39.4s** | 34.8s | 5.3s | 5.37 GB |
| with `--low-mem` | 51.4s | 1:06 | 61.1s | 5.6s | 4.35 GB |

## Summary

| Version | `--low-mem` | Build Graph | Wall Time | Peak RSS |
|---|---|---|---|---|
| Baseline (494b78b) | no | 87.6s | 142s | 5.5 GB |
| c2435ac | no | 29.2s | 44.1s | 5.35 GB |
| c2435ac | yes | 58.8s | 1:16 | 4.35 GB |
| New optimizations | **no** | **25.2s** | **39.4s** | 5.37 GB |
| New optimizations | yes | 51.4s | 1:06 | 4.35 GB |

**Conclusion**: `--low-mem` is a net performance loss when ≥10 GB RAM is available.
The full cache avoids ~25s of redundant AST re-traversal. Omit `--low-mem` unless
the machine is memory-constrained.
