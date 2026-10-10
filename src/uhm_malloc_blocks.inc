static int uhm_free_block_isolatable_for_merge(const uhm_block *block) {
    return block && block->magic == UHM_BLOCK_MAGIC &&
           (block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
           (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 &&
           (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) == 0;
}

static int uhm_free_blocks_adjacent_for_merge(const uhm_block *left,
                                              const uhm_block *right) {
    uintptr_t left_end;

    return left && right && left->magic == UHM_BLOCK_MAGIC &&
           right->magic == UHM_BLOCK_MAGIC &&
           (left->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
           (right->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
           !uhm_range_overflows((uintptr_t)left, left->total_size,
                                &left_end) &&
           left_end == (uintptr_t)right;
}

static uhm_result uhm_block_prepare_adjacent_free_for_merge(
    uhm_heap *heap, uhm_block *left, uhm_block *right,
    uint32_t *merged_flags_out) {
    uint32_t merged_flags = UHM_BLOCK_FLAG_FREE;

    if (!uhm_free_blocks_adjacent_for_merge(left, right) ||
        !uhm_free_block_isolatable_for_merge(left) ||
        !uhm_free_block_isolatable_for_merge(right) || !merged_flags_out) {
        return UHM_ERR_CORRUPTION;
    }

    if ((left->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0) {
        uhm_block_release_retained_free_extent(heap, left);
    }
    if ((right->flags & UHM_BLOCK_FLAG_DECOMMIT_RETAINED) != 0) {
        uhm_block_release_retained_free_extent(heap, right);
    }

    if (((left->flags | right->flags) & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0) {
        merged_flags |= UHM_BLOCK_FLAG_DECOMMIT_HOT;
    }
    if ((left->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0 &&
        (right->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0) {
        merged_flags |= UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;
    }
    *merged_flags_out = merged_flags;
    return UHM_OK;
}

static uhm_result uhm_split_block(uhm_heap *heap, uhm_block *block,
                                  size_t wanted,
                                  int allow_platform_unlock) {
    size_t header = uhm_malloc_header_size();
    size_t remaining = block->total_size - wanted;
    uhm_block *next;

    if (remaining < header + UHM_MIN_ALIGNMENT) {
        return UHM_OK;
    }

    block->total_size = wanted;
    next = (uhm_block *)((uintptr_t)block + wanted);
    memset(next, 0, sizeof(*next));
    next->magic = UHM_BLOCK_MAGIC;
    next->flags = UHM_BLOCK_FLAG_FREE;
    if ((block->flags & UHM_BLOCK_FLAG_DECOMMIT_HOT) != 0) {
        next->flags |= UHM_BLOCK_FLAG_DECOMMIT_HOT;
    }
    if ((block->flags & UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN) != 0) {
        next->flags |= UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;
    }
    next->size_class = UHM_SIZE_CLASS_NONE;
    next->total_size = remaining;
    next->requested_size = 0;
    next->prev_phys = block;
    next->next_phys = block->next_phys;
    uhm_block_tree_priority_init_free(heap, next);
    next->generation = uhm_heap_generation_next(heap);
    uhm_block_descriptor_set(next);
    if (block->next_phys) {
        block->next_phys->prev_phys = next;
    } else {
        heap->core.last_block = next;
    }
    block->next_phys = next;
    block->generation = uhm_heap_generation_next(heap);
    uhm_block_descriptor_set(block);
    uhm_free_list_insert(heap, next);
    return uhm_block_decommit_free_extent(heap, next,
                                          allow_platform_unlock);
}

static uhm_result uhm_coalesce_block(uhm_heap *heap, uhm_block **block_inout) {
    uhm_block *block = block_inout ? *block_inout : 0;
    uhm_block *next;
    uhm_block *prev;
    uhm_result result;
    uintptr_t block_end;
    uintptr_t prev_end;

    if (!block) {
        return UHM_ERR_CORRUPTION;
    }
    next = block->next_phys;
    prev = block->prev_phys;

    block->size_class = UHM_SIZE_CLASS_NONE;
    if (next && next->magic == UHM_BLOCK_MAGIC &&
        (next->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
        (next->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 &&
        (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 &&
        !uhm_range_overflows((uintptr_t)block, block->total_size,
                             &block_end) &&
        block_end == (uintptr_t)next) {
        size_t merged_size;
        uint32_t merged_flags;
        if (uhm_add_overflows_size(block->total_size, next->total_size,
                                   &merged_size)) {
            return UHM_ERR_CORRUPTION;
        }
        result = uhm_block_prepare_adjacent_free_for_merge(heap, block, next,
                                                           &merged_flags);
        if (result != UHM_OK) {
            return result;
        }
        uhm_free_list_remove(heap, next);
        uhm_malloc_free_tree_remove(heap, block);
        block->flags = merged_flags;
        block->total_size = merged_size;
        block->next_phys = next->next_phys;
        if (next->next_phys) {
            next->next_phys->prev_phys = block;
        } else {
            heap->core.last_block = block;
        }
        uhm_extent_descriptor_retire(&next->desc);
        next->magic = 0;
        next->size_class = UHM_SIZE_CLASS_NONE;
        next->free_tree_priority = 0;
        next->live_tree_priority = 0;
        block->size_class = UHM_SIZE_CLASS_NONE;
        block->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(block);
        uhm_malloc_free_tree_insert(heap, block);
    }

    if (prev && prev->magic == UHM_BLOCK_MAGIC &&
        (prev->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
        (prev->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 &&
        (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0 &&
        !uhm_range_overflows((uintptr_t)prev, prev->total_size,
                             &prev_end) &&
        prev_end == (uintptr_t)block) {
        size_t merged_size;
        uint32_t merged_flags;
        if (uhm_add_overflows_size(prev->total_size, block->total_size,
                                   &merged_size)) {
            return UHM_ERR_CORRUPTION;
        }
        result = uhm_block_prepare_adjacent_free_for_merge(heap, prev, block,
                                                           &merged_flags);
        if (result != UHM_OK) {
            return result;
        }
        uhm_free_list_remove(heap, block);
        uhm_malloc_free_tree_remove(heap, prev);
        prev->flags = merged_flags;
        prev->total_size = merged_size;
        prev->next_phys = block->next_phys;
        if (block->next_phys) {
            block->next_phys->prev_phys = prev;
        } else {
            heap->core.last_block = prev;
        }
        uhm_extent_descriptor_retire(&block->desc);
        block->magic = 0;
        block->size_class = UHM_SIZE_CLASS_NONE;
        block->free_tree_priority = 0;
        block->live_tree_priority = 0;
        prev->size_class = UHM_SIZE_CLASS_NONE;
        prev->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(prev);
        uhm_malloc_free_tree_insert(heap, prev);
        block = prev;
    }
    if (block_inout) {
        *block_inout = block;
    }
    return UHM_OK;
}

static uhm_block *uhm_live_block_from_payload_descriptor_hint(
    uhm_heap *heap, const void *ptr, uint64_t descriptor_id) {
    const size_t desc_offset = offsetof(uhm_block, desc);
    uintptr_t block_addr;
    uintptr_t block_end;
    uhm_block *block;

    if (!heap || !ptr || descriptor_id == 0 ||
        descriptor_id > (uint64_t)UINTPTR_MAX ||
        descriptor_id < (uint64_t)desc_offset) {
        return 0;
    }
    block_addr = (uintptr_t)(descriptor_id - (uint64_t)desc_offset);
    if (block_addr < heap->core.arena_base ||
        uhm_range_overflows(block_addr, sizeof(*block), &block_end) ||
        block_end > heap->core.arena_end ||
        (block_addr % alignof(uhm_block)) != 0) {
        return 0;
    }
    block = (uhm_block *)block_addr;
    /* The page record descriptor id already points at this descriptor;
       tolerate desc.id drift so legal payloads do not scan the physical chain. */
    if (block->magic != UHM_BLOCK_MAGIC ||
        (block->flags & UHM_BLOCK_FLAG_FREE) != 0 ||
        block->requested_size == 0 ||
        block->desc.object != block ||
        uhm_block_payload(block) != ptr) {
        return 0;
    }
    return block;
}

static int uhm_live_block_descriptor_hint_points_elsewhere(
    uhm_heap *heap, const void *ptr, uint64_t descriptor_id) {
    const size_t desc_offset = offsetof(uhm_block, desc);
    uintptr_t block_addr;
    uintptr_t block_end;
    uhm_block *block;
    uhm_vma *vma;

    if (!heap || !ptr || descriptor_id == 0 ||
        descriptor_id > (uint64_t)UINTPTR_MAX) {
        return 0;
    }
    if (descriptor_id >= (uint64_t)desc_offset) {
        block_addr = (uintptr_t)(descriptor_id - (uint64_t)desc_offset);
        if (block_addr >= heap->core.arena_base &&
            !uhm_range_overflows(block_addr, sizeof(*block), &block_end) &&
            block_end <= heap->core.arena_end &&
            (block_addr % alignof(uhm_block)) == 0) {
            block = (uhm_block *)block_addr;
            if (block->magic == UHM_BLOCK_MAGIC &&
                block->desc.magic == UHM_EXTENT_DESCRIPTOR_MAGIC &&
                block->desc.kind == UHM_EXTENT_KIND_MALLOC &&
                block->desc.id == descriptor_id &&
                block->desc.object == block &&
                uhm_block_payload(block) != ptr) {
                return 1;
            }
        }
    }
    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        if (vma->magic == UHM_VMA_MAGIC &&
            vma->desc.magic == UHM_EXTENT_DESCRIPTOR_MAGIC &&
            vma->desc.kind == UHM_EXTENT_KIND_MMAP &&
            vma->desc.id == descriptor_id &&
            vma->desc.object == vma) {
            return 1;
        }
    }
    return 0;
}

static int uhm_live_block_note_metadata_drift(uhm_heap *heap) {
    if (!heap) {
        return 0;
    }
    heap->core.live_block_health_flags |=
        UHM_LIVE_BLOCK_HEALTH_METADATA_DRIFT;
    if ((heap->state_flags & UHM_CONFIG_FAIL_ON_LIVE_BLOCK_HEALTH) != 0u) {
        uhm_set_error(heap, UHM_ERR_CORRUPTION);
        return 1;
    }
    return 0;
}

static uhm_block *uhm_find_live_block_by_payload_slow(
    uhm_heap *heap, const void *ptr, uint64_t payload_descriptor_id,
    size_t callsite) {
    uhm_block *block;
    uint64_t tree_steps = 0;
    uint64_t scanned = 0;
    uint64_t scan_limit;
    int aborted = 0;

    if (uhm_live_block_fallback_negative_hit(heap, ptr, callsite)) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_misses));
        return 0;
    }
    block = uhm_live_block_tree_find_by_payload(heap, ptr, &tree_steps);
    if (block) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_hits));
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_tree_hits));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_scanned_nodes),
            tree_steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_max_scanned_nodes),
            tree_steps);
        return block;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_tree_misses));
    block = uhm_live_block_from_payload_descriptor_hint(
        heap, ptr, payload_descriptor_id);
    if (block) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_hits));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_scanned_nodes),
            tree_steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_max_scanned_nodes),
            tree_steps);
        return block;
    }
    if (uhm_live_block_descriptor_hint_points_elsewhere(
            heap, ptr, payload_descriptor_id)) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_misses));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_scanned_nodes),
            tree_steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_max_scanned_nodes),
            tree_steps);
        return 0;
    }
    if (uhm_live_block_fallback_scenario_throttled(heap, callsite)) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_misses));
        uhm_live_block_fallback_negative_store(heap, ptr, callsite);
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_scanned_nodes),
            tree_steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_max_scanned_nodes),
            tree_steps);
        return 0;
    }
    if (uhm_live_block_fallback_epoch_throttled(heap)) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_misses));
        uhm_live_block_fallback_negative_store(heap, ptr, callsite);
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_scanned_nodes),
            tree_steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_max_scanned_nodes),
            tree_steps);
        return 0;
    }
    /* A physical-chain walk is a metadata recovery path, not normal lookup. */
    if (uhm_live_block_note_metadata_drift(heap)) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_misses));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_scanned_nodes),
            tree_steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_max_scanned_nodes),
            tree_steps);
        return 0;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_fallback_scans));
    scan_limit = uhm_live_block_fallback_scan_limit(heap);
    for (block = heap->core.first_block; block; block = block->next_phys) {
        scanned++;
        if (scan_limit != 0 && scanned > scan_limit) {
            aborted = 1;
            break;
        }
        if (block->magic == UHM_BLOCK_MAGIC &&
            (block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
            uhm_block_payload(block) == ptr) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_hits));
            uhm_diag_note_live_block_fallback_scan(heap, scanned, 0);
            uhm_diag_note_u64_add(
                UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_scanned_nodes),
                tree_steps + scanned);
            uhm_diag_note_max_u64(
                UHM_DIAG_CORE_FIELD_PTR(heap,
                                        live_block_slow_max_scanned_nodes),
                tree_steps + scanned);
            return block;
        }
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_misses));
    uhm_diag_note_live_block_fallback_scan(heap, scanned, aborted);
    uhm_live_block_fallback_negative_store(heap, ptr, callsite);
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_scanned_nodes),
        tree_steps + scanned);
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_max_scanned_nodes),
        tree_steps + scanned);
    return 0;
}

