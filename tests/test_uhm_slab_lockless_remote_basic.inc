static void test_slab_lockless_remote_unsupported_fallback(
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
    uhm_stats stats;
    uint64_t state_word;
    uint32_t state_flags;
    uint32_t state_requested;
    int tagged_supported = uhm_tagged_freelist_is_supported();

    if (test_lockless_remote_primitives_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless unsupported heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless unsupported arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless unsupported usermode vm init");

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
              "lockless unsupported heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless unsupported api");
    expect_true((heap->state_flags & UHM_CONFIG_LOCKLESS_REMOTE_FREE) == 0,
                "lockless unsupported flag downgraded");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless unsupported allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless unsupported slab metadata");

    api->free(heap, ptr);
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    state_requested = (uint32_t)(state_word >> 32);
    if (tagged_supported) {
        expect_true((state_flags &
                     (UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING)) ==
                        (UHM_SLAB_SLOT_FLAG_FREE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) &&
                        (state_flags &
                         UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) == 0 &&
                        state_requested == 0,
                    "lockless unsupported keeps heap-level tagged publishing");
    } else {
        expect_true((state_flags &
                     (UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING)) ==
                        (UHM_SLAB_SLOT_FLAG_FREE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) &&
                        (state_flags &
                         UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) == 0 &&
                        state_requested == 0,
                    "lockless unsupported uses locked remote state");
    }
    expect_true(slot->flags == state_flags && slot->requested_size == 0,
                "lockless unsupported mirror synced");
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "lockless unsupported uses slab queue");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless unsupported skips class queue");
    if (tagged_supported) {
        expect_true(test_slab_remote_inflight_mirror(slab) == 1u &&
                        atomic_load_explicit(&slab->remote_enqueue_inflight,
                                             UHM_ATOMIC_ACQUIRE) == 1u,
                    "lockless unsupported heap-level tagged inflight");
    } else {
        expect_true(test_slab_remote_inflight_mirror(slab) == 0u &&
                        atomic_load_explicit(&slab->remote_enqueue_inflight,
                                             UHM_ATOMIC_ACQUIRE) == 0u,
                    "lockless unsupported has no publishing inflight");
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "lockless unsupported stats after free");
    expect_true(stats.malloc_live_bytes == 0,
                "lockless unsupported free stats");
    expect_ok(api->check(heap, 0), "lockless unsupported check after free");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == ptr, "lockless unsupported locked reuse");
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    state_requested = (uint32_t)(state_word >> 32);
    expect_true(state_flags == 0 && state_requested == 64u &&
                    slot->flags == 0 && slot->requested_size == 64u,
                "lockless unsupported live mirror synced");
    expect_ok(api->check(heap, 0), "lockless unsupported final check");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "lockless unsupported close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_remote_free_unaccounted(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *contended_reuse;
    void *reuse;
    void *mismatch_reuse;
    accounted_remote_contended_free_args contended_free_args;
    accounted_remote_contended_malloc_args contended_malloc_args;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_stats stats;
    uint64_t state_word;
    uint32_t state_flags;
    uint32_t state_requested;
    size_t live_before;
    size_t free_before;
    uint64_t epoch_before_free;
    uint64_t epoch_after_free;
    uint64_t epoch_before_contended_free;
    uint64_t epoch_after_contended_free;
    uint64_t epoch_before_contended_reuse;
    uint64_t epoch_after_contended_reuse;
    uint64_t epoch_before_reuse;
    uint64_t epoch_after_reuse;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless remote heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless remote arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless remote usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "lockless remote heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless remote api");

    first = api->malloc(heap, 64u);
    expect_true(first != 0, "lockless remote first allocation");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless remote slab metadata");
    live_before = slab->live_slots;
    free_before = slab->free_slots;

    epoch_before_free = heap->core.metadata_epoch;
    api->free(heap, first);
    epoch_after_free = heap->core.metadata_epoch;
    expect_true(epoch_after_free == epoch_before_free,
                "lockless remote free skips idle metadata epoch");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "lockless remote stats after free");
    expect_true(stats.malloc_live_bytes == 0,
                "lockless remote free updates atomic stats");
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    expect_true((state_flags &
                 (UHM_SLAB_SLOT_FLAG_FREE |
                  UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                  UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                  UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) ==
                    (UHM_SLAB_SLOT_FLAG_FREE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                     UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED),
                "lockless remote atomic state unaccounted");
    expect_true(slot->flags == 0 && slot->requested_size == 64u,
                "lockless remote raw mirror deferred");
    expect_true(slab->live_slots == live_before &&
                    slab->free_slots == free_before,
                "lockless remote slab counts deferred");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote queued in class queue");
    expect_true(test_slab_remote_inflight_mirror(slab) == 1u,
                "lockless remote inflight queued");
    expect_ok(api->check(heap, 0), "lockless remote check deferred");

    {
        uhm_tagged_freelist_node *saved_next =
            uhm_tagged_freelist_next_load(&slot->remote_link);
        uhm_tagged_freelist_next_store(&slot->remote_link,
                                       &slot->remote_link);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "lockless remote class queue cycle detected");
        uhm_tagged_freelist_next_store(&slot->remote_link, saved_next);
        expect_ok(api->check(heap, 0),
                  "lockless remote class queue cycle restored");
    }

    memset(&contended_malloc_args, 0, sizeof(contended_malloc_args));
    contended_malloc_args.api = api;
    contended_malloc_args.heap = heap;
    atomic_init(&contended_malloc_args.done, 0u);
    epoch_before_contended_reuse = heap->core.metadata_epoch;
