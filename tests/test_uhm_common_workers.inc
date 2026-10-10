#if defined(_WIN32)
static DWORD WINAPI ctx_cross_thread_worker(LPVOID user) {
    ctx_cross_thread_args *args = (ctx_cross_thread_args *)user;
    args->malloc_result = args->api->malloc_ctx(args->heap, args->ctx, 16u);
    args->api->thread_ctx_close(args->heap, args->ctx);
    return 0;
}
#else
static void *ctx_cross_thread_worker(void *user) {
    ctx_cross_thread_args *args = (ctx_cross_thread_args *)user;
    args->malloc_result = args->api->malloc_ctx(args->heap, args->ctx, 16u);
    args->api->thread_ctx_close(args->heap, args->ctx);
    return 0;
}
#endif

static int run_small_churn_worker(small_churn_args *args) {
    size_t i;
    args->ok = 0;
    args->fail_stage = 0;
    args->fail_iteration = 0;
    args->last_error = UHM_OK;
    args->platform_error = 0;
    for (i = 0; i < 2000u; ++i) {
        size_t size = ((i * 17u) + (size_t)args->id * 29u) % 512u + 1u;
        unsigned char *ptr = (unsigned char *)args->api->malloc(args->heap, size);
        if (!ptr) {
            args->fail_stage = 1u;
            args->fail_iteration = i;
            if (args->api->get_last_error) {
                (void)args->api->get_last_error(args->heap,
                                                &args->last_error,
                                                &args->platform_error);
            }
            return 0;
        }
        memset(ptr, (int)(0x20u + args->id), size);
        if ((i % 5u) == 0) {
            size_t new_size = size + ((i % 7u) + 1u) * 13u;
            unsigned char *grown = (unsigned char *)args->api->realloc(args->heap, ptr,
                                                                       new_size);
            if (!grown) {
                args->fail_stage = 2u;
                args->fail_iteration = i;
                if (args->api->get_last_error) {
                    (void)args->api->get_last_error(args->heap,
                                                    &args->last_error,
                                                    &args->platform_error);
                }
                args->api->free(args->heap, ptr);
                return 0;
            }
            ptr = grown;
            memset(ptr, (int)(0x40u + args->id), new_size);
        }
        if ((i % 11u) == 0) {
            void *aligned = args->api->aligned_alloc(args->heap, 64u, size + 64u);
            if (!aligned) {
                args->fail_stage = 3u;
                args->fail_iteration = i;
                if (args->api->get_last_error) {
                    (void)args->api->get_last_error(args->heap,
                                                    &args->last_error,
                                                    &args->platform_error);
                }
                args->api->free(args->heap, ptr);
                return 0;
            }
            memset(aligned, (int)(0x60u + args->id), size + 64u);
            args->api->free(args->heap, aligned);
        }
        args->api->free(args->heap, ptr);
    }
    args->ok = 1;
    return 1;
}

