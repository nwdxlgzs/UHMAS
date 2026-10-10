static void test_mprotect_exact_vma_elides_live_rescan(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 4u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t query_calls_before;
    uint64_t query_pages_before;
    uint64_t vma_insertions_before;
    uint64_t vma_removes_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "exact mprotect query heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "exact mprotect query arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "exact mprotect query vm init");

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

    expect_ok(uhm_open(&config, &heap), "exact mprotect query open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "exact mprotect query api");

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "exact mprotect query mmap");
    expect_true(map != 0, "exact mprotect query address");

    query_calls_before =
        test_atomic_u64_load(&heap->core.page_range_query_calls);
    query_pages_before =
        test_atomic_u64_load(&heap->core.page_range_query_pages);
    vma_insertions_before = heap->core.vma_tree_insertions;
    vma_removes_before = heap->core.vma_tree_remove_calls;
    expect_ok(api->mprotect(heap, map, map_size, UHM_PROT_READ),
              "exact mprotect query protect");
    expect_true(test_atomic_u64_load(&heap->core.page_range_query_calls) ==
                    query_calls_before + 4u &&
                    test_atomic_u64_load(&heap->core.page_range_query_pages) ==
                        query_pages_before + 4u * page_count,
                "exact mprotect query skips redundant live-state rescan");
    expect_true(heap->core.vma_tree_insertions == vma_insertions_before &&
                    heap->core.vma_tree_remove_calls == vma_removes_before,
                "exact mprotect query updates VMA in place");
    expect_ok(api->check(heap, 0), "exact mprotect query check after");
    expect_ok(api->munmap(heap, map, map_size),
              "exact mprotect query munmap");
    expect_ok(api->close(heap), "exact mprotect query close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_page_private_elides_live_rescan(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t alloc_size = 2u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *page_private = 0;
    void *free_private = 0;
    void *realloc_private = 0;
    void *realloc_result = 0;
    uint64_t query_calls_before;
    uint64_t query_pages_before;
    uint64_t descriptor_hits_before;
    uint64_t scan_fallbacks_before;
    uint64_t fast_before;
    uint64_t slow_before;
    uint64_t has_state_before;
    uint64_t get_expected_before;
    uint64_t tree_find_before;
    uint64_t fallback_before;
    uint64_t lookup_free_before;
    uint64_t lookup_free_slow_before;
    uint64_t lookup_realloc_before;
    uint64_t lookup_realloc_slow_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "page-private mprotect query heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "page-private mprotect query arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "page-private mprotect query vm init");

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

    expect_ok(uhm_open(&config, &heap), "page-private mprotect query open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "page-private mprotect query api");

    page_private = api->aligned_alloc(heap, page_size, alloc_size);
    expect_true(page_private != 0, "page-private mprotect query alloc");
    expect_true(((uintptr_t)page_private % page_size) == 0,
                "page-private mprotect query page aligned");

    query_calls_before =
        test_atomic_u64_load(&heap->core.page_range_query_calls);
    query_pages_before =
        test_atomic_u64_load(&heap->core.page_range_query_pages);
    descriptor_hits_before =
        heap->core.mprotect_page_private_descriptor_hits;
    scan_fallbacks_before =
        heap->core.mprotect_page_private_scan_fallbacks;
    expect_ok(api->mprotect(heap, page_private, page_size, UHM_PROT_READ),
              "page-private mprotect query protect");
    expect_true(test_atomic_u64_load(&heap->core.page_range_query_calls) ==
                    query_calls_before + 5u &&
                    test_atomic_u64_load(&heap->core.page_range_query_pages) ==
                        query_pages_before + 5u,
                "page-private mprotect query skips redundant live-state rescan");
    expect_true(heap->core.mprotect_page_private_descriptor_hits ==
                    descriptor_hits_before + 1u &&
                    heap->core.mprotect_page_private_scan_fallbacks ==
                        scan_fallbacks_before,
                "page-private mprotect query uses descriptor lookup");

    query_calls_before =
        test_atomic_u64_load(&heap->core.page_range_query_calls);
    query_pages_before =
        test_atomic_u64_load(&heap->core.page_range_query_pages);
    descriptor_hits_before =
        heap->core.mprotect_page_private_descriptor_hits;
    scan_fallbacks_before =
        heap->core.mprotect_page_private_scan_fallbacks;
    expect_ok(api->mprotect(heap, page_private, page_size, UHM_PROT_READ),
              "page-private mprotect query no-op");
    expect_true(test_atomic_u64_load(&heap->core.page_range_query_calls) ==
                    query_calls_before + 3u &&
                    test_atomic_u64_load(&heap->core.page_range_query_pages) ==
                        query_pages_before + 3u,
                "page-private mprotect query no-op reuses expected state");
    expect_true(heap->core.mprotect_page_private_descriptor_hits ==
                    descriptor_hits_before + 1u &&
                    heap->core.mprotect_page_private_scan_fallbacks ==
                        scan_fallbacks_before,
                "page-private mprotect query no-op uses descriptor lookup");

    expect_ok(api->mprotect(heap, page_private, page_size, UHM_PROT_NONE),
              "page-private mprotect query protect none");
    fast_before = heap->core.live_block_fast_hits;
    slow_before = heap->core.live_block_slow_calls;
    has_state_before =
        test_atomic_u64_load(
            &heap->core.page_range_query_phase_calls
                 [UHM_PAGE_QUERY_PHASE_HAS_STATE]);
    get_expected_before =
        test_atomic_u64_load(
            &heap->core.page_range_query_phase_calls
                 [UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE]);
    tree_find_before = heap->core.live_block_tree_find_calls;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    expect_true(api->malloc_usable_size(heap, page_private) >= alloc_size,
                "page-private protected usable size");
    expect_true(heap->core.live_block_fast_hits == fast_before + 1u &&
                    heap->core.live_block_slow_calls == slow_before,
                "page-private protected lookup stays fast");
    expect_true(test_atomic_u64_load(
                    &heap->core.page_range_query_phase_calls
                         [UHM_PAGE_QUERY_PHASE_HAS_STATE]) ==
                    has_state_before,
                "page-private protected lookup skips header has-state");
    expect_true(test_atomic_u64_load(
                    &heap->core.page_range_query_phase_calls
                         [UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE]) ==
                    get_expected_before + 2u,
                "page-private protected lookup uses descriptor probes");
    expect_true(heap->core.live_block_tree_find_calls == tree_find_before &&
                    heap->core.live_block_slow_fallback_scans ==
                        fallback_before,
                "page-private protected lookup avoids tree and fallback");

    expect_ok(api->check(heap, 0), "page-private mprotect query check after");
    expect_ok(api->mprotect(heap, page_private, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "page-private mprotect query restore rw");
    api->free(heap, page_private);
    page_private = 0;

    free_private = api->aligned_alloc(heap, page_size, alloc_size);
    expect_true(free_private != 0, "page-private protected free alloc");
    expect_ok(api->mprotect(heap, free_private, page_size, UHM_PROT_NONE),
              "page-private protected free protect none");
    fast_before = heap->core.live_block_fast_hits;
    slow_before = heap->core.live_block_slow_calls;
    tree_find_before = heap->core.live_block_tree_find_calls;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    lookup_free_before =
        heap->core
            .live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE];
    lookup_free_slow_before =
        heap->core.live_block_lookup_slow_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE];
    api->free(heap, free_private);
    free_private = 0;
    expect_true(heap->core.live_block_lookup_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] ==
                    lookup_free_before + 1u,
                "page-private protected free lookup counted");
    expect_true(heap->core.live_block_fast_hits == fast_before + 1u &&
                    heap->core.live_block_slow_calls == slow_before,
                "page-private protected free lookup stays fast");
    expect_true(heap->core.live_block_lookup_slow_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] ==
                    lookup_free_slow_before,
                "page-private protected free avoids slow callsite");
    expect_true(heap->core.live_block_tree_find_calls == tree_find_before &&
                    heap->core.live_block_slow_fallback_scans ==
                        fallback_before,
                "page-private protected free avoids tree and fallback");

    realloc_private = api->aligned_alloc(heap, page_size, alloc_size);
    expect_true(realloc_private != 0, "page-private protected realloc alloc");
    expect_ok(api->mprotect(heap, realloc_private, page_size, UHM_PROT_NONE),
              "page-private protected realloc protect none");
    fast_before = heap->core.live_block_fast_hits;
    slow_before = heap->core.live_block_slow_calls;
    tree_find_before = heap->core.live_block_tree_find_calls;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    lookup_realloc_before =
        heap->core
            .live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC];
    lookup_realloc_slow_before =
        heap->core.live_block_lookup_slow_calls
            [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC];
    realloc_result = api->realloc(heap, realloc_private, page_size);
    expect_true(realloc_result == realloc_private,
                "page-private protected realloc stays in place");
    realloc_private = realloc_result;
    expect_true(heap->core.live_block_lookup_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC] ==
                    lookup_realloc_before + 1u,
                "page-private protected realloc lookup counted");
    expect_true(heap->core.live_block_fast_hits == fast_before + 1u &&
                    heap->core.live_block_slow_calls == slow_before,
                "page-private protected realloc lookup stays fast");
    expect_true(heap->core.live_block_lookup_slow_calls
                        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC] ==
                    lookup_realloc_slow_before,
                "page-private protected realloc avoids slow callsite");
    expect_true(heap->core.live_block_tree_find_calls == tree_find_before &&
                    heap->core.live_block_slow_fallback_scans ==
                        fallback_before,
                "page-private protected realloc avoids tree and fallback");
    api->free(heap, realloc_private);
    realloc_private = 0;

    expect_ok(api->check(heap, 0),
              "page-private protected free/realloc final check");
    expect_ok(api->close(heap), "page-private mprotect query close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_exact_vma_platform_unlock(
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
    uint64_t vm_protect_lock_held_before;
    uint64_t vm_protect_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "exact mprotect unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "exact mprotect unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "exact mprotect unlock vm init");
    vm.user = &state;
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

    expect_ok(uhm_open(&config, &heap), "exact mprotect unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "exact mprotect unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "exact mprotect unlock mmap");
    expect_true(map != 0, "exact mprotect unlock address");
    expect_ok(api->check(heap, 0), "exact mprotect unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_protect_lock_held_before = heap->core.vm_protect_lock_held_calls;
    vm_protect_calls_before = heap->core.vm_protect_calls;
    state.run_check = 1;
    expect_ok(api->mprotect(heap, map, map_size, UHM_PROT_READ),
              "exact mprotect unlock protect");
    state.run_check = 0;
    expect_true(state.protect_count == 1,
                "exact mprotect unlock protect callback called");
    expect_true(state.check_calls == 1,
                "exact mprotect unlock reentrant check called");
    expect_ok(state.check_result, "exact mprotect unlock reentrant check ok");
    expect_true(heap->core.vm_protect_calls == vm_protect_calls_before + 1u,
                "exact mprotect unlock protect call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_protect_lock_held_calls ==
                        vm_protect_lock_held_before,
                "exact mprotect unlock protect not counted lock-held");
    expect_ok(api->check(heap, 0), "exact mprotect unlock check after");

    expect_ok(api->mprotect(heap, map, map_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "exact mprotect unlock restore rw");
    expect_ok(api->munmap(heap, map, map_size), "exact mprotect unlock munmap");
    expect_ok(api->close(heap), "exact mprotect unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_partial_vma_platform_unlock(
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
    uint64_t vm_protect_lock_held_before;
    uint64_t vm_protect_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "partial mprotect unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "partial mprotect unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "partial mprotect unlock vm init");
    vm.user = &state;
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

    expect_ok(uhm_open(&config, &heap), "partial mprotect unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "partial mprotect unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "partial mprotect unlock mmap");
    expect_true(map != 0, "partial mprotect unlock address");
    memset(map, 0x39, map_size);
    expect_ok(api->check(heap, 0), "partial mprotect unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_protect_lock_held_before = heap->core.vm_protect_lock_held_calls;
    vm_protect_calls_before = heap->core.vm_protect_calls;
    state.run_check = 1;
    expect_ok(api->mprotect(heap, (unsigned char *)map + page_size,
                            2u * page_size, UHM_PROT_READ),
              "partial mprotect unlock protect");
    state.run_check = 0;
    expect_true(state.protect_count == 1,
                "partial mprotect unlock protect callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "partial mprotect unlock reentrant check ok");
    expect_ok(state.check_result, "partial mprotect unlock last check ok");
    expect_true(heap->core.vm_protect_calls == vm_protect_calls_before + 1u,
                "partial mprotect unlock protect call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_protect_lock_held_calls ==
                        vm_protect_lock_held_before,
                "partial mprotect unlock protect not counted lock-held");
    expect_ok(api->check(heap, 0), "partial mprotect unlock check after");

    expect_ok(api->munmap(heap, map, map_size),
              "partial mprotect unlock cleanup munmap");
    expect_ok(api->close(heap), "partial mprotect unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_page_private_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t alloc_size = 2u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *page_private = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_protect_lock_held_before;
    uint64_t vm_protect_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "page-private mprotect unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "page-private mprotect unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "page-private mprotect unlock vm init");
    vm.user = &state;
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

    expect_ok(uhm_open(&config, &heap), "page-private mprotect unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "page-private mprotect unlock api");
    state.heap = heap;
    state.api = api;

    page_private = api->aligned_alloc(heap, page_size, alloc_size);
    expect_true(page_private != 0, "page-private mprotect unlock alloc");
    expect_true(((uintptr_t)page_private % page_size) == 0,
                "page-private mprotect unlock page aligned");
    memset(page_private, 0x4du, alloc_size);
    expect_ok(api->check(heap, 0), "page-private mprotect unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_protect_lock_held_before = heap->core.vm_protect_lock_held_calls;
    vm_protect_calls_before = heap->core.vm_protect_calls;
    state.run_check = 1;
    expect_ok(api->mprotect(heap, page_private, page_size, UHM_PROT_READ),
              "page-private mprotect unlock protect");
    state.run_check = 0;
    expect_true(state.protect_count == 1,
                "page-private mprotect unlock protect callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "page-private mprotect unlock reentrant check ok");
    expect_ok(state.check_result,
              "page-private mprotect unlock last check ok");
    expect_true(heap->core.vm_protect_calls == vm_protect_calls_before + 1u,
                "page-private mprotect unlock protect call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_protect_lock_held_calls ==
                        vm_protect_lock_held_before,
                "page-private mprotect unlock protect not counted lock-held");
    expect_ok(api->check(heap, 0), "page-private mprotect unlock check after");

    expect_ok(api->mprotect(heap, page_private, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "page-private mprotect unlock restore rw");
    api->free(heap, page_private);
    expect_ok(api->close(heap), "page-private mprotect unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_page_private_restore_rw_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t alloc_size = 2u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *page_private = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_protect_lock_held_before;
    uint64_t vm_protect_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "page-private restore unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "page-private restore unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "page-private restore unlock vm init");
    vm.user = &state;
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

    expect_ok(uhm_open(&config, &heap), "page-private restore unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "page-private restore unlock api");
    state.heap = heap;
    state.api = api;

    page_private = api->aligned_alloc(heap, page_size, alloc_size);
    expect_true(page_private != 0, "page-private restore unlock alloc");
    expect_true(((uintptr_t)page_private % page_size) == 0,
                "page-private restore unlock page aligned");
    memset(page_private, 0x73u, alloc_size);
    expect_ok(api->mprotect(heap, page_private, page_size, UHM_PROT_READ),
              "page-private restore unlock make mixed prot");
    expect_ok(api->check(heap, 0), "page-private restore unlock check before");

    state.protect_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    state.check_result = UHM_ERR_CORRUPTION;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_protect_lock_held_before = heap->core.vm_protect_lock_held_calls;
    vm_protect_calls_before = heap->core.vm_protect_calls;
    state.run_check = 1;
    api->free(heap, page_private);
    state.run_check = 0;

    expect_true(state.protect_count == 1,
                "page-private restore unlock protect callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "page-private restore unlock reentrant check ok");
    expect_ok(state.check_result,
              "page-private restore unlock last check ok");
    expect_true(heap->core.vm_protect_calls == vm_protect_calls_before + 1u,
                "page-private restore unlock protect call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_protect_lock_held_calls ==
                        vm_protect_lock_held_before,
                "page-private restore unlock protect not counted lock-held");
    expect_ok(api->check(heap, 0), "page-private restore unlock check after");

    expect_ok(api->close(heap), "page-private restore unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_cross_vma_partial_platform_unlock(
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
    uintptr_t base;
    uint64_t vm_lock_held_before;
    uint64_t vm_protect_lock_held_before;
    uint64_t vm_protect_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "cross mprotect unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "cross mprotect unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "cross mprotect unlock vm init");
    vm.user = &state;
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

    expect_ok(uhm_open(&config, &heap), "cross mprotect unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "cross mprotect unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "cross mprotect unlock mmap");
    expect_true(map != 0, "cross mprotect unlock address");
    base = (uintptr_t)map;
    memset(map, 0x71, map_size);
    expect_ok(api->mprotect(heap, (void *)(base + 2u * page_size),
                            page_size, UHM_PROT_READ),
              "cross mprotect unlock split middle page");
    expect_ok(api->check(heap, 0), "cross mprotect unlock check before");

    state.check_result = UHM_ERR_CORRUPTION;
    state.protect_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_protect_lock_held_before = heap->core.vm_protect_lock_held_calls;
    vm_protect_calls_before = heap->core.vm_protect_calls;
    state.run_check = 1;
    expect_ok(api->mprotect(heap, (void *)(base + page_size),
                            3u * page_size, UHM_PROT_NONE),
              "cross mprotect unlock partial protect");
    state.run_check = 0;
    expect_true(state.protect_count == 1,
                "cross mprotect unlock protect callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "cross mprotect unlock reentrant check ok");
    expect_ok(state.check_result, "cross mprotect unlock last check ok");
    expect_true(heap->core.vm_protect_calls == vm_protect_calls_before + 1u,
                "cross mprotect unlock protect call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_protect_lock_held_calls ==
                        vm_protect_lock_held_before,
                "cross mprotect unlock protect not counted lock-held");
    expect_ok(api->check(heap, 0), "cross mprotect unlock check after");

    expect_ok(api->munmap(heap, map, map_size),
              "cross mprotect unlock cleanup munmap");
    expect_ok(api->close(heap), "cross mprotect unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

typedef struct mprotect_no_commit_platform_state {
    unsigned protect_count;
    void *last_addr;
    size_t last_size;
    uint32_t last_prot;
} mprotect_no_commit_platform_state;

static uhm_result UHM_CALL mprotect_no_commit_vm_protect(
    void *user, void *addr, size_t size, uint32_t prot) {
    mprotect_no_commit_platform_state *state =
        (mprotect_no_commit_platform_state *)user;
    state->protect_count++;
    state->last_addr = addr;
    state->last_size = size;
    state->last_prot = prot;
    return UHM_OK;
}

static void test_mprotect_no_commit_skips_platform_protect(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    mprotect_no_commit_platform_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *no_commit_map = 0;
    void *high_map = 0;
    void *low_map = 0;
    uintptr_t high_addr;
    uintptr_t low_addr;
    size_t first_page;
    size_t page_count;
    uhm_page_record *low_record;
    uhm_page_record *high_record;
    uint64_t platform_before;
    uint64_t vm_protect_before;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "no-commit mprotect heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "no-commit mprotect arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "no-commit mprotect vm init");
    vm.user = &state;
    vm.protect = mprotect_no_commit_vm_protect;

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

    expect_ok(uhm_open(&config, &heap), "no-commit mprotect open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "no-commit mprotect api");

    expect_ok(api->mmap(heap, &no_commit_map, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "no-commit mprotect mmap");
    platform_before = heap->core.mprotect_platform_calls;
    vm_protect_before = heap->core.vm_protect_calls;
    expect_ok(api->mprotect(heap, no_commit_map, page_size * 2u,
                            UHM_PROT_READ),
              "no-commit mprotect exact protect");
    expect_true(state.protect_count == 0u,
                "no-commit mprotect exact skips protect callback");
    expect_true(heap->core.mprotect_platform_calls == platform_before &&
                    heap->core.vm_protect_calls == vm_protect_before,
                "no-commit mprotect exact skips platform counters");
    expect_true(test_shadow_page_span(heap, (uintptr_t)no_commit_map,
                                      page_size * 2u, &first_page,
                                      &page_count) != 0 &&
                    page_count == 2u,
                "no-commit mprotect exact page span");
    low_record = test_page_record_at(heap, first_page);
    high_record = test_page_record_at(heap, first_page + 1u);
    expect_true(low_record && high_record &&
                    low_record->commit_state == UHM_PAGE_COMMIT_DECOMMITTED &&
                    high_record->commit_state == UHM_PAGE_COMMIT_DECOMMITTED &&
                    low_record->prot == UHM_PROT_READ &&
                    high_record->prot == UHM_PROT_READ,
                "no-commit mprotect exact updates page records");
    expect_ok(api->check(heap, 0), "no-commit mprotect exact check");
    expect_ok(api->munmap(heap, no_commit_map, page_size * 2u),
              "no-commit mprotect exact cleanup");

    expect_ok(api->mmap(heap, &high_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mixed no-commit mprotect high mmap");
    high_addr = (uintptr_t)high_map;
    expect_true(high_addr >= heap->core.arena_base + page_size,
                "mixed no-commit mprotect has lower page");
    low_addr = high_addr - page_size;
    low_map = (void *)low_addr;
    expect_ok(api->mmap(heap, &low_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT |
                            UHM_MAP_FIXED_NOREPLACE),
              "mixed no-commit mprotect low fixed mmap");
    expect_true((uintptr_t)low_map == low_addr,
                "mixed no-commit mprotect low fixed address");

    memset(&state, 0, sizeof(state));
    platform_before = heap->core.mprotect_platform_calls;
    vm_protect_before = heap->core.vm_protect_calls;
    expect_ok(api->mprotect(heap, low_map, page_size * 2u, UHM_PROT_READ),
              "mixed no-commit mprotect protect");
    expect_true(state.protect_count == 1u,
                "mixed no-commit mprotect protects committed island only");
    expect_true(state.last_addr == high_map && state.last_size == page_size &&
                    state.last_prot == UHM_PROT_READ,
                "mixed no-commit mprotect protect range skips no-commit page");
    expect_true(heap->core.mprotect_platform_calls == platform_before + 1u &&
                    heap->core.vm_protect_calls == vm_protect_before + 1u,
                "mixed no-commit mprotect counts one platform run");
    expect_true(test_shadow_page_span(heap, low_addr, page_size * 2u,
                                      &first_page, &page_count) != 0 &&
                    page_count == 2u,
                "mixed no-commit mprotect page span");
    low_record = test_page_record_at(heap, first_page);
    high_record = test_page_record_at(heap, first_page + 1u);
    expect_true(low_record && high_record &&
                    low_record->commit_state == UHM_PAGE_COMMIT_DECOMMITTED &&
                    high_record->commit_state == UHM_PAGE_COMMIT_LIVE &&
                    low_record->prot == UHM_PROT_READ &&
                    high_record->prot == UHM_PROT_READ,
                "mixed no-commit mprotect updates both page records");
    expect_ok(api->check(heap, 0), "mixed no-commit mprotect check");

    expect_ok(api->munmap(heap, low_map, page_size * 2u),
              "mixed no-commit mprotect cleanup munmap");
    expect_ok(api->close(heap), "no-commit mprotect close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

