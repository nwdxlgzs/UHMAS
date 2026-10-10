static uint64_t uhm_tree_priority_mix(uint64_t value);
static uint64_t uhm_tree_priority_mix_salted(const uhm_heap *heap,
                                             uint64_t value);
static uint64_t uhm_tree_priority_mix_for_heap(uhm_heap *heap,
                                               uint64_t value,
                                               uint64_t *tree_calls,
                                               uint64_t *tree_ns,
                                               uint64_t *tree_max_ns);

static int uhm_malloc_free_tree_less(const uhm_block *a, const uhm_block *b) {
    if (a->total_size != b->total_size) {
        return a->total_size < b->total_size;
    }
    return (uintptr_t)a < (uintptr_t)b;
}

static uint64_t uhm_block_free_tree_priority_value(uhm_heap *heap,
                                                   const uhm_block *block) {
    return uhm_tree_priority_mix_salted(heap, (uint64_t)(uintptr_t)block);
}

static uint64_t uhm_block_free_tree_priority_create(uhm_heap *heap,
                                                    const uhm_block *block) {
    return uhm_tree_priority_mix_for_heap(
        heap, (uint64_t)(uintptr_t)block,
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_priority_mix_calls),
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_priority_mix_ns),
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_priority_mix_max_ns));
}

static uint64_t uhm_block_live_tree_priority_value(uhm_heap *heap,
                                                   const uhm_block *block) {
    return uhm_tree_priority_mix_salted(
        heap,
        (uint64_t)(uintptr_t)uhm_block_payload((uhm_block *)block) ^
            UINT64_C(0x489a4f1d3c6b2e17));
}

static uint64_t uhm_block_live_tree_priority_create(uhm_heap *heap,
                                                    const uhm_block *block) {
    return uhm_tree_priority_mix_for_heap(
        heap,
        (uint64_t)(uintptr_t)uhm_block_payload((uhm_block *)block) ^
            UINT64_C(0x489a4f1d3c6b2e17),
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_priority_mix_calls),
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_priority_mix_ns),
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_priority_mix_max_ns));
}

static void uhm_block_free_tree_priority_refresh(uhm_heap *heap,
                                                 uhm_block *block) {
    if (!block) {
        return;
    }
    block->free_tree_priority =
        uhm_block_free_tree_priority_create(heap, block);
}

static void uhm_block_live_tree_priority_refresh(uhm_heap *heap,
                                                 uhm_block *block) {
    if (!block) {
        return;
    }
    block->live_tree_priority =
        uhm_block_live_tree_priority_create(heap, block);
}

static void uhm_block_tree_priority_init_free(uhm_heap *heap,
                                              uhm_block *block) {
    if (!block) {
        return;
    }
    uhm_block_free_tree_priority_refresh(heap, block);
    block->live_tree_priority = 0;
}

static void uhm_block_tree_priority_init_live(uhm_heap *heap,
                                              uhm_block *block) {
    if (!block) {
        return;
    }
    block->free_tree_priority = 0;
    uhm_block_live_tree_priority_refresh(heap, block);
}