static int run_lockless_remote_reuse_worker(
    lockless_remote_reuse_args *args) {
    unsigned char *ptr;

    args->ok = 0;
    if (!args || !args->api || !args->heap ||
        !args->ready_count || !args->start) {
        return 0;
    }
    (void)atomic_fetch_add_explicit(args->ready_count, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    while (atomic_load_explicit(args->start, UHM_ATOMIC_ACQUIRE) == 0u) {
        test_thread_pause();
    }
    ptr = (unsigned char *)args->api->malloc(args->heap, 64u);
    args->result = ptr;
    if (!ptr) {
        return 0;
    }
    memset(ptr, (int)(0x31u + args->id), 64u);
    args->ok = 1;
    return 1;
}

static int run_accounted_remote_contended_free_worker(
    accounted_remote_contended_free_args *args) {
    if (!args || !args->api || !args->heap || !args->ptr) {
        return 0;
    }
    args->ok = 0;
    args->api->free(args->heap, args->ptr);
    args->ok = 1;
    atomic_store_explicit(&args->done, 1u, UHM_ATOMIC_RELEASE);
    return 1;
}

static int run_accounted_remote_contended_malloc_worker(
    accounted_remote_contended_malloc_args *args) {
    unsigned char *ptr;

    if (!args || !args->api || !args->heap) {
        return 0;
    }
    args->ok = 0;
    ptr = (unsigned char *)args->api->malloc(args->heap, 64u);
    args->result = ptr;
    if (ptr) {
        memset(ptr, 0x6c, 64u);
        args->ok = 1;
    }
    atomic_store_explicit(&args->done, 1u, UHM_ATOMIC_RELEASE);
    return args->ok;
}

static int run_tls_exit_worker(tls_exit_args *args) {
    unsigned char *ptr;

    args->ok = 0;
    ptr = (unsigned char *)args->api->malloc(args->heap, 64u);
    if (!ptr) {
        return 0;
    }
    memset(ptr, 0x5a, 64u);
    args->slot = test_slab_slot_from_payload(ptr);
    args->ptr = ptr;
    args->api->free(args->heap, ptr);
    args->ok = args->slot != 0;
    return args->ok;
}

static int run_tls_churn_worker(tls_churn_args *args) {
    size_t i;

    args->ok = 0;
    args->fail_stage = 0;
    args->fail_iteration = 0;
    args->last_error = UHM_OK;
    args->platform_error = 0;
    for (i = 0; i < args->iterations; ++i) {
        size_t size = 1u + (((i + 3u) * 23u + (size_t)args->id * 41u) % 512u);
        unsigned char *ptr;
        if ((i % 9u) == 0) {
            ptr = (unsigned char *)args->api->calloc(args->heap, size, 1u);
            if (!ptr) {
                args->fail_stage = 1u;
                args->fail_iteration = i;
                if (args->api->get_last_error) {
                    (void)args->api->get_last_error(args->heap,
                                                    &args->last_error,
                                                    &args->platform_error);
                }
                return 0;
            }
            if (!test_bytes_equal(ptr, size, 0u)) {
                args->fail_stage = 2u;
                args->fail_iteration = i;
                args->api->free(args->heap, ptr);
                return 0;
            }
        } else {
            ptr = (unsigned char *)args->api->malloc(args->heap, size);
            if (!ptr) {
                args->fail_stage = 3u;
                args->fail_iteration = i;
                if (args->api->get_last_error) {
                    (void)args->api->get_last_error(args->heap,
                                                    &args->last_error,
                                                    &args->platform_error);
                }
                return 0;
            }
            test_fill_bytes(ptr, size, 0x44u + args->id);
            if (!test_bytes_equal(ptr, size, 0x44u + args->id)) {
                args->fail_stage = 4u;
                args->fail_iteration = i;
                args->api->free(args->heap, ptr);
                return 0;
            }
        }
        args->api->free(args->heap, ptr);
        if (i == 0u && args->ready_count && args->release) {
            (void)atomic_fetch_add_explicit(args->ready_count, 1u,
                                            UHM_ATOMIC_ACQ_REL);
            while (atomic_load_explicit(args->release,
                                        UHM_ATOMIC_ACQUIRE) == 0u) {
                test_thread_pause();
            }
        }
    }
    args->ok = 1;
    return 1;
}

static int run_tls_hold_worker(tls_hold_args *args) {
    unsigned char *ptr;

    args->ok = 0;
    ptr = (unsigned char *)args->api->malloc(args->heap, 80u);
    if (!ptr) {
        atomic_store_explicit(&args->ready, 1u, UHM_ATOMIC_RELEASE);
        return 0;
    }
    memset(ptr, 0x6c, 80u);
    args->slot = test_slab_slot_from_payload(ptr);
    args->ptr = ptr;
    args->api->free(args->heap, ptr);
    args->ok = args->slot != 0;
    atomic_store_explicit(&args->ready, 1u, UHM_ATOMIC_RELEASE);
    while (atomic_load_explicit(&args->release, UHM_ATOMIC_ACQUIRE) == 0u) {
        test_thread_pause();
    }
    return args->ok;
}

static int run_lockless_tls_class_flush_worker(
    lockless_tls_class_flush_args *args) {
    enum { ptr_count = 80 };
    void *ptrs[ptr_count];
    size_t i;
    uhm_tagged_freelist_node *head;

    if (!args || !args->api || !args->heap) {
        return 0;
    }
    args->ok = 0;
    args->remote_ptr = 0;
    args->remote_slot = 0;
    args->remote_count = 0;
    memset(ptrs, 0, sizeof(ptrs));
    for (i = 0; i < ptr_count; ++i) {
        ptrs[i] = args->api->malloc(args->heap, 64u);
        if (!ptrs[i]) {
            return 0;
        }
        memset(ptrs[i], (int)(0x71u + (i & 7u)), 64u);
    }
    for (i = 0; i < ptr_count; ++i) {
        uhm_slab_slot *slot = test_slab_slot_from_payload(ptrs[i]);
        args->api->free(args->heap, ptrs[i]);
        if (slot && slot->slab &&
            test_slab_remote_class_queue_contains(args->heap, slot->slab,
                                                  slot)) {
            ++args->remote_count;
        }
    }
    if (args->remote_count == 0) {
        return 0;
    }
    head = uhm_tagged_freelist_unpack_ptr(
        atomic_load_explicit(
            &args->heap->core.slab_remote_class_lf[
                 test_slab_slot_from_payload(ptrs[0])->size_class]
                 .queue.head,
            UHM_ATOMIC_ACQUIRE));
    args->remote_slot = test_slab_slot_from_payload(ptrs[0]);
    if (head) {
        args->remote_slot = test_slab_slot_from_remote_link(head);
    }
    if (!args->remote_slot || !args->remote_slot->slab ||
        !test_slab_remote_class_queue_contains(
            args->heap, args->remote_slot->slab, args->remote_slot)) {
        return 0;
    }
    args->remote_ptr =
        (void *)((uintptr_t)args->remote_slot +
                 test_slab_slot_header_size());
    args->ok = 1;
    return 1;
}

static int run_large_realloc_worker(large_realloc_args *args) {
    enum { slot_count = 8, iterations = 320 };
    unsigned char *ptrs[slot_count];
    size_t sizes[slot_count];
    unsigned patterns[slot_count];
    size_t i;

    memset(ptrs, 0, sizeof(ptrs));
    memset(sizes, 0, sizeof(sizes));
    memset(patterns, 0, sizeof(patterns));
    args->ok = 0;

    for (i = 0; i < slot_count; ++i) {
        sizes[i] = 4096u + ((size_t)args->id * 503u) + i * 3072u;
        patterns[i] = 0x80u + args->id * 17u + (unsigned)i;
        ptrs[i] = (unsigned char *)args->api->malloc(args->heap, sizes[i]);
        if (!ptrs[i]) {
            goto fail;
        }
        test_fill_bytes(ptrs[i], sizes[i], patterns[i]);
    }

    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 5u + (size_t)args->id) % slot_count;
        size_t old_size = sizes[slot];
        size_t new_size = 4096u + (((i + 11u) * (slot + 7u) +
                                    (size_t)args->id * 977u) %
                                   (48u * 1024u));
        size_t preserved = test_min_size(old_size, new_size);
        unsigned char *grown;

        grown = (unsigned char *)args->api->realloc(args->heap, ptrs[slot], new_size);
        if (!grown) {
            goto fail;
        }
        ptrs[slot] = grown;
        if (!test_bytes_equal(ptrs[slot], preserved, patterns[slot])) {
            goto fail;
        }
        patterns[slot] = 0xa0u + args->id * 19u + (unsigned)i + (unsigned)slot;
        sizes[slot] = new_size;
        test_fill_bytes(ptrs[slot], sizes[slot], patterns[slot]);
    }

    args->ok = 1;