#if defined(_WIN32)
    {
        HANDLE thread;
        int completed;

        test_spin_lock(&heap->lock);
        thread = CreateThread(0, 0, accounted_remote_contended_malloc_worker,
                              &contended_malloc_args, 0, 0);
        expect_true(thread != 0,
                    "lockless remote contended malloc create thread");
        completed = test_wait_atomic_u32(&contended_malloc_args.done, 1u);
        epoch_after_contended_reuse = heap->core.metadata_epoch;
        test_spin_unlock(&heap->lock);
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "lockless remote contended malloc join");
        CloseHandle(thread);
        expect_true(completed,
                    "lockless remote malloc completes while heap lock held");
    }
#else
    {
        pthread_t thread;
        int completed;

        test_spin_lock(&heap->lock);
        expect_true(pthread_create(&thread, 0,
                                   accounted_remote_contended_malloc_worker,
                                   &contended_malloc_args) == 0,
                    "lockless remote contended malloc create thread");
        completed = test_wait_atomic_u32(&contended_malloc_args.done, 1u);
        epoch_after_contended_reuse = heap->core.metadata_epoch;
        test_spin_unlock(&heap->lock);
        expect_true(pthread_join(thread, 0) == 0,
                    "lockless remote contended malloc join");
        expect_true(completed,
                    "lockless remote malloc completes while heap lock held");
    }
#endif
    contended_reuse = contended_malloc_args.result;
    expect_true(contended_malloc_args.ok,
                "lockless remote contended malloc worker ok");
    expect_true(contended_reuse == first,
                "lockless remote contended malloc reuses slot");
    expect_true(epoch_after_contended_reuse == epoch_before_contended_reuse,
                "lockless remote contended malloc skips heap-lock epoch");
    expect_true(test_slab_remote_queue_empty(slab),
                "lockless remote contended malloc queue empty");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote contended malloc pops class queue");
    expect_ok(api->check(heap, 0),
              "lockless remote contended malloc check");

    memset(&contended_free_args, 0, sizeof(contended_free_args));
    contended_free_args.api = api;
    contended_free_args.heap = heap;
    contended_free_args.ptr = contended_reuse;
    atomic_init(&contended_free_args.done, 0u);
    epoch_before_contended_free = heap->core.metadata_epoch;
