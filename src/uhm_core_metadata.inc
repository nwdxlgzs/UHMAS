static uhm_slab *uhm_slab_slot_owner(const uhm_slab_slot *slot) {
    return slot ? slot->slab : 0;
}

static int uhm_slab_slot_belongs_to(const uhm_slab_slot *slot,
                                    const uhm_slab *slab) {
    return slab && uhm_slab_slot_owner(slot) == slab;
}

#if UHMAS_STRICT_SLOT_OWNER_GEOMETRY
static int uhm_slab_slot_belongs_to_slab_geometry(
    const uhm_slab_slot *slot,
    const uhm_slab *slab) {
    uintptr_t slot_addr = (uintptr_t)slot;
    uintptr_t slab_end;
    size_t offset;
    size_t slot_index;

    if (!slot || !slab ||
        slab->magic != UHM_SLAB_MAGIC ||
        slab->slot_stride == 0 ||
        slab->total_slots == 0 ||
        slab->size > UINTPTR_MAX - slab->base) {
        return 0;
    }
    slab_end = slab->base + slab->size;
    if (slot_addr < slab->base || slot_addr >= slab_end) {
        return 0;
    }
    offset = (size_t)(slot_addr - slab->base);
    if ((offset % slab->slot_stride) != 0) {
        return 0;
    }
    slot_index = offset / slab->slot_stride;
    return slot_index < slab->total_slots &&
           slot->magic == UHM_SLAB_SLOT_MAGIC &&
           slot->size_class == slab->size_class &&
           slot->state == 0;
}
#endif

static int uhm_slab_slot_belongs_to_verified_slab(
    const uhm_slab_slot *slot,
    const uhm_slab *slab) {
#if UHMAS_STRICT_SLOT_OWNER_GEOMETRY
    return uhm_slab_slot_belongs_to_slab_geometry(slot, slab);
#else
    return uhm_slab_slot_belongs_to(slot, slab);
#endif
}

static int uhm_slab_slot_belongs_to_remote_batch(
    const uhm_slab_slot *slot,
    const uhm_slab *slab) {
    return uhm_slab_slot_belongs_to_verified_slab(slot, slab);
}

static size_t uhm_slab_slot_owner_slot_size(const uhm_slab_slot *slot) {
    uhm_slab *slab = uhm_slab_slot_owner(slot);
    return slab ? slab->slot_size : 0;
}

static void uhm_metadata_reader_accumulate_safe_epoch(
    const uhm_metadata_reader_state *reader,
    uint64_t current_epoch,
    uint64_t *safe_epoch) {
    size_t depth;
    uint64_t epoch;

    if (!reader || !safe_epoch) {
        return;
    }
    depth = atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE);
    if (depth == 0) {
        return;
    }
    epoch = atomic_load_explicit(&reader->epoch, UHM_ATOMIC_ACQUIRE);
    if (epoch == 0 || epoch > current_epoch) {
        *safe_epoch = 0;
    } else if (epoch < *safe_epoch) {
        *safe_epoch = epoch;
    }
}

static void uhm_remote_drain_owner_accumulate_safe_epoch_locked(
    uhm_heap *heap,
    uint64_t current_epoch,
    uint64_t *safe_epoch) {
    uhm_slab *slab;

    if (!heap || !safe_epoch || *safe_epoch == 0) {
        return;
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        uint32_t state =
            atomic_load_explicit(&slab->remote_drain_active,
                                 UHM_ATOMIC_ACQUIRE);
        uint64_t epoch =
            atomic_load_explicit(&slab->remote_drain_epoch,
                                 UHM_ATOMIC_ACQUIRE);

        if (state == 0) {
            if (epoch != 0) {
                *safe_epoch = 0;
                return;
            }
            continue;
        }
        if (state != UHM_REMOTE_DRAIN_OWNER_ACTIVE || epoch == 0 ||
            epoch > current_epoch) {
            *safe_epoch = 0;
            return;
        }
        if (epoch < *safe_epoch) {
            *safe_epoch = epoch;
        }
    }
}

static uint64_t uhm_metadata_safe_epoch_locked(uhm_heap *heap) {
    uint64_t current_epoch;
    uint64_t safe_epoch;
    uhm_thread_ctx *ctx;
    uhm_tls_cache *cache;
    size_t i;

    if (!heap || uhm_metadata_hazard_readers_load(heap) != 0) {
        return 0;
    }
    current_epoch = heap->core.metadata_epoch;
    safe_epoch = current_epoch;
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        uhm_metadata_reader_accumulate_safe_epoch(
            &heap->core.lockless_remote_readers[i], current_epoch,
            &safe_epoch);
    }
    for (ctx = heap->core.thread_ctx_head; ctx; ctx = ctx->next_active) {
        uhm_metadata_reader_accumulate_safe_epoch(&ctx->metadata_reader,
                                                  current_epoch, &safe_epoch);
    }
    for (cache = heap->core.tls_cache_head; cache; cache = cache->next_active) {
        uhm_metadata_reader_accumulate_safe_epoch(&cache->metadata_reader,
                                                  current_epoch, &safe_epoch);
    }
    uhm_remote_drain_owner_accumulate_safe_epoch_locked(heap, current_epoch,
                                                       &safe_epoch);
    return safe_epoch;
}

static int uhm_metadata_reader_hazards_node(
    const uhm_metadata_reader_state *reader,
    const void *node) {
    size_t i;

    if (!reader || !node) {
        return 0;
    }
    for (i = 0; i < UHM_METADATA_HAZARD_SLOTS; ++i) {
        if ((void *)atomic_load_explicit(&reader->hazards[i],
                                         UHM_ATOMIC_ACQUIRE) == node) {
            return 1;
        }
    }
    return 0;
}

static int uhm_metadata_node_is_hazard_locked(uhm_heap *heap, const void *node) {
    uhm_thread_ctx *ctx;
    uhm_tls_cache *cache;
    size_t i;

    if (!heap || !node) {
        return 0;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        if (uhm_metadata_reader_hazards_node(
                &heap->core.lockless_remote_readers[i], node)) {
            return 1;
        }
    }
    for (ctx = heap->core.thread_ctx_head; ctx; ctx = ctx->next_active) {
        if (uhm_metadata_reader_hazards_node(&ctx->metadata_reader, node)) {
            return 1;
        }
    }
    for (cache = heap->core.tls_cache_head; cache; cache = cache->next_active) {
        if (uhm_metadata_reader_hazards_node(&cache->metadata_reader, node)) {
            return 1;
        }
    }
    return 0;
}

