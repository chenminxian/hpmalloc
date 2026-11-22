# hpmalloc 性能对比

[English](Performance.md) | **简体中文**

本文报告两台机器的两轮全量测试：8 线程笔记本和 28 线程工作站。两轮都比较 hpmalloc 与发行版仓库安装的 glibc ptmalloc、jemalloc、tcmalloc 和 mimalloc。

## 1. 环境

| 项目 | 笔记本 | 工作站 |
| --- | --- | --- |
| CPU | Intel Core i7-1195G7 (4C/8T) | Intel Core i7-14700K (8P+12E/28T) |
| 内存 | 16GiB LPDDR4，3733MT/s | 48GiB DDR5，4200MT/s |
| 发行版 | Arch Linux (CachyOS x86-64-v4 仓库) | Ubuntu 22.04 |
| 内核 | Linux 7.3.0-4-cachyos x86-64-v4 | Linux 6.8.0-147-generic x86-64 |
| libc | glibc 2.44 | glibc 2.35 |
| hpmalloc 编译器 | GCC 16.2.1 | GCC 11.4.0 (Ubuntu 22.04) |
| CMake / Ninja | CMake 4.x / Ninja 1.13 | CMake 3.22.1 / Ninja 1.10.1 |
| jemalloc | 5.4.0 | 5.2.1 |
| tcmalloc | 4.6.10 | 4.5.9 |
| mimalloc | 3.5 | 2.0 |
| THP | `always` | `always` |
| CPU governor | `performance` | `performance` |
| reps × 测量/预热 | 8 × 0.6 s / 0.2 s | 12 × 0.6 s / 0.2 s |
| 线程序列 | 1,2,3,...,8 | 1,4,7,...,28 |
| cross 序列 | 2,4,6,8 | 2,6,10,...,28 |

两机的绝对 calls/s 不可直接比较。表中比值在**同一次进程重复内**计算（五个分配器连续运行、顺序逐轮轮换），再取中位数，可抵消大部分频率漂移。2-5% 的差异不要当作结论；以配对表和归档原始数据为准。

## 2. 方法

`bench/alloc_bench.cpp` 与分配器无关，通过 `LD_PRELOAD` 选择。每个 workload 使用独立进程，0.2 s 预热 + 0.6 s 测量；worker 绑定允许的 CPU。`bench/run_bench.sh` 每轮轮换五个分配器；`bench/summarize.py` 输出中位吞吐、配对比值、peak RSS 和波动。

分配器 API calls/s 的计数：
- `pair`、`fixed`、`churn`、`cross`、`large`、`calloc`：每个循环两次调用（分配 + 释放）
- `realloc`、`realloc-multi`、`realloc-mixed`：每次 realloc 调用计一次

工作负载：
- `pair-SIZE`：malloc + 触碰首字节 + free
- `fixed-SIZE`：在 1024（64 KiB 为 256）个存活对象中替换一个
- `churn-*`：每个循环替换一个对象，log-uniform 大小
- `cross-*`：producer 分配，consumer 释放，队列 256 个指针
- `realloc-*`：单 buffer 从 16 B 按 1.5× 增长（glibc 可原地扩展 top chunk， 这是对比工件）
- `realloc-multi-*`：每 worker 多个存活 buffer，增长后重置；消除 top-chunk优势
- `realloc-mixed-*`：多 buffer 随机增长/缩小
- `large-*`：一次大对象分配/释放；`pages` 触碰每个 4 KiB 页
- `calloc-*`：calloc + 读首字节（必须为零）+ free

28 核主机的自动序列有 10 个点；fixed/realloc/large 只保留 1 线程和最大线程端点。笔记本序列为 1..8。

## 3. 代表性结果

> 比值大于 1 表示 hpmalloc 更快。`hpm` 列是逐轮配对比值的中位数；绝对calls/s 是 hpmalloc 的中位吞吐，依赖主机。64 KiB/1 MiB 的极大比值（尤其 jemalloc、mimalloc）是重复 `pair` workload 下真实的 cache-tier 塌缩，不是格式错误。

**笔记本**

