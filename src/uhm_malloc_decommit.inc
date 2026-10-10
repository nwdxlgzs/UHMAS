static int uhm_block_decommit_range(uhm_heap *heap, const uhm_block *block,
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
    if (uhm_range_overflows(block_start, block->total_size, &block_end) ||
        uhm_range_overflows(block_start, uhm_malloc_header_size(), &header_end) ||
        header_end > block_end) {
        return 0;
    }
    page_size = heap->F.vm.page_size;
    if (uhm_align_up_uintptr_checked(header_end, page_size,
                                     &decommit_start)) {
        return 0;
    }
    decommit_end = uhm_align_down_uintptr(block_end, page_size);
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

static size_t uhm_block_retention_threshold_bytes(uhm_heap *heap) {
    if (!heap) {
        return 0;
    }
    return heap->F.vm.page_size *
           heap->core.malloc_free_decommit_retention_pages;
}

static size_t uhm_block_retention_base_high_water_bytes(uhm_heap *heap) {
    size_t threshold;

    if (!heap) {
        return 0;
    }
    threshold = uhm_block_retention_threshold_bytes(heap);
    if (threshold > SIZE_MAX /
                        UHM_MALLOC_FREE_DECOMMIT_RETENTION_HIGH_WATER_MULTIPLIER) {
        return SIZE_MAX;
    }
    return threshold *
           UHM_MALLOC_FREE_DECOMMIT_RETENTION_HIGH_WATER_MULTIPLIER;
}

static size_t uhm_block_retention_max_high_water_bytes(uhm_heap *heap) {
    size_t threshold;
    size_t adaptive_max;
    size_t arena_cap;

    if (!heap) {
        return 0;
    }
    threshold = uhm_block_retention_threshold_bytes(heap);
    if (threshold >
        SIZE_MAX /
            UHM_MALLOC_FREE_DECOMMIT_RETENTION_ADAPTIVE_MAX_MULTIPLIER) {
        adaptive_max = SIZE_MAX;
    } else {
        adaptive_max =
            threshold *
            UHM_MALLOC_FREE_DECOMMIT_RETENTION_ADAPTIVE_MAX_MULTIPLIER;
    }
    arena_cap = heap->core.arena_size / 4u;
    arena_cap = uhm_align_down_uintptr((uintptr_t)arena_cap,
                                       heap->F.vm.page_size);
    if (arena_cap == 0) {
        arena_cap = heap->core
                        .malloc_free_decommit_retention_base_high_water_bytes;
    }
    adaptive_max = uhm_min_size(adaptive_max, arena_cap);
    return uhm_max_size(
        heap->core.malloc_free_decommit_retention_base_high_water_bytes,
        adaptive_max);
}

static void uhm_block_retention_adaptive_grow(uhm_heap *heap,
                                              size_t target_high_water) {
    size_t old_high_water;
    size_t new_high_water;
    size_t max_high_water;

    if (!heap || target_high_water == 0) {
        return;
    }
    old_high_water =
        heap->core.malloc_free_decommit_retention_high_water_bytes;
    max_high_water =
        heap->core.malloc_free_decommit_retention_max_high_water_bytes;
    if (old_high_water >= target_high_water ||
        old_high_water >= max_high_water) {
        return;
    }
    if (old_high_water > max_high_water / 2u) {
        new_high_water = max_high_water;
    } else {
        new_high_water = old_high_water * 2u;
    }
    if (new_high_water < target_high_water) {
        new_high_water = target_high_water;
    }
    if (new_high_water > max_high_water) {
        new_high_water = max_high_water;
    }
    if (new_high_water > old_high_water) {
        heap->core.malloc_free_decommit_retention_high_water_bytes =
            new_high_water;
        heap->core.malloc_free_decommit_retention_idle_drains = 0u;
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                malloc_free_decommit_retention_adaptive_grow_events));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                malloc_free_decommit_retention_adaptive_grow_bytes),
            new_high_water - old_high_water);
    }
}

