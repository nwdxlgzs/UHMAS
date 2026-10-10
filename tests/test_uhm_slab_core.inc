static void test_leak_enumeration(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *malloc_ptr;
    void *page_private_ptr;
    void *map = 0;
    leak_collect_state leaks;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "leak enum heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "leak enum arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "leak enum usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "leak enum heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->enumerate_leaks != 0, "leak enum api");

    malloc_ptr = api->malloc(heap, 123u);
    page_private_ptr = api->aligned_alloc(heap, page_size, page_size);
    expect_true(malloc_ptr != 0, "leak enum malloc");
    expect_true(page_private_ptr != 0, "leak enum page-private malloc");
    expect_ok(api->mmap(heap, &map, page_size * 2u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "leak enum mmap");

    memset(&leaks, 0, sizeof(leaks));
    expect_ok(api->enumerate_leaks(heap, collect_leak, &leaks), "leak enum collect");
    expect_true(leaks.total_count == 3u, "leak enum total count");
    expect_true(leaks.malloc_count == 2u, "leak enum malloc count");
    expect_true(leaks.mmap_count == 1u, "leak enum mmap count");
    expect_true(leaks.page_private_count == 1u, "leak enum page-private count");
    expect_true(leaks.seen_malloc != 0 && leaks.seen_mmap == map,
                "leak enum records addresses");

    memset(&leaks, 0, sizeof(leaks));
    leaks.callback_limit = 1u;
    expect_true(api->enumerate_leaks(heap, collect_leak, &leaks) == UHM_ERR_UNSUPPORTED,
                "leak enum propagates callback stop");
    expect_true(leaks.total_count == 1u, "leak enum stops after callback error");

    api->free(heap, malloc_ptr);
    api->free(heap, page_private_ptr);
    expect_ok(api->munmap(heap, map, page_size * 2u), "leak enum munmap");
    memset(&leaks, 0, sizeof(leaks));
    expect_ok(api->enumerate_leaks(heap, collect_leak, &leaks), "leak enum empty collect");
    expect_true(leaks.total_count == 0, "leak enum empty after cleanup");
    expect_ok(api->check(heap, 0), "leak enum heap check");
    expect_ok(api->close(heap), "leak enum heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static uint64_t test_leak_live_lookup_call_sum(uhm_heap *heap) {
    uint64_t sum = 0;
    size_t i;

    for (i = 0; i < UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT; ++i) {
        sum += heap->core.live_block_lookup_calls[i];
    }
    return sum;
}

static void test_leak_enumeration_uses_live_tree(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    const size_t large_size = page_size * 2u + 128u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *head_ptr;
    void *gap_ptr;
    void *tail_ptr;
    uhm_block *head_block;
    uhm_block *gap_block;
    uhm_block *tail_block;
    uhm_block *saved_gap_next;
    leak_collect_state leaks;
    uint64_t lookup_calls_before;
    uint64_t fallback_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "leak tree heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "leak tree arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "leak tree usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.malloc_free_decommit_retention_pages = 1u;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "leak tree heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->enumerate_leaks != 0, "leak tree api");

    head_ptr = api->malloc(heap, large_size);
    gap_ptr = api->malloc(heap, large_size);
    tail_ptr = api->malloc(heap, large_size);
    expect_true(head_ptr != 0 && gap_ptr != 0 && tail_ptr != 0,
                "leak tree allocations");
    head_block = test_find_block_by_payload(heap, head_ptr);
    gap_block = test_find_block_by_payload(heap, gap_ptr);
    tail_block = test_find_block_by_payload(heap, tail_ptr);
    expect_true(head_block != 0 && gap_block != 0 && tail_block != 0,
                "leak tree block lookup");
    expect_true(head_block->next_phys == gap_block &&
                    gap_block->next_phys == tail_block,
                "leak tree physical order");
    api->free(heap, gap_ptr);
    expect_true((gap_block->flags & UHM_BLOCK_FLAG_FREE) != 0,
                "leak tree gap freed");
    expect_true(heap->core.live_block_tree_current_nodes == 2u,
                "leak tree tracks two live blocks");
    expect_ok(api->check(heap, 0), "leak tree check before perturb");

    saved_gap_next = gap_block->next_phys;
    lookup_calls_before = test_leak_live_lookup_call_sum(heap);
    fallback_before = heap->core.live_block_slow_fallback_scans;
    gap_block->next_phys = 0;
    memset(&leaks, 0, sizeof(leaks));
    expect_ok(api->enumerate_leaks(heap, collect_leak, &leaks),
              "leak tree collect through broken free chain");
    gap_block->next_phys = saved_gap_next;
    expect_true(leaks.total_count == 2u && leaks.malloc_count == 2u,
                "leak tree enumerates all live malloc blocks");
    expect_true(test_leak_live_lookup_call_sum(heap) == lookup_calls_before,
                "leak tree enumeration avoids live lookup calls");
    expect_true(heap->core.live_block_slow_fallback_scans == fallback_before,
                "leak tree enumeration avoids physical fallback scan");

    api->free(heap, head_ptr);
    api->free(heap, tail_ptr);
    expect_ok(api->check(heap, 0), "leak tree final check");
    expect_ok(api->close(heap), "leak tree close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_malloc_physical_chain_check_guard(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 1024u * 1024u;
    const size_t large_size = page_size * 2u + 128u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *head_ptr;
    void *tail_ptr;
    uhm_block *head_block;
    uhm_block *tail_block;
    uhm_block *saved_tail_next;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "malloc physical guard heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "malloc physical guard arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "malloc physical guard usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "malloc physical guard heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "malloc physical guard api");

    head_ptr = api->malloc(heap, large_size);
    tail_ptr = api->malloc(heap, large_size);
    expect_true(head_ptr != 0 && tail_ptr != 0,
                "malloc physical guard allocations");
    head_block = test_find_block_by_payload(heap, head_ptr);
    tail_block = test_find_block_by_payload(heap, tail_ptr);
    expect_true(head_block != 0 && tail_block != 0 &&
                    head_block->next_phys == tail_block &&
                    tail_block->prev_phys == head_block,
                "malloc physical guard physical order");
    expect_ok(api->check(heap, 0), "malloc physical guard check before");

    saved_tail_next = tail_block->next_phys;
    tail_block->next_phys = head_block;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "malloc physical guard detects next cycle");
    tail_block->next_phys = saved_tail_next;
    expect_ok(api->check(heap, 0), "malloc physical guard check restored");

    api->free(heap, head_ptr);
    api->free(heap, tail_ptr);
    expect_ok(api->check(heap, 0), "malloc physical guard final check");
    expect_ok(api->close(heap), "malloc physical guard close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_state_transitions(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *first;
    void *reuse;
    void *remote_free_a;
    void *remote_free_b;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uint16_t size_class;
    size_t slot_count;
    size_t i;
    uintptr_t free_head_ref_before = 0;
    uint32_t state_mirror_before = 0;
    size_t live_count_before = 0;
    size_t free_count_before = 0;
    uint64_t metadata_epoch_before = 0;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t raw_lock_calls_before;
    uint64_t heap_try_lock_calls_before = 0;
#endif

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab state heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "slab state arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab state usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "slab state heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab state api");

    first = api->malloc(heap, 1u);
    expect_true(first != 0, "slab state first allocation");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    size_class = slab->size_class;
    slot_count = slab->total_slots;
    expect_true(slot->magic == UHM_SLAB_SLOT_MAGIC && slab->magic == UHM_SLAB_MAGIC,
                "slab state first slot metadata");
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL, "slab state first partial");
    expect_true(heap->core.slab_class_heads[size_class] == slab,
                "slab state partial head");
    expect_true(heap->core.slab_full_heads[size_class] == 0,
                "slab state full empty before fill");
    expect_true(heap->core.slab_empty_heads[size_class] == 0,
                "slab state empty empty before fill");
    expect_ok(api->check(heap, 0), "slab state check after first");

    ptrs = (void **)calloc(slot_count, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab state pointer table");
    ptrs[0] = first;
    expect_true(slot_count > 1u, "slab state has reusable partial slots");
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_spinlock_set_diagnostics_enabled(&slab->raw_lock, 1u);
    raw_lock_calls_before =
        uhm_diag_counter_load_relaxed(&slab->raw_lock.lock_calls);
#endif
    ptrs[1] = api->malloc(heap, 1u);
    expect_true(ptrs[1] != 0, "slab state second allocation");
    expect_true(test_slab_slot_from_payload(ptrs[1])->slab == slab,
                "slab state second allocation reuses partial slab");
#if UHMAS_ENABLE_DIAGNOSTICS
    expect_true(
        uhm_diag_counter_load_relaxed(&slab->raw_lock.lock_calls) ==
            raw_lock_calls_before + 1u,
        "slab state partial selection uses atomic free-head hint");
#endif
    for (i = 2; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 1u);
        expect_true(ptrs[i] != 0, "slab state fill allocation");
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == slab,
                    "slab state fills same slab");
    }
    expect_true(slab->state == UHM_SLAB_STATE_FULL, "slab state full after fill");
    expect_true(heap->core.slab_class_heads[size_class] == 0,
                "slab state no partial when full");
    expect_true(heap->core.slab_full_heads[size_class] == slab,
                "slab state full head");
    expect_true(heap->core.slab_empty_heads[size_class] == 0,
                "slab state no empty when full");
    expect_true(heap->core.slab_empty_count[size_class] == 0,
                "slab state empty count zero when full");
    expect_true(heap->core.slab_committed_empty_count[size_class] == 0,
                "slab state committed empty count zero when full");
    expect_ok(api->check(heap, 0), "slab state check after full");

    remote_free_a = ptrs[0];
    remote_free_b = ptrs[1];
    api->free(heap, ptrs[0]);
    if (uhm_tagged_freelist_is_supported()) {
        state_mirror_before = atomic_load_explicit(&slab->state_mirror,
                                                   UHM_ATOMIC_ACQUIRE);
        free_head_ref_before = atomic_load_explicit(&slab->free_head_ref,
                                                    UHM_ATOMIC_ACQUIRE);
        live_count_before = atomic_load_explicit(&slab->live_slots_count,
                                                 UHM_ATOMIC_ACQUIRE);
        free_count_before = atomic_load_explicit(&slab->free_slots_count,
                                                 UHM_ATOMIC_ACQUIRE);
        expect_true(atomic_load_explicit(
                        &heap->core.metadata_reclaim_pending,
                        UHM_ATOMIC_ACQUIRE) == 0u,
                    "slab state no metadata reclaim pending");
        metadata_epoch_before = heap->core.metadata_epoch;
#if UHMAS_ENABLE_DIAGNOSTICS
        heap_try_lock_calls_before =
            uhm_diag_counter_load_relaxed(&heap->lock.try_calls);
#endif
    }
    api->free(heap, ptrs[1]);
    ptrs[0] = 0;
    ptrs[1] = 0;
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->free_slots == 2u &&
                    test_slab_remote_pending(slab) == 2u,
                "slab state partial after two remote frees");
    expect_true(heap->core.slab_class_heads[size_class] == slab,
                "slab state partial head after two frees");
    expect_true(heap->core.slab_full_heads[size_class] == 0,
                "slab state no full after two frees");
    expect_ok(api->check(heap, 0), "slab state check after two frees");
    if (uhm_tagged_freelist_is_supported()) {
        expect_true(atomic_load_explicit(&slab->state_mirror,
                                         UHM_ATOMIC_ACQUIRE) ==
                        state_mirror_before &&
                        state_mirror_before == UHM_SLAB_STATE_PARTIAL &&
                        atomic_load_explicit(&slab->free_head_ref,
                                             UHM_ATOMIC_ACQUIRE) ==
                            free_head_ref_before,
                    "slab state partial remote free keeps cold mirrors");
        expect_true(atomic_load_explicit(&slab->live_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                            live_count_before - 1u &&
                        atomic_load_explicit(&slab->free_slots_count,
                                             UHM_ATOMIC_ACQUIRE) ==
                            free_count_before + 1u,
                    "slab state partial remote free publishes counts");
        expect_true(heap->core.metadata_epoch == metadata_epoch_before,
                    "slab state partial free skips idle metadata epoch");
#if UHMAS_ENABLE_DIAGNOSTICS
        expect_true(
            uhm_diag_counter_load_relaxed(&heap->lock.try_calls) ==
                heap_try_lock_calls_before,
            "slab state partial free skips idle metadata try lock");
#endif
    }

#if UHMAS_ENABLE_DIAGNOSTICS
    if (uhm_tagged_freelist_is_supported()) {
        raw_lock_calls_before =
            uhm_diag_counter_load_relaxed(&slab->raw_lock.lock_calls);
    }
#endif
    if (uhm_tagged_freelist_is_supported()) {
        state_mirror_before = atomic_load_explicit(&slab->state_mirror,
                                                   UHM_ATOMIC_ACQUIRE);
        free_head_ref_before = atomic_load_explicit(&slab->free_head_ref,
                                                    UHM_ATOMIC_ACQUIRE);
        live_count_before = atomic_load_explicit(&slab->live_slots_count,
                                                 UHM_ATOMIC_ACQUIRE);
        free_count_before = atomic_load_explicit(&slab->free_slots_count,
                                                 UHM_ATOMIC_ACQUIRE);
        metadata_epoch_before = heap->core.metadata_epoch;
#if UHMAS_ENABLE_DIAGNOSTICS
        heap_try_lock_calls_before =
            uhm_diag_counter_load_relaxed(&heap->lock.try_calls);
#endif
    }
    ptrs[0] = api->malloc(heap, 1u);
    expect_true(ptrs[0] == remote_free_a || ptrs[0] == remote_free_b,
                "slab state partial remote reuse");
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->free_slots == 1u,
                "slab state remains partial after first remote reuse");
    if (uhm_tagged_freelist_is_supported()) {
        expect_true(atomic_load_explicit(&slab->state_mirror,
                                         UHM_ATOMIC_ACQUIRE) ==
                        state_mirror_before &&
                        state_mirror_before == UHM_SLAB_STATE_PARTIAL &&
                        atomic_load_explicit(&slab->free_head_ref,
                                             UHM_ATOMIC_ACQUIRE) ==
                            free_head_ref_before,
                    "slab state partial remote reuse keeps cold mirrors");
        expect_true(atomic_load_explicit(&slab->live_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                            live_count_before + 1u &&
                        atomic_load_explicit(&slab->free_slots_count,
                                             UHM_ATOMIC_ACQUIRE) ==
                            free_count_before - 1u,
                    "slab state partial remote reuse publishes counts");
        expect_true(heap->core.metadata_epoch == metadata_epoch_before,
                    "slab state partial reuse skips idle metadata epoch");
#if UHMAS_ENABLE_DIAGNOSTICS
        expect_true(
            uhm_diag_counter_load_relaxed(&heap->lock.try_calls) ==
                heap_try_lock_calls_before,
            "slab state partial reuse skips idle metadata try lock");
#endif
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    if (uhm_tagged_freelist_is_supported()) {
        expect_true(
            uhm_diag_counter_load_relaxed(&slab->raw_lock.lock_calls) ==
                raw_lock_calls_before + 1u,
            "slab state remote reuse hint avoids classification raw lock");
    }
#endif
    ptrs[1] = api->malloc(heap, 1u);
    expect_true((ptrs[1] == remote_free_a || ptrs[1] == remote_free_b) &&
                    ptrs[1] != ptrs[0],
                "slab state second remote reuse");
    expect_true(slab->state == UHM_SLAB_STATE_FULL &&
                    slab->free_slots == 0,
                "slab state restored full after remote reuse");
    expect_ok(api->check(heap, 0), "slab state check after remote reuse");

    for (i = 0; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
    }
    expect_true(slab->state == UHM_SLAB_STATE_EMPTY &&
                    slab->live_slots == 0 &&
                    test_slab_remote_pending(slab) != 0,
                "slab state empty after all free");
    expect_true(heap->core.slab_empty_heads[size_class] == slab,
                "slab state empty head after all free");
    expect_true(heap->core.slab_committed_empty_count[size_class] == 1u,
                "slab state committed empty count after all free");
    expect_ok(api->check(heap, 0), "slab state check after all remote free");

    reuse = api->malloc(heap, 1u);
    expect_true(reuse != 0, "slab state reuse allocation");
    expect_true(test_slab_slot_from_payload(reuse)->slab == slab,
                "slab state reuses empty slab");
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL,
                "slab state empty becomes partial on reuse");
    api->free(heap, reuse);

    free(ptrs);
    expect_ok(api->check(heap, 0), "slab state final check");
    expect_ok(api->close(heap), "slab state heap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_multi_state_lists(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *reuse;
    void *p512;
    void *p513;
    uhm_slab *slab_a;
    uhm_slab *slab_b = 0;
    uhm_slab *reuse_slab;
    uhm_slab *remaining_slab;
    uhm_stats stats;
    uint16_t size_class;
    size_t slot_count;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab multi heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "slab multi arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab multi usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "slab multi heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab multi api");

    ptrs = 0;
    reuse = 0;
    ptrs = (void **)calloc(256u, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab multi pointer table initial");

    ptrs[0] = api->malloc(heap, 1u);
    expect_true(ptrs[0] != 0, "slab multi first allocation");
    slab_a = test_find_slab_by_payload(heap, ptrs[0]);
    expect_true(slab_a != 0, "slab multi first slab");
    size_class = slab_a->size_class;
    slot_count = slab_a->total_slots;
    expect_true(slot_count * 2u <= 256u, "slab multi pointer table capacity");

    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 1u);
        expect_true(ptrs[i] != 0, "slab multi fill first slab");
        expect_true(test_find_slab_by_payload(heap, ptrs[i]) == slab_a,
                    "slab multi first slab ownership");
    }
    expect_true(slab_a->state == UHM_SLAB_STATE_FULL,
                "slab multi first slab full");
    expect_true(heap->core.slab_full_heads[size_class] == slab_a,
                "slab multi first full head");

    for (i = slot_count; i < slot_count * 2u; ++i) {
        ptrs[i] = api->malloc(heap, 1u);
        expect_true(ptrs[i] != 0, "slab multi fill second slab");
        if (i == slot_count) {
            slab_b = test_find_slab_by_payload(heap, ptrs[i]);
            expect_true(slab_b != 0 && slab_b != slab_a,
                        "slab multi second slab created");
        }
        expect_true(test_find_slab_by_payload(heap, ptrs[i]) == slab_b,
                    "slab multi second slab ownership");
    }
    expect_true(slab_b->state == UHM_SLAB_STATE_FULL,
                "slab multi second slab full");
    expect_true(heap->core.slab_full_heads[size_class] == slab_b,
                "slab multi second full head");
    expect_true(slab_b->next_class == slab_a && slab_a->prev_class == slab_b,
                "slab multi full list links");
    expect_ok(api->check(heap, 0), "slab multi check with two full slabs");

    api->free(heap, ptrs[0]);
    ptrs[0] = 0;
    expect_true(slab_a->state == UHM_SLAB_STATE_PARTIAL &&
                    test_slab_remote_pending(slab_a) == 1u,
                "slab multi non-head full first free to partial");
    expect_true(heap->core.slab_class_heads[size_class] == slab_a,
                "slab multi partial list receives non-head");
    expect_true(heap->core.slab_full_heads[size_class] == slab_b &&
                    slab_b->next_class == 0,
                "slab multi full list unlinks non-head");
    api->free(heap, ptrs[1]);
    ptrs[1] = 0;
    expect_true(slab_a->state == UHM_SLAB_STATE_PARTIAL,
                "slab multi non-head remains partial");
    expect_true(heap->core.slab_class_heads[size_class] == slab_a,
                "slab multi partial head after non-head unlink");
    expect_true(heap->core.slab_full_heads[size_class] == slab_b &&
                    slab_b->next_class == 0,
                "slab multi full head survives non-head unlink");
    expect_ok(api->check(heap, 0), "slab multi check after non-head unlink");

    for (i = 2; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(slab_a->state == UHM_SLAB_STATE_EMPTY,
                "slab multi first slab empty");
    expect_true(heap->core.slab_empty_count[size_class] == 1u,
                "slab multi one empty slab");
    expect_true(heap->core.slab_committed_empty_count[size_class] == 1u,
                "slab multi one committed empty slab");

    for (i = slot_count; i < slot_count * 2u; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(slab_b->state == UHM_SLAB_STATE_EMPTY,
                "slab multi second slab empty");
    expect_true(heap->core.slab_empty_count[size_class] == 2u,
                "slab multi two empty slabs");
    expect_true(heap->core.slab_committed_empty_count[size_class] == 2u,
                "slab multi keeps two committed empty slabs below high water");
    expect_true(heap->core.slab_empty_heads[size_class] == slab_b &&
                    slab_b->next_class == slab_a &&
                    slab_a->prev_class == slab_b,
                "slab multi empty list links");
    expect_true((slab_a->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab multi first empty remains committed");
    expect_true((slab_b->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab multi second empty remains committed below high water");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "slab multi stats below high water");
    expect_ok(api->check(heap, 0), "slab multi check with two empty slabs");

    reuse = api->malloc(heap, 1u);
    expect_true(reuse != 0, "slab multi reuse empty slab");
    reuse_slab = test_find_slab_by_payload(heap, reuse);
    expect_true((reuse_slab == slab_a || reuse_slab == slab_b) &&
                    (reuse_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab multi reuses a committed empty slab");
    remaining_slab = reuse_slab == slab_a ? slab_b : slab_a;
    expect_true(reuse_slab->state == UHM_SLAB_STATE_PARTIAL,
                "slab multi reused slab partial");
    expect_true(heap->core.slab_empty_count[size_class] == 1u,
                "slab multi empty count decremented once");
    expect_true(heap->core.slab_committed_empty_count[size_class] == 1u,
                "slab multi committed empty count leaves one committed");
    expect_true(heap->core.slab_empty_heads[size_class] == remaining_slab,
                "slab multi remaining committed empty head");
    expect_true((remaining_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab multi remaining empty still committed");

    ptrs[0] = reuse;
    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 1u);
        expect_true(ptrs[i] != 0, "slab multi refill committed empty");
        expect_true(test_find_slab_by_payload(heap, ptrs[i]) == reuse_slab,
                    "slab multi refill stays on committed slab");
    }
    ptrs[slot_count] = api->malloc(heap, 1u);
    expect_true(ptrs[slot_count] != 0, "slab multi second empty allocation");
    expect_true(test_find_slab_by_payload(heap, ptrs[slot_count]) ==
                    remaining_slab,
                "slab multi consumes remaining committed slab");
    expect_true(heap->core.slab_committed_empty_count[size_class] == 0,
                "slab multi consumed committed empty count");
    expect_ok(api->check(heap, 0), "slab multi check after recommit");
    for (i = 0; i <= slot_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_ok(api->check(heap, 0), "slab multi check after reuse");

    p512 = api->malloc(heap, 512u);
    p513 = api->malloc(heap, 513u);
    expect_true(p512 != 0 && p513 != 0, "slab multi boundary allocations");
    expect_true(api->malloc_usable_size(heap, p512) == 512u,
                "slab multi 512 usable");
    expect_true(api->malloc_usable_size(heap, p513) >= 513u,
                "slab multi 513 usable");
    expect_true(test_find_slab_by_payload(heap, p512) != 0,
                "slab multi 512 uses slab");
    expect_true(test_find_slab_by_payload(heap, p513) == 0,
                "slab multi 513 bypasses slab");
    api->free(heap, p512);
    api->free(heap, p513);

    free(ptrs);
    expect_ok(api->check(heap, 0), "slab multi final check");
    expect_ok(api->close(heap), "slab multi heap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_empty_decommit_failure_keeps_committed(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *first;
    void *reuse;
    uhm_slab *slab_a;
    uhm_slab *slab_b = 0;
    uint16_t size_class;
    size_t slot_count;
    size_t slab_count;
    size_t ptr_count;
    size_t pressure_start;
    size_t i;

    memset(&state, 0, sizeof(state));
    state.platform_error = test_platform_error_code();
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab decommit failure heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "slab decommit failure arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab decommit failure usermode vm init");
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
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "slab decommit failure heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab decommit failure api");

    first = api->malloc(heap, 512u);
    expect_true(first != 0, "slab decommit failure first allocation");
    slab_a = test_find_slab_by_payload(heap, first);
    expect_true(slab_a != 0, "slab decommit failure first slab");
    size_class = slab_a->size_class;
    slot_count = slab_a->total_slots;
    slab_count = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER + 1u;
    ptr_count = slot_count * slab_count;
    pressure_start =
        slot_count * UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    ptrs = (void **)calloc(ptr_count, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab decommit failure pointer table");
    ptrs[0] = first;

    for (i = 1; i < ptr_count; ++i) {
        ptrs[i] = api->malloc(heap, 512u);
        expect_true(ptrs[i] != 0, "slab decommit failure fill slabs");
        if (i == pressure_start) {
            slab_b = test_find_slab_by_payload(heap, ptrs[i]);
            expect_true(slab_b != 0 && slab_b != slab_a,
                        "slab decommit failure pressure slab");
        }
    }

    for (i = 0; i < pressure_start; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(slab_a->state == UHM_SLAB_STATE_EMPTY,
                "slab decommit failure first slab empty");
    expect_true((slab_a->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab decommit failure first slab committed");
    expect_true(heap->core.slab_committed_empty_count[size_class] ==
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER,
                "slab decommit failure fills high water");

    state.fail_decommit = 1;
    for (i = pressure_start; i < ptr_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    state.fail_decommit = 0;
    expect_true(state.decommit_count != 0,
                "slab decommit failure attempted decommit");
    expect_true(slab_b->state == UHM_SLAB_STATE_EMPTY,
                "slab decommit failure second slab empty");
    expect_true((slab_b->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab decommit failure keeps slab committed");
    expect_true(slab_b->free_head != 0,
                "slab decommit failure restores free list");
    expect_true(heap->core.slab_empty_count[size_class] == slab_count,
                "slab decommit failure keeps empty count");
    expect_true(heap->core.slab_committed_empty_count[size_class] == slab_count,
                "slab decommit failure keeps committed empty count");
    expect_ok(api->check(heap, 0), "slab decommit failure heap check");

    reuse = api->malloc(heap, 1u);
    expect_true(reuse != 0, "slab decommit failure reuse allocation");
    expect_true(test_find_slab_by_payload(heap, reuse) != 0 &&
                    (test_find_slab_by_payload(heap, reuse)->flags &
                     UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab decommit failure reuses a committed empty slab");
    api->free(heap, reuse);

    free(ptrs);
    expect_ok(api->check(heap, 0), "slab decommit failure final check");
    expect_ok(api->close(heap), "slab decommit failure close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_empty_decommit_high_water_pressure(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *first;
    uhm_slab *first_slab;
    uhm_slab *pressure_slab = 0;
    uint16_t size_class;
    size_t slot_count;
    size_t slab_count;
    size_t ptr_count;
    size_t pressure_start;
    size_t i;
    uint64_t pressure_events_before;
    uint64_t pressure_bytes_before;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "slab high water heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "slab high water arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab high water usermode vm init");
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
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "slab high water heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab high water api");

    first = api->malloc(heap, 512u);
    expect_true(first != 0, "slab high water first allocation");
    first_slab = test_find_slab_by_payload(heap, first);
    expect_true(first_slab != 0, "slab high water first slab");
    size_class = first_slab->size_class;
    slot_count = first_slab->total_slots;
    slab_count = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER + 1u;
    ptr_count = slot_count * slab_count;
    pressure_start =
        slot_count * UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    ptrs = (void **)calloc(ptr_count, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab high water pointer table");
    ptrs[0] = first;

    for (i = 1; i < ptr_count; ++i) {
        ptrs[i] = api->malloc(heap, 512u);
        expect_true(ptrs[i] != 0, "slab high water fill slabs");
        if (i == pressure_start) {
            pressure_slab = test_find_slab_by_payload(heap, ptrs[i]);
            expect_true(pressure_slab != 0 && pressure_slab != first_slab,
                        "slab high water pressure slab");
        }
    }

    pressure_events_before = heap->core.slab_empty_decommit_pressure_events;
    pressure_bytes_before = heap->core.slab_empty_decommit_pressure_bytes;
    for (i = 0; i < ptr_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }

    expect_true(heap->core.slab_empty_count[size_class] == slab_count,
                "slab high water keeps empty list nodes");
    expect_true(heap->core.slab_committed_empty_count[size_class] ==
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER,
                "slab high water caps committed empty count");
    expect_true(pressure_slab != 0 &&
                    (pressure_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0,
                "slab high water decommits pressure slab");
    expect_true(state.decommit_count != 0,
                "slab high water uses decommit callback");
    expect_true(heap->core.slab_empty_decommit_pressure_events >
                    pressure_events_before,
                "slab high water pressure event counted");
    expect_true(heap->core.slab_empty_decommit_pressure_bytes >=
                    pressure_bytes_before + page_size,
                "slab high water pressure bytes counted");
    expect_ok(api->check(heap, 0), "slab high water check");

    free(ptrs);
    expect_ok(api->close(heap), "slab high water close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_empty_adaptive_high_water_after_recommit(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *first;
    uhm_slab *first_slab;
    uhm_slab *pressure_slab = 0;
    uint16_t size_class;
    size_t slot_count;
    size_t base_high_water;
    size_t slab_count;
    size_t ptr_count;
    size_t warm_count;
    size_t pressure_start;
    size_t i;
    void **warm_ptrs;
    uint64_t grow_events_before;
    int commit_count_before_recommit;
    int decommit_count_before_second_free;
    int decommit_count_before_idle_drain;
    int commit_count_before_committed_reuse;
    int saw_pressure_slab;
    uhm_thread_config ctx_config;
    uhm_thread_ctx *ctx = 0;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "slab adaptive high water heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "slab adaptive high water arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab adaptive high water usermode vm init");
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
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "slab adaptive high water heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab adaptive high water api");

    first = api->malloc(heap, 512u);
    expect_true(first != 0, "slab adaptive high water first allocation");
    first_slab = test_find_slab_by_payload(heap, first);
    expect_true(first_slab != 0, "slab adaptive high water first slab");
    size_class = first_slab->size_class;
    slot_count = first_slab->total_slots;
    base_high_water = uhm_slab_empty_committed_cache_state_high_water(
        heap->core.slab_empty_committed_cache_high_water[size_class]);
    expect_true(base_high_water == UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER,
                "slab adaptive high water starts at base");
    slab_count = base_high_water + 1u;
    ptr_count = slot_count * slab_count;
    pressure_start = slot_count * base_high_water;
    ptrs = (void **)calloc(ptr_count, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab adaptive high water pointer table");
    ptrs[0] = first;

    for (i = 1; i < ptr_count; ++i) {
        ptrs[i] = api->malloc(heap, 512u);
        expect_true(ptrs[i] != 0, "slab adaptive high water fill slabs");
        if (i == pressure_start) {
            pressure_slab = test_find_slab_by_payload(heap, ptrs[i]);
            expect_true(pressure_slab != 0 && pressure_slab != first_slab,
                        "slab adaptive high water pressure slab");
        }
    }
    for (i = 0; i < ptr_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(pressure_slab != 0 &&
                    (pressure_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0,
                "slab adaptive high water first cycle decommits pressure");
    expect_true(uhm_slab_empty_committed_cache_state_high_water(
                    heap->core.slab_empty_committed_cache_high_water
                        [size_class]) == base_high_water,
                "slab adaptive high water unchanged before recommit");
    expect_ok(api->check(heap, 0),
              "slab adaptive high water check after first pressure");

    grow_events_before =
        heap->core.slab_empty_committed_cache_adaptive_grow_events;
    commit_count_before_recommit = state.commit_count;
    saw_pressure_slab = 0;
    for (i = 0; i < ptr_count; ++i) {
        ptrs[i] = api->malloc(heap, 512u);
        expect_true(ptrs[i] != 0,
                    "slab adaptive high water refill after pressure");
        if (test_find_slab_by_payload(heap, ptrs[i]) == pressure_slab) {
            saw_pressure_slab = 1;
        }
    }
    expect_true(saw_pressure_slab != 0,
                "slab adaptive high water reused decommitted slab");
    expect_true(state.commit_count == commit_count_before_recommit + 1,
                "slab adaptive high water recommit callback used");
    expect_true(heap->core.slab_empty_committed_cache_adaptive_grow_events >
                    grow_events_before,
                "slab adaptive high water grow event counted");
    expect_true(uhm_slab_empty_committed_cache_state_high_water(
                    heap->core.slab_empty_committed_cache_high_water
                        [size_class]) > base_high_water,
                "slab adaptive high water grows after same-address recommit");
    expect_true(uhm_slab_empty_committed_cache_state_high_water(
                    heap->core.slab_empty_committed_cache_high_water
                        [size_class]) <=
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER,
                "slab adaptive high water capped");
    expect_ok(api->check(heap, 0),
              "slab adaptive high water check after recommit");

    decommit_count_before_second_free = state.decommit_count;
    for (i = 0; i < ptr_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(state.decommit_count == decommit_count_before_second_free,
                "slab adaptive high water avoids second decommit burst");
    expect_true(heap->core.slab_committed_empty_count[size_class] ==
                    slab_count,
                "slab adaptive high water keeps all slabs committed");
    expect_true((pressure_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab adaptive high water pressure slab stays committed");
    expect_ok(api->check(heap, 0),
              "slab adaptive high water check after retained free");

    commit_count_before_committed_reuse = state.commit_count;
    for (i = 0; i < ptr_count; ++i) {
        ptrs[i] = api->malloc(heap, 512u);
        expect_true(ptrs[i] != 0,
                    "slab adaptive high water committed reuse");
    }
    expect_true(state.commit_count == commit_count_before_committed_reuse,
                "slab adaptive high water reuse avoids recommit");
    expect_ok(api->check(heap, 0),
              "slab adaptive high water check after committed reuse");

    for (i = 0; i < ptr_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }

    warm_count = slot_count * (slab_count - 1u);
    warm_ptrs = (void **)calloc(warm_count, sizeof(*warm_ptrs));
    expect_true(warm_ptrs != 0,
                "slab adaptive high water idle shrink pointer table");
    for (i = 0; i < warm_count; ++i) {
        warm_ptrs[i] = api->malloc(heap, 512u);
        expect_true(warm_ptrs[i] != 0,
                    "slab adaptive high water idle shrink fill");
    }
    expect_true(heap->core.slab_committed_empty_count[size_class] ==
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_LOW_WATER,
                "slab adaptive high water idle shrink reaches low water");
    expect_true(uhm_slab_empty_committed_cache_state_high_water(
                    heap->core.slab_empty_committed_cache_high_water
                        [size_class]) > base_high_water,
                "slab adaptive high water idle shrink starts expanded");
    decommit_count_before_idle_drain = state.decommit_count;
    expect_true(test_vm_reserve_commit_alloc(sizeof(uhm_thread_ctx),
                                             alignof(uhm_thread_ctx),
                                             &ctx_mem) != 0,
                "slab adaptive high water idle shrink ctx storage");
    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = sizeof(uhm_thread_ctx);
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "slab adaptive high water idle shrink ctx open");
    for (i = 1u;
         i < UHM_SLAB_EMPTY_COMMITTED_CACHE_IDLE_SHRINK_DRAINS;
         ++i) {
        expect_ok(api->thread_ctx_flush(heap, ctx),
                  "slab adaptive high water idle shrink aging drain");
        expect_true(
            uhm_slab_empty_committed_cache_state_high_water(
                heap->core.slab_empty_committed_cache_high_water
                    [size_class]) >
                base_high_water,
            "slab adaptive high water idle shrink waits for age");
    }
    expect_ok(api->thread_ctx_flush(heap, ctx),
              "slab adaptive high water idle shrink drain");
    expect_true(uhm_slab_empty_committed_cache_state_high_water(
                    heap->core.slab_empty_committed_cache_high_water
                        [size_class]) == base_high_water,
                "slab adaptive high water idle shrink returns to base");
    expect_true(heap->core.slab_committed_empty_count[size_class] ==
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_LOW_WATER,
                "slab adaptive high water idle shrink keeps physical state");
    expect_true(state.decommit_count == decommit_count_before_idle_drain,
                "slab adaptive high water idle shrink skips VM decommit");
    heap->core.slab_empty_committed_cache_high_water[size_class] |=
        UHM_SLAB_EMPTY_COMMITTED_CACHE_STATE_MASK + 1u;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab adaptive high water rejects unknown state bits");
    heap->core.slab_empty_committed_cache_high_water[size_class] =
        uhm_slab_empty_committed_cache_state_make(base_high_water, 0u);
    for (i = 0; i < warm_count; ++i) {
        api->free(heap, warm_ptrs[i]);
        warm_ptrs[i] = 0;
    }
    free(warm_ptrs);
    api->thread_ctx_close(heap, ctx);
    test_vm_release_alloc(&ctx_mem);
    expect_ok(api->check(heap, 0),
              "slab adaptive high water idle shrink final check");
    free(ptrs);
    expect_ok(api->check(heap, 0), "slab adaptive high water final check");
    expect_ok(api->close(heap), "slab adaptive high water close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_decommit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *first;
    uhm_slab *first_slab;
    uhm_slab *pressure_slab = 0;
    uint16_t size_class;
    size_t slot_count;
    size_t slab_count;
    size_t ptr_count;
    size_t pressure_start;
    size_t i;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;
    int decommit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "slab decommit unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "slab decommit unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab decommit unlock usermode vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "slab decommit unlock heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab decommit unlock api");
    state.heap = heap;
    state.api = api;

    first = api->malloc(heap, 512u);
    expect_true(first != 0, "slab decommit unlock first allocation");
    first_slab = test_find_slab_by_payload(heap, first);
    expect_true(first_slab != 0, "slab decommit unlock first slab");
    size_class = first_slab->size_class;
    slot_count = first_slab->total_slots;
    slab_count = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER + 1u;
    ptr_count = slot_count * slab_count;
    pressure_start =
        slot_count * UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    ptrs = (void **)calloc(ptr_count, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab decommit unlock pointer table");
    ptrs[0] = first;

    for (i = 1; i < ptr_count; ++i) {
        ptrs[i] = api->malloc(heap, 512u);
        expect_true(ptrs[i] != 0, "slab decommit unlock fill slabs");
        if (i == pressure_start) {
            pressure_slab = test_find_slab_by_payload(heap, ptrs[i]);
            expect_true(pressure_slab != 0 && pressure_slab != first_slab,
                        "slab decommit unlock pressure slab");
        }
    }

    for (i = 0; i < pressure_start; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(heap->core.slab_committed_empty_count[size_class] ==
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER,
                "slab decommit unlock fills high water");
    expect_ok(api->check(heap, 0), "slab decommit unlock check before");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    decommit_count_before = state.decommit_count;
    state.check_calls = 0;
    state.run_check = 1;
    for (i = pressure_start; i < ptr_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    state.run_check = 0;

    expect_true(state.decommit_count == decommit_count_before + 1,
                "slab decommit unlock decommit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "slab decommit unlock reentrant check ok");
    expect_ok(state.check_result, "slab decommit unlock last check ok");
    expect_true(heap->core.vm_decommit_calls ==
                    vm_decommit_calls_before + 1u,
                "slab decommit unlock decommit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "slab decommit unlock decommit not counted lock-held");
    expect_true(pressure_slab != 0 &&
                    (pressure_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0,
                "slab decommit unlock pressure slab decommitted");
    expect_true((pressure_slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0,
                "slab decommit unlock busy flag cleared");
    expect_true(pressure_slab->free_head == 0,
                "slab decommit unlock free list cleared");
    expect_ok(api->check(heap, 0), "slab decommit unlock check after");

    free(ptrs);
    expect_ok(api->close(heap), "slab decommit unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_empty_contiguous_batch_decommit(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    enum {
        SLAB_COUNT = 13u,
        GAP_SLAB_COUNT = SLAB_COUNT + 2u
    };
    const size_t page_size = uhm_platform_system_page_size();
    const size_t slab_count = SLAB_COUNT;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    void **ptrs = 0;
    uhm_slab *slabs[SLAB_COUNT] = {0};
    uhm_slab *gap_slabs[GAP_SLAB_COUNT] = {0};
    uhm_slab *separator_slab = 0;
    void *separator = 0;
    size_t slot_count;
    size_t ptr_count;
    size_t gap_ptr_count;
    size_t i;
    size_t unique_count = 0;
    size_t gap_unique_count = 0;
    size_t decommitted_count;
    int decommit_before;
    int commit_before;
    uint64_t vm_decommit_calls_before;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    state.platform_error = test_platform_error_code();
    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "slab batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    2u * 1024u * 1024u, page_size, &arena_mem) != 0,
                "slab batch arena alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    ctx_info->storage_size, ctx_info->storage_alignment,
                    &ctx_mem) != 0,
                "slab batch ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab batch usermode vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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
    expect_ok(uhm_open(&config, &heap), "slab batch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab batch api");
    state.heap = heap;
    state.api = api;

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    ctx_config.local_cache_bytes = 2u * slab_count * page_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "slab batch ctx open");

    ptrs = (void **)calloc(page_size, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab batch pointer table");
    ptrs[0] = api->malloc_ctx(heap, ctx, 512u);
    expect_true(ptrs[0] != 0, "slab batch first allocation");
    slot_count = test_slab_slot_from_payload(ptrs[0])->slab->total_slots;
    ptr_count = slab_count * slot_count;
    expect_true(ptr_count != 0 && ptr_count <= page_size,
                "slab batch pointer count bounded");
    for (i = 1u; i < ptr_count; ++i) {
        ptrs[i] = api->malloc_ctx(heap, ctx, 512u);
        expect_true(ptrs[i] != 0, "slab batch fill allocation");
    }
    for (i = 0u; i < ptr_count; ++i) {
        uhm_slab *slab = test_slab_slot_from_payload(ptrs[i])->slab;
        if (unique_count == 0u || slabs[unique_count - 1u] != slab) {
            expect_true(unique_count < slab_count,
                        "slab batch unique slab bounded");
            slabs[unique_count++] = slab;
        }
    }
    expect_true(unique_count == slab_count,
                "slab batch crosses legacy eight-candidate window");
    for (i = 1u; i < slab_count; ++i) {
        expect_true(slabs[i - 1u]->base + slabs[i - 1u]->size ==
                        slabs[i]->base,
                    "slab batch slabs are contiguous");
    }
    for (i = 0u; i < ptr_count; ++i) {
        api->free_ctx(heap, ctx, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(ctx->local_cache_count == ptr_count &&
                    ctx->retained_slab_pages == slab_count,
                "slab batch ctx retains all slots and pages");

    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    decommit_before = state.decommit_count;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    state.check_calls = 0;
    state.check_failures = 0;
    state.run_check = 1;
    expect_ok(api->thread_ctx_flush(heap, ctx), "slab batch ctx flush");
    state.run_check = 0;
    expect_true(state.decommit_count == decommit_before + 1,
                "slab batch uses one decommit callback");
    expect_true(state.last_decommit_size == (slab_count - 1u) * page_size,
                "slab batch callback spans contiguous excess pages");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "slab batch callback reentrant check succeeds");
    expect_true(heap->core.vm_decommit_calls ==
                    vm_decommit_calls_before + 1u,
                "slab batch counts one VM decommit call");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "slab batch VM call stays outside heap lock");
    decommitted_count = 0u;
    for (i = 0u; i < slab_count; ++i) {
        if ((slabs[i]->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0u) {
            ++decommitted_count;
            expect_true(slabs[i]->free_head == 0,
                        "slab batch decommitted free head cleared");
        }
        expect_true((slabs[i]->flags & UHM_SLAB_FLAG_VM_BUSY) == 0u,
                    "slab batch busy flag cleared");
    }
    expect_true(decommitted_count == slab_count - 1u &&
                    heap->core.slab_committed_empty_count
                        [slabs[0]->size_class] == 1u,
                "slab batch preserves committed high water");
    expect_ok(api->check(heap, 0), "slab batch success check");

    for (i = 0u; i < ptr_count; ++i) {
        ptrs[i] = api->malloc_ctx(heap, ctx, 512u);
        expect_true(ptrs[i] != 0, "slab batch publish rollback refill");
    }
    for (i = 0u; i < ptr_count; ++i) {
        api->free_ctx(heap, ctx, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(ctx->local_cache_count == ptr_count,
                "slab batch publish rollback retains all slots");
    decommit_before = state.decommit_count;
    commit_before = state.commit_count;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    heap->core.fault_slab_decommit_page_publish_countdown = 2u;
    expect_true(api->thread_ctx_flush(heap, ctx) == UHM_ERR_OUT_OF_MEMORY,
                "slab batch partial page publish failure reported");
    expect_true(
        heap->core.fault_slab_decommit_page_publish_countdown == 0u,
        "slab batch partial page publish fault consumed");
    expect_true(state.decommit_count == decommit_before &&
                    state.commit_count == commit_before &&
                    heap->core.vm_decommit_calls == vm_decommit_calls_before,
                "slab batch partial page publish avoids platform VM");
    for (i = 0u; i < slab_count; ++i) {
        expect_true((slabs[i]->flags &
                     (UHM_SLAB_FLAG_DECOMMITTED | UHM_SLAB_FLAG_VM_BUSY)) ==
                        0u &&
                        slabs[i]->free_head != 0,
                    "slab batch partial page publish restores claimed slab");
    }
    expect_true(heap->core.slab_committed_empty_count
                    [slabs[0]->size_class] == slab_count,
                "slab batch partial page publish restores committed count");
    expect_ok(api->check(heap, 0),
              "slab batch partial page publish rollback check");

    for (i = 0u; i < ptr_count; ++i) {
        ptrs[i] = api->malloc_ctx(heap, ctx, 512u);
        expect_true(ptrs[i] != 0, "slab batch platform rollback refill");
    }
    for (i = 0u; i < ptr_count; ++i) {
        api->free_ctx(heap, ctx, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(ctx->local_cache_count == ptr_count,
                "slab batch platform rollback retains all slots");
    decommit_before = state.decommit_count;
    commit_before = state.commit_count;
    state.check_calls = 0;
    state.check_failures = 0;
    state.fail_decommit = 1;
    state.run_check = 1;
    expect_ok(api->thread_ctx_flush(heap, ctx),
              "slab batch failed decommit flush is recoverable");
    state.run_check = 0;
    state.fail_decommit = 0;
    expect_true(state.decommit_count == decommit_before + 1,
                "slab batch failure still uses one decommit callback");
    expect_true(state.commit_count == commit_before + 1,
                "slab batch failure recommits one contiguous range");
    expect_true(state.check_calls == 2 && state.check_failures == 0,
                "slab batch rollback callbacks pass reentrant check");
    for (i = 0u; i < slab_count; ++i) {
        expect_true((slabs[i]->flags &
                     (UHM_SLAB_FLAG_DECOMMITTED | UHM_SLAB_FLAG_VM_BUSY)) ==
                        0u &&
                        slabs[i]->free_head != 0,
                    "slab batch failure restores committed slab");
    }
    expect_true(heap->core.slab_committed_empty_count
                    [slabs[0]->size_class] == slab_count,
                "slab batch failure restores committed count");
    expect_ok(api->check(heap, 0), "slab batch rollback check");

    for (i = 0u; i < ptr_count; ++i) {
        ptrs[i] = api->malloc_ctx(heap, ctx, 512u);
        expect_true(ptrs[i] != 0,
                    "slab batch revalidate rollback refill");
    }
    for (i = 0u; i < ptr_count; ++i) {
        api->free_ctx(heap, ctx, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(ctx->local_cache_count == ptr_count,
                "slab batch revalidate rollback retains all slots");
    decommit_before = state.decommit_count;
    commit_before = state.commit_count;
    state.check_calls = 0;
    state.check_failures = 0;
    state.zero_decommit_range = 1;
    state.decommit_revalidate_slab = slabs[0];
    state.run_check = 1;
    expect_true(api->thread_ctx_flush(heap, ctx) == UHM_ERR_CORRUPTION,
                "slab batch revalidate failure is reported");
    state.run_check = 0;
    state.decommit_revalidate_slab = 0;
    state.zero_decommit_range = 0;
    expect_true(state.decommit_count == decommit_before + 1,
                "slab batch revalidate failure decommits one range");
    expect_true(state.commit_count == commit_before + 1,
                "slab batch revalidate failure recommits one range");
    expect_true(state.check_calls == 2 && state.check_failures == 0,
                "slab batch revalidate rollback callbacks pass check");
    for (i = 0u; i < slab_count; ++i) {
        expect_true((slabs[i]->flags &
                     (UHM_SLAB_FLAG_DECOMMITTED | UHM_SLAB_FLAG_VM_BUSY)) ==
                        0u &&
                        slabs[i]->free_head != 0,
                    "slab batch revalidate rollback rebuilds free slab");
    }
    expect_true(heap->core.slab_committed_empty_count
                    [slabs[0]->size_class] == slab_count,
                "slab batch revalidate rollback restores committed count");
    expect_ok(api->check(heap, 0),
              "slab batch revalidate rollback rebuild check");

    for (i = 0u; i < ptr_count; ++i) {
        ptrs[i] = api->malloc_ctx(heap, ctx, 512u);
        expect_true(ptrs[i] != 0, "slab batch gap refill");
    }
    separator = api->malloc(heap, 64u);
    expect_true(separator != 0, "slab batch gap separator allocation");
    separator_slab = test_slab_slot_from_payload(separator)->slab;
    expect_true(separator_slab != 0,
                "slab batch gap separator uses slab");
    gap_ptr_count = (slab_count + 2u) * slot_count;
    expect_true(gap_ptr_count <= page_size,
                "slab batch gap pointer count bounded");
    for (i = ptr_count; i < gap_ptr_count; ++i) {
        ptrs[i] = api->malloc_ctx(heap, ctx, 512u);
        expect_true(ptrs[i] != 0, "slab batch gap extension allocation");
    }
    for (i = 0u; i < gap_ptr_count; ++i) {
        uhm_slab *slab = test_slab_slot_from_payload(ptrs[i])->slab;
        if (gap_unique_count == 0u ||
            gap_slabs[gap_unique_count - 1u] != slab) {
            expect_true(gap_unique_count < slab_count + 2u,
                        "slab batch gap unique slab bounded");
            gap_slabs[gap_unique_count++] = slab;
        }
    }
    expect_true(gap_unique_count == slab_count + 2u,
                "slab batch gap creates bounded target slabs");
    for (i = 1u; i < gap_unique_count; ++i) {
        uhm_slab *ordered = gap_slabs[i];
        size_t j = i;
        while (j != 0u && gap_slabs[j - 1u]->base > ordered->base) {
            gap_slabs[j] = gap_slabs[j - 1u];
            --j;
        }
        gap_slabs[j] = ordered;
    }
    expect_true(gap_slabs[slab_count - 1u]->base + page_size ==
                        separator_slab->base &&
                    separator_slab->base + separator_slab->size ==
                        gap_slabs[slab_count]->base,
                "slab batch gap has one live separator page");
    for (i = 0u; i < gap_ptr_count; ++i) {
        api->free_ctx(heap, ctx, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(ctx->local_cache_count == gap_ptr_count &&
                    ctx->retained_slab_pages == slab_count + 2u,
                "slab batch gap retains all target slabs");
    decommit_before = state.decommit_count;
    state.check_calls = 0;
    state.check_failures = 0;
    state.run_check = 1;
    expect_ok(api->thread_ctx_flush(heap, ctx),
              "slab batch noncontiguous flush");
    state.run_check = 0;
    expect_true(state.decommit_count == decommit_before + 2,
                "slab batch noncontiguous uses two VM ranges");
    expect_true(state.last_decommit_size == page_size,
                "slab batch noncontiguous tail falls back per slab");
    expect_true(state.check_calls == 2 && state.check_failures == 0,
                "slab batch noncontiguous callbacks pass check");
    decommitted_count = 0u;
    for (i = 0u; i < gap_unique_count; ++i) {
        if ((gap_slabs[i]->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0u) {
            ++decommitted_count;
        }
    }
    expect_true(decommitted_count == gap_unique_count - 1u,
                "slab batch noncontiguous preserves one committed target");
    memset(separator, 0x5a, 64u);
    expect_true(((unsigned char *)separator)[0] == 0x5a &&
                    ((unsigned char *)separator)[63] == 0x5a,
                "slab batch noncontiguous preserves live separator");
    expect_ok(api->check(heap, 0),
              "slab batch noncontiguous fallback check");

    heap->core.test_slab_empty_decommit_high_water_override = 0u;
    api->free(heap, separator);
    free(ptrs);
    api->thread_ctx_close(heap, ctx);
    expect_ok(api->close(heap), "slab batch heap close");
    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_empty_decommit_prefers_largest_run(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    enum {
        RETAINED_SLAB_COUNT = 4u,
        EXCESS_SLAB_COUNT = 12u,
        TARGET_SLAB_COUNT = RETAINED_SLAB_COUNT + EXCESS_SLAB_COUNT
    };
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    void **ptrs = 0;
    void *separator = 0;
    uhm_slab *separator_slab = 0;
    uhm_slab *slabs[TARGET_SLAB_COUNT] = {0};
    size_t slot_count;
    size_t ptr_count;
    size_t unique_count = 0u;
    size_t i;
    size_t j;
    int decommit_before;
    uint64_t decommit_events_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    state.platform_error = test_platform_error_code();
    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "slab largest-run heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    2u * 1024u * 1024u, page_size, &arena_mem) != 0,
                "slab largest-run arena alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    ctx_info->storage_size, ctx_info->storage_alignment,
                    &ctx_mem) != 0,
                "slab largest-run ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab largest-run usermode vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 2u * 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.control_band_min_size = 256u * 1024u;
    config.vm = vm;
    expect_ok(uhm_open(&config, &heap), "slab largest-run heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab largest-run api");
    state.heap = heap;
    state.api = api;

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    ctx_config.local_cache_bytes = 2u * TARGET_SLAB_COUNT * page_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "slab largest-run ctx open");

    ptrs = (void **)calloc(page_size, sizeof(ptrs[0]));
    expect_true(ptrs != 0, "slab largest-run pointer table");
    ptrs[0] = api->malloc_ctx(heap, ctx, 512u);
    expect_true(ptrs[0] != 0, "slab largest-run first allocation");
    slot_count = test_slab_slot_from_payload(ptrs[0])->slab->total_slots;
    ptr_count = TARGET_SLAB_COUNT * slot_count;
    expect_true(slot_count != 0u && ptr_count <= page_size,
                "slab largest-run pointer count bounded");
    for (i = 1u; i < RETAINED_SLAB_COUNT * slot_count; ++i) {
        ptrs[i] = api->malloc_ctx(heap, ctx, 512u);
        expect_true(ptrs[i] != 0, "slab largest-run low fill");
    }
    separator = api->malloc(heap, 64u);
    expect_true(separator != 0, "slab largest-run separator allocation");
    separator_slab = test_slab_slot_from_payload(separator)->slab;
    expect_true(separator_slab != 0,
                "slab largest-run separator slab");
    for (i = RETAINED_SLAB_COUNT * slot_count; i < ptr_count; ++i) {
        ptrs[i] = api->malloc_ctx(heap, ctx, 512u);
        expect_true(ptrs[i] != 0, "slab largest-run high fill");
    }
    for (i = 0u; i < ptr_count; ++i) {
        uhm_slab *slab = test_slab_slot_from_payload(ptrs[i])->slab;

        for (j = 0u; j < unique_count; ++j) {
            if (slabs[j] == slab) {
                break;
            }
        }
        if (j == unique_count) {
            expect_true(unique_count < TARGET_SLAB_COUNT,
                        "slab largest-run unique slab bound");
            slabs[unique_count++] = slab;
        }
    }
    expect_true(unique_count == TARGET_SLAB_COUNT,
                "slab largest-run exact target count");
    for (i = 1u; i < unique_count; ++i) {
        uhm_slab *ordered = slabs[i];

        j = i;
        while (j != 0u && slabs[j - 1u]->base > ordered->base) {
            slabs[j] = slabs[j - 1u];
            --j;
        }
        slabs[j] = ordered;
    }
    for (i = 1u; i < RETAINED_SLAB_COUNT; ++i) {
        expect_true(slabs[i - 1u]->base + page_size == slabs[i]->base,
                    "slab largest-run retained group contiguous");
    }
    expect_true(slabs[RETAINED_SLAB_COUNT - 1u]->base + page_size ==
                        separator_slab->base &&
                    separator_slab->base + separator_slab->size ==
                        slabs[RETAINED_SLAB_COUNT]->base,
                "slab largest-run live separator splits groups");
    for (i = RETAINED_SLAB_COUNT + 1u; i < TARGET_SLAB_COUNT; ++i) {
        expect_true(slabs[i - 1u]->base + page_size == slabs[i]->base,
                    "slab largest-run excess group contiguous");
    }

    for (i = 0u; i < ptr_count; ++i) {
        api->free_ctx(heap, ctx, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(ctx->local_cache_count == ptr_count &&
                    ctx->retained_slab_pages == TARGET_SLAB_COUNT,
                "slab largest-run ctx retains target groups");
    heap->core.test_slab_empty_decommit_high_water_override =
        RETAINED_SLAB_COUNT;
    decommit_before = state.decommit_count;
    decommit_events_before = heap->core.decommit_events;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    state.check_calls = 0;
    state.check_failures = 0;
    state.run_check = 1;
    expect_ok(api->thread_ctx_flush(heap, ctx),
              "slab largest-run ctx flush");
    state.run_check = 0;
    expect_true(state.decommit_count == decommit_before + 1 &&
                    heap->core.vm_decommit_calls ==
                        vm_decommit_calls_before + 1u,
                "slab largest-run uses one VM call");
    expect_true(state.last_decommit_addr ==
                        (void *)slabs[RETAINED_SLAB_COUNT]->base &&
                    state.last_decommit_size ==
                        EXCESS_SLAB_COUNT * page_size,
                "slab largest-run decommits the twelve-page excess run");
    expect_true(heap->core.decommit_events ==
                    decommit_events_before + EXCESS_SLAB_COUNT &&
                    heap->core.slab_committed_empty_count
                        [slabs[0]->size_class] == RETAINED_SLAB_COUNT,
                "slab largest-run preserves four-page high water");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "slab largest-run callback reentrant check succeeds");
    for (i = 0u; i < RETAINED_SLAB_COUNT; ++i) {
        expect_true((slabs[i]->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0u,
                    "slab largest-run retains low-address high water");
    }
    for (i = RETAINED_SLAB_COUNT; i < TARGET_SLAB_COUNT; ++i) {
        expect_true((slabs[i]->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0u,
                    "slab largest-run decommits high-address excess");
    }
    memset(separator, 0x6b, 64u);
    expect_true(((unsigned char *)separator)[0] == 0x6b &&
                    ((unsigned char *)separator)[63] == 0x6b,
                "slab largest-run preserves live separator");
    expect_ok(api->check(heap, 0), "slab largest-run final check");

    heap->core.test_slab_empty_decommit_high_water_override = 0u;
    api->free(heap, separator);
    free(ptrs);
    api->thread_ctx_close(heap, ctx);
    expect_ok(api->close(heap), "slab largest-run heap close");
    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_empty_decommit_candidate_skips_decommitted(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    enum {
        LOW_SLAB_COUNT = 7u,
        HIGH_SLAB_COUNT = 6u,
        GROUP_COUNT = 2u
    };
    const size_t page_size = uhm_platform_system_page_size();
    const size_t payload_size = 512u;
    const size_t slab_counts[GROUP_COUNT] = {LOW_SLAB_COUNT,
                                             HIGH_SLAB_COUNT};
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem[GROUP_COUNT] = {{0}};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *contexts[GROUP_COUNT] = {0};
    const uhm_api *api;
    void **ptrs = 0;
    uhm_slab *groups[GROUP_COUNT][LOW_SLAB_COUNT] = {{0}};
    uhm_slab *list_cursor;
    size_t slot_count = 0u;
    uint16_t size_class = UHM_SIZE_CLASS_COUNT;
    size_t group_index;
    size_t i;
    size_t j;
    int decommit_before;
    uint64_t decommit_events_before;
    uint64_t vm_decommit_calls_before;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    state.platform_error = test_platform_error_code();
    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "slab candidate filter heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    2u * 1024u * 1024u, page_size, &arena_mem) != 0,
                "slab candidate filter arena alloc");
    for (group_index = 0u; group_index < GROUP_COUNT; ++group_index) {
        expect_true(test_vm_reserve_commit_alloc(
                        ctx_info->storage_size, ctx_info->storage_alignment,
                        &ctx_mem[group_index]) != 0,
                    "slab candidate filter ctx storage alloc");
    }
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab candidate filter usermode vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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
    expect_ok(uhm_open(&config, &heap),
              "slab candidate filter heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab candidate filter api");
    state.heap = heap;
    state.api = api;

    for (group_index = 0u; group_index < GROUP_COUNT; ++group_index) {
        memset(&ctx_config, 0, sizeof(ctx_config));
        ctx_config.size = sizeof(ctx_config);
        ctx_config.abi_major = UHM_ABI_MAJOR;
        ctx_config.abi_minor = UHM_ABI_MINOR;
        ctx_config.storage = ctx_mem[group_index].aligned;
        ctx_config.storage_size = ctx_info->storage_size;
        ctx_config.local_cache_bytes =
            2u * LOW_SLAB_COUNT * page_size;
        expect_ok(api->thread_ctx_open(heap, &ctx_config,
                                       &contexts[group_index]),
                  "slab candidate filter ctx open");
    }

    for (group_index = 0u; group_index < GROUP_COUNT; ++group_index) {
        size_t ptr_count;
        size_t unique_count = 0u;

        if (group_index == 0u) {
            void *first = api->malloc_ctx(heap, contexts[group_index],
                                          payload_size);
            uhm_slab *first_slab;

            expect_true(first != 0,
                        "slab candidate filter first allocation");
            first_slab = test_find_slab_by_payload(heap, first);
            expect_true(first_slab != 0,
                        "slab candidate filter first slab");
            slot_count = first_slab->total_slots;
            size_class = first_slab->size_class;
            expect_true(slot_count != 0u &&
                            slot_count <= SIZE_MAX / LOW_SLAB_COUNT,
                        "slab candidate filter pointer count bounded");
            ptrs = (void **)calloc(LOW_SLAB_COUNT * slot_count,
                                   sizeof(ptrs[0]));
            expect_true(ptrs != 0,
                        "slab candidate filter pointer table");
            ptrs[0] = first;
        }
        ptr_count = slab_counts[group_index] * slot_count;
        expect_true(ptr_count != 0u &&
                        ptr_count <= LOW_SLAB_COUNT * slot_count,
                    "slab candidate filter group pointer count");
        if (group_index != 0u) {
            memset(ptrs, 0,
                   LOW_SLAB_COUNT * slot_count * sizeof(ptrs[0]));
            ptrs[0] = api->malloc_ctx(heap, contexts[group_index],
                                      payload_size);
            expect_true(ptrs[0] != 0,
                        "slab candidate filter next first allocation");
        }
        for (i = 1u; i < ptr_count; ++i) {
            ptrs[i] = api->malloc_ctx(heap, contexts[group_index],
                                      payload_size);
            expect_true(ptrs[i] != 0,
                        "slab candidate filter fill allocation");
        }
        for (i = 0u; i < ptr_count; ++i) {
            uhm_slab *slab = test_find_slab_by_payload(heap, ptrs[i]);

            expect_true(slab != 0 && slab->size_class == size_class,
                        "slab candidate filter allocation owner");
            for (j = 0u; j < unique_count; ++j) {
                if (groups[group_index][j] == slab) {
                    break;
                }
            }
            if (j == unique_count) {
                expect_true(unique_count < slab_counts[group_index],
                            "slab candidate filter unique slab bound");
                groups[group_index][unique_count++] = slab;
            }
        }
        expect_true(unique_count == slab_counts[group_index],
                    "slab candidate filter exact slab count");
        for (i = 1u; i < unique_count; ++i) {
            uhm_slab *ordered = groups[group_index][i];

            j = i;
            while (j != 0u &&
                   groups[group_index][j - 1u]->base > ordered->base) {
                groups[group_index][j] = groups[group_index][j - 1u];
                --j;
            }
            groups[group_index][j] = ordered;
        }
        for (i = 0u; i < unique_count; ++i) {
            expect_true(groups[group_index][i]->size == page_size,
                        "slab candidate filter page-sized slab");
            if (i != 0u) {
                expect_true(groups[group_index][i - 1u]->base + page_size ==
                                groups[group_index][i]->base,
                            "slab candidate filter contiguous group");
            }
        }
        for (i = 0u; i < ptr_count; ++i) {
            api->free_ctx(heap, contexts[group_index], ptrs[i]);
            ptrs[i] = 0;
        }
        expect_true(contexts[group_index]->local_cache_count == ptr_count &&
                        contexts[group_index]->retained_slab_pages ==
                            slab_counts[group_index],
                    "slab candidate filter ctx retains group");
    }

    expect_true(groups[0][LOW_SLAB_COUNT - 1u]->base + page_size ==
                    groups[1][0]->base,
                "slab candidate filter groups share contiguous arena run");
    list_cursor = heap->core.slab_all_head;
    for (group_index = GROUP_COUNT; group_index != 0u; --group_index) {
        size_t current_group = group_index - 1u;

        for (i = slab_counts[current_group]; i != 0u; --i) {
            expect_true(list_cursor == groups[current_group][i - 1u],
                        "slab candidate filter all-list order");
            list_cursor = list_cursor->next_all;
        }
    }
    expect_true(list_cursor == 0,
                "slab candidate filter all-list exact length");

    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    decommit_before = state.decommit_count;
    state.check_calls = 0;
    state.check_failures = 0;
    state.run_check = 1;
    expect_ok(api->thread_ctx_flush(heap, contexts[1]),
              "slab candidate filter high ctx flush");
    state.run_check = 0;
    expect_true(state.decommit_count == decommit_before + 1 &&
                    state.last_decommit_addr ==
                        (void *)groups[1][0]->base &&
                    state.last_decommit_size ==
                        (HIGH_SLAB_COUNT - 1u) * page_size,
                "slab candidate filter primes five stale candidates");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "slab candidate filter high flush reentrant check");
    for (i = 0u; i < HIGH_SLAB_COUNT - 1u; ++i) {
        expect_true((groups[1][i]->flags &
                     UHM_SLAB_FLAG_DECOMMITTED) != 0u,
                    "slab candidate filter high stale slab decommitted");
    }
    expect_true((groups[1][HIGH_SLAB_COUNT - 1u]->flags &
                 (UHM_SLAB_FLAG_DECOMMITTED | UHM_SLAB_FLAG_VM_BUSY)) == 0u &&
                    heap->core.slab_committed_empty_count[size_class] == 1u,
                "slab candidate filter retains one high committed slab");

    decommit_before = state.decommit_count;
    decommit_events_before = heap->core.decommit_events;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    state.check_calls = 0;
    state.check_failures = 0;
    state.run_check = 1;
    expect_ok(api->thread_ctx_flush(heap, contexts[0]),
              "slab candidate filter low ctx flush");
    state.run_check = 0;
    expect_true(state.decommit_count == decommit_before + 1 &&
                    heap->core.vm_decommit_calls ==
                        vm_decommit_calls_before + 1u,
                "slab candidate filter preserves one VM batch call");
    expect_true(state.last_decommit_addr == (void *)groups[0][0]->base &&
                    state.last_decommit_size ==
                        LOW_SLAB_COUNT * page_size,
                "slab candidate filter batches all seven ready pages");
    expect_true(heap->core.decommit_events ==
                    decommit_events_before + LOW_SLAB_COUNT,
                "slab candidate filter accounts seven logical pages");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "slab candidate filter low flush reentrant check");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "slab candidate filter VM call stays outside heap lock");
    expect_true(contexts[0]->local_cache_count == 0u &&
                    contexts[0]->retained_slab_pages == 0u &&
                    heap->core.slab_committed_empty_count[size_class] == 1u,
                "slab candidate filter drains cache and preserves high water");
    for (i = 0u; i < LOW_SLAB_COUNT; ++i) {
        expect_true((groups[0][i]->flags &
                     UHM_SLAB_FLAG_DECOMMITTED) != 0u &&
                        (groups[0][i]->flags & UHM_SLAB_FLAG_VM_BUSY) == 0u &&
                        groups[0][i]->free_head == 0 &&
                        test_slab_remote_pending(groups[0][i]) == 0u &&
                        test_slab_remote_inflight_mirror(groups[0][i]) == 0u,
                    "slab candidate filter decommits clean low slab");
    }
    expect_true((groups[1][HIGH_SLAB_COUNT - 1u]->flags &
                 UHM_SLAB_FLAG_DECOMMITTED) == 0u,
                "slab candidate filter leaves original high-water slab");
    expect_ok(api->check(heap, 0),
              "slab candidate filter final heap check");

    heap->core.test_slab_empty_decommit_high_water_override = 0u;
    free(ptrs);
    for (group_index = 0u; group_index < GROUP_COUNT; ++group_index) {
        api->thread_ctx_close(heap, contexts[group_index]);
    }
    expect_ok(api->close(heap), "slab candidate filter heap close");
    for (group_index = 0u; group_index < GROUP_COUNT; ++group_index) {
        test_vm_release_alloc(&ctx_mem[group_index]);
    }
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_recommit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *first;
    void *reuse;
    uhm_slab *first_slab;
    uhm_slab *pressure_slab = 0;
    size_t slot_count;
    size_t slab_count;
    size_t ptr_count;
    size_t pressure_start;
    size_t i;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_commit_calls_before;
    int commit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "slab recommit unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "slab recommit unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab recommit unlock usermode vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "slab recommit unlock heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab recommit unlock api");
    state.heap = heap;
    state.api = api;

    first = api->malloc(heap, 512u);
    expect_true(first != 0, "slab recommit unlock first allocation");
    first_slab = test_find_slab_by_payload(heap, first);
    expect_true(first_slab != 0, "slab recommit unlock first slab");
    slot_count = first_slab->total_slots;
    slab_count = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER + 1u;
    ptr_count = slot_count * slab_count;
    pressure_start =
        slot_count * UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    ptrs = (void **)calloc(ptr_count, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab recommit unlock pointer table");
    ptrs[0] = first;

    for (i = 1; i < ptr_count; ++i) {
        ptrs[i] = api->malloc(heap, 512u);
        expect_true(ptrs[i] != 0, "slab recommit unlock fill slabs");
        if (i == pressure_start) {
            pressure_slab = test_find_slab_by_payload(heap, ptrs[i]);
            expect_true(pressure_slab != 0 && pressure_slab != first_slab,
                        "slab recommit unlock pressure slab");
        }
    }
    for (i = 0; i < ptr_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(pressure_slab != 0 &&
                    (pressure_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0,
                "slab recommit unlock decommitted before reuse");
    expect_ok(api->check(heap, 0), "slab recommit unlock check before");

    for (i = 0; i < pressure_start; ++i) {
        ptrs[i] = api->malloc(heap, 512u);
        expect_true(ptrs[i] != 0,
                    "slab recommit unlock consume committed slab");
        expect_true(test_find_slab_by_payload(heap, ptrs[i]) != pressure_slab,
                    "slab recommit unlock committed slab consumed first");
    }
    expect_true((pressure_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0,
                "slab recommit unlock pressure slab still decommitted");

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    commit_count_before = state.commit_count;
    state.check_calls = 0;
    state.run_check = 1;
    reuse = api->malloc(heap, 512u);
    state.run_check = 0;
    ptrs[pressure_start] = reuse;

    expect_true(reuse != 0, "slab recommit unlock reuse allocation");
    expect_true(test_find_slab_by_payload(heap, reuse) == pressure_slab,
                "slab recommit unlock decommitted slab reused");
    expect_true(state.commit_count == commit_count_before + 1,
                "slab recommit unlock commit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "slab recommit unlock reentrant check ok");
    expect_ok(state.check_result, "slab recommit unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "slab recommit unlock commit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before,
                "slab recommit unlock commit not counted lock-held");
    expect_true((pressure_slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0,
                "slab recommit unlock busy flag cleared");
    expect_true((pressure_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab recommit unlock decommitted flag cleared");
    expect_ok(api->check(heap, 0), "slab recommit unlock check after");

    for (i = 0; i <= pressure_start; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    free(ptrs);
    expect_ok(api->close(heap), "slab recommit unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_new_commit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_slab *slab;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_commit_calls_before;
    int commit_count_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "slab new commit unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "slab new commit unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab new commit unlock usermode vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;

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

    expect_ok(uhm_open(&config, &heap), "slab new commit unlock heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab new commit unlock api");
    state.heap = heap;
    state.api = api;

    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    commit_count_before = state.commit_count;
    state.run_check = 1;
    ptr = api->malloc(heap, 512u);
    state.run_check = 0;

    expect_true(ptr != 0, "slab new commit unlock allocation");
    slab = test_find_slab_by_payload(heap, ptr);
    expect_true(slab != 0, "slab new commit unlock slab lookup");
    expect_true(state.commit_count == commit_count_before + 1,
                "slab new commit unlock commit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "slab new commit unlock reentrant check ok");
    expect_ok(state.check_result, "slab new commit unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "slab new commit unlock commit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before,
                "slab new commit unlock commit not counted lock-held");
    expect_true(heap->core.low_watermark_claim_inflight == 0u,
                "slab new commit unlock low claim cleared");
    expect_ok(api->check(heap, 0), "slab new commit unlock check after");

    api->free(heap, ptr);
    expect_ok(api->close(heap), "slab new commit unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

