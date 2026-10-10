static size_t test_slab_slot_header_size(void) {
    return (size_t)test_align_up_uintptr(sizeof(uhm_slab_slot), UHM_MIN_ALIGNMENT);
}

static uhm_slab_slot *test_slab_slot_from_payload(void *ptr) {
    return (uhm_slab_slot *)((uintptr_t)ptr - test_slab_slot_header_size());
}

static size_t test_local_page_hint_set_base(size_t page_index) {
    return (page_index & (UHM_LOCAL_PAGE_HINT_SET_COUNT - 1u)) *
           UHM_LOCAL_PAGE_HINT_WAYS;
}

static uint64_t test_slab_slot_state_pack(uint32_t flags,
                                          uint32_t requested_size) {
    return ((uint64_t)requested_size << 32) | (uint64_t)flags;
}

static uint32_t test_slab_slot_state_flags_load(const uhm_slab_slot *slot) {
    uint64_t state_word =
        slot ? atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE) : 0u;
    return (uint32_t)(state_word & UINT64_C(0xffffffff));
}

static uint32_t test_slab_slot_state_requested_load(
    const uhm_slab_slot *slot) {
    uint64_t state_word =
        slot ? atomic_load_explicit(&slot->state_word, UHM_ATOMIC_ACQUIRE) : 0u;
    return (uint32_t)(state_word >> 32);
}

static uhm_slab_slot *test_slab_slot_from_remote_link(
    uhm_tagged_freelist_node *link) {
    uintptr_t link_addr = (uintptr_t)link;

    if (!link || link_addr < offsetof(uhm_slab_slot, remote_link)) {
        return 0;
    }
    return (uhm_slab_slot *)(link_addr - offsetof(uhm_slab_slot, remote_link));
}

static int test_slab_remote_queue_empty(uhm_slab *slab) {
    uintptr_t head;
    size_t pending;
    size_t pending_mirror;

    if (!slab) {
        return 1;
    }
    pending_mirror = atomic_load_explicit(&slab->remote_free_slots,
                                          UHM_ATOMIC_ACQUIRE);
    pending = atomic_load_explicit(&slab->remote_free_pending,
                                   UHM_ATOMIC_ACQUIRE);
    head = atomic_load_explicit(&slab->remote_free_lf.head,
                                UHM_ATOMIC_ACQUIRE);
    return pending_mirror == 0 &&
           pending == 0 &&
           slab->remote_free_head == 0 &&
           uhm_tagged_freelist_unpack_ptr(head) == 0;
}

static size_t test_slab_remote_pending_mirror(uhm_slab *slab) {
    return slab ? atomic_load_explicit(&slab->remote_free_slots,
                                       UHM_ATOMIC_ACQUIRE)
                : 0u;
}

static size_t test_slab_remote_pending(uhm_slab *slab) {
    return slab ? atomic_load_explicit(&slab->remote_free_pending,
                                       UHM_ATOMIC_ACQUIRE)
                : 0u;
}

static size_t test_slab_remote_inflight_mirror(uhm_slab *slab) {
    return slab ? atomic_load_explicit(&slab->remote_enqueue_inflight_count,
                                       UHM_ATOMIC_ACQUIRE)
                : 0u;
}

static int test_slab_remote_queue_contains(uhm_slab *slab,
                                           uhm_slab_slot *target) {
    uhm_slab_slot *slot;
    uhm_tagged_freelist_node *link;
    size_t seen = 0;

    if (!slab || !target) {
        return 0;
    }
    for (slot = slab->remote_free_head; slot; slot = slot->next_free) {
        if (++seen > slab->total_slots) {
            return 0;
        }
        if (slot == target) {
            return 1;
        }
    }
    link = uhm_tagged_freelist_unpack_ptr(
        atomic_load_explicit(&slab->remote_free_lf.head,
                             UHM_ATOMIC_ACQUIRE));
    while (link) {
        slot = test_slab_slot_from_remote_link(link);
        if (++seen > slab->total_slots || !slot) {
            return 0;
        }
        if (slot == target) {
            return 1;
        }
        link = uhm_tagged_freelist_next_load(link);
    }
    return 0;
}

