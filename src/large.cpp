// Standalone direct mappings and slot-run large objects, including mremap
// based direct realloc and the large-run cache.

#include "allocator_internal.hpp"

#include <cerrno>
#include <sys/mman.h>

namespace hpm
{

// ---------------------------------------------------------------------------
// Large runs and direct mappings
// ---------------------------------------------------------------------------
static bool direct_insert_locked(uintptr_t base, size_t len, size_t req, size_t align) noexcept
{
    const size_t mask = kDirectCapacity - 1;
    size_t start = (base >> 12) & mask;
    size_t first_tomb = kDirectCapacity;
    for (size_t probe = 0; probe < kDirectCapacity; ++probe)
    {
        const size_t i = (start + probe) & mask;
        DirectEntry &e = g_direct[i];
        if (e.base == base)
        {
            e.len = len;
            e.req = req;
            e.align = align;
            return true;
        }
        if (e.base == kDirectTombstone && first_tomb == kDirectCapacity)
        {
            first_tomb = i;
        }
        if (e.base == 0)
        {
            DirectEntry &slot = (first_tomb != kDirectCapacity) ? g_direct[first_tomb] : e;
            slot.base = base;
            slot.len = len;
            slot.req = req;
            slot.align = align;
            return true;
        }
    }
    if (first_tomb != kDirectCapacity)
    {
        DirectEntry &slot = g_direct[first_tomb];
        slot.base = base;
        slot.len = len;
        slot.req = req;
        slot.align = align;
        return true;
    }
    return false;
}

static DirectEntry *direct_find_locked(uintptr_t base) noexcept
{
    const size_t mask = kDirectCapacity - 1;
    size_t start = (base >> 12) & mask;
    for (size_t probe = 0; probe < kDirectCapacity; ++probe)
    {
        DirectEntry &e = g_direct[(start + probe) & mask];
        if (e.base == base)
            return &e;
        if (e.base == 0)
            return nullptr;
    }
    return nullptr;
}

void *direct_alloc(size_t n, size_t align) noexcept
{
    if (n == 0)
        n = 1;
    if (n > static_cast<size_t>(-1) - kPageSize)
        return nullptr;
    const size_t len = (n + kPageSize - 1) / kPageSize * kPageSize;
    // Only honor the alignment that was actually requested.  For normal
    // allocations this is just a page, not a whole 2 MiB slot; rounding every
    // direct mapping up to a slot made realloc promotion pay two extra
    // munmap() calls and inflated the VMA footprint.
    const size_t a = align < kPageSize ? kPageSize : align;
    if (len > static_cast<size_t>(-1) - a)
        return nullptr;

    void *raw = mmap(nullptr, len + a, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED)
        return nullptr;
    const uintptr_t base = (reinterpret_cast<uintptr_t>(raw) + a - 1) & ~(a - 1);
    const size_t head = base - reinterpret_cast<uintptr_t>(raw);
    const uintptr_t end = base + len;
    const size_t tail = reinterpret_cast<uintptr_t>(raw) + len + a - end;
    if (head)
        (void)munmap(raw, head);
    if (tail)
        (void)munmap(reinterpret_cast<void *>(end), tail);

    g_direct_lock.lock();
    const bool ok = direct_insert_locked(base, len, n, align);
    g_direct_lock.unlock();
    if (!ok)
    {
        (void)munmap(reinterpret_cast<void *>(base), len);
        errno = ENOMEM;
        return nullptr;
    }
    apply_large_thp(reinterpret_cast<void *>(base), static_cast<uint32_t>((len + kSlotSize - 1) / kSlotSize), n);
    return reinterpret_cast<void *>(base);
}

void direct_free(void *p) noexcept
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(p);
    g_direct_lock.lock();
    DirectEntry *e = direct_find_locked(base);
    if (!e)
    {
        g_direct_lock.unlock();
        fatal_free(p);
    }
    const size_t len = e->len;
    e->base = kDirectTombstone;
    g_direct_lock.unlock();
    (void)munmap(p, len);
}

bool direct_usable(const void *p, size_t *out) noexcept
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(p);
    g_direct_lock.lock();
    DirectEntry *e = direct_find_locked(base);
    if (!e)
    {
        g_direct_lock.unlock();
        return false;
    }
    *out = e->len;
    g_direct_lock.unlock();
    return true;
}

