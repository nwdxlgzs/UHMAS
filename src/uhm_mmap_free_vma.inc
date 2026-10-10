static void uhm_mmap_free_tree_insert(uhm_heap *heap, uhm_free_extent *node);
static void uhm_mmap_free_tree_remove(uhm_heap *heap, uhm_free_extent *node);
static void uhm_mmap_free_tree_update_up(uhm_free_extent *node);
static uhm_free_extent *uhm_mmap_free_tree_find_highest_fit(uhm_heap *heap,
                                                            uhm_free_extent *node,
                                                            size_t size);
static int uhm_mmap_free_tree_find_neighbors_by_addr(
    uhm_heap *heap, uintptr_t addr, uhm_free_extent **left_out,
    uhm_free_extent **right_out);
static uhm_result uhm_mmap_free_merge_forward(uhm_heap *heap,
                                              uhm_free_extent *node,
                                              int fresh_node);

#ifndef UHMAS_MMAP_FRESH_EXTENT_NOTE_FAST_PATH
#define UHMAS_MMAP_FRESH_EXTENT_NOTE_FAST_PATH 1
#endif

#ifndef UHMAS_MMAP_HOT_CACHE_PROMOTE_FAST_PATH
#define UHMAS_MMAP_HOT_CACHE_PROMOTE_FAST_PATH 0
#endif

#ifndef UHMAS_MMAP_EXACT_FIT_EARLY_EXIT
#define UHMAS_MMAP_EXACT_FIT_EARLY_EXIT 1
#endif

#ifndef UHMAS_MMAP_CACHE_BOUNDED_MAINTENANCE
#define UHMAS_MMAP_CACHE_BOUNDED_MAINTENANCE 1
#endif

static uhm_free_extent *uhm_free_extent_new(uhm_heap *heap, uintptr_t addr, size_t size,
                                             int allow_emergency) {
    uhm_free_extent *node = uhm_metadata_hot_free_extent_take(heap);
    int emergency = 0;

    if (!node) {
        node = (uhm_free_extent *)uhm_control_alloc_uninitialized(
            heap, sizeof(*node), alignof(uhm_free_extent));
    }
    if (!node && allow_emergency) {
        node = uhm_emergency_free_extent_take(heap);
        emergency = node != 0;
    }
    if (!node) {
        return 0;
    }
    node->magic = UHM_FREE_MAGIC;
    node->flags = emergency ? UHM_META_FLAG_EMERGENCY : 0u;
    node->addr = addr;
    node->size = size;
    node->retire_epoch = 0;
    node->prev = 0;
    node->next = 0;
    node->tree_parent = 0;
    node->tree_left = 0;
    node->tree_right = 0;
    node->tree_max_size = size;
    node->tree_priority =
        uhm_tree_priority_mix_for_heap(
            heap, (uint64_t)(uintptr_t)node,
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_priority_mix_calls),
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_priority_mix_ns),
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_priority_mix_max_ns));
    uhm_extent_descriptor_set(&node->desc, UHM_EXTENT_KIND_MMAP_FREE,
                              addr, size, UHM_PAGE_OWNER_FREE,
                              UHM_PAGE_VA_FREE_REUSABLE,
                              UHM_PAGE_COMMIT_DECOMMITTED,
                              UHM_PROT_NONE, uhm_heap_generation_next(heap), node);
    return node;
}

static void uhm_free_extent_descriptor_sync(uhm_heap *heap,
                                            uhm_free_extent *node) {
    if (!node) {
        return;
    }
    uhm_extent_descriptor_set(&node->desc, UHM_EXTENT_KIND_MMAP_FREE,
                              node->addr, node->size,
                              UHM_PAGE_OWNER_FREE,
                              UHM_PAGE_VA_FREE_REUSABLE,
                              UHM_PAGE_COMMIT_DECOMMITTED,
                              UHM_PROT_NONE, uhm_heap_generation_next(heap), node);
    uhm_mmap_free_tree_update_up(node);
}

static int uhm_mmap_free_list_contains_exact(uhm_free_extent *head,
                                             const uhm_free_extent *needle) {
    for (; head; head = head->next) {
        if (head == needle) {
            return 1;
        }
    }
    return 0;
}

static int uhm_mmap_free_extent_is_listed(const uhm_free_extent *extent) {
    return extent && extent->magic == UHM_FREE_MAGIC && extent->size != 0 &&
           (extent->flags & UHM_META_FLAG_IN_MMAP_FREE_LIST) != 0;
}

static int uhm_mmap_free_list_unlink_node(uhm_heap *heap,
                                          uhm_free_extent *node) {
    if (!heap || !uhm_mmap_free_extent_is_listed(node)) {
        return 0;
    }
    if (node->prev) {
        if (node->prev->next != node) {
            return 0;
        }
        node->prev->next = node->next;
    } else {
        if (heap->core.mmap_free_head != node) {
            return 0;
        }
        heap->core.mmap_free_head = node->next;
    }
    if (node->next) {
        if (node->next->prev != node) {
            return 0;
        }
        node->next->prev = node->prev;
    }
    node->prev = 0;
    node->next = 0;
    node->flags &= ~UHM_META_FLAG_IN_MMAP_FREE_LIST;
    return 1;
}

static size_t uhm_mmap_size_hint_bucket(uhm_heap *heap, size_t size) {
    size_t page_size;
    size_t pages;
    size_t bucket = 0;

    if (!heap || size == 0) {
        return 0;
    }
    page_size = heap->F.vm.page_size != 0 ? heap->F.vm.page_size : 1u;
    pages = ((size - 1u) / page_size) + 1u;
    if (pages == 0) {
        pages = 1u;
    }
    --pages;
    while (pages != 0 && bucket + 1u < UHM_MMAP_SIZE_HINT_BUCKETS) {
        pages >>= 1u;
        ++bucket;
    }
    return bucket;
}

static void uhm_mmap_size_hint_clear_position(uhm_free_extent *extent) {
    if (!extent) {
        return;
    }
    extent->flags &= ~UHM_FREE_EXTENT_SIZE_HINT_MASK;
}

static void uhm_mmap_size_hint_set_position(uhm_free_extent *extent,
                                            size_t bucket, size_t slot) {
    uint32_t flags;

    if (!extent || bucket >= UHM_MMAP_SIZE_HINT_BUCKETS ||
        slot >= UHM_MMAP_SIZE_HINT_SLOTS) {
        return;
    }
    flags = extent->flags & ~UHM_FREE_EXTENT_SIZE_HINT_MASK;
    flags |= UHM_FREE_EXTENT_FLAG_IN_MMAP_SIZE_HINT;
    flags |= ((uint32_t)bucket & UHM_FREE_EXTENT_SIZE_HINT_INDEX_MASK)
             << UHM_FREE_EXTENT_SIZE_HINT_BUCKET_SHIFT;
    flags |= ((uint32_t)slot & UHM_FREE_EXTENT_SIZE_HINT_INDEX_MASK)
             << UHM_FREE_EXTENT_SIZE_HINT_SLOT_SHIFT;
    extent->flags = flags;
}