static int uhm_lockless_remote_reader_hazards_slab_locked(
    uhm_heap *heap,
    const uhm_slab *slab) {
    size_t i;

    if (!heap || !slab) {
        return 0;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        const uhm_metadata_reader_state *reader =
            &heap->core.lockless_remote_readers[i];
        if (atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE) != 0 &&
            uhm_metadata_reader_hazards_node(reader, slab)) {
            return 1;
        }
    }
    return 0;
}

static int uhm_lockless_remote_reader_hazards_slab_and_node_locked(
    uhm_heap *heap,
    const uhm_slab *slab,
    const void *node) {
    size_t i;

    if (!heap || !slab || !node) {
        return 0;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        const uhm_metadata_reader_state *reader =
            &heap->core.lockless_remote_readers[i];
        if (atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE) != 0 &&
            uhm_metadata_reader_hazards_node(reader, slab) &&
            uhm_metadata_reader_hazards_node(reader, node)) {
            return 1;
        }
    }
    return 0;
}

static int uhm_slab_scanner_hazard_node_matches(const uhm_slab *slab,
                                                const void *node,
                                                int include_slot_remote) {
    uintptr_t addr;
    uintptr_t end;
    size_t offset;

    if (!slab || !node) {
        return 0;
    }
    if (node == (const void *)&slab->remote_ready_link) {
        return 1;
    }
    if (!include_slot_remote ||
        slab->slot_stride == 0 ||
        slab->total_slots == 0 ||
        uhm_range_overflows(slab->base, slab->size, &end)) {
        return 0;
    }
    addr = (uintptr_t)node;
    if (addr < slab->base || addr >= end) {
        return 0;
    }
    offset = (size_t)(addr - slab->base);
    return (offset / slab->slot_stride) < slab->total_slots &&
           (offset % slab->slot_stride) == offsetof(uhm_slab_slot,
                                                    remote_link);
}

static int uhm_lockless_remote_reader_hazards_slab_and_scanner_node_locked(
    uhm_heap *heap,
    const uhm_slab *slab,
    int include_slot_remote) {
    size_t i;

    if (!heap || !slab) {
        return 0;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        const uhm_metadata_reader_state *reader =
            &heap->core.lockless_remote_readers[i];
        size_t hazard_index;

        if (atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE) == 0 ||
            !uhm_metadata_reader_hazards_node(reader, slab)) {
            continue;
        }
        for (hazard_index = 0; hazard_index < UHM_METADATA_HAZARD_SLOTS;
             ++hazard_index) {
            const void *hazard = (const void *)atomic_load_explicit(
                &reader->hazards[hazard_index], UHM_ATOMIC_ACQUIRE);
            if (uhm_slab_scanner_hazard_node_matches(slab, hazard,
                                                     include_slot_remote)) {
                return 1;
            }
        }
    }
    return 0;
}

static int uhm_metadata_vma_node_contains(const uhm_vma *vma,
                                          const void *node) {
    return vma && node &&
           ((const void *)vma == node ||
            (const void *)&vma->prev == node ||
            (const void *)&vma->next == node ||
            (const void *)&vma->tree_parent == node ||
            (const void *)&vma->tree_left == node ||
            (const void *)&vma->tree_right == node ||
            (const void *)&vma->desc == node);
}

static int uhm_metadata_vma_list_contains(uhm_vma *head, const void *node,
                                          size_t limit) {
    size_t count = 0;

    for (; head; head = head->next) {
        if (uhm_metadata_vma_node_contains(head, node)) {
            return 1;
        }
        if (++count > limit) {
            return 0;
        }
    }
    return 0;
}

static int uhm_metadata_free_extent_node_contains(
    const uhm_free_extent *extent,
    const void *node) {
    return extent && node &&
           ((const void *)extent == node ||
            (const void *)&extent->prev == node ||
            (const void *)&extent->next == node ||
            (const void *)&extent->tree_parent == node ||
            (const void *)&extent->tree_left == node ||
            (const void *)&extent->tree_right == node ||
            (const void *)&extent->desc == node);
}

static int uhm_metadata_free_extent_list_contains(uhm_free_extent *head,
                                                  const void *node,
                                                  size_t limit) {
    size_t count = 0;

    for (; head; head = head->next) {
        if (uhm_metadata_free_extent_node_contains(head, node)) {
            return 1;
        }
        if (++count > limit) {
            return 0;
        }
    }
    return 0;
}

static int uhm_control_extent_list_contains(uhm_control_extent *head,
                                            const uhm_control_extent *needle,
                                            size_t limit) {
    size_t count = 0;

    for (; head; head = head->next) {
        if (head == needle) {
            return 1;
        }
        if (++count > limit) {
            return 0;
        }
    }
    return 0;
}