static void uhm_block_retention_adaptive_shrink(uhm_heap *heap) {
    size_t base_high_water;
    size_t old_high_water;
    size_t new_high_water;

    if (!heap) {
        return;
    }
    base_high_water =
        heap->core.malloc_free_decommit_retention_base_high_water_bytes;
    old_high_water =
        heap->core.malloc_free_decommit_retention_high_water_bytes;
    if (old_high_water <= base_high_water) {
        heap->core.malloc_free_decommit_retention_idle_drains = 0u;
        return;
    }
    if (heap->core.malloc_free_decommit_retained_bytes != 0u) {
        return;
    }
    if (heap->core.malloc_free_decommit_retention_idle_drains <
        UHM_MALLOC_FREE_DECOMMIT_RETENTION_SHRINK_IDLE_DRAINS) {
        ++heap->core.malloc_free_decommit_retention_idle_drains;
    }
    if (heap->core.malloc_free_decommit_retention_idle_drains <
        UHM_MALLOC_FREE_DECOMMIT_RETENTION_SHRINK_IDLE_DRAINS) {
        return;
    }
    heap->core.malloc_free_decommit_retention_idle_drains = 0u;
    new_high_water = old_high_water / 2u;
    if (new_high_water < base_high_water) {
        new_high_water = base_high_water;
    }
    if (new_high_water >= old_high_water) {
        return;
    }
    heap->core.malloc_free_decommit_retention_high_water_bytes =
        new_high_water;
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(
            heap,
            malloc_free_decommit_retention_adaptive_shrink_events));
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(
            heap,
            malloc_free_decommit_retention_adaptive_shrink_bytes),
        old_high_water - new_high_water);
}

static void uhm_block_retained_bytes_add(uhm_heap *heap, size_t size) {
    if (!heap || size == 0) {
        return;
    }
    if (heap->core.malloc_free_decommit_retained_bytes > SIZE_MAX - size) {
        heap->core.malloc_free_decommit_retained_bytes = SIZE_MAX;
    } else {
        heap->core.malloc_free_decommit_retained_bytes += size;
    }
}

static void uhm_block_retained_bytes_sub(uhm_heap *heap, size_t size) {
    if (!heap || size == 0) {
        return;
    }
    if (heap->core.malloc_free_decommit_retained_bytes >= size) {
        heap->core.malloc_free_decommit_retained_bytes -= size;
    } else {
        heap->core.malloc_free_decommit_retained_bytes = 0;
    }
    uhm_block_retention_adaptive_shrink(heap);
}

static int uhm_block_can_defer_decommit_free_extent(uhm_heap *heap,
                                                    const uhm_block *block,
                                                    size_t decommit_size,
                                                    int *hot_out) {
    size_t pages;
    size_t threshold;
    size_t high_water;
    size_t max_high_water;
    size_t retained_bytes;
    size_t target_retained_bytes;
    int hot_candidate = 0;

    if (hot_out) {
        *hot_out = 0;
    }
    if (!heap || decommit_size == 0) {
        return 0;
    }
    pages = heap->core.malloc_free_decommit_retention_pages;
    threshold = heap->F.vm.page_size * pages;
    high_water = heap->core.malloc_free_decommit_retention_high_water_bytes;
    max_high_water =
        heap->core.malloc_free_decommit_retention_max_high_water_bytes;
    if (decommit_size > threshold) {
        if (!block ||
            (block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) == 0 ||
            high_water == 0 || decommit_size > max_high_water) {
            return 0;
        }
        hot_candidate = 1;
    }
    retained_bytes = heap->core.malloc_free_decommit_retained_bytes;
    if (retained_bytes > SIZE_MAX - decommit_size) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                malloc_free_decommit_retention_pressure_events));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                malloc_free_decommit_retention_pressure_bytes),
            decommit_size);
        return 0;
    }
    target_retained_bytes = retained_bytes + decommit_size;
    if (target_retained_bytes > high_water && hot_candidate &&
        target_retained_bytes <= max_high_water) {
        uhm_block_retention_adaptive_grow(heap, target_retained_bytes);
        high_water = heap->core.malloc_free_decommit_retention_high_water_bytes;
    }
    if (high_water != 0 && target_retained_bytes > high_water) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                malloc_free_decommit_retention_pressure_events));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                malloc_free_decommit_retention_pressure_bytes),
            decommit_size);
        return 0;
    }
    if (hot_out) {
        *hot_out = hot_candidate;
    }
    return 1;
}

