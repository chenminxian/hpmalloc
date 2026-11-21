// Per-class central bins, TCache refill/flush slow paths, retained-span
// trimming, and invalid-pointer reporting.

#include "allocator_internal.hpp"

#include <cerrno>
#include <unistd.h>

namespace hpm
{

// ---------------------------------------------------------------------------
// Central bins
// ---------------------------------------------------------------------------
static inline size_t span_bytes_of(const Span *s) noexcept
{
    return static_cast<size_t>(s->granule_count) * kPageSize;
}

static void insert_span_locked(CentralBin &bin, Span *s) noexcept
{
    if (s->flags & kInBin)
        return;
    s->flags |= kInBin;
    s->prev = nullptr;
    s->next = bin.spans;
    if (bin.spans)
        bin.spans->prev = s;
    bin.spans = s;
}

static void clear_retained_locked(CentralBin &bin, Span *s) noexcept
{
    if (!(s->flags & kRetainedSpan))
        return;
    s->flags &= ~kRetainedSpan;
    const size_t bytes = span_bytes_of(s);
    if (bytes <= kEmptySpanMaxBytes)
    {
        if (bin.empty_spans)
            --bin.empty_spans;
        if (bin.empty_bytes >= bytes)
            bin.empty_bytes -= bytes;
        else
            bin.empty_bytes = 0;
    }
    else
    {
        if (bin.large_empty_spans)
            --bin.large_empty_spans;
        if (bin.large_empty_bytes >= bytes)
            bin.large_empty_bytes -= bytes;
        else
            bin.large_empty_bytes = 0;
    }
}

static void remove_span_locked(CentralBin &bin, Span *s) noexcept
{
    if (!(s->flags & kInBin))
        return;
    if (s->prev)
        s->prev->next = s->next;
    else
        bin.spans = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->prev = nullptr;
    s->next = nullptr;
    s->flags &= ~kInBin;
    clear_retained_locked(bin, s);
}

// Keep a limited number of completely free spans in the central bin.  Small
// spans and the 2 MiB spans used by 256 KiB/1 MiB classes have separate
// budgets so large-span retention does not lengthen the cross-small lists.
static bool retain_full_span_locked(CentralBin &bin, Span *s) noexcept
{
    if (s->flags & kRetainedSpan)
        return true;
    if (!(s->flags & kInBin))
        insert_span_locked(bin, s);
    const size_t bytes = span_bytes_of(s);
    if (bytes <= kEmptySpanMaxBytes)
    {
        if (bin.empty_spans < kEmptySpanPerShard && bin.empty_bytes + bytes <= kEmptySpanShardBytes)
        {
            s->flags |= kRetainedSpan;
            ++bin.empty_spans;
            bin.empty_bytes += bytes;
            return true;
        }
    }
    else if (bytes <= kLargeEmptySpanMaxBytes)
    {
        if (bin.large_empty_spans < kLargeEmptySpanPerShard &&
            bin.large_empty_bytes + bytes <= kLargeEmptySpanShardBytes)
        {
            s->flags |= kRetainedSpan;
            ++bin.large_empty_spans;
            bin.large_empty_bytes += bytes;
            return true;
        }
    }
    remove_span_locked(bin, s);
    return false;
}

static uint32_t take_batch_locked(CentralBin &bin, uint32_t want, void *&list_out) noexcept
{
    uint32_t got = 0;
    void *head = nullptr;
    void *tail = nullptr;

    while (got < want)
    {
        Span *s = bin.spans;
        if (!s)
            break;
        clear_retained_locked(bin, s);

        bool progressed = false;

        // Detach a whole chunk of an already-linked span free list instead of
        // relinking every object one at a time.  The chunk's internal links
        // are preserved; only the tail of the returned list is terminated
        // once, after all chunks (and any bump objects) have been appended.
        if (s->free_head && s->free_count > 0)
        {
            const uint32_t remaining = want - got;
            const uint32_t limit = (s->free_count < remaining) ? s->free_count : remaining;
            void *chunk = s->free_head;
            void *cur = chunk;
            void *last = nullptr;
            uint32_t n = 0;
            while (cur && n < limit)
            {
                last = cur;
                cur = load_next(cur);
                ++n;
            }
            if (n)
            {
                s->free_head = cur;
                s->free_count -= n;
                if (tail)
                    store_next(tail, chunk);
                else
                    head = chunk;
                tail = last;
                got += n;
                progressed = true;
            }
        }

        while (got < want && s->free_count > 0 && s->bump_off < s->bump_end)
        {
            void *obj = reinterpret_cast<void *>(span_start_addr(s) + s->bump_off);
            s->bump_off += s->object_size;
            --s->free_count;
            store_next(obj, nullptr);
            if (tail)
                store_next(tail, obj);
            else
                head = obj;
            tail = obj;
            ++got;
            progressed = true;
        }

        if (s->free_count == 0)
            remove_span_locked(bin, s);
        if (!progressed)
            break;
    }

    if (tail)
        store_next(tail, nullptr);
    list_out = head;
    return got;
}

void fatal_free(const void *p) noexcept
{
    (void)p;
    static const char msg[] = "hpmalloc: invalid pointer passed to free/realloc\n";
    (void)!write(2, msg, sizeof(msg) - 1);
    __builtin_abort();
}

void *refill(TCache *tc, uint32_t class_id) noexcept
{
    TCacheBin &tb = tc->bins[class_id];
    void *list = nullptr;
    uint32_t got = 0;
    const uint32_t pref = tc->shard & (kCentralShards - 1);

    // Prefer the owning thread's shard, but allow stealing from another shard
    // before allocating a new span.  The span stays in its original shard;
    // objects taken here are returned to that shard when the TCache flushes.
    for (uint32_t k = 0; k < kCentralShards && got == 0; ++k)
    {
        CentralBin &bin = g_bins[class_id][(pref + k) & (kCentralShards - 1)];
        bin.lock.lock();
        got = take_batch_locked(bin, tb.batch, list);
        bin.lock.unlock();
    }

    if (got == 0)
    {
        Span *s = page_alloc_span(class_id);
        if (!s)
        {
            errno = ENOMEM;
            return nullptr;
        }
        set_span_shard(s, pref);
        CentralBin &bin = g_bins[class_id][pref];
        bin.lock.lock();
        insert_span_locked(bin, s);
        got = take_batch_locked(bin, tb.batch, list);
        bin.lock.unlock();
        if (got == 0)
            return nullptr;
    }

    tb.head = list;
    tb.len = static_cast<uint16_t>(got);
    void *p = list;
    tb.head = load_next(p);
    --tb.len;
    return p;
}

void free_no_tcache(Span *s, void *obj, uint32_t class_id) noexcept
{
    if (!s)
        fatal_free(obj);
    CentralBin &bin = g_bins[class_id][span_shard(s)];
    Span *release = nullptr;
    bin.lock.lock();
    store_next(obj, s->free_head);
    s->free_head = obj;
    ++s->free_count;
    if (s->free_count >= s->capacity)
    {
        if (!retain_full_span_locked(bin, s))
            release = s;
    }
    else if (!(s->flags & kInBin))
    {
        insert_span_locked(bin, s);
    }
    bin.lock.unlock();
    if (release)
        page_release_span(release);
}

void flush_slow(TCache *tc, uint32_t class_id) noexcept
{
    TCacheBin &tb = tc->bins[class_id];
    void *list = tb.head;
    tb.head = nullptr;
    tb.len = 0;
    if (!list)
        return;

    constexpr uint32_t kMaxBatch = 512;
    Span *spans[kMaxBatch];
    void *objs[kMaxBatch];
    uint32_t m = 0;

    for (void *cur = list; cur && m < kMaxBatch; cur = load_next(cur))
    {
        Arena *a = find_arena(tc, cur);
        if (!a)
            fatal_free(cur);
        const uintptr_t off = reinterpret_cast<uintptr_t>(cur) - a->base;
        const uintptr_t e = load_entry(&a->map[off >> 21]);
        if (e == 0 || (e & 1))
            fatal_free(cur);
        Extent *ex = reinterpret_cast<Extent *>(e);
        const GranuleEntry ge = ex->entries[(off >> 12) & (kGranulesPerSlot - 1)];
        if (!ge.span || ge.class_id != class_id)
            fatal_free(cur);
        spans[m] = ge.span;
        objs[m] = cur;
        ++m;
    }

    struct FlushItem
    {
        Span *span;
        void *obj;
    };
    FlushItem items[kMaxBatch];
    uint32_t shard_count[kCentralShards]{};
    uint32_t shard_offset[kCentralShards]{};
    uint32_t shard_pos[kCentralShards]{};

    for (uint32_t i = 0; i < m; ++i)
    {
        ++shard_count[span_shard(spans[i])];
    }
    uint32_t offset = 0;
    for (uint32_t sh = 0; sh < kCentralShards; ++sh)
    {
        shard_offset[sh] = offset;
        offset += shard_count[sh];
    }
    for (uint32_t i = 0; i < m; ++i)
    {
        const uint32_t sh = span_shard(spans[i]);
        items[shard_offset[sh] + shard_pos[sh]++] = FlushItem{spans[i], objs[i]};
    }

    Span *releases[kMaxBatch];
    uint32_t release_count = 0;

    for (uint32_t sh = 0; sh < kCentralShards; ++sh)
    {
        const uint32_t count = shard_count[sh];
        if (count == 0)
            continue;
        CentralBin &bin = g_bins[class_id][sh];
        bin.lock.lock();
        const uint32_t begin = shard_offset[sh];
        const uint32_t end = begin + count;
        for (uint32_t i = begin; i < end; ++i)
        {
            Span *s = items[i].span;
            void *obj = items[i].obj;
            store_next(obj, s->free_head);
            s->free_head = obj;
            ++s->free_count;
            if (s->free_count >= s->capacity)
            {
                if (!retain_full_span_locked(bin, s))
                {
                    releases[release_count++] = s;
                }
            }
            else if (!(s->flags & kInBin))
            {
                insert_span_locked(bin, s);
            }
        }
        bin.lock.unlock();
    }

    for (uint32_t i = 0; i < release_count; ++i)
    {
        page_release_span(releases[i]);
    }
}

void flush_all(TCache *tc) noexcept
{
    for (uint32_t c = 1; c <= kTotalClasses; ++c)
    {
        if (tc->bins[c].head)
            flush_slow(tc, c);
    }
}

// Push one object whose class is already known.  Used by free() after the
// granule lookup and by realloc(), which still has the old Extent/class from
// its own lookup and therefore must not repeat the arena/pagemap walk.
void trim_retained_spans() noexcept
{
    for (uint32_t c = 1; c <= kTotalClasses; ++c)
    {
        for (uint32_t sh = 0; sh < kCentralShards; ++sh)
        {
            CentralBin &bin = g_bins[c][sh];
            Span *releases[kEmptySpanPerShard + kLargeEmptySpanPerShard];
            uint32_t n = 0;
            bin.lock.lock();
            Span *s = bin.spans;
            while (s && n < kEmptySpanPerShard + kLargeEmptySpanPerShard)
            {
                Span *next = s->next;
                if (s->flags & kRetainedSpan)
                {
                    remove_span_locked(bin, s);
                    releases[n++] = s;
                }
                s = next;
            }
            bin.lock.unlock();
            for (uint32_t i = 0; i < n; ++i)
                page_release_span(releases[i]);
        }
    }
}

} // namespace hpm
