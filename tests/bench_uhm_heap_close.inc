enum {
    BENCH_HEAP_CLOSE_SAMPLE_COUNT = 9u,
    BENCH_HEAP_CLOSE_MAX_CONTEXT_COUNT = 9u,
    BENCH_HEAP_CLOSE_MAX_CLASS_QUEUE_NODE_COUNT = 2u
};

typedef struct bench_heap_close_run {
    bench_result result;
    bench_diag_snapshot representative_before;
    bench_diag_snapshot representative_after;
    uint64_t closed_heaps;
    uint64_t total_contexts;
    uint64_t invalidated_contexts;
    uint64_t class_queue_nodes;
    uint64_t class_queue_fallback_frees;
    uint64_t vm_release_calls;
    uint64_t vm_decommit_calls;
    uint64_t vm_failures;
    uint64_t vm_lock_held_calls;
    uint64_t vm_release_lock_held_calls;
    uint64_t active_ops_after;
    uint64_t registered_readers_after;
    uint64_t registry_leaks;
    uint64_t queue_leaks;
    size_t contexts_per_close;
    size_t class_queue_nodes_per_close;
    const char *metadata_name;
    uint32_t lockless_remote_supported;
    uint32_t vm_counters_available;
} bench_heap_close_run;

static size_t bench_tagged_freelist_node_count(
    const uhm_tagged_freelist *queue, size_t max_nodes) {
    uintptr_t packed;
    uhm_tagged_freelist_node *node;
    size_t count = 0u;

    if (!queue || !uhm_tagged_freelist_is_supported()) {
        return 0u;
    }
    packed = atomic_load_explicit(&queue->head, UHM_ATOMIC_ACQUIRE);
    node = uhm_tagged_freelist_unpack_ptr(packed);
    while (node) {
        ++count;
        bench_expect(count <= max_nodes,
                      "heap close class queue bounded node count");
        node = uhm_tagged_freelist_next_load(node);
    }
    return count;
}
static int bench_heap_remote_queues_empty(const uhm_heap *heap) {
    size_t size_class;

    if (!heap) {
        return 0;
    }
    for (size_class = 0u; size_class < UHM_SIZE_CLASS_COUNT; ++size_class) {
        uintptr_t class_head = atomic_load_explicit(
            &heap->core.slab_remote_class_lf[size_class].queue.head,
            UHM_ATOMIC_ACQUIRE);
        uintptr_t ready_head = atomic_load_explicit(
            &heap->core.slab_remote_ready_lf[size_class].queue.head,
            UHM_ATOMIC_ACQUIRE);

        if (uhm_tagged_freelist_unpack_ptr(class_head) != 0 ||
            uhm_tagged_freelist_unpack_ptr(ready_head) != 0) {
            return 0;
        }
    }
    return 1;
}

static void bench_heap_close_capture_safe_after(
    const bench_heap_env *env, const bench_diag_snapshot *before,
    bench_diag_snapshot *after) {
    const uhm_heap_core *core;

    bench_expect(env != 0 && env->heap != 0 && before != 0 && after != 0,
                 "heap close safe diagnostic capture arguments");
    *after = *before;
    core = &env->heap->core;

    after->vm_decommit_calls = core->vm_decommit_calls;
    after->vm_decommit_failures = core->vm_decommit_failures;
    after->vm_decommit_ns = core->vm_decommit_ns;
    after->vm_release_calls = core->vm_release_calls;
    after->vm_release_failures = core->vm_release_failures;
    after->vm_release_ns = core->vm_release_ns;
    after->vm_lock_held_calls = core->vm_lock_held_calls;
    after->vm_lock_held_failures = core->vm_lock_held_failures;
    after->vm_lock_held_ns = core->vm_lock_held_ns;
    after->vm_decommit_lock_held_calls = core->vm_decommit_lock_held_calls;
    after->vm_release_lock_held_calls = core->vm_release_lock_held_calls;
#if UHMAS_ENABLE_DIAGNOSTICS
    after->heap_lock_calls =
        uhm_diag_counter_load_relaxed(&env->heap->lock.lock_calls);
    after->heap_lock_try_calls =
        uhm_diag_counter_load_relaxed(&env->heap->lock.try_calls);
    after->heap_lock_try_failures =
        uhm_diag_counter_load_relaxed(&env->heap->lock.try_failures);
    after->heap_lock_contended_calls =
        uhm_diag_counter_load_relaxed(&env->heap->lock.contended_calls);
    after->heap_lock_wait_ns =
        uhm_diag_counter_load_relaxed(&env->heap->lock.wait_ns);
    after->heap_lock_max_wait_ns = uhm_diag_counter_exchange_relaxed(
        &env->heap->lock.max_wait_ns, UINT64_C(0));
    after->heap_lock_hold_ns =
        uhm_diag_counter_load_relaxed(&env->heap->lock.hold_ns);
    after->heap_lock_max_hold_ns = uhm_diag_counter_exchange_relaxed(
        &env->heap->lock.max_hold_ns, UINT64_C(0));
#endif
}

