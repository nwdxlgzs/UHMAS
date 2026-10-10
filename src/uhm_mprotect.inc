static uhm_result uhm_mprotect_page_private_malloc(uhm_heap *heap, uintptr_t addr,
                                                   size_t size, uint32_t prot,
                                                   uhm_tx *tx) {
    uhm_block *block;
    uint32_t old_commit_state = UHM_PAGE_COMMIT_LIVE;
    uint32_t old_prot = UHM_PROT_NONE;
    uint64_t old_descriptor_id = 0;
    uint64_t old_generation = 0;
    uint64_t protecting_generation_id;
    size_t target_first = 0;
    size_t target_count = 0;
    int state_loaded_from_descriptor = 0;
    uhm_result result;

    if (uhm_page_range_overlaps_va_state(heap, addr, size, UHM_PAGE_VA_PROTECTING)) {
        return UHM_ERR_PROTECTION;
    }
    block = uhm_find_page_private_block_covering(
        heap, addr, size, &old_commit_state, &old_prot,
        &old_descriptor_id, &old_generation, &state_loaded_from_descriptor);
    if (!block) {
        return UHM_ERR_UNSUPPORTED;
    }
    if ((!state_loaded_from_descriptor &&
         !uhm_page_range_get_expected_state(heap, addr, size,
                                            UHM_PAGE_OWNER_MALLOC,
                                            UHM_PAGE_VA_MALLOC_LIVE,
                                            &old_commit_state, &old_prot,
                                            &old_descriptor_id,
                                            &old_generation)) ||
        old_descriptor_id != block->desc.id) {
        return UHM_ERR_PROTECTION;
    }
    uhm_tx_stage(tx, UHM_TX_STAGE_PREPARED);
    if (old_prot == prot) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_page_private_noop_calls));
        uhm_tx_stage(tx, UHM_TX_STAGE_PUBLISH);
        tx->published = 1u;
        tx->publish_generation = block->generation;
        return UHM_OK;
    }
    protecting_generation_id = uhm_heap_generation_next(heap);
    if (!uhm_page_span(heap, addr, size, &target_first, &target_count)) {
        (void)uhm_bump_failure_generation(heap);
        return UHM_ERR_CORRUPTION;
    }
    {
        uhm_lockset mark_lockset;
        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &mark_lockset);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, target_first, target_count, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_PROTECTING, old_commit_state, old_prot,
            block->desc.id, protecting_generation_id, &mark_lockset, 0);
        uhm_page_unlock_shards(heap, &mark_lockset);
    }
    if (result != UHM_OK) {
        return result;
    }
    if (!uhm_page_range_matches_state_generation_id(
            heap, addr, size, UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_PROTECTING,
            old_commit_state, old_prot, block->desc.id,
            protecting_generation_id)) {
        uhm_lockset restore_lockset;
        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &restore_lockset);
        (void)uhm_page_span_mark_range_locked_prepared(
            heap, target_first, target_count, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, old_commit_state, old_prot,
            old_descriptor_id, block->generation, &restore_lockset, 0);
        uhm_page_unlock_shards(heap, &restore_lockset);
        return UHM_ERR_CORRUPTION;
    }

    uhm_tx_stage(tx, UHM_TX_STAGE_PLATFORM);
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_platform_calls));
    {
        uhm_vm_call_record protect_record;

        uhm_unlock(&heap->lock);
        protect_record = uhm_vm_protect_unlocked_call(heap, (void *)addr,
                                                      size, prot);
        uhm_lock(&heap->lock);
        result = uhm_vm_protect_publish_record_locked(heap, &protect_record);
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             !block || block->magic != UHM_BLOCK_MAGIC ||
             (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) == 0 ||
             block->desc.id != old_descriptor_id ||
              !uhm_page_range_matches_state_generation_id(
                  heap, addr, size, UHM_PAGE_OWNER_MALLOC,
                  UHM_PAGE_VA_PROTECTING, old_commit_state, old_prot,
                  old_descriptor_id, protecting_generation_id))) {
            uhm_vm_call_record restore_record;

            uhm_unlock(&heap->lock);
            restore_record = uhm_vm_protect_unlocked_call(
                heap, (void *)addr, size, old_prot);
            uhm_lock(&heap->lock);
            (void)uhm_vm_protect_publish_record_locked(heap,
                                                       &restore_record);
            result = UHM_ERR_CORRUPTION;
        }
    }
    if (result == UHM_OK) {
        uhm_lockset publish_lockset;
        tx->platform_done = 1u;
        uhm_tx_stage(tx, UHM_TX_STAGE_PUBLISH);
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_publish_calls));
        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &publish_lockset);
        uhm_extent_lock_all_shards(heap, &publish_lockset);
        block->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(block);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, target_first, target_count, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, old_commit_state, prot, block->desc.id,
            block->generation, &publish_lockset, 0);
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
        if (result == UHM_OK) {
            tx->published = 1u;
            tx->publish_generation = block->generation;
        } else {
            (void)uhm_bump_failure_generation(heap);
        }
    } else {
        uhm_tx_stage(tx, UHM_TX_STAGE_ROLLBACK);
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_rollback_calls));
        {
            uhm_lockset restore_lockset;
            uhm_page_lock_shard_range(heap, target_first, target_count,
                                      &restore_lockset);
            (void)uhm_page_span_mark_range_locked_prepared(
                heap, target_first, target_count, UHM_PAGE_OWNER_MALLOC,
                UHM_PAGE_VA_MALLOC_LIVE, old_commit_state, old_prot,
                block->desc.id, block->generation, &restore_lockset, 0);
            uhm_page_unlock_shards(heap, &restore_lockset);
        }
        tx->rolled_back = 1u;
    }
    return result;
}

static void uhm_diag_note_mprotect_vma_segments(uhm_heap *heap,
                                                size_t segments) {
#if UHMAS_ENABLE_DIAGNOSTICS
    if (heap) {
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_vma_max_segments),
            (uint64_t)segments);
    }
#else
    (void)heap;
    (void)segments;
#endif
}

static int uhm_mprotect_collect_mmap_range(uhm_heap *heap, uintptr_t addr,
                                           size_t size, uhm_vma **first_out,
                                           uhm_vma **last_out,
                                           size_t *segment_count_out) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *first = 0;
    uhm_vma *vma = 0;
    uhm_vma *last = 0;
    size_t segments = 0;

    if (uhm_range_overflows(addr, size, &end)) {
        return 0;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_vma_collect_queries));
    first = uhm_find_vma_covering(heap, addr, 1u);
    if (!first) {
        return 0;
    }
    cursor = addr;
    vma = first;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        uint32_t commit_state;

        if (!vma || vma->magic != UHM_VMA_MAGIC ||
            uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
            cursor < vma->addr || cursor >= vma_end) {
            return 0;
        }
        seg_end = vma_end < end ? vma_end : end;
        commit_state = uhm_vma_commit_state_from_flags(vma->flags);
        if (!uhm_page_range_matches_descriptor(
                heap, cursor, (size_t)(seg_end - cursor),
                UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
                commit_state, vma->prot, &vma->desc, 1, 1)) {
            return 0;
        }
        last = vma;
        segments++;
        cursor = seg_end;
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }
    uhm_diag_note_mprotect_vma_segments(heap, segments);
    if (first_out) {
        *first_out = first;
    }
    if (last_out) {
        *last_out = last;
    }
    if (segment_count_out) {
        *segment_count_out = segments;
    }
    return first != 0 && last != 0;
}

static int uhm_mprotect_vma_segment_at(uhm_vma *vma, uintptr_t cursor,
                                       uintptr_t end, uintptr_t *vma_end_out,
                                       uintptr_t *seg_end_out) {
    uintptr_t vma_end;
    uintptr_t seg_end;

    if (!vma || vma->magic != UHM_VMA_MAGIC ||
        uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
        cursor < vma->addr || cursor >= vma_end) {
        return 0;
    }
    seg_end = vma_end < end ? vma_end : end;
    if (seg_end <= cursor) {
        return 0;
    }
    if (vma_end_out) {
        *vma_end_out = vma_end;
    }
    if (seg_end_out) {
        *seg_end_out = seg_end;
    }
    return 1;
}

