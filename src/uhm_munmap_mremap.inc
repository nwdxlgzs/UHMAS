static void uhm_diag_note_munmap_vma_segments(uhm_heap *heap,
                                              size_t segments) {
#if UHMAS_ENABLE_DIAGNOSTICS
    if (heap) {
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, munmap_vma_max_segments),
            (uint64_t)segments);
    }
#else
    (void)heap;
    (void)segments;
#endif
}

static void uhm_diag_note_munmap_trim_repair_elided(uhm_heap *heap,
                                                    size_t size) {
#if UHMAS_ENABLE_DIAGNOSTICS
    size_t count;
    size_t page_size;

    if (!heap || size == 0 || heap->F.vm.page_size == 0) {
        return;
    }
    page_size = heap->F.vm.page_size;
    if ((size % page_size) != 0) {
        return;
    }
    count = size / page_size;
    if (count == 0) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, munmap_vma_trim_repair_elided));
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, munmap_vma_trim_repair_elided_pages),
        (uint64_t)count);
#else
    (void)heap;
    (void)size;
#endif
}

static int uhm_munmap_vma_segment_at(uhm_vma *vma, uintptr_t cursor,
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

static int uhm_munmap_walk_mmap_range(uintptr_t addr, size_t size,
                                      uhm_vma *first, uhm_vma **last_out,
                                      uintptr_t *publish_start_out,
                                      uintptr_t *publish_end_out,
                                      size_t *segment_count_out) {
    uintptr_t end;
    uintptr_t cursor;
    uintptr_t publish_start = 0;
    uintptr_t publish_end = 0;
    uhm_vma *vma;
    uhm_vma *last = 0;
    size_t segments = 0;

    if (!first || uhm_range_overflows(addr, size, &end)) {
        return 0;
    }

    cursor = addr;
    vma = first;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;

        if (!uhm_munmap_vma_segment_at(vma, cursor, end, &vma_end,
                                       &seg_end)) {
            return 0;
        }
        if (segments == 0u) {
            publish_start = vma->addr;
        }
        publish_end = vma_end;
        last = vma;
        segments++;
        cursor = seg_end;
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }

    if (last_out) {
        *last_out = last;
    }
    if (publish_start_out) {
        *publish_start_out = publish_start;
    }
    if (publish_end_out) {
        *publish_end_out = publish_end;
    }
    if (segment_count_out) {
        *segment_count_out = segments;
    }
    return first != 0 && last != 0 && segments != 0u;
}

static int uhm_munmap_collect_mmap_range(uhm_heap *heap, uintptr_t addr,
                                         size_t size, uhm_vma **first_out,
                                         uhm_vma **last_out,
                                         uintptr_t *publish_start_out,
                                         uintptr_t *publish_end_out,
                                         size_t *segment_count_out) {
    uintptr_t ignored_end;
    uhm_vma *first;
    size_t segments = 0;
    int ok;

    if (uhm_range_overflows(addr, size, &ignored_end)) {
        return 0;
    }

    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, munmap_vma_collect_queries));
    first = uhm_find_vma_covering(heap, addr, 1u);
    if (!first) {
        return 0;
    }

    ok = uhm_munmap_walk_mmap_range(
        addr, size, first, last_out, publish_start_out, publish_end_out,
        segment_count_out ? segment_count_out : &segments);
    if (ok) {
        if (first_out) {
            *first_out = first;
        }
        if (segment_count_out) {
            segments = *segment_count_out;
        }
        uhm_diag_note_munmap_vma_segments(heap, segments);
    }
    return ok;
}

static int uhm_munmap_recheck_collected_mmap_range(
    uintptr_t addr,
    size_t size,
    uhm_vma *first,
    uhm_vma *expected_last,
    uintptr_t expected_publish_start,
    uintptr_t expected_publish_end,
    size_t expected_segments) {
    uintptr_t publish_start = 0;
    uintptr_t publish_end = 0;
    uhm_vma *last = 0;
    size_t segments = 0;

    if (!expected_last || expected_segments == 0u ||
        !uhm_munmap_walk_mmap_range(addr, size, first, &last, &publish_start,
                                    &publish_end, &segments)) {
        return 0;
    }
    return last == expected_last &&
           publish_start == expected_publish_start &&
           publish_end == expected_publish_end &&
           segments == expected_segments;
}

#define UHM_MUNMAP_DECOMMIT_SEGMENT_MAX 64u

typedef struct uhm_munmap_decommit_segment {
    uintptr_t addr;
    size_t size;
    uint32_t prot;
} uhm_munmap_decommit_segment;

static uhm_result uhm_munmap_decommit_segment_push(
    uhm_munmap_decommit_segment *segments,
    size_t segment_capacity,
    size_t *segment_count,
    uintptr_t addr,
    size_t size,
    uint32_t prot,
    int *overflow_out) {
    if (size == 0) {
        return UHM_OK;
    }
    if (!segments || !segment_count || *segment_count >= segment_capacity) {
        if (overflow_out) {
            *overflow_out = 1;
        }
        return UHM_OK;
    }
    segments[*segment_count].addr = addr;
    segments[*segment_count].size = size;
    segments[*segment_count].prot = prot;
    ++(*segment_count);
    return UHM_OK;
}

static uhm_result uhm_munmap_collect_live_decommit_segments(
    uintptr_t addr,
    size_t size,
    uhm_vma *first,
    uhm_munmap_decommit_segment *segments,
    size_t segment_capacity,
    size_t *segment_count_out,
    size_t *live_bytes_out,
    int *overflow_out) {
    uintptr_t end;
    uintptr_t cursor;
    uintptr_t run_start = 0;
    size_t run_size = 0;
    uint32_t run_prot = UHM_PROT_NONE;
    uhm_vma *vma;
    int have_run = 0;
    size_t segment_count = 0;
    size_t live_bytes = 0;
    int overflow = 0;

    if (!first || uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_CORRUPTION;
    }
    cursor = addr;
    vma = first;
    while (cursor < end) {
        uintptr_t vma_end;
        uintptr_t seg_end;
        size_t seg_size;
        uint32_t commit_state;

        if (!uhm_munmap_vma_segment_at(vma, cursor, end, &vma_end,
                                       &seg_end)) {
            return UHM_ERR_CORRUPTION;
        }
        seg_size = (size_t)(seg_end - cursor);
        commit_state = uhm_vma_commit_state_from_flags(vma->flags);
        if (commit_state == UHM_PAGE_COMMIT_LIVE) {
            if (live_bytes > SIZE_MAX - seg_size) {
                return UHM_ERR_CORRUPTION;
            }
            live_bytes += seg_size;
            if (have_run && cursor == run_start + run_size &&
                vma->prot == run_prot) {
                if (run_size > SIZE_MAX - seg_size) {
                    return UHM_ERR_CORRUPTION;
                }
                run_size += seg_size;
            } else {
                if (have_run) {
                    (void)uhm_munmap_decommit_segment_push(
                        segments, segment_capacity, &segment_count, run_start,
                        run_size, run_prot, &overflow);
                }
                run_start = cursor;
                run_size = seg_size;
                run_prot = vma->prot;
                have_run = 1;
            }
        } else {
            if (have_run) {
                (void)uhm_munmap_decommit_segment_push(
                    segments, segment_capacity, &segment_count, run_start,
                    run_size, run_prot, &overflow);
            }
            have_run = 0;
            run_size = 0;
        }
        cursor = seg_end;
        if (cursor < end) {
            vma = uhm_vma_tree_successor(vma);
        }
    }

    if (have_run) {
        (void)uhm_munmap_decommit_segment_push(
            segments, segment_capacity, &segment_count, run_start, run_size,
            run_prot, &overflow);
    }
    if (overflow && segment_capacity != 0u) {
        segments[0].addr = addr;
        segments[0].size = size;
        segments[0].prot = first->prot;
        segment_count = 1u;
        overflow = 0;
    }
    if (segment_count_out) {
        *segment_count_out = segment_count;
    }
    if (live_bytes_out) {
        *live_bytes_out = live_bytes;
    }
    if (overflow_out) {
        *overflow_out = overflow;
    }
    return UHM_OK;
}

