typedef struct uhm_vm_call_record {
    uhm_result result;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t elapsed_ns;
#endif
    uint32_t platform_error;
    int did_call;
} uhm_vm_call_record;

#if UHMAS_ENABLE_DIAGNOSTICS
#define UHM_VM_CALL_BEGIN(heap_, begin_name_) \
    uint64_t begin_name_ = uhm_diag_begin_ns((heap_))
#define UHM_VM_CALL_RECORD_ELAPSED(heap_, record_, begin_name_) \
    do { \
        (record_).elapsed_ns = uhm_diag_elapsed_ns((heap_), (begin_name_)); \
    } while (0)
#define UHM_VM_CALL_PUBLISH_RECORD(heap_, record_, calls_, failures_, ns_) \
    do { \
        if ((record_)->did_call) { \
            uhm_diag_note_vm_call_elapsed((calls_), (failures_), (ns_), \
                                          (record_)->elapsed_ns, \
                                          (record_)->result); \
        } \
    } while (0)
#else
#define UHM_VM_CALL_BEGIN(heap_, begin_name_) ((void)(heap_))
#define UHM_VM_CALL_RECORD_ELAPSED(heap_, record_, begin_name_) \
    do { \
        (void)(heap_); \
        (void)(record_); \
    } while (0)
#define UHM_VM_CALL_PUBLISH_RECORD(heap_, record_, calls_, failures_, ns_) \
    do { \
        (void)(heap_); \
        (void)(record_); \
        (void)(calls_); \
        (void)(failures_); \
        (void)(ns_); \
    } while (0)
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
#define UHM_VM_DIRECT_BEGIN(heap_, begin_name_) \
    uint64_t begin_name_ = uhm_diag_begin_ns((heap_))
#define UHM_VM_DIRECT_PUBLISH(heap_, begin_name_, calls_, failures_, ns_, \
                              lock_held_calls_, result_) \
    do { \
        uhm_diag_note_vm_call((heap_), (calls_), (failures_), (ns_), \
                              (begin_name_), (result_)); \
        uhm_diag_note_vm_lock_held_call((heap_), (lock_held_calls_), \
                                        (begin_name_), (result_)); \
    } while (0)
#else
#define UHM_VM_DIRECT_BEGIN(heap_, begin_name_) ((void)(heap_))
#define UHM_VM_DIRECT_PUBLISH(heap_, begin_name_, calls_, failures_, ns_, \
                              lock_held_calls_, result_) \
    do { \
        (void)(heap_); \
        (void)(calls_); \
        (void)(failures_); \
        (void)(ns_); \
        (void)(lock_held_calls_); \
        (void)(result_); \
    } while (0)
#endif

static uhm_vm_call_record uhm_vm_protect_unlocked_call(uhm_heap *heap,
                                                       void *addr,
                                                       size_t size,
                                                       uint32_t prot) {
    uhm_vm_call_record record;

    memset(&record, 0, sizeof(record));
    record.result = UHM_OK;
    if (size == 0) {
        return record;
    }
    record.result = uhm_vm_validate_prot(heap, prot);
    if (record.result != UHM_OK) {
        return record;
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_PROTECT) == 0 ||
        heap->F.vm.protect == 0) {
        if ((heap->F.vm.capabilities & UHM_VM_USERMODE_LOGICAL_VM) != 0) {
            (void)addr;
            (void)prot;
            record.result = UHM_OK;
        } else {
            record.result = UHM_ERR_UNSUPPORTED;
        }
        return record;
    }

    uhm_platform_error_before_call();
    UHM_VM_CALL_BEGIN(heap, begin_ns);
    record.result = heap->F.vm.protect(heap->F.vm.user, addr, size, prot);
    UHM_VM_CALL_RECORD_ELAPSED(heap, record, begin_ns);
    record.did_call = 1;
    if (record.result == UHM_ERR_PLATFORM) {
        record.platform_error = uhm_platform_error_after_call();
    }
    return record;
}

