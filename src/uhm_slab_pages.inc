static uhm_slab *uhm_slab_select_empty(uhm_heap *heap, uint16_t size_class) {
    uhm_slab *slab;
    uhm_slab *fallback = heap->core.slab_empty_heads[size_class];

    for (slab = fallback; slab; slab = slab->next_class) {
        if ((slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0 &&
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
            uhm_slab_free_head_available_hint(slab)) {
            return slab;
        }
    }
    for (slab = fallback; slab; slab = slab->next_class) {
        if ((slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0 &&
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 &&
            slab->ctx_cached_slots == 0) {
            return slab;
        }
    }
    return 0;
}

static uhm_slab *uhm_slab_select_partial(uhm_heap *heap,
                                         uint16_t size_class) {
    uhm_slab *slab;

    for (slab = heap->core.slab_class_heads[size_class]; slab;
         slab = slab->next_class) {
        if ((slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0 &&
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
            uhm_slab_free_head_available_hint(slab)) {
            return slab;
        }
    }
    return 0;
}

typedef struct uhm_slab_decommit_claim {
    uhm_slab *slab;
    uhm_slab_slot *old_free_head;
    size_t first_page;
    size_t page_count;
    uint64_t generation;
} uhm_slab_decommit_claim;

static int uhm_slab_empty_decommit_ready(const uhm_slab *slab) {
    return slab && slab->magic == UHM_SLAB_MAGIC &&
           slab->state == UHM_SLAB_STATE_EMPTY &&
           (slab->flags &
            (UHM_SLAB_FLAG_DECOMMITTED | UHM_SLAB_FLAG_VM_BUSY)) == 0u &&
           slab->live_slots == 0u &&
           slab->free_slots == slab->total_slots &&
           slab->ctx_cached_slots == 0u &&
           uhm_slab_remote_inflight_load(slab) == 0u &&
           uhm_slab_remote_pending_load(slab) == 0u &&
           uhm_slab_remote_head_empty_snapshot(slab) &&
           uhm_slab_remote_lf_empty(slab);
}

static int uhm_slab_decommit_restore_claims_locked(
    uhm_heap *heap,
    uhm_slab_decommit_claim *claims,
    size_t claim_count,
    size_t first_page,
    size_t page_count,
    int rebuild_free_slots) {
    uhm_lockset rollback_lockset;
    size_t i;
    int restored = 1;

    uhm_page_lock_shard_range(heap, first_page, page_count,
                              &rollback_lockset);
    uhm_slab_lock_all_shards(heap, &rollback_lockset);
    for (i = 0u; i < claim_count; ++i) {
        uhm_slab *slab = claims[i].slab;
        int was_committed_empty;

        if (!slab || slab->magic != UHM_SLAB_MAGIC ||
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0u ||
            (slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0u ||
            slab->generation != claims[i].generation) {
            restored = 0;
            continue;
        }
        was_committed_empty = uhm_slab_committed_empty_counted(slab);
        slab->flags &= ~(UHM_SLAB_FLAG_DECOMMITTED |
                         UHM_SLAB_FLAG_VM_BUSY);
        uhm_slab_committed_empty_apply_transition(heap, slab,
                                                  was_committed_empty);
        if (rebuild_free_slots) {
            /*
             * A successful or partially effective decommit may have erased
             * the in-page next_free links.  A same-address recommit restores
             * accessibility, not those links, so rebuild every empty slot.
             */
            uhm_slab_rebuild_free_slots(slab);
        } else {
            uhm_slab_raw_lock(slab);
            slab->free_head = claims[i].old_free_head;
            uhm_slab_summary_publish_raw_locked(slab);
            uhm_slab_raw_unlock(slab);
        }
        slab->generation = uhm_heap_generation_next(heap);
        uhm_slab_descriptor_set(slab);
        if (uhm_page_span_mark_range_locked_prepared(
                heap, claims[i].first_page, claims[i].page_count,
                UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_MALLOC_LIVE,
                UHM_PAGE_COMMIT_LIVE, UHM_PROT_READ | UHM_PROT_WRITE,
                slab->desc.id, slab->generation, &rollback_lockset,
                0) != UHM_OK) {
            restored = 0;
        }
    }
    uhm_metadata_unlock_shards(heap, &rollback_lockset);
    uhm_page_unlock_shards(heap, &rollback_lockset);
    return restored;
}

static uhm_result uhm_slab_decommit_empty_run(
    uhm_heap *heap,
    uhm_slab *const *slabs,
    size_t slab_count) {
    uhm_slab_decommit_claim claims[UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX];
    uintptr_t run_base;
    uintptr_t run_end;
    size_t run_size = 0u;
    size_t first_page = 0u;
    size_t page_count = 0u;
    size_t claimed_count = 0u;
    size_t i;
    uhm_result result = UHM_OK;
    int decommit_may_need_recommit = 0;

    if (!heap || !slabs || slab_count == 0u ||
        slab_count > UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_metadata_registered_reader_active_locked(heap)) {
        return UHM_OK;
    }
    memset(claims, 0, sizeof(claims));
    run_base = slabs[0] ? slabs[0]->base : 0u;
    run_end = run_base;
    for (i = 0u; i < slab_count; ++i) {
        uhm_slab *slab = slabs[i];
        size_t next_page_count;
        uintptr_t slab_end;

        if (!uhm_slab_empty_decommit_ready(slab) ||
            slab->size_class != slabs[0]->size_class ||
            slab->base != run_end ||
            uhm_range_overflows(slab->base, slab->size, &slab_end) ||
            run_size > SIZE_MAX - slab->size ||
            !uhm_page_span(heap, slab->base, slab->size,
                           &claims[i].first_page,
                           &claims[i].page_count)) {
            return UHM_ERR_CORRUPTION;
        }
        if (i == 0u) {
            first_page = claims[i].first_page;
        } else if (claims[i].first_page !=
                   claims[i - 1u].first_page +
                       claims[i - 1u].page_count) {
            return UHM_ERR_CORRUPTION;
        }
        next_page_count = page_count + claims[i].page_count;
        if (next_page_count < page_count) {
            return UHM_ERR_CORRUPTION;
        }
        claims[i].slab = slab;
        page_count = next_page_count;
        run_size += slab->size;
        run_end = slab_end;
    }

    {
        uhm_lockset publish_lockset;

        uhm_page_lock_shard_range(heap, first_page, page_count,
                                  &publish_lockset);
        uhm_slab_lock_all_shards(heap, &publish_lockset);
        for (i = 0u; i < slab_count; ++i) {
            uhm_slab *slab = claims[i].slab;
            int was_committed_empty;

            if (!uhm_slab_empty_decommit_ready(slab)) {
                result = UHM_ERR_CORRUPTION;
                break;
            }
            was_committed_empty = uhm_slab_committed_empty_counted(slab);
            slab->flags |= UHM_SLAB_FLAG_DECOMMITTED |
                           UHM_SLAB_FLAG_VM_BUSY;
            uhm_slab_committed_empty_apply_transition(
                heap, slab, was_committed_empty);
            claims[i].generation = uhm_heap_generation_next(heap);
            slab->generation = claims[i].generation;
            uhm_slab_descriptor_set(slab);
            uhm_slab_raw_lock(slab);
            claims[i].old_free_head = slab->free_head;
            slab->free_head = 0;
            uhm_slab_summary_publish_raw_locked(slab);
            uhm_slab_raw_unlock(slab);
            ++claimed_count;
#if UHMAS_ENABLE_TEST_HOOKS
            if (heap->core.fault_slab_decommit_page_publish_countdown != 0u &&
                --heap->core.fault_slab_decommit_page_publish_countdown ==
                    0u) {
                result = UHM_ERR_OUT_OF_MEMORY;
                break;
            }
#endif
            result = uhm_page_span_mark_range_locked_prepared(
                heap, claims[i].first_page, claims[i].page_count,
                UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_MALLOC_LIVE,
                UHM_PAGE_COMMIT_DECOMMITTED, UHM_PROT_NONE,
                slab->desc.id, slab->generation, &publish_lockset, 0);
            if (result != UHM_OK) {
                break;
            }
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result != UHM_OK) {
        if (!uhm_slab_decommit_restore_claims_locked(
                heap, claims, claimed_count, first_page, page_count, 0)) {
            return UHM_ERR_CORRUPTION;
        }
        return result;
    }

    {
        uhm_vm_call_record decommit_record;

        memset(&decommit_record, 0, sizeof(decommit_record));
        uhm_unlock(&heap->lock);
        decommit_record = uhm_vm_decommit_unlocked_call(
            heap, (void *)run_base, run_size);
        uhm_lock(&heap->lock);
        result = uhm_vm_decommit_publish_record_locked(heap,
                                                       &decommit_record);
        decommit_may_need_recommit =
            decommit_record.result == UHM_OK || decommit_record.did_call;
    }
    if (result == UHM_OK && !uhm_heap_is_accessible_pinned(heap)) {
        result = UHM_ERR_CORRUPTION;
    }
    if (result == UHM_OK) {
        for (i = 0u; i < slab_count; ++i) {
            uhm_slab *slab = claims[i].slab;

            if (!slab || slab->magic != UHM_SLAB_MAGIC ||
                (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0u ||
                (slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0u ||
                slab->generation != claims[i].generation ||
                slab->state != UHM_SLAB_STATE_EMPTY ||
                slab->live_slots != 0u ||
                slab->free_slots != slab->total_slots ||
                uhm_slab_free_head_snapshot(slab) != 0 ||
                slab->ctx_cached_slots != 0u ||
                uhm_slab_remote_inflight_load(slab) != 0u ||
                uhm_slab_remote_pending_load(slab) != 0u ||
                !uhm_slab_remote_head_empty_snapshot(slab) ||
                !uhm_slab_remote_lf_empty(slab)) {
                result = UHM_ERR_CORRUPTION;
                break;
            }
        }
    }
    if (result != UHM_OK) {
        uint32_t saved_platform_error = atomic_load_explicit(
            &heap->platform_error, UHM_ATOMIC_ACQUIRE);

        if (decommit_may_need_recommit) {
            uhm_vm_call_record rollback_record;

            uhm_unlock(&heap->lock);
            rollback_record = uhm_vm_commit_unlocked_call(
                heap, (void *)run_base, run_size,
                UHM_PROT_READ | UHM_PROT_WRITE);
            uhm_lock(&heap->lock);
            if (uhm_vm_commit_publish_record_locked(
                    heap, &rollback_record) != UHM_OK) {
                atomic_store_explicit(&heap->platform_error,
                                      saved_platform_error,
                                      UHM_ATOMIC_RELEASE);
                for (i = 0u; i < slab_count; ++i) {
                    uhm_slab *slab = claims[i].slab;
                    if (slab && slab->magic == UHM_SLAB_MAGIC &&
                        (slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0u) {
                        slab->flags &= ~UHM_SLAB_FLAG_VM_BUSY;
                    }
                }
                return result;
            }
        }
        atomic_store_explicit(&heap->platform_error, saved_platform_error,
                              UHM_ATOMIC_RELEASE);
        if (!uhm_slab_decommit_restore_claims_locked(
                heap, claims, slab_count, first_page, page_count,
                decommit_may_need_recommit)) {
            return UHM_ERR_CORRUPTION;
        }
        return result;
    }

    for (i = 0u; i < slab_count; ++i) {
        uhm_slab *slab = claims[i].slab;
        slab->flags &= ~UHM_SLAB_FLAG_VM_BUSY;
        heap->stats.decommitted_bytes += slab->size;
        uhm_vm_account_decommit(heap, slab->base, slab->size);
    }
    return UHM_OK;
}

static uhm_result uhm_slab_decommit_empty(uhm_heap *heap, uhm_slab *slab) {
    uhm_slab_slot *old_free_head;
    size_t first;
    size_t count;
    uhm_result result;
    uint64_t decommit_generation;
    int decommit_may_need_recommit = 0;

    if (!heap || !slab) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
        return UHM_OK;
    }
    if (uhm_metadata_registered_reader_active_locked(heap)) {
        return UHM_OK;
    }
    if (slab->state != UHM_SLAB_STATE_EMPTY ||
        slab->live_slots != 0 ||
        slab->free_slots != slab->total_slots ||
        slab->ctx_cached_slots != 0 ||
        uhm_slab_remote_inflight_load(slab) != 0 ||
        uhm_slab_remote_pending_load(slab) != 0 ||
        !uhm_slab_remote_head_empty_snapshot(slab) ||
        !uhm_slab_remote_lf_empty(slab)) {
        return UHM_ERR_CORRUPTION;
    }

    if (!uhm_page_span(heap, slab->base, slab->size, &first, &count)) {
        return UHM_ERR_CORRUPTION;
    }

    old_free_head = 0;
    {
        uhm_lockset publish_lockset;
        uhm_page_lock_shard_range(heap, first, count, &publish_lockset);
        uhm_slab_lock_all_shards(heap, &publish_lockset);
        {
            int was_committed_empty =
                uhm_slab_committed_empty_counted(slab);
            slab->flags |= UHM_SLAB_FLAG_DECOMMITTED |
                           UHM_SLAB_FLAG_VM_BUSY;
            uhm_slab_committed_empty_apply_transition(
                heap, slab, was_committed_empty);
        }
        decommit_generation = uhm_heap_generation_next(heap);
        slab->generation = decommit_generation;
        uhm_slab_descriptor_set(slab);
        uhm_slab_raw_lock(slab);
        old_free_head = slab->free_head;
        slab->free_head = 0;
        uhm_slab_summary_publish_raw_locked(slab);
        uhm_slab_raw_unlock(slab);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, first, count, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_DECOMMITTED,
            UHM_PROT_NONE, slab->desc.id, slab->generation, &publish_lockset,
            0);
        if (result != UHM_OK) {
            int was_committed_empty =
                uhm_slab_committed_empty_counted(slab);
            slab->flags &= ~(UHM_SLAB_FLAG_DECOMMITTED |
                             UHM_SLAB_FLAG_VM_BUSY);
            uhm_slab_committed_empty_apply_transition(
                heap, slab, was_committed_empty);
            uhm_slab_raw_lock(slab);
            slab->free_head = old_free_head;
            uhm_slab_summary_publish_raw_locked(slab);
            uhm_slab_raw_unlock(slab);
            slab->generation = uhm_heap_generation_next(heap);
            uhm_slab_descriptor_set(slab);
            (void)uhm_page_span_mark_range_locked_prepared(
                heap, first, count, UHM_PAGE_OWNER_MALLOC,
                UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
                UHM_PROT_READ | UHM_PROT_WRITE, slab->desc.id,
                slab->generation, &publish_lockset, 0);
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result != UHM_OK) {
        return result;
    }

    {
        uhm_vm_call_record decommit_record;

        memset(&decommit_record, 0, sizeof(decommit_record));
        uhm_unlock(&heap->lock);
        decommit_record =
            uhm_vm_decommit_unlocked_call(heap, (void *)slab->base,
                                          slab->size);
        uhm_lock(&heap->lock);
        result = uhm_vm_decommit_publish_record_locked(heap,
                                                       &decommit_record);
        decommit_may_need_recommit =
            decommit_record.result == UHM_OK || decommit_record.did_call;
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             slab->magic != UHM_SLAB_MAGIC ||
             (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 ||
             (slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0 ||
             slab->generation != decommit_generation ||
             slab->state != UHM_SLAB_STATE_EMPTY ||
             slab->live_slots != 0 ||
             slab->free_slots != slab->total_slots ||
             uhm_slab_free_head_snapshot(slab) != 0 ||
             slab->ctx_cached_slots != 0 ||
             uhm_slab_remote_inflight_load(slab) != 0 ||
             uhm_slab_remote_pending_load(slab) != 0 ||
             !uhm_slab_remote_head_empty_snapshot(slab) ||
             !uhm_slab_remote_lf_empty(slab))) {
            result = UHM_ERR_CORRUPTION;
        }
    }
    if (result != UHM_OK) {
        if (slab->magic == UHM_SLAB_MAGIC &&
            (slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0) {
            if (decommit_may_need_recommit) {
                uhm_vm_call_record rollback_record;
                uint32_t saved_platform_error;

                saved_platform_error = atomic_load_explicit(
                    &heap->platform_error, UHM_ATOMIC_ACQUIRE);
                uhm_unlock(&heap->lock);
                rollback_record = uhm_vm_commit_unlocked_call(
                    heap, (void *)slab->base, slab->size,
                    UHM_PROT_READ | UHM_PROT_WRITE);
                uhm_lock(&heap->lock);
                if (uhm_vm_commit_publish_record_locked(
                        heap, &rollback_record) != UHM_OK) {
                    atomic_store_explicit(&heap->platform_error,
                                          saved_platform_error,
                                          UHM_ATOMIC_RELEASE);
                    if (slab->magic == UHM_SLAB_MAGIC &&
                        (slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0) {
                        slab->flags &= ~UHM_SLAB_FLAG_VM_BUSY;
                    }
                    return result;
                }
                atomic_store_explicit(&heap->platform_error,
                                      saved_platform_error,
                                      UHM_ATOMIC_RELEASE);
            }
            uhm_lockset rollback_lockset;
            uhm_page_lock_shard_range(heap, first, count, &rollback_lockset);
            uhm_slab_lock_all_shards(heap, &rollback_lockset);
            {
                int was_committed_empty =
                    uhm_slab_committed_empty_counted(slab);
                slab->flags &= ~(UHM_SLAB_FLAG_DECOMMITTED |
                                 UHM_SLAB_FLAG_VM_BUSY);
                uhm_slab_committed_empty_apply_transition(
                    heap, slab, was_committed_empty);
            }
            uhm_slab_raw_lock(slab);
            slab->free_head = old_free_head;
            uhm_slab_summary_publish_raw_locked(slab);
            uhm_slab_raw_unlock(slab);
            slab->generation = uhm_heap_generation_next(heap);
            uhm_slab_descriptor_set(slab);
            (void)uhm_page_span_mark_range_locked_prepared(
                heap, first, count, UHM_PAGE_OWNER_MALLOC,
                UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
                UHM_PROT_READ | UHM_PROT_WRITE, slab->desc.id,
                slab->generation, &rollback_lockset, 0);
            uhm_metadata_unlock_shards(heap, &rollback_lockset);
            uhm_page_unlock_shards(heap, &rollback_lockset);
        }
        return result;
    }
    slab->flags &= ~UHM_SLAB_FLAG_VM_BUSY;
    heap->stats.decommitted_bytes += slab->size;
    uhm_vm_account_decommit(heap, slab->base, slab->size);
    return UHM_OK;
}

static uhm_result uhm_slab_maybe_decommit_empty(uhm_heap *heap,
                                                uhm_slab *slab) {
    uhm_result result;

    if (!heap || !slab) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (slab->state != UHM_SLAB_STATE_EMPTY ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
        (slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0 ||
        slab->ctx_cached_slots != 0 ||
        !uhm_slab_empty_committed_cache_over_high_water(heap,
                                                        slab->size_class)) {
        return UHM_OK;
    }
    if (uhm_metadata_registered_reader_active_locked(heap)) {
        return UHM_OK;
    }
    if (uhm_slab_remote_pending_load(slab) != 0 ||
        uhm_slab_remote_inflight_load(slab) != 0 ||
        !uhm_slab_remote_head_empty_snapshot(slab) ||
        !uhm_slab_remote_lf_empty(slab)) {
        if (uhm_slab_remote_inflight_load(slab) != 0) {
            return UHM_OK;
        }
        result = uhm_slab_drain_remote(heap, slab);
        if (result != UHM_OK) {
            return result;
        }
    }
    uhm_slab_note_decommit_pressure(heap, slab);
    result = uhm_slab_decommit_empty(heap, slab);
    if (result == UHM_ERR_UNSUPPORTED || result == UHM_ERR_PLATFORM) {
        return UHM_OK;
    }
    return result;
}

static void uhm_slab_decommit_candidate_sort(uhm_slab **slabs,
                                             size_t slab_count) {
    size_t i;

    for (i = 1u; i < slab_count; ++i) {
        uhm_slab *slab = slabs[i];
        size_t j = i;

        while (j != 0u && slabs[j - 1u]->base > slab->base) {
            slabs[j] = slabs[j - 1u];
            --j;
        }
        slabs[j] = slab;
    }
}

static uhm_result uhm_slab_decommit_candidate_batch(
    uhm_heap *heap,
    uhm_slab **slabs,
    size_t slab_count) {
    unsigned char processed[UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX] = {0};
    uhm_result first_error = UHM_OK;
    size_t processed_count = 0u;

    uhm_slab_decommit_candidate_sort(slabs, slab_count);
    while (processed_count < slab_count) {
        size_t best_start = SIZE_MAX;
        size_t best_count = 0u;
        size_t fallback_start = SIZE_MAX;
        size_t scan = 0u;
        size_t i;
        uhm_slab *slab;
        size_t contiguous_count;
        size_t batch_count = 0u;
        size_t committed_count;
        size_t high_water;
        size_t j;
        uhm_result result;

        /*
         * A control-band expansion or a live slab can split one bounded
         * candidate window into several physical runs.  Process the largest
         * ready run first so the committed high-water tail is preferentially
         * left in smaller fragments.  Address-order greedy processing could
         * otherwise decommit a short run and then part of a long run, turning
         * one sufficient platform call into two.  The scan is O(B^2) only in
         * the cold full-drain path and B is capped by
         * UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX.
         */
        while (scan < slab_count) {
            size_t run_count = 1u;

            if (processed[scan]) {
                ++scan;
                continue;
            }
            if (fallback_start == SIZE_MAX) {
                fallback_start = scan;
            }
            if (!uhm_slab_empty_decommit_ready(slabs[scan])) {
                ++scan;
                continue;
            }
            while (scan + run_count < slab_count &&
                   !processed[scan + run_count]) {
                uhm_slab *prev = slabs[scan + run_count - 1u];
                uhm_slab *next = slabs[scan + run_count];
                uintptr_t prev_end;

                if (!uhm_slab_empty_decommit_ready(next) ||
                    next->size_class != slabs[scan]->size_class ||
                    uhm_range_overflows(prev->base, prev->size, &prev_end) ||
                    prev_end != next->base) {
                    break;
                }
                ++run_count;
            }
            if (run_count > best_count) {
                best_start = scan;
                best_count = run_count;
            }
            scan += run_count;
        }
        i = best_start != SIZE_MAX ? best_start : fallback_start;
        if (i == SIZE_MAX) {
            return first_error != UHM_OK ? first_error : UHM_ERR_CORRUPTION;
        }
        slab = slabs[i];
        contiguous_count = best_start != SIZE_MAX ? best_count : 1u;
        for (j = 0u; j < contiguous_count; ++j) {
            if (!processed[i + j]) {
                processed[i + j] = 1u;
                ++processed_count;
            }
        }
        if (!uhm_slab_empty_decommit_ready(slab)) {
            result = uhm_slab_maybe_decommit_empty(heap, slab);
            if (result != UHM_OK && result != UHM_ERR_UNSUPPORTED &&
                result != UHM_ERR_PLATFORM && first_error == UHM_OK) {
                first_error = result;
            }
            continue;
        }
        committed_count =
            uhm_slab_committed_empty_count(heap, slab->size_class);
        high_water = uhm_slab_empty_committed_cache_high_water(
            heap, slab->size_class);
        if (committed_count > high_water) {
            batch_count = committed_count - high_water;
            if (batch_count > contiguous_count) {
                batch_count = contiguous_count;
            }
        }
        if (batch_count >= 2u) {
            for (j = 0u; j < batch_count; ++j) {
                uhm_slab_note_decommit_pressure(heap, slabs[i + j]);
            }
            result = uhm_slab_decommit_empty_run(
                heap, &slabs[i], batch_count);
            if (result != UHM_OK && result != UHM_ERR_UNSUPPORTED &&
                result != UHM_ERR_PLATFORM && first_error == UHM_OK) {
                first_error = result;
            }
            /*
             * The remainder of this contiguous class run is the retained
             * high-water tail.  If the platform call failed, do not turn the
             * same flush into an immediate per-slab retry storm after the
             * transaction restored the run.
             */
            continue;
        }
        result = uhm_slab_maybe_decommit_empty(heap, slab);
        if (result != UHM_OK && result != UHM_ERR_UNSUPPORTED &&
            result != UHM_ERR_PLATFORM && first_error == UHM_OK) {
            first_error = result;
        }
    }
    return first_error;
}

static uhm_result uhm_slab_maybe_decommit_empty_all(uhm_heap *heap) {
    uhm_result result = UHM_OK;
    uhm_slab *candidates[UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX];
    uhm_slab *slab;
    size_t candidate_count = 0u;
    size_t size_class;

    if (!heap) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        if (slab->magic != UHM_SLAB_MAGIC) {
            return UHM_ERR_CORRUPTION;
        }
        if (slab->ctx_cached_slots != 0 ||
            slab->state != UHM_SLAB_STATE_EMPTY ||
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
            continue;
        }
        candidates[candidate_count++] = slab;
        if (candidate_count == UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX) {
            uhm_result batch_result = uhm_slab_decommit_candidate_batch(
                heap, candidates, candidate_count);
            if (batch_result != UHM_OK && result == UHM_OK) {
                result = batch_result;
            }
            candidate_count = 0u;
        }
    }
    if (candidate_count != 0u) {
        uhm_result batch_result = uhm_slab_decommit_candidate_batch(
            heap, candidates, candidate_count);
        if (batch_result != UHM_OK && result == UHM_OK) {
            result = batch_result;
        }
    }
    for (size_class = 0; size_class < UHM_SIZE_CLASS_COUNT; ++size_class) {
        uhm_slab_empty_committed_cache_maybe_shrink(
            heap, (uint16_t)size_class);
    }
    return result;
}

static uhm_result uhm_slab_recommit(uhm_heap *heap, uhm_slab *slab) {
    size_t first;
    size_t count;
    uhm_result result;
    uint64_t original_descriptor_id;
    int commit_may_need_rollback = 0;

    if ((slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0) {
        return UHM_OK;
    }
    if ((slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0) {
        return UHM_ERR_BUSY;
    }
    if (slab->state != UHM_SLAB_STATE_EMPTY ||
        slab->live_slots != 0 ||
        slab->free_slots != slab->total_slots ||
        uhm_slab_free_head_snapshot(slab) != 0 ||
        slab->ctx_cached_slots != 0 ||
        uhm_slab_remote_inflight_load(slab) != 0 ||
        uhm_slab_remote_pending_load(slab) != 0 ||
        !uhm_slab_remote_head_empty_snapshot(slab) ||
        !uhm_slab_remote_lf_empty(slab)) {
        return UHM_ERR_CORRUPTION;
    }

    if (!uhm_page_span(heap, slab->base, slab->size, &first, &count)) {
        return UHM_ERR_CORRUPTION;
    }

    original_descriptor_id = slab->desc.id;
    slab->flags |= UHM_SLAB_FLAG_VM_BUSY;
    {
        uhm_vm_call_record commit_record;
        uhm_vm_call_record numa_record;
        int32_t numa_node;

        memset(&commit_record, 0, sizeof(commit_record));
        memset(&numa_record, 0, sizeof(numa_record));
        numa_node =
            uhm_numa_effective_node(heap, UHM_NUMA_POLICY_DEFAULT, -1);
        uhm_unlock(&heap->lock);
        commit_record = uhm_vm_commit_unlocked_call(
            heap, (void *)slab->base, slab->size,
            UHM_PROT_READ | UHM_PROT_WRITE);
        if (commit_record.result == UHM_OK) {
            numa_record = uhm_vm_numa_hint_unlocked_call(
                heap, (void *)slab->base, slab->size,
                UHM_NUMA_POLICY_DEFAULT, numa_node, 0u);
        }
        commit_may_need_rollback =
            commit_record.result == UHM_OK || commit_record.did_call;
        uhm_lock(&heap->lock);
        result = uhm_vm_commit_publish_record_locked(heap, &commit_record);
        uhm_vm_numa_hint_publish_best_effort_record_locked(heap,
                                                          &numa_record);
        if (result == UHM_OK &&
            (!uhm_heap_is_accessible_pinned(heap) ||
             slab->magic != UHM_SLAB_MAGIC ||
             (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 ||
             (slab->flags & UHM_SLAB_FLAG_VM_BUSY) == 0 ||
             slab->desc.id != original_descriptor_id ||
             slab->state != UHM_SLAB_STATE_EMPTY ||
             slab->live_slots != 0 ||
             slab->free_slots != slab->total_slots ||
             uhm_slab_free_head_snapshot(slab) != 0 ||
             slab->ctx_cached_slots != 0 ||
             uhm_slab_remote_inflight_load(slab) != 0 ||
             uhm_slab_remote_pending_load(slab) != 0 ||
             !uhm_slab_remote_head_empty_snapshot(slab) ||
             !uhm_slab_remote_lf_empty(slab))) {
            result = UHM_ERR_CORRUPTION;
        }
    }
    if (result != UHM_OK) {
        if (slab->magic == UHM_SLAB_MAGIC &&
            (slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0) {
            if (commit_may_need_rollback &&
                (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
                uhm_vm_decommit_rollback_unlocked_preserve_error(
                    heap, (void *)slab->base, slab->size);
            }
            slab->flags &= ~UHM_SLAB_FLAG_VM_BUSY;
        }
        return result;
    }

    {
        uhm_lockset publish_lockset;
        uhm_page_lock_shard_range(heap, first, count, &publish_lockset);
        uhm_slab_lock_all_shards(heap, &publish_lockset);
        {
            int was_committed_empty =
                uhm_slab_committed_empty_counted(slab);
            slab->flags &= ~UHM_SLAB_FLAG_DECOMMITTED;
            uhm_slab_committed_empty_apply_transition(
                heap, slab, was_committed_empty);
        }
        slab->flags &= ~UHM_SLAB_FLAG_VM_BUSY;
        slab->generation = uhm_heap_generation_next(heap);
        uhm_slab_descriptor_set(slab);
        uhm_slab_rebuild_free_slots(slab);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, first, count, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
            UHM_PROT_READ | UHM_PROT_WRITE, slab->desc.id,
            slab->generation, &publish_lockset, 0);
        if (result != UHM_OK) {
            uhm_slab_raw_lock(slab);
            slab->free_head = 0;
            uhm_slab_summary_publish_raw_locked(slab);
            uhm_slab_raw_unlock(slab);
            {
                int was_committed_empty =
                    uhm_slab_committed_empty_counted(slab);
                slab->flags |= UHM_SLAB_FLAG_DECOMMITTED;
                uhm_slab_committed_empty_apply_transition(
                    heap, slab, was_committed_empty);
            }
            slab->flags |= UHM_SLAB_FLAG_VM_BUSY;
            slab->generation = uhm_heap_generation_next(heap);
            uhm_slab_descriptor_set(slab);
            (void)uhm_page_span_mark_range_locked_prepared(
                heap, first, count, UHM_PAGE_OWNER_MALLOC,
                UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_DECOMMITTED,
                UHM_PROT_NONE, slab->desc.id, slab->generation,
                &publish_lockset, 0);
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result != UHM_OK) {
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)slab->base, slab->size);
        if (slab->magic == UHM_SLAB_MAGIC &&
            (slab->flags & UHM_SLAB_FLAG_VM_BUSY) != 0) {
            slab->flags &= ~UHM_SLAB_FLAG_VM_BUSY;
        }
        return result;
    }
    heap->stats.committed_bytes += slab->size;
    if (uhm_diag_note_recommit(heap, slab->base, slab->size) != 0) {
        uhm_slab_empty_committed_cache_adaptive_grow(heap,
                                                     slab->size_class);
    }
    return UHM_OK;
}

static uhm_result uhm_slab_claim_commit(uhm_heap *heap, size_t slab_size,
                                        uhm_range_claim *claim) {
    uhm_result result;
    uhm_vm_call_record commit_record;
    uhm_vm_call_record numa_record;
    int32_t numa_node;

    for (;;) {
        result = uhm_claim_low_watermark(heap, slab_size,
                                         heap->F.vm.page_size, claim);
        if (result != UHM_ERR_BUSY) {
            break;
        }
        uhm_unlock(&heap->lock);
        uhm_spinlock_yield();
        uhm_lock(&heap->lock);
        if (!uhm_heap_is_accessible_pinned(heap)) {
            return UHM_ERR_BUSY;
        }
    }
    if (result != UHM_OK) {
        return result;
    }
    result = uhm_page_prepare_range(heap, claim->addr, claim->size);
    if (result != UHM_OK) {
        (void)uhm_claim_abort(heap, claim, 0);
        return result;
    }
    result = uhm_claim_begin_low_watermark_inflight(heap, claim);
    if (result != UHM_OK) {
        (void)uhm_claim_abort(heap, claim, 0);
        return result;
    }

    memset(&commit_record, 0, sizeof(commit_record));
    memset(&numa_record, 0, sizeof(numa_record));
    numa_node = uhm_numa_effective_node(heap, UHM_NUMA_POLICY_DEFAULT, -1);
    uhm_unlock(&heap->lock);
    commit_record = uhm_vm_commit_unlocked_call(
        heap, (void *)claim->addr, claim->size,
        UHM_PROT_READ | UHM_PROT_WRITE);
    if (commit_record.result == UHM_OK) {
        numa_record = uhm_vm_numa_hint_unlocked_call(
            heap, (void *)claim->addr, claim->size,
            UHM_NUMA_POLICY_DEFAULT, numa_node, 0u);
    }
    uhm_lock(&heap->lock);
    result = uhm_vm_commit_publish_record_locked(heap, &commit_record);
    uhm_vm_numa_hint_publish_best_effort_record_locked(heap, &numa_record);
    if (result != UHM_OK) {
        if (commit_record.did_call) {
            uhm_vm_decommit_rollback_unlocked_preserve_error(
                heap, (void *)claim->addr, claim->size);
        }
        (void)uhm_claim_abort(heap, claim, 0);
        return result;
    }
    return UHM_OK;
}

static uhm_slab *uhm_slab_new(uhm_heap *heap, uint16_t size_class,
                              size_t slot_size) {
    size_t header = uhm_slab_slot_header_size();
    size_t stride;
    size_t slot_count;
    size_t slab_size;
    uhm_slab *slab;
    uhm_range_claim claim;
    size_t publish_first;
    size_t publish_count;
    uhm_result result;

    if (uhm_add_overflows_size(header, slot_size, &stride) ||
        uhm_align_up_size(stride, UHM_MIN_ALIGNMENT, &stride)) {
        return 0;
    }

    slab_size = heap->F.vm.page_size;
    slot_count = slab_size / stride;
    if (slot_count == 0) {
        return 0;
    }

    slab = 0;
    result = uhm_slab_claim_commit(heap, slab_size, &claim);
    if (result != UHM_OK) {
        return 0;
    }

    slab = (uhm_slab *)uhm_control_alloc_existing_uninitialized(
        heap, sizeof(*slab), alignof(uhm_slab));
    if (!slab) {
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)claim.addr, claim.size);
        (void)uhm_claim_abort(heap, &claim, 0);
        slab = (uhm_slab *)uhm_control_alloc_uninitialized(
            heap, sizeof(*slab), alignof(uhm_slab));
        if (!slab) {
            return 0;
        }
        result = uhm_slab_claim_commit(heap, slab_size, &claim);
        if (result != UHM_OK) {
            return 0;
        }
    }
    memset(slab, 0, sizeof(*slab));

    slab->magic = UHM_SLAB_MAGIC;
    slab->size_class = size_class;
    slab->base = claim.addr;
    slab->size = claim.size;
    slab->slot_size = slot_size;
    slab->slot_stride = stride;
    slab->total_slots = slot_count;
    slab->free_slots = slot_count;
    slab->generation = uhm_heap_generation_next(heap);
    uhm_spinlock_init_ex(&slab->raw_lock, heap->core.spinlock_mode,
                         heap->core.spinlock_yield_after_spins);
    uhm_spinlock_set_diagnostics_enabled(
        &slab->raw_lock,
        (uint32_t)uhm_spinlock_diagnostics_enabled(&heap->lock));
    uhm_slab_summary_init(slab);
    uhm_slab_descriptor_set(slab);
    uhm_slab_rebuild_free_slots(slab);

    if (!uhm_page_span(heap, slab->base, slab->size, &publish_first,
                       &publish_count)) {
        result = UHM_ERR_CORRUPTION;
    } else {
        uhm_lockset publish_lockset;
        uhm_page_lock_shard_range(heap, publish_first, publish_count,
                                  &publish_lockset);
        uhm_slab_lock_all_shards(heap, &publish_lockset);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, publish_first, publish_count, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
            UHM_PROT_READ | UHM_PROT_WRITE, slab->desc.id,
            slab->generation, &publish_lockset, 0);
        if (result == UHM_OK) {
            uhm_slab_insert_all(heap, slab);
            uhm_slab_state_link(heap, slab, UHM_SLAB_STATE_EMPTY);
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result != UHM_OK) {
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)slab->base, slab->size);
        (void)uhm_claim_abort(heap, &claim, 0);
        uhm_extent_descriptor_retire(&slab->desc);
        slab->magic = 0;
        return 0;
    }

    heap->stats.committed_bytes += slab->size;
    uhm_claim_disarm(heap, &claim);
    return slab;
}

static int uhm_lockless_remote_free_enabled(const uhm_heap *heap) {
    return heap &&
           (heap->state_flags & UHM_CONFIG_LOCKLESS_REMOTE_FREE) != 0 &&
           uhm_lockless_remote_primitives_supported();
}

static int uhm_low_lock_remote_free_enabled(const uhm_heap *heap) {
#if UINTPTR_MAX == UINT32_MAX
    return heap && heap->core.low_lock_remote_free_enabled != 0u &&
           uhm_atomic_u32_is_lock_free();
#else
    (void)heap;
    return 0;
#endif
}

static int uhm_slab_payload_page_record_snapshot_ref(
    uhm_heap *heap,
    uintptr_t payload,
    uhm_page_record *out,
    uhm_page_record **out_record) {
    size_t page_index;
    size_t page_size;
    size_t shard;
    uhm_page_record *record;
    int ok = 0;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (out_record) {
        *out_record = 0;
    }
    if (!heap || !out || payload < heap->core.arena_base ||
        payload >= heap->core.arena_end) {
        return 0;
    }
    page_size = heap->F.vm.page_size;
    if (page_size == 0) {
        return 0;
    }
    page_index = (size_t)(payload - heap->core.arena_base) / page_size;
    if (page_index >= heap->core.page_count) {
        return 0;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_atomic_add_u64(&heap->core.page_span_calls, UINT64_C(1));
    uhm_diag_atomic_add_u64(&heap->core.page_span_pages, UINT64_C(1));
#endif
    shard = uhm_page_shard_for_index(page_index);
    uhm_lock(&heap->core.page_map_locks[shard].lock);
    uhm_diag_note_page_lock_range(heap, 1u);
    record = uhm_page_record_get_unlocked(heap, page_index);
    if (record) {
        *out = *record;
        if (out_record) {
            *out_record = record;
        }
        ok = 1;
    }
    uhm_unlock(&heap->core.page_map_locks[shard].lock);
    return ok;
}

static int uhm_slab_payload_page_record_snapshot(uhm_heap *heap,
                                                 uintptr_t payload,
                                                 uhm_page_record *out) {
    return uhm_slab_payload_page_record_snapshot_ref(heap, payload, out, 0);
}

static uhm_slab *uhm_slab_from_page_record_descriptor(
    uhm_heap *heap,
    const uhm_page_record *record,
    int exact_generation) {
    uintptr_t desc_addr;
    uintptr_t slab_addr;
    uhm_slab *slab;

    if (!heap || !record ||
        record->owner != UHM_PAGE_OWNER_MALLOC ||
        record->va_state != UHM_PAGE_VA_MALLOC_LIVE ||
        record->commit_state != UHM_PAGE_COMMIT_LIVE ||
        record->prot != (UHM_PROT_READ | UHM_PROT_WRITE) ||
        record->descriptor_id == 0 ||
        record->descriptor_id > (uint64_t)UINTPTR_MAX) {
        return 0;
    }
    desc_addr = (uintptr_t)record->descriptor_id;
    if (desc_addr < offsetof(uhm_slab, desc)) {
        return 0;
    }
    slab_addr = desc_addr - offsetof(uhm_slab, desc);
    if ((slab_addr % alignof(uhm_slab)) != 0 ||
        !uhm_metadata_range_is_live_control(heap, slab_addr,
                                            sizeof(uhm_slab))) {
        return 0;
    }
    slab = (uhm_slab *)slab_addr;
    if (slab->magic != UHM_SLAB_MAGIC ||
        slab->desc.magic != UHM_EXTENT_DESCRIPTOR_MAGIC ||
        slab->desc.kind != UHM_EXTENT_KIND_MALLOC ||
        slab->desc.addr != slab->base ||
        slab->desc.size != slab->size ||
        slab->desc.owner != UHM_PAGE_OWNER_MALLOC ||
        slab->desc.va_state != UHM_PAGE_VA_MALLOC_LIVE ||
        slab->desc.commit_state != UHM_PAGE_COMMIT_LIVE ||
        slab->desc.prot != (UHM_PROT_READ | UHM_PROT_WRITE) ||
        slab->desc.id != record->descriptor_id ||
        slab->desc.id != uhm_extent_descriptor_id(&slab->desc) ||
        slab->desc.object != slab ||
        (exact_generation && slab->desc.generation != record->generation)) {
        return 0;
    }
    return slab;
}

static uhm_slab *uhm_slab_slot_owner_from_heap(uhm_heap *heap,
                                               const uhm_slab_slot *slot,
                                               int exact_generation) {
    uintptr_t slot_addr = (uintptr_t)slot;
    uintptr_t slab_end;
    size_t offset;
    size_t slot_index;
    uhm_page_record record;
    uhm_slab *slab;

    if (!heap || !slot ||
        slot_addr < heap->core.arena_base ||
        slot_addr >= heap->core.arena_end ||
        !uhm_slab_payload_page_record_snapshot(heap, slot_addr, &record)) {
        return 0;
    }
    slab = uhm_slab_from_page_record_descriptor(heap, &record,
                                                exact_generation);
    if (!slab || slab->slot_stride == 0 ||
        uhm_range_overflows(slab->base, slab->size, &slab_end) ||
        slot_addr < slab->base || slot_addr >= slab_end) {
        return 0;
    }
    offset = (size_t)(slot_addr - slab->base);
    if ((offset % slab->slot_stride) != 0) {
        return 0;
    }
    slot_index = offset / slab->slot_stride;
    if (slot_index >= slab->total_slots ||
        slot->magic != UHM_SLAB_SLOT_MAGIC ||
        slot->size_class != slab->size_class ||
        slot->state != 0) {
        return 0;
    }
    return slab;
}

static int uhm_slab_slot_belongs_to_heap(uhm_heap *heap,
                                         const uhm_slab_slot *slot,
                                         const uhm_slab *slab,
                                         int exact_generation) {
    return slab &&
           uhm_slab_slot_owner_from_heap(heap, slot, exact_generation) == slab;
}

static size_t uhm_slab_slot_owner_slot_size_from_heap(
    uhm_heap *heap,
    const uhm_slab_slot *slot,
    int exact_generation) {
    uhm_slab *slab =
        uhm_slab_slot_owner_from_heap(heap, slot, exact_generation);
    return slab ? slab->slot_size : 0;
}

static uhm_slab_slot *uhm_slab_slot_from_payload_record(
    uhm_heap *heap,
    uintptr_t payload,
    const uhm_page_record *record,
    int live_only,
    int exact_generation,
    uhm_slab **out_slab) {
    size_t header = uhm_slab_slot_header_size();
    uintptr_t slot_addr;
    uintptr_t slot_end;
    uintptr_t slab_end;
    size_t offset;
    size_t slot_index;
    uhm_slab *slab;
    uhm_slab_slot *slot;

    if (out_slab) {
        *out_slab = 0;
    }
    if (!heap || !record || payload < header) {
        return 0;
    }
    slab = uhm_slab_from_page_record_descriptor(heap, record,
                                                exact_generation);
    if (!slab || slab->slot_stride == 0 ||
        uhm_range_overflows(slab->base, slab->size, &slab_end)) {
        return 0;
    }
    slot_addr = payload - header;
    if (slot_addr < slab->base ||
        uhm_range_overflows(slot_addr, sizeof(*slot), &slot_end) ||
        slot_end > slab_end ||
        (slot_addr % alignof(uhm_slab_slot)) != 0) {
        return 0;
    }
    offset = (size_t)(slot_addr - slab->base);
    if ((offset % slab->slot_stride) != 0) {
        return 0;
    }
    slot_index = offset / slab->slot_stride;
    if (slot_index >= slab->total_slots ||
        payload != slot_addr + header) {
        return 0;
    }
    slot = (uhm_slab_slot *)slot_addr;
    if (slot->magic != UHM_SLAB_SLOT_MAGIC ||
        slot->size_class != slab->size_class ||
        slot->state != 0) {
        return 0;
    }
    if (live_only) {
        if (!uhm_slab_slot_live_state_atomic_load(slot, slab, 0, 0)) {
            return 0;
        }
        if (out_slab) {
            *out_slab = slab;
        }
        return slot;
    }
    if (!uhm_slab_slot_state_valid_for_check(slot)) {
        return 0;
    }
    if (out_slab) {
        *out_slab = slab;
    }
    return slot;
}

static uhm_slab_slot *uhm_find_slab_slot_by_payload_with_slab(
    uhm_heap *heap,
    const void *ptr,
    int live_only,
    uhm_slab **out_slab) {
    uintptr_t payload = (uintptr_t)ptr;
    uhm_page_record record;

    if (out_slab) {
        *out_slab = 0;
    }
    if (!heap || !ptr ||
        payload < heap->core.arena_base || payload >= heap->core.arena_end ||
        !uhm_slab_payload_page_record_snapshot(heap, payload, &record)) {
        return 0;
    }
    return uhm_slab_slot_from_payload_record(heap, payload, &record,
                                             live_only, 0, out_slab);
}

static uhm_slab_slot *uhm_find_slab_slot_by_payload(uhm_heap *heap,
                                                    const void *ptr,
                                                    int live_only) {
    return uhm_find_slab_slot_by_payload_with_slab(heap, ptr, live_only, 0);
}

static int uhm_slab_descriptor_live_for_lockless(uhm_heap *heap,
                                                 const uhm_slab *slab) {
    if (!heap || !slab ||
        slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
        !uhm_extent_descriptor_matches(&slab->desc, UHM_EXTENT_KIND_MALLOC,
                                       slab->base, slab->size,
                                       UHM_PAGE_OWNER_MALLOC,
                                       UHM_PAGE_VA_MALLOC_LIVE,
                                       UHM_PAGE_COMMIT_LIVE,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       slab->generation, slab)) {
        return 0;
    }
    return uhm_page_range_matches_descriptor(
        heap, slab->base, slab->size, UHM_PAGE_OWNER_MALLOC,
        UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
        UHM_PROT_READ | UHM_PROT_WRITE, &slab->desc, 1, 1);
}

static uhm_slab_slot *uhm_slab_slot_from_payload_lockless_ex(
    uhm_heap *heap,
    void *ptr,
    uhm_metadata_reader_state *reader,
    int require_tagged) {
    uintptr_t payload = (uintptr_t)ptr;
    uintptr_t slot_addr;
    size_t header = uhm_slab_slot_header_size();
    uhm_page_record record;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uintptr_t slot_end;
    uintptr_t slab_end;
    size_t offset;

    if (!heap || !ptr || payload < header ||
        payload < heap->core.arena_base || payload >= heap->core.arena_end) {
        return 0;
    }
    if (!uhm_slab_payload_page_record_snapshot(heap, payload, &record)) {
        return 0;
    }
    slab = uhm_slab_from_page_record_descriptor(heap, &record, 1);
    if (!slab) {
        return 0;
    }
    if (reader && !uhm_metadata_reader_hazard_set(reader, 1u, slab)) {
        return 0;
    }

    slot_addr = payload - header;
    if (slot_addr < heap->core.arena_base ||
        uhm_range_overflows(slot_addr, sizeof(*slot), &slot_end) ||
        slot_end > heap->core.arena_end ||
        (slot_addr % alignof(uhm_slab_slot)) != 0) {
        return 0;
    }
    slot = (uhm_slab_slot *)slot_addr;
    if (reader && !uhm_metadata_reader_hazard_set(reader, 0u, slot)) {
        return 0;
    }
    if (slot->magic != UHM_SLAB_SLOT_MAGIC ||
        !slab ||
        slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
        uhm_range_overflows(slab->base, slab->size, &slab_end) ||
        slot_addr < slab->base || slot_addr >= slab_end ||
        slab->slot_stride == 0 ||
        slot->size_class != slab->size_class ||
        slot->state != 0) {
        return 0;
    }
    if (!uhm_slab_descriptor_live_for_lockless(heap, slab)) {
        return 0;
    }
    offset = (size_t)(slot_addr - slab->base);
    if ((offset % slab->slot_stride) != 0 ||
        payload != slot_addr + header ||
        !uhm_slab_slot_live_state_atomic_load(slot, slab, 0, 0) ||
        (require_tagged &&
         !uhm_tagged_freelist_ptr_packable(&slot->remote_link))) {
        return 0;
    }
    return slot;
}

static uhm_slab_slot *uhm_slab_slot_from_payload_lockless(
    uhm_heap *heap,
    void *ptr,
    uhm_metadata_reader_state *reader) {
    return uhm_slab_slot_from_payload_lockless_ex(heap, ptr, reader, 1);
}

