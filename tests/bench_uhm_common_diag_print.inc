static uint64_t bench_diag_max_delta(uint64_t before, uint64_t after) {
    (void)before;
    return after;
}

static uint64_t bench_diag_counter_delta(uint64_t before, uint64_t after) {
    return after >= before ? after - before : 0u;
}

static void bench_diag_print_page_range_phase_delta(
    const char *name, const bench_diag_snapshot *before,
    const bench_diag_snapshot *after) {
    static const char *const mark_names[UHM_PAGE_MARK_PHASE_COUNT] = {
        "unknown", "control", "malloc_live", "malloc_decommitted",
        "mmap_live", "free_reusable", "protecting"};
    static const char *const query_names[UHM_PAGE_QUERY_PHASE_COUNT] = {
        "is_free", "has_owner", "has_state", "matches_descriptor",
        "matches_descriptor_protecting", "overlaps_owner", "has_live_mmap",
        "overlaps_va_state", "get_expected_state", "matches_generation"};
    size_t i;

    if (!name || !before || !after) {
        return;
    }
    printf("metadata page-range-phase %s:", name);
    for (i = 0; i < UHM_PAGE_MARK_PHASE_COUNT; ++i) {
        printf(" mark_%s_calls=%llu mark_%s_pages=%llu "
               "mark_%s_budget_exceeded=%llu",
               mark_names[i],
               (unsigned long long)bench_diag_counter_delta(
                   before->page_mark_phase_calls[i],
                   after->page_mark_phase_calls[i]),
               mark_names[i],
               (unsigned long long)bench_diag_counter_delta(
                   before->page_mark_phase_pages[i],
                   after->page_mark_phase_pages[i]),
               mark_names[i],
               (unsigned long long)bench_diag_counter_delta(
                   before->page_mark_phase_budget_exceeded[i],
                   after->page_mark_phase_budget_exceeded[i]));
    }
    for (i = 0; i < UHM_PAGE_QUERY_PHASE_COUNT; ++i) {
        printf(" query_%s_calls=%llu query_%s_pages=%llu "
               "query_%s_budget_exceeded=%llu",
               query_names[i],
               (unsigned long long)bench_diag_counter_delta(
                   before->page_range_query_phase_calls[i],
                   after->page_range_query_phase_calls[i]),
               query_names[i],
               (unsigned long long)bench_diag_counter_delta(
                   before->page_range_query_phase_pages[i],
                   after->page_range_query_phase_pages[i]),
               query_names[i],
               (unsigned long long)bench_diag_counter_delta(
                   before->page_range_query_phase_budget_exceeded[i],
                   after->page_range_query_phase_budget_exceeded[i]));
    }
    printf("\n");
}

static void bench_diag_print_tls_cache_delta(
    const char *name, const bench_diag_snapshot *before,
    const bench_diag_snapshot *after) {
    if (!name || !before || !after) {
        return;
    }
    printf("metadata tls-cache %s: hits=%llu misses=%llu pushes=%llu "
           "overflows=%llu retained_page_pressure=%llu\n",
           name,
           (unsigned long long)bench_diag_counter_delta(
               before->tls_cache_hits, after->tls_cache_hits),
           (unsigned long long)bench_diag_counter_delta(
               before->tls_cache_misses, after->tls_cache_misses),
           (unsigned long long)bench_diag_counter_delta(
               before->tls_cache_pushes, after->tls_cache_pushes),
           (unsigned long long)bench_diag_counter_delta(
               before->tls_cache_overflows, after->tls_cache_overflows),
           (unsigned long long)bench_diag_counter_delta(
               before->tls_cache_retained_page_pressure,
               after->tls_cache_retained_page_pressure));
}

