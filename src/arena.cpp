// Bootstrap arena, metadata pool, arena reservation, slot commit/purge, and extent/span page management.

#include "allocator_internal.hpp"

#include <atomic>
#include <cerrno>
#include <sys/mman.h>

namespace hpm
{

// ---------------------------------------------------------------------------
// Bootstrap allocation (only during global init / fork handlers)
// ---------------------------------------------------------------------------
void *bootstrap_alloc(size_t n) noexcept
{
    if (n == 0)
        n = 1;
    size_t off = g_bootstrap_off.load(std::memory_order_acquire);
    while (true)
    {
        size_t aligned = (off + 15) & ~size_t{15};
        if (aligned + n > sizeof(g_bootstrap))
        {
            errno = ENOMEM;
            return nullptr;
        }
        if (g_bootstrap_off.compare_exchange_weak(off, aligned + n, std::memory_order_release,
                                                  std::memory_order_relaxed))
        {
            return g_bootstrap + aligned;
        }
    }
}

// ---------------------------------------------------------------------------
// Metadata pool
// ---------------------------------------------------------------------------
static void *map_zero(size_t bytes) noexcept
{
    if (bytes == 0)
        bytes = kPageSize;
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
    (void)madvise(p, bytes, MADV_NOHUGEPAGE);
    return p;
}

void *meta_alloc(size_t size) noexcept
{
    size_t cls;
    void *result = nullptr;
    if (size <= kMetaBlock64)
        cls = kMetaBlock64;
    else if (size <= kMetaBlock256)
        cls = kMetaBlock256;
    else if (size <= kMetaBlock16K)
        cls = kMetaBlock16K;
    else
        return nullptr;

    g_meta.lock.lock();
    void **head = (cls == kMetaBlock64) ? &g_meta.free64 : (cls == kMetaBlock256) ? &g_meta.free256 : &g_meta.free16k;
    if (*head)
    {
        result = *head;
        *head = *reinterpret_cast<void **>(result);
    }
    else
    {
        uintptr_t p = (g_meta.cur + 63) & ~uintptr_t{63};
        if (p + cls > g_meta.end)
        {
            void *chunk = mmap(nullptr, kMetaChunkBytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (chunk == MAP_FAILED)
            {
                g_meta.lock.unlock();
                return nullptr;
            }
            (void)madvise(chunk, kMetaChunkBytes, MADV_NOHUGEPAGE);
            g_meta.cur = reinterpret_cast<uintptr_t>(chunk);
            g_meta.end = g_meta.cur + kMetaChunkBytes;
            p = (g_meta.cur + 63) & ~uintptr_t{63};
        }
        result = reinterpret_cast<void *>(p);
        g_meta.cur = p + cls;
    }
    g_meta.lock.unlock();
    return result;
}

void meta_free(void *ptr, size_t size) noexcept
{
    if (!ptr)
        return;
    size_t cls;
    void **head;
    if (size <= kMetaBlock64)
    {
        cls = kMetaBlock64;
        head = &g_meta.free64;
    }
    else if (size <= kMetaBlock256)
    {
        cls = kMetaBlock256;
        head = &g_meta.free256;
    }
    else if (size <= kMetaBlock16K)
    {
        cls = kMetaBlock16K;
        head = &g_meta.free16k;
    }
    else
        return;
    (void)cls;
    g_meta.lock.lock();
    *reinterpret_cast<void **>(ptr) = *head;
    *head = ptr;
    g_meta.lock.unlock();
}

// ---------------------------------------------------------------------------
// Arena management
// ---------------------------------------------------------------------------
static void *reserve_aligned(size_t bytes) noexcept
{
    if (bytes > static_cast<size_t>(-1) - kSlotSize)
        return nullptr;
    void *raw = mmap(nullptr, bytes + kSlotSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED)
        return nullptr;
    const uintptr_t start = (reinterpret_cast<uintptr_t>(raw) + kSlotSize - 1) & ~uintptr_t{kSlotSize - 1};
    const size_t head = start - reinterpret_cast<uintptr_t>(raw);
    const uintptr_t end = start + bytes;
    const size_t tail = reinterpret_cast<uintptr_t>(raw) + bytes + kSlotSize - end;
    if (head)
        (void)munmap(raw, head);
    if (tail)
        (void)munmap(reinterpret_cast<void *>(end), tail);
    return reinterpret_cast<void *>(start);
}

void purge_range(void *addr, size_t len, bool trim) noexcept
{
    int mode = trim ? kPurgeDontNeed : g_purge_mode.load(std::memory_order_acquire);
    if (mode == kPurgeDefer)
        mode = kPurgeFree;
    const int advice = (mode == kPurgeDontNeed) ? MADV_DONTNEED : MADV_FREE;
    (void)madvise(addr, len, advice);
    g_purge_count.fetch_add(1, std::memory_order_relaxed);
}

static void apply_small_thp(void *addr) noexcept
{
    const int mode = g_thp_mode.load(std::memory_order_acquire);
    const int advice = (mode == kThpAlways || mode == kThpCollapse) ? MADV_HUGEPAGE : MADV_NOHUGEPAGE;
    (void)madvise(addr, kSlotSize, advice);
}

void apply_large_thp(void *addr, uint32_t slots, size_t req) noexcept
{
    const int mode = g_thp_mode.load(std::memory_order_acquire);
    if (mode == kThpNever)
    {
        (void)madvise(addr, static_cast<size_t>(slots) * kSlotSize, MADV_NOHUGEPAGE);
        return;
    }
    size_t full = req / kSlotSize;
    if (full > slots)
        full = slots;
    if (full)
    {
        (void)madvise(addr, full * kSlotSize, MADV_HUGEPAGE);
    }
    if (full < slots)
    {
        (void)madvise(static_cast<char *>(addr) + full * kSlotSize, (slots - full) * kSlotSize, MADV_NOHUGEPAGE);
    }
}

static Arena *create_arena_locked(size_t want_bytes) noexcept
{
    if (want_bytes < kSlotSize)
        want_bytes = kSlotSize;
    if (want_bytes > static_cast<size_t>(-1) - kSlotSize)
        return nullptr;
    want_bytes = (want_bytes + kSlotSize - 1) / kSlotSize * kSlotSize;
    if (want_bytes / kSlotSize > 0xFFFFFFFFull)
        return nullptr;

    void *base = reserve_aligned(want_bytes);
    if (!base)
        return nullptr;

    Arena *a = static_cast<Arena *>(meta_alloc(kMetaBlock256));
    if (!a)
    {
        (void)munmap(base, want_bytes);
        return nullptr;
    }
    new (&a->lock) SpinLock();
    a->base = reinterpret_cast<uintptr_t>(base);
    a->size = want_bytes;
    a->slot_count = static_cast<uint32_t>(want_bytes / kSlotSize);
    a->pad0 = 0;
    a->large_cache = nullptr;
    a->large_cache_tail = nullptr;
    a->large_cache_bytes = 0;
    for (Extent *&b : a->buckets)
        b = nullptr;

    const size_t words = (a->slot_count + 63) / 64;
    a->map = static_cast<uintptr_t *>(map_zero(static_cast<size_t>(a->slot_count) * sizeof(uintptr_t)));
    a->slot_bits = static_cast<uint64_t *>(map_zero(words * sizeof(uint64_t)));
    a->committed_bits = static_cast<uint64_t *>(map_zero(words * sizeof(uint64_t)));
    a->class_map = g_class_map_enabled
                       ? static_cast<uint8_t *>(map_zero(static_cast<size_t>(a->slot_count) * kGranulesPerSlot))
                       : nullptr;
    if (!a->map || !a->slot_bits || !a->committed_bits || (g_class_map_enabled && !a->class_map))
    {
        if (a->map)
            (void)munmap(a->map, static_cast<size_t>(a->slot_count) * sizeof(uintptr_t));
        if (a->slot_bits)
            (void)munmap(a->slot_bits, words * sizeof(uint64_t));
        if (a->committed_bits)
            (void)munmap(a->committed_bits, words * sizeof(uint64_t));
        if (a->class_map)
            (void)munmap(a->class_map, static_cast<size_t>(a->slot_count) * kGranulesPerSlot);
        meta_free(a, kMetaBlock256);
        (void)munmap(base, want_bytes);
        return nullptr;
    }

    g_reserved_bytes.fetch_add(want_bytes, std::memory_order_relaxed);
    g_free_slots.fetch_add(a->slot_count, std::memory_order_relaxed);
    return a;
}

Arena *grow_arena(size_t min_bytes) noexcept
{
    g_dir_lock.lock();
    const size_t n = g_arena_count.load(std::memory_order_acquire);
    if (n >= kMaxArenas)
    {
        g_dir_lock.unlock();
        return nullptr;
    }

    size_t first = g_default_arena_bytes;
    if (min_bytes > first)
        first = min_bytes;
    if (first > static_cast<size_t>(-1) - kSlotSize)
    {
        g_dir_lock.unlock();
        return nullptr;
    }
    first = (first + kSlotSize - 1) / kSlotSize * kSlotSize;

    const size_t candidates[] = {
        first,
        8ull * 1024 * 1024 * 1024,
        1024ull * 1024 * 1024,
        256ull * 1024 * 1024,
        64ull * 1024 * 1024,
        16ull * 1024 * 1024,
        4ull * 1024 * 1024,
    };

    Arena *a = nullptr;
    for (size_t c : candidates)
    {
        if (c < min_bytes)
            continue;
        if (c < 4ull * 1024 * 1024)
            c = 4ull * 1024 * 1024;
        a = create_arena_locked(c);
        if (a)
            break;
    }
    if (a)
    {
        g_arenas[n] = a;
        g_arena_count.store(n + 1, std::memory_order_release);
    }
    g_dir_lock.unlock();
    return a;
}

uint32_t find_free_run_locked(Arena *a, uint32_t need, uint32_t align_slots) noexcept
{
    if (need == 0 || need > a->slot_count)
        return kNoBucket;
    if (align_slots == 0)
        align_slots = 1;
    uint32_t i = 0;
    while (i + need <= a->slot_count)
    {
        bool found_free = false;
        while (i < a->slot_count)
        {
            const uint32_t wi = i >> 6;
            const uint64_t bits = a->slot_bits[wi];
            if (bits == ~uint64_t{0})
            {
                i = (wi + 1) * 64;
                continue;
            }
            uint64_t avail = ~bits;
            if (i & 63)
                avail &= (~uint64_t{0}) << (i & 63);
            if (avail == 0)
            {
                i = (wi + 1) * 64;
                continue;
            }
            i = wi * 64 + static_cast<uint32_t>(std::countr_zero(avail));
            found_free = true;
            break;
        }
        if (!found_free || i + need > a->slot_count)
            return kNoBucket;
        if (i % align_slots)
        {
            i += align_slots - (i % align_slots);
            continue;
        }
        bool ok = true;
        for (uint32_t j = 1; j < need; ++j)
        {
            if (i + j >= a->slot_count || slot_used(a, i + j))
            {
                ok = false;
                i = i + j + 1;
                break;
            }
        }
        if (ok)
            return i;
    }
    return kNoBucket;
}

bool commit_range_locked(Arena *a, uint32_t start, uint32_t count) noexcept
{
    bool any = false;
    for (uint32_t j = 0; j < count; ++j)
    {
        if (!committed_get(a, start + j))
        {
            any = true;
            break;
        }
    }
    if (!any)
        return true;

    void *addr = reinterpret_cast<void *>(a->base + static_cast<size_t>(start) * kSlotSize);
    const size_t len = static_cast<size_t>(count) * kSlotSize;
    if (mprotect(addr, len, PROT_READ | PROT_WRITE) != 0)
        return false;

    uint64_t newly = 0;
    for (uint32_t j = 0; j < count; ++j)
    {
        if (!committed_get(a, start + j))
        {
            committed_set(a, start + j);
            ++newly;
        }
    }
    g_committed_slots.fetch_add(newly, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------
// Extent / span management
// ---------------------------------------------------------------------------
static inline bool granule_set(const Extent *e, uint32_t g) noexcept
{
    return (e->bitmap[g >> 6] >> (g & 63)) & 1u;
}

static inline void granule_set_bit(Extent *e, uint32_t g) noexcept
{
    e->bitmap[g >> 6] |= (uint64_t{1} << (g & 63));
}

static inline void granule_clear_bit(Extent *e, uint32_t g) noexcept
{
    e->bitmap[g >> 6] &= ~(uint64_t{1} << (g & 63));
}

static uint32_t find_run_in_extent(const Extent *e, uint32_t need) noexcept
{
    for (uint32_t i = 0; i + need <= kGranulesPerSlot;)
    {
        if (granule_set(e, i))
        {
            ++i;
            continue;
        }
        uint32_t j = 1;
        while (j < need && !granule_set(e, i + j))
            ++j;
        if (j == need)
            return i;
        i += j + 1;
    }
    return kNoBucket;
}

static void extent_recompute_largest(Extent *e) noexcept
{
    uint32_t best = 0, run = 0;
    for (uint32_t i = 0; i < kGranulesPerSlot; ++i)
    {
        if (granule_set(e, i))
        {
            run = 0;
        }
        else
        {
            ++run;
            if (run > best)
                best = run;
        }
    }
    e->largest_run = best;
}

static void extent_bucket_remove_locked(Arena *a, Extent *e) noexcept
{
    const uint32_t b = e->bucket;
    if (b == kNoBucket)
        return;
    if (e->prev)
        e->prev->next = e->next;
    else
        a->buckets[b] = e->next;
    if (e->next)
        e->next->prev = e->prev;
    e->prev = nullptr;
    e->next = nullptr;
    e->bucket = kNoBucket;
}

static void extent_bucket_insert_locked(Arena *a, Extent *e) noexcept
{
    if (e->largest_run == 0)
    {
        e->bucket = kNoBucket;
        return;
    }
    const uint32_t b = static_cast<uint32_t>(std::bit_width(e->largest_run) - 1);
    e->bucket = b;
    e->prev = nullptr;
    e->next = a->buckets[b];
    if (e->next)
        e->next->prev = e;
    a->buckets[b] = e;
}

static void extent_rebucket_locked(Arena *a, Extent *e) noexcept
{
    extent_bucket_remove_locked(a, e);
    extent_bucket_insert_locked(a, e);
}

static Extent *extent_create_locked(Arena *a, uint32_t slot) noexcept
{
    Extent *e = static_cast<Extent *>(meta_alloc(kMetaBlock16K));
    if (!e)
        return nullptr;
    std::memset(e, 0, sizeof(Extent));
    e->arena = a;
    e->slot = slot;
    e->free_granules = kGranulesPerSlot;
    e->largest_run = kGranulesPerSlot;
    e->bucket = kNoBucket;
    store_entry(&a->map[slot], reinterpret_cast<uintptr_t>(e));
    extent_bucket_insert_locked(a, e);
    g_extent_slots.fetch_add(1, std::memory_order_relaxed);
    return e;
}

static Span *extent_alloc_span_locked(Arena *a, uint32_t class_id) noexcept
{
    const ClassInfo &ci = kClasses[class_id];
    const uint32_t need = ci.granules;

    Extent *ex = nullptr;
    uint32_t start = kNoBucket;

    const uint32_t first_bucket = (need == 0) ? 0 : static_cast<uint32_t>(std::bit_width(need) - 1);
    for (uint32_t b = first_bucket; b < 10 && !ex; ++b)
    {
        for (Extent *e = a->buckets[b]; e; e = e->next)
        {
            if (e->largest_run < need)
                continue;
            const uint32_t s = find_run_in_extent(e, need);
            if (s != kNoBucket)
            {
                ex = e;
                start = s;
                break;
            }
        }
    }

    if (!ex)
    {
        const uint32_t slot = find_free_run_locked(a, 1, 1);
        if (slot == kNoBucket)
            return nullptr;
        slot_set(a, slot);
        g_free_slots.fetch_sub(1, std::memory_order_relaxed);
        if (!commit_range_locked(a, slot, 1))
        {
            slot_clear(a, slot);
            g_free_slots.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        apply_small_thp(reinterpret_cast<void *>(a->base + static_cast<size_t>(slot) * kSlotSize));
        ex = extent_create_locked(a, slot);
        if (!ex)
        {
            slot_clear(a, slot);
            g_free_slots.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        start = find_run_in_extent(ex, need);
        if (start == kNoBucket)
            return nullptr;
    }

    Span *sp = static_cast<Span *>(meta_alloc(kMetaBlock64));
    if (!sp)
        return nullptr;

    for (uint32_t j = 0; j < need; ++j)
    {
        granule_set_bit(ex, start + j);
        ex->entries[start + j] = GranuleEntry{static_cast<uint16_t>(class_id), 0, 0, sp};
        ex->class_ids[start + j] = static_cast<uint16_t>(class_id);
    }
    if (class_id <= kNormalClasses && a->class_map)
    {
        uint8_t *cm = a->class_map + static_cast<size_t>(ex->slot) * kGranulesPerSlot + start;
        std::memset(cm, static_cast<int>(class_id), need);
    }
    ex->free_granules -= need;
    extent_recompute_largest(ex);
    extent_rebucket_locked(a, ex);

    sp->free_head = nullptr;
    sp->free_count = ci.capacity;
    sp->bump_off = 0;
    sp->bump_end = ci.capacity * ci.stride;
    sp->class_id = static_cast<uint16_t>(class_id);
    sp->capacity = static_cast<uint16_t>(ci.capacity);
    sp->granule_count = static_cast<uint16_t>(need);
    sp->flags = 0;
    sp->start_granule = start;
    sp->extent = ex;
    sp->next = nullptr;
    sp->prev = nullptr;
    sp->object_size = ci.stride;
    g_span_count.fetch_add(1, std::memory_order_relaxed);
    return sp;
}

Span *page_alloc_span(uint32_t class_id) noexcept
{
    const size_t n = g_arena_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i)
    {
        Arena *a = g_arenas[i];
        a->lock.lock();
        Span *s = extent_alloc_span_locked(a, class_id);
        a->lock.unlock();
        if (s)
            return s;
    }
    Arena *na = grow_arena(0);
    if (!na)
        return nullptr;
    na->lock.lock();
    Span *s = extent_alloc_span_locked(na, class_id);
    na->lock.unlock();
    return s;
}

uintptr_t span_start_addr(const Span *s) noexcept
{
    const Extent *e = s->extent;
    return e->arena->base + static_cast<size_t>(e->slot) * kSlotSize +
           static_cast<size_t>(s->start_granule) * kPageSize;
}

void page_release_span(Span *sp) noexcept
{
    Extent *e = sp->extent;
    Arena *a = e->arena;
    const uint32_t start = sp->start_granule;
    const uint32_t need = sp->granule_count;

    a->lock.lock();
    const bool will_die = (e->free_granules + need == kGranulesPerSlot);
    if (will_die)
    {
        // Freeze this extent while the pages are discarded.  The granules and
        // the slot stay marked used, so neither this span nor the extent can be
        // handed out until the post-purge bookkeeping below runs.
        extent_bucket_remove_locked(a, e);
    }
    a->lock.unlock();

    if (will_die)
    {
        purge_range(reinterpret_cast<void *>(a->base + static_cast<size_t>(e->slot) * kSlotSize), kSlotSize);
    }
    else
    {
        purge_range(reinterpret_cast<void *>(span_start_addr(sp)), static_cast<size_t>(need) * kPageSize);
    }

    a->lock.lock();
    for (uint32_t j = 0; j < need; ++j)
    {
        e->entries[start + j] = GranuleEntry{};
        e->class_ids[start + j] = 0;
        granule_clear_bit(e, start + j);
    }
    if (a->class_map)
    {
        std::memset(a->class_map + static_cast<size_t>(e->slot) * kGranulesPerSlot + start, 0, need);
    }
    e->free_granules += need;
    extent_recompute_largest(e);

    // Another span in this extent may have finished releasing while we were
    // purging; free the extent only after the last user has accounted for its
    // granules.  The slot stays marked used until this point.
    const bool free_extent = (e->free_granules == kGranulesPerSlot);
    if (free_extent)
    {
        extent_bucket_remove_locked(a, e);
        store_entry(&a->map[e->slot], 0);
        slot_clear(a, e->slot);
        g_free_slots.fetch_add(1, std::memory_order_relaxed);
        g_extent_slots.fetch_sub(1, std::memory_order_relaxed);
    }
    else
    {
        extent_rebucket_locked(a, e);
    }
    a->lock.unlock();

    g_span_count.fetch_sub(1, std::memory_order_relaxed);
    meta_free(sp, kMetaBlock64);
    if (free_extent)
        meta_free(e, kMetaBlock16K);
}

} // namespace hpm
