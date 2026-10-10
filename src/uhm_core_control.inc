static void uhm_claim_reset(uhm_range_claim *claim) {
    if (claim) {
        memset(claim, 0, sizeof(*claim));
    }
}

static int uhm_claim_is_active(const uhm_range_claim *claim) {
    return claim && claim->source != UHM_CLAIM_SOURCE_NONE;
}

static void uhm_claim_clear_low_watermark_inflight(uhm_heap *heap,
                                                   uhm_range_claim *claim) {
    if (!heap || !claim ||
        (claim->flags & UHM_CLAIM_FLAG_LOW_WATERMARK_INFLIGHT) == 0) {
        return;
    }
    heap->core.low_watermark_claim_inflight = 0u;
    heap->core.low_watermark_claim_addr = 0;
    heap->core.low_watermark_claim_size = 0;
    claim->flags &= ~UHM_CLAIM_FLAG_LOW_WATERMARK_INFLIGHT;
}

static uhm_result uhm_claim_begin_low_watermark_inflight(
    uhm_heap *heap, uhm_range_claim *claim) {
    if (!heap || !claim ||
        claim->source != UHM_CLAIM_SOURCE_LOW_WATERMARK ||
        claim->size == 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (heap->core.low_watermark_claim_inflight != 0u) {
        return UHM_ERR_BUSY;
    }
    heap->core.low_watermark_claim_inflight = 1u;
    heap->core.low_watermark_claim_addr = claim->addr;
    heap->core.low_watermark_claim_size = claim->size;
    claim->flags |= UHM_CLAIM_FLAG_LOW_WATERMARK_INFLIGHT;
    return UHM_OK;
}

static void uhm_claim_disarm(uhm_heap *heap, uhm_range_claim *claim) {
    if (claim) {
        uhm_claim_clear_low_watermark_inflight(heap, claim);
        claim->source = UHM_CLAIM_SOURCE_NONE;
    }
}

static uhm_result uhm_claim_low_watermark(uhm_heap *heap, size_t size,
                                          size_t alignment,
                                          uhm_range_claim *claim) {
    uintptr_t addr;
    uintptr_t end;

    uhm_claim_reset(claim);
    if (!heap || !claim || size == 0 || !uhm_is_power_of_two(alignment)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (heap->core.low_watermark_claim_inflight != 0u) {
        return UHM_ERR_BUSY;
    }
    if (uhm_align_up_uintptr_checked(heap->core.malloc_hi, alignment,
                                     &addr)) {
        return UHM_ERR_OUT_OF_ADDRESS_SPACE;
    }
    if (uhm_range_overflows(addr, size, &end) || end > heap->core.mmap_lo) {
        return UHM_ERR_OUT_OF_ADDRESS_SPACE;
    }
    claim->source = UHM_CLAIM_SOURCE_LOW_WATERMARK;
    claim->addr = addr;
    claim->size = size;
    claim->old_malloc_hi = heap->core.malloc_hi;
    claim->old_mmap_lo = heap->core.mmap_lo;
    heap->core.malloc_hi = end;
    return UHM_OK;
}

static uhm_result uhm_claim_high_watermark(uhm_heap *heap, size_t size,
                                           size_t alignment,
                                           uhm_range_claim *claim) {
    uintptr_t addr;
    size_t available;

    uhm_claim_reset(claim);
    if (!heap || !claim || size == 0 || !uhm_is_power_of_two(alignment)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    available = (size_t)(heap->core.mmap_lo - heap->core.arena_base);
    if (size > available) {
        return UHM_ERR_OUT_OF_ADDRESS_SPACE;
    }
    addr = uhm_align_down_uintptr(heap->core.mmap_lo - size, alignment);
    if (addr < heap->core.malloc_hi || addr < heap->core.control_end) {
        return UHM_ERR_OUT_OF_ADDRESS_SPACE;
    }
    claim->source = UHM_CLAIM_SOURCE_HIGH_WATERMARK;
    claim->addr = addr;
    claim->size = heap->core.mmap_lo - addr;
    claim->old_malloc_hi = heap->core.malloc_hi;
    claim->old_mmap_lo = heap->core.mmap_lo;
    heap->core.mmap_lo = addr;
    return UHM_OK;
}

static uhm_result uhm_claim_fixed_high_watermark(uhm_heap *heap, uintptr_t addr,
                                                 size_t size,
                                                 uhm_range_claim *claim) {
    uintptr_t end;
    uintptr_t old_mmap_lo;
    uhm_result result;

    uhm_claim_reset(claim);
    if (!heap || !claim || size == 0 || uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (end > heap->core.mmap_lo) {
        return UHM_ERR_ALREADY_MAPPED;
    }
    old_mmap_lo = heap->core.mmap_lo;
    claim->source = UHM_CLAIM_SOURCE_HIGH_WATERMARK;
    claim->addr = addr;
    claim->size = size;
    claim->old_malloc_hi = heap->core.malloc_hi;
    claim->old_mmap_lo = old_mmap_lo;
    heap->core.mmap_lo = addr;
    if (end < old_mmap_lo) {
        result = uhm_mmap_free_insert(heap, end, old_mmap_lo - end, 0);
        if (result != UHM_OK) {
            heap->core.mmap_lo = old_mmap_lo;
            uhm_claim_reset(claim);
            return result;
        }
    }
    return UHM_OK;
}

static uhm_result uhm_claim_mmap_fixed(uhm_heap *heap, uintptr_t addr,
                                       size_t size, uhm_range_claim *claim) {
    int claimed = 0;
    uhm_result result;

    result = uhm_mmap_free_claim(heap, addr, size, &claimed);
    if (result != UHM_OK) {
        return result;
    }
    if (claimed) {
        uhm_claim_reset(claim);
        claim->source = UHM_CLAIM_SOURCE_MMAP_FREE;
        claim->addr = addr;
        claim->size = size;
        claim->old_malloc_hi = heap->core.malloc_hi;
        claim->old_mmap_lo = heap->core.mmap_lo;
        return UHM_OK;
    }
    return uhm_claim_fixed_high_watermark(heap, addr, size, claim);
}

static uhm_result uhm_claim_mmap_auto(uhm_heap *heap, size_t size,
                                      uhm_range_claim *claim,
                                      uintptr_t *addr_out) {
    uintptr_t addr = 0;
    uhm_result result;

    if (uhm_mmap_free_take_auto(heap, size, &addr)) {
        uhm_claim_reset(claim);
        claim->source = UHM_CLAIM_SOURCE_MMAP_FREE;
        claim->addr = addr;
        claim->size = size;
        claim->old_malloc_hi = heap->core.malloc_hi;
        claim->old_mmap_lo = heap->core.mmap_lo;
        *addr_out = addr;
        return UHM_OK;
    }
    result = uhm_claim_high_watermark(heap, size, heap->F.vm.page_size, claim);
    if (result == UHM_OK) {
        *addr_out = claim->addr;
    }
    return result;
}

static uhm_result uhm_claim_abort(uhm_heap *heap, uhm_range_claim *claim,
                                  int allow_emergency) {
    uhm_result result = UHM_OK;

    if (!uhm_claim_is_active(claim)) {
        return UHM_OK;
    }
    if (claim->source == UHM_CLAIM_SOURCE_LOW_WATERMARK) {
        uintptr_t end;
        if (uhm_range_overflows(claim->addr, claim->size, &end)) {
            result = UHM_ERR_CORRUPTION;
        } else if (heap->core.malloc_hi != end) {
            result = UHM_ERR_CORRUPTION;
        } else {
            heap->core.malloc_hi = claim->old_malloc_hi;
        }
        uhm_claim_clear_low_watermark_inflight(heap, claim);
    } else if (claim->source == UHM_CLAIM_SOURCE_HIGH_WATERMARK ||
               claim->source == UHM_CLAIM_SOURCE_MMAP_FREE) {
        result = uhm_mmap_free_insert(heap, claim->addr, claim->size,
                                      allow_emergency);
    }
    uhm_claim_reset(claim);
    return result;
}

static void *uhm_control_alloc_from(uintptr_t *alloc, uintptr_t end,
                                    size_t size, size_t alignment) {
    uintptr_t start;
    uintptr_t alloc_end;

    if (uhm_align_up_uintptr_checked(*alloc, alignment, &start)) {
        return 0;
    }
    if (uhm_range_overflows(start, size, &alloc_end) || alloc_end > end) {
        return 0;
    }
    *alloc = alloc_end;
    return (void *)start;
}

static void uhm_diag_note_control_alloc_scan(uhm_heap *heap, size_t steps) {
    if (!heap) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, control_alloc_scan_calls));
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, control_alloc_scan_steps), steps);
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, control_alloc_scan_max_steps),
                          (uint64_t)steps);
    if (steps > UHM_CONTROL_ALLOC_SCAN_BUDGET) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap,
                                    control_alloc_scan_budget_exceeded));
    }
}

