static int test_control_extent_hint_position_matches(
    uhm_heap *heap, const uhm_control_extent *extent) {
    size_t bucket;
    size_t slot;

    if (!heap || !extent ||
        (extent->flags & UHM_CONTROL_EXTENT_FLAG_HINTED) == 0) {
        return 0;
    }
    bucket = (size_t)((extent->flags & UHM_CONTROL_EXTENT_HINT_BUCKET_MASK) >>
                      UHM_CONTROL_EXTENT_HINT_BUCKET_SHIFT);
    slot = (size_t)((extent->flags & UHM_CONTROL_EXTENT_HINT_SLOT_MASK) >>
                    UHM_CONTROL_EXTENT_HINT_SLOT_SHIFT);
    return bucket < UHM_CONTROL_ALLOC_HINT_BUCKETS &&
           slot < UHM_CONTROL_ALLOC_HINT_SLOTS &&
           heap->core.control_alloc_hint[bucket][slot] == extent;
}

static void test_collision_exhaustion(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 160u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *malloc_block;
    void *map = 0;
    void *failed_map = 0;
    void *failed_malloc;
    uhm_stats stats;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment, &heap_mem) != 0,
                "collision heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "collision arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "collision usermode vm init");

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
    config.control_band_min_size = 32u * 1024u;

    expect_ok(uhm_open(&config, &heap), "collision heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "collision api");

    malloc_block = api->malloc(heap, 80u * 1024u);
    expect_true(malloc_block != 0, "collision malloc block");
    expect_ok(api->mmap(heap, &map, 40u * 1024u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "collision mmap block");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "collision stats");
    expect_true(stats.collision_free_bytes > 0 && stats.collision_free_bytes < page_size * 2u,
                "collision gap is tight");

    expect_true(api->mmap(heap, &failed_map, page_size * 2u,
                          UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE) ==
                    UHM_ERR_OUT_OF_ADDRESS_SPACE,
                "collision mmap returns OAS");
    failed_malloc = api->malloc(heap, page_size * 2u);
    expect_true(failed_malloc == 0, "collision malloc returns null");
    expect_ok(api->check(heap, 0), "collision check after failures");

    expect_ok(api->munmap(heap, map, 40u * 1024u), "collision cleanup munmap");
    api->free(heap, malloc_block);
    expect_ok(api->check(heap, 0), "collision final check");
    expect_ok(api->close(heap), "collision close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_malloc_mmap_bidirectional_collision_pressure(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 512u * 1024u;
    enum { max_blocks = 96 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *malloc_blocks[max_blocks];
    size_t malloc_sizes[max_blocks];
    void *maps[max_blocks];
    size_t malloc_count = 0;
    size_t map_count = 0;
    void *failed_malloc;
    void *failed_map = 0;
    uhm_result map_result;
    uhm_stats stats;
    size_t i;

    memset(malloc_blocks, 0, sizeof(malloc_blocks));
    memset(malloc_sizes, 0, sizeof(malloc_sizes));
    memset(maps, 0, sizeof(maps));

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "bidirectional collision heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "bidirectional collision arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "bidirectional collision usermode vm init");

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
    config.control_band_min_size = 64u * 1024u;

    expect_ok(uhm_open(&config, &heap), "bidirectional collision heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "bidirectional collision api");

    for (i = 0; i < max_blocks; ++i) {
        size_t malloc_size = 6144u + (i % 5u) * 1024u;
        malloc_blocks[malloc_count] = api->malloc(heap, malloc_size);
        if (!malloc_blocks[malloc_count]) {
            break;
        }
        malloc_sizes[malloc_count] = malloc_size;
        test_fill_bytes(malloc_blocks[malloc_count], malloc_size,
                        0xb0u + (unsigned)(malloc_count & 15u));
        ++malloc_count;

        map_result = api->mmap(heap, &maps[map_count], page_size,
                               UHM_PROT_READ | UHM_PROT_WRITE,
                               UHM_MAP_PRIVATE);
        if (map_result != UHM_OK) {
            break;
        }
        ++map_count;
    }

    expect_true(malloc_count >= 20u, "bidirectional collision malloc pressure depth");
    expect_true(map_count >= 20u, "bidirectional collision mmap pressure depth");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "bidirectional collision tight stats");
    expect_true(stats.collision_free_bytes < 16u * page_size,
                "bidirectional collision gap is tight");

    failed_malloc = api->malloc(heap, 32u * 1024u);
    expect_true(failed_malloc == 0, "bidirectional collision large malloc fails");
    map_result = api->mmap(heap, &failed_map, 32u * 1024u,
                           UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE);
    expect_true(map_result == UHM_ERR_OUT_OF_ADDRESS_SPACE ||
                    map_result == UHM_ERR_OUT_OF_MEMORY,
                "bidirectional collision large mmap fails");
    expect_ok(api->check(heap, 0), "bidirectional collision check after pressure");

    for (i = 0; i < malloc_count; i += 2u) {
        expect_true(test_bytes_equal(malloc_blocks[i], malloc_sizes[i],
                                     0xb0u + (unsigned)(i & 15u)),
                    "bidirectional collision malloc bytes");
        api->free(heap, malloc_blocks[i]);
        malloc_blocks[i] = 0;
    }
    for (i = 0; i < map_count; i += 2u) {
        expect_ok(api->munmap(heap, maps[i], page_size),
                  "bidirectional collision partial munmap");
        maps[i] = 0;
    }

    failed_malloc = api->malloc(heap, page_size);
    expect_true(failed_malloc != 0, "bidirectional collision malloc reuses freed space");
    api->free(heap, failed_malloc);
    failed_map = 0;
    expect_ok(api->mmap(heap, &failed_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "bidirectional collision mmap reuses freed extent");
    expect_ok(api->munmap(heap, failed_map, page_size),
              "bidirectional collision reuse munmap");

    for (i = 0; i < malloc_count; ++i) {
        if (malloc_blocks[i]) {
            api->free(heap, malloc_blocks[i]);
        }
    }
    for (i = 0; i < map_count; ++i) {
        if (maps[i]) {
            expect_ok(api->munmap(heap, maps[i], page_size),
                      "bidirectional collision cleanup munmap");
        }
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "bidirectional collision final stats");
    expect_true(stats.malloc_live_bytes == 0, "bidirectional collision malloc cleanup");
    expect_true(stats.mmap_live_bytes == 0, "bidirectional collision mmap cleanup");
    expect_ok(api->check(heap, 0), "bidirectional collision final check");
    expect_ok(api->close(heap), "bidirectional collision close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_near_address_space_exhaustion_long_run(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    enum { max_blocks = 128, rounds = 64 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *malloc_blocks[max_blocks];
    size_t malloc_sizes[max_blocks];
    void *maps[max_blocks];
    size_t map_sizes[max_blocks];
    unsigned round;
    uhm_stats stats;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "near exhaustion heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "near exhaustion arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "near exhaustion usermode vm init");

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
    config.control_band_min_size = 256u * 1024u;

    for (round = 0; round < rounds; ++round) {
        size_t malloc_count = 0;
        size_t map_count = 0;
        int malloc_exhausted = 0;
        int mmap_exhausted = 0;
        size_t i;
        void *reuse_malloc;
        void *reuse_map = 0;
        void *failed_malloc;
        void *failed_map = 0;
        uhm_result map_result;

        memset(malloc_blocks, 0, sizeof(malloc_blocks));
        memset(malloc_sizes, 0, sizeof(malloc_sizes));
        memset(maps, 0, sizeof(maps));
        memset(map_sizes, 0, sizeof(map_sizes));
        heap = 0;
        expect_ok(uhm_open(&config, &heap), "near exhaustion heap open");
        api = uhm_get_api(heap);
        expect_true(api != 0, "near exhaustion api");

        for (i = 0; i < max_blocks && (!malloc_exhausted || !mmap_exhausted); ++i) {
            if (!malloc_exhausted && malloc_count < max_blocks) {
                size_t size = 2048u + (((i + round) % 7u) * 1024u);
                void *ptr = api->malloc(heap, size);
                if (ptr) {
                    malloc_blocks[malloc_count] = ptr;
                    malloc_sizes[malloc_count] = size;
                    test_fill_bytes(ptr, size,
                                    0x70u + (unsigned)((round + i) & 15u));
                    ++malloc_count;
                } else {
                    malloc_exhausted = 1;
                }
            }
            if (!mmap_exhausted && map_count < max_blocks) {
                size_t size = page_size * (1u + ((i + round) % 2u));
                map_result = api->mmap(heap, &maps[map_count], size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE);
                if (map_result == UHM_OK) {
                    map_sizes[map_count] = size;
                    ++map_count;
                } else {
                    expect_true(map_result == UHM_ERR_OUT_OF_ADDRESS_SPACE ||
                                    map_result == UHM_ERR_OUT_OF_MEMORY,
                                "near exhaustion mmap fails only by exhaustion");
                    mmap_exhausted = 1;
                }
            }
        }

        expect_true(malloc_count >= 24u, "near exhaustion malloc depth");
        expect_true(map_count >= 8u, "near exhaustion mmap depth");
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(api->get_stats(heap, &stats), "near exhaustion tight stats");
        expect_true(stats.collision_free_bytes < 192u * 1024u,
                    "near exhaustion collision gap stays small");
        failed_malloc = api->malloc(heap, 256u * 1024u);
        expect_true(failed_malloc == 0, "near exhaustion large malloc fails");
        map_result = api->mmap(heap, &failed_map, 256u * 1024u,
                               UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE);
        expect_true(map_result == UHM_ERR_OUT_OF_ADDRESS_SPACE ||
                        map_result == UHM_ERR_OUT_OF_MEMORY,
                    "near exhaustion large mmap fails");
        expect_ok(api->check(heap, 0), "near exhaustion check while tight");

        for (i = 0; i < malloc_count; i += 3u) {
            expect_true(test_bytes_equal(malloc_blocks[i], malloc_sizes[i],
                                         0x70u + (unsigned)((round + i) & 15u)),
                        "near exhaustion malloc bytes");
            api->free(heap, malloc_blocks[i]);
            malloc_blocks[i] = 0;
        }
        for (i = 0; i < map_count; i += 3u) {
            expect_ok(api->munmap(heap, maps[i], map_sizes[i]),
                      "near exhaustion partial munmap");
            maps[i] = 0;
            map_sizes[i] = 0;
        }

        reuse_malloc = api->malloc(heap, 2048u + (round % 5u) * 256u);
        expect_true(reuse_malloc != 0, "near exhaustion malloc reuses freed space");
        api->free(heap, reuse_malloc);
        map_result = api->mmap(heap, &reuse_map, page_size,
                               UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE);
        expect_ok(map_result, "near exhaustion mmap reuses freed extent");
        expect_ok(api->munmap(heap, reuse_map, page_size),
                  "near exhaustion reuse munmap");

        for (i = 0; i < malloc_count; ++i) {
            if (malloc_blocks[i]) {
                api->free(heap, malloc_blocks[i]);
            }
        }
        for (i = 0; i < map_count; ++i) {
            if (maps[i]) {
                expect_ok(api->munmap(heap, maps[i], map_sizes[i]),
                          "near exhaustion cleanup munmap");
            }
        }

        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(api->get_stats(heap, &stats), "near exhaustion final stats");
        expect_true(stats.malloc_live_bytes == 0,
                    "near exhaustion malloc cleanup");
        expect_true(stats.mmap_live_bytes == 0, "near exhaustion mmap cleanup");
        expect_ok(api->check(heap, 0), "near exhaustion round final check");
        expect_ok(api->close(heap), "near exhaustion round close");
    }

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_control_extent_expansion(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t control_size = 16u * 1024u;
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *map2 = 0;
    void *map3 = 0;
    void *map4 = 0;
    void *map5 = 0;
    void *map6 = 0;
    uhm_stats stats;
    void *first_control_extent_page;
    uhm_control_extent *saved_recent_extent;
    uhm_control_extent *saved_hint_extent;
    uhm_control_extent *no_fit_hint_extent;
    uint32_t saved_hint_flags;
    uint64_t scan_calls_before;
    uint64_t scan_steps_before;
    uint64_t hint_hits_before;
    uint64_t hint_stale_before;
    uint64_t vm_commit_calls_before;
    uint64_t vm_commit_lock_held_before;
    uhm_control_extent *stable_hint_snapshot[UHM_CONTROL_ALLOC_HINT_SLOTS];
    uint32_t stable_hint_flags;
    size_t stable_hint_bucket;
    size_t stable_hint_slot;
    size_t stable_hint_index;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "control extent heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "control extent arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "control extent usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "control extent heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "control extent api");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "control extent initial stats");
    expect_true(stats.control_bytes == control_size, "control extent starts with small band");

    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    expect_ok(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "control extent trigger mmap");
    expect_true(map != 0, "control extent mmap address");
    expect_true(heap->core.vm_commit_calls > vm_commit_calls_before,
                "control extent commit callback counted");
    expect_true(heap->core.vm_commit_lock_held_calls ==
                    vm_commit_lock_held_before,
                "control extent commit not counted lock-held");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "control extent expanded stats");
    expect_true(stats.control_bytes > control_size, "control extent carved from collision zone");
    expect_true(heap->core.control_extents != 0 &&
                    heap->core.control_recent_extent == heap->core.control_extents,
                "control extent recent cursor tracks new extent");

    first_control_extent_page = (void *)((uintptr_t)arena_mem.aligned + control_size);
    expect_true(api->mprotect(heap, first_control_extent_page, page_size, UHM_PROT_READ) ==
                    UHM_ERR_PROTECTION,
                "secondary control extent mprotect rejected");

    scan_calls_before = heap->core.control_alloc_scan_calls;
    scan_steps_before = heap->core.control_alloc_scan_steps;
    heap->core.control_alloc = heap->core.control_end;
    heap->core.control_recent_extent = 0;
    memset(heap->core.control_alloc_hint, 0,
           sizeof(heap->core.control_alloc_hint));
    expect_ok(api->mmap(heap, &map2, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "control extent scan mmap");
    expect_true(map2 != 0, "control extent scan mmap address");
    expect_true(heap->core.control_alloc_scan_calls > scan_calls_before,
                "control extent scan call counted");
    expect_true(heap->core.control_alloc_scan_steps > scan_steps_before,
                "control extent scan steps counted");
    expect_true(heap->core.control_alloc_scan_max_steps != 0,
                "control extent scan max counted");
    expect_true(heap->core.control_recent_extent == heap->core.control_extents,
                "control extent scan restores recent cursor");

    scan_calls_before = heap->core.control_alloc_scan_calls;
    hint_hits_before = heap->core.control_alloc_hint_hits;
    heap->core.control_recent_extent = 0;
    expect_ok(api->mmap(heap, &map3, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "control extent hint mmap");
    expect_true(map3 != 0, "control extent hint mmap address");
    expect_true(heap->core.control_alloc_hint_hits > hint_hits_before,
                "control extent hint hit counted");
    expect_true(heap->core.control_alloc_scan_calls == scan_calls_before,
                "control extent hint avoids scan");
    expect_true(heap->core.control_recent_extent == heap->core.control_extents,
                "control extent hint restores recent cursor");
    expect_true(test_control_extent_hint_position_matches(
                     heap, heap->core.control_extents),
                 "control extent hint position tracks hint hit");

    stable_hint_flags = heap->core.control_extents->flags;
    stable_hint_bucket = (size_t)((stable_hint_flags &
                                   UHM_CONTROL_EXTENT_HINT_BUCKET_MASK) >>
                                  UHM_CONTROL_EXTENT_HINT_BUCKET_SHIFT);
    stable_hint_slot = 0;
    for (stable_hint_index = 0;
         stable_hint_index < UHM_CONTROL_ALLOC_HINT_SLOTS;
         ++stable_hint_index) {
        stable_hint_snapshot[stable_hint_index] =
            heap->core.control_alloc_hint[stable_hint_bucket]
                                         [stable_hint_index];
    }
    expect_true(uhm_test_control_alloc_hint_note(
                    heap, heap->core.control_extents) == 1,
                "control extent stable MRU hint avoids requeue");
    expect_true(heap->core.control_extents->flags == stable_hint_flags &&
                    heap->core.control_alloc_hint[stable_hint_bucket]
                                                   [stable_hint_slot] ==
                        heap->core.control_extents,
                "control extent stable MRU hint keeps position");
    for (stable_hint_index = 0;
         stable_hint_index < UHM_CONTROL_ALLOC_HINT_SLOTS;
         ++stable_hint_index) {
        expect_true(heap->core.control_alloc_hint[stable_hint_bucket]
                        [stable_hint_index] ==
                        stable_hint_snapshot[stable_hint_index],
                    "control extent stable MRU hint keeps window");
    }

    scan_calls_before = heap->core.control_alloc_scan_calls;
    hint_hits_before = heap->core.control_alloc_hint_hits;
    hint_stale_before = heap->core.control_alloc_hint_stale;
    heap->core.control_recent_extent = 0;
    memset(heap->core.control_alloc_hint, 0,
           sizeof(heap->core.control_alloc_hint));
    heap->core.control_alloc_hint[UHM_CONTROL_ALLOC_HINT_BUCKETS - 1u][0] =
        (uhm_control_extent *)heap;
    heap->core.control_alloc_hint[UHM_CONTROL_ALLOC_HINT_BUCKETS - 1u][1] =
        heap->core.control_extents;
    expect_ok(api->mmap(heap, &map4, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "control extent multi hint mmap");
    expect_true(map4 != 0, "control extent multi hint mmap address");
    expect_true(heap->core.control_alloc_hint_stale == hint_stale_before + 1u,
                "control extent multi hint stale removed");
    expect_true(heap->core.control_alloc_hint_hits > hint_hits_before,
                "control extent multi hint second slot hit counted");
    expect_true(heap->core.control_alloc_scan_calls == scan_calls_before,
                "control extent multi hint avoids scan");
    expect_true(heap->core.control_recent_extent == heap->core.control_extents,
                "control extent multi hint restores recent cursor");
    expect_true(test_control_extent_hint_position_matches(
                    heap, heap->core.control_extents),
                "control extent multi hint refreshes position");
    saved_hint_flags = heap->core.control_extents->flags;
    heap->core.control_extents->flags &=
        (uint32_t)~UHM_CONTROL_EXTENT_FLAG_HINTED;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "control extent missing hinted flag rejected");
    heap->core.control_extents->flags = saved_hint_flags;
    expect_ok(api->check(heap, 0), "control extent restored hint flag check");

    heap->core.control_alloc = heap->core.control_end;
    heap->core.control_recent_extent = heap->core.control_extents;
    heap->core.control_recent_extent->alloc =
        heap->core.control_recent_extent->end;
    memset(heap->core.control_alloc_hint, 0,
           sizeof(heap->core.control_alloc_hint));
    heap->core.control_extents->flags &= ~UHM_CONTROL_EXTENT_HINT_MASK;
    scan_calls_before = heap->core.control_alloc_scan_calls;
    expect_ok(api->mmap(heap, &map5, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "control extent no-fit first mmap");
    expect_true(map5 != 0, "control extent no-fit first mmap address");
    expect_true(heap->core.control_alloc_scan_calls > scan_calls_before,
                "control extent no-fit first mmap scans old extents");
    expect_true(heap->core.control_no_fit_extent_count + 1u ==
                    heap->core.control_extent_count,
                "control extent no-fit memo covers old extents");

    no_fit_hint_extent = heap->core.control_extents->next;
    expect_true(no_fit_hint_extent != 0 &&
                    no_fit_hint_extent->magic == UHM_CONTROL_MAGIC,
                "control extent no-fit hint extent");
    no_fit_hint_extent->alloc = no_fit_hint_extent->end;
    no_fit_hint_extent->flags &=
        (uint32_t)~UHM_CONTROL_EXTENT_HINT_MASK;
    no_fit_hint_extent->flags |= UHM_CONTROL_EXTENT_FLAG_HINTED;
    no_fit_hint_extent->flags |=
        ((uint32_t)(UHM_CONTROL_ALLOC_HINT_BUCKETS - 1u) &
         UHM_CONTROL_EXTENT_HINT_INDEX_MASK)
        << UHM_CONTROL_EXTENT_HINT_BUCKET_SHIFT;
    no_fit_hint_extent->flags |=
        ((uint32_t)0u & UHM_CONTROL_EXTENT_HINT_INDEX_MASK)
        << UHM_CONTROL_EXTENT_HINT_SLOT_SHIFT;
    heap->core.control_alloc_hint[UHM_CONTROL_ALLOC_HINT_BUCKETS - 1u][0] =
        no_fit_hint_extent;
    hint_hits_before = heap->core.control_alloc_hint_hits;
    hint_stale_before = heap->core.control_alloc_hint_stale;
    scan_calls_before = heap->core.control_alloc_scan_calls;
    {
        uint64_t no_fit_fast_paths_before =
            heap->core.control_alloc_no_fit_fast_paths;

        heap->core.control_recent_extent = heap->core.control_extents;
        heap->core.control_recent_extent->alloc =
            heap->core.control_recent_extent->end;
        expect_ok(api->mmap(heap, &map6, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
                  "control extent early no-fit memo mmap");
        expect_true(map6 != 0, "control extent early no-fit memo address");
        expect_true(heap->core.control_alloc_no_fit_fast_paths ==
                        no_fit_fast_paths_before + 1u,
                    "control extent early no-fit fast path counted");
    }
    expect_true(heap->core.control_alloc_hint_hits == hint_hits_before,
                "control extent early no-fit skips hint hit");
    expect_true(heap->core.control_alloc_hint_stale == hint_stale_before,
                "control extent early no-fit skips stale hint cleanup");
    expect_true(heap->core.control_alloc_scan_calls == scan_calls_before,
                "control extent early no-fit skips old extent scan");
    expect_true(test_control_extent_hint_position_matches(
                    heap, no_fit_hint_extent),
                "control extent early no-fit preserves hint position");

    expect_ok(api->munmap(heap, map6, page_size),
              "control extent early no-fit memo cleanup munmap");

    /* The earlier memo test remains a separate path: after the new extent
       created above, its no-fit proof must continue to advance coverage. */
    no_fit_hint_extent->flags &= (uint32_t)~UHM_CONTROL_EXTENT_HINT_MASK;
    heap->core.control_recent_extent = heap->core.control_extents;
    heap->core.control_recent_extent->alloc =
        heap->core.control_recent_extent->end;
    memset(heap->core.control_alloc_hint, 0,
           sizeof(heap->core.control_alloc_hint));
    heap->core.control_extents->flags &= ~UHM_CONTROL_EXTENT_HINT_MASK;
    scan_calls_before = heap->core.control_alloc_scan_calls;
    expect_ok(api->mmap(heap, &map6, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "control extent no-fit memo mmap");
    expect_true(map6 != 0, "control extent no-fit memo mmap address");
    expect_true(heap->core.control_alloc_scan_calls == scan_calls_before,
                "control extent no-fit memo avoids old extent scan");
    expect_true(heap->core.control_no_fit_extent_count + 1u ==
                    heap->core.control_extent_count,
                "control extent no-fit memo advances coverage");

    expect_ok(api->munmap(heap, map6, page_size),
              "control extent no-fit memo cleanup munmap");
    expect_ok(api->munmap(heap, map5, page_size),
              "control extent no-fit first cleanup munmap");
    expect_ok(api->munmap(heap, map4, page_size),
              "control extent multi hint cleanup munmap");
    expect_ok(api->munmap(heap, map3, page_size),
              "control extent hint cleanup munmap");
    expect_ok(api->munmap(heap, map2, page_size),
              "control extent scan cleanup munmap");
    expect_ok(api->munmap(heap, map, page_size), "control extent cleanup munmap");
    saved_recent_extent = heap->core.control_recent_extent;
    heap->core.control_recent_extent = (uhm_control_extent *)heap;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "control extent invalid recent cursor rejected");
    heap->core.control_recent_extent = saved_recent_extent;
    expect_ok(api->check(heap, 0), "control extent check");
    saved_hint_extent = heap->core.control_alloc_hint[0][0];
    heap->core.control_alloc_hint[0][0] = (uhm_control_extent *)heap;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "control extent invalid hint rejected");
    heap->core.control_alloc_hint[0][0] = saved_hint_extent;
    expect_ok(api->check(heap, 0), "control extent restored hint check");
    expect_ok(api->close(heap), "control extent close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_control_extent_concurrent_competition(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t control_size = 24u * 1024u;
    const size_t arena_size = 8u * 1024u * 1024u;
    const size_t pressure_pages = 192u;
    enum { thread_count = 4 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *pressure_map = 0;
    control_compete_args args[thread_count];
    uhm_stats before;
    uhm_stats after;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "control compete heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "control compete arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "control compete usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "control compete heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "control compete api");

    expect_ok(api->mmap(heap, &pressure_map, pressure_pages * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT),
              "control compete pressure mmap");

    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before), "control compete stats before");

    memset(args, 0, sizeof(args));
    for (i = 0; i < thread_count; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].pressure_map = pressure_map;
        args[i].page_size = page_size;
        args[i].pressure_pages = pressure_pages;
        args[i].role = i < 2u ? (unsigned)i : 2u;
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, control_compete_worker, &args[i], 0, 0);
            expect_true(threads[i] != 0, "control compete create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                        "control compete wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, control_compete_worker, &args[i]) == 0,
                        "control compete create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0, "control compete join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        if (!args[i].ok) {
            fprintf(stderr,
                    "control compete worker failed: role=%u op=%s iter=%zu result=%d\n",
                    args[i].role, args[i].fail_op ? args[i].fail_op : "(unknown)",
                    args[i].fail_iter, (int)args[i].fail_result);
        }
        expect_true(args[i].ok, "control compete worker completed");
    }

    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after), "control compete stats after");
    expect_true(after.control_bytes > before.control_bytes,
                "control compete carved control extent during pressure");

    expect_ok(api->munmap(heap, pressure_map, pressure_pages * page_size),
              "control compete pressure cleanup");
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after), "control compete final stats");
    expect_true(after.malloc_live_bytes == 0, "control compete malloc cleanup");
    expect_true(after.mmap_live_bytes == 0, "control compete mmap cleanup");
    expect_ok(api->check(heap, 0), "control compete heap check");
    expect_ok(api->close(heap), "control compete close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_page_map_leaf_concurrent_creation(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t leaf_pages = 256u;
    const size_t arena_size = 16u * 1024u * 1024u;
    enum { thread_count = 6 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    page_leaf_churn_args args[thread_count];
    uhm_stats stats;
    size_t nonempty_shards = 0;
    size_t leaf_count = 0;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "page leaf heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "page leaf arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "page leaf usermode vm init");

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
    config.control_band_min_size = 64u * 1024u;

    expect_ok(uhm_open(&config, &heap), "page leaf heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "page leaf api");

    memset(args, 0, sizeof(args));
    for (i = 0; i < thread_count; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].arena_base = (uintptr_t)arena_mem.aligned;
        args[i].page_size = page_size;
        args[i].leaf_pages = leaf_pages;
        args[i].leaf_id = 1u + (unsigned)i * 2u;
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, page_leaf_churn_worker, &args[i], 0, 0);
            expect_true(threads[i] != 0, "page leaf create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                        "page leaf wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, page_leaf_churn_worker, &args[i]) == 0,
                        "page leaf create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0, "page leaf join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        expect_true(args[i].ok, "page leaf worker completed");
    }
    for (i = 0; i < UHM_PAGE_MAP_SHARDS; ++i) {
        uhm_page_leaf *leaf = heap->core.page_map[i];
        if (leaf) {
            ++nonempty_shards;
        }
        while (leaf) {
            ++leaf_count;
            if (leaf->next) {
                expect_true(leaf->base_index < leaf->next->base_index,
                            "page leaf shard remains ordered");
            }
            leaf = leaf->next;
        }
    }
    expect_true(nonempty_shards >= thread_count,
                "page leaf map distributes leaves across shards");
    expect_true(leaf_count >= thread_count,
                "page leaf map creates lazy leaves");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "page leaf final stats");
    expect_true(stats.mmap_live_bytes == 0, "page leaf mmap cleanup");
    expect_ok(api->check(heap, 0), "page leaf heap check");
    expect_ok(api->close(heap), "page leaf close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_page_map_leaf_single_shard_hotspot(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t leaf_pages = UHM_PAGE_LEAF_PAGES;
    enum { thread_count = 4 };
    const size_t highest_leaf =
        1u + ((size_t)thread_count - 1u) * UHM_PAGE_MAP_SHARDS;
    const size_t arena_size = (highest_leaf + 2u) * leaf_pages * page_size;
    const size_t target_shard = 1u % UHM_PAGE_MAP_SHARDS;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    page_leaf_churn_args args[thread_count];
    size_t target_leaf_count = 0;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "page leaf hotspot heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "page leaf hotspot arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "page leaf hotspot usermode vm init");

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
    config.control_band_min_size = 64u * 1024u;

    expect_ok(uhm_open(&config, &heap), "page leaf hotspot heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "page leaf hotspot api");

    memset(args, 0, sizeof(args));
    for (i = 0; i < thread_count; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].arena_base = (uintptr_t)arena_mem.aligned;
        args[i].page_size = page_size;
        args[i].leaf_pages = leaf_pages;
        args[i].leaf_id = 1u + (unsigned)(i * UHM_PAGE_MAP_SHARDS);
        expect_true((args[i].leaf_id % UHM_PAGE_MAP_SHARDS) == target_shard,
                    "page leaf hotspot targets one shard");
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, page_leaf_churn_worker, &args[i], 0, 0);
            expect_true(threads[i] != 0, "page leaf hotspot create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                        "page leaf hotspot wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, page_leaf_churn_worker, &args[i]) == 0,
                        "page leaf hotspot create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0,
                        "page leaf hotspot join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        expect_true(args[i].ok, "page leaf hotspot worker completed");
    }

    {
        uhm_page_leaf *leaf = heap->core.page_map[target_shard];
        size_t expected;
        while (leaf) {
            if (leaf->next) {
                expect_true(leaf->base_index < leaf->next->base_index,
                            "page leaf hotspot shard remains ordered");
            }
            for (expected = 0; expected < thread_count; ++expected) {
                size_t leaf_id = 1u + expected * UHM_PAGE_MAP_SHARDS;
                if (leaf->base_index == leaf_id * leaf_pages) {
                    ++target_leaf_count;
                    break;
                }
            }
            leaf = leaf->next;
        }
    }
    expect_true(target_leaf_count >= thread_count,
                "page leaf hotspot creates all target leaves");
    expect_ok(api->check(heap, 0), "page leaf hotspot heap check");
    expect_ok(api->close(heap), "page leaf hotspot close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_page_map_leaf_spare_reuse(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t leaf_pages = UHM_PAGE_LEAF_PAGES;
    const size_t first_leaf_id = 1u;
    const size_t second_leaf_id = first_leaf_id + UHM_PAGE_MAP_SHARDS;
    const size_t target_shard = first_leaf_id % UHM_PAGE_MAP_SHARDS;
    const size_t arena_size = (second_leaf_id + 2u) * leaf_pages * page_size;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uintptr_t first_addr;
    uintptr_t second_addr;
    void *fixed;
    uhm_page_leaf **link;
    uhm_page_leaf *spare_leaf = 0;
    uhm_page_leaf *seeded_spare = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "page leaf spare heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "page leaf spare arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "page leaf spare usermode vm init");

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
    config.control_band_min_size = 64u * 1024u;

    expect_ok(uhm_open(&config, &heap), "page leaf spare heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "page leaf spare api");

    first_addr = (uintptr_t)arena_mem.aligned +
                 (first_leaf_id * leaf_pages + 8u) * page_size;
    fixed = (void *)first_addr;
    expect_ok(api->mmap(heap, &fixed, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
              "page leaf spare first map");
    expect_true(fixed == (void *)first_addr, "page leaf spare fixed first");
    expect_ok(api->munmap(heap, fixed, page_size),
              "page leaf spare first unmap");

    link = &heap->core.page_map[target_shard];
    while (*link) {
        if ((*link)->base_index == first_leaf_id * leaf_pages) {
            spare_leaf = *link;
            *link = spare_leaf->next;
            memset(spare_leaf, 0, sizeof(*spare_leaf));
            spare_leaf->next = heap->core.page_map_spares[target_shard];
            heap->core.page_map_spares[target_shard] = spare_leaf;
            ++heap->core.page_map_spare_count[target_shard];
            break;
        }
        link = &(*link)->next;
    }
    expect_true(spare_leaf != 0, "page leaf spare seeded");
    seeded_spare = spare_leaf;
    expect_ok(api->check(heap, 0), "page leaf spare seeded check");

    second_addr = (uintptr_t)arena_mem.aligned +
                  (second_leaf_id * leaf_pages + 8u) * page_size;
    fixed = (void *)second_addr;
    expect_ok(api->mmap(heap, &fixed, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
              "page leaf spare second map");
    expect_true(fixed == (void *)second_addr, "page leaf spare fixed second");
    expect_true(heap->core.page_map_spares[target_shard] == 0 &&
                    heap->core.page_map_spare_count[target_shard] == 0,
                "page leaf spare consumed");

    spare_leaf = heap->core.page_map[target_shard];
    while (spare_leaf && spare_leaf->base_index != second_leaf_id * leaf_pages) {
        spare_leaf = spare_leaf->next;
    }
    expect_true(spare_leaf == seeded_spare, "page leaf spare reused into live map");
    expect_ok(api->munmap(heap, fixed, page_size),
              "page leaf spare second unmap");
    expect_ok(api->check(heap, 0), "page leaf spare final check");
    expect_ok(api->close(heap), "page leaf spare close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