| 工作负载 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc | hpm/tcmalloc | hpm/mimalloc |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| pair-16 | 424.13M | 370.36M | 402.33M | 466.81M | 412.48M | 0.97x | 1.11x | 1.02x | 0.88x |
| pair-64 | 262.80M | 363.19M | 287.33M | 368.89M | 383.28M | 0.95x | 1.06x | 0.95x | 0.86x |
| pair-256 | 321.94M | 298.15M | 337.21M | 361.79M | 338.11M | 0.96x | 1.10x | 1.00x | 0.94x |
| pair-1024 | 266.13M | 288.79M | 275.15M | 248.95M | 376.15M | 0.93x | 1.30x | 1.38x | 1.02x |
| pair-4096 | 56.57M | 184.19M | 397.32M | 91.73M | 334.75M | 5.13x | 1.44x | 0.96x | 2.63x |
| pair-16384 | 54.58M | 96.18M | 248.39M | 118.20M | 295.65M | 4.74x | 3.48x | 0.89x | 2.96x |
| pair-65536 | 74.22M | 7.20M | 397.92M | 118.55M | 334.04M | 4.74x | 48.86x | 0.89x | 2.95x |
| pair-1048576 | 73.64M | 4.61M | 54.94M | 7.64M | 235.04M | 4.68x | 48.22x | 5.67x | 38.75x |
| fixed-64-t1 | 237.82M | 276.86M | 313.92M | 272.01M | 307.03M | 1.04x | 1.08x | 0.97x | 1.00x |
| fixed-64-t8 | 960.33M | 949.25M | 940.88M | 1.04G | 939.19M | 0.98x | 0.98x | 1.03x | 0.90x |
| fixed-4096-t1 | 49.00M | 211.51M | 197.85M | 19.25M | 224.00M | 3.10x | 1.11x | 0.80x | 10.97x |
| fixed-4096-t8 | 284.08M | 673.55M | 424.67M | 44.63M | 496.93M | 1.77x | 0.75x | 1.14x | 10.76x |
| fixed-65536-t1 | 74.07M | 8.91M | 226.78M | 25.07M | 155.50M | 3.24x | 22.43x | 0.79x | 9.10x |
| churn-small-t1 | 42.19M | 114.63M | 110.74M | 111.95M | 113.50M | 2.54x | 1.01x | 1.02x | 1.01x |
| churn-small-t2 | 84.76M | 210.34M | 211.09M | 168.63M | 215.11M | 2.54x | 1.02x | 1.02x | 1.29x |
| churn-small-t3 | 112.42M | 286.19M | 279.32M | 182.31M | 282.89M | 2.53x | 0.99x | 1.01x | 1.56x |
| churn-small-t4 | 139.08M | 347.29M | 339.75M | 202.44M | 344.55M | 2.48x | 0.99x | 1.02x | 1.70x |
| churn-small-t5 | 150.74M | 364.03M | 365.85M | 198.65M | 377.60M | 2.49x | 1.03x | 1.04x | 1.90x |
| churn-small-t6 | 162.91M | 377.24M | 386.47M | 191.79M | 406.35M | 2.49x | 1.08x | 1.04x | 2.12x |
| churn-small-t7 | 170.70M | 394.61M | 412.27M | 183.79M | 432.15M | 2.51x | 1.09x | 1.04x | 2.34x |
| churn-small-t8 | 178.30M | 406.31M | 429.92M | 180.99M | 454.64M | 2.56x | 1.12x | 1.06x | 2.52x |
| churn-mixed-t1 | 19.46M | 18.20M | 94.83M | 47.49M | 91.42M | 3.09x | 3.88x | 0.88x | 1.26x |
| churn-mixed-t2 | 54.28M | 43.08M | 194.89M | 121.41M | 167.70M | 3.10x | 3.90x | 0.87x | 1.39x |
| churn-mixed-t3 | 70.77M | 57.34M | 253.49M | 147.39M | 220.44M | 3.13x | 3.83x | 0.86x | 1.49x |
| churn-mixed-t4 | 86.98M | 70.27M | 313.08M | 172.99M | 277.66M | 3.18x | 3.95x | 0.89x | 1.59x |
| churn-mixed-t5 | 94.47M | 73.51M | 332.04M | 172.52M | 302.96M | 3.21x | 4.12x | 0.91x | 1.76x |
| churn-mixed-t6 | 100.42M | 76.07M | 325.32M | 171.37M | 320.59M | 3.25x | 4.38x | 0.99x | 1.88x |
| churn-mixed-t7 | 110.05M | 78.62M | 319.27M | 169.97M | 361.09M | 3.33x | 4.64x | 1.15x | 2.15x |
| churn-mixed-t8 | 116.11M | 82.11M | 295.77M | 171.78M | 390.66M | 3.37x | 4.78x | 1.32x | 2.28x |
| cross-small-t2 | 5.24M | 22.07M | 23.39M | 23.81M | 28.55M | 5.40x | 1.31x | 1.19x | 1.17x |
| cross-small-t4 | 7.67M | 31.09M | 26.49M | 35.18M | 43.19M | 5.62x | 1.35x | 1.60x | 1.21x |
| cross-small-t6 | 9.07M | 39.53M | 28.05M | 46.00M | 50.41M | 5.72x | 1.29x | 1.80x | 1.10x |
| cross-small-t8 | 11.00M | 45.10M | 30.29M | 60.07M | 65.13M | 5.81x | 1.43x | 2.15x | 1.10x |
| cross-mixed-t2 | 3.86M | 8.39M | 15.57M | 21.76M | 28.86M | 7.53x | 3.44x | 1.87x | 1.27x |
| cross-mixed-t4 | 5.92M | 11.70M | 12.03M | 33.67M | 43.40M | 7.30x | 3.67x | 3.57x | 1.28x |
| cross-mixed-t6 | 7.22M | 14.66M | 11.93M | 45.73M | 54.63M | 7.54x | 3.73x | 4.57x | 1.19x |
| cross-mixed-t8 | 8.06M | 16.74M | 11.94M | 54.21M | 66.10M | 8.22x | 3.92x | 5.54x | 1.22x |

**工作站**

