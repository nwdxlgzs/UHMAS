static bench_result bench_treap_malloc_free_pressure(bench_heap_env *env,
                                                     size_t iterations,
                                                     size_t page_size) {
    void **targets;
    void **guards;
    size_t i;
    uint64_t begin;
    uint64_t end;

    targets = (void **)calloc(iterations, sizeof(targets[0]));
    guards = (void **)calloc(iterations, sizeof(guards[0]));
    bench_expect(targets != 0 && guards != 0,
                 "treap malloc pressure arrays");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t target_size = page_size * (2u + (i & 7u));
        size_t guard_size = page_size * (1u + ((i + 3u) & 3u));
        targets[i] = env->api->malloc(env->heap, target_size);
        bench_expect(targets[i] != 0, "treap malloc pressure target");
        guards[i] = env->api->malloc(env->heap, guard_size);
        bench_expect(guards[i] != 0, "treap malloc pressure guard");
        memset(targets[i], (int)i, 64u);
        memset(guards[i], (int)(i + 1u), 64u);
    }
    for (i = 0; i < iterations; ++i) {
        env->api->free(env->heap, targets[i]);
        targets[i] = 0;
    }
    for (i = 0; i < iterations; i += 2u) {
        size_t target_size = page_size * (2u + (i & 7u));
        targets[i] = env->api->malloc(env->heap, target_size);
        bench_expect(targets[i] != 0, "treap malloc pressure reuse");
        memset(targets[i], (int)(i + 2u), 64u);
    }
    end = bench_now_ns();
    free(guards);
    free(targets);
    return bench_result_make("treap malloc free-tree pressure", begin, end,
                             (iterations * 3u) + ((iterations + 1u) / 2u));
}

static bench_result bench_treap_malloc_same_size_pressure(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    void **targets;
    void **guards;
    void **reuse;
    size_t target_size;
    size_t guard_size;
    size_t reuse_count = 0;
    size_t operations = 0;
    size_t i;
    uint64_t begin;
    uint64_t end;

    targets = (void **)calloc(iterations, sizeof(targets[0]));
    guards = (void **)calloc(iterations, sizeof(guards[0]));
    reuse = (void **)calloc(iterations, sizeof(reuse[0]));
    bench_expect(targets != 0 && guards != 0 && reuse != 0,
                 "treap same-size malloc pressure arrays");
    target_size = page_size * 3u + 128u;
    guard_size = page_size + 256u;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        targets[i] = env->api->malloc(env->heap, target_size);
        bench_expect(targets[i] != 0,
                     "treap same-size malloc pressure target");
        guards[i] = env->api->malloc(env->heap, guard_size);
        bench_expect(guards[i] != 0,
                     "treap same-size malloc pressure guard");
        memset(targets[i], (int)i, 64u);
        memset(guards[i], (int)(i + 1u), 64u);
        operations += 2u;
    }
    for (i = 0; i < iterations; ++i) {
        env->api->free(env->heap, targets[i]);
        targets[i] = 0;
        ++operations;
    }
    for (i = 0; i < iterations; i += 3u) {
        reuse[reuse_count] = env->api->malloc(env->heap, target_size);
        bench_expect(reuse[reuse_count] != 0,
                     "treap same-size malloc pressure reuse");
        memset(reuse[reuse_count], (int)(i + 2u), 64u);
        ++reuse_count;
        ++operations;
    }
    for (i = 0; i < reuse_count; i += 2u) {
        env->api->free(env->heap, reuse[i]);
        reuse[i] = 0;
        ++operations;
    }
    for (i = 0; i < iterations && reuse_count < iterations; i += 5u) {
        reuse[reuse_count] = env->api->malloc(env->heap, target_size);
        bench_expect(reuse[reuse_count] != 0,
                     "treap same-size malloc pressure second reuse");
        memset(reuse[reuse_count], (int)(i + 3u), 64u);
        ++reuse_count;
        ++operations;
    }
    end = bench_now_ns();
    free(reuse);
    free(guards);
    free(targets);
    return bench_result_make("treap same-size malloc free-tree pressure",
                             begin, end, operations);
}

static bench_result bench_treap_malloc_remove_pressure(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    void **targets;
    void **guards;
    size_t target_size;
    size_t guard_size;
    size_t operations = 0;
    size_t i;
    uint64_t begin;
    uint64_t end;

    targets = (void **)calloc(iterations, sizeof(targets[0]));
    guards = (void **)calloc(iterations, sizeof(guards[0]));
    bench_expect(targets != 0 && guards != 0,
                 "treap malloc remove pressure arrays");
    target_size = page_size * 3u + 128u;
    guard_size = page_size + 512u;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        targets[i] = env->api->malloc(env->heap, target_size);
        bench_expect(targets[i] != 0,
                     "treap malloc remove pressure target");
        guards[i] = env->api->malloc(env->heap, guard_size);
        bench_expect(guards[i] != 0, "treap malloc remove pressure guard");
        memset(targets[i], (int)i, 64u);
        memset(guards[i], (int)(i + 11u), 64u);
        operations += 2u;
    }
    for (i = 0; i < iterations; ++i) {
        env->api->free(env->heap, targets[i]);
        targets[i] = 0;
        ++operations;
    }
    for (i = 0; i < iterations; ++i) {
        targets[i] = env->api->malloc(env->heap, target_size);
        bench_expect(targets[i] != 0,
                     "treap malloc remove pressure drain reuse");
        memset(targets[i], (int)(i + 23u), 64u);
        ++operations;
    }
    end = bench_now_ns();
    free(guards);
    free(targets);
    return bench_result_make("treap malloc free-tree remove pressure",
                             begin, end, operations);
}

static void *bench_malloc_block_payload(uhm_block *block) {
    size_t header = (sizeof(uhm_block) + (UHM_MIN_ALIGNMENT - 1u)) &
                    ~(size_t)(UHM_MIN_ALIGNMENT - 1u);

    return block ? (void *)((uintptr_t)block + header) : 0;
}

static size_t bench_malloc_free_tree_max_total(const uhm_block *node) {
    size_t left;
    size_t right;

    if (!node) {
        return 0;
    }
    left = bench_malloc_free_tree_max_total(node->free_tree_left);
    right = bench_malloc_free_tree_max_total(node->free_tree_right);
    if (left < right) {
        left = right;
    }
    return left > node->total_size ? left : node->total_size;
}

static uhm_block *bench_treap_malloc_two_child_candidate(uhm_block *node) {
    uhm_block *candidate;

    if (!node) {
        return 0;
    }
    candidate = bench_treap_malloc_two_child_candidate(node->free_tree_left);
    if (candidate) {
        return candidate;
    }
    if (node->free_tree_left && node->free_tree_right &&
        bench_malloc_free_tree_max_total(node->free_tree_left) <
            node->total_size) {
        return node;
    }
    return bench_treap_malloc_two_child_candidate(node->free_tree_right);
}

static bench_result bench_treap_malloc_two_child_remove_pressure(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    void **targets;
    void **guards;
    size_t build_count = iterations;
    size_t operations = 0;
    size_t i;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t sink_before;
#endif
    uint64_t begin;
    uint64_t end;

    if (build_count < 64u) {
        build_count = 64u;
    }
    if (build_count > 768u) {
        build_count = 768u;
    }
    targets = (void **)calloc(build_count, sizeof(targets[0]));
    guards = (void **)calloc(build_count, sizeof(guards[0]));
    bench_expect(targets != 0 && guards != 0,
                 "treap malloc two-child remove arrays");

    for (i = 0; i < build_count; ++i) {
        size_t target_size = page_size + 1024u + i * 64u;
        size_t guard_size = page_size + 512u;
        targets[i] = env->api->malloc(env->heap, target_size);
        bench_expect(targets[i] != 0,
                     "treap malloc two-child remove target");
        guards[i] = env->api->malloc(env->heap, guard_size);
        bench_expect(guards[i] != 0,
                     "treap malloc two-child remove guard");
        memset(targets[i], (int)i, 64u);
        memset(guards[i], (int)(i + 17u), 64u);
    }
    for (i = 0; i < build_count; ++i) {
        env->api->free(env->heap, targets[i]);
        targets[i] = 0;
    }

#if UHMAS_ENABLE_DIAGNOSTICS
    sink_before = env->heap->core.malloc_free_tree_remove_sink_steps;
#endif
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        uhm_block *target =
            bench_treap_malloc_two_child_candidate(
                env->heap->core.malloc_free_tree_root);
        void *expected_payload;
        void *reuse;
        size_t header_size;
        size_t request_size;

        bench_expect(target != 0,
                     "treap malloc two-child remove candidate");
        expected_payload = bench_malloc_block_payload(target);
        header_size = (size_t)((uintptr_t)expected_payload -
                               (uintptr_t)target);
        bench_expect(target->total_size > header_size,
                     "treap malloc two-child remove payload room");
        request_size = target->total_size - header_size;
        reuse = env->api->malloc(env->heap, request_size);
        bench_expect(reuse == expected_payload,
                     "treap malloc two-child remove exact reuse");
        env->api->free(env->heap, reuse);
        operations += 2u;
    }
    end = bench_now_ns();

#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(env->heap->core.malloc_free_tree_remove_sink_steps >
                     sink_before,
                 "treap malloc two-child remove sink covered");
#endif
    for (i = 0; i < build_count; ++i) {
        env->api->free(env->heap, guards[i]);
    }
    free(guards);
    free(targets);
    return bench_result_make(
        "treap malloc free-tree two-child remove pressure", begin, end,
        operations);
}

static bench_result bench_treap_vma_pressure(bench_heap_env *env,
                                             size_t iterations,
                                             size_t page_size) {
    void **maps;
    size_t i;
    uint64_t begin;
    uint64_t end;

    maps = (void **)calloc(iterations, sizeof(maps[0]));
    bench_expect(maps != 0, "treap vma pressure maps");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = page_size * (1u + (i & 3u));
        uint32_t prot = (i & 1u) ? (UHM_PROT_READ | UHM_PROT_WRITE)
                                 : UHM_PROT_READ;
        bench_expect_ok(env->api->mmap(env->heap, &maps[i], size,
                                       prot, UHM_MAP_PRIVATE),
                        "treap vma pressure mmap");
        if (prot & UHM_PROT_WRITE) {
            memset(maps[i], (int)i, page_size);
        }
    }
    for (i = 0; i < iterations; ++i) {
        size_t size = page_size * (1u + (i & 3u));
        uint32_t prot = (i & 1u) ? (UHM_PROT_READ | UHM_PROT_WRITE)
                                 : UHM_PROT_READ;
        bench_expect_ok(env->api->mprotect(env->heap, maps[i], size,
                                           prot),
                        "treap vma pressure mprotect");
    }
    end = bench_now_ns();
    free(maps);
    return bench_result_make("treap vma live-tree pressure", begin, end,
                             iterations * 2u);
}

typedef struct bench_treap_fixed_sparse_vma_state {
    void **maps;
    void *span;
    uintptr_t base;
    size_t span_pages;
    size_t span_size;
    size_t iterations;
    size_t page_size;
} bench_treap_fixed_sparse_vma_state;

static void bench_treap_fixed_sparse_vma_prepare(
    bench_heap_env *env, bench_treap_fixed_sparse_vma_state *state,
    size_t iterations, size_t page_size) {
    void *span = 0;

    memset(state, 0, sizeof(*state));
    state->iterations = iterations;
    state->page_size = page_size;
    state->maps = (void **)calloc(iterations, sizeof(state->maps[0]));
    bench_expect(state->maps != 0, "treap fixed sparse VMA maps");
    bench_expect(page_size != 0, "treap fixed sparse VMA page size");
    bench_expect(iterations <= (SIZE_MAX / page_size - 1u) / 2u,
                 "treap fixed sparse VMA span fits size_t");
    state->span_pages = iterations * 2u + 1u;
    state->span_size = state->span_pages * page_size;

    bench_expect_ok(env->api->mmap(env->heap, &span, state->span_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "treap fixed sparse VMA setup mmap");
    bench_expect(span != 0, "treap fixed sparse VMA setup address");
    bench_expect_ok(env->api->munmap(env->heap, span, state->span_size),
                    "treap fixed sparse VMA setup munmap");
    state->span = span;
    state->base = (uintptr_t)span;
}

static void bench_treap_fixed_sparse_vma_cleanup(
    bench_treap_fixed_sparse_vma_state *state) {
    free(state->maps);
    memset(state, 0, sizeof(*state));
}

static bench_result bench_treap_fixed_sparse_vma_build_pressure(
    bench_heap_env *env, bench_treap_fixed_sparse_vma_state *state) {
    size_t operations = 0;
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < state->iterations; ++i) {
        void *addr = (void *)(state->base + (uintptr_t)i * 2u *
                                             (uintptr_t)state->page_size);
        bench_expect_ok(env->api->mmap(env->heap, &addr, state->page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE |
                                           UHM_MAP_FIXED_NOREPLACE),
                        "treap fixed sparse VMA mmap");
        bench_expect(addr == (void *)(state->base + (uintptr_t)i * 2u *
                                                     (uintptr_t)state->page_size),
                     "treap fixed sparse VMA address");
        state->maps[i] = addr;
        memset(addr, (int)i, state->page_size);
        ++operations;
    }
    end = bench_now_ns();
    return bench_result_make("treap fixed sparse VMA build pressure", begin,
                             end, operations);
}

static bench_result bench_treap_fixed_sparse_vma_protect_pressure(
    bench_heap_env *env, bench_treap_fixed_sparse_vma_state *state) {
    size_t operations = 0;
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < state->iterations; ++i) {
        uint32_t prot = (i & 1u) ? (UHM_PROT_READ | UHM_PROT_WRITE)
                                 : UHM_PROT_READ;
        bench_expect_ok(env->api->mprotect(env->heap, state->maps[i],
                                           state->page_size, prot),
                        "treap fixed sparse VMA mprotect");
        ++operations;
    }
    end = bench_now_ns();
    return bench_result_make("treap fixed sparse VMA protect pressure", begin,
                             end, operations);
}