static void uhm_block_tree_priority_init(uhm_heap *heap, uhm_block *block) {
    if (!block) {
        return;
    }
    block->free_tree_priority = 0;
    block->live_tree_priority = 0;
    if ((block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
        (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0) {
        uhm_block_free_tree_priority_refresh(heap, block);
    } else if (block->requested_size != 0) {
        uhm_block_live_tree_priority_refresh(heap, block);
    }
}

static void uhm_block_free_tree_priority_ensure(uhm_heap *heap,
                                                uhm_block *block) {
    if (block && block->free_tree_priority == 0) {
        uhm_block_free_tree_priority_refresh(heap, block);
    }
}

static void uhm_block_live_tree_priority_ensure(uhm_heap *heap,
                                                uhm_block *block) {
    if (block && block->live_tree_priority == 0) {
        uhm_block_live_tree_priority_refresh(heap, block);
    }
}

static void uhm_diag_mark_treap_health(uhm_heap *heap, uint64_t flags) {
    if (heap && flags != 0) {
        heap->core.treap_health_flags |= flags;
        (void)atomic_fetch_or_explicit(&heap->core.treap_repair_pending_flags,
                                       (uint32_t)flags, UHM_ATOMIC_RELEASE);
    }
}

static uint64_t uhm_diag_tree_depth_limit(uint64_t nodes) {
    uint64_t bits = 0;

    if (nodes == 0) {
        return 0;
    }
    while (nodes != 0) {
        ++bits;
        nodes >>= 1;
    }
    return bits * UINT64_C(4) + UINT64_C(16);
}

static int uhm_diag_note_tree_insert(uint64_t *current_nodes,
                                     uint64_t *max_nodes,
                                     uint64_t *depth_limit_exceeded,
                                     uint64_t depth) {
    uint64_t nodes;

    if (!current_nodes || !max_nodes || !depth_limit_exceeded) {
        return 0;
    }
    nodes = ++*current_nodes;
    uhm_diag_note_max_u64(max_nodes, nodes);
    if (depth > uhm_diag_tree_depth_limit(nodes)) {
        uhm_diag_note_u64_inc(depth_limit_exceeded);
        return 1;
    }
    return 0;
}

static void uhm_diag_note_tree_remove(uint64_t *current_nodes) {
    if (current_nodes && *current_nodes != 0) {
        --*current_nodes;
    }
}

static int uhm_diag_note_tree_operation_budget(uint64_t *budget_exceeded,
                                               uint64_t current_nodes,
                                               uint64_t steps) {
    if (budget_exceeded && current_nodes != 0 &&
        steps > uhm_diag_tree_depth_limit(current_nodes)) {
        uhm_diag_note_u64_inc(budget_exceeded);
        return 1;
    }
    return 0;
}

static uint64_t uhm_diag_tree_find_hard_limit(uint64_t current_nodes) {
    uint64_t limit = current_nodes != 0 ? current_nodes + UINT64_C(1)
                                        : UINT64_C(1024);

    if (limit < UINT64_C(1024)) {
        limit = UINT64_C(1024);
    }
    return limit;
}

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_diag_note_tree_priority_bucket(uint64_t *bucket_mask,
                                               uint64_t *bucket_max,
                                               uint64_t *buckets,
                                               uint64_t priority) {
    uint64_t bucket = priority >> 60;
    uint64_t count;

    if (!bucket_mask || !bucket_max || !buckets) {
        return;
    }
    count = ++buckets[bucket];
    *bucket_mask |= UINT64_C(1) << bucket;
    uhm_diag_note_max_u64(bucket_max, count);
}
#endif

static uint64_t uhm_tree_priority_mix(uint64_t value) {
#if UINTPTR_MAX <= UINT32_MAX
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);

    /*
     * A 32-bit target may lower uint64_t multiplication to a helper call.
     * Mix the two input lanes with native-width multiplies instead, then
     * preserve the existing 64-bit priority space and ordering contract.
     */
    low ^= high + UINT32_C(0x9e3779b9);
    high ^= low + UINT32_C(0x85ebca6b);
    low ^= (high >> 16) | (high << 16);
    high ^= (low >> 13) | (low << 19);
    low ^= low >> 16;
    low *= UINT32_C(0x7feb352d);
    low ^= low >> 15;
    low *= UINT32_C(0x846ca68b);
    low ^= low >> 16;
    high ^= high >> 16;
    high *= UINT32_C(0x7feb352d);
    high ^= high >> 15;
    high *= UINT32_C(0x846ca68b);
    high ^= high >> 16;
    return ((uint64_t)high << 32) | (uint64_t)low;
#else
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
#endif
}

static uint64_t uhm_tree_priority_mix_salted(const uhm_heap *heap,
                                             uint64_t value) {
    if (heap) {
        value ^= heap->core.treap_priority_salt;
    }
    return uhm_tree_priority_mix(value);
}

static uint64_t uhm_tree_priority_mix_for_heap(uhm_heap *heap,
                                               uint64_t value,
                                               uint64_t *tree_calls,
                                               uint64_t *tree_ns,
                                               uint64_t *tree_max_ns) {
#if !UHMAS_ENABLE_DIAGNOSTICS
    (void)tree_calls;
    (void)tree_ns;
    (void)tree_max_ns;
    if (heap) {
        value ^= heap->core.treap_priority_salt;
    }
    return uhm_tree_priority_mix(value);
#else
    uint64_t begin_ns;
    uint64_t priority;
    uint64_t elapsed_ns;

    if (heap) {
        value ^= heap->core.treap_priority_salt;
    }
    if (!heap || !uhm_spinlock_diagnostics_enabled(&heap->lock)) {
        return uhm_tree_priority_mix(value);
    }
    begin_ns = uhm_diag_timing_enabled(heap)
                   ? uhm_diag_now_ns(
                         uhm_spinlock_diagnostics_timer_frequency(&heap->lock))
                   : UINT64_C(0);
    priority = uhm_tree_priority_mix(value);
    elapsed_ns =
        begin_ns != 0 ? uhm_diag_elapsed_ns(heap, begin_ns) : UINT64_C(0);
    ++heap->core.treap_priority_mix_calls;
    heap->core.treap_priority_mix_ns += elapsed_ns;
    uhm_diag_note_max_u64(&heap->core.treap_priority_mix_max_ns, elapsed_ns);
    if (tree_calls) {
        ++*tree_calls;
    }
    if (tree_ns) {
        *tree_ns += elapsed_ns;
    }
    uhm_diag_note_max_u64(tree_max_ns, elapsed_ns);
    return priority;
#endif
}

static uint64_t uhm_live_block_fallback_scan_limit(const uhm_heap *heap) {
    uint64_t nodes;

    if (!heap || heap->core.live_block_tree_current_nodes == 0) {
        nodes = UINT64_C(1);
    } else {
        nodes = heap->core.live_block_tree_current_nodes;
    }
    return uhm_diag_tree_depth_limit(nodes);
}

static uint64_t
uhm_live_block_fallback_epoch_scan_limit(const uhm_heap *heap) {
    uint64_t limit = uhm_live_block_fallback_scan_limit(heap);

    if (limit < (uint64_t)UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS) {
        limit = (uint64_t)UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
    }
    return limit;
}

static uint64_t
uhm_live_block_fallback_scenario_limit(const uhm_heap *heap) {
    uint64_t limit = uhm_live_block_fallback_epoch_scan_limit(heap);

    limit = (limit + UINT64_C(1)) / UINT64_C(2);
    if (limit < (uint64_t)UHM_LIVE_BLOCK_FALLBACK_SCENARIO_MAX_SCANS) {
        limit = (uint64_t)UHM_LIVE_BLOCK_FALLBACK_SCENARIO_MAX_SCANS;
    } else if (limit > (uint64_t)UHM_LIVE_BLOCK_FALLBACK_SCENARIO_MAX_SCANS) {
        limit = (uint64_t)UHM_LIVE_BLOCK_FALLBACK_SCENARIO_MAX_SCANS;
    }
    return limit;
}

static void uhm_live_block_fallback_scenario_reset(uhm_heap *heap,
                                                   uint64_t epoch) {
    size_t i;

    if (!heap) {
        return;
    }
    heap->core.live_block_slow_fallback_scenario_epoch = epoch;
    heap->core.live_block_slow_fallback_scenario_throttled_mask = 0u;
    for (i = 0; i < UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT; ++i) {
        heap->core.live_block_slow_fallback_scenario_calls[i] = 0;
        heap->core.live_block_slow_fallback_scenario_candidates[i] = 0;
    }
}

static void uhm_live_block_fallback_scenario_note_call(uhm_heap *heap,
                                                       size_t callsite) {
    uint64_t epoch;

    if (!heap || heap->core.live_block_tree_mutation_epoch == 0) {
        return;
    }
    if (callsite >= UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT) {
        callsite = UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN;
    }
    epoch = heap->core.live_block_tree_mutation_epoch;
    if (heap->core.live_block_slow_fallback_scenario_epoch != epoch) {
        uhm_live_block_fallback_scenario_reset(heap, epoch);
    }
    ++heap->core.live_block_slow_fallback_scenario_calls[callsite];
}

static int uhm_live_block_fallback_scenario_throttled(uhm_heap *heap,
                                                      size_t callsite) {
    uint64_t epoch;
    uint64_t calls;
    uint64_t candidates;
    uint64_t limit;

    if (!heap || heap->core.live_block_tree_mutation_epoch == 0) {
        return 0;
    }
    if (callsite >= UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT) {
        callsite = UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN;
    }
    epoch = heap->core.live_block_tree_mutation_epoch;
    if (heap->core.live_block_slow_fallback_scenario_epoch != epoch) {
        uhm_live_block_fallback_scenario_reset(heap, epoch);
    }
    uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
        heap, live_block_slow_fallback_scenario_checks));
    if ((heap->core.live_block_slow_fallback_scenario_throttled_mask &
         (uint32_t)(1u << callsite)) != 0u) {
        uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
            heap, live_block_slow_fallback_throttles));
        uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
            heap, live_block_slow_fallback_scenario_throttles));
        return 1;
    }
    calls = heap->core.live_block_slow_fallback_scenario_calls[callsite];
    candidates =
        heap->core.live_block_slow_fallback_scenario_candidates[callsite];
    limit = uhm_live_block_fallback_scenario_limit(heap);
    if (calls != 0 && candidates >= limit && candidates > calls / 2u) {
        heap->core.live_block_slow_fallback_scenario_throttled_mask |=
            (uint32_t)(1u << callsite);
        uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
            heap, live_block_slow_fallback_throttles));
        uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
            heap, live_block_slow_fallback_scenario_throttles));
        return 1;
    }
    ++heap->core.live_block_slow_fallback_scenario_candidates[callsite];
    return 0;
}