fail:
    for (i = 0; i < slot_count; ++i) {
        if (ptrs[i]) {
            args->api->free(args->heap, ptrs[i]);
        }
    }
    return args->ok;
}

static int run_cross_thread_alloc_worker(cross_thread_free_args *args) {
    size_t i;
    args->ok = 0;
    for (i = 0; i < args->count; ++i) {
        size_t index = args->start + i;
        size_t size = 1u + (((i + 1u) * 37u + (size_t)args->id * 101u) % 2048u);
        unsigned pattern = 0x30u + args->id;
        void *ptr = args->api->malloc(args->heap, size);
        if (!ptr) {
            args->fail_index = index;
            if (args->api->get_last_error) {
                (void)args->api->get_last_error(args->heap,
                                                &args->last_error,
                                                &args->platform_error);
            }
            return 0;
        }
        test_fill_bytes(ptr, size, pattern);
        args->ptrs[index] = ptr;
        args->sizes[index] = size;
    }
    args->ok = 1;
    return 1;
}

static int run_cross_thread_free_worker(cross_thread_free_args *args) {
    size_t i;
    args->ok = 0;
    for (i = 0; i < args->count; ++i) {
        size_t index = args->start + i;
        void *ptr = args->ptrs[index];
        if (!ptr || !test_bytes_equal(ptr, args->sizes[index], 0x30u + args->id)) {
            return 0;
        }
        args->api->free(args->heap, ptr);
        args->ptrs[index] = 0;
        args->sizes[index] = 0;
    }
    args->ok = 1;
    return 1;
}