static size_t uhm_control_alloc_hint_bucket(size_t size) {
    size_t bucket = 0;

    if (size == 0) {
        return 0;
    }
    --size;
    while (size != 0 && bucket + 1u < UHM_CONTROL_ALLOC_HINT_BUCKETS) {
        size >>= 1u;
        ++bucket;
    }
    return bucket;
}

static void uhm_control_alloc_hint_clear_position(uhm_control_extent *extent) {
    if (!extent) {
        return;
    }
    extent->flags &= ~UHM_CONTROL_EXTENT_HINT_MASK;
}

static void uhm_control_alloc_hint_set_position(uhm_control_extent *extent,
                                                size_t bucket, size_t slot) {
    uint32_t flags;

    if (!extent || bucket >= UHM_CONTROL_ALLOC_HINT_BUCKETS ||
        slot >= UHM_CONTROL_ALLOC_HINT_SLOTS) {
        return;
    }
    flags = extent->flags & ~UHM_CONTROL_EXTENT_HINT_MASK;
    flags |= UHM_CONTROL_EXTENT_FLAG_HINTED;
    flags |= ((uint32_t)bucket & UHM_CONTROL_EXTENT_HINT_INDEX_MASK)
             << UHM_CONTROL_EXTENT_HINT_BUCKET_SHIFT;
    flags |= ((uint32_t)slot & UHM_CONTROL_EXTENT_HINT_INDEX_MASK)
             << UHM_CONTROL_EXTENT_HINT_SLOT_SHIFT;
    extent->flags = flags;
}

