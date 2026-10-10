static int uhm_ctx_cache_slot_is_valid(uhm_slab_slot *slot) {
    uintptr_t slot_addr;
    uintptr_t slab_end;
    size_t offset;
    uhm_slab *slab;
    uint32_t flags;
    uint32_t cached_requested;

    slab = uhm_slab_slot_owner(slot);
    if (!slot || !slab) {
        return 0;
    }
    flags = uhm_slab_slot_flags_load(slot);
    slot_addr = (uintptr_t)slot;
    if (slab->magic != UHM_SLAB_MAGIC ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
        uhm_range_overflows(slab->base, slab->size, &slab_end) ||
        slot_addr < slab->base || slot_addr >= slab_end) {
        return 0;
    }
    offset = (size_t)(slot_addr - slab->base);
    return (offset % slab->slot_stride) == 0 &&
           slot->magic == UHM_SLAB_SLOT_MAGIC &&
           slot->size_class == slab->size_class &&
           slot->state == 0 &&
           uhm_slab_slot_requested_load(slot) == 0 &&
           uhm_slab_slot_ctx_cache_state_valid_with_slab(
               slot, slab, &cached_requested) &&
           (flags & (UHM_SLAB_SLOT_FLAG_FREE |
                     UHM_SLAB_SLOT_FLAG_CTX_CACHE)) ==
               (UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_CTX_CACHE) &&
           (flags & ~(UHM_SLAB_SLOT_FLAG_FREE |
                      UHM_SLAB_SLOT_FLAG_CTX_CACHE)) == 0;
}

static int uhm_local_page_hints_validate(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    const uhm_local_page_hint *hints) {
    size_t i;

    if (!heap || !heads || !hints || heap->F.vm.page_size == 0) {
        return 0;
    }
    for (i = 0; i < UHM_LOCAL_PAGE_HINT_COUNT; ++i) {
        uhm_slab_slot *anchor = hints[i].anchor;
        uhm_page_record *record = hints[i].record;
        uhm_slab *slab;
        uhm_slab_slot *cursor;
        uhm_page_record *actual;
        size_t page_index;
        size_t set_base;
        size_t shard;
        int found = 0;

        if (!anchor || !record) {
            if (anchor || record) {
                return 0;
            }
            if ((i % UHM_LOCAL_PAGE_HINT_WAYS) + 1u <
                    UHM_LOCAL_PAGE_HINT_WAYS &&
                i + 1u < UHM_LOCAL_PAGE_HINT_COUNT &&
                (hints[i + 1u].anchor || hints[i + 1u].record)) {
                return 0;
            }
            continue;
        }
        if (!uhm_ctx_cache_slot_is_valid(anchor) ||
            anchor->size_class >= UHM_SIZE_CLASS_COUNT) {
            return 0;
        }
        for (cursor = heads[anchor->size_class]; cursor;
             cursor = cursor->next_free) {
            if (cursor == anchor) {
                found = 1;
                break;
            }
        }
        slab = uhm_slab_slot_owner(anchor);
        if (!found || !slab || slab->base < heap->core.arena_base ||
            slab->base >= heap->core.arena_end) {
            return 0;
        }
        page_index =
            (size_t)(slab->base - heap->core.arena_base) /
            heap->F.vm.page_size;
        set_base =
            (page_index & (UHM_LOCAL_PAGE_HINT_SET_COUNT - 1u)) *
            UHM_LOCAL_PAGE_HINT_WAYS;
        if (page_index >= heap->core.page_count ||
            i < set_base || i >= set_base + UHM_LOCAL_PAGE_HINT_WAYS) {
            return 0;
        }
        if (i > set_base && hints[i - 1u].anchor &&
            uhm_slab_slot_owner(hints[i - 1u].anchor) == slab) {
            return 0;
        }
        shard = uhm_page_shard_for_index(page_index);
        uhm_lock(&heap->core.page_map_locks[shard].lock);
        actual = uhm_page_record_get_unlocked(heap, page_index);
        uhm_unlock(&heap->core.page_map_locks[shard].lock);
        if (actual != record || record->owner != UHM_PAGE_OWNER_MALLOC ||
            record->va_state != UHM_PAGE_VA_MALLOC_LIVE ||
            record->commit_state != UHM_PAGE_COMMIT_LIVE ||
            record->prot != (UHM_PROT_READ | UHM_PROT_WRITE) ||
            record->descriptor_id != slab->desc.id ||
            record->generation != slab->generation) {
            return 0;
        }
    }
    return 1;
}

