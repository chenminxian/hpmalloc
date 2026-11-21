// TCache attachment, pthread key/fork hooks, environment options, and global initialization.

#include "allocator_internal.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>

namespace hpm
{

// The pthread key destructor and atfork callbacks are installed before their
// definitions appear below.
static void thread_dtor(void *) noexcept;
static void ensure_hooks() noexcept;
static void atfork_prepare() noexcept;
static void atfork_parent() noexcept;
static void atfork_child() noexcept;

TCache *attach_tcache() noexcept
{
    if (!ensure_init())
        return nullptr;
    void *mem = meta_alloc(kMetaBlock16K);
    if (!mem)
        return nullptr;
    TCache *tc = static_cast<TCache *>(mem);
    std::memset(tc, 0, sizeof(TCache));
    for (uint32_t c = 1; c <= kTotalClasses; ++c)
    {
        const uint32_t stride = kClasses[c].stride;
        uint32_t mx = static_cast<uint32_t>(kTcacheByteTarget / stride);
        if (mx < 1)
            mx = 1;
        if (mx > 256)
            mx = 256;
        TCacheBin &tb = tc->bins[c];
        tb.max = static_cast<uint16_t>(mx);
        uint32_t bt = mx < 64 ? mx : 64;
        if (bt == 0)
            bt = 1;
        tb.batch = static_cast<uint16_t>(bt);
    }
    tc->last_arena = nullptr;
    tc->shard = g_tcache_seq.fetch_add(1, std::memory_order_acq_rel) & (kCentralShards - 1);
    tls_cache = tc;
    ensure_hooks();
    if (g_key_created.load(std::memory_order_acquire))
    {
        (void)pthread_setspecific(g_thread_key, tc);
    }
    return tc;
}

// slow path used by malloc_impl
void *malloc_slow_attach_helper(size_t n) noexcept
{
    if (tls_in_fork)
        return bootstrap_alloc(n);
    if (tls_in_init)
        return bootstrap_alloc(n);
    if (!ensure_init())
    {
        errno = ENOMEM;
        return nullptr;
    }
    TCache *tc = attach_tcache();
    if (!tc)
    {
        errno = ENOMEM;
        return nullptr;
    }
    return malloc_with_cache(tc, n);
}

static void thread_dtor(void *value) noexcept
{
    TCache *tc = static_cast<TCache *>(value);
    if (!tc)
        return;
    if (tls_cache == tc)
        tls_cache = nullptr;
    flush_all(tc);
    if (g_key_created.load(std::memory_order_acquire))
    {
        (void)pthread_setspecific(g_thread_key, nullptr);
    }
    meta_free(tc, kMetaBlock16K);
}

static void ensure_hooks() noexcept
{
    int s = g_hooks_state.load(std::memory_order_acquire);
    if (s == 2)
        return;
    if (s == 1)
    {
        for (unsigned i = 0;; ++i)
        {
            s = g_hooks_state.load(std::memory_order_acquire);
            if (s != 1)
                return;
            if ((i & 1023u) == 0)
                sched_yield();
            else
                SpinLock::pause_cpu();
        }
    }
    int expected = 0;
    if (g_hooks_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        if (pthread_key_create(&g_thread_key, thread_dtor) == 0)
        {
            g_key_created.store(true, std::memory_order_release);
        }
        (void)pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
        g_hooks_state.store(2, std::memory_order_release);
    }
    else
    {
        for (unsigned i = 0;; ++i)
        {
            s = g_hooks_state.load(std::memory_order_acquire);
            if (s != 1)
                return;
            if ((i & 1023u) == 0)
                sched_yield();
            else
                SpinLock::pause_cpu();
        }
    }
}

static void atfork_prepare() noexcept
{
    if (!tls_cache)
        (void)attach_tcache();
    tls_in_fork = true;
    g_dir_lock.lock();
    const size_t n = g_arena_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i)
        g_arenas[i]->lock.lock();
    g_meta.lock.lock();
    for (uint32_t c = 1; c <= kTotalClasses; ++c)
    {
        for (uint32_t sh = 0; sh < kCentralShards; ++sh)
        {
            g_bins[c][sh].lock.lock();
        }
    }
    g_direct_lock.lock();
}

static void atfork_release() noexcept
{
    g_direct_lock.unlock();
    for (uint32_t c = kTotalClasses; c >= 1; --c)
    {
        for (uint32_t sh = kCentralShards; sh > 0; --sh)
        {
            g_bins[c][sh - 1].lock.unlock();
        }
    }
    g_meta.lock.unlock();
    const size_t n = g_arena_count.load(std::memory_order_acquire);
    for (size_t i = n; i > 0; --i)
        g_arenas[i - 1]->lock.unlock();
    g_dir_lock.unlock();
    tls_in_fork = false;
}

static void atfork_parent() noexcept
{
    atfork_release();
}
static void atfork_child() noexcept
{
    atfork_release();
}

// ---------------------------------------------------------------------------
// Global init and environment configuration
// ---------------------------------------------------------------------------
static bool key_equals(const char *key, size_t len, const char *lit) noexcept
{
    size_t i = 0;
    for (; i < len && lit[i]; ++i)
    {
        if (key[i] != lit[i])
            return false;
    }
    return i == len && lit[i] == '\0';
}