static int uhm_control_alloc_hint_position(const uhm_control_extent *extent,
                                           size_t *bucket_out,
                                           size_t *slot_out) {
    size_t bucket;
    size_t slot;

    if (!extent || (extent->flags & UHM_CONTROL_EXTENT_FLAG_HINTED) == 0) {
        return 0;
    }
    bucket = (size_t)((extent->flags & UHM_CONTROL_EXTENT_HINT_BUCKET_MASK) >>
                      UHM_CONTROL_EXTENT_HINT_BUCKET_SHIFT);
    slot = (size_t)((extent->flags & UHM_CONTROL_EXTENT_HINT_SLOT_MASK) >>
                    UHM_CONTROL_EXTENT_HINT_SLOT_SHIFT);
    if (bucket >= UHM_CONTROL_ALLOC_HINT_BUCKETS ||
        slot >= UHM_CONTROL_ALLOC_HINT_SLOTS) {
        return 0;
    }
    if (bucket_out) {
        *bucket_out = bucket;
    }
    if (slot_out) {
        *slot_out = slot;
    }
    return 1;
}

static int uhm_control_alloc_hint_position_matches(
    uhm_heap *heap, const uhm_control_extent *extent) {
    size_t bucket;
    size_t slot;

    if (!heap ||
        !uhm_control_alloc_hint_position(extent, &bucket, &slot)) {
        return 0;
    }
    return heap->core.control_alloc_hint[bucket][slot] == extent;
}