| 工作负载 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc | hpm/tcmalloc | hpm/mimalloc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| pair-16 | 366.36M | 280.24M | 681.23M | 40.02M | 559.03M | 1.52x | 1.99x | 0.82x | 13.99x |
| pair-64 | 366.27M | 281.41M | 681.05M | 38.81M | 560.86M | 1.53x | 2.00x | 0.82x | 14.46x |
| pair-256 | 366.33M | 281.62M | 680.53M | 37.47M | 561.43M | 1.53x | 1.99x | 0.82x | 15.00x |
| pair-1024 | 366.67M | 281.33M | 680.08M | 33.14M | 529.24M | 1.44x | 1.87x | 0.78x | 15.95x |
| pair-4096 | 94.72M | 280.83M | 672.58M | 25.97M | 530.24M | 5.60x | 1.88x | 0.79x | 20.41x |
| pair-16384 | 94.74M | 200.45M | 673.09M | 13.41M | 480.01M | 5.08x | 2.40x | 0.72x | 35.81x |
| pair-65536 | 93.96M | 24.53M | 672.11M | 1.19M | 479.61M | 5.10x | 19.56x | 0.71x | 402.12x |
| pair-1048576 | 93.91M | 24.54M | 82.91M | 78.69k | 478.15M | 5.09x | 19.48x | 5.77x | 6075.37x |
| fixed-64-t1 | 297.35M | 305.06M | 446.54M | 41.17M | 399.81M | 1.34x | 1.31x | 0.89x | 9.81x |
| fixed-64-t28 | 3.80G | 4.51G | 5.72G | 596.68M | 5.49G | 1.44x | 1.22x | 0.96x | 9.21x |
| fixed-4096-t1 | 86.18M | 305.69M | 419.84M | 12.46M | 354.98M | 4.12x | 1.16x | 0.85x | 28.49x |
| fixed-4096-t28 | 1.26G | 1.37G | 1.30G | 19.15M | 1.31G | 1.05x | 0.96x | 1.01x | 68.69x |
| fixed-65536-t1 | 86.83M | 20.58M | 450.47M | 871.49k | 358.38M | 4.13x | 17.42x | 0.80x | 411.63x |
| churn-small-t1 | 47.56M | 120.25M | 143.93M | 18.66M | 122.06M | 2.57x | 1.02x | 0.85x | 6.54x |
| churn-small-t4 | 136.61M | 335.43M | 396.02M | 39.65M | 382.77M | 2.80x | 1.14x | 0.97x | 9.62x |
| churn-small-t7 | 247.78M | 613.13M | 709.37M | 52.37M | 681.60M | 2.75x | 1.11x | 0.96x | 12.81x |
| churn-small-t10 | 332.65M | 821.34M | 924.80M | 45.80M | 919.10M | 2.76x | 1.12x | 0.99x | 20.09x |
| churn-small-t13 | 440.35M | 1.08G | 1.21G | 48.77M | 1.19G | 2.70x | 1.09x | 0.99x | 24.38x |
| churn-small-t16 | 519.02M | 1.28G | 1.41G | 44.03M | 1.40G | 2.69x | 1.09x | 0.99x | 31.80x |
| churn-small-t19 | 592.24M | 1.45G | 1.56G | 42.09M | 1.57G | 2.65x | 1.08x | 1.01x | 37.18x |
| churn-small-t22 | 668.84M | 1.63G | 1.68G | 40.06M | 1.73G | 2.58x | 1.06x | 1.03x | 43.03x |
| churn-small-t25 | 734.46M | 1.81G | 1.79G | 38.36M | 1.89G | 2.58x | 1.05x | 1.06x | 49.49x |
| churn-small-t28 | 793.48M | 1.97G | 1.87G | 35.35M | 2.02G | 2.54x | 1.03x | 1.08x | 57.01x |
| churn-mixed-t1 | 32.02M | 32.75M | 131.28M | 3.88M | 98.42M | 3.07x | 3.01x | 0.75x | 25.46x |
| churn-mixed-t4 | 93.74M | 76.21M | 385.12M | 4.91M | 320.65M | 3.42x | 4.21x | 0.83x | 64.91x |
| churn-mixed-t7 | 168.62M | 133.97M | 518.67M | 4.86M | 553.95M | 3.28x | 4.12x | 1.07x | 114.11x |
| churn-mixed-t10 | 225.38M | 177.64M | 430.44M | 3.90M | 757.75M | 3.37x | 4.27x | 1.76x | 194.27x |
| churn-mixed-t13 | 290.96M | 233.34M | 302.43M | 3.78M | 973.82M | 3.36x | 4.17x | 3.22x | 255.82x |
| churn-mixed-t16 | 340.38M | 273.38M | 195.45M | 3.37M | 1.15G | 3.39x | 4.22x | 5.91x | 342.32x |
| churn-mixed-t19 | 382.60M | 319.43M | 131.27M | 3.37M | 1.27G | 3.33x | 3.98x | 9.77x | 377.92x |
| churn-mixed-t22 | 425.57M | 366.06M | 95.74M | 3.29M | 1.40G | 3.29x | 3.82x | 14.50x | 424.47x |
| churn-mixed-t25 | 460.48M | 411.45M | 73.74M | 3.21M | 1.52G | 3.30x | 3.69x | 20.48x | 473.80x |
| churn-mixed-t28 | 495.33M | 446.07M | 58.49M | 3.02M | 1.62G | 3.28x | 3.64x | 27.71x | 536.97x |
| cross-small-t2 | 9.24M | 41.07M | 70.78M | 17.03M | 51.82M | 5.59x | 1.26x | 0.73x | 3.03x |
| cross-small-t6 | 26.94M | 121.33M | 55.90M | 41.88M | 129.77M | 4.82x | 1.07x | 2.34x | 3.10x |
| cross-small-t10 | 43.78M | 199.56M | 55.73M | 58.26M | 205.61M | 4.69x | 1.03x | 3.66x | 3.52x |
| cross-small-t14 | 60.30M | 275.96M | 53.29M | 67.22M | 272.44M | 4.50x | 0.99x | 5.12x | 4.08x |
| cross-small-t18 | 72.59M | 331.71M | 49.16M | 67.79M | 326.53M | 4.53x | 0.98x | 6.60x | 4.87x |
| cross-small-t22 | 79.68M | 371.56M | 46.30M | 65.95M | 368.41M | 4.62x | 0.99x | 7.94x | 5.57x |
| cross-small-t26 | 87.41M | 409.61M | 44.11M | 64.63M | 422.58M | 4.84x | 1.03x | 9.57x | 6.52x |
| cross-small-t28 | 90.58M | 418.04M | 42.69M | 64.92M | 426.26M | 4.72x | 1.02x | 9.88x | 6.46x |
| cross-mixed-t2 | 6.14M | 16.20M | 42.95M | 2.81M | 53.08M | 8.61x | 3.27x | 1.23x | 18.86x |
| cross-mixed-t6 | 18.01M | 44.10M | 19.29M | 8.31M | 142.14M | 7.84x | 3.23x | 7.38x | 17.17x |
| cross-mixed-t10 | 29.20M | 70.75M | 17.51M | 13.56M | 218.74M | 7.37x | 3.09x | 12.50x | 16.17x |
| cross-mixed-t14 | 40.15M | 96.63M | 16.90M | 18.58M | 274.92M | 6.82x | 2.84x | 16.25x | 14.83x |
| cross-mixed-t18 | 49.46M | 115.20M | 16.14M | 22.42M | 330.67M | 6.67x | 2.88x | 20.54x | 14.67x |
| cross-mixed-t22 | 55.94M | 128.50M | 15.67M | 24.19M | 364.52M | 6.58x | 2.84x | 23.28x | 14.95x |
| cross-mixed-t26 | 62.40M | 141.17M | 15.41M | 26.33M | 403.32M | 6.48x | 2.86x | 25.99x | 15.16x |
| cross-mixed-t28 | 65.19M | 145.93M | 15.22M | 29.18M | 407.84M | 6.26x | 2.80x | 26.67x | 13.78x |