static uhm_result uhm_vm_protect_publish_record_locked(
    uhm_heap *heap, const uhm_vm_call_record *record) {
    if (!record) {
        return UHM_ERR_CORRUPTION;
    }
    UHM_VM_CALL_PUBLISH_RECORD(heap, record, &heap->core.vm_protect_calls,
                               &heap->core.vm_protect_failures,
                               &heap->core.vm_protect_ns);
    return uhm_record_vm_result_captured(heap, record->result,
                                         record->platform_error);
}

static uhm_vm_call_record uhm_vm_decommit_unlocked_call(uhm_heap *heap,
                                                        void *addr,
                                                        size_t size) {
    uhm_vm_call_record record;

    memset(&record, 0, sizeof(record));
    record.result = UHM_OK;
    if (size == 0) {
        return record;
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_DECOMMIT) != 0 &&
        heap->F.vm.decommit != 0) {
        uhm_platform_error_before_call();
        UHM_VM_CALL_BEGIN(heap, begin_ns);
        record.result = heap->F.vm.decommit(heap->F.vm.user, addr, size);
        UHM_VM_CALL_RECORD_ELAPSED(heap, record, begin_ns);
        record.did_call = 1;
        if (record.result == UHM_ERR_PLATFORM) {
            record.platform_error = uhm_platform_error_after_call();
        }
        return record;
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_FIXED_REMAP) != 0 &&
        heap->F.vm.fixed_remap != 0) {
        uhm_platform_error_before_call();
        UHM_VM_CALL_BEGIN(heap, begin_ns);
        record.result =
            heap->F.vm.fixed_remap(heap->F.vm.user, addr, size, UHM_PROT_NONE);
        UHM_VM_CALL_RECORD_ELAPSED(heap, record, begin_ns);
        record.did_call = 2;
        if (record.result == UHM_ERR_PLATFORM) {
            record.platform_error = uhm_platform_error_after_call();
        }
        return record;
    }
    if ((heap->F.vm.capabilities & UHM_VM_USERMODE_LOGICAL_VM) != 0) {
        record.result = UHM_OK;
    } else {
        record.result = UHM_ERR_UNSUPPORTED;
    }
    return record;
}

static uhm_result uhm_vm_decommit_publish_record_locked(
    uhm_heap *heap, const uhm_vm_call_record *record) {
    if (!record) {
        return UHM_ERR_CORRUPTION;
    }
    if (record->did_call == 1) {
        UHM_VM_CALL_PUBLISH_RECORD(heap, record, &heap->core.vm_decommit_calls,
                                   &heap->core.vm_decommit_failures,
                                   &heap->core.vm_decommit_ns);
    } else if (record->did_call == 2) {
        UHM_VM_CALL_PUBLISH_RECORD(
            heap, record, &heap->core.vm_fixed_remap_calls,
            &heap->core.vm_fixed_remap_failures,
            &heap->core.vm_fixed_remap_ns);
    }
    return uhm_record_vm_result_captured(heap, record->result,
                                         record->platform_error);
}

static void uhm_vm_decommit_rollback_unlocked_preserve_error(uhm_heap *heap,
                                                             void *addr,
                                                             size_t size) {
    uhm_vm_call_record record;
    uint32_t saved_platform_error;

    if (!heap || size == 0) {
        return;
    }
    saved_platform_error =
        atomic_load_explicit(&heap->platform_error, UHM_ATOMIC_ACQUIRE);
    uhm_unlock(&heap->lock);
    record = uhm_vm_decommit_unlocked_call(heap, addr, size);
    uhm_lock(&heap->lock);
    (void)uhm_vm_decommit_publish_record_locked(heap, &record);
    atomic_store_explicit(&heap->platform_error, saved_platform_error,
                          UHM_ATOMIC_RELEASE);
}