static int uhm_metadata_slab_or_slot_contains(uhm_slab *head,
                                              const void *node,
                                              size_t limit) {
    uintptr_t addr;
    size_t count = 0;

    if (!node) {
        return 0;
    }
    addr = (uintptr_t)node;
    for (; head; head = head->next_all) {
        uintptr_t end;
        size_t offset;
        size_t slot_index;
        uhm_slab_slot *slot;

        if (++count > limit ||
            head->magic != UHM_SLAB_MAGIC ||
            head->size_class >= UHM_SIZE_CLASS_COUNT ||
            head->slot_stride == 0 ||
            head->total_slots == 0 ||
            uhm_range_overflows(head->base, head->size, &end)) {
            return 0;
        }
        if ((const void *)head == node ||
            (const void *)&head->next_class == node ||
            (const void *)&head->prev_class == node ||
            (const void *)&head->next_all == node ||
            (const void *)&head->prev_all == node ||
            (const void *)&head->remote_ready_link == node ||
            (const void *)&head->desc == node) {
            return 1;
        }
        if ((head->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
            addr < head->base || addr >= end) {
            continue;
        }
        offset = (size_t)(addr - head->base);
        slot_index = offset / head->slot_stride;
        if ((offset % head->slot_stride) != 0) {
            uintptr_t slot_base;
            if (slot_index >= head->total_slots) {
                continue;
            }
            slot_base = head->base + slot_index * head->slot_stride;
            slot = (uhm_slab_slot *)slot_base;
            if (uhm_slab_slot_belongs_to_remote_batch(slot, head) &&
                addr == (uintptr_t)&slot->remote_link) {
                return 1;
            }
            continue;
        }
        if (slot_index >= head->total_slots) {
            continue;
        }
        slot = (uhm_slab_slot *)(head->base + slot_index * head->slot_stride);
        if (uhm_slab_slot_belongs_to_remote_batch(slot, head)) {
            return 1;
        }
    }
    return 0;
}

static int uhm_metadata_hazard_target_known_locked(uhm_heap *heap,
                                                   const void *node) {
    size_t limit;

    if (!heap || !node) {
        return 0;
    }
    limit = heap->core.page_count +
            heap->core.retired_vma_count +
            heap->core.retired_free_extent_count +
            heap->core.emergency_vma_available +
            heap->core.emergency_free_extent_available + 16u;
    return uhm_metadata_vma_list_contains(heap->core.vma_head, node, limit) ||
           uhm_metadata_vma_list_contains(heap->core.retired_vma_head, node,
                                          limit) ||
           uhm_metadata_vma_list_contains(
               heap->core.retired_emergency_vma_head, node, limit) ||
           uhm_metadata_vma_list_contains(heap->core.emergency_vma_head, node,
                                          limit) ||
           uhm_metadata_free_extent_list_contains(heap->core.mmap_free_head,
                                                  node, limit) ||
           uhm_metadata_free_extent_list_contains(
               heap->core.retired_free_extent_head, node, limit) ||
           uhm_metadata_free_extent_list_contains(
               heap->core.retired_emergency_free_extent_head, node, limit) ||
           uhm_metadata_free_extent_list_contains(
               heap->core.emergency_free_extent_head, node, limit) ||
           uhm_metadata_slab_or_slot_contains(heap->core.slab_all_head,
                                              node, limit);
}

static int uhm_metadata_reader_update_observed(
    const uhm_metadata_reader_state *reader,
    uint32_t seq_before) {
    uint32_t seq_after = uhm_metadata_reader_local_update_seq_load(reader);
    return seq_before != seq_after ||
           uhm_metadata_reader_local_update_seq_active_value(seq_before) ||
           uhm_metadata_reader_local_update_seq_active_value(seq_after);
}

static int uhm_metadata_reader_update_allows_transient(
    uhm_heap *heap,
    const uhm_metadata_reader_state *reader,
    uint32_t seq_before) {
    if (!heap ||
        uhm_heap_active_ops_load(heap) <= 1u) {
        return 0;
    }
    return uhm_metadata_reader_update_observed(reader, seq_before) ||
           uhm_metadata_registered_reader_update_active(heap);
}

static int uhm_metadata_reader_finish_validate(
    uhm_heap *heap,
    const uhm_metadata_reader_state *reader,
    uint32_t seq_before) {
    if (!uhm_metadata_reader_update_observed(reader, seq_before)) {
        return 1;
    }
    return heap &&
           uhm_heap_active_ops_load(heap) > 1u;
}

static int uhm_metadata_reader_state_validate_locked(
    uhm_heap *heap,
    const uhm_metadata_reader_state *reader) {
    size_t depth;
    uint64_t epoch;
    uint32_t seq_before;
    size_t i;

    if (!heap || !reader) {
        return 0;
    }
    seq_before = uhm_metadata_reader_local_update_seq_load(reader);
    depth = atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE);
    epoch = atomic_load_explicit(&reader->epoch, UHM_ATOMIC_ACQUIRE);
    if (depth == 0) {
        if (epoch != 0 ||
            atomic_load_explicit(&reader->owner_heap,
                                 UHM_ATOMIC_ACQUIRE) != (uintptr_t)0 ||
            uhm_metadata_reader_registered(reader)) {
            return uhm_metadata_reader_update_allows_transient(
                heap, reader, seq_before);
        }
        for (i = 0; i < UHM_METADATA_HAZARD_SLOTS; ++i) {
            if (atomic_load_explicit(&reader->hazards[i],
                                     UHM_ATOMIC_ACQUIRE) != (uintptr_t)0) {
                return uhm_metadata_reader_update_allows_transient(
                    heap, reader, seq_before);
            }
        }
        return uhm_metadata_reader_finish_validate(heap, reader, seq_before);
    }
    if (epoch > heap->core.metadata_epoch) {
        if (depth == SIZE_MAX &&
            uhm_metadata_reader_update_allows_transient(heap, reader,
                                                        seq_before)) {
            return 1;
        }
        return 0;
    }
    if (atomic_load_explicit(&reader->owner_heap,
                             UHM_ATOMIC_ACQUIRE) != (uintptr_t)heap) {
        if (uhm_metadata_reader_update_allows_transient(heap, reader,
                                                        seq_before)) {
            return 1;
        }
        return 0;
    }
    for (i = 0; i < UHM_METADATA_HAZARD_SLOTS; ++i) {
        uintptr_t hazard =
            atomic_load_explicit(&reader->hazards[i], UHM_ATOMIC_ACQUIRE);
        if (hazard != (uintptr_t)0 &&
            !uhm_metadata_hazard_target_known_locked(heap,
                                                     (const void *)hazard)) {
            return 0;
        }
    }
    return uhm_metadata_reader_finish_validate(heap, reader, seq_before);
}

static int uhm_lockless_remote_readers_validate_locked(uhm_heap *heap) {
    size_t i;

    if (!heap) {
        return 0;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        if (!uhm_metadata_reader_state_validate_locked(
                heap, &heap->core.lockless_remote_readers[i])) {
            return 0;
        }
    }
    return 1;
}

static int uhm_metadata_reader_active(
    const uhm_metadata_reader_state *reader) {
    return reader &&
           atomic_load_explicit(&reader->depth, UHM_ATOMIC_ACQUIRE) != 0;
}

static size_t uhm_metadata_registered_reader_count_locked(uhm_heap *heap) {
    uhm_thread_ctx *ctx;
    uhm_tls_cache *cache;
    uhm_slab *slab;
    size_t i;
    size_t count = 0;

    if (!heap) {
        return 0;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        if (uhm_metadata_reader_active(
                &heap->core.lockless_remote_readers[i]) &&
            uhm_metadata_reader_registered(
                &heap->core.lockless_remote_readers[i])) {
            ++count;
        }
    }
    for (ctx = heap->core.thread_ctx_head; ctx; ctx = ctx->next_active) {
        if (uhm_metadata_reader_active(&ctx->metadata_reader) &&
            uhm_metadata_reader_registered(&ctx->metadata_reader)) {
            ++count;
        }
    }
    for (cache = heap->core.tls_cache_head; cache; cache = cache->next_active) {
        if (uhm_metadata_reader_active(&cache->metadata_reader) &&
            uhm_metadata_reader_registered(&cache->metadata_reader)) {
            ++count;
        }
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        if (atomic_load_explicit(&slab->remote_drain_active,
                                 UHM_ATOMIC_ACQUIRE) != 0) {
            ++count;
        }
    }
    return count;
}