static void uhm_live_block_note_fast_fail(uhm_heap *heap, uint32_t reason) {
    if (!heap) {
        return;
    }
    switch (reason) {
    case UHM_LIVE_BLOCK_FAST_FAIL_INVALID:
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_fast_fail_invalid));
        break;
    case UHM_LIVE_BLOCK_FAST_FAIL_RANGE:
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_fast_fail_range));
        break;
    case UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE:
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_fast_fail_header_page));
        break;
    case UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE:
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_fast_fail_payload_page));
        break;
    case UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR:
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_fast_fail_descriptor));
        break;
    default:
        break;
    }
}

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_live_block_note_callsite_fast_fail(uhm_heap *heap,
                                                   size_t callsite,
                                                   uint32_t reason) {
    if (!heap || reason == UHM_LIVE_BLOCK_FAST_FAIL_NONE ||
        reason >= UHM_LIVE_BLOCK_FAST_FAIL_REASON_COUNT ||
        callsite >= UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT) {
        return;
    }
    uhm_diag_note_u64_inc(
        &heap->core.live_block_lookup_fast_fail_reasons[callsite][reason]);
}
#endif

static int uhm_live_block_fast_fail_is_terminal(uint32_t reason) {
    return reason == UHM_LIVE_BLOCK_FAST_FAIL_INVALID ||
           reason == UHM_LIVE_BLOCK_FAST_FAIL_RANGE;
}

