static uhm_result UHM_CALL uhm_api_check(uhm_heap *heap, uint32_t check_flags) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_check_pinned(heap, check_flags);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

static uint64_t uhm_leak_tree_step_limit(const uhm_heap *heap) {
    uint64_t limit = heap ? heap->core.live_block_tree_current_nodes : 0;

    return limit == UINT64_MAX ? limit : limit + UINT64_C(1);
}

static uhm_block *uhm_leak_live_tree_leftmost(uhm_heap *heap,
                                              uhm_block *node,
                                              int *corrupt) {
    uint64_t steps = 0;
    const uint64_t limit = uhm_leak_tree_step_limit(heap);

    while (node && node->live_tree_left) {
        if (++steps > limit) {
            if (corrupt) {
                *corrupt = 1;
            }
            return 0;
        }
        node = node->live_tree_left;
    }
    return node;
}

static uhm_block *uhm_leak_live_tree_successor(uhm_heap *heap,
                                               uhm_block *node,
                                               int *corrupt) {
    uhm_block *child;
    uhm_block *parent;
    uint64_t steps = 0;
    const uint64_t limit = uhm_leak_tree_step_limit(heap);

    if (!node) {
        return 0;
    }
    if (node->live_tree_right) {
        return uhm_leak_live_tree_leftmost(heap, node->live_tree_right,
                                           corrupt);
    }
    child = node;
    parent = node->live_tree_parent;
    while (parent && child == parent->live_tree_right) {
        if (++steps > limit) {
            if (corrupt) {
                *corrupt = 1;
            }
            return 0;
        }
        child = parent;
        parent = parent->live_tree_parent;
    }
    if (parent && parent->live_tree_left != child) {
        if (corrupt) {
            *corrupt = 1;
        }
        return 0;
    }
    return parent;
}

