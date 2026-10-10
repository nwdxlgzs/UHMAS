static void test_large_malloc_small_extent_decommit_deferred(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    const size_t small_large_size = 4u * page_size;
    void *ptr;
    void *reuse;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    uint64_t deferred_events_before;
    uint64_t deferred_bytes_before;
    uhm_stats stats_before;
    uhm_stats stats_after_free;
    int commit_before_reuse;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "small large extent heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(2u * 1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "small large extent arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "small large extent usermode vm init");
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

    expect_ok(uhm_open(&config, &heap), "small large extent heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "small large extent api");

    ptr = api->malloc(heap, small_large_size);
    expect_true(ptr != 0, "small large extent malloc");
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "small large extent block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size),
                "small large extent has decommittable interior pages");

    memset(&stats_before, 0, sizeof(stats_before));
    stats_before.size = sizeof(stats_before);
    expect_ok(api->get_stats(heap, &stats_before),
              "small large extent stats before free");
    deferred_events_before =
        heap->core.malloc_free_decommit_deferred_events;
    deferred_bytes_before =
        heap->core.malloc_free_decommit_deferred_bytes;
    api->free(heap, ptr);
    expect_true((block->flags & UHM_BLOCK_FLAG_FREE) != 0,
                "small large extent free flag");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0,
                "small large extent stays committed");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0,
                "small large extent retained flag");
    expect_true(heap->core.malloc_free_decommit_retained_bytes >=
                    decommit_size,
                "small large extent retained bytes counted");
    expect_true(state.decommit_count == 0,
                "small large extent decommit callback deferred");
    expect_true(heap->core.malloc_free_decommit_deferred_events >
                    deferred_events_before,
                "small large extent deferred event counted");
    expect_true(heap->core.malloc_free_decommit_deferred_bytes >=
                    deferred_bytes_before + decommit_size,
                "small large extent deferred bytes counted");
    memset(&stats_after_free, 0, sizeof(stats_after_free));
    stats_after_free.size = sizeof(stats_after_free);
    expect_ok(api->get_stats(heap, &stats_after_free),
              "small large extent stats after free");
    expect_true(stats_after_free.malloc_free_decommit_deferred_events >
                    stats_before.malloc_free_decommit_deferred_events,
                "small large extent public deferred event counted");
    expect_true(stats_after_free.malloc_free_decommit_deferred_bytes >=
                    stats_before.malloc_free_decommit_deferred_bytes +
                        decommit_size,
                "small large extent public deferred bytes counted");
    expect_true(stats_after_free.decommit_events == stats_before.decommit_events,
                "small large extent public decommit event deferred");

    commit_before_reuse = state.commit_count;
    reuse = api->malloc(heap, small_large_size);
    expect_true(reuse == ptr, "small large extent reuses same block");
    expect_true(state.commit_count == commit_before_reuse,
                "small large extent reuse avoids recommit callback");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) == 0,
                "small large extent reuse clears retained flag");
    expect_true(heap->core.malloc_free_decommit_retained_bytes == 0u,
                "small large extent reuse releases retained bytes");
    expect_ok(api->check(heap, 0), "small large extent check after reuse");
    test_shadow_model_check(heap, "small large extent shadow after reuse");

    api->free(heap, reuse);
    expect_ok(api->check(heap, 0), "small large extent final check");
    expect_ok(api->close(heap), "small large extent close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_retention_high_water_pressure(
    const uhm_storage_info *heap_info) {
    enum { block_count = 80 };
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    const size_t small_large_size = 4u * page_size;
    void *ptrs[block_count];
    size_t i;
    size_t retained_after_first = 0;
    uint64_t decommit_before;
    uint64_t pressure_before;
    uint64_t pressure_bytes_before;
    int saw_retained = 0;
    int saw_decommitted = 0;

    memset(ptrs, 0, sizeof(ptrs));
    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "retention high water heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(8u * 1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "retention high water arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "retention high water usermode vm init");
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
    config.arena_size = 8u * 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "retention high water heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "retention high water api");
    expect_true(heap->core.malloc_free_decommit_retention_high_water_bytes >
                    page_size,
                "retention high water configured");

    for (i = 0; i < block_count; ++i) {
        ptrs[i] = api->malloc(heap, small_large_size);
        expect_true(ptrs[i] != 0, "retention high water malloc");
    }

    decommit_before = heap->core.decommit_events;
    pressure_before =
        heap->core.malloc_free_decommit_retention_pressure_events;
    pressure_bytes_before =
        heap->core.malloc_free_decommit_retention_pressure_bytes;

    for (i = 0; i < block_count; i += 2u) {
        uhm_block *block = test_find_block_by_payload(heap, ptrs[i]);
        expect_true(block != 0, "retention high water block lookup");
        api->free(heap, ptrs[i]);
        if ((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0) {
            saw_retained = 1;
        }
        if ((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0) {
            saw_decommitted = 1;
        }
        ptrs[i] = 0;
    }

    retained_after_first =
        heap->core.malloc_free_decommit_retained_bytes;
    expect_true(saw_retained != 0,
                "retention high water retained initial small extents");
    expect_true(saw_decommitted != 0,
                "retention high water decommits under pressure");
    expect_true(retained_after_first <=
                    heap->core
                        .malloc_free_decommit_retention_high_water_bytes,
                "retention high water caps retained bytes");
    expect_true(heap->core.decommit_events > decommit_before,
                "retention high water decommit events counted");
    expect_true(heap->core.malloc_free_decommit_retention_pressure_events >
                    pressure_before,
                "retention high water pressure events counted");
    expect_true(heap->core.malloc_free_decommit_retention_pressure_bytes >
                    pressure_bytes_before,
                "retention high water pressure bytes counted");
    expect_ok(api->check(heap, 0), "retention high water check after pressure");

    for (i = 1; i < block_count; i += 2u) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_ok(api->check(heap, 0), "retention high water final check");
    expect_ok(api->close(heap), "retention high water close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_adaptive_hot_retention_high_water(
    const uhm_storage_info *heap_info) {
    enum { target_count = 8 };
    const size_t page_size = uhm_platform_system_page_size();
    const size_t target_size = 3u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *targets[target_count];
    void *guards[target_count];
    size_t i;
    size_t shrink_cycle;
    size_t initial_high_water;
    size_t grown_high_water;
    size_t max_high_water;
    size_t expected_shrunk_high_water;
    uint64_t grow_events_before;
    uint64_t shrink_events_before;
    uint64_t hot_deferred_before;
    int decommit_before_hot_release;
    int commit_before_retained_reuse;

    memset(targets, 0, sizeof(targets));
    memset(guards, 0, sizeof(guards));
    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "adaptive hot retention heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(8u * 1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "adaptive hot retention arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "adaptive hot retention usermode vm init");
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
    config.arena_size = 8u * 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "adaptive hot retention heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "adaptive hot retention api");
    initial_high_water =
        heap->core.malloc_free_decommit_retention_high_water_bytes;
    max_high_water =
        heap->core.malloc_free_decommit_retention_max_high_water_bytes;
    expect_true(initial_high_water ==
                    heap->core
                        .malloc_free_decommit_retention_base_high_water_bytes,
                "adaptive hot retention starts at base high water");
    expect_true(max_high_water > initial_high_water,
                "adaptive hot retention has grow room");

    for (i = 0; i < target_count; ++i) {
        targets[i] = api->malloc(heap, target_size);
        guards[i] = api->malloc(heap, target_size);
        expect_true(targets[i] != 0 && guards[i] != 0,
                    "adaptive hot retention malloc pair");
    }

    for (i = 0; i < target_count; ++i) {
        uhm_block *block = test_find_block_by_payload(heap, targets[i]);
        void *reuse;

        expect_true(block != 0, "adaptive hot retention block lookup");
        api->free(heap, targets[i]);
        expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                    "adaptive hot retention first free decommits");
        reuse = api->malloc(heap, target_size);
        expect_true(reuse == targets[i],
                    "adaptive hot retention same address reuse");
        targets[i] = reuse;
        expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0,
                    "adaptive hot retention hot hint set");
    }
    expect_ok(api->check(heap, 0),
              "adaptive hot retention check after warming");

    grow_events_before =
        heap->core.malloc_free_decommit_retention_adaptive_grow_events;
    shrink_events_before =
        heap->core.malloc_free_decommit_retention_adaptive_shrink_events;
    hot_deferred_before =
        heap->core.malloc_free_decommit_hot_deferred_events;
    decommit_before_hot_release = state.decommit_count;

    for (i = 0; i < target_count; ++i) {
        uhm_block *block = test_find_block_by_payload(heap, targets[i]);

        expect_true(block != 0,
                    "adaptive hot retention hot block lookup");
        api->free(heap, targets[i]);
        expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0,
                    "adaptive hot retention retained hot block");
        targets[i] = 0;
    }

    expect_true(state.decommit_count == decommit_before_hot_release,
                "adaptive hot retention avoids hot decommit burst");
    expect_true(heap->core
                        .malloc_free_decommit_retention_adaptive_grow_events >
                    grow_events_before,
                "adaptive hot retention grow counted");
    expect_true(heap->core.malloc_free_decommit_retention_high_water_bytes >
                    initial_high_water,
                "adaptive hot retention high water grew");
    expect_true(heap->core.malloc_free_decommit_retention_high_water_bytes <=
                    max_high_water,
                "adaptive hot retention high water capped");
    grown_high_water =
        heap->core.malloc_free_decommit_retention_high_water_bytes;
    expect_true(heap->core.malloc_free_decommit_retained_bytes <=
                    heap->core
                        .malloc_free_decommit_retention_high_water_bytes,
                "adaptive hot retention retained bytes capped");
    expect_true(heap->core.malloc_free_decommit_hot_deferred_events >=
                    hot_deferred_before + target_count,
                "adaptive hot retention hot deferred counted");
    expect_ok(api->check(heap, 0),
              "adaptive hot retention check after hot frees");

    commit_before_retained_reuse = state.commit_count;
    for (i = 0; i < target_count; ++i) {
        targets[i] = api->malloc(heap, target_size);
        expect_true(targets[i] != 0,
                    "adaptive hot retention retained reuse malloc");
    }
    expect_true(state.commit_count == commit_before_retained_reuse,
                "adaptive hot retention retained reuse avoids recommit");
    expect_true(heap->core.malloc_free_decommit_retained_bytes == 0u,
                "adaptive hot retention retained bytes drained");
    expected_shrunk_high_water = grown_high_water / 2u;
    if (expected_shrunk_high_water < initial_high_water) {
        expected_shrunk_high_water = initial_high_water;
    }
    expect_true(heap->core.malloc_free_decommit_retention_high_water_bytes ==
                    grown_high_water,
                "adaptive hot retention keeps high water after first idle drain");
    expect_true(heap->core
                        .malloc_free_decommit_retention_adaptive_shrink_events ==
                    shrink_events_before,
                "adaptive hot retention delays shrink");

    for (shrink_cycle = 1u;
         shrink_cycle <
         UHM_MALLOC_FREE_DECOMMIT_RETENTION_SHRINK_IDLE_DRAINS;
         ++shrink_cycle) {
        int commit_before_cycle;

        for (i = 0; i < target_count; ++i) {
            uhm_block *block = test_find_block_by_payload(heap, targets[i]);

            expect_true(block != 0,
                        "adaptive hot retention shrink cycle block lookup");
            api->free(heap, targets[i]);
            expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0,
                        "adaptive hot retention shrink cycle retained block");
            targets[i] = 0;
        }

        commit_before_cycle = state.commit_count;
        for (i = 0; i < target_count; ++i) {
            targets[i] = api->malloc(heap, target_size);
            expect_true(targets[i] != 0,
                        "adaptive hot retention shrink cycle malloc");
        }
        expect_true(state.commit_count == commit_before_cycle,
                    "adaptive hot retention shrink cycle avoids recommit");
    }

    expect_true(heap->core.malloc_free_decommit_retention_high_water_bytes ==
                    expected_shrunk_high_water,
                "adaptive hot retention shrinks after delayed idle drains");
    expect_true(heap->core
                        .malloc_free_decommit_retention_adaptive_shrink_events >
                    shrink_events_before,
                "adaptive hot retention delayed shrink counted");
    expect_ok(api->check(heap, 0),
              "adaptive hot retention check after retained reuse");

    for (i = 0; i < target_count; ++i) {
        api->free(heap, targets[i]);
        api->free(heap, guards[i]);
    }
    expect_ok(api->check(heap, 0), "adaptive hot retention final check");
    expect_ok(api->close(heap), "adaptive hot retention close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_hot_retention_above_base_retained(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t target_size = 20u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *target;
    void *guard;
    void *reuse;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    size_t base_high_water;
    size_t max_high_water;
    uint64_t grow_events_before;
    int decommit_before_hot_free;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "hot above-base retention heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(16u * 1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "hot above-base retention arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "hot above-base retention usermode vm init");
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
    config.arena_size = 16u * 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "hot above-base retention heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "hot above-base retention api");
    base_high_water =
        heap->core.malloc_free_decommit_retention_base_high_water_bytes;
    max_high_water =
        heap->core.malloc_free_decommit_retention_max_high_water_bytes;

    target = api->malloc(heap, target_size);
    guard = api->malloc(heap, target_size);
    expect_true(target != 0 && guard != 0,
                "hot above-base retention malloc pair");
    block = test_find_block_by_payload(heap, target);
    expect_true(block != 0, "hot above-base retention block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size) != 0,
                "hot above-base retention has decommit range");
    expect_true(decommit_size > base_high_water &&
                    decommit_size <= max_high_water,
                "hot above-base retention range within max high water");

    api->free(heap, target);
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "hot above-base retention first free decommits");
    reuse = api->malloc(heap, target_size);
    expect_true(reuse == target,
                "hot above-base retention same address reuse");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0,
                "hot above-base retention hot hint set");

    grow_events_before =
        heap->core.malloc_free_decommit_retention_adaptive_grow_events;
    decommit_before_hot_free = state.decommit_count;
    api->free(heap, reuse);
    expect_true(state.decommit_count == decommit_before_hot_free,
                "hot above-base retention avoids second decommit");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0,
                "hot above-base retention retained");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0,
                "hot above-base retention stays committed");
    expect_true(heap->core
                    .malloc_free_decommit_retention_adaptive_grow_events >
                    grow_events_before,
                "hot above-base retention grows");
    expect_true(heap->core.malloc_free_decommit_retained_bytes >=
                    decommit_size,
                "hot above-base retention bytes counted");
    expect_true(heap->core.malloc_free_decommit_retention_high_water_bytes <=
                    max_high_water,
                "hot above-base retention high water capped");
    expect_ok(api->check(heap, 0),
              "hot above-base retention check after free");

    api->free(heap, guard);
    expect_ok(api->check(heap, 0), "hot above-base retention final check");
    expect_ok(api->close(heap), "hot above-base retention close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_hot_retention_over_max_decommits(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t target_size = 40u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *target;
    void *guard;
    void *reuse;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    size_t max_high_water;
    uint64_t grow_events_before;
    int decommit_before_hot_free;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "hot over-max decommit heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(16u * 1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "hot over-max decommit arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "hot over-max decommit usermode vm init");
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
    config.arena_size = 16u * 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "hot over-max decommit heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "hot over-max decommit api");
    max_high_water =
        heap->core.malloc_free_decommit_retention_max_high_water_bytes;

    target = api->malloc(heap, target_size);
    guard = api->malloc(heap, target_size);
    expect_true(target != 0 && guard != 0,
                "hot over-max decommit malloc pair");
    block = test_find_block_by_payload(heap, target);
    expect_true(block != 0, "hot over-max decommit block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size) != 0,
                "hot over-max decommit has decommit range");
    expect_true(decommit_size > max_high_water,
                "hot over-max decommit range exceeds max high water");

    api->free(heap, target);
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "hot over-max decommit first free decommits");
    reuse = api->malloc(heap, target_size);
    expect_true(reuse == target, "hot over-max decommit same address reuse");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0,
                "hot over-max decommit hot hint set");

    grow_events_before =
        heap->core.malloc_free_decommit_retention_adaptive_grow_events;
    decommit_before_hot_free = state.decommit_count;
    api->free(heap, reuse);
    expect_true(state.decommit_count > decommit_before_hot_free,
                "hot over-max decommit forces second decommit");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) == 0,
                "hot over-max decommit not retained");
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "hot over-max decommit stays decommitted");
    expect_true(heap->core
                    .malloc_free_decommit_retention_adaptive_grow_events ==
                    grow_events_before,
                "hot over-max decommit does not grow");
    expect_ok(api->check(heap, 0), "hot over-max decommit check after free");

    api->free(heap, guard);
    expect_ok(api->check(heap, 0), "hot over-max decommit final check");
    expect_ok(api->close(heap), "hot over-max decommit close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_large_malloc_decommit_retention_pages_config(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    const size_t small_large_size = 4u * page_size;
    void *ptr;
    uhm_block *block;
    uintptr_t decommit_addr;
    size_t decommit_size;
    uint64_t deferred_events_before;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "large retention config heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(2u * 1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "large retention config arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large retention config usermode vm init");
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
    config.malloc_free_decommit_retention_pages = 1u;

    expect_ok(uhm_open(&config, &heap), "large retention config heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large retention config api");
    expect_true(heap->core.malloc_free_decommit_retention_pages == 1u,
                "large retention config stored");

    ptr = api->malloc(heap, small_large_size);
    expect_true(ptr != 0, "large retention config malloc");
    block = test_find_block_by_payload(heap, ptr);
    expect_true(block != 0, "large retention config block lookup");
    expect_true(test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size),
                "large retention config has decommittable interior pages");
    expect_true(decommit_size > page_size,
                "large retention config exceeds one retained page");

    deferred_events_before =
        heap->core.malloc_free_decommit_deferred_events;
    api->free(heap, ptr);
    expect_true((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0,
                "large retention config decommitted");
    expect_true(state.decommit_count != 0,
                "large retention config decommit callback used");
    expect_true(heap->core.malloc_free_decommit_deferred_events ==
                    deferred_events_before,
                "large retention config did not defer");
    expect_ok(api->check(heap, 0), "large retention config check after free");

    expect_ok(api->close(heap), "large retention config close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_config_abi7_tail_compat_uses_default_retention(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "abi7 tail compat heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(2u * 1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "abi7 tail compat arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "abi7 tail compat usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = UHM_CONFIG_SIZE_ABI7;
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = 7u;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 2u * 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.malloc_free_decommit_retention_pages = 1u;

    expect_ok(uhm_open(&config, &heap),
              "abi7 tail compat heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "abi7 tail compat api");
    expect_true(heap->core.malloc_free_decommit_retention_pages == 8u,
                "abi7 tail compat ignores abi8 retention field");
    expect_ok(api->close(heap), "abi7 tail compat close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

