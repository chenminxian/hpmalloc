#!/usr/bin/env bash
# Compare hpmalloc with the allocators already installed on this machine.
# No network access or package installation is used.
#
# Usage:
#   bench/run_bench.sh [--reps N] [--seconds F] [--warmup F] [--only REGEX]
#                      [--outdir DIR] [--hpm-options STR]
#                      [--max-threads N] [--thread-step N]
#
# Libraries can be overridden with HPMALLOC_LIB, JEMALLOC_LIB, TCMALLOC_LIB,
# MIMALLOC_LIB.
# MAX_THREADS defaults to the online hardware concurrency; THREAD_STEP is
# derived from it, so the thread-scaling workloads adapt to the host.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BENCH=${BENCH:-$ROOT/build/hpmalloc_bench}
OUTDIR=${OUTDIR:-$ROOT/bench/results}
HPMALLOC_LIB=${HPMALLOC_LIB:-$ROOT/build/libhpmalloc.so}
JEMALLOC_LIB=${JEMALLOC_LIB:-/usr/lib/libjemalloc.so}
TCMALLOC_LIB=${TCMALLOC_LIB:-/usr/lib/libtcmalloc_minimal.so}
MIMALLOC_LIB=${MIMALLOC_LIB:-/usr/lib/libmimalloc.so}
REPS=${REPS:-10}
SECS=${SECS:-0.6}
WARM=${WARM:-0.2}
ONLY=${ONLY:-}
HPM_OPTIONS=${HPM_OPTIONS:-}
MAX_THREADS=${MAX_THREADS:-}
THREAD_STEP=${THREAD_STEP:-}

while (($#)); do
    case "$1" in
        --reps) REPS=$2; shift 2 ;;
        --seconds) SECS=$2; shift 2 ;;
        --warmup) WARM=$2; shift 2 ;;
        --only) ONLY=$2; shift 2 ;;
        --outdir) OUTDIR=$2; shift 2 ;;
        --hpm-options) HPM_OPTIONS=$2; shift 2 ;;
        --max-threads) MAX_THREADS=$2; shift 2 ;;
        --thread-step) THREAD_STEP=$2; shift 2 ;;
        -h|--help)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *) echo "run_bench.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