static int uhm_mmap_size_hint_position(const uhm_free_extent *extent,
                                       size_t *bucket_out,
                                       size_t *slot_out) {
    size_t bucket;
    size_t slot;

    if (!extent ||
        (extent->flags & UHM_FREE_EXTENT_FLAG_IN_MMAP_SIZE_HINT) == 0) {
        return 0;
    }
    bucket = (size_t)((extent->flags &
                       UHM_FREE_EXTENT_SIZE_HINT_BUCKET_MASK) >>
                      UHM_FREE_EXTENT_SIZE_HINT_BUCKET_SHIFT);
    slot = (size_t)((extent->flags & UHM_FREE_EXTENT_SIZE_HINT_SLOT_MASK) >>
                    UHM_FREE_EXTENT_SIZE_HINT_SLOT_SHIFT);
    if (bucket >= UHM_MMAP_SIZE_HINT_BUCKETS ||
        slot >= UHM_MMAP_SIZE_HINT_SLOTS) {
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

static int uhm_mmap_size_hint_position_matches(
    uhm_heap *heap, const uhm_free_extent *extent) {
    size_t bucket;
    size_t slot;

    if (!heap || !uhm_mmap_size_hint_position(extent, &bucket, &slot)) {
        return 0;
    }
    return heap->core.mmap_size_hint[bucket][slot] == extent;
}

static void uhm_mmap_size_hint_recompute_bucket(uhm_heap *heap,
                                                 size_t bucket) {
    size_t slot;
    size_t max_size = 0;
    uhm_free_extent *max_extent = 0;

    if (!heap || bucket >= UHM_MMAP_SIZE_HINT_BUCKETS) {
        return;
    }
    for (slot = 0; slot < UHM_MMAP_SIZE_HINT_SLOTS; ++slot) {
        uhm_free_extent *extent = heap->core.mmap_size_hint[bucket][slot];
        if (extent && uhm_mmap_free_extent_is_listed(extent) &&
            extent->size > max_size) {
            max_size = extent->size;
            max_extent = extent;
        }
    }
    heap->core.mmap_size_hint_bucket_max[bucket] = max_size;
    heap->core.mmap_size_hint_bucket_max_extent[bucket] = max_extent;
}

static uhm_free_extent *uhm_mmap_size_hint_exact_max_candidate(
    uhm_heap *heap, size_t bucket, size_t size) {
    uhm_free_extent *extent;

    if (!heap || bucket >= UHM_MMAP_SIZE_HINT_BUCKETS || size == 0 ||
        heap->core.mmap_size_hint_bucket_max[bucket] != size) {
        return 0;
    }
    extent = heap->core.mmap_size_hint_bucket_max_extent[bucket];
    if (!extent || !uhm_mmap_free_extent_is_listed(extent) ||
        extent->size != size ||
        uhm_mmap_size_hint_bucket(heap, extent->size) != bucket ||
        !uhm_mmap_size_hint_position_matches(heap, extent)) {
        return 0;
    }
    return extent;
}

static void uhm_mmap_size_hint_compact_bucket(uhm_heap *heap,
                                              size_t bucket) {
    size_t read_slot;
    size_t write_slot = 0;

    if (!heap || bucket >= UHM_MMAP_SIZE_HINT_BUCKETS) {
        return;
    }
    for (read_slot = 0; read_slot < UHM_MMAP_SIZE_HINT_SLOTS; ++read_slot) {
        uhm_free_extent *extent = heap->core.mmap_size_hint[bucket][read_slot];
        if (!extent) {
            continue;
        }
        if (write_slot != read_slot) {
            heap->core.mmap_size_hint[bucket][write_slot] = extent;
            heap->core.mmap_size_hint[bucket][read_slot] = 0;
        }
        if (extent->magic == UHM_FREE_MAGIC) {
            uhm_mmap_size_hint_set_position(extent, bucket, write_slot);
        }
        ++write_slot;
    }
    while (write_slot < UHM_MMAP_SIZE_HINT_SLOTS) {
        heap->core.mmap_size_hint[bucket][write_slot] = 0;
        ++write_slot;
    }
    uhm_mmap_size_hint_recompute_bucket(heap, bucket);
}

static void uhm_mmap_size_hint_remove_at(uhm_heap *heap, size_t bucket,
                                         size_t slot) {
    size_t i;
    uhm_free_extent *removed;

    if (!heap || bucket >= UHM_MMAP_SIZE_HINT_BUCKETS ||
        slot >= UHM_MMAP_SIZE_HINT_SLOTS) {
        return;
    }
    removed = heap->core.mmap_size_hint[bucket][slot];
    if (removed && removed->magic == UHM_FREE_MAGIC) {
        uhm_mmap_size_hint_clear_position(removed);
    }
#if UHMAS_MMAP_CACHE_BOUNDED_MAINTENANCE
    for (i = slot; i + 1u < UHM_MMAP_SIZE_HINT_SLOTS; ++i) {
        uhm_free_extent *moved = heap->core.mmap_size_hint[bucket][i + 1u];
        heap->core.mmap_size_hint[bucket][i] = moved;
        if (!moved) {
            uhm_mmap_size_hint_recompute_bucket(heap, bucket);
            return;
        }
        if (moved->magic == UHM_FREE_MAGIC) {
            uhm_mmap_size_hint_set_position(moved, bucket, i);
        }
    }
#else
    for (i = slot; i + 1u < UHM_MMAP_SIZE_HINT_SLOTS; ++i) {
        uhm_free_extent *moved = heap->core.mmap_size_hint[bucket][i + 1u];
        heap->core.mmap_size_hint[bucket][i] = moved;
        if (moved && moved->magic == UHM_FREE_MAGIC) {
            uhm_mmap_size_hint_set_position(moved, bucket, i);
        }
    }
#endif
    heap->core.mmap_size_hint[bucket][UHM_MMAP_SIZE_HINT_SLOTS - 1u] = 0;
    uhm_mmap_size_hint_recompute_bucket(heap, bucket);
}

static void uhm_mmap_size_hint_forget_bucket(uhm_heap *heap,
                                             size_t bucket,
                                             const uhm_free_extent *extent) {
    size_t slot;
    size_t pos_bucket;
    size_t pos_slot;
    int removed = 0;

    if (!heap || !extent || bucket >= UHM_MMAP_SIZE_HINT_BUCKETS) {
        return;
    }
    if (extent->magic == UHM_FREE_MAGIC &&
        uhm_mmap_size_hint_position(extent, &pos_bucket, &pos_slot) &&
        heap->core.mmap_size_hint[pos_bucket][pos_slot] == extent) {
        uhm_mmap_size_hint_remove_at(heap, pos_bucket, pos_slot);
        return;
    }
    for (slot = 0; slot < UHM_MMAP_SIZE_HINT_SLOTS; ++slot) {
        if (heap->core.mmap_size_hint[bucket][slot] == extent) {
            heap->core.mmap_size_hint[bucket][slot] = 0;
            removed = 1;
        }
    }
    if (removed) {
        uhm_mmap_size_hint_compact_bucket(heap, bucket);
    }
    if (extent->magic == UHM_FREE_MAGIC) {
        uhm_mmap_size_hint_clear_position((uhm_free_extent *)extent);
    }
}

static void uhm_mmap_size_hint_forget(uhm_heap *heap,
                                      const uhm_free_extent *extent) {
    size_t i;
    size_t slot;

    if (!heap || !extent) {
        return;
    }
    if (extent->magic == UHM_FREE_MAGIC && extent->size != 0) {
        uhm_mmap_size_hint_forget_bucket(
            heap, uhm_mmap_size_hint_bucket(heap, extent->size), extent);
        return;
    }
    for (i = 0; i < UHM_MMAP_SIZE_HINT_BUCKETS; ++i) {
        int removed = 0;
        for (slot = 0; slot < UHM_MMAP_SIZE_HINT_SLOTS; ++slot) {
            if (heap->core.mmap_size_hint[i][slot] == extent) {
                heap->core.mmap_size_hint[i][slot] = 0;
                removed = 1;
            }
        }
        if (removed) {
            uhm_mmap_size_hint_compact_bucket(heap, i);
        }
    }
    if (extent->magic == UHM_FREE_MAGIC) {
        uhm_mmap_size_hint_clear_position((uhm_free_extent *)extent);
    }
}

static void uhm_mmap_size_hint_forget_current_if_bucket_changes(
    uhm_heap *heap, const uhm_free_extent *extent, size_t new_size) {
    size_t old_bucket;
    size_t new_bucket;

    if (!heap || !extent || extent->size == 0 || new_size == 0) {
        return;
    }
    old_bucket = uhm_mmap_size_hint_bucket(heap, extent->size);
    new_bucket = uhm_mmap_size_hint_bucket(heap, new_size);
    if (old_bucket != new_bucket) {
        uhm_mmap_size_hint_forget_bucket(heap, old_bucket, extent);
    }
}

static void uhm_mmap_size_hint_push(uhm_heap *heap,
                                    uhm_free_extent *extent,
                                    size_t bucket) {
    size_t slot;
#if UHMAS_MMAP_CACHE_BOUNDED_MAINTENANCE
    uhm_free_extent *incoming = extent;
    uhm_free_extent *dropped =
        heap->core.mmap_size_hint[bucket][UHM_MMAP_SIZE_HINT_SLOTS - 1u];

    if (dropped) {
        if (dropped->magic == UHM_FREE_MAGIC) {
            uhm_mmap_size_hint_clear_position(dropped);
        }
        for (slot = UHM_MMAP_SIZE_HINT_SLOTS - 1u; slot > 0; --slot) {
            uhm_free_extent *moved =
                heap->core.mmap_size_hint[bucket][slot - 1u];
            heap->core.mmap_size_hint[bucket][slot] = moved;
            if (moved && moved->magic == UHM_FREE_MAGIC) {
                uhm_mmap_size_hint_set_position(moved, bucket, slot);
            }
        }
        heap->core.mmap_size_hint[bucket][0] = extent;
        uhm_mmap_size_hint_set_position(extent, bucket, 0);
        uhm_mmap_size_hint_recompute_bucket(heap, bucket);
        return;
    }
    for (slot = 0; slot < UHM_MMAP_SIZE_HINT_SLOTS; ++slot) {
        uhm_free_extent *moved = heap->core.mmap_size_hint[bucket][slot];
        heap->core.mmap_size_hint[bucket][slot] = incoming;
        if (incoming && incoming->magic == UHM_FREE_MAGIC) {
            uhm_mmap_size_hint_set_position(incoming, bucket, slot);
        }
        incoming = moved;
        if (!incoming) {
            uhm_mmap_size_hint_recompute_bucket(heap, bucket);
            return;
        }
    }
    if (incoming->magic == UHM_FREE_MAGIC) {
        uhm_mmap_size_hint_clear_position(incoming);
    }
    uhm_mmap_size_hint_recompute_bucket(heap, bucket);
#else
    uhm_free_extent *dropped;

    dropped = heap->core.mmap_size_hint[bucket][UHM_MMAP_SIZE_HINT_SLOTS - 1u];
    if (dropped && dropped->magic == UHM_FREE_MAGIC) {
        uhm_mmap_size_hint_clear_position(dropped);
    }
    for (slot = UHM_MMAP_SIZE_HINT_SLOTS - 1u; slot > 0; --slot) {
        uhm_free_extent *moved = heap->core.mmap_size_hint[bucket][slot - 1u];
        heap->core.mmap_size_hint[bucket][slot] = moved;
        if (moved && moved->magic == UHM_FREE_MAGIC) {
            uhm_mmap_size_hint_set_position(moved, bucket, slot);
        }
    }
    heap->core.mmap_size_hint[bucket][0] = extent;
    uhm_mmap_size_hint_set_position(extent, bucket, 0);
    uhm_mmap_size_hint_recompute_bucket(heap, bucket);
#endif
}

#if UHMAS_MMAP_HOT_CACHE_PROMOTE_FAST_PATH
static int uhm_mmap_size_hint_promote_existing(uhm_heap *heap,
                                               uhm_free_extent *extent,
                                               size_t bucket) {
    size_t position_bucket;
    size_t position_slot;
    size_t slot;

    if (!heap || !extent ||
        !uhm_mmap_size_hint_position(extent, &position_bucket,
                                     &position_slot) ||
        position_bucket != bucket ||
        heap->core.mmap_size_hint[position_bucket][position_slot] != extent) {
        return 0;
    }
    if (position_slot == 0) {
        return 1;
    }
    for (slot = position_slot; slot > 0; --slot) {
        uhm_free_extent *moved =
            heap->core.mmap_size_hint[position_bucket][slot - 1u];
        heap->core.mmap_size_hint[position_bucket][slot] = moved;
        if (moved && moved->magic == UHM_FREE_MAGIC) {
            uhm_mmap_size_hint_set_position(moved, position_bucket, slot);
        }
    }
    heap->core.mmap_size_hint[position_bucket][0] = extent;
    uhm_mmap_size_hint_set_position(extent, position_bucket, 0);
    return 1;
}
#endif

static void uhm_mmap_size_hint_note(uhm_heap *heap,
                                    uhm_free_extent *extent) {
    size_t bucket;

    if (!heap || !extent || extent->magic != UHM_FREE_MAGIC ||
        extent->size == 0) {
        return;
    }
    bucket = uhm_mmap_size_hint_bucket(heap, extent->size);
    uhm_mmap_size_hint_forget_bucket(heap, bucket, extent);
    uhm_mmap_size_hint_push(heap, extent, bucket);
}

static void uhm_mmap_size_hint_note_fresh(uhm_heap *heap,
                                          uhm_free_extent *extent) {
#if UHMAS_MMAP_FRESH_EXTENT_NOTE_FAST_PATH
    if (heap && extent && extent->magic == UHM_FREE_MAGIC &&
        extent->size != 0 &&
        (extent->flags & UHM_FREE_EXTENT_CACHE_POSITION_MASK) == 0) {
        uhm_mmap_size_hint_push(
            heap, extent, uhm_mmap_size_hint_bucket(heap, extent->size));
        return;
    }
#endif
    uhm_mmap_size_hint_note(heap, extent);
}

static void uhm_mmap_recent_free_clear_position(uhm_free_extent *extent) {
    if (!extent) {
        return;
    }
    extent->flags &= ~UHM_FREE_EXTENT_RECENT_MASK;
}

static void uhm_mmap_recent_free_set_position(uhm_free_extent *extent,
                                              size_t slot) {
    uint32_t flags;

    if (!extent || slot >= UHM_MMAP_RECENT_FREE_CACHE_SLOTS) {
        return;
    }
    flags = extent->flags & ~UHM_FREE_EXTENT_RECENT_MASK;
    flags |= UHM_FREE_EXTENT_FLAG_IN_MMAP_RECENT;
    flags |= ((uint32_t)slot & UHM_FREE_EXTENT_RECENT_SLOT_INDEX_MASK)
             << UHM_FREE_EXTENT_RECENT_SLOT_SHIFT;
    extent->flags = flags;
}

static int uhm_mmap_recent_free_position(const uhm_free_extent *extent,
                                         size_t *slot_out) {
    size_t slot;

    if (!extent ||
        (extent->flags & UHM_FREE_EXTENT_FLAG_IN_MMAP_RECENT) == 0) {
        return 0;
    }
    slot = (size_t)((extent->flags & UHM_FREE_EXTENT_RECENT_SLOT_MASK) >>
                    UHM_FREE_EXTENT_RECENT_SLOT_SHIFT);
    if (slot >= UHM_MMAP_RECENT_FREE_CACHE_SLOTS) {
        return 0;
    }
    if (slot_out) {
        *slot_out = slot;
    }
    return 1;
}

static int uhm_mmap_recent_free_position_matches(
    uhm_heap *heap, const uhm_free_extent *extent) {
    size_t slot;

    if (!heap || !uhm_mmap_recent_free_position(extent, &slot)) {
        return 0;
    }
    return heap->core.mmap_recent_free_cache[slot] == extent;
}

static size_t uhm_mmap_recent_free_used_slots(uhm_heap *heap) {
    size_t used = 0;

    if (!heap) {
        return 0;
    }
    while (used < UHM_MMAP_RECENT_FREE_CACHE_SLOTS &&
           heap->core.mmap_recent_free_cache[used]) {
        ++used;
    }
    heap->core.mmap_recent_free_next = used;
    return used;
}

static size_t uhm_mmap_recent_free_compact(uhm_heap *heap) {
    size_t read_slot;
    size_t write_slot = 0;

    if (!heap) {
        return 0;
    }
    for (read_slot = 0; read_slot < UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
         ++read_slot) {
        uhm_free_extent *extent =
            heap->core.mmap_recent_free_cache[read_slot];
        if (!extent) {
            continue;
        }
        if (write_slot != read_slot) {
            heap->core.mmap_recent_free_cache[write_slot] = extent;
            heap->core.mmap_recent_free_cache[read_slot] = 0;
        }
        if (extent->magic == UHM_FREE_MAGIC) {
            uhm_mmap_recent_free_set_position(extent, write_slot);
        }
        ++write_slot;
    }
    {
        size_t used = write_slot;
        while (write_slot < UHM_MMAP_RECENT_FREE_CACHE_SLOTS) {
            heap->core.mmap_recent_free_cache[write_slot] = 0;
            ++write_slot;
        }
        heap->core.mmap_recent_free_next =
            used < UHM_MMAP_RECENT_FREE_CACHE_SLOTS
                ? used
                : UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
        return used;
    }
}

static size_t uhm_mmap_recent_free_remove_at(uhm_heap *heap, size_t slot) {
    size_t i;
    size_t used;
    uhm_free_extent *removed;

    if (!heap || slot >= UHM_MMAP_RECENT_FREE_CACHE_SLOTS) {
        return 0;
    }
#if UHMAS_MMAP_CACHE_BOUNDED_MAINTENANCE
    used = heap->core.mmap_recent_free_next;
    if (used > UHM_MMAP_RECENT_FREE_CACHE_SLOTS || slot >= used) {
        used = uhm_mmap_recent_free_used_slots(heap);
        if (slot >= used) {
            return used;
        }
    }
#else
    used = UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
#endif
    removed = heap->core.mmap_recent_free_cache[slot];
    if (removed && removed->magic == UHM_FREE_MAGIC) {
        uhm_mmap_recent_free_clear_position(removed);
    }
    if (heap->core.mmap_recent_free_extent == removed) {
        heap->core.mmap_recent_free_extent = 0;
    }
    for (i = slot; i + 1u < used; ++i) {
        uhm_free_extent *moved = heap->core.mmap_recent_free_cache[i + 1u];
        heap->core.mmap_recent_free_cache[i] = moved;
        if (moved && moved->magic == UHM_FREE_MAGIC) {
            uhm_mmap_recent_free_set_position(moved, i);
        }
    }
#if UHMAS_MMAP_CACHE_BOUNDED_MAINTENANCE
    heap->core.mmap_recent_free_cache[used - 1u] = 0;
    heap->core.mmap_recent_free_next = used - 1u;
    return used - 1u;
#else
    heap->core.mmap_recent_free_cache[UHM_MMAP_RECENT_FREE_CACHE_SLOTS - 1u] =
        0;
    return uhm_mmap_recent_free_used_slots(heap);
#endif
}

#if UHMAS_MMAP_HOT_CACHE_PROMOTE_FAST_PATH
static int uhm_mmap_recent_free_promote_existing(uhm_heap *heap,
                                                  uhm_free_extent *extent) {
    size_t position_slot;
    size_t slot;

    if (!heap || !extent ||
        !uhm_mmap_recent_free_position(extent, &position_slot) ||
        heap->core.mmap_recent_free_cache[position_slot] != extent) {
        return 0;
    }
    heap->core.mmap_recent_free_extent = extent;
    if (position_slot == 0) {
        return 1;
    }
    for (slot = position_slot; slot > 0; --slot) {
        uhm_free_extent *moved =
            heap->core.mmap_recent_free_cache[slot - 1u];
        heap->core.mmap_recent_free_cache[slot] = moved;
        if (moved && moved->magic == UHM_FREE_MAGIC) {
            uhm_mmap_recent_free_set_position(moved, slot);
        }
    }
    heap->core.mmap_recent_free_cache[0] = extent;
    uhm_mmap_recent_free_set_position(extent, 0);
    return 1;
}
#endif

static size_t uhm_mmap_recent_free_forget_cache_entry(
    uhm_heap *heap, const uhm_free_extent *extent) {
    size_t i;
    size_t pos_slot;
    int removed = 0;

    if (!heap || !extent) {
        return 0;
    }
    if (heap->core.mmap_recent_free_extent == extent) {
        heap->core.mmap_recent_free_extent = 0;
    }
    if (extent->magic == UHM_FREE_MAGIC &&
        uhm_mmap_recent_free_position(extent, &pos_slot) &&
        heap->core.mmap_recent_free_cache[pos_slot] == extent) {
        return uhm_mmap_recent_free_remove_at(heap, pos_slot);
    }
    for (i = 0; i < UHM_MMAP_RECENT_FREE_CACHE_SLOTS; ++i) {
        if (heap->core.mmap_recent_free_cache[i] == extent) {
            heap->core.mmap_recent_free_cache[i] = 0;
            removed = 1;
        }
    }
    if (removed) {
        (void)uhm_mmap_recent_free_compact(heap);
    }
    if (extent->magic == UHM_FREE_MAGIC) {
        uhm_mmap_recent_free_clear_position((uhm_free_extent *)extent);
    }
    return heap->core.mmap_recent_free_next;
}

static void uhm_mmap_recent_free_forget(uhm_heap *heap,
                                        const uhm_free_extent *extent) {
    if (!heap || !extent) {
        return;
    }
    uhm_mmap_size_hint_forget(heap, extent);
    (void)uhm_mmap_recent_free_forget_cache_entry(heap, extent);
}

static void uhm_mmap_recent_free_push(uhm_heap *heap,
                                      uhm_free_extent *extent,
                                      size_t count);

static void uhm_mmap_recent_free_note(uhm_heap *heap,
                                      uhm_free_extent *extent) {
    size_t count;

    if (!heap || !extent || extent->magic != UHM_FREE_MAGIC) {
        return;
    }
    uhm_mmap_size_hint_note(heap, extent);
    count = uhm_mmap_recent_free_forget_cache_entry(heap, extent);
    uhm_mmap_recent_free_push(heap, extent, count);
}

static void uhm_mmap_recent_free_note_fresh(uhm_heap *heap,
                                            uhm_free_extent *extent) {
#if UHMAS_MMAP_FRESH_EXTENT_NOTE_FAST_PATH
    if (heap && extent && extent->magic == UHM_FREE_MAGIC &&
        (extent->flags & UHM_FREE_EXTENT_CACHE_POSITION_MASK) == 0) {
        uhm_mmap_size_hint_note_fresh(heap, extent);
        uhm_mmap_recent_free_push(
            heap, extent, heap->core.mmap_recent_free_next);
        return;
    }
#endif
    uhm_mmap_recent_free_note(heap, extent);
}

static void uhm_mmap_recent_free_push(uhm_heap *heap,
                                      uhm_free_extent *extent,
                                      size_t count) {
    size_t i;
#if UHMAS_MMAP_CACHE_BOUNDED_MAINTENANCE
    size_t move_count;
#endif

    heap->core.mmap_recent_free_extent = extent;
    if (count > UHM_MMAP_RECENT_FREE_CACHE_SLOTS) {
        count = UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
    }
    if (heap->core.mmap_recent_free_cache[UHM_MMAP_RECENT_FREE_CACHE_SLOTS -
                                          1u]) {
        uhm_free_extent *dropped =
            heap->core.mmap_recent_free_cache
                [UHM_MMAP_RECENT_FREE_CACHE_SLOTS - 1u];
        if (dropped->magic == UHM_FREE_MAGIC) {
            uhm_mmap_recent_free_clear_position(dropped);
        }
    }
#if UHMAS_MMAP_CACHE_BOUNDED_MAINTENANCE
    move_count = count < UHM_MMAP_RECENT_FREE_CACHE_SLOTS
                     ? count
                     : UHM_MMAP_RECENT_FREE_CACHE_SLOTS - 1u;
    for (i = move_count; i > 0; --i) {
#else
    for (i = UHM_MMAP_RECENT_FREE_CACHE_SLOTS - 1u; i > 0; --i) {
#endif
        uhm_free_extent *moved = heap->core.mmap_recent_free_cache[i - 1u];
        heap->core.mmap_recent_free_cache[i] = moved;
        if (moved && moved->magic == UHM_FREE_MAGIC) {
            uhm_mmap_recent_free_set_position(moved, i);
        }
    }
    heap->core.mmap_recent_free_cache[0] = extent;
    uhm_mmap_recent_free_set_position(extent, 0);
    heap->core.mmap_recent_free_next =
        count + 1u < UHM_MMAP_RECENT_FREE_CACHE_SLOTS
            ? count + 1u
            : UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
}

static void uhm_mmap_free_note_partial_remainder(
    uhm_heap *heap, uhm_free_extent *extent) {
#if UHMAS_MMAP_HOT_CACHE_PROMOTE_FAST_PATH
    size_t bucket;
    size_t count;

    if (!heap || !extent || extent->magic != UHM_FREE_MAGIC ||
        extent->size == 0) {
        return;
    }
    bucket = uhm_mmap_size_hint_bucket(heap, extent->size);
    if (!uhm_mmap_size_hint_promote_existing(heap, extent, bucket)) {
        uhm_mmap_size_hint_note(heap, extent);
    }
    if (uhm_mmap_recent_free_promote_existing(heap, extent)) {
        return;
    }
    count = uhm_mmap_recent_free_forget_cache_entry(heap, extent);
    uhm_mmap_recent_free_push(heap, extent, count);
#else
    uhm_mmap_recent_free_note(heap, extent);
#endif
}

static int uhm_mmap_recent_free_cache_valid(uhm_heap *heap) {
    size_t i;
    size_t recent_used = 0;
    int recent_gap_seen = 0;

    if (!heap) {
        return 0;
    }
    for (i = 0; i < UHM_MMAP_RECENT_FREE_CACHE_SLOTS; ++i) {
        uhm_free_extent *extent = heap->core.mmap_recent_free_cache[i];
        if (!extent) {
            recent_gap_seen = 1;
            continue;
        }
        if (recent_gap_seen) {
            return 0;
        }
        if (!uhm_mmap_free_extent_is_listed(extent) ||
            !uhm_mmap_free_list_contains_exact(heap->core.mmap_free_head,
                                               extent)) {
            return 0;
        }
        if (!uhm_mmap_recent_free_position_matches(heap, extent)) {
            return 0;
        }
        ++recent_used;
    }
    if (heap->core.mmap_recent_free_next != recent_used) {
        return 0;
    }
    for (i = 0; i < UHM_MMAP_SIZE_HINT_BUCKETS; ++i) {
        size_t slot;
        int gap_seen = 0;
        size_t expected_max = 0;
        uhm_free_extent *expected_max_extent = 0;
        for (slot = 0; slot < UHM_MMAP_SIZE_HINT_SLOTS; ++slot) {
            uhm_free_extent *extent = heap->core.mmap_size_hint[i][slot];
            if (!extent) {
                gap_seen = 1;
                continue;
            }
            if (gap_seen) {
                return 0;
            }
            if (!uhm_mmap_free_extent_is_listed(extent) ||
                !uhm_mmap_free_list_contains_exact(heap->core.mmap_free_head,
                                                   extent)) {
                return 0;
            }
            if (uhm_mmap_size_hint_bucket(heap, extent->size) != i) {
                return 0;
            }
            if (!uhm_mmap_size_hint_position_matches(heap, extent)) {
                return 0;
            }
            if (extent->size > expected_max) {
                expected_max = extent->size;
                expected_max_extent = extent;
            }
        }
        if (heap->core.mmap_size_hint_bucket_max[i] != expected_max) {
            return 0;
        }
        if (heap->core.mmap_size_hint_bucket_max_extent[i] !=
            expected_max_extent) {
            return 0;
        }
    }
    return !heap->core.mmap_recent_free_extent ||
           (uhm_mmap_free_extent_is_listed(heap->core.mmap_recent_free_extent) &&
            uhm_mmap_free_list_contains_exact(
                heap->core.mmap_free_head,
                heap->core.mmap_recent_free_extent));
}

static uhm_result uhm_mmap_free_find_adjacent(
    uhm_heap *heap, uintptr_t addr, size_t size, uhm_free_extent **left_out,
    uhm_free_extent **right_out) {
    uintptr_t end;
    uhm_free_extent *left = 0;
    uhm_free_extent *right = 0;

    if (left_out) {
        *left_out = 0;
    }
    if (right_out) {
        *right_out = 0;
    }
    if (!heap || size == 0 || uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (heap->core.mmap_free_tree_current_nodes <=
        UHM_MMAP_FREE_ADJACENT_LIST_SCAN_LIMIT) {
        uhm_free_extent *prev = 0;
        uhm_free_extent *cursor = heap->core.mmap_free_head;
        while (cursor && cursor->addr < addr) {
            if (cursor->prev != prev) {
                return UHM_ERR_CORRUPTION;
            }
            prev = cursor;
            cursor = cursor->next;
        }
        if (cursor && cursor->prev != prev) {
            return UHM_ERR_CORRUPTION;
        }
        left = prev;
        right = cursor;
        goto validate_candidates;
    }
    if (!uhm_mmap_free_tree_find_neighbors_by_addr(heap, addr, &left,
                                                   &right)) {
        return UHM_ERR_CORRUPTION;
    }
    if ((left && left->next != right) ||
        (right && right->prev != left) ||
        (!left && heap->core.mmap_free_head != right)) {
        return UHM_ERR_CORRUPTION;
    }

validate_candidates:
    if (left) {
        uintptr_t prev_end;
        if (uhm_range_overflows(left->addr, left->size, &prev_end)) {
            return UHM_ERR_CORRUPTION;
        }
        if (prev_end > addr) {
            return UHM_ERR_CORRUPTION;
        }
        if (prev_end == addr) {
            /* keep left */
        } else {
            left = 0;
        }
    }
    if (right) {
        if (right->addr < end) {
            return UHM_ERR_CORRUPTION;
        }
        if (right->addr != end) {
            right = 0;
        }
    }

    if (left_out) {
        *left_out = left;
    }
    if (right_out) {
        *right_out = right;
    }
    return UHM_OK;
}

static uhm_result uhm_mmap_free_extend_adjacent_prevalidated(
    uhm_heap *heap, uintptr_t addr, size_t size, uhm_free_extent *left,
    uhm_free_extent *right) {
    uintptr_t end;

    if (!heap || size == 0 || uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    /* The caller performed uhm_mmap_free_find_adjacent after its last
       unlock/relock boundary and keeps heap->lock held through this publish. */
    if ((!left && !right) ||
        (left && (left->magic != UHM_FREE_MAGIC || left->size == 0)) ||
        (right && (right->magic != UHM_FREE_MAGIC || right->size == 0))) {
        return UHM_ERR_CORRUPTION;
    }

    if (left) {
        uintptr_t left_end;
        size_t new_size;

        if (uhm_range_overflows(left->addr, left->size, &left_end) ||
            left_end != addr) {
            return UHM_ERR_CORRUPTION;
        }
        if (right && (left->next != right || right->addr != end)) {
            return UHM_ERR_CORRUPTION;
        }
        new_size = (size_t)(end - left->addr);
        uhm_mmap_size_hint_forget_current_if_bucket_changes(heap, left,
                                                            new_size);
        left->size = new_size;
        uhm_free_extent_descriptor_sync(heap, left);
        return uhm_mmap_free_merge_forward(heap, left, 0);
    }

    if (right) {
        uintptr_t right_end;
        size_t new_size;

        if (right->addr != end ||
            uhm_range_overflows(right->addr, right->size, &right_end)) {
            return UHM_ERR_CORRUPTION;
        }
        new_size = (size_t)(right_end - addr);
        uhm_mmap_size_hint_forget_current_if_bucket_changes(heap, right,
                                                            new_size);
        uhm_mmap_free_tree_remove(heap, right);
        right->addr = addr;
        right->size = new_size;
        uhm_free_extent_descriptor_sync(heap, right);
        uhm_mmap_free_tree_insert(heap, right);
        uhm_mmap_recent_free_note(heap, right);
        return UHM_OK;
    }

    return UHM_ERR_CORRUPTION;
}

static uhm_result uhm_mmap_free_merge_forward(uhm_heap *heap,
                                               uhm_free_extent *node,
                                               int fresh_node) {
    while (node && node->next) {
        uintptr_t node_end;
        uintptr_t next_end;
        uhm_free_extent *next;

        if (uhm_range_overflows(node->addr, node->size, &node_end) ||
            uhm_range_overflows(node->next->addr, node->next->size,
                                &next_end)) {
            return UHM_ERR_CORRUPTION;
        }
        if (node_end < node->next->addr) {
            break;
        }

        next = node->next;
        if (next_end > node_end) {
            size_t new_size = next_end - node->addr;
            uhm_mmap_size_hint_forget_current_if_bucket_changes(heap, node,
                                                                new_size);
            node->size = next_end - node->addr;
            uhm_free_extent_descriptor_sync(heap, node);
        }
        node->next = next->next;
        if (node->next) {
            node->next->prev = node;
        }
        next->prev = 0;
        next->next = 0;
        uhm_mmap_free_tree_remove(heap, next);
        uhm_free_extent_release_node(heap, next);
    }
    if (fresh_node) {
        uhm_mmap_recent_free_note_fresh(heap, node);
    } else {
        uhm_mmap_recent_free_note(heap, node);
    }
    return UHM_OK;
}

static uhm_result uhm_mmap_free_insert_node(uhm_heap *heap,
                                            uhm_free_extent *node) {
    uhm_free_extent **cursor = &heap->core.mmap_free_head;
    uhm_free_extent *prev = 0;

#if UHMAS_ENABLE_TEST_HOOKS
    if (heap && heap->core.fault_mmap_free_insert_countdown != 0 &&
        --heap->core.fault_mmap_free_insert_countdown == 0) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
#endif

    if (!node || node->size == 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    while (*cursor && (*cursor)->addr < node->addr) {
        prev = *cursor;
        cursor = &(*cursor)->next;
    }

    node->prev = prev;
    node->next = *cursor;
    if (node->next) {
        node->next->prev = node;
    }
    *cursor = node;
    node->flags |= UHM_META_FLAG_IN_MMAP_FREE_LIST;
    uhm_mmap_free_tree_insert(heap, node);

    if (prev) {
        uintptr_t prev_end;
        uintptr_t node_end;
        if (uhm_range_overflows(prev->addr, prev->size, &prev_end)) {
            return UHM_ERR_CORRUPTION;
        }
        if (prev_end < node->addr) {
            return uhm_mmap_free_merge_forward(heap, node, 1);
        }
        if (uhm_range_overflows(node->addr, node->size, &node_end)) {
            return UHM_ERR_CORRUPTION;
        }
        if (node_end > prev_end) {
            size_t new_size = node_end - prev->addr;
            uhm_mmap_size_hint_forget_current_if_bucket_changes(heap, prev,
                                                                new_size);
            prev->size = node_end - prev->addr;
            uhm_free_extent_descriptor_sync(heap, prev);
        }
        uhm_mmap_free_tree_remove(heap, node);
        if (!uhm_mmap_free_list_unlink_node(heap, node)) {
            return UHM_ERR_CORRUPTION;
        }
        uhm_free_extent_release_node(heap, node);
        return uhm_mmap_free_merge_forward(heap, prev, 0);
    } else {
        return uhm_mmap_free_merge_forward(heap, node, 1);
    }
}

static uhm_result uhm_mmap_free_insert(uhm_heap *heap, uintptr_t addr, size_t size,
                                       int allow_emergency) {
    uhm_free_extent *node = uhm_free_extent_new(heap, addr, size, allow_emergency);
    if (!node) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    return uhm_mmap_free_insert_node(heap, node);
}

static uhm_result uhm_mmap_free_claim(uhm_heap *heap, uintptr_t addr, size_t size, int *claimed) {
    uhm_free_extent **cursor = &heap->core.mmap_free_head;
    uhm_free_extent *prev = 0;
    uintptr_t end;

    *claimed = 0;
    if (uhm_range_overflows(addr, size, &end)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    while (*cursor) {
        uhm_free_extent *free_extent = *cursor;
        uintptr_t free_end;
        if (free_extent->prev != prev) {
            return UHM_ERR_CORRUPTION;
        }
        if (uhm_range_overflows(free_extent->addr, free_extent->size,
                                &free_end)) {
            return UHM_ERR_CORRUPTION;
        }
        if (addr >= free_extent->addr && end <= free_end) {
            if (addr == free_extent->addr && end == free_end) {
                uhm_mmap_free_tree_remove(heap, free_extent);
                if (!uhm_mmap_free_list_unlink_node(heap, free_extent)) {
                    return UHM_ERR_CORRUPTION;
                }
                uhm_free_extent_release_node(heap, free_extent);
            } else if (addr == free_extent->addr) {
                size_t new_size = free_end - end;
                uhm_mmap_free_tree_remove(heap, free_extent);
                uhm_mmap_size_hint_forget_current_if_bucket_changes(
                    heap, free_extent, new_size);
                free_extent->addr = end;
                free_extent->size = new_size;
                uhm_free_extent_descriptor_sync(heap, free_extent);
                uhm_mmap_size_hint_note(heap, free_extent);
                uhm_mmap_free_tree_insert(heap, free_extent);
            } else if (end == free_end) {
                size_t new_size = addr - free_extent->addr;
                uhm_mmap_size_hint_forget_current_if_bucket_changes(
                    heap, free_extent, new_size);
                free_extent->size = addr - free_extent->addr;
                uhm_free_extent_descriptor_sync(heap, free_extent);
                uhm_mmap_size_hint_note(heap, free_extent);
            } else {
                uhm_free_extent *tail = uhm_free_extent_new(heap, end, free_end - end, 0);
                if (!tail) {
                    return UHM_ERR_OUT_OF_MEMORY;
                }
                tail->prev = free_extent;
                tail->next = free_extent->next;
                if (tail->next) {
                    tail->next->prev = tail;
                }
                free_extent->next = tail;
                tail->flags |= UHM_META_FLAG_IN_MMAP_FREE_LIST;
                uhm_mmap_free_tree_insert(heap, tail);
                uhm_mmap_size_hint_forget_current_if_bucket_changes(
                    heap, free_extent, addr - free_extent->addr);
                free_extent->size = addr - free_extent->addr;
                uhm_free_extent_descriptor_sync(heap, free_extent);
                uhm_mmap_size_hint_note(heap, free_extent);
                uhm_mmap_size_hint_note_fresh(heap, tail);
            }
            *claimed = 1;
            return UHM_OK;
        }
        prev = free_extent;
        cursor = &(*cursor)->next;
    }
    return UHM_OK;
}

static int uhm_mmap_free_take_extent(uhm_heap *heap, uhm_free_extent *extent,
                                     size_t size, uintptr_t *addr_out) {
    size_t new_size;

    if (!heap || !extent || extent->magic != UHM_FREE_MAGIC ||
        extent->size < size || !addr_out) {
        return 0;
    }

    new_size = extent->size - size;
    uhm_mmap_size_hint_forget_current_if_bucket_changes(
        heap, extent, new_size);
    if (uhm_range_overflows(extent->addr, new_size, addr_out)) {
        return 0;
    }
    if (new_size == 0) {
        uhm_mmap_free_tree_remove(heap, extent);
        if (!uhm_mmap_free_list_unlink_node(heap, extent)) {
            return 0;
        }
        uhm_free_extent_release_node(heap, extent);
    } else {
        extent->size = new_size;
        uhm_free_extent_descriptor_sync(heap, extent);
        uhm_mmap_free_note_partial_remainder(heap, extent);
    }
    return 1;
}

static int uhm_mmap_free_take_auto(uhm_heap *heap, size_t size, uintptr_t *addr_out) {
    uhm_free_extent *best;
    uhm_free_extent *root;
    uhm_free_extent *recent_best = 0;
    uhm_free_extent *size_best = 0;
    size_t scanned;
    size_t bucket;
    int saw_recent = 0;
    int saw_size_hint = 0;
    int fit_attempted = 0;

    if (!heap) {
        return 0;
    }
    root = heap->core.mmap_free_tree_root;
    if (!root || root->tree_max_size < size) {
        return 0;
    }

restart_recent_scan:
    recent_best = 0;
    for (scanned = 0; heap && scanned < UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
         ++scanned) {
        size_t index = scanned;
        uhm_free_extent *recent = heap->core.mmap_recent_free_cache[index];
        if (!recent) {
            break;
        }
        saw_recent = 1;
        if (!uhm_mmap_free_extent_is_listed(recent)) {
            (void)uhm_mmap_recent_free_remove_at(heap, index);
            goto restart_recent_scan;
        }
        if (recent->size < size) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, mmap_recent_free_too_small));
            continue;
        }
        if (!recent_best || recent->size < recent_best->size) {
            if (recent_best) {
                uhm_diag_note_u64_inc(
                    UHM_DIAG_CORE_FIELD_PTR(
                        heap, mmap_recent_free_best_fit_replacements));
            }
            recent_best = recent;
        }
#if UHMAS_MMAP_EXACT_FIT_EARLY_EXIT
        if (recent->size == size) {
            break;
        }
#endif
    }
    if (recent_best) {
        fit_attempted = 1;
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_recent_free_attempts));
        if (uhm_mmap_free_take_extent(heap, recent_best, size, addr_out)) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, mmap_recent_free_hits));
            return 1;
        }
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_recent_free_misses));
    }
    if (saw_recent && !fit_attempted) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_recent_free_attempts));
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_recent_free_misses));
    }
    for (bucket = heap ? uhm_mmap_size_hint_bucket(heap, size) : 0;
         heap && bucket < UHM_MMAP_SIZE_HINT_BUCKETS; ++bucket) {
        size_t slot;
        int bucket_dirty = 0;
#if UHMAS_MMAP_EXACT_FIT_EARLY_EXIT
        int exact_fit = 0;
#endif
        if (heap->core.mmap_size_hint_bucket_max[bucket] < size) {
            continue;
        }
        if (heap->core.mmap_size_hint_bucket_max[bucket] == size) {
            uhm_free_extent *exact =
                uhm_mmap_size_hint_exact_max_candidate(heap, bucket, size);
            if (exact) {
                size_best = exact;
#if UHMAS_ENABLE_TEST_HOOKS
                ++heap->core.mmap_size_hint_direct_exact_hits;
#endif
                saw_size_hint = 1;
#if UHMAS_MMAP_EXACT_FIT_EARLY_EXIT
                exact_fit = 1;
#endif
                break;
            }
        }
        for (slot = 0; slot < UHM_MMAP_SIZE_HINT_SLOTS; ++slot) {
            uhm_free_extent *hint = heap->core.mmap_size_hint[bucket][slot];
            if (!hint) {
                break;
            }
            saw_size_hint = 1;
            if (!uhm_mmap_free_extent_is_listed(hint)) {
                heap->core.mmap_size_hint[bucket][slot] = 0;
                bucket_dirty = 1;
                if (hint && hint->magic == UHM_FREE_MAGIC) {
                    uhm_mmap_size_hint_clear_position(hint);
                }
                uhm_diag_note_u64_inc(
                    UHM_DIAG_CORE_FIELD_PTR(heap, mmap_size_hint_stale));
                continue;
            }
            if (hint->size < size) {
                heap->core.mmap_size_hint[bucket][slot] = 0;
                bucket_dirty = 1;
                uhm_mmap_size_hint_clear_position(hint);
                uhm_diag_note_u64_inc(
                    UHM_DIAG_CORE_FIELD_PTR(heap, mmap_size_hint_too_small));
                continue;
            }
            if (!size_best || hint->size < size_best->size) {
                if (size_best) {
                    uhm_diag_note_u64_inc(
                        UHM_DIAG_CORE_FIELD_PTR(
                            heap, mmap_size_hint_best_fit_replacements));
                }
                size_best = hint;
            }
#if UHMAS_MMAP_EXACT_FIT_EARLY_EXIT
            if (hint->size == size) {
                exact_fit = 1;
                break;
            }
#endif
        }
        if (bucket_dirty) {
            uhm_mmap_size_hint_compact_bucket(heap, bucket);
        }
#if UHMAS_MMAP_EXACT_FIT_EARLY_EXIT
        if (exact_fit) {
            break;
        }
#endif
    }
    if (size_best) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_size_hint_attempts));
        if (uhm_mmap_free_take_extent(heap, size_best, size, addr_out)) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, mmap_size_hint_hits));
            return 1;
        }
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_size_hint_misses));
    } else if (saw_size_hint) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_size_hint_attempts));
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_size_hint_misses));
    }
    best = uhm_mmap_free_tree_find_highest_fit(
        heap, heap->core.mmap_free_tree_root, size);
    if (!best) {
        return 0;
    }

    return uhm_mmap_free_take_extent(heap, best, size, addr_out);
}