static void uhm_control_alloc_hint_remove_at(uhm_heap *heap, size_t bucket,
                                             size_t slot) {
    size_t i;
    uhm_control_extent *removed;

    if (!heap || bucket >= UHM_CONTROL_ALLOC_HINT_BUCKETS ||
        slot >= UHM_CONTROL_ALLOC_HINT_SLOTS) {
        return;
    }
    removed = heap->core.control_alloc_hint[bucket][slot];
    if (removed && removed->magic == UHM_CONTROL_MAGIC) {
        uhm_control_alloc_hint_clear_position(removed);
    }
    for (i = slot; i + 1u < UHM_CONTROL_ALLOC_HINT_SLOTS; ++i) {
        uhm_control_extent *moved =
            heap->core.control_alloc_hint[bucket][i + 1u];
        heap->core.control_alloc_hint[bucket][i] =
            moved;
        if (moved && moved->magic == UHM_CONTROL_MAGIC) {
            uhm_control_alloc_hint_set_position(moved, bucket, i);
        }
    }
    heap->core.control_alloc_hint[bucket][UHM_CONTROL_ALLOC_HINT_SLOTS - 1u] =
        0;
}

static void uhm_control_alloc_hint_forget(uhm_heap *heap,
                                          const uhm_control_extent *extent) {
    size_t bucket;
    size_t slot;

    if (!heap || !extent) {
        return;
    }
    if (extent->magic == UHM_CONTROL_MAGIC &&
        uhm_control_alloc_hint_position(extent, &bucket, &slot) &&
        heap->core.control_alloc_hint[bucket][slot] == extent) {
        uhm_control_alloc_hint_remove_at(heap, bucket, slot);
        return;
    }
    for (bucket = 0; bucket < UHM_CONTROL_ALLOC_HINT_BUCKETS; ++bucket) {
        slot = 0;
        while (slot < UHM_CONTROL_ALLOC_HINT_SLOTS) {
            if (heap->core.control_alloc_hint[bucket][slot] == extent) {
                uhm_control_alloc_hint_remove_at(heap, bucket, slot);
                continue;
            }
            ++slot;
        }
    }
    if (extent->magic == UHM_CONTROL_MAGIC) {
        uhm_control_alloc_hint_clear_position((uhm_control_extent *)extent);
    }
}

static void uhm_control_alloc_hint_push(uhm_heap *heap, size_t bucket,
                                        uhm_control_extent *extent) {
    size_t slot;
    uhm_control_extent *dropped;

    if (!heap || !extent || bucket >= UHM_CONTROL_ALLOC_HINT_BUCKETS) {
        return;
    }
    dropped = heap->core.control_alloc_hint[bucket]
                                          [UHM_CONTROL_ALLOC_HINT_SLOTS - 1u];
    if (dropped && dropped->magic == UHM_CONTROL_MAGIC) {
        uhm_control_alloc_hint_clear_position(dropped);
    }
    for (slot = UHM_CONTROL_ALLOC_HINT_SLOTS - 1u; slot != 0; --slot) {
        uhm_control_extent *moved =
            heap->core.control_alloc_hint[bucket][slot - 1u];
        heap->core.control_alloc_hint[bucket][slot] =
            moved;
        if (moved && moved->magic == UHM_CONTROL_MAGIC) {
            uhm_control_alloc_hint_set_position(moved, bucket, slot);
        }
    }
    heap->core.control_alloc_hint[bucket][0] = extent;
    uhm_control_alloc_hint_set_position(extent, bucket, 0);
}

/*
 * Re-record the extent after an allocation.  A return value of one is an
 * internal proof that the extent was already the MRU entry in the same size
 * bucket and therefore needed no hint-window writes.
 */
