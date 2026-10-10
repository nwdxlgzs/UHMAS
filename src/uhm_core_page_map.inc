static int uhm_page_span(uhm_heap *heap, uintptr_t addr, size_t size,
                         size_t *first_out, size_t *count_out) {
    uintptr_t end;
    size_t offset_start;
    size_t offset_end;
    size_t page_size;
    size_t first;
    size_t end_page;

    if (!heap || size == 0 || uhm_range_overflows(addr, size, &end) ||
        addr < heap->core.arena_base || end > heap->core.arena_end) {
        return 0;
    }

    page_size = heap->F.vm.page_size;
    offset_start = (size_t)(addr - heap->core.arena_base);
    offset_end = (size_t)(end - heap->core.arena_base);
    first = offset_start / page_size;
    if (uhm_align_up_size(offset_end, page_size, &offset_end)) {
        return 0;
    }
    end_page = offset_end / page_size;
    if (end_page < first || end_page > heap->core.page_count) {
        return 0;
    }

    *first_out = first;
    *count_out = end_page - first;
    if (*count_out == 0) {
        return 0;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_atomic_add_u64(&heap->core.page_span_calls, UINT64_C(1));
    uhm_diag_atomic_add_u64(&heap->core.page_span_pages,
                            (uint64_t)*count_out);
#endif
    return 1;
}

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_diag_note_page_range_counter(uhm_diag_counter_u64 *calls,
                                             uhm_diag_counter_u64 *pages,
                                             uhm_diag_counter_u64 *max_pages,
                                             uhm_diag_counter_u64 *budget_exceeded,
                                             uhm_diag_counter_u64 *leaf_spans,
                                             uhm_diag_counter_u64 *max_leaf_spans,
                                             size_t count,
                                             size_t leaf_count) {
    if (!calls || !pages || !max_pages || !budget_exceeded || count == 0) {
        return;
    }
    uhm_diag_atomic_add_u64(calls, UINT64_C(1));
    uhm_diag_atomic_add_u64(pages, (uint64_t)count);
    uhm_diag_atomic_max_u64(max_pages, (uint64_t)count);
    if (count > UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
        uhm_diag_atomic_add_u64(budget_exceeded, UINT64_C(1));
    }
    if (leaf_spans) {
        uhm_diag_atomic_add_u64(leaf_spans, (uint64_t)leaf_count);
    }
    if (max_leaf_spans) {
        uhm_diag_atomic_max_u64(max_leaf_spans, (uint64_t)leaf_count);
    }
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static size_t uhm_page_leaf_span_count(size_t first, size_t count) {
    size_t first_leaf;
    size_t last_leaf;

    if (count == 0) {
        return 0;
    }
    first_leaf = first / UHM_PAGE_LEAF_PAGES;
    last_leaf = (first + count - 1u) / UHM_PAGE_LEAF_PAGES;
    return (last_leaf - first_leaf) + 1u;
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static uint32_t uhm_page_mark_phase(uint32_t owner, uint32_t va_state,
                                    uint32_t commit_state) {
    if (va_state == UHM_PAGE_VA_PROTECTING) {
        return UHM_PAGE_MARK_PHASE_PROTECTING;
    }
    if (owner == UHM_PAGE_OWNER_CONTROL) {
        return UHM_PAGE_MARK_PHASE_CONTROL;
    }
    if (owner == UHM_PAGE_OWNER_MALLOC &&
        va_state == UHM_PAGE_VA_MALLOC_LIVE) {
        return commit_state == UHM_PAGE_COMMIT_DECOMMITTED
                   ? UHM_PAGE_MARK_PHASE_MALLOC_DECOMMITTED
                   : UHM_PAGE_MARK_PHASE_MALLOC_LIVE;
    }
    if (owner == UHM_PAGE_OWNER_MMAP && va_state == UHM_PAGE_VA_MMAP_LIVE) {
        return UHM_PAGE_MARK_PHASE_MMAP_LIVE;
    }
    if (owner == UHM_PAGE_OWNER_FREE &&
        va_state == UHM_PAGE_VA_FREE_REUSABLE) {
        return UHM_PAGE_MARK_PHASE_FREE_REUSABLE;
    }
    return UHM_PAGE_MARK_PHASE_UNKNOWN;
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_diag_note_page_mark_phase(uhm_heap *heap, uint32_t phase,
                                          size_t count) {
    if (!heap || count == 0 || phase >= UHM_PAGE_MARK_PHASE_COUNT) {
        return;
    }
    uhm_diag_atomic_add_u64(&heap->core.page_mark_phase_calls[phase],
                            UINT64_C(1));
    uhm_diag_atomic_add_u64(&heap->core.page_mark_phase_pages[phase],
                            (uint64_t)count);
    if (count > UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
        uhm_diag_atomic_add_u64(
            &heap->core.page_mark_phase_budget_exceeded[phase],
            UINT64_C(1));
    }
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_diag_note_page_query_phase(uhm_heap *heap, uint32_t phase,
                                           size_t count) {
    if (!heap || count == 0 || phase >= UHM_PAGE_QUERY_PHASE_COUNT) {
        return;
    }
    uhm_diag_atomic_add_u64(&heap->core.page_range_query_phase_calls[phase],
                            UINT64_C(1));
    uhm_diag_atomic_add_u64(&heap->core.page_range_query_phase_pages[phase],
                            (uint64_t)count);
    if (count > UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
        uhm_diag_atomic_add_u64(
            &heap->core.page_range_query_phase_budget_exceeded[phase],
            UINT64_C(1));
    }
}
#endif

static void uhm_diag_note_page_mark_range(uhm_heap *heap, size_t first,
                                          size_t count, uint32_t owner,
                                          uint32_t va_state,
                                          uint32_t commit_state) {
#if UHMAS_ENABLE_DIAGNOSTICS
    size_t leaf_count;

    if (!heap || count == 0) {
        return;
    }
    leaf_count = uhm_page_leaf_span_count(first, count);
    uhm_diag_note_page_range_counter(
        &heap->core.page_mark_calls, &heap->core.page_mark_pages,
        &heap->core.page_mark_max_pages,
        &heap->core.page_mark_budget_exceeded,
        &heap->core.page_mark_leaf_spans,
        &heap->core.page_mark_max_leaf_spans, count, leaf_count);
    uhm_diag_note_page_mark_phase(
        heap, uhm_page_mark_phase(owner, va_state, commit_state), count);
#else
    (void)heap;
    (void)first;
    (void)count;
    (void)owner;
    (void)va_state;
    (void)commit_state;
#endif
}

static void uhm_diag_note_page_range_query(uhm_heap *heap, size_t count,
                                           size_t first,
                                           uint32_t phase) {
#if UHMAS_ENABLE_DIAGNOSTICS
    size_t leaf_count;

    if (!heap || count == 0) {
        return;
    }
    leaf_count = uhm_page_leaf_span_count(first, count);
    uhm_diag_note_page_range_counter(
        &heap->core.page_range_query_calls,
        &heap->core.page_range_query_pages,
        &heap->core.page_range_query_max_pages,
        &heap->core.page_range_query_budget_exceeded,
        &heap->core.page_range_query_leaf_spans,
        &heap->core.page_range_query_max_leaf_spans, count, leaf_count);
    uhm_diag_note_page_query_phase(heap, phase, count);
#else
    (void)heap;
    (void)count;
    (void)first;
    (void)phase;
#endif
}

static void uhm_diag_note_page_lock_range(uhm_heap *heap,
                                          size_t shard_count) {
#if UHMAS_ENABLE_DIAGNOSTICS
    if (!heap || shard_count == 0) {
        return;
    }
    uhm_diag_atomic_add_u64(&heap->core.page_lock_range_calls, UINT64_C(1));
    uhm_diag_atomic_add_u64(&heap->core.page_lock_range_shards,
                            (uint64_t)shard_count);
    uhm_diag_atomic_max_u64(&heap->core.page_lock_range_max_shards,
                            (uint64_t)shard_count);
    if (shard_count > UHM_PAGE_RANGE_DIAG_SHARD_BUDGET) {
        uhm_diag_atomic_add_u64(
            &heap->core.page_lock_range_budget_exceeded, UINT64_C(1));
    }
#else
    (void)heap;
    (void)shard_count;
#endif
}

static size_t uhm_page_shard_for_index(size_t page_index) {
    size_t base_index = page_index - (page_index % UHM_PAGE_LEAF_PAGES);
    size_t leaf_index = base_index / UHM_PAGE_LEAF_PAGES;
    return leaf_index % UHM_PAGE_MAP_SHARDS;
}

static void uhm_lockset_init(uhm_lockset *lockset) {
    if (lockset) {
        memset(lockset, 0, sizeof(*lockset));
    }
}

static int uhm_lockset_note_rank(uhm_lockset *lockset, uint32_t rank) {
    if (!lockset || rank == UHM_LOCK_RANK_NONE) {
        return 0;
    }
    if (lockset->highest_rank > rank) {
        lockset->violation = 1u;
        return 0;
    }
    lockset->highest_rank = rank;
    return 1;
}

static int uhm_lockset_add_page_shard(uhm_lockset *lockset, size_t shard) {
    if (!lockset || shard >= UHM_PAGE_MAP_SHARDS ||
        lockset->page_shard_count >= UHM_PAGE_MAP_SHARDS ||
        !uhm_lockset_note_rank(lockset, UHM_LOCK_RANK_PAGE_MAP)) {
        if (lockset) {
            lockset->violation = 1u;
        }
        return 0;
    }
    if (lockset->page_shard_count != 0 &&
        lockset->page_shards[lockset->page_shard_count - 1u] >= shard) {
        lockset->violation = 1u;
        return 0;
    }
    lockset->page_shards[lockset->page_shard_count++] = shard;
    return 1;
}

static int uhm_lockset_add_ranked_shard(size_t *shards, size_t *count,
                                        size_t capacity, uint32_t rank,
                                        uhm_lockset *lockset, size_t shard) {
    if (!lockset || !shards || !count || shard >= capacity ||
        *count >= capacity || !uhm_lockset_note_rank(lockset, rank)) {
        if (lockset) {
            lockset->violation = 1u;
        }
        return 0;
    }
    if (*count != 0 && shards[*count - 1u] >= shard) {
        lockset->violation = 1u;
        return 0;
    }
    shards[(*count)++] = shard;
    return 1;
}

static int uhm_lockset_add_vma_shard(uhm_lockset *lockset, size_t shard) {
    return uhm_lockset_add_ranked_shard(
        lockset ? lockset->vma_shards : 0,
        lockset ? &lockset->vma_shard_count : 0, UHM_VMA_SHARDS,
        UHM_LOCK_RANK_VMA, lockset, shard);
}

static int uhm_lockset_add_extent_shard(uhm_lockset *lockset, size_t shard) {
    return uhm_lockset_add_ranked_shard(
        lockset ? lockset->extent_shards : 0,
        lockset ? &lockset->extent_shard_count : 0, UHM_EXTENT_SHARDS,
        UHM_LOCK_RANK_EXTENT, lockset, shard);
}

static int uhm_lockset_add_slab_shard(uhm_lockset *lockset, size_t shard) {
    return uhm_lockset_add_ranked_shard(
        lockset ? lockset->slab_shards : 0,
        lockset ? &lockset->slab_shard_count : 0, UHM_SLAB_SHARDS,
        UHM_LOCK_RANK_SLAB, lockset, shard);
}

static void uhm_vma_lock_all_shards(uhm_heap *heap, uhm_lockset *lockset) {
    size_t i;
    for (i = 0; i < UHM_VMA_SHARDS; ++i) {
        if (!uhm_lockset_add_vma_shard(lockset, i)) {
            break;
        }
        uhm_lock(&heap->core.vma_locks[i].lock);
    }
}

static void uhm_extent_lock_all_shards(uhm_heap *heap, uhm_lockset *lockset) {
    size_t i;
    for (i = 0; i < UHM_EXTENT_SHARDS; ++i) {
        if (!uhm_lockset_add_extent_shard(lockset, i)) {
            break;
        }
        uhm_lock(&heap->core.extent_locks[i].lock);
    }
}

static void uhm_slab_lock_all_shards(uhm_heap *heap, uhm_lockset *lockset) {
    size_t i;
    for (i = 0; i < UHM_SLAB_SHARDS; ++i) {
        if (!uhm_lockset_add_slab_shard(lockset, i)) {
            break;
        }
        uhm_lock(&heap->core.slab_locks[i].lock);
    }
}

static void uhm_slab_unlock_shards(uhm_heap *heap, uhm_lockset *lockset) {
    while (lockset && lockset->slab_shard_count != 0) {
        --lockset->slab_shard_count;
        uhm_unlock(&heap->core.slab_locks[
            lockset->slab_shards[lockset->slab_shard_count]].lock);
    }
}

static void uhm_extent_unlock_shards(uhm_heap *heap, uhm_lockset *lockset) {
    while (lockset && lockset->extent_shard_count != 0) {
        --lockset->extent_shard_count;
        uhm_unlock(&heap->core.extent_locks[
            lockset->extent_shards[lockset->extent_shard_count]].lock);
    }
}

static void uhm_vma_unlock_shards(uhm_heap *heap, uhm_lockset *lockset) {
    while (lockset && lockset->vma_shard_count != 0) {
        --lockset->vma_shard_count;
        uhm_unlock(&heap->core.vma_locks[
            lockset->vma_shards[lockset->vma_shard_count]].lock);
    }
}

static void uhm_metadata_unlock_shards(uhm_heap *heap, uhm_lockset *lockset) {
    uhm_slab_unlock_shards(heap, lockset);
    uhm_extent_unlock_shards(heap, lockset);
    uhm_vma_unlock_shards(heap, lockset);
}

static void uhm_page_lock_shard_range(uhm_heap *heap, size_t first,
                                      size_t count, uhm_lockset *lockset) {
    unsigned char touched[UHM_PAGE_MAP_SHARDS];
    size_t end = first + count;
    size_t page = first;
    size_t i;

    uhm_lockset_init(lockset);
    memset(touched, 0, sizeof(touched));
    while (page < end) {
        size_t shard = uhm_page_shard_for_index(page);
        size_t next_leaf =
            page - (page % UHM_PAGE_LEAF_PAGES) + UHM_PAGE_LEAF_PAGES;
        touched[shard] = 1u;
        page = next_leaf < end ? next_leaf : end;
    }
    for (i = 0; i < UHM_PAGE_MAP_SHARDS; ++i) {
        if (touched[i]) {
            if (!uhm_lockset_add_page_shard(lockset, i)) {
                break;
            }
            uhm_lock(&heap->core.page_map_locks[i].lock);
        }
    }
    uhm_diag_note_page_lock_range(heap, lockset->page_shard_count);
}

static void uhm_page_unlock_shards(uhm_heap *heap, uhm_lockset *lockset) {
    if (!lockset) {
        return;
    }
    while (lockset->page_shard_count != 0) {
        --lockset->page_shard_count;
        uhm_unlock(&heap->core.page_map_locks[
            lockset->page_shards[lockset->page_shard_count]].lock);
    }
    lockset->highest_rank = UHM_LOCK_RANK_NONE;
}

static int uhm_lockset_has_page_shard(const uhm_lockset *lockset,
                                      size_t shard) {
    size_t i;

    if (!lockset || lockset->violation) {
        return 0;
    }
    for (i = 0; i < lockset->page_shard_count; ++i) {
        if (lockset->page_shards[i] == shard) {
            return 1;
        }
    }
    return 0;
}

static int uhm_lockset_covers_page_range(const uhm_lockset *lockset,
                                         size_t first, size_t count) {
    size_t end;
    size_t page;

    if (!lockset || lockset->violation ||
        uhm_add_overflows_size(first, count, &end)) {
        return 0;
    }
    page = first;
    while (page < end) {
        size_t shard = uhm_page_shard_for_index(page);
        size_t next_leaf =
            page - (page % UHM_PAGE_LEAF_PAGES) + UHM_PAGE_LEAF_PAGES;
        if (!uhm_lockset_has_page_shard(lockset, shard)) {
            return 0;
        }
        page = next_leaf < end ? next_leaf : end;
    }
    return 1;
}

static uhm_page_leaf *uhm_page_leaf_spare_take_locked(uhm_heap *heap,
                                                      size_t shard) {
    uhm_page_leaf *leaf = heap->core.page_map_spares[shard];
    if (!leaf) {
        return 0;
    }
    heap->core.page_map_spares[shard] = leaf->next;
    if (heap->core.page_map_spare_count[shard] != 0) {
        --heap->core.page_map_spare_count[shard];
    }
    leaf->next = 0;
    return leaf;
}

static void uhm_page_leaf_spare_put_locked(uhm_heap *heap, size_t shard,
                                           uhm_page_leaf *leaf) {
    if (!leaf) {
        return;
    }
    memset(leaf, 0, sizeof(*leaf));
    leaf->next = heap->core.page_map_spares[shard];
    heap->core.page_map_spares[shard] = leaf;
    ++heap->core.page_map_spare_count[shard];
}

static void uhm_page_leaf_init(uhm_page_leaf *leaf, size_t base_index) {
    memset(leaf, 0, sizeof(*leaf));
    leaf->base_index = base_index;
}

/*
 * Allocation stays outside the shard lock because control-band expansion marks
 * page-map ranges. If lock splitting lets two threads create the same leaf, the
 * racing loser is returned to this shard's spare list instead of being leaked.
 */
static uhm_page_leaf *uhm_page_leaf_get(uhm_heap *heap, size_t page_index, int create) {
    size_t base_index = page_index - (page_index % UHM_PAGE_LEAF_PAGES);
    size_t leaf_index = base_index / UHM_PAGE_LEAF_PAGES;
    size_t shard = leaf_index % UHM_PAGE_MAP_SHARDS;

    for (;;) {
        uhm_page_leaf **cursor = &heap->core.page_map[shard];
        uhm_page_leaf *leaf;

        uhm_lock(&heap->core.page_map_locks[shard].lock);
        while (*cursor && (*cursor)->base_index < base_index) {
            cursor = &(*cursor)->next;
        }
        if (*cursor && (*cursor)->base_index == base_index) {
            leaf = *cursor;
            uhm_unlock(&heap->core.page_map_locks[shard].lock);
            return leaf;
        }
        if (!create) {
            uhm_unlock(&heap->core.page_map_locks[shard].lock);
            return 0;
        }
        leaf = uhm_page_leaf_spare_take_locked(heap, shard);
        if (leaf) {
            uhm_page_leaf_init(leaf, base_index);
            leaf->next = *cursor;
            *cursor = leaf;
            uhm_unlock(&heap->core.page_map_locks[shard].lock);
            return leaf;
        }
        uhm_unlock(&heap->core.page_map_locks[shard].lock);

        leaf = (uhm_page_leaf *)uhm_control_alloc_existing_uninitialized(
            heap, sizeof(*leaf), alignof(uhm_page_leaf));
        if (!leaf) {
            if (!uhm_control_extent_carve(heap, sizeof(*leaf),
                                          alignof(uhm_page_leaf))) {
                return 0;
            }
            continue;
        }
        uhm_page_leaf_init(leaf, base_index);
        uhm_lock(&heap->core.page_map_locks[shard].lock);
        cursor = &heap->core.page_map[shard];
        while (*cursor && (*cursor)->base_index < base_index) {
            cursor = &(*cursor)->next;
        }
        if (*cursor && (*cursor)->base_index == base_index) {
            uhm_page_leaf *existing = *cursor;
            uhm_page_leaf_spare_put_locked(heap, shard, leaf);
            uhm_unlock(&heap->core.page_map_locks[shard].lock);
            return existing;
        }
        leaf->next = *cursor;
        *cursor = leaf;
        uhm_unlock(&heap->core.page_map_locks[shard].lock);
        return leaf;
    }
}

/* Caller must hold the page-map shard lock for page_index. */
static uhm_page_leaf *uhm_page_leaf_get_unlocked(uhm_heap *heap,
                                                 size_t page_index) {
    size_t base_index = page_index - (page_index % UHM_PAGE_LEAF_PAGES);
    size_t shard = uhm_page_shard_for_index(page_index);
    uhm_page_leaf *leaf;

    for (leaf = heap->core.page_map[shard]; leaf; leaf = leaf->next) {
        if (leaf->base_index == base_index) {
            return leaf;
        }
        if (leaf->base_index > base_index) {
            return 0;
        }
    }
    return 0;
}

static uhm_page_record *uhm_page_record_get_unlocked(uhm_heap *heap,
                                                     size_t page_index) {
    uhm_page_leaf *leaf;
    if (page_index >= heap->core.page_count) {
        return 0;
    }
    leaf = uhm_page_leaf_get_unlocked(heap, page_index);
    return leaf ? &leaf->records[page_index - leaf->base_index] : 0;
}

static int uhm_page_leaf_chunk_unlocked(uhm_heap *heap, size_t cursor,
                                        size_t end, uhm_page_leaf **leaf_out,
                                        size_t *offset_out,
                                        size_t *chunk_out) {
    size_t base_index;
    size_t offset;
    size_t leaf_remaining;
    size_t remaining;
    uhm_page_leaf *leaf;

    if (!heap || cursor >= end || cursor >= heap->core.page_count ||
        end > heap->core.page_count) {
        return 0;
    }
    base_index = cursor - (cursor % UHM_PAGE_LEAF_PAGES);
    offset = cursor - base_index;
    leaf_remaining = UHM_PAGE_LEAF_PAGES - offset;
    remaining = end - cursor;
    leaf = uhm_page_leaf_get_unlocked(heap, cursor);
    if (leaf && leaf->base_index != base_index) {
        return 0;
    }
    if (leaf_out) {
        *leaf_out = leaf;
    }
    if (offset_out) {
        *offset_out = offset;
    }
    if (chunk_out) {
        *chunk_out = leaf_remaining < remaining ? leaf_remaining : remaining;
    }
    return 1;
}

static uhm_result uhm_page_prepare_range(uhm_heap *heap, uintptr_t addr, size_t size) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    cursor = first;
    end = first + count;
    while (cursor < end) {
        size_t leaf_end = cursor - (cursor % UHM_PAGE_LEAF_PAGES) +
                          UHM_PAGE_LEAF_PAGES;
        if (!uhm_page_leaf_get(heap, cursor, 1)) {
            return UHM_ERR_OUT_OF_MEMORY;
        }
        cursor = leaf_end < end ? leaf_end : end;
    }
    return UHM_OK;
}

static uhm_result uhm_page_mark_range_locked_prepared(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    uint64_t descriptor_id, uint64_t generation,
    const uhm_lockset *lockset, int leaves_ready);
static uhm_result uhm_page_span_mark_range_locked_prepared(
    uhm_heap *heap, size_t first, size_t count, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    uint64_t descriptor_id, uint64_t generation,
    const uhm_lockset *lockset, int leaves_ready);
static uhm_result uhm_page_mark_range_ready_locked(
    uhm_heap *heap, uintptr_t addr, size_t size,
    const uhm_lockset *lockset);
static uhm_result uhm_page_span_mark_range_ready_locked(
    uhm_heap *heap, size_t first, size_t count,
    const uhm_lockset *lockset);
static uhm_result uhm_page_span_ready_overlaps_va_state_locked(
    uhm_heap *heap, size_t first, size_t count, uint32_t va_state,
    const uhm_lockset *lockset, int *overlaps_out);
static uhm_result uhm_page_mark_range_locked_budgeted(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    uint64_t descriptor_id, uint64_t generation,
    const uhm_lockset *lockset);

static void uhm_page_record_fill(uhm_page_record *records, size_t count,
                                 const uhm_page_record *value) {
    size_t filled;

    if (!records || !value || count == 0) {
        return;
    }
    if (count <= 4u) {
        size_t i;
        for (i = 0; i < count; ++i) {
            records[i] = *value;
        }
        return;
    }

    records[0] = *value;
    filled = 1u;
    while (filled < count) {
        size_t remaining = count - filled;
        size_t copy_count = filled < remaining ? filled : remaining;
        memcpy(records + filled, records, copy_count * sizeof(*records));
        filled += copy_count;
    }
}

static uhm_result uhm_page_mark_range(uhm_heap *heap, uintptr_t addr, size_t size,
                                      uint32_t owner, uint32_t va_state,
                                      uint32_t commit_state, uint32_t prot,
                                      uint64_t descriptor_id, uint64_t generation,
                                      int create) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    uhm_result result;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    cursor = first;
    end = first + count;
    while (cursor < end) {
        size_t leaf_end = cursor - (cursor % UHM_PAGE_LEAF_PAGES) +
                          UHM_PAGE_LEAF_PAGES;
        if (!uhm_page_leaf_get(heap, cursor, create)) {
            return create ? UHM_ERR_OUT_OF_MEMORY : UHM_ERR_CORRUPTION;
        }
        cursor = leaf_end < end ? leaf_end : end;
    }
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    result = uhm_page_span_mark_range_locked_prepared(
        heap, first, count, owner, va_state, commit_state, prot,
        descriptor_id, generation, &lockset, 0);
    uhm_page_unlock_shards(heap, &lockset);
    return result;
}

static uhm_result uhm_page_mark_range_ready_locked(
    uhm_heap *heap, uintptr_t addr, size_t size,
    const uhm_lockset *lockset) {
    size_t first;
    size_t count;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    return uhm_page_span_mark_range_ready_locked(heap, first, count, lockset);
}

static uhm_result uhm_page_span_mark_range_ready_locked(
    uhm_heap *heap, size_t first, size_t count,
    const uhm_lockset *lockset) {
    size_t cursor;
    size_t end;

    if (!heap || count == 0 || first > heap->core.page_count ||
        count > heap->core.page_count - first) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!uhm_lockset_covers_page_range(lockset, first, count)) {
        return UHM_ERR_CORRUPTION;
    }

    cursor = first;
    end = first + count;
    while (cursor < end) {
        size_t leaf_end =
            cursor - (cursor % UHM_PAGE_LEAF_PAGES) + UHM_PAGE_LEAF_PAGES;
        if (!uhm_page_leaf_get_unlocked(heap, cursor)) {
            return UHM_ERR_CORRUPTION;
        }
        cursor = leaf_end < end ? leaf_end : end;
    }
    return UHM_OK;
}

static uhm_result uhm_page_mark_range_locked_prepared(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    uint64_t descriptor_id, uint64_t generation,
    const uhm_lockset *lockset, int leaves_ready) {
    size_t first;
    size_t count;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    return uhm_page_span_mark_range_locked_prepared(
        heap, first, count, owner, va_state, commit_state, prot,
        descriptor_id, generation, lockset, leaves_ready);
}

static uhm_result uhm_page_span_mark_range_locked_prepared(
    uhm_heap *heap, size_t first, size_t count, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    uint64_t descriptor_id, uint64_t generation,
    const uhm_lockset *lockset, int leaves_ready) {
    size_t cursor;
    size_t end;
    uhm_page_record value;

    if (!heap || count == 0 || first > heap->core.page_count ||
        count > heap->core.page_count - first) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!uhm_lockset_covers_page_range(lockset, first, count)) {
        return UHM_ERR_CORRUPTION;
    }
    end = first + count;
    if (!leaves_ready) {
        cursor = first;
        while (cursor < end) {
            size_t leaf_end =
                cursor - (cursor % UHM_PAGE_LEAF_PAGES) +
                UHM_PAGE_LEAF_PAGES;
            if (!uhm_page_leaf_get_unlocked(heap, cursor)) {
                return UHM_ERR_CORRUPTION;
            }
            cursor = leaf_end < end ? leaf_end : end;
        }
    }

    value.owner = owner;
    value.va_state = va_state;
    value.commit_state = commit_state;
    value.prot = prot;
    value.descriptor_id = descriptor_id;
    value.generation = generation;
    value.pin_count = 0;
    value.flags = 0;

    cursor = first;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            return UHM_ERR_CORRUPTION;
        }
        uhm_page_record_fill(&leaf->records[offset], chunk, &value);
        cursor += chunk;
    }
    uhm_diag_note_page_mark_range(heap, first, count,
                                  owner, va_state, commit_state);
    return UHM_OK;
}

