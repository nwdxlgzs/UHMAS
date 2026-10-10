#include "uhm/uhm.h"
#include "uhm/uhm_platform.h"
#include "uhm_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#if defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#elif defined(__linux__)
#define MAP_ANONYMOUS 0x20
#endif
#endif
#endif

typedef struct bench_heap_env {
    uhm_platform_heap owner;
    uhm_heap *heap;
    const uhm_api *api;
    size_t arena_size;
} bench_heap_env;

typedef struct bench_result {
    const char *name;
    double seconds;
    size_t operations;
    uint64_t wall_time_ns;
    uint64_t cpu_work;
    uint32_t cpu_work_valid;
    uint32_t cpu_work_is_cycles;
    uint64_t latency_sample_count;
    uint64_t latency_min_ns;
    uint64_t latency_p50_ns;
    uint64_t latency_p95_ns;
    uint64_t latency_p99_ns;
    uint64_t latency_max_ns;
    uint64_t latency_work_units_per_sample;
    uint64_t latency_p95_per_work_unit_ns;
    uint64_t latency_p99_per_work_unit_ns;
    uint64_t latency_max_per_work_unit_ns;
    uint64_t latency_pages_per_sample;
    uint64_t latency_p95_per_page_ns;
    uint64_t latency_p99_per_page_ns;
    uint64_t latency_max_per_page_ns;
    uint64_t worker_thread_count;
    uint64_t worker_cpu_work;
    uint32_t worker_cpu_work_is_cycles;
    uint64_t worker_cpu_ns;
    uint64_t worker_wall_ns;
    uint64_t worker_wait_ns;
    uint64_t worker_max_wait_ns;
    uint64_t worker_cpu_ratio_ppm;
    uint64_t worker_wait_ratio_ppm;
    uint32_t worker_runtime_valid;
} bench_result;

enum {
    BENCH_WORKER_LATENCY_CAPACITY = 32u
};

typedef struct bench_worker_profile {
    uint64_t wall_begin_ns;
    uint64_t sample_begin_ns;
    uint64_t cpu_begin_work;
    uint64_t wall_ns;
    uint64_t cpu_work;
    uint64_t cpu_ns;
    uint64_t wait_ns;
    uint64_t latency_ns_per_op[BENCH_WORKER_LATENCY_CAPACITY];
    size_t sample_begin_iteration;
    size_t sample_count;
    size_t sample_iterations;
    size_t operations_per_iteration;
    uint32_t enabled;
    uint32_t cpu_valid;
    uint32_t cpu_work_is_cycles;
} bench_worker_profile;

