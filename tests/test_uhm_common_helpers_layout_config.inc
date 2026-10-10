static void test_internal_cacheline_layout(void) {
    expect_true(UHM_CACHELINE_SIZE != 0u &&
                    (UHM_CACHELINE_SIZE & (UHM_CACHELINE_SIZE - 1u)) == 0u,
                "cacheline size power of two");
    expect_true((sizeof(uhm_heap) % alignof(uhm_heap)) == 0u,
                "heap size alignment");
    expect_true((sizeof(uhm_heap_core) % alignof(uhm_heap_core)) == 0u,
                "heap core size alignment");
    expect_true((sizeof(uhm_slab) % alignof(uhm_slab)) == 0u,
                "slab size alignment");
    expect_true((sizeof(uhm_slab_slot) % alignof(uhm_slab_slot)) == 0u,
                "slab slot size alignment");
    expect_true((sizeof(uhm_tagged_freelist) %
                 alignof(uhm_tagged_freelist)) == 0u,
                "tagged freelist size alignment");
    expect_true((sizeof(uhm_metadata_reader_state) %
                 alignof(uhm_metadata_reader_state)) == 0u,
                "metadata reader size alignment");
    expect_true((sizeof(uhm_tls_cache) % alignof(uhm_tls_cache)) == 0u,
                "TLS cache size alignment");
    expect_true((sizeof(uhm_heap_stats_internal) %
                 alignof(uhm_heap_stats_internal)) == 0u,
                "heap stats size alignment");
    expect_true(offsetof(uhm_slab_slot, state_word) %
                        alignof(_Atomic(uint64_t)) ==
                    0u,
                "slab slot state word aligned");
    expect_true(offsetof(uhm_slab_slot, remote_link) %
                        alignof(uhm_tagged_freelist_node) ==
                    0u,
                "slab slot remote link aligned");
    expect_true(offsetof(uhm_slab_slot, next_free) %
                        alignof(uhm_slab_slot *) ==
                    0u,
                "slab slot next free aligned");
    expect_true(sizeof(uhm_spinlock_cell) >= UHM_CACHELINE_SIZE &&
                    (sizeof(uhm_spinlock_cell) % UHM_CACHELINE_SIZE) == 0u,
                "spinlock cell cacheline size");
    expect_true(offsetof(uhm_spinlock_cell, lock) == 0u,
                "spinlock cell lock at start");
    expect_true(sizeof(uhm_tagged_freelist_cell) >= UHM_CACHELINE_SIZE &&
                    (sizeof(uhm_tagged_freelist_cell) %
                     UHM_CACHELINE_SIZE) == 0u,
                "tagged freelist cell cacheline size");
    expect_true(offsetof(uhm_tagged_freelist_cell, queue) == 0u,
                "tagged freelist cell queue at start");
    expect_true(sizeof(uhm_tagged_freelist) >=
                        (UHM_CACHELINE_SIZE * 2u) &&
                    (sizeof(uhm_tagged_freelist) %
                     UHM_CACHELINE_SIZE) == 0u,
                "tagged freelist hot/cold cacheline size");
    expect_true(offsetof(uhm_tagged_freelist, head) == 0u,
                "tagged freelist head at start");
    expect_true((offsetof(uhm_tagged_freelist, overflow_lock) %
                 UHM_CACHELINE_SIZE) == 0u,
                "tagged freelist cold fields cacheline aligned");
    expect_true(sizeof(uhm_atomic_u64_cell) >= UHM_CACHELINE_SIZE &&
                    (sizeof(uhm_atomic_u64_cell) %
                     UHM_CACHELINE_SIZE) == 0u,
                "atomic u64 cell cacheline size");
    expect_true(offsetof(uhm_atomic_u64_cell, value) == 0u,
                "atomic u64 cell value at start");
    expect_true(sizeof(uhm_metadata_reader_state) >=
                        (UHM_CACHELINE_SIZE * 2u) &&
                    (sizeof(uhm_metadata_reader_state) %
                     UHM_CACHELINE_SIZE) == 0u,
                "metadata reader state cacheline grouped");
    expect_true((offsetof(uhm_metadata_reader_state, hazards) %
                 UHM_CACHELINE_SIZE) == 0u,
                "metadata reader hazards cacheline aligned");
    expect_true((offsetof(uhm_heap_core, slab_remote_class_lf) %
                 UHM_CACHELINE_SIZE) == 0u,
                "class remote queues cacheline aligned");
    expect_true((offsetof(uhm_heap_core, slab_remote_ready_lf) %
                 UHM_CACHELINE_SIZE) == 0u,
                "ready remote queues cacheline aligned");
    expect_true((offsetof(uhm_heap_core, slab_remote_ready_seq) %
                 UHM_CACHELINE_SIZE) == 0u,
                "ready seq cells cacheline aligned");
    expect_true((offsetof(uhm_heap_core, slab_remote_pending_masks) %
                 UHM_CACHELINE_SIZE) == 0u,
                "pending mask cells cacheline aligned");
#if UINTPTR_MAX == UINT32_MAX
    expect_true(offsetof(uhm_heap_core, slab_remote_ready_raw) ==
                    offsetof(uhm_heap_core, slab_remote_ready_seq) +
                    sizeof(((uhm_heap_core *)0)->slab_remote_ready_seq),
                "32-bit raw-ready directory follows ready sequences");
    expect_true(offsetof(uhm_heap_core, slab_remote_pending_masks) ==
                    offsetof(uhm_heap_core, slab_remote_ready_raw) +
                    sizeof(((uhm_heap_core *)0)->slab_remote_ready_raw),
                "32-bit raw-ready directory keeps padded class stride");
#else
    expect_true(offsetof(uhm_heap_core, slab_remote_pending_masks) ==
                    offsetof(uhm_heap_core, slab_remote_ready_seq) +
                    sizeof(((uhm_heap_core *)0)->slab_remote_ready_seq),
                "non-32-bit layout has no unused raw-ready directory");
#endif
    expect_true((offsetof(uhm_heap_core, page_map_locks) %
                 UHM_CACHELINE_SIZE) == 0u,
                "page-map shard locks cacheline aligned");
    expect_true((offsetof(uhm_heap_core, vma_locks) %
                 UHM_CACHELINE_SIZE) == 0u,
                "vma shard locks cacheline aligned");
    expect_true((offsetof(uhm_heap_core, extent_locks) %
                 UHM_CACHELINE_SIZE) == 0u,
                "extent shard locks cacheline aligned");
    expect_true((offsetof(uhm_heap_core, slab_locks) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab shard locks cacheline aligned");
    expect_true((offsetof(uhm_heap_core, lockless_remote_readers) %
                 UHM_CACHELINE_SIZE) == 0u,
                "lockless metadata readers cacheline aligned");
    expect_true((offsetof(uhm_thread_ctx, metadata_reader) %
                 UHM_CACHELINE_SIZE) == 0u,
                "thread ctx metadata reader cacheline aligned");
    expect_true((offsetof(uhm_tls_cache, metadata_reader) %
                 UHM_CACHELINE_SIZE) == 0u,
                "TLS cache metadata reader cacheline aligned");
    expect_true(alignof(uhm_tls_cache) >= UHM_CACHELINE_SIZE,
                "TLS cache slot cacheline alignment");
    expect_true(sizeof(uhm_tls_cache) >= UHM_CACHELINE_SIZE &&
                    (sizeof(uhm_tls_cache) % UHM_CACHELINE_SIZE) == 0u,
                "TLS cache slot stride padded");
    expect_true(alignof(uhm_heap_stats_internal) >= UHM_CACHELINE_SIZE,
                "heap stats cacheline alignment");
    expect_true(sizeof(uhm_heap_stats_internal) >=
                        (UHM_CACHELINE_SIZE *
                         (UHM_MALLOC_LIVE_STAT_SHARDS + 1u)) &&
                    (sizeof(uhm_heap_stats_internal) %
                     UHM_CACHELINE_SIZE) == 0u,
                "heap stats hot/cold cacheline size");
    expect_true(offsetof(uhm_heap_stats_internal, malloc_live_shards) == 0u,
                "heap stats malloc live shards at start");
    expect_true(sizeof(((uhm_heap_stats_internal *)0)->malloc_live_shards[0]) %
                        UHM_CACHELINE_SIZE ==
                    0u,
                "heap stats malloc live shard stride");
    expect_true((offsetof(uhm_heap_stats_internal, committed_bytes) %
                 UHM_CACHELINE_SIZE) == 0u,
                "heap stats cold fields cacheline aligned");
    expect_true(alignof(uhm_slab) >= UHM_CACHELINE_SIZE,
                "slab cacheline alignment");
    expect_true((offsetof(uhm_slab, summary_seq) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab summary seq cacheline aligned");
    expect_true((offsetof(uhm_slab, raw_publish_seq) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab raw publish seq cacheline aligned");
    expect_true((offsetof(uhm_slab, state_transition_seq) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab state transition seq cacheline aligned");
    expect_true((offsetof(uhm_slab, raw_lock) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab raw lock cacheline aligned");
    expect_true((offsetof(uhm_slab, live_slots_count) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab raw mirror counters cacheline aligned");
    expect_true((offsetof(uhm_slab, remote_counter_seq) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab remote counters cacheline aligned");
    expect_true((offsetof(uhm_slab, remote_drain_active) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab remote drain owner cacheline aligned");
    expect_true((offsetof(uhm_slab, remote_ready_queued) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab remote ready owner cacheline aligned");
    expect_true((offsetof(uhm_slab, free_head) %
                 UHM_CACHELINE_SIZE) == 0u,
                "slab pointer fields cacheline aligned");
    expect_true((offsetof(uhm_heap, tls_cache_slots) %
                 UHM_CACHELINE_SIZE) == 0u,
                "TLS cache registry cacheline aligned");
    expect_true(alignof(uhm_heap) >= UHM_CACHELINE_SIZE,
                "heap cacheline alignment");
    expect_true((offsetof(uhm_heap, lifecycle_state) %
                 UHM_CACHELINE_SIZE) == 0u,
                "heap lifecycle cacheline aligned");
    expect_true((offsetof(uhm_heap, active_ops) %
                 UHM_CACHELINE_SIZE) == 0u,
                "heap active ops cacheline aligned");
    expect_true((offsetof(uhm_heap, metadata_hazard_readers) %
                 UHM_CACHELINE_SIZE) == 0u,
                "heap hazard reader counter cacheline aligned");
    expect_true((offsetof(uhm_heap, metadata_registered_readers) %
                 UHM_CACHELINE_SIZE) == 0u,
                "heap registered reader counter cacheline aligned");
    expect_true((offsetof(uhm_heap, metadata_registered_reader_seq) %
                 UHM_CACHELINE_SIZE) == 0u,
                "heap registered reader seq cacheline aligned");
    expect_true((offsetof(uhm_heap, F) % UHM_CACHELINE_SIZE) == 0u,
                "heap F table cacheline aligned");
    expect_true((offsetof(uhm_heap, lock) % UHM_CACHELINE_SIZE) == 0u,
                "heap lock cacheline aligned");
    expect_true((offsetof(uhm_heap, last_error) % UHM_CACHELINE_SIZE) == 0u,
                "heap last error cacheline aligned");
    expect_true((offsetof(uhm_heap, platform_error) %
                 UHM_CACHELINE_SIZE) == 0u,
                "heap platform error cacheline aligned");
    expect_true((offsetof(uhm_heap, core) % UHM_CACHELINE_SIZE) == 0u,
                "heap core cacheline aligned");
    expect_true((offsetof(uhm_heap, stats) % UHM_CACHELINE_SIZE) == 0u,
                "heap stats cacheline aligned");
    expect_true(sizeof(((uhm_heap_core *)0)->page_map_locks[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)->page_map_locks[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "page-map shard lock stride padded");
    expect_true(sizeof(((uhm_heap_core *)0)->vma_locks[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)->vma_locks[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "vma shard lock stride padded");
    expect_true(sizeof(((uhm_heap_core *)0)->extent_locks[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)->extent_locks[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "extent shard lock stride padded");
    expect_true(sizeof(((uhm_heap_core *)0)->slab_locks[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)->slab_locks[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "slab shard lock stride padded");
    expect_true(sizeof(((uhm_heap_core *)0)->slab_remote_class_lf[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)->slab_remote_class_lf[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "class remote queue stride padded");
    expect_true(sizeof(((uhm_heap_core *)0)->slab_remote_ready_lf[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)->slab_remote_ready_lf[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "ready remote queue stride padded");
    expect_true(sizeof(((uhm_heap_core *)0)->slab_remote_ready_seq[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)->slab_remote_ready_seq[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "ready seq stride padded");
    expect_true(sizeof(((uhm_heap_core *)0)->slab_remote_pending_masks[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)
                                ->slab_remote_pending_masks[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "pending mask stride padded");
    expect_true(sizeof(((uhm_heap_core *)0)->lockless_remote_readers[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap_core *)0)
                                ->lockless_remote_readers[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "lockless metadata reader stride padded");
    expect_true(sizeof(((uhm_heap *)0)->tls_cache_slots[0]) >=
                    UHM_CACHELINE_SIZE &&
                    (sizeof(((uhm_heap *)0)->tls_cache_slots[0]) %
                     UHM_CACHELINE_SIZE) == 0u,
                "TLS cache registry stride padded");
}

static void test_internal_metadata_footprint_budget(void) {
    expect_true(sizeof(uhm_heap) <= (256u * 1024u),
                "heap storage footprint budget");
    expect_true(sizeof(uhm_heap_core) <= (128u * 1024u),
                "heap core footprint budget");
    expect_true(sizeof(uhm_thread_ctx) <= (16u * 1024u),
                "thread ctx footprint budget");
    expect_true(sizeof(uhm_tls_cache) <= 2048u,
                "TLS cache slot footprint budget");
    expect_true(sizeof(uhm_slab) <= 4096u,
                "slab metadata footprint budget");
    expect_true(sizeof(uhm_slab_slot) <= 128u,
                "slab slot metadata footprint budget");
    expect_true(sizeof(uhm_tagged_freelist) <= 256u,
                "tagged freelist footprint budget");
    expect_true(sizeof(uhm_metadata_reader_state) <= 256u,
                "metadata reader footprint budget");
    expect_true(sizeof(uhm_heap_stats_internal) <=
                    UHM_CACHELINE_SIZE *
                        (UHM_MALLOC_LIVE_STAT_SHARDS + 2u),
                "heap stats footprint budget");
    expect_true(sizeof(uhm_page_leaf) <= (64u * 1024u),
                "page-map leaf footprint budget");
    expect_true(sizeof(uhm_block) <= 512u,
                "malloc block metadata footprint budget");
    expect_true(sizeof(uhm_vma) <= 512u,
                "VMA metadata footprint budget");
    expect_true(sizeof(uhm_free_extent) <= 512u,
                "free extent metadata footprint budget");
    expect_true(sizeof(uhm_control_extent) <= 512u,
                "control extent metadata footprint budget");
}

static void test_spinlock_policy_config(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const uint32_t flags[] = {
        0u,
        UHM_CONFIG_SPINLOCK_BURN,
        UHM_CONFIG_SPINLOCK_YIELD,
    };
    const uint32_t modes[] = {
        UHM_SPINLOCK_MODE_YIELD,
        UHM_SPINLOCK_MODE_BURN,
        UHM_SPINLOCK_MODE_YIELD,
    };
    size_t i;

    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        test_alloc heap_mem = {0};
        test_alloc arena_mem = {0};
        uhm_vm_ops vm;
        uhm_config config;
        uhm_heap *heap = 0;
        const uhm_api *api;

        expect_true(test_vm_reserve_commit_alloc(
                        heap_info->storage_size, heap_info->storage_alignment,
                        &heap_mem) != 0,
                    "spin policy heap storage alloc");
        expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                                 &arena_mem) != 0,
                    "spin policy arena alloc");
        expect_ok(uhm_platform_usermode_vm_ops_init(
                      &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
                  "spin policy usermode vm init");

        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.abi_major = UHM_ABI_MAJOR;
        config.abi_minor = UHM_ABI_MINOR;
        config.flags = flags[i];
        config.heap_storage = heap_mem.aligned;
        config.heap_storage_size = heap_info->storage_size;
        config.arena_base = arena_mem.aligned;
        config.arena_size = 1024u * 1024u;
        config.arena_flags = UHM_ARENA_PRE_RESERVED;
        config.vm = vm;

        expect_ok(uhm_open(&config, &heap), "spin policy heap open");
        expect_true(heap->core.spinlock_mode == modes[i],
                    "spin policy heap mode");
        expect_true(heap->lock.mode == modes[i], "spin policy heap lock mode");
        expect_true(heap->lock.yield_after_spins ==
                        UHM_SPINLOCK_DEFAULT_YIELD_AFTER,
                    "spin policy yield threshold");
        expect_true(heap->core.page_map_locks[0].lock.mode == modes[i],
                    "spin policy shard lock mode");
        expect_true(heap->core.vma_locks[0].lock.yield_after_spins ==
                        UHM_SPINLOCK_DEFAULT_YIELD_AFTER,
                    "spin policy vma shard default threshold");
        expect_true(heap->core.extent_locks[0].lock.yield_after_spins ==
                        UHM_SPINLOCK_DEFAULT_YIELD_AFTER,
                    "spin policy extent shard default threshold");
        expect_true(heap->core.slab_locks[0].lock.yield_after_spins ==
                        UHM_SPINLOCK_DEFAULT_YIELD_AFTER,
                    "spin policy slab shard default threshold");
#if UHMAS_ENABLE_DIAGNOSTICS && defined(_WIN32)
        expect_true(heap->lock.diagnostics_timer_frequency != 0u,
                    "spin policy diagnostics timer frequency cached");
        expect_true(heap->core.page_map_locks[0]
                            .lock.diagnostics_timer_frequency != 0u,
                    "spin policy shard diagnostics timer frequency cached");
#endif
        api = uhm_get_api(heap);
        expect_true(api != 0, "spin policy api");
#if UHMAS_ENABLE_DIAGNOSTICS
        {
            uhm_stats stats;
            uint64_t lock_calls_before =
                uhm_diag_counter_load_relaxed(&heap->lock.lock_calls);
            uint64_t hold_ns_before =
                uhm_diag_counter_load_relaxed(&heap->lock.hold_ns);
            uhm_spinlock_set_diagnostics_enabled(&heap->lock, 1u);
            uhm_spinlock_set_diagnostics_timing_enabled(&heap->lock, 0u);
            memset(&stats, 0, sizeof(stats));
            stats.size = sizeof(stats);
            expect_ok(api->get_stats(heap, &stats),
                      "spin diagnostics counter-only stats");
            expect_true(uhm_diag_counter_load_relaxed(&heap->lock.lock_calls) ==
                            lock_calls_before + 1u,
                        "spin diagnostics counter-only counts lock");
            expect_true(uhm_diag_counter_load_relaxed(&heap->lock.hold_ns) ==
                            hold_ns_before,
                        "spin diagnostics counter-only skips timing");
            uhm_spinlock_set_diagnostics_timing_enabled(&heap->lock, 1u);
            expect_true(uhm_spinlock_diagnostics_timing_enabled(&heap->lock),
                        "spin diagnostics timing opt-in");
            uhm_spinlock_set_diagnostics_enabled(&heap->lock, 0u);
            uhm_spinlock_set_diagnostics_timing_enabled(&heap->lock, 0u);
        }
#endif
        expect_ok(api->close(heap), "spin policy close");

        test_vm_release_alloc(&arena_mem);
        test_vm_release_alloc(&heap_mem);
    }

    {
        test_alloc heap_mem = {0};
        test_alloc arena_mem = {0};
        uhm_vm_ops vm;
        uhm_config config;
        uhm_heap *heap = 0;
        const uhm_api *api;
        void *ptr;
        uhm_slab *slab;
        const uint32_t custom_yield_after = 7u;

        expect_true(test_vm_reserve_commit_alloc(
                        heap_info->storage_size, heap_info->storage_alignment,
                        &heap_mem) != 0,
                    "spin threshold heap storage alloc");
        expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                                 &arena_mem) != 0,
                    "spin threshold arena alloc");
        expect_ok(uhm_platform_usermode_vm_ops_init(
                      &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
                  "spin threshold usermode vm init");

        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.abi_major = UHM_ABI_MAJOR;
        config.abi_minor = UHM_ABI_MINOR;
        config.flags = UHM_CONFIG_SPINLOCK_YIELD;
        config.heap_storage = heap_mem.aligned;
        config.heap_storage_size = heap_info->storage_size;
        config.arena_base = arena_mem.aligned;
        config.arena_size = 1024u * 1024u;
        config.arena_flags = UHM_ARENA_PRE_RESERVED;
        config.vm = vm;
        config.spinlock_yield_after_spins = custom_yield_after;

        expect_ok(uhm_open(&config, &heap), "spin threshold heap open");
        expect_true(heap->core.spinlock_yield_after_spins ==
                        custom_yield_after,
                    "spin threshold stored in core");
        expect_true(heap->lock.yield_after_spins == custom_yield_after,
                    "spin threshold heap lock");
        expect_true(heap->core.page_map_locks[0].lock.yield_after_spins ==
                        custom_yield_after,
                    "spin threshold page-map shard");
        expect_true(heap->core.vma_locks[0].lock.yield_after_spins ==
                        custom_yield_after,
                    "spin threshold vma shard");
        expect_true(heap->core.extent_locks[0].lock.yield_after_spins ==
                        custom_yield_after,
                    "spin threshold extent shard");
        expect_true(heap->core.slab_locks[0].lock.yield_after_spins ==
                        custom_yield_after,
                    "spin threshold slab shard");
        api = uhm_get_api(heap);
        expect_true(api != 0, "spin threshold api");
        ptr = api->malloc(heap, 16u);
        expect_true(ptr != 0, "spin threshold small malloc");
        slab = heap->core.slab_class_heads[0];
        expect_true(slab != 0, "spin threshold slab created");
        expect_true(slab->raw_lock.yield_after_spins == custom_yield_after,
                    "spin threshold slab raw lock");
        api->free(heap, ptr);
        expect_ok(api->close(heap), "spin threshold close");

        test_vm_release_alloc(&arena_mem);
        test_vm_release_alloc(&heap_mem);
    }

    {
        uhm_platform_heap_config platform_config;
        uhm_platform_heap owner;
        uhm_heap *heap = 0;

        memset(&platform_config, 0, sizeof(platform_config));
        platform_config.size = sizeof(platform_config);
        platform_config.abi_major = UHM_ABI_MAJOR;
        platform_config.abi_minor = UHM_ABI_MINOR;
        platform_config.arena_size = 1024u * 1024u;
        platform_config.spinlock_yield_after_spins = 11u;
        expect_ok(uhm_platform_heap_open(&platform_config, &owner, &heap),
                  "platform spin threshold heap open");
        expect_true(heap->core.spinlock_yield_after_spins == 11u,
                    "platform spin threshold propagated");
        expect_true(heap->lock.yield_after_spins == 11u,
                    "platform spin threshold heap lock");
        expect_ok(uhm_platform_heap_close(&owner),
                  "platform spin threshold close");
    }

    {
        test_alloc heap_mem = {0};
        test_alloc arena_mem = {0};
        uhm_vm_ops vm;
        uhm_config config;
        uhm_heap *heap = 0;
        const uhm_api *api;

        expect_true(test_vm_reserve_commit_alloc(
                        heap_info->storage_size, heap_info->storage_alignment,
                        &heap_mem) != 0,
                    "spin abi8 heap storage alloc");
        expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                                 &arena_mem) != 0,
                    "spin abi8 arena alloc");
        expect_ok(uhm_platform_usermode_vm_ops_init(
                      &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
                  "spin abi8 usermode vm init");

        memset(&config, 0, sizeof(config));
        config.size = UHM_CONFIG_SIZE_ABI8;
        config.abi_major = UHM_ABI_MAJOR;
        config.abi_minor = 8u;
        config.heap_storage = heap_mem.aligned;
        config.heap_storage_size = heap_info->storage_size;
        config.arena_base = arena_mem.aligned;
        config.arena_size = 1024u * 1024u;
        config.arena_flags = UHM_ARENA_PRE_RESERVED;
        config.vm = vm;
        config.spinlock_yield_after_spins = 5u;

        expect_ok(uhm_open(&config, &heap), "spin abi8 heap open");
        expect_true(heap->core.spinlock_yield_after_spins ==
                        UHM_SPINLOCK_DEFAULT_YIELD_AFTER,
                    "spin abi8 ignores abi9 threshold");
        api = uhm_get_api(heap);
        expect_true(api != 0, "spin abi8 api");
        expect_ok(api->close(heap), "spin abi8 close");

        test_vm_release_alloc(&arena_mem);
        test_vm_release_alloc(&heap_mem);
    }

    {
        test_alloc heap_mem = {0};
        uhm_config config;
        uhm_heap *heap = 0;

        expect_true(test_vm_reserve_commit_alloc(
                        heap_info->storage_size, heap_info->storage_alignment,
                        &heap_mem) != 0,
                    "spin policy invalid heap storage alloc");
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.abi_major = UHM_ABI_MAJOR;
        config.abi_minor = UHM_ABI_MINOR;
        config.heap_storage = heap_mem.aligned;
        config.heap_storage_size = heap_info->storage_size;
        config.flags = UHM_CONFIG_SPINLOCK_BURN | UHM_CONFIG_SPINLOCK_YIELD;
        expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                    "spin policy rejects conflicting modes");
        test_vm_release_alloc(&heap_mem);
    }
}

static void test_treap_runtime_fail_fast_config(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_stats stats;
    uhm_result last_error = UHM_OK;
    uint32_t platform_error = 0u;
    void *map = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "treap runtime heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "treap runtime arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "treap runtime usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_CONFIG_FAIL_ON_TREAP_HEALTH |
                   UHM_CONFIG_FAIL_ON_LIVE_BLOCK_HEALTH;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "treap runtime heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "treap runtime api");
    expect_true((heap->state_flags & UHM_CONFIG_FAIL_ON_TREAP_HEALTH) != 0u,
                "treap runtime config enabled");
    expect_true((heap->state_flags & UHM_CONFIG_FAIL_ON_LIVE_BLOCK_HEALTH) !=
                    0u,
                "live block health runtime config enabled");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "treap runtime stats");
    expect_true((stats.config_flags_enabled &
                 UHM_CONFIG_FAIL_ON_TREAP_HEALTH) != 0u,
                "treap runtime stats expose flag");
    expect_true((stats.config_flags_enabled &
                 UHM_CONFIG_FAIL_ON_LIVE_BLOCK_HEALTH) != 0u,
                "live block health runtime stats expose flag");

    heap->core.treap_health_flags = UHM_TREAP_HEALTH_LIVE_FIND;
    api->free(heap, 0);
    expect_ok(api->get_last_error(heap, &last_error, &platform_error),
              "treap runtime last error after free null");
    expect_true(last_error == UHM_OK,
                "treap runtime free null remains no-op");
    expect_ok(api->check(heap, 0), "treap runtime default check ignores health");
    expect_true(api->check(heap, UHM_CHECK_FAIL_ON_TREAP_HEALTH) ==
                    UHM_ERR_CORRUPTION,
                "treap runtime strict check sees health");
    expect_true(api->malloc(heap, 16u) == 0,
                "treap runtime malloc fail-fast");
    expect_ok(api->get_last_error(heap, &last_error, &platform_error),
              "treap runtime malloc last error query");
    expect_true(last_error == UHM_ERR_CORRUPTION,
                "treap runtime malloc sets corruption");
    expect_true(api->mmap(heap, &map, page_size,
                          UHM_PROT_READ | UHM_PROT_WRITE,
                          UHM_MAP_PRIVATE) == UHM_ERR_CORRUPTION,
                "treap runtime mmap fail-fast");
    expect_true(map == 0, "treap runtime failed mmap leaves address null");

    heap->core.treap_health_flags = 0;
    heap->core.live_block_health_flags =
        UHM_LIVE_BLOCK_HEALTH_METADATA_DRIFT;
    expect_ok(api->check(heap, 0),
              "live block health default check ignores health");
    expect_ok(api->check(heap, UHM_CHECK_FAIL_ON_TREAP_HEALTH),
              "live block health treap check ignores live health");
    expect_true(api->check(heap, UHM_CHECK_FAIL_ON_LIVE_BLOCK_HEALTH) ==
                    UHM_ERR_CORRUPTION,
                "live block health strict check sees health");
    expect_true(api->malloc(heap, 16u) == 0,
                "live block health runtime malloc fail-fast");
    expect_ok(api->get_last_error(heap, &last_error, &platform_error),
              "live block health runtime last error query");
    expect_true(last_error == UHM_ERR_CORRUPTION,
                "live block health runtime malloc sets corruption");
    expect_ok(api->close(heap), "treap runtime close still allowed");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);

    {
        test_alloc repair_heap_mem = {0};
        test_alloc repair_arena_mem = {0};
        uhm_vm_ops repair_vm;
        uhm_config repair_config;
        uhm_heap *repair_heap = 0;
        const uhm_api *repair_api;
        void *live_large;
        void *free_large;
        void *small_after_repair;
        unsigned char *repair_map = 0;
        uhm_block *live_root;
        uhm_block *free_root;
        uhm_vma *vma_root;
        uhm_free_extent *mmap_free_root;
        uint64_t repair_rebuilds_before;
        uint64_t repair_nodes_before;

        expect_true(test_vm_reserve_commit_alloc(
                        heap_info->storage_size, heap_info->storage_alignment,
                        &repair_heap_mem) != 0,
                    "treap repair heap storage alloc");
        expect_true(test_vm_reserve_commit_alloc(2u * 1024u * 1024u,
                                                 page_size,
                                                 &repair_arena_mem) != 0,
                    "treap repair arena alloc");
        expect_ok(uhm_platform_usermode_vm_ops_init(
                      &repair_vm, page_size, page_size,
                      UHM_VM_DECOMMIT_ZERO_FILLS),
                  "treap repair usermode vm init");

        memset(&repair_config, 0, sizeof(repair_config));
        repair_config.size = sizeof(repair_config);
        repair_config.abi_major = UHM_ABI_MAJOR;
        repair_config.abi_minor = UHM_ABI_MINOR;
        repair_config.flags = UHM_CONFIG_REPAIR_TREAP_HEALTH;
        repair_config.heap_storage = repair_heap_mem.aligned;
        repair_config.heap_storage_size = heap_info->storage_size;
        repair_config.arena_base = repair_arena_mem.aligned;
        repair_config.arena_size = 2u * 1024u * 1024u;
        repair_config.arena_flags = UHM_ARENA_PRE_RESERVED;
        repair_config.vm = repair_vm;

        expect_ok(uhm_open(&repair_config, &repair_heap),
                  "treap repair heap open");
        repair_api = uhm_get_api(repair_heap);
        expect_true(repair_api != 0, "treap repair api");
        expect_true((repair_heap->state_flags &
                     UHM_CONFIG_REPAIR_TREAP_HEALTH) != 0u,
                    "treap repair config enabled");

        live_large = repair_api->malloc(repair_heap, page_size * 2u + 128u);
        free_large = repair_api->malloc(repair_heap, page_size * 3u + 128u);
        expect_true(live_large != 0 && free_large != 0,
                    "treap repair large mallocs");
        repair_api->free(repair_heap, free_large);
        expect_ok(repair_api->mmap(repair_heap, (void **)&repair_map,
                                   page_size * 3u,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                  "treap repair mmap");
        expect_ok(repair_api->munmap(repair_heap, repair_map + page_size,
                                     page_size),
                  "treap repair middle munmap");
        expect_true(repair_heap->core.live_block_tree_root != 0 &&
                        repair_heap->core.malloc_free_tree_root != 0 &&
                        repair_heap->core.vma_tree_root != 0 &&
                        repair_heap->core.mmap_free_tree_root != 0,
                    "treap repair all tree roots exist");

        live_root = repair_heap->core.live_block_tree_root;
        free_root = repair_heap->core.malloc_free_tree_root;
        vma_root = repair_heap->core.vma_tree_root;
        mmap_free_root = repair_heap->core.mmap_free_tree_root;
        live_root->live_tree_right = live_root;
        free_root->free_tree_left = free_root;
        vma_root->tree_left = vma_root;
        mmap_free_root->tree_right = mmap_free_root;
        repair_heap->core.treap_health_flags = UHM_TREAP_HEALTH_LIVE_FIND;
        atomic_store_explicit(
            &repair_heap->core.treap_repair_pending_flags,
            (uint32_t)(UHM_TREAP_HEALTH_LIVE_FIND |
                       UHM_TREAP_HEALTH_MALLOC_FIND |
                       UHM_TREAP_HEALTH_VMA_FIND |
                       UHM_TREAP_HEALTH_MMAP_FREE_FIND),
            UHM_ATOMIC_RELEASE);
        repair_rebuilds_before = repair_heap->core.treap_repair_rebuilds;
        repair_nodes_before = repair_heap->core.treap_repair_nodes;

        small_after_repair = repair_api->malloc(repair_heap, 16u);
        expect_true(small_after_repair != 0,
                    "treap repair malloc succeeds after rebuild");
        expect_true(repair_heap->core.treap_repair_rebuilds ==
                        repair_rebuilds_before + 1u,
                    "treap repair rebuild counted");
        expect_true(repair_heap->core.treap_repair_nodes >
                        repair_nodes_before,
                    "treap repair nodes counted");
        expect_true(atomic_load_explicit(
                        &repair_heap->core.treap_repair_pending_flags,
                        UHM_ATOMIC_ACQUIRE) == 0u,
                    "treap repair pending cleared");
        expect_true(repair_heap->core.treap_repair_failures == 0u,
                    "treap repair no validation failure");
        expect_ok(repair_api->check(repair_heap, 0),
                  "treap repair default check passes");
        expect_true(repair_api->check(
                        repair_heap, UHM_CHECK_FAIL_ON_TREAP_HEALTH) ==
                        UHM_ERR_CORRUPTION,
                    "treap repair sticky health remains visible");

        repair_api->free(repair_heap, small_after_repair);
        repair_api->free(repair_heap, live_large);
        expect_ok(repair_api->munmap(repair_heap, repair_map, page_size),
                  "treap repair left munmap cleanup");
        expect_ok(repair_api->munmap(repair_heap,
                                     repair_map + page_size * 2u,
                                     page_size),
                  "treap repair right munmap cleanup");
        expect_ok(repair_api->close(repair_heap), "treap repair close");
        test_vm_release_alloc(&repair_arena_mem);
        test_vm_release_alloc(&repair_heap_mem);
    }

    {
        test_alloc conflict_heap_mem = {0};
        uhm_config conflict_config;
        uhm_heap *conflict_heap = 0;

        expect_true(test_vm_reserve_commit_alloc(
                        heap_info->storage_size, heap_info->storage_alignment,
                        &conflict_heap_mem) != 0,
                    "treap strategy conflict heap storage alloc");
        memset(&conflict_config, 0, sizeof(conflict_config));
        conflict_config.size = sizeof(conflict_config);
        conflict_config.abi_major = UHM_ABI_MAJOR;
        conflict_config.abi_minor = UHM_ABI_MINOR;
        conflict_config.heap_storage = conflict_heap_mem.aligned;
        conflict_config.heap_storage_size = heap_info->storage_size;
        conflict_config.flags = UHM_CONFIG_FAIL_ON_TREAP_HEALTH |
                                UHM_CONFIG_REPAIR_TREAP_HEALTH;
        expect_true(uhm_open(&conflict_config, &conflict_heap) ==
                        UHM_ERR_INVALID_ARGUMENT,
                    "treap runtime rejects conflicting strategies");
        test_vm_release_alloc(&conflict_heap_mem);
    }
}

static void test_open_rejects_wrapping_arena_range(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "wrapping arena heap storage alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "wrapping arena usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = (void *)(UINTPTR_MAX - (uintptr_t)(page_size / 2u));
    config.arena_size = page_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "wrapping arena range rejected");
    test_vm_release_alloc(&heap_mem);
}

static void test_open_rejects_unrounded_pre_reserved_arena(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "unrounded arena heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u + page_size,
                                    page_size, &arena_mem) != 0,
                "unrounded arena backing alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "unrounded arena usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u + 1u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "pre-reserved arena size must be page rounded");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

#define TEST_SHADOW_PRESENT          (1u << 0)
#define TEST_SHADOW_EXACT_PROT       (1u << 1)
#define TEST_SHADOW_EXACT_DESC       (1u << 2)
#define TEST_SHADOW_EXACT_GENERATION (1u << 3)
#define TEST_SHADOW_FREE_FLEXIBLE    (1u << 4)

typedef struct test_shadow_page {
    uint32_t flags;
    uint32_t owner;
    uint32_t va_state;
    uint32_t commit_state;
    uint32_t prot;
    uint64_t descriptor_id;
    uint64_t generation;
} test_shadow_page;

static size_t test_min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

static uintptr_t test_align_up_uintptr(uintptr_t value, size_t alignment) {
    uintptr_t mask = (uintptr_t)alignment - 1u;
    return (value + mask) & ~mask;
}

static void *test_block_payload(uhm_block *block) {
    size_t header = test_align_up_uintptr(sizeof(uhm_block), UHM_MIN_ALIGNMENT);
    return (void *)((uintptr_t)block + header);
}

static int test_block_decommit_range(uhm_heap *heap, const uhm_block *block,
                                     uintptr_t *addr_out, size_t *size_out) {
    uintptr_t block_start;
    uintptr_t block_end;
    uintptr_t header_end;
    uintptr_t decommit_start;
    uintptr_t decommit_end;
    size_t page_size;

    if (!heap || !block || block->magic != UHM_BLOCK_MAGIC ||
        block->total_size == 0) {
        return 0;
    }
    block_start = (uintptr_t)block;
    block_end = block_start + block->total_size;
    header_end = block_start +
                 test_align_up_uintptr(sizeof(uhm_block), UHM_MIN_ALIGNMENT);
    if (block_end < block_start || header_end < block_start ||
        header_end > block_end) {
        return 0;
    }
    page_size = heap->F.vm.page_size;
    decommit_start = test_align_up_uintptr(header_end, page_size);
    decommit_end = block_end & ~((uintptr_t)page_size - 1u);
    if (decommit_end <= decommit_start) {
        return 0;
    }
    if (addr_out) {
        *addr_out = decommit_start;
    }
    if (size_out) {
        *size_out = (size_t)(decommit_end - decommit_start);
    }
    return 1;
}

