static void uhm_local_cache_repair_head_after_unlink(
    uhm_slab_slot **cache_head,
    uhm_slab_slot *const *heads,
    uhm_slab_slot *removed,
    uint16_t size_class) {
    size_t i;

    if (!cache_head || !heads || !removed || *cache_head != removed) {
        return;
    }
    if (size_class < UHM_SIZE_CLASS_COUNT && heads[size_class]) {
        *cache_head = heads[size_class];
        return;
    }
    *cache_head = 0;
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        if (heads[i]) {
            *cache_head = heads[i];
            return;
        }
    }
}

static size_t uhm_cached_requested_bytes_load(
    const _Atomic(size_t) *total) {
    return total ? atomic_load_explicit(total, UHM_ATOMIC_ACQUIRE) : 0u;
}

static int uhm_cached_requested_bytes_can_add(
    const _Atomic(size_t) *total,
    size_t value) {
    size_t current = uhm_cached_requested_bytes_load(total);
    return total && value != 0 && current <= SIZE_MAX - value;
}

static int uhm_cached_requested_bytes_can_sub(
    const _Atomic(size_t) *total,
    size_t value) {
    size_t current = uhm_cached_requested_bytes_load(total);
    return total && value != 0 && current >= value;
}

static void uhm_cached_requested_bytes_add_unchecked(
    _Atomic(size_t) *total,
    size_t value) {
    size_t current = uhm_cached_requested_bytes_load(total);
    atomic_store_explicit(total, current + value, UHM_ATOMIC_RELEASE);
}

static void uhm_cached_requested_bytes_sub_unchecked(
    _Atomic(size_t) *total,
    size_t value) {
    size_t current = uhm_cached_requested_bytes_load(total);
    atomic_store_explicit(total, current - value, UHM_ATOMIC_RELEASE);
}

