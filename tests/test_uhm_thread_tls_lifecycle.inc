static void test_slab_slot_atomic_state_mirror_check(
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
    uint32_t saved_flags;
    uint32_t saved_requested;
    uint64_t saved_state;
    uint64_t saved_raw_publish_seq;
    size_t saved_live_slots;
    size_t saved_free_slots;
    uintptr_t saved_free_head;
    uint32_t saved_slab_state;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "slab slot state heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "slab slot state arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "slab slot state usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "slab slot state heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "slab slot state api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "slab slot state allocation");
    slot = test_slab_slot_from_payload(ptr);
    expect_true(slot != 0 && slot->magic == UHM_SLAB_SLOT_MAGIC,
                "slab slot state slot lookup");
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "slab slot state slab lookup");
    saved_flags = slot->flags;
    saved_requested = slot->requested_size;
    saved_state = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    saved_raw_publish_seq =
        atomic_load_explicit(&slab->raw_publish_seq, UHM_ATOMIC_ACQUIRE);
    saved_live_slots = atomic_load_explicit(&slab->live_slots_count,
                                            UHM_ATOMIC_ACQUIRE);
    saved_free_slots = atomic_load_explicit(&slab->free_slots_count,
                                            UHM_ATOMIC_ACQUIRE);
    saved_free_head = atomic_load_explicit(&slab->free_head_ref,
                                           UHM_ATOMIC_ACQUIRE);
    saved_slab_state = atomic_load_explicit(&slab->state_mirror,
                                            UHM_ATOMIC_ACQUIRE);
    expect_ok(api->check(heap, 0), "slab slot state baseline check");

    slot->flags = saved_flags | UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab slot state catches raw mirror corruption");
    slot->flags = saved_flags;
    expect_ok(api->check(heap, 0), "slab slot state raw mirror restored");

    atomic_store_explicit(
        &slot->state_word,
        ((uint64_t)saved_requested << 32) |
            (uint64_t)(saved_flags | UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE),
        UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab slot state catches atomic mirror corruption");
    atomic_store_explicit(&slot->state_word, saved_state, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "slab slot state atomic mirror restored");

    atomic_store_explicit(&slab->live_slots_count, saved_live_slots + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab summary catches live mirror corruption");
    atomic_store_explicit(&slab->live_slots_count, saved_live_slots,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "slab summary live mirror restored");

    atomic_store_explicit(&slab->free_slots_count, saved_free_slots + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab summary catches free mirror corruption");
    atomic_store_explicit(&slab->free_slots_count, saved_free_slots,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "slab summary free mirror restored");

    atomic_store_explicit(&slab->state_mirror,
                          saved_slab_state == UHM_SLAB_STATE_FULL
                              ? UHM_SLAB_STATE_EMPTY
                              : UHM_SLAB_STATE_FULL,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab summary catches state mirror corruption");
    atomic_store_explicit(&slab->state_mirror, saved_slab_state,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "slab summary state mirror restored");

    atomic_store_explicit(&slab->free_head_ref,
                          saved_free_head ^ (uintptr_t)UHM_MIN_ALIGNMENT,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab summary catches free head mirror corruption");
    atomic_store_explicit(&slab->free_head_ref, saved_free_head,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "slab summary free head mirror restored");

    atomic_store_explicit(&slab->raw_publish_seq,
                          saved_raw_publish_seq | 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab raw publish without op pin rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "slab raw publish with op pin accepted");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->raw_publish_seq, saved_raw_publish_seq,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "slab raw publish restored");

    atomic_store_explicit(&slab->summary_seq, 1u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->live_slots_count, saved_live_slots + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "slab summary update without op pin rejected");
    atomic_store_explicit(&heap->active_ops, 1u, UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "slab summary update with op pin accepted");
    atomic_store_explicit(&heap->active_ops, 0u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->summary_seq, 0u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->live_slots_count, saved_live_slots,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0), "slab summary update window restored");

    api->free(heap, ptr);
    expect_ok(api->check(heap, 0), "slab slot state final check");
    expect_ok(api->close(heap), "slab slot state close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_thread_ctx_close_flushes_remote_queue(
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
    void *reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "ctx remote heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "ctx remote arena alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size, ctx_info->storage_alignment,
                                    &ctx_mem) != 0,
                "ctx remote ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "ctx remote usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "ctx remote heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "ctx remote api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx), "ctx remote open");

    first = api->malloc(heap, 64u);
    expect_true(first != 0, "ctx remote allocation");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "ctx remote slab metadata");

    api->free(heap, first);
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_queue_contains(slab, slot),
                "ctx remote queued before close");
    api->thread_ctx_close(heap, ctx);
    expect_true(test_slab_remote_queue_empty(slab),
                "ctx remote close drains queue");
    expect_true((slot->flags & UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) == 0,
                "ctx remote close clears remote flag");
    expect_true((slot->flags & UHM_SLAB_SLOT_FLAG_FREE) != 0,
                "ctx remote close leaves slot free");
    expect_ok(api->check(heap, 0), "ctx remote check after close");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == first, "ctx remote close drain enables reuse");
    api->free(heap, reuse);
    expect_ok(api->check(heap, 0), "ctx remote final check");
    expect_ok(api->close(heap), "ctx remote heap close");

    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_thread_ctx_flush_skips_lockless_class_queue(
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
    void *reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "ctx lockless class heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "ctx lockless class arena alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size,
                                    ctx_info->storage_alignment,
                                    &ctx_mem) != 0,
                "ctx lockless class ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "ctx lockless class usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "ctx lockless class heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "ctx lockless class api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "ctx lockless class open");

    first = api->malloc(heap, 64u);
    expect_true(first != 0, "ctx lockless class allocation");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "ctx lockless class slab metadata");

    api->free(heap, first);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "ctx lockless class queued before flush");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "ctx lockless class counters before flush");

    expect_ok(api->thread_ctx_flush(heap, ctx),
              "ctx lockless class explicit flush");
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "ctx lockless class explicit flush keeps class queue");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "ctx lockless class counters after explicit flush");
    expect_ok(api->check(heap, 0),
              "ctx lockless class check after explicit flush");

    api->thread_ctx_close(heap, ctx);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "ctx lockless class close keeps class queue");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "ctx lockless class counters after close");
    expect_ok(api->check(heap, 0), "ctx lockless class check after close");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == first,
                "ctx lockless class later lockless malloc reuses slot");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab, slot),
                "ctx lockless class later malloc consumes class queue");
    expect_ok(api->check(heap, 0), "ctx lockless class final check");
    api->free(heap, reuse);
    expect_ok(api->close(heap), "ctx lockless class heap close");

    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_tls_flush_skips_lockless_class_queue(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    lockless_tls_class_flush_args args;
    void *reuse;
    uhm_slab *slab;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "tls lockless class heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "tls lockless class arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "tls lockless class usermode vm init");

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
    config.thread_registry_slots = 2u;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 1u;

    expect_ok(uhm_open(&config, &heap), "tls lockless class heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "tls lockless class api");

    memset(&args, 0, sizeof(args));
    args.api = api;
    args.heap = heap;
#if defined(_WIN32)
    {
        HANDLE thread = CreateThread(0, 0, lockless_tls_class_flush_worker,
                                     &args, 0, 0);
        expect_true(thread != 0, "tls lockless class create thread");
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "tls lockless class wait thread");
        CloseHandle(thread);
    }
