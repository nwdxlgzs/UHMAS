typedef struct test_live_lookup_counters {
    uint64_t fast_hits;
    uint64_t slow_calls;
    uint64_t tree_find_calls;
    uint64_t fallback_scans;
    uint64_t callsite_calls;
    uint64_t callsite_slow_calls;
    uint64_t callsite_fallback_scans;
    uint64_t callsite_throttles;
} test_live_lookup_counters;

typedef struct test_live_page_record_fault {
    uint32_t owner;
    uint32_t va_state;
    uint32_t commit_state;
} test_live_page_record_fault;

static const test_live_page_record_fault test_live_page_record_faults[] = {
    {UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE, UHM_PAGE_COMMIT_LIVE},
    {UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_FREE_REUSABLE, UHM_PAGE_COMMIT_LIVE},
    {UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_NONE},
};

static test_live_lookup_counters test_live_lookup_counters_take(
    uhm_heap *heap, size_t callsite) {
    test_live_lookup_counters counters;

    memset(&counters, 0, sizeof(counters));
    counters.fast_hits = heap->core.live_block_fast_hits;
    counters.slow_calls = heap->core.live_block_slow_calls;
    counters.tree_find_calls = heap->core.live_block_tree_find_calls;
    counters.fallback_scans = heap->core.live_block_slow_fallback_scans;
    if (callsite < UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT) {
        counters.callsite_calls = heap->core.live_block_lookup_calls[callsite];
        counters.callsite_slow_calls =
            heap->core.live_block_lookup_slow_calls[callsite];
        counters.callsite_fallback_scans =
            heap->core.live_block_lookup_fallback_scans[callsite];
        counters.callsite_throttles =
            heap->core.live_block_lookup_throttles[callsite];
    }
    return counters;
}

