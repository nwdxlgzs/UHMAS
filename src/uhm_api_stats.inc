#if UHMAS_ENABLE_DIAGNOSTICS
#define UHM_STATS_DIAG_LOAD_FIELD(heap_, field_) \
    uhm_diag_atomic_load_u64(&(heap_)->core.field_)
#else
#define UHM_STATS_DIAG_LOAD_FIELD(heap_, field_) UINT64_C(0)
#endif

static uhm_result UHM_CALL uhm_api_get_stats(uhm_heap *heap, uhm_stats *out_stats) {
    uint32_t requested_size;
    uint32_t lockless_remote_enabled;
    size_t op_shard;

    if (!out_stats) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    requested_size = out_stats->size;
    if (requested_size < UHM_STATS_SIZE_ABI10) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    lockless_remote_enabled = (uint32_t)uhm_lockless_remote_free_enabled(heap);
    out_stats->size = sizeof(*out_stats);
    out_stats->abi_major = UHM_ABI_MAJOR;
    out_stats->abi_minor = UHM_ABI_MINOR;
    out_stats->flags = 0u;
    out_stats->arena_size = heap->core.arena_size;
    out_stats->committed_bytes = heap->stats.committed_bytes;
    out_stats->decommitted_bytes = heap->stats.decommitted_bytes;
    out_stats->logical_decommitted_bytes = heap->stats.logical_decommitted_bytes;
    out_stats->physical_released_bytes = heap->stats.physical_released_bytes;
    out_stats->malloc_live_bytes = uhm_stats_malloc_live_load(heap);
    out_stats->mmap_live_bytes = heap->stats.mmap_live_bytes;
    out_stats->control_bytes = heap->core.control_bytes;
    out_stats->collision_free_bytes =
        heap->core.mmap_lo > heap->core.malloc_hi ? heap->core.mmap_lo - heap->core.malloc_hi : 0;
    if (requested_size >= offsetof(uhm_stats, config_flags_enabled) +
                              sizeof(out_stats->config_flags_enabled)) {
        out_stats->config_flags_enabled = heap->state_flags;
    }
    if (requested_size >= offsetof(uhm_stats, lockless_remote_free_enabled) +
                              sizeof(out_stats->lockless_remote_free_enabled)) {
        out_stats->lockless_remote_free_enabled = lockless_remote_enabled;
    }
    if (requested_size >= offsetof(uhm_stats, lockless_remote_malloc_enabled) +
                              sizeof(out_stats->lockless_remote_malloc_enabled)) {
        out_stats->lockless_remote_malloc_enabled = lockless_remote_enabled;
    }
    if (requested_size >= offsetof(uhm_stats, lockless_remote_mode) +
                              sizeof(out_stats->lockless_remote_mode)) {
        out_stats->lockless_remote_mode =
            lockless_remote_enabled
                ? UHM_LOCKLESS_REMOTE_MODE_POINTER_TAGGED_LOCKFREE
                : UHM_LOCKLESS_REMOTE_MODE_NONE;
    }
    if (requested_size >= offsetof(uhm_stats, reserved) +
                              sizeof(out_stats->reserved)) {
        out_stats->reserved = 0u;
    }
    if (requested_size >= offsetof(uhm_stats, decommit_events) +
                              sizeof(out_stats->decommit_events)) {
        out_stats->decommit_events = heap->core.decommit_events;
    }
    if (requested_size >= offsetof(uhm_stats, decommit_bytes) +
                              sizeof(out_stats->decommit_bytes)) {
        out_stats->decommit_bytes = heap->core.decommit_bytes;
    }
    if (requested_size >= offsetof(uhm_stats, recommit_events) +
                              sizeof(out_stats->recommit_events)) {
        out_stats->recommit_events = heap->core.recommit_events;
    }
    if (requested_size >= offsetof(uhm_stats, recommit_bytes) +
                              sizeof(out_stats->recommit_bytes)) {
        out_stats->recommit_bytes = heap->core.recommit_bytes;
    }
    if (requested_size >=
        offsetof(uhm_stats, malloc_free_decommit_deferred_events) +
            sizeof(out_stats->malloc_free_decommit_deferred_events)) {
        out_stats->malloc_free_decommit_deferred_events =
            heap->core.malloc_free_decommit_deferred_events;
    }
    if (requested_size >=
        offsetof(uhm_stats, malloc_free_decommit_deferred_bytes) +
            sizeof(out_stats->malloc_free_decommit_deferred_bytes)) {
        out_stats->malloc_free_decommit_deferred_bytes =
            heap->core.malloc_free_decommit_deferred_bytes;
    }
    if (requested_size >=
        offsetof(uhm_stats, decommit_recommit_same_addr_events) +
            sizeof(out_stats->decommit_recommit_same_addr_events)) {
        out_stats->decommit_recommit_same_addr_events =
            heap->core.decommit_recommit_same_addr_events;
    }
    if (requested_size >=
        offsetof(uhm_stats, decommit_recommit_same_addr_bytes) +
            sizeof(out_stats->decommit_recommit_same_addr_bytes)) {
        out_stats->decommit_recommit_same_addr_bytes =
            heap->core.decommit_recommit_same_addr_bytes;
    }
    if (requested_size >= offsetof(uhm_stats, page_span_calls) +
                              sizeof(out_stats->page_span_calls)) {
        out_stats->page_span_calls =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_span_calls);
    }
    if (requested_size >= offsetof(uhm_stats, page_span_pages) +
                              sizeof(out_stats->page_span_pages)) {
        out_stats->page_span_pages =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_span_pages);
    }
    if (requested_size >= offsetof(uhm_stats, page_mark_calls) +
                              sizeof(out_stats->page_mark_calls)) {
        out_stats->page_mark_calls =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_mark_calls);
    }
    if (requested_size >= offsetof(uhm_stats, page_mark_pages) +
                              sizeof(out_stats->page_mark_pages)) {
        out_stats->page_mark_pages =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_mark_pages);
    }
    if (requested_size >= offsetof(uhm_stats, page_mark_max_pages) +
                              sizeof(out_stats->page_mark_max_pages)) {
        out_stats->page_mark_max_pages =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_mark_max_pages);
    }
    if (requested_size >= offsetof(uhm_stats, page_mark_budget_exceeded) +
                              sizeof(out_stats->page_mark_budget_exceeded)) {
        out_stats->page_mark_budget_exceeded =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_mark_budget_exceeded);
    }
    if (requested_size >= offsetof(uhm_stats, page_mark_leaf_spans) +
                              sizeof(out_stats->page_mark_leaf_spans)) {
        out_stats->page_mark_leaf_spans =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_mark_leaf_spans);
    }
    if (requested_size >= offsetof(uhm_stats, page_mark_max_leaf_spans) +
                              sizeof(out_stats->page_mark_max_leaf_spans)) {
        out_stats->page_mark_max_leaf_spans =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_mark_max_leaf_spans);
    }
    if (requested_size >= offsetof(uhm_stats, page_range_query_calls) +
                              sizeof(out_stats->page_range_query_calls)) {
        out_stats->page_range_query_calls =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_range_query_calls);
    }
    if (requested_size >= offsetof(uhm_stats, page_range_query_pages) +
                              sizeof(out_stats->page_range_query_pages)) {
        out_stats->page_range_query_pages =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_range_query_pages);
    }
    if (requested_size >= offsetof(uhm_stats, page_range_query_max_pages) +
                              sizeof(out_stats->page_range_query_max_pages)) {
        out_stats->page_range_query_max_pages =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_range_query_max_pages);
    }
    if (requested_size >=
        offsetof(uhm_stats, page_range_query_budget_exceeded) +
            sizeof(out_stats->page_range_query_budget_exceeded)) {
        out_stats->page_range_query_budget_exceeded =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_range_query_budget_exceeded);
    }
    if (requested_size >= offsetof(uhm_stats, page_range_query_leaf_spans) +
                              sizeof(out_stats->page_range_query_leaf_spans)) {
        out_stats->page_range_query_leaf_spans =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_range_query_leaf_spans);
    }
    if (requested_size >= offsetof(uhm_stats, page_range_query_max_leaf_spans) +
                              sizeof(out_stats->page_range_query_max_leaf_spans)) {
        out_stats->page_range_query_max_leaf_spans =
            UHM_STATS_DIAG_LOAD_FIELD(heap,
                                      page_range_query_max_leaf_spans);
    }
    if (requested_size >= offsetof(uhm_stats, vm_reserve_calls) +
                              sizeof(out_stats->vm_reserve_calls)) {
        out_stats->vm_reserve_calls = heap->core.vm_reserve_calls;
    }
    if (requested_size >= offsetof(uhm_stats, vm_reserve_failures) +
                              sizeof(out_stats->vm_reserve_failures)) {
        out_stats->vm_reserve_failures = heap->core.vm_reserve_failures;
    }
    if (requested_size >= offsetof(uhm_stats, vm_reserve_ns) +
                              sizeof(out_stats->vm_reserve_ns)) {
        out_stats->vm_reserve_ns = heap->core.vm_reserve_ns;
    }
    if (requested_size >= offsetof(uhm_stats, vm_commit_calls) +
                              sizeof(out_stats->vm_commit_calls)) {
        out_stats->vm_commit_calls = heap->core.vm_commit_calls;
    }
    if (requested_size >= offsetof(uhm_stats, vm_commit_failures) +
                              sizeof(out_stats->vm_commit_failures)) {
        out_stats->vm_commit_failures = heap->core.vm_commit_failures;
    }
    if (requested_size >= offsetof(uhm_stats, vm_commit_ns) +
                              sizeof(out_stats->vm_commit_ns)) {
        out_stats->vm_commit_ns = heap->core.vm_commit_ns;
    }
    if (requested_size >= offsetof(uhm_stats, vm_decommit_calls) +
                              sizeof(out_stats->vm_decommit_calls)) {
        out_stats->vm_decommit_calls = heap->core.vm_decommit_calls;
    }
    if (requested_size >= offsetof(uhm_stats, vm_decommit_failures) +
                              sizeof(out_stats->vm_decommit_failures)) {
        out_stats->vm_decommit_failures = heap->core.vm_decommit_failures;
    }
    if (requested_size >= offsetof(uhm_stats, vm_decommit_ns) +
                              sizeof(out_stats->vm_decommit_ns)) {
        out_stats->vm_decommit_ns = heap->core.vm_decommit_ns;
    }
    if (requested_size >= offsetof(uhm_stats, vm_fixed_remap_calls) +
                              sizeof(out_stats->vm_fixed_remap_calls)) {
        out_stats->vm_fixed_remap_calls = heap->core.vm_fixed_remap_calls;
    }
    if (requested_size >= offsetof(uhm_stats, vm_fixed_remap_failures) +
                              sizeof(out_stats->vm_fixed_remap_failures)) {
        out_stats->vm_fixed_remap_failures =
            heap->core.vm_fixed_remap_failures;
    }
    if (requested_size >= offsetof(uhm_stats, vm_fixed_remap_ns) +
                              sizeof(out_stats->vm_fixed_remap_ns)) {
        out_stats->vm_fixed_remap_ns = heap->core.vm_fixed_remap_ns;
    }
    if (requested_size >= offsetof(uhm_stats, vm_protect_calls) +
                              sizeof(out_stats->vm_protect_calls)) {
        out_stats->vm_protect_calls = heap->core.vm_protect_calls;
    }
    if (requested_size >= offsetof(uhm_stats, vm_protect_failures) +
                              sizeof(out_stats->vm_protect_failures)) {
        out_stats->vm_protect_failures = heap->core.vm_protect_failures;
    }
    if (requested_size >= offsetof(uhm_stats, vm_protect_ns) +
                              sizeof(out_stats->vm_protect_ns)) {
        out_stats->vm_protect_ns = heap->core.vm_protect_ns;
    }
    if (requested_size >= offsetof(uhm_stats, vm_release_calls) +
                              sizeof(out_stats->vm_release_calls)) {
        out_stats->vm_release_calls = heap->core.vm_release_calls;
    }
    if (requested_size >= offsetof(uhm_stats, vm_release_failures) +
                              sizeof(out_stats->vm_release_failures)) {
        out_stats->vm_release_failures = heap->core.vm_release_failures;
    }
    if (requested_size >= offsetof(uhm_stats, vm_release_ns) +
                              sizeof(out_stats->vm_release_ns)) {
        out_stats->vm_release_ns = heap->core.vm_release_ns;
    }
    if (requested_size >= offsetof(uhm_stats, vm_numa_hint_calls) +
                              sizeof(out_stats->vm_numa_hint_calls)) {
        out_stats->vm_numa_hint_calls = heap->core.vm_numa_hint_calls;
    }
    if (requested_size >= offsetof(uhm_stats, vm_numa_hint_failures) +
                              sizeof(out_stats->vm_numa_hint_failures)) {
        out_stats->vm_numa_hint_failures = heap->core.vm_numa_hint_failures;
    }
    if (requested_size >= offsetof(uhm_stats, vm_numa_hint_ns) +
                              sizeof(out_stats->vm_numa_hint_ns)) {
        out_stats->vm_numa_hint_ns = heap->core.vm_numa_hint_ns;
    }
    if (requested_size >= offsetof(uhm_stats, metadata_retired_vma_count) +
                              sizeof(out_stats->metadata_retired_vma_count)) {
        out_stats->metadata_retired_vma_count =
            (uint64_t)heap->core.retired_vma_count;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_retired_free_extent_count) +
            sizeof(out_stats->metadata_retired_free_extent_count)) {
        out_stats->metadata_retired_free_extent_count =
            (uint64_t)heap->core.retired_free_extent_count;
    }
    if (requested_size >= offsetof(uhm_stats, metadata_hot_vma_count) +
                              sizeof(out_stats->metadata_hot_vma_count)) {
        out_stats->metadata_hot_vma_count =
            (uint64_t)heap->core.hot_vma_count;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_hot_free_extent_count) +
            sizeof(out_stats->metadata_hot_free_extent_count)) {
        out_stats->metadata_hot_free_extent_count =
            (uint64_t)heap->core.hot_free_extent_count;
    }
    if (requested_size >= offsetof(uhm_stats, metadata_hot_vma_hits) +
                              sizeof(out_stats->metadata_hot_vma_hits)) {
        out_stats->metadata_hot_vma_hits = heap->core.metadata_hot_vma_hits;
    }
    if (requested_size >= offsetof(uhm_stats, metadata_hot_vma_misses) +
                              sizeof(out_stats->metadata_hot_vma_misses)) {
        out_stats->metadata_hot_vma_misses =
            heap->core.metadata_hot_vma_misses;
    }
    if (requested_size >= offsetof(uhm_stats, metadata_hot_vma_reclaimed) +
                              sizeof(out_stats->metadata_hot_vma_reclaimed)) {
        out_stats->metadata_hot_vma_reclaimed =
            heap->core.metadata_hot_vma_reclaimed;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_hot_free_extent_hits) +
            sizeof(out_stats->metadata_hot_free_extent_hits)) {
        out_stats->metadata_hot_free_extent_hits =
            heap->core.metadata_hot_free_extent_hits;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_hot_free_extent_misses) +
            sizeof(out_stats->metadata_hot_free_extent_misses)) {
        out_stats->metadata_hot_free_extent_misses =
            heap->core.metadata_hot_free_extent_misses;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_hot_free_extent_reclaimed) +
            sizeof(out_stats->metadata_hot_free_extent_reclaimed)) {
        out_stats->metadata_hot_free_extent_reclaimed =
            heap->core.metadata_hot_free_extent_reclaimed;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_quarantine_drain_calls) +
            sizeof(out_stats->metadata_quarantine_drain_calls)) {
        out_stats->metadata_quarantine_drain_calls =
            heap->core.metadata_quarantine_drain_calls;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_quarantine_drain_scanned) +
            sizeof(out_stats->metadata_quarantine_drain_scanned)) {
        out_stats->metadata_quarantine_drain_scanned =
            heap->core.metadata_quarantine_drain_scanned;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_quarantine_drain_reclaimed) +
            sizeof(out_stats->metadata_quarantine_drain_reclaimed)) {
        out_stats->metadata_quarantine_drain_reclaimed =
            heap->core.metadata_quarantine_drain_reclaimed;
    }
    if (requested_size >=
        offsetof(uhm_stats, metadata_quarantine_drain_budget_exhausted) +
            sizeof(out_stats->metadata_quarantine_drain_budget_exhausted)) {
        out_stats->metadata_quarantine_drain_budget_exhausted =
            heap->core.metadata_quarantine_drain_budget_exhausted;
    }
    if (requested_size >= offsetof(uhm_stats, page_lock_range_calls) +
                              sizeof(out_stats->page_lock_range_calls)) {
        out_stats->page_lock_range_calls =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_lock_range_calls);
    }
    if (requested_size >= offsetof(uhm_stats, page_lock_range_shards) +
                              sizeof(out_stats->page_lock_range_shards)) {
        out_stats->page_lock_range_shards =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_lock_range_shards);
    }
    if (requested_size >= offsetof(uhm_stats, page_lock_range_max_shards) +
                              sizeof(out_stats->page_lock_range_max_shards)) {
        out_stats->page_lock_range_max_shards =
            UHM_STATS_DIAG_LOAD_FIELD(heap, page_lock_range_max_shards);
    }
    if (requested_size >=
        offsetof(uhm_stats, page_lock_range_budget_exceeded) +
            sizeof(out_stats->page_lock_range_budget_exceeded)) {
        out_stats->page_lock_range_budget_exceeded =
            UHM_STATS_DIAG_LOAD_FIELD(heap,
                                      page_lock_range_budget_exceeded);
    }
    uhm_unlock(&heap->lock);
    uhm_heap_op_end(heap, op_shard);
    return UHM_OK;
}

#undef UHM_STATS_DIAG_LOAD_FIELD