static void uhm_diag_note_live_block_fallback_scan(uhm_heap *heap,
                                                   uint64_t scanned,
                                                   int aborted) {
#if UHMAS_ENABLE_DIAGNOSTICS
    if (!heap || scanned == 0) {
        return;
    }
    uhm_diag_note_u64_add(&heap->core.live_block_slow_fallback_scanned_nodes,
                          scanned);
    uhm_diag_note_max_u64(&heap->core.live_block_slow_fallback_max_scanned_nodes,
                          scanned);
    if (scanned > uhm_live_block_fallback_scan_limit(heap)) {
        uhm_diag_note_u64_inc(
            &heap->core.live_block_slow_fallback_budget_exceeded);
    }
    if (aborted) {
        uhm_diag_note_u64_inc(&heap->core.live_block_slow_fallback_aborts);
    }
#else
    (void)heap;
    (void)scanned;
    (void)aborted;
#endif
}

static int uhm_live_block_fallback_epoch_throttled(uhm_heap *heap) {
    uint64_t epoch;
    uint64_t limit;

    if (!heap) {
        return 0;
    }
    epoch = heap->core.live_block_tree_mutation_epoch;
    if (epoch == 0) {
        return 0;
    }
    if (heap->core.live_block_slow_fallback_throttle_epoch != epoch) {
        heap->core.live_block_slow_fallback_throttle_epoch = epoch;
        heap->core.live_block_slow_fallback_epoch_scans = 0;
    }
    limit = uhm_live_block_fallback_epoch_scan_limit(heap);
    if (heap->core.live_block_slow_fallback_epoch_scans >= limit) {
        uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
            heap, live_block_slow_fallback_throttles));
        return 1;
    }
    ++heap->core.live_block_slow_fallback_epoch_scans;
    return 0;
}

static void uhm_live_block_fallback_negative_clear(uhm_heap *heap) {
    size_t i;

    if (!heap) {
        return;
    }
    for (i = 0; i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS; ++i) {
        heap->core.live_block_slow_fallback_negative_payloads[i] = 0;
        heap->core.live_block_slow_fallback_negative_epochs[i] = 0;
    }
    heap->core.live_block_slow_fallback_negative_next = 0;
}