static uhm_result uhm_page_mark_range_locked_budgeted(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    uint64_t descriptor_id, uint64_t generation,
    const uhm_lockset *lockset) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (count <= UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
        return uhm_page_span_mark_range_locked_prepared(
            heap, first, count, owner, va_state, commit_state, prot,
            descriptor_id, generation, lockset, 0);
    }
    if (!uhm_lockset_covers_page_range(lockset, first, count)) {
        return UHM_ERR_CORRUPTION;
    }
    cursor = first;
    end = first + count;
    while (cursor < end) {
        size_t chunk_pages = end - cursor;
        uhm_result result;

        if (chunk_pages > UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
            chunk_pages = UHM_PAGE_RANGE_DIAG_PAGE_BUDGET;
        }
        result = uhm_page_span_mark_range_locked_prepared(
            heap, cursor, chunk_pages, owner, va_state, commit_state, prot,
            descriptor_id, generation, lockset, 0);
        if (result != UHM_OK) {
            return result;
        }
        cursor += chunk_pages;
    }
    return UHM_OK;
}

static int uhm_page_range_is_free(uhm_heap *heap, uintptr_t addr, size_t size) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_IS_FREE);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk)) {
            ok = 0;
            break;
        }
        if (leaf) {
            for (i = 0; i < chunk; ++i) {
                if (leaf->records[offset + i].owner != UHM_PAGE_OWNER_FREE) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                break;
            }
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    return ok;
}

