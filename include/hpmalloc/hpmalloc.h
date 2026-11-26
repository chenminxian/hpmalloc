#ifndef HPMALLOC_H
#define HPMALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define HPMALLOC_VERSION_MAJOR "0"
#define HPMALLOC_VERSION_MINOR "1"
#define HPMALLOC_VERSION_PATCH "1"

    typedef struct hpm_stats
    {
        size_t reserved_bytes;     /* arena VA reservation */
        size_t committed_bytes;    /* slots/runs committed via mprotect */
        size_t active_bytes;       /* approximate live bytes */
        size_t free_slot_bytes;    /* free slot capacity */
        size_t tcache_bytes;       /* approximate bytes held by the calling thread */
        size_t span_count;         /* live spans */
        size_t partial_span_count; /* reserved for future use */
        size_t extent_count;       /* slots currently used as small extents */
        uint64_t purge_count;      /* madvise purge calls */
        uint64_t syscall_count;    /* reserved for future use */
    } hpm_stats_t;

    /*
     * Explicit allocator API.  The library also exports the standard malloc
     * family and C++ operator new/delete, so applications normally do not need
     * these functions.
     */
    void *hpm_malloc(size_t size);
    void hpm_free(void *ptr);
    void *hpm_calloc(size_t nmemb, size_t size);
    void *hpm_realloc(void *ptr, size_t size);
    void *hpm_memalign(size_t alignment, size_t size);
    size_t hpm_usable_size(const void *ptr);

    int hpm_stats_get(hpm_stats_t *out);
    int hpm_trim(size_t pad);
    int hpm_set_option(const char *name, const char *value);
    const char *hpm_version(void);

#ifdef __cplusplus
}
#endif

#endif /* HPMALLOC_H */
