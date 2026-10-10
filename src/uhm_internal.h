#ifndef UHM_INTERNAL_H
#define UHM_INTERNAL_H

#include "uhm/uhm.h"

#include <stdalign.h>
#include <stdatomic.h>

#if defined(_MSC_VER)
#include <intrin.h>
#pragma warning(disable : 4324)
#endif

#ifndef UHMAS_ENABLE_TEST_HOOKS
#define UHMAS_ENABLE_TEST_HOOKS 0
#endif

#ifndef UHMAS_ENABLE_DIAGNOSTICS
#define UHMAS_ENABLE_DIAGNOSTICS UHMAS_ENABLE_TEST_HOOKS
#endif

#ifndef UHMAS_STRICT_SLOT_OWNER_GEOMETRY
#define UHMAS_STRICT_SLOT_OWNER_GEOMETRY 0
#endif

#define UHM_MAGIC 0x55484d53u
#define UHM_THREAD_MAGIC 0x55485443u
#define UHM_TLS_CACHE_MAGIC 0x5548544cu
#define UHM_CLOSING_MAGIC 0x5548434cu
#define UHM_CLOSED_MAGIC 0x55484445u
#define UHM_BLOCK_MAGIC 0x5548424cu
#define UHM_VMA_MAGIC 0x5548564du
#define UHM_FREE_MAGIC 0x55484652u
#define UHM_CONTROL_MAGIC 0x55484354u
#define UHM_SLAB_MAGIC 0x5548534cu
#define UHM_SLAB_SLOT_MAGIC 0x55485353u
#define UHM_HEAP_F_CANARY UINT64_C(0x55484d41535f4654)

#define UHM_MIN_ALIGNMENT 16u
#define UHM_MIN_CONTROL_BAND (64u * 1024u)
#define UHM_PAGE_LEAF_PAGES 256u
#define UHM_PAGE_MAP_SHARDS 64u
#define UHM_EMERGENCY_VMA_NODES 4u
#define UHM_EMERGENCY_FREE_EXTENT_NODES 8u
#define UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET 8u
#define UHM_METADATA_HOT_VMA_MAX 64u
#define UHM_METADATA_HOT_FREE_EXTENT_MAX 64u
#define UHM_METADATA_HOT_VMA_DRAIN_LOW_WATER 32u
#define UHM_METADATA_HOT_FREE_EXTENT_DRAIN_LOW_WATER 32u
#define UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER 48u
#define UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER 48u
#define UHM_TRANSIENT_PROTECTING_RETRY_LIMIT 4096u
#define UHM_TLS_CACHE_MAX_SLOTS 64u
#define UHM_THREAD_CTX_DEFERRED_FREE_SLOTS 64u
#define UHM_METADATA_HAZARD_SLOTS 2u
#define UHM_LOCKLESS_REMOTE_READER_SLOTS 64u
#define UHM_LOCKLESS_REMOTE_SCAN_LIMIT 16u
#define UHM_RAW_REMOTE_BATCH_LIMIT 16u
#if UINTPTR_MAX <= UINT32_MAX
#define UHM_MALLOC_LIVE_STAT_SHARDS 16u
#define UHM_ACTIVE_OP_SHARDS 16u
#else
#define UHM_MALLOC_LIVE_STAT_SHARDS 256u
#define UHM_ACTIVE_OP_SHARDS 64u
#endif

#ifndef UHM_CACHELINE_SIZE
#define UHM_CACHELINE_SIZE 64u
#endif

#define UHM_TREE_PRIORITY_BUCKETS 16u
#define UHM_TREAP_HEALTH_REASON_DEPTH 0u
#define UHM_TREAP_HEALTH_REASON_INSERT 1u
#define UHM_TREAP_HEALTH_REASON_INSERT_ROTATION 2u
#define UHM_TREAP_HEALTH_REASON_FIND 3u
#define UHM_TREAP_HEALTH_REASON_REMOVE 4u
#define UHM_TREAP_HEALTH_TREE_STRIDE 8u
#define UHM_TREAP_HEALTH_LIVE_SHIFT 0u
#define UHM_TREAP_HEALTH_MALLOC_SHIFT 8u
#define UHM_TREAP_HEALTH_VMA_SHIFT 16u
#define UHM_TREAP_HEALTH_MMAP_FREE_SHIFT 24u
#define UHM_TREAP_HEALTH_BIT(tree_shift, reason) \
    (UINT64_C(1) << ((tree_shift) + (reason)))
#define UHM_TREAP_HEALTH_TREE_MASK(tree_shift) \
    (UINT64_C(0x1f) << (tree_shift))
#define UHM_TREAP_HEALTH_LIVE_DEPTH \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_LIVE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_DEPTH)
#define UHM_TREAP_HEALTH_LIVE_INSERT \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_LIVE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_INSERT)
#define UHM_TREAP_HEALTH_LIVE_INSERT_ROTATION \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_LIVE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_INSERT_ROTATION)
#define UHM_TREAP_HEALTH_LIVE_FIND \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_LIVE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_FIND)
#define UHM_TREAP_HEALTH_LIVE_REMOVE \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_LIVE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_REMOVE)
#define UHM_TREAP_HEALTH_MALLOC_DEPTH \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MALLOC_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_DEPTH)
#define UHM_TREAP_HEALTH_MALLOC_INSERT \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MALLOC_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_INSERT)
#define UHM_TREAP_HEALTH_MALLOC_INSERT_ROTATION \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MALLOC_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_INSERT_ROTATION)
#define UHM_TREAP_HEALTH_MALLOC_FIND \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MALLOC_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_FIND)
#define UHM_TREAP_HEALTH_MALLOC_REMOVE \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MALLOC_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_REMOVE)
#define UHM_TREAP_HEALTH_VMA_DEPTH \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_VMA_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_DEPTH)
#define UHM_TREAP_HEALTH_VMA_INSERT \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_VMA_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_INSERT)
#define UHM_TREAP_HEALTH_VMA_INSERT_ROTATION \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_VMA_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_INSERT_ROTATION)
#define UHM_TREAP_HEALTH_VMA_FIND \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_VMA_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_FIND)
#define UHM_TREAP_HEALTH_VMA_REMOVE \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_VMA_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_REMOVE)
#define UHM_TREAP_HEALTH_MMAP_FREE_DEPTH \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MMAP_FREE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_DEPTH)
#define UHM_TREAP_HEALTH_MMAP_FREE_INSERT \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MMAP_FREE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_INSERT)
#define UHM_TREAP_HEALTH_MMAP_FREE_INSERT_ROTATION \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MMAP_FREE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_INSERT_ROTATION)
#define UHM_TREAP_HEALTH_MMAP_FREE_FIND \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MMAP_FREE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_FIND)
#define UHM_TREAP_HEALTH_MMAP_FREE_REMOVE \
    UHM_TREAP_HEALTH_BIT(UHM_TREAP_HEALTH_MMAP_FREE_SHIFT, \
                         UHM_TREAP_HEALTH_REASON_REMOVE)
#define UHM_TREAP_HEALTH_LIVE_MASK \
    UHM_TREAP_HEALTH_TREE_MASK(UHM_TREAP_HEALTH_LIVE_SHIFT)
#define UHM_TREAP_HEALTH_MALLOC_MASK \
    UHM_TREAP_HEALTH_TREE_MASK(UHM_TREAP_HEALTH_MALLOC_SHIFT)
#define UHM_TREAP_HEALTH_VMA_MASK \
    UHM_TREAP_HEALTH_TREE_MASK(UHM_TREAP_HEALTH_VMA_SHIFT)
#define UHM_TREAP_HEALTH_MMAP_FREE_MASK \
    UHM_TREAP_HEALTH_TREE_MASK(UHM_TREAP_HEALTH_MMAP_FREE_SHIFT)
_Static_assert((UHM_TREAP_HEALTH_LIVE_MASK |
                UHM_TREAP_HEALTH_MALLOC_MASK |
                UHM_TREAP_HEALTH_VMA_MASK |
                UHM_TREAP_HEALTH_MMAP_FREE_MASK) <= UINT32_MAX,
               "treap repair pending flags must fit uint32_t");
#define UHM_CORRUPTION_SITE_RETIRED_VMA_TAIL 4301u
#define UHM_CORRUPTION_SITE_RETIRED_FREE_TAIL 4302u
#define UHM_MMAP_RECENT_FREE_CACHE_SLOTS 16u
#define UHM_MMAP_FREE_ADJACENT_LIST_SCAN_LIMIT 64u
#define UHM_MMAP_SIZE_HINT_BUCKETS 8u
#define UHM_MMAP_SIZE_HINT_SLOTS 8u
#define UHM_DECOMMIT_RECENT_RANGE_SLOTS 4u
#define UHM_BLOCK_DECOMMIT_SEGMENT_MAX 64u
#define UHM_MALLOC_FREE_DECOMMIT_RETENTION_SHRINK_IDLE_DRAINS 4u
#define UHM_PAGE_RANGE_DIAG_PAGE_BUDGET 64u
#define UHM_PAGE_RANGE_DIAG_SHARD_BUDGET 2u
#define UHM_CONTROL_ALLOC_HINT_BUCKETS 8u
#define UHM_CONTROL_ALLOC_HINT_SLOTS 4u
#define UHM_CONTROL_ALLOC_SCAN_BUDGET 8u
#define UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT 5u
#define UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN 0u
#define UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE 1u
#define UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC 2u
#define UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE 3u
#define UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED 4u
#define UHM_LIVE_BLOCK_FAST_FAIL_REASON_COUNT 6u
#define UHM_LIVE_BLOCK_FAST_FAIL_NONE 0u
#define UHM_LIVE_BLOCK_FAST_FAIL_INVALID 1u
#define UHM_LIVE_BLOCK_FAST_FAIL_RANGE 2u
#define UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE 3u
#define UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE 4u
#define UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR 5u
#define UHM_LIVE_BLOCK_HEALTH_METADATA_DRIFT (UINT64_C(1) << 0)

#define UHM_PAGE_MARK_PHASE_COUNT 7u
#define UHM_PAGE_MARK_PHASE_UNKNOWN 0u
#define UHM_PAGE_MARK_PHASE_CONTROL 1u
#define UHM_PAGE_MARK_PHASE_MALLOC_LIVE 2u
#define UHM_PAGE_MARK_PHASE_MALLOC_DECOMMITTED 3u
#define UHM_PAGE_MARK_PHASE_MMAP_LIVE 4u
#define UHM_PAGE_MARK_PHASE_FREE_REUSABLE 5u
#define UHM_PAGE_MARK_PHASE_PROTECTING 6u

#define UHM_PAGE_QUERY_PHASE_COUNT 10u
#define UHM_PAGE_QUERY_PHASE_IS_FREE 0u
#define UHM_PAGE_QUERY_PHASE_HAS_OWNER 1u
#define UHM_PAGE_QUERY_PHASE_HAS_STATE 2u
#define UHM_PAGE_QUERY_PHASE_MATCHES_DESCRIPTOR 3u
#define UHM_PAGE_QUERY_PHASE_MATCHES_DESCRIPTOR_PROTECTING 4u
#define UHM_PAGE_QUERY_PHASE_OVERLAPS_OWNER 5u
#define UHM_PAGE_QUERY_PHASE_HAS_LIVE_MMAP 6u
#define UHM_PAGE_QUERY_PHASE_OVERLAPS_VA_STATE 7u
#define UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE 8u
#define UHM_PAGE_QUERY_PHASE_MATCHES_GENERATION 9u

#define UHM_CLOSE_PHASE_METADATA_EPOCH 0u
#define UHM_CLOSE_PHASE_LOCAL_REGISTRY_DRAIN 1u
#define UHM_CLOSE_PHASE_FALLBACK_MAINTENANCE 2u
#define UHM_CLOSE_PHASE_FINAL_CLASS_PUBLISH 3u
#define UHM_CLOSE_PHASE_REGISTRY_DETACH 4u
#define UHM_CLOSE_PHASE_TLS_KEY_DELETE 5u
#define UHM_CLOSE_PHASE_ARENA_RELEASE 6u
#define UHM_CLOSE_PHASE_COUNT 7u

#define UHM_CACHELINE_ALIGN alignas(UHM_CACHELINE_SIZE)
#define UHM_CACHELINE_PAD_FOR(type_name) \
    (UHM_CACHELINE_SIZE - (sizeof(type_name) % UHM_CACHELINE_SIZE))

#define UHM_META_FLAG_EMERGENCY (1u << 31)
#define UHM_META_FLAG_IN_MMAP_FREE_LIST (1u << 30)
#define UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS 8u
#define UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS 3u
#define UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_MASK \
    ((UINT64_C(1) << UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS) - 1u)
#define UHM_LIVE_BLOCK_NEGATIVE_CACHE_EPOCH_MAX \
    (UINT64_MAX >> UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS)
#define UHM_LIVE_BLOCK_FALLBACK_SCENARIO_MAX_SCANS 2u

#define UHM_FREE_EXTENT_FLAG_IN_MMAP_SIZE_HINT (1u << 0)
#define UHM_FREE_EXTENT_FLAG_IN_MMAP_RECENT (1u << 1)
#define UHM_FREE_EXTENT_SIZE_HINT_BUCKET_SHIFT 8u
#define UHM_FREE_EXTENT_SIZE_HINT_SLOT_SHIFT 16u
#define UHM_FREE_EXTENT_RECENT_SLOT_SHIFT 24u
#define UHM_FREE_EXTENT_SIZE_HINT_INDEX_MASK 0xffu
#define UHM_FREE_EXTENT_RECENT_SLOT_INDEX_MASK 0x3fu
#define UHM_FREE_EXTENT_SIZE_HINT_BUCKET_MASK \
    (UHM_FREE_EXTENT_SIZE_HINT_INDEX_MASK << \
     UHM_FREE_EXTENT_SIZE_HINT_BUCKET_SHIFT)
#define UHM_FREE_EXTENT_SIZE_HINT_SLOT_MASK \
    (UHM_FREE_EXTENT_SIZE_HINT_INDEX_MASK << \
     UHM_FREE_EXTENT_SIZE_HINT_SLOT_SHIFT)
#define UHM_FREE_EXTENT_RECENT_SLOT_MASK \
    (UHM_FREE_EXTENT_RECENT_SLOT_INDEX_MASK << \
     UHM_FREE_EXTENT_RECENT_SLOT_SHIFT)