static uhm_vm_call_record uhm_vm_commit_unlocked_call(uhm_heap *heap,
                                                      void *addr,
                                                      size_t size,
                                                      uint32_t prot) {
    uhm_vm_call_record record;

    memset(&record, 0, sizeof(record));
    record.result = UHM_OK;
    if (size == 0) {
        return record;
    }
    record.result = uhm_vm_validate_prot(heap, prot);
    if (record.result != UHM_OK) {
        return record;
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_COMMIT) == 0 ||
        heap->F.vm.commit == 0) {
        if ((heap->F.vm.capabilities & UHM_VM_USERMODE_LOGICAL_VM) != 0) {
            record.result = UHM_OK;
        } else {
            record.result = UHM_ERR_UNSUPPORTED;
        }
        return record;
    }

    uhm_platform_error_before_call();
    UHM_VM_CALL_BEGIN(heap, begin_ns);
    record.result = heap->F.vm.commit(heap->F.vm.user, addr, size, prot);
    UHM_VM_CALL_RECORD_ELAPSED(heap, record, begin_ns);
    record.did_call = 1;
    if (record.result == UHM_ERR_PLATFORM) {
        record.platform_error = uhm_platform_error_after_call();
    }
    return record;
}

static uhm_result uhm_vm_commit_publish_record_locked(
    uhm_heap *heap, const uhm_vm_call_record *record) {
    if (!record) {
        return UHM_ERR_CORRUPTION;
    }
    UHM_VM_CALL_PUBLISH_RECORD(heap, record, &heap->core.vm_commit_calls,
                               &heap->core.vm_commit_failures,
                               &heap->core.vm_commit_ns);
    return uhm_record_vm_result_captured(heap, record->result,
                                         record->platform_error);
}

static void uhm_vm_commit_rollback_unlocked_preserve_error(uhm_heap *heap,
                                                           void *addr,
                                                           size_t size,
                                                           uint32_t prot) {
    uhm_vm_call_record record;
    uint32_t saved_platform_error;

    if (!heap || size == 0) {
        return;
    }
    saved_platform_error =
        atomic_load_explicit(&heap->platform_error, UHM_ATOMIC_ACQUIRE);
    uhm_unlock(&heap->lock);
    record = uhm_vm_commit_unlocked_call(heap, addr, size, prot);
    uhm_lock(&heap->lock);
    (void)uhm_vm_commit_publish_record_locked(heap, &record);
    atomic_store_explicit(&heap->platform_error, saved_platform_error,
                          UHM_ATOMIC_RELEASE);
}

static int uhm_numa_policy_is_valid(uint32_t policy) {
    return policy == UHM_NUMA_POLICY_DEFAULT ||
           policy == UHM_NUMA_POLICY_FIRST_TOUCH ||
           policy == UHM_NUMA_POLICY_PREFERRED ||
           policy == UHM_NUMA_POLICY_BIND;
}

static uint64_t uhm_numa_policy_capability(uint32_t policy) {
    switch (policy) {
    case UHM_NUMA_POLICY_PREFERRED:
        return UHM_VM_CAN_NUMA_PREFERRED;
    case UHM_NUMA_POLICY_BIND:
        return UHM_VM_CAN_NUMA_BIND;
    default:
        return 0;
    }
}

static uhm_result uhm_numa_validate_node(uint32_t policy, int32_t node) {
    return (policy == UHM_NUMA_POLICY_PREFERRED ||
            policy == UHM_NUMA_POLICY_BIND) &&
                   node < 0
               ? UHM_ERR_INVALID_ARGUMENT
               : UHM_OK;
}

static uint32_t uhm_numa_effective_policy(uhm_heap *heap, uint32_t policy) {
    if (policy != UHM_NUMA_POLICY_DEFAULT) {
        return policy;
    }
    return heap ? heap->core.numa_policy : UHM_NUMA_POLICY_DEFAULT;
}

static int32_t uhm_numa_effective_node(uhm_heap *heap, uint32_t policy,
                                       int32_t node) {
    return policy == UHM_NUMA_POLICY_DEFAULT && heap
               ? heap->core.preferred_numa_node
               : node;
}

