#include "uhm/uhm.h"
#include "uhm/uhm_platform.h"
#include "uhm_internal.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define TEST_MAYBE_UNUSED __attribute__((unused))
#else
#define TEST_MAYBE_UNUSED
#endif

#if defined(_MSC_VER)
#pragma warning(disable : 4702)
#endif

static uint64_t test_atomic_u64_load(const uhm_diag_counter_u64 *slot) {
    return uhm_diag_counter_load_relaxed(slot);
}

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

typedef struct test_alloc {
    void *raw;
    void *aligned;
    size_t reserved_size;
    uhm_vm_ops vm;
} test_alloc;

typedef struct failing_vm_state {
    test_alloc arena;
    size_t reserve_size;
    size_t reserve_alignment;
    int reserve_count;
    int commit_count;
    int release_count;
    int fail_reserve;
} failing_vm_state;

#define FAULT_VM_OP_COMMIT       (1u << 0)
#define FAULT_VM_OP_DECOMMIT     (1u << 1)
#define FAULT_VM_OP_PROTECT      (1u << 2)
#define FAULT_VM_OP_FIXED_REMAP  (1u << 3)
#define FAULT_VM_OP_NUMA_HINT    (1u << 4)
#define FAULT_VM_PROB_DENOM      1000000u

typedef struct fault_vm_state {
    int commit_count;
    int decommit_count;
    int protect_count;
    int fixed_remap_count;
    int numa_hint_count;
    int fail_commit;
    int fail_commit_once;
    int fail_decommit;
    int fail_protect;
    int fail_fixed_remap;
    int fail_numa_hint;
    int partial_commit_before_failure;
    uint32_t platform_error;
    uint32_t fail_ops;
    uint32_t fail_probability_ppm;
    uint64_t fault_seed;
    uint64_t fault_rng_state;
    uint64_t fault_match_count;
    uint64_t fail_on_match;
    int fail_addr_range_enabled;
    uintptr_t fail_addr_begin;
    uintptr_t fail_addr_end;
    void *last_commit_addr;
    size_t last_commit_size;
    void *last_decommit_addr;
    size_t last_decommit_size;
    void *last_numa_hint_addr;
    size_t last_numa_hint_size;
    uint32_t last_numa_hint_policy;
    int32_t last_numa_hint_node;
} fault_vm_state;

typedef struct reentrant_check_vm_state {
    uhm_heap *heap;
    const uhm_api *api;
    uhm_result check_result;
    int commit_count;
    int decommit_count;
    int numa_hint_count;
    int protect_count;
    int check_calls;
    int check_failures;
    int run_check;
    int fail_decommit;
    uint32_t platform_error;
    int zero_decommit_range;
    uhm_slab *decommit_revalidate_slab;
    int run_mprotect_probe;
    int mprotect_probe_calls;
    void *mprotect_probe_addr;
    size_t mprotect_probe_size;
    uint32_t mprotect_probe_prot;
    uhm_result mprotect_probe_result;
    void *last_commit_addr;
    size_t last_commit_size;
    void *last_decommit_addr;
    size_t last_decommit_size;
} reentrant_check_vm_state;

typedef struct ctx_cross_thread_args {
    const uhm_api *api;
    uhm_heap *heap;
    uhm_thread_ctx *ctx;
    void *malloc_result;
} ctx_cross_thread_args;

typedef struct small_churn_args {
    const uhm_api *api;
    uhm_heap *heap;
    unsigned id;
    unsigned fail_stage;
    size_t fail_iteration;
    uhm_result last_error;
    uint32_t platform_error;
    int ok;
} small_churn_args;

typedef struct lockless_remote_reuse_args {
    const uhm_api *api;
    uhm_heap *heap;
    _Atomic(uint32_t) *ready_count;
    _Atomic(uint32_t) *start;
    void *result;
    unsigned id;
    int ok;
} lockless_remote_reuse_args;

typedef struct accounted_remote_contended_free_args {
    const uhm_api *api;
    uhm_heap *heap;
    void *ptr;
    _Atomic(uint32_t) done;
    int ok;
} accounted_remote_contended_free_args;

typedef struct accounted_remote_contended_malloc_args {
    const uhm_api *api;
    uhm_heap *heap;
    void *result;
    _Atomic(uint32_t) done;
    int ok;
} accounted_remote_contended_malloc_args;

typedef struct tls_exit_args {
    const uhm_api *api;
    uhm_heap *heap;
    void *ptr;
    uhm_slab_slot *slot;
    int ok;
} tls_exit_args;

typedef struct tls_churn_args {
    const uhm_api *api;
    uhm_heap *heap;
    unsigned id;
    size_t iterations;
    unsigned fail_stage;
    size_t fail_iteration;
    uhm_result last_error;
    uint32_t platform_error;
    _Atomic(uint32_t) *ready_count;
    _Atomic(uint32_t) *release;
    int ok;
} tls_churn_args;