static uhm_result uhm_munmap_pinned(uhm_heap *heap, void *addr_ptr, size_t size) {
    uintptr_t addr = (uintptr_t)addr_ptr;
    uintptr_t end;
    uintptr_t cursor;
    size_t rounded;
    uhm_tx tx;
    uhm_vma *vma;
    uhm_vma *first_vma = 0;
    uhm_vma *last_vma = 0;
    uhm_vma *left_vma = 0;
    uhm_vma *right_vma = 0;
    uhm_free_extent *free_node = 0;
    uhm_free_extent *adjacent_free_left = 0;
    uhm_free_extent *adjacent_free_right = 0;
    uintptr_t vma_start;
    uintptr_t vma_end;
    uintptr_t last_vma_end = 0;
    uintptr_t publish_start = 0;
    uintptr_t publish_end = 0;
    size_t publish_first = 0;
    size_t publish_count = 0;
    size_t publish_end_page = 0;
    size_t target_end_page = 0;
    size_t left_remnant_first = 0;
    size_t left_remnant_count = 0;
    size_t right_remnant_first = 0;
    size_t right_remnant_count = 0;
    size_t segment_count = 0;
    uhm_lockset publish_lockset;
    int publish_leaves_ready = 0;
    uint64_t free_generation;
    uint32_t old_commit_state = UHM_PAGE_COMMIT_LIVE;
    uint32_t old_prot = UHM_PROT_NONE;
    uint32_t old_flags = 0;
    uint64_t old_descriptor_id = 0;
    uint64_t old_generation = 0;
    uint64_t protecting_generation_id = 0;
    size_t target_first = 0;
    size_t target_count = 0;
    uhm_munmap_decommit_segment
        decommit_segments[UHM_MUNMAP_DECOMMIT_SEGMENT_MAX];
    uhm_vm_call_record decommit_records[UHM_MUNMAP_DECOMMIT_SEGMENT_MAX];
    size_t decommit_segment_count = 0;
    size_t decommit_record_count = 0;
    size_t decommitted_segment_count = 0;
    size_t live_decommit_bytes = 0;
    int decommit_whole_range = 0;
    int platform_unlocked = 0;
    int cross_vma_unlocked = 0;
    unsigned protecting_retries = 0;
    uhm_result result;

    if (!uhm_heap_is_accessible_pinned(heap) || !addr_ptr ||
        (addr % heap->F.vm.page_size) != 0 ||
        uhm_align_up_size(size, heap->F.vm.page_size, &rounded) || rounded == 0 ||
        uhm_range_overflows(addr, rounded, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

retry:
    uhm_tx_begin(&tx, heap, UHM_TX_OP_MUNMAP, addr, rounded, UHM_PROT_NONE, 0);
    uhm_tx_stage(&tx, UHM_TX_STAGE_VALIDATE);
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_INVALID_ARGUMENT);
    }
    uhm_metadata_epoch_advance(heap);
    result = uhm_page_range_require_live_mmap_no_protecting(heap, addr,
                                                            rounded);
    if (result != UHM_OK) {
        uhm_unlock(&heap->lock);
        if (result == UHM_ERR_PROTECTION &&
            uhm_transient_protecting_retry(&protecting_retries)) {
            goto retry;
        }
        return uhm_tx_finish(&tx, result);
    }

    if (!uhm_munmap_collect_mmap_range(heap, addr, rounded, &first_vma,
                                       &last_vma, &publish_start,
                                       &publish_end, &segment_count)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_NOT_MAPPED);
    }

    vma = first_vma;
    if (vma) {
        vma_start = vma->addr;
        if (uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
        }
        old_commit_state = uhm_vma_commit_state_from_flags(vma->flags);
        old_prot = vma->prot;
        old_flags = vma->flags & ~UHM_META_FLAG_EMERGENCY;
        old_descriptor_id = vma->desc.id;
        old_generation = vma->generation;
    } else {
        vma_start = 0;
        vma_end = 0;
    }

    if (vma && segment_count == 1u && !(addr == vma_start && end == vma_end) &&
        uhm_vma_range_overlaps_protecting(heap, vma)) {
        uhm_unlock(&heap->lock);
        if (uhm_transient_protecting_retry(&protecting_retries)) {
            goto retry;
        }
        return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
    }
    if (segment_count > 1u) {
        if (!last_vma ||
            uhm_range_overflows(last_vma->addr, last_vma->size,
                                &last_vma_end)) {
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
        }
        if ((addr > vma_start &&
             uhm_vma_range_overlaps_protecting(heap, first_vma)) ||
            (end < last_vma_end &&
             uhm_vma_range_overlaps_protecting(heap, last_vma))) {
            uhm_unlock(&heap->lock);
            if (uhm_transient_protecting_retry(&protecting_retries)) {
                goto retry;
            }
            return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
        }
    }

    result = uhm_mmap_free_find_adjacent(heap, addr, rounded,
                                         &adjacent_free_left,
                                         &adjacent_free_right);
    if (result != UHM_OK) {
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, result);
    }
    if (!adjacent_free_left && !adjacent_free_right) {
        free_node = uhm_free_extent_new(heap, addr, rounded, 1);
        if (!free_node) {
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_OUT_OF_MEMORY);
        }
        tx.free_node = free_node;
    }

    if (vma && addr > vma_start && end < vma_end) {
        if (uhm_vma_range_overlaps_protecting(heap, vma)) {
            uhm_free_extent_release_node(heap, free_node);
            uhm_unlock(&heap->lock);
            if (uhm_transient_protecting_retry(&protecting_retries)) {
                goto retry;
            }
            return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
        }
        if ((size_t)(addr - vma_start) <= (size_t)(vma_end - end)) {
            left_vma = uhm_vma_new(heap, vma_start, addr - vma_start,
                                   vma->prot,
                                   vma->flags & ~UHM_META_FLAG_EMERGENCY, 1);
            if (!left_vma) {
                uhm_free_extent_release_node(heap, free_node);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, UHM_ERR_OUT_OF_MEMORY);
            }
            tx.left_vma = left_vma;
        } else {
            right_vma = uhm_vma_new(heap, end, vma_end - end, vma->prot,
                                    vma->flags & ~UHM_META_FLAG_EMERGENCY, 1);
            if (!right_vma) {
                uhm_free_extent_release_node(heap, free_node);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, UHM_ERR_OUT_OF_MEMORY);
            }
            tx.right_vma = right_vma;
        }
    }

    uhm_tx_stage(&tx, UHM_TX_STAGE_PREPARED);
    cross_vma_unlocked = segment_count > 1u && first_vma && last_vma;
    platform_unlocked = vma && (segment_count == 1u || cross_vma_unlocked);
    if (platform_unlocked) {
        decommit_segment_count = 0;
        decommit_record_count = 0;
        decommitted_segment_count = 0;
        live_decommit_bytes = 0;
        decommit_whole_range = 0;
        memset(decommit_records, 0, sizeof(decommit_records));
        result = uhm_munmap_collect_live_decommit_segments(
            addr, rounded, first_vma, decommit_segments,
            UHM_MUNMAP_DECOMMIT_SEGMENT_MAX, &decommit_segment_count,
            &live_decommit_bytes, 0);
        if (result != UHM_OK) {
            uhm_free_extent_release_node(heap, free_node);
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
        decommit_whole_range = live_decommit_bytes == rounded;
        protecting_generation_id = uhm_heap_generation_next(heap);
        if (!uhm_page_span(heap, addr, rounded, &target_first,
                           &target_count)) {
            uhm_free_extent_release_node(heap, free_node);
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
            if (segment_count == 1u) {
                result = uhm_page_span_mark_range_locked_prepared(
                    heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
                    UHM_PAGE_VA_PROTECTING, old_commit_state, old_prot,
                    old_descriptor_id, protecting_generation_id,
                    &mark_lockset, 0);
            } else {
                result = uhm_mmap_mark_vma_range_pages_from_first(
                    heap, addr, rounded, first_vma, UHM_PAGE_VA_PROTECTING,
                    protecting_generation_id, &mark_lockset);
            }
            uhm_page_unlock_shards(heap, &mark_lockset);
        }
        if (result != UHM_OK) {
            if (segment_count > 1u) {
                uhm_lockset restore_lockset;
                uhm_page_lock_shard_range(heap, target_first, target_count,
                                          &restore_lockset);
                (void)uhm_mmap_restore_replace_range_pages_from_first(
                    heap, addr, rounded, first_vma, &restore_lockset);
                uhm_page_unlock_shards(heap, &restore_lockset);
            }
            uhm_free_extent_release_node(heap, free_node);
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
        if (segment_count > 1u &&
            !uhm_page_range_matches_owner_va_generation(
                heap, addr, rounded, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_PROTECTING, protecting_generation_id)) {
            uhm_lockset restore_lockset;
            uhm_page_lock_shard_range(heap, target_first, target_count,
                                      &restore_lockset);
            (void)uhm_mmap_restore_replace_range_pages_from_first(
                heap, addr, rounded, first_vma, &restore_lockset);
            uhm_page_unlock_shards(heap, &restore_lockset);
            uhm_free_extent_release_node(heap, free_node);
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
    }
    uhm_tx_stage(&tx, UHM_TX_STAGE_PLATFORM);
    if (platform_unlocked) {
        size_t i;

        result = UHM_OK;
        if (decommit_segment_count != 0u) {
            uhm_unlock(&heap->lock);
            if (decommit_whole_range) {
                decommit_records[0] =
                    uhm_vm_decommit_unlocked_call(heap, addr_ptr, rounded);
                decommit_record_count = 1u;
            } else {
                for (i = 0; i < decommit_segment_count; ++i) {
                    decommit_records[i] = uhm_vm_decommit_unlocked_call(
                        heap, (void *)decommit_segments[i].addr,
                        decommit_segments[i].size);
                    ++decommit_record_count;
                    if (decommit_records[i].result != UHM_OK) {
                        break;
                    }
                }
            }
            uhm_lock(&heap->lock);
        }
        for (i = 0; i < decommit_record_count; ++i) {
            uhm_result publish_result =
                uhm_vm_decommit_publish_record_locked(heap,
                                                      &decommit_records[i]);
            if (publish_result != UHM_OK && result == UHM_OK) {
                result = publish_result;
            }
            if (publish_result == UHM_OK &&
                decommit_records[i].result == UHM_OK) {
                decommitted_segment_count +=
                    decommit_whole_range ? decommit_segment_count : 1u;
            } else {
                break;
            }
        }
        if (result == UHM_OK) {
            if (segment_count == 1u) {
                uintptr_t current_vma_end;
                int vma_still_covers_target =
                    vma && vma->magic == UHM_VMA_MAGIC &&
                    !uhm_range_overflows(vma->addr, vma->size,
                                         &current_vma_end) &&
                    addr >= vma->addr && end <= current_vma_end;
                if (!vma_still_covers_target || vma->prot != old_prot ||
                    (vma->flags & ~UHM_META_FLAG_EMERGENCY) != old_flags ||
                    vma->desc.id != old_descriptor_id ||
                    vma->generation != old_generation ||
                    !uhm_page_range_matches_state_generation_id(
                        heap, addr, rounded, UHM_PAGE_OWNER_MMAP,
                        UHM_PAGE_VA_PROTECTING, old_commit_state, old_prot,
                        old_descriptor_id, protecting_generation_id)) {
                    result = UHM_ERR_CORRUPTION;
                }
            } else {
                if (!uhm_heap_is_accessible_pinned(heap) ||
                    !uhm_munmap_recheck_collected_mmap_range(
                        addr, rounded, first_vma, last_vma, publish_start,
                        publish_end, segment_count) ||
                    !uhm_page_range_matches_owner_va_generation(
                        heap, addr, rounded, UHM_PAGE_OWNER_MMAP,
                        UHM_PAGE_VA_PROTECTING, protecting_generation_id)) {
                    result = UHM_ERR_CORRUPTION;
                }
            }
        }
        if (result == UHM_OK) {
            result = uhm_mmap_free_find_adjacent(heap, addr, rounded,
                                                 &adjacent_free_left,
                                                 &adjacent_free_right);
            if (result == UHM_OK) {
                if (adjacent_free_left || adjacent_free_right) {
                    if (free_node) {
                        uhm_free_extent_release_node(heap, free_node);
                        free_node = 0;
                        tx.free_node = 0;
                    }
                } else if (!free_node) {
                    free_node = uhm_free_extent_new(heap, addr, rounded, 1);
                    if (!free_node) {
                        result = UHM_ERR_OUT_OF_MEMORY;
                    } else {
                        tx.free_node = free_node;
                    }
                }
            }
        }
    } else {
        result = UHM_ERR_CORRUPTION;
    }
    if (result != UHM_OK) {
        uhm_tx_stage(&tx, UHM_TX_STAGE_ROLLBACK);
        if (platform_unlocked) {
            uhm_lockset restore_lockset;
            while (decommitted_segment_count != 0u) {
                --decommitted_segment_count;
                uhm_vm_commit_rollback_unlocked_preserve_error(
                    heap,
                    (void *)decommit_segments[decommitted_segment_count].addr,
                    decommit_segments[decommitted_segment_count].size,
                    decommit_segments[decommitted_segment_count].prot);
            }
            uhm_page_lock_shard_range(heap, target_first, target_count,
                                      &restore_lockset);
            if (segment_count == 1u) {
                (void)uhm_page_span_mark_range_locked_prepared(
                    heap, target_first, target_count, UHM_PAGE_OWNER_MMAP,
                    UHM_PAGE_VA_MMAP_LIVE, old_commit_state, old_prot,
                    old_descriptor_id, old_generation, &restore_lockset, 0);
            } else {
                (void)uhm_mmap_restore_replace_range_pages_from_first(
                    heap, addr, rounded, first_vma, &restore_lockset);
            }
            uhm_page_unlock_shards(heap, &restore_lockset);
        }
        uhm_free_extent_release_node(heap, free_node);
        if (left_vma) {
            uhm_vma_release_node(heap, left_vma);
        }
        if (right_vma) {
            uhm_vma_release_node(heap, right_vma);
        }
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        tx.rolled_back = 1u;
        return uhm_tx_finish(&tx, result);
    }
    tx.platform_done = 1u;

    uhm_tx_stage(&tx, UHM_TX_STAGE_PUBLISH);
    if (segment_count == 0u ||
        !uhm_page_span(heap, publish_start, publish_end - publish_start,
                       &publish_first, &publish_count)) {
        uhm_free_extent_release_node(heap, free_node);
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
    if (publish_first > SIZE_MAX - publish_count ||
        target_first > SIZE_MAX - target_count) {
        uhm_free_extent_release_node(heap, free_node);
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
    publish_end_page = publish_first + publish_count;
    target_end_page = target_first + target_count;
    if (target_first < publish_first || target_end_page > publish_end_page) {
        uhm_free_extent_release_node(heap, free_node);
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
    left_remnant_first = publish_first;
    left_remnant_count = target_first - publish_first;
    right_remnant_first = target_end_page;
    right_remnant_count = publish_end_page - target_end_page;
    if ((left_vma &&
         (left_vma->addr != publish_start ||
          left_remnant_count == 0 ||
          (left_vma->size % heap->F.vm.page_size) != 0 ||
          left_vma->size / heap->F.vm.page_size != left_remnant_count)) ||
        (right_vma &&
         (right_vma->addr != end ||
          right_remnant_count == 0 ||
          (right_vma->size % heap->F.vm.page_size) != 0 ||
          right_vma->size / heap->F.vm.page_size != right_remnant_count))) {
        uhm_free_extent_release_node(heap, free_node);
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
    uhm_extent_lock_all_shards(heap, &publish_lockset);
    publish_leaves_ready = (left_vma || right_vma) ? 1 : 0;
    if (publish_leaves_ready) {
        result = uhm_page_span_mark_range_ready_locked(
            heap, publish_first, publish_count, &publish_lockset);
        if (result != UHM_OK) {
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            uhm_free_extent_release_node(heap, free_node);
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
    }
    free_generation = uhm_heap_generation_next(heap);
    result = uhm_page_span_mark_range_locked_prepared(
        heap, target_first, target_count, UHM_PAGE_OWNER_FREE,
        UHM_PAGE_VA_FREE_REUSABLE,
        UHM_PAGE_COMMIT_DECOMMITTED, UHM_PROT_NONE, 0, free_generation,
        &publish_lockset, publish_leaves_ready);
    if (result != UHM_OK) {
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
        uhm_free_extent_release_node(heap, free_node);
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

    cursor = addr;
    vma = first_vma;
    while (cursor < end) {
        uhm_vma *next_vma;
        uintptr_t seg_end;
        if (!uhm_munmap_vma_segment_at(vma, cursor, end, &vma_end,
                                       &seg_end)) {
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            uhm_free_extent_release_node(heap, free_node);
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
        vma_start = vma->addr;
        next_vma = uhm_vma_tree_successor(vma);

        if (cursor == vma_start && seg_end == vma_end) {
            uhm_vma_remove(heap, vma);
        } else if (cursor == vma_start) {
            uhm_vma_tree_remove(heap, vma);
            vma->addr = seg_end;
            vma->size = vma_end - seg_end;
            uhm_vma_descriptor_sync(vma, uhm_vma_commit_state_from_flags(vma->flags));
            uhm_vma_tree_insert(heap, vma);
            uhm_diag_note_munmap_trim_repair_elided(heap, vma->size);
        } else if (seg_end == vma_end) {
            vma->size = cursor - vma_start;
            uhm_vma_descriptor_sync(vma, uhm_vma_commit_state_from_flags(vma->flags));
            uhm_diag_note_munmap_trim_repair_elided(heap, vma->size);
        } else if (left_vma) {
            if (left_vma->addr != vma_start ||
                left_vma->size != cursor - vma_start) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                uhm_free_extent_release_node(heap, free_node);
                uhm_vma_release_node(heap, left_vma);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
            }
            result = uhm_page_span_mark_range_locked_prepared(
                heap, left_remnant_first, left_remnant_count,
                UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE,
                uhm_vma_commit_state_from_flags(left_vma->flags),
                left_vma->prot, left_vma->desc.id, left_vma->generation,
                &publish_lockset, 1);
            if (result != UHM_OK) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                uhm_free_extent_release_node(heap, free_node);
                uhm_vma_release_node(heap, left_vma);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, result);
            }
            uhm_vma_tree_remove(heap, vma);
            vma->addr = seg_end;
            vma->size = vma_end - seg_end;
            uhm_vma_descriptor_sync(vma, uhm_vma_commit_state_from_flags(vma->flags));
            uhm_vma_tree_insert(heap, vma);
            uhm_diag_note_munmap_trim_repair_elided(heap, vma->size);
            uhm_vma_insert_before(heap, vma, left_vma);
            left_vma = 0;
        } else if (right_vma) {
            if (right_vma->addr != seg_end ||
                right_vma->size != vma_end - seg_end) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                uhm_free_extent_release_node(heap, free_node);
                uhm_vma_release_node(heap, right_vma);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
            }
            result = uhm_page_span_mark_range_locked_prepared(
                heap, right_remnant_first, right_remnant_count,
                UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE,
                uhm_vma_commit_state_from_flags(right_vma->flags),
                right_vma->prot, right_vma->desc.id, right_vma->generation,
                &publish_lockset, 1);
            if (result != UHM_OK) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                uhm_free_extent_release_node(heap, free_node);
                uhm_vma_release_node(heap, right_vma);
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                return uhm_tx_finish(&tx, result);
            }
            vma->size = cursor - vma_start;
            uhm_vma_descriptor_sync(vma, uhm_vma_commit_state_from_flags(vma->flags));
            uhm_diag_note_munmap_trim_repair_elided(heap, vma->size);
            uhm_vma_insert_after(heap, vma, right_vma);
            right_vma = 0;
        } else {
            uhm_metadata_unlock_shards(heap, &publish_lockset);
            uhm_page_unlock_shards(heap, &publish_lockset);
            uhm_free_extent_release_node(heap, free_node);
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            return uhm_tx_finish(&tx, UHM_ERR_CORRUPTION);
        }

        cursor = seg_end;
        vma = next_vma;
    }

    heap->stats.decommitted_bytes += rounded;
    uhm_vm_account_decommit(heap, addr, rounded);
    if (heap->stats.mmap_live_bytes >= rounded) {
        heap->stats.mmap_live_bytes -= rounded;
    } else {
        heap->stats.mmap_live_bytes = 0;
    }
    if (free_node) {
        result = uhm_mmap_free_insert_node(heap, free_node);
    } else {
        result = uhm_mmap_free_extend_adjacent_prevalidated(
            heap, addr, rounded, adjacent_free_left, adjacent_free_right);
    }
    if (result != UHM_OK) {
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, result);
    }
    uhm_metadata_unlock_shards(heap, &publish_lockset);
    uhm_page_unlock_shards(heap, &publish_lockset);
    uhm_unlock(&heap->lock);
    tx.published = 1u;
    tx.publish_generation = uhm_heap_generation_load(heap);
    return uhm_tx_finish(&tx, UHM_OK);
}

static uhm_result UHM_CALL uhm_api_munmap(uhm_heap *heap, void *addr_ptr,
                                          size_t size) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_munmap_pinned(heap, addr_ptr, size);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

static uhm_result uhm_mremap_shrink_locked(uhm_heap *heap, uintptr_t addr,
                                           size_t old_size, size_t new_size,
                                           uhm_tx *tx) {
    uintptr_t old_end;
    uintptr_t tail_addr;
    size_t tail_size;
    uhm_vma *first_tail_vma;
    uhm_vma *tail_last_vma = 0;
    uhm_vma *tail_split_vma = 0;
    uhm_vma *split_left_vma = 0;
    uintptr_t tail_publish_start = 0;
    uintptr_t tail_publish_end = 0;
    size_t tail_segment_count = 0;
    size_t split_left_old_size = 0;
    size_t target_first = 0;
    size_t target_count = 0;
    uhm_free_extent *free_node = 0;
    uhm_free_extent *adjacent_free_left = 0;
    uhm_free_extent *adjacent_free_right = 0;
    uhm_lockset publish_lockset;
    uint64_t free_generation;
    uint64_t protecting_generation_id = 0;
    uintptr_t cursor;
    uhm_result result;

    if (uhm_range_overflows(addr, old_size, &old_end) ||
        uhm_range_overflows(addr, new_size, &tail_addr) ||
        old_size <= new_size) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    tail_size = old_size - new_size;
    if (uhm_page_range_overlaps_va_state(heap, tail_addr, tail_size,
                                         UHM_PAGE_VA_PROTECTING)) {
        return UHM_ERR_PROTECTION;
    }
    if (!uhm_page_range_has_live_mmap(heap, tail_addr, tail_size)) {
        return UHM_ERR_NOT_MAPPED;
    }
    first_tail_vma = uhm_find_vma_covering(heap, tail_addr, 1u);
    if (!first_tail_vma) {
        return UHM_ERR_NOT_MAPPED;
    }
    if (first_tail_vma->addr > tail_addr || first_tail_vma->addr >= old_end) {
        return UHM_ERR_CORRUPTION;
    }

    result = uhm_mmap_free_find_adjacent(heap, tail_addr, tail_size,
                                         &adjacent_free_left,
                                         &adjacent_free_right);
    if (result != UHM_OK) {
        (void)uhm_bump_failure_generation(heap);
        return result;
    }
    if (!adjacent_free_left && !adjacent_free_right) {
        free_node = uhm_free_extent_new(heap, tail_addr, tail_size, 1);
        if (!free_node) {
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_OUT_OF_MEMORY;
        }
        tx->free_node = free_node;
    }

    uhm_tx_stage(tx, UHM_TX_STAGE_PREPARED);
    if (!uhm_page_span(heap, tail_addr, tail_size, &target_first,
                       &target_count)) {
        uhm_free_extent_release_node(heap, free_node);
        (void)uhm_bump_failure_generation(heap);
        return UHM_ERR_CORRUPTION;
    }
    if (first_tail_vma->addr < tail_addr) {
        uintptr_t first_tail_end;
        uint32_t split_flags =
            first_tail_vma->flags & ~UHM_META_FLAG_EMERGENCY;

        if (uhm_range_overflows(first_tail_vma->addr, first_tail_vma->size,
                                &first_tail_end) ||
            tail_addr >= first_tail_end) {
            uhm_free_extent_release_node(heap, free_node);
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_CORRUPTION;
        }
        tail_split_vma =
            uhm_vma_new(heap, tail_addr, (size_t)(first_tail_end - tail_addr),
                        first_tail_vma->prot, split_flags, 1);
        if (!tail_split_vma) {
            uhm_free_extent_release_node(heap, free_node);
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_OUT_OF_MEMORY;
        }
        split_left_vma = first_tail_vma;
        split_left_old_size = split_left_vma->size;
        split_left_vma->size = (size_t)(tail_addr - split_left_vma->addr);
        uhm_vma_descriptor_sync(
            split_left_vma,
            uhm_vma_commit_state_from_flags(split_left_vma->flags));
        uhm_diag_note_munmap_trim_repair_elided(heap, split_left_vma->size);
        uhm_vma_insert_after(heap, split_left_vma, tail_split_vma);
        first_tail_vma = tail_split_vma;
    }

    if (!uhm_munmap_walk_mmap_range(tail_addr, tail_size, first_tail_vma,
                                    &tail_last_vma, &tail_publish_start,
                                    &tail_publish_end,
                                    &tail_segment_count) ||
        tail_publish_start != tail_addr || tail_publish_end != old_end ||
        tail_segment_count == 0u) {
        if (tail_split_vma) {
            uhm_vma_remove(heap, tail_split_vma);
            tail_split_vma = 0;
        }
        if (split_left_vma) {
            split_left_vma->size = split_left_old_size;
            uhm_vma_descriptor_sync(
                split_left_vma,
                uhm_vma_commit_state_from_flags(split_left_vma->flags));
        }
        uhm_free_extent_release_node(heap, free_node);
        (void)uhm_bump_failure_generation(heap);
        return UHM_ERR_CORRUPTION;
    }

    protecting_generation_id = uhm_heap_generation_next(heap);
    {
        uhm_lockset mark_lockset;

        uhm_page_lock_shard_range(heap, target_first, target_count,
                                  &mark_lockset);
        result = uhm_mmap_mark_vma_range_pages_from_first(
            heap, tail_addr, tail_size, first_tail_vma,
            UHM_PAGE_VA_PROTECTING, protecting_generation_id,
            &mark_lockset);
        uhm_page_unlock_shards(heap, &mark_lockset);
    }
    if (result != UHM_OK ||
        !uhm_page_range_matches_owner_va_generation(
            heap, tail_addr, tail_size, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_PROTECTING, protecting_generation_id)) {
        if (tail_split_vma) {
            uhm_vma_remove(heap, tail_split_vma);
            tail_split_vma = 0;
        }
        if (split_left_vma) {
            split_left_vma->size = split_left_old_size;
            uhm_vma_descriptor_sync(
                split_left_vma,
                uhm_vma_commit_state_from_flags(split_left_vma->flags));
        }
        uhm_free_extent_release_node(heap, free_node);
        (void)uhm_bump_failure_generation(heap);
        return result != UHM_OK ? result : UHM_ERR_CORRUPTION;
    }

    uhm_tx_stage(tx, UHM_TX_STAGE_PLATFORM);
    {
        uhm_vm_call_record decommit_record;

        uhm_unlock(&heap->lock);
        decommit_record = uhm_vm_decommit_unlocked_call(
            heap, (void *)tail_addr, tail_size);
        uhm_lock(&heap->lock);
        result = uhm_vm_decommit_publish_record_locked(heap,
                                                       &decommit_record);
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             !uhm_munmap_recheck_collected_mmap_range(
                 tail_addr, tail_size, first_tail_vma, tail_last_vma,
                 tail_publish_start, tail_publish_end, tail_segment_count) ||
             !uhm_page_range_matches_owner_va_generation(
                 heap, tail_addr, tail_size, UHM_PAGE_OWNER_MMAP,
                  UHM_PAGE_VA_PROTECTING, protecting_generation_id))) {
            result = UHM_ERR_CORRUPTION;
        }
        if (result == UHM_OK) {
            result = uhm_mmap_free_find_adjacent(heap, tail_addr, tail_size,
                                                 &adjacent_free_left,
                                                 &adjacent_free_right);
            if (result == UHM_OK) {
                if (adjacent_free_left || adjacent_free_right) {
                    if (free_node) {
                        uhm_free_extent_release_node(heap, free_node);
                        free_node = 0;
                        tx->free_node = 0;
                    }
                } else if (!free_node) {
                    free_node =
                        uhm_free_extent_new(heap, tail_addr, tail_size, 1);
                    if (!free_node) {
                        result = UHM_ERR_OUT_OF_MEMORY;
                    } else {
                        tx->free_node = free_node;
                    }
                }
            }
        }
    }
    if (result != UHM_OK) {
        uhm_tx_stage(tx, UHM_TX_STAGE_ROLLBACK);
        {
            uhm_lockset restore_lockset;

            uhm_page_lock_shard_range(heap, target_first, target_count,
                                      &restore_lockset);
            (void)uhm_mmap_restore_replace_range_pages_from_first(
                heap, tail_addr, tail_size, first_tail_vma,
                &restore_lockset);
            uhm_page_unlock_shards(heap, &restore_lockset);
        }
        uhm_free_extent_release_node(heap, free_node);
        (void)uhm_bump_failure_generation(heap);
        tx->rolled_back = 1u;
        return result;
    }
    tx->platform_done = 1u;

    uhm_tx_stage(tx, UHM_TX_STAGE_PUBLISH);
    uhm_page_lock_shard_range(heap, target_first, target_count,
                              &publish_lockset);
    uhm_vma_lock_all_shards(heap, &publish_lockset);
    uhm_extent_lock_all_shards(heap, &publish_lockset);

    free_generation = uhm_heap_generation_next(heap);
    result = uhm_page_span_mark_range_locked_prepared(
        heap, target_first, target_count, UHM_PAGE_OWNER_FREE,
        UHM_PAGE_VA_FREE_REUSABLE, UHM_PAGE_COMMIT_DECOMMITTED,
        UHM_PROT_NONE, 0, free_generation, &publish_lockset, 0);
    if (result != UHM_OK) {
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
        uhm_free_extent_release_node(heap, free_node);
        (void)uhm_bump_failure_generation(heap);
        return result;
    }

    cursor = tail_addr;
    {
        uhm_vma *vma = first_tail_vma;
        while (cursor < old_end) {
            uintptr_t vma_start;
            uintptr_t vma_end;
            uintptr_t seg_end;
            uhm_vma *next_vma;

            if (!vma ||
                uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                uhm_free_extent_release_node(heap, free_node);
                (void)uhm_bump_failure_generation(heap);
                return UHM_ERR_CORRUPTION;
            }
            vma_start = vma->addr;
            seg_end = vma_end < old_end ? vma_end : old_end;
            if (seg_end != vma_end) {
                uhm_metadata_unlock_shards(heap, &publish_lockset);
                uhm_page_unlock_shards(heap, &publish_lockset);
                uhm_free_extent_release_node(heap, free_node);
                (void)uhm_bump_failure_generation(heap);
                return UHM_ERR_CORRUPTION;
            }
            next_vma = uhm_vma_tree_successor(vma);
            if (cursor == vma_start) {
                uhm_vma_remove(heap, vma);
            } else {
                vma->size = cursor - vma_start;
                uhm_vma_descriptor_sync(
                    vma, uhm_vma_commit_state_from_flags(vma->flags));
                uhm_diag_note_munmap_trim_repair_elided(heap, vma->size);
            }
            cursor = seg_end;
            vma = next_vma;
        }
    }

    heap->stats.decommitted_bytes += tail_size;
    uhm_vm_account_decommit(heap, tail_addr, tail_size);
    if (heap->stats.mmap_live_bytes >= tail_size) {
        heap->stats.mmap_live_bytes -= tail_size;
    } else {
        heap->stats.mmap_live_bytes = 0;
    }
    if (free_node) {
        result = uhm_mmap_free_insert_node(heap, free_node);
    } else {
        result = uhm_mmap_free_extend_adjacent_prevalidated(
            heap, tail_addr, tail_size, adjacent_free_left,
            adjacent_free_right);
    }
    if (result != UHM_OK) {
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
        (void)uhm_bump_failure_generation(heap);
        return result;
    }
    uhm_metadata_unlock_shards(heap, &publish_lockset);
    uhm_page_unlock_shards(heap, &publish_lockset);
    tx->published = 1u;
    tx->publish_generation = uhm_heap_generation_load(heap);
    return UHM_OK;
}

static uhm_result uhm_mremap_publish_grow_tail(uhm_heap *heap,
                                               uhm_vma *anchor,
                                               uhm_vma *tail_vma,
                                               uint32_t commit_state) {
    size_t first;
    size_t count;
    uhm_lockset lockset;
    uhm_result result;

    if (!tail_vma ||
        !uhm_page_span(heap, tail_vma->addr, tail_vma->size, &first, &count)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_page_lock_shard_range(heap, first, count, &lockset);
    uhm_vma_lock_all_shards(heap, &lockset);
    result = uhm_page_span_mark_range_locked_prepared(
        heap, first, count, UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
        commit_state, tail_vma->prot, tail_vma->desc.id,
        tail_vma->generation, &lockset, 0);
    if (result == UHM_OK) {
        if (anchor && anchor->magic == UHM_VMA_MAGIC) {
            uhm_vma_insert_after(heap, anchor, tail_vma);
        } else {
            uhm_vma_insert_head(heap, tail_vma);
        }
    }
    uhm_metadata_unlock_shards(heap, &lockset);
    uhm_page_unlock_shards(heap, &lockset);
    return result;
}

static uhm_result uhm_mremap_grow_locked(uhm_heap *heap, uintptr_t addr,
                                         size_t old_size, size_t new_size,
                                         uhm_vma *first_vma,
                                         uhm_vma *last_vma, uhm_tx *tx) {
    uintptr_t tail_addr;
    uintptr_t new_end;
    size_t tail_size;
    uint32_t tail_flags;
    uint32_t commit_state;
    uintptr_t anchor_addr;
    size_t anchor_size;
    uint32_t anchor_flags;
    uint32_t anchor_prot;
    uint64_t anchor_descriptor_id;
    uint64_t anchor_generation;
    uhm_vma *tail_vma;
    uhm_range_claim claim;
    size_t target_first = 0;
    size_t target_count = 0;
    uint64_t protecting_generation_id = 0;
    int platform_unlocked = 0;
    uhm_result result;

    uhm_claim_reset(&claim);
    if (uhm_range_overflows(addr, old_size, &tail_addr) ||
        uhm_range_overflows(addr, new_size, &new_end) ||
        old_size >= new_size || !first_vma || !last_vma) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    tail_size = new_size - old_size;
    if (new_end > heap->core.arena_end || tail_addr < heap->core.malloc_hi ||
        tail_addr < heap->core.control_end) {
        return UHM_ERR_PROTECTION;
    }
    if (uhm_page_range_overlaps_va_state(heap, tail_addr, tail_size,
                                         UHM_PAGE_VA_PROTECTING)) {
        return UHM_ERR_PROTECTION;
    }
    if (uhm_find_vma_overlap(heap, tail_addr, tail_size) != 0 ||
        !uhm_page_range_is_free(heap, tail_addr, tail_size)) {
        return UHM_ERR_PROTECTION;
    }

    anchor_addr = last_vma->addr;
    anchor_size = last_vma->size;
    anchor_flags = last_vma->flags & ~UHM_META_FLAG_EMERGENCY;
    anchor_prot = last_vma->prot;
    anchor_descriptor_id = last_vma->desc.id;
    anchor_generation = last_vma->generation;
    tail_flags = anchor_flags;
    commit_state = uhm_vma_commit_state_from_flags(tail_flags);
    tail_vma = uhm_vma_new(heap, tail_addr, tail_size, anchor_prot,
                           tail_flags, 0);
    if (!tail_vma) {
        (void)uhm_bump_failure_generation(heap);
        return UHM_ERR_OUT_OF_MEMORY;
    }

    result = uhm_claim_mmap_fixed(heap, tail_addr, tail_size, &claim);
    if (result != UHM_OK) {
        uhm_vma_release_node(heap, tail_vma);
        (void)uhm_bump_failure_generation(heap);
        return result;
    }
    tx->range_claimed = 1u;
    uhm_tx_stage(tx, UHM_TX_STAGE_CLAIMED);

    result = uhm_page_prepare_range(heap, tail_addr, tail_size);
    if (result != UHM_OK) {
        (void)uhm_claim_abort(heap, &claim, 1);
        uhm_vma_release_node(heap, tail_vma);
        (void)uhm_bump_failure_generation(heap);
        return result;
    }
    uhm_tx_stage(tx, UHM_TX_STAGE_PREPARED);

    if (commit_state == UHM_PAGE_COMMIT_LIVE) {
        uhm_vm_call_record commit_record;
        uhm_vm_call_record numa_record;

        memset(&commit_record, 0, sizeof(commit_record));
        memset(&numa_record, 0, sizeof(numa_record));
        protecting_generation_id = uhm_heap_generation_next(heap);
        if (!uhm_page_span(heap, tail_addr, tail_size, &target_first,
                           &target_count)) {
            (void)uhm_claim_abort(heap, &claim, 1);
            uhm_vma_release_node(heap, tail_vma);
            (void)uhm_bump_failure_generation(heap);
            return UHM_ERR_CORRUPTION;
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
            uhm_vma_release_node(heap, tail_vma);
            (void)uhm_bump_failure_generation(heap);
            return result;
        }
        platform_unlocked = 1;
        uhm_tx_stage(tx, UHM_TX_STAGE_PLATFORM);
        uhm_unlock(&heap->lock);
        commit_record = uhm_vm_commit_unlocked_call(
            heap, (void *)tail_addr, tail_size, anchor_prot);
        if (commit_record.result == UHM_OK) {
            numa_record = uhm_vm_numa_hint_unlocked_call(
                heap, (void *)tail_addr, tail_size,
                UHM_NUMA_POLICY_DEFAULT, heap->core.preferred_numa_node,
                tail_flags);
        }
        uhm_lock(&heap->lock);
        result = uhm_vm_commit_publish_record_locked(heap, &commit_record);
        if (result == UHM_OK) {
            result = uhm_vm_numa_hint_publish_record_locked(heap,
                                                           &numa_record);
        }
        if (result == UHM_OK) {
            if (!uhm_heap_is_accessible_pinned(heap) ||
                !uhm_mmap_recheck_exact_range_from_first(
                    heap, addr, old_size, first_vma, last_vma) ||
                last_vma->magic != UHM_VMA_MAGIC ||
                last_vma->addr != anchor_addr ||
                last_vma->size != anchor_size ||
                (last_vma->flags & ~UHM_META_FLAG_EMERGENCY) !=
                    anchor_flags ||
                last_vma->prot != anchor_prot ||
                last_vma->desc.id != anchor_descriptor_id ||
                last_vma->generation != anchor_generation ||
                uhm_find_vma_overlap(heap, tail_addr, tail_size) != 0 ||
                !uhm_page_range_matches_state_generation_id(
                    heap, tail_addr, tail_size, UHM_PAGE_OWNER_MMAP,
                    UHM_PAGE_VA_PROTECTING, UHM_PAGE_COMMIT_NONE,
                    UHM_PROT_NONE, 0, protecting_generation_id)) {
                result = UHM_ERR_CORRUPTION;
            }
        }
        if (result != UHM_OK) {
            uhm_tx_stage(tx, UHM_TX_STAGE_ROLLBACK);
            if (commit_record.result == UHM_OK || commit_record.did_call) {
                uhm_vm_decommit_rollback_unlocked_preserve_error(
                    heap, (void *)tail_addr, tail_size);
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
            uhm_vma_release_node(heap, tail_vma);
            (void)uhm_bump_failure_generation(heap);
            tx->rolled_back = 1u;
            return result;
        }
        tx->platform_done = 1u;
    }

    uhm_tx_stage(tx, UHM_TX_STAGE_PUBLISH);
    result = uhm_mremap_publish_grow_tail(heap, last_vma, tail_vma,
                                          commit_state);
    if (result != UHM_OK) {
        if (platform_unlocked) {
            uhm_vm_decommit_rollback_unlocked_preserve_error(
                heap, (void *)tail_addr, tail_size);
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
        uhm_vma_release_node(heap, tail_vma);
        (void)uhm_bump_failure_generation(heap);
        tx->rolled_back = 1u;
        return result;
    }
    if (commit_state == UHM_PAGE_COMMIT_LIVE) {
        heap->stats.committed_bytes += tail_size;
    }
    heap->stats.mmap_live_bytes += tail_size;
    uhm_claim_disarm(heap, &claim);
    tx->published = 1u;
    tx->publish_generation = uhm_heap_generation_load(heap);
    return UHM_OK;
}

static uhm_result uhm_mprotect_pinned(uhm_heap *heap, void *addr_ptr,
                                      size_t size, uint32_t prot);

static uhm_result uhm_mremap_pinned(uhm_heap *heap, void **addr_inout,
                                    size_t old_size, size_t new_size,
                                    uint32_t flags) {
    uintptr_t addr;
    uintptr_t old_end;
    uintptr_t new_end;
    size_t old_rounded;
    size_t new_rounded;
    uhm_vma *first_vma = 0;
    uhm_vma *last_vma = 0;
    uhm_tx tx;
    uhm_result result;

    if (!uhm_heap_is_accessible_pinned(heap) || !addr_inout ||
        !*addr_inout) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((flags & UHM_REMAP_MAYMOVE) != 0) {
        return UHM_ERR_UNSUPPORTED;
    }
    if (flags != 0u) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    addr = (uintptr_t)*addr_inout;
    if ((addr % heap->F.vm.page_size) != 0 ||
        uhm_align_up_size(old_size, heap->F.vm.page_size, &old_rounded) ||
        uhm_align_up_size(new_size, heap->F.vm.page_size, &new_rounded) ||
        old_rounded == 0 || new_rounded == 0 ||
        uhm_range_overflows(addr, old_rounded, &old_end) ||
        uhm_range_overflows(addr, new_rounded, &new_end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    uhm_tx_begin(&tx, heap, UHM_TX_OP_MREMAP, addr, new_rounded,
                 UHM_PROT_NONE, flags);
    uhm_tx_stage(&tx, UHM_TX_STAGE_VALIDATE);
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_INVALID_ARGUMENT);
    }
    uhm_metadata_epoch_advance(heap);
    if (addr < heap->core.arena_base || old_end > heap->core.arena_end ||
        new_end > heap->core.arena_end) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_NOT_MAPPED);
    }
    if (uhm_page_range_overlaps_va_state(heap, addr, old_rounded,
                                         UHM_PAGE_VA_PROTECTING)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_PROTECTION);
    }
    if (!uhm_mmap_collect_exact_range(heap, addr, old_rounded, &first_vma,
                                      &last_vma)) {
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_ERR_NOT_MAPPED);
    }

    if (new_rounded == old_rounded) {
        uhm_tx_stage(&tx, UHM_TX_STAGE_PREPARED);
        uhm_tx_stage(&tx, UHM_TX_STAGE_PUBLISH);
        tx.published = 1u;
        tx.publish_generation = uhm_heap_generation_load(heap);
        *addr_inout = (void *)addr;
        uhm_unlock(&heap->lock);
        return uhm_tx_finish(&tx, UHM_OK);
    }

    if (new_rounded < old_rounded) {
        result = uhm_mremap_shrink_locked(heap, addr, old_rounded,
                                          new_rounded, &tx);
    } else {
        result = uhm_mremap_grow_locked(heap, addr, old_rounded,
                                        new_rounded, first_vma, last_vma,
                                        &tx);
    }
    if (result == UHM_OK) {
        *addr_inout = (void *)addr;
    }
    uhm_unlock(&heap->lock);
    return uhm_tx_finish(&tx, result);
}