static int uhm_live_block_direct_descriptor_valid(uhm_block *block,
                                                  uintptr_t block_addr,
                                                  const void *ptr) {
    return block && block->magic == UHM_BLOCK_MAGIC &&
           (block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
           block->desc.magic == UHM_EXTENT_DESCRIPTOR_MAGIC &&
           block->desc.kind == UHM_EXTENT_KIND_MALLOC &&
           block->desc.addr == block_addr &&
           block->desc.size == block->total_size &&
           block->desc.owner == UHM_PAGE_OWNER_MALLOC &&
           block->desc.va_state == UHM_PAGE_VA_MALLOC_LIVE &&
           block->desc.commit_state == UHM_PAGE_COMMIT_LIVE &&
           block->desc.id == uhm_extent_descriptor_id(&block->desc) &&
           block->desc.object == block && uhm_block_payload(block) == ptr;
}

static int uhm_live_block_terminal_free_payload(uhm_block *block,
                                                uintptr_t block_addr,
                                                const void *ptr,
                                                uint64_t descriptor_id) {
    return block && block->magic == UHM_BLOCK_MAGIC &&
           (block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
           block->desc.magic == UHM_EXTENT_DESCRIPTOR_MAGIC &&
           block->desc.kind == UHM_EXTENT_KIND_MALLOC &&
           block->desc.addr == block_addr &&
           block->desc.size == block->total_size &&
           block->desc.id == descriptor_id &&
           block->desc.id == uhm_extent_descriptor_id(&block->desc) &&
           block->desc.object == block && uhm_block_payload(block) == ptr;
}

static int uhm_live_block_tree_contains_linked(uhm_heap *heap,
                                               const uhm_block *block) {
    const uhm_block *parent;

    if (!heap || !block) {
        return 0;
    }
    if (heap->core.live_block_tree_root == block) {
        return 1;
    }
    parent = block->live_tree_parent;
    return parent &&
           (parent->live_tree_left == block || parent->live_tree_right == block);
}

static int uhm_live_payload_page_record_probe(uhm_heap *heap,
                                              uintptr_t payload,
                                              uhm_page_record *record_out) {
    size_t first;
    size_t count;
    size_t shard;
    const uhm_page_record *record;

    if (!record_out || !uhm_page_span(heap, payload, 1u, &first, &count) ||
        count != 1u) {
        return 0;
    }
    uhm_diag_note_page_range_query(heap, count, first,
                                   UHM_PAGE_QUERY_PHASE_GET_EXPECTED_STATE);
    shard = uhm_page_shard_for_index(first);
    uhm_diag_note_page_lock_range(heap, 1u);
    uhm_lock(&heap->core.page_map_locks[shard].lock);
    record = uhm_page_record_get_unlocked(heap, first);
    if (record) {
        *record_out = *record;
    }
    uhm_unlock(&heap->core.page_map_locks[shard].lock);
    return record != 0;
}

static int uhm_live_block_header_page_record_is_readable_malloc(
    uhm_heap *heap, uintptr_t block_addr) {
    uhm_page_record record;

    return uhm_live_payload_page_record_probe(heap, block_addr, &record) &&
           record.owner == UHM_PAGE_OWNER_MALLOC &&
           record.va_state == UHM_PAGE_VA_MALLOC_LIVE &&
           record.commit_state == UHM_PAGE_COMMIT_LIVE &&
           (record.prot & UHM_PROT_READ) != 0;
}

static int uhm_live_block_same_page(uhm_heap *heap, uintptr_t a,
                                    uintptr_t b) {
    size_t page_size = heap ? heap->F.vm.page_size : 0;

    return page_size != 0 &&
           uhm_align_down_uintptr(a, page_size) ==
               uhm_align_down_uintptr(b, page_size);
}

static uhm_block *uhm_live_block_try_exact_descriptor_large(
    uhm_heap *heap, const void *ptr, uintptr_t block_addr,
    uint64_t *descriptor_id_out,
    const uhm_page_record *payload_record_in) {
    const uintptr_t payload = (uintptr_t)ptr;
    const uint64_t expected_descriptor_id =
        (uint64_t)(block_addr + offsetof(uhm_block, desc));
    uhm_page_record payload_record;
    uhm_block *block;
    int payload_readable;
    int header_is_separate;

    if (descriptor_id_out) {
        *descriptor_id_out = 0;
    }
    if (!heap || !ptr || heap->F.vm.page_size == 0) {
        return 0;
    }
    if (!payload_record_in) {
        return 0;
    }
    payload_record = *payload_record_in;
    if (payload_record.owner != UHM_PAGE_OWNER_MALLOC ||
        payload_record.va_state != UHM_PAGE_VA_MALLOC_LIVE ||
        payload_record.commit_state != UHM_PAGE_COMMIT_LIVE ||
        payload_record.descriptor_id != expected_descriptor_id) {
        return 0;
    }
    payload_readable = (payload_record.prot & UHM_PROT_READ) != 0;
    header_is_separate = !uhm_live_block_same_page(heap, block_addr, payload);
    if (!payload_readable &&
        (!header_is_separate || (payload % heap->F.vm.page_size) != 0)) {
        return 0;
    }
    if (header_is_separate &&
        !uhm_live_block_header_page_record_is_readable_malloc(heap,
                                                              block_addr)) {
        return 0;
    }
    block = (uhm_block *)block_addr;
    if (!uhm_live_block_direct_descriptor_valid(block, block_addr, ptr) ||
        (!payload_readable &&
         (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) == 0) ||
        (block->requested_size < heap->F.vm.page_size &&
         (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) == 0)) {
        return 0;
    }
    if (descriptor_id_out) {
        *descriptor_id_out = payload_record.descriptor_id;
    }
    return block;
}

static uhm_block *uhm_find_live_block_by_payload_fast(uhm_heap *heap,
                                                      const void *ptr,
                                                      uint32_t *fail_reason,
                                                      uint64_t *descriptor_id,
                                                      int *terminal_miss) {
    uintptr_t payload = (uintptr_t)ptr;
    size_t header = uhm_malloc_header_size();
    uintptr_t block_addr;
    uintptr_t header_end;
    uint32_t commit_state = UHM_PAGE_COMMIT_NONE;
    uint64_t payload_descriptor_id = 0;
    int payload_state_loaded;
    int payload_record_loaded;
    uhm_page_record payload_record;
    uhm_page_record header_record;
    uhm_block *block;
    uhm_block *exact_block;

    if (fail_reason) {
        *fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_NONE;
    }
    if (descriptor_id) {
        *descriptor_id = 0;
    }
    if (terminal_miss) {
        *terminal_miss = 0;
    }
    if (!heap || !ptr || payload < header) {
        if (fail_reason) {
            *fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_INVALID;
        }
        return 0;
    }
    block_addr = payload - header;
    if (block_addr < heap->core.arena_base ||
        uhm_range_overflows(block_addr, sizeof(*block), &header_end) ||
        header_end > heap->core.arena_end ||
        (block_addr % alignof(uhm_block)) != 0) {
        if (fail_reason) {
            *fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_RANGE;
        }
        return 0;
    }
    payload_record_loaded = uhm_live_payload_page_record_probe(
        heap, payload, &payload_record);
    exact_block = uhm_live_block_try_exact_descriptor_large(
        heap, ptr, block_addr, &payload_descriptor_id,
        payload_record_loaded ? &payload_record : 0);
    if (exact_block) {
        if (descriptor_id) {
            *descriptor_id = payload_descriptor_id;
        }
        return exact_block;
    }
    if (!uhm_page_range_has_state(heap, block_addr, sizeof(*block),
                                  UHM_PAGE_OWNER_MALLOC,
                                  UHM_PAGE_VA_MALLOC_LIVE,
                                  UHM_PAGE_COMMIT_LIVE,
                                  UHM_PROT_READ | UHM_PROT_WRITE)) {
        if (terminal_miss &&
            uhm_live_payload_page_record_probe(heap, block_addr,
                                               &header_record) &&
            (header_record.owner != UHM_PAGE_OWNER_MALLOC ||
             header_record.va_state != UHM_PAGE_VA_MALLOC_LIVE ||
             header_record.commit_state != UHM_PAGE_COMMIT_LIVE)) {
            *terminal_miss = 1;
        }
        if (fail_reason) {
            *fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_HEADER_PAGE;
        }
        return 0;
    }
    if (payload_record_loaded) {
        payload_state_loaded =
            payload_record.owner == UHM_PAGE_OWNER_MALLOC &&
            payload_record.va_state == UHM_PAGE_VA_MALLOC_LIVE;
        commit_state = payload_record.commit_state;
        payload_descriptor_id = payload_record.descriptor_id;
    } else {
        payload_state_loaded = uhm_page_range_get_expected_state(
            heap, payload, 1u, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, &commit_state, 0,
            &payload_descriptor_id, 0);
    }
    if (!payload_state_loaded || commit_state != UHM_PAGE_COMMIT_LIVE) {
        if (terminal_miss) {
            if (payload_state_loaded) {
                if (commit_state != UHM_PAGE_COMMIT_LIVE) {
                    *terminal_miss = 1;
                }
            } else if (payload_record_loaded &&
                       (payload_record.owner != UHM_PAGE_OWNER_MALLOC ||
                        payload_record.va_state != UHM_PAGE_VA_MALLOC_LIVE ||
                        payload_record.commit_state !=
                            UHM_PAGE_COMMIT_LIVE)) {
                *terminal_miss = 1;
            }
        }
        if (fail_reason) {
            *fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE;
        }
        return 0;
    }
    if (payload_descriptor_id != 0 && descriptor_id) {
        *descriptor_id = payload_descriptor_id;
    }

    block = (uhm_block *)block_addr;
    if (payload_descriptor_id == 0) {
        if (uhm_live_block_direct_descriptor_valid(block, block_addr, ptr) &&
            uhm_live_block_tree_contains_linked(heap, block)) {
            return block;
        }
        if (terminal_miss) {
            *terminal_miss = 1;
        }
        if (fail_reason) {
            *fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_PAYLOAD_PAGE;
        }
        return 0;
    }
    if (!uhm_live_block_direct_descriptor_valid(block, block_addr, ptr)) {
        if (terminal_miss &&
            uhm_live_block_terminal_free_payload(
                block, block_addr, ptr, payload_descriptor_id)) {
            *terminal_miss = 1;
        }
        if (fail_reason) {
            *fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR;
        }
        return 0;
    }
    if (block->desc.id != payload_descriptor_id) {
        if (!uhm_live_block_tree_contains_linked(heap, block)) {
            if (fail_reason) {
                *fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_DESCRIPTOR;
            }
            return 0;
        }
    }
    return block;
}

static uhm_block *uhm_find_live_block_by_payload_impl(uhm_heap *heap,
                                                      const void *ptr,
                                                      size_t callsite
#if UHMAS_ENABLE_DIAGNOSTICS
                                                      ,
                                                      uint32_t *fail_reason_out
#endif
) {
    uhm_block *block;
    uint32_t fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_NONE;
    uint64_t payload_descriptor_id = 0;
    int terminal_miss = 0;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t slow_begin_ns;
    uint64_t slow_elapsed_ns;
    if (fail_reason_out) {
        *fail_reason_out = UHM_LIVE_BLOCK_FAST_FAIL_NONE;
    }
#endif
    if (!heap) {
        return 0;
    }
    block = uhm_find_live_block_by_payload_fast(
        heap, ptr, &fail_reason, &payload_descriptor_id, &terminal_miss);
#if UHMAS_ENABLE_DIAGNOSTICS
    if (fail_reason_out) {
        *fail_reason_out = fail_reason;
    }
#endif
    if (block) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_fast_hits));
        return block;
    }
    uhm_live_block_note_fast_fail(heap, fail_reason);
    if (terminal_miss || uhm_live_block_fast_fail_is_terminal(fail_reason)) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_fast_terminal_misses));
        return 0;
    }
    /* Scenario accounting is only useful after the O(1) path fails. */
    uhm_live_block_fallback_scenario_note_call(heap, callsite);
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_slow_calls));
#if UHMAS_ENABLE_DIAGNOSTICS
    slow_begin_ns = uhm_diag_begin_ns(heap);