static int uhm_metadata_registered_readers_valid_for_check(uhm_heap *heap) {
    uint64_t seq_before;
    uint64_t seq_after;
    size_t registered;
    size_t counted;

    if (!heap) {
        return 0;
    }
    seq_before = uhm_metadata_registered_reader_seq_load(heap);
    registered = uhm_metadata_registered_readers_load(heap);
    counted = uhm_metadata_registered_reader_count_locked(heap);
    seq_after = uhm_metadata_registered_reader_seq_load(heap);
    if (registered == counted) {
        if ((uhm_metadata_registered_reader_seq_active_value(seq_before) ||
             uhm_metadata_registered_reader_seq_active_value(seq_after)) &&
            uhm_heap_active_ops_load(heap) <= 1u) {
            return 0;
        }
        return 1;
    }
    if (uhm_heap_active_ops_load(heap) <= 1u) {
        return 0;
    }
    return seq_before != seq_after ||
           uhm_metadata_registered_reader_seq_active_value(seq_before) ||
           uhm_metadata_registered_reader_seq_active_value(seq_after);
}

static int uhm_metadata_registered_reader_active_locked(uhm_heap *heap) {
    uhm_thread_ctx *ctx;
    uhm_tls_cache *cache;
    uhm_slab *slab;
    size_t i;

    if (!heap) {
        return 0;
    }
    if (uhm_metadata_hazard_readers_load(heap) != 0) {
        return 1;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        if (uhm_metadata_reader_active(
                &heap->core.lockless_remote_readers[i])) {
            return 1;
        }
    }
    for (ctx = heap->core.thread_ctx_head; ctx; ctx = ctx->next_active) {
        if (uhm_metadata_reader_active(&ctx->metadata_reader)) {
            return 1;
        }
    }
    for (cache = heap->core.tls_cache_head; cache; cache = cache->next_active) {
        if (uhm_metadata_reader_active(&cache->metadata_reader)) {
            return 1;
        }
    }
    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        if (atomic_load_explicit(&slab->remote_drain_active,
                                 UHM_ATOMIC_ACQUIRE) != 0) {
            return 1;
        }
    }
    return 0;
}

static uhm_metadata_reader_state *uhm_lockless_remote_reader_acquire(
    uhm_heap *heap) {
    size_t i;

    if (!heap || uhm_metadata_hazard_readers_load(heap) == SIZE_MAX) {
        return 0;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        uhm_metadata_reader_state *reader =
            &heap->core.lockless_remote_readers[i];
        if (uhm_metadata_reader_try_begin_inactive(reader, heap)) {
            return reader;
        }
    }
    return 0;
}

static void uhm_lockless_remote_reader_release(
    uhm_metadata_reader_state *reader) {
    (void)uhm_metadata_reader_end(reader);
}

static void uhm_metadata_hot_vma_note_full_defer(uhm_heap *heap);
static void uhm_metadata_hot_free_extent_note_full_defer(uhm_heap *heap);
static void uhm_metadata_hot_vma_clear_refill_defer_if_low(
    uhm_heap *heap);
static void uhm_metadata_hot_free_extent_clear_refill_defer_if_low(
    uhm_heap *heap);
static int uhm_metadata_hot_vma_drain_saturated_with_pending(
    uhm_heap *heap, size_t pending);
static int uhm_metadata_hot_free_extent_drain_saturated_with_pending(
    uhm_heap *heap, size_t pending);

static size_t uhm_metadata_hot_vma_return_batch(uhm_heap *heap,
                                                uhm_vma *head,
                                                uhm_vma *tail,
                                                size_t count) {
    uhm_vma *node;
    uhm_vma *batch_head = 0;
    uhm_vma *batch_tail = 0;
    size_t pushed = 0;

    if (!heap || !head || !tail || count == 0) {
        return 0;
    }
    if (heap->core.hot_vma_count > UHM_METADATA_HOT_VMA_MAX ||
        count > UHM_METADATA_HOT_VMA_MAX - heap->core.hot_vma_count) {
        uhm_metadata_hot_vma_note_full_defer(heap);
        return 0;
    }
    node = head;
    while (pushed < count) {
        uhm_vma *next =
            node != tail && pushed + 1u < count ? node->next : 0;
        memset(node, 0, sizeof(*node));
        if (batch_tail) {
            batch_tail->next = node;
        } else {
            batch_head = node;
        }
        batch_tail = node;
        node = next;
        ++pushed;
    }
    batch_tail->next = heap->core.hot_vma_head;
    heap->core.hot_vma_head = batch_head;
    heap->core.hot_vma_count += pushed;
    uhm_diag_note_u64_add(&heap->core.metadata_hot_vma_reclaimed,
                          (uint64_t)pushed);
    return pushed;
}

static uhm_vma *uhm_metadata_hot_vma_take_batch(uhm_heap *heap,
                                                size_t max_count,
                                                size_t *count_out) {
    uhm_vma *head;
    uhm_vma *node;
    uhm_vma *tail = 0;
    size_t count = 0;

    if (count_out) {
        *count_out = 0;
    }
    if (!heap || max_count == 0) {
        return 0;
    }
    head = heap->core.hot_vma_head;
    node = head;
    while (node && count < max_count) {
        tail = node;
        node = node->next;
        ++count;
    }
    if (count == 0) {
        uhm_diag_note_u64_inc(&heap->core.metadata_hot_vma_misses);
        return 0;
    }
    heap->core.hot_vma_head = node;
    tail->next = 0;
    if (heap->core.hot_vma_count >= count) {
        heap->core.hot_vma_count -= count;
    } else {
        heap->core.hot_vma_count = 0;
    }
    uhm_metadata_hot_vma_clear_refill_defer_if_low(heap);
    uhm_diag_note_u64_add(&heap->core.metadata_hot_vma_hits,
                          (uint64_t)count);
    if (count_out) {
        *count_out = count;
    }
    return head;
}