static void bench_heap_close_release_wrapper_storage(bench_heap_env *env) {
    uhm_result result;

    bench_expect(env != 0 && env->owner.vm.release != 0 &&
                     env->owner.heap_storage != 0 &&
                     env->owner.heap_storage_reserved_size != 0u,
                 "heap close wrapper storage release arguments");
    result = env->owner.vm.release(
        env->owner.vm.user, env->owner.heap_storage,
        env->owner.heap_storage_reserved_size);
    bench_expect_ok(result, "heap close wrapper storage release");
    memset(env, 0, sizeof(*env));
}

static bench_heap_close_run bench_heap_close_owned_contexts(
    size_t arena_size, size_t context_count, size_t class_queue_node_count,
    const char *result_name, const char *metadata_name) {
    bench_heap_close_run run;
    uhm_storage_info ctx_info;
    uint64_t latencies[BENCH_HEAP_CLOSE_SAMPLE_COUNT] = {0};
    uint64_t total_close_ns = 0u;
    size_t sample;

    memset(&run, 0, sizeof(run));
    memset(&ctx_info, 0, sizeof(ctx_info));
    bench_expect(context_count != 0u &&
                     context_count <= BENCH_HEAP_CLOSE_MAX_CONTEXT_COUNT &&
                     class_queue_node_count <=
                         BENCH_HEAP_CLOSE_MAX_CLASS_QUEUE_NODE_COUNT &&
                     result_name != 0 && metadata_name != 0,
                 "heap close context benchmark configuration");
    ctx_info.size = sizeof(ctx_info);
    bench_expect_ok(uhm_heap_storage_query(0, &ctx_info),
                    "heap close context storage query");
    run.lockless_remote_supported =
        uhm_lockless_remote_primitives_supported() ? 1u : 0u;
    run.vm_counters_available = UHMAS_ENABLE_DIAGNOSTICS ? 1u : 0u;
    run.contexts_per_close = context_count;
    run.class_queue_nodes_per_close = class_queue_node_count;
    run.metadata_name = metadata_name;

    for (sample = 0u; sample < BENCH_HEAP_CLOSE_SAMPLE_COUNT; ++sample) {
        bench_heap_env env;
        uhm_thread_ctx *contexts[BENCH_HEAP_CLOSE_MAX_CONTEXT_COUNT] = {0};
        void *ctx_raws[BENCH_HEAP_CLOSE_MAX_CONTEXT_COUNT] = {0};
        uhm_thread_config ctx_config;
        void *class_ptrs[BENCH_HEAP_CLOSE_MAX_CLASS_QUEUE_NODE_COUNT] = {0};
        uhm_slab *class_slab = 0;
        size_t size_class;
        size_t context_index;
        size_t queue_node_count = 0u;
        uint64_t vm_release_before;
        uint64_t vm_decommit_before;
        uint64_t vm_failures_before;
        uint64_t vm_lock_held_before;
        uint64_t vm_release_lock_held_before;
        uint64_t begin;
        uint64_t end;

        bench_heap_open_no_tls_config_flags_ex(
            &env, arena_size, UHM_CONFIG_LOCKLESS_REMOTE_FREE, 0u);
        bench_expect(env.heap->core.owns_arena != 0u,
                     "heap close benchmark owns arena");
        bench_expect(env.api->thread_ctx_open != 0 &&
                         env.api->malloc_ctx != 0 && env.api->free_ctx != 0,
                     "heap close context API available");

        for (context_index = 0u;
             context_index < context_count;
             ++context_index) {
            void *ctx_storage = bench_aligned_host_alloc(
                ctx_info.storage_size, ctx_info.storage_alignment,
                &ctx_raws[context_index]);
            void *ptr;

            bench_expect(ctx_storage != 0,
                         "heap close context storage allocation");
            memset(&ctx_config, 0, sizeof(ctx_config));
            ctx_config.size = sizeof(ctx_config);
            ctx_config.abi_major = UHM_ABI_MAJOR;
            ctx_config.abi_minor = UHM_ABI_MINOR;
            ctx_config.storage = ctx_storage;
            ctx_config.storage_size = ctx_info.storage_size;
            ctx_config.local_cache_bytes = env.heap->F.vm.page_size;
            bench_expect_ok(
                env.api->thread_ctx_open(env.heap, &ctx_config,
                                         &contexts[context_index]),
                "heap close context open");
            ptr = env.api->malloc_ctx(env.heap, contexts[context_index], 64u);
            bench_expect(ptr != 0, "heap close context allocation");
            env.api->free_ctx(env.heap, contexts[context_index], ptr);
            bench_expect(contexts[context_index]->local_cache_count == 1u,
                         "heap close context retains one slot");
        }
        bench_expect(env.heap->core.thread_ctx_head != 0,
                     "heap close context registry active");

        for (context_index = 0u;
             context_index < class_queue_node_count;
             ++context_index) {
            class_ptrs[context_index] = env.api->malloc(env.heap, 512u);
            bench_expect(class_ptrs[context_index] != 0,
                         "heap close class queue allocation");
            if (context_index == 0u) {
                class_slab = bench_find_slab_by_payload(
                    &env, class_ptrs[context_index]);
                bench_expect(class_slab != 0,
                             "heap close class queue slab");
            }
        }
        if (class_queue_node_count != 0u) {
            size_class = class_slab->size_class;
            for (context_index = 0u;
                 context_index < class_queue_node_count;
                 ++context_index) {
                env.api->free(env.heap, class_ptrs[context_index]);
            }
            if (run.lockless_remote_supported) {
                queue_node_count = bench_tagged_freelist_node_count(
                    &env.heap->core
                         .slab_remote_class_lf[size_class].queue,
                    class_queue_node_count);
                bench_expect(queue_node_count == class_queue_node_count,
                             "heap close exact class queue setup");
            }
        }

        for (size_class = 0u; size_class < UHM_SIZE_CLASS_COUNT;
             ++size_class) {
            env.heap->core.slab_empty_committed_cache_high_water[size_class] =
                uhm_slab_empty_committed_cache_state_make(
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER,
                    0u);
        }
        bench_check_heap(&env, "heap close benchmark pre-close check");
        if (sample == 0u) {
            bench_diag_capture(&env, &run.representative_before);
        }

        vm_release_before = env.heap->core.vm_release_calls;
        vm_decommit_before = env.heap->core.vm_decommit_calls;
        vm_failures_before = env.heap->core.vm_release_failures +
                             env.heap->core.vm_decommit_failures;
        vm_lock_held_before = env.heap->core.vm_lock_held_calls;
        vm_release_lock_held_before =
            env.heap->core.vm_release_lock_held_calls;
        begin = bench_now_ns();
        bench_expect_ok(env.api->close(env.heap),
                        "heap close benchmark API close");
        end = bench_now_ns();
        latencies[sample] = end - begin;
        total_close_ns += latencies[sample];

        bench_expect(atomic_load_explicit(&env.heap->lifecycle_state,
                                          UHM_ATOMIC_ACQUIRE) ==
                         UHM_CLOSED_MAGIC &&
                         env.heap->magic == UHM_CLOSED_MAGIC,
                     "heap close benchmark closed lifecycle");
        bench_expect(env.heap->core.thread_ctx_head == 0 &&
                         env.heap->core.tls_cache_head == 0 &&
                         env.heap->core.tls_cache_active_count == 0u,
                     "heap close benchmark registries detached");
        bench_expect(uhm_heap_active_ops_load(env.heap) == 0u,
                     "heap close benchmark active ops drained");
        bench_expect(uhm_metadata_registered_readers_load(env.heap) == 0u,
                     "heap close benchmark readers drained");
        bench_expect(bench_heap_remote_queues_empty(env.heap),
                     "heap close benchmark queues drained");
        if (run.vm_counters_available) {
            bench_expect(
                env.heap->core.vm_decommit_calls == vm_decommit_before,
                "heap close benchmark no decommit");
            bench_expect(env.heap->core.vm_release_calls ==
                             vm_release_before + 1u,
                         "heap close benchmark one arena release");
            bench_expect(env.heap->core.vm_release_failures +
                                 env.heap->core.vm_decommit_failures ==
                             vm_failures_before,
                         "heap close benchmark no VM failure");
            bench_expect(env.heap->core.vm_lock_held_calls ==
                             vm_lock_held_before + 1u &&
                             env.heap->core.vm_release_lock_held_calls ==
                                 vm_release_lock_held_before + 1u,
                         "heap close benchmark terminal release under close lock");
        }
        if (sample == 0u) {
            bench_heap_close_capture_safe_after(
                &env, &run.representative_before,
                &run.representative_after);
        }

        ++run.closed_heaps;
        run.total_contexts += context_count;
        run.class_queue_nodes += queue_node_count;
        if (!run.lockless_remote_supported) {
            run.class_queue_fallback_frees +=
                class_queue_node_count;
        }
        if (run.vm_counters_available) {
            run.vm_release_calls +=
                env.heap->core.vm_release_calls - vm_release_before;
            run.vm_decommit_calls +=
                env.heap->core.vm_decommit_calls - vm_decommit_before;
            run.vm_failures +=
                env.heap->core.vm_release_failures +
                env.heap->core.vm_decommit_failures - vm_failures_before;
            run.vm_lock_held_calls +=
                env.heap->core.vm_lock_held_calls - vm_lock_held_before;
            run.vm_release_lock_held_calls +=
                env.heap->core.vm_release_lock_held_calls -
                vm_release_lock_held_before;
        }
        run.active_ops_after += uhm_heap_active_ops_load(env.heap);
        run.registered_readers_after +=
            uhm_metadata_registered_readers_load(env.heap);
        if (env.heap->core.thread_ctx_head != 0 ||
            env.heap->core.tls_cache_head != 0 ||
            env.heap->core.tls_cache_active_count != 0u) {
            ++run.registry_leaks;
        }
        if (!bench_heap_remote_queues_empty(env.heap)) {
            ++run.queue_leaks;
        }
        for (context_index = 0u;
             context_index < context_count;
             ++context_index) {
            if (contexts[context_index]->magic == 0u &&
                contexts[context_index]->owner == 0) {
                ++run.invalidated_contexts;
            }
            free(ctx_raws[context_index]);
        }
        bench_heap_close_release_wrapper_storage(&env);
    }

    run.result = bench_result_make(result_name, 0u, total_close_ns,
                                   BENCH_HEAP_CLOSE_SAMPLE_COUNT);
    bench_result_set_latency(&run.result, latencies,
                             BENCH_HEAP_CLOSE_SAMPLE_COUNT);
    bench_result_set_latency_units(&run.result,
                                   context_count, 0u);
    return run;
}