#endif
    block = uhm_find_live_block_by_payload_slow(
        heap, ptr, payload_descriptor_id, callsite);
#if UHMAS_ENABLE_DIAGNOSTICS
    if (slow_begin_ns != 0) {
        slow_elapsed_ns = uhm_diag_elapsed_ns(heap, slow_begin_ns);
        uhm_diag_note_u64_add(&heap->core.live_block_slow_ns,
                              slow_elapsed_ns);
        uhm_diag_note_max_u64(&heap->core.live_block_slow_max_ns,
                              slow_elapsed_ns);
    }
#endif
    return block;
}

static uhm_block *uhm_find_live_block_by_payload_from(uhm_heap *heap,
                                                      const void *ptr,
                                                      size_t callsite) {
#if !UHMAS_ENABLE_DIAGNOSTICS
    if (!heap) {
        return 0;
    }
    if (callsite >= UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT) {
        callsite = UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN;
    }

    return uhm_find_live_block_by_payload_impl(heap, ptr, callsite);
#else
    uint64_t slow_before;
    uint64_t fallback_before;
    uint64_t throttle_before;
    uint32_t fail_reason = UHM_LIVE_BLOCK_FAST_FAIL_NONE;
    uhm_block *block;

    if (!heap) {
        return 0;
    }
    if (callsite >= UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT) {
        callsite = UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN;
    }
    slow_before = heap->core.live_block_slow_calls;
    fallback_before = heap->core.live_block_slow_fallback_scans;
    throttle_before = heap->core.live_block_slow_fallback_throttles;
    uhm_diag_note_u64_inc(&heap->core.live_block_lookup_calls[callsite]);
    block = uhm_find_live_block_by_payload_impl(heap, ptr, callsite,
                                                &fail_reason);
    uhm_live_block_note_callsite_fast_fail(heap, callsite, fail_reason);
    if (heap->core.live_block_slow_calls != slow_before) {
        uhm_diag_note_u64_inc(
            &heap->core.live_block_lookup_slow_calls[callsite]);
    }
    if (heap->core.live_block_slow_fallback_scans != fallback_before) {
        uhm_diag_note_u64_inc(
            &heap->core.live_block_lookup_fallback_scans[callsite]);
    }
    if (heap->core.live_block_slow_fallback_throttles != throttle_before) {
        uhm_diag_note_u64_inc(
            &heap->core.live_block_lookup_throttles[callsite]);
    }
    return block;
#endif
}

