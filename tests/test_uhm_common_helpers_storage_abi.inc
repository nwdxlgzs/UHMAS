/*
 * UHM test heap storage and arenas must be backed by the same platform VM
 * contract that mmap/protect/decommit/remap exercise. This helper never uses
 * the C heap; it reserves, commits, and releases through uhm_vm_ops.
 */
static void *test_vm_reserve_commit_alloc(size_t size, size_t alignment, test_alloc *out) {
    uhm_vm_ops vm;
    size_t reserve_size;
    size_t reserve_alignment;
    uhm_result result;
    void *addr = 0;

    if (!out || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1u)) != 0) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    result = uhm_platform_vm_ops_init(&vm);
    if (result != UHM_OK ||
        (vm.capabilities & (UHM_VM_CAN_RESERVE | UHM_VM_CAN_COMMIT |
                            UHM_VM_CAN_RELEASE)) !=
            (UHM_VM_CAN_RESERVE | UHM_VM_CAN_COMMIT | UHM_VM_CAN_RELEASE) ||
        !vm.reserve || !vm.commit || !vm.release) {
        return 0;
    }
    reserve_alignment = alignment > vm.allocation_granularity
                            ? alignment
                            : vm.allocation_granularity;
    if (!uhm_platform_size_align_up(size, vm.page_size, &reserve_size)) {
        return 0;
    }
    result = vm.reserve(vm.user, &addr, reserve_size, reserve_alignment);
    if (result != UHM_OK) {
        return 0;
    }
    if (((uintptr_t)addr % alignment) != 0) {
        (void)vm.release(vm.user, addr, reserve_size);
        return 0;
    }
    result = vm.commit(vm.user, addr, reserve_size,
                       UHM_PROT_READ | UHM_PROT_WRITE);
    if (result != UHM_OK) {
        (void)vm.release(vm.user, addr, reserve_size);
        return 0;
    }
    out->raw = addr;
    out->aligned = addr;
    out->reserved_size = reserve_size;
    out->vm = vm;
    return out->aligned;
}

static void test_vm_release_alloc(test_alloc *alloc) {
    if (!alloc) {
        return;
    }
    if (alloc->raw && alloc->vm.release && alloc->reserved_size != 0) {
        (void)alloc->vm.release(alloc->vm.user, alloc->raw,
                                alloc->reserved_size);
    }
    memset(alloc, 0, sizeof(*alloc));
}

