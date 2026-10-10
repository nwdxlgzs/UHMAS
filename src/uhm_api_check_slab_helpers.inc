static int uhm_check_slab_slot_address(uhm_heap *heap,
                                       uhm_slab_slot *slot,
                                       uhm_slab **out_slab) {
    uintptr_t slot_addr = (uintptr_t)slot;
    uhm_slab *slab;

    if (out_slab) {
        *out_slab = 0;
    }
    if (!heap || !slot) {
        return 0;
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        uintptr_t slab_end;
        size_t offset;

        if (slab->magic != UHM_SLAB_MAGIC ||
            slab->slot_stride == 0 ||
            uhm_range_overflows(slab->base, slab->size, &slab_end)) {
            return 0;
        }
        if (slot_addr < slab->base || slot_addr >= slab_end) {
            continue;
        }
        offset = (size_t)(slot_addr - slab->base);
        if ((offset % slab->slot_stride) != 0) {
            return 0;
        }
        if (out_slab) {
            *out_slab = slab;
        }
        return 1;
    }
    return 0;
}

static int uhm_check_slab_class_queue_limit(uhm_heap *heap,
                                            uint16_t size_class,
                                            size_t *out_limit) {
    uhm_slab *slab;
    size_t limit = 0;

    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT || !out_limit) {
        return 0;
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        if (slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class >= UHM_SIZE_CLASS_COUNT ||
            slab->total_slots == 0) {
            return 0;
        }
        if (slab->size_class == size_class) {
            if (limit > SIZE_MAX - slab->total_slots) {
                return 0;
            }
            limit += slab->total_slots;
        }
    }
    *out_limit = limit != 0 ? limit : 1u;
    return 1;
}

static int uhm_check_remote_class_queue_count_for_slab(uhm_heap *heap,
                                                       uhm_slab *target,
                                                       size_t *out_count) {
    uhm_tagged_freelist_node *link;
    size_t count = 0;
    size_t seen = 0;
    size_t limit = 0;
    uint16_t size_class;

    if (!heap || !target || target->size_class >= UHM_SIZE_CLASS_COUNT ||
        !out_count) {
        return 0;
    }
    size_class = target->size_class;
    if (!uhm_check_slab_class_queue_limit(heap, size_class, &limit)) {
        return 0;
    }
    if (uhm_slab_remote_raw_ready_supported(heap)) {
        int raw_ok = uhm_slab_remote_raw_ready_validate_locked(
            heap, size_class, target, limit, out_count);
        /* Raw ready tracks slabs; the slot list is counted below. */
        *out_count = 0;
        return raw_ok;
    }
    link = uhm_tagged_freelist_unpack_ptr(atomic_load_explicit(
        &heap->core.slab_remote_class_lf[size_class].queue.head,
        UHM_ATOMIC_ACQUIRE));
    while (link) {
        uhm_slab_slot *slot = uhm_slab_slot_from_remote_link(link);
        uhm_slab *owner = 0;
        uint32_t flags;

        if (++seen > limit ||
            !uhm_check_slab_slot_address(heap, slot, &owner) ||
            !owner ||
            owner->size_class != size_class ||
            slot->magic != UHM_SLAB_SLOT_MAGIC ||
            !uhm_slab_slot_belongs_to(slot, owner) ||
            slot->size_class != size_class ||
            slot->state != 0 ||
            !uhm_slab_slot_state_mirror_valid_for_remote(slot)) {
            return 0;
        }
        flags = uhm_slab_slot_flags_load(slot);
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
            uhm_slab_slot_requested_load(slot) != 0) {
            return 0;
        }
        if (owner == target) {
            ++count;
        }
        link = uhm_tagged_freelist_next_load(link);
    }
    *out_count = count;
    return 1;
}

static int uhm_check_slab_ready_queue_limit(uhm_heap *heap,
                                            uint16_t size_class,
                                            size_t *out_limit) {
    uhm_slab *slab;
    size_t limit = 0;

    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT || !out_limit) {
        return 0;
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        if (slab->magic != UHM_SLAB_MAGIC ||
            slab->size_class >= UHM_SIZE_CLASS_COUNT) {
            return 0;
        }
        if (slab->size_class == size_class) {
            ++limit;
        }
    }
    *out_limit = limit != 0 ? limit : 1u;
    return 1;
}