static int uhm_control_alloc_hint_note(uhm_heap *heap,
                                       uhm_control_extent *extent) {
    size_t remaining;
    size_t bucket;
    size_t old_bucket;
    size_t old_slot;

    if (!heap || !extent || extent->magic != UHM_CONTROL_MAGIC ||
        extent->alloc >= extent->end) {
        uhm_control_alloc_hint_forget(heap, extent);
        return 0;
    }
    remaining = (size_t)(extent->end - extent->alloc);
    if (remaining == 0) {
        uhm_control_alloc_hint_forget(heap, extent);
        return 0;
    }
    bucket = uhm_control_alloc_hint_bucket(remaining);
    if (uhm_control_alloc_hint_position(extent, &old_bucket, &old_slot) &&
        old_bucket < UHM_CONTROL_ALLOC_HINT_BUCKETS &&
        old_slot < UHM_CONTROL_ALLOC_HINT_SLOTS &&
        old_bucket == bucket && old_slot == 0 &&
        heap->core.control_alloc_hint[old_bucket][old_slot] == extent) {
        return 1;
    }
    uhm_control_alloc_hint_forget(heap, extent);
    uhm_control_alloc_hint_push(heap, bucket, extent);
    return 0;
}

#if UHMAS_ENABLE_TEST_HOOKS
int uhm_test_control_alloc_hint_note(uhm_heap *heap,
                                     uhm_control_extent *extent) {
    return uhm_control_alloc_hint_note(heap, extent);
}
#endif

static int uhm_control_alloc_no_fit_covers(uhm_heap *heap, size_t size,
                                           size_t alignment,
                                           int recent_failed) {
    if (!heap || !recent_failed || heap->core.control_extent_count == 0 ||
        heap->core.control_no_fit_extent_count == 0) {
        return 0;
    }
    if (heap->core.control_no_fit_extent_count + 1u <
        heap->core.control_extent_count) {
        return 0;
    }
    return size >= heap->core.control_no_fit_size &&
           alignment >= heap->core.control_no_fit_alignment;
}

static void uhm_control_alloc_note_no_fit(uhm_heap *heap, size_t size,
                                          size_t alignment) {
    if (!heap) {
        return;
    }
    heap->core.control_no_fit_extent_count = heap->core.control_extent_count;
    heap->core.control_no_fit_size = size;
    heap->core.control_no_fit_alignment = alignment;
}

static void *uhm_control_alloc_existing_raw(uhm_heap *heap, size_t size,
                                            size_t alignment) {
    uhm_control_extent *extent;
    void *ptr;
    size_t scan_steps = 0;
    size_t bucket;
    int recent_failed = 0;

    ptr = uhm_control_alloc_from(&heap->core.control_alloc,
                                 heap->core.control_end, size, alignment);
    if (ptr) {
        return ptr;
    }

    extent = heap->core.control_recent_extent;
    if (extent && extent->magic == UHM_CONTROL_MAGIC) {
        ptr = uhm_control_alloc_from(&extent->alloc, extent->end, size,
                                     alignment);
        if (ptr) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, control_alloc_recent_hits));
            uhm_control_alloc_hint_note(heap, extent);
            return ptr;
        }
        uhm_control_alloc_hint_note(heap, extent);
        recent_failed = 1;
    }

    /* A full-scan no-fit proof dominates all existing hint entries.  New
       extents, when present, are the recent extent checked immediately
       above; skipping the old hint window avoids rereading known failures. */
    if (uhm_control_alloc_no_fit_covers(heap, size, alignment,
                                        recent_failed)) {
        uhm_control_alloc_note_no_fit(heap, size, alignment);
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, control_alloc_no_fit_fast_paths));
        return 0;
    }

    for (bucket = uhm_control_alloc_hint_bucket(size);
         bucket < UHM_CONTROL_ALLOC_HINT_BUCKETS; ++bucket) {
        size_t slot = 0;
        while (slot < UHM_CONTROL_ALLOC_HINT_SLOTS) {
            extent = heap->core.control_alloc_hint[bucket][slot];
            if (!extent) {
                break;
            }
            if (extent->magic != UHM_CONTROL_MAGIC) {
                uhm_control_alloc_hint_remove_at(heap, bucket, slot);
                uhm_diag_note_u64_inc(
                    UHM_DIAG_CORE_FIELD_PTR(heap, control_alloc_hint_stale));
                continue;
            }
            if (extent == heap->core.control_recent_extent) {
                ++slot;
                continue;
            }
            ptr = uhm_control_alloc_from(&extent->alloc, extent->end, size,
                                         alignment);
            if (ptr) {
                uhm_diag_note_u64_inc(
                    UHM_DIAG_CORE_FIELD_PTR(heap, control_alloc_hint_hits));
                heap->core.control_recent_extent = extent;
                uhm_control_alloc_hint_note(heap, extent);
                return ptr;
            }
            uhm_control_alloc_hint_remove_at(heap, bucket, slot);
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, control_alloc_hint_misses));
        }
    }

    for (extent = heap->core.control_extents; extent; extent = extent->next) {
        ++scan_steps;
        if (extent->magic != UHM_CONTROL_MAGIC) {
            continue;
        }
        if (extent == heap->core.control_recent_extent) {
            continue;
        }
        ptr = uhm_control_alloc_from(&extent->alloc, extent->end, size,
                                     alignment);
        if (ptr) {
            heap->core.control_recent_extent = extent;
            uhm_control_alloc_hint_note(heap, extent);
            uhm_diag_note_control_alloc_scan(heap, scan_steps);
            return ptr;
        }
        uhm_control_alloc_hint_note(heap, extent);
    }
    if (scan_steps != 0) {
        uhm_diag_note_control_alloc_scan(heap, scan_steps);
        uhm_control_alloc_note_no_fit(heap, size, alignment);
    }
    return 0;
}