static bool parse_uint(const char *s, size_t len, uint64_t &out) noexcept
{
    if (len == 0)
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + static_cast<uint64_t>(s[i] - '0');
        if (v > (1ull << 40))
            return false;
    }
    out = v;
    return true;
}

int apply_option(const char *key, size_t klen, const char *value, size_t vlen) noexcept
{
    if (key_equals(key, klen, "thp"))
    {
        if (key_equals(value, vlen, "auto"))
        {
            g_thp_mode.store(kThpAuto, std::memory_order_release);
            return 0;
        }
        if (key_equals(value, vlen, "always"))
        {
            g_thp_mode.store(kThpAlways, std::memory_order_release);
            return 0;
        }
        if (key_equals(value, vlen, "never"))
        {
            g_thp_mode.store(kThpNever, std::memory_order_release);
            return 0;
        }
        if (key_equals(value, vlen, "collapse"))
        {
            g_thp_mode.store(kThpCollapse, std::memory_order_release);
            return 0;
        }
        return -1;
    }
    if (key_equals(key, klen, "purge"))
    {
        if (key_equals(value, vlen, "free"))
        {
            g_purge_mode.store(kPurgeFree, std::memory_order_release);
            return 0;
        }
        if (key_equals(value, vlen, "dontneed"))
        {
            g_purge_mode.store(kPurgeDontNeed, std::memory_order_release);
            return 0;
        }
        if (key_equals(value, vlen, "defer"))
        {
            g_purge_mode.store(kPurgeDefer, std::memory_order_release);
            return 0;
        }
        return -1;
    }
    if (key_equals(key, klen, "arena_mb"))
    {
        if (g_state.load(std::memory_order_acquire) == 2)
            return -1;
        uint64_t mb = 0;
        if (!parse_uint(value, vlen, mb) || mb < 4)
            return -1;
        g_default_arena_bytes = static_cast<size_t>(mb) * 1024ull * 1024ull;
        return 0;
    }
    if (key_equals(key, klen, "tcache_kb"))
    {
        // Accepted for compatibility, the static per-class policy is used.
        return 0;
    }
    if (key_equals(key, klen, "realloc_direct_kb"))
    {
        uint64_t kb = 0;
        if (!parse_uint(value, vlen, kb))
            return -1;
        if (kb == 0)
        {
            g_realloc_direct_threshold = static_cast<size_t>(-1);
        }
        else
        {
            if (kb > static_cast<uint64_t>(-1) / 1024ull)
                return -1;
            g_realloc_direct_threshold = static_cast<size_t>(kb) * 1024ull;
        }
        return 0;
    }
    if (key_equals(key, klen, "realloc_slack_pct"))
    {
        uint64_t pct = 0;
        if (!parse_uint(value, vlen, pct) || pct > 400)
            return -1;
        g_realloc_slack_percent = static_cast<uint32_t>(pct);
        return 0;
    }
    if (key_equals(key, klen, "class_map"))
    {
        if (key_equals(value, vlen, "0") || key_equals(value, vlen, "off"))
        {
            g_class_map_enabled = false;
            return 0;
        }
        if (key_equals(value, vlen, "1") || key_equals(value, vlen, "on"))
        {
            g_class_map_enabled = true;
            return 0;
        }
        return -1;
    }
    return -1;
}

static void parse_env_options() noexcept
{
    const char *s = getenv("HPMALLOC_OPTIONS");
    if (!s)
        return;
    const char *p = s;
    while (*p)
    {
        const char *key = p;
        while (*p && *p != '=' && *p != ',')
            ++p;
        const size_t klen = static_cast<size_t>(p - key);
        const char *value = "";
        size_t vlen = 0;
        if (*p == '=')
        {
            ++p;
            value = p;
            while (*p && *p != ',')
                ++p;
            vlen = static_cast<size_t>(p - value);
        }
        if (klen)
            (void)apply_option(key, klen, value, vlen);
        if (*p == ',')
            ++p;
    }
}

static bool global_init() noexcept
{
    parse_env_options();
    return grow_arena(0) != nullptr;
}

bool ensure_init() noexcept
{
    int s = g_state.load(std::memory_order_acquire);
    if (s == 2)
        return true;
    if (s == 3)
        return false;
    if (s == 1)
    {
        if (tls_in_init)
            return false;
        for (unsigned i = 0;; ++i)
        {
            s = g_state.load(std::memory_order_acquire);
            if (s != 1)
                return s == 2;
            if ((i & 1023u) == 0)
                sched_yield();
            else
                SpinLock::pause_cpu();
        }
    }
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        if (tls_in_init)
            return false;
        for (unsigned i = 0;; ++i)
        {
            s = g_state.load(std::memory_order_acquire);
            if (s != 1)
                return s == 2;
            if ((i & 1023u) == 0)
                sched_yield();
            else
                SpinLock::pause_cpu();
        }
    }
    tls_in_init = true;
    const bool ok = global_init();
    g_state.store(ok ? 2 : 3, std::memory_order_release);
    tls_in_init = false;
    return ok;
}

// ---------------------------------------------------------------------------

} // namespace hpm