static int uhm_mremap_vma_matches_exact_fields(
    uhm_vma *vma, uintptr_t addr, size_t size, uintptr_t expected_end,
    uint32_t prot, uint32_t flags, uint64_t descriptor_id,
    uint64_t generation) {
    uintptr_t vma_end = 0;

    return vma && vma->magic == UHM_VMA_MAGIC && vma->addr == addr &&
           !uhm_range_overflows(vma->addr, vma->size, &vma_end) &&
           vma->size == size && vma_end == expected_end &&
           vma->prot == prot &&
           (vma->flags & ~UHM_META_FLAG_EMERGENCY) == flags &&
           vma->desc.id == descriptor_id && vma->generation == generation;
}

typedef struct uhm_mremap_maymove_source_claim {
    uintptr_t addr;
    size_t size;
    uint32_t commit_state;
    uint32_t prot;
    uint32_t flags;
    uint64_t descriptor_id;
    uint64_t generation;
    uint64_t protecting_generation_id;
    int claimed;
} uhm_mremap_maymove_source_claim;

static uhm_result uhm_mremap_maymove_restore_source_claim(
    uhm_heap *heap, uhm_mremap_maymove_source_claim *claim) {
    uhm_result result = UHM_OK;

    if (!claim || !claim->claimed) {
        return UHM_OK;
    }
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        result = UHM_ERR_CORRUPTION;
    } else {
        uhm_lockset restore_lockset;
        size_t first = 0;
        size_t count = 0;

        if (!uhm_page_span(heap, claim->addr, claim->size, &first, &count)) {
            result = UHM_ERR_CORRUPTION;
        } else {
            uhm_page_lock_shard_range(heap, first, count, &restore_lockset);
            result = uhm_page_span_mark_range_locked_prepared(
                heap, first, count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, claim->commit_state, claim->prot,
                claim->descriptor_id, claim->generation, &restore_lockset, 0);
            uhm_page_unlock_shards(heap, &restore_lockset);
        }
    }
    if (result == UHM_OK) {
        claim->claimed = 0;
    } else {
        (void)uhm_bump_failure_generation(heap);
    }
    uhm_unlock(&heap->lock);
    return result;
}