static uint64_t uhm_live_block_negative_cache_key(uint64_t epoch,
                                                  size_t callsite) {
    if (callsite >= UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT) {
        callsite = UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN;
    }
    return (epoch << UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS) |
           (uint64_t)callsite;
}

static void uhm_live_block_tree_mutation_advance(uhm_heap *heap) {
    int epoch_wrapped = 0;

    if (!heap) {
        return;
    }
    if (heap->core.live_block_tree_mutation_epoch >=
        UHM_LIVE_BLOCK_NEGATIVE_CACHE_EPOCH_MAX) {
        heap->core.live_block_tree_mutation_epoch = UINT64_C(1);
        epoch_wrapped = 1;
    } else {
        ++heap->core.live_block_tree_mutation_epoch;
        if (heap->core.live_block_tree_mutation_epoch == 0) {
            heap->core.live_block_tree_mutation_epoch = UINT64_C(1);
            epoch_wrapped = 1;
        }
    }
    /* Epoch-tagged entries are invalidated lazily; only wrap needs a sweep. */
    if (epoch_wrapped) {
        uhm_live_block_fallback_negative_clear(heap);
    }
    heap->core.live_block_slow_fallback_throttle_epoch =
        heap->core.live_block_tree_mutation_epoch;
    heap->core.live_block_slow_fallback_epoch_scans = 0;
    uhm_live_block_fallback_scenario_reset(
        heap, heap->core.live_block_tree_mutation_epoch);
}

static int uhm_live_block_fallback_negative_hit(uhm_heap *heap,
                                                const void *ptr,
                                                size_t callsite) {
    uintptr_t payload = (uintptr_t)ptr;
    uint64_t epoch;
    size_t i;

    if (!heap || !ptr) {
        return 0;
    }
    epoch = heap->core.live_block_tree_mutation_epoch;
    if (epoch == 0) {
        return 0;
    }
    epoch = uhm_live_block_negative_cache_key(epoch, callsite);
    for (i = 0; i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS; ++i) {
        if (heap->core.live_block_slow_fallback_negative_epochs[i] == epoch &&
            heap->core.live_block_slow_fallback_negative_payloads[i] ==
                payload) {
            uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
                heap, live_block_slow_fallback_negative_hits));
            return 1;
        }
    }
    return 0;
}

static void uhm_live_block_fallback_negative_store(uhm_heap *heap,
                                                   const void *ptr,
                                                   size_t callsite) {
    uintptr_t payload = (uintptr_t)ptr;
    uint64_t epoch;
    uint64_t mutation_epoch;
    uint64_t entry_epoch;
    size_t slot;
    size_t i;

    if (!heap || !ptr || heap->core.live_block_tree_mutation_epoch == 0) {
        return;
    }
    if (callsite >= UHM_LIVE_BLOCK_LOOKUP_CALLSITE_COUNT) {
        callsite = UHM_LIVE_BLOCK_LOOKUP_CALLSITE_UNKNOWN;
    }
    mutation_epoch = heap->core.live_block_tree_mutation_epoch;
    epoch = uhm_live_block_negative_cache_key(mutation_epoch, callsite);
    slot = UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
    for (i = 0; i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS; ++i) {
        entry_epoch = heap->core.live_block_slow_fallback_negative_epochs[i];
        if (entry_epoch == epoch &&
            heap->core.live_block_slow_fallback_negative_payloads[i] ==
                payload) {
            return;
        }
        if (slot == UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS &&
            (entry_epoch == 0 ||
             (entry_epoch >> UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_BITS) !=
                 mutation_epoch)) {
            slot = i;
        }
    }
    if (slot == UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS) {
        size_t home = callsite % UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
        slot = UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
        for (i = 0; i < UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS; ++i) {
            size_t candidate =
                (home + i) % UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
            if ((heap->core.live_block_slow_fallback_negative_epochs[
                     candidate] &
                 UHM_LIVE_BLOCK_NEGATIVE_CACHE_CALLSITE_MASK) ==
                (uint64_t)callsite) {
                slot = candidate;
                break;
            }
        }
        if (slot == UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS) {
            slot = heap->core.live_block_slow_fallback_negative_next %
                   UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
        }
        uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
            heap, live_block_slow_fallback_negative_evictions));
    }
    heap->core.live_block_slow_fallback_negative_payloads[slot] = payload;
    heap->core.live_block_slow_fallback_negative_epochs[slot] = epoch;
    heap->core.live_block_slow_fallback_negative_next =
        (slot + 1u) % UHM_LIVE_BLOCK_NEGATIVE_CACHE_SLOTS;
    uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
        heap, live_block_slow_fallback_negative_stores));
}

static uint64_t uhm_malloc_free_tree_priority(const uhm_block *block) {
    return block ? block->free_tree_priority : 0;
}

static void uhm_malloc_free_tree_rotate_left(uhm_heap *heap, uhm_block *node,
                                             uint64_t *rotation_steps) {
    uhm_block *right = node ? node->free_tree_right : 0;
    uhm_block *parent = node ? node->free_tree_parent : 0;

    if (!heap || !node || !right) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_rotations));
    if (rotation_steps) {
        ++*rotation_steps;
    }
    node->free_tree_right = right->free_tree_left;
    if (node->free_tree_right) {
        node->free_tree_right->free_tree_parent = node;
    }
    right->free_tree_left = node;
    right->free_tree_parent = parent;
    if (!parent) {
        heap->core.malloc_free_tree_root = right;
    } else if (parent->free_tree_left == node) {
        parent->free_tree_left = right;
    } else {
        parent->free_tree_right = right;
    }
    node->free_tree_parent = right;
}

