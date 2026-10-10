static int uhm_retired_vma_list_validate(uhm_heap *heap, uhm_vma *head,
                                         uhm_vma *tail,
                                         int require_emergency,
                                         size_t *count_out) {
    uhm_vma *slow = head;
    uhm_vma *fast = head;
    uhm_vma *node;
    uhm_vma *last = 0;
    size_t count = 0;

    if (!heap || !count_out) {
        return 0;
    }
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return 0;
        }
    }
    for (node = head; node; node = node->next) {
        int emergency = (node->flags & UHM_META_FLAG_EMERGENCY) != 0;
        if (node->magic != 0 ||
            emergency != require_emergency ||
            node->prev != 0 ||
            node->tree_parent != 0 ||
            node->tree_left != 0 ||
            node->tree_right != 0 ||
            node->desc.magic != 0 ||
            node->desc.id != 0 ||
            node->retire_epoch > heap->core.metadata_epoch) {
            return 0;
        }
        last = node;
        ++count;
    }
    if ((head == 0) != (tail == 0) ||
        (tail && (tail->next != 0 || last != tail))) {
        return 0;
    }
    *count_out = count;
    return 1;
}

static int uhm_retired_free_extent_list_validate(
    uhm_heap *heap, uhm_free_extent *head, uhm_free_extent *tail,
    int require_emergency, size_t *count_out) {
    uhm_free_extent *slow = head;
    uhm_free_extent *fast = head;
    uhm_free_extent *node;
    uhm_free_extent *last = 0;
    size_t count = 0;

    if (!heap || !count_out) {
        return 0;
    }
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return 0;
        }
    }
    for (node = head; node; node = node->next) {
        uint32_t expected_flags =
            require_emergency ? UHM_META_FLAG_EMERGENCY : 0u;
        int emergency = (node->flags & UHM_META_FLAG_EMERGENCY) != 0;
        if (node->magic != 0 ||
            emergency != require_emergency ||
            node->flags != expected_flags ||
            node->tree_parent != 0 ||
            node->tree_left != 0 ||
            node->tree_right != 0 ||
            node->desc.magic != 0 ||
            node->desc.id != 0 ||
            node->retire_epoch > heap->core.metadata_epoch) {
            return 0;
        }
        last = node;
        ++count;
    }
    if ((head == 0) != (tail == 0) ||
        (tail && (tail->next != 0 || last != tail))) {
        return 0;
    }
    *count_out = count;
    return 1;
}

static int uhm_metadata_quarantine_validate(uhm_heap *heap) {
    uhm_vma *vma;
    uhm_free_extent *free_extent;
    uhm_vma *hot_vma_slow;
    uhm_vma *hot_vma_fast;
    uhm_free_extent *hot_free_slow;
    uhm_free_extent *hot_free_fast;
    size_t normal_vma_count = 0;
    size_t emergency_vma_count = 0;
    size_t normal_free_extent_count = 0;
    size_t emergency_free_extent_count = 0;
    size_t count = 0;

    if (!heap) {
        return 0;
    }

    if (atomic_load_explicit(&heap->core.metadata_published_epoch,
                             UHM_ATOMIC_ACQUIRE) >
        heap->core.metadata_epoch) {
        return 0;
    }
    if (uhm_metadata_quarantine_has_pending(heap) &&
        !uhm_metadata_reclaim_pending_load(heap)) {
        return 0;
    }

    if (!uhm_retired_vma_list_validate(heap, heap->core.retired_vma_head,
                                       heap->core.retired_vma_tail, 0,
                                       &normal_vma_count) ||
        !uhm_retired_vma_list_validate(
            heap, heap->core.retired_emergency_vma_head,
            heap->core.retired_emergency_vma_tail, 1,
            &emergency_vma_count) ||
        emergency_vma_count != heap->core.retired_emergency_vma_count ||
        normal_vma_count + emergency_vma_count !=
            heap->core.retired_vma_count) {
        return 0;
    }
    if (!uhm_retired_free_extent_list_validate(
            heap, heap->core.retired_free_extent_head,
            heap->core.retired_free_extent_tail, 0,
            &normal_free_extent_count) ||
        !uhm_retired_free_extent_list_validate(
            heap, heap->core.retired_emergency_free_extent_head,
            heap->core.retired_emergency_free_extent_tail, 1,
            &emergency_free_extent_count) ||
        emergency_free_extent_count !=
            heap->core.retired_emergency_free_extent_count ||
        normal_free_extent_count + emergency_free_extent_count !=
            heap->core.retired_free_extent_count) {
        return 0;
    }

    count = 0;
    hot_vma_slow = heap->core.hot_vma_head;
    hot_vma_fast = heap->core.hot_vma_head;
    while (hot_vma_fast && hot_vma_fast->next) {
        hot_vma_slow = hot_vma_slow->next;
        hot_vma_fast = hot_vma_fast->next->next;
        if (hot_vma_slow == hot_vma_fast) {
            return 0;
        }
    }
    for (vma = heap->core.hot_vma_head; vma; vma = vma->next) {
        if (vma->magic != 0 ||
            vma->flags != 0 ||
            vma->addr != 0 ||
            vma->size != 0 ||
            vma->prot != 0 ||
            vma->generation != 0 ||
            vma->retire_epoch != 0 ||
            vma->prev != 0 ||
            vma->tree_parent != 0 ||
            vma->tree_left != 0 ||
            vma->tree_right != 0 ||
            vma->tree_priority != 0 ||
            vma->desc.magic != 0 ||
            vma->desc.id != 0) {
            return 0;
        }
        ++count;
    }
    if (count != heap->core.hot_vma_count ||
        count > UHM_METADATA_HOT_VMA_MAX) {
        return 0;
    }

    count = 0;
    hot_free_slow = heap->core.hot_free_extent_head;
    hot_free_fast = heap->core.hot_free_extent_head;
    while (hot_free_fast && hot_free_fast->next) {
        hot_free_slow = hot_free_slow->next;
        hot_free_fast = hot_free_fast->next->next;
        if (hot_free_slow == hot_free_fast) {
            return 0;
        }
    }
    for (free_extent = heap->core.hot_free_extent_head; free_extent;
         free_extent = free_extent->next) {
        if (free_extent->magic != 0 ||
            free_extent->flags != 0 ||
            free_extent->addr != 0 ||
            free_extent->size != 0 ||
            free_extent->retire_epoch != 0 ||
            free_extent->tree_parent != 0 ||
            free_extent->tree_left != 0 ||
            free_extent->tree_right != 0 ||
            free_extent->tree_max_size != 0 ||
            free_extent->tree_priority != 0 ||
            free_extent->desc.magic != 0 ||
            free_extent->desc.id != 0) {
            return 0;
        }
        ++count;
    }
    return count == heap->core.hot_free_extent_count &&
           count <= UHM_METADATA_HOT_FREE_EXTENT_MAX;
}
