static void test_slab_lockless_remote_publish_rollback(
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

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless remote rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless remote rollback arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless remote rollback usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "lockless remote rollback heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless remote rollback api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless remote rollback allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless remote rollback slab metadata");

    heap->core.fault_lockless_remote_publish_countdown = 1u;
    api->free(heap, ptr);
    expect_true(heap->core.fault_lockless_remote_publish_countdown == 0,
                "lockless remote rollback fault consumed");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "lockless remote rollback stats after free");
    expect_true(stats.malloc_live_bytes == 0,
                "lockless remote rollback stats restored");
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    expect_true((state_flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) == 0,
                "lockless remote rollback clears unaccounted");
    expect_true((state_flags & (UHM_SLAB_SLOT_FLAG_FREE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING)) ==
                    (UHM_SLAB_SLOT_FLAG_FREE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING),
                "lockless remote rollback fallback state");
    expect_true(slot->flags == state_flags && slot->requested_size == 0,
                "lockless remote rollback fallback mirror synced");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote rollback skips class queue");
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "lockless remote rollback uses slab queue");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "lockless remote rollback counters restored once");
    expect_true(uhm_metadata_hazard_readers_load(heap) == 0,
                "lockless remote rollback reader count restored");
    expect_ok(api->check(heap, 0), "lockless remote rollback check");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == ptr, "lockless remote rollback fallback reuse");
    expect_ok(api->check(heap, 0), "lockless remote rollback final check");
    api->free(heap, reuse);
    expect_ok(api->close(heap), "lockless remote rollback close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_remote_alloc_rollback(
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
    void *later_reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_stats stats;
    uint64_t state_word;
    uint32_t state_flags;
    uint32_t state_requested;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless remote alloc rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless remote alloc rollback arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless remote alloc rollback usermode vm init");

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
              "lockless remote alloc rollback heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless remote alloc rollback api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless remote alloc rollback allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless remote alloc rollback slab metadata");
    api->free(heap, ptr);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote alloc rollback queued");

    heap->core.fault_lockless_remote_alloc_countdown = 1u;
    reuse = api->malloc(heap, 64u);
    expect_true(reuse != 0 && reuse != ptr,
                "lockless remote alloc rollback locked fallback skips class queue");
    expect_true(heap->core.fault_lockless_remote_alloc_countdown == 0,
                "lockless remote alloc rollback fault consumed");
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote alloc rollback class queue retained");
    expect_true(slab->remote_free_head == 0 &&
                    uhm_tagged_freelist_unpack_ptr(atomic_load_explicit(
                        &slab->remote_free_lf.head,
                        UHM_ATOMIC_ACQUIRE)) == 0,
                "lockless remote alloc rollback slab queue empty");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "lockless remote alloc rollback counters retained");
    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    state_requested = (uint32_t)(state_word >> 32);
    expect_true(slot->flags == 0 && slot->requested_size == 64u &&
                    (state_flags &
                     (UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                      UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) ==
                        (UHM_SLAB_SLOT_FLAG_FREE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                         UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) &&
                    state_requested == 0,
                "lockless remote alloc rollback remote state retained");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "lockless remote alloc rollback stats");
    expect_true(stats.malloc_live_bytes == 64u,
                "lockless remote alloc rollback live bytes");
    expect_last_error(api, heap, UHM_OK, 0u,
                      "lockless remote alloc rollback no public error");
    expect_ok(api->check(heap, 0), "lockless remote alloc rollback check");

    later_reuse = api->malloc(heap, 64u);
    expect_true(later_reuse == ptr,
                "lockless remote alloc rollback later lockless reuse");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote alloc rollback class queue later drained");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "lockless remote alloc rollback counters later drained");
    expect_ok(api->check(heap, 0),
              "lockless remote alloc rollback later check");

    api->free(heap, reuse);
    api->free(heap, later_reuse);
    expect_ok(api->close(heap), "lockless remote alloc rollback close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_remote_corruption_stops_fallback(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *failed;
    void *reuse;
    void *descriptor_failed;
    void *descriptor_reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_page_record *record;
    size_t first_page;
    size_t page_count;
    uint64_t saved_generation;
    uint16_t saved_size_class;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless remote corruption heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless remote corruption arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless remote corruption usermode vm init");

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
              "lockless remote corruption heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless remote corruption api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless remote corruption allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless remote corruption slab metadata");
    api->free(heap, ptr);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote corruption queued");

    saved_size_class = slot->size_class;
    slot->size_class = (uint16_t)((saved_size_class + 1u) %
                                  UHM_SIZE_CLASS_COUNT);
    failed = api->malloc(heap, 64u);
    expect_true(failed == 0,
                "lockless remote corruption stops locked fallback");
    expect_last_error(api, heap, UHM_ERR_CORRUPTION, 0u,
                      "lockless remote corruption last error");

    slot->size_class = saved_size_class;
    uhm_tagged_freelist_next_store(&slot->remote_link, 0);
    expect_true(uhm_tagged_freelist_push(
                    &heap->core.slab_remote_class_lf[slab->size_class].queue,
                    &slot->remote_link),
                "lockless remote corruption restore queue");
    expect_ok(api->check(heap, 0),
              "lockless remote corruption restored check");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == ptr, "lockless remote corruption reuse restored");

    api->free(heap, reuse);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote descriptor corruption queued");
    expect_true(test_shadow_page_span(heap, slab->base, slab->size,
                                      &first_page, &page_count) &&
                    page_count != 0,
                "lockless remote descriptor corruption page span");
    record = test_page_record_at(heap, first_page);
    expect_true(record != 0,
                "lockless remote descriptor corruption page record");
    saved_generation = record->generation;
    record->generation = saved_generation + 1u;
    descriptor_failed = api->malloc(heap, 64u);
    expect_true(descriptor_failed == 0,
                "lockless remote descriptor corruption stops fallback");
    expect_last_error(api, heap, UHM_ERR_CORRUPTION, 0u,
                      "lockless remote descriptor corruption last error");
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote descriptor corruption queue restored");
    record->generation = saved_generation;
    expect_ok(api->check(heap, 0),
              "lockless remote descriptor corruption restored check");
    descriptor_reuse = api->malloc(heap, 64u);
    expect_true(descriptor_reuse == ptr,
                "lockless remote descriptor corruption reuse restored");
    api->free(heap, descriptor_reuse);
    expect_ok(api->close(heap), "lockless remote corruption close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_remote_concurrent_reuse(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    enum { ptr_count = 16, thread_count = 16 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptrs[ptr_count];
    lockless_remote_reuse_args args[thread_count];
    _Atomic(uint32_t) ready_count;
    _Atomic(uint32_t) start;
    uhm_slab *slab = 0;
    uhm_stats stats;
    size_t i;
    size_t j;

    memset(ptrs, 0, sizeof(ptrs));
    memset(args, 0, sizeof(args));
    atomic_init(&ready_count, 0u);
    atomic_init(&start, 0u);

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless remote concurrent heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless remote concurrent arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless remote concurrent usermode vm init");

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
              "lockless remote concurrent heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless remote concurrent api");

    for (i = 0; i < ptr_count; ++i) {
        uhm_slab_slot *slot;
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0,
                    "lockless remote concurrent seed allocation");
        slot = test_slab_slot_from_payload(ptrs[i]);
        expect_true(slot != 0 && slot->slab != 0,
                    "lockless remote concurrent seed metadata");
        if (i == 0) {
            slab = slot->slab;
            expect_true(slab->total_slots >= ptr_count,
                        "lockless remote concurrent one slab capacity");
        } else {
            expect_true(slot->slab == slab,
                        "lockless remote concurrent seeds share slab");
        }
    }

    for (i = 0; i < ptr_count; ++i) {
        api->free(heap, ptrs[i]);
        expect_true(test_slab_remote_class_queue_contains(
                        heap, slab, test_slab_slot_from_payload(ptrs[i])),
                    "lockless remote concurrent seed queued");
    }
    expect_true(test_slab_remote_pending_mirror(slab) == ptr_count &&
                    test_slab_remote_pending(slab) == ptr_count &&
                    test_slab_remote_inflight_mirror(slab) == ptr_count,
                "lockless remote concurrent counters queued");
    expect_ok(api->check(heap, 0),
              "lockless remote concurrent queued check");

    for (i = 0; i < thread_count; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].ready_count = &ready_count;
        args[i].start = &start;
        args[i].id = (unsigned)i;
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, lockless_remote_reuse_worker,
                                      &args[i], 0, 0);
            expect_true(threads[i] != 0,
                        "lockless remote concurrent create thread");
        }
        while (atomic_load_explicit(&ready_count, UHM_ATOMIC_ACQUIRE) !=
               thread_count) {
            test_thread_pause();
        }
        atomic_store_explicit(&start, 1u, UHM_ATOMIC_RELEASE);
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) ==
                            WAIT_OBJECT_0,
                        "lockless remote concurrent wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0,
                                       lockless_remote_reuse_worker,
                                       &args[i]) == 0,
                        "lockless remote concurrent create thread");
        }
        while (atomic_load_explicit(&ready_count, UHM_ATOMIC_ACQUIRE) !=
               thread_count) {
            test_thread_pause();
        }
        atomic_store_explicit(&start, 1u, UHM_ATOMIC_RELEASE);
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0,
                        "lockless remote concurrent join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        int found = 0;
        uhm_slab_slot *slot;

        expect_true(args[i].ok && args[i].result != 0,
                    "lockless remote concurrent worker result");
        for (j = 0; j < ptr_count; ++j) {
            if (args[i].result == ptrs[j]) {
                found = 1;
                break;
            }
        }
        expect_true(found, "lockless remote concurrent reused seed pointer");
        for (j = 0; j < i; ++j) {
            expect_true(args[i].result != args[j].result,
                        "lockless remote concurrent unique result");
        }
        slot = test_slab_slot_from_payload(args[i].result);
        {
            uint64_t state_word =
                atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
            uint32_t state_flags =
                (uint32_t)(state_word & UINT64_C(0xffffffff));
            uint32_t state_requested = (uint32_t)(state_word >> 32);
            expect_true(state_flags == UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED &&
                            state_requested == 64u &&
                            slot->flags == 0 &&
                            slot->requested_size == 64u &&
                            test_slab_remote_class_queue_contains(heap, slab,
                                                                  slot) == 0,
                    "lockless remote concurrent live slot state");
        }
    }

    expect_true(test_slab_remote_queue_empty(slab),
                "lockless remote concurrent remote queues empty");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "lockless remote concurrent stats");
    expect_true(stats.malloc_live_bytes == 64u * thread_count,
                "lockless remote concurrent live bytes");
    expect_ok(api->check(heap, 0),
              "lockless remote concurrent post reuse check");

    for (i = 0; i < thread_count; ++i) {
        api->free(heap, args[i].result);
    }
    expect_ok(api->close(heap), "lockless remote concurrent close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_decommit_defers_while_metadata_reader_active(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *second;
    void *reuse;
    uhm_slab *first_slab;
    uhm_slab *second_slab;
    size_t slot_count;
    size_t decommitted_before;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab decommit reader heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "slab decommit reader arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab decommit reader usermode vm init");

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
              "slab decommit reader heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab decommit reader api");

    ptrs = (void **)calloc(page_size, sizeof(*ptrs));
    expect_true(ptrs != 0, "slab decommit reader pointer table");

    ptrs[0] = api->malloc(heap, 64u);
    expect_true(ptrs[0] != 0, "slab decommit reader first allocation");
    first_slab = test_slab_slot_from_payload(ptrs[0])->slab;
    slot_count = first_slab->total_slots;
    expect_true(slot_count != 0 && slot_count <= page_size,
                "slab decommit reader slot count");
    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0,
                    "slab decommit reader fill first slab");
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == first_slab,
                    "slab decommit reader same first slab");
    }
    second = api->malloc(heap, 64u);
    expect_true(second != 0, "slab decommit reader second slab allocation");
    second_slab = test_slab_slot_from_payload(second)->slab;
    expect_true(second_slab != 0 && second_slab != first_slab,
                "slab decommit reader distinct second slab");

    for (i = 0; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
    }
    expect_true(first_slab->state == UHM_SLAB_STATE_EMPTY &&
                    (first_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab decommit reader first slab empty committed");
    expect_ok(api->check(heap, 0),
              "slab decommit reader check after first empty");

    decommitted_before = heap->stats.decommitted_bytes;
    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    expect_true(uhm_metadata_hazard_begin(heap),
                "slab decommit reader begin");
    api->free(heap, second);
    expect_true((second_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "slab decommit reader active defers second slab");
    expect_true(heap->stats.decommitted_bytes == decommitted_before,
                "slab decommit reader active stats unchanged");
    expect_ok(api->check(heap, 0),
              "slab decommit reader active check");
    expect_true(uhm_metadata_hazard_end(heap),
                "slab decommit reader end");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse != 0, "slab decommit reader reuse after reader");
    api->free(heap, reuse);
    expect_true(heap->stats.decommitted_bytes >= decommitted_before + page_size,
                "slab decommit reader later decommits");
    expect_true(((first_slab->flags | second_slab->flags) &
                 UHM_SLAB_FLAG_DECOMMITTED) != 0,
                "slab decommit reader one slab decommitted");
    expect_ok(api->check(heap, 0),
              "slab decommit reader final check");
    expect_ok(api->close(heap), "slab decommit reader close");

    free(ptrs);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_lockless_remote_reader_slots_gate_lifecycle_and_decommit(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void **ptrs;
    void *second;
    void *reuse;
    uhm_slab *first_slab;
    uhm_slab *second_slab;
    uhm_slab_slot *second_slot;
    size_t slot_count;
    size_t decommitted_before;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless reader heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless reader arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless reader usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "lockless reader heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless reader api");

    ptrs = (void **)calloc(page_size, sizeof(*ptrs));
    expect_true(ptrs != 0, "lockless reader pointer table");

    ptrs[0] = api->malloc(heap, 64u);
    expect_true(ptrs[0] != 0, "lockless reader first allocation");
    first_slab = test_slab_slot_from_payload(ptrs[0])->slab;
    slot_count = first_slab->total_slots;
    expect_true(slot_count != 0 && slot_count <= page_size,
                "lockless reader slot count");
    for (i = 1; i < slot_count; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0, "lockless reader fill first slab");
        expect_true(test_slab_slot_from_payload(ptrs[i])->slab == first_slab,
                    "lockless reader same first slab");
    }
    second = api->malloc(heap, 64u);
    expect_true(second != 0, "lockless reader second allocation");
    second_slot = test_slab_slot_from_payload(second);
    second_slab = second_slot->slab;
    expect_true(second_slab != 0 && second_slab != first_slab,
                "lockless reader distinct second slab");

    for (i = 0; i < slot_count; ++i) {
        api->free(heap, ptrs[i]);
    }
    expect_true(first_slab->state == UHM_SLAB_STATE_EMPTY &&
                    (first_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "lockless reader first slab empty committed");

    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    expect_true(uhm_metadata_reader_begin(
                    &heap->core.lockless_remote_readers[0], heap),
                "lockless reader slot begin");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 0u,
                    second_slot),
                "lockless reader slot hazard");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 1u,
                    second_slab),
                "lockless reader slab hazard");
    expect_ok(api->check(heap, 0),
              "lockless reader known hazard check");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 0u,
                    (void *)(heap->core.arena_base + 3u)),
                "lockless reader unknown hazard set");
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "lockless reader unknown hazard diagnosed");
    expect_true(uhm_metadata_reader_hazard_set(
                    &heap->core.lockless_remote_readers[0], 0u,
                    second_slot),
                "lockless reader hazard restore");
    expect_ok(api->check(heap, 0),
              "lockless reader restored hazard check");

    decommitted_before = heap->stats.decommitted_bytes;
    api->free(heap, second);
    expect_true((second_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0,
                "lockless reader active defers decommit");
    expect_true(heap->stats.decommitted_bytes == decommitted_before,
                "lockless reader active keeps decommit stats");
    expect_true(api->close(heap) == UHM_ERR_BUSY,
                "lockless reader active close busy");
    expect_ok(api->check(heap, 0), "lockless reader active check");
    expect_true(uhm_metadata_reader_end(
                    &heap->core.lockless_remote_readers[0]),
                "lockless reader slot end");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse != 0, "lockless reader reuse after end");
    api->free(heap, reuse);
    expect_true(heap->stats.decommitted_bytes >=
                    decommitted_before + page_size,
                "lockless reader later decommits");
    expect_ok(api->check(heap, 0), "lockless reader final check");
    expect_ok(api->close(heap), "lockless reader close");

    free(ptrs);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

