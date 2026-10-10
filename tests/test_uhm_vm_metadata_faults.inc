static void test_vm_failure_rollbacks(const uhm_storage_info *heap_info) {
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    const size_t page_size = uhm_platform_system_page_size();
    void *map = 0;
    void *failed_addr;
    void *failed_malloc;
    void *fixed;
    void *middle;
    uhm_stats before;
    uhm_stats after;
    uint32_t platform_error;
    int before_commit_count;
    int before_decommit_count;
    uint64_t vm_commit_calls_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_decommit_calls_before;
    uint64_t vm_decommit_lock_held_before;
    uintptr_t malloc_hi_before_failure;
    uint64_t generation_before;

    memset(&state, 0, sizeof(state));
    platform_error = test_platform_error_code();
    state.platform_error = platform_error;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment, &heap_mem) != 0,
                "rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "rollback arena alloc");

    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "rollback usermode vm init");
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
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "rollback heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "rollback api");

    state.partial_commit_before_failure = 1;
    state.fail_commit = 1;
    map = 0;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE) == UHM_ERR_PLATFORM,
                "mmap commit failure is reported");
    expect_generation_advanced(heap, generation_before,
                               "mmap commit failure advances generation");
    test_set_platform_error(platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "mmap commit failure captures platform error");
    failed_addr = state.last_commit_addr;
    expect_true(failed_addr != 0 && state.last_commit_size == page_size,
                "mmap commit failure records range");
    expect_true(state.decommit_count == 1, "mmap commit failure rolls back commit");
    expect_true(state.last_decommit_addr == failed_addr &&
                    state.last_decommit_size == page_size,
                "mmap commit rollback covers failed range");
    state.fail_commit = 0;
    fixed = failed_addr;
    expect_ok(api->mmap(heap, &fixed, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
              "failed mmap range is reusable");
    expect_true(fixed == failed_addr, "failed mmap range same address");
    expect_ok(api->munmap(heap, fixed, page_size), "failed mmap range cleanup");

    before_decommit_count = state.decommit_count;
    state.fail_commit_once = 1;
    malloc_hi_before_failure = heap->core.malloc_hi;
    generation_before = uhm_heap_generation_load(heap);
    failed_malloc = api->malloc(heap, page_size);
    expect_true(failed_malloc == 0, "malloc commit failure returns null");
    expect_true(heap->core.malloc_hi == malloc_hi_before_failure,
                "malloc claim rollback restores low watermark");
    expect_generation_advanced(heap, generation_before,
                               "malloc commit failure advances generation");
    expect_true(state.decommit_count == before_decommit_count + 1,
                "malloc commit failure rolls back commit");
    expect_true(state.last_decommit_addr == state.last_commit_addr &&
                    state.last_decommit_size == state.last_commit_size,
                "malloc commit rollback covers failed range");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 3u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "decommit failure test mmap");
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before), "stats before decommit failure");
    middle = (void *)((uintptr_t)map + page_size);
    state.fail_decommit = 1;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->munmap(heap, middle, page_size) == UHM_ERR_PLATFORM,
                "munmap decommit failure is reported");
    expect_generation_advanced(heap, generation_before,
                               "munmap decommit failure advances generation");
    test_set_platform_error(platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "munmap decommit failure captures platform error");
    state.fail_decommit = 0;
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after), "stats after decommit failure");
    expect_true(before.mmap_live_bytes == after.mmap_live_bytes,
                "failed munmap keeps mmap live bytes");
    fixed = middle;
    expect_true(api->mmap(heap, &fixed, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                    UHM_ERR_ALREADY_MAPPED,
                "failed munmap keeps address mapped");
    expect_ok(api->mprotect(heap, map, page_size * 3u, UHM_PROT_READ),
              "failed munmap keeps covering VMA");
    expect_ok(api->munmap(heap, map, page_size * 3u), "decommit failure map cleanup");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 3u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "protect failure test mmap");
    middle = (void *)((uintptr_t)map + page_size);
    state.fail_protect = 1;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mprotect(heap, middle, page_size, UHM_PROT_READ) == UHM_ERR_PLATFORM,
                "mprotect failure is reported");
    expect_generation_advanced(heap, generation_before,
                               "mprotect failure advances generation");
    test_set_platform_error(platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "mprotect failure captures platform error");
    state.fail_protect = 0;
    expect_ok(api->mprotect(heap, map, page_size * 3u, UHM_PROT_READ),
              "failed mprotect keeps covering VMA");
    expect_ok(api->munmap(heap, map, page_size * 3u), "protect failure map cleanup");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "exact fixed replace failure test mmap");
    fixed = map;
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before),
              "stats before exact fixed replace failure");
    before_commit_count = state.commit_count;
    before_decommit_count = state.decommit_count;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    state.fail_commit_once = 1;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mmap(heap, &fixed, page_size * 2u, UHM_PROT_READ,
                          UHM_MAP_PRIVATE | UHM_MAP_FIXED) ==
                    UHM_ERR_PLATFORM,
                "exact fixed replace commit failure is reported");
    expect_generation_advanced(
        heap, generation_before,
        "exact fixed replace failure advances generation");
    test_set_platform_error(platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "exact fixed replace failure captures platform error");
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after),
              "stats after exact fixed replace failure");
    expect_true(before.committed_bytes == after.committed_bytes,
                "failed exact fixed replace does not publish commit stats");
    expect_true(before.decommitted_bytes == after.decommitted_bytes,
                "failed exact fixed replace does not publish decommit stats");
    expect_true(before.logical_decommitted_bytes ==
                    after.logical_decommitted_bytes,
                "failed exact fixed replace does not publish logical stats");
    expect_true(before.physical_released_bytes ==
                    after.physical_released_bytes,
                "failed exact fixed replace does not publish physical stats");
    expect_true(state.commit_count == before_commit_count + 2,
                "failed exact fixed replace commits new range and restores old range");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 2u,
                "failed exact fixed replace counts unlocked old-range recommit");
    expect_true(heap->core.vm_commit_lock_held_calls ==
                    vm_commit_lock_held_before,
                "failed exact fixed replace old-range recommit not lock-held");
    expect_true(state.decommit_count == before_decommit_count + 2,
                "failed exact fixed replace decommits old range and rollback range");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 2u,
                "failed exact fixed replace counts unlocked decommit rollback");
    expect_true(heap->core.vm_decommit_lock_held_calls ==
                    vm_decommit_lock_held_before,
                "failed exact fixed replace rollback decommit not lock-held");
    expect_true(fixed == map,
                "failed exact fixed replace preserves requested address");
    expect_ok(api->mprotect(heap, map, page_size * 2u, UHM_PROT_READ),
              "failed exact fixed replace keeps old VMA");
    expect_ok(api->munmap(heap, map, page_size * 2u),
              "exact fixed replace failure cleanup");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 3u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "fixed replace failure test mmap");
    middle = (void *)((uintptr_t)map + page_size);
    fixed = middle;
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before), "stats before fixed replace failure");
    before_commit_count = state.commit_count;
    before_decommit_count = state.decommit_count;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    state.fail_commit_once = 1;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mmap(heap, &fixed, page_size, UHM_PROT_READ,
                          UHM_MAP_PRIVATE | UHM_MAP_FIXED) == UHM_ERR_PLATFORM,
                "fixed replace commit failure is reported");
    expect_generation_advanced(heap, generation_before,
                               "fixed replace failure advances generation");
    test_set_platform_error(platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "fixed replace failure captures platform error");
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after), "stats after fixed replace failure");
    expect_true(before.committed_bytes == after.committed_bytes,
                "failed fixed replace does not publish commit stats");
    expect_true(before.decommitted_bytes == after.decommitted_bytes,
                "failed fixed replace does not publish decommit stats");
    expect_true(before.logical_decommitted_bytes == after.logical_decommitted_bytes,
                "failed fixed replace does not publish logical stats");
    expect_true(before.physical_released_bytes == after.physical_released_bytes,
                "failed fixed replace does not publish physical stats");
    expect_true(state.commit_count == before_commit_count + 2,
                "failed fixed replace commits new range and restores old range");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 2u,
                "failed fixed replace counts unlocked old-range recommit");
    expect_true(heap->core.vm_commit_lock_held_calls ==
                    vm_commit_lock_held_before,
                "failed fixed replace old-range recommit not lock-held");
    expect_true(state.decommit_count == before_decommit_count + 2,
                "failed fixed replace decommits old range and rollback range");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 2u,
                "failed fixed replace counts unlocked decommit rollback");
    expect_true(heap->core.vm_decommit_lock_held_calls ==
                    vm_decommit_lock_held_before,
                "failed fixed replace rollback decommit not lock-held");
    expect_true(fixed == middle, "failed fixed replace preserves requested address");
    expect_ok(api->mprotect(heap, map, page_size * 3u, UHM_PROT_READ),
              "failed fixed replace keeps old covering VMA");
    expect_ok(api->munmap(heap, map, page_size * 3u), "fixed replace failure cleanup");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 4u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "cross fixed replace failure test mmap");
    expect_ok(api->mprotect(heap, (void *)((uintptr_t)map + page_size),
                            page_size * 2u, UHM_PROT_READ),
              "cross fixed replace failure split old VMAs");
    fixed = map;
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before), "stats before cross fixed failure");
    before_commit_count = state.commit_count;
    before_decommit_count = state.decommit_count;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    state.fail_commit_once = 1;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mmap(heap, &fixed, page_size * 3u, UHM_PROT_READ,
                          UHM_MAP_PRIVATE | UHM_MAP_FIXED) == UHM_ERR_PLATFORM,
                "cross fixed replace commit failure is reported");
    expect_generation_advanced(heap, generation_before,
                               "cross fixed replace failure advances generation");
    test_set_platform_error(platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "cross fixed replace failure captures platform error");
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after), "stats after cross fixed failure");
    expect_true(before.committed_bytes == after.committed_bytes,
                "failed cross fixed replace does not publish commit stats");
    expect_true(before.decommitted_bytes == after.decommitted_bytes,
                "failed cross fixed replace does not publish decommit stats");
    expect_true(before.logical_decommitted_bytes == after.logical_decommitted_bytes,
                "failed cross fixed replace does not publish logical stats");
    expect_true(before.physical_released_bytes == after.physical_released_bytes,
                "failed cross fixed replace does not publish physical stats");
    expect_true(state.commit_count == before_commit_count + 3,
                "failed cross fixed replace commits new range and restores old segments");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 3u,
                "failed cross fixed replace counts unlocked old-range recommits");
    expect_true(heap->core.vm_commit_lock_held_calls ==
                    vm_commit_lock_held_before,
                "failed cross fixed replace old-range recommits not lock-held");
    expect_true(state.decommit_count == before_decommit_count + 2,
                "failed cross fixed replace decommits old range and rollback range");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 2u,
                "failed cross fixed replace counts unlocked decommit rollback");
    expect_true(heap->core.vm_decommit_lock_held_calls ==
                    vm_decommit_lock_held_before,
                "failed cross fixed replace rollback decommit not lock-held");
    expect_true(fixed == map, "failed cross fixed replace preserves requested address");
    {
        void *conflict = (void *)((uintptr_t)map + page_size);
        expect_true(api->mmap(heap, &conflict, page_size,
                              UHM_PROT_READ | UHM_PROT_WRITE,
                              UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                        UHM_ERR_ALREADY_MAPPED,
                    "failed cross fixed replace keeps old split VMA live");
    }
    expect_ok(api->mprotect(heap, map, page_size, UHM_PROT_READ),
              "failed cross fixed replace keeps first old VMA");
    expect_ok(api->mprotect(heap, (void *)((uintptr_t)map + page_size),
                            page_size * 2u, UHM_PROT_READ),
              "failed cross fixed replace keeps second old VMA");
    expect_ok(api->munmap(heap, map, page_size * 4u),
              "cross fixed replace failure cleanup");

    expect_ok(api->check(heap, 0), "rollback heap check");
    test_shadow_model_check(heap, "rollback shadow model");
    expect_ok(api->close(heap), "rollback heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_fault_injection_rules(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    fault_vm_state seq_a;
    fault_vm_state seq_b;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uint32_t platform_error;
    void *map1 = 0;
    void *map2 = 0;
    void *map3 = 0;
    void *reusable = 0;
    void *fixed = 0;
    int saw_fail = 0;
    int saw_pass = 0;
    size_t i;

    memset(&seq_a, 0, sizeof(seq_a));
    memset(&seq_b, 0, sizeof(seq_b));
    seq_a.fail_ops = FAULT_VM_OP_COMMIT;
    seq_b.fail_ops = FAULT_VM_OP_COMMIT;
    seq_a.fail_probability_ppm = FAULT_VM_PROB_DENOM / 2u;
    seq_b.fail_probability_ppm = FAULT_VM_PROB_DENOM / 2u;
    seq_a.fault_seed = UINT64_C(0x123456789abcdef0);
    seq_b.fault_seed = UINT64_C(0x123456789abcdef0);
    for (i = 0; i < 64u; ++i) {
        int a = fault_vm_should_fail(&seq_a, FAULT_VM_OP_COMMIT,
                                     (void *)(uintptr_t)(page_size * (i + 1u)),
                                     page_size);
        int b = fault_vm_should_fail(&seq_b, FAULT_VM_OP_COMMIT,
                                     (void *)(uintptr_t)(page_size * (i + 1u)),
                                     page_size);
        expect_true(a == b, "fault deterministic seed sequence");
        saw_fail |= a;
        saw_pass |= !a;
    }
    expect_true(saw_fail && saw_pass, "fault probability seed has mixed outcomes");

    memset(&state, 0, sizeof(state));
    platform_error = test_platform_error_code();
    state.platform_error = platform_error;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "fault rules heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "fault rules arena alloc");

    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "fault rules usermode vm init");
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
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "fault rules heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "fault rules api");

    fault_vm_clear_rules(&state);
    state.fail_ops = FAULT_VM_OP_COMMIT;
    state.fail_on_match = 2u;
    map1 = 0;
    map2 = 0;
    expect_ok(api->mmap(heap, &map1, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "fault call ordinal first mmap");
    expect_true(api->mmap(heap, &map2, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE) == UHM_ERR_PLATFORM,
                "fault call ordinal second mmap fails");
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "fault call ordinal captures platform error");
    expect_true(state.fault_match_count == 2u, "fault call ordinal count");
    fault_vm_clear_rules(&state);
    expect_ok(api->munmap(heap, map1, page_size), "fault call ordinal cleanup");

    state.fail_ops = FAULT_VM_OP_COMMIT;
    state.fail_probability_ppm = FAULT_VM_PROB_DENOM;
    state.fault_seed = UINT64_C(0xfeed1234abcd5678);
    map3 = 0;
    expect_true(api->mmap(heap, &map3, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE) == UHM_ERR_PLATFORM,
                "fault probability mmap fails");
    expect_true(state.fault_match_count == 1u, "fault probability count");
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "fault probability captures platform error");
    fault_vm_clear_rules(&state);

    reusable = 0;
    expect_ok(api->mmap(heap, &reusable, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "fault address reusable mmap");
    expect_ok(api->munmap(heap, reusable, page_size), "fault address reusable munmap");

    state.fail_ops = FAULT_VM_OP_COMMIT;
    state.fail_probability_ppm = FAULT_VM_PROB_DENOM;
    state.fail_addr_range_enabled = 1;
    state.fail_addr_begin = (uintptr_t)reusable + page_size * 2u;
    state.fail_addr_end = state.fail_addr_begin + page_size;
    fixed = reusable;
    expect_ok(api->mmap(heap, &fixed, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
              "fault address out-of-range fixed mmap");
    expect_true(fixed == reusable, "fault address out-of-range fixed address");
    expect_true(state.fault_match_count == 0, "fault address out-of-range not matched");
    expect_ok(api->munmap(heap, fixed, page_size), "fault address out-of-range cleanup");

    fault_vm_clear_rules(&state);
    state.fail_ops = FAULT_VM_OP_COMMIT;
    state.fail_probability_ppm = FAULT_VM_PROB_DENOM;
    state.fail_addr_range_enabled = 1;
    state.fail_addr_begin = (uintptr_t)reusable;
    state.fail_addr_end = (uintptr_t)reusable + page_size;
    fixed = reusable;
    expect_true(api->mmap(heap, &fixed, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                    UHM_ERR_PLATFORM,
                "fault address in-range fixed mmap fails");
    expect_true(state.fault_match_count == 1u, "fault address in-range matched");
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "fault address captures platform error");
    fault_vm_clear_rules(&state);
    fixed = reusable;
    expect_ok(api->mmap(heap, &fixed, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
              "fault address range reusable after failure");
    expect_true(fixed == reusable, "fault address reusable same address");
    expect_ok(api->munmap(heap, fixed, page_size), "fault address cleanup");

    expect_ok(api->check(heap, 0), "fault rules heap check");
    expect_ok(api->close(heap), "fault rules heap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mmap_publish_failure_rolls_back_commit(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 640u * 1024u;
    const size_t control_size = 3u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *pin;
    void *map = 0;
    void *pressure_map = 0;
    void *reusable = 0;
    uhm_stats before;
    uhm_stats after;
    int commit_before;
    int decommit_before;
    size_t vma_count_before;
    size_t i;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "publish rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "publish rollback arena alloc");

    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "publish rollback usermode vm init");
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
    config.vm = vm;
    config.control_band_min_size = control_size;

    expect_ok(uhm_open(&config, &heap), "publish rollback heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "publish rollback api");

    expect_ok(api->mmap(heap, &pressure_map, page_size * 86u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "publish rollback pressure mmap");
    expect_ok(api->mmap(heap, &reusable, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "publish rollback reusable mmap");
    expect_ok(api->munmap(heap, reusable, page_size),
              "publish rollback reusable munmap");
    pin = api->malloc(heap, 160u * 1024u);
    expect_true(pin != 0, "publish rollback collision pin");

    for (i = 0; i < 80u; i += 2u) {
        void *page = (void *)((uintptr_t)pressure_map + i * page_size);
        uhm_result result = api->mprotect(heap, page, page_size, UHM_PROT_READ);
        if (result == UHM_ERR_OUT_OF_MEMORY) {
            break;
        }
        expect_ok(result, "publish rollback pressure mprotect");
    }

    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before), "publish rollback stats before");
    commit_before = state.commit_count;
    decommit_before = state.decommit_count;
    vma_count_before = test_vma_count_all(heap);

    heap->core.fault_control_alloc_countdown = 1u;
    expect_true(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE) == UHM_ERR_OUT_OF_MEMORY,
                "publish rollback committed mmap fails at publish");
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after), "publish rollback stats after");
    expect_true(state.commit_count == commit_before + 1,
                "publish rollback performed platform commit");
    expect_true(state.decommit_count == decommit_before + 1,
                "publish rollback decommitted unpublished range");
    expect_true(before.committed_bytes == after.committed_bytes,
                "publish rollback removes unpublished commit stats");
    expect_true(before.decommitted_bytes == after.decommitted_bytes,
                "publish rollback does not publish decommit stats");
    expect_true(before.logical_decommitted_bytes == after.logical_decommitted_bytes,
                "publish rollback does not publish logical stats");
    expect_true(before.physical_released_bytes == after.physical_released_bytes,
                "publish rollback does not publish physical stats");
    expect_true(test_vma_count_all(heap) == vma_count_before,
                "publish rollback does not insert failed VMA");
    expect_ok(api->check(heap, 0), "publish rollback heap check");
    expect_ok(api->close(heap), "publish rollback heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_allocation_failures(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t leaf_pages = 256u;
    const size_t leaf_span = leaf_pages * page_size;
    const size_t control_size = 64u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *fixed;
    void *map = 0;
    uhm_free_extent *saved_free_extents;
    size_t saved_free_extent_count;
    uint64_t generation_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "metadata leaf failure heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(2u * leaf_span, page_size,
                                    &arena_mem) != 0,
                "metadata leaf failure arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata leaf failure usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 2u * leaf_span;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.control_band_min_size = control_size;

    expect_ok(uhm_open(&config, &heap), "metadata leaf failure heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata leaf failure api");

    fixed = (void *)((uintptr_t)arena_mem.aligned + 2u * leaf_span - page_size);
    heap->core.fault_control_alloc_countdown = 1u;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mmap(heap, &fixed, page_size,
                          UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                    UHM_ERR_OUT_OF_MEMORY,
                "metadata page leaf allocation failure");
    expect_generation_advanced(heap, generation_before,
                               "metadata page leaf failure advances generation");
    expect_ok(api->check(heap, 0), "metadata leaf failure heap check");
    expect_ok(api->close(heap), "metadata leaf failure close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);

    memset(&heap_mem, 0, sizeof(heap_mem));
    memset(&arena_mem, 0, sizeof(arena_mem));
    heap = 0;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "metadata vma failure heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "metadata vma failure arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata vma failure usermode vm init");
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.vm = vm;
    config.control_band_min_size = control_size;

    expect_ok(uhm_open(&config, &heap), "metadata vma failure heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata vma failure api");
    expect_ok(api->mmap(heap, &map, page_size * 3u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "metadata vma failure source mmap");
    heap->core.fault_control_alloc_countdown = 1u;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mprotect(heap, (void *)((uintptr_t)map + page_size),
                              page_size, UHM_PROT_READ) == UHM_ERR_OUT_OF_MEMORY,
                "metadata VMA node allocation failure");
    expect_generation_advanced(heap, generation_before,
                               "metadata VMA failure advances generation");
    expect_ok(api->mprotect(heap, map, page_size * 3u, UHM_PROT_READ),
              "metadata VMA failure keeps covering VMA");
    expect_ok(api->munmap(heap, map, page_size * 3u),
              "metadata VMA failure cleanup");
    expect_ok(api->check(heap, 0), "metadata vma failure heap check");
    expect_ok(api->close(heap), "metadata vma failure close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);

    memset(&heap_mem, 0, sizeof(heap_mem));
    memset(&arena_mem, 0, sizeof(arena_mem));
    heap = 0;
    map = 0;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "metadata extent failure heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "metadata extent failure arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata extent failure usermode vm init");
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.vm = vm;
    config.control_band_min_size = control_size;

    expect_ok(uhm_open(&config, &heap), "metadata extent failure heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata extent failure api");
    expect_ok(api->mmap(heap, &map, page_size * 3u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "metadata extent failure source mmap");
    expect_ok(api->mprotect(heap, (void *)((uintptr_t)map + page_size),
                            page_size, UHM_PROT_READ),
              "metadata extent failure isolate middle VMA");
    saved_free_extents = heap->core.emergency_free_extent_head;
    saved_free_extent_count = heap->core.emergency_free_extent_available;
    heap->core.emergency_free_extent_head = 0;
    heap->core.emergency_free_extent_available = 0;
    heap->core.fault_control_alloc_countdown = 1u;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->munmap(heap, (void *)((uintptr_t)map + page_size),
                            page_size) == UHM_ERR_OUT_OF_MEMORY,
                "metadata free extent node allocation failure");
    expect_generation_advanced(heap, generation_before,
                               "metadata free extent failure advances generation");
    heap->core.emergency_free_extent_head = saved_free_extents;
    heap->core.emergency_free_extent_available = saved_free_extent_count;
    expect_ok(api->mprotect(heap, (void *)((uintptr_t)map + page_size),
                            page_size, UHM_PROT_READ | UHM_PROT_WRITE),
              "metadata extent failure keeps mapping live");
    expect_ok(api->munmap(heap, map, page_size * 3u),
              "metadata extent failure cleanup");
    expect_ok(api->check(heap, 0), "metadata extent failure heap check");
    expect_ok(api->close(heap), "metadata extent failure close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_exec_and_wx_capability_gates(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment, &heap_mem) != 0,
                "exec gate heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "exec gate arena alloc");

    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "exec gate usermode vm init");
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "exec gate heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "exec gate api");
    expect_true(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_EXEC,
                          UHM_MAP_PRIVATE) == UHM_ERR_UNSUPPORTED,
                "execute mmap rejected without capability");
    map = 0;
    expect_true(api->mmap(heap, &map, page_size, UHM_PROT_EXEC,
                          UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT) == UHM_ERR_UNSUPPORTED,
                "execute no-commit mmap rejected without capability");
    map = 0;
    expect_ok(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "exec gate writable mmap");
    expect_true(api->mprotect(heap, map, page_size, UHM_PROT_READ | UHM_PROT_EXEC) ==
                    UHM_ERR_UNSUPPORTED,
                "execute mprotect rejected without capability");
    expect_ok(api->munmap(heap, map, page_size), "exec gate cleanup munmap");
    expect_ok(api->check(heap, 0), "exec gate check");
    expect_ok(api->close(heap), "exec gate close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);

    memset(&heap_mem, 0, sizeof(heap_mem));
    memset(&arena_mem, 0, sizeof(arena_mem));
    heap = 0;
    map = 0;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment, &heap_mem) != 0,
                "wx gate heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "wx gate arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS |
                                                    UHM_VM_PROTECT_EXEC_SUPPORTED |
                                                    UHM_VM_WX_POLICY_RESTRICTED),
              "wx gate usermode vm init");
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "wx gate heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "wx gate api");
    expect_true(api->mmap(heap, &map, page_size,
                          UHM_PROT_READ | UHM_PROT_WRITE | UHM_PROT_EXEC,
                          UHM_MAP_PRIVATE) == UHM_ERR_PROTECTION,
                "wx mmap rejected by policy");
    map = 0;
    expect_ok(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_EXEC,
                        UHM_MAP_PRIVATE),
              "rx mmap allowed with exec capability");
    expect_true(api->mprotect(heap, map, page_size,
                              UHM_PROT_READ | UHM_PROT_WRITE | UHM_PROT_EXEC) ==
                    UHM_ERR_PROTECTION,
                "wx mprotect rejected by policy");
    expect_ok(api->munmap(heap, map, page_size), "wx gate cleanup munmap");
    expect_ok(api->check(heap, 0), "wx gate check");
    expect_ok(api->close(heap), "wx gate close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