# Hardware concurrency.  nproc respects the current CPU affinity, which is
# what the benchmark workers can actually use.
NPROC=$(nproc 2>/dev/null || true)
if [[ ! "$NPROC" =~ ^[0-9]+$ ]] || (( 10#$NPROC < 1 )); then
    NPROC=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
fi
if [[ ! "$NPROC" =~ ^[0-9]+$ ]] || (( 10#$NPROC < 1 )); then
    NPROC=1
fi
NPROC=$((10#$NPROC))

if [[ -z "$MAX_THREADS" ]]; then
    MAX_THREADS=$NPROC
fi
if [[ ! "$MAX_THREADS" =~ ^[0-9]+$ ]] || (( 10#$MAX_THREADS < 1 )); then
    echo "run_bench.sh: --max-threads must be a positive integer" >&2
    exit 2
fi
MAX_THREADS=$((10#$MAX_THREADS))

if [[ -z "$THREAD_STEP" ]]; then
    # About ten points from 1 to MAX_THREADS
    THREAD_STEP=$(( (MAX_THREADS + 9) / 10 ))
fi
if [[ ! "$THREAD_STEP" =~ ^[0-9]+$ ]] || (( 10#$THREAD_STEP < 1 )); then
    echo "run_bench.sh: --thread-step must be a positive integer" >&2
    exit 2
fi
THREAD_STEP=$((10#$THREAD_STEP))

if [[ ! "$REPS" =~ ^[0-9]+$ ]] || (( 10#$REPS < 1 )); then
    echo "run_bench.sh: --reps must be a positive integer" >&2
    exit 2
fi
REPS=$((10#$REPS))

# Print an arithmetic sequence start, start+step, ... <= max, and append max
# when it is not already on the sequence.  Keeping the endpoint makes the
# final row comparable with the hardware's full concurrency.
thread_seq() {
    local start=$1 step=$2 max=$3 t
    t=$start
    while (( t <= max )); do
        printf '%s\n' "$t"
        t=$((t + step))
    done
    if (( t - step != max )); then
        printf '%s\n' "$max"
    fi
}

THREAD_SEQ=()
mapfile -t THREAD_SEQ < <(thread_seq 1 "$THREAD_STEP" "$MAX_THREADS")

# cross-* requires at least two workers and an even worker count.
CROSS_THREAD_SEQ=()
CROSS_MAX=$(( MAX_THREADS - MAX_THREADS % 2 ))
if (( CROSS_MAX >= 2 )); then
    CROSS_STEP=$(( THREAD_STEP + THREAD_STEP % 2 ))
    mapfile -t CROSS_THREAD_SEQ < <(thread_seq 2 "$CROSS_STEP" "$CROSS_MAX")
fi

for f in "$BENCH" "$HPMALLOC_LIB" "$JEMALLOC_LIB" "$TCMALLOC_LIB" "$MIMALLOC_LIB"; do
    if [[ ! -e "$f" ]]; then
        echo "run_bench.sh: missing $f" >&2
        exit 1
    fi
done

mkdir -p "$OUTDIR"
RAW=$OUTDIR/raw.csv
ERR=$OUTDIR/errors.log
META=$OUTDIR/metadata.txt
FAILS=0
CFG_INDEX=0

printf 'label,tag,scenario,touch,threads,size,min_size,max_size,live,seconds,cycles,calls,cycles_per_s,calls_per_s,ns_per_cycle,bytes,bytes_per_s,peak_rss_kb,rss_kb,user_s,sys_s,minflt,majflt\n' >"$RAW"
: >"$ERR"

{
    echo "date: $(date -Is)"
    echo "kernel: $(uname -srmo)"
    echo "cpu: $(LC_ALL=C lscpu | sed -n 's/^Model name:[[:space:]]*//p' | head -1)"
    echo "nproc: $NPROC"
    echo "max_threads: $MAX_THREADS"
    echo "thread_step: $THREAD_STEP"
    echo "thread_sequence: ${THREAD_SEQ[*]}"
    if (( ${#CROSS_THREAD_SEQ[@]} )); then
        echo "cross_thread_sequence: ${CROSS_THREAD_SEQ[*]}"
    else
        echo "cross_thread_sequence: <none>"
    fi
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "thp: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo unknown)"
    echo "libc: $(getconf GNU_LIBC_VERSION 2>/dev/null || echo unknown)"
    echo "compiler: $(c++ --version 2>/dev/null | head -1)"
    echo "benchmark: $BENCH"
    echo "hpmalloc_lib: $HPMALLOC_LIB"
    echo "hpmalloc_lib_real: $(readlink -f "$HPMALLOC_LIB")"
    echo "hpmalloc_options: ${HPM_OPTIONS:-<default>}"
    echo "jemalloc_lib: $JEMALLOC_LIB"
    echo "jemalloc_lib_real: $(readlink -f "$JEMALLOC_LIB")"
    echo "tcmalloc_lib: $TCMALLOC_LIB"
    echo "tcmalloc_lib_real: $(readlink -f "$TCMALLOC_LIB")"
    echo "mimalloc_lib: $MIMALLOC_LIB"
    echo "mimalloc_lib_real: $(readlink -f "$MIMALLOC_LIB")"
    echo "reps: $REPS seconds: $SECS warmup: $WARM"
    echo "jemalloc_version: $(jemalloc-config --version 2>/dev/null || strings "$JEMALLOC_LIB" 2>/dev/null | grep -m1 -E '^[0-9]+\.[0-9]+\.[0-9]+' || echo unknown)"
    echo "tcmalloc_version: $(pkg-config --modversion libtcmalloc_minimal 2>/dev/null || echo unknown)"
    echo "mimalloc_version: $(pkg-config --modversion mimalloc 2>/dev/null || strings "$MIMALLOC_LIB" 2>/dev/null | grep -m1 -E '^[0-9]+\.[0-9]+\.[0-9]+' || echo unknown)"
    echo "sha256:"
    sha256sum "$BENCH" "$HPMALLOC_LIB" "$JEMALLOC_LIB" "$TCMALLOC_LIB" "$MIMALLOC_LIB" 2>/dev/null | sed 's/^/  /'
} >"$META"

allocators=(glibc jemalloc tcmalloc mimalloc hpmalloc)
declare -A libs=(
    [glibc]=""
    [jemalloc]="$JEMALLOC_LIB"
    [tcmalloc]="$TCMALLOC_LIB"
    [mimalloc]="$MIMALLOC_LIB"
    [hpmalloc]="$HPMALLOC_LIB"
)

run_allocator() {
    local label=$1
    shift
    local -a e=(env -u LD_PRELOAD -u MALLOC_CONF -u GLIBC_TUNABLES
                    -u HPMALLOC_OPTIONS -u TCMALLOC_RELEASE_RATE
                    -u TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES)
    if [[ -n "${libs[$label]}" ]]; then
        e+=("LD_PRELOAD=${libs[$label]}")
    fi
    if [[ "$label" == hpmalloc && -n "$HPM_OPTIONS" ]]; then
        e+=("HPMALLOC_OPTIONS=$HPM_OPTIONS")
    fi
    timeout 30s "${e[@]}" "$BENCH" "$@"
}

run_cfg() {
    local tag=$1
    shift
    if [[ -n "$ONLY" && ! "$tag" =~ $ONLY ]]; then
        return 0
    fi
    printf '%-28s ' "$tag" >&2
    local rep i offset label out nlines
    for ((rep = 1; rep <= REPS; rep++)); do
        offset=$(( (rep + CFG_INDEX) % ${#allocators[@]} ))
        for ((i = 0; i < ${#allocators[@]}; i++)); do
            label=${allocators[$(( (offset + i) % ${#allocators[@]} ))]}
            if ! out=$(run_allocator "$label" "$@" --seconds "$SECS" --warmup "$WARM" \
                        --tag "$tag" --label "$label" 2>>"$ERR"); then
                echo "FAILED $tag $label" >>"$ERR"
                printf '!' >&2
                FAILS=$((FAILS + 1))
                continue
            fi
            nlines=$(printf '%s\n' "$out" | wc -l)
            if [[ "$nlines" -ne 1 ]]; then
                echo "BAD OUTPUT $tag $label: $nlines lines" >>"$ERR"
                printf 'B' >&2
                FAILS=$((FAILS + 1))
                continue
            fi
            printf '%s\n' "$out" >>"$RAW"
            printf '.' >&2
        done
    done
    printf '\n' >&2
    CFG_INDEX=$((CFG_INDEX + 1))
}

run_cfg pair-16        --scenario pair --size 16 --threads 1
run_cfg pair-64        --scenario pair --size 64 --threads 1
run_cfg pair-256       --scenario pair --size 256 --threads 1
run_cfg pair-1024      --scenario pair --size 1024 --threads 1
run_cfg pair-4096      --scenario pair --size 4096 --threads 1
run_cfg pair-16384     --scenario pair --size 16384 --threads 1
run_cfg pair-65536     --scenario pair --size 65536 --threads 1
run_cfg pair-1048576   --scenario pair --size 1048576 --threads 1

run_cfg fixed-64-t1    --scenario fixed --size 64 --live 1024 --threads 1
if (( MAX_THREADS > 1 )); then
    run_cfg "fixed-64-t${MAX_THREADS}" \
        --scenario fixed --size 64 --live 1024 --threads "$MAX_THREADS"
fi
run_cfg fixed-4096-t1  --scenario fixed --size 4096 --live 1024 --threads 1
if (( MAX_THREADS > 1 )); then
    run_cfg "fixed-4096-t${MAX_THREADS}" \
        --scenario fixed --size 4096 --live 1024 --threads "$MAX_THREADS"
fi
run_cfg fixed-65536-t1 --scenario fixed --size 65536 --live 256 --threads 1

# Churn and cross use the full arithmetic sequence; fixed, realloc and large
# use the two endpoints to keep the suite bounded on high-core-count hosts.
for t in "${THREAD_SEQ[@]}"; do
    run_cfg "churn-small-t${t}" \
        --scenario churn --min-size 16 --max-size 4096 --live 4096 --threads "$t"
done
for t in "${THREAD_SEQ[@]}"; do
    run_cfg "churn-mixed-t${t}" \
        --scenario churn --min-size 16 --max-size 65536 --live 512 --threads "$t"
done

if (( ${#CROSS_THREAD_SEQ[@]} )); then
    for t in "${CROSS_THREAD_SEQ[@]}"; do
        run_cfg "cross-small-t${t}" \
            --scenario cross --min-size 16 --max-size 4096 --threads "$t"
    done
    for t in "${CROSS_THREAD_SEQ[@]}"; do
        run_cfg "cross-mixed-t${t}" \
            --scenario cross --min-size 16 --max-size 65536 --threads "$t"
    done
else
    echo "run_bench.sh: skipping cross-* (need at least 2 CPUs)" >&2
fi

# Single-buffer realloc: glibc can often extend the top chunk in place.
# Keep it as a historical/contrast row, not as the only realloc result.
run_cfg realloc-256k-t1 --scenario realloc --min-size 16 --max-size 262144 --threads 1
if (( MAX_THREADS > 1 )); then
    run_cfg "realloc-256k-t${MAX_THREADS}" \
        --scenario realloc --min-size 16 --max-size 262144 --threads "$MAX_THREADS"
fi
run_cfg realloc-1m-t1   --scenario realloc --min-size 16 --max-size 1048576 --threads 1

# Multi-buffer realloc: keep 64 (256 KiB) or 16 (1 MiB) buffers live per
# worker and grow/shrink them in turn.  The interleaving removes glibc's
# top-chunk advantage; realloc-mixed additionally covers shrink and random
# grow/shrink transitions.
run_cfg realloc-multi-4k-t1 \
    --scenario realloc-multi --min-size 16 --max-size 4096 --live 256 --threads 1
run_cfg realloc-multi-64k-t1 \
    --scenario realloc-multi --min-size 16 --max-size 65536 --live 128 --threads 1
run_cfg realloc-mixed-64k-t1 \
    --scenario realloc-mixed --min-size 16 --max-size 65536 --live 128 --threads 1
run_cfg realloc-multi-256k-t1 \
    --scenario realloc-multi --min-size 16 --max-size 262144 --live 64 --threads 1
run_cfg realloc-mixed-256k-t1 \
    --scenario realloc-mixed --min-size 16 --max-size 262144 --live 64 --threads 1
run_cfg realloc-multi-1m-t1 \
    --scenario realloc-multi --min-size 16 --max-size 1048576 --live 16 --threads 1
if (( MAX_THREADS > 1 )); then
    run_cfg "realloc-multi-64k-t${MAX_THREADS}" \
        --scenario realloc-multi --min-size 16 --max-size 65536 --live 128 --threads "$MAX_THREADS"
    run_cfg "realloc-mixed-64k-t${MAX_THREADS}" \
        --scenario realloc-mixed --min-size 16 --max-size 65536 --live 128 --threads "$MAX_THREADS"
    run_cfg "realloc-multi-256k-t${MAX_THREADS}" \
        --scenario realloc-multi --min-size 16 --max-size 262144 --live 64 --threads "$MAX_THREADS"
    run_cfg "realloc-mixed-256k-t${MAX_THREADS}" \
        --scenario realloc-mixed --min-size 16 --max-size 262144 --live 64 --threads "$MAX_THREADS"
    run_cfg "realloc-multi-1m-t${MAX_THREADS}" \
        --scenario realloc-multi --min-size 16 --max-size 1048576 --live 16 --threads "$MAX_THREADS"
fi

run_cfg large-1m-pages-t1  --scenario large --size 1048576 --live 2 --threads 1 --touch pages
run_cfg large-4m-first-t1  --scenario large --size 4194304 --live 2 --threads 1 --touch first
run_cfg large-4m-pages-t1  --scenario large --size 4194304 --live 2 --threads 1 --touch pages
if (( MAX_THREADS > 1 )); then
    run_cfg "large-4m-pages-t${MAX_THREADS}" \
        --scenario large --size 4194304 --live 2 --threads "$MAX_THREADS" --touch pages
fi
run_cfg large-16m-first-t1 --scenario large --size 16777216 --live 1 --threads 1 --touch first
run_cfg large-16m-pages-t1 --scenario large --size 16777216 --live 1 --threads 1 --touch pages
if (( MAX_THREADS > 1 )); then
    run_cfg "large-16m-pages-t${MAX_THREADS}" \
        --scenario large --size 16777216 --live 1 --threads "$MAX_THREADS" --touch pages
fi
run_cfg large-64m-pages-t1 --scenario large --size 67108864 --live 1 --threads 1 --touch pages

run_cfg calloc-64k-t1  --scenario calloc --size 65536 --threads 1
run_cfg calloc-1m-t1   --scenario calloc --size 1048576 --threads 1
run_cfg calloc-16m-t1  --scenario calloc --size 16777216 --threads 1

if ((FAILS)); then
    echo "run_bench.sh: $FAILS invocation(s) failed; see $ERR" >&2
    exit 1
fi

echo "raw data: $RAW" >&2
echo "metadata: $META" >&2
