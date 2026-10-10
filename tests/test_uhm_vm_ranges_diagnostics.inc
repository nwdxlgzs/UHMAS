static void test_mmap_page_rounding_and_owner_isolation(
    const uhm_storage_info *heap_info) {
    const size_t page_size = 8192u;
    const size_t rounded_two_pages = page_size * 2u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *small;
    void *fixed;
    uintptr_t map_addr;
    uintptr_t malloc_page;
    size_t first;
    size_t count;
    size_t i;
    uhm_stats stats;
    uhm_mmap_options options;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "mmap rounding heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "mmap rounding arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mmap rounding vm init");

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

    expect_ok(uhm_open(&config, &heap), "mmap rounding heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mmap rounding api");

    small = api->malloc(heap, 64u);
    expect_true(small != 0, "mmap rounding malloc allocation");
    malloc_page = (uintptr_t)small & ~((uintptr_t)page_size - 1u);
    fixed = (void *)malloc_page;
    expect_true(api->mmap(heap, &fixed, 1u,
                          UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                    UHM_ERR_ALREADY_MAPPED,
                "mmap cannot claim malloc-owned physical page");

    expect_ok(api->mmap(heap, &map, page_size + 123u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap rounding auto");
    expect_true(map != 0 && ((uintptr_t)map % page_size) == 0,
                "mmap rounding page aligned address");
    map_addr = (uintptr_t)map;
    expect_true(test_has_exact_vma(heap, map_addr, rounded_two_pages,
                                   UHM_PROT_READ | UHM_PROT_WRITE),
                "mmap rounding vma is page multiple");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "mmap rounding stats");
    expect_true(stats.mmap_live_bytes == rounded_two_pages,
                "mmap rounding stats uses page multiple");
    expect_true(test_shadow_page_span(heap, map_addr, rounded_two_pages,
                                      &first, &count) &&
                    count == 2u,
                "mmap rounding page span");
    for (i = 0; i < count; ++i) {
        uhm_page_record *record = test_page_record_at(heap, first + i);
        expect_true(record != 0 &&
                        record->owner == UHM_PAGE_OWNER_MMAP &&
                        record->va_state == UHM_PAGE_VA_MMAP_LIVE &&
                        record->commit_state == UHM_PAGE_COMMIT_LIVE,
                    "mmap rounded page owner is mmap only");
    }

    expect_ok(api->munmap(heap, map, page_size + 123u),
              "munmap rounds original request size");
    expect_true(api->munmap(heap, (void *)(map_addr + page_size), 1u) ==
                    UHM_ERR_NOT_MAPPED,
                "munmap rounded size released full second page");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "mmap rounding stats after munmap");
    expect_true(stats.mmap_live_bytes == 0,
                "mmap rounding munmap clears rounded live bytes");

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.abi_minor = UHM_ABI_MINOR;
    options.addr = map;
    options.map_size = page_size + 1u;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE;
    expect_ok(api->mmap_ex(heap, &options),
              "mmap_ex rounds and reports size");
    expect_true(options.addr == map &&
                    options.map_size == rounded_two_pages,
                "mmap_ex reports canonical rounded map size");
    expect_ok(api->munmap(heap, options.addr, page_size + 1u),
              "munmap accepts unrounded mmap_ex request size");

    api->free(heap, small);
    expect_ok(api->check(heap, 0), "mmap rounding final check");
    expect_ok(api->close(heap), "mmap rounding close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_page_range_budget_diagnostics(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count =
        UHM_PAGE_LEAF_PAGES * (UHM_PAGE_RANGE_DIAG_SHARD_BUDGET + 1u) + 32u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = map_size + 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t mark_budget_before;
    uint64_t query_budget_before;
    uint64_t lock_budget_before;
    uhm_stats stats_after;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "page range budget heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "page range budget arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "page range budget vm init");

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

    expect_ok(uhm_open(&config, &heap), "page range budget heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "page range budget api");

    mark_budget_before =
        test_atomic_u64_load(&heap->core.page_mark_budget_exceeded);
    query_budget_before =
        test_atomic_u64_load(&heap->core.page_range_query_budget_exceeded);
    lock_budget_before =
        test_atomic_u64_load(&heap->core.page_lock_range_budget_exceeded);
    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "page range budget mmap");
    expect_true(map != 0, "page range budget mmap address");
    expect_ok(api->mprotect(heap, map, map_size, UHM_PROT_READ),
              "page range budget mprotect");
    expect_true(test_atomic_u64_load(&heap->core.page_mark_max_pages) >=
                    page_count,
                "page range budget mark max pages counted");
    expect_true(test_atomic_u64_load(&heap->core.page_mark_budget_exceeded) >
                    mark_budget_before,
                "page range budget mark budget counted");
    expect_true(test_atomic_u64_load(&heap->core.page_range_query_max_pages) >=
                    page_count,
                "page range budget query max pages counted");
    expect_true(test_atomic_u64_load(
                    &heap->core.page_range_query_budget_exceeded) >
                    query_budget_before,
                "page range budget query budget counted");
    expect_true(test_atomic_u64_load(&heap->core.page_lock_range_max_shards) >
                    UHM_PAGE_RANGE_DIAG_SHARD_BUDGET,
                "page range budget lock max shards counted");
    expect_true(test_atomic_u64_load(
                    &heap->core.page_lock_range_budget_exceeded) >
                    lock_budget_before,
                "page range budget lock shard budget counted");
    memset(&stats_after, 0, sizeof(stats_after));
    stats_after.size = sizeof(stats_after);
    expect_ok(api->get_stats(heap, &stats_after),
              "page range budget public stats");
    expect_true(stats_after.page_mark_max_pages >= page_count,
                "page range budget public mark max pages counted");
    expect_true(stats_after.page_mark_budget_exceeded > mark_budget_before,
                "page range budget public mark budget counted");
    expect_true(stats_after.page_range_query_max_pages >= page_count,
                "page range budget public query max pages counted");
    expect_true(stats_after.page_range_query_budget_exceeded >
                    query_budget_before,
                "page range budget public query budget counted");
    expect_true(stats_after.page_mark_leaf_spans != 0u &&
                    stats_after.page_range_query_leaf_spans != 0u,
                "page range budget public leaf spans counted");
    expect_true(stats_after.page_lock_range_max_shards >
                    UHM_PAGE_RANGE_DIAG_SHARD_BUDGET,
                "page range budget public lock max shards counted");
    expect_true(stats_after.page_lock_range_budget_exceeded >
                    lock_budget_before,
                "page range budget public lock budget counted");

    expect_ok(api->munmap(heap, map, map_size), "page range budget munmap");
    expect_ok(api->check(heap, 0), "page range budget final check");
    expect_ok(api->close(heap), "page range budget close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_vma_pass_diagnostics(
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
    uintptr_t base;
    uhm_stats stats_before;
    uhm_stats stats_after;
    uint64_t collect_before;
    uint64_t noop_before;
    uint64_t mark_before;
    uint64_t publish_before;
    uint64_t max_segments_before;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_protect_lock_held_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mprotect VMA pass diagnostics heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mprotect VMA pass diagnostics arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mprotect VMA pass diagnostics vm init");

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

    expect_ok(uhm_open(&config, &heap), "mprotect VMA pass diagnostics open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mprotect VMA pass diagnostics api");

    memset(&stats_before, 0, sizeof(stats_before));
    stats_before.size = sizeof(stats_before);
    expect_ok(api->get_stats(heap, &stats_before),
              "mprotect VMA pass diagnostics stats before mmap");
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mprotect VMA pass diagnostics mmap");
    expect_true(map != 0, "mprotect VMA pass diagnostics address");
    base = (uintptr_t)map;
    memset(&stats_after, 0, sizeof(stats_after));
    stats_after.size = sizeof(stats_after);
    expect_ok(api->get_stats(heap, &stats_after),
              "mprotect VMA pass diagnostics stats after mmap");
    expect_true(stats_after.vm_commit_calls > stats_before.vm_commit_calls,
                "mprotect VMA pass diagnostics public commit calls grow");
    expect_true(stats_after.vm_commit_calls == heap->core.vm_commit_calls,
                "mprotect VMA pass diagnostics public commit matches core");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before,
                "mprotect VMA pass diagnostics mmap commit unlocked");
    stats_before = stats_after;

    expect_ok(api->mprotect(heap, (void *)base, page_size, UHM_PROT_READ),
              "mprotect VMA pass diagnostics split page 0");
    expect_ok(api->mprotect(heap, (void *)(base + page_size), page_size,
                            UHM_PROT_NONE),
              "mprotect VMA pass diagnostics split page 1");
    expect_ok(api->mprotect(heap, (void *)(base + 2u * page_size), page_size,
                            UHM_PROT_READ),
              "mprotect VMA pass diagnostics split page 2");
    expect_ok(api->mprotect(heap, (void *)(base + 3u * page_size), page_size,
                            UHM_PROT_NONE),
              "mprotect VMA pass diagnostics split page 3");

    collect_before = heap->core.mprotect_vma_collect_queries;
    noop_before = heap->core.mprotect_vma_noop_queries;
    mark_before = heap->core.mprotect_vma_mark_queries;
    publish_before = heap->core.mprotect_vma_publish_queries;
    max_segments_before = heap->core.mprotect_vma_max_segments;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_protect_lock_held_before = heap->core.vm_protect_lock_held_calls;

    expect_ok(api->mprotect(heap, map, map_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "mprotect VMA pass diagnostics multi mprotect");
    expect_true(heap->core.mprotect_vma_collect_queries ==
                    collect_before + 1u,
                "mprotect VMA pass diagnostics collect uses one tree query");
    expect_true(heap->core.mprotect_vma_noop_queries == noop_before,
                "mprotect VMA pass diagnostics noop reuses collected cursor");
    expect_true(heap->core.mprotect_vma_mark_queries == mark_before,
                "mprotect VMA pass diagnostics mark reuses collected cursor");
    expect_true(heap->core.mprotect_vma_publish_queries == publish_before,
                "mprotect VMA pass diagnostics publish reuses collected cursor");
    expect_true(heap->core.mprotect_vma_max_segments >= page_count &&
                    heap->core.mprotect_vma_max_segments >=
                        max_segments_before,
                "mprotect VMA pass diagnostics segment count preserved");
    memset(&stats_after, 0, sizeof(stats_after));
    stats_after.size = sizeof(stats_after);
    expect_ok(api->get_stats(heap, &stats_after),
              "mprotect VMA pass diagnostics stats after mprotect");
    expect_true(stats_after.vm_protect_calls >
                    stats_before.vm_protect_calls,
                "mprotect VMA pass diagnostics public protect calls grow");
    expect_true(stats_after.vm_protect_calls == heap->core.vm_protect_calls,
                "mprotect VMA pass diagnostics public protect matches core");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_protect_lock_held_calls ==
                        vm_protect_lock_held_before,
                "mprotect VMA pass diagnostics protect unlocked");
    stats_before = stats_after;

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    expect_ok(api->munmap(heap, map, map_size),
              "mprotect VMA pass diagnostics munmap");
    memset(&stats_after, 0, sizeof(stats_after));
    stats_after.size = sizeof(stats_after);
    expect_ok(api->get_stats(heap, &stats_after),
              "mprotect VMA pass diagnostics stats after munmap");
    expect_true(stats_after.vm_decommit_calls >
                    stats_before.vm_decommit_calls,
                "mprotect VMA pass diagnostics public decommit calls grow");
    expect_true(stats_after.vm_decommit_calls ==
                    heap->core.vm_decommit_calls,
                "mprotect VMA pass diagnostics public decommit matches core");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "mprotect VMA pass diagnostics exact decommit unlocked");
    expect_ok(api->check(heap, 0), "mprotect VMA pass diagnostics final check");
    expect_ok(api->close(heap), "mprotect VMA pass diagnostics close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mmap_commit_numa_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t map_size = 2u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_mmap_options options;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_numa_hint_lock_held_before;
    uint64_t vm_commit_calls_before;
    uint64_t vm_numa_hint_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mmap commit unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mmap commit unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mmap commit unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.numa_hint = reentrant_check_vm_numa_hint;
    vm.capabilities |= UHM_VM_CAN_NUMA_PREFERRED;

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

    expect_ok(uhm_open(&config, &heap), "mmap commit unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mmap commit unlock api");
    state.heap = heap;
    state.api = api;
    state.check_result = UHM_ERR_CORRUPTION;
    state.commit_count = 0;
    state.numa_hint_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.abi_minor = UHM_ABI_MINOR;
    options.map_size = map_size;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = 2;

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_numa_hint_lock_held_before = heap->core.vm_numa_hint_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_numa_hint_calls_before = heap->core.vm_numa_hint_calls;
    state.run_check = 1;
    expect_ok(api->mmap_ex(heap, &options), "mmap commit unlock mmap_ex");
    state.run_check = 0;
    expect_true(options.addr != 0, "mmap commit unlock address");
    expect_true(state.commit_count == 1,
                "mmap commit unlock commit callback called");
    expect_true(state.numa_hint_count == 1,
                "mmap commit unlock numa callback called");
    expect_true(state.check_calls == 2 && state.check_failures == 0,
                "mmap commit unlock reentrant checks ok");
    expect_ok(state.check_result, "mmap commit unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "mmap commit unlock commit call counted");
    expect_true(heap->core.vm_numa_hint_calls ==
                    vm_numa_hint_calls_before + 1u,
                "mmap commit unlock numa call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before &&
                    heap->core.vm_numa_hint_lock_held_calls ==
                        vm_numa_hint_lock_held_before,
                "mmap commit unlock platform calls not counted lock-held");
    expect_ok(api->check(heap, 0), "mmap commit unlock check after");

    expect_ok(api->munmap(heap, options.addr, options.map_size),
              "mmap commit unlock munmap");
    expect_ok(api->close(heap), "mmap commit unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mmap_fixed_exact_replace_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t map_size = 2u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_mmap_options options;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_numa_hint_lock_held_before;
    uint64_t vm_commit_calls_before;
    uint64_t vm_decommit_calls_before;
    uint64_t vm_numa_hint_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "fixed exact unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "fixed exact unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "fixed exact unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;
    vm.numa_hint = reentrant_check_vm_numa_hint;
    vm.capabilities |= UHM_VM_CAN_NUMA_PREFERRED;

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

    expect_ok(uhm_open(&config, &heap), "fixed exact unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mmap_ex != 0, "fixed exact unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "fixed exact unlock initial mmap");
    expect_true(map != 0, "fixed exact unlock initial address");
    memset(map, 0x55, map_size);

    state.check_result = UHM_ERR_CORRUPTION;
    state.commit_count = 0;
    state.decommit_count = 0;
    state.numa_hint_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_numa_hint_lock_held_before = heap->core.vm_numa_hint_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_numa_hint_calls_before = heap->core.vm_numa_hint_calls;

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.abi_minor = UHM_ABI_MINOR;
    options.addr = map;
    options.map_size = map_size;
    options.prot = UHM_PROT_READ;
    options.map_flags = UHM_MAP_PRIVATE | UHM_MAP_FIXED;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = 4;

    state.run_check = 1;
    expect_ok(api->mmap_ex(heap, &options),
              "fixed exact unlock replacement mmap_ex");
    state.run_check = 0;
    expect_true(options.addr == map, "fixed exact unlock replacement address");
    expect_true(options.map_size == map_size,
                "fixed exact unlock replacement size");
    expect_true(state.decommit_count == 1,
                "fixed exact unlock decommit callback called");
    expect_true(state.commit_count == 1,
                "fixed exact unlock commit callback called");
    expect_true(state.numa_hint_count == 1,
                "fixed exact unlock numa callback called");
    expect_true(state.check_calls == 3 && state.check_failures == 0,
                "fixed exact unlock reentrant checks ok");
    expect_ok(state.check_result, "fixed exact unlock last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "fixed exact unlock decommit call counted");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "fixed exact unlock commit call counted");
    expect_true(heap->core.vm_numa_hint_calls ==
                    vm_numa_hint_calls_before + 1u,
                "fixed exact unlock numa call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before &&
                    heap->core.vm_numa_hint_lock_held_calls ==
                        vm_numa_hint_lock_held_before,
                "fixed exact unlock platform calls not counted lock-held");
    expect_ok(api->check(heap, 0), "fixed exact unlock check after");

    expect_ok(api->munmap(heap, map, map_size), "fixed exact unlock cleanup");
    expect_ok(api->close(heap), "fixed exact unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mmap_fixed_partial_replace_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t map_size = 3u * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_mmap_options options;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *middle = 0;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_numa_hint_lock_held_before;
    uint64_t vm_commit_calls_before;
    uint64_t vm_decommit_calls_before;
    uint64_t vm_numa_hint_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "fixed partial unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "fixed partial unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "fixed partial unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;
    vm.numa_hint = reentrant_check_vm_numa_hint;
    vm.capabilities |= UHM_VM_CAN_NUMA_PREFERRED;

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

    expect_ok(uhm_open(&config, &heap), "fixed partial unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mmap_ex != 0, "fixed partial unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "fixed partial unlock initial mmap");
    expect_true(map != 0, "fixed partial unlock initial address");
    memset(map, 0x5a, map_size);
    middle = (void *)((uintptr_t)map + page_size);

    state.check_result = UHM_ERR_CORRUPTION;
    state.commit_count = 0;
    state.decommit_count = 0;
    state.numa_hint_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_numa_hint_lock_held_before = heap->core.vm_numa_hint_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_numa_hint_calls_before = heap->core.vm_numa_hint_calls;

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.abi_minor = UHM_ABI_MINOR;
    options.addr = middle;
    options.map_size = page_size;
    options.prot = UHM_PROT_READ;
    options.map_flags = UHM_MAP_PRIVATE | UHM_MAP_FIXED;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = 2;

    state.run_check = 1;
    expect_ok(api->mmap_ex(heap, &options),
              "fixed partial unlock replacement mmap_ex");
    state.run_check = 0;
    expect_true(options.addr == middle,
                "fixed partial unlock replacement address");
    expect_true(options.map_size == page_size,
                "fixed partial unlock replacement size");
    expect_true(state.decommit_count == 1,
                "fixed partial unlock decommit callback called");
    expect_true(state.commit_count == 1,
                "fixed partial unlock commit callback called");
    expect_true(state.numa_hint_count == 1,
                "fixed partial unlock numa callback called");
    expect_true(state.check_calls == 3 && state.check_failures == 0,
                "fixed partial unlock reentrant checks ok");
    expect_ok(state.check_result, "fixed partial unlock last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "fixed partial unlock decommit call counted");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "fixed partial unlock commit call counted");
    expect_true(heap->core.vm_numa_hint_calls ==
                    vm_numa_hint_calls_before + 1u,
                "fixed partial unlock numa call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before &&
                    heap->core.vm_numa_hint_lock_held_calls ==
                        vm_numa_hint_lock_held_before,
                "fixed partial unlock platform calls not counted lock-held");
    expect_ok(api->check(heap, 0), "fixed partial unlock check after");

    expect_ok(api->munmap(heap, map, map_size),
              "fixed partial unlock cleanup");
    expect_ok(api->close(heap), "fixed partial unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}