static bench_result bench_treap_mmap_free_pressure(bench_heap_env *env,
                                                   size_t iterations,
                                                   size_t page_size) {
    void **targets;
    void **guards;
    size_t *target_sizes;
    size_t *guard_sizes;
    size_t i;
    uint64_t begin;
    uint64_t end;

    targets = (void **)calloc(iterations, sizeof(targets[0]));
    guards = (void **)calloc(iterations, sizeof(guards[0]));
    target_sizes = (size_t *)calloc(iterations, sizeof(target_sizes[0]));
    guard_sizes = (size_t *)calloc(iterations, sizeof(guard_sizes[0]));
    bench_expect(targets != 0 && guards != 0 && target_sizes != 0 &&
                     guard_sizes != 0,
                 "treap mmap free pressure arrays");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        target_sizes[i] = page_size * (1u + (i & 3u));
        guard_sizes[i] = page_size * (1u + ((i + 1u) & 3u));
        bench_expect_ok(env->api->mmap(env->heap, &targets[i],
                                       target_sizes[i],
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "treap mmap free pressure target");
        bench_expect_ok(env->api->mmap(env->heap, &guards[i],
                                       guard_sizes[i],
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "treap mmap free pressure guard");
        memset(targets[i], (int)i, page_size);
        memset(guards[i], (int)(i + 1u), page_size);
    }
    for (i = 0; i < iterations; ++i) {
        bench_expect_ok(env->api->munmap(env->heap, targets[i],
                                         target_sizes[i]),
                        "treap mmap free pressure munmap");
        targets[i] = 0;
    }
    for (i = 0; i < iterations; i += 2u) {
        bench_expect_ok(env->api->mmap(env->heap, &targets[i],
                                       target_sizes[i],
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "treap mmap free pressure reuse");
        memset(targets[i], (int)(i + 2u), page_size);
    }
    end = bench_now_ns();
    free(guard_sizes);
    free(target_sizes);
    free(guards);
    free(targets);
    return bench_result_make("treap mmap free-tree pressure", begin, end,
                             (iterations * 3u) + ((iterations + 1u) / 2u));
}

static void bench_control_allocator_hint_window_prepare(bench_heap_env *env,
                                                        size_t page_size) {
    void *setup_map = 0;

    bench_expect(env && env->heap && env->api,
                 "control allocator hint window env");
    bench_expect(page_size != 0, "control allocator hint window page size");

    env->heap->core.control_alloc = env->heap->core.control_end;
    env->heap->core.control_recent_extent = 0;
    memset(env->heap->core.control_alloc_hint, 0,
           sizeof(env->heap->core.control_alloc_hint));
    bench_expect_ok(env->api->mmap(env->heap, &setup_map, page_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "control allocator hint window setup mmap");
    bench_expect(setup_map != 0, "control allocator hint window setup addr");
    bench_expect(env->heap->core.control_extents != 0,
                 "control allocator hint window secondary extent");
    bench_expect(env->heap->core.control_extents->magic == UHM_CONTROL_MAGIC,
                 "control allocator hint window extent magic");
}

static bench_result bench_control_allocator_hint_window_churn(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    void **maps;
    uhm_control_extent *extent;
    size_t i;
    uint64_t begin;
    uint64_t end;

    bench_expect(iterations != 0, "control allocator hint window iterations");
    maps = (void **)calloc(iterations, sizeof(maps[0]));
    bench_expect(maps != 0, "control allocator hint window maps");
    extent = env->heap->core.control_extents;
    bench_expect(extent != 0 && extent->magic == UHM_CONTROL_MAGIC,
                 "control allocator hint window live extent");

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t bucket = UHM_CONTROL_ALLOC_HINT_BUCKETS - 1u;

        env->heap->core.control_recent_extent = 0;
        memset(env->heap->core.control_alloc_hint, 0,
               sizeof(env->heap->core.control_alloc_hint));
        env->heap->core.control_alloc_hint[bucket][0] =
            (uhm_control_extent *)env->heap;
        env->heap->core.control_alloc_hint[bucket][1] = extent;
        bench_expect_ok(env->api->mmap(env->heap, &maps[i], page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "control allocator hint window mmap");
        bench_expect(maps[i] != 0, "control allocator hint window address");
    }
    end = bench_now_ns();

    free(maps);
    return bench_result_make("control allocator hint-window churn", begin,
                             end, iterations);
}

static bench_result bench_control_allocator_recent_extent_churn(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    void **maps;
    size_t i;
    uint64_t begin;
    uint64_t end;

    bench_expect(iterations != 0, "control allocator recent extent iterations");
    maps = (void **)calloc(iterations, sizeof(maps[0]));
    bench_expect(maps != 0, "control allocator recent extent maps");

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        bench_expect_ok(env->api->mmap(env->heap, &maps[i], page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "control allocator recent extent mmap");
        bench_expect(maps[i] != 0, "control allocator recent extent address");
    }
    end = bench_now_ns();

    free(maps);
    return bench_result_make("control allocator recent-extent churn", begin,
                             end, iterations);
}

static bench_result bench_collision_pressure(bench_heap_env *env,
                                              size_t page_size) {
    enum { max_mallocs = 4096, max_maps = 4096 };
    void *mallocs[max_mallocs];
    void *maps[max_maps];
    size_t malloc_count = 0;
    size_t map_count = 0;
    size_t step = 0;
    uint64_t begin;
    uint64_t end;

    memset(mallocs, 0, sizeof(mallocs));
    memset(maps, 0, sizeof(maps));
    begin = bench_now_ns();
    while (malloc_count < max_mallocs && map_count < max_maps) {
        void *ptr = env->api->malloc(env->heap, 2048u + ((step % 8u) * 512u));
        void *map = 0;
        if (!ptr) {
            break;
        }
        mallocs[malloc_count++] = ptr;
        if (env->api->mmap(env->heap, &map, page_size,
                           UHM_PROT_READ | UHM_PROT_WRITE,
                           UHM_MAP_PRIVATE) != UHM_OK) {
            break;
        }
        maps[map_count++] = map;
        ++step;
    }
    bench_check_heap(env, "collision pressure check after allocation phase");
    while (map_count != 0) {
        --map_count;
        (void)env->api->munmap(env->heap, maps[map_count], page_size);
    }
    bench_check_heap(env, "collision pressure check after mmap cleanup");
    while (malloc_count != 0) {
        --malloc_count;
        env->api->free(env->heap, mallocs[malloc_count]);
    }
    bench_check_heap(env, "collision pressure check after malloc cleanup");
    end = bench_now_ns();
    return bench_result_make("uhm malloc/mmap collision pressure",
                             begin, end, step * 2u);
}

static bench_result bench_slab_empty_churn(bench_heap_env *env,
                                           size_t payload_size,
                                           const char *result_name) {
    void **ptrs;
    void *first;
    uhm_slab *first_slab;
    uint16_t size_class;
    size_t slot_count;
    size_t base_high_water;
    size_t slab_count;
    size_t ptr_count;
    size_t i;
    size_t operations = 0;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t pressure_before;
    uint64_t pressure_before_second_free;
    uint64_t grow_before;
#endif
    uint64_t recommit_before_committed_reuse;
    uint64_t begin;
    uint64_t end;

    first = env->api->malloc(env->heap, payload_size);
    bench_expect(first != 0, "slab empty churn first allocation");
    memset(first, 0xa5, payload_size);
    first_slab = bench_find_slab_by_payload(env, first);
    bench_expect(first_slab != 0, "slab empty churn first slab");
    size_class = first_slab->size_class;
    bench_expect(size_class < UHM_SIZE_CLASS_COUNT,
                 "slab empty churn valid size class");
    slot_count = first_slab->total_slots;
    bench_expect(slot_count != 0, "slab empty churn non-empty slab");
    base_high_water = uhm_slab_empty_committed_cache_state_high_water(
        env->heap->core.slab_empty_committed_cache_high_water[size_class]);
    if (base_high_water == 0) {
        base_high_water = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
    }
    bench_expect(base_high_water <
                     UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER,
                 "slab empty churn high water has headroom");
    slab_count = base_high_water + 1u;
    bench_expect(slot_count <= SIZE_MAX / slab_count,
                 "slab empty churn pointer count overflow");
    ptr_count = slot_count * slab_count;
    ptrs = (void **)calloc(ptr_count, sizeof(ptrs[0]));
    bench_expect(ptrs != 0, "slab empty churn pointer table");
    ptrs[0] = first;

#if UHMAS_ENABLE_DIAGNOSTICS
    pressure_before = env->heap->core.slab_empty_decommit_pressure_events;
    grow_before = env->heap->core.slab_empty_committed_cache_adaptive_grow_events;
#endif
    begin = bench_now_ns();
    for (i = 1; i < ptr_count; ++i) {
        ptrs[i] = env->api->malloc(env->heap, payload_size);
        bench_expect(ptrs[i] != 0, "slab empty churn fill");
        memset(ptrs[i], (int)(i & 0xffu), payload_size);
        ++operations;
    }
    for (i = 0; i < ptr_count; ++i) {
        env->api->free(env->heap, ptrs[i]);
        ptrs[i] = 0;
        ++operations;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(env->heap->core.slab_empty_decommit_pressure_events >
                     pressure_before,
                 "slab empty churn first cycle decommits pressure slab");
#endif
    for (i = 0; i < ptr_count; ++i) {
        ptrs[i] = env->api->malloc(env->heap, payload_size);
        bench_expect(ptrs[i] != 0, "slab empty churn refill");
        memset(ptrs[i], (int)((i + 17u) & 0xffu), payload_size);
        ++operations;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(env->heap->core.slab_empty_committed_cache_adaptive_grow_events >
                     grow_before,
                 "slab empty churn adaptive high water grows");
#endif
    bench_expect(uhm_slab_empty_committed_cache_state_high_water(
                     env->heap->core.slab_empty_committed_cache_high_water
                         [size_class]) > base_high_water,
                 "slab empty churn current high water increased");

#if UHMAS_ENABLE_DIAGNOSTICS
    pressure_before_second_free =
        env->heap->core.slab_empty_decommit_pressure_events;
#endif
    for (i = 0; i < ptr_count; ++i) {
        env->api->free(env->heap, ptrs[i]);
        ptrs[i] = 0;
        ++operations;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(env->heap->core.slab_empty_decommit_pressure_events ==
                     pressure_before_second_free,
                 "slab empty churn second free avoids decommit pressure");
#endif

    recommit_before_committed_reuse = env->heap->core.recommit_events;
    for (i = 0; i < ptr_count; ++i) {
        ptrs[i] = env->api->malloc(env->heap, payload_size);
        bench_expect(ptrs[i] != 0, "slab empty churn committed reuse");
        memset(ptrs[i], (int)((i + 31u) & 0xffu), payload_size);
        ++operations;
    }
    bench_expect(env->heap->core.recommit_events ==
                     recommit_before_committed_reuse,
                 "slab empty churn committed reuse avoids recommit");
    for (i = 0; i < ptr_count; ++i) {
        env->api->free(env->heap, ptrs[i]);
        ptrs[i] = 0;
        ++operations;
    }
    end = bench_now_ns();
    free(ptrs);

    return bench_result_make(result_name, begin, end, operations);
}

enum { BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT = 9u };

static bench_result bench_slab_empty_full_drain_batch(
    bench_heap_env *env, bench_diag_snapshot *diag_before,
    uhm_thread_ctx **contexts_out, void **ctx_raws_out) {
    const size_t payload_size = 512u;
    const size_t batch_excess = 12u;
    uhm_storage_info ctx_info;
    uhm_thread_config ctx_config;
    void **ptrs = 0;
    uhm_slab *slab_groups[BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT]
                           [UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX] = {{0}};
    size_t page_size;
    size_t high_water = 0u;
    size_t slot_count = 0u;
    size_t max_ptr_count = 0u;
    uint16_t size_class = UHM_SIZE_CLASS_COUNT;
    uint64_t latencies[BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT] = {0};
    size_t sample;
    size_t i;
    size_t j;
    uint64_t total_begin;
    uint64_t total_end;
    bench_result result;

    bench_expect(env != 0 && env->heap != 0 && env->api != 0,
                 "slab full-drain batch environment");
    bench_expect(diag_before != 0 && contexts_out != 0 &&
                     ctx_raws_out != 0,
                 "slab full-drain batch outputs");
    memset(contexts_out, 0,
           BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT * sizeof(contexts_out[0]));
    memset(ctx_raws_out, 0,
           BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT * sizeof(ctx_raws_out[0]));
    page_size = env->heap->F.vm.page_size;
    bench_expect(page_size != 0u,
                 "slab full-drain batch runtime page size");

    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.size = sizeof(ctx_info);
    bench_expect_ok(uhm_heap_storage_query(0, &ctx_info),
                    "slab full-drain batch ctx storage query");
    bench_expect(page_size <=
                     SIZE_MAX / (2u * UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX),
                 "slab full-drain batch ctx cache size overflow");
    bench_expect(env->api->thread_ctx_open != 0 &&
                     env->api->thread_ctx_close != 0 &&
                     env->api->thread_ctx_flush != 0 &&
                     env->api->malloc_ctx != 0 &&
                     env->api->free_ctx != 0,
                 "slab full-drain batch ctx API available");

    for (sample = 0u; sample < BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT;
         ++sample) {
        void *ctx_storage;
        void *first;
        uhm_slab *first_slab;
        size_t slab_count;
        size_t ptr_count;
        size_t unique_count = 0u;

        ctx_storage = bench_aligned_host_alloc(
            ctx_info.storage_size, ctx_info.storage_alignment,
            &ctx_raws_out[sample]);
        bench_expect(ctx_storage != 0,
                     "slab full-drain batch ctx storage alloc");
        memset(&ctx_config, 0, sizeof(ctx_config));
        ctx_config.size = sizeof(ctx_config);
        ctx_config.abi_major = UHM_ABI_MAJOR;
        ctx_config.abi_minor = UHM_ABI_MINOR;
        ctx_config.storage = ctx_storage;
        ctx_config.storage_size = ctx_info.storage_size;
        ctx_config.local_cache_bytes =
            2u * UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX * page_size;
        bench_expect_ok(
            env->api->thread_ctx_open(env->heap, &ctx_config,
                                      &contexts_out[sample]),
            "slab full-drain batch ctx open");
        first = env->api->malloc_ctx(env->heap, contexts_out[sample],
                                     payload_size);
        bench_expect(first != 0,
                     "slab full-drain batch first allocation");
        first_slab = bench_find_slab_by_payload(env, first);
        bench_expect(first_slab != 0,
                     "slab full-drain batch first slab");
        if (sample == 0u) {
            size_class = first_slab->size_class;
            slot_count = first_slab->total_slots;
            high_water = uhm_slab_empty_committed_cache_state_high_water(
                env->heap->core.slab_empty_committed_cache_high_water
                    [size_class]);
            if (high_water == 0u) {
                high_water = UHM_SLAB_EMPTY_COMMITTED_CACHE_HIGH_WATER;
            }
            bench_expect(
                high_water <=
                    UHM_SLAB_EMPTY_DECOMMIT_BATCH_MAX - batch_excess,
                "slab full-drain batch bounded high water");
            slab_count = high_water + batch_excess;
            bench_expect(slot_count != 0u &&
                             slot_count <= SIZE_MAX / slab_count,
                         "slab full-drain batch pointer count");
            max_ptr_count = slot_count * slab_count;
            ptrs = (void **)calloc(max_ptr_count, sizeof(ptrs[0]));
            bench_expect(ptrs != 0,
                         "slab full-drain batch pointer table");
        } else {
            bench_expect(first_slab->size_class == size_class &&
                             first_slab->total_slots == slot_count,
                         "slab full-drain batch stable slab geometry");
            slab_count = batch_excess;
        }
        ptr_count = slot_count * slab_count;
        bench_expect(ptr_count != 0u && ptr_count <= max_ptr_count,
                     "slab full-drain batch group pointer bound");
        memset(ptrs, 0, max_ptr_count * sizeof(ptrs[0]));
        ptrs[0] = first;
        for (i = 1u; i < ptr_count; ++i) {
            ptrs[i] = env->api->malloc_ctx(
                env->heap, contexts_out[sample], payload_size);
            bench_expect(ptrs[i] != 0,
                         "slab full-drain batch fill allocation");
            *(volatile unsigned char *)ptrs[i] =
                (unsigned char)(i + sample);
        }
        for (i = 0u; i < ptr_count; ++i) {
            uhm_slab *slab = bench_find_slab_by_payload(env, ptrs[i]);

            bench_expect(slab != 0 && slab->size_class == size_class,
                         "slab full-drain batch allocation owner");
            for (j = 0u; j < unique_count; ++j) {
                if (slab_groups[sample][j] == slab) {
                    break;
                }
            }
            if (j == unique_count) {
                bench_expect(unique_count < slab_count,
                             "slab full-drain batch unique slab bound");
                slab_groups[sample][unique_count++] = slab;
            }
        }
        bench_expect(unique_count == slab_count,
                     "slab full-drain batch exact slab count");
        for (i = 1u; i < unique_count; ++i) {
            uhm_slab *slab = slab_groups[sample][i];
            j = i;
            while (j != 0u &&
                   slab_groups[sample][j - 1u]->base > slab->base) {
                slab_groups[sample][j] = slab_groups[sample][j - 1u];
                --j;
            }
            slab_groups[sample][j] = slab;
        }
        for (i = 0u; i < slab_count; ++i) {
            bench_expect(slab_groups[sample][i]->size == page_size,
                         "slab full-drain batch page-sized slab");
            if (i != 0u) {
                bench_expect(
                    slab_groups[sample][i - 1u]->base +
                            slab_groups[sample][i - 1u]->size ==
                        slab_groups[sample][i]->base,
                    "slab full-drain batch contiguous slabs");
            }
        }
        for (i = 0u; i < ptr_count; ++i) {
            env->api->free_ctx(env->heap, contexts_out[sample], ptrs[i]);
            ptrs[i] = 0;
        }
        bench_expect(contexts_out[sample]->local_cache_count == ptr_count &&
                         contexts_out[sample]->retained_slab_pages ==
                             slab_count,
                     "slab full-drain batch ctx retains complete slabs");
    }

    bench_diag_capture(env, diag_before);
    total_begin = bench_now_ns();
    for (sample = 0u; sample < BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT;
         ++sample) {
        uint64_t decommit_events_before = env->heap->core.decommit_events;
        uint64_t decommit_bytes_before = env->heap->core.decommit_bytes;
        uint64_t vm_decommit_calls_before =
            env->heap->core.vm_decommit_calls;
        uint64_t vm_decommit_failures_before =
            env->heap->core.vm_decommit_failures;
        uint64_t vm_lock_held_calls_before =
            env->heap->core.vm_lock_held_calls;
        uint64_t vm_decommit_lock_held_calls_before =
            env->heap->core.vm_decommit_lock_held_calls;
#if UHMAS_ENABLE_DIAGNOSTICS
        uint64_t pressure_before =
            env->heap->core.slab_empty_decommit_pressure_events;
        uint64_t pressure_bytes_before =
            env->heap->core.slab_empty_decommit_pressure_bytes;
        uint64_t page_span_pages_before = uhm_diag_counter_load_relaxed(
            &env->heap->core.page_span_pages);
        uint64_t page_mark_pages_before = uhm_diag_counter_load_relaxed(
            &env->heap->core.page_mark_pages);
#endif
        uint64_t begin = bench_now_ns();
        uint64_t end;

        bench_expect_ok(
            env->api->thread_ctx_flush(env->heap, contexts_out[sample]),
            "slab full-drain batch ctx flush");
        end = bench_now_ns();
        latencies[sample] = end - begin;
        bench_expect(contexts_out[sample]->local_cache_count == 0u &&
                         contexts_out[sample]->retained_slab_pages == 0u,
                     "slab full-drain batch ctx drained");
#if UHMAS_ENABLE_DIAGNOSTICS
        bench_expect(
            env->heap->core.slab_empty_decommit_pressure_events ==
                    pressure_before + batch_excess &&
                env->heap->core.slab_empty_decommit_pressure_bytes ==
                    pressure_bytes_before + batch_excess * page_size,
            "slab full-drain batch logical pressure accounting");
#endif
        bench_expect(
            env->heap->core.decommit_events ==
                    decommit_events_before + batch_excess &&
                env->heap->core.decommit_bytes ==
                    decommit_bytes_before + batch_excess * page_size,
            "slab full-drain batch logical decommit accounting");
        bench_expect(
            env->heap->core.vm_decommit_calls ==
                    vm_decommit_calls_before + 1u &&
                env->heap->core.vm_decommit_failures ==
                    vm_decommit_failures_before,
            "slab full-drain batch one successful platform call");
        bench_expect(
            env->heap->core.vm_lock_held_calls ==
                    vm_lock_held_calls_before &&
                env->heap->core.vm_decommit_lock_held_calls ==
                    vm_decommit_lock_held_calls_before,
            "slab full-drain batch platform call outside heap lock");
#if UHMAS_ENABLE_DIAGNOSTICS
        bench_expect(
            uhm_diag_counter_load_relaxed(
                &env->heap->core.page_span_pages) >=
                    page_span_pages_before + batch_excess &&
                uhm_diag_counter_load_relaxed(
                    &env->heap->core.page_mark_pages) ==
                    page_mark_pages_before + batch_excess,
            "slab full-drain batch bounded page-map work");
#endif
        bench_expect(
            env->heap->core.slab_committed_empty_count[size_class] ==
                high_water,
            "slab full-drain batch preserves committed high water");
    }
    total_end = bench_now_ns();

    free(ptrs);
    result = bench_result_make(
        "slab empty full-drain batch 512B", total_begin, total_end,
        BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT);
    bench_result_set_latency(&result, latencies,
                             BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT);
    bench_result_set_latency_units(&result, batch_excess, batch_excess);
    return result;
}

/* Keep the close-lifecycle module at its original unity-TU code position. */
#include "bench_uhm_heap_close.inc"
#include "bench_uhm_heap_close_tls.inc"
#include "bench_uhm_heap_close_mixed.inc"

static bench_result bench_release_pressure(bench_heap_env *env,
                                           size_t iterations,
                                           size_t page_size);
static bench_result bench_release_pressure_with_latency(bench_heap_env *env,
                                                        size_t iterations,
                                                        size_t page_size,
                                                        const char *result_name);

static bench_result bench_release_malloc_churn(bench_heap_env *env,
                                               size_t iterations) {
    enum { slot_count = 96 };
    void *mallocs[slot_count];
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies =
        (uint64_t *)calloc(iterations + slot_count, sizeof(latencies[0]));
    bench_expect(latencies != 0, "release breakdown malloc latency samples");
    memset(mallocs, 0, sizeof(mallocs));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 13u) % slot_count;
        uint64_t op_begin = bench_now_ns();
        if (mallocs[slot]) {
            env->api->free(env->heap, mallocs[slot]);
        }
        mallocs[slot] = env->api->malloc(env->heap,
                                         64u + ((i * 37u) % 8192u));
        latencies[i] = bench_now_ns() - op_begin;
        bench_expect(mallocs[slot] != 0, "release breakdown malloc");
        memset(mallocs[slot], (int)i, 64u);
    }
    for (i = 0; i < slot_count; ++i) {
        uint64_t op_begin = bench_now_ns();
        env->api->free(env->heap, mallocs[i]);
        latencies[iterations + i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    result = bench_result_make("release breakdown malloc churn", begin, end,
                               iterations + slot_count);
    bench_result_set_latency(&result, latencies, iterations + slot_count);
    free(latencies);
    return result;
}

static bench_result bench_release_free_burst(bench_heap_env *env,
                                             size_t iterations) {
    void **ptrs;
    uint64_t *latencies;
    size_t i;
    size_t count = iterations > 16384u ? 16384u : iterations;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    ptrs = (void **)calloc(count, sizeof(ptrs[0]));
    latencies = (uint64_t *)calloc(count * 2u, sizeof(latencies[0]));
    bench_expect(ptrs != 0, "release breakdown free ptrs");
    bench_expect(latencies != 0, "release breakdown free latency samples");
    begin = bench_now_ns();
    for (i = 0; i < count; ++i) {
        uint64_t op_begin = bench_now_ns();
        ptrs[i] = env->api->malloc(env->heap, 64u + ((i * 37u) % 8192u));
        latencies[i] = bench_now_ns() - op_begin;
        bench_expect(ptrs[i] != 0, "release breakdown free setup");
    }
    for (i = 0; i < count; ++i) {
        uint64_t op_begin = bench_now_ns();
        env->api->free(env->heap, ptrs[i]);
        latencies[count + i] = bench_now_ns() - op_begin;
        ptrs[i] = 0;
    }
    end = bench_now_ns();
    free(ptrs);
    result = bench_result_make("release breakdown malloc/free burst", begin,
                               end, count * 2u);
    bench_result_set_latency(&result, latencies, count * 2u);
    free(latencies);
    return result;
}

static size_t bench_release_pressure_limited_count(size_t iterations) {
    return iterations > 4096u ? 4096u : iterations;
}

static void bench_release_pressure_setup_mallocs(bench_heap_env *env,
                                                 void **ptrs,
                                                 size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        ptrs[i] = env->api->malloc(env->heap, 64u + ((i * 37u) % 8192u));
        bench_expect(ptrs[i] != 0, "release pressure matrix malloc setup");
        memset(ptrs[i], (int)(i & 0xffu), 64u);
    }
}

static void bench_release_pressure_cleanup_mallocs(bench_heap_env *env,
                                                   void **ptrs,
                                                   size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (ptrs[i]) {
            env->api->free(env->heap, ptrs[i]);
            ptrs[i] = 0;
        }
    }
}

static bench_result bench_release_pressure_malloc_only(bench_heap_env *env,
                                                       void **ptrs,
                                                       size_t count) {
    uint64_t *latencies;
    uint64_t begin;
    uint64_t end;
    size_t i;

    latencies = (uint64_t *)calloc(count, sizeof(latencies[0]));
    bench_expect(latencies != 0, "release pressure malloc latency samples");
    begin = bench_now_ns();
    for (i = 0; i < count; ++i) {
        uint64_t op_begin = bench_now_ns();
        ptrs[i] = env->api->malloc(env->heap, 64u + ((i * 37u) % 8192u));
        latencies[i] = bench_now_ns() - op_begin;
        bench_expect(ptrs[i] != 0, "release pressure matrix malloc-only");
        memset(ptrs[i], (int)(i & 0xffu), 64u);
    }
    end = bench_now_ns();
    {
        bench_result result =
            bench_result_make("release pressure phase malloc-only", begin,
                              end, count);
        bench_result_set_latency(&result, latencies, count);
        free(latencies);
        return result;
    }
}

static bench_result bench_release_pressure_free_only(bench_heap_env *env,
                                                     void **ptrs,
                                                     size_t count) {
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;

    latencies = (uint64_t *)calloc(count, sizeof(latencies[0]));
    bench_expect(latencies != 0, "release pressure free latency samples");
    begin = bench_now_ns();
    for (i = 0; i < count; ++i) {
        uint64_t op_begin = bench_now_ns();
        env->api->free(env->heap, ptrs[i]);
        latencies[i] = bench_now_ns() - op_begin;
        ptrs[i] = 0;
    }
    end = bench_now_ns();
    {
        bench_result result =
            bench_result_make("release pressure phase free-only", begin, end,
                              count);
        bench_result_set_latency(&result, latencies, count);
        free(latencies);
        return result;
    }
}

static void bench_release_pressure_setup_maps(bench_heap_env *env,
                                              void **maps,
                                              size_t *map_sizes,
                                              size_t count,
                                              size_t page_size) {
    size_t i;

    for (i = 0; i < count; ++i) {
        map_sizes[i] = page_size * (1u + (i % 4u));
        bench_expect_ok(env->api->mmap(env->heap, &maps[i], map_sizes[i],
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "release pressure matrix mmap setup");
        memset(maps[i], (int)(i & 0xffu), page_size);
    }
}

static void bench_release_pressure_cleanup_maps(bench_heap_env *env,
                                                void **maps,
                                                size_t *map_sizes,
                                                size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (maps[i]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[i],
                                             map_sizes[i]),
                            "release pressure matrix mmap cleanup");
            maps[i] = 0;
            map_sizes[i] = 0;
        }
    }
}

static bench_result bench_release_pressure_mmap_only(bench_heap_env *env,
                                                     void **maps,
                                                     size_t *map_sizes,
                                                     size_t count,
                                                     size_t page_size) {
    uint64_t *latencies;
    uint64_t begin;
    uint64_t end;
    size_t i;

    latencies = (uint64_t *)calloc(count, sizeof(latencies[0]));
    bench_expect(latencies != 0, "release pressure mmap latency samples");
    begin = bench_now_ns();
    for (i = 0; i < count; ++i) {
        uint64_t op_begin;

        map_sizes[i] = page_size * (1u + (i % 4u));
        op_begin = bench_now_ns();
        bench_expect_ok(env->api->mmap(env->heap, &maps[i], map_sizes[i],
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "release pressure matrix mmap-only");
        latencies[i] = bench_now_ns() - op_begin;
        memset(maps[i], (int)(i & 0xffu), page_size);
    }
    end = bench_now_ns();
    {
        bench_result result =
            bench_result_make("release pressure phase mmap-only", begin, end,
                              count);
        bench_result_set_latency(&result, latencies, count);
        free(latencies);
        return result;
    }
}

static bench_result bench_release_pressure_munmap_only(bench_heap_env *env,
                                                       void **maps,
                                                       size_t *map_sizes,
                                                       size_t count) {
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;

    latencies = (uint64_t *)calloc(count, sizeof(latencies[0]));
    bench_expect(latencies != 0, "release pressure munmap latency samples");
    begin = bench_now_ns();
    for (i = 0; i < count; ++i) {
        uint64_t op_begin = bench_now_ns();
        bench_expect_ok(env->api->munmap(env->heap, maps[i], map_sizes[i]),
                        "release pressure matrix munmap-only");
        latencies[i] = bench_now_ns() - op_begin;
        maps[i] = 0;
        map_sizes[i] = 0;
    }
    end = bench_now_ns();
    {
        bench_result result =
            bench_result_make("release pressure phase munmap-only", begin,
                              end, count);
        bench_result_set_latency(&result, latencies, count);
        free(latencies);
        return result;
    }
}

static bench_result bench_release_pressure_mprotect_only(bench_heap_env *env,
                                                         void **maps,
                                                         size_t count,
                                                         size_t iterations,
                                                         size_t page_size) {
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;

    latencies = (uint64_t *)calloc(iterations, sizeof(latencies[0]));
    bench_expect(latencies != 0, "release pressure mprotect latency samples");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 13u) % count;
        uint32_t prot = (i & 1u) != 0
                            ? UHM_PROT_READ
                            : (UHM_PROT_READ | UHM_PROT_WRITE);
        uint64_t op_begin = bench_now_ns();
        bench_expect_ok(env->api->mprotect(env->heap, maps[slot],
                                           page_size, prot),
                        "release pressure matrix mprotect-only");
        latencies[i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    {
        bench_result result =
            bench_result_make("release pressure phase mprotect-only", begin,
                              end, iterations);
        bench_result_set_latency(&result, latencies, iterations);
        free(latencies);
        return result;
    }
}

static void bench_release_pressure_matrix_run(size_t arena_size,
                                              size_t iterations,
                                              size_t page_size,
                                              int diagnostics) {
    bench_heap_env env;
    bench_diag_snapshot diag_before;
    bench_result result;
    size_t count = bench_release_pressure_limited_count(iterations);
    void **ptrs = 0;
    void **maps = 0;
    size_t *map_sizes = 0;

    ptrs = (void **)calloc(count, sizeof(ptrs[0]));
    maps = (void **)calloc(count, sizeof(maps[0]));
    map_sizes = (size_t *)calloc(count, sizeof(map_sizes[0]));
    bench_expect(ptrs != 0 && maps != 0 && map_sizes != 0,
                 "release pressure matrix arrays");

    bench_heap_open(&env, arena_size);
    bench_diag_capture(&env, &diag_before);
    result = bench_release_pressure_malloc_only(&env, ptrs, count);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after release pressure malloc-only");
    bench_release_pressure_cleanup_mallocs(&env, ptrs, count);
    bench_heap_close(&env);

    bench_heap_open(&env, arena_size);
    bench_release_pressure_setup_mallocs(&env, ptrs, count);
    bench_diag_capture(&env, &diag_before);
    result = bench_release_pressure_free_only(&env, ptrs, count);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after release pressure free-only");
    bench_heap_close(&env);

    bench_heap_open(&env, arena_size);
    bench_diag_capture(&env, &diag_before);
    result = bench_release_pressure_mmap_only(&env, maps, map_sizes, count,
                                              page_size);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after release pressure mmap-only");
    bench_release_pressure_cleanup_maps(&env, maps, map_sizes, count);
    bench_heap_close(&env);

    bench_heap_open(&env, arena_size);
    bench_release_pressure_setup_maps(&env, maps, map_sizes, count, page_size);
    bench_diag_capture(&env, &diag_before);
    result = bench_release_pressure_munmap_only(&env, maps, map_sizes, count);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after release pressure munmap-only");
    bench_heap_close(&env);

    bench_heap_open(&env, arena_size);
    bench_release_pressure_setup_maps(&env, maps, map_sizes, 96u, page_size);
    bench_diag_capture(&env, &diag_before);
    result = bench_release_pressure_mprotect_only(
        &env, maps, 96u, iterations, page_size);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after release pressure mprotect-only");
    bench_release_pressure_cleanup_maps(&env, maps, map_sizes, 96u);
    bench_heap_close(&env);

    bench_heap_open(&env, arena_size);
    bench_diag_capture(&env, &diag_before);
    result = bench_release_pressure_with_latency(
        &env, iterations, page_size, "release pressure phase full-mixed");
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after release pressure full-mixed");
    bench_heap_close(&env);

    free(map_sizes);
    free(maps);
    free(ptrs);
}

static bench_result bench_mmap_churn_named(bench_heap_env *env,
                                           size_t iterations,
                                           size_t page_size,
                                           const char *result_name) {
    enum { slot_count = 96 };
    void *maps[slot_count];
    size_t map_sizes[slot_count];
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies =
        (uint64_t *)calloc(iterations + slot_count, sizeof(latencies[0]));
    bench_expect(latencies != 0, "release breakdown mmap latency samples");
    memset(maps, 0, sizeof(maps));
    memset(map_sizes, 0, sizeof(map_sizes));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 13u) % slot_count;
        uint64_t op_begin = bench_now_ns();
        if (maps[slot]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[slot],
                                             map_sizes[slot]),
                            "release breakdown mmap churn munmap");
        }
        map_sizes[slot] = page_size * (1u + (i % 4u));
        maps[slot] = 0;
        bench_expect_ok(env->api->mmap(env->heap, &maps[slot],
                                       map_sizes[slot],
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "release breakdown mmap churn mmap");
        latencies[i] = bench_now_ns() - op_begin;
        memset(maps[slot], (int)i, page_size);
    }
    for (i = 0; i < slot_count; ++i) {
        uint64_t op_begin = bench_now_ns();
        if (maps[i]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[i], map_sizes[i]),
                            "release breakdown mmap churn cleanup");
        }
        latencies[iterations + i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    result = bench_result_make(result_name, begin, end,
                               iterations + slot_count);
    bench_result_set_latency(&result, latencies, iterations + slot_count);
    free(latencies);
    return result;
}

