static uhm_result uhm_thread_ctx_open_pinned(uhm_heap *heap,
                                             const uhm_thread_config *config,
                                             uhm_thread_ctx **out_ctx) {
    uhm_thread_ctx *ctx;

    if (!uhm_heap_is_accessible_pinned(heap) || !config || !out_ctx ||
        config->abi_major != UHM_ABI_MAJOR ||
        config->size < sizeof(*config) || !config->storage ||
        config->storage_size < sizeof(uhm_thread_ctx) ||
        ((uintptr_t)config->storage % alignof(uhm_thread_ctx)) != 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    ctx = (uhm_thread_ctx *)config->storage;
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic = UHM_THREAD_MAGIC;
    ctx->abi_major = UHM_ABI_MAJOR;
    ctx->abi_minor = UHM_ABI_MINOR;
    atomic_init(&ctx->lifecycle_state, 0u);
    uhm_metadata_reader_state_init(&ctx->metadata_reader);
    atomic_init(&ctx->cached_requested_bytes, 0u);
    ctx->owner = heap;
    ctx->owner_thread_id = uhm_current_thread_id();
    ctx->storage_base = (uintptr_t)config->storage;
    ctx->storage_size = config->storage_size;
    ctx->local_cache_limit = config->local_cache_bytes;
    atomic_init(&ctx->idle_miss_streak, 0u);
    ctx->retained_slab_page_limit = uhm_local_cache_retained_page_limit(
        uhm_local_cache_page_limit_for_bytes(heap,
                                             config->local_cache_bytes));
    atomic_init(&ctx->last_error, (int)UHM_OK);
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        atomic_store_explicit(&ctx->lifecycle_state, 0u,
                              UHM_ATOMIC_RELEASE);
        ctx->magic = 0;
        ctx->owner = 0;
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_metadata_epoch_advance(heap);
    atomic_store_explicit(&ctx->lifecycle_state, UHM_THREAD_MAGIC,
                          UHM_ATOMIC_RELEASE);
    ctx->prev_active = 0;
    ctx->next_active = heap->core.thread_ctx_head;
    if (heap->core.thread_ctx_head) {
        heap->core.thread_ctx_head->prev_active = ctx;
    }
    heap->core.thread_ctx_head = ctx;
    uhm_unlock(&heap->lock);
    *out_ctx = ctx;
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_thread_ctx_open(
    uhm_heap *heap,
    const uhm_thread_config *config,
    uhm_thread_ctx **out_ctx) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    result = uhm_thread_ctx_open_pinned(heap, config, out_ctx);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

static int uhm_thread_ctx_maybe_live(uhm_thread_ctx *ctx) {
    return ctx &&
           atomic_load_explicit(&ctx->lifecycle_state,
                                UHM_ATOMIC_ACQUIRE) == UHM_THREAD_MAGIC;
}

static int uhm_thread_ctx_is_current_owner_locked(uhm_heap *heap,
                                                  uhm_thread_ctx *ctx) {
    return uhm_heap_is_accessible_pinned(heap) && ctx &&
           ctx->owner == heap &&
           ctx->magic == UHM_THREAD_MAGIC &&
           ctx->owner_thread_id == uhm_current_thread_id();
}

static int uhm_thread_ctx_is_current_owner_pinned(uhm_heap *heap,
                                                  uhm_thread_ctx *ctx) {
    return uhm_heap_is_accessible_pinned(heap) && ctx &&
           atomic_load_explicit(&ctx->lifecycle_state,
                                UHM_ATOMIC_ACQUIRE) == UHM_THREAD_MAGIC &&
           ctx->owner == heap &&
           ctx->magic == UHM_THREAD_MAGIC &&
           ctx->owner_thread_id == uhm_current_thread_id();
}

static int uhm_thread_ctx_deferred_ptr_is_live(uhm_heap *heap, void *ptr) {
    return ptr &&
           (uhm_find_slab_slot_by_payload(heap, ptr, 1) != 0 ||
            uhm_find_live_block_by_payload_from(
                heap, ptr,
                UHM_LIVE_BLOCK_LOOKUP_CALLSITE_THREAD_CTX_DEFERRED) != 0);
}

static uhm_result uhm_thread_ctx_deferred_flush_locked(
    uhm_heap *heap,
    uhm_thread_ctx *ctx) {
    uhm_result result = UHM_OK;

    if (!heap || !ctx) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    while (ctx->deferred_free_count != 0) {
        void *ptr = ctx->deferred_free[ctx->deferred_free_count - 1u];
        uhm_result free_result;
        ctx->deferred_free[ctx->deferred_free_count - 1u] = 0;
        --ctx->deferred_free_count;
        free_result = uhm_free_owned_locked(heap, ptr);
        if (free_result != UHM_OK && result == UHM_OK) {
            result = free_result;
        }
    }
    if (ctx->deferred_free_count == 0) {
        ctx->deferred_free_epoch = heap->core.metadata_epoch;
    }
    return result;
}

static uhm_result uhm_thread_ctx_flush_local_locked(
    uhm_heap *heap, uhm_thread_ctx *ctx, int *maintenance_needed) {
    uhm_result result;
    uhm_result cache_result;

    if (maintenance_needed) {
        *maintenance_needed = 0;
    }
    if (!heap || !ctx) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    result = uhm_thread_ctx_deferred_flush_locked(heap, ctx);
    cache_result = uhm_ctx_cache_flush_slots_locked(heap, ctx);
    if (cache_result == UHM_OK && maintenance_needed) {
        *maintenance_needed = 1;
    }
    if (result == UHM_OK) {
        result = cache_result;
    }
    return result;
}

static uhm_result uhm_thread_ctx_flush_locked(uhm_heap *heap,
                                              uhm_thread_ctx *ctx) {
    uhm_result result;
    uhm_result maintenance_result = UHM_OK;
    int maintenance_needed = 0;

    result = uhm_thread_ctx_flush_local_locked(heap, ctx,
                                               &maintenance_needed);
    if (maintenance_needed) {
        maintenance_result = uhm_slab_drain_remote_all(
            heap, !uhm_lockless_remote_free_enabled(heap));
        if (maintenance_result == UHM_OK) {
            maintenance_result = uhm_slab_maybe_decommit_empty_all(heap);
        }
    }
    if (result == UHM_OK) {
        result = maintenance_result;
    }
    return result;
}

static void uhm_thread_ctx_close_pinned(uhm_heap *heap, uhm_thread_ctx *ctx) {
    uhm_result result;

    if (!uhm_heap_is_accessible_pinned(heap) || !uhm_thread_ctx_maybe_live(ctx)) {
        return;
    }
    uhm_lock(&heap->lock);
    if (!uhm_thread_ctx_is_current_owner_locked(heap, ctx)) {
        uhm_unlock(&heap->lock);
        return;
    }
    if (uhm_metadata_reader_active(&ctx->metadata_reader)) {
        atomic_store_explicit(&ctx->last_error, (int)UHM_ERR_BUSY,
                              UHM_ATOMIC_RELEASE);
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, UHM_ERR_BUSY);
        return;
    }
    uhm_metadata_epoch_advance(heap);
    result = uhm_thread_ctx_flush_locked(heap, ctx);
    uhm_thread_ctx_detach_locked(heap, ctx);
    uhm_unlock(&heap->lock);
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
    }
}

