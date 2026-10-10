static void test_slab_lockless_remote_scan_mismatch(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *want;
    void *blocker;
    void *want_reuse;
    void *blocker_reuse;
    uhm_slab_slot *want_slot;
    uhm_slab_slot *blocker_slot;
    uhm_slab *want_slab;
    uhm_slab *blocker_slab;
    uhm_stats stats;
    uint64_t blocker_state;
    uint32_t blocker_flags;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless remote scan heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless remote scan arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless remote scan usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "lockless remote scan heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless remote scan api");

    want = api->malloc(heap, 64u);
    blocker = api->malloc(heap, 63u);
    expect_true(want != 0 && blocker != 0 && want != blocker,
                "lockless remote scan allocations");
    want_slot = test_slab_slot_from_payload(want);
    blocker_slot = test_slab_slot_from_payload(blocker);
    want_slab = want_slot->slab;
    blocker_slab = blocker_slot->slab;
    expect_true(want_slab != 0 && blocker_slab != 0,
                "lockless remote scan slab metadata");

    api->free(heap, want);
    api->free(heap, blocker);
    expect_true(test_slab_remote_class_queue_contains(heap, want_slab,
                                                      want_slot),
                "lockless remote scan wanted slot queued");
    expect_true(test_slab_remote_class_queue_contains(heap, blocker_slab,
                                                      blocker_slot),
                "lockless remote scan blocker slot queued");
    expect_ok(api->check(heap, 0), "lockless remote scan queued check");

    blocker_reuse = api->malloc(heap, 64u);
    expect_true(blocker_reuse == blocker,
                "lockless remote scan reuses head mismatch lockless");
    expect_true(test_slab_remote_class_queue_contains(heap, want_slab,
                                                      want_slot),
                "lockless remote scan leaves older slot queued");
    expect_true(!test_slab_remote_class_queue_contains(heap, blocker_slab,
                                                       blocker_slot),
                "lockless remote scan removes head mismatch slot");
    blocker_state =
        atomic_load_explicit(&blocker_slot->state_word, UHM_ATOMIC_ACQUIRE);
    blocker_flags = (uint32_t)(blocker_state & UINT64_C(0xffffffff));
    expect_true(blocker_flags == UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED &&
                    (uint32_t)(blocker_state >> 32) == 64u,
                "lockless remote scan blocker live deferred");
    expect_true(blocker_slot->flags == 0 &&
                    blocker_slot->requested_size == 63u,
                "lockless remote scan blocker raw requested deferred");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "lockless remote scan stats after reuse");
    expect_true(stats.malloc_live_bytes == 64u,
                "lockless remote scan stats only head reuse live");
    expect_ok(api->check(heap, 0), "lockless remote scan post reuse check");

    want_reuse = api->malloc(heap, 64u);
    expect_true(want_reuse == want,
                "lockless remote scan reuses remaining slot");
    expect_true(!test_slab_remote_class_queue_contains(heap, want_slab,
                                                       want_slot),
                "lockless remote scan wanted slot popped");
    expect_ok(api->check(heap, 0), "lockless remote scan final check");
    api->free(heap, want_reuse);
    api->free(heap, blocker_reuse);
    expect_ok(api->close(heap), "lockless remote scan close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_remote_tag_wrap_reader_gate(
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
    void *hazard_fallback;
    void *reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_tagged_freelist *queue;
    uint64_t deferrals_before;
    uint64_t overflows_before;
    uint64_t valid_wrap_epoch;

    if (!test_lockless_remote_primitives_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless tag wrap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless tag wrap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless tag wrap usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "lockless tag wrap heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless tag wrap api");

    first = api->malloc(heap, 64u);
    expect_true(first != 0, "lockless tag wrap allocation");
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless tag wrap slab metadata");
    queue = &heap->core.slab_remote_class_lf[slab->size_class].queue;

    api->free(heap, first);
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless tag wrap class queued");
    atomic_store_explicit(
        &queue->head,
        uhm_tagged_freelist_pack(&slot->remote_link,
                                 UHM_TAGGED_FREELIST_TAG_MASK),
        UHM_ATOMIC_RELEASE);
    deferrals_before = uhm_tagged_freelist_tag_wrap_deferrals(queue);
    overflows_before = uhm_tagged_freelist_tag_overflows(queue);

    expect_true(uhm_metadata_reader_begin(
                    &heap->core.lockless_remote_readers[0], heap),
                "lockless tag wrap external reader begin");
    fallback = api->malloc(heap, 64u);
    expect_true(fallback != 0 && fallback != first,
                "lockless tag wrap active reader uses fallback allocation");
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless tag wrap active reader keeps class queue");
    expect_true(uhm_tagged_freelist_tag_wrap_deferrals(queue) >
                    deferrals_before,
                "lockless tag wrap deferral counted");
    expect_true(uhm_tagged_freelist_tag_overflows(queue) == overflows_before,
                "lockless tag wrap does not wrap under external reader");
    expect_ok(api->check(heap, 0),
              "lockless tag wrap active reader check");
    expect_true(uhm_metadata_reader_end(
                    &heap->core.lockless_remote_readers[0]),
                "lockless tag wrap external reader end");

    deferrals_before = uhm_tagged_freelist_tag_wrap_deferrals(queue);
    expect_true(uhm_metadata_hazard_begin(heap),
                "lockless tag wrap conservative reader begin");
    hazard_fallback = api->malloc(heap, 64u);
    expect_true(hazard_fallback != 0 && hazard_fallback != first,
                "lockless tag wrap conservative reader uses fallback");
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless tag wrap conservative reader keeps class queue");
    expect_true(uhm_tagged_freelist_tag_wrap_deferrals(queue) >
                    deferrals_before,
                "lockless tag wrap conservative deferral counted");
    expect_ok(api->check(heap, 0),
              "lockless tag wrap conservative reader check");
    expect_true(uhm_metadata_hazard_end(heap),
                "lockless tag wrap conservative reader end");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == first,
                "lockless tag wrap reuses after reader quiesces");
    expect_true(uhm_tagged_freelist_tag_overflows(queue) >
                    overflows_before,
                "lockless tag wrap overflow after quiescence");
    expect_true(uhm_tagged_freelist_last_wrap_epoch(queue) != 0,
                "lockless tag wrap records metadata epoch");
    expect_ok(api->check(heap, 0),
              "lockless tag wrap final check before free");
    valid_wrap_epoch = uhm_tagged_freelist_last_wrap_epoch(queue);
    atomic_store_explicit(&queue->last_wrap_epoch,
                          heap->core.metadata_epoch + 1u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "lockless tag wrap future epoch rejected");
    atomic_store_explicit(&queue->last_wrap_epoch, 0u,
                          UHM_ATOMIC_RELEASE);
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "lockless tag wrap missing epoch rejected");
    atomic_store_explicit(&queue->last_wrap_epoch, valid_wrap_epoch,
                          UHM_ATOMIC_RELEASE);
    expect_ok(api->check(heap, 0),
              "lockless tag wrap epoch restored");

    api->free(heap, reuse);
    api->free(heap, fallback);
    api->free(heap, hazard_fallback);
    expect_ok(api->close(heap), "lockless tag wrap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_remote_hazard_gate(
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
    void *queued;
    void *fallback_alloc;
    void *lockless_reuse;
    uhm_slab_slot *slot;
    uhm_slab_slot *queued_slot;
    uhm_slab *slab;
    uint64_t state_word;
    uint32_t state_flags;
    size_t i;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless remote hazard heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless remote hazard arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless remote hazard usermode vm init");

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

    expect_ok(uhm_open(&config, &heap), "lockless remote hazard heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless remote hazard api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless remote hazard allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot->slab;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless remote hazard slab metadata");

    atomic_store_explicit(&heap->metadata_hazard_readers, SIZE_MAX,
                          UHM_ATOMIC_RELEASE);
    api->free(heap, ptr);
    atomic_store_explicit(&heap->metadata_hazard_readers, 0u,
                          UHM_ATOMIC_RELEASE);

    state_word = atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
    state_flags = (uint32_t)(state_word & UINT64_C(0xffffffff));
    expect_true((state_flags & (UHM_SLAB_SLOT_FLAG_FREE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                                UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) ==
                    (UHM_SLAB_SLOT_FLAG_FREE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                     UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED),
                "lockless remote hazard fallback unaccounted state");
    expect_true(slot->flags == 0 && slot->requested_size == 64u,
                "lockless remote hazard fallback keeps raw mirror live");
    expect_true(test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless remote hazard fallback uses class queue");
    expect_true(!test_slab_remote_queue_contains(slab, slot),
                "lockless remote hazard fallback skips slab queue");
    expect_true(uhm_metadata_hazard_readers_load(heap) == 0,
                "lockless remote hazard reader count restored");
    expect_ok(api->check(heap, 0), "lockless remote hazard check");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == ptr,
                "lockless remote hazard fallback lockless reuse");
    expect_ok(api->check(heap, 0), "lockless remote hazard final check");
    api->free(heap, reuse);

    queued = api->malloc(heap, 64u);
    expect_true(queued != 0, "lockless remote reader drain seed");
    queued_slot = test_slab_slot_from_payload(queued);
    expect_true(queued_slot != 0 && queued_slot->slab != 0,
                "lockless remote reader drain seed metadata");
    api->free(heap, queued);
    expect_true(test_slab_remote_class_queue_contains(
                    heap, queued_slot->slab, queued_slot),
                "lockless remote reader drain seed queued");
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        expect_true(uhm_metadata_reader_begin(
                        &heap->core.lockless_remote_readers[i], heap),
                    "lockless remote reader exhaustion begin");
    }
    fallback_alloc = api->malloc(heap, 64u);
    expect_true(fallback_alloc != 0,
                "lockless remote reader exhaustion fallback alloc succeeds");
    expect_true(fallback_alloc != queued,
                "lockless remote reader exhaustion skips locked class drain");
    expect_true(test_slab_remote_class_queue_contains(
                    heap, queued_slot->slab, queued_slot),
                "lockless remote reader exhaustion keeps class queue");
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        size_t hazard_index;
        expect_true(atomic_load_explicit(
                        &heap->core.lockless_remote_readers[i].depth,
                        UHM_ATOMIC_ACQUIRE) == 1u,
                    "lockless remote reader exhaustion no nested acquire");
        expect_true(atomic_load_explicit(
                        &heap->core.lockless_remote_readers[i].epoch,
                        UHM_ATOMIC_ACQUIRE) != 0,
                    "lockless remote reader exhaustion epoch retained");
        for (hazard_index = 0; hazard_index < UHM_METADATA_HAZARD_SLOTS;
             ++hazard_index) {
            expect_true(atomic_load_explicit(
                            &heap->core.lockless_remote_readers[i]
                                 .hazards[hazard_index],
                            UHM_ATOMIC_ACQUIRE) == (uintptr_t)0,
                        "lockless remote reader exhaustion hazard untouched");
        }
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        expect_true(uhm_metadata_reader_end(
                        &heap->core.lockless_remote_readers[i]),
                    "lockless remote reader exhaustion end");
    }
    expect_ok(api->check(heap, 0),
              "lockless remote reader exhaustion check");

    lockless_reuse = api->malloc(heap, 64u);
    expect_true(lockless_reuse == queued,
                "lockless remote reader exhaustion later lockless reuse");
    expect_true(!test_slab_remote_class_queue_contains(
                    heap, queued_slot->slab, queued_slot),
                "lockless remote reader exhaustion class queue consumed later");
    api->free(heap, fallback_alloc);
    api->free(heap, lockless_reuse);
    expect_ok(api->close(heap), "lockless remote hazard close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_slab_lockless_remote_class_publish_wrap_busy_fallback(
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
    uhm_tagged_freelist *class_queue;
    uint64_t deferrals_before;
    uint32_t state_flags;

    if (!test_lockless_remote_primitives_supported()) {
        return;
    }

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "lockless class publish busy heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "lockless class publish busy arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "lockless class publish busy usermode vm init");

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
              "lockless class publish busy heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "lockless class publish busy api");

    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "lockless class publish busy allocation");
    slot = test_slab_slot_from_payload(ptr);
    slab = slot ? slot->slab : 0;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "lockless class publish busy slab metadata");
    class_queue = &heap->core.slab_remote_class_lf[slab->size_class].queue;

    atomic_store_explicit(
        &class_queue->head,
        uhm_tagged_freelist_pack(0, UHM_TAGGED_FREELIST_TAG_MASK),
        UHM_ATOMIC_RELEASE);
    deferrals_before = uhm_tagged_freelist_tag_wrap_deferrals(class_queue);
    expect_true(uhm_metadata_reader_begin(
                    &heap->core.lockless_remote_readers[0], heap),
                "lockless class publish busy external reader begin");

    api->free(heap, ptr);
    expect_true(uhm_tagged_freelist_tag_wrap_deferrals(class_queue) >
                    deferrals_before,
                "lockless class publish busy deferral counted");
    expect_true(!test_slab_remote_class_queue_contains(heap, slab, slot),
                "lockless class publish busy skips class queue");
    expect_true(test_slab_remote_queue_contains(slab, slot),
                "lockless class publish busy falls back to slab queue");
    state_flags = test_slab_slot_state_flags_load(slot);
    expect_true((state_flags & (UHM_SLAB_SLOT_FLAG_FREE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                                UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING)) ==
                    (UHM_SLAB_SLOT_FLAG_FREE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING),
                "lockless class publish busy accounted state");
    expect_true(test_slab_remote_pending_mirror(slab) == 1u &&
                    test_slab_remote_pending(slab) == 1u &&
                    test_slab_remote_inflight_mirror(slab) == 1u,
                "lockless class publish busy counters retained");
    expect_last_error(api, heap, UHM_OK, 0u,
                      "lockless class publish busy last error clean");
    expect_ok(api->check(heap, 0), "lockless class publish busy active check");
    expect_true(uhm_metadata_reader_end(
                    &heap->core.lockless_remote_readers[0]),
                "lockless class publish busy external reader end");
    expect_true(uhm_metadata_registered_readers_load(heap) == 0,
                "lockless class publish busy reader released");

    reuse = api->malloc(heap, 64u);
    expect_true(reuse == ptr,
                "lockless class publish busy accounted reuse");
    expect_true(test_slab_remote_pending_mirror(slab) == 0 &&
                    test_slab_remote_pending(slab) == 0 &&
                    test_slab_remote_inflight_mirror(slab) == 0,
                "lockless class publish busy counters drained");
    expect_ok(api->check(heap, 0), "lockless class publish busy final check");

    api->free(heap, reuse);
    expect_ok(api->close(heap), "lockless class publish busy close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