static uhm_result uhm_mremap_maymove_claim_source(
    uhm_heap *heap, uintptr_t addr, size_t old_rounded,
    uhm_mremap_maymove_source_claim *claim) {
    uintptr_t old_end;
    uhm_result result = UHM_OK;
    size_t first = 0;
    size_t count = 0;

    if (!claim) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    memset(claim, 0, sizeof(*claim));
    claim->addr = addr;
    claim->size = old_rounded;
    if (uhm_range_overflows(addr, old_rounded, &old_end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap) ||
        addr < heap->core.arena_base || old_end > heap->core.arena_end ||
        uhm_page_range_overlaps_va_state(heap, addr, old_rounded,
                                         UHM_PAGE_VA_PROTECTING)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_PROTECTION;
    }
    {
        uhm_vma *first_vma = 0;
        uhm_vma *last_vma = 0;

        if (!uhm_mmap_collect_exact_range(heap, addr, old_rounded,
                                          &first_vma, &last_vma) ||
            first_vma != last_vma || !first_vma ||
            first_vma->addr != addr || first_vma->size != old_rounded ||
            (first_vma->prot & UHM_PROT_READ) == 0 ||
            uhm_vma_commit_state_from_flags(first_vma->flags) !=
                UHM_PAGE_COMMIT_LIVE) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_UNSUPPORTED;
        }
        claim->commit_state = UHM_PAGE_COMMIT_LIVE;
        claim->prot = first_vma->prot;
        claim->flags = first_vma->flags & ~UHM_META_FLAG_EMERGENCY;
        claim->descriptor_id = first_vma->desc.id;
        claim->generation = first_vma->generation;
    }
    if (!uhm_page_span(heap, addr, old_rounded, &first, &count)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    claim->protecting_generation_id = uhm_heap_generation_next(heap);
    {
        uhm_lockset mark_lockset;

        uhm_page_lock_shard_range(heap, first, count, &mark_lockset);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, first, count, UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_PROTECTING,
            claim->commit_state, claim->prot, claim->descriptor_id,
            claim->protecting_generation_id, &mark_lockset, 0);
        uhm_page_unlock_shards(heap, &mark_lockset);
    }
    if (result == UHM_OK) {
        if (!uhm_page_range_matches_state_generation_id(
                heap, addr, old_rounded, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_PROTECTING, claim->commit_state, claim->prot,
                claim->descriptor_id, claim->protecting_generation_id)) {
            result = UHM_ERR_CORRUPTION;
        }
    }
    if (result == UHM_OK) {
        claim->claimed = 1;
    } else {
        uhm_lockset restore_lockset;

        uhm_page_lock_shard_range(heap, first, count, &restore_lockset);
        (void)uhm_page_span_mark_range_locked_prepared(
            heap, first, count, UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
            claim->commit_state, claim->prot, claim->descriptor_id,
            claim->generation, &restore_lockset, 0);
        uhm_page_unlock_shards(heap, &restore_lockset);
        (void)uhm_bump_failure_generation(heap);
    }
    uhm_unlock(&heap->lock);
    return result;
}

