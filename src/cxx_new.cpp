// This file exports C++ new/delete revelant ABI

#include "hpmalloc/hpmalloc.h"

#include <cstddef>
#include <new>

#if defined(__GNUC__) || defined(__clang__)
#define HPM_EXPORT __attribute__((visibility("default")))
#else
#define HPM_EXPORT
#endif

namespace
{

inline void *checked_alloc(std::size_t n)
{
    void *p = hpm_malloc(n ? n : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}

inline void *checked_aligned(std::size_t n, std::size_t align)
{
    void *p = hpm_memalign(align, n ? n : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}

} // anonymous namespace

HPM_EXPORT void *operator new(std::size_t n)
{
    return checked_alloc(n);
}

HPM_EXPORT void *operator new[](std::size_t n)
{
    return checked_alloc(n);
}

HPM_EXPORT void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    return hpm_malloc(n ? n : 1);
}

HPM_EXPORT void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
    return hpm_malloc(n ? n : 1);
}

HPM_EXPORT void *operator new(std::size_t n, std::align_val_t align)
{
    return checked_aligned(n, static_cast<std::size_t>(align));
}

HPM_EXPORT void *operator new[](std::size_t n, std::align_val_t align)
{
    return checked_aligned(n, static_cast<std::size_t>(align));
}

HPM_EXPORT void *operator new(std::size_t n, std::align_val_t align, const std::nothrow_t &) noexcept
{
    return hpm_memalign(static_cast<std::size_t>(align), n ? n : 1);
}

HPM_EXPORT void *operator new[](std::size_t n, std::align_val_t align, const std::nothrow_t &) noexcept
{
    return hpm_memalign(static_cast<std::size_t>(align), n ? n : 1);
}

HPM_EXPORT void operator delete(void *p) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete[](void *p) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete(void *p, std::size_t) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete[](void *p, std::size_t) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete(void *p, const std::nothrow_t &) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete[](void *p, const std::nothrow_t &) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete(void *p, std::align_val_t) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete[](void *p, std::align_val_t) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete(void *p, std::size_t, std::align_val_t) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete[](void *p, std::size_t, std::align_val_t) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
    hpm_free(p);
}

HPM_EXPORT void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
    hpm_free(p);
}
