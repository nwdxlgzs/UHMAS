static uhm_result uhm_prepare_alloc_size(size_t alignment, size_t size,
                                         size_t *out_total, size_t *out_payload,
                                         uint16_t *out_size_class) {
    size_t header;
    size_t aligned_payload;
    size_t total;
    uint16_t size_class;

    if (!uhm_is_power_of_two(alignment) || alignment < UHM_MIN_ALIGNMENT) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    size_class = uhm_size_class_for(alignment, size, &aligned_payload);
    if (size_class == UHM_SIZE_CLASS_NONE) {
        if (uhm_align_up_size(size == 0 ? 1u : size, UHM_MIN_ALIGNMENT, &aligned_payload)) {
            return UHM_ERR_OUT_OF_MEMORY;
        }
    }
    header = uhm_malloc_header_size();
    if (uhm_add_overflows_size(header, aligned_payload, &total)) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    if (alignment > UHM_MIN_ALIGNMENT) {
        size_t extra = alignment - UHM_MIN_ALIGNMENT;
        if (uhm_add_overflows_size(total, extra, &total)) {
            return UHM_ERR_OUT_OF_MEMORY;
        }
    }
    if (uhm_align_up_size(total, UHM_MIN_ALIGNMENT, out_total)) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    if (out_payload) {
        *out_payload = aligned_payload;
    }
    if (out_size_class) {
        *out_size_class = size_class;
    }
    return UHM_OK;
}

static void *uhm_aligned_alloc_pinned(uhm_heap *heap, size_t alignment,
                                      size_t size);
static void uhm_free_pinned(uhm_heap *heap, void *ptr);

static void *UHM_CALL uhm_api_malloc(uhm_heap *heap, size_t size) {
    void *ptr;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return 0;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return 0;
    }
    ptr = uhm_tls_malloc_pinned(heap, size);
    if (ptr) {
        uhm_heap_op_end(heap, op_shard);
        return ptr;
    }
    ptr = uhm_aligned_alloc_pinned(heap, UHM_MIN_ALIGNMENT, size);
    uhm_heap_op_end(heap, op_shard);
    return ptr;
}

static void *uhm_aligned_alloc_pinned(uhm_heap *heap, size_t alignment,
                                      size_t size) {
    size_t total;
    size_t payload_size;
    size_t header;
    size_t block_needed;
    uint16_t size_class;
    uhm_result result;
    uhm_block *block;
    uintptr_t start;
    uintptr_t payload;
    uintptr_t block_start;
    uintptr_t end;
    uintptr_t commit_start;
    uintptr_t commit_end;
    size_t commit_size;
    size_t claim_size;
    uint64_t generation;
    uhm_range_claim claim;
    size_t publish_first;
    size_t publish_count;
    uhm_vm_call_record commit_record;
    uhm_vm_call_record numa_record;
    int32_t numa_node;

    if (!uhm_heap_is_accessible_pinned(heap)) {
        return 0;
    }
    result = uhm_prepare_alloc_size(alignment, size, &total, &payload_size, &size_class);
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
        return 0;
    }
    if (alignment == UHM_MIN_ALIGNMENT &&
        uhm_size_class_uses_slab(size_class, payload_size)) {
        uhm_result remote_result = UHM_OK;
        void *remote_ptr =
            uhm_slab_try_alloc_remote_lockless(heap, size_class, size,
                                               &remote_result);
        if (remote_ptr) {
            return remote_ptr;
        }
        if (remote_result != UHM_OK) {
            uhm_set_error(heap, remote_result);
            return 0;
        }
        remote_ptr = uhm_slab_try_alloc_accounted_remote_lockless(
            heap, size_class, size, &remote_result);
        if (remote_ptr) {
            return remote_ptr;
        }
        if (remote_result != UHM_OK) {
            uhm_set_error(heap, remote_result);
            return 0;
        }
#if UINTPTR_MAX == UINT32_MAX
        remote_ptr = uhm_slab_try_alloc_raw_remote_32bit(
            heap, size_class, size, &remote_result);
        if (remote_ptr) {
            return remote_ptr;
        }
        if (remote_result != UHM_OK) {
            uhm_set_error(heap, remote_result);
            return 0;
        }
#endif
    }

    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return 0;
    }
