static void test_mmap_fixed_munmap_mprotect_mix(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 8u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *base = 0;
    void *map = 0;
    void *fixed = 0;
    void *auto_reuse = 0;
    uhm_stats stats;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "mmap mix heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "mmap mix arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mmap mix usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "mmap mix heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mmap mix api");

    expect_ok(api->mmap(heap, &map, page_size * 16u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap mix initial auto mmap");
    base = (unsigned char *)map;
    expect_ok(api->mprotect(heap, base + page_size * 2u, page_size * 2u,
                            UHM_PROT_READ),
              "mmap mix split with mprotect");
    fixed = base + page_size;
    expect_ok(api->mmap(heap, &fixed, page_size * 3u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "mmap mix fixed replace across split VMAs");
    expect_true(fixed == base + page_size, "mmap mix cross-VMA replace address");
    expect_ok(api->munmap(heap, base + page_size * 4u, page_size * 4u),
              "mmap mix middle munmap");
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)(base + page_size * 4u),
                                           page_size * 4u) == 0u,
                "mmap mix middle munmap removes VMA coverage");
    expect_true(api->mprotect(heap, base + page_size * 4u, page_size,
                              UHM_PROT_READ) == UHM_ERR_NOT_MAPPED,
                "mmap mix middle munmap page map reusable");
    expect_ok(api->check(heap, 0), "mmap mix middle munmap check");
    test_shadow_model_check(heap, "mmap mix middle munmap shadow");

    fixed = base + page_size * 5u;
    expect_ok(api->mmap(heap, &fixed, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
              "mmap mix fixed noreplace into hole");
    expect_true(fixed == base + page_size * 5u, "mmap mix fixed address");

    expect_ok(api->mmap(heap, &auto_reuse, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap mix auto reuse extent");
    expect_true(auto_reuse != 0 && ((uintptr_t)auto_reuse % page_size) == 0,
                "mmap mix auto reuse page aligned");

    expect_ok(api->mprotect(heap, fixed, page_size, UHM_PROT_READ),
              "mmap mix fixed page mprotect");
    fixed = base + page_size * 10u;
    expect_ok(api->mmap(heap, &fixed, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "mmap mix fixed replace live page");
    expect_true(fixed == base + page_size * 10u, "mmap mix replace address");

    expect_ok(api->munmap(heap, base + page_size * 2u, page_size * 2u),
              "mmap mix munmap protected pages");
    fixed = base + page_size * 2u;
    expect_ok(api->mmap(heap, &fixed, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
              "mmap mix fixed remap protected hole");
    expect_true(fixed == base + page_size * 2u, "mmap mix remap address");

    expect_ok(api->check(heap, 0), "mmap mix check after operations");
    test_shadow_model_check(heap, "mmap mix shadow after operations");

    expect_ok(api->munmap(heap, base, page_size * 4u), "mmap mix cleanup low run");
    expect_ok(api->munmap(heap, base + page_size * 5u, page_size),
              "mmap mix cleanup fixed page");
    expect_ok(api->munmap(heap, auto_reuse, page_size * 2u),
              "mmap mix cleanup auto reuse");
    expect_ok(api->munmap(heap, base + page_size * 8u, page_size * 8u),
              "mmap mix cleanup high run");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "mmap mix stats");
    expect_true(stats.mmap_live_bytes == 0, "mmap mix releases all mmap bytes");
    expect_ok(api->check(heap, 0), "mmap mix heap check");
    test_shadow_model_check(heap, "mmap mix shadow after cleanup");
    expect_ok(api->close(heap), "mmap mix heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_cross_vma_merge(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *base = 0;
    void *map = 0;
    void *fixed = 0;
    uhm_vma *target_last_vma = 0;
    uhm_vma *right_neighbor_vma = 0;
    uhm_stats stats;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "cross-vma mprotect heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "cross-vma mprotect arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "cross-vma mprotect usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "cross-vma mprotect heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "cross-vma mprotect api");

    expect_ok(api->mmap(heap, &map, page_size * 6u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "cross-vma mprotect initial mmap");
    base = (unsigned char *)map;
    expect_ok(api->mprotect(heap, base + page_size, page_size, UHM_PROT_READ),
              "cross-vma mprotect first split");
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 3u,
                "cross-vma mprotect first split creates three VMAs");
    expect_true(test_has_exact_vma(heap, (uintptr_t)(base + page_size),
                                   page_size, UHM_PROT_READ),
                "cross-vma mprotect first split exact protected VMA");
    expect_ok(api->check(heap, 0), "cross-vma mprotect first split check");
    test_shadow_model_check(heap, "cross-vma mprotect first split shadow");
    expect_ok(api->mprotect(heap, base + page_size * 3u, page_size, UHM_PROT_READ),
              "cross-vma mprotect second split");
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base, page_size * 6u) == 5u,
                "cross-vma mprotect creates five VMAs");
    expect_ok(api->check(heap, 0), "cross-vma mprotect second split check");
    test_shadow_model_check(heap, "cross-vma mprotect second split shadow");

    fixed = base + page_size * 5u;
    expect_ok(api->mmap(heap, &fixed, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "cross-vma mprotect scramble list with fixed replacement");
    expect_true(fixed == base + page_size * 5u,
                "cross-vma mprotect fixed replacement address");
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 6u,
                "cross-vma mprotect fixed replacement leaves merge chain");
    target_last_vma =
        test_find_vma_containing(heap, base + page_size * 3u);
    right_neighbor_vma =
        test_find_vma_containing(heap, base + page_size * 4u);
    expect_true(target_last_vma && right_neighbor_vma &&
                    target_last_vma->next != right_neighbor_vma,
                "cross-vma mprotect list order differs from address order");
    expect_ok(api->check(heap, 0),
              "cross-vma mprotect fixed replacement check");
    test_shadow_model_check(heap,
                            "cross-vma mprotect fixed replacement shadow");

    expect_ok(api->mprotect(heap, base + page_size, page_size * 3u,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "cross-vma mprotect spans multiple VMAs");
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 2u,
                "cross-vma mprotect merges compatible adjacent VMAs");
    expect_true(test_has_exact_vma(heap, (uintptr_t)base, page_size * 5u,
                                    UHM_PROT_READ | UHM_PROT_WRITE),
                "cross-vma mprotect merged VMA reaches tree right neighbor");
    expect_true(test_has_exact_vma(heap,
                                   (uintptr_t)(base + page_size * 5u),
                                   page_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE),
                "cross-vma mprotect preserves fixed replacement VMA");
    expect_ok(api->check(heap, 0), "cross-vma mprotect heap check");
    test_shadow_model_check(heap, "cross-vma mprotect shadow");

    expect_ok(api->munmap(heap, base, page_size * 6u),
              "cross-vma mprotect cleanup munmap");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "cross-vma mprotect stats");
    expect_true(stats.mmap_live_bytes == 0, "cross-vma mprotect releases mmap bytes");
    expect_ok(api->check(heap, 0), "cross-vma mprotect final check");
    test_shadow_model_check(heap, "cross-vma mprotect final shadow");
    expect_ok(api->close(heap), "cross-vma mprotect heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_three_vma_merge_batches_page_mark(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *base = 0;
    void *map = 0;
    void *fixed = 0;
    size_t fixed_page;
    uint64_t page_mark_pages_before;
    uint64_t page_mark_pages_after;
    uint64_t page_mark_delta;
    uint64_t vma_find_calls_before;
    uint64_t vma_find_calls_after;
    uint64_t vma_find_delta;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "three-vma merge heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "three-vma merge arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "three-vma merge usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "three-vma merge heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "three-vma merge api");

    expect_ok(api->mmap(heap, &map, page_size * 6u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "three-vma merge mmap");
    base = (unsigned char *)map;
    expect_ok(api->mprotect(heap, base + page_size * 2u, page_size * 2u,
                            UHM_PROT_READ),
              "three-vma merge split middle");
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 3u,
                "three-vma merge split creates three VMAs");

    page_mark_pages_before =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_before = heap->core.vma_tree_find_calls;
    expect_ok(api->mprotect(heap, base + page_size * 2u, page_size * 2u,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "three-vma merge restore middle");
    page_mark_pages_after =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_after = heap->core.vma_tree_find_calls;
    page_mark_delta = page_mark_pages_after - page_mark_pages_before;
    vma_find_delta = vma_find_calls_after - vma_find_calls_before;

    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 1u,
                "three-vma merge batches to one VMA");
    expect_true(page_mark_delta > 0u && page_mark_delta <= 8u,
                "three-vma merge avoids target plus pairwise page-map rewrite");
    expect_true(vma_find_delta <= 1u,
                "three-vma merge uses tree neighbors after entry lookup");
    expect_ok(api->check(heap, 0), "three-vma merge check");
    test_shadow_model_check(heap, "three-vma merge shadow");

    expect_ok(api->munmap(heap, map, page_size * 6u),
              "three-vma merge cleanup munmap");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 8u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "exact closure mmap");
    base = (unsigned char *)map;
    for (fixed_page = 6u;; --fixed_page) {
        fixed = base + fixed_page * page_size;
        expect_ok(api->mmap(heap, &fixed, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED),
                  "exact closure fixed replacement");
        expect_true(fixed == base + fixed_page * page_size,
                    "exact closure fixed replacement address");
        if (fixed_page == 3u) {
            break;
        }
    }
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 8u) == 6u,
                "exact closure creates four adjacent fixed VMAs");
    expect_ok(api->mprotect(heap, base + page_size * 4u, page_size,
                            UHM_PROT_READ),
              "exact closure split target permission");

    page_mark_pages_before =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_before = heap->core.vma_tree_find_calls;
    expect_ok(api->mprotect(heap, base + page_size * 4u, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "exact closure restore target permission");
    page_mark_pages_after =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_after = heap->core.vma_tree_find_calls;
    page_mark_delta = page_mark_pages_after - page_mark_pages_before;
    vma_find_delta = vma_find_calls_after - vma_find_calls_before;

    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 8u) == 3u,
                "exact closure merges the full fixed VMA chain");
    expect_true(test_has_exact_vma(heap,
                                   (uintptr_t)(base + page_size * 3u),
                                   page_size * 4u,
                                   UHM_PROT_READ | UHM_PROT_WRITE),
                "exact closure publishes one fixed VMA");
    expect_true(page_mark_delta == 5u,
                "exact closure publishes protecting plus final range once");
    expect_true(vma_find_delta <= 1u,
                "exact closure walks tree neighbors without root lookup");
    expect_ok(api->check(heap, 0), "exact closure check");
    test_shadow_model_check(heap, "exact closure shadow");
    expect_ok(api->munmap(heap, map, page_size * 8u),
              "exact closure cleanup munmap");

    expect_ok(api->close(heap), "three-vma merge close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_partial_edge_merge_uses_tree_neighbors(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *base = 0;
    void *map = 0;
    void *fixed = 0;
    size_t fixed_page;
    uint64_t page_mark_pages_before;
    uint64_t page_mark_pages_after;
    uint64_t page_mark_delta;
    uint64_t vma_find_calls_before;
    uint64_t vma_find_calls_after;
    uint64_t vma_find_delta;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "partial edge merge heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "partial edge merge arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "partial edge merge usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "partial edge merge heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "partial edge merge api");

    expect_ok(api->mmap(heap, &map, page_size * 6u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "partial edge merge mmap");
    base = (unsigned char *)map;
    expect_ok(api->mprotect(heap, base, page_size * 2u, UHM_PROT_READ),
              "partial edge merge seed left read VMA");
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 2u,
                "partial edge merge seed creates two VMAs");

    page_mark_pages_before =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_before = heap->core.vma_tree_find_calls;
    expect_ok(api->mprotect(heap, base + page_size * 2u, page_size * 2u,
                            UHM_PROT_READ),
              "partial edge merge protect adjacent range");
    page_mark_pages_after =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_after = heap->core.vma_tree_find_calls;
    page_mark_delta = page_mark_pages_after - page_mark_pages_before;
    vma_find_delta = vma_find_calls_after - vma_find_calls_before;

    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 2u,
                "partial edge merge keeps merged left and right VMA");
    expect_true(test_has_exact_vma(heap, (uintptr_t)base, page_size * 4u,
                                   UHM_PROT_READ),
                "partial edge merge left range is merged");
    expect_true(test_has_exact_vma(heap, (uintptr_t)(base + page_size * 4u),
                                   page_size * 2u,
                                   UHM_PROT_READ | UHM_PROT_WRITE),
                "partial edge merge right remainder is preserved");
    expect_true(page_mark_delta > 0u && page_mark_delta <= 8u,
                "partial edge merge elides target before merged publish");
    expect_true(vma_find_delta <= 1u,
                "partial edge merge uses tree neighbors after entry lookup");
    expect_ok(api->check(heap, 0), "partial edge merge check");
    test_shadow_model_check(heap, "partial edge merge shadow");

    expect_ok(api->munmap(heap, map, page_size * 6u),
              "partial edge merge cleanup head munmap");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 6u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "partial edge merge tail mmap");
    base = (unsigned char *)map;
    expect_ok(api->mprotect(heap, base + page_size * 4u, page_size * 2u,
                            UHM_PROT_READ),
              "partial edge merge seed right read VMA");
    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 2u,
                "partial edge merge tail seed creates two VMAs");

    page_mark_pages_before =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_before = heap->core.vma_tree_find_calls;
    expect_ok(api->mprotect(heap, base + page_size * 2u, page_size * 2u,
                            UHM_PROT_READ),
              "partial edge merge tail protect adjacent range");
    page_mark_pages_after =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_after = heap->core.vma_tree_find_calls;
    page_mark_delta = page_mark_pages_after - page_mark_pages_before;
    vma_find_delta = vma_find_calls_after - vma_find_calls_before;

    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 6u) == 2u,
                "partial edge merge tail keeps left and merged right VMA");
    expect_true(test_has_exact_vma(heap, (uintptr_t)base, page_size * 2u,
                                   UHM_PROT_READ | UHM_PROT_WRITE),
                "partial edge merge tail left remainder is preserved");
    expect_true(test_has_exact_vma(heap, (uintptr_t)(base + page_size * 2u),
                                   page_size * 4u, UHM_PROT_READ),
                "partial edge merge tail right range is merged");
    expect_true(page_mark_delta > 0u && page_mark_delta <= 8u,
                "partial edge merge tail elides target before merged publish");
    expect_true(vma_find_delta <= 1u,
                "partial edge merge tail uses tree neighbors after entry lookup");
    expect_ok(api->check(heap, 0), "partial edge merge tail check");
    test_shadow_model_check(heap, "partial edge merge tail shadow");

    expect_ok(api->munmap(heap, map, page_size * 6u),
              "partial edge merge cleanup tail munmap");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 8u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "partial head closure mmap");
    base = (unsigned char *)map;
    for (fixed_page = 1u; fixed_page <= 3u; ++fixed_page) {
        fixed = base + fixed_page * page_size;
        expect_ok(api->mmap(heap, &fixed, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED),
                  "partial head closure fixed neighbor");
    }
    fixed = base + page_size * 4u;
    expect_ok(api->mmap(heap, &fixed, page_size * 3u, UHM_PROT_READ,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "partial head closure fixed target");

    page_mark_pages_before =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_before = heap->core.vma_tree_find_calls;
    expect_ok(api->mprotect(heap, base + page_size * 4u, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "partial head closure protect");
    page_mark_pages_after =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_after = heap->core.vma_tree_find_calls;
    page_mark_delta = page_mark_pages_after - page_mark_pages_before;
    vma_find_delta = vma_find_calls_after - vma_find_calls_before;

    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 8u) == 4u,
                "partial head closure merges all left neighbors");
    expect_true(test_has_exact_vma(heap,
                                   (uintptr_t)(base + page_size),
                                   page_size * 4u,
                                   UHM_PROT_READ | UHM_PROT_WRITE),
                "partial head closure publishes merged left range");
    expect_true(test_has_exact_vma(heap,
                                   (uintptr_t)(base + page_size * 5u),
                                   page_size * 2u, UHM_PROT_READ),
                "partial head closure preserves right remnant");
    expect_true(page_mark_delta == 7u,
                "partial head closure publishes each final range once");
    expect_true(vma_find_delta <= 1u,
                "partial head closure avoids root neighbor lookup");
    expect_ok(api->check(heap, 0), "partial head closure check");
    test_shadow_model_check(heap, "partial head closure shadow");
    expect_ok(api->munmap(heap, map, page_size * 8u),
              "partial head closure cleanup");

    map = 0;
    expect_ok(api->mmap(heap, &map, page_size * 8u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "partial tail closure mmap");
    base = (unsigned char *)map;
    fixed = base + page_size;
    expect_ok(api->mmap(heap, &fixed, page_size * 3u, UHM_PROT_READ,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED),
              "partial tail closure fixed target");
    for (fixed_page = 4u; fixed_page <= 6u; ++fixed_page) {
        fixed = base + fixed_page * page_size;
        expect_ok(api->mmap(heap, &fixed, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED),
                  "partial tail closure fixed neighbor");
    }

    page_mark_pages_before =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_before = heap->core.vma_tree_find_calls;
    expect_ok(api->mprotect(heap, base + page_size * 3u, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "partial tail closure protect");
    page_mark_pages_after =
        test_atomic_u64_load(&heap->core.page_mark_pages);
    vma_find_calls_after = heap->core.vma_tree_find_calls;
    page_mark_delta = page_mark_pages_after - page_mark_pages_before;
    vma_find_delta = vma_find_calls_after - vma_find_calls_before;

    expect_true(test_vma_count_overlapping(heap, (uintptr_t)base,
                                           page_size * 8u) == 4u,
                "partial tail closure merges all right neighbors");
    expect_true(test_has_exact_vma(heap,
                                   (uintptr_t)(base + page_size),
                                   page_size * 2u, UHM_PROT_READ),
                "partial tail closure preserves left remnant");
    expect_true(test_has_exact_vma(heap,
                                   (uintptr_t)(base + page_size * 3u),
                                   page_size * 4u,
                                   UHM_PROT_READ | UHM_PROT_WRITE),
                "partial tail closure publishes merged right range");
    expect_true(page_mark_delta == 7u,
                "partial tail closure publishes each final range once");
    expect_true(vma_find_delta <= 1u,
                "partial tail closure avoids root neighbor lookup");
    expect_ok(api->check(heap, 0), "partial tail closure check");
    test_shadow_model_check(heap, "partial tail closure shadow");
    expect_ok(api->munmap(heap, map, page_size * 8u),
              "partial tail closure cleanup");

    expect_ok(api->close(heap), "partial edge merge close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_merged_vma_alternating_subranges(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    enum { map_count = 64 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *maps[map_count];
    size_t sizes[map_count];
    size_t i;

    memset(maps, 0, sizeof(maps));
    memset(sizes, 0, sizeof(sizes));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "merged-vma alternating heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "merged-vma alternating arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "merged-vma alternating usermode vm init");

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
              "merged-vma alternating heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "merged-vma alternating api");

    for (i = 0; i < map_count; ++i) {
        sizes[i] = page_size * (1u + (i & 3u));
        expect_ok(api->mmap(heap, &maps[i], sizes[i],
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE),
                  "merged-vma alternating mmap");
    }
    expect_ok(api->check(heap, 0), "merged-vma alternating after mmap check");
    test_shadow_model_check(heap, "merged-vma alternating after mmap shadow");

    for (i = 0; i < map_count; ++i) {
        uint32_t prot = (i & 1u) ? (UHM_PROT_READ | UHM_PROT_WRITE)
                                 : UHM_PROT_READ;
        expect_ok(api->mprotect(heap, maps[i], sizes[i], prot),
                  "merged-vma alternating mprotect");
        expect_ok(api->check(heap, 0),
                  "merged-vma alternating mprotect check");
        test_shadow_model_check(heap,
                                "merged-vma alternating mprotect shadow");
    }

    for (i = 0; i < map_count; ++i) {
        expect_ok(api->munmap(heap, maps[i], sizes[i]),
                  "merged-vma alternating cleanup munmap");
    }
    expect_ok(api->check(heap, 0), "merged-vma alternating final check");
    test_shadow_model_check(heap, "merged-vma alternating final shadow");
    expect_ok(api->close(heap), "merged-vma alternating close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_usermode_decommit_and_fallback_modes(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uhm_stats stats;
    fault_vm_state state;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment, &heap_mem) != 0,
                "logical vm heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "logical vm arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "logical vm init");

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

    expect_ok(uhm_open(&config, &heap), "logical vm heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "logical vm api");
    expect_ok(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "logical vm mmap");
    expect_ok(api->munmap(heap, map, page_size), "logical vm munmap");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "logical vm stats");
    expect_true(stats.logical_decommitted_bytes >= page_size,
                "logical vm decommit counted as logical");
    expect_true(stats.physical_released_bytes == 0,
                "logical vm does not count physical release");
    {
        void *reuse = map;
        expect_ok(api->mmap(heap, &reuse, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
                  "logical vm fixed reuse");
        expect_true(reuse == map, "logical vm reuse same address");
        expect_ok(api->munmap(heap, reuse, page_size), "logical vm reuse cleanup");
    }
    expect_ok(api->check(heap, 0), "logical vm check");
    expect_ok(api->close(heap), "logical vm close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);

    memset(&heap_mem, 0, sizeof(heap_mem));
    memset(&arena_mem, 0, sizeof(arena_mem));
    memset(&state, 0, sizeof(state));
    heap = 0;
    map = 0;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment, &heap_mem) != 0,
                "unsupported protect heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "unsupported protect arena alloc");
    memset(&vm, 0, sizeof(vm));
    vm.size = sizeof(vm);
    vm.abi_major = UHM_ABI_MAJOR;
    vm.abi_minor = UHM_ABI_MINOR;
    vm.user = &state;
    vm.page_size = page_size;
    vm.allocation_granularity = page_size;
    vm.capabilities = UHM_VM_CAN_COMMIT | UHM_VM_CAN_DECOMMIT |
                      UHM_VM_CAN_RECOMMIT_SAME_ADDRESS;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "unsupported protect heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "unsupported protect api");
    expect_ok(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "unsupported protect mmap");
    expect_true(api->mprotect(heap, map, page_size, UHM_PROT_READ) == UHM_ERR_UNSUPPORTED,
                "missing protect capability returns unsupported");
    expect_ok(api->munmap(heap, map, page_size), "unsupported protect cleanup");
    expect_ok(api->check(heap, 0), "unsupported protect check");
    expect_ok(api->close(heap), "unsupported protect close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);

    memset(&heap_mem, 0, sizeof(heap_mem));
    memset(&arena_mem, 0, sizeof(arena_mem));
    memset(&state, 0, sizeof(state));
    heap = 0;
    map = 0;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment, &heap_mem) != 0,
                "fixed remap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "fixed remap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "fixed remap vm init");
    vm.user = &state;
    vm.capabilities &= ~UHM_VM_CAN_DECOMMIT;
    vm.decommit = 0;
    vm.fixed_remap = fault_vm_fixed_remap;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "fixed remap heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "fixed remap api");
    expect_ok(api->mmap(heap, &map, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "fixed remap mmap");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "fixed remap stats before failure");
    state.fail_fixed_remap = 1;
    expect_true(api->munmap(heap, map, page_size) == UHM_ERR_PLATFORM,
                "fixed remap failure reported");
    state.fail_fixed_remap = 0;
    {
        uhm_stats failed_stats;
        memset(&failed_stats, 0, sizeof(failed_stats));
        failed_stats.size = sizeof(failed_stats);
        expect_ok(api->get_stats(heap, &failed_stats), "fixed remap stats after failure");
        expect_true(stats.decommitted_bytes == failed_stats.decommitted_bytes,
                    "failed fixed remap does not publish decommit stats");
        expect_true(stats.logical_decommitted_bytes == failed_stats.logical_decommitted_bytes,
                    "failed fixed remap does not publish logical stats");
        expect_true(stats.physical_released_bytes == failed_stats.physical_released_bytes,
                    "failed fixed remap does not publish physical stats");
    }
    {
        void *conflict = map;
        expect_true(api->mmap(heap, &conflict, page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                              UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                        UHM_ERR_ALREADY_MAPPED,
                    "fixed remap failure keeps mapping live");
    }
    expect_ok(api->munmap(heap, map, page_size), "fixed remap cleanup");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "fixed remap stats");
    expect_true(state.fixed_remap_count >= 2, "fixed remap callback used");
    expect_true(stats.logical_decommitted_bytes >= page_size,
                "fixed remap usermode counted as logical");
    expect_true(stats.physical_released_bytes == 0,
                "fixed remap usermode does not count physical release");
    expect_ok(api->check(heap, 0), "fixed remap check");
    expect_ok(api->close(heap), "fixed remap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