static uhm_vma *uhm_metadata_hot_vma_take(uhm_heap *heap) {
    size_t count = 0;
    uhm_vma *node = uhm_metadata_hot_vma_take_batch(heap, 1u, &count);

    if (!node || count != 1u) {
        return 0;
    }
    memset(node, 0, sizeof(*node));
    return node;
}

static size_t uhm_metadata_hot_free_extent_return_batch(
    uhm_heap *heap, uhm_free_extent *head, uhm_free_extent *tail,
    size_t count) {
    uhm_free_extent *node;
    uhm_free_extent *batch_head = 0;
    uhm_free_extent *batch_tail = 0;
    size_t pushed = 0;

    if (!heap || !head || !tail || count == 0) {
        return 0;
    }
    if (heap->core.hot_free_extent_count >
            UHM_METADATA_HOT_FREE_EXTENT_MAX ||
        count > UHM_METADATA_HOT_FREE_EXTENT_MAX -
                    heap->core.hot_free_extent_count) {
        uhm_metadata_hot_free_extent_note_full_defer(heap);
        return 0;
    }
    node = head;
    while (pushed < count) {
        uhm_free_extent *next =
            node != tail && pushed + 1u < count ? node->next : 0;
        memset(node, 0, sizeof(*node));
        if (batch_tail) {
            batch_tail->next = node;
        } else {
            batch_head = node;
        }
        batch_tail = node;
        node = next;
        ++pushed;
    }
    batch_tail->next = heap->core.hot_free_extent_head;
    heap->core.hot_free_extent_head = batch_head;
    heap->core.hot_free_extent_count += pushed;
    uhm_diag_note_u64_add(&heap->core.metadata_hot_free_extent_reclaimed,
                          (uint64_t)pushed);
    return pushed;
}

static uhm_free_extent *uhm_metadata_hot_free_extent_take_batch(
    uhm_heap *heap, size_t max_count, size_t *count_out) {
    uhm_free_extent *head;
    uhm_free_extent *node;
    uhm_free_extent *tail = 0;
    size_t count = 0;

    if (count_out) {
        *count_out = 0;
    }
    if (!heap || max_count == 0) {
        return 0;
    }
    head = heap->core.hot_free_extent_head;
    node = head;
    while (node && count < max_count) {
        tail = node;
        node = node->next;
        ++count;
    }
    if (count == 0) {
        uhm_diag_note_u64_inc(&heap->core.metadata_hot_free_extent_misses);
        return 0;
    }
    heap->core.hot_free_extent_head = node;
    tail->next = 0;
    if (heap->core.hot_free_extent_count >= count) {
        heap->core.hot_free_extent_count -= count;
    } else {
        heap->core.hot_free_extent_count = 0;
    }
    uhm_metadata_hot_free_extent_clear_refill_defer_if_low(heap);
    uhm_diag_note_u64_add(&heap->core.metadata_hot_free_extent_hits,
                          (uint64_t)count);
    if (count_out) {
        *count_out = count;
    }
    return head;
}

static uhm_free_extent *uhm_metadata_hot_free_extent_take(uhm_heap *heap) {
    size_t count = 0;
    uhm_free_extent *node =
        uhm_metadata_hot_free_extent_take_batch(heap, 1u, &count);

    if (!node || count != 1u) {
        return 0;
    }
    memset(node, 0, sizeof(*node));
    return node;
}