static void uhm_malloc_free_tree_rotate_right(uhm_heap *heap, uhm_block *node,
                                              uint64_t *rotation_steps) {
    uhm_block *left = node ? node->free_tree_left : 0;
    uhm_block *parent = node ? node->free_tree_parent : 0;

    if (!heap || !node || !left) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_rotations));
    if (rotation_steps) {
        ++*rotation_steps;
    }
    node->free_tree_left = left->free_tree_right;
    if (node->free_tree_left) {
        node->free_tree_left->free_tree_parent = node;
    }
    left->free_tree_right = node;
    left->free_tree_parent = parent;
    if (!parent) {
        heap->core.malloc_free_tree_root = left;
    } else if (parent->free_tree_left == node) {
        parent->free_tree_left = left;
    } else {
        parent->free_tree_right = left;
    }
    node->free_tree_parent = left;
}

static int uhm_malloc_free_tree_validate(uhm_heap *heap, uhm_block *node,
                                         uhm_block *parent, size_t *count) {
    if (!node) {
        return 1;
    }
    if (node->free_tree_parent != parent ||
        node->magic != UHM_BLOCK_MAGIC ||
        node->free_tree_priority !=
            uhm_block_free_tree_priority_value(heap, node) ||
        (node->flags & UHM_BLOCK_FLAG_FREE) == 0) {
        return 0;
    }
    if (node->free_tree_left &&
        (!uhm_malloc_free_tree_less(node->free_tree_left, node) ||
         uhm_malloc_free_tree_priority(node->free_tree_left) <
             uhm_malloc_free_tree_priority(node))) {
        return 0;
    }
    if (node->free_tree_right &&
        (!uhm_malloc_free_tree_less(node, node->free_tree_right) ||
         uhm_malloc_free_tree_priority(node->free_tree_right) <
             uhm_malloc_free_tree_priority(node))) {
        return 0;
    }
    ++*count;
    return uhm_malloc_free_tree_validate(heap, node->free_tree_left, node,
                                         count) &&
           uhm_malloc_free_tree_validate(heap, node->free_tree_right, node,
                                         count);
}

static void uhm_malloc_free_tree_insert(uhm_heap *heap, uhm_block *block) {
    uhm_block **link = &heap->core.malloc_free_tree_root;
    uhm_block *parent = 0;
    uint64_t depth = 0;
    uint64_t rotation_steps = 0;
    uint64_t inserted_priority;

    block->free_tree_parent = 0;
    block->free_tree_left = 0;
    block->free_tree_right = 0;
    while (*link) {
        parent = *link;
        depth++;
        if (uhm_malloc_free_tree_less(block, parent)) {
            link = &parent->free_tree_left;
        } else {
            link = &parent->free_tree_right;
        }
    }
    block->free_tree_parent = parent;
    *link = block;
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_insertions));
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_max_insert_depth),
        depth);
    if (uhm_diag_note_tree_insert(
            &heap->core.malloc_free_tree_current_nodes,
            &heap->core.malloc_free_tree_max_nodes,
            &heap->core.malloc_free_tree_depth_limit_exceeded, depth)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_MALLOC_DEPTH);
    }
    inserted_priority = uhm_malloc_free_tree_priority(block);
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_note_tree_priority_bucket(
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_priority_bucket_mask),
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_priority_bucket_max),
        heap->core.malloc_free_tree_priority_buckets, inserted_priority);
#endif
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.malloc_free_tree_insert_budget_exceeded,
            heap->core.malloc_free_tree_current_nodes, depth)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_MALLOC_INSERT);
    }
    while (block->free_tree_parent) {
        uint64_t parent_priority =
            uhm_malloc_free_tree_priority(block->free_tree_parent);
        if (inserted_priority == parent_priority) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap,
                                        malloc_free_tree_priority_ties));
        }
        if (inserted_priority >= parent_priority) {
            break;
        }
        if (block->free_tree_parent->free_tree_left == block) {
            uhm_malloc_free_tree_rotate_right(heap, block->free_tree_parent,
                                              &rotation_steps);
        } else {
            uhm_malloc_free_tree_rotate_left(heap, block->free_tree_parent,
                                             &rotation_steps);
        }
    }
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.malloc_free_tree_insert_rotation_budget_exceeded,
            heap->core.malloc_free_tree_current_nodes, rotation_steps)) {
        uhm_diag_mark_treap_health(
            heap, UHM_TREAP_HEALTH_MALLOC_INSERT_ROTATION);
    }
}

static void uhm_malloc_free_tree_replace(uhm_heap *heap, uhm_block *old_node,
                                         uhm_block *new_node) {
    if (!old_node->free_tree_parent) {
        heap->core.malloc_free_tree_root = new_node;
    } else if (old_node->free_tree_parent->free_tree_left == old_node) {
        old_node->free_tree_parent->free_tree_left = new_node;
    } else {
        old_node->free_tree_parent->free_tree_right = new_node;
    }
    if (new_node) {
        new_node->free_tree_parent = old_node->free_tree_parent;
    }
}

