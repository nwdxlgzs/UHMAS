static void test_slab_lockless_class_queue_close_batch_publish(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *second;
    uhm_slab_slot *first_slot;
    uhm_slab_slot *second_slot;
    uhm_slab *slab;
    uint64_t raw_before;
    uint64_t raw_after;
    uint64_t counter_before;
    uint64_t counter_after;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless class close batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless class close batch arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless class close batch usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "lockless class close batch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless class close batch api");

    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0 && first != second,
                "lockless class close batch allocations");
    first_slot = test_slab_slot_from_payload(first);
    second_slot = test_slab_slot_from_payload(second);
    slab = first_slot ? first_slot->slab : 0;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC &&
                    second_slot != 0 && second_slot->slab == slab,
                "lockless class close batch same slab");

    api->free(heap, first);
    api->free(heap, second);
    expect_true(test_slab_remote_class_queue_contains(heap, slab,
                                                      first_slot) &&
                    test_slab_remote_class_queue_contains(heap, slab,
                                                          second_slot),
                "lockless class close batch queued");
    raw_before = atomic_load_explicit(&slab->raw_publish_seq,
                                      UHM_ATOMIC_ACQUIRE);
    counter_before = atomic_load_explicit(&slab->remote_counter_seq,
                                          UHM_ATOMIC_ACQUIRE);
    expect_ok(api->close(heap), "lockless class close batch close");
    raw_after = atomic_load_explicit(&slab->raw_publish_seq,
                                     UHM_ATOMIC_ACQUIRE);
    counter_after = atomic_load_explicit(&slab->remote_counter_seq,
                                         UHM_ATOMIC_ACQUIRE);
    expect_true(raw_after - raw_before == (UINT64_C(1) << 32) &&
                    (raw_after & UINT64_C(0xffffffff)) == 0,
                "lockless class close batch single raw publish window");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "lockless class close batch counters drained");
    expect_true(counter_after - counter_before == (UINT64_C(1) << 32) &&
                    (counter_after & UINT64_C(0xffffffff)) == 0,
                "lockless class close batch single counter window");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "lockless class close batch reader released");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_class_queue_owned_arena_releases_without_decommit(
    const uhm_storage_info *heap_info) {
    const size_t arena_size = 1024u * 1024u;
    test_alloc heap_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uint64_t vm_decommit_calls_before;
    uint64_t vm_release_calls_before;
    size_t i;

    if (!test_lockless_remote_primitives_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "lockless class owned heap storage alloc");
    expect_ok(uhm_platform_vm_ops_init(&vm),
              "lockless class owned platform vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = 0;
    config.arena_size = arena_size;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "lockless class owned heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && heap->core.owns_arena != 0u,
                "lockless class owned api and ownership");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless class owned allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot ? slot->slab : 0;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless class owned slab metadata");
    api->free(heap, ptr);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless class owned queued before close");

    for (i = 0u; i < UHM_SIZE_CLASS_COUNT; ++i) {
        heap->core.slab_empty_committed_cache_high_water[i] =
            uhm_slab_empty_committed_cache_state_make(
                UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER, 0u);
    }
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    vm_release_calls_before = heap->core.vm_release_calls;
    expect_ok(api->close(heap), "lockless class owned close");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before,
                "lockless class owned skips redundant decommit");
    expect_true(heap->core.vm_release_calls == vm_release_calls_before + 1u,
                "lockless class owned releases arena once");

    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_class_queue_close_tag_wrap_self_reader(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_tagged_freelist *queue;
    uint64_t deferrals_before;
    uint64_t overflows_before;

    if (!test_lockless_remote_primitives_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless class close wrap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless class close wrap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless class close wrap usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "lockless class close wrap heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless class close wrap api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless class close wrap allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot ? slot->slab : 0;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless class close wrap slab metadata");
    queue = &heap->core.slab_remote_class_lf[slab->size_class].queue;

    api->free(heap, ptr);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless class close wrap queued");
    atomic_store_explicit(
        &queue->head,
        uhm_tagged_freelist_pack(&slot->remote_link,
                                 UHM_TAGGED_FREELIST_TAG_MASK),
        UHM_ATOMIC_RELEASE);
    deferrals_before = uhm_tagged_freelist_tag_wrap_deferrals(queue);
    overflows_before = uhm_tagged_freelist_tag_overflows(queue);

    expect_ok(api->close(heap), "lockless class close wrap close");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless class close wrap queue drained");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "lockless class close wrap counters drained");
    expect_true(uhm_tagged_freelist_tag_wrap_deferrals(queue) ==
                    deferrals_before,
                "lockless class close wrap no self-reader deferral");
    expect_true(uhm_tagged_freelist_tag_overflows(queue) > overflows_before,
                "lockless class close wrap overflow recorded");
    expect_true(uhm_tagged_freelist_last_wrap_epoch(queue) != 0,
                "lockless class close wrap epoch recorded");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "lockless class close wrap reader released");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_class_queue_close_publish_busy_restores(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *second;
    uhm_slab_slot *first_slot;
    uhm_slab_slot *second_slot;
    uhm_slab *slab;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless class busy heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless class busy arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless class busy usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "lockless class busy heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless class busy api");

    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0 && first != second,
                "lockless class busy allocations");
    first_slot = test_slab_slot_from_payload(first);
    second_slot = test_slab_slot_from_payload(second);
    slab = first_slot ? first_slot->slab : 0;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC &&
                    second_slot != 0 && second_slot->slab == slab,
                "lockless class busy same slab");

    api->free(heap, first);
    api->free(heap, second);
    expect_true(test_slab_remote_class_queue_contains(heap, slab,
                                                      first_slot) &&
                    test_slab_remote_class_queue_contains(heap, slab,
                                                          second_slot),
                "lockless class busy queued");
    heap->core.fault_lockless_remote_drain_countdown = 1u;
    expect_true(api->close(heap) == UHM_ERR_BUSY,
                "lockless class busy close returns busy");
    expect_true(heap->core.fault_lockless_remote_drain_countdown == 0,
                "lockless class busy fault consumed");
    expect_true(test_slab_remote_class_queue_contains(heap, slab,
                                                      first_slot) &&
                    test_slab_remote_class_queue_contains(heap, slab,
                                                          second_slot),
                "lockless class busy restores class queue");
    expect_true(test_slab_remote_pending_mirror(slab) == 2u &&
                    test_slab_remote_pending(slab) == 2u &&
                    test_slab_remote_inflight_mirror(slab) == 2u,
                "lockless class busy keeps counters");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "lockless class busy no reader leak");
    expect_ok(api->check(heap, 0), "lockless class busy heap still live");
    expect_ok(api->close(heap), "lockless class busy retry close");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "lockless class busy retry drains counters");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_class_queue_close_corruption_restores(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uint16_t saved_size_class;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless class corrupt heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless class corrupt arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless class corrupt usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap),
              "lockless class corrupt heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless class corrupt api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless class corrupt allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot ? slot->slab : 0;
    expect_true(slot != 0 && slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless class corrupt metadata");

    api->free(heap, ptr);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless class corrupt queued");
    saved_size_class = slot->size_class;
    slot->size_class = UHM_SIZE_CLASS_COUNT;
    expect_true(api->close(heap) == UHM_ERR_CORRUPTION,
                "lockless class corrupt close returns corruption");
    slot->size_class = saved_size_class;
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless class corrupt restores queue");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "lockless class corrupt keeps counters");
    expect_ok(api->check(heap, 0), "lockless class corrupt heap still live");
    expect_ok(api->close(heap), "lockless class corrupt retry close");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "lockless class corrupt retry drains counters");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

