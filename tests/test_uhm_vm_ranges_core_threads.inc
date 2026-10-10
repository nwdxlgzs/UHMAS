static void test_multithread_small_object_churn(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 8u * 1024u * 1024u;
    enum { thread_count = 4 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    small_churn_args args[thread_count];
    uhm_stats stats;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "small churn heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "small churn arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "small churn usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "small churn heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "small churn api");

    memset(args, 0, sizeof(args));
    for (i = 0; i < thread_count; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].id = (unsigned)i;
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, small_churn_worker, &args[i], 0, 0);
            expect_true(threads[i] != 0, "small churn create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                        "small churn wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, small_churn_worker, &args[i]) == 0,
                        "small churn create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0, "small churn join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        if (!args[i].ok) {
            uint32_t corruption_site = atomic_load_explicit(
                &heap->core.debug_last_corruption_site,
                UHM_ATOMIC_ACQUIRE);
            fprintf(stderr,
                    "small churn worker %u failed: stage=%u iteration=%zu "
                    "last_error=%d platform_error=%u corruption_site=%u\n",
                    args[i].id, args[i].fail_stage,
                    args[i].fail_iteration, (int)args[i].last_error,
                    args[i].platform_error, corruption_site);
        }
        expect_true(args[i].ok, "small churn worker completed");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "small churn stats");
    expect_true(stats.malloc_live_bytes == 0, "small churn releases all malloc bytes");
    expect_ok(api->check(heap, 0), "small churn heap check");
    expect_ok(api->close(heap), "small churn heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_multithread_large_realloc(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 64u * 1024u * 1024u;
    enum { thread_count = 4 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    large_realloc_args args[thread_count];
    uhm_stats stats;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "large realloc heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "large realloc arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "large realloc usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "large realloc heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "large realloc api");

    memset(args, 0, sizeof(args));
    for (i = 0; i < thread_count; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].id = (unsigned)i;
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, large_realloc_worker, &args[i], 0, 0);
            expect_true(threads[i] != 0, "large realloc create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                        "large realloc wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, large_realloc_worker, &args[i]) == 0,
                        "large realloc create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0, "large realloc join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        expect_true(args[i].ok, "large realloc worker completed");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "large realloc stats");
    expect_true(stats.malloc_live_bytes == 0, "large realloc releases all malloc bytes");
    expect_ok(api->check(heap, 0), "large realloc heap check");
    expect_ok(api->close(heap), "large realloc heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_cross_thread_free(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 16u * 1024u * 1024u;
    enum { thread_count = 4, ptrs_per_thread = 256, total_ptrs = thread_count * ptrs_per_thread };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptrs[total_ptrs];
    size_t sizes[total_ptrs];
    cross_thread_free_args args[thread_count];
    uhm_stats stats;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "cross free heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "cross free arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "cross free usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "cross free heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "cross free api");

    memset(ptrs, 0, sizeof(ptrs));
    memset(sizes, 0, sizeof(sizes));
    memset(args, 0, sizeof(args));
    for (i = 0; i < thread_count; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].ptrs = ptrs;
        args[i].sizes = sizes;
        args[i].start = i * ptrs_per_thread;
        args[i].count = ptrs_per_thread;
        args[i].id = (unsigned)i;
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, cross_thread_alloc_worker, &args[i], 0, 0);
            expect_true(threads[i] != 0, "cross free alloc create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                        "cross free alloc wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, cross_thread_alloc_worker, &args[i]) == 0,
                        "cross free alloc create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0, "cross free alloc join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        if (!args[i].ok) {
            uint32_t corruption_site = atomic_load_explicit(
                &heap->core.debug_last_corruption_site, UHM_ATOMIC_ACQUIRE);
            fprintf(stderr,
                    "cross free allocator worker %u failed at index %zu "
                    "last_error=%d platform_error=%u corruption_site=%u "
                    "malloc_hi=%p mmap_lo=%p low_inflight=%u "
                    "low_addr=%p low_size=%zu\n",
                    args[i].id, args[i].fail_index,
                    (int)args[i].last_error, args[i].platform_error,
                    corruption_site, (void *)heap->core.malloc_hi,
                    (void *)heap->core.mmap_lo,
                    heap->core.low_watermark_claim_inflight,
                    (void *)heap->core.low_watermark_claim_addr,
                    heap->core.low_watermark_claim_size);
        }
        expect_true(args[i].ok, "cross free allocator completed");
    }

    for (i = 0; i < thread_count; ++i) {
        size_t source = (i + 1u) % thread_count;
        args[i].start = source * ptrs_per_thread;
        args[i].count = ptrs_per_thread;
        args[i].id = (unsigned)source;
        args[i].ok = 0;
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, cross_thread_free_worker, &args[i], 0, 0);
            expect_true(threads[i] != 0, "cross free free create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                        "cross free free wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, cross_thread_free_worker, &args[i]) == 0,
                        "cross free free create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0, "cross free free join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        expect_true(args[i].ok, "cross free freer completed");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "cross free stats");
    expect_true(stats.malloc_live_bytes == 0, "cross free releases all malloc bytes");
    expect_ok(api->check(heap, 0), "cross free heap check");
    expect_ok(api->close(heap), "cross free heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_ctx_open_close_concurrent_alloc(const uhm_storage_info *heap_info,
                                                 const uhm_storage_info *ctx_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 16u * 1024u * 1024u;
    enum { thread_count = 4 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem[thread_count];
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    ctx_churn_args args[thread_count];
    uhm_stats stats;
    size_t i;

    memset(ctx_mem, 0, sizeof(ctx_mem));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "ctx churn heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "ctx churn arena alloc");
    for (i = 0; i < thread_count; ++i) {
        expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size, ctx_info->storage_alignment,
                                        &ctx_mem[i]) != 0,
                    "ctx churn ctx storage alloc");
    }
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "ctx churn usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "ctx churn heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "ctx churn api");

    memset(args, 0, sizeof(args));
    for (i = 0; i < thread_count; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].ctx_storage = ctx_mem[i].aligned;
        args[i].ctx_storage_size = ctx_info->storage_size;
        args[i].id = (unsigned)i;
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            threads[i] = CreateThread(0, 0, ctx_churn_worker, &args[i], 0, 0);
            expect_true(threads[i] != 0, "ctx churn create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                        "ctx churn wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, ctx_churn_worker, &args[i]) == 0,
                        "ctx churn create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0, "ctx churn join thread");
        }
    }