#else
    {
        pthread_t thread;
        expect_true(pthread_create(&thread, 0,
                                   lockless_tls_class_flush_worker,
                                   &args) == 0,
                    "tls lockless class create thread");
        expect_true(pthread_join(thread, 0) == 0,
                    "tls lockless class join thread");
    }
#endif
    expect_true(args.ok && args.remote_ptr != 0 && args.remote_slot != 0 &&
                    args.remote_count != 0,
                "tls lockless class worker queued remote slots");
    slab = args.remote_slot->slab;
    expect_true(heap->core.tls_cache_head == 0 &&
                    heap->core.tls_cache_active_count == 0u,
                "tls lockless class destructor detached cache");
    expect_true(test_slab_remote_class_queue_contains(heap, slab,
                                                      args.remote_slot),
                "tls lockless class destructor keeps class queue");
    expect_true(test_slab_remote_pending_mirror(slab) >= 1u &&
                    test_slab_remote_pending(slab) >= 1u &&
                    test_slab_remote_inflight_mirror(slab) >= 1u,
                "tls lockless class counters after destructor");
    expect_ok(api->check(heap, 0),
              "tls lockless class check after destructor");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == args.remote_ptr,
                "tls lockless class later lockless malloc reuses head");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab,
                                                       args.remote_slot),
                "tls lockless class later malloc consumes head");
    api->free(heap, reuse);
    expect_ok(api->check(heap, 0), "tls lockless class final check");
    expect_ok(api->close(heap), "tls lockless class heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_heap_close_invalidates_active_thread_ctx(
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
    void *ptr;
    void *deferred_ptr;
    uhm_slab_slot *slot;
    uhm_slab_slot *deferred_slot;
    uhm_slab *slab;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "close active ctx heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "close active ctx arena alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info->storage_size, ctx_info->storage_alignment,
                                    &ctx_mem) != 0,
                "close active ctx ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "close active ctx usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "close active ctx heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "close active ctx api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    ctx_config.local_cache_bytes = page_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx), "close active ctx open");

    ptr = api->malloc_ctx(heap, ctx, 64u);
    expect_true(ptr != 0, "close active ctx malloc");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot->slab;
    api->free_ctx(heap, ctx, ptr);
    expect_true(ctx->local_cache_count == 1u && ctx->local_cache_head == slot,
                "close active ctx cached before close");
    expect_true(slab->ctx_cached_slots == 1u, "close active ctx slab cached count");
    deferred_ptr = api->malloc(heap, 96u);
    expect_true(deferred_ptr != 0, "close active ctx deferred malloc");
    deferred_slot = test_slab_slot_from_payload(deferred_ptr);
    expect_ok(api->free_deferred_ctx(heap, ctx, deferred_ptr),
              "close active ctx deferred enqueue");
    expect_true(ctx->deferred_free_count == 1u,
                "close active ctx deferred count before close");

    expect_ok(api->close(heap), "close active ctx heap close");
    expect_true(uhm_get_api(heap) == 0, "close active ctx get api rejected");
    expect_true(api->malloc(heap, 16u) == 0,
                "close active ctx old api malloc rejected");
    expect_true(heap->core.thread_ctx_head == 0, "close active ctx list cleared");
    expect_true(ctx->magic == 0 && ctx->owner == 0 && ctx->owner_thread_id == 0,
                "close active ctx invalidated");
    expect_true(ctx->local_cache_head == 0 && ctx->local_cache_count == 0 &&
                    ctx->local_cache_bytes == 0 &&
                    ctx->deferred_free_count == 0,
                "close active ctx cache/deferred fields cleared");
    expect_true(slab->ctx_cached_slots == 0 &&
                    test_slab_remote_queue_empty(slab),
                "close active ctx slab queues flushed");
    expect_true((slot->flags & (UHM_SLAB_SLOT_FLAG_CTX_CACHE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) == 0,
                "close active ctx slot no longer queued");
    expect_true((slot->flags & UHM_SLAB_SLOT_FLAG_FREE) != 0,
                "close active ctx slot returned free");
    expect_true((deferred_slot->flags & (UHM_SLAB_SLOT_FLAG_CTX_CACHE |
                                         UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) == 0,
                "close active ctx deferred slot no longer queued");
    expect_true((deferred_slot->flags & UHM_SLAB_SLOT_FLAG_FREE) != 0,
                "close active ctx deferred slot returned free");

    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_heap_close_batches_thread_ctx_global_maintenance(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    enum { CONTEXT_COUNT = 9u };
    const size_t page_size = uhm_platform_system_page_size();
    const uint16_t size_class = 0u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem[CONTEXT_COUNT] = {{0}};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *contexts[CONTEXT_COUNT] = {0};
    const uhm_api *api;
    size_t state;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "close ctx batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    1024u * 1024u, page_size, &arena_mem) != 0,
                "close ctx batch arena alloc");
    for (i = 0u; i < CONTEXT_COUNT; ++i) {
        expect_true(test_vm_reserve_commit_alloc(
                        ctx_info->storage_size, ctx_info->storage_alignment,
                        &ctx_mem[i]) != 0,
                    "close ctx batch ctx storage alloc");
    }
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "close ctx batch usermode vm init");

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
    expect_ok(uhm_open(&config, &heap), "close ctx batch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "close ctx batch api");

    for (i = 0u; i < CONTEXT_COUNT; ++i) {
        memset(&ctx_config, 0, sizeof(ctx_config));
        ctx_config.size = sizeof(ctx_config);
        ctx_config.abi_major = UHM_ABI_MAJOR;
        ctx_config.abi_minor = UHM_ABI_MINOR;
        ctx_config.storage = ctx_mem[i].aligned;
        ctx_config.storage_size = ctx_info->storage_size;
        ctx_config.local_cache_bytes = page_size;
        expect_ok(api->thread_ctx_open(heap, &ctx_config, &contexts[i]),
                  "close ctx batch ctx open");
    }
    expect_true(heap->core.thread_ctx_head != 0,
                "close ctx batch active contexts");
    expect_true(heap->core.slab_committed_empty_count[size_class] == 0u,
                "close ctx batch no committed slabs");
    heap->core.slab_empty_committed_cache_high_water[size_class] =
        uhm_slab_empty_committed_cache_state_make(
            UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER, 0u);

    expect_ok(api->close(heap), "close ctx batch heap close");
    state = heap->core.slab_empty_committed_cache_high_water[size_class];
    expect_true(
        uhm_slab_empty_committed_cache_state_high_water(state) ==
                UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER &&
            uhm_slab_empty_committed_cache_state_idle_drains(state) == 1u,
        "close ctx batch advances global maintenance once");
    expect_true(heap->core.thread_ctx_head == 0,
                "close ctx batch detaches all contexts");
    for (i = 0u; i < CONTEXT_COUNT; ++i) {
        expect_true(contexts[i]->magic == 0 && contexts[i]->owner == 0,
                    "close ctx batch invalidates context");
        test_vm_release_alloc(&ctx_mem[i]);
    }
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_heap_close_batches_tls_and_ctx_global_maintenance(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    enum { CACHE_COUNT = 3u, CONTEXT_COUNT = 3u };
    const size_t page_size = uhm_platform_system_page_size();
    const uint16_t size_class = 0u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    test_alloc ctx_mem[CONTEXT_COUNT] = {{0}};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_thread_config ctx_config;
    uhm_heap *heap = 0;
    uhm_thread_ctx *contexts[CONTEXT_COUNT] = {0};
    const uhm_api *api;
    tls_churn_args args[CACHE_COUNT];
    _Atomic(uint32_t) ready_count;
    _Atomic(uint32_t) release;
    size_t state;
    size_t i;
#if defined(_WIN32)
    HANDLE threads[CACHE_COUNT];
#else
    pthread_t threads[CACHE_COUNT];
#endif

    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "close cache families heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    1024u * 1024u, page_size, &arena_mem) != 0,
                "close cache families arena alloc");
    for (i = 0u; i < CONTEXT_COUNT; ++i) {
        expect_true(test_vm_reserve_commit_alloc(
                        ctx_info->storage_size, ctx_info->storage_alignment,
                        &ctx_mem[i]) != 0,
                    "close cache families ctx storage alloc");
    }
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "close cache families usermode vm init");

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
    config.thread_registry_slots = CACHE_COUNT;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 1u;
    expect_ok(uhm_open(&config, &heap), "close cache families heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "close cache families api");

    for (i = 0u; i < CONTEXT_COUNT; ++i) {
        memset(&ctx_config, 0, sizeof(ctx_config));
        ctx_config.size = sizeof(ctx_config);
        ctx_config.abi_major = UHM_ABI_MAJOR;
        ctx_config.abi_minor = UHM_ABI_MINOR;
        ctx_config.storage = ctx_mem[i].aligned;
        ctx_config.storage_size = ctx_info->storage_size;
        ctx_config.local_cache_bytes = page_size;
        expect_ok(api->thread_ctx_open(heap, &ctx_config, &contexts[i]),
                  "close cache families ctx open");
    }

    memset(args, 0, sizeof(args));
    atomic_init(&ready_count, 0u);
    atomic_init(&release, 0u);
    for (i = 0u; i < CACHE_COUNT; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].id = (unsigned)i;
        args[i].iterations = 1u;
        args[i].ready_count = &ready_count;
        args[i].release = &release;
#if defined(_WIN32)
        threads[i] = CreateThread(0, 0, tls_churn_worker, &args[i], 0, 0);
        expect_true(threads[i] != 0,
                    "close cache families create thread");
#else
        expect_true(pthread_create(&threads[i], 0, tls_churn_worker,
                                   &args[i]) == 0,
                    "close cache families create thread");
#endif
    }
    while (atomic_load_explicit(&ready_count, UHM_ATOMIC_ACQUIRE) !=
           CACHE_COUNT) {
        test_thread_pause();
    }
    expect_true(heap->core.tls_cache_active_count == CACHE_COUNT &&
                    heap->core.thread_ctx_head != 0,
                "close cache families both registries active");
    heap->core.slab_empty_committed_cache_high_water[size_class] =
        uhm_slab_empty_committed_cache_state_make(
            UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER, 0u);

    expect_ok(api->close(heap), "close cache families heap close");
    state = heap->core.slab_empty_committed_cache_high_water[size_class];
    expect_true(
        uhm_slab_empty_committed_cache_state_high_water(state) ==
                UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER &&
            uhm_slab_empty_committed_cache_state_idle_drains(state) == 1u,
        "close cache families advance global maintenance once");
    expect_true(heap->core.tls_cache_active_count == 0u &&
                    heap->core.tls_cache_head == 0 &&
                    heap->core.thread_ctx_head == 0,
                "close cache families detach both registries");

    atomic_store_explicit(&release, 1u, UHM_ATOMIC_RELEASE);
    for (i = 0u; i < CACHE_COUNT; ++i) {
#if defined(_WIN32)
        expect_true(WaitForSingleObject(threads[i], INFINITE) ==
                        WAIT_OBJECT_0,
                    "close cache families wait thread");
        CloseHandle(threads[i]);
#else
        expect_true(pthread_join(threads[i], 0) == 0,
                    "close cache families join thread");
#endif
        expect_true(args[i].ok, "close cache families worker completed");
    }
    for (i = 0u; i < CONTEXT_COUNT; ++i) {
        expect_true(contexts[i]->magic == 0 && contexts[i]->owner == 0,
                    "close cache families invalidates context");
        test_vm_release_alloc(&ctx_mem[i]);
    }
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_heap_close_class_queue_retry_after_cache_maintenance(
    const uhm_storage_info *heap_info,
    const uhm_storage_info *ctx_info) {
    enum { POINTER_COUNT = 96u };
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
    tls_hold_args tls_args;
    void *ctx_ptr;
    void *fallback_ptrs[POINTER_COUNT] = {0};
    void *class_ptrs[POINTER_COUNT] = {0};
    uhm_slab_slot *fallback_slot = 0;
    uhm_slab_slot *class_slot = 0;
    uhm_slab *fallback_slab = 0;
    uhm_slab *class_slab = 0;
    uint64_t vm_decommit_calls_before;
    size_t state;
    size_t i;
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif

    if (!test_lockless_remote_primitives_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "close class retry heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    1024u * 1024u, page_size, &arena_mem) != 0,
                "close class retry arena alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    ctx_info->storage_size, ctx_info->storage_alignment,
                    &ctx_mem) != 0,
                "close class retry ctx storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "close class retry usermode vm init");

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
    config.thread_registry_slots = 4u;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 1u;
    expect_ok(uhm_open(&config, &heap), "close class retry heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "close class retry api");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info->storage_size;
    ctx_config.local_cache_bytes = page_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx),
              "close class retry ctx open");
    ctx_ptr = api->malloc_ctx(heap, ctx, 64u);
    expect_true(ctx_ptr != 0, "close class retry ctx allocation");
    api->free_ctx(heap, ctx, ctx_ptr);
    expect_true(ctx->local_cache_count != 0u,
                "close class retry ctx cache active");

    memset(&tls_args, 0, sizeof(tls_args));
    tls_args.api = api;
    tls_args.heap = heap;
    atomic_init(&tls_args.ready, 0u);
    atomic_init(&tls_args.release, 0u);
#if defined(_WIN32)
    thread = CreateThread(0, 0, tls_hold_worker, &tls_args, 0, 0);
    expect_true(thread != 0, "close class retry create TLS thread");
#else
    expect_true(pthread_create(&thread, 0, tls_hold_worker, &tls_args) == 0,
                "close class retry create TLS thread");
#endif
    while (atomic_load_explicit(&tls_args.ready, UHM_ATOMIC_ACQUIRE) == 0u) {
        test_thread_pause();
    }
    expect_true(tls_args.ok && tls_args.ptr != 0 && tls_args.slot != 0,
                "close class retry TLS cache active");

    for (i = 0u; i < POINTER_COUNT; ++i) {
        fallback_ptrs[i] = api->malloc(heap, 64u);
        expect_true(fallback_ptrs[i] != 0,
                    "close class retry fallback allocation");
    }
    heap->core.fault_lockless_remote_publish_countdown = 1u;
    for (i = 0u; i < POINTER_COUNT; ++i) {
        uhm_slab_slot *slot = test_slab_slot_from_payload(fallback_ptrs[i]);

        expect_true(slot != 0 && slot->slab != 0,
                    "close class retry fallback slot");
        api->free(heap, fallback_ptrs[i]);
        if (heap->core.fault_lockless_remote_publish_countdown == 0u &&
            test_slab_remote_queue_contains(slot->slab, slot)) {
            fallback_slot = slot;
            fallback_slab = slot->slab;
            break;
        }
    }
    for (i = 0u; i < POINTER_COUNT; ++i) {
        class_ptrs[i] = api->malloc(heap, 512u);
        expect_true(class_ptrs[i] != 0,
                    "close class retry class allocation");
    }
    for (i = 0u; i < POINTER_COUNT; ++i) {
        uhm_slab_slot *slot = test_slab_slot_from_payload(class_ptrs[i]);

        expect_true(slot != 0 && slot->slab != 0,
                    "close class retry class slot");
        api->free(heap, class_ptrs[i]);
        if (test_slab_remote_class_queue_contains(heap, slot->slab, slot)) {
            class_slot = slot;
            class_slab = slot->slab;
            break;
        }
    }
    expect_true(fallback_slot != 0 && fallback_slab != 0 &&
                    test_slab_remote_queue_contains(fallback_slab,
                                                    fallback_slot),
                "close class retry fallback remote queued");
    expect_true(class_slot != 0 && class_slab != 0 &&
                    test_slab_remote_class_queue_contains(heap, class_slab,
                                                          class_slot),
                "close class retry class remote queued");
    expect_true(fallback_slot->size_class != class_slot->size_class,
                "close class retry queue classes isolated");
    expect_true(heap->core.tls_cache_active_count >= 2u &&
                    heap->core.thread_ctx_head == ctx,
                "close class retry both cache registries active");

    for (i = 0u; i < UHM_SIZE_CLASS_COUNT; ++i) {
        heap->core.slab_empty_committed_cache_high_water[i] =
            uhm_slab_empty_committed_cache_state_make(
                UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER, 0u);
    }
    vm_decommit_calls_before = heap->core.vm_decommit_calls;
    heap->core.fault_lockless_remote_drain_countdown = 1u;
    expect_true(api->close(heap) == UHM_ERR_BUSY,
                "close class retry first close busy");
    expect_true(heap->core.fault_lockless_remote_drain_countdown == 0u,
                "close class retry drain fault consumed");
    expect_true(test_slab_remote_queue_empty(fallback_slab),
                "close class retry fallback maintenance completed");
    expect_true(test_slab_remote_class_queue_contains(heap, class_slab,
                                                      class_slot),
                "close class retry final class queue restored");
    expect_true(heap->core.tls_cache_active_count >= 2u &&
                    heap->core.thread_ctx_head == ctx &&
                    ctx->magic == UHM_THREAD_MAGIC && ctx->owner == heap,
                "close class retry failed close keeps registries attached");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before,
                "close class retry failed close adds no decommit");
    state = heap->core.slab_empty_committed_cache_high_water[
        fallback_slot->size_class];
    expect_true(uhm_slab_empty_committed_cache_state_idle_drains(state) == 1u,
                "close class retry failed close runs one maintenance pass");
    expect_ok(api->check(heap, 0), "close class retry failed close check");

    expect_ok(api->close(heap), "close class retry second close");
    expect_true(!test_slab_remote_class_queue_contains(heap, class_slab,
                                                       class_slot) &&
                    test_slab_remote_pending_mirror(class_slab) == 0u &&
                    test_slab_remote_pending(class_slab) == 0u &&
                    test_slab_remote_inflight_mirror(class_slab) == 0u,
                "close class retry second close drains class queue");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before,
                "close class retry final publish adds no decommit");
    state = heap->core.slab_empty_committed_cache_high_water[
        fallback_slot->size_class];
    expect_true(uhm_slab_empty_committed_cache_state_idle_drains(state) == 2u,
                "close class retry each close attempt maintains once");
    expect_true(heap->core.tls_cache_active_count == 0u &&
                    heap->core.tls_cache_head == 0 &&
                    heap->core.thread_ctx_head == 0 &&
                    uhm_metadata_registered_readers_load(heap) == 0u,
                "close class retry success detaches without reader leak");
    expect_true(ctx->magic == 0 && ctx->owner == 0,
                "close class retry success invalidates context");

    atomic_store_explicit(&tls_args.release, 1u, UHM_ATOMIC_RELEASE);