static uhm_result uhm_numa_validate_vm_policy(const uhm_vm_ops *vm,
                                              uint32_t policy) {
    uint64_t needed;

    if (!uhm_numa_policy_is_valid(policy)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    needed = uhm_numa_policy_capability(policy);
    if (needed != 0 &&
        (((vm ? vm->capabilities : 0u) & needed) == 0 ||
         !vm || !vm->numa_hint)) {
        return UHM_ERR_UNSUPPORTED;
    }
    return UHM_OK;
}

static uhm_result uhm_numa_validate_request(uhm_heap *heap, uint32_t policy,
                                             int32_t node,
                                             uint32_t map_flags) {
    uint64_t needed;

    node = uhm_numa_effective_node(heap, policy, node);
    policy = uhm_numa_effective_policy(heap, policy);
    if (!uhm_numa_policy_is_valid(policy)) {
        uhm_clear_platform_error(heap);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_numa_validate_node(policy, node) != UHM_OK) {
        uhm_clear_platform_error(heap);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    needed = uhm_numa_policy_capability(policy);
    if (needed == 0) {
        uhm_clear_platform_error(heap);
        return UHM_OK;
    }
    if ((map_flags & UHM_MAP_NO_COMMIT) != 0 ||
        (heap->F.vm.capabilities & needed) == 0 || !heap->F.vm.numa_hint) {
        uhm_clear_platform_error(heap);
        return UHM_ERR_UNSUPPORTED;
    }
    return UHM_OK;
}

static uhm_vm_call_record uhm_vm_numa_hint_unlocked_call(uhm_heap *heap,
                                                         void *addr,
                                                         size_t size,
                                                         uint32_t policy,
                                                         int32_t node,
                                                         uint32_t flags) {
    uhm_vm_call_record record;

    memset(&record, 0, sizeof(record));
    record.result = UHM_OK;
    if (size == 0) {
        return record;
    }
    node = uhm_numa_effective_node(heap, policy, node);
    record.result = uhm_numa_validate_request(heap, policy, node, flags);
    if (record.result != UHM_OK) {
        return record;
    }
    policy = uhm_numa_effective_policy(heap, policy);
    if (policy == UHM_NUMA_POLICY_DEFAULT ||
        policy == UHM_NUMA_POLICY_FIRST_TOUCH) {
        record.result = UHM_OK;
        return record;
    }

    uhm_platform_error_before_call();
    UHM_VM_CALL_BEGIN(heap, begin_ns);
    record.result = heap->F.vm.numa_hint(heap->F.vm.user, addr, size, policy,
                                         node, flags);
    UHM_VM_CALL_RECORD_ELAPSED(heap, record, begin_ns);
    record.did_call = 1;
    if (record.result == UHM_ERR_PLATFORM) {
        record.platform_error = uhm_platform_error_after_call();
    }
    return record;
}

static uhm_result uhm_vm_numa_hint_publish_record_locked(
    uhm_heap *heap, const uhm_vm_call_record *record) {
    if (!record) {
        return UHM_ERR_CORRUPTION;
    }
    UHM_VM_CALL_PUBLISH_RECORD(heap, record, &heap->core.vm_numa_hint_calls,
                               &heap->core.vm_numa_hint_failures,
                               &heap->core.vm_numa_hint_ns);
    return uhm_record_vm_result_captured(heap, record->result,
                                         record->platform_error);
}

static void uhm_vm_numa_hint_publish_best_effort_record_locked(
    uhm_heap *heap, const uhm_vm_call_record *record) {
    if (!record) {
        return;
    }
    UHM_VM_CALL_PUBLISH_RECORD(heap, record, &heap->core.vm_numa_hint_calls,
                               &heap->core.vm_numa_hint_failures,
                               &heap->core.vm_numa_hint_ns);
}

static uhm_result uhm_vm_reserve(uhm_heap *heap, void **addr, size_t size, size_t alignment) {
    uhm_result result;
    if (size == 0) {
        uhm_clear_platform_error(heap);
        return UHM_OK;
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_RESERVE) == 0 || heap->F.vm.reserve == 0) {
        uhm_clear_platform_error(heap);
        return UHM_ERR_UNSUPPORTED;
    }
    uhm_platform_error_before_call();
    UHM_VM_DIRECT_BEGIN(heap, begin_ns);
    result = heap->F.vm.reserve(heap->F.vm.user, addr, size, alignment);
    UHM_VM_DIRECT_PUBLISH(heap, begin_ns, &heap->core.vm_reserve_calls,
                          &heap->core.vm_reserve_failures,
                          &heap->core.vm_reserve_ns,
                          &heap->core.vm_reserve_lock_held_calls, result);
    return uhm_record_vm_result(heap, result);
}

static uhm_result uhm_vm_commit(uhm_heap *heap, void *addr, size_t size, uint32_t prot) {
    uhm_result result;
    if (size == 0) {
        uhm_clear_platform_error(heap);
        return UHM_OK;
    }
    result = uhm_vm_validate_prot(heap, prot);
    if (result != UHM_OK) {
        return result;
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_COMMIT) == 0 || heap->F.vm.commit == 0) {
        if ((heap->F.vm.capabilities & UHM_VM_USERMODE_LOGICAL_VM) != 0) {
            uhm_clear_platform_error(heap);
            return UHM_OK;
        }
        uhm_clear_platform_error(heap);
        return UHM_ERR_UNSUPPORTED;
    }
    uhm_platform_error_before_call();
    UHM_VM_DIRECT_BEGIN(heap, begin_ns);
    result = heap->F.vm.commit(heap->F.vm.user, addr, size, prot);
    UHM_VM_DIRECT_PUBLISH(heap, begin_ns, &heap->core.vm_commit_calls,
                          &heap->core.vm_commit_failures,
                          &heap->core.vm_commit_ns,
                          &heap->core.vm_commit_lock_held_calls, result);
    return uhm_record_vm_result(heap, result);
}

