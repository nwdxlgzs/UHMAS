static void test_emergency_reserve_allows_release_after_metadata_exhaustion(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 512u * 1024u;
    const size_t control_size = 3u * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *split_map = 0;
    void *pressure_map = 0;
    void *pin;
    size_t i;
    int exhausted = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "emergency reserve heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "emergency reserve arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "emergency reserve usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "emergency reserve heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "emergency reserve api");

    expect_ok(api->mmap(heap, &split_map, page_size * 3u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "emergency reserve split source mmap");
    expect_ok(api->mmap(heap, &pressure_map, page_size * 80u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "emergency reserve pressure mmap");
    pin = api->malloc(heap, 16u * 1024u);
    expect_true(pin != 0, "emergency reserve collision pin");

    for (i = 0; i < 86u; i += 2u) {
        void *page = (void *)((uintptr_t)pressure_map + i * page_size);
        uhm_result result = api->mprotect(heap, page, page_size, UHM_PROT_READ);
        if (result == UHM_ERR_OUT_OF_MEMORY ||
            result == UHM_ERR_OUT_OF_ADDRESS_SPACE ||
            result == UHM_ERR_NOT_MAPPED) {
            exhausted = 1;
            break;
        }
        expect_ok(result, "emergency reserve pressure mprotect");
    }
    expect_true(exhausted,
                "emergency reserve exhausted normal metadata or address pressure");
    expect_ok(api->munmap(heap, (void *)((uintptr_t)split_map + page_size), page_size),
              "emergency reserve munmap middle after metadata exhaustion");
    expect_ok(api->check(heap, 0), "emergency reserve check");
    expect_ok(api->close(heap), "emergency reserve close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_quarantine_retired_nodes(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *reuse = 0;
    size_t retired_vmas;
    size_t retired_extents;
    uint64_t hot_vma_reclaimed_before;
    uint64_t hot_vma_hits_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "metadata quarantine heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "metadata quarantine arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata quarantine usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata quarantine heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata quarantine api");

    expect_ok(api->mmap(heap, &map, page_size * 3u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "metadata quarantine mmap");
    expect_ok(api->mprotect(heap, (void *)((uintptr_t)map + page_size),
                            page_size, UHM_PROT_READ),
              "metadata quarantine split vma");
    expect_ok(api->check(heap, 0), "metadata quarantine check after split");

    expect_ok(api->munmap(heap, map, page_size * 3u),
              "metadata quarantine munmap");
    retired_vmas = heap->core.retired_vma_count;
    hot_vma_reclaimed_before = heap->core.metadata_hot_vma_reclaimed;
    hot_vma_hits_before = heap->core.metadata_hot_vma_hits;
    expect_true(retired_vmas != 0, "metadata quarantine retired vma");
    expect_ok(api->check(heap, 0), "metadata quarantine check after vma retire");
    expect_ok(api->mmap(heap, &reuse, page_size * 3u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "metadata quarantine reuse free extent");
    retired_extents = heap->core.retired_free_extent_count;
    expect_true(retired_extents != 0, "metadata quarantine retired free extent");
    expect_true(heap->core.metadata_hot_vma_reclaimed >
                    hot_vma_reclaimed_before ||
                    heap->core.metadata_hot_vma_hits > hot_vma_hits_before,
                "metadata quarantine reclaims normal vmas into hot cache");
    expect_ok(api->check(heap, 0), "metadata quarantine final check");
    expect_ok(api->munmap(heap, reuse, page_size * 3u),
              "metadata quarantine cleanup munmap");
    expect_ok(api->close(heap), "metadata quarantine close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_quarantine_skips_empty_drain(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *reuse = 0;
    uint64_t calls_before;
    uint64_t calls_after_empty_mmap;
    uint64_t calls_after_empty_munmap;
    uint64_t calls_after_pending_mmap;
    uint64_t scanned_before;
    uint64_t scanned_after_pending_mmap;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata empty drain heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "metadata empty drain arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata empty drain usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata empty drain heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata empty drain api");

    calls_before = heap->core.metadata_quarantine_drain_calls;
    expect_ok(api->mmap(heap, &map, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "metadata empty drain mmap");
    calls_after_empty_mmap = heap->core.metadata_quarantine_drain_calls;
    expect_true(calls_after_empty_mmap == calls_before,
                "metadata empty drain skips mmap no pending");

    expect_ok(api->munmap(heap, map, page_size * 2u),
              "metadata empty drain munmap");
    calls_after_empty_munmap = heap->core.metadata_quarantine_drain_calls;
    expect_true(calls_after_empty_munmap == calls_after_empty_mmap,
                "metadata empty drain skips munmap no pending at entry");
    expect_true(heap->core.retired_vma_count != 0u ||
                    heap->core.retired_free_extent_count != 0u,
                "metadata empty drain munmap leaves pending retired metadata");

    scanned_before = heap->core.metadata_quarantine_drain_scanned;
    expect_ok(api->mmap(heap, &reuse, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "metadata empty drain pending mmap");
    calls_after_pending_mmap = heap->core.metadata_quarantine_drain_calls;
    scanned_after_pending_mmap = heap->core.metadata_quarantine_drain_scanned;
    expect_true(calls_after_pending_mmap == calls_after_empty_munmap + 1u,
                "metadata empty drain runs when pending exists");
    expect_true(scanned_after_pending_mmap > scanned_before,
                "metadata empty drain pending scans retired metadata");

    expect_ok(api->munmap(heap, reuse, page_size * 2u),
              "metadata empty drain cleanup munmap");
    expect_ok(api->close(heap), "metadata empty drain close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_quarantine_inflation_pressure(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_stats stats_before;
    uhm_stats stats_after;
    size_t i;
    uint64_t hot_vma_reclaimed_before;
    uint64_t hot_free_reclaimed_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "metadata inflation heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "metadata inflation arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata inflation usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata inflation heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata inflation api");
    memset(&stats_before, 0, sizeof(stats_before));
    stats_before.size = sizeof(stats_before);
    expect_ok(api->get_stats(heap, &stats_before),
              "metadata inflation public stats before");
    hot_vma_reclaimed_before = heap->core.metadata_hot_vma_reclaimed;
    hot_free_reclaimed_before =
        heap->core.metadata_hot_free_extent_reclaimed;

    for (i = 0; i < 96u; ++i) {
        void *map = 0;
        void *reuse = 0;
        expect_ok(api->mmap(heap, &map, page_size * 4u,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
                  "metadata inflation mmap");
        expect_ok(api->mprotect(heap, (void *)((uintptr_t)map + page_size),
                                page_size * 2u, UHM_PROT_READ),
                  "metadata inflation split");
        expect_ok(api->munmap(heap, map, page_size * 4u),
                  "metadata inflation munmap");
        expect_ok(api->mmap(heap, &reuse, page_size * 4u,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
                  "metadata inflation reuse extent");
        expect_ok(api->munmap(heap, reuse, page_size * 4u),
                  "metadata inflation reuse cleanup");
        if ((i % 16u) == 0) {
            expect_ok(api->check(heap, 0), "metadata inflation periodic check");
        }
    }

    expect_true(heap->core.metadata_hot_vma_reclaimed >
                    hot_vma_reclaimed_before,
                "metadata inflation vmas reclaimed to hot cache");
    expect_true(heap->core.metadata_hot_free_extent_reclaimed >
                    hot_free_reclaimed_before,
                "metadata inflation extents reclaimed to hot cache");
    expect_true(heap->core.metadata_hot_vma_hits != 0,
                "metadata inflation vma hot cache hits");
    expect_true(heap->core.metadata_hot_free_extent_hits != 0,
                "metadata inflation extent hot cache hits");
    expect_true(heap->core.retired_vma_count < 96u ||
                    heap->core.hot_vma_count != 0u,
                "metadata inflation retired vma pressure bounded");
    expect_true(heap->core.retired_free_extent_count < 96u ||
                    heap->core.hot_free_extent_count != 0u,
                "metadata inflation retired extent pressure bounded");
    memset(&stats_after, 0, sizeof(stats_after));
    stats_after.size = sizeof(stats_after);
    expect_ok(api->get_stats(heap, &stats_after),
              "metadata inflation public stats after");
    expect_true(stats_after.metadata_hot_vma_reclaimed >
                    stats_before.metadata_hot_vma_reclaimed,
                "metadata inflation public vma reclaim counted");
    expect_true(stats_after.metadata_hot_free_extent_reclaimed >
                    stats_before.metadata_hot_free_extent_reclaimed,
                "metadata inflation public extent reclaim counted");
    expect_true(stats_after.metadata_hot_vma_hits != 0u &&
                    stats_after.metadata_hot_free_extent_hits != 0u,
                "metadata inflation public hot hits counted");
    expect_true(stats_after.metadata_retired_vma_count ==
                        (uint64_t)heap->core.retired_vma_count &&
                    stats_after.metadata_hot_vma_count ==
                        (uint64_t)heap->core.hot_vma_count,
                "metadata inflation public metadata counts match core");
    expect_ok(api->check(heap, 0), "metadata inflation final check");
    expect_ok(api->close(heap), "metadata inflation close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_hot_cache_capacity_limit(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma hot_vmas[UHM_METADATA_HOT_VMA_MAX];
    uhm_free_extent hot_extents[UHM_METADATA_HOT_FREE_EXTENT_MAX];
    uhm_vma retired_vmas[UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u];
    uhm_free_extent
        retired_extents[UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u];
    void *pin;
    size_t i;
    uint64_t vma_full_before;
    uint64_t free_full_before;
    uint64_t scanned_before;
    uint64_t exhausted_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata hot cap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "metadata hot cap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata hot cap usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata hot cap heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata hot cap api");
    expect_true(heap->core.hot_vma_count == 0u &&
                    heap->core.hot_free_extent_count == 0u &&
                    heap->core.retired_vma_count == 0u &&
                    heap->core.retired_free_extent_count == 0u,
                "metadata hot cap starts empty");

    memset(hot_vmas, 0, sizeof(hot_vmas));
    for (i = 0; i < UHM_METADATA_HOT_VMA_MAX; ++i) {
        hot_vmas[i].next =
            i + 1u < UHM_METADATA_HOT_VMA_MAX ? &hot_vmas[i + 1u] : 0;
    }
    heap->core.hot_vma_head = &hot_vmas[0];
    heap->core.hot_vma_count = UHM_METADATA_HOT_VMA_MAX;

    memset(hot_extents, 0, sizeof(hot_extents));
    for (i = 0; i < UHM_METADATA_HOT_FREE_EXTENT_MAX; ++i) {
        hot_extents[i].next =
            i + 1u < UHM_METADATA_HOT_FREE_EXTENT_MAX
                ? &hot_extents[i + 1u]
                : 0;
    }
    heap->core.hot_free_extent_head = &hot_extents[0];
    heap->core.hot_free_extent_count = UHM_METADATA_HOT_FREE_EXTENT_MAX;

    memset(retired_vmas, 0, sizeof(retired_vmas));
    for (i = 0;
         i < UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u;
         ++i) {
        retired_vmas[i].retire_epoch = heap->core.metadata_epoch;
        retired_vmas[i].next =
            i + 1u < UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u
                ? &retired_vmas[i + 1u]
                : 0;
    }
    heap->core.retired_vma_head = &retired_vmas[0];
    heap->core.retired_vma_tail =
        &retired_vmas[UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 1u];
    heap->core.retired_vma_count =
        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u;

    memset(retired_extents, 0, sizeof(retired_extents));
    for (i = 0;
         i < UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u;
         ++i) {
        retired_extents[i].retire_epoch = heap->core.metadata_epoch;
        retired_extents[i].next =
            i + 1u < UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u
                ? &retired_extents[i + 1u]
                : 0;
    }
    heap->core.retired_free_extent_head = &retired_extents[0];
    heap->core.retired_free_extent_tail =
        &retired_extents[UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 1u];
    heap->core.retired_free_extent_count =
        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u;

    vma_full_before = heap->core.metadata_hot_vma_full_defers;
    free_full_before = heap->core.metadata_hot_free_extent_full_defers;
    scanned_before = heap->core.metadata_quarantine_drain_scanned;
    exhausted_before = heap->core.metadata_quarantine_drain_budget_exhausted;
    pin = api->malloc(heap, 64u);
    expect_true(pin != 0, "metadata hot cap epoch advance allocation");
    expect_true(heap->core.hot_vma_count == UHM_METADATA_HOT_VMA_MAX &&
                    heap->core.retired_vma_count ==
                        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u &&
                    heap->core.retired_vma_head == &retired_vmas[0],
                "metadata hot cap defers vma reclaim when full");
    expect_true(heap->core.hot_free_extent_count ==
                        UHM_METADATA_HOT_FREE_EXTENT_MAX &&
                    heap->core.retired_free_extent_count ==
                        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u &&
                    heap->core.retired_free_extent_head ==
                        &retired_extents[0],
                "metadata hot cap defers free extent reclaim when full");
    expect_true(heap->core.metadata_quarantine_drain_scanned ==
                    scanned_before,
                "metadata hot cap skips saturated lists without scanning");
    expect_true(heap->core.metadata_quarantine_drain_budget_exhausted ==
                    exhausted_before,
                "metadata hot cap does not exhaust budget while saturated");
    expect_true(heap->core.metadata_hot_vma_full_defers ==
                    vma_full_before + 1u,
                "metadata hot cap vma full counted once");
    expect_true(heap->core.metadata_hot_free_extent_full_defers ==
                    free_full_before + 1u,
                "metadata hot cap free extent full counted once");

    heap->core.hot_vma_head = 0;
    heap->core.hot_vma_count = 0;
    heap->core.metadata_hot_vma_refill_deferred = 0;
    heap->core.hot_free_extent_head = 0;
    heap->core.hot_free_extent_count = 0;
    heap->core.metadata_hot_free_extent_refill_deferred = 0;
    heap->core.retired_vma_head = 0;
    heap->core.retired_vma_tail = 0;
    heap->core.retired_vma_count = 0;
    heap->core.retired_free_extent_head = 0;
    heap->core.retired_free_extent_tail = 0;
    heap->core.retired_free_extent_count = 0;

    api->free(heap, pin);
    expect_ok(api->check(heap, 0), "metadata hot cap final check");
    expect_ok(api->close(heap), "metadata hot cap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_hot_cache_drain_high_water_limit(
    const uhm_storage_info *heap_info) {
    enum {
        retired_node_count =
            UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER -
            UHM_METADATA_HOT_VMA_DRAIN_LOW_WATER +
            UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u,
        test_quarantine_list_vma = 2u,
        test_quarantine_list_free = 3u
    };
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma hot_vmas[UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER];
    uhm_free_extent
        hot_extents[UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER];
    uhm_vma retired_vmas[retired_node_count];
    uhm_free_extent retired_extents[retired_node_count];
    void *pins[7] = {0};
    size_t i;
    size_t pin_count = 0;
    uint64_t vma_full_before;
    uint64_t free_full_before;
    uint64_t scanned_before;
    uint64_t reclaimed_before;
    uint64_t exhausted_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata hot high-water heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "metadata hot high-water arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata hot high-water usermode vm init");

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

    expect_ok(uhm_open(&config, &heap),
              "metadata hot high-water heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata hot high-water api");

    memset(hot_vmas, 0, sizeof(hot_vmas));
    for (i = 0; i < UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER; ++i) {
        hot_vmas[i].next =
            i + 1u < UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER
                ? &hot_vmas[i + 1u]
                : 0;
    }
    heap->core.hot_vma_head = &hot_vmas[0];
    heap->core.hot_vma_count = UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER;

    memset(hot_extents, 0, sizeof(hot_extents));
    for (i = 0; i < UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER; ++i) {
        hot_extents[i].next =
            i + 1u < UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER
                ? &hot_extents[i + 1u]
                : 0;
    }
    heap->core.hot_free_extent_head = &hot_extents[0];
    heap->core.hot_free_extent_count =
        UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER;

    memset(retired_vmas, 0, sizeof(retired_vmas));
    for (i = 0; i < retired_node_count; ++i) {
        retired_vmas[i].retire_epoch = heap->core.metadata_epoch;
        retired_vmas[i].next =
            i + 1u < retired_node_count
                ? &retired_vmas[i + 1u]
                : 0;
    }
    heap->core.retired_vma_head = &retired_vmas[0];
    heap->core.retired_vma_tail = &retired_vmas[retired_node_count - 1u];
    heap->core.retired_vma_count = retired_node_count;

    memset(retired_extents, 0, sizeof(retired_extents));
    for (i = 0; i < retired_node_count; ++i) {
        retired_extents[i].retire_epoch = heap->core.metadata_epoch;
        retired_extents[i].next =
            i + 1u < retired_node_count
                ? &retired_extents[i + 1u]
                : 0;
    }
    heap->core.retired_free_extent_head = &retired_extents[0];
    heap->core.retired_free_extent_tail =
        &retired_extents[retired_node_count - 1u];
    heap->core.retired_free_extent_count = retired_node_count;

    vma_full_before = heap->core.metadata_hot_vma_full_defers;
    free_full_before = heap->core.metadata_hot_free_extent_full_defers;
    scanned_before = heap->core.metadata_quarantine_drain_scanned;
    reclaimed_before = heap->core.metadata_quarantine_drain_reclaimed;
    exhausted_before = heap->core.metadata_quarantine_drain_budget_exhausted;
    pins[pin_count] = api->malloc(heap, 64u);
    expect_true(pins[pin_count] != 0,
                "metadata hot high-water epoch advance allocation");
    ++pin_count;
    expect_true(heap->core.hot_vma_count ==
                        UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER &&
                    heap->core.retired_vma_count == retired_node_count &&
                    heap->core.retired_vma_head == &retired_vmas[0],
                "metadata hot high-water defers vma reclaim");
    expect_true(heap->core.hot_free_extent_count ==
                        UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER &&
                    heap->core.retired_free_extent_count ==
                        retired_node_count &&
                    heap->core.retired_free_extent_head ==
                        &retired_extents[0],
                "metadata hot high-water defers free extent reclaim");
    expect_true(heap->core.metadata_quarantine_drain_scanned ==
                    scanned_before,
                "metadata hot high-water skips saturated lists");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before,
                "metadata hot high-water reclaims nothing");
    expect_true(heap->core.metadata_quarantine_drain_budget_exhausted ==
                    exhausted_before,
                "metadata hot high-water does not exhaust budget");
    expect_true(heap->core.metadata_hot_vma_full_defers ==
                    vma_full_before + 1u,
                "metadata hot high-water vma defer counted");
    expect_true(heap->core.metadata_hot_free_extent_full_defers ==
                    free_full_before + 1u,
                "metadata hot high-water free extent defer counted");
    pins[pin_count] = api->malloc(heap, 64u);
    expect_true(pins[pin_count] != 0,
                "metadata hot high-water second epoch allocation");
    ++pin_count;
    expect_true(heap->core.metadata_hot_vma_full_defers ==
                    vma_full_before + 1u,
                "metadata hot high-water vma defer latched");
    expect_true(heap->core.metadata_hot_free_extent_full_defers ==
                    free_full_before + 1u,
                "metadata hot high-water free extent defer latched");
    expect_true(heap->core.metadata_quarantine_drain_scanned ==
                    scanned_before,
                "metadata hot high-water latch avoids scan");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before,
                "metadata hot high-water latch reclaims nothing");

    heap->core.hot_vma_count =
        UHM_METADATA_HOT_VMA_DRAIN_LOW_WATER + 1u;
    heap->core.hot_free_extent_count =
        UHM_METADATA_HOT_FREE_EXTENT_DRAIN_LOW_WATER + 1u;
    heap->core.metadata_quarantine_drain_next_list =
        test_quarantine_list_vma;
    pins[pin_count] = api->malloc(heap, 64u);
    expect_true(pins[pin_count] != 0,
                "metadata hot low-water above-low allocation");
    ++pin_count;
    expect_true(heap->core.metadata_quarantine_drain_scanned ==
                    scanned_before,
                "metadata hot low-water keeps deferring above low-water");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before,
                "metadata hot low-water above-low reclaims nothing");
    expect_true(heap->core.metadata_hot_vma_full_defers ==
                    vma_full_before + 1u &&
                    heap->core.metadata_hot_free_extent_full_defers ==
                        free_full_before + 1u,
                "metadata hot low-water above-low keeps defer latch");

    heap->core.hot_vma_count = UHM_METADATA_HOT_VMA_DRAIN_LOW_WATER;
    heap->core.metadata_quarantine_drain_next_list =
        test_quarantine_list_vma;
    pins[pin_count] = api->malloc(heap, 64u);
    expect_true(pins[pin_count] != 0,
                "metadata hot low-water first vma refill allocation");
    ++pin_count;
    expect_true(heap->core.hot_vma_count >
                    UHM_METADATA_HOT_VMA_DRAIN_LOW_WATER,
                "metadata hot low-water resumes vma refill at low-water");
    pins[pin_count] = api->malloc(heap, 64u);
    expect_true(pins[pin_count] != 0,
                "metadata hot low-water second vma refill allocation");
    ++pin_count;
    expect_true(heap->core.hot_vma_count ==
                    UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER,
                "metadata hot low-water refills vma to high-water");

    heap->core.hot_free_extent_count =
        UHM_METADATA_HOT_FREE_EXTENT_DRAIN_LOW_WATER;
    heap->core.metadata_quarantine_drain_next_list =
        test_quarantine_list_free;
    pins[pin_count] = api->malloc(heap, 64u);
    expect_true(pins[pin_count] != 0,
                "metadata hot low-water first free refill allocation");
    ++pin_count;
    expect_true(heap->core.hot_free_extent_count >
                    UHM_METADATA_HOT_FREE_EXTENT_DRAIN_LOW_WATER,
                "metadata hot low-water resumes free extent refill");
    pins[pin_count] = api->malloc(heap, 64u);
    expect_true(pins[pin_count] != 0,
                "metadata hot low-water second free refill allocation");
    ++pin_count;
    expect_true(heap->core.hot_free_extent_count ==
                    UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER,
                "metadata hot low-water refills free extent to high-water");
    expect_true(heap->core.metadata_quarantine_drain_scanned >
                    scanned_before,
                "metadata hot low-water refill scans only after low-water");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed >
                    reclaimed_before,
                "metadata hot low-water refill reclaims after low-water");

    heap->core.hot_vma_head = 0;
    heap->core.hot_vma_count = 0;
    heap->core.metadata_hot_vma_refill_deferred = 0;
    heap->core.hot_free_extent_head = 0;
    heap->core.hot_free_extent_count = 0;
    heap->core.metadata_hot_free_extent_refill_deferred = 0;
    heap->core.retired_vma_head = 0;
    heap->core.retired_vma_tail = 0;
    heap->core.retired_vma_count = 0;
    heap->core.retired_free_extent_head = 0;
    heap->core.retired_free_extent_tail = 0;
    heap->core.retired_free_extent_count = 0;

    while (pin_count != 0) {
        --pin_count;
        api->free(heap, pins[pin_count]);
    }
    expect_ok(api->check(heap, 0), "metadata hot high-water final check");
    expect_ok(api->close(heap), "metadata hot high-water close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_hot_cache_batch_return_invariants(
    const uhm_storage_info *heap_info) {
    enum {
        retired_vma_count = 3u,
        retired_free_extent_count = 4u
    };
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma retired_vmas[retired_vma_count];
    uhm_free_extent retired_extents[retired_free_extent_count];
    void *pin;
    size_t i;
    uint64_t reclaimed_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata hot batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "metadata hot batch arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata hot batch usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata hot batch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata hot batch api");

    memset(retired_vmas, 0, sizeof(retired_vmas));
    for (i = 0; i < retired_vma_count; ++i) {
        retired_vmas[i].retire_epoch = heap->core.metadata_epoch;
        retired_vmas[i].next =
            i + 1u < retired_vma_count ? &retired_vmas[i + 1u] : 0;
    }
    heap->core.retired_vma_head = &retired_vmas[0];
    heap->core.retired_vma_tail = &retired_vmas[retired_vma_count - 1u];
    heap->core.retired_vma_count = retired_vma_count;

    memset(retired_extents, 0, sizeof(retired_extents));
    for (i = 0; i < retired_free_extent_count; ++i) {
        retired_extents[i].retire_epoch = heap->core.metadata_epoch;
        retired_extents[i].next =
            i + 1u < retired_free_extent_count
                ? &retired_extents[i + 1u]
                : 0;
    }
    heap->core.retired_free_extent_head = &retired_extents[0];
    heap->core.retired_free_extent_tail =
        &retired_extents[retired_free_extent_count - 1u];
    heap->core.retired_free_extent_count = retired_free_extent_count;

    reclaimed_before = heap->core.metadata_quarantine_drain_reclaimed;
    pin = api->malloc(heap, 64u);
    expect_true(pin != 0, "metadata hot batch trigger allocation");
    expect_true(heap->core.retired_vma_count == 0u &&
                    heap->core.retired_vma_head == 0 &&
                    heap->core.retired_vma_tail == 0,
                "metadata hot batch drains vma retired list");
    expect_true(heap->core.retired_free_extent_count == 0u &&
                    heap->core.retired_free_extent_head == 0 &&
                    heap->core.retired_free_extent_tail == 0,
                "metadata hot batch drains free extent retired list");
    expect_true(heap->core.hot_vma_count == retired_vma_count,
                "metadata hot batch vma hot count exact");
    expect_true(heap->core.hot_free_extent_count ==
                    retired_free_extent_count,
                "metadata hot batch free extent hot count exact");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before + retired_vma_count +
                        retired_free_extent_count,
                "metadata hot batch reclaimed count exact");
    expect_ok(api->check(heap, 0), "metadata hot batch check exact lists");

    heap->core.hot_vma_head = 0;
    heap->core.hot_vma_count = 0;
    heap->core.hot_free_extent_head = 0;
    heap->core.hot_free_extent_count = 0;

    api->free(heap, pin);
    expect_ok(api->close(heap), "metadata hot batch close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_hot_cache_pending_high_water_budget(
    const uhm_storage_info *heap_info) {
    enum {
        hot_vma_count = UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER - 1u,
        hot_free_count =
            UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER - 1u,
        retired_count = 3u
    };
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma hot_vmas[hot_vma_count];
    uhm_free_extent hot_extents[hot_free_count];
    uhm_vma retired_vmas[retired_count];
    uhm_free_extent retired_extents[retired_count];
    void *pin;
    size_t i;
    uint64_t scanned_before;
    uint64_t reclaimed_before;
    uint64_t exhausted_before;
    uint64_t vma_full_before;
    uint64_t free_full_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata hot pending heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "metadata hot pending arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata hot pending usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata hot pending heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata hot pending api");

    memset(hot_vmas, 0, sizeof(hot_vmas));
    for (i = 0; i < hot_vma_count; ++i) {
        hot_vmas[i].next = i + 1u < hot_vma_count ? &hot_vmas[i + 1u] : 0;
    }
    heap->core.hot_vma_head = &hot_vmas[0];
    heap->core.hot_vma_count = hot_vma_count;

    memset(hot_extents, 0, sizeof(hot_extents));
    for (i = 0; i < hot_free_count; ++i) {
        hot_extents[i].next =
            i + 1u < hot_free_count ? &hot_extents[i + 1u] : 0;
    }
    heap->core.hot_free_extent_head = &hot_extents[0];
    heap->core.hot_free_extent_count = hot_free_count;

    memset(retired_vmas, 0, sizeof(retired_vmas));
    for (i = 0; i < retired_count; ++i) {
        retired_vmas[i].retire_epoch = heap->core.metadata_epoch;
        retired_vmas[i].next =
            i + 1u < retired_count ? &retired_vmas[i + 1u] : 0;
    }
    heap->core.retired_vma_head = &retired_vmas[0];
    heap->core.retired_vma_tail = &retired_vmas[retired_count - 1u];
    heap->core.retired_vma_count = retired_count;

    memset(retired_extents, 0, sizeof(retired_extents));
    for (i = 0; i < retired_count; ++i) {
        retired_extents[i].retire_epoch = heap->core.metadata_epoch;
        retired_extents[i].next =
            i + 1u < retired_count ? &retired_extents[i + 1u] : 0;
    }
    heap->core.retired_free_extent_head = &retired_extents[0];
    heap->core.retired_free_extent_tail =
        &retired_extents[retired_count - 1u];
    heap->core.retired_free_extent_count = retired_count;
    heap->core.metadata_quarantine_drain_next_list = 2u;

    scanned_before = heap->core.metadata_quarantine_drain_scanned;
    reclaimed_before = heap->core.metadata_quarantine_drain_reclaimed;
    exhausted_before = heap->core.metadata_quarantine_drain_budget_exhausted;
    vma_full_before = heap->core.metadata_hot_vma_full_defers;
    free_full_before = heap->core.metadata_hot_free_extent_full_defers;

    pin = api->malloc(heap, 64u);
    expect_true(pin != 0, "metadata hot pending trigger allocation");
    expect_true(heap->core.hot_vma_count ==
                    UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER,
                "metadata hot pending vma reaches high-water exactly");
    expect_true(heap->core.hot_free_extent_count ==
                    UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER,
                "metadata hot pending free extent reaches high-water exactly");
    expect_true(heap->core.retired_vma_count == retired_count - 1u,
                "metadata hot pending leaves extra vma retired");
    expect_true(heap->core.retired_free_extent_count == retired_count - 1u,
                "metadata hot pending leaves extra free extent retired");
    expect_true(heap->core.metadata_quarantine_drain_scanned ==
                    scanned_before + 2u,
                "metadata hot pending scans only reclaimed nodes");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before + 2u,
                "metadata hot pending reclaims exact batch nodes");
    expect_true(heap->core.metadata_quarantine_drain_budget_exhausted ==
                    exhausted_before,
                "metadata hot pending does not burn budget on saturation");
    expect_true(heap->core.metadata_hot_vma_full_defers ==
                    vma_full_before + 1u,
                "metadata hot pending vma defer counted once");
    expect_true(heap->core.metadata_hot_free_extent_full_defers ==
                    free_full_before + 1u,
                "metadata hot pending free extent defer counted once");

    heap->core.hot_vma_head = 0;
    heap->core.hot_vma_count = 0;
    heap->core.metadata_hot_vma_refill_deferred = 0;
    heap->core.hot_free_extent_head = 0;
    heap->core.hot_free_extent_count = 0;
    heap->core.metadata_hot_free_extent_refill_deferred = 0;
    heap->core.retired_vma_head = 0;
    heap->core.retired_vma_tail = 0;
    heap->core.retired_vma_count = 0;
    heap->core.retired_free_extent_head = 0;
    heap->core.retired_free_extent_tail = 0;
    heap->core.retired_free_extent_count = 0;

    api->free(heap, pin);
    expect_ok(api->check(heap, 0), "metadata hot pending final check");
    expect_ok(api->close(heap), "metadata hot pending close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static uhm_vma *test_retire_emergency_vma_node(uhm_heap *heap,
                                               uint64_t retired_epoch) {
    uhm_vma *node;

    if (!heap || !heap->core.emergency_vma_head) {
        return 0;
    }
    node = heap->core.emergency_vma_head;
    heap->core.emergency_vma_head = node->next;
    --heap->core.emergency_vma_available;
    memset(node, 0, sizeof(*node));
    node->flags = UHM_META_FLAG_EMERGENCY;
    node->retire_epoch = retired_epoch;
    node->next = heap->core.retired_emergency_vma_head;
    heap->core.retired_emergency_vma_head = node;
    if (!heap->core.retired_emergency_vma_tail) {
        heap->core.retired_emergency_vma_tail = node;
    }
    ++heap->core.retired_emergency_vma_count;
    ++heap->core.retired_vma_count;
    atomic_store_explicit(&heap->core.metadata_reclaim_pending, 1u,
                          UHM_ATOMIC_RELEASE);
    return node;
}

static uhm_free_extent *test_retire_emergency_free_extent_node(
    uhm_heap *heap, uint64_t retired_epoch) {
    uhm_free_extent *node;

    if (!heap || !heap->core.emergency_free_extent_head) {
        return 0;
    }
    node = heap->core.emergency_free_extent_head;
    heap->core.emergency_free_extent_head = node->next;
    --heap->core.emergency_free_extent_available;
    memset(node, 0, sizeof(*node));
    node->flags = UHM_META_FLAG_EMERGENCY;
    node->retire_epoch = retired_epoch;
    node->next = heap->core.retired_emergency_free_extent_head;
    heap->core.retired_emergency_free_extent_head = node;
    if (!heap->core.retired_emergency_free_extent_tail) {
        heap->core.retired_emergency_free_extent_tail = node;
    }
    ++heap->core.retired_emergency_free_extent_count;
    ++heap->core.retired_free_extent_count;
    atomic_store_explicit(&heap->core.metadata_reclaim_pending, 1u,
                          UHM_ATOMIC_RELEASE);
    return node;
}

static void test_metadata_quarantine_rotates_drain_start(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *pin;
    void *pin2;
    uint64_t retired_epoch;
    uint64_t scanned_before;
    uint64_t reclaimed_before;
    size_t retired_extents = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata rotate heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "metadata rotate arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata rotate usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata rotate heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata rotate api");

    retired_epoch = heap->core.metadata_epoch;
    expect_true(test_retire_emergency_vma_node(heap, retired_epoch) != 0,
                "metadata rotate retire emergency vma");
    while (heap->core.emergency_free_extent_head) {
        expect_true(test_retire_emergency_free_extent_node(heap,
                                                           retired_epoch) != 0,
                    "metadata rotate retire emergency free extent");
        ++retired_extents;
    }
    expect_true(retired_extents == UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata rotate free extent count fills budget");

    heap->core.metadata_quarantine_drain_next_list = 1u;
    scanned_before = heap->core.metadata_quarantine_drain_scanned;
    reclaimed_before = heap->core.metadata_quarantine_drain_reclaimed;

    pin = api->malloc(heap, 64u);
    expect_true(pin != 0, "metadata rotate first allocation");
    expect_true(heap->core.metadata_quarantine_drain_scanned ==
                    scanned_before +
                        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata rotate first drain spends budget");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before +
                        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata rotate first drain reclaims free extents");
    expect_true(heap->core.emergency_free_extent_available ==
                    UHM_EMERGENCY_FREE_EXTENT_NODES,
                "metadata rotate free extents reclaimed first");
    expect_true(heap->core.emergency_vma_available ==
                    UHM_EMERGENCY_VMA_NODES - 1u &&
                    heap->core.retired_emergency_vma_count == 1u,
                "metadata rotate leaves vma for next round");
    expect_true(heap->core.metadata_quarantine_drain_next_list == 2u,
                "metadata rotate advances cursor after exhausted list");

    pin2 = api->malloc(heap, 64u);
    expect_true(pin2 != 0, "metadata rotate second allocation");
    expect_true(heap->core.emergency_vma_available == UHM_EMERGENCY_VMA_NODES &&
                    heap->core.retired_emergency_vma_count == 0u,
                "metadata rotate second drain reclaims delayed vma");
    expect_ok(api->check(heap, 0), "metadata rotate final check");

    api->free(heap, pin);
    api->free(heap, pin2);
    expect_ok(api->close(heap), "metadata rotate close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_quarantine_rotates_blocked_head(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_metadata_reader_state *reader;
    uhm_vma *blocked_head;
    uhm_vma *reclaimable_tail;
    void *warm;
    void *pin;
    void *pin2;
    uint64_t retired_epoch;
    uint64_t scanned_before;
    uint64_t reclaimed_before;
    uint64_t rotated_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata blocked head heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "metadata blocked head arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata blocked head usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata blocked head heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata blocked head api");

    warm = api->malloc(heap, 64u);
    expect_true(warm != 0, "metadata blocked head warm allocation");
    api->free(heap, warm);
    expect_true(heap->core.metadata_epoch > 0u,
                "metadata blocked head epoch advanced");

    retired_epoch = heap->core.metadata_epoch - 1u;
    reclaimable_tail = test_retire_emergency_vma_node(heap, retired_epoch);
    blocked_head = test_retire_emergency_vma_node(heap, retired_epoch);
    expect_true(blocked_head != 0 && reclaimable_tail != 0 &&
                    heap->core.retired_emergency_vma_head == blocked_head &&
                    heap->core.retired_emergency_vma_tail == reclaimable_tail,
                "metadata blocked head retired order");

    reader = &heap->core.lockless_remote_readers[0];
    expect_true(uhm_metadata_reader_begin(reader, heap),
                "metadata blocked head reader begin");
    expect_true(uhm_metadata_reader_hazard_set(reader, 0u, blocked_head),
                "metadata blocked head hazard set");
    heap->core.metadata_quarantine_drain_next_list = 0u;
    scanned_before = heap->core.metadata_quarantine_drain_scanned;
    reclaimed_before = heap->core.metadata_quarantine_drain_reclaimed;
    rotated_before = heap->core.metadata_quarantine_drain_rotated;

    pin = api->malloc(heap, 64u);
    expect_true(pin != 0, "metadata blocked head trigger allocation");
    expect_true(heap->core.metadata_quarantine_drain_scanned >=
                    scanned_before + 2u,
                "metadata blocked head scans hazard and successor");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before + 1u,
                "metadata blocked head reclaims successor");
    expect_true(heap->core.metadata_quarantine_drain_rotated ==
                    rotated_before + 1u,
                "metadata blocked head rotation counted");
    expect_true(heap->core.retired_emergency_vma_count == 1u &&
                    heap->core.retired_emergency_vma_head == blocked_head &&
                    heap->core.retired_emergency_vma_tail == blocked_head,
                "metadata blocked head remains retired");
    expect_true(heap->core.emergency_vma_available ==
                    UHM_EMERGENCY_VMA_NODES - 1u,
                "metadata blocked head recovered one vma");
    expect_ok(api->check(heap, 0), "metadata blocked head check while reader active");

    expect_true(uhm_metadata_reader_end(reader),
                "metadata blocked head reader end");
    pin2 = api->malloc(heap, 64u);
    expect_true(pin2 != 0, "metadata blocked head second allocation");
    expect_true(heap->core.retired_emergency_vma_count == 0u &&
                    heap->core.emergency_vma_available ==
                        UHM_EMERGENCY_VMA_NODES,
                "metadata blocked head later reclaims hazard node");
    expect_ok(api->check(heap, 0), "metadata blocked head final check");

    api->free(heap, pin);
    api->free(heap, pin2);
    expect_ok(api->close(heap), "metadata blocked head close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_quarantine_budgeted_drain(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *pin;
    void *pin2;
    void *pin3;
    void *pin4;
    void *pin5;
    size_t retired_vmas = 0;
    size_t retired_extents = 0;
    size_t initial_vma_available;
    size_t initial_extent_available;
    uint64_t retired_epoch;
    uint64_t calls_before;
    uint64_t scanned_before;
    uint64_t reclaimed_before;
    uint64_t exhausted_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata budget heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "metadata budget arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata budget usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata budget heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata budget api");

    initial_vma_available = heap->core.emergency_vma_available;
    initial_extent_available = heap->core.emergency_free_extent_available;
    expect_true(initial_vma_available == UHM_EMERGENCY_VMA_NODES &&
                    initial_extent_available ==
                        UHM_EMERGENCY_FREE_EXTENT_NODES,
                "metadata budget emergency reserve size");
    retired_epoch = heap->core.metadata_epoch;

    while (heap->core.emergency_vma_head) {
        uhm_vma *node = heap->core.emergency_vma_head;
        heap->core.emergency_vma_head = node->next;
        --heap->core.emergency_vma_available;
        memset(node, 0, sizeof(*node));
        node->flags = UHM_META_FLAG_EMERGENCY;
        node->retire_epoch = retired_epoch;
        node->next = heap->core.retired_emergency_vma_head;
        heap->core.retired_emergency_vma_head = node;
        if (!heap->core.retired_emergency_vma_tail) {
            heap->core.retired_emergency_vma_tail = node;
        }
        ++heap->core.retired_emergency_vma_count;
        ++heap->core.retired_vma_count;
        ++retired_vmas;
    }
    while (heap->core.emergency_free_extent_head) {
        uhm_free_extent *node = heap->core.emergency_free_extent_head;
        heap->core.emergency_free_extent_head = node->next;
        --heap->core.emergency_free_extent_available;
        memset(node, 0, sizeof(*node));
        node->flags = UHM_META_FLAG_EMERGENCY;
        node->retire_epoch = retired_epoch;
        node->next = heap->core.retired_emergency_free_extent_head;
        heap->core.retired_emergency_free_extent_head = node;
        if (!heap->core.retired_emergency_free_extent_tail) {
            heap->core.retired_emergency_free_extent_tail = node;
        }
        ++heap->core.retired_emergency_free_extent_count;
        ++heap->core.retired_free_extent_count;
        ++retired_extents;
    }
    expect_true(retired_vmas + retired_extents >
                    UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata budget test exceeds drain budget");

    calls_before = heap->core.metadata_quarantine_drain_calls;
    scanned_before = heap->core.metadata_quarantine_drain_scanned;
    reclaimed_before = heap->core.metadata_quarantine_drain_reclaimed;
    exhausted_before = heap->core.metadata_quarantine_drain_budget_exhausted;

    pin = api->malloc(heap, 64u);
    expect_true(pin != 0, "metadata budget first epoch allocation");
    expect_true(heap->core.metadata_quarantine_drain_calls == calls_before + 1u,
                "metadata budget first drain call counted");
    expect_true(heap->core.metadata_quarantine_drain_scanned ==
                    scanned_before +
                        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata budget first drain scanned only budget");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before +
                        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata budget first drain reclaimed only budget");
    expect_true(heap->core.metadata_quarantine_drain_budget_exhausted ==
                    exhausted_before,
                "metadata budget productive drain not exhausted");
    expect_true(heap->core.emergency_vma_available ==
                    initial_vma_available &&
                    heap->core.emergency_free_extent_available <
                        initial_extent_available,
                "metadata budget leaves some emergency extents retired");
    expect_ok(api->check(heap, 0), "metadata budget check after partial drain");

    pin2 = api->malloc(heap, 64u);
    expect_true(pin2 != 0, "metadata budget second epoch allocation");
    expect_true(heap->core.emergency_vma_available ==
                    initial_vma_available,
                "metadata budget vma fully reclaimed");
    expect_true(heap->core.emergency_free_extent_available ==
                    initial_extent_available,
                "metadata budget extent fully reclaimed");
    expect_ok(api->check(heap, 0), "metadata budget final check");

    retired_vmas = 0;
    retired_extents = 0;
    retired_epoch = heap->core.metadata_epoch + 1u;
    while (heap->core.emergency_vma_head) {
        uhm_vma *node = heap->core.emergency_vma_head;
        heap->core.emergency_vma_head = node->next;
        --heap->core.emergency_vma_available;
        memset(node, 0, sizeof(*node));
        node->flags = UHM_META_FLAG_EMERGENCY;
        node->retire_epoch = retired_epoch;
        node->next = heap->core.retired_emergency_vma_head;
        heap->core.retired_emergency_vma_head = node;
        if (!heap->core.retired_emergency_vma_tail) {
            heap->core.retired_emergency_vma_tail = node;
        }
        ++heap->core.retired_emergency_vma_count;
        ++heap->core.retired_vma_count;
        ++retired_vmas;
    }
    while (heap->core.emergency_free_extent_head) {
        uhm_free_extent *node = heap->core.emergency_free_extent_head;
        heap->core.emergency_free_extent_head = node->next;
        --heap->core.emergency_free_extent_available;
        memset(node, 0, sizeof(*node));
        node->flags = UHM_META_FLAG_EMERGENCY;
        node->retire_epoch = retired_epoch;
        node->next = heap->core.retired_emergency_free_extent_head;
        heap->core.retired_emergency_free_extent_head = node;
        if (!heap->core.retired_emergency_free_extent_tail) {
            heap->core.retired_emergency_free_extent_tail = node;
        }
        ++heap->core.retired_emergency_free_extent_count;
        ++heap->core.retired_free_extent_count;
        ++retired_extents;
    }
    expect_true(retired_vmas + retired_extents >
                    UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata budget no-progress test exceeds drain budget");

    calls_before = heap->core.metadata_quarantine_drain_calls;
    scanned_before = heap->core.metadata_quarantine_drain_scanned;
    reclaimed_before = heap->core.metadata_quarantine_drain_reclaimed;
    exhausted_before = heap->core.metadata_quarantine_drain_budget_exhausted;
    pin3 = api->malloc(heap, 64u);
    expect_true(pin3 != 0, "metadata budget no-progress trigger allocation");
    expect_true(heap->core.metadata_quarantine_drain_calls == calls_before + 1u,
                "metadata budget no-progress drain call counted");
    expect_true(heap->core.metadata_quarantine_drain_scanned ==
                    scanned_before +
                        UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata budget no-progress scanned budget");
    expect_true(heap->core.metadata_quarantine_drain_reclaimed ==
                    reclaimed_before,
                "metadata budget no-progress reclaimed none");
    expect_true(heap->core.metadata_quarantine_drain_budget_exhausted ==
                    exhausted_before + 1u,
                "metadata budget no-progress exhausted");

    {
        uhm_vma *vma;
        uhm_free_extent *extent;
        uint64_t reclaim_epoch = heap->core.metadata_epoch - 1u;
        for (vma = heap->core.retired_emergency_vma_head; vma;
             vma = vma->next) {
            vma->retire_epoch = reclaim_epoch;
        }
        for (extent = heap->core.retired_emergency_free_extent_head; extent;
             extent = extent->next) {
            extent->retire_epoch = reclaim_epoch;
        }
    }
    pin4 = api->malloc(heap, 64u);
    pin5 = api->malloc(heap, 64u);
    expect_true(pin4 != 0 && pin5 != 0,
                "metadata budget cleanup trigger allocations");
    expect_true(heap->core.emergency_vma_available == initial_vma_available &&
                    heap->core.emergency_free_extent_available ==
                        initial_extent_available,
                "metadata budget cleanup reclaimed no-progress nodes");
    expect_ok(api->check(heap, 0), "metadata budget cleanup check");

    api->free(heap, pin);
    api->free(heap, pin2);
    api->free(heap, pin3);
    api->free(heap, pin4);
    api->free(heap, pin5);
    expect_ok(api->close(heap), "metadata budget close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_quarantine_emergency_chain_diagnostics(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma *retired_vma;
    uhm_free_extent *retired_extent;
    uhm_vma *saved_vma_tail;
    uhm_free_extent *saved_extent_tail;
    uint64_t retired_epoch;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata emergency diag heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "metadata emergency diag arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata emergency diag usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata emergency diag heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata emergency diag api");
    retired_epoch = heap->core.metadata_epoch;
    retired_vma = test_retire_emergency_vma_node(heap, retired_epoch);
    retired_extent =
        test_retire_emergency_free_extent_node(heap, retired_epoch);
    expect_true(retired_vma != 0 && retired_extent != 0,
                "metadata emergency diag retired nodes");
    expect_ok(api->check(heap, 0), "metadata emergency diag initial check");

    atomic_store_explicit(&heap->core.metadata_reclaim_pending, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata emergency diag missing pending hint corruption");
    atomic_store_explicit(&heap->core.metadata_reclaim_pending, 1u,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata emergency diag pending hint restored");

    --heap->core.retired_emergency_vma_count;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata emergency diag vma split count corruption");
    ++heap->core.retired_emergency_vma_count;
    expect_ok(api->check(heap, 0),
              "metadata emergency diag vma split count restored");

    saved_vma_tail = heap->core.retired_emergency_vma_tail;
    heap->core.retired_emergency_vma_tail = 0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata emergency diag vma tail corruption");
    heap->core.retired_emergency_vma_tail = saved_vma_tail;
    expect_ok(api->check(heap, 0),
              "metadata emergency diag vma tail restored");

    --heap->core.retired_emergency_free_extent_count;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata emergency diag extent split count corruption");
    ++heap->core.retired_emergency_free_extent_count;
    expect_ok(api->check(heap, 0),
              "metadata emergency diag extent split count restored");

    saved_extent_tail = heap->core.retired_emergency_free_extent_tail;
    heap->core.retired_emergency_free_extent_tail = 0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata emergency diag extent tail corruption");
    heap->core.retired_emergency_free_extent_tail = saved_extent_tail;
    expect_ok(api->check(heap, 0),
              "metadata emergency diag extent tail restored");

    expect_ok(api->close(heap), "metadata emergency diag close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_quarantine_retired_tail_corruption_is_not_repaired(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma fake_vma;
    uhm_free_extent fake_extent;
    uhm_result last_error = UHM_OK;
    uint32_t site = 0;
    void *map = 0;
    void *reuse = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata retired tail heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "metadata retired tail arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata retired tail usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata retired tail heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata retired tail api");

    expect_ok(api->mmap(heap, &map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "metadata retired tail mmap");
    memset(&fake_vma, 0, sizeof(fake_vma));
    fake_vma.retire_epoch = heap->core.metadata_epoch + 1024u;
    heap->core.retired_vma_head = &fake_vma;
    heap->core.retired_vma_tail = 0;
    heap->core.retired_vma_count = 1u;
    expect_true(uhm_metadata_hazard_begin(heap),
                "metadata retired tail vma hazard begin");
    expect_ok(api->munmap(heap, map, page_size),
              "metadata retired tail munmap triggers vma release");
    expect_true(uhm_metadata_hazard_end(heap),
                "metadata retired tail vma hazard end");
    expect_ok(api->get_last_error(heap, &last_error, 0),
              "metadata retired tail vma last error query");
    site = atomic_load_explicit(&heap->core.debug_last_corruption_site,
                                UHM_ATOMIC_ACQUIRE);
    expect_true(last_error == UHM_ERR_CORRUPTION,
                "metadata retired tail vma sets corruption error");
    expect_true(site == UHM_CORRUPTION_SITE_RETIRED_VMA_TAIL,
                "metadata retired tail vma records corruption site");
    expect_true(heap->core.retired_vma_tail == 0,
                "metadata retired tail vma not silently repaired");
    heap->core.retired_vma_head = 0;
    heap->core.retired_vma_tail = 0;
    heap->core.retired_vma_count = 0;
    atomic_store_explicit(&heap->core.debug_last_corruption_site, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&heap->last_error, (int)UHM_OK,
                          UHM_ATOMIC_RELEASE);

    expect_ok(api->mmap(heap, &map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "metadata retired tail free source mmap");
    expect_ok(api->munmap(heap, map, page_size),
              "metadata retired tail free source munmap");
    memset(&fake_extent, 0, sizeof(fake_extent));
    fake_extent.retire_epoch = heap->core.metadata_epoch + 1024u;
    heap->core.retired_free_extent_head = &fake_extent;
    heap->core.retired_free_extent_tail = 0;
    heap->core.retired_free_extent_count = 1u;
    expect_true(uhm_metadata_hazard_begin(heap),
                "metadata retired tail extent hazard begin");
    expect_ok(api->mmap(heap, &reuse, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "metadata retired tail mmap triggers extent release");
    expect_true(uhm_metadata_hazard_end(heap),
                "metadata retired tail extent hazard end");
    expect_ok(api->get_last_error(heap, &last_error, 0),
              "metadata retired tail extent last error query");
    site = atomic_load_explicit(&heap->core.debug_last_corruption_site,
                                UHM_ATOMIC_ACQUIRE);
    expect_true(last_error == UHM_ERR_CORRUPTION,
                "metadata retired tail extent sets corruption error");
    expect_true(site == UHM_CORRUPTION_SITE_RETIRED_FREE_TAIL,
                "metadata retired tail extent records corruption site");
    expect_true(heap->core.retired_free_extent_tail == 0,
                "metadata retired tail extent not silently repaired");
    heap->core.retired_free_extent_head = 0;
    heap->core.retired_free_extent_tail = 0;
    heap->core.retired_free_extent_count = 0;
    atomic_store_explicit(&heap->core.debug_last_corruption_site, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&heap->last_error, (int)UHM_OK,
                          UHM_ATOMIC_RELEASE);

    expect_ok(api->munmap(heap, reuse, page_size),
              "metadata retired tail cleanup munmap");
    expect_ok(api->check(heap, 0), "metadata retired tail final check");
    expect_ok(api->close(heap), "metadata retired tail close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_quarantine_prioritizes_emergency(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma *retired_vma;
    size_t initial_vma_available;
    uint64_t retired_epoch;
    uint64_t reclaimed_before;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "metadata priority heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "metadata priority arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata priority usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata priority heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata priority api");

    initial_vma_available = heap->core.emergency_vma_available;
    retired_epoch = heap->core.metadata_epoch;
    retired_vma = test_retire_emergency_vma_node(heap, retired_epoch);
    expect_true(retired_vma != 0, "metadata priority retire emergency vma");

    expect_true(uhm_metadata_hazard_begin(heap),
                "metadata priority hazard begin");
    for (i = 0; i < UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET + 2u; ++i) {
        void *map = 0;
        expect_ok(api->mmap(heap, &map, page_size * 4u,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
                  "metadata priority mmap normal retired source");
        expect_ok(api->mprotect(heap, (void *)((uintptr_t)map + page_size),
                                page_size * 2u, UHM_PROT_READ),
                  "metadata priority split normal retired source");
        expect_ok(api->munmap(heap, map, page_size * 4u),
                  "metadata priority munmap normal retired source");
    }
    expect_true(heap->core.retired_vma_count >
                    UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET,
                "metadata priority normal retired pressure");
    expect_true(heap->core.emergency_vma_available + 1u ==
                    initial_vma_available,
                "metadata priority emergency still retired while hazard held");
    expect_true(uhm_metadata_hazard_end(heap),
                "metadata priority hazard end");

    reclaimed_before = heap->core.metadata_quarantine_drain_reclaimed;
    {
        void *pin = api->malloc(heap, 64u);
        expect_true(pin != 0, "metadata priority trigger drain malloc");
        api->free(heap, pin);
    }
    expect_true(heap->core.metadata_quarantine_drain_reclaimed >
                    reclaimed_before,
                "metadata priority drain reclaimed emergency node");
    expect_true(heap->core.emergency_vma_available == initial_vma_available,
                "metadata priority emergency vma reclaimed before blockers");
    expect_ok(api->check(heap, 0), "metadata priority final check");
    expect_ok(api->close(heap), "metadata priority close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_hazard_defers_emergency_reuse(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma *retired_vma;
    uhm_free_extent *retired_extent;
    void *pin;
    size_t initial_vma_available;
    size_t initial_extent_available;
    uint64_t retired_epoch;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "metadata hazard heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "metadata hazard arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata hazard usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata hazard heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata hazard api");
    expect_true(heap->core.emergency_vma_head != 0 &&
                    heap->core.emergency_free_extent_head != 0,
                "metadata hazard emergency reserve present");

    initial_vma_available = heap->core.emergency_vma_available;
    initial_extent_available = heap->core.emergency_free_extent_available;
    retired_epoch = heap->core.metadata_epoch;

    retired_vma = test_retire_emergency_vma_node(heap, retired_epoch);
    expect_true(retired_vma != 0, "metadata hazard retire emergency vma");

    retired_extent =
        test_retire_emergency_free_extent_node(heap, retired_epoch);
    expect_true(retired_extent != 0,
                "metadata hazard retire emergency extent");

    expect_true(uhm_metadata_hazard_begin(heap),
                "metadata hazard reader begin");
    pin = api->malloc(heap, 64u);
    expect_true(pin != 0, "metadata hazard epoch advance malloc");
    expect_true(heap->core.metadata_epoch > retired_epoch,
                "metadata hazard epoch advanced");
    expect_true(heap->core.emergency_vma_available + 1u ==
                    initial_vma_available,
                "metadata hazard defers vma reuse");
    expect_true(heap->core.emergency_free_extent_available + 1u ==
                    initial_extent_available,
                "metadata hazard defers extent reuse");
    expect_ok(api->check(heap, 0), "metadata hazard check while held");
    expect_true(uhm_metadata_hazard_end(heap), "metadata hazard reader end");
    expect_ok(api->check(heap, 0),
              "metadata hazard check after reader before drain");

    api->free(heap, pin);
    expect_true(heap->core.emergency_vma_available == initial_vma_available,
                "metadata hazard releases vma after grace");
    expect_true(heap->core.emergency_free_extent_available ==
                    initial_extent_available,
                "metadata hazard releases extent after grace");
    expect_true(uhm_metadata_hazard_readers_load(heap) == 0,
                "metadata hazard reader count drained");
    expect_ok(api->check(heap, 0), "metadata hazard final check");
    expect_ok(api->close(heap), "metadata hazard close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_remote_drain_owner_defers_emergency_reuse(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_vma *retired_vma;
    uhm_free_extent *retired_extent;
    uhm_slab *slab;
    void *slab_pin;
    void *epoch_pin;
    size_t initial_vma_available;
    size_t initial_extent_available;
    uint64_t retired_epoch;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "remote drain epoch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "remote drain epoch arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "remote drain epoch usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "remote drain epoch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "remote drain epoch api");

    slab_pin = api->malloc(heap, 64u);
    expect_true(slab_pin != 0, "remote drain epoch slab allocation");
    slab = heap->core.slab_all_head;
    expect_true(slab != 0, "remote drain epoch slab present");

    initial_vma_available = heap->core.emergency_vma_available;
    initial_extent_available = heap->core.emergency_free_extent_available;
    retired_epoch = heap->core.metadata_epoch;

    retired_vma = test_retire_emergency_vma_node(heap, retired_epoch);
    expect_true(retired_vma != 0,
                "remote drain epoch retire emergency vma");

    retired_extent =
        test_retire_emergency_free_extent_node(heap, retired_epoch);
    expect_true(retired_extent != 0,
                "remote drain epoch retire emergency extent");

    atomic_store_explicit(&slab->remote_drain_epoch, retired_epoch,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_drain_active,
                          UHM_REMOTE_DRAIN_OWNER_ACTIVE,
                          UHM_ATOMIC_RELEASE);

    epoch_pin = api->malloc(heap, 64u);
    expect_true(epoch_pin != 0, "remote drain epoch advance malloc");
    expect_true(heap->core.metadata_epoch > retired_epoch,
                "remote drain epoch advanced");
    expect_true(heap->core.emergency_vma_available + 1u ==
                    initial_vma_available,
                "remote drain owner defers vma reuse");
    expect_true(heap->core.emergency_free_extent_available + 1u ==
                    initial_extent_available,
                "remote drain owner defers extent reuse");

    atomic_store_explicit(&slab->remote_drain_active, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_drain_epoch, 0u,
                          UHM_ATOMIC_RELEASE);
    api->free(heap, epoch_pin);
    expect_true(heap->core.emergency_vma_available == initial_vma_available,
                "remote drain owner releases vma after clear");
    expect_true(heap->core.emergency_free_extent_available ==
                    initial_extent_available,
                "remote drain owner releases extent after clear");
    api->free(heap, slab_pin);
    expect_ok(api->check(heap, 0), "remote drain epoch final check");
    expect_ok(api->close(heap), "remote drain epoch close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_ctx_reader_defers_emergency_reuse(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc ctx_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    uhm_vma *retired_vma;
    uhm_free_extent *retired_extent;
    void *pin;
    uhm_result last_error;
    size_t initial_vma_available;
    size_t initial_extent_available;
    uint64_t retired_epoch;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "metadata ctx reader heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size, ctx_info->storage_alignment,
                                    &ctx_mem) != 0,
                "metadata ctx reader ctx storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "metadata ctx reader arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata ctx reader usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata ctx reader heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata ctx reader api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "metadata ctx reader open ctx");

    initial_vma_available = heap->core.emergency_vma_available;
    initial_extent_available = heap->core.emergency_free_extent_available;
    retired_epoch = heap->core.metadata_epoch;

    retired_vma = test_retire_emergency_vma_node(heap, retired_epoch);
    expect_true(retired_vma != 0,
                "metadata ctx reader retire emergency vma");

    retired_extent =
        test_retire_emergency_free_extent_node(heap, retired_epoch);
    expect_true(retired_extent != 0,
                "metadata ctx reader retire emergency extent");

    expect_true(uhm_metadata_reader_begin(&ctx->metadata_reader, heap),
                "metadata ctx reader begin");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               retired_vma),
                "metadata ctx reader vma hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               retired_extent),
                "metadata ctx reader extent hazard");
    pin = api->malloc(heap, 64u);
    expect_true(pin != 0, "metadata ctx reader epoch advance malloc");
    expect_true(heap->core.metadata_epoch > retired_epoch,
                "metadata ctx reader epoch advanced");
    expect_true(heap->core.emergency_vma_available + 1u ==
                    initial_vma_available,
                "metadata ctx reader defers vma reuse");
    expect_true(heap->core.emergency_free_extent_available + 1u ==
                    initial_extent_available,
                "metadata ctx reader defers extent reuse");
    api->thread_ctx_close(heap, ctx);
    expect_ok(api->get_last_error(heap, &last_error, 0),
              "metadata ctx reader close busy last error query");
    expect_true(last_error == UHM_ERR_BUSY,
                "metadata ctx reader close busy last error");
    expect_true(ctx->magic == UHM_THREAD_MAGIC &&
                    heap->core.thread_ctx_head == ctx,
                "metadata ctx reader close busy keeps ctx attached");
    expect_true(api->close(heap) == UHM_ERR_BUSY,
                "metadata ctx reader heap close busy");
    expect_ok(api->check(heap, 0), "metadata ctx reader check after busy close");
    expect_ok(api->check(heap, 0), "metadata ctx reader check while held");
    expect_true(uhm_metadata_reader_end(&ctx->metadata_reader),
                "metadata ctx reader end");
    expect_ok(api->check(heap, 0),
              "metadata ctx reader check after reader before drain");

    api->free(heap, pin);
    expect_true(heap->core.emergency_vma_available == initial_vma_available,
                "metadata ctx reader releases vma after grace");
    expect_true(heap->core.emergency_free_extent_available ==
                    initial_extent_available,
                "metadata ctx reader releases extent after grace");
    expect_ok(api->check(heap, 0), "metadata ctx reader final check");
    api->thread_ctx_close(heap, ctx);
    expect_ok(api->close(heap), "metadata ctx reader close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_metadata_reader_state_diagnostics(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc ctx_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    uint64_t valid_epoch;
    void *small;
    void *map = 0;
    void *free_map = 0;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_vma *live_vma;
    uhm_free_extent *free_extent;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "metadata reader diag heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size,
                                    ctx_info->storage_alignment,
                                    &ctx_mem) != 0,
                "metadata reader diag ctx storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "metadata reader diag arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "metadata reader diag usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "metadata reader diag heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "metadata reader diag api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "metadata reader diag open ctx");
    expect_ok(api->check(heap, 0), "metadata reader diag initial check");

    atomic_store_explicit(&heap->core.metadata_published_epoch,
                          heap->core.metadata_epoch + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag future published epoch");
    atomic_store_explicit(&heap->core.metadata_published_epoch,
                          heap->core.metadata_epoch, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag published epoch restored");

    atomic_store_explicit(&heap->metadata_hazard_readers, SIZE_MAX,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag saturated conservative hazard rejected");
    atomic_store_explicit(&heap->metadata_hazard_readers, 1u,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag conservative hazard active accepted");
    atomic_store_explicit(&heap->metadata_hazard_readers, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag conservative hazard restored");

    uhm_metadata_registered_reader_update_begin(heap);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag stable registered seq active rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag registered seq active op accepted");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_metadata_registered_reader_update_end(heap);
    expect_ok(api->check(heap, 0),
              "metadata reader diag registered seq active restored");

    atomic_store_explicit(&heap->metadata_registered_readers, 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag registered mismatch stable");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag registered mismatch op only");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_metadata_registered_reader_update_begin(heap);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag registered mismatch seq only");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag registered mismatch seq with op");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_metadata_registered_reader_update_end(heap);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag registered mismatch after seq");
    atomic_store_explicit(&heap->metadata_registered_readers, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag registered mismatch restored");

    atomic_store_explicit(&ctx->metadata_reader.epoch,
                          heap->core.metadata_epoch, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag inactive dirty epoch");
    atomic_store_explicit(&ctx->metadata_reader.epoch, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag inactive epoch restored");

    atomic_store_explicit(&ctx->metadata_reader.hazards[0],
                          (uintptr_t)ctx, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag inactive dirty hazard");
    atomic_store_explicit(&ctx->metadata_reader.hazards[0],
                          (uintptr_t)0, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag inactive hazard restored");
    {
        size_t readers_before = uhm_metadata_registered_readers_load(heap);
        uint64_t seq_before = uhm_metadata_registered_reader_seq_load(heap);

        expect_true(uhm_metadata_reader_begin_unregistered(
                        &ctx->metadata_reader, heap),
                    "metadata reader diag begin unregistered active reader");
        expect_true(uhm_metadata_registered_readers_load(heap) ==
                        readers_before,
                    "metadata reader diag unregistered count unchanged");
        expect_true(uhm_metadata_registered_reader_seq_load(heap) ==
                        seq_before,
                    "metadata reader diag unregistered seq unchanged");
        expect_ok(api->check(heap, 0),
                  "metadata reader diag unregistered active check");
        expect_true(uhm_metadata_reader_end(&ctx->metadata_reader),
                    "metadata reader diag end unregistered active reader");
        expect_true(uhm_metadata_registered_readers_load(heap) ==
                        readers_before,
                    "metadata reader diag unregistered end count unchanged");
        expect_true(uhm_metadata_registered_reader_seq_load(heap) ==
                        seq_before,
                    "metadata reader diag unregistered end seq unchanged");
        expect_ok(api->check(heap, 0),
                  "metadata reader diag unregistered restored");
    }

    expect_ok(api->mmap(heap, &map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "metadata reader diag live vma mmap");
    live_vma = test_find_vma_containing(heap, map);
    expect_true(live_vma != 0 && live_vma->magic == UHM_VMA_MAGIC,
                "metadata reader diag live vma found");
    expect_ok(api->mmap(heap, &free_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "metadata reader diag free extent mmap");
    expect_ok(api->munmap(heap, free_map, page_size),
              "metadata reader diag free extent munmap");
    free_extent = test_find_free_extent_containing(heap, free_map);
    expect_true(free_extent != 0 && free_extent->magic == UHM_FREE_MAGIC,
                "metadata reader diag free extent found");

    expect_true(uhm_metadata_reader_begin(&ctx->metadata_reader, heap),
                "metadata reader diag begin active reader");
    valid_epoch = atomic_load_explicit(&ctx->metadata_reader.epoch,
                                       UHM_ATOMIC_ACQUIRE);
    atomic_store_explicit(&ctx->metadata_reader.epoch,
                          heap->core.metadata_epoch + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag future active epoch");
    atomic_store_explicit(&ctx->metadata_reader.epoch, valid_epoch,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "metadata reader diag active epoch restored");

    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               &live_vma->tree_left),
                "metadata reader diag set vma tree hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               &live_vma->desc),
                "metadata reader diag set vma desc hazard");
    expect_ok(api->check(heap, 0),
              "metadata reader diag vma embedded hazards known");
    expect_true(uhm_metadata_reader_hazard_set(
                    &ctx->metadata_reader, 0u,
                    (void *)((uintptr_t)&live_vma->tree_left + 1u)),
                "metadata reader diag set misaligned vma tree hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag misaligned vma tree hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               &free_extent->tree_left),
                "metadata reader diag set free extent tree hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               &free_extent->desc),
                "metadata reader diag set free extent desc hazard");
    expect_ok(api->check(heap, 0),
              "metadata reader diag free extent embedded hazards known");
    expect_true(uhm_metadata_reader_hazard_set(
                    &ctx->metadata_reader, 0u,
                    (void *)((uintptr_t)&free_extent->tree_left + 1u)),
                "metadata reader diag set misaligned free extent tree hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag misaligned free extent tree hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               0),
                "metadata reader diag clear embedded hazard 0");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               0),
                "metadata reader diag clear embedded hazard 1");
    expect_ok(api->check(heap, 0),
              "metadata reader diag embedded hazards restored");

    small = api->malloc(heap, 64u);
    expect_true(small != 0, "metadata reader diag slab hazard allocation");
    slot = test_slab_slot_from_payload(small);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "metadata reader diag slab hazard metadata");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               slab),
                "metadata reader diag set slab hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               slot),
                "metadata reader diag set slot hazard");
    expect_ok(api->check(heap, 0),
              "metadata reader diag slab slot hazards known");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               &slab->next_all),
                "metadata reader diag set slab all-link hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               &slab->desc),
                "metadata reader diag set slab desc hazard");
    expect_ok(api->check(heap, 0),
              "metadata reader diag slab embedded hazards known");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               &slab->remote_ready_link),
                "metadata reader diag set ready link hazard");
    expect_ok(api->check(heap, 0),
              "metadata reader diag ready link hazard known");
    expect_true(uhm_metadata_reader_hazard_set(
                    &ctx->metadata_reader, 0u,
                    (void *)((uintptr_t)&slab->remote_ready_link + 1u)),
                "metadata reader diag set misaligned ready link hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag misaligned ready link hazard");
    expect_true(uhm_metadata_reader_hazard_set(
                    &ctx->metadata_reader, 0u,
                    (void *)((uintptr_t)&slab->desc + 1u)),
                "metadata reader diag set misaligned slab desc hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag misaligned slab desc hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               slab),
                "metadata reader diag restore slab hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               slot),
                "metadata reader diag restore slot hazard before link");
    expect_ok(api->check(heap, 0),
              "metadata reader diag slab embedded hazards restored");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               &slot->remote_link),
                "metadata reader diag set remote link hazard");
    expect_ok(api->check(heap, 0),
              "metadata reader diag remote link hazard known");
    expect_true(uhm_metadata_reader_hazard_set(
                    &ctx->metadata_reader, 1u,
                    (void *)((uintptr_t)&slot->remote_link + 1u)),
                "metadata reader diag set misaligned remote link hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag misaligned remote link hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               slot),
                "metadata reader diag restore slot after remote link hazard");
    expect_ok(api->check(heap, 0),
              "metadata reader diag restored after remote link hazard");
    expect_true(uhm_metadata_reader_hazard_set(
                    &ctx->metadata_reader, 1u,
                    (void *)((uintptr_t)slot + 1u)),
                "metadata reader diag set misaligned slot hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag misaligned slot hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               slot),
                "metadata reader diag restore slot hazard");
    expect_ok(api->check(heap, 0),
              "metadata reader diag restored slot hazard");

    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               ctx),
                "metadata reader diag set unknown hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "metadata reader diag unknown active hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 0u,
                                               0),
                "metadata reader diag clear unknown hazard");
    expect_true(uhm_metadata_reader_hazard_set(&ctx->metadata_reader, 1u,
                                               0),
                "metadata reader diag clear slot hazard");
    expect_true(uhm_metadata_reader_end(&ctx->metadata_reader),
                "metadata reader diag end active reader");
    api->free(heap, small);
    expect_ok(api->munmap(heap, map, page_size),
              "metadata reader diag munmap live vma");
    expect_ok(api->check(heap, 0), "metadata reader diag final check");

    api->thread_ctx_close(heap, ctx);
    expect_ok(api->close(heap), "metadata reader diag close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&heap_mem);
}