static uint64_t uhm_vma_tree_priority(const uhm_vma *vma);
static void uhm_vma_tree_rotate_left(uhm_heap *heap, uhm_vma *node,
                                     uint64_t *rotation_steps);
static void uhm_vma_tree_rotate_right(uhm_heap *heap, uhm_vma *node,
                                      uint64_t *rotation_steps);

static void uhm_vma_tree_insert(uhm_heap *heap, uhm_vma *vma) {
    uhm_vma **link = &heap->core.vma_tree_root;
    uhm_vma *parent = 0;
    uint64_t depth = 0;
    uint64_t rotation_steps = 0;
    uint64_t inserted_priority;

    vma->tree_parent = 0;
    vma->tree_left = 0;
    vma->tree_right = 0;
    while (*link) {
        parent = *link;
        depth++;
        if (vma->addr < parent->addr ||
            (vma->addr == parent->addr && (uintptr_t)vma < (uintptr_t)parent)) {
            link = &parent->tree_left;
        } else {
            link = &parent->tree_right;
        }
    }
    vma->tree_parent = parent;
    *link = vma;
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_insertions));
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_max_insert_depth), depth);
    if (uhm_diag_note_tree_insert(&heap->core.vma_tree_current_nodes,
                                  &heap->core.vma_tree_max_nodes,
                                  &heap->core.vma_tree_depth_limit_exceeded,
                                  depth)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_DEPTH);
    }
    inserted_priority = uhm_vma_tree_priority(vma);
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_note_tree_priority_bucket(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_bucket_mask),
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_bucket_max),
        heap->core.vma_tree_priority_buckets, inserted_priority);
