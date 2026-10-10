static int test_mmap_size_hint_position_matches(
    uhm_heap *heap, const uhm_free_extent *extent) {
    size_t bucket;
    size_t slot;

    if (!heap || !extent ||
        (extent->flags & UHM_FREE_EXTENT_FLAG_IN_MMAP_SIZE_HINT) == 0) {
        return 0;
    }
    bucket = (size_t)((extent->flags &
                       UHM_FREE_EXTENT_SIZE_HINT_BUCKET_MASK) >>
                      UHM_FREE_EXTENT_SIZE_HINT_BUCKET_SHIFT);
    slot = (size_t)((extent->flags & UHM_FREE_EXTENT_SIZE_HINT_SLOT_MASK) >>
                    UHM_FREE_EXTENT_SIZE_HINT_SLOT_SHIFT);
    return bucket < UHM_MMAP_SIZE_HINT_BUCKETS &&
           slot < UHM_MMAP_SIZE_HINT_SLOTS &&
           heap->core.mmap_size_hint[bucket][slot] == extent;
}

static int test_mmap_recent_free_position_matches(
    uhm_heap *heap, const uhm_free_extent *extent) {
    size_t slot;

    if (!heap || !extent ||
        (extent->flags & UHM_FREE_EXTENT_FLAG_IN_MMAP_RECENT) == 0) {
        return 0;
    }
    slot = (size_t)((extent->flags & UHM_FREE_EXTENT_RECENT_SLOT_MASK) >>
                    UHM_FREE_EXTENT_RECENT_SLOT_SHIFT);
    return slot < UHM_MMAP_RECENT_FREE_CACHE_SLOTS &&
           heap->core.mmap_recent_free_cache[slot] == extent;
}