typedef struct bench_diag_snapshot {
    uint64_t treap_priority_mix_calls;
    uint64_t treap_priority_mix_ns;
    uint64_t treap_priority_mix_max_ns;
    uint64_t treap_health_flags;
    uint64_t live_block_health_flags;
    uint64_t treap_repair_pending_flags;
    uint64_t treap_repair_rebuilds;
    uint64_t treap_repair_rehashes;
    uint64_t treap_repair_nodes;
    uint64_t treap_repair_failures;
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
    uint64_t live_block_tree_insertions;
    uint64_t live_block_tree_rotations;
    uint64_t live_block_tree_priority_ties;
    uint64_t live_block_tree_priority_bucket_mask;
    uint64_t live_block_tree_priority_bucket_max;
    uint64_t live_block_tree_priority_mix_calls;
    uint64_t live_block_tree_priority_mix_ns;
    uint64_t live_block_tree_priority_mix_max_ns;
    uint64_t live_block_tree_max_insert_depth;
    uint64_t live_block_tree_tracked_nodes;
    uint64_t live_block_tree_max_nodes;
    uint64_t live_block_tree_depth_limit_exceeded;
    uint64_t live_block_tree_insert_budget_exceeded;
    uint64_t live_block_tree_insert_rotation_budget_exceeded;
    uint64_t live_block_tree_find_calls;
    uint64_t live_block_tree_find_steps;
    uint64_t live_block_tree_max_find_steps;
    uint64_t live_block_tree_find_budget_exceeded;
    uint64_t live_block_tree_mutation_epoch;
    uint64_t live_block_tree_remove_calls;
    uint64_t live_block_tree_remove_sink_steps;
    uint64_t live_block_tree_max_remove_sink_steps;
    uint64_t live_block_tree_remove_budget_exceeded;
    uint64_t live_block_slow_tree_hits;
    uint64_t live_block_slow_tree_misses;
    uint64_t live_block_slow_fallback_scans;
    uint64_t live_block_slow_fallback_scanned_nodes;
    uint64_t live_block_slow_fallback_max_scanned_nodes;
    uint64_t live_block_slow_fallback_budget_exceeded;
    uint64_t live_block_slow_fallback_aborts;
    uint64_t live_block_slow_fallback_negative_stores;
    uint64_t live_block_slow_fallback_negative_hits;
    uint64_t live_block_slow_fallback_negative_evictions;
    uint64_t live_block_slow_fallback_throttle_epoch;
    uint64_t live_block_slow_fallback_epoch_scans;
    uint64_t live_block_slow_fallback_throttles;
    uint64_t live_block_slow_fallback_scenario_epoch;
    uint64_t live_block_slow_fallback_scenario_checks;
    uint64_t live_block_slow_fallback_scenario_throttles;
    uint64_t live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_scenario_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_scenario_candidates[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT];
    uint64_t live_block_lookup_fast_fail_reasons
        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT]
        [UHM_LIVE_BLOCK_FAST_FAIL_REASON_COUNT];
    uint64_t malloc_free_tree_insertions;
    uint64_t malloc_free_tree_rotations;
    uint64_t malloc_free_tree_priority_ties;
    uint64_t malloc_free_tree_priority_bucket_mask;
    uint64_t malloc_free_tree_priority_bucket_max;
    uint64_t malloc_free_tree_priority_mix_calls;
    uint64_t malloc_free_tree_priority_mix_ns;
    uint64_t malloc_free_tree_priority_mix_max_ns;
    uint64_t malloc_free_tree_max_insert_depth;
    uint64_t malloc_free_tree_tracked_nodes;
    uint64_t malloc_free_tree_max_nodes;
    uint64_t malloc_free_tree_depth_limit_exceeded;
    uint64_t malloc_free_tree_insert_budget_exceeded;
    uint64_t malloc_free_tree_insert_rotation_budget_exceeded;
    uint64_t malloc_free_tree_find_calls;
    uint64_t malloc_free_tree_find_steps;
    uint64_t malloc_free_tree_max_find_steps;
    uint64_t malloc_free_tree_find_budget_exceeded;
    uint64_t malloc_free_tree_remove_calls;
    uint64_t malloc_free_tree_remove_sink_steps;
    uint64_t malloc_free_tree_max_remove_sink_steps;
    uint64_t malloc_free_tree_remove_budget_exceeded;
    uint64_t vma_tree_insertions;
    uint64_t vma_tree_rotations;
    uint64_t vma_tree_priority_ties;
    uint64_t vma_tree_priority_bucket_mask;
    uint64_t vma_tree_priority_bucket_max;
    uint64_t vma_tree_priority_mix_calls;
    uint64_t vma_tree_priority_mix_ns;
    uint64_t vma_tree_priority_mix_max_ns;
    uint64_t vma_tree_max_insert_depth;
    uint64_t vma_tree_tracked_nodes;
    uint64_t vma_tree_max_nodes;
    uint64_t vma_tree_depth_limit_exceeded;
    uint64_t vma_tree_insert_budget_exceeded;
    uint64_t vma_tree_insert_rotation_budget_exceeded;
    uint64_t vma_tree_find_calls;
    uint64_t vma_tree_find_steps;
    uint64_t vma_tree_max_find_steps;
    uint64_t vma_tree_find_budget_exceeded;
    uint64_t vma_tree_remove_calls;
    uint64_t vma_tree_remove_sink_steps;
    uint64_t vma_tree_max_remove_sink_steps;
    uint64_t vma_tree_remove_budget_exceeded;
    uint64_t mmap_free_tree_insertions;
    uint64_t mmap_free_tree_rotations;
    uint64_t mmap_free_tree_priority_ties;
    uint64_t mmap_free_tree_priority_bucket_mask;
    uint64_t mmap_free_tree_priority_bucket_max;
    uint64_t mmap_free_tree_priority_mix_calls;
    uint64_t mmap_free_tree_priority_mix_ns;
    uint64_t mmap_free_tree_priority_mix_max_ns;
    uint64_t mmap_free_tree_max_insert_depth;
    uint64_t mmap_free_tree_tracked_nodes;
    uint64_t mmap_free_tree_max_nodes;
    uint64_t mmap_free_tree_depth_limit_exceeded;
    uint64_t mmap_free_tree_insert_budget_exceeded;
    uint64_t mmap_free_tree_insert_rotation_budget_exceeded;
    uint64_t mmap_free_tree_find_calls;
    uint64_t mmap_free_tree_find_steps;
    uint64_t mmap_free_tree_max_find_steps;
    uint64_t mmap_free_tree_find_budget_exceeded;
    uint64_t mmap_free_tree_remove_calls;
    uint64_t mmap_free_tree_remove_sink_steps;
    uint64_t mmap_free_tree_max_remove_sink_steps;
    uint64_t mmap_free_tree_remove_budget_exceeded;
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
    uint64_t page_span_calls;
    uint64_t page_span_pages;
    uint64_t page_mark_calls;
    uint64_t page_mark_pages;
    uint64_t page_mark_max_pages;
    uint64_t page_mark_budget_exceeded;
    uint64_t page_mark_leaf_spans;
    uint64_t page_mark_max_leaf_spans;
    uint64_t page_mark_phase_calls[UHM_PAGE_MARK_PHASE_COUNT];
    uint64_t page_mark_phase_pages[UHM_PAGE_MARK_PHASE_COUNT];
    uint64_t page_mark_phase_budget_exceeded[UHM_PAGE_MARK_PHASE_COUNT];
    uint64_t page_range_query_calls;
    uint64_t page_range_query_pages;
    uint64_t page_range_query_max_pages;
    uint64_t page_range_query_budget_exceeded;
    uint64_t page_range_query_leaf_spans;
    uint64_t page_range_query_max_leaf_spans;
    uint64_t page_range_query_phase_calls[UHM_PAGE_QUERY_PHASE_COUNT];
    uint64_t page_range_query_phase_pages[UHM_PAGE_QUERY_PHASE_COUNT];
    uint64_t page_range_query_phase_budget_exceeded[UHM_PAGE_QUERY_PHASE_COUNT];
    uint64_t page_lock_range_calls;
    uint64_t page_lock_range_shards;
    uint64_t page_lock_range_max_shards;
    uint64_t page_lock_range_budget_exceeded;
    uint64_t mprotect_mmap_noop_calls;
    uint64_t mprotect_page_private_noop_calls;
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
    uint64_t decommit_events;
    uint64_t decommit_bytes;
    uint64_t recommit_events;
    uint64_t recommit_bytes;
    uint64_t malloc_free_decommit_commit_all_fast_paths;
    uint64_t malloc_free_decommit_deferred_events;
    uint64_t malloc_free_decommit_deferred_bytes;
    uint64_t malloc_free_decommit_retained_bytes;
    uint64_t malloc_free_decommit_retention_base_high_water_bytes;
    uint64_t malloc_free_decommit_retention_high_water_bytes;
    uint64_t malloc_free_decommit_retention_max_high_water_bytes;
    uint64_t malloc_free_decommit_retention_pressure_events;
    uint64_t malloc_free_decommit_retention_pressure_bytes;
    uint64_t malloc_free_decommit_retention_adaptive_grow_events;
    uint64_t malloc_free_decommit_retention_adaptive_grow_bytes;
    uint64_t malloc_free_decommit_retention_adaptive_shrink_events;
    uint64_t malloc_free_decommit_retention_adaptive_shrink_bytes;
    uint64_t malloc_free_decommit_hot_deferred_events;
    uint64_t malloc_free_decommit_hot_deferred_bytes;
    uint64_t slab_empty_committed_cache_low_water;
    uint64_t slab_empty_committed_cache_base_high_water;
    uint64_t slab_empty_committed_cache_high_water;
    uint64_t slab_empty_committed_cache_max_high_water;
    uint64_t slab_empty_decommit_pressure_events;
    uint64_t slab_empty_decommit_pressure_bytes;
    uint64_t slab_empty_committed_cache_adaptive_grow_events;
    uint64_t slab_empty_committed_cache_adaptive_grow_bytes;
    uint64_t decommit_recommit_same_addr_events;
    uint64_t decommit_recommit_same_addr_bytes;
    uint64_t malloc_free_tree_current_nodes;
    uint64_t malloc_free_tree_current_depth;
    uint64_t vma_tree_current_nodes;
    uint64_t vma_tree_current_depth;
    uint64_t mmap_free_tree_current_nodes;
    uint64_t mmap_free_tree_current_depth;
    uint64_t vma_list_nodes;
    uint64_t mmap_free_list_nodes;
    uint64_t retired_vma_count;
    uint64_t retired_free_extent_count;
    uint64_t hot_vma_count;
    uint64_t hot_free_extent_count;
    uint64_t metadata_hot_vma_hits;
    uint64_t metadata_hot_vma_misses;
    uint64_t metadata_hot_vma_reclaimed;
    uint64_t metadata_hot_vma_full_defers;
    uint64_t metadata_hot_free_extent_hits;
    uint64_t metadata_hot_free_extent_misses;
    uint64_t metadata_hot_free_extent_reclaimed;
    uint64_t metadata_hot_free_extent_full_defers;
    uint64_t metadata_quarantine_drain_calls;
    uint64_t metadata_quarantine_drain_scanned;
    uint64_t metadata_quarantine_drain_reclaimed;
    uint64_t metadata_quarantine_drain_budget_exhausted;
    uint64_t metadata_quarantine_drain_rotated;
    uint64_t control_alloc_recent_hits;
    uint64_t control_alloc_hint_hits;
    uint64_t control_alloc_hint_misses;
    uint64_t control_alloc_hint_stale;
    uint64_t control_alloc_no_fit_fast_paths;
    uint64_t control_alloc_scan_calls;
    uint64_t control_alloc_scan_steps;
    uint64_t control_alloc_scan_max_steps;
    uint64_t control_alloc_scan_budget_exceeded;
    uint64_t tls_cache_hits;
    uint64_t tls_cache_misses;
    uint64_t tls_cache_pushes;
    uint64_t tls_cache_overflows;
    uint64_t tls_cache_retained_page_pressure;
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
    uint64_t heap_lock_calls;
    uint64_t heap_lock_try_calls;
    uint64_t heap_lock_try_failures;
    uint64_t heap_lock_contended_calls;
    uint64_t heap_lock_wait_ns;
    uint64_t heap_lock_max_wait_ns;
    uint64_t heap_lock_hold_ns;
    uint64_t heap_lock_max_hold_ns;
    uint64_t slab_raw_lock_calls;
    uint64_t slab_raw_lock_try_calls;
    uint64_t slab_raw_lock_try_failures;
    uint64_t slab_raw_lock_contended_calls;
    uint64_t slab_raw_lock_wait_ns;
    uint64_t slab_raw_lock_max_wait_ns;
    uint64_t slab_raw_lock_hold_ns;
    uint64_t slab_raw_lock_max_hold_ns;
} bench_diag_snapshot;