#define UHM_FREE_EXTENT_SIZE_HINT_MASK \
    (UHM_FREE_EXTENT_FLAG_IN_MMAP_SIZE_HINT | \
     UHM_FREE_EXTENT_SIZE_HINT_BUCKET_MASK | \
     UHM_FREE_EXTENT_SIZE_HINT_SLOT_MASK)
#define UHM_FREE_EXTENT_RECENT_MASK \
    (UHM_FREE_EXTENT_FLAG_IN_MMAP_RECENT | \
     UHM_FREE_EXTENT_RECENT_SLOT_MASK)
#define UHM_FREE_EXTENT_CACHE_POSITION_MASK \
    (UHM_FREE_EXTENT_SIZE_HINT_MASK | UHM_FREE_EXTENT_RECENT_MASK)

#define UHM_CONTROL_EXTENT_FLAG_HINTED (1u << 0)
#define UHM_CONTROL_EXTENT_HINT_BUCKET_SHIFT 8u
#define UHM_CONTROL_EXTENT_HINT_SLOT_SHIFT 16u
#define UHM_CONTROL_EXTENT_HINT_INDEX_MASK 0xffu
#define UHM_CONTROL_EXTENT_HINT_BUCKET_MASK \
    (UHM_CONTROL_EXTENT_HINT_INDEX_MASK << \
     UHM_CONTROL_EXTENT_HINT_BUCKET_SHIFT)
#define UHM_CONTROL_EXTENT_HINT_SLOT_MASK \
    (UHM_CONTROL_EXTENT_HINT_INDEX_MASK << UHM_CONTROL_EXTENT_HINT_SLOT_SHIFT)
#define UHM_CONTROL_EXTENT_HINT_MASK \
    (UHM_CONTROL_EXTENT_FLAG_HINTED | UHM_CONTROL_EXTENT_HINT_BUCKET_MASK | \
     UHM_CONTROL_EXTENT_HINT_SLOT_MASK)

#define UHM_PAGE_OWNER_FREE 0u
#define UHM_PAGE_OWNER_CONTROL 1u
#define UHM_PAGE_OWNER_MALLOC 2u
#define UHM_PAGE_OWNER_MMAP 3u

#define UHM_PAGE_VA_FREE_RESERVED 0u
#define UHM_PAGE_VA_CONTROL_LIVE 1u
#define UHM_PAGE_VA_MALLOC_LIVE 2u
#define UHM_PAGE_VA_MMAP_LIVE 3u
#define UHM_PAGE_VA_FREE_REUSABLE 4u
#define UHM_PAGE_VA_PROTECTING 5u

#define UHM_PAGE_COMMIT_NONE 0u
#define UHM_PAGE_COMMIT_LIVE 1u
#define UHM_PAGE_COMMIT_DECOMMITTED 2u

#define UHM_EXTENT_DESCRIPTOR_MAGIC 0x55484544u

#define UHM_EXTENT_KIND_NONE 0u
#define UHM_EXTENT_KIND_CONTROL 1u
#define UHM_EXTENT_KIND_MALLOC 2u
#define UHM_EXTENT_KIND_MMAP 3u
#define UHM_EXTENT_KIND_MMAP_FREE 4u

#define UHM_BLOCK_FLAG_FREE (1u << 0)
#define UHM_BLOCK_FLAG_PAGE_PRIVATE (1u << 1)
#define UHM_BLOCK_FLAG_DECOMMITTED (1u << 2)
#define UHM_BLOCK_FLAG_DECOMMIT_RETAINED (1u << 3)
#define UHM_BLOCK_FLAG_VM_BUSY (1u << 4)
#define UHM_BLOCK_FLAG_DECOMMIT_HOT (1u << 5)
#define UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN (1u << 6)

#define UHM_SLAB_FLAG_DECOMMITTED (1u << 0)
#define UHM_SLAB_FLAG_VM_BUSY (1u << 1)

#define UHM_SLAB_SLOT_FLAG_FREE (1u << 0)
#define UHM_SLAB_SLOT_FLAG_CTX_CACHE (1u << 1)
#define UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE (1u << 2)
#define UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING (1u << 3)
#define UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED (1u << 4)
#define UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED (1u << 5)
#define UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING (1u << 6)
#define UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT (1u << 7)

#define UHM_SLAB_STATE_NONE 0u
#define UHM_SLAB_STATE_PARTIAL 1u
#define UHM_SLAB_STATE_FULL 2u
#define UHM_SLAB_STATE_EMPTY 3u

#define UHM_REMOTE_DRAIN_OWNER_ACTIVE 1u
#define UHM_REMOTE_DRAIN_OWNER_TRANSIENT UINT32_MAX

#define UHM_SIZE_CLASS_NONE UINT16_MAX
#define UHM_SIZE_CLASS_COUNT 28u
_Static_assert(UHM_SIZE_CLASS_COUNT < 32u,
               "local cache class mask requires fewer than 32 classes");
#define UHM_SLAB_REMOTE_PENDING_MASK_WORD_BITS 8u
#define UHM_SLAB_REMOTE_PENDING_MASK_WORD_COUNT \
    ((UHM_SIZE_CLASS_COUNT + UHM_SLAB_REMOTE_PENDING_MASK_WORD_BITS - 1u) / \
     UHM_SLAB_REMOTE_PENDING_MASK_WORD_BITS)
#define UHM_VMA_SHARDS 16u
#define UHM_EXTENT_SHARDS 16u
#define UHM_SLAB_SHARDS UHM_SIZE_CLASS_COUNT
#define UHM_SLAB_EMPTY_COMMITTED_CACHE_LOW_WATER 1u
#define UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER 4u
#define UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER 16u
#define UHM_SLAB_EMPTY_COMMITTED_CACHE_IDLE_SHRINK_DRAINS 8u
/*
 * Keep the full-drain transaction bounded, but large enough to coalesce a
 * contiguous run that crossed the original eight-candidate window.  The
 * claims remain cold-path stack state; no heap, TLS, ABI, or global state is
 * added.
 */
#define UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX 32u
#define UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_HIGH_WATER_MASK ((size_t)0xffu)
#define UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_IDLE_SHIFT 8u
#define UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_IDLE_MASK \
    ((size_t)0xffu << UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_IDLE_SHIFT)
#define UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_MASK \
    (UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_HIGH_WATER_MASK | \
     UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_IDLE_MASK)

_Static_assert(UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER <=
                   UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_HIGH_WATER_MASK,
               "slab empty high-water must fit encoded state");

static inline size_t uhm_slab_empty_committed_cache_state_high_water(
    size_t state) {
    return state & UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_HIGH_WATER_MASK;
}

static inline uint8_t uhm_slab_empty_committed_cache_state_idle_drains(
    size_t state) {
    return (uint8_t)((state &
                      UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_IDLE_MASK) >>
                     UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_IDLE_SHIFT);
}

static inline size_t uhm_slab_empty_committed_cache_state_make(
    size_t high_water,
    uint8_t idle_drains) {
    return (high_water &
            UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_HIGH_WATER_MASK) |
           ((size_t)idle_drains <<
            UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_IDLE_SHIFT);
}

static inline int uhm_slab_empty_committed_cache_state_is_valid(size_t state) {
    size_t high_water =
        uhm_slab_empty_committed_cache_state_high_water(state);
    uint8_t idle_drains =
        uhm_slab_empty_committed_cache_state_idle_drains(state);

    return (state & ~UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_MASK) == 0 &&
           (high_water == UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER ||
            high_water == UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER * 2u ||
            high_water ==
                UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER) &&
           idle_drains < UHM_SLAB_EMPTY_COMMITTED_CACHE_IDLE_SHRINK_DRAINS &&
           (high_water != UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER ||
            idle_drains == 0u);
}

#define UHM_TX_OP_NONE 0u
#define UHM_TX_OP_MMAP_AUTO 1u
#define UHM_TX_OP_MMAP_FIXED 2u
#define UHM_TX_OP_MUNMAP 3u
#define UHM_TX_OP_MPROTECT 4u
#define UHM_TX_OP_MREMAP 5u

#define UHM_TX_STAGE_INIT 0u
#define UHM_TX_STAGE_VALIDATE 1u
#define UHM_TX_STAGE_CLAIMED 2u
#define UHM_TX_STAGE_PREPARED 3u
#define UHM_TX_STAGE_PLATFORM 4u
#define UHM_TX_STAGE_PUBLISH 5u
#define UHM_TX_STAGE_ROLLBACK 6u
#define UHM_TX_STAGE_DONE 7u

#define UHM_LOCK_RANK_NONE 0u
#define UHM_LOCK_RANK_HEAP 1u
#define UHM_LOCK_RANK_PAGE_MAP 2u
#define UHM_LOCK_RANK_VMA 3u
#define UHM_LOCK_RANK_EXTENT 4u
#define UHM_LOCK_RANK_SLAB 5u
#define UHM_LOCK_RANK_VM_GATE 6u

#define UHM_CLAIM_SOURCE_NONE 0u
#define UHM_CLAIM_SOURCE_LOW_WATERMARK 1u
#define UHM_CLAIM_SOURCE_HIGH_WATERMARK 2u
#define UHM_CLAIM_SOURCE_MMAP_FREE 3u

#define UHM_CLAIM_FLAG_LOW_WATERMARK_INFLIGHT 0x1u

/*
 * Atomic order policy:
 * - RELAXED: object initialization or statistics/diagnostics with no ordering edge.
 * - ACQUIRE: entering heap-owned critical sections or reading published diagnostics.
 * - RELEASE: leaving heap-owned critical sections or publishing diagnostics.
 * - ACQ_REL: reserved for future CAS-based freelists, shards, and remote queues.
 */
#define UHM_ATOMIC_RELAXED memory_order_relaxed
#define UHM_ATOMIC_ACQUIRE memory_order_acquire
#define UHM_ATOMIC_RELEASE memory_order_release
#define UHM_ATOMIC_ACQ_REL memory_order_acq_rel

#if UINTPTR_MAX <= UINT32_MAX
typedef uint64_t uhm_diag_counter_u64;
#define UHM_DIAG_COUNTER_ATOMIC 0
#else
typedef _Atomic(uint64_t) uhm_diag_counter_u64;
#define UHM_DIAG_COUNTER_ATOMIC 1
#endif

static inline void uhm_diag_counter_store_relaxed(uhm_diag_counter_u64 *slot,
                                                  uint64_t value) {
#if !UHMAS_ENABLE_DIAGNOSTICS
    (void)slot;
    (void)value;
#else
    if (!slot) {
        return;
    }
#if UHM_DIAG_COUNTER_ATOMIC
    atomic_store_explicit(slot, value, UHM_ATOMIC_RELAXED);
#else
    *slot = value;
#endif
#endif
}

static inline uint64_t
uhm_diag_counter_load_relaxed(const uhm_diag_counter_u64 *slot) {
#if !UHMAS_ENABLE_DIAGNOSTICS
    (void)slot;
    return UINT64_C(0);
#else
    if (!slot) {
        return UINT64_C(0);
    }
#if UHM_DIAG_COUNTER_ATOMIC
    return atomic_load_explicit(slot, UHM_ATOMIC_RELAXED);
#else
    return *slot;
#endif
#endif
}

static inline uint64_t
uhm_diag_counter_exchange_relaxed(uhm_diag_counter_u64 *slot, uint64_t value) {
#if !UHMAS_ENABLE_DIAGNOSTICS
    (void)slot;
    (void)value;
    return UINT64_C(0);
#else
    if (!slot) {
        return UINT64_C(0);
    }
#if UHM_DIAG_COUNTER_ATOMIC
    return atomic_exchange_explicit(slot, value, UHM_ATOMIC_RELAXED);
#else
    {
        uint64_t old = *slot;
        *slot = value;
        return old;
    }
#endif
#endif
}

static inline void uhm_diag_counter_add_relaxed(uhm_diag_counter_u64 *slot,
                                                uint64_t value) {
#if !UHMAS_ENABLE_DIAGNOSTICS
    (void)slot;
    (void)value;
#else
    if (!slot || value == 0) {
        return;
    }
#if UHM_DIAG_COUNTER_ATOMIC
    (void)atomic_fetch_add_explicit(slot, value, UHM_ATOMIC_RELAXED);
#else
    *slot += value;
#endif
#endif
}

static inline void uhm_diag_counter_max_relaxed(uhm_diag_counter_u64 *slot,
                                                uint64_t value) {
#if !UHMAS_ENABLE_DIAGNOSTICS
    (void)slot;
    (void)value;
#else
    if (!slot || value == 0) {
        return;
    }
#if UHM_DIAG_COUNTER_ATOMIC
    {
    uint64_t current;
    unsigned attempts = 0u;

    current = atomic_load_explicit(slot, UHM_ATOMIC_RELAXED);
    while (current < value) {
        if (atomic_compare_exchange_weak_explicit(
                slot, &current, value, UHM_ATOMIC_RELAXED,
                UHM_ATOMIC_RELAXED)) {
            return;
        }
        if (++attempts >= 64u) {
            current = atomic_load_explicit(slot, UHM_ATOMIC_RELAXED);
            if (current < value) {
                atomic_store_explicit(slot, value, UHM_ATOMIC_RELAXED);
            }
            return;
        }
    }
    }
#else
    if (*slot < value) {
        *slot = value;
    }
#endif
#endif
}

typedef struct uhm_spinlock {
    atomic_flag flag;
    _Atomic(uint32_t) held;
    uint32_t mode;
    uint32_t yield_after_spins;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint32_t diagnostics_enabled;
    uint32_t diagnostics_timing_enabled;
#if defined(_WIN32)
    uint64_t diagnostics_timer_frequency;
#endif
    uhm_diag_counter_u64 lock_calls;
    uhm_diag_counter_u64 try_calls;
    uhm_diag_counter_u64 try_failures;
    uhm_diag_counter_u64 contended_calls;
    uhm_diag_counter_u64 wait_ns;
    uhm_diag_counter_u64 max_wait_ns;
    uhm_diag_counter_u64 hold_ns;
    uhm_diag_counter_u64 max_hold_ns;
    uhm_diag_counter_u64 acquired_ns;
#endif
} uhm_spinlock;