#endif
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.vma_tree_insert_budget_exceeded,
            heap->core.vma_tree_current_nodes, depth)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_INSERT);
    }
    while (vma->tree_parent) {
        uint64_t parent_priority =
            uhm_vma_tree_priority(vma->tree_parent);
        if (inserted_priority == parent_priority) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_ties));
        }
        if (inserted_priority >= parent_priority) {
            break;
        }
        if (vma->tree_parent->tree_left == vma) {
            uhm_vma_tree_rotate_right(heap, vma->tree_parent,
                                      &rotation_steps);
        } else {
            uhm_vma_tree_rotate_left(heap, vma->tree_parent,
                                     &rotation_steps);
        }
    }
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.vma_tree_insert_rotation_budget_exceeded,
            heap->core.vma_tree_current_nodes, rotation_steps)) {
        uhm_diag_mark_treap_health(heap,
                                   UHM_TREAP_HEALTH_VMA_INSERT_ROTATION);
    }
}

static int uhm_vma_tree_less(const uhm_vma *a, const uhm_vma *b) {
    if (a->addr != b->addr) {
        return a->addr < b->addr;
    }
    return (uintptr_t)a < (uintptr_t)b;
}

static uint64_t uhm_vma_tree_priority(const uhm_vma *vma) {
    return vma ? vma->tree_priority : 0;
}