static int run_ctx_churn_worker(ctx_churn_args *args) {
    enum { iterations = 600 };
    size_t i;

    args->ok = 0;
    for (i = 0; i < iterations; ++i) {
        uhm_thread_config ctx_config;
        uhm_thread_ctx *ctx = 0;
        size_t ctx_size = 32u + ((i * 13u + (size_t)args->id * 17u) % 256u);
        size_t heap_size = 48u + ((i * 29u + (size_t)args->id * 31u) % 512u);
        void *ctx_ptr;
        void *heap_ptr;

        memset(&ctx_config, 0, sizeof(ctx_config));
        ctx_config.size = sizeof(ctx_config);
        ctx_config.abi_major = UHM_ABI_MAJOR;
        ctx_config.abi_minor = UHM_ABI_MINOR;
        ctx_config.storage = args->ctx_storage;
        ctx_config.storage_size = args->ctx_storage_size;
        ctx_config.local_cache_bytes = args->heap->F.vm.page_size;

        if (args->api->thread_ctx_open(args->heap, &ctx_config, &ctx) != UHM_OK) {
            return 0;
        }
        ctx_ptr = args->api->malloc_ctx(args->heap, ctx, ctx_size);
        if (!ctx_ptr) {
            args->api->thread_ctx_close(args->heap, ctx);
            return 0;
        }
        heap_ptr = args->api->malloc(args->heap, heap_size);
        if (!heap_ptr) {
            args->api->free_ctx(args->heap, ctx, ctx_ptr);
            args->api->thread_ctx_close(args->heap, ctx);
            return 0;
        }
        test_fill_bytes(ctx_ptr, ctx_size, 0x50u + args->id);
        test_fill_bytes(heap_ptr, heap_size, 0x70u + args->id);
        if (!test_bytes_equal(ctx_ptr, ctx_size, 0x50u + args->id) ||
            !test_bytes_equal(heap_ptr, heap_size, 0x70u + args->id)) {
            args->api->free(args->heap, heap_ptr);
            args->api->free_ctx(args->heap, ctx, ctx_ptr);
            args->api->thread_ctx_close(args->heap, ctx);
            return 0;
        }
        args->api->free(args->heap, heap_ptr);
        args->api->free_ctx(args->heap, ctx, ctx_ptr);
        args->api->thread_ctx_close(args->heap, ctx);
        if (args->api->malloc_ctx(args->heap, ctx, 16u) != 0) {
            return 0;
        }
    }
    args->ok = 1;
    return 1;
}

static int run_control_compete_metadata_worker(control_compete_args *args) {
    size_t i;
    args->ok = 0;
    args->fail_op = 0;
    args->fail_iter = 0;
    args->fail_result = UHM_OK;
    for (i = 0; i < args->pressure_pages; ++i) {
        void *page = (void *)((uintptr_t)args->pressure_map + i * args->page_size);
        uint32_t prot = (i % 3u) == 0 ? UHM_PROT_READ : (UHM_PROT_READ | UHM_PROT_WRITE);
        uhm_result result =
            args->api->mprotect(args->heap, page, args->page_size, prot);
        if (result != UHM_OK) {
            if (result == UHM_ERR_OUT_OF_MEMORY ||
                result == UHM_ERR_OUT_OF_ADDRESS_SPACE) {
                args->ok = 1;
                return 1;
            }
            args->fail_op = "mprotect";
            args->fail_iter = i;
            args->fail_result = result;
            return 0;
        }
    }
    args->ok = 1;
    return 1;
}

static int run_control_compete_malloc_worker(control_compete_args *args) {
    enum { slot_count = 24, iterations = 900 };
    void *slots[slot_count];
    size_t sizes[slot_count];
    size_t i;

    memset(slots, 0, sizeof(slots));
    memset(sizes, 0, sizeof(sizes));
    args->ok = 0;
    args->fail_op = 0;
    args->fail_iter = 0;
    args->fail_result = UHM_OK;

    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 7u + 3u) % slot_count;
        size_t size = 96u + ((i * 53u + 17u) % 4096u);
        if (slots[slot]) {
            if (!test_bytes_equal(slots[slot], sizes[slot], 0x90u + (unsigned)slot)) {
                args->fail_op = "malloc_payload";
                args->fail_iter = i;
                args->fail_result = UHM_ERR_CORRUPTION;
                goto fail;
            }
            args->api->free(args->heap, slots[slot]);
            slots[slot] = 0;
            sizes[slot] = 0;
        }
        slots[slot] = args->api->malloc(args->heap, size);
        if (!slots[slot]) {
            args->fail_op = "malloc";
            args->fail_iter = i;
            args->fail_result = UHM_ERR_OUT_OF_MEMORY;
            goto fail;
        }
        sizes[slot] = size;
        test_fill_bytes(slots[slot], size, 0x90u + (unsigned)slot);
    }
    args->ok = 1;