static void expect_ok(uhm_result result, const char *message) {
    if (result != UHM_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static void expect_true(int value, const char *message) {
    if (!value) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void expect_generation_advanced(uhm_heap *heap, uint64_t before,
                                       const char *message) {
    expect_true(heap != 0 && uhm_heap_generation_load(heap) > before, message);
}

static int test_lockless_remote_primitives_supported(void) {
    int supported = uhm_lockless_remote_primitives_supported();
#if defined(_WIN32)
    if (GetCurrentThreadId() == 0u) {
#else
    if (getpid() == 0) {
#endif
        return !supported;
    }
    return supported;
}

#define EXPECT_ABI_PREFIX(type_name)                                             \
    do {                                                                         \
        expect_true(offsetof(type_name, size) == 0u, #type_name " size offset"); \
        expect_true(offsetof(type_name, abi_major) == 4u,                        \
                    #type_name " abi_major offset");                            \
        expect_true(offsetof(type_name, abi_minor) == 6u,                        \
                    #type_name " abi_minor offset");                            \
        expect_true(offsetof(type_name, flags) == 8u,                            \
                    #type_name " flags offset");                                \
        expect_true((sizeof(type_name) % alignof(type_name)) == 0u,              \
                    #type_name " size alignment");                              \
    } while (0)

static void test_public_abi_layout_and_storage_contracts(void) {
    uhm_abi_info abi_info;
    uhm_storage_info heap_info;
    uhm_storage_info ctx_info;
    uhm_config config;
    test_alloc heap_mem = {0};
    uhm_heap *heap = 0;

    EXPECT_ABI_PREFIX(uhm_abi_info);
    EXPECT_ABI_PREFIX(uhm_storage_info);
    EXPECT_ABI_PREFIX(uhm_vm_ops);
    EXPECT_ABI_PREFIX(uhm_mmap_options);
    EXPECT_ABI_PREFIX(uhm_config);
    EXPECT_ABI_PREFIX(uhm_thread_config);
    EXPECT_ABI_PREFIX(uhm_stats);
    EXPECT_ABI_PREFIX(uhm_leak_record);
    EXPECT_ABI_PREFIX(uhm_api);

    memset(&abi_info, 0xa5, sizeof(abi_info));
    abi_info.size = sizeof(abi_info);
    expect_ok(uhm_abi_query(&abi_info), "abi layout query");
    expect_true(abi_info.size == sizeof(abi_info), "abi layout size");
    expect_true(abi_info.abi_major == UHM_ABI_MAJOR &&
                    abi_info.abi_minor == UHM_ABI_MINOR,
                "abi layout version");
    expect_true(abi_info.flags == 0u && abi_info.reserved == 0u,
                "abi layout flags cleared");
    expect_true(abi_info.pointer_size == sizeof(void *) &&
                    abi_info.size_t_size == sizeof(size_t),
                "abi layout word sizes");
    expect_true(abi_info.atomic_pointer_lock_free == ATOMIC_POINTER_LOCK_FREE,
                "abi layout atomic pointer lock-free");
    expect_true(abi_info.atomic_u64_lock_free ==
                    (uint32_t)uhm_atomic_u64_is_lock_free(),
                "abi layout atomic u64 lock-free");
    expect_true(abi_info.tagged_freelist_supported ==
                    (uint32_t)uhm_tagged_freelist_is_supported(),
                "abi layout tagged freelist supported");
    expect_true(abi_info.tagged_freelist_tag_bits ==
                    (uint32_t)UHM_TAGGED_FREELIST_TAG_BITS,
                "abi layout tagged freelist tag bits");
    expect_true(abi_info.lockless_remote_primitives_supported ==
                    (uint32_t)test_lockless_remote_primitives_supported(),
                "abi layout lockless remote primitives");
    expect_true(abi_info.lockless_remote_config_supported ==
                    (uint32_t)test_lockless_remote_primitives_supported(),
                "abi layout lockless remote config supported");
    expect_true(abi_info.reserved2 == 0u, "abi layout reserved2 cleared");
    expect_true(abi_info.atomic_u32_lock_free ==
                    (uint32_t)uhm_atomic_u32_is_lock_free(),
                "abi layout atomic u32 lock-free");
    expect_true(abi_info.malloc_min_alignment == UHM_MIN_ALIGNMENT,
                "abi layout malloc min alignment");
    expect_true(abi_info.cacheline_size == UHM_CACHELINE_SIZE,
                "abi layout cacheline size");
#if defined(_WIN32)
    expect_true(abi_info.call_abi == UHM_CALL_ABI_CDECL,
                "abi layout Windows call ABI");
#else
    expect_true(abi_info.call_abi == UHM_CALL_ABI_C,
                "abi layout C call ABI");
#endif

    memset(&abi_info, 0xa5, sizeof(abi_info));
    abi_info.size = UHM_ABI_INFO_SIZE_ABI9;
    expect_ok(uhm_abi_query(&abi_info), "abi layout abi9 query");
    expect_true(abi_info.size == sizeof(abi_info), "abi layout abi9 size");
    expect_true(abi_info.abi_minor == UHM_ABI_MINOR,
                "abi layout abi9 reports current version");
    expect_true(abi_info.reserved == 0u, "abi layout abi9 reserved");
    expect_true(abi_info.atomic_u64_lock_free == 0xa5a5a5a5u,
                "abi layout abi9 preserves tail");

    abi_info.size = UHM_ABI_INFO_SIZE_ABI9 - 1u;
    expect_true(uhm_abi_query(&abi_info) == UHM_ERR_INVALID_ARGUMENT,
                "abi layout rejects short query");

    memset(&heap_info, 0xa5, sizeof(heap_info));
    memset(&ctx_info, 0xa5, sizeof(ctx_info));
    heap_info.size = sizeof(heap_info);
    ctx_info.size = sizeof(ctx_info);
    expect_ok(uhm_heap_storage_query(&heap_info, &ctx_info),
              "abi layout storage query");
    expect_true(heap_info.size == sizeof(heap_info) &&
                    heap_info.flags == 0u &&
                    heap_info.abi_major == UHM_ABI_MAJOR &&
                    heap_info.abi_minor == UHM_ABI_MINOR,
                "abi layout heap storage metadata");
    expect_true(ctx_info.size == sizeof(ctx_info) &&
                    ctx_info.flags == 0u &&
                    ctx_info.abi_major == UHM_ABI_MAJOR &&
                    ctx_info.abi_minor == UHM_ABI_MINOR,
                "abi layout ctx storage metadata");
    expect_true(heap_info.storage_size == sizeof(uhm_heap) &&
                    heap_info.storage_alignment == alignof(uhm_heap),
                "abi layout heap storage contract");
    expect_true(ctx_info.storage_size == sizeof(uhm_thread_ctx) &&
                    ctx_info.storage_alignment == alignof(uhm_thread_ctx),
                "abi layout ctx storage contract");
    expect_true(heap_info.storage_alignment != 0u &&
                    (heap_info.storage_alignment &
                     (heap_info.storage_alignment - 1u)) == 0u &&
                    ctx_info.storage_alignment != 0u &&
                    (ctx_info.storage_alignment &
                     (ctx_info.storage_alignment - 1u)) == 0u,
                "abi layout storage alignment power of two");

    heap_info.size = sizeof(heap_info) - 1u;
    expect_true(uhm_heap_storage_query(&heap_info, 0) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "abi layout rejects short heap storage query");
    ctx_info.size = sizeof(ctx_info) - 1u;
    expect_true(uhm_heap_storage_query(0, &ctx_info) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "abi layout rejects short ctx storage query");

    expect_true(test_vm_reserve_commit_alloc(sizeof(uhm_heap) +
                                        heap_info.storage_alignment,
                                    heap_info.storage_alignment, &heap_mem) != 0,
                "abi layout heap storage allocation");
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = sizeof(uhm_heap) - 1u;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "abi layout rejects undersized heap storage");
    config.heap_storage = (void *)((uintptr_t)heap_mem.aligned + 1u);
    config.heap_storage_size = sizeof(uhm_heap);
    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "abi layout rejects misaligned heap storage");
    test_vm_release_alloc(&heap_mem);
}

static void test_stats_tail_capability_status(const uhm_storage_info *heap_info) {
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_stats stats;
    uint32_t expected_enabled;
    const size_t page_size = uhm_platform_system_page_size();

    expect_ok(uhm_platform_vm_ops_init(&vm), "stats tail vm init");
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "stats tail heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(page_size * 64u, page_size,
                                             &arena_mem) != 0,
                "stats tail arena alloc");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE |
                   UHM_CONFIG_FAIL_ON_TREAP_HEALTH;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_mem.reserved_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    expect_ok(uhm_open(&config, &heap), "stats tail heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "stats tail api");

    expected_enabled = (uint32_t)test_lockless_remote_primitives_supported();
    memset(&stats, 0xa5, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "stats tail full query");
    expect_true(stats.size == sizeof(stats), "stats tail full size");
    expect_true(stats.abi_minor == UHM_ABI_MINOR, "stats tail abi minor");
    expect_true(stats.flags == 0u, "stats tail flags");
    expect_true(stats.config_flags_enabled ==
                    ((expected_enabled ? UHM_CONFIG_LOCKLESS_REMOTE_FREE : 0u) |
                     UHM_CONFIG_FAIL_ON_TREAP_HEALTH),
                "stats tail actual config flags");
    expect_true(stats.lockless_remote_free_enabled == expected_enabled,
                "stats tail lockless free enabled");
    expect_true(stats.lockless_remote_malloc_enabled == expected_enabled,
                "stats tail lockless malloc enabled");
    expect_true(stats.lockless_remote_mode ==
                    (expected_enabled
                         ? UHM_LOCKLESS_REMOTE_MODE_POINTER_TAGGED_LOCKFREE
                         : UHM_LOCKLESS_REMOTE_MODE_NONE),
                "stats tail lockless mode");
    expect_true(stats.reserved == 0u, "stats tail reserved");
    expect_true(stats.decommit_events == 0u && stats.decommit_bytes == 0u,
                "stats tail decommit counters initially zero");
    expect_true(stats.recommit_events == 0u && stats.recommit_bytes == 0u,
                "stats tail recommit counters initially zero");
    expect_true(stats.malloc_free_decommit_deferred_events == 0u &&
                    stats.malloc_free_decommit_deferred_bytes == 0u,
                "stats tail deferred decommit counters initially zero");
    expect_true(stats.decommit_recommit_same_addr_events == 0u &&
                    stats.decommit_recommit_same_addr_bytes == 0u,
                "stats tail same-address counters initially zero");
    expect_true(stats.page_span_calls ==
                        test_atomic_u64_load(&heap->core.page_span_calls) &&
                    stats.page_span_pages ==
                        test_atomic_u64_load(&heap->core.page_span_pages),
                "stats tail page span counters match core");
    expect_true(stats.page_mark_calls ==
                        test_atomic_u64_load(&heap->core.page_mark_calls) &&
                    stats.page_mark_pages ==
                        test_atomic_u64_load(&heap->core.page_mark_pages) &&
                    stats.page_mark_max_pages ==
                        test_atomic_u64_load(&heap->core.page_mark_max_pages),
                "stats tail page mark counters match core");
    expect_true(stats.page_range_query_calls ==
                        test_atomic_u64_load(
                            &heap->core.page_range_query_calls) &&
                    stats.page_range_query_pages ==
                        test_atomic_u64_load(
                            &heap->core.page_range_query_pages) &&
                    stats.page_range_query_max_pages ==
                        test_atomic_u64_load(
                            &heap->core.page_range_query_max_pages),
                "stats tail page query counters match core");
    expect_true(stats.vm_reserve_calls == heap->core.vm_reserve_calls &&
                    stats.vm_reserve_failures ==
                        heap->core.vm_reserve_failures &&
                    stats.vm_reserve_ns == heap->core.vm_reserve_ns,
                "stats tail vm reserve counters match core");
    expect_true(stats.vm_commit_calls == heap->core.vm_commit_calls &&
                    stats.vm_commit_failures ==
                        heap->core.vm_commit_failures &&
                    stats.vm_commit_ns == heap->core.vm_commit_ns,
                "stats tail vm commit counters match core");
    expect_true(stats.vm_decommit_calls == heap->core.vm_decommit_calls &&
                    stats.vm_decommit_failures ==
                        heap->core.vm_decommit_failures &&
                    stats.vm_decommit_ns == heap->core.vm_decommit_ns,
                "stats tail vm decommit counters match core");
    expect_true(stats.vm_fixed_remap_calls ==
                        heap->core.vm_fixed_remap_calls &&
                    stats.vm_fixed_remap_failures ==
                        heap->core.vm_fixed_remap_failures &&
                    stats.vm_fixed_remap_ns == heap->core.vm_fixed_remap_ns,
                "stats tail vm fixed remap counters match core");
    expect_true(stats.vm_protect_calls == heap->core.vm_protect_calls &&
                    stats.vm_protect_failures ==
                        heap->core.vm_protect_failures &&
                    stats.vm_protect_ns == heap->core.vm_protect_ns,
                "stats tail vm protect counters match core");
    expect_true(stats.vm_release_calls == heap->core.vm_release_calls &&
                    stats.vm_release_failures ==
                        heap->core.vm_release_failures &&
                    stats.vm_release_ns == heap->core.vm_release_ns,
                "stats tail vm release counters match core");
    expect_true(stats.vm_numa_hint_calls == heap->core.vm_numa_hint_calls &&
                    stats.vm_numa_hint_failures ==
                        heap->core.vm_numa_hint_failures &&
                    stats.vm_numa_hint_ns == heap->core.vm_numa_hint_ns,
                "stats tail vm numa counters match core");
    expect_true(stats.metadata_retired_vma_count ==
                        (uint64_t)heap->core.retired_vma_count &&
                    stats.metadata_retired_free_extent_count ==
                        (uint64_t)heap->core.retired_free_extent_count,
                "stats tail metadata retired counters match core");
    expect_true(stats.metadata_hot_vma_count ==
                        (uint64_t)heap->core.hot_vma_count &&
                    stats.metadata_hot_free_extent_count ==
                        (uint64_t)heap->core.hot_free_extent_count,
                "stats tail metadata hot counts match core");
    expect_true(stats.metadata_hot_vma_hits ==
                        heap->core.metadata_hot_vma_hits &&
                    stats.metadata_hot_vma_misses ==
                        heap->core.metadata_hot_vma_misses &&
                    stats.metadata_hot_vma_reclaimed ==
                        heap->core.metadata_hot_vma_reclaimed,
                "stats tail metadata hot vma counters match core");
    expect_true(stats.metadata_hot_free_extent_hits ==
                        heap->core.metadata_hot_free_extent_hits &&
                    stats.metadata_hot_free_extent_misses ==
                        heap->core.metadata_hot_free_extent_misses &&
                    stats.metadata_hot_free_extent_reclaimed ==
                        heap->core.metadata_hot_free_extent_reclaimed,
                "stats tail metadata hot extent counters match core");
    expect_true(stats.metadata_quarantine_drain_calls ==
                        heap->core.metadata_quarantine_drain_calls &&
                    stats.metadata_quarantine_drain_scanned ==
                        heap->core.metadata_quarantine_drain_scanned &&
                    stats.metadata_quarantine_drain_reclaimed ==
                        heap->core.metadata_quarantine_drain_reclaimed &&
                    stats.metadata_quarantine_drain_budget_exhausted ==
                        heap->core.metadata_quarantine_drain_budget_exhausted,
                "stats tail metadata quarantine counters match core");
    expect_true(stats.page_lock_range_calls ==
                        test_atomic_u64_load(
                            &heap->core.page_lock_range_calls) &&
                    stats.page_lock_range_shards ==
                        test_atomic_u64_load(
                            &heap->core.page_lock_range_shards) &&
                    stats.page_lock_range_max_shards ==
                        test_atomic_u64_load(
                            &heap->core.page_lock_range_max_shards) &&
                    stats.page_lock_range_budget_exceeded ==
                        test_atomic_u64_load(
                            &heap->core.page_lock_range_budget_exceeded),
                "stats tail page lock counters match core");

    memset(&stats, 0xa5, sizeof(stats));
    stats.size = UHM_STATS_SIZE_ABI15;
    expect_ok(api->get_stats(heap, &stats), "stats tail abi15 query");
    expect_true(stats.size == sizeof(stats), "stats tail abi15 size");
    expect_true(stats.metadata_retired_vma_count ==
                    (uint64_t)heap->core.retired_vma_count,
                "stats tail abi15 metadata counters visible");
    expect_true(stats.page_lock_range_calls ==
                    UINT64_C(0xa5a5a5a5a5a5a5a5),
                "stats tail abi15 preserves page lock counters");

    memset(&stats, 0xa5, sizeof(stats));
    stats.size = UHM_STATS_SIZE_ABI14;
    expect_ok(api->get_stats(heap, &stats), "stats tail abi14 query");
    expect_true(stats.size == sizeof(stats), "stats tail abi14 size");
    expect_true(stats.vm_reserve_calls == heap->core.vm_reserve_calls,
                "stats tail abi14 vm counters visible");
    expect_true(stats.metadata_retired_vma_count ==
                    UINT64_C(0xa5a5a5a5a5a5a5a5),
                "stats tail abi14 preserves metadata counters");

    memset(&stats, 0xa5, sizeof(stats));
    stats.size = UHM_STATS_SIZE_ABI13;
    expect_ok(api->get_stats(heap, &stats), "stats tail abi13 query");
    expect_true(stats.size == sizeof(stats), "stats tail abi13 size");
    expect_true(stats.page_span_calls ==
                    test_atomic_u64_load(&heap->core.page_span_calls),
                "stats tail abi13 page counters visible");
    expect_true(stats.vm_reserve_calls == UINT64_C(0xa5a5a5a5a5a5a5a5),
                "stats tail abi13 preserves vm counters");

    memset(&stats, 0xa5, sizeof(stats));
    stats.size = UHM_STATS_SIZE_ABI12;
    expect_ok(api->get_stats(heap, &stats), "stats tail abi12 query");
    expect_true(stats.size == sizeof(stats), "stats tail abi12 size");
    expect_true(stats.decommit_events == heap->core.decommit_events,
                "stats tail abi12 decommit visible");
    expect_true(stats.page_span_calls == UINT64_C(0xa5a5a5a5a5a5a5a5),
                "stats tail abi12 preserves page counters");

    memset(&stats, 0xa5, sizeof(stats));
    stats.size = UHM_STATS_SIZE_ABI11;
    expect_ok(api->get_stats(heap, &stats), "stats tail abi11 query");
    expect_true(stats.size == sizeof(stats), "stats tail abi11 size");
    expect_true(stats.reserved == 0u, "stats tail abi11 reserved");
    expect_true(stats.decommit_events == UINT64_C(0xa5a5a5a5a5a5a5a5),
                "stats tail abi11 preserves decommit counters");

    memset(&stats, 0xa5, sizeof(stats));
    stats.size = UHM_STATS_SIZE_ABI10;
    expect_ok(api->get_stats(heap, &stats), "stats tail abi10 query");
    expect_true(stats.size == sizeof(stats), "stats tail abi10 size");
    expect_true(stats.collision_free_bytes <= stats.arena_size,
                "stats tail abi10 prefix valid");
    expect_true(stats.config_flags_enabled == 0xa5a5a5a5u,
                "stats tail abi10 preserves tail");

    stats.size = UHM_STATS_SIZE_ABI10 - 1u;
    expect_true(api->get_stats(heap, &stats) == UHM_ERR_INVALID_ARGUMENT,
                "stats tail rejects short query");

    expect_ok(api->close(heap), "stats tail close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

