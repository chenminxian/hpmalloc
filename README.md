# hpmalloc

**English** | [简体中文](README-zh_CN.md)

A Linux-only, C++23 high-performance generic memory allocator.

**Short description:** one allocator for requests from 16 B to multi-megabyte runs, with a full-range thread cache, sharded central bins, a reserved
2 MiB-slot arena, a flat class map for the normal-size free path, a cached large-object path, and no external allocator runtime dependency.  It can be linked as a normal library or dropped into an existing process with `LD_PRELOAD`. It is compatible with basic components of Linux systems and desktop distributions.

## What makes it different

- **Full-range TCache:** 68 normal size classes from 16 B to 2 MiB, plus eight aligned class groups (32 B to 4 KiB alignment); 612 classes total.  A 16-byte AoS cache-bin layout keeps the hot bin metadata in one cache line.
- **Sharded central bins:** Eight shards per class, each with its own cache-line-isolated TTAS lock.  Threads prefer one shard, can steal from another, and flush objects back to the span's owning shard.  This is what keeps multi-thread same-class refill/flush from collapsing onto one lock.
- **Flat class map:** One byte per 4 KiB granule maps a normal-size object directly to its class, so `free`/`realloc`/`usable_size` avoid the
  pagemap → extent → granule-entry chain.  Aligned classes and large runs keep the precise metadata path; `class_map=0` disables the flat table.
- **Reserved arena:** 64 GiB of virtual address space by default (`PROT_NONE|MAP_NORESERVE`), carved into 2 MiB slots and 4 KiB granules with lazy `mprotect` commit.  Large requests become contiguous slot runs.
- **Large-run cache:** Freed large runs are retained up to 64 MiB per arena, so repeated 4 MiB/16 MiB/64 MiB use does not pay `mprotect`/`madvise` and page faults on every cycle.
- **Bounded empty-span retention:** Small spans and 256 KiB–2 MiB spans have separate per-shard budgets, so cross-thread producer/consumer workloads can reuse spans without letting rare large spans starve the hot small classes.
- **Lazy large calloc:** Large zeroed allocations use anonymous `MADV_DONTNEED` pages instead of eagerly `memset` the buffer.
- **Two-phase purge:** Small-span, large-run and large-cache purge work is done outside the arena lock; the address range stays reserved until it completes.
- **THP policy with nuance.** Small extents are marked `MADV_NOHUGEPAGE`; full large chunks may use `MADV_HUGEPAGE`; large tails stay out of the way.
- **Realloc that avoids copies.** Normal-class growth over-allocates by 100% (bounded by the class table), existing direct mappings grow with `mremap(MREMAP_MAYMOVE)`, and large runs grow in place when the adjacent slots are free.
- **Compile-time size classes.** `consteval` tables generate the class plan at build time, with `static_assert` validation.
- **No allocator dependency.** The shared/static library only needs libc, no extra runtime is linked.

## Performance at a glance

Two full runs are used: an 8-thread laptop (8 process repetitions) and a 28-thread workstation (12 repetitions).  Each run compares glibc, jemalloc, tcmalloc, mimalloc and hpmalloc in fresh processes, rotates the allocator order every repetition, and reports **paired ratios** (ratio computed inside one repetition, then median) so CPU-frequency drift cancels.  Absolute calls/s are host-specific; compare ratios, not the absolute columns across machines.

**See [Performance.md](docs/Performance.md) for raw data and full analysis of the benchmark.**