#if defined(_WIN32)
    expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                "close class retry wait TLS thread");
    CloseHandle(thread);
#else
    expect_true(pthread_join(thread, 0) == 0,
                "close class retry join TLS thread");
#endif
    expect_true(tls_args.ok, "close class retry TLS worker completed");

    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_heap_close_rejects_new_ops_during_active_mmap(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    close_during_mmap_vm_state state;
    close_during_mmap_map_args map_args;
    close_during_mmap_close_args close_args;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.block_commit, 0u);
    atomic_init(&state.commit_entered, 0u);
    atomic_init(&state.release_commit, 0u);
    memset(&map_args, 0, sizeof(map_args));
    memset(&close_args, 0, sizeof(close_args));
    atomic_init(&close_args.started, 0u);
    atomic_init(&close_args.done, 0u);

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "close active mmap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "close active mmap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "close active mmap usermode vm init");
    vm.user = &state;
    vm.commit = close_during_mmap_vm_commit;

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

    expect_ok(uhm_open(&config, &heap), "close active mmap heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "close active mmap api");

    atomic_store_explicit(&state.block_commit, 1u, UHM_ATOMIC_RELEASE);
    map_args.api = api;
    map_args.heap = heap;
    map_args.map_size = 2u * page_size;
    close_args.api = api;
    close_args.heap = heap;
    close_args.result = UHM_ERR_CORRUPTION;

#if defined(_WIN32)
    {
        HANDLE map_thread;
        HANDLE close_thread;
        map_thread = CreateThread(0, 0, close_during_mmap_map_worker,
                                  &map_args, 0, 0);
        expect_true(map_thread != 0, "close active mmap map thread create");
        expect_true(test_wait_atomic_u32(&state.commit_entered, 1u),
                    "close active mmap commit entered");
        close_thread = CreateThread(0, 0, close_during_mmap_close_worker,
                                    &close_args, 0, 0);
        expect_true(close_thread != 0, "close active mmap close thread create");
        expect_true(test_wait_atomic_u32(&close_args.started, 1u),
                    "close active mmap close started");
        expect_true(test_wait_heap_lifecycle(heap, UHM_CLOSING_MAGIC),
                    "close active mmap lifecycle closing");
        expect_true(
            uhm_heap_active_ops_load(heap) == 1u,
            "close active mmap active op pinned");
        expect_true(
            atomic_load_explicit(&close_args.done, UHM_ATOMIC_ACQUIRE) == 0u,
            "close active mmap close waits");
        expect_true(uhm_get_api(heap) == 0,
                    "close active mmap get api rejected while closing");
        expect_true(api->malloc(heap, 16u) == 0,
                    "close active mmap old api malloc rejected while closing");
        atomic_store_explicit(&state.release_commit, 1u, UHM_ATOMIC_RELEASE);
        expect_true(WaitForSingleObject(map_thread, INFINITE) == WAIT_OBJECT_0,
                    "close active mmap map wait");
        expect_true(WaitForSingleObject(close_thread, INFINITE) == WAIT_OBJECT_0,
                    "close active mmap close wait");
        CloseHandle(map_thread);
        CloseHandle(close_thread);
    }
#else
    {
        pthread_t map_thread;
        pthread_t close_thread;
        expect_true(pthread_create(&map_thread, 0,
                                   close_during_mmap_map_worker,
                                   &map_args) == 0,
                    "close active mmap map thread create");
        expect_true(test_wait_atomic_u32(&state.commit_entered, 1u),
                    "close active mmap commit entered");
        expect_true(pthread_create(&close_thread, 0,
                                   close_during_mmap_close_worker,
                                   &close_args) == 0,
                    "close active mmap close thread create");
        expect_true(test_wait_atomic_u32(&close_args.started, 1u),
                    "close active mmap close started");
        expect_true(test_wait_heap_lifecycle(heap, UHM_CLOSING_MAGIC),
                    "close active mmap lifecycle closing");
        expect_true(
            uhm_heap_active_ops_load(heap) == 1u,
            "close active mmap active op pinned");
        expect_true(
            atomic_load_explicit(&close_args.done, UHM_ATOMIC_ACQUIRE) == 0u,
            "close active mmap close waits");
        expect_true(uhm_get_api(heap) == 0,
                    "close active mmap get api rejected while closing");
        expect_true(api->malloc(heap, 16u) == 0,
                    "close active mmap old api malloc rejected while closing");
        atomic_store_explicit(&state.release_commit, 1u, UHM_ATOMIC_RELEASE);
        expect_true(pthread_join(map_thread, 0) == 0,
                    "close active mmap map join");
        expect_true(pthread_join(close_thread, 0) == 0,
                    "close active mmap close join");
    }
#endif

    expect_true(map_args.result == UHM_OK && map_args.addr != 0,
                "close active mmap operation completed");
    expect_true(close_args.result == UHM_OK,
                "close active mmap close completed");
    expect_true(
        atomic_load_explicit(&close_args.done, UHM_ATOMIC_ACQUIRE) == 1u,
        "close active mmap close done");
    expect_true(
        atomic_load_explicit(&heap->lifecycle_state, UHM_ATOMIC_ACQUIRE) ==
            UHM_CLOSED_MAGIC,
        "close active mmap lifecycle closed");
    expect_true(uhm_get_api(heap) == 0, "close active mmap get api rejected");
    expect_true(api->malloc(heap, 16u) == 0,
                "close active mmap old api malloc rejected");
    expect_true(state.last_commit_size == map_args.map_size,
                "close active mmap blocked expected commit");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