static void uhm_vma_tree_rotate_left(uhm_heap *heap, uhm_vma *node,
                                     uint64_t *rotation_steps) {
    uhm_vma *right = node ? node->tree_right : 0;
    uhm_vma *parent = node ? node->tree_parent : 0;

    if (!heap || !node || !right) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_rotations));
    if (rotation_steps) {
        ++*rotation_steps;
    }
    node->tree_right = right->tree_left;
    if (node->tree_right) {
        node->tree_right->tree_parent = node;
    }
    right->tree_left = node;
    right->tree_parent = parent;
    if (!parent) {
        heap->core.vma_tree_root = right;
    } else if (parent->tree_left == node) {
        parent->tree_left = right;
    } else {
        parent->tree_right = right;
    }
    node->tree_parent = right;
}

static void uhm_vma_tree_rotate_right(uhm_heap *heap, uhm_vma *node,
                                      uint64_t *rotation_steps) {
    uhm_vma *left = node ? node->tree_left : 0;
    uhm_vma *parent = node ? node->tree_parent : 0;

    if (!heap || !node || !left) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_rotations));
    if (rotation_steps) {
        ++*rotation_steps;
    }
    node->tree_left = left->tree_right;
    if (node->tree_left) {
        node->tree_left->tree_parent = node;
    }
    left->tree_right = node;
    left->tree_parent = parent;
    if (!parent) {
        heap->core.vma_tree_root = left;
    } else if (parent->tree_left == node) {
        parent->tree_left = left;
    } else {
        parent->tree_right = left;
    }
    node->tree_parent = left;
}

static int uhm_vma_tree_validate(uhm_heap *heap, uhm_vma *node,
                                 uhm_vma *parent, size_t *count) {
    if (!node) {
        return 1;
    }
    if (node->tree_parent != parent || node->magic != UHM_VMA_MAGIC ||
        node->tree_priority !=
            uhm_tree_priority_mix_salted(heap, (uint64_t)(uintptr_t)node)) {
        return 0;
    }
    if (node->tree_left &&
        (!uhm_vma_tree_less(node->tree_left, node) ||
         uhm_vma_tree_priority(node->tree_left) <
             uhm_vma_tree_priority(node))) {
        return 0;
    }
    if (node->tree_right &&
        (!uhm_vma_tree_less(node, node->tree_right) ||
         uhm_vma_tree_priority(node->tree_right) <
             uhm_vma_tree_priority(node))) {
        return 0;
    }
    ++*count;
    return uhm_vma_tree_validate(heap, node->tree_left, node, count) &&
           uhm_vma_tree_validate(heap, node->tree_right, node, count);
}

static void uhm_vma_tree_replace(uhm_heap *heap, uhm_vma *old_node,
                                 uhm_vma *new_node) {
    if (!old_node->tree_parent) {
        heap->core.vma_tree_root = new_node;
    } else if (old_node->tree_parent->tree_left == old_node) {
        old_node->tree_parent->tree_left = new_node;
    } else {
        old_node->tree_parent->tree_right = new_node;
    }
    if (new_node) {
        new_node->tree_parent = old_node->tree_parent;
    }
}

static void uhm_vma_tree_remove(uhm_heap *heap, uhm_vma *vma) {
    uint64_t sink_steps = 0;

    if (!vma) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_remove_calls));
    if (vma->tree_parent == 0 && heap->core.vma_tree_root != vma) {
        vma->tree_left = 0;
        vma->tree_right = 0;
        return;
    }
    while (vma->tree_left && vma->tree_right) {
        sink_steps++;
        if (uhm_vma_tree_priority(vma->tree_left) <
            uhm_vma_tree_priority(vma->tree_right)) {
            uhm_vma_tree_rotate_right(heap, vma, 0);
        } else {
            uhm_vma_tree_rotate_left(heap, vma, 0);
        }
    }
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_remove_sink_steps),
        sink_steps);
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_max_remove_sink_steps),
        sink_steps);
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.vma_tree_remove_budget_exceeded,
            heap->core.vma_tree_current_nodes, sink_steps)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_REMOVE);
    }
    uhm_vma_tree_replace(heap, vma,
                         vma->tree_left ? vma->tree_left : vma->tree_right);
    uhm_diag_note_tree_remove(&heap->core.vma_tree_current_nodes);
    vma->tree_parent = 0;
    vma->tree_left = 0;
    vma->tree_right = 0;
}

