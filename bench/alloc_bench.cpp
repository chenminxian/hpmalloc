// Allocator-agnostic malloc benchmark.
//
// This program deliberately does not link against any allocator.  Select one with LD_PRELOAD:
//
//   LD_PRELOAD=./build/libhpmalloc.so ./build/hpmalloc_bench ...
//   LD_PRELOAD=/usr/lib/libjemalloc.so ./build/hpmalloc_bench ...
//   LD_PRELOAD=/usr/lib/libtcmalloc_minimal.so ./build/hpmalloc_bench ...
//   LD_PRELOAD=/usr/lib/libmimalloc.so ./build/hpmalloc_bench ...
//
// It reports one CSV row per invocation.  See bench/run_bench.sh for the multi-allocator / repetition driver.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <print>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class Scenario : uint8_t
{
    Pair,
    Fixed,
    Churn,
    Cross,
    Realloc,
    ReallocMulti,
    ReallocMixed,
    Large,
    Calloc
};

enum class Touch : uint8_t
{
    None,
    First,
    Pages
};

struct Config
{
    Scenario scenario = Scenario::Pair;
    Touch touch = Touch::First;
    bool pin = true;
    bool probe = false;
    bool header = false;
    size_t size = 64;
    size_t min_size = 16;
    size_t max_size = 4096;
    uint32_t live = 1024;
    uint32_t threads = 1;
    double seconds = 0.5;
    double warmup = 0.15;
    uint64_t seed = 0x243f6a8885a308d3ull;
    std::string label = "unknown";
    std::string tag = "-";
};

struct WorkerResult
{
    uint64_t cycles = 0;
    uint64_t calls = 0;
    uint64_t bytes = 0;
    TimePoint end{};
};

std::atomic<uint32_t> g_num_threads{1};
TimePoint g_deadline{};

std::mutex g_gate_mutex;
std::condition_variable g_gate_cv;
uint32_t g_ready{0};
bool g_go{false};

std::mutex g_stop_mutex;
std::condition_variable g_stop_cv;
uint32_t g_stopped{0};

std::vector<int> g_cpus;

[[noreturn]] void die(const char *what)
{
    std::println(stderr, "alloc_bench: {}", what);
    std::abort();
}

inline void cpu_relax()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

std::string csv_clean(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
    {
        if (c == ',' || c == '\n' || c == '\r')
            c = '_';
    }
    return out;
}

void parse_scenario(const char *s, Config &cfg)
{
    if (!std::strcmp(s, "pair"))
        cfg.scenario = Scenario::Pair;
    else if (!std::strcmp(s, "fixed"))
        cfg.scenario = Scenario::Fixed;
    else if (!std::strcmp(s, "churn"))
        cfg.scenario = Scenario::Churn;
    else if (!std::strcmp(s, "cross"))
        cfg.scenario = Scenario::Cross;
    else if (!std::strcmp(s, "realloc"))
        cfg.scenario = Scenario::Realloc;
    else if (!std::strcmp(s, "realloc-multi"))
        cfg.scenario = Scenario::ReallocMulti;
    else if (!std::strcmp(s, "realloc-mixed"))
        cfg.scenario = Scenario::ReallocMixed;
    else if (!std::strcmp(s, "large"))
        cfg.scenario = Scenario::Large;
    else if (!std::strcmp(s, "calloc"))
        cfg.scenario = Scenario::Calloc;
    else
        die("unknown scenario");
}

void parse_touch(const char *s, Config &cfg)
{
    if (!std::strcmp(s, "none"))
        cfg.touch = Touch::None;
    else if (!std::strcmp(s, "first"))
        cfg.touch = Touch::First;
    else if (!std::strcmp(s, "pages"))
        cfg.touch = Touch::Pages;
    else
        die("unknown touch mode");
}

uint64_t parse_u64(const char *s)
{
    errno = 0;
    char *end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 0);
    if (errno || !end || *end)
        die("bad numeric option");
    return v;
}

double parse_double(const char *s)
{
    errno = 0;
    char *end = nullptr;
    double v = std::strtod(s, &end);
    if (errno || !end || *end || v < 0)
        die("bad numeric option");
    return v;
}

