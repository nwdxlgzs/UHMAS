static void test_slab_accounted_remote_lockless_partial_reuse(
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
    void *contended_reuse;
    accounted_remote_contended_free_args contended_args;
    accounted_remote_contended_malloc_args contended_malloc_args;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uint64_t raw_publish_before;
    uint64_t raw_publish_after;
    uint64_t epoch_before_free;
    uint64_t epoch_after_free;
    uint64_t epoch_before_reuse;
    uint64_t epoch_after_reuse;
    uint64_t epoch_before_contended_free;
    uint64_t epoch_after_contended_free;
    uint64_t epoch_before_contended_malloc;
    uint64_t epoch_after_contended_malloc;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "accounted remote partial heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "accounted remote partial arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "accounted remote partial usermode vm init");

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
              "accounted remote partial heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "accounted remote partial api");

    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0,
                "accounted remote partial allocations");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    expect_true(slab != 0 && test_slab_slot_from_payload(second)->slab == slab,
                "accounted remote partial same slab");
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL,
                "accounted remote partial starts partial");

    epoch_before_free = heap->core.metadata_epoch;
    api->free(heap, first);
    epoch_after_free = heap->core.metadata_epoch;
    expect_true(epoch_after_free == epoch_before_free,
                "accounted remote partial free skips idle metadata epoch");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "accounted remote partial queued counters");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "accounted remote partial free releases reader");
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "accounted remote partial queued slot");
    expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                     UHM_ATOMIC_ACQUIRE) == 1u,
                "accounted remote partial ready slab queued");

    raw_publish_before = atomic_load_explicit(&slab->raw_publish_seq,
                                              UHM_ATOMIC_ACQUIRE);
    epoch_before_reuse = heap->core.metadata_epoch;
    reuse = api->malloc(heap, 64u);
    epoch_after_reuse = heap->core.metadata_epoch;
    raw_publish_after = atomic_load_explicit(&slab->raw_publish_seq,
                                             UHM_ATOMIC_ACQUIRE);
    expect_true(reuse == first,
                "accounted remote partial lockless reuse pointer");
    expect_true(epoch_after_reuse == epoch_before_reuse,
                "accounted remote partial malloc skips idle metadata epoch");
    expect_true(raw_publish_after == raw_publish_before,
                "accounted remote partial avoids raw publish drain");
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->free_slots != 0 && slab->live_slots != 0,
                "accounted remote partial remains partial after reuse");
    expect_true(test_slab_remote_queue_empty(slab),
                "accounted remote partial remote queue empty");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "accounted remote partial counters cleared");
    expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                     UHM_ATOMIC_ACQUIRE) == 0u,
                "accounted remote partial ready slab consumed");
    expect_true(test_slab_slot_state_flags_load(slot) == 0 &&
                    test_slab_slot_state_requested_load(slot) == 64u,
                "accounted remote partial slot live");
    expect_ok(api->check(heap, 0), "accounted remote partial check");

    memset(&contended_args, 0, sizeof(contended_args));
    contended_args.api = api;
    contended_args.heap = heap;
    contended_args.ptr = reuse;
    atomic_init(&contended_args.done, 0u);
    epoch_before_contended_free = heap->core.metadata_epoch;
#if defined(_WIN32)
    {
        HANDLE thread;
        int completed;

        test_spin_lock(&heap->lock);
        thread = CreateThread(0, 0, accounted_remote_contended_free_worker,
                              &contended_args, 0, 0);
        expect_true(thread != 0,
                    "accounted remote partial contended free create thread");
        completed = test_wait_atomic_u32(&contended_args.done, 1u);
        epoch_after_contended_free = heap->core.metadata_epoch;
        test_spin_unlock(&heap->lock);
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "accounted remote partial contended free join");
        CloseHandle(thread);
        expect_true(completed,
                    "accounted remote partial free completes while heap lock held");
    }
#else
    {
        pthread_t thread;
        int completed;

        test_spin_lock(&heap->lock);
        expect_true(pthread_create(&thread, 0,
                                   accounted_remote_contended_free_worker,
                                   &contended_args) == 0,
                    "accounted remote partial contended free create thread");
        completed = test_wait_atomic_u32(&contended_args.done, 1u);
        epoch_after_contended_free = heap->core.metadata_epoch;
        test_spin_unlock(&heap->lock);
        expect_true(pthread_join(thread, 0) == 0,
                    "accounted remote partial contended free join");
        expect_true(completed,
                    "accounted remote partial free completes while heap lock held");
    }
