#!/usr/bin/env python3
"""Summarize alloc_bench raw CSV as Markdown."""

import argparse
import csv
import math
import os
import re
import statistics
import sys

LABELS = ["glibc", "jemalloc", "tcmalloc", "mimalloc", "hpmalloc"]


def rate(v):
    if v >= 1e9:
        return f"{v / 1e9:.2f}G"
    if v >= 1e6:
        return f"{v / 1e6:.2f}M"
    if v >= 1e3:
        return f"{v / 1e3:.2f}k"
    return f"{v:.1f}"


def ratio(a, b):
    if b <= 0:
        return "-"
    return f"{a / b:.2f}x"


def mb(kb):
    return f"{kb / 1024.0:.1f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="?", default="bench/results/raw.csv")
    ap.add_argument("--out", default="bench/results/summary.md")
    ap.add_argument("--meta", default=None)
    args = ap.parse_args()

    meta_path = args.meta
    if meta_path is None:
        meta_path = os.path.join(os.path.dirname(args.csv), "metadata.txt")

    rows = []
    failures = []
    with open(args.csv, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("scenario") in (None, "", "FAIL"):
                failures.append(row)
                continue
            try:
                row["calls_per_s"] = float(row["calls_per_s"])
                row["cycles_per_s"] = float(row["cycles_per_s"])
                row["peak_rss_kb"] = float(row["peak_rss_kb"])
                row["rss_kb"] = float(row["rss_kb"])
                row["calls"] = int(row["calls"])
                row["cycles"] = int(row["cycles"])
                row["seconds"] = float(row["seconds"])
            except (KeyError, ValueError):
                failures.append(row)
                continue
            rows.append(row)

    if not rows:
        sys.stderr.write("no usable rows\n")
        return 1

    tags = []
    for r in rows:
        if r["tag"] not in tags:
            tags.append(r["tag"])

    by = {}
    by_tag = {}
    for r in rows:
        by.setdefault((r["tag"], r["label"]), []).append(r)
        by_tag.setdefault(r["tag"], []).append(r)

    def med(tag, label, field):
        vals = [x[field] for x in by.get((tag, label), [])]
        if not vals:
            return None
        return statistics.median(vals)

    def rep_chunks(tag):
        """Driver writes all LABELS in one repetition, then the next repetition."""
        xs = by_tag.get(tag, [])
        if len(xs) % len(LABELS):
            return []
        out = []
        for i in range(0, len(xs), len(LABELS)):
            d = {r["label"]: r for r in xs[i:i + len(LABELS)]}
            if set(d) == set(LABELS):
                out.append(d)
        return out

    def paired_ratio_med(tag, num, den):
        vals = []
        for d in rep_chunks(tag):
            try:
                a = d[num]["calls_per_s"]
                b = d[den]["calls_per_s"]
            except KeyError:
                continue
            if b > 0:
                vals.append(a / b)
        return statistics.median(vals) if vals else None

    lines = []
    if os.path.exists(meta_path):
        lines.append("### Environment\n")
        with open(meta_path) as f:
            for line in f:
                line = line.rstrip()
                if line:
                    lines.append(f"- `{line}`")
        lines.append("")

    lines.append("### Throughput (median allocator API calls/s)\n")
    lines.append("Each call is one allocator entry point.  `pair`, `fixed`, `churn`, "
                 "`cross`, `large`, and `calloc` have two calls per cycle; "
                 "`realloc` counts one call per realloc invocation.  "
                 "Higher is better.  Absolute rates vary with CPU frequency and "
                 "scheduling; the ratio columns use paired repetitions (the five "
                 "allocators back-to-back within one repetition) and then take the "
                 "median ratio, which removes most of that drift.\n")
    lines.append("| workload | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc | hpm/tcmalloc | hpm/mimalloc |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for tag in tags:
        vals = {label: med(tag, label, "calls_per_s") for label in LABELS}
        if not any(vals.values()):
            continue
        cells = []
        for label in LABELS:
            cells.append(rate(vals[label]) if vals[label] is not None else "-")
        def pr(num, den):
            v = paired_ratio_med(tag, num, den)
            return f"{v:.2f}x" if v is not None else "-"
        lines.append(
            f"| {tag} | " + " | ".join(cells) +
            f" | {pr('hpmalloc', 'glibc')}"
            f" | {pr('hpmalloc', 'jemalloc')}"
            f" | {pr('hpmalloc', 'tcmalloc')}"
            f" | {pr('hpmalloc', 'mimalloc')} |"
        )
    lines.append("")

    # Thread-scaling families.  The driver selects an arithmetic sequence from
    # 1 (or 2 for cross) to the host's hardware concurrency; fixed/realloc/large
    # keep only the endpoint rows.  Use the lowest tested count as the base so
    # cross (which starts at 2) works without special cases.
    scaling_note = (
        "Same workload and live set as threads increase; values are aggregate "
        "calls/s divided by the lowest thread count tested for that allocator. "
        "The design target in DESIGN.md is expressed at eight threads; the "
        "host's auto-selected sequence may go above or below that."
    )

    def scaling_section(prefix, title):
        subset = [t for t in tags
                  if re.fullmatch(re.escape(prefix) + r"-t\d+", t)]
        if len(subset) < 2:
            return
        subset.sort(key=lambda t: int(t.rsplit("-t", 1)[1]))
        base_tag = subset[0]
        lines.append(f"### {title}\n")
        lines.append(scaling_note + "\n")
        lines.append("| threads | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc |")
        lines.append("|---:|---:|---:|---:|---:|---:|")
        for tag in subset:
            nthr = tag.rsplit("-t", 1)[1]
            cells = []
            for label in LABELS:
                v = med(tag, label, "calls_per_s")
                base = med(base_tag, label, "calls_per_s")
                cells.append(f"{v / base:.2f}x" if v is not None and base else "-")
            lines.append(f"| {nthr} | " + " | ".join(cells) + " |")
        lines.append("")

    scaling_section("churn-small", "Churn-small thread scaling")
    scaling_section("churn-mixed", "Churn-mixed thread scaling")
    scaling_section("cross-small", "Cross-small thread scaling")
    scaling_section("cross-mixed", "Cross-mixed thread scaling")

    lines.append("### Peak RSS (MiB, median)\n")
    lines.append("Peak RSS includes the process, worker stacks, allocator metadata, "
                 "and live workload.  It is not a fragmentation measurement by itself.\n")
    lines.append("| workload | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/mimalloc |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|")
    for tag in tags:
        vals = {label: med(tag, label, "peak_rss_kb") for label in LABELS}
        if not any(vals.values()):
            continue
        cells = [mb(vals[label]) if vals[label] is not None else "-" for label in LABELS]
        h = vals["hpmalloc"]
        g = vals["glibc"]
        mi = vals["mimalloc"]
        lines.append(
            f"| {tag} | " + " | ".join(cells) +
            f" | {ratio(h, g) if h is not None and g else '-'}"
            f" | {ratio(h, mi) if h is not None and mi else '-'} |"
        )
    lines.append("")

    # Run-to-run variability: CV of calls/s, per allocator across all rows.
    lines.append("### Run-to-run variability\n")
    lines.append("Coefficient of variation of calls/s across the repetitions; "
                 "lower is better.  Few repetitions give only a rough stability "
                 "indicator; use at least 7-10 for final numbers.\n")
    lines.append("| allocator | median CV | max CV | max-CV workload |")
    lines.append("|---|---:|---:|---|")
    for label in LABELS:
        cvs = []
        max_cv = -1.0
        max_tag = "-"
        for tag in tags:
            vals = [x["calls_per_s"] for x in by.get((tag, label), [])]
            if len(vals) < 2:
                continue
            mean = statistics.fmean(vals)
            if mean <= 0:
                continue
            cv = statistics.pstdev(vals) / mean
            cvs.append(cv)
            if cv > max_cv:
                max_cv = cv
                max_tag = tag
        if cvs:
            lines.append(f"| {label} | {statistics.median(cvs) * 100:.1f}% | "
                         f"{max_cv * 100:.1f}% | {max_tag} |")
        else:
            lines.append(f"| {label} | - | - | - |")
    lines.append("")

    # Unweighted geometric mean over the pair/fixed/churn workloads.
    lines.append("### Paired-ratio variability vs glibc\n")
    lines.append("Coefficient of variation of the per-repetition ratio to glibc, "
                 "median and maximum across workloads.  This is the stability of "
                 "the comparison after cancelling most machine-level drift.\n")
    lines.append("| allocator | median CV of ratio | max CV of ratio | max-CV workload |")
    lines.append("|---|---:|---:|---|")
    for label in ("jemalloc", "tcmalloc", "mimalloc", "hpmalloc"):
        cvs = []
        max_cv = -1.0
        max_tag = "-"
        for tag in tags:
            ratios = []
            for d in rep_chunks(tag):
                try:
                    a = d[label]["calls_per_s"]
                    b = d["glibc"]["calls_per_s"]
                except KeyError:
                    continue
                if b > 0:
                    ratios.append(a / b)
            if len(ratios) < 2:
                continue
            mean = statistics.fmean(ratios)
            if mean <= 0:
                continue
            cv = statistics.pstdev(ratios) / mean
            cvs.append(cv)
            if cv > max_cv:
                max_cv = cv
                max_tag = tag
        if cvs:
            lines.append(f"| {label} | {statistics.median(cvs) * 100:.1f}% | "
                         f"{max_cv * 100:.1f}% | {max_tag} |")
        else:
            lines.append(f"| {label} | - | - | - |")
    lines.append("")

    lines.append("### Aggregate pair/fixed/churn throughput\n")
    lines.append("Unweighted geometric mean of paired calls/s ratios over all "
                 "`pair-*`, `fixed-*`, and `churn-*` rows.  This includes 1 MiB "
                 "`pair` rows and is therefore not a pure small-object metric.  "
                 "Because churn covers several thread counts, its rows carry more "
                 "weight than pair/fixed rows.  It is a summary, not a single "
                 "acceptance criterion; the per-workload table is authoritative.\n")
    lines.append("| comparison | geometric mean ratio |")
    lines.append("|---|---:|")
    selected = [t for t in tags if t.startswith(("pair-", "fixed-", "churn-"))]
    for label in ("jemalloc", "tcmalloc", "mimalloc", "hpmalloc"):
        ratios = []
        for tag in selected:
            for d in rep_chunks(tag):
                try:
                    a = d[label]["calls_per_s"]
                    b = d["glibc"]["calls_per_s"]
                except KeyError:
                    continue
                if b > 0:
                    ratios.append(a / b)
        if ratios:
            gm = math.exp(sum(math.log(x) for x in ratios) / len(ratios))
            lines.append(f"| {label}/glibc | {gm:.2f}x |")
    lines.append("")

    lines.append("### Notes\n")
    lines.append("- `pair`: malloc + free of the same size, one live object; the "
                 "fastest possible tcache path.")
    lines.append("- `fixed` / `churn`: replace one object in a live set per cycle; "
                 "`churn` draws log-uniform sizes in the given range.")
    lines.append("- `cross`: producer thread allocates, consumer thread frees.")
    lines.append("- `realloc`: single buffer grown from min-size to max-size in "
                 "1.5x steps; glibc can often extend the top chunk in place, so "
                 "this row is a contrast case rather than a general realloc metric.")
    lines.append("- `realloc-multi`: several live buffers per worker, grown in "
                 "1.5x steps and reset to the minimum after reaching max; "
                 "interleaving removes glibc's top-chunk advantage.")
    lines.append("- `realloc-mixed`: several live buffers, each reallocated to a "
                 "random size in [min,max]; covers shrink and random grow/shrink, "
                 "and reports one allocator call per realloc invocation.")
    lines.append("- `large`: one large allocate/free cycle; `pages` writes one byte "
                 "per 4 KiB page, `first` writes only the first byte.")
    lines.append("- `calloc`: calloc + first-byte read/write + free; no full-buffer "
                 "touch, so lazy-zero behavior is visible.")
    lines.append("- The benchmark clears `LD_PRELOAD` for glibc and uses absolute "
                 "paths for the other four libraries; no allocator is downloaded "
                 "or installed by this suite.")
    lines.append("")

    if failures:
        lines.append(f"### Failed rows\n")
        lines.append(f"{len(failures)} invocation(s) had unusable output; see "
                     f"`{os.path.join(os.path.dirname(args.csv), 'errors.log')}`.\n")

    text = "\n".join(lines)
    with open(args.out, "w") as f:
        f.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