static int test_slab_remote_class_queue_contains(uhm_heap *heap,
                                                 uhm_slab *slab,
                                                 uhm_slab_slot *target) {
    uhm_tagged_freelist_node *link;
    size_t seen = 0;

    if (!heap || !slab || !target ||
        slab->size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    link = uhm_tagged_freelist_unpack_ptr(atomic_load_explicit(
        &heap->core.slab_remote_class_lf[slab->size_class].queue.head,
        UHM_ATOMIC_ACQUIRE));
    while (link) {
        uhm_slab_slot *slot = test_slab_slot_from_remote_link(link);
        if (++seen > slab->total_slots || !slot) {
            return 0;
        }
        if (slot == target) {
            return 1;
        }
        link = uhm_tagged_freelist_next_load(link);
    }
    return 0;
}

static int test_slab_remote_ready_queue_contains(uhm_heap *heap,
                                                 uhm_slab *target) {
    uhm_tagged_freelist_node *link;
    size_t seen = 0;

    if (!heap || !target || target->size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    link = uhm_tagged_freelist_unpack_ptr(atomic_load_explicit(
        &heap->core.slab_remote_ready_lf[target->size_class].queue.head,
        UHM_ATOMIC_ACQUIRE));
    while (link) {
        if (++seen > heap->core.page_count) {
            return 0;
        }
        if (link == &target->remote_ready_link) {
            return 1;
        }
        link = uhm_tagged_freelist_next_load(link);
    }
    return 0;
}

static uhm_slab **test_slab_state_head(uhm_heap *heap, uint16_t size_class,
                                       uint16_t state) {
    if (!heap || size_class >= UHM_SIZE_CLASS_COUNT) {
        return 0;
    }
    if (state == UHM_SLAB_STATE_PARTIAL) {
        return &heap->core.slab_class_heads[size_class];
    }
    if (state == UHM_SLAB_STATE_FULL) {
        return &heap->core.slab_full_heads[size_class];
    }
    if (state == UHM_SLAB_STATE_EMPTY) {
        return &heap->core.slab_empty_heads[size_class];
    }
    return 0;
}

static void TEST_MAYBE_UNUSED test_slab_move_state_list(uhm_heap *heap,
                                                        uhm_slab *slab,
                                                        uint16_t state) {
    uhm_slab **old_head;
    uhm_slab **new_head;

    if (!heap || !slab || slab->size_class >= UHM_SIZE_CLASS_COUNT ||
        slab->state == state) {
        return;
    }
    old_head = test_slab_state_head(heap, slab->size_class, slab->state);
    new_head = test_slab_state_head(heap, slab->size_class, state);
    if (!old_head || !new_head) {
        return;
    }
    if (slab->state == UHM_SLAB_STATE_EMPTY &&
        (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
        slab->ctx_cached_slots == 0 &&
        heap->core.slab_committed_empty_count[slab->size_class] != 0) {
        --heap->core.slab_committed_empty_count[slab->size_class];
    }
    if (slab->prev_class) {
        slab->prev_class->next_class = slab->next_class;
    } else if (*old_head == slab) {
        *old_head = slab->next_class;
    }
    if (slab->next_class) {
        slab->next_class->prev_class = slab->prev_class;
    }
    if (slab->state == UHM_SLAB_STATE_EMPTY &&
        heap->core.slab_empty_count[slab->size_class] != 0) {
        --heap->core.slab_empty_count[slab->size_class];
    }
    slab->state = state;
    if (state == UHM_SLAB_STATE_EMPTY) {
        ++heap->core.slab_empty_count[slab->size_class];
        if ((slab->flags & UHM_SLAB_FLAG_DECOMMITTED) == 0 &&
            slab->ctx_cached_slots == 0) {
            ++heap->core.slab_committed_empty_count[slab->size_class];
        }
    }
    slab->prev_class = 0;
    slab->next_class = *new_head;
    if (*new_head) {
        (*new_head)->prev_class = slab;
    }
    *new_head = slab;
    atomic_store_explicit(&slab->state_mirror, (uint32_t)state,
                          UHM_ATOMIC_RELEASE);
}

static uhm_slab *test_find_slab_by_payload(uhm_heap *heap, void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    size_t header = test_slab_slot_header_size();
    uhm_slab *slab;

    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        uintptr_t end = slab->base + slab->size;
        size_t offset;
        if (slab->magic != UHM_SLAB_MAGIC || addr < slab->base || addr >= end) {
            continue;
        }
        offset = (size_t)(addr - slab->base);
        if ((offset % slab->slot_stride) == header) {
            return slab;
        }
    }
    return 0;
}

static uhm_block *test_find_block_by_payload(uhm_heap *heap, void *ptr) {
    uhm_block *block;

    for (block = heap->core.first_block; block; block = block->next_phys) {
        if (block->magic == UHM_BLOCK_MAGIC &&
            test_block_payload(block) == ptr) {
            return block;
        }
    }
    return 0;
}

static int test_shadow_page_span(uhm_heap *heap, uintptr_t addr, size_t size,
                                 size_t *first_out, size_t *count_out) {
    uintptr_t end = addr + size;
    uintptr_t aligned_end;
    size_t first;
    size_t count;

    if (!heap || size == 0 || end < addr ||
        addr < heap->core.arena_base || end > heap->core.arena_end) {
        return 0;
    }
    aligned_end = test_align_up_uintptr(end - heap->core.arena_base,
                                        heap->F.vm.page_size);
    first = (size_t)(addr - heap->core.arena_base) / heap->F.vm.page_size;
    count = aligned_end / heap->F.vm.page_size - first;
    *first_out = first;
    *count_out = count;
    return count != 0 && first + count <= heap->core.page_count;
}

static uhm_page_record *test_page_record_at(uhm_heap *heap, size_t page_index) {
    size_t base_index = page_index - (page_index % UHM_PAGE_LEAF_PAGES);
    size_t leaf_index = base_index / UHM_PAGE_LEAF_PAGES;
    size_t shard = leaf_index % UHM_PAGE_MAP_SHARDS;
    uhm_page_leaf *leaf;

    for (leaf = heap->core.page_map[shard]; leaf; leaf = leaf->next) {
        if (leaf->base_index == base_index) {
            return &leaf->records[page_index - leaf->base_index];
        }
        if (leaf->base_index > base_index) {
            break;
        }
    }
    return 0;
}

static void test_shadow_mark_range(uhm_heap *heap, test_shadow_page *shadow,
                                   uintptr_t addr, size_t size,
                                   uint32_t owner, uint32_t va_state,
                                   uint32_t commit_state, uint32_t prot,
                                   uint64_t descriptor_id, uint64_t generation,
                                   uint32_t flags, const char *message) {
    size_t first;
    size_t count;
    size_t i;

    expect_true(test_shadow_page_span(heap, addr, size, &first, &count), message);
    for (i = 0; i < count; ++i) {
        test_shadow_page *page = &shadow[first + i];
        if ((page->flags & TEST_SHADOW_PRESENT) != 0 &&
            page->owner != owner &&
            owner != UHM_PAGE_OWNER_MALLOC &&
            page->owner != UHM_PAGE_OWNER_MALLOC) {
            expect_true(0, "shadow owner overlap");
        }
        page->flags = TEST_SHADOW_PRESENT | flags;
        page->owner = owner;
        page->va_state = va_state;
        page->commit_state = commit_state;
        page->prot = prot;
        page->descriptor_id = descriptor_id;
        page->generation = generation;
    }
}

static int test_record_is_zero_free(const uhm_page_record *record) {
    return !record ||
           (record->owner == UHM_PAGE_OWNER_FREE &&
            record->va_state == UHM_PAGE_VA_FREE_RESERVED &&
            record->commit_state == UHM_PAGE_COMMIT_NONE &&
            record->prot == UHM_PROT_NONE &&
            record->descriptor_id == 0 &&
            record->generation == 0);
}

static int test_record_is_flexible_free(const uhm_page_record *record) {
    return test_record_is_zero_free(record) ||
           (record->owner == UHM_PAGE_OWNER_FREE &&
            (record->va_state == UHM_PAGE_VA_FREE_RESERVED ||
             record->va_state == UHM_PAGE_VA_FREE_REUSABLE) &&
            (record->commit_state == UHM_PAGE_COMMIT_NONE ||
             record->commit_state == UHM_PAGE_COMMIT_DECOMMITTED) &&
            record->prot == UHM_PROT_NONE &&
            record->descriptor_id == 0);
}

static void test_shadow_model_check(uhm_heap *heap, const char *message) {
    test_shadow_page *shadow;
    uhm_control_extent *control_extent;
    uhm_block *block;
    uhm_slab *slab;
    uhm_vma *vma;
    uhm_free_extent *free_extent;
    size_t i;

    expect_true(heap != 0 && heap->magic == UHM_MAGIC, message);
    shadow = (test_shadow_page *)calloc(heap->core.page_count, sizeof(*shadow));
    expect_true(shadow != 0, "shadow allocation");

    test_shadow_mark_range(heap, shadow, heap->core.control_base,
                           (size_t)(heap->core.control_end - heap->core.control_base),
                           UHM_PAGE_OWNER_CONTROL, UHM_PAGE_VA_CONTROL_LIVE,
                           UHM_PAGE_COMMIT_LIVE, UHM_PROT_READ | UHM_PROT_WRITE,
                           heap->core.primary_control_desc.id,
                           heap->core.primary_control_desc.generation,
                           TEST_SHADOW_EXACT_PROT |
                               TEST_SHADOW_EXACT_DESC |
                               TEST_SHADOW_EXACT_GENERATION,
                           "shadow primary control");

    for (control_extent = heap->core.control_extents; control_extent;
         control_extent = control_extent->next) {
        if (control_extent->magic == UHM_CONTROL_MAGIC) {
            test_shadow_mark_range(heap, shadow, control_extent->base,
                                   (size_t)(control_extent->end - control_extent->base),
                                   UHM_PAGE_OWNER_CONTROL, UHM_PAGE_VA_CONTROL_LIVE,
                                   UHM_PAGE_COMMIT_LIVE,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   control_extent->desc.id,
                                   control_extent->desc.generation,
                                   TEST_SHADOW_EXACT_PROT |
                                       TEST_SHADOW_EXACT_DESC |
                                       TEST_SHADOW_EXACT_GENERATION,
                                   "shadow control extent");
        }
    }

    for (block = heap->core.first_block; block; block = block->next_phys) {
        if (block->magic == UHM_BLOCK_MAGIC) {
            uintptr_t decommit_addr;
            size_t decommit_size;
            test_shadow_mark_range(heap, shadow, (uintptr_t)block, block->total_size,
                                   UHM_PAGE_OWNER_MALLOC, UHM_PAGE_VA_MALLOC_LIVE,
                                   UHM_PAGE_COMMIT_LIVE,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   0, 0, 0, "shadow malloc block");
            if ((block->flags & UHM_BLOCK_FLAG_DECOMMITTED) != 0 &&
                test_block_decommit_range(heap, block, &decommit_addr,
                                          &decommit_size)) {
                test_shadow_mark_range(heap, shadow, decommit_addr, decommit_size,
                                       UHM_PAGE_OWNER_MALLOC,
                                       UHM_PAGE_VA_MALLOC_LIVE,
                                       UHM_PAGE_COMMIT_DECOMMITTED,
                                       UHM_PROT_NONE,
                                       block->desc.id, block->generation,
                                       TEST_SHADOW_EXACT_PROT,
                                       "shadow malloc decommitted free extent");
            }
            if ((block->flags & UHM_BLOCK_FLAG_PAGE_PRIVATE) != 0 &&
                (block->flags & UHM_BLOCK_FLAG_FREE) == 0 &&
                block->requested_size != 0) {
                test_shadow_mark_range(heap, shadow, (uintptr_t)test_block_payload(block),
                                       block->requested_size,
                                       UHM_PAGE_OWNER_MALLOC,
                                       UHM_PAGE_VA_MALLOC_LIVE,
                                       UHM_PAGE_COMMIT_LIVE,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       block->desc.id, 0,
                                       TEST_SHADOW_EXACT_DESC,
                                       "shadow page-private malloc");
            }
        }
    }

    for (slab = heap->core.slab_all_head; slab; slab = slab->next_all) {
        if (slab->magic == UHM_SLAB_MAGIC) {
            uint32_t commit_state =
                (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0
                    ? UHM_PAGE_COMMIT_DECOMMITTED
                    : UHM_PAGE_COMMIT_LIVE;
            uint32_t prot = (slab->flags & UHM_SLAB_FLAG_DECOMMITTED) != 0
                                ? UHM_PROT_NONE
                                : (UHM_PROT_READ | UHM_PROT_WRITE);
            test_shadow_mark_range(heap, shadow, slab->base, slab->size,
                                   UHM_PAGE_OWNER_MALLOC,
                                   UHM_PAGE_VA_MALLOC_LIVE,
                                   commit_state,
                                   prot,
                                   slab->desc.id, slab->generation,
                                   TEST_SHADOW_EXACT_PROT |
                                       TEST_SHADOW_EXACT_DESC |
                                       TEST_SHADOW_EXACT_GENERATION,
                                   "shadow malloc slab");
        }
    }

    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        if (vma->magic == UHM_VMA_MAGIC) {
            test_shadow_mark_range(heap, shadow, vma->addr, vma->size,
                                   UHM_PAGE_OWNER_MMAP, UHM_PAGE_VA_MMAP_LIVE,
                                   (vma->flags & UHM_MAP_NO_COMMIT) != 0
                                       ? UHM_PAGE_COMMIT_DECOMMITTED
                                       : UHM_PAGE_COMMIT_LIVE,
                                   vma->prot,
                                   vma->desc.id, vma->generation,
                                   TEST_SHADOW_EXACT_PROT |
                                       TEST_SHADOW_EXACT_DESC |
                                       TEST_SHADOW_EXACT_GENERATION,
                                   "shadow mmap VMA");
        }
    }

    for (free_extent = heap->core.mmap_free_head; free_extent;
         free_extent = free_extent->next) {
        if (free_extent->magic == UHM_FREE_MAGIC) {
            test_shadow_mark_range(heap, shadow, free_extent->addr, free_extent->size,
                                   UHM_PAGE_OWNER_FREE, UHM_PAGE_VA_FREE_REUSABLE,
                                   UHM_PAGE_COMMIT_DECOMMITTED, UHM_PROT_NONE,
                                   0, 0, TEST_SHADOW_EXACT_PROT |
                                             TEST_SHADOW_FREE_FLEXIBLE,
                                   "shadow mmap free extent");
        }
    }

    for (i = 0; i < heap->core.page_count; ++i) {
        uhm_page_record *record = test_page_record_at(heap, i);
        test_shadow_page *expected = &shadow[i];
        if ((expected->flags & TEST_SHADOW_PRESENT) == 0) {
            expect_true(test_record_is_zero_free(record), "shadow free reserved page");
            continue;
        }
        if ((expected->flags & TEST_SHADOW_FREE_FLEXIBLE) != 0) {
            expect_true(test_record_is_flexible_free(record), "shadow flexible free extent");
            continue;
        }
        expect_true(record != 0, "shadow page record exists");
        expect_true(record->owner == expected->owner, "shadow owner");
        expect_true(record->va_state == expected->va_state, "shadow va state");
        expect_true(record->commit_state == expected->commit_state, "shadow commit state");
        if ((expected->flags & TEST_SHADOW_EXACT_PROT) != 0) {
            expect_true(record->prot == expected->prot, "shadow prot");
        }
        if ((expected->flags & TEST_SHADOW_EXACT_DESC) != 0) {
            expect_true(record->descriptor_id == expected->descriptor_id,
                        "shadow descriptor");
        }
        if ((expected->flags & TEST_SHADOW_EXACT_GENERATION) != 0) {
            expect_true(record->generation == expected->generation,
                        "shadow generation exact");
        } else {
            expect_true(record->generation != 0, "shadow generation nonzero");
        }
    }

    free(shadow);
}

static size_t test_vma_count_overlapping(uhm_heap *heap, uintptr_t addr,
                                         size_t size) {
    uintptr_t end = addr + size;
    size_t count = 0;
    uhm_vma *vma;

    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        uintptr_t vma_end = vma->addr + vma->size;
        if (addr < vma_end && end > vma->addr) {
            ++count;
        }
    }
    return count;
}

static size_t test_vma_count_all(uhm_heap *heap) {
    size_t count = 0;
    uhm_vma *vma;

    for (vma = heap ? heap->core.vma_head : 0; vma; vma = vma->next) {
        ++count;
    }
    return count;
}

static int test_has_exact_vma(uhm_heap *heap, uintptr_t addr, size_t size,
                              uint32_t prot) {
    uhm_vma *vma;

    for (vma = heap->core.vma_head; vma; vma = vma->next) {
        if (vma->addr == addr && vma->size == size && vma->prot == prot) {
            return 1;
        }
    }
    return 0;
}

static int test_uintptr_range_overflows(uintptr_t addr, size_t size,
                                        uintptr_t *end_out) {
    if (size > (size_t)(UINTPTR_MAX - addr)) {
        return 1;
    }
    if (end_out) {
        *end_out = addr + (uintptr_t)size;
    }
    return 0;
}

static uhm_vma *test_find_vma_containing(uhm_heap *heap, const void *addr) {
    uintptr_t target = (uintptr_t)addr;
    uhm_vma *vma;

    for (vma = heap ? heap->core.vma_head : 0; vma; vma = vma->next) {
        uintptr_t end;
        if (test_uintptr_range_overflows(vma->addr, vma->size, &end)) {
            continue;
        }
        if (target >= vma->addr && target < end) {
            return vma;
        }
    }
    return 0;
}

static uhm_free_extent *test_find_free_extent_containing(uhm_heap *heap,
                                                        const void *addr) {
    uintptr_t target = (uintptr_t)addr;
    uhm_free_extent *extent;

    for (extent = heap ? heap->core.mmap_free_head : 0; extent;
         extent = extent->next) {
        uintptr_t end;
        if (test_uintptr_range_overflows(extent->addr, extent->size, &end)) {
            continue;
        }
        if (target >= extent->addr && target < end) {
            return extent;
        }
    }
    return 0;
}