typedef struct bench_start_gate {
    _Atomic(uint32_t) ready;
    _Atomic(uint32_t) start;
} bench_start_gate;

typedef struct bench_thread_args {
    bench_heap_env *env;
    bench_start_gate *start_gate;
    size_t iterations;
    size_t affinity_index;
    int use_system;
    int pin_thread;
    int affinity_applied;
    int ok;
    bench_worker_profile *profile;
} bench_thread_args;

typedef struct bench_remote_free_args {
    bench_heap_env *env;
    bench_start_gate *start_gate;
    void **ptrs;
    size_t begin_index;
    size_t count;
    size_t affinity_index;
    int affinity_applied;
    int ok;
    bench_worker_profile *profile;
} bench_remote_free_args;

typedef struct bench_page_hotspot_args {
    bench_heap_env *env;
    bench_start_gate *start_gate;
    size_t iterations;
    size_t page_size;
    size_t affinity_index;
    int affinity_applied;
    int ok;
    bench_worker_profile *profile;
} bench_page_hotspot_args;

typedef struct bench_fault_vm_state {
    uhm_vm_bound_ops original;
    int fail_protect;
} bench_fault_vm_state;

typedef struct bench_logical_vm_state {
    uhm_vm_bound_ops original;
} bench_logical_vm_state;

