static void test_slab_remote_drain_reservation_rollback(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *reuse;
    void *descriptor_reuse;
    uhm_slab *first_slab;
    uhm_slab *second_slab = 0;
    uhm_slab_slot *last_slot;
    uhm_page_record *record;
    uhm_result last_error = UHM_OK;
    uint32_t platform_error = 0;
    uint64_t saved_generation;
    uint64_t raw_publish_seq_before;
    uint64_t raw_publish_seq_after;
    size_t first_page;
    size_t page_count;
    size_t slot_count;
    size_t i;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab drain reservation heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab drain reservation arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab drain reservation usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "slab drain reservation heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab drain reservation api");

    ptrs = (void **)calloc(page_size, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab drain reservation pointer table");

    ptrs[0] = api->malloc(heap, 64u);
    expect_true(ptrs[0] != 0, "slab drain reservation first allocation");
    first_slab = test_slab_slot_from_payload(ptrs[0])->slab;
    slot_count = first_slab->total_slots;
    expect_true(slot_count != 0 && slot_count * 2u <= page_size,
                "slab drain reservation slot count");

    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0,
                    "slab drain reservation fill first slab");
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == first_slab,
                    "slab drain reservation first slab ownership");
    }
    for (i = slot_count; i < slot_count * 2u; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0,
                    "slab drain reservation fill second slab");
        if (i == slot_count) {
            second_slab = test_slab_slot_from_payload(ptrs[i])->slab;
            expect_true(second_slab != 0 && second_slab != first_slab,
                        "slab drain reservation second slab created");
        }
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == second_slab,
                    "slab drain reservation second slab ownership");
    }

    for (i = 0; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(first_slab->state == UHM_SLAB_STATE_EMPTY &&
                    (first_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab drain reservation first slab empty committed");

    for (i = slot_count; i + 1u < slot_count * 2u; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    last_slot = test_slab_slot_from_payload(ptrs[slot_count * 2u - 1u]);
    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    heap->core.fault_lockless_remote_drain_countdown = 1u;
    api->free(heap, ptrs[slot_count * 2u - 1u]);
    ptrs[slot_count * 2u - 1u] = 0;

    expect_true(heap->core.fault_lockless_remote_drain_countdown == 0,
                "slab drain reservation fault consumed");
    expect_ok(api->get_last_error(heap, &last_error, &platform_error),
              "slab drain reservation last error query");
    expect_true(last_error == UHM_ERR_OUT_OF_MEMORY,
                "slab drain reservation reports rollback fault");
    expect_true((second_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab drain reservation rollback prevents decommit");
    expect_true(test_slab_remote_pending_mirror(second_slab) == slot_count &&
                    test_slab_remote_pending(second_slab) == slot_count &&
                    test_slab_remote_inflight_mirror(second_slab) == slot_count,
                "slab drain reservation rollback keeps counters");
    expect_true(atomic_load_explicit(&second_slab->remote_drain_active,
                                     UHM_ATOMIC_ACQUIRE) == 0,
                "slab drain reservation rollback clears drain owner");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "slab drain reservation rollback clears registered reader");
    expect_true((test_slab_slot_state_flags_load(last_slot) &
                 UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) == 0,
                "slab drain reservation rollback clears draining flag");
    expect_true((test_slab_slot_state_flags_load(last_slot) &
                 UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) == 0,
                "slab drain reservation rollback restores accounting flag");
    expect_true(test_slab_remote_queue_contains(second_slab, last_slot),
                "slab drain reservation rollback restores queue");
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        size_t hazard_index;
        expect_true(atomic_load_explicit(
                        &heap->core.lockless_remote_readers[i].depth,
                        UHM_ATOMIC_ACQUIRE) == 0,
                    "slab drain reservation reader depth cleared");
        expect_true(atomic_load_explicit(
                        &heap->core.lockless_remote_readers[i].epoch,
                        UHM_ATOMIC_ACQUIRE) == 0,
                    "slab drain reservation reader epoch cleared");
        for (hazard_index = 0; hazard_index < UHM_METADATA_HAZARD_SLOTS;
             ++hazard_index) {
            expect_true(atomic_load_explicit(
                            &heap->core.lockless_remote_readers[i]
                                 .hazards[hazard_index],
                            UHM_ATOMIC_ACQUIRE) == (uintptr_t)0,
                        "slab drain reservation reader hazard cleared");
        }
    }
    expect_ok(api->check(heap, 0), "slab drain reservation rollback check");

    raw_publish_seq_before = atomic_load_explicit(
        &second_slab->raw_publish_seq, UHM_ATOMIC_ACQUIRE);
    reuse = api->malloc(heap, 64u);
    raw_publish_seq_after = atomic_load_explicit(
        &second_slab->raw_publish_seq, UHM_ATOMIC_ACQUIRE);
    expect_true(reuse != 0, "slab drain reservation later reuse");
    expect_true(raw_publish_seq_after > raw_publish_seq_before &&
                    (raw_publish_seq_after & 1u) == 0,
                "slab drain reservation raw publish sequence advanced");
    expect_true(test_slab_remote_pending_mirror(second_slab) == 0 &&
                    test_slab_remote_pending(second_slab) == 0 &&
                    test_slab_remote_inflight_mirror(second_slab) == 0,
                "slab drain reservation later drain clears counters");
    expect_true(atomic_load_explicit(&second_slab->remote_drain_active,
                                     UHM_ATOMIC_ACQUIRE) == 0,
                "slab drain reservation later drain clears owner");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "slab drain reservation later drain clears registered reader");
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        size_t hazard_index;
        expect_true(atomic_load_explicit(
                        &heap->core.lockless_remote_readers[i].depth,
                        UHM_ATOMIC_ACQUIRE) == 0,
                    "slab drain reservation later reader depth cleared");
        expect_true(atomic_load_explicit(
                        &heap->core.lockless_remote_readers[i].epoch,
                        UHM_ATOMIC_ACQUIRE) == 0,
                    "slab drain reservation later reader epoch cleared");
        for (hazard_index = 0; hazard_index < UHM_METADATA_HAZARD_SLOTS;
             ++hazard_index) {
            expect_true(atomic_load_explicit(
                            &heap->core.lockless_remote_readers[i]
                                 .hazards[hazard_index],
                            UHM_ATOMIC_ACQUIRE) == (uintptr_t)0,
                        "slab drain reservation later reader hazard cleared");
        }
    }
    for (i = 0; i < second_slab->total_slots; ++i) {
        uhm_slab_slot *check_slot =
            (uhm_slab_slot *)(second_slab->base +
                              i * second_slab->slot_stride);
        expect_true((test_slab_slot_state_flags_load(check_slot) &
                     UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) == 0,
                    "slab drain reservation success clears accounted flag");
    }
    expect_ok(api->check(heap, 0), "slab drain reservation later check");

    last_slot = test_slab_slot_from_payload(reuse);
    expect_true(last_slot != 0 && last_slot->slab == second_slab,
                "slab drain reservation descriptor live slot");
    expect_true(test_shadow_page_span(heap, second_slab->base,
                                      second_slab->size, &first_page,
                                      &page_count) &&
                    page_count != 0,
                "slab drain reservation descriptor page span");
    record = test_page_record_at(heap, first_page);
    expect_true(record != 0,
                "slab drain reservation descriptor page record");
    saved_generation = record->generation;
    record->generation = saved_generation + 1u;
    api->free(heap, reuse);
    reuse = 0;
    expect_last_error(api, heap, UHM_ERR_CORRUPTION, 0u,
                      "slab drain reservation descriptor last error");
    expect_true(atomic_load_explicit(&second_slab->remote_drain_active,
                                     UHM_ATOMIC_ACQUIRE) == 0,
                "slab drain reservation descriptor clears drain owner");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "slab drain reservation descriptor clears registered reader");
    expect_true(test_slab_remote_pending_mirror(second_slab) == 1u &&
                    test_slab_remote_pending(second_slab) == 1u &&
                    test_slab_remote_inflight_mirror(second_slab) == 1u,
                "slab drain reservation descriptor preserves counters");
    expect_true(test_slab_remote_queue_contains(second_slab, last_slot),
                "slab drain reservation descriptor keeps queue");
    record->generation = saved_generation;
    expect_ok(api->check(heap, 0),
              "slab drain reservation descriptor restored check");
    descriptor_reuse = api->malloc(heap, 64u);
    expect_true(descriptor_reuse != 0,
                "slab drain reservation descriptor later reuse");
    expect_true(test_slab_remote_pending_mirror(second_slab) == 0 &&
                    test_slab_remote_pending(second_slab) == 0 &&
                    test_slab_remote_inflight_mirror(second_slab) == 0,
                "slab drain reservation descriptor later drain counters");
    expect_ok(api->check(heap, 0),
              "slab drain reservation descriptor later check");

    api->free(heap, descriptor_reuse);
    expect_ok(api->close(heap), "slab drain reservation close");

    free(ptrs);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_drain_reservation_skips_unreserved_fallback(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *reuse;
    uhm_slab *first_slab;
    uhm_slab *second_slab = 0;
    uhm_slab_slot *fallback_slot;
    uhm_tagged_freelist_node *detached;
    uint64_t raw_before;
    uint64_t raw_after;
    uint64_t counter_before;
    uint64_t counter_after;
    size_t slot_count;
    size_t i;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab drain skip heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab drain skip arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab drain skip usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "slab drain skip heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab drain skip api");

    ptrs = (void **)calloc(page_size, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab drain skip pointer table");

    ptrs[0] = api->malloc(heap, 64u);
    expect_true(ptrs[0] != 0, "slab drain skip first allocation");
    first_slab = test_slab_slot_from_payload(ptrs[0])->slab;
    slot_count = first_slab->total_slots;
    expect_true(slot_count != 0 && slot_count * 2u <= page_size,
                "slab drain skip slot count");

    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0, "slab drain skip fill first slab");
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == first_slab,
                    "slab drain skip first slab ownership");
    }
    for (i = slot_count; i < slot_count * 2u; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0, "slab drain skip fill second slab");
        if (i == slot_count) {
            second_slab = test_slab_slot_from_payload(ptrs[i])->slab;
            expect_true(second_slab != 0 && second_slab != first_slab,
                        "slab drain skip second slab created");
        }
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == second_slab,
                    "slab drain skip second slab ownership");
    }

    for (i = 0; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(first_slab->state == UHM_SLAB_STATE_EMPTY &&
                    (first_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab drain skip first slab empty committed");

    fallback_slot = test_slab_slot_from_payload(ptrs[slot_count]);
    api->free(heap, ptrs[slot_count]);
    ptrs[slot_count] = 0;
    detached = uhm_tagged_freelist_take_all(&second_slab->remote_free_lf);
    expect_true(detached == &fallback_slot->remote_link &&
                    uhm_tagged_freelist_next_load(detached) == 0,
                "slab drain skip detaches fallback node");
    uhm_tagged_freelist_next_store(detached, 0);
    fallback_slot->next_free = second_slab->remote_free_head;
    second_slab->remote_free_head = fallback_slot;
    expect_ok(api->check(heap, 0),
              "slab drain skip fallback node check");

    for (i = slot_count + 1u; i + 1u < slot_count * 2u; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    raw_before = atomic_load_explicit(&second_slab->raw_publish_seq,
                                      UHM_ATOMIC_ACQUIRE);
    counter_before = atomic_load_explicit(&second_slab->remote_counter_seq,
                                          UHM_ATOMIC_ACQUIRE);
    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    api->free(heap, ptrs[slot_count * 2u - 1u]);
    ptrs[slot_count * 2u - 1u] = 0;
    raw_after = atomic_load_explicit(&second_slab->raw_publish_seq,
                                     UHM_ATOMIC_ACQUIRE);
    counter_after = atomic_load_explicit(&second_slab->remote_counter_seq,
                                         UHM_ATOMIC_ACQUIRE);

    expect_true((second_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab drain skip keeps residual remote slab committed");
    expect_true(raw_after - raw_before == (UINT64_C(1) << 32) &&
                    (raw_after & UINT64_C(0xffffffff)) == 0,
                "slab drain skip reservation single raw publish window");
    expect_true(counter_after - counter_before ==
                        UINT64_C(3) * (UINT64_C(1) << 32) &&
                    (counter_after & UINT64_C(0xffffffff)) == 0,
                "slab drain skip reservation batched counter window");
    expect_true(second_slab->remote_free_head == fallback_slot &&
                    test_slab_remote_queue_contains(second_slab,
                                                    fallback_slot),
                "slab drain skip leaves unreserved fallback node");
    expect_true(test_slab_remote_pending_mirror(second_slab) == 1u &&
                    test_slab_remote_pending(second_slab) == 1u &&
                    test_slab_remote_inflight_mirror(second_slab) == 1u,
                "slab drain skip leaves residual counters");
    expect_true(uhm_tagged_freelist_unpack_ptr(atomic_load_explicit(
                    &second_slab->remote_free_lf.head,
                    UHM_ATOMIC_ACQUIRE)) == 0,
                "slab drain skip consumed only reserved tagged nodes");
    expect_ok(api->check(heap, 0), "slab drain skip residual check");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse != 0, "slab drain skip later allocation");
    expect_true(test_slab_remote_pending_mirror(second_slab) == 0 &&
                    test_slab_remote_pending(second_slab) == 0 &&
                    test_slab_remote_inflight_mirror(second_slab) == 0,
                "slab drain skip later drain clears counters");
    expect_ok(api->check(heap, 0), "slab drain skip later check");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "slab drain skip close");

    free(ptrs);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_drain_reservation_reader_busy_defers(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *reuse;
    uhm_slab *first_slab;
    uhm_slab *second_slab = 0;
    size_t slot_count;
    size_t i;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab drain reader busy heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab drain reader busy arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab drain reader busy usermode vm init");

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
              "slab drain reader busy heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab drain reader busy api");

    ptrs = (void **)calloc(page_size, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab drain reader busy pointer table");

    ptrs[0] = api->malloc(heap, 64u);
    expect_true(ptrs[0] != 0, "slab drain reader busy first allocation");
    first_slab = test_slab_slot_from_payload(ptrs[0])->slab;
    slot_count = first_slab->total_slots;
    expect_true(slot_count != 0 && slot_count * 2u <= page_size,
                "slab drain reader busy slot count");

    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0,
                    "slab drain reader busy fill first slab");
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == first_slab,
                    "slab drain reader busy first slab ownership");
    }
    for (i = slot_count; i < slot_count * 2u; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0,
                    "slab drain reader busy fill second slab");
        if (i == slot_count) {
            second_slab = test_slab_slot_from_payload(ptrs[i])->slab;
            expect_true(second_slab != 0 && second_slab != first_slab,
                        "slab drain reader busy second slab created");
        }
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == second_slab,
                    "slab drain reader busy second slab ownership");
    }

    for (i = 0; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(first_slab->state == UHM_SLAB_STATE_EMPTY &&
                    (first_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab drain reader busy first slab empty committed");

    for (i = slot_count; i + 1u < slot_count * 2u; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }

    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        expect_true(uhm_metadata_reader_try_begin_inactive(
                        &heap->core.lockless_remote_readers[i], heap),
                    "slab drain reader busy occupy reader");
    }
    api->free(heap, ptrs[slot_count * 2u - 1u]);
    ptrs[slot_count * 2u - 1u] = 0;

    expect_true((second_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab drain reader busy keeps slab committed");
    expect_true(test_slab_remote_pending_mirror(second_slab) == slot_count &&
                    test_slab_remote_pending(second_slab) == slot_count &&
                    test_slab_remote_inflight_mirror(second_slab) == slot_count,
                "slab drain reader busy defers counters");
    expect_true(!test_slab_remote_queue_empty(second_slab),
                "slab drain reader busy keeps remote queue");
    expect_ok(api->check(heap, 0), "slab drain reader busy active check");

    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        expect_true(uhm_metadata_reader_end(
                        &heap->core.lockless_remote_readers[i]),
                    "slab drain reader busy release reader");
    }
    expect_ok(api->check(heap, 0), "slab drain reader busy release check");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse != 0, "slab drain reader busy later allocation");
    expect_true(test_slab_remote_pending_mirror(second_slab) == 0 &&
                    test_slab_remote_pending(second_slab) == 0 &&
                    test_slab_remote_inflight_mirror(second_slab) == 0,
                "slab drain reader busy later drain clears counters");
    expect_ok(api->check(heap, 0), "slab drain reader busy later check");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "slab drain reader busy close");

    free(ptrs);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_drain_decommit_trylock_busy_defers(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *reuse;
    uhm_slab *first_slab;
    uhm_slab *second_slab = 0;
    uint64_t raw_before;
    uint64_t raw_after;
    size_t decommitted_before;
    size_t slot_count;
    size_t i;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab drain decommit trylock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab drain decommit trylock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab drain decommit trylock usermode vm init");

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
              "slab drain decommit trylock heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab drain decommit trylock api");

    ptrs = (void **)calloc(page_size, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab drain decommit trylock pointer table");

    ptrs[0] = api->malloc(heap, 64u);
    expect_true(ptrs[0] != 0,
                "slab drain decommit trylock first allocation");
    first_slab = test_slab_slot_from_payload(ptrs[0])->slab;
    slot_count = first_slab->total_slots;
    expect_true(slot_count != 0 && slot_count * 2u <= page_size,
                "slab drain decommit trylock slot count");

    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0,
                    "slab drain decommit trylock fill first slab");
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == first_slab,
                    "slab drain decommit trylock first slab ownership");
    }
    for (i = slot_count; i < slot_count * 2u; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0,
                    "slab drain decommit trylock fill second slab");
        if (i == slot_count) {
            second_slab = test_slab_slot_from_payload(ptrs[i])->slab;
            expect_true(second_slab != 0 && second_slab != first_slab,
                        "slab drain decommit trylock second slab created");
        }
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == second_slab,
                    "slab drain decommit trylock second slab ownership");
    }

    for (i = 0; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }
    expect_true(first_slab->state == UHM_SLAB_STATE_EMPTY &&
                    (first_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab drain decommit trylock first slab empty committed");

    for (i = slot_count; i + 1u < slot_count * 2u; ++i) {
        api->free(heap, ptrs[i]);
        ptrs[i] = 0;
    }

    raw_before = atomic_load_explicit(&second_slab->raw_publish_seq,
                                      UHM_ATOMIC_ACQUIRE);
    decommitted_before = heap->stats.decommitted_bytes;
    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    heap->core.fault_lockless_remote_decommit_trylock_countdown = 1u;
    api->free(heap, ptrs[slot_count * 2u - 1u]);
    ptrs[slot_count * 2u - 1u] = 0;
    raw_after = atomic_load_explicit(&second_slab->raw_publish_seq,
                                     UHM_ATOMIC_ACQUIRE);

    expect_true(heap->core.fault_lockless_remote_decommit_trylock_countdown == 0,
                "slab drain decommit trylock fault consumed");
    expect_true(raw_after > raw_before && (raw_after & 1u) == 0,
                "slab drain decommit trylock raw publish completed");
    expect_true(test_slab_remote_pending_mirror(second_slab) == 0 &&
                    test_slab_remote_pending(second_slab) == 0 &&
                    test_slab_remote_inflight_mirror(second_slab) == 0,
                "slab drain decommit trylock counters cleared");
    expect_true(test_slab_remote_queue_empty(second_slab),
                "slab drain decommit trylock queue empty");
    expect_true((second_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab drain decommit trylock keeps slab committed");
    expect_true(heap->stats.decommitted_bytes == decommitted_before,
                "slab drain decommit trylock leaves stats unchanged");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "slab drain decommit trylock releases registered reader");
    expect_ok(api->check(heap, 0),
              "slab drain decommit trylock deferred check");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse != 0, "slab drain decommit trylock later allocation");
    api->free(heap, reuse);
    expect_true(heap->stats.decommitted_bytes >=
                    decommitted_before + page_size,
                "slab drain decommit trylock later decommits");
    expect_ok(api->check(heap, 0),
              "slab drain decommit trylock final check");
    expect_ok(api->close(heap), "slab drain decommit trylock close");

    free(ptrs);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_queue_tag_wrap_reader_gate(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *fallback;
    void *reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_tagged_freelist *queue;
    uint64_t deferrals_before;
    uint64_t overflows_before;
    uint64_t valid_wrap_epoch;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab remote wrap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab remote wrap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab remote wrap usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "slab remote wrap heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab remote wrap api");

    first = api->malloc(heap, 64u);
    expect_true(first != 0, "slab remote wrap allocation");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "slab remote wrap slab metadata");
    queue = &slab->remote_free_lf;

    api->free(heap, first);
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "slab remote wrap queued");
    atomic_store_explicit(
        &queue->head,
        uhm_tagged_freelist_pack(&slot->remote_link,
                                 UHM_TAGGED_FREELIST_TAG_MASK),
        UHM_ATOMIC_RELEASE);
    deferrals_before = uhm_tagged_freelist_tag_wrap_deferrals(queue);
    overflows_before = uhm_tagged_freelist_tag_overflows(queue);

    expect_true(uhm_metadata_reader_begin(
                    &heap->core.lockless_remote_readers[0], heap),
                "slab remote wrap external reader begin");
    fallback = api->malloc(heap, 64u);
    expect_true(fallback != 0 && fallback != first,
                "slab remote wrap active reader uses fallback slot");
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "slab remote wrap active reader keeps queue");
    expect_true(uhm_tagged_freelist_tag_wrap_deferrals(queue) >
                    deferrals_before,
                "slab remote wrap deferral counted");
    expect_true(uhm_tagged_freelist_tag_overflows(queue) == overflows_before,
                "slab remote wrap does not wrap under external reader");
    expect_ok(api->check(heap, 0), "slab remote wrap active reader check");
    expect_true(uhm_metadata_reader_end(
                    &heap->core.lockless_remote_readers[0]),
                "slab remote wrap external reader end");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == first,
                "slab remote wrap reuses after reader quiesces");
    expect_true(!test_slab_remote_queue_contains(slab, slot),
                "slab remote wrap queue consumed later");
    expect_true(uhm_tagged_freelist_tag_overflows(queue) > overflows_before,
                "slab remote wrap overflow after quiescence");
    expect_true(uhm_tagged_freelist_last_wrap_epoch(queue) != 0,
                "slab remote wrap records metadata epoch");
    expect_ok(api->check(heap, 0), "slab remote wrap final check");
    valid_wrap_epoch = uhm_tagged_freelist_last_wrap_epoch(queue);
    atomic_store_explicit(&queue->last_wrap_epoch,
                          heap->core.metadata_epoch + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote wrap future epoch rejected");
    atomic_store_explicit(&queue->last_wrap_epoch, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote wrap missing epoch rejected");
    atomic_store_explicit(&queue->last_wrap_epoch, valid_wrap_epoch,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "slab remote wrap epoch restored");

    api->free(heap, reuse);
    api->free(heap, fallback);
    expect_ok(api->close(heap), "slab remote wrap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_tagged_batch_publish(
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
    void *reuse;
    uhm_slab_slot *first_slot;
    uhm_slab_slot *second_slot;
    uhm_slab *slab;
    uint64_t raw_before;
    uint64_t raw_after;
    uint64_t counter_before;
    uint64_t counter_after;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab remote tagged batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab remote tagged batch arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab remote tagged batch usermode vm init");

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
              "slab remote tagged batch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab remote tagged batch api");

    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0 && first != second,
                "slab remote tagged batch allocations");
    first_slot = test_slab_slot_from_payload(first);
    second_slot = test_slab_slot_from_payload(second);
    slab = first_slot ? first_slot->slab : 0;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC &&
                    second_slot != 0 && second_slot->slab == slab,
                "slab remote tagged batch same slab");

    api->free(heap, first);
    api->free(heap, second);
    expect_true(test_slab_remote_queue_contains(slab, first_slot) &&
                    test_slab_remote_queue_contains(slab, second_slot),
                "slab remote tagged batch queued");
    expect_true(test_slab_remote_pending_mirror(slab) == 2u &&
                    test_slab_remote_pending(slab) == 2u &&
                    test_slab_remote_inflight_mirror(slab) == 2u,
                "slab remote tagged batch counters queued");
    counter_before = atomic_load_explicit(&slab->remote_counter_seq,
                                          UHM_ATOMIC_ACQUIRE);
    raw_before = atomic_load_explicit(&slab->raw_publish_seq,
                                      UHM_ATOMIC_ACQUIRE);
    reuse = api->malloc(heap, 64u);
    raw_after = atomic_load_explicit(&slab->raw_publish_seq,
                                     UHM_ATOMIC_ACQUIRE);
    counter_after = atomic_load_explicit(&slab->remote_counter_seq,
                                         UHM_ATOMIC_ACQUIRE);
    expect_true(reuse == first || reuse == second,
                "slab remote tagged batch reuses queued slot");
    expect_true(raw_after - raw_before == (UINT64_C(1) << 32) &&
                    (raw_after & UINT64_C(0xffffffff)) == 0,
                "slab remote tagged batch single raw publish window");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "slab remote tagged batch counters drained");
    expect_true(counter_after - counter_before == (UINT64_C(1) << 32) &&
                    (counter_after & UINT64_C(0xffffffff)) == 0,
                "slab remote tagged batch single counter window");
    expect_ok(api->check(heap, 0), "slab remote tagged batch check");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "slab remote tagged batch close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_fallback_list_batch_publish(
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
    void *reuse;
    uhm_slab_slot *first_slot;
    uhm_slab_slot *second_slot;
    uhm_slab *slab;
    uhm_tagged_freelist_node *detached;
    uint64_t raw_before;
    uint64_t raw_after;
    uint64_t counter_before;
    uint64_t counter_after;
    size_t expected_inflight;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab remote fallback batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab remote fallback batch arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab remote fallback batch usermode vm init");

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
              "slab remote fallback batch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab remote fallback batch api");

    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0 && first != second,
                "slab remote fallback batch allocations");
    first_slot = test_slab_slot_from_payload(first);
    second_slot = test_slab_slot_from_payload(second);
    slab = first_slot ? first_slot->slab : 0;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC &&
                    second_slot != 0 && second_slot->slab == slab,
                "slab remote fallback batch same slab");

    api->free(heap, first);
    api->free(heap, second);
    expected_inflight = uhm_tagged_freelist_is_supported() ? 2u : 0u;
    detached = uhm_tagged_freelist_take_all(&slab->remote_free_lf);
    while (detached) {
        uhm_tagged_freelist_node *next =
            uhm_tagged_freelist_next_load(detached);
        uhm_slab_slot *slot = test_slab_slot_from_remote_link(detached);
        expect_true(slot != 0 && slot->slab == slab,
                    "slab remote fallback batch detach slot");
        uhm_tagged_freelist_next_store(detached, 0);
        slot->next_free = slab->remote_free_head;
        slab->remote_free_head = slot;
        detached = next;
    }
    expect_true(test_slab_remote_queue_contains(slab, first_slot) &&
                    test_slab_remote_queue_contains(slab, second_slot),
                "slab remote fallback batch queued");
    expect_true(test_slab_remote_pending_mirror(slab) == 2u &&
                    test_slab_remote_pending(slab) == 2u &&
                    test_slab_remote_inflight_mirror(slab) == expected_inflight,
                "slab remote fallback batch counters queued");
    expect_ok(api->check(heap, 0), "slab remote fallback batch queued check");

    raw_before = atomic_load_explicit(&slab->raw_publish_seq,
                                      UHM_ATOMIC_ACQUIRE);
    counter_before = atomic_load_explicit(&slab->remote_counter_seq,
                                          UHM_ATOMIC_ACQUIRE);
    reuse = api->malloc(heap, 64u);
    raw_after = atomic_load_explicit(&slab->raw_publish_seq,
                                     UHM_ATOMIC_ACQUIRE);
    counter_after = atomic_load_explicit(&slab->remote_counter_seq,
                                         UHM_ATOMIC_ACQUIRE);
    expect_true(reuse == first || reuse == second,
                "slab remote fallback batch reuses queued slot");
    expect_true(raw_after - raw_before == (UINT64_C(1) << 32) &&
                    (raw_after & UINT64_C(0xffffffff)) == 0,
                "slab remote fallback batch single raw publish window");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "slab remote fallback batch counters drained");
    expect_true(counter_after - counter_before == (UINT64_C(1) << 32) &&
                    (counter_after & UINT64_C(0xffffffff)) == 0,
                "slab remote fallback batch single counter window");
    expect_ok(api->check(heap, 0), "slab remote fallback batch check");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "slab remote fallback batch close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_pending_mask_skips_empty_class_scan(
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
    void *reuse;
    uhm_slab_slot *first_slot;
    uhm_slab *slab;
    uint16_t size_class;
    uint32_t class_bit;
    uint64_t scan_calls_before;
    uint64_t scan_slabs_before;
    int ready_note_fault_supported;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "slab pending mask heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "slab pending mask arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab pending mask usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "slab pending mask heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab pending mask api");

    uhm_slab_remote_pending_note(heap, 0u);
    uhm_slab_remote_pending_note(heap, 8u);
    expect_true((uhm_slab_remote_pending_class_mask_load(heap, 0u) &
                 uhm_slab_remote_pending_class_bit(0u)) != 0u &&
                    (uhm_slab_remote_pending_class_mask_load(heap, 8u) &
                     uhm_slab_remote_pending_class_bit(8u)) != 0u,
                "slab pending mask cross-word bits set");
    (void)uhm_slab_remote_pending_take(heap, 0u);
    expect_true((uhm_slab_remote_pending_class_mask_load(heap, 0u) &
                 uhm_slab_remote_pending_class_bit(0u)) == 0u &&
                    (uhm_slab_remote_pending_class_mask_load(heap, 8u) &
                     uhm_slab_remote_pending_class_bit(8u)) != 0u,
                "slab pending mask cross-word clear isolated");
    (void)uhm_slab_remote_pending_take(heap, 8u);

    scan_calls_before = heap->core.slab_remote_class_scan_calls;
    scan_slabs_before = heap->core.slab_remote_class_scan_slabs;
    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0 && first != second,
                "slab pending mask allocations");
    first_slot = test_slab_slot_from_payload(first);
    slab = first_slot ? first_slot->slab : 0;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC &&
                    test_slab_slot_from_payload(second)->slab == slab,
                "slab pending mask same slab");
    size_class = slab->size_class;
    class_bit = uhm_slab_remote_pending_class_bit(size_class);
    ready_note_fault_supported =
        uhm_tagged_freelist_is_supported() &&
        uhm_tagged_freelist_ptr_packable(&first_slot->remote_link);

    expect_true(heap->core.slab_remote_class_scan_calls == scan_calls_before &&
                    heap->core.slab_remote_class_scan_slabs ==
                        scan_slabs_before,
                "slab pending mask skips empty class scan");

    if (ready_note_fault_supported) {
        heap->core.fault_remote_ready_note_countdown = 1u;
    }
    api->free(heap, first);
    expect_true((!ready_note_fault_supported ||
                 heap->core.fault_remote_ready_note_countdown == 0) &&
                    (uhm_slab_remote_pending_class_mask_load(heap, size_class) &
                     class_bit) != 0u,
                ready_note_fault_supported
                    ? "slab pending mask records ready note failure"
                    : "slab pending mask records raw fallback");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == first, "slab pending mask recovery reuse");
    expect_true(heap->core.slab_remote_class_scan_calls ==
                    scan_calls_before + 1u &&
                    heap->core.slab_remote_class_scan_slabs >
                        scan_slabs_before,
                "slab pending mask performs one recovery scan");
    expect_true((uhm_slab_remote_pending_class_mask_load(heap, size_class) &
                 class_bit) == 0u &&
                    test_slab_remote_queue_empty(slab),
                "slab pending mask clears after recovery");
    expect_ok(api->check(heap, 0), "slab pending mask recovery check");

    api->free(heap, reuse);
    api->free(heap, second);
    expect_ok(api->close(heap), "slab pending mask close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