static int uhm_page_range_is_free_budgeted(uhm_heap *heap, uintptr_t addr,
                                           size_t size) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    size_t page_size;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    if (count <= UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
        return uhm_page_range_is_free(heap, addr, size);
    }
    page_size = heap->F.vm.page_size;
    cursor = first;
    end = first + count;
    while (cursor < end) {
        size_t chunk_pages = end - cursor;
        uintptr_t chunk_addr;
        size_t chunk_size;

        if (chunk_pages > UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
            chunk_pages = UHM_PAGE_RANGE_DIAG_PAGE_BUDGET;
        }
        chunk_addr = heap->core.arena_base + cursor * page_size;
        chunk_size = chunk_pages * page_size;
        if (!uhm_page_range_is_free(heap, chunk_addr, chunk_size)) {
            return 0;
        }
        cursor += chunk_pages;
    }
    return 1;
}

static int uhm_page_range_has_state(uhm_heap *heap, uintptr_t addr, size_t size,
                                    uint32_t owner, uint32_t va_state,
                                    uint32_t commit_state, uint32_t prot) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_HAS_STATE);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->owner != owner || record->va_state != va_state ||
                record->commit_state != commit_state ||
                record->prot != prot) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    return ok;
}

static int uhm_page_range_matches_descriptor_locked(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    const uhm_extent_descriptor *desc, int exact_prot, int exact_generation,
    const uhm_lockset *lockset);