retry_locked:
    uhm_metadata_epoch_advance(heap);

    if (uhm_size_class_uses_slab(size_class, payload_size)) {
        uhm_result slab_result = UHM_OK;
        int drain_class_queue = !uhm_lockless_remote_free_enabled(heap);
        void *slab_ptr = uhm_slab_alloc(heap, size_class, payload_size, size,
                                        drain_class_queue,
                                        &slab_result);
        if (!slab_ptr) {
            (void)uhm_bump_failure_generation(heap);
            uhm_unlock(&heap->lock);
            uhm_set_error(heap, slab_result != UHM_OK ? slab_result
                                                      : UHM_ERR_OUT_OF_MEMORY);
            return 0;
        }
        uhm_unlock(&heap->lock);
        return slab_ptr;
    }

    if (alignment == UHM_MIN_ALIGNMENT) {
        block = uhm_malloc_free_tree_find_best(
            heap, heap->core.malloc_free_tree_root, total);
        if (block) {
            int prefix_recommit_handled = 0;
            result = uhm_block_recommit_split_prefix(
                heap, block, total, &prefix_recommit_handled);
            if (result == UHM_OK && !prefix_recommit_handled) {
                result = uhm_block_recommit_free_extent(heap, block, 1);
            }
            if (result != UHM_OK) {
                (void)uhm_bump_failure_generation(heap);
                uhm_unlock(&heap->lock);
                uhm_set_error(heap, result);
                return 0;
            }
            uhm_free_list_remove(heap, block);
            block->flags &= ~(UHM_BLOCK_FLAG_PAGE_PRIVATE |
                              UHM_BLOCK_FLAG_DECOMMITTED);
            block->size_class = size_class;
            block->requested_size = size;
            block->generation = uhm_heap_generation_next(heap);
            uhm_block_descriptor_set(block);
            uhm_live_block_tree_insert(heap, block);
            uhm_stats_malloc_live_add(heap, (uintptr_t)uhm_block_payload(block),
                                      size);
            (void)uhm_split_block(heap, block, total, 1);
            if (prefix_recommit_handled) {
                /* The partial recommit leaves the split remainder unknown;
                   establish this hint only for the now-live prefix. */
                block->flags |= UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;
            }
            uhm_unlock(&heap->lock);
            return uhm_block_payload(block);
        }
    }

    header = uhm_malloc_header_size();
    if (uhm_align_up_uintptr_checked(heap->core.malloc_hi,
                                     UHM_MIN_ALIGNMENT, &start) ||
        uhm_range_overflows(start, header, &payload) ||
        uhm_align_up_uintptr_checked(payload, alignment, &payload)) {
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, UHM_ERR_OUT_OF_ADDRESS_SPACE);
        return 0;
    }
    block_start = payload - header;
    if (uhm_add_overflows_size(header, payload_size, &block_needed) ||
        uhm_range_overflows(block_start, block_needed, &end) ||
        end > heap->core.mmap_lo) {
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, UHM_ERR_OUT_OF_ADDRESS_SPACE);
        return 0;
    }
    commit_start = uhm_align_down_uintptr(start, heap->F.vm.page_size);
    if (uhm_align_up_size((size_t)(end - commit_start), heap->F.vm.page_size, &commit_size)) {
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, UHM_ERR_OUT_OF_MEMORY);
        return 0;
    }
    commit_end = commit_start + commit_size;
    if (commit_end < commit_start || commit_end > heap->core.mmap_lo) {
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, UHM_ERR_OUT_OF_ADDRESS_SPACE);
        return 0;
    }
    claim_size = (size_t)(end - start);
    result = uhm_claim_low_watermark(heap, claim_size, UHM_MIN_ALIGNMENT, &claim);
    if (result == UHM_ERR_BUSY) {
        uhm_unlock(&heap->lock);
        uhm_spinlock_yield();
        uhm_lock(&heap->lock);
        if (!uhm_heap_is_accessible_pinned(heap)) {
            uhm_unlock(&heap->lock);
            uhm_set_error(heap, UHM_ERR_BUSY);
            return 0;
        }
        goto retry_locked;
    }
    if (result != UHM_OK || claim.addr != start) {
        if (result == UHM_OK) {
            (void)uhm_claim_abort(heap, &claim, 0);
            result = UHM_ERR_CORRUPTION;
        }
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, result);
        return 0;
    }

    result = uhm_page_prepare_range(heap, commit_start, commit_size);
    if (result != UHM_OK) {
        (void)uhm_claim_abort(heap, &claim, 0);
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, result);
        return 0;
    }

    result = uhm_claim_begin_low_watermark_inflight(heap, &claim);
    if (result != UHM_OK) {
        (void)uhm_claim_abort(heap, &claim, 0);
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, result);
        return 0;
    }

    memset(&commit_record, 0, sizeof(commit_record));
    memset(&numa_record, 0, sizeof(numa_record));
    numa_node = uhm_numa_effective_node(heap, UHM_NUMA_POLICY_DEFAULT, -1);
    uhm_unlock(&heap->lock);
    commit_record = uhm_vm_commit_unlocked_call(
        heap, (void *)commit_start, commit_size,
        UHM_PROT_READ | UHM_PROT_WRITE);
    if (commit_record.result == UHM_OK) {
        numa_record = uhm_vm_numa_hint_unlocked_call(
            heap, (void *)commit_start, commit_size,
            UHM_NUMA_POLICY_DEFAULT, numa_node, 0u);
    }
    uhm_lock(&heap->lock);
    result = uhm_vm_commit_publish_record_locked(heap, &commit_record);
    uhm_vm_numa_hint_publish_best_effort_record_locked(heap, &numa_record);
    if (result != UHM_OK) {
        if (commit_record.did_call) {
            uhm_vm_decommit_rollback_unlocked_preserve_error(
                heap, (void *)commit_start, commit_size);
        }
        (void)uhm_claim_abort(heap, &claim, 0);
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, result);
        return 0;
    }

    generation = uhm_heap_generation_next(heap);
    block = (uhm_block *)block_start;
    memset(block, 0, sizeof(*block));
    block->magic = UHM_BLOCK_MAGIC;
    block->size_class = size_class;
    block->total_size = header + payload_size;
    block->requested_size = size;
    if (uhm_block_is_page_private_candidate(heap, payload, size)) {
        block->flags |= UHM_BLOCK_FLAG_PAGE_PRIVATE;
    }
    block->prev_phys = heap->core.last_block;
    uhm_block_tree_priority_init_live(heap, block);
    block->generation = generation;
    uhm_block_descriptor_set(block);

    if (!uhm_page_span(heap, start, end - start, &publish_first,
                       &publish_count)) {
        result = UHM_ERR_CORRUPTION;
    } else {
        uhm_lockset publish_lockset;
        uhm_page_lock_shard_range(heap, publish_first, publish_count,
                                  &publish_lockset);
        uhm_extent_lock_all_shards(heap, &publish_lockset);
        result = uhm_page_mark_range_locked_budgeted(
            heap, start, end - start, UHM_PAGE_OWNER_MALLOC,
            UHM_PAGE_VA_MALLOC_LIVE, UHM_PAGE_COMMIT_LIVE,
            UHM_PROT_READ | UHM_PROT_WRITE, block->desc.id, generation,
            &publish_lockset);
        if (result == UHM_OK) {
            if (heap->core.last_block) {
                heap->core.last_block->next_phys = block;
            } else {
                heap->core.first_block = block;
            }
            heap->core.last_block = block;
            uhm_live_block_tree_insert(heap, block);
            block->flags |= UHM_BLOCK_FLAG_COMMIT_ALL_KNOWN;
        }
        uhm_metadata_unlock_shards(heap, &publish_lockset);
        uhm_page_unlock_shards(heap, &publish_lockset);
    }
    if (result != UHM_OK) {
        uhm_vm_decommit_rollback_unlocked_preserve_error(
            heap, (void *)commit_start, commit_size);
        (void)uhm_claim_abort(heap, &claim, 0);
        uhm_extent_descriptor_retire(&block->desc);
        block->magic = 0;
        block->free_tree_priority = 0;
        block->live_tree_priority = 0;
        (void)uhm_bump_failure_generation(heap);
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, result);
        return 0;
    }

    uhm_claim_disarm(heap, &claim);
    heap->stats.committed_bytes += commit_size;
    uhm_stats_malloc_live_add(heap, payload, size);

    uhm_unlock(&heap->lock);
    return (void *)payload;
}