fail:
    for (i = 0; i < slot_count; ++i) {
        if (slots[i]) {
            args->api->free(args->heap, slots[i]);
        }
    }
    return args->ok;
}

static int run_control_compete_mmap_worker(control_compete_args *args) {
    enum { slot_count = 12, iterations = 360 };
    void *maps[slot_count];
    size_t map_sizes[slot_count];
    size_t i;

    memset(maps, 0, sizeof(maps));
    memset(map_sizes, 0, sizeof(map_sizes));
    args->ok = 0;
    args->fail_op = 0;
    args->fail_iter = 0;
    args->fail_result = UHM_OK;

    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 5u + 1u) % slot_count;
        size_t size = args->page_size * (1u + (i % 3u));
        if (maps[slot]) {
            uhm_result result =
                args->api->munmap(args->heap, maps[slot], map_sizes[slot]);
            if (result != UHM_OK) {
                args->fail_op = "munmap";
                args->fail_iter = i;
                args->fail_result = result;
                goto fail;
            }
            maps[slot] = 0;
            map_sizes[slot] = 0;
        }
        {
            uhm_result result =
                args->api->mmap(args->heap, &maps[slot], size,
                                UHM_PROT_READ | UHM_PROT_WRITE,
                                UHM_MAP_PRIVATE);
            if (result != UHM_OK) {
                if (result == UHM_ERR_OUT_OF_MEMORY ||
                    result == UHM_ERR_OUT_OF_ADDRESS_SPACE) {
                    args->ok = 1;
                    goto fail;
                }
                args->fail_op = "mmap";
                args->fail_iter = i;
                args->fail_result = result;
                goto fail;
            }
        }
        map_sizes[slot] = size;
        if ((i % 4u) == 0) {
            uhm_result result =
                args->api->mprotect(args->heap, maps[slot], args->page_size,
                                    UHM_PROT_READ);
            if (result != UHM_OK) {
                if (result == UHM_ERR_OUT_OF_MEMORY ||
                    result == UHM_ERR_OUT_OF_ADDRESS_SPACE) {
                    args->ok = 1;
                    goto fail;
                }
                args->fail_op = "mmap_mprotect";
                args->fail_iter = i;
                args->fail_result = result;
                goto fail;
            }
        }
    }
    args->ok = 1;

fail:
    for (i = 0; i < slot_count; ++i) {
        if (maps[i]) {
            (void)args->api->munmap(args->heap, maps[i], map_sizes[i]);
        }
    }
    return args->ok;
}

static int run_control_compete_worker(control_compete_args *args) {
    if (args->role == 0u) {
        return run_control_compete_metadata_worker(args);
    }
    if (args->role == 1u) {
        return run_control_compete_malloc_worker(args);
    }
    return run_control_compete_mmap_worker(args);
}

static int run_page_leaf_churn_worker(page_leaf_churn_args *args) {
    enum { iterations = 48 };
    size_t i;

    args->ok = 0;
    for (i = 0; i < iterations; ++i) {
        size_t page_in_leaf = 8u + ((i * 13u + (size_t)args->leaf_id * 7u) %
                                    (args->leaf_pages - 16u));
        uintptr_t addr = args->arena_base +
                         (((size_t)args->leaf_id * args->leaf_pages) + page_in_leaf) *
                             args->page_size;
        void *fixed = (void *)addr;
        if (args->api->mmap(args->heap, &fixed, args->page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) != UHM_OK) {
            return 0;
        }
        if (fixed != (void *)addr) {
            (void)args->api->munmap(args->heap, fixed, args->page_size);
            return 0;
        }
        if ((i % 3u) == 0 &&
            args->api->mprotect(args->heap, fixed, args->page_size,
                                UHM_PROT_READ) != UHM_OK) {
            (void)args->api->munmap(args->heap, fixed, args->page_size);
            return 0;
        }
        if (args->api->munmap(args->heap, fixed, args->page_size) != UHM_OK) {
            return 0;
        }
    }
    args->ok = 1;
    return 1;
}