// Grow a standalone mapping without a user-space copy.  Most realloc-heavy
// buffers promoted by realloc_impl() arrive here after their first growth, so
// this is the path that removes the memmove that dominated multi-threaded
// 64 KiB..1 MiB realloc.  Aligned mappings keep the old copy path unless the
// kernel can preserve the stronger alignment (mremap only guarantees page
// alignment).
void *direct_realloc(void *p, size_t n) noexcept
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(p);
    size_t old_len = 0;
    size_t old_align = 0;
    g_direct_lock.lock();
    DirectEntry *e = direct_find_locked(base);
    if (!e)
    {
        g_direct_lock.unlock();
        fatal_free(p);
    }
    old_len = e->len;
    old_align = e->align;
    const size_t old_req = e->req;
    if (n <= old_len)
    {
        e->req = n;
        g_direct_lock.unlock();
        return p;
    }
    if (n > static_cast<size_t>(-1) - kPageSize)
    {
        g_direct_lock.unlock();
        errno = ENOMEM;
        return nullptr;
    }
    const size_t new_len = (n + kPageSize - 1) / kPageSize * kPageSize;
    if (old_align <= kPageSize)
    {
        // Remove the old base from the table before mremap() releases it.
        // Otherwise another thread can mmap() that address and insert its own
        // entry before we update the table, and we would then tombstone the
        // wrong mapping.
        e->base = kDirectTombstone;
        g_direct_lock.unlock();

        void *np = mremap(p, old_len, new_len, MREMAP_MAYMOVE);
        if (np != MAP_FAILED)
        {
            const uintptr_t nbase = reinterpret_cast<uintptr_t>(np);
            g_direct_lock.lock();
            // The tombstone left by the removal guarantees a free slot, even
            // when the table was otherwise full.
            const bool ok = direct_insert_locked(nbase, new_len, n, old_align);
            g_direct_lock.unlock();
            if (ok)
                return np;
            // Do not return an untracked mapping that free() could not resolve.
            fatal_free(np);
        }

        // mremap() failed and the original mapping is still intact.  Restore
        // its table entry before using the generic alloc+copy fallback.
        g_direct_lock.lock();
        (void)direct_insert_locked(base, old_len, old_req, old_align);
        g_direct_lock.unlock();
    }
    else
    {
        g_direct_lock.unlock();
    }

    void *np = direct_alloc(n, old_align);
    if (!np)
        return nullptr;
    std::memcpy(np, p, old_len);
    direct_free(p);
    return np;
}

static inline void large_cache_unlink_locked(Arena *a, LargeDesc *d) noexcept
{
    if (d->prev)
        d->prev->next = d->next;
    else
        a->large_cache = d->next;
    if (d->next)
        d->next->prev = d->prev;
    else
        a->large_cache_tail = d->prev;
    d->prev = nullptr;
    d->next = nullptr;
    a->large_cache_bytes -= static_cast<size_t>(d->slot_count) * kSlotSize;
}

static inline void large_cache_push_locked(Arena *a, LargeDesc *d) noexcept
{
    d->prev = nullptr;
    d->next = a->large_cache;
    if (a->large_cache)
        a->large_cache->prev = d;
    else
        a->large_cache_tail = d;
    a->large_cache = d;
    a->large_cache_bytes += static_cast<size_t>(d->slot_count) * kSlotSize;
}

// Purge a detached list of cached runs outside the arena lock.  The slots stay
// marked used until the madvise completes, so nobody can reuse them while the
// kernel is discarding the pages.
static void large_cache_purge_list(LargeDesc *list, bool trim) noexcept
{
    while (list)
    {
        LargeDesc *next = list->next;
        Arena *a = list->arena;
        const uintptr_t addr = a->base + static_cast<size_t>(list->base_slot) * kSlotSize;
        purge_range(reinterpret_cast<void *>(addr), static_cast<size_t>(list->slot_count) * kSlotSize, trim);
        a->lock.lock();
        for (uint32_t j = 0; j < list->slot_count; ++j)
        {
            slot_clear(a, list->base_slot + j);
        }
        g_free_slots.fetch_add(list->slot_count, std::memory_order_relaxed);
        a->lock.unlock();
        meta_free(list, kMetaBlock64);
        list = next;
    }
}

bool large_cache_flush(Arena *a, bool trim) noexcept
{
    a->lock.lock();
    LargeDesc *list = a->large_cache;
    if (!list)
    {
        a->lock.unlock();
        return false;
    }
    a->large_cache = nullptr;
    a->large_cache_tail = nullptr;
    a->large_cache_bytes = 0;
    a->lock.unlock();
    large_cache_purge_list(list, trim);
    return true;
}

