static void test_large_malloc_extent_decommit_recommit(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    const size_t large_size = 12u * page_size;
    void *ptr;
    void *reuse;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    int commit_before_reuse;
    uhm_stats before;
    uhm_stats after_free;
    uhm_stats after_reuse;
    uint64_t same_addr_events_before;
    uint64_t same_addr_bytes_before;
    uint64_t commit_all_fast_paths_before;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "large extent heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(2u * 1024u * 1024u, page_size, &arena_mem) != 0,
                "large extent arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large extent usermode vm init");
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
    config.arena_size = 2u * 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "large extent heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large extent api");

    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before), "large extent stats before");

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "large extent malloc");
    memset(ptr, 0x6du, large_size);
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "large extent block lookup");
    expect_true((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0,
                "large extent commit state known after malloc");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size),
                "large extent has decommittable interior pages");

    commit_all_fast_paths_before =
        heap->core.malloc_free_decommit_commit_all_fast_paths;
    api->free(heap, ptr);
    expect_true((block->flags & UHM_BLOCK_FLAG_FREE) != 0,
                "large extent free flag");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large extent decommitted flag");
    expect_true(state.decommit_count != 0, "large extent decommit callback used");
    expect_true(heap->core.malloc_free_decommit_commit_all_fast_paths ==
                    commit_all_fast_paths_before + 1u,
                "large extent free skips page-map live scan");
    expect_true((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) == 0,
                "large extent decommit clears commit state hint");
    expect_ok(api->check(heap, 0), "large extent check after free");
    test_shadow_model_check(heap, "large extent shadow after free");

    memset(&after_free, 0, sizeof(after_free));
    after_free.size = sizeof(after_free);
    expect_ok(api->get_stats(heap, &after_free), "large extent stats after free");
    expect_true(after_free.decommitted_bytes >= before.decommitted_bytes + decommit_size,
                "large extent decommit stats");
    expect_true(after_free.logical_decommitted_bytes >=
                    before.logical_decommitted_bytes + decommit_size,
                "large extent logical decommit stats");
    expect_true(after_free.decommit_events > before.decommit_events,
                "large extent public decommit event counted");
    expect_true(after_free.decommit_bytes >=
                    before.decommit_bytes + decommit_size,
                "large extent public decommit bytes counted");

    commit_before_reuse = state.commit_count;
    same_addr_events_before =
        heap->core.decommit_recommit_same_addr_events;
    same_addr_bytes_before =
        heap->core.decommit_recommit_same_addr_bytes;
    reuse = api->malloc(heap, large_size);
    expect_true(reuse == ptr, "large extent reuses same block");
    expect_true(state.commit_count > commit_before_reuse,
                "large extent recommit callback used");
    expect_true(heap->core.decommit_recommit_same_addr_events >
                    same_addr_events_before,
                "large extent same-address decommit/recommit event counted");
    expect_true(heap->core.decommit_recommit_same_addr_bytes >=
                    same_addr_bytes_before + decommit_size,
                "large extent same-address decommit/recommit bytes counted");
    memset(&after_reuse, 0, sizeof(after_reuse));
    after_reuse.size = sizeof(after_reuse);
    expect_ok(api->get_stats(heap, &after_reuse),
              "large extent stats after reuse");
    expect_true(after_reuse.recommit_events > after_free.recommit_events,
                "large extent public recommit event counted");
    expect_true(after_reuse.recommit_bytes >=
                    after_free.recommit_bytes + decommit_size,
                "large extent public recommit bytes counted");
    expect_true(after_reuse.decommit_recommit_same_addr_events >
                    after_free.decommit_recommit_same_addr_events,
                "large extent public same-address event counted");
    expect_true(after_reuse.decommit_recommit_same_addr_bytes >=
                    after_free.decommit_recommit_same_addr_bytes +
                        decommit_size,
                "large extent public same-address bytes counted");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0,
                "large extent recommit clears flag");
    expect_true((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0,
                "large extent recommit restores commit state hint");
    memset(reuse, 0x7eu, large_size);
    expect_ok(api->check(heap, 0), "large extent check after reuse");
    test_shadow_model_check(heap, "large extent shadow after reuse");

    {
        int decommit_before_hot_free = state.decommit_count;
        uint64_t hot_deferred_events_before =
            heap->core.malloc_free_decommit_hot_deferred_events;
        uint64_t hot_deferred_bytes_before =
            heap->core.malloc_free_decommit_hot_deferred_bytes;

        api->free(heap, reuse);
        expect_true(state.decommit_count == decommit_before_hot_free,
                    "large extent hot free keeps committed pages");
        expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0,
                    "large extent hot hint retained");
        expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0,
                    "large extent hot free retained");
        expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0,
                    "large extent hot free not decommitted");
        expect_true((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0,
                    "large extent retained free keeps commit state hint");
        expect_true(heap->core.malloc_free_decommit_hot_deferred_events >
                        hot_deferred_events_before,
                    "large extent hot deferred event counted");
        expect_true(heap->core.malloc_free_decommit_hot_deferred_bytes >=
                        hot_deferred_bytes_before + decommit_size,
                    "large extent hot deferred bytes counted");
    }
    expect_ok(api->check(heap, 0), "large extent final check");
    expect_ok(api->close(heap), "large extent close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_decommit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t large_size = 12u * page_size;
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_block *block;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "large decommit unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "large decommit unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large decommit unlock vm init");
    vm.user = &state;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "large decommit unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large decommit unlock api");
    state.heap = heap;
    state.api = api;

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "large decommit unlock malloc");
    memset(ptr, 0x53u, large_size);
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "large decommit unlock block lookup");
    expect_ok(api->check(heap, 0), "large decommit unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    state.run_check = 1;
    api->free(heap, ptr);
    state.run_check = 0;

    expect_true(state.decommit_count == 1,
                "large decommit unlock decommit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "large decommit unlock reentrant check ok");
    expect_ok(state.check_result, "large decommit unlock last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "large decommit unlock decommit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "large decommit unlock decommit not counted lock-held");
    expect_true((block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0,
                "large decommit unlock busy flag cleared");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large decommit unlock decommitted");
    expect_ok(api->check(heap, 0), "large decommit unlock check after");

    expect_ok(api->close(heap), "large decommit unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_recommit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t large_size = 12u * page_size;
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *reuse;
    uhm_block *block;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_commit_calls_before;
    int commit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "large recommit unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "large recommit unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large recommit unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "large recommit unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large recommit unlock api");
    state.heap = heap;
    state.api = api;

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "large recommit unlock malloc");
    memset(ptr, 0x63u, large_size);
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "large recommit unlock block lookup");
    api->free(heap, ptr);
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large recommit unlock decommitted before reuse");
    expect_ok(api->check(heap, 0), "large recommit unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    commit_count_before = state.commit_count;
    state.check_calls = 0;
    state.run_check = 1;
    reuse = api->malloc(heap, large_size);
    state.run_check = 0;

    expect_true(reuse == ptr, "large recommit unlock same block reused");
    expect_true(state.commit_count == commit_count_before + 1,
                "large recommit unlock commit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "large recommit unlock reentrant check ok");
    expect_ok(state.check_result, "large recommit unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "large recommit unlock commit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before,
                "large recommit unlock commit not counted lock-held");
    expect_true((block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0,
                "large recommit unlock busy flag cleared");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0,
                "large recommit unlock decommitted flag cleared");
    memset(reuse, 0x64u, large_size);
    expect_ok(api->check(heap, 0), "large recommit unlock check after");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "large recommit unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_realloc_recommit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t first_size = 4u * page_size;
    const size_t second_size = 12u * page_size;
    const size_t new_size = 10u * page_size;
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *second;
    void *grown;
    uhm_block *first_block;
    uhm_block *second_block;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_commit_calls_before;
    int commit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "large realloc unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "large realloc unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large realloc unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "large realloc unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large realloc unlock api");
    state.heap = heap;
    state.api = api;

    first = api->malloc(heap, first_size);
    second = api->malloc(heap, second_size);
    expect_true(first != 0 && second != 0,
                "large realloc unlock adjacent mallocs");
    first_block = test_find_block_by_payload(heap, first);
    second_block = test_find_block_by_payload(heap, second);
    expect_true(first_block != 0 && second_block != 0 &&
                    first_block->next_phys == second_block,
                "large realloc unlock physical neighbors");
    memset(first, 0x51u, first_size);
    memset(second, 0x52u, second_size);

    api->free(heap, second);
    expect_true((second_block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large realloc unlock next block decommitted");
    expect_ok(api->check(heap, 0), "large realloc unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    commit_count_before = state.commit_count;
    state.check_calls = 0;
    state.check_failures = 0;
    state.check_result = UHM_ERR_CORRUPTION;
    state.run_check = 1;
    grown = api->realloc(heap, first, new_size);
    state.run_check = 0;

    expect_true(grown == first, "large realloc unlock grew in place");
    expect_true(state.commit_count == commit_count_before + 1,
                "large realloc unlock commit callback called");
    expect_true(state.check_calls >= 1 && state.check_failures == 0,
                "large realloc unlock reentrant check ok");
    expect_ok(state.check_result, "large realloc unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "large realloc unlock commit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before,
                "large realloc unlock commit not counted lock-held");
    first_block = test_find_block_by_payload(heap, grown);
    expect_true(first_block != 0 &&
                    (first_block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0,
                "large realloc unlock grown block committed");
    memset(grown, 0x53u, new_size);
    expect_ok(api->check(heap, 0), "large realloc unlock check after");

    api->free(heap, grown);
    expect_ok(api->close(heap), "large realloc unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_coalesce_recommit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t block_size = 12u * page_size;
    const size_t arena_size = 8u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *middle;
    void *last;
    uhm_block *first_block;
    uhm_block *middle_block;
    uhm_block *last_block;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_commit_calls_before;
    uint64_t vm_decommit_calls_before;
    uint64_t page_mark_pages_before;
    uint64_t page_mark_pages_after;
    uintptr_t middle_decommit_addr;
    size_t middle_decommit_size;
    size_t middle_decommit_first;
    size_t middle_decommit_count;
    int commit_count_before;
    int decommit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "large coalesce unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "large coalesce unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large coalesce unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "large coalesce unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large coalesce unlock api");
    state.heap = heap;
    state.api = api;

    first = api->malloc(heap, block_size);
    middle = api->malloc(heap, block_size);
    last = api->malloc(heap, block_size);
    expect_true(first != 0 && middle != 0 && last != 0,
                "large coalesce unlock mallocs");
    first_block = test_find_block_by_payload(heap, first);
    middle_block = test_find_block_by_payload(heap, middle);
    last_block = test_find_block_by_payload(heap, last);
    expect_true(first_block != 0 && middle_block != 0 && last_block != 0 &&
                    first_block->next_phys == middle_block &&
                    middle_block->next_phys == last_block,
                "large coalesce unlock physical chain");
    memset(first, 0x61u, block_size);
    memset(middle, 0x62u, block_size);
    memset(last, 0x63u, block_size);

    api->free(heap, first);
    api->free(heap, last);
    expect_true((first_block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0 &&
                    (last_block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large coalesce unlock outer blocks decommitted");
    expect_ok(api->check(heap, 0), "large coalesce unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    page_mark_pages_before =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    expect_true(test_block_decommit_range(heap, middle_block,
                                          &middle_decommit_addr,
                                          &middle_decommit_size),
                "large coalesce middle decommit range");
    expect_true(test_shadow_page_span(heap, middle_decommit_addr,
                                      middle_decommit_size,
                                      &middle_decommit_first,
                                      &middle_decommit_count),
                "large coalesce middle decommit span");
    commit_count_before = state.commit_count;
    decommit_count_before = state.decommit_count;
    state.check_calls = 0;
    state.check_failures = 0;
    state.check_result = UHM_ERR_CORRUPTION;
    state.run_check = 1;
    api->free(heap, middle);
    state.run_check = 0;

    expect_true(state.commit_count == commit_count_before,
                "large coalesce avoids merge recommit callbacks");
    expect_true(state.decommit_count == decommit_count_before + 1,
                "large coalesce decommits only merged committed gap");
    expect_true(state.check_calls >= 1 && state.check_failures == 0,
                "large coalesce unlock reentrant checks ok");
    expect_ok(state.check_result, "large coalesce unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before,
                "large coalesce merge does not recommit");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "large coalesce final gap decommit counted");
    page_mark_pages_after =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    expect_true(page_mark_pages_after > page_mark_pages_before &&
                    page_mark_pages_after - page_mark_pages_before <=
                        (uint64_t)middle_decommit_count + 2u,
                "large coalesce page-map publish only touches committed gap");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "large coalesce unlock VM calls not counted lock-held");
    expect_true(first_block->magic == UHM_BLOCK_MAGIC &&
                    (first_block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                    (first_block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 &&
                    (first_block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large coalesce unlock final block decommitted");
    expect_ok(api->check(heap, 0), "large coalesce unlock check after");

    expect_ok(api->close(heap), "large coalesce unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_mixed_decommit_batch_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t block_size = 96u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    size_t decommit_first;
    size_t decommit_count;
    size_t hole_pages = 0;
    size_t live_pages = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;
    uint64_t page_mark_pages_before;
    uint64_t page_mark_pages_after;
    int decommit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "large mixed batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "large mixed batch arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large mixed batch vm init");
    vm.user = &state;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "large mixed batch open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large mixed batch api");
    state.heap = heap;
    state.api = api;

    ptr = api->malloc(heap, block_size);
    expect_true(ptr != 0, "large mixed batch malloc");
    memset(ptr, 0x6bu, block_size);
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "large mixed batch block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size),
                "large mixed batch decommit range");
    expect_true(test_shadow_page_span(heap, decommit_addr, decommit_size,
                                      &decommit_first, &decommit_count),
                "large mixed batch page span");
    expect_true(decommit_count >= 40u,
                "large mixed batch enough pages for overflow-sized islands");
    for (size_t i = 1; i < decommit_count; i += 2u) {
        uhm_page_record *hole =
            test_page_record_at(heap, decommit_first + i);
        expect_true(hole != 0, "large mixed batch hole page record");
        expect_true(hole->owner == UHM_PAGE_OWNER_MALLOC &&
                        hole->va_state == UHM_PAGE_VA_MALLOC_LIVE &&
                        hole->commit_state == UHM_PAGE_COMMIT_LIVE &&
                        hole->prot == (UHM_PROT_READ | UHM_PROT_WRITE),
                    "large mixed batch hole starts live");
        hole->commit_state = UHM_PAGE_COMMIT_DECOMMITTED;
        hole->prot = UHM_PROT_NONE;
        ++hole_pages;
    }
    live_pages = decommit_count - hole_pages;
    expect_true(live_pages > 16u && live_pages <= UHM_BLOCK_DECOMMIT_SEGMENT_MAX,
                "large mixed batch exercises extended segment capacity");
    /* The test deliberately edits page-map records to create a mixed state;
       invalidate the commit-state hint to model that external mutation. */
    block->flags &= ~UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    page_mark_pages_before =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    decommit_count_before = state.decommit_count;
    state.check_calls = 0;
    state.check_failures = 0;
    state.check_result = UHM_ERR_CORRUPTION;
    state.run_check = 1;
    api->free(heap, ptr);
    state.run_check = 0;

    expect_true(state.decommit_count == decommit_count_before + 1,
                "large mixed batch uses one decommit callback");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "large mixed batch reentrant check ok");
    expect_ok(state.check_result, "large mixed batch last check ok");
    expect_true(state.last_decommit_addr == (void *)decommit_addr &&
                    state.last_decommit_size == decommit_size,
                "large mixed batch decommits full mixed range once");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "large mixed batch VM decommit counted once");
    page_mark_pages_after =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    expect_true(page_mark_pages_after > page_mark_pages_before &&
                    page_mark_pages_after - page_mark_pages_before <=
                        (uint64_t)live_pages,
                "large mixed batch page-map skips many decommitted holes");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "large mixed batch VM call not lock-held");
    expect_true(block->magic == UHM_BLOCK_MAGIC &&
                    (block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                    (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 &&
                    (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large mixed batch final block decommitted");
    expect_ok(api->check(heap, 0), "large mixed batch check after");

    expect_ok(api->close(heap), "large mixed batch close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_new_commit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t large_size = 2u * page_size;
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_block *block;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_commit_calls_before;
    int commit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "large new commit unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "large new commit unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large new commit unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;

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

    expect_ok(uhm_open(&config, &heap), "large new commit unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large new commit unlock api");
    state.heap = heap;
    state.api = api;

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    commit_count_before = state.commit_count;
    state.run_check = 1;
    ptr = api->malloc(heap, large_size);
    state.run_check = 0;

    expect_true(ptr != 0, "large new commit unlock malloc");
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "large new commit unlock block lookup");
    expect_true(state.commit_count == commit_count_before + 1,
                "large new commit unlock commit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "large new commit unlock reentrant check ok");
    expect_ok(state.check_result, "large new commit unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "large new commit unlock commit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before,
                "large new commit unlock commit not counted lock-held");
    expect_true(heap->core.low_watermark_claim_inflight == 0u,
                "large new commit unlock low claim cleared");
    expect_ok(api->check(heap, 0), "large new commit unlock check after");

    api->free(heap, ptr);
    expect_ok(api->close(heap), "large new commit unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_cold_split_prefix_recommit(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t large_size = 64u * page_size;
    const size_t small_size = 6u * page_size;
    const size_t arena_size = 16u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *reuse;
    uhm_block *block;
    uhm_block *remainder;
    uintptr_t decommit_addr;
    size_t decommit_size;
    size_t source_decommit_size;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_commit_calls_before;
    uint64_t vm_decommit_calls_before;
    int commit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "large split unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "large split unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large split unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "large split unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large split unlock api");
    state.heap = heap;
    state.api = api;

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "large split unlock initial malloc");
    memset(ptr, 0x73u, large_size);
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "large split unlock initial block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &source_decommit_size) != 0,
                "large split unlock source has decommit range");
    api->free(heap, ptr);
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large split unlock source block decommitted");
    expect_ok(api->check(heap, 0), "large split unlock check before");

    state.decommit_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    state.check_result = UHM_ERR_CORRUPTION;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    commit_count_before = state.commit_count;
    state.run_check = 1;
    reuse = api->malloc(heap, small_size);
    state.run_check = 0;

    expect_true(reuse == ptr, "large split unlock reused source block");
    expect_true(state.commit_count == commit_count_before + 1,
                "large split unlock prefix commit callback called");
    expect_true(state.last_commit_addr == (void *)decommit_addr,
                "large split unlock prefix commit starts at extent interior");
    expect_true(state.last_commit_size < source_decommit_size,
                "large split unlock prefix commit is smaller than extent");
    expect_true(state.decommit_count == 0,
                "large split unlock skips remainder decommit callback");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "large split unlock reentrant check ok");
    expect_ok(state.check_result, "large split unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "large split unlock prefix commit call counted");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before,
                "large split unlock no split decommit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "large split unlock VM calls not counted lock-held");

    block = test_find_block_by_payload(heap, reuse);
    expect_true(block != 0, "large split unlock reused block lookup");
    remainder = block->next_phys;
    expect_true(remainder != 0, "large split unlock remainder exists");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_FREE) != 0,
                "large split unlock remainder free");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0,
                "large split unlock remainder busy flag cleared");
    expect_true(test_block_decommit_range(heap, remainder, &decommit_addr,
                                          &decommit_size) != 0,
                "large split unlock remainder has decommit range");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large split unlock remainder decommitted");
    expect_true((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0,
                "large split unlock prefix commit state known");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) == 0,
                "large split unlock remainder commit state remains unknown");
    expect_ok(api->check(heap, 0), "large split unlock check after");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "large split unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_hot_split_remainder_retained(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t large_size = 32u * page_size;
    const size_t small_size = 6u * page_size;
    const size_t arena_size = 16u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *warm;
    void *reuse;
    uhm_block *block;
    uhm_block *remainder;
    uintptr_t decommit_addr;
    size_t decommit_size;
    int decommit_before_reuse;
    uint64_t hot_deferred_before;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "hot split retained heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "hot split retained arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "hot split retained vm init");
    vm.user = &state;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "hot split retained open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "hot split retained api");

    ptr = api->malloc(heap, large_size);
    expect_true(ptr != 0, "hot split retained initial malloc");
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "hot split retained initial block lookup");
    api->free(heap, ptr);
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "hot split retained first free decommits");

    warm = api->malloc(heap, large_size);
    expect_true(warm == ptr, "hot split retained warm reuse");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0,
                "hot split retained warm reuse marks hot");
    api->free(heap, warm);
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0,
                "hot split retained warm free stays committed");

    decommit_before_reuse = state.decommit_count;
    hot_deferred_before =
        heap->core.malloc_free_decommit_hot_deferred_events;
    reuse = api->malloc(heap, small_size);
    expect_true(reuse == ptr, "hot split retained reused source block");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0,
                "hot split retained source is hot");
    expect_true(state.decommit_count == decommit_before_reuse,
                "hot split retained avoids split remainder decommit");

    block = test_find_block_by_payload(heap, reuse);
    expect_true(block != 0, "hot split retained reused block lookup");
    remainder = block->next_phys;
    expect_true(remainder != 0, "hot split retained remainder exists");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_FREE) != 0,
                "hot split retained remainder free");
    expect_true(test_block_decommit_range(heap, remainder, &decommit_addr,
                                          &decommit_size) != 0,
                "hot split retained remainder has decommit range");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0,
                "hot split retained remainder inherited hot hint");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0,
                "hot split retained remainder retained");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0,
                "hot split retained remainder stays committed");
    expect_true((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0,
                "hot split retained source commit state known");
    expect_true((remainder->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0,
                "hot split retained remainder commit state known");
    expect_true(heap->core.malloc_free_decommit_retained_bytes >=
                    decommit_size,
                "hot split retained bytes counted");
    expect_true(heap->core.malloc_free_decommit_hot_deferred_events >
                    hot_deferred_before,
                "hot split retained hot deferred counted");
    expect_ok(api->check(heap, 0), "hot split retained check after");

    api->free(heap, reuse);
    expect_ok(api->check(heap, 0), "hot split retained final check");
    expect_ok(api->close(heap), "hot split retained close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

