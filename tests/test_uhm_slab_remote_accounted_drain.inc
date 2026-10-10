static void test_slab_remote_queue_drain(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_stats stats;
    size_t saved_remote_slots;
    size_t saved_remote_pending;
    int tagged_supported = uhm_tagged_freelist_is_supported();

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab remote heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "slab remote arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab remote usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "slab remote heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab remote api");

    first = api->malloc(heap, 64u);
    expect_true(first != 0, "slab remote first allocation");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "slab remote slab metadata");
    expect_true(test_slab_remote_queue_empty(slab),
                "slab remote starts empty");

    api->free(heap, first);
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "slab remote stats after free");
    expect_true(stats.malloc_live_bytes == 0, "slab remote free updates stats");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_queue_contains(slab, slot),
                "slab remote queued slot");
    if (tagged_supported) {
        expect_true((slot->flags & (UHM_SLAB_SLOT_FLAG_FREE |
                                    UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                                    UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING)) ==
                        (UHM_SLAB_SLOT_FLAG_FREE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING),
                    "slab remote lock-free publish flags");
        expect_true(test_slab_remote_inflight_mirror(slab) == 1u &&
                        atomic_load_explicit(&slab->remote_enqueue_inflight,
                                             UHM_ATOMIC_ACQUIRE) == 1u,
                    "slab remote lock-free publish defers inflight");
    } else {
        expect_true((slot->flags & (UHM_SLAB_SLOT_FLAG_FREE |
                                    UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) ==
                        (UHM_SLAB_SLOT_FLAG_FREE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) &&
                        (slot->flags &
                         UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) == 0,
                    "slab remote unsupported publish uses locked flags");
        expect_true(test_slab_remote_inflight_mirror(slab) == 0u &&
                        atomic_load_explicit(&slab->remote_enqueue_inflight,
                                             UHM_ATOMIC_ACQUIRE) == 0u,
                    "slab remote unsupported publish has no inflight");
    }
    saved_remote_slots = test_slab_remote_pending_mirror(slab);
    saved_remote_pending = test_slab_remote_pending(slab);
    atomic_store_explicit(&slab->remote_free_pending,
                          saved_remote_pending + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote atomic pending corruption detected");
    atomic_store_explicit(&slab->remote_free_pending,
                          saved_remote_pending,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_free_slots,
                          saved_remote_slots + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote raw pending mirror corruption detected");
    atomic_store_explicit(&slab->remote_free_slots,
                          saved_remote_slots,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "slab remote check with queued slot");

    atomic_store_explicit(&slab->remote_counter_seq, 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote counter update without op pin rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "slab remote counter update with op pin accepted");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_counter_seq, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "slab remote counter update window restored");

    if (tagged_supported) {
        size_t saved_inflight_mirror =
            test_slab_remote_inflight_mirror(slab);
        size_t saved_inflight =
            atomic_load_explicit(&slab->remote_enqueue_inflight,
                                 UHM_ATOMIC_ACQUIRE);

        atomic_store_explicit(&slab->remote_enqueue_inflight_count,
                              saved_inflight_mirror + 1u,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight,
                              saved_inflight + 1u,
                              UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "slab remote stable extra inflight rejected");
        atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "slab remote extra inflight op pin without reader rejected");
        expect_true(uhm_metadata_reader_try_begin_inactive(
                        &heap->core.lockless_remote_readers[0], heap),
                    "slab remote extra inflight reader begin");
        expect_true(uhm_metadata_reader_hazard_set(
                        &heap->core.lockless_remote_readers[0], 1u, slab),
                    "slab remote extra inflight slab hazard");
        expect_ok(api->check(heap, 0),
                  "slab remote extra inflight with reader accepted");
        expect_true(uhm_metadata_reader_end(
                        &heap->core.lockless_remote_readers[0]),
                    "slab remote extra inflight reader end");
        atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight_count,
                              saved_inflight_mirror,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight,
                              saved_inflight,
                              UHM_ATOMIC_RELEASE);
        expect_ok(api->check(heap, 0),
                  "slab remote extra inflight restored");

        atomic_store_explicit(&slab->remote_free_slots,
                              saved_remote_slots + 1u,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_free_pending,
                              saved_remote_pending + 1u,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight_count,
                              saved_inflight_mirror + 1u,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight,
                              saved_inflight + 1u,
                              UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "slab remote pending gap stable rejected");
        atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "slab remote pending gap op pin without reader rejected");
        expect_true(uhm_metadata_reader_try_begin_inactive(
                        &heap->core.lockless_remote_readers[0], heap),
                    "slab remote pending gap reader begin");
        expect_true(uhm_metadata_reader_hazard_set(
                        &heap->core.lockless_remote_readers[0], 1u, slab),
                    "slab remote pending gap slab hazard");
        expect_ok(api->check(heap, 0),
                  "slab remote pending gap with reader accepted");
        expect_true(uhm_metadata_reader_end(
                        &heap->core.lockless_remote_readers[0]),
                    "slab remote pending gap reader end");
        atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_free_slots,
                              saved_remote_slots,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_free_pending,
                              saved_remote_pending,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight_count,
                              saved_inflight_mirror,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight,
                              saved_inflight,
                              UHM_ATOMIC_RELEASE);
        expect_ok(api->check(heap, 0),
                  "slab remote pending gap restored");
    }

    atomic_store_explicit(&slab->remote_drain_active, 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(uhm_metadata_registered_reader_count_increment(heap),
                "slab remote drain owner registered");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote drain owner without op pin rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote drain owner without reader rejected");
    expect_true(uhm_metadata_reader_try_begin_inactive(
                    &heap->core.lockless_remote_readers[0], heap),
                "slab remote drain owner reader begin");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 1u, slab),
                "slab remote drain owner slab hazard");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote drain owner missing epoch rejected");
    atomic_store_explicit(&slab->remote_drain_epoch,
                          heap->core.metadata_epoch + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote drain owner future epoch rejected");
    atomic_store_explicit(&slab->remote_drain_epoch,
                          heap->core.metadata_epoch,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "slab remote drain owner with reader op pin accepted");
    expect_true(uhm_metadata_reader_end(
                    &heap->core.lockless_remote_readers[0]),
                "slab remote drain owner reader end");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    expect_true(uhm_metadata_registered_reader_count_decrement(heap),
                "slab remote drain owner unregistered");
    atomic_store_explicit(&slab->remote_drain_active, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_drain_epoch, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "slab remote drain owner restored");
    atomic_store_explicit(&slab->remote_drain_epoch,
                          heap->core.metadata_epoch,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab remote drain stale epoch rejected");
    atomic_store_explicit(&slab->remote_drain_epoch, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "slab remote drain stale epoch restored");

    if (tagged_supported) {
        uhm_tagged_freelist_node *draining_link =
            uhm_tagged_freelist_take_all(&slab->remote_free_lf);
        uint64_t saved_state =
            atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
        uint32_t draining_flags =
            slot->flags | UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING;

        expect_true(draining_link == &slot->remote_link &&
                        uhm_tagged_freelist_next_load(draining_link) == 0,
                    "slab remote draining flag detaches queued node");
        atomic_store_explicit(&slot->state_word,
                              test_slab_slot_state_pack(draining_flags, 0u),
                              UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "slab remote draining flag without owner rejected");
        atomic_store_explicit(&slab->remote_drain_active, 1u,
                              UHM_ATOMIC_RELEASE);
        expect_true(uhm_metadata_registered_reader_count_increment(heap),
                    "slab remote draining flag owner registered");
        atomic_store_explicit(&slab->remote_drain_epoch,
                              heap->core.metadata_epoch,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "slab remote draining flag owner without reader rejected");
        expect_true(uhm_metadata_reader_try_begin_inactive(
                        &heap->core.lockless_remote_readers[0], heap),
                    "slab remote draining flag reader begin");
        expect_true(uhm_metadata_reader_hazard_set(
                        &heap->core.lockless_remote_readers[0], 1u, slab),
                    "slab remote draining flag slab hazard");
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "slab remote draining flag slab hazard without link rejected");
        expect_true(uhm_metadata_reader_hazard_set(
                        &heap->core.lockless_remote_readers[0], 0u,
                        draining_link),
                    "slab remote draining flag link hazard");
        expect_ok(api->check(heap, 0),
                  "slab remote draining flag with reader owner op accepted");
        expect_true(uhm_metadata_reader_end(
                        &heap->core.lockless_remote_readers[0]),
                    "slab remote draining flag reader end");
        expect_true(uhm_metadata_registered_reader_count_decrement(heap),
                    "slab remote draining flag owner unregistered");
        atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_drain_active, 0u,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_drain_epoch, 0u,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slot->state_word, saved_state,
                              UHM_ATOMIC_RELEASE);
        uhm_tagged_freelist_next_store(draining_link, 0);
        expect_true(uhm_tagged_freelist_push(&slab->remote_free_lf,
                                             draining_link),
                    "slab remote draining flag restores queue");
        expect_ok(api->check(heap, 0),
                  "slab remote draining flag restored");
    }

    {
        uint32_t publishing_flags =
            slot->flags | UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING;
        slot->flags = publishing_flags;
        slot->requested_size = 0;
        atomic_store_explicit(
            &slot->state_word,
            test_slab_slot_state_pack(publishing_flags, 0u),
            UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight_count, 1u,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_enqueue_inflight, 1u,
                              UHM_ATOMIC_RELEASE);
    }
    expect_ok(api->check(heap, 0),
              "slab remote publishing window check");
    reuse = api->malloc(heap, 64u);
    expect_true(reuse == first, "slab remote drain reuses queued slot");
    expect_true(test_slab_remote_queue_empty(slab),
                "slab remote queue drained");
    expect_true(test_slab_remote_inflight_mirror(slab) == 0u &&
                    atomic_load_explicit(&slab->remote_enqueue_inflight,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "slab remote drain consumes publishing inflight");
    expect_true((slot->flags & (UHM_SLAB_SLOT_FLAG_FREE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) == 0,
                "slab remote reused slot live");
    api->free(heap, reuse);
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_queue_contains(slab, slot),
                "slab remote final free queued before close");
    if (tagged_supported) {
        expect_true(test_slab_remote_inflight_mirror(slab) == 1u,
                    "slab remote final free leaves publish for close drain");
    } else {
        expect_true(test_slab_remote_inflight_mirror(slab) == 0u,
                    "slab remote final free unsupported has no inflight");
    }
    expect_ok(api->check(heap, 0), "slab remote final check");
    expect_ok(api->close(heap), "slab remote close");
    expect_true(test_slab_remote_queue_empty(slab),
                "slab remote close drains queued slot");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_tagged_close_corruption_restores(
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
    uint32_t saved_magic;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab tagged corrupt heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab tagged corrupt arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab tagged corrupt usermode vm init");

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
              "slab tagged corrupt heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab tagged corrupt api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "slab tagged corrupt allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot ? slot->slab : 0;
    expect_true(slot != 0 && slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "slab tagged corrupt metadata");

    api->free(heap, ptr);
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "slab tagged corrupt queued");
    saved_magic = slot->magic;
    slot->magic = 0u;
    expect_true(api->close(heap) == UHM_ERR_CORRUPTION,
                "slab tagged corrupt close returns corruption");
    slot->magic = saved_magic;
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "slab tagged corrupt restores queue");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "slab tagged corrupt keeps counters");
    expect_ok(api->check(heap, 0), "slab tagged corrupt heap still live");
    expect_ok(api->close(heap), "slab tagged corrupt retry close");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "slab tagged corrupt retry drains counters");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