static int uhm_page_span_matches_descriptor_locked(
    uhm_heap *heap, size_t first, size_t count, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    const uhm_extent_descriptor *desc, int exact_prot, int exact_generation,
    const uhm_lockset *lockset);

static int uhm_page_range_matches_descriptor_live_or_protecting_relaxed(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t live_va_state, uint32_t protecting_va_state,
    uint32_t commit_state, const uhm_extent_descriptor *desc) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!desc || !uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(
        heap, count, first,
        UHM_PAGE_QUERY_PHASE_MATCHES_DESCRIPTOR_PROTECTING);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->owner != owner ||
                record->commit_state != commit_state ||
                record->descriptor_id != desc->id ||
                (record->va_state != live_va_state &&
                 record->va_state != protecting_va_state)) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    return ok;
}

static int uhm_page_range_matches_descriptor_live_or_anon_protecting(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t live_va_state, uint32_t protecting_va_state,
    uint32_t live_commit_state, uint32_t live_prot,
    const uhm_extent_descriptor *desc) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!desc || !uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(
        heap, count, first,
        UHM_PAGE_QUERY_PHASE_MATCHES_DESCRIPTOR_PROTECTING);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->owner != owner) {
                ok = 0;
                break;
            }
            if (record->va_state == live_va_state) {
                if (record->commit_state != live_commit_state ||
                    record->descriptor_id != desc->id ||
                    record->prot != live_prot ||
                    record->generation != desc->generation) {
                    ok = 0;
                    break;
                }
            } else if (record->va_state == protecting_va_state) {
                int matches_old_descriptor =
                    record->commit_state == live_commit_state &&
                    record->descriptor_id == desc->id &&
                    record->prot == live_prot;
                int matches_anon_claim =
                    record->commit_state == UHM_PAGE_COMMIT_NONE &&
                    record->descriptor_id == 0 &&
                    record->prot == UHM_PROT_NONE;
                if (!matches_old_descriptor && !matches_anon_claim) {
                    ok = 0;
                    break;
                }
            } else {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    return ok;
}