> `calloc-16m-t1` 是只触碰首字节的 workload；工作站 223x 的 hpmalloc/glibc 比值是 lazy-zero/page-fault 工件，不是可移植的 calloc 加速。详见第 6 节。

## 4. 线程扩展

同一 workload、同一 live set，随线程数增加；每格是该分配器相对自身最低测试线程数（churn 为 1，cross 为 2）的聚合 calls/s 倍数。

**笔记本**

| 家族 | 线程 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc |
|---|---:|---:|---:|---:|---:|---:|
| churn-small | 1 | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| churn-small | 2 | 2.01x | 1.83x | 1.91x | 1.51x | 1.90x |
| churn-small | 3 | 2.66x | 2.50x | 2.52x | 1.63x | 2.49x |
| churn-small | 4 | 3.30x | 3.03x | 3.07x | 1.81x | 3.04x |
| churn-small | 5 | 3.57x | 3.18x | 3.30x | 1.77x | 3.33x |
| churn-small | 6 | 3.86x | 3.29x | 3.49x | 1.71x | 3.58x |
| churn-small | 7 | 4.05x | 3.44x | 3.72x | 1.64x | 3.81x |
| churn-small | 8 | 4.23x | 3.54x | 3.88x | 1.62x | 4.01x |
| churn-mixed | 1 | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| churn-mixed | 2 | 2.79x | 2.37x | 2.06x | 2.56x | 1.83x |
| churn-mixed | 3 | 3.64x | 3.15x | 2.67x | 3.10x | 2.41x |
| churn-mixed | 4 | 4.47x | 3.86x | 3.30x | 3.64x | 3.04x |
| churn-mixed | 5 | 4.86x | 4.04x | 3.50x | 3.63x | 3.31x |
| churn-mixed | 6 | 5.16x | 4.18x | 3.43x | 3.61x | 3.51x |
| churn-mixed | 7 | 5.66x | 4.32x | 3.37x | 3.58x | 3.95x |
| churn-mixed | 8 | 5.97x | 4.51x | 3.12x | 3.62x | 4.27x |
| cross-small | 2 | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| cross-small | 4 | 1.46x | 1.41x | 1.13x | 1.48x | 1.51x |
| cross-small | 6 | 1.73x | 1.79x | 1.20x | 1.93x | 1.77x |
| cross-small | 8 | 2.10x | 2.04x | 1.29x | 2.52x | 2.28x |
| cross-mixed | 2 | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| cross-mixed | 4 | 1.53x | 1.39x | 0.77x | 1.55x | 1.50x |
| cross-mixed | 6 | 1.87x | 1.75x | 0.77x | 2.10x | 1.89x |
| cross-mixed | 8 | 2.09x | 2.00x | 0.77x | 2.49x | 2.29x |

**工作站**