static int uhm_block_is_page_private_candidate(uhm_heap *heap, uintptr_t payload,
                                               size_t requested_size) {
    size_t page_size = heap->F.vm.page_size;
    return requested_size != 0 &&
           requested_size >= page_size &&
           (requested_size % page_size) == 0 &&
           (payload % page_size) == 0;
}

static uhm_block *uhm_page_private_block_from_descriptor_id(
    uhm_heap *heap, uint64_t descriptor_id, uintptr_t addr, uintptr_t end) {
    const size_t desc_offset = offsetof(uhm_block, desc);
    uintptr_t block_addr;
    uintptr_t block_header_end;
    uintptr_t payload;
    uintptr_t payload_end;
    uhm_block *block;

    if (!heap || descriptor_id == 0 ||
        descriptor_id > (uint64_t)UINTPTR_MAX ||
        descriptor_id < (uint64_t)desc_offset) {
        return 0;
    }
    block_addr = (uintptr_t)(descriptor_id - (uint64_t)desc_offset);
    if (block_addr < heap->core.arena_base ||
        uhm_range_overflows(block_addr, sizeof(*block), &block_header_end) ||
        block_header_end > heap->core.arena_end ||
        (block_addr % alignof(uhm_block)) != 0) {
        return 0;
    }
    block = (uhm_block *)block_addr;
    if (block->magic != UHM_BLOCK_MAGIC ||
        (block->flags & UHM_BLOCK_FLAG_FREE) != 0 ||
        (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) == 0 ||
        block->requested_size == 0 || block->desc.id != descriptor_id ||
        block->desc.object != block) {
        return 0;
    }
    payload = (uintptr_t)uhm_block_payload(block);
    if (uhm_range_overflows(payload, block->requested_size, &payload_end)) {
        return 0;
    }
    return addr >= payload && end <= payload_end ? block : 0;
}

