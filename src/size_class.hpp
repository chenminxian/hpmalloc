// This header manages size relevant strategy
// mainly including compile-time constants, calculation, look-up tables

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace hpm
{

// 4kiB page
inline constexpr uint32_t kPageSize = 4096;
// 2MiB transparent huge page
inline constexpr uint32_t kSlotSize = 2u * 1024u * 1024u;
inline constexpr uint32_t kGranulesPerSlot = kSlotSize / kPageSize;
// <1> 16, 32, 64, ..., 240 (total 15)
// <2> for k in 8..20: base=2^k q=base/4
//     base(k), base(k)+q, base(k)+2*q, base(k)+3*q (total 13*4)
// <3> 2MiB
// 15 + 13*4 + 1 = 68
inline constexpr uint32_t kNormalClasses = 68;
// minimum alignment of normal class is 16(2^4)
// so the first alignment of aligned class is 32(2^5)
inline constexpr uint32_t kFirstAlignLog2 = 5;
// aligned as 32, 64, ..., 4096 (2^5B, 2^6B, ..., 4kiB) (total 8)
inline constexpr uint32_t kAlignGroups = 8;
inline constexpr uint32_t kTotalClasses = kNormalClasses * (1 + kAlignGroups);
inline constexpr uint32_t kMaxClassSize = kSlotSize;
inline constexpr uint32_t kSpanTargetObjects = 32;
inline constexpr uint32_t kMinSpan = kPageSize;
inline constexpr uint32_t kMaxSpan = kSlotSize;

struct ClassInfo
{
    uint32_t size;       // largest request served by this class
    uint32_t stride;     // object spacing inside a span
    uint32_t span_bytes; // span size, multiple of kPageSize
    uint32_t capacity;   // objects per span
    uint16_t granules;   // span_bytes / kPageSize
    uint8_t align_log2;  // 4 for normal classes, 5..12 for aligned classes
    uint8_t pad;
};

consteval uint32_t round_up_const(uint64_t value, uint32_t align)
{
    return static_cast<uint32_t>((value + align - 1) / align * align);
}

consteval std::array<uint32_t, kNormalClasses + 1> make_normal_sizes()
{
    std::array<uint32_t, kNormalClasses + 1> a{};
    uint32_t idx = 1;
    for (uint32_t i = 1; i <= 16; ++i)
    {
        a[idx++] = 16u * i;
    }
    for (uint32_t k = 8; k <= 20; ++k)
    {
        const uint32_t base = 1u << k;
        const uint32_t q = base >> 2;
        if (k == 8)
        {
            a[idx++] = base + q;
            a[idx++] = base + 2 * q;
            a[idx++] = base + 3 * q;
        }
        else
        {
            a[idx++] = base;
            a[idx++] = base + q;
            a[idx++] = base + 2 * q;
            a[idx++] = base + 3 * q;
        }
    }
    a[idx++] = kSlotSize;
    return a;
}

consteval bool validate_normal_sizes()
{
    constexpr auto s = make_normal_sizes();
    for (uint32_t i = 2; i <= kNormalClasses; ++i)
    {
        if (s[i] <= s[i - 1])
            return false;
    }
    return s[1] == 16 && s[16] == 256 && s[kNormalClasses] == kSlotSize;
}

inline constexpr auto kNormalSizes = make_normal_sizes();
static_assert(validate_normal_sizes());

consteval std::array<uint16_t, 22> make_octave_base()
{
    constexpr auto s = make_normal_sizes();
    std::array<uint16_t, 22> b{};
    for (uint32_t k = 8; k <= 21; ++k)
    {
        const uint32_t value = 1u << k;
        for (uint32_t i = 1; i <= kNormalClasses; ++i)
        {
            if (s[i] == value)
                b[k] = static_cast<uint16_t>(i);
        }
    }
    return b;
}

// kNormalSizes[kOctaveBase[i]] == 2^i (i=8..21)
inline constexpr auto kOctaveBase = make_octave_base();
static_assert(kOctaveBase[8] == 16);
static_assert(kOctaveBase[9] == 20);
static_assert(kOctaveBase[21] == 68);

consteval ClassInfo plan_class(uint32_t size, uint32_t stride, uint8_t align_log2)
{
    const uint32_t max_n = (stride == 0) ? 1 : (kMaxSpan / stride);
    uint32_t n = (max_n < kSpanTargetObjects) ? max_n : kSpanTargetObjects;
    if (n == 0)
        n = 1;

    uint64_t span64 = static_cast<uint64_t>(n) * stride;
    span64 = (span64 + kMinSpan - 1) / kMinSpan * kMinSpan;
    if (span64 < kMinSpan)
        span64 = kMinSpan;
    if (span64 > kMaxSpan)
        span64 = kMaxSpan;

    uint32_t span = static_cast<uint32_t>(span64);
    uint32_t capacity = span / stride;
    if (capacity == 0)
    {
        // Defensive: stride should always fit in one 2 MiB span.
        span = round_up_const(stride, kMinSpan);
        capacity = 1;
    }
    return ClassInfo{size, stride, span, capacity, static_cast<uint16_t>(span / kPageSize), align_log2, 0};
}

consteval std::array<ClassInfo, kTotalClasses + 1> make_classes()
{
    constexpr auto sizes = make_normal_sizes();
    std::array<ClassInfo, kTotalClasses + 1> out{};
    for (uint32_t i = 1; i <= kNormalClasses; ++i)
    {
        out[i] = plan_class(sizes[i], sizes[i], 4);
    }
    for (uint32_t g = 0; g < kAlignGroups; ++g)
    {
        const uint32_t lg = kFirstAlignLog2 + g;
        const uint32_t align = 1u << lg;
        for (uint32_t i = 1; i <= kNormalClasses; ++i)
        {
            const uint32_t stride = (sizes[i] + align - 1) / align * align;
            out[kNormalClasses + g * kNormalClasses + i] = plan_class(sizes[i], stride, static_cast<uint8_t>(lg));
        }
    }
    return out;
}

// kClasses[i][j] with size=kNormalSizes[j] alignment=2^(4+i)
inline constexpr auto kClasses = make_classes();

consteval bool validate_classes()
{
    for (uint32_t id = 1; id <= kTotalClasses; ++id)
    {
        const ClassInfo &c = kClasses[id];
        if (c.size == 0 || c.stride < c.size)
            return false;
        if (c.span_bytes < kMinSpan || c.span_bytes > kMaxSpan)
            return false;
        if ((c.span_bytes % kPageSize) != 0)
            return false;
        if (c.capacity == 0 || c.capacity > 65535)
            return false;
        if (c.capacity != c.span_bytes / c.stride)
            return false;
        if (c.granules == 0 || c.granules > kGranulesPerSlot)
            return false;
        if (c.granules != c.span_bytes / kPageSize)
            return false;
    }
    return true;
}
static_assert(validate_classes());
static_assert(kClasses[1].span_bytes == 4096 && kClasses[1].capacity == 256);
static_assert(kClasses[3].span_bytes == 4096 && kClasses[3].capacity == 85);      // 48 B
static_assert(kClasses[6].span_bytes == 4096 && kClasses[6].capacity == 42);      // 96 B
static_assert(kClasses[49].span_bytes == 2048000 && kClasses[49].capacity == 25); // 80 KiB
static_assert(kClasses[65].span_bytes == 1310720 && kClasses[65].capacity == 1);  // 1.25 MiB
static_assert(kClasses[kNormalClasses].span_bytes == kSlotSize);
static_assert(kClasses[kNormalClasses].capacity == 1);

consteval uint32_t normal_class_slow(uint32_t n)
{
    if (n == 0)
        return 1;
    if (n <= 256)
        return (n + 15) >> 4;
    const unsigned k = 31u - static_cast<unsigned>(std::countl_zero(n)); // floor(log2(n))
    const uint32_t base = uint32_t{1} << k;
    const uint32_t q = base >> 2;
    return static_cast<uint32_t>(kOctaveBase[k] + ((n - base + q - 1) >> (k - 2)));
}

consteval std::array<uint8_t, 257> make_small_class_table()
{
    std::array<uint8_t, 257> t{};
    for (uint32_t i = 0; i <= 256; ++i)
    {
        const uint32_t n = i == 0 ? 16 : i * 16;
        t[i] = static_cast<uint8_t>(normal_class_slow(n));
    }
    return t;
}

// kNormalSizes[kSmallClass[(n+15)/16]] is the size to allocate (n<=4kiB)
inline constexpr auto kSmallClass = make_small_class_table();
static_assert(kSmallClass[17] == 17); // requests 257..272
static_assert(kSmallClass[256] == normal_class_slow(4096));

inline uint32_t normal_class(size_t n) noexcept
{
    if (n == 0)
        return 1;
    if (n <= 256)
    {
        return static_cast<uint32_t>((n + 15) >> 4);
    }
    if (n <= 4096)
    {
        return kSmallClass[(n + 15) >> 4];
    }
    const unsigned k = 63u - static_cast<unsigned>(std::countl_zero(static_cast<uint64_t>(n)));
    const size_t base = size_t{1} << k;
    const size_t q = base >> 2;
    return static_cast<uint32_t>(kOctaveBase[k] + ((n - base + q - 1) >> (k - 2)));
}

inline uint32_t aligned_class_id(uint32_t normal_id, size_t align) noexcept
{
    const unsigned lg = static_cast<unsigned>(std::countr_zero(align));
    return kNormalClasses + (lg - kFirstAlignLog2) * kNormalClasses + normal_id;
}

inline uint32_t class_of(size_t n, size_t align) noexcept
{
    const uint32_t normal = normal_class(n);
    if (align <= 16)
        return normal;
    if (align > 4096)
        return 0;
    return aligned_class_id(normal, align);
}

} // namespace hpm