static uhm_result UHM_CALL bench_logical_vm_commit(void *user, void *addr,
                                                    size_t size,
                                                    uint32_t prot) {
    (void)user;
    (void)addr;
    (void)size;
    (void)prot;
    return UHM_OK;
}

static uhm_result UHM_CALL bench_logical_vm_decommit(void *user, void *addr,
                                                      size_t size) {
    (void)user;
    (void)addr;
    (void)size;
    return UHM_OK;
}

static uhm_result UHM_CALL bench_logical_vm_protect(void *user, void *addr,
                                                     size_t size,
                                                     uint32_t prot) {
    (void)user;
    (void)addr;
    (void)size;
    (void)prot;
    return UHM_OK;
}

static uhm_result UHM_CALL bench_logical_vm_fixed_remap(void *user, void *addr,
                                                         size_t size,
                                                         uint32_t prot) {
    (void)user;
    (void)addr;
    (void)size;
    (void)prot;
    return UHM_OK;
}

static uhm_result UHM_CALL bench_logical_vm_numa_hint(
    void *user, void *addr, size_t size, uint32_t policy, int32_t node,
    uint32_t flags) {
    (void)user;
    (void)addr;
    (void)size;
    (void)policy;
    (void)node;
    (void)flags;
    return UHM_OK;
}