#endif

    for (i = 0; i < thread_count; ++i) {
        expect_true(args[i].ok, "ctx churn worker completed");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "ctx churn stats");
    expect_true(stats.malloc_live_bytes == 0, "ctx churn releases all malloc bytes");
    expect_ok(api->check(heap, 0), "ctx churn heap check");
    expect_ok(api->close(heap), "ctx churn heap close");

    for (i = 0; i < thread_count; ++i) {
        test_vm_release_alloc(&ctx_mem[i]);
    }
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_thread_ctx_local_cache_flush(const uhm_storage_info *heap_info,
                                              const uhm_storage_info *ctx_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    void *first;
    void *reuse;
    void *after_close;
    uhm_slab_slot *cached_slot;
    uhm_stats stats;
    uint64_t epoch_after_malloc;
    uint64_t epoch_after_free_ctx;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "ctx cache heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "ctx cache arena alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size, ctx_info->storage_alignment,
                                    &ctx_mem) != 0,
                "ctx cache ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "ctx cache usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "ctx cache heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "ctx cache api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    ctx_config.local_cache_bytes = page_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx), "ctx cache open");

    first = api->malloc_ctx(heap, ctx, 64u);
    expect_true(first != 0, "ctx cache first malloc");
    memset(first, 0xa5, 64u);
    epoch_after_malloc = heap->core.metadata_epoch;
    api->free_ctx(heap, ctx, first);
    epoch_after_free_ctx = heap->core.metadata_epoch;
    expect_true(epoch_after_free_ctx == epoch_after_malloc,
                "ctx cache push does not advance metadata epoch");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "ctx cache stats after free_ctx");
    expect_true(stats.malloc_live_bytes == 0, "ctx cache free_ctx removes live bytes");
    expect_true(ctx->local_cache_count == 1u && ctx->local_cache_head != 0,
                "ctx cache stores freed slot");
    cached_slot = ctx->local_cache_head;
    expect_true((void *)((uintptr_t)cached_slot + test_slab_slot_header_size()) == first,
                "ctx cache tracks expected slot");
    expect_ok(api->check(heap, 0), "ctx cache check with cached slot");

    reuse = api->malloc_ctx(heap, ctx, 48u);
    expect_true(reuse == first, "ctx cache malloc_ctx reuses local slot");
    expect_true(heap->core.metadata_epoch == epoch_after_free_ctx,
                "ctx cache pop does not advance metadata epoch");
    api->free_ctx(heap, ctx, reuse);
    expect_ok(api->check(heap, 0), "ctx cache check after reuse");

    {
        uint64_t saved_state =
            atomic_load_explicit(&cached_slot->state_word,
                                 UHM_ATOMIC_ACQUIRE);
        atomic_store_explicit(
            &cached_slot->state_word,
            test_slab_slot_state_pack(0u, 64u),
            UHM_ATOMIC_RELEASE);
        expect_true(api->thread_ctx_flush(heap, ctx) == UHM_ERR_CORRUPTION,
                    "ctx cache corrupt flush fails without spinning");
        atomic_store_explicit(&cached_slot->state_word, saved_state,
                              UHM_ATOMIC_RELEASE);
        expect_ok(api->check(heap, 0),
                  "ctx cache check after corrupt flush restore");
    }

    api->thread_ctx_close(heap, ctx);
    expect_ok(api->check(heap, 0), "ctx cache check after close flush");
    after_close = api->malloc(heap, 64u);
    expect_true(after_close == first, "ctx cache close flush returns slot to heap");
    api->free(heap, after_close);

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "ctx cache final stats");
    expect_true(stats.malloc_live_bytes == 0, "ctx cache final live bytes");
    expect_ok(api->check(heap, 0), "ctx cache final check");
    expect_ok(api->close(heap), "ctx cache heap close");

    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_thread_ctx_retained_page_rotation(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    void *allocs[2048] = {0};
    size_t slab_representatives[17] = {0};
    uhm_slab *last_slab = 0;
    size_t count = 0;
    size_t slab_count = 0;
    size_t i;
    void *idle_ptr;

    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "ctx retained rotation heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    1024u * 1024u, page_size, &arena_mem) != 0,
                "ctx retained rotation arena alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    ctx_info->storage_size, ctx_info->storage_alignment,
                    &ctx_mem) != 0,
                "ctx retained rotation ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "ctx retained rotation usermode vm init");

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
    expect_ok(uhm_open(&config, &heap), "ctx retained rotation heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "ctx retained rotation api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    ctx_config.local_cache_bytes = page_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "ctx retained rotation open");

    while (count < sizeof(allocs) / sizeof(allocs[0]) &&
           slab_count < sizeof(slab_representatives) /
                            sizeof(slab_representatives[0])) {
        allocs[count] = api->malloc_ctx(heap, ctx, 64u);
        expect_true(allocs[count] != 0,
                    "ctx retained rotation fill malloc");
        if (test_slab_slot_from_payload(allocs[count])->slab != last_slab) {
            last_slab = test_slab_slot_from_payload(allocs[count])->slab;
            slab_representatives[slab_count++] = count;
        }
        ++count;
    }
    expect_true(slab_count == sizeof(slab_representatives) /
                                  sizeof(slab_representatives[0]),
                "ctx retained rotation reaches ceiling plus one slab");

    for (i = 0; i + 1u < slab_count; ++i) {
        api->free_ctx(heap, ctx, allocs[slab_representatives[i]]);
    }
    expect_true(ctx->retained_slab_pages ==
                        ctx->retained_slab_page_limit &&
                    ctx->local_cache_count == slab_count - 1u,
                "ctx retained rotation fills the physical ceiling");
    api->free_ctx(heap, ctx, allocs[slab_representatives[slab_count - 1u]]);
    expect_true(ctx->retained_slab_pages ==
                        ctx->retained_slab_page_limit &&
                    ctx->local_cache_count == slab_count - 1u &&
                    ctx->local_cache_head ==
                        test_slab_slot_from_payload(
                            allocs[slab_representatives[slab_count - 1u]]) &&
                    (test_slab_slot_from_payload(
                         allocs[slab_representatives[0]])->flags &
                     UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0u,
                "ctx retained pressure evicts the cold slab");
    expect_ok(api->check(heap, 0),
              "ctx retained rotation check after pressure");

    for (i = 0; i < UHM_LOCAL_CACHE_IDLE_MISS_THRESHOLD; ++i) {
        idle_ptr = api->malloc_ctx(heap, ctx, 1024u);
        expect_true(idle_ptr != 0, "ctx retained idle miss malloc");
        api->free(heap, idle_ptr);
    }
    expect_true(ctx->retained_slab_pages ==
                        ctx->retained_slab_page_limit - 1u &&
                    ctx->local_cache_count == slab_count - 2u &&
                    atomic_load_explicit(&ctx->idle_miss_streak,
                                         UHM_ATOMIC_ACQUIRE) == 0u &&
                    (test_slab_slot_from_payload(
                         allocs[slab_representatives[1]])->flags &
                     UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0u,
                "ctx retained idle misses evict one excess slab");
    expect_ok(api->check(heap, 0),
              "ctx retained rotation check after idle aging");

    for (i = 0u; i < count; ++i) {
        size_t representative;
        int already_freed = 0;
        for (representative = 0; representative < slab_count;
             ++representative) {
            if (slab_representatives[representative] == i) {
                already_freed = 1;
                break;
            }
        }
        if (!already_freed) {
            api->free(heap, allocs[i]);
        }
    }
    api->thread_ctx_close(heap, ctx);
    expect_ok(api->check(heap, 0), "ctx retained rotation check after close");
    expect_ok(api->close(heap), "ctx retained rotation heap close");

    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_thread_ctx_local_cache_partial_lowlock(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    void *first;
    void *second;
    void *third;
    void *reuse;
    void *empty_cache_alloc;
    void *drift_alloc;
    void *head_small;
    void *head_preserved;
    void *head_removed;
    void *head_reuse;
    uhm_slab_slot *first_slot;
    uhm_slab_slot *second_slot;
    uhm_slab_slot *head_small_slot;
    uhm_slab_slot *head_preserved_slot;
    uhm_slab_slot *head_removed_slot;
    uhm_slab *slab;
    uint64_t lock_before;
    uint64_t raw_lock_before;
    uint64_t page_span_before;
    uint64_t page_lock_calls_before;
    uint64_t page_lock_shards_before;
    uint64_t reader_seq_before;
    size_t readers_before;
    uintptr_t free_head_ref_before;
    uint32_t state_mirror_before;
    size_t live_count_before;
    size_t free_count_before;
    size_t cached_count_before;
    size_t cached_page;
    size_t cached_page_count;
    size_t cached_hint_base;
    uhm_page_record *cached_record;
    uint64_t cached_generation;
    uint32_t cached_prot;
    uhm_slab_slot *proof_heads[UHM_SIZE_CLASS_COUNT] = {0};
    uhm_local_retained_hint
        proof_hints[UHM_LOCAL_RETAINED_HINT_COUNT] = {{0}};
    uhm_slab proof_collision_candidates[UHM_LOCAL_RETAINED_HINT_COUNT * 2u];
    uhm_slab_slot proof_collision_slot;
    uhm_slab *proof_collision_slab = 0;
    size_t proof_retained_pages = 1u;
    size_t proof_retained_pages_before;
    size_t proof_hint_base;
    size_t proof_hint_outside;
    size_t proof_slab_pages;
    size_t proof_candidate_index;
    size_t proof_existing_slot_count = SIZE_MAX;
    uint16_t proof_fit_class = UHM_SIZE_CLASS_COUNT;
    uint32_t proof_nonempty_mask;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "ctx lowlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "ctx lowlock arena alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size,
                                    ctx_info->storage_alignment,
                                    &ctx_mem) != 0,
                "ctx lowlock ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "ctx lowlock usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "ctx lowlock heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "ctx lowlock api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    ctx_config.local_cache_bytes = 4u * page_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "ctx lowlock open");

    head_small = api->malloc_ctx(heap, ctx, 16u);
    head_preserved = api->malloc_ctx(heap, ctx, 32u);
    head_removed = api->malloc_ctx(heap, ctx, 64u);
    head_small_slot = test_slab_slot_from_payload(head_small);
    head_preserved_slot = test_slab_slot_from_payload(head_preserved);
    head_removed_slot = test_slab_slot_from_payload(head_removed);
    expect_true(head_small_slot != 0 && head_preserved_slot != 0 &&
                    head_removed_slot != 0 &&
                    head_small_slot->size_class <
                        head_preserved_slot->size_class &&
                    head_preserved_slot->size_class <
                        head_removed_slot->size_class,
                "ctx cache head repair uses ordered size classes");
    api->free_ctx(heap, ctx, head_small);
    api->free_ctx(heap, ctx, head_removed);
    api->free_ctx(heap, ctx, head_preserved);
    expect_true(ctx->local_cache_head == head_preserved_slot &&
                    ctx->nonempty_class_mask ==
                        ((UINT32_C(1) << head_small_slot->size_class) |
                         (UINT32_C(1) << head_preserved_slot->size_class) |
                         (UINT32_C(1) << head_removed_slot->size_class)),
                "ctx cache head tracks newest bucket before unlink");
    head_reuse = api->malloc_ctx(heap, ctx, 64u);
    expect_true(head_reuse == head_removed &&
                    ctx->local_cache_head == head_preserved_slot &&
                    (ctx->nonempty_class_mask &
                     (UINT32_C(1) << head_removed_slot->size_class)) == 0u,
                "ctx unrelated bucket pop preserves valid cache head");
    api->free_ctx(heap, ctx, head_reuse);
    expect_ok(api->thread_ctx_flush(heap, ctx),
              "ctx cache head repair setup flush");
    expect_true(ctx->local_cache_count == 0u &&
                    ctx->local_cache_head == 0 &&
                    ctx->nonempty_class_mask == 0u,
                "ctx cache head repair flush clears cache");
    expect_ok(api->check(heap, 0), "ctx cache head repair check");

    first = api->malloc_ctx(heap, ctx, 64u);
    second = api->malloc_ctx(heap, ctx, 64u);
    third = api->malloc_ctx(heap, ctx, 64u);
    expect_true(first != 0 && second != 0 && third != 0,
                "ctx lowlock seed allocations");
    first_slot = test_slab_slot_from_payload(first);
    second_slot = test_slab_slot_from_payload(second);
    slab = first_slot ? first_slot->slab : 0;
    expect_true(slab != 0 &&
                    second_slot != 0 &&
                    second_slot->slab == slab &&
                    test_slab_slot_from_payload(third)->slab == slab &&
                    slab->state == UHM_SLAB_STATE_PARTIAL,
                "ctx lowlock partial slab seeded");

    proof_hint_base = uhm_test_local_retained_hint_set_base(slab);
    proof_hint_outside =
        (proof_hint_base + UHM_LOCAL_RETAINED_HINT_WAYS) %
        UHM_LOCAL_RETAINED_HINT_COUNT;
    expect_true(proof_hint_base < UHM_LOCAL_RETAINED_HINT_COUNT &&
                    UHM_LOCAL_RETAINED_HINT_WAYS == 1u &&
                    proof_hint_outside < UHM_LOCAL_RETAINED_HINT_COUNT,
                "ctx retained hint uses one direct-mapped index");
    proof_hints[proof_hint_base].slab = slab;
    proof_hints[proof_hint_base].slot_count = 1u;
    proof_hints[proof_hint_outside].slab = slab;
    proof_hints[proof_hint_outside].slot_count = 4u;
    uhm_test_local_cache_note_slab_push(
        heap, proof_heads, proof_hints, &proof_retained_pages, slab,
        proof_hint_base);
    expect_true(proof_hints[proof_hint_base].slot_count == 2u &&
                    proof_hints[proof_hint_outside].slot_count == 4u,
                "ctx direct retained hint proof bypasses other indices");
    uhm_test_local_cache_note_slab_push(
        heap, proof_heads, proof_hints, &proof_retained_pages, slab,
        proof_hint_outside);
    expect_true(proof_hints[proof_hint_base].slot_count == 3u &&
                    proof_hints[proof_hint_outside].slot_count == 4u,
                "ctx stale retained proof falls back to direct index");

    memset(proof_collision_candidates, 0,
           sizeof(proof_collision_candidates));
    for (proof_candidate_index = 0;
         proof_candidate_index < UHM_LOCAL_RETAINED_HINT_COUNT * 2u;
         ++proof_candidate_index) {
        proof_collision_candidates[proof_candidate_index].size_class =
            slab->size_class;
        proof_collision_candidates[proof_candidate_index].size = slab->size;
        if (uhm_test_local_retained_hint_set_base(
                &proof_collision_candidates[proof_candidate_index]) ==
            proof_hint_base) {
            proof_collision_slab =
                &proof_collision_candidates[proof_candidate_index];
            break;
        }
    }
    proof_slab_pages =
        (slab->size + page_size - 1u) / page_size;
    proof_retained_pages_before = proof_retained_pages;
    uhm_test_local_cache_note_slab_push(
        heap, proof_heads, proof_hints, &proof_retained_pages,
        proof_collision_slab, UHM_LOCAL_RETAINED_HINT_COUNT);
    expect_true(proof_collision_slab != 0 && proof_slab_pages != 0u &&
                    proof_hints[proof_hint_base].slab == slab &&
                    proof_hints[proof_hint_base].slot_count == 3u &&
                    proof_retained_pages ==
                        proof_retained_pages_before + proof_slab_pages,
                "ctx direct retained hint collision stays accounted");
    memset(&proof_collision_slot, 0, sizeof(proof_collision_slot));
    proof_collision_slot.slab = proof_collision_slab;
    proof_heads[slab->size_class] = &proof_collision_slot;
    proof_retained_pages_before = proof_retained_pages;
    expect_true(UHMAS_LOCAL_RETAINED_PREPARED_PROOF == 1 &&
                    uhm_test_local_cache_prepare_and_note_slab_push(
                        heap, proof_heads, proof_hints,
                        &proof_retained_pages, SIZE_MAX,
                        proof_collision_slab,
                        &proof_existing_slot_count) != 0 &&
                    proof_existing_slot_count == 1u &&
                    proof_retained_pages == proof_retained_pages_before &&
                    proof_hints[proof_hint_base].slab == slab &&
                    proof_hints[proof_hint_base].slot_count == 3u,
                "ctx retained collision reuses prepared bucket scan");

    memset(proof_heads, 0, sizeof(proof_heads));
    proof_heads[head_preserved_slot->size_class] = head_preserved_slot;
    proof_heads[head_removed_slot->size_class] = head_removed_slot;
    proof_nonempty_mask =
        (UINT32_C(1) << head_preserved_slot->size_class) |
        (UINT32_C(1) << head_removed_slot->size_class);
    expect_true(uhm_test_local_cache_find_fit_class(
                    proof_nonempty_mask, head_small_slot->size_class,
                    &proof_fit_class) != 0 &&
                    proof_fit_class == head_preserved_slot->size_class,
                "ctx cache class mask returns lowest nonempty class");
    expect_true(!uhm_test_local_cache_find_fit_class(
                    0u, head_small_slot->size_class,
                    &proof_fit_class) &&
                    proof_fit_class == UHM_SIZE_CLASS_COUNT,
                "ctx cache empty class mask invalidates first-fit proof");

    api->free_ctx(heap, ctx, first);
    expect_true(ctx->local_cache_head == first_slot &&
                    ctx->local_cache_count == 1u &&
                    ctx->nonempty_class_mask ==
                        (UINT32_C(1) << first_slot->size_class),
                "ctx lowlock warmup creates nonempty cache");
    ctx->nonempty_class_mask = 0u;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ctx check catches missing nonempty class bit");
    ctx->nonempty_class_mask = UINT32_C(1) << first_slot->size_class;
    expect_ok(api->check(heap, 0), "ctx nonempty class mask restore check");
    expect_true(test_shadow_page_span(heap, (uintptr_t)second,
                                      sizeof(*second_slot), &cached_page,
                                      &cached_page_count) &&
                    cached_page_count == 1u,
                "ctx lowlock live slot page span");
    cached_record = test_page_record_at(heap, cached_page);
    expect_true(cached_record != 0,
                "ctx lowlock live slot page record");
    cached_prot = cached_record->prot;
    cached_record->prot = UHM_PROT_READ;
    api->free_ctx(heap, ctx, second);
    expect_true(ctx->local_cache_head == first_slot &&
                    ctx->local_cache_count == 1u &&
                    atomic_load_explicit(&ctx->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u &&
                    (second_slot->flags & UHM_SLAB_SLOT_FLAG_FREE) == 0u,
                "ctx lowlock rejected push releases reader and keeps slot live");
    cached_record->prot = cached_prot;

    lock_before = test_atomic_u64_load(&heap->lock.lock_calls);
    readers_before = uhm_metadata_registered_readers_load(heap);
    reader_seq_before = uhm_metadata_registered_reader_seq_load(heap);
    state_mirror_before = atomic_load_explicit(&slab->state_mirror,
                                               UHM_ATOMIC_ACQUIRE);
    free_head_ref_before = atomic_load_explicit(&slab->free_head_ref,
                                                UHM_ATOMIC_ACQUIRE);
    live_count_before = atomic_load_explicit(&slab->live_slots_count,
                                             UHM_ATOMIC_ACQUIRE);
    free_count_before = atomic_load_explicit(&slab->free_slots_count,
                                             UHM_ATOMIC_ACQUIRE);
    cached_count_before = atomic_load_explicit(&slab->ctx_cached_slots,
                                               UHM_ATOMIC_ACQUIRE);
    raw_lock_before = test_atomic_u64_load(&slab->raw_lock.lock_calls);
    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_calls_before =
        test_atomic_u64_load(&heap->core.page_lock_range_calls);
    page_lock_shards_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    api->free_ctx(heap, ctx, second);
    expect_true(test_atomic_u64_load(&heap->core.page_span_calls) ==
                    page_span_before,
                "ctx anchored cache push skips page owner probe");
    expect_true(test_atomic_u64_load(&heap->core.page_lock_range_calls) ==
                         page_lock_calls_before &&
                    test_atomic_u64_load(&heap->core.page_lock_range_shards) ==
                        page_lock_shards_before,
                "ctx anchored cache push skips page shard lock");
    expect_true(test_atomic_u64_load(&heap->lock.lock_calls) == lock_before,
                "ctx lowlock cache push avoids heap lock");
    expect_true(test_atomic_u64_load(&slab->raw_lock.lock_calls) ==
                    raw_lock_before,
                "ctx reservation push avoids slab raw lock");
    expect_true(uhm_metadata_registered_readers_load(heap) == readers_before,
                "ctx lowlock cache push avoids registered reader count");
    expect_true(uhm_metadata_registered_reader_seq_load(heap) ==
                    reader_seq_before,
                "ctx lowlock cache push avoids registered reader seq");
    expect_true(ctx->local_cache_head == second_slot &&
                    ctx->local_cache_count == 2u &&
                    atomic_load_explicit(&ctx->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "ctx lowlock push stores slot and releases reader");
    expect_true(atomic_load_explicit(&slab->state_mirror,
                                     UHM_ATOMIC_ACQUIRE) ==
                    state_mirror_before &&
                    state_mirror_before == UHM_SLAB_STATE_PARTIAL,
                "ctx lowlock push keeps state mirror unchanged");
    expect_true(atomic_load_explicit(&slab->free_head_ref,
                                     UHM_ATOMIC_ACQUIRE) ==
                    free_head_ref_before,
                "ctx lowlock push keeps free head mirror unchanged");
    expect_true(atomic_load_explicit(&slab->live_slots_count,
                                     UHM_ATOMIC_ACQUIRE) ==
                    live_count_before &&
                    atomic_load_explicit(&slab->live_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                        slab->live_slots,
                "ctx reservation push keeps live capacity");
    expect_true(atomic_load_explicit(&slab->free_slots_count,
                                     UHM_ATOMIC_ACQUIRE) ==
                    free_count_before &&
                    atomic_load_explicit(&slab->free_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                        slab->free_slots,
                "ctx reservation push keeps slab free capacity");
    expect_true(atomic_load_explicit(&slab->ctx_cached_slots,
                                     UHM_ATOMIC_ACQUIRE) ==
                    cached_count_before + 1u,
                "ctx lowlock push updates authoritative cached count");
    cached_hint_base = test_local_page_hint_set_base(cached_page);
    expect_true(ctx->local_page_hints[cached_hint_base].anchor ==
                        first_slot &&
                    ctx->local_page_hints[cached_hint_base].record ==
                        cached_record,
                "ctx empty cache push publishes page hint");
    expect_ok(api->check(heap, 0), "ctx lowlock check after push");
    ctx->local_page_hints[cached_hint_base].record = 0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "ctx page hint missing record check catches corruption");
    ctx->local_page_hints[cached_hint_base].record = cached_record;
    expect_ok(api->check(heap, 0), "ctx page hint restored check");

    expect_true(test_shadow_page_span(heap, (uintptr_t)second_slot,
                                      sizeof(*second_slot), &cached_page,
                                      &cached_page_count) &&
                    cached_page_count == 1u,
                "ctx lowlock cached slot page span");
    cached_record = test_page_record_at(heap, cached_page);
    expect_true(cached_record != 0,
                "ctx lowlock cached slot page record");
    cached_generation = cached_record->generation;
    cached_record->generation = cached_generation + 1u;
    drift_alloc = api->malloc_ctx(heap, ctx, 64u);
    expect_true(drift_alloc == second,
                "ctx private cache pop ignores diagnostic page drift");
    expect_true(ctx->local_cache_head == first_slot &&
                    ctx->local_cache_count == 1u &&
                    atomic_load_explicit(&ctx->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "ctx private cache pop avoids reader under page drift");
    expect_true(ctx->local_page_hints[
                        test_local_page_hint_set_base(cached_page)].anchor ==
                    first_slot &&
                    ctx->local_page_hints[
                        test_local_page_hint_set_base(cached_page)].record ==
                    cached_record,
                "ctx private cache pop transfers page hint anchor");
    cached_record->generation = cached_generation;
    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_calls_before =
        test_atomic_u64_load(&heap->core.page_lock_range_calls);
    page_lock_shards_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    reader_seq_before = uhm_metadata_registered_reader_seq_load(heap);
    api->free_ctx(heap, ctx, drift_alloc);
    expect_true(test_atomic_u64_load(&heap->core.page_span_calls) ==
                    page_span_before &&
                    test_atomic_u64_load(&heap->core.page_lock_range_calls) ==
                        page_lock_calls_before &&
                    test_atomic_u64_load(&heap->core.page_lock_range_shards) ==
                        page_lock_shards_before,
                "ctx anchored cache push skips page owner lock");
    expect_true(uhm_metadata_registered_reader_seq_load(heap) ==
                    reader_seq_before &&
                    atomic_load_explicit(&ctx->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "ctx anchored cache push skips metadata reader");
    expect_true(ctx->local_cache_head == second_slot &&
                    ctx->local_cache_count == 2u &&
                    ctx->local_page_hints[cached_hint_base].anchor ==
                        first_slot &&
                    ctx->local_page_hints[cached_hint_base].record ==
                        cached_record,
                "ctx repeated anchored push preserves older cache anchor");

    lock_before = test_atomic_u64_load(&heap->lock.lock_calls);
    readers_before = uhm_metadata_registered_readers_load(heap);
    reader_seq_before = uhm_metadata_registered_reader_seq_load(heap);

    state_mirror_before = atomic_load_explicit(&slab->state_mirror,
                                               UHM_ATOMIC_ACQUIRE);
    free_head_ref_before = atomic_load_explicit(&slab->free_head_ref,
                                                UHM_ATOMIC_ACQUIRE);
    live_count_before = atomic_load_explicit(&slab->live_slots_count,
                                             UHM_ATOMIC_ACQUIRE);
    free_count_before = atomic_load_explicit(&slab->free_slots_count,
                                             UHM_ATOMIC_ACQUIRE);
    cached_count_before = atomic_load_explicit(&slab->ctx_cached_slots,
                                               UHM_ATOMIC_ACQUIRE);
    raw_lock_before = test_atomic_u64_load(&slab->raw_lock.lock_calls);
    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_calls_before =
        test_atomic_u64_load(&heap->core.page_lock_range_calls);
    page_lock_shards_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    atomic_store_explicit(&slab->free_slots_count, 0u,
                          UHM_ATOMIC_RELEASE);
    reuse = api->malloc_ctx(heap, ctx, 64u);
    atomic_store_explicit(&slab->free_slots_count, free_count_before,
                          UHM_ATOMIC_RELEASE);
    expect_true(reuse == second, "ctx lowlock pop reuses expected slot");
    expect_true(test_atomic_u64_load(&heap->core.page_span_calls) ==
                    page_span_before,
                "ctx private cache pop skips page owner probe");
    expect_true(test_atomic_u64_load(&heap->core.page_lock_range_calls) ==
                        page_lock_calls_before &&
                    test_atomic_u64_load(&heap->core.page_lock_range_shards) ==
                        page_lock_shards_before,
                "ctx private cache pop skips page shard lock");
    expect_true(test_atomic_u64_load(&heap->lock.lock_calls) == lock_before,
                "ctx pop ignores unrelated free mirror without heap lock");
    expect_true(test_atomic_u64_load(&slab->raw_lock.lock_calls) ==
                    raw_lock_before,
                "ctx reservation pop avoids slab raw lock");
    expect_true(uhm_metadata_registered_readers_load(heap) == readers_before,
                "ctx lowlock cache pop avoids registered reader count");
    expect_true(uhm_metadata_registered_reader_seq_load(heap) ==
                    reader_seq_before,
                "ctx lowlock cache pop avoids registered reader seq");
    expect_true(atomic_load_explicit(&slab->state_mirror,
                                     UHM_ATOMIC_ACQUIRE) ==
                    state_mirror_before &&
                    state_mirror_before == UHM_SLAB_STATE_PARTIAL,
                "ctx lowlock pop keeps state mirror unchanged");
    expect_true(atomic_load_explicit(&slab->free_head_ref,
                                     UHM_ATOMIC_ACQUIRE) ==
                    free_head_ref_before,
                "ctx lowlock pop keeps free head mirror unchanged");
    expect_true(atomic_load_explicit(&slab->live_slots_count,
                                     UHM_ATOMIC_ACQUIRE) ==
                    live_count_before &&
                    atomic_load_explicit(&slab->live_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                        slab->live_slots,
                "ctx reservation pop keeps live capacity");
    expect_true(atomic_load_explicit(&slab->free_slots_count,
                                     UHM_ATOMIC_ACQUIRE) ==
                    free_count_before &&
                    atomic_load_explicit(&slab->free_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                        slab->free_slots,
                "ctx reservation pop keeps slab free capacity");
    expect_true(atomic_load_explicit(&slab->ctx_cached_slots,
                                     UHM_ATOMIC_ACQUIRE) ==
                    cached_count_before - 1u,
                "ctx lowlock pop updates authoritative cached count");
    expect_ok(api->check(heap, 0), "ctx lowlock check after pop");

    empty_cache_alloc = api->malloc_ctx(heap, ctx, 64u);
    expect_true(empty_cache_alloc == first && ctx->local_cache_count == 0u &&
                    ctx->local_cache_head == 0 &&
                    ctx->nonempty_class_mask == 0u,
                "ctx lowlock last cache slot pop reaches empty cache");
    lock_before = test_atomic_u64_load(&heap->lock.lock_calls);
    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_calls_before =
        test_atomic_u64_load(&heap->core.page_lock_range_calls);
    page_lock_shards_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    api->free_ctx(heap, ctx, empty_cache_alloc);
    expect_true(test_atomic_u64_load(&heap->core.page_span_calls) ==
                    page_span_before + 1u &&
                    test_atomic_u64_load(&heap->core.page_lock_range_calls) ==
                        page_lock_calls_before + 1u &&
                    test_atomic_u64_load(&heap->core.page_lock_range_shards) ==
                        page_lock_shards_before + 1u &&
                    test_atomic_u64_load(&heap->lock.lock_calls) == lock_before &&
                    ctx->local_cache_count == 1u &&
                    ctx->local_cache_head == first_slot &&
                    atomic_load_explicit(&ctx->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "ctx lowlock empty cache push stays outside heap lock");
    expect_ok(api->check(heap, 0), "ctx lowlock empty cache check");

    api->free_ctx(heap, ctx, reuse);
    api->free_ctx(heap, ctx, third);
    api->thread_ctx_close(heap, ctx);
    expect_ok(api->check(heap, 0), "ctx lowlock final check");
    expect_ok(api->close(heap), "ctx lowlock heap close");

    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_thread_ctx_deferred_free_flush(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *ctx = 0;
    const uhm_api *api;
    void *small;
    void *small_reuse;
    void *large;
    void *close_ptr;
    uhm_slab_slot *small_slot;
    uhm_slab_slot *close_slot;
    uhm_stats before;
    uhm_stats deferred_stats;
    uhm_stats flushed_stats;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "ctx deferred heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(2u * 1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "ctx deferred arena alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size,
                                    ctx_info->storage_alignment,
                                    &ctx_mem) != 0,
                "ctx deferred ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "ctx deferred usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "ctx deferred heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->thread_ctx_flush != 0 &&
                    api->free_deferred_ctx != 0,
                "ctx deferred api slots");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    ctx_config.local_cache_bytes = page_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "ctx deferred open");

    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before), "ctx deferred stats before");
    small = api->malloc(heap, 64u);
    large = api->malloc(heap, page_size * 3u);
    expect_true(small != 0 && large != 0, "ctx deferred allocs");
    small_slot = test_slab_slot_from_payload(small);
    test_fill_bytes(small, 64u, 0x37u);
    test_fill_bytes(large, 128u, 0x49u);

    expect_ok(api->free_deferred_ctx(heap, ctx, small),
              "ctx deferred small enqueue");
    expect_true(api->free_deferred_ctx(heap, ctx, small) ==
                    UHM_ERR_CORRUPTION,
                "ctx deferred duplicate rejected");
    expect_ok(api->free_deferred_ctx(heap, ctx, large),
              "ctx deferred large enqueue");
    expect_true(ctx->deferred_free_count == 2u,
                "ctx deferred queue count");
    expect_true(test_bytes_equal(small, 64u, 0x37u) &&
                    test_bytes_equal(large, 128u, 0x49u),
                "ctx deferred pointers remain live before flush");
    memset(&deferred_stats, 0, sizeof(deferred_stats));
    deferred_stats.size = sizeof(deferred_stats);
    expect_ok(api->get_stats(heap, &deferred_stats),
              "ctx deferred stats queued");
    expect_true(deferred_stats.malloc_live_bytes ==
                    before.malloc_live_bytes + 64u + page_size * 3u,
                "ctx deferred free keeps live stats until flush");
    expect_ok(api->check(heap, 0), "ctx deferred check queued");

    expect_ok(api->thread_ctx_flush(heap, ctx), "ctx deferred flush");
    expect_true(ctx->deferred_free_count == 0u,
                "ctx deferred queue empty after flush");
    memset(&flushed_stats, 0, sizeof(flushed_stats));
    flushed_stats.size = sizeof(flushed_stats);
    expect_ok(api->get_stats(heap, &flushed_stats),
              "ctx deferred stats flushed");
    expect_true(flushed_stats.malloc_live_bytes == before.malloc_live_bytes,
                "ctx deferred flush updates live stats");
    expect_ok(api->check(heap, 0), "ctx deferred check flushed");
    small_reuse = api->malloc(heap, 64u);
    expect_true(small_reuse == small,
                "ctx deferred flush returns slab slot to heap");
    api->free(heap, small_reuse);

    close_ptr = api->malloc(heap, 96u);
    expect_true(close_ptr != 0, "ctx deferred close alloc");
    close_slot = test_slab_slot_from_payload(close_ptr);
    expect_ok(api->free_deferred_ctx(heap, ctx, close_ptr),
              "ctx deferred close enqueue");
    expect_true(ctx->deferred_free_count == 1u,
                "ctx deferred close queue count");
    api->thread_ctx_close(heap, ctx);
    expect_true(ctx->magic == 0 && ctx->deferred_free_count == 0u,
                "ctx deferred close detaches and clears queue");
    expect_true((close_slot->flags &
                 (UHM_SLAB_SLOT_FLAG_CTX_CACHE |
                  UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) == 0,
                "ctx deferred close drains remote/cache flags");
    expect_true((close_slot->flags & UHM_SLAB_SLOT_FLAG_FREE) != 0,
                "ctx deferred close frees slot");
    expect_true((small_slot->flags & UHM_SLAB_SLOT_FLAG_FREE) != 0,
                "ctx deferred earlier slot remains free");

    expect_ok(api->check(heap, 0), "ctx deferred final check");
    expect_ok(api->close(heap), "ctx deferred heap close");

    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