static uhm_result uhm_mremap_maymove_release_source_claim(
    uhm_heap *heap, uhm_mremap_maymove_source_claim *claim) {
    uhm_free_extent *free_node = 0;
    uhm_vma *vma = 0;
    uhm_vm_call_record decommit_record;
    uintptr_t claim_end = 0;
    uintptr_t vma_end = 0;
    size_t first = 0;
    size_t count = 0;
    uhm_result result;

    if (!claim || !claim->claimed ||
        uhm_range_overflows(claim->addr, claim->size, &claim_end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    memset(&decommit_record, 0, sizeof(decommit_record));
    uhm_lock(&heap->lock);
    vma = uhm_find_vma_covering(heap, claim->addr, 1u);
    if (!uhm_heap_is_accessible_pinned(heap) ||
        !uhm_page_span(heap, claim->addr, claim->size, &first, &count) ||
        !vma || vma->addr != claim->addr ||
        uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
        vma->size != claim->size ||
        vma_end != claim_end ||
        vma->prot != claim->prot ||
        (vma->flags & ~UHM_META_FLAG_EMERGENCY) != claim->flags ||
        vma->desc.id != claim->descriptor_id ||
        vma->generation != claim->generation ||
        !uhm_page_range_matches_state_generation_id(
            heap, claim->addr, claim->size, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_PROTECTING, claim->commit_state, claim->prot,
            claim->descriptor_id, claim->protecting_generation_id)) {
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }

    free_node = uhm_free_extent_new(heap, claim->addr, claim->size, 1);
    if (!free_node) {
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return UHM_ERR_OUT_OF_MEMORY;
    }

    uhm_unlock(&heap->lock);
    decommit_record =
        uhm_vm_decommit_unlocked_call(heap, (void *)claim->addr, claim->size);
    uhm_lock(&heap->lock);
    result = uhm_vm_decommit_publish_record_locked(heap, &decommit_record);
    if (result == UHM_OK &&
        (!uhm_heap_is_accessible_pinned(heap) ||
         !uhm_mremap_vma_matches_exact_fields(
             vma, claim->addr, claim->size, claim_end, claim->prot,
             claim->flags, claim->descriptor_id, claim->generation) ||
          !uhm_page_range_matches_state_generation_id(
              heap, claim->addr, claim->size, UHM_PAGE_OWNER_MMAP,
              UHM_PAGE_VA_PROTECTING, claim->commit_state, claim->prot,
              claim->descriptor_id, claim->protecting_generation_id))) {
        result = UHM_ERR_CORRUPTION;
    }
    if (result != UHM_OK) {
        if (decommit_record.result != UHM_OK) {
            uhm_lockset restore_lockset;

            uhm_page_lock_shard_range(heap, first, count, &restore_lockset);
            (void)uhm_page_span_mark_range_locked_prepared(
                heap, first, count, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, claim->commit_state, claim->prot,
                claim->descriptor_id, claim->generation, &restore_lockset, 0);
            uhm_page_unlock_shards(heap, &restore_lockset);
            claim->claimed = 0;
        }
        uhm_free_extent_release_node(heap, free_node);
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return result;
    }

    {
        uhm_lockset publish_lockset;
        uint64_t free_generation = uhm_heap_generation_next(heap);

        uhm_page_lock_shard_range(heap, first, count, &publish_lockset);
        uhm_vma_lock_all_shards(heap, &publish_lockset);
        uhm_extent_lock_all_shards(heap, &publish_lockset);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, first, count, UHM_PAGE_OWNER_FREE,
            UHM_PAGE_VA_FREE_REUSABLE, UHM_PAGE_COMMIT_DECOMMITTED,
            UHM_PROT_NONE, 0, free_generation, &publish_lockset, 0);
        if (result == UHM_OK) {
            uhm_vma_remove(heap, vma);
            heap->stats.decommitted_bytes += claim->size;
            uhm_vm_account_decommit(heap, claim->addr, claim->size);
            if (heap->stats.mmap_live_bytes >= claim->size) {
                heap->stats.mmap_live_bytes -= claim->size;
            } else {
                heap->stats.mmap_live_bytes = 0;
            }
            result = uhm_mmap_free_insert_node(heap, free_node);
            if (result == UHM_ERR_OUT_OF_MEMORY) {
                /*
                 * The move has already linearized: the source pages are
                 * decommitted and the new mapping contains the payload. Keep
                 * the old address unmapped/free in the page map, but drop it
                 * from auto-mmap reuse rather than reporting a failed move
                 * that cannot truthfully restore the old payload.
                 */
                uhm_free_extent_release_node(heap, free_node);
                free_node = 0;
                result = UHM_OK;
            }
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result != UHM_OK) {
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return result;
    }
    claim->claimed = 0;
    uhm_unlock(&heap->lock);
    return UHM_OK;
}

static uhm_result uhm_mremap_maymove_rollback_new_mapping(
    uhm_heap *heap, void *addr_ptr, size_t size) {
    uintptr_t addr = (uintptr_t)addr_ptr;
    uintptr_t end = 0;
    uintptr_t vma_end = 0;
    uhm_vma *vma;
    uhm_free_extent *free_node;
    uhm_vm_call_record decommit_record;
    size_t first = 0;
    size_t count = 0;
    uint32_t old_prot = UHM_PROT_NONE;
    uint32_t old_flags = 0;
    uint64_t old_descriptor_id = 0;
    uint64_t old_generation = 0;
    int insert_attempted = 0;
    uhm_result result;

    if (!addr_ptr || size == 0 || uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    memset(&decommit_record, 0, sizeof(decommit_record));
    uhm_lock(&heap->lock);
    vma = uhm_find_vma_covering(heap, addr, 1u);
    if (!uhm_heap_is_accessible_pinned(heap) ||
        !uhm_page_span(heap, addr, size, &first, &count) ||
        !vma || vma->addr != addr ||
        uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
        vma->size != size || vma_end != end ||
        uhm_vma_commit_state_from_flags(vma->flags) != UHM_PAGE_COMMIT_LIVE ||
        !uhm_page_range_matches_descriptor(
            heap, addr, size, UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
            UHM_PAGE_COMMIT_LIVE, vma->prot, &vma->desc, 1, 1)) {
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    old_prot = vma->prot;
    old_flags = vma->flags & ~UHM_META_FLAG_EMERGENCY;
    old_descriptor_id = vma->desc.id;
    old_generation = vma->generation;
    free_node = uhm_free_extent_new(heap, addr, size, 1);
    if (!free_node) {
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return UHM_ERR_OUT_OF_MEMORY;
    }

    uhm_unlock(&heap->lock);
    decommit_record = uhm_vm_decommit_unlocked_call(heap, addr_ptr, size);
    uhm_lock(&heap->lock);
    result = uhm_vm_decommit_publish_record_locked(heap, &decommit_record);
    if (result == UHM_OK &&
        (!uhm_heap_is_accessible_pinned(heap) ||
         !uhm_mremap_vma_matches_exact_fields(
             vma, addr, size, end, old_prot, old_flags, old_descriptor_id,
             old_generation) ||
         uhm_vma_commit_state_from_flags(vma->flags) !=
             UHM_PAGE_COMMIT_LIVE ||
         !uhm_page_range_matches_descriptor(
             heap, addr, size, UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
             UHM_PAGE_COMMIT_LIVE, vma->prot, &vma->desc, 1, 1))) {
        result = UHM_ERR_CORRUPTION;
    }
    if (result != UHM_OK) {
        uhm_free_extent_release_node(heap, free_node);
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        return result;
    }

    {
        uhm_lockset publish_lockset;
        uint64_t free_generation = uhm_heap_generation_next(heap);

        uhm_page_lock_shard_range(heap, first, count, &publish_lockset);
        uhm_vma_lock_all_shards(heap, &publish_lockset);
        uhm_extent_lock_all_shards(heap, &publish_lockset);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, first, count, UHM_PAGE_OWNER_FREE,
            UHM_PAGE_VA_FREE_REUSABLE, UHM_PAGE_COMMIT_DECOMMITTED,
            UHM_PROT_NONE, 0, free_generation, &publish_lockset, 0);
        if (result == UHM_OK) {
            uhm_vma_remove(heap, vma);
            if (heap->stats.mmap_live_bytes >= size) {
                heap->stats.mmap_live_bytes -= size;
            } else {
                heap->stats.mmap_live_bytes = 0;
            }
            if (heap->stats.committed_bytes >= size) {
                heap->stats.committed_bytes -= size;
            } else {
                heap->stats.committed_bytes = 0;
            }
            insert_attempted = 1;
            result = uhm_mmap_free_insert_node(heap, free_node);
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result != UHM_OK) {
        if (!insert_attempted) {
            uhm_free_extent_release_node(heap, free_node);
        }
        (void)uhm_bump_failure_generation(heap);
    }
    uhm_unlock(&heap->lock);
    return result;
}

static uint32_t uhm_mremap_maymove_copy_prot(uint32_t source_prot) {
    uint32_t copy_prot = source_prot;

    if ((copy_prot & UHM_PROT_WRITE) == 0 ||
        (copy_prot & UHM_PROT_EXEC) != 0) {
        copy_prot = (source_prot | UHM_PROT_READ | UHM_PROT_WRITE) &
                    ~UHM_PROT_EXEC;
    }
    return copy_prot;
}

static uhm_result uhm_mremap_maymove_pinned(uhm_heap *heap,
                                            void **addr_inout,
                                            size_t old_size,
                                            size_t new_size,
                                            uint32_t flags) {
    void *old_addr;
    void *new_addr = 0;
    uintptr_t addr;
    uintptr_t old_end;
    size_t old_rounded;
    size_t new_rounded;
    size_t copy_size;
    uint32_t copy_prot;
    uhm_mremap_maymove_source_claim source_claim;
    uhm_result result;
    uhm_result cleanup_result;

    memset(&source_claim, 0, sizeof(source_claim));
    if ((flags & ~UHM_REMAP_MAYMOVE) != 0u) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    old_addr = addr_inout ? *addr_inout : 0;
    result = uhm_mremap_pinned(heap, addr_inout, old_size, new_size,
                               flags & ~UHM_REMAP_MAYMOVE);
    if (result == UHM_OK) {
        return UHM_OK;
    }
    if (!addr_inout || !old_addr ||
        (result != UHM_ERR_PROTECTION && result != UHM_ERR_NOT_MAPPED)) {
        return result;
    }
    addr = (uintptr_t)old_addr;
    if ((addr % heap->F.vm.page_size) != 0 ||
        uhm_align_up_size(old_size, heap->F.vm.page_size, &old_rounded) ||
        uhm_align_up_size(new_size, heap->F.vm.page_size, &new_rounded) ||
        old_rounded == 0 || new_rounded == 0 ||
        new_rounded <= old_rounded ||
        uhm_range_overflows(addr, old_rounded, &old_end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    result = uhm_mremap_maymove_claim_source(heap, addr, old_rounded,
                                             &source_claim);
    if (result != UHM_OK) {
        return result;
    }

    copy_prot = uhm_mremap_maymove_copy_prot(source_claim.prot);
    result = uhm_mmap_pinned(heap, &new_addr, new_rounded, copy_prot,
                             UHM_MAP_PRIVATE, UHM_NUMA_POLICY_DEFAULT,
                             heap->core.preferred_numa_node, 0);
    if (result != UHM_OK) {
        cleanup_result =
            uhm_mremap_maymove_restore_source_claim(heap, &source_claim);
        if (cleanup_result != UHM_OK) {
            return cleanup_result;
        }
        return result;
    }

    copy_size = old_rounded < new_rounded ? old_rounded : new_rounded;
    memcpy(new_addr, old_addr, copy_size);
    if (copy_prot != source_claim.prot) {
        result = uhm_mprotect_pinned(heap, new_addr, new_rounded,
                                     source_claim.prot);
        if (result != UHM_OK) {
            cleanup_result =
                uhm_mremap_maymove_rollback_new_mapping(heap, new_addr,
                                                        new_rounded);
            (void)cleanup_result;
            cleanup_result =
                uhm_mremap_maymove_restore_source_claim(heap, &source_claim);
            if (cleanup_result != UHM_OK) {
                return cleanup_result;
            }
            *addr_inout = old_addr;
            return result;
        }
    }

    result = uhm_mremap_maymove_release_source_claim(heap, &source_claim);
    if (result != UHM_OK) {
        cleanup_result =
            uhm_mremap_maymove_rollback_new_mapping(heap, new_addr,
                                                    new_rounded);
        (void)cleanup_result;
        if (source_claim.claimed) {
            cleanup_result =
                uhm_mremap_maymove_restore_source_claim(heap,
                                                        &source_claim);
            if (cleanup_result != UHM_OK) {
                return cleanup_result;
            }
        }
        *addr_inout = old_addr;
        return result;
    }
    *addr_inout = new_addr;
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_mremap(uhm_heap *heap, void **addr_inout,
                                          size_t old_size, size_t new_size,
                                          uint32_t flags) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_CORRUPTION;
    }
    if ((flags & UHM_REMAP_MAYMOVE) != 0u) {
        result = uhm_mremap_maymove_pinned(heap, addr_inout, old_size,
                                           new_size, flags);
    } else {
        result = uhm_mremap_pinned(heap, addr_inout, old_size, new_size,
                                   flags);
    }
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
    }
    uhm_heap_op_end(heap, op_shard);
    return result;
}

