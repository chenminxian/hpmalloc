# hpmalloc

[English](README.md) | **简体中文**

Linux-only、C++23 高性能通用内存分配器。

**概述：** 从 16 B 到数 MB 的统一分配器，全尺寸线程缓存、分片中心 bin、预留 2 MiB slot 的 arena、普通类扁平 class_map、大对象缓存，不依赖其他分配器运行时。既可以作为普通库链接，也可以通过 `LD_PRELOAD` 接管现有进程。兼容系统基础组件和桌面发行版。

## 设计特点

- **全尺寸 TCache：** 68 个普通类覆盖 16 B–2 MiB，另有 8 组对齐类（32 B–4 KiB 对齐），共 612 类。16 B 的 AoS cache bin 让热 bin 元数据落在一个 cache line 内。
- **分片中心 bin：** 每类 8 个分片，每个分片一把独占 cache line 的 TTAS锁。线程优先使用自己的分片，空了可以 stealing，flush 时按 span 所属分片归还。这是多线程下同类 refill/flush 不塌缩到一把锁的关键。
- **扁平 class_map：** 每个 4 KiB granule 一个字节，普通类对象直接得到 class id；`free`/`realloc`/`usable_size` 不再走 pagemap → extent → granule-entry 链。对齐类和大对象仍走精确元数据路径；`class_map=0` 可关闭。
- **预留 arena：** 默认 64 GiB 虚拟地址（`PROT_NONE|MAP_NORESERVE`），按 2 MiB slot、4 KiB granule 切分，惰性 `mprotect` 提交；大对象使用连续 slot run。
- **large-run 缓存：** 每个 arena 最多保留 64 MiB 已释放大 run，4 MiB/16 MiB/64 MiB 反复分配不再每次支付 `mprotect`/`madvise` 和缺页。
- **有界空 span 保留：** 小 span 与 256 KiB–2 MiB span 使用独立的分片预算，跨线程生产者/消费者可以复用 span，又不会让少量大 span 挤占热的小类。
- **大块 calloc 惰性清零：** 大块零初始化使用匿名页 `MADV_DONTNEED` 语义，不做 eager `memset`。
- **两阶段 purge：** 小 span、large run、large cache 的 purge 都在 arena 锁外完成；地址范围在完成前保持占用，避免“归还页与重新分配”竞争。
- **有分寸的 THP 策略：** 小 extent 标记 `MADV_NOHUGEPAGE`；完整 large chunk 可用 `MADV_HUGEPAGE`；尾部不放大。
- **少拷贝的 realloc：** 普通类增长默认 100% 超额分配（受类表上限约束），已有 direct mapping 用 `mremap(MREMAP_MAYMOVE)`，large run 邻接 slot 空闲时原地扩展。
- **编译期大小类：** `consteval` 在构建期生成类表，`static_assert` 校验。
- **不依赖其他分配器：** 共享/静态库只需要 libc，不链接其他运行时。

## 性能速览

使用两台机器的两轮全量测试：8 线程笔记本（8 次进程重复）和 28 线程工作站（12 次重复）。每轮比较 glibc、jemalloc、tcmalloc、mimalloc 和
hpmalloc，每次使用独立进程，逐轮轮换 allocator 顺序，并报告**配对比值**（同一重复内计算比值，再取中位数），以抵消 CPU 频率漂移。绝对 calls/s依赖主机，跨机器只比较比值。

**完整性能测试数据与分析见 [Performance-zh_CN.md](docs/Performance-zh_CN.md)。**

**8 线程笔记本（Intel i7-1195G7，16GiB LPDDR4，3733MT/s）**
| 工作负载 | hpm calls/s | 对 glibc | 对 jemalloc | 对 tcmalloc | 对 mimalloc |
|---|---:|---:|---:|---:|---:|
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

**28 线程工作站（Intel i7-14700K，48GiB DDR5，4200MT/s）**
| 工作负载 | hpm calls/s | 对 glibc | 对 jemalloc | 对 tcmalloc | 对 mimalloc |
|---|---:|---:|---:|---:|---:|
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
> `mimalloc` 在 64 KiB/1 MiB 的极大比值是它在重复 `pair` workload 下的真实吞吐塌缩，不是格式错误；同一批行也显示了 jemalloc 的 cache-tier 悬崖。
> 原始速率见 [Performance-zh_CN.md](docs/Performance-zh_CN.md)。

**数字说明了什么**

