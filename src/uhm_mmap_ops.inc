static uhm_result uhm_mmap_publish(uhm_heap *heap, uintptr_t addr, size_t size,
                                   uint32_t prot, uint32_t map_flags,
                                   uint32_t commit_state, void **addr_inout) {
    uhm_vma *vma = uhm_vma_new(heap, addr, size, prot, map_flags, 0);
    size_t first;
    size_t count;
    uhm_lockset lockset;
    uhm_result result;

    if (!vma) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        uhm_vma_release_node(heap, vma);
        return UHM_ERR_INVALID_ARGUMENT;
    }

    /*
     * This is the first publish path converted to the eventual shard order:
     * page-map shards are prelocked before the VMA becomes visible. VMA node
     * allocation stays outside the page-map lock because control allocation can
     * itself publish control pages.
     */
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    uhm_vma_lock_all_shards(heap, &lockset);
    result = uhm_page_span_mark_range_locked_prepared(
        heap, first, count, UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
        commit_state, prot, vma->desc.id, vma->generation, &lockset, 0);
    if (result != UHM_OK) {
        uhm_metadata_unlock_shards(heap, &lockset);
        uhm_page_unlock_shards(heap, &lockset);
        uhm_vma_release_node(heap, vma);
        return result;
    }
    uhm_vma_insert_head(heap, vma);
    uhm_metadata_unlock_shards(heap, &lockset);
    uhm_page_unlock_shards(heap, &lockset);
    heap->stats.mmap_live_bytes += size;
    *addr_inout = (void *)addr;
    return UHM_OK;
}

static int uhm_mmap_collect_replace_range(uhm_heap *heap, uintptr_t addr,
                                          size_t size, uhm_vma **first_out,
                                          uhm_vma **last_out,
                                          size_t *old_live_bytes_out,
                                          const uhm_lockset *lockset) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *first = 0;
    uhm_vma *vma = 0;
    uhm_vma *last = 0;
    size_t old_live_bytes = 0;

    if (uhm_range_overflows(addr, size, &end)) {
        return 0;
    }
    first = uhm_find_vma_covering(heap, addr, 1u);
    if (!first) {
        return 0;
    }
    cursor = addr;
    vma = first;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        size_t seg_size;
        uint32_t commit_state;

        if (!vma || vma->magic != UHM_VMA_MAGIC ||
            uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
            return 0;
        }
        if (cursor < vma->addr || vma_end <= cursor) {
            return 0;
        }
        seg_end = vma_end < end ? vma_end : end;
        seg_size = (size_t)(seg_end - cursor);
        commit_state = uhm_vma_commit_state_from_flags(vma->flags);
        if (lockset) {
            if (!uhm_page_range_matches_descriptor_locked(
                    heap, cursor, seg_size, UHM_PAGE_OWNER_MMAP,
                    UHM_PAGE_VA_MMAP_LIVE, commit_state, vma->prot,
                    &vma->desc, 1, 1, lockset)) {
                return 0;
            }
        } else if (!uhm_page_range_matches_descriptor(
                       heap, cursor, seg_size, UHM_PAGE_OWNER_MMAP,
                       UHM_PAGE_VA_MMAP_LIVE, commit_state, vma->prot,
                       &vma->desc, 1, 1)) {
            return 0;
        }
        last = vma;
        if (commit_state == UHM_PAGE_COMMIT_LIVE) {
            old_live_bytes += seg_size;
        }
        cursor = seg_end;
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }
    if (first_out) {
        *first_out = first;
    }
    if (last_out) {
        *last_out = last;
    }
    if (old_live_bytes_out) {
        *old_live_bytes_out = old_live_bytes;
    }
    return first != 0 && last != 0;
}

static int uhm_mmap_collect_exact_range(uhm_heap *heap, uintptr_t addr,
                                        size_t size, uhm_vma **first_out,
                                        uhm_vma **last_out) {
    uintptr_t end;
    uintptr_t last_end;
    uhm_vma *first = 0;
    uhm_vma *last = 0;

    if (uhm_range_overflows(addr, size, &end) ||
        !uhm_mmap_collect_replace_range(heap, addr, size, &first, &last, 0,
                                        0) ||
        !first || !last || first->addr != addr ||
        uhm_range_overflows(last->addr, last->size, &last_end) ||
        last_end != end) {
        return 0;
    }
    if (first_out) {
        *first_out = first;
    }
    if (last_out) {
        *last_out = last;
    }
    return 1;
}

static int uhm_mmap_recheck_exact_range_from_first(
    uhm_heap *heap, uintptr_t addr, size_t size, uhm_vma *expected_first,
    uhm_vma *expected_last) {
    uintptr_t end;
    uintptr_t cursor;
    uintptr_t last_end = 0;
    uhm_vma *vma;
    uhm_vma *last = 0;

    if (!heap || !expected_first || !expected_last ||
        uhm_range_overflows(addr, size, &end) ||
        expected_first->magic != UHM_VMA_MAGIC ||
        expected_first->addr != addr) {
        return 0;
    }

    cursor = addr;
    vma = expected_first;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        size_t seg_size;
        uint32_t commit_state;

        if (!vma || vma->magic != UHM_VMA_MAGIC ||
            uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
            cursor < vma->addr || cursor >= vma_end) {
            return 0;
        }
        seg_end = vma_end < end ? vma_end : end;
        seg_size = (size_t)(seg_end - cursor);
        commit_state = uhm_vma_commit_state_from_flags(vma->flags);
        if (!uhm_page_range_matches_descriptor(
                heap, cursor, seg_size, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, commit_state, vma->prot,
                &vma->desc, 1, 1)) {
            return 0;
        }
        last = vma;
        last_end = vma_end;
        cursor = seg_end;
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }

    return last == expected_last && last_end == end;
}

