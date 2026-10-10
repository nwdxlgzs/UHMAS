static uhm_slab_slot *uhm_slab_slot_from_remote_link(
    uhm_tagged_freelist_node *link) {
    uintptr_t link_addr = (uintptr_t)link;

    if (!link || link_addr < offsetof(uhm_slab_slot, remote_link)) {
        return 0;
    }
    return (uhm_slab_slot *)(link_addr - offsetof(uhm_slab_slot, remote_link));
}

static int uhm_metadata_range_is_live_control(uhm_heap *heap,
                                              uintptr_t addr,
                                              size_t size) {
    size_t first;
    size_t count;
    size_t i;

    if (!uhm_page_span(heap, addr, size, &first, &count)) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        size_t page = first + i;
        size_t shard = uhm_page_shard_for_index(page);
        uhm_page_record *record;
        int ok;

        uhm_lock(&heap->core.page_map_locks[shard].lock);
        record = uhm_page_record_get_unlocked(heap, page);
        ok = record &&
             record->owner == UHM_PAGE_OWNER_CONTROL &&
             record->va_state == UHM_PAGE_VA_CONTROL_LIVE &&
             record->commit_state == UHM_PAGE_COMMIT_LIVE;
        uhm_unlock(&heap->core.page_map_locks[shard].lock);
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

static uhm_slab *uhm_slab_from_remote_ready_link(
    uhm_heap *heap,
    uhm_tagged_freelist_node *link) {
    uintptr_t link_addr = (uintptr_t)link;
    uintptr_t slab_addr;

    if (!heap || !link || link_addr < offsetof(uhm_slab, remote_ready_link)) {
        return 0;
    }
    slab_addr = link_addr - offsetof(uhm_slab, remote_ready_link);
    if ((slab_addr % alignof(uhm_slab)) != 0 ||
        !uhm_metadata_range_is_live_control(heap, slab_addr, sizeof(uhm_slab))) {
        return 0;
    }
    return (uhm_slab *)slab_addr;
}

static int uhm_slab_remote_raw_ready_supported(const uhm_heap *heap) {
    return heap && UINTPTR_MAX == UINT32_MAX &&
           heap->core.low_lock_remote_free_enabled != 0u &&
           !uhm_tagged_freelist_is_supported();
}

#if UINTPTR_MAX == UINT32_MAX
static uhm_slab *uhm_slab_from_raw_ready_link(
    uhm_tagged_freelist_node *link) {
    uintptr_t link_addr = (uintptr_t)link;
    uintptr_t slab_addr;

    if (!link || link_addr < offsetof(uhm_slab, remote_ready_link)) {
        return 0;
    }
    slab_addr = link_addr - offsetof(uhm_slab, remote_ready_link);
    if ((slab_addr % alignof(uhm_slab)) != 0) {
        return 0;
    }
    return (uhm_slab *)slab_addr;
}

static uhm_slab *uhm_slab_raw_ready_next(const uhm_slab *slab) {
    uhm_tagged_freelist_node *link;

    if (!slab) {
        return 0;
    }
    link = uhm_tagged_freelist_next_load(&slab->remote_ready_link);
    return link ? uhm_slab_from_raw_ready_link(link) : 0;
}
#endif

/*
 * The raw directory is only used when the 32-bit low-lock capability is
 * enabled.  Its class shard serializes both producers and consumers, while
 * the metadata reader held by callers keeps the slab alive across raw-lock
 * work and possible concurrent close/decommit attempts.
 */
static int uhm_slab_remote_raw_ready_note(uhm_heap *heap,
                                          uhm_slab *slab) {
#if UINTPTR_MAX == UINT32_MAX
    uint16_t size_class;
    uhm_slab *head;

    if (!uhm_slab_remote_raw_ready_supported(heap) || !slab ||
        slab->size_class >= UHM_SIZE_CLASS_COUNT ||
        slab->magic != UHM_SLAB_MAGIC) {
        return 0;
    }
    size_class = slab->size_class;
    uhm_lock(&heap->core.slab_locks[size_class].lock);
    if (atomic_load_explicit(&slab->remote_ready_queued,
                             UHM_ATOMIC_ACQUIRE) ==
        UHM_SLAB_REMOTE_READY_QUEUED) {
        uhm_unlock(&heap->core.slab_locks[size_class].lock);
        return 1;
    }
    if (atomic_load_explicit(&slab->remote_ready_queued,
                             UHM_ATOMIC_ACQUIRE) !=
        UHM_SLAB_REMOTE_READY_IDLE) {
        uhm_unlock(&heap->core.slab_locks[size_class].lock);
        return 0;
    }
    head = atomic_load_explicit(
        &heap->core.slab_remote_ready_raw[size_class].value,
        UHM_ATOMIC_ACQUIRE);
    uhm_slab_remote_ready_update_begin(heap, size_class);
    uhm_tagged_freelist_next_store(
        &slab->remote_ready_link,
        head ? &head->remote_ready_link : 0);
    atomic_store_explicit(
        &heap->core.slab_remote_ready_raw[size_class].value, slab,
        UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&slab->remote_ready_queued,
                          UHM_SLAB_REMOTE_READY_QUEUED,
                          UHM_ATOMIC_RELEASE);
    uhm_slab_remote_ready_update_end(heap, size_class);
    uhm_unlock(&heap->core.slab_locks[size_class].lock);
    return 1;
#else
    (void)heap;
    (void)slab;
    return 0;
#endif
}

#if UINTPTR_MAX == UINT32_MAX
static int uhm_slab_raw_ready_decommitted_valid(const uhm_slab *slab) {
    return slab->state == UHM_SLAB_STATE_EMPTY &&
           slab->live_slots == 0u &&
           slab->free_slots == slab->total_slots &&
           atomic_load_explicit(&slab->ctx_cached_slots,
                                UHM_ATOMIC_ACQUIRE) == 0u &&
           uhm_slab_remote_pending_load(slab) == 0u &&
           uhm_slab_remote_inflight_load(slab) == 0u &&
           uhm_slab_remote_head_empty_snapshot(slab) &&
           atomic_load_explicit(&slab->remote_free_lf.head,
                                UHM_ATOMIC_ACQUIRE) == 0u &&
           uhm_extent_descriptor_matches(
               &slab->desc, UHM_EXTENT_KIND_MALLOC, slab->base,
               slab->size, UHM_PAGE_OWNER_MALLOC,
               UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_DECOMMITTED,
               UHM_PROT_NONE, slab->generation, slab);
}

static uhm_slab *uhm_slab_remote_raw_ready_take(
    uhm_heap *heap,
    uint16_t size_class,
    uhm_metadata_reader_state *reader,
    uhm_result *out_result) {
    uhm_slab *slab;
    uhm_slab *next;

    if (out_result) {
        *out_result = UHM_OK;
    }
    if (!uhm_slab_remote_raw_ready_supported(heap) ||
        size_class >= UHM_SIZE_CLASS_COUNT || !reader) {
        return 0;
    }
    uhm_lock(&heap->core.slab_locks[size_class].lock);
    slab = atomic_load_explicit(
        &heap->core.slab_remote_ready_raw[size_class].value,
        UHM_ATOMIC_ACQUIRE);
    if (!slab) {
        uhm_unlock(&heap->core.slab_locks[size_class].lock);
        return 0;
    }
    if (!uhm_metadata_reader_hazard_set(reader, 1u, slab)) {
        uhm_unlock(&heap->core.slab_locks[size_class].lock);
        if (out_result) {
            *out_result = UHM_ERR_CORRUPTION;
        }
        return 0;
    }
    next = uhm_slab_raw_ready_next(slab);
    if (next && next->size_class != size_class) {
        uhm_unlock(&heap->core.slab_locks[size_class].lock);
        if (out_result) {
            *out_result = UHM_ERR_CORRUPTION;
        }
        return 0;
    }
    atomic_store_explicit(
        &heap->core.slab_remote_ready_raw[size_class].value, next,
        UHM_ATOMIC_RELEASE);
    uhm_tagged_freelist_next_store(&slab->remote_ready_link, 0);
    uhm_slab_remote_ready_update_begin(heap, size_class);
    atomic_store_explicit(&slab->remote_ready_queued,
                          UHM_SLAB_REMOTE_READY_IDLE,
                          UHM_ATOMIC_RELEASE);
    uhm_slab_remote_ready_update_end(heap, size_class);
    uhm_unlock(&heap->core.slab_locks[size_class].lock);
    return slab;
}
#endif

static uhm_result uhm_slab_remote_raw_ready_clear_all(uhm_heap *heap) {
#if UINTPTR_MAX == UINT32_MAX
    uint16_t size_class;

    if (!uhm_slab_remote_raw_ready_supported(heap)) {
        return UHM_OK;
    }
    for (size_class = 0; size_class < UHM_SIZE_CLASS_COUNT; ++size_class) {
        uhm_slab *slab;

        uhm_lock(&heap->core.slab_locks[size_class].lock);
        slab = atomic_load_explicit(
            &heap->core.slab_remote_ready_raw[size_class].value,
            UHM_ATOMIC_ACQUIRE);
        atomic_store_explicit(
            &heap->core.slab_remote_ready_raw[size_class].value, 0,
            UHM_ATOMIC_RELEASE);
        while (slab) {
            uhm_slab *next;

            if (slab->magic != UHM_SLAB_MAGIC ||
                slab->size_class != size_class) {
                uhm_unlock(&heap->core.slab_locks[size_class].lock);
                return UHM_ERR_CORRUPTION;
            }
            uhm_slab_raw_lock(slab);
            if (slab->remote_free_head != 0 ||
                uhm_slab_remote_pending_load(slab) != 0) {
                uhm_slab_raw_unlock(slab);
                uhm_unlock(&heap->core.slab_locks[size_class].lock);
                return UHM_ERR_CORRUPTION;
            }
            next = uhm_slab_raw_ready_next(slab);
            uhm_slab_raw_unlock(slab);
            atomic_store_explicit(&slab->remote_ready_queued,
                                  UHM_SLAB_REMOTE_READY_IDLE,
                                  UHM_ATOMIC_RELEASE);
            uhm_tagged_freelist_next_store(&slab->remote_ready_link, 0);
            slab = next;
        }
        uhm_unlock(&heap->core.slab_locks[size_class].lock);
    }
#else
    (void)heap;
#endif
    return UHM_OK;
}

static int uhm_slab_remote_raw_ready_validate_locked(
    uhm_heap *heap,
    uint16_t size_class,
    const uhm_slab *target,
    size_t limit,
    size_t *out_count) {
#if UINTPTR_MAX == UINT32_MAX
    uhm_slab *slab;
    size_t count = 0;
    size_t seen = 0;
#else
    (void)target;
    (void)limit;
#endif

    if (out_count) {
        *out_count = 0;
    }
    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    if (!uhm_slab_remote_raw_ready_supported(heap)) {
        return 1;
    }
#if UINTPTR_MAX == UINT32_MAX
    uhm_lock(&heap->core.slab_locks[size_class].lock);
    for (slab = atomic_load_explicit(
             &heap->core.slab_remote_ready_raw[size_class].value,
             UHM_ATOMIC_ACQUIRE); slab; slab = uhm_slab_raw_ready_next(slab)) {
        if (++seen > limit || slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class ||
            atomic_load_explicit(&slab->remote_ready_queued,
                                 UHM_ATOMIC_ACQUIRE) !=
                UHM_SLAB_REMOTE_READY_QUEUED) {
            uhm_unlock(&heap->core.slab_locks[size_class].lock);
            return 0;
        }
        if (slab == target) {
            if (count != 0u) {
                uhm_unlock(&heap->core.slab_locks[size_class].lock);
                return 0;
            }
            count = 1u;
        }
    }
    uhm_unlock(&heap->core.slab_locks[size_class].lock);
    if (out_count) {
        *out_count = count;
    }
#endif
    return 1;
}

static int uhm_slab_remote_lf_empty(const uhm_slab *slab) {
    uintptr_t head;

    if (!slab) {
        return 1;
    }
    head = atomic_load_explicit(&slab->remote_free_lf.head,
                                UHM_ATOMIC_ACQUIRE);
    return uhm_tagged_freelist_unpack_ptr(head) == 0;
}

static uhm_result uhm_slab_remote_slot_validate(uhm_slab *slab,
                                                uhm_slab_slot *slot) {
    uint32_t flags;

    if (!slot || !uhm_slab_slot_state_mirror_valid_for_remote(slot)) {
        return UHM_ERR_CORRUPTION;
    }
    flags = uhm_slab_slot_flags_load(slot);
    if (!slot || slot->magic != UHM_SLAB_SLOT_MAGIC ||
        !uhm_slab_slot_belongs_to_remote_batch(slot, slab) ||
        (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                   UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                   UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                   UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED |
                   UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                   UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) != 0 ||
        (flags & (UHM_SLAB_SLOT_FLAG_FREE |
                  UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
            (UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) ||
        uhm_slab_slot_requested_load(slot) != 0) {
        return UHM_ERR_CORRUPTION;
    }
    return UHM_OK;
}

static uhm_result uhm_slab_remote_publish_tagged_batch_locked(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_tagged_freelist_node *link,
    int *out_publish_started) {
    uhm_tagged_freelist_node *scan;
    uhm_tagged_freelist_node *publish;
    size_t node_count = 0;
    size_t unaccounted_count = 0;
    size_t pending_decrements = 0;
    size_t inflight_decrements = 0;
    size_t published = 0;
    uhm_result result = UHM_OK;

    if (out_publish_started) {
        *out_publish_started = 0;
    }
    if (!heap || !slab) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    for (scan = link; scan; scan = uhm_tagged_freelist_next_load(scan)) {
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(scan);
        uint32_t flags;

        result = uhm_slab_remote_slot_validate(slab, slot);
        if (result != UHM_OK) {
            return result;
        }
        flags = uhm_slab_slot_flags_load(slot);
        ++node_count;
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0) {
            ++unaccounted_count;
        }
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) == 0) {
            ++pending_decrements;
            if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) != 0) {
                ++inflight_decrements;
            }
        }
    }
    if (unaccounted_count >
        atomic_load_explicit(&slab->live_slots_count, UHM_ATOMIC_ACQUIRE)) {
        return UHM_ERR_CORRUPTION;
    }

    if (out_publish_started) {
        *out_publish_started = 1;
    }
    uhm_slab_raw_lock(slab);
    uhm_slab_raw_publish_begin(slab);
    publish = link;
    while (publish) {
        uhm_tagged_freelist_node *next =
            uhm_tagged_freelist_next_load(publish);
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(publish);
        uint32_t flags = uhm_slab_slot_flags_load(slot);
        int had_unaccounted =
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0;

        if ((flags & (UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
                (UHM_SLAB_SLOT_FLAG_FREE |
                 UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) ||
            (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                       UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED |
                       UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                       UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) != 0 ||
            uhm_slab_slot_requested_load(slot) != 0 ||
            !uhm_slab_slot_state_cas(
                slot, flags, 0u, UHM_SLAB_SLOT_FLAG_FREE, 0u)) {
            result = UHM_ERR_CORRUPTION;
            break;
        }
        uhm_tagged_freelist_next_store(&slot->remote_link, 0);
        slot->next_free = slab->free_head;
        slab->free_head = slot;
        if (had_unaccounted) {
            --slab->live_slots;
            ++slab->free_slots;
        }
        ++published;
        publish = next;
    }
    if (result == UHM_OK) {
        if (unaccounted_count != 0) {
            uhm_slab_refresh_state_raw_locked(heap, slab);
        } else {
            uhm_slab_summary_publish_raw_locked(slab);
        }
    }
    uhm_slab_raw_publish_end(slab);
    uhm_slab_raw_unlock(slab);

    if (result != UHM_OK || published != node_count) {
        return result != UHM_OK ? result : UHM_ERR_CORRUPTION;
    }
    return uhm_slab_remote_counters_decrement_batch(
        slab, pending_decrements, inflight_decrements);
}

static uhm_result uhm_slab_remote_publish_list_batch_locked(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_slab_slot *head) {
    uhm_slab_slot *scan;
    uhm_slab_slot *publish;
    size_t node_count = 0;
    size_t unaccounted_count = 0;
    size_t pending_decrements = 0;
    size_t inflight_decrements = 0;
    size_t published = 0;
    uhm_result result = UHM_OK;

    if (!heap || !slab) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    for (scan = head; scan; scan = scan->next_free) {
        uint32_t flags;

        result = uhm_slab_remote_slot_validate(slab, scan);
        if (result != UHM_OK) {
            return result;
        }
        flags = uhm_slab_slot_flags_load(scan);
        ++node_count;
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0) {
            ++unaccounted_count;
        }
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) == 0) {
            ++pending_decrements;
            if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) != 0) {
                ++inflight_decrements;
            }
        }
    }
    if (unaccounted_count >
        atomic_load_explicit(&slab->live_slots_count, UHM_ATOMIC_ACQUIRE)) {
        return UHM_ERR_CORRUPTION;
    }

    uhm_slab_raw_lock(slab);
    uhm_slab_raw_publish_begin(slab);
    publish = head;
    while (publish) {
        uhm_slab_slot *next = publish->next_free;
        uint32_t flags = uhm_slab_slot_flags_load(publish);
        int had_unaccounted =
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0;

        if ((flags & (UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE)) !=
                (UHM_SLAB_SLOT_FLAG_FREE |
                 UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE) ||
            (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_QUEUE |
                       UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING |
                       UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED |
                       UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                       UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) != 0 ||
            uhm_slab_slot_requested_load(publish) != 0 ||
            !uhm_slab_slot_state_cas(
                publish, flags, 0u, UHM_SLAB_SLOT_FLAG_FREE, 0u)) {
            result = UHM_ERR_CORRUPTION;
            break;
        }
        uhm_tagged_freelist_next_store(&publish->remote_link, 0);
        publish->next_free = slab->free_head;
        slab->free_head = publish;
        if (had_unaccounted) {
            --slab->live_slots;
            ++slab->free_slots;
        }
        ++published;
        publish = next;
    }
    if (result == UHM_OK) {
        if (unaccounted_count != 0) {
            uhm_slab_refresh_state_raw_locked(heap, slab);
        } else {
            uhm_slab_summary_publish_raw_locked(slab);
        }
    }
    uhm_slab_raw_publish_end(slab);
    uhm_slab_raw_unlock(slab);

    if (result != UHM_OK || published != node_count) {
        return result != UHM_OK ? result : UHM_ERR_CORRUPTION;
    }
    return uhm_slab_remote_counters_decrement_batch(
        slab, pending_decrements, inflight_decrements);
}

