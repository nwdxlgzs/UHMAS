static size_t test_vma_tree_depth(const uhm_vma *node) {
    size_t left_depth;
    size_t right_depth;

    if (!node) {
        return 0;
    }
    left_depth = test_vma_tree_depth(node->tree_left);
    right_depth = test_vma_tree_depth(node->tree_right);
    return (left_depth > right_depth ? left_depth : right_depth) + 1u;
}

static size_t test_vma_tree_count(const uhm_vma *node) {
    if (!node) {
        return 0;
    }
    return 1u + test_vma_tree_count(node->tree_left) +
           test_vma_tree_count(node->tree_right);
}

static void test_vma_tree_ordered_fixed_mmap_pressure(
    const uhm_storage_info *heap_info) {
    enum { map_count = 96 };
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uintptr_t base;
    void *maps[map_count];
    size_t tree_count;
    size_t tree_depth;
    size_t i;
    uint64_t saved_priority;
    uint64_t saved_salt;
    uhm_vma *saved_left;
    uintptr_t guard_addr;
    uint64_t find_budget_before;
    uint64_t treap_health_before;

    memset(maps, 0, sizeof(maps));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "vma tree heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "vma tree arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "vma tree usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "vma tree heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "vma tree api");

    base = (heap->core.control_end + (uintptr_t)page_size - 1u) &
           ~((uintptr_t)page_size - 1u);
    base += (uintptr_t)(page_size * 8u);
    expect_true(base + (uintptr_t)(map_count * 2u) * page_size <
                    heap->core.arena_end,
                "vma tree pressure range fits arena");

    for (i = 0; i < map_count; ++i) {
        void *addr = (void *)(base + (uintptr_t)i * 2u * page_size);
        expect_ok(api->mmap(heap, &addr, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
                  "vma tree fixed mmap");
        expect_true(addr == (void *)(base + (uintptr_t)i * 2u * page_size),
                    "vma tree fixed mmap address");
        maps[i] = addr;
    }
    expect_ok(api->check(heap, 0), "vma tree check after ordered maps");

    tree_count = test_vma_tree_count(heap->core.vma_tree_root);
    tree_depth = test_vma_tree_depth(heap->core.vma_tree_root);
    expect_true(tree_count == map_count, "vma tree pressure count");
    expect_true(tree_depth <= 48u, "vma tree treap avoids ordered chain");
    expect_true(heap->core.vma_tree_insertions >= map_count,
                "vma tree diagnostics insertions counted");
    expect_true(heap->core.vma_tree_max_insert_depth != 0,
                "vma tree diagnostics max insert depth counted");
    expect_true(heap->core.vma_tree_current_nodes == tree_count,
                "vma tree tracked node count");
    expect_true(heap->core.vma_tree_max_nodes >= tree_count,
                "vma tree max node count");
    expect_true(heap->core.vma_tree_depth_limit_exceeded == 0,
                "vma tree depth limit not exceeded");
    expect_true(heap->core.vma_tree_priority_bucket_mask != 0,
                "vma tree priority bucket mask counted");
    expect_true(test_popcount_u64(heap->core.vma_tree_priority_bucket_mask) >=
                    4u,
                "vma tree priority buckets distributed");
    expect_true(heap->core.vma_tree_priority_bucket_max != 0,
                "vma tree priority bucket max counted");
    expect_true(heap->core.vma_tree_insert_budget_exceeded == 0,
                "vma tree insert budget not exceeded");
    expect_true(heap->core.vma_tree_insert_rotation_budget_exceeded == 0,
                "vma tree insert rotation budget not exceeded");

    expect_true(heap->core.vma_tree_root != 0,
                "vma tree find guard corruption target");
    guard_addr = heap->core.vma_tree_root->addr - (uintptr_t)page_size;
    expect_true(guard_addr >= heap->core.malloc_hi,
                "vma tree find guard address in mmap half");
    saved_left = heap->core.vma_tree_root->tree_left;
    find_budget_before = heap->core.vma_tree_find_budget_exceeded;
    treap_health_before = heap->core.treap_health_flags;
    heap->core.vma_tree_root->tree_left = heap->core.vma_tree_root;
    expect_true(api->mprotect(heap, (void *)guard_addr, page_size,
                              UHM_PROT_READ) != UHM_OK,
                "vma tree find guard returns instead of hanging");
    expect_true(heap->core.vma_tree_find_budget_exceeded >
                    find_budget_before,
                "vma tree find guard budget counted");
    expect_true((heap->core.treap_health_flags &
                 UHM_TREAP_HEALTH_VMA_FIND) != 0,
                "vma tree find guard health flag set");
    expect_true(heap->core.treap_health_flags != treap_health_before,
                "vma tree health flag is sticky");
    heap->core.vma_tree_root->tree_left = saved_left;
    expect_ok(api->check(heap, 0), "vma tree find guard restored");
    expect_true(api->check(heap, UHM_CHECK_FAIL_ON_TREAP_HEALTH) ==
                    UHM_ERR_CORRUPTION,
                "vma tree strict health check fails after guard");

    expect_true(heap->core.vma_tree_root != 0,
                "vma tree priority cache corruption target");
    expect_true(heap->core.treap_priority_salt != 0,
                "vma tree priority salt initialized");
    saved_salt = heap->core.treap_priority_salt;
    heap->core.treap_priority_salt ^= UINT64_C(0x9e3779b97f4a7c15);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "vma tree priority salt mismatch detected");
    heap->core.treap_priority_salt = saved_salt;
    expect_ok(api->check(heap, 0), "vma tree priority salt restored");
    saved_priority = heap->core.vma_tree_root->tree_priority;
    heap->core.vma_tree_root->tree_priority ^= UINT64_C(1);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "vma tree cached priority corruption detected");
    heap->core.vma_tree_root->tree_priority = saved_priority;
    expect_ok(api->check(heap, 0),
              "vma tree cached priority corruption restored");

    for (i = 0; i < map_count; ++i) {
        expect_ok(api->munmap(heap, maps[i], page_size),
                  "vma tree pressure munmap");
    }
    expect_ok(api->check(heap, 0), "vma tree final check");
    expect_ok(api->close(heap), "vma tree close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static size_t test_mmap_free_tree_depth(const uhm_free_extent *node) {
    size_t left_depth;
    size_t right_depth;

    if (!node) {
        return 0;
    }
    left_depth = test_mmap_free_tree_depth(node->tree_left);
    right_depth = test_mmap_free_tree_depth(node->tree_right);
    return (left_depth > right_depth ? left_depth : right_depth) + 1u;
}