static uhm_slab_slot **uhm_ctx_cache_bucket(uhm_thread_ctx *ctx,
                                            uint16_t size_class) {
    if (!ctx || size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    return &ctx->local_cache_heads[size_class];
}

static int uhm_cache_size_class_uses_slab(uint16_t size_class) {
    return size_class < UHM_SIZE_CLASS_COUNT &&
           uhm_size_class_uses_slab(size_class, uhm_size_classes[size_class]);
}

static int uhm_local_page_hint_set_base(uhm_heap *heap,
                                        uintptr_t address,
                                        size_t *out_base);

static size_t uhm_local_cache_slab_pages(uhm_heap *heap,
                                         const uhm_slab *slab) {
    size_t page_size;

    if (!heap || !slab || slab->size == 0) {
        return 0;
    }
    page_size = heap->F.vm.page_size;
    if (page_size == 0 || slab->size > SIZE_MAX - (page_size - 1u)) {
        return 0;
    }
    return (slab->size + page_size - 1u) / page_size;
}

static size_t uhm_local_cache_page_limit_for_bytes(uhm_heap *heap,
                                                   size_t byte_limit) {
    size_t page_size;

    if (!heap || byte_limit == 0) {
        return 0;
    }
    page_size = heap->F.vm.page_size;
    if (page_size == 0) {
        return 0;
    }
    return byte_limit / page_size + (byte_limit % page_size != 0u);
}

static size_t uhm_local_cache_retained_page_limit(size_t byte_pages) {
    if (byte_pages > SIZE_MAX / UHM_LOCAL_RETAINED_PAGE_FACTOR) {
        return SIZE_MAX;
    }
    return byte_pages * UHM_LOCAL_RETAINED_PAGE_FACTOR;
}

static int uhm_local_cache_bucket_has_slab(uhm_slab_slot *head,
                                           const uhm_slab *slab) {
    for (; head; head = head->next_free) {
        if (uhm_slab_slot_owner(head) == slab) {
            return 1;
        }
    }
    return 0;
}

static size_t uhm_local_cache_bucket_slab_count(uhm_slab_slot *head,
                                                const uhm_slab *slab) {
    size_t count = 0;

    for (; head; head = head->next_free) {
        if (uhm_slab_slot_owner(head) == slab) {
            ++count;
        }
    }
    return count;
}

#define UHM_LOCAL_RETAIN_PREPARED_FLAG (SIZE_MAX ^ (SIZE_MAX >> 1u))
#define UHM_LOCAL_RETAIN_NO_HINT_FLAG \
    (UHM_LOCAL_RETAIN_PREPARED_FLAG >> 1u)
#define UHM_LOCAL_RETAIN_PREPARED_COUNT_MASK \
    (UHM_LOCAL_RETAIN_NO_HINT_FLAG - 1u)

static size_t uhm_local_retained_hint_set_base(const uhm_slab *slab) {
    uintptr_t key;

    if (!slab) {
        return UHM_LOCAL_RETAINED_HINT_COUNT;
    }
    key = (uintptr_t)slab / sizeof(*slab);
    key ^= key >> 7u;
    key ^= (uintptr_t)slab->size_class;
    return (size_t)(key & (UHM_LOCAL_RETAINED_HINT_SET_COUNT - 1u)) *
           UHM_LOCAL_RETAINED_HINT_WAYS;
}

static size_t uhm_local_retained_hint_find(
    const uhm_local_retained_hint *hints,
    const uhm_slab *slab,
    size_t *out_empty_index) {
    size_t base;
    size_t way;

    if (out_empty_index) {
        *out_empty_index = UHM_LOCAL_RETAINED_HINT_COUNT;
    }
    if (!hints || !slab) {
        return UHM_LOCAL_RETAINED_HINT_COUNT;
    }
    base = uhm_local_retained_hint_set_base(slab);
    if (base >= UHM_LOCAL_RETAINED_HINT_COUNT) {
        return UHM_LOCAL_RETAINED_HINT_COUNT;
    }
    for (way = 0; way < UHM_LOCAL_RETAINED_HINT_WAYS; ++way) {
        size_t index = base + way;
        if (hints[index].slab == slab &&
            hints[index].slot_count != 0u) {
            return index;
        }
        if (out_empty_index &&
            *out_empty_index == UHM_LOCAL_RETAINED_HINT_COUNT &&
            !hints[index].slab && hints[index].slot_count == 0u) {
            *out_empty_index = index;
        }
    }
    return UHM_LOCAL_RETAINED_HINT_COUNT;
}

static void uhm_local_retained_hint_remove(
    uhm_local_retained_hint *hints,
    size_t index) {
    size_t base;
    size_t last;

    if (!hints || index >= UHM_LOCAL_RETAINED_HINT_COUNT) {
        return;
    }
    base = index & ~(UHM_LOCAL_RETAINED_HINT_WAYS - 1u);
    last = base + UHM_LOCAL_RETAINED_HINT_WAYS - 1u;
    while (last > index && !hints[last].slab &&
           hints[last].slot_count == 0u) {
        --last;
    }
    if (last > index) {
        hints[index] = hints[last];
    }
    hints[last].slab = 0;
    hints[last].slot_count = 0u;
}

static int uhm_local_cache_can_retain_slab(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    uhm_local_retained_hint *hints,
    size_t retained_pages,
    size_t page_limit,
    const uhm_slab *slab,
    size_t *out_retain_token) {
#if UHMAS_LOCAL_RETAINED_PREPARED_PROOF
    size_t empty_hint_index = UHM_LOCAL_RETAINED_HINT_COUNT;
#endif
    size_t hint_index;
    size_t slab_pages;

    if (out_retain_token) {
        *out_retain_token = SIZE_MAX;
    }
    if (!heap || !heads || !hints || !slab ||
        slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    if (retained_pages == 0u) {
        slab_pages = uhm_local_cache_slab_pages(heap, slab);
        return slab_pages != 0 && slab_pages <= page_limit;
    }
#if UHMAS_LOCAL_RETAINED_PREPARED_PROOF
    hint_index = uhm_local_retained_hint_find(
        hints, slab, &empty_hint_index);
#else
    hint_index = uhm_local_retained_hint_find(hints, slab, 0);
#endif
    if (hint_index < UHM_LOCAL_RETAINED_HINT_COUNT) {
        if (out_retain_token) {
            *out_retain_token = hint_index;
        }
        return 1;
    }
#if UHMAS_LOCAL_RETAINED_PREPARED_PROOF
    if (empty_hint_index >= UHM_LOCAL_RETAINED_HINT_COUNT) {
        int already_retained = uhm_local_cache_bucket_has_slab(
            heads[slab->size_class], slab);
        if (out_retain_token) {
            *out_retain_token = UHM_LOCAL_RETAIN_PREPARED_FLAG |
                                UHM_LOCAL_RETAIN_NO_HINT_FLAG |
                                (already_retained ? 1u : 0u);
        }
        if (already_retained) {
            return 1;
        }
    } else {
        size_t existing_slot_count = uhm_local_cache_bucket_slab_count(
            heads[slab->size_class], slab);
        if (out_retain_token &&
            existing_slot_count <= UHM_LOCAL_RETAIN_PREPARED_COUNT_MASK) {
            *out_retain_token = UHM_LOCAL_RETAIN_PREPARED_FLAG |
                                existing_slot_count;
        }
        if (existing_slot_count != 0u) {
            return 1;
        }
    }
#else
    if (uhm_local_cache_bucket_has_slab(heads[slab->size_class], slab)) {
        return 1;
    }
#endif
    slab_pages = uhm_local_cache_slab_pages(heap, slab);
    return slab_pages != 0 && retained_pages <= page_limit &&
           slab_pages <= page_limit - retained_pages;
}

static void uhm_local_cache_note_slab_push(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    uhm_local_retained_hint *hints,
    size_t *retained_pages,
    const uhm_slab *slab,
    size_t retain_token) {
    uhm_local_retained_hint *empty_hint = 0;
    uhm_slab_slot *cursor;
    size_t existing_count = 0;
    size_t empty_hint_index;
    size_t hint_index;
    size_t set_base;

    if (!heap || !heads || !hints || !retained_pages || !slab ||
        slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return;
    }
    set_base = uhm_local_retained_hint_set_base(slab);
    if (set_base >= UHM_LOCAL_RETAINED_HINT_COUNT) {
        return;
    }
    if (retain_token >= set_base &&
        retain_token < set_base + UHM_LOCAL_RETAINED_HINT_WAYS &&
        hints[retain_token].slab == slab &&
        hints[retain_token].slot_count != 0u) {
        ++hints[retain_token].slot_count;
        return;
    }
    if (*retained_pages == 0u) {
        size_t slab_pages = uhm_local_cache_slab_pages(heap, slab);
        if (slab_pages != 0u) {
            hints[set_base].slab = (uhm_slab *)slab;
            hints[set_base].slot_count = 1u;
            *retained_pages = slab_pages;
        }
        return;
    }
#if UHMAS_LOCAL_RETAINED_PREPARED_PROOF
    if (retain_token != SIZE_MAX &&
        (retain_token & UHM_LOCAL_RETAIN_PREPARED_FLAG) != 0u) {
        int no_hint_available =
            (retain_token & UHM_LOCAL_RETAIN_NO_HINT_FLAG) != 0u;
        size_t prepared_count =
            retain_token & UHM_LOCAL_RETAIN_PREPARED_COUNT_MASK;

        if (no_hint_available) {
            if (prepared_count != 0u) {
                return;
            }
            *retained_pages += uhm_local_cache_slab_pages(heap, slab);
            return;
        }
        hint_index = uhm_local_retained_hint_find(
            hints, slab, &empty_hint_index);
        if (hint_index < UHM_LOCAL_RETAINED_HINT_COUNT) {
            ++hints[hint_index].slot_count;
            return;
        }
        if (empty_hint_index < UHM_LOCAL_RETAINED_HINT_COUNT) {
            empty_hint = &hints[empty_hint_index];
        }
        if (prepared_count != 0u) {
            if (empty_hint) {
                empty_hint->slab = (uhm_slab *)slab;
                empty_hint->slot_count = prepared_count + 1u;
            }
            return;
        }
        *retained_pages += uhm_local_cache_slab_pages(heap, slab);
        if (empty_hint) {
            empty_hint->slab = (uhm_slab *)slab;
            empty_hint->slot_count = 1u;
        }
        return;
    }
#endif
    hint_index = uhm_local_retained_hint_find(
        hints, slab, &empty_hint_index);
    if (hint_index < UHM_LOCAL_RETAINED_HINT_COUNT) {
        ++hints[hint_index].slot_count;
        return;
    }
    if (empty_hint_index < UHM_LOCAL_RETAINED_HINT_COUNT) {
        empty_hint = &hints[empty_hint_index];
    }
    for (cursor = heads[slab->size_class]; cursor;
         cursor = cursor->next_free) {
        if (uhm_slab_slot_owner(cursor) == slab) {
            ++existing_count;
        }
    }
    if (existing_count != 0u) {
        if (empty_hint) {
            empty_hint->slab = (uhm_slab *)slab;
            empty_hint->slot_count = existing_count + 1u;
        }
        return;
    }
    *retained_pages += uhm_local_cache_slab_pages(heap, slab);
    if (empty_hint) {
        empty_hint->slab = (uhm_slab *)slab;
        empty_hint->slot_count = 1u;
    }
}

#if UHMAS_ENABLE_TEST_HOOKS
void uhm_test_local_cache_note_slab_push(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    uhm_local_retained_hint *hints,
    size_t *retained_pages,
    const uhm_slab *slab,
    size_t known_hint_index) {
    uhm_local_cache_note_slab_push(heap, heads, hints, retained_pages, slab,
                                   known_hint_index);
}

int uhm_test_local_cache_prepare_and_note_slab_push(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    uhm_local_retained_hint *hints,
    size_t *retained_pages,
    size_t page_limit,
    const uhm_slab *slab,
    size_t *out_existing_slot_count) {
    size_t retain_token = SIZE_MAX;

    if (out_existing_slot_count) {
        *out_existing_slot_count = SIZE_MAX;
    }
    if (!uhm_local_cache_can_retain_slab(
            heap, heads, hints, retained_pages ? *retained_pages : 0u,
            page_limit, slab, &retain_token)) {
        return 0;
    }
    if (out_existing_slot_count) {
        *out_existing_slot_count =
            retain_token != SIZE_MAX &&
                    (retain_token & UHM_LOCAL_RETAIN_PREPARED_FLAG) != 0u
                ? ((retain_token & UHM_LOCAL_RETAIN_NO_HINT_FLAG) != 0u
                       ? ((retain_token &
                           UHM_LOCAL_RETAIN_PREPARED_COUNT_MASK) != 0u
                              ? 1u
                              : 0u)
                       : retain_token &
                             UHM_LOCAL_RETAIN_PREPARED_COUNT_MASK)
                : SIZE_MAX;
    }
    uhm_local_cache_note_slab_push(heap, heads, hints, retained_pages, slab,
                                   retain_token);
    return 1;
}

size_t uhm_test_local_retained_hint_set_base(const uhm_slab *slab) {
    return uhm_local_retained_hint_set_base(slab);
}
#endif

static int uhm_local_cache_note_slab_unlink(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    uhm_local_retained_hint *hints,
    size_t *retained_pages,
    const uhm_slab *slab) {
    size_t hint_index;
    size_t slab_pages;

    if (!heap || !heads || !hints || !retained_pages || !slab ||
        slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return -1;
    }
    hint_index = uhm_local_retained_hint_find(hints, slab, 0);
    if (hint_index < UHM_LOCAL_RETAINED_HINT_COUNT) {
        --hints[hint_index].slot_count;
        if (hints[hint_index].slot_count != 0u) {
            return 1;
        }
        uhm_local_retained_hint_remove(hints, hint_index);
    }
    if (hint_index == UHM_LOCAL_RETAINED_HINT_COUNT &&
        uhm_local_cache_bucket_has_slab(heads[slab->size_class], slab)) {
        return 1;
    }
    slab_pages = uhm_local_cache_slab_pages(heap, slab);
    *retained_pages = *retained_pages >= slab_pages
                          ? *retained_pages - slab_pages
                          : 0u;
    return 0;
}

static int uhm_local_cache_retained_pages_calculate(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    size_t *out_pages) {
    size_t pages = 0;
    size_t i;

    if (out_pages) {
        *out_pages = 0;
    }
    if (!heap || !heads || !out_pages) {
        return 0;
    }
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        uhm_slab_slot *slot;
        for (slot = heads[i]; slot; slot = slot->next_free) {
            uhm_slab_slot *prior;
            uhm_slab *slab = uhm_slab_slot_owner(slot);
            size_t slab_pages;
            int seen = 0;
            for (prior = heads[i]; prior != slot; prior = prior->next_free) {
                if (!prior) {
                    return 0;
                }
                if (uhm_slab_slot_owner(prior) == slab) {
                    seen = 1;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            slab_pages = uhm_local_cache_slab_pages(heap, slab);
            if (slab_pages == 0 || pages > SIZE_MAX - slab_pages) {
                return 0;
            }
            pages += slab_pages;
        }
    }
    *out_pages = pages;
    return 1;
}

static int uhm_local_cache_retained_hints_validate(
    uhm_heap *heap,
    uhm_slab_slot *const *heads,
    const uhm_local_retained_hint *hints) {
    size_t set;

    if (!heap || !heads || !hints) {
        return 0;
    }
    for (set = 0; set < UHM_LOCAL_RETAINED_HINT_SET_COUNT; ++set) {
        size_t base = set * UHM_LOCAL_RETAINED_HINT_WAYS;
        size_t way;
        int saw_empty = 0;
        for (way = 0; way < UHM_LOCAL_RETAINED_HINT_WAYS; ++way) {
            size_t index = base + way;
            const uhm_slab *slab = hints[index].slab;
            uhm_slab_slot *slot;
            size_t prior_way;
            size_t count = 0;
            if (!slab) {
                if (hints[index].slot_count != 0u) {
                    return 0;
                }
                saw_empty = 1;
                continue;
            }
            if (saw_empty || hints[index].slot_count == 0u ||
                slab->size_class >= UHM_SIZE_CLASS_COUNT ||
                uhm_local_retained_hint_set_base(slab) != base) {
                return 0;
            }
            for (prior_way = 0; prior_way < way; ++prior_way) {
                if (hints[base + prior_way].slab == slab) {
                    return 0;
                }
            }
            for (slot = heads[slab->size_class]; slot;
                 slot = slot->next_free) {
                if (uhm_slab_slot_owner(slot) == slab) {
                    ++count;
                }
            }
            if (count != hints[index].slot_count) {
                return 0;
            }
        }
    }
    return 1;
}

static uint32_t uhm_local_cache_nonempty_mask_calculate(
    uhm_slab_slot *const *heads) {
    uint32_t mask = 0u;
    uint16_t size_class;

    if (!heads) {
        return 0u;
    }
    for (size_class = 0u; size_class < UHM_SIZE_CLASS_COUNT;
         ++size_class) {
        if (heads[size_class]) {
            mask |= UINT32_C(1) << size_class;
        }
    }
    return mask;
}

static unsigned uhm_uint32_trailing_zero_count(uint32_t value) {
#if defined(_MSC_VER)
    unsigned long index = 0;
    (void)_BitScanForward(&index, (unsigned long)value);
    return (unsigned)index;
#elif defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctz(value);
#else
    unsigned index = 0;
    while ((value & UINT32_C(1)) == 0u) {
        value >>= 1u;
        ++index;
    }
    return index;
#endif
}

static int uhm_local_cache_find_fit_class(
    uint32_t nonempty_class_mask,
    uint16_t minimum_size_class,
    uint16_t *out_size_class) {
    uint32_t eligible;
    unsigned offset;

    if (out_size_class) {
        *out_size_class = UHM_SIZE_CLASS_COUNT;
    }
    if (minimum_size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    nonempty_class_mask &=
        (UINT32_C(1) << UHM_SIZE_CLASS_COUNT) - UINT32_C(1);
    eligible = nonempty_class_mask >> minimum_size_class;
    if (eligible == 0u) {
        return 0;
    }
    offset = uhm_uint32_trailing_zero_count(eligible);
    if (out_size_class) {
        *out_size_class = (uint16_t)(minimum_size_class + offset);
    }
    return 1;
}

#if UHMAS_ENABLE_TEST_HOOKS
int uhm_test_local_cache_find_fit_class(
    uint32_t nonempty_class_mask,
    uint16_t minimum_size_class,
    uint16_t *out_size_class) {
    return uhm_local_cache_find_fit_class(
        nonempty_class_mask, minimum_size_class, out_size_class);
}
#endif

static int uhm_local_cache_cached_slot_owner(
    uhm_heap *heap,
    uhm_slab_slot *slot,
    uint16_t size_class,
    size_t payload_size,
    uhm_slab **out_slab) {
    uintptr_t slab_addr;
    uintptr_t slab_end;
    uhm_slab *slab;

    if (out_slab) {
        *out_slab = 0;
    }
    if (!heap || !slot) {
        return 0;
    }
    slab = uhm_slab_slot_owner(slot);
    slab_addr = (uintptr_t)slab;
    if ((slab_addr % alignof(uhm_slab)) != 0u ||
        slab_addr < heap->core.arena_base ||
        uhm_range_overflows(slab_addr, sizeof(*slab), &slab_end) ||
        slab_end > heap->core.arena_end) {
        return 0;
    }
    /* Cached slots remain reserved live capacity at the slab level. The
       atomic cache count pins their descriptor/page until pop or flush, while
       live/free/state only change when flush returns a reservation to the
       slab free list. */
    if (!slab || slab->magic != UHM_SLAB_MAGIC ||
        slab->size_class != size_class ||
        slab->slot_size < payload_size ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
        atomic_load_explicit(&slab->ctx_cached_slots,
                             UHM_ATOMIC_ACQUIRE) == 0u ||
        !uhm_extent_descriptor_matches(
            &slab->desc, UHM_EXTENT_KIND_MALLOC, slab->base, slab->size,
            UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_MALLOC_LIVE,
            UHM_PAGE_COMMIT_LIVE, UHM_PROT_READ | UHM_PROT_WRITE,
            slab->generation, slab) ||
        !uhm_slab_slot_belongs_to_verified_slab(slot, slab)) {
        return 0;
    }
    if (out_slab) {
        *out_slab = slab;
    }
    return 1;
}

static int uhm_local_cache_pop_candidate(
    uhm_heap *heap,
    uhm_slab_slot *slot,
    uint16_t size_class,
    size_t payload_size,
    uhm_slab **out_slab,
    uint32_t *out_cached_requested) {
    uhm_slab *slab = 0;
    uint32_t state;

    if (out_slab) {
        *out_slab = 0;
    }
    if (out_cached_requested) {
        *out_cached_requested = 0;
    }
    if (!uhm_local_cache_cached_slot_owner(
            heap, slot, size_class, payload_size, &slab) ||
        slot->magic != UHM_SLAB_SLOT_MAGIC ||
        slot->size_class != size_class ||
        slot->state != 0 ||
        !uhm_slab_slot_ctx_cache_state_valid_with_slab(
            slot, slab, out_cached_requested)) {
        return 0;
    }
    state = atomic_load_explicit(&slab->state_mirror, UHM_ATOMIC_ACQUIRE);
    if (state != UHM_SLAB_STATE_PARTIAL && state != UHM_SLAB_STATE_FULL) {
        if (out_cached_requested) {
            *out_cached_requested = 0;
        }
        return 0;
    }
    if (out_slab) {
        *out_slab = slab;
    }
    return 1;
}

static uhm_slab_slot *uhm_local_cache_push_candidate_from_record(
    uhm_heap *heap,
    uhm_metadata_reader_state *reader,
    void *ptr,
    const uhm_page_record *record,
    uhm_slab **out_slab,
    uint32_t *out_flags,
    uint32_t *out_requested) {
    uintptr_t payload = (uintptr_t)ptr;
    uintptr_t slot_addr;
    uintptr_t slot_end;
    uintptr_t slab_addr;
    uintptr_t slab_end;
    size_t header = uhm_slab_slot_header_size();
    size_t offset;
    uint32_t state;
    uint32_t flags;
    uint32_t requested;
    uhm_slab *slab;
    uhm_slab_slot *slot;

    if (out_slab) {
        *out_slab = 0;
    }
    if (out_flags) {
        *out_flags = 0;
    }
    if (out_requested) {
        *out_requested = 0;
    }
    if (!heap || !ptr || !record || payload < header ||
        payload < heap->core.arena_base || payload >= heap->core.arena_end) {
        return 0;
    }
    if (record->owner != UHM_PAGE_OWNER_MALLOC ||
        record->va_state != UHM_PAGE_VA_MALLOC_LIVE ||
        record->commit_state != UHM_PAGE_COMMIT_LIVE ||
        record->prot != (UHM_PROT_READ | UHM_PROT_WRITE) ||
        record->descriptor_id < offsetof(uhm_slab, desc) ||
        record->descriptor_id > (uint64_t)UINTPTR_MAX) {
        return 0;
    }
    slab_addr =
        (uintptr_t)record->descriptor_id - offsetof(uhm_slab, desc);
    if ((slab_addr % alignof(uhm_slab)) != 0u ||
        uhm_range_overflows(slab_addr, sizeof(uhm_slab), &slab_end) ||
        slab_addr < heap->core.arena_base || slab_end > heap->core.arena_end) {
        return 0;
    }
    slot_addr = payload - header;
    slot = (uhm_slab_slot *)slot_addr;
    slab = (uhm_slab *)slab_addr;
    /* The reader epoch protects the descriptor while the Page Map snapshot
       is converted into a concrete slab and slot. */
    state = atomic_load_explicit(&slab->state_mirror, UHM_ATOMIC_ACQUIRE);
    if (!slab || slab->magic != UHM_SLAB_MAGIC ||
        (state != UHM_SLAB_STATE_PARTIAL && state != UHM_SLAB_STATE_FULL) ||
        atomic_load_explicit(&slab->live_slots_count, UHM_ATOMIC_ACQUIRE) ==
            0u ||
        slab->slot_stride == 0 ||
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0 ||
        slab->desc.id != record->descriptor_id ||
        slab->generation != record->generation ||
        !uhm_extent_descriptor_matches(
            &slab->desc, UHM_EXTENT_KIND_MALLOC, slab->base, slab->size,
            UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_MALLOC_LIVE,
            UHM_PAGE_COMMIT_LIVE, UHM_PROT_READ | UHM_PROT_WRITE,
            record->generation, slab) ||
        uhm_range_overflows(slab->base, slab->size, &slab_end)) {
        return 0;
    }
    if (slot_addr < slab->base ||
        uhm_range_overflows(slot_addr, sizeof(*slot), &slot_end) ||
        slot_end > slab_end ||
        (slot_addr % alignof(uhm_slab_slot)) != 0) {
        return 0;
    }
    offset = (size_t)(slot_addr - slab->base);
    if ((offset % slab->slot_stride) != 0 ||
        offset / slab->slot_stride >= slab->total_slots ||
        payload != slot_addr + header) {
        return 0;
    }
    if (reader) {
        if (!uhm_metadata_reader_hazard_set(reader, 0u, slot) ||
            !uhm_metadata_reader_hazard_set(reader, 1u, slab)) {
            return 0;
        }
    }
    if (slot->magic != UHM_SLAB_SLOT_MAGIC ||
        slot->size_class != slab->size_class ||
        slot->state != 0 ||
        !uhm_slab_slot_belongs_to_verified_slab(slot, slab) ||
        !uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested)) {
        return 0;
    }
    if (out_slab) {
        *out_slab = slab;
    }
    if (out_flags) {
        *out_flags = flags;
    }
    if (out_requested) {
        *out_requested = requested;
    }
    return slot;
}

static uhm_slab_slot *uhm_local_cache_push_candidate_likely(
    uhm_heap *heap,
    uhm_metadata_reader_state *reader,
    void *ptr,
    uhm_slab **out_slab,
    uhm_page_record **out_record,
    uint32_t *out_flags,
    uint32_t *out_requested) {
    uhm_page_record record;
    uhm_page_record *record_ref = 0;
    uhm_slab_slot *slot;

    if (out_record) {
        *out_record = 0;
    }
    if (!reader ||
        !uhm_slab_payload_page_record_snapshot_ref(
            heap, (uintptr_t)ptr, &record, &record_ref)) {
        return 0;
    }
    slot = uhm_local_cache_push_candidate_from_record(
        heap, reader, ptr, &record, out_slab, out_flags, out_requested);
    if (slot && out_record) {
        *out_record = record_ref;
    }
    return slot;
}

static int uhm_local_page_hint_set_base(uhm_heap *heap,
                                        uintptr_t address,
                                        size_t *out_base) {
    size_t page_size;
    size_t page_index;

    if (out_base) {
        *out_base = 0;
    }
    if (!heap || !out_base || address < heap->core.arena_base ||
        address >= heap->core.arena_end) {
        return 0;
    }
    page_size = heap->F.vm.page_size;
    if (page_size == 0) {
        return 0;
    }
    page_index = (size_t)(address - heap->core.arena_base) / page_size;
    if (page_index >= heap->core.page_count) {
        return 0;
    }
    *out_base =
        (page_index & (UHM_LOCAL_PAGE_HINT_SET_COUNT - 1u)) *
        UHM_LOCAL_PAGE_HINT_WAYS;
    return 1;
}

static void uhm_local_page_hint_promote(uhm_local_page_hint *hints,
                                        size_t base,
                                        size_t way) {
    uhm_local_page_hint selected;

    if (!hints || way == 0u || way >= UHM_LOCAL_PAGE_HINT_WAYS) {
        return;
    }
    selected = hints[base + way];
    while (way != 0u) {
        hints[base + way] = hints[base + way - 1u];
        --way;
    }
    hints[base] = selected;
}

static void uhm_local_page_hint_note(uhm_heap *heap,
                                     uhm_local_page_hint *hints,
                                     uhm_slab_slot *anchor,
                                     uhm_slab *slab,
                                     uhm_page_record *record) {
    size_t base;
    size_t way;
    size_t target = UHM_LOCAL_PAGE_HINT_WAYS - 1u;

    if (!hints || !anchor || !slab || !record ||
        !uhm_local_page_hint_set_base(heap, slab->base, &base)) {
        return;
    }
    for (way = 0u; way < UHM_LOCAL_PAGE_HINT_WAYS; ++way) {
        uhm_local_page_hint *hint = &hints[base + way];
        if (hint->anchor && uhm_slab_slot_owner(hint->anchor) == slab) {
            uhm_local_page_hint_promote(hints, base, way);
            return;
        }
        if (!hint->anchor && !hint->record) {
            target = way;
            break;
        }
    }
    hints[base + target].anchor = anchor;
    hints[base + target].record = record;
    uhm_local_page_hint_promote(hints, base, target);
}

static void uhm_local_page_hint_replace_unlinked(
    uhm_heap *heap,
    uhm_local_page_hint *hints,
    uhm_slab_slot *const *heads,
    uhm_slab_slot *removed,
    uhm_slab *slab,
    int slab_still_cached) {
    uhm_slab_slot *replacement;
    size_t base;
    size_t way;

    if (!hints || !heads || !removed || !slab ||
        slab->size_class >= UHM_SIZE_CLASS_COUNT ||
        !uhm_local_page_hint_set_base(heap, slab->base, &base)) {
        return;
    }
    for (way = 0u; way < UHM_LOCAL_PAGE_HINT_WAYS; ++way) {
        if (hints[base + way].anchor == removed) {
            break;
        }
    }
    if (way == UHM_LOCAL_PAGE_HINT_WAYS) {
        return;
    }
    replacement = 0;
    if (slab_still_cached != 0) {
        replacement = heads[slab->size_class];
        while (replacement && uhm_slab_slot_owner(replacement) != slab) {
            replacement = replacement->next_free;
        }
    }
    hints[base + way].anchor = replacement;
    if (replacement) {
        return;
    }
    while (way + 1u < UHM_LOCAL_PAGE_HINT_WAYS) {
        hints[base + way] = hints[base + way + 1u];
        ++way;
    }
    hints[base + UHM_LOCAL_PAGE_HINT_WAYS - 1u].anchor = 0;
    hints[base + UHM_LOCAL_PAGE_HINT_WAYS - 1u].record = 0;
}

static uhm_slab_slot *uhm_local_cache_push_candidate_from_hint(
    uhm_heap *heap,
    uhm_local_page_hint *hints,
    void *ptr,
    uhm_slab **out_slab,
    uhm_page_record **out_record,
    uint32_t *out_flags,
    uint32_t *out_requested) {
    uintptr_t payload = (uintptr_t)ptr;
    uhm_local_page_hint *hint;
    uhm_page_record record;
    uhm_slab_slot *anchor;
    uhm_slab *slab = 0;
    uhm_slab_slot *slot;
    uintptr_t slot_addr;
    uintptr_t slot_end;
    uintptr_t slab_end;
    size_t header = uhm_slab_slot_header_size();
    size_t offset;
    uint32_t state;
    uint32_t flags;
    uint32_t requested;
    uint32_t cached_requested = 0;
    size_t base;
    size_t way;

    if (out_slab) {
        *out_slab = 0;
    }
    if (out_record) {
        *out_record = 0;
    }
    if (out_flags) {
        *out_flags = 0;
    }
    if (out_requested) {
        *out_requested = 0;
    }
    if (!hints ||
        !uhm_local_page_hint_set_base(heap, payload, &base)) {
        return 0;
    }
    for (way = 0u; way < UHM_LOCAL_PAGE_HINT_WAYS; ++way) {
        hint = &hints[base + way];
        anchor = hint->anchor;
        if (!anchor || !hint->record ||
            !uhm_local_cache_cached_slot_owner(
                heap, anchor, anchor->size_class, 0u, &slab) ||
            !uhm_slab_slot_ctx_cache_state_valid_with_slab(
                anchor, slab, &cached_requested) ||
            uhm_range_overflows(slab->base, slab->size, &slab_end) ||
            payload < slab->base || payload >= slab_end) {
            continue;
        }
        record = *hint->record;
        state = atomic_load_explicit(&slab->state_mirror,
                                     UHM_ATOMIC_ACQUIRE);
        if (record.owner != UHM_PAGE_OWNER_MALLOC ||
            record.va_state != UHM_PAGE_VA_MALLOC_LIVE ||
            record.commit_state != UHM_PAGE_COMMIT_LIVE ||
            record.prot != (UHM_PROT_READ | UHM_PROT_WRITE) ||
            record.descriptor_id != slab->desc.id ||
            record.generation != slab->generation ||
            (state != UHM_SLAB_STATE_PARTIAL &&
             state != UHM_SLAB_STATE_FULL) ||
            atomic_load_explicit(&slab->live_slots_count,
                                 UHM_ATOMIC_ACQUIRE) == 0u ||
            slab->slot_stride == 0u || payload < header) {
            continue;
        }
        slot_addr = payload - header;
        slot = (uhm_slab_slot *)slot_addr;
        if (slot_addr < slab->base ||
            uhm_range_overflows(slot_addr, sizeof(*slot), &slot_end) ||
            slot_end > slab_end ||
            (slot_addr % alignof(uhm_slab_slot)) != 0u) {
            continue;
        }
        offset = (size_t)(slot_addr - slab->base);
        if ((offset % slab->slot_stride) != 0u ||
            offset / slab->slot_stride >= slab->total_slots ||
            payload != slot_addr + header ||
            slot->magic != UHM_SLAB_SLOT_MAGIC ||
            slot->size_class != slab->size_class || slot->state != 0u ||
            !uhm_slab_slot_belongs_to_verified_slab(slot, slab) ||
            !uhm_slab_slot_live_state_mirror_valid_with_slab(
                slot, slab, &flags, &requested)) {
            continue;
        }
        if (out_slab) {
            *out_slab = slab;
        }
        if (out_record) {
            *out_record = hint->record;
        }
        if (out_flags) {
            *out_flags = flags;
        }
        if (out_requested) {
            *out_requested = requested;
        }
        return slot;
    }
    return 0;
}

static void uhm_ctx_cache_unlink(uhm_heap *heap,
                                 uhm_thread_ctx *ctx,
                                 uhm_slab_slot **link,
                                 uhm_slab_slot *slot,
                                 uhm_slab *slab) {
    int slab_still_cached;
    size_t slot_bytes;

    if (!heap || !ctx || !link || !slot || !slab || *link != slot) {
        return;
    }
    slot_bytes = slab->slot_size;
    *link = slot->next_free;
    slot->next_free = 0;
    if (!ctx->local_cache_heads[slab->size_class]) {
        ctx->nonempty_class_mask &=
            ~(UINT32_C(1) << slab->size_class);
    }
    uhm_local_cache_repair_head_after_unlink(
        &ctx->local_cache_head, ctx->local_cache_heads, slot,
        slab->size_class);
    slab_still_cached = uhm_local_cache_note_slab_unlink(
        heap, ctx->local_cache_heads, ctx->local_retained_hints,
        &ctx->retained_slab_pages, slab);
    uhm_local_page_hint_replace_unlinked(
        heap, ctx->local_page_hints, ctx->local_cache_heads, slot, slab,
        slab_still_cached);
    if (ctx->local_cache_count != 0) {
        --ctx->local_cache_count;
    }
    if (ctx->local_cache_bytes >= slot_bytes) {
        ctx->local_cache_bytes -= slot_bytes;
    } else {
        ctx->local_cache_bytes = 0;
    }
}

static void *uhm_ctx_cache_pop(uhm_heap *heap, uhm_thread_ctx *ctx,
                               uint16_t size_class, size_t payload_size,
                               size_t requested_size) {
    uhm_slab_slot **link;

    if (!heap || !ctx || payload_size > UHM_SLAB_MAX_SLOT_SIZE) {
        return 0;
    }
    for (; uhm_cache_size_class_uses_slab(size_class); ++size_class) {
        link = uhm_ctx_cache_bucket(ctx, size_class);
        if (!link) {
            return 0;
        }
        while (*link) {
            uhm_slab_slot *slot = *link;
            uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 1);
            uint32_t cached_requested = 0;
            if (slab && slab->magic == UHM_SLAB_MAGIC &&
                (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
                slab->size_class == size_class &&
                slab->slot_size >= payload_size) {
                uhm_slab_raw_lock(slab);
                if (*link != slot ||
                    !uhm_slab_slot_ctx_cache_state_valid_with_slab(
                        slot, slab, &cached_requested)) {
                    uhm_slab_raw_unlock(slab);
                    link = &slot->next_free;
                    continue;
                }
                if (!uhm_cached_requested_bytes_can_sub(
                        &ctx->cached_requested_bytes, cached_requested)) {
                    uhm_slab_raw_unlock(slab);
                    return 0;
                }
                if (slab->ctx_cached_slots == 0) {
                    uhm_slab_raw_unlock(slab);
                    return 0;
                }
                if (!uhm_slab_slot_state_cas(
                        slot,
                        UHM_SLAB_SLOT_FLAG_FREE |
                            UHM_SLAB_SLOT_FLAG_CTX_CACHE,
                        0u, 0u, (uint32_t)requested_size)) {
                    uhm_slab_raw_unlock(slab);
                    return 0;
                }
                uhm_cached_requested_bytes_sub_unchecked(
                    &ctx->cached_requested_bytes, cached_requested);
                uhm_ctx_cache_unlink(heap, ctx, link, slot, slab);
                --slab->ctx_cached_slots;
                uhm_slab_raw_unlock(slab);
                uhm_stats_malloc_live_replace(
                    heap, (uintptr_t)uhm_slab_slot_payload(slot),
                    cached_requested, requested_size);
                if (atomic_load_explicit(&ctx->idle_miss_streak,
                                         UHM_ATOMIC_RELAXED) != 0u) {
                    atomic_store_explicit(&ctx->idle_miss_streak, 0u,
                                          UHM_ATOMIC_RELAXED);
                }
                return uhm_slab_slot_payload(slot);
            }
            link = &slot->next_free;
        }
    }
    return 0;
}

static void *uhm_ctx_cache_pop_partial_lowlock(uhm_heap *heap,
                                               uhm_thread_ctx *ctx,
                                               uint16_t size_class,
                                               size_t payload_size,
                                               size_t requested_size) {
    uhm_slab_slot **link;

    if (!heap || !ctx || payload_size > UHM_SLAB_MAX_SLOT_SIZE ||
        !uhm_cache_size_class_uses_slab(size_class) ||
        !ctx->local_cache_heads[size_class]) {
        return 0;
    }
    for (; uhm_cache_size_class_uses_slab(size_class); ++size_class) {
        link = uhm_ctx_cache_bucket(ctx, size_class);
        if (!link) {
            break;
        }
        while (*link) {
            uhm_slab_slot *slot = *link;
            uhm_slab *slab = 0;
            uint32_t cached_requested = 0;
            void *payload;

            if (!uhm_local_cache_pop_candidate(
                    heap, slot, size_class, payload_size, &slab,
                    &cached_requested)) {
                break;
            }
            if (uhm_cached_requested_bytes_can_sub(
                    &ctx->cached_requested_bytes, cached_requested) &&
                uhm_slab_slot_state_cas(
                    slot,
                    UHM_SLAB_SLOT_FLAG_FREE |
                        UHM_SLAB_SLOT_FLAG_CTX_CACHE,
                    0u, 0u, (uint32_t)requested_size)) {
                uhm_cached_requested_bytes_sub_unchecked(
                    &ctx->cached_requested_bytes, cached_requested);
                uhm_ctx_cache_unlink(heap, ctx, link, slot, slab);
                (void)atomic_fetch_sub_explicit(
                    &slab->ctx_cached_slots, 1u, UHM_ATOMIC_ACQ_REL);
                payload = uhm_slab_slot_payload(slot);
                uhm_stats_malloc_live_replace(heap, (uintptr_t)payload,
                                              cached_requested,
                                              requested_size);
                if (atomic_load_explicit(&ctx->idle_miss_streak,
                                         UHM_ATOMIC_RELAXED) != 0u) {
                    atomic_store_explicit(&ctx->idle_miss_streak, 0u,
                                          UHM_ATOMIC_RELAXED);
                }
                return payload;
            }
            break;
        }
    }
    return 0;
}

static int uhm_ctx_cache_can_accept(uhm_heap *heap,
                                    uhm_thread_ctx *ctx,
                                    const uhm_slab_slot *slot,
                                    size_t *out_retain_token) {
    size_t slot_bytes;

    if (!heap || !ctx || !slot ||
        !uhm_slab_slot_owner_from_heap(heap, slot, 0) ||
        ctx->local_cache_limit == 0) {
        return 0;
    }
    slot_bytes = uhm_slab_slot_owner_slot_size_from_heap(heap, slot, 0);
    return slot_bytes != 0 &&
           slot_bytes <= ctx->local_cache_limit &&
           ctx->local_cache_bytes <= ctx->local_cache_limit - slot_bytes &&
           uhm_local_cache_can_retain_slab(
               heap, ctx->local_cache_heads, ctx->local_retained_hints,
               ctx->retained_slab_pages,
               ctx->retained_slab_page_limit,
               uhm_slab_slot_owner(slot), out_retain_token);
}

static int uhm_ctx_cache_can_accept_known_slot_size(uhm_heap *heap,
                                                    uhm_thread_ctx *ctx,
                                                    uhm_slab *slab,
                                                    size_t slot_bytes,
                                                    size_t *out_retain_token) {
    return ctx && slot_bytes != 0 &&
           slot_bytes <= ctx->local_cache_limit &&
           ctx->local_cache_bytes <= ctx->local_cache_limit - slot_bytes &&
           uhm_local_cache_can_retain_slab(
               heap, ctx->local_cache_heads, ctx->local_retained_hints,
               ctx->retained_slab_pages,
               ctx->retained_slab_page_limit,
               slab, out_retain_token);
}

static uhm_result uhm_ctx_cache_push(uhm_heap *heap, uhm_thread_ctx *ctx,
                                     uhm_slab_slot *slot) {
    uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
    uhm_slab_slot **link;
    size_t requested;
    size_t slot_bytes;
    size_t retain_token = SIZE_MAX;
    uint32_t flags;
    uint32_t requested32;

    if (!heap || !ctx || !slot || !slab ||
        slab->magic != UHM_SLAB_MAGIC ||
        !uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested32) ||
        !uhm_ctx_cache_can_accept(heap, ctx, slot,
                                  &retain_token)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    link = uhm_ctx_cache_bucket(ctx, slab->size_class);
    if (!link) {
        return UHM_ERR_CORRUPTION;
    }
    requested = requested32;
    slot_bytes = slab->slot_size;
    uhm_slab_raw_lock(slab);
    if (!uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested32)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    requested = requested32;
    if (!uhm_cached_requested_bytes_can_add(&ctx->cached_requested_bytes,
                                            requested)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_slab_slot_state_cas(
            slot, flags, (uint32_t)requested,
            UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_CTX_CACHE, 0u)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    uhm_cached_requested_bytes_add_unchecked(&ctx->cached_requested_bytes,
                                             requested);
    slot->requested_size = (uint32_t)requested;
    uhm_local_cache_note_slab_push(
        heap, ctx->local_cache_heads, ctx->local_retained_hints,
        &ctx->retained_slab_pages, slab, retain_token);
    slot->next_free = *link;
    *link = slot;
    if (!slot->next_free) {
        ctx->nonempty_class_mask |= UINT32_C(1) << slab->size_class;
    }
    ctx->local_cache_head = slot;
    ctx->local_cache_bytes += slot_bytes;
    ++ctx->local_cache_count;
    ++slab->ctx_cached_slots;
    uhm_slab_raw_unlock(slab);
    return UHM_OK;
}

static int uhm_ctx_cache_push_partial_lowlock(uhm_heap *heap,
                                              uhm_thread_ctx *ctx,
                                              void *ptr) {
    uhm_metadata_reader_state *reader = 0;
    uhm_slab_slot *slot;
    uhm_slab *slab = 0;
    uhm_page_record *record_ref = 0;
    uhm_slab_slot **link;
    uint32_t flags = 0u;
    uint32_t requested32 = 0u;
    size_t requested;
    size_t slot_bytes;
    size_t retain_token = SIZE_MAX;
    int handled = 0;

    if (!heap || !ctx || !ptr) {
        return 0;
    }
    slot = 0;
    if (ctx->local_cache_count != 0u) {
        slot = uhm_local_cache_push_candidate_from_hint(
            heap, ctx->local_page_hints, ptr, &slab, &record_ref,
            &flags, &requested32);
    }
    if (!slot) {
        if (!uhm_metadata_reader_begin_unregistered(&ctx->metadata_reader,
                                                    heap)) {
            return 0;
        }
        reader = &ctx->metadata_reader;
        slot = uhm_local_cache_push_candidate_likely(
            heap, reader, ptr, &slab, &record_ref, &flags,
            &requested32);
    }
    if (!slot ||
        !uhm_ctx_cache_can_accept_known_slot_size(
            heap, ctx, slab, slab->slot_size, &retain_token)) {
        if (reader) {
            uhm_lockless_remote_reader_release(reader);
        }
        return 0;
    }
    link = uhm_ctx_cache_bucket(ctx, slab->size_class);
    if (!link) {
        if (reader) {
            uhm_lockless_remote_reader_release(reader);
        }
        return 0;
    }
    slot_bytes = slab->slot_size;
    requested = requested32;
    if (uhm_cached_requested_bytes_can_add(
            &ctx->cached_requested_bytes, requested) &&
        uhm_slab_slot_state_cas(
            slot, flags, (uint32_t)requested,
            UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_CTX_CACHE,
            0u)) {
        uhm_cached_requested_bytes_add_unchecked(
            &ctx->cached_requested_bytes, requested);
        slot->requested_size = (uint32_t)requested;
        uhm_local_cache_note_slab_push(
            heap, ctx->local_cache_heads, ctx->local_retained_hints,
            &ctx->retained_slab_pages, slab, retain_token);
        slot->next_free = *link;
        *link = slot;
        if (!slot->next_free) {
            ctx->nonempty_class_mask |= UINT32_C(1) << slab->size_class;
        }
        ctx->local_cache_head = slot;
        ctx->local_cache_bytes += slot_bytes;
        ++ctx->local_cache_count;
        (void)atomic_fetch_add_explicit(
            &slab->ctx_cached_slots, 1u, UHM_ATOMIC_ACQ_REL);
        uhm_local_page_hint_note(heap, ctx->local_page_hints,
                                 slot, slab, record_ref);
        handled = 1;
    }
    if (reader) {
        uhm_lockless_remote_reader_release(reader);
    }
    return handled;
}

static uhm_result uhm_ctx_cache_flush_slots_locked(uhm_heap *heap,
                                                   uhm_thread_ctx *ctx) {
    uhm_result result = UHM_OK;
    uhm_slab *slab;

    if (!heap || !ctx) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    while (ctx->local_cache_count != 0) {
        uhm_slab_slot **link = 0;
        uhm_slab_slot *slot;
        uint16_t size_class = UHM_SIZE_CLASS_COUNT;

        if (uhm_local_cache_find_fit_class(
                ctx->nonempty_class_mask, 0u, &size_class) &&
            ctx->local_cache_heads[size_class]) {
            link = &ctx->local_cache_heads[size_class];
        }
        if (!link) {
            result = UHM_ERR_CORRUPTION;
            break;
        }
        slot = *link;
        slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
        if (!slab || slab->magic != UHM_SLAB_MAGIC) {
            result = UHM_ERR_CORRUPTION;
            break;
        }
        uhm_slab_raw_lock(slab);
        {
            uint32_t cached_requested = 0;
            if (!uhm_slab_slot_ctx_cache_state_valid_with_slab(
                    slot, slab, &cached_requested) ||
                !uhm_cached_requested_bytes_can_sub(
                    &ctx->cached_requested_bytes, cached_requested) ||
                slab->ctx_cached_slots == 0 || slab->live_slots == 0) {
                uhm_slab_raw_unlock(slab);
                result = UHM_ERR_CORRUPTION;
                break;
            }
            if (!uhm_slab_slot_state_cas(
                    slot,
                    UHM_SLAB_SLOT_FLAG_FREE |
                        UHM_SLAB_SLOT_FLAG_CTX_CACHE,
                    0u, UHM_SLAB_SLOT_FLAG_FREE, 0u)) {
                uhm_slab_raw_unlock(slab);
                result = UHM_ERR_CORRUPTION;
                break;
            }
            uhm_cached_requested_bytes_sub_unchecked(
                &ctx->cached_requested_bytes, cached_requested);
            uhm_stats_malloc_live_sub(
                heap, (uintptr_t)uhm_slab_slot_payload(slot),
                cached_requested);
        }
        uhm_ctx_cache_unlink(heap, ctx, link, slot, slab);
        slot->next_free = slab->free_head;
        slab->free_head = slot;
        --slab->ctx_cached_slots;
        --slab->live_slots;
        ++slab->free_slots;
        uhm_slab_refresh_state_raw_locked(heap, slab);
        uhm_slab_raw_unlock(slab);
    }
    return result;
}

static uhm_slab *uhm_local_cache_cold_slab_excluding(
    uhm_slab_slot *const *heads,
    const uhm_slab *excluded) {
    size_t i;

    if (!heads) {
        return 0;
    }
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        uhm_slab_slot *slot;
        uhm_slab *candidate = 0;

        for (slot = heads[i]; slot; slot = slot->next_free) {
            uhm_slab *slab = uhm_slab_slot_owner(slot);
            if (slab && slab != excluded) {
                candidate = slab;
            }
        }
        if (candidate) {
            return candidate;
        }
    }
    return 0;
}

static uhm_result uhm_ctx_cache_evict_slab_locked(uhm_heap *heap,
                                                   uhm_thread_ctx *ctx,
                                                   uhm_slab *victim) {
    size_t i;

    if (!heap || !ctx || !victim || victim->magic != UHM_SLAB_MAGIC) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_slab_raw_lock(victim);
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        uhm_slab_slot **link = &ctx->local_cache_heads[i];

        while (*link) {
            uhm_slab_slot *slot = *link;
            uint32_t cached_requested = 0;

            if (uhm_slab_slot_owner(slot) != victim) {
                link = &slot->next_free;
                continue;
            }
            if (!uhm_slab_slot_ctx_cache_state_valid_with_slab(
                    slot, victim, &cached_requested) ||
                !uhm_cached_requested_bytes_can_sub(
                    &ctx->cached_requested_bytes, cached_requested) ||
                victim->ctx_cached_slots == 0 || victim->live_slots == 0 ||
                !uhm_slab_slot_state_cas(
                    slot,
                    UHM_SLAB_SLOT_FLAG_FREE |
                        UHM_SLAB_SLOT_FLAG_CTX_CACHE,
                    0u, UHM_SLAB_SLOT_FLAG_FREE, 0u)) {
                uhm_slab_raw_unlock(victim);
                return UHM_ERR_CORRUPTION;
            }
            uhm_cached_requested_bytes_sub_unchecked(
                &ctx->cached_requested_bytes, cached_requested);
            uhm_stats_malloc_live_sub(
                heap, (uintptr_t)uhm_slab_slot_payload(slot),
                cached_requested);
            uhm_ctx_cache_unlink(heap, ctx, link, slot, victim);
            slot->next_free = victim->free_head;
            victim->free_head = slot;
            --victim->ctx_cached_slots;
            --victim->live_slots;
            ++victim->free_slots;
        }
    }
    uhm_slab_refresh_state_raw_locked(heap, victim);
    uhm_slab_raw_unlock(victim);
    return uhm_slab_maybe_decommit_empty(heap, victim);
}

static uhm_result uhm_ctx_cache_evict_cold_slab_locked(
    uhm_heap *heap,
    uhm_thread_ctx *ctx,
    const uhm_slab *incoming) {
    uhm_slab *victim = uhm_local_cache_cold_slab_excluding(
        ctx ? ctx->local_cache_heads : 0, incoming);

    if (!victim) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    return uhm_ctx_cache_evict_slab_locked(heap, ctx, victim);
}

static void uhm_ctx_cache_note_idle_miss(uhm_heap *heap,
                                         uhm_thread_ctx *ctx) {
    size_t base_pages;
    uhm_result result = UHM_OK;

    if (!heap || !ctx || ctx->local_cache_count == 0u) {
        return;
    }
    {
        unsigned streak = atomic_load_explicit(
            &ctx->idle_miss_streak, UHM_ATOMIC_RELAXED);
        if (streak + 1u < UHM_LOCAL_CACHE_IDLE_MISS_THRESHOLD) {
            atomic_store_explicit(&ctx->idle_miss_streak, streak + 1u,
                                  UHM_ATOMIC_RELAXED);
            return;
        }
    }
    atomic_store_explicit(&ctx->idle_miss_streak, 0u,
                          UHM_ATOMIC_RELAXED);
    base_pages = uhm_local_cache_page_limit_for_bytes(
        heap, ctx->local_cache_limit);
    if (ctx->retained_slab_pages <= base_pages) {
        return;
    }
    uhm_lock(&heap->lock);
    if (ctx->magic == UHM_THREAD_MAGIC && ctx->owner == heap &&
        atomic_load_explicit(&ctx->lifecycle_state,
                             UHM_ATOMIC_ACQUIRE) == UHM_THREAD_MAGIC &&
        ctx->retained_slab_pages > base_pages) {
        result = uhm_ctx_cache_evict_cold_slab_locked(heap, ctx, 0);
    }
    uhm_unlock(&heap->lock);
    if (result != UHM_OK && result != UHM_ERR_OUT_OF_MEMORY) {
        atomic_store_explicit(&ctx->last_error, (int)result,
                              UHM_ATOMIC_RELEASE);
        uhm_set_error(heap, result);
    }
}

static uhm_result uhm_thread_ctx_flush_all_local_locked(
    uhm_heap *heap, int *maintenance_needed) {
    uhm_result result = UHM_OK;
    uhm_thread_ctx *ctx;

    if (maintenance_needed) {
        *maintenance_needed = 0;
    }
    if (!heap) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    for (ctx = heap->core.thread_ctx_head; ctx; ctx = ctx->next_active) {
        int ctx_maintenance_needed = 0;
        uhm_result flush_result = uhm_thread_ctx_flush_local_locked(
            heap, ctx, &ctx_maintenance_needed);

        if (ctx_maintenance_needed && maintenance_needed) {
            *maintenance_needed = 1;
        }
        if (flush_result != UHM_OK && result == UHM_OK) {
            result = flush_result;
        }
    }
    return result;
}

static void uhm_thread_ctx_detach_locked(uhm_heap *heap, uhm_thread_ctx *ctx) {
    if (!heap || !ctx) {
        return;
    }
    if (ctx->prev_active) {
        ctx->prev_active->next_active = ctx->next_active;
    } else if (heap->core.thread_ctx_head == ctx) {
        heap->core.thread_ctx_head = ctx->next_active;
    }
    if (ctx->next_active) {
        ctx->next_active->prev_active = ctx->prev_active;
    }
    atomic_store_explicit(&ctx->lifecycle_state, 0u, UHM_ATOMIC_RELEASE);
    uhm_metadata_reader_clear(&ctx->metadata_reader);
    ctx->magic = 0;
    ctx->owner = 0;
    ctx->owner_thread_id = 0;
    ctx->local_cache_head = 0;
    memset(ctx->local_cache_heads, 0, sizeof(ctx->local_cache_heads));
    ctx->nonempty_class_mask = 0u;
    memset(ctx->local_page_hints, 0, sizeof(ctx->local_page_hints));
    memset(ctx->local_retained_hints, 0,
           sizeof(ctx->local_retained_hints));
    ctx->local_cache_bytes = 0;
    ctx->local_cache_count = 0;
    ctx->retained_slab_pages = 0;
    ctx->retained_slab_page_limit = 0;
    atomic_store_explicit(&ctx->cached_requested_bytes, 0u,
                          UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&ctx->idle_miss_streak, 0u,
                          UHM_ATOMIC_RELEASE);
    memset(ctx->deferred_free, 0, sizeof(ctx->deferred_free));
    ctx->deferred_free_count = 0;
    ctx->deferred_free_epoch = 0;
    ctx->prev_active = 0;
    ctx->next_active = 0;
}

static void uhm_thread_ctx_detach_all_locked(uhm_heap *heap) {
    if (!heap) {
        return;
    }
    while (heap->core.thread_ctx_head) {
        uhm_thread_ctx_detach_locked(heap, heap->core.thread_ctx_head);
    }
}

static size_t uhm_tls_cache_limit_for_pages(uhm_heap *heap, size_t pages) {
    if (!heap || pages == 0 || pages > SIZE_MAX / heap->F.vm.page_size) {
        return 0;
    }
    return pages * heap->F.vm.page_size;
}

static void uhm_tls_cache_recompute_limit(uhm_heap *heap,
                                          uhm_tls_cache *cache) {
    if (!heap || !cache) {
        return;
    }
    cache->local_cache_limit =
        uhm_tls_cache_limit_for_pages(heap, cache->adaptive_pages);
}

static void uhm_tls_cache_grow_one_page(uhm_heap *heap,
                                        uhm_tls_cache *cache) {
    if (!heap || !cache ||
        cache->adaptive_pages >= heap->core.tls_cache_max_pages) {
        return;
    }
    ++cache->adaptive_pages;
    uhm_tls_cache_recompute_limit(heap, cache);
}

static void uhm_tls_cache_adapt_after_hit(uhm_heap *heap,
                                          uhm_tls_cache *cache) {
    if (!cache) {
        return;
    }
    ++cache->hit_count;
    if ((cache->hit_count % 64u) == 0u) {
        uhm_tls_cache_grow_one_page(heap, cache);
    }
}

static void uhm_tls_cache_adapt_after_overflow(uhm_heap *heap,
                                               uhm_tls_cache *cache) {
    if (!cache) {
        return;
    }
    ++cache->overflow_count;
    if ((cache->overflow_count % 8u) == 0u) {
        uhm_tls_cache_grow_one_page(heap, cache);
    }
}

static void uhm_tls_cache_registry_init(uhm_heap *heap, size_t slot_count) {
    size_t i;

    if (!heap) {
        return;
    }
    if (slot_count > UHM_TLS_CACHE_MAX_SLOTS) {
        slot_count = UHM_TLS_CACHE_MAX_SLOTS;
    }
    heap->core.tls_cache_slot_count = slot_count;
    heap->core.tls_cache_active_count = 0;
    heap->core.tls_cache_head = 0;
    heap->tls_cache_free_head = 0;
    for (i = 0; i < UHM_TLS_CACHE_MAX_SLOTS; ++i) {
        uhm_tls_cache *cache = &heap->tls_cache_slots[i];
        memset(cache, 0, sizeof(*cache));
        uhm_metadata_reader_state_init(&cache->metadata_reader);
        atomic_init(&cache->owner_ref, (uintptr_t)0);
        atomic_init(&cache->cached_requested_bytes, 0u);
        if (i < slot_count) {
            cache->next_free = heap->tls_cache_free_head;
            heap->tls_cache_free_head = cache;
        }
    }
}

static int uhm_tls_cache_is_enabled(const uhm_heap *heap) {
    return heap && heap->core.tls_cache_max_pages != 0 &&
           heap->core.tls_cache_key_valid != 0 &&
           heap->core.tls_cache_slot_count != 0;
}

static void uhm_tls_cache_adapt_after_retained_page_pressure(
    uhm_heap *heap,
    uhm_tls_cache *cache) {
    if (!cache) {
        return;
    }
    ++cache->retained_page_pressure_count;
    uhm_tls_cache_grow_one_page(heap, cache);
}

static uhm_tls_cache *uhm_tls_cache_current(uhm_heap *heap) {
    uhm_tls_cache *cache;

    if (!uhm_tls_cache_is_enabled(heap)) {
        return 0;
    }
    cache = (uhm_tls_cache *)uhm_tls_get_value(heap);
    if (cache && cache->magic == UHM_TLS_CACHE_MAGIC &&
        cache->owner == heap &&
        atomic_load_explicit(&cache->lifecycle_state,
                             UHM_ATOMIC_ACQUIRE) == UHM_TLS_CACHE_MAGIC) {
        return cache;
    }
    return 0;
}

static uhm_tls_cache *uhm_tls_cache_get_or_create_locked(uhm_heap *heap) {
    uhm_tls_cache *cache;
    size_t pages;

    if (!uhm_tls_cache_is_enabled(heap)) {
        return 0;
    }
    cache = uhm_tls_cache_current(heap);
    if (cache) {
        return cache;
    }
    if (!heap->tls_cache_free_head) {
        return 0;
    }
    cache = heap->tls_cache_free_head;
    heap->tls_cache_free_head = cache->next_free;
    memset(cache, 0, sizeof(*cache));
    cache->magic = UHM_TLS_CACHE_MAGIC;
    cache->abi_major = UHM_ABI_MAJOR;
    cache->abi_minor = UHM_ABI_MINOR;
    atomic_init(&cache->lifecycle_state, 0u);
    uhm_metadata_reader_state_init(&cache->metadata_reader);
    atomic_init(&cache->owner_ref, (uintptr_t)0);
    atomic_init(&cache->cached_requested_bytes, 0u);
    cache->owner = heap;
    atomic_store_explicit(&cache->owner_ref, (uintptr_t)heap,
                          UHM_ATOMIC_RELEASE);
    cache->owner_thread_id = uhm_current_thread_id();
    pages = heap->core.tls_cache_default_pages;
    if (pages == 0) {
        pages = heap->core.tls_cache_min_pages != 0
                    ? heap->core.tls_cache_min_pages
                    : 1u;
    }
    if (pages > heap->core.tls_cache_max_pages) {
        pages = heap->core.tls_cache_max_pages;
    }
    cache->adaptive_pages = pages;
    atomic_init(&cache->idle_miss_streak, 0u);
    cache->retained_slab_page_limit =
        heap->core.tls_cache_max_retained_pages;
    uhm_tls_cache_recompute_limit(heap, cache);
    cache->next_active = heap->core.tls_cache_head;
    if (heap->core.tls_cache_head) {
        heap->core.tls_cache_head->prev_active = cache;
    }
    heap->core.tls_cache_head = cache;
    ++heap->core.tls_cache_active_count;
    atomic_store_explicit(&cache->lifecycle_state, UHM_TLS_CACHE_MAGIC,
                          UHM_ATOMIC_RELEASE);
    if (!uhm_tls_set_value(heap, cache)) {
        if (cache->next_active) {
            cache->next_active->prev_active = 0;
        }
        heap->core.tls_cache_head = cache->next_active;
        if (heap->core.tls_cache_active_count != 0) {
            --heap->core.tls_cache_active_count;
        }
        atomic_store_explicit(&cache->lifecycle_state, 0u,
                              UHM_ATOMIC_RELEASE);
        atomic_store_explicit(&cache->owner_ref, (uintptr_t)0,
                              UHM_ATOMIC_RELEASE);
        cache->magic = 0;
        cache->owner = 0;
        cache->next_free = heap->tls_cache_free_head;
        heap->tls_cache_free_head = cache;
        return 0;
    }
    return cache;
}

static uhm_slab_slot **uhm_tls_cache_bucket(uhm_tls_cache *cache,
                                            uint16_t size_class) {
    if (!cache || size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    return &cache->local_cache_heads[size_class];
}

static void uhm_tls_cache_unlink_slot(uhm_heap *heap,
                                      uhm_tls_cache *cache,
                                      uhm_slab_slot **link,
                                      uhm_slab_slot *slot,
                                      uhm_slab *slab);

static void *uhm_tls_cache_pop_partial_lowlock(uhm_heap *heap,
                                               uhm_tls_cache *cache,
                                               uint16_t size_class,
                                               size_t payload_size,
                                               size_t requested_size) {
    uhm_slab_slot **link;

    if (!heap || !cache || payload_size > UHM_SLAB_MAX_SLOT_SIZE ||
        !uhm_cache_size_class_uses_slab(size_class) ||
        !cache->local_cache_heads[size_class]) {
        return 0;
    }
    for (; uhm_cache_size_class_uses_slab(size_class); ++size_class) {
        link = uhm_tls_cache_bucket(cache, size_class);
        if (!link) {
            break;
        }
        while (*link) {
            uhm_slab_slot *slot = *link;
            uhm_slab *slab = 0;
            uint32_t cached_requested = 0;
            void *payload;

            if (!uhm_local_cache_pop_candidate(
                    heap, slot, size_class, payload_size, &slab,
                    &cached_requested)) {
                break;
            }
            if (uhm_cached_requested_bytes_can_sub(
                    &cache->cached_requested_bytes, cached_requested) &&
                uhm_slab_slot_state_cas(
                    slot,
                    UHM_SLAB_SLOT_FLAG_FREE |
                        UHM_SLAB_SLOT_FLAG_CTX_CACHE,
                    0u, 0u, (uint32_t)requested_size)) {
                uhm_cached_requested_bytes_sub_unchecked(
                    &cache->cached_requested_bytes, cached_requested);
                uhm_tls_cache_unlink_slot(heap, cache, link, slot, slab);
                (void)atomic_fetch_sub_explicit(
                    &slab->ctx_cached_slots, 1u, UHM_ATOMIC_ACQ_REL);
                payload = uhm_slab_slot_payload(slot);
                uhm_stats_malloc_live_replace(heap, (uintptr_t)payload,
                                              cached_requested,
                                              requested_size);
                uhm_tls_cache_adapt_after_hit(heap, cache);
                return payload;
            }
            break;
        }
    }
    return 0;
}

static void uhm_tls_cache_unlink_slot(uhm_heap *heap,
                                      uhm_tls_cache *cache,
                                      uhm_slab_slot **link,
                                      uhm_slab_slot *slot,
                                      uhm_slab *slab) {
    int slab_still_cached;
    size_t slot_bytes;

    if (!heap || !cache || !link || !slot || !slab || *link != slot) {
        return;
    }
    slot_bytes = slab->slot_size;
    *link = slot->next_free;
    slot->next_free = 0;
    if (!cache->local_cache_heads[slab->size_class]) {
        cache->nonempty_class_mask &=
            ~(UINT32_C(1) << slab->size_class);
    }
    uhm_local_cache_repair_head_after_unlink(
        &cache->local_cache_head, cache->local_cache_heads, slot,
        slab->size_class);
    slab_still_cached = uhm_local_cache_note_slab_unlink(
        heap, cache->local_cache_heads, cache->local_retained_hints,
        &cache->retained_slab_pages, slab);
    uhm_local_page_hint_replace_unlinked(
        heap, cache->local_page_hints, cache->local_cache_heads, slot, slab,
        slab_still_cached);
    if (cache->local_cache_count != 0) {
        --cache->local_cache_count;
    }
    if (cache->local_cache_bytes >= slot_bytes) {
        cache->local_cache_bytes -= slot_bytes;
    } else {
        cache->local_cache_bytes = 0;
    }
}

static void *uhm_tls_cache_pop(uhm_heap *heap, uhm_tls_cache *cache,
                               uint16_t size_class, size_t payload_size,
                               size_t requested_size) {
    uhm_slab_slot **link;

    if (!heap || !cache || payload_size > UHM_SLAB_MAX_SLOT_SIZE) {
        return 0;
    }
    for (; uhm_cache_size_class_uses_slab(size_class); ++size_class) {
        link = uhm_tls_cache_bucket(cache, size_class);
        if (!link) {
            return 0;
        }
        while (*link) {
            uhm_slab_slot *slot = *link;
            uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 1);
            uint32_t cached_requested = 0;
            if (slab && slab->magic == UHM_SLAB_MAGIC &&
                (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
                slab->size_class == size_class &&
                slab->slot_size >= payload_size) {
                uhm_slab_raw_lock(slab);
                if (*link != slot ||
                    !uhm_slab_slot_ctx_cache_state_valid_with_slab(
                        slot, slab, &cached_requested)) {
                    uhm_slab_raw_unlock(slab);
                    link = &slot->next_free;
                    continue;
                }
                if (!uhm_cached_requested_bytes_can_sub(
                        &cache->cached_requested_bytes, cached_requested)) {
                    uhm_slab_raw_unlock(slab);
                    return 0;
                }
                if (slab->ctx_cached_slots == 0) {
                    uhm_slab_raw_unlock(slab);
                    return 0;
                }
                if (!uhm_slab_slot_state_cas(
                        slot,
                        UHM_SLAB_SLOT_FLAG_FREE |
                            UHM_SLAB_SLOT_FLAG_CTX_CACHE,
                        0u, 0u, (uint32_t)requested_size)) {
                    uhm_slab_raw_unlock(slab);
                    return 0;
                }
                uhm_cached_requested_bytes_sub_unchecked(
                    &cache->cached_requested_bytes, cached_requested);
                uhm_tls_cache_unlink_slot(heap, cache, link, slot, slab);
                --slab->ctx_cached_slots;
                uhm_slab_raw_unlock(slab);
                uhm_stats_malloc_live_replace(
                    heap, (uintptr_t)uhm_slab_slot_payload(slot),
                    cached_requested, requested_size);
                uhm_tls_cache_adapt_after_hit(heap, cache);
                return uhm_slab_slot_payload(slot);
            }
            link = &slot->next_free;
        }
    }
    ++cache->miss_count;
    return 0;
}

static int uhm_tls_cache_can_accept(uhm_heap *heap, uhm_tls_cache *cache,
                                    const uhm_slab_slot *slot,
                                    size_t *out_retain_token) {
    uhm_slab *slab;
    size_t slot_bytes;

    if (!heap || !cache || !slot ||
        !uhm_slab_slot_owner_from_heap(heap, slot, 0) ||
        cache->local_cache_limit == 0) {
        return 0;
    }
    if (out_retain_token) {
        *out_retain_token = SIZE_MAX;
    }
    slab = uhm_slab_slot_owner(slot);
    slot_bytes = uhm_slab_slot_owner_slot_size_from_heap(heap, slot, 0);
    if (slot_bytes == 0 || slot_bytes > cache->local_cache_limit) {
        return 0;
    }
    if (cache->local_cache_bytes <= cache->local_cache_limit - slot_bytes &&
        uhm_local_cache_can_retain_slab(
            heap, cache->local_cache_heads, cache->local_retained_hints,
            cache->retained_slab_pages,
            cache->retained_slab_page_limit, slab, out_retain_token)) {
        return 1;
    }
    if (cache->local_cache_bytes >
        cache->local_cache_limit - slot_bytes) {
        uhm_tls_cache_adapt_after_overflow(heap, cache);
    } else {
        uhm_tls_cache_adapt_after_retained_page_pressure(heap, cache);
    }
    return cache->local_cache_limit >= slot_bytes &&
           cache->local_cache_bytes <= cache->local_cache_limit - slot_bytes &&
           uhm_local_cache_can_retain_slab(
               heap, cache->local_cache_heads, cache->local_retained_hints,
               cache->retained_slab_pages,
               cache->retained_slab_page_limit, slab, out_retain_token);
}

static int uhm_tls_cache_can_accept_known_slot_size(uhm_heap *heap,
                                                    uhm_tls_cache *cache,
                                                    uhm_slab *slab,
                                                    size_t slot_bytes,
                                                    size_t *out_retain_token) {
    if (!heap || !cache || slot_bytes == 0 ||
        slot_bytes > cache->local_cache_limit) {
        return 0;
    }
    if (out_retain_token) {
        *out_retain_token = SIZE_MAX;
    }
    if (cache->local_cache_bytes <= cache->local_cache_limit - slot_bytes &&
        uhm_local_cache_can_retain_slab(
            heap, cache->local_cache_heads, cache->local_retained_hints,
            cache->retained_slab_pages,
            cache->retained_slab_page_limit, slab, out_retain_token)) {
        return 1;
    }
    if (cache->local_cache_bytes >
        cache->local_cache_limit - slot_bytes) {
        uhm_tls_cache_adapt_after_overflow(heap, cache);
    } else {
        uhm_tls_cache_adapt_after_retained_page_pressure(heap, cache);
    }
    return cache->local_cache_limit >= slot_bytes &&
           cache->local_cache_bytes <= cache->local_cache_limit - slot_bytes &&
           uhm_local_cache_can_retain_slab(
               heap, cache->local_cache_heads, cache->local_retained_hints,
               cache->retained_slab_pages,
               cache->retained_slab_page_limit, slab, out_retain_token);
}

static uhm_result uhm_tls_cache_push(uhm_heap *heap, uhm_tls_cache *cache,
                                     uhm_slab_slot *slot) {
    uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
    uhm_slab_slot **link;
    size_t requested;
    size_t slot_bytes;
    size_t retain_token = SIZE_MAX;
    uint32_t flags;
    uint32_t requested32;

    if (!heap || !cache || !slot || !slab ||
        slab->magic != UHM_SLAB_MAGIC ||
        !uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested32) ||
        !uhm_tls_cache_can_accept(heap, cache, slot,
                                  &retain_token)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    link = uhm_tls_cache_bucket(cache, slab->size_class);
    if (!link) {
        return UHM_ERR_CORRUPTION;
    }
    requested = requested32;
    slot_bytes = slab->slot_size;
    uhm_slab_raw_lock(slab);
    if (!uhm_slab_slot_live_state_mirror_valid_with_slab(
            slot, slab, &flags, &requested32)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    requested = requested32;
    if (!uhm_cached_requested_bytes_can_add(&cache->cached_requested_bytes,
                                            requested)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    if (!uhm_slab_slot_state_cas(
            slot, flags, (uint32_t)requested,
            UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_CTX_CACHE, 0u)) {
        uhm_slab_raw_unlock(slab);
        return UHM_ERR_CORRUPTION;
    }
    uhm_cached_requested_bytes_add_unchecked(&cache->cached_requested_bytes,
                                             requested);
    slot->requested_size = (uint32_t)requested;
    uhm_local_cache_note_slab_push(
        heap, cache->local_cache_heads, cache->local_retained_hints,
        &cache->retained_slab_pages, slab, retain_token);
    slot->next_free = *link;
    *link = slot;
    if (!slot->next_free) {
        cache->nonempty_class_mask |= UINT32_C(1) << slab->size_class;
    }
    cache->local_cache_head = slot;
    cache->local_cache_bytes += slot_bytes;
    ++cache->local_cache_count;
    ++cache->push_count;
    ++slab->ctx_cached_slots;
    uhm_slab_raw_unlock(slab);
    return UHM_OK;
}

static int uhm_tls_cache_push_partial_lowlock(uhm_heap *heap,
                                              uhm_tls_cache *cache,
                                              void *ptr) {
    uhm_metadata_reader_state *reader = 0;
    uhm_slab_slot *slot;
    uhm_slab *slab = 0;
    uhm_page_record *record_ref = 0;
    uhm_slab_slot **link;
    uint32_t flags = 0u;
    uint32_t requested32 = 0u;
    size_t requested;
    size_t slot_bytes;
    size_t retain_token = SIZE_MAX;
    int handled = 0;

    if (!heap || !cache || !ptr) {
        return 0;
    }
    slot = 0;
    if (cache->local_cache_count != 0u) {
        slot = uhm_local_cache_push_candidate_from_hint(
            heap, cache->local_page_hints, ptr, &slab, &record_ref,
            &flags, &requested32);
    }
    if (!slot) {
        if (!uhm_metadata_reader_begin_unregistered(&cache->metadata_reader,
                                                    heap)) {
            return 0;
        }
        reader = &cache->metadata_reader;
        slot = uhm_local_cache_push_candidate_likely(
            heap, reader, ptr, &slab, &record_ref, &flags,
            &requested32);
    }
    if (!slot ||
        !uhm_tls_cache_can_accept_known_slot_size(
            heap, cache, slab, slab->slot_size, &retain_token)) {
        if (reader) {
            uhm_lockless_remote_reader_release(reader);
        }
        return 0;
    }
    link = uhm_tls_cache_bucket(cache, slab->size_class);
    if (!link) {
        if (reader) {
            uhm_lockless_remote_reader_release(reader);
        }
        return 0;
    }
    slot_bytes = slab->slot_size;
    requested = requested32;
    if (uhm_cached_requested_bytes_can_add(
            &cache->cached_requested_bytes, requested) &&
        uhm_slab_slot_state_cas(
            slot, flags, (uint32_t)requested,
            UHM_SLAB_SLOT_FLAG_FREE | UHM_SLAB_SLOT_FLAG_CTX_CACHE,
            0u)) {
        uhm_cached_requested_bytes_add_unchecked(
            &cache->cached_requested_bytes, requested);
        slot->requested_size = (uint32_t)requested;
        uhm_local_cache_note_slab_push(
            heap, cache->local_cache_heads, cache->local_retained_hints,
            &cache->retained_slab_pages, slab, retain_token);
        slot->next_free = *link;
        *link = slot;
        if (!slot->next_free) {
            cache->nonempty_class_mask |=
                UINT32_C(1) << slab->size_class;
        }
        cache->local_cache_head = slot;
        cache->local_cache_bytes += slot_bytes;
        ++cache->local_cache_count;
        ++cache->push_count;
        (void)atomic_fetch_add_explicit(
            &slab->ctx_cached_slots, 1u, UHM_ATOMIC_ACQ_REL);
        uhm_local_page_hint_note(heap, cache->local_page_hints,
                                 slot, slab, record_ref);
        handled = 1;
    }
    if (reader) {
        uhm_lockless_remote_reader_release(reader);
    }
    return handled;
}

static uhm_result uhm_tls_cache_flush_slots_locked(uhm_heap *heap,
                                                   uhm_tls_cache *cache) {
    uhm_result result = UHM_OK;

    if (!heap || !cache) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    while (cache->local_cache_count != 0) {
        uhm_slab_slot **link = 0;
        uhm_slab_slot *slot;
        uint16_t size_class = UHM_SIZE_CLASS_COUNT;

        if (uhm_local_cache_find_fit_class(
                cache->nonempty_class_mask, 0u, &size_class) &&
            cache->local_cache_heads[size_class]) {
            link = &cache->local_cache_heads[size_class];
        }
        if (!link) {
            result = UHM_ERR_CORRUPTION;
            break;
        }
        slot = *link;
        uhm_slab *slab = uhm_slab_slot_owner_from_heap(heap, slot, 0);
        if (!slab || slab->magic != UHM_SLAB_MAGIC) {
            result = UHM_ERR_CORRUPTION;
            break;
        }
        uhm_slab_raw_lock(slab);
        {
            uint32_t cached_requested = 0;
            if (!uhm_slab_slot_ctx_cache_state_valid_with_slab(
                    slot, slab, &cached_requested) ||
                !uhm_cached_requested_bytes_can_sub(
                    &cache->cached_requested_bytes, cached_requested) ||
                slab->ctx_cached_slots == 0 || slab->live_slots == 0) {
                uhm_slab_raw_unlock(slab);
                result = UHM_ERR_CORRUPTION;
                break;
            }
            if (!uhm_slab_slot_state_cas(
                    slot,
                    UHM_SLAB_SLOT_FLAG_FREE |
                        UHM_SLAB_SLOT_FLAG_CTX_CACHE,
                    0u, UHM_SLAB_SLOT_FLAG_FREE, 0u)) {
                uhm_slab_raw_unlock(slab);
                result = UHM_ERR_CORRUPTION;
                break;
            }
            uhm_cached_requested_bytes_sub_unchecked(
                &cache->cached_requested_bytes, cached_requested);
            uhm_stats_malloc_live_sub(
                heap, (uintptr_t)uhm_slab_slot_payload(slot),
                cached_requested);
        }
        uhm_tls_cache_unlink_slot(heap, cache, link, slot, slab);
        slot->next_free = slab->free_head;
        slab->free_head = slot;
        --slab->ctx_cached_slots;
        --slab->live_slots;
        ++slab->free_slots;
        uhm_slab_refresh_state_raw_locked(heap, slab);
        uhm_slab_raw_unlock(slab);
    }
    return result;
}

static uhm_result uhm_tls_cache_flush_locked(uhm_heap *heap,
                                             uhm_tls_cache *cache) {
    uhm_result result;

    result = uhm_tls_cache_flush_slots_locked(heap, cache);
    if (result == UHM_OK) {
        result = uhm_slab_drain_remote_all(
            heap, !uhm_lockless_remote_free_enabled(heap));
    }
    if (result == UHM_OK) {
        result = uhm_slab_maybe_decommit_empty_all(heap);
    }
    return result;
}

static uhm_result uhm_tls_cache_evict_slab_locked(uhm_heap *heap,
                                                   uhm_tls_cache *cache,
                                                   uhm_slab *victim) {
    size_t i;

    if (!heap || !cache || !victim || victim->magic != UHM_SLAB_MAGIC) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_slab_raw_lock(victim);
    for (i = 0; i < UHM_SIZE_CLASS_COUNT; ++i) {
        uhm_slab_slot **link = &cache->local_cache_heads[i];

        while (*link) {
            uhm_slab_slot *slot = *link;
            uint32_t cached_requested = 0;

            if (uhm_slab_slot_owner(slot) != victim) {
                link = &slot->next_free;
                continue;
            }
            if (!uhm_slab_slot_ctx_cache_state_valid_with_slab(
                    slot, victim, &cached_requested) ||
                !uhm_cached_requested_bytes_can_sub(
                    &cache->cached_requested_bytes, cached_requested) ||
                victim->ctx_cached_slots == 0 || victim->live_slots == 0 ||
                !uhm_slab_slot_state_cas(
                    slot,
                    UHM_SLAB_SLOT_FLAG_FREE |
                        UHM_SLAB_SLOT_FLAG_CTX_CACHE,
                    0u, UHM_SLAB_SLOT_FLAG_FREE, 0u)) {
                uhm_slab_raw_unlock(victim);
                return UHM_ERR_CORRUPTION;
            }
            uhm_cached_requested_bytes_sub_unchecked(
                &cache->cached_requested_bytes, cached_requested);
            uhm_stats_malloc_live_sub(
                heap, (uintptr_t)uhm_slab_slot_payload(slot),
                cached_requested);
            uhm_tls_cache_unlink_slot(heap, cache, link, slot, victim);
            slot->next_free = victim->free_head;
            victim->free_head = slot;
            --victim->ctx_cached_slots;
            --victim->live_slots;
            ++victim->free_slots;
        }
    }
    uhm_slab_refresh_state_raw_locked(heap, victim);
    uhm_slab_raw_unlock(victim);
    return uhm_slab_maybe_decommit_empty(heap, victim);
}

static uhm_result uhm_tls_cache_evict_cold_slab_locked(
    uhm_heap *heap,
    uhm_tls_cache *cache,
    const uhm_slab *incoming) {
    uhm_slab *victim = uhm_local_cache_cold_slab_excluding(
        cache ? cache->local_cache_heads : 0, incoming);

    if (!victim) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    return uhm_tls_cache_evict_slab_locked(heap, cache, victim);
}

static void uhm_tls_cache_note_idle_miss(uhm_heap *heap,
                                         uhm_tls_cache *cache) {
    uhm_result result = UHM_OK;
    unsigned state;
    unsigned streak;
    unsigned hit_tag;
    unsigned current_hit_tag;

    if (!heap || !cache || cache->local_cache_count == 0u) {
        return;
    }
    state = atomic_load_explicit(&cache->idle_miss_streak,
                                 UHM_ATOMIC_RELAXED);
    streak = state & UHM_LOCAL_CACHE_IDLE_STREAK_MASK;
    hit_tag = state >> UHM_LOCAL_CACHE_IDLE_HIT_TAG_SHIFT;
    current_hit_tag =
        (unsigned)cache->hit_count & UHM_LOCAL_CACHE_IDLE_HIT_TAG_MASK;
    if (hit_tag != current_hit_tag) {
        hit_tag = current_hit_tag;
        streak = 0u;
    }
    if (streak + 1u < UHM_LOCAL_CACHE_IDLE_MISS_THRESHOLD) {
        atomic_store_explicit(
            &cache->idle_miss_streak,
            (hit_tag << UHM_LOCAL_CACHE_IDLE_HIT_TAG_SHIFT) |
                (streak + 1u),
            UHM_ATOMIC_RELAXED);
        return;
    }
    atomic_store_explicit(
        &cache->idle_miss_streak,
        hit_tag << UHM_LOCAL_CACHE_IDLE_HIT_TAG_SHIFT,
        UHM_ATOMIC_RELAXED);
    if (cache->retained_slab_pages <= cache->adaptive_pages) {
        return;
    }
    uhm_lock(&heap->lock);
    if (cache == uhm_tls_cache_current(heap) &&
        cache->retained_slab_pages > cache->adaptive_pages) {
        result = uhm_tls_cache_evict_cold_slab_locked(heap, cache, 0);
    }
    uhm_unlock(&heap->lock);
    if (result != UHM_OK && result != UHM_ERR_OUT_OF_MEMORY) {
        uhm_set_error(heap, result);
    }
}

static uhm_result uhm_tls_cache_flush_all_local_locked(
    uhm_heap *heap, int *maintenance_needed) {
    uhm_result result = UHM_OK;
    uhm_tls_cache *cache;

    if (maintenance_needed) {
        *maintenance_needed = 0;
    }
    if (!heap) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    for (cache = heap->core.tls_cache_head; cache; cache = cache->next_active) {
        uhm_result flush_result =
            uhm_tls_cache_flush_slots_locked(heap, cache);

        if (flush_result == UHM_OK && maintenance_needed) {
            *maintenance_needed = 1;
        }
        if (flush_result != UHM_OK && result == UHM_OK) {
            result = flush_result;
        }
    }
    return result;
}

static void uhm_tls_cache_detach_locked(uhm_heap *heap,
                                        uhm_tls_cache *cache) {
    if (!heap || !cache) {
        return;
    }
    if (cache->prev_active) {
        cache->prev_active->next_active = cache->next_active;
    } else if (heap->core.tls_cache_head == cache) {
        heap->core.tls_cache_head = cache->next_active;
    }
    if (cache->next_active) {
        cache->next_active->prev_active = cache->prev_active;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    heap->core.tls_cache_hits += cache->hit_count;
    heap->core.tls_cache_misses += cache->miss_count;
    heap->core.tls_cache_pushes += cache->push_count;
    heap->core.tls_cache_overflows += cache->overflow_count;
    heap->core.tls_cache_retained_page_pressure +=
        cache->retained_page_pressure_count;
#endif
    if (uhm_tls_get_value(heap) == cache) {
        (void)uhm_tls_set_value(heap, 0);
    }
    if (heap->core.tls_cache_active_count != 0) {
        --heap->core.tls_cache_active_count;
    }
    atomic_store_explicit(&cache->lifecycle_state, 0u, UHM_ATOMIC_RELEASE);
    atomic_store_explicit(&cache->owner_ref, (uintptr_t)0,
                          UHM_ATOMIC_RELEASE);
    uhm_metadata_reader_clear(&cache->metadata_reader);
    cache->magic = 0;
    cache->owner = 0;
    cache->owner_thread_id = 0;
    cache->local_cache_head = 0;
    memset(cache->local_cache_heads, 0, sizeof(cache->local_cache_heads));
    cache->nonempty_class_mask = 0u;
    memset(cache->local_page_hints, 0, sizeof(cache->local_page_hints));
    memset(cache->local_retained_hints, 0,
           sizeof(cache->local_retained_hints));
    cache->local_cache_bytes = 0;
    cache->local_cache_count = 0;
    cache->retained_slab_pages = 0;
    cache->retained_slab_page_limit = 0;
    atomic_store_explicit(&cache->cached_requested_bytes, 0u,
                          UHM_ATOMIC_RELEASE);
    cache->adaptive_pages = 0;
    atomic_store_explicit(&cache->idle_miss_streak, 0u,
                          UHM_ATOMIC_RELEASE);
    cache->retained_page_pressure_count = 0;
    cache->prev_active = 0;
    cache->next_active = 0;
    cache->next_free = heap->tls_cache_free_head;
    heap->tls_cache_free_head = cache;
}

static void uhm_tls_cache_detach_all_locked(uhm_heap *heap) {
    if (!heap) {
        return;
    }
    while (heap->core.tls_cache_head) {
        uhm_tls_cache_detach_locked(heap, heap->core.tls_cache_head);
    }
}

static void uhm_tls_cache_destructor_common(void *value) {
    uhm_tls_cache *cache = (uhm_tls_cache *)value;
    uhm_heap *heap;
    uhm_result result;
    size_t op_shard;

    if (!cache ||
        atomic_load_explicit(&cache->lifecycle_state, UHM_ATOMIC_ACQUIRE) !=
            UHM_TLS_CACHE_MAGIC) {
        return;
    }
    heap = (uhm_heap *)atomic_load_explicit(&cache->owner_ref,
                                            UHM_ATOMIC_ACQUIRE);
    if (!heap || !uhm_heap_op_begin(heap, &op_shard)) {
        return;
    }
    uhm_lock(&heap->lock);
    if (cache->magic == UHM_TLS_CACHE_MAGIC && cache->owner == heap) {
        result = uhm_tls_cache_flush_locked(heap, cache);
        uhm_tls_cache_detach_locked(heap, cache);
    } else {
        result = UHM_OK;
    }
    uhm_unlock(&heap->lock);
    if (result != UHM_OK) {
        uhm_set_error(heap, result);
    }
    uhm_heap_op_end(heap, op_shard);
}

static void *uhm_tls_malloc_pinned(uhm_heap *heap, size_t size) {
    size_t total;
    size_t payload_size;
    uint16_t size_class;
    uint16_t fit_size_class = UHM_SIZE_CLASS_COUNT;
    uhm_tls_cache *cache;
    uhm_tls_cache *candidate_cache;
    void *ptr = 0;

    if (!uhm_tls_cache_is_enabled(heap)) {
        return 0;
    }
    if (uhm_prepare_alloc_size(UHM_MIN_ALIGNMENT, size, &total,
                               &payload_size, &size_class) != UHM_OK) {
        return 0;
    }
    (void)total;
    cache = uhm_tls_cache_current(heap);
    if (!uhm_size_class_uses_slab(size_class, payload_size)) {
        uhm_tls_cache_note_idle_miss(heap, cache);
        return 0;
    }
    if (!cache || cache->local_cache_count == 0 ||
        !uhm_local_cache_find_fit_class(
            cache->nonempty_class_mask, size_class, &fit_size_class)) {
        uhm_tls_cache_note_idle_miss(heap, cache);
        return 0;
    }
    candidate_cache = cache;
    ptr = uhm_tls_cache_pop_partial_lowlock(heap, cache, fit_size_class,
                                            payload_size, size);
    if (ptr) {
        return ptr;
    }
    uhm_lock(&heap->lock);
    if (uhm_heap_is_accessible_pinned(heap)) {
        cache = uhm_tls_cache_current(heap);
        if (cache && cache->local_cache_count != 0) {
            if (cache != candidate_cache ||
                fit_size_class >= UHM_SIZE_CLASS_COUNT ||
                !cache->local_cache_heads[fit_size_class]) {
                (void)uhm_local_cache_find_fit_class(
                    cache->nonempty_class_mask, size_class,
                    &fit_size_class);
            }
            if (fit_size_class < UHM_SIZE_CLASS_COUNT) {
                ptr = uhm_tls_cache_pop(
                    heap, cache, fit_size_class, payload_size, size);
            }
        }
    }
    uhm_unlock(&heap->lock);
    if (!ptr) {
        uhm_tls_cache_note_idle_miss(heap, cache);
    }
    return ptr;
}

static int uhm_tls_free_pinned(uhm_heap *heap, void *ptr) {
    uhm_tls_cache *cache = 0;
    uhm_slab_slot *slot = 0;
    uhm_result result = UHM_OK;
    int handled = 0;

    if (!uhm_tls_cache_is_enabled(heap) || !ptr) {
        return 0;
    }
    cache = uhm_tls_cache_current(heap);
    if (cache && uhm_tls_cache_push_partial_lowlock(heap, cache, ptr)) {
        return 1;
    }
    uhm_lock(&heap->lock);
    if (uhm_heap_is_accessible_pinned(heap)) {
        cache = uhm_tls_cache_get_or_create_locked(heap);
        if (cache) {
            slot = uhm_find_slab_slot_by_payload(heap, ptr, 1);
            if (slot) {
                uhm_slab *slab = uhm_slab_slot_owner(slot);
                size_t slot_bytes = slab ? slab->slot_size : 0u;
                int can_accept =
                    uhm_tls_cache_can_accept(heap, cache, slot, 0);

                if (!can_accept && slab && slot_bytes != 0u &&
                    slot_bytes <= cache->local_cache_limit &&
                    cache->local_cache_bytes <=
                        cache->local_cache_limit - slot_bytes &&
                    uhm_tls_cache_evict_cold_slab_locked(
                        heap, cache, slab) == UHM_OK) {
                    can_accept =
                        uhm_tls_cache_can_accept(heap, cache, slot, 0);
                }
                if (can_accept) {
                    result = uhm_tls_cache_push(heap, cache, slot);
                    handled = 1;
                }
            }
        }
    }
    uhm_unlock(&heap->lock);
    if (handled && result != UHM_OK) {
        uhm_set_error(heap, result);
    }
    return handled;
}

