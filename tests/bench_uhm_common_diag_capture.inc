static void bench_diag_capture(bench_heap_env *env,
                               bench_diag_snapshot *snapshot) {
    uhm_heap_core *core;
    uhm_spinlock *heap_lock;
    size_t i;
#if UHMAS_ENABLE_DIAGNOSTICS
    size_t j;
#endif

    memset(snapshot, 0, sizeof(*snapshot));
    if (!env || !env->heap) {
        return;
    }
    core = &env->heap->core;
    heap_lock = &env->heap->lock;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->treap_priority_mix_calls = core->treap_priority_mix_calls;
    snapshot->treap_priority_mix_ns = core->treap_priority_mix_ns;
    snapshot->treap_priority_mix_max_ns = core->treap_priority_mix_max_ns;
#endif
    snapshot->treap_health_flags = core->treap_health_flags;
    snapshot->live_block_health_flags = core->live_block_health_flags;
    snapshot->treap_repair_pending_flags = atomic_load_explicit(
        &core->treap_repair_pending_flags, UHM_ATOMIC_ACQUIRE);
    snapshot->treap_repair_rebuilds = core->treap_repair_rebuilds;
    snapshot->treap_repair_rehashes = core->treap_repair_rehashes;
    snapshot->treap_repair_nodes = core->treap_repair_nodes;
    snapshot->treap_repair_failures = core->treap_repair_failures;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->live_block_fast_hits = core->live_block_fast_hits;
    snapshot->live_block_slow_calls = core->live_block_slow_calls;
    snapshot->live_block_slow_hits = core->live_block_slow_hits;
    snapshot->live_block_slow_misses = core->live_block_slow_misses;
    snapshot->live_block_slow_scanned_nodes =
        core->live_block_slow_scanned_nodes;
    snapshot->live_block_slow_max_scanned_nodes =
        core->live_block_slow_max_scanned_nodes;
    snapshot->live_block_slow_ns = core->live_block_slow_ns;
    snapshot->live_block_slow_max_ns = core->live_block_slow_max_ns;
    snapshot->live_block_fast_fail_invalid =
        core->live_block_fast_fail_invalid;
    snapshot->live_block_fast_fail_range = core->live_block_fast_fail_range;
    snapshot->live_block_fast_fail_header_page =
        core->live_block_fast_fail_header_page;
    snapshot->live_block_fast_fail_payload_page =
        core->live_block_fast_fail_payload_page;
    snapshot->live_block_fast_fail_descriptor =
        core->live_block_fast_fail_descriptor;
    snapshot->live_block_fast_terminal_misses =
        core->live_block_fast_terminal_misses;
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->live_block_tree_insertions = core->live_block_tree_insertions;
    snapshot->live_block_tree_rotations = core->live_block_tree_rotations;
    snapshot->live_block_tree_priority_ties =
        core->live_block_tree_priority_ties;
    snapshot->live_block_tree_priority_bucket_mask =
        core->live_block_tree_priority_bucket_mask;
    snapshot->live_block_tree_priority_bucket_max =
        core->live_block_tree_priority_bucket_max;
    snapshot->live_block_tree_priority_mix_calls =
        core->live_block_tree_priority_mix_calls;
    snapshot->live_block_tree_priority_mix_ns =
        core->live_block_tree_priority_mix_ns;
    snapshot->live_block_tree_priority_mix_max_ns =
        core->live_block_tree_priority_mix_max_ns;
    snapshot->live_block_tree_max_insert_depth =
        core->live_block_tree_max_insert_depth;
#endif
    snapshot->live_block_tree_tracked_nodes =
        core->live_block_tree_current_nodes;
    snapshot->live_block_tree_max_nodes = core->live_block_tree_max_nodes;
    snapshot->live_block_tree_depth_limit_exceeded =
        core->live_block_tree_depth_limit_exceeded;
    snapshot->live_block_tree_insert_budget_exceeded =
        core->live_block_tree_insert_budget_exceeded;
    snapshot->live_block_tree_insert_rotation_budget_exceeded =
        core->live_block_tree_insert_rotation_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->live_block_tree_find_calls =
        core->live_block_tree_find_calls;
    snapshot->live_block_tree_find_steps =
        core->live_block_tree_find_steps;
    snapshot->live_block_tree_max_find_steps =
        core->live_block_tree_max_find_steps;
#endif
    snapshot->live_block_tree_find_budget_exceeded =
        core->live_block_tree_find_budget_exceeded;
    snapshot->live_block_tree_mutation_epoch =
        core->live_block_tree_mutation_epoch;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->live_block_tree_remove_calls =
        core->live_block_tree_remove_calls;
    snapshot->live_block_tree_remove_sink_steps =
        core->live_block_tree_remove_sink_steps;
    snapshot->live_block_tree_max_remove_sink_steps =
        core->live_block_tree_max_remove_sink_steps;
#endif
    snapshot->live_block_tree_remove_budget_exceeded =
        core->live_block_tree_remove_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->live_block_slow_tree_hits = core->live_block_slow_tree_hits;
    snapshot->live_block_slow_tree_misses =
        core->live_block_slow_tree_misses;
    snapshot->live_block_slow_fallback_scans =
        core->live_block_slow_fallback_scans;
    snapshot->live_block_slow_fallback_scanned_nodes =
        core->live_block_slow_fallback_scanned_nodes;
    snapshot->live_block_slow_fallback_max_scanned_nodes =
        core->live_block_slow_fallback_max_scanned_nodes;
    snapshot->live_block_slow_fallback_budget_exceeded =
        core->live_block_slow_fallback_budget_exceeded;
    snapshot->live_block_slow_fallback_aborts =
        core->live_block_slow_fallback_aborts;
    snapshot->live_block_slow_fallback_negative_stores =
        core->live_block_slow_fallback_negative_stores;
    snapshot->live_block_slow_fallback_negative_hits =
        core->live_block_slow_fallback_negative_hits;
    snapshot->live_block_slow_fallback_negative_evictions =
        core->live_block_slow_fallback_negative_evictions;
#endif
    snapshot->live_block_slow_fallback_throttle_epoch =
        core->live_block_slow_fallback_throttle_epoch;
    snapshot->live_block_slow_fallback_epoch_scans =
        core->live_block_slow_fallback_epoch_scans;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->live_block_slow_fallback_throttles =
        core->live_block_slow_fallback_throttles;
#endif
    snapshot->live_block_slow_fallback_scenario_epoch =
        core->live_block_slow_fallback_scenario_epoch;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->live_block_slow_fallback_scenario_checks =
        core->live_block_slow_fallback_scenario_checks;
    snapshot->live_block_slow_fallback_scenario_throttles =
        core->live_block_slow_fallback_scenario_throttles;
#endif
    for (i = 0; i < UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT; ++i) {
#if UHMAS_ENABLE_DIAGNOSTICS
        snapshot->live_block_lookup_calls[i] =
            core->live_block_lookup_calls[i];
        snapshot->live_block_lookup_slow_calls[i] =
            core->live_block_lookup_slow_calls[i];
        snapshot->live_block_lookup_fallback_scans[i] =
            core->live_block_lookup_fallback_scans[i];
        snapshot->live_block_lookup_throttles[i] =
            core->live_block_lookup_throttles[i];
#endif
        snapshot->live_block_lookup_scenario_calls[i] =
            core->live_block_slow_fallback_scenario_calls[i];
        snapshot->live_block_lookup_scenario_candidates[i] =
            core->live_block_slow_fallback_scenario_candidates[i];
#if UHMAS_ENABLE_DIAGNOSTICS
        for (j = 0; j < UHM_LIVE_BLOCK_FAST_FAIL_REASON_COUNT; ++j) {
            snapshot->live_block_lookup_fast_fail_reasons[i][j] =
                core->live_block_lookup_fast_fail_reasons[i][j];
        }
#endif
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->malloc_free_tree_insertions =
        core->malloc_free_tree_insertions;
    snapshot->malloc_free_tree_rotations = core->malloc_free_tree_rotations;
    snapshot->malloc_free_tree_priority_ties =
        core->malloc_free_tree_priority_ties;
    snapshot->malloc_free_tree_priority_bucket_mask =
        core->malloc_free_tree_priority_bucket_mask;
    snapshot->malloc_free_tree_priority_bucket_max =
        core->malloc_free_tree_priority_bucket_max;
    snapshot->malloc_free_tree_priority_mix_calls =
        core->malloc_free_tree_priority_mix_calls;
    snapshot->malloc_free_tree_priority_mix_ns =
        core->malloc_free_tree_priority_mix_ns;
    snapshot->malloc_free_tree_priority_mix_max_ns =
        core->malloc_free_tree_priority_mix_max_ns;
    snapshot->malloc_free_tree_max_insert_depth =
        core->malloc_free_tree_max_insert_depth;
#endif
    snapshot->malloc_free_tree_tracked_nodes =
        core->malloc_free_tree_current_nodes;
    snapshot->malloc_free_tree_max_nodes = core->malloc_free_tree_max_nodes;
    snapshot->malloc_free_tree_depth_limit_exceeded =
        core->malloc_free_tree_depth_limit_exceeded;
    snapshot->malloc_free_tree_insert_budget_exceeded =
        core->malloc_free_tree_insert_budget_exceeded;
    snapshot->malloc_free_tree_insert_rotation_budget_exceeded =
        core->malloc_free_tree_insert_rotation_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->malloc_free_tree_find_calls =
        core->malloc_free_tree_find_calls;
    snapshot->malloc_free_tree_find_steps =
        core->malloc_free_tree_find_steps;
    snapshot->malloc_free_tree_max_find_steps =
        core->malloc_free_tree_max_find_steps;
#endif
    snapshot->malloc_free_tree_find_budget_exceeded =
        core->malloc_free_tree_find_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->malloc_free_tree_remove_calls =
        core->malloc_free_tree_remove_calls;
    snapshot->malloc_free_tree_remove_sink_steps =
        core->malloc_free_tree_remove_sink_steps;
    snapshot->malloc_free_tree_max_remove_sink_steps =
        core->malloc_free_tree_max_remove_sink_steps;
#endif
    snapshot->malloc_free_tree_remove_budget_exceeded =
        core->malloc_free_tree_remove_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->vma_tree_insertions = core->vma_tree_insertions;
    snapshot->vma_tree_rotations = core->vma_tree_rotations;
    snapshot->vma_tree_priority_ties = core->vma_tree_priority_ties;
    snapshot->vma_tree_priority_bucket_mask =
        core->vma_tree_priority_bucket_mask;
    snapshot->vma_tree_priority_bucket_max =
        core->vma_tree_priority_bucket_max;
    snapshot->vma_tree_priority_mix_calls =
        core->vma_tree_priority_mix_calls;
    snapshot->vma_tree_priority_mix_ns = core->vma_tree_priority_mix_ns;
    snapshot->vma_tree_priority_mix_max_ns =
        core->vma_tree_priority_mix_max_ns;
    core->vma_tree_priority_mix_max_ns = 0u;
    snapshot->vma_tree_max_insert_depth = core->vma_tree_max_insert_depth;
    core->vma_tree_max_insert_depth = 0u;
#endif
    snapshot->vma_tree_tracked_nodes = core->vma_tree_current_nodes;
    snapshot->vma_tree_max_nodes = core->vma_tree_max_nodes;
    snapshot->vma_tree_depth_limit_exceeded =
        core->vma_tree_depth_limit_exceeded;
    snapshot->vma_tree_insert_budget_exceeded =
        core->vma_tree_insert_budget_exceeded;
    snapshot->vma_tree_insert_rotation_budget_exceeded =
        core->vma_tree_insert_rotation_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->vma_tree_find_calls = core->vma_tree_find_calls;
    snapshot->vma_tree_find_steps = core->vma_tree_find_steps;
    snapshot->vma_tree_max_find_steps = core->vma_tree_max_find_steps;
    core->vma_tree_max_find_steps = 0u;
#endif
    snapshot->vma_tree_find_budget_exceeded =
        core->vma_tree_find_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->vma_tree_remove_calls = core->vma_tree_remove_calls;
    snapshot->vma_tree_remove_sink_steps =
        core->vma_tree_remove_sink_steps;
    snapshot->vma_tree_max_remove_sink_steps =
        core->vma_tree_max_remove_sink_steps;
    core->vma_tree_max_remove_sink_steps = 0u;
#endif
    snapshot->vma_tree_remove_budget_exceeded =
        core->vma_tree_remove_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->mmap_free_tree_insertions = core->mmap_free_tree_insertions;
    snapshot->mmap_free_tree_rotations = core->mmap_free_tree_rotations;
    snapshot->mmap_free_tree_priority_ties =
        core->mmap_free_tree_priority_ties;
    snapshot->mmap_free_tree_priority_bucket_mask =
        core->mmap_free_tree_priority_bucket_mask;
    snapshot->mmap_free_tree_priority_bucket_max =
        core->mmap_free_tree_priority_bucket_max;
    snapshot->mmap_free_tree_priority_mix_calls =
        core->mmap_free_tree_priority_mix_calls;
    snapshot->mmap_free_tree_priority_mix_ns =
        core->mmap_free_tree_priority_mix_ns;
    snapshot->mmap_free_tree_priority_mix_max_ns =
        core->mmap_free_tree_priority_mix_max_ns;
    snapshot->mmap_free_tree_max_insert_depth =
        core->mmap_free_tree_max_insert_depth;
#endif
    snapshot->mmap_free_tree_tracked_nodes =
        core->mmap_free_tree_current_nodes;
    snapshot->mmap_free_tree_max_nodes = core->mmap_free_tree_max_nodes;
    snapshot->mmap_free_tree_depth_limit_exceeded =
        core->mmap_free_tree_depth_limit_exceeded;
    snapshot->mmap_free_tree_insert_budget_exceeded =
        core->mmap_free_tree_insert_budget_exceeded;
    snapshot->mmap_free_tree_insert_rotation_budget_exceeded =
        core->mmap_free_tree_insert_rotation_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->mmap_free_tree_find_calls =
        core->mmap_free_tree_find_calls;
    snapshot->mmap_free_tree_find_steps =
        core->mmap_free_tree_find_steps;
    snapshot->mmap_free_tree_max_find_steps =
        core->mmap_free_tree_max_find_steps;
#endif
    snapshot->mmap_free_tree_find_budget_exceeded =
        core->mmap_free_tree_find_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->mmap_free_tree_remove_calls =
        core->mmap_free_tree_remove_calls;
    snapshot->mmap_free_tree_remove_sink_steps =
        core->mmap_free_tree_remove_sink_steps;
    snapshot->mmap_free_tree_max_remove_sink_steps =
        core->mmap_free_tree_max_remove_sink_steps;
#endif
    snapshot->mmap_free_tree_remove_budget_exceeded =
        core->mmap_free_tree_remove_budget_exceeded;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->mmap_recent_free_attempts = core->mmap_recent_free_attempts;
    snapshot->mmap_recent_free_hits = core->mmap_recent_free_hits;
    snapshot->mmap_recent_free_misses = core->mmap_recent_free_misses;
    snapshot->mmap_recent_free_too_small = core->mmap_recent_free_too_small;
    snapshot->mmap_recent_free_best_fit_replacements =
        core->mmap_recent_free_best_fit_replacements;
    snapshot->mmap_size_hint_attempts = core->mmap_size_hint_attempts;
    snapshot->mmap_size_hint_hits = core->mmap_size_hint_hits;
    snapshot->mmap_size_hint_misses = core->mmap_size_hint_misses;
    snapshot->mmap_size_hint_stale = core->mmap_size_hint_stale;
    snapshot->mmap_size_hint_too_small = core->mmap_size_hint_too_small;
    snapshot->mmap_size_hint_best_fit_replacements =
        core->mmap_size_hint_best_fit_replacements;
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->page_span_calls =
        uhm_diag_counter_load_relaxed(&core->page_span_calls);
    snapshot->page_span_pages =
        uhm_diag_counter_load_relaxed(&core->page_span_pages);
    snapshot->page_mark_calls =
        uhm_diag_counter_load_relaxed(&core->page_mark_calls);
    snapshot->page_mark_pages =
        uhm_diag_counter_load_relaxed(&core->page_mark_pages);
    snapshot->page_mark_max_pages =
        uhm_diag_counter_load_relaxed(&core->page_mark_max_pages);
    snapshot->page_mark_budget_exceeded =
        uhm_diag_counter_load_relaxed(&core->page_mark_budget_exceeded);
    snapshot->page_mark_leaf_spans =
        uhm_diag_counter_load_relaxed(&core->page_mark_leaf_spans);
    snapshot->page_mark_max_leaf_spans =
        uhm_diag_counter_load_relaxed(&core->page_mark_max_leaf_spans);
    for (i = 0; i < UHM_PAGE_MARK_PHASE_COUNT; ++i) {
        snapshot->page_mark_phase_calls[i] =
            uhm_diag_counter_load_relaxed(&core->page_mark_phase_calls[i]);
        snapshot->page_mark_phase_pages[i] =
            uhm_diag_counter_load_relaxed(&core->page_mark_phase_pages[i]);
        snapshot->page_mark_phase_budget_exceeded[i] =
            uhm_diag_counter_load_relaxed(
                &core->page_mark_phase_budget_exceeded[i]);
    }
    snapshot->page_range_query_calls =
        uhm_diag_counter_load_relaxed(&core->page_range_query_calls);
    snapshot->page_range_query_pages =
        uhm_diag_counter_load_relaxed(&core->page_range_query_pages);
    snapshot->page_range_query_max_pages =
        uhm_diag_counter_load_relaxed(&core->page_range_query_max_pages);
    snapshot->page_range_query_budget_exceeded =
        uhm_diag_counter_load_relaxed(
            &core->page_range_query_budget_exceeded);
    snapshot->page_range_query_leaf_spans =
        uhm_diag_counter_load_relaxed(&core->page_range_query_leaf_spans);
    snapshot->page_range_query_max_leaf_spans =
        uhm_diag_counter_load_relaxed(
            &core->page_range_query_max_leaf_spans);
    for (i = 0; i < UHM_PAGE_QUERY_PHASE_COUNT; ++i) {
        snapshot->page_range_query_phase_calls[i] =
            uhm_diag_counter_load_relaxed(
                &core->page_range_query_phase_calls[i]);
        snapshot->page_range_query_phase_pages[i] =
            uhm_diag_counter_load_relaxed(
                &core->page_range_query_phase_pages[i]);
        snapshot->page_range_query_phase_budget_exceeded[i] =
            uhm_diag_counter_load_relaxed(
                &core->page_range_query_phase_budget_exceeded[i]);
    }
    snapshot->page_lock_range_calls =
        uhm_diag_counter_load_relaxed(&core->page_lock_range_calls);
    snapshot->page_lock_range_shards =
        uhm_diag_counter_load_relaxed(&core->page_lock_range_shards);
    snapshot->page_lock_range_max_shards =
        uhm_diag_counter_load_relaxed(&core->page_lock_range_max_shards);
    snapshot->page_lock_range_budget_exceeded =
        uhm_diag_counter_load_relaxed(
            &core->page_lock_range_budget_exceeded);
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->mprotect_mmap_noop_calls = core->mprotect_mmap_noop_calls;
    snapshot->mprotect_page_private_noop_calls =
        core->mprotect_page_private_noop_calls;
    snapshot->mprotect_platform_calls = core->mprotect_platform_calls;
    snapshot->mprotect_publish_calls = core->mprotect_publish_calls;
    snapshot->mprotect_rollback_calls = core->mprotect_rollback_calls;
    snapshot->mprotect_vma_collect_queries =
        core->mprotect_vma_collect_queries;
    snapshot->mprotect_vma_noop_queries = core->mprotect_vma_noop_queries;
    snapshot->mprotect_vma_mark_queries = core->mprotect_vma_mark_queries;
    snapshot->mprotect_vma_restore_queries =
        core->mprotect_vma_restore_queries;
    snapshot->mprotect_vma_publish_queries =
        core->mprotect_vma_publish_queries;
    snapshot->mprotect_vma_max_segments = core->mprotect_vma_max_segments;
    snapshot->munmap_vma_collect_queries = core->munmap_vma_collect_queries;
    snapshot->munmap_vma_publish_queries = core->munmap_vma_publish_queries;
    snapshot->munmap_vma_max_segments = core->munmap_vma_max_segments;
    snapshot->munmap_vma_trim_repair_elided =
        core->munmap_vma_trim_repair_elided;
    snapshot->munmap_vma_trim_repair_elided_pages =
        core->munmap_vma_trim_repair_elided_pages;
#endif
    snapshot->decommit_events = core->decommit_events;
    snapshot->decommit_bytes = core->decommit_bytes;
    snapshot->recommit_events = core->recommit_events;
    snapshot->recommit_bytes = core->recommit_bytes;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->malloc_free_decommit_commit_all_fast_paths =
        core->malloc_free_decommit_commit_all_fast_paths;
#endif
    snapshot->malloc_free_decommit_deferred_events =
        core->malloc_free_decommit_deferred_events;
    snapshot->malloc_free_decommit_deferred_bytes =
        core->malloc_free_decommit_deferred_bytes;
    snapshot->malloc_free_decommit_retained_bytes =
        (uint64_t)core->malloc_free_decommit_retained_bytes;
    snapshot->malloc_free_decommit_retention_base_high_water_bytes =
        (uint64_t)core->malloc_free_decommit_retention_base_high_water_bytes;
    snapshot->malloc_free_decommit_retention_high_water_bytes =
        (uint64_t)core->malloc_free_decommit_retention_high_water_bytes;
    snapshot->malloc_free_decommit_retention_max_high_water_bytes =
        (uint64_t)core->malloc_free_decommit_retention_max_high_water_bytes;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->malloc_free_decommit_retention_pressure_events =
        core->malloc_free_decommit_retention_pressure_events;
    snapshot->malloc_free_decommit_retention_pressure_bytes =
        core->malloc_free_decommit_retention_pressure_bytes;
    snapshot->malloc_free_decommit_retention_adaptive_grow_events =
        core->malloc_free_decommit_retention_adaptive_grow_events;
    snapshot->malloc_free_decommit_retention_adaptive_grow_bytes =
        core->malloc_free_decommit_retention_adaptive_grow_bytes;
    snapshot->malloc_free_decommit_retention_adaptive_shrink_events =
        core->malloc_free_decommit_retention_adaptive_shrink_events;
    snapshot->malloc_free_decommit_retention_adaptive_shrink_bytes =
        core->malloc_free_decommit_retention_adaptive_shrink_bytes;
    snapshot->malloc_free_decommit_hot_deferred_events =
        core->malloc_free_decommit_hot_deferred_events;
    snapshot->malloc_free_decommit_hot_deferred_bytes =
        core->malloc_free_decommit_hot_deferred_bytes;
#endif
    snapshot->slab_empty_committed_cache_low_water =
        UHM_SLAB_EMPTY_COMMITTED_CACHE_LOW_WATER;
    snapshot->slab_empty_committed_cache_base_high_water =
        UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    snapshot->slab_empty_committed_cache_high_water =
        UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        size_t high_water =
            uhm_slab_empty_committed_cache_state_high_water(
                core->slab_empty_committed_cache_high_water[i]);
        if (high_water >
            snapshot->slab_empty_committed_cache_high_water) {
            snapshot->slab_empty_committed_cache_high_water =
                (uint64_t)high_water;
        }
    }
    snapshot->slab_empty_committed_cache_max_high_water =
        UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->slab_empty_decommit_pressure_events =
        core->slab_empty_decommit_pressure_events;
    snapshot->slab_empty_decommit_pressure_bytes =
        core->slab_empty_decommit_pressure_bytes;
    snapshot->slab_empty_committed_cache_adaptive_grow_events =
        core->slab_empty_committed_cache_adaptive_grow_events;
    snapshot->slab_empty_committed_cache_adaptive_grow_bytes =
        core->slab_empty_committed_cache_adaptive_grow_bytes;
#endif
    snapshot->decommit_recommit_same_addr_events =
        core->decommit_recommit_same_addr_events;
    snapshot->decommit_recommit_same_addr_bytes =
        core->decommit_recommit_same_addr_bytes;
    snapshot->malloc_free_tree_current_nodes =
        bench_malloc_free_tree_count(core->malloc_free_tree_root);
    snapshot->malloc_free_tree_current_depth =
        bench_malloc_free_tree_depth(core->malloc_free_tree_root);
    snapshot->vma_tree_current_nodes =
        bench_vma_tree_count(core->vma_tree_root);
    snapshot->vma_tree_current_depth =
        bench_vma_tree_depth(core->vma_tree_root);
    snapshot->mmap_free_tree_current_nodes =
        bench_mmap_free_tree_count(core->mmap_free_tree_root);
    snapshot->mmap_free_tree_current_depth =
        bench_mmap_free_tree_depth(core->mmap_free_tree_root);
    snapshot->vma_list_nodes = bench_vma_list_count(core->vma_head);
    snapshot->mmap_free_list_nodes =
        bench_mmap_free_list_count(core->mmap_free_head);
    snapshot->retired_vma_count = (uint64_t)core->retired_vma_count;
    snapshot->retired_free_extent_count =
        (uint64_t)core->retired_free_extent_count;
    snapshot->hot_vma_count = (uint64_t)core->hot_vma_count;
    snapshot->hot_free_extent_count = (uint64_t)core->hot_free_extent_count;
    snapshot->metadata_hot_vma_hits = core->metadata_hot_vma_hits;
    snapshot->metadata_hot_vma_misses = core->metadata_hot_vma_misses;
    snapshot->metadata_hot_vma_reclaimed = core->metadata_hot_vma_reclaimed;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->metadata_hot_vma_full_defers =
        core->metadata_hot_vma_full_defers;
#endif
    snapshot->metadata_hot_free_extent_hits =
        core->metadata_hot_free_extent_hits;
    snapshot->metadata_hot_free_extent_misses =
        core->metadata_hot_free_extent_misses;
    snapshot->metadata_hot_free_extent_reclaimed =
        core->metadata_hot_free_extent_reclaimed;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->metadata_hot_free_extent_full_defers =
        core->metadata_hot_free_extent_full_defers;
#endif
    snapshot->metadata_quarantine_drain_calls =
        core->metadata_quarantine_drain_calls;
    snapshot->metadata_quarantine_drain_scanned =
        core->metadata_quarantine_drain_scanned;
    snapshot->metadata_quarantine_drain_reclaimed =
        core->metadata_quarantine_drain_reclaimed;
    snapshot->metadata_quarantine_drain_budget_exhausted =
        core->metadata_quarantine_drain_budget_exhausted;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->metadata_quarantine_drain_rotated =
        core->metadata_quarantine_drain_rotated;
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->control_alloc_recent_hits = core->control_alloc_recent_hits;
    snapshot->control_alloc_hint_hits = core->control_alloc_hint_hits;
    snapshot->control_alloc_hint_misses = core->control_alloc_hint_misses;
    snapshot->control_alloc_hint_stale = core->control_alloc_hint_stale;
    snapshot->control_alloc_no_fit_fast_paths =
        core->control_alloc_no_fit_fast_paths;
    snapshot->control_alloc_scan_calls = core->control_alloc_scan_calls;
    snapshot->control_alloc_scan_steps = core->control_alloc_scan_steps;
    snapshot->control_alloc_scan_max_steps =
        core->control_alloc_scan_max_steps;
    snapshot->control_alloc_scan_budget_exceeded =
        core->control_alloc_scan_budget_exceeded;
#endif
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->tls_cache_hits = core->tls_cache_hits;
    snapshot->tls_cache_misses = core->tls_cache_misses;
    snapshot->tls_cache_pushes = core->tls_cache_pushes;
    snapshot->tls_cache_overflows = core->tls_cache_overflows;
    snapshot->tls_cache_retained_page_pressure =
        core->tls_cache_retained_page_pressure;
    {
        const uhm_tls_cache *cache;
        for (cache = core->tls_cache_head; cache; cache = cache->next_active) {
            snapshot->tls_cache_hits += cache->hit_count;
            snapshot->tls_cache_misses += cache->miss_count;
            snapshot->tls_cache_pushes += cache->push_count;
            snapshot->tls_cache_overflows += cache->overflow_count;
            snapshot->tls_cache_retained_page_pressure +=
                cache->retained_page_pressure_count;
        }
    }
#endif
    snapshot->vm_reserve_calls = core->vm_reserve_calls;
    snapshot->vm_reserve_failures = core->vm_reserve_failures;
    snapshot->vm_reserve_ns = core->vm_reserve_ns;
    snapshot->vm_commit_calls = core->vm_commit_calls;
    snapshot->vm_commit_failures = core->vm_commit_failures;
    snapshot->vm_commit_ns = core->vm_commit_ns;
    snapshot->vm_decommit_calls = core->vm_decommit_calls;
    snapshot->vm_decommit_failures = core->vm_decommit_failures;
    snapshot->vm_decommit_ns = core->vm_decommit_ns;
    snapshot->vm_fixed_remap_calls = core->vm_fixed_remap_calls;
    snapshot->vm_fixed_remap_failures = core->vm_fixed_remap_failures;
    snapshot->vm_fixed_remap_ns = core->vm_fixed_remap_ns;
    snapshot->vm_protect_calls = core->vm_protect_calls;
    snapshot->vm_protect_failures = core->vm_protect_failures;
    snapshot->vm_protect_ns = core->vm_protect_ns;
    snapshot->vm_release_calls = core->vm_release_calls;
    snapshot->vm_release_failures = core->vm_release_failures;
    snapshot->vm_release_ns = core->vm_release_ns;
    snapshot->vm_numa_hint_calls = core->vm_numa_hint_calls;
    snapshot->vm_numa_hint_failures = core->vm_numa_hint_failures;
    snapshot->vm_numa_hint_ns = core->vm_numa_hint_ns;
    snapshot->vm_lock_held_calls = core->vm_lock_held_calls;
    snapshot->vm_lock_held_failures = core->vm_lock_held_failures;
    snapshot->vm_lock_held_ns = core->vm_lock_held_ns;
    snapshot->vm_reserve_lock_held_calls =
        core->vm_reserve_lock_held_calls;
    snapshot->vm_commit_lock_held_calls = core->vm_commit_lock_held_calls;
    snapshot->vm_decommit_lock_held_calls =
        core->vm_decommit_lock_held_calls;
    snapshot->vm_fixed_remap_lock_held_calls =
        core->vm_fixed_remap_lock_held_calls;
    snapshot->vm_protect_lock_held_calls =
        core->vm_protect_lock_held_calls;
    snapshot->vm_release_lock_held_calls =
        core->vm_release_lock_held_calls;
    snapshot->vm_numa_hint_lock_held_calls =
        core->vm_numa_hint_lock_held_calls;
#if UHMAS_ENABLE_DIAGNOSTICS
    snapshot->heap_lock_calls =
        uhm_diag_counter_load_relaxed(&heap_lock->lock_calls);
    snapshot->heap_lock_try_calls =
        uhm_diag_counter_load_relaxed(&heap_lock->try_calls);
    snapshot->heap_lock_try_failures =
        uhm_diag_counter_load_relaxed(&heap_lock->try_failures);
    snapshot->heap_lock_contended_calls =
        uhm_diag_counter_load_relaxed(&heap_lock->contended_calls);
    snapshot->heap_lock_wait_ns =
        uhm_diag_counter_load_relaxed(&heap_lock->wait_ns);
    snapshot->heap_lock_max_wait_ns =
        uhm_diag_counter_exchange_relaxed(&heap_lock->max_wait_ns,
                                          UINT64_C(0));
    snapshot->heap_lock_hold_ns =
        uhm_diag_counter_load_relaxed(&heap_lock->hold_ns);
    snapshot->heap_lock_max_hold_ns =
        uhm_diag_counter_exchange_relaxed(&heap_lock->max_hold_ns,
                                          UINT64_C(0));
    {
        uhm_slab *slab;
        for (slab = core->slab_all_head; slab; slab = slab->next_all) {
            uhm_spinlock *raw_lock = &slab->raw_lock;
            uint64_t max_wait;
            uint64_t max_hold;

            snapshot->slab_raw_lock_calls +=
                uhm_diag_counter_load_relaxed(&raw_lock->lock_calls);
            snapshot->slab_raw_lock_try_calls +=
                uhm_diag_counter_load_relaxed(&raw_lock->try_calls);
            snapshot->slab_raw_lock_try_failures +=
                uhm_diag_counter_load_relaxed(&raw_lock->try_failures);
            snapshot->slab_raw_lock_contended_calls +=
                uhm_diag_counter_load_relaxed(&raw_lock->contended_calls);
            snapshot->slab_raw_lock_wait_ns +=
                uhm_diag_counter_load_relaxed(&raw_lock->wait_ns);
            max_wait = uhm_diag_counter_exchange_relaxed(
                &raw_lock->max_wait_ns, UINT64_C(0));
            if (max_wait > snapshot->slab_raw_lock_max_wait_ns) {
                snapshot->slab_raw_lock_max_wait_ns = max_wait;
            }
            snapshot->slab_raw_lock_hold_ns +=
                uhm_diag_counter_load_relaxed(&raw_lock->hold_ns);
            max_hold = uhm_diag_counter_exchange_relaxed(
                &raw_lock->max_hold_ns, UINT64_C(0));
            if (max_hold > snapshot->slab_raw_lock_max_hold_ns) {
                snapshot->slab_raw_lock_max_hold_ns = max_hold;
            }
        }
    }
#else
    (void)heap_lock;
#endif
}

static void bench_diag_interval_max_self_check(bench_heap_env *env) {
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_diag_snapshot first;
    bench_diag_snapshot second;

    if (!env || !env->heap || !bench_diagnostics_enabled()) {
        return;
    }
    uhm_diag_counter_store_relaxed(&env->heap->lock.max_wait_ns,
                                   UINT64_C(77));
    uhm_diag_counter_store_relaxed(&env->heap->lock.max_hold_ns,
                                   UINT64_C(123));
    env->heap->core.vma_tree_max_remove_sink_steps = UINT64_C(15);
    bench_diag_capture(env, &first);
    bench_diag_capture(env, &second);
    bench_expect(first.heap_lock_max_wait_ns == UINT64_C(77) &&
                     first.heap_lock_max_hold_ns == UINT64_C(123),
                 "diagnostic interval max first capture");
    bench_expect(second.heap_lock_max_wait_ns == 0u &&
                     second.heap_lock_max_hold_ns == 0u,
                 "diagnostic interval max reset");
    bench_expect(first.vma_tree_max_remove_sink_steps == UINT64_C(15) &&
                     second.vma_tree_max_remove_sink_steps == 0u,
                 "diagnostic VMA interval max reset");
#else
    (void)env;
#endif
}

