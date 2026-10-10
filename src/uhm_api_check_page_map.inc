static int uhm_page_leaf_records_are_zero(const uhm_page_leaf *leaf) {
    size_t i;

    if (!leaf) {
        return 0;
    }
    for (i = 0; i < UHM_PAGE_LEAF_PAGES; ++i) {
        const uhm_page_record *record = &leaf->records[i];
        if (record->owner != 0 ||
            record->va_state != 0 ||
            record->commit_state != 0 ||
            record->prot != 0 ||
            record->descriptor_id != 0 ||
            record->generation != 0 ||
            record->pin_count != 0 ||
            record->flags != 0) {
            return 0;
        }
    }
    return 1;
}

static int uhm_page_map_validate(uhm_heap *heap) {
    size_t shard;

    if (!heap) {
        return 0;
    }

    for (shard = 0; shard < UHM_PAGE_MAP_SHARDS; ++shard) {
        uhm_page_leaf *slow;
        uhm_page_leaf *fast;
        uhm_page_leaf *leaf;
        size_t previous_base = 0;
        size_t spare_count = 0;
        int have_previous = 0;
        int ok = 1;

        uhm_lock(&heap->core.page_map_locks[shard].lock);
        slow = heap->core.page_map[shard];
        fast = heap->core.page_map[shard];
        while (fast && fast->next) {
            slow = slow->next;
            fast = fast->next->next;
            if (slow == fast) {
                ok = 0;
                break;
            }
        }
        for (leaf = heap->core.page_map[shard]; ok && leaf; leaf = leaf->next) {
            if (leaf->base_index >= heap->core.page_count ||
                (leaf->base_index % UHM_PAGE_LEAF_PAGES) != 0 ||
                uhm_page_shard_for_index(leaf->base_index) != shard ||
                (have_previous && previous_base >= leaf->base_index)) {
                ok = 0;
                break;
            }
            previous_base = leaf->base_index;
            have_previous = 1;
        }

        slow = heap->core.page_map_spares[shard];
        fast = heap->core.page_map_spares[shard];
        while (ok && fast && fast->next) {
            slow = slow->next;
            fast = fast->next->next;
            if (slow == fast) {
                ok = 0;
                break;
            }
        }
        for (leaf = heap->core.page_map_spares[shard]; ok && leaf; leaf = leaf->next) {
            if (leaf->base_index != 0 || !uhm_page_leaf_records_are_zero(leaf)) {
                ok = 0;
                break;
            }
            ++spare_count;
        }
        if (ok && spare_count != heap->core.page_map_spare_count[shard]) {
            ok = 0;
        }
        uhm_unlock(&heap->core.page_map_locks[shard].lock);
        if (!ok) {
            return 0;
        }
    }
    return 1;
}