static uhm_result uhm_enumerate_leaks_pinned(uhm_heap *heap,
                                             uhm_leak_callback callback,
                                             void *user) {
    uhm_block *block;
    uhm_slab *slab;
    uhm_vma *vma;
    uhm_result result = UHM_OK;
    uint64_t live_seen = 0;
    int tree_corrupt = 0;

    if (!uhm_heap_is_accessible_pinned(heap) || !callback) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    block = uhm_leak_live_tree_leftmost(heap, heap->core.live_block_tree_root,
                                        &tree_corrupt);
    while (block) {
        uhm_block *next;

        if (++live_seen > heap->core.live_block_tree_current_nodes) {
            tree_corrupt = 1;
            break;
        }
        next = uhm_leak_live_tree_successor(heap, block, &tree_corrupt);
        if (block->magic == UHM_BLOCK_MAGIC &&
            (block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
            block->requested_size != 0) {
            uhm_leak_record record;
            memset(&record, 0, sizeof(record));
            record.size = sizeof(record);
            record.abi_major = UHM_ABI_MAJOR;
            record.abi_minor = UHM_ABI_MINOR;
            record.kind = UHM_LEAK_KIND_MALLOC;
            record.prot = UHM_PROT_READ | UHM_PROT_WRITE;
            record.addr = uhm_block_payload(block);
            record.requested_size = block->requested_size;
            record.allocated_size = block->total_size - uhm_malloc_header_size();
            record.generation = block->generation;
            if ((block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) != 0) {
                record.flags |= UHM_LEAK_FLAG_PAGE_PRIVATE;
            }
            result = callback(user, &record);
            if (result != UHM_OK) {
                uhm_unlock(&heap->lock);
                return result;
            }
        }
        if (tree_corrupt) {
            break;
        }
        block = next;
    }
    if (tree_corrupt || live_seen != heap->core.live_block_tree_current_nodes) {
        uhm_unlock(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }

    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        size_t i;
        if (slab->magic != UHM_SLAB_MAGIC ||
            (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0) {
            continue;
        }
        for (i = 0; i < slab->total_slots; ++i) {
            uhm_slab_slot *slot =
                (uhm_slab_slot *)(slab->base + i * slab->slot_stride);
            uint32_t slot_flags = uhm_slab_slot_flags_load(slot);
            uint32_t slot_requested = uhm_slab_slot_requested_load(slot);
            if (slot->magic == UHM_SLAB_SLOT_MAGIC &&
                uhm_slab_slot_state_valid_for_check(slot) &&
                (slot_flags & UHM_SLAB_SLOT_FLAG_FREE) == 0 &&
                slot_requested != 0) {
                uhm_leak_record record;
                memset(&record, 0, sizeof(record));
                record.size = sizeof(record);
                record.abi_major = UHM_ABI_MAJOR;
                record.abi_minor = UHM_ABI_MINOR;
                record.kind = UHM_LEAK_KIND_MALLOC;
                record.prot = UHM_PROT_READ | UHM_PROT_WRITE;
                record.addr = uhm_slab_slot_payload(slot);
                record.requested_size = slot_requested;
                record.allocated_size = slab->slot_size;
                record.generation = slab->generation;
                result = callback(user, &record);
                if (result != UHM_OK) {
                    uhm_unlock(&heap->lock);
                    return result;
                }
            }
        }
    }

    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        if (vma->magic == UHM_VMA_MAGIC) {
            uhm_leak_record record;
            memset(&record, 0, sizeof(record));
            record.size = sizeof(record);
            record.abi_major = UHM_ABI_MAJOR;
            record.abi_minor = UHM_ABI_MINOR;
            record.flags = vma->flags & ~UHM_META_FLAG_EMERGENCY;
            record.kind = UHM_LEAK_KIND_MMAP;
            record.prot = vma->prot;
            record.addr = (void *)vma->addr;
            record.requested_size = vma->size;
            record.allocated_size = vma->size;
            record.generation = vma->generation;
            result = callback(user, &record);
            if (result != UHM_OK) {
                uhm_unlock(&heap->lock);
                return result;
            }
        }
    }

    uhm_unlock(&heap->lock);
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_enumerate_leaks(uhm_heap *heap,
                                                   uhm_leak_callback callback,
                                                   void *user) {
    uhm_result result;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    result = uhm_enumerate_leaks_pinned(heap, callback, user);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

static uhm_result UHM_CALL uhm_api_get_last_error(uhm_heap *heap,
                                                  uhm_result *out_error,
                                                  uint32_t *out_platform_error) {
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!out_error && !out_platform_error) {
        uhm_heap_op_end(heap, op_shard);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (out_error) {
        *out_error =
            (uhm_result)atomic_load_explicit(&heap->last_error, UHM_ATOMIC_ACQUIRE);
    }
    if (out_platform_error) {
        *out_platform_error =
            atomic_load_explicit(&heap->platform_error, UHM_ATOMIC_ACQUIRE);
    }
    uhm_heap_op_end(heap, op_shard);
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_close(uhm_heap *heap) {
    uhm_result result;
    uhm_result maintenance_result = UHM_OK;
    uint32_t expected = UHM_MAGIC;
    int tls_maintenance_needed = 0;
    int ctx_maintenance_needed = 0;
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_close_phase_sample phase_sample;
#endif

    if (!heap || heap->F.canary != UHM_HEAP_F_CANARY ||
        !atomic_compare_exchange_strong_explicit(
            &heap->lifecycle_state, &expected, UHM_CLOSING_MAGIC,
            UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_heap_wait_for_ops(heap);
    uhm_lock(&heap->lock);
    if (uhm_metadata_registered_reader_active_locked(heap)) {
        result = UHM_ERR_BUSY;
        uhm_set_error(heap, result);
        atomic_store_explicit(&heap->lifecycle_state, UHM_MAGIC,
                              UHM_ATOMIC_RELEASE);
        uhm_unlock(&heap->lock);
        return result;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    phase_sample = uhm_diag_close_phase_begin(heap);
#endif
    uhm_metadata_epoch_advance(heap);
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_close_phase_end(heap, UHM_CLOSE_PHASE_METADATA_EPOCH,
                             &phase_sample);
    phase_sample = uhm_diag_close_phase_begin(heap);
#endif
    result = uhm_tls_cache_flush_all_local_locked(
        heap, &tls_maintenance_needed);
    if (result == UHM_OK) {
        result = uhm_thread_ctx_flush_all_local_locked(
            heap, &ctx_maintenance_needed);
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_close_phase_end(heap, UHM_CLOSE_PHASE_LOCAL_REGISTRY_DRAIN,
                             &phase_sample);
#endif
    if (tls_maintenance_needed || ctx_maintenance_needed) {
#if UHMAS_ENABLE_DIAGNOSTICS
        phase_sample = uhm_diag_close_phase_begin(heap);
#endif
        maintenance_result = uhm_slab_drain_remote_all(
            heap, !uhm_lockless_remote_free_enabled(heap));
        if (maintenance_result == UHM_OK) {
            maintenance_result = uhm_slab_maybe_decommit_empty_all(heap);
        }
#if UHMAS_ENABLE_DIAGNOSTICS
        uhm_diag_close_phase_end(
            heap, UHM_CLOSE_PHASE_FALLBACK_MAINTENANCE, &phase_sample);
#endif
        if (result == UHM_OK) {
            result = maintenance_result;
        }
    }
    if (result == UHM_OK) {
        /*
         * Final class-queue publish deliberately has no trailing decommit
         * pass. An owned arena is released in full immediately below, while
         * a pre-reserved arena remains under the caller's lifetime policy and
         * close may still leave live allocations in it. Selectively
         * decommitting only slabs made empty by this publish would add a new
         * VM failure point without providing complete arena teardown.
         */
#if UHMAS_ENABLE_DIAGNOSTICS
        phase_sample = uhm_diag_close_phase_begin(heap);
#endif
        result = uhm_slab_drain_remote_all(heap, 1);
        if (result == UHM_OK) {
            result = uhm_slab_remote_raw_ready_clear_all(heap);
        }
#if UHMAS_ENABLE_DIAGNOSTICS
        uhm_diag_close_phase_end(heap, UHM_CLOSE_PHASE_FINAL_CLASS_PUBLISH,
                                 &phase_sample);
#endif
    }
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
        atomic_store_explicit(&heap->lifecycle_state, UHM_MAGIC,
                              UHM_ATOMIC_RELEASE);
        uhm_unlock(&heap->lock);
        return result;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    phase_sample = uhm_diag_close_phase_begin(heap);
#endif
    uhm_tls_cache_detach_all_locked(heap);
    uhm_thread_ctx_detach_all_locked(heap);
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_close_phase_end(heap, UHM_CLOSE_PHASE_REGISTRY_DETACH,
                             &phase_sample);
    phase_sample = uhm_diag_close_phase_begin(heap);
#endif
    uhm_tls_key_delete(heap);
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_close_phase_end(heap, UHM_CLOSE_PHASE_TLS_KEY_DELETE,
                             &phase_sample);
#endif
    if (heap->core.owns_arena) {
#if UHMAS_ENABLE_DIAGNOSTICS
        phase_sample = uhm_diag_close_phase_begin(heap);
#endif
        (void)uhm_vm_release(heap, (void *)heap->core.arena_base, heap->core.arena_size);
#if UHMAS_ENABLE_DIAGNOSTICS
        uhm_diag_close_phase_end(heap, UHM_CLOSE_PHASE_ARENA_RELEASE,
                                 &phase_sample);
#endif
    }
    atomic_store_explicit(&heap->lifecycle_state, UHM_CLOSED_MAGIC,
                          UHM_ATOMIC_RELEASE);
    heap->magic = UHM_CLOSED_MAGIC;
    uhm_unlock(&heap->lock);
    return result;
}

static void uhm_fill_api(uhm_api *api, uint64_t capabilities) {
    memset(api, 0, sizeof(*api));
    api->size = sizeof(*api);
    api->abi_major = UHM_ABI_MAJOR;
    api->abi_minor = UHM_ABI_MINOR;
    api->capabilities = capabilities;
    api->malloc = uhm_api_malloc;
    api->calloc = uhm_api_calloc;
    api->realloc = uhm_api_realloc;
    api->aligned_alloc = uhm_api_aligned_alloc;
    api->free = uhm_api_free;
    api->malloc_usable_size = uhm_api_malloc_usable_size;
    api->mmap = uhm_api_mmap;
    api->munmap = uhm_api_munmap;
    api->mprotect = uhm_api_mprotect;
    api->mmap_ex = uhm_api_mmap_ex;
    api->thread_ctx_open = uhm_api_thread_ctx_open;
    api->thread_ctx_close = uhm_api_thread_ctx_close;
    api->malloc_ctx = uhm_api_malloc_ctx;
    api->free_ctx = uhm_api_free_ctx;
    api->get_stats = uhm_api_get_stats;
    api->check = uhm_api_check;
    api->close = uhm_api_close;
    api->enumerate_leaks = uhm_api_enumerate_leaks;
    api->get_last_error = uhm_api_get_last_error;
    api->thread_ctx_flush = uhm_api_thread_ctx_flush;
    api->free_deferred_ctx = uhm_api_free_deferred_ctx;
    api->mremap = uhm_api_mremap;
}

static uhm_result uhm_validate_vm(const uhm_vm_ops *vm) {
    const uint64_t numa_caps = UHM_VM_CAN_NUMA_FIRST_TOUCH |
                               UHM_VM_CAN_NUMA_PREFERRED |
                               UHM_VM_CAN_NUMA_BIND;
    if (!vm || vm->size < sizeof(*vm) || vm->abi_major != UHM_ABI_MAJOR ||
        vm->page_size == 0 || !uhm_is_power_of_two(vm->page_size) ||
        vm->allocation_granularity == 0 ||
        !uhm_is_power_of_two(vm->allocation_granularity) ||
        vm->allocation_granularity < vm->page_size ||
        (vm->allocation_granularity % vm->page_size) != 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((vm->capabilities & UHM_VM_CAN_COMMIT) != 0 && !vm->commit) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((vm->capabilities & UHM_VM_CAN_DECOMMIT) != 0 && !vm->decommit) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((vm->capabilities & UHM_VM_CAN_PROTECT) != 0 && !vm->protect) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((vm->capabilities & UHM_VM_CAN_RELEASE) != 0 && !vm->release) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((vm->capabilities & UHM_VM_CAN_FIXED_REMAP) != 0 && !vm->fixed_remap) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((vm->capabilities & numa_caps) != 0 && !vm->numa_hint) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    return UHM_OK;
}

static uhm_result uhm_normalize_vm_ops(const uhm_vm_ops *in,
                                       uhm_vm_ops *out) {
    if (!in || !out || in->size < sizeof(*in) ||
        in->abi_major != UHM_ABI_MAJOR) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    *out = *in;
    if (out->page_size == 0) {
        out->page_size = uhm_system_page_size();
        if (out->page_size == 0) {
            return UHM_ERR_PLATFORM;
        }
    }
    if (out->allocation_granularity == 0) {
        out->allocation_granularity = uhm_system_allocation_granularity();
        if (out->allocation_granularity == 0) {
            return UHM_ERR_PLATFORM;
        }
        if (out->allocation_granularity < out->page_size) {
            out->allocation_granularity = out->page_size;
        }
    }
    return uhm_validate_vm(out);
}

static uhm_result uhm_normalize_tls_cache_config(const uhm_config *config,
                                                 size_t *min_pages,
                                                 size_t *default_pages,
                                                 size_t *max_pages) {
    size_t min_value = config->tls_cache_min_pages;
    size_t default_value = config->tls_cache_default_pages;
    size_t max_value = config->tls_cache_max_pages;

    if (max_value == 0) {
        if (min_value != 0 || default_value != 0) {
            return UHM_ERR_INVALID_ARGUMENT;
        }
    } else if (min_value > default_value || default_value > max_value) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    *min_pages = min_value;
    *default_pages = default_value;
    *max_pages = max_value;
    return UHM_OK;
}

uhm_result UHM_CALL uhm_abi_query(uhm_abi_info *out) {
    uint32_t requested_size;

    if (!out) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    requested_size = out->size;
    if (requested_size < UHM_ABI_INFO_SIZE_ABI9) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    out->size = sizeof(*out);
    out->abi_major = UHM_ABI_MAJOR;
    out->abi_minor = UHM_ABI_MINOR;
    out->flags = 0u;
    out->pointer_size = (uint32_t)sizeof(void *);
    out->size_t_size = (uint32_t)sizeof(size_t);
    out->atomic_pointer_lock_free = ATOMIC_POINTER_LOCK_FREE;
    out->reserved = 0u;
    if (requested_size >= offsetof(uhm_abi_info, atomic_u64_lock_free) +
                              sizeof(out->atomic_u64_lock_free)) {
        out->atomic_u64_lock_free = (uint32_t)uhm_atomic_u64_is_lock_free();
    }
    if (requested_size >= offsetof(uhm_abi_info, tagged_freelist_supported) +
                              sizeof(out->tagged_freelist_supported)) {
        out->tagged_freelist_supported =
            (uint32_t)uhm_tagged_freelist_is_supported();
    }
    if (requested_size >= offsetof(uhm_abi_info, tagged_freelist_tag_bits) +
                              sizeof(out->tagged_freelist_tag_bits)) {
        out->tagged_freelist_tag_bits =
            (uint32_t)UHM_TAGGED_FREELIST_TAG_BITS;
    }
    if (requested_size >=
        offsetof(uhm_abi_info, lockless_remote_primitives_supported) +
            sizeof(out->lockless_remote_primitives_supported)) {
        out->lockless_remote_primitives_supported =
            (uint32_t)uhm_lockless_remote_primitives_supported();
    }
    if (requested_size >= offsetof(uhm_abi_info,
                                   lockless_remote_config_supported) +
                              sizeof(out->lockless_remote_config_supported)) {
        out->lockless_remote_config_supported =
            (uint32_t)uhm_lockless_remote_primitives_supported();
    }
    if (requested_size >= offsetof(uhm_abi_info, reserved2) +
                              sizeof(out->reserved2)) {
        out->reserved2 = 0u;
    }
    if (requested_size >= offsetof(uhm_abi_info, atomic_u32_lock_free) +
                              sizeof(out->atomic_u32_lock_free)) {
        out->atomic_u32_lock_free =
            (uint32_t)uhm_atomic_u32_is_lock_free();
    }
    if (requested_size >= offsetof(uhm_abi_info, malloc_min_alignment) +
                              sizeof(out->malloc_min_alignment)) {
        out->malloc_min_alignment = (uint32_t)UHM_MIN_ALIGNMENT;
    }
    if (requested_size >= offsetof(uhm_abi_info, cacheline_size) +
                              sizeof(out->cacheline_size)) {
        out->cacheline_size = (uint32_t)UHM_CACHELINE_SIZE;
    }
    if (requested_size >= offsetof(uhm_abi_info, call_abi) +
                              sizeof(out->call_abi)) {
#if defined(_WIN32)
        out->call_abi = UHM_CALL_ABI_CDECL;
#else
        out->call_abi = UHM_CALL_ABI_C;
#endif
    }
    return UHM_OK;
}

uhm_result UHM_CALL uhm_heap_storage_query(uhm_storage_info *out_heap,
                                           uhm_storage_info *out_thread_ctx) {
    if (out_heap) {
        if (out_heap->size < sizeof(*out_heap)) {
            return UHM_ERR_INVALID_ARGUMENT;
        }
        out_heap->size = sizeof(*out_heap);
        out_heap->abi_major = UHM_ABI_MAJOR;
        out_heap->abi_minor = UHM_ABI_MINOR;
        out_heap->flags = 0u;
        out_heap->storage_size = sizeof(uhm_heap);
        out_heap->storage_alignment = alignof(uhm_heap);
    }
    if (out_thread_ctx) {
        if (out_thread_ctx->size < sizeof(*out_thread_ctx)) {
            return UHM_ERR_INVALID_ARGUMENT;
        }
        out_thread_ctx->size = sizeof(*out_thread_ctx);
        out_thread_ctx->abi_major = UHM_ABI_MAJOR;
        out_thread_ctx->abi_minor = UHM_ABI_MINOR;
        out_thread_ctx->flags = 0u;
        out_thread_ctx->storage_size = sizeof(uhm_thread_ctx);
        out_thread_ctx->storage_alignment = alignof(uhm_thread_ctx);
    }
    return UHM_OK;
}

uhm_result UHM_CALL uhm_open(const uhm_config *config, uhm_heap **out_heap) {
    uhm_result result;
    uhm_heap *heap;
    uhm_vm_ops vm;
    uintptr_t arena_base;
    uintptr_t arena_end;
    uintptr_t control_end;
    size_t page_size;
    size_t arena_size;
    size_t control_size;
    size_t arena_page_bytes;
    size_t tls_cache_min_pages;
    size_t tls_cache_default_pages;
    size_t tls_cache_max_pages;
    size_t tls_cache_max_retained_pages;
    size_t tls_cache_slot_count;
    size_t malloc_free_decommit_retention_pages;
    uint32_t spinlock_mode;
    uint32_t spinlock_yield_after_spins;
    size_t i;
    int owns_arena = 0;

    if (!config || !out_heap || config->size < UHM_CONFIG_SIZE_ABI7 ||
        config->abi_major != UHM_ABI_MAJOR || !config->heap_storage ||
        config->heap_storage_size < sizeof(uhm_heap) ||
        ((uintptr_t)config->heap_storage % alignof(uhm_heap)) != 0 ||
        (config->flags & ~(UHM_CONFIG_LOCKLESS_REMOTE_FREE |
                           UHM_CONFIG_SPINLOCK_BURN |
                           UHM_CONFIG_SPINLOCK_YIELD |
                           UHM_CONFIG_FAIL_ON_TREAP_HEALTH |
                           UHM_CONFIG_REPAIR_TREAP_HEALTH |
                           UHM_CONFIG_FAIL_ON_LIVE_BLOCK_HEALTH)) != 0 ||
        ((config->flags & UHM_CONFIG_SPINLOCK_BURN) != 0 &&
         (config->flags & UHM_CONFIG_SPINLOCK_YIELD) != 0) ||
        ((config->flags & UHM_CONFIG_FAIL_ON_TREAP_HEALTH) != 0 &&
         (config->flags & UHM_CONFIG_REPAIR_TREAP_HEALTH) != 0)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    spinlock_mode = (config->flags & UHM_CONFIG_SPINLOCK_BURN) != 0
                        ? UHM_SPINLOCK_MODE_BURN
                        : UHM_SPINLOCK_MODE_YIELD;
    spinlock_yield_after_spins =
        config->size >= UHM_CONFIG_SIZE_ABI17 &&
                config->spinlock_yield_after_spins != 0u
            ? config->spinlock_yield_after_spins
            : UHM_SPINLOCK_DEFAULT_YIELD_AFTER;
    result = uhm_normalize_vm_ops(&config->vm, &vm);
    if (result != UHM_OK) {
        return result;
    }
    result = uhm_normalize_tls_cache_config(config, &tls_cache_min_pages,
                                            &tls_cache_default_pages,
                                            &tls_cache_max_pages);
    if (result != UHM_OK) {
        return result;
    }
    tls_cache_max_retained_pages =
        config->size >= UHM_CONFIG_SIZE_ABI18
            ? config->tls_cache_max_retained_pages
            : 0u;
    if (tls_cache_max_pages == 0u) {
        if (tls_cache_max_retained_pages != 0u) {
            return UHM_ERR_INVALID_ARGUMENT;
        }
    } else {
        if (tls_cache_max_retained_pages == 0u) {
            tls_cache_max_retained_pages =
                uhm_local_cache_retained_page_limit(
                    tls_cache_max_pages);
        }
        if (tls_cache_max_retained_pages < tls_cache_max_pages) {
            return UHM_ERR_INVALID_ARGUMENT;
        }
    }
    tls_cache_slot_count = config->thread_registry_slots;
    if (tls_cache_slot_count == 0) {
        tls_cache_slot_count = UHM_TLS_CACHE_MAX_SLOTS;
    }
    if (tls_cache_slot_count > UHM_TLS_CACHE_MAX_SLOTS) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    result = uhm_numa_validate_vm_policy(&vm, config->numa_policy);
    if (result != UHM_OK) {
        return result;
    }
    result = uhm_numa_validate_node(config->numa_policy,
                                    config->preferred_numa_node);
    if (result != UHM_OK) {
        return result;
    }
    if (config->arena_size == 0 ||
        uhm_align_up_size(config->arena_size, vm.page_size, &arena_size)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (config->arena_base != 0 && arena_size != config->arena_size) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    malloc_free_decommit_retention_pages =
        config->size >= UHM_CONFIG_SIZE_ABI8 &&
                config->malloc_free_decommit_retention_pages != 0
            ? config->malloc_free_decommit_retention_pages
            : (size_t)UHM_MALLOC_FREE_DECOMMIT_DEFAULT_RETENTION_PAGES;
    if (malloc_free_decommit_retention_pages >
        SIZE_MAX / vm.page_size) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    heap = (uhm_heap *)config->heap_storage;
    memset(heap, 0, sizeof(*heap));
    heap->magic = UHM_MAGIC;
    heap->abi_major = UHM_ABI_MAJOR;
    heap->abi_minor = UHM_ABI_MINOR;
    heap->state_flags = config->flags & (UHM_CONFIG_LOCKLESS_REMOTE_FREE |
                                         UHM_CONFIG_FAIL_ON_TREAP_HEALTH |
                                         UHM_CONFIG_REPAIR_TREAP_HEALTH |
                                         UHM_CONFIG_FAIL_ON_LIVE_BLOCK_HEALTH);
    heap->core.low_lock_remote_free_enabled =
        ((config->flags & UHM_CONFIG_LOCKLESS_REMOTE_FREE) != 0u &&
         UINTPTR_MAX == UINT32_MAX && uhm_atomic_u32_is_lock_free())
            ? 1u
            : 0u;
    if ((heap->state_flags & UHM_CONFIG_LOCKLESS_REMOTE_FREE) != 0 &&
        !uhm_lockless_remote_primitives_supported()) {
        heap->state_flags &= ~UHM_CONFIG_LOCKLESS_REMOTE_FREE;
    }
    atomic_init(&heap->lifecycle_state, 0u);
    atomic_init(&heap->active_ops, 0u);
    for (i = 0; i < UHM_ACTIVE_OP_SHARDS; ++i) {
        atomic_init(&heap->active_op_shards[i].value, 0u);
    }
    atomic_init(&heap->metadata_hazard_readers, 0u);
    atomic_init(&heap->metadata_registered_readers, 0u);
    atomic_init(&heap->metadata_registered_reader_seq, UINT64_C(0));
    uhm_heap_init_diagnostics_timing_enabled(heap);
    for (i = 0; i < UHM_MALLOC_LIVE_STAT_SHARDS; ++i) {
        atomic_init(&heap->stats.malloc_live_shards[i].value, 0u);
    }
    atomic_init(&heap->core.generation, 0u);
    atomic_init(&heap->core.metadata_published_epoch, 0u);
    atomic_init(&heap->core.metadata_reclaim_pending, 0u);
    atomic_init(&heap->core.treap_repair_pending_flags, 0u);
    heap->core.spinlock_mode = spinlock_mode;
    heap->core.spinlock_yield_after_spins = spinlock_yield_after_spins;
    uhm_spinlock_init_ex(&heap->lock, heap->core.spinlock_mode,
                         heap->core.spinlock_yield_after_spins);
    for (i = 0; i < UHM_PAGE_MAP_SHARDS; ++i) {
        uhm_spinlock_init_ex(&heap->core.page_map_locks[i].lock,
                             heap->core.spinlock_mode,
                             heap->core.spinlock_yield_after_spins);
    }
    for (i = 0; i < UHM_VMA_SHARDS; ++i) {
        uhm_spinlock_init_ex(&heap->core.vma_locks[i].lock,
                             heap->core.spinlock_mode,
                             heap->core.spinlock_yield_after_spins);
    }
    for (i = 0; i < UHM_EXTENT_SHARDS; ++i) {
        uhm_spinlock_init_ex(&heap->core.extent_locks[i].lock,
                             heap->core.spinlock_mode,
                             heap->core.spinlock_yield_after_spins);
    }
    for (i = 0; i < UHM_SLAB_SHARDS; ++i) {
        uhm_spinlock_init_ex(&heap->core.slab_locks[i].lock,
                             heap->core.spinlock_mode,
                             heap->core.spinlock_yield_after_spins);
    }
    for (i = 0; i < UHM_SLAB_REMOTE_PENDING_MASK_WORD_COUNT; ++i) {
        atomic_init(&heap->core.slab_remote_pending_masks[i].value, 0u);
    }
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        uhm_tagged_freelist_init(&heap->core.slab_remote_class_lf[i].queue);
        uhm_tagged_freelist_init(&heap->core.slab_remote_ready_lf[i].queue);
        atomic_init(&heap->core.slab_remote_ready_seq[i].value, UINT64_C(0));
#if UINTPTR_MAX == UINT32_MAX
        atomic_init(&heap->core.slab_remote_ready_raw[i].value, 0);
#endif
        heap->core.slab_empty_committed_cache_high_water[i] =
            UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    }
    for (i = 0; i < UHM_LOCKLESS_REMOTE_READER_SLOTS; ++i) {
        uhm_metadata_reader_state_init(&heap->core.lockless_remote_readers[i]);
    }
    atomic_init(&heap->core.debug_last_corruption_site, 0u);
    atomic_init(&heap->last_error, (int)UHM_OK);
    atomic_init(&heap->platform_error, 0u);

    heap->F.size = sizeof(heap->F);
    heap->F.abi_major = UHM_ABI_MAJOR;
    heap->F.abi_minor = UHM_ABI_MINOR;
    heap->F.capabilities = vm.capabilities;
    uhm_fill_api(&heap->F.api, vm.capabilities);
    heap->F.vm.user = vm.user;
    heap->F.vm.page_size = vm.page_size;
    heap->F.vm.allocation_granularity = vm.allocation_granularity;
    heap->F.vm.capabilities = vm.capabilities;
    heap->F.vm.reserve = vm.reserve;
    heap->F.vm.commit = vm.commit;
    heap->F.vm.decommit = vm.decommit;
    heap->F.vm.release = vm.release;
    heap->F.vm.protect = vm.protect;
    heap->F.vm.fixed_remap = vm.fixed_remap;
    heap->F.vm.numa_hint = vm.numa_hint;
    heap->F.canary = UHM_HEAP_F_CANARY;
    uhm_tls_cache_registry_init(heap, tls_cache_slot_count);
    if (tls_cache_max_pages != 0) {
        result = uhm_tls_key_create(heap);
        if (result != UHM_OK) {
            atomic_store_explicit(&heap->lifecycle_state, 0u,
                                  UHM_ATOMIC_RELEASE);
            heap->F.canary = 0;
            heap->magic = 0;
            return result;
        }
    }

    arena_base = (uintptr_t)config->arena_base;
    if (arena_base == 0) {
        void *reserved = 0;
        if ((heap->F.vm.capabilities & UHM_VM_CAN_RESERVE) == 0 || !heap->F.vm.reserve) {
            atomic_store_explicit(&heap->lifecycle_state, 0u,
                                  UHM_ATOMIC_RELEASE);
            heap->magic = 0;
            return UHM_ERR_INVALID_ARGUMENT;
        }
        result = uhm_vm_reserve(heap, &reserved, arena_size,
                                heap->F.vm.allocation_granularity);
        if (result != UHM_OK) {
            atomic_store_explicit(&heap->lifecycle_state, 0u,
                                  UHM_ATOMIC_RELEASE);
            heap->magic = 0;
            return result;
        }
        arena_base = (uintptr_t)reserved;
        owns_arena = 1;
        heap->core.owns_arena = 1u;
    }

    if (uhm_range_overflows(arena_base, arena_size, &arena_end) ||
        (arena_base % vm.allocation_granularity) != 0) {
        uhm_open_cleanup(heap, arena_base, arena_size, 0, owns_arena, 0);
        return UHM_ERR_INVALID_ARGUMENT;
    }

    page_size = vm.page_size;
    control_size = config->control_band_min_size;
    if (control_size == 0) {
        control_size = uhm_max_size(UHM_MIN_CONTROL_BAND, 16u * page_size);
    }
    if (uhm_align_up_size(control_size, page_size, &control_size) ||
        control_size >= arena_size) {
        uhm_open_cleanup(heap, arena_base, arena_size, 0, owns_arena, 0);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    arena_page_bytes = arena_size;

    if (uhm_range_overflows(arena_base, control_size, &control_end)) {
        uhm_open_cleanup(heap, arena_base, arena_size, 0, owns_arena, 0);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    result = uhm_vm_commit(heap, (void *)arena_base, control_size, UHM_PROT_READ | UHM_PROT_WRITE);
    if (result != UHM_OK) {
        uhm_vm_rollback_failed_commit(heap, (void *)arena_base, control_size);
        uhm_open_cleanup(heap, arena_base, arena_size, 0, owns_arena, 0);
        return result;
    }

    heap->core.arena_base = arena_base;
    heap->core.arena_end = arena_end;
    heap->core.arena_size = arena_size;
    heap->core.control_base = arena_base;
    heap->core.control_end = control_end;
    heap->core.control_alloc = arena_base;
    heap->core.control_bytes = control_size;
    heap->core.malloc_hi = control_end;
    heap->core.mmap_lo = arena_end;
    heap->core.page_count = arena_page_bytes / page_size;
    heap->core.treap_priority_salt = uhm_tree_priority_mix(
        ((uint64_t)(uintptr_t)heap << 17) ^
        ((uint64_t)arena_base >> 4) ^
        ((uint64_t)arena_size << 7) ^
        ((uint64_t)page_size << 33) ^
        ((uint64_t)vm.allocation_granularity << 11));
    if (heap->core.treap_priority_salt == 0) {
        heap->core.treap_priority_salt =
            UINT64_C(0x6a09e667f3bcc909);
    }
    heap->core.tls_cache_min_pages = tls_cache_min_pages;
    heap->core.tls_cache_default_pages = tls_cache_default_pages;
    heap->core.tls_cache_max_pages = tls_cache_max_pages;
    heap->core.tls_cache_max_retained_pages =
        tls_cache_max_retained_pages;
    heap->core.numa_policy = config->numa_policy;
    heap->core.preferred_numa_node = config->preferred_numa_node;
    heap->core.malloc_free_decommit_retention_pages =
        malloc_free_decommit_retention_pages;
    heap->core.malloc_free_decommit_retention_base_high_water_bytes =
        uhm_block_retention_base_high_water_bytes(heap);
    heap->core.malloc_free_decommit_retention_high_water_bytes =
        heap->core.malloc_free_decommit_retention_base_high_water_bytes;
    heap->core.malloc_free_decommit_retention_max_high_water_bytes =
        uhm_block_retention_max_high_water_bytes(heap);
    heap->stats.committed_bytes = control_size;
    uhm_extent_descriptor_set(&heap->core.primary_control_desc,
                              UHM_EXTENT_KIND_CONTROL,
                              arena_base, control_size,
                              UHM_PAGE_OWNER_CONTROL,
                              UHM_PAGE_VA_CONTROL_LIVE,
                              UHM_PAGE_COMMIT_LIVE,
                              UHM_PROT_READ | UHM_PROT_WRITE,
                              uhm_heap_generation_next(heap), heap);

    result = uhm_page_mark_range(heap, arena_base, control_size, UHM_PAGE_OWNER_CONTROL,
                                 UHM_PAGE_VA_CONTROL_LIVE, UHM_PAGE_COMMIT_LIVE,
                                 UHM_PROT_READ | UHM_PROT_WRITE,
                                 heap->core.primary_control_desc.id,
                                 heap->core.primary_control_desc.generation, 1);
    if (result != UHM_OK) {
        uhm_open_cleanup(heap, arena_base, arena_size, control_size, owns_arena, 1);
        return result;
    }

    result = uhm_emergency_reserve_init(heap);
    if (result != UHM_OK) {
        uhm_open_cleanup(heap, arena_base, arena_size, control_size, owns_arena, 1);
        return result;
    }

    atomic_store_explicit(&heap->lifecycle_state, UHM_MAGIC,
                          UHM_ATOMIC_RELEASE);
    *out_heap = heap;
    return UHM_OK;
}

const uhm_api *UHM_CALL uhm_get_api(uhm_heap *heap) {
    if (!uhm_heap_is_live(heap)) {
        return 0;
    }
    return &heap->F.api;
}