static int uhm_mmap_collected_vma_chain_covers(uhm_vma *first,
                                               uhm_vma *last,
                                               uintptr_t addr,
                                               size_t size) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *vma;

    if (!first || !last || uhm_range_overflows(addr, size, &end)) {
        return 0;
    }
    cursor = addr;
    vma = first;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;

        if (!vma || vma->magic != UHM_VMA_MAGIC ||
            uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
            cursor < vma->addr || vma_end <= cursor) {
            return 0;
        }
        seg_end = vma_end < end ? vma_end : end;
        cursor = seg_end;
        if (vma == last) {
            return cursor == end;
        }
        vma = uhm_vma_tree_successor(vma);
    }
    return 0;
}

static uhm_result uhm_mmap_remove_collected_vma_range(
    uhm_heap *heap, uhm_vma *first, uhm_vma *last,
    uintptr_t addr, size_t size) {
    uhm_vma *vma;

    if (!uhm_mmap_collected_vma_chain_covers(first, last, addr, size)) {
        return UHM_ERR_CORRUPTION;
    }

    vma = first;
    for (;;) {
        uhm_vma *next = vma == last ? 0 : uhm_vma_tree_successor(vma);

        uhm_vma_remove(heap, vma);
        if (!next) {
            break;
        }
        vma = next;
    }
    return UHM_OK;
}

static uhm_result uhm_mmap_restore_replace_range_pages_from_first(
    uhm_heap *heap, uintptr_t addr, size_t size, uhm_vma *first_vma,
    const uhm_lockset *lockset) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *vma;

    if (uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        return UHM_OK;
    }
    if (lockset) {
        uhm_result ready_result =
            uhm_page_mark_range_ready_locked(heap, addr, size, lockset);
        if (ready_result != UHM_OK) {
            return ready_result;
        }
    }
    vma = first_vma;
    if (!vma) {
        return UHM_ERR_CORRUPTION;
    }
    cursor = addr;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        uhm_result result;

        if (!vma || vma->magic != UHM_VMA_MAGIC ||
            uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
            return UHM_ERR_CORRUPTION;
        }
        if (cursor < vma->addr || vma_end <= cursor) {
            return UHM_ERR_CORRUPTION;
        }
        seg_end = vma_end < end ? vma_end : end;
        if (lockset) {
            result = uhm_page_mark_range_locked_prepared(
                heap, cursor, (size_t)(seg_end - cursor),
                UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
                uhm_vma_commit_state_from_flags(vma->flags), vma->prot,
                vma->desc.id, vma->generation, lockset, 1);
        } else {
            result = uhm_page_mark_range(
                heap, cursor, (size_t)(seg_end - cursor),
                UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
                uhm_vma_commit_state_from_flags(vma->flags), vma->prot,
                vma->desc.id, vma->generation, 0);
        }
        if (result != UHM_OK) {
            return result;
        }
        cursor = seg_end;
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }
    return UHM_OK;
}

static uhm_result uhm_mmap_mark_vma_range_pages_from_first(
    uhm_heap *heap, uintptr_t addr, size_t size, uhm_vma *first_vma,
    uint32_t va_state, uint64_t generation,
    const uhm_lockset *lockset) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *vma;

    if (uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        return UHM_OK;
    }
    if (lockset) {
        uhm_result ready_result =
            uhm_page_mark_range_ready_locked(heap, addr, size, lockset);
        if (ready_result != UHM_OK) {
            return ready_result;
        }
    }
    vma = first_vma;
    if (!vma) {
        return UHM_ERR_CORRUPTION;
    }
    cursor = addr;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        size_t seg_size;
        uhm_result result;

        if (!vma || vma->magic != UHM_VMA_MAGIC ||
            uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
            cursor < vma->addr || vma_end <= cursor) {
            return UHM_ERR_CORRUPTION;
        }
        seg_end = vma_end < end ? vma_end : end;
        seg_size = (size_t)(seg_end - cursor);
        if (lockset) {
            result = uhm_page_mark_range_locked_prepared(
                heap, cursor, seg_size, UHM_PAGE_OWNER_MMAP, va_state,
                uhm_vma_commit_state_from_flags(vma->flags), vma->prot,
                vma->desc.id, generation, lockset, 1);
        } else {
            result = uhm_page_mark_range(
                heap, cursor, seg_size, UHM_PAGE_OWNER_MMAP, va_state,
                uhm_vma_commit_state_from_flags(vma->flags), vma->prot,
                vma->desc.id, generation, 0);
        }
        if (result != UHM_OK) {
            return result;
        }
        cursor = seg_end;
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }
    return UHM_OK;
}

