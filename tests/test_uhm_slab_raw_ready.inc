/* Raw ready entries identify slabs, not ownership of their remote slots. */
#if UINTPTR_MAX == UINT32_MAX
static void test_slab_raw_ready_direct_decommit_case(
    const uhm_storage_info *heap_info, unsigned mode) {
    test_alloc heap_mem;
    test_alloc arena_mem;
    uhm_vm_ops vm;
    uhm_heap *heap;
    const uhm_api *api;
    uhm_slab *first_slab;
    uhm_slab *second_slab;
    void **ptrs;
    size_t first_count;
    size_t i;

    if (!uhm_atomic_u32_is_lock_free()) {
        return;
    }
    expect_true(test_open_32bit_low_lock_heap(
                    heap_info, &heap_mem, &arena_mem, &vm, &heap, &api),
                "32-bit raw ready decommit heap open");
    {
        void *first = api->malloc(heap, 64u);
        expect_true(first != 0, "32-bit raw ready initial allocation");
        first_slab = test_slab_slot_from_payload(first)->slab;
        first_count = first_slab->total_slots;
        ptrs = (void **)calloc(first_count + 2u, sizeof(*ptrs));
        expect_true(ptrs != 0, "32-bit raw ready pointer storage");
        ptrs[0] = first;
    }
    for (i = 1u; i < first_count + 2u; ++i) {
        ptrs[i] = api->malloc(heap, 64u);
        expect_true(ptrs[i] != 0, "32-bit raw ready fill two slabs");
    }
    second_slab = test_slab_slot_from_payload(ptrs[first_count])->slab;
    expect_true(second_slab != first_slab &&
                    test_slab_slot_from_payload(ptrs[first_count + 1u])->slab ==
                        second_slab,
                "32-bit raw ready second partial slab");
    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    for (i = 0u; i < first_count + 2u; ++i) {
        api->free(heap, ptrs[i]);
    }
    expect_true((second_slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0u &&
                    second_slab->remote_free_head == 0 &&
                    test_slab_remote_pending(second_slab) == 0u,
                "32-bit direct drain decommits second slab");
    expect_true(atomic_load_explicit(&second_slab->remote_ready_queued,
                                    UHM_ATOMIC_ACQUIRE) ==
                    1u,
                "32-bit direct drain leaves a stale ready notification");
    expect_true(atomic_load_explicit(
                    &heap->core.slab_remote_ready_raw[second_slab->size_class]
                         .value,
                    UHM_ATOMIC_ACQUIRE) == second_slab,
                "32-bit stale decommitted slab is first ready candidate");
    if (mode == 1u) {
        void *reuse = api->malloc(heap, 64u);
        expect_true(reuse != 0,
                    "32-bit malloc skips stale decommitted notification");
        api->free(heap, reuse);
    } else if (mode == 2u) {
        /* Never dereference this intentionally invalid slot address. */
        second_slab->remote_free_head = (uhm_slab_slot *)(uintptr_t)1u;
        expect_true(api->close(heap) == UHM_ERR_CORRUPTION,
                    "32-bit decommitted nonempty raw queue stays corruption");
        expect_true(second_slab->remote_free_head ==
                        (uhm_slab_slot *)(uintptr_t)1u,
                    "32-bit invalid decommitted head is not consumed");
        second_slab->remote_free_head = 0;
    } else if (mode == 3u) {
        uint64_t generation = second_slab->desc.generation;
        second_slab->desc.generation ^= UINT64_C(1);
        expect_true(api->malloc(heap, 64u) == 0,
                    "32-bit stale ready with invalid descriptor is rejected");
        {
            uhm_result error;
            uint32_t platform_error;
            expect_ok(api->get_last_error(heap, &error, &platform_error),
                      "32-bit stale ready error query");
            expect_true(error == UHM_ERR_CORRUPTION,
                        "32-bit descriptor mismatch reports corruption");
        }
        second_slab->desc.generation = generation;
    } else if (mode == 4u) {
        /* unpack_ptr deliberately returns NULL on unsupported 32-bit queues. */
        atomic_store_explicit(&second_slab->remote_free_lf.head,
                              (uintptr_t)1u, UHM_ATOMIC_RELEASE);
        expect_true(api->close(heap) == UHM_ERR_CORRUPTION,
                    "32-bit invalid tagged queue head stays corruption");
        expect_true(atomic_load_explicit(&second_slab->remote_free_lf.head,
                                         UHM_ATOMIC_ACQUIRE) == (uintptr_t)1u,
                    "32-bit invalid tagged head is not interpreted as empty");
        atomic_store_explicit(&second_slab->remote_free_lf.head,
                              (uintptr_t)0u, UHM_ATOMIC_RELEASE);
    }
    expect_ok(api->check(heap, 0), "32-bit stale decommitted ready check");
    expect_ok(api->close(heap), "32-bit stale decommitted ready close");
    free(ptrs);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

typedef struct test_raw_ready_stress_args {
    const uhm_api *api;
    uhm_heap *heap;
    void *slots[8];
    unsigned id;
    int batch;
    int ok;
} test_raw_ready_stress_args;

#if defined(_WIN32)
static DWORD WINAPI test_raw_ready_stress_worker(LPVOID arg) {
#else
static void *test_raw_ready_stress_worker(void *arg) {
#endif
    test_raw_ready_stress_args *args = (test_raw_ready_stress_args *)arg;
    unsigned round;
    size_t i;

    args->ok = 1;
    for (round = 0u; round < 1000u && args->ok; ++round) {
        for (i = 0u; i < 8u; ++i) {
            unsigned char *ptr = (unsigned char *)args->slots[i];
            unsigned char pattern = (unsigned char)(args->id + i);
            if (!ptr || ptr[0] != pattern || ptr[63] != pattern) {
                args->ok = 0;
                break;
            }
            args->api->free(args->heap, ptr);
            if (args->batch) {
                args->slots[i] = 0;
                continue;
            }
            ptr = (unsigned char *)args->api->malloc(args->heap, 64u);
            args->slots[i] = ptr;
            if (!ptr) {
                args->ok = 0;
                break;
            }
            memset(ptr, pattern, 64u);
        }
        if (args->batch && args->ok) {
            for (i = 0u; i < 8u; ++i) {
                args->slots[i] = args->api->malloc(args->heap, 64u);
                if (!args->slots[i]) {
                    args->ok = 0;
                    break;
                }
                memset(args->slots[i], (int)(args->id + i), 64u);
            }
        }
    }
    for (i = 0u; i < 8u; ++i) {
        if (args->slots[i]) {
            args->api->free(args->heap, args->slots[i]);
            args->slots[i] = 0;
        }
    }
    return 0;
}
#endif

static void test_slab_32bit_raw_ready_after_direct_decommit(
    const uhm_storage_info *heap_info) {
#if UINTPTR_MAX != UINT32_MAX
    (void)heap_info;
#else
    unsigned mode;
    for (mode = 0u; mode < 5u; ++mode) {
        test_slab_raw_ready_direct_decommit_case(heap_info, mode);
    }
#endif
}

static void test_slab_32bit_raw_ready_late_notification(
    const uhm_storage_info *heap_info) {
#if UINTPTR_MAX != UINT32_MAX
    (void)heap_info;
#else
    test_alloc heap_mem;
    test_alloc arena_mem;
    uhm_vm_ops vm;
    uhm_heap *heap;
    const uhm_api *api;
    void *first;
    void *second;
    uhm_slab *slab;

    if (!uhm_atomic_u32_is_lock_free()) {
        return;
    }
    expect_true(test_open_32bit_low_lock_heap(
                    heap_info, &heap_mem, &arena_mem, &vm, &heap, &api),
                "32-bit late ready heap open");
    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0, "32-bit late ready allocations");
    slab = test_slab_slot_from_payload(first)->slab;
    api->free(heap, first);
    first = api->malloc(heap, 64u);
    expect_true(first != 0 && slab->remote_free_head == 0 &&
                    test_slab_remote_pending(slab) == 0u,
                "32-bit late ready remote slots consumed");
    /*
     * Reproduce the state of a delayed producer/consumer requeue after
     * another consumer used the final remote slot. No slot state changes.
     */
    uhm_tagged_freelist_next_store(&slab->remote_ready_link, 0);
    atomic_store_explicit(&slab->remote_ready_queued, 1u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(
        &heap->core.slab_remote_ready_raw[slab->size_class].value, slab,
        UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "32-bit late ready remains a valid hint");
    /* close permits live objects in a caller-owned arena. */
    expect_ok(api->close(heap), "32-bit empty committed ready close");
    expect_true(slab->remote_free_head == 0 &&
                    test_slab_remote_pending(slab) == 0u &&
                    atomic_load_explicit(&slab->remote_ready_queued,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "32-bit late ready discarded without slot accounting");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
#endif
}

static void test_slab_32bit_raw_ready_concurrent_reuse_case(
    const uhm_storage_info *heap_info, int batch) {
#if UINTPTR_MAX != UINT32_MAX
    (void)heap_info;
    (void)batch;
#else
    test_alloc heap_mem;
    test_alloc arena_mem;
    uhm_vm_ops vm;
    uhm_heap *heap;
    const uhm_api *api;
    test_raw_ready_stress_args args[4];
    uhm_stats stats;
    size_t t;
    size_t i;
#if defined(_WIN32)
    HANDLE threads[4];
#else
    pthread_t threads[4];
#endif

    if (!uhm_atomic_u32_is_lock_free()) {
        return;
    }
    expect_true(test_open_32bit_low_lock_heap(
                    heap_info, &heap_mem, &arena_mem, &vm, &heap, &api),
                "32-bit concurrent ready heap open");
    heap->core.test_slab_empty_decommit_high_water_override = 1u;
    memset(args, 0, sizeof(args));
    for (t = 0u; t < 4u; ++t) {
        args[t].api = api;
        args[t].heap = heap;
        args[t].id = (unsigned)(t * 16u);
        args[t].batch = batch;
        for (i = 0u; i < 8u; ++i) {
            args[t].slots[i] = api->malloc(heap, 64u);
            expect_true(args[t].slots[i] != 0,
                        "32-bit concurrent ready preallocation");
            memset(args[t].slots[i], (int)(args[t].id + i), 64u);
        }
    }
    for (t = 0u; t < 4u; ++t) {
#if defined(_WIN32)
        threads[t] = CreateThread(0, 0, test_raw_ready_stress_worker,
                                  &args[t], 0, 0);
        expect_true(threads[t] != 0, "32-bit ready worker create");
#else
        expect_true(pthread_create(&threads[t], 0, test_raw_ready_stress_worker,
                                    &args[t]) == 0,
                    "32-bit ready worker create");
#endif
    }
    for (t = 0u; t < 4u; ++t) {
#if defined(_WIN32)
        expect_true(WaitForSingleObject(threads[t], INFINITE) == WAIT_OBJECT_0,
                    "32-bit ready worker join");
        CloseHandle(threads[t]);
#else
        expect_true(pthread_join(threads[t], 0) == 0,
                    "32-bit ready worker join");
#endif
        expect_true(args[t].ok, "32-bit concurrent ready payload and reuse");
    }
    expect_ok(api->check(heap, 0), "32-bit concurrent ready final check");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "32-bit concurrent ready stats");
    expect_true(stats.malloc_live_bytes == 0u,
                "32-bit concurrent ready no lost live accounting");
    expect_ok(api->close(heap), "32-bit concurrent ready close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
#endif
}

static void test_slab_32bit_raw_ready_concurrent_reuse(
    const uhm_storage_info *heap_info) {
    test_slab_32bit_raw_ready_concurrent_reuse_case(heap_info, 0);
}

static void test_slab_32bit_raw_batch_concurrent_reuse(
    const uhm_storage_info *heap_info) {
    test_slab_32bit_raw_ready_concurrent_reuse_case(heap_info, 1);
}