static uhm_result uhm_slab_drain_remote(uhm_heap *heap, uhm_slab *slab);
static uhm_result
uhm_slab_drain_remote_reserved_no_decommit_with_reader_lockless(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_metadata_reader_state *reader,
    size_t self_readers);
static uhm_result uhm_slab_drain_remote_reserved_with_reader_lockless(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_metadata_reader_state *reader,
    size_t self_readers);
static uhm_result uhm_slab_drain_remote_reserved_lockless(
    uhm_heap *heap,
    uhm_slab *slab,
    int needs_decommit_check);
static uhm_result uhm_slab_maybe_decommit_empty(uhm_heap *heap,
                                                uhm_slab *slab);
static int uhm_slab_descriptor_live_for_lockless(uhm_heap *heap,
                                                 const uhm_slab *slab);

static int uhm_heap_tagged_freelist_wrap_allowed(
    uhm_heap *heap,
    size_t self_readers) {
    size_t readers = uhm_metadata_registered_readers_load(heap);

    if (heap && uhm_metadata_hazard_readers_load(heap) != 0) {
        return 0;
    }
    return readers <= self_readers;
}

static int uhm_heap_tagged_freelist_wrap_tag(
    uhm_heap *heap,
    uhm_tagged_freelist *list,
    size_t self_readers) {
    uintptr_t old_head;
    uintptr_t new_head;
    uint32_t tag;

    if (!list) {
        return 0;
    }
    while (atomic_flag_test_and_set_explicit(&list->overflow_lock,
                                             UHM_ATOMIC_ACQUIRE)) {
    }
    old_head = atomic_load_explicit(&list->head, UHM_ATOMIC_ACQUIRE);
    tag = uhm_tagged_freelist_unpack_tag(old_head);
    if (tag == UHM_TAGGED_FREELIST_TAG_MASK) {
        if (!uhm_heap_tagged_freelist_wrap_allowed(heap, self_readers)) {
            (void)atomic_fetch_add_explicit(&list->tag_wrap_deferrals,
                                            UINT64_C(1), UHM_ATOMIC_RELAXED);
            atomic_flag_clear_explicit(&list->overflow_lock,
                                       UHM_ATOMIC_RELEASE);
            return 0;
        }
        new_head = uhm_tagged_freelist_pack(
            uhm_tagged_freelist_unpack_ptr(old_head), 0u);
        if (atomic_compare_exchange_strong_explicit(
                &list->head, &old_head, new_head, UHM_ATOMIC_ACQ_REL,
                UHM_ATOMIC_ACQUIRE)) {
            (void)atomic_fetch_add_explicit(&list->tag_overflows,
                                            UINT64_C(1), UHM_ATOMIC_RELAXED);
            atomic_store_explicit(
                &list->last_wrap_epoch,
                heap ? atomic_load_explicit(
                           &heap->core.metadata_published_epoch,
                           UHM_ATOMIC_ACQUIRE)
                     : UINT64_C(0),
                UHM_ATOMIC_RELEASE);
        }
    }
    atomic_flag_clear_explicit(&list->overflow_lock, UHM_ATOMIC_RELEASE);
    return 1;
}