static void bench_logical_vm_install(bench_heap_env *env,
                                     bench_logical_vm_state *state) {
    memset(state, 0, sizeof(*state));
    state->original = env->heap->F.vm;
    env->heap->F.vm.user = state;
    env->heap->F.vm.commit = bench_logical_vm_commit;
    env->heap->F.vm.decommit = bench_logical_vm_decommit;
    env->heap->F.vm.protect = bench_logical_vm_protect;
    env->heap->F.vm.fixed_remap = bench_logical_vm_fixed_remap;
    env->heap->F.vm.numa_hint = bench_logical_vm_numa_hint;
}

static void bench_logical_vm_restore(bench_heap_env *env,
                                     bench_logical_vm_state *state) {
    env->heap->F.vm = state->original;
}

static uhm_result UHM_CALL bench_fault_vm_commit(void *user, void *addr,
                                                 size_t size, uint32_t prot) {
    bench_fault_vm_state *state = (bench_fault_vm_state *)user;
    return state->original.commit(state->original.user, addr, size, prot);
}

static uhm_result UHM_CALL bench_fault_vm_decommit(void *user, void *addr,
                                                   size_t size) {
    bench_fault_vm_state *state = (bench_fault_vm_state *)user;
    return state->original.decommit(state->original.user, addr, size);
}

static uhm_result UHM_CALL bench_fault_vm_protect(void *user, void *addr,
                                                  size_t size, uint32_t prot) {
    bench_fault_vm_state *state = (bench_fault_vm_state *)user;
    if (state->fail_protect) {
        return UHM_ERR_PLATFORM;
    }
    return state->original.protect(state->original.user, addr, size, prot);
}

static uhm_result UHM_CALL bench_fault_vm_fixed_remap(void *user, void *addr,
                                                      size_t size,
                                                      uint32_t prot) {
    bench_fault_vm_state *state = (bench_fault_vm_state *)user;
    return state->original.fixed_remap(state->original.user, addr, size, prot);
}

static uhm_result UHM_CALL bench_fault_vm_release(void *user, void *addr,
                                                  size_t size) {
    bench_fault_vm_state *state = (bench_fault_vm_state *)user;
    return state->original.release(state->original.user, addr, size);
}

static uhm_result UHM_CALL bench_fault_vm_numa_hint(void *user, void *addr,
                                                    size_t size,
                                                    uint32_t policy,
                                                    int32_t node,
                                                    uint32_t flags) {
    bench_fault_vm_state *state = (bench_fault_vm_state *)user;
    if (!state->original.numa_hint) {
        return UHM_ERR_UNSUPPORTED;
    }
    return state->original.numa_hint(state->original.user, addr, size, policy,
                                     node, flags);
}

static void bench_fault_vm_install(bench_heap_env *env,
                                   bench_fault_vm_state *state) {
    memset(state, 0, sizeof(*state));
    state->original = env->heap->F.vm;
    env->heap->F.vm.user = state;
    env->heap->F.vm.commit = bench_fault_vm_commit;
    env->heap->F.vm.decommit = bench_fault_vm_decommit;
    env->heap->F.vm.protect = bench_fault_vm_protect;
    env->heap->F.vm.fixed_remap = bench_fault_vm_fixed_remap;
    env->heap->F.vm.release = bench_fault_vm_release;
    env->heap->F.vm.numa_hint = bench_fault_vm_numa_hint;
}

static void bench_fault_vm_restore(bench_heap_env *env,
                                   bench_fault_vm_state *state) {
    env->heap->F.vm = state->original;
}