static uhm_result uhm_thread_ctx_flush_pinned(uhm_heap *heap,
                                              uhm_thread_ctx *ctx) {
    uhm_result result;

    if (!uhm_heap_is_accessible_pinned(heap) ||
        !uhm_thread_ctx_maybe_live(ctx)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_lock(&heap->lock);
    if (!uhm_thread_ctx_is_current_owner_locked(heap, ctx)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_metadata_epoch_advance(heap);
    result = uhm_thread_ctx_flush_locked(heap, ctx);
    uhm_unlock(&heap->lock);
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
    }
    return result;
}

static uhm_result UHM_CALL uhm_api_thread_ctx_flush(uhm_heap *heap,
                                                    uhm_thread_ctx *ctx) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_thread_ctx_flush_pinned(heap, ctx);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

static void UHM_CALL uhm_api_thread_ctx_close(uhm_heap *heap, uhm_thread_ctx *ctx) {
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return;
    }
    uhm_thread_ctx_close_pinned(heap, ctx);
    uhm_heap_op_end(heap, op_shard);
}

static void *uhm_malloc_ctx_pinned(uhm_heap *heap, uhm_thread_ctx *ctx,
                                   size_t size) {
    size_t total;
    size_t payload_size;
    uint16_t size_class;
    uint16_t fit_size_class = UHM_SIZE_CLASS_COUNT;
    uhm_result result;
    void *ptr;

    if (!uhm_heap_is_accessible_pinned(heap) || !uhm_thread_ctx_maybe_live(ctx)) {
        return 0;
    }
    result = uhm_prepare_alloc_size(UHM_MIN_ALIGNMENT, size, &total,
                                    &payload_size, &size_class);
    (void)total;
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
        return 0;
    }
    if (!uhm_thread_ctx_is_current_owner_pinned(heap, ctx)) {
        return 0;
    }
    if (uhm_size_class_uses_slab(size_class, payload_size)) {
        if (uhm_local_cache_find_fit_class(
                ctx->nonempty_class_mask, size_class, &fit_size_class)) {
            ptr = uhm_ctx_cache_pop_partial_lowlock(
                heap, ctx, fit_size_class, payload_size, size);
            if (ptr) {
                return ptr;
            }
            uhm_lock(&heap->lock);
            if (!uhm_thread_ctx_is_current_owner_locked(heap, ctx)) {
                uhm_unlock(&heap->lock);
                return 0;
            }
            ptr = uhm_ctx_cache_pop(heap, ctx, fit_size_class, payload_size,
                                    size);
            uhm_unlock(&heap->lock);
            if (ptr) {
                return ptr;
            }
        }
    }
    uhm_ctx_cache_note_idle_miss(heap, ctx);
    return uhm_aligned_alloc_pinned(heap, UHM_MIN_ALIGNMENT, size);
}