static inline void uhm_spinlock_set_diagnostics_enabled(
    uhm_spinlock *lock, uint32_t enabled) {
#if UHMAS_ENABLE_DIAGNOSTICS
    if (lock) {
        lock->diagnostics_enabled = enabled != 0u ? 1u : 0u;
    }
#else
    (void)lock;
    (void)enabled;
#endif
}

static inline int uhm_spinlock_diagnostics_enabled(
    const uhm_spinlock *lock) {
#if UHMAS_ENABLE_DIAGNOSTICS
    return lock && lock->diagnostics_enabled != 0u;
#else
    (void)lock;
    return 0;
#endif
}

static inline void uhm_spinlock_set_diagnostics_timing_enabled(
    uhm_spinlock *lock, uint32_t enabled) {
#if UHMAS_ENABLE_DIAGNOSTICS
    if (lock) {
        lock->diagnostics_timing_enabled = enabled != 0u ? 1u : 0u;
    }
#else
    (void)lock;
    (void)enabled;
#endif
}

static inline int uhm_spinlock_diagnostics_timing_enabled(
    const uhm_spinlock *lock) {
#if UHMAS_ENABLE_DIAGNOSTICS
    return lock && lock->diagnostics_timing_enabled != 0u;
#else
    (void)lock;
    return 0;
#endif
}

static inline uint64_t uhm_spinlock_diagnostics_timer_frequency(
    const uhm_spinlock *lock) {
#if UHMAS_ENABLE_DIAGNOSTICS && defined(_WIN32)
    return lock ? lock->diagnostics_timer_frequency : UINT64_C(0);
#else
    (void)lock;
    return UINT64_C(0);
#endif
}

typedef struct uhm_spinlock_cell {
    uhm_spinlock lock;
    unsigned char padding[UHM_CACHELINE_PAD_FOR(uhm_spinlock)];
} uhm_spinlock_cell;

_Static_assert((UHM_CACHELINE_SIZE & (UHM_CACHELINE_SIZE - 1u)) == 0u,
               "cache-line size must be a power of two");
_Static_assert((UHM_MALLOC_LIVE_STAT_SHARDS &
                (UHM_MALLOC_LIVE_STAT_SHARDS - 1u)) == 0u,
               "malloc live stat shard count must be a power of two");
_Static_assert((UHM_ACTIVE_OP_SHARDS & (UHM_ACTIVE_OP_SHARDS - 1u)) == 0u,
               "active op shard count must be a power of two");
_Static_assert(UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER <=
                   UHM_METADATA_HOT_VMA_MAX,
               "hot VMA drain high-water must fit hot cache");
_Static_assert(UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER <=
                   UHM_METADATA_HOT_FREE_EXTENT_MAX,
               "hot free extent drain high-water must fit hot cache");
_Static_assert(UHM_METADATA_HOT_VMA_DRAIN_LOW_WATER <
                   UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER,
               "hot VMA drain low-water must stay below high-water");
_Static_assert(UHM_METADATA_HOT_FREE_EXTENT_DRAIN_LOW_WATER <
                   UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER,
               "hot free extent drain low-water must stay below high-water");
_Static_assert(sizeof(uhm_spinlock_cell) % UHM_CACHELINE_SIZE == 0u,
               "spinlock cell size must be cache-line sized");

#define UHM_SPINLOCK_MODE_BURN 0u
#define UHM_SPINLOCK_MODE_YIELD 1u
#define UHM_SPINLOCK_DEFAULT_YIELD_AFTER 256u

#if UHMAS_ENABLE_DIAGNOSTICS
#define UHM_DIAG_CORE_FIELD_PTR(heap_, field_) \
    ((heap_) ? &(heap_)->core.field_ : 0)
#else
#define UHM_DIAG_CORE_FIELD_PTR(heap_, field_) ((uint64_t *)0)
#endif

typedef struct uhm_tagged_freelist_node uhm_tagged_freelist_node;

struct uhm_tagged_freelist_node {
    _Atomic(uhm_tagged_freelist_node *) next;
};

typedef struct uhm_tagged_freelist_head_fields {
    _Atomic(uintptr_t) head;
} uhm_tagged_freelist_head_fields;

typedef struct uhm_tagged_freelist {
    _Atomic(uintptr_t) head;
    unsigned char head_padding[UHM_CACHELINE_PAD_FOR(uhm_tagged_freelist_head_fields)];
    UHM_CACHELINE_ALIGN atomic_flag overflow_lock;
    _Atomic(uint64_t) cas_failures;
    _Atomic(uint64_t) tag_overflows;
    _Atomic(uint64_t) tag_wrap_deferrals;
    _Atomic(uint64_t) last_wrap_epoch;
} uhm_tagged_freelist;

typedef struct uhm_tagged_freelist_cell {
    uhm_tagged_freelist queue;
    unsigned char padding[UHM_CACHELINE_PAD_FOR(uhm_tagged_freelist)];
} uhm_tagged_freelist_cell;

typedef struct uhm_atomic_u64_cell {
    _Atomic(uint64_t) value;
    unsigned char padding[UHM_CACHELINE_PAD_FOR(_Atomic(uint64_t))];
} uhm_atomic_u64_cell;

typedef struct uhm_atomic_u32_cell {
    _Atomic(uint32_t) value;
    unsigned char padding[UHM_CACHELINE_PAD_FOR(_Atomic(uint32_t))];
} uhm_atomic_u32_cell;

typedef struct uhm_atomic_size_cell {
    _Atomic(size_t) value;
    unsigned char padding[UHM_CACHELINE_PAD_FOR(_Atomic(size_t))];
} uhm_atomic_size_cell;

_Static_assert(sizeof(uhm_tagged_freelist_cell) % UHM_CACHELINE_SIZE == 0u,
               "tagged freelist cell size must be cache-line sized");
_Static_assert(offsetof(uhm_tagged_freelist, overflow_lock) %
                       UHM_CACHELINE_SIZE ==
                   0u,
               "tagged freelist cold fields must start on a cache line");
_Static_assert(sizeof(uhm_tagged_freelist) % UHM_CACHELINE_SIZE == 0u,
               "tagged freelist size must be cache-line sized");
_Static_assert(sizeof(uhm_atomic_u64_cell) % UHM_CACHELINE_SIZE == 0u,
               "u64 atomic cell size must be cache-line sized");
_Static_assert(sizeof(uhm_atomic_u32_cell) % UHM_CACHELINE_SIZE == 0u,
               "u32 atomic cell size must be cache-line sized");
_Static_assert(sizeof(uhm_atomic_size_cell) % UHM_CACHELINE_SIZE == 0u,
               "size atomic cell size must be cache-line sized");

static inline uhm_tagged_freelist_node *uhm_tagged_freelist_next_load(
    const uhm_tagged_freelist_node *node) {
    return node ? atomic_load_explicit(&node->next, UHM_ATOMIC_ACQUIRE) : 0;
}

static inline void uhm_tagged_freelist_next_store(
    uhm_tagged_freelist_node *node,
    uhm_tagged_freelist_node *next) {
    if (node) {
        atomic_store_explicit(&node->next, next, UHM_ATOMIC_RELEASE);
    }
}

#if UINTPTR_MAX > UINT32_MAX
#define UHM_TAGGED_FREELIST_TAG_BITS 16u
#define UHM_TAGGED_FREELIST_TAG_SHIFT 48u
#define UHM_TAGGED_FREELIST_PTR_MASK ((uintptr_t)UINT64_C(0x0000FFFFFFFFFFFF))
#define UHM_TAGGED_FREELIST_TAG_MASK ((uint32_t)0xFFFFu)
#else
#define UHM_TAGGED_FREELIST_TAG_BITS 0u
#define UHM_TAGGED_FREELIST_TAG_SHIFT 0u
#define UHM_TAGGED_FREELIST_PTR_MASK ((uintptr_t)0u)
#define UHM_TAGGED_FREELIST_TAG_MASK ((uint32_t)0u)
#endif

static inline void uhm_tagged_freelist_init(uhm_tagged_freelist *list) {
    if (!list) {
        return;
    }
    atomic_init(&list->head, (uintptr_t)0);
    atomic_flag_clear_explicit(&list->overflow_lock, UHM_ATOMIC_RELAXED);
    atomic_init(&list->cas_failures, UINT64_C(0));
    atomic_init(&list->tag_overflows, UINT64_C(0));
    atomic_init(&list->tag_wrap_deferrals, UINT64_C(0));
    atomic_init(&list->last_wrap_epoch, UINT64_C(0));
}

static inline int uhm_tagged_freelist_is_supported(void) {
#if UINTPTR_MAX > UINT32_MAX
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    return 1;
#else
    return ATOMIC_POINTER_LOCK_FREE == 2;
#endif
#else
    return 0;
#endif
}

static inline int uhm_atomic_u64_is_lock_free(void) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    return 1;
#else
#if defined(ATOMIC_LLONG_LOCK_FREE) && ATOMIC_LLONG_LOCK_FREE == 2
    if (sizeof(uint64_t) <= sizeof(unsigned long long)) {
        return 1;
    }
#endif
#if defined(ATOMIC_LONG_LOCK_FREE) && ATOMIC_LONG_LOCK_FREE == 2
    if (sizeof(uint64_t) <= sizeof(unsigned long)) {
        return 1;
    }
#endif
    return 0;
#endif
}

static inline int uhm_atomic_u32_is_lock_free(void) {
#if defined(_MSC_VER) && \
    (defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64))
    return 1;
#else
#if defined(__GCC_ATOMIC_INT_LOCK_FREE) && __GCC_ATOMIC_INT_LOCK_FREE == 2
    if (sizeof(uint32_t) <= sizeof(unsigned int)) {
        return 1;
    }
#endif
#if defined(__GCC_ATOMIC_LONG_LOCK_FREE) && __GCC_ATOMIC_LONG_LOCK_FREE == 2
    if (sizeof(uint32_t) <= sizeof(unsigned long)) {
        return 1;
    }
#endif
#if defined(ATOMIC_INT_LOCK_FREE) && ATOMIC_INT_LOCK_FREE == 2
    if (sizeof(uint32_t) <= sizeof(unsigned int)) {
        return 1;
    }
#endif
#if defined(ATOMIC_LONG_LOCK_FREE) && ATOMIC_LONG_LOCK_FREE == 2
    if (sizeof(uint32_t) <= sizeof(unsigned long)) {
        return 1;
    }
#endif
    return 0;
#endif
}

static inline int uhm_lockless_remote_primitives_supported(void) {
    return uhm_tagged_freelist_is_supported() &&
           uhm_atomic_u64_is_lock_free();
}

static inline int uhm_tagged_freelist_ptr_packable(const void *ptr) {
#if UINTPTR_MAX > UINT32_MAX
    uintptr_t value = (uintptr_t)ptr;
    return (value & ~UHM_TAGGED_FREELIST_PTR_MASK) == 0;
#else
    (void)ptr;
    return 0;
#endif
}

static inline uintptr_t uhm_tagged_freelist_pack(
    const uhm_tagged_freelist_node *node,
    uint32_t tag) {
#if UINTPTR_MAX > UINT32_MAX
    return (((uintptr_t)(tag & UHM_TAGGED_FREELIST_TAG_MASK))
            << UHM_TAGGED_FREELIST_TAG_SHIFT) |
           (((uintptr_t)node) & UHM_TAGGED_FREELIST_PTR_MASK);
#else
    (void)node;
    (void)tag;
    return (uintptr_t)0;
#endif
}

static inline uhm_tagged_freelist_node *uhm_tagged_freelist_unpack_ptr(
    uintptr_t packed) {
#if UINTPTR_MAX > UINT32_MAX
    return (uhm_tagged_freelist_node *)(packed & UHM_TAGGED_FREELIST_PTR_MASK);
#else
    (void)packed;
    return 0;
#endif
}

static inline uint32_t uhm_tagged_freelist_unpack_tag(uintptr_t packed) {
#if UINTPTR_MAX > UINT32_MAX
    return (uint32_t)((packed >> UHM_TAGGED_FREELIST_TAG_SHIFT) &
                      UHM_TAGGED_FREELIST_TAG_MASK);
#else
    (void)packed;
    return 0;
#endif
}

static inline void uhm_tagged_freelist_wrap_tag(uhm_tagged_freelist *list) {
    uintptr_t old_head;
    uintptr_t new_head;
    uint32_t tag;

    while (atomic_flag_test_and_set_explicit(&list->overflow_lock,
                                             UHM_ATOMIC_ACQUIRE)) {
    }
    old_head = atomic_load_explicit(&list->head, UHM_ATOMIC_ACQUIRE);
    tag = uhm_tagged_freelist_unpack_tag(old_head);
    if (tag == UHM_TAGGED_FREELIST_TAG_MASK) {
        new_head = uhm_tagged_freelist_pack(
            uhm_tagged_freelist_unpack_ptr(old_head), 0u);
        if (atomic_compare_exchange_strong_explicit(
                &list->head, &old_head, new_head, UHM_ATOMIC_ACQ_REL,
                UHM_ATOMIC_ACQUIRE)) {
            (void)atomic_fetch_add_explicit(&list->tag_overflows, UINT64_C(1),
                                            UHM_ATOMIC_RELAXED);
        }
    }
    atomic_flag_clear_explicit(&list->overflow_lock, UHM_ATOMIC_RELEASE);
}

static inline int uhm_tagged_freelist_push(uhm_tagged_freelist *list,
                                           uhm_tagged_freelist_node *node) {
    uintptr_t old_head;
    uintptr_t new_head;
    uint32_t tag;

    if (!list || !node || !uhm_tagged_freelist_is_supported() ||
        !uhm_tagged_freelist_ptr_packable(node)) {
        return 0;
    }
    for (;;) {
        old_head = atomic_load_explicit(&list->head, UHM_ATOMIC_ACQUIRE);
        tag = uhm_tagged_freelist_unpack_tag(old_head);
        if (tag == UHM_TAGGED_FREELIST_TAG_MASK) {
            uhm_tagged_freelist_wrap_tag(list);
            continue;
        }
        uhm_tagged_freelist_next_store(
            node, uhm_tagged_freelist_unpack_ptr(old_head));
        new_head = uhm_tagged_freelist_pack(node, tag + 1u);
        if (atomic_compare_exchange_weak_explicit(
                &list->head, &old_head, new_head, UHM_ATOMIC_ACQ_REL,
                UHM_ATOMIC_ACQUIRE)) {
            return 1;
        }
        (void)atomic_fetch_add_explicit(&list->cas_failures, UINT64_C(1),
                                        UHM_ATOMIC_RELAXED);
    }
}