static uhm_block *uhm_find_page_private_block_covering_by_scan(
    uhm_heap *heap, uintptr_t addr, uintptr_t end) {
    uhm_block *block;

    for (block = heap->core.first_block; block; block = block->next_phys) {
        uintptr_t payload;
        uintptr_t payload_end;
        if (block->magic != UHM_BLOCK_MAGIC ||
            (block->flags & UHM_BLOCK_FLAG_FREE) != 0 ||
            (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) == 0) {
            continue;
        }
        payload = (uintptr_t)uhm_block_payload(block);
        if (uhm_range_overflows(payload, block->requested_size, &payload_end)) {
            continue;
        }
        if (addr >= payload && end <= payload_end) {
            return block;
        }
    }
    return 0;
}

static uhm_block *uhm_find_page_private_block_covering(
    uhm_heap *heap, uintptr_t addr, size_t size,
    uint32_t *commit_state_out, uint32_t *prot_out,
    uint64_t *descriptor_id_out, uint64_t *generation_out,
    int *state_loaded_out) {
    uintptr_t end;
    uint32_t commit_state = UHM_PAGE_COMMIT_NONE;
    uint32_t prot = UHM_PROT_NONE;
    uint64_t descriptor_id = 0;
    uint64_t generation = 0;
    uhm_block *block;

    if (state_loaded_out) {
        *state_loaded_out = 0;
    }
    if (uhm_range_overflows(addr, size, &end)) {
        return 0;
    }

    if (uhm_page_range_get_expected_state(
            heap, addr, size, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, &commit_state, &prot, &descriptor_id,
            &generation)) {
        block = uhm_page_private_block_from_descriptor_id(
            heap, descriptor_id, addr, end);
        if (block) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(
                    heap, mprotect_page_private_descriptor_hits));
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
            if (state_loaded_out) {
                *state_loaded_out = 1;
            }
            return block;
        }
    }

    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(
            heap, mprotect_page_private_scan_fallbacks));
    return uhm_find_page_private_block_covering_by_scan(heap, addr, end);
}