#if defined(_WIN32)
    {
        HANDLE thread;
        int completed;

        test_spin_lock(&heap->lock);
        thread = CreateThread(0, 0, accounted_remote_contended_free_worker,
                              &contended_free_args, 0, 0);
        expect_true(thread != 0,
                    "lockless remote contended free create thread");
        completed = test_wait_atomic_u32(&contended_free_args.done, 1u);
        epoch_after_contended_free = heap->core.metadata_epoch;
        test_spin_unlock(&heap->lock);
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "lockless remote contended free join");
        CloseHandle(thread);
        expect_true(completed,
                    "lockless remote free completes while heap lock held");
    }
#else
    {
        pthread_t thread;
        int completed;

        test_spin_lock(&heap->lock);
        expect_true(pthread_create(&thread, 0,
                                   accounted_remote_contended_free_worker,
                                   &contended_free_args) == 0,
                    "lockless remote contended free create thread");
        completed = test_wait_atomic_u32(&contended_free_args.done, 1u);
        epoch_after_contended_free = heap->core.metadata_epoch;
        test_spin_unlock(&heap->lock);
        expect_true(pthread_join(thread, 0) == 0,
                    "lockless remote contended free join");
        expect_true(completed,
                    "lockless remote free completes while heap lock held");
    }
#endif
    expect_true(contended_free_args.ok,
                "lockless remote contended free worker ok");
    expect_true(epoch_after_contended_free == epoch_before_contended_free,
                "lockless remote contended free skips heap-lock epoch");
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote contended free queued");

    epoch_before_reuse = heap->core.metadata_epoch;
    reuse = api->malloc(heap, 64u);
    epoch_after_reuse = heap->core.metadata_epoch;
    expect_true(reuse == first, "lockless remote drain reuses slot");
    expect_true(epoch_after_reuse == epoch_before_reuse,
                "lockless remote malloc skips idle metadata epoch");
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    state_requested = (uint32_t)(state_word >> 32);
    expect_true(state_flags == UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED &&
                    state_requested == 64u &&
                    slot->flags == 0 && slot->requested_size == 64u,
                "lockless remote same-size reuse live deferred");
    expect_true(test_slab_remote_queue_empty(slab),
                "lockless remote malloc clears slab remote counters");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote malloc pops class queue");
    expect_true((slot->flags & (UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                                UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) == 0,
                "lockless remote malloc clears flags");
    expect_true(slab->live_slots == live_before &&
                    slab->free_slots == free_before,
                "lockless remote malloc keeps slab counts live");
    expect_ok(api->check(heap, 0), "lockless remote final check before free");
    api->free(heap, reuse);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote second free queued");
    mismatch_reuse = api->malloc(heap, 63u);
    expect_true(mismatch_reuse == reuse,
                "lockless remote requested-size mismatch reuses lockless");
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    state_requested = (uint32_t)(state_word >> 32);
    expect_true(state_flags == UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED &&
                    state_requested == 63u &&
                    slot->flags == 0 && slot->requested_size == 64u,
                "lockless remote mismatch keeps raw mirror deferred");
    expect_true(api->malloc_usable_size(heap, mismatch_reuse) >= 63u,
                "lockless remote mismatch usable size works");
    expect_ok(api->check(heap, 0),
              "lockless remote mismatch live-deferred check");
    api->free(heap, mismatch_reuse);
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "lockless remote mismatch stats after free");
    expect_true(stats.malloc_live_bytes == 0,
                "lockless remote mismatch free uses atomic requested");
    mismatch_reuse = api->malloc(heap, 62u);
    expect_true(mismatch_reuse == reuse,
                "lockless remote mismatch second reuse");
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    state_requested = (uint32_t)(state_word >> 32);
    expect_true(state_flags == UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED &&
                    state_requested == 62u,
                "lockless remote mismatch second live deferred");
    mismatch_reuse = api->realloc(heap, mismatch_reuse, 61u);
    expect_true(mismatch_reuse == reuse,
                "lockless remote live-deferred realloc in place");
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    state_requested = (uint32_t)(state_word >> 32);
    expect_true(state_flags == 0 && state_requested == 61u &&
                    slot->flags == 0 && slot->requested_size == 61u,
                "lockless remote realloc normalizes mirror");
    expect_ok(api->check(heap, 0),
              "lockless remote realloc normalized check");
    api->free(heap, mismatch_reuse);
    expect_ok(api->close(heap), "lockless remote close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_32bit_partial_low_lock_remote_free(
    const uhm_storage_info *heap_info) {
#if UINTPTR_MAX != UINT32_MAX
    (void)heap_info;
    return;
#else
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    accounted_remote_contended_free_args free_args;
    void *first;
    void *second;
    void *reuse;
    uhm_slab_slot *first_slot;
    uhm_slab *slab;
    size_t live_before;
    size_t free_before;
    int completed;
#if UHMAS_ENABLE_TEST_HOOKS
    uint64_t scan_slabs_before;
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t heap_lock_calls_before;
    uint64_t heap_lock_calls_after;
    uint64_t raw_lock_calls_before;
    uint64_t raw_lock_calls_after;
#endif

    if (!uhm_atomic_u32_is_lock_free()) {
        return;
    }
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "32-bit low-lock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "32-bit low-lock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "32-bit low-lock usermode vm init");

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
    expect_ok(uhm_open(&config, &heap), "32-bit low-lock heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && heap->core.low_lock_remote_free_enabled == 1u,
                "32-bit low-lock capability enabled internally");
    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0 && first != second,
                "32-bit low-lock partial allocations");
    first_slot = test_slab_slot_from_payload(first);
    slab = first_slot ? first_slot->slab : 0;
    expect_true(slab != 0 && slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->live_slots > 1u && slab->free_slots != 0u,
                "32-bit low-lock partial slab geometry");
    live_before = slab->live_slots;
    free_before = slab->free_slots;

