/* 32-bit raw batches stay entirely inside the slab raw-lock transaction. */
#if UINTPTR_MAX == UINT32_MAX
enum { TEST_RAW_BATCH_COUNT = UHM_RAW_REMOTE_BATCH_LIMIT + 4u };

static uhm_slab *test_raw_batch_prepare(const uhm_api *api, uhm_heap *heap,
                                       void **live) {
    uhm_slab *slab;
    size_t i;

    live[0] = api->malloc(heap, 64u);
    expect_true(live[0] != 0, "raw batch first allocation");
    slab = test_slab_slot_from_payload(live[0])->slab;
    expect_true(slab->total_slots > TEST_RAW_BATCH_COUNT + 2u,
                "raw batch geometry leaves a partial slab");
    for (i = 1u; i < TEST_RAW_BATCH_COUNT + 2u; ++i) {
        live[i] = api->malloc(heap, 64u);
        expect_true(live[i] != 0 &&
                        test_slab_slot_from_payload(live[i])->slab == slab,
                    "raw batch allocations share a slab");
    }
    for (i = 2u; i < TEST_RAW_BATCH_COUNT + 2u; ++i) {
        api->free(heap, live[i]);
        live[i] = 0;
    }
    expect_true(test_slab_remote_pending(slab) == TEST_RAW_BATCH_COUNT &&
                    slab->live_slots == 2u &&
                    slab->state == UHM_SLAB_STATE_PARTIAL,
                "raw batch has more than one reservation");
    return slab;
}

static void *test_raw_batch_malloc_under_heap_lock(const uhm_api *api,
                                                  uhm_heap *heap) {
    accounted_remote_contended_malloc_args args;
    int completed;

    memset(&args, 0, sizeof(args));
    args.api = api;
    args.heap = heap;
    atomic_init(&args.done, 0u);
    test_spin_lock(&heap->lock);
#if defined(_WIN32)
    {
        HANDLE thread = CreateThread(0, 0,
            accounted_remote_contended_malloc_worker, &args, 0, 0);
        expect_true(thread != 0, "raw batch contended malloc create");
        completed = test_wait_atomic_u32(&args.done, 1u);
        test_spin_unlock(&heap->lock);
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "raw batch contended malloc join");
        CloseHandle(thread);
    }
#else
    {
        pthread_t thread;
        expect_true(pthread_create(&thread, 0,
            accounted_remote_contended_malloc_worker, &args) == 0,
                    "raw batch contended malloc create");
        completed = test_wait_atomic_u32(&args.done, 1u);
        test_spin_unlock(&heap->lock);
        expect_true(pthread_join(thread, 0) == 0,
                    "raw batch contended malloc join");
    }
#endif
    expect_true(completed && args.ok,
                "raw batch malloc completes while heap lock held");
    expect_true(((unsigned char *)args.result)[0] == 0x6c &&
                    ((unsigned char *)args.result)[63] == 0x6c,
                "raw batch contended payload is writable");
    return args.result;
}