static uhm_result uhm_vm_decommit(uhm_heap *heap, void *addr, size_t size) {
    uhm_result result;
    if (size == 0) {
        uhm_clear_platform_error(heap);
        return UHM_OK;
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_DECOMMIT) != 0 && heap->F.vm.decommit != 0) {
        uhm_platform_error_before_call();
        UHM_VM_DIRECT_BEGIN(heap, begin_ns);
        result = heap->F.vm.decommit(heap->F.vm.user, addr, size);
        UHM_VM_DIRECT_PUBLISH(
            heap, begin_ns, &heap->core.vm_decommit_calls,
            &heap->core.vm_decommit_failures, &heap->core.vm_decommit_ns,
            &heap->core.vm_decommit_lock_held_calls, result);
        return uhm_record_vm_result(heap, result);
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_FIXED_REMAP) != 0 && heap->F.vm.fixed_remap != 0) {
        uhm_platform_error_before_call();
        UHM_VM_DIRECT_BEGIN(heap, begin_ns);
        result = heap->F.vm.fixed_remap(heap->F.vm.user, addr, size, UHM_PROT_NONE);
        UHM_VM_DIRECT_PUBLISH(
            heap, begin_ns, &heap->core.vm_fixed_remap_calls,
            &heap->core.vm_fixed_remap_failures,
            &heap->core.vm_fixed_remap_ns,
            &heap->core.vm_fixed_remap_lock_held_calls, result);
        return uhm_record_vm_result(heap, result);
    }
    if ((heap->F.vm.capabilities & UHM_VM_USERMODE_LOGICAL_VM) != 0) {
        uhm_clear_platform_error(heap);
        return UHM_OK;
    }
    uhm_clear_platform_error(heap);
    return UHM_ERR_UNSUPPORTED;
}

static size_t uhm_diag_range_overlap_size(uintptr_t a, size_t a_size,
                                          uintptr_t b, size_t b_size) {
    uintptr_t a_end;
    uintptr_t b_end;
    uintptr_t begin;
    uintptr_t end;

    if (a_size == 0 || b_size == 0 ||
        uhm_range_overflows(a, a_size, &a_end) ||
        uhm_range_overflows(b, b_size, &b_end)) {
        return 0;
    }
    begin = a > b ? a : b;
    end = a_end < b_end ? a_end : b_end;
    if (end <= begin) {
        return 0;
    }
    return (size_t)(end - begin);
}

static void uhm_diag_note_recent_decommit(uhm_heap *heap, uintptr_t addr,
                                          size_t size) {
    size_t slot;

    if (!heap || size == 0) {
        return;
    }
    slot = heap->core.recent_decommit_next % UHM_DECOMMIT_RECENT_RANGE_SLOTS;
    heap->core.recent_decommit_addrs[slot] = addr;
    heap->core.recent_decommit_sizes[slot] = size;
    heap->core.recent_decommit_next =
        (slot + 1u) % UHM_DECOMMIT_RECENT_RANGE_SLOTS;
}