static void *UHM_CALL uhm_api_aligned_alloc(uhm_heap *heap, size_t alignment, size_t size) {
    void *ptr;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return 0;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return 0;
    }
    ptr = uhm_aligned_alloc_pinned(heap, alignment, size);
    uhm_heap_op_end(heap, op_shard);
    return ptr;
}

static void *UHM_CALL uhm_api_calloc(uhm_heap *heap, size_t count, size_t size) {
    size_t total;
    void *ptr;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return 0;
    }
    if (uhm_mul_overflows_size(count, size, &total)) {
        uhm_set_error(heap, UHM_ERR_OUT_OF_MEMORY);
        uhm_heap_op_end(heap, op_shard);
        return 0;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return 0;
    }
    ptr = uhm_tls_malloc_pinned(heap, total);
    if (ptr) {
        memset(ptr, 0, total);
        uhm_heap_op_end(heap, op_shard);
        return ptr;
    }
    ptr = uhm_aligned_alloc_pinned(heap, UHM_MIN_ALIGNMENT, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    uhm_heap_op_end(heap, op_shard);
    return ptr;
}

static uhm_result uhm_free_owned_locked(uhm_heap *heap, void *ptr) {
    uhm_block *block;
    uhm_slab_slot *slot;
    uhm_result result;

    if (!heap || !ptr || !uhm_heap_is_accessible_pinned(heap)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_metadata_epoch_advance(heap);
    slot = uhm_find_slab_slot_by_payload(heap, ptr, 1);
    if (slot) {
        return uhm_slab_remote_free_slot_locked(heap, slot);
    }
    block = uhm_find_live_block_by_payload_from(
        heap, ptr, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE);
    if (!block) {
        return UHM_ERR_CORRUPTION;
    }
    result = uhm_restore_page_private_block_rw(heap, block);
    if (result != UHM_OK) {
        return result;
    }
    uhm_live_block_tree_remove(heap, block);
    uhm_stats_malloc_live_sub(heap, (uintptr_t)uhm_block_payload(block),
                              block->requested_size);
    block->requested_size = 0;
    block->size_class = UHM_SIZE_CLASS_NONE;
    block->generation = uhm_heap_generation_next(heap);
    uhm_block_descriptor_set(block);
    uhm_free_list_insert(heap, block);
    result = uhm_coalesce_block(heap, &block);
    if (result == UHM_OK) {
        result = uhm_block_decommit_free_extent(heap, block, 1);
    }
    if (result == UHM_ERR_UNSUPPORTED || result == UHM_ERR_PLATFORM) {
        return UHM_OK;
    }
    return result;
}

static void uhm_free_pinned(uhm_heap *heap, void *ptr) {
    uhm_block *block;
    uhm_slab_slot *slot;
    uhm_slab *remote_slab = 0;
    uhm_result result;
    int remote_needs_decommit_check = 0;
    uint32_t remote_live_flags = 0;
    uint32_t remote_live_requested = 0;
    int force_accounted_remote_free = 0;
    int remote_needs_state_refresh = 0;

    if (!uhm_heap_is_accessible_pinned(heap) || !ptr) {
        return;
    }
    if (uhm_lockless_remote_free_enabled(heap)) {
        uhm_metadata_reader_state *reader =
            uhm_lockless_remote_reader_acquire(heap);
        if (reader) {
            slot = uhm_slab_slot_from_payload_lockless(heap, ptr, reader);
            if (slot) {
                result = uhm_slab_remote_free_slot_prepare_unaccounted(
                    heap, slot, &remote_slab, &remote_live_flags,
                    &remote_live_requested);
                if (result == UHM_OK) {
                    result = uhm_slab_remote_free_slot_publish_unaccounted(
                        heap, remote_slab, slot);
                    if (result == UHM_OK) {
                        uhm_lockless_remote_reader_release(reader);
                        (void)uhm_metadata_epoch_advance_pinned_try(heap);
                        return;
                    }
                    {
                        uhm_result publish_result = result;
                        result =
                            uhm_slab_remote_free_slot_rollback_unaccounted(
                                heap, remote_slab, slot, remote_live_flags,
                                remote_live_requested);
                        uhm_lockless_remote_reader_release(reader);
                        if (result != UHM_OK) {
                            uhm_set_error(heap, result);
                            return;
                        }
                        if (publish_result != UHM_ERR_OUT_OF_MEMORY &&
                            publish_result != UHM_ERR_BUSY) {
                            uhm_set_error(heap, publish_result);
                            return;
                        }
                    }
                    /* Rollback restored the live slot; fall back to locked free. */
                    force_accounted_remote_free = 1;
                    goto locked_free_fallback;
                }
                uhm_lockless_remote_reader_release(reader);
                if (result != UHM_ERR_UNSUPPORTED) {
                    uhm_set_error(heap, result);
                    return;
                }
            } else {
                uhm_lockless_remote_reader_release(reader);
            }
        }
    }
    if (!force_accounted_remote_free &&
        uhm_low_lock_remote_free_enabled(heap)) {
        uhm_metadata_reader_state *reader =
            uhm_lockless_remote_reader_acquire(heap);
        if (reader) {
            slot = uhm_slab_slot_from_payload_lockless_ex(heap, ptr, reader, 0);
            if (slot) {
                result = uhm_slab_remote_free_slot_prepare_partial_low_lock(
                    heap, slot, &remote_slab);
                uhm_lockless_remote_reader_release(reader);
                if (result == UHM_OK) {
                    (void)uhm_metadata_epoch_advance_pinned_try(heap);
                    return;
                }
                if (result != UHM_ERR_UNSUPPORTED) {
                    uhm_set_error(heap, result);
                    return;
                }
            } else {
                uhm_lockless_remote_reader_release(reader);
            }
        }
    }
    if (!force_accounted_remote_free && uhm_tagged_freelist_is_supported()) {
        uhm_metadata_reader_state *reader =
            uhm_lockless_remote_reader_acquire(heap);
        if (reader) {
            slot = uhm_slab_slot_from_payload_lockless(heap, ptr, reader);
            if (slot) {
                result =
                    uhm_slab_remote_free_slot_prepare_partial_lockless(
                        heap, slot, &remote_slab,
                        &remote_needs_state_refresh,
                        &remote_needs_decommit_check);
                if (result == UHM_OK) {
                    uhm_lockless_remote_reader_release(reader);
                    result = uhm_slab_remote_free_slot_publish_lockless(
                        heap, remote_slab, slot, 0);
                    if (result == UHM_OK && !remote_needs_state_refresh) {
                        (void)uhm_metadata_epoch_advance_pinned_try(heap);
                    } else if (result == UHM_OK || remote_needs_state_refresh) {
                        uhm_lock(&heap->lock);
                        if (uhm_heap_is_accessible_pinned(heap)) {
                            if (remote_needs_state_refresh &&
                                remote_slab &&
                                remote_slab->magic == UHM_SLAB_MAGIC &&
                                (remote_slab->flags &
                                 UHM_SLAB_FLAG_DECOMMITTED) == 0) {
                                uhm_slab_raw_lock(remote_slab);
                                uhm_slab_refresh_state_raw_locked(
                                    heap, remote_slab);
                                uhm_slab_raw_unlock(remote_slab);
                                if (remote_needs_decommit_check &&
                                    (remote_slab->state !=
                                         UHM_SLAB_STATE_EMPTY ||
                                     uhm_slab_empty_committed_cache_within_high_water(
                                         heap, remote_slab->size_class))) {
                                    remote_needs_decommit_check = 0;
                                }
                            }
                            if (result == UHM_OK) {
                                uhm_metadata_epoch_advance(heap);
                            }
                        }
                        if (remote_needs_state_refresh && remote_slab) {
                            uhm_slab_state_transition_update_end(remote_slab);
                            remote_needs_state_refresh = 0;
                        }
                        uhm_unlock(&heap->lock);
                    }
                    if (result != UHM_OK) {
                        uhm_set_error(heap, result);
                    } else if (remote_needs_decommit_check &&
                               remote_slab &&
                               remote_slab->magic == UHM_SLAB_MAGIC) {
                        result = uhm_slab_drain_remote_reserved_lockless(
                            heap, remote_slab, 1);
                        if (result != UHM_OK) {
                            uhm_set_error(heap, result);
                        }
                    }
                    return;
                }
                uhm_lockless_remote_reader_release(reader);
                if (result != UHM_ERR_UNSUPPORTED) {
                    uhm_set_error(heap, result);
                    return;
                }
            } else {
                uhm_lockless_remote_reader_release(reader);
            }
        }
    }
locked_free_fallback:
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return;
    }
    uhm_metadata_epoch_advance(heap);
    slot = uhm_find_slab_slot_by_payload(heap, ptr, 1);
    if (slot) {
        if (uhm_lockless_remote_free_enabled(heap) &&
            !force_accounted_remote_free) {
            result = uhm_slab_remote_free_slot_prepare_unaccounted(
                heap, slot, &remote_slab, &remote_live_flags,
                &remote_live_requested);
            if (result == UHM_OK) {
                uhm_result publish_result;
                uhm_unlock(&heap->lock);
                publish_result =
                    uhm_slab_remote_free_slot_publish_unaccounted(
                        heap, remote_slab, slot);
                if (publish_result == UHM_OK) {
                    return;
                }
                result = uhm_slab_remote_free_slot_rollback_unaccounted(
                    heap, remote_slab, slot, remote_live_flags,
                    remote_live_requested);
                if (result != UHM_OK) {
                    uhm_set_error(heap, result);
                    return;
                }
                if (publish_result == UHM_ERR_OUT_OF_MEMORY ||
                    publish_result == UHM_ERR_BUSY) {
                    force_accounted_remote_free = 1;
                    goto locked_free_fallback;
                }
                uhm_set_error(heap, publish_result);
                return;
            }
            if (result != UHM_ERR_UNSUPPORTED) {
                uhm_unlock(&heap->lock);
                uhm_set_error(heap, result);
                return;
            }
        }
        result = uhm_slab_remote_free_slot_prepare_lockless(
            heap, slot, &remote_slab, &remote_needs_decommit_check);
        if (result == UHM_ERR_UNSUPPORTED) {
            result = uhm_slab_remote_free_slot_locked(heap, slot);
            uhm_unlock(&heap->lock);
            if (result != UHM_OK) {
                uhm_set_error(heap, result);
            }
            return;
        }
        uhm_unlock(&heap->lock);
        if (result == UHM_OK) {
            result = uhm_slab_remote_free_slot_publish_lockless(
                heap, remote_slab, slot, remote_needs_decommit_check);
        }
        if (result != UHM_OK) {
            uhm_set_error(heap, result);
        }
        return;
    }
    block = uhm_find_live_block_by_payload_from(
        heap, ptr, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_FREE);
    if (!block) {
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, UHM_ERR_CORRUPTION);
        return;
    }
    result = uhm_restore_page_private_block_rw(heap, block);
    if (result != UHM_OK) {
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, result);
        return;
    }
    uhm_live_block_tree_remove(heap, block);
    uhm_stats_malloc_live_sub(heap, (uintptr_t)uhm_block_payload(block),
                              block->requested_size);
    block->requested_size = 0;
    block->size_class = UHM_SIZE_CLASS_NONE;
    block->generation = uhm_heap_generation_next(heap);
    uhm_block_descriptor_set(block);
    uhm_free_list_insert(heap, block);
    result = uhm_coalesce_block(heap, &block);
    if (result == UHM_OK) {
        result = uhm_block_decommit_free_extent(heap, block, 1);
    }
    uhm_unlock(&heap->lock);
    if (result != UHM_OK && result != UHM_ERR_UNSUPPORTED &&
        result != UHM_ERR_PLATFORM) {
        uhm_set_error(heap, result);
    }
}