static int uhm_heap_tagged_freelist_push_ex(
    uhm_heap *heap,
    uhm_tagged_freelist *list,
    uhm_tagged_freelist_node *node,
    size_t self_readers,
    int *out_wrap_deferred) {
    uintptr_t old_head;
    uintptr_t new_head;
    uint32_t tag;

    if (out_wrap_deferred) {
        *out_wrap_deferred = 0;
    }
    if (!list || !node || !uhm_tagged_freelist_is_supported() ||
        !uhm_tagged_freelist_ptr_packable(node)) {
        return 0;
    }
    for (;;) {
        old_head = atomic_load_explicit(&list->head, UHM_ATOMIC_ACQUIRE);
        tag = uhm_tagged_freelist_unpack_tag(old_head);
        if (tag == UHM_TAGGED_FREELIST_TAG_MASK) {
            if (!uhm_heap_tagged_freelist_wrap_tag(heap, list,
                                                   self_readers)) {
                if (out_wrap_deferred) {
                    *out_wrap_deferred = 1;
                }
                return 0;
            }
            continue;
        }
        uhm_tagged_freelist_next_store(
            node, uhm_tagged_freelist_unpack_ptr(old_head));
        new_head = uhm_tagged_freelist_pack(node, tag + 1u);
        if (atomic_compare_exchange_weak_explicit(
                &list->head, &old_head, new_head, UHM_ATOMIC_ACQ_REL,
                UHM_ATOMIC_ACQUIRE)) {
            return 1;
        }
        (void)atomic_fetch_add_explicit(&list->cas_failures, UINT64_C(1),
                                        UHM_ATOMIC_RELAXED);
    }
}