static int uhm_thread_ctx_cache_validate(uhm_heap *heap,
                                         uhm_thread_ctx *ctx) {
    uhm_slab_slot *slot;
    size_t count = 0;
    size_t bytes = 0;
    size_t cached_requested_bytes = 0;
    size_t retained_slab_pages = 0;
    size_t max_count;
    int head_seen = 0;
    size_t i;

    if (!heap || !ctx) {
        return 0;
    }
    if (ctx->local_cache_limit == 0) {
        max_count = 0;
    } else {
        max_count = ctx->local_cache_limit / UHM_MIN_ALIGNMENT + 1u;
    }
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        for (slot = ctx->local_cache_heads[i]; slot; slot = slot->next_free) {
            size_t slot_size;
            if (count > max_count ||
                !uhm_ctx_cache_slot_is_valid(slot) ||
                slot->size_class != i) {
                return 0;
            }
            slot_size = uhm_slab_slot_owner_slot_size(slot);
            if (cached_requested_bytes >
                SIZE_MAX - slot->requested_size) {
                return 0;
            }
            cached_requested_bytes += slot->requested_size;
            if (bytes > SIZE_MAX - slot_size) {
                return 0;
            }
            bytes += slot_size;
            ++count;
            if (slot == ctx->local_cache_head) {
                head_seen = 1;
            }
        }
    }
    if (!uhm_local_cache_retained_pages_calculate(
            heap, ctx->local_cache_heads, &retained_slab_pages)) {
        return 0;
    }
    return count == ctx->local_cache_count &&
           ctx->nonempty_class_mask ==
               uhm_local_cache_nonempty_mask_calculate(
                   ctx->local_cache_heads) &&
           ((count == 0 && ctx->local_cache_head == 0) ||
            (count != 0 && head_seen)) &&
           bytes == ctx->local_cache_bytes &&
           cached_requested_bytes ==
               atomic_load_explicit(&ctx->cached_requested_bytes,
                                    UHM_ATOMIC_ACQUIRE) &&
           bytes <= ctx->local_cache_limit &&
           retained_slab_pages == ctx->retained_slab_pages &&
           retained_slab_pages <= ctx->retained_slab_page_limit &&
           atomic_load_explicit(&ctx->idle_miss_streak,
                                UHM_ATOMIC_ACQUIRE) <
               UHM_LOCAL_CACHE_IDLE_MISS_THRESHOLD &&
           ctx->retained_slab_page_limit ==
               uhm_local_cache_retained_page_limit(
                   uhm_local_cache_page_limit_for_bytes(
                       heap, ctx->local_cache_limit)) &&
           uhm_local_cache_retained_hints_validate(
               heap, ctx->local_cache_heads, ctx->local_retained_hints) &&
           uhm_local_page_hints_validate(
               heap, ctx->local_cache_heads, ctx->local_page_hints);
}