static void uhm_block_note_decommit_deferred(uhm_heap *heap, uhm_block *block,
                                             size_t size, int hot) {
    if (!heap || !block || size == 0) {
        return;
    }
    if ((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) == 0) {
        block->flags |= UHM_BLOCK_FLAG_DECOMMIT_RETAINED;
        uhm_block_retained_bytes_add(heap, size);
    }
    heap->core.malloc_free_decommit_deferred_events++;
    heap->core.malloc_free_decommit_deferred_bytes += size;
    if (hot) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                malloc_free_decommit_hot_deferred_events));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                malloc_free_decommit_hot_deferred_bytes),
            size);
    }
}

static void uhm_block_release_retained_free_extent(uhm_heap *heap,
                                                   uhm_block *block) {
    size_t size;

    if (!heap || !block ||
        (block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) == 0) {
        return;
    }
    if (uhm_block_decommit_range(heap, block, 0, &size)) {
        uhm_block_retained_bytes_sub(heap, size);
    } else {
        heap->core.malloc_free_decommit_retained_bytes = 0;
    }
    block->flags &= ~UHM_BLOCK_FLAG_DECOMMIT_RETAINED;
}

typedef struct uhm_block_decommit_segment {
    uintptr_t addr;
    size_t size;
} uhm_block_decommit_segment;