static void uhm_metadata_hot_vma_note_full_defer(uhm_heap *heap) {
    if (heap) {
        heap->core.metadata_hot_vma_refill_deferred = 1u;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    if (heap && !heap->core.metadata_hot_vma_full_defer_latched) {
        uhm_diag_note_u64_inc(&heap->core.metadata_hot_vma_full_defers);
        heap->core.metadata_hot_vma_full_defer_latched = 1u;
    }
#else
    (void)heap;
#endif
}

static void uhm_metadata_hot_free_extent_note_full_defer(uhm_heap *heap) {
    if (heap) {
        heap->core.metadata_hot_free_extent_refill_deferred = 1u;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    if (heap && !heap->core.metadata_hot_free_extent_full_defer_latched) {
        uhm_diag_note_u64_inc(
            &heap->core.metadata_hot_free_extent_full_defers);
        heap->core.metadata_hot_free_extent_full_defer_latched = 1u;
    }
#else
    (void)heap;
#endif
}

static void uhm_metadata_hot_vma_clear_refill_defer_if_low(
    uhm_heap *heap) {
    if (!heap ||
        heap->core.hot_vma_count >
            UHM_METADATA_HOT_VMA_DRAIN_LOW_WATER) {
        return;
    }
    heap->core.metadata_hot_vma_refill_deferred = 0;
#if UHMAS_ENABLE_DIAGNOSTICS
    heap->core.metadata_hot_vma_full_defer_latched = 0;
#endif
}

static void uhm_metadata_hot_free_extent_clear_refill_defer_if_low(
    uhm_heap *heap) {
    if (!heap ||
        heap->core.hot_free_extent_count >
            UHM_METADATA_HOT_FREE_EXTENT_DRAIN_LOW_WATER) {
        return;
    }
    heap->core.metadata_hot_free_extent_refill_deferred = 0;
#if UHMAS_ENABLE_DIAGNOSTICS
    heap->core.metadata_hot_free_extent_full_defer_latched = 0;
#endif
}

static int uhm_metadata_hot_vma_drain_saturated_with_pending(
    uhm_heap *heap, size_t pending) {
    if (!heap) {
        return 0;
    }
    uhm_metadata_hot_vma_clear_refill_defer_if_low(heap);
    return heap->core.metadata_hot_vma_refill_deferred != 0u ||
           pending >= UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER ||
           heap->core.hot_vma_count >=
               UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER ||
           heap->core.hot_vma_count >=
               UHM_METADATA_HOT_VMA_DRAIN_HIGH_WATER - pending;
}

static int uhm_metadata_hot_vma_drain_saturated(uhm_heap *heap) {
    return uhm_metadata_hot_vma_drain_saturated_with_pending(heap, 0);
}

static int uhm_metadata_hot_free_extent_drain_saturated_with_pending(
    uhm_heap *heap, size_t pending) {
    if (!heap) {
        return 0;
    }
    uhm_metadata_hot_free_extent_clear_refill_defer_if_low(heap);
    return heap->core.metadata_hot_free_extent_refill_deferred != 0u ||
           pending >= UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER ||
           heap->core.hot_free_extent_count >=
               UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER ||
           heap->core.hot_free_extent_count >=
               UHM_METADATA_HOT_FREE_EXTENT_DRAIN_HIGH_WATER - pending;
}

static int uhm_metadata_hot_free_extent_drain_saturated(uhm_heap *heap) {
    return uhm_metadata_hot_free_extent_drain_saturated_with_pending(heap,
                                                                     0);
}

enum {
    UHM_METADATA_QUARANTINE_LIST_EMERGENCY_VMA = 0,
    UHM_METADATA_QUARANTINE_LIST_EMERGENCY_FREE = 1,
    UHM_METADATA_QUARANTINE_LIST_VMA = 2,
    UHM_METADATA_QUARANTINE_LIST_FREE = 3,
    UHM_METADATA_QUARANTINE_LIST_COUNT = 4
};

static void uhm_metadata_quarantine_drain_vma_list(
    uhm_heap *heap, uhm_vma **head, uhm_vma **tail,
    size_t list_count, size_t *emergency_count, int force_emergency_reuse,
    uint64_t safe_epoch, size_t *budget, size_t *scanned,
    size_t *reclaimed, int *budget_exhausted) {
    uhm_vma **link;
    uhm_vma *hot_batch_head = 0;
    uhm_vma *hot_batch_tail = 0;
    size_t hot_batch_count = 0;
    size_t inspected = 0;
    size_t reclaimed_at_entry;

    if (!heap || !head || !tail || !budget || !scanned || !reclaimed ||
        !budget_exhausted) {
        return;
    }
    link = head;
    reclaimed_at_entry = *reclaimed;
    while (*link && *budget != 0 && inspected < list_count) {
        uhm_vma *node = *link;
        uhm_vma *prev = link == head
                            ? 0
                            : (uhm_vma *)((char *)link -
                                          offsetof(uhm_vma, next));
        if (!force_emergency_reuse &&
            (node->flags & UHM_META_FLAG_EMERGENCY) == 0 &&
            uhm_metadata_hot_vma_drain_saturated_with_pending(
                heap, hot_batch_count)) {
            uhm_metadata_hot_vma_note_full_defer(heap);
            break;
        }
        ++inspected;
        --*budget;
        ++*scanned;
        if (node->retire_epoch < safe_epoch &&
            !uhm_metadata_node_is_hazard_locked(heap, node)) {
            int emergency =
                force_emergency_reuse ||
                (node->flags & UHM_META_FLAG_EMERGENCY) != 0;
            if (!emergency &&
                uhm_metadata_hot_vma_drain_saturated_with_pending(
                    heap, hot_batch_count)) {
                uhm_metadata_hot_vma_note_full_defer(heap);
                goto rotate_or_advance;
            }
            *link = node->next;
            if (*tail == node) {
                *tail = prev;
            }
            if (emergency_count && *emergency_count != 0) {
                --*emergency_count;
            }
            if (heap->core.retired_vma_count != 0) {
                --heap->core.retired_vma_count;
            }
            if (emergency) {
                memset(node, 0, sizeof(*node));
                node->flags = UHM_META_FLAG_EMERGENCY;
                node->next = heap->core.emergency_vma_head;
                heap->core.emergency_vma_head = node;
                ++heap->core.emergency_vma_available;
            } else {
                node->next = hot_batch_head;
                hot_batch_head = node;
                if (!hot_batch_tail) {
                    hot_batch_tail = node;
                }
                ++hot_batch_count;
            }
            ++*reclaimed;
            continue;
        }
rotate_or_advance:
        if (node->next && *tail != node) {
            *link = node->next;
            node->next = 0;
            if (*tail) {
                (*tail)->next = node;
            } else {
                *head = node;
            }
            *tail = node;
            uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
                heap, metadata_quarantine_drain_rotated));
            continue;
        }
        link = &node->next;
    }
    if (hot_batch_count != 0) {
        (void)uhm_metadata_hot_vma_return_batch(heap, hot_batch_head,
                                                hot_batch_tail,
                                                hot_batch_count);
    }
    if (*link && *budget == 0 && *reclaimed == reclaimed_at_entry) {
        *budget_exhausted = 1;
    }
}

static void uhm_metadata_quarantine_drain_free_extent_list(
    uhm_heap *heap, uhm_free_extent **head, uhm_free_extent **tail,
    size_t list_count, size_t *emergency_count, int force_emergency_reuse,
    uint64_t safe_epoch, size_t *budget, size_t *scanned,
    size_t *reclaimed, int *budget_exhausted) {
    uhm_free_extent **link;
    uhm_free_extent *hot_batch_head = 0;
    uhm_free_extent *hot_batch_tail = 0;
    size_t hot_batch_count = 0;
    size_t inspected = 0;
    size_t reclaimed_at_entry;

    if (!heap || !head || !tail || !budget || !scanned || !reclaimed ||
        !budget_exhausted) {
        return;
    }
    link = head;
    reclaimed_at_entry = *reclaimed;
    while (*link && *budget != 0 && inspected < list_count) {
        uhm_free_extent *node = *link;
        uhm_free_extent *prev =
            link == head
                ? 0
                : (uhm_free_extent *)((char *)link -
                                      offsetof(uhm_free_extent, next));
        if (!force_emergency_reuse &&
            (node->flags & UHM_META_FLAG_EMERGENCY) == 0 &&
            uhm_metadata_hot_free_extent_drain_saturated_with_pending(
                heap, hot_batch_count)) {
            uhm_metadata_hot_free_extent_note_full_defer(heap);
            break;
        }
        ++inspected;
        --*budget;
        ++*scanned;
        if (node->retire_epoch < safe_epoch &&
            !uhm_metadata_node_is_hazard_locked(heap, node)) {
            int emergency =
                force_emergency_reuse ||
                (node->flags & UHM_META_FLAG_EMERGENCY) != 0;
            if (!emergency &&
                uhm_metadata_hot_free_extent_drain_saturated_with_pending(
                    heap, hot_batch_count)) {
                uhm_metadata_hot_free_extent_note_full_defer(heap);
                goto rotate_or_advance;
            }
            *link = node->next;
            if (*tail == node) {
                *tail = prev;
            }
            if (emergency_count && *emergency_count != 0) {
                --*emergency_count;
            }
            if (heap->core.retired_free_extent_count != 0) {
                --heap->core.retired_free_extent_count;
            }
            if (emergency) {
                memset(node, 0, sizeof(*node));
                node->flags = UHM_META_FLAG_EMERGENCY;
                node->next = heap->core.emergency_free_extent_head;
                heap->core.emergency_free_extent_head = node;
                ++heap->core.emergency_free_extent_available;
            } else {
                node->next = hot_batch_head;
                hot_batch_head = node;
                if (!hot_batch_tail) {
                    hot_batch_tail = node;
                }
                ++hot_batch_count;
            }
            ++*reclaimed;
            continue;
        }
rotate_or_advance:
        if (node->next && *tail != node) {
            *link = node->next;
            node->next = 0;
            if (*tail) {
                (*tail)->next = node;
            } else {
                *head = node;
            }
            *tail = node;
            uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
                heap, metadata_quarantine_drain_rotated));
            continue;
        }
        link = &node->next;
    }
    if (hot_batch_count != 0) {
        (void)uhm_metadata_hot_free_extent_return_batch(
            heap, hot_batch_head, hot_batch_tail, hot_batch_count);
    }
    if (*link && *budget == 0 && *reclaimed == reclaimed_at_entry) {
        *budget_exhausted = 1;
    }
}