| 家族 | 线程 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc |
|---|---:|---:|---:|---:|---:|---:|
| churn-small | 1 | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| churn-small | 4 | 2.87x | 2.79x | 2.75x | 2.13x | 3.14x |
| churn-small | 7 | 5.21x | 5.10x | 4.93x | 2.81x | 5.58x |
| churn-small | 10 | 6.99x | 6.83x | 6.43x | 2.45x | 7.53x |
| churn-small | 13 | 9.26x | 9.02x | 8.38x | 2.61x | 9.74x |
| churn-small | 16 | 10.91x | 10.66x | 9.76x | 2.36x | 11.45x |
| churn-small | 19 | 12.45x | 12.07x | 10.84x | 2.26x | 12.83x |
| churn-small | 22 | 14.06x | 13.56x | 11.70x | 2.15x | 14.14x |
| churn-small | 25 | 15.44x | 15.03x | 12.41x | 2.06x | 15.52x |
| churn-small | 28 | 16.68x | 16.37x | 12.98x | 1.90x | 16.52x |
| churn-mixed | 1 | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| churn-mixed | 4 | 2.93x | 2.33x | 2.93x | 1.26x | 3.26x |
| churn-mixed | 7 | 5.27x | 4.09x | 3.95x | 1.25x | 5.63x |
| churn-mixed | 10 | 7.04x | 5.42x | 3.28x | 1.00x | 7.70x |
| churn-mixed | 13 | 9.09x | 7.12x | 2.30x | 0.97x | 9.89x |
| churn-mixed | 16 | 10.63x | 8.35x | 1.49x | 0.87x | 11.71x |
| churn-mixed | 19 | 11.95x | 9.75x | 1.00x | 0.87x | 12.92x |
| churn-mixed | 22 | 13.29x | 11.18x | 0.73x | 0.85x | 14.20x |
| churn-mixed | 25 | 14.38x | 12.56x | 0.56x | 0.83x | 15.44x |
| churn-mixed | 28 | 15.47x | 13.62x | 0.45x | 0.78x | 16.45x |
| cross-small | 2 | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| cross-small | 6 | 2.91x | 2.95x | 0.79x | 2.46x | 2.50x |
| cross-small | 10 | 4.74x | 4.86x | 0.79x | 3.42x | 3.97x |
| cross-small | 14 | 6.52x | 6.72x | 0.75x | 3.95x | 5.26x |
| cross-small | 18 | 7.85x | 8.08x | 0.69x | 3.98x | 6.30x |
| cross-small | 22 | 8.62x | 9.05x | 0.65x | 3.87x | 7.11x |
| cross-small | 26 | 9.46x | 9.97x | 0.62x | 3.80x | 8.16x |
| cross-small | 28 | 9.80x | 10.18x | 0.60x | 3.81x | 8.23x |
| cross-mixed | 2 | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| cross-mixed | 6 | 2.93x | 2.72x | 0.45x | 2.95x | 2.68x |
| cross-mixed | 10 | 4.75x | 4.37x | 0.41x | 4.82x | 4.12x |
| cross-mixed | 14 | 6.54x | 5.96x | 0.39x | 6.60x | 5.18x |
| cross-mixed | 18 | 8.05x | 7.11x | 0.38x | 7.97x | 6.23x |
| cross-mixed | 22 | 9.11x | 7.93x | 0.36x | 8.60x | 6.87x |
| cross-mixed | 26 | 10.16x | 8.71x | 0.36x | 9.36x | 7.60x |
| cross-mixed | 28 | 10.61x | 9.01x | 0.35x | 10.37x | 7.68x |

## 5. Realloc

单 buffer `realloc-*` 行保留为对比工件：glibc 往往能原地扩展 top chunk，而 hpmalloc 的 class-based realloc 需要移动和拷贝。

多 buffer 行是更有意义的比较。`realloc-multi` 每 worker 保持 64 个（256 KiB）或 16 个（1 MiB）buffer，按 1.5× 增长；`realloc-mixed` 还会缩小和随机增长。

**笔记本**

| 工作负载 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc | hpm/tcmalloc | hpm/mimalloc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| realloc-256k-t1 | 62.47M | 2.67M | 1.15M | 1.29M | 4.30M | 0.08x | 1.74x | 3.21x | 4.22x |
| realloc-256k-t8 | 256.66M | 10.66M | 3.11M | 2.30M | 18.27M | 0.07x | 1.71x | 6.07x | 8.37x |
| realloc-1m-t1 | 64.70M | 2.37M | 340.06k | 233.98k | 1.76M | 0.03x | 0.67x | 6.66x | 7.87x |
| realloc-multi-4k-t1 | 9.26M | 9.03M | 26.50M | 16.02M | 27.29M | 2.99x | 2.65x | 1.12x | 1.33x |
| realloc-multi-64k-t1 | 1.93M | 1.99M | 2.56M | 3.91M | 4.30M | 3.26x | 2.17x | 2.04x | 1.48x |
| realloc-mixed-64k-t1 | 3.72M | 4.19M | 7.14M | 4.89M | 8.49M | 2.67x | 2.18x | 1.30x | 1.66x |
| realloc-multi-256k-t1 | 405.18k | 802.17k | 539.31k | 645.17k | 657.35k | 1.80x | 0.91x | 1.39x | 1.11x |
| realloc-mixed-256k-t1 | 2.07M | 2.11M | 2.36M | 2.20M | 3.06M | 1.48x | 1.42x | 1.60x | 1.47x |
| realloc-multi-1m-t1 | 200.34k | 352.27k | 281.70k | 229.02k | 391.33k | 2.11x | 1.05x | 1.41x | 1.68x |
| realloc-multi-64k-t8 | 4.67M | 5.06M | 4.15M | 5.54M | 12.04M | 2.57x | 2.38x | 2.87x | 2.15x |
| realloc-mixed-64k-t8 | 15.79M | 15.06M | 18.13M | 12.57M | 28.68M | 1.80x | 1.92x | 1.58x | 2.26x |
| realloc-multi-256k-t8 | 935.14k | 1.22M | 1.08M | 1.13M | 2.39M | 2.55x | 1.95x | 2.21x | 2.13x |
| realloc-mixed-256k-t8 | 7.17M | 7.17M | 3.45M | 3.97M | 6.46M | 0.90x | 0.90x | 1.86x | 1.63x |
| realloc-multi-1m-t8 | 317.53k | 487.05k | 377.41k | 360.31k | 1.06M | 3.32x | 2.18x | 2.80x | 2.95x |