static uhm_vma *uhm_find_vma_overlap(uhm_heap *heap, uintptr_t addr, size_t size) {
    uhm_vma *vma = heap->core.vma_tree_root;
    uintptr_t end;
    uint64_t steps = 0;
    uint64_t hard_limit =
        uhm_diag_tree_find_hard_limit(heap->core.vma_tree_current_nodes);

    if (uhm_range_overflows(addr, size, &end)) {
        return 0;
    }
    while (vma) {
        uintptr_t vma_end;
        steps++;
        if (steps > hard_limit) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_calls));
            uhm_diag_note_u64_add(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_steps), steps);
            uhm_diag_note_u64_inc(
                &heap->core.vma_tree_find_budget_exceeded);
            uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_FIND);
            uhm_diag_note_max_u64(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_max_find_steps),
                steps);
            return 0;
        }
        if (uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
            return 0;
        }
        if (end <= vma->addr) {
            vma = vma->tree_left;
        } else if (addr >= vma_end) {
            vma = vma->tree_right;
        } else {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_calls));
            uhm_diag_note_u64_add(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_steps), steps);
            uhm_diag_note_max_u64(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_max_find_steps),
                steps);
            if (uhm_diag_note_tree_operation_budget(
                    &heap->core.vma_tree_find_budget_exceeded,
                    heap->core.vma_tree_current_nodes, steps)) {
                uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_FIND);
            }
            return vma->magic == UHM_VMA_MAGIC ? vma : 0;
        }
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_calls));
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_steps), steps);
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_max_find_steps), steps);
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.vma_tree_find_budget_exceeded,
            heap->core.vma_tree_current_nodes, steps)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_FIND);
    }
    return 0;
}

static uhm_vma *uhm_find_vma_covering(uhm_heap *heap, uintptr_t addr, size_t size) {
    uhm_vma *vma = heap->core.vma_tree_root;
    uintptr_t end;
    uint64_t steps = 0;
    uint64_t hard_limit =
        uhm_diag_tree_find_hard_limit(heap->core.vma_tree_current_nodes);
    if (uhm_range_overflows(addr, size, &end)) {
        return 0;
    }
    while (vma) {
        uintptr_t vma_end;
        steps++;
        if (steps > hard_limit) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_calls));
            uhm_diag_note_u64_add(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_steps), steps);
            uhm_diag_note_u64_inc(
                &heap->core.vma_tree_find_budget_exceeded);
            uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_FIND);
            uhm_diag_note_max_u64(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_max_find_steps),
                steps);
            return 0;
        }
        if (uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
            return 0;
        }
        if (addr < vma->addr) {
            vma = vma->tree_left;
        } else if (addr >= vma_end) {
            vma = vma->tree_right;
        } else {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_calls));
            uhm_diag_note_u64_add(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_steps), steps);
            uhm_diag_note_max_u64(
                UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_max_find_steps),
                steps);
            if (uhm_diag_note_tree_operation_budget(
                    &heap->core.vma_tree_find_budget_exceeded,
                    heap->core.vma_tree_current_nodes, steps)) {
                uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_FIND);
            }
            return end <= vma_end && vma->magic == UHM_VMA_MAGIC ? vma : 0;
        }
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_calls));
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_find_steps), steps);
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_max_find_steps), steps);
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.vma_tree_find_budget_exceeded,
            heap->core.vma_tree_current_nodes, steps)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_VMA_FIND);
    }
    return 0;
}

static uhm_vma *uhm_vma_new(uhm_heap *heap, uintptr_t addr, size_t size,
                            uint32_t prot, uint32_t map_flags, int allow_emergency) {
    uhm_vma *vma = uhm_metadata_hot_vma_take(heap);
    int emergency = 0;

    if (!vma) {
        vma = (uhm_vma *)uhm_control_alloc_uninitialized(
            heap, sizeof(*vma), alignof(uhm_vma));
    }
    if (!vma && allow_emergency) {
        vma = uhm_emergency_vma_take(heap);
        emergency = vma != 0;
    }
    if (!vma) {
        return 0;
    }
    vma->magic = UHM_VMA_MAGIC;
    vma->flags = (map_flags & ~UHM_META_FLAG_EMERGENCY) |
                 (emergency ? UHM_META_FLAG_EMERGENCY : 0u);
    vma->addr = addr;
    vma->size = size;
    vma->prot = prot;
    vma->retire_epoch = 0;
    vma->prev = 0;
    vma->next = 0;
    vma->tree_parent = 0;
    vma->tree_left = 0;
    vma->tree_right = 0;
    vma->tree_priority =
        uhm_tree_priority_mix_for_heap(
            heap, (uint64_t)(uintptr_t)vma,
            UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_mix_calls),
            UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_mix_ns),
            UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_mix_max_ns));
    vma->generation = uhm_heap_generation_next(heap);
    uhm_extent_descriptor_set(&vma->desc, UHM_EXTENT_KIND_MMAP,
                              addr, size, UHM_PAGE_OWNER_MMAP,
                              UHM_PAGE_VA_MMAP_LIVE,
                              (map_flags & UHM_MAP_NO_COMMIT) != 0
                                  ? UHM_PAGE_COMMIT_DECOMMITTED
                                  : UHM_PAGE_COMMIT_LIVE,
                              prot, vma->generation, vma);
    return vma;
}

static void uhm_vma_descriptor_sync(uhm_vma *vma, uint32_t commit_state) {
    if (!vma) {
        return;
    }
    uhm_extent_descriptor_set(&vma->desc, UHM_EXTENT_KIND_MMAP,
                              vma->addr, vma->size, UHM_PAGE_OWNER_MMAP,
                              UHM_PAGE_VA_MMAP_LIVE, commit_state,
                              vma->prot, vma->generation, vma);
}

static uint32_t uhm_vma_commit_state_from_flags(uint32_t flags) {
    return (flags & UHM_MAP_NO_COMMIT) != 0
               ? UHM_PAGE_COMMIT_DECOMMITTED
               : UHM_PAGE_COMMIT_LIVE;
}

static size_t uhm_mmap_free_tree_child_max(const uhm_free_extent *node) {
    return node ? node->tree_max_size : 0u;
}

static void uhm_mmap_free_tree_recompute(uhm_free_extent *node) {
    size_t max_size;

    if (!node) {
        return;
    }
    max_size = node->size;
    if (node->tree_left && node->tree_left->tree_max_size > max_size) {
        max_size = node->tree_left->tree_max_size;
    }
    if (node->tree_right && node->tree_right->tree_max_size > max_size) {
        max_size = node->tree_right->tree_max_size;
    }
    node->tree_max_size = max_size;
}

static void uhm_mmap_free_tree_update_up(uhm_free_extent *node) {
    while (node) {
        uhm_mmap_free_tree_recompute(node);
        node = node->tree_parent;
    }
}

static uint64_t uhm_mmap_free_tree_priority(const uhm_free_extent *node) {
    return node ? node->tree_priority : 0;
}

static void uhm_mmap_free_tree_rotate_left(uhm_heap *heap,
                                           uhm_free_extent *node,
                                           uint64_t *rotation_steps) {
    uhm_free_extent *right = node ? node->tree_right : 0;
    uhm_free_extent *parent = node ? node->tree_parent : 0;

    if (!heap || !node || !right) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_rotations));
    if (rotation_steps) {
        ++*rotation_steps;
    }
    node->tree_right = right->tree_left;
    if (node->tree_right) {
        node->tree_right->tree_parent = node;
    }
    right->tree_left = node;
    right->tree_parent = parent;
    if (!parent) {
        heap->core.mmap_free_tree_root = right;
    } else if (parent->tree_left == node) {
        parent->tree_left = right;
    } else {
        parent->tree_right = right;
    }
    node->tree_parent = right;
    uhm_mmap_free_tree_recompute(node);
    uhm_mmap_free_tree_recompute(right);
    uhm_mmap_free_tree_update_up(parent);
}

static void uhm_mmap_free_tree_rotate_right(uhm_heap *heap,
                                            uhm_free_extent *node,
                                            uint64_t *rotation_steps) {
    uhm_free_extent *left = node ? node->tree_left : 0;
    uhm_free_extent *parent = node ? node->tree_parent : 0;

    if (!heap || !node || !left) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_rotations));
    if (rotation_steps) {
        ++*rotation_steps;
    }
    node->tree_left = left->tree_right;
    if (node->tree_left) {
        node->tree_left->tree_parent = node;
    }
    left->tree_right = node;
    left->tree_parent = parent;
    if (!parent) {
        heap->core.mmap_free_tree_root = left;
    } else if (parent->tree_left == node) {
        parent->tree_left = left;
    } else {
        parent->tree_right = left;
    }
    node->tree_parent = left;
    uhm_mmap_free_tree_recompute(node);
    uhm_mmap_free_tree_recompute(left);
    uhm_mmap_free_tree_update_up(parent);
}

static void uhm_mmap_free_tree_insert(uhm_heap *heap, uhm_free_extent *node) {
    uhm_free_extent **link = &heap->core.mmap_free_tree_root;
    uhm_free_extent *parent = 0;
    uint64_t depth = 0;
    uint64_t rotation_steps = 0;
    uint64_t inserted_priority;

    node->tree_parent = 0;
    node->tree_left = 0;
    node->tree_right = 0;
    node->tree_max_size = node->size;
    while (*link) {
        parent = *link;
        depth++;
        if (node->addr < parent->addr ||
            (node->addr == parent->addr && (uintptr_t)node < (uintptr_t)parent)) {
            link = &parent->tree_left;
        } else {
            link = &parent->tree_right;
        }
    }
    node->tree_parent = parent;
    *link = node;
    uhm_mmap_free_tree_update_up(parent);
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_insertions));
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_max_insert_depth),
        depth);
    if (uhm_diag_note_tree_insert(
            &heap->core.mmap_free_tree_current_nodes,
            &heap->core.mmap_free_tree_max_nodes,
            &heap->core.mmap_free_tree_depth_limit_exceeded, depth)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_MMAP_FREE_DEPTH);
    }
    inserted_priority = uhm_mmap_free_tree_priority(node);
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_note_tree_priority_bucket(
        UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_priority_bucket_mask),
        UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_priority_bucket_max),
        heap->core.mmap_free_tree_priority_buckets, inserted_priority);
#endif
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.mmap_free_tree_insert_budget_exceeded,
            heap->core.mmap_free_tree_current_nodes, depth)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_MMAP_FREE_INSERT);
    }
    while (node->tree_parent) {
        uint64_t parent_priority =
            uhm_mmap_free_tree_priority(node->tree_parent);
        if (inserted_priority == parent_priority) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap,
                                        mmap_free_tree_priority_ties));
        }
        if (inserted_priority >= parent_priority) {
            break;
        }
        if (node->tree_parent->tree_left == node) {
            uhm_mmap_free_tree_rotate_right(heap, node->tree_parent,
                                            &rotation_steps);
        } else {
            uhm_mmap_free_tree_rotate_left(heap, node->tree_parent,
                                           &rotation_steps);
        }
    }
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.mmap_free_tree_insert_rotation_budget_exceeded,
            heap->core.mmap_free_tree_current_nodes, rotation_steps)) {
        uhm_diag_mark_treap_health(
            heap, UHM_TREAP_HEALTH_MMAP_FREE_INSERT_ROTATION);
    }
    uhm_mmap_free_tree_update_up(node);
}

static int uhm_mmap_free_tree_less(const uhm_free_extent *a,
                                   const uhm_free_extent *b) {
    if (a->addr != b->addr) {
        return a->addr < b->addr;
    }
    return (uintptr_t)a < (uintptr_t)b;
}