static void *uhm_control_alloc_existing_zeroed(uhm_heap *heap, size_t size,
                                               size_t alignment) {
    void *ptr = uhm_control_alloc_existing_raw(heap, size, alignment);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

/* The caller must initialize every field before publishing this object. */
static void *uhm_control_alloc_existing_uninitialized(uhm_heap *heap,
                                                     size_t size,
                                                     size_t alignment) {
    return uhm_control_alloc_existing_raw(heap, size, alignment);
}

static int uhm_control_extent_size(uhm_heap *heap, size_t requested,
                                   size_t alignment, size_t *out_size) {
    size_t need;
    size_t extent_size;
    size_t leaf_reserve;
    size_t min_extent;

    if (uhm_add_overflows_size(sizeof(uhm_control_extent), alignment - 1u, &need) ||
        uhm_add_overflows_size(need, requested, &need)) {
        return 0;
    }
    leaf_reserve = sizeof(uhm_page_leaf) * 2u;
    if (uhm_add_overflows_size(need, leaf_reserve, &need)) {
        return 0;
    }

    min_extent = uhm_max_size(UHM_MIN_CONTROL_BAND, 16u * heap->F.vm.page_size);
    extent_size = uhm_max_size(min_extent, need);
    if (uhm_align_up_size(extent_size, heap->F.vm.page_size, &extent_size)) {
        return 0;
    }
    *out_size = extent_size;
    return 1;
}

enum {
    UHM_CONTROL_CARVE_FAILED = 0,
    UHM_CONTROL_CARVE_EXTENT = 1,
    UHM_CONTROL_CARVE_WAITED = 2
};

static int uhm_control_extent_carve(uhm_heap *heap, size_t requested, size_t alignment) {
    size_t extent_size;
    uintptr_t base;
    uintptr_t end;
    uhm_control_extent *extent;
    uhm_range_claim claim;
    uhm_vm_call_record commit_record;
    uhm_result result;
    uint64_t current_thread_id;

    memset(&commit_record, 0, sizeof(commit_record));
    if (heap->core.control_expanding) {
        current_thread_id = uhm_current_thread_id();
        if (heap->core.control_expanding_owner_thread_id == current_thread_id) {
            return UHM_CONTROL_CARVE_FAILED;
        }
        do {
            uhm_unlock(&heap->lock);
            uhm_spinlock_yield();
            uhm_lock(&heap->lock);
            if (!uhm_heap_is_accessible_pinned(heap)) {
                return UHM_CONTROL_CARVE_FAILED;
            }
        } while (heap->core.control_expanding &&
                 heap->core.control_expanding_owner_thread_id != current_thread_id);
        if (heap->core.control_expanding) {
            return UHM_CONTROL_CARVE_FAILED;
        }
        return UHM_CONTROL_CARVE_WAITED;
    }
    if (!uhm_control_extent_size(heap, requested, alignment, &extent_size)) {
        return UHM_CONTROL_CARVE_FAILED;
    }

    for (;;) {
        result = uhm_claim_low_watermark(heap, extent_size,
                                         heap->F.vm.page_size, &claim);
        if (result != UHM_ERR_BUSY) {
            break;
        }
        uhm_unlock(&heap->lock);
        uhm_spinlock_yield();
        uhm_lock(&heap->lock);
        if (!uhm_heap_is_accessible_pinned(heap) ||
            heap->core.control_expanding) {
            return UHM_CONTROL_CARVE_FAILED;
        }
    }
    if (result != UHM_OK) {
        return UHM_CONTROL_CARVE_FAILED;
    }
    base = claim.addr;
    if (uhm_range_overflows(claim.addr, claim.size, &end)) {
        (void)uhm_claim_abort(heap, &claim, 0);
        return UHM_CONTROL_CARVE_FAILED;
    }

    heap->core.control_expanding = 1u;
    heap->core.control_expanding_owner_thread_id = uhm_current_thread_id();
    result = uhm_claim_begin_low_watermark_inflight(heap, &claim);
    if (result != UHM_OK) {
        (void)uhm_claim_abort(heap, &claim, 0);
        heap->core.control_expanding = 0u;
        heap->core.control_expanding_owner_thread_id = 0;
        return UHM_CONTROL_CARVE_FAILED;
    }
    uhm_unlock(&heap->lock);
    commit_record = uhm_vm_commit_unlocked_call(
        heap, (void *)base, extent_size, UHM_PROT_READ | UHM_PROT_WRITE);
    uhm_lock(&heap->lock);
    result = uhm_vm_commit_publish_record_locked(heap, &commit_record);
    if (result != UHM_OK ||
        !uhm_heap_is_accessible_pinned(heap) ||
        heap->core.control_expanding != 1u ||
        heap->core.low_watermark_claim_inflight == 0u ||
        heap->core.low_watermark_claim_addr != claim.addr ||
        heap->core.low_watermark_claim_size != claim.size) {
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)base, extent_size);
        (void)uhm_claim_abort(heap, &claim, 0);
        heap->core.control_expanding = 0u;
        heap->core.control_expanding_owner_thread_id = 0;
        return UHM_CONTROL_CARVE_FAILED;
    }

    extent = (uhm_control_extent *)base;
    memset(extent, 0, sizeof(*extent));
    extent->magic = UHM_CONTROL_MAGIC;
    extent->base = base;
    extent->end = end;
    if (uhm_range_overflows(base, sizeof(*extent), &extent->alloc) ||
        uhm_align_up_uintptr_checked(extent->alloc, UHM_MIN_ALIGNMENT,
                                     &extent->alloc) ||
        extent->alloc > end) {
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)base, extent_size);
        (void)uhm_claim_abort(heap, &claim, 0);
        heap->core.control_expanding = 0u;
        heap->core.control_expanding_owner_thread_id = 0;
        return UHM_CONTROL_CARVE_FAILED;
    }
    uhm_extent_descriptor_set(&extent->desc, UHM_EXTENT_KIND_CONTROL,
                              base, extent_size, UHM_PAGE_OWNER_CONTROL,
                              UHM_PAGE_VA_CONTROL_LIVE, UHM_PAGE_COMMIT_LIVE,
                              UHM_PROT_READ | UHM_PROT_WRITE,
                              uhm_heap_generation_next(heap), extent);
    extent->next = heap->core.control_extents;
    heap->core.control_extents = extent;
    heap->core.control_recent_extent = extent;
    ++heap->core.control_extent_count;
    uhm_control_alloc_hint_note(heap, extent);
    heap->core.control_bytes += extent_size;
    heap->stats.committed_bytes += extent_size;

    result = uhm_page_mark_range(heap, base, extent_size, UHM_PAGE_OWNER_CONTROL,
                                 UHM_PAGE_VA_CONTROL_LIVE, UHM_PAGE_COMMIT_LIVE,
                                 UHM_PROT_READ | UHM_PROT_WRITE, extent->desc.id,
                                 extent->desc.generation, 1);
    if (result != UHM_OK) {
        heap->core.control_extents = extent->next;
        if (heap->core.control_recent_extent == extent) {
            heap->core.control_recent_extent = heap->core.control_extents;
        }
        if (heap->core.control_extent_count != 0) {
            --heap->core.control_extent_count;
        }
        if (heap->core.control_no_fit_extent_count >
            heap->core.control_extent_count) {
            heap->core.control_no_fit_extent_count =
                heap->core.control_extent_count;
        }
        uhm_control_alloc_hint_forget(heap, extent);
        if (heap->core.control_bytes >= extent_size) {
            heap->core.control_bytes -= extent_size;
        } else {
            heap->core.control_bytes = 0;
        }
        if (heap->stats.committed_bytes >= extent_size) {
            heap->stats.committed_bytes -= extent_size;
        } else {
            heap->stats.committed_bytes = 0;
        }
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)base, extent_size);
        (void)uhm_claim_abort(heap, &claim, 0);
        heap->core.control_expanding = 0u;
        heap->core.control_expanding_owner_thread_id = 0;
        return UHM_CONTROL_CARVE_FAILED;
    }

    uhm_claim_disarm(heap, &claim);
    heap->core.control_expanding = 0u;
    heap->core.control_expanding_owner_thread_id = 0;
    return UHM_CONTROL_CARVE_EXTENT;
}

