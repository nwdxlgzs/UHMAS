typedef struct mprotect_restore_batch_state {
    uhm_heap *heap;
    size_t corrupt_page_index;
    int corrupt_next_protect;
    unsigned protect_calls_after_arm;
    unsigned restore_calls_after_corruption;
    void *last_restore_addr;
    size_t last_restore_size;
    uint32_t last_restore_prot;
} mprotect_restore_batch_state;

static uhm_result UHM_CALL mprotect_restore_batch_vm_protect(
    void *user, void *addr, size_t size, uint32_t prot) {
    mprotect_restore_batch_state *state =
        (mprotect_restore_batch_state *)user;

    state->protect_calls_after_arm++;
    if (state->corrupt_next_protect) {
        uhm_page_record *record =
            test_page_record_at(state->heap, state->corrupt_page_index);
        state->corrupt_next_protect = 0;
        if (record) {
            record->va_state = UHM_PAGE_VA_FREE_RESERVED;
        }
        return UHM_OK;
    }

    state->restore_calls_after_corruption++;
    state->last_restore_addr = addr;
    state->last_restore_size = size;
    state->last_restore_prot = prot;
    return UHM_OK;
}

static void test_mprotect_restore_platform_batches_same_old_prot(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    mprotect_restore_batch_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *high_map = 0;
    void *low_map = 0;
    uintptr_t high_addr;
    uintptr_t low_addr;
    size_t low_page_index;
    size_t low_page_count;
    uint64_t noop_before;
    uint64_t page_query_calls_before;
    uint64_t page_query_pages_before;
    uint64_t rollback_before;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mprotect restore batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mprotect restore batch arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mprotect restore batch vm init");
    vm.user = &state;
    vm.protect = mprotect_restore_batch_vm_protect;

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

    expect_ok(uhm_open(&config, &heap), "mprotect restore batch open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mprotect restore batch api");

    expect_ok(api->mmap(heap, &high_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mprotect restore batch high mmap");
    high_addr = (uintptr_t)high_map;
    expect_true(high_addr >= heap->core.arena_base + page_size,
                "mprotect restore batch has lower neighbor space");
    low_addr = high_addr - page_size;
    low_map = (void *)low_addr;
    expect_ok(api->mmap(heap, &low_map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_PAGE_PRIVATE |
                            UHM_MAP_FIXED_NOREPLACE),
              "mprotect restore batch low fixed mmap");
    expect_true((uintptr_t)low_map == low_addr,
                "mprotect restore batch low fixed address");

    expect_true(test_shadow_page_span(heap, low_addr, page_size,
                                      &low_page_index, &low_page_count) != 0 &&
                    low_page_count == 1u,
                "mprotect restore batch page span");

    noop_before = heap->core.mprotect_mmap_noop_calls;
    page_query_calls_before =
        test_atomic_u64_load(&heap->core.page_range_query_calls);
    page_query_pages_before =
        test_atomic_u64_load(&heap->core.page_range_query_pages);
    expect_ok(api->mprotect(heap, low_map, page_size * 2u,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "mprotect restore batch multi VMA no-op");
    expect_true(heap->core.mprotect_mmap_noop_calls == noop_before + 1u,
                "mprotect restore batch multi VMA no-op counted");
    expect_true(state.protect_calls_after_arm == 0u,
                "mprotect restore batch multi VMA no-op skips platform");
    expect_true(test_atomic_u64_load(&heap->core.page_range_query_calls) ==
                    page_query_calls_before + 3u &&
                    test_atomic_u64_load(&heap->core.page_range_query_pages) ==
                        page_query_pages_before + 4u,
                "mprotect restore batch multi VMA no-op reuses collect proof");

    state.heap = heap;
    state.corrupt_page_index = low_page_index;
    state.corrupt_next_protect = 1;
    state.protect_calls_after_arm = 0;
    rollback_before = heap->core.mprotect_rollback_calls;

    expect_true(api->mprotect(heap, low_map, page_size * 2u, UHM_PROT_READ) ==
                    UHM_ERR_CORRUPTION,
                "mprotect restore batch detects revalidate corruption");
    expect_true(heap->core.mprotect_rollback_calls == rollback_before + 1u,
                "mprotect restore batch rollback counted");
    expect_true(state.protect_calls_after_arm == 2u,
                "mprotect restore batch one platform protect plus one restore");
    expect_true(state.restore_calls_after_corruption == 1u,
                "mprotect restore batch coalesces adjacent old prot restore");
    expect_true(state.last_restore_addr == low_map &&
                    state.last_restore_size == page_size * 2u &&
                    state.last_restore_prot ==
                        (UHM_PROT_READ | UHM_PROT_WRITE),
                "mprotect restore batch restores combined old range");
    expect_ok(api->check(heap, 0), "mprotect restore batch check after rollback");
    expect_ok(api->munmap(heap, low_map, page_size * 2u),
              "mprotect restore batch cleanup munmap");
    expect_ok(api->close(heap), "mprotect restore batch close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mprotect_vma_pass_rollback(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t page_count = 4u;
    const size_t map_size = page_count * page_size;
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    uintptr_t base;
    uint64_t restore_before;
    uint64_t rollback_before;
    uint64_t generation_before;

    memset(&state, 0, sizeof(state));
    state.platform_error = test_platform_error_code();
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mprotect VMA rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mprotect VMA rollback arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mprotect VMA rollback vm init");
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
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "mprotect VMA rollback open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "mprotect VMA rollback api");
    expect_ok(api->mmap(heap, &map, map_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mprotect VMA rollback mmap");
    expect_true(map != 0, "mprotect VMA rollback address");
    base = (uintptr_t)map;

    expect_ok(api->mprotect(heap, (void *)base, page_size, UHM_PROT_READ),
              "mprotect VMA rollback split page 0");
    expect_ok(api->mprotect(heap, (void *)(base + page_size), page_size,
                            UHM_PROT_NONE),
              "mprotect VMA rollback split page 1");
    expect_ok(api->mprotect(heap, (void *)(base + 2u * page_size), page_size,
                            UHM_PROT_READ),
              "mprotect VMA rollback split page 2");
    expect_ok(api->mprotect(heap, (void *)(base + 3u * page_size), page_size,
                            UHM_PROT_NONE),
              "mprotect VMA rollback split page 3");

    restore_before = heap->core.mprotect_vma_restore_queries;
    rollback_before = heap->core.mprotect_rollback_calls;
    state.fail_protect = 1;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mprotect(heap, map, map_size,
                              UHM_PROT_READ | UHM_PROT_WRITE) ==
                    UHM_ERR_PLATFORM,
                "mprotect VMA rollback protect failure");
    expect_generation_advanced(heap, generation_before,
                               "mprotect VMA rollback generation advances");
    expect_true(heap->core.mprotect_rollback_calls == rollback_before + 1u,
                "mprotect VMA rollback call counted");
    expect_true(heap->core.mprotect_vma_restore_queries == restore_before,
                "mprotect VMA rollback restore reuses collected cursor");
    expect_last_error(api, heap, UHM_ERR_PLATFORM, state.platform_error,
                      "mprotect VMA rollback captures platform error");

    state.fail_protect = 0;
    expect_ok(api->check(heap, 0), "mprotect VMA rollback check after failure");
    expect_ok(api->mprotect(heap, map, map_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "mprotect VMA rollback retry succeeds");
    expect_ok(api->munmap(heap, map, map_size), "mprotect VMA rollback munmap");
    expect_ok(api->close(heap), "mprotect VMA rollback close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