#endif
    expect_true(contended_args.ok,
                "accounted remote partial contended free worker ok");
    expect_true(epoch_after_contended_free == epoch_before_contended_free,
                "accounted remote partial contended free skips heap-lock epoch");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "accounted remote partial contended free queued counters");
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "accounted remote partial contended free queued slot");
    expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                     UHM_ATOMIC_ACQUIRE) == 1u,
                "accounted remote partial contended free ready queued");
    expect_ok(api->check(heap, 0),
              "accounted remote partial contended free check");

    contended_reuse = api->malloc(heap, 64u);
    expect_true(contended_reuse == reuse,
                "accounted remote partial contended reuse pointer");
    expect_true(heap->core.metadata_epoch == epoch_after_contended_free,
                "accounted remote partial contended reuse skips idle epoch");

    api->free(heap, contended_reuse);
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "accounted remote partial prepares contended malloc queue");

    memset(&contended_malloc_args, 0, sizeof(contended_malloc_args));
    contended_malloc_args.api = api;
    contended_malloc_args.heap = heap;
    atomic_init(&contended_malloc_args.done, 0u);
    epoch_before_contended_malloc = heap->core.metadata_epoch;
#if defined(_WIN32)
    {
        HANDLE thread;
        int completed;

        test_spin_lock(&heap->lock);
        thread = CreateThread(0, 0, accounted_remote_contended_malloc_worker,
                              &contended_malloc_args, 0, 0);
        expect_true(thread != 0,
                    "accounted remote partial contended malloc create thread");
        completed = test_wait_atomic_u32(&contended_malloc_args.done, 1u);
        epoch_after_contended_malloc = heap->core.metadata_epoch;
        test_spin_unlock(&heap->lock);
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "accounted remote partial contended malloc join");
        CloseHandle(thread);
        expect_true(completed,
                    "accounted remote partial malloc completes while heap lock held");
    }
#else
    {
        pthread_t thread;
        int completed;

        test_spin_lock(&heap->lock);
        expect_true(pthread_create(&thread, 0,
                                   accounted_remote_contended_malloc_worker,
                                   &contended_malloc_args) == 0,
                    "accounted remote partial contended malloc create thread");
        completed = test_wait_atomic_u32(&contended_malloc_args.done, 1u);
        epoch_after_contended_malloc = heap->core.metadata_epoch;
        test_spin_unlock(&heap->lock);
        expect_true(pthread_join(thread, 0) == 0,
                    "accounted remote partial contended malloc join");
        expect_true(completed,
                    "accounted remote partial malloc completes while heap lock held");
    }