static void uhm_metadata_quarantine_drain_one_list(
    uhm_heap *heap, uint32_t list_id, uint64_t safe_epoch, size_t *budget,
    size_t *scanned, size_t *reclaimed, int *budget_exhausted) {
    switch (list_id) {
    case UHM_METADATA_QUARANTINE_LIST_EMERGENCY_VMA:
        uhm_metadata_quarantine_drain_vma_list(
            heap, &heap->core.retired_emergency_vma_head,
            &heap->core.retired_emergency_vma_tail,
            heap->core.retired_emergency_vma_count,
            &heap->core.retired_emergency_vma_count, 1, safe_epoch, budget,
            scanned, reclaimed, budget_exhausted);
        break;
    case UHM_METADATA_QUARANTINE_LIST_EMERGENCY_FREE:
        uhm_metadata_quarantine_drain_free_extent_list(
            heap, &heap->core.retired_emergency_free_extent_head,
            &heap->core.retired_emergency_free_extent_tail,
            heap->core.retired_emergency_free_extent_count,
            &heap->core.retired_emergency_free_extent_count, 1, safe_epoch,
            budget, scanned, reclaimed, budget_exhausted);
        break;
    case UHM_METADATA_QUARANTINE_LIST_VMA:
        if (heap->core.retired_vma_head &&
            heap->core.retired_vma_count >
                heap->core.retired_emergency_vma_count &&
            uhm_metadata_hot_vma_drain_saturated(heap)) {
            uhm_metadata_hot_vma_note_full_defer(heap);
            break;
        }
        uhm_metadata_quarantine_drain_vma_list(
            heap, &heap->core.retired_vma_head, &heap->core.retired_vma_tail,
            heap->core.retired_vma_count >=
                    heap->core.retired_emergency_vma_count
                ? heap->core.retired_vma_count -
                      heap->core.retired_emergency_vma_count
                : 0,
            0, 0, safe_epoch, budget, scanned, reclaimed, budget_exhausted);
        break;
    case UHM_METADATA_QUARANTINE_LIST_FREE:
        if (heap->core.retired_free_extent_head &&
            heap->core.retired_free_extent_count >
                heap->core.retired_emergency_free_extent_count &&
            uhm_metadata_hot_free_extent_drain_saturated(heap)) {
            uhm_metadata_hot_free_extent_note_full_defer(heap);
            break;
        }
        uhm_metadata_quarantine_drain_free_extent_list(
            heap, &heap->core.retired_free_extent_head,
            &heap->core.retired_free_extent_tail,
            heap->core.retired_free_extent_count >=
                    heap->core.retired_emergency_free_extent_count
                ? heap->core.retired_free_extent_count -
                      heap->core.retired_emergency_free_extent_count
                : 0,
            0, 0, safe_epoch, budget, scanned, reclaimed, budget_exhausted);
        break;
    default:
        break;
    }
}

static int uhm_metadata_quarantine_has_pending(const uhm_heap *heap) {
    return heap &&
           (heap->core.retired_vma_head ||
            heap->core.retired_free_extent_head ||
            heap->core.retired_emergency_vma_head ||
            heap->core.retired_emergency_free_extent_head);
}

static int uhm_metadata_reclaim_pending_load(const uhm_heap *heap) {
    return heap &&
           atomic_load_explicit(&heap->core.metadata_reclaim_pending,
                                UHM_ATOMIC_ACQUIRE) != 0;
}

static void uhm_metadata_reclaim_pending_publish(uhm_heap *heap) {
    if (!heap) {
        return;
    }
    atomic_store_explicit(&heap->core.metadata_reclaim_pending,
                          uhm_metadata_quarantine_has_pending(heap) ? 1u : 0u,
                          UHM_ATOMIC_RELEASE);
}

static void uhm_metadata_quarantine_drain(uhm_heap *heap) {
    uint64_t safe_epoch;
    size_t budget = UHM_METADATA_QUARANTINE_DRAIN_NODE_BUDGET;
    size_t scanned = 0;
    size_t reclaimed = 0;
    uint32_t start;
    uint32_t next_start;
    int budget_exhausted = 0;
    size_t i;

    if (!heap) {
        return;
    }
    if (!uhm_metadata_quarantine_has_pending(heap)) {
        uhm_metadata_reclaim_pending_publish(heap);
        return;
    }
    uhm_diag_note_u64_inc(&heap->core.metadata_quarantine_drain_calls);
    safe_epoch = uhm_metadata_safe_epoch_locked(heap);
    if (safe_epoch == 0) {
        return;
    }

    start = heap->core.metadata_quarantine_drain_next_list %
            UHM_METADATA_QUARANTINE_LIST_COUNT;
    next_start = (start + 1u) % UHM_METADATA_QUARANTINE_LIST_COUNT;
    for (i = 0; i < UHM_METADATA_QUARANTINE_LIST_COUNT && budget != 0; ++i) {
        uint32_t list_id =
            (start + (uint32_t)i) % UHM_METADATA_QUARANTINE_LIST_COUNT;
        uhm_metadata_quarantine_drain_one_list(
            heap, list_id, safe_epoch, &budget, &scanned, &reclaimed,
            &budget_exhausted);
        if (budget == 0) {
            next_start = (list_id + 1u) % UHM_METADATA_QUARANTINE_LIST_COUNT;
            break;
        }
    }
    heap->core.metadata_quarantine_drain_next_list = next_start;
    uhm_diag_note_u64_add(&heap->core.metadata_quarantine_drain_scanned,
                          scanned);
    uhm_diag_note_u64_add(&heap->core.metadata_quarantine_drain_reclaimed,
                          reclaimed);
    if (budget_exhausted) {
        uhm_diag_note_u64_inc(
            &heap->core.metadata_quarantine_drain_budget_exhausted);
    }
    uhm_metadata_reclaim_pending_publish(heap);
}