static int uhm_page_range_matches_descriptor(uhm_heap *heap, uintptr_t addr,
                                             size_t size, uint32_t owner,
                                             uint32_t va_state,
                                             uint32_t commit_state,
                                             uint32_t prot,
                                             const uhm_extent_descriptor *desc,
                                             int exact_prot,
                                             int exact_generation) {
    size_t first;
    size_t count;
    uhm_lockset lockset;
    int ok;

    if (!desc || !uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    ok = uhm_page_span_matches_descriptor_locked(
        heap, first, count, owner, va_state, commit_state, prot, desc,
        exact_prot, exact_generation, &lockset);
    uhm_page_unlock_shards(heap, &lockset);
    return ok;
}

static int uhm_page_range_matches_descriptor_locked(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    const uhm_extent_descriptor *desc, int exact_prot, int exact_generation,
    const uhm_lockset *lockset) {
    size_t first;
    size_t count;

    if (!desc || !uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    return uhm_page_span_matches_descriptor_locked(
        heap, first, count, owner, va_state, commit_state, prot, desc,
        exact_prot, exact_generation, lockset);
}

static int uhm_page_span_matches_descriptor_locked(
    uhm_heap *heap, size_t first, size_t count, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    const uhm_extent_descriptor *desc, int exact_prot, int exact_generation,
    const uhm_lockset *lockset) {
    size_t cursor;
    size_t end;
    int ok = 1;

    if (!heap || !desc || count == 0 || first > heap->core.page_count ||
        count > heap->core.page_count - first) {
        return 0;
    }
    uhm_diag_note_page_range_query(
        heap, count, first, UHM_PAGE_QUERY_PHASE_MATCHES_DESCRIPTOR);
    if (!uhm_lockset_covers_page_range(lockset, first, count)) {
        return 1;
    }
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->owner != owner || record->va_state != va_state ||
                record->commit_state != commit_state ||
                record->descriptor_id != desc->id ||
                (exact_prot && record->prot != prot) ||
                (exact_generation && record->generation != desc->generation)) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        cursor += chunk;
    }
    return ok;
}