static void bench_metadata_print_attribution_delta(
    const bench_result *result, const bench_diag_snapshot *before,
    const bench_diag_snapshot *after, int diagnostics_enabled) {
    uint64_t vm_calls;
    uint64_t vm_failures;
    uint64_t vm_ns;
    uint64_t vm_lock_held_calls;
    uint64_t page_map_pages;
    uint64_t page_budget_exceeded;
    uint64_t tree_nodes_after;
    uint64_t tree_depth_after;
    uint64_t tree_budget_exceeded;
    uint64_t metadata_nodes_after;
    uint64_t metadata_hot_hits;
    uint64_t metadata_hot_misses;
    uint64_t metadata_hot_reclaimed;

    if (!result || !result->name || !before || !after) {
        return;
    }

    vm_calls =
        bench_diag_counter_delta(before->vm_reserve_calls,
                                 after->vm_reserve_calls) +
        bench_diag_counter_delta(before->vm_commit_calls,
                                 after->vm_commit_calls) +
        bench_diag_counter_delta(before->vm_decommit_calls,
                                 after->vm_decommit_calls) +
        bench_diag_counter_delta(before->vm_fixed_remap_calls,
                                 after->vm_fixed_remap_calls) +
        bench_diag_counter_delta(before->vm_protect_calls,
                                 after->vm_protect_calls) +
        bench_diag_counter_delta(before->vm_release_calls,
                                 after->vm_release_calls) +
        bench_diag_counter_delta(before->vm_numa_hint_calls,
                                 after->vm_numa_hint_calls);
    vm_failures =
        bench_diag_counter_delta(before->vm_reserve_failures,
                                 after->vm_reserve_failures) +
        bench_diag_counter_delta(before->vm_commit_failures,
                                 after->vm_commit_failures) +
        bench_diag_counter_delta(before->vm_decommit_failures,
                                 after->vm_decommit_failures) +
        bench_diag_counter_delta(before->vm_fixed_remap_failures,
                                 after->vm_fixed_remap_failures) +
        bench_diag_counter_delta(before->vm_protect_failures,
                                 after->vm_protect_failures) +
        bench_diag_counter_delta(before->vm_release_failures,
                                 after->vm_release_failures) +
        bench_diag_counter_delta(before->vm_numa_hint_failures,
                                 after->vm_numa_hint_failures);
    vm_ns =
        bench_diag_counter_delta(before->vm_reserve_ns,
                                 after->vm_reserve_ns) +
        bench_diag_counter_delta(before->vm_commit_ns,
                                 after->vm_commit_ns) +
        bench_diag_counter_delta(before->vm_decommit_ns,
                                 after->vm_decommit_ns) +
        bench_diag_counter_delta(before->vm_fixed_remap_ns,
                                 after->vm_fixed_remap_ns) +
        bench_diag_counter_delta(before->vm_protect_ns,
                                 after->vm_protect_ns) +
        bench_diag_counter_delta(before->vm_release_ns,
                                 after->vm_release_ns) +
        bench_diag_counter_delta(before->vm_numa_hint_ns,
                                 after->vm_numa_hint_ns);
    vm_lock_held_calls =
        bench_diag_counter_delta(before->vm_lock_held_calls,
                                 after->vm_lock_held_calls);
    page_map_pages =
        bench_diag_counter_delta(before->page_span_pages,
                                 after->page_span_pages) +
        bench_diag_counter_delta(before->page_mark_pages,
                                 after->page_mark_pages) +
        bench_diag_counter_delta(before->page_range_query_pages,
                                 after->page_range_query_pages);
    page_budget_exceeded =
        bench_diag_counter_delta(before->page_mark_budget_exceeded,
                                 after->page_mark_budget_exceeded) +
        bench_diag_counter_delta(before->page_range_query_budget_exceeded,
                                 after->page_range_query_budget_exceeded) +
        bench_diag_counter_delta(before->page_lock_range_budget_exceeded,
                                 after->page_lock_range_budget_exceeded);
    tree_nodes_after = after->malloc_free_tree_current_nodes +
                       after->vma_tree_current_nodes +
                       after->mmap_free_tree_current_nodes;
    tree_depth_after =
        bench_u64_max(after->malloc_free_tree_current_depth,
                      bench_u64_max(after->vma_tree_current_depth,
                                    after->mmap_free_tree_current_depth));
    tree_budget_exceeded =
        bench_diag_counter_delta(before->malloc_free_tree_depth_limit_exceeded,
                                 after->malloc_free_tree_depth_limit_exceeded) +
        bench_diag_counter_delta(before->malloc_free_tree_insert_budget_exceeded,
                                 after->malloc_free_tree_insert_budget_exceeded) +
        bench_diag_counter_delta(
            before->malloc_free_tree_insert_rotation_budget_exceeded,
            after->malloc_free_tree_insert_rotation_budget_exceeded) +
        bench_diag_counter_delta(before->malloc_free_tree_find_budget_exceeded,
                                 after->malloc_free_tree_find_budget_exceeded) +
        bench_diag_counter_delta(before->malloc_free_tree_remove_budget_exceeded,
                                 after->malloc_free_tree_remove_budget_exceeded) +
        bench_diag_counter_delta(before->vma_tree_depth_limit_exceeded,
                                 after->vma_tree_depth_limit_exceeded) +
        bench_diag_counter_delta(before->vma_tree_insert_budget_exceeded,
                                 after->vma_tree_insert_budget_exceeded) +
        bench_diag_counter_delta(
            before->vma_tree_insert_rotation_budget_exceeded,
            after->vma_tree_insert_rotation_budget_exceeded) +
        bench_diag_counter_delta(before->vma_tree_find_budget_exceeded,
                                 after->vma_tree_find_budget_exceeded) +
        bench_diag_counter_delta(before->vma_tree_remove_budget_exceeded,
                                 after->vma_tree_remove_budget_exceeded) +
        bench_diag_counter_delta(before->mmap_free_tree_depth_limit_exceeded,
                                 after->mmap_free_tree_depth_limit_exceeded) +
        bench_diag_counter_delta(before->mmap_free_tree_insert_budget_exceeded,
                                 after->mmap_free_tree_insert_budget_exceeded) +
        bench_diag_counter_delta(
            before->mmap_free_tree_insert_rotation_budget_exceeded,
            after->mmap_free_tree_insert_rotation_budget_exceeded) +
        bench_diag_counter_delta(before->mmap_free_tree_find_budget_exceeded,
                                 after->mmap_free_tree_find_budget_exceeded) +
        bench_diag_counter_delta(before->mmap_free_tree_remove_budget_exceeded,
                                 after->mmap_free_tree_remove_budget_exceeded);
    metadata_nodes_after = after->vma_list_nodes + after->mmap_free_list_nodes +
                           after->retired_vma_count +
                           after->retired_free_extent_count +
                           after->hot_vma_count +
                           after->hot_free_extent_count;
    metadata_hot_hits =
        bench_diag_counter_delta(before->metadata_hot_vma_hits,
                                 after->metadata_hot_vma_hits) +
        bench_diag_counter_delta(before->metadata_hot_free_extent_hits,
                                 after->metadata_hot_free_extent_hits);
    metadata_hot_misses =
        bench_diag_counter_delta(before->metadata_hot_vma_misses,
                                 after->metadata_hot_vma_misses) +
        bench_diag_counter_delta(before->metadata_hot_free_extent_misses,
                                 after->metadata_hot_free_extent_misses);
    metadata_hot_reclaimed =
        bench_diag_counter_delta(before->metadata_hot_vma_reclaimed,
                                 after->metadata_hot_vma_reclaimed) +
        bench_diag_counter_delta(before->metadata_hot_free_extent_reclaimed,
                                 after->metadata_hot_free_extent_reclaimed);

    printf("metadata attribution %s: operations=%llu "
           "diagnostics_enabled=%u heap_check_excluded=1 "
           "vm_calls=%llu vm_failures=%llu vm_ns=%llu "
           "vm_lock_held_calls=%llu "
           "vm_commit_calls=%llu vm_decommit_calls=%llu "
           "vm_protect_calls=%llu vm_numa_hint_calls=%llu "
           "decommit_events=%llu decommit_bytes=%llu "
           "recommit_events=%llu recommit_bytes=%llu "
           "commit_all_known_fast_paths=%llu "
           "same_addr_events=%llu same_addr_bytes=%llu "
           "page_map_pages=%llu page_span_pages=%llu "
           "page_mark_pages=%llu page_query_pages=%llu "
           "page_lock_shards=%llu page_budget_exceeded=%llu "
           "heap_lock_calls=%llu heap_lock_contended=%llu "
           "heap_lock_hold_ns=%llu heap_lock_max_hold_ns=%llu "
           "live_slow_calls=%llu live_fallback_scans=%llu "
           "tree_nodes_after=%llu tree_depth_after=%llu "
           "tree_budget_exceeded=%llu metadata_nodes_after=%llu "
           "metadata_hot_hits=%llu metadata_hot_misses=%llu "
           "metadata_hot_reclaimed=%llu "
           "metadata_quarantine_drain_calls=%llu "
           "metadata_quarantine_scanned=%llu "
           "metadata_quarantine_budget_exhausted=%llu\n",
           result->name,
           (unsigned long long)result->operations,
           diagnostics_enabled ? 1u : 0u,
           (unsigned long long)vm_calls,
           (unsigned long long)vm_failures,
           (unsigned long long)vm_ns,
           (unsigned long long)vm_lock_held_calls,
           (unsigned long long)bench_diag_counter_delta(
               before->vm_commit_calls, after->vm_commit_calls),
           (unsigned long long)bench_diag_counter_delta(
               before->vm_decommit_calls, after->vm_decommit_calls),
           (unsigned long long)bench_diag_counter_delta(
               before->vm_protect_calls, after->vm_protect_calls),
           (unsigned long long)bench_diag_counter_delta(
               before->vm_numa_hint_calls, after->vm_numa_hint_calls),
           (unsigned long long)bench_diag_counter_delta(
               before->decommit_events, after->decommit_events),
           (unsigned long long)bench_diag_counter_delta(
               before->decommit_bytes, after->decommit_bytes),
           (unsigned long long)bench_diag_counter_delta(
               before->recommit_events, after->recommit_events),
           (unsigned long long)bench_diag_counter_delta(
               before->recommit_bytes, after->recommit_bytes),
           (unsigned long long)bench_diag_counter_delta(
               before->malloc_free_decommit_commit_all_fast_paths,
               after->malloc_free_decommit_commit_all_fast_paths),
           (unsigned long long)bench_diag_counter_delta(
               before->decommit_recommit_same_addr_events,
               after->decommit_recommit_same_addr_events),
           (unsigned long long)bench_diag_counter_delta(
               before->decommit_recommit_same_addr_bytes,
               after->decommit_recommit_same_addr_bytes),
           (unsigned long long)page_map_pages,
           (unsigned long long)bench_diag_counter_delta(
               before->page_span_pages, after->page_span_pages),
           (unsigned long long)bench_diag_counter_delta(
               before->page_mark_pages, after->page_mark_pages),
           (unsigned long long)bench_diag_counter_delta(
               before->page_range_query_pages,
               after->page_range_query_pages),
           (unsigned long long)bench_diag_counter_delta(
               before->page_lock_range_shards,
               after->page_lock_range_shards),
           (unsigned long long)page_budget_exceeded,
           (unsigned long long)bench_diag_counter_delta(
               before->heap_lock_calls, after->heap_lock_calls),
           (unsigned long long)bench_diag_counter_delta(
               before->heap_lock_contended_calls,
               after->heap_lock_contended_calls),
           (unsigned long long)bench_diag_counter_delta(
               before->heap_lock_hold_ns, after->heap_lock_hold_ns),
            (unsigned long long)after->heap_lock_max_hold_ns,
           (unsigned long long)bench_diag_counter_delta(
               before->live_block_slow_calls,
               after->live_block_slow_calls),
           (unsigned long long)bench_diag_counter_delta(
               before->live_block_slow_fallback_scans,
               after->live_block_slow_fallback_scans),
           (unsigned long long)tree_nodes_after,
           (unsigned long long)tree_depth_after,
           (unsigned long long)tree_budget_exceeded,
           (unsigned long long)metadata_nodes_after,
           (unsigned long long)metadata_hot_hits,
           (unsigned long long)metadata_hot_misses,
           (unsigned long long)metadata_hot_reclaimed,
           (unsigned long long)bench_diag_counter_delta(
               before->metadata_quarantine_drain_calls,
               after->metadata_quarantine_drain_calls),
           (unsigned long long)bench_diag_counter_delta(
               before->metadata_quarantine_drain_scanned,
               after->metadata_quarantine_drain_scanned),
           (unsigned long long)bench_diag_counter_delta(
               before->metadata_quarantine_drain_budget_exhausted,
               after->metadata_quarantine_drain_budget_exhausted));
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
static void bench_diag_print_delta(const char *name,
                                   const bench_diag_snapshot *before,
                                   const bench_diag_snapshot *after) {
    uint64_t live_lookup_fast_fail_delta
        [UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT]
        [UHM_LIVE_BLOCK_FAST_FAIL_REASON_COUNT];
    size_t i;
    size_t j;

    for (i = 0; i < UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT; ++i) {
        for (j = 0; j < UHM_LIVE_BLOCK_FAST_FAIL_REASON_COUNT; ++j) {
            live_lookup_fast_fail_delta[i][j] =
                bench_diag_counter_delta(
                    before->live_block_lookup_fast_fail_reasons[i][j],
                    after->live_block_lookup_fast_fail_reasons[i][j]);
        }
    }

    printf("diag|%s|"
           "treap_priority_mix_calls=%llu "
           "treap_priority_mix_ns=%llu "
           "treap_priority_mix_max_ns=%llu "
           "treap_health_flags=%llu "
           "live_block_health_flags=%llu "
           "treap_repair_pending_flags=%llu "
           "treap_repair_rebuilds=%llu "
           "treap_repair_rehashes=%llu "
           "treap_repair_nodes=%llu "
           "treap_repair_failures=%llu "
           "live_fast=%llu "
           "live_slow_calls=%llu "
           "live_slow_hits=%llu "
           "live_slow_misses=%llu "
           "live_slow_scanned=%llu "
           "live_slow_max_scanned=%llu "
           "live_slow_ns=%llu "
           "live_slow_max_ns=%llu "
           "live_fast_fail_invalid=%llu "
           "live_fast_fail_range=%llu "
           "live_fast_fail_header_page=%llu "
           "live_fast_fail_payload_page=%llu "
           "live_fast_fail_descriptor=%llu "
           "live_fast_terminal_misses=%llu "
           "live_tree_insertions=%llu "
           "live_tree_rotations=%llu "
           "live_tree_priority_ties=%llu "
           "live_tree_priority_bucket_mask=%llu "
           "live_tree_priority_bucket_used=%llu "
           "live_tree_priority_bucket_max=%llu "
           "live_tree_priority_mix_calls=%llu "
           "live_tree_priority_mix_ns=%llu "
           "live_tree_priority_mix_max_ns=%llu "
           "live_tree_max_insert_depth=%llu "
           "live_tree_tracked_nodes=%llu "
           "live_tree_max_nodes=%llu "
           "live_tree_depth_limit_exceeded=%llu "
           "live_tree_insert_budget_exceeded=%llu "
           "live_tree_insert_rotation_budget_exceeded=%llu "
           "live_tree_find_calls=%llu "
           "live_tree_find_steps=%llu "
           "live_tree_max_find_steps=%llu "
           "live_tree_find_budget_exceeded=%llu "
           "live_tree_mutation_epoch=%llu "
           "live_tree_remove_calls=%llu "
           "live_tree_remove_sink_steps=%llu "
           "live_tree_max_remove_sink_steps=%llu "
           "live_tree_remove_budget_exceeded=%llu "
           "live_slow_tree_hits=%llu "
           "live_slow_tree_misses=%llu "
           "live_slow_fallback_scans=%llu "
           "live_slow_fallback_scanned=%llu "
           "live_slow_fallback_max_scanned=%llu "
           "live_slow_fallback_budget_exceeded=%llu "
           "live_slow_fallback_aborts=%llu "
           "live_slow_fallback_negative_stores=%llu "
           "live_slow_fallback_negative_hits=%llu "
           "live_slow_fallback_negative_evictions=%llu "
           "live_slow_fallback_throttle_epoch=%llu "
           "live_slow_fallback_epoch_scans=%llu "
           "live_slow_fallback_throttles=%llu "
           "live_slow_fallback_scenario_epoch=%llu "
           "live_slow_fallback_scenario_checks=%llu "
           "live_slow_fallback_scenario_throttles=%llu "
           "live_lookup_unknown_calls=%llu "
           "live_lookup_unknown_slow=%llu "
           "live_lookup_unknown_fallback=%llu "
           "live_lookup_unknown_throttles=%llu "
           "live_lookup_unknown_scenario_calls=%llu "
           "live_lookup_unknown_scenario_candidates=%llu "
           "live_lookup_free_calls=%llu "
           "live_lookup_free_slow=%llu "
           "live_lookup_free_fallback=%llu "
           "live_lookup_free_throttles=%llu "
           "live_lookup_free_scenario_calls=%llu "
           "live_lookup_free_scenario_candidates=%llu "
           "live_lookup_realloc_calls=%llu "
           "live_lookup_realloc_slow=%llu "
           "live_lookup_realloc_fallback=%llu "
           "live_lookup_realloc_throttles=%llu "
           "live_lookup_realloc_scenario_calls=%llu "
           "live_lookup_realloc_scenario_candidates=%llu "
           "live_lookup_usable_calls=%llu "
           "live_lookup_usable_slow=%llu "
           "live_lookup_usable_fallback=%llu "
           "live_lookup_usable_throttles=%llu "
           "live_lookup_usable_scenario_calls=%llu "
           "live_lookup_usable_scenario_candidates=%llu "
           "live_lookup_thread_ctx_calls=%llu "
           "live_lookup_thread_ctx_slow=%llu "
           "live_lookup_thread_ctx_fallback=%llu "
           "live_lookup_thread_ctx_throttles=%llu "
           "live_lookup_thread_ctx_scenario_calls=%llu "
           "live_lookup_thread_ctx_scenario_candidates=%llu "
           "live_lookup_unknown_fail_invalid=%llu "
           "live_lookup_unknown_fail_range=%llu "
           "live_lookup_unknown_fail_header_page=%llu "
           "live_lookup_unknown_fail_payload_page=%llu "
           "live_lookup_unknown_fail_descriptor=%llu "
           "live_lookup_free_fail_invalid=%llu "
           "live_lookup_free_fail_range=%llu "
           "live_lookup_free_fail_header_page=%llu "
           "live_lookup_free_fail_payload_page=%llu "
           "live_lookup_free_fail_descriptor=%llu "
           "live_lookup_realloc_fail_invalid=%llu "
           "live_lookup_realloc_fail_range=%llu "
           "live_lookup_realloc_fail_header_page=%llu "
           "live_lookup_realloc_fail_payload_page=%llu "
           "live_lookup_realloc_fail_descriptor=%llu "
           "live_lookup_usable_fail_invalid=%llu "
           "live_lookup_usable_fail_range=%llu "
           "live_lookup_usable_fail_header_page=%llu "
           "live_lookup_usable_fail_payload_page=%llu "
           "live_lookup_usable_fail_descriptor=%llu "
           "live_lookup_thread_ctx_fail_invalid=%llu "
           "live_lookup_thread_ctx_fail_range=%llu "
           "live_lookup_thread_ctx_fail_header_page=%llu "
           "live_lookup_thread_ctx_fail_payload_page=%llu "
           "live_lookup_thread_ctx_fail_descriptor=%llu "
           "malloc_tree_insertions=%llu "
           "malloc_tree_rotations=%llu "
           "malloc_tree_priority_ties=%llu "
           "malloc_tree_priority_bucket_mask=%llu "
           "malloc_tree_priority_bucket_used=%llu "
           "malloc_tree_priority_bucket_max=%llu "
           "malloc_tree_priority_mix_calls=%llu "
           "malloc_tree_priority_mix_ns=%llu "
           "malloc_tree_priority_mix_max_ns=%llu "
           "malloc_tree_max_insert_depth=%llu "
           "malloc_tree_tracked_nodes=%llu "
           "malloc_tree_max_nodes=%llu "
           "malloc_tree_depth_limit_exceeded=%llu "
           "malloc_tree_insert_budget_exceeded=%llu "
           "malloc_tree_insert_rotation_budget_exceeded=%llu "
           "malloc_tree_find_calls=%llu "
           "malloc_tree_find_steps=%llu "
           "malloc_tree_max_find_steps=%llu "
           "malloc_tree_find_budget_exceeded=%llu "
           "malloc_tree_remove_calls=%llu "
           "malloc_tree_remove_sink_steps=%llu "
           "malloc_tree_max_remove_sink_steps=%llu "
           "malloc_tree_remove_budget_exceeded=%llu "
           "vma_tree_insertions=%llu "
           "vma_tree_rotations=%llu "
           "vma_tree_priority_ties=%llu "
           "vma_tree_priority_bucket_mask=%llu "
           "vma_tree_priority_bucket_used=%llu "
           "vma_tree_priority_bucket_max=%llu "
           "vma_tree_priority_mix_calls=%llu "
           "vma_tree_priority_mix_ns=%llu "
           "vma_tree_priority_mix_max_ns=%llu "
           "vma_tree_max_insert_depth=%llu "
           "vma_tree_tracked_nodes=%llu "
           "vma_tree_max_nodes=%llu "
           "vma_tree_depth_limit_exceeded=%llu "
           "vma_tree_insert_budget_exceeded=%llu "
           "vma_tree_insert_rotation_budget_exceeded=%llu "
           "vma_tree_find_calls=%llu "
           "vma_tree_find_steps=%llu "
           "vma_tree_max_find_steps=%llu "
           "vma_tree_find_budget_exceeded=%llu "
           "vma_tree_remove_calls=%llu "
           "vma_tree_remove_sink_steps=%llu "
           "vma_tree_max_remove_sink_steps=%llu "
           "vma_tree_remove_budget_exceeded=%llu "
           "mmap_free_tree_insertions=%llu "
           "mmap_free_tree_rotations=%llu "
           "mmap_free_tree_priority_ties=%llu "
           "mmap_free_tree_priority_bucket_mask=%llu "
           "mmap_free_tree_priority_bucket_used=%llu "
           "mmap_free_tree_priority_bucket_max=%llu "
           "mmap_free_tree_priority_mix_calls=%llu "
           "mmap_free_tree_priority_mix_ns=%llu "
           "mmap_free_tree_priority_mix_max_ns=%llu "
           "mmap_free_tree_max_insert_depth=%llu "
           "mmap_free_tree_tracked_nodes=%llu "
           "mmap_free_tree_max_nodes=%llu "
           "mmap_free_tree_depth_limit_exceeded=%llu "
           "mmap_free_tree_insert_budget_exceeded=%llu "
           "mmap_free_tree_insert_rotation_budget_exceeded=%llu "
           "mmap_free_tree_find_calls=%llu "
           "mmap_free_tree_find_steps=%llu "
           "mmap_free_tree_max_find_steps=%llu "
           "mmap_free_tree_find_budget_exceeded=%llu "
           "mmap_free_tree_remove_calls=%llu "
           "mmap_free_tree_remove_sink_steps=%llu "
           "mmap_free_tree_max_remove_sink_steps=%llu "
           "mmap_free_tree_remove_budget_exceeded=%llu "
           "mmap_recent_free_attempts=%llu "
           "mmap_recent_free_hits=%llu "
           "mmap_recent_free_misses=%llu "
           "mmap_recent_free_too_small=%llu "
           "mmap_recent_free_best_fit_replacements=%llu "
           "mmap_size_hint_attempts=%llu "
           "mmap_size_hint_hits=%llu "
           "mmap_size_hint_misses=%llu "
           "mmap_size_hint_stale=%llu "
           "mmap_size_hint_too_small=%llu "
           "mmap_size_hint_best_fit_replacements=%llu "
           "page_span_calls=%llu "
           "page_span_pages=%llu "
           "page_mark_calls=%llu "
           "page_mark_pages=%llu "
           "page_mark_max_pages=%llu "
           "page_mark_budget_exceeded=%llu "
           "page_mark_leaf_spans=%llu "
           "page_mark_max_leaf_spans=%llu "
           "page_range_query_calls=%llu "
           "page_range_query_pages=%llu "
           "page_range_query_max_pages=%llu "
           "page_range_query_budget_exceeded=%llu "
           "page_range_query_leaf_spans=%llu "
           "page_range_query_max_leaf_spans=%llu "
           "page_lock_range_calls=%llu "
           "page_lock_range_shards=%llu "
           "page_lock_range_max_shards=%llu "
           "page_lock_range_budget_exceeded=%llu "
           "mprotect_mmap_noop_calls=%llu "
           "mprotect_page_private_noop_calls=%llu "
           "mprotect_platform_calls=%llu "
           "mprotect_publish_calls=%llu "
           "mprotect_rollback_calls=%llu "
           "mprotect_vma_collect_queries=%llu "
           "mprotect_vma_noop_queries=%llu "
           "mprotect_vma_mark_queries=%llu "
           "mprotect_vma_restore_queries=%llu "
           "mprotect_vma_publish_queries=%llu "
           "mprotect_vma_max_segments=%llu "
           "munmap_vma_collect_queries=%llu "
           "munmap_vma_publish_queries=%llu "
           "munmap_vma_max_segments=%llu "
           "munmap_vma_trim_repair_elided=%llu "
           "munmap_vma_trim_repair_elided_pages=%llu "
           "decommit_events=%llu "
           "decommit_bytes=%llu "
           "recommit_events=%llu "
           "recommit_bytes=%llu "
           "malloc_free_decommit_commit_all_fast_paths=%llu "
           "malloc_free_decommit_deferred_events=%llu "
           "malloc_free_decommit_deferred_bytes=%llu "
           "malloc_free_decommit_retained_bytes=%llu "
           "malloc_free_decommit_retention_base_high_water_bytes=%llu "
           "malloc_free_decommit_retention_high_water_bytes=%llu "
           "malloc_free_decommit_retention_max_high_water_bytes=%llu "
           "malloc_free_decommit_retention_pressure_events=%llu "
           "malloc_free_decommit_retention_pressure_bytes=%llu "
           "malloc_free_decommit_retention_adaptive_grow_events=%llu "
           "malloc_free_decommit_retention_adaptive_grow_bytes=%llu "
           "malloc_free_decommit_retention_adaptive_shrink_events=%llu "
           "malloc_free_decommit_retention_adaptive_shrink_bytes=%llu "
           "malloc_free_decommit_hot_deferred_events=%llu "
           "malloc_free_decommit_hot_deferred_bytes=%llu "
           "slab_empty_committed_cache_low_water=%llu "
           "slab_empty_committed_cache_base_high_water=%llu "
           "slab_empty_committed_cache_high_water=%llu "
           "slab_empty_committed_cache_max_high_water=%llu "
           "slab_empty_decommit_pressure_events=%llu "
           "slab_empty_decommit_pressure_bytes=%llu "
           "slab_empty_committed_cache_adaptive_grow_events=%llu "
           "slab_empty_committed_cache_adaptive_grow_bytes=%llu "
           "decommit_recommit_same_addr_events=%llu "
           "decommit_recommit_same_addr_bytes=%llu "
           "malloc_free_tree_current_nodes=%llu "
           "malloc_free_tree_current_depth=%llu "
           "vma_tree_current_nodes=%llu "
           "vma_tree_current_depth=%llu "
           "mmap_free_tree_current_nodes=%llu "
           "mmap_free_tree_current_depth=%llu "
           "vma_list_nodes=%llu "
           "mmap_free_list_nodes=%llu "
           "retired_vma_count=%llu "
           "retired_free_extent_count=%llu "
           "hot_vma_count=%llu "
           "hot_free_extent_count=%llu "
           "metadata_hot_vma_hits=%llu "
           "metadata_hot_vma_misses=%llu "
           "metadata_hot_vma_reclaimed=%llu "
           "metadata_hot_vma_full_defers=%llu "
           "metadata_hot_free_extent_hits=%llu "
           "metadata_hot_free_extent_misses=%llu "
           "metadata_hot_free_extent_reclaimed=%llu "
           "metadata_hot_free_extent_full_defers=%llu "
           "metadata_quarantine_drain_calls=%llu "
           "metadata_quarantine_drain_scanned=%llu "
           "metadata_quarantine_drain_reclaimed=%llu "
           "metadata_quarantine_drain_budget_exhausted=%llu "
           "metadata_quarantine_drain_rotated=%llu "
           "control_alloc_recent_hits=%llu "
           "control_alloc_hint_hits=%llu "
           "control_alloc_hint_misses=%llu "
           "control_alloc_hint_stale=%llu "
           "control_alloc_no_fit_fast_paths=%llu "
           "control_alloc_scan_calls=%llu "
           "control_alloc_scan_steps=%llu "
           "control_alloc_scan_max_steps=%llu "
           "control_alloc_scan_budget_exceeded=%llu "
           "vm_reserve_calls=%llu "
           "vm_reserve_failures=%llu "
           "vm_reserve_ns=%llu "
           "vm_commit_calls=%llu "
           "vm_commit_failures=%llu "
           "vm_commit_ns=%llu "
           "vm_decommit_calls=%llu "
           "vm_decommit_failures=%llu "
           "vm_decommit_ns=%llu "
           "vm_fixed_remap_calls=%llu "
           "vm_fixed_remap_failures=%llu "
           "vm_fixed_remap_ns=%llu "
           "vm_protect_calls=%llu "
           "vm_protect_failures=%llu "
           "vm_protect_ns=%llu "
           "vm_release_calls=%llu "
           "vm_release_failures=%llu "
           "vm_release_ns=%llu "
           "vm_numa_hint_calls=%llu "
           "vm_numa_hint_failures=%llu "
           "vm_numa_hint_ns=%llu "
           "vm_lock_held_calls=%llu "
           "vm_lock_held_failures=%llu "
           "vm_lock_held_ns=%llu "
           "vm_reserve_lock_held_calls=%llu "
           "vm_commit_lock_held_calls=%llu "
           "vm_decommit_lock_held_calls=%llu "
           "vm_fixed_remap_lock_held_calls=%llu "
           "vm_protect_lock_held_calls=%llu "
           "vm_release_lock_held_calls=%llu "
           "vm_numa_hint_lock_held_calls=%llu "
           "heap_lock_calls=%llu "
           "heap_lock_try_calls=%llu "
           "heap_lock_try_failures=%llu "
           "heap_lock_contended_calls=%llu "
           "heap_lock_wait_ns=%llu "
           "heap_lock_max_wait_ns=%llu "
           "heap_lock_hold_ns=%llu "
           "heap_lock_max_hold_ns=%llu "
           "slab_raw_lock_calls=%llu "
           "slab_raw_lock_try_calls=%llu "
           "slab_raw_lock_try_failures=%llu "
           "slab_raw_lock_contended_calls=%llu "
           "slab_raw_lock_wait_ns=%llu "
           "slab_raw_lock_max_wait_ns=%llu "
           "slab_raw_lock_hold_ns=%llu "
           "slab_raw_lock_max_hold_ns=%llu\n",
           name,
           (unsigned long long)(after->treap_priority_mix_calls -
                                before->treap_priority_mix_calls),
           (unsigned long long)(after->treap_priority_mix_ns -
                                before->treap_priority_mix_ns),
           (unsigned long long)bench_diag_max_delta(
               before->treap_priority_mix_max_ns,
               after->treap_priority_mix_max_ns),
           (unsigned long long)after->treap_health_flags,
           (unsigned long long)after->live_block_health_flags,
           (unsigned long long)after->treap_repair_pending_flags,
           (unsigned long long)(after->treap_repair_rebuilds -
                                before->treap_repair_rebuilds),
           (unsigned long long)(after->treap_repair_rehashes -
                                before->treap_repair_rehashes),
           (unsigned long long)(after->treap_repair_nodes -
                                before->treap_repair_nodes),
           (unsigned long long)(after->treap_repair_failures -
                                before->treap_repair_failures),
           (unsigned long long)(after->live_block_fast_hits -
                                before->live_block_fast_hits),
           (unsigned long long)(after->live_block_slow_calls -
                                before->live_block_slow_calls),
           (unsigned long long)(after->live_block_slow_hits -
                                before->live_block_slow_hits),
           (unsigned long long)(after->live_block_slow_misses -
                                before->live_block_slow_misses),
           (unsigned long long)(after->live_block_slow_scanned_nodes -
                                before->live_block_slow_scanned_nodes),
           (unsigned long long)bench_diag_max_delta(
               before->live_block_slow_max_scanned_nodes,
               after->live_block_slow_max_scanned_nodes),
           (unsigned long long)(after->live_block_slow_ns -
                                before->live_block_slow_ns),
           (unsigned long long)bench_diag_max_delta(
               before->live_block_slow_max_ns,
               after->live_block_slow_max_ns),
           (unsigned long long)(after->live_block_fast_fail_invalid -
                                before->live_block_fast_fail_invalid),
           (unsigned long long)(after->live_block_fast_fail_range -
                                before->live_block_fast_fail_range),
           (unsigned long long)(after->live_block_fast_fail_header_page -
                                before->live_block_fast_fail_header_page),
           (unsigned long long)(after->live_block_fast_fail_payload_page -
                                before->live_block_fast_fail_payload_page),
           (unsigned long long)(after->live_block_fast_fail_descriptor -
                                before->live_block_fast_fail_descriptor),
           (unsigned long long)(after->live_block_fast_terminal_misses -
                                before->live_block_fast_terminal_misses),
           (unsigned long long)(after->live_block_tree_insertions -
                                before->live_block_tree_insertions),
           (unsigned long long)(after->live_block_tree_rotations -
                                before->live_block_tree_rotations),
           (unsigned long long)(after->live_block_tree_priority_ties -
                                before->live_block_tree_priority_ties),
           (unsigned long long)after->live_block_tree_priority_bucket_mask,
           (unsigned long long)bench_diag_popcount64(
               after->live_block_tree_priority_bucket_mask),
           (unsigned long long)after->live_block_tree_priority_bucket_max,
           (unsigned long long)(after->live_block_tree_priority_mix_calls -
                                before->live_block_tree_priority_mix_calls),
           (unsigned long long)(after->live_block_tree_priority_mix_ns -
                                before->live_block_tree_priority_mix_ns),
           (unsigned long long)bench_diag_max_delta(
               before->live_block_tree_priority_mix_max_ns,
               after->live_block_tree_priority_mix_max_ns),
           (unsigned long long)bench_diag_max_delta(
               before->live_block_tree_max_insert_depth,
               after->live_block_tree_max_insert_depth),
           (unsigned long long)after->live_block_tree_tracked_nodes,
           (unsigned long long)bench_diag_max_delta(
               before->live_block_tree_max_nodes,
               after->live_block_tree_max_nodes),
            (unsigned long long)(
                after->live_block_tree_depth_limit_exceeded -
                before->live_block_tree_depth_limit_exceeded),
            (unsigned long long)(
                after->live_block_tree_insert_budget_exceeded -
                before->live_block_tree_insert_budget_exceeded),
            (unsigned long long)(
                after->live_block_tree_insert_rotation_budget_exceeded -
                before->live_block_tree_insert_rotation_budget_exceeded),
            (unsigned long long)(after->live_block_tree_find_calls -
                                 before->live_block_tree_find_calls),
           (unsigned long long)(after->live_block_tree_find_steps -
                                before->live_block_tree_find_steps),
           (unsigned long long)bench_diag_max_delta(
               before->live_block_tree_max_find_steps,
               after->live_block_tree_max_find_steps),
           (unsigned long long)(
               after->live_block_tree_find_budget_exceeded -
               before->live_block_tree_find_budget_exceeded),
           (unsigned long long)after->live_block_tree_mutation_epoch,
           (unsigned long long)(after->live_block_tree_remove_calls -
                                before->live_block_tree_remove_calls),
           (unsigned long long)(
               after->live_block_tree_remove_sink_steps -
               before->live_block_tree_remove_sink_steps),
           (unsigned long long)bench_diag_max_delta(
               before->live_block_tree_max_remove_sink_steps,
               after->live_block_tree_max_remove_sink_steps),
           (unsigned long long)(
               after->live_block_tree_remove_budget_exceeded -
               before->live_block_tree_remove_budget_exceeded),
           (unsigned long long)(after->live_block_slow_tree_hits -
                                before->live_block_slow_tree_hits),
           (unsigned long long)(after->live_block_slow_tree_misses -
                                before->live_block_slow_tree_misses),
           (unsigned long long)(after->live_block_slow_fallback_scans -
                                before->live_block_slow_fallback_scans),
           (unsigned long long)(
               after->live_block_slow_fallback_scanned_nodes -
               before->live_block_slow_fallback_scanned_nodes),
           (unsigned long long)bench_diag_max_delta(
               before->live_block_slow_fallback_max_scanned_nodes,
               after->live_block_slow_fallback_max_scanned_nodes),
           (unsigned long long)(
               after->live_block_slow_fallback_budget_exceeded -
               before->live_block_slow_fallback_budget_exceeded),
           (unsigned long long)(after->live_block_slow_fallback_aborts -
                                before->live_block_slow_fallback_aborts),
           (unsigned long long)(
               after->live_block_slow_fallback_negative_stores -
               before->live_block_slow_fallback_negative_stores),
           (unsigned long long)(
               after->live_block_slow_fallback_negative_hits -
               before->live_block_slow_fallback_negative_hits),
           (unsigned long long)(
               after->live_block_slow_fallback_negative_evictions -
               before->live_block_slow_fallback_negative_evictions),
           (unsigned long long)after->live_block_slow_fallback_throttle_epoch,
           (unsigned long long)after->live_block_slow_fallback_epoch_scans,
           (unsigned long long)(after->live_block_slow_fallback_throttles -
                                before->live_block_slow_fallback_throttles),
           (unsigned long long)after->live_block_slow_fallback_scenario_epoch,
           (unsigned long long)(
               after->live_block_slow_fallback_scenario_checks -
               before->live_block_slow_fallback_scenario_checks),
           (unsigned long long)(
               after->live_block_slow_fallback_scenario_throttles -
               before->live_block_slow_fallback_scenario_throttles),
           (unsigned long long)(after->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN] -
                                before->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN]),
           (unsigned long long)(after->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN] -
                                before->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN]),
           (unsigned long long)(
               after->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN] -
               before->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN]),
           (unsigned long long)(after->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN] -
                                before->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN]),
           (unsigned long long)after->live_block_lookup_scenario_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN],
           (unsigned long long)after->live_block_lookup_scenario_candidates[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN],
           (unsigned long long)(after->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] -
                                before->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE]),
           (unsigned long long)(after->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] -
                                before->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE]),
           (unsigned long long)(
               after->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] -
               before->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE]),
           (unsigned long long)(after->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE] -
                                before->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE]),
           (unsigned long long)after->live_block_lookup_scenario_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE],
           (unsigned long long)after->live_block_lookup_scenario_candidates[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE],
           (unsigned long long)(after->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC] -
                                before->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC]),
           (unsigned long long)(after->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC] -
                                before->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC]),
           (unsigned long long)(
               after->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC] -
               before->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC]),
           (unsigned long long)(after->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC] -
                                before->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC]),
           (unsigned long long)after->live_block_lookup_scenario_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC],
           (unsigned long long)after->live_block_lookup_scenario_candidates[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC],
           (unsigned long long)(after->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] -
                                before->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]),
           (unsigned long long)(after->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] -
                                before->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]),
           (unsigned long long)(
               after->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] -
               before->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]),
           (unsigned long long)(after->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE] -
                                before->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE]),
           (unsigned long long)after->live_block_lookup_scenario_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE],
           (unsigned long long)after->live_block_lookup_scenario_candidates[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE],
           (unsigned long long)(after->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED] -
                                before->live_block_lookup_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED]),
           (unsigned long long)(after->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED] -
                                before->live_block_lookup_slow_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED]),
           (unsigned long long)(
               after->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED] -
               before->live_block_lookup_fallback_scans[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED]),
           (unsigned long long)(after->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED] -
                                before->live_block_lookup_throttles[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED]),
           (unsigned long long)after->live_block_lookup_scenario_calls[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED],
           (unsigned long long)after->live_block_lookup_scenario_candidates[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN][UHM_LIVE_BLOCK_FAST_FAIL_INVALID],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN][UHM_LIVE_BLOCK_FAST_FAIL_RANGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN][UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN][UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN][UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE][UHM_LIVE_BLOCK_FAST_FAIL_INVALID],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE][UHM_LIVE_BLOCK_FAST_FAIL_RANGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE][UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE][UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE][UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC][UHM_LIVE_BLOCK_FAST_FAIL_INVALID],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC][UHM_LIVE_BLOCK_FAST_FAIL_RANGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC][UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC][UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC][UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE][UHM_LIVE_BLOCK_FAST_FAIL_INVALID],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE][UHM_LIVE_BLOCK_FAST_FAIL_RANGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE][UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE][UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE][UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED][UHM_LIVE_BLOCK_FAST_FAIL_INVALID],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED][UHM_LIVE_BLOCK_FAST_FAIL_RANGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED][UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED][UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE],
           (unsigned long long)live_lookup_fast_fail_delta[UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED][UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR],
           (unsigned long long)(after->malloc_free_tree_insertions -
                                before->malloc_free_tree_insertions),
           (unsigned long long)(after->malloc_free_tree_rotations -
                                before->malloc_free_tree_rotations),
           (unsigned long long)(after->malloc_free_tree_priority_ties -
                                before->malloc_free_tree_priority_ties),
           (unsigned long long)after->malloc_free_tree_priority_bucket_mask,
           (unsigned long long)bench_diag_popcount64(
               after->malloc_free_tree_priority_bucket_mask),
           (unsigned long long)after->malloc_free_tree_priority_bucket_max,
           (unsigned long long)(after->malloc_free_tree_priority_mix_calls -
                                before->malloc_free_tree_priority_mix_calls),
           (unsigned long long)(after->malloc_free_tree_priority_mix_ns -
                                before->malloc_free_tree_priority_mix_ns),
           (unsigned long long)bench_diag_max_delta(
               before->malloc_free_tree_priority_mix_max_ns,
               after->malloc_free_tree_priority_mix_max_ns),
           (unsigned long long)bench_diag_max_delta(
               before->malloc_free_tree_max_insert_depth,
               after->malloc_free_tree_max_insert_depth),
           (unsigned long long)after->malloc_free_tree_tracked_nodes,
           (unsigned long long)bench_diag_max_delta(
               before->malloc_free_tree_max_nodes,
               after->malloc_free_tree_max_nodes),
            (unsigned long long)(after->malloc_free_tree_depth_limit_exceeded -
                                 before->malloc_free_tree_depth_limit_exceeded),
            (unsigned long long)(
                after->malloc_free_tree_insert_budget_exceeded -
                before->malloc_free_tree_insert_budget_exceeded),
            (unsigned long long)(
                after->malloc_free_tree_insert_rotation_budget_exceeded -
                before->malloc_free_tree_insert_rotation_budget_exceeded),
            (unsigned long long)(after->malloc_free_tree_find_calls -
                                 before->malloc_free_tree_find_calls),
           (unsigned long long)(after->malloc_free_tree_find_steps -
                                before->malloc_free_tree_find_steps),
           (unsigned long long)bench_diag_max_delta(
               before->malloc_free_tree_max_find_steps,
               after->malloc_free_tree_max_find_steps),
           (unsigned long long)(
               after->malloc_free_tree_find_budget_exceeded -
               before->malloc_free_tree_find_budget_exceeded),
           (unsigned long long)(after->malloc_free_tree_remove_calls -
                                before->malloc_free_tree_remove_calls),
           (unsigned long long)(
               after->malloc_free_tree_remove_sink_steps -
               before->malloc_free_tree_remove_sink_steps),
           (unsigned long long)bench_diag_max_delta(
               before->malloc_free_tree_max_remove_sink_steps,
               after->malloc_free_tree_max_remove_sink_steps),
           (unsigned long long)(
               after->malloc_free_tree_remove_budget_exceeded -
               before->malloc_free_tree_remove_budget_exceeded),
           (unsigned long long)(after->vma_tree_insertions -
                                before->vma_tree_insertions),
           (unsigned long long)(after->vma_tree_rotations -
                                before->vma_tree_rotations),
           (unsigned long long)(after->vma_tree_priority_ties -
                                before->vma_tree_priority_ties),
           (unsigned long long)after->vma_tree_priority_bucket_mask,
           (unsigned long long)bench_diag_popcount64(
               after->vma_tree_priority_bucket_mask),
           (unsigned long long)after->vma_tree_priority_bucket_max,
           (unsigned long long)(after->vma_tree_priority_mix_calls -
                                before->vma_tree_priority_mix_calls),
           (unsigned long long)(after->vma_tree_priority_mix_ns -
                                before->vma_tree_priority_mix_ns),
           (unsigned long long)bench_diag_max_delta(
               before->vma_tree_priority_mix_max_ns,
               after->vma_tree_priority_mix_max_ns),
           (unsigned long long)bench_diag_max_delta(
               before->vma_tree_max_insert_depth,
               after->vma_tree_max_insert_depth),
           (unsigned long long)after->vma_tree_tracked_nodes,
           (unsigned long long)bench_diag_max_delta(
               before->vma_tree_max_nodes, after->vma_tree_max_nodes),
            (unsigned long long)(after->vma_tree_depth_limit_exceeded -
                                 before->vma_tree_depth_limit_exceeded),
            (unsigned long long)(after->vma_tree_insert_budget_exceeded -
                                 before->vma_tree_insert_budget_exceeded),
            (unsigned long long)(
                after->vma_tree_insert_rotation_budget_exceeded -
                before->vma_tree_insert_rotation_budget_exceeded),
            (unsigned long long)(after->vma_tree_find_calls -
                                 before->vma_tree_find_calls),
           (unsigned long long)(after->vma_tree_find_steps -
                                before->vma_tree_find_steps),
           (unsigned long long)bench_diag_max_delta(
               before->vma_tree_max_find_steps,
               after->vma_tree_max_find_steps),
           (unsigned long long)(after->vma_tree_find_budget_exceeded -
                                before->vma_tree_find_budget_exceeded),
           (unsigned long long)(after->vma_tree_remove_calls -
                                before->vma_tree_remove_calls),
           (unsigned long long)(after->vma_tree_remove_sink_steps -
                                before->vma_tree_remove_sink_steps),
           (unsigned long long)bench_diag_max_delta(
               before->vma_tree_max_remove_sink_steps,
               after->vma_tree_max_remove_sink_steps),
           (unsigned long long)(after->vma_tree_remove_budget_exceeded -
                                before->vma_tree_remove_budget_exceeded),
           (unsigned long long)(after->mmap_free_tree_insertions -
                                before->mmap_free_tree_insertions),
           (unsigned long long)(after->mmap_free_tree_rotations -
                                before->mmap_free_tree_rotations),
           (unsigned long long)(after->mmap_free_tree_priority_ties -
                                before->mmap_free_tree_priority_ties),
           (unsigned long long)after->mmap_free_tree_priority_bucket_mask,
           (unsigned long long)bench_diag_popcount64(
               after->mmap_free_tree_priority_bucket_mask),
           (unsigned long long)after->mmap_free_tree_priority_bucket_max,
           (unsigned long long)(after->mmap_free_tree_priority_mix_calls -
                                before->mmap_free_tree_priority_mix_calls),
           (unsigned long long)(after->mmap_free_tree_priority_mix_ns -
                                before->mmap_free_tree_priority_mix_ns),
           (unsigned long long)bench_diag_max_delta(
               before->mmap_free_tree_priority_mix_max_ns,
               after->mmap_free_tree_priority_mix_max_ns),
           (unsigned long long)bench_diag_max_delta(
               before->mmap_free_tree_max_insert_depth,
               after->mmap_free_tree_max_insert_depth),
           (unsigned long long)after->mmap_free_tree_tracked_nodes,
           (unsigned long long)bench_diag_max_delta(
               before->mmap_free_tree_max_nodes,
               after->mmap_free_tree_max_nodes),
            (unsigned long long)(after->mmap_free_tree_depth_limit_exceeded -
                                 before->mmap_free_tree_depth_limit_exceeded),
            (unsigned long long)(
                after->mmap_free_tree_insert_budget_exceeded -
                before->mmap_free_tree_insert_budget_exceeded),
            (unsigned long long)(
                after->mmap_free_tree_insert_rotation_budget_exceeded -
                before->mmap_free_tree_insert_rotation_budget_exceeded),
            (unsigned long long)(after->mmap_free_tree_find_calls -
                                 before->mmap_free_tree_find_calls),
           (unsigned long long)(after->mmap_free_tree_find_steps -
                                before->mmap_free_tree_find_steps),
           (unsigned long long)bench_diag_max_delta(
               before->mmap_free_tree_max_find_steps,
               after->mmap_free_tree_max_find_steps),
           (unsigned long long)(
               after->mmap_free_tree_find_budget_exceeded -
               before->mmap_free_tree_find_budget_exceeded),
           (unsigned long long)(after->mmap_free_tree_remove_calls -
                                before->mmap_free_tree_remove_calls),
           (unsigned long long)(
               after->mmap_free_tree_remove_sink_steps -
               before->mmap_free_tree_remove_sink_steps),
           (unsigned long long)bench_diag_max_delta(
               before->mmap_free_tree_max_remove_sink_steps,
               after->mmap_free_tree_max_remove_sink_steps),
           (unsigned long long)(
               after->mmap_free_tree_remove_budget_exceeded -
               before->mmap_free_tree_remove_budget_exceeded),
           (unsigned long long)(after->mmap_recent_free_attempts -
                                before->mmap_recent_free_attempts),
           (unsigned long long)(after->mmap_recent_free_hits -
                                before->mmap_recent_free_hits),
           (unsigned long long)(after->mmap_recent_free_misses -
                                before->mmap_recent_free_misses),
           (unsigned long long)(after->mmap_recent_free_too_small -
                                before->mmap_recent_free_too_small),
           (unsigned long long)(
               after->mmap_recent_free_best_fit_replacements -
               before->mmap_recent_free_best_fit_replacements),
           (unsigned long long)(after->mmap_size_hint_attempts -
                                before->mmap_size_hint_attempts),
           (unsigned long long)(after->mmap_size_hint_hits -
                                before->mmap_size_hint_hits),
           (unsigned long long)(after->mmap_size_hint_misses -
                                before->mmap_size_hint_misses),
           (unsigned long long)(after->mmap_size_hint_stale -
                                before->mmap_size_hint_stale),
           (unsigned long long)(after->mmap_size_hint_too_small -
                                before->mmap_size_hint_too_small),
           (unsigned long long)(
               after->mmap_size_hint_best_fit_replacements -
               before->mmap_size_hint_best_fit_replacements),
           (unsigned long long)(after->page_span_calls -
                                before->page_span_calls),
           (unsigned long long)(after->page_span_pages -
                                before->page_span_pages),
           (unsigned long long)(after->page_mark_calls -
                                before->page_mark_calls),
           (unsigned long long)(after->page_mark_pages -
                                before->page_mark_pages),
           (unsigned long long)bench_diag_max_delta(
               before->page_mark_max_pages, after->page_mark_max_pages),
           (unsigned long long)(after->page_mark_budget_exceeded -
                                before->page_mark_budget_exceeded),
           (unsigned long long)(after->page_mark_leaf_spans -
                                before->page_mark_leaf_spans),
           (unsigned long long)bench_diag_max_delta(
               before->page_mark_max_leaf_spans,
               after->page_mark_max_leaf_spans),
           (unsigned long long)(after->page_range_query_calls -
                                before->page_range_query_calls),
           (unsigned long long)(after->page_range_query_pages -
                                before->page_range_query_pages),
           (unsigned long long)bench_diag_max_delta(
               before->page_range_query_max_pages,
               after->page_range_query_max_pages),
           (unsigned long long)(
               after->page_range_query_budget_exceeded -
               before->page_range_query_budget_exceeded),
           (unsigned long long)(after->page_range_query_leaf_spans -
                                before->page_range_query_leaf_spans),
           (unsigned long long)bench_diag_max_delta(
               before->page_range_query_max_leaf_spans,
               after->page_range_query_max_leaf_spans),
           (unsigned long long)(after->page_lock_range_calls -
                                before->page_lock_range_calls),
           (unsigned long long)(after->page_lock_range_shards -
                                before->page_lock_range_shards),
           (unsigned long long)bench_diag_max_delta(
               before->page_lock_range_max_shards,
               after->page_lock_range_max_shards),
           (unsigned long long)(after->page_lock_range_budget_exceeded -
                                before->page_lock_range_budget_exceeded),
           (unsigned long long)(after->mprotect_mmap_noop_calls -
                                before->mprotect_mmap_noop_calls),
           (unsigned long long)(
               after->mprotect_page_private_noop_calls -
               before->mprotect_page_private_noop_calls),
           (unsigned long long)(after->mprotect_platform_calls -
                                before->mprotect_platform_calls),
           (unsigned long long)(after->mprotect_publish_calls -
                                before->mprotect_publish_calls),
           (unsigned long long)(after->mprotect_rollback_calls -
                                before->mprotect_rollback_calls),
           (unsigned long long)(after->mprotect_vma_collect_queries -
                                before->mprotect_vma_collect_queries),
           (unsigned long long)(after->mprotect_vma_noop_queries -
                                before->mprotect_vma_noop_queries),
           (unsigned long long)(after->mprotect_vma_mark_queries -
                                before->mprotect_vma_mark_queries),
           (unsigned long long)(after->mprotect_vma_restore_queries -
                                before->mprotect_vma_restore_queries),
           (unsigned long long)(after->mprotect_vma_publish_queries -
                                before->mprotect_vma_publish_queries),
           (unsigned long long)bench_diag_max_delta(
               before->mprotect_vma_max_segments,
               after->mprotect_vma_max_segments),
           (unsigned long long)(after->munmap_vma_collect_queries -
                                before->munmap_vma_collect_queries),
           (unsigned long long)(after->munmap_vma_publish_queries -
                                before->munmap_vma_publish_queries),
           (unsigned long long)bench_diag_max_delta(
               before->munmap_vma_max_segments,
               after->munmap_vma_max_segments),
           (unsigned long long)(after->munmap_vma_trim_repair_elided -
                                before->munmap_vma_trim_repair_elided),
           (unsigned long long)(
               after->munmap_vma_trim_repair_elided_pages -
               before->munmap_vma_trim_repair_elided_pages),
           (unsigned long long)(after->decommit_events -
                                before->decommit_events),
           (unsigned long long)(after->decommit_bytes -
                                before->decommit_bytes),
           (unsigned long long)(after->recommit_events -
                                before->recommit_events),
           (unsigned long long)(after->recommit_bytes -
                                before->recommit_bytes),
           (unsigned long long)(
               after->malloc_free_decommit_commit_all_fast_paths -
               before->malloc_free_decommit_commit_all_fast_paths),
           (unsigned long long)(
               after->malloc_free_decommit_deferred_events -
               before->malloc_free_decommit_deferred_events),
           (unsigned long long)(
               after->malloc_free_decommit_deferred_bytes -
               before->malloc_free_decommit_deferred_bytes),
           (unsigned long long)after->malloc_free_decommit_retained_bytes,
           (unsigned long long)after
               ->malloc_free_decommit_retention_base_high_water_bytes,
           (unsigned long long)
               after->malloc_free_decommit_retention_high_water_bytes,
           (unsigned long long)after
               ->malloc_free_decommit_retention_max_high_water_bytes,
           (unsigned long long)(
               after->malloc_free_decommit_retention_pressure_events -
               before->malloc_free_decommit_retention_pressure_events),
           (unsigned long long)(
               after->malloc_free_decommit_retention_pressure_bytes -
               before->malloc_free_decommit_retention_pressure_bytes),
           (unsigned long long)(
               after->malloc_free_decommit_retention_adaptive_grow_events -
               before->malloc_free_decommit_retention_adaptive_grow_events),
           (unsigned long long)(
               after->malloc_free_decommit_retention_adaptive_grow_bytes -
               before->malloc_free_decommit_retention_adaptive_grow_bytes),
           (unsigned long long)(
               after->malloc_free_decommit_retention_adaptive_shrink_events -
               before->malloc_free_decommit_retention_adaptive_shrink_events),
           (unsigned long long)(
               after->malloc_free_decommit_retention_adaptive_shrink_bytes -
               before->malloc_free_decommit_retention_adaptive_shrink_bytes),
           (unsigned long long)(
               after->malloc_free_decommit_hot_deferred_events -
               before->malloc_free_decommit_hot_deferred_events),
           (unsigned long long)(
               after->malloc_free_decommit_hot_deferred_bytes -
               before->malloc_free_decommit_hot_deferred_bytes),
           (unsigned long long)after->slab_empty_committed_cache_low_water,
           (unsigned long long)
               after->slab_empty_committed_cache_base_high_water,
           (unsigned long long)after->slab_empty_committed_cache_high_water,
           (unsigned long long)after->slab_empty_committed_cache_max_high_water,
           (unsigned long long)(
               after->slab_empty_decommit_pressure_events -
               before->slab_empty_decommit_pressure_events),
           (unsigned long long)(
               after->slab_empty_decommit_pressure_bytes -
               before->slab_empty_decommit_pressure_bytes),
           (unsigned long long)(
               after->slab_empty_committed_cache_adaptive_grow_events -
               before->slab_empty_committed_cache_adaptive_grow_events),
           (unsigned long long)(
               after->slab_empty_committed_cache_adaptive_grow_bytes -
               before->slab_empty_committed_cache_adaptive_grow_bytes),
           (unsigned long long)(
               after->decommit_recommit_same_addr_events -
               before->decommit_recommit_same_addr_events),
           (unsigned long long)(
               after->decommit_recommit_same_addr_bytes -
               before->decommit_recommit_same_addr_bytes),
           (unsigned long long)after->malloc_free_tree_current_nodes,
           (unsigned long long)after->malloc_free_tree_current_depth,
           (unsigned long long)after->vma_tree_current_nodes,
           (unsigned long long)after->vma_tree_current_depth,
           (unsigned long long)after->mmap_free_tree_current_nodes,
           (unsigned long long)after->mmap_free_tree_current_depth,
           (unsigned long long)after->vma_list_nodes,
           (unsigned long long)after->mmap_free_list_nodes,
           (unsigned long long)after->retired_vma_count,
           (unsigned long long)after->retired_free_extent_count,
           (unsigned long long)after->hot_vma_count,
           (unsigned long long)after->hot_free_extent_count,
           (unsigned long long)(after->metadata_hot_vma_hits -
                                before->metadata_hot_vma_hits),
           (unsigned long long)(after->metadata_hot_vma_misses -
                                before->metadata_hot_vma_misses),
           (unsigned long long)(after->metadata_hot_vma_reclaimed -
                                before->metadata_hot_vma_reclaimed),
           (unsigned long long)(after->metadata_hot_vma_full_defers -
                                before->metadata_hot_vma_full_defers),
           (unsigned long long)(after->metadata_hot_free_extent_hits -
                                before->metadata_hot_free_extent_hits),
           (unsigned long long)(after->metadata_hot_free_extent_misses -
                                before->metadata_hot_free_extent_misses),
           (unsigned long long)(
               after->metadata_hot_free_extent_reclaimed -
               before->metadata_hot_free_extent_reclaimed),
           (unsigned long long)(
               after->metadata_hot_free_extent_full_defers -
               before->metadata_hot_free_extent_full_defers),
           (unsigned long long)(after->metadata_quarantine_drain_calls -
                                before->metadata_quarantine_drain_calls),
           (unsigned long long)(after->metadata_quarantine_drain_scanned -
                                before->metadata_quarantine_drain_scanned),
           (unsigned long long)(after->metadata_quarantine_drain_reclaimed -
                                before->metadata_quarantine_drain_reclaimed),
           (unsigned long long)(
               after->metadata_quarantine_drain_budget_exhausted -
               before->metadata_quarantine_drain_budget_exhausted),
           (unsigned long long)(after->metadata_quarantine_drain_rotated -
                                before->metadata_quarantine_drain_rotated),
           (unsigned long long)(after->control_alloc_recent_hits -
                                before->control_alloc_recent_hits),
           (unsigned long long)(after->control_alloc_hint_hits -
                                before->control_alloc_hint_hits),
           (unsigned long long)(after->control_alloc_hint_misses -
                                before->control_alloc_hint_misses),
           (unsigned long long)(after->control_alloc_hint_stale -
                                before->control_alloc_hint_stale),
           (unsigned long long)(after->control_alloc_no_fit_fast_paths -
                                before->control_alloc_no_fit_fast_paths),
           (unsigned long long)(after->control_alloc_scan_calls -
                                before->control_alloc_scan_calls),
           (unsigned long long)(after->control_alloc_scan_steps -
                                before->control_alloc_scan_steps),
           (unsigned long long)bench_diag_max_delta(
               before->control_alloc_scan_max_steps,
               after->control_alloc_scan_max_steps),
           (unsigned long long)(
               after->control_alloc_scan_budget_exceeded -
               before->control_alloc_scan_budget_exceeded),
           (unsigned long long)(after->vm_reserve_calls -
                                before->vm_reserve_calls),
           (unsigned long long)(after->vm_reserve_failures -
                                before->vm_reserve_failures),
           (unsigned long long)(after->vm_reserve_ns - before->vm_reserve_ns),
           (unsigned long long)(after->vm_commit_calls -
                                before->vm_commit_calls),
           (unsigned long long)(after->vm_commit_failures -
                                before->vm_commit_failures),
           (unsigned long long)(after->vm_commit_ns - before->vm_commit_ns),
           (unsigned long long)(after->vm_decommit_calls -
                                before->vm_decommit_calls),
           (unsigned long long)(after->vm_decommit_failures -
                                before->vm_decommit_failures),
           (unsigned long long)(after->vm_decommit_ns -
                                before->vm_decommit_ns),
           (unsigned long long)(after->vm_fixed_remap_calls -
                                before->vm_fixed_remap_calls),
           (unsigned long long)(after->vm_fixed_remap_failures -
                                before->vm_fixed_remap_failures),
           (unsigned long long)(after->vm_fixed_remap_ns -
                                before->vm_fixed_remap_ns),
           (unsigned long long)(after->vm_protect_calls -
                                before->vm_protect_calls),
           (unsigned long long)(after->vm_protect_failures -
                                before->vm_protect_failures),
           (unsigned long long)(after->vm_protect_ns - before->vm_protect_ns),
           (unsigned long long)(after->vm_release_calls -
                                before->vm_release_calls),
           (unsigned long long)(after->vm_release_failures -
                                before->vm_release_failures),
           (unsigned long long)(after->vm_release_ns - before->vm_release_ns),
           (unsigned long long)(after->vm_numa_hint_calls -
                                before->vm_numa_hint_calls),
           (unsigned long long)(after->vm_numa_hint_failures -
                                before->vm_numa_hint_failures),
           (unsigned long long)(after->vm_numa_hint_ns -
                                before->vm_numa_hint_ns),
           (unsigned long long)(after->vm_lock_held_calls -
                                before->vm_lock_held_calls),
           (unsigned long long)(after->vm_lock_held_failures -
                                before->vm_lock_held_failures),
           (unsigned long long)(after->vm_lock_held_ns -
                                before->vm_lock_held_ns),
           (unsigned long long)(after->vm_reserve_lock_held_calls -
                                before->vm_reserve_lock_held_calls),
           (unsigned long long)(after->vm_commit_lock_held_calls -
                                before->vm_commit_lock_held_calls),
           (unsigned long long)(after->vm_decommit_lock_held_calls -
                                before->vm_decommit_lock_held_calls),
           (unsigned long long)(
               after->vm_fixed_remap_lock_held_calls -
               before->vm_fixed_remap_lock_held_calls),
           (unsigned long long)(after->vm_protect_lock_held_calls -
                                before->vm_protect_lock_held_calls),
           (unsigned long long)(after->vm_release_lock_held_calls -
                                before->vm_release_lock_held_calls),
           (unsigned long long)(after->vm_numa_hint_lock_held_calls -
                                before->vm_numa_hint_lock_held_calls),
           (unsigned long long)(after->heap_lock_calls -
                                before->heap_lock_calls),
           (unsigned long long)(after->heap_lock_try_calls -
                                before->heap_lock_try_calls),
           (unsigned long long)(after->heap_lock_try_failures -
                                before->heap_lock_try_failures),
           (unsigned long long)(after->heap_lock_contended_calls -
                                before->heap_lock_contended_calls),
           (unsigned long long)(after->heap_lock_wait_ns -
                                before->heap_lock_wait_ns),
            (unsigned long long)after->heap_lock_max_wait_ns,
           (unsigned long long)(after->heap_lock_hold_ns -
                                before->heap_lock_hold_ns),
            (unsigned long long)after->heap_lock_max_hold_ns,
           (unsigned long long)(after->slab_raw_lock_calls -
                                before->slab_raw_lock_calls),
           (unsigned long long)(after->slab_raw_lock_try_calls -
                                before->slab_raw_lock_try_calls),
           (unsigned long long)(after->slab_raw_lock_try_failures -
                                before->slab_raw_lock_try_failures),
           (unsigned long long)(after->slab_raw_lock_contended_calls -
                                before->slab_raw_lock_contended_calls),
           (unsigned long long)(after->slab_raw_lock_wait_ns -
                                before->slab_raw_lock_wait_ns),
            (unsigned long long)after->slab_raw_lock_max_wait_ns,
           (unsigned long long)(after->slab_raw_lock_hold_ns -
                                before->slab_raw_lock_hold_ns),
            (unsigned long long)after->slab_raw_lock_max_hold_ns);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