static int uhm_heap_tagged_freelist_push(
    uhm_heap *heap,
    uhm_tagged_freelist *list,
    uhm_tagged_freelist_node *node,
    size_t self_readers) {
    return uhm_heap_tagged_freelist_push_ex(
        heap, list, node, self_readers, 0);
}

static int uhm_slab_remote_ready_note_in_window(uhm_heap *heap,
                                                uhm_slab *slab,
                                                size_t self_readers) {
    uint32_t expected = UHM_SLAB_REMOTE_READY_IDLE;

    if (!heap || !slab || slab->size_class >= UHM_SIZE_CLASS_COUNT ||
        !uhm_tagged_freelist_is_supported() ||
        !uhm_tagged_freelist_ptr_packable(&slab->remote_ready_link)) {
        return 0;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &slab->remote_ready_queued, &expected,
            UHM_SLAB_REMOTE_READY_QUEUED, UHM_ATOMIC_ACQ_REL,
            UHM_ATOMIC_ACQUIRE)) {
        if (atomic_load_explicit(&slab->remote_ready_queued,
                                 UHM_ATOMIC_ACQUIRE) ==
            UHM_SLAB_REMOTE_READY_IDLE) {
            uhm_slab_remote_pending_note(heap, slab->size_class);
        }
        return 0;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.fault_remote_ready_note_countdown != 0 &&
        --heap->core.fault_remote_ready_note_countdown == 0) {
        atomic_store_explicit(&slab->remote_ready_queued,
                              UHM_SLAB_REMOTE_READY_IDLE,
                              UHM_ATOMIC_RELEASE);
        uhm_slab_remote_pending_note(heap, slab->size_class);
        return 0;
    }
#endif
    uhm_tagged_freelist_next_store(&slab->remote_ready_link, 0);
    if (!uhm_heap_tagged_freelist_push(
            heap, &heap->core.slab_remote_ready_lf[slab->size_class].queue,
            &slab->remote_ready_link, self_readers)) {
        atomic_store_explicit(&slab->remote_ready_queued,
                              UHM_SLAB_REMOTE_READY_IDLE,
                              UHM_ATOMIC_RELEASE);
        uhm_slab_remote_pending_note(heap, slab->size_class);
        return 0;
    }
    /*
     * The class-ready queue is only a hint when the lockless remote-free
     * path is disabled. A bounded class scan remains the recovery path for
     * a concurrent producer that publishes after the ready node is taken.
     */
    if ((heap->state_flags & UHM_CONFIG_LOCKLESS_REMOTE_FREE) == 0u) {
        uhm_slab_remote_pending_note(heap, slab->size_class);
    }
    return 1;
}

static void uhm_slab_remote_ready_note(uhm_heap *heap,
                                       uhm_slab *slab,
                                       size_t self_readers) {
    if (!heap || !slab || slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return;
    }
    uhm_slab_remote_ready_update_begin(heap, slab->size_class);
    (void)uhm_slab_remote_ready_note_in_window(heap, slab, self_readers);
    uhm_slab_remote_ready_update_end(heap, slab->size_class);
}

static int uhm_slab_remote_ready_clear_scanning_in_window(uhm_heap *heap,
                                                          uhm_slab *slab) {
    uint32_t expected = UHM_SLAB_REMOTE_READY_SCANNING;

    if (!slab) {
        return 0;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap && heap->core.fault_remote_ready_clear_countdown != 0 &&
        --heap->core.fault_remote_ready_clear_countdown == 0) {
        return 0;
    }
#else
    (void)heap;
#endif
    return atomic_compare_exchange_strong_explicit(
        &slab->remote_ready_queued, &expected,
        UHM_SLAB_REMOTE_READY_IDLE, UHM_ATOMIC_ACQ_REL,
        UHM_ATOMIC_ACQUIRE);
}