static void bench_mmap_hint_reset(bench_heap_env *env) {
    memset(env->heap->core.mmap_recent_free_cache, 0,
           sizeof(env->heap->core.mmap_recent_free_cache));
    memset(env->heap->core.mmap_size_hint, 0,
           sizeof(env->heap->core.mmap_size_hint));
    memset(env->heap->core.mmap_size_hint_bucket_max, 0,
           sizeof(env->heap->core.mmap_size_hint_bucket_max));
    memset(env->heap->core.mmap_size_hint_bucket_max_extent, 0,
           sizeof(env->heap->core.mmap_size_hint_bucket_max_extent));
    env->heap->core.mmap_recent_free_extent = 0;
    env->heap->core.mmap_recent_free_next = 0;
}

static uint64_t bench_mmap_size_hint_hits(const bench_heap_env *env) {
#if UHMAS_ENABLE_DIAGNOSTICS
    return env && env->heap ? env->heap->core.mmap_size_hint_hits
                            : UINT64_C(0);
#else
    (void)env;
    return UINT64_C(0);
#endif
}

static uint64_t
bench_mmap_size_hint_direct_exact_hits(const bench_heap_env *env) {
#if UHMAS_ENABLE_TEST_HOOKS
    return env && env->heap ? env->heap->core.mmap_size_hint_direct_exact_hits
                            : UINT64_C(0);
#else
    (void)env;
    return UINT64_C(0);
#endif
}