hpmalloc 的设计目标是尺寸谱的中间段。它不会在 1 KiB（glibc 的经典 tcache 边界）掉下线程缓存悬崖，也不会在 64 KiB/1 MiB 上像 jemalloc 那样在重复 `pair` 中塌缩。同一套 TCache 策略覆盖整个普通类范围；256 KiB/1 MiB 类使用 2 MiB span，由大对象机制支撑。

class_map 提升了小对象分配的性能。在此之前，普通类 free 需要 arena range check、pagemap load 和 16 B granule-entry load；`pair-16`–`pair-1024` 和 `fixed-64` 明显落后 glibc。使用扁平表后，普通类 free 只剩 range check + 1 字节 load；最小尺寸上与 glibc/tcmalloc 的差距很小，而一旦 live set 开始 churn 或尺寸超过 glibc tcache 范围，全尺寸 TCache 的优势就很明显。

大对象首次触达和逐页触碰不再只是 syscall 税。large-run 缓存让 4 MiB/16 MiB/64 MiB 反复使用保持热；大块 lazy calloc 避免写内核已经保证为零的页。

**客观短板**

- **最小单对象循环不是强项。** `pair-16`–`pair-1024` 和 `fixed-64` 接近 glibc，但 tcmalloc 在部分行仍更快（尤其在 28 线程主机上）。hpmalloc 的优势是 live-set churn 下的全尺寸缓存，不是最紧的单对象循环。
- **64 KiB 固定尺寸偏向 tcmalloc。** 它的大类线程缓存非常有效；hpmalloc 的每类 512 KiB 目标只能放 8 个 64 KiB 对象。
- **部分 4 MiB 逐页触碰行受 page fault 支配。** 多线程 `large-4m-pages` 可能低于 glibc，因为 benchmark 触碰每一页；这是 THP/page 行为，不是分配器锁。
- **256 KiB mixed realloc 是 realloc 的残余弱项。** 8 线程 `realloc-mixed-256k-t8` 约为 glibc/jemalloc 的 0.90x；28 线程 `realloc-mixed-256k-t28` 为 0.77x glibc、0.59x jemalloc（tcmalloc 仍更慢）。2 MiB span 与混合 shrink/grow 模式仍留有按类定制的优化空间。
- **跨线程混合 RSS 是深缓存的代价。** `cross-mixed` 峰值高于 glibc/tcmalloc；单线程 TCache 最坏约 28 MiB（68 个普通类）或 283 MiB（612 个类全部填满）。目前没有全局字节预算。
- **单 buffer realloc 是对比工件。** benchmark 只增长一个 top-of-heap buffer，glibc 可以原地扩展；通用比较应看 `realloc-multi` 和 `realloc-mixed`。
- **范围限制。** 未声明延迟分位数、长期碎片/RSS 平台、NUMA、静态链接或 setuid 加固；peak RSS 不是碎片指标。

## 设计概览

```text
应用
  │  malloc/free/calloc/realloc/aligned/C++ new
  ▼
TCache（每线程 × 每类，16 B AoS bin）
  │  批量 refill / flush
  ▼
CentralBin[class][8]（每分片 TTAS 锁、span 链表、保留空 span）
  │  page alloc / release
  ▼
Arena：64 GiB VA、2 MiB slot、4 KiB granule、惰性提交
  ├─ small extent：512 granule bitmap + granule 表 + class_map
  ├─ large run：连续 slot + 每 arena 64 MiB 缓存
  └─ direct mmap 后备：>2 MiB 对齐或 arena 耗尽
```

对象没有 owner：free 进入当前线程 TCache，再批量回到 central bin 分片；任何线程都可以复用。这使跨线程生产者/消费者稳定，不会出现“owner 线程睡着导致 remote free 无法处理”。代价是 free 查找和每线程缓存占用。