static void *UHM_CALL uhm_api_malloc_ctx(uhm_heap *heap, uhm_thread_ctx *ctx,
                                         size_t size) {
    void *ptr;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return 0;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return 0;
    }
    ptr = uhm_malloc_ctx_pinned(heap, ctx, size);
    uhm_heap_op_end(heap, op_shard);
    return ptr;
}

static void uhm_free_ctx_pinned(uhm_heap *heap, uhm_thread_ctx *ctx, void *ptr) {
    uhm_slab_slot *slot;
    uhm_result result;

    if (!uhm_heap_is_accessible_pinned(heap) || !uhm_thread_ctx_maybe_live(ctx)) {
        return;
    }
    if (!ptr) {
        return;
    }
    if (!uhm_thread_ctx_is_current_owner_pinned(heap, ctx)) {
        return;
    }
    if (uhm_ctx_cache_push_partial_lowlock(heap, ctx, ptr)) {
        return;
    }
    uhm_lock(&heap->lock);
    if (!uhm_thread_ctx_is_current_owner_locked(heap, ctx)) {
        uhm_unlock(&heap->lock);
        return;
    }
    slot = uhm_find_slab_slot_by_payload(heap, ptr, 1);
    if (slot) {
        uhm_slab *slab = uhm_slab_slot_owner(slot);
        size_t slot_bytes = slab ? slab->slot_size : 0u;
        int can_accept = uhm_ctx_cache_can_accept(heap, ctx, slot, 0);

        if (!can_accept && slab && slot_bytes != 0u &&
            slot_bytes <= ctx->local_cache_limit &&
            ctx->local_cache_bytes <=
                ctx->local_cache_limit - slot_bytes &&
            uhm_ctx_cache_evict_cold_slab_locked(
                heap, ctx, slab) == UHM_OK) {
            can_accept = uhm_ctx_cache_can_accept(heap, ctx, slot, 0);
        }
        if (can_accept) {
            result = uhm_ctx_cache_push(heap, ctx, slot);
            uhm_unlock(&heap->lock);
            if (result != UHM_OK) {
                uhm_set_error(heap, result);
            }
            return;
        }
    }
    uhm_unlock(&heap->lock);
    uhm_free_pinned(heap, ptr);
}

static void UHM_CALL uhm_api_free_ctx(uhm_heap *heap, uhm_thread_ctx *ctx, void *ptr) {
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return;
    }
    if (!ptr) {
        uhm_heap_op_end(heap, op_shard);
        return;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return;
    }
    uhm_free_ctx_pinned(heap, ctx, ptr);
    uhm_heap_op_end(heap, op_shard);
}

static uhm_result uhm_free_deferred_ctx_pinned(uhm_heap *heap,
                                               uhm_thread_ctx *ctx,
                                               void *ptr) {
    uhm_result result = UHM_OK;

    if (!uhm_heap_is_accessible_pinned(heap) ||
        !uhm_thread_ctx_maybe_live(ctx)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!ptr) {
        return UHM_OK;
    }
    uhm_lock(&heap->lock);
    if (!uhm_thread_ctx_is_current_owner_locked(heap, ctx)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_metadata_epoch_advance(heap);
    if (!uhm_thread_ctx_deferred_ptr_is_live(heap, ptr)) {
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, UHM_ERR_CORRUPTION);
        return UHM_ERR_CORRUPTION;
    }
    {
        size_t i;
        for (i = 0; i < ctx->deferred_free_count; ++i) {
            if (ctx->deferred_free[i] == ptr) {
                uhm_unlock(&heap->lock);
                uhm_set_error(heap, UHM_ERR_CORRUPTION);
                return UHM_ERR_CORRUPTION;
            }
        }
    }
    if (ctx->deferred_free_count >= UHM_THREAD_CTX_DEFERRED_FREE_SLOTS) {
        result = uhm_thread_ctx_deferred_flush_locked(heap, ctx);
    }
    if (result == UHM_OK) {
        if (ctx->deferred_free_count >= UHM_THREAD_CTX_DEFERRED_FREE_SLOTS) {
            result = UHM_ERR_OUT_OF_MEMORY;
        } else {
            ctx->deferred_free[ctx->deferred_free_count++] = ptr;
            ctx->deferred_free_epoch = heap->core.metadata_epoch;
        }
    }
    uhm_unlock(&heap->lock);
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
    }
    return result;
}

static uhm_result UHM_CALL uhm_api_free_deferred_ctx(uhm_heap *heap,
                                                     uhm_thread_ctx *ctx,
                                                     void *ptr) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!ptr) {
        result = uhm_free_deferred_ctx_pinned(heap, ctx, ptr);
        uhm_heap_op_end(heap, op_shard);
        return result;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_free_deferred_ctx_pinned(heap, ctx, ptr);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