static uint64_t bench_mmap_free_tree_find_calls(const bench_heap_env *env) {
#if UHMAS_ENABLE_DIAGNOSTICS
    return env && env->heap ? env->heap->core.mmap_free_tree_find_calls
                            : UINT64_C(0);
#else
    (void)env;
    return UINT64_C(0);
#endif
}

typedef struct bench_mmap_locality_fragmentation {
    uint64_t samples;
    uint64_t range_free_extents_sum;
    uint64_t range_free_pages_sum;
    uint64_t range_largest_pages_max;
    uint64_t range_fragmented_pages_sum;
    uint64_t range_fragmentation_ppm_max;
    uint64_t exact_reuse_hits;
    uint64_t reuse_distance_pages_sum;
    uint64_t reuse_distance_pages_max;
} bench_mmap_locality_fragmentation;

static uint64_t bench_mmap_locality_page_distance(uintptr_t a, uintptr_t b,
                                                  size_t page_size) {
    uintptr_t diff = a > b ? a - b : b - a;
    return page_size != 0 ? (uint64_t)(diff / page_size) : 0u;
}

static int bench_mmap_locality_range_overflows(uintptr_t start, size_t size,
                                               uintptr_t *end_out) {
    if (!end_out || size > (size_t)(UINTPTR_MAX - start)) {
        return 1;
    }
    *end_out = start + (uintptr_t)size;
    return 0;
}

static void bench_mmap_locality_observe_range(
    bench_heap_env *env,
    uintptr_t range_start,
    size_t range_size,
    size_t page_size,
    bench_mmap_locality_fragmentation *meta) {
    uintptr_t range_end;
    uhm_free_extent *extent;
    uint64_t extents = 0;
    uint64_t total_pages = 0;
    uint64_t largest_pages = 0;
    uint64_t fragmented_pages;
    uint64_t fragmentation_ppm;

    bench_expect(env != 0 && env->heap != 0 && meta != 0,
                 "mmap locality metadata env");
    bench_expect(page_size != 0, "mmap locality metadata page size");
    bench_expect(!bench_mmap_locality_range_overflows(range_start, range_size,
                                                      &range_end),
                 "mmap locality metadata range");

    for (extent = env->heap->core.mmap_free_head; extent;
         extent = extent->next) {
        uintptr_t extent_end;
        uintptr_t overlap_start;
        uintptr_t overlap_end;
        uint64_t pages;

        bench_expect(!bench_mmap_locality_range_overflows(
                         extent->addr, extent->size, &extent_end),
                     "mmap locality metadata extent range");
        overlap_start = extent->addr > range_start ? extent->addr : range_start;
        overlap_end = extent_end < range_end ? extent_end : range_end;
        if (overlap_start >= overlap_end) {
            continue;
        }
        pages = (uint64_t)((overlap_end - overlap_start) / page_size);
        if (pages == 0) {
            continue;
        }
        ++extents;
        total_pages += pages;
        if (pages > largest_pages) {
            largest_pages = pages;
        }
    }

    fragmented_pages =
        total_pages > largest_pages ? total_pages - largest_pages : 0u;
    fragmentation_ppm = total_pages != 0
                            ? (fragmented_pages * 1000000ull) / total_pages
                            : 0u;

    ++meta->samples;
    meta->range_free_extents_sum += extents;
    meta->range_free_pages_sum += total_pages;
    if (largest_pages > meta->range_largest_pages_max) {
        meta->range_largest_pages_max = largest_pages;
    }
    meta->range_fragmented_pages_sum += fragmented_pages;
    if (fragmentation_ppm > meta->range_fragmentation_ppm_max) {
        meta->range_fragmentation_ppm_max = fragmentation_ppm;
    }
}