static inline uhm_tagged_freelist_node *uhm_tagged_freelist_pop(
    uhm_tagged_freelist *list) {
    uintptr_t old_head;
    uintptr_t new_head;
    uint32_t tag;
    uhm_tagged_freelist_node *node;
    uhm_tagged_freelist_node *next;

    if (!list || !uhm_tagged_freelist_is_supported()) {
        return 0;
    }
    for (;;) {
        old_head = atomic_load_explicit(&list->head, UHM_ATOMIC_ACQUIRE);
        node = uhm_tagged_freelist_unpack_ptr(old_head);
        if (!node) {
            return 0;
        }
        tag = uhm_tagged_freelist_unpack_tag(old_head);
        if (tag == UHM_TAGGED_FREELIST_TAG_MASK) {
            uhm_tagged_freelist_wrap_tag(list);
            continue;
        }
        next = uhm_tagged_freelist_next_load(node);
        new_head = uhm_tagged_freelist_pack(next, tag + 1u);
        if (atomic_compare_exchange_weak_explicit(
                &list->head, &old_head, new_head, UHM_ATOMIC_ACQ_REL,
                UHM_ATOMIC_ACQUIRE)) {
            uhm_tagged_freelist_next_store(node, 0);
            return node;
        }
        (void)atomic_fetch_add_explicit(&list->cas_failures, UINT64_C(1),
                                        UHM_ATOMIC_RELAXED);
    }
}

static inline uhm_tagged_freelist_node *uhm_tagged_freelist_take_all(
    uhm_tagged_freelist *list) {
    uintptr_t old_head;
    uintptr_t new_head;
    uint32_t tag;
    uhm_tagged_freelist_node *node;

    if (!list || !uhm_tagged_freelist_is_supported()) {
        return 0;
    }
    for (;;) {
        old_head = atomic_load_explicit(&list->head, UHM_ATOMIC_ACQUIRE);
        node = uhm_tagged_freelist_unpack_ptr(old_head);
        if (!node) {
            return 0;
        }
        tag = uhm_tagged_freelist_unpack_tag(old_head);
        if (tag == UHM_TAGGED_FREELIST_TAG_MASK) {
            uhm_tagged_freelist_wrap_tag(list);
            continue;
        }
        new_head = uhm_tagged_freelist_pack(0, tag + 1u);
        if (atomic_compare_exchange_weak_explicit(
                &list->head, &old_head, new_head, UHM_ATOMIC_ACQ_REL,
                UHM_ATOMIC_ACQUIRE)) {
            return node;
        }
        (void)atomic_fetch_add_explicit(&list->cas_failures, UINT64_C(1),
                                        UHM_ATOMIC_RELAXED);
    }
}

static inline uint64_t uhm_tagged_freelist_cas_failures(
    const uhm_tagged_freelist *list) {
    return list ? atomic_load_explicit(&list->cas_failures,
                                       UHM_ATOMIC_RELAXED)
                : UINT64_C(0);
}

static inline uint64_t uhm_tagged_freelist_tag_overflows(
    const uhm_tagged_freelist *list) {
    return list ? atomic_load_explicit(&list->tag_overflows,
                                       UHM_ATOMIC_RELAXED)
                : UINT64_C(0);
}

static inline uint64_t uhm_tagged_freelist_tag_wrap_deferrals(
    const uhm_tagged_freelist *list) {
    return list ? atomic_load_explicit(&list->tag_wrap_deferrals,
                                       UHM_ATOMIC_RELAXED)
                : UINT64_C(0);
}

static inline uint64_t uhm_tagged_freelist_last_wrap_epoch(
    const uhm_tagged_freelist *list) {
    return list ? atomic_load_explicit(&list->last_wrap_epoch,
                                       UHM_ATOMIC_ACQUIRE)
                : UINT64_C(0);
}

typedef struct uhm_vm_bound_ops {
    void *user;
    size_t page_size;
    size_t allocation_granularity;
    uint64_t capabilities;

    uhm_result (UHM_CALL *reserve)(void *user, void **addr, size_t size, size_t alignment);
    uhm_result (UHM_CALL *commit)(void *user, void *addr, size_t size, uint32_t prot);
    uhm_result (UHM_CALL *decommit)(void *user, void *addr, size_t size);
    uhm_result (UHM_CALL *release)(void *user, void *addr, size_t size);
    uhm_result (UHM_CALL *protect)(void *user, void *addr, size_t size, uint32_t prot);
    uhm_result (UHM_CALL *fixed_remap)(void *user, void *addr, size_t size, uint32_t prot);
    uhm_result (UHM_CALL *numa_hint)(void *user, void *addr, size_t size,
                                     uint32_t policy, int32_t node,
                                     uint32_t flags);
} uhm_vm_bound_ops;

typedef struct uhm_diag_ops {
    uint32_t reserved;
} uhm_diag_ops;

typedef struct uhm_heap_F {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;
    uint64_t capabilities;

    uhm_api api;
    uhm_vm_bound_ops vm;
    uhm_diag_ops diag;

    uint64_t canary;
} uhm_heap_F;

typedef struct uhm_extent_descriptor {
    uint32_t magic;
    uint32_t kind;
    uintptr_t addr;
    size_t size;
    uint32_t owner;
    uint32_t va_state;
    uint32_t commit_state;
    uint32_t prot;
    uint64_t id;
    uint64_t generation;
    void *object;
} uhm_extent_descriptor;

typedef struct uhm_block {
    uint32_t magic;
    uint32_t flags;
    uint16_t size_class;
    uint16_t reserved_size_class;
    size_t total_size;
    size_t requested_size;
    struct uhm_block *prev_phys;
    struct uhm_block *next_phys;
    struct uhm_block *prev_free;
    struct uhm_block *next_free;
    struct uhm_block *free_tree_parent;
    struct uhm_block *free_tree_left;
    struct uhm_block *free_tree_right;
    struct uhm_block *live_tree_parent;
    struct uhm_block *live_tree_left;
    struct uhm_block *live_tree_right;
    uint64_t free_tree_priority;
    uint64_t live_tree_priority;
    uint64_t generation;
    uhm_extent_descriptor desc;
} uhm_block;

typedef struct uhm_slab uhm_slab;
typedef struct uhm_tls_cache uhm_tls_cache;

typedef struct uhm_slab_slot {
    uint32_t magic;
    uint32_t flags;
    uint16_t size_class;
    uint16_t state;
    uint32_t requested_size;
    _Atomic(uint64_t) state_word;
    uhm_slab *slab;
    uhm_tagged_freelist_node remote_link;
    struct uhm_slab_slot *next_free;
} uhm_slab_slot;

struct uhm_slab {
    uint32_t magic;
    uint32_t flags;
    uint16_t size_class;
    uint16_t state;
    uintptr_t base;
    size_t size;
    size_t slot_size;
    size_t slot_stride;
    size_t total_slots;
    size_t free_slots;
    size_t live_slots;
    _Atomic(size_t) ctx_cached_slots;
    UHM_CACHELINE_ALIGN _Atomic(uint64_t) summary_seq;
    UHM_CACHELINE_ALIGN _Atomic(uint64_t) raw_publish_seq;
    UHM_CACHELINE_ALIGN _Atomic(uint64_t) state_transition_seq;
    UHM_CACHELINE_ALIGN uhm_spinlock raw_lock;
    UHM_CACHELINE_ALIGN _Atomic(size_t) live_slots_count;
    _Atomic(size_t) free_slots_count;
    _Atomic(uintptr_t) free_head_ref;
    _Atomic(uint32_t) state_mirror;
    UHM_CACHELINE_ALIGN _Atomic(uint64_t) remote_counter_seq;
    _Atomic(size_t) remote_free_slots;
    _Atomic(size_t) remote_free_pending;
    _Atomic(size_t) remote_enqueue_inflight_count;
    _Atomic(size_t) remote_enqueue_inflight;
    UHM_CACHELINE_ALIGN _Atomic(uint32_t) remote_drain_active;
    _Atomic(uint64_t) remote_drain_epoch;
    UHM_CACHELINE_ALIGN _Atomic(uint32_t) remote_ready_queued;
    UHM_CACHELINE_ALIGN uhm_slab_slot *free_head;
    uhm_slab_slot *remote_free_head;
    uhm_tagged_freelist remote_free_lf;
    uhm_tagged_freelist_node remote_ready_link;
    uhm_slab *next_class;
    uhm_slab *prev_class;
    uhm_slab *next_all;
    uhm_slab *prev_all;
    uint64_t generation;
    uhm_extent_descriptor desc;
};

_Static_assert(alignof(uhm_slab) >= UHM_CACHELINE_SIZE,
               "slab alignment must be cache-line sized");
_Static_assert(offsetof(uhm_slab, summary_seq) % UHM_CACHELINE_SIZE == 0u,
               "slab summary seq must start on a cache line");
_Static_assert(offsetof(uhm_slab, raw_publish_seq) % UHM_CACHELINE_SIZE == 0u,
               "slab raw publish seq must start on a cache line");
_Static_assert(offsetof(uhm_slab, state_transition_seq) %
                       UHM_CACHELINE_SIZE ==
                   0u,
               "slab state transition seq must start on a cache line");
_Static_assert(offsetof(uhm_slab, raw_lock) % UHM_CACHELINE_SIZE == 0u,
               "slab raw lock must start on a cache line");
_Static_assert(offsetof(uhm_slab, live_slots_count) % UHM_CACHELINE_SIZE == 0u,
               "slab raw mirror counters must start on a cache line");
_Static_assert(offsetof(uhm_slab, remote_counter_seq) %
                       UHM_CACHELINE_SIZE ==
                   0u,
               "slab remote counters must start on a cache line");
_Static_assert(offsetof(uhm_slab, remote_drain_active) %
                       UHM_CACHELINE_SIZE ==
                   0u,
               "slab remote drain owner must start on a cache line");
_Static_assert(offsetof(uhm_slab, remote_ready_queued) %
                       UHM_CACHELINE_SIZE ==
                   0u,
               "slab remote ready owner must start on a cache line");
_Static_assert(offsetof(uhm_slab, free_head) % UHM_CACHELINE_SIZE == 0u,
               "slab pointer/list fields must start on a cache line");

typedef struct uhm_vma {
    uint32_t magic;
    uint32_t flags;
    uintptr_t addr;
    size_t size;
    uint32_t prot;
    uint64_t generation;
    uint64_t retire_epoch;
    struct uhm_vma *prev;
    struct uhm_vma *next;
    struct uhm_vma *tree_parent;
    struct uhm_vma *tree_left;
    struct uhm_vma *tree_right;
    uint64_t tree_priority;
    uhm_extent_descriptor desc;
} uhm_vma;

typedef struct uhm_free_extent {
    uint32_t magic;
    uint32_t flags;
    uintptr_t addr;
    size_t size;
    uint64_t retire_epoch;
    struct uhm_free_extent *prev;
    struct uhm_free_extent *next;
    struct uhm_free_extent *tree_parent;
    struct uhm_free_extent *tree_left;
    struct uhm_free_extent *tree_right;
    size_t tree_max_size;
    uint64_t tree_priority;
    uhm_extent_descriptor desc;
} uhm_free_extent;

typedef struct uhm_control_extent {
    uint32_t magic;
    uint32_t flags;
    uintptr_t base;
    uintptr_t end;
    uintptr_t alloc;
    struct uhm_control_extent *next;
    uhm_extent_descriptor desc;
} uhm_control_extent;

typedef struct uhm_range_claim {
    uint32_t source;
    uint32_t flags;
    uintptr_t addr;
    size_t size;
    uintptr_t old_malloc_hi;
    uintptr_t old_mmap_lo;
} uhm_range_claim;

typedef struct uhm_tx {
    uhm_heap *heap;
    uint32_t op;
    uint32_t stage;
    uint32_t stage_error;
    uhm_result result;

    uintptr_t addr;
    size_t size;
    uint32_t flags;
    uint32_t old_prot;
    uint32_t new_prot;
    uint32_t old_commit_state;
    uint32_t new_commit_state;

    uint64_t begin_generation;
    uint64_t publish_generation;
    uintptr_t old_mmap_lo;

    uint32_t range_claimed;
    uint32_t platform_done;
    uint32_t published;
    uint32_t rolled_back;

    uhm_vma *old_vma;
    uhm_vma *left_vma;
    uhm_vma *right_vma;
    uhm_free_extent *free_node;
} uhm_tx;

typedef struct uhm_lockset {
    uint32_t highest_rank;
    uint32_t violation;
    size_t page_shards[UHM_PAGE_MAP_SHARDS];
    size_t page_shard_count;
    size_t vma_shards[UHM_VMA_SHARDS];
    size_t vma_shard_count;
    size_t extent_shards[UHM_EXTENT_SHARDS];
    size_t extent_shard_count;
    size_t slab_shards[UHM_SLAB_SHARDS];
    size_t slab_shard_count;
} uhm_lockset;

typedef struct uhm_page_record {
    uint32_t owner;
    uint32_t va_state;
    uint32_t commit_state;
    uint32_t prot;
    uint64_t descriptor_id;
    uint64_t generation;
    uint32_t pin_count;
    uint32_t flags;
} uhm_page_record;

#define UHM_LOCAL_PAGE_HINT_COUNT 32u
#define UHM_LOCAL_PAGE_HINT_WAYS 2u
#define UHM_LOCAL_PAGE_HINT_SET_COUNT \
    (UHM_LOCAL_PAGE_HINT_COUNT / UHM_LOCAL_PAGE_HINT_WAYS)

_Static_assert((UHM_LOCAL_PAGE_HINT_COUNT &
                (UHM_LOCAL_PAGE_HINT_COUNT - 1u)) == 0u,
               "local page hint count must be a power of two");
_Static_assert(UHM_LOCAL_PAGE_HINT_COUNT % UHM_LOCAL_PAGE_HINT_WAYS == 0u,
               "local page hint ways must divide the entry count");
_Static_assert((UHM_LOCAL_PAGE_HINT_SET_COUNT &
                (UHM_LOCAL_PAGE_HINT_SET_COUNT - 1u)) == 0u,
               "local page hint set count must be a power of two");