static void uhm_mmap_recommit_old_live_segments_unlocked_preserve_error(
    uhm_heap *heap, uintptr_t addr, size_t size, uhm_vma *first_vma,
    uhm_vma *last_vma, uint64_t protecting_generation_id) {
    uintptr_t end;
    uintptr_t cursor;
    uhm_vma *vma;
    uint32_t saved_platform_error;

    if (!heap || size == 0 || !first_vma || !last_vma ||
        uhm_range_overflows(addr, size, &end)) {
        return;
    }

    saved_platform_error =
        atomic_load_explicit(&heap->platform_error, UHM_ATOMIC_ACQUIRE);
    cursor = addr;
    vma = first_vma;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        size_t seg_size;
        uint32_t commit_state;
        uint32_t seg_prot;
        uhm_vm_call_record record;
        uhm_result publish_result;

        if (!vma || vma->magic != UHM_VMA_MAGIC ||
            uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
            cursor < vma->addr || vma_end <= cursor) {
            break;
        }
        seg_end = vma_end < end ? vma_end : end;
        seg_size = (size_t)(seg_end - cursor);
        commit_state = uhm_vma_commit_state_from_flags(vma->flags);
        seg_prot = vma->prot;
        if (commit_state == UHM_PAGE_COMMIT_LIVE) {
            if (!uhm_page_range_matches_owner_va_generation(
                    heap, cursor, seg_size, UHM_PAGE_OWNER_MMAP,
                    UHM_PAGE_VA_PROTECTING, protecting_generation_id)) {
                break;
            }
            uhm_unlock(&heap->lock);
            record = uhm_vm_commit_unlocked_call(heap, (void *)cursor,
                                                 seg_size, seg_prot);
            uhm_lock(&heap->lock);
            publish_result =
                uhm_vm_commit_publish_record_locked(heap, &record);
            if (publish_result != UHM_OK || record.result != UHM_OK ||
                !uhm_heap_is_accessible_pinned(heap) ||
                vma->magic != UHM_VMA_MAGIC ||
                !uhm_page_range_matches_owner_va_generation(
                    heap, cursor, seg_size, UHM_PAGE_OWNER_MMAP,
                    UHM_PAGE_VA_PROTECTING, protecting_generation_id)) {
                break;
            }
        }
        cursor = seg_end;
        if (vma == last_vma) {
            break;
        }
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }
    atomic_store_explicit(&heap->platform_error, saved_platform_error,
                          UHM_ATOMIC_RELEASE);
}