static void uhm_vm_account_decommit(uhm_heap *heap, uintptr_t addr,
                                    size_t size) {
    if (!heap || size == 0) {
        return;
    }
    heap->core.decommit_events++;
    heap->core.decommit_bytes += size;
    uhm_diag_note_recent_decommit(heap, addr, size);
    if ((heap->F.vm.capabilities & UHM_VM_USERMODE_LOGICAL_VM) != 0) {
        heap->stats.logical_decommitted_bytes += size;
    } else if ((heap->F.vm.capabilities & UHM_VM_DECOMMIT_DROPS_PHYSICAL) != 0) {
        heap->stats.physical_released_bytes += size;
    }
}

static size_t uhm_diag_note_recommit(uhm_heap *heap, uintptr_t addr,
                                     size_t size) {
    size_t i;
    size_t same_addr_bytes = 0;

    if (!heap || size == 0) {
        return 0;
    }
    heap->core.recommit_events++;
    heap->core.recommit_bytes += size;
    for (i = 0; i < UHM_DECOMMIT_RECENT_RANGE_SLOTS; ++i) {
        same_addr_bytes += uhm_diag_range_overlap_size(
            addr, size, heap->core.recent_decommit_addrs[i],
            heap->core.recent_decommit_sizes[i]);
        if (same_addr_bytes >= size) {
            same_addr_bytes = size;
            break;
        }
    }
    if (same_addr_bytes != 0) {
        heap->core.decommit_recommit_same_addr_events++;
        heap->core.decommit_recommit_same_addr_bytes += same_addr_bytes;
    }
    return same_addr_bytes;
}

static size_t uhm_stats_malloc_live_shard(uintptr_t key) {
    uintptr_t x = key >> 4;
    x ^= x >> 17;
    x *= (uintptr_t)0xed5ad4bbu;
    x ^= x >> 11;
    return (size_t)x & (UHM_MALLOC_LIVE_STAT_SHARDS - 1u);
}

static size_t uhm_stats_malloc_live_load(const uhm_heap *heap) {
    size_t total = 0;
    size_t cached = 0;
    size_t i;
    uhm_thread_ctx *ctx;
    uhm_tls_cache *cache;

    if (!heap) {
        return 0u;
    }
    for (i = 0; i < UHM_MALLOC_LIVE_STAT_SHARDS; ++i) {
        size_t value = atomic_load_explicit(
            &heap->stats.malloc_live_shards[i].value, UHM_ATOMIC_RELAXED);
        if (total > SIZE_MAX - value) {
            return SIZE_MAX;
        }
        total += value;
    }
    for (ctx = heap->core.thread_ctx_head; ctx; ctx = ctx->next_active) {
        size_t ctx_cached = atomic_load_explicit(
            &ctx->cached_requested_bytes, UHM_ATOMIC_ACQUIRE);
        if (cached > SIZE_MAX - ctx_cached) {
            cached = SIZE_MAX;
            break;
        }
        cached += ctx_cached;
    }
    if (cached != SIZE_MAX) {
        for (cache = heap->core.tls_cache_head; cache;
             cache = cache->next_active) {
            size_t tls_cached = atomic_load_explicit(
                &cache->cached_requested_bytes, UHM_ATOMIC_ACQUIRE);
            if (cached > SIZE_MAX - tls_cached) {
                cached = SIZE_MAX;
                break;
            }
            cached += tls_cached;
        }
    }
    return total > cached ? total - cached : 0u;
}

static void uhm_stats_malloc_live_add(uhm_heap *heap, uintptr_t key,
                                      size_t size) {
    size_t shard;

    if (!heap || size == 0) {
        return;
    }
    shard = uhm_stats_malloc_live_shard(key);
    (void)atomic_fetch_add_explicit(
        &heap->stats.malloc_live_shards[shard].value, size,
        UHM_ATOMIC_RELAXED);
}