typedef struct uhm_local_page_hint {
    uhm_slab_slot *anchor;
    uhm_page_record *record;
} uhm_local_page_hint;

#ifndef UHMAS_LOCAL_RETAINED_PREPARED_PROOF
#define UHMAS_LOCAL_RETAINED_PREPARED_PROOF 1
#endif

#define UHM_LOCAL_RETAINED_HINT_COUNT 8u
#define UHM_LOCAL_RETAINED_HINT_WAYS 1u
#define UHM_LOCAL_RETAINED_HINT_SET_COUNT \
    (UHM_LOCAL_RETAINED_HINT_COUNT / UHM_LOCAL_RETAINED_HINT_WAYS)
#define UHM_LOCAL_RETAINED_PAGE_FACTOR 16u
#define UHM_LOCAL_CACHE_IDLE_MISS_THRESHOLD 256u
#define UHM_LOCAL_CACHE_IDLE_STREAK_MASK 0xffu
#define UHM_LOCAL_CACHE_IDLE_HIT_TAG_SHIFT 8u
#define UHM_LOCAL_CACHE_IDLE_HIT_TAG_MASK 0x00ffffffu

_Static_assert((UHM_LOCAL_RETAINED_HINT_COUNT &
                (UHM_LOCAL_RETAINED_HINT_COUNT - 1u)) == 0u,
               "local retained hint count must be a power of two");
_Static_assert((UHM_LOCAL_RETAINED_HINT_WAYS &
                (UHM_LOCAL_RETAINED_HINT_WAYS - 1u)) == 0u,
               "local retained hint ways must be a power of two");
_Static_assert(UHM_LOCAL_RETAINED_HINT_COUNT %
                       UHM_LOCAL_RETAINED_HINT_WAYS ==
                   0u,
               "local retained hint count must contain complete sets");
_Static_assert((UHM_LOCAL_RETAINED_HINT_SET_COUNT &
                (UHM_LOCAL_RETAINED_HINT_SET_COUNT - 1u)) == 0u,
               "local retained hint set count must be a power of two");

typedef struct uhm_local_retained_hint {
    uhm_slab *slab;
    size_t slot_count;
} uhm_local_retained_hint;

typedef struct uhm_page_leaf {
    size_t base_index;
    struct uhm_page_leaf *next;
    uhm_page_record records[UHM_PAGE_LEAF_PAGES];
} uhm_page_leaf;

typedef struct uhm_metadata_reader_hot_fields {
    _Atomic(size_t) depth;
    _Atomic(uint64_t) epoch;
    _Atomic(uintptr_t) owner_heap;
    _Atomic(uint32_t) registered;
    _Atomic(uint32_t) local_update_seq;
} uhm_metadata_reader_hot_fields;

typedef struct uhm_metadata_reader_hazard_fields {
    UHM_CACHELINE_ALIGN _Atomic(uintptr_t) hazards[UHM_METADATA_HAZARD_SLOTS];
} uhm_metadata_reader_hazard_fields;

typedef struct uhm_metadata_reader_state {
    _Atomic(size_t) depth;
    _Atomic(uint64_t) epoch;
    _Atomic(uintptr_t) owner_heap;
    _Atomic(uint32_t) registered;
    _Atomic(uint32_t) local_update_seq;
    unsigned char hot_padding[UHM_CACHELINE_PAD_FOR(uhm_metadata_reader_hot_fields)];
    UHM_CACHELINE_ALIGN _Atomic(uintptr_t) hazards[UHM_METADATA_HAZARD_SLOTS];
    unsigned char hazard_padding[UHM_CACHELINE_PAD_FOR(uhm_metadata_reader_hazard_fields)];
} uhm_metadata_reader_state;

_Static_assert(offsetof(uhm_metadata_reader_state, hazards) %
                       UHM_CACHELINE_SIZE ==
                   0u,
               "metadata reader hazards must start on a cache line");
_Static_assert(sizeof(uhm_metadata_reader_state) % UHM_CACHELINE_SIZE == 0u,
               "metadata reader state must be cache-line sized");

struct uhm_tls_cache {
    uint32_t magic;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;
    atomic_uint lifecycle_state;
    UHM_CACHELINE_ALIGN uhm_metadata_reader_state metadata_reader;

    uhm_heap *owner;
    _Atomic(uintptr_t) owner_ref;
    uint64_t owner_thread_id;
    size_t local_cache_limit;
    size_t local_cache_bytes;
    size_t local_cache_count;
    size_t retained_slab_pages;
    size_t retained_slab_page_limit;
    _Atomic(size_t) cached_requested_bytes;
    size_t adaptive_pages;
    atomic_uint idle_miss_streak;
    uint32_t nonempty_class_mask;
    uint64_t hit_count;
    uint64_t miss_count;
    uint64_t push_count;
    uint64_t overflow_count;
    uint64_t retained_page_pressure_count;
    uhm_slab_slot *local_cache_head;
    uhm_slab_slot *local_cache_heads[UHM_SIZE_CLASS_COUNT];
    uhm_local_page_hint local_page_hints[UHM_LOCAL_PAGE_HINT_COUNT];
    uhm_local_retained_hint
        local_retained_hints[UHM_LOCAL_RETAINED_HINT_COUNT];
    uhm_tls_cache *prev_active;
    uhm_tls_cache *next_active;
    uhm_tls_cache *next_free;
};

_Static_assert(alignof(uhm_tls_cache) >= UHM_CACHELINE_SIZE,
               "TLS cache slot alignment must be cache-line sized");
_Static_assert(sizeof(uhm_tls_cache) % UHM_CACHELINE_SIZE == 0u,
               "TLS cache slot stride must be cache-line sized");

typedef struct uhm_slab_ptr_cell {
    _Atomic(uhm_slab *) value;
    unsigned char padding[UHM_CACHELINE_PAD_FOR(_Atomic(uhm_slab *))];
} uhm_slab_ptr_cell;

_Static_assert(sizeof(uhm_slab_ptr_cell) % UHM_CACHELINE_SIZE == 0u,
               "slab pointer cell size must be cache-line sized");

typedef struct uhm_heap_core {
    uintptr_t arena_base;
    uintptr_t arena_end;
    size_t arena_size;
    uint32_t owns_arena;

    uintptr_t control_base;
    uintptr_t control_end;
    uintptr_t control_alloc;
    size_t control_bytes;
    uhm_extent_descriptor primary_control_desc;

    uintptr_t malloc_hi;
    uintptr_t mmap_lo;
    uint32_t low_watermark_claim_inflight;
    uintptr_t low_watermark_claim_addr;
    size_t low_watermark_claim_size;

    uhm_block *first_block;
    uhm_block *last_block;
    uhm_block *malloc_free_head;
    uhm_block *malloc_free_tree_root;
    uhm_block *live_block_tree_root;
    uhm_slab *slab_class_heads[UHM_SIZE_CLASS_COUNT];
    uhm_slab *slab_full_heads[UHM_SIZE_CLASS_COUNT];
    uhm_slab *slab_empty_heads[UHM_SIZE_CLASS_COUNT];
    UHM_CACHELINE_ALIGN uhm_tagged_freelist_cell slab_remote_class_lf[UHM_SIZE_CLASS_COUNT];
    UHM_CACHELINE_ALIGN uhm_tagged_freelist_cell slab_remote_ready_lf[UHM_SIZE_CLASS_COUNT];
    UHM_CACHELINE_ALIGN uhm_atomic_u64_cell slab_remote_ready_seq[UHM_SIZE_CLASS_COUNT];
#if UINTPTR_MAX == UINT32_MAX
    /*
     * 32-bit targets cannot pack a tagged pointer.  Keep a per-class raw
     * ready directory so the low-lock path does not scan slab_all_head.
     * Each head is protected by the matching slab_locks[class] shard.
     */
    UHM_CACHELINE_ALIGN uhm_slab_ptr_cell
        slab_remote_ready_raw[UHM_SIZE_CLASS_COUNT];
#endif
    /*
     * A set bit means that remote work may not be reachable through the
     * lockless ready queue and therefore requires the bounded slab-list
     * recovery scan for that class. Keep this separate from ready-queue
     * state so a queued slab remains a fast-path concern.
     */
    UHM_CACHELINE_ALIGN uhm_atomic_u32_cell
        slab_remote_pending_masks[UHM_SLAB_REMOTE_PENDING_MASK_WORD_COUNT];
    UHM_CACHELINE_ALIGN uhm_metadata_reader_state lockless_remote_readers[UHM_LOCKLESS_REMOTE_READER_SLOTS];
    size_t slab_empty_count[UHM_SIZE_CLASS_COUNT];
    size_t slab_committed_empty_count[UHM_SIZE_CLASS_COUNT];
    size_t slab_empty_committed_cache_high_water[UHM_SIZE_CLASS_COUNT];
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t slab_empty_decommit_pressure_events;
    uint64_t slab_empty_decommit_pressure_bytes;
    uint64_t slab_empty_committed_cache_adaptive_grow_events;
    uint64_t slab_empty_committed_cache_adaptive_grow_bytes;
#endif
#if UHMAS_ENABLE_TEST_HOOKS
    uint64_t slab_remote_class_scan_calls;
    uint64_t slab_remote_class_scan_slabs;
#endif
    uhm_slab *slab_all_head;

    uhm_vma *vma_head;
    uhm_vma *vma_tree_root;
    uhm_free_extent *mmap_free_head;
    uhm_free_extent *mmap_free_tree_root;
    uhm_free_extent *mmap_recent_free_extent;
    uhm_free_extent *mmap_recent_free_cache[UHM_MMAP_RECENT_FREE_CACHE_SLOTS];
    size_t mmap_recent_free_next;
    uhm_free_extent
        *mmap_size_hint[UHM_MMAP_SIZE_HINT_BUCKETS][UHM_MMAP_SIZE_HINT_SLOTS];
    size_t mmap_size_hint_bucket_max[UHM_MMAP_SIZE_HINT_BUCKETS];
    uhm_free_extent *mmap_size_hint_bucket_max_extent[
        UHM_MMAP_SIZE_HINT_BUCKETS];
    uhm_control_extent *control_extents;
    uhm_control_extent *control_recent_extent;
    size_t control_extent_count;
    size_t control_no_fit_extent_count;
    size_t control_no_fit_size;
    size_t control_no_fit_alignment;
    uhm_control_extent
        *control_alloc_hint[UHM_CONTROL_ALLOC_HINT_BUCKETS]
                           [UHM_CONTROL_ALLOC_HINT_SLOTS];
    uhm_thread_ctx *thread_ctx_head;
    uhm_tls_cache *tls_cache_head;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t tls_cache_hits;
    uint64_t tls_cache_misses;
    uint64_t tls_cache_pushes;
    uint64_t tls_cache_overflows;
    uint64_t tls_cache_retained_page_pressure;
#endif
    uhm_vma *emergency_vma_head;
    uhm_free_extent *emergency_free_extent_head;
    size_t emergency_vma_available;
    size_t emergency_free_extent_available;
    uhm_vma *retired_vma_head;
    uhm_vma *retired_vma_tail;
    uhm_free_extent *retired_free_extent_head;
    uhm_free_extent *retired_free_extent_tail;
    uhm_vma *retired_emergency_vma_head;
    uhm_vma *retired_emergency_vma_tail;
    uhm_free_extent *retired_emergency_free_extent_head;
    uhm_free_extent *retired_emergency_free_extent_tail;
    size_t retired_emergency_vma_count;
    size_t retired_emergency_free_extent_count;
    size_t retired_vma_count;
    size_t retired_free_extent_count;
    uhm_vma *hot_vma_head;
    uhm_free_extent *hot_free_extent_head;
    size_t hot_vma_count;
    size_t hot_free_extent_count;
    uint32_t metadata_hot_vma_refill_deferred;
    uint32_t metadata_hot_free_extent_refill_deferred;
    uint64_t metadata_hot_vma_hits;
    uint64_t metadata_hot_vma_misses;
    uint64_t metadata_hot_vma_reclaimed;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t metadata_hot_vma_full_defers;
    uint32_t metadata_hot_vma_full_defer_latched;
#endif
    uint64_t metadata_hot_free_extent_hits;
    uint64_t metadata_hot_free_extent_misses;
    uint64_t metadata_hot_free_extent_reclaimed;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t metadata_hot_free_extent_full_defers;
    uint32_t metadata_hot_free_extent_full_defer_latched;
#endif
    uint64_t metadata_quarantine_drain_calls;
    uint64_t metadata_quarantine_drain_scanned;
    uint64_t metadata_quarantine_drain_reclaimed;
    uint64_t metadata_quarantine_drain_budget_exhausted;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t metadata_quarantine_drain_rotated;
#endif
    uint32_t metadata_quarantine_drain_next_list;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t control_alloc_recent_hits;
    uint64_t control_alloc_hint_hits;
    uint64_t control_alloc_hint_misses;
    uint64_t control_alloc_hint_stale;
    uint64_t control_alloc_no_fit_fast_paths;
    uint64_t control_alloc_scan_calls;
    uint64_t control_alloc_scan_steps;
    uint64_t control_alloc_scan_max_steps;
    uint64_t control_alloc_scan_budget_exceeded;
#endif

    size_t page_count;
    UHM_CACHELINE_ALIGN uhm_spinlock_cell page_map_locks[UHM_PAGE_MAP_SHARDS];
    UHM_CACHELINE_ALIGN uhm_spinlock_cell vma_locks[UHM_VMA_SHARDS];
    UHM_CACHELINE_ALIGN uhm_spinlock_cell extent_locks[UHM_EXTENT_SHARDS];
    UHM_CACHELINE_ALIGN uhm_spinlock_cell slab_locks[UHM_SLAB_SHARDS];
    uhm_page_leaf *page_map[UHM_PAGE_MAP_SHARDS];
    uhm_page_leaf *page_map_spares[UHM_PAGE_MAP_SHARDS];
    size_t page_map_spare_count[UHM_PAGE_MAP_SHARDS];

    uint64_t treap_priority_salt;
    uint64_t treap_health_flags;
    _Atomic(uint32_t) treap_repair_pending_flags;
    uint64_t treap_repair_rebuilds;
    uint64_t treap_repair_rehashes;
    uint64_t treap_repair_nodes;
    uint64_t treap_repair_failures;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t treap_priority_mix_calls;
    uint64_t treap_priority_mix_ns;
    uint64_t treap_priority_mix_max_ns;
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_fast_hits;
    uint64_t live_block_slow_calls;
    uint64_t live_block_slow_hits;
    uint64_t live_block_slow_misses;
    uint64_t live_block_slow_scanned_nodes;
    uint64_t live_block_slow_max_scanned_nodes;
    uint64_t live_block_slow_ns;
    uint64_t live_block_slow_max_ns;
    uint64_t live_block_fast_fail_invalid;
    uint64_t live_block_fast_fail_range;
    uint64_t live_block_fast_fail_header_page;
    uint64_t live_block_fast_fail_payload_page;
    uint64_t live_block_fast_fail_descriptor;
    uint64_t live_block_fast_terminal_misses;
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_tree_insertions;
    uint64_t live_block_tree_rotations;
    uint64_t live_block_tree_priority_ties;
    uint64_t live_block_tree_priority_bucket_mask;
    uint64_t live_block_tree_priority_bucket_max;
    uint64_t live_block_tree_priority_buckets[UHM_TREE_PRIORITY_BUCKETS];
    uint64_t live_block_tree_priority_mix_calls;
    uint64_t live_block_tree_priority_mix_ns;
    uint64_t live_block_tree_priority_mix_max_ns;
    uint64_t live_block_tree_max_insert_depth;
#endif
    uint64_t live_block_tree_current_nodes;
    uint64_t live_block_tree_max_nodes;
    uint64_t live_block_tree_depth_limit_exceeded;
    uint64_t live_block_tree_insert_budget_exceeded;
    uint64_t live_block_tree_insert_rotation_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_tree_find_calls;
    uint64_t live_block_tree_find_steps;
    uint64_t live_block_tree_max_find_steps;
#endif
    uint64_t live_block_tree_find_budget_exceeded;
    uint64_t live_block_tree_mutation_epoch;
    uint64_t live_block_health_flags;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_tree_remove_calls;
    uint64_t live_block_tree_remove_sink_steps;
    uint64_t live_block_tree_max_remove_sink_steps;
#endif
    uint64_t live_block_tree_remove_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_slow_tree_hits;
    uint64_t live_block_slow_tree_misses;
    uint64_t live_block_slow_fallback_scans;
    uint64_t live_block_slow_fallback_scanned_nodes;
    uint64_t live_block_slow_fallback_max_scanned_nodes;
    uint64_t live_block_slow_fallback_budget_exceeded;
    uint64_t live_block_slow_fallback_aborts;
#endif
    uintptr_t live_block_slow_fallback_negative_payloads[UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS];
    uint64_t live_block_slow_fallback_negative_epochs[UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS];
    size_t live_block_slow_fallback_negative_next;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_slow_fallback_negative_stores;
    uint64_t live_block_slow_fallback_negative_hits;
    uint64_t live_block_slow_fallback_negative_evictions;
#endif
    uint64_t live_block_slow_fallback_throttle_epoch;
    uint64_t live_block_slow_fallback_epoch_scans;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_slow_fallback_throttles;
#endif
    uint64_t live_block_slow_fallback_scenario_epoch;
    uint32_t live_block_slow_fallback_scenario_throttled_mask;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_slow_fallback_scenario_checks;
    uint64_t live_block_slow_fallback_scenario_throttles;
#endif
    uint64_t live_block_slow_fallback_scenario_calls
        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_slow_fallback_scenario_candidates
        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_fast_fail_reasons
        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT]
        [UHM_LIVE_BLOCK_FAST_FAIL_REASON_COUNT];
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t malloc_free_tree_insertions;
    uint64_t malloc_free_tree_rotations;
    uint64_t malloc_free_tree_priority_ties;
    uint64_t malloc_free_tree_priority_bucket_mask;
    uint64_t malloc_free_tree_priority_bucket_max;
    uint64_t malloc_free_tree_priority_buckets[UHM_TREE_PRIORITY_BUCKETS];
    uint64_t malloc_free_tree_priority_mix_calls;
    uint64_t malloc_free_tree_priority_mix_ns;
    uint64_t malloc_free_tree_priority_mix_max_ns;
    uint64_t malloc_free_tree_max_insert_depth;