static int uhm_mmap_free_tree_validate(uhm_heap *heap, uhm_free_extent *node,
                                       uhm_free_extent *parent,
                                       size_t *count) {
    size_t expected_max;

    if (!node) {
        return 1;
    }
    if (node->tree_parent != parent || node->magic != UHM_FREE_MAGIC ||
        (node->flags & UHM_META_FLAG_IN_MMAP_FREE_LIST) == 0 ||
        (node->flags &
         ~(UHM_META_FLAG_EMERGENCY | UHM_META_FLAG_IN_MMAP_FREE_LIST |
           UHM_FREE_EXTENT_CACHE_POSITION_MASK)) != 0 ||
        node->tree_priority !=
            uhm_tree_priority_mix_salted(heap, (uint64_t)(uintptr_t)node)) {
        return 0;
    }
    expected_max = node->size;
    if (node->tree_left) {
        if (!uhm_mmap_free_tree_less(node->tree_left, node) ||
            uhm_mmap_free_tree_priority(node->tree_left) <
                uhm_mmap_free_tree_priority(node)) {
            return 0;
        }
        if (node->tree_left->tree_max_size > expected_max) {
            expected_max = node->tree_left->tree_max_size;
        }
    }
    if (node->tree_right) {
        if (!uhm_mmap_free_tree_less(node, node->tree_right) ||
            uhm_mmap_free_tree_priority(node->tree_right) <
                uhm_mmap_free_tree_priority(node)) {
            return 0;
        }
        if (node->tree_right->tree_max_size > expected_max) {
            expected_max = node->tree_right->tree_max_size;
        }
    }
    if (node->tree_max_size != expected_max) {
        return 0;
    }
    ++*count;
    return uhm_mmap_free_tree_validate(heap, node->tree_left, node, count) &&
           uhm_mmap_free_tree_validate(heap, node->tree_right, node, count);
}

static void uhm_mmap_free_tree_replace(uhm_heap *heap,
                                       uhm_free_extent *old_node,
                                       uhm_free_extent *new_node) {
    if (!old_node->tree_parent) {
        heap->core.mmap_free_tree_root = new_node;
    } else if (old_node->tree_parent->tree_left == old_node) {
        old_node->tree_parent->tree_left = new_node;
    } else {
        old_node->tree_parent->tree_right = new_node;
    }
    if (new_node) {
        new_node->tree_parent = old_node->tree_parent;
    }
    uhm_mmap_free_tree_update_up(new_node ? new_node : old_node->tree_parent);
}

static void uhm_mmap_free_tree_remove(uhm_heap *heap, uhm_free_extent *node) {
    uint64_t sink_steps = 0;

    if (!node) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_remove_calls));
    if (node->tree_parent == 0 && heap->core.mmap_free_tree_root != node) {
        node->tree_left = 0;
        node->tree_right = 0;
        node->tree_max_size = node->size;
        return;
    }
    while (node->tree_left && node->tree_right) {
        sink_steps++;
        if (uhm_mmap_free_tree_priority(node->tree_left) <
            uhm_mmap_free_tree_priority(node->tree_right)) {
            uhm_mmap_free_tree_rotate_right(heap, node, 0);
        } else {
            uhm_mmap_free_tree_rotate_left(heap, node, 0);
        }
    }
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_remove_sink_steps),
        sink_steps);
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap,
                                mmap_free_tree_max_remove_sink_steps),
        sink_steps);
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.mmap_free_tree_remove_budget_exceeded,
            heap->core.mmap_free_tree_current_nodes, sink_steps)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_MMAP_FREE_REMOVE);
    }
    uhm_mmap_free_tree_replace(heap, node,
                               node->tree_left ? node->tree_left
                                               : node->tree_right);
    uhm_diag_note_tree_remove(&heap->core.mmap_free_tree_current_nodes);
    node->tree_parent = 0;
    node->tree_left = 0;
    node->tree_right = 0;
    node->tree_max_size = node->size;
}

static int uhm_mmap_free_tree_find_neighbors_by_addr(
    uhm_heap *heap, uintptr_t addr, uhm_free_extent **left_out,
    uhm_free_extent **right_out) {
    uhm_free_extent *node;
    uhm_free_extent *left = 0;
    uhm_free_extent *right = 0;
    uint64_t steps = 0;
    uint64_t hard_limit =
        uhm_diag_tree_find_hard_limit(
            heap ? heap->core.mmap_free_tree_current_nodes : 0);

    if (left_out) {
        *left_out = 0;
    }
    if (right_out) {
        *right_out = 0;
    }
    if (!heap) {
        return 0;
    }
    node = heap->core.mmap_free_tree_root;
    while (node) {
        ++steps;
        if (steps > hard_limit) {
            uhm_diag_note_u64_inc(
                &heap->core.mmap_free_tree_find_budget_exceeded);
            uhm_diag_mark_treap_health(heap,
                                       UHM_TREAP_HEALTH_MMAP_FREE_FIND);
            return 0;
        }
        if (node->addr < addr) {
            left = node;
            node = node->tree_right;
        } else {
            right = node;
            node = node->tree_left;
        }
    }
    if (left_out) {
        *left_out = left;
    }
    if (right_out) {
        *right_out = right;
    }
    return 1;
}

static uhm_free_extent *uhm_mmap_free_tree_find_highest_fit(
    uhm_heap *heap, uhm_free_extent *node, size_t size) {
    uint64_t steps = 0;
    uint64_t hard_limit =
        uhm_diag_tree_find_hard_limit(
            heap ? heap->core.mmap_free_tree_current_nodes : 0);

    while (node) {
        steps++;
        if (steps > hard_limit) {
            if (heap) {
                uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
                    heap, mmap_free_tree_find_calls));
                uhm_diag_note_u64_add(UHM_DIAG_CORE_FIELD_PTR(
                                          heap, mmap_free_tree_find_steps),
                                      steps);
                uhm_diag_note_u64_inc(
                    &heap->core.mmap_free_tree_find_budget_exceeded);
                uhm_diag_mark_treap_health(
                    heap, UHM_TREAP_HEALTH_MMAP_FREE_FIND);
                uhm_diag_note_max_u64(UHM_DIAG_CORE_FIELD_PTR(
                                          heap,
                                          mmap_free_tree_max_find_steps),
                                      steps);
            }
            return 0;
        }
        if (node->tree_right &&
            uhm_mmap_free_tree_child_max(node->tree_right) >= size) {
            node = node->tree_right;
        } else if (node->size >= size) {
            if (heap) {
                uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
                    heap, mmap_free_tree_find_calls));
                uhm_diag_note_u64_add(UHM_DIAG_CORE_FIELD_PTR(
                                          heap, mmap_free_tree_find_steps),
                                      steps);
                uhm_diag_note_max_u64(UHM_DIAG_CORE_FIELD_PTR(
                                          heap,
                                          mmap_free_tree_max_find_steps),
                                      steps);
                if (uhm_diag_note_tree_operation_budget(
                        &heap->core.mmap_free_tree_find_budget_exceeded,
                        heap->core.mmap_free_tree_current_nodes, steps)) {
                    uhm_diag_mark_treap_health(
                        heap, UHM_TREAP_HEALTH_MMAP_FREE_FIND);
                }
            }
            return node;
        } else {
            node = node->tree_left;
        }
    }
    if (heap) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_find_calls));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_find_steps),
            steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_max_find_steps),
            steps);
        if (uhm_diag_note_tree_operation_budget(
                &heap->core.mmap_free_tree_find_budget_exceeded,
                heap->core.mmap_free_tree_current_nodes, steps)) {
            uhm_diag_mark_treap_health(heap,
                                       UHM_TREAP_HEALTH_MMAP_FREE_FIND);
        }
    }
    return 0;
}

static uint64_t uhm_treap_next_repair_salt(uhm_heap *heap) {
    uint64_t seed;

    if (!heap) {
        return UINT64_C(0x6a09e667f3bcc909);
    }
    seed = heap->core.treap_priority_salt ^
           ((uint64_t)(uintptr_t)heap << 7) ^
           ((uint64_t)(uintptr_t)heap->core.vma_head >> 3) ^
           (heap->core.treap_repair_rehashes * UINT64_C(0x9e3779b97f4a7c15)) ^
           uhm_heap_generation_load(heap);
    seed = uhm_tree_priority_mix(seed);
    return seed != 0 ? seed : UINT64_C(0x6a09e667f3bcc909);
}

static size_t uhm_vma_tree_rebuild_locked(uhm_heap *heap) {
    uhm_vma *vma;
    size_t nodes = 0;

    if (!heap) {
        return 0;
    }
    heap->core.vma_tree_root = 0;
    heap->core.vma_tree_current_nodes = 0;
    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        if (vma->magic != UHM_VMA_MAGIC) {
            continue;
        }
        vma->tree_parent = 0;
        vma->tree_left = 0;
        vma->tree_right = 0;
        vma->tree_priority = uhm_tree_priority_mix_for_heap(
            heap, (uint64_t)(uintptr_t)vma,
            UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_mix_calls),
            UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_mix_ns),
            UHM_DIAG_CORE_FIELD_PTR(heap, vma_tree_priority_mix_max_ns));
    }
    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        if (vma->magic == UHM_VMA_MAGIC) {
            uhm_vma_tree_insert(heap, vma);
            ++nodes;
        }
    }
    return nodes;
}

static size_t uhm_mmap_free_tree_rebuild_locked(uhm_heap *heap) {
    uhm_free_extent *extent;
    uhm_free_extent *prev = 0;
    size_t nodes = 0;

    if (!heap) {
        return 0;
    }
    heap->core.mmap_free_tree_root = 0;
    heap->core.mmap_free_tree_current_nodes = 0;
    for (extent = heap->core.mmap_free_head; extent; extent = extent->next) {
        if (extent->magic != UHM_FREE_MAGIC) {
            continue;
        }
        extent->prev = prev;
        prev = extent;
        extent->flags |= UHM_META_FLAG_IN_MMAP_FREE_LIST;
        extent->tree_parent = 0;
        extent->tree_left = 0;
        extent->tree_right = 0;
        extent->tree_max_size = extent->size;
        extent->tree_priority = uhm_tree_priority_mix_for_heap(
            heap, (uint64_t)(uintptr_t)extent,
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_priority_mix_calls),
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_priority_mix_ns),
            UHM_DIAG_CORE_FIELD_PTR(heap, mmap_free_tree_priority_mix_max_ns));
    }
    for (extent = heap->core.mmap_free_head; extent; extent = extent->next) {
        if (extent->magic == UHM_FREE_MAGIC) {
            uhm_mmap_free_tree_insert(heap, extent);
            ++nodes;
        }
    }
    return nodes;
}

static uhm_result uhm_treap_rebuild_validate_locked(uhm_heap *heap) {
    uhm_block *block;
    uhm_vma *vma;
    uhm_free_extent *free_extent;
    size_t tree_count = 0;
    size_t list_count = 0;
    size_t busy_free_count = 0;
    uhm_free_extent *prev_free_extent = 0;

    if (!heap || !uhm_spinlock_is_held(&heap->lock)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!uhm_malloc_free_tree_validate(heap, heap->core.malloc_free_tree_root,
                                       0, &tree_count)) {
        return UHM_ERR_CORRUPTION;
    }
    for (block = heap->core.malloc_free_head; block; block = block->next_free) {
        ++list_count;
        if ((block->flags & UHM_BLOCK_FLAG_VM_BUSY) != 0) {
            ++busy_free_count;
        }
    }
    if (tree_count + busy_free_count != list_count) {
        return UHM_ERR_CORRUPTION;
    }

    tree_count = 0;
    if (!uhm_live_block_tree_validate(heap, heap->core.live_block_tree_root,
                                      0, &tree_count) ||
        heap->core.live_block_tree_current_nodes != tree_count) {
        return UHM_ERR_CORRUPTION;
    }

    tree_count = 0;
    if (!uhm_vma_tree_validate(heap, heap->core.vma_tree_root, 0,
                               &tree_count)) {
        return UHM_ERR_CORRUPTION;
    }
    list_count = 0;
    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        ++list_count;
    }
    if (tree_count != list_count) {
        return UHM_ERR_CORRUPTION;
    }

    tree_count = 0;
    if (!uhm_mmap_free_tree_validate(heap, heap->core.mmap_free_tree_root, 0,
                                     &tree_count)) {
        return UHM_ERR_CORRUPTION;
    }
    list_count = 0;
    prev_free_extent = 0;
    for (free_extent = heap->core.mmap_free_head; free_extent;
         free_extent = free_extent->next) {
        if (free_extent->prev != prev_free_extent) {
            return UHM_ERR_CORRUPTION;
        }
        ++list_count;
        prev_free_extent = free_extent;
    }
    if (tree_count != list_count || !uhm_mmap_recent_free_cache_valid(heap)) {
        return UHM_ERR_CORRUPTION;
    }
    return UHM_OK;
}