static void uhm_malloc_free_tree_remove(uhm_heap *heap, uhm_block *block) {
    uint64_t sink_steps = 0;

    if (!block) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_remove_calls));
    if (block->free_tree_parent == 0 &&
        heap->core.malloc_free_tree_root != block) {
        block->free_tree_left = 0;
        block->free_tree_right = 0;
        return;
    }
    while (block->free_tree_left && block->free_tree_right) {
        sink_steps++;
        if (uhm_malloc_free_tree_priority(block->free_tree_left) <
            uhm_malloc_free_tree_priority(block->free_tree_right)) {
            uhm_malloc_free_tree_rotate_right(heap, block, 0);
        } else {
            uhm_malloc_free_tree_rotate_left(heap, block, 0);
        }
    }
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_remove_sink_steps),
        sink_steps);
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap,
                                malloc_free_tree_max_remove_sink_steps),
        sink_steps);
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.malloc_free_tree_remove_budget_exceeded,
            heap->core.malloc_free_tree_current_nodes, sink_steps)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_MALLOC_REMOVE);
    }
    uhm_malloc_free_tree_replace(heap, block,
                                 block->free_tree_left
                                     ? block->free_tree_left
                                     : block->free_tree_right);
    uhm_diag_note_tree_remove(&heap->core.malloc_free_tree_current_nodes);
    block->free_tree_parent = 0;
    block->free_tree_left = 0;
    block->free_tree_right = 0;
}

static uhm_block *uhm_malloc_free_tree_find_best(uhm_heap *heap,
                                                 uhm_block *node,
                                                 size_t wanted) {
    uhm_block *best = 0;
    uint64_t steps = 0;
    uint64_t hard_limit =
        uhm_diag_tree_find_hard_limit(
            heap ? heap->core.malloc_free_tree_current_nodes : 0);

    while (node) {
        steps++;
        if (steps > hard_limit) {
            if (heap) {
                uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
                    heap, malloc_free_tree_find_calls));
                uhm_diag_note_u64_add(UHM_DIAG_CORE_FIELD_PTR(
                                          heap, malloc_free_tree_find_steps),
                                      steps);
                uhm_diag_note_u64_inc(
                    &heap->core.malloc_free_tree_find_budget_exceeded);
                uhm_diag_mark_treap_health(heap,
                                           UHM_TREAP_HEALTH_MALLOC_FIND);
                uhm_diag_note_max_u64(UHM_DIAG_CORE_FIELD_PTR(
                                          heap,
                                          malloc_free_tree_max_find_steps),
                                      steps);
            }
            return 0;
        }
        if (node->total_size >= wanted) {
            best = node;
            node = node->free_tree_left;
        } else {
            node = node->free_tree_right;
        }
    }
    if (heap) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_find_calls));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_find_steps),
            steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, malloc_free_tree_max_find_steps),
            steps);
        if (uhm_diag_note_tree_operation_budget(
                &heap->core.malloc_free_tree_find_budget_exceeded,
                heap->core.malloc_free_tree_current_nodes, steps)) {
            uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_MALLOC_FIND);
        }
    }
    return best;
}

static int uhm_live_block_tree_less(const uhm_block *a, const uhm_block *b) {
    return (uintptr_t)uhm_block_payload((uhm_block *)a) <
           (uintptr_t)uhm_block_payload((uhm_block *)b);
}

static uint64_t uhm_live_block_tree_priority(const uhm_block *block) {
    return block ? block->live_tree_priority : 0;
}

static void uhm_live_block_tree_rotate_left(uhm_heap *heap, uhm_block *node,
                                            uint64_t *rotation_steps) {
    uhm_block *right = node ? node->live_tree_right : 0;
    uhm_block *parent = node ? node->live_tree_parent : 0;

    if (!heap || !node || !right) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_rotations));
    if (rotation_steps) {
        ++*rotation_steps;
    }
    node->live_tree_right = right->live_tree_left;
    if (node->live_tree_right) {
        node->live_tree_right->live_tree_parent = node;
    }
    right->live_tree_left = node;
    right->live_tree_parent = parent;
    if (!parent) {
        heap->core.live_block_tree_root = right;
    } else if (parent->live_tree_left == node) {
        parent->live_tree_left = right;
    } else {
        parent->live_tree_right = right;
    }
    node->live_tree_parent = right;
}

static void uhm_live_block_tree_rotate_right(uhm_heap *heap, uhm_block *node,
                                             uint64_t *rotation_steps) {
    uhm_block *left = node ? node->live_tree_left : 0;
    uhm_block *parent = node ? node->live_tree_parent : 0;

    if (!heap || !node || !left) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_rotations));
    if (rotation_steps) {
        ++*rotation_steps;
    }
    node->live_tree_left = left->live_tree_right;
    if (node->live_tree_left) {
        node->live_tree_left->live_tree_parent = node;
    }
    left->live_tree_right = node;
    left->live_tree_parent = parent;
    if (!parent) {
        heap->core.live_block_tree_root = left;
    } else if (parent->live_tree_left == node) {
        parent->live_tree_left = left;
    } else {
        parent->live_tree_right = left;
    }
    node->live_tree_parent = left;
}