#endif
    expect_true(contended_malloc_args.ok,
                "accounted remote partial contended malloc worker ok");
    expect_true(contended_malloc_args.result == contended_reuse,
                "accounted remote partial contended malloc pointer");
    expect_true(epoch_after_contended_malloc == epoch_before_contended_malloc,
                "accounted remote partial contended malloc skips heap-lock epoch");
    expect_true(test_slab_remote_queue_empty(slab),
                "accounted remote partial contended malloc queue empty");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "accounted remote partial contended malloc counters cleared");
    expect_ok(api->check(heap, 0),
              "accounted remote partial contended malloc check");

    api->free(heap, contended_malloc_args.result);
    expect_true(heap->core.metadata_epoch == epoch_after_contended_malloc,
                "accounted remote partial post-contended free skips idle epoch");
    api->free(heap, second);
    expect_ok(api->close(heap), "accounted remote partial close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_remote_ready_queue_diagnostics(
    const uhm_storage_info *heap_info) {
    enum {
        TEST_REMOTE_READY_IDLE = 0u,
        TEST_REMOTE_READY_QUEUED = 1u,
        TEST_REMOTE_READY_SCANNING = 2u
    };
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *second;
    void *repair_reuse;
    void *guard;
    void *repair_tail;
    void *reuse;
    void *requeue_tail;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_tagged_freelist *ready_queue;
    uintptr_t saved_ready_head;
    uintptr_t saved_remote_head;
    uint64_t ready_seq_before;
    uint64_t ready_seq_after;

    if (!uhm_tagged_freelist_is_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "ready queue diag heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "ready queue diag arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "ready queue diag usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "ready queue diag heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "ready queue diag api");

    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    guard = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0 && guard != 0,
                "ready queue diag allocations");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    expect_true(slab != 0 &&
                    test_slab_slot_from_payload(second)->slab == slab &&
                    test_slab_slot_from_payload(guard)->slab == slab,
                "ready queue diag same slab");

    api->free(heap, first);
    ready_queue = &heap->core.slab_remote_ready_lf[slab->size_class].queue;
    saved_ready_head = atomic_load_explicit(&ready_queue->head,
                                            UHM_ATOMIC_ACQUIRE);
    saved_remote_head = atomic_load_explicit(&slab->remote_free_lf.head,
                                             UHM_ATOMIC_ACQUIRE);
    expect_true(test_slab_remote_ready_queue_contains(heap, slab),
                "ready queue diag queued slab present");
    expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                     UHM_ATOMIC_ACQUIRE) ==
                    TEST_REMOTE_READY_QUEUED,
                "ready queue diag state queued");
    expect_ok(api->check(heap, 0), "ready queue diag initial check");

    atomic_store_explicit(
        &slab->remote_free_lf.head,
        uhm_tagged_freelist_pack(
            0, uhm_tagged_freelist_unpack_tag(saved_remote_head)),
        UHM_ATOMIC_RELEASE);
    heap->core.fault_remote_ready_clear_countdown = 1u;
    expect_true(api->malloc(heap, 64u) == 0,
                "ready queue diag clear failure returns null");
    expect_last_error(api, heap, UHM_ERR_CORRUPTION, 0u,
                      "ready queue diag clear failure last error");
    expect_true(heap->core.fault_remote_ready_clear_countdown == 0,
                "ready queue diag clear failure consumed");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "ready queue diag clear failure releases reader");
    atomic_store_explicit(&slab->remote_free_lf.head, saved_remote_head,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&ready_queue->head, saved_ready_head,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued,
                          TEST_REMOTE_READY_QUEUED, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "ready queue diag clear failure restored");

    uhm_slab_remote_ready_update_begin(heap, slab->size_class);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag stable active seq rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "ready queue diag active seq op accepted");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_slab_remote_ready_update_end(heap, slab->size_class);
    expect_ok(api->check(heap, 0),
              "ready queue diag active seq restored");

    atomic_store_explicit(
        &ready_queue->head,
        uhm_tagged_freelist_pack(
            &slot->remote_link,
            uhm_tagged_freelist_unpack_tag(saved_ready_head)),
        UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag slot remote link rejected");
    expect_true(api->malloc(heap, 64u) == 0,
                "ready queue diag runtime rejects wrong ready node");
    expect_last_error(api, heap, UHM_ERR_CORRUPTION, 0u,
                      "ready queue diag runtime corruption last error");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "ready queue diag runtime releases reader after corruption");
    atomic_store_explicit(&ready_queue->head, saved_ready_head,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "ready queue diag wrong ready node restored");

    atomic_store_explicit(&slab->remote_ready_queued,
                          TEST_REMOTE_READY_IDLE, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag node without queued state rejected");
    expect_true(api->malloc(heap, 64u) == 0,
                "ready queue diag runtime rejects idle ready node");
    expect_last_error(api, heap, UHM_ERR_CORRUPTION, 0u,
                      "ready queue diag idle node runtime last error");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "ready queue diag idle node runtime releases reader");
    atomic_store_explicit(&ready_queue->head, saved_ready_head,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued,
                          TEST_REMOTE_READY_QUEUED, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "ready queue diag queued state restored");

    atomic_store_explicit(
        &ready_queue->head,
        uhm_tagged_freelist_pack(
            0, uhm_tagged_freelist_unpack_tag(saved_ready_head)),
        UHM_ATOMIC_RELEASE);
    expect_true(!test_slab_remote_ready_queue_contains(heap, slab),
                "ready queue diag detached saved head");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag queued state without node rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag queued publish op only rejected");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_slab_remote_ready_update_begin(heap, slab->size_class);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag queued publish seq only rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "ready queue diag queued publish seq window accepted");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    uhm_slab_remote_ready_update_end(heap, slab->size_class);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag queued publish after seq rejected");
    atomic_store_explicit(&ready_queue->head, saved_ready_head,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "ready queue diag head restored");

    atomic_store_explicit(
        &ready_queue->head,
        uhm_tagged_freelist_pack(
            0, uhm_tagged_freelist_unpack_tag(saved_ready_head)),
        UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued,
                          TEST_REMOTE_READY_SCANNING, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag stale scanning rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ready queue diag active scanning without reader rejected");
    expect_true(uhm_metadata_reader_begin(
                    &heap->core.lockless_remote_readers[0], heap),
                "ready queue diag scanning reader begin");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 0u,
                    &slab->remote_ready_link),
                "ready queue diag scanning ready hazard");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 1u, slab),
                "ready queue diag scanning slab hazard");
    expect_ok(api->check(heap, 0),
              "ready queue diag protected scanning window accepted");
    expect_true(uhm_metadata_reader_end(
                    &heap->core.lockless_remote_readers[0]),
                "ready queue diag scanning reader end");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued,
                          TEST_REMOTE_READY_QUEUED, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&ready_queue->head, saved_ready_head,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "ready queue diag final restored check");

    api->free(heap, second);
    heap->core.fault_remote_ready_note_countdown = 1u;
    heap->core.fault_lockless_remote_drain_countdown = 2u;
    ready_seq_before = uhm_slab_remote_ready_seq_load(heap, slab->size_class);
    repair_reuse = api->malloc(heap, 64u);
    ready_seq_after = uhm_slab_remote_ready_seq_load(heap, slab->size_class);
    expect_true(repair_reuse == first || repair_reuse == second,
                "ready queue diag finish repair first reuse");
    expect_true(heap->core.fault_remote_ready_note_countdown == 0,
                "ready queue diag finish repair fault consumed");
    expect_true(heap->core.fault_lockless_remote_drain_countdown == 1u,
                "ready queue diag finish repair used reservation drain");
    heap->core.fault_lockless_remote_drain_countdown = 0u;
    expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                     UHM_ATOMIC_ACQUIRE) ==
                    TEST_REMOTE_READY_IDLE,
                "ready queue diag finish repair leaves ready idle");
    expect_true(!test_slab_remote_ready_queue_contains(heap, slab),
                "ready queue diag finish repair drains ready node");
    expect_true(ready_seq_after != ready_seq_before &&
                    !uhm_slab_remote_ready_seq_active_value(ready_seq_after),
                "ready queue diag finish repair seq completed");
    expect_ok(api->check(heap, 0), "ready queue diag finish repair check");
    repair_tail = api->malloc(heap, 64u);
    expect_true((repair_reuse == first && repair_tail == second) ||
                    (repair_reuse == second && repair_tail == first),
                "ready queue diag finish repair second reuse");
    expect_ok(api->check(heap, 0),
              "ready queue diag finish repair second check");
    api->free(heap, repair_reuse);
    api->free(heap, repair_tail);

    ready_seq_before = uhm_slab_remote_ready_seq_load(heap, slab->size_class);
    reuse = api->malloc(heap, 64u);
    ready_seq_after = uhm_slab_remote_ready_seq_load(heap, slab->size_class);
    expect_true(reuse == first || reuse == second,
                "ready queue diag restored queue first reuse");
    expect_true(test_slab_remote_ready_queue_contains(heap, slab),
                "ready queue diag requeue keeps remaining ready node");
    expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                     UHM_ATOMIC_ACQUIRE) ==
                    TEST_REMOTE_READY_QUEUED,
                "ready queue diag requeue state queued");
    expect_true(ready_seq_after != ready_seq_before &&
                    !uhm_slab_remote_ready_seq_active_value(ready_seq_after),
                "ready queue diag requeue seq completed");
    expect_ok(api->check(heap, 0), "ready queue diag requeue check");
    ready_seq_before = ready_seq_after;
    requeue_tail = api->malloc(heap, 64u);
    ready_seq_after = uhm_slab_remote_ready_seq_load(heap, slab->size_class);
    expect_true((reuse == first && requeue_tail == second) ||
                    (reuse == second && requeue_tail == first),
                "ready queue diag restored queue second reuse");
    expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                     UHM_ATOMIC_ACQUIRE) ==
                    TEST_REMOTE_READY_IDLE,
                "ready queue diag requeue consumed");
    expect_true(ready_seq_after != ready_seq_before &&
                    !uhm_slab_remote_ready_seq_active_value(ready_seq_after),
                "ready queue diag clear seq completed");
    expect_ok(api->check(heap, 0), "ready queue diag requeue consumed check");
    api->free(heap, reuse);
    api->free(heap, requeue_tail);
    api->free(heap, guard);
    expect_ok(api->close(heap), "ready queue diag close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

