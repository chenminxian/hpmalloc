// This file exports hpm_* API, statistics/trim entry points, and glibc compatibility shims

#include "allocator_internal.hpp"
#include "hpmalloc/hpmalloc.h"

#include <atomic>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace hpm
{

// ---------------------------------------------------------------------------
// Stats / trim / options
// ---------------------------------------------------------------------------

extern "C" HPM_EXPORT int hpm_stats_get(hpm_stats_t *out)
{
    if (!out)
        return -1;
    std::memset(out, 0, sizeof(*out));
    out->reserved_bytes = static_cast<size_t>(g_reserved_bytes.load(std::memory_order_acquire));
    out->committed_bytes = static_cast<size_t>(g_committed_slots.load(std::memory_order_acquire) * kSlotSize);
    out->free_slot_bytes = static_cast<size_t>(g_free_slots.load(std::memory_order_acquire) * kSlotSize);
    out->span_count = static_cast<size_t>(g_span_count.load(std::memory_order_acquire));
    out->extent_count = static_cast<size_t>(g_extent_slots.load(std::memory_order_acquire));
    out->purge_count = g_purge_count.load(std::memory_order_acquire);
    TCache *tc = tls_cache;
    if (tc)
    {
        size_t bytes = 0;
        for (uint32_t c = 1; c <= kTotalClasses; ++c)
        {
            bytes += static_cast<size_t>(tc->bins[c].len) * kClasses[c].stride;
        }
        out->tcache_bytes = bytes;
    }
    return 0;
}

extern "C" HPM_EXPORT int hpm_trim(size_t pad)
{
    (void)pad;
    if (g_state.load(std::memory_order_acquire) != 2)
        return 0;
    TCache *tc = tls_cache;
    if (tc)
        flush_all(tc);

    bool released = false;
    trim_retained_spans();
    const size_t n = g_arena_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i)
    {
        Arena *a = g_arenas[i];
        a->lock.lock();
        for (uint32_t s = 0; s < a->slot_count; ++s)
        {
            if (slot_used(a, s) || !committed_get(a, s))
                continue;
            (void)madvise(reinterpret_cast<void *>(a->base + static_cast<size_t>(s) * kSlotSize), kSlotSize,
                          MADV_DONTNEED);
            g_purge_count.fetch_add(1, std::memory_order_relaxed);
            released = true;
        }
        a->lock.unlock();
        if (large_cache_flush(a, true))
            released = true;
    }
    return released ? 1 : 0;
}

extern "C" HPM_EXPORT int hpm_set_option(const char *name, const char *value)
{
    if (!name || !value)
        return -1;
    const size_t nlen = std::strlen(name);
    const size_t vlen = std::strlen(value);
    return apply_option(name, nlen, value, vlen);
}

extern "C" HPM_EXPORT const char *hpm_version(void)
{
    return HPMALLOC_VERSION_MAJOR "." HPMALLOC_VERSION_MINOR "." HPMALLOC_VERSION_PATCH;
}

// ---------------------------------------------------------------------------
// Explicit API
// ---------------------------------------------------------------------------

extern "C" HPM_EXPORT void *hpm_malloc(size_t size)
{
    return malloc_impl(size);
}

extern "C" HPM_EXPORT void hpm_free(void *ptr)
{
    free_impl(ptr);
}

extern "C" HPM_EXPORT void *hpm_calloc(size_t nmemb, size_t size)
{
    return calloc_impl(nmemb, size);
}

extern "C" HPM_EXPORT void *hpm_realloc(void *ptr, size_t size)
{
    return realloc_impl(ptr, size);
}

extern "C" HPM_EXPORT void *hpm_memalign(size_t alignment, size_t size)
{
    return memalign_impl(size, alignment);
}

extern "C" HPM_EXPORT size_t hpm_usable_size(const void *ptr)
{
    return usable_impl(ptr);
}

// ---------------------------------------------------------------------------
// glibc compatibility shims
// ---------------------------------------------------------------------------

extern "C" HPM_EXPORT int malloc_trim(size_t pad)
{
    return hpm_trim(pad);
}

struct mallinfo
{
    int arena;
    int ordblks;
    int smblks;
    int hblks;
    int hblkhd;
    int usmblks;
    int fsmblks;
    int uordblks;
    int fordblks;
    int keepcost;
};

struct mallinfo2
{
    size_t arena;
    size_t ordblks;
    size_t smblks;
    size_t hblks;
    size_t hblkhd;
    size_t usmblks;
    size_t fsmblks;
    size_t uordblks;
    size_t fordblks;
    size_t keepcost;
};

extern "C" HPM_EXPORT int mallopt(int, int)
{
    // not implemented yet
    // glibc compat
    // 0: fail    non-0: success
    return 0;
}

extern "C" HPM_EXPORT struct mallinfo mallinfo(void)
{
    struct mallinfo mi{};
    hpm_stats_t st{};
    if (hpm_stats_get(&st) == 0)
    {
        mi.arena = static_cast<int>(st.committed_bytes);
        mi.fordblks = static_cast<int>(st.free_slot_bytes);
    }
    return mi;
}

extern "C" HPM_EXPORT struct mallinfo2 mallinfo2(void)
{
    struct mallinfo2 mi{};
    hpm_stats_t st{};
    if (hpm_stats_get(&st) == 0)
    {
        mi.arena = st.committed_bytes;
        mi.fordblks = st.free_slot_bytes;
    }
    return mi;
}

extern "C" HPM_EXPORT void malloc_stats(void)
{
    hpm_stats_t st{};
    (void)hpm_stats_get(&st);
    char buf[256];
    const int n = snprintf(buf, sizeof(buf),
                           "hpmalloc: reserved=%zu committed=%zu free_slots=%zu spans=%zu extents=%zu purges=%llu\n",
                           st.reserved_bytes, st.committed_bytes, st.free_slot_bytes, st.span_count, st.extent_count,
                           static_cast<unsigned long long>(st.purge_count));
    if (n > 0)
        (void)!write(2, buf, static_cast<size_t>(n));
}

struct _IO_FILE;
extern "C" HPM_EXPORT int malloc_info(int, struct _IO_FILE *)
{
    return -1;
}

} // namespace hpm