**工作站**

| 工作负载 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc | hpm/tcmalloc | hpm/mimalloc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| realloc-256k-t1 | 87.37M | 8.07M | 2.26M | 589.59k | 6.48M | 0.07x | 0.80x | 2.87x | 10.99x |
| realloc-256k-t28 | 1.56G | 93.45M | 15.47M | 6.07M | 60.55M | 0.04x | 0.65x | 3.91x | 9.97x |
| realloc-1m-t1 | 88.78M | 7.48M | 452.15k | 199.25k | 2.61M | 0.03x | 0.35x | 5.78x | 13.11x |
| realloc-multi-4k-t1 | 11.85M | 21.41M | 35.62M | 11.68M | 39.40M | 3.32x | 1.84x | 1.11x | 3.37x |
| realloc-multi-64k-t1 | 1.71M | 3.87M | 5.21M | 2.62M | 8.41M | 4.92x | 2.17x | 1.61x | 3.21x |
| realloc-mixed-64k-t1 | 7.98M | 7.47M | 14.64M | 1.92M | 16.60M | 2.08x | 2.22x | 1.13x | 8.65x |
| realloc-multi-256k-t1 | 398.85k | 1.28M | 768.25k | 651.79k | 1.31M | 3.27x | 1.02x | 1.70x | 2.01x |
| realloc-mixed-256k-t1 | 5.13M | 4.31M | 5.46M | 604.70k | 6.06M | 1.18x | 1.41x | 1.11x | 10.03x |
| realloc-multi-1m-t1 | 126.79k | 526.69k | 502.29k | 215.65k | 576.59k | 4.54x | 1.09x | 1.15x | 2.67x |
| realloc-multi-64k-t28 | 6.89M | 4.64M | 4.70M | 2.56M | 13.82M | 2.00x | 2.97x | 2.96x | 5.39x |
| realloc-mixed-64k-t28 | 31.15M | 42.93M | 26.23M | 1.99M | 29.49M | 0.95x | 0.69x | 1.12x | 14.82x |
| realloc-multi-256k-t28 | 1.28M | 1.10M | 1.05M | 595.99k | 2.28M | 1.79x | 2.06x | 2.16x | 3.82x |
| realloc-mixed-256k-t28 | 8.10M | 10.45M | 5.22M | 535.17k | 6.20M | 0.77x | 0.59x | 1.19x | 11.58x |
| realloc-multi-1m-t28 | 443.90k | 413.85k | 372.75k | 186.21k | 940.30k | 2.12x | 2.27x | 2.53x | 5.05x |

## 6. 大对象与 calloc

`large-*` 区分首次触达和逐页触碰成本：`large-4m-first` 只写一个字节，`large-4m-pages` 触碰每个 4 KiB 页。large-run 缓存和 THP 策略对这两类行影响不同。

`calloc-16m-t1` 使用 `touch=first`：测的是 lazy-zero/page-fault 行为，不是完整 memset。工作站 hpmalloc 1.44M calls/s 对笔记本 32.9k 是主机/内核/THP 效应，不是可移植的 44x 加速；因此该行对 glibc/tcmalloc 的 223x 是 benchmark 工件。稳定的 calloc 比较应看 `calloc-64k`/`calloc-1m`。

本节中极大的比值（例如 jemalloc 的 16 MiB first-touch 行）反映的是那些分配器的 purge/mmap 策略，不是 hpmalloc 的加速。

**笔记本**