static void uhm_metadata_epoch_advance(uhm_heap *heap) {
    if (!heap) {
        return;
    }
    if (heap->core.metadata_epoch != UINT64_MAX) {
        ++heap->core.metadata_epoch;
    }
    atomic_store_explicit(&heap->core.metadata_published_epoch,
                          heap->core.metadata_epoch, UHM_ATOMIC_RELEASE);
    uhm_metadata_quarantine_drain(heap);
}

static int uhm_metadata_epoch_advance_pinned_try(uhm_heap *heap) {
    int advanced = 0;

    if (!uhm_metadata_reclaim_pending_load(heap) ||
        !uhm_try_lock(&heap->lock)) {
        return 0;
    }
    if (uhm_heap_is_accessible_pinned(heap)) {
        uhm_metadata_epoch_advance(heap);
        advanced = 1;
    }
    uhm_unlock(&heap->lock);
    return advanced;
}

static uhm_vma *uhm_emergency_vma_take(uhm_heap *heap) {
    uhm_vma *node = heap->core.emergency_vma_head;
    if (!node) {
        return 0;
    }
    heap->core.emergency_vma_head = node->next;
    if (heap->core.emergency_vma_available != 0) {
        --heap->core.emergency_vma_available;
    }
    memset(node, 0, sizeof(*node));
    node->flags = UHM_META_FLAG_EMERGENCY;
    return node;
}

static void uhm_metadata_note_retired_tail_corruption(uhm_heap *heap,
                                                      uint32_t site) {
    uhm_debug_corruption_site(heap, site);
    uhm_set_error(heap, UHM_ERR_CORRUPTION);
}

static void uhm_retired_vma_append_node(uhm_heap *heap, uhm_vma **head,
                                        uhm_vma **tail, uhm_vma *node) {
    if (!head || !tail || !node) {
        return;
    }
    node->next = 0;
    if (!*head) {
        *head = node;
        *tail = node;
        return;
    }
    if (!*tail) {
        uhm_metadata_note_retired_tail_corruption(
            heap, UHM_CORRUPTION_SITE_RETIRED_VMA_TAIL);
        node->next = *head;
        *head = node;
        return;
    }
    (*tail)->next = node;
    *tail = node;
}

static void uhm_vma_release_node(uhm_heap *heap, uhm_vma *node) {
    int emergency;

    if (!node) {
        return;
    }
    emergency = (node->flags & UHM_META_FLAG_EMERGENCY) != 0;
    uhm_extent_descriptor_retire(&node->desc);
    node->magic = 0;
    node->prev = 0;
    node->tree_parent = 0;
    node->tree_left = 0;
    node->tree_right = 0;
    node->tree_priority = 0;
    node->retire_epoch = heap ? heap->core.metadata_epoch : 0;
    if (emergency) {
        node->next = heap->core.retired_emergency_vma_head;
        heap->core.retired_emergency_vma_head = node;
        if (!heap->core.retired_emergency_vma_tail) {
            heap->core.retired_emergency_vma_tail = node;
        }
        ++heap->core.retired_emergency_vma_count;
    } else {
        uhm_retired_vma_append_node(heap, &heap->core.retired_vma_head,
                                    &heap->core.retired_vma_tail, node);
    }
    ++heap->core.retired_vma_count;
    uhm_metadata_reclaim_pending_publish(heap);
}

static uhm_free_extent *uhm_emergency_free_extent_take(uhm_heap *heap) {
    uhm_free_extent *node = heap->core.emergency_free_extent_head;
    if (!node) {
        return 0;
    }
    heap->core.emergency_free_extent_head = node->next;
    if (heap->core.emergency_free_extent_available != 0) {
        --heap->core.emergency_free_extent_available;
    }
    memset(node, 0, sizeof(*node));
    node->flags = UHM_META_FLAG_EMERGENCY;
    return node;
}

static void uhm_retired_free_extent_append_node(uhm_heap *heap,
                                                uhm_free_extent **head,
                                                uhm_free_extent **tail,
                                                uhm_free_extent *node) {
    if (!head || !tail || !node) {
        return;
    }
    node->next = 0;
    if (!*head) {
        *head = node;
        *tail = node;
        return;
    }
    if (!*tail) {
        uhm_metadata_note_retired_tail_corruption(
            heap, UHM_CORRUPTION_SITE_RETIRED_FREE_TAIL);
        node->next = *head;
        *head = node;
        return;
    }
    (*tail)->next = node;
    *tail = node;
}

static void uhm_free_extent_release_node(uhm_heap *heap, uhm_free_extent *node) {
    int emergency;

    if (!node) {
        return;
    }
    uhm_mmap_recent_free_forget(heap, node);
    emergency = (node->flags & UHM_META_FLAG_EMERGENCY) != 0;
    uhm_extent_descriptor_retire(&node->desc);
    node->magic = 0;
    node->flags = emergency ? UHM_META_FLAG_EMERGENCY : 0u;
    node->prev = 0;
    node->tree_parent = 0;
    node->tree_left = 0;
    node->tree_right = 0;
    node->tree_max_size = 0;
    node->tree_priority = 0;
    node->retire_epoch = heap ? heap->core.metadata_epoch : 0;
    if (emergency) {
        node->next = heap->core.retired_emergency_free_extent_head;
        heap->core.retired_emergency_free_extent_head = node;
        if (!heap->core.retired_emergency_free_extent_tail) {
            heap->core.retired_emergency_free_extent_tail = node;
        }
        ++heap->core.retired_emergency_free_extent_count;
    } else {
        uhm_retired_free_extent_append_node(
            heap, &heap->core.retired_free_extent_head,
            &heap->core.retired_free_extent_tail, node);
    }
    ++heap->core.retired_free_extent_count;
    uhm_metadata_reclaim_pending_publish(heap);
}