static void *uhm_control_alloc_raw(uhm_heap *heap, size_t size,
                                   size_t alignment) {
    void *ptr;
    int carve_result;

    if (!uhm_is_power_of_two(alignment) || alignment < UHM_MIN_ALIGNMENT) {
        alignment = UHM_MIN_ALIGNMENT;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.fault_control_alloc_countdown != 0 &&
        --heap->core.fault_control_alloc_countdown == 0) {
        return 0;
    }
#endif

    for (;;) {
        ptr = uhm_control_alloc_existing_raw(heap, size, alignment);
        if (ptr) {
            return ptr;
        }
        carve_result = uhm_control_extent_carve(heap, size, alignment);
        if (carve_result == UHM_CONTROL_CARVE_FAILED) {
            return 0;
        }
        if (carve_result == UHM_CONTROL_CARVE_WAITED) {
            continue;
        }
        break;
    }
    return uhm_control_alloc_existing_raw(heap, size, alignment);
}

/* The caller must initialize every field before publishing this object. */
static void *uhm_control_alloc_uninitialized(uhm_heap *heap, size_t size,
                                             size_t alignment) {
    return uhm_control_alloc_raw(heap, size, alignment);
}

static uhm_result uhm_emergency_reserve_init(uhm_heap *heap) {
    size_t i;

    for (i = 0; i < UHM_EMERGENCY_VMA_NODES; ++i) {
        uhm_vma *node = (uhm_vma *)uhm_control_alloc_existing_zeroed(
            heap, sizeof(*node), alignof(uhm_vma));
        if (!node) {
            return UHM_ERR_OUT_OF_MEMORY;
        }
        node->flags = UHM_META_FLAG_EMERGENCY;
        node->next = heap->core.emergency_vma_head;
        heap->core.emergency_vma_head = node;
        ++heap->core.emergency_vma_available;
    }

    for (i = 0; i < UHM_EMERGENCY_FREE_EXTENT_NODES; ++i) {
        uhm_free_extent *node =
            (uhm_free_extent *)uhm_control_alloc_existing_zeroed(
                heap, sizeof(*node), alignof(uhm_free_extent));
        if (!node) {
            return UHM_ERR_OUT_OF_MEMORY;
        }
        node->flags = UHM_META_FLAG_EMERGENCY;
        node->next = heap->core.emergency_free_extent_head;
        heap->core.emergency_free_extent_head = node;
        ++heap->core.emergency_free_extent_available;
    }

    return UHM_OK;
}