static int uhm_page_range_get_malloc_state(uhm_heap *heap, uintptr_t addr, size_t size,
                                           uhm_block *block,
                                           uint32_t *commit_state_out,
                                           uint32_t *prot_out) {
    uint64_t descriptor_id = 0;
    if (!uhm_page_range_get_expected_state(heap, addr, size,
                                           UHM_PAGE_OWNER_MALLOC,
                                           UHM_PAGE_VA_MALLOC_LIVE,
                                           commit_state_out, prot_out,
                                           &descriptor_id, 0)) {
        return 0;
    }
    return descriptor_id == block->desc.id;
}

static int uhm_page_range_is_live_malloc_block(uhm_heap *heap, uintptr_t addr,
                                               size_t size, uhm_block *block) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(
        heap, count, first, UHM_PAGE_QUERY_PHASE_MATCHES_DESCRIPTOR);
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
            if (record->owner != UHM_PAGE_OWNER_MALLOC ||
                record->va_state != UHM_PAGE_VA_MALLOC_LIVE ||
                record->commit_state != UHM_PAGE_COMMIT_LIVE ||
                record->descriptor_id != block->desc.id) {
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

static uhm_result uhm_page_span_flip_malloc_va_state_preserve_locked(
    uhm_heap *heap, size_t first, size_t count, uint32_t from_va_state,
    uint32_t to_va_state, uint64_t descriptor_id,
    const uhm_lockset *lockset) {
    size_t cursor;
    size_t end;
    int pass;

    if (!heap || count == 0 || first > SIZE_MAX - count) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!uhm_lockset_covers_page_range(lockset, first, count)) {
        return UHM_ERR_CORRUPTION;
    }

    for (pass = 0; pass < 2; ++pass) {
        cursor = first;
        end = first + count;
        while (cursor < end) {
            uhm_page_leaf *leaf;
            size_t offset;
            size_t chunk;
            size_t i;

            if (!uhm_page_leaf_chunk_unlocked(heap, cursor, end, &leaf,
                                              &offset, &chunk) ||
                !leaf) {
                return UHM_ERR_CORRUPTION;
            }
            for (i = 0; i < chunk; ++i) {
                uhm_page_record *record = &leaf->records[offset + i];
                if (record->owner != UHM_PAGE_OWNER_MALLOC ||
                    record->va_state != from_va_state ||
                    record->commit_state != UHM_PAGE_COMMIT_LIVE ||
                    record->descriptor_id != descriptor_id) {
                    return UHM_ERR_PROTECTION;
                }
                if (pass != 0) {
                    record->va_state = to_va_state;
                }
            }
            cursor += chunk;
        }
    }
    uhm_diag_note_page_mark_range(heap, first, count,
                                  UHM_PAGE_OWNER_MALLOC, to_va_state,
                                  UHM_PAGE_COMMIT_LIVE);
    return UHM_OK;
}