| 工作负载 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc | hpm/tcmalloc | hpm/mimalloc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| large-1m-pages-t1 | 3.61M | 2.40M | 2.27M | 2.49M | 3.76M | 1.04x | 1.45x | 1.39x | 1.49x |
| large-4m-first-t1 | 31.41M | 7.46M | 21.46M | 7.28M | 48.99M | 1.36x | 6.98x | 1.64x | 7.62x |
| large-4m-pages-t1 | 837.91k | 743.80k | 708.83k | 772.08k | 862.95k | 1.04x | 1.17x | 1.18x | 1.13x |
| large-4m-pages-t8 | 1.09M | 684.00k | 730.38k | 728.90k | 774.13k | 0.73x | 1.13x | 1.05x | 1.05x |
| large-16m-first-t1 | 31.77M | 34.68k | 22.95M | 6.02M | 34.68M | 1.19x | 1269.91x | 1.56x | 8.16x |
| large-16m-pages-t1 | 124.36k | 4.05k | 99.36k | 114.10k | 119.84k | 0.96x | 29.34x | 1.10x | 1.05x |
| large-16m-pages-t8 | 165.71k | 5.60k | 147.92k | 157.71k | 157.81k | 0.95x | 28.10x | 1.07x | 1.00x |
| large-64m-pages-t1 | 794.1 | 1.05k | 12.97k | 13.87k | 14.13k | 16.96x | 13.07x | 1.07x | 1.00x |
| calloc-64k-t1 | 2.59M | 1.97M | 2.66M | 2.64M | 2.71M | 1.05x | 1.38x | 1.02x | 1.03x |
| calloc-1m-t1 | 162.45k | 156.36k | 157.46k | 166.08k | 164.44k | 0.99x | 1.03x | 1.05x | 0.99x |
| calloc-16m-t1 | 5.90k | 34.64k | 5.93k | 5.92k | 32.91k | 6.53x | 1.03x | 6.48x | 6.42x |

**工作站**

| 工作负载 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc | hpm/tcmalloc | hpm/mimalloc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| large-1m-pages-t1 | 3.43M | 3.15M | 2.74M | 78.34k | 3.51M | 1.03x | 1.12x | 1.28x | 44.83x |
| large-4m-first-t1 | 52.16M | 19.72M | 42.70M | 12.50k | 70.97M | 1.36x | 3.60x | 1.66x | 5675.75x |
| large-4m-pages-t1 | 698.99k | 684.26k | 605.47k | 12.13k | 695.94k | 1.00x | 1.02x | 1.15x | 57.39x |
| large-4m-pages-t28 | 847.09k | 938.41k | 808.25k | 8.53k | 891.69k | 1.05x | 0.95x | 1.11x | 104.69x |
| large-16m-first-t1 | 55.55M | 64.84k | 41.74M | 3.12k | 66.60M | 1.20x | 1027.07x | 1.60x | 21325.81x |
| large-16m-pages-t1 | 176.05k | 5.63k | 152.94k | 3.02k | 173.25k | 0.98x | 30.87x | 1.14x | 57.35x |
| large-16m-pages-t28 | 180.07k | 3.93k | 164.80k | 1.93k | 183.47k | 1.02x | 46.70x | 1.11x | 94.90x |
| large-64m-pages-t1 | 458.6 | 459.0 | 10.31k | 231.4 | 12.84k | 28.00x | 28.07x | 1.25x | 55.45x |
| calloc-64k-t1 | 2.63M | 1.50M | 2.73M | 826.85k | 2.73M | 1.04x | 1.82x | 1.00x | 3.31x |
| calloc-1m-t1 | 172.65k | 1.43M | 170.20k | 54.09k | 173.12k | 1.00x | 0.12x | 1.02x | 3.20x |
| calloc-16m-t1 | 6.45k | 1.19M | 6.44k | 2.09k | 1.44M | 223.22x | 1.21x | 224.07x | 687.41x |

## 7. Peak RSS

Peak RSS 包含进程、worker 栈、分配器元数据和 live workload；它不是碎片指标。cross 行尤其受队列占用和 worker 栈影响。

**笔记本**

| 工作负载 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc |
|---|---:|---:|---:|---:|---:|---:|---:|
| pair-16 | 5.9 | 15.1 | 9.7 | 6.6 | 5.9 | 1.00x | 0.39x |
| pair-65536 | 5.9 | 15.0 | 9.8 | 8.6 | 5.9 | 1.00x | 0.39x |
| fixed-64-t8 | 5.9 | 42.9 | 10.6 | 8.7 | 6.3 | 1.06x | 0.15x |
| fixed-4096-t8 | 36.1 | 61.9 | 41.9 | 42.7 | 38.0 | 1.05x | 0.61x |
| churn-small-t8 | 48.0 | 83.4 | 61.4 | 68.7 | 57.0 | 1.19x | 0.68x |
| churn-mixed-t8 | 67.4 | 144.7 | 91.2 | 132.7 | 27.9 | 0.41x | 0.19x |
| cross-small-t8 | 8.0 | 34.9 | 13.0 | 14.7 | 27.4 | 3.41x | 0.78x |
| cross-mixed-t8 | 21.1 | 69.2 | 24.2 | 64.7 | 47.4 | 2.25x | 0.69x |
| realloc-multi-64k-t8 | 45.3 | 199.0 | 100.5 | 68.7 | 70.8 | 1.56x | 0.36x |
| large-16m-pages-t1 | 20.3 | 34.8 | 26.0 | 22.6 | 20.6 | 1.01x | 0.59x |
| calloc-16m-t1 | 20.4 | 16.8 | 26.0 | 22.6 | 6.6 | 0.32x | 0.39x |

**工作站**