static int uhm_thread_ctx_deferred_validate(uhm_heap *heap,
                                            uhm_thread_ctx *ctx) {
    size_t i;

    if (!heap || !ctx ||
        ctx->deferred_free_count > UHM_THREAD_CTX_DEFERRED_FREE_SLOTS ||
        ctx->deferred_free_epoch > heap->core.metadata_epoch) {
        return 0;
    }
    for (i = 0; i < ctx->deferred_free_count; ++i) {
        size_t j;
        void *ptr = ctx->deferred_free[i];
        if (!ptr || !uhm_thread_ctx_deferred_ptr_is_live(heap, ptr)) {
            return 0;
        }
        for (j = i + 1u; j < ctx->deferred_free_count; ++j) {
            if (ctx->deferred_free[j] == ptr) {
                return 0;
            }
        }
    }
    for (i = ctx->deferred_free_count;
         i < UHM_THREAD_CTX_DEFERRED_FREE_SLOTS; ++i) {
        if (ctx->deferred_free[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int uhm_thread_ctx_list_validate(uhm_heap *heap) {
    uhm_thread_ctx *slow;
    uhm_thread_ctx *fast;
    uhm_thread_ctx *ctx;
    uhm_thread_ctx *prev = 0;

    slow = heap->core.thread_ctx_head;
    fast = heap->core.thread_ctx_head;
    while (fast && fast->next_active) {
        slow = slow->next_active;
        fast = fast->next_active->next_active;
        if (slow == fast) {
            return 0;
        }
    }
    for (ctx = heap->core.thread_ctx_head; ctx; ctx = ctx->next_active) {
        if (ctx->magic != UHM_THREAD_MAGIC ||
            ctx->owner != heap ||
            ctx->prev_active != prev ||
            !uhm_metadata_reader_state_validate_locked(
                heap, &ctx->metadata_reader) ||
            !uhm_thread_ctx_cache_validate(heap, ctx) ||
            !uhm_thread_ctx_deferred_validate(heap, ctx)) {
            return 0;
        }
        prev = ctx;
    }
    return 1;
}

static int uhm_tls_cache_validate(uhm_heap *heap, uhm_tls_cache *cache) {
    uhm_slab_slot *slot;
    size_t count = 0;
    size_t bytes = 0;
    size_t cached_requested_bytes = 0;
    size_t retained_slab_pages = 0;
    size_t max_count;
    int head_seen = 0;
    size_t i;

    if (!heap || !cache) {
        return 0;
    }
    if (cache->local_cache_limit == 0) {
        max_count = 0;
    } else {
        max_count = cache->local_cache_limit / UHM_MIN_ALIGNMENT + 1u;
    }
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        for (slot = cache->local_cache_heads[i]; slot; slot = slot->next_free) {
            size_t slot_size;
            if (count > max_count ||
                !uhm_ctx_cache_slot_is_valid(slot) ||
                slot->size_class != i) {
                return 0;
            }
            slot_size = uhm_slab_slot_owner_slot_size(slot);
            if (cached_requested_bytes >
                SIZE_MAX - slot->requested_size) {
                return 0;
            }
            cached_requested_bytes += slot->requested_size;
            if (bytes > SIZE_MAX - slot_size) {
                return 0;
            }
            bytes += slot_size;
            ++count;
            if (slot == cache->local_cache_head) {
                head_seen = 1;
            }
        }
    }
    if (!uhm_local_cache_retained_pages_calculate(
            heap, cache->local_cache_heads, &retained_slab_pages)) {
        return 0;
    }
    return cache->magic == UHM_TLS_CACHE_MAGIC &&
           cache->owner == heap &&
           atomic_load_explicit(&cache->owner_ref, UHM_ATOMIC_ACQUIRE) ==
               (uintptr_t)heap &&
           atomic_load_explicit(&cache->lifecycle_state,
                                UHM_ATOMIC_ACQUIRE) == UHM_TLS_CACHE_MAGIC &&
           uhm_metadata_reader_state_validate_locked(
               heap, &cache->metadata_reader) &&
           cache->local_cache_count == count &&
           cache->nonempty_class_mask ==
               uhm_local_cache_nonempty_mask_calculate(
                   cache->local_cache_heads) &&
           ((count == 0 && cache->local_cache_head == 0) ||
            (count != 0 && head_seen)) &&
           cache->local_cache_bytes == bytes &&
           atomic_load_explicit(&cache->cached_requested_bytes,
                                UHM_ATOMIC_ACQUIRE) ==
               cached_requested_bytes &&
           cache->local_cache_bytes <= cache->local_cache_limit &&
           retained_slab_pages == cache->retained_slab_pages &&
           retained_slab_pages <= cache->retained_slab_page_limit &&
           cache->retained_slab_page_limit ==
               heap->core.tls_cache_max_retained_pages &&
           (atomic_load_explicit(&cache->idle_miss_streak,
                                 UHM_ATOMIC_ACQUIRE) &
            UHM_LOCAL_CACHE_IDLE_STREAK_MASK) <
               UHM_LOCAL_CACHE_IDLE_MISS_THRESHOLD &&
           cache->adaptive_pages <= heap->core.tls_cache_max_pages &&
           uhm_local_cache_retained_hints_validate(
               heap, cache->local_cache_heads,
               cache->local_retained_hints) &&
           uhm_local_page_hints_validate(
               heap, cache->local_cache_heads, cache->local_page_hints);
}

static int uhm_tls_cache_list_validate(uhm_heap *heap) {
    uhm_tls_cache *slow;
    uhm_tls_cache *fast;
    uhm_tls_cache *cache;
    uhm_tls_cache *prev = 0;
    size_t active_count = 0;
    size_t free_count = 0;

    if (!heap) {
        return 0;
    }
    slow = heap->core.tls_cache_head;
    fast = heap->core.tls_cache_head;
    while (fast && fast->next_active) {
        slow = slow->next_active;
        fast = fast->next_active->next_active;
        if (slow == fast) {
            return 0;
        }
    }
    for (cache = heap->core.tls_cache_head; cache; cache = cache->next_active) {
        if (active_count >= heap->core.tls_cache_slot_count ||
            cache->prev_active != prev ||
            !uhm_tls_cache_validate(heap, cache)) {
            return 0;
        }
        prev = cache;
        ++active_count;
    }
    slow = heap->tls_cache_free_head;
    fast = heap->tls_cache_free_head;
    while (fast && fast->next_free) {
        slow = slow->next_free;
        fast = fast->next_free->next_free;
        if (slow == fast) {
            return 0;
        }
    }
    for (cache = heap->tls_cache_free_head; cache; cache = cache->next_free) {
        if (free_count >= heap->core.tls_cache_slot_count ||
            cache->magic != 0 ||
            cache->owner != 0 ||
            atomic_load_explicit(&cache->owner_ref, UHM_ATOMIC_ACQUIRE) !=
                (uintptr_t)0 ||
            cache->local_cache_head != 0) {
            return 0;
        }
        {
            size_t i;
            for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
                if (cache->local_cache_heads[i] != 0) {
                    return 0;
                }
            }
        }
        ++free_count;
    }
    return active_count == heap->core.tls_cache_active_count &&
           active_count + free_count == heap->core.tls_cache_slot_count;
}

static int uhm_ctx_cache_count_for_slab(uhm_heap *heap, uhm_slab *target,
                                        size_t *count_out) {
    uhm_thread_ctx *ctx;
    uhm_tls_cache *cache;
    size_t count = 0;

    if (!heap || !target || !count_out) {
        return 0;
    }
    for (ctx = heap->core.thread_ctx_head; ctx; ctx = ctx->next_active) {
        size_t i;
        for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
            uhm_slab_slot *slot;
            for (slot = ctx->local_cache_heads[i]; slot;
                 slot = slot->next_free) {
                if (!uhm_ctx_cache_slot_is_valid(slot) ||
                    slot->size_class != i) {
                    return 0;
                }
                if (uhm_slab_slot_belongs_to(slot, target)) {
                    ++count;
                }
            }
        }
    }
    for (cache = heap->core.tls_cache_head; cache; cache = cache->next_active) {
        size_t i;
        for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
            uhm_slab_slot *slot;
            for (slot = cache->local_cache_heads[i]; slot;
                 slot = slot->next_free) {
                if (!uhm_ctx_cache_slot_is_valid(slot) ||
                    slot->size_class != i) {
                    return 0;
                }
                if (uhm_slab_slot_belongs_to(slot, target)) {
                    ++count;
                }
            }
        }
    }
    *count_out = count;
    return 1;
}
