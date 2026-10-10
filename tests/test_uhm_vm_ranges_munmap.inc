static void test_munmap_exact_vma_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 2u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "exact munmap unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "exact munmap unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "exact munmap unlock vm init");
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
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "exact munmap unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "exact munmap unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "exact munmap unlock mmap");
    expect_true(map != 0, "exact munmap unlock address");
    expect_ok(api->check(heap, 0), "exact munmap unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    state.run_check = 1;
    expect_ok(api->munmap(heap, map, map_size),
              "exact munmap unlock munmap");
    state.run_check = 0;
    expect_true(state.decommit_count == 1,
                "exact munmap unlock decommit callback called");
    expect_true(state.check_calls == 1,
                "exact munmap unlock reentrant check called");
    expect_ok(state.check_result, "exact munmap unlock reentrant check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "exact munmap unlock decommit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "exact munmap unlock decommit not counted lock-held");
    expect_ok(api->check(heap, 0), "exact munmap unlock check after");

    expect_ok(api->close(heap), "exact munmap unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_exact_vma_skips_publish_ready_precheck(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 2u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    const uint64_t expected_query_delta = (uint64_t)(page_count * 2u);
    const uint64_t expected_span_delta = (uint64_t)(page_count * 4u);
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t query_pages_before;
    uint64_t query_pages_after;
    uint64_t page_span_before;
    uint64_t page_span_after;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "exact munmap span heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "exact munmap span arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "exact munmap span vm init");

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

    expect_ok(uhm_open(&config, &heap), "exact munmap span open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "exact munmap span api");

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "exact munmap span mmap");
    expect_true(map != 0, "exact munmap span address");
    query_pages_before =
        test_atomic_u64_load(&heap->core.page_range_query_pages);
    page_span_before = test_atomic_u64_load(&heap->core.page_span_pages);
    expect_ok(api->munmap(heap, map, map_size),
              "exact munmap span munmap");
    page_span_after = test_atomic_u64_load(&heap->core.page_span_pages);
    query_pages_after =
        test_atomic_u64_load(&heap->core.page_range_query_pages);
    expect_true(page_span_after == page_span_before + expected_span_delta,
                "exact munmap skips publish-ready page span");
    expect_true(query_pages_after == query_pages_before + expected_query_delta,
                "exact munmap fuses initial live/protecting queries");
    expect_ok(api->check(heap, 0), "exact munmap span check after");

    expect_ok(api->close(heap), "exact munmap span close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_trim_vma_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 4u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "trim munmap unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "trim munmap unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "trim munmap unlock vm init");
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
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "trim munmap unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "trim munmap unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "trim munmap unlock mmap");
    expect_true(map != 0, "trim munmap unlock address");
    memset(map, 0x7a, map_size);
    expect_ok(api->check(heap, 0), "trim munmap unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    state.run_check = 1;
    expect_ok(api->munmap(heap, map, page_size),
              "trim munmap unlock head");
    expect_ok(api->munmap(heap, (unsigned char *)map + 3u * page_size,
                          page_size),
              "trim munmap unlock tail");
    state.run_check = 0;
    expect_true(state.decommit_count == 2,
                "trim munmap unlock decommit callbacks called");
    expect_true(state.check_calls == 2 && state.check_failures == 0,
                "trim munmap unlock reentrant checks ok");
    expect_ok(state.check_result, "trim munmap unlock last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 2u,
                "trim munmap unlock decommit calls counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "trim munmap unlock decommit not counted lock-held");
    expect_ok(api->check(heap, 0), "trim munmap unlock check after");

    expect_ok(api->munmap(heap, (unsigned char *)map + page_size,
                          2u * page_size),
              "trim munmap unlock cleanup");
    expect_ok(api->close(heap), "trim munmap unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_middle_vma_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 4u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "middle munmap unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "middle munmap unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "middle munmap unlock vm init");
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
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "middle munmap unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "middle munmap unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "middle munmap unlock mmap");
    expect_true(map != 0, "middle munmap unlock address");
    memset(map, 0x63, map_size);
    expect_ok(api->check(heap, 0), "middle munmap unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    state.run_check = 1;
    expect_ok(api->munmap(heap, (unsigned char *)map + page_size,
                          2u * page_size),
              "middle munmap unlock middle hole");
    state.run_check = 0;
    expect_true(state.decommit_count == 1,
                "middle munmap unlock decommit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "middle munmap unlock reentrant check ok");
    expect_ok(state.check_result, "middle munmap unlock last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "middle munmap unlock decommit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "middle munmap unlock decommit not counted lock-held");
    expect_ok(api->check(heap, 0), "middle munmap unlock check after");

    expect_ok(api->munmap(heap, map, page_size),
              "middle munmap unlock cleanup left");
    expect_ok(api->munmap(heap, (unsigned char *)map + 3u * page_size,
                          page_size),
              "middle munmap unlock cleanup right");
    expect_ok(api->close(heap), "middle munmap unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_cross_vma_exact_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 4u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "cross munmap unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "cross munmap unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "cross munmap unlock vm init");
    vm.user = &state;
    vm.decommit = reentrant_check_vm_decommit;
    vm.protect = reentrant_check_vm_protect;

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

    expect_ok(uhm_open(&config, &heap), "cross munmap unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "cross munmap unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "cross munmap unlock mmap");
    expect_true(map != 0, "cross munmap unlock address");
    memset(map, 0x6b, map_size);
    expect_ok(api->mprotect(heap, (unsigned char *)map + page_size,
                            page_size, UHM_PROT_READ),
              "cross munmap unlock split page 1");
    expect_ok(api->mprotect(heap, (unsigned char *)map + 3u * page_size,
                            page_size, UHM_PROT_READ),
              "cross munmap unlock split page 3");
    expect_ok(api->check(heap, 0), "cross munmap unlock check before");

    state.check_result = UHM_ERR_CORRUPTION;
    state.decommit_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    state.run_check = 1;
    expect_ok(api->munmap(heap, map, map_size),
              "cross munmap unlock munmap");
    state.run_check = 0;
    expect_true(state.decommit_count == 1,
                "cross munmap unlock decommit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "cross munmap unlock reentrant check ok");
    expect_ok(state.check_result, "cross munmap unlock last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "cross munmap unlock decommit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "cross munmap unlock decommit not counted lock-held");
    expect_ok(api->check(heap, 0), "cross munmap unlock check after");

    expect_ok(api->close(heap), "cross munmap unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_cross_vma_partial_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 6u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "cross partial munmap unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "cross partial munmap unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "cross partial munmap unlock vm init");
    vm.user = &state;
    vm.decommit = reentrant_check_vm_decommit;
    vm.protect = reentrant_check_vm_protect;

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

    expect_ok(uhm_open(&config, &heap), "cross partial munmap unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "cross partial munmap unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "cross partial munmap unlock mmap");
    expect_true(map != 0, "cross partial munmap unlock address");
    memset(map, 0x58, map_size);
    expect_ok(api->mprotect(heap, (unsigned char *)map + 2u * page_size,
                            page_size, UHM_PROT_READ),
              "cross partial munmap unlock split middle page");
    expect_ok(api->check(heap, 0), "cross partial munmap unlock check before");

    state.check_result = UHM_ERR_CORRUPTION;
    state.decommit_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    state.run_check = 1;
    expect_ok(api->munmap(heap, (unsigned char *)map + page_size,
                          3u * page_size),
              "cross partial munmap unlock munmap");
    state.run_check = 0;
    expect_true(state.decommit_count == 1,
                "cross partial munmap unlock decommit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "cross partial munmap unlock reentrant check ok");
    expect_ok(state.check_result,
              "cross partial munmap unlock last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "cross partial munmap unlock decommit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "cross partial munmap unlock decommit not counted lock-held");
    expect_ok(api->check(heap, 0), "cross partial munmap unlock check after");

    expect_ok(api->munmap(heap, map, page_size),
              "cross partial munmap unlock cleanup left");
    expect_ok(api->munmap(heap, (unsigned char *)map + 4u * page_size,
                          2u * page_size),
              "cross partial munmap unlock cleanup right");
    expect_ok(api->close(heap), "cross partial munmap unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_no_commit_skips_platform_decommit(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *no_commit_map = 0;
    void *high_map = 0;
    void *low_map = 0;
    uintptr_t high_addr;
    uintptr_t low_addr;
    uint64_t vm_decommit_calls_before;
    uint64_t vm_decommit_lock_held_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "no-commit munmap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "no-commit munmap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "no-commit munmap vm init");
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
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "no-commit munmap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "no-commit munmap api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &no_commit_map, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "no-commit munmap mmap");
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    state.run_check = 1;
    expect_ok(api->munmap(heap, no_commit_map, page_size * 2u),
              "no-commit munmap exact");
    state.run_check = 0;
    expect_true(state.decommit_count == 0,
                "no-commit munmap skips decommit callback");
    expect_true(state.check_calls == 0,
                "no-commit munmap does not run decommit reentrant check");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "no-commit munmap skips decommit counters");
    expect_ok(api->check(heap, 0), "no-commit munmap exact check");

    expect_ok(api->mmap(heap, &high_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mixed no-commit munmap high mmap");
    high_addr = (uintptr_t)high_map;
    expect_true(high_addr >= heap->core.arena_base + page_size,
                "mixed no-commit munmap has lower page");
    low_addr = high_addr - page_size;
    low_map = (void *)low_addr;
    expect_ok(api->mmap(heap, &low_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT |
                            UHM_MAP_FIXED_NOREPLACE),
              "mixed no-commit munmap low fixed mmap");
    expect_true((uintptr_t)low_map == low_addr,
                "mixed no-commit munmap low fixed address");

    state.decommit_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    state.check_result = UHM_ERR_CORRUPTION;
    state.last_decommit_addr = 0;
    state.last_decommit_size = 0;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    state.run_check = 1;
    expect_ok(api->munmap(heap, low_map, page_size * 2u),
              "mixed no-commit munmap range");
    state.run_check = 0;
    expect_true(state.decommit_count == 1,
                "mixed no-commit munmap decommits committed island only");
    expect_true(state.last_decommit_addr == high_map &&
                    state.last_decommit_size == page_size,
                "mixed no-commit munmap decommit range");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "mixed no-commit munmap reentrant check ok");
    expect_ok(state.check_result, "mixed no-commit munmap last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "mixed no-commit munmap decommit counted unlocked once");
    expect_ok(api->check(heap, 0), "mixed no-commit munmap check");

    expect_ok(api->close(heap), "no-commit munmap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_adjacent_free_extent_skips_node_alloc(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *target;
    void *bridge_target;
    void *right_hole;
    uhm_free_extent *left_extent;
    uhm_free_extent *right_extent;
    uintptr_t left_extent_end;
    uhm_free_extent *saved_free_extents;
    size_t saved_free_extent_count;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "munmap adjacent free heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "munmap adjacent free arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "munmap adjacent free vm init");

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

    expect_ok(uhm_open(&config, &heap), "munmap adjacent free open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "munmap adjacent free api");
    expect_ok(api->mmap(heap, &map, page_size * 5u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "munmap adjacent free mmap");
    expect_true(map != 0, "munmap adjacent free address");
    expect_true(test_find_free_extent_containing(heap, map) == 0,
                "munmap adjacent free starts live");
    expect_ok(api->munmap(heap, (void *)((uintptr_t)map + page_size),
                          page_size),
              "munmap adjacent free setup left neighbor");
    target = (void *)((uintptr_t)map + page_size * 2u);
    left_extent = test_find_free_extent_containing(
        heap, (void *)((uintptr_t)map + page_size));
    expect_true(left_extent != 0,
                "munmap adjacent free left neighbor exists");
    left_extent_end = left_extent->addr + left_extent->size;
    expect_true(left_extent_end >= left_extent->addr &&
                    left_extent_end == (uintptr_t)target,
                "munmap adjacent free left neighbor touches target");

    saved_free_extents = heap->core.emergency_free_extent_head;
    saved_free_extent_count = heap->core.emergency_free_extent_available;
    heap->core.emergency_free_extent_head = 0;
    heap->core.emergency_free_extent_available = 0;
    heap->core.fault_control_alloc_countdown = 1u;
    expect_ok(api->munmap(heap, target, page_size),
              "munmap adjacent free skips free node alloc");
    expect_true(heap->core.fault_control_alloc_countdown == 1u,
                "munmap adjacent free allocation hook not consumed");
    heap->core.fault_control_alloc_countdown = 0;
    heap->core.emergency_free_extent_head = saved_free_extents;
    heap->core.emergency_free_extent_available = saved_free_extent_count;
    expect_true(test_find_free_extent_containing(heap, target) != 0,
                "munmap adjacent free merged free extent");
    expect_ok(api->check(heap, 0), "munmap adjacent free check after");

    right_hole = (void *)((uintptr_t)map + page_size * 4u);
    expect_ok(api->munmap(heap, right_hole, page_size),
              "munmap adjacent free setup right neighbor");
    right_extent = test_find_free_extent_containing(heap, right_hole);
    expect_true(right_extent != 0,
                "munmap adjacent free right neighbor exists");
    bridge_target = (void *)((uintptr_t)map + page_size * 3u);
    left_extent = test_find_free_extent_containing(heap, target);
    expect_true(left_extent && left_extent->next == right_extent,
                "munmap adjacent free dual neighbors are directly linked");

    heap->core.fault_control_alloc_countdown = 1u;
    expect_ok(api->munmap(heap, bridge_target, page_size),
              "munmap adjacent free bridges both neighbors");
    expect_true(heap->core.fault_control_alloc_countdown == 1u,
                "munmap adjacent free bridge skips node allocation");
    heap->core.fault_control_alloc_countdown = 0;
    left_extent = test_find_free_extent_containing(heap, target);
    expect_true(left_extent &&
                    left_extent->addr == (uintptr_t)map + page_size &&
                    left_extent->size == page_size * 4u &&
                    left_extent->next != right_extent,
                "munmap adjacent free bridge retires right extent");
    expect_ok(api->check(heap, 0),
              "munmap adjacent free bridge check after");

    expect_ok(api->munmap(heap, map, page_size),
              "munmap adjacent free cleanup live prefix");

    expect_ok(api->close(heap), "munmap adjacent free close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

typedef struct munmap_reentrant_neighbor_state {
    const uhm_api *api;
    uhm_heap *heap;
    void *neighbor;
    size_t neighbor_size;
    int invoked;
    uhm_result munmap_result;
} munmap_reentrant_neighbor_state;

static uhm_result UHM_CALL
munmap_reentrant_neighbor_decommit(void *user, void *addr, size_t size) {
    munmap_reentrant_neighbor_state *state =
        (munmap_reentrant_neighbor_state *)user;
    (void)addr;
    (void)size;
    if (state && !state->invoked && state->api && state->heap &&
        state->neighbor) {
        state->invoked = 1;
        state->munmap_result = state->api->munmap(
            state->heap, state->neighbor, state->neighbor_size);
    }
    return UHM_OK;
}

static void test_munmap_rechecks_adjacent_after_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    munmap_reentrant_neighbor_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *span = 0;
    void *left;
    void *target;
    void *tail;
    uhm_free_extent *extent;

    memset(&state, 0, sizeof(state));
    state.munmap_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "munmap relock adjacent heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "munmap relock adjacent arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "munmap relock adjacent vm init");
    vm.user = &state;
    vm.decommit = munmap_reentrant_neighbor_decommit;

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

    expect_ok(uhm_open(&config, &heap), "munmap relock adjacent open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "munmap relock adjacent api");
    state.api = api;
    state.heap = heap;

    expect_ok(api->mmap(heap, &span, page_size * 3u,
                        UHM_PROT_NONE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "munmap relock adjacent no-commit span");
    expect_true(span != 0, "munmap relock adjacent span address");
    left = span;
    target = (void *)((uintptr_t)span + page_size);
    tail = (void *)((uintptr_t)span + 2u * page_size);
    expect_ok(api->mmap(heap, &left, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "munmap relock adjacent left fixed live");
    expect_true(left == span, "munmap relock adjacent left address");
    expect_ok(api->mmap(heap, &target, page_size, UHM_PROT_READ,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "munmap relock adjacent target fixed live");
    expect_true(target == (void *)((uintptr_t)span + page_size),
                "munmap relock adjacent target address");
    expect_ok(api->check(heap, 0), "munmap relock adjacent check before");

    state.neighbor = left;
    state.neighbor_size = page_size;
    heap->core.fault_mmap_free_insert_countdown = 2u;
    expect_ok(api->munmap(heap, target, page_size),
              "munmap relock adjacent target");
    expect_true(state.invoked == 1,
                "munmap relock adjacent reentrant neighbor invoked");
    expect_ok(state.munmap_result,
              "munmap relock adjacent reentrant neighbor result");
    expect_true(heap->core.fault_mmap_free_insert_countdown == 1u,
                "munmap relock adjacent skipped second free insert");
    heap->core.fault_mmap_free_insert_countdown = 0;

    extent = test_find_free_extent_containing(heap, left);
    expect_true(extent != 0, "munmap relock adjacent merged extent exists");
    expect_true(extent->addr == (uintptr_t)left &&
                    extent->size >= page_size * 2u,
                "munmap relock adjacent merged left and target");
    expect_ok(api->check(heap, 0), "munmap relock adjacent check after");
    expect_ok(api->munmap(heap, tail, page_size),
              "munmap relock adjacent tail cleanup");
    expect_ok(api->check(heap, 0), "munmap relock adjacent final check");

    expect_ok(api->close(heap), "munmap relock adjacent close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_make_three_separated_live_islands(
    const uhm_api *api,
    uhm_heap *heap,
    size_t page_size,
    void **span_out) {
    const size_t span_size = 5u * page_size;
    void *span = 0;
    uintptr_t base;
    void *live_page;

    expect_ok(api->mmap(heap, &span, span_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "separated munmap no-commit span");
    expect_true(span != 0, "separated munmap span address");
    base = (uintptr_t)span;

    live_page = (void *)base;
    expect_ok(api->mmap(heap, &live_page, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "separated munmap live page 0");
    expect_true(live_page == (void *)base,
                "separated munmap live page 0 address");

    live_page = (void *)(base + 2u * page_size);
    expect_ok(api->mmap(heap, &live_page, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "separated munmap live page 2");
    expect_true(live_page == (void *)(base + 2u * page_size),
                "separated munmap live page 2 address");

    live_page = (void *)(base + 4u * page_size);
    expect_ok(api->mmap(heap, &live_page, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "separated munmap live page 4");
    expect_true(live_page == (void *)(base + 4u * page_size),
                "separated munmap live page 4 address");

    expect_true(test_vma_count_overlapping(heap, base, span_size) == 5u,
                "separated munmap VMA setup count");
    *span_out = span;
}

static void test_munmap_separated_live_islands_decommit_batches(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t span_size = 5u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *span = 0;
    uintptr_t base;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "separated munmap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "separated munmap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "separated munmap vm init");
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

    expect_ok(uhm_open(&config, &heap), "separated munmap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "separated munmap api");

    test_munmap_make_three_separated_live_islands(api, heap, page_size,
                                                  &span);
    base = (uintptr_t)span;
    expect_ok(api->check(heap, 0), "separated munmap check before");

    state.commit_count = 0;
    state.decommit_count = 0;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    expect_ok(api->munmap(heap, span, span_size),
              "separated munmap whole range");
    expect_true(state.decommit_count == 3,
                "separated munmap decommits only live islands");
    expect_true(state.commit_count == 0,
                "separated munmap success does not rollback commit");
    expect_true(state.last_decommit_addr == (void *)(base + 4u * page_size) &&
                    state.last_decommit_size == page_size,
                "separated munmap last decommit island");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 3u,
                "separated munmap decommit calls counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "separated munmap decommit stays unlocked");
    expect_ok(api->check(heap, 0), "separated munmap check after");

    expect_ok(api->close(heap), "separated munmap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_separated_live_island_failure_rolls_back(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t span_size = 5u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    const uint32_t platform_error = test_platform_error_code();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *span = 0;
    uintptr_t base;
    uhm_stats before;
    uhm_stats after;
    int commit_count_before;

    memset(&state, 0, sizeof(state));
    state.platform_error = platform_error;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "separated munmap failure heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "separated munmap failure arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "separated munmap failure vm init");
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

    expect_ok(uhm_open(&config, &heap),
              "separated munmap failure open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "separated munmap failure api");

    test_munmap_make_three_separated_live_islands(api, heap, page_size,
                                                  &span);
    base = (uintptr_t)span;
    expect_ok(api->check(heap, 0), "separated munmap failure check before");
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before),
              "separated munmap failure stats before");

    state.decommit_count = 0;
    state.fault_match_count = 0;
    commit_count_before = state.commit_count;
    state.fail_ops = FAULT_VM_OP_DECOMMIT;
    state.fail_on_match = 2u;
    expect_true(api->munmap(heap, span, span_size) == UHM_ERR_PLATFORM,
                "separated munmap failure reported");
    test_set_platform_error(platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "separated munmap failure captures platform error");
    expect_true(state.decommit_count == 2,
                "separated munmap stops after failed island");
    expect_true(state.fault_match_count == 2u,
                "separated munmap failure ordinal count");
    expect_true(state.commit_count == commit_count_before + 1,
                "separated munmap rolls back successful island commit");

    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after),
              "separated munmap failure stats after");
    expect_true(after.mmap_live_bytes == before.mmap_live_bytes,
                "separated munmap failure keeps mmap live bytes");
    expect_true(after.committed_bytes == before.committed_bytes,
                "separated munmap failure keeps committed bytes");
    expect_true(after.decommitted_bytes == before.decommitted_bytes,
                "separated munmap failure keeps decommitted bytes");
    expect_true(after.logical_decommitted_bytes ==
                    before.logical_decommitted_bytes,
                "separated munmap failure keeps logical decommit bytes");
    expect_true(after.physical_released_bytes ==
                    before.physical_released_bytes,
                "separated munmap failure keeps physical release bytes");
    expect_true(test_vma_count_overlapping(heap, base, span_size) == 5u,
                "separated munmap failure keeps VMA setup");
    expect_ok(api->check(heap, 0), "separated munmap failure check after");

    fault_vm_clear_rules(&state);
    state.decommit_count = 0;
    expect_ok(api->munmap(heap, span, span_size),
              "separated munmap failure cleanup");
    expect_true(state.decommit_count == 3,
                "separated munmap failure cleanup decommits islands");
    expect_ok(api->close(heap), "separated munmap failure close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_protecting_vma_split_barrier(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 4u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    state.mprotect_probe_result = UHM_OK;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "protecting split barrier heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "protecting split barrier arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "protecting split barrier vm init");
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
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "protecting split barrier open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "protecting split barrier api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "protecting split barrier mmap");
    expect_true(map != 0, "protecting split barrier address");
    memset(map, 0x5e, map_size);
    expect_ok(api->check(heap, 0), "protecting split barrier check before");

    state.run_check = 1;
    state.run_mprotect_probe = 1;
    state.mprotect_probe_addr = (unsigned char *)map + 2u * page_size;
    state.mprotect_probe_size = page_size;
    state.mprotect_probe_prot = UHM_PROT_READ;
    expect_ok(api->munmap(heap, map, page_size),
              "protecting split barrier munmap head");
    state.run_mprotect_probe = 0;
    state.run_check = 0;
    expect_true(state.mprotect_probe_calls == 1,
                "protecting split barrier mprotect probe called");
    expect_true(state.mprotect_probe_result == UHM_ERR_PROTECTION,
                "protecting split barrier mprotect probe blocked");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "protecting split barrier reentrant check ok");
    expect_ok(state.check_result, "protecting split barrier last check ok");
    expect_ok(api->check(heap, 0), "protecting split barrier check after");

    expect_ok(api->munmap(heap, (unsigned char *)map + page_size,
                          3u * page_size),
              "protecting split barrier cleanup");
    expect_ok(api->close(heap), "protecting split barrier close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_munmap_trim_elides_remnant_page_repair(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 8u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *map2 = 0;
    uintptr_t base;
    uintptr_t base2;
    uint64_t mark_pages_before;
    uint64_t elided_before;
    uint64_t elided_pages_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "munmap trim elide heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "munmap trim elide arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "munmap trim elide vm init");

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

    expect_ok(uhm_open(&config, &heap), "munmap trim elide open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "munmap trim elide api");
    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "munmap trim elide mmap");
    expect_true(map != 0, "munmap trim elide address");
    base = (uintptr_t)map;

    mark_pages_before = test_atomic_u64_load(&heap->core.page_mark_pages);
    elided_before = heap->core.munmap_vma_trim_repair_elided;
    elided_pages_before = heap->core.munmap_vma_trim_repair_elided_pages;
    expect_ok(api->munmap(heap, (void *)base, page_size),
              "munmap trim elide head trim");
    expect_true(test_atomic_u64_load(&heap->core.page_mark_pages) ==
                    mark_pages_before + 2u,
                "munmap trim elide head marks protecting and freed page only");
    expect_true(heap->core.munmap_vma_trim_repair_elided ==
                    elided_before + 1u,
                "munmap trim elide head counted");
    expect_true(heap->core.munmap_vma_trim_repair_elided_pages ==
                    elided_pages_before + (page_count - 1u),
                "munmap trim elide head pages counted");
    expect_ok(api->check(heap, 0), "munmap trim elide check after head");

    mark_pages_before = test_atomic_u64_load(&heap->core.page_mark_pages);
    elided_before = heap->core.munmap_vma_trim_repair_elided;
    elided_pages_before = heap->core.munmap_vma_trim_repair_elided_pages;
    expect_ok(api->munmap(heap, (void *)(base + (page_count - 1u) * page_size),
                          page_size),
              "munmap trim elide tail trim");
    expect_true(test_atomic_u64_load(&heap->core.page_mark_pages) ==
                    mark_pages_before + 2u,
                "munmap trim elide tail marks protecting and freed page only");
    expect_true(heap->core.munmap_vma_trim_repair_elided ==
                    elided_before + 1u,
                "munmap trim elide tail counted");
    expect_true(heap->core.munmap_vma_trim_repair_elided_pages ==
                    elided_pages_before + (page_count - 2u),
                "munmap trim elide tail pages counted");
    expect_ok(api->check(heap, 0), "munmap trim elide check after tail");

    mark_pages_before = test_atomic_u64_load(&heap->core.page_mark_pages);
    elided_before = heap->core.munmap_vma_trim_repair_elided;
    elided_pages_before = heap->core.munmap_vma_trim_repair_elided_pages;
    expect_ok(api->munmap(heap, (void *)(base + 3u * page_size), page_size),
              "munmap trim elide middle hole");
    expect_true(test_atomic_u64_load(&heap->core.page_mark_pages) ==
                    mark_pages_before + 4u,
                "munmap trim elide middle marks protecting, free, and left remnant");
    expect_true(heap->core.munmap_vma_trim_repair_elided ==
                    elided_before + 1u,
                "munmap trim elide middle counted");
    expect_true(heap->core.munmap_vma_trim_repair_elided_pages ==
                    elided_pages_before + 3u,
                "munmap trim elide middle right pages counted");
    expect_ok(api->check(heap, 0), "munmap trim elide check after middle");

    expect_ok(api->munmap(heap, (void *)(base + page_size), 2u * page_size),
              "munmap trim elide cleanup low");
    expect_ok(api->munmap(heap, (void *)(base + 4u * page_size),
                          3u * page_size),
              "munmap trim elide cleanup high");
    expect_ok(api->check(heap, 0), "munmap trim elide final check");

    expect_ok(api->mmap(heap, &map2, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "munmap trim elide right-small mmap");
    expect_true(map2 != 0, "munmap trim elide right-small address");
    base2 = (uintptr_t)map2;
    mark_pages_before = test_atomic_u64_load(&heap->core.page_mark_pages);
    elided_before = heap->core.munmap_vma_trim_repair_elided;
    elided_pages_before = heap->core.munmap_vma_trim_repair_elided_pages;
    expect_ok(api->munmap(heap, (void *)(base2 + 5u * page_size), page_size),
              "munmap trim elide right-small middle hole");
    expect_true(test_atomic_u64_load(&heap->core.page_mark_pages) ==
                    mark_pages_before + 4u,
                "munmap trim elide right-small marks protecting, free, and right remnant");
    expect_true(heap->core.munmap_vma_trim_repair_elided ==
                    elided_before + 1u,
                "munmap trim elide right-small counted");
    expect_true(heap->core.munmap_vma_trim_repair_elided_pages ==
                    elided_pages_before + 5u,
                "munmap trim elide right-small left pages counted");
    expect_ok(api->check(heap, 0),
              "munmap trim elide check after right-small middle");

    expect_ok(api->munmap(heap, (void *)base2, 5u * page_size),
              "munmap trim elide right-small cleanup low");
    expect_ok(api->munmap(heap, (void *)(base2 + 6u * page_size),
                          2u * page_size),
              "munmap trim elide right-small cleanup high");
    expect_ok(api->check(heap, 0), "munmap trim elide right-small final check");
    expect_ok(api->close(heap), "munmap trim elide close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

