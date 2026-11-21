// Shared allocator types, constants, global state, and cross-translation-unit interfaces.
// This header is private to the library implementation.

// Name         Meaning
// Arena        A large reserved virtual address range (default 64GiB)
// Slot         2MiB VA unit inside an arena, aligned to THP
// Granule      4kiB VA unit inside a slot, aligned to page size
// Extent       512 granules + metadata, when a slot is used for small object allocation
// Span         Several continuous granules in an extent, spectalized for a certain size_class allocation
// Class        Total 612(68*9) kinds of size_class with different size and alignment, see size_class.hpp
// TCache       Per-thread, per-class object LIFO cache
// CentralBin
// LargeDesc    Several continuous slots for an object exceeds 2MiB
// DirectMap    In support direct mmap VA, when arena not available or alignment exceeds 2MiB
// Pagemap      Per-slot `slot -> metadata pointer` array of an arena
// class_map    Per-granule bytemap of an arena, 0: not normal class, 1-68: normal class id
// GranuleEntry 16B immutable `granule -> (class_id,Span*)` entry of an extent

#pragma once

#include "size_class.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sched.h>

#if defined(__GNUC__) || defined(__clang__)
#define HPM_EXPORT __attribute__((visibility("default")))
#define HPM_TLS __attribute__((tls_model("initial-exec")))
#define HPM_LIKELY(x) __builtin_expect(!!(x), 1)
#define HPM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define HPM_EXPORT
#define HPM_TLS
#define HPM_LIKELY(x) (x)
#define HPM_UNLIKELY(x) (x)
#endif

namespace hpm
{

// ---------------------------------------------------------------------------
// Constants and low-level utilities
// ---------------------------------------------------------------------------
inline constexpr size_t kMaxArenas = 64;
inline constexpr size_t kMetaChunkBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMetaBlock64 = 64;
inline constexpr size_t kMetaBlock256 = 256;
inline constexpr size_t kMetaBlock16K = 16384;
inline constexpr size_t kDirectCapacity = 4096;
inline constexpr size_t kDirectTombstone = 1;
inline constexpr uint32_t kNoBucket = 0xFFFFFFFFu;
inline constexpr uint32_t kInBin = 1u;
inline constexpr uint32_t kRetainedSpan = 2u;
inline constexpr uint32_t kCentralShards = 8;
static_assert((kCentralShards & (kCentralShards - 1)) == 0);
inline constexpr uint32_t kSpanShardShift = 8;
inline constexpr uint16_t kSpanShardMask = kCentralShards - 1;
inline constexpr size_t kDefaultArenaBytes = 64ull * 1024ull * 1024ull * 1024ull;
inline constexpr size_t kTcacheByteTarget = 512ull * 1024ull;
inline constexpr size_t kLargeCacheBytes = 64ull * 1024ull * 1024ull;
// Growing a normal-class object by realloc() otherwise copies the whole old
// object every time.  On this machine the standalone-mapping + mremap path
// was slower than a geometric class allocation at every size we measured
// because the syscall and direct-table cost exceeded the copy it avoided.
// Keep promotion available as a tuning option, but default to the span
// allocator with realloc slack.  Existing direct mappings still use mremap.
inline constexpr size_t kReallocDirectThreshold = static_cast<size_t>(-1);
// For objects handled by the span allocator, over-allocate by this percentage
// on a growing realloc.  Vector-style growth is about 1.5x, so 100% slack makes
// the next growth fit without another copy while keeping the tradeoff bounded
// by the 2 MiB class floor.
inline constexpr uint32_t kReallocSlackPercent = 100;
inline constexpr size_t kEmptySpanMaxBytes = 256ull * 1024ull;
// Cross-thread workloads have no stable live set: spans become completely
// free as soon as the consumer drains them.  If each shard retains only one
// span, producer refill immediately needs a fresh span and the resulting
// page-release/realloc churn dominates both the arena lock and the purge
// syscalls.  Keep a small-span budget and a separate large-span budget: 256
// KiB/1 MiB class spans are 2 MiB and are common in copy-bound realloc, but
// letting them consume the small-span budget would make cross-small worse.
inline constexpr size_t kEmptySpanShardBytes = 512ull * 1024ull;
inline constexpr uint32_t kEmptySpanPerShard = 4;
inline constexpr size_t kLargeEmptySpanMaxBytes = 2ull * 1024ull * 1024ull;
inline constexpr size_t kLargeEmptySpanShardBytes = 4ull * 1024ull * 1024ull;
inline constexpr uint32_t kLargeEmptySpanPerShard = 2;
static_assert(kEmptySpanShardBytes >= kEmptySpanMaxBytes);
static_assert(kLargeEmptySpanMaxBytes >= kEmptySpanMaxBytes);
static_assert(kLargeEmptySpanShardBytes >= kLargeEmptySpanMaxBytes);

enum ThpMode : int
{
    kThpAuto = 0,
    kThpAlways = 1,
    kThpNever = 2,
    kThpCollapse = 3
};
enum PurgeMode : int
{
    kPurgeFree = 0,
    kPurgeDontNeed = 1,
    kPurgeDefer = 2
};

// The lock owns a cache line by itself to avoid false-sharing
struct alignas(64) SpinLock
{
    std::atomic<bool> locked{false};
    constexpr SpinLock() noexcept = default;
    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;