static void test_live_block_lookup_diagnostics(
    const uhm_storage_info *heap_info) {
    enum { fallback_pressure_count = 64 };
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    const size_t large_size = page_size * 2u + 128u;
    const size_t malloc_header_size =
        (sizeof(uhm_block) + (UHM_MIN_ALIGNMENT - 1u)) &
        ~(size_t)(UHM_MIN_ALIGNMENT - 1u);
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_storage_info ctx_info;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    void *ptr;
    void *thread_ctx_epoch_ptr;
    void *fallback_ptrs[fallback_pressure_count] = {0};
    void *fallback_target;
    void *fallback_second_target;
    void *cross_owner_map;
    void *cross_owner_fake_payload;
    void *other_descriptor_ptr;
    void *descriptor_owner_map;
    uhm_block *block;
    uhm_block *other_descriptor_block;
    uhm_block *fallback_block;
    uhm_block *fallback_second_block;
    uhm_block *saved_live_tree_root;
    uhm_vma *descriptor_owner_vma;
    uhm_extent_descriptor saved_desc;
    uhm_extent_descriptor saved_fallback_desc;
    uhm_extent_descriptor saved_fallback_second_desc;
    size_t fallback_i;
    size_t payload_page = 0;
    size_t payload_page_count = 0;
    int outside_word = 0;
    uint64_t fast_before;
    uint64_t slow_calls_before;
    uint64_t slow_hits_before;
    uint64_t slow_misses_before;
    uint64_t scanned_before;
    uint64_t slow_ns_before;
    uint64_t slow_max_ns_before;
    uint64_t fail_range_before;
    uint64_t fail_descriptor_before;
    uint64_t fail_header_before;
    uint64_t fail_payload_before;
    uint64_t terminal_before;
    uint64_t tree_find_before;
    uint64_t tree_hits_before;
    uint64_t fallback_before;
    uint64_t fallback_tree_misses_before;
    uint64_t fallback_misses_before;
    uint64_t fallback_scanned_before;
    uint64_t fallback_max_scanned_before;
    uint64_t fallback_budget_before;
    uint64_t fallback_abort_before;
    uint64_t fallback_negative_store_before;
    uint64_t fallback_negative_hit_before;
    uint64_t fallback_negative_eviction_before;
    uint64_t negative_cross_callsite_hit_before;
    uint64_t fallback_throttle_before;
    uint64_t fallback_throttle_after_first;
    uint64_t fallback_scan_after_throttle;
    uint64_t fallback_scenario_check_before;
    uint64_t fallback_scenario_throttle_before;
    uint64_t fallback_scenario_throttle_after_first;
    uint64_t fallback_scenario_candidate_before;
    uint64_t lookup_free_throttle_before;
    uint64_t lookup_thread_ctx_fallback_before;
    uint64_t lookup_thread_ctx_throttle_before;
    uint64_t thread_ctx_scenario_candidate_before;
    uint64_t thread_ctx_scenario_throttle_before;
    uint64_t thread_ctx_scenario_throttle_after_first;
    uint64_t thread_ctx_scan_after_throttle;
    size_t negative_slot_i;
    int negative_usable_tag_seen;
    int negative_thread_ctx_tag_seen;
    int negative_cache_empty;
    uint64_t scenario_calls_before;
    uint64_t scenario_candidates_before;
    uint64_t lookup_usable_calls_before;
    uint64_t lookup_usable_slow_before;
    uint64_t lookup_usable_fallback_before;
    uint64_t lookup_usable_throttle_before;
    uint64_t lookup_usable_fail_descriptor_before;
    uint64_t lookup_usable_fail_range_before;
    uint64_t lookup_usable_fail_header_before;
    uint64_t lookup_usable_fail_payload_before;
    uint64_t lookup_free_calls_before;
    uint64_t lookup_realloc_calls_before;
    uint64_t exact_get_expected_before;
    uint64_t exact_has_state_before;
    uint64_t exact_tree_find_before;
    uint64_t exact_fallback_before;
    uint64_t exact_expected_probes;
    uint64_t negative_epoch_before_mutation;
    uint64_t saved_priority;
    uhm_block *fallback_loop_block;
    uhm_block *saved_live_right;
    void *fake_payload;
    uhm_extent_descriptor saved_fallback_loop_desc;
    uhm_page_record *payload_record;
    uhm_page_record saved_payload_record;
    uint64_t saved_payload_descriptor_id;
    size_t page_record_fault_i;
    uint64_t descriptor_drift_ids[2];
    size_t descriptor_drift_i;
    size_t thread_ctx_i;
    uint64_t live_find_budget_before;
    int fallback_throttle_seen;
    int thread_ctx_throttle_seen;
    uint64_t saved_salt;
    uint64_t treap_health_before;
    void *thread_ctx_fake_payload;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live block diagnostics heap storage alloc");
    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.size = sizeof(ctx_info);
    expect_ok(uhm_heap_storage_query(0, &ctx_info),
              "live block diagnostics ctx storage query");
    expect_true(test_vm_reserve_commit_alloc(ctx_info.storage_size,
                                             ctx_info.storage_alignment,
                                             &ctx_mem) != 0,
                "live block diagnostics ctx storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live block diagnostics arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live block diagnostics usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "live block diagnostics heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live block diagnostics api");
    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info.storage_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "live block diagnostics thread ctx open");

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "live block diagnostics malloc");
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "live block diagnostics block lookup");
    expect_true(heap->core.live_block_tree_current_nodes == 1u,
                "live block diagnostics live tree tracks allocation");
    expect_true(heap->core.live_block_tree_insert_budget_exceeded == 0,
                "live block diagnostics insert budget not exceeded");
    expect_true(heap->core.live_block_tree_insert_rotation_budget_exceeded == 0,
                "live block diagnostics insert rotation budget not exceeded");
    expect_true(heap->core.live_block_tree_root != 0,
                "live block diagnostics priority cache corruption target");
    expect_true(heap->core.treap_priority_salt != 0,
                "live block tree priority salt initialized");
    saved_salt = heap->core.treap_priority_salt;
    heap->core.treap_priority_salt ^= UINT64_C(0x9e3779b97f4a7c15);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "live block tree priority salt mismatch detected");
    heap->core.treap_priority_salt = saved_salt;
    expect_ok(api->check(heap, 0),
              "live block tree priority salt restored");

    saved_priority = heap->core.live_block_tree_root->live_tree_priority;
    heap->core.live_block_tree_root->live_tree_priority ^= UINT64_C(1);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "live block tree cached priority corruption detected");
    heap->core.live_block_tree_root->live_tree_priority = saved_priority;
    expect_ok(api->check(heap, 0),
              "live block tree cached priority corruption restored");

    scenario_calls_before =
        heap->core.live_block_slow_fallback_scenario_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    scenario_candidates_before =
        heap->core.live_block_slow_fallback_scenario_candidates
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    expect_true(api->malloc_usable_size(heap, ptr) >= large_size,
                "live block fast lookup scenario probe");
    expect_true(heap->core.live_block_slow_fallback_scenario_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    scenario_calls_before &&
                    heap->core.live_block_slow_fallback_scenario_candidates
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                        scenario_candidates_before,
                "live block fast lookup skips scenario bookkeeping");

    expect_true(heap->core.live_block_tree_root != 0,
                "live block tree find guard corruption target");
    fake_payload = (void *)((uintptr_t)ptr + UHM_MIN_ALIGNMENT);
    saved_live_right = heap->core.live_block_tree_root->live_tree_right;
    live_find_budget_before = heap->core.live_block_tree_find_budget_exceeded;
    treap_health_before = heap->core.treap_health_flags;
    heap->core.live_block_tree_root->live_tree_right =
        heap->core.live_block_tree_root;
    expect_true(api->malloc_usable_size(heap, fake_payload) == 0u,
                "live block tree find guard returns instead of hanging");
    expect_true(heap->core.live_block_tree_find_budget_exceeded >
                    live_find_budget_before,
                "live block tree find guard budget counted");
    expect_true((heap->core.treap_health_flags &
                 UHM_TREAP_HEALTH_LIVE_FIND) != 0,
                "live block tree find guard health flag set");
    expect_true(heap->core.treap_health_flags != treap_health_before,
                "live block tree health flag is sticky");
    heap->core.live_block_tree_root->live_tree_right = saved_live_right;
    expect_ok(api->check(heap, 0), "live block tree find guard restored");
    expect_true(api->check(heap, UHM_CHECK_FAIL_ON_TREAP_HEALTH) ==
                    UHM_ERR_CORRUPTION,
                "live block tree strict health check fails after guard");

    fast_before = heap->core.live_block_fast_hits;
    slow_calls_before = heap->core.live_block_slow_calls;
    exact_get_expected_before =
        test_atomic_u64_load(
            &heap->core.page_range_query_phase_calls
                 [UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE]);
    exact_has_state_before =
        test_atomic_u64_load(
            &heap->core.page_range_query_phase_calls
                 [UHM_PAGE_QUERY_PHASE_HAS_STATE]);
    exact_tree_find_before = heap->core.live_block_tree_find_calls;
    exact_fallback_before = heap->core.live_block_slow_fallback_scans;
    exact_expected_probes =
        ((uintptr_t)block / page_size == (uintptr_t)ptr / page_size) ? 1u : 2u;
    expect_true(exact_expected_probes == 1u ||
                    ((uintptr_t)ptr % page_size) == 0,
                "live block exact descriptor test layout supported");
    lookup_usable_calls_before =
        heap->core.live_block_lookup_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_slow_before =
        heap->core.live_block_lookup_slow_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    expect_true(api->malloc_usable_size(heap, ptr) >= large_size,
                "live block diagnostics fast usable size");
    expect_true(heap->core.live_block_fast_hits == fast_before + 1u,
                "live block diagnostics fast hit counted");
    expect_true(heap->core.live_block_slow_calls == slow_calls_before,
                "live block diagnostics fast path avoids slow scan");
    expect_true(test_atomic_u64_load(
                    &heap->core.page_range_query_phase_calls
                         [UHM_PAGE_QUERY_PHASE_HAS_STATE]) ==
                    exact_has_state_before,
                "live block exact descriptor skips header range has-state");
    expect_true(test_atomic_u64_load(
                    &heap->core.page_range_query_phase_calls
                         [UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE]) ==
                    exact_get_expected_before + exact_expected_probes,
                "live block exact descriptor uses single-page probes");
    expect_true(heap->core.live_block_tree_find_calls == exact_tree_find_before,
                "live block exact descriptor avoids live tree lookup");
    expect_true(heap->core.live_block_slow_fallback_scans ==
                    exact_fallback_before,
                "live block exact descriptor avoids physical fallback");
    expect_true(heap->core.live_block_lookup_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    lookup_usable_calls_before + 1u,
                "live block diagnostics usable lookup call counted");
    expect_true(heap->core.live_block_lookup_slow_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    lookup_usable_slow_before,
                "live block diagnostics fast usable avoids slow callsite");

    expect_true(test_shadow_page_span(heap, (uintptr_t)ptr, 1u,
                                      &payload_page, &payload_page_count) &&
                    payload_page_count == 1u,
                "live block descriptor missing payload page span");
    payload_record = test_page_record_at(heap, payload_page);
    expect_true(payload_record != 0 &&
                    payload_record->owner == UHM_PAGE_OWNER_MALLOC &&
                    payload_record->va_state == UHM_PAGE_VA_MALLOC_LIVE &&
                    payload_record->commit_state == UHM_PAGE_COMMIT_LIVE &&
                    payload_record->descriptor_id == block->desc.id,
                "live block descriptor missing baseline record");
    saved_payload_descriptor_id = payload_record->descriptor_id;
    payload_record->descriptor_id = 0;
    exact_get_expected_before =
        test_atomic_u64_load(
            &heap->core.page_range_query_phase_calls
                 [UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE]);

    fast_before = heap->core.live_block_fast_hits;
    slow_calls_before = heap->core.live_block_slow_calls;
    tree_find_before = heap->core.live_block_tree_find_calls;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    lookup_usable_calls_before =
        heap->core.live_block_lookup_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_slow_before =
        heap->core.live_block_lookup_slow_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_fallback_before =
        heap->core.live_block_lookup_fallback_scans
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    expect_true(api->malloc_usable_size(heap, ptr) >= large_size,
                "live block descriptor missing usable still fast");
    expect_true(test_atomic_u64_load(
                    &heap->core.page_range_query_phase_calls
                         [UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE]) ==
                    exact_get_expected_before + 1u,
                "descriptor slow lookup reuses payload page probe");
    expect_true(heap->core.live_block_fast_hits == fast_before + 1u,
                "live block descriptor missing fast hit counted");
    expect_true(heap->core.live_block_slow_calls == slow_calls_before &&
                    heap->core.live_block_tree_find_calls == tree_find_before &&
                    heap->core.live_block_slow_fallback_scans ==
                        fallback_before,
                "live block descriptor missing avoids slow tree and fallback");
    expect_true(heap->core.live_block_lookup_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    lookup_usable_calls_before + 1u &&
                    heap->core.live_block_lookup_slow_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                        lookup_usable_slow_before &&
                    heap->core.live_block_lookup_fallback_scans
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                        lookup_usable_fallback_before,
                "live block descriptor missing callsite stays fast");

    slow_calls_before = heap->core.live_block_slow_calls;
    slow_misses_before = heap->core.live_block_slow_misses;
    tree_find_before = heap->core.live_block_tree_find_calls;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    terminal_before = heap->core.live_block_fast_terminal_misses;
    fail_payload_before = heap->core.live_block_fast_fail_payload_page;
    lookup_usable_fail_payload_before =
        heap->core.live_block_lookup_fast_fail_reasons
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
            [UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE];
    lookup_usable_fallback_before =
        heap->core.live_block_lookup_fallback_scans
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    expect_true(api->malloc_usable_size(heap, fake_payload) == 0u,
                "live block descriptor missing fake payload rejected");
    expect_true(heap->core.live_block_slow_calls == slow_calls_before &&
                    heap->core.live_block_slow_misses == slow_misses_before &&
                    heap->core.live_block_tree_find_calls == tree_find_before,
                "live block descriptor missing fake skips slow lookup");
    expect_true(heap->core.live_block_slow_fallback_scans == fallback_before &&
                    heap->core.live_block_lookup_fallback_scans
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                        lookup_usable_fallback_before,
                "live block descriptor missing fake avoids fallback");
    expect_true(heap->core.live_block_fast_terminal_misses ==
                    terminal_before + 1u,
                "live block descriptor missing fake terminal counted");
    expect_true(heap->core.live_block_fast_fail_payload_page ==
                    fail_payload_before + 1u,
                "live block descriptor missing fake payload fail counted");
    expect_true(heap->core.live_block_lookup_fast_fail_reasons
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                        [UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE] ==
                    lookup_usable_fail_payload_before + 1u,
                "live block descriptor missing fake callsite attributed");

    payload_record->descriptor_id = saved_payload_descriptor_id;
    expect_ok(api->check(heap, 0),
              "live block descriptor missing restored check");

    saved_payload_record = *payload_record;
    for (page_record_fault_i = 0;
         page_record_fault_i <
         sizeof(test_live_page_record_faults) /
             sizeof(test_live_page_record_faults[0]);
         ++page_record_fault_i) {
        *payload_record = saved_payload_record;
        payload_record->owner =
            test_live_page_record_faults[page_record_fault_i].owner;
        payload_record->va_state =
            test_live_page_record_faults[page_record_fault_i].va_state;
        payload_record->commit_state =
            test_live_page_record_faults[page_record_fault_i].commit_state;

        fast_before = heap->core.live_block_fast_hits;
        slow_calls_before = heap->core.live_block_slow_calls;
        slow_misses_before = heap->core.live_block_slow_misses;
        tree_find_before = heap->core.live_block_tree_find_calls;
        fallback_before = heap->core.live_block_slow_fallback_scans;
        terminal_before = heap->core.live_block_fast_terminal_misses;
        fail_header_before = heap->core.live_block_fast_fail_header_page;
        lookup_usable_calls_before =
            heap->core.live_block_lookup_calls
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
        lookup_usable_slow_before =
            heap->core.live_block_lookup_slow_calls
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
        lookup_usable_fallback_before =
            heap->core.live_block_lookup_fallback_scans
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
        lookup_usable_fail_header_before =
            heap->core.live_block_lookup_fast_fail_reasons
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                [UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE];

        expect_true(api->malloc_usable_size(heap, ptr) == 0u,
                    "live block page record fault rejected");
        expect_true(heap->core.live_block_fast_hits == fast_before,
                    "live block page record fault no fast hit");
        expect_true(heap->core.live_block_slow_calls == slow_calls_before &&
                        heap->core.live_block_slow_misses ==
                            slow_misses_before &&
                        heap->core.live_block_tree_find_calls ==
                            tree_find_before,
                    "live block page record fault skips slow lookup");
        expect_true(heap->core.live_block_slow_fallback_scans ==
                        fallback_before &&
                        heap->core.live_block_lookup_fallback_scans
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                            lookup_usable_fallback_before,
                    "live block page record fault avoids fallback");
        expect_true(heap->core.live_block_fast_terminal_misses ==
                        terminal_before + 1u,
                    "live block page record fault terminal counted");
        expect_true(heap->core.live_block_fast_fail_header_page ==
                        fail_header_before + 1u,
                    "live block page record fault header fail counted");
        expect_true(heap->core.live_block_lookup_calls
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                        lookup_usable_calls_before + 1u &&
                        heap->core.live_block_lookup_slow_calls
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                            lookup_usable_slow_before &&
                        heap->core.live_block_lookup_fast_fail_reasons
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                            [UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE] ==
                            lookup_usable_fail_header_before + 1u,
                    "live block page record fault callsite attributed");
    }
    *payload_record = saved_payload_record;
    expect_ok(api->check(heap, 0),
              "live block page record fault restored check");

    other_descriptor_ptr = api->malloc(heap, large_size);
    expect_true(other_descriptor_ptr != 0,
                "live block descriptor drift other malloc");
    other_descriptor_block =
        test_find_block_by_payload(heap, other_descriptor_ptr);
    expect_true(other_descriptor_block != 0 &&
                    other_descriptor_block != block,
                "live block descriptor drift other malloc block");
    descriptor_owner_map = 0;
    expect_ok(api->mmap(heap, &descriptor_owner_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "live block descriptor drift mmap owner");
    descriptor_owner_vma = heap->core.vma_head;
    while (descriptor_owner_vma &&
           !((uintptr_t)descriptor_owner_map >= descriptor_owner_vma->addr &&
             (uintptr_t)descriptor_owner_map <
                 descriptor_owner_vma->addr + descriptor_owner_vma->size)) {
        descriptor_owner_vma = descriptor_owner_vma->next;
    }
    expect_true(descriptor_owner_vma != 0 &&
                    descriptor_owner_vma->desc.id != 0,
                "live block descriptor drift mmap vma descriptor");
    descriptor_drift_ids[0] = other_descriptor_block->desc.id;
    descriptor_drift_ids[1] = descriptor_owner_vma->desc.id;
    for (descriptor_drift_i = 0; descriptor_drift_i < 2u;
         ++descriptor_drift_i) {
        payload_record->descriptor_id = descriptor_drift_ids[descriptor_drift_i];
        fast_before = heap->core.live_block_fast_hits;
        slow_calls_before = heap->core.live_block_slow_calls;
        tree_find_before = heap->core.live_block_tree_find_calls;
        fallback_before = heap->core.live_block_slow_fallback_scans;
        lookup_usable_calls_before =
            heap->core.live_block_lookup_calls
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
        lookup_usable_slow_before =
            heap->core.live_block_lookup_slow_calls
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
        lookup_usable_fallback_before =
            heap->core.live_block_lookup_fallback_scans
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];

        expect_true(api->malloc_usable_size(heap, ptr) >= large_size,
                    "live block descriptor drift legal payload fast");
        expect_true(heap->core.live_block_fast_hits == fast_before + 1u,
                    "live block descriptor drift fast hit counted");
        expect_true(heap->core.live_block_slow_calls == slow_calls_before &&
                        heap->core.live_block_tree_find_calls ==
                            tree_find_before &&
                        heap->core.live_block_slow_fallback_scans ==
                            fallback_before,
                    "live block descriptor drift avoids slow tree and fallback");
        expect_true(heap->core.live_block_lookup_calls
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                        lookup_usable_calls_before + 1u &&
                        heap->core.live_block_lookup_slow_calls
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                            lookup_usable_slow_before &&
                        heap->core.live_block_lookup_fallback_scans
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                            lookup_usable_fallback_before,
                    "live block descriptor drift callsite stays fast");

        slow_calls_before = heap->core.live_block_slow_calls;
        slow_misses_before = heap->core.live_block_slow_misses;
        tree_find_before = heap->core.live_block_tree_find_calls;
        fallback_before = heap->core.live_block_slow_fallback_scans;
        fail_descriptor_before = heap->core.live_block_fast_fail_descriptor;
        lookup_usable_calls_before =
            heap->core.live_block_lookup_calls
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
        lookup_usable_slow_before =
            heap->core.live_block_lookup_slow_calls
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
        lookup_usable_fallback_before =
            heap->core.live_block_lookup_fallback_scans
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
        lookup_usable_fail_descriptor_before =
            heap->core.live_block_lookup_fast_fail_reasons
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                [UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR];
        expect_true(api->malloc_usable_size(heap, fake_payload) == 0u,
                    "live block descriptor drift fake payload rejected");
        expect_true(heap->core.live_block_slow_calls ==
                        slow_calls_before + 1u &&
                        heap->core.live_block_slow_misses ==
                            slow_misses_before + 1u &&
                        heap->core.live_block_tree_find_calls ==
                            tree_find_before + 1u,
                    "live block descriptor drift fake uses bounded tree miss");
        expect_true(heap->core.live_block_slow_fallback_scans ==
                        fallback_before &&
                        heap->core.live_block_lookup_fallback_scans
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                            lookup_usable_fallback_before,
                    "live block descriptor drift fake avoids fallback");
        expect_true(heap->core.live_block_fast_fail_descriptor ==
                        fail_descriptor_before + 1u,
                    "live block descriptor drift fake descriptor fail counted");
        expect_true(heap->core.live_block_lookup_calls
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                        lookup_usable_calls_before + 1u &&
                        heap->core.live_block_lookup_slow_calls
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                            lookup_usable_slow_before + 1u &&
                        heap->core.live_block_lookup_fast_fail_reasons
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                            [UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR] ==
                            lookup_usable_fail_descriptor_before + 1u,
                    "live block descriptor drift fake callsite attributed");
    }
    *payload_record = saved_payload_record;
    expect_ok(api->check(heap, 0),
              "live block descriptor drift restored check");

    saved_desc = block->desc;
    slow_calls_before = heap->core.live_block_slow_calls;
    slow_hits_before = heap->core.live_block_slow_hits;
    scanned_before = heap->core.live_block_slow_scanned_nodes;
    slow_ns_before = heap->core.live_block_slow_ns;
    slow_max_ns_before = heap->core.live_block_slow_max_ns;
    fail_descriptor_before = heap->core.live_block_fast_fail_descriptor;
    tree_find_before = heap->core.live_block_tree_find_calls;
    tree_hits_before = heap->core.live_block_slow_tree_hits;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    lookup_usable_calls_before =
        heap->core.live_block_lookup_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_slow_before =
        heap->core.live_block_lookup_slow_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_fallback_before =
        heap->core.live_block_lookup_fallback_scans
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_fail_descriptor_before =
        heap->core.live_block_lookup_fast_fail_reasons
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
            [UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR];
    block->desc.magic ^= 0x5a5a5a5au;
    expect_true(api->malloc_usable_size(heap, ptr) >= large_size,
                "live block diagnostics slow usable size");
    block->desc = saved_desc;
    expect_true(heap->core.live_block_slow_calls == slow_calls_before + 1u,
                "live block diagnostics slow call counted");
    expect_true(heap->core.live_block_slow_hits == slow_hits_before + 1u,
                "live block diagnostics slow hit counted");
    expect_true(heap->core.live_block_slow_scanned_nodes > scanned_before,
                "live block diagnostics slow scanned nodes counted");
    expect_true(heap->core.live_block_slow_max_scanned_nodes != 0,
                "live block diagnostics slow max scan counted");
    expect_true(heap->core.live_block_slow_ns == slow_ns_before,
                "live block diagnostics timing disabled by default");
    expect_true(heap->core.live_block_slow_max_ns == slow_max_ns_before,
                "live block diagnostics max timing disabled by default");
    expect_true(heap->core.live_block_fast_fail_descriptor ==
                    fail_descriptor_before + 1u,
                "live block diagnostics descriptor fail counted");
    expect_true(heap->core.live_block_lookup_fast_fail_reasons
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                        [UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR] ==
                    lookup_usable_fail_descriptor_before + 1u,
                "live block diagnostics descriptor fail callsite attributed");
    expect_true(heap->core.live_block_tree_find_calls == tree_find_before + 1u,
                "live block diagnostics live tree find counted");
    expect_true(heap->core.live_block_slow_tree_hits == tree_hits_before + 1u,
                "live block diagnostics slow path uses live tree");
    expect_true(heap->core.live_block_slow_fallback_scans == fallback_before,
                "live block diagnostics avoids physical chain fallback");
    expect_true(heap->core.live_block_lookup_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    lookup_usable_calls_before + 1u,
                "live block diagnostics slow usable lookup call counted");
    expect_true(heap->core.live_block_lookup_slow_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    lookup_usable_slow_before + 1u,
                "live block diagnostics slow usable callsite counted");
    expect_true(heap->core.live_block_lookup_fallback_scans
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    lookup_usable_fallback_before,
                "live block diagnostics tree hit avoids fallback callsite");

    uhm_heap_set_diagnostics_timing_enabled(heap, 1u);
    slow_ns_before = heap->core.live_block_slow_ns;
    slow_max_ns_before = heap->core.live_block_slow_max_ns;
    block->desc.magic ^= 0x13572468u;
    expect_true(api->malloc_usable_size(heap, ptr) >= large_size,
                "live block diagnostics timed slow usable size");
    block->desc = saved_desc;
    expect_true(heap->core.live_block_slow_ns > slow_ns_before,
                "live block diagnostics timing opt-in elapsed tracked");
    expect_true(heap->core.live_block_slow_max_ns >= slow_max_ns_before,
                "live block diagnostics timing opt-in max tracked");
    uhm_heap_set_diagnostics_timing_enabled(heap, 0u);

    slow_calls_before = heap->core.live_block_slow_calls;
    slow_misses_before = heap->core.live_block_slow_misses;
    fail_range_before = heap->core.live_block_fast_fail_range;
    terminal_before = heap->core.live_block_fast_terminal_misses;
    lookup_usable_fail_range_before =
        heap->core.live_block_lookup_fast_fail_reasons
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
            [UHM_LIVE_BLOCK_FAST_FAIL_RANGE];
    expect_true(api->malloc_usable_size(heap, &outside_word) == 0,
                "live block diagnostics invalid pointer rejected");
    expect_true(heap->core.live_block_slow_calls == slow_calls_before,
                "live block diagnostics terminal miss skips slow call");
    expect_true(heap->core.live_block_slow_misses == slow_misses_before,
                "live block diagnostics terminal miss skips slow miss");
    expect_true(heap->core.live_block_fast_fail_range == fail_range_before + 1u,
                "live block diagnostics range fail counted");
    expect_true(heap->core.live_block_lookup_fast_fail_reasons
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                        [UHM_LIVE_BLOCK_FAST_FAIL_RANGE] ==
                    lookup_usable_fail_range_before + 1u,
                "live block diagnostics range fail callsite attributed");
    expect_true(heap->core.live_block_fast_terminal_misses ==
                    terminal_before + 1u,
                "live block diagnostics terminal miss counted");

    cross_owner_map = 0;
    expect_ok(api->mmap(heap, &cross_owner_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "live block diagnostics cross-owner mmap");
    cross_owner_fake_payload =
        (void *)((uintptr_t)cross_owner_map + malloc_header_size);
    slow_calls_before = heap->core.live_block_slow_calls;
    slow_misses_before = heap->core.live_block_slow_misses;
    tree_find_before = heap->core.live_block_tree_find_calls;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    terminal_before = heap->core.live_block_fast_terminal_misses;
    fail_header_before = heap->core.live_block_fast_fail_header_page;
    lookup_usable_fail_header_before =
        heap->core.live_block_lookup_fast_fail_reasons
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
            [UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE];
    lookup_usable_fallback_before =
        heap->core.live_block_lookup_fallback_scans
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    expect_true(api->malloc_usable_size(heap, cross_owner_fake_payload) == 0u,
                "live block diagnostics cross-owner payload rejected");
    expect_true(heap->core.live_block_slow_calls == slow_calls_before &&
                    heap->core.live_block_slow_misses == slow_misses_before &&
                    heap->core.live_block_tree_find_calls == tree_find_before,
                "live block diagnostics cross-owner skips slow lookup");
    expect_true(heap->core.live_block_slow_fallback_scans == fallback_before &&
                    heap->core.live_block_lookup_fallback_scans
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                        lookup_usable_fallback_before,
                "live block diagnostics cross-owner skips physical fallback");
    expect_true(heap->core.live_block_fast_terminal_misses ==
                    terminal_before + 1u,
                "live block diagnostics cross-owner terminal miss counted");
    expect_true(heap->core.live_block_fast_fail_header_page ==
                    fail_header_before + 1u,
                "live block diagnostics cross-owner header fail counted");
    expect_true(heap->core.live_block_lookup_fast_fail_reasons
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                        [UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE] ==
                    lookup_usable_fail_header_before + 1u,
                "live block diagnostics cross-owner callsite attributed");
    expect_ok(api->munmap(heap, cross_owner_map, page_size),
              "live block diagnostics cross-owner munmap");

    for (fallback_i = 0; fallback_i < fallback_pressure_count; ++fallback_i) {
        fallback_ptrs[fallback_i] = api->malloc(heap, large_size);
        expect_true(fallback_ptrs[fallback_i] != 0,
                    "live block diagnostics fallback pressure malloc");
    }
    fallback_target = fallback_ptrs[fallback_pressure_count - 1u];
    fallback_second_target = fallback_ptrs[fallback_pressure_count - 2u];
    fallback_block = test_find_block_by_payload(heap, fallback_target);
    fallback_second_block =
        test_find_block_by_payload(heap, fallback_second_target);
    expect_true(fallback_block != 0,
                "live block diagnostics fallback target block");
    expect_true(fallback_second_block != 0,
                "live block diagnostics second fallback target block");
    saved_fallback_desc = fallback_block->desc;
    saved_fallback_second_desc = fallback_second_block->desc;
    saved_live_tree_root = heap->core.live_block_tree_root;
    fallback_tree_misses_before = heap->core.live_block_slow_tree_misses;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    slow_hits_before = heap->core.live_block_slow_hits;
    heap->core.live_block_tree_root = 0;
    fallback_block->desc.magic ^= 0x44446666u;
    expect_true(api->malloc_usable_size(heap, fallback_target) >= large_size,
                "live block diagnostics descriptor hint usable");
    fallback_block->desc = saved_fallback_desc;
    heap->core.live_block_tree_root = saved_live_tree_root;
    expect_true(heap->core.live_block_slow_tree_misses >
                    fallback_tree_misses_before,
                "live block diagnostics descriptor hint after tree miss");
    expect_true(heap->core.live_block_slow_hits == slow_hits_before + 1u,
                "live block diagnostics descriptor hint hit counted");
    expect_true(heap->core.live_block_slow_fallback_scans == fallback_before,
                "live block diagnostics descriptor hint avoids fallback");

    fallback_tree_misses_before = heap->core.live_block_slow_tree_misses;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    slow_hits_before = heap->core.live_block_slow_hits;
    heap->core.live_block_tree_root = 0;
    fallback_block->desc.id = 0;
    expect_true(api->malloc_usable_size(heap, fallback_target) >= large_size,
                "live block diagnostics descriptor id drift hint usable");
    fallback_block->desc = saved_fallback_desc;
    heap->core.live_block_tree_root = saved_live_tree_root;
    expect_true(heap->core.live_block_slow_tree_misses >
                    fallback_tree_misses_before,
                "live block diagnostics descriptor id drift after tree miss");
    expect_true(heap->core.live_block_slow_hits == slow_hits_before + 1u,
                "live block diagnostics descriptor id drift hit counted");
    expect_true(heap->core.live_block_slow_fallback_scans == fallback_before,
                "live block diagnostics descriptor id drift avoids fallback");

    fallback_before = heap->core.live_block_slow_fallback_scans;
    tree_find_before = heap->core.live_block_tree_find_calls;
    fallback_tree_misses_before = heap->core.live_block_slow_tree_misses;
    fallback_misses_before = heap->core.live_block_slow_misses;
    fallback_scanned_before =
        heap->core.live_block_slow_fallback_scanned_nodes;
    fallback_max_scanned_before =
        heap->core.live_block_slow_fallback_max_scanned_nodes;
    fallback_budget_before =
        heap->core.live_block_slow_fallback_budget_exceeded;
    fallback_abort_before = heap->core.live_block_slow_fallback_aborts;
    fallback_negative_store_before =
        heap->core.live_block_slow_fallback_negative_stores;
    fallback_negative_hit_before =
        heap->core.live_block_slow_fallback_negative_hits;
    fallback_negative_eviction_before =
        heap->core.live_block_slow_fallback_negative_evictions;
    lookup_usable_fallback_before =
        heap->core.live_block_lookup_fallback_scans
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_fail_descriptor_before =
        heap->core.live_block_lookup_fast_fail_reasons
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
            [UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR];

    heap->core.live_block_tree_root = 0;
    fallback_block->desc.object = 0;
    fallback_second_block->desc.object = 0;
    expect_true(api->malloc_usable_size(heap, fallback_target) == 0,
                "live block diagnostics fallback hard limit rejects scan");
    expect_true(api->malloc_usable_size(heap, fallback_second_target) == 0,
                "live block diagnostics second fallback hard limit rejects scan");
    expect_true(api->malloc_usable_size(heap, fallback_target) == 0,
                "live block diagnostics multi negative cache rejects repeat");
    fallback_block->desc = saved_fallback_desc;
    fallback_second_block->desc = saved_fallback_second_desc;
    heap->core.live_block_tree_root = saved_live_tree_root;

    expect_true(heap->core.live_block_tree_find_calls ==
                    tree_find_before + 2u,
                "live block diagnostics negative repeat skips live tree");
    expect_true(heap->core.live_block_slow_tree_misses ==
                    fallback_tree_misses_before + 2u,
                "live block diagnostics fallback tree miss counted for distinct misses");
    expect_true(heap->core.live_block_slow_fallback_scans ==
                    fallback_before + 2u,
                "live block diagnostics fallback scans counted for distinct misses");
    expect_true(heap->core.live_block_lookup_fallback_scans
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    lookup_usable_fallback_before + 2u,
                "live block diagnostics fallback callsite counted");
    expect_true(heap->core.live_block_lookup_fast_fail_reasons
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                        [UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR] ==
                    lookup_usable_fail_descriptor_before + 3u,
                "live block diagnostics fallback descriptor fail callsite attributed");
    expect_true(heap->core.live_block_slow_misses ==
                    fallback_misses_before + 3u,
                "live block diagnostics fallback hard limit miss counted");
    expect_true(heap->core.live_block_slow_fallback_scanned_nodes >
                    fallback_scanned_before,
                "live block diagnostics fallback scanned nodes counted");
    expect_true(heap->core.live_block_slow_fallback_max_scanned_nodes >
                    fallback_max_scanned_before,
                "live block diagnostics fallback max scanned counted");
    expect_true(heap->core.live_block_slow_fallback_budget_exceeded ==
                    fallback_budget_before + 2u,
                "live block diagnostics fallback budget exceeded counted");
    expect_true(heap->core.live_block_slow_fallback_aborts ==
                    fallback_abort_before + 2u,
                "live block diagnostics fallback abort counted");
    expect_true((heap->core.live_block_health_flags &
                 UHM_LIVE_BLOCK_HEALTH_METADATA_DRIFT) != 0u,
                "live block diagnostics metadata drift health is sticky");
    expect_true(heap->core.live_block_slow_fallback_negative_stores ==
                    fallback_negative_store_before + 2u,
                "live block diagnostics fallback negative store counted");
    expect_true(heap->core.live_block_slow_fallback_negative_hits ==
                    fallback_negative_hit_before + 1u,
                "live block diagnostics fallback negative hit counted");
    expect_true(heap->core.live_block_slow_fallback_negative_evictions ==
                    fallback_negative_eviction_before,
                "live block diagnostics multi negative cache avoids eviction");

    /* A negative result from usable-size must not poison deferred-free. */
    negative_epoch_before_mutation =
        heap->core.live_block_tree_mutation_epoch;
    thread_ctx_epoch_ptr = api->malloc(heap, large_size);
    expect_true(thread_ctx_epoch_ptr != 0,
                "live block diagnostics negative cache epoch alloc");
    api->free(heap, thread_ctx_epoch_ptr);
    thread_ctx_epoch_ptr = 0;
    expect_true(heap->core.live_block_tree_mutation_epoch !=
                    negative_epoch_before_mutation,
                "live block diagnostics negative cache epoch advances");
    negative_usable_tag_seen = 0;
    for (negative_slot_i = 0;
         negative_slot_i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
         ++negative_slot_i) {
        if (heap->core.live_block_slow_fallback_negative_epochs[
                    negative_slot_i] ==
                ((negative_epoch_before_mutation <<
                  UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS) |
                 UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE) &&
            heap->core.live_block_slow_fallback_negative_payloads[
                    negative_slot_i] == (uintptr_t)fallback_target) {
            negative_usable_tag_seen = 1;
            break;
        }
    }
    expect_true(negative_usable_tag_seen,
                "live block diagnostics negative cache lazily invalidates");
    saved_live_tree_root = heap->core.live_block_tree_root;
    heap->core.live_block_tree_root = 0;
    fallback_block->desc.object = 0;
    fake_payload = (void *)((uintptr_t)fallback_target + UHM_MIN_ALIGNMENT);
    for (negative_slot_i = 0;
         negative_slot_i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
         ++negative_slot_i) {
        heap->core.live_block_slow_fallback_negative_epochs[
            negative_slot_i] =
            (negative_epoch_before_mutation <<
             UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS) |
            (uint64_t)(negative_slot_i %
                       UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT);
        heap->core.live_block_slow_fallback_negative_payloads[
            negative_slot_i] =
            (uintptr_t)fallback_ptrs[negative_slot_i] + UHM_MIN_ALIGNMENT;
    }
    heap->core.live_block_slow_fallback_negative_next = 0;
    fallback_negative_eviction_before =
        heap->core.live_block_slow_fallback_negative_evictions;
    negative_cross_callsite_hit_before =
        heap->core.live_block_slow_fallback_negative_hits;
    expect_true(api->malloc_usable_size(heap, fake_payload) == 0,
                "live block diagnostics usable negative cache seed");
    expect_true(heap->core.live_block_slow_fallback_negative_evictions ==
                    fallback_negative_eviction_before,
                "live block diagnostics stale negative slot avoids eviction");
    negative_usable_tag_seen = 0;
    for (negative_slot_i = 0;
         negative_slot_i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
         ++negative_slot_i) {
        if (heap->core.live_block_slow_fallback_negative_epochs[
                    negative_slot_i] ==
                ((heap->core.live_block_tree_mutation_epoch <<
                  UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS) |
                 UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE) &&
            heap->core.live_block_slow_fallback_negative_payloads[
                    negative_slot_i] == (uintptr_t)fake_payload) {
            negative_usable_tag_seen = 1;
            break;
        }
    }
    expect_true(negative_usable_tag_seen,
                "live block diagnostics negative cache stores usable tag");
    expect_true(api->free_deferred_ctx(heap, ctx, fake_payload) ==
                    UHM_ERR_CORRUPTION,
                "live block diagnostics deferred negative cache miss");
    negative_thread_ctx_tag_seen = 0;
    for (negative_slot_i = 0;
         negative_slot_i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
         ++negative_slot_i) {
        if (heap->core.live_block_slow_fallback_negative_epochs[
                    negative_slot_i] ==
                ((heap->core.live_block_tree_mutation_epoch <<
                  UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS) |
                 UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED) &&
            heap->core.live_block_slow_fallback_negative_payloads[
                    negative_slot_i] == (uintptr_t)fake_payload) {
            negative_thread_ctx_tag_seen = 1;
            break;
        }
    }
    expect_true(negative_thread_ctx_tag_seen,
                "live block diagnostics negative cache stores deferred tag");
    expect_true(heap->core.live_block_slow_fallback_negative_hits ==
                    negative_cross_callsite_hit_before,
                "live block diagnostics negative cache isolates callsites");
    fallback_block->desc = saved_fallback_desc;
    heap->core.live_block_tree_root = saved_live_tree_root;

    saved_live_tree_root = heap->core.live_block_tree_root;
    heap->core.live_block_tree_root = 0;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    fallback_throttle_before = heap->core.live_block_slow_fallback_throttles;
    fallback_scenario_check_before =
        heap->core.live_block_slow_fallback_scenario_checks;
    fallback_scenario_throttle_before =
        heap->core.live_block_slow_fallback_scenario_throttles;
    fallback_scenario_candidate_before =
        heap->core.live_block_slow_fallback_scenario_candidates
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_fallback_before =
        heap->core.live_block_lookup_fallback_scans
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    lookup_usable_throttle_before =
        heap->core.live_block_lookup_throttles
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE];
    fallback_throttle_seen = 0;
    for (fallback_i = 0; fallback_i < fallback_pressure_count; ++fallback_i) {
        fallback_loop_block =
            test_find_block_by_payload(heap, fallback_ptrs[fallback_i]);
        expect_true(fallback_loop_block != 0,
                    "live block diagnostics throttle block lookup");
        saved_fallback_loop_desc = fallback_loop_block->desc;
        fallback_loop_block->desc.object = 0;
        (void)api->malloc_usable_size(heap, fallback_ptrs[fallback_i]);
        fallback_loop_block->desc = saved_fallback_loop_desc;
        if (heap->core.live_block_slow_fallback_throttles >
            fallback_throttle_before) {
            fallback_throttle_seen = 1;
            break;
        }
    }
    expect_true(fallback_throttle_seen,
                "live block diagnostics fallback throttle triggers");
    expect_true(fallback_i + 1u < fallback_pressure_count,
                "live block diagnostics throttle leaves probe pointer");
    fallback_throttle_after_first =
        heap->core.live_block_slow_fallback_throttles;
    fallback_scenario_throttle_after_first =
        heap->core.live_block_slow_fallback_scenario_throttles;
    fallback_scan_after_throttle =
        heap->core.live_block_slow_fallback_scans;
    ++fallback_i;
    fallback_loop_block =
        test_find_block_by_payload(heap, fallback_ptrs[fallback_i]);
    expect_true(fallback_loop_block != 0,
                "live block diagnostics post-throttle block lookup");
    saved_fallback_loop_desc = fallback_loop_block->desc;
    fallback_loop_block->desc.object = 0;
    (void)api->malloc_usable_size(heap, fallback_ptrs[fallback_i]);
    fallback_loop_block->desc = saved_fallback_loop_desc;
    heap->core.live_block_tree_root = saved_live_tree_root;
    expect_true(heap->core.live_block_slow_fallback_throttles ==
                    fallback_throttle_after_first + 1u,
                "live block diagnostics repeated fallback throttle counted");
    expect_true(heap->core.live_block_slow_fallback_scenario_throttles ==
                    fallback_scenario_throttle_after_first + 1u,
                "live block diagnostics repeated scenario throttle counted");
    expect_true(fallback_scenario_throttle_after_first >
                    fallback_scenario_throttle_before,
                "live block diagnostics scenario throttle triggers first");
    expect_true(
        (heap->core.live_block_slow_fallback_scenario_throttled_mask &
         (uint32_t)(1u << UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE)) != 0u,
        "live block diagnostics scenario throttle is sticky per callsite");
    expect_true(heap->core.live_block_slow_fallback_scenario_checks >
                    fallback_scenario_check_before,
                "live block diagnostics scenario checks counted");
    expect_true(heap->core.live_block_slow_fallback_scenario_candidates
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] >
                    fallback_scenario_candidate_before,
                "live block diagnostics scenario candidates counted");
    expect_true(heap->core.live_block_lookup_throttles
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] ==
                    lookup_usable_throttle_before + 2u,
                "live block diagnostics throttle callsite counted");
    expect_true(heap->core.live_block_slow_fallback_scans ==
                    fallback_scan_after_throttle,
                "live block diagnostics throttle skips physical scan");
    expect_true(heap->core.live_block_slow_fallback_scans >
                    fallback_before,
                "live block diagnostics throttle allows initial scans");
    expect_true(heap->core.live_block_lookup_fallback_scans
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] >
                    lookup_usable_fallback_before,
                "live block diagnostics throttle preserves initial fallback callsite");
    expect_true(heap->core.live_block_slow_fallback_scenario_candidates
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] == 0,
                "live block diagnostics scenario gate isolates free candidate window");

    lookup_free_calls_before =
        heap->core
            .live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE];
    lookup_free_throttle_before =
        heap->core.live_block_lookup_throttles
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE];
    lookup_realloc_calls_before =
        heap->core
            .live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC];
    api->free(heap, fallback_ptrs[0]);
    fallback_ptrs[0] = 0;
    expect_true(
        (heap->core.live_block_slow_fallback_scenario_throttled_mask &
         (uint32_t)(1u << UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE)) == 0u &&
            (heap->core.live_block_slow_fallback_scenario_throttled_mask &
             (uint32_t)(1u << UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE)) ==
                0u,
        "live block diagnostics scenario throttle resets after mutation");
    expect_true(heap->core.live_block_lookup_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] ==
                    lookup_free_calls_before + 1u,
                "live block diagnostics free callsite counted");
    expect_true(heap->core.live_block_lookup_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC] ==
                    lookup_realloc_calls_before,
                "live block diagnostics free does not count realloc callsite");
    expect_true(heap->core.live_block_slow_fallback_scenario_candidates
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] ==
                    0,
                "live block diagnostics free scenario candidate unchanged on fast free");
    expect_true(heap->core.live_block_lookup_throttles
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] ==
                    lookup_free_throttle_before,
                "live block diagnostics free not throttled by usable storm");

    thread_ctx_epoch_ptr = api->malloc(heap, large_size);
    expect_true(thread_ctx_epoch_ptr != 0,
                "live block diagnostics thread ctx epoch alloc");
    api->free(heap, thread_ctx_epoch_ptr);

    saved_live_tree_root = heap->core.live_block_tree_root;
    heap->core.live_block_tree_root = 0;
    lookup_thread_ctx_fallback_before =
        heap->core.live_block_lookup_fallback_scans
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED];
    lookup_thread_ctx_throttle_before =
        heap->core.live_block_lookup_throttles
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED];
    thread_ctx_scenario_candidate_before =
        heap->core.live_block_slow_fallback_scenario_candidates
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED];
    thread_ctx_scenario_throttle_before =
        heap->core.live_block_slow_fallback_scenario_throttles;
    thread_ctx_throttle_seen = 0;
    thread_ctx_i = fallback_i + 1u;
    for (; thread_ctx_i < fallback_pressure_count; ++thread_ctx_i) {
        fallback_loop_block =
            test_find_block_by_payload(heap, fallback_ptrs[thread_ctx_i]);
        expect_true(fallback_loop_block != 0,
                    "live block diagnostics thread ctx block lookup");
        saved_fallback_loop_desc = fallback_loop_block->desc;
        fallback_loop_block->desc.object = 0;
        thread_ctx_fake_payload =
            (void *)((uintptr_t)fallback_ptrs[thread_ctx_i] +
                     UHM_MIN_ALIGNMENT);
        expect_true(api->free_deferred_ctx(heap, ctx,
                                           thread_ctx_fake_payload) ==
                        UHM_ERR_CORRUPTION,
                    "live block diagnostics thread ctx rejects corrupt ptr");
        fallback_loop_block->desc = saved_fallback_loop_desc;
        if (heap->core.live_block_slow_fallback_scenario_throttles >
            thread_ctx_scenario_throttle_before) {
            thread_ctx_throttle_seen = 1;
            break;
        }
    }
    expect_true(thread_ctx_throttle_seen,
                "live block diagnostics thread ctx scenario throttle triggers");
    expect_true(thread_ctx_i + 1u < fallback_pressure_count,
                "live block diagnostics thread ctx leaves probe pointer");
    thread_ctx_scenario_throttle_after_first =
        heap->core.live_block_slow_fallback_scenario_throttles;
    thread_ctx_scan_after_throttle =
        heap->core.live_block_slow_fallback_scans;
    ++thread_ctx_i;
    fallback_loop_block =
        test_find_block_by_payload(heap, fallback_ptrs[thread_ctx_i]);
    expect_true(fallback_loop_block != 0,
                "live block diagnostics thread ctx post-throttle block lookup");
    saved_fallback_loop_desc = fallback_loop_block->desc;
    fallback_loop_block->desc.object = 0;
    thread_ctx_fake_payload =
        (void *)((uintptr_t)fallback_ptrs[thread_ctx_i] + UHM_MIN_ALIGNMENT);
    expect_true(api->free_deferred_ctx(heap, ctx,
                                       thread_ctx_fake_payload) ==
                    UHM_ERR_CORRUPTION,
                "live block diagnostics thread ctx post-throttle rejects");
    fallback_loop_block->desc = saved_fallback_loop_desc;
    heap->core.live_block_tree_root = saved_live_tree_root;
    expect_true(heap->core.live_block_slow_fallback_scenario_throttles ==
                    thread_ctx_scenario_throttle_after_first + 1u,
                "live block diagnostics thread ctx repeated scenario throttle counted");
    expect_true(heap->core.live_block_slow_fallback_scenario_candidates
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED] >
                    thread_ctx_scenario_candidate_before,
                "live block diagnostics thread ctx scenario candidates counted");
    expect_true(heap->core.live_block_lookup_throttles
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED] >
                    lookup_thread_ctx_throttle_before,
                "live block diagnostics thread ctx throttle callsite counted");
    expect_true(heap->core.live_block_slow_fallback_scans ==
                    thread_ctx_scan_after_throttle,
                "live block diagnostics thread ctx throttle skips physical scan");
    expect_true(heap->core.live_block_lookup_fallback_scans
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED] >
                    lookup_thread_ctx_fallback_before,
                "live block diagnostics thread ctx preserves initial fallback callsite");
    expect_true(heap->core.live_block_slow_fallback_scenario_candidates
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] == 0,
                "live block diagnostics thread ctx epoch isolates usable window");

    /* Ordinary epochs invalidate lazily; the finite key space still needs a
       physical sweep when the packed epoch wraps. */
    heap->core.live_block_tree_mutation_epoch =
        UHM_LIVE_BLOCK_NEGATIVE_CACHE_EPOCH_MAX;
    thread_ctx_epoch_ptr = api->malloc(heap, large_size);
    expect_true(thread_ctx_epoch_ptr != 0,
                "live block diagnostics negative cache wrap alloc");
    negative_cache_empty = 1;
    for (negative_slot_i = 0;
         negative_slot_i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
         ++negative_slot_i) {
        if (heap->core.live_block_slow_fallback_negative_epochs[
                negative_slot_i] != 0) {
            negative_cache_empty = 0;
            break;
        }
    }
    expect_true(negative_cache_empty,
                "live block diagnostics negative cache wrap clears slots");
    api->free(heap, thread_ctx_epoch_ptr);
    thread_ctx_epoch_ptr = 0;

    for (fallback_i = 1; fallback_i < fallback_pressure_count; ++fallback_i) {
        api->free(heap, fallback_ptrs[fallback_i]);
    }
    api->free(heap, other_descriptor_ptr);
    expect_ok(api->munmap(heap, descriptor_owner_map, page_size),
              "live block descriptor drift mmap owner munmap");
    api->free(heap, ptr);
    expect_true(heap->core.live_block_tree_current_nodes == 0u,
                "live block diagnostics live tree removes free block");
    api->thread_ctx_close(heap, ctx);
    ctx = 0;
    expect_ok(api->check(heap, 0), "live block diagnostics final check");
    expect_ok(api->close(heap), "live block diagnostics close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_live_block_health_fail_fast_skips_physical_fallback(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 512u * 1024u;
    const size_t large_size = page_size * 2u + 128u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_block *block;
    uhm_block *saved_live_tree_root;
    uhm_extent_descriptor saved_desc;
    uint64_t fallback_before;
    uint64_t misses_before;
    uint64_t health_before;
    uhm_result last_error = UHM_OK;
    uint32_t platform_error = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live block health fail-fast heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live block health fail-fast arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live block health fail-fast usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_FAIL_ON_LIVE_BLOCK_HEALTH;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "live block health fail-fast heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live block health fail-fast api");

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "live block health fail-fast malloc");
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "live block health fail-fast block lookup");
    expect_true(heap->core.live_block_tree_root != 0,
                "live block health fail-fast live tree populated");

    saved_live_tree_root = heap->core.live_block_tree_root;
    saved_desc = block->desc;
    health_before = heap->core.live_block_health_flags;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    misses_before = heap->core.live_block_slow_misses;

    heap->core.live_block_tree_root = 0;
    block->desc.object = 0;
    expect_true(api->malloc_usable_size(heap, ptr) == 0u,
                "live block health fail-fast rejects drift immediately");
    expect_true((heap->core.live_block_health_flags &
                 UHM_LIVE_BLOCK_HEALTH_METADATA_DRIFT) != 0u,
                "live block health fail-fast marks metadata drift");
    expect_true(heap->core.live_block_slow_fallback_scans == fallback_before,
                "live block health fail-fast skips physical fallback");
    expect_true(heap->core.live_block_slow_misses == misses_before + 1u,
                "live block health fail-fast counts slow miss");
    expect_ok(api->get_last_error(heap, &last_error, &platform_error),
              "live block health fail-fast last error query");
    expect_true(last_error == UHM_ERR_CORRUPTION,
                "live block health fail-fast reports corruption");

    heap->core.live_block_tree_root = saved_live_tree_root;
    block->desc = saved_desc;
    heap->core.live_block_health_flags = health_before;
    api->free(heap, ptr);
    expect_ok(api->check(heap, 0), "live block health fail-fast final check");
    expect_ok(api->close(heap), "live block health fail-fast close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_malloc_block_priority_lifecycle_lazy_init(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 512u * 1024u;
    const size_t large_size = page_size * 2u + 128u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uint64_t live_mix_before;
    uint64_t live_mix_after_malloc;
    uint64_t free_mix_before;
    uint64_t free_mix_after_malloc;
    uint64_t free_mix_after_free;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "priority lazy heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "priority lazy arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "priority lazy usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "priority lazy heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "priority lazy api");
    uhm_spinlock_set_diagnostics_enabled(&heap->lock, 1u);
    uhm_heap_set_diagnostics_timing_enabled(heap, 1u);

    live_mix_before = heap->core.live_block_tree_priority_mix_calls;
    free_mix_before = heap->core.malloc_free_tree_priority_mix_calls;
    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "priority lazy large malloc");
    live_mix_after_malloc = heap->core.live_block_tree_priority_mix_calls;
    free_mix_after_malloc = heap->core.malloc_free_tree_priority_mix_calls;
    expect_true(live_mix_after_malloc > live_mix_before,
                "priority lazy live priority initialized for new live block");
    expect_true(free_mix_after_malloc == free_mix_before,
                "priority lazy new live block skips free-tree priority");

    api->free(heap, ptr);
    free_mix_after_free = heap->core.malloc_free_tree_priority_mix_calls;
    expect_true(free_mix_after_free > free_mix_after_malloc,
                "priority lazy free block initializes free-tree priority");
    expect_ok(api->check(heap, 0), "priority lazy final check");
    expect_ok(api->close(heap), "priority lazy close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_live_block_realloc_split_merge_fast_lookup(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    const size_t first_size = 2u * page_size + 128u;
    const size_t second_size = 12u * page_size + 128u;
    const size_t guard_size = 2u * page_size + 128u;
    const size_t grow_size = first_size + 3u * page_size;
    const size_t shrink_size = first_size + 64u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *first;
    void *second;
    void *guard;
    unsigned char *grown;
    unsigned char *shrunk;
    uhm_block *first_block;
    uhm_block *second_block;
    uhm_block *guard_block;
    uhm_block *remainder;
    test_live_lookup_counters before;
    test_live_lookup_counters after;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live realloc fast heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live realloc fast arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live realloc fast usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "live realloc fast heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live realloc fast api");

    first = (unsigned char *)api->malloc(heap, first_size);
    second = api->malloc(heap, second_size);
    guard = api->malloc(heap, guard_size);
    expect_true(first != 0 && second != 0 && guard != 0,
                "live realloc fast adjacent allocations");
    first_block = test_find_block_by_payload(heap, first);
    second_block = test_find_block_by_payload(heap, second);
    guard_block = test_find_block_by_payload(heap, guard);
    expect_true(first_block != 0 && second_block != 0 && guard_block != 0,
                "live realloc fast block lookup");
    expect_true(first_block->next_phys == second_block &&
                    second_block->next_phys == guard_block,
                "live realloc fast physical order");
    memset(first, 0x31, first_size);
    memset(second, 0x32, second_size);
    memset(guard, 0x33, guard_size);

    api->free(heap, second);
    expect_true((second_block->flags & UHM_BLOCK_FLAG_FREE) != 0,
                "live realloc fast right neighbor freed");
    expect_ok(api->check(heap, 0), "live realloc fast check before grow");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    grown = (unsigned char *)api->realloc(heap, first, grow_size);
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    expect_true(grown == first, "live realloc fast grow in place");
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc fast grow callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live realloc fast grow hits fast path");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.callsite_slow_calls == before.callsite_slow_calls,
                "live realloc fast grow avoids slow lookup");
    expect_true(after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live realloc fast grow avoids tree and physical fallback");
    first_block = test_find_block_by_payload(heap, grown);
    expect_true(first_block != 0 &&
                    first_block->requested_size == grow_size,
                "live realloc fast grown block tracked");
    remainder = first_block->next_phys;
    expect_true(remainder != 0 &&
                    (remainder->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                    remainder->next_phys == guard_block,
                "live realloc fast grow split remainder");
    expect_ok(api->check(heap, 0), "live realloc fast check after grow");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, grown) >= grow_size,
                "live realloc fast grown usable");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc fast grown usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live realloc fast grown usable hits fast path");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans,
                "live realloc fast grown usable avoids fallback");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    shrunk = (unsigned char *)api->realloc(heap, grown, shrink_size);
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    expect_true(shrunk == grown, "live realloc fast shrink in place");
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc fast shrink callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live realloc fast shrink hits fast path");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.callsite_slow_calls == before.callsite_slow_calls,
                "live realloc fast shrink avoids slow lookup");
    expect_true(after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans,
                "live realloc fast shrink avoids tree and physical fallback");
    first_block = test_find_block_by_payload(heap, shrunk);
    expect_true(first_block != 0 &&
                    first_block->requested_size == shrink_size,
                "live realloc fast shrunk block tracked");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, shrunk) >= shrink_size,
                "live realloc fast shrunk usable");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc fast shrunk usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live realloc fast shrunk usable hits fast path");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans,
                "live realloc fast shrunk usable avoids fallback");

    api->free(heap, shrunk);
    api->free(heap, guard);
    expect_ok(api->check(heap, 0), "live realloc fast final check");
    expect_ok(api->close(heap), "live realloc fast close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_live_block_realloc_move_failure_keeps_old_fast_lookup(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    const size_t first_size = 4u * page_size;
    const size_t guard_size = 4u * page_size;
    const size_t grow_size = 12u * page_size;
    const uint32_t platform_error = test_platform_error_code();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *first;
    void *guard;
    void *moved;
    uhm_block *first_block;
    uhm_block *guard_block;
    uhm_stats before_fail;
    uhm_stats after_fail;
    int commit_before_fail;
    test_live_lookup_counters before;
    test_live_lookup_counters after;
    size_t i;

    memset(&state, 0, sizeof(state));
    state.platform_error = platform_error;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live realloc move rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live realloc move rollback arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live realloc move rollback usermode vm init");
    vm.user = &state;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    vm.protect = fault_vm_protect;
    vm.fixed_remap = fault_vm_fixed_remap;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "live realloc move rollback heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live realloc move rollback api");

    first = (unsigned char *)api->malloc(heap, first_size);
    guard = api->malloc(heap, guard_size);
    expect_true(first != 0 && guard != 0,
                "live realloc move rollback adjacent allocations");
    first_block = test_find_block_by_payload(heap, first);
    guard_block = test_find_block_by_payload(heap, guard);
    expect_true(first_block != 0 && guard_block != 0 &&
                    first_block->next_phys == guard_block,
                "live realloc move rollback physical guard");
    for (i = 0; i < first_size; ++i) {
        first[i] = (unsigned char)(0xa0u + (unsigned char)(i & 0x1fu));
    }
    expect_ok(api->check(heap, 0),
              "live realloc move rollback check before failure");

    memset(&before_fail, 0, sizeof(before_fail));
    before_fail.size = sizeof(before_fail);
    expect_ok(api->get_stats(heap, &before_fail),
              "live realloc move rollback stats before failure");
    commit_before_fail = state.commit_count;
    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    state.fail_commit_once = 1;
    moved = api->realloc(heap, first, grow_size);
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    expect_true(moved == 0, "live realloc move rollback failed realloc");
    expect_true(state.commit_count == commit_before_fail + 1,
                "live realloc move rollback attempted move commit");
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "live realloc move rollback captures platform error");
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc move rollback realloc callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live realloc move rollback realloc lookup stays fast");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.callsite_slow_calls == before.callsite_slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live realloc move rollback realloc avoids fallback");

    memset(&after_fail, 0, sizeof(after_fail));
    after_fail.size = sizeof(after_fail);
    expect_ok(api->get_stats(heap, &after_fail),
              "live realloc move rollback stats after failure");
    expect_true(after_fail.malloc_live_bytes == before_fail.malloc_live_bytes,
                "live realloc move rollback keeps live bytes");
    expect_true(after_fail.committed_bytes == before_fail.committed_bytes,
                "live realloc move rollback keeps committed bytes");
    for (i = 0; i < first_size; ++i) {
        expect_true(first[i] ==
                        (unsigned char)(0xa0u + (unsigned char)(i & 0x1fu)),
                    "live realloc move rollback preserves old payload");
    }
    first_block = test_find_block_by_payload(heap, first);
    expect_true(first_block != 0 &&
                    (first_block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
                    first_block->requested_size == first_size &&
                    first_block->next_phys == guard_block,
                "live realloc move rollback old block still live");
    expect_ok(api->check(heap, 0),
              "live realloc move rollback check after failure");
    test_shadow_model_check(heap, "live realloc move rollback shadow after failure");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, first) >= first_size,
                "live realloc move rollback old usable");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc move rollback usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live realloc move rollback usable stays fast");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live realloc move rollback usable avoids fallback");

    api->free(heap, first);
    api->free(heap, guard);
    expect_ok(api->check(heap, 0), "live realloc move rollback final check");
    expect_ok(api->close(heap), "live realloc move rollback close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_live_block_realloc_move_success_reuse_fast_lookup(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    const size_t first_size = 4u * page_size;
    const size_t guard_size = 4u * page_size;
    const size_t grow_size = 12u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *first;
    void *guard;
    unsigned char *moved;
    void *reuse;
    uhm_block *first_block;
    uhm_block *guard_block;
    uhm_block *moved_block;
    test_live_lookup_counters before;
    test_live_lookup_counters after;
    test_live_lookup_counters free_before;
    test_live_lookup_counters free_after;
    size_t i;

    memset(&state, 0, sizeof(state));
    state.platform_error = test_platform_error_code();
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live realloc move success heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live realloc move success arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live realloc move success usermode vm init");
    vm.user = &state;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    vm.protect = fault_vm_protect;
    vm.fixed_remap = fault_vm_fixed_remap;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "live realloc move success heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live realloc move success api");

    first = (unsigned char *)api->malloc(heap, first_size);
    guard = api->malloc(heap, guard_size);
    expect_true(first != 0 && guard != 0,
                "live realloc move success adjacent allocations");
    first_block = test_find_block_by_payload(heap, first);
    guard_block = test_find_block_by_payload(heap, guard);
    expect_true(first_block != 0 && guard_block != 0 &&
                    first_block->next_phys == guard_block,
                "live realloc move success physical guard");
    for (i = 0; i < first_size; ++i) {
        first[i] = (unsigned char)(0xc0u + (unsigned char)(i & 0x3fu));
    }
    expect_ok(api->check(heap, 0),
              "live realloc move success check before move");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    free_before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE);
    moved = (unsigned char *)api->realloc(heap, first, grow_size);
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    free_after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE);
    expect_true(moved != 0, "live realloc move success result");
    expect_true(moved != first, "live realloc move success moves block");
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc move success realloc callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 2u,
                "live realloc move success realloc and old free stay fast");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.callsite_slow_calls == before.callsite_slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live realloc move success realloc avoids fallback");
    expect_true(free_after.callsite_calls == free_before.callsite_calls + 1u,
                "live realloc move success old free callsite counted");
    expect_true(free_after.slow_calls == free_before.slow_calls &&
                    free_after.tree_find_calls == free_before.tree_find_calls &&
                    free_after.fallback_scans == free_before.fallback_scans &&
                    free_after.callsite_fallback_scans ==
                        free_before.callsite_fallback_scans &&
                    free_after.callsite_throttles ==
                        free_before.callsite_throttles,
                "live realloc move success old free avoids fallback");
    for (i = 0; i < first_size; ++i) {
        expect_true(moved[i] ==
                        (unsigned char)(0xc0u + (unsigned char)(i & 0x3fu)),
                    "live realloc move success preserves payload");
    }

    moved_block = test_find_block_by_payload(heap, moved);
    expect_true(moved_block != 0 &&
                    (moved_block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
                    moved_block->requested_size == grow_size,
                "live realloc move success new block live");
    expect_true(first_block->magic == UHM_BLOCK_MAGIC &&
                    (first_block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                    first_block->requested_size == 0 &&
                    first_block->next_phys == guard_block,
                "live realloc move success old block free");
    expect_ok(api->check(heap, 0),
              "live realloc move success check after move");
    test_shadow_model_check(heap, "live realloc move success shadow after move");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, moved) >= grow_size,
                "live realloc move success new usable");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc move success new usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live realloc move success new usable stays fast");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live realloc move success new usable avoids fallback");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, first) == 0u,
                "live realloc move success old payload not live");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc move success old miss callsite counted");
    expect_true(after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live realloc move success old miss avoids physical fallback");

    reuse = api->malloc(heap, first_size);
    expect_true(reuse == first, "live realloc move success old block reused");
    expect_true((first_block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
                    first_block->requested_size == first_size,
                "live realloc move success reused block live");
    expect_ok(api->check(heap, 0),
              "live realloc move success check after reuse");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, reuse) >= first_size,
                "live realloc move success reused usable");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live realloc move success reused usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live realloc move success reused usable stays fast");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live realloc move success reused usable avoids fallback");

    api->free(heap, reuse);
    api->free(heap, moved);
    api->free(heap, guard);
    expect_ok(api->check(heap, 0), "live realloc move success final check");
    expect_ok(api->close(heap), "live realloc move success close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_live_block_generation_drift_no_physical_fallback(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    const size_t large_size = 3u * page_size + 128u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_block *block;
    uint64_t saved_desc_generation;
    uhm_extent_descriptor saved_desc;
    uint64_t fail_descriptor_before;
    uint64_t lookup_usable_fail_descriptor_before;
    size_t descriptor_fault_i;
    test_live_lookup_counters before;
    test_live_lookup_counters after;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live generation drift heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live generation drift arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live generation drift usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "live generation drift heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live generation drift api");

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "live generation drift malloc");
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "live generation drift block lookup");
    expect_ok(api->check(heap, 0), "live generation drift baseline check");

    saved_desc_generation = block->desc.generation;
    block->desc.generation = saved_desc_generation + 17u;
    if (block->desc.generation == saved_desc_generation) {
        ++block->desc.generation;
    }
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "live generation drift check catches descriptor generation");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, ptr) >= large_size,
                "live generation drift usable still resolves payload");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live generation drift usable callsite counted");
    expect_true(after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live generation drift avoids physical fallback");
    expect_true(after.slow_calls == before.slow_calls ||
                    after.slow_calls == before.slow_calls + 1u,
                "live generation drift slow path bounded");
    expect_true(after.tree_find_calls == before.tree_find_calls ||
                    after.tree_find_calls == before.tree_find_calls + 1u,
                "live generation drift tree lookup bounded");

    block->desc.generation = saved_desc_generation;
    expect_ok(api->check(heap, 0), "live generation drift restored check");

    saved_desc = block->desc;
    for (descriptor_fault_i = 0; descriptor_fault_i < 3u;
         ++descriptor_fault_i) {
        block->desc = saved_desc;
        switch (descriptor_fault_i) {
        case 0:
            block->desc.owner = UHM_PAGE_OWNER_MMAP;
            break;
        case 1:
            block->desc.kind = UHM_EXTENT_KIND_MMAP;
            break;
        default:
            block->desc.id = saved_desc.id + UHM_MIN_ALIGNMENT;
            if (block->desc.id == saved_desc.id) {
                ++block->desc.id;
            }
            break;
        }
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "live descriptor field drift check catches corruption");

        before = test_live_lookup_counters_take(
            heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
        fail_descriptor_before = heap->core.live_block_fast_fail_descriptor;
        lookup_usable_fail_descriptor_before =
            heap->core.live_block_lookup_fast_fail_reasons
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                [UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR];
        expect_true(api->malloc_usable_size(heap, ptr) >= large_size,
                    "live descriptor field drift usable resolves payload");
        after = test_live_lookup_counters_take(
            heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
        expect_true(after.callsite_calls == before.callsite_calls + 1u,
                    "live descriptor field drift callsite counted");
        expect_true(after.fast_hits == before.fast_hits,
                    "live descriptor field drift leaves fast path");
        expect_true(after.slow_calls == before.slow_calls + 1u &&
                        after.callsite_slow_calls ==
                            before.callsite_slow_calls + 1u &&
                        after.tree_find_calls == before.tree_find_calls + 1u,
                    "live descriptor field drift uses bounded live tree");
        expect_true(after.fallback_scans == before.fallback_scans &&
                        after.callsite_fallback_scans ==
                            before.callsite_fallback_scans &&
                        after.callsite_throttles ==
                            before.callsite_throttles,
                    "live descriptor field drift avoids physical fallback");
        expect_true(heap->core.live_block_fast_fail_descriptor ==
                        fail_descriptor_before + 1u,
                    "live descriptor field drift descriptor fail counted");
        expect_true(heap->core.live_block_lookup_fast_fail_reasons
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                            [UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR] ==
                        lookup_usable_fail_descriptor_before + 1u,
                    "live descriptor field drift callsite attributed");
    }
    block->desc = saved_desc;
    expect_ok(api->check(heap, 0),
              "live descriptor field drift restored check");

    api->free(heap, ptr);
    expect_ok(api->check(heap, 0), "live generation drift final check");
    expect_ok(api->close(heap), "live generation drift close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_page_private_page_map_generation_drift_fast_lookup(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    const size_t alloc_size = 2u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_block *block;
    size_t first_page = 0;
    size_t page_count = 0;
    uhm_page_record *record;
    uhm_page_record saved_record;
    uint64_t saved_generation;
    uint64_t drift_generation;
    test_live_lookup_counters before;
    test_live_lookup_counters after;
    uint64_t descriptor_hits_before;
    uint64_t scan_fallbacks_before;
    uint64_t terminal_before;
    uint64_t fail_payload_before;
    uint64_t lookup_usable_fail_payload_before;
    size_t page_record_fault_i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "page-map generation drift heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "page-map generation drift arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "page-map generation drift usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "page-map generation drift heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "page-map generation drift api");

    ptr = api->aligned_alloc(heap, page_size, alloc_size);
    expect_true(ptr != 0, "page-map generation drift page-private alloc");
    expect_true(((uintptr_t)ptr % page_size) == 0,
                "page-map generation drift payload page aligned");
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0 &&
                    (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) != 0,
                "page-map generation drift block is page private");
    expect_ok(api->mprotect(heap, ptr, page_size, UHM_PROT_NONE),
              "page-map generation drift protect none");
    expect_true(test_shadow_page_span(heap, (uintptr_t)ptr, page_size,
                                      &first_page, &page_count) &&
                    page_count == 1u,
                "page-map generation drift payload page span");
    record = test_page_record_at(heap, first_page);
    expect_true(record != 0 &&
                    record->descriptor_id == block->desc.id &&
                    record->owner == UHM_PAGE_OWNER_MALLOC &&
                    record->va_state == UHM_PAGE_VA_MALLOC_LIVE &&
                    record->commit_state == UHM_PAGE_COMMIT_LIVE,
                "page-map generation drift record targets block");

    saved_record = *record;
    saved_generation = record->generation;
    drift_generation = saved_generation + UINT64_C(313);
    if (drift_generation == saved_generation) {
        ++drift_generation;
    }
    record->generation = drift_generation;

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, ptr) >= alloc_size,
                "page-map generation drift usable resolves payload");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "page-map generation drift usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "page-map generation drift usable stays fast");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "page-map generation drift avoids slow and fallback");

    descriptor_hits_before =
        heap->core.mprotect_page_private_descriptor_hits;
    scan_fallbacks_before =
        heap->core.mprotect_page_private_scan_fallbacks;
    expect_ok(api->mprotect(heap, ptr, page_size, UHM_PROT_NONE),
              "page-map generation drift mprotect no-op");
    expect_true(heap->core.mprotect_page_private_descriptor_hits ==
                    descriptor_hits_before + 1u &&
                    heap->core.mprotect_page_private_scan_fallbacks ==
                        scan_fallbacks_before,
                "page-map generation drift mprotect avoids scan fallback");

    record = test_page_record_at(heap, first_page);
    expect_true(record != 0, "page-map generation drift record restore lookup");
    *record = saved_record;

    for (page_record_fault_i = 0;
         page_record_fault_i <
         sizeof(test_live_page_record_faults) /
             sizeof(test_live_page_record_faults[0]);
         ++page_record_fault_i) {
        *record = saved_record;
        record->owner =
            test_live_page_record_faults[page_record_fault_i].owner;
        record->va_state =
            test_live_page_record_faults[page_record_fault_i].va_state;
        record->commit_state =
            test_live_page_record_faults[page_record_fault_i].commit_state;

        before = test_live_lookup_counters_take(
            heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
        terminal_before = heap->core.live_block_fast_terminal_misses;
        fail_payload_before = heap->core.live_block_fast_fail_payload_page;
        lookup_usable_fail_payload_before =
            heap->core.live_block_lookup_fast_fail_reasons
                [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                [UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE];
        expect_true(api->malloc_usable_size(heap, ptr) == 0u,
                    "page-private page record fault rejected");
        after = test_live_lookup_counters_take(
            heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
        expect_true(after.callsite_calls == before.callsite_calls + 1u,
                    "page-private page record fault callsite counted");
        expect_true(after.fast_hits == before.fast_hits,
                    "page-private page record fault no fast hit");
        expect_true(after.slow_calls == before.slow_calls &&
                        after.callsite_slow_calls ==
                            before.callsite_slow_calls &&
                        after.tree_find_calls == before.tree_find_calls,
                    "page-private page record fault skips slow lookup");
        expect_true(after.fallback_scans == before.fallback_scans &&
                        after.callsite_fallback_scans ==
                            before.callsite_fallback_scans &&
                        after.callsite_throttles ==
                            before.callsite_throttles,
                    "page-private page record fault avoids fallback");
        expect_true(heap->core.live_block_fast_terminal_misses ==
                        terminal_before + 1u,
                    "page-private page record fault terminal counted");
        expect_true(heap->core.live_block_fast_fail_payload_page ==
                        fail_payload_before + 1u,
                    "page-private page record fault payload fail counted");
        expect_true(heap->core.live_block_lookup_fast_fail_reasons
                            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]
                            [UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE] ==
                        lookup_usable_fail_payload_before + 1u,
                    "page-private page record fault callsite attributed");
    }
    *record = saved_record;

    expect_ok(api->mprotect(heap, ptr, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "page-map generation drift restore rw");
    expect_ok(api->check(heap, 0), "page-map generation drift restored check");
    api->free(heap, ptr);
    expect_ok(api->check(heap, 0), "page-map generation drift final check");
    expect_ok(api->close(heap), "page-map generation drift close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_live_block_recommit_reuse_fast_lookup(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    const size_t large_size = 12u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *reuse;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    int commit_before_reuse;
    int decommit_before_free;
    test_live_lookup_counters before;
    test_live_lookup_counters after;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live recommit fast heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live recommit fast arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live recommit fast usermode vm init");
    vm.user = &state;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    vm.protect = fault_vm_protect;
    vm.fixed_remap = fault_vm_fixed_remap;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "live recommit fast heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live recommit fast api");

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "live recommit fast malloc");
    memset(ptr, 0x4bu, large_size);
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "live recommit fast block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size) &&
                    decommit_size != 0,
                "live recommit fast has decommit range");
    expect_ok(api->check(heap, 0), "live recommit fast check before free");

    decommit_before_free = state.decommit_count;
    api->free(heap, ptr);
    expect_true(state.decommit_count > decommit_before_free,
                "live recommit fast free decommits extent");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "live recommit fast block decommitted");
    expect_ok(api->check(heap, 0), "live recommit fast check after free");

    commit_before_reuse = state.commit_count;
    reuse = api->malloc(heap, large_size);
    expect_true(reuse == ptr, "live recommit fast reuses same block");
    expect_true(state.commit_count > commit_before_reuse,
                "live recommit fast recommits extent");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0,
                "live recommit fast block live after reuse");
    expect_ok(api->check(heap, 0), "live recommit fast check after reuse");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, reuse) >= large_size,
                "live recommit fast usable after reuse");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live recommit fast usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live recommit fast usable hits fast path");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.callsite_slow_calls == before.callsite_slow_calls,
                "live recommit fast usable avoids slow lookup");
    expect_true(after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live recommit fast usable avoids tree and physical fallback");

    api->free(heap, reuse);
    expect_ok(api->check(heap, 0), "live recommit fast final check");
    expect_ok(api->close(heap), "live recommit fast close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_live_block_recommit_failure_rollback_fast_lookup(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    const size_t large_size = 12u * page_size;
    const uint32_t platform_error = test_platform_error_code();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *failed;
    void *reuse;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    int commit_before_fail;
    int commit_before_reuse;
    uhm_stats before_fail;
    uhm_stats after_fail;
    test_live_lookup_counters before;
    test_live_lookup_counters after;

    memset(&state, 0, sizeof(state));
    state.platform_error = platform_error;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live recommit rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live recommit rollback arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live recommit rollback usermode vm init");
    vm.user = &state;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    vm.protect = fault_vm_protect;
    vm.fixed_remap = fault_vm_fixed_remap;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "live recommit rollback heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live recommit rollback api");

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "live recommit rollback malloc");
    memset(ptr, 0x5cu, large_size);
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "live recommit rollback block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size) &&
                    decommit_size != 0,
                "live recommit rollback has decommit range");
    api->free(heap, ptr);
    expect_true((block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                    (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "live recommit rollback block decommitted");
    expect_true(heap->core.malloc_free_tree_root == block,
                "live recommit rollback block in free tree");
    expect_ok(api->check(heap, 0),
              "live recommit rollback check before failure");
    test_shadow_model_check(heap, "live recommit rollback shadow before failure");

    memset(&before_fail, 0, sizeof(before_fail));
    before_fail.size = sizeof(before_fail);
    expect_ok(api->get_stats(heap, &before_fail),
              "live recommit rollback stats before failure");
    commit_before_fail = state.commit_count;
    state.fail_commit_once = 1;
    failed = api->malloc(heap, large_size);
    expect_true(failed == 0, "live recommit rollback failed malloc");
    expect_true(state.commit_count == commit_before_fail + 1,
                "live recommit rollback attempted recommit");
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "live recommit rollback captures platform error");

    memset(&after_fail, 0, sizeof(after_fail));
    after_fail.size = sizeof(after_fail);
    expect_ok(api->get_stats(heap, &after_fail),
              "live recommit rollback stats after failure");
    expect_true(after_fail.committed_bytes == before_fail.committed_bytes,
                "live recommit rollback does not publish committed bytes");
    expect_true(after_fail.recommit_events == before_fail.recommit_events &&
                    after_fail.recommit_bytes == before_fail.recommit_bytes,
                "live recommit rollback does not publish recommit stats");
    expect_true(after_fail.decommit_recommit_same_addr_events ==
                        before_fail.decommit_recommit_same_addr_events &&
                    after_fail.decommit_recommit_same_addr_bytes ==
                        before_fail.decommit_recommit_same_addr_bytes,
                "live recommit rollback does not publish same-address stats");
    expect_true((block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                    (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0 &&
                    (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0,
                "live recommit rollback restores free decommitted block");
    expect_true(heap->core.malloc_free_tree_root == block,
                "live recommit rollback reinserts free tree node");
    expect_ok(api->check(heap, 0),
              "live recommit rollback check after failure");
    test_shadow_model_check(heap, "live recommit rollback shadow after failure");

    commit_before_reuse = state.commit_count;
    reuse = api->malloc(heap, large_size);
    expect_true(reuse == ptr, "live recommit rollback reuses after failure");
    expect_true(state.commit_count == commit_before_reuse + 1,
                "live recommit rollback recommits after failure");
    expect_true((block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
                    (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0 &&
                    (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0,
                "live recommit rollback block live after reuse");
    expect_ok(api->check(heap, 0),
              "live recommit rollback check after reuse");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, reuse) >= large_size,
                "live recommit rollback usable after reuse");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live recommit rollback usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live recommit rollback usable stays fast");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live recommit rollback usable avoids fallback");

    api->free(heap, reuse);
    expect_ok(api->check(heap, 0), "live recommit rollback final check");
    expect_ok(api->close(heap), "live recommit rollback close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_live_block_decommit_failure_committed_reuse_fast_lookup(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    const size_t large_size = 12u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *reuse;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    int decommit_before_free;
    int commit_before_reuse;
    uhm_stats before_free;
    uhm_stats after_free;
    test_live_lookup_counters before;
    test_live_lookup_counters after;

    memset(&state, 0, sizeof(state));
    state.platform_error = test_platform_error_code();
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "live decommit rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "live decommit rollback arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "live decommit rollback usermode vm init");
    vm.user = &state;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    vm.protect = fault_vm_protect;
    vm.fixed_remap = fault_vm_fixed_remap;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "live decommit rollback heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "live decommit rollback api");

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "live decommit rollback malloc");
    memset(ptr, 0x6eu, large_size);
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "live decommit rollback block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size) &&
                    decommit_size != 0,
                "live decommit rollback has decommit range");
    expect_ok(api->check(heap, 0),
              "live decommit rollback check before free");

    memset(&before_free, 0, sizeof(before_free));
    before_free.size = sizeof(before_free);
    expect_ok(api->get_stats(heap, &before_free),
              "live decommit rollback stats before free");
    decommit_before_free = state.decommit_count;
    state.fail_decommit = 1;
    api->free(heap, ptr);
    state.fail_decommit = 0;
    expect_true(state.decommit_count == decommit_before_free + 1,
                "live decommit rollback attempted decommit");

    memset(&after_free, 0, sizeof(after_free));
    after_free.size = sizeof(after_free);
    expect_ok(api->get_stats(heap, &after_free),
              "live decommit rollback stats after free");
    expect_true(after_free.decommitted_bytes == before_free.decommitted_bytes &&
                    after_free.logical_decommitted_bytes ==
                        before_free.logical_decommitted_bytes,
                "live decommit rollback does not publish decommitted bytes");
    expect_true(after_free.decommit_events == before_free.decommit_events &&
                    after_free.decommit_bytes == before_free.decommit_bytes,
                "live decommit rollback does not publish decommit stats");
    expect_true((block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                    (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0 &&
                    (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0,
                "live decommit rollback keeps committed free block");
    expect_true(heap->core.malloc_free_tree_root == block,
                "live decommit rollback reinserts committed free block");
    expect_ok(api->check(heap, 0),
              "live decommit rollback check after failed decommit");
    test_shadow_model_check(heap,
                            "live decommit rollback shadow after failed decommit");

    commit_before_reuse = state.commit_count;
    reuse = api->malloc(heap, large_size);
    expect_true(reuse == ptr, "live decommit rollback reuses committed block");
    expect_true(state.commit_count == commit_before_reuse,
                "live decommit rollback reuse does not recommit");
    expect_true((block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
                    (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0 &&
                    (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0,
                "live decommit rollback block live after reuse");
    expect_ok(api->check(heap, 0),
              "live decommit rollback check after reuse");

    before = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(api->malloc_usable_size(heap, reuse) >= large_size,
                "live decommit rollback usable after reuse");
    after = test_live_lookup_counters_take(
        heap, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    expect_true(after.callsite_calls == before.callsite_calls + 1u,
                "live decommit rollback usable callsite counted");
    expect_true(after.fast_hits == before.fast_hits + 1u,
                "live decommit rollback usable stays fast");
    expect_true(after.slow_calls == before.slow_calls &&
                    after.tree_find_calls == before.tree_find_calls &&
                    after.fallback_scans == before.fallback_scans &&
                    after.callsite_fallback_scans ==
                        before.callsite_fallback_scans &&
                    after.callsite_throttles == before.callsite_throttles,
                "live decommit rollback usable avoids fallback");

    api->free(heap, reuse);
    expect_ok(api->check(heap, 0), "live decommit rollback final check");
    expect_ok(api->close(heap), "live decommit rollback close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static size_t test_malloc_free_tree_depth(const uhm_block *node) {
    size_t left_depth;
    size_t right_depth;

    if (!node) {
        return 0;
    }
    left_depth = test_malloc_free_tree_depth(node->free_tree_left);
    right_depth = test_malloc_free_tree_depth(node->free_tree_right);
    return (left_depth > right_depth ? left_depth : right_depth) + 1u;
}

static size_t test_popcount_u64(uint64_t value) {
    size_t count = 0;

    while (value != 0) {
        count += (size_t)(value & UINT64_C(1));
        value >>= 1;
    }
    return count;
}

static size_t test_malloc_free_tree_count(const uhm_block *node) {
    if (!node) {
        return 0;
    }
    return 1u + test_malloc_free_tree_count(node->free_tree_left) +
           test_malloc_free_tree_count(node->free_tree_right);
}

static uhm_block *test_malloc_free_tree_two_child_candidate(uhm_block *node) {
    uhm_block *candidate;

    if (!node) {
        return 0;
    }
    candidate = test_malloc_free_tree_two_child_candidate(node->free_tree_left);
    if (candidate) {
        return candidate;
    }
    if (node->free_tree_left && node->free_tree_right &&
        node->free_tree_left->total_size < node->total_size &&
        node->free_tree_right->total_size > node->total_size) {
        return node;
    }
    return test_malloc_free_tree_two_child_candidate(node->free_tree_right);
}

static void test_malloc_free_tree_remove_two_child_sink_covered(
    const uhm_storage_info *heap_info) {
    enum { block_count = 64 };
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = page_size * 256u;
    const size_t guard_size = page_size + 128u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *free_ptrs[block_count];
    void *guard_ptrs[block_count];
    uhm_block *target;
    void *expected_payload;
    void *reuse;
    size_t header_size;
    size_t target_request;
    size_t tree_count;
    size_t i;
    uint64_t remove_calls_before;
    uint64_t sink_steps_before;
    uint64_t max_sink_before;

    memset(free_ptrs, 0, sizeof(free_ptrs));
    memset(guard_ptrs, 0, sizeof(guard_ptrs));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "malloc free tree two-child heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "malloc free tree two-child arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "malloc free tree two-child usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 128u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "malloc free tree two-child heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "malloc free tree two-child api");

    for (i = 0; i < block_count; ++i) {
        size_t free_size = page_size + 1024u + i * 64u;
        free_ptrs[i] = api->malloc(heap, free_size);
        guard_ptrs[i] = api->malloc(heap, guard_size);
        expect_true(free_ptrs[i] != 0 && guard_ptrs[i] != 0,
                    "malloc free tree two-child allocation");
    }
    for (i = 0; i < block_count; ++i) {
        api->free(heap, free_ptrs[i]);
    }
    expect_ok(api->check(heap, 0),
              "malloc free tree two-child check after frees");

    tree_count = test_malloc_free_tree_count(heap->core.malloc_free_tree_root);
    expect_true(tree_count == block_count,
                "malloc free tree two-child free count");
    target =
        test_malloc_free_tree_two_child_candidate(heap->core.malloc_free_tree_root);
    expect_true(target != 0, "malloc free tree two-child candidate found");
    expect_true((target->flags & UHM_BLOCK_FLAG_FREE) != 0,
                "malloc free tree two-child target is free");
    expected_payload = test_block_payload(target);
    header_size = (size_t)((uintptr_t)expected_payload - (uintptr_t)target);
    expect_true(target->total_size > header_size,
                "malloc free tree two-child target payload room");
    target_request = target->total_size - header_size;

    remove_calls_before = heap->core.malloc_free_tree_remove_calls;
    sink_steps_before = heap->core.malloc_free_tree_remove_sink_steps;
    max_sink_before = heap->core.malloc_free_tree_max_remove_sink_steps;
    reuse = api->malloc(heap, target_request);
    expect_true(reuse == expected_payload,
                "malloc free tree two-child exact target reused");
    expect_true(heap->core.malloc_free_tree_remove_calls >
                    remove_calls_before,
                "malloc free tree two-child remove call counted");
    expect_true(heap->core.malloc_free_tree_remove_sink_steps >
                    sink_steps_before,
                "malloc free tree two-child sink steps counted");
    expect_true(heap->core.malloc_free_tree_max_remove_sink_steps >
                    max_sink_before,
                "malloc free tree two-child max sink updated");
    expect_true(heap->core.malloc_free_tree_remove_budget_exceeded == 0,
                "malloc free tree two-child remove budget not exceeded");
    expect_ok(api->check(heap, 0),
              "malloc free tree two-child check after reuse");

    api->free(heap, reuse);
    for (i = 0; i < block_count; ++i) {
        api->free(heap, guard_ptrs[i]);
    }
    expect_ok(api->check(heap, 0), "malloc free tree two-child final check");
    expect_ok(api->close(heap), "malloc free tree two-child close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_malloc_free_tree_ordered_same_size_pressure(
    const uhm_storage_info *heap_info) {
    enum { free_count = 96 };
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    const size_t alloc_size = 640u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *free_ptrs[free_count];
    void *live_ptrs[free_count];
    void *reuse;
    size_t tree_count;
    size_t tree_depth;
    size_t i;
    uint64_t saved_priority;
    uhm_block *saved_free_left;
    uint64_t find_budget_before;
    uint64_t treap_health_before;

    memset(free_ptrs, 0, sizeof(free_ptrs));
    memset(live_ptrs, 0, sizeof(live_ptrs));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "malloc free tree heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "malloc free tree arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "malloc free tree usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "malloc free tree heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "malloc free tree api");

    for (i = 0; i < free_count; ++i) {
        free_ptrs[i] = api->malloc(heap, alloc_size);
        live_ptrs[i] = api->malloc(heap, alloc_size);
        expect_true(free_ptrs[i] != 0 && live_ptrs[i] != 0,
                    "malloc free tree pressure allocation");
    }
    for (i = 0; i < free_count; ++i) {
        api->free(heap, free_ptrs[i]);
    }
    expect_ok(api->check(heap, 0), "malloc free tree check after ordered frees");

    tree_count = test_malloc_free_tree_count(heap->core.malloc_free_tree_root);
    tree_depth = test_malloc_free_tree_depth(heap->core.malloc_free_tree_root);
    expect_true(tree_count == free_count, "malloc free tree pressure count");
    expect_true(tree_depth <= 48u, "malloc free tree treap avoids ordered chain");
    expect_true(heap->core.malloc_free_tree_insertions >= free_count,
                "malloc free tree diagnostics insertions counted");
    expect_true(heap->core.malloc_free_tree_max_insert_depth != 0,
                "malloc free tree diagnostics max insert depth counted");
    expect_true(heap->core.malloc_free_tree_current_nodes == tree_count,
                "malloc free tree tracked node count");
    expect_true(heap->core.malloc_free_tree_max_nodes >= tree_count,
                "malloc free tree max node count");
    expect_true(heap->core.malloc_free_tree_depth_limit_exceeded == 0,
                "malloc free tree depth limit not exceeded");
    expect_true(heap->core.malloc_free_tree_priority_bucket_mask != 0,
                "malloc free tree priority bucket mask counted");
    expect_true(test_popcount_u64(
                    heap->core.malloc_free_tree_priority_bucket_mask) >= 4u,
                "malloc free tree priority buckets distributed");
    expect_true(heap->core.malloc_free_tree_priority_bucket_max != 0,
                "malloc free tree priority bucket max counted");
    expect_true(heap->core.malloc_free_tree_insert_budget_exceeded == 0,
                "malloc free tree insert budget not exceeded");
    expect_true(
        heap->core.malloc_free_tree_insert_rotation_budget_exceeded == 0,
        "malloc free tree insert rotation budget not exceeded");

    expect_true(heap->core.malloc_free_tree_root != 0,
                "malloc free tree find guard corruption target");
    saved_free_left = heap->core.malloc_free_tree_root->free_tree_left;
    find_budget_before = heap->core.malloc_free_tree_find_budget_exceeded;
    treap_health_before = heap->core.treap_health_flags;
    heap->core.malloc_free_tree_root->free_tree_left =
        heap->core.malloc_free_tree_root;
    reuse = api->malloc(heap, alloc_size);
    expect_true(reuse != 0,
                "malloc free tree find guard returns instead of hanging");
    expect_true(heap->core.malloc_free_tree_find_budget_exceeded >
                    find_budget_before,
                "malloc free tree find guard budget counted");
    expect_true((heap->core.treap_health_flags &
                 UHM_TREAP_HEALTH_MALLOC_FIND) != 0,
                "malloc free tree find guard health flag set");
    expect_true(heap->core.treap_health_flags != treap_health_before,
                "malloc free tree health flag is sticky");
    heap->core.malloc_free_tree_root->free_tree_left = saved_free_left;
    api->free(heap, reuse);
    expect_ok(api->check(heap, 0), "malloc free tree find guard restored");
    expect_true(api->check(heap, UHM_CHECK_FAIL_ON_TREAP_HEALTH) ==
                    UHM_ERR_CORRUPTION,
                "malloc free tree strict health check fails after guard");

    expect_true(heap->core.malloc_free_tree_root != 0,
                "malloc free tree priority cache corruption target");
    saved_priority = heap->core.malloc_free_tree_root->free_tree_priority;
    heap->core.malloc_free_tree_root->free_tree_priority ^= UINT64_C(1);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "malloc free tree cached priority corruption detected");
    heap->core.malloc_free_tree_root->free_tree_priority = saved_priority;
    expect_ok(api->check(heap, 0),
              "malloc free tree cached priority corruption restored");

    reuse = api->malloc(heap, alloc_size);
    expect_true(reuse != 0, "malloc free tree pressure reuse");
    api->free(heap, reuse);
    for (i = 0; i < free_count; ++i) {
        api->free(heap, live_ptrs[i]);
    }
    expect_ok(api->check(heap, 0), "malloc free tree final check");
    expect_ok(api->close(heap), "malloc free tree close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