#endif
    uint64_t malloc_free_tree_current_nodes;
    uint64_t malloc_free_tree_max_nodes;
    uint64_t malloc_free_tree_depth_limit_exceeded;
    uint64_t malloc_free_tree_insert_budget_exceeded;
    uint64_t malloc_free_tree_insert_rotation_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t malloc_free_tree_find_calls;
    uint64_t malloc_free_tree_find_steps;
    uint64_t malloc_free_tree_max_find_steps;
#endif
    uint64_t malloc_free_tree_find_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t malloc_free_tree_remove_calls;
    uint64_t malloc_free_tree_remove_sink_steps;
    uint64_t malloc_free_tree_max_remove_sink_steps;
#endif
    uint64_t malloc_free_tree_remove_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t vma_tree_insertions;
    uint64_t vma_tree_rotations;
    uint64_t vma_tree_priority_ties;
    uint64_t vma_tree_priority_bucket_mask;
    uint64_t vma_tree_priority_bucket_max;
    uint64_t vma_tree_priority_buckets[UHM_TREE_PRIORITY_BUCKETS];
    uint64_t vma_tree_priority_mix_calls;
    uint64_t vma_tree_priority_mix_ns;
    uint64_t vma_tree_priority_mix_max_ns;
    uint64_t vma_tree_max_insert_depth;
#endif
    uint64_t vma_tree_current_nodes;
    uint64_t vma_tree_max_nodes;
    uint64_t vma_tree_depth_limit_exceeded;
    uint64_t vma_tree_insert_budget_exceeded;
    uint64_t vma_tree_insert_rotation_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t vma_tree_find_calls;
    uint64_t vma_tree_find_steps;
    uint64_t vma_tree_max_find_steps;
#endif
    uint64_t vma_tree_find_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t vma_tree_remove_calls;
    uint64_t vma_tree_remove_sink_steps;
    uint64_t vma_tree_max_remove_sink_steps;
#endif
    uint64_t vma_tree_remove_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t mmap_free_tree_insertions;
    uint64_t mmap_free_tree_rotations;
    uint64_t mmap_free_tree_priority_ties;
    uint64_t mmap_free_tree_priority_bucket_mask;
    uint64_t mmap_free_tree_priority_bucket_max;
    uint64_t mmap_free_tree_priority_buckets[UHM_TREE_PRIORITY_BUCKETS];
    uint64_t mmap_free_tree_priority_mix_calls;
    uint64_t mmap_free_tree_priority_mix_ns;
    uint64_t mmap_free_tree_priority_mix_max_ns;
    uint64_t mmap_free_tree_max_insert_depth;
#endif
    uint64_t mmap_free_tree_current_nodes;
    uint64_t mmap_free_tree_max_nodes;
    uint64_t mmap_free_tree_depth_limit_exceeded;
    uint64_t mmap_free_tree_insert_budget_exceeded;
    uint64_t mmap_free_tree_insert_rotation_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t mmap_free_tree_find_calls;
    uint64_t mmap_free_tree_find_steps;
    uint64_t mmap_free_tree_max_find_steps;
#endif
    uint64_t mmap_free_tree_find_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t mmap_free_tree_remove_calls;
    uint64_t mmap_free_tree_remove_sink_steps;
    uint64_t mmap_free_tree_max_remove_sink_steps;
#endif
    uint64_t mmap_free_tree_remove_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t mmap_recent_free_attempts;
    uint64_t mmap_recent_free_hits;
    uint64_t mmap_recent_free_misses;
    uint64_t mmap_recent_free_too_small;
    uint64_t mmap_recent_free_best_fit_replacements;
    uint64_t mmap_size_hint_attempts;
    uint64_t mmap_size_hint_hits;
    uint64_t mmap_size_hint_misses;
    uint64_t mmap_size_hint_stale;
    uint64_t mmap_size_hint_too_small;
    uint64_t mmap_size_hint_best_fit_replacements;
#endif
#if UHMAS_ENABLE_TEST_HOOKS
    uint64_t mmap_size_hint_direct_exact_hits;
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_counter_u64 page_span_calls;
    uhm_diag_counter_u64 page_span_pages;
    uhm_diag_counter_u64 page_mark_calls;
    uhm_diag_counter_u64 page_mark_pages;
    uhm_diag_counter_u64 page_mark_max_pages;
    uhm_diag_counter_u64 page_mark_budget_exceeded;
    uhm_diag_counter_u64 page_mark_leaf_spans;
    uhm_diag_counter_u64 page_mark_max_leaf_spans;
    uhm_diag_counter_u64 page_mark_phase_calls[UHM_PAGE_MARK_PHASE_COUNT];
    uhm_diag_counter_u64 page_mark_phase_pages[UHM_PAGE_MARK_PHASE_COUNT];
    uhm_diag_counter_u64 page_mark_phase_budget_exceeded[UHM_PAGE_MARK_PHASE_COUNT];
    uhm_diag_counter_u64 page_range_query_calls;
    uhm_diag_counter_u64 page_range_query_pages;
    uhm_diag_counter_u64 page_range_query_max_pages;
    uhm_diag_counter_u64 page_range_query_budget_exceeded;
    uhm_diag_counter_u64 page_range_query_leaf_spans;
    uhm_diag_counter_u64 page_range_query_max_leaf_spans;
    uhm_diag_counter_u64 page_range_query_phase_calls[UHM_PAGE_QUERY_PHASE_COUNT];
    uhm_diag_counter_u64 page_range_query_phase_pages[UHM_PAGE_QUERY_PHASE_COUNT];
    uhm_diag_counter_u64 page_range_query_phase_budget_exceeded[UHM_PAGE_QUERY_PHASE_COUNT];
    uhm_diag_counter_u64 page_lock_range_calls;
    uhm_diag_counter_u64 page_lock_range_shards;
    uhm_diag_counter_u64 page_lock_range_max_shards;
    uhm_diag_counter_u64 page_lock_range_budget_exceeded;
    uhm_diag_counter_u64 malloc_free_decommit_commit_all_fast_paths;
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t mprotect_mmap_noop_calls;
    uint64_t mprotect_page_private_noop_calls;
    uint64_t mprotect_page_private_descriptor_hits;
    uint64_t mprotect_page_private_scan_fallbacks;
    uint64_t mprotect_platform_calls;
    uint64_t mprotect_publish_calls;
    uint64_t mprotect_rollback_calls;
    uint64_t mprotect_vma_collect_queries;
    uint64_t mprotect_vma_noop_queries;
    uint64_t mprotect_vma_mark_queries;
    uint64_t mprotect_vma_restore_queries;
    uint64_t mprotect_vma_publish_queries;
    uint64_t mprotect_vma_max_segments;
    uint64_t munmap_vma_collect_queries;
    uint64_t munmap_vma_publish_queries;
    uint64_t munmap_vma_max_segments;
    uint64_t munmap_vma_trim_repair_elided;
    uint64_t munmap_vma_trim_repair_elided_pages;
#endif
    uint64_t decommit_events;
    uint64_t decommit_bytes;
    uint64_t recommit_events;
    uint64_t recommit_bytes;
    uint64_t malloc_free_decommit_deferred_events;
    uint64_t malloc_free_decommit_deferred_bytes;
    size_t malloc_free_decommit_retained_bytes;
    size_t malloc_free_decommit_retention_base_high_water_bytes;
    size_t malloc_free_decommit_retention_high_water_bytes;
    size_t malloc_free_decommit_retention_max_high_water_bytes;
    size_t malloc_free_decommit_retention_idle_drains;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t malloc_free_decommit_retention_pressure_events;
    uint64_t malloc_free_decommit_retention_pressure_bytes;
    uint64_t malloc_free_decommit_retention_adaptive_grow_events;
    uint64_t malloc_free_decommit_retention_adaptive_grow_bytes;
    uint64_t malloc_free_decommit_retention_adaptive_shrink_events;
    uint64_t malloc_free_decommit_retention_adaptive_shrink_bytes;
    uint64_t malloc_free_decommit_hot_deferred_events;
    uint64_t malloc_free_decommit_hot_deferred_bytes;
#endif
    uintptr_t recent_decommit_addrs[UHM_DECOMMIT_RECENT_RANGE_SLOTS];
    size_t recent_decommit_sizes[UHM_DECOMMIT_RECENT_RANGE_SLOTS];
    size_t recent_decommit_next;
    uint64_t decommit_recommit_same_addr_events;
    uint64_t decommit_recommit_same_addr_bytes;

    uint64_t vm_reserve_calls;
    uint64_t vm_reserve_failures;
    uint64_t vm_reserve_ns;
    uint64_t vm_commit_calls;
    uint64_t vm_commit_failures;
    uint64_t vm_commit_ns;
    uint64_t vm_decommit_calls;
    uint64_t vm_decommit_failures;
    uint64_t vm_decommit_ns;
    uint64_t vm_fixed_remap_calls;
    uint64_t vm_fixed_remap_failures;
    uint64_t vm_fixed_remap_ns;
    uint64_t vm_protect_calls;
    uint64_t vm_protect_failures;
    uint64_t vm_protect_ns;
    uint64_t vm_release_calls;
    uint64_t vm_release_failures;
    uint64_t vm_release_ns;
    uint64_t vm_numa_hint_calls;
    uint64_t vm_numa_hint_failures;
    uint64_t vm_numa_hint_ns;
    uint64_t vm_lock_held_calls;
    uint64_t vm_lock_held_failures;
    uint64_t vm_lock_held_ns;
    uint64_t vm_reserve_lock_held_calls;
    uint64_t vm_commit_lock_held_calls;
    uint64_t vm_decommit_lock_held_calls;
    uint64_t vm_fixed_remap_lock_held_calls;
    uint64_t vm_protect_lock_held_calls;
    uint64_t vm_release_lock_held_calls;
    uint64_t vm_numa_hint_lock_held_calls;

#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t close_phase_wall_ns[UHM_CLOSE_PHASE_COUNT];
    uint64_t close_phase_cpu_work[UHM_CLOSE_PHASE_COUNT];
    uint64_t close_phase_calls[UHM_CLOSE_PHASE_COUNT];
    uint32_t close_phase_cpu_work_valid[UHM_CLOSE_PHASE_COUNT];
    uint32_t close_phase_cpu_work_is_cycles[UHM_CLOSE_PHASE_COUNT];