static uhm_result uhm_block_collect_decommit_live_segments(
    uhm_heap *heap, uhm_block *block, uintptr_t addr, size_t size,
    uhm_block_decommit_segment *segments, size_t segment_capacity,
    size_t *segment_count_out, size_t *live_bytes_out, int *overflow_out) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    size_t page_size;
    size_t segment_count = 0;
    size_t live_bytes = 0;
    uintptr_t current_addr = 0;
    size_t current_size = 0;
    uhm_result result = UHM_OK;

    if (segment_count_out) {
        *segment_count_out = 0;
    }
    if (live_bytes_out) {
        *live_bytes_out = 0;
    }
    if (overflow_out) {
        *overflow_out = 0;
    }
    if (!heap || !block || !segments || segment_capacity == 0 || size == 0 ||
        !uhm_page_span(heap, addr, size, &first, &count)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    page_size = heap->F.vm.page_size;
    cursor = first;
    end = first + count;
    while (cursor < end && result == UHM_OK) {
        size_t scan_count = end - cursor;
        size_t scan_end;
        uhm_lockset lockset;

        if (scan_count > UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
            scan_count = UHM_PAGE_RANGE_DIAG_PAGE_BUDGET;
        }
        scan_end = cursor + scan_count;
        uhm_page_lock_shard_range(heap, cursor, scan_count, &lockset);
        while (cursor < scan_end && result == UHM_OK) {
            uhm_page_leaf *leaf;
            size_t offset;
            size_t chunk;
            size_t i;
            size_t repaired_pages = 0;

            if (!uhm_page_leaf_chunk_unlocked(heap, cursor, scan_end, &leaf,
                                              &offset, &chunk) ||
                !leaf) {
                result = UHM_ERR_CORRUPTION;
                break;
            }
            for (i = 0; i < chunk; ++i) {
                uhm_page_record *record = &leaf->records[offset + i];
                uintptr_t page_addr =
                    heap->core.arena_base + (cursor + i) * page_size;

                if (record->owner != UHM_PAGE_OWNER_MALLOC ||
                    record->va_state != UHM_PAGE_VA_MALLOC_LIVE) {
                    result = UHM_ERR_CORRUPTION;
                    break;
                }
                if (record->commit_state == UHM_PAGE_COMMIT_LIVE &&
                    record->prot == (UHM_PROT_READ | UHM_PROT_WRITE)) {
                    if (current_size == 0) {
                        current_addr = page_addr;
                    }
                    if (current_size > SIZE_MAX - page_size ||
                        live_bytes > SIZE_MAX - page_size) {
                        result = UHM_ERR_CORRUPTION;
                        break;
                    }
                    current_size += page_size;
                    live_bytes += page_size;
                } else if (record->commit_state ==
                               UHM_PAGE_COMMIT_DECOMMITTED &&
                           record->prot == UHM_PROT_NONE) {
                    /* A split remainder inherits the source block's page
                       descriptor. Coalesced descriptors already inside the
                       merged block remain valid and are left untouched. */
                    if (record->descriptor_id != block->desc.id &&
                        !uhm_malloc_decommitted_record_descriptor_in_block(
                            block, record->descriptor_id)) {
                        record->descriptor_id = block->desc.id;
                        record->generation = block->generation;
                        ++repaired_pages;
                    }
                    if (current_size != 0) {
                        if (segment_count >= segment_capacity) {
                            if (overflow_out) {
                                *overflow_out = 1;
                            }
                            uhm_page_unlock_shards(heap, &lockset);
                            result = UHM_OK;
                            goto done;
                        }
                        segments[segment_count].addr = current_addr;
                        segments[segment_count].size = current_size;
                        ++segment_count;
                        current_size = 0;
                    }
                } else {
                    result = UHM_ERR_CORRUPTION;
                    break;
                }
            }
            if (repaired_pages != 0) {
                uhm_diag_note_page_mark_range(
                    heap, cursor, repaired_pages, UHM_PAGE_OWNER_MALLOC,
                    UHM_PAGE_VA_MALLOC_LIVE,
                    UHM_PAGE_COMMIT_DECOMMITTED);
            }
            cursor += chunk;
        }
        uhm_page_unlock_shards(heap, &lockset);
    }

done:
    if (result == UHM_OK && current_size != 0) {
        if (segment_count >= segment_capacity) {
            if (overflow_out) {
                *overflow_out = 1;
            }
        } else {
            segments[segment_count].addr = current_addr;
            segments[segment_count].size = current_size;
            ++segment_count;
        }
    }
    if (result != UHM_OK) {
        return result;
    }
    if (segment_count_out) {
        *segment_count_out = segment_count;
    }
    if (live_bytes_out) {
        *live_bytes_out = live_bytes;
    }
    return UHM_OK;
}