void usage(const char *argv0)
{
    std::println(
        "usage: {} --scenario pair|fixed|churn|cross|realloc|realloc-multi|realloc-mixed|large|calloc [options]\n"
        "  --size N          fixed/pair/large/calloc object size (default 64)\n"
        "  --min-size N      churn/cross/realloc minimum request (default 16)\n"
        "  --max-size N      churn/cross/realloc maximum request (default 4096)\n"
        "  --live N          live objects/buffers per thread (default 1024)\n"
        "  --threads N       worker threads (default 1; cross requires even)\n"
        "  --seconds F       timed duration (default 0.5)\n"
        "  --warmup F        warmup duration (default 0.15)\n"
        "  --seed N          RNG seed\n"
        "  --label S         allocator label for CSV output\n"
        "  --tag S           workload tag for CSV output\n"
        "  --touch none|first|pages (default first; large defaults to pages)\n"
        "  --pin / --no-pin  pin workers to CPUs (default pin)\n"
        "  --probe           print allocator identity hints to stderr\n"
        "  --header          print CSV header\n",
        argv0);
}

// ---------------------------------------------------------------------------
// RNG and request size generation
// ---------------------------------------------------------------------------

inline uint64_t splitmix64(uint64_t &state)
{
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

uint64_t seed_for(uint32_t idx, uint64_t seed)
{
    return seed ^ (static_cast<uint64_t>(idx + 1) * 0x9e3779b97f4a7c15ull);
}

size_t random_size(uint64_t &state, size_t lo, size_t hi)
{
    if (lo == 0)
        lo = 1;
    if (hi < lo)
        hi = lo;
    const unsigned lo_log = 63u - static_cast<unsigned>(__builtin_clzll(lo));
    const unsigned hi_log = 63u - static_cast<unsigned>(__builtin_clzll(hi));
    const uint64_t r = splitmix64(state);
    const unsigned e = lo_log + static_cast<unsigned>(r % (hi_log - lo_log + 1));
    const size_t base = size_t{1} << e;
    size_t v = base + static_cast<size_t>((r >> 32) & (base - 1));
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

// ---------------------------------------------------------------------------
// Allocation helpers
// ---------------------------------------------------------------------------

void *xalloc(size_t n)
{
    void *p = std::malloc(n ? n : 1);
    if (!p)
        die("malloc failed");
    return p;
}

void *xcalloc(size_t n)
{
    void *p = std::calloc(1, n ? n : 1);
    if (!p)
        die("calloc failed");
    return p;
}

inline void touch_first(void *p)
{
    *static_cast<volatile unsigned char *>(p) = 0x5a;
}

void touch_object(void *p, size_t n, Touch mode)
{
    if (mode == Touch::None || n == 0)
        return;
    volatile unsigned char *q = static_cast<volatile unsigned char *>(p);
    q[0] = 0x5a;
    if (mode == Touch::Pages)
    {
        for (size_t off = 4096; off < n; off += 4096)
            q[off] = 0xa5;
        q[n - 1] = 0x3c;
    }
}

// ---------------------------------------------------------------------------
// Pinning / phase barriers
// ---------------------------------------------------------------------------

void read_cpus()
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return;
    for (int i = 0; i < CPU_SETSIZE; ++i)
    {
        if (CPU_ISSET(i, &set))
            g_cpus.push_back(i);
    }
}

void pin_worker(uint32_t idx)
{
    if (g_cpus.empty())
        return;
    const int cpu = g_cpus[idx % g_cpus.size()];
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

void warmup_done()
{
    std::unique_lock lock(g_gate_mutex);
    ++g_ready;
    g_gate_cv.notify_all();
    g_gate_cv.wait(lock, [] { return g_go; });
}

void finish_barrier()
{
    std::unique_lock lock(g_stop_mutex);
    ++g_stopped;
    g_stop_cv.notify_all();
    const uint32_t n = g_num_threads.load(std::memory_order_acquire);
    g_stop_cv.wait(lock, [n] { return g_stopped >= n; });
}

inline bool timed_out()
{
    return Clock::now() >= g_deadline;
}

inline TimePoint after_seconds(double seconds)
{
    return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

// ---------------------------------------------------------------------------
// Pair: malloc/free the same size immediately.  One cycle = alloc + free.
// ---------------------------------------------------------------------------

void worker_pair(uint32_t idx, const Config &cfg, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    const TimePoint warm_end = after_seconds(cfg.warmup);
    while (Clock::now() < warm_end)
    {
        void *p = xalloc(cfg.size);
        touch_object(p, cfg.size, cfg.touch);
        std::free(p);
    }
    warmup_done();

    uint64_t cycles = 0;
    uint64_t bytes = 0;
    while (true)
    {
        void *p = xalloc(cfg.size);
        touch_object(p, cfg.size, cfg.touch);
        std::free(p);
        ++cycles;
        bytes += cfg.size;
        if ((cycles & 63u) == 0 && timed_out())
            break;
    }
    out.end = Clock::now();
    out.cycles = cycles;
    out.calls = cycles * 2;
    out.bytes = bytes;
    finish_barrier();
}

// ---------------------------------------------------------------------------
// Fixed / churn: keep a live set and replace one entry per cycle.
// ---------------------------------------------------------------------------

void worker_fixed(uint32_t idx, const Config &cfg, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    uint64_t rng = seed_for(idx, cfg.seed);
    std::vector<void *> live(cfg.live);
    for (void *&p : live)
    {
        p = xalloc(cfg.size);
        touch_object(p, cfg.size, cfg.touch);
    }

    const TimePoint warm_end = after_seconds(cfg.warmup);
    do
    {
        const uint32_t slot = static_cast<uint32_t>(splitmix64(rng) % cfg.live);
        std::free(live[slot]);
        live[slot] = xalloc(cfg.size);
        touch_object(live[slot], cfg.size, cfg.touch);
    } while (Clock::now() < warm_end);
    warmup_done();

    uint64_t cycles = 0;
    uint64_t bytes = 0;
    while (true)
    {
        const uint32_t slot = static_cast<uint32_t>(splitmix64(rng) % cfg.live);
        std::free(live[slot]);
        live[slot] = xalloc(cfg.size);
        touch_object(live[slot], cfg.size, cfg.touch);
        ++cycles;
        bytes += cfg.size;
        if ((cycles & 63u) == 0 && timed_out())
            break;
    }
    out.end = Clock::now();
    out.cycles = cycles;
    out.calls = cycles * 2;
    out.bytes = bytes;
    finish_barrier();
    for (void *p : live)
        std::free(p);
}

void worker_churn(uint32_t idx, const Config &cfg, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    uint64_t rng = seed_for(idx, cfg.seed);
    std::vector<void *> live(cfg.live);
    for (void *&p : live)
    {
        const size_t n = random_size(rng, cfg.min_size, cfg.max_size);
        p = xalloc(n);
        touch_object(p, n, cfg.touch);
    }

    const TimePoint warm_end = after_seconds(cfg.warmup);
    do
    {
        const uint32_t slot = static_cast<uint32_t>(splitmix64(rng) % cfg.live);
        std::free(live[slot]);
        const size_t n = random_size(rng, cfg.min_size, cfg.max_size);
        live[slot] = xalloc(n);
        touch_object(live[slot], n, cfg.touch);
    } while (Clock::now() < warm_end);
    warmup_done();

    uint64_t cycles = 0;
    uint64_t bytes = 0;
    while (true)
    {
        const uint32_t slot = static_cast<uint32_t>(splitmix64(rng) % cfg.live);
        std::free(live[slot]);
        const size_t n = random_size(rng, cfg.min_size, cfg.max_size);
        live[slot] = xalloc(n);
        touch_object(live[slot], n, cfg.touch);
        ++cycles;
        bytes += n;
        if ((cycles & 63u) == 0 && timed_out())
            break;
    }
    out.end = Clock::now();
    out.cycles = cycles;
    out.calls = cycles * 2;
    out.bytes = bytes;
    finish_barrier();
    for (void *p : live)
        std::free(p);
}

// ---------------------------------------------------------------------------
// Cross-thread: one producer thread allocates, one consumer thread frees.
// One cycle = one allocation + its free in the other thread.
// ---------------------------------------------------------------------------

struct alignas(64) CrossCell
{
    std::atomic<uint32_t> state{0}; // 0 = empty, 1 = full
    void *ptr = nullptr;
};

struct alignas(64) CrossQueue
{
    static constexpr uint32_t kCap = 256;
    static constexpr uint32_t kMask = kCap - 1;

    CrossCell cells[kCap];
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> warm_done{false};
    std::atomic<bool> done{false};
};

void worker_cross_producer(uint32_t idx, const Config &cfg, CrossQueue *q, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    uint64_t rng = seed_for(idx, cfg.seed);
    uint32_t head = 0;

    auto publish = [&](size_t n)
    {
        CrossCell &cell = q->cells[head];
        uint32_t spins = 0;
        while (cell.state.load(std::memory_order_acquire) != 0)
        {
            if ((++spins & 1023u) == 0)
                sched_yield();
            else
                cpu_relax();
        }
        void *p = xalloc(n);
        touch_first(p);
        cell.ptr = p;
        cell.state.store(1, std::memory_order_release);
        head = (head + 1) & CrossQueue::kMask;
        q->produced.fetch_add(1, std::memory_order_release);
    };

    const TimePoint warm_end = after_seconds(cfg.warmup);
    while (Clock::now() < warm_end)
    {
        const size_t n = random_size(rng, cfg.min_size, cfg.max_size);
        publish(n);
    }
    q->warm_done.store(true, std::memory_order_release);
    warmup_done();

    uint64_t cycles = 0;
    uint64_t bytes = 0;
    while (true)
    {
        const size_t n = random_size(rng, cfg.min_size, cfg.max_size);
        publish(n);
        ++cycles;
        bytes += n;
        if ((cycles & 63u) == 0 && timed_out())
            break;
    }
    q->done.store(true, std::memory_order_release);
    out.end = Clock::now();
    out.cycles = cycles;
    out.calls = cycles * 2;
    out.bytes = bytes;
    finish_barrier();
}

void worker_cross_consumer(uint32_t idx, const Config &cfg, CrossQueue *q, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    uint32_t tail = 0;

    auto consume_until = [&](bool warm_phase)
    {
        while (true)
        {
            // Read the stop flag before the counters.  The producer publishes
            // the flag after its final produced increment, so an acquire load
            // of the flag makes that increment visible to the counter loads
            // below.  Loading the counters first allowed a stale snapshot to
            // pair with a fresh `done` flag and exit with entries still queued.
            const bool stopped =
                warm_phase ? q->warm_done.load(std::memory_order_acquire) : q->done.load(std::memory_order_acquire);
            const uint64_t produced = q->produced.load(std::memory_order_acquire);
            const uint64_t consumed = q->consumed.load(std::memory_order_acquire);
            if (stopped && produced == consumed)
                break;

            CrossCell &cell = q->cells[tail];
            if (cell.state.load(std::memory_order_acquire) == 1)
            {
                void *p = cell.ptr;
                cell.state.store(0, std::memory_order_release);
                std::free(p);
                tail = (tail + 1) & CrossQueue::kMask;
                q->consumed.fetch_add(1, std::memory_order_release);
            }
            else
            {
                cpu_relax();
            }
        }
    };

    consume_until(true);
    warmup_done();
    consume_until(false);
    out.end = Clock::now();
    finish_barrier();
}

// ---------------------------------------------------------------------------
// Realloc: one cycle = grow one buffer from min_size to max_size, then free.
// ---------------------------------------------------------------------------

void worker_realloc(uint32_t idx, const Config &cfg, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    (void)idx;

    auto grow_cycle = [&](uint64_t &reallocs, uint64_t &bytes)
    {
        size_t requested = cfg.min_size;
        void *p = xalloc(requested);
        touch_object(p, requested, cfg.touch);
        bytes += requested;
        while (requested < cfg.max_size)
        {
            size_t next = requested + requested / 2 + 16;
            if (next > cfg.max_size)
                next = cfg.max_size;
            p = std::realloc(p, next);
            if (!p)
                die("realloc failed");
            requested = next;
            touch_object(p, requested, cfg.touch);
            ++reallocs;
            bytes += requested;
        }
        std::free(p);
    };

    const TimePoint warm_end = after_seconds(cfg.warmup);
    do
    {
        uint64_t ignored_reallocs = 0;
        uint64_t ignored_bytes = 0;
        grow_cycle(ignored_reallocs, ignored_bytes);
    } while (Clock::now() < warm_end);
    warmup_done();

    uint64_t cycles = 0;
    uint64_t calls = 0;
    uint64_t bytes = 0;
    while (true)
    {
        grow_cycle(calls, bytes);
        ++cycles;
        if (timed_out())
            break;
    }
    out.end = Clock::now();
    out.cycles = cycles;
    out.calls = calls;
    out.bytes = bytes;
    finish_barrier();
}

// ---------------------------------------------------------------------------
// Multi-buffer realloc: keep several buffers live and grow/shrink each in
// turn.  Unlike the single-buffer realloc row, glibc cannot satisfy every
// request by extending the top chunk, so this measures the allocator's own
// move/copy path.
// ---------------------------------------------------------------------------

void worker_realloc_multi(uint32_t idx, const Config &cfg, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    std::vector<void *> live(cfg.live);
    std::vector<size_t> sizes(cfg.live);
    for (uint32_t i = 0; i < cfg.live; ++i)
    {
        sizes[i] = cfg.min_size;
        live[i] = xalloc(sizes[i]);
        touch_object(live[i], sizes[i], cfg.touch);
    }

    uint64_t cursor = 0;
    auto step = [&](uint64_t &calls, uint64_t &bytes)
    {
        const uint32_t slot = static_cast<uint32_t>(cursor++ % cfg.live);
        size_t next = sizes[slot] + sizes[slot] / 2 + 16;
        if (next > cfg.max_size)
        {
            // realloc to min would keep the large usable capacity and make the
            // rest of the growth sequence a no-op.  Free and allocate a fresh
            // min-size buffer so the next growth cycle actually copies.
            std::free(live[slot]);
            live[slot] = xalloc(cfg.min_size);
            sizes[slot] = cfg.min_size;
            touch_object(live[slot], cfg.min_size, cfg.touch);
            return;
        }
        void *p = std::realloc(live[slot], next);
        if (!p)
            die("realloc failed");
        live[slot] = p;
        sizes[slot] = next;
        touch_object(p, next, cfg.touch);
        ++calls;
        bytes += next;
    };

    const TimePoint warm_end = after_seconds(cfg.warmup);
    do
    {
        uint64_t ignored_calls = 0;
        uint64_t ignored_bytes = 0;
        step(ignored_calls, ignored_bytes);
    } while (Clock::now() < warm_end);
    warmup_done();

    uint64_t calls = 0;
    uint64_t bytes = 0;
    while (true)
    {
        step(calls, bytes);
        if (timed_out())
            break;
    }
    out.end = Clock::now();
    out.cycles = calls;
    out.calls = calls;
    out.bytes = bytes;
    finish_barrier();
    for (void *p : live)
        std::free(p);
}

// Random grow/shrink over several live buffers.  This also covers the shrink
// path, which the single-buffer grow sequence does not exercise.
void worker_realloc_mixed(uint32_t idx, const Config &cfg, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    uint64_t rng = seed_for(idx, cfg.seed);
    std::vector<void *> live(cfg.live);
    std::vector<size_t> sizes(cfg.live);
    std::vector<uint32_t> age(cfg.live, 0);
    for (uint32_t i = 0; i < cfg.live; ++i)
    {
        sizes[i] = random_size(rng, cfg.min_size, cfg.max_size);
        live[i] = xalloc(sizes[i]);
        touch_object(live[i], sizes[i], cfg.touch);
    }

    auto step = [&](uint64_t &calls, uint64_t &bytes)
    {
        const uint32_t slot = static_cast<uint32_t>(splitmix64(rng) % cfg.live);
        // Periodic free+malloc retires the accumulated large usable capacity;
        // otherwise repeated random shrink requests become no-ops and the row
        // stops measuring grow/shrink work.
        if (++age[slot] >= 8)
        {
            age[slot] = 0;
            std::free(live[slot]);
            const size_t fresh = random_size(rng, cfg.min_size, cfg.max_size);
            live[slot] = xalloc(fresh);
            sizes[slot] = fresh;
            touch_object(live[slot], fresh, cfg.touch);
            return;
        }
        const size_t next = random_size(rng, cfg.min_size, cfg.max_size);
        void *p = std::realloc(live[slot], next);
        if (!p)
            die("realloc failed");
        live[slot] = p;
        sizes[slot] = next;
        touch_object(p, next, cfg.touch);
        ++calls;
        bytes += next;
    };

    const TimePoint warm_end = after_seconds(cfg.warmup);
    do
    {
        uint64_t ignored_calls = 0;
        uint64_t ignored_bytes = 0;
        step(ignored_calls, ignored_bytes);
    } while (Clock::now() < warm_end);
    warmup_done();

    uint64_t calls = 0;
    uint64_t bytes = 0;
    while (true)
    {
        step(calls, bytes);
        if (timed_out())
            break;
    }
    out.end = Clock::now();
    out.cycles = calls;
    out.calls = calls;
    out.bytes = bytes;
    finish_barrier();
    for (void *p : live)
        std::free(p);
}

// ---------------------------------------------------------------------------
// Large: live set of large allocations, touch pages, free/allocate one.
// ---------------------------------------------------------------------------

void worker_large(uint32_t idx, const Config &cfg, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    uint64_t rng = seed_for(idx, cfg.seed);
    std::vector<void *> live(cfg.live);
    for (void *&p : live)
    {
        p = xalloc(cfg.size);
        touch_object(p, cfg.size, cfg.touch);
    }

    const TimePoint warm_end = after_seconds(cfg.warmup);
    do
    {
        const uint32_t slot = static_cast<uint32_t>(splitmix64(rng) % cfg.live);
        std::free(live[slot]);
        live[slot] = xalloc(cfg.size);
        touch_object(live[slot], cfg.size, cfg.touch);
    } while (Clock::now() < warm_end);
    warmup_done();

    uint64_t cycles = 0;
    uint64_t bytes = 0;
    while (true)
    {
        const uint32_t slot = static_cast<uint32_t>(splitmix64(rng) % cfg.live);
        std::free(live[slot]);
        live[slot] = xalloc(cfg.size);
        touch_object(live[slot], cfg.size, cfg.touch);
        ++cycles;
        bytes += cfg.size;
        if (timed_out())
            break;
    }
    out.end = Clock::now();
    out.cycles = cycles;
    out.calls = cycles * 2;
    out.bytes = bytes;
    finish_barrier();
    for (void *p : live)
        std::free(p);
}

// ---------------------------------------------------------------------------
// Calloc: one cycle = calloc + free.  No explicit touch: zeroing is the point.
// ---------------------------------------------------------------------------

void worker_calloc(uint32_t idx, const Config &cfg, WorkerResult &out)
{
    if (cfg.pin)
        pin_worker(idx);
    auto calloc_once = [&]
    {
        void *p = xcalloc(cfg.size);
        // Observe the zero-filled buffer.  Without this the compiler can remove
        // the calloc/free pair entirely (and even with the volatile store the
        // benchmark target is built with -fno-builtin-calloc as a second guard).
        volatile unsigned char *q = static_cast<volatile unsigned char *>(p);
        const unsigned char seen = q[0];
        if (seen != 0)
            die("calloc did not return zeroed memory");
        q[0] = 1;
        std::free(p);
    };
    const TimePoint warm_end = after_seconds(cfg.warmup);
    while (Clock::now() < warm_end)
        calloc_once();
    warmup_done();

    uint64_t cycles = 0;
    uint64_t bytes = 0;
    while (true)
    {
        calloc_once();
        ++cycles;
        bytes += cfg.size;
        if ((cycles & 63u) == 0 && timed_out())
            break;
    }
    out.end = Clock::now();
    out.cycles = cycles;
    out.calls = cycles * 2;
    out.bytes = bytes;
    finish_barrier();
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

size_t proc_status_kb(const char *key)
{
    FILE *f = std::fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char line[256];
    const size_t key_len = std::strlen(key);
    size_t value = 0;
    while (std::fgets(line, sizeof(line), f))
    {
        if (!std::strncmp(line, key, key_len))
        {
            (void)std::sscanf(line + key_len, " %zu", &value);
            break;
        }
    }
    std::fclose(f);
    return value;
}

void print_probe()
{
    void *p = std::malloc(100);
    if (p)
    {
        std::println(stderr, "probe: malloc_usable_size(100)={}", malloc_usable_size(p));
        std::free(p);
    }
    const char *names[] = {
        "hpm_version",
        "mallctl",
        "malloc_stats_print",
        "MallocExtension_GetNumericProperty",
    };
    for (const char *name : names)
    {
        if (dlsym(RTLD_DEFAULT, name))
            std::println(stderr, "probe: symbol {} present", name);
    }
    if (auto fn = reinterpret_cast<const char *(*)()>(dlsym(RTLD_DEFAULT, "hpm_version")))
    {
        std::println(stderr, "probe: hpm_version={}", fn());
    }
}

} // anonymous namespace

int main(int argc, char **argv)
{
    Config cfg;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto need = [&](const char *name) -> const char *
        {
            if (i + 1 >= argc)
            {
                std::println(stderr, "alloc_bench: {} needs an argument", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--scenario")
            parse_scenario(need("--scenario"), cfg);
        else if (a == "--size")
            cfg.size = static_cast<size_t>(parse_u64(need("--size")));
        else if (a == "--min-size")
            cfg.min_size = static_cast<size_t>(parse_u64(need("--min-size")));
        else if (a == "--max-size")
            cfg.max_size = static_cast<size_t>(parse_u64(need("--max-size")));
        else if (a == "--live")
            cfg.live = static_cast<uint32_t>(parse_u64(need("--live")));
        else if (a == "--threads")
            cfg.threads = static_cast<uint32_t>(parse_u64(need("--threads")));
        else if (a == "--seconds")
            cfg.seconds = parse_double(need("--seconds"));
        else if (a == "--warmup")
            cfg.warmup = parse_double(need("--warmup"));
        else if (a == "--seed")
            cfg.seed = parse_u64(need("--seed"));
        else if (a == "--label")
            cfg.label = need("--label");
        else if (a == "--tag")
            cfg.tag = need("--tag");
        else if (a == "--touch")
            parse_touch(need("--touch"), cfg);
        else if (a == "--pin")
            cfg.pin = true;
        else if (a == "--no-pin")
            cfg.pin = false;
        else if (a == "--probe")
            cfg.probe = true;
        else if (a == "--header")
            cfg.header = true;
        else if (a == "--help" || a == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            std::println(stderr, "alloc_bench: unknown option {}", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.seconds <= 0)
        die("--seconds must be > 0");
    if (cfg.threads == 0 || cfg.threads > 64)
        die("--threads out of range");
    if (cfg.live == 0)
        die("--live must be > 0");
    if (cfg.min_size == 0 || cfg.max_size < cfg.min_size)
        die("bad size range");
    if (cfg.scenario == Scenario::Fixed || cfg.scenario == Scenario::Large || cfg.scenario == Scenario::Calloc ||
        cfg.scenario == Scenario::Pair)
    {
        if (cfg.size == 0)
            die("--size must be > 0");
    }
    if (cfg.scenario == Scenario::Fixed || cfg.scenario == Scenario::Churn || cfg.scenario == Scenario::Large)
    {
        if (cfg.live > (1u << 20))
            die("--live too large");
    }
    if (cfg.scenario == Scenario::Cross && (cfg.threads < 2 || (cfg.threads & 1u)))
    {
        die("cross requires an even thread count >= 2");
    }
    if (cfg.scenario == Scenario::Large && cfg.touch == Touch::First)
    {
        // The caller can still explicitly request first-page touching; pages is
        // the default only when no explicit --touch was given.  Kept simple:
        // large callers use --touch pages in the driver.
    }

    if (cfg.probe)
        print_probe();
    read_cpus();

    const uint32_t nthreads = cfg.threads;
    g_num_threads.store(nthreads, std::memory_order_release);
    g_ready = 0;
    g_go = false;
    g_stopped = 0;
    std::vector<WorkerResult> results(nthreads);
    std::vector<std::thread> threads;
    threads.reserve(nthreads);

    std::vector<std::unique_ptr<CrossQueue>> queues;
    if (cfg.scenario == Scenario::Cross)
    {
        queues.reserve(nthreads / 2);
        for (uint32_t i = 0; i < nthreads / 2; ++i)
        {
            queues.emplace_back(std::make_unique<CrossQueue>());
        }
    }

    for (uint32_t i = 0; i < nthreads; ++i)
    {
        switch (cfg.scenario)
        {
        case Scenario::Pair:
            threads.emplace_back(worker_pair, i, std::cref(cfg), std::ref(results[i]));
            break;
        case Scenario::Fixed:
            threads.emplace_back(worker_fixed, i, std::cref(cfg), std::ref(results[i]));
            break;
        case Scenario::Churn:
            threads.emplace_back(worker_churn, i, std::cref(cfg), std::ref(results[i]));
            break;
        case Scenario::Cross:
        {
            const uint32_t pair = i / 2;
            if ((i & 1u) == 0)
            {
                threads.emplace_back(worker_cross_producer, i, std::cref(cfg), queues[pair].get(),
                                     std::ref(results[i]));
            }
            else
            {
                threads.emplace_back(worker_cross_consumer, i, std::cref(cfg), queues[pair].get(),
                                     std::ref(results[i]));
            }
            break;
        }
        case Scenario::Realloc:
            threads.emplace_back(worker_realloc, i, std::cref(cfg), std::ref(results[i]));
            break;
        case Scenario::ReallocMulti:
            threads.emplace_back(worker_realloc_multi, i, std::cref(cfg), std::ref(results[i]));
            break;
        case Scenario::ReallocMixed:
            threads.emplace_back(worker_realloc_mixed, i, std::cref(cfg), std::ref(results[i]));
            break;
        case Scenario::Large:
            threads.emplace_back(worker_large, i, std::cref(cfg), std::ref(results[i]));
            break;
        case Scenario::Calloc:
            threads.emplace_back(worker_calloc, i, std::cref(cfg), std::ref(results[i]));
            break;
        }
    }

    TimePoint start{};
    {
        std::unique_lock lock(g_gate_mutex);
        g_gate_cv.wait(lock, [nthreads] { return g_ready >= nthreads; });
        start = Clock::now();
        g_deadline = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(cfg.seconds));
        g_go = true;
        g_gate_cv.notify_all();
    }

    for (auto &t : threads)
    {
        if (t.joinable())
            t.join();
    }
    const TimePoint joined = Clock::now();

    TimePoint last_end = start;
    uint64_t cycles = 0;
    uint64_t calls = 0;
    uint64_t bytes = 0;
    for (const WorkerResult &r : results)
    {
        if (r.end > last_end)
            last_end = r.end;
        cycles += r.cycles;
        calls += r.calls;
        bytes += r.bytes;
    }
    if (last_end > joined)
        last_end = joined;
    double wall = std::chrono::duration<double>(last_end - start).count();
    if (!(wall > 0.0))
        wall = std::chrono::duration<double>(joined - start).count();
    if (!(wall > 0.0))
        wall = cfg.seconds;

    for (size_t i = 0; i < queues.size(); ++i)
    {
        CrossQueue &q = *queues[i];
        if (q.produced.load(std::memory_order_acquire) != q.consumed.load(std::memory_order_acquire))
        {
            std::println(stderr, "alloc_bench: cross pair {} leaked queue entries", i);
            std::abort();
        }
        for (uint32_t j = 0; j < CrossQueue::kCap; ++j)
        {
            if (q.cells[j].state.load(std::memory_order_acquire) != 0)
            {
                std::println(stderr, "alloc_bench: cross pair {} queue not empty", i);
                std::abort();
            }
        }
    }

    struct rusage ru;
    std::memset(&ru, 0, sizeof(ru));
    (void)getrusage(RUSAGE_SELF, &ru);
    size_t peak_kb = static_cast<size_t>(ru.ru_maxrss);
    const size_t hwm_kb = proc_status_kb("VmHWM:");
    if (hwm_kb > peak_kb)
        peak_kb = hwm_kb;
    const size_t rss_kb = proc_status_kb("VmRSS:");

    if (cfg.header)
    {
        std::println("label,tag,scenario,touch,threads,size,min_size,max_size,live,"
                     "seconds,cycles,calls,cycles_per_s,calls_per_s,ns_per_cycle,"
                     "bytes,bytes_per_s,peak_rss_kb,rss_kb,user_s,sys_s,minflt,majflt");
    }

    const char *scenario_name = "";
    switch (cfg.scenario)
    {
    case Scenario::Pair:
        scenario_name = "pair";
        break;
    case Scenario::Fixed:
        scenario_name = "fixed";
        break;
    case Scenario::Churn:
        scenario_name = "churn";
        break;
    case Scenario::Cross:
        scenario_name = "cross";
        break;
    case Scenario::Realloc:
        scenario_name = "realloc";
        break;
    case Scenario::ReallocMulti:
        scenario_name = "realloc-multi";
        break;
    case Scenario::ReallocMixed:
        scenario_name = "realloc-mixed";
        break;
    case Scenario::Large:
        scenario_name = "large";
        break;
    case Scenario::Calloc:
        scenario_name = "calloc";
        break;
    }
    const double cycles_per_s = static_cast<double>(cycles) / wall;
    const double calls_per_s = static_cast<double>(calls) / wall;
    const double ns_per_cycle = cycles ? (wall * 1e9) / static_cast<double>(cycles) : 0.0;
    const double bytes_per_s = static_cast<double>(bytes) / wall;
    const double user_s = static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) / 1e6;
    const double sys_s = static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
    const char *touch_name = cfg.touch == Touch::None ? "none" : cfg.touch == Touch::First ? "first" : "pages";

    std::println("{},{},{},{},{},{},{},{},{},{:.6f},{},{},{:.1f},{:.1f},{:.2f},{},{:.1f},{},{},{:.6f},{:.6f},{},{}",
                 csv_clean(cfg.label), csv_clean(cfg.tag), scenario_name, touch_name, nthreads, cfg.size, cfg.min_size,
                 cfg.max_size, cfg.live, wall, cycles, calls, cycles_per_s, calls_per_s, ns_per_cycle, bytes,
                 bytes_per_s, peak_kb, rss_kb, user_s, sys_s, ru.ru_minflt, ru.ru_majflt);

    return 0;
}