static void test_mmap_free_recent_extent_reuse(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 8u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *base = 0;
    unsigned char *base2 = 0;
    unsigned char *base3 = 0;
    unsigned char *base4 = 0;
    unsigned char *base5 = 0;
    unsigned char *base6 = 0;
    void *map = 0;
    void *map2 = 0;
    void *map3 = 0;
    void *map4 = 0;
    void *map5 = 0;
    void *map6 = 0;
    void *reuse = 0;
    void *reuse_big = 0;
    void *reuse_exact = 0;
    void *reuse_size_hint = 0;
    void *reuse_size_hint_bucket = 0;
    void *reuse_size_hint_deep = 0;
    uhm_free_extent *saved_recent;
    uhm_free_extent *saved_cache0;
    uhm_free_extent *saved_cache1;
    uhm_free_extent *saved_size_hint0;
    uhm_free_extent *saved_size_hint1;
    uhm_free_extent *saved_wrong_bucket_hint0;
    uhm_free_extent *size_hint_exact_extent = 0;
    uhm_free_extent fake_stale_extent;
    size_t wrong_hint_bucket;
    size_t filler_slot;
    size_t deep_slot;
    size_t size_hint_exact_bucket;
    size_t size_hint_slot;
    size_t saved_recent_next;
    size_t deep_too_small_count = UHM_MMAP_SIZE_HINT_SLOTS - 1u;
    size_t deep_extra_recent_count =
        UHM_MMAP_RECENT_FREE_CACHE_SLOTS > deep_too_small_count
            ? UHM_MMAP_RECENT_FREE_CACHE_SLOTS - deep_too_small_count
            : 0u;
    size_t deep_total_pages =
        6u + deep_too_small_count * 4u + deep_extra_recent_count * 2u + 1u;
    uint64_t find_calls_before_reuse;
    uint64_t recent_attempts_before;
    uint64_t recent_hits_before;
    uint64_t recent_misses_before;
    uint64_t recent_too_small_before;
    uint64_t recent_best_fit_before;
    uint64_t size_hint_attempts_before;
    uint64_t size_hint_hits_before;
    uint64_t size_hint_misses_before;
    uint64_t size_hint_too_small_before;
    uint64_t size_hint_direct_exact_hits_before;
    uint32_t saved_recent_flags;
    uint32_t saved_recent_cache_flags;
    uint32_t saved_size_hint_flags;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mmap recent heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mmap recent arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mmap recent usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "mmap recent heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mmap recent api");

    expect_ok(api->mmap(heap, &map, 4u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap recent initial map");
    expect_true(map != 0, "mmap recent initial map address");
    base = (unsigned char *)map;

    expect_ok(api->munmap(heap, base + 3u * page_size, page_size),
              "mmap recent high munmap");
    expect_true(heap->core.mmap_recent_free_extent != 0 &&
                    heap->core.mmap_recent_free_extent->addr ==
                        (uintptr_t)(base + 3u * page_size),
                "mmap recent high hint");
    expect_ok(api->munmap(heap, base + page_size, page_size),
              "mmap recent low munmap");
    expect_true(heap->core.mmap_recent_free_extent != 0 &&
                    heap->core.mmap_recent_free_extent->addr ==
                        (uintptr_t)(base + page_size),
                "mmap recent low hint");

    saved_recent = heap->core.mmap_recent_free_extent;
    expect_true((saved_recent->flags & UHM_META_FLAG_IN_MMAP_FREE_LIST) != 0,
                "mmap recent hint has free-list membership bit");
    saved_recent_flags = saved_recent->flags;
    saved_recent->flags &= ~UHM_META_FLAG_IN_MMAP_FREE_LIST;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap recent missing membership bit check");
    saved_recent->flags = saved_recent_flags;
    expect_ok(api->check(heap, 0),
              "mmap recent membership bit restored check");
    heap->core.mmap_recent_free_extent = (uhm_free_extent *)heap;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap recent invalid hint check");
    heap->core.mmap_recent_free_extent = saved_recent;
    expect_ok(api->check(heap, 0), "mmap recent restored hint check");

    saved_cache0 = heap->core.mmap_recent_free_cache[0];
    expect_true(test_mmap_recent_free_position_matches(heap, saved_cache0),
                "mmap recent cache position tracks slot zero");
    saved_recent_cache_flags = saved_cache0->flags;
    saved_cache0->flags &= ~UHM_FREE_EXTENT_FLAG_IN_MMAP_RECENT;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap recent cache missing position flag check");
    saved_cache0->flags = saved_recent_cache_flags;
    expect_ok(api->check(heap, 0),
              "mmap recent cache restored position flag check");
    heap->core.mmap_recent_free_cache[0] = (uhm_free_extent *)heap;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap recent invalid cache entry check");
    heap->core.mmap_recent_free_cache[0] = saved_cache0;
    expect_ok(api->check(heap, 0), "mmap recent restored cache entry check");

    saved_cache1 = heap->core.mmap_recent_free_cache[1];
    saved_recent_next = heap->core.mmap_recent_free_next;
    expect_true(saved_recent_next == 2u,
                "mmap recent used-count tracks compact cache");
    heap->core.mmap_recent_free_next = 0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap recent used-count corruption check");
    heap->core.mmap_recent_free_next = saved_recent_next;
    expect_ok(api->check(heap, 0),
              "mmap recent used-count restored check");
    heap->core.mmap_recent_free_cache[0] = 0;
    heap->core.mmap_recent_free_cache[1] = saved_cache0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap recent cache gap check");
    heap->core.mmap_recent_free_cache[0] = saved_cache0;
    heap->core.mmap_recent_free_cache[1] = saved_cache1;
    expect_ok(api->check(heap, 0),
              "mmap recent cache gap restored check");
    expect_true(saved_cache0->prev == 0,
                "mmap free-list first extent prev is null");
    saved_cache0->prev = saved_cache0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap free-list head prev corruption check");
    saved_cache0->prev = 0;
    expect_ok(api->check(heap, 0),
              "mmap free-list head prev restored check");
    expect_true(!saved_cache1 || saved_cache1->prev == saved_cache0,
                "mmap free-list second extent prev tracks first");
    if (saved_cache1) {
        saved_cache1->prev = 0;
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "mmap free-list second prev corruption check");
        saved_cache1->prev = saved_cache0;
        expect_ok(api->check(heap, 0),
                  "mmap free-list second prev restored check");
    }

    saved_size_hint0 = heap->core.mmap_size_hint[0][0];
    expect_true(test_mmap_size_hint_position_matches(heap, saved_size_hint0),
                "mmap size hint position tracks slot zero");
    saved_size_hint_flags = saved_size_hint0->flags;
    saved_size_hint0->flags &= ~UHM_FREE_EXTENT_FLAG_IN_MMAP_SIZE_HINT;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap size hint missing position flag check");
    saved_size_hint0->flags = saved_size_hint_flags;
    expect_ok(api->check(heap, 0),
              "mmap size hint restored position flag check");

    heap->core.mmap_size_hint[0][0] = (uhm_free_extent *)heap;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap size hint invalid entry check");
    heap->core.mmap_size_hint[0][0] = saved_size_hint0;
    expect_ok(api->check(heap, 0), "mmap size hint restored entry check");

    saved_size_hint1 = heap->core.mmap_size_hint[0][1];
    heap->core.mmap_size_hint[0][0] = 0;
    heap->core.mmap_size_hint[0][1] = saved_size_hint0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap size hint bucket gap check");
    heap->core.mmap_size_hint[0][0] = saved_size_hint0;
    heap->core.mmap_size_hint[0][1] = saved_size_hint1;
    expect_ok(api->check(heap, 0), "mmap size hint bucket gap restored check");

    wrong_hint_bucket = 1u;
    saved_wrong_bucket_hint0 = heap->core.mmap_size_hint[wrong_hint_bucket][0];
    heap->core.mmap_size_hint[wrong_hint_bucket][0] = saved_recent;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap size hint wrong bucket check");
    heap->core.mmap_size_hint[wrong_hint_bucket][0] =
        saved_wrong_bucket_hint0;
    expect_ok(api->check(heap, 0),
              "mmap size hint wrong bucket restored check");

    memset(&fake_stale_extent, 0, sizeof(fake_stale_extent));
    fake_stale_extent.magic = UHM_FREE_MAGIC;
    fake_stale_extent.addr = (uintptr_t)base;
    fake_stale_extent.size = page_size;
    heap->core.mmap_recent_free_cache[0] = &fake_stale_extent;
    heap->core.mmap_recent_free_cache[1] = saved_cache0;
    heap->core.mmap_recent_free_extent = &fake_stale_extent;
    find_calls_before_reuse = heap->core.mmap_free_tree_find_calls;
    expect_ok(api->mmap(heap, &reuse, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap recent auto reuse");
    expect_true(reuse == base + page_size,
                "mmap recent reuses most recent extent before highest fit");
    expect_true(heap->core.mmap_recent_free_attempts == 1u,
                "mmap recent attempt counted");
    expect_true(heap->core.mmap_recent_free_hits == 1u,
                "mmap recent hit counted");
    expect_true(heap->core.mmap_recent_free_misses == 0,
                "mmap recent miss not counted on hit");
    expect_true(heap->core.mmap_recent_free_too_small == 0,
                "mmap recent too-small not counted on hit");
    expect_true(heap->core.mmap_recent_free_extent != &fake_stale_extent &&
                    heap->core.mmap_recent_free_cache[0] !=
                        &fake_stale_extent,
                "mmap recent stale unlisted entry was dropped");
    expect_true(heap->core.mmap_recent_free_next == 0 &&
                    heap->core.mmap_recent_free_cache[0] == 0,
                "mmap recent sparse exact consume clears used window");
    expect_true(heap->core.mmap_free_tree_find_calls == find_calls_before_reuse,
                "mmap recent hit skips highest-fit tree find");

    expect_ok(api->mmap(heap, &map2, 6u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap recent cache second map");
    expect_true(map2 != 0, "mmap recent cache second map address");
    base2 = (unsigned char *)map2;

    expect_ok(api->munmap(heap, base2 + 4u * page_size, 2u * page_size),
              "mmap recent cache older big munmap");
    expect_ok(api->munmap(heap, base2 + page_size, page_size),
              "mmap recent cache newer small munmap");

    recent_attempts_before = heap->core.mmap_recent_free_attempts;
    recent_hits_before = heap->core.mmap_recent_free_hits;
    recent_misses_before = heap->core.mmap_recent_free_misses;
    recent_too_small_before = heap->core.mmap_recent_free_too_small;
    find_calls_before_reuse = heap->core.mmap_free_tree_find_calls;

    expect_ok(api->mmap(heap, &reuse_big, 2u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap recent cache auto reuse");
    expect_true(reuse_big == base2 + 4u * page_size,
                "mmap recent cache skips too-small latest and reuses older fit");
    expect_true(heap->core.mmap_recent_free_attempts ==
                    recent_attempts_before + 1u,
                "mmap recent cache fit attempt counted");
    expect_true(heap->core.mmap_recent_free_hits == recent_hits_before + 1u,
                "mmap recent cache hit counted");
    expect_true(heap->core.mmap_recent_free_misses == recent_misses_before,
                "mmap recent cache miss not counted on hit");
    expect_true(heap->core.mmap_recent_free_too_small >
                    recent_too_small_before,
                "mmap recent cache too-small counted");
    expect_true(heap->core.mmap_free_tree_find_calls == find_calls_before_reuse,
                "mmap recent cache hit skips highest-fit tree find");

    expect_ok(api->mmap(heap, &map3, 10u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap recent best-fit map");
    expect_true(map3 != 0, "mmap recent best-fit map address");
    base3 = (unsigned char *)map3;

    expect_ok(api->munmap(heap, base3 + 9u * page_size, page_size),
              "mmap recent best-fit older too-small munmap");
    expect_ok(api->munmap(heap, base3 + page_size, 2u * page_size),
              "mmap recent best-fit older exact munmap");
    expect_ok(api->munmap(heap, base3 + 4u * page_size, 4u * page_size),
              "mmap recent best-fit newer large munmap");

    recent_attempts_before = heap->core.mmap_recent_free_attempts;
    recent_hits_before = heap->core.mmap_recent_free_hits;
    recent_misses_before = heap->core.mmap_recent_free_misses;
    recent_too_small_before = heap->core.mmap_recent_free_too_small;
    recent_best_fit_before =
        heap->core.mmap_recent_free_best_fit_replacements;
    find_calls_before_reuse = heap->core.mmap_free_tree_find_calls;

    expect_ok(api->mmap(heap, &reuse_exact, 2u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap recent best-fit auto reuse");
    expect_true(reuse_exact == base3 + page_size,
                "mmap recent best-fit prefers smaller hot extent");
    expect_true(heap->core.mmap_recent_free_attempts ==
                    recent_attempts_before + 1u,
                "mmap recent best-fit attempt counted");
    expect_true(heap->core.mmap_recent_free_hits == recent_hits_before + 1u,
                "mmap recent best-fit hit counted");
    expect_true(heap->core.mmap_recent_free_misses == recent_misses_before,
                "mmap recent best-fit miss not counted on hit");
    expect_true(heap->core.mmap_recent_free_too_small ==
                    recent_too_small_before,
                "mmap recent exact fit stops before older too-small entry");
    expect_true(heap->core.mmap_recent_free_best_fit_replacements ==
                    recent_best_fit_before + 1u,
                "mmap recent best-fit replacement counted");
    expect_true(heap->core.mmap_free_tree_find_calls == find_calls_before_reuse,
                "mmap recent best-fit skips highest-fit tree find");

    expect_ok(api->munmap(heap, base3, page_size),
              "mmap recent best-fit first cleanup");
    expect_ok(api->munmap(heap, reuse_exact, 2u * page_size),
              "mmap recent best-fit exact cleanup");
    expect_ok(api->munmap(heap, base3 + 3u * page_size, page_size),
              "mmap recent best-fit middle cleanup");
    expect_ok(api->munmap(heap, base3 + 8u * page_size, page_size),
              "mmap recent best-fit tail guard cleanup");

    memset(heap->core.mmap_recent_free_cache, 0,
           sizeof(heap->core.mmap_recent_free_cache));
    heap->core.mmap_recent_free_extent = 0;
    heap->core.mmap_recent_free_next = 0;
    memset(heap->core.mmap_size_hint, 0, sizeof(heap->core.mmap_size_hint));
    memset(heap->core.mmap_size_hint_bucket_max, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max));
    memset(heap->core.mmap_size_hint_bucket_max_extent, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max_extent));

    expect_ok(api->mmap(heap, &map4,
                        (4u + UHM_MMAP_RECENT_FREE_CACHE_SLOTS * 2u) *
                            page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap size hint map");
    expect_true(map4 != 0, "mmap size hint map address");
    base4 = (unsigned char *)map4;
    memset(heap->core.mmap_recent_free_cache, 0,
           sizeof(heap->core.mmap_recent_free_cache));
    heap->core.mmap_recent_free_extent = 0;
    heap->core.mmap_recent_free_next = 0;
    memset(heap->core.mmap_size_hint, 0, sizeof(heap->core.mmap_size_hint));
    memset(heap->core.mmap_size_hint_bucket_max, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max));
    memset(heap->core.mmap_size_hint_bucket_max_extent, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max_extent));

    expect_ok(api->munmap(heap, base4 + page_size, 2u * page_size),
              "mmap size hint exact extent munmap");
    for (filler_slot = 0; filler_slot < UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
         ++filler_slot) {
        size_t page_index = 4u + filler_slot * 2u;
        expect_ok(api->munmap(heap, base4 + page_index * page_size,
                              page_size),
                  "mmap size hint filler munmap");
    }

    recent_attempts_before = heap->core.mmap_recent_free_attempts;
    recent_misses_before = heap->core.mmap_recent_free_misses;
    recent_too_small_before = heap->core.mmap_recent_free_too_small;
    size_hint_attempts_before = heap->core.mmap_size_hint_attempts;
    size_hint_hits_before = heap->core.mmap_size_hint_hits;
    size_hint_misses_before = heap->core.mmap_size_hint_misses;
    size_hint_too_small_before = heap->core.mmap_size_hint_too_small;
    size_hint_direct_exact_hits_before =
        heap->core.mmap_size_hint_direct_exact_hits;
    find_calls_before_reuse = heap->core.mmap_free_tree_find_calls;
    size_hint_exact_bucket = 1u;
    for (size_hint_slot = 0; size_hint_slot < UHM_MMAP_SIZE_HINT_SLOTS;
         ++size_hint_slot) {
        uhm_free_extent *hint =
            heap->core.mmap_size_hint[size_hint_exact_bucket]
                                     [size_hint_slot];
        if (hint && hint->addr == (uintptr_t)(base4 + page_size) &&
            hint->size == 2u * page_size) {
            size_hint_exact_extent = hint;
            break;
        }
    }
    expect_true(size_hint_exact_extent != 0,
                "mmap size hint exact extent is cached before reuse");

    expect_ok(api->mmap(heap, &reuse_size_hint, 2u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap size hint auto reuse");
    expect_true(reuse_size_hint == base4 + page_size,
                "mmap size hint reuses exact extent outside recent cache");
    expect_true(heap->core.mmap_recent_free_attempts ==
                    recent_attempts_before + 1u,
                "mmap size hint recent miss attempt counted");
    expect_true(heap->core.mmap_recent_free_misses ==
                    recent_misses_before + 1u,
                "mmap size hint recent miss counted");
    expect_true(heap->core.mmap_recent_free_too_small >
                    recent_too_small_before,
                "mmap size hint recent too-small fillers counted");
    expect_true(heap->core.mmap_size_hint_attempts ==
                    size_hint_attempts_before + 1u,
                "mmap size hint attempt counted");
    expect_true(heap->core.mmap_size_hint_hits ==
                    size_hint_hits_before + 1u,
                "mmap size hint hit counted");
    expect_true(heap->core.mmap_size_hint_misses == size_hint_misses_before,
                "mmap size hint miss not counted on hit");
    expect_true(heap->core.mmap_size_hint_too_small ==
                    size_hint_too_small_before,
                "mmap size hint exact bucket not too small");
    expect_true(heap->core.mmap_size_hint_direct_exact_hits ==
                    size_hint_direct_exact_hits_before + 1u,
                "mmap size hint exact max direct candidate used");
    expect_true(heap->core.mmap_size_hint[size_hint_exact_bucket][0] == 0,
                "mmap size hint consumed exact bucket is compacted empty");
    for (size_hint_slot = 0; size_hint_slot < UHM_MMAP_SIZE_HINT_SLOTS;
         ++size_hint_slot) {
        expect_true(
            heap->core.mmap_size_hint[size_hint_exact_bucket]
                                     [size_hint_slot] !=
                size_hint_exact_extent,
            "mmap size hint consumed exact extent is forgotten");
    }
    expect_true(heap->core.mmap_free_tree_find_calls == find_calls_before_reuse,
                "mmap size hint skips highest-fit tree find");

    expect_ok(api->munmap(heap, base4, 4u * page_size),
              "mmap size hint first live cleanup");
    for (filler_slot = 0; filler_slot < UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
         ++filler_slot) {
        size_t page_index = 5u + filler_slot * 2u;
        expect_ok(api->munmap(heap, base4 + page_index * page_size,
                              page_size),
                  "mmap size hint filler live cleanup");
    }

    memset(heap->core.mmap_recent_free_cache, 0,
           sizeof(heap->core.mmap_recent_free_cache));
    heap->core.mmap_recent_free_extent = 0;
    heap->core.mmap_recent_free_next = 0;
    memset(heap->core.mmap_size_hint, 0, sizeof(heap->core.mmap_size_hint));
    memset(heap->core.mmap_size_hint_bucket_max, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max));
    memset(heap->core.mmap_size_hint_bucket_max_extent, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max_extent));

    expect_ok(api->mmap(heap, &map5,
                        (15u + UHM_MMAP_RECENT_FREE_CACHE_SLOTS * 2u) *
                            page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap size hint bucket map");
    expect_true(map5 != 0, "mmap size hint bucket map address");
    base5 = (unsigned char *)map5;
    memset(heap->core.mmap_recent_free_cache, 0,
           sizeof(heap->core.mmap_recent_free_cache));
    heap->core.mmap_recent_free_extent = 0;
    heap->core.mmap_recent_free_next = 0;
    memset(heap->core.mmap_size_hint, 0, sizeof(heap->core.mmap_size_hint));
    memset(heap->core.mmap_size_hint_bucket_max, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max));
    memset(heap->core.mmap_size_hint_bucket_max_extent, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max_extent));

    expect_ok(api->munmap(
                  heap,
                  base5 +
                      (12u + UHM_MMAP_RECENT_FREE_CACHE_SLOTS * 2u) *
                          page_size,
                  3u * page_size),
              "mmap size hint bucket older too-small munmap");
    expect_ok(api->munmap(heap, base5 + page_size, 4u * page_size),
              "mmap size hint bucket older exact munmap");
    expect_ok(api->munmap(heap, base5 + 6u * page_size, 3u * page_size),
              "mmap size hint bucket newer too-small munmap");
    for (filler_slot = 0; filler_slot < UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
         ++filler_slot) {
        size_t page_index = 10u + filler_slot * 2u;
        expect_ok(api->munmap(heap, base5 + page_index * page_size,
                              page_size),
                  "mmap size hint bucket filler munmap");
    }

    recent_attempts_before = heap->core.mmap_recent_free_attempts;
    recent_misses_before = heap->core.mmap_recent_free_misses;
    recent_too_small_before = heap->core.mmap_recent_free_too_small;
    size_hint_attempts_before = heap->core.mmap_size_hint_attempts;
    size_hint_hits_before = heap->core.mmap_size_hint_hits;
    size_hint_misses_before = heap->core.mmap_size_hint_misses;
    size_hint_too_small_before = heap->core.mmap_size_hint_too_small;
    find_calls_before_reuse = heap->core.mmap_free_tree_find_calls;
    expect_true(heap->core.mmap_size_hint_bucket_max[2] == 4u * page_size,
                "mmap size hint bucket max summarizes candidates");

    expect_ok(api->mmap(heap, &reuse_size_hint_bucket, 4u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap size hint bucket auto reuse");
    expect_true(reuse_size_hint_bucket == base5 + page_size,
                "mmap size hint bucket skips newer too-small entry");
    expect_true(heap->core.mmap_recent_free_attempts ==
                    recent_attempts_before + 1u,
                "mmap size hint bucket recent miss attempt counted");
    expect_true(heap->core.mmap_recent_free_misses ==
                    recent_misses_before + 1u,
                "mmap size hint bucket recent miss counted");
    expect_true(heap->core.mmap_recent_free_too_small >
                    recent_too_small_before,
                "mmap size hint bucket recent fillers counted");
    expect_true(heap->core.mmap_size_hint_attempts ==
                    size_hint_attempts_before + 1u,
                "mmap size hint bucket attempt counted");
    expect_true(heap->core.mmap_size_hint_hits ==
                    size_hint_hits_before + 1u,
                "mmap size hint bucket hit counted");
    expect_true(heap->core.mmap_size_hint_misses == size_hint_misses_before,
                "mmap size hint bucket miss not counted on hit");
    expect_true(heap->core.mmap_size_hint_too_small ==
                    size_hint_too_small_before,
                "mmap size hint exact max skips same bucket too-small candidate");
    expect_true(heap->core.mmap_size_hint_direct_exact_hits >= 1u,
                "mmap size hint exact max summary remains active");
    expect_true(heap->core.mmap_free_tree_find_calls == find_calls_before_reuse,
                "mmap size hint bucket skips highest-fit tree find");

    expect_ok(api->munmap(heap, base5, 6u * page_size),
              "mmap size hint bucket first live cleanup");
    expect_ok(api->munmap(heap, base5 + 9u * page_size, page_size),
              "mmap size hint bucket guard cleanup");
    for (filler_slot = 0;
         filler_slot + 1u < UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
         ++filler_slot) {
        size_t page_index = 11u + filler_slot * 2u;
        expect_ok(api->munmap(heap, base5 + page_index * page_size,
                              page_size),
                  "mmap size hint bucket filler live cleanup");
    }
    expect_ok(api->munmap(
                  heap,
                  base5 +
                      (9u + UHM_MMAP_RECENT_FREE_CACHE_SLOTS * 2u) *
                          page_size,
                  3u * page_size),
              "mmap size hint bucket tail guard cleanup");
    memset(heap->core.mmap_recent_free_cache, 0,
           sizeof(heap->core.mmap_recent_free_cache));
    heap->core.mmap_recent_free_extent = 0;
    heap->core.mmap_recent_free_next = 0;
    memset(heap->core.mmap_size_hint, 0, sizeof(heap->core.mmap_size_hint));
    memset(heap->core.mmap_size_hint_bucket_max, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max));
    memset(heap->core.mmap_size_hint_bucket_max_extent, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max_extent));

    expect_ok(api->mmap(heap, &map6, deep_total_pages * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap size hint deep bucket map");
    expect_true(map6 != 0, "mmap size hint deep bucket map address");
    base6 = (unsigned char *)map6;
    memset(heap->core.mmap_recent_free_cache, 0,
           sizeof(heap->core.mmap_recent_free_cache));
    heap->core.mmap_recent_free_extent = 0;
    heap->core.mmap_recent_free_next = 0;
    memset(heap->core.mmap_size_hint, 0, sizeof(heap->core.mmap_size_hint));
    memset(heap->core.mmap_size_hint_bucket_max, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max));
    memset(heap->core.mmap_size_hint_bucket_max_extent, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max_extent));

    expect_ok(api->munmap(heap, base6 + page_size, 4u * page_size),
              "mmap size hint deep bucket older exact munmap");
    for (deep_slot = 0; deep_slot < deep_too_small_count; ++deep_slot) {
        size_t page_index = 6u + deep_slot * 4u;
        expect_ok(api->munmap(heap, base6 + page_index * page_size,
                              3u * page_size),
                  "mmap size hint deep bucket newer too-small munmap");
    }
    for (filler_slot = 0; filler_slot < deep_extra_recent_count;
         ++filler_slot) {
        size_t page_index = 6u + deep_too_small_count * 4u +
                            filler_slot * 2u;
        expect_ok(api->munmap(heap, base6 + page_index * page_size,
                              page_size),
                  "mmap size hint deep bucket recent filler munmap");
    }

    recent_attempts_before = heap->core.mmap_recent_free_attempts;
    recent_misses_before = heap->core.mmap_recent_free_misses;
    recent_too_small_before = heap->core.mmap_recent_free_too_small;
    size_hint_attempts_before = heap->core.mmap_size_hint_attempts;
    size_hint_hits_before = heap->core.mmap_size_hint_hits;
    size_hint_misses_before = heap->core.mmap_size_hint_misses;
    size_hint_too_small_before = heap->core.mmap_size_hint_too_small;
    find_calls_before_reuse = heap->core.mmap_free_tree_find_calls;

    expect_ok(api->mmap(heap, &reuse_size_hint_deep, 4u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap size hint deep bucket auto reuse");
    expect_true(reuse_size_hint_deep == base6 + page_size,
                "mmap size hint deep bucket retains older exact entry");
    expect_true(heap->core.mmap_recent_free_attempts ==
                    recent_attempts_before + 1u,
                "mmap size hint deep bucket recent miss attempt counted");
    expect_true(heap->core.mmap_recent_free_misses ==
                    recent_misses_before + 1u,
                "mmap size hint deep bucket recent miss counted");
    expect_true(heap->core.mmap_recent_free_too_small >
                    recent_too_small_before,
                "mmap size hint deep bucket recent too-small counted");
    expect_true(heap->core.mmap_size_hint_attempts ==
                    size_hint_attempts_before + 1u,
                "mmap size hint deep bucket attempt counted");
    expect_true(heap->core.mmap_size_hint_hits ==
                    size_hint_hits_before + 1u,
                "mmap size hint deep bucket hit counted");
    expect_true(heap->core.mmap_size_hint_misses == size_hint_misses_before,
                "mmap size hint deep bucket miss not counted on hit");
    expect_true(heap->core.mmap_size_hint_too_small ==
                    size_hint_too_small_before,
                "mmap size hint deep exact max skips too-small slots");
    expect_true(heap->core.mmap_free_tree_find_calls == find_calls_before_reuse,
                "mmap size hint deep bucket skips highest-fit tree find");

    expect_ok(api->munmap(heap, base6, page_size),
              "mmap size hint deep bucket first live cleanup");
    expect_ok(api->munmap(heap, reuse_size_hint_deep, 4u * page_size),
              "mmap size hint deep bucket exact live cleanup");
    expect_ok(api->munmap(heap, base6 + 5u * page_size, page_size),
              "mmap size hint deep bucket guard cleanup");
    for (deep_slot = 0; deep_slot < deep_too_small_count; ++deep_slot) {
        size_t guard_page = 9u + deep_slot * 4u;
        expect_ok(api->munmap(heap, base6 + guard_page * page_size,
                              page_size),
                  "mmap size hint deep bucket guard live cleanup");
    }
    for (filler_slot = 0; filler_slot < deep_extra_recent_count;
         ++filler_slot) {
        size_t guard_page = 7u + deep_too_small_count * 4u +
                            filler_slot * 2u;
        expect_ok(api->munmap(heap, base6 + guard_page * page_size,
                              page_size),
                  "mmap size hint deep bucket recent filler guard cleanup");
    }
    expect_ok(api->munmap(heap,
                          base6 + (deep_total_pages - 1u) * page_size,
                          page_size),
              "mmap size hint deep bucket tail live cleanup");

    expect_ok(api->munmap(heap, base2, page_size),
              "mmap recent cache second first cleanup");
    expect_ok(api->munmap(heap, base2 + 2u * page_size, 4u * page_size),
              "mmap recent cache second tail cleanup");
    expect_ok(api->munmap(heap, reuse, page_size), "mmap recent reuse cleanup");
    expect_ok(api->munmap(heap, base, page_size), "mmap recent first cleanup");
    expect_ok(api->munmap(heap, base + 2u * page_size, page_size),
              "mmap recent third cleanup");
    expect_ok(api->check(heap, 0), "mmap recent final check");
    expect_ok(api->close(heap), "mmap recent close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mmap_free_no_fit_skips_hot_cache(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *base = 0;
    void *map = 0;
    void *reuse_no_fit = 0;
    uint64_t recent_attempts_before;
    uint64_t recent_too_small_before;
    uint64_t size_hint_attempts_before;
    uint64_t size_hint_too_small_before;
    uint64_t find_calls_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mmap no-fit heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mmap no-fit arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mmap no-fit usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "mmap no-fit heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mmap no-fit api");

    expect_ok(api->mmap(heap, &map, 4u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap no-fit initial map");
    expect_true(map != 0, "mmap no-fit initial map address");
    base = (unsigned char *)map;

    expect_ok(api->munmap(heap, base + page_size, page_size),
              "mmap no-fit one-page hole");
    expect_true(heap->core.mmap_free_tree_root != 0 &&
                    heap->core.mmap_free_tree_root->tree_max_size == page_size,
                "mmap no-fit free tree proves no 2-page extent");
    expect_true(heap->core.mmap_recent_free_extent != 0 ||
                    heap->core.mmap_recent_free_cache[0] != 0,
                "mmap no-fit hot cache has a too-small candidate");

    recent_attempts_before = heap->core.mmap_recent_free_attempts;
    recent_too_small_before = heap->core.mmap_recent_free_too_small;
    size_hint_attempts_before = heap->core.mmap_size_hint_attempts;
    size_hint_too_small_before = heap->core.mmap_size_hint_too_small;
    find_calls_before = heap->core.mmap_free_tree_find_calls;

    expect_ok(api->mmap(heap, &reuse_no_fit, 2u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap no-fit high-watermark fallback");
    expect_true(reuse_no_fit != 0, "mmap no-fit fallback address");
    expect_true(reuse_no_fit != base + page_size,
                "mmap no-fit does not reuse too-small hole");
    expect_true(heap->core.mmap_recent_free_attempts == recent_attempts_before,
                "mmap no-fit skips recent-cache attempt counter");
    expect_true(heap->core.mmap_recent_free_too_small == recent_too_small_before,
                "mmap no-fit skips recent-cache too-small scan");
    expect_true(heap->core.mmap_size_hint_attempts == size_hint_attempts_before,
                "mmap no-fit skips size-hint attempt counter");
    expect_true(heap->core.mmap_size_hint_too_small ==
                    size_hint_too_small_before,
                "mmap no-fit skips size-hint too-small scan");
    expect_true(heap->core.mmap_free_tree_find_calls == find_calls_before,
                "mmap no-fit skips highest-fit tree find");

    expect_ok(api->munmap(heap, reuse_no_fit, 2u * page_size),
              "mmap no-fit fallback cleanup");
    expect_ok(api->munmap(heap, base, page_size),
              "mmap no-fit first page cleanup");
    expect_ok(api->munmap(heap, base + 2u * page_size, 2u * page_size),
              "mmap no-fit tail cleanup");
    expect_ok(api->check(heap, 0), "mmap no-fit final check");
    expect_ok(api->close(heap), "mmap no-fit close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mmap_size_hint_bucket_max_skips_too_small_bucket(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *base;
    void *map = 0;
    void *reuse = 0;
    uhm_free_extent *extent;
    uint64_t size_hint_attempts_before;
    uint64_t size_hint_too_small_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mmap size hint max heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mmap size hint max arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mmap size hint max usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "mmap size hint max heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mmap size hint max api");

    expect_ok(api->mmap(heap, &map, 16u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap size hint max initial map");
    base = (unsigned char *)map;
    expect_ok(api->munmap(heap, base + page_size, 3u * page_size),
              "mmap size hint max small hole");
    expect_ok(api->munmap(heap, base + 6u * page_size, 5u * page_size),
              "mmap size hint max large hole");
    expect_true(heap->core.mmap_size_hint_bucket_max[2] == 3u * page_size,
                "mmap size hint max records small bucket");
    expect_true(heap->core.mmap_size_hint_bucket_max[3] == 5u * page_size,
                "mmap size hint max records large bucket");

    for (extent = heap->core.mmap_free_head; extent; extent = extent->next) {
        extent->flags &= ~UHM_FREE_EXTENT_RECENT_MASK;
    }
    memset(heap->core.mmap_recent_free_cache, 0,
           sizeof(heap->core.mmap_recent_free_cache));
    heap->core.mmap_recent_free_extent = 0;
    heap->core.mmap_recent_free_next = 0;

    size_hint_attempts_before = heap->core.mmap_size_hint_attempts;
    size_hint_too_small_before = heap->core.mmap_size_hint_too_small;
    expect_ok(api->mmap(heap, &reuse, 4u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap size hint max large bucket reuse");
    expect_true(reuse == base + 7u * page_size,
                "mmap size hint max skips small bucket and reuses large");
    expect_true(heap->core.mmap_size_hint_attempts ==
                    size_hint_attempts_before + 1u,
                "mmap size hint max attempt counted");
    expect_true(heap->core.mmap_size_hint_too_small ==
                    size_hint_too_small_before,
                "mmap size hint max avoids small bucket scan");
    expect_true(heap->core.mmap_size_hint_bucket_max[2] == 3u * page_size,
                "mmap size hint max preserves skipped bucket summary");
    expect_true(heap->core.mmap_size_hint_bucket_max[3] == 0,
                "mmap size hint max updates consumed bucket summary");

    expect_ok(api->munmap(heap, base, page_size),
              "mmap size hint max first live cleanup");
    expect_ok(api->munmap(heap, base + 4u * page_size, 2u * page_size),
              "mmap size hint max middle live cleanup");
    expect_ok(api->munmap(heap, reuse, 4u * page_size),
              "mmap size hint max reused live cleanup");
    expect_ok(api->munmap(heap, base + 11u * page_size, 5u * page_size),
              "mmap size hint max tail live cleanup");
    expect_ok(api->check(heap, 0), "mmap size hint max final check");
    expect_ok(api->close(heap), "mmap size hint max close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}