static uhm_result uhm_block_decommit_free_extent(uhm_heap *heap,
                                                 uhm_block *block,
                                                 int allow_platform_unlock) {
    uintptr_t addr;
    size_t size;
    uhm_block_decommit_segment segments[UHM_BLOCK_DECOMMIT_SEGMENT_MAX];
    size_t segment_count = 0;
    size_t live_bytes = 0;
    int segment_overflow = 0;
    size_t decommitted_segments = 0;
    size_t marked_segments = 0;
    int isolated_for_platform = 0;
    uhm_result result;
    size_t i;

    (void)allow_platform_unlock;
    if (!block || block->magic != UHM_BLOCK_MAGIC ||
        (block->flags & UHM_BLOCK_FLAG_FREE) == 0) {
        return UHM_ERR_CORRUPTION;
    }
    if ((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0) {
        return UHM_OK;
    }
    if (!uhm_block_decommit_range(heap, block, &addr, &size)) {
        return UHM_OK;
    }

    if ((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0) {
        /* A complete commit was published for this block.  Treat its
           decommittable range as one live segment and avoid a page-map walk;
           the hint is cleared before the platform transaction begins. */
        segments[0].addr = addr;
        segments[0].size = size;
        segment_count = 1;
        live_bytes = size;
        segment_overflow = 0;
#if UHMAS_ENABLE_DIAGNOSTICS
        ++heap->core.malloc_free_decommit_commit_all_fast_paths;
#endif
        result = UHM_OK;
    } else {
        result = uhm_block_collect_decommit_live_segments(
            heap, block, addr, size, segments, UHM_BLOCK_DECOMMIT_SEGMENT_MAX,
            &segment_count, &live_bytes, &segment_overflow);
    }
    if (result != UHM_OK) {
        return result;
    }
    if (segment_overflow) {
        segments[0].addr = addr;
        segments[0].size = size;
        segment_count = 1;
        live_bytes = size;
    }

    {
        int hot_defer = 0;
        if (live_bytes == size && !segment_overflow &&
            uhm_block_can_defer_decommit_free_extent(heap, block, size,
                                                     &hot_defer)) {
            uhm_block_note_decommit_deferred(heap, block, size, hot_defer);
            return UHM_OK;
        }
    }
    uhm_block_release_retained_free_extent(heap, block);

    if (live_bytes != 0) {
        uhm_vm_call_record decommit_records[UHM_BLOCK_DECOMMIT_SEGMENT_MAX];
        size_t record_count = 0;
        size_t original_total_size = block->total_size;
        uint64_t original_descriptor_id = block->desc.id;

        memset(decommit_records, 0, sizeof(decommit_records));
        block->flags &= ~UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;
        block->flags |= UHM_BLOCK_FLAG_VM_BUSY;
        uhm_malloc_free_tree_remove(heap, block);
        isolated_for_platform = 1;
        uhm_unlock(&heap->lock);
        if (segment_count > 1u) {
            decommit_records[0] =
                uhm_vm_decommit_unlocked_call(heap, (void *)addr, size);
            record_count = 1u;
        } else {
            for (i = 0; i < segment_count; ++i) {
                decommit_records[i] = uhm_vm_decommit_unlocked_call(
                    heap, (void *)segments[i].addr, segments[i].size);
                ++record_count;
                if (decommit_records[i].result != UHM_OK) {
                    break;
                }
            }
        }
        uhm_lock(&heap->lock);
        result = UHM_OK;
        for (i = 0; i < record_count; ++i) {
            uhm_result publish_result =
                uhm_vm_decommit_publish_record_locked(heap,
                                                      &decommit_records[i]);
            if (publish_result != UHM_OK && result == UHM_OK) {
                result = publish_result;
            }
            if (publish_result == UHM_OK &&
                decommit_records[i].result == UHM_OK) {
                decommitted_segments +=
                    (segment_count > 1u) ? segment_count : 1u;
            } else {
                break;
            }
        }
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             block->magic != UHM_BLOCK_MAGIC ||
             (block->flags & UHM_BLOCK_FLAG_FREE) == 0 ||
             (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 ||
             block->total_size != original_total_size ||
             block->desc.id != original_descriptor_id ||
             !uhm_block_decommit_range(heap, block, &addr, &size))) {
            result = UHM_ERR_CORRUPTION;
        }
        if (result != UHM_OK) {
            while (decommitted_segments > 0) {
                --decommitted_segments;
                uhm_vm_commit_rollback_unlocked_preserve_error(
                    heap, (void *)segments[decommitted_segments].addr,
                    segments[decommitted_segments].size,
                    UHM_PROT_READ | UHM_PROT_WRITE);
            }
            if (isolated_for_platform &&
                block->magic == UHM_BLOCK_MAGIC &&
                (block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                (block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0) {
                block->flags &= ~UHM_BLOCK_FLAG_VM_BUSY;
                uhm_malloc_free_tree_insert(heap, block);
            }
            return result;
        }
    } else {
        result = UHM_OK;
    }
    if (result != UHM_OK) {
        return result;
    }

    {
        uhm_lockset descriptor_lockset;
        uhm_lockset_init(&descriptor_lockset);
        uhm_extent_lock_all_shards(heap, &descriptor_lockset);
        block->flags |= UHM_BLOCK_FLAG_DECOMMITTED;
        block->flags &= ~UHM_BLOCK_FLAG_VM_BUSY;
        block->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(block);
        uhm_metadata_unlock_shards(heap, &descriptor_lockset);
        result = UHM_OK;
        for (i = 0; i < segment_count; ++i) {
            size_t segment_first;
            size_t segment_count_pages;
            uhm_lockset publish_lockset;

            if (!uhm_page_span(heap, segments[i].addr, segments[i].size,
                               &segment_first, &segment_count_pages)) {
                result = UHM_ERR_CORRUPTION;
                break;
            }
            uhm_page_lock_shard_range(heap, segment_first, segment_count_pages,
                                      &publish_lockset);
            uhm_extent_lock_all_shards(heap, &publish_lockset);
            result = uhm_page_mark_range_locked_budgeted(
                heap, segments[i].addr, segments[i].size,
                UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_MALLOC_LIVE,
                UHM_PAGE_COMMIT_DECOMMITTED, UHM_PROT_NONE, block->desc.id,
                block->generation, &publish_lockset);
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            if (result != UHM_OK) {
                break;
            }
            ++marked_segments;
        }
        if (result != UHM_OK) {
            uhm_lockset rollback_descriptor_lockset;
            uhm_lockset_init(&rollback_descriptor_lockset);
            uhm_extent_lock_all_shards(heap, &rollback_descriptor_lockset);
            block->flags &= ~UHM_BLOCK_FLAG_DECOMMITTED;
            if (isolated_for_platform) {
                block->flags |= UHM_BLOCK_FLAG_VM_BUSY;
            }
            block->generation = uhm_heap_generation_next(heap);
            uhm_block_descriptor_set(block);
            uhm_metadata_unlock_shards(heap, &rollback_descriptor_lockset);
            while (marked_segments > 0) {
                size_t segment_first;
                size_t segment_count_pages;
                uhm_lockset rollback_lockset;

                --marked_segments;
                if (!uhm_page_span(heap, segments[marked_segments].addr,
                                   segments[marked_segments].size,
                                   &segment_first, &segment_count_pages)) {
                    continue;
                }
                uhm_page_lock_shard_range(heap, segment_first,
                                          segment_count_pages,
                                          &rollback_lockset);
                uhm_extent_lock_all_shards(heap, &rollback_lockset);
                (void)uhm_page_mark_range_locked_budgeted(
                    heap, segments[marked_segments].addr,
                    segments[marked_segments].size, UHM_PAGE_OWNER_MALLOC,
                    UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
                    UHM_PROT_READ | UHM_PROT_WRITE, block->desc.id,
                    block->generation, &rollback_lockset);
                uhm_metadata_unlock_shards(heap, &rollback_lockset);
                uhm_page_unlock_shards(heap, &rollback_lockset);
            }
        }
    }
    if (result != UHM_OK) {
        if (decommitted_segments != 0) {
            while (decommitted_segments > 0) {
                --decommitted_segments;
                uhm_vm_commit_rollback_unlocked_preserve_error(
                    heap, (void *)segments[decommitted_segments].addr,
                    segments[decommitted_segments].size,
                    UHM_PROT_READ | UHM_PROT_WRITE);
            }
        }
        if (isolated_for_platform) {
            if (block->magic == UHM_BLOCK_MAGIC &&
                (block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                (block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0) {
                block->flags &= ~UHM_BLOCK_FLAG_VM_BUSY;
                uhm_malloc_free_tree_insert(heap, block);
            }
        }
        return result;
    }
    if (isolated_for_platform) {
        uhm_malloc_free_tree_insert(heap, block);
    }
    heap->stats.decommitted_bytes += live_bytes;
    for (i = 0; i < segment_count; ++i) {
        uhm_vm_account_decommit(heap, segments[i].addr, segments[i].size);
    }
    return UHM_OK;
}

static uhm_result uhm_block_recommit_free_extent_range(
    uhm_heap *heap, uhm_block *block, uintptr_t addr, size_t size,
    int completes_extent) {
    uintptr_t decommit_addr;
    uintptr_t decommit_end;
    uintptr_t range_end;
    size_t decommit_size;
    size_t first;
    size_t count;
    uhm_result result;

    if (!heap || !block || block->magic != UHM_BLOCK_MAGIC ||
        (block->flags & UHM_BLOCK_FLAG_FREE) == 0 ||
        (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0 ||
        (block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0 || size == 0 ||
        (addr % heap->F.vm.page_size) != 0 ||
        (size % heap->F.vm.page_size) != 0 ||
        !uhm_block_decommit_range(heap, block, &decommit_addr,
                                  &decommit_size) ||
        uhm_range_overflows(decommit_addr, decommit_size, &decommit_end) ||
        uhm_range_overflows(addr, size, &range_end) || addr < decommit_addr ||
        range_end > decommit_end ||
        (completes_extent &&
         (addr != decommit_addr || size != decommit_size))) {
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return UHM_ERR_CORRUPTION;
    }

    {
        uhm_vm_call_record commit_record;
        uhm_vm_call_record numa_record;
        size_t original_total_size = block->total_size;
        uint64_t original_descriptor_id = block->desc.id;
        int32_t numa_node;

        memset(&commit_record, 0, sizeof(commit_record));
        memset(&numa_record, 0, sizeof(numa_record));
        block->flags |= UHM_BLOCK_FLAG_VM_BUSY;
        uhm_malloc_free_tree_remove(heap, block);
        numa_node =
            uhm_numa_effective_node(heap, UHM_NUMA_POLICY_DEFAULT, -1);
        uhm_unlock(&heap->lock);
        commit_record = uhm_vm_commit_unlocked_call(
            heap, (void *)addr, size, UHM_PROT_READ | UHM_PROT_WRITE);
        if (commit_record.result == UHM_OK) {
            numa_record = uhm_vm_numa_hint_unlocked_call(
                heap, (void *)addr, size, UHM_NUMA_POLICY_DEFAULT,
                numa_node, 0u);
        }
        uhm_lock(&heap->lock);
        result = uhm_vm_commit_publish_record_locked(heap, &commit_record);
        uhm_vm_numa_hint_publish_best_effort_record_locked(heap,
                                                          &numa_record);
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             block->magic != UHM_BLOCK_MAGIC ||
             (block->flags & UHM_BLOCK_FLAG_FREE) == 0 ||
             (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 ||
             (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0 ||
             block->total_size != original_total_size ||
             block->desc.id != original_descriptor_id ||
             !uhm_block_decommit_range(heap, block, &decommit_addr,
                                       &decommit_size) ||
             uhm_range_overflows(decommit_addr, decommit_size,
                                 &decommit_end) ||
             uhm_range_overflows(addr, size, &range_end) ||
             addr < decommit_addr || range_end > decommit_end)) {
            result = UHM_ERR_CORRUPTION;
        }
        if (result != UHM_OK) {
            if (commit_record.result == UHM_OK || commit_record.did_call) {
                uhm_vm_decommit_rollback_unlocked_preserve_error(
                    heap, (void *)addr, size);
            }
            if (block->magic == UHM_BLOCK_MAGIC &&
                (block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
                (block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0) {
                block->flags &= ~UHM_BLOCK_FLAG_VM_BUSY;
                uhm_malloc_free_tree_insert(heap, block);
            }
            return result;
        }
    }

    {
        uhm_lockset publish_lockset;
        uhm_page_lock_shard_range(heap, first, count, &publish_lockset);
        uhm_extent_lock_all_shards(heap, &publish_lockset);
        if (completes_extent) {
            block->flags &= ~UHM_BLOCK_FLAG_DECOMMITTED;
            block->flags |= UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;
        }
        block->flags &= ~UHM_BLOCK_FLAG_DECOMMIT_RETAINED;
        block->flags &= ~UHM_BLOCK_FLAG_VM_BUSY;
        block->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(block);
        result = uhm_page_mark_range_locked_budgeted(
            heap, addr, size, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
            UHM_PROT_READ | UHM_PROT_WRITE, block->desc.id,
            block->generation, &publish_lockset);
        if (result != UHM_OK) {
            if (completes_extent) {
                block->flags |= UHM_BLOCK_FLAG_DECOMMITTED;
                block->flags &= ~UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;
            }
            block->flags &= ~UHM_BLOCK_FLAG_DECOMMIT_RETAINED;
            block->flags |= UHM_BLOCK_FLAG_VM_BUSY;
            block->generation = uhm_heap_generation_next(heap);
            uhm_block_descriptor_set(block);
            (void)uhm_page_mark_range_locked_budgeted(
                heap, addr, size, UHM_PAGE_OWNER_MALLOC,
                UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_DECOMMITTED,
                UHM_PROT_NONE, block->desc.id, block->generation,
                &publish_lockset);
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result != UHM_OK) {
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)addr, size);
        if (block->magic == UHM_BLOCK_MAGIC &&
            (block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
            (block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0) {
            block->flags &= ~UHM_BLOCK_FLAG_VM_BUSY;
            uhm_malloc_free_tree_insert(heap, block);
        }
        return result;
    }
    uhm_malloc_free_tree_insert(heap, block);
    heap->stats.committed_bytes += size;
    if (uhm_diag_note_recommit(heap, addr, size) != 0) {
        block->flags |= UHM_BLOCK_FLAG_DECOMMIT_HOT;
    }
    return UHM_OK;
}

static uhm_result uhm_block_recommit_split_prefix(
    uhm_heap *heap, uhm_block *block, size_t wanted,
    int *handled_out) {
    uintptr_t decommit_addr;
    uintptr_t decommit_end;
    uintptr_t split_addr;
    uintptr_t split_header_end;
    uintptr_t commit_end;
    size_t decommit_size;
    size_t remaining;
    size_t header;

    if (handled_out) {
        *handled_out = 0;
    }
    if (!heap || !block || !handled_out ||
        block->magic != UHM_BLOCK_MAGIC ||
        (block->flags & UHM_BLOCK_FLAG_FREE) == 0 ||
        (block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0 ||
        wanted > block->total_size) {
        return UHM_ERR_CORRUPTION;
    }
    if ((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0) {
        return UHM_OK;
    }
    if ((block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0) {
        return UHM_OK;
    }
    header = uhm_malloc_header_size();
    remaining = block->total_size - wanted;
    if (remaining < header + UHM_MIN_ALIGNMENT ||
        !uhm_block_decommit_range(heap, block, &decommit_addr,
                                  &decommit_size) ||
        uhm_range_overflows(decommit_addr, decommit_size, &decommit_end) ||
        uhm_range_overflows((uintptr_t)block, wanted, &split_addr) ||
        uhm_range_overflows(split_addr, header, &split_header_end) ||
        uhm_align_up_uintptr_checked(split_header_end,
                                     heap->F.vm.page_size, &commit_end)) {
        return UHM_OK;
    }
    if (commit_end > decommit_end) {
        commit_end = decommit_end;
    }
    if (commit_end >= decommit_end) {
        return UHM_OK;
    }

    *handled_out = 1;
    if (commit_end <= decommit_addr) {
        return UHM_OK;
    }
    return uhm_block_recommit_free_extent_range(
        heap, block, decommit_addr,
        (size_t)(commit_end - decommit_addr), 0);
}

static uhm_result uhm_block_recommit_free_extent(uhm_heap *heap,
                                                 uhm_block *block,
                                                 int allow_platform_unlock) {
    uintptr_t addr;
    size_t size;

    (void)allow_platform_unlock;
    if (!block || block->magic != UHM_BLOCK_MAGIC ||
        (block->flags & UHM_BLOCK_FLAG_FREE) == 0) {
        return UHM_ERR_CORRUPTION;
    }
    if ((block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0) {
        return UHM_ERR_BUSY;
    }
    if ((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) == 0) {
        uhm_block_release_retained_free_extent(heap, block);
        return UHM_OK;
    }
    if (!uhm_block_decommit_range(heap, block, &addr, &size)) {
        block->flags &= ~UHM_BLOCK_FLAG_DECOMMITTED;
        block->flags &= ~UHM_BLOCK_FLAG_DECOMMIT_RETAINED;
        block->flags &= ~UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;
        block->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(block);
        return UHM_OK;
    }
    return uhm_block_recommit_free_extent_range(heap, block, addr, size, 1);
}

