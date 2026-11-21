// Global allocator state and thread-local caches shared by all translation units
// Keeping the definitions in one file avoids possible initialization-order problems

#include "allocator_internal.hpp"

namespace hpm
{

std::atomic<int> g_state{0}; // 0 new, 1 init, 2 ready, 3 failed
constinit SpinLock g_dir_lock;
Arena *g_arenas[kMaxArenas]{};
std::atomic<size_t> g_arena_count{0};

constinit MetaPool g_meta;

constinit CentralBin g_bins[kTotalClasses + 1][kCentralShards];

constinit DirectEntry g_direct[kDirectCapacity]{};
constinit SpinLock g_direct_lock;

std::atomic<int> g_thp_mode{kThpAuto};
std::atomic<int> g_purge_mode{kPurgeFree};

std::atomic<uint64_t> g_reserved_bytes{0};
std::atomic<uint64_t> g_committed_slots{0};
std::atomic<uint64_t> g_free_slots{0};
std::atomic<uint64_t> g_span_count{0};
std::atomic<uint64_t> g_extent_slots{0};
std::atomic<uint64_t> g_purge_count{0};
std::atomic<uint32_t> g_tcache_seq{0};

size_t g_default_arena_bytes{kDefaultArenaBytes};
size_t g_realloc_direct_threshold{kReallocDirectThreshold};
uint32_t g_realloc_slack_percent{kReallocSlackPercent};
bool g_class_map_enabled{true};

alignas(64) unsigned char g_bootstrap[64 * 1024];
std::atomic<size_t> g_bootstrap_off{0};

pthread_key_t g_thread_key;
std::atomic<bool> g_key_created{false};
std::atomic<int> g_hooks_state{0}; // 0 none, 1 creating, 2 done

} // namespace hpm