#endif

    _Atomic(uint64_t) generation;
    uint64_t metadata_epoch;
    _Atomic(uint64_t) metadata_published_epoch;
    _Atomic(uint32_t) metadata_reclaim_pending;
#if UHMAS_ENABLE_TEST_HOOKS
    uint64_t fault_control_alloc_countdown;
    uint64_t fault_lockless_remote_publish_countdown;
    uint64_t fault_lockless_remote_alloc_countdown;
    uint64_t fault_lockless_remote_drain_countdown;
    uint64_t fault_lockless_remote_decommit_trylock_countdown;
    uint64_t fault_remote_ready_clear_countdown;
    uint64_t fault_remote_ready_note_countdown;
    uint64_t fault_mmap_free_insert_countdown;
    uint64_t fault_slab_decommit_page_publish_countdown;
    size_t test_slab_empty_decommit_high_water_override;
#endif
    _Atomic(uint32_t) debug_last_corruption_site;
    size_t tls_cache_min_pages;
    size_t tls_cache_max_pages;
    size_t tls_cache_max_retained_pages;
    size_t tls_cache_default_pages;
    size_t tls_cache_slot_count;
    size_t tls_cache_active_count;
    uintptr_t tls_cache_key;
    uint32_t tls_cache_key_valid;
    uint32_t numa_policy;
    int32_t preferred_numa_node;
    size_t malloc_free_decommit_retention_pages;
    uint32_t control_expanding;
    uint64_t control_expanding_owner_thread_id;
    uint32_t spinlock_mode;
    uint32_t spinlock_yield_after_spins;
    /* Internal 32-bit raw-queue remote-free capability; never public lockless. */
    uint32_t low_lock_remote_free_enabled;
} uhm_heap_core;

typedef struct uhm_heap_stats_internal {
    UHM_CACHELINE_ALIGN uhm_atomic_size_cell
        malloc_live_shards[UHM_MALLOC_LIVE_STAT_SHARDS];
    UHM_CACHELINE_ALIGN size_t committed_bytes;
    size_t decommitted_bytes;
    size_t logical_decommitted_bytes;
    size_t physical_released_bytes;
    size_t mmap_live_bytes;
} uhm_heap_stats_internal;

_Static_assert(offsetof(uhm_heap_stats_internal, committed_bytes) %
                       UHM_CACHELINE_SIZE ==
                   0u,
               "heap stats cold fields must start on a cache line");
_Static_assert(sizeof(uhm_heap_stats_internal) % UHM_CACHELINE_SIZE == 0u,
               "heap stats size must be cache-line sized");

struct uhm_heap {
    uint32_t magic;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t state_flags;
    UHM_CACHELINE_ALIGN atomic_uint lifecycle_state;
    UHM_CACHELINE_ALIGN atomic_uint active_ops;
    UHM_CACHELINE_ALIGN uhm_atomic_size_cell
        active_op_shards[UHM_ACTIVE_OP_SHARDS];
    UHM_CACHELINE_ALIGN _Atomic(size_t) metadata_hazard_readers;
    UHM_CACHELINE_ALIGN _Atomic(size_t) metadata_registered_readers;
    UHM_CACHELINE_ALIGN _Atomic(uint64_t) metadata_registered_reader_seq;

    UHM_CACHELINE_ALIGN uhm_heap_F F;

    UHM_CACHELINE_ALIGN uhm_spinlock lock;
#if UHMAS_ENABLE_DIAGNOSTICS
    UHM_CACHELINE_ALIGN atomic_uint diagnostics_timing_enabled;
#endif
    UHM_CACHELINE_ALIGN atomic_int last_error;
    UHM_CACHELINE_ALIGN atomic_uint platform_error;

    UHM_CACHELINE_ALIGN uhm_heap_core core;
    UHM_CACHELINE_ALIGN uhm_heap_stats_internal stats;
    uhm_tls_cache *tls_cache_free_head;
    UHM_CACHELINE_ALIGN uhm_tls_cache tls_cache_slots[UHM_TLS_CACHE_MAX_SLOTS];
};

static inline void uhm_heap_set_diagnostics_timing_enabled(uhm_heap *heap,
                                                           uint32_t enabled) {
#if UHMAS_ENABLE_DIAGNOSTICS
    if (heap) {
        atomic_store_explicit(&heap->diagnostics_timing_enabled,
                              enabled != 0u ? 1u : 0u, UHM_ATOMIC_RELEASE);
    }
#else
    (void)heap;
    (void)enabled;
#endif
}

static inline void uhm_heap_init_diagnostics_timing_enabled(uhm_heap *heap) {
#if UHMAS_ENABLE_DIAGNOSTICS
    if (heap) {
        atomic_init(&heap->diagnostics_timing_enabled, 0u);
    }
#else
    (void)heap;
#endif
}

static inline int uhm_heap_diagnostics_timing_enabled(const uhm_heap *heap) {
#if UHMAS_ENABLE_DIAGNOSTICS
    return heap &&
           atomic_load_explicit(&heap->diagnostics_timing_enabled,
                                UHM_ATOMIC_RELAXED) != 0u;
#else
    (void)heap;
    return 0;
#endif
}

struct uhm_thread_ctx {
    uint32_t magic;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;
    atomic_uint lifecycle_state;
    UHM_CACHELINE_ALIGN uhm_metadata_reader_state metadata_reader;

    uhm_heap *owner;
    uint64_t owner_thread_id;
    uintptr_t storage_base;
    size_t storage_size;
    size_t local_cache_limit;
    size_t local_cache_bytes;
    size_t local_cache_count;
    size_t retained_slab_pages;
    size_t retained_slab_page_limit;
    _Atomic(size_t) cached_requested_bytes;
    atomic_uint idle_miss_streak;
    uint32_t nonempty_class_mask;
    uhm_slab_slot *local_cache_head;
    uhm_slab_slot *local_cache_heads[UHM_SIZE_CLASS_COUNT];
    uhm_local_page_hint local_page_hints[UHM_LOCAL_PAGE_HINT_COUNT];
    uhm_local_retained_hint
        local_retained_hints[UHM_LOCAL_RETAINED_HINT_COUNT];
    size_t deferred_free_count;
    uint64_t deferred_free_epoch;
    void *deferred_free[UHM_THREAD_CTX_DEFERRED_FREE_SLOTS];
    struct uhm_thread_ctx *prev_active;
    struct uhm_thread_ctx *next_active;
    atomic_int last_error;
};

static inline size_t uhm_heap_active_ops_load(const uhm_heap *heap) {
    size_t total;
    size_t i;

    if (!heap) {
        return 0u;
    }
    total = atomic_load_explicit(&heap->active_ops, UHM_ATOMIC_ACQUIRE);
    for (i = 0; i < UHM_ACTIVE_OP_SHARDS; ++i) {
        total += atomic_load_explicit(&heap->active_op_shards[i].value,
                                      UHM_ATOMIC_ACQUIRE);
    }
    return total;
}

static inline uint64_t uhm_heap_generation_load(const uhm_heap *heap) {
    return heap ? atomic_load_explicit(&heap->core.generation,
                                       UHM_ATOMIC_ACQUIRE)
                : 0u;
}

static inline uint64_t uhm_heap_generation_next(uhm_heap *heap) {
    return heap ? atomic_fetch_add_explicit(&heap->core.generation, 1u,
                                            UHM_ATOMIC_ACQ_REL) + 1u
                : 0u;
}

#define UHM_REMOTE_READY_SEQ_ACTIVE_MASK UINT64_C(0xffffffff)
#define UHM_REMOTE_READY_SEQ_VERSION_ONE (UINT64_C(1) << 32)
#define UHM_REMOTE_READY_SEQ_END_DELTA \
    (UHM_REMOTE_READY_SEQ_VERSION_ONE - UINT64_C(1))

static inline void uhm_slab_remote_pending_note(uhm_heap *heap,
                                                uint16_t size_class) {
    uhm_atomic_u32_cell *cell;
    uint32_t bit;
    uint32_t mask;

    if (heap && size_class < UHM_SIZE_CLASS_COUNT) {
        cell = &heap->core.slab_remote_pending_masks[
            size_class / UHM_SLAB_REMOTE_PENDING_MASK_WORD_BITS];
        bit = UINT32_C(1) <<
              (size_class % UHM_SLAB_REMOTE_PENDING_MASK_WORD_BITS);
        mask = atomic_load_explicit(&cell->value,
                                    UHM_ATOMIC_RELAXED);
        if ((mask & bit) != 0u) {
            return;
        }
        do {
            if ((mask & bit) != 0u) {
                return;
            }
        } while (!atomic_compare_exchange_weak_explicit(
            &cell->value, &mask, mask | bit,
            UHM_ATOMIC_RELEASE, UHM_ATOMIC_RELAXED));
    }
}

static inline uint32_t uhm_slab_remote_pending_class_bit(
    uint16_t size_class) {
    return size_class < UHM_SIZE_CLASS_COUNT
               ? UINT32_C(1) <<
                     (size_class % UHM_SLAB_REMOTE_PENDING_MASK_WORD_BITS)
               : 0u;
}

static inline uint32_t uhm_slab_remote_pending_class_mask_load(
    const uhm_heap *heap,
    uint16_t size_class) {
    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0u;
    }
    return atomic_load_explicit(
        &((uhm_heap *)heap)
             ->core
             .slab_remote_pending_masks[
                 size_class / UHM_SLAB_REMOTE_PENDING_MASK_WORD_BITS]
             .value,
        UHM_ATOMIC_ACQUIRE);
}

static inline uint32_t uhm_slab_remote_pending_take(uhm_heap *heap,
                                                    uint16_t size_class) {
    uhm_atomic_u32_cell *cell;

    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0u;
    }
    cell = &heap->core.slab_remote_pending_masks[
        size_class / UHM_SLAB_REMOTE_PENDING_MASK_WORD_BITS];
    return atomic_fetch_and_explicit(
        &cell->value,
        ~uhm_slab_remote_pending_class_bit(size_class),
        UHM_ATOMIC_ACQ_REL);
}

static inline uint64_t uhm_slab_remote_ready_seq_load(
    const uhm_heap *heap,
    uint16_t size_class) {
    return heap && size_class < UHM_SIZE_CLASS_COUNT
               ? atomic_load_explicit(
                     &((uhm_heap *)heap)->core.slab_remote_ready_seq[size_class].value,
                     UHM_ATOMIC_ACQUIRE)
               : UINT64_C(0);
}

static inline void uhm_slab_remote_ready_update_begin(
    uhm_heap *heap,
    uint16_t size_class) {
    if (heap && size_class < UHM_SIZE_CLASS_COUNT) {
        (void)atomic_fetch_add_explicit(
            &heap->core.slab_remote_ready_seq[size_class].value, UINT64_C(1),
            UHM_ATOMIC_ACQ_REL);
    }
}

static inline void uhm_slab_remote_ready_update_end(
    uhm_heap *heap,
    uint16_t size_class) {
    if (heap && size_class < UHM_SIZE_CLASS_COUNT) {
        (void)atomic_fetch_add_explicit(
            &heap->core.slab_remote_ready_seq[size_class].value,
            UHM_REMOTE_READY_SEQ_END_DELTA, UHM_ATOMIC_RELEASE);
    }
}

static inline int uhm_slab_remote_ready_seq_active_value(uint64_t seq) {
    return (seq & UHM_REMOTE_READY_SEQ_ACTIVE_MASK) != UINT64_C(0);
}

#define UHM_SLAB_STATE_TRANSITION_ACTIVE_MASK UINT64_C(0xffffffff)
#define UHM_SLAB_STATE_TRANSITION_VERSION_ONE (UINT64_C(1) << 32)
#define UHM_SLAB_STATE_TRANSITION_END_DELTA \
    (UHM_SLAB_STATE_TRANSITION_VERSION_ONE - UINT64_C(1))

static inline uint64_t uhm_slab_state_transition_seq_load(
    const uhm_slab *slab) {
    return slab ? atomic_load_explicit(
                      &((uhm_slab *)slab)->state_transition_seq,
                      UHM_ATOMIC_ACQUIRE)
                : UINT64_C(0);
}

static inline void uhm_slab_state_transition_update_begin(uhm_slab *slab) {
    if (slab) {
        (void)atomic_fetch_add_explicit(&slab->state_transition_seq,
                                        UINT64_C(1), UHM_ATOMIC_ACQ_REL);
    }
}

static inline void uhm_slab_state_transition_update_end(uhm_slab *slab) {
    if (slab) {
        (void)atomic_fetch_add_explicit(&slab->state_transition_seq,
                                        UHM_SLAB_STATE_TRANSITION_END_DELTA,
                                        UHM_ATOMIC_RELEASE);
    }
}

static inline int uhm_slab_state_transition_seq_active_value(uint64_t seq) {
    return (seq & UHM_SLAB_STATE_TRANSITION_ACTIVE_MASK) != UINT64_C(0);
}

static inline size_t uhm_metadata_registered_readers_load(
    const uhm_heap *heap) {
    return heap ? atomic_load_explicit(&heap->metadata_registered_readers,
                                       UHM_ATOMIC_ACQUIRE)
                : 0u;
}

#define UHM_METADATA_READER_SEQ_ACTIVE_MASK UINT64_C(0xffffffff)
#define UHM_METADATA_READER_SEQ_VERSION_ONE (UINT64_C(1) << 32)
#define UHM_METADATA_READER_SEQ_END_DELTA \
    (UHM_METADATA_READER_SEQ_VERSION_ONE - UINT64_C(1))

static inline uint64_t uhm_metadata_registered_reader_seq_load(
    const uhm_heap *heap) {
    return heap ? atomic_load_explicit(&heap->metadata_registered_reader_seq,
                                       UHM_ATOMIC_ACQUIRE)
                : UINT64_C(0);
}

static inline void uhm_metadata_registered_reader_update_begin(
    uhm_heap *heap) {
    if (heap) {
        (void)atomic_fetch_add_explicit(
            &heap->metadata_registered_reader_seq, UINT64_C(1),
            UHM_ATOMIC_ACQ_REL);
    }
}

static inline void uhm_metadata_registered_reader_update_end(
    uhm_heap *heap) {
    if (heap) {
        (void)atomic_fetch_add_explicit(
            &heap->metadata_registered_reader_seq,
            UHM_METADATA_READER_SEQ_END_DELTA, UHM_ATOMIC_ACQ_REL);
    }
}

