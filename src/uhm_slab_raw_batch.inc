/* Included only by the 32-bit low-lock allocator. Caller owns raw_lock. */
static int uhm_slab_raw_slot_address_valid(const uhm_slab *slab,
                                           const uhm_slab_slot *slot) {
    uintptr_t addr = (uintptr_t)slot;
    size_t offset;

    if (!slot || slab->slot_stride == 0u || slab->total_slots == 0u ||
        slab->size > UINTPTR_MAX - slab->base || addr < slab->base ||
        addr >= slab->base + slab->size) {
        return 0;
    }
    offset = (size_t)(addr - slab->base);
    return offset % slab->slot_stride == 0u &&
           offset / slab->slot_stride < slab->total_slots;
}

static uhm_result uhm_slab_raw_remote_publish_batch_locked(
    uhm_heap *heap, uhm_slab *slab) {
    const uint32_t queued = UHM_SLAB_SLOT_FLAG_FREE |
                            UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE;
    uhm_slab_slot *nodes[UHM_RAW_REMOTE_BATCH_LIMIT];
    uhm_slab_slot *after = slab->remote_free_head;
    size_t count = 0u;
    size_t i;
    int accounted = 0;
    uhm_result result = UHM_OK;

    if (!after) {
        return UHM_OK;
    }
    if (!uhm_slab_raw_slot_address_valid(slab, after)) {
        return UHM_ERR_CORRUPTION;
    }
    if (uhm_slab_remote_drain_active(slab)) {
        return UHM_ERR_BUSY;
    }
    /* Caller already pins slab through its metadata reader and hazard[1].
     * The raw lock owns every source node until publication, including
     * against a heap-lock drain. No detached reservation needs an owner. */
    uhm_slab_raw_publish_begin(slab);
    while (after && count < UHM_RAW_REMOTE_BATCH_LIMIT) {
        uint64_t state;

        if (!uhm_slab_raw_slot_address_valid(slab, after)) {
            result = UHM_ERR_CORRUPTION;
            goto restore;
        }
        state = uhm_slab_slot_state_load(after);
        if (after->magic != UHM_SLAB_SLOT_MAGIC || after->slab != slab ||
            after->size_class != slab->size_class || after->state != 0u ||
            state != uhm_slab_slot_state_pack(queued, 0u) ||
            after->flags != queued || after->requested_size != 0u) {
            result = UHM_ERR_CORRUPTION;
            goto restore;
        }
        nodes[count++] = after;
        after = after->next_free;
    }
    if (after) {
        if (!uhm_slab_raw_slot_address_valid(slab, after)) {
            result = UHM_ERR_CORRUPTION;
            goto restore;
        }
        for (i = 0u; i < count; ++i) {
            if (nodes[i] == after) {
                result = UHM_ERR_CORRUPTION;
                goto restore;
            }
        }
    }
    if (count > slab->free_slots) {
        result = UHM_ERR_CORRUPTION;
        goto restore;
    }
    result = uhm_slab_remote_counters_decrement_batch(slab, count, 0u);
    if (result != UHM_OK) {
        goto restore;
    }
    accounted = 1;
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.fault_lockless_remote_drain_countdown != 0u &&
        --heap->core.fault_lockless_remote_drain_countdown == 0u) {
        result = UHM_ERR_OUT_OF_MEMORY;
        goto restore;
    }
#endif
    /* The raw lock hides the entire transaction. No detached chain escapes
     * it, so per-slot DRAINING/ACCOUNTED_OUT transitions are unnecessary. */
    slab->remote_free_head = after;
    /* Reverse publication preserves the remote list's LIFO reuse order. */
    for (i = count; i != 0u; --i) {
        uhm_slab_slot *slot = nodes[i - 1u];
        uhm_slab_slot_state_store_locked(slot, UHM_SLAB_SLOT_FLAG_FREE, 0u);
        uhm_tagged_freelist_next_store(&slot->remote_link, 0);
        slot->next_free = slab->free_head;
        slab->free_head = slot;
    }
    /* Caller publishes the free head and allocation counts together. */
    goto done;

restore:
    if (accounted) {
        for (i = 0u; i < count; ++i) {
            uhm_slab_remote_pending_increment(slab);
        }
    }
done:
    uhm_slab_raw_publish_end(slab);
#if !UHMAS_ENABLE_TEST_HOOKS
    (void)heap;
#endif
    return result;
}