static size_t test_mmap_free_tree_count(const uhm_free_extent *node) {
    if (!node) {
        return 0;
    }
    return 1u + test_mmap_free_tree_count(node->tree_left) +
           test_mmap_free_tree_count(node->tree_right);
}

static void test_mmap_free_tree_ordered_munmap_pressure(
    const uhm_storage_info *heap_info) {
    enum { free_count = 96 };
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *base = 0;
    void *map = 0;
    void *reuse = 0;
    size_t mapped_size = (size_t)free_count * 2u * page_size;
    size_t tree_count;
    size_t tree_depth;
    size_t i;
    uint64_t saved_priority;
    uhm_free_extent *saved_tree_right;
    void *guard_reuse = 0;
    uhm_result guard_result;
    uint64_t find_budget_before;
    uint64_t treap_health_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "mmap free tree heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mmap free tree arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mmap free tree usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "mmap free tree heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mmap free tree api");

    expect_ok(api->mmap(heap, &map, mapped_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap free tree initial map");
    expect_true(map != 0, "mmap free tree initial map address");
    base = (unsigned char *)map;

    for (i = 0; i < free_count; ++i) {
        expect_ok(api->munmap(heap, base + i * 2u * page_size, page_size),
                  "mmap free tree ordered munmap");
    }
    expect_ok(api->check(heap, 0), "mmap free tree check after ordered munmap");

    tree_count = test_mmap_free_tree_count(heap->core.mmap_free_tree_root);
    tree_depth = test_mmap_free_tree_depth(heap->core.mmap_free_tree_root);
    expect_true(tree_count == free_count, "mmap free tree pressure count");
    expect_true(tree_depth <= 48u, "mmap free tree treap avoids ordered chain");
    expect_true(heap->core.mmap_free_tree_insertions >= free_count,
                "mmap free tree diagnostics insertions counted");
    expect_true(heap->core.mmap_free_tree_max_insert_depth != 0,
                "mmap free tree diagnostics max insert depth counted");
    expect_true(heap->core.mmap_free_tree_current_nodes == tree_count,
                "mmap free tree tracked node count");
    expect_true(heap->core.mmap_free_tree_max_nodes >= tree_count,
                "mmap free tree max node count");
    expect_true(heap->core.mmap_free_tree_depth_limit_exceeded == 0,
                "mmap free tree depth limit not exceeded");
    expect_true(heap->core.mmap_free_tree_priority_bucket_mask != 0,
                "mmap free tree priority bucket mask counted");
    expect_true(test_popcount_u64(
                    heap->core.mmap_free_tree_priority_bucket_mask) >= 4u,
                "mmap free tree priority buckets distributed");
    expect_true(heap->core.mmap_free_tree_priority_bucket_max != 0,
                "mmap free tree priority bucket max counted");
    expect_true(heap->core.mmap_free_tree_insert_budget_exceeded == 0,
                "mmap free tree insert budget not exceeded");
    expect_true(heap->core.mmap_free_tree_insert_rotation_budget_exceeded == 0,
                "mmap free tree insert rotation budget not exceeded");
    expect_true(heap->core.mmap_free_tree_root == 0 ||
                    heap->core.mmap_free_tree_root->tree_max_size == page_size,
                "mmap free tree max-size summary");

    expect_true(heap->core.mmap_free_tree_root != 0,
                "mmap free tree find guard corruption target");
    saved_tree_right = heap->core.mmap_free_tree_root->tree_right;
    find_budget_before = heap->core.mmap_free_tree_find_budget_exceeded;
    heap->core.mmap_recent_free_extent = 0;
    memset(heap->core.mmap_recent_free_cache, 0,
           sizeof(heap->core.mmap_recent_free_cache));
    heap->core.mmap_recent_free_next = 0;
    memset(heap->core.mmap_size_hint, 0, sizeof(heap->core.mmap_size_hint));
    memset(heap->core.mmap_size_hint_bucket_max, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max));
    memset(heap->core.mmap_size_hint_bucket_max_extent, 0,
           sizeof(heap->core.mmap_size_hint_bucket_max_extent));
    heap->core.mmap_free_tree_root->tree_right =
        heap->core.mmap_free_tree_root;
    treap_health_before = heap->core.treap_health_flags;
    guard_result = api->mmap(heap, &guard_reuse, page_size,
                             UHM_PROT_READ | UHM_PROT_WRITE,
                             UHM_MAP_PRIVATE);
    expect_true(guard_result == UHM_OK ||
                    guard_result == UHM_ERR_OUT_OF_ADDRESS_SPACE ||
                    guard_result == UHM_ERR_OUT_OF_MEMORY,
                "mmap free tree find guard returns instead of hanging");
    expect_true(heap->core.mmap_free_tree_find_budget_exceeded >
                    find_budget_before,
                "mmap free tree find guard budget counted");
    expect_true((heap->core.treap_health_flags &
                 UHM_TREAP_HEALTH_MMAP_FREE_FIND) != 0,
                "mmap free tree find guard health flag set");
    expect_true(heap->core.treap_health_flags != treap_health_before,
                "mmap free tree health flag is sticky");
    heap->core.mmap_free_tree_root->tree_right = saved_tree_right;
    expect_ok(api->check(heap, 0), "mmap free tree find guard restored");
    expect_true(api->check(heap, UHM_CHECK_FAIL_ON_TREAP_HEALTH) ==
                    UHM_ERR_CORRUPTION,
                "mmap free tree strict health check fails after guard");

    expect_true(heap->core.mmap_free_tree_root != 0,
                "mmap free tree priority cache corruption target");
    saved_priority = heap->core.mmap_free_tree_root->tree_priority;
    heap->core.mmap_free_tree_root->tree_priority ^= UINT64_C(1);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "mmap free tree cached priority corruption detected");
    heap->core.mmap_free_tree_root->tree_priority = saved_priority;
    expect_ok(api->check(heap, 0),
              "mmap free tree cached priority corruption restored");

    expect_ok(api->mmap(heap, &reuse, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap free tree auto reuse");
    expect_true(reuse == base + (free_count - 1u) * 2u * page_size,
                "mmap free tree auto reuses highest fit");
    expect_ok(api->munmap(heap, reuse, page_size),
              "mmap free tree reuse cleanup");
    if (guard_reuse) {
        expect_ok(api->munmap(heap, guard_reuse, page_size),
                  "mmap free tree guard cleanup");
    }
    for (i = 0; i < free_count; ++i) {
        expect_ok(api->munmap(heap, base + (i * 2u + 1u) * page_size,
                              page_size),
                  "mmap free tree live page cleanup");
    }
    expect_ok(api->check(heap, 0), "mmap free tree final check");
    expect_ok(api->close(heap), "mmap free tree close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