static int run_tagged_freelist_worker(tagged_freelist_args *args) {
    size_t i;

    args->ok = 0;
    for (i = 0; i < args->iterations; ++i) {
        uhm_tagged_freelist_node *link = 0;
        tagged_freelist_test_node *node;
        size_t spins = 0;

        while (!link && spins < 100000u) {
            link = uhm_tagged_freelist_pop(args->list);
            ++spins;
        }
        if (!link) {
            return 0;
        }
        node = (tagged_freelist_test_node *)link;
        if (atomic_exchange_explicit(&node->in_use, 1u,
                                     UHM_ATOMIC_ACQ_REL) != 0u) {
            return 0;
        }
        (void)atomic_fetch_add_explicit(&node->visits, 1u,
                                        UHM_ATOMIC_RELAXED);
        if (node->id >= 1024u) {
            return 0;
        }
        if (atomic_exchange_explicit(&node->in_use, 0u,
                                     UHM_ATOMIC_ACQ_REL) != 1u) {
            return 0;
        }
        if (!uhm_tagged_freelist_push(args->list, link)) {
            return 0;
        }
    }
    args->ok = 1;
    return 1;
}

static int run_close_during_mmap_map_worker(
    close_during_mmap_map_args *args) {
    void *addr = 0;
    args->result = args->api->mmap(args->heap, &addr, args->map_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE, 0);
    args->addr = addr;
    return args->result == UHM_OK;
}

static int run_close_during_mmap_close_worker(
    close_during_mmap_close_args *args) {
    atomic_store_explicit(&args->started, 1u, UHM_ATOMIC_RELEASE);
    args->result = args->api->close(args->heap);
    atomic_store_explicit(&args->done, 1u, UHM_ATOMIC_RELEASE);
    return args->result == UHM_OK;
}

#if defined(_WIN32)
static DWORD WINAPI small_churn_worker(LPVOID user) {
    return run_small_churn_worker((small_churn_args *)user) ? 0u : 1u;
}

static DWORD WINAPI lockless_remote_reuse_worker(LPVOID user) {
    return run_lockless_remote_reuse_worker(
               (lockless_remote_reuse_args *)user)
               ? 0u
               : 1u;
}

static DWORD WINAPI accounted_remote_contended_free_worker(LPVOID user) {
    return run_accounted_remote_contended_free_worker(
               (accounted_remote_contended_free_args *)user)
               ? 0u
               : 1u;
}

static DWORD WINAPI accounted_remote_contended_malloc_worker(LPVOID user) {
    return run_accounted_remote_contended_malloc_worker(
               (accounted_remote_contended_malloc_args *)user)
               ? 0u
               : 1u;
}

static DWORD WINAPI tls_exit_worker(LPVOID user) {
    return run_tls_exit_worker((tls_exit_args *)user) ? 0u : 1u;
}

static DWORD WINAPI tls_churn_worker(LPVOID user) {
    return run_tls_churn_worker((tls_churn_args *)user) ? 0u : 1u;
}

static DWORD WINAPI tls_hold_worker(LPVOID user) {
    return run_tls_hold_worker((tls_hold_args *)user) ? 0u : 1u;
}

static DWORD WINAPI lockless_tls_class_flush_worker(LPVOID user) {
    return run_lockless_tls_class_flush_worker(
               (lockless_tls_class_flush_args *)user)
               ? 0u
               : 1u;
}

static DWORD WINAPI large_realloc_worker(LPVOID user) {
    return run_large_realloc_worker((large_realloc_args *)user) ? 0u : 1u;
}

static DWORD WINAPI cross_thread_alloc_worker(LPVOID user) {
    return run_cross_thread_alloc_worker((cross_thread_free_args *)user) ? 0u : 1u;
}

static DWORD WINAPI cross_thread_free_worker(LPVOID user) {
    return run_cross_thread_free_worker((cross_thread_free_args *)user) ? 0u : 1u;
}

static DWORD WINAPI ctx_churn_worker(LPVOID user) {
    return run_ctx_churn_worker((ctx_churn_args *)user) ? 0u : 1u;
}

static DWORD WINAPI control_compete_worker(LPVOID user) {
    return run_control_compete_worker((control_compete_args *)user) ? 0u : 1u;
}

static DWORD WINAPI page_leaf_churn_worker(LPVOID user) {
    return run_page_leaf_churn_worker((page_leaf_churn_args *)user) ? 0u : 1u;
}

