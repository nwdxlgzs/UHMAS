static void test_slab_accounted_remote_lockless_full_to_partial(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs = 0;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    size_t slot_count;
    size_t i;
    uint64_t raw_publish_before;
    uint64_t raw_publish_after;
    uint64_t epoch_before_free;
    void *reuse;
    uintptr_t saved_ready_head;
    uintptr_t saved_remote_head;
    uint32_t saved_ready_state;
    uint64_t saved_slot_state;
    uint32_t saved_slot_flags;
    uint32_t saved_slot_requested;
    uhm_slab_slot *saved_slot_next_free;
    uhm_tagged_freelist_node *saved_slot_remote_next;
    size_t saved_slab_live;
    size_t saved_slab_free;
    size_t saved_slab_live_mirror;
    size_t saved_slab_free_mirror;
    size_t saved_remote_pending;
    size_t saved_remote_pending_mirror;
    size_t saved_remote_inflight;
    size_t saved_remote_inflight_mirror;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "accounted remote full heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "accounted remote full arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "accounted remote full usermode vm init");

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
              "accounted remote full heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "accounted remote full api");

    ptrs = (void **)calloc(256u, sizeof(*ptrs));
    expect_true(ptrs != 0, "accounted remote full ptr array");
    ptrs[0] = api->malloc(heap, 64u);
    expect_true(ptrs[0] != 0, "accounted remote full first alloc");
    slot = test_slab_slot_from_payload(ptrs[0]);
    slab = slot->slab;
    slot_count = slab->total_slots;
    expect_true(slot_count <= 256u && slot_count > 1u,
                "accounted remote full slot count");
    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0 &&
                        test_slab_slot_from_payload(ptrs[i])->slab == slab,
                    "accounted remote full fill slab");
    }
    expect_true(slab->state == UHM_SLAB_STATE_FULL &&
                    slab->free_slots == 0,
                "accounted remote full starts full");

    epoch_before_free = heap->core.metadata_epoch;
    api->free(heap, ptrs[0]);
    expect_true(heap->core.metadata_epoch > epoch_before_free,
                "accounted remote full free advances epoch");
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->free_slots == 1u,
                "accounted remote full free refreshes partial state");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "accounted remote full queued counters");
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "accounted remote full queued slot");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "accounted remote full free releases reader");
    expect_ok(api->check(heap, 0), "accounted remote full check after free");

    saved_ready_head = atomic_load_explicit(
        &heap->core.slab_remote_ready_lf[slab->size_class].queue.head,
        UHM_ATOMIC_ACQUIRE);
    saved_remote_head = atomic_load_explicit(&slab->remote_free_lf.head,
                                             UHM_ATOMIC_ACQUIRE);
    saved_ready_state = atomic_load_explicit(&slab->remote_ready_queued,
                                             UHM_ATOMIC_ACQUIRE);
    saved_slot_state = atomic_load_explicit(&slot->state_word,
                                            UHM_ATOMIC_ACQUIRE);
    saved_slot_flags = slot->flags;
    saved_slot_requested = slot->requested_size;
    saved_slot_next_free = slot->next_free;
    saved_slot_remote_next = uhm_tagged_freelist_next_load(&slot->remote_link);
    saved_slab_live = slab->live_slots;
    saved_slab_free = slab->free_slots;
    saved_slab_live_mirror =
        atomic_load_explicit(&slab->live_slots_count, UHM_ATOMIC_ACQUIRE);
    saved_slab_free_mirror =
        atomic_load_explicit(&slab->free_slots_count, UHM_ATOMIC_ACQUIRE);
    saved_remote_pending =
        atomic_load_explicit(&slab->remote_free_pending, UHM_ATOMIC_ACQUIRE);
    saved_remote_pending_mirror =
        atomic_load_explicit(&slab->remote_free_slots, UHM_ATOMIC_ACQUIRE);
    saved_remote_inflight =
        atomic_load_explicit(&slab->remote_enqueue_inflight, UHM_ATOMIC_ACQUIRE);
    saved_remote_inflight_mirror = atomic_load_explicit(
        &slab->remote_enqueue_inflight_count, UHM_ATOMIC_ACQUIRE);

    atomic_store_explicit(
        &heap->core.slab_remote_ready_lf[slab->size_class].queue.head,
        uhm_tagged_freelist_pack(
            0, uhm_tagged_freelist_unpack_tag(saved_ready_head)),
        UHM_ATOMIC_RELEASE);
    atomic_store_explicit(
        &slab->remote_free_lf.head,
        uhm_tagged_freelist_pack(
            0, uhm_tagged_freelist_unpack_tag(saved_remote_head)),
        UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued,
                          2u, UHM_ATOMIC_RELEASE);
    uhm_tagged_freelist_next_store(&slot->remote_link, 0);
    slot->next_free = 0;
    slot->flags = 0u;
    slot->requested_size = 64u;
    atomic_store_explicit(&slot->state_word,
                          test_slab_slot_state_pack(0u, 64u),
                          UHM_ATOMIC_RELEASE);
    slab->live_slots = slot_count;
    slab->free_slots = 0u;
    atomic_store_explicit(&slab->live_slots_count, slot_count,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->free_slots_count, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_pending, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_slots, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_enqueue_inflight, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_enqueue_inflight_count, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "accounted remote full transient without reader rejected");
    expect_true(uhm_metadata_reader_begin(
                    &heap->core.lockless_remote_readers[0], heap),
                "accounted remote full transient reader begin");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 0u,
                    &slab->remote_ready_link),
                "accounted remote full transient ready hazard");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 1u, slab),
                "accounted remote full transient slab hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "accounted remote full scanner reader without seq rejected");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_slab_state_transition_update_begin(slab);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "accounted remote full scanner seq without op rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "accounted remote full protected partial-list transient");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_slab_state_transition_update_end(slab);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "accounted remote full scanner after seq rejected");
    expect_true(uhm_metadata_reader_end(
                    &heap->core.lockless_remote_readers[0]),
                "accounted remote full transient reader end");

    slot->flags = saved_slot_flags;
    slot->requested_size = saved_slot_requested;
    atomic_store_explicit(&slot->state_word, saved_slot_state,
                          UHM_ATOMIC_RELEASE);
    slot->next_free = saved_slot_next_free;
    uhm_tagged_freelist_next_store(&slot->remote_link, saved_slot_remote_next);
    slab->live_slots = saved_slab_live;
    slab->free_slots = saved_slab_free;
    atomic_store_explicit(&slab->live_slots_count, saved_slab_live_mirror,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->free_slots_count, saved_slab_free_mirror,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_pending, saved_remote_pending,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_slots, saved_remote_pending_mirror,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_enqueue_inflight,
                          saved_remote_inflight, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_enqueue_inflight_count,
                          saved_remote_inflight_mirror, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_lf.head, saved_remote_head,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(
        &heap->core.slab_remote_ready_lf[slab->size_class].queue.head,
        saved_ready_head, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued, saved_ready_state,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "accounted remote full transient restored check");

    raw_publish_before = atomic_load_explicit(&slab->raw_publish_seq,
                                              UHM_ATOMIC_ACQUIRE);
    reuse = api->malloc(heap, 64u);
    raw_publish_after = atomic_load_explicit(&slab->raw_publish_seq,
                                             UHM_ATOMIC_ACQUIRE);
    expect_true(reuse == ptrs[0],
                "accounted remote full later reuse pointer");
    ptrs[0] = reuse;
    expect_true(raw_publish_after == raw_publish_before,
                "accounted remote full later avoids raw publish drain");
    expect_true(slab->state == UHM_SLAB_STATE_FULL &&
                    slab->free_slots == 0,
                "accounted remote full later refreshes full state");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "accounted remote full counters cleared");
    expect_ok(api->check(heap, 0), "accounted remote full final check");

    for (i = 0; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
    }
    expect_ok(api->close(heap), "accounted remote full close");

    free(ptrs);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_accounted_remote_lockless_partial_to_empty(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uint64_t epoch_before_free;
    uint64_t epoch_before_reuse;
    uint64_t epoch_after_reuse;
    uint64_t raw_publish_before;
    uint64_t raw_publish_after;
    uintptr_t saved_ready_head;
    uintptr_t saved_remote_head;
    uint32_t saved_ready_state;
    uint64_t saved_slot_state;
    uint32_t saved_slot_flags;
    uint32_t saved_slot_requested;
    uhm_slab_slot *saved_slot_next_free;
    uhm_tagged_freelist_node *saved_slot_remote_next;
    size_t saved_slab_live;
    size_t saved_slab_free;
    size_t saved_slab_live_mirror;
    size_t saved_slab_free_mirror;
    size_t saved_remote_pending;
    size_t saved_remote_pending_mirror;
    size_t saved_remote_inflight;
    size_t saved_remote_inflight_mirror;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "accounted remote empty heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "accounted remote empty arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "accounted remote empty usermode vm init");

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
              "accounted remote empty heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "accounted remote empty api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "accounted remote empty allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot->slab;
    expect_true(slab != 0 && slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->live_slots == 1u,
                "accounted remote empty starts partial single live");

    epoch_before_free = heap->core.metadata_epoch;
    api->free(heap, ptr);
    expect_true(heap->core.metadata_epoch > epoch_before_free,
                "accounted remote empty free advances epoch");
    expect_true(slab->state == UHM_SLAB_STATE_EMPTY &&
                    slab->live_slots == 0 &&
                    slab->free_slots == slab->total_slots,
                "accounted remote empty free refreshes empty state");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "accounted remote empty queued counters");
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "accounted remote empty queued slot");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "accounted remote empty free releases reader");
    expect_ok(api->check(heap, 0), "accounted remote empty check after free");

    saved_ready_head = atomic_load_explicit(
        &heap->core.slab_remote_ready_lf[slab->size_class].queue.head,
        UHM_ATOMIC_ACQUIRE);
    saved_remote_head = atomic_load_explicit(&slab->remote_free_lf.head,
                                             UHM_ATOMIC_ACQUIRE);
    saved_ready_state = atomic_load_explicit(&slab->remote_ready_queued,
                                             UHM_ATOMIC_ACQUIRE);
    saved_slot_state = atomic_load_explicit(&slot->state_word,
                                            UHM_ATOMIC_ACQUIRE);
    saved_slot_flags = slot->flags;
    saved_slot_requested = slot->requested_size;
    saved_slot_next_free = slot->next_free;
    saved_slot_remote_next = uhm_tagged_freelist_next_load(&slot->remote_link);
    saved_slab_live = slab->live_slots;
    saved_slab_free = slab->free_slots;
    saved_slab_live_mirror =
        atomic_load_explicit(&slab->live_slots_count, UHM_ATOMIC_ACQUIRE);
    saved_slab_free_mirror =
        atomic_load_explicit(&slab->free_slots_count, UHM_ATOMIC_ACQUIRE);
    saved_remote_pending =
        atomic_load_explicit(&slab->remote_free_pending, UHM_ATOMIC_ACQUIRE);
    saved_remote_pending_mirror =
        atomic_load_explicit(&slab->remote_free_slots, UHM_ATOMIC_ACQUIRE);
    saved_remote_inflight =
        atomic_load_explicit(&slab->remote_enqueue_inflight, UHM_ATOMIC_ACQUIRE);
    saved_remote_inflight_mirror = atomic_load_explicit(
        &slab->remote_enqueue_inflight_count, UHM_ATOMIC_ACQUIRE);

    atomic_store_explicit(
        &heap->core.slab_remote_ready_lf[slab->size_class].queue.head,
        uhm_tagged_freelist_pack(
            0, uhm_tagged_freelist_unpack_tag(saved_ready_head)),
        UHM_ATOMIC_RELEASE);
    atomic_store_explicit(
        &slab->remote_free_lf.head,
        uhm_tagged_freelist_pack(
            0, uhm_tagged_freelist_unpack_tag(saved_remote_head)),
        UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued,
                          2u, UHM_ATOMIC_RELEASE);
    uhm_tagged_freelist_next_store(&slot->remote_link, 0);
    slot->next_free = 0;
    slot->flags = 0u;
    slot->requested_size = 64u;
    atomic_store_explicit(&slot->state_word,
                          test_slab_slot_state_pack(0u, 64u),
                          UHM_ATOMIC_RELEASE);
    slab->live_slots = 1u;
    slab->free_slots = slab->total_slots - 1u;
    atomic_store_explicit(&slab->live_slots_count, 1u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->free_slots_count, slab->total_slots - 1u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_pending, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_slots, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_enqueue_inflight, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_enqueue_inflight_count, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "accounted remote empty transient without reader rejected");
    expect_true(uhm_metadata_reader_begin(
                    &heap->core.lockless_remote_readers[0], heap),
                "accounted remote empty transient reader begin");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 0u,
                    &slab->remote_ready_link),
                "accounted remote empty transient ready hazard");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 1u, slab),
                "accounted remote empty transient slab hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "accounted remote empty scanner reader without seq rejected");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_slab_state_transition_update_begin(slab);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "accounted remote empty scanner seq without op rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "accounted remote empty protected empty-list transient");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_slab_state_transition_update_end(slab);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "accounted remote empty scanner after seq rejected");
    expect_true(uhm_metadata_reader_end(
                    &heap->core.lockless_remote_readers[0]),
                "accounted remote empty transient reader end");

    slot->flags = saved_slot_flags;
    slot->requested_size = saved_slot_requested;
    atomic_store_explicit(&slot->state_word, saved_slot_state,
                          UHM_ATOMIC_RELEASE);
    slot->next_free = saved_slot_next_free;
    uhm_tagged_freelist_next_store(&slot->remote_link, saved_slot_remote_next);
    slab->live_slots = saved_slab_live;
    slab->free_slots = saved_slab_free;
    atomic_store_explicit(&slab->live_slots_count, saved_slab_live_mirror,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->free_slots_count, saved_slab_free_mirror,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_pending, saved_remote_pending,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_slots, saved_remote_pending_mirror,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_enqueue_inflight,
                          saved_remote_inflight, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_enqueue_inflight_count,
                          saved_remote_inflight_mirror, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_lf.head, saved_remote_head,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(
        &heap->core.slab_remote_ready_lf[slab->size_class].queue.head,
        saved_ready_head, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued, saved_ready_state,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "accounted remote empty transient restored check");

    raw_publish_before = atomic_load_explicit(&slab->raw_publish_seq,
                                              UHM_ATOMIC_ACQUIRE);
    epoch_before_reuse = heap->core.metadata_epoch;
    reuse = api->malloc(heap, 64u);
    epoch_after_reuse = heap->core.metadata_epoch;
    raw_publish_after = atomic_load_explicit(&slab->raw_publish_seq,
                                             UHM_ATOMIC_ACQUIRE);
    expect_true(reuse == ptr,
                "accounted remote empty lockless reuse pointer");
    expect_true(epoch_after_reuse > epoch_before_reuse,
                "accounted remote empty malloc advances epoch");
    expect_true(raw_publish_after == raw_publish_before,
                "accounted remote empty avoids raw publish drain");
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->live_slots == 1u &&
                    slab->free_slots == slab->total_slots - 1u,
                "accounted remote empty refreshes to partial");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "accounted remote empty reuse clears counters");
    expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                     UHM_ATOMIC_ACQUIRE) == 0u,
                "accounted remote empty ready slab consumed");
    expect_ok(api->check(heap, 0), "accounted remote empty check after reuse");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "accounted remote empty close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