static void bench_mmap_locality_observe_reuse(
    bench_mmap_locality_fragmentation *meta,
    uintptr_t expected,
    uintptr_t actual,
    size_t page_size) {
    uint64_t distance_pages;

    bench_expect(meta != 0, "mmap locality reuse metadata");
    distance_pages =
        bench_mmap_locality_page_distance(expected, actual, page_size);
    meta->reuse_distance_pages_sum += distance_pages;
    if (distance_pages > meta->reuse_distance_pages_max) {
        meta->reuse_distance_pages_max = distance_pages;
    }
    if (distance_pages == 0) {
        ++meta->exact_reuse_hits;
    }
}

static void bench_mmap_locality_observe_reuse_set(
    bench_mmap_locality_fragmentation *meta,
    const uintptr_t *expected,
    unsigned char *used,
    size_t count,
    uintptr_t actual,
    size_t page_size) {
    size_t slot;

    bench_expect(expected != 0 && used != 0, "mmap locality reuse set args");
    for (slot = 0; slot < count; ++slot) {
        if (!used[slot] && actual == expected[slot]) {
            used[slot] = 1u;
            bench_mmap_locality_observe_reuse(meta, expected[slot], actual,
                                              page_size);
            return;
        }
    }
    bench_expect(0, "mmap locality reuse from expected set");
}

static void bench_mmap_locality_print_metadata(
    const char *result_name,
    size_t exact_pages,
    const bench_mmap_locality_fragmentation *meta) {
    bench_expect(result_name != 0 && meta != 0,
                 "mmap locality metadata print args");
    printf("metadata %s: "
           "mmap_locality_samples=%llu "
           "mmap_locality_exact_pages=%llu "
           "mmap_locality_recent_cache_slots=%llu "
           "mmap_locality_size_hint_slots=%llu "
           "mmap_locality_range_free_extents_sum=%llu "
           "mmap_locality_range_free_pages_sum=%llu "
           "mmap_locality_range_largest_pages_max=%llu "
           "mmap_locality_range_fragmented_pages_sum=%llu "
           "mmap_locality_range_fragmentation_ppm_max=%llu "
           "mmap_locality_exact_reuse_hits=%llu "
           "mmap_locality_reuse_distance_pages_sum=%llu "
           "mmap_locality_reuse_distance_pages_max=%llu\n",
           result_name,
           (unsigned long long)meta->samples,
           (unsigned long long)exact_pages,
           (unsigned long long)UHM_MMAP_RECENT_FREE_CACHE_SLOTS,
           (unsigned long long)UHM_MMAP_SIZE_HINT_SLOTS,
           (unsigned long long)meta->range_free_extents_sum,
           (unsigned long long)meta->range_free_pages_sum,
           (unsigned long long)meta->range_largest_pages_max,
           (unsigned long long)meta->range_fragmented_pages_sum,
           (unsigned long long)meta->range_fragmentation_ppm_max,
           (unsigned long long)meta->exact_reuse_hits,
           (unsigned long long)meta->reuse_distance_pages_sum,
           (unsigned long long)meta->reuse_distance_pages_max);
}

