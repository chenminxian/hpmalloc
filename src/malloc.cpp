// Hot malloc/free/realloc/calloc/aligned paths and the exported malloc-family entry points.
// Keeping these together preserves inlining in the fast path.

#include "allocator_internal.hpp"

#include <cerrno>
#include <sys/mman.h>

namespace hpm
{

static inline void *tcache_alloc_small(TCache *tc, uint32_t class_id) noexcept
{
    TCacheBin &tb = tc->bins[class_id];
    void *p = tb.head;
    if (HPM_LIKELY(p != nullptr))
    {
        tb.head = load_next(p);
        --tb.len;
    }
    else
    {
        p = refill(tc, class_id);
    }
    return p;
}

void *class_alloc(TCache *tc, uint32_t class_id) noexcept
{
    return tcache_alloc_small(tc, class_id);
}

void *malloc_with_cache(TCache *tc, size_t n) noexcept
{
    if (HPM_UNLIKELY(n > kMaxClassSize))
        return large_alloc(n, 16);
    const uint32_t c = normal_class(n);
    return class_alloc(tc, c);
}

void *malloc_impl(size_t n) noexcept
{
    if (HPM_UNLIKELY(tls_in_fork))
        return bootstrap_alloc(n);
    TCache *tc = tls_cache;
    if (HPM_UNLIKELY(!tc))
        return malloc_slow_attach_helper(n);
    return malloc_with_cache(tc, n);
}

static inline void tcache_push_small(TCache *tc, uint32_t class_id, void *p) noexcept
{
    TCacheBin &tb = tc->bins[class_id];
    store_next(p, tb.head);
    tb.head = p;
    if (HPM_UNLIKELY(tls_in_fork))
    {
        ++tb.len;
        return;
    }
    if (HPM_UNLIKELY(++tb.len > tb.max))
        flush_slow(tc, class_id);
}

void *memalign_impl(size_t size, size_t align) noexcept
{
    if (align == 0 || (align & (align - 1)) != 0)
    {
        errno = EINVAL;
        return nullptr;
    }
    if (size == 0)
        size = 1;
    if (HPM_UNLIKELY(tls_in_fork))
    {
        if (align <= 16)
            return bootstrap_alloc(size);
        return nullptr;
    }
    if (align <= 16)
    {
        return malloc_impl(size);
    }
    if (size > kMaxClassSize || align > 4096)
    {
        return large_alloc(size, align);
    }
    TCache *tc = tls_cache;
    if (HPM_UNLIKELY(!tc))
    {
        tc = attach_tcache();
        if (!tc)
        {
            errno = ENOMEM;
            return nullptr;
        }
    }
    const uint32_t normal = normal_class(size);
    const uint32_t c = aligned_class_id(normal, align);
    return class_alloc(tc, c);
}

void *calloc_impl(size_t nmemb, size_t size) noexcept
{
    size_t total = 0;
    if (__builtin_mul_overflow(nmemb, size, &total))
    {
        errno = ENOMEM;
        return nullptr;
    }
    if (total > kMaxClassSize && !tls_in_fork && !tls_in_init && ensure_init())
    {
        void *p = large_alloc(total, 16);
        if (!p)
            return nullptr;
        if (total)
        {
            const size_t len = (total + kPageSize - 1) / kPageSize * kPageSize;
            // Anonymous MADV_DONTNEED pages read back as zero.  This gives
            // calloc the same lazy-zero behavior as the other large allocators
            // without ever returning stale data.
            if (madvise(p, len, MADV_DONTNEED) != 0)
            {
                std::memset(p, 0, total);
            }
        }
        return p;
    }
    void *p = malloc_impl(total);
    if (!p)
        return nullptr;
    if (total)
        std::memset(p, 0, total);
    return p;
}

void *realloc_impl(void *p, size_t n) noexcept
{
    if (!p)
        return malloc_impl(n);
    if (n == 0)
    {
        free_impl(p);
        return nullptr;
    }
    if (is_bootstrap(p))
    {
        void *np = malloc_impl(n);
        return np; // bootstrap allocations are internal and tiny
    }

    TCache *tc = tls_cache;
    if (!tc)
    {
        tc = attach_tcache();
        if (!tc)
        {
            errno = ENOMEM;
            return nullptr;
        }
    }
    Arena *a = find_arena(tc, p);
    if (!a)
        return direct_realloc(p, n);

    const uintptr_t off = reinterpret_cast<uintptr_t>(p) - a->base;
    if (HPM_LIKELY(a->class_map != nullptr))
    {
        const uint32_t fc = a->class_map[off >> 12];
        if (HPM_LIKELY(fc != 0))
        {
            const size_t usable = kClasses[fc].stride;
            if (n <= usable)
                return p;

            void *np = nullptr;
            if (n >= g_realloc_direct_threshold && n <= kMaxClassSize && HPM_LIKELY(!tls_in_fork))
            {
                np = direct_alloc(n, 16);
            }
            if (!np)
            {
                size_t alloc_n = n;
                if (HPM_UNLIKELY(tls_in_fork))
                {
                    np = malloc_impl(n);
                }
                else
                {
                    const uint32_t slack = g_realloc_slack_percent;
                    if (slack != 0 && n < g_realloc_direct_threshold && n <= kMaxClassSize)
                    {
                        const size_t extra = n * slack / 100;
                        if (extra <= kMaxClassSize - n)
                            alloc_n = n + extra;
                        else
                            alloc_n = kMaxClassSize;
                    }
                    np = malloc_with_cache(tc, alloc_n);
                }
            }
            if (!np)
                return nullptr;
            std::memcpy(np, p, usable < n ? usable : n);
            tcache_push_small(tc, fc, p);
            return np;
        }
    }

    const uintptr_t e = load_entry(&a->map[off >> 21]);
    if (e == 0)
        fatal_free(p);

    if (e & 1)
    {
        LargeDesc *d = reinterpret_cast<LargeDesc *>(e & ~uintptr_t{1});
        if (n <= d->usable)
        {
            d->req = n;
            return p;
        }
        if (large_grow_in_place(d, n))
        {
            d->req = n;
            return p;
        }
        void *np = large_alloc(n, d->align);
        if (!np)
            return nullptr;
        std::memcpy(np, p, d->req);
        large_free(d, p);
        return np;
    }

    Extent *ex = reinterpret_cast<Extent *>(e);
    const uint32_t gidx = static_cast<uint32_t>((off >> 12) & (kGranulesPerSlot - 1));
    const uint32_t c = ex->class_ids[gidx];
    if (!c)
        fatal_free(p);
    const size_t usable = kClasses[c].stride;
    if (n <= usable)
        return p;

    void *np = HPM_UNLIKELY(tls_in_fork) ? malloc_impl(n) : malloc_with_cache(tc, n);
    if (!np)
        return nullptr;
    std::memcpy(np, p, usable < n ? usable : n);
    tcache_push_small(tc, c, p);
    return np;
}

size_t usable_impl(const void *p) noexcept
{
    if (!p || is_bootstrap(p))
        return 0;
    TCache *tc = tls_cache;
    Arena *a = find_arena(tc, p);
    if (!a)
    {
        size_t len = 0;
        return direct_usable(p, &len) ? len : 0;
    }
    const uintptr_t off = reinterpret_cast<uintptr_t>(p) - a->base;
    if (HPM_LIKELY(a->class_map != nullptr))
    {
        const uint32_t fc = a->class_map[off >> 12];
        if (HPM_LIKELY(fc != 0))
            return kClasses[fc].stride;
    }
    const uintptr_t e = load_entry(&a->map[off >> 21]);
    if (e == 0)
        return 0;
    if (e & 1)
    {
        LargeDesc *d = reinterpret_cast<LargeDesc *>(e & ~uintptr_t{1});
        return d->usable;
    }
    Extent *ex = reinterpret_cast<Extent *>(e);
    const uint32_t c = ex->class_ids[(off >> 12) & (kGranulesPerSlot - 1)];
    return c ? kClasses[c].stride : 0;
}

void free_impl(void *p) noexcept
{
    if (HPM_UNLIKELY(p == nullptr))
        return;
    if (HPM_UNLIKELY(is_bootstrap(p)))
        return;
    TCache *tc = tls_cache;
    if (HPM_UNLIKELY(!tc))
    {
        if (tls_in_init)
            return;
        if (!tls_in_fork)
            tc = attach_tcache();
    }
    Arena *a = find_arena(tc, p);
    if (HPM_UNLIKELY(!a))
    {
        if (tls_in_fork)
            return;
        direct_free(p);
        return;
    }
    const uintptr_t off = reinterpret_cast<uintptr_t>(p) - a->base;
    if (HPM_LIKELY(a->class_map != nullptr))
    {
        const uint32_t fc = a->class_map[off >> 12];
        if (HPM_LIKELY(fc != 0))
        {
            if (HPM_LIKELY(tc != nullptr))
            {
                if (HPM_UNLIKELY(tls_in_fork))
                {
                    TCacheBin &tb = tc->bins[fc];
                    store_next(p, tb.head);
                    tb.head = p;
                    ++tb.len;
                }
                else
                {
                    tcache_push_small(tc, fc, p);
                }
                return;
            }
        }
    }
    const uintptr_t e = load_entry(&a->map[off >> 21]);
    if (HPM_UNLIKELY(e == 0))
        fatal_free(p);
    if (HPM_UNLIKELY(e & 1))
    {
        if (tls_in_fork)
            return;
        large_free(reinterpret_cast<LargeDesc *>(e & ~uintptr_t{1}), p);
        return;
    }
    Extent *ex = reinterpret_cast<Extent *>(e);
    const uint32_t gidx = static_cast<uint32_t>((off >> 12) & (kGranulesPerSlot - 1));
    const uint32_t c = ex->class_ids[gidx];
    if (HPM_UNLIKELY(!c))
        fatal_free(p);
    if (HPM_UNLIKELY(tls_in_fork))
    {
        if (tc)
        {
            TCacheBin &tb = tc->bins[c];
            store_next(p, tb.head);
            tb.head = p;
            ++tb.len;
        }
        return;
    }
    if (HPM_UNLIKELY(!tc))
    {
        free_no_tcache(ex->entries[gidx].span, p, c);
        return;
    }
    tcache_push_small(tc, c, p);
}

extern "C" HPM_EXPORT void *malloc(size_t n)
{
    TCache *tc = tls_cache;
    if (HPM_UNLIKELY(!tc))
        return malloc_slow_attach_helper(n);
    if (HPM_UNLIKELY(tls_in_fork))
        return bootstrap_alloc(n);
    if (HPM_UNLIKELY(n > kMaxClassSize))
        return large_alloc(n, 16);
    const uint32_t c = normal_class(n);
    return tcache_alloc_small(tc, c);
}

extern "C" HPM_EXPORT void free(void *p)
{
    free_impl(p);
}

extern "C" HPM_EXPORT void *calloc(size_t nmemb, size_t size)
{
    return calloc_impl(nmemb, size);
}

extern "C" HPM_EXPORT void *realloc(void *p, size_t n)
{
    return realloc_impl(p, n);
}

extern "C" HPM_EXPORT void *reallocarray(void *p, size_t nmemb, size_t size)
{
    size_t total = 0;
    if (__builtin_mul_overflow(nmemb, size, &total))
    {
        errno = ENOMEM;
        return nullptr;
    }
    return realloc_impl(p, total);
}

extern "C" HPM_EXPORT void *memalign(size_t alignment, size_t size)
{
    return memalign_impl(size, alignment);
}

extern "C" HPM_EXPORT void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
    {
        errno = EINVAL;
        return nullptr;
    }
    return memalign_impl(size, alignment);
}

extern "C" HPM_EXPORT int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (!memptr)
        return EINVAL;
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;
    void *p = memalign_impl(size, alignment);
    if (!p)
        return ENOMEM;
    *memptr = p;
    return 0;
}

extern "C" HPM_EXPORT void *valloc(size_t size)
{
    return memalign_impl(size, kPageSize);
}

extern "C" HPM_EXPORT void *pvalloc(size_t size)
{
    if (size == 0)
        size = kPageSize;
    if (size > static_cast<size_t>(-1) - kPageSize)
    {
        errno = ENOMEM;
        return nullptr;
    }
    size = (size + kPageSize - 1) / kPageSize * kPageSize;
    return memalign_impl(size, kPageSize);
}

extern "C" HPM_EXPORT void cfree(void *p)
{
    free_impl(p);
}

extern "C" HPM_EXPORT size_t malloc_usable_size(const void *p)
{
    return usable_impl(p);
}

} // namespace hpm
