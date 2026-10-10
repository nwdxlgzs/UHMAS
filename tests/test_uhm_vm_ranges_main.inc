static void test_mmap_mremap_resize(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *same;
    uintptr_t map_addr;
    uhm_stats stats;
    uint64_t mark_pages_before;
    uint64_t elided_before;
    uint64_t elided_pages_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "mremap heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "mremap arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mremap vm init");

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

    expect_ok(uhm_open(&config, &heap), "mremap heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mremap != 0, "mremap api");

    expect_ok(api->mmap(heap, &map, page_size + 1u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mremap initial mmap");
    expect_true(map != 0, "mremap initial address");
    map_addr = (uintptr_t)map;
    memset(map, 0x21, page_size * 2u);

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "mremap initial stats");
    expect_true(stats.mmap_live_bytes == page_size * 2u,
                "mremap initial size rounded");

    same = map;
    expect_ok(api->mremap(heap, &same, page_size + 1u,
                          page_size * 2u - 17u, 0u),
              "mremap same rounded page span");
    expect_true(same == map, "mremap no-op keeps address");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "mremap no-op stats");
    expect_true(stats.mmap_live_bytes == page_size * 2u,
                "mremap no-op keeps live bytes");

    same = map;
    expect_ok(api->mremap(heap, &same, page_size + 1u,
                          page_size * 3u, UHM_REMAP_MAYMOVE),
              "mremap maymove relocates mapping");
    expect_true(same != map, "mremap maymove returns new address");
    expect_true(((unsigned char *)same)[0] == 0x21u &&
                    ((unsigned char *)same)[page_size * 2u - 1u] == 0x21u,
                "mremap maymove copies old payload");
    expect_true(api->munmap(heap, map, page_size) == UHM_ERR_NOT_MAPPED,
                "mremap maymove old address unmapped");
    expect_ok(api->munmap(heap, same, page_size * 3u),
              "mremap maymove cleanup");

    expect_ok(api->mmap(heap, &map, page_size + 1u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mremap second mmap");
    expect_true(map != 0, "mremap second address");
    map_addr = (uintptr_t)map;
    memset(map, 0x31, page_size * 2u);

    same = map;
    mark_pages_before = test_atomic_u64_load(&heap->core.page_mark_pages);
    elided_before = heap->core.munmap_vma_trim_repair_elided;
    elided_pages_before = heap->core.munmap_vma_trim_repair_elided_pages;
    expect_ok(api->mremap(heap, &same, page_size + 1u, page_size, 0u),
              "mremap shrink releases tail page");
    expect_true(same == map, "mremap shrink keeps address");
    expect_true(test_atomic_u64_load(&heap->core.page_mark_pages) ==
                    mark_pages_before + 2u,
                "mremap shrink marks protecting then frees only tail page");
    expect_true(heap->core.munmap_vma_trim_repair_elided ==
                    elided_before + 1u,
                "mremap shrink elides left remnant repair");
    expect_true(heap->core.munmap_vma_trim_repair_elided_pages ==
                    elided_pages_before + 1u,
                "mremap shrink elided left remnant page counted");
    expect_true(api->munmap(heap, (void *)(map_addr + page_size), 1u) ==
                    UHM_ERR_NOT_MAPPED,
                "mremap shrink tail page unmapped");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "mremap shrink stats");
    expect_true(stats.mmap_live_bytes == page_size,
                "mremap shrink updates live bytes");

    same = map;
    expect_ok(api->mremap(heap, &same, page_size, page_size + 1u, 0u),
              "mremap grow reclaims adjacent tail page");
    expect_true(same == map, "mremap grow keeps address");
    memset((void *)(map_addr + page_size), 0x42, page_size);
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "mremap grow stats");
    expect_true(stats.mmap_live_bytes == page_size * 2u,
                "mremap grow updates live bytes");

    expect_ok(api->munmap(heap, map, page_size + 1u), "mremap cleanup munmap");
    expect_ok(api->check(heap, 0), "mremap final check");
    expect_ok(api->close(heap), "mremap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

int main(void) {
    uhm_storage_info heap_info;
    uhm_storage_info ctx_info;
    uhm_abi_info abi_info;
    test_alloc heap_mem = {0};
    test_alloc ctx_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *p1;
    void *p2;
    void *p3;
    void *p4;
    void *p5;
    void *p6;
    void *sc1;
    void *sc17;
    void *sc49;
    void *sc4097;
    unsigned char *r1;
    void *r2;
    unsigned char *r3;
    void *r4;
    unsigned char *r_new;
    void *page_private;
    unsigned char *page_reuse;
    unsigned char *cz;
    int outside_word = 0;
    void *map = 0;
    uhm_stats stats;
    uhm_thread_config ctx_config;
    uhm_thread_ctx *ctx = 0;
    void *ctx_ptr;
    const size_t page_size = uhm_platform_system_page_size();
    uint64_t mprotect_noop_before;
    uint64_t mprotect_platform_before;
    uint64_t mprotect_publish_before;

    memset(&abi_info, 0, sizeof(abi_info));
    abi_info.size = sizeof(abi_info);
    expect_ok(uhm_abi_query(&abi_info), "abi query");
    expect_true(abi_info.abi_major == UHM_ABI_MAJOR, "abi major");
    test_public_abi_layout_and_storage_contracts();

    memset(&heap_info, 0, sizeof(heap_info));
    memset(&ctx_info, 0, sizeof(ctx_info));
    heap_info.size = sizeof(heap_info);
    ctx_info.size = sizeof(ctx_info);
    expect_ok(uhm_heap_storage_query(&heap_info, &ctx_info), "storage query");
    expect_true(heap_info.storage_size > 0, "heap storage size");
    expect_true(ctx_info.storage_size > 0, "ctx storage size");
    test_internal_cacheline_layout();
    test_internal_metadata_footprint_budget();
    test_stats_tail_capability_status(&heap_info);
    test_spinlock_policy_config(&heap_info);
    test_treap_runtime_fail_fast_config(&heap_info);
    test_open_rejects_wrapping_arena_range(&heap_info);
    test_open_rejects_unrounded_pre_reserved_arena(&heap_info);
    test_tagged_freelist_basic_overflow_and_contention();
    test_open_failure_releases_reserved_arena(&heap_info);
    test_vm_zero_page_size_auto_detects_system(&heap_info);
    test_tls_config_and_mmap_ex_numa(&heap_info);
    test_tls_cache_reuse_adapt_and_heap_close_flush(&heap_info);
    test_tls_cache_page_hint_set_associativity(&heap_info);
    test_tls_cache_partial_lowlock(&heap_info);
    test_tls_cache_thread_exit_flush_and_registry_pressure(&heap_info);
    test_tls_cache_heap_close_flushes_foreign_thread_cache(&heap_info);
    test_heap_close_batches_tls_cache_global_maintenance(&heap_info);
    test_numa_first_touch_preserves_uninitialized_payload(&heap_info);
    test_numa_policy_flag_fuzz_long_run(&heap_info);
    test_vm_failure_rollbacks(&heap_info);
    test_fault_injection_rules(&heap_info);
    test_mmap_publish_failure_rolls_back_commit(&heap_info);
    test_metadata_allocation_failures(&heap_info);
    test_exec_and_wx_capability_gates(&heap_info);
    test_collision_exhaustion(&heap_info);
    test_malloc_mmap_bidirectional_collision_pressure(&heap_info);
    test_near_address_space_exhaustion_long_run(&heap_info);
    test_control_extent_expansion(&heap_info);
    test_control_extent_concurrent_competition(&heap_info);
    test_page_map_leaf_concurrent_creation(&heap_info);
    test_page_map_leaf_single_shard_hotspot(&heap_info);
    test_page_map_leaf_spare_reuse(&heap_info);
    test_emergency_reserve_allows_release_after_metadata_exhaustion(&heap_info);
    test_metadata_quarantine_retired_nodes(&heap_info);
    test_metadata_quarantine_skips_empty_drain(&heap_info);
    test_metadata_quarantine_inflation_pressure(&heap_info);
    test_metadata_hot_cache_capacity_limit(&heap_info);
    test_metadata_hot_cache_drain_high_water_limit(&heap_info);
    test_metadata_hot_cache_batch_return_invariants(&heap_info);
    test_metadata_hot_cache_pending_high_water_budget(&heap_info);
    test_metadata_quarantine_rotates_drain_start(&heap_info);
    test_metadata_quarantine_rotates_blocked_head(&heap_info);
    test_metadata_quarantine_budgeted_drain(&heap_info);
    test_metadata_quarantine_emergency_chain_diagnostics(&heap_info);
    test_metadata_quarantine_retired_tail_corruption_is_not_repaired(&heap_info);
    test_metadata_quarantine_prioritizes_emergency(&heap_info);
    test_metadata_hazard_defers_emergency_reuse(&heap_info);
    test_metadata_remote_drain_owner_defers_emergency_reuse(&heap_info);
    test_metadata_ctx_reader_defers_emergency_reuse(&heap_info, &ctx_info);
    test_metadata_reader_state_diagnostics(&heap_info, &ctx_info);
    test_leak_enumeration(&heap_info);
    test_leak_enumeration_uses_live_tree(&heap_info);
    test_malloc_physical_chain_check_guard(&heap_info);
    test_slab_state_transitions(&heap_info);
    test_slab_multi_state_lists(&heap_info);
    test_slab_empty_decommit_failure_keeps_committed(&heap_info);
    test_slab_empty_decommit_high_water_pressure(&heap_info);
    test_slab_empty_adaptive_high_water_after_recommit(&heap_info);
    test_slab_decommit_platform_unlock(&heap_info);
    test_slab_empty_contiguous_batch_decommit(&heap_info, &ctx_info);
    test_slab_empty_decommit_prefers_largest_run(&heap_info, &ctx_info);
    test_slab_empty_decommit_candidate_skips_decommitted(&heap_info,
                                                         &ctx_info);
    test_slab_recommit_platform_unlock(&heap_info);
    test_slab_new_commit_platform_unlock(&heap_info);
    test_slab_remote_queue_drain(&heap_info);
    test_slab_32bit_partial_low_lock_remote_free(&heap_info);
    test_slab_32bit_raw_ready_after_direct_decommit(&heap_info);
    test_slab_32bit_raw_ready_late_notification(&heap_info);
    test_slab_32bit_raw_ready_concurrent_reuse(&heap_info);
    test_slab_32bit_raw_batch(&heap_info);
    test_slab_32bit_raw_single_counter_transaction(&heap_info);
    test_slab_32bit_raw_batch_concurrent_reuse(&heap_info);
    test_slab_remote_pending_mask_skips_empty_class_scan(&heap_info);
    test_slab_remote_tagged_close_corruption_restores(&heap_info);
    test_slab_accounted_remote_lockless_partial_reuse(&heap_info);
    test_slab_remote_ready_queue_diagnostics(&heap_info);
    test_slab_accounted_remote_lockless_full_to_partial(&heap_info);
    test_slab_accounted_remote_lockless_partial_to_empty(&heap_info);
    test_slab_remote_drain_reservation_rollback(&heap_info);
    test_slab_remote_drain_reservation_skips_unreserved_fallback(&heap_info);
    test_slab_remote_drain_reservation_reader_busy_defers(&heap_info);
    test_slab_remote_drain_decommit_trylock_busy_defers(&heap_info);
    test_slab_remote_queue_tag_wrap_reader_gate(&heap_info);
    test_slab_remote_tagged_batch_publish(&heap_info);
    test_slab_remote_fallback_list_batch_publish(&heap_info);
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
    if (test_lockless_remote_primitives_supported()) {
        test_slab_lockless_remote_free_unaccounted(&heap_info);
        test_slab_lockless_class_queue_close_batch_publish(&heap_info);
        test_slab_lockless_class_queue_owned_arena_releases_without_decommit(
            &heap_info);
        test_slab_lockless_class_queue_close_tag_wrap_self_reader(&heap_info);
        test_slab_lockless_class_queue_close_publish_busy_restores(&heap_info);
        test_slab_lockless_class_queue_close_corruption_restores(&heap_info);
        test_slab_lockless_remote_scan_mismatch(&heap_info);
        test_slab_lockless_remote_tag_wrap_reader_gate(&heap_info);
        test_slab_lockless_remote_hazard_gate(&heap_info);
        test_slab_lockless_remote_class_publish_wrap_busy_fallback(&heap_info);
        test_slab_lockless_remote_publish_rollback(&heap_info);
        test_slab_lockless_remote_alloc_rollback(&heap_info);
        test_slab_lockless_remote_corruption_stops_fallback(&heap_info);
        test_slab_lockless_remote_concurrent_reuse(&heap_info);
        test_thread_ctx_flush_skips_lockless_class_queue(&heap_info,
                                                         &ctx_info);
        test_tls_flush_skips_lockless_class_queue(&heap_info);
    } else {
    test_slab_lockless_remote_unsupported_fallback(&heap_info);
    test_slab_low_lock_fallback_boundaries(&heap_info);
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    test_slab_decommit_defers_while_metadata_reader_active(&heap_info);
    test_lockless_remote_reader_slots_gate_lifecycle_and_decommit(&heap_info);
    test_slab_slot_atomic_state_mirror_check(&heap_info);
    test_thread_ctx_close_flushes_remote_queue(&heap_info, &ctx_info);
    test_heap_close_invalidates_active_thread_ctx(&heap_info, &ctx_info);
    test_heap_close_batches_thread_ctx_global_maintenance(&heap_info,
                                                          &ctx_info);
    test_heap_close_batches_tls_and_ctx_global_maintenance(&heap_info,
                                                           &ctx_info);
    test_heap_close_class_queue_retry_after_cache_maintenance(&heap_info,
                                                              &ctx_info);
    test_heap_close_rejects_new_ops_during_active_mmap(&heap_info);
    test_large_malloc_extent_decommit_recommit(&heap_info);
    test_large_malloc_decommit_platform_unlock(&heap_info);
    test_large_malloc_recommit_platform_unlock(&heap_info);
    test_large_malloc_realloc_recommit_platform_unlock(&heap_info);
    test_large_malloc_coalesce_recommit_platform_unlock(&heap_info);
    test_large_malloc_mixed_decommit_batch_platform_unlock(&heap_info);
    test_large_malloc_new_commit_platform_unlock(&heap_info);
    test_large_malloc_cold_split_prefix_recommit(&heap_info);
    test_large_malloc_hot_split_remainder_retained(&heap_info);
    test_large_malloc_small_extent_decommit_deferred(&heap_info);
    test_large_malloc_retention_high_water_pressure(&heap_info);
    test_large_malloc_adaptive_hot_retention_high_water(&heap_info);
    test_large_malloc_hot_retention_above_base_retained(&heap_info);
    test_large_malloc_hot_retention_over_max_decommits(&heap_info);
    test_large_malloc_decommit_retention_pages_config(&heap_info);
    test_config_abi7_tail_compat_uses_default_retention(&heap_info);
    test_live_block_lookup_diagnostics(&heap_info);
    test_live_block_health_fail_fast_skips_physical_fallback(&heap_info);
    test_malloc_block_priority_lifecycle_lazy_init(&heap_info);
    test_live_block_realloc_split_merge_fast_lookup(&heap_info);
    test_live_block_realloc_move_failure_keeps_old_fast_lookup(&heap_info);
    test_live_block_realloc_move_success_reuse_fast_lookup(&heap_info);
    test_live_block_generation_drift_no_physical_fallback(&heap_info);
    test_page_private_page_map_generation_drift_fast_lookup(&heap_info);
    test_live_block_recommit_reuse_fast_lookup(&heap_info);
    test_live_block_recommit_failure_rollback_fast_lookup(&heap_info);
    test_live_block_decommit_failure_committed_reuse_fast_lookup(&heap_info);
    test_malloc_free_tree_remove_two_child_sink_covered(&heap_info);
    test_malloc_free_tree_ordered_same_size_pressure(&heap_info);
    test_multithread_small_object_churn(&heap_info);
    test_multithread_large_realloc(&heap_info);
    test_cross_thread_free(&heap_info);
    test_ctx_open_close_concurrent_alloc(&heap_info, &ctx_info);
    test_thread_ctx_local_cache_flush(&heap_info, &ctx_info);
    test_thread_ctx_retained_page_rotation(&heap_info, &ctx_info);
    test_thread_ctx_local_cache_partial_lowlock(&heap_info, &ctx_info);
    test_thread_ctx_deferred_free_flush(&heap_info, &ctx_info);
    test_mmap_fixed_munmap_mprotect_mix(&heap_info);
    test_vma_tree_ordered_fixed_mmap_pressure(&heap_info);
    test_mmap_free_tree_ordered_munmap_pressure(&heap_info);
    test_mmap_free_recent_extent_reuse(&heap_info);
    test_mmap_free_no_fit_skips_hot_cache(&heap_info);
    test_mmap_size_hint_bucket_max_skips_too_small_bucket(&heap_info);
    test_mmap_page_rounding_and_owner_isolation(&heap_info);
    test_page_range_budget_diagnostics(&heap_info);
    test_mmap_commit_numa_platform_unlock(&heap_info);
    test_mmap_fixed_exact_replace_platform_unlock(&heap_info);
    test_mmap_fixed_partial_replace_platform_unlock(&heap_info);
    test_mremap_grow_commit_numa_platform_unlock(&heap_info);
    test_mremap_shrink_decommit_platform_unlock(&heap_info);
    test_mremap_shrink_extends_adjacent_free_extent(&heap_info);
    test_mremap_maymove_source_claim(&heap_info);
    test_mremap_maymove_restore_failure_rollback(&heap_info);
    test_mprotect_exact_vma_elides_live_rescan(&heap_info);
    test_mprotect_page_private_elides_live_rescan(&heap_info);
    test_mprotect_exact_vma_platform_unlock(&heap_info);
    test_mprotect_partial_vma_platform_unlock(&heap_info);
    test_mprotect_page_private_platform_unlock(&heap_info);
    test_page_private_restore_rw_platform_unlock(&heap_info);
    test_mprotect_cross_vma_partial_platform_unlock(&heap_info);
    test_mprotect_no_commit_skips_platform_protect(&heap_info);
    test_munmap_exact_vma_platform_unlock(&heap_info);
    test_munmap_exact_vma_skips_publish_ready_precheck(&heap_info);
    test_munmap_trim_vma_platform_unlock(&heap_info);
    test_munmap_middle_vma_platform_unlock(&heap_info);
    test_munmap_cross_vma_exact_platform_unlock(&heap_info);
    test_munmap_cross_vma_partial_platform_unlock(&heap_info);
    test_munmap_no_commit_skips_platform_decommit(&heap_info);
    test_munmap_adjacent_free_extent_skips_node_alloc(&heap_info);
    test_munmap_rechecks_adjacent_after_unlock(&heap_info);
    test_munmap_separated_live_islands_decommit_batches(&heap_info);
    test_munmap_separated_live_island_failure_rolls_back(&heap_info);
    test_protecting_vma_split_barrier(&heap_info);
    test_mprotect_vma_pass_diagnostics(&heap_info);
    test_munmap_trim_elides_remnant_page_repair(&heap_info);
    test_mprotect_restore_platform_batches_same_old_prot(&heap_info);
    test_mprotect_vma_pass_rollback(&heap_info);
    test_mmap_mremap_resize(&heap_info);
    test_mprotect_cross_vma_merge(&heap_info);
    test_mprotect_three_vma_merge_batches_page_mark(&heap_info);
    test_mprotect_partial_edge_merge_uses_tree_neighbors(&heap_info);
    test_mprotect_merged_vma_alternating_subranges(&heap_info);
    test_usermode_decommit_and_fallback_modes(&heap_info);

    expect_true(test_vm_reserve_commit_alloc(heap_info.storage_size, heap_info.storage_alignment, &heap_mem) != 0,
                "heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(ctx_info.storage_size, ctx_info.storage_alignment, &ctx_mem) != 0,
                "ctx storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size, &arena_mem) != 0,
                "arena alloc");

    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info.storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "uhm_open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "api table");
    expect_true(api->malloc != 0 && api->mmap != 0 && api->mmap_ex != 0 &&
                    api->mremap != 0 &&
                    api->thread_ctx_flush != 0 &&
                    api->free_deferred_ctx != 0 && api->close != 0,
                "api slots");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "size class stats before");
    sc1 = api->malloc(heap, 1u);
    sc17 = api->malloc(heap, 17u);
    sc49 = api->malloc(heap, 49u);
    sc4097 = api->malloc(heap, 4097u);
    expect_true(sc1 != 0 && sc17 != 0 && sc49 != 0 && sc4097 != 0,
                "size class allocations");
    expect_true(heap->core.slab_all_head != 0, "small allocations create slab");
    expect_true(api->malloc_usable_size(heap, sc1) == 16u, "size class 1 -> 16");
    expect_true(api->malloc_usable_size(heap, sc17) == 32u, "size class 17 -> 32");
    expect_true(api->malloc_usable_size(heap, sc49) == 64u, "size class 49 -> 64");
    expect_true(api->malloc_usable_size(heap, sc4097) == 4112u, "large malloc rounds to 16");
    {
        uhm_stats after_size_class;
        memset(&after_size_class, 0, sizeof(after_size_class));
        after_size_class.size = sizeof(after_size_class);
        expect_ok(api->get_stats(heap, &after_size_class), "size class stats after");
        expect_true(after_size_class.malloc_live_bytes ==
                        stats.malloc_live_bytes + 1u + 17u + 49u + 4097u,
                    "size class stats use requested bytes");
    }
    api->free(heap, sc1);
    api->free(heap, sc17);
    api->free(heap, sc49);
    api->free(heap, sc4097);

    p1 = api->malloc(heap, 24u);
    p2 = api->aligned_alloc(heap, 64u, 80u);
    expect_true(p1 != 0, "malloc p1");
    expect_true(p2 != 0, "malloc p2");
    expect_true(((uintptr_t)p1 % 16u) == 0, "p1 16-byte aligned");
    expect_true(((uintptr_t)p2 % 64u) == 0, "p2 64-byte aligned");
    memset(p1, 0x11, 24u);
    memset(p2, 0x22, 80u);
    cz = (unsigned char *)api->calloc(heap, 8u, 8u);
    expect_true(cz != 0, "calloc");
    for (size_t i = 0; i < 64u; ++i) {
        expect_true(cz[i] == 0, "calloc zero fill");
    }
    api->free(heap, cz);
    expect_true(api->calloc(heap, ((size_t)-1) / 2u + 1u, 2u) == 0, "calloc overflow");

    api->free(heap, p1);
    p3 = api->malloc(heap, 24u);
    expect_true(p3 == p1, "slab malloc reuses same size-class slot");
    api->free(heap, (void *)((uintptr_t)p2 + 1u));
    api->free(heap, &outside_word);
    expect_ok(api->check(heap, 0), "invalid frees leave heap consistent");
    p4 = api->malloc(heap, 1024u);
    p5 = api->malloc(heap, 1024u);
    expect_true(p4 != 0 && p5 != 0, "coalesce allocations");
    api->free(heap, p4);
    api->free(heap, p4);
    expect_ok(api->check(heap, 0), "double free leaves heap consistent");
    api->free(heap, p5);
    p6 = api->malloc(heap, 1500u);
    expect_true(p6 != 0, "coalesced free block reuse");

    r1 = (unsigned char *)api->malloc(heap, 640u);
    r2 = api->malloc(heap, 768u);
    expect_true(r1 != 0 && r2 != 0, "realloc in-place setup");
    for (size_t i = 0; i < 64u; ++i) {
        r1[i] = (unsigned char)(i + 1u);
    }
    api->free(heap, r2);
    r_new = (unsigned char *)api->realloc(heap, r1, 1200u);
    expect_true(r_new == r1, "realloc expands into adjacent free block");
    for (size_t i = 0; i < 64u; ++i) {
        expect_true(r_new[i] == (unsigned char)(i + 1u), "realloc in-place preserves data");
    }
    r1 = r_new;

    r3 = (unsigned char *)api->malloc(heap, 64u);
    r4 = api->malloc(heap, 64u);
    expect_true(r3 != 0 && r4 != 0, "realloc move setup");
    for (size_t i = 0; i < 64u; ++i) {
        r3[i] = (unsigned char)(0xa0u + i);
    }
    r_new = (unsigned char *)api->realloc(heap, r3, page_size);
    expect_true(r_new != 0, "realloc move result");
    expect_true(r_new != r3, "realloc moves when adjacent block is live");
    for (size_t i = 0; i < 64u; ++i) {
        expect_true(r_new[i] == (unsigned char)(0xa0u + i), "realloc move preserves data");
    }
    r3 = r_new;

    expect_true(api->mprotect(heap, arena_mem.aligned, page_size, UHM_PROT_READ) ==
                    UHM_ERR_PROTECTION,
                "control page mprotect rejected");
    expect_true(api->mprotect(heap,
                              (void *)((uintptr_t)p2 &
                                       ~((uintptr_t)page_size - 1u)),
                              page_size,
                              UHM_PROT_READ) == UHM_ERR_UNSUPPORTED,
                "malloc page mprotect unsupported");
    page_private = api->aligned_alloc(heap, page_size, 2u * page_size);
    expect_true(page_private != 0, "page-private malloc alloc");
    expect_true(((uintptr_t)page_private % page_size) == 0,
                "page-private malloc page aligned");
    mprotect_platform_before = heap->core.mprotect_platform_calls;
    mprotect_publish_before = heap->core.mprotect_publish_calls;
    expect_ok(api->mprotect(heap, page_private, page_size, UHM_PROT_READ),
              "page-private malloc first page mprotect");
    expect_true(heap->core.mprotect_platform_calls ==
                    mprotect_platform_before + 1u,
                "page-private first mprotect uses platform");
    expect_true(heap->core.mprotect_publish_calls ==
                    mprotect_publish_before + 1u,
                "page-private first mprotect publishes");
    mprotect_noop_before = heap->core.mprotect_page_private_noop_calls;
    mprotect_platform_before = heap->core.mprotect_platform_calls;
    expect_ok(api->mprotect(heap, page_private, page_size, UHM_PROT_READ),
              "page-private malloc first page mprotect no-op");
    expect_true(heap->core.mprotect_page_private_noop_calls ==
                    mprotect_noop_before + 1u,
                "page-private first page no-op counted");
    expect_true(heap->core.mprotect_platform_calls == mprotect_platform_before,
                "page-private first page no-op skips platform");
    expect_ok(api->mprotect(heap, (void *)((uintptr_t)page_private + page_size),
                            page_size,
                            UHM_PROT_NONE),
              "page-private malloc second page mprotect");
    mprotect_noop_before = heap->core.mprotect_page_private_noop_calls;
    mprotect_platform_before = heap->core.mprotect_platform_calls;
    expect_ok(api->mprotect(heap, (void *)((uintptr_t)page_private + page_size),
                            page_size,
                            UHM_PROT_NONE),
              "page-private malloc second page mprotect no-op");
    expect_true(heap->core.mprotect_page_private_noop_calls ==
                    mprotect_noop_before + 1u,
                "page-private second page no-op counted");
    expect_true(heap->core.mprotect_platform_calls == mprotect_platform_before,
                "page-private second page no-op skips platform");
    api->free(heap, page_private);
    page_reuse = (unsigned char *)api->malloc(heap, 2u * page_size);
    expect_true(page_reuse == page_private, "page-private free restores and reuses block");
    memset(page_reuse, 0x5bu, 2u * page_size);
    api->free(heap, page_reuse);

    expect_ok(api->mmap(heap, &map, 2u * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mmap auto");
    expect_true(map != 0, "mmap address");
    expect_true(((uintptr_t)map % page_size) == 0, "mmap page aligned");
    {
        void *fixed_conflict = map;
        expect_true(api->mmap(heap, &fixed_conflict, page_size,
                              UHM_PROT_READ | UHM_PROT_WRITE,
                              UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                        UHM_ERR_ALREADY_MAPPED,
                    "fixed noreplace conflict");
    }
    mprotect_platform_before = heap->core.mprotect_platform_calls;
    mprotect_publish_before = heap->core.mprotect_publish_calls;
    expect_ok(api->mprotect(heap, map, page_size, UHM_PROT_READ), "mprotect");
    expect_true(heap->core.mprotect_platform_calls ==
                    mprotect_platform_before + 1u,
                "mmap mprotect uses platform");
    expect_true(heap->core.mprotect_publish_calls ==
                    mprotect_publish_before + 1u,
                "mmap mprotect publishes");
    mprotect_noop_before = heap->core.mprotect_mmap_noop_calls;
    mprotect_platform_before = heap->core.mprotect_platform_calls;
    expect_ok(api->mprotect(heap, map, page_size, UHM_PROT_READ),
              "mmap mprotect no-op");
    expect_true(heap->core.mprotect_mmap_noop_calls ==
                    mprotect_noop_before + 1u,
                "mmap mprotect no-op counted");
    expect_true(heap->core.mprotect_platform_calls == mprotect_platform_before,
                "mmap mprotect no-op skips platform");
    expect_ok(api->munmap(heap, map, 2u * page_size), "munmap");
    expect_true(api->mprotect(heap, map, page_size, UHM_PROT_READ) == UHM_ERR_NOT_MAPPED,
                "mprotect after munmap rejects free page");
    expect_true(api->munmap(heap, map, page_size) == UHM_ERR_NOT_MAPPED,
                "double munmap rejects free page");
    {
        void *fixed_reuse = map;
        expect_ok(api->mmap(heap, &fixed_reuse, 2u * page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
                  "fixed noreplace reuse");
        expect_true(fixed_reuse == map, "fixed noreplace same address");
        expect_ok(api->munmap(heap, fixed_reuse, 2u * page_size),
                  "fixed noreplace munmap");
    }
    {
        void *replace_map = 0;
        void *replace_middle;
        expect_ok(api->mmap(heap, &replace_map, 3u * page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE),
                  "fixed replace source mmap");
        replace_middle = (void *)((uintptr_t)replace_map + page_size);
        expect_ok(api->mmap(heap, &replace_middle, page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED),
                  "fixed replace middle page");
        expect_true(replace_middle == (void *)((uintptr_t)replace_map + page_size),
                    "fixed replace same address");
        expect_ok(api->munmap(heap, replace_map, 3u * page_size),
                  "fixed replace cleanup");
    }

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_mem.aligned;
    ctx_config.storage_size = ctx_info.storage_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx), "ctx open");
    expect_ctx_cross_thread_rejected(api, heap, ctx);
    ctx_ptr = api->malloc_ctx(heap, ctx, 32u);
    expect_true(ctx_ptr != 0, "ctx malloc");
    api->free_ctx(heap, ctx, ctx_ptr);
    api->thread_ctx_close(heap, ctx);

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "get stats");
    expect_true(stats.arena_size == 1024u * 1024u, "stats arena");
    expect_ok(api->check(heap, 0), "check");
    test_shadow_model_check(heap, "main shadow with live allocations");

    api->free(heap, p2);
    api->free(heap, p3);
    api->free(heap, p6);
    api->free(heap, r1);
    api->free(heap, r3);
    api->free(heap, r4);
    test_shadow_model_check(heap, "main shadow after malloc cleanup");
    expect_ok(api->close(heap), "close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&ctx_mem);
    test_vm_release_alloc(&heap_mem);
    return 0;
}