static int uhm_slab_remote_ready_clear_scanning(uhm_heap *heap,
                                               uhm_slab *slab) {
    int cleared;

    if (!heap || !slab || slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    uhm_slab_remote_ready_update_begin(heap, slab->size_class);
    cleared = uhm_slab_remote_ready_clear_scanning_in_window(heap, slab);
    uhm_slab_remote_ready_update_end(heap, slab->size_class);
    return cleared;
}

static int uhm_slab_remote_ready_requeue_scanning(uhm_heap *heap,
                                                 uhm_slab *slab,
                                                 size_t self_readers) {
    if (!heap || !slab || slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    uhm_slab_remote_ready_update_begin(heap, slab->size_class);
    if (!uhm_slab_remote_ready_clear_scanning_in_window(heap, slab)) {
        uhm_slab_remote_ready_update_end(heap, slab->size_class);
        return 0;
    }
    (void)uhm_slab_remote_ready_note_in_window(heap, slab, self_readers);
    uhm_slab_remote_ready_update_end(heap, slab->size_class);
    return 1;
}

static int uhm_slab_remote_ready_finish_scanning(uhm_heap *heap,
                                                uhm_slab *slab,
                                                size_t self_readers) {
    int finished = 0;

    if (!heap || !slab || slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    uhm_slab_remote_ready_update_begin(heap, slab->size_class);
    if (!uhm_slab_remote_ready_clear_scanning_in_window(heap, slab)) {
        uhm_slab_remote_ready_update_end(heap, slab->size_class);
        return 0;
    }
    if (!uhm_slab_remote_lf_empty(slab)) {
        if (!uhm_slab_remote_ready_note_in_window(heap, slab, self_readers) &&
            atomic_load_explicit(&slab->remote_ready_queued,
                                 UHM_ATOMIC_ACQUIRE) !=
                UHM_SLAB_REMOTE_READY_QUEUED) {
            uhm_slab_remote_ready_update_end(heap, slab->size_class);
            return 0;
        }
    }
    finished = 1;
    uhm_slab_remote_ready_update_end(heap, slab->size_class);
    return finished;
}

static int uhm_slab_remote_ready_finish_after_live(uhm_heap *heap,
                                                  uhm_slab *slab,
                                                  uhm_metadata_reader_state *reader,
                                                  size_t self_readers) {
    uhm_result result = UHM_OK;

    if (uhm_slab_remote_ready_finish_scanning(heap, slab, self_readers)) {
        return 1;
    }
    if (!heap || !slab || slab->size_class >= UHM_SIZE_CLASS_COUNT ||
        atomic_load_explicit(&slab->remote_ready_queued,
                             UHM_ATOMIC_ACQUIRE) ==
            UHM_SLAB_REMOTE_READY_SCANNING) {
        return 0;
    }

    result =
        uhm_slab_drain_remote_reserved_no_decommit_with_reader_lockless(
            heap, slab, reader, self_readers);
    if (result != UHM_OK) {
        return 0;
    }
    return atomic_load_explicit(&slab->remote_ready_queued,
                                UHM_ATOMIC_ACQUIRE) !=
               UHM_SLAB_REMOTE_READY_SCANNING &&
           uhm_slab_remote_head_empty_snapshot(slab) &&
           uhm_slab_remote_lf_empty(slab);
}

static int uhm_slab_remote_ready_mark_scanning(uhm_slab *slab) {
    uint32_t expected = UHM_SLAB_REMOTE_READY_QUEUED;

    return slab &&
           atomic_compare_exchange_strong_explicit(
               &slab->remote_ready_queued, &expected,
               UHM_SLAB_REMOTE_READY_SCANNING, UHM_ATOMIC_ACQ_REL,
               UHM_ATOMIC_ACQUIRE);
}

static uhm_tagged_freelist_node *uhm_heap_tagged_freelist_pop(
    uhm_heap *heap,
    uhm_tagged_freelist *list,
    size_t self_readers,
    int *wrap_deferred) {
    uintptr_t old_head;
    uintptr_t new_head;
    uint32_t tag;
    uhm_tagged_freelist_node *node;
    uhm_tagged_freelist_node *next;

    if (wrap_deferred) {
        *wrap_deferred = 0;
    }
    if (!list || !uhm_tagged_freelist_is_supported()) {
        return 0;
    }
    for (;;) {
        old_head = atomic_load_explicit(&list->head, UHM_ATOMIC_ACQUIRE);
        node = uhm_tagged_freelist_unpack_ptr(old_head);
        if (!node) {
            return 0;
        }
        tag = uhm_tagged_freelist_unpack_tag(old_head);
        if (tag == UHM_TAGGED_FREELIST_TAG_MASK) {
            if (!uhm_heap_tagged_freelist_wrap_tag(heap, list,
                                                   self_readers)) {
                if (wrap_deferred) {
                    *wrap_deferred = 1;
                }
                return 0;
            }
            continue;
        }
        next = uhm_tagged_freelist_next_load(node);
        new_head = uhm_tagged_freelist_pack(next, tag + 1u);
        if (atomic_compare_exchange_weak_explicit(
                &list->head, &old_head, new_head, UHM_ATOMIC_ACQ_REL,
                UHM_ATOMIC_ACQUIRE)) {
            uhm_tagged_freelist_next_store(node, 0);
            return node;
        }
        (void)atomic_fetch_add_explicit(&list->cas_failures, UINT64_C(1),
                                        UHM_ATOMIC_RELAXED);
    }
}

static uhm_tagged_freelist_node *uhm_heap_tagged_freelist_take_all(
    uhm_heap *heap,
    uhm_tagged_freelist *list,
    size_t self_readers,
    int *wrap_deferred) {
    uintptr_t old_head;
    uintptr_t new_head;
    uint32_t tag;
    uhm_tagged_freelist_node *node;

    if (wrap_deferred) {
        *wrap_deferred = 0;
    }
    if (!list || !uhm_tagged_freelist_is_supported()) {
        return 0;
    }
    for (;;) {
        old_head = atomic_load_explicit(&list->head, UHM_ATOMIC_ACQUIRE);
        node = uhm_tagged_freelist_unpack_ptr(old_head);
        if (!node) {
            return 0;
        }
        tag = uhm_tagged_freelist_unpack_tag(old_head);
        if (tag == UHM_TAGGED_FREELIST_TAG_MASK) {
            if (!uhm_heap_tagged_freelist_wrap_tag(heap, list,
                                                   self_readers)) {
                if (wrap_deferred) {
                    *wrap_deferred = 1;
                }
                return 0;
            }
            continue;
        }
        new_head = uhm_tagged_freelist_pack(0, tag + 1u);
        if (atomic_compare_exchange_weak_explicit(
                &list->head, &old_head, new_head, UHM_ATOMIC_ACQ_REL,
                UHM_ATOMIC_ACQUIRE)) {
            return node;
        }
        (void)atomic_fetch_add_explicit(&list->cas_failures, UINT64_C(1),
                                        UHM_ATOMIC_RELAXED);
    }
}

static int uhm_slab_remote_restore_tagged_batch(
    uhm_heap *heap,
    uhm_tagged_freelist *queue,
    size_t self_readers,
    uhm_tagged_freelist_node *link) {
    while (link) {
        uhm_tagged_freelist_node *next =
            uhm_tagged_freelist_next_load(link);
        uhm_tagged_freelist_next_store(link, 0);
        if (!uhm_heap_tagged_freelist_push(heap, queue, link,
                                           self_readers)) {
            return 0;
        }
        link = next;
    }
    return 1;
}

static int uhm_slab_remote_clear_draining_batch(
    uhm_slab *slab,
    uhm_tagged_freelist_node *link) {
    int ok = 1;

    while (link) {
        uhm_tagged_freelist_node *next =
            uhm_tagged_freelist_next_load(link);
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(link);
        uint32_t flags;

        if (!slot || slot->magic != UHM_SLAB_SLOT_MAGIC ||
            !uhm_slab_slot_belongs_to_remote_batch(slot, slab)) {
            ok = 0;
            link = next;
            continue;
        }
        flags = uhm_slab_slot_flags_load(slot);
        if ((flags & (UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                      UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT)) != 0) {
            int had_accounted_out =
                (flags & UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) != 0;
            int had_publishing =
                (flags & UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) != 0;
            if (!uhm_slab_slot_state_cas_atomic_only(
                    slot, flags, 0u,
                    flags & ~(UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING |
                              UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT),
                    0u)) {
                ok = 0;
            } else if (had_accounted_out) {
                uhm_slab_remote_pending_increment(slab);
                if (had_publishing) {
                    uhm_slab_remote_inflight_increment(slab);
                }
            }
        }
        link = next;
    }
    return ok;
}

static int uhm_slab_remote_restore_reserved_batch(
    uhm_heap *heap,
    uhm_tagged_freelist *queue,
    uhm_slab *slab,
    size_t self_readers,
    uhm_tagged_freelist_node *link) {
    int ok;

    ok = uhm_slab_remote_clear_draining_batch(slab, link);
    if (!uhm_slab_remote_restore_tagged_batch(heap, queue, self_readers,
                                              link)) {
        ok = 0;
    }
    return ok;
}

static uhm_result uhm_slab_remote_mark_reserved_batch_lockless(
    uhm_slab *slab,
    uhm_tagged_freelist_node *link) {
    while (link) {
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(link);
        uint32_t flags;
        uhm_result result;

        result = uhm_slab_remote_slot_validate(slab, slot);
        if (result != UHM_OK) {
            return result;
        }
        flags = uhm_slab_slot_flags_load(slot);
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) != 0 ||
            !uhm_slab_slot_state_cas_atomic_only(
                slot, flags, 0u,
                flags | UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING, 0u)) {
            return UHM_ERR_CORRUPTION;
        }
        link = uhm_tagged_freelist_next_load(link);
    }
    return UHM_OK;
}

static int uhm_slab_remote_clear_accounted_batch_no_count(
    uhm_slab *slab,
    uhm_tagged_freelist_node *link) {
    int ok = 1;

    while (link) {
        uhm_tagged_freelist_node *next =
            uhm_tagged_freelist_next_load(link);
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(link);
        uint32_t flags;

        if (!slot || slot->magic != UHM_SLAB_SLOT_MAGIC ||
            !uhm_slab_slot_belongs_to_remote_batch(slot, slab)) {
            ok = 0;
            link = next;
            continue;
        }
        flags = uhm_slab_slot_flags_load(slot);
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) != 0 &&
            !uhm_slab_slot_state_cas_atomic_only(
                slot, flags, 0u,
                flags & ~UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT, 0u)) {
            ok = 0;
        }
        link = next;
    }
    return ok;
}