static uhm_vma *uhm_vma_tree_successor(uhm_vma *vma) {
    uhm_vma *parent;

    if (!vma) {
        return 0;
    }
    if (vma->tree_right) {
        vma = vma->tree_right;
        while (vma->tree_left) {
            vma = vma->tree_left;
        }
        return vma;
    }
    parent = vma->tree_parent;
    while (parent && vma == parent->tree_right) {
        vma = parent;
        parent = parent->tree_parent;
    }
    return parent;
}

static uhm_vma *uhm_vma_tree_predecessor(uhm_vma *vma) {
    uhm_vma *parent;

    if (!vma) {
        return 0;
    }
    if (vma->tree_left) {
        vma = vma->tree_left;
        while (vma->tree_right) {
            vma = vma->tree_right;
        }
        return vma;
    }
    parent = vma->tree_parent;
    while (parent && vma == parent->tree_left) {
        vma = parent;
        parent = parent->tree_parent;
    }
    return parent;
}

static int uhm_mprotect_mmap_range_already_prot(uhm_vma *first_vma,
                                                uintptr_t addr, size_t size,
                                                uint32_t prot) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *vma;

    if (uhm_range_overflows(addr, size, &end)) {
        return 0;
    }
    cursor = addr;
    vma = first_vma;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;

        if (!uhm_mprotect_vma_segment_at(vma, cursor, end, &vma_end,
                                         &seg_end) ||
            vma->prot != prot) {
            return 0;
        }
        vma = uhm_vma_tree_successor(vma);
        cursor = seg_end;
    }
    return 1;
}

static uhm_result uhm_mprotect_mark_mmap_range(uhm_heap *heap,
                                               uhm_vma *first_vma,
                                               uintptr_t addr, size_t size,
                                               size_t target_first,
                                               size_t target_count,
                                               uint32_t va_state,
                                               uint64_t generation,
                                               const uhm_lockset *lockset) {
    uintptr_t end;
    uintptr_t cursor;
    size_t page_cursor;
    size_t page_end;
    size_t page_size;
    uhm_vma *vma;

    if (!heap || !lockset || target_count == 0 ||
        target_first > heap->core.page_count ||
        target_count > heap->core.page_count - target_first ||
        uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    page_size = heap->F.vm.page_size;
    if (page_size == 0 || addr < heap->core.arena_base ||
        ((size_t)(addr - heap->core.arena_base) % page_size) != 0 ||
        (size_t)(addr - heap->core.arena_base) / page_size != target_first ||
        (size % page_size) != 0 || size / page_size != target_count) {
        return UHM_ERR_CORRUPTION;
    }
    {
        uhm_result ready_result = uhm_page_span_mark_range_ready_locked(
            heap, target_first, target_count, lockset);
        if (ready_result != UHM_OK) {
            return ready_result;
        }
    }
    cursor = addr;
    page_cursor = target_first;
    page_end = target_first + target_count;
    vma = first_vma;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        size_t seg_size;
        size_t seg_pages;
        uhm_result result;

        if (!uhm_mprotect_vma_segment_at(vma, cursor, end, &vma_end,
                                         &seg_end)) {
            return UHM_ERR_CORRUPTION;
        }
        seg_size = (size_t)(seg_end - cursor);
        if ((seg_size % page_size) != 0) {
            return UHM_ERR_CORRUPTION;
        }
        seg_pages = seg_size / page_size;
        if (seg_pages == 0 || seg_pages > page_end - page_cursor) {
            return UHM_ERR_CORRUPTION;
        }
        result = uhm_page_span_mark_range_locked_prepared(
            heap, page_cursor, seg_pages, UHM_PAGE_OWNER_MMAP, va_state,
            uhm_vma_commit_state_from_flags(vma->flags), vma->prot,
            vma->desc.id, generation, lockset, 1);
        if (result != UHM_OK) {
            return result;
        }
        vma = uhm_vma_tree_successor(vma);
        cursor = seg_end;
        page_cursor += seg_pages;
    }
    return page_cursor == page_end ? UHM_OK : UHM_ERR_CORRUPTION;
}

static uhm_result uhm_mprotect_restore_mmap_range(uhm_heap *heap, uintptr_t addr,
                                                  uhm_vma *first_vma,
                                                  size_t size,
                                                  size_t target_first,
                                                  size_t target_count,
                                                  const uhm_lockset *lockset) {
    uintptr_t end;
    uintptr_t cursor;
    size_t page_cursor;
    size_t page_end;
    size_t page_size;
    uhm_vma *vma;

    if (!heap || !lockset || target_count == 0 ||
        target_first > heap->core.page_count ||
        target_count > heap->core.page_count - target_first ||
        uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    page_size = heap->F.vm.page_size;
    if (page_size == 0 || addr < heap->core.arena_base ||
        ((size_t)(addr - heap->core.arena_base) % page_size) != 0 ||
        (size_t)(addr - heap->core.arena_base) / page_size != target_first ||
        (size % page_size) != 0 || size / page_size != target_count) {
        return UHM_ERR_CORRUPTION;
    }
    {
        uhm_result ready_result = uhm_page_span_mark_range_ready_locked(
            heap, target_first, target_count, lockset);
        if (ready_result != UHM_OK) {
            return ready_result;
        }
    }
    cursor = addr;
    page_cursor = target_first;
    page_end = target_first + target_count;
    vma = first_vma;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        size_t seg_size;
        size_t seg_pages;
        uhm_result result;

        if (!uhm_mprotect_vma_segment_at(vma, cursor, end, &vma_end,
                                         &seg_end)) {
            return UHM_ERR_CORRUPTION;
        }
        seg_size = (size_t)(seg_end - cursor);
        if ((seg_size % page_size) != 0) {
            return UHM_ERR_CORRUPTION;
        }
        seg_pages = seg_size / page_size;
        if (seg_pages == 0 || seg_pages > page_end - page_cursor) {
            return UHM_ERR_CORRUPTION;
        }
        result = uhm_page_span_mark_range_locked_prepared(
            heap, page_cursor, seg_pages, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE,
            uhm_vma_commit_state_from_flags(vma->flags), vma->prot,
            vma->desc.id, vma->generation, lockset, 1);
        if (result != UHM_OK) {
            return result;
        }
        vma = uhm_vma_tree_successor(vma);
        cursor = seg_end;
        page_cursor += seg_pages;
    }
    return page_cursor == page_end ? UHM_OK : UHM_ERR_CORRUPTION;
}

static int uhm_mprotect_mmap_range_matches_protecting(
    uhm_heap *heap, uhm_vma *first_vma, uhm_vma *last_vma, uintptr_t addr,
    size_t size, uint64_t protecting_generation, size_t expected_segments) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *vma;
    uhm_vma *last_seen = 0;
    size_t segments = 0;

    if (uhm_range_overflows(addr, size, &end) || !first_vma || !last_vma) {
        return 0;
    }
    cursor = addr;
    vma = first_vma;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        size_t seg_size;
        uint32_t commit_state;

        if (!uhm_mprotect_vma_segment_at(vma, cursor, end, &vma_end,
                                         &seg_end)) {
            return 0;
        }
        seg_size = (size_t)(seg_end - cursor);
        commit_state = uhm_vma_commit_state_from_flags(vma->flags);
        if (!uhm_page_range_matches_state_generation_id(
                heap, cursor, seg_size, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_PROTECTING, commit_state, vma->prot,
                vma->desc.id, protecting_generation)) {
            return 0;
        }
        last_seen = vma;
        ++segments;
        cursor = seg_end;
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }
    return last_seen == last_vma && segments == expected_segments;
}