static DWORD WINAPI tagged_freelist_worker(LPVOID user) {
    return run_tagged_freelist_worker((tagged_freelist_args *)user) ? 0u : 1u;
}

static DWORD WINAPI close_during_mmap_map_worker(LPVOID user) {
    return run_close_during_mmap_map_worker(
               (close_during_mmap_map_args *)user)
               ? 0u
               : 1u;
}

static DWORD WINAPI close_during_mmap_close_worker(LPVOID user) {
    return run_close_during_mmap_close_worker(
               (close_during_mmap_close_args *)user)
               ? 0u
               : 1u;
}
#else
static void *small_churn_worker(void *user) {
    (void)run_small_churn_worker((small_churn_args *)user);
    return 0;
}

static void *lockless_remote_reuse_worker(void *user) {
    (void)run_lockless_remote_reuse_worker(
        (lockless_remote_reuse_args *)user);
    return 0;
}

static void *accounted_remote_contended_free_worker(void *user) {
    (void)run_accounted_remote_contended_free_worker(
        (accounted_remote_contended_free_args *)user);
    return 0;
}

static void *accounted_remote_contended_malloc_worker(void *user) {
    (void)run_accounted_remote_contended_malloc_worker(
        (accounted_remote_contended_malloc_args *)user);
    return 0;
}

static void *tls_exit_worker(void *user) {
    (void)run_tls_exit_worker((tls_exit_args *)user);
    return 0;
}

static void *tls_churn_worker(void *user) {
    (void)run_tls_churn_worker((tls_churn_args *)user);
    return 0;
}

static void *tls_hold_worker(void *user) {
    (void)run_tls_hold_worker((tls_hold_args *)user);
    return 0;
}

static void *lockless_tls_class_flush_worker(void *user) {
    (void)run_lockless_tls_class_flush_worker(
        (lockless_tls_class_flush_args *)user);
    return 0;
}

static void *large_realloc_worker(void *user) {
    (void)run_large_realloc_worker((large_realloc_args *)user);
    return 0;
}

static void *cross_thread_alloc_worker(void *user) {
    (void)run_cross_thread_alloc_worker((cross_thread_free_args *)user);
    return 0;
}

static void *cross_thread_free_worker(void *user) {
    (void)run_cross_thread_free_worker((cross_thread_free_args *)user);
    return 0;
}

static void *ctx_churn_worker(void *user) {
    (void)run_ctx_churn_worker((ctx_churn_args *)user);
    return 0;
}

static void *control_compete_worker(void *user) {
    (void)run_control_compete_worker((control_compete_args *)user);
    return 0;
}

static void *page_leaf_churn_worker(void *user) {
    (void)run_page_leaf_churn_worker((page_leaf_churn_args *)user);
    return 0;
}

static void *tagged_freelist_worker(void *user) {
    (void)run_tagged_freelist_worker((tagged_freelist_args *)user);
    return 0;
}

static void *close_during_mmap_map_worker(void *user) {
    (void)run_close_during_mmap_map_worker(
        (close_during_mmap_map_args *)user);
    return 0;
}

static void *close_during_mmap_close_worker(void *user) {
    (void)run_close_during_mmap_close_worker(
        (close_during_mmap_close_args *)user);
    return 0;
}
#endif

static void expect_ctx_cross_thread_rejected(const uhm_api *api, uhm_heap *heap,
                                             uhm_thread_ctx *ctx) {
    ctx_cross_thread_args args;
    memset(&args, 0, sizeof(args));
    args.api = api;
    args.heap = heap;
    args.ctx = ctx;
#if defined(_WIN32)
    {
        HANDLE thread = CreateThread(0, 0, ctx_cross_thread_worker, &args, 0, 0);
        expect_true(thread != 0, "ctx cross-thread create");
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "ctx cross-thread wait");
        CloseHandle(thread);
    }
#else
    {
        pthread_t thread;
        expect_true(pthread_create(&thread, 0, ctx_cross_thread_worker, &args) == 0,
                    "ctx cross-thread create");
        expect_true(pthread_join(thread, 0) == 0, "ctx cross-thread join");
    }
#endif
    expect_true(args.malloc_result == 0, "ctx cross-thread malloc rejected");
}