    static inline void pause_cpu() noexcept
    {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }

    void lock() noexcept
    {
        while (true)
        {
            if (!locked.load(std::memory_order_acquire) && !locked.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            for (unsigned i = 0; i < 64; ++i)
            {
                if (!locked.load(std::memory_order_acquire))
                    break;
                pause_cpu();
            }
            if (locked.load(std::memory_order_acquire))
                sched_yield();
        }
    }
    void unlock() noexcept
    {
        locked.store(false, std::memory_order_release);
    }
};
static_assert(sizeof(SpinLock) == 64);

static inline void *load_next(const void *p) noexcept
{
    void *v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static inline void store_next(void *p, void *v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}

static inline uintptr_t load_entry(uintptr_t *slot) noexcept
{
    return std::atomic_ref<uintptr_t>(*slot).load(std::memory_order_acquire);
}

static inline void store_entry(uintptr_t *slot, uintptr_t value) noexcept
{
    std::atomic_ref<uintptr_t>(*slot).store(value, std::memory_order_relaxed);
}

struct Arena;
struct Extent;
struct TCache;
struct LargeDesc;

// ---------------------------------------------------------------------------
// Core data structures
// ---------------------------------------------------------------------------

struct alignas(64) Span
{
    void *free_head;     // central-bin protected
    uint32_t free_count; // central-bin protected
    uint32_t bump_off;   // central-bin protected
    uint32_t bump_end;   // capacity * object_size
    uint16_t class_id;
    uint16_t capacity;
    uint16_t granule_count;
    uint16_t flags;
    uint32_t start_granule;
    Extent *extent;
    Span *next;
    Span *prev;
    uint32_t object_size;
};
static_assert(sizeof(Span) == 64);

static inline uint32_t span_shard(const Span *s) noexcept
{
    return (s->flags >> kSpanShardShift) & kSpanShardMask;
}

static inline void set_span_shard(Span *s, uint32_t shard) noexcept
{
    s->flags = static_cast<uint16_t>((s->flags & ~(kSpanShardMask << kSpanShardShift)) |
                                     ((shard & kSpanShardMask) << kSpanShardShift));
}

struct alignas(16) GranuleEntry
{
    // class_id is first so the free() fast path can validate and find the
    // tcache bin without loading the Span pointer at all.
    uint16_t class_id;
    uint16_t flags;
    uint32_t pad;
    Span *span;
};
static_assert(sizeof(GranuleEntry) == 16);

struct Extent
{
    Arena *arena;
    uint32_t slot;
    uint32_t free_granules;
    uint32_t largest_run;
    uint32_t bucket;
    Extent *prev;
    Extent *next;
    uint64_t bitmap[kGranulesPerSlot / 64];
    GranuleEntry entries[kGranulesPerSlot];
    // Class-only mirror of entries[] for the free/realloc fast path.  The
    // fast path only needs the class ID, not the Span pointer; this compact
    // 1 KiB array keeps the lookup cache footprint well below the 8 KiB
    // GranuleEntry table.  Slow paths still read entries[] for the Span.
    uint16_t class_ids[kGranulesPerSlot];
};
static_assert(sizeof(Extent) <= kMetaBlock16K);

struct alignas(64) Arena
{
    uintptr_t base;
    size_t size;
    uint32_t slot_count;
    uint32_t pad0;
    uintptr_t *map;           // per-slot -> `Extent*` or `LargeDesc*|0x01` or `0`
    uint64_t *slot_bits;      // used slots
    uint64_t *committed_bits; // slots ever mprotect'ed
    // One byte per 4 KiB granule for normal classes (1..kNormalClasses).
    // Aligned classes, large runs, and unused granules are zero and take the
    // existing Extent/GranuleEntry slow path.  The map is mmap'ed lazily, so a
    // pair/fixed workload only touches the few class-map pages it actually
    // needs even though the virtual array covers the whole 64 GiB arena.
    uint8_t *class_map;     // granule -> normal class id or 0
    Extent *buckets[10];    // by largest free granule run
    LargeDesc *large_cache; // freed large runs, slot_bits still set
    LargeDesc *large_cache_tail;
    size_t large_cache_bytes;
    SpinLock lock;
};
static_assert(sizeof(Arena) <= kMetaBlock256);

struct LargeDesc
{
    Arena *arena;
    uint32_t base_slot;
    uint32_t slot_count;
    size_t req;
    size_t usable;
    size_t align;
    uint32_t flags;
    LargeDesc *next;
    LargeDesc *prev;
};
static_assert(sizeof(LargeDesc) <= kMetaBlock64);

struct alignas(64) CentralBin
{
    SpinLock lock; // occupies its own cache line
    Span *spans = nullptr;
    uint32_t empty_spans = 0; // full-free small spans kept for reuse
    size_t empty_bytes = 0;
    uint32_t large_empty_spans = 0; // 256 KiB..2 MiB spans
    size_t large_empty_bytes = 0;
};

struct alignas(16) TCacheBin
{
    void *head;
    uint16_t len;
    uint16_t max;
    uint16_t batch;
    uint16_t pad;
};

struct alignas(64) TCache
{
    TCacheBin bins[kTotalClasses + 1];
    size_t cached_bytes;
    Arena *last_arena;
    uint32_t shard;
};
static_assert(sizeof(TCacheBin) == 16);
static_assert(offsetof(TCache, bins) == 0);
static_assert(sizeof(TCache) <= kMetaBlock16K);

struct DirectEntry
{
    uintptr_t base;
    size_t len;
    size_t req;
    size_t align;
};

struct MetaPool
{
    SpinLock lock;
    uintptr_t cur = 0;
    uintptr_t end = 0;
    void *free64 = nullptr;  // for Span and LargeDesc
    void *free256 = nullptr; // for Arena
    void *free16k = nullptr; // for Extent and TCache
};

// ---------------------------------------------------------------------------
// Global state shared by the allocator translation units
// ---------------------------------------------------------------------------
extern std::atomic<int> g_state;
extern SpinLock g_dir_lock;
extern Arena *g_arenas[kMaxArenas];
extern std::atomic<size_t> g_arena_count;

extern MetaPool g_meta;

extern CentralBin g_bins[kTotalClasses + 1][kCentralShards];

extern DirectEntry g_direct[kDirectCapacity];
extern SpinLock g_direct_lock;

extern std::atomic<int> g_thp_mode;
extern std::atomic<int> g_purge_mode;

extern std::atomic<uint64_t> g_reserved_bytes;
extern std::atomic<uint64_t> g_committed_slots;
extern std::atomic<uint64_t> g_free_slots;
extern std::atomic<uint64_t> g_span_count;
extern std::atomic<uint64_t> g_extent_slots;
extern std::atomic<uint64_t> g_purge_count;
extern std::atomic<uint32_t> g_tcache_seq;

extern size_t g_default_arena_bytes;
extern size_t g_realloc_direct_threshold;
extern uint32_t g_realloc_slack_percent;
extern bool g_class_map_enabled;

// Keep the thread-local definitions visible in every translation unit.
// An extern thread_local declaration alone makes GCC/Clang emit a TLS init guard
// around every access in the hot malloc/free path, which costs 20%+ on pair workloads.
inline thread_local TCache *tls_cache HPM_TLS = nullptr;
inline thread_local bool tls_in_init HPM_TLS = false;
inline thread_local bool tls_in_fork HPM_TLS = false;

extern unsigned char g_bootstrap[64 * 1024];
extern std::atomic<size_t> g_bootstrap_off;

extern pthread_key_t g_thread_key;
extern std::atomic<bool> g_key_created;
extern std::atomic<int> g_hooks_state;

// ---------------------------------------------------------------------------
// Small helpers shared by the allocation paths
// ---------------------------------------------------------------------------
static inline bool committed_get(const Arena *a, uint32_t slot) noexcept
{
    return (a->committed_bits[slot >> 6] >> (slot & 63)) & 1u;
}

static inline void committed_set(Arena *a, uint32_t slot) noexcept
{
    a->committed_bits[slot >> 6] |= (uint64_t{1} << (slot & 63));
}

static inline bool slot_used(const Arena *a, uint32_t slot) noexcept
{
    return (a->slot_bits[slot >> 6] >> (slot & 63)) & 1u;
}

static inline void slot_set(Arena *a, uint32_t slot) noexcept
{
    a->slot_bits[slot >> 6] |= (uint64_t{1} << (slot & 63));
}

static inline void slot_clear(Arena *a, uint32_t slot) noexcept
{
    a->slot_bits[slot >> 6] &= ~(uint64_t{1} << (slot & 63));
}

static inline bool is_bootstrap(const void *p) noexcept
{
    const uintptr_t u = reinterpret_cast<uintptr_t>(p);
    const uintptr_t b = reinterpret_cast<uintptr_t>(g_bootstrap);
    return u >= b && u < b + sizeof(g_bootstrap);
}

static inline Arena *find_arena(TCache *tc, const void *ptr) noexcept
{
    const uintptr_t u = reinterpret_cast<uintptr_t>(ptr);
    if (tc && tc->last_arena)
    {
        Arena *a = tc->last_arena;
        if (u - a->base < a->size)
            return a;
    }
    const size_t n = g_arena_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i)
    {
        Arena *a = g_arenas[i];
        if (u - a->base < a->size)
        {
            if (tc)
                tc->last_arena = a;
            return a;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Internal interfaces between translation units
// ---------------------------------------------------------------------------

// arena.cpp
void *bootstrap_alloc(size_t) noexcept;
void *meta_alloc(size_t) noexcept;
void meta_free(void *, size_t) noexcept;
Arena *grow_arena(size_t) noexcept;
void purge_range(void *, size_t, bool trim = false) noexcept;
void apply_large_thp(void *, uint32_t, size_t) noexcept;
uint32_t find_free_run_locked(Arena *, uint32_t, uint32_t) noexcept;
bool commit_range_locked(Arena *, uint32_t, uint32_t) noexcept;
Span *page_alloc_span(uint32_t) noexcept;
void page_release_span(Span *) noexcept;
uintptr_t span_start_addr(const Span *) noexcept;

// central.cpp
void fatal_free(const void *) noexcept;
void *refill(TCache *, uint32_t) noexcept;
void free_no_tcache(Span *, void *, uint32_t) noexcept;
void flush_slow(TCache *, uint32_t) noexcept;
void flush_all(TCache *) noexcept;
void trim_retained_spans() noexcept;

// large.cpp
void *direct_alloc(size_t, size_t) noexcept;
void direct_free(void *) noexcept;
bool direct_usable(const void *, size_t *) noexcept;
void *direct_realloc(void *, size_t) noexcept;
void *large_alloc(size_t, size_t) noexcept;
void large_free(LargeDesc *, void *) noexcept;
bool large_grow_in_place(LargeDesc *, size_t) noexcept;
bool large_cache_flush(Arena *, bool) noexcept;

// runtime.cpp
bool ensure_init() noexcept;
TCache *attach_tcache() noexcept;
void *malloc_slow_attach_helper(size_t) noexcept;
int apply_option(const char *, size_t, const char *, size_t) noexcept;

// malloc.cpp
void *malloc_impl(size_t) noexcept;
void *malloc_with_cache(TCache *, size_t) noexcept;
void *class_alloc(TCache *, uint32_t) noexcept;
void free_impl(void *) noexcept;
void *calloc_impl(size_t, size_t) noexcept;
void *realloc_impl(void *, size_t) noexcept;
void *memalign_impl(size_t, size_t) noexcept;
size_t usable_impl(const void *) noexcept;

} // namespace hpm