#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_spinlock_set_diagnostics_enabled(&heap->lock, 1u);
    uhm_spinlock_set_diagnostics_enabled(&slab->raw_lock, 1u);
#endif
    memset(&free_args, 0, sizeof(free_args));
    free_args.api = api;
    free_args.heap = heap;
    free_args.ptr = first;
    atomic_init(&free_args.done, 0u);
    test_spin_lock(&heap->lock);
#if UHMAS_ENABLE_DIAGNOSTICS
    heap_lock_calls_before =
        uhm_diag_counter_load_relaxed(&heap->lock.lock_calls);
    raw_lock_calls_before =
        uhm_diag_counter_load_relaxed(&slab->raw_lock.lock_calls);
#endif
#if defined(_WIN32)
    {
        HANDLE thread = CreateThread(0, 0,
                                     accounted_remote_contended_free_worker,
                                     &free_args, 0, 0);
        expect_true(thread != 0, "32-bit low-lock free thread create");
        completed = test_wait_atomic_u32(&free_args.done, 1u);
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "32-bit low-lock free thread join");
        CloseHandle(thread);
    }
#else
    {
        pthread_t thread;
        expect_true(pthread_create(&thread, 0,
                                   accounted_remote_contended_free_worker,
                                   &free_args) == 0,
                    "32-bit low-lock free thread create");
        completed = test_wait_atomic_u32(&free_args.done, 1u);
        expect_true(pthread_join(thread, 0) == 0,
                    "32-bit low-lock free thread join");
    }
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    heap_lock_calls_after =
        uhm_diag_counter_load_relaxed(&heap->lock.lock_calls);
    raw_lock_calls_after =
        uhm_diag_counter_load_relaxed(&slab->raw_lock.lock_calls);