void *large_alloc(size_t n, size_t align) noexcept
{
    if (n == 0)
        n = 1;
    if (n > static_cast<size_t>(-1) - kSlotSize)
    {
        errno = ENOMEM;
        return nullptr;
    }
    if (align > kSlotSize)
    {
        return direct_alloc(n, align);
    }
    const size_t k64 = (n + kSlotSize - 1) / kSlotSize;
    if (k64 == 0 || k64 > 0xFFFFFFFFull)
    {
        return direct_alloc(n, align);
    }
    const uint32_t slots = static_cast<uint32_t>(k64);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const size_t arena_count = g_arena_count.load(std::memory_order_acquire);
        for (size_t i = 0; i < arena_count; ++i)
        {
            Arena *a = g_arenas[i];
            if (a->slot_count < slots)
                continue;
            a->lock.lock();
            LargeDesc *cached = nullptr;
            for (LargeDesc *d = a->large_cache; d; d = d->next)
            {
                if (d->slot_count < slots)
                    continue;
                if (!cached || d->slot_count < cached->slot_count)
                    cached = d;
            }
            if (cached)
            {
                large_cache_unlink_locked(a, cached);
                cached->req = n;
                cached->usable = static_cast<size_t>(cached->slot_count) * kSlotSize;
                cached->align = align;
                cached->flags = 0;
                const uintptr_t tagged = reinterpret_cast<uintptr_t>(cached) | 1u;
                for (uint32_t j = 0; j < cached->slot_count; ++j)
                {
                    store_entry(&a->map[cached->base_slot + j], tagged);
                }
                void *addr = reinterpret_cast<void *>(a->base + static_cast<size_t>(cached->base_slot) * kSlotSize);
                a->lock.unlock();
                return addr;
            }
            const uint32_t start = find_free_run_locked(a, slots, 1);
            if (start == kNoBucket)
            {
                a->lock.unlock();
                continue;
            }
            for (uint32_t j = 0; j < slots; ++j)
                slot_set(a, start + j);
            if (!commit_range_locked(a, start, slots))
            {
                for (uint32_t j = 0; j < slots; ++j)
                    slot_clear(a, start + j);
                a->lock.unlock();
                continue;
            }
            LargeDesc *d = static_cast<LargeDesc *>(meta_alloc(kMetaBlock64));
            if (!d)
            {
                for (uint32_t j = 0; j < slots; ++j)
                    slot_clear(a, start + j);
                a->lock.unlock();
                continue;
            }
            d->arena = a;
            d->base_slot = start;
            d->slot_count = slots;
            d->req = n;
            d->usable = static_cast<size_t>(slots) * kSlotSize;
            d->align = align;
            d->flags = 0;
            const uintptr_t tagged = reinterpret_cast<uintptr_t>(d) | 1u;
            for (uint32_t j = 0; j < slots; ++j)
            {
                store_entry(&a->map[start + j], tagged);
            }
            g_free_slots.fetch_sub(slots, std::memory_order_relaxed);
            a->lock.unlock();
            void *addr = reinterpret_cast<void *>(a->base + static_cast<size_t>(start) * kSlotSize);
            apply_large_thp(addr, slots, n);
            return addr;
        }
        if (attempt == 0 && !grow_arena(n))
            break;
    }
    return direct_alloc(n, align);
}

bool large_grow_in_place(LargeDesc *d, size_t n) noexcept
{
    const size_t k64 = (n + kSlotSize - 1) / kSlotSize;
    if (k64 == 0 || k64 > 0xFFFFFFFFull)
        return false;
    const uint32_t new_slots = static_cast<uint32_t>(k64);
    if (new_slots <= d->slot_count)
        return true;
    Arena *a = d->arena;
    a->lock.lock();
    if (static_cast<size_t>(d->base_slot) + new_slots > a->slot_count)
    {
        a->lock.unlock();
        return false;
    }
    for (uint32_t j = d->slot_count; j < new_slots; ++j)
    {
        if (slot_used(a, d->base_slot + j))
        {
            a->lock.unlock();
            return false;
        }
    }
    for (uint32_t j = d->slot_count; j < new_slots; ++j)
    {
        slot_set(a, d->base_slot + j);
    }
    if (!commit_range_locked(a, d->base_slot + d->slot_count, new_slots - d->slot_count))
    {
        for (uint32_t j = d->slot_count; j < new_slots; ++j)
        {
            slot_clear(a, d->base_slot + j);
        }
        a->lock.unlock();
        return false;
    }
    const uintptr_t tagged = reinterpret_cast<uintptr_t>(d) | 1u;
    for (uint32_t j = d->slot_count; j < new_slots; ++j)
    {
        store_entry(&a->map[d->base_slot + j], tagged);
    }
    g_free_slots.fetch_sub(new_slots - d->slot_count, std::memory_order_relaxed);
    d->slot_count = new_slots;
    d->usable = static_cast<size_t>(new_slots) * kSlotSize;
    a->lock.unlock();
    return true;
}

void large_free(LargeDesc *d, void *p) noexcept
{
    Arena *a = d->arena;
    const uintptr_t expected = a->base + static_cast<size_t>(d->base_slot) * kSlotSize;
    if (reinterpret_cast<uintptr_t>(p) != expected)
        fatal_free(p);

    const size_t bytes = static_cast<size_t>(d->slot_count) * kSlotSize;
    LargeDesc *evicted = nullptr;

    a->lock.lock();
    // Keep slot_bits set until either the run is reused from the cache or the
    // purge below completes; this makes the range unallocatable in the window
    // where it has no pagemap entry.
    for (uint32_t j = 0; j < d->slot_count; ++j)
    {
        store_entry(&a->map[d->base_slot + j], 0);
    }

    if (bytes <= kLargeCacheBytes)
    {
        while (a->large_cache_bytes + bytes > kLargeCacheBytes && a->large_cache_tail)
        {
            LargeDesc *victim = a->large_cache_tail;
            large_cache_unlink_locked(a, victim);
            victim->next = evicted;
            evicted = victim;
        }
        large_cache_push_locked(a, d);
    }
    else
    {
        d->next = evicted;
        evicted = d;
    }
    a->lock.unlock();

    if (evicted)
        large_cache_purge_list(evicted, false);
}

} // namespace hpm