static void uhm_live_block_tree_insert(uhm_heap *heap, uhm_block *block) {
    uhm_block **link;
    uhm_block *parent = 0;
    uint64_t depth = 0;
    uint64_t rotation_steps = 0;
    uint64_t inserted_priority;

    if (!heap || !block) {
        return;
    }
    uhm_block_live_tree_priority_ensure(heap, block);
    link = &heap->core.live_block_tree_root;
    block->live_tree_parent = 0;
    block->live_tree_left = 0;
    block->live_tree_right = 0;
    while (*link) {
        parent = *link;
        depth++;
        if (uhm_live_block_tree_less(block, parent)) {
            link = &parent->live_tree_left;
        } else {
            link = &parent->live_tree_right;
        }
    }
    block->live_tree_parent = parent;
    *link = block;
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_insertions));
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_max_insert_depth),
        depth);
    if (uhm_diag_note_tree_insert(
            &heap->core.live_block_tree_current_nodes,
            &heap->core.live_block_tree_max_nodes,
            &heap->core.live_block_tree_depth_limit_exceeded, depth)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_LIVE_DEPTH);
    }
    inserted_priority = uhm_live_block_tree_priority(block);
#if UHMAS_ENABLE_DIAGNOSTICS
    uhm_diag_note_tree_priority_bucket(
        &heap->core.live_block_tree_priority_bucket_mask,
        &heap->core.live_block_tree_priority_bucket_max,
        heap->core.live_block_tree_priority_buckets, inserted_priority);
#endif
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.live_block_tree_insert_budget_exceeded,
            heap->core.live_block_tree_current_nodes, depth)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_LIVE_INSERT);
    }
    while (block->live_tree_parent) {
        uint64_t parent_priority =
            uhm_live_block_tree_priority(block->live_tree_parent);
        if (inserted_priority == parent_priority) {
            uhm_diag_note_u64_inc(
                UHM_DIAG_CORE_FIELD_PTR(heap,
                                        live_block_tree_priority_ties));
        }
        if (inserted_priority >= parent_priority) {
            break;
        }
        if (block->live_tree_parent->live_tree_left == block) {
            uhm_live_block_tree_rotate_right(heap, block->live_tree_parent,
                                             &rotation_steps);
        } else {
            uhm_live_block_tree_rotate_left(heap, block->live_tree_parent,
                                            &rotation_steps);
        }
    }
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.live_block_tree_insert_rotation_budget_exceeded,
            heap->core.live_block_tree_current_nodes, rotation_steps)) {
        uhm_diag_mark_treap_health(heap,
                                   UHM_TREAP_HEALTH_LIVE_INSERT_ROTATION);
    }
    uhm_live_block_tree_mutation_advance(heap);
}

static void uhm_live_block_tree_replace(uhm_heap *heap, uhm_block *old_node,
                                        uhm_block *new_node) {
    if (!old_node->live_tree_parent) {
        heap->core.live_block_tree_root = new_node;
    } else if (old_node->live_tree_parent->live_tree_left == old_node) {
        old_node->live_tree_parent->live_tree_left = new_node;
    } else {
        old_node->live_tree_parent->live_tree_right = new_node;
    }
    if (new_node) {
        new_node->live_tree_parent = old_node->live_tree_parent;
    }
}

static void uhm_live_block_tree_remove(uhm_heap *heap, uhm_block *block) {
    uint64_t sink_steps = 0;

    if (!heap || !block) {
        return;
    }
    uhm_diag_note_u64_inc(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_remove_calls));
    if (block->live_tree_parent == 0 &&
        heap->core.live_block_tree_root != block) {
        block->live_tree_left = 0;
        block->live_tree_right = 0;
        return;
    }
    while (block->live_tree_left && block->live_tree_right) {
        sink_steps++;
        if (uhm_live_block_tree_priority(block->live_tree_left) <
            uhm_live_block_tree_priority(block->live_tree_right)) {
            uhm_live_block_tree_rotate_right(heap, block, 0);
        } else {
            uhm_live_block_tree_rotate_left(heap, block, 0);
        }
    }
    uhm_diag_note_u64_add(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_remove_sink_steps),
        sink_steps);
    uhm_diag_note_max_u64(
        UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_max_remove_sink_steps),
        sink_steps);
    if (uhm_diag_note_tree_operation_budget(
            &heap->core.live_block_tree_remove_budget_exceeded,
            heap->core.live_block_tree_current_nodes, sink_steps)) {
        uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_LIVE_REMOVE);
    }
    uhm_live_block_tree_replace(heap, block,
                                block->live_tree_left
                                    ? block->live_tree_left
                                    : block->live_tree_right);
    uhm_diag_note_tree_remove(&heap->core.live_block_tree_current_nodes);
    block->live_tree_parent = 0;
    block->live_tree_left = 0;
    block->live_tree_right = 0;
    uhm_live_block_tree_mutation_advance(heap);
}