static int uhm_malloc_decommitted_record_descriptor_in_block(
    const uhm_block *block, uint64_t descriptor_id) {
    uintptr_t block_start;
    uintptr_t block_end;
    uintptr_t desc_addr;
    uintptr_t candidate_addr;

    if (!block || descriptor_id == 0 ||
        descriptor_id > (uint64_t)UINTPTR_MAX ||
        descriptor_id < (uint64_t)offsetof(uhm_block, desc)) {
        return 0;
    }
    block_start = (uintptr_t)block;
    if (uhm_range_overflows(block_start, block->total_size, &block_end)) {
        return 0;
    }
    desc_addr = (uintptr_t)descriptor_id;
    if (desc_addr < block_start || desc_addr >= block_end) {
        return 0;
    }
    candidate_addr = desc_addr - (uintptr_t)offsetof(uhm_block, desc);
    return candidate_addr >= block_start &&
           candidate_addr + sizeof(uhm_block) <= block_end &&
           (candidate_addr % alignof(uhm_block)) == 0 &&
           desc_addr == candidate_addr + offsetof(uhm_block, desc);
}

static int uhm_page_range_overlaps_owner(uhm_heap *heap, uintptr_t addr, size_t size,
                                         uint32_t owner) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int found = 0;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_OVERLAPS_OWNER);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk)) {
            break;
        }
        if (leaf) {
            for (i = 0; i < chunk; ++i) {
                if (leaf->records[offset + i].owner == owner) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    return found;
}

static int uhm_page_range_has_live_mmap(uhm_heap *heap, uintptr_t addr, size_t size) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_HAS_LIVE_MMAP);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->owner != UHM_PAGE_OWNER_MMAP ||
                record->va_state != UHM_PAGE_VA_MMAP_LIVE) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    return ok;
}