static void uhm_mprotect_platform_run_locked(uhm_heap *heap,
                                             uintptr_t run_start,
                                             uintptr_t run_end,
                                             uint32_t prot,
                                             uhm_result *result_inout) {
    uhm_vm_call_record restore_record;
    uhm_result publish_result;

    if (run_end <= run_start) {
        return;
    }
    uhm_unlock(&heap->lock);
    restore_record = uhm_vm_protect_unlocked_call(
        heap, (void *)run_start, (size_t)(run_end - run_start), prot);
    uhm_lock(&heap->lock);
    publish_result = uhm_vm_protect_publish_record_locked(
        heap, &restore_record);
    if (publish_result != UHM_OK && result_inout &&
        *result_inout == UHM_OK) {
        *result_inout = publish_result;
    }
}

static uhm_result uhm_mprotect_restore_platform_mmap_range_locked(
    uhm_heap *heap, uintptr_t addr, size_t size, uhm_vma *first_vma) {
    uintptr_t end;
    uintptr_t cursor;
    uintptr_t run_start = 0;
    uintptr_t run_end = 0;
    uhm_vma *vma;
    uint32_t run_prot = UHM_PROT_NONE;
    uint32_t run_commit_state = UHM_PAGE_COMMIT_NONE;
    uhm_result result = UHM_OK;
    int have_run = 0;

    if (uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    cursor = addr;
    vma = first_vma;
    while (cursor < end) {
        uhm_vma *next_vma;
        uintptr_t vma_end;
        uintptr_t seg_end;
        uint32_t old_prot;
        uint32_t old_commit_state;

        if (!uhm_mprotect_vma_segment_at(vma, cursor, end, &vma_end,
                                         &seg_end)) {
            if (have_run) {
                uhm_mprotect_platform_run_locked(
                    heap, run_start, run_end, run_prot, &result);
            }
            return result != UHM_OK ? result : UHM_ERR_CORRUPTION;
        }
        next_vma = uhm_vma_tree_successor(vma);
        old_prot = vma->prot;
        old_commit_state = uhm_vma_commit_state_from_flags(vma->flags);
        if (old_commit_state != UHM_PAGE_COMMIT_LIVE) {
            if (have_run) {
                uhm_mprotect_platform_run_locked(
                    heap, run_start, run_end, run_prot, &result);
                have_run = 0;
            }
            if (result != UHM_OK) {
                return result;
            }
            cursor = seg_end;
            vma = next_vma;
            continue;
        }

        if (have_run && cursor == run_end && old_prot == run_prot &&
            old_commit_state == run_commit_state) {
            run_end = seg_end;
        } else {
            if (have_run) {
                uhm_mprotect_platform_run_locked(
                    heap, run_start, run_end, run_prot, &result);
                if (result != UHM_OK) {
                    return result;
                }
            }
            run_start = cursor;
            run_end = seg_end;
            run_prot = old_prot;
            run_commit_state = old_commit_state;
            have_run = 1;
        }
        cursor = seg_end;
        vma = next_vma;
    }
    if (have_run) {
        uhm_mprotect_platform_run_locked(heap, run_start, run_end,
                                        run_prot, &result);
    }
    return result;
}

static uhm_result uhm_mprotect_platform_mmap_range_locked(
    uhm_heap *heap, uintptr_t addr, size_t size, uhm_vma *first_vma,
    uint32_t prot, int *attempted_out) {
    uintptr_t end;
    uintptr_t cursor;
    uintptr_t run_start = 0;
    uintptr_t run_end = 0;
    uhm_vma *vma;
    uhm_result result = UHM_OK;
    int have_run = 0;

    if (attempted_out) {
        *attempted_out = 0;
    }
    if (uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    cursor = addr;
    vma = first_vma;
    while (cursor < end) {
        uhm_vma *next_vma;
        uintptr_t vma_end;
        uintptr_t seg_end;
        uint32_t commit_state;

        if (!uhm_mprotect_vma_segment_at(vma, cursor, end, &vma_end,
                                         &seg_end)) {
            if (have_run) {
                if (attempted_out) {
                    *attempted_out = 1;
                }
                uhm_mprotect_platform_run_locked(
                    heap, run_start, run_end, prot, &result);
            }
            return result != UHM_OK ? result : UHM_ERR_CORRUPTION;
        }
        next_vma = uhm_vma_tree_successor(vma);
        commit_state = uhm_vma_commit_state_from_flags(vma->flags);

        if (commit_state == UHM_PAGE_COMMIT_LIVE) {
            if (have_run && cursor == run_end) {
                run_end = seg_end;
            } else {
                if (have_run) {
                    if (attempted_out) {
                        *attempted_out = 1;
                    }
                    uhm_mprotect_platform_run_locked(
                        heap, run_start, run_end, prot, &result);
                    if (result != UHM_OK) {
                        return result;
                    }
                }
                run_start = cursor;
                run_end = seg_end;
                have_run = 1;
            }
        } else if (have_run) {
            if (attempted_out) {
                *attempted_out = 1;
            }
            uhm_mprotect_platform_run_locked(heap, run_start, run_end, prot,
                                            &result);
            have_run = 0;
            if (result != UHM_OK) {
                return result;
            }
        }

        cursor = seg_end;
        vma = next_vma;
    }
    if (have_run) {
        if (attempted_out) {
            *attempted_out = 1;
        }
        uhm_mprotect_platform_run_locked(heap, run_start, run_end, prot,
                                        &result);
    }
    return result;
}

static int uhm_mprotect_publish_subspan(
    uhm_heap *heap, uintptr_t publish_start, size_t publish_first,
    size_t publish_count, uintptr_t addr, size_t size, size_t *first_out,
    size_t *count_out) {
    uintptr_t publish_end;
    uintptr_t end;
    size_t page_size;
    size_t publish_size;
    size_t offset;

    if (!heap || !first_out || !count_out || publish_count == 0 ||
        publish_first > heap->core.page_count ||
        publish_count > heap->core.page_count - publish_first) {
        return 0;
    }
    page_size = heap->F.vm.page_size;
    if (page_size == 0 || publish_count > SIZE_MAX / page_size) {
        return 0;
    }
    publish_size = publish_count * page_size;
    if (publish_start < heap->core.arena_base ||
        ((size_t)(publish_start - heap->core.arena_base) % page_size) != 0 ||
        (size_t)(publish_start - heap->core.arena_base) / page_size !=
            publish_first ||
        uhm_range_overflows(publish_start, publish_size, &publish_end) ||
        size == 0 || (size % page_size) != 0 ||
        uhm_range_overflows(addr, size, &end) || addr < publish_start ||
        end > publish_end) {
        return 0;
    }
    offset = (size_t)(addr - publish_start);
    if ((offset % page_size) != 0) {
        return 0;
    }
    *first_out = publish_first + offset / page_size;
    *count_out = size / page_size;
    return *count_out != 0 && *first_out <= heap->core.page_count &&
           *count_out <= heap->core.page_count - *first_out;
}

static uhm_result uhm_mprotect_publish_mmap_range(uhm_heap *heap,
                                                  uhm_vma *first_vma,
                                                  uintptr_t addr,
                                                  size_t size,
                                                  uint32_t prot,
                                                  uhm_vma *left_vma,
                                                  uhm_vma *right_vma,
                                                  uhm_vma **changed_out,
                                                  uintptr_t publish_start,
                                                  size_t publish_first,
                                                  size_t publish_count,
                                                  const uhm_lockset *lockset) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *changed = 0;
    uhm_vma *vma;

    if (!lockset || uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    cursor = addr;
    vma = first_vma;
    while (cursor < end) {
        uhm_vma *next_vma;
        uintptr_t vma_start;
        uintptr_t vma_end;
        uintptr_t seg_end;
        uint32_t old_prot;
        uint32_t old_flags;
        uint32_t old_commit_state;
        size_t live_first;
        size_t live_count;
        uhm_result result;

        if (!uhm_mprotect_vma_segment_at(vma, cursor, end, &vma_end,
                                         &seg_end)) {
            return UHM_ERR_CORRUPTION;
        }
        vma_start = vma->addr;
        next_vma = uhm_vma_tree_successor(vma);
        old_prot = vma->prot;
        old_flags = vma->flags & ~UHM_META_FLAG_EMERGENCY;
        old_commit_state = uhm_vma_commit_state_from_flags(vma->flags);

        if (cursor == vma_start && seg_end == vma_end) {
            if (!uhm_mprotect_publish_subspan(
                    heap, publish_start, publish_first, publish_count,
                    vma->addr, vma->size, &live_first, &live_count)) {
                return UHM_ERR_CORRUPTION;
            }
            vma->prot = prot;
            vma->generation = uhm_heap_generation_next(heap);
            uhm_vma_descriptor_sync(vma, old_commit_state);
            result = uhm_page_span_mark_range_locked_prepared(
                heap, live_first, live_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, old_commit_state, prot,
                vma->desc.id, vma->generation, lockset, 1);
            if (result != UHM_OK) {
                return result;
            }
            if (!changed) {
                changed = vma;
            }
        } else if (cursor == vma_start) {
            size_t right_first;
            size_t right_count;

            if (!right_vma || right_vma->addr != seg_end ||
                right_vma->size != vma_end - seg_end ||
                !uhm_mprotect_publish_subspan(
                    heap, publish_start, publish_first, publish_count,
                    vma_start, (size_t)(seg_end - vma_start), &live_first,
                    &live_count) ||
                !uhm_mprotect_publish_subspan(
                    heap, publish_start, publish_first, publish_count,
                    right_vma->addr, right_vma->size, &right_first,
                    &right_count)) {
                return UHM_ERR_CORRUPTION;
            }
            vma->size = (size_t)(seg_end - vma_start);
            vma->prot = prot;
            vma->generation = uhm_heap_generation_next(heap);
            uhm_vma_descriptor_sync(vma, old_commit_state);
            result = uhm_page_span_mark_range_locked_prepared(
                heap, live_first, live_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, old_commit_state, prot,
                vma->desc.id, vma->generation, lockset, 1);
            if (result != UHM_OK) {
                return result;
            }
            result = uhm_page_span_mark_range_locked_prepared(
                heap, right_first, right_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, old_commit_state, old_prot,
                right_vma->desc.id, right_vma->generation, lockset, 1);
            if (result != UHM_OK) {
                return result;
            }
            uhm_vma_insert_after(heap, vma, right_vma);
            right_vma = 0;
            if (!changed) {
                changed = vma;
            }
        } else if (seg_end == vma_end) {
            size_t left_first;
            size_t left_count;

            if (!left_vma || left_vma->addr != vma_start ||
                left_vma->size != cursor - vma_start ||
                !uhm_mprotect_publish_subspan(
                    heap, publish_start, publish_first, publish_count,
                    cursor, (size_t)(seg_end - cursor), &live_first,
                    &live_count) ||
                !uhm_mprotect_publish_subspan(
                    heap, publish_start, publish_first, publish_count,
                    left_vma->addr, left_vma->size, &left_first,
                    &left_count)) {
                return UHM_ERR_CORRUPTION;
            }
            uhm_vma_tree_remove(heap, vma);
            vma->addr = cursor;
            vma->size = (size_t)(seg_end - cursor);
            vma->prot = prot;
            vma->generation = uhm_heap_generation_next(heap);
            uhm_vma_descriptor_sync(vma, old_commit_state);
            uhm_vma_tree_insert(heap, vma);
            result = uhm_page_span_mark_range_locked_prepared(
                heap, live_first, live_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, old_commit_state, prot,
                vma->desc.id, vma->generation, lockset, 1);
            if (result != UHM_OK) {
                return result;
            }
            result = uhm_page_span_mark_range_locked_prepared(
                heap, left_first, left_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, old_commit_state, old_prot,
                left_vma->desc.id, left_vma->generation, lockset, 1);
            if (result != UHM_OK) {
                return result;
            }
            uhm_vma_insert_before(heap, vma, left_vma);
            left_vma = 0;
            if (!changed) {
                changed = vma;
            }
        } else {
            return UHM_ERR_CORRUPTION;
        }

        (void)old_flags;
        vma = next_vma;
        cursor = seg_end;
    }
    if (left_vma || right_vma) {
        return UHM_ERR_CORRUPTION;
    }
    if (changed_out) {
        *changed_out = changed;
    }
    return UHM_OK;
}

static uhm_result uhm_mprotect_mmap_multi(uhm_heap *heap, uintptr_t addr,
                                          size_t size, uint32_t prot,
                                          uhm_tx *tx) {
    uintptr_t end;
    uhm_vma *first_vma = 0;
    uhm_vma *last_vma = 0;
    uhm_vma *left_vma = 0;
    uhm_vma *right_vma = 0;
    uhm_vma *changed_vma = 0;
    uint64_t protecting_generation;
    size_t target_first;
    size_t target_count;
    size_t segment_count = 0;
    uhm_result result;
    int platform_attempted = 0;
    int platform_restored = 0;

    if (uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!uhm_mprotect_collect_mmap_range(heap, addr, size, &first_vma,
                                         &last_vma, &segment_count)) {
        return UHM_ERR_NOT_MAPPED;
    }
    if (uhm_mprotect_mmap_range_already_prot(first_vma, addr, size, prot)) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_mmap_noop_calls));
        uhm_tx_stage(tx, UHM_TX_STAGE_PREPARED);
        uhm_tx_stage(tx, UHM_TX_STAGE_PUBLISH);
        tx->published = 1u;
        tx->publish_generation = first_vma ? first_vma->generation : 0u;
        return UHM_OK;
    }
    if (first_vma == last_vma) {
        return UHM_ERR_NOT_MAPPED;
    }
    {
        uintptr_t last_end_guard;
        if (uhm_range_overflows(last_vma->addr, last_vma->size,
                                &last_end_guard)) {
            return UHM_ERR_CORRUPTION;
        }
        if ((addr > first_vma->addr &&
             uhm_vma_range_overlaps_protecting(heap, first_vma)) ||
            (end < last_end_guard &&
             uhm_vma_range_overlaps_protecting(heap, last_vma))) {
            return UHM_ERR_PROTECTION;
        }
    }
    if (addr > first_vma->addr) {
        left_vma = uhm_vma_new(heap, first_vma->addr,
                               addr - first_vma->addr, first_vma->prot,
                               first_vma->flags & ~UHM_META_FLAG_EMERGENCY, 0);
        if (!left_vma) {
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_OUT_OF_MEMORY;
        }
    }
    {
        uintptr_t last_end;
        if (uhm_range_overflows(last_vma->addr, last_vma->size,
                                &last_end)) {
            if (left_vma) {
                uhm_vma_release_node(heap, left_vma);
            }
            return UHM_ERR_CORRUPTION;
        }
    if (end < last_end) {
        right_vma = uhm_vma_new(heap, end,
                                last_end - end,
                                last_vma->prot,
                                last_vma->flags & ~UHM_META_FLAG_EMERGENCY, 0);
        if (!right_vma) {
            if (left_vma) {
                uhm_vma_release_node(heap, left_vma);
            }
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_OUT_OF_MEMORY;
        }
    }
    }

    uhm_tx_stage(tx, UHM_TX_STAGE_PREPARED);
    /* Heap lock is still held; split-node allocation cannot change target VMA
       coverage, so the initial collect remains the stable revalidation base. */
    protecting_generation = uhm_heap_generation_next(heap);
    if (!uhm_page_span(heap, addr, size, &target_first, &target_count)) {
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        return UHM_ERR_CORRUPTION;
    }
    {
        uhm_lockset mark_lockset;
        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &mark_lockset);
        result = uhm_mprotect_mark_mmap_range(
            heap, first_vma, addr, size, target_first, target_count,
            UHM_PAGE_VA_PROTECTING, protecting_generation, &mark_lockset);
        uhm_page_unlock_shards(heap, &mark_lockset);
    }
    if (result != UHM_OK) {
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        return result;
    }

    uhm_tx_stage(tx, UHM_TX_STAGE_PLATFORM);
    {
        result = uhm_mprotect_platform_mmap_range_locked(
            heap, addr, size, first_vma, prot, &platform_attempted);
        if (platform_attempted) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_platform_calls));
        }
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             !uhm_mprotect_mmap_range_matches_protecting(
                 heap, first_vma, last_vma, addr, size,
                 protecting_generation, segment_count))) {
            (void)uhm_mprotect_restore_platform_mmap_range_locked(
                heap, addr, size, first_vma);
            platform_restored = 1;
            result = UHM_ERR_CORRUPTION;
        }
    }
    if (result != UHM_OK) {
        uhm_tx_stage(tx, UHM_TX_STAGE_ROLLBACK);
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_rollback_calls));
        if (platform_attempted && !platform_restored) {
            uint32_t saved_platform_error =
                atomic_load_explicit(&heap->platform_error,
                                     UHM_ATOMIC_ACQUIRE);
            (void)uhm_mprotect_restore_platform_mmap_range_locked(
                heap, addr, size, first_vma);
            atomic_store_explicit(&heap->platform_error,
                                  saved_platform_error,
                                  UHM_ATOMIC_RELEASE);
        }
        {
            uhm_lockset restore_lockset;
            uhm_page_lock_shard_range(heap, target_first, target_count,
                                      &restore_lockset);
            (void)uhm_mprotect_restore_mmap_range(
                heap, addr, first_vma, size, target_first, target_count,
                &restore_lockset);
            uhm_page_unlock_shards(heap, &restore_lockset);
        }
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        tx->rolled_back = 1u;
        return result;
    }
    tx->platform_done = 1u;

    uhm_tx_stage(tx, UHM_TX_STAGE_PUBLISH);
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_publish_calls));
    {
        uintptr_t publish_start = first_vma->addr;
        uintptr_t last_end;
        uintptr_t publish_end;
        size_t publish_first;
        size_t publish_count;
        uhm_lockset publish_lockset;

        if (uhm_range_overflows(last_vma->addr, last_vma->size,
                                &last_end)) {
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_CORRUPTION;
        }
        publish_end = last_end;

        {
            uhm_vma *left_cursor = first_vma;
            uint32_t first_commit =
                uhm_vma_commit_state_from_flags(first_vma->flags);
            uint32_t first_flags =
                first_vma->flags & ~UHM_META_FLAG_EMERGENCY;

            for (;;) {
                uhm_vma *candidate =
                    uhm_vma_tree_predecessor(left_cursor);
                uintptr_t candidate_end;

                if (!candidate || candidate->magic != UHM_VMA_MAGIC ||
                    uhm_range_overflows(candidate->addr, candidate->size,
                                        &candidate_end) ||
                    candidate_end != publish_start || candidate->prot != prot ||
                    uhm_vma_commit_state_from_flags(candidate->flags) !=
                        first_commit ||
                    (candidate->flags & ~UHM_META_FLAG_EMERGENCY) !=
                        first_flags) {
                    break;
                }
                publish_start = candidate->addr;
                left_cursor = candidate;
            }
        }
        {
            uhm_vma *right_cursor = last_vma;
            uint32_t last_commit =
                uhm_vma_commit_state_from_flags(last_vma->flags);
            uint32_t last_flags =
                last_vma->flags & ~UHM_META_FLAG_EMERGENCY;

            for (;;) {
                uhm_vma *candidate = uhm_vma_tree_successor(right_cursor);
                uintptr_t candidate_end;

                if (!candidate || candidate->magic != UHM_VMA_MAGIC ||
                    candidate->addr != publish_end || candidate->prot != prot ||
                    uhm_vma_commit_state_from_flags(candidate->flags) !=
                        last_commit ||
                    (candidate->flags & ~UHM_META_FLAG_EMERGENCY) != last_flags ||
                    uhm_range_overflows(candidate->addr, candidate->size,
                                        &candidate_end)) {
                    break;
                }
                publish_end = candidate_end;
                right_cursor = candidate;
            }
        }
        if (publish_end < publish_start ||
            !uhm_page_span(heap, publish_start, publish_end - publish_start,
                           &publish_first, &publish_count)) {
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_CORRUPTION;
        }
        uhm_page_lock_shard_range(heap, publish_first, publish_count,
                                  &publish_lockset);
        uhm_vma_lock_all_shards(heap, &publish_lockset);
        result = uhm_page_span_mark_range_ready_locked(
            heap, publish_first, publish_count, &publish_lockset);
        if (result == UHM_OK) {
            result = uhm_mprotect_publish_mmap_range(
                heap, first_vma, addr, size, prot, left_vma, right_vma,
                &changed_vma, publish_start, publish_first, publish_count,
                &publish_lockset);
        }
        if (result == UHM_OK) {
            result = uhm_vma_merge_around_with_lockset(heap, &changed_vma,
                                                       &publish_lockset);
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
        if (result != UHM_OK) {
            (void)uhm_bump_failure_generation(heap);
            return result;
        }
    }
    tx->published = 1u;
    tx->publish_generation = uhm_heap_generation_load(heap);
    return UHM_OK;
}

