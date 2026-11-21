#include "hpmalloc/hpmalloc.h"
#include "size_class.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <print>
#include <thread>
#include <vector>

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::println(stderr, "CHECK `" #cond "` failed: at " __FILE__ ":{}", __LINE__);                            \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{

void test_size_class_table()
{
    using namespace hpm;
    auto round4k = [](uint64_t v) { return (v + kMinSpan - 1) / kMinSpan * kMinSpan; };

    CHECK(normal_class(0) == 1);
    for (uint32_t id = 1; id <= kTotalClasses; ++id)
    {
        const ClassInfo &c = kClasses[id];
        CHECK(c.size > 0);
        CHECK(c.stride >= c.size);
        CHECK(c.span_bytes >= kMinSpan && c.span_bytes <= kMaxSpan);
        CHECK(c.span_bytes % kPageSize == 0);
        CHECK(c.capacity == c.span_bytes / c.stride);
        CHECK(c.capacity >= 1 && c.capacity <= 65535);
        CHECK(c.granules == c.span_bytes / kPageSize);

        const uint64_t max_n = kMaxSpan / c.stride;
        const uint64_t n = std::min<uint64_t>(kSpanTargetObjects, max_n);
        uint64_t expected = std::max<uint64_t>(kMinSpan, round4k(n * c.stride));
        if (expected > kMaxSpan)
            expected = kMaxSpan;
        CHECK(c.span_bytes == expected);
    }

    for (size_t n = 1; n <= kMaxClassSize; ++n)
    {
        const uint32_t c = normal_class(n);
        CHECK(c >= 1 && c <= kNormalClasses);
        CHECK(kClasses[c].size >= n);
        if (c > 1)
            CHECK(kClasses[c - 1].size < n);
    }

    const size_t samples[] = {
        1, 15, 16, 17, 100, 256, 257, 320, 384, 1000, 1024, 4096, 16384, 65536, 131072, 1048576, 2097152,
    };
    for (uint32_t lg = kFirstAlignLog2; lg < kFirstAlignLog2 + kAlignGroups; ++lg)
    {
        const size_t align = size_t{1} << lg;
        for (size_t s : samples)
        {
            const uint32_t id = aligned_class_id(normal_class(s), align);
            CHECK(id >= 1 && id <= kTotalClasses);
            CHECK(kClasses[id].stride % align == 0);
            CHECK(kClasses[id].stride >= s);
            CHECK(kClasses[id].size >= s);
        }
    }
}

void fill(void *p, size_t n, unsigned char v)
{
    std::memset(p, v, n);
}

bool all_bytes(const void *p, size_t n, unsigned char v)
{
    const unsigned char *b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; ++i)
    {
        if (b[i] != v)
            return false;
    }
    return true;
}

void test_sizes()
{
    const size_t sizes[] = {
        0,      1,      8,       15,      16,      17,      31,
        32,     63,     64,      65,      100,     255,     256,
        257,    320,    384,     448,     511,     512,     640,
        1024,   1025,   4095,    4096,    4097,    16384,   65536,
        131072, 262144, 1048576, 1572864, 2097152, 2097153, 3 * 1024 * 1024 + 123,
    };
    for (size_t n : sizes)
    {
        void *p = hpm_malloc(n);
        CHECK(p != nullptr);
        CHECK(reinterpret_cast<uintptr_t>(p) % 16 == 0);
        CHECK(hpm_usable_size(p) >= n);
        const size_t touch = n ? n : 1;
        fill(p, touch, 0xA5);
        CHECK(all_bytes(p, touch, 0xA5));
        hpm_free(p);
    }
}

void test_alignment()
{
    const size_t aligns[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 65536, 2 * 1024 * 1024};
    const size_t sizes[] = {1, 16, 100, 1000, 4096, 65536, 200000, 3000000};
    for (size_t a : aligns)
    {
        for (size_t n : sizes)
        {
            if (a == 8)
            {
                void *p = nullptr;
                CHECK(posix_memalign(&p, a, n) == 0);
                CHECK(p != nullptr);
                CHECK(reinterpret_cast<uintptr_t>(p) % a == 0);
                hpm_free(p);
                continue;
            }
            void *p = hpm_memalign(a, n);
            CHECK(p != nullptr);
            CHECK(reinterpret_cast<uintptr_t>(p) % a == 0);
            const size_t touch = n ? n : 1;
            fill(p, touch, 0x5A);
            CHECK(all_bytes(p, touch, 0x5A));
            hpm_free(p);
        }
    }
    void *bad = reinterpret_cast<void *>(0x1);
    CHECK(posix_memalign(&bad, 24, 100) == EINVAL);
}

void test_calloc()
{
    for (size_t n : {1, 16, 100, 4096, 100000, 3000000})
    {
        unsigned char *p = static_cast<unsigned char *>(hpm_calloc(1, n));
        CHECK(p != nullptr);
        CHECK(all_bytes(p, n, 0));
        fill(p, n, 0xFF);
        hpm_free(p);
    }
    CHECK(hpm_calloc(SIZE_MAX / 2, 4) == nullptr);
}