static uhm_result uhm_page_range_require_live_mmap_no_protecting(
    uhm_heap *heap, uintptr_t addr, size_t size) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int has_not_mapped = 0;
    uhm_result result = UHM_OK;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return UHM_ERR_NOT_MAPPED;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_HAS_LIVE_MMAP);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk)) {
            has_not_mapped = 1;
            break;
        }
        if (!leaf) {
            has_not_mapped = 1;
            cursor += chunk;
            continue;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->va_state == UHM_PAGE_VA_PROTECTING) {
                result = UHM_ERR_PROTECTION;
                break;
            }
            if (record->owner != UHM_PAGE_OWNER_MMAP ||
                record->va_state != UHM_PAGE_VA_MMAP_LIVE) {
                has_not_mapped = 1;
            }
        }
        if (result != UHM_OK) {
            break;
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    if (result != UHM_OK) {
        return result;
    }
    return has_not_mapped ? UHM_ERR_NOT_MAPPED : UHM_OK;
}

static int uhm_page_span_overlaps_va_state_locked(
    uhm_heap *heap, size_t first, size_t count, uint32_t va_state,
    const uhm_lockset *lockset) {
    size_t cursor;
    size_t end;
    int found = 0;

    if (!heap || count == 0 || first > heap->core.page_count ||
        count > heap->core.page_count - first) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_OVERLAPS_VA_STATE);
    if (!uhm_lockset_covers_page_range(lockset, first, count)) {
        return 0;
    }
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk)) {
            break;
        }
        if (leaf) {
            for (i = 0; i < chunk; ++i) {
                if (leaf->records[offset + i].va_state == va_state) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        cursor += chunk;
    }
    return found;
}

static int uhm_page_range_overlaps_va_state_locked(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t va_state,
    const uhm_lockset *lockset) {
    size_t first;
    size_t count;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    return uhm_page_span_overlaps_va_state_locked(
        heap, first, count, va_state, lockset);
}

static uhm_result uhm_page_span_ready_overlaps_va_state_locked(
    uhm_heap *heap, size_t first, size_t count, uint32_t va_state,
    const uhm_lockset *lockset, int *overlaps_out) {
    size_t cursor;
    size_t end;
    int found = 0;

    if (overlaps_out) {
        *overlaps_out = 0;
    }
    if (!heap || count == 0 || first > heap->core.page_count ||
        count > heap->core.page_count - first) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_OVERLAPS_VA_STATE);
    if (!uhm_lockset_covers_page_range(lockset, first, count)) {
        return UHM_ERR_CORRUPTION;
    }
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            return UHM_ERR_CORRUPTION;
        }
        for (i = 0; i < chunk; ++i) {
            if (leaf->records[offset + i].va_state == va_state) {
                found = 1;
                break;
            }
        }
        if (found) {
            if (overlaps_out) {
                *overlaps_out = 1;
            }
            return UHM_OK;
        }
        cursor += chunk;
    }
    return UHM_OK;
}