static uhm_result uhm_slab_remote_account_reserved_batch_lockless(
    uhm_slab *slab,
    uhm_tagged_freelist_node *link) {
    uhm_tagged_freelist_node *scan;
    size_t pending_decrements = 0;
    size_t inflight_decrements = 0;
    uhm_result result;

    for (scan = link; scan; scan = uhm_tagged_freelist_next_load(scan)) {
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(scan);
        uint32_t flags;

        result = uhm_slab_remote_slot_validate(slab, slot);
        if (result != UHM_OK) {
            return result;
        }
        flags = uhm_slab_slot_flags_load(slot);
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) == 0 ||
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) != 0) {
            return UHM_ERR_CORRUPTION;
        }
        ++pending_decrements;
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_PUBLISHING) != 0) {
            ++inflight_decrements;
        }
    }

    for (scan = link; scan; scan = uhm_tagged_freelist_next_load(scan)) {
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(scan);
        uint32_t flags;

        if (!uhm_slab_slot_belongs_to_remote_batch(slot, slab)) {
            (void)uhm_slab_remote_clear_accounted_batch_no_count(slab, link);
            return UHM_ERR_CORRUPTION;
        }
        flags = uhm_slab_slot_flags_load(slot);
        if (
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) == 0 ||
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) != 0 ||
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0) {
            (void)uhm_slab_remote_clear_accounted_batch_no_count(slab, link);
            return UHM_ERR_CORRUPTION;
        }
        if (!uhm_slab_slot_state_cas_atomic_only(
                slot, flags, 0u,
                flags | UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT, 0u)) {
            (void)uhm_slab_remote_clear_accounted_batch_no_count(slab, link);
            return UHM_ERR_CORRUPTION;
        }
    }

    result = uhm_slab_remote_counters_decrement_batch(
        slab, pending_decrements, inflight_decrements);
    if (result != UHM_OK) {
        (void)uhm_slab_remote_clear_accounted_batch_no_count(slab, link);
    }
    return result;
}

static uhm_result uhm_slab_remote_publish_accounted_batch_lockless(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_tagged_freelist_node *link) {
    uhm_tagged_freelist_node *scan;
    uhm_tagged_freelist_node *publish;
    size_t node_count = 0;
    size_t published = 0;
    uhm_result result = UHM_OK;

    for (scan = link; scan; scan = uhm_tagged_freelist_next_load(scan)) {
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(scan);
        uint32_t flags;

        result = uhm_slab_remote_slot_validate(slab, slot);
        if (result != UHM_OK) {
            if (!uhm_slab_remote_restore_reserved_batch(
                    heap, &slab->remote_free_lf, slab, 1u, link)) {
                return UHM_ERR_CORRUPTION;
            }
            return result;
        }
        flags = uhm_slab_slot_flags_load(slot);
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) == 0 ||
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) == 0 ||
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0) {
            if (!uhm_slab_remote_restore_reserved_batch(
                    heap, &slab->remote_free_lf, slab, 1u, link)) {
                return UHM_ERR_CORRUPTION;
            }
            return UHM_ERR_CORRUPTION;
        }
        ++node_count;
    }

    uhm_slab_raw_lock(slab);
    uhm_slab_raw_publish_begin(slab);
    publish = link;
    while (publish) {
        uhm_tagged_freelist_node *next =
            uhm_tagged_freelist_next_load(publish);
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(publish);
        uint32_t flags;

        flags = uhm_slab_slot_flags_load(slot);
        if ((flags & UHM_SLAB_SLOT_FLAG_REMOTE_DRAINING) == 0 ||
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_ACCOUNTED_OUT) == 0 ||
            (flags & UHM_SLAB_SLOT_FLAG_REMOTE_UNACCOUNTED) != 0 ||
            !uhm_slab_slot_state_cas(
                slot, flags, 0u, UHM_SLAB_SLOT_FLAG_FREE, 0u)) {
            result = UHM_ERR_CORRUPTION;
            break;
        }
        slot->next_free = slab->free_head;
        uhm_tagged_freelist_next_store(publish, 0);
        slab->free_head = slot;
        ++published;
        publish = next;
    }
    if (published != 0 || result == UHM_OK) {
        uhm_slab_summary_publish_raw_locked(slab);
    }
    uhm_slab_raw_publish_end(slab);
    uhm_slab_raw_unlock(slab);

    if (result != UHM_OK || published != node_count) {
        if (publish &&
            !uhm_slab_remote_restore_reserved_batch(
                heap, &slab->remote_free_lf, slab, 1u, publish)) {
            return UHM_ERR_CORRUPTION;
        }
        return result != UHM_OK ? result : UHM_ERR_CORRUPTION;
    }
    return UHM_OK;
}

static uhm_result uhm_slab_drain_remote_reserved_with_reader_lockless(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_metadata_reader_state *reader,
    size_t self_readers) {
    uhm_tagged_freelist_node *reserved = 0;
    uhm_result result = UHM_OK;
    size_t owned_readers;
    int drain_owned = 0;
    int wrap_deferred = 0;

    if (!heap || !slab || !reader) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    owned_readers =
        self_readers < SIZE_MAX ? self_readers + 1u : self_readers;
    if (!uhm_metadata_reader_hazard_set(reader, 1u, slab)) {
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_slab_remote_drain_try_acquire(heap, slab)) {
        return UHM_OK;
    }
    drain_owned = 1;
    if (!uhm_slab_descriptor_live_for_lockless(heap, slab)) {
        result = UHM_ERR_CORRUPTION;
        goto done;
    }

    reserved = uhm_heap_tagged_freelist_take_all(
        heap, &slab->remote_free_lf, owned_readers, &wrap_deferred);
    if (!reserved) {
        (void)wrap_deferred;
        result = UHM_OK;
        goto done;
    }
    if (!uhm_metadata_reader_hazard_set(reader, 0u, reserved)) {
        if (!uhm_slab_remote_restore_tagged_batch(
                heap, &slab->remote_free_lf, owned_readers, reserved)) {
            result = UHM_ERR_CORRUPTION;
        } else {
            result = UHM_ERR_CORRUPTION;
        }
        reserved = 0;
        goto done;
    }

    result = uhm_slab_remote_mark_reserved_batch_lockless(slab, reserved);
    if (result != UHM_OK) {
        if (!uhm_slab_remote_restore_reserved_batch(
                heap, &slab->remote_free_lf, slab, owned_readers,
                reserved)) {
            result = UHM_ERR_CORRUPTION;
        }
        reserved = 0;
        goto done;
    }
    result = uhm_slab_remote_account_reserved_batch_lockless(slab, reserved);
    if (result != UHM_OK) {
        if (!uhm_slab_remote_restore_reserved_batch(
                heap, &slab->remote_free_lf, slab, owned_readers,
                reserved)) {
            result = UHM_ERR_CORRUPTION;
        }
        reserved = 0;
        goto done;
    }
    if (!uhm_slab_descriptor_live_for_lockless(heap, slab)) {
        if (!uhm_slab_remote_restore_reserved_batch(
                heap, &slab->remote_free_lf, slab, owned_readers,
                reserved)) {
            result = UHM_ERR_CORRUPTION;
        } else {
            result = UHM_ERR_CORRUPTION;
        }
        reserved = 0;
        goto done;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.fault_lockless_remote_drain_countdown != 0 &&
        --heap->core.fault_lockless_remote_drain_countdown == 0) {
        if (!uhm_slab_remote_restore_reserved_batch(
                heap, &slab->remote_free_lf, slab, owned_readers,
                reserved)) {
            result = UHM_ERR_CORRUPTION;
        } else {
            result = UHM_ERR_OUT_OF_MEMORY;
        }
        reserved = 0;
        goto done;
    }
#endif
    result = uhm_slab_remote_publish_accounted_batch_lockless(heap, slab,
                                                              reserved);
    reserved = 0;

done:
    if (drain_owned) {
        uhm_slab_remote_drain_release(heap, slab);
    }
    return result;
}