void test_large_cache_and_trim()
{
    constexpr size_t kBytes = 4u * 1024u * 1024u;
    unsigned char *p = static_cast<unsigned char *>(hpm_malloc(kBytes));
    CHECK(p != nullptr);
    fill(p, kBytes, 0xA7);
    hpm_free(p);

    // This should hit the large-run cache and still return zeroed memory.
    unsigned char *q = static_cast<unsigned char *>(hpm_calloc(1, kBytes));
    CHECK(q != nullptr);
    CHECK(all_bytes(q, kBytes, 0));
    fill(q, kBytes, 0x3C);
    hpm_free(q);
    CHECK(hpm_trim(0) >= 0);
}

void test_realloc()
{
    for (size_t n : {1, 16, 100, 1000, 8192, 100000, 3000000})
    {
        unsigned char *p = static_cast<unsigned char *>(hpm_malloc(n));
        CHECK(p != nullptr);
        fill(p, n, 0x33);
        const size_t bigger = n * 2 + 37;
        p = static_cast<unsigned char *>(hpm_realloc(p, bigger));
        CHECK(p != nullptr);
        CHECK(all_bytes(p, n, 0x33));
        p = static_cast<unsigned char *>(hpm_realloc(p, n / 2 + 1));
        CHECK(p != nullptr);
        CHECK(all_bytes(p, n / 2 + 1, 0x33));
        hpm_free(p);
    }
    void *p = hpm_realloc(nullptr, 64);
    CHECK(p != nullptr);
    p = hpm_realloc(p, 0);
    CHECK(p == nullptr);
}

void test_realloc_growth()
{
    // Exercise the geometric growth path repeatedly, including transitions
    // across the span/direct-size boundary and the large-run path.
    unsigned char *p = static_cast<unsigned char *>(hpm_malloc(16));
    CHECK(p != nullptr);
    std::memset(p, 0x6d, 16);
    size_t old = 16;
    const size_t targets[] = {
        64, 1024, 8192, 65536, 131072, 262144, 524288, 1048576, 1572864, 2097152, 3145728,
    };
    for (size_t target : targets)
    {
        p = static_cast<unsigned char *>(hpm_realloc(p, target));
        CHECK(p != nullptr);
        CHECK(hpm_usable_size(p) >= target);
        CHECK(all_bytes(p, old, 0x6d));
        std::memset(p, 0x6d, target);
        old = target;
    }
    hpm_free(p);
}

void test_cpp_new()
{
    int *a = new int[100];
    CHECK(a != nullptr);
    for (int i = 0; i < 100; ++i)
        a[i] = i;
    delete[] a;

    struct alignas(64) Aligned
    {
        unsigned char data[128];
    };
    Aligned *al = new Aligned;
    CHECK(reinterpret_cast<uintptr_t>(al) % 64 == 0);
    delete al;

    void *nf = operator new(100, std::nothrow);
    CHECK(nf != nullptr);
    operator delete(nf, std::nothrow);
}

void test_threads()
{
    constexpr int kThreads = 8;
    constexpr int kOps = 20000;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [t, &failures]
            {
                uint64_t state = 0x9e3779b97f4a7c15ull ^ static_cast<uint64_t>(t);
                void *slots[64]{};
                for (int i = 0; i < kOps; ++i)
                {
                    state = state * 6364136223846793005ull + 1;
                    unsigned idx = static_cast<unsigned>((state >> 33) & 63);
                    if (slots[idx])
                    {
                        hpm_free(slots[idx]);
                        slots[idx] = nullptr;
                    }
                    else
                    {
                        size_t n = static_cast<size_t>((state >> 17) % 8192) + 1;
                        void *p = hpm_malloc(n);
                        if (!p)
                        {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        std::memset(p, static_cast<int>(state & 0xFF), n);
                        slots[idx] = p;
                    }
                    if ((i & 1023) == 0)
                        std::this_thread::yield();
                }
                for (void *&p : slots)
                {
                    if (p)
                    {
                        hpm_free(p);
                        p = nullptr;
                    }
                }
            });
    }
    for (auto &th : threads)
    {
        if (th.joinable())
            th.join();
    }
    CHECK(failures.load() == 0);
}

void test_cross_thread()
{
    constexpr int kCount = 2000;
    std::vector<void *> ptrs;
    ptrs.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        size_t n = static_cast<size_t>(i % 300) + 1;
        void *p = hpm_malloc(n);
        CHECK(p != nullptr);
        ptrs.push_back(p);
    }
    std::thread worker(
        [&]
        {
            for (void *p : ptrs)
                hpm_free(p);
        });
    worker.join();
}

} // anonymous namespace

int main()
{
    test_size_class_table();
    test_sizes();
    test_alignment();
    test_calloc();
    test_large_cache_and_trim();
    test_realloc();
    test_realloc_growth();
    test_cpp_new();
    test_threads();
    test_cross_thread();

    hpm_stats_t st{};
    CHECK(hpm_stats_get(&st) == 0);
    CHECK(st.reserved_bytes >= 64ull * 1024 * 1024);
    CHECK(st.committed_bytes > 0);
    CHECK(st.span_count > 0);

    CHECK(hpm_trim(0) >= 0);
    CHECK(std::strcmp(hpm_version(), HPMALLOC_VERSION_MAJOR "." HPMALLOC_VERSION_MINOR "." HPMALLOC_VERSION_PATCH) ==
          0);

    std::println("hpmalloc basic test: PASS (reserved={} committed={} spans={})", st.reserved_bytes, st.committed_bytes,
                 st.span_count);
    return 0;
}