static bench_result bench_mmap_locality_size_hint(bench_heap_env *env,
                                                  size_t iterations,
                                                  size_t page_size,
                                                  size_t exact_pages,
                                                  const char *result_name) {
    const size_t filler_count = UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
    bench_mmap_locality_fragmentation meta = {0};
    size_t i;
    size_t operations = 0;
    uint64_t hint_hits_before;
    uint64_t begin;
    uint64_t end;
    size_t total_pages;

    bench_expect(exact_pages != 0, "mmap locality exact pages");
    total_pages = exact_pages + 2u + (filler_count * 2u);

    hint_hits_before = bench_mmap_size_hint_hits(env);
#if !UHMAS_ENABLE_DIAGNOSTICS
    (void)hint_hits_before;
#endif
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *reuse = 0;
        unsigned char *base;
        uint64_t tree_find_before_reuse;
        size_t filler;

        bench_expect_ok(env->api->mmap(env->heap, &map,
                                       total_pages * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality map");
        bench_expect(map != 0, "mmap locality map address");
        base = (unsigned char *)map;
        memset(base, (int)(i & 0xffu), page_size);
        ++operations;

        bench_mmap_hint_reset(env);
        bench_expect_ok(env->api->munmap(env->heap, base + page_size,
                                         exact_pages * page_size),
                        "mmap locality exact hole");
        ++operations;
        for (filler = 0; filler < filler_count; ++filler) {
            size_t page_index = exact_pages + 2u + (filler * 2u);
            bench_expect_ok(env->api->munmap(
                                env->heap, base + (page_index * page_size),
                                page_size),
                            "mmap locality filler");
            ++operations;
        }

        bench_mmap_locality_observe_range(
            env, (uintptr_t)base, total_pages * page_size, page_size, &meta);
        tree_find_before_reuse = bench_mmap_free_tree_find_calls(env);
        bench_expect_ok(env->api->mmap(env->heap, &reuse,
                                       exact_pages * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality size hint reuse");
        bench_expect(reuse == base + page_size,
                     "mmap locality size hint exact reuse");
        bench_mmap_locality_observe_reuse(
            &meta, (uintptr_t)(base + page_size), (uintptr_t)reuse,
            page_size);
        bench_expect(bench_mmap_free_tree_find_calls(env) ==
                         tree_find_before_reuse,
                     "mmap locality size hint skips highest-fit tree");
        memset(reuse, (int)((i + 19u) & 0xffu), page_size);
        ++operations;

        bench_expect_ok(env->api->munmap(env->heap, base,
                                         (exact_pages + 2u) * page_size),
                        "mmap locality cleanup first");
        ++operations;
        for (filler = 0; filler < filler_count; ++filler) {
            size_t page_index = exact_pages + 3u + (filler * 2u);
            bench_expect_ok(env->api->munmap(
                                env->heap, base + (page_index * page_size),
                                page_size),
                            "mmap locality cleanup filler guard");
            ++operations;
        }
    }
    end = bench_now_ns();
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(bench_mmap_size_hint_hits(env) >=
                     hint_hits_before + iterations,
                 "mmap locality size hint hits counted");
#endif
    bench_mmap_locality_print_metadata(result_name, exact_pages, &meta);
    return bench_result_make(result_name, begin, end, operations);
}

static bench_result bench_mmap_locality_exact_front_hot_cache(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    const size_t exact_pages = 2u;
    const size_t filler_count = UHM_MMAP_RECENT_FREE_CACHE_SLOTS - 1u;
    const size_t filler_start_page = 5u;
    const size_t total_pages = filler_start_page + filler_count * 2u;
    const size_t repetitions = iterations * 256u;
    const char *result_name =
        "mmap locality exact-front hot-cache metadata";
    bench_mmap_locality_fragmentation meta = {0};
    bench_logical_vm_state logical_vm;
    size_t operations = repetitions * 2u;
    size_t i;
    size_t filler;
    void *map = 0;
    unsigned char *base;
    uhm_free_extent *target_extent;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t too_small_before;
#endif
    uint64_t begin;
    uint64_t end;

    bench_logical_vm_install(env, &logical_vm);
    bench_expect_ok(env->api->mmap(env->heap, &map,
                    total_pages * page_size,
                    UHM_PROT_READ | UHM_PROT_WRITE,
                    UHM_MAP_PRIVATE),
                    "mmap locality exact-front map");
    bench_expect(map != 0, "mmap locality exact-front map address");
    base = (unsigned char *)map;
    bench_mmap_hint_reset(env);
    for (filler = 0; filler < filler_count; ++filler) {
        size_t page_index = filler_start_page + filler * 2u;
        bench_expect_ok(env->api->munmap(
                            env->heap, base + page_index * page_size,
                            page_size),
                        "mmap locality exact-front filler hole");
    }
    bench_expect_ok(env->api->munmap(env->heap, base + page_size,
                                     exact_pages * page_size),
                    "mmap locality exact-front target hole");
    target_extent = env->heap->core.mmap_recent_free_cache[0];
    bench_expect(target_extent != 0 &&
                     target_extent->addr == (uintptr_t)(base + page_size) &&
                     target_extent->size == exact_pages * page_size,
                 "mmap locality exact-front target cache position");
    bench_expect(env->heap->core.mmap_recent_free_cache[filler_count] != 0,
                 "mmap locality exact-front recent cache is full");
    bench_mmap_locality_observe_range(
        env, (uintptr_t)base, total_pages * page_size, page_size, &meta);
#if UHMAS_ENABLE_DIAGNOSTICS
    too_small_before = env->heap->core.mmap_recent_free_too_small;
#endif

    begin = bench_now_ns();
    for (i = 0; i < repetitions; ++i) {
        void *reuse = 0;

        bench_expect_ok(env->api->mmap(env->heap, &reuse,
                                       exact_pages * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality exact-front reuse");
        bench_expect(reuse == base + page_size,
                     "mmap locality exact-front expected reuse");
        bench_expect_ok(env->api->munmap(env->heap, reuse,
                                         exact_pages * page_size),
                        "mmap locality exact-front restore hole");
        target_extent = env->heap->core.mmap_recent_free_cache[0];
        bench_expect(target_extent != 0 &&
                         target_extent->addr ==
                             (uintptr_t)(base + page_size) &&
                         target_extent->size == exact_pages * page_size,
                     "mmap locality exact-front target stays newest");
    }
    end = bench_now_ns();
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(env->heap->core.mmap_recent_free_too_small ==
                     too_small_before,
                 "mmap locality exact-front skips older too-small entries");
#endif
    meta.samples = repetitions;
    meta.exact_reuse_hits = repetitions;
    bench_expect_ok(env->api->munmap(env->heap, base, page_size),
                    "mmap locality exact-front first guard cleanup");
    bench_expect_ok(env->api->munmap(env->heap, base + 3u * page_size,
                                     2u * page_size),
                    "mmap locality exact-front target guard cleanup");
    for (filler = 0; filler < filler_count; ++filler) {
        size_t page_index = filler_start_page + filler * 2u + 1u;
        bench_expect_ok(env->api->munmap(
                            env->heap, base + page_index * page_size,
                            page_size),
                        "mmap locality exact-front filler guard cleanup");
    }
    bench_logical_vm_restore(env, &logical_vm);
    bench_mmap_locality_print_metadata(result_name, exact_pages, &meta);
    return bench_result_make(result_name, begin, end, operations);
}

static bench_result bench_mmap_locality_sparse_cache_maintenance(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    const size_t exact_pages = 1u;
    const size_t total_pages = 3u;
    const size_t repetitions = iterations * 256u;
    const char *result_name =
        "mmap locality sparse hot-cache maintenance";
    bench_mmap_locality_fragmentation meta = {0};
    bench_logical_vm_state logical_vm;
    size_t operations = repetitions * 2u;
    size_t i;
    void *map = 0;
    unsigned char *base;
    uint64_t begin;
    uint64_t end;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t recent_attempts_before;
    uint64_t recent_hits_before;
    uint64_t recent_too_small_before;
    uint64_t tree_find_before;
#endif

    bench_logical_vm_install(env, &logical_vm);
    bench_expect_ok(env->api->mmap(env->heap, &map,
                                   total_pages * page_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "mmap locality sparse maintenance map");
    bench_expect(map != 0, "mmap locality sparse maintenance map address");
    base = (unsigned char *)map;
    bench_mmap_hint_reset(env);
    bench_expect_ok(env->api->munmap(env->heap, base + page_size,
                                     exact_pages * page_size),
                    "mmap locality sparse maintenance target hole");
    bench_expect(env->heap->core.mmap_recent_free_next == 1u &&
                     env->heap->core.mmap_recent_free_cache[0] != 0 &&
                     env->heap->core.mmap_recent_free_cache[1] == 0,
                 "mmap locality sparse maintenance one-slot cache");
    bench_mmap_locality_observe_range(
        env, (uintptr_t)base, total_pages * page_size, page_size, &meta);
#if UHMAS_ENABLE_DIAGNOSTICS
    recent_attempts_before = env->heap->core.mmap_recent_free_attempts;
    recent_hits_before = env->heap->core.mmap_recent_free_hits;
    recent_too_small_before = env->heap->core.mmap_recent_free_too_small;
    tree_find_before = env->heap->core.mmap_free_tree_find_calls;
#endif

    begin = bench_now_ns();
    for (i = 0; i < repetitions; ++i) {
        void *reuse = 0;

        bench_expect_ok(env->api->mmap(env->heap, &reuse,
                                       exact_pages * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality sparse maintenance reuse");
        bench_expect(reuse == base + page_size,
                     "mmap locality sparse maintenance expected reuse");
        bench_expect_ok(env->api->munmap(env->heap, reuse,
                                         exact_pages * page_size),
                        "mmap locality sparse maintenance restore hole");
    }
    end = bench_now_ns();
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(env->heap->core.mmap_recent_free_attempts >=
                     recent_attempts_before + repetitions,
                 "mmap locality sparse maintenance attempts counted");
    bench_expect(env->heap->core.mmap_recent_free_hits >=
                     recent_hits_before + repetitions,
                 "mmap locality sparse maintenance hits counted");
    bench_expect(env->heap->core.mmap_recent_free_too_small ==
                     recent_too_small_before,
                 "mmap locality sparse maintenance no too-small scans");
    bench_expect(env->heap->core.mmap_free_tree_find_calls == tree_find_before,
                 "mmap locality sparse maintenance skips tree find");
#endif
    meta.samples = repetitions;
    meta.exact_reuse_hits = repetitions;
    bench_expect_ok(env->api->munmap(env->heap, base, page_size),
                    "mmap locality sparse maintenance left cleanup");
    bench_expect_ok(env->api->munmap(env->heap, base + 2u * page_size,
                                     page_size),
                    "mmap locality sparse maintenance right cleanup");
    bench_logical_vm_restore(env, &logical_vm);
    bench_mmap_locality_print_metadata(result_name, exact_pages, &meta);
    return bench_result_make(result_name, begin, end, operations);
}

static bench_result bench_mmap_locality_adjacent_release_lookup(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    const size_t target_pages = 8u;
    const size_t target_start_page = 1u;
    const size_t filler_count =
        UHM_MMAP_FREE_ADJACENT_LIST_SCAN_LIMIT + 8u;
    const size_t filler_start_page = target_start_page + target_pages + 2u;
    const size_t total_pages = filler_start_page + filler_count * 2u;
    const size_t setup_chunk_count = 3u;
    const size_t repetitions = iterations * 128u;
    const char *result_name =
        "mmap locality adjacent-release recent lookup";
    bench_mmap_locality_fragmentation meta = {0};
    bench_logical_vm_state logical_vm;
    size_t operations = repetitions * (target_pages + 1u);
    size_t i;
    size_t page;
    size_t filler;
    void *setup_maps[3] = {0};
    size_t setup_pages[3] = {0};
    uintptr_t setup_low = UINTPTR_MAX;
    uintptr_t setup_high = 0;
    size_t setup_chunk;
    unsigned char *base;
    uint64_t begin;
    uint64_t end;

    bench_logical_vm_install(env, &logical_vm);
    for (setup_chunk = 0; setup_chunk < setup_chunk_count; ++setup_chunk) {
        uintptr_t chunk_start;
        uintptr_t chunk_end;
        size_t chunk_bytes;
        uint32_t map_flags = UHM_MAP_PRIVATE;

        setup_pages[setup_chunk] =
            total_pages / setup_chunk_count +
            (setup_chunk < total_pages % setup_chunk_count ? 1u : 0u);
        bench_expect(setup_pages[setup_chunk] <=
                         UHM_MMAP_FREE_ADJACENT_LIST_SCAN_LIMIT,
                     "mmap locality adjacent release setup chunk budget");
        chunk_bytes = setup_pages[setup_chunk] * page_size;
        if (setup_chunk != 0u) {
            bench_expect(setup_low >= chunk_bytes,
                         "mmap locality adjacent release setup underflow");
            setup_maps[setup_chunk] =
                (void *)(setup_low - (uintptr_t)chunk_bytes);
            map_flags |= UHM_MAP_FIXED_NOREPLACE;
        }
        bench_expect_ok(env->api->mmap(
                            env->heap, &setup_maps[setup_chunk],
                            chunk_bytes,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            map_flags),
                        "mmap locality adjacent release setup map");
        bench_expect(setup_maps[setup_chunk] != 0,
                     "mmap locality adjacent release setup address");
        chunk_start = (uintptr_t)setup_maps[setup_chunk];
        bench_expect(!bench_mmap_locality_range_overflows(
                         chunk_start, chunk_bytes, &chunk_end),
                     "mmap locality adjacent release setup range");
        if (chunk_start < setup_low) {
            setup_low = chunk_start;
        }
        if (chunk_end > setup_high) {
            setup_high = chunk_end;
        }
    }
    bench_expect(setup_low != UINTPTR_MAX &&
                     setup_high - setup_low == total_pages * page_size,
                 "mmap locality adjacent release contiguous setup");
    base = (unsigned char *)setup_low;
    for (filler = 0; filler < filler_count; ++filler) {
        size_t page_index = filler_start_page + filler * 2u;
        bench_expect_ok(env->api->munmap(
                            env->heap, base + page_index * page_size,
                            page_size),
                        "mmap locality adjacent release filler hole");
    }
    bench_expect(env->heap->core.mmap_free_tree_current_nodes >
                     UHM_MMAP_FREE_ADJACENT_LIST_SCAN_LIMIT,
                 "mmap locality adjacent release tree regime");
    bench_mmap_locality_observe_range(
        env, (uintptr_t)base, total_pages * page_size, page_size, &meta);

    begin = bench_now_ns();
    for (i = 0; i < repetitions; ++i) {
        void *reuse = 0;
        uhm_free_extent *target_extent;

        for (page = 0; page < target_pages; ++page) {
            bench_expect_ok(env->api->munmap(
                                env->heap,
                                base + (target_start_page + page) *
                                           page_size,
                                page_size),
                            "mmap locality adjacent release target page");
        }
        target_extent = env->heap->core.mmap_recent_free_cache[0];
        bench_expect(target_extent != 0 &&
                         target_extent->addr ==
                             (uintptr_t)(base +
                                         target_start_page * page_size) &&
                         target_extent->size == target_pages * page_size,
                     "mmap locality adjacent release merged target");
        bench_expect_ok(env->api->mmap(env->heap, &reuse,
                                       target_pages * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality adjacent release exact reuse");
        bench_expect(reuse == base + target_start_page * page_size,
                     "mmap locality adjacent release exact address");
    }
    end = bench_now_ns();

    meta.samples = repetitions;
    meta.exact_reuse_hits = repetitions;
    bench_expect_ok(env->api->munmap(
                        env->heap, base, filler_start_page * page_size),
                    "mmap locality adjacent release target cleanup");
    for (filler = 0; filler < filler_count; ++filler) {
        size_t guard_page = filler_start_page + filler * 2u + 1u;
        bench_expect_ok(env->api->munmap(
                            env->heap, base + guard_page * page_size,
                            page_size),
                        "mmap locality adjacent release guard cleanup");
    }
    bench_logical_vm_restore(env, &logical_vm);
    bench_mmap_locality_print_metadata(result_name, target_pages, &meta);
    return bench_result_make(result_name, begin, end, operations);
}

static bench_result
bench_mmap_locality_same_bucket_hint(bench_heap_env *env,
                                     size_t iterations,
                                     size_t page_size) {
    const size_t filler_count = UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
    const size_t total_pages = 10u + filler_count * 2u + 2u;
    bench_mmap_locality_fragmentation meta = {0};
    size_t i;
    size_t operations = 0;
    uint64_t hint_hits_before;
    uint64_t direct_exact_hits_before;
    uint64_t begin;
    uint64_t end;

    hint_hits_before = bench_mmap_size_hint_hits(env);
    direct_exact_hits_before = bench_mmap_size_hint_direct_exact_hits(env);
#if !UHMAS_ENABLE_DIAGNOSTICS || !UHMAS_ENABLE_TEST_HOOKS
    (void)hint_hits_before;
    (void)direct_exact_hits_before;
#endif
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *reuse = 0;
        unsigned char *base;
        uint64_t tree_find_before_reuse;
        size_t filler;

        bench_expect_ok(env->api->mmap(env->heap, &map,
                                       total_pages * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality same bucket map");
        bench_expect(map != 0, "mmap locality same bucket map address");
        base = (unsigned char *)map;
        memset(base, (int)((i + 31u) & 0xffu), page_size);
        ++operations;

        bench_mmap_hint_reset(env);
        bench_expect_ok(env->api->munmap(env->heap, base + page_size,
                                         4u * page_size),
                        "mmap locality same bucket exact hole");
        ++operations;
        bench_expect_ok(env->api->munmap(env->heap, base + 6u * page_size,
                                         3u * page_size),
                        "mmap locality same bucket too-small hole");
        ++operations;
        for (filler = 0; filler < filler_count; ++filler) {
            size_t page_index = 10u + (filler * 2u);
            bench_expect_ok(env->api->munmap(
                                env->heap, base + (page_index * page_size),
                                page_size),
                            "mmap locality same bucket filler");
            ++operations;
        }

        bench_mmap_locality_observe_range(
            env, (uintptr_t)base, total_pages * page_size, page_size, &meta);
        tree_find_before_reuse = bench_mmap_free_tree_find_calls(env);
        bench_expect_ok(env->api->mmap(env->heap, &reuse, 4u * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality same bucket reuse");
        bench_expect(reuse == base + page_size,
                     "mmap locality same bucket exact reuse");
        bench_mmap_locality_observe_reuse(
            &meta, (uintptr_t)(base + page_size), (uintptr_t)reuse,
            page_size);
        bench_expect(bench_mmap_free_tree_find_calls(env) ==
                         tree_find_before_reuse,
                     "mmap locality same bucket skips highest-fit tree");
        memset(reuse, (int)((i + 47u) & 0xffu), page_size);
        ++operations;

        bench_expect_ok(env->api->munmap(env->heap, base, 6u * page_size),
                        "mmap locality same bucket cleanup first");
        ++operations;
        bench_expect_ok(env->api->munmap(env->heap, base + 9u * page_size,
                                         page_size),
                        "mmap locality same bucket cleanup guard");
        ++operations;
        for (filler = 0; filler < filler_count - 1u; ++filler) {
            size_t page_index = 11u + (filler * 2u);
            bench_expect_ok(env->api->munmap(
                                env->heap, base + (page_index * page_size),
                                page_size),
                            "mmap locality same bucket cleanup filler guard");
            ++operations;
        }
        bench_expect_ok(env->api->munmap(
                            env->heap,
                            base + (9u + filler_count * 2u) * page_size,
                            3u * page_size),
                        "mmap locality same bucket cleanup tail");
        ++operations;
    }
    end = bench_now_ns();
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(bench_mmap_size_hint_hits(env) >=
                     hint_hits_before + iterations,
                 "mmap locality same bucket size hint hits counted");
#if UHMAS_ENABLE_TEST_HOOKS
    bench_expect(bench_mmap_size_hint_direct_exact_hits(env) >=
                     direct_exact_hits_before + iterations,
                 "mmap locality same bucket direct exact candidates counted");
#endif
#endif
    bench_mmap_locality_print_metadata(
        "mmap locality same-bucket hint churn", 4u, &meta);
    return bench_result_make("mmap locality same-bucket hint churn", begin,
                             end, operations);
}

static bench_result
bench_mmap_locality_deep_bucket_hint(bench_heap_env *env,
                                     size_t iterations,
                                     size_t page_size) {
    const size_t too_small_count = UHM_MMAP_SIZE_HINT_SLOTS - 1u;
    const size_t extra_recent_count =
        UHM_MMAP_RECENT_FREE_CACHE_SLOTS > too_small_count
            ? UHM_MMAP_RECENT_FREE_CACHE_SLOTS - too_small_count
            : 0u;
    const size_t total_pages =
        6u + too_small_count * 4u + extra_recent_count * 2u + 1u;
    bench_mmap_locality_fragmentation meta = {0};
    size_t i;
    size_t operations = 0;
    uint64_t hint_hits_before;
    uint64_t direct_exact_hits_before;
    uint64_t begin;
    uint64_t end;

    bench_expect(too_small_count != 0, "mmap locality deep bucket slots");
    hint_hits_before = bench_mmap_size_hint_hits(env);
    direct_exact_hits_before = bench_mmap_size_hint_direct_exact_hits(env);
#if !UHMAS_ENABLE_DIAGNOSTICS || !UHMAS_ENABLE_TEST_HOOKS
    (void)hint_hits_before;
    (void)direct_exact_hits_before;
#endif
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *reuse = 0;
        unsigned char *base;
        uint64_t tree_find_before_reuse;
        size_t slot;

        bench_expect_ok(env->api->mmap(env->heap, &map,
                                       total_pages * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality deep bucket map");
        bench_expect(map != 0, "mmap locality deep bucket map address");
        base = (unsigned char *)map;
        memset(base, (int)((i + 59u) & 0xffu), page_size);
        ++operations;

        bench_mmap_hint_reset(env);
        bench_expect_ok(env->api->munmap(env->heap, base + page_size,
                                         4u * page_size),
                        "mmap locality deep bucket exact hole");
        ++operations;
        for (slot = 0; slot < too_small_count; ++slot) {
            size_t page_index = 6u + slot * 4u;
            bench_expect_ok(env->api->munmap(
                                env->heap, base + page_index * page_size,
                                3u * page_size),
                            "mmap locality deep bucket too-small hole");
            ++operations;
        }
        for (slot = 0; slot < extra_recent_count; ++slot) {
            size_t page_index = 6u + too_small_count * 4u + slot * 2u;
            bench_expect_ok(env->api->munmap(
                                env->heap, base + page_index * page_size,
                                page_size),
                            "mmap locality deep bucket recent filler");
            ++operations;
        }

        bench_mmap_locality_observe_range(
            env, (uintptr_t)base, total_pages * page_size, page_size, &meta);
        tree_find_before_reuse = bench_mmap_free_tree_find_calls(env);
        bench_expect_ok(env->api->mmap(env->heap, &reuse, 4u * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality deep bucket reuse");
        bench_expect(reuse == base + page_size,
                     "mmap locality deep bucket exact reuse");
        bench_mmap_locality_observe_reuse(
            &meta, (uintptr_t)(base + page_size), (uintptr_t)reuse,
            page_size);
        bench_expect(bench_mmap_free_tree_find_calls(env) ==
                         tree_find_before_reuse,
                     "mmap locality deep bucket skips highest-fit tree");
        memset(reuse, (int)((i + 83u) & 0xffu), page_size);
        ++operations;

        bench_expect_ok(env->api->munmap(env->heap, base, page_size),
                        "mmap locality deep bucket cleanup first");
        ++operations;
        bench_expect_ok(env->api->munmap(env->heap, reuse,
                                         4u * page_size),
                        "mmap locality deep bucket cleanup exact reuse");
        ++operations;
        bench_expect_ok(env->api->munmap(env->heap, base + 5u * page_size,
                                         page_size),
                        "mmap locality deep bucket cleanup first guard");
        ++operations;
        for (slot = 0; slot < too_small_count; ++slot) {
            size_t guard_page = 9u + slot * 4u;
            bench_expect_ok(env->api->munmap(
                                env->heap, base + guard_page * page_size,
                                page_size),
                            "mmap locality deep bucket cleanup guard");
            ++operations;
        }
        for (slot = 0; slot < extra_recent_count; ++slot) {
            size_t guard_page = 7u + too_small_count * 4u + slot * 2u;
            bench_expect_ok(env->api->munmap(
                                env->heap, base + guard_page * page_size,
                                page_size),
                            "mmap locality deep bucket cleanup recent guard");
            ++operations;
        }
        bench_expect_ok(env->api->munmap(
                            env->heap,
                            base + (total_pages - 1u) * page_size, page_size),
                        "mmap locality deep bucket cleanup tail");
        ++operations;
    }
    end = bench_now_ns();
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(bench_mmap_size_hint_hits(env) >=
                     hint_hits_before + iterations,
                 "mmap locality deep bucket size hint hits counted");
#if UHMAS_ENABLE_TEST_HOOKS
    bench_expect(bench_mmap_size_hint_direct_exact_hits(env) >=
                     direct_exact_hits_before + iterations,
                 "mmap locality deep bucket direct exact candidates counted");
#endif
#endif
    bench_mmap_locality_print_metadata(
        "mmap locality deep-bucket hint churn", 4u, &meta);
    return bench_result_make("mmap locality deep-bucket hint churn", begin,
                             end, operations);
}

static bench_result
bench_mmap_locality_fragmented_ring_hint(bench_heap_env *env,
                                         size_t iterations,
                                         size_t page_size) {
    const size_t exact_pages = 4u;
    const size_t exact_count =
        UHM_MMAP_SIZE_HINT_SLOTS < 4u ? UHM_MMAP_SIZE_HINT_SLOTS : 4u;
    const size_t filler_count = UHM_MMAP_RECENT_FREE_CACHE_SLOTS;
    const size_t exact_start = 1u;
    const size_t exact_stride = exact_pages + 1u;
    const size_t filler_start = exact_start + exact_count * exact_stride;
    const size_t total_pages = filler_start + filler_count * 2u + 1u;
    bench_mmap_locality_fragmentation meta = {0};
    size_t i;
    size_t operations = 0;
    uint64_t hint_hits_before;
    uint64_t begin;
    uint64_t end;

    bench_expect(exact_count != 0, "mmap locality fragmented ring slots");
    hint_hits_before = bench_mmap_size_hint_hits(env);
#if !UHMAS_ENABLE_DIAGNOSTICS
    (void)hint_hits_before;
#endif
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *reuses[UHM_MMAP_SIZE_HINT_SLOTS];
        uintptr_t exact_addrs[UHM_MMAP_SIZE_HINT_SLOTS];
        unsigned char exact_used[UHM_MMAP_SIZE_HINT_SLOTS];
        unsigned char *base;
        uint64_t tree_find_before_reuse;
        size_t slot;

        memset(reuses, 0, sizeof(reuses));
        memset(exact_used, 0, sizeof(exact_used));
        bench_expect_ok(env->api->mmap(env->heap, &map,
                                       total_pages * page_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mmap locality fragmented ring map");
        bench_expect(map != 0, "mmap locality fragmented ring map address");
        base = (unsigned char *)map;
        memset(base, (int)((i + 101u) & 0xffu), page_size);
        ++operations;

        bench_mmap_hint_reset(env);
        for (slot = 0; slot < exact_count; ++slot) {
            const size_t page_index = exact_start + slot * exact_stride;
            exact_addrs[slot] = (uintptr_t)(base + page_index * page_size);
            bench_expect_ok(env->api->munmap(
                                env->heap, (void *)exact_addrs[slot],
                                exact_pages * page_size),
                            "mmap locality fragmented ring exact hole");
            ++operations;
        }
        for (slot = 0; slot < filler_count; ++slot) {
            const size_t page_index = filler_start + slot * 2u;
            bench_expect_ok(env->api->munmap(
                                env->heap, base + page_index * page_size,
                                page_size),
                            "mmap locality fragmented ring recent filler");
            ++operations;
        }

        bench_mmap_locality_observe_range(
            env, (uintptr_t)base, total_pages * page_size, page_size, &meta);
        tree_find_before_reuse = bench_mmap_free_tree_find_calls(env);
        for (slot = 0; slot < exact_count; ++slot) {
            void *reuse = 0;
            bench_expect_ok(env->api->mmap(env->heap, &reuse,
                                           exact_pages * page_size,
                                           UHM_PROT_READ | UHM_PROT_WRITE,
                                           UHM_MAP_PRIVATE),
                            "mmap locality fragmented ring reuse");
            bench_mmap_locality_observe_reuse_set(
                &meta, exact_addrs, exact_used, exact_count,
                (uintptr_t)reuse, page_size);
            bench_expect(bench_mmap_free_tree_find_calls(env) ==
                             tree_find_before_reuse,
                         "mmap locality fragmented ring skips highest-fit "
                         "tree");
            memset(reuse, (int)((i + slot + 127u) & 0xffu), page_size);
            reuses[slot] = reuse;
            ++operations;
        }

        bench_expect_ok(env->api->munmap(env->heap, base, page_size),
                        "mmap locality fragmented ring cleanup head");
        ++operations;
        for (slot = 0; slot < exact_count; ++slot) {
            const size_t guard_page =
                exact_start + slot * exact_stride + exact_pages;
            bench_expect_ok(env->api->munmap(env->heap, reuses[slot],
                                             exact_pages * page_size),
                            "mmap locality fragmented ring cleanup reuse");
            ++operations;
            bench_expect_ok(env->api->munmap(
                                env->heap, base + guard_page * page_size,
                                page_size),
                            "mmap locality fragmented ring cleanup exact "
                            "guard");
            ++operations;
        }
        for (slot = 0; slot < filler_count; ++slot) {
            const size_t guard_page = filler_start + slot * 2u + 1u;
            bench_expect_ok(env->api->munmap(
                                env->heap, base + guard_page * page_size,
                                page_size),
                            "mmap locality fragmented ring cleanup filler "
                            "guard");
            ++operations;
        }
        bench_expect_ok(env->api->munmap(
                            env->heap,
                            base + (total_pages - 1u) * page_size, page_size),
                        "mmap locality fragmented ring cleanup tail");
        ++operations;
    }
    end = bench_now_ns();
#if UHMAS_ENABLE_DIAGNOSTICS
    bench_expect(bench_mmap_size_hint_hits(env) >=
                     hint_hits_before + iterations * exact_count,
                 "mmap locality fragmented ring size hint hits counted");
#endif
    bench_mmap_locality_print_metadata(
        "mmap locality fragmented-ring hint churn", exact_pages, &meta);
    return bench_result_make("mmap locality fragmented-ring hint churn", begin,
                             end, operations);
}

static bench_result bench_release_munmap_burst(bench_heap_env *env,
                                               size_t iterations,
                                               size_t page_size) {
    void **maps;
    size_t *map_sizes;
    uint64_t *latencies;
    size_t i;
    size_t count = iterations > 2048u ? 2048u : iterations;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    maps = (void **)calloc(count, sizeof(maps[0]));
    map_sizes = (size_t *)calloc(count, sizeof(map_sizes[0]));
    latencies = (uint64_t *)calloc(count * 2u, sizeof(latencies[0]));
    bench_expect(maps != 0 && map_sizes != 0,
                 "release breakdown munmap arrays");
    bench_expect(latencies != 0, "release breakdown munmap latency samples");
    begin = bench_now_ns();
    for (i = 0; i < count; ++i) {
        uint64_t op_begin;
        map_sizes[i] = page_size * (1u + (i % 4u));
        op_begin = bench_now_ns();
        bench_expect_ok(env->api->mmap(env->heap, &maps[i], map_sizes[i],
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "release breakdown munmap setup");
        latencies[i] = bench_now_ns() - op_begin;
    }
    for (i = 0; i < count; ++i) {
        uint64_t op_begin = bench_now_ns();
        bench_expect_ok(env->api->munmap(env->heap, maps[i], map_sizes[i]),
                        "release breakdown munmap burst");
        latencies[count + i] = bench_now_ns() - op_begin;
        maps[i] = 0;
    }
    end = bench_now_ns();
    free(map_sizes);
    free(maps);
    result = bench_result_make("release breakdown mmap/munmap burst", begin,
                               end, count * 2u);
    bench_result_set_latency(&result, latencies, count * 2u);
    free(latencies);
    return result;
}

static bench_result bench_release_mprotect_toggle(bench_heap_env *env,
                                                  size_t iterations,
                                                  size_t page_size) {
    enum { slot_count = 96 };
    void *maps[slot_count];
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)calloc(iterations + (slot_count * 2u),
                                   sizeof(latencies[0]));
    bench_expect(latencies != 0, "release breakdown mprotect latency samples");
    memset(maps, 0, sizeof(maps));
    begin = bench_now_ns();
    for (i = 0; i < slot_count; ++i) {
        uint64_t op_begin = bench_now_ns();
        bench_expect_ok(env->api->mmap(env->heap, &maps[i], page_size * 2u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "release breakdown mprotect setup");
        latencies[i] = bench_now_ns() - op_begin;
    }
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 13u) % slot_count;
        uint32_t prot = (i & 1u) != 0
                            ? UHM_PROT_READ
                            : (UHM_PROT_READ | UHM_PROT_WRITE);
        uint64_t op_begin = bench_now_ns();
        bench_expect_ok(env->api->mprotect(env->heap, maps[slot],
                                           page_size, prot),
                        "release breakdown mprotect toggle");
        latencies[slot_count + i] = bench_now_ns() - op_begin;
    }
    for (i = 0; i < slot_count; ++i) {
        uint64_t op_begin = bench_now_ns();
        bench_expect_ok(env->api->munmap(env->heap, maps[i], page_size * 2u),
                        "release breakdown mprotect cleanup");
        latencies[slot_count + iterations + i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    result = bench_result_make("release breakdown mprotect lifecycle", begin,
                               end, iterations + (slot_count * 2u));
    bench_result_set_latency(&result, latencies,
                             iterations + (slot_count * 2u));
    free(latencies);
    return result;
}

static void bench_mprotect_multi_vma_make_alternating(bench_heap_env *env,
                                                      void *map,
                                                      size_t page_size,
                                                      size_t page_count) {
    uintptr_t base = (uintptr_t)map;
    size_t i;

    for (i = 0; i < page_count; ++i) {
        uint32_t prot = (i & 1u) != 0 ? UHM_PROT_NONE : UHM_PROT_READ;
        bench_expect_ok(env->api->mprotect(env->heap,
                                           (void *)(base + (i * page_size)),
                                           page_size, prot),
                        "multi-VMA mprotect split");
    }
}

static bench_result bench_mprotect_multi_vma_pass(bench_heap_env *env,
                                                  size_t iterations,
                                                  size_t page_size,
                                                  size_t page_count,
                                                  const char *name) {
    const size_t map_size = page_count * page_size;
    uint64_t *latencies;
    void *map = 0;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)calloc(iterations, sizeof(latencies[0]));
    bench_expect(latencies != 0, "multi-VMA mprotect pass latency samples");
    bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "multi-VMA mprotect mmap");
    bench_mprotect_multi_vma_make_alternating(env, map, page_size,
                                              page_count);

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        uint64_t op_begin = bench_now_ns();
        bench_expect_ok(env->api->mprotect(env->heap, map, map_size,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "multi-VMA mprotect whole range");
        bench_mprotect_multi_vma_make_alternating(env, map, page_size,
                                                  page_count);
        latencies[i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();

    bench_expect_ok(env->api->mprotect(env->heap, map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE),
                    "multi-VMA mprotect restore before munmap");
    bench_expect_ok(env->api->munmap(env->heap, map, map_size),
                    "multi-VMA mprotect cleanup");
    result = bench_result_make(name, begin, end,
                               iterations * (page_count + 1u));
    bench_result_set_latency(&result, latencies, iterations);
    bench_result_set_latency_units(&result, (uint64_t)(page_count + 1u),
                                   (uint64_t)(page_count * 2u));
    free(latencies);
    return result;
}

static bench_result bench_mprotect_multi_vma_rollback(bench_heap_env *env,
                                                      size_t iterations,
                                                      size_t page_size,
                                                      size_t page_count,
                                                      const char *name) {
    const size_t map_size = page_count * page_size;
    bench_fault_vm_state fault_vm;
    uint64_t *latencies;
    void *map = 0;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)calloc(iterations, sizeof(latencies[0]));
    bench_expect(latencies != 0, "multi-VMA rollback latency samples");
    bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "multi-VMA rollback mmap");
    bench_mprotect_multi_vma_make_alternating(env, map, page_size,
                                              page_count);
    bench_fault_vm_install(env, &fault_vm);

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        uint64_t op_begin = bench_now_ns();
        fault_vm.fail_protect = 1;
        bench_expect(env->api->mprotect(env->heap, map, map_size,
                                        UHM_PROT_READ | UHM_PROT_WRITE) ==
                         UHM_ERR_PLATFORM,
                     "multi-VMA rollback expected protect failure");
        fault_vm.fail_protect = 0;
        bench_expect_ok(env->api->mprotect(env->heap, map, map_size,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "multi-VMA rollback retry");
        bench_mprotect_multi_vma_make_alternating(env, map, page_size,
                                                  page_count);
        latencies[i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();

    bench_fault_vm_restore(env, &fault_vm);
    bench_expect_ok(env->api->mprotect(env->heap, map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE),
                    "multi-VMA rollback restore before munmap");
    bench_expect_ok(env->api->munmap(env->heap, map, map_size),
                    "multi-VMA rollback cleanup");
    result = bench_result_make(name, begin, end,
                               iterations * (page_count + 2u));
    bench_result_set_latency(&result, latencies, iterations);
    bench_result_set_latency_units(&result, (uint64_t)(page_count + 2u),
                                   (uint64_t)(page_count * 3u));
    free(latencies);
    return result;
}

static void bench_mprotect_multi_vma_make_merge_candidates(bench_heap_env *env,
                                                           void *map,
                                                           size_t page_size) {
    uintptr_t base = (uintptr_t)map;

    bench_expect_ok(env->api->mprotect(env->heap, (void *)(base + page_size),
                                       page_size, UHM_PROT_READ),
                    "multi-VMA merge split first");
    bench_expect_ok(env->api->mprotect(env->heap,
                                       (void *)(base + (2u * page_size)),
                                       page_size, UHM_PROT_NONE),
                    "multi-VMA merge split second");
    bench_expect_ok(env->api->mprotect(env->heap,
                                       (void *)(base + (3u * page_size)),
                                       page_size, UHM_PROT_READ),
                    "multi-VMA merge split third");
    bench_expect_ok(env->api->mprotect(env->heap,
                                       (void *)(base + (4u * page_size)),
                                       page_size, UHM_PROT_NONE),
                    "multi-VMA merge split fourth");
}

static bench_result bench_mprotect_multi_vma_merge(bench_heap_env *env,
                                                   size_t iterations,
                                                   size_t page_size) {
    const size_t page_count = 6u;
    const size_t map_size = page_count * page_size;
    void *map = 0;
    size_t i;
    uint64_t begin;
    uint64_t end;

    bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "multi-VMA merge mmap");

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        bench_mprotect_multi_vma_make_merge_candidates(env, map, page_size);
        bench_expect_ok(env->api->mprotect(env->heap,
                                           (void *)((uintptr_t)map +
                                                    page_size),
                                           page_size * 4u,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "multi-VMA merge interior with neighbors");
    }
    end = bench_now_ns();

    bench_expect_ok(env->api->munmap(env->heap, map, map_size),
                    "multi-VMA merge cleanup");
    return bench_result_make("multi-VMA mprotect merge pressure", begin, end,
                             iterations * 5u);
}

static bench_result bench_mprotect_exact_neighbor_merge(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    const size_t map_size = page_size * 3u;
    void *map = 0;
    void *middle;
    size_t i;
    uint64_t begin;
    uint64_t end;

    bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "exact-neighbor merge mmap");
    middle = (void *)((uintptr_t)map + page_size);

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        bench_expect_ok(env->api->mprotect(env->heap, middle, page_size,
                                           UHM_PROT_READ),
                        "exact-neighbor merge split middle");
        bench_expect_ok(env->api->mprotect(env->heap, middle, page_size,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "exact-neighbor merge restore middle");
    }
    end = bench_now_ns();

    bench_expect_ok(env->api->munmap(env->heap, map, map_size),
                    "exact-neighbor merge cleanup");
    return bench_result_make("exact-neighbor mprotect merge pressure", begin,
                             end, iterations * 2u);
}

static bench_result bench_mprotect_partial_edge_merge(bench_heap_env *env,
                                                      size_t iterations,
                                                      size_t page_size) {
    const size_t page_count = 6u;
    const size_t map_size = page_count * page_size;
    void *map = 0;
    uintptr_t base;
    size_t i;
    uint64_t begin;
    uint64_t end;

    bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "partial-edge merge mmap");
    base = (uintptr_t)map;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        bench_expect_ok(env->api->mprotect(env->heap, map, page_size * 2u,
                                           UHM_PROT_READ),
                        "partial-edge merge seed head left");
        bench_expect_ok(env->api->mprotect(
                            env->heap, (void *)(base + page_size * 2u),
                            page_size * 2u, UHM_PROT_READ),
                        "partial-edge merge head");
        bench_expect_ok(env->api->mprotect(env->heap, map, map_size,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "partial-edge merge head restore");

        bench_expect_ok(env->api->mprotect(
                            env->heap, (void *)(base + page_size * 4u),
                            page_size * 2u, UHM_PROT_READ),
                        "partial-edge merge seed tail right");
        bench_expect_ok(env->api->mprotect(
                            env->heap, (void *)(base + page_size * 2u),
                            page_size * 2u, UHM_PROT_READ),
                        "partial-edge merge tail");
        bench_expect_ok(env->api->mprotect(env->heap, map, map_size,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "partial-edge merge tail restore");
    }
    end = bench_now_ns();

    bench_expect_ok(env->api->munmap(env->heap, map, map_size),
                    "partial-edge merge cleanup");
    return bench_result_make("partial-edge mprotect merge pressure", begin,
                             end, iterations * 6u);
}

static bench_result bench_mprotect_multi_vma_merge_rollback(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    const size_t page_count = 6u;
    const size_t map_size = page_count * page_size;
    bench_fault_vm_state fault_vm;
    void *map = 0;
    size_t i;
    uint64_t begin;
    uint64_t end;

    bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "multi-VMA merge rollback mmap");
    bench_fault_vm_install(env, &fault_vm);

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        bench_mprotect_multi_vma_make_merge_candidates(env, map, page_size);
        fault_vm.fail_protect = 1;
        bench_expect(env->api->mprotect(env->heap,
                                        (void *)((uintptr_t)map + page_size),
                                        page_size * 4u,
                                        UHM_PROT_READ | UHM_PROT_WRITE) ==
                         UHM_ERR_PLATFORM,
                     "multi-VMA merge rollback expected protect failure");
        fault_vm.fail_protect = 0;
        bench_expect_ok(env->api->mprotect(env->heap,
                                           (void *)((uintptr_t)map +
                                                    page_size),
                                           page_size * 4u,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "multi-VMA merge rollback retry");
    }
    end = bench_now_ns();

    bench_fault_vm_restore(env, &fault_vm);
    bench_expect_ok(env->api->munmap(env->heap, map, map_size),
                    "multi-VMA merge rollback cleanup");
    return bench_result_make("multi-VMA mprotect merge rollback pressure",
                             begin, end, iterations * 6u);
}

static bench_result bench_mprotect_multi_vma_random_fault(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    const size_t max_page_count = 32u;
    const size_t map_size = max_page_count * page_size;
    static const size_t page_counts[] = {8u, 16u, 32u};
    bench_fault_vm_state fault_vm;
    uint64_t rng = UINT64_C(0x714f4d41534d5056);
    void *map = 0;
    size_t i;
    size_t operations = 0;
    size_t injected_failures = 0;
    uint64_t begin;
    uint64_t end;

    bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                   UHM_PROT_READ | UHM_PROT_WRITE,
                                   UHM_MAP_PRIVATE),
                    "multi-VMA random fault mmap");
    bench_fault_vm_install(env, &fault_vm);

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        uint32_t r = bench_next_random(&rng);
        size_t page_count = page_counts[r % (sizeof(page_counts) /
                                             sizeof(page_counts[0]))];
        size_t range_size = page_count * page_size;
        int inject_failure = ((r >> 8) & 7u) < 3u;

        bench_mprotect_multi_vma_make_alternating(env, map, page_size,
                                                  page_count);
        operations += page_count;

        if (inject_failure) {
            fault_vm.fail_protect = 1;
            bench_expect(env->api->mprotect(env->heap, map, range_size,
                                            UHM_PROT_READ |
                                                UHM_PROT_WRITE) ==
                             UHM_ERR_PLATFORM,
                         "multi-VMA random fault expected protect failure");
            fault_vm.fail_protect = 0;
            ++operations;
            ++injected_failures;
        }

        bench_expect_ok(env->api->mprotect(env->heap, map, range_size,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "multi-VMA random fault retry/pass");
        ++operations;
    }
    end = bench_now_ns();

    bench_expect(injected_failures != 0,
                 "multi-VMA random fault saw at least one failure");
    bench_fault_vm_restore(env, &fault_vm);
    bench_expect_ok(env->api->mprotect(env->heap, map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE),
                    "multi-VMA random fault restore before munmap");
    bench_expect_ok(env->api->munmap(env->heap, map, map_size),
                    "multi-VMA random fault cleanup");
    return bench_result_make("multi-VMA mprotect random fault pressure",
                             begin, end, operations);
}

static bench_result bench_munmap_exact_vma(bench_heap_env *env,
                                           size_t iterations,
                                           size_t page_size) {
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    bench_expect(latencies != 0, "munmap exact VMA latency samples");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        uint64_t op_begin = bench_now_ns();

        bench_expect_ok(env->api->mmap(env->heap, &map, page_size * 2u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "munmap exact mmap");
        bench_expect_ok(env->api->munmap(env->heap, map, page_size * 2u),
                        "munmap exact whole VMA");
        latencies[i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    result = bench_result_make("munmap exact VMA pressure", begin, end,
                               iterations * 2u);
    bench_result_set_latency(&result, latencies, iterations);
    bench_result_set_latency_units(&result, 2u, 4u);
    free(latencies);
    return result;
}

static bench_result bench_munmap_head_tail_middle(bench_heap_env *env,
                                                  size_t iterations,
                                                  size_t page_size) {
    const size_t map_size = page_size * 4u;
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    bench_expect(latencies != 0,
                 "munmap head/tail/middle latency samples");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        uintptr_t base;
        void *map = 0;
        uint64_t op_begin = bench_now_ns();

        bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "munmap head mmap");
        bench_expect_ok(env->api->munmap(env->heap, map, page_size),
                        "munmap head trim");
        bench_expect_ok(env->api->munmap(env->heap,
                                         (void *)((uintptr_t)map + page_size),
                                         page_size * 3u),
                        "munmap head cleanup");

        bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "munmap tail mmap");
        bench_expect_ok(env->api->munmap(env->heap,
                                         (void *)((uintptr_t)map +
                                                  (page_size * 3u)),
                                         page_size),
                        "munmap tail trim");
        bench_expect_ok(env->api->munmap(env->heap, map, page_size * 3u),
                        "munmap tail cleanup");

        bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "munmap middle mmap");
        base = (uintptr_t)map;
        bench_expect_ok(env->api->munmap(env->heap,
                                         (void *)(base + page_size),
                                         page_size * 2u),
                        "munmap middle hole");
        bench_expect_ok(env->api->munmap(env->heap, (void *)base, page_size),
                        "munmap middle left cleanup");
        bench_expect_ok(env->api->munmap(env->heap,
                                         (void *)(base + (page_size * 3u)),
                                         page_size),
                        "munmap middle right cleanup");
        latencies[i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    result = bench_result_make("munmap head tail middle pressure", begin, end,
                               iterations * 9u);
    bench_result_set_latency(&result, latencies, iterations);
    bench_result_set_latency_units(&result, 9u, 24u);
    free(latencies);
    return result;
}

static bench_result bench_munmap_right_small_middle(bench_heap_env *env,
                                                    size_t iterations,
                                                    size_t page_size) {
    const size_t page_count = 8u;
    const size_t map_size = page_count * page_size;
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    bench_expect(latencies != 0,
                 "munmap right-small middle latency samples");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        uintptr_t base;
        void *map = 0;
        uint64_t op_begin = bench_now_ns();

        bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "munmap right-small middle mmap");
        base = (uintptr_t)map;
        bench_expect_ok(env->api->munmap(env->heap,
                                         (void *)(base + 5u * page_size),
                                         page_size),
                        "munmap right-small middle hole");
        bench_expect_ok(env->api->munmap(env->heap, (void *)base,
                                         5u * page_size),
                        "munmap right-small middle left cleanup");
        bench_expect_ok(env->api->munmap(env->heap,
                                         (void *)(base + 6u * page_size),
                                         2u * page_size),
                        "munmap right-small middle right cleanup");
        latencies[i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    result = bench_result_make("munmap right-small middle pressure", begin,
                               end, iterations * 4u);
    bench_result_set_latency(&result, latencies, iterations);
    bench_result_set_latency_units(&result, 4u, 16u);
    free(latencies);
    return result;
}

static bench_result bench_munmap_multi_vma_full(bench_heap_env *env,
                                                size_t iterations,
                                                size_t page_size,
                                                size_t page_count,
                                                const char *name) {
    const size_t map_size = page_count * page_size;
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    bench_expect(latencies != 0, "multi-VMA munmap full latency samples");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        uint64_t op_begin = bench_now_ns();

        bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "multi-VMA munmap mmap");
        bench_mprotect_multi_vma_make_alternating(env, map, page_size,
                                                  page_count);
        bench_expect_ok(env->api->munmap(env->heap, map, map_size),
                        "multi-VMA munmap whole range");
        latencies[i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    result = bench_result_make(name, begin, end,
                               iterations * (page_count + 2u));
    bench_result_set_latency(&result, latencies, iterations);
    bench_result_set_latency_units(&result, (uint64_t)(page_count + 2u),
                                   (uint64_t)(page_count * 3u));
    free(latencies);
    return result;
}

static bench_result bench_munmap_multi_vma_partial(bench_heap_env *env,
                                                   size_t iterations,
                                                   size_t page_size) {
    const size_t page_count = 6u;
    const size_t map_size = page_count * page_size;
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    bench_expect(latencies != 0,
                 "multi-VMA munmap partial latency samples");
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        uintptr_t base;
        uint64_t op_begin = bench_now_ns();

        bench_expect_ok(env->api->mmap(env->heap, &map, map_size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "multi-VMA partial munmap mmap");
        base = (uintptr_t)map;
        bench_expect_ok(env->api->mprotect(
                            env->heap, (void *)(base + 2u * page_size),
                            page_size, UHM_PROT_READ),
                        "multi-VMA partial munmap split");
        bench_expect_ok(env->api->munmap(env->heap,
                                         (void *)(base + page_size),
                                         3u * page_size),
                        "multi-VMA partial munmap range");
        bench_expect_ok(env->api->munmap(env->heap, (void *)base, page_size),
                        "multi-VMA partial munmap left cleanup");
        bench_expect_ok(env->api->munmap(env->heap,
                                         (void *)(base + 4u * page_size),
                                         2u * page_size),
                        "multi-VMA partial munmap right cleanup");
        latencies[i] = bench_now_ns() - op_begin;
    }
    end = bench_now_ns();
    result = bench_result_make("multi-VMA munmap partial trim", begin, end,
                               iterations * 5u);
    bench_result_set_latency(&result, latencies, iterations);
    bench_result_set_latency_units(&result, 5u, 13u);
    free(latencies);
    return result;
}

