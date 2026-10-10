static uint32_t uhm_slab_commit_state(const uhm_slab *slab) {
    return (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0
               ? UHM_PAGE_COMMIT_DECOMMITTED
               : UHM_PAGE_COMMIT_LIVE;
}

static uint32_t uhm_slab_prot(const uhm_slab *slab) {
    return (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0
               ? UHM_PROT_NONE
               : (UHM_PROT_READ | UHM_PROT_WRITE);
}

static void uhm_slab_descriptor_set(uhm_slab *slab) {
    if (!slab) {
        return;
    }
    uhm_extent_descriptor_set(&slab->desc, UHM_EXTENT_KIND_MALLOC,
                              slab->base, slab->size, UHM_PAGE_OWNER_MALLOC,
                              UHM_PAGE_VA_MALLOC_LIVE,
                              uhm_slab_commit_state(slab),
                              uhm_slab_prot(slab),
                              slab->generation, slab);
}

static uhm_slab **uhm_slab_state_head(uhm_heap *heap, uint16_t size_class,
                                      uint16_t state) {
    if (size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    if (state == UHM_SLAB_STATE_PARTIAL) {
        return &heap->core.slab_class_heads[size_class];
    }
    if (state == UHM_SLAB_STATE_FULL) {
        return &heap->core.slab_full_heads[size_class];
    }
    if (state == UHM_SLAB_STATE_EMPTY) {
        return &heap->core.slab_empty_heads[size_class];
    }
    return 0;
}

#define UHM_SLAB_SUMMARY_ACTIVE_MASK UINT64_C(0xffffffff)
#define UHM_SLAB_SUMMARY_VERSION_ONE (UINT64_C(1) << 32)
#define UHM_SLAB_SUMMARY_END_DELTA \
    (UHM_SLAB_SUMMARY_VERSION_ONE - UINT64_C(1))

static void uhm_slab_summary_update_begin(uhm_slab *slab) {
    if (slab) {
        (void)atomic_fetch_add_explicit(&slab->summary_seq, 1u,
                                        UHM_ATOMIC_ACQ_REL);
    }
}

static void uhm_slab_summary_update_end(uhm_slab *slab) {
    if (slab) {
        (void)atomic_fetch_add_explicit(&slab->summary_seq,
                                        UHM_SLAB_SUMMARY_END_DELTA,
                                        UHM_ATOMIC_RELEASE);
    }
}

static int uhm_slab_summary_update_active(const uhm_slab *slab) {
    uint64_t seq;

    if (!slab) {
        return 0;
    }
    seq = atomic_load_explicit(&slab->summary_seq, UHM_ATOMIC_ACQUIRE);
    return (seq & UHM_SLAB_SUMMARY_ACTIVE_MASK) != 0;
}

static void uhm_slab_raw_lock(uhm_slab *slab) {
    if (slab) {
        uhm_lock(&slab->raw_lock);
    }
}

static void uhm_slab_raw_unlock(uhm_slab *slab) {
    if (slab) {
        uhm_unlock(&slab->raw_lock);
    }
}

static uhm_slab_slot *uhm_slab_free_head_snapshot(const uhm_slab *slab) {
    uhm_slab *mutable_slab = (uhm_slab *)slab;
    uhm_slab_slot *head;

    if (!slab) {
        return 0;
    }
    uhm_slab_raw_lock(mutable_slab);
    head = slab->free_head;
    uhm_slab_raw_unlock(mutable_slab);
    return head;
}

static int uhm_slab_free_head_available_hint(const uhm_slab *slab) {
    if (!slab) {
        return 0;
    }
    return atomic_load_explicit(&slab->free_head_ref,
                                UHM_ATOMIC_ACQUIRE) != 0;
}

static int uhm_slab_accounted_remote_alloc_candidate_hint(
    const uhm_slab *slab,
    int *out_empty_candidate) {
    uint32_t state;
    size_t live_slots;
    size_t free_slots;
    size_t cached_slots;

    if (out_empty_candidate) {
        *out_empty_candidate = 0;
    }
    if (!slab) {
        return 0;
    }
    state = atomic_load_explicit(&slab->state_mirror, UHM_ATOMIC_ACQUIRE);
    live_slots = atomic_load_explicit(&slab->live_slots_count,
                                      UHM_ATOMIC_ACQUIRE);
    free_slots = atomic_load_explicit(&slab->free_slots_count,
                                      UHM_ATOMIC_ACQUIRE);
    cached_slots = atomic_load_explicit(&slab->ctx_cached_slots,
                                        UHM_ATOMIC_ACQUIRE);
    if (state == UHM_SLAB_STATE_PARTIAL && free_slots != 0) {
        return 1;
    }
    if (state == UHM_SLAB_STATE_EMPTY && live_slots == 0 &&
        free_slots != 0 && cached_slots == 0) {
        if (out_empty_candidate) {
            *out_empty_candidate = 1;
        }
        return 1;
    }
    return 0;
}

static int uhm_slab_remote_head_empty_snapshot(const uhm_slab *slab) {
    uhm_slab *mutable_slab = (uhm_slab *)slab;
    int empty;

    if (!slab) {
        return 1;
    }
    uhm_slab_raw_lock(mutable_slab);
    empty = slab->remote_free_head == 0;
    uhm_slab_raw_unlock(mutable_slab);
    return empty;
}

static uhm_slab_slot *uhm_slab_remote_head_take(uhm_slab *slab) {
    uhm_slab_slot *head;

    if (!slab) {
        return 0;
    }
    uhm_slab_raw_lock(slab);
    head = slab->remote_free_head;
    slab->remote_free_head = 0;
    uhm_slab_raw_unlock(slab);
    return head;
}

static void uhm_slab_raw_publish_begin(uhm_slab *slab) {
    if (slab) {
        (void)atomic_fetch_add_explicit(&slab->raw_publish_seq, 1u,
                                        UHM_ATOMIC_ACQ_REL);
    }
}

static void uhm_slab_raw_publish_end(uhm_slab *slab) {
    if (slab) {
        (void)atomic_fetch_add_explicit(&slab->raw_publish_seq,
                                        UHM_SLAB_SUMMARY_END_DELTA,
                                        UHM_ATOMIC_RELEASE);
    }
}

static int uhm_slab_raw_publish_active(const uhm_slab *slab) {
    uint64_t seq;

    if (!slab) {
        return 0;
    }
    seq = atomic_load_explicit(&slab->raw_publish_seq, UHM_ATOMIC_ACQUIRE);
    return (seq & UHM_SLAB_SUMMARY_ACTIVE_MASK) != 0;
}

static void uhm_slab_summary_publish_raw_locked(uhm_slab *slab) {
    if (!slab) {
        return;
    }
    uhm_slab_summary_update_begin(slab);
    atomic_store_explicit(&slab->live_slots_count, slab->live_slots,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->free_slots_count, slab->free_slots,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->free_head_ref,
                          (uintptr_t)slab->free_head,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->state_mirror, (uint32_t)slab->state,
                          UHM_ATOMIC_RELEASE);
    uhm_slab_summary_update_end(slab);
}

static void uhm_slab_summary_publish_counts_raw_locked(uhm_slab *slab) {
    if (!slab) {
        return;
    }
    uhm_slab_summary_update_begin(slab);
    atomic_store_explicit(&slab->live_slots_count, slab->live_slots,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->free_slots_count, slab->free_slots,
                          UHM_ATOMIC_RELEASE);
    uhm_slab_summary_update_end(slab);
}

static void uhm_slab_summary_init(uhm_slab *slab) {
    if (!slab) {
        return;
    }
    atomic_init(&slab->summary_seq, 0u);
    atomic_init(&slab->raw_publish_seq, 0u);
    atomic_init(&slab->state_transition_seq, 0u);
    atomic_init(&slab->live_slots_count, slab->live_slots);
    atomic_init(&slab->free_slots_count, slab->free_slots);
    atomic_init(&slab->free_head_ref, (uintptr_t)slab->free_head);
    atomic_init(&slab->state_mirror, (uint32_t)slab->state);
}

static int uhm_slab_summary_stable_match(const uhm_slab *slab) {
    size_t attempt;

    if (!slab) {
        return 0;
    }
    for (attempt = 0; attempt < 4u; ++attempt) {
        uint64_t seq_before =
            atomic_load_explicit(&slab->summary_seq, UHM_ATOMIC_ACQUIRE);
        size_t live_slots =
            atomic_load_explicit(&slab->live_slots_count,
                                 UHM_ATOMIC_ACQUIRE);
        size_t free_slots =
            atomic_load_explicit(&slab->free_slots_count,
                                 UHM_ATOMIC_ACQUIRE);
        uintptr_t free_head =
            atomic_load_explicit(&slab->free_head_ref,
                                 UHM_ATOMIC_ACQUIRE);
        uint32_t state =
            atomic_load_explicit(&slab->state_mirror, UHM_ATOMIC_ACQUIRE);
        uint64_t seq_after =
            atomic_load_explicit(&slab->summary_seq, UHM_ATOMIC_ACQUIRE);

        if (seq_before == seq_after &&
            (seq_after & UHM_SLAB_SUMMARY_ACTIVE_MASK) == 0) {
            return live_slots == slab->live_slots &&
                   free_slots == slab->free_slots &&
                   free_head == (uintptr_t)uhm_slab_free_head_snapshot(slab) &&
                   state == (uint32_t)slab->state;
        }
    }
    return 0;
}

static int uhm_slab_summary_valid_for_check(uhm_heap *heap,
                                            const uhm_slab *slab) {
    if (uhm_slab_summary_stable_match(slab)) {
        return 1;
    }
    return heap &&
           uhm_heap_active_ops_load(heap) > 1u &&
           uhm_slab_summary_update_active(slab);
}

static int uhm_slab_committed_empty_counted(const uhm_slab *slab) {
    return slab && slab->state == UHM_SLAB_STATE_EMPTY &&
           (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
           slab->ctx_cached_slots == 0;
}

static void uhm_slab_committed_empty_apply_transition(uhm_heap *heap,
                                                      uhm_slab *slab,
                                                      int was_counted) {
    int is_counted;
    size_t *counter;

    if (!heap || !slab || slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return;
    }
    is_counted = uhm_slab_committed_empty_counted(slab);
    if (was_counted == is_counted) {
        return;
    }
    counter = &heap->core.slab_committed_empty_count[slab->size_class];
    if (is_counted) {
        ++*counter;
    } else if (*counter != 0) {
        --*counter;
    }
}

static void uhm_slab_state_unlink_raw_locked(uhm_heap *heap,
                                             uhm_slab *slab) {
    uhm_slab **head = uhm_slab_state_head(heap, slab->size_class, slab->state);
    int was_committed_empty = uhm_slab_committed_empty_counted(slab);

    if (!head) {
        slab->prev_class = 0;
        slab->next_class = 0;
        slab->state = UHM_SLAB_STATE_NONE;
        uhm_slab_committed_empty_apply_transition(heap, slab,
                                                  was_committed_empty);
        uhm_slab_summary_publish_raw_locked(slab);
        return;
    }
    if (slab->prev_class) {
        slab->prev_class->next_class = slab->next_class;
    } else if (*head == slab) {
        *head = slab->next_class;
    }
    if (slab->next_class) {
        slab->next_class->prev_class = slab->prev_class;
    }
    if (slab->state == UHM_SLAB_STATE_EMPTY &&
        heap->core.slab_empty_count[slab->size_class] != 0) {
        --heap->core.slab_empty_count[slab->size_class];
    }
    slab->prev_class = 0;
    slab->next_class = 0;
    slab->state = UHM_SLAB_STATE_NONE;
    uhm_slab_committed_empty_apply_transition(heap, slab,
                                              was_committed_empty);
    uhm_slab_summary_publish_raw_locked(slab);
}

static void uhm_slab_state_link_raw_locked(uhm_heap *heap, uhm_slab *slab,
                                           uint16_t state) {
    uhm_slab **head;
    int was_committed_empty;

    if (slab->state != UHM_SLAB_STATE_NONE) {
        uhm_slab_state_unlink_raw_locked(heap, slab);
    }
    was_committed_empty = uhm_slab_committed_empty_counted(slab);
    head = uhm_slab_state_head(heap, slab->size_class, state);
    if (!head) {
        uhm_slab_committed_empty_apply_transition(heap, slab,
                                                  was_committed_empty);
        uhm_slab_summary_publish_raw_locked(slab);
        return;
    }
    slab->state = state;
    slab->prev_class = 0;
    slab->next_class = *head;
    if (*head) {
        (*head)->prev_class = slab;
    }
    *head = slab;
    if (state == UHM_SLAB_STATE_EMPTY) {
        ++heap->core.slab_empty_count[slab->size_class];
    }
    uhm_slab_committed_empty_apply_transition(heap, slab,
                                              was_committed_empty);
    uhm_slab_summary_publish_raw_locked(slab);
}

static void uhm_slab_state_link(uhm_heap *heap, uhm_slab *slab,
                                uint16_t state) {
    uhm_slab_raw_lock(slab);
    uhm_slab_state_link_raw_locked(heap, slab, state);
    uhm_slab_raw_unlock(slab);
}

static void uhm_slab_refresh_state_raw_locked(uhm_heap *heap,
                                              uhm_slab *slab) {
    uint16_t state;

    if (slab->live_slots == 0) {
        state = UHM_SLAB_STATE_EMPTY;
    } else if (slab->free_slots == 0) {
        state = UHM_SLAB_STATE_FULL;
    } else {
        state = UHM_SLAB_STATE_PARTIAL;
    }
    if (slab->state != state) {
        uhm_slab_state_link_raw_locked(heap, slab, state);
    } else {
        uhm_slab_summary_publish_raw_locked(slab);
    }
}

#define UHM_SLAB_REMOTE_READY_IDLE     0u
#define UHM_SLAB_REMOTE_READY_QUEUED   1u
#define UHM_SLAB_REMOTE_READY_SCANNING 2u

static int uhm_slab_all_contains(uhm_slab *head, uhm_slab *needle,
                                 size_t max_count) {
    uhm_slab *slab;
    size_t seen = 0;

    for (slab = head; slab; slab = slab->next_all) {
        if (seen >= max_count) {
            return 0;
        }
        if (slab == needle) {
            return 1;
        }
        ++seen;
    }
    return 0;
}

static int uhm_slab_state_list_validate(uhm_slab *head, uhm_slab *all_head,
                                        uint16_t size_class, uint16_t state,
                                        size_t *count, size_t max_count,
                                        int allow_state_transient) {
    uhm_slab *prev = 0;
    uhm_slab *slab;
    size_t seen = 0;

    for (slab = head; slab; slab = slab->next_class) {
        if (seen >= max_count) {
            return 0;
        }
        if (slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class ||
            slab->state != state ||
            slab->prev_class != prev ||
            !uhm_slab_all_contains(all_head, slab, max_count)) {
            return 0;
        }
        if (state == UHM_SLAB_STATE_EMPTY && slab->live_slots != 0 &&
            (!allow_state_transient ||
             atomic_load_explicit(&slab->remote_ready_queued,
                                  UHM_ATOMIC_ACQUIRE) !=
                 UHM_SLAB_REMOTE_READY_SCANNING ||
             !uhm_slab_state_transition_seq_active_value(
                 uhm_slab_state_transition_seq_load(slab)))) {
            return 0;
        }
        if (state == UHM_SLAB_STATE_FULL && slab->free_slots != 0) {
            if (!allow_state_transient ||
                slab->live_slots == 0 ||
                atomic_load_explicit(&slab->remote_free_pending,
                                     UHM_ATOMIC_ACQUIRE) == 0 ||
                !uhm_slab_state_transition_seq_active_value(
                    uhm_slab_state_transition_seq_load(slab))) {
                return 0;
            }
        }
        if (state == UHM_SLAB_STATE_PARTIAL &&
            ((slab->live_slots == 0 &&
              (!allow_state_transient ||
               atomic_load_explicit(&slab->remote_free_pending,
                                    UHM_ATOMIC_ACQUIRE) == 0 ||
               !uhm_slab_state_transition_seq_active_value(
                   uhm_slab_state_transition_seq_load(slab)))) ||
             (slab->free_slots == 0 &&
              (!allow_state_transient ||
               atomic_load_explicit(&slab->remote_ready_queued,
                                    UHM_ATOMIC_ACQUIRE) !=
                   UHM_SLAB_REMOTE_READY_SCANNING ||
               !uhm_slab_state_transition_seq_active_value(
                   uhm_slab_state_transition_seq_load(slab)))))) {
            return 0;
        }
        ++seen;
        if (count) {
            ++*count;
        }
        prev = slab;
    }
    return 1;
}

static void uhm_slab_insert_all(uhm_heap *heap, uhm_slab *slab) {
    slab->prev_all = 0;
    slab->next_all = heap->core.slab_all_head;
    if (heap->core.slab_all_head) {
        heap->core.slab_all_head->prev_all = slab;
    }
    heap->core.slab_all_head = slab;
}

static uint64_t uhm_slab_slot_state_pack(uint32_t flags,
                                         uint32_t requested_size) {
    return ((uint64_t)requested_size << 32) | (uint64_t)flags;
}

static uint32_t uhm_slab_slot_state_flags(uint64_t state_word) {
    return (uint32_t)(state_word & UINT64_C(0xffffffff));
}

static uint32_t uhm_slab_slot_state_requested(uint64_t state_word) {
    return (uint32_t)(state_word >> 32);
}

static uint64_t uhm_slab_slot_state_load(const uhm_slab_slot *slot) {
    return atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE);
}

static uint32_t uhm_slab_slot_flags_load(const uhm_slab_slot *slot) {
    return uhm_slab_slot_state_flags(uhm_slab_slot_state_load(slot));
}

static uint32_t uhm_slab_slot_requested_load(const uhm_slab_slot *slot) {
    return uhm_slab_slot_state_requested(uhm_slab_slot_state_load(slot));
}

static void uhm_slab_slot_state_init(uhm_slab_slot *slot, uint32_t flags,
                                     uint32_t requested_size) {
    slot->flags = flags;
    slot->requested_size = requested_size;
    atomic_init(&slot->state_word,
                uhm_slab_slot_state_pack(flags, requested_size));
}

static int uhm_slab_slot_state_cas(uhm_slab_slot *slot,
                                   uint32_t expected_flags,
                                   uint32_t expected_requested,
                                   uint32_t desired_flags,
                                   uint32_t desired_requested) {
    uint64_t expected =
        uhm_slab_slot_state_pack(expected_flags, expected_requested);
    uint64_t desired =
        uhm_slab_slot_state_pack(desired_flags, desired_requested);

    if (!atomic_compare_exchange_strong_explicit(
            &slot->state_word, &expected, desired, UHM_ATOMIC_ACQ_REL,
            UHM_ATOMIC_ACQUIRE)) {
        return 0;
    }
    slot->flags = desired_flags;
    slot->requested_size = desired_requested;
    return 1;
}

/*
 * The 32-bit low-lock path already owns the slab raw lock and has no other
 * lockless slot writer enabled.  A direct atomic store avoids requiring an
 * 8-byte compare-exchange instruction while keeping state_word observable to
 * diagnostic readers.
 */
static void uhm_slab_slot_state_store_locked(uhm_slab_slot *slot,
                                             uint32_t desired_flags,
                                             uint32_t desired_requested) {
    if (!slot) {
        return;
    }
    atomic_store_explicit(
        &slot->state_word,
        uhm_slab_slot_state_pack(desired_flags, desired_requested),
        UHM_ATOMIC_RELEASE);
    slot->flags = desired_flags;
    slot->requested_size = desired_requested;
}

static int uhm_slab_slot_state_cas_atomic_only(
    uhm_slab_slot *slot,
    uint32_t expected_flags,
    uint32_t expected_requested,
    uint32_t desired_flags,
    uint32_t desired_requested) {
    uint64_t expected =
        uhm_slab_slot_state_pack(expected_flags, expected_requested);
    uint64_t desired =
        uhm_slab_slot_state_pack(desired_flags, desired_requested);

    return atomic_compare_exchange_strong_explicit(
        &slot->state_word, &expected, desired, UHM_ATOMIC_ACQ_REL,
        UHM_ATOMIC_ACQUIRE);
}

static int uhm_slab_slot_state_mirror_matches(const uhm_slab_slot *slot) {
    uint64_t state_word;

    if (!slot) {
        return 0;
    }
    state_word = uhm_slab_slot_state_load(slot);
    return slot->flags == uhm_slab_slot_state_flags(state_word) &&
           slot->requested_size == uhm_slab_slot_state_requested(state_word);
}

static int uhm_slab_slot_ctx_cache_state_valid_with_slab(
    const uhm_slab_slot *slot,
    const uhm_slab *slab,
    uint32_t *out_cached_requested) {
    uint64_t state_word;
    uint32_t flags;
    uint32_t requested;
    uint32_t cached_requested;

    if (out_cached_requested) {
        *out_cached_requested = 0;
    }
    if (!slot || !slab || slab->slot_size > UINT32_MAX) {
        return 0;
    }
    state_word = uhm_slab_slot_state_load(slot);
    flags = uhm_slab_slot_state_flags(state_word);
    requested = uhm_slab_slot_state_requested(state_word);
    cached_requested = slot->requested_size;
    if (requested != 0 ||
        flags != (UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_CTX_CACHE) ||
        slot->flags != flags ||
        cached_requested == 0 ||
        cached_requested > (uint32_t)slab->slot_size) {
        return 0;
    }
    if (out_cached_requested) {
        *out_cached_requested = cached_requested;
    }
    return 1;
}

static int uhm_slab_slot_ctx_cache_state_valid(
    const uhm_slab_slot *slot,
    uint32_t *out_cached_requested) {
    return uhm_slab_slot_ctx_cache_state_valid_with_slab(
        slot, uhm_slab_slot_owner(slot), out_cached_requested);
}

static int uhm_slab_slot_live_state_atomic_load(const uhm_slab_slot *slot,
                                                const uhm_slab *slab,
                                                uint32_t *out_flags,
                                                uint32_t *out_requested) {
    uint64_t state_word;
    uint32_t flags;
    uint32_t requested;

    if (out_flags) {
        *out_flags = 0;
    }
    if (out_requested) {
        *out_requested = 0;
    }
    if (!slot || !slab) {
        return 0;
    }
    state_word = uhm_slab_slot_state_load(slot);
    flags = uhm_slab_slot_state_flags(state_word);
    requested = uhm_slab_slot_state_requested(state_word);
    if ((flags != 0 && flags != UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED) ||
        requested == 0 || requested > slab->slot_size) {
        return 0;
    }
    if (out_flags) {
        *out_flags = flags;
    }
    if (out_requested) {
        *out_requested = requested;
    }
    return 1;
}

static int uhm_slab_slot_live_state_mirror_valid_with_slab(
    const uhm_slab_slot *slot,
    const uhm_slab *slab,
    uint32_t *out_flags,
    uint32_t *out_requested) {
    uint32_t flags;
    uint32_t requested;

    if (out_flags) {
        *out_flags = 0;
    }
    if (out_requested) {
        *out_requested = 0;
    }
    if (!slot || !slab ||
        !uhm_slab_slot_live_state_atomic_load(slot, slab, &flags,
                                              &requested)) {
        return 0;
    }
    if (flags == 0) {
        if (slot->flags != 0 || slot->requested_size != requested) {
            return 0;
        }
    } else if (flags == UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED) {
        if (slot->flags != 0 || slot->requested_size == 0 ||
            slot->requested_size > slab->slot_size) {
            return 0;
        }
    } else {
        return 0;
    }
    if (out_flags) {
        *out_flags = flags;
    }
    if (out_requested) {
        *out_requested = requested;
    }
    return 1;
}

static int uhm_slab_slot_live_state_mirror_valid(
    const uhm_slab_slot *slot,
    uint32_t *out_flags,
    uint32_t *out_requested) {
    return uhm_slab_slot_live_state_mirror_valid_with_slab(
        slot, uhm_slab_slot_owner(slot), out_flags, out_requested);
}

static int uhm_slab_slot_state_mirror_valid_for_remote(
    const uhm_slab_slot *slot) {
    uint64_t state_word;
    uint32_t flags;
    uint32_t requested;
    uint32_t flags_without_draining;

    if (!slot) {
        return 0;
    }
    if (uhm_slab_slot_state_mirror_matches(slot)) {
        return 1;
    }
    state_word = uhm_slab_slot_state_load(slot);
    flags = uhm_slab_slot_state_flags(state_word);
    requested = uhm_slab_slot_state_requested(state_word);
    flags_without_draining =
        flags & ~(UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                  UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT);
    if (requested != 0 ||
        (flags & (UHM_SLAB_SLOT_FLAG_FREE |
                  UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
            (UHM_SLAB_SLOT_FLAG_FREE |
             UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) {
        return 0;
    }
    if ((flags & (UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                  UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) != 0 &&
        slot->flags == flags_without_draining &&
        slot->requested_size == 0) {
        return 1;
    }
    return (flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0 &&
           slot->flags == 0 &&
           slot->requested_size != 0 &&
           uhm_slab_slot_owner_slot_size(slot) != 0 &&
           slot->requested_size <= uhm_slab_slot_owner_slot_size(slot);
}

static int uhm_slab_slot_state_valid_for_check(const uhm_slab_slot *slot) {
    uint64_t state_word;
    uint32_t flags;
    uint32_t requested;

    if (!slot) {
        return 0;
    }
    state_word = uhm_slab_slot_state_load(slot);
    flags = uhm_slab_slot_state_flags(state_word);
    if (flags == (UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_CTX_CACHE) &&
        uhm_slab_slot_ctx_cache_state_valid(slot, &requested)) {
        return 1;
    }
    if (flags == UHM_SLAB_SLOT_FLAG_LIVE_DEFERRED) {
        return uhm_slab_slot_live_state_mirror_valid(slot, &flags,
                                                     &requested);
    }
    if (uhm_slab_slot_state_mirror_valid_for_remote(slot)) {
        return 1;
    }
    return uhm_slab_slot_live_state_mirror_valid(slot, &flags, &requested);
}

#define UHM_REMOTE_COUNTER_ACTIVE_MASK UINT64_C(0xffffffff)
#define UHM_REMOTE_COUNTER_VERSION_ONE (UINT64_C(1) << 32)
#define UHM_REMOTE_COUNTER_END_DELTA \
    (UHM_REMOTE_COUNTER_VERSION_ONE - UINT64_C(1))

static void uhm_slab_remote_counter_update_begin(uhm_slab *slab) {
    if (slab) {
        (void)atomic_fetch_add_explicit(&slab->remote_counter_seq, 1u,
                                        UHM_ATOMIC_ACQ_REL);
    }
}

static void uhm_slab_remote_counter_update_end(uhm_slab *slab) {
    if (slab) {
        (void)atomic_fetch_add_explicit(&slab->remote_counter_seq,
                                        UHM_REMOTE_COUNTER_END_DELTA,
                                        UHM_ATOMIC_RELEASE);
    }
}

static int uhm_slab_remote_counter_update_active(const uhm_slab *slab) {
    uint64_t seq;

    if (!slab) {
        return 0;
    }
    seq = atomic_load_explicit(&slab->remote_counter_seq,
                               UHM_ATOMIC_ACQUIRE);
    return (seq & UHM_REMOTE_COUNTER_ACTIVE_MASK) != 0;
}

static int uhm_slab_remote_counters_stable_match(const uhm_slab *slab) {
    size_t attempt;

    if (!slab) {
        return 0;
    }
    for (attempt = 0; attempt < 4u; ++attempt) {
        uint64_t seq_before =
            atomic_load_explicit(&slab->remote_counter_seq,
                                 UHM_ATOMIC_ACQUIRE);
        size_t pending_mirror;
        size_t pending;
        size_t inflight_mirror;
        size_t inflight;
        uint64_t seq_after;

        if ((seq_before & UHM_REMOTE_COUNTER_ACTIVE_MASK) != 0) {
            continue;
        }
        pending_mirror = atomic_load_explicit(&slab->remote_free_slots,
                                              UHM_ATOMIC_ACQUIRE);
        pending = atomic_load_explicit(&slab->remote_free_pending,
                                       UHM_ATOMIC_ACQUIRE);
        inflight_mirror = atomic_load_explicit(
            &slab->remote_enqueue_inflight_count, UHM_ATOMIC_ACQUIRE);
        inflight = atomic_load_explicit(&slab->remote_enqueue_inflight,
                                        UHM_ATOMIC_ACQUIRE);
        seq_after = atomic_load_explicit(&slab->remote_counter_seq,
                                         UHM_ATOMIC_ACQUIRE);
        if (seq_before == seq_after &&
            (seq_after & UHM_REMOTE_COUNTER_ACTIVE_MASK) == 0) {
            return pending_mirror == pending &&
                   inflight_mirror == inflight;
        }
    }
    return 0;
}

static int uhm_slab_remote_counters_valid_for_check(uhm_heap *heap,
                                                    const uhm_slab *slab) {
    if (uhm_slab_remote_counters_stable_match(slab)) {
        return 1;
    }
    return heap &&
           uhm_heap_active_ops_load(heap) > 1u &&
           uhm_slab_remote_counter_update_active(slab);
}

static void uhm_slab_remote_pending_init(uhm_slab *slab, size_t value) {
    atomic_init(&slab->remote_counter_seq, 0u);
    atomic_init(&slab->remote_free_slots, value);
    atomic_init(&slab->remote_free_pending, value);
}

static size_t uhm_slab_remote_pending_mirror_load(const uhm_slab *slab) {
    if (!slab) {
        return 0;
    }
    return atomic_load_explicit(&slab->remote_free_slots,
                                UHM_ATOMIC_ACQUIRE);
}

static size_t uhm_slab_remote_pending_load(const uhm_slab *slab) {
    if (!slab) {
        return 0;
    }
    return atomic_load_explicit(&slab->remote_free_pending,
                                UHM_ATOMIC_ACQUIRE);
}

static void uhm_slab_remote_pending_increment(uhm_slab *slab) {
    uhm_slab_remote_counter_update_begin(slab);
    (void)atomic_fetch_add_explicit(&slab->remote_free_slots, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    (void)atomic_fetch_add_explicit(&slab->remote_free_pending, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    uhm_slab_remote_counter_update_end(slab);
}

static uhm_result uhm_slab_remote_pending_decrement(uhm_slab *slab) {
    size_t pending;

    if (!slab ||
        uhm_slab_remote_pending_mirror_load(slab) == 0) {
        return UHM_ERR_CORRUPTION;
    }
    pending = atomic_load_explicit(&slab->remote_free_pending,
                                   UHM_ATOMIC_ACQUIRE);
    if (pending == 0) {
        return UHM_ERR_CORRUPTION;
    }
    uhm_slab_remote_counter_update_begin(slab);
    (void)atomic_fetch_sub_explicit(&slab->remote_free_slots, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    (void)atomic_fetch_sub_explicit(&slab->remote_free_pending, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    uhm_slab_remote_counter_update_end(slab);
    return UHM_OK;
}

static void uhm_slab_remote_inflight_init(uhm_slab *slab, size_t value) {
    atomic_init(&slab->remote_enqueue_inflight_count, value);
    atomic_init(&slab->remote_enqueue_inflight, value);
}

static void uhm_slab_remote_drain_owner_init(uhm_slab *slab) {
    if (slab) {
        atomic_init(&slab->remote_drain_active, 0u);
        atomic_init(&slab->remote_drain_epoch, UINT64_C(0));
        atomic_init(&slab->remote_ready_queued, 0u);
        uhm_tagged_freelist_next_store(&slab->remote_ready_link, 0);
    }
}

static int uhm_slab_remote_drain_try_acquire(uhm_heap *heap,
                                             uhm_slab *slab) {
    uint32_t expected = 0;
    uint64_t epoch;

    if (!heap || !slab) {
        return 0;
    }
    uhm_metadata_registered_reader_update_begin(heap);
    if (!atomic_compare_exchange_strong_explicit(
            &slab->remote_drain_active, &expected,
            UHM_REMOTE_DRAIN_OWNER_TRANSIENT,
            UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
        uhm_metadata_registered_reader_update_end(heap);
        return 0;
    }
    if (!uhm_metadata_registered_reader_count_increment(heap)) {
        atomic_store_explicit(&slab->remote_drain_active, 0u,
                              UHM_ATOMIC_RELEASE);
        uhm_metadata_registered_reader_update_end(heap);
        return 0;
    }
    epoch = atomic_load_explicit(&heap->core.metadata_published_epoch,
                                 UHM_ATOMIC_ACQUIRE);
    if (epoch == 0) {
        epoch = heap->core.metadata_epoch;
    }
    atomic_store_explicit(&slab->remote_drain_epoch, epoch,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_drain_active,
                          UHM_REMOTE_DRAIN_OWNER_ACTIVE,
                          UHM_ATOMIC_RELEASE);
    uhm_metadata_registered_reader_update_end(heap);
    return 1;
}

static void uhm_slab_remote_drain_release(uhm_heap *heap, uhm_slab *slab) {
    if (slab) {
        if (heap) {
            uhm_metadata_registered_reader_update_begin(heap);
        }
        {
            uint32_t state = atomic_exchange_explicit(
                &slab->remote_drain_active, UHM_REMOTE_DRAIN_OWNER_TRANSIENT,
                UHM_ATOMIC_ACQ_REL);
            if (heap && state != 0) {
                (void)uhm_metadata_registered_reader_count_decrement(heap);
            }
        }
        atomic_store_explicit(&slab->remote_drain_epoch, UINT64_C(0),
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&slab->remote_drain_active, 0u,
                              UHM_ATOMIC_RELEASE);
        if (heap) {
            uhm_metadata_registered_reader_update_end(heap);
        }
    }
}

static uint32_t uhm_slab_remote_drain_state(const uhm_slab *slab) {
    return slab ? atomic_load_explicit(&slab->remote_drain_active,
                                       UHM_ATOMIC_ACQUIRE)
                : 0u;
}

static int uhm_slab_remote_drain_active(const uhm_slab *slab) {
    return uhm_slab_remote_drain_state(slab) != 0;
}

static uint64_t uhm_slab_remote_drain_epoch(const uhm_slab *slab) {
    return slab ? atomic_load_explicit(&slab->remote_drain_epoch,
                                       UHM_ATOMIC_ACQUIRE)
                : UINT64_C(0);
}

static size_t uhm_slab_remote_inflight_mirror_load(const uhm_slab *slab) {
    if (!slab) {
        return 0;
    }
    return atomic_load_explicit(&slab->remote_enqueue_inflight_count,
                                UHM_ATOMIC_ACQUIRE);
}

static size_t uhm_slab_remote_inflight_load(const uhm_slab *slab) {
    if (!slab) {
        return 0;
    }
    return atomic_load_explicit(&slab->remote_enqueue_inflight,
                                UHM_ATOMIC_ACQUIRE);
}

static void uhm_slab_remote_inflight_increment(uhm_slab *slab) {
    uhm_slab_remote_counter_update_begin(slab);
    (void)atomic_fetch_add_explicit(&slab->remote_enqueue_inflight_count, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    (void)atomic_fetch_add_explicit(&slab->remote_enqueue_inflight, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    uhm_slab_remote_counter_update_end(slab);
}

static uhm_result uhm_slab_remote_inflight_decrement(uhm_slab *slab) {
    size_t inflight;

    if (!slab ||
        uhm_slab_remote_inflight_mirror_load(slab) == 0) {
        return UHM_ERR_CORRUPTION;
    }
    inflight = atomic_load_explicit(&slab->remote_enqueue_inflight,
                                    UHM_ATOMIC_ACQUIRE);
    if (inflight == 0) {
        return UHM_ERR_CORRUPTION;
    }
    uhm_slab_remote_counter_update_begin(slab);
    (void)atomic_fetch_sub_explicit(&slab->remote_enqueue_inflight_count, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    (void)atomic_fetch_sub_explicit(&slab->remote_enqueue_inflight, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    uhm_slab_remote_counter_update_end(slab);
    return UHM_OK;
}

static uhm_result uhm_slab_remote_counters_decrement_batch(
    uhm_slab *slab,
    size_t pending_count,
    size_t inflight_count) {
    size_t pending;
    size_t pending_mirror;
    size_t inflight;
    size_t inflight_mirror;

    if (!slab) {
        return UHM_ERR_CORRUPTION;
    }
    if (pending_count == 0 && inflight_count == 0) {
        return UHM_OK;
    }
    pending_mirror = uhm_slab_remote_pending_mirror_load(slab);
    pending = uhm_slab_remote_pending_load(slab);
    inflight_mirror = uhm_slab_remote_inflight_mirror_load(slab);
    inflight = uhm_slab_remote_inflight_load(slab);
    if (pending_mirror < pending_count || pending < pending_count ||
        inflight_mirror < inflight_count || inflight < inflight_count) {
        return UHM_ERR_CORRUPTION;
    }
    uhm_slab_remote_counter_update_begin(slab);
    if (pending_count != 0) {
        (void)atomic_fetch_sub_explicit(&slab->remote_free_slots,
                                        pending_count, UHM_ATOMIC_ACQ_REL);
        (void)atomic_fetch_sub_explicit(&slab->remote_free_pending,
                                        pending_count, UHM_ATOMIC_ACQ_REL);
    }
    if (inflight_count != 0) {
        (void)atomic_fetch_sub_explicit(
            &slab->remote_enqueue_inflight_count, inflight_count,
            UHM_ATOMIC_ACQ_REL);
        (void)atomic_fetch_sub_explicit(&slab->remote_enqueue_inflight,
                                        inflight_count, UHM_ATOMIC_ACQ_REL);
    }
    uhm_slab_remote_counter_update_end(slab);
    return UHM_OK;
}

static void uhm_slab_rebuild_free_slots(uhm_slab *slab) {
    size_t i;

    uhm_slab_raw_lock(slab);
    slab->free_head = 0;
    slab->remote_free_head = 0;
    uhm_tagged_freelist_init(&slab->remote_free_lf);
    slab->free_slots = slab->total_slots;
    slab->live_slots = 0;
    slab->ctx_cached_slots = 0;
    uhm_slab_remote_pending_init(slab, 0u);
    uhm_slab_remote_inflight_init(slab, 0u);
    uhm_slab_remote_drain_owner_init(slab);
    for (i = 0; i < slab->total_slots; ++i) {
        uhm_slab_slot *slot =
            (uhm_slab_slot *)(slab->base + i * slab->slot_stride);
        memset(slot, 0, sizeof(*slot));
        slot->magic = UHM_SLAB_SLOT_MAGIC;
        slot->size_class = slab->size_class;
        uhm_slab_slot_state_init(slot, UHM_SLAB_SLOT_FLAG_FREE, 0u);
        slot->slab = slab;
        uhm_tagged_freelist_next_store(&slot->remote_link, 0);
        slot->next_free = slab->free_head;
        slab->free_head = slot;
    }
    uhm_slab_summary_publish_raw_locked(slab);
    uhm_slab_raw_unlock(slab);
}

static size_t uhm_slab_committed_empty_count(uhm_heap *heap,
                                             uint16_t size_class) {
    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    return heap->core.slab_committed_empty_count[size_class];
}

static size_t uhm_slab_empty_committed_cache_high_water(uhm_heap *heap,
                                                        uint16_t size_class) {
    size_t high_water = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;

    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return high_water;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.test_slab_empty_decommit_high_water_override != 0) {
        return heap->core.test_slab_empty_decommit_high_water_override;
    }
#endif
    high_water = uhm_slab_empty_committed_cache_state_high_water(
        heap->core.slab_empty_committed_cache_high_water[size_class]);
    return high_water != 0 ? high_water
                           : (size_t)UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
}

static void uhm_slab_empty_committed_cache_adaptive_grow(
    uhm_heap *heap,
    uint16_t size_class) {
    size_t old_high_water;
    size_t new_high_water;
    size_t state;

    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.test_slab_empty_decommit_high_water_override != 0) {
        return;
    }
#endif
    state = heap->core.slab_empty_committed_cache_high_water[size_class];
    old_high_water =
        uhm_slab_empty_committed_cache_state_high_water(state);
    if (old_high_water == 0) {
        old_high_water = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    }
    heap->core.slab_empty_committed_cache_high_water[size_class] =
        uhm_slab_empty_committed_cache_state_make(old_high_water, 0u);
    if (old_high_water >=
        UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER) {
        return;
    }
    if (old_high_water >
        UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER / 2u) {
        new_high_water =
            UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER;
    } else {
        new_high_water = old_high_water * 2u;
    }
    if (new_high_water >
        UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER) {
        new_high_water =
            UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER;
    }
    if (new_high_water > old_high_water) {
        heap->core.slab_empty_committed_cache_high_water[size_class] =
            uhm_slab_empty_committed_cache_state_make(new_high_water, 0u);
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                slab_empty_committed_cache_adaptive_grow_events));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(
                heap,
                slab_empty_committed_cache_adaptive_grow_bytes),
            (new_high_water - old_high_water) * heap->F.vm.page_size);
    }
}

static void uhm_slab_empty_committed_cache_maybe_shrink(
    uhm_heap *heap,
    uint16_t size_class) {
    uint8_t idle_drains;
    size_t high_water;
    size_t new_high_water;
    size_t state;

    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.test_slab_empty_decommit_high_water_override != 0) {
        state = heap->core.slab_empty_committed_cache_high_water[size_class];
        high_water = uhm_slab_empty_committed_cache_state_high_water(state);
        if (high_water == 0) {
            high_water = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
        }
        heap->core.slab_empty_committed_cache_high_water[size_class] =
            uhm_slab_empty_committed_cache_state_make(high_water, 0u);
        return;
    }
#endif
    state = heap->core.slab_empty_committed_cache_high_water[size_class];
    high_water = uhm_slab_empty_committed_cache_state_high_water(state);
    if (high_water == 0) {
        high_water = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    }
    if (high_water <= UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER ||
        uhm_slab_committed_empty_count(heap, size_class) >
            UHM_SLAB_EMPTY_COMMITTED_CACHE_LOW_WATER) {
        heap->core.slab_empty_committed_cache_high_water[size_class] =
            uhm_slab_empty_committed_cache_state_make(high_water, 0u);
        return;
    }

    idle_drains =
        uhm_slab_empty_committed_cache_state_idle_drains(state);
    if (idle_drains < UHM_SLAB_EMPTY_COMMITTED_CACHE_IDLE_SHRINK_DRAINS) {
        ++idle_drains;
    }
    heap->core.slab_empty_committed_cache_high_water[size_class] =
        uhm_slab_empty_committed_cache_state_make(high_water, idle_drains);
    if (idle_drains < UHM_SLAB_EMPTY_COMMITTED_CACHE_IDLE_SHRINK_DRAINS) {
        return;
    }

    new_high_water = high_water / 2u;
    if (new_high_water < UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER) {
        new_high_water = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    }
    heap->core.slab_empty_committed_cache_high_water[size_class] =
        uhm_slab_empty_committed_cache_state_make(new_high_water, 0u);
}

static int uhm_slab_empty_committed_cache_over_high_water(uhm_heap *heap,
                                                          uint16_t size_class) {
    size_t high_water =
        uhm_slab_empty_committed_cache_high_water(heap, size_class);
    return uhm_slab_committed_empty_count(heap, size_class) > high_water;
}

static int uhm_slab_empty_committed_cache_within_high_water(uhm_heap *heap,
                                                            uint16_t size_class) {
    size_t high_water =
        uhm_slab_empty_committed_cache_high_water(heap, size_class);
    return uhm_slab_committed_empty_count(heap, size_class) <= high_water;
}

static void uhm_slab_note_decommit_pressure(uhm_heap *heap,
                                            const uhm_slab *slab) {
    if (!heap || !slab) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, slab_empty_decommit_pressure_events));
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, slab_empty_decommit_pressure_bytes),
        slab->size);
}

static size_t uhm_slab_committed_empty_count_for_check(uhm_heap *heap,
                                                       uint16_t size_class) {
    uhm_slab *slab;
    size_t count = 0;

    for (slab = heap->core.slab_empty_heads[size_class]; slab;
         slab = slab->next_class) {
        if (uhm_slab_committed_empty_counted(slab)) {
            ++count;
        }
    }
    return count;
}