static int uhm_page_range_overlaps_va_state(uhm_heap *heap, uintptr_t addr,
                                            size_t size, uint32_t va_state) {
    size_t first;
    size_t count;
    uhm_lockset lockset;
    int found;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    found = uhm_page_span_overlaps_va_state_locked(
        heap, first, count, va_state, &lockset);
    uhm_page_unlock_shards(heap, &lockset);
    return found;
}

static uint64_t uhm_generation_fingerprint_mix(uint64_t acc, uint64_t generation) {
    acc ^= generation + UINT64_C(0x9e3779b97f4a7c15) + (acc << 6) + (acc >> 2);
    return acc;
}

static int uhm_page_range_get_expected_state(uhm_heap *heap, uintptr_t addr,
                                             size_t size, uint32_t owner,
                                             uint32_t va_state,
                                             uint32_t *commit_state_out,
                                             uint32_t *prot_out,
                                             uint64_t *descriptor_id_out,
                                             uint64_t *generation_out) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uint32_t commit_state = UHM_PAGE_COMMIT_NONE;
    uint32_t prot = UHM_PROT_NONE;
    uint64_t descriptor_id = 0;
    uint64_t generation = UINT64_C(0xcbf29ce484222325);
    uhm_lockset lockset;
    int ok = 1;
    int first_record = 1;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->owner != owner || record->va_state != va_state) {
                ok = 0;
                break;
            }
            if (first_record) {
                commit_state = record->commit_state;
                prot = record->prot;
                descriptor_id = record->descriptor_id;
                first_record = 0;
            } else if (record->commit_state != commit_state ||
                       record->prot != prot ||
                       record->descriptor_id != descriptor_id) {
                ok = 0;
                break;
            }
            generation =
                uhm_generation_fingerprint_mix(generation,
                                               record->generation);
        }
        if (!ok) {
            break;
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    if (!ok) {
        return 0;
    }
    if (commit_state_out) {
        *commit_state_out = commit_state;
    }
    if (prot_out) {
        *prot_out = prot;
    }
    if (descriptor_id_out) {
        *descriptor_id_out = descriptor_id;
    }
    if (generation_out) {
        *generation_out = generation;
    }
    return 1;
}

static int uhm_page_range_matches_state(uhm_heap *heap, uintptr_t addr,
                                        size_t size, uint32_t owner,
                                        uint32_t va_state,
                                        uint32_t commit_state,
                                        uint32_t prot,
                                        uint64_t descriptor_id,
                                        uint64_t generation) {
    uint32_t actual_commit_state;
    uint32_t actual_prot;
    uint64_t actual_descriptor_id;
    uint64_t actual_generation;

    if (!uhm_page_range_get_expected_state(heap, addr, size, owner, va_state,
                                           &actual_commit_state, &actual_prot,
                                           &actual_descriptor_id,
                                           &actual_generation)) {
        return 0;
    }
    return actual_commit_state == commit_state &&
           actual_prot == prot &&
           actual_descriptor_id == descriptor_id &&
           actual_generation == generation;
}

static int uhm_page_range_matches_state_generation_id(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t owner,
    uint32_t va_state, uint32_t commit_state, uint32_t prot,
    uint64_t descriptor_id, uint64_t generation) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_MATCHES_GENERATION);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->owner != owner || record->va_state != va_state ||
                record->commit_state != commit_state || record->prot != prot ||
                record->descriptor_id != descriptor_id ||
                record->generation != generation) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    return ok;
}

static int uhm_page_range_matches_owner_va_generation(uhm_heap *heap,
                                                      uintptr_t addr,
                                                      size_t size,
                                                      uint32_t owner,
                                                      uint32_t va_state,
                                                      uint64_t generation) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_MATCHES_GENERATION);
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    cursor = first;
    end = first + count;
    while (cursor < end) {
        uhm_page_leaf *leaf;
        size_t offset;
        size_t chunk;
        size_t i;

        if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf, &offset,
                                          &chunk) ||
            !leaf) {
            ok = 0;
            break;
        }
        for (i = 0; i < chunk; ++i) {
            const uhm_page_record *record = &leaf->records[offset + i];
            if (record->owner != owner || record->va_state != va_state ||
                record->generation != generation) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
        cursor += chunk;
    }
    uhm_page_unlock_shards(heap, &lockset);
    return ok;
}

static size_t uhm_malloc_header_size(void) {
    size_t out = 0;
    (void)uhm_align_up_size(sizeof(uhm_block), UHM_MIN_ALIGNMENT, &out);
    return out;
}

static void *uhm_block_payload(uhm_block *block) {
    return (void *)((uintptr_t)block + uhm_malloc_header_size());
}

static size_t uhm_slab_slot_header_size(void) {
    size_t out = 0;
    (void)uhm_align_up_size(sizeof(uhm_slab_slot), UHM_MIN_ALIGNMENT, &out);
    return out;
}

static void *uhm_slab_slot_payload(uhm_slab_slot *slot) {
    return (void *)((uintptr_t)slot + uhm_slab_slot_header_size());
}

static int uhm_size_class_uses_slab(uint16_t size_class, size_t payload_size) {
    return size_class != UHM_SIZE_CLASS_NONE &&
           size_class < UHM_SIZE_CLASS_COUNT &&
           payload_size <= UHM_SLAB_MAX_SLOT_SIZE;
}