static inline int uhm_metadata_registered_reader_seq_active_value(
    uint64_t seq) {
    return (seq & UHM_METADATA_READER_SEQ_ACTIVE_MASK) != UINT64_C(0);
}

static inline int uhm_metadata_registered_reader_update_active(
    const uhm_heap *heap) {
    return uhm_metadata_registered_reader_seq_active_value(
        uhm_metadata_registered_reader_seq_load(heap));
}

#define UHM_METADATA_READER_LOCAL_SEQ_ACTIVE_MASK UINT32_C(1)

static inline uint32_t uhm_metadata_reader_local_update_seq_load(
    const uhm_metadata_reader_state *reader) {
    return reader ? atomic_load_explicit(&reader->local_update_seq,
                                         UHM_ATOMIC_ACQUIRE)
                  : 0u;
}

static inline int uhm_metadata_reader_local_update_seq_active_value(
    uint32_t seq) {
    return (seq & UHM_METADATA_READER_LOCAL_SEQ_ACTIVE_MASK) != 0u;
}

static inline void uhm_metadata_reader_local_update_begin(
    uhm_metadata_reader_state *reader) {
    if (reader) {
        (void)atomic_fetch_add_explicit(&reader->local_update_seq, 1u,
                                        UHM_ATOMIC_ACQ_REL);
    }
}

static inline void uhm_metadata_reader_local_update_end(
    uhm_metadata_reader_state *reader) {
    if (reader) {
        (void)atomic_fetch_add_explicit(&reader->local_update_seq, 1u,
                                        UHM_ATOMIC_ACQ_REL);
    }
}

static inline int uhm_metadata_reader_registered(
    const uhm_metadata_reader_state *reader) {
    return reader &&
           atomic_load_explicit(&reader->registered,
                                UHM_ATOMIC_ACQUIRE) != 0u;
}

static inline int uhm_metadata_registered_reader_count_increment(
    uhm_heap *heap) {
    size_t readers;

    if (!heap) {
        return 0;
    }
    readers = atomic_load_explicit(&heap->metadata_registered_readers,
                                   UHM_ATOMIC_ACQUIRE);
    for (;;) {
        if (readers == SIZE_MAX) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &heap->metadata_registered_readers, &readers, readers + 1u,
                UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
}

static inline int uhm_metadata_registered_reader_count_decrement(
    uhm_heap *heap) {
    size_t readers;

    if (!heap) {
        return 0;
    }
    readers = atomic_load_explicit(&heap->metadata_registered_readers,
                                   UHM_ATOMIC_ACQUIRE);
    for (;;) {
        if (readers == 0) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &heap->metadata_registered_readers, &readers, readers - 1u,
                UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
}

static inline int uhm_metadata_reader_owner_publish(
    uhm_metadata_reader_state *reader,
    const uhm_heap *heap,
    int registered) {
    if (!reader || !heap) {
        return 0;
    }
    atomic_store_explicit(&reader->registered, registered ? 1u : 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&reader->owner_heap, (uintptr_t)heap,
                          UHM_ATOMIC_RELEASE);
    if (registered &&
        !uhm_metadata_registered_reader_count_increment((uhm_heap *)heap)) {
        atomic_store_explicit(&reader->owner_heap, (uintptr_t)0,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&reader->registered, 0u, UHM_ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

static inline int uhm_metadata_reader_owner_release(
    uhm_metadata_reader_state *reader) {
    uintptr_t owner;
    uhm_heap *heap;
    uint32_t registered;

    if (!reader) {
        return 0;
    }
    owner = atomic_exchange_explicit(&reader->owner_heap, (uintptr_t)0,
                                     UHM_ATOMIC_ACQ_REL);
    registered = atomic_exchange_explicit(&reader->registered, 0u,
                                          UHM_ATOMIC_ACQ_REL);
    if (owner == (uintptr_t)0) {
        return 0;
    }
    heap = (uhm_heap *)owner;
    return registered != 0u
               ? uhm_metadata_registered_reader_count_decrement(heap)
               : 1;
}

static inline void uhm_metadata_reader_state_init(
    uhm_metadata_reader_state *reader) {
    size_t i;

    if (!reader) {
        return;
    }
    atomic_init(&reader->depth, 0u);
    atomic_init(&reader->epoch, 0u);
    atomic_init(&reader->owner_heap, (uintptr_t)0);
    atomic_init(&reader->registered, 0u);
    atomic_init(&reader->local_update_seq, 0u);
    for (i = 0; i < UHM_METADATA_HAZARD_SLOTS; ++i) {
        atomic_init(&reader->hazards[i], (uintptr_t)0);
    }
}

static inline void uhm_metadata_reader_clear(
    uhm_metadata_reader_state *reader) {
    uintptr_t owner;
    uhm_heap *heap = 0;
    int registered = 0;
    size_t i;

    if (!reader) {
        return;
    }
    owner = atomic_load_explicit(&reader->owner_heap, UHM_ATOMIC_ACQUIRE);
    if (owner != (uintptr_t)0) {
        heap = (uhm_heap *)owner;
        registered = uhm_metadata_reader_registered(reader);
        if (registered) {
            uhm_metadata_registered_reader_update_begin(heap);
        } else {
            uhm_metadata_reader_local_update_begin(reader);
        }
    }
    for (i = 0; i < UHM_METADATA_HAZARD_SLOTS; ++i) {
        atomic_store_explicit(&reader->hazards[i], (uintptr_t)0,
                              UHM_ATOMIC_RELEASE);
    }
    (void)uhm_metadata_reader_owner_release(reader);
    atomic_store_explicit(&reader->epoch, 0u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&reader->depth, 0u, UHM_ATOMIC_RELEASE);
    if (heap) {
        if (registered) {
            uhm_metadata_registered_reader_update_end(heap);
        } else {
            uhm_metadata_reader_local_update_end(reader);
        }
    }
}

static inline int uhm_metadata_reader_begin_mode(
    uhm_metadata_reader_state *reader,
    const uhm_heap *heap,
    int registered) {
    size_t depth;
    uint64_t epoch;

    if (!reader || !heap) {
        return 0;
    }
    epoch = atomic_load_explicit(&heap->core.metadata_published_epoch,
                                 UHM_ATOMIC_ACQUIRE);
    depth = atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE);
    for (;;) {
        if (depth == SIZE_MAX) {
            return 0;
        }
        if (depth == 0) {
            size_t expected = 0;
            if (registered) {
                uhm_metadata_registered_reader_update_begin((uhm_heap *)heap);
            } else {
                uhm_metadata_reader_local_update_begin(reader);
            }
            if (atomic_compare_exchange_weak_explicit(
                    &reader->depth, &expected, 1u,
                    UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
                if (!uhm_metadata_reader_owner_publish(reader, heap,
                                                       registered)) {
                    atomic_store_explicit(&reader->depth, 0u,
                                          UHM_ATOMIC_RELEASE);
                    if (registered) {
                        uhm_metadata_registered_reader_update_end(
                            (uhm_heap *)heap);
                    } else {
                        uhm_metadata_reader_local_update_end(reader);
                    }
                    return 0;
                }
                atomic_store_explicit(&reader->epoch, epoch,
                                      UHM_ATOMIC_RELEASE);
                if (registered) {
                    uhm_metadata_registered_reader_update_end(
                        (uhm_heap *)heap);
                } else {
                    uhm_metadata_reader_local_update_end(reader);
                }
                return 1;
            }
            if (registered) {
                uhm_metadata_registered_reader_update_end((uhm_heap *)heap);
            } else {
                uhm_metadata_reader_local_update_end(reader);
            }
            depth = expected;
            continue;
        }
        if (atomic_load_explicit(&reader->owner_heap, UHM_ATOMIC_ACQUIRE) !=
                (uintptr_t)heap ||
            uhm_metadata_reader_registered(reader) != (registered != 0)) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &reader->depth, &depth, depth + 1u,
                UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
}

static inline int uhm_metadata_reader_begin(
    uhm_metadata_reader_state *reader,
    const uhm_heap *heap) {
    return uhm_metadata_reader_begin_mode(reader, heap, 1);
}

static inline int uhm_metadata_reader_begin_unregistered(
    uhm_metadata_reader_state *reader,
    const uhm_heap *heap) {
    return uhm_metadata_reader_begin_mode(reader, heap, 0);
}

static inline int uhm_metadata_reader_try_begin_inactive(
    uhm_metadata_reader_state *reader,
    const uhm_heap *heap) {
    size_t expected = 0;
    uint64_t epoch;
    size_t i;

    if (!reader || !heap) {
        return 0;
    }
    if (atomic_load_explicit(&reader->epoch, UHM_ATOMIC_ACQUIRE) != 0) {
        return 0;
    }
    if (uhm_metadata_reader_registered(reader)) {
        return 0;
    }
    for (i = 0; i < UHM_METADATA_HAZARD_SLOTS; ++i) {
        if (atomic_load_explicit(&reader->hazards[i],
                                 UHM_ATOMIC_ACQUIRE) != (uintptr_t)0) {
            return 0;
        }
    }
    epoch = atomic_load_explicit(&heap->core.metadata_published_epoch,
                                 UHM_ATOMIC_ACQUIRE);
    uhm_metadata_registered_reader_update_begin((uhm_heap *)heap);
    if (!atomic_compare_exchange_strong_explicit(
            &reader->depth, &expected, 1u, UHM_ATOMIC_ACQ_REL,
            UHM_ATOMIC_ACQUIRE)) {
        uhm_metadata_registered_reader_update_end((uhm_heap *)heap);
        return 0;
    }
    if (!uhm_metadata_reader_owner_publish(reader, heap, 1)) {
        atomic_store_explicit(&reader->depth, 0u, UHM_ATOMIC_RELEASE);
        uhm_metadata_registered_reader_update_end((uhm_heap *)heap);
        return 0;
    }
    atomic_store_explicit(&reader->epoch, epoch, UHM_ATOMIC_RELEASE);
    uhm_metadata_registered_reader_update_end((uhm_heap *)heap);
    return 1;
}

static inline int uhm_metadata_reader_end(
    uhm_metadata_reader_state *reader) {
    size_t depth;
    size_t i;

    if (!reader) {
        return 0;
    }
    depth = atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE);
    for (;;) {
        if (depth == 0) {
            return 0;
        }
        if (depth == 1u) {
            size_t releasing = SIZE_MAX;
            uhm_heap *heap = 0;
            int registered = 0;
            uintptr_t owner =
                atomic_load_explicit(&reader->owner_heap, UHM_ATOMIC_ACQUIRE);
            if (owner != (uintptr_t)0) {
                heap = (uhm_heap *)owner;
                registered = uhm_metadata_reader_registered(reader);
                if (registered) {
                    uhm_metadata_registered_reader_update_begin(heap);
                } else {
                    uhm_metadata_reader_local_update_begin(reader);
                }
            }
            if (atomic_compare_exchange_weak_explicit(
                    &reader->depth, &depth, releasing,
                    UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
                for (i = 0; i < UHM_METADATA_HAZARD_SLOTS; ++i) {
                    atomic_store_explicit(&reader->hazards[i], (uintptr_t)0,
                                          UHM_ATOMIC_RELEASE);
                }
                atomic_store_explicit(&reader->epoch, 0u,
                                      UHM_ATOMIC_RELEASE);
                (void)uhm_metadata_reader_owner_release(reader);
                atomic_store_explicit(&reader->depth, 0u,
                                      UHM_ATOMIC_RELEASE);
                if (heap) {
                    if (registered) {
                        uhm_metadata_registered_reader_update_end(heap);
                    } else {
                        uhm_metadata_reader_local_update_end(reader);
                    }
                }
                return 1;
            }
            if (heap) {
                if (registered) {
                    uhm_metadata_registered_reader_update_end(heap);
                } else {
                    uhm_metadata_reader_local_update_end(reader);
                }
            }
            continue;
        }
        if (depth == SIZE_MAX) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &reader->depth, &depth, depth - 1u,
                UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
}

static inline int uhm_metadata_reader_hazard_set(
    uhm_metadata_reader_state *reader,
    size_t slot,
    const void *ptr) {
    if (!reader || slot >= UHM_METADATA_HAZARD_SLOTS ||
        atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE) == 0) {
        return 0;
    }
    atomic_store_explicit(&reader->hazards[slot], (uintptr_t)ptr,
                          UHM_ATOMIC_RELEASE);
    return 1;
}

static inline size_t uhm_metadata_hazard_readers_load(const uhm_heap *heap) {
    return heap ? atomic_load_explicit(&heap->metadata_hazard_readers,
                                       UHM_ATOMIC_ACQUIRE)
                : 0u;
}

static inline int uhm_metadata_hazard_begin(uhm_heap *heap) {
    size_t readers;

    if (!heap) {
        return 0;
    }
    readers = atomic_load_explicit(&heap->metadata_hazard_readers,
                                   UHM_ATOMIC_ACQUIRE);
    for (;;) {
        if (readers == SIZE_MAX) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &heap->metadata_hazard_readers, &readers, readers + 1u,
                UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
}

static inline int uhm_metadata_hazard_end(uhm_heap *heap) {
    size_t readers;

    if (!heap) {
        return 0;
    }
    readers = atomic_load_explicit(&heap->metadata_hazard_readers,
                                   UHM_ATOMIC_ACQUIRE);
    for (;;) {
        if (readers == 0) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &heap->metadata_hazard_readers, &readers, readers - 1u,
                UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
}

#if UHMAS_ENABLE_TEST_HOOKS
void uhm_test_local_cache_note_slab_push(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    uhm_local_retained_hint *hints,
    size_t *retained_pages,
    const uhm_slab *slab,
    size_t known_hint_index);
int uhm_test_local_cache_prepare_and_note_slab_push(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    uhm_local_retained_hint *hints,
    size_t *retained_pages,
    size_t page_limit,
    const uhm_slab *slab,
    size_t *out_existing_slot_count);
size_t uhm_test_local_retained_hint_set_base(const uhm_slab *slab);
int uhm_test_local_cache_find_fit_class(
    uint32_t nonempty_class_mask,
    uint16_t minimum_size_class,
    uint16_t *out_size_class);
int uhm_test_control_alloc_hint_note(uhm_heap *heap,
                                     uhm_control_extent *extent);
#endif

#endif