static void bench_heap_close_print_run(const bench_heap_close_run *run,
                                       int diagnostics_enabled) {
    bench_expect(run != 0, "heap close print run");
    bench_print(&run->result);
    if (diagnostics_enabled) {
        bench_diag_print_delta(
            run->result.name, &run->representative_before,
            &run->representative_after);
    }
    printf("metadata %s: "
           "latency_samples=%llu representative_close_count=1 "
           "contexts_per_close=%u class_queue_nodes_per_close=%u "
           "lockless_remote_supported=%u vm_counters_available=%u "
           "closed_heaps=%llu "
           "total_contexts=%llu invalidated_contexts=%llu "
           "class_queue_nodes=%llu class_queue_fallback_frees=%llu "
           "vm_release_calls=%llu "
           "vm_decommit_calls=%llu vm_failures=%llu "
           "vm_lock_held_calls=%llu vm_release_lock_held_calls=%llu "
           "active_ops_after=%llu registered_readers_after=%llu "
           "registry_leaks=%llu queue_leaks=%llu\n",
           run->metadata_name,
           (unsigned long long)run->result.latency_sample_count,
           (unsigned)run->contexts_per_close,
           (unsigned)run->class_queue_nodes_per_close,
           run->lockless_remote_supported,
           run->vm_counters_available,
           (unsigned long long)run->closed_heaps,
           (unsigned long long)run->total_contexts,
           (unsigned long long)run->invalidated_contexts,
           (unsigned long long)run->class_queue_nodes,
           (unsigned long long)run->class_queue_fallback_frees,
           (unsigned long long)run->vm_release_calls,
           (unsigned long long)run->vm_decommit_calls,
           (unsigned long long)run->vm_failures,
           (unsigned long long)run->vm_lock_held_calls,
           (unsigned long long)run->vm_release_lock_held_calls,
           (unsigned long long)run->active_ops_after,
           (unsigned long long)run->registered_readers_after,
           (unsigned long long)run->registry_leaks,
           (unsigned long long)run->queue_leaks);
}