#endif
    test_spin_unlock(&heap->lock);
    expect_true(completed && free_args.ok,
                "32-bit low-lock free completes under heap lock");
#if UHMAS_ENABLE_DIAGNOSTICS
    expect_true(heap_lock_calls_after == heap_lock_calls_before,
                "32-bit low-lock free skips heap lock");
    expect_true(raw_lock_calls_after == raw_lock_calls_before + 1u,
                "32-bit low-lock free uses slab raw lock");
#endif
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->live_slots == live_before - 1u &&
                    slab->free_slots == free_before + 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    slab->remote_free_head == first_slot,
                "32-bit low-lock free publishes raw queue and counts");
    expect_true(test_slab_slot_state_flags_load(first_slot) ==
                    (UHM_SLAB_SLOT_FLAG_FREE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) &&
                    test_slab_slot_state_requested_load(first_slot) == 0u &&
                    first_slot->flags ==
                        (UHM_SLAB_SLOT_FLAG_FREE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) &&
                    first_slot->requested_size == 0u,
                "32-bit low-lock free publishes slot state");
    expect_ok(api->check(heap, 0), "32-bit low-lock check after free");
#if UHMAS_ENABLE_TEST_HOOKS
    scan_slabs_before = heap->core.slab_remote_class_scan_slabs;
#endif
    reuse = api->malloc(heap, 64u);
    expect_true(reuse == first,
                "32-bit low-lock malloc drains and reuses raw queue");
#if UHMAS_ENABLE_TEST_HOOKS
    expect_true(heap->core.slab_remote_class_scan_slabs == scan_slabs_before,
                "32-bit low-lock malloc skips slab-all recovery scan");
#endif
    expect_true(test_slab_remote_pending(slab) == 0u &&
                    slab->remote_free_head == 0,
                "32-bit low-lock queue drained");
    api->free(heap, reuse);
    api->free(heap, second);
    expect_ok(api->check(heap, 0), "32-bit low-lock final check");
    expect_ok(api->close(heap), "32-bit low-lock heap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
#endif
}

#if UINTPTR_MAX == UINT32_MAX
static int test_low_lock_contended_free(const uhm_api *api,
                                        uhm_heap *heap,
                                        void *ptr,
                                        int *out_completed_while_locked) {
    accounted_remote_contended_free_args args;
    size_t i;
    int completed;

    if (out_completed_while_locked) {
        *out_completed_while_locked = 0;
    }
    memset(&args, 0, sizeof(args));
    args.api = api;
    args.heap = heap;
    args.ptr = ptr;
    atomic_init(&args.done, 0u);

    test_spin_lock(&heap->lock);
#if defined(_WIN32)
    {
        HANDLE thread = CreateThread(0, 0, accounted_remote_contended_free_worker,
                                     &args, 0, 0);
        expect_true(thread != 0, "32-bit fallback free thread create");
        for (i = 0; i < 4096u; ++i) {
            if (atomic_load_explicit(&args.done, UHM_ATOMIC_ACQUIRE) != 0u) {
                break;
            }
            test_thread_pause();
        }
        completed = atomic_load_explicit(&args.done, UHM_ATOMIC_ACQUIRE) != 0u;
        test_spin_unlock(&heap->lock);
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "32-bit fallback free thread join");
        CloseHandle(thread);
    }
#else
    {
        pthread_t thread;
        expect_true(pthread_create(&thread, 0,
                                   accounted_remote_contended_free_worker,
                                   &args) == 0,
                    "32-bit fallback free thread create");
        for (i = 0; i < 4096u; ++i) {
            if (atomic_load_explicit(&args.done, UHM_ATOMIC_ACQUIRE) != 0u) {
                break;
            }
            test_thread_pause();
        }
        completed = atomic_load_explicit(&args.done, UHM_ATOMIC_ACQUIRE) != 0u;
        test_spin_unlock(&heap->lock);
        expect_true(pthread_join(thread, 0) == 0,
                    "32-bit fallback free thread join");
    }