static void uhm_stats_malloc_live_sub(uhm_heap *heap, uintptr_t key,
                                      size_t size) {
    size_t current;
    size_t shard;

    if (!heap || size == 0) {
        return;
    }
    shard = uhm_stats_malloc_live_shard(key);
    current = atomic_load_explicit(
        &heap->stats.malloc_live_shards[shard].value, UHM_ATOMIC_RELAXED);
    for (;;) {
        size_t desired = current >= size ? current - size : 0u;
        if (atomic_compare_exchange_weak_explicit(
                &heap->stats.malloc_live_shards[shard].value, &current,
                desired, UHM_ATOMIC_RELAXED, UHM_ATOMIC_RELAXED)) {
            return;
        }
    }
}

static void uhm_stats_malloc_live_replace(uhm_heap *heap, uintptr_t key,
                                          size_t old_size,
                                          size_t new_size) {
    if (!heap || old_size == new_size) {
        return;
    }
    if (new_size > old_size) {
        uhm_stats_malloc_live_add(heap, key, new_size - old_size);
    } else {
        uhm_stats_malloc_live_sub(heap, key, old_size - new_size);
    }
}

static void uhm_vm_rollback_failed_commit(uhm_heap *heap, void *addr, size_t size) {
    uint32_t saved_platform_error;

    if (!heap || size == 0) {
        return;
    }
    saved_platform_error =
        atomic_load_explicit(&heap->platform_error, UHM_ATOMIC_ACQUIRE);
    (void)uhm_vm_decommit(heap, addr, size);
    atomic_store_explicit(&heap->platform_error, saved_platform_error,
                          UHM_ATOMIC_RELEASE);
}

static uhm_result uhm_vm_release(uhm_heap *heap, void *addr, size_t size) {
    uhm_result result;
    if (size == 0) {
        uhm_clear_platform_error(heap);
        return UHM_OK;
    }
    if ((heap->F.vm.capabilities & UHM_VM_CAN_RELEASE) == 0 || heap->F.vm.release == 0) {
        uhm_clear_platform_error(heap);
        return UHM_ERR_UNSUPPORTED;
    }
    uhm_platform_error_before_call();
    UHM_VM_DIRECT_BEGIN(heap, begin_ns);
    result = heap->F.vm.release(heap->F.vm.user, addr, size);
    UHM_VM_DIRECT_PUBLISH(heap, begin_ns, &heap->core.vm_release_calls,
                          &heap->core.vm_release_failures,
                          &heap->core.vm_release_ns,
                          &heap->core.vm_release_lock_held_calls, result);
    return uhm_record_vm_result(heap, result);
}

static void uhm_open_cleanup(uhm_heap *heap, uintptr_t arena_base, size_t arena_size,
                             size_t control_size, int owns_arena, int control_committed) {
    if (heap) {
        uhm_tls_key_delete(heap);
        if (control_committed && control_size != 0) {
            (void)uhm_vm_decommit(heap, (void *)arena_base, control_size);
        }
        if (owns_arena && arena_base != 0 && arena_size != 0) {
            (void)uhm_vm_release(heap, (void *)arena_base, arena_size);
        }
        atomic_store_explicit(&heap->lifecycle_state, 0u,
                              UHM_ATOMIC_RELEASE);
        heap->F.canary = 0;
        heap->magic = 0;
    }
}

static uhm_result uhm_page_mark_range(uhm_heap *heap, uintptr_t addr, size_t size,
                                      uint32_t owner, uint32_t va_state,
                                      uint32_t commit_state, uint32_t prot,
                                      uint64_t descriptor_id, uint64_t generation,
                                      int create);
static uhm_result uhm_mmap_free_insert(uhm_heap *heap, uintptr_t addr, size_t size,
                                       int allow_emergency);
static uhm_result uhm_mmap_free_claim(uhm_heap *heap, uintptr_t addr, size_t size,
                                      int *claimed);
static int uhm_mmap_free_take_auto(uhm_heap *heap, size_t size, uintptr_t *addr_out);
static void uhm_mmap_recent_free_forget(uhm_heap *heap,
                                        const uhm_free_extent *extent);