### 8-thread laptop (Intel i7-1195G7, 16GiB LPDDR4, 3733MT/s)
| workload | hpm calls/s | vs glibc | vs jemalloc | vs tcmalloc | vs mimalloc |
| --- | ---: | ---: | ---: | ---: | ---: |
| pair-16 | 412.48M | 0.97x | 1.11x | 1.02x | 0.88x |
| pair-4096 | 334.75M | 5.13x | 1.44x | 0.96x | 2.63x |
| pair-65536 | 334.04M | 4.74x | 48.86x | 0.89x | 2.95x |
| pair-1048576 | 235.04M | 4.68x | 48.22x | 5.67x | 38.75x |
| fixed-64-t1 | 307.03M | 1.04x | 1.08x | 0.97x | 1.00x |
| churn-small-t8 | 454.64M | 2.56x | 1.12x | 1.06x | 2.52x |
| churn-mixed-t8 | 390.66M | 3.37x | 4.78x | 1.32x | 2.28x |
| cross-small-t8 | 65.13M | 5.81x | 1.43x | 2.15x | 1.10x |
| cross-mixed-t8 | 66.10M | 8.22x | 3.92x | 5.54x | 1.22x |
| realloc-multi-64k-t8 | 12.04M | 2.57x | 2.38x | 2.87x | 2.15x |
| realloc-multi-256k-t8 | 2.39M | 2.55x | 1.95x | 2.21x | 2.13x |
| large-16m-pages-t1 | 119.84k | 0.96x | 29.34x | 1.10x | 1.05x |
| calloc-64k-t1 | 2.71M | 1.05x | 1.38x | 1.02x | 1.03x |

### 28-thread workstation (Intel i7-14700K, 48GiB DDR5, 4200MT/s)
| workload | hpm calls/s | vs glibc | vs jemalloc | vs tcmalloc | vs mimalloc |
| --- | ---: | ---: | ---: | ---: | ---: |
| pair-16 | 559.03M | 1.52x | 1.99x | 0.82x | 13.99x |
| pair-4096 | 530.24M | 5.60x | 1.88x | 0.79x | 20.41x |
| pair-65536 | 479.61M | 5.10x | 19.56x | 0.71x | 402.12x |
| pair-1048576 | 478.15M | 5.09x | 19.48x | 5.77x | 6075.37x |
| fixed-64-t1 | 399.81M | 1.34x | 1.31x | 0.89x | 9.81x |
| churn-small-t28 | 2.02G | 2.54x | 1.03x | 1.08x | 57.01x |
| churn-mixed-t28 | 1.62G | 3.28x | 3.64x | 27.71x | 536.97x |
| cross-small-t28 | 426.26M | 4.72x | 1.02x | 9.88x | 6.46x |
| cross-mixed-t28 | 407.84M | 6.26x | 2.80x | 26.67x | 13.78x |
| realloc-multi-64k-t28 | 13.82M | 2.00x | 2.97x | 2.96x | 5.39x |
| realloc-multi-256k-t28 | 2.28M | 1.79x | 2.06x | 2.16x | 3.82x |
| large-16m-pages-t1 | 173.25k | 0.98x | 30.87x | 1.14x | 57.35x |
| calloc-64k-t1 | 2.73M | 1.04x | 1.82x | 1.00x | 3.31x |
> The very large `mimalloc` ratios at 64 KiB/1 MiB are a real throughput collapse in this repeated-`pair` workload, not a formatting error; the same rows show jemalloc's cache-tier cliff.
> See [Performance.md](docs/Performance.md) for the raw rates.

### What the numbers mean

hpmalloc's design target is the middle of the size spectrum.  It does not fall off a thread-cache cliff at 1 KiB (a familiar glibc boundary) or at 64 KiB and 1 MiB, where the jemalloc comparison collapses in repeated `pair` workloads. The same TCache policy covers the whole normal range, and the 256 KiB/1 MiB classes use 2 MiB spans backed by the large-object machinery.

The class map improved the performance of small-object allocation.  Before it, a normal-size free did an arena range check, a pagemap load and a 16-byte granule-entry load; `pair-16` through `pair-1024` and `fixed-64` were visibly behind glibc.  With the flat map, the normal-class free path is a range check plus a one-byte load; the remaining gap to glibc/tcmalloc on the smallest sizes is small, and the full-range TCache wins clearly once the live set churns or the size passes the glibc tcache range.