| 工作负载 | glibc | jemalloc | tcmalloc | mimalloc | hpmalloc | hpm/glibc | hpm/jemalloc |
|---|---:|---:|---:|---:|---:|---:|---:|
| pair-16 | 3.2 | 13.9 | 8.5 | 5.5 | 3.5 | 1.09x | 0.25x |
| pair-65536 | 3.2 | 13.8 | 9.4 | 5.6 | 3.5 | 1.09x | 0.25x |
| fixed-64-t28 | 5.7 | 121.9 | 10.6 | 12.0 | 9.3 | 1.62x | 0.08x |
| fixed-4096-t28 | 112.9 | 189.8 | 123.5 | 168.2 | 120.3 | 1.07x | 0.63x |
| churn-small-t28 | 149.7 | 265.2 | 172.4 | 260.0 | 187.9 | 1.26x | 0.71x |
| churn-mixed-t28 | 232.8 | 494.1 | 204.6 | 443.1 | 81.5 | 0.35x | 0.17x |
| cross-small-t28 | 12.6 | 69.7 | 20.4 | 19.1 | 74.2 | 5.91x | 1.06x |
| cross-mixed-t28 | 56.2 | 144.5 | 44.3 | 99.6 | 158.8 | 2.83x | 1.10x |
| realloc-multi-64k-t28 | 126.9 | 1381.4 | 211.0 | 228.8 | 165.4 | 1.30x | 0.12x |
| large-16m-pages-t1 | 19.2 | 33.4 | 24.9 | 23.4 | 19.4 | 1.01x | 0.58x |
| calloc-16m-t1 | 19.2 | 13.9 | 24.5 | 23.4 | 3.4 | 0.18x | 0.25x |

## 8. 分析

笔记本 62 个 workload tag 的逐轮配对几何平均为 **2.09x glibc、2.50x jemalloc、1.46x tcmalloc、1.96x mimalloc**；工作站 74 个 tag 为**2.57x glibc、2.17x jemalloc、2.31x tcmalloc**（mimalloc 在该主机/版本上严重退化，hpmalloc 领先 28.25x）。这些都是紧凑摘要，不是验收标准：混合了极小单对象、1 MiB pair、多线程 churn 和 `calloc-16m` 首字节工件，逐 workload 表才是权威。

**尺寸谱中间段是优势。** 4 KiB、64 KiB、1 MiB 的重复 `pair` 比 glibc 快数倍；jemalloc 在 64 KiB/1 MiB 上因为 cache 层级结束而塌缩。hpmalloc 的TCache 覆盖整个普通类范围，同一套机制服务 4 KiB 和 1 MiB。

**最小单对象循环接近，但不统治。** 扁平 class_map 去掉了普通类 free 中 pagemap/granule-entry 的依赖；`pair-16`–`pair-1024` 和 `fixed-64` 已接近 glibc。tcmalloc 在部分 tiny/fixed 行仍更快，尤其在 28 线程主机上，因为它的每类缓存非常精简。

**Churn 和跨线程复用强。** 随机 churn 和 producer/consumer cross 受益于同一套全尺寸缓存和 bounded empty-span retention。8 分片 central bin 让 28 线程下同类 refill/flush 不会串行化到一把锁。工作站 `churn-small-t28` 2.02G calls/s（2.54x glibc、1.03x jemalloc、1.08x tcmalloc），`churn-mixed-t28` 1.62G（3.28x/3.64x/27.7x），`cross-small-t28` 426M（4.72x/1.02x/9.88x）。

**多 live buffer 的 realloc 成为强项。** 100% slack 和已有 direct mapping 的 `mremap` 避免重复拷贝；单 buffer top-chunk 行按构造仍偏向 glibc。仍有一个残余：笔记本 8T 的 `realloc-mixed-256k-t8` 约为 glibc/jemalloc 的 0.90x，工作站 28T 的 `realloc-mixed-256k-t28` 为 0.77x glibc / 0.59x jemalloc，因为 2 MiB span 与混合 shrink/grow 的组合不如 64 KiB 有利；tcmalloc 在该行仍更慢。这是按类调优的机会，不是通用 realloc 失败。

**大对象受益于复用，但逐页触碰受 page 支配。** 每 arena 64 MiB 的 large-run 缓存消除了重复 4 MiB/16 MiB/64 MiB 的 mprotect/缺页成本；当 workload 触碰每一页时，THP 和 page fault 主导，多线程行仍可能低于 glibc。

**RSS 是取舍，不是免费胜利。** 大多数非 cross workload 的 peak RSS 中位比值远低于 jemalloc；但深 TCache、空 span 保留、large-run 缓存和 realloc slack 都是用内存换吞吐。`cross-mixed` 是最明显的代价；目前没有全局 TCache 字节预算。

**两台机器的定性模式一致。** 笔记本和工作站的绝对速率、分配器库版本不同，但形状相同：中间尺寸 TCache 远高于 glibc，极小尺寸接近但不总第一，多 buffer realloc/cross/churn 是强项。这种一致性比任何单个绝对值更有意义。

## 9. 限制

- 两台主机、默认分配器策略；`HPMALLOC_OPTIONS` 变体和非默认 jemalloc/tcmalloc/mimalloc 设置不在主表。
- 未测延迟分位数（p50/p99/p999）、长期碎片/RSS 平台、NUMA。
- Peak RSS 不是碎片指标；包含运行时/库映射和 worker 栈。
- 单 buffer `realloc-*` 是 top-chunk 对比工件；通用 realloc 看 `realloc-multi`/`realloc-mixed`。
- cross 行包含队列占用和 worker 栈，不是纯分配器 footprint。