static uhm_result
uhm_slab_drain_remote_reserved_no_decommit_with_reader_lockless(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_metadata_reader_state *reader,
    size_t self_readers) {
    return uhm_slab_drain_remote_reserved_with_reader_lockless(
        heap, slab, reader, self_readers);
}

static uhm_result uhm_slab_drain_remote_reserved_lockless(
    uhm_heap *heap,
    uhm_slab *slab,
    int needs_decommit_check) {
    uhm_metadata_reader_state *reader;
    uhm_result result = UHM_OK;

    if (!heap || !slab) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    reader = uhm_lockless_remote_reader_acquire(heap);
    if (!reader) {
        return UHM_OK;
    }
    result = uhm_slab_drain_remote_reserved_with_reader_lockless(
        heap, slab, reader, 1u);
    uhm_lockless_remote_reader_release(reader);
    if (result != UHM_OK) {
        return result;
    }

    if (!needs_decommit_check) {
        return UHM_OK;
    }

#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.fault_lockless_remote_decommit_trylock_countdown != 0 &&
        --heap->core.fault_lockless_remote_decommit_trylock_countdown == 0) {
        return UHM_OK;
    }
#endif
    if (!uhm_try_lock(&heap->lock)) {
        return UHM_OK;
    }
    if (!uhm_heap_is_accessible_pinned(heap) ||
        slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
        !uhm_slab_descriptor_live_for_lockless(heap, slab)) {
        result = UHM_ERR_CORRUPTION;
    } else {
        if (result == UHM_OK && needs_decommit_check &&
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
            uhm_slab_remote_pending_load(slab) == 0 &&
            uhm_slab_remote_inflight_load(slab) == 0 &&
            uhm_slab_remote_head_empty_snapshot(slab) &&
            uhm_slab_remote_lf_empty(slab)) {
            result = uhm_slab_maybe_decommit_empty(heap, slab);
        }
    }
    uhm_unlock(&heap->lock);
    if (result == UHM_ERR_UNSUPPORTED || result == UHM_ERR_PLATFORM) {
        return UHM_OK;
    }
    return result;
}

static uhm_result uhm_slab_drain_remote(uhm_heap *heap, uhm_slab *slab) {
    int wrap_deferred = 0;

    if (!slab || slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
        uhm_debug_corruption_site(heap, 4401u);
        return UHM_ERR_CORRUPTION;
    }
    for (;;) {
        for (;;) {
            uhm_tagged_freelist_node *link =
                uhm_heap_tagged_freelist_take_all(
                    heap, &slab->remote_free_lf, 0u, &wrap_deferred);
            uhm_result result;
            int publish_started = 0;

            if (!link) {
                break;
            }
            result = uhm_slab_remote_publish_tagged_batch_locked(
                heap, slab, link, &publish_started);
            if (result != UHM_OK) {
                if (!publish_started) {
                    (void)uhm_slab_remote_restore_tagged_batch(
                        heap, &slab->remote_free_lf, 0u, link);
                }
                return result;
            }
        }
        {
            uhm_slab_slot *head = uhm_slab_remote_head_take(slab);
            uhm_result result;
            if (head) {
                result = uhm_slab_remote_publish_list_batch_locked(heap, slab,
                                                                   head);
                if (result != UHM_OK) {
                    return result;
                }
            }
        }

        if (uhm_slab_remote_pending_load(slab) == 0 &&
            uhm_slab_remote_head_empty_snapshot(slab) &&
            uhm_slab_remote_lf_empty(slab)) {
            if (uhm_slab_remote_inflight_load(slab) == 0) {
                return UHM_OK;
            }
            if (uhm_heap_active_ops_load(heap) >
                    1u ||
                uhm_slab_remote_counter_update_active(slab)) {
                return UHM_OK;
            }
            uhm_debug_corruption_site(heap, 4402u);
            return UHM_ERR_CORRUPTION;
        }
        if (wrap_deferred) {
            return UHM_OK;
        }
        if (atomic_load_explicit(&slab->remote_ready_queued,
                                 UHM_ATOMIC_ACQUIRE) ==
            UHM_SLAB_REMOTE_READY_SCANNING) {
            return UHM_OK;
        }
        return UHM_OK;
    }
}

static uhm_tagged_freelist_node *uhm_slab_remote_class_take_slab_batch(
    uhm_tagged_freelist_node **head,
    uhm_slab *slab) {
    uhm_tagged_freelist_node *prev = 0;
    uhm_tagged_freelist_node *node;
    uhm_tagged_freelist_node *batch = 0;

    if (!head || !*head || !slab) {
        return 0;
    }
    node = *head;
    while (node) {
        uhm_tagged_freelist_node *next =
            uhm_tagged_freelist_next_load(node);
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(node);

        if (uhm_slab_slot_belongs_to_remote_batch(slot, slab)) {
            if (prev) {
                uhm_tagged_freelist_next_store(prev, next);
            } else {
                *head = next;
            }
            uhm_tagged_freelist_next_store(node, batch);
            batch = node;
            node = next;
            continue;
        }
        prev = node;
        node = next;
    }
    return batch;
}

static uhm_result uhm_slab_remote_publish_class_batch_locked(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_tagged_freelist_node *batch,
    uhm_metadata_reader_state *reader,
    int *out_publish_started);

static uhm_result uhm_slab_drain_remote_class_queue(uhm_heap *heap,
                                                    uint16_t size_class) {
    uhm_tagged_freelist *queue;
    uhm_metadata_reader_state *reader = 0;
    uhm_tagged_freelist_node *link;
    int wrap_deferred = 0;

    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    queue = &heap->core.slab_remote_class_lf[size_class].queue;
    if (uhm_tagged_freelist_unpack_ptr(
            atomic_load_explicit(&queue->head, UHM_ATOMIC_ACQUIRE)) == 0) {
        return UHM_OK;
    }
    reader = uhm_lockless_remote_reader_acquire(heap);
    if (!reader) {
        return UHM_ERR_BUSY;
    }
    link = uhm_heap_tagged_freelist_take_all(
        heap, queue, 1u, &wrap_deferred);
    if (!link && wrap_deferred) {
        uhm_lockless_remote_reader_release(reader);
        return UHM_OK;
    }
    while (link) {
        uhm_tagged_freelist_node *batch;
        uhm_slab_slot *slot;
        uhm_slab *slab;
        uhm_result result;

        if (!uhm_metadata_reader_hazard_set(reader, 0u, link)) {
            (void)uhm_slab_remote_restore_tagged_batch(heap, queue, 1u, link);
            uhm_lockless_remote_reader_release(reader);
            return UHM_ERR_CORRUPTION;
        }
        slot = uhm_slab_slot_from_remote_link(link);
        if (!slot || slot->size_class != size_class) {
            (void)uhm_slab_remote_restore_tagged_batch(
                heap, queue, 1u, link);
            uhm_lockless_remote_reader_release(reader);
            return UHM_ERR_CORRUPTION;
        }
        slab = uhm_slab_slot_owner(slot);
        if (!slab || slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class) {
            (void)uhm_slab_remote_restore_tagged_batch(
                heap, queue, 1u, link);
            uhm_lockless_remote_reader_release(reader);
            return UHM_ERR_CORRUPTION;
        }
        batch = uhm_slab_remote_class_take_slab_batch(&link, slab);
        {
            int publish_started = 0;
            result = uhm_slab_remote_publish_class_batch_locked(
                heap, slab, batch, reader, &publish_started);
            if (result != UHM_OK) {
                if (!publish_started) {
                    int restored = 1;
                    if (batch &&
                        !uhm_slab_remote_restore_tagged_batch(
                            heap, queue, 1u, batch)) {
                        restored = 0;
                    }
                    if (link &&
                        !uhm_slab_remote_restore_tagged_batch(
                            heap, queue, 1u, link)) {
                        restored = 0;
                    }
                    if (!restored) {
                        uhm_lockless_remote_reader_release(reader);
                        return UHM_ERR_CORRUPTION;
                    }
                }
                uhm_lockless_remote_reader_release(reader);
                return result;
            }
        }
    }
    uhm_lockless_remote_reader_release(reader);
    return UHM_OK;
}

