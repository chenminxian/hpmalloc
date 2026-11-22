# 分配器 benchmark

[English](Bench.md) | **简体中文**

`bench/alloc_bench.cpp` 与具体分配器无关：它只调用标准 `malloc` 家族，运行时通过 `LD_PRELOAD` 选择分配器。`bench/run_bench.sh` 按轮换顺序交错运行 glibc、jemalloc、tcmalloc、mimalloc 和 hpmalloc，每个 workload 使用独立进程；`bench/summarize.py` 输出 Markdown 表格。

这些脚本不会下载或安装分配器，只使用机器上已有的库。可用 `HPMALLOC_LIB`、`JEMALLOC_LIB`、`TCMALLOC_LIB`、`MIMALLOC_LIB` 覆盖路径。

## 构建

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hpmalloc_bench
```

## 运行

```sh
# 全量：10 次进程重复，0.6 s 测量 + 0.2 s 预热。
# 线程数根据在线 CPU 数自动选择。
bench/run_bench.sh

# 换一个 hpmalloc 构建或选项。
HPMALLOC_LIB=$PWD/build-clang/libhpmalloc.so \
HPM_OPTIONS=thp=never \
bench/run_bench.sh --outdir bench/results/clang-thp-never

# 单个或正则筛选的 workload，减少重复做 smoke test。
bench/run_bench.sh --only 'churn-small|large-4m' --reps 3 --seconds 1.0

# 覆盖自动线程序列。
bench/run_bench.sh --max-threads 32 --thread-step 4 --reps 7

# 汇总已有 raw 数据。
python3 -I bench/summarize.py bench/results/raw.csv \
    --out bench/results/summary.md
```

默认 `--reps` 为 10：足以降低这类机器上 CPU 频率和调度抖动的影响。只在 smoke test 或迭代分配器时用 3；最终数字用更多重复。全量运行时间随 reps 和线程点增加，高核机器上用 `--only` 做定向比较。

`run_bench.sh` 用 `nproc` 读取在线 CPU 数（失败时回退 `getconf _NPROCESSORS_ONLN`）。`MAX_THREADS` 默认为该值，`THREAD_STEP` 默认为 `ceil(MAX_THREADS / 10)`，得到约 10 个从 1 到硬件并发的点。例如 28 CPU 主机的 churn 序列是 `1,4,7,10,13,16,19,22,25,28`，cross 序列是 `2,6,10,14,18,22,26,28`（cross worker 必须为偶数）。最后一个端点不在等差序列时会追加。

`churn-*` 和 `cross-*` 使用完整序列；`fixed-*`、`realloc-*` 和多线程 `large-*` 只用 1 线程和最大线程端点，以控制高核机器的运行时间。需要不同范围时设置 `--max-threads` 和 `--thread-step`。

输出：
- `bench/results/raw.csv`：每次调用一行
- `bench/results/metadata.txt`：主机、库、选项、哈希和自动线程序列
- `bench/results/errors.log`：失败调用
- `bench/results/summary.md`：中位吞吐、peak RSS、波动和聚合比值

## 工作负载

| tag | 测量内容 |
|---|---|
| `pair-SIZE` | malloc(size) + 触碰首字节 + free；tcache 快路径 |
| `fixed-SIZE` | 在 1024（64 KiB 为 256）个存活对象中替换一个 |
| `churn-*` | 每个循环替换一个对象；大小在区间内 log-uniform |
| `cross-*` | producer 分配、consumer 释放；队列 256 个指针 |
| `realloc-*` | 单 buffer 从 16 B 按 1.5× 增长到 max；glibc top-chunk 优势 |
| `realloc-multi-*` | 每 worker 多个 live buffer，按 1.5× 增长后重置；无 top-chunk 优势 |
| `realloc-mixed-*` | 多 buffer，在 [min,max] 内随机增长/缩小；覆盖 shrink |
| `large-*` | 一次大对象 alloc/free；`pages` 触碰每个 4 KiB 页 |
| `calloc-*` | calloc + 读首字节（必须为零）+ free；lazy zero 可见 |

`pair`、`fixed`、`churn`、`cross`、`large`、`calloc` 每循环计两个分配器入口；`realloc`、`realloc-multi`、`realloc-mixed` 每次 realloc 计一个。summary 使用 allocator API calls/s；raw CSV 的 `ns_per_cycle` 是循环时间。`realloc-multi`/`realloc-mixed` 的 `live` 是每 worker 的 live buffer 数。

绝对速率随 CPU 频率和调度变化；`summarize.py` 因此计算每次重复内相对 glibc 的配对比值并取中位数。应使用这些比值，而不是两个独立中位数的商。