关键取舍：
| 取舍 | 收益 | 代价 |
|---|---|---|
| 64 GiB VA 预留、惰性提交 | 地址稳定、增长便宜、无需 remap | 对 VA/RLIMIT/overcommit 敏感；per-slot 元数据 |
| 2 MiB slot、4 KiB granule | span 简单、恢复快、slot 对齐 THP | 奇数尺寸有内部碎片 |
| 深 per-class TCache（512 KiB 目标） | 全尺寸速度、churn/cross 强 | 单线程最坏 28 MiB（普通类）/283 MiB（全部类） |
| 8 个 central-bin 分片 | 28 线程同类 refill/flush 不串行 | 约 613 KiB 元数据；stealing/分片逻辑 |
| 扁平 class_map | 普通类 free 少两次依赖 load | 每 arena 16 MiB VA；page release 需同步清表 |
| 有界空 span 保留 | 跨线程复用、减少 page churn | 保留 RSS，直到 `hpm_trim` |
| 每 arena 64 MiB large cache | 4/16/64 MiB 反复使用免 syscall/缺页 | 保留地址空间和大对象锁工作 |
| 100% realloc slack | vector 增长少一次拷贝 | 短期 RSS 可到 2×；仍受类上限约束 |
| THP auto / `MADV_FREE` | 小 extent 不放大；释放便宜 | 依赖主机 THP；RSS 可能保持较高 |
| 7 个 TU、C++23、CMake 3.20 | 可读、工具链广 | 跨 TU 内联受限；TLS 声明方式很关键 |

## 构建

要求：Linux x86-64、CMake ≥3.20、C++23 编译器（GCC 11+ 或 Clang 16+）、Ninja 或 Make。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

产物：
- `build/libhpmalloc.so`（soname `libhpmalloc.so.1`）
- `build/libhpmalloc.a`
- `build/hpmalloc_basic_test`
- `build/hpmalloc_preload_cpp_test`
- `build/hpmalloc_bench`（与分配器无关的 benchmark，不链接 hpmalloc）

## 使用

```sh
LD_PRELOAD=$PWD/build/libhpmalloc.so your-program
```

也可以作为普通 C/C++ 库链接。公共头是 `include/hpmalloc/hpmalloc.h`；导出标准 `malloc`/`free`/`calloc`/`realloc`、对齐分配函数和 C++ `operator new/delete`。

运行期选项：

```sh
HPMALLOC_OPTIONS="thp=auto,purge=free,arena_mb=65536" \
LD_PRELOAD=$PWD/build/libhpmalloc.so your-program
```

| 选项 | 值 | 作用 |
|---|---|---|
| `thp` | `auto`、`always`、`never`、`collapse` | 小/大对象 THP 策略；`collapse` 当前等同 `always` |
| `purge` | `free`、`dontneed`、`defer` | 归还策略；`defer` 当前等同 `free` |
| `arena_mb` | 十进制 MB，≥4 | 初始 arena 大小（init 前） |
| `class_map` | `1`/`on`、`0`/`off` | 是否启用普通类扁平表 |
| `realloc_direct_kb` | 十进制 KB；0=关闭 | 增长型小对象 realloc 提升为 direct mapping |
| `realloc_slack_pct` | 0–400；默认 100 | 增长 realloc 的超额分配比例 |

## 测试

```sh
ctest --test-dir build --output-on-failure
```

CTest 项：
- `hpmalloc_basic`：类表穷举、API、对齐、realloc/calloc、C++ new、多线程和跨线程 free。
- `hpmalloc_preload_cpp`：`LD_PRELOAD` 下 C++ new/delete、aligned new、容器、线程、跨线程 free 和 fork。
- `hpmalloc_compat_coreutils`：常用 GNU coreutils 的启动矩阵和文件/shell/归档功能对比。
- `hpmalloc_compat_common`：gcc/g++、Python、Git、CMake、SQLite、Perl、`RLIMIT_AS`。

也可以直接运行脚本：

```sh
bash tests/compat_coreutils.sh build/libhpmalloc.so
bash tests/compat_common.sh build/libhpmalloc.so
```

## 基准测试

分配器无关的 benchmark 默认构建（`HPM_BUILD_BENCH=ON`）：

```sh
cmake --build build --target hpmalloc_bench
bench/run_bench.sh --reps 8 --outdir /tmp/hpm_bench_manual
```

脚本会读取在线 CPU 数、生成等差数列线程点、逐轮轮换 glibc/jemalloc/tcmalloc/mimalloc/hpmalloc 顺序，并使用独立进程。`--only`、`--max-threads`、`--thread-step` 可用于定向测试。realloc 套件包含历史的单 buffer 行，以及 `realloc-multi`/`realloc-mixed` 多 buffer 行，避免比较被 glibc top-chunk 原地扩展主导。workload 定义和 caveat 见 [Bench-zh_CN.md](docs/Bench-zh_CN.md)。

## 备注

长期碎片/RSS 平台、延迟分位数、完整模型化压力、NUMA/后台 purge、mTHP/`MADV_COLLAPSE`、setuid/静态链接加固等，仍未完成。
请把它视为开发阶段的高性能分配器，而不是对所有工作负载都已生产加固的通用替代品。