static uhm_result uhm_mmap_fixed_replace(uhm_heap *heap, uintptr_t addr, size_t size,
                                         uint32_t prot, uint32_t map_flags,
                                         uint32_t numa_policy,
                                         int32_t numa_node,
                                         void **addr_inout) {
    uintptr_t end;
    uhm_tx tx;
    uhm_vma *first_vma = 0;
    uhm_vma *last_vma = 0;
    uhm_vma *replacement_vma = 0;
    uhm_vma *left_vma = 0;
    uhm_vma *right_vma = 0;
    uintptr_t first_start;
    uintptr_t last_end;
    uint32_t first_flags;
    uint32_t last_flags;
    uint32_t first_commit_state;
    uint32_t last_commit_state;
    uint32_t new_commit_state;
    uint64_t protecting_generation_id;
    size_t old_live_bytes = 0;
    size_t old_live_bytes_recheck = 0;
    size_t target_first = 0;
    size_t target_count = 0;
    uint32_t old_prot = UHM_PROT_NONE;
    uint32_t old_flags = 0;
    uint64_t old_descriptor_id = 0;
    uint64_t old_generation = 0;
    int fixed_exact_unlock = 0;
    uhm_result result;

    if (uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_tx_begin(&tx, heap, UHM_TX_OP_MMAP_FIXED, addr, size, prot, map_flags);
    uhm_tx_stage(&tx, UHM_TX_STAGE_VALIDATE);
    if (uhm_page_range_overlaps_va_state(heap, addr, size, UHM_PAGE_VA_PROTECTING)) {
        return UHM_ERR_PROTECTION;
    }
    if (!uhm_mmap_collect_replace_range(heap, addr, size, &first_vma,
                                        &last_vma, &old_live_bytes, 0)) {
        return UHM_ERR_PROTECTION;
    }
    if (uhm_vma_range_list_overlaps_protecting(heap, first_vma, last_vma)) {
        return UHM_ERR_PROTECTION;
    }
    uhm_tx_record_old_mmap(&tx, first_vma,
                           uhm_vma_commit_state_from_flags(first_vma->flags),
                           first_vma->prot);

    first_start = first_vma->addr;
    if (uhm_range_overflows(last_vma->addr, last_vma->size, &last_end)) {
        return UHM_ERR_CORRUPTION;
    }
    first_flags = first_vma->flags & ~UHM_META_FLAG_EMERGENCY;
    last_flags = last_vma->flags & ~UHM_META_FLAG_EMERGENCY;
    first_commit_state = uhm_vma_commit_state_from_flags(first_vma->flags);
    last_commit_state = uhm_vma_commit_state_from_flags(last_vma->flags);
    old_prot = first_vma->prot;
    old_flags = first_flags;
    old_descriptor_id = first_vma->desc.id;
    old_generation = first_vma->generation;

    if (addr > first_start) {
        left_vma = uhm_vma_new(heap, first_start, addr - first_start,
                               first_vma->prot, first_flags, 0);
        if (!left_vma) {
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_OUT_OF_MEMORY;
        }
    }
    if (end < last_end) {
        right_vma = uhm_vma_new(heap, end, last_end - end,
                                last_vma->prot, last_flags, 0);
        if (!right_vma) {
            if (left_vma) {
                uhm_vma_release_node(heap, left_vma);
            }
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_OUT_OF_MEMORY;
        }
    }
    new_commit_state = (map_flags & UHM_MAP_NO_COMMIT) != 0
                           ? UHM_PAGE_COMMIT_DECOMMITTED
                           : UHM_PAGE_COMMIT_LIVE;
    replacement_vma = uhm_vma_new(heap, addr, size, prot, map_flags, 0);
    if (!replacement_vma) {
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        return UHM_ERR_OUT_OF_MEMORY;
    }
    fixed_exact_unlock = first_vma == last_vma && left_vma == 0 &&
                         right_vma == 0 && addr == first_start &&
                         end == last_end;

    uhm_tx_stage(&tx, UHM_TX_STAGE_PREPARED);
    if (!uhm_page_span(heap, addr, size, &target_first, &target_count)) {
        uhm_vma_release_node(heap, replacement_vma);
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        return UHM_ERR_CORRUPTION;
    }
    protecting_generation_id = uhm_heap_generation_next(heap);
    {
        uhm_vma *first_recheck = 0;
        uhm_vma *last_recheck = 0;
        uhm_lockset mark_lockset;
        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &mark_lockset);
        if (!uhm_mmap_collect_replace_range(heap, addr, size, &first_recheck,
                                              &last_recheck,
                                              &old_live_bytes_recheck,
                                              &mark_lockset) ||
            first_recheck != first_vma || last_recheck != last_vma ||
            old_live_bytes_recheck != old_live_bytes) {
            result = UHM_ERR_PROTECTION;
        } else if (fixed_exact_unlock) {
            result = uhm_page_span_mark_range_locked_prepared(
                heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_PROTECTING, first_commit_state, old_prot,
                old_descriptor_id, protecting_generation_id, &mark_lockset, 0);
        } else {
            result = uhm_page_span_mark_range_locked_prepared(
                heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_PROTECTING, UHM_PAGE_COMMIT_NONE, UHM_PROT_NONE,
                0, protecting_generation_id, &mark_lockset, 0);
        }
        uhm_page_unlock_shards(heap, &mark_lockset);
    }
    if (result != UHM_OK) {
        uhm_vma_release_node(heap, replacement_vma);
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        return result;
    }
    if (fixed_exact_unlock
            ? !uhm_page_range_matches_state_generation_id(
                  heap, addr, size, UHM_PAGE_OWNER_MMAP,
                  UHM_PAGE_VA_PROTECTING, first_commit_state, old_prot,
                  old_descriptor_id, protecting_generation_id)
            : !uhm_page_range_matches_owner_va_generation(
                  heap, addr, size, UHM_PAGE_OWNER_MMAP,
                  UHM_PAGE_VA_PROTECTING, protecting_generation_id)) {
        uhm_lockset restore_lockset;
        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &restore_lockset);
        (void)uhm_mmap_restore_replace_range_pages_from_first(
            heap, addr, size, first_vma, &restore_lockset);
        uhm_page_unlock_shards(heap, &restore_lockset);
        uhm_vma_release_node(heap, replacement_vma);
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        return UHM_ERR_CORRUPTION;
    }

    tx.new_commit_state = new_commit_state;
    tx.left_vma = left_vma;
    tx.right_vma = right_vma;

    if (fixed_exact_unlock) {
        uhm_vm_call_record decommit_record;
        uhm_vm_call_record commit_record;
        uhm_vm_call_record numa_record;

        memset(&decommit_record, 0, sizeof(decommit_record));
        memset(&commit_record, 0, sizeof(commit_record));
        memset(&numa_record, 0, sizeof(numa_record));
        decommit_record.result = UHM_OK;
        commit_record.result = UHM_OK;
        numa_record.result = UHM_OK;
        uhm_tx_stage(&tx, UHM_TX_STAGE_PLATFORM);
        uhm_unlock(&heap->lock);
        if (old_live_bytes != 0) {
            decommit_record =
                uhm_vm_decommit_unlocked_call(heap, (void *)addr, size);
        }
        if (decommit_record.result == UHM_OK &&
            new_commit_state == UHM_PAGE_COMMIT_LIVE) {
            commit_record =
                uhm_vm_commit_unlocked_call(heap, (void *)addr, size, prot);
            if (commit_record.result == UHM_OK) {
                numa_record = uhm_vm_numa_hint_unlocked_call(
                    heap, (void *)addr, size, numa_policy, numa_node,
                    map_flags);
            }
        }
        uhm_lock(&heap->lock);
        result = uhm_vm_decommit_publish_record_locked(heap,
                                                       &decommit_record);
        if (result == UHM_OK) {
            result = uhm_vm_commit_publish_record_locked(heap,
                                                        &commit_record);
        }
        if (result == UHM_OK) {
            result = uhm_vm_numa_hint_publish_record_locked(heap,
                                                           &numa_record);
        }
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             first_vma->magic != UHM_VMA_MAGIC ||
             first_vma->addr != addr || first_vma->size != size ||
             first_vma->prot != old_prot ||
             (first_vma->flags & ~UHM_META_FLAG_EMERGENCY) != old_flags ||
             first_vma->desc.id != old_descriptor_id ||
             first_vma->generation != old_generation ||
             !uhm_page_range_matches_state_generation_id(
                 heap, addr, size, UHM_PAGE_OWNER_MMAP,
                 UHM_PAGE_VA_PROTECTING, first_commit_state, old_prot,
                 old_descriptor_id, protecting_generation_id))) {
            result = UHM_ERR_CORRUPTION;
        }
        if (result != UHM_OK) {
            uhm_tx_stage(&tx, UHM_TX_STAGE_ROLLBACK);
            if (new_commit_state == UHM_PAGE_COMMIT_LIVE &&
                commit_record.did_call) {
                uhm_vm_decommit_rollback_unlocked_preserve_error(
                    heap, (void *)addr, size);
            }
            if (old_live_bytes != 0 && decommit_record.result == UHM_OK) {
                uhm_mmap_recommit_old_live_segments_unlocked_preserve_error(
                    heap, addr, size, first_vma, last_vma,
                    protecting_generation_id);
            }
            {
                uhm_lockset restore_lockset;
                uhm_page_lock_shard_range(heap, target_first, target_count,
                                          &restore_lockset);
                (void)uhm_mmap_restore_replace_range_pages_from_first(
                    heap, addr, size, first_vma, &restore_lockset);
                uhm_page_unlock_shards(heap, &restore_lockset);
            }
            uhm_vma_release_node(heap, replacement_vma);
            tx.rolled_back = 1u;
            if (result == UHM_ERR_CORRUPTION) {
                (void)uhm_bump_failure_generation(heap);
            }
            return uhm_tx_finish(&tx, result);
        }
        tx.platform_done = 1u;
    } else {
        uhm_vm_call_record decommit_record;
        uhm_vm_call_record commit_record;
        uhm_vm_call_record numa_record;

        memset(&decommit_record, 0, sizeof(decommit_record));
        memset(&commit_record, 0, sizeof(commit_record));
        memset(&numa_record, 0, sizeof(numa_record));
        decommit_record.result = UHM_OK;
        commit_record.result = UHM_OK;
        numa_record.result = UHM_OK;

        uhm_tx_stage(&tx, UHM_TX_STAGE_PLATFORM);
        uhm_unlock(&heap->lock);
        if (old_live_bytes != 0) {
            decommit_record =
                uhm_vm_decommit_unlocked_call(heap, (void *)addr, size);
        }
        if (decommit_record.result == UHM_OK &&
            new_commit_state == UHM_PAGE_COMMIT_LIVE) {
            commit_record =
                uhm_vm_commit_unlocked_call(heap, (void *)addr, size, prot);
            if (commit_record.result == UHM_OK) {
                numa_record = uhm_vm_numa_hint_unlocked_call(
                    heap, (void *)addr, size, numa_policy, numa_node,
                    map_flags);
            }
        }
        uhm_lock(&heap->lock);
        result = uhm_vm_decommit_publish_record_locked(heap,
                                                       &decommit_record);
        if (result == UHM_OK) {
            result = uhm_vm_commit_publish_record_locked(heap,
                                                        &commit_record);
        }
        if (result == UHM_OK) {
            result = uhm_vm_numa_hint_publish_record_locked(heap,
                                                           &numa_record);
        }
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             !uhm_mmap_collected_vma_chain_covers(first_vma, last_vma,
                                                  addr, size) ||
             !uhm_page_range_matches_owner_va_generation(
                 heap, addr, size, UHM_PAGE_OWNER_MMAP,
                 UHM_PAGE_VA_PROTECTING, protecting_generation_id))) {
            result = UHM_ERR_CORRUPTION;
        }
        if (result != UHM_OK) {
            uhm_tx_stage(&tx, UHM_TX_STAGE_ROLLBACK);
            if (new_commit_state == UHM_PAGE_COMMIT_LIVE &&
                commit_record.did_call) {
                uhm_vm_decommit_rollback_unlocked_preserve_error(
                    heap, (void *)addr, size);
            }
            if (old_live_bytes != 0 && decommit_record.result == UHM_OK) {
                uhm_mmap_recommit_old_live_segments_unlocked_preserve_error(
                    heap, addr, size, first_vma, last_vma,
                    protecting_generation_id);
            }
            {
                uhm_lockset restore_lockset;
                uhm_page_lock_shard_range(heap, target_first, target_count,
                                          &restore_lockset);
                (void)uhm_mmap_restore_replace_range_pages_from_first(
                    heap, addr, size, first_vma, &restore_lockset);
                uhm_page_unlock_shards(heap, &restore_lockset);
            }
            uhm_vma_release_node(heap, replacement_vma);
            if (left_vma) {
                uhm_vma_release_node(heap, left_vma);
            }
            if (right_vma) {
                uhm_vma_release_node(heap, right_vma);
            }
            if (result == UHM_ERR_CORRUPTION) {
                (void)uhm_bump_failure_generation(heap);
            }
            tx.rolled_back = 1u;
            return uhm_tx_finish(&tx, result);
        }
        tx.platform_done = 1u;
    }

    uhm_tx_stage(&tx, UHM_TX_STAGE_PUBLISH);
    {
        size_t publish_first;
        size_t publish_count;
        size_t publish_limit;
        size_t target_limit;
        size_t left_count;
        size_t right_count;
        uhm_lockset publish_lockset;

        if (!uhm_page_span(heap, first_start, (size_t)(last_end - first_start),
                           &publish_first, &publish_count)) {
            result = UHM_ERR_CORRUPTION;
            goto fixed_replace_publish_fail;
        }
        publish_limit = publish_first + publish_count;
        target_limit = target_first + target_count;
        if (target_first < publish_first || target_limit > publish_limit) {
            result = UHM_ERR_CORRUPTION;
            goto fixed_replace_publish_fail;
        }
        left_count = target_first - publish_first;
        right_count = publish_limit - target_limit;
        if (heap->F.vm.page_size == 0 ||
            (left_vma == 0) != (left_count == 0) ||
            (right_vma == 0) != (right_count == 0) ||
            (left_vma &&
             (left_vma->size % heap->F.vm.page_size != 0 ||
              left_vma->size / heap->F.vm.page_size != left_count)) ||
            (right_vma &&
             (right_vma->size % heap->F.vm.page_size != 0 ||
              right_vma->size / heap->F.vm.page_size != right_count))) {
            result = UHM_ERR_CORRUPTION;
            goto fixed_replace_publish_fail;
        }
        uhm_page_lock_shard_range(heap, publish_first, publish_count,
                                  &publish_lockset);
        uhm_vma_lock_all_shards(heap, &publish_lockset);
        if (left_vma) {
            result = uhm_page_span_mark_range_locked_prepared(
                heap, publish_first, left_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, first_commit_state, left_vma->prot,
                left_vma->desc.id, left_vma->generation, &publish_lockset, 0);
            if (result != UHM_OK) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                goto fixed_replace_publish_fail;
            }
        }
        result = uhm_page_span_mark_range_locked_prepared(
            heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, new_commit_state, prot,
            replacement_vma->desc.id, replacement_vma->generation,
            &publish_lockset, 0);
        if (result != UHM_OK) {
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            goto fixed_replace_publish_fail;
        }
        if (right_vma) {
            result = uhm_page_span_mark_range_locked_prepared(
                heap, target_limit, right_count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, last_commit_state, right_vma->prot,
                right_vma->desc.id, right_vma->generation, &publish_lockset,
                0);
            if (result != UHM_OK) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                goto fixed_replace_publish_fail;
            }
        }

        result = uhm_mmap_remove_collected_vma_range(
            heap, first_vma, last_vma, addr, size);
        if (result != UHM_OK) {
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            goto fixed_replace_publish_fail;
        }
        if (left_vma) {
            uhm_vma_insert_head(heap, left_vma);
            left_vma = 0;
        }
        uhm_vma_insert_head(heap, replacement_vma);
        replacement_vma = 0;
        if (right_vma) {
            uhm_vma_insert_head(heap, right_vma);
            right_vma = 0;
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    *addr_inout = (void *)addr;
    if (new_commit_state == UHM_PAGE_COMMIT_LIVE) {
        heap->stats.committed_bytes += size;
    }
    if (old_live_bytes != 0) {
        heap->stats.decommitted_bytes += old_live_bytes;
        uhm_vm_account_decommit(heap, addr, old_live_bytes);
    }
    tx.published = 1u;
    tx.publish_generation = uhm_heap_generation_load(heap);
    return uhm_tx_finish(&tx, UHM_OK);

fixed_replace_publish_fail:
    uhm_tx_stage(&tx, UHM_TX_STAGE_ROLLBACK);
    if (new_commit_state == UHM_PAGE_COMMIT_LIVE) {
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)addr, size);
    }
    if (old_live_bytes != 0) {
        uhm_mmap_recommit_old_live_segments_unlocked_preserve_error(
            heap, addr, size, first_vma, last_vma,
            protecting_generation_id);
    }
    {
        size_t restore_first;
        size_t restore_count;
        uhm_lockset restore_lockset;

        if (uhm_page_span(heap, first_start, (size_t)(last_end - first_start),
                          &restore_first, &restore_count)) {
            uhm_page_lock_shard_range(heap, restore_first, restore_count,
                                      &restore_lockset);
            (void)uhm_mmap_restore_replace_range_pages_from_first(
                heap, first_start, (size_t)(last_end - first_start),
                first_vma, &restore_lockset);
            uhm_page_unlock_shards(heap, &restore_lockset);
        }
    }
    uhm_vma_release_node(heap, replacement_vma);
    if (left_vma) {
        uhm_vma_release_node(heap, left_vma);
    }
    if (right_vma) {
        uhm_vma_release_node(heap, right_vma);
    }
    (void)uhm_bump_failure_generation(heap);
    tx.rolled_back = 1u;
    return uhm_tx_finish(&tx, result);
}