static uhm_result uhm_treap_rebuild_all_locked(uhm_heap *heap) {
    uint32_t pending;
    uhm_result result;
    size_t nodes = 0;

    if (!heap || !uhm_spinlock_is_held(&heap->lock)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    pending = atomic_load_explicit(&heap->core.treap_repair_pending_flags,
                                   UHM_ATOMIC_RELAXED);
    if (pending == 0) {
        return UHM_OK;
    }
    atomic_store_explicit(&heap->core.treap_repair_pending_flags,
                          0u, UHM_ATOMIC_RELAXED);
    heap->core.treap_priority_salt = uhm_treap_next_repair_salt(heap);
    ++heap->core.treap_repair_rehashes;
    nodes += uhm_malloc_trees_rebuild_locked(heap);
    nodes += uhm_vma_tree_rebuild_locked(heap);
    nodes += uhm_mmap_free_tree_rebuild_locked(heap);
    ++heap->core.treap_repair_rebuilds;
    heap->core.treap_repair_nodes += (uint64_t)nodes;
    if (atomic_load_explicit(&heap->core.treap_repair_pending_flags,
                             UHM_ATOMIC_RELAXED) != 0u) {
        ++heap->core.treap_repair_failures;
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_treap_rebuild_validate_locked(heap);
    if (result != UHM_OK) {
        ++heap->core.treap_repair_failures;
        return result;
    }
    return UHM_OK;
}

static void uhm_vma_insert_head(uhm_heap *heap, uhm_vma *vma) {
    vma->prev = 0;
    vma->next = heap->core.vma_head;
    if (heap->core.vma_head) {
        heap->core.vma_head->prev = vma;
    }
    heap->core.vma_head = vma;
    uhm_vma_tree_insert(heap, vma);
}

static void uhm_vma_insert_after(uhm_heap *heap, uhm_vma *existing, uhm_vma *vma) {
    vma->prev = existing;
    vma->next = existing->next;
    if (existing->next) {
        existing->next->prev = vma;
    }
    existing->next = vma;
    uhm_vma_tree_insert(heap, vma);
}

static void uhm_vma_insert_before(uhm_heap *heap, uhm_vma *existing, uhm_vma *vma) {
    vma->next = existing;
    vma->prev = existing->prev;
    if (existing->prev) {
        existing->prev->next = vma;
    } else {
        heap->core.vma_head = vma;
    }
    existing->prev = vma;
    uhm_vma_tree_insert(heap, vma);
}

static void uhm_vma_remove(uhm_heap *heap, uhm_vma *vma) {
    uhm_vma_tree_remove(heap, vma);
    if (vma->prev) {
        vma->prev->next = vma->next;
    } else {
        heap->core.vma_head = vma->next;
    }
    if (vma->next) {
        vma->next->prev = vma->prev;
    }
    vma->prev = 0;
    vma->next = 0;
    uhm_vma_release_node(heap, vma);
}

static uhm_vma *uhm_vma_tree_successor(uhm_vma *vma);
static uhm_vma *uhm_vma_tree_predecessor(uhm_vma *vma);

static int uhm_vma_can_merge(const uhm_vma *left, const uhm_vma *right) {
    uintptr_t left_end;

    return left && right &&
           left->magic == UHM_VMA_MAGIC &&
           right->magic == UHM_VMA_MAGIC &&
           !uhm_range_overflows(left->addr, left->size, &left_end) &&
           left_end == right->addr &&
           left->prot == right->prot &&
           (left->flags & ~UHM_META_FLAG_EMERGENCY) ==
               (right->flags & ~UHM_META_FLAG_EMERGENCY) &&
           uhm_vma_commit_state_from_flags(left->flags) ==
               uhm_vma_commit_state_from_flags(right->flags);
}

static uhm_result uhm_vma_merge_pair_with_lockset(
    uhm_heap *heap, uhm_vma *left, uhm_vma *right,
    const uhm_lockset *lockset, int *merged_out) {
    uintptr_t right_end;
    size_t merged_size;
    size_t merged_first;
    size_t merged_count;
    uint32_t commit_state;
    uint64_t merged_generation;
    int protecting = 0;
    uhm_result result;

    if (merged_out) {
        *merged_out = 0;
    }
    if (!uhm_vma_can_merge(left, right)) {
        return UHM_OK;
    }
    if (uhm_range_overflows(right->addr, right->size, &right_end)) {
        return UHM_ERR_CORRUPTION;
    }
    merged_size = (size_t)(right_end - left->addr);
    commit_state = uhm_vma_commit_state_from_flags(left->flags);
    if (lockset) {
        if (!uhm_page_span(heap, left->addr, merged_size, &merged_first,
                           &merged_count)) {
            return UHM_ERR_CORRUPTION;
        }
        result = uhm_page_span_ready_overlaps_va_state_locked(
            heap, merged_first, merged_count, UHM_PAGE_VA_PROTECTING,
            lockset, &protecting);
        if (result != UHM_OK) {
            return result;
        }
        if (protecting) {
            return UHM_OK;
        }
        merged_generation = uhm_heap_generation_next(heap);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, merged_first, merged_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, commit_state, left->prot,
            left->desc.id, merged_generation, lockset, 1);
        if (result != UHM_OK) {
            return result;
        }
        left->size = merged_size;
        left->generation = merged_generation;
        uhm_vma_descriptor_sync(left, commit_state);
    } else if (uhm_page_range_overlaps_va_state(
                   heap, left->addr, merged_size, UHM_PAGE_VA_PROTECTING)) {
        return UHM_OK;
    } else {
        left->size = merged_size;
        left->generation = uhm_heap_generation_next(heap);
        uhm_vma_descriptor_sync(left, commit_state);
        result = uhm_page_mark_range(heap, left->addr, left->size,
                                     UHM_PAGE_OWNER_MMAP,
                                     UHM_PAGE_VA_MMAP_LIVE, commit_state,
                                     left->prot, left->desc.id,
                                     left->generation, 0);
        if (result != UHM_OK) {
            return result;
        }
    }
    uhm_vma_remove(heap, right);
    if (merged_out) {
        *merged_out = 1;
    }
    return UHM_OK;
}

static uhm_result uhm_vma_merge_triple_with_lockset(
    uhm_heap *heap, uhm_vma *left, uhm_vma *middle, uhm_vma *right,
    const uhm_lockset *lockset, int *merged_out) {
    uintptr_t right_end;
    size_t merged_size;
    size_t merged_first;
    size_t merged_count;
    uint32_t commit_state;
    uint64_t merged_generation;
    int protecting = 0;
    uhm_result result;

    if (merged_out) {
        *merged_out = 0;
    }
    if (!uhm_vma_can_merge(left, middle) ||
        !uhm_vma_can_merge(middle, right)) {
        return UHM_OK;
    }
    if (uhm_range_overflows(right->addr, right->size, &right_end)) {
        return UHM_ERR_CORRUPTION;
    }
    merged_size = (size_t)(right_end - left->addr);
    commit_state = uhm_vma_commit_state_from_flags(left->flags);
    if (lockset) {
        if (!uhm_page_span(heap, left->addr, merged_size, &merged_first,
                           &merged_count)) {
            return UHM_ERR_CORRUPTION;
        }
        result = uhm_page_span_ready_overlaps_va_state_locked(
            heap, merged_first, merged_count, UHM_PAGE_VA_PROTECTING,
            lockset, &protecting);
        if (result != UHM_OK) {
            return result;
        }
        if (protecting) {
            return UHM_OK;
        }
        merged_generation = uhm_heap_generation_next(heap);
        result = uhm_page_span_mark_range_locked_prepared(
            heap, merged_first, merged_count, UHM_PAGE_OWNER_MMAP,
            UHM_PAGE_VA_MMAP_LIVE, commit_state, left->prot,
            left->desc.id, merged_generation, lockset, 1);
        if (result != UHM_OK) {
            return result;
        }
        left->size = merged_size;
        left->generation = merged_generation;
        uhm_vma_descriptor_sync(left, commit_state);
    } else if (uhm_page_range_overlaps_va_state(
                   heap, left->addr, merged_size, UHM_PAGE_VA_PROTECTING)) {
        return UHM_OK;
    } else {
        left->size = merged_size;
        left->generation = uhm_heap_generation_next(heap);
        uhm_vma_descriptor_sync(left, commit_state);
        result = uhm_page_mark_range(heap, left->addr, left->size,
                                     UHM_PAGE_OWNER_MMAP,
                                     UHM_PAGE_VA_MMAP_LIVE, commit_state,
                                     left->prot, left->desc.id,
                                     left->generation, 0);
        if (result != UHM_OK) {
            return result;
        }
    }
    uhm_vma_remove(heap, middle);
    uhm_vma_remove(heap, right);
    if (merged_out) {
        *merged_out = 1;
    }
    return UHM_OK;
}

static uhm_result uhm_vma_merge_around_with_lockset(
    uhm_heap *heap, uhm_vma **vma_inout, const uhm_lockset *lockset) {
    uhm_vma *vma;
    uhm_result result;
    int changed;

    if (!heap || !vma_inout || !*vma_inout) {
        return UHM_OK;
    }
    vma = *vma_inout;
    do {
        uhm_vma *prev = 0;
        uhm_vma *next = 0;
        changed = 0;

        prev = uhm_vma_tree_predecessor(vma);
        next = uhm_vma_tree_successor(vma);
        if (uhm_vma_can_merge(prev, vma) && uhm_vma_can_merge(vma, next)) {
            int merged = 0;
            result = uhm_vma_merge_triple_with_lockset(heap, prev, vma,
                                                       next, lockset,
                                                       &merged);
            if (result != UHM_OK) {
                return result;
            }
            if (merged) {
                vma = prev;
                changed = 1;
                continue;
            }
        }
        if (uhm_vma_can_merge(prev, vma)) {
            int merged = 0;
            result = uhm_vma_merge_pair_with_lockset(heap, prev, vma,
                                                     lockset, &merged);
            if (result != UHM_OK) {
                return result;
            }
            if (merged) {
                vma = prev;
                changed = 1;
                continue;
            }
        }

        if (uhm_vma_can_merge(vma, next)) {
            int merged = 0;
            result = uhm_vma_merge_pair_with_lockset(heap, vma, next,
                                                     lockset, &merged);
            if (result != UHM_OK) {
                return result;
            }
            if (merged) {
                changed = 1;
            }
        }
    } while (changed);
    *vma_inout = vma;
    return UHM_OK;
}

static int uhm_vma_range_overlaps_protecting(uhm_heap *heap,
                                             const uhm_vma *vma) {
    uintptr_t vma_end;

    if (!heap || !vma || vma->magic != UHM_VMA_MAGIC ||
        uhm_range_overflows(vma->addr, vma->size, &vma_end)) {
        return 1;
    }
    return uhm_page_range_overlaps_va_state(heap, vma->addr, vma->size,
                                            UHM_PAGE_VA_PROTECTING);
}

static int uhm_vma_range_list_overlaps_protecting(uhm_heap *heap,
                                                  uhm_vma *first,
                                                  uhm_vma *last) {
    uhm_vma *vma = first;

    if (!first || !last) {
        return 1;
    }
    for (;;) {
        if (uhm_vma_range_overlaps_protecting(heap, vma)) {
            return 1;
        }
        if (vma == last) {
            return 0;
        }
        vma = uhm_vma_tree_successor(vma);
        if (!vma) {
            return 1;
        }
    }
}