#endif
    if (out_completed_while_locked) {
        *out_completed_while_locked = completed;
    }
    return args.ok;
}

static int test_open_32bit_low_lock_heap(
    const uhm_storage_info *heap_info,
    test_alloc *heap_mem,
    test_alloc *arena_mem,
    uhm_vm_ops *vm,
    uhm_heap **out_heap,
    const uhm_api **out_api) {
    const size_t page_size = uhm_platform_system_page_size();
    uhm_config config;

    if (!heap_info || !heap_mem || !arena_mem || !vm || !out_heap ||
        !out_api || !uhm_atomic_u32_is_lock_free()) {
        return 0;
    }
    memset(heap_mem, 0, sizeof(*heap_mem));
    memset(arena_mem, 0, sizeof(*arena_mem));
    *out_heap = 0;
    *out_api = 0;
    if (!test_vm_reserve_commit_alloc(heap_info->storage_size,
                                      heap_info->storage_alignment, heap_mem) ||
        !test_vm_reserve_commit_alloc(1024u * 1024u, page_size, arena_mem)) {
        test_vm_release_alloc(arena_mem);
        test_vm_release_alloc(heap_mem);
        return 0;
    }
    if (uhm_platform_usermode_vm_ops_init(
            vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS) != UHM_OK) {
        test_vm_release_alloc(arena_mem);
        test_vm_release_alloc(heap_mem);
        return 0;
    }
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE;
    config.heap_storage = heap_mem->aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem->aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = *vm;
    if (uhm_open(&config, out_heap) != UHM_OK || !*out_heap) {
        test_vm_release_alloc(arena_mem);
        test_vm_release_alloc(heap_mem);
        return 0;
    }
    *out_api = uhm_get_api(*out_heap);
    return *out_api != 0 && (*out_heap)->core.low_lock_remote_free_enabled != 0u;
}
#endif

