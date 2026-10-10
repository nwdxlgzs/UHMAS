static int uhm_check_page_addr_in_range(uintptr_t page_addr,
                                        uintptr_t range_addr,
                                        size_t range_size) {
    uintptr_t range_end;

    return range_size != 0 &&
           !uhm_range_overflows(range_addr, range_size, &range_end) &&
           page_addr >= range_addr && page_addr < range_end;
}

static int uhm_check_malloc_block_page_records(uhm_heap *heap,
                                               const uhm_block *block,
                                               size_t *retained_size_out) {
    uintptr_t decommit_addr = 0;
    size_t decommit_size = 0;
    uintptr_t retained_addr = 0;
    size_t retained_size = 0;
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    size_t page_size;
    int has_decommitted;
    int has_retained;
    int ok = 1;

    if (retained_size_out) {
        *retained_size_out = 0;
    }
    if (!heap || !block || block->magic != UHM_BLOCK_MAGIC ||
        !uhm_page_span(heap, (uintptr_t)block, block->total_size, &first,
                       &count)) {
        return 0;
    }
    has_decommitted = (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0;
    has_retained = (block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0;
    if (has_decommitted &&
        !uhm_block_decommit_range(heap, block, &decommit_addr,
                                  &decommit_size)) {
        return 0;
    }
    if (has_retained &&
        !uhm_block_decommit_range(heap, block, &retained_addr,
                                  &retained_size)) {
        return 0;
    }

    page_size = heap->F.vm.page_size;
    cursor = first;
    end = first + count;
    while (cursor < end && ok) {
        size_t scan_count = end - cursor;
        size_t scan_end;
        size_t decommitted_pages = 0;
        size_t retained_pages = 0;
        uhm_lockset lockset;

        if (scan_count > UHM_PAGE_RANGE_DIAG_PAGE_BUDGET) {
            scan_count = UHM_PAGE_RANGE_DIAG_PAGE_BUDGET;
        }
        scan_end = cursor + scan_count;
        uhm_diag_note_page_range_query(heap, scan_count, cursor,
                                       UHM_PAGE_QUERY_PHASE_HAS_OWNER);
        uhm_page_lock_shard_range(heap, cursor, scan_count, &lockset);
        while (cursor < scan_end && ok) {
            uhm_page_leaf *leaf;
            size_t offset;
            size_t chunk;
            size_t i;

            if (!uhm_page_leaf_chunk_unlocked(heap, cursor, scan_end, &leaf,
                                              &offset, &chunk) ||
                !leaf) {
                ok = 0;
                break;
            }
            for (i = 0; i < chunk; ++i) {
                const uhm_page_record *record = &leaf->records[offset + i];
                uintptr_t page_addr =
                    heap->core.arena_base + (cursor + i) * page_size;
                int in_decommitted =
                    has_decommitted &&
                    uhm_check_page_addr_in_range(page_addr, decommit_addr,
                                                 decommit_size);
                int in_retained =
                    has_retained &&
                    uhm_check_page_addr_in_range(page_addr, retained_addr,
                                                 retained_size);

                if (record->owner != UHM_PAGE_OWNER_MALLOC) {
                    ok = 0;
                    break;
                }
                if (in_decommitted) {
                    ++decommitted_pages;
                    if (record->va_state != UHM_PAGE_VA_MALLOC_LIVE ||
                        record->commit_state !=
                            UHM_PAGE_COMMIT_DECOMMITTED ||
                        record->prot != UHM_PROT_NONE ||
                        record->generation == 0 ||
                        (record->descriptor_id != block->desc.id &&
                         !uhm_malloc_decommitted_record_descriptor_in_block(
                             block, record->descriptor_id))) {
                        ok = 0;
                        break;
                    }
                } else if (in_retained) {
                    ++retained_pages;
                    if (record->va_state != UHM_PAGE_VA_MALLOC_LIVE ||
                        record->commit_state != UHM_PAGE_COMMIT_LIVE ||
                        record->prot !=
                            (UHM_PROT_READ | UHM_PROT_WRITE)) {
                        ok = 0;
                        break;
                    }
                }
            }
            cursor += chunk;
        }
        uhm_page_unlock_shards(heap, &lockset);
        if (decommitted_pages != 0) {
            uhm_diag_note_page_range_query(
                heap, decommitted_pages, scan_end - scan_count,
                UHM_PAGE_QUERY_PHASE_MATCHES_DESCRIPTOR);
        }
        if (retained_pages != 0) {
            uhm_diag_note_page_range_query(
                heap, retained_pages, scan_end - scan_count,
                UHM_PAGE_QUERY_PHASE_HAS_STATE);
        }
    }
    if (!ok) {
        return 0;
    }
    if (retained_size_out && has_retained) {
        *retained_size_out = retained_size;
    }
    return 1;
}

static uhm_result uhm_check_pinned(uhm_heap *heap, uint32_t check_flags) {
    uhm_block *block;
    uhm_slab *slab;
    uhm_vma *vma;
    uhm_free_extent *free_extent;
    uhm_control_extent *control_extent;
    size_t primary_control_size;
    size_t list_count;
    size_t tree_count;
    size_t busy_free_count = 0;
    size_t control_extent_count = 0;
    size_t retained_bytes = 0;
    size_t slab_all_count = 0;
    size_t slab_state_total = 0;
    size_t malloc_phys_seen = 0;
    size_t malloc_phys_limit = 0;
    size_t slab_expected_partial[UHM_SIZE_CLASS_COUNT];
    size_t slab_expected_full[UHM_SIZE_CLASS_COUNT];
    size_t slab_expected_empty[UHM_SIZE_CLASS_COUNT];
    size_t class_index;
    size_t hint_index;
    uhm_block *prev_phys = 0;
    uhm_free_extent *prev_free_extent = 0;

    if (!uhm_heap_is_accessible_pinned(heap)) {
        return UHM_ERR_CORRUPTION;
    }
    memset(slab_expected_partial, 0, sizeof(slab_expected_partial));
    memset(slab_expected_full, 0, sizeof(slab_expected_full));
    memset(slab_expected_empty, 0, sizeof(slab_expected_empty));
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if ((check_flags & UHM_CHECK_FAIL_ON_TREAP_HEALTH) != 0 &&
        heap->core.treap_health_flags != 0) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if ((check_flags & UHM_CHECK_FAIL_ON_LIVE_BLOCK_HEALTH) != 0 &&
        heap->core.live_block_health_flags != 0) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if ((heap->core.live_block_health_flags &
         ~UHM_LIVE_BLOCK_HEALTH_METADATA_DRIFT) != 0) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (heap->core.arena_base >= heap->core.arena_end ||
        heap->core.control_end > heap->core.malloc_hi ||
        heap->core.malloc_hi > heap->core.mmap_lo ||
        heap->core.mmap_lo > heap->core.arena_end) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    malloc_phys_limit =
        (size_t)((heap->core.arena_end - heap->core.arena_base) /
                 UHM_MIN_ALIGNMENT) +
        1u;
    if (heap->core.low_watermark_claim_inflight != 0u) {
        uintptr_t low_claim_end;
        size_t active_ops =
            uhm_heap_active_ops_load(heap);
        if (active_ops <= 1u ||
            heap->core.low_watermark_claim_size == 0 ||
            uhm_range_overflows(heap->core.low_watermark_claim_addr,
                                heap->core.low_watermark_claim_size,
                                &low_claim_end) ||
            heap->core.low_watermark_claim_addr < heap->core.control_end ||
            low_claim_end > heap->core.malloc_hi ||
            low_claim_end > heap->core.mmap_lo) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
    }
    if (!uhm_thread_ctx_list_validate(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_tls_cache_list_validate(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_metadata_quarantine_validate(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (uhm_metadata_hazard_readers_load(heap) == SIZE_MAX) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_lockless_remote_readers_validate_locked(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_metadata_registered_readers_valid_for_check(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_page_map_validate(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    primary_control_size = (size_t)(heap->core.control_end - heap->core.control_base);
    if (!uhm_extent_descriptor_matches(&heap->core.primary_control_desc,
                                       UHM_EXTENT_KIND_CONTROL,
                                       heap->core.control_base,
                                       primary_control_size,
                                       UHM_PAGE_OWNER_CONTROL,
                                       UHM_PAGE_VA_CONTROL_LIVE,
                                       UHM_PAGE_COMMIT_LIVE,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       heap->core.primary_control_desc.generation,
                                       heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_page_range_matches_descriptor(heap, heap->core.control_base,
                                           primary_control_size,
                                           UHM_PAGE_OWNER_CONTROL,
                                           UHM_PAGE_VA_CONTROL_LIVE,
                                           UHM_PAGE_COMMIT_LIVE,
                                           UHM_PROT_READ | UHM_PROT_WRITE,
                                           &heap->core.primary_control_desc,
                                           1, 1)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    for (control_extent = heap->core.control_extents; control_extent;
         control_extent = control_extent->next) {
        ++control_extent_count;
        if (control_extent->magic != UHM_CONTROL_MAGIC ||
            control_extent->base >= control_extent->end ||
            control_extent->alloc < control_extent->base ||
            control_extent->alloc > control_extent->end ||
            (control_extent->flags & ~UHM_CONTROL_EXTENT_HINT_MASK) != 0 ||
            ((control_extent->flags & UHM_CONTROL_EXTENT_FLAG_HINTED) != 0 &&
             !uhm_control_alloc_hint_position_matches(heap, control_extent)) ||
            !uhm_extent_descriptor_matches(&control_extent->desc,
                                           UHM_EXTENT_KIND_CONTROL,
                                           control_extent->base,
                                           (size_t)(control_extent->end -
                                                    control_extent->base),
                                           UHM_PAGE_OWNER_CONTROL,
                                           UHM_PAGE_VA_CONTROL_LIVE,
                                           UHM_PAGE_COMMIT_LIVE,
                                           UHM_PROT_READ | UHM_PROT_WRITE,
                                           control_extent->desc.generation,
                                           control_extent) ||
            !uhm_page_range_matches_descriptor(
                heap, control_extent->base,
                (size_t)(control_extent->end - control_extent->base),
                UHM_PAGE_OWNER_CONTROL, UHM_PAGE_VA_CONTROL_LIVE,
                UHM_PAGE_COMMIT_LIVE, UHM_PROT_READ | UHM_PROT_WRITE,
                &control_extent->desc, 1, 1)) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
    }
    if (heap->core.control_extent_count != control_extent_count ||
        heap->core.control_no_fit_extent_count > control_extent_count) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (heap->core.control_recent_extent &&
        !uhm_control_extent_list_contains(heap->core.control_extents,
                                          heap->core.control_recent_extent,
                                          SIZE_MAX)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    for (hint_index = 0; hint_index < UHM_CONTROL_ALLOC_HINT_BUCKETS;
         ++hint_index) {
        size_t hint_slot;
        int seen_empty = 0;
        for (hint_slot = 0; hint_slot < UHM_CONTROL_ALLOC_HINT_SLOTS;
             ++hint_slot) {
            uhm_control_extent *hint =
                heap->core.control_alloc_hint[hint_index][hint_slot];
            if (!hint) {
                seen_empty = 1;
                continue;
            }
            if (seen_empty ||
                !uhm_control_extent_list_contains(
                    heap->core.control_extents, hint, SIZE_MAX) ||
                !uhm_control_alloc_hint_position_matches(heap, hint)) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
        }
    }
    tree_count = 0;
    if (!uhm_malloc_free_tree_validate(heap, heap->core.malloc_free_tree_root, 0,
                                       &tree_count)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    list_count = 0;
    for (block = heap->core.malloc_free_head; block; block = block->next_free) {
        ++list_count;
        if ((block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0) {
            ++busy_free_count;
        }
    }
    if (tree_count != list_count &&
        (uhm_heap_active_ops_load(heap) <= 1u ||
         tree_count + busy_free_count != list_count)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    tree_count = 0;
    if (!uhm_live_block_tree_validate(heap, heap->core.live_block_tree_root, 0,
                                      &tree_count)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (heap->core.live_block_tree_current_nodes != tree_count) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }

    list_count = 0;
    for (block = heap->core.first_block; block; block = block->next_phys) {
        uintptr_t block_addr = (uintptr_t)block;
        uintptr_t block_end;
        size_t block_retained_size = 0;

        if (++malloc_phys_seen > malloc_phys_limit ||
            block_addr < heap->core.arena_base ||
            block_addr >= heap->core.arena_end ||
            (block_addr % alignof(uhm_block)) != 0 ||
            (prev_phys && block_addr <= (uintptr_t)prev_phys) ||
            block->prev_phys != prev_phys ||
            (prev_phys && prev_phys->next_phys != block) ||
            block->total_size < uhm_malloc_header_size() ||
            uhm_range_overflows(block_addr, block->total_size, &block_end) ||
            block_end > heap->core.arena_end) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        if (block->magic != UHM_BLOCK_MAGIC) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        if ((block->flags & ~(UHM_BLOCK_FLAG_FREE |
                              UHM_BLOCK_FLAG_PAGE_PRIVATE |
                              UHM_BLOCK_FLAG_DECOMMITTED |
                              UHM_BLOCK_FLAG_DECOMMIT_RETAINED |
                              UHM_BLOCK_FLAG_VM_BUSY |
                              UHM_BLOCK_FLAG_DECOMMIT_HOT |
                              UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN)) != 0 ||
            ((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0 &&
             (block->flags & UHM_BLOCK_FLAG_FREE) == 0) ||
            ((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0 &&
             (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) != 0) ||
            ((block->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0 &&
             ((block->flags & UHM_BLOCK_FLAG_FREE) == 0 ||
              (block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0 ||
              (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) != 0)) ||
            ((block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0 &&
             ((block->flags & UHM_BLOCK_FLAG_FREE) == 0 ||
              (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) != 0)) ||
            ((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0 &&
             ((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0 ||
              (block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0))) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        if (!uhm_extent_descriptor_matches(&block->desc,
                                           UHM_EXTENT_KIND_MALLOC,
                                           (uintptr_t)block,
                                           block->total_size,
                                           UHM_PAGE_OWNER_MALLOC,
                                           UHM_PAGE_VA_MALLOC_LIVE,
                                           UHM_PAGE_COMMIT_LIVE,
                                           UHM_PROT_READ | UHM_PROT_WRITE,
                                           block->generation,
                                           block)) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        if (!uhm_check_malloc_block_page_records(heap, block,
                                                 &block_retained_size)) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        if (block_retained_size != 0) {
            if (retained_bytes > SIZE_MAX - block_retained_size) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            retained_bytes += block_retained_size;
        }
        if ((block->flags & UHM_BLOCK_FLAG_FREE) != 0) {
            if (block->live_tree_parent || block->live_tree_left ||
                block->live_tree_right ||
                heap->core.live_block_tree_root == block ||
                ((block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0 &&
                 (block->free_tree_parent || block->free_tree_left ||
                  block->free_tree_right ||
                  heap->core.malloc_free_tree_root == block))) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
        } else {
            ++list_count;
        }
        if ((block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) != 0 &&
            (block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
            block->requested_size != 0) {
            int page_private_records_ok = uhm_page_range_matches_descriptor(
                heap, (uintptr_t)uhm_block_payload(block),
                block->requested_size, UHM_PAGE_OWNER_MALLOC,
                UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
                UHM_PROT_READ | UHM_PROT_WRITE, &block->desc, 0, 0);
            if (!page_private_records_ok &&
                uhm_heap_active_ops_load(heap) > 1u) {
                page_private_records_ok =
                    uhm_page_range_matches_descriptor_live_or_protecting_relaxed(
                        heap, (uintptr_t)uhm_block_payload(block),
                        block->requested_size, UHM_PAGE_OWNER_MALLOC,
                        UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_VA_PROTECTING,
                        UHM_PAGE_COMMIT_LIVE, &block->desc);
            }
            if (!page_private_records_ok) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
        }
        prev_phys = block;
    }
    if (prev_phys != heap->core.last_block) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (tree_count != list_count) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (retained_bytes != heap->core.malloc_free_decommit_retained_bytes) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (heap->core.malloc_free_decommit_retention_base_high_water_bytes >
            heap->core.malloc_free_decommit_retention_high_water_bytes ||
        heap->core.malloc_free_decommit_retention_high_water_bytes >
            heap->core.malloc_free_decommit_retention_max_high_water_bytes ||
        retained_bytes >
            heap->core.malloc_free_decommit_retention_high_water_bytes) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }

    {
        uhm_slab *prev_all = 0;
        for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
            size_t live_slots = 0;
            size_t free_slots = 0;
            size_t free_list_count = 0;
            size_t ctx_cache_count = 0;
            size_t ctx_cache_list_count = 0;
            size_t remote_free_count = 0;
            size_t remote_free_list_count = 0;
            size_t remote_class_queue_count = 0;
            size_t remote_ready_queue_count = 0;
            size_t remote_publish_count = 0;
            size_t remote_unaccounted_count = 0;
            size_t remote_draining_count = 0;
            size_t remote_accounted_out_count = 0;
            size_t remote_accounted_out_publish_count = 0;
            size_t remote_detached_link_hazard_count = 0;
            size_t expected_stride = 0;
            size_t expected_slots;
            size_t i;
            uintptr_t slab_end;
            uint16_t expected_state;
            uint32_t expected_commit_state;
            uint32_t expected_prot;
            uint32_t remote_ready_state;
            uint64_t remote_ready_seq_before;
            uint64_t remote_ready_seq_after;
            uint64_t state_transition_seq_before;
            uint64_t state_transition_seq_after;
            int remote_ready_update_window;
            int state_transition_window;
            int remote_ready_scanner_protected = 1;
            int slab_decommitted;
            int slab_busy;
            uhm_slab_slot *remote_slot;
            uhm_tagged_freelist_node *remote_link;

            if (slab_all_count >= heap->core.page_count ||
                slab->prev_all != prev_all ||
                uhm_range_overflows(slab->base, slab->size, &slab_end)) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            if (slab->magic != UHM_SLAB_MAGIC ||
                (slab->flags &
                 ~(UHM_SLAB_FLAG_DECOMMITTED |
                   UHM_SLAB_FLAG_VM_BUSY)) != 0 ||
                slab->size_class >= UHM_SIZE_CLASS_COUNT ||
                !uhm_tagged_freelist_epoch_valid_for_check(
                    heap, &slab->remote_free_lf) ||
                slab->base < heap->core.control_end ||
                slab_end > heap->core.mmap_lo ||
                slab->size != heap->F.vm.page_size ||
                slab->slot_size != uhm_size_classes[slab->size_class] ||
                slab->slot_size > UHM_SLAB_MAX_SLOT_SIZE ||
                uhm_add_overflows_size(uhm_slab_slot_header_size(),
                                       slab->slot_size, &expected_stride) ||
                uhm_align_up_size(expected_stride, UHM_MIN_ALIGNMENT,
                                  &expected_stride)) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            expected_slots = slab->size / expected_stride;
            slab_decommitted =
                (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0;
            slab_busy = (slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0;
            if (slab_busy &&
                (uhm_heap_active_ops_load(heap) <= 1u ||
                 slab->state != UHM_SLAB_STATE_EMPTY ||
                 slab->live_slots != 0 ||
                 slab->free_slots != slab->total_slots ||
                 slab->ctx_cached_slots != 0)) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            expected_commit_state = slab_decommitted
                                        ? UHM_PAGE_COMMIT_DECOMMITTED
                                        : UHM_PAGE_COMMIT_LIVE;
            expected_prot = slab_decommitted
                                ? UHM_PROT_NONE
                                : (UHM_PROT_READ | UHM_PROT_WRITE);
            if (slab->slot_stride != expected_stride ||
                slab->total_slots != expected_slots ||
                slab->total_slots == 0 ||
                slab->total_slots * slab->slot_stride > slab->size ||
                !uhm_extent_descriptor_matches(&slab->desc,
                                               UHM_EXTENT_KIND_MALLOC,
                                               slab->base,
                                               slab->size,
                                               UHM_PAGE_OWNER_MALLOC,
                                               UHM_PAGE_VA_MALLOC_LIVE,
                                               expected_commit_state,
                                               expected_prot,
                                               slab->generation,
                                               slab) ||
                !uhm_page_range_matches_descriptor(
                    heap, slab->base, slab->size, UHM_PAGE_OWNER_MALLOC,
                    UHM_PAGE_VA_MALLOC_LIVE, expected_commit_state,
                    expected_prot, &slab->desc, 1, 1)) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            state_transition_seq_before =
                uhm_slab_state_transition_seq_load(slab);
            if (!uhm_check_remote_class_queue_count_for_slab(
                    heap, slab, &remote_class_queue_count)) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            remote_ready_seq_before =
                uhm_slab_remote_ready_seq_load(heap, slab->size_class);
            if (!uhm_check_remote_ready_queue_count_for_slab(
                    heap, slab, &remote_ready_queue_count)) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            remote_ready_state = atomic_load_explicit(
                &slab->remote_ready_queued, UHM_ATOMIC_ACQUIRE);
            remote_ready_seq_after =
                uhm_slab_remote_ready_seq_load(heap, slab->size_class);
            remote_ready_update_window =
                uhm_heap_active_ops_load(heap) > 1u &&
                (remote_ready_seq_before != remote_ready_seq_after ||
                 uhm_slab_remote_ready_seq_active_value(
                     remote_ready_seq_before) ||
                 uhm_slab_remote_ready_seq_active_value(
                     remote_ready_seq_after));
            if (remote_ready_state == UHM_SLAB_REMOTE_READY_SCANNING) {
                remote_ready_scanner_protected =
                    uhm_lockless_remote_reader_hazards_slab_and_scanner_node_locked(
                        heap, slab, !slab_decommitted);
            }
            if (remote_ready_state > UHM_SLAB_REMOTE_READY_SCANNING ||
                remote_ready_queue_count > 1u ||
                (remote_ready_queue_count != 0 &&
                 remote_ready_state != UHM_SLAB_REMOTE_READY_QUEUED) ||
                (remote_ready_state == UHM_SLAB_REMOTE_READY_QUEUED &&
                 remote_ready_queue_count == 0 &&
                 !remote_ready_update_window) ||
                (remote_ready_state == UHM_SLAB_REMOTE_READY_SCANNING &&
                 (uhm_heap_active_ops_load(heap) <= 1u ||
                  !remote_ready_scanner_protected))) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            if (slab_decommitted) {
                if (slab->state != UHM_SLAB_STATE_EMPTY ||
                    slab->live_slots != 0 ||
                    slab->free_slots != slab->total_slots ||
                    slab->ctx_cached_slots != 0 ||
                    !uhm_slab_remote_counters_valid_for_check(heap, slab) ||
                    uhm_slab_remote_drain_active(slab) ||
                    uhm_slab_remote_drain_epoch(slab) != 0 ||
                    uhm_slab_remote_pending_load(slab) != 0 ||
                    uhm_slab_remote_inflight_load(slab) != 0 ||
                    remote_class_queue_count != 0 ||
                    !((remote_ready_state == UHM_SLAB_REMOTE_READY_IDLE &&
                       remote_ready_queue_count == 0) ||
                      (remote_ready_state == UHM_SLAB_REMOTE_READY_QUEUED &&
                       remote_ready_queue_count == 1u)) ||
                    uhm_slab_free_head_snapshot(slab) != 0 ||
                    !uhm_slab_remote_head_empty_snapshot(slab) ||
                    !uhm_slab_remote_lf_empty(slab)) {
                    uhm_unlock(&heap->lock);
                    return UHM_ERR_CORRUPTION;
                }
                free_slots = slab->free_slots;
            } else {
                if (!uhm_check_slab_free_list_count(slab, slab_end,
                                                    &free_list_count)) {
                    uhm_unlock(&heap->lock);
                    return UHM_ERR_CORRUPTION;
                }
                remote_link = uhm_tagged_freelist_unpack_ptr(
                    atomic_load_explicit(&slab->remote_free_lf.head,
                                         UHM_ATOMIC_ACQUIRE));
                while (remote_link) {
                    uhm_slab_slot *remote_lf_slot =
                        uhm_slab_slot_from_remote_link(remote_link);
                    uintptr_t slot_addr = (uintptr_t)remote_lf_slot;
                    size_t offset;
                    uint32_t remote_flags;
                    if (remote_free_list_count >= slab->total_slots ||
                        !remote_lf_slot ||
                        slot_addr < slab->base ||
                        slot_addr >= slab_end) {
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                    offset = (size_t)(slot_addr - slab->base);
                    if ((offset % slab->slot_stride) != 0 ||
                        remote_lf_slot->magic != UHM_SLAB_SLOT_MAGIC ||
                        !uhm_slab_slot_belongs_to(remote_lf_slot, slab) ||
                        remote_lf_slot->size_class != slab->size_class ||
                        remote_lf_slot->state != 0 ||
                        !uhm_slab_slot_state_mirror_valid_for_remote(
                            remote_lf_slot)) {
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                    remote_flags = uhm_slab_slot_flags_load(remote_lf_slot);
                    if ((remote_flags &
                         ~(UHM_SLAB_SLOT_FLAG_FREE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                           UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) != 0 ||
                        (remote_flags &
                         (UHM_SLAB_SLOT_FLAG_FREE |
                          UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
                            (UHM_SLAB_SLOT_FLAG_FREE |
                             UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) ||
                        uhm_slab_slot_requested_load(remote_lf_slot) != 0) {
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                    remote_link = uhm_tagged_freelist_next_load(remote_link);
                    ++remote_free_list_count;
                }
                uhm_slab_raw_lock(slab);
                for (remote_slot = slab->remote_free_head; remote_slot;
                     remote_slot = remote_slot->next_free) {
                    uintptr_t slot_addr = (uintptr_t)remote_slot;
                    size_t offset;
                    uint32_t remote_flags;
                    if (remote_free_list_count >= slab->total_slots ||
                        slot_addr < slab->base ||
                        slot_addr >= slab_end) {
                        uhm_slab_raw_unlock(slab);
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                    offset = (size_t)(slot_addr - slab->base);
                    if ((offset % slab->slot_stride) != 0 ||
                        remote_slot->magic != UHM_SLAB_SLOT_MAGIC ||
                        !uhm_slab_slot_belongs_to(remote_slot, slab) ||
                        remote_slot->size_class != slab->size_class ||
                        remote_slot->state != 0 ||
                        !uhm_slab_slot_state_mirror_valid_for_remote(
                            remote_slot)) {
                        uhm_slab_raw_unlock(slab);
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                    remote_flags = uhm_slab_slot_flags_load(remote_slot);
                    if ((remote_flags &
                         ~(UHM_SLAB_SLOT_FLAG_FREE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                           UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) != 0 ||
                        (remote_flags &
                         (UHM_SLAB_SLOT_FLAG_FREE |
                          UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
                            (UHM_SLAB_SLOT_FLAG_FREE |
                             UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) ||
                         uhm_slab_slot_requested_load(remote_slot) != 0) {
                        uhm_slab_raw_unlock(slab);
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                    ++remote_free_list_count;
                }
                uhm_slab_raw_unlock(slab);
                if (remote_free_list_count >
                    SIZE_MAX - remote_class_queue_count) {
                    uhm_unlock(&heap->lock);
                    return UHM_ERR_CORRUPTION;
                }
                remote_free_list_count += remote_class_queue_count;
                for (i = 0; i < slab->total_slots; ++i) {
                    uhm_slab_slot *slot =
                        (uhm_slab_slot *)(slab->base + i * slab->slot_stride);
                    uint32_t slot_flags;
                    uint32_t slot_requested;
                    if (slot->magic != UHM_SLAB_SLOT_MAGIC ||
                        !uhm_slab_slot_belongs_to(slot, slab) ||
                        slot->size_class != slab->size_class ||
                        slot->state != 0 ||
                        !uhm_slab_slot_state_valid_for_check(slot)) {
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                    slot_flags = uhm_slab_slot_flags_load(slot);
                    slot_requested = uhm_slab_slot_requested_load(slot);
                    if ((slot_flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                                        UHM_SLAB_SLOT_FLAG_CTX_CACHE |
                                        UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                                        UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                                        UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED |
                                        UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                                        UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT |
                                        UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED)) != 0 ||
                        ((slot_flags & UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED) != 0 &&
                         slot_flags != UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED) ||
                        ((slot_flags & UHM_SLAB_SLOT_FLAG_CTX_CACHE) != 0 &&
                         (slot_flags & UHM_SLAB_SLOT_FLAG_FREE) == 0) ||
                        ((slot_flags & UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) != 0 &&
                         ((slot_flags & UHM_SLAB_SLOT_FLAG_FREE) == 0 ||
                          (slot_flags & UHM_SLAB_SLOT_FLAG_CTX_CACHE) != 0)) ||
                        ((slot_flags &
                          UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) != 0 &&
                         (slot_flags &
                          (UHM_SLAB_SLOT_FLAG_FREE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
                             (UHM_SLAB_SLOT_FLAG_FREE |
                              UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) ||
                        ((slot_flags &
                          UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0 &&
                         (slot_flags &
                          (UHM_SLAB_SLOT_FLAG_FREE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
                             (UHM_SLAB_SLOT_FLAG_FREE |
                              UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) ||
                        ((slot_flags &
                          UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) != 0 &&
                         (slot_flags &
                          (UHM_SLAB_SLOT_FLAG_FREE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
                             (UHM_SLAB_SLOT_FLAG_FREE |
                              UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) ||
                        ((slot_flags &
                          UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) != 0 &&
                         (slot_flags &
                          (UHM_SLAB_SLOT_FLAG_FREE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING)) !=
                             (UHM_SLAB_SLOT_FLAG_FREE |
                              UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                              UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING))) {
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                    if ((slot_flags & UHM_SLAB_SLOT_FLAG_FREE) != 0) {
                        if (slot_requested != 0) {
                            uhm_unlock(&heap->lock);
                            return UHM_ERR_CORRUPTION;
                        }
                        if ((slot_flags & UHM_SLAB_SLOT_FLAG_CTX_CACHE) != 0) {
                            ++ctx_cache_count;
                            ++live_slots;
                        }
                        if ((slot_flags & UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) != 0) {
                            ++remote_free_count;
                        }
                        if ((slot_flags &
                             UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) != 0) {
                            ++remote_publish_count;
                        }
                        if ((slot_flags &
                             UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0) {
                            ++remote_unaccounted_count;
                        }
                        if ((slot_flags &
                             UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) != 0) {
                            ++remote_draining_count;
                            if (uhm_lockless_remote_reader_hazards_slab_and_node_locked(
                                    heap, slab, &slot->remote_link)) {
                                ++remote_detached_link_hazard_count;
                            }
                        }
                        if ((slot_flags &
                             UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) != 0) {
                            ++remote_accounted_out_count;
                            if ((slot_flags &
                                 UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) == 0 &&
                                uhm_lockless_remote_reader_hazards_slab_and_node_locked(
                                    heap, slab, &slot->remote_link)) {
                                ++remote_detached_link_hazard_count;
                            }
                            if ((slot_flags &
                                 UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) != 0) {
                                ++remote_accounted_out_publish_count;
                            }
                        }
                        if ((slot_flags & UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0) {
                            ++free_slots;
                        }
                    } else {
                        ++live_slots;
                        if (slot_requested == 0 ||
                            slot_requested > slab->slot_size) {
                            uhm_unlock(&heap->lock);
                            return UHM_ERR_CORRUPTION;
                        }
                    }
                }
                {
                    size_t remote_pending_count =
                        uhm_slab_remote_pending_load(slab);
                    size_t remote_inflight_count =
                        uhm_slab_remote_inflight_load(slab);
                    uint32_t remote_drain_state =
                        uhm_slab_remote_drain_state(slab);
                    int remote_drain_active = remote_drain_state != 0u;
                    uint64_t remote_drain_epoch =
                        uhm_slab_remote_drain_epoch(slab);
                    int remote_drain_has_reader =
                        remote_drain_active &&
                        uhm_lockless_remote_reader_hazards_slab_locked(
                            heap, slab);
                    size_t accounted_remote_free_count =
                        remote_free_count >= remote_accounted_out_count
                            ? remote_free_count - remote_accounted_out_count
                            : SIZE_MAX;
                    size_t accounted_remote_publish_count =
                        remote_publish_count >=
                                remote_accounted_out_publish_count
                            ? remote_publish_count -
                                  remote_accounted_out_publish_count
                            : SIZE_MAX;
                    size_t remote_pending_gap =
                        remote_pending_count > accounted_remote_free_count
                            ? remote_pending_count -
                                  accounted_remote_free_count
                            : 0u;
                    size_t remote_inflight_gap =
                        remote_inflight_count > accounted_remote_publish_count
                            ? remote_inflight_count -
                                  accounted_remote_publish_count
                            : 0u;
                    size_t active_ops = uhm_heap_active_ops_load(heap);
                    int remote_counter_update_active =
                        uhm_slab_remote_counter_update_active(slab);
                    int remote_gap_protected =
                        remote_counter_update_active ||
                        uhm_lockless_remote_reader_hazards_slab_locked(heap,
                                                                       slab);
                    size_t remote_detached_count =
                        remote_draining_count + remote_accounted_out_count;
                    int remote_drain_epoch_valid =
                        remote_drain_state ==
                                UHM_REMOTE_DRAIN_OWNER_TRANSIENT
                            ? active_ops > 1u
                            : remote_drain_active
                                  ? (remote_drain_state ==
                                         UHM_REMOTE_DRAIN_OWNER_ACTIVE &&
                                     remote_drain_epoch != 0 &&
                                     remote_drain_epoch <=
                                         heap->core.metadata_epoch)
                                  : (remote_drain_epoch == 0 ||
                                     active_ops > 1u);

                    if (!remote_drain_epoch_valid ||
                        (remote_drain_active &&
                         (active_ops <= 1u || !remote_drain_has_reader)) ||
                        !uhm_ctx_cache_count_for_slab(
                            heap, slab, &ctx_cache_list_count) ||
                        ctx_cache_count != ctx_cache_list_count ||
                        ctx_cache_count != slab->ctx_cached_slots ||
                        remote_free_list_count >
                            accounted_remote_free_count ||
                        remote_free_count < remote_accounted_out_count ||
                        remote_publish_count <
                            remote_accounted_out_publish_count ||
                        remote_accounted_out_publish_count >
                            remote_accounted_out_count ||
                        remote_free_count - remote_free_list_count >
                            remote_publish_count + remote_draining_count ||
                        !uhm_slab_remote_counters_valid_for_check(heap,
                                                                  slab) ||
                        accounted_remote_free_count > remote_pending_count ||
                        accounted_remote_publish_count >
                            remote_inflight_count ||
                        remote_pending_gap > remote_inflight_gap ||
                        (remote_pending_gap != 0 &&
                         (active_ops <= 1u || !remote_gap_protected)) ||
                        (remote_inflight_gap != 0 &&
                         (active_ops <= 1u || !remote_gap_protected)) ||
                        remote_unaccounted_count > remote_free_count ||
                        remote_unaccounted_count > remote_publish_count ||
                        (remote_draining_count != 0 &&
                         (!remote_drain_active ||
                          !remote_drain_has_reader ||
                          remote_detached_link_hazard_count == 0 ||
                          active_ops <= 1u)) ||
                        (remote_accounted_out_count != 0 &&
                         (!remote_drain_active ||
                          !remote_drain_has_reader ||
                          remote_detached_link_hazard_count == 0 ||
                          active_ops <= 1u)) ||
                        (remote_detached_count != 0 &&
                         remote_detached_link_hazard_count == 0) ||
                        free_list_count + remote_free_count -
                                remote_unaccounted_count !=
                            slab->free_slots) {
                        uhm_unlock(&heap->lock);
                        return UHM_ERR_CORRUPTION;
                    }
                }
            }
            if (remote_unaccounted_count > free_slots ||
                live_slots + remote_unaccounted_count != slab->live_slots ||
                free_slots - remote_unaccounted_count != slab->free_slots ||
                live_slots + free_slots != slab->total_slots) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            if (!uhm_slab_summary_valid_for_check(heap, slab)) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            if (uhm_slab_raw_publish_active(slab) &&
                uhm_heap_active_ops_load(heap) <= 1u) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            state_transition_seq_after =
                uhm_slab_state_transition_seq_load(slab);
            {
                size_t active_ops = uhm_heap_active_ops_load(heap);
                int state_transition_active =
                    uhm_slab_state_transition_seq_active_value(
                        state_transition_seq_before) ||
                    uhm_slab_state_transition_seq_active_value(
                        state_transition_seq_after);
                int state_transition_observed =
                    state_transition_seq_before !=
                        state_transition_seq_after ||
                    state_transition_active;
                if (state_transition_active && active_ops <= 1u) {
                    uhm_unlock(&heap->lock);
                    return UHM_ERR_CORRUPTION;
                }
                state_transition_window =
                    active_ops > 1u && state_transition_observed;
            }
            if (slab->live_slots == 0) {
                expected_state = UHM_SLAB_STATE_EMPTY;
                if (slab->state == UHM_SLAB_STATE_PARTIAL &&
                    uhm_slab_remote_pending_load(slab) != 0 &&
                    state_transition_window) {
                    ++slab_expected_partial[slab->size_class];
                } else {
                    ++slab_expected_empty[slab->size_class];
                }
            } else if (slab->free_slots == 0) {
                expected_state = UHM_SLAB_STATE_FULL;
                if (slab->state == UHM_SLAB_STATE_PARTIAL &&
                    atomic_load_explicit(&slab->remote_ready_queued,
                                         UHM_ATOMIC_ACQUIRE) ==
                        UHM_SLAB_REMOTE_READY_SCANNING &&
                    remote_ready_scanner_protected &&
                    state_transition_window &&
                    uhm_heap_active_ops_load(heap) > 1u) {
                    ++slab_expected_partial[slab->size_class];
                } else {
                    ++slab_expected_full[slab->size_class];
                }
            } else {
                expected_state = UHM_SLAB_STATE_PARTIAL;
                if (slab->state == UHM_SLAB_STATE_FULL &&
                    uhm_slab_remote_pending_load(slab) != 0 &&
                    state_transition_window) {
                    ++slab_expected_full[slab->size_class];
                } else if (slab->state == UHM_SLAB_STATE_EMPTY &&
                           atomic_load_explicit(&slab->remote_ready_queued,
                                                UHM_ATOMIC_ACQUIRE) ==
                               UHM_SLAB_REMOTE_READY_SCANNING &&
                           remote_ready_scanner_protected &&
                           state_transition_window &&
                           uhm_heap_active_ops_load(heap) > 1u) {
                    ++slab_expected_empty[slab->size_class];
                } else {
                    ++slab_expected_partial[slab->size_class];
                }
            }
            if (slab->state != expected_state &&
                !(((slab->state == UHM_SLAB_STATE_FULL &&
                    expected_state == UHM_SLAB_STATE_PARTIAL &&
                    uhm_slab_remote_pending_load(slab) != 0 &&
                    state_transition_window) ||
                   (slab->state == UHM_SLAB_STATE_PARTIAL &&
                    expected_state == UHM_SLAB_STATE_EMPTY &&
                    uhm_slab_remote_pending_load(slab) != 0 &&
                    state_transition_window) ||
                    (slab->state == UHM_SLAB_STATE_PARTIAL &&
                    expected_state == UHM_SLAB_STATE_FULL &&
                     atomic_load_explicit(&slab->remote_ready_queued,
                                          UHM_ATOMIC_ACQUIRE) ==
                        UHM_SLAB_REMOTE_READY_SCANNING &&
                     remote_ready_scanner_protected &&
                     state_transition_window) ||
                    (slab->state == UHM_SLAB_STATE_EMPTY &&
                     expected_state == UHM_SLAB_STATE_PARTIAL &&
                     atomic_load_explicit(&slab->remote_ready_queued,
                                          UHM_ATOMIC_ACQUIRE) ==
                        UHM_SLAB_REMOTE_READY_SCANNING &&
                     remote_ready_scanner_protected &&
                     state_transition_window)) &&
                  (remote_ready_state != UHM_SLAB_REMOTE_READY_IDLE ||
                   uhm_heap_active_ops_load(heap) > 1u))) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
            ++slab_all_count;
            prev_all = slab;
        }
    }

    for (class_index = 0; class_index < UHM_SIZE_CLASS_COUNT; ++class_index) {
        size_t partial_count = 0;
        size_t full_count = 0;
        size_t empty_count = 0;
        size_t committed_empty_count;
        uint64_t ready_seq =
            uhm_slab_remote_ready_seq_load(heap, (uint16_t)class_index);
        if (uhm_slab_remote_ready_seq_active_value(ready_seq) &&
            uhm_heap_active_ops_load(heap) <= 1u) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        if (!uhm_slab_empty_committed_cache_state_is_valid(
                heap->core.slab_empty_committed_cache_high_water[class_index])) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        if (!uhm_slab_state_list_validate(heap->core.slab_class_heads[class_index],
                                          heap->core.slab_all_head,
                                          (uint16_t)class_index,
                                          UHM_SLAB_STATE_PARTIAL,
                                          &partial_count, slab_all_count,
                                          uhm_heap_active_ops_load(heap) > 1u) ||
            !uhm_slab_state_list_validate(heap->core.slab_full_heads[class_index],
                                          heap->core.slab_all_head,
                                          (uint16_t)class_index,
                                          UHM_SLAB_STATE_FULL,
                                          &full_count, slab_all_count,
                                          uhm_heap_active_ops_load(heap) > 1u) ||
            !uhm_slab_state_list_validate(heap->core.slab_empty_heads[class_index],
                                          heap->core.slab_all_head,
                                          (uint16_t)class_index,
                                          UHM_SLAB_STATE_EMPTY,
                                          &empty_count, slab_all_count,
                                          uhm_heap_active_ops_load(heap) > 1u)) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        committed_empty_count = uhm_slab_committed_empty_count_for_check(
            heap, (uint16_t)class_index);
        if (partial_count != slab_expected_partial[class_index] ||
            full_count != slab_expected_full[class_index] ||
            empty_count != slab_expected_empty[class_index] ||
            !uhm_tagged_freelist_epoch_valid_for_check(
                heap, &heap->core.slab_remote_class_lf[class_index].queue) ||
            !uhm_tagged_freelist_epoch_valid_for_check(
                heap, &heap->core.slab_remote_ready_lf[class_index].queue) ||
            heap->core.slab_empty_count[class_index] != empty_count ||
            heap->core.slab_committed_empty_count[class_index] !=
                committed_empty_count) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        slab_state_total += partial_count + full_count + empty_count;
    }
    if (slab_state_total != slab_all_count) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }

    tree_count = 0;
    if (!uhm_vma_tree_validate(heap, heap->core.vma_tree_root, 0,
                               &tree_count)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    list_count = 0;
    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        ++list_count;
    }
    if (tree_count != list_count) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }

    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        uintptr_t vma_end;
        if (vma->magic != UHM_VMA_MAGIC ||
            uhm_range_overflows(vma->addr, vma->size, &vma_end) ||
            vma->addr < heap->core.mmap_lo ||
            vma_end > heap->core.arena_end) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        if (!uhm_extent_descriptor_matches(&vma->desc,
                                           UHM_EXTENT_KIND_MMAP,
                                           vma->addr,
                                           vma->size,
                                           UHM_PAGE_OWNER_MMAP,
                                           UHM_PAGE_VA_MMAP_LIVE,
                                           uhm_vma_commit_state_from_flags(vma->flags),
                                           vma->prot,
                                           vma->generation,
                                           vma)) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        {
            uint32_t commit_state = uhm_vma_commit_state_from_flags(vma->flags);
            int active_op_window =
                uhm_heap_active_ops_load(heap) > 1u;
            int page_records_ok = uhm_page_range_matches_descriptor(
                heap, vma->addr, vma->size, UHM_PAGE_OWNER_MMAP,
                UHM_PAGE_VA_MMAP_LIVE, commit_state, vma->prot, &vma->desc,
                1, 1);
            if (!page_records_ok && active_op_window) {
                page_records_ok =
                    uhm_page_range_matches_descriptor_live_or_anon_protecting(
                    heap, vma->addr, vma->size, UHM_PAGE_OWNER_MMAP,
                    UHM_PAGE_VA_MMAP_LIVE, UHM_PAGE_VA_PROTECTING,
                    commit_state, vma->prot, &vma->desc);
            }
            if (!page_records_ok) {
                uhm_unlock(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
        }
    }
    tree_count = 0;
    if (!uhm_mmap_free_tree_validate(heap, heap->core.mmap_free_tree_root, 0,
                                     &tree_count)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    list_count = 0;
    prev_free_extent = 0;
    for (free_extent = heap->core.mmap_free_head; free_extent;
         free_extent = free_extent->next) {
        if (free_extent->prev != prev_free_extent) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        ++list_count;
        prev_free_extent = free_extent;
    }
    if (tree_count != list_count) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_mmap_recent_free_cache_valid(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }

    prev_free_extent = 0;
    for (free_extent = heap->core.mmap_free_head; free_extent;
         free_extent = free_extent->next) {
        if (free_extent->magic != UHM_FREE_MAGIC ||
            free_extent->prev != prev_free_extent ||
            (free_extent->flags & UHM_META_FLAG_IN_MMAP_FREE_LIST) == 0 ||
            (free_extent->flags &
             ~(UHM_META_FLAG_EMERGENCY | UHM_META_FLAG_IN_MMAP_FREE_LIST |
               UHM_FREE_EXTENT_CACHE_POSITION_MASK)) != 0 ||
            !uhm_extent_descriptor_matches(&free_extent->desc,
                                           UHM_EXTENT_KIND_MMAP_FREE,
                                           free_extent->addr,
                                           free_extent->size,
                                           UHM_PAGE_OWNER_FREE,
                                           UHM_PAGE_VA_FREE_REUSABLE,
                                           UHM_PAGE_COMMIT_DECOMMITTED,
                                           UHM_PROT_NONE,
                                           free_extent->desc.generation,
                                           free_extent) ||
            !uhm_page_range_is_free_budgeted(heap, free_extent->addr,
                                             free_extent->size)) {
            uhm_unlock(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        prev_free_extent = free_extent;
    }
    uhm_unlock(&heap->lock);
    return UHM_OK;
}