static uhm_result uhm_mprotect_publish_exact_merge_locked(
    uhm_heap *heap, uhm_vma **vma_inout, uintptr_t addr, size_t size,
    uint32_t prot, uint32_t flags, uint32_t commit_state,
    uhm_vma *leftmost_candidate, uhm_vma *rightmost_candidate,
    const uhm_lockset *lockset, int *merged_out) {
    uhm_vma *vma;
    uhm_vma *survivor;
    uhm_vma *rightmost;
    uhm_vma *cursor;
    uintptr_t vma_end;
    uintptr_t merged_start;
    uintptr_t merged_end;
    size_t merged_size;
    size_t merged_first;
    size_t merged_count;
    uint64_t merged_generation;
    uhm_result result;
    int saw_vma = 0;

    if (merged_out) {
        *merged_out = 0;
    }
    if (!heap || !vma_inout || !*vma_inout ||
        uhm_range_overflows(addr, size, &vma_end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    vma = *vma_inout;
    if (vma->magic != UHM_VMA_MAGIC || vma->addr != addr ||
        vma->size != size) {
        return UHM_ERR_CORRUPTION;
    }
    if (!leftmost_candidate && !rightmost_candidate) {
        return UHM_OK;
    }

    survivor = leftmost_candidate ? leftmost_candidate : vma;
    rightmost = rightmost_candidate ? rightmost_candidate : vma;
    merged_start = survivor->addr;
    merged_end = merged_start;
    cursor = survivor;
    for (;;) {
        uintptr_t cursor_end;

        if (!cursor || cursor->magic != UHM_VMA_MAGIC ||
            cursor->addr != merged_end ||
            uhm_range_overflows(cursor->addr, cursor->size, &cursor_end)) {
            return UHM_ERR_CORRUPTION;
        }
        if (cursor == vma) {
            if (cursor->addr != addr || cursor->size != size) {
                return UHM_ERR_CORRUPTION;
            }
            saw_vma = 1;
        } else if (cursor->prot != prot ||
                   (cursor->flags & ~UHM_META_FLAG_EMERGENCY) != flags ||
                   uhm_vma_commit_state_from_flags(cursor->flags) !=
                       commit_state ||
                   uhm_page_range_overlaps_va_state_locked(
                       heap, cursor->addr, cursor->size,
                       UHM_PAGE_VA_PROTECTING, lockset)) {
            return UHM_OK;
        }
        merged_end = cursor_end;
        if (cursor == rightmost) {
            break;
        }
        cursor = uhm_vma_tree_successor(cursor);
    }
    if (!saw_vma || addr < merged_start || vma_end > merged_end) {
        return UHM_ERR_CORRUPTION;
    }
    merged_size = (size_t)(merged_end - merged_start);
    if (!uhm_page_span(heap, merged_start, merged_size, &merged_first,
                       &merged_count)) {
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_page_span_mark_range_ready_locked(
        heap, merged_first, merged_count, lockset);
    if (result != UHM_OK) {
        return result;
    }
    merged_generation = uhm_heap_generation_next(heap);
    result = uhm_page_span_mark_range_locked_prepared(
        heap, merged_first, merged_count, UHM_PAGE_OWNER_MMAP,
        UHM_PAGE_VA_MMAP_LIVE, commit_state, prot, survivor->desc.id,
        merged_generation, lockset, 1);
    if (result != UHM_OK) {
        return result;
    }
    survivor->prot = prot;
    survivor->size = merged_size;
    survivor->generation = merged_generation;
    uhm_vma_descriptor_sync(survivor, commit_state);
    cursor = uhm_vma_tree_successor(survivor);
    while (cursor) {
        uhm_vma *next = uhm_vma_tree_successor(cursor);
        int reached_rightmost = cursor == rightmost;

        uhm_vma_remove(heap, cursor);
        if (reached_rightmost) {
            break;
        }
        cursor = next;
    }
    *vma_inout = survivor;
    if (merged_out) {
        *merged_out = 1;
    }
    return UHM_OK;
}

static uhm_result uhm_mprotect_publish_partial_edge_merge_locked(
    uhm_heap *heap, uhm_vma **vma_inout, uintptr_t addr, size_t size,
    uint32_t prot, uint32_t flags, uint32_t commit_state, uint32_t old_prot,
    uhm_vma *left_vma, uhm_vma *right_vma, uhm_vma *leftmost_candidate,
    uhm_vma *rightmost_candidate, const uhm_lockset *lockset,
    int *published_out) {
    uhm_vma *vma;
    uintptr_t end;
    uintptr_t vma_end;
    uhm_result result;

    if (published_out) {
        *published_out = 0;
    }
    if (!heap || !vma_inout || !*vma_inout ||
        uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    vma = *vma_inout;
    if (vma->magic != UHM_VMA_MAGIC ||
        uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
        return UHM_ERR_CORRUPTION;
    }

    if (!left_vma && right_vma && leftmost_candidate &&
        addr == vma->addr && end < vma_end) {
        uhm_vma *left = leftmost_candidate;
        uhm_vma *cursor = left;
        uintptr_t cursor_addr = left->addr;
        size_t left_size;
        size_t left_first;
        size_t left_count;
        size_t right_first;
        size_t right_count;
        uint64_t left_generation;

        for (;;) {
            uintptr_t cursor_end;
            uhm_vma *next;

            if (!cursor || cursor->magic != UHM_VMA_MAGIC ||
                cursor->addr != cursor_addr || cursor->prot != prot ||
                (cursor->flags & ~UHM_META_FLAG_EMERGENCY) != flags ||
                uhm_vma_commit_state_from_flags(cursor->flags) !=
                    commit_state ||
                uhm_page_range_overlaps_va_state_locked(
                    heap, cursor->addr, cursor->size,
                    UHM_PAGE_VA_PROTECTING, lockset) ||
                uhm_range_overflows(cursor->addr, cursor->size,
                                    &cursor_end)) {
                return UHM_OK;
            }
            next = uhm_vma_tree_successor(cursor);
            if (next == vma) {
                if (cursor_end != addr) {
                    return UHM_ERR_CORRUPTION;
                }
                break;
            }
            if (!next || next->addr != cursor_end) {
                return UHM_ERR_CORRUPTION;
            }
            cursor_addr = cursor_end;
            cursor = next;
        }
        left_size = (size_t)(end - left->addr);
        if (!uhm_page_span(heap, left->addr, left_size, &left_first,
                           &left_count) ||
            !uhm_page_span(heap, right_vma->addr, right_vma->size,
                           &right_first, &right_count)) {
            return UHM_ERR_CORRUPTION;
        }
        result = uhm_page_span_mark_range_ready_locked(
            heap, left_first, left_count, lockset);
        if (result != UHM_OK) {
            return result;
        }
        result = uhm_page_span_mark_range_ready_locked(
            heap, right_first, right_count, lockset);
        if (result != UHM_OK) {
            return result;
        }
        left_generation = uhm_heap_generation_next(heap);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, left_first, left_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, commit_state, prot, left->desc.id,
            left_generation, lockset, 1);
        if (result != UHM_OK) {
            return result;
        }
        result = uhm_page_span_mark_range_locked_prepared(
            heap, right_first, right_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, commit_state, old_prot,
            right_vma->desc.id, right_vma->generation, lockset, 1);
        if (result != UHM_OK) {
            return result;
        }
        left->size = left_size;
        left->generation = left_generation;
        uhm_vma_descriptor_sync(left, commit_state);
        uhm_vma_descriptor_sync(right_vma, commit_state);
        cursor = uhm_vma_tree_successor(left);
        while (cursor) {
            uhm_vma *next = uhm_vma_tree_successor(cursor);
            int reached_vma = cursor == vma;

            uhm_vma_remove(heap, cursor);
            if (reached_vma) {
                break;
            }
            cursor = next;
        }
        uhm_vma_insert_after(heap, left, right_vma);
        *vma_inout = left;
        if (published_out) {
            *published_out = 1;
        }
        return UHM_OK;
    }

    if (left_vma && !right_vma && rightmost_candidate &&
        addr > vma->addr && end == vma_end) {
        uhm_vma *cursor = uhm_vma_tree_successor(vma);
        uintptr_t cursor_addr = end;
        uintptr_t right_end;
        size_t merged_size;
        size_t left_first;
        size_t left_count;
        size_t merged_first;
        size_t merged_count;
        uint64_t merged_generation;

        for (;;) {
            uintptr_t cursor_end;

            if (!cursor || cursor->magic != UHM_VMA_MAGIC ||
                cursor->addr != cursor_addr || cursor->prot != prot ||
                (cursor->flags & ~UHM_META_FLAG_EMERGENCY) != flags ||
                uhm_vma_commit_state_from_flags(cursor->flags) !=
                    commit_state ||
                uhm_page_range_overlaps_va_state_locked(
                    heap, cursor->addr, cursor->size,
                    UHM_PAGE_VA_PROTECTING, lockset) ||
                uhm_range_overflows(cursor->addr, cursor->size,
                                    &cursor_end)) {
                return UHM_OK;
            }
            right_end = cursor_end;
            if (cursor == rightmost_candidate) {
                break;
            }
            cursor_addr = cursor_end;
            cursor = uhm_vma_tree_successor(cursor);
        }
        merged_size = (size_t)(right_end - addr);
        if (!uhm_page_span(heap, left_vma->addr, left_vma->size,
                           &left_first, &left_count) ||
            !uhm_page_span(heap, addr, merged_size, &merged_first,
                           &merged_count)) {
            return UHM_ERR_CORRUPTION;
        }
        result = uhm_page_span_mark_range_ready_locked(
            heap, left_first, left_count, lockset);
        if (result != UHM_OK) {
            return result;
        }
        result = uhm_page_span_mark_range_ready_locked(
            heap, merged_first, merged_count, lockset);
        if (result != UHM_OK) {
            return result;
        }
        merged_generation = uhm_heap_generation_next(heap);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, left_first, left_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, commit_state, old_prot,
            left_vma->desc.id, left_vma->generation, lockset, 1);
        if (result != UHM_OK) {
            return result;
        }
        result = uhm_page_span_mark_range_locked_prepared(
            heap, merged_first, merged_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, commit_state, prot, vma->desc.id,
            merged_generation, lockset, 1);
        if (result != UHM_OK) {
            return result;
        }
        uhm_vma_tree_remove(heap, vma);
        uhm_vma_insert_before(heap, vma, left_vma);
        vma->prot = prot;
        vma->addr = addr;
        vma->size = merged_size;
        vma->generation = merged_generation;
        uhm_vma_descriptor_sync(vma, commit_state);
        uhm_vma_tree_insert(heap, vma);
        uhm_vma_descriptor_sync(left_vma, commit_state);
        cursor = uhm_vma_tree_successor(vma);
        while (cursor) {
            uhm_vma *next = uhm_vma_tree_successor(cursor);
            int reached_rightmost = cursor == rightmost_candidate;

            uhm_vma_remove(heap, cursor);
            if (reached_rightmost) {
                break;
            }
            cursor = next;
        }
        *vma_inout = vma;
        if (published_out) {
            *published_out = 1;
        }
        return UHM_OK;
    }

    return UHM_OK;
}

static uhm_result uhm_mprotect_pinned(uhm_heap *heap, void *addr_ptr,
                                      size_t size, uint32_t prot) {
    uintptr_t addr = (uintptr_t)addr_ptr;
    uintptr_t end;
    size_t rounded;
    uhm_vma *vma;
    uhm_vma *left_vma = 0;
    uhm_vma *right_vma = 0;
    uintptr_t vma_start;
    uintptr_t vma_end;
    uint32_t old_prot;
    uint32_t old_flags;
    uint32_t old_commit_state = UHM_PAGE_COMMIT_LIVE;
    uint64_t old_descriptor_id = 0;
    uint64_t old_generation = 0;
    uint64_t protecting_generation_id;
    size_t target_first = 0;
    size_t target_count = 0;
    uhm_tx tx;
    uhm_result result;
    int platform_revalidate_failed = 0;

    if (!uhm_heap_is_accessible_pinned(heap) || !addr_ptr ||
        (addr % heap->F.vm.page_size) != 0 ||
        uhm_align_up_size(size, heap->F.vm.page_size, &rounded) || rounded == 0 ||
        uhm_range_overflows(addr, rounded, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    result = uhm_vm_validate_prot(heap, prot);
    if (result != UHM_OK) {
        return result;
    }

    uhm_tx_begin(&tx, heap, UHM_TX_OP_MPROTECT, addr, rounded, prot, 0);
    uhm_tx_stage(&tx, UHM_TX_STAGE_VALIDATE);
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_INVALID_ARGUMENT);
    }
    uhm_metadata_epoch_advance(heap);
    if (addr < heap->core.arena_base || end > heap->core.arena_end) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_NOT_MAPPED);
    }
    if (addr < heap->core.malloc_hi) {
        if (uhm_page_range_overlaps_owner(heap, addr, rounded,
                                          UHM_PAGE_OWNER_CONTROL)) {
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
        }
        result = uhm_mprotect_page_private_malloc(heap, addr, rounded, prot, &tx);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, result);
    }
    if (uhm_page_range_overlaps_va_state(heap, addr, rounded, UHM_PAGE_VA_PROTECTING)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
    }
    vma = uhm_find_vma_covering(heap, addr, rounded);
    if (!vma) {
        result = uhm_mprotect_mmap_multi(heap, addr, rounded, prot, &tx);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, result);
    }
    if (!uhm_page_range_get_expected_state(heap, addr, rounded, UHM_PAGE_OWNER_MMAP,
                                           UHM_PAGE_VA_MMAP_LIVE,
                                           &old_commit_state, &old_prot,
                                           &old_descriptor_id, &old_generation)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_NOT_MAPPED);
    }
    if (!vma || old_descriptor_id != vma->desc.id) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_NOT_MAPPED);
    }
    vma_start = vma->addr;
    if (uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
    }
    old_prot = vma->prot;
    old_flags = vma->flags & ~UHM_META_FLAG_EMERGENCY;
    if (old_prot == prot) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_mmap_noop_calls));
        uhm_tx_stage(&tx, UHM_TX_STAGE_PREPARED);
        uhm_tx_stage(&tx, UHM_TX_STAGE_PUBLISH);
        tx.published = 1u;
        tx.publish_generation = vma->generation;
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_OK);
    }
    if ((addr > vma_start || end < vma_end) &&
        uhm_vma_range_overlaps_protecting(heap, vma)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
    }
    uhm_tx_record_old_mmap(&tx, vma, old_commit_state, old_prot);

    if (addr > vma_start) {
        left_vma = uhm_vma_new(heap, vma_start, addr - vma_start, old_prot, old_flags, 0);
        if (!left_vma) {
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_OUT_OF_MEMORY);
        }
        tx.left_vma = left_vma;
    }
    if (end < vma_end) {
        right_vma = uhm_vma_new(heap, end, vma_end - end, old_prot, old_flags, 0);
        if (!right_vma) {
            if (left_vma) {
                uhm_vma_release_node(heap, left_vma);
            }
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_OUT_OF_MEMORY);
        }
        tx.right_vma = right_vma;
    }

    uhm_tx_stage(&tx, UHM_TX_STAGE_PREPARED);
    if ((left_vma || right_vma) &&
        !uhm_page_range_matches_state(heap, addr, rounded,
                                      UHM_PAGE_OWNER_MMAP,
                                      UHM_PAGE_VA_MMAP_LIVE, old_commit_state,
                                      old_prot, old_descriptor_id,
                                      old_generation)) {
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
    }
    protecting_generation_id = uhm_heap_generation_next(heap);
    if (!uhm_page_span(heap, addr, rounded, &target_first, &target_count)) {
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
    }
    {
        uhm_lockset mark_lockset;
        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &mark_lockset);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_PROTECTING, old_commit_state, old_prot, vma->desc.id,
            protecting_generation_id, &mark_lockset, 0);
        uhm_page_unlock_shards(heap, &mark_lockset);
    }
    if (result != UHM_OK) {
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, result);
    }
    if (!uhm_page_range_matches_state_generation_id(
            heap, addr, rounded, UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_PROTECTING,
            old_commit_state, old_prot, vma->desc.id,
            protecting_generation_id)) {
        uhm_lockset restore_lockset;
        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &restore_lockset);
        (void)uhm_page_span_mark_range_locked_prepared(
            heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, old_commit_state, old_prot,
            old_descriptor_id, vma->generation, &restore_lockset, 0);
        uhm_page_unlock_shards(heap, &restore_lockset);
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
    }

    uhm_tx_stage(&tx, UHM_TX_STAGE_PLATFORM);
    if (old_commit_state == UHM_PAGE_COMMIT_LIVE) {
        uhm_vm_call_record protect_record;

        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_platform_calls));
        uhm_unlock(&heap->lock);
        protect_record = uhm_vm_protect_unlocked_call(heap, addr_ptr, rounded,
                                                      prot);
        uhm_lock(&heap->lock);
        result = uhm_vm_protect_publish_record_locked(heap, &protect_record);
        if (result == UHM_OK &&
            (!vma || vma->magic != UHM_VMA_MAGIC || vma->addr != vma_start ||
             vma->size != (size_t)(vma_end - vma_start) ||
             vma->prot != old_prot ||
             (vma->flags & ~UHM_META_FLAG_EMERGENCY) != old_flags ||
             vma->desc.id != old_descriptor_id ||
              !uhm_page_range_matches_state_generation_id(
                  heap, addr, rounded, UHM_PAGE_OWNER_MMAP,
                  UHM_PAGE_VA_PROTECTING, old_commit_state, old_prot,
                  old_descriptor_id, protecting_generation_id))) {
            uhm_vm_call_record restore_record;

            platform_revalidate_failed = 1;
            uhm_unlock(&heap->lock);
            restore_record = uhm_vm_protect_unlocked_call(
                heap, addr_ptr, rounded, old_prot);
            uhm_lock(&heap->lock);
            (void)uhm_vm_protect_publish_record_locked(heap, &restore_record);
            result = UHM_ERR_CORRUPTION;
        }
    }
    if (result == UHM_OK) {
        uhm_vma *anchor = vma;
        uhm_vma *exact_left_merge_vma = 0;
        uhm_vma *exact_right_merge_vma = 0;
        uhm_vma *partial_left_merge_vma = 0;
        uhm_vma *partial_right_merge_vma = 0;
        uintptr_t publish_start = vma_start;
        uintptr_t publish_end = vma_end;
        size_t publish_first;
        size_t publish_count;
        uhm_lockset publish_lockset;
        int exact_vma_publish =
            !left_vma && !right_vma && addr == vma_start && end == vma_end;
        tx.platform_done = 1u;
        uhm_tx_stage(&tx, UHM_TX_STAGE_PUBLISH);
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_publish_calls));
        if (exact_vma_publish) {
            uhm_vma *left_cursor = vma;
            uhm_vma *right_cursor = vma;

            for (;;) {
                uhm_vma *candidate =
                    uhm_vma_tree_predecessor(left_cursor);
                uintptr_t candidate_end;

                if (!candidate || candidate->magic != UHM_VMA_MAGIC ||
                    uhm_range_overflows(candidate->addr, candidate->size,
                                        &candidate_end) ||
                    candidate_end != publish_start || candidate->prot != prot ||
                    (candidate->flags & ~UHM_META_FLAG_EMERGENCY) !=
                        old_flags ||
                    uhm_vma_commit_state_from_flags(candidate->flags) !=
                        old_commit_state) {
                    break;
                }
                publish_start = candidate->addr;
                exact_left_merge_vma = candidate;
                left_cursor = candidate;
            }
            for (;;) {
                uhm_vma *candidate =
                    uhm_vma_tree_successor(right_cursor);
                uintptr_t candidate_end;

                if (!candidate || candidate->magic != UHM_VMA_MAGIC ||
                    candidate->addr != publish_end || candidate->prot != prot ||
                    (candidate->flags & ~UHM_META_FLAG_EMERGENCY) !=
                        old_flags ||
                    uhm_vma_commit_state_from_flags(candidate->flags) !=
                        old_commit_state ||
                    uhm_range_overflows(candidate->addr, candidate->size,
                                        &candidate_end)) {
                    break;
                }
                publish_end = candidate_end;
                exact_right_merge_vma = candidate;
                right_cursor = candidate;
            }
        } else {
            if (addr == vma_start) {
                uhm_vma *left_cursor = vma;
                for (;;) {
                    uhm_vma *candidate =
                        uhm_vma_tree_predecessor(left_cursor);
                    uintptr_t candidate_end;

                    if (!candidate || candidate->magic != UHM_VMA_MAGIC ||
                        uhm_range_overflows(candidate->addr, candidate->size,
                                            &candidate_end) ||
                        candidate_end != publish_start ||
                        candidate->prot != prot ||
                        (candidate->flags & ~UHM_META_FLAG_EMERGENCY) !=
                            old_flags ||
                        uhm_vma_commit_state_from_flags(candidate->flags) !=
                            old_commit_state) {
                        break;
                    }
                    publish_start = candidate->addr;
                    partial_left_merge_vma = candidate;
                    left_cursor = candidate;
                }
            }
            if (end == vma_end) {
                uhm_vma *right_cursor = vma;
                for (;;) {
                    uhm_vma *candidate =
                        uhm_vma_tree_successor(right_cursor);
                    uintptr_t candidate_end;

                    if (!candidate || candidate->magic != UHM_VMA_MAGIC ||
                        candidate->addr != publish_end ||
                        candidate->prot != prot ||
                        (candidate->flags & ~UHM_META_FLAG_EMERGENCY) !=
                            old_flags ||
                        uhm_vma_commit_state_from_flags(candidate->flags) !=
                            old_commit_state ||
                        uhm_range_overflows(candidate->addr, candidate->size,
                                            &candidate_end)) {
                        break;
                    }
                    publish_end = candidate_end;
                    partial_right_merge_vma = candidate;
                    right_cursor = candidate;
                }
            }
        }
        if (!uhm_page_span(heap, publish_start, publish_end - publish_start,
                           &publish_first, &publish_count)) {
            if (left_vma) {
                uhm_vma_release_node(heap, left_vma);
            }
            if (right_vma) {
                uhm_vma_release_node(heap, right_vma);
            }
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
        }
        uhm_page_lock_shard_range(heap, publish_first, publish_count,
                                  &publish_lockset);
        uhm_vma_lock_all_shards(heap, &publish_lockset);
        if (exact_vma_publish) {
            int exact_merge_published = 0;
            result = uhm_mprotect_publish_exact_merge_locked(
                heap, &vma, addr, rounded, prot, old_flags,
                old_commit_state, exact_left_merge_vma,
                exact_right_merge_vma, &publish_lockset,
                &exact_merge_published);
            if (result != UHM_OK) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, result);
            }
            if (exact_merge_published) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                tx.published = 1u;
                tx.publish_generation = vma->generation;
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, UHM_OK);
            }
        }
        if (!exact_vma_publish &&
            (partial_left_merge_vma || partial_right_merge_vma)) {
            int partial_edge_published = 0;
            result = uhm_mprotect_publish_partial_edge_merge_locked(
                heap, &vma, addr, rounded, prot, old_flags,
                old_commit_state, old_prot, left_vma, right_vma,
                partial_left_merge_vma, partial_right_merge_vma,
                &publish_lockset, &partial_edge_published);
            if (result != UHM_OK) {
                if (left_vma) {
                    uhm_vma_release_node(heap, left_vma);
                }
                if (right_vma) {
                    uhm_vma_release_node(heap, right_vma);
                }
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, result);
            }
            if (partial_edge_published) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                tx.published = 1u;
                tx.publish_generation = vma->generation;
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, UHM_OK);
            }
        }
        result = uhm_page_span_mark_range_ready_locked(
            heap, publish_first, publish_count, &publish_lockset);
        if (result != UHM_OK) {
            if (left_vma) {
                uhm_vma_release_node(heap, left_vma);
            }
            if (right_vma) {
                uhm_vma_release_node(heap, right_vma);
            }
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, result);
        }
        if (!exact_vma_publish) {
            uhm_vma_tree_remove(heap, vma);
            if (left_vma) {
                left_vma->prev = vma->prev;
                left_vma->next = vma;
                if (vma->prev) {
                    vma->prev->next = left_vma;
                } else {
                    heap->core.vma_head = left_vma;
                }
                vma->prev = left_vma;
                uhm_vma_tree_insert(heap, left_vma);
            }
        }
        vma->prot = prot;
        vma->addr = addr;
        vma->size = rounded;
        vma->generation = uhm_heap_generation_next(heap);
        uhm_vma_descriptor_sync(vma, old_commit_state);
        if (!exact_vma_publish) {
            uhm_vma_tree_insert(heap, vma);
        }
        result = uhm_page_span_mark_range_locked_prepared(
            heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, old_commit_state, prot, vma->desc.id,
            vma->generation, &publish_lockset, 1);
        if (result != UHM_OK) {
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, result);
        }
        if (left_vma) {
            size_t left_first;
            size_t left_count;

            if (!uhm_mprotect_publish_subspan(
                    heap, publish_start, publish_first, publish_count,
                    left_vma->addr, left_vma->size, &left_first,
                    &left_count)) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
            }
            result = uhm_page_span_mark_range_locked_prepared(
                heap, left_first, left_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, old_commit_state, old_prot,
                left_vma->desc.id, left_vma->generation, &publish_lockset, 1);
            if (result != UHM_OK) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, result);
            }
        }
        if (right_vma) {
            size_t right_first;
            size_t right_count;

            if (!uhm_mprotect_publish_subspan(
                    heap, publish_start, publish_first, publish_count,
                    right_vma->addr, right_vma->size, &right_first,
                    &right_count)) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
            }
            result = uhm_page_span_mark_range_locked_prepared(
                heap, right_first, right_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, old_commit_state, old_prot,
                right_vma->desc.id, right_vma->generation, &publish_lockset,
                1);
            if (result != UHM_OK) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, result);
            }
            uhm_vma_insert_after(heap, anchor, right_vma);
        }
        result = uhm_vma_merge_around_with_lockset(heap, &vma,
                                                   &publish_lockset);
        if (result != UHM_OK) {
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, result);
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
        tx.published = 1u;
        tx.publish_generation = vma->generation;
    } else {
        uhm_tx_stage(&tx, UHM_TX_STAGE_ROLLBACK);
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mprotect_rollback_calls));
        if (platform_revalidate_failed) {
            (void)uhm_bump_failure_generation(heap);
        }
        {
            uhm_lockset restore_lockset;
            uhm_page_lock_shard_range(heap, target_first, target_count,
                                      &restore_lockset);
            (void)uhm_page_span_mark_range_locked_prepared(
                heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, old_commit_state, old_prot,
                vma->desc.id, vma->generation, &restore_lockset, 0);
            uhm_page_unlock_shards(heap, &restore_lockset);
        }
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        tx.rolled_back = 1u;
    }
    uhm_unlock(&heap->lock);
    return uhm_tx_finish(&tx, result);
}

static uhm_result UHM_CALL uhm_api_mprotect(uhm_heap *heap, void *addr_ptr,
                                            size_t size, uint32_t prot) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_mprotect_pinned(heap, addr_ptr, size, prot);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