static void test_raw_batch_case(const uhm_storage_info *heap_info,
                                unsigned mode) {
    test_alloc heap_mem;
    test_alloc arena_mem;
    uhm_vm_ops vm;
    uhm_heap *heap;
    const uhm_api *api;
    void *live[TEST_RAW_BATCH_COUNT + 2u] = {0};
    uhm_slab_slot *nodes[TEST_RAW_BATCH_COUNT];
    uhm_slab_slot *old_free;
    uhm_slab *slab;
    size_t i;
    uint64_t seq;
    uhm_result error;
    uint32_t platform_error;

    expect_true(test_open_32bit_low_lock_heap(
                    heap_info, &heap_mem, &arena_mem, &vm, &heap, &api),
                "raw batch heap open");
    slab = test_raw_batch_prepare(api, heap, live);
    nodes[0] = slab->remote_free_head;
    for (i = 1u; i < TEST_RAW_BATCH_COUNT; ++i) {
        nodes[i] = nodes[i - 1u]->next_free;
    }
    expect_ok(api->check(heap, 0), "raw batch prepared heap check");
    old_free = slab->free_head;
    seq = atomic_load_explicit(&slab->remote_counter_seq, UHM_ATOMIC_ACQUIRE);
    if (mode == 0u) {
        live[2] = test_raw_batch_malloc_under_heap_lock(api, heap);
        expect_true(live[2] == (void *)((uintptr_t)nodes[0] +
                                       test_slab_slot_header_size()),
                    "raw batch keeps remote LIFO reuse");
        expect_true(test_slab_remote_pending(slab) ==
                        TEST_RAW_BATCH_COUNT - UHM_RAW_REMOTE_BATCH_LIMIT &&
                        slab->remote_free_head ==
                            nodes[UHM_RAW_REMOTE_BATCH_LIMIT] &&
                        atomic_load_explicit(&slab->remote_counter_seq,
                                             UHM_ATOMIC_ACQUIRE) ==
                            seq + (UINT64_C(1) << 32),
                    "raw batch decrements a bounded prefix in one sequence");
        live[3] = test_raw_batch_malloc_under_heap_lock(api, heap);
        expect_true(test_slab_remote_pending(slab) == 0u &&
                        slab->remote_free_head == 0,
                    "raw batch second reservation drains remainder");
        seq = atomic_load_explicit(&slab->remote_counter_seq,
                                    UHM_ATOMIC_ACQUIRE);
        live[4] = test_raw_batch_malloc_under_heap_lock(api, heap);
        expect_true(atomic_load_explicit(&slab->remote_counter_seq,
                                         UHM_ATOMIC_ACQUIRE) == seq &&
                        slab->live_slots == 5u,
                    "raw free-head reuse does not decrement pending twice");
    } else {
        if (mode == 1u) {
#if UHMAS_ENABLE_TEST_HOOKS
            heap->core.fault_lockless_remote_drain_countdown = 1u;
#endif
        } else if (mode == 2u) {
            nodes[5]->next_free = (uhm_slab_slot *)(uintptr_t)1u;
        } else if (mode == 3u) {
            nodes[5]->slab = 0;
        } else if (mode == 4u) {
            nodes[5]->next_free = nodes[2];
        } else if (mode == 5u) {
            atomic_store_explicit(&slab->remote_free_slots, 1u,
                                  UHM_ATOMIC_RELEASE);
            atomic_store_explicit(&slab->remote_free_pending, 1u,
                                  UHM_ATOMIC_RELEASE);
        } else if (mode == 6u) {
            nodes[5]->requested_size = 1u;
        }
        expect_true(api->malloc(heap, 64u) == 0,
                    "raw batch failure stops unsafe allocation");
        expect_ok(api->get_last_error(heap, &error, &platform_error),
                  "raw batch failure error query");
        expect_true(error == (mode == 1u ? UHM_ERR_OUT_OF_MEMORY :
                                          UHM_ERR_CORRUPTION),
                    "raw batch failure retains exact error");
        expect_true(slab->remote_free_head == nodes[0] &&
                        slab->free_head == old_free && slab->live_slots == 2u &&
                        test_slab_remote_pending(slab) ==
                            (mode == 5u ? 1u : TEST_RAW_BATCH_COUNT) &&
                        test_slab_remote_pending_mirror(slab) ==
                            (mode == 5u ? 1u : TEST_RAW_BATCH_COUNT),
                    "raw batch failure restores heads and counters");
        for (i = 0u; i < TEST_RAW_BATCH_COUNT; ++i) {
            expect_true(test_slab_slot_state_flags_load(nodes[i]) ==
                            (UHM_SLAB_SLOT_FLAG_FREE |
                             UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE),
                        "raw batch failure leaves no drain markers");
        }
        nodes[5]->next_free = nodes[6];
        nodes[5]->slab = slab;
        nodes[5]->requested_size = 0u;
        if (mode == 5u) {
            atomic_store_explicit(&slab->remote_free_slots,
                                  TEST_RAW_BATCH_COUNT, UHM_ATOMIC_RELEASE);
            atomic_store_explicit(&slab->remote_free_pending,
                                  TEST_RAW_BATCH_COUNT, UHM_ATOMIC_RELEASE);
        }
        expect_true(atomic_load_explicit(&slab->remote_drain_active,
                                         UHM_ATOMIC_ACQUIRE) == 0u &&
                        atomic_load_explicit(&slab->remote_drain_epoch,
                                             UHM_ATOMIC_ACQUIRE) == 0u &&
                        atomic_load_explicit(&heap->metadata_hazard_readers,
                                             UHM_ATOMIC_ACQUIRE) == 0u,
                    "raw batch failure clears owner and readers");
        expect_ok(api->check(heap, 0), "raw batch rollback heap check");
        live[2] = api->malloc(heap, 64u);
        expect_true(live[2] != 0, "raw batch retry succeeds");
    }
    expect_true(uhm_metadata_registered_readers_load(heap) == 0u,
                "raw batch releases registered reader accounting");
    for (i = 0u; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        size_t hazard;
        uhm_metadata_reader_state *reader = &heap->core.lockless_remote_readers[i];
        expect_true(atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE) == 0u,
                    "raw batch leaves no active reader slot");
        for (hazard = 0u; hazard < UHM_METADATA_HAZARD_SLOTS; ++hazard) {
            expect_true(atomic_load_explicit(&reader->hazards[hazard],
                                             UHM_ATOMIC_ACQUIRE) == 0u,
                        "raw batch clears every reader hazard");
        }
    }
    expect_ok(api->check(heap, 0), "raw batch final heap check");
    for (i = 0u; i < TEST_RAW_BATCH_COUNT + 2u; ++i) {
        if (live[i]) {
            api->free(heap, live[i]);
        }
    }
    expect_ok(api->close(heap), "raw batch heap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}
#endif

static void test_slab_32bit_raw_batch(const uhm_storage_info *heap_info) {
#if UINTPTR_MAX == UINT32_MAX
    unsigned mode;
    if (!uhm_atomic_u32_is_lock_free()) {
        return;
    }
    for (mode = 0u; mode <= 6u; ++mode) {
#if !UHMAS_ENABLE_TEST_HOOKS
        if (mode == 1u) {
            continue;
        }
#endif
        test_raw_batch_case(heap_info, mode);
    }
#else
    (void)heap_info;
#endif
}

static void test_slab_32bit_raw_single_counter_transaction(
    const uhm_storage_info *heap_info) {
#if UINTPTR_MAX == UINT32_MAX
    unsigned mode;

    if (!uhm_atomic_u32_is_lock_free()) {
        return;
    }
    for (mode = 0u; mode < 3u; ++mode) {
        test_alloc heap_mem;
        test_alloc arena_mem;
        uhm_vm_ops vm;
        uhm_heap *heap;
        const uhm_api *api;
        uhm_slab *slab;
        uhm_slab_slot *slot;
        uhm_slab_slot *old_free;
        void *live[3];
        uhm_stats before;
        uhm_stats after;
        size_t old_live;
        size_t old_free_count;
        size_t i;
        uint64_t counter_seq;

        expect_true(test_open_32bit_low_lock_heap(
                        heap_info, &heap_mem, &arena_mem, &vm, &heap, &api),
                    "raw single counter heap open");
        for (i = 0u; i < 3u; ++i) {
            live[i] = api->malloc(heap, 64u);
            expect_true(live[i] != 0, "raw single counter setup malloc");
        }
        slot = test_slab_slot_from_payload(live[2]);
        slab = slot->slab;
        expect_true(test_slab_slot_from_payload(live[0])->slab == slab &&
                        test_slab_slot_from_payload(live[1])->slab == slab,
                    "raw single counter setup shares slab");
        api->free(heap, live[2]);
        expect_true(slab->remote_free_head == slot && slot->next_free == 0 &&
                        test_slab_remote_pending(slab) == 1u &&
                        test_slab_remote_pending_mirror(slab) == 1u,
                    "raw single counter setup has one pending node");
        old_free = slab->free_head;
        old_live = slab->live_slots;
        old_free_count = slab->free_slots;
        counter_seq = atomic_load_explicit(&slab->remote_counter_seq,
                                           UHM_ATOMIC_ACQUIRE);
        memset(&before, 0, sizeof(before));
        before.size = sizeof(before);
        expect_ok(api->get_stats(heap, &before), "raw single counter before stats");
        if (mode != 0u) {
            uhm_result error;
            uint32_t platform_error;

            atomic_store_explicit(mode == 1u ? &slab->remote_free_slots :
                                              &slab->remote_free_pending,
                                  0u, UHM_ATOMIC_RELEASE);
            expect_true(api->malloc(heap, 64u) == 0,
                        "raw single counter drift rejects allocation");
            expect_ok(api->get_last_error(heap, &error, &platform_error),
                      "raw single counter drift error query");
            expect_true(error == UHM_ERR_CORRUPTION,
                        "raw single counter drift exact error");
            expect_true(slab->remote_free_head == slot && slot->next_free == 0 &&
                            slab->free_head == old_free &&
                            slab->live_slots == old_live &&
                            slab->free_slots == old_free_count &&
                            test_slab_slot_state_flags_load(slot) ==
                                (UHM_SLAB_SLOT_FLAG_FREE |
                                 UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) &&
                            slot->requested_size == 0u,
                        "raw single counter failure leaves allocation unpublished");
            expect_true(test_slab_remote_pending(slab) == (mode == 2u ? 0u : 1u) &&
                            test_slab_remote_pending_mirror(slab) == (mode == 1u ? 0u : 1u) &&
                            atomic_load_explicit(&slab->remote_counter_seq,
                                                 UHM_ATOMIC_ACQUIRE) == counter_seq,
                        "raw single counter failure does not change accounting");
            expect_true(atomic_load_explicit(&slab->remote_ready_queued,
                                             UHM_ATOMIC_ACQUIRE) == 1u,
                        "raw single counter failure preserves ready notification");
            memset(&after, 0, sizeof(after));
            after.size = sizeof(after);
            expect_ok(api->get_stats(heap, &after), "raw single counter failure stats");
            expect_true(after.malloc_live_bytes == before.malloc_live_bytes,
                        "raw single counter failure preserves live byte stats");
            atomic_store_explicit(&slab->remote_free_slots, 1u, UHM_ATOMIC_RELEASE);
            atomic_store_explicit(&slab->remote_free_pending, 1u, UHM_ATOMIC_RELEASE);
            expect_ok(api->check(heap, 0), "raw single counter restored heap check");
        }
        live[2] = test_raw_batch_malloc_under_heap_lock(api, heap);
        expect_true(test_slab_slot_from_payload(live[2]) == slot &&
                        slab->remote_free_head == 0 && slab->free_head == old_free &&
                        slab->live_slots == old_live + 1u &&
                        slab->free_slots == old_free_count - 1u &&
                        test_slab_remote_pending(slab) == 0u &&
                        test_slab_remote_pending_mirror(slab) == 0u &&
                        atomic_load_explicit(&slab->remote_counter_seq,
                                             UHM_ATOMIC_ACQUIRE) ==
                            counter_seq + (UINT64_C(1) << 32),
                    "raw single counter success publishes exactly once");
        expect_true(uhm_metadata_registered_readers_load(heap) == 0u &&
                        atomic_load_explicit(&heap->metadata_hazard_readers,
                                             UHM_ATOMIC_ACQUIRE) == 0u,
                    "raw single counter success and failure release readers");
        expect_ok(api->check(heap, 0), "raw single counter final heap check");
        for (i = 0u; i < 3u; ++i) {
            api->free(heap, live[i]);
        }
        expect_ok(api->close(heap), "raw single counter heap close");
        test_vm_release_alloc(&arena_mem);
        test_vm_release_alloc(&heap_mem);
    }
#else
    (void)heap_info;
#endif
}