static void UHM_CALL uhm_api_free(uhm_heap *heap, void *ptr) {
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
    if (uhm_tls_free_pinned(heap, ptr)) {
        uhm_heap_op_end(heap, op_shard);
        return;
    }
    uhm_free_pinned(heap, ptr);
    uhm_heap_op_end(heap, op_shard);
}

static void *uhm_realloc_pinned(uhm_heap *heap, void *ptr, size_t new_size) {
    size_t old_size;
    size_t wanted;
    size_t wanted_payload;
    uint16_t size_class;
    void *new_ptr;
    uhm_block *block;
    uhm_block *next;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_result result;
    uint32_t slot_flags;
    uint32_t slot_requested;

    if (!ptr) {
        return uhm_aligned_alloc_pinned(heap, UHM_MIN_ALIGNMENT, new_size);
    }
    if (!uhm_heap_is_accessible_pinned(heap)) {
        return 0;
    }
    if (new_size == 0) {
        uhm_free_pinned(heap, ptr);
        return 0;
    }

    result = uhm_prepare_alloc_size(UHM_MIN_ALIGNMENT, new_size,
                                    &wanted, &wanted_payload, &size_class);
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
        return 0;
    }
    (void)wanted_payload;

    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        return 0;
    }
    uhm_metadata_epoch_advance(heap);
    slab = 0;
    slot = uhm_find_slab_slot_by_payload_with_slab(heap, ptr, 1, &slab);
    if (slot) {
        if (!uhm_slab_slot_live_state_mirror_valid_with_slab(
                slot, slab, &slot_flags, &slot_requested)) {
            uhm_unlock(&heap->lock);
            uhm_set_error(heap, UHM_ERR_CORRUPTION);
            return 0;
        }
        old_size = slot_requested;
        if (slab && new_size <= slab->slot_size) {
            if (!uhm_slab_slot_state_cas(slot, slot_flags,
                                         (uint32_t)old_size, 0u,
                                         (uint32_t)new_size)) {
                uhm_unlock(&heap->lock);
                uhm_set_error(heap, UHM_ERR_CORRUPTION);
                return 0;
            }
            if (new_size > old_size) {
                uhm_stats_malloc_live_add(heap, (uintptr_t)ptr,
                                          new_size - old_size);
            } else {
                uhm_stats_malloc_live_sub(heap, (uintptr_t)ptr,
                                          old_size - new_size);
            }
            uhm_unlock(&heap->lock);
            return ptr;
        }
        uhm_unlock(&heap->lock);

        new_ptr = uhm_aligned_alloc_pinned(heap, UHM_MIN_ALIGNMENT, new_size);
        if (!new_ptr) {
            return 0;
        }
        memcpy(new_ptr, ptr, old_size);
        uhm_free_pinned(heap, ptr);
        return new_ptr;
    }

    block = uhm_find_live_block_by_payload_from(
        heap, ptr, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_REALLOC);
    if (!block) {
        uhm_unlock(&heap->lock);
        uhm_set_error(heap, UHM_ERR_CORRUPTION);
        return 0;
    }
    old_size = block->requested_size;
    if ((block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) != 0) {
        result = uhm_restore_page_private_block_rw(heap, block);
        if (result != UHM_OK) {
            uhm_unlock(&heap->lock);
            uhm_set_error(heap, result);
            return 0;
        }
    }
    if (old_size >= new_size || block->total_size >= wanted) {
        if (new_size > old_size) {
            uhm_stats_malloc_live_add(heap, (uintptr_t)ptr,
                                      new_size - old_size);
        } else {
            uhm_stats_malloc_live_sub(heap, (uintptr_t)ptr,
                                      old_size - new_size);
        }
        block->requested_size = new_size;
        block->size_class = size_class;
        block->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(block);
        uhm_unlock(&heap->lock);
        return ptr;
    }

    next = block->next_phys;
    if (next && next->magic == UHM_BLOCK_MAGIC &&
        (next->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
        (next->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0) {
        uintptr_t block_end;
        size_t merged_size;
        if (uhm_range_overflows((uintptr_t)block, block->total_size,
                                &block_end) ||
            block_end != (uintptr_t)next ||
            uhm_add_overflows_size(block->total_size, next->total_size,
                                   &merged_size)) {
            uhm_unlock(&heap->lock);
            uhm_set_error(heap, UHM_ERR_CORRUPTION);
            return 0;
        }
        if (merged_size < wanted) {
            uhm_unlock(&heap->lock);
            new_ptr = uhm_aligned_alloc_pinned(heap, UHM_MIN_ALIGNMENT,
                                               new_size);
            if (!new_ptr) {
                return 0;
            }
            memcpy(new_ptr, ptr, old_size);
            uhm_free_pinned(heap, ptr);
            return new_ptr;
        }
        result = uhm_block_recommit_free_extent(heap, next, 1);
        if (result != UHM_OK) {
            uhm_unlock(&heap->lock);
            uhm_set_error(heap, result);
            return 0;
        }
        uhm_free_list_remove(heap, next);
        block->total_size = merged_size;
        block->next_phys = next->next_phys;
        if (next->next_phys) {
            next->next_phys->prev_phys = block;
        } else {
            heap->core.last_block = block;
        }
        uhm_extent_descriptor_retire(&next->desc);
        next->magic = 0;
        next->size_class = UHM_SIZE_CLASS_NONE;
        next->free_tree_priority = 0;
        next->live_tree_priority = 0;
        uhm_stats_malloc_live_add(heap, (uintptr_t)ptr,
                                  new_size - old_size);
        block->requested_size = new_size;
        block->size_class = size_class;
        block->generation = uhm_heap_generation_next(heap);
        uhm_block_descriptor_set(block);
        (void)uhm_split_block(heap, block, wanted, 1);
        uhm_unlock(&heap->lock);
        return ptr;
    }
    uhm_unlock(&heap->lock);

    new_ptr = uhm_aligned_alloc_pinned(heap, UHM_MIN_ALIGNMENT, new_size);
    if (!new_ptr) {
        return 0;
    }
    memcpy(new_ptr, ptr, old_size);
    uhm_free_pinned(heap, ptr);
    return new_ptr;
}

static void *UHM_CALL uhm_api_realloc(uhm_heap *heap, void *ptr, size_t new_size) {
    void *new_ptr;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return 0;
    }
    if (uhm_treap_health_runtime_failed(heap)) {
        uhm_heap_op_end(heap, op_shard);
        return 0;
    }
    new_ptr = uhm_realloc_pinned(heap, ptr, new_size);
    uhm_heap_op_end(heap, op_shard);
    return new_ptr;
}

static size_t UHM_CALL uhm_api_malloc_usable_size(uhm_heap *heap, const void *ptr) {
    uhm_block *block;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    size_t result = 0;
    size_t op_shard;

    if (!uhm_heap_op_begin(heap, &op_shard)) {
        return 0;
    }
    if (!ptr) {
        uhm_heap_op_end(heap, op_shard);
        return 0;
    }
    uhm_lock(&heap->lock);
    if (!uhm_heap_is_accessible_pinned(heap)) {
        uhm_unlock(&heap->lock);
        uhm_heap_op_end(heap, op_shard);
        return 0;
    }
    slab = 0;
    slot = uhm_find_slab_slot_by_payload_with_slab(heap, ptr, 1, &slab);
    if (slot) {
        result = slab ? slab->slot_size : 0u;
        uhm_unlock(&heap->lock);
        uhm_heap_op_end(heap, op_shard);
        return result;
    }
    block = uhm_find_live_block_by_payload_from(
        heap, ptr, UHM_LIVE_BLOCK_LOOKUP_CALLSITE_USABLE_SIZE);
    if (block) {
        result = uhm_block_usable_size(block);
    }
    uhm_unlock(&heap->lock);
    uhm_heap_op_end(heap, op_shard);
    return result;
}