static uhm_block *uhm_live_block_tree_find_by_payload(uhm_heap *heap,
                                                      const void *ptr,
                                                      uint64_t *steps_out) {
    uintptr_t payload = (uintptr_t)ptr;
    uhm_block *node = heap ? heap->core.live_block_tree_root : 0;
    uint64_t steps = 0;
    uint64_t hard_limit =
        uhm_diag_tree_find_hard_limit(
            heap ? heap->core.live_block_tree_current_nodes : 0);

    while (node) {
        uintptr_t node_payload = (uintptr_t)uhm_block_payload(node);
        steps++;
        if (steps > hard_limit) {
            if (heap) {
                uhm_diag_note_u64_inc(UHM_DIAG_CORE_FIELD_PTR(
                    heap, live_block_tree_find_calls));
                uhm_diag_note_u64_add(UHM_DIAG_CORE_FIELD_PTR(
                                          heap, live_block_tree_find_steps),
                                      steps);
                uhm_diag_note_u64_inc(
                    &heap->core.live_block_tree_find_budget_exceeded);
                uhm_diag_mark_treap_health(heap,
                                           UHM_TREAP_HEALTH_LIVE_FIND);
                uhm_diag_note_max_u64(
                    UHM_DIAG_CORE_FIELD_PTR(
                        heap, live_block_tree_max_find_steps),
                    steps);
            }
            if (steps_out) {
                *steps_out = steps;
            }
            return 0;
        }
        if (payload == node_payload) {
            break;
        }
        node = payload < node_payload ? node->live_tree_left
                                      : node->live_tree_right;
    }
    if (heap) {
        uhm_diag_note_u64_inc(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_find_calls));
        uhm_diag_note_u64_add(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_find_steps),
            steps);
        uhm_diag_note_max_u64(
            UHM_DIAG_CORE_FIELD_PTR(heap, live_block_tree_max_find_steps),
            steps);
        if (uhm_diag_note_tree_operation_budget(
                &heap->core.live_block_tree_find_budget_exceeded,
                heap->core.live_block_tree_current_nodes, steps)) {
            uhm_diag_mark_treap_health(heap, UHM_TREAP_HEALTH_LIVE_FIND);
        }
    }
    if (steps_out) {
        *steps_out = steps;
    }
    if (!node || node->magic != UHM_BLOCK_MAGIC ||
        (node->flags & UHM_BLOCK_FLAG_FREE) != 0 ||
        uhm_block_payload(node) != ptr) {
        return 0;
    }
    return node;
}

static int uhm_live_block_tree_validate(uhm_heap *heap, uhm_block *node,
                                        uhm_block *parent, size_t *count) {
    if (!node) {
        return 1;
    }
    if (node->live_tree_parent != parent ||
        node->magic != UHM_BLOCK_MAGIC ||
        node->live_tree_priority !=
            uhm_block_live_tree_priority_value(heap, node) ||
        (node->flags & UHM_BLOCK_FLAG_FREE) != 0) {
        return 0;
    }
    if (node->live_tree_left &&
        (!uhm_live_block_tree_less(node->live_tree_left, node) ||
         uhm_live_block_tree_priority(node->live_tree_left) <
             uhm_live_block_tree_priority(node))) {
        return 0;
    }
    if (node->live_tree_right &&
        (!uhm_live_block_tree_less(node, node->live_tree_right) ||
         uhm_live_block_tree_priority(node->live_tree_right) <
             uhm_live_block_tree_priority(node))) {
        return 0;
    }
    ++*count;
    return uhm_live_block_tree_validate(heap, node->live_tree_left, node,
                                        count) &&
           uhm_live_block_tree_validate(heap, node->live_tree_right, node,
                                        count);
}

static size_t uhm_malloc_trees_rebuild_locked(uhm_heap *heap) {
    uhm_block *block;
    size_t nodes = 0;

    if (!heap) {
        return 0;
    }
    heap->core.malloc_free_tree_root = 0;
    heap->core.live_block_tree_root = 0;
    heap->core.malloc_free_tree_current_nodes = 0;
    heap->core.live_block_tree_current_nodes = 0;
    for (block = heap->core.first_block; block; block = block->next_phys) {
        if (block->magic != UHM_BLOCK_MAGIC) {
            continue;
        }
        block->free_tree_parent = 0;
        block->free_tree_left = 0;
        block->free_tree_right = 0;
        block->live_tree_parent = 0;
        block->live_tree_left = 0;
        block->live_tree_right = 0;
        uhm_block_tree_priority_init(heap, block);
    }
    for (block = heap->core.first_block; block; block = block->next_phys) {
        if (block->magic != UHM_BLOCK_MAGIC) {
            continue;
        }
        if ((block->flags & UHM_BLOCK_FLAG_FREE) != 0 &&
            (block->flags & UHM_BLOCK_FLAG_VM_BUSY) == 0) {
            uhm_malloc_free_tree_insert(heap, block);
            ++nodes;
        } else if (block->requested_size != 0) {
            uhm_live_block_tree_insert(heap, block);
            ++nodes;
        }
    }
    return nodes;
}

static void uhm_free_list_remove(uhm_heap *heap, uhm_block *block) {
    uhm_malloc_free_tree_remove(heap, block);
    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        heap->core.malloc_free_head = block->next_free;
    }
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    block->prev_free = 0;
    block->next_free = 0;
    block->flags &= ~UHM_BLOCK_FLAG_FREE;
}

static void uhm_free_list_insert(uhm_heap *heap, uhm_block *block) {
    uhm_block_free_tree_priority_ensure(heap, block);
    block->flags |= UHM_BLOCK_FLAG_FREE;
    block->prev_free = 0;
    block->next_free = heap->core.malloc_free_head;
    if (heap->core.malloc_free_head) {
        heap->core.malloc_free_head->prev_free = block;
    }
    heap->core.malloc_free_head = block;
    uhm_malloc_free_tree_insert(heap, block);
}

static void uhm_block_descriptor_set(uhm_block *block) {
    if (!block) {
        return;
    }
    uhm_extent_descriptor_set(&block->desc, UHM_EXTENT_KIND_MALLOC,
                              (uintptr_t)block, block->total_size,
                              UHM_PAGE_OWNER_MALLOC,
                              UHM_PAGE_VA_MALLOC_LIVE,
                              UHM_PAGE_COMMIT_LIVE,
                              UHM_PROT_READ | UHM_PROT_WRITE,
                              block->generation, block);
}