static int uhm_check_slab_ready_link_owner(uhm_heap *heap,
                                           uhm_tagged_freelist_node *link,
                                           uhm_slab **out_slab) {
    uhm_slab *slab;

    if (out_slab) {
        *out_slab = 0;
    }
    if (!heap || !link) {
        return 0;
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        if (slab->magic != UHM_SLAB_MAGIC) {
            return 0;
        }
        if (&slab->remote_ready_link == link) {
            if (out_slab) {
                *out_slab = slab;
            }
            return 1;
        }
    }
    return 0;
}

static int uhm_check_remote_ready_queue_count_for_slab(uhm_heap *heap,
                                                       uhm_slab *target,
                                                       size_t *out_count) {
    uhm_tagged_freelist_node *link;
    size_t count = 0;
    size_t seen = 0;
    size_t limit = 0;
    uint16_t size_class;

    if (!heap || !target || target->size_class >= UHM_SIZE_CLASS_COUNT ||
        !out_count ||
        !uhm_check_slab_ready_queue_limit(heap, target->size_class, &limit)) {
        return 0;
    }
    size_class = target->size_class;
    if (uhm_slab_remote_raw_ready_supported(heap)) {
        return uhm_slab_remote_raw_ready_validate_locked(
            heap, size_class, target, limit, out_count);
    }
    link = uhm_tagged_freelist_unpack_ptr(atomic_load_explicit(
        &heap->core.slab_remote_ready_lf[size_class].queue.head,
        UHM_ATOMIC_ACQUIRE));
    while (link) {
        uhm_slab *owner = 0;

        if (++seen > limit ||
            !uhm_check_slab_ready_link_owner(heap, link, &owner) ||
            !owner ||
            owner->size_class != size_class ||
            atomic_load_explicit(&owner->remote_ready_queued,
                                 UHM_ATOMIC_ACQUIRE) !=
                UHM_SLAB_REMOTE_READY_QUEUED) {
            return 0;
        }
        if (owner == target) {
            ++count;
        }
        link = uhm_tagged_freelist_next_load(link);
    }
    *out_count = count;
    return 1;
}

static int uhm_check_slab_free_list_count(uhm_slab *slab,
                                          uintptr_t slab_end,
                                          size_t *out_count) {
    uhm_slab_slot *free_slot;
    size_t free_list_count = 0;

    if (!slab || !out_count) {
        return 0;
    }
    uhm_slab_raw_lock(slab);
    for (free_slot = slab->free_head; free_slot;
         free_slot = free_slot->next_free) {
        uintptr_t slot_addr = (uintptr_t)free_slot;
        size_t offset;
        uint32_t free_flags;

        if (free_list_count >= slab->total_slots ||
            slot_addr < slab->base ||
            slot_addr >= slab_end) {
            uhm_slab_raw_unlock(slab);
            return 0;
        }
        offset = (size_t)(slot_addr - slab->base);
        if ((offset % slab->slot_stride) != 0 ||
            free_slot->magic != UHM_SLAB_SLOT_MAGIC ||
            !uhm_slab_slot_belongs_to(free_slot, slab) ||
            free_slot->size_class != slab->size_class ||
            free_slot->state != 0 ||
            !uhm_slab_slot_state_mirror_matches(free_slot)) {
            uhm_slab_raw_unlock(slab);
            return 0;
        }
        free_flags = uhm_slab_slot_flags_load(free_slot);
        if ((free_flags & ~UHM_SLAB_SLOT_FLAG_FREE) != 0 ||
            (free_flags & UHM_SLAB_SLOT_FLAG_FREE) == 0 ||
            uhm_slab_slot_requested_load(free_slot) != 0) {
            uhm_slab_raw_unlock(slab);
            return 0;
        }
        ++free_list_count;
    }
    uhm_slab_raw_unlock(slab);
    *out_count = free_list_count;
    return 1;
}

static int uhm_tagged_freelist_epoch_valid_for_check(
    const uhm_heap *heap,
    const uhm_tagged_freelist *list) {
    uint64_t overflows;
    uint64_t last_epoch;

    if (!heap || !list) {
        return 0;
    }
    overflows = atomic_load_explicit(&list->tag_overflows,
                                     UHM_ATOMIC_ACQUIRE);
    last_epoch = atomic_load_explicit(&list->last_wrap_epoch,
                                      UHM_ATOMIC_ACQUIRE);
    if (overflows == 0) {
        return last_epoch == 0;
    }
    return last_epoch != 0 && last_epoch <= heap->core.metadata_epoch;
}