static void test_slab_low_lock_fallback_boundaries(
    const uhm_storage_info *heap_info) {
#if UINTPTR_MAX != UINT32_MAX
    (void)heap_info;
    return;
#else
    test_alloc heap_mem;
    test_alloc arena_mem;
    uhm_vm_ops vm;
    uhm_heap *heap = 0;
    const uhm_api *api = 0;
    void **slots;
    uhm_slab_slot *target_slot;
    uhm_slab *slab;
    size_t slot_count;
    size_t i;
    int completed_while_locked;
    uhm_result last_error;
    uint32_t last_platform_error;

    if (!uhm_atomic_u32_is_lock_free()) {
        return;
    }
    expect_true(test_open_32bit_low_lock_heap(
                    heap_info, &heap_mem, &arena_mem, &vm, &heap, &api),
                "32-bit fallback boundary heap open");
    slot_count = 0;
    slots = 0;
    slots = (void **)calloc(256u, sizeof(*slots));
    expect_true(slots != 0, "32-bit fallback boundary pointer storage");
    slots[0] = api->malloc(heap, 64u);
    expect_true(slots[0] != 0, "32-bit fallback full target allocation");
    target_slot = test_slab_slot_from_payload(slots[0]);
    slab = target_slot ? target_slot->slab : 0;
    expect_true(slab != 0 && slab->total_slots <= 256u,
                "32-bit fallback slab fits pointer storage");
    slot_count = slab ? slab->total_slots : 0u;
    for (i = 1u; i < slot_count; ++i) {
        slots[i] = api->malloc(heap, 64u);
        expect_true(slots[i] != 0 &&
                        test_slab_slot_from_payload(slots[i])->slab == slab,
                    "32-bit fallback fill one slab");
    }
    expect_true(slab->state == UHM_SLAB_STATE_FULL && slab->free_slots == 0u,
                "32-bit fallback full slab state");
    expect_true(test_low_lock_contended_free(api, heap, slots[0],
                                             &completed_while_locked),
                "32-bit full fallback free succeeds");
    expect_true(!completed_while_locked,
                "32-bit full fallback waits for heap lock");
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL &&
                    slab->live_slots == slot_count - 1u &&
                    slab->free_slots == 1u &&
                    test_slab_remote_pending(slab) == 1u,
                "32-bit full fallback transitions to partial");
    expect_ok(api->check(heap, 0), "32-bit full fallback check");
    for (i = 1u; i < slot_count; ++i) {
        api->free(heap, slots[i]);
    }
    expect_ok(api->check(heap, 0), "32-bit fallback close-drain precheck");
    expect_true(slab->state == UHM_SLAB_STATE_EMPTY && slab->live_slots == 0u &&
                    test_slab_remote_pending(slab) == slot_count,
                "32-bit close-drain pending raw queue");
    expect_ok(api->close(heap), "32-bit fallback close drains raw queue");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);

    expect_true(test_open_32bit_low_lock_heap(
                    heap_info, &heap_mem, &arena_mem, &vm, &heap, &api),
                "32-bit last-live fallback heap open");
    slots[0] = api->malloc(heap, 64u);
    slots[1] = api->malloc(heap, 64u);
    target_slot = test_slab_slot_from_payload(slots[0]);
    slab = target_slot ? target_slot->slab : 0;
    expect_true(slab != 0 && test_slab_slot_from_payload(slots[1])->slab == slab,
                "32-bit last-live same slab");
    api->free(heap, slots[1]);
    expect_true(slab->state == UHM_SLAB_STATE_PARTIAL && slab->live_slots == 1u,
                "32-bit last-live setup");
    expect_true(test_low_lock_contended_free(api, heap, slots[0],
                                             &completed_while_locked),
                "32-bit last-live fallback free succeeds");
    expect_true(!completed_while_locked,
                "32-bit last-live fallback waits for heap lock");
    expect_true(slab->state == UHM_SLAB_STATE_EMPTY && slab->live_slots == 0u &&
                    test_slab_remote_pending(slab) == 2u,
                "32-bit last-live fallback transitions to empty");
    expect_ok(api->check(heap, 0), "32-bit last-live fallback check");
    expect_ok(api->close(heap), "32-bit last-live fallback close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);

    expect_true(test_open_32bit_low_lock_heap(
                    heap_info, &heap_mem, &arena_mem, &vm, &heap, &api),
                "32-bit VM-busy fallback heap open");
    slots[0] = api->malloc(heap, 64u);
    slots[1] = api->malloc(heap, 64u);
    target_slot = test_slab_slot_from_payload(slots[0]);
    slab = target_slot ? target_slot->slab : 0;
    expect_true(slab != 0 && test_slab_slot_from_payload(slots[1])->slab == slab,
                "32-bit VM-busy same slab");
    slab->flags |= UHM_SLAB_FLAG_VM_BUSY;
    expect_true(test_low_lock_contended_free(api, heap, slots[0],
                                             &completed_while_locked),
                "32-bit VM-busy fallback free succeeds");
    expect_true(!completed_while_locked,
                "32-bit VM-busy fallback waits for heap lock");
    slab->flags &= ~UHM_SLAB_FLAG_VM_BUSY;
    last_error = UHM_ERR_CORRUPTION;
    last_platform_error = UINT32_MAX;
    expect_ok(api->get_last_error(heap, &last_error, &last_platform_error),
              "32-bit VM-busy fallback error query");
    expect_true(last_error == UHM_OK && last_platform_error == 0u,
                "32-bit VM-busy fallback leaves no API error");
    api->free(heap, slots[1]);
    expect_ok(api->check(heap, 0), "32-bit VM-busy fallback check");
    expect_ok(api->close(heap), "32-bit VM-busy fallback close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
    free(slots);
#endif
}