Large-object first-touch and page-touching costs are no longer a pure syscall tax.  The large-run cache keeps 4 MiB/16 MiB/64 MiB reuse warm, and lazy large `calloc` avoids writing zeros that the kernel already guarantees.

### The honest part

- **The smallest single-object loops are not the win.** `pair-16` through `pair-1024` and `fixed-64` are close to glibc, but tcmalloc is still faster on some of them (especially on the 28-thread host).  hpmalloc's advantage is the full-range cache under live-set churn, not the tight one-object loop.
- **64 KiB fixed-size requests favor tcmalloc.** Its large-class thread cache is very effective; hpmalloc's 512 KiB per-class target holds only eight 64 KiB objects.
- **Some 4 MiB page-touching rows are page-fault-bound.** Multi-thread `large-4m-pages` can be below glibc because the benchmark touches every page; this is dominated by THP/page behavior, not allocator locking.
- **256 KiB mixed realloc is the residual realloc weakness.** hpmalloc is about 0.90x glibc/jemalloc on `realloc-mixed-256k-t8` and 0.77x glibc / 0.59x jemalloc on `realloc-mixed-256k-t28` (tcmalloc is still slower there). The 2 MiB span and mixed shrink/grow pattern leave room for a future class-specific policy.
- **Cross-thread mixed RSS is the price of deep caches.** `cross-mixed` peaks higher than glibc/tcmalloc; the per-thread TCache worst case is about 28 MiB for normal classes and 283 MiB for all 612 classes.  There is no global byte budget yet.
- **Single-buffer realloc is a contrast case.** The benchmark grows one top-of-heap buffer; glibc can extend it in place.  Use the `realloc-multi` and `realloc-mixed` rows for the general comparison.
- **Scope.** No latency percentiles, long-running fragmentation/RSS plateau, NUMA, static linking or setuid hardening are claimed.  Peak RSS is not a fragmentation metric.

## Design at a glance

```text
application
  │  malloc/free/calloc/realloc/aligned/C++ new
  ▼
TCache (per-thread, per-class, 16 B AoS bins)
  │  batch refill / flush
  ▼
CentralBin[class][8] (TTAS lock per shard, span list, retained empty spans)
  │  page alloc / release
  ▼
Arena: 64 GiB VA, 2 MiB slots, 4 KiB granules, lazy commit
  ├─ small extent: 512 granule bitmap + granule table + class_map
  ├─ large run: contiguous slots + per-arena 64 MiB cache
  └─ direct mmap fallback: >2 MiB alignment or arena exhaustion
```

Object ownership is intentionally absent: a freed object goes into the current thread's TCache, then back to a central bin shard; any thread can reuse it. This is what makes cross-thread producer/consumer workloads stable and avoids "owner thread is asleep" stalls.  The cost is the free-path lookup and the per-thread cache footprint.

Key trade-offs:
| choice | what it buys | what it costs |
|---|---|---|
| 64 GiB VA reservation, lazy commit | stable addresses, cheap growth, no remap | VA/RLIMIT/overcommit sensitivity; per-slot metadata |
| 2 MiB slots, 4 KiB granules | simple spans, fast restore, THP-aligned slots | internal fragmentation for odd sizes |
| deep per-class TCache (512 KiB target) | full-range speed, strong churn/cross | up to 28 MiB normal / 283 MiB all classes per thread |
| 8 central-bin shards | 28-thread same-class refill/flush does not serialize | ~613 KiB metadata; stealing/extra shard logic |
| flat class_map | normal-class free drops two dependent loads | 16 MiB VA per arena; must track page releases |
| bounded empty-span retention | cross-thread reuse without page churn | retained RSS until `hpm_trim` |
| 64 MiB/arena large cache | repeated 4/16/64 MiB use avoids syscalls/faults | retained address space and large-path lock work |
| 100% realloc slack | vector growth avoids a copy | short-term RSS up to 2x; still bounded by class |
| THP auto / `MADV_FREE` | small extents avoid huge-page inflation; cheap release | host/THP dependent; RSS can stay high |
| seven TUs, C++23, CMake 3.20 | readable layout, broad toolchain | cross-TU inlining is limited; TLS declarations matter |