static uhm_result uhm_slab_remote_publish_class_batch_locked(
    uhm_heap *heap,
    uhm_slab *slab,
    uhm_tagged_freelist_node *batch,
    uhm_metadata_reader_state *reader,
    int *out_publish_started) {
    uhm_result result;

    if (out_publish_started) {
        *out_publish_started = 0;
    }
    if (!heap || !slab || !batch || !reader) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    if (heap->core.fault_lockless_remote_drain_countdown != 0 &&
        --heap->core.fault_lockless_remote_drain_countdown == 0) {
        return UHM_ERR_BUSY;
    }
#endif
    if (!uhm_metadata_reader_hazard_set(reader, 1u, slab) ||
        !uhm_metadata_reader_hazard_set(reader, 0u, batch)) {
        return UHM_ERR_CORRUPTION;
    }
    if (out_publish_started) {
        *out_publish_started = 1;
    }
    result = uhm_slab_remote_publish_tagged_batch_locked(
        heap, slab, batch, out_publish_started);
    return result;
}

#if UINTPTR_MAX == UINT32_MAX
static uhm_result uhm_slab_drain_remote_raw_ready_class(
    uhm_heap *heap,
    uint16_t size_class) {
    uhm_metadata_reader_state *reader;
    size_t scanned = 0u;
    size_t scan_limit;

    if (!uhm_slab_remote_raw_ready_supported(heap) ||
        size_class >= UHM_SIZE_CLASS_COUNT) {
        return UHM_OK;
    }
    if (atomic_load_explicit(
            &heap->core.slab_remote_ready_raw[size_class].value,
            UHM_ATOMIC_ACQUIRE) == 0) {
        return UHM_OK;
    }
    reader = uhm_lockless_remote_reader_acquire(heap);
    if (!reader) {
        return UHM_ERR_BUSY;
    }
    scan_limit = heap->core.page_count == SIZE_MAX
                     ? SIZE_MAX
                     : heap->core.page_count + 1u;
    for (;;) {
        uhm_result take_result = UHM_OK;
        uhm_slab *slab = uhm_slab_remote_raw_ready_take(
            heap, size_class, reader, &take_result);
        uhm_slab_slot *head;
        uhm_result result;

        if (!slab) {
            uhm_lockless_remote_reader_release(reader);
            return take_result;
        }
        if (++scanned > scan_limit || slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class) {
            uhm_lockless_remote_reader_release(reader);
            return UHM_ERR_CORRUPTION;
        }
        /*
         * Ready is a notification, not ownership of the remote slot list.
         * A direct empty-slab drain or another raw consumer can empty that
         * list before this notification is consumed.  In particular, a
         * direct drain may already have decommitted the slab: inspect only
         * its control metadata, never an old slot in the decommitted pages.
         */
        if ((slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0u) {
            if (!uhm_slab_raw_ready_decommitted_valid(slab)) {
                uhm_lockless_remote_reader_release(reader);
                return UHM_ERR_CORRUPTION;
            }
            continue;
        }
        if (!uhm_slab_descriptor_live_for_lockless(heap, slab)) {
            uhm_lockless_remote_reader_release(reader);
            return UHM_ERR_CORRUPTION;
        }
        head = uhm_slab_remote_head_take(slab);
        if (!head) {
            continue;
        }
        result = uhm_slab_remote_publish_list_batch_locked(heap, slab, head);
        if (result != UHM_OK) {
            uhm_lockless_remote_reader_release(reader);
            return result;
        }
    }
}
#endif

static uhm_result uhm_slab_drain_remote_class(uhm_heap *heap,
                                              uint16_t size_class,
                                              int drain_class_queue) {
    uhm_slab *slab;
    uhm_result result = UHM_OK;
    uint32_t pending_mask;
    uint32_t class_bit;

    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
#if UINTPTR_MAX == UINT32_MAX
    result = uhm_slab_drain_remote_raw_ready_class(heap, size_class);
    if (result != UHM_OK) {
        return result;
    }
#endif
    if (drain_class_queue) {
        result = uhm_slab_drain_remote_class_queue(heap, size_class);
        if (result != UHM_OK) {
            return result;
        }
    }

    class_bit = uhm_slab_remote_pending_class_bit(size_class);
    pending_mask = uhm_slab_remote_pending_take(heap, size_class);
    if ((pending_mask & class_bit) == 0u) {
        return UHM_OK;
    }
#if UHMAS_ENABLE_TEST_HOOKS
    ++heap->core.slab_remote_class_scan_calls;
#endif

    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
#if UHMAS_ENABLE_TEST_HOOKS
        ++heap->core.slab_remote_class_scan_slabs;
#endif
        if (slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class != size_class) {
            continue;
        }
        if (drain_class_queue) {
            if (uhm_slab_remote_pending_load(slab) == 0) {
                continue;
            }
        } else if (uhm_slab_remote_head_empty_snapshot(slab) &&
                   uhm_slab_remote_lf_empty(slab)) {
            continue;
        }
        result = uhm_slab_drain_remote(heap, slab);
        if (result != UHM_OK) {
            uhm_slab_remote_pending_note(heap, size_class);
            return result;
        }
    }
    return UHM_OK;
}

static uhm_result uhm_slab_drain_remote_all(uhm_heap *heap,
                                            int drain_class_queue) {
    uhm_slab *slab;
    uint16_t size_class;

    if (!heap) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
#if UINTPTR_MAX == UINT32_MAX
    for (size_class = 0; size_class < UHM_SIZE_CLASS_COUNT; ++size_class) {
        uhm_result result = uhm_slab_drain_remote_raw_ready_class(
            heap, size_class);
        if (result != UHM_OK) {
            return result;
        }
    }
#endif
    if (drain_class_queue) {
        for (size_class = 0; size_class < UHM_SIZE_CLASS_COUNT;
             ++size_class) {
            uhm_result result =
                uhm_slab_drain_remote_class_queue(heap, size_class);
            if (result != UHM_OK) {
                return result;
            }
        }
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        uhm_result result;
        if (slab->magic != UHM_SLAB_MAGIC) {
            return UHM_ERR_CORRUPTION;
        }
        if ((drain_class_queue &&
             uhm_slab_remote_pending_load(slab) == 0 &&
             uhm_slab_remote_head_empty_snapshot(slab) &&
             uhm_slab_remote_lf_empty(slab)) ||
            (!drain_class_queue &&
             uhm_slab_remote_head_empty_snapshot(slab) &&
             uhm_slab_remote_lf_empty(slab))) {
            continue;
        }
        result = uhm_slab_drain_remote(heap, slab);
        if (result != UHM_OK) {
            return result;
        }
    }
    return UHM_OK;
}