static void test_tagged_freelist_basic_overflow_and_contention(void) {
    enum { node_count = 16, thread_count = 8, iterations = 12000 };
    uhm_tagged_freelist list;
    tagged_freelist_test_node nodes[node_count];
    tagged_freelist_args args[thread_count];
    uhm_tagged_freelist_node *link;
    uint64_t total_visits = 0;
    size_t i;
    size_t drained = 0;

    uhm_tagged_freelist_init(&list);
    memset(nodes, 0, sizeof(nodes));
    nodes[0].id = 0;
    atomic_init(&nodes[0].in_use, 0u);
    atomic_init(&nodes[0].visits, 0u);
    if (!uhm_tagged_freelist_is_supported()) {
        expect_true(!uhm_tagged_freelist_ptr_packable(&nodes[0].link),
                    "tagged freelist unsupported nodes are not packable");
        expect_true(!uhm_tagged_freelist_push(&list, &nodes[0].link),
                    "tagged freelist unsupported push fails cleanly");
        expect_true(uhm_tagged_freelist_pop(&list) == 0,
                    "tagged freelist unsupported pop empty");
        expect_true(uhm_tagged_freelist_take_all(&list) == 0,
                    "tagged freelist unsupported take_all empty");
        return;
    }
    expect_true(uhm_tagged_freelist_ptr_packable(&nodes[0].link),
                "tagged freelist node packable");
    atomic_store_explicit(
        &list.head,
        uhm_tagged_freelist_pack(&nodes[0].link,
                                 UHM_TAGGED_FREELIST_TAG_MASK),
        UHM_ATOMIC_RELEASE);
    link = uhm_tagged_freelist_pop(&list);
    expect_true(link == &nodes[0].link, "tagged freelist overflow pop");
    expect_true(uhm_tagged_freelist_tag_overflows(&list) == 1u,
                "tagged freelist overflow counted");
    expect_true(uhm_tagged_freelist_pop(&list) == 0,
                "tagged freelist empty after overflow pop");

    uhm_tagged_freelist_init(&list);
    test_tagged_freelist_force_cas_failure(&list, &nodes[0].link,
                                           &nodes[1].link);
    for (i = 0; i < node_count; ++i) {
        uhm_tagged_freelist_next_store(&nodes[i].link, 0);
        nodes[i].id = (uint32_t)i;
        atomic_init(&nodes[i].in_use, 0u);
        atomic_init(&nodes[i].visits, 0u);
        expect_true(uhm_tagged_freelist_push(&list, &nodes[i].link),
                    "tagged freelist initial push");
    }

#if defined(_WIN32)
    {
        HANDLE threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            memset(&args[i], 0, sizeof(args[i]));
            args[i].list = &list;
            args[i].iterations = iterations;
            threads[i] = CreateThread(0, 0, tagged_freelist_worker,
                                      &args[i], 0, 0);
            expect_true(threads[i] != 0, "tagged freelist create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) ==
                            WAIT_OBJECT_0,
                        "tagged freelist join thread");
            CloseHandle(threads[i]);
            expect_true(args[i].ok, "tagged freelist worker ok");
        }
    }
#else
    {
        pthread_t threads[thread_count];
        for (i = 0; i < thread_count; ++i) {
            memset(&args[i], 0, sizeof(args[i]));
            args[i].list = &list;
            args[i].iterations = iterations;
            expect_true(pthread_create(&threads[i], 0,
                                       tagged_freelist_worker,
                                       &args[i]) == 0,
                        "tagged freelist create thread");
        }
        for (i = 0; i < thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0,
                        "tagged freelist join thread");
            expect_true(args[i].ok, "tagged freelist worker ok");
        }
    }
#endif

    while ((link = uhm_tagged_freelist_pop(&list)) != 0) {
        tagged_freelist_test_node *node = (tagged_freelist_test_node *)link;
        expect_true(node->id < node_count, "tagged freelist drain id");
        expect_true(atomic_exchange_explicit(&node->in_use, 1u,
                                             UHM_ATOMIC_ACQ_REL) == 0u,
                    "tagged freelist drain unique");
        total_visits += atomic_load_explicit(&node->visits,
                                             UHM_ATOMIC_RELAXED);
        ++drained;
    }
    expect_true(drained == node_count, "tagged freelist drains all nodes");
    expect_true(total_visits == (uint64_t)thread_count * (uint64_t)iterations,
                "tagged freelist visit accounting");
    expect_true(uhm_tagged_freelist_tag_overflows(&list) != 0,
                "tagged freelist contention overflow counted");
    expect_true(uhm_tagged_freelist_cas_failures(&list) != 0,
                "tagged freelist CAS failures observed");
}