static uhm_result uhm_mmap_pinned(uhm_heap *heap, void **addr_inout,
                                  size_t size, uint32_t prot,
                                  uint32_t map_flags,
                                  uint32_t numa_policy,
                                  int32_t numa_node,
                                  size_t *out_mapped_size) {
    size_t rounded;
    uintptr_t addr = 0;
    uintptr_t end = 0;
    uhm_tx tx;
    uhm_range_claim claim;
    size_t target_first = 0;
    size_t target_count = 0;
    uint64_t protecting_generation_id = 0;
    int platform_unlocked = 0;
    uhm_result result;

    if (!uhm_heap_is_accessible_pinned(heap) || !addr_inout) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_align_up_size(size, heap->F.vm.page_size, &rounded) || rounded == 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    result = uhm_vm_validate_prot(heap, prot);
    if (result != UHM_OK) {
        return result;
    }
    numa_node = uhm_numa_effective_node(heap, numa_policy, numa_node);
    result = uhm_numa_validate_request(heap, numa_policy, numa_node, map_flags);
    if (result != UHM_OK) {
        return result;
    }

    uhm_tx_begin(&tx, heap,
                 (map_flags & (UHM_MAP_FIXED | UHM_MAP_FIXED_NOREPLACE)) != 0
                     ? UHM_TX_OP_MMAP_FIXED
                     : UHM_TX_OP_MMAP_AUTO,
                 (uintptr_t)*addr_inout, rounded, prot, map_flags);
    uhm_tx_stage(&tx, UHM_TX_STAGE_VALIDATE);

    uhm_lock(&heap->lock);
    uhm_claim_reset(&claim);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_INVALID_ARGUMENT);
    }
    uhm_metadata_epoch_advance(heap);

    if ((map_flags & (UHM_MAP_FIXED | UHM_MAP_FIXED_NOREPLACE)) != 0) {
        addr = (uintptr_t)*addr_inout;
        if (addr == 0 || (addr % heap->F.vm.page_size) != 0 ||
            uhm_range_overflows(addr, rounded, &end) ||
            addr < heap->core.control_end || end > heap->core.arena_end ||
            addr < heap->core.malloc_hi) {
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, (map_flags & UHM_MAP_FIXED_NOREPLACE)
                                          ? UHM_ERR_ALREADY_MAPPED
                                          : UHM_ERR_PROTECTION);
        }
        if (uhm_page_range_overlaps_va_state(heap, addr, rounded,
                                             UHM_PAGE_VA_PROTECTING)) {
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
        }

        if ((map_flags & UHM_MAP_FIXED_NOREPLACE) == 0 &&
            uhm_page_range_has_live_mmap(heap, addr, rounded)) {
            result = uhm_mmap_fixed_replace(heap, addr, rounded, prot, map_flags,
                                            numa_policy, numa_node, addr_inout);
            if (result == UHM_OK) {
                if (out_mapped_size) {
                    *out_mapped_size = rounded;
                }
                uhm_tx_stage(&tx, UHM_TX_STAGE_PREPARED);
                uhm_tx_stage(&tx, UHM_TX_STAGE_PLATFORM);
                uhm_tx_stage(&tx, UHM_TX_STAGE_PUBLISH);
                tx.published = 1u;
                tx.publish_generation = uhm_heap_generation_load(heap);
            }
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, result);
        }

        if (uhm_find_vma_overlap(heap, addr, rounded) != 0 ||
            !uhm_page_range_is_free(heap, addr, rounded)) {
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, (map_flags & UHM_MAP_FIXED_NOREPLACE)
                                          ? UHM_ERR_ALREADY_MAPPED
                                          : UHM_ERR_PROTECTION);
        }
        result = uhm_claim_mmap_fixed(heap, addr, rounded, &claim);
        if (result != UHM_OK) {
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, result);
        }
        tx.range_claimed = 1u;
    } else {
        result = uhm_claim_mmap_auto(heap, rounded, &claim, &addr);
        if (result != UHM_OK) {
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, result);
        }
        tx.range_claimed = 1u;
    }

    if (!uhm_page_range_is_free(heap, addr, rounded)) {
        (void)uhm_claim_abort(heap, &claim, tx.range_claimed);
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
    }

    tx.addr = addr;
    tx.size = rounded;
    uhm_tx_stage(&tx, UHM_TX_STAGE_CLAIMED);
    result = uhm_page_prepare_range(heap, addr, rounded);
    if (result != UHM_OK) {
        (void)uhm_claim_abort(heap, &claim, 1);
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, result);
    }
    uhm_tx_stage(&tx, UHM_TX_STAGE_PREPARED);

    if ((map_flags & UHM_MAP_NO_COMMIT) == 0) {
        uhm_vm_call_record commit_record;
        uhm_vm_call_record numa_record;

        memset(&commit_record, 0, sizeof(commit_record));
        memset(&numa_record, 0, sizeof(numa_record));
        protecting_generation_id = uhm_heap_generation_next(heap);
        if (!uhm_page_span(heap, addr, rounded, &target_first,
                           &target_count)) {
            (void)uhm_claim_abort(heap, &claim, 1);
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
                UHM_PAGE_VA_PROTECTING, UHM_PAGE_COMMIT_NONE, UHM_PROT_NONE,
                0, protecting_generation_id, &mark_lockset, 0);
            uhm_page_unlock_shards(heap, &mark_lockset);
        }
        if (result != UHM_OK) {
            (void)uhm_claim_abort(heap, &claim, 1);
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, result);
        }
        platform_unlocked = 1;
        uhm_tx_stage(&tx, UHM_TX_STAGE_PLATFORM);
        uhm_unlock(&heap->lock);
        commit_record = uhm_vm_commit_unlocked_call(heap, (void *)addr,
                                                    rounded, prot);
        if (commit_record.result == UHM_OK) {
            numa_record = uhm_vm_numa_hint_unlocked_call(
                heap, (void *)addr, rounded, numa_policy, numa_node,
                map_flags);
        }
        uhm_lock(&heap->lock);
        result = uhm_vm_commit_publish_record_locked(heap, &commit_record);
        if (result == UHM_OK) {
            result = uhm_vm_numa_hint_publish_record_locked(heap,
                                                           &numa_record);
        }
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             uhm_find_vma_overlap(heap, addr, rounded) != 0 ||
             !uhm_page_range_matches_state_generation_id(
                 heap, addr, rounded, UHM_PAGE_OWNER_MMAP,
                 UHM_PAGE_VA_PROTECTING, UHM_PAGE_COMMIT_NONE,
                 UHM_PROT_NONE, 0, protecting_generation_id))) {
            result = UHM_ERR_CORRUPTION;
        }
        if (result != UHM_OK) {
            uhm_tx_stage(&tx, UHM_TX_STAGE_ROLLBACK);
            if (commit_record.result == UHM_OK || commit_record.did_call) {
                uhm_vm_decommit_rollback_unlocked_preserve_error(
                    heap, (void *)addr, rounded);
            }
            if (target_count != 0) {
                uhm_lockset rollback_lockset;

                uhm_page_lock_shard_range(heap, target_first, target_count,
                                          &rollback_lockset);
                (void)uhm_page_span_mark_range_locked_prepared(
                    heap, target_first, target_count, UHM_PAGE_OWNER_FREE,
                    UHM_PAGE_VA_FREE_REUSABLE, UHM_PAGE_COMMIT_DECOMMITTED,
                    UHM_PROT_NONE, 0, uhm_heap_generation_next(heap),
                    &rollback_lockset, 0);
                uhm_page_unlock_shards(heap, &rollback_lockset);
            }
            (void)uhm_claim_abort(heap, &claim, 1);
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            tx.rolled_back = 1u;
            return uhm_tx_finish(&tx, result);
        }
        tx.platform_done = 1u;
    }

    uhm_tx_stage(&tx, UHM_TX_STAGE_PUBLISH);
    tx.new_commit_state = (map_flags & UHM_MAP_NO_COMMIT) == 0
                              ? UHM_PAGE_COMMIT_LIVE
                              : UHM_PAGE_COMMIT_DECOMMITTED;
    result = uhm_mmap_publish(heap, addr, rounded, prot, map_flags,
                              tx.new_commit_state, addr_inout);
    if (result != UHM_OK) {
        uhm_tx_stage(&tx, UHM_TX_STAGE_ROLLBACK);
        if (platform_unlocked) {
            uhm_vm_decommit_rollback_unlocked_preserve_error(
                heap, (void *)addr, rounded);
        }
        if (target_count != 0) {
            uhm_lockset rollback_lockset;

            uhm_page_lock_shard_range(heap, target_first, target_count,
                                      &rollback_lockset);
            (void)uhm_page_span_mark_range_locked_prepared(
                heap, target_first, target_count, UHM_PAGE_OWNER_FREE,
                UHM_PAGE_VA_FREE_REUSABLE, UHM_PAGE_COMMIT_DECOMMITTED,
                UHM_PROT_NONE, 0, uhm_heap_generation_next(heap),
                &rollback_lockset, 0);
            uhm_page_unlock_shards(heap, &rollback_lockset);
        }
        (void)uhm_claim_abort(heap, &claim, 1);
        (void)uhm_bump_failure_generation(heap);
        tx.rolled_back = 1u;
    } else {
        uhm_claim_disarm(heap, &claim);
        if (out_mapped_size) {
            *out_mapped_size = rounded;
        }
        if (tx.new_commit_state == UHM_PAGE_COMMIT_LIVE) {
            heap->stats.committed_bytes += rounded;
        }
        tx.published = 1u;
        tx.publish_generation = uhm_heap_generation_load(heap);
    }
    uhm_unlock(&heap->lock);
    return uhm_tx_finish(&tx, result);
}

static uhm_result UHM_CALL uhm_api_mmap(uhm_heap *heap, void **addr_inout,
                                        size_t size, uint32_t prot,
                                        uint32_t map_flags) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_mmap_pinned(heap, addr_inout, size, prot, map_flags,
                             UHM_NUMA_POLICY_DEFAULT,
                             heap->core.preferred_numa_node, 0);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

static uhm_result UHM_CALL uhm_api_mmap_ex(uhm_heap *heap,
                                           uhm_mmap_options *options) {
    void *addr;
    uhm_result result;
    size_t op_shard;

    if (!options || options->size < sizeof(*options) ||
        options->abi_major != UHM_ABI_MAJOR || options->flags != 0u ||
        !uhm_numa_policy_is_valid(options->numa_policy)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_CORRUPTION;
    }
    addr = options->addr;
    result = uhm_mmap_pinned(heap, &addr, options->map_size, options->prot,
                             options->map_flags, options->numa_policy,
                             options->numa_node, &options->map_size);
    if (result == UHM_OK) {
        options->addr = addr;
    }
    uhm_heap_op_end(heap, op_shard);
    return result;
}

