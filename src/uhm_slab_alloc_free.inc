static void *uhm_slab_alloc(uhm_heap *heap, uint16_t size_class,
                            size_t slot_size, size_t requested_size,
                            int drain_class_queue,
                            uhm_result *out_result) {
    uhm_slab *slab;
    uhm_slab_slot *slot;
    uhm_result result;

    if (out_result) {
        *out_result = UHM_OK;
    }

    result = uhm_slab_drain_remote_class(heap, size_class,
                                         drain_class_queue);
    if (result != UHM_OK) {
        if (result == UHM_ERR_CORRUPTION) {
            uhm_debug_corruption_site(heap, 5101u);
        }
        if (out_result) {
            *out_result = result;
        }
        return 0;
    }
    slab = uhm_slab_select_partial(heap, size_class);
    if (!slab) {
        slab = uhm_slab_select_empty(heap, size_class);
    }
    if (!slab) {
        slab = uhm_slab_new(heap, size_class, slot_size);
    }
    if (!slab) {
        if (out_result) {
            *out_result = UHM_ERR_OUT_OF_MEMORY;
        }
        return 0;
    }
    result = uhm_slab_recommit(heap, slab);
    if (result != UHM_OK) {
        if (out_result) {
            *out_result = result;
        }
        return 0;
    }
    uhm_slab_raw_lock(slab);
    if (!slab->free_head) {
        uhm_slab_raw_unlock(slab);
        uhm_debug_corruption_site(heap, 5102u);
        if (out_result) {
            *out_result = UHM_ERR_CORRUPTION;
        }
        return 0;
    }

    slot = slab->free_head;
    if (!uhm_slab_slot_state_mirror_matches(slot) ||
        uhm_slab_slot_flags_load(slot) != UHM_SLAB_SLOT_FLAG_FREE ||
        uhm_slab_slot_requested_load(slot) != 0) {
        uhm_slab_raw_unlock(slab);
        uhm_debug_corruption_site(heap, 5103u);
        if (out_result) {
            *out_result = UHM_ERR_CORRUPTION;
        }
        return 0;
    }
    if (!uhm_slab_slot_state_cas(slot, UHM_SLAB_SLOT_FLAG_FREE, 0u, 0u,
                                 (uint32_t)requested_size)) {
        uhm_slab_raw_unlock(slab);
        uhm_debug_corruption_site(heap, 5104u);
        if (out_result) {
            *out_result = UHM_ERR_CORRUPTION;
        }
        return 0;
    }
    slab->free_head = slot->next_free;
    slot->next_free = 0;
    --slab->free_slots;
    ++slab->live_slots;
    uhm_slab_refresh_state_raw_locked(heap, slab);
    uhm_slab_raw_unlock(slab);
    uhm_stats_malloc_live_add(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                              requested_size);
    return uhm_slab_slot_payload(slot);
}