## Build

Requirements: Linux x86-64, CMake ≥3.20, a C++23 compiler (GCC 11+ or Clang 16+), Ninja or Make.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Products:
- `build/libhpmalloc.so` (soname `libhpmalloc.so.1`)
- `build/libhpmalloc.a`
- `build/hpmalloc_basic_test`
- `build/hpmalloc_preload_cpp_test`
- `build/hpmalloc_bench` (allocator-agnostic benchmark; not linked to hpmalloc)

## Use

```sh
LD_PRELOAD=$PWD/build/libhpmalloc.so your-program
```

It can also be linked as a normal C/C++ library.  The public header is `include/hpmalloc/hpmalloc.h`; standard `malloc`/`free`/`calloc`/`realloc`, aligned allocation functions and C++ `operator new/delete` are exported.

Runtime options can be supplied with:

```sh
HPMALLOC_OPTIONS="thp=auto,purge=free,arena_mb=65536" \
LD_PRELOAD=$PWD/build/libhpmalloc.so your-program
```

| option | values | effect |
|---|---|---|
| `thp` | `auto`, `always`, `never`, `collapse` | small/large THP policy; `collapse` currently equals `always` |
| `purge` | `free`, `dontneed`, `defer` | page-return policy; `defer` currently equals `free` |
| `arena_mb` | decimal MB, ≥4 | initial arena size (before init) |
| `class_map` | `1`/`on`, `0`/`off` | enable the flat normal-class map |
| `realloc_direct_kb` | decimal KB; 0 disables | promote growing small reallocs to direct mappings |
| `realloc_slack_pct` | 0–400; default 100 | over-allocation percentage on growing realloc |

## Tests

```sh
ctest --test-dir build --output-on-failure
```

CTest runs:
- `hpmalloc_basic` — exhaustive size-class/API/alignment/realloc/calloc/C++ new/thread and cross-thread free checks.
- `hpmalloc_preload_cpp` — C++ new/delete, aligned new, containers, threads, cross-thread free and fork under `LD_PRELOAD`.
- `hpmalloc_compat_coreutils` — startup matrix plus functional checks for common GNU coreutils and file/shell/archive workflows.
- `hpmalloc_compat_common` — gcc/g++, Python, Git, CMake, SQLite, Perl and `RLIMIT_AS` under `LD_PRELOAD`.

The scripts can also be run directly:

```sh
bash tests/compat_coreutils.sh build/libhpmalloc.so
bash tests/compat_common.sh build/libhpmalloc.so
```

## Benchmark

The allocator-agnostic benchmark is built by default (`HPM_BUILD_BENCH=ON`):

```sh
cmake --build build --target hpmalloc_bench
bench/run_bench.sh --reps 8 --outdir /tmp/hpm_bench_manual
```

The script detects the online CPU count, selects an arithmetic thread sequence, rotates glibc/jemalloc/tcmalloc/mimalloc/hpmalloc order between repetitions, and uses fresh processes.  Use `--only`, `--max-threads`, and `--thread-step` for targeted runs.  The realloc suite includes the historical single-buffer row plus `realloc-multi` and `realloc-mixed` multi-buffer rows, so the comparison is not dominated by glibc's top-chunk advantage.  Workload definitions and caveats are in [Bench.md](docs/Bench.md).

## Note

Long-running fragmentation/RSS plateau, latency percentiles, full model-based stress, NUMA/background purge, mTHP/`MADV_COLLAPSE`, setuid and static-link hardening are still not implemented or tested yet.  
Treat this as a strong development-stage allocator, not a production-hardened drop-in for every workload.