static int uhm_page_range_matches_malloc_va_state_desc(
    uhm_heap *heap, uintptr_t addr, size_t size, uint32_t va_state,
    uint64_t descriptor_id) {
    size_t first;
    size_t count;
    size_t cursor;
    size_t end;
    uhm_lockset lockset;
    int ok = 1;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    uhm_diag_note_page_range_query(
        heap, count, first, UHM_PAGE_QUERY_PHASE_MATCHES_DESCRIPTOR);
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
            if (record->owner != UHM_PAGE_OWNER_MALLOC ||
                record->va_state != va_state ||
                record->commit_state != UHM_PAGE_COMMIT_LIVE ||
                record->descriptor_id != descriptor_id) {
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

static uhm_result uhm_restore_page_private_block_rw(uhm_heap *heap, uhm_block *block) {
    uintptr_t payload;
    size_t requested_size;
    uint32_t old_prot = UHM_PROT_NONE;
    size_t first;
    size_t count;
    uint64_t old_descriptor_id;
    uhm_result result;

    if ((block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) == 0) {
        return UHM_OK;
    }
    payload = (uintptr_t)uhm_block_payload(block);
    requested_size = block->requested_size;
    old_descriptor_id = block->desc.id;
    if (uhm_page_range_overlaps_va_state(heap, payload, requested_size,
                                         UHM_PAGE_VA_PROTECTING)) {
        return UHM_ERR_PROTECTION;
    }
    if (!uhm_page_range_is_live_malloc_block(heap, payload, requested_size, block)) {
        return UHM_ERR_CORRUPTION;
    }
    if (uhm_page_range_get_malloc_state(heap, payload, requested_size, block,
                                        0, &old_prot) &&
        old_prot == (UHM_PROT_READ | UHM_PROT_WRITE)) {
        block->flags &= ~UHM_BLOCK_FLAG_PAGE_PRIVATE;
        return UHM_OK;
    }
    if (!uhm_page_span(heap, payload, requested_size, &first, &count)) {
        return UHM_ERR_CORRUPTION;
    }
    {
        uhm_lockset mark_lockset;
        uhm_page_lock_shard_range(heap, first, count, &mark_lockset);
        result = uhm_page_span_flip_malloc_va_state_preserve_locked(
            heap, first, count, UHM_PAGE_VA_MALLOC_LIVE,
            UHM_PAGE_VA_PROTECTING, old_descriptor_id, &mark_lockset);
        uhm_page_unlock_shards(heap, &mark_lockset);
    }
    if (result != UHM_OK) {
        return result;
    }
    {
        uhm_vm_call_record protect_record;

        uhm_unlock(&heap->lock);
        protect_record = uhm_vm_protect_unlocked_call(
            heap, (void *)payload, requested_size,
            UHM_PROT_READ | UHM_PROT_WRITE);
        uhm_lock(&heap->lock);
        result = uhm_vm_protect_publish_record_locked(heap, &protect_record);
    }
    if (result == UHM_OK &&
        (!uhm_heap_is_accessible_pinned(heap) ||
         block->magic != UHM_BLOCK_MAGIC ||
         (block->flags & UHM_BLOCK_FLAG_FREE) != 0 ||
         (block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) == 0 ||
         block->requested_size != requested_size ||
         block->desc.id != old_descriptor_id ||
         (uintptr_t)uhm_block_payload(block) != payload ||
         !uhm_page_range_matches_malloc_va_state_desc(
             heap, payload, requested_size, UHM_PAGE_VA_PROTECTING,
             old_descriptor_id))) {
        result = UHM_ERR_CORRUPTION;
    }
    if (result != UHM_OK) {
        uhm_lockset restore_lockset;
        uhm_page_lock_shard_range(heap, first, count, &restore_lockset);
        (void)uhm_page_span_flip_malloc_va_state_preserve_locked(
            heap, first, count, UHM_PAGE_VA_PROTECTING,
            UHM_PAGE_VA_MALLOC_LIVE, old_descriptor_id, &restore_lockset);
        uhm_page_unlock_shards(heap, &restore_lockset);
        return result;
    }
    {
        uhm_lockset publish_lockset;
        uhm_page_lock_shard_range(heap, first, count, &publish_lockset);
        uhm_extent_lock_all_shards(heap, &publish_lockset);
        block->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(block);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, first, count, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
            UHM_PROT_READ | UHM_PROT_WRITE, block->desc.id,
            block->generation, &publish_lockset, 0);
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result == UHM_OK) {
        block->flags &= ~UHM_BLOCK_FLAG_PAGE_PRIVATE;
    }
    return result;
}