static uhm_result uhm_slab_remote_free_slot_locked(uhm_heap *heap,
                                                   uhm_slab_slot *slot) {
    uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
    size_t requested;
    uhm_result result;
    uint32_t flags;
    uint32_t requested32;
    int raw_fallback_published = 0;
    int raw_ready_candidate = 0;

    if (!heap || !slot || !slab || slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
        return UHM_ERR_CORRUPTION;
    }
    uhm_slab_raw_lock(slab);
    if (!uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested32)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    requested = requested32;
    if (!uhm_slab_slot_state_cas(
            slot, flags, (uint32_t)requested,
            UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE, 0u)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    uhm_stats_malloc_live_sub(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                              requested);
    slot->next_free = 0;
    uhm_tagged_freelist_next_store(&slot->remote_link, 0);
    if (uhm_heap_tagged_freelist_push(heap, &slab->remote_free_lf,
                                      &slot->remote_link, 0u)) {
        uhm_slab_remote_ready_note(heap, slab, 0u);
    } else {
        slot->next_free = slab->remote_free_head;
        slab->remote_free_head = slot;
        raw_fallback_published = 1;
    }
    if (slab->live_slots != 0) {
        --slab->live_slots;
    }
    ++slab->free_slots;
    uhm_slab_remote_pending_increment(slab);
    uhm_slab_refresh_state_raw_locked(heap, slab);
    raw_ready_candidate = slab->state == UHM_SLAB_STATE_PARTIAL;
    uhm_slab_raw_unlock(slab);
    if (raw_fallback_published) {
        if (!raw_ready_candidate ||
            !uhm_slab_remote_raw_ready_note(heap, slab)) {
            uhm_slab_remote_pending_note(heap, slab->size_class);
        }
    }
    result = uhm_slab_maybe_decommit_empty(heap, slab);
    if (result == UHM_ERR_UNSUPPORTED || result == UHM_ERR_PLATFORM) {
        return UHM_OK;
    }
    return result;
}

/*
 * 32-bit fallback queue path.  The raw slab lock protects the non-atomic
 * remote list and slab counters; no state-list transition or VM operation is
 * allowed here.  Callers only use this for a PARTIAL slab that remains
 * PARTIAL after the free.
 */
static uhm_result uhm_slab_remote_free_slot_prepare_partial_low_lock(
    uhm_heap *heap,
    uhm_slab_slot *slot,
    uhm_slab **out_slab) {
    uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
    uint32_t flags;
    uint32_t requested32;

    if (out_slab) {
        *out_slab = 0;
    }
    if (!heap || !slot || !slab || slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0u) {
        return UHM_ERR_CORRUPTION;
    }
    if ((slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0u) {
        return UHM_ERR_UNSUPPORTED;
    }
    if (!uhm_slab_descriptor_live_for_lockless(heap, slab)) {
        return UHM_ERR_CORRUPTION;
    }

    uhm_slab_raw_lock(slab);
    if (slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0u ||
        slab->state != UHM_SLAB_STATE_PARTIAL || slab->live_slots <= 1u ||
        slab->free_slots == 0u) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_UNSUPPORTED;
    }
    if ((slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0u) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_UNSUPPORTED;
    }
    if (!uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested32)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    uhm_slab_slot_state_store_locked(
        slot, UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE, 0u);
    uhm_stats_malloc_live_sub(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                              (size_t)requested32);
    slot->next_free = slab->remote_free_head;
    slab->remote_free_head = slot;
    --slab->live_slots;
    ++slab->free_slots;
    uhm_slab_remote_pending_increment(slab);
    uhm_slab_summary_publish_counts_raw_locked(slab);
    if (out_slab) {
        *out_slab = slab;
    }
    uhm_slab_raw_unlock(slab);
    if (!uhm_slab_remote_raw_ready_note(heap, slab)) {
        uhm_slab_remote_pending_note(heap, slab->size_class);
    }
    return UHM_OK;
}

#if UINTPTR_MAX == UINT32_MAX
#include "uhm_slab_raw_batch.inc"

/*
 * On 32-bit targets the raw remote list is the fallback representation for
 * a non-packable pointer queue.  Reuse one slot only while the slab remains
 * PARTIAL after the allocation; transitions to FULL still belong to the
 * heap-lock path because they update the class state list.
 */
static void *uhm_slab_try_alloc_raw_remote_32bit(
    uhm_heap *heap,
    uint16_t size_class,
    size_t requested_size,
    uhm_result *out_result) {
    uhm_metadata_reader_state *reader;
    size_t scanned;

    if (out_result) {
        *out_result = UHM_OK;
    }
    if (!uhm_slab_remote_raw_ready_supported(heap) ||
        size_class >= UHM_SIZE_CLASS_COUNT || requested_size == 0u ||
        requested_size > UINT32_MAX) {
        return 0;
    }
    reader = uhm_lockless_remote_reader_acquire(heap);
    if (!reader) {
        return 0;
    }

    for (scanned = 0u; scanned < UHM_LOCKLESS_REMOTE_SCAN_LIMIT;
         ++scanned) {
        uhm_result take_result = UHM_OK;
        uhm_slab *slab = uhm_slab_remote_raw_ready_take(
            heap, size_class, reader, &take_result);
        uhm_slab_slot *slot;
        uint32_t expected_slot_flags;
        int requeue = 0;
        int consumed_remote = 0;

        if (!slab) {
            if (take_result != UHM_OK && out_result) {
                *out_result = take_result;
            }
            break;
        }
        if (slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class) {
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        /*
         * EMPTY/FULL and cold-page descriptor transitions belong to the
         * heap-lock path. Keep their notification reachable for that drain;
         * reading a cold descriptor here could race a concurrent recommit.
         */
        if (atomic_load_explicit(&slab->state_mirror, UHM_ATOMIC_ACQUIRE) !=
            UHM_SLAB_STATE_PARTIAL) {
            if (!uhm_slab_remote_raw_ready_note(heap, slab)) {
                uhm_slab_remote_pending_note(heap, size_class);
            }
            break;
        }
        if ((slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0u ||
            !uhm_slab_descriptor_live_for_lockless(heap, slab)) {
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }

        uhm_slab_raw_lock(slab);
        if (slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class ||
            (slab->flags & (UHM_SLAB_FLAG_DECOMMITTED |
                            UHM_SLAB_FLAG_VM_BUSY)) != 0u ||
            slab->state != UHM_SLAB_STATE_PARTIAL ||
            slab->live_slots == 0u || slab->free_slots <= 1u ||
            slab->free_slots > slab->total_slots ||
            (slab->remote_free_head == 0 && slab->free_head == 0)) {
            requeue = slab->remote_free_head != 0;
            uhm_slab_raw_unlock(slab);
            if (requeue) {
                uhm_slab_remote_pending_note(heap, size_class);
            }
            continue;
        }

        slot = slab->remote_free_head;
        if (slot && uhm_slab_raw_slot_address_valid(slab, slot) &&
            slot->next_free) {
            uhm_result batch_result = uhm_slab_raw_remote_publish_batch_locked(
                heap, slab);
            if (batch_result != UHM_OK) {
                uhm_slab_raw_unlock(slab);
                if (!uhm_slab_remote_raw_ready_note(heap, slab)) {
                    uhm_slab_remote_pending_note(heap, size_class);
                }
                if (batch_result == UHM_ERR_BUSY) {
                    break;
                }
                uhm_lockless_remote_reader_release(reader);
                if (out_result) {
                    *out_result = batch_result;
                }
                return 0;
            }
            slot = slab->free_head;
        } else if (slot) {
            consumed_remote = 1;
        } else {
            slot = slab->free_head;
        }
        expected_slot_flags = UHM_SLAB_SLOT_FLAG_FREE |
            (consumed_remote ? UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE : 0u);
        /* One atomic snapshot under raw_lock replaces separate mirror,
         * flags and requested-size loads, including non-lock-free u64. */
        if (!uhm_slab_raw_slot_address_valid(slab, slot) ||
            slot->magic != UHM_SLAB_SLOT_MAGIC ||
            !uhm_slab_slot_belongs_to_verified_slab(slot, slab) ||
            slot->size_class != size_class || slot->state != 0 ||
            uhm_slab_slot_state_load(slot) !=
                uhm_slab_slot_state_pack(expected_slot_flags, 0u) ||
            slot->flags != expected_slot_flags || slot->requested_size != 0u) {
            uhm_slab_raw_unlock(slab);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }

        if (consumed_remote) {
            /* Account while the raw lock still owns the queued slot. A
             * missing pending/mirror must leave the live state unpublished,
             * and the original notification reachable for repair/retry. */
            uhm_result account_result = uhm_slab_remote_pending_decrement(slab);
            if (account_result != UHM_OK) {
                uhm_slab_raw_unlock(slab);
                if (!uhm_slab_remote_raw_ready_note(heap, slab)) {
                    uhm_slab_remote_pending_note(heap, size_class);
                }
                uhm_lockless_remote_reader_release(reader);
                if (out_result) {
                    *out_result = account_result;
                }
                return 0;
            }
        }
        if (consumed_remote) {
            slab->remote_free_head = slot->next_free;
        } else {
            slab->free_head = slot->next_free;
        }
        slot->next_free = 0;
        uhm_tagged_freelist_next_store(&slot->remote_link, 0);
        uhm_slab_slot_state_store_locked(slot, 0u,
                                          (uint32_t)requested_size);
        --slab->free_slots;
        ++slab->live_slots;
        if (consumed_remote) {
            uhm_slab_summary_publish_counts_raw_locked(slab);
        } else {
            uhm_slab_summary_publish_raw_locked(slab);
        }
        requeue = slab->remote_free_head != 0 || slab->free_head != 0;
        uhm_slab_raw_unlock(slab);

        if (requeue && !uhm_slab_remote_raw_ready_note(heap, slab)) {
            uhm_slab_remote_pending_note(heap, size_class);
        }
        uhm_stats_malloc_live_add(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                                  requested_size);
        uhm_lockless_remote_reader_release(reader);
        return uhm_slab_slot_payload(slot);
    }

    uhm_lockless_remote_reader_release(reader);
    return 0;
}
#endif

static uhm_result uhm_slab_remote_free_slot_prepare_lockless(
    uhm_heap *heap,
    uhm_slab_slot *slot,
    uhm_slab **out_slab,
    int *out_needs_decommit_check) {
    uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
    size_t requested;
    uint32_t flags;
    uint32_t requested32;

    if (out_slab) {
        *out_slab = 0;
    }
    if (out_needs_decommit_check) {
        *out_needs_decommit_check = 0;
    }
    if (!heap || !slot || !slab || slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_tagged_freelist_is_supported() ||
        !uhm_tagged_freelist_ptr_packable(&slot->remote_link)) {
        return UHM_ERR_UNSUPPORTED;
    }
    uhm_slab_raw_lock(slab);
    if (!uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested32)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    requested = requested32;
    if (!uhm_slab_slot_state_cas(
            slot, flags, (uint32_t)requested,
            UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING,
            0u)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    uhm_stats_malloc_live_sub(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                              requested);
    slot->next_free = 0;
    uhm_tagged_freelist_next_store(&slot->remote_link, 0);
    if (slab->live_slots != 0) {
        --slab->live_slots;
    }
    ++slab->free_slots;
    uhm_slab_remote_pending_increment(slab);
    uhm_slab_remote_inflight_increment(slab);
    uhm_slab_refresh_state_raw_locked(heap, slab);
    if (out_slab) {
        *out_slab = slab;
    }
    if (out_needs_decommit_check &&
        slab->state == UHM_SLAB_STATE_EMPTY &&
        uhm_slab_empty_committed_cache_over_high_water(heap,
                                                       slab->size_class)) {
        *out_needs_decommit_check = 1;
    }
    uhm_slab_raw_unlock(slab);
    return UHM_OK;
}

static uhm_result
uhm_slab_remote_free_slot_prepare_partial_lockless(
    uhm_heap *heap,
    uhm_slab_slot *slot,
    uhm_slab **out_slab,
    int *out_needs_state_refresh,
    int *out_needs_decommit_check) {
    uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
    size_t requested;
    uint32_t flags;
    uint32_t requested32;
    int needs_state_refresh = 0;
    int needs_decommit_check = 0;

    if (out_slab) {
        *out_slab = 0;
    }
    if (out_needs_state_refresh) {
        *out_needs_state_refresh = 0;
    }
    if (out_needs_decommit_check) {
        *out_needs_decommit_check = 0;
    }
    if (!heap || !slot || !slab || slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_tagged_freelist_is_supported() ||
        !uhm_tagged_freelist_ptr_packable(&slot->remote_link)) {
        return UHM_ERR_UNSUPPORTED;
    }
    if (!uhm_slab_descriptor_live_for_lockless(heap, slab)) {
        return UHM_ERR_CORRUPTION;
    }

    uhm_slab_raw_lock(slab);
    if (!((slab->state == UHM_SLAB_STATE_PARTIAL &&
           slab->live_slots != 0 &&
           slab->free_slots != 0) ||
          (slab->state == UHM_SLAB_STATE_FULL &&
           slab->live_slots > 1u &&
           slab->free_slots == 0)) ||
        !uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested32)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_UNSUPPORTED;
    }
    needs_state_refresh =
        slab->state == UHM_SLAB_STATE_FULL ||
        (slab->state == UHM_SLAB_STATE_PARTIAL &&
         slab->live_slots == 1u);
    needs_decommit_check =
        slab->state == UHM_SLAB_STATE_PARTIAL &&
        slab->live_slots == 1u;
    if (needs_state_refresh) {
        uhm_slab_state_transition_update_begin(slab);
    }
    requested = requested32;
    if (!uhm_slab_slot_state_cas(
            slot, flags, (uint32_t)requested,
            UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING,
            0u)) {
        if (needs_state_refresh) {
            uhm_slab_state_transition_update_end(slab);
        }
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    uhm_stats_malloc_live_sub(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                              requested);
    slot->next_free = 0;
    uhm_tagged_freelist_next_store(&slot->remote_link, 0);
    --slab->live_slots;
    ++slab->free_slots;
    uhm_slab_remote_pending_increment(slab);
    uhm_slab_remote_inflight_increment(slab);
    if (needs_state_refresh) {
        uhm_slab_summary_publish_raw_locked(slab);
    } else {
        uhm_slab_summary_publish_counts_raw_locked(slab);
    }
    if (out_slab) {
        *out_slab = slab;
    }
    if (out_needs_state_refresh) {
        *out_needs_state_refresh = needs_state_refresh;
    }
    if (out_needs_decommit_check) {
        *out_needs_decommit_check = needs_decommit_check;
    }
    uhm_slab_raw_unlock(slab);
    return UHM_OK;
}

static uhm_result uhm_slab_remote_free_slot_prepare_unaccounted(
    uhm_heap *heap,
    uhm_slab_slot *slot,
    uhm_slab **out_slab,
    uint32_t *out_live_flags,
    uint32_t *out_live_requested) {
    uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
    size_t requested;
    uint32_t flags;
    uint32_t requested32;

    if (out_slab) {
        *out_slab = 0;
    }
    if (out_live_flags) {
        *out_live_flags = 0;
    }
    if (out_live_requested) {
        *out_live_requested = 0;
    }
    if (!heap || !slot || !slab || slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
        !uhm_tagged_freelist_is_supported() ||
        !uhm_tagged_freelist_ptr_packable(&slot->remote_link)) {
        return UHM_ERR_UNSUPPORTED;
    }
    if (!uhm_slab_descriptor_live_for_lockless(heap, slab)) {
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_slab_slot_live_state_atomic_load(slot, slab, &flags,
                                              &requested32)) {
        return UHM_ERR_CORRUPTION;
    }
    requested = requested32;

    uhm_slab_remote_pending_increment(slab);
    uhm_slab_remote_inflight_increment(slab);
    if (!uhm_slab_slot_state_cas_atomic_only(
            slot, flags, (uint32_t)requested,
            UHM_SLAB_SLOT_FLAG_FREE |
                UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED,
            0u)) {
        (void)uhm_slab_remote_inflight_decrement(slab);
        (void)uhm_slab_remote_pending_decrement(slab);
        return UHM_ERR_CORRUPTION;
    }
    uhm_stats_malloc_live_sub(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                              requested);
    uhm_tagged_freelist_next_store(&slot->remote_link, 0);
    if (out_slab) {
        *out_slab = slab;
    }
    if (out_live_flags) {
        *out_live_flags = flags;
    }
    if (out_live_requested) {
        *out_live_requested = (uint32_t)requested;
    }
    return UHM_OK;
}

static uhm_result uhm_slab_remote_free_slot_publish_unaccounted(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_slab_slot *slot) {
    int wrap_deferred = 0;

    if (!heap || !slab || !slot ||
        slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return UHM_ERR_CORRUPTION;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.fault_lockless_remote_publish_countdown != 0 &&
        --heap->core.fault_lockless_remote_publish_countdown == 0) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
#endif
    if (!uhm_heap_tagged_freelist_push_ex(
            heap, &heap->core.slab_remote_class_lf[slab->size_class].queue,
            &slot->remote_link, 1u, &wrap_deferred)) {
        if (wrap_deferred) {
            return UHM_ERR_BUSY;
        }
        return UHM_ERR_OUT_OF_MEMORY;
    }
    return UHM_OK;
}

static uhm_result uhm_slab_remote_free_slot_rollback_unaccounted(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_slab_slot *slot,
    uint32_t live_flags,
    uint32_t live_requested) {
    uint32_t flags;
    uint32_t requested;
    uhm_result result;

    if (!heap || !slab || !slot ||
        slab->magic != UHM_SLAB_MAGIC ||
        slot->magic != UHM_SLAB_SLOT_MAGIC ||
        !uhm_slab_slot_belongs_to_heap(heap, slot, slab, 0) ||
        slot->size_class != slab->size_class ||
        (live_flags != 0 &&
         live_flags != UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED) ||
        live_requested == 0 ||
        live_requested > slab->slot_size) {
        return UHM_ERR_CORRUPTION;
    }
    flags = uhm_slab_slot_flags_load(slot);
    requested = uhm_slab_slot_requested_load(slot);
    if ((flags & (UHM_SLAB_SLOT_FLAG_FREE |
                  UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                  UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                  UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) !=
            (UHM_SLAB_SLOT_FLAG_FREE |
             UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
             UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
             UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) ||
        (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                   UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                   UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                   UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) != 0 ||
        requested != 0) {
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_slab_slot_state_cas_atomic_only(
            slot, flags, 0u, live_flags, live_requested)) {
        return UHM_ERR_CORRUPTION;
    }
    uhm_tagged_freelist_next_store(&slot->remote_link, 0);
    uhm_stats_malloc_live_add(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                              live_requested);
    result = uhm_slab_remote_inflight_decrement(slab);
    if (result == UHM_OK) {
        result = uhm_slab_remote_pending_decrement(slab);
    }
    return result;
}

static void uhm_slab_remote_class_defer_node(
    uhm_tagged_freelist_node **deferred,
    uhm_tagged_freelist_node *node) {
    if (!deferred || !node) {
        return;
    }
    uhm_tagged_freelist_next_store(node, *deferred);
    *deferred = node;
}

static int uhm_slab_remote_class_restore_nodes(
    uhm_heap *heap,
    uhm_tagged_freelist *queue,
    size_t self_readers,
    uhm_tagged_freelist_node *deferred) {
    while (deferred) {
        uhm_tagged_freelist_node *next =
            uhm_tagged_freelist_next_load(deferred);
        uhm_tagged_freelist_next_store(deferred, 0);
        if (!uhm_heap_tagged_freelist_push(heap, queue, deferred,
                                           self_readers)) {
            return 0;
        }
        deferred = next;
    }
    return 1;
}

static uhm_result uhm_slab_remote_alloc_rollback_lockless(
    uhm_heap *heap,
    uhm_tagged_freelist *queue,
    uhm_tagged_freelist_node *link,
    uhm_tagged_freelist_node *deferred,
    uhm_slab_slot *slot,
    uint32_t requested_size) {
    const uint32_t remote_flags =
        UHM_SLAB_SLOT_FLAG_FREE |
        UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
        UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
        UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED;

    if (!queue || !link || !slot || requested_size == 0) {
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_slab_slot_state_cas_atomic_only(
            slot, UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED, requested_size,
            remote_flags, 0u)) {
        return UHM_ERR_CORRUPTION;
    }
    uhm_tagged_freelist_next_store(link, 0);
    if (!uhm_heap_tagged_freelist_push(heap, queue, link, 1u)) {
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_slab_remote_class_restore_nodes(heap, queue, 1u, deferred)) {
        return UHM_ERR_CORRUPTION;
    }
    return UHM_OK;
}

/*
 * MSVC Release can misdiagnose the scan loop after the C11 atomic hazard
 * reader gate as unreachable.
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
static void *uhm_slab_try_alloc_remote_lockless(
    uhm_heap *heap,
    uint16_t size_class,
    size_t requested_size,
    uhm_result *out_result) {
    uhm_tagged_freelist *queue;
    uhm_tagged_freelist_node *deferred = 0;
    uhm_metadata_reader_state *reader;
    uint32_t desired_requested;
    size_t scanned;

    if (out_result) {
        *out_result = UHM_OK;
    }
    if (!uhm_lockless_remote_free_enabled(heap) ||
        size_class >= UHM_SIZE_CLASS_COUNT ||
        requested_size == 0 || requested_size > UINT32_MAX) {
        return 0;
    }
    if (!uhm_tagged_freelist_is_supported()) {
        return 0;
    }
    queue = &heap->core.slab_remote_class_lf[size_class].queue;
    desired_requested = (uint32_t)requested_size;
    reader = uhm_lockless_remote_reader_acquire(heap);
    if (!reader) {
        return 0;
    }

#define UHM_REQUEUE_SCANNING_OR_FAIL(site_)                                  \
    do {                                                                     \
        if (!uhm_slab_remote_ready_requeue_scanning(heap, slab, 1u)) {        \
            uhm_debug_corruption_site(heap, (site_));                         \
            uhm_lockless_remote_reader_release(reader);                       \
            if (out_result) {                                                \
                *out_result = UHM_ERR_CORRUPTION;                             \
            }                                                                \
            return 0;                                                        \
        }                                                                    \
    } while (0)

#define UHM_CLEAR_SCANNING_OR_FAIL(site_)                                    \
    do {                                                                     \
        if (!uhm_slab_remote_ready_clear_scanning(heap, slab)) {              \
            uhm_debug_corruption_site(heap, (site_));                         \
            uhm_lockless_remote_reader_release(reader);                       \
            if (out_result) {                                                \
                *out_result = UHM_ERR_CORRUPTION;                             \
            }                                                                \
            return 0;                                                        \
        }                                                                    \
    } while (0)

#define UHM_FINISH_SCANNING_OR_FAIL(site_)                                   \
    do {                                                                     \
        if (!uhm_slab_remote_ready_finish_scanning(heap, slab, 1u)) {         \
            uhm_debug_corruption_site(heap, (site_));                         \
            uhm_lockless_remote_reader_release(reader);                       \
            if (out_result) {                                                \
                *out_result = UHM_ERR_CORRUPTION;                             \
            }                                                                \
            return 0;                                                        \
        }                                                                    \
    } while (0)

#define UHM_FINISH_SCANNING_AFTER_LIVE(site_)                                \
    do {                                                                     \
        if (!uhm_slab_remote_ready_finish_after_live(heap, slab, reader,      \
                                                     1u)) {                  \
            uhm_debug_corruption_site(heap, (site_));                         \
            uhm_set_error(heap, UHM_ERR_CORRUPTION);                          \
        }                                                                    \
    } while (0)

    for (scanned = 0; scanned < UHM_LOCKLESS_REMOTE_SCAN_LIMIT; ++scanned) {
        int wrap_deferred = 0;
        uhm_tagged_freelist_node *link =
            uhm_heap_tagged_freelist_pop(heap, queue, 1u, &wrap_deferred);
        uhm_slab_slot *slot;
        uhm_slab *slab;
        uint32_t flags;
        uint32_t requested;
        uhm_result result;
        int pending_decremented = 0;

        if (!link) {
            break;
        }
        slot = uhm_slab_slot_from_remote_link(link);
        if (slot &&
            !uhm_metadata_reader_hazard_set(reader, 0u, slot)) {
            (void)uhm_slab_remote_class_restore_nodes(heap, queue, 1u,
                                                      deferred);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        if (!slot || slot->magic != UHM_SLAB_SLOT_MAGIC ||
            slot->size_class != size_class) {
            (void)uhm_slab_remote_class_restore_nodes(heap, queue, 1u,
                                                      deferred);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
        if (slab &&
            !uhm_metadata_reader_hazard_set(reader, 1u, slab)) {
            (void)uhm_slab_remote_class_restore_nodes(heap, queue, 1u,
                                                      deferred);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        if (!slab || slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class ||
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
            (void)uhm_slab_remote_class_restore_nodes(heap, queue, 1u,
                                                      deferred);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        if (!uhm_slab_descriptor_live_for_lockless(heap, slab)) {
            uhm_tagged_freelist_next_store(link, deferred);
            (void)uhm_slab_remote_class_restore_nodes(heap, queue, 1u,
                                                      link);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        flags = uhm_slab_slot_flags_load(slot);
        requested = uhm_slab_slot_requested_load(slot);
        if ((flags & (UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                      UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) !=
                (UHM_SLAB_SLOT_FLAG_FREE |
                 UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                 UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                 UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) ||
            (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                       UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) != 0 ||
            requested != 0) {
            uhm_slab_remote_class_defer_node(&deferred, link);
            break;
        }
        if (!uhm_slab_slot_state_cas_atomic_only(
                slot, flags, 0u, UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED,
                desired_requested)) {
            uhm_slab_remote_class_defer_node(&deferred, link);
            break;
        }
#if UHMAS_ENABLE_TEST_HOOKS
        if (heap->core.fault_lockless_remote_alloc_countdown != 0 &&
            --heap->core.fault_lockless_remote_alloc_countdown == 0) {
            result = UHM_ERR_OUT_OF_MEMORY;
        } else {
            result = uhm_slab_remote_pending_decrement(slab);
        }
#else
        result = uhm_slab_remote_pending_decrement(slab);
#endif
        if (result == UHM_OK) {
            pending_decremented = 1;
            result = uhm_slab_remote_inflight_decrement(slab);
        }
        if (result != UHM_OK) {
            uhm_result rollback_result;
            if (pending_decremented) {
                uhm_slab_remote_pending_increment(slab);
            }
            rollback_result = uhm_slab_remote_alloc_rollback_lockless(
                heap, queue, link, deferred, slot, desired_requested);
            uhm_lockless_remote_reader_release(reader);
            if (rollback_result != UHM_OK) {
                if (out_result) {
                    *out_result = rollback_result;
                }
            } else if (result != UHM_ERR_OUT_OF_MEMORY) {
                if (out_result) {
                    *out_result = result;
                }
            }
            return 0;
        }
        if (!uhm_slab_remote_class_restore_nodes(heap, queue, 1u, deferred)) {
            uhm_set_error(heap, UHM_ERR_CORRUPTION);
        }
        uhm_stats_malloc_live_add(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                                  requested_size);
        uhm_lockless_remote_reader_release(reader);
        (void)uhm_metadata_epoch_advance_pinned_try(heap);
        return uhm_slab_slot_payload(slot);
    }

    if (!uhm_slab_remote_class_restore_nodes(heap, queue, 1u, deferred)) {
        if (out_result) {
            *out_result = UHM_ERR_CORRUPTION;
        }
    }
    uhm_lockless_remote_reader_release(reader);
    return 0;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static void *uhm_slab_try_alloc_accounted_remote_lockless(
    uhm_heap *heap,
    uint16_t size_class,
    size_t requested_size,
    uhm_result *out_result) {
    uhm_tagged_freelist *ready_queue;
    uhm_metadata_reader_state *reader;
    uint32_t desired_requested;
    size_t scanned;

    if (out_result) {
        *out_result = UHM_OK;
    }
    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT ||
        requested_size == 0 || requested_size > UINT32_MAX ||
        !uhm_tagged_freelist_is_supported()) {
        return 0;
    }
    ready_queue = &heap->core.slab_remote_ready_lf[size_class].queue;
    desired_requested = (uint32_t)requested_size;
    reader = uhm_lockless_remote_reader_acquire(heap);
    if (!reader) {
        return 0;
    }

    for (scanned = 0; scanned < UHM_LOCKLESS_REMOTE_SCAN_LIMIT; ++scanned) {
        int ready_wrap_deferred = 0;
        int remote_wrap_deferred = 0;
        uhm_tagged_freelist_node *ready_link;
        uhm_tagged_freelist_node *remote_link;
        uhm_slab *slab;
        uhm_slab_slot *slot;
        uhm_result result;
        uint32_t flags;
        int raw_published = 0;
        int needs_state_refresh = 0;
        int empty_candidate = 0;
        int state_transition_active = 0;

        uhm_slab_remote_ready_update_begin(heap, size_class);
        ready_link =
            uhm_heap_tagged_freelist_pop(heap, ready_queue, 1u,
                                         &ready_wrap_deferred);
        if (!ready_link) {
            (void)ready_wrap_deferred;
            uhm_slab_remote_ready_update_end(heap, size_class);
            break;
        }
        if (!uhm_metadata_reader_hazard_set(reader, 0u, ready_link)) {
            uhm_slab_remote_ready_update_end(heap, size_class);
            uhm_debug_corruption_site(heap, 5699u);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        slab = uhm_slab_from_remote_ready_link(heap, ready_link);
        uhm_tagged_freelist_next_store(ready_link, 0);
        if (!slab) {
            uhm_slab_remote_ready_update_end(heap, size_class);
            uhm_debug_corruption_site(heap, 5700u);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        if (!uhm_metadata_reader_hazard_set(reader, 1u, slab)) {
            uhm_slab_remote_ready_update_end(heap, size_class);
            uhm_debug_corruption_site(heap, 5701u);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        if (!uhm_slab_remote_ready_mark_scanning(slab)) {
            uhm_slab_remote_ready_update_end(heap, size_class);
            uhm_debug_corruption_site(heap, 5711u);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        uhm_slab_remote_ready_update_end(heap, size_class);
        if (!slab || slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class) {
            (void)uhm_slab_remote_ready_clear_scanning(heap, slab);
            uhm_debug_corruption_site(heap, 5702u);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        if ((slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
            !uhm_slab_descriptor_live_for_lockless(heap, slab)) {
            UHM_CLEAR_SCANNING_OR_FAIL(5713u);
            continue;
        }

        remote_link = uhm_heap_tagged_freelist_pop(
            heap, &slab->remote_free_lf, 1u, &remote_wrap_deferred);
        if (!remote_link) {
            if (remote_wrap_deferred) {
                UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
            } else {
                UHM_FINISH_SCANNING_OR_FAIL(5714u);
            }
            continue;
        }
        if (!uhm_metadata_reader_hazard_set(reader, 0u, remote_link)) {
            (void)uhm_slab_remote_restore_tagged_batch(
                heap, &slab->remote_free_lf, 1u, remote_link);
            UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
            uhm_debug_corruption_site(heap, 5703u);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }
        slot = uhm_slab_slot_from_remote_link(remote_link);
        if (!slot || slot->magic != UHM_SLAB_SLOT_MAGIC ||
            !uhm_slab_slot_belongs_to_heap(heap, slot, slab, 1) ||
            slot->size_class != size_class) {
            (void)uhm_slab_remote_restore_tagged_batch(
                heap, &slab->remote_free_lf, 1u, remote_link);
            UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
            uhm_debug_corruption_site(heap, 5704u);
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = UHM_ERR_CORRUPTION;
            }
            return 0;
        }

        result = uhm_slab_remote_mark_reserved_batch_lockless(slab,
                                                              remote_link);
        if (result != UHM_OK) {
            if (!uhm_slab_remote_restore_reserved_batch(
                    heap, &slab->remote_free_lf, slab, 1u, remote_link)) {
                result = UHM_ERR_CORRUPTION;
            }
            UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
            if (result == UHM_ERR_CORRUPTION) {
                uhm_debug_corruption_site(heap, 5705u);
            }
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = result;
            }
            return 0;
        }

        if (!uhm_slab_accounted_remote_alloc_candidate_hint(
                slab, &empty_candidate)) {
            uhm_slab_raw_lock(slab);
            if (slab->state == UHM_SLAB_STATE_EMPTY &&
                slab->live_slots == 0 &&
                slab->free_slots != 0 &&
                slab->ctx_cached_slots == 0) {
                empty_candidate = 1;
            } else if (!(slab->state == UHM_SLAB_STATE_PARTIAL &&
                         slab->free_slots != 0)) {
                uhm_slab_raw_unlock(slab);
                if (!uhm_slab_remote_restore_reserved_batch(
                        heap, &slab->remote_free_lf, slab, 1u, remote_link)) {
                    (void)uhm_slab_remote_ready_clear_scanning(heap, slab);
                    uhm_debug_corruption_site(heap, 5706u);
                    uhm_lockless_remote_reader_release(reader);
                    if (out_result) {
                        *out_result = UHM_ERR_CORRUPTION;
                    }
                    return 0;
                }
                UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
                continue;
            }
            uhm_slab_raw_unlock(slab);
        }

        if (empty_candidate &&
            (uhm_slab_remote_pending_load(slab) != 1u ||
             uhm_slab_remote_inflight_load(slab) != 1u)) {
            if (!uhm_slab_remote_restore_reserved_batch(
                    heap, &slab->remote_free_lf, slab, 1u, remote_link)) {
                (void)uhm_slab_remote_ready_clear_scanning(heap, slab);
                uhm_debug_corruption_site(heap, 5710u);
                uhm_lockless_remote_reader_release(reader);
                if (out_result) {
                    *out_result = UHM_ERR_CORRUPTION;
                }
                return 0;
            }
            UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
            continue;
        }

        result = uhm_slab_remote_account_reserved_batch_lockless(
            slab, remote_link);
        if (result != UHM_OK) {
            if (!uhm_slab_remote_restore_reserved_batch(
                    heap, &slab->remote_free_lf, slab, 1u, remote_link)) {
                result = UHM_ERR_CORRUPTION;
            }
            UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
            if (result == UHM_ERR_CORRUPTION) {
                uhm_debug_corruption_site(heap, 5707u);
            }
            uhm_lockless_remote_reader_release(reader);
            if (out_result) {
                *out_result = result;
            }
            return 0;
        }

        if (empty_candidate) {
            int empty_transition_active = 0;
            uhm_lock(&heap->lock);
            if (uhm_heap_is_accessible_pinned(heap) &&
                slab->magic == UHM_SLAB_MAGIC &&
                (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
                heap->core.slab_empty_heads[size_class] == slab &&
                uhm_slab_remote_pending_load(slab) == 0 &&
                uhm_slab_remote_inflight_load(slab) == 0 &&
                uhm_slab_empty_committed_cache_within_high_water(
                    heap, size_class) &&
                uhm_slab_descriptor_live_for_lockless(heap, slab)) {
                uhm_slab_raw_lock(slab);
                flags = uhm_slab_slot_flags_load(slot);
                if (slab->state == UHM_SLAB_STATE_EMPTY &&
                    slab->live_slots == 0 &&
                    slab->free_slots != 0 &&
                    slab->ctx_cached_slots == 0 &&
                    (flags & (UHM_SLAB_SLOT_FLAG_FREE |
                              UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                              UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                              UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) ==
                        (UHM_SLAB_SLOT_FLAG_FREE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                         UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                         UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) &&
                    (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                               UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                               UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                               UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                               UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) == 0 &&
                    uhm_slab_slot_requested_load(slot) == 0) {
                    uhm_slab_state_transition_update_begin(slab);
                    empty_transition_active = 1;
                    if (uhm_slab_slot_state_cas(
                            slot, flags, 0u, 0u, desired_requested)) {
                        uhm_tagged_freelist_next_store(remote_link, 0);
                        slot->next_free = 0;
                        --slab->free_slots;
                        ++slab->live_slots;
                        uhm_slab_refresh_state_raw_locked(heap, slab);
                        uhm_slab_state_transition_update_end(slab);
                        empty_transition_active = 0;
                        raw_published = 1;
                        uhm_metadata_epoch_advance(heap);
                    }
                }
                if (empty_transition_active) {
                    uhm_slab_state_transition_update_end(slab);
                }
                uhm_slab_raw_unlock(slab);
            }
            uhm_unlock(&heap->lock);

            if (!raw_published) {
                if (!uhm_slab_remote_restore_reserved_batch(
                        heap, &slab->remote_free_lf, slab, 1u,
                        remote_link)) {
                    (void)uhm_slab_remote_ready_clear_scanning(heap, slab);
                    uhm_debug_corruption_site(heap, 5709u);
                    uhm_lockless_remote_reader_release(reader);
                    if (out_result) {
                        *out_result = UHM_ERR_CORRUPTION;
                    }
                    return 0;
                }
                UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
                continue;
            }
            UHM_FINISH_SCANNING_AFTER_LIVE(5714u);
            uhm_stats_malloc_live_add(heap,
                                      (uintptr_t)uhm_slab_slot_payload(slot),
                                      requested_size);
            uhm_lockless_remote_reader_release(reader);
            return uhm_slab_slot_payload(slot);
        }

        uhm_slab_raw_lock(slab);
        flags = uhm_slab_slot_flags_load(slot);
        if (slab->state == UHM_SLAB_STATE_PARTIAL &&
            slab->free_slots != 0 &&
            (flags & (UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                      UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) ==
                (UHM_SLAB_SLOT_FLAG_FREE |
                 UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                 UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                 UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) &&
            (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                       UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                       UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) == 0 &&
            uhm_slab_slot_requested_load(slot) == 0) {
            int will_need_state_refresh =
                slab->state == UHM_SLAB_STATE_PARTIAL &&
                slab->free_slots == 1u;
            if (will_need_state_refresh) {
                uhm_slab_state_transition_update_begin(slab);
            }
            if (uhm_slab_slot_state_cas(
                    slot, flags, 0u, 0u, desired_requested)) {
                uhm_tagged_freelist_next_store(remote_link, 0);
                slot->next_free = 0;
                --slab->free_slots;
                ++slab->live_slots;
                needs_state_refresh = slab->free_slots == 0;
                if (needs_state_refresh) {
                    uhm_slab_summary_publish_raw_locked(slab);
                } else {
                    uhm_slab_summary_publish_counts_raw_locked(slab);
                }
                raw_published = 1;
                state_transition_active = will_need_state_refresh;
            } else if (will_need_state_refresh) {
                uhm_slab_state_transition_update_end(slab);
            }
        }
        uhm_slab_raw_unlock(slab);

        if (!raw_published) {
            if (!uhm_slab_remote_restore_reserved_batch(
                    heap, &slab->remote_free_lf, slab, 1u, remote_link)) {
                (void)uhm_slab_remote_ready_clear_scanning(heap, slab);
                uhm_debug_corruption_site(heap, 5708u);
                uhm_lockless_remote_reader_release(reader);
                if (out_result) {
                    *out_result = UHM_ERR_CORRUPTION;
                }
                return 0;
            }
            UHM_REQUEUE_SCANNING_OR_FAIL(5712u);
            continue;
        }

        if (!needs_state_refresh) {
            (void)uhm_metadata_epoch_advance_pinned_try(heap);
        } else {
            uhm_lock(&heap->lock);
            if (uhm_heap_is_accessible_pinned(heap)) {
                if (slab->magic == UHM_SLAB_MAGIC &&
                    (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0) {
                    uhm_slab_raw_lock(slab);
                    uhm_slab_refresh_state_raw_locked(heap, slab);
                    uhm_slab_raw_unlock(slab);
                }
                uhm_metadata_epoch_advance(heap);
            }
            if (state_transition_active) {
                uhm_slab_state_transition_update_end(slab);
                state_transition_active = 0;
            }
            uhm_unlock(&heap->lock);
        }
        UHM_FINISH_SCANNING_AFTER_LIVE(5714u);
        uhm_stats_malloc_live_add(heap, (uintptr_t)uhm_slab_slot_payload(slot),
                                  requested_size);
        uhm_lockless_remote_reader_release(reader);
        return uhm_slab_slot_payload(slot);
    }

    uhm_lockless_remote_reader_release(reader);
#undef UHM_CLEAR_SCANNING_OR_FAIL
#undef UHM_FINISH_SCANNING_OR_FAIL
#undef UHM_FINISH_SCANNING_AFTER_LIVE
#undef UHM_REQUEUE_SCANNING_OR_FAIL
    return 0;
}

static uhm_result uhm_slab_remote_free_slot_publish_lockless(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_slab_slot *slot,
    int needs_decommit_check) {
    uhm_result result = UHM_OK;
    uint32_t flags;
    int pushed;
    int cleared_publishing = 0;
    int raw_locked = 0;
    int raw_fallback_published = 0;

    if (!heap || !slab || !slot) {
        return UHM_ERR_CORRUPTION;
    }
    pushed = uhm_heap_tagged_freelist_push(
        heap, &slab->remote_free_lf, &slot->remote_link, 0u);
    if (pushed) {
        uhm_slab_remote_ready_note(heap, slab, 0u);
        /*
         * The common tagged path is fully published by the lock-free push.
         * Keep REMOTE_PUBLISHING/inflight set until drain consumes the slot;
         * that lets check/drain distinguish this valid post-push window from a
         * lost remote-free node without taking the heap lock again here.
         */
        if (!needs_decommit_check) {
            return UHM_OK;
        }
        return uhm_slab_drain_remote_reserved_lockless(
            heap, slab, needs_decommit_check);
    }

    if (!uhm_heap_is_accessible_pinned(heap) ||
        !uhm_slab_descriptor_live_for_lockless(heap, slab)) {
        result = UHM_ERR_CORRUPTION;
    } else {
        uhm_slab_raw_lock(slab);
        raw_locked = 1;
        if (slab->magic != UHM_SLAB_MAGIC ||
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
            !uhm_slab_slot_belongs_to_heap(heap, slot, slab, 1) ||
            !uhm_slab_slot_state_mirror_matches(slot)) {
            result = UHM_ERR_CORRUPTION;
        }
    }
    if (result == UHM_OK) {
        flags = uhm_slab_slot_flags_load(slot);
        if ((flags & (UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING)) !=
                (UHM_SLAB_SLOT_FLAG_FREE |
                 UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                 UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) ||
            (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                       UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) != 0 ||
            uhm_slab_slot_requested_load(slot) != 0) {
            result = UHM_ERR_CORRUPTION;
        } else {
            slot->next_free = slab->remote_free_head;
            slab->remote_free_head = slot;
            raw_fallback_published = 1;
        }
    }

    if (result == UHM_OK &&
        uhm_slab_slot_belongs_to_heap(heap, slot, slab, 1) &&
        uhm_slab_slot_state_mirror_matches(slot)) {
        flags = uhm_slab_slot_flags_load(slot);
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) != 0) {
            if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0) {
                cleared_publishing = 0;
            } else if ((flags & (UHM_SLAB_SLOT_FLAG_FREE |
                          UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
                    (UHM_SLAB_SLOT_FLAG_FREE |
                     UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) ||
                (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                           UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                           UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED)) != 0 ||
                uhm_slab_slot_requested_load(slot) != 0 ||
                !uhm_slab_slot_state_cas(
                    slot, flags, 0u,
                    flags & ~UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING, 0u)) {
                result = UHM_ERR_CORRUPTION;
            } else {
                cleared_publishing = 1;
            }
        } else if (!pushed ||
                   flags != UHM_SLAB_SLOT_FLAG_FREE ||
                   uhm_slab_slot_requested_load(slot) != 0) {
            result = UHM_ERR_CORRUPTION;
        }
    }
    if (raw_locked) {
        uhm_slab_raw_unlock(slab);
    }
    if (result == UHM_OK && raw_fallback_published) {
        uhm_slab_remote_pending_note(heap, slab->size_class);
    }

    if (result == UHM_OK && cleared_publishing) {
        result = uhm_slab_remote_inflight_decrement(slab);
    }
    if (result == UHM_OK && needs_decommit_check) {
        result = uhm_slab_drain_remote_reserved_lockless(heap, slab, 1);
    }
    if (result == UHM_ERR_UNSUPPORTED || result == UHM_ERR_PLATFORM) {
        return UHM_OK;
    }
    return result;
}