typedef struct tls_hold_args {
    const uhm_api *api;
    uhm_heap *heap;
    _Atomic(uint32_t) ready;
    _Atomic(uint32_t) release;
    void *ptr;
    uhm_slab_slot *slot;
    int ok;
} tls_hold_args;

typedef struct lockless_tls_class_flush_args {
    const uhm_api *api;
    uhm_heap *heap;
    void *remote_ptr;
    uhm_slab_slot *remote_slot;
    size_t remote_count;
    int ok;
} lockless_tls_class_flush_args;

typedef struct large_realloc_args {
    const uhm_api *api;
    uhm_heap *heap;
    unsigned id;
    int ok;
} large_realloc_args;

typedef struct cross_thread_free_args {
    const uhm_api *api;
    uhm_heap *heap;
    void **ptrs;
    size_t *sizes;
    size_t start;
    size_t count;
    unsigned id;
    size_t fail_index;
    uhm_result last_error;
    uint32_t platform_error;
    int ok;
} cross_thread_free_args;

typedef struct ctx_churn_args {
    const uhm_api *api;
    uhm_heap *heap;
    void *ctx_storage;
    size_t ctx_storage_size;
    unsigned id;
    int ok;
} ctx_churn_args;

typedef struct control_compete_args {
    const uhm_api *api;
    uhm_heap *heap;
    void *pressure_map;
    size_t page_size;
    size_t pressure_pages;
    unsigned role;
    const char *fail_op;
    size_t fail_iter;
    uhm_result fail_result;
    int ok;
} control_compete_args;

typedef struct page_leaf_churn_args {
    const uhm_api *api;
    uhm_heap *heap;
    uintptr_t arena_base;
    size_t page_size;
    size_t leaf_pages;
    unsigned leaf_id;
    int ok;
} page_leaf_churn_args;

typedef struct close_during_mmap_vm_state {
    _Atomic(uint32_t) block_commit;
    _Atomic(uint32_t) commit_entered;
    _Atomic(uint32_t) release_commit;
    uint32_t commit_count;
    void *last_commit_addr;
    size_t last_commit_size;
} close_during_mmap_vm_state;

typedef struct close_during_mmap_map_args {
    const uhm_api *api;
    uhm_heap *heap;
    size_t map_size;
    void *addr;
    uhm_result result;
} close_during_mmap_map_args;

typedef struct close_during_mmap_close_args {
    const uhm_api *api;
    uhm_heap *heap;
    _Atomic(uint32_t) started;
    _Atomic(uint32_t) done;
    uhm_result result;
} close_during_mmap_close_args;

typedef struct tagged_freelist_test_node {
    uhm_tagged_freelist_node link;
    _Atomic(uint32_t) in_use;
    _Atomic(uint32_t) visits;
    uint32_t id;
} tagged_freelist_test_node;

typedef struct tagged_freelist_args {
    uhm_tagged_freelist *list;
    size_t iterations;
    int ok;
} tagged_freelist_args;

static void expect_true(int value, const char *message);

static void test_tagged_freelist_force_cas_failure(
    uhm_tagged_freelist *list,
    uhm_tagged_freelist_node *first,
    uhm_tagged_freelist_node *second) {
    uintptr_t stale_head;
    uintptr_t replacement_head;
    uintptr_t desired_head;
    uhm_tagged_freelist_node *link;

    uhm_tagged_freelist_next_store(first, 0);
    uhm_tagged_freelist_next_store(second, 0);
    stale_head = uhm_tagged_freelist_pack(first, 1u);
    replacement_head = uhm_tagged_freelist_pack(second, 2u);
    atomic_store_explicit(&list->head, stale_head, UHM_ATOMIC_RELEASE);
    expect_true(atomic_compare_exchange_strong_explicit(
                    &list->head, &stale_head, replacement_head,
                    UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE),
                "tagged freelist forced CAS setup");

    stale_head = uhm_tagged_freelist_pack(first, 1u);
    desired_head = uhm_tagged_freelist_pack(0, 2u);
    expect_true(!atomic_compare_exchange_strong_explicit(
                    &list->head, &stale_head, desired_head,
                    UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE),
                "tagged freelist forced stale CAS fails");
    (void)atomic_fetch_add_explicit(&list->cas_failures, UINT64_C(1),
                                    UHM_ATOMIC_RELAXED);

    link = uhm_tagged_freelist_pop(list);
    expect_true(link == second, "tagged freelist forced CAS cleanup pop");
    expect_true(uhm_tagged_freelist_pop(list) == 0,
                "tagged freelist forced CAS cleanup empty");
}

typedef struct leak_collect_state {
    size_t total_count;
    size_t malloc_count;
    size_t mmap_count;
    size_t page_private_count;
    size_t callback_limit;
    void *seen_malloc;
    void *seen_mmap;
} leak_collect_state;

