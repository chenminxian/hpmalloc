# Allocator benchmark

**English** | [简体中文](Bench-zh_CN.md)

`bench/alloc_bench.cpp` is allocator-agnostic: it calls the standard `malloc` family and is selected at run time with `LD_PRELOAD`.  `bench/run_bench.sh` interleaves glibc, jemalloc, tcmalloc, mimalloc and hpmalloc in rotating order, repeats each workload in a fresh process, and `bench/summarize.py` emits Markdown tables.

No allocator is downloaded or installed by these scripts.  The comparison uses the allocator libraries already present on the machine.  Override paths with `HPMALLOC_LIB`, `JEMALLOC_LIB`, `TCMALLOC_LIB` and `MIMALLOC_LIB`.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hpmalloc_bench
```

## Run

```sh
# Full suite: 10 process repetitions, 0.6 s measured, 0.2 s warmup.
# Thread counts are selected automatically from the online CPU count.
bench/run_bench.sh

# Same with a different hpmalloc build or options.
HPMALLOC_LIB=$PWD/build-clang/libhpmalloc.so \
HPM_OPTIONS=thp=never \
bench/run_bench.sh --outdir bench/results/clang-thp-never

# One workload or a regex-selected subset, with fewer repetitions for a
# quick smoke test.
bench/run_bench.sh --only 'churn-small|large-4m' --reps 3 --seconds 1.0

# Override the automatic thread sequence.
bench/run_bench.sh --max-threads 32 --thread-step 4 --reps 7

# Summarize existing raw data.
python3 -I bench/summarize.py bench/results/raw.csv \
    --out bench/results/summary.md
```

The default `--reps` is 10: enough to reduce the effect of CPU-frequency and scheduler jitter on this class of machine.  Use `--reps 3` only for smoke tests or while iterating on the allocator; use more repetitions for final numbers. Full-suite runtime grows with both `--reps` and the number of thread points, so use `--only` for targeted comparisons on a high-core-count host.

`run_bench.sh` reads the online CPU count with `nproc` (falling back to `getconf _NPROCESSORS_ONLN`).  `MAX_THREADS` defaults to that value and `THREAD_STEP` defaults to `ceil(MAX_THREADS / 10)`, giving about ten points from 1 to the hardware concurrency.  For example, on a 28-CPU host the churn sequence is `1,4,7,10,13,16,19,22,25,28`, and the cross sequence is `2,6,10,14,18,22,26,28` (cross workers must be even).  The final endpoint is appended if it is not already on the arithmetic sequence.

`churn-*` and `cross-*` use the full sequence.  `fixed-*`, `realloc-*` and the multi-threaded `large-*` rows use only the 1-thread and maximum-thread endpoints, which keeps the suite bounded on high-core-count machines.  Set `--max-threads` and `--thread-step` when a different range is needed.

Outputs:
- `bench/results/raw.csv` — one row per invocation
- `bench/results/metadata.txt` — host, libraries, options, hashes, and the auto-selected thread sequences
- `bench/results/errors.log` — failed invocations, if any
- `bench/results/summary.md` — median throughput, peak RSS, variability and aggregate small-object ratios

## Workloads

| tag | measures |
|---|---|
| `pair-SIZE` | malloc(size) + touch first byte + free(size); tcache fast path |
| `fixed-SIZE` | replace one object in a live set of 1024 (or 256 for 64 KiB) |
| `churn-*` | replace one object per cycle; sizes log-uniform in the range |
| `cross-*` | producer allocates, consumer frees; queue of 256 pointers |
| `realloc-*` | single buffer grown from 16 B to max in 1.5x steps; glibc top-chunk advantage |
| `realloc-multi-*` | several live buffers per worker, grown in 1.5x steps and reset to min; no top-chunk advantage |
| `realloc-mixed-*` | several live buffers, random grow/shrink in [min,max]; covers shrink too |
| `large-*` | one alloc/free cycle of a large object; `pages` touches each 4 KiB page |
| `calloc-*` | calloc + read first byte (must be zero) + free; lazy zero is visible |

`pair`, `fixed`, `churn`, `cross`, `large` and `calloc` report two allocator entry points per cycle; `realloc`, `realloc-multi` and `realloc-mixed` report one per realloc call.  The summary uses allocator API calls/s.  `ns_per_cycle` in the raw CSV is the cycle time.  For `realloc-multi` and `realloc-mixed`, `live` in the raw row is the number of live buffers per worker.

Absolute rates move with CPU frequency and scheduling; `summarize.py` therefore also computes per-repetition paired ratios to glibc and reports their median. Those ratios, not the quotient of two independently taken medians, are the appropriate comparison.
