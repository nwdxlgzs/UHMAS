enum {
    BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT = 9u,
    BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT = 3u,
    BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT = 3u,
    BENCH_HEAP_CLOSE_MIXED_MAX_CLASS_QUEUE_NODE_COUNT = 32u
};

typedef struct bench_heap_close_phase_run {
    bench_result wall_result;
    uint64_t phase_calls;
    uint64_t cpu_sample_count;
    uint64_t cpu_min_work;
    uint64_t cpu_p50_work;
    uint64_t cpu_p95_work;
    uint64_t cpu_p99_work;
    uint64_t cpu_max_work;
    uint32_t cpu_work_valid;
    uint32_t cpu_work_is_cycles;
} bench_heap_close_phase_run;

typedef struct bench_heap_close_mixed_run {
    bench_result result;
    bench_diag_snapshot representative_before;
    bench_diag_snapshot representative_after;
    uint64_t closed_heaps;
    uint64_t total_contexts;
    uint64_t invalidated_contexts;
    uint64_t total_workers;
    uint64_t ready_caches;
    uint64_t cached_slots;
    uint64_t invalidated_caches;
    uint64_t class_queue_nodes;
    uint64_t class_queue_fallback_frees;
    uint64_t class_queue_multi_slab_closes;
    uint64_t worker_failures;
    uint64_t vm_release_calls;
    uint64_t vm_decommit_calls;
    uint64_t vm_failures;
    uint64_t vm_lock_held_calls;
    uint64_t vm_release_lock_held_calls;
    uint64_t active_ops_after;
    uint64_t registered_readers_after;
    uint64_t registry_leaks;
    uint64_t queue_leaks;
    uint64_t caller_arena_probe_calls;
    uint64_t caller_arena_probe_failures;
    uint64_t caller_release_calls;
    uint64_t caller_release_failures;
    uint64_t arena_release_phase_calls;
    size_t contexts_per_close;
    size_t workers_per_close;
    size_t class_queue_nodes_per_close;
    const char *metadata_name;
    const char *phase_scope;
    uint32_t lockless_remote_supported;
    uint32_t vm_counters_available;
    uint32_t arena_owned;
    uint32_t phase_breakdown_requested;
    uint32_t phase_timing_available;
    bench_heap_close_phase_run phases[UHM_CLOSE_PHASE_COUNT];
} bench_heap_close_mixed_run;

static bench_heap_close_mixed_run bench_heap_close_mixed(
    size_t arena_size, uint32_t arena_owned, size_t class_queue_node_count,
    const char *result_name, const char *metadata_name,
    const char *phase_scope) {
    bench_heap_close_mixed_run run;
    uhm_storage_info ctx_info;
    uint64_t latencies[BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT] = {0};
    uint64_t total_close_ns = 0u;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t phase_wall_samples[UHM_CLOSE_PHASE_COUNT]
                               [BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT] = {{0}};
    uint64_t phase_cpu_samples[UHM_CLOSE_PHASE_COUNT]
                              [BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT] = {{0}};
    uint32_t phase_wall_sample_valid[UHM_CLOSE_PHASE_COUNT]
                                    [BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT] = {{0}};
    uint32_t phase_cpu_sample_valid[UHM_CLOSE_PHASE_COUNT]
                                   [BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT] = {{0}};
    uint32_t phase_cpu_work_is_cycles[UHM_CLOSE_PHASE_COUNT] = {0};
    uint32_t phase_cpu_work_unit_set[UHM_CLOSE_PHASE_COUNT] = {0};
#endif
    size_t sample;

    memset(&run, 0, sizeof(run));
    memset(&ctx_info, 0, sizeof(ctx_info));
    bench_expect(arena_size != 0u && class_queue_node_count != 0u &&
                     class_queue_node_count <=
                         BENCH_HEAP_CLOSE_MIXED_MAX_CLASS_QUEUE_NODE_COUNT &&
                     result_name != 0 && metadata_name != 0,
                  "heap close mixed benchmark configuration");
    ctx_info.size = sizeof(ctx_info);
    bench_expect_ok(uhm_heap_storage_query(0, &ctx_info),
                    "heap close mixed context storage query");
    run.contexts_per_close = BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT;
    run.workers_per_close = BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT;
    run.class_queue_nodes_per_close = class_queue_node_count;
    run.metadata_name = metadata_name;
    run.phase_scope = phase_scope;
    run.phase_breakdown_requested = phase_scope != 0 ? 1u : 0u;
#if UHMAS_ENABLE_DIAGNOSTICS
    run.phase_timing_available =
        bench_diagnostics_enabled() && bench_diagnostics_timing_enabled()
            ? 1u
            : 0u;
#else
    run.phase_timing_available = 0u;
#endif
    run.lockless_remote_supported =
        uhm_lockless_remote_primitives_supported() ? 1u : 0u;
    run.vm_counters_available = UHMAS_ENABLE_DIAGNOSTICS ? 1u : 0u;
    run.arena_owned = arena_owned ? 1u : 0u;

    for (sample = 0u; sample < BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT;
         ++sample) {
        bench_heap_env env;
        uhm_thread_ctx *contexts[BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT] = {0};
        void *ctx_raws[BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT] = {0};
        uhm_thread_config ctx_config;
        void *class_ptrs[BENCH_HEAP_CLOSE_MIXED_MAX_CLASS_QUEUE_NODE_COUNT] = {
            0};
        uhm_slab *class_slabs[
            BENCH_HEAP_CLOSE_MIXED_MAX_CLASS_QUEUE_NODE_COUNT] = {0};
        uhm_slab *class_slab = 0;
        uhm_tls_cache *caches[BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT] = {0};
        bench_heap_close_tls_worker_args
            args[BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT];
        _Atomic(uint32_t) ready_count;
        _Atomic(uint32_t) release;
        size_t ready_caches;
        size_t cached_slots;
        size_t invalidated_caches = 0u;
        size_t class_slab_count = 0u;
        size_t size_class;
        size_t context_index;
        size_t worker;
        size_t queue_node_count = 0u;
        void *caller_arena = 0;
        uint64_t vm_release_before;
        uint64_t vm_decommit_before;
        uint64_t vm_failures_before;
        uint64_t vm_lock_held_before;
        uint64_t vm_release_lock_held_before;
        uint64_t begin;
        uint64_t end;
#if defined(_WIN32)
        HANDLE threads[BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT] = {0};
#else
        pthread_t threads[BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT];
#endif

        if (run.arena_owned) {
            bench_heap_open_config_flags_slots_ex(
                &env, arena_size, 1, BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT,
                UHM_CONFIG_LOCKLESS_REMOTE_FREE, 0u);
        } else {
            bench_heap_open_pre_reserved_flags_slots_ex(
                &env, arena_size, BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT,
                UHM_CONFIG_LOCKLESS_REMOTE_FREE, &caller_arena);
        }
        bench_expect(env.heap->core.owns_arena == run.arena_owned,
                     "heap close mixed arena ownership");
        bench_expect((run.arena_owned && caller_arena == 0) ||
                         (!run.arena_owned && caller_arena != 0),
                     "heap close mixed caller arena ownership");
        bench_expect(env.api->thread_ctx_open != 0 &&
                         env.api->malloc_ctx != 0 && env.api->free_ctx != 0,
                     "heap close mixed context API available");

        for (context_index = 0u;
             context_index < BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT;
             ++context_index) {
            void *ctx_storage = bench_aligned_host_alloc(
                ctx_info.storage_size, ctx_info.storage_alignment,
                &ctx_raws[context_index]);
            void *ptr;

            bench_expect(ctx_storage != 0,
                         "heap close mixed context storage allocation");
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
                "heap close mixed context open");
            ptr = env.api->malloc_ctx(env.heap, contexts[context_index], 64u);
            bench_expect(ptr != 0, "heap close mixed context allocation");
            env.api->free_ctx(env.heap, contexts[context_index], ptr);
            bench_expect(contexts[context_index]->local_cache_count == 1u,
                         "heap close mixed context retains one slot");
        }

        for (context_index = 0u;
             context_index < class_queue_node_count;
             ++context_index) {
            uhm_slab *current_slab;
            size_t slab_index;

            class_ptrs[context_index] = env.api->malloc_ctx(
                env.heap, contexts[0], 512u);
            bench_expect(class_ptrs[context_index] != 0,
                          "heap close mixed class queue allocation");
            current_slab = bench_find_slab_by_payload(
                &env, class_ptrs[context_index]);
            bench_expect(current_slab != 0,
                         "heap close mixed class queue slab");
            if (context_index == 0u) {
                class_slab = current_slab;
            } else {
                bench_expect(current_slab->size_class ==
                                 class_slab->size_class,
                             "heap close mixed class queue size class");
            }
            for (slab_index = 0u; slab_index < class_slab_count;
                 ++slab_index) {
                if (class_slabs[slab_index] == current_slab) {
                    break;
                }
            }
            if (slab_index == class_slab_count) {
                class_slabs[class_slab_count++] = current_slab;
            }
        }
        bench_expect(class_queue_node_count != 1u || class_slab_count == 1u,
                     "heap close mixed q1 uses one slab");
        bench_expect(class_queue_node_count !=
                             BENCH_HEAP_CLOSE_MIXED_MAX_CLASS_QUEUE_NODE_COUNT ||
                         class_slab_count > 1u,
                     "heap close mixed q32 spans multiple slabs");
        size_class = class_slab->size_class;
        memset(args, 0, sizeof(args));
        atomic_init(&ready_count, 0u);
        atomic_init(&release, 0u);
        for (worker = 0u;
             worker < BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT; ++worker) {
            args[worker].api = env.api;
            args[worker].heap = env.heap;
            args[worker].ready_count = &ready_count;
            args[worker].release = &release;
#if defined(_WIN32)
            threads[worker] = CreateThread(
                0, 0, bench_heap_close_tls_worker_entry, &args[worker], 0, 0);
            bench_expect(threads[worker] != 0,
                         "heap close mixed worker create");
#else
            bench_expect(pthread_create(&threads[worker], 0,
                                        bench_heap_close_tls_worker_entry,
                                        &args[worker]) == 0,
                         "heap close mixed worker create");
#endif
        }
        while (atomic_load_explicit(&ready_count, UHM_ATOMIC_ACQUIRE) !=
               BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT) {
            bench_thread_yield();
        }
        for (worker = 0u;
             worker < BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT; ++worker) {
            bench_expect(args[worker].ok,
                         "heap close mixed worker setup");
        }
        bench_expect(env.heap->core.tls_cache_active_count ==
                         BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT,
                     "heap close mixed TLS caches active");
        for (context_index = 0u;
             context_index < class_queue_node_count;
             ++context_index) {
            env.api->free(env.heap, class_ptrs[context_index]);
            class_ptrs[context_index] = 0;
        }
        ready_caches = bench_heap_close_tls_snapshot_caches(
            env.heap, caches, BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT,
            &cached_slots);
        bench_expect(ready_caches == BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT &&
                         cached_slots == BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT,
                     "heap close mixed exact TLS cache setup");
        if (run.lockless_remote_supported) {
            queue_node_count = bench_tagged_freelist_node_count(
                &env.heap->core.slab_remote_class_lf[size_class].queue,
                class_queue_node_count);
            bench_expect(queue_node_count == class_queue_node_count,
                         "heap close mixed exact class queue setup");
        }

        for (size_class = 0u; size_class < UHM_SIZE_CLASS_COUNT;
             ++size_class) {
            env.heap->core.slab_empty_committed_cache_high_water[size_class] =
                uhm_slab_empty_committed_cache_state_make(
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER,
                    0u);
        }
        bench_check_heap(&env, "heap close mixed benchmark pre-close check");
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
                        "heap close mixed benchmark API close");
        end = bench_now_ns();
        latencies[sample] = end - begin;
        total_close_ns += latencies[sample];

#if UHMAS_ENABLE_DIAGNOSTICS
        if (run.phase_timing_available) {
            uint32_t phase;

            for (phase = 0u; phase < UHM_CLOSE_PHASE_COUNT; ++phase) {
                uint64_t calls = env.heap->core.close_phase_calls[phase];

                run.phases[phase].phase_calls += calls;
                if (calls != 0u) {
                    phase_wall_samples[phase][sample] =
                        env.heap->core.close_phase_wall_ns[phase];
                    phase_wall_sample_valid[phase][sample] = 1u;
                }
                if (env.heap->core.close_phase_cpu_work_valid[phase]) {
                    uint32_t is_cycles =
                        env.heap->core
                            .close_phase_cpu_work_is_cycles[phase];

                    if (phase_cpu_work_unit_set[phase]) {
                        bench_expect(
                            phase_cpu_work_is_cycles[phase] == is_cycles,
                            "heap close mixed stable phase CPU unit");
                    } else {
                        phase_cpu_work_is_cycles[phase] = is_cycles;
                        phase_cpu_work_unit_set[phase] = 1u;
                    }
                    phase_cpu_samples[phase][sample] =
                        env.heap->core.close_phase_cpu_work[phase];
                    phase_cpu_sample_valid[phase][sample] = 1u;
                }
            }
            run.arena_release_phase_calls +=
                env.heap->core
                    .close_phase_calls[UHM_CLOSE_PHASE_ARENA_RELEASE];
            bench_expect(
                env.heap->core.close_phase_calls
                        [UHM_CLOSE_PHASE_METADATA_EPOCH] == 1u &&
                    env.heap->core.close_phase_calls
                            [UHM_CLOSE_PHASE_LOCAL_REGISTRY_DRAIN] == 1u &&
                    env.heap->core.close_phase_calls
                            [UHM_CLOSE_PHASE_FALLBACK_MAINTENANCE] == 1u &&
                    env.heap->core.close_phase_calls
                            [UHM_CLOSE_PHASE_FINAL_CLASS_PUBLISH] == 1u &&
                    env.heap->core.close_phase_calls
                            [UHM_CLOSE_PHASE_REGISTRY_DETACH] == 1u &&
                    env.heap->core.close_phase_calls
                            [UHM_CLOSE_PHASE_TLS_KEY_DELETE] == 1u &&
                    env.heap->core.close_phase_calls
                            [UHM_CLOSE_PHASE_ARENA_RELEASE] ==
                        (run.arena_owned ? 1u : 0u),
                "heap close mixed exact phase execution");
        }
#endif

        bench_expect(atomic_load_explicit(&env.heap->lifecycle_state,
                                          UHM_ATOMIC_ACQUIRE) ==
                         UHM_CLOSED_MAGIC &&
                         env.heap->magic == UHM_CLOSED_MAGIC,
                     "heap close mixed closed lifecycle");
        bench_expect(env.heap->core.thread_ctx_head == 0 &&
                         env.heap->core.tls_cache_head == 0 &&
                         env.heap->core.tls_cache_active_count == 0u &&
                         env.heap->core.tls_cache_key_valid == 0u,
                     "heap close mixed registries detached");
        bench_expect(uhm_heap_active_ops_load(env.heap) == 0u,
                     "heap close mixed active ops drained");
        bench_expect(uhm_metadata_registered_readers_load(env.heap) == 0u,
                     "heap close mixed readers drained");
        bench_expect(bench_heap_remote_queues_empty(env.heap),
                     "heap close mixed queues drained");
        for (context_index = 0u;
             context_index < BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT;
             ++context_index) {
            if (contexts[context_index]->magic == 0u &&
                contexts[context_index]->owner == 0) {
                ++run.invalidated_contexts;
            }
        }
        for (worker = 0u;
             worker < BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT; ++worker) {
            if (atomic_load_explicit(&caches[worker]->lifecycle_state,
                                     UHM_ATOMIC_ACQUIRE) == 0u &&
                atomic_load_explicit(&caches[worker]->owner_ref,
                                     UHM_ATOMIC_ACQUIRE) == 0u &&
                caches[worker]->magic == 0u && caches[worker]->owner == 0) {
                ++invalidated_caches;
            }
        }
        bench_expect(invalidated_caches ==
                         BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT,
                     "heap close mixed invalidates every TLS cache");
        bench_expect(run.invalidated_contexts ==
                         (uint64_t)(sample + 1u) *
                             BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT,
                     "heap close mixed invalidates every context");
        if (run.vm_counters_available) {
            bench_expect(env.heap->core.vm_decommit_calls ==
                             vm_decommit_before,
                         "heap close mixed no decommit");
            bench_expect(env.heap->core.vm_release_calls ==
                             vm_release_before +
                                 (run.arena_owned ? 1u : 0u),
                         "heap close mixed arena release contract");
            bench_expect(env.heap->core.vm_release_failures +
                                 env.heap->core.vm_decommit_failures ==
                             vm_failures_before,
                         "heap close mixed no VM failure");
            bench_expect(
                env.heap->core.vm_lock_held_calls ==
                        vm_lock_held_before +
                            (run.arena_owned ? 1u : 0u) &&
                    env.heap->core.vm_release_lock_held_calls ==
                        vm_release_lock_held_before +
                            (run.arena_owned ? 1u : 0u),
                "heap close mixed terminal release ownership contract");
        }
        if (sample == 0u) {
            bench_heap_close_capture_safe_after(
                &env, &run.representative_before,
                &run.representative_after);
        }

        ++run.closed_heaps;
        run.total_contexts += BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT;
        run.total_workers += BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT;
        run.ready_caches += ready_caches;
        run.cached_slots += cached_slots;
        run.invalidated_caches += invalidated_caches;
        run.class_queue_nodes += queue_node_count;
        if (class_slab_count > 1u) {
            ++run.class_queue_multi_slab_closes;
        }
        if (!run.lockless_remote_supported) {
            run.class_queue_fallback_frees +=
                class_queue_node_count;
        }
        if (run.vm_counters_available) {
            run.vm_release_calls +=
                env.heap->core.vm_release_calls - vm_release_before;
            run.vm_decommit_calls +=
                env.heap->core.vm_decommit_calls - vm_decommit_before;
            run.vm_failures += env.heap->core.vm_release_failures +
                               env.heap->core.vm_decommit_failures -
                               vm_failures_before;
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

        atomic_store_explicit(&release, 1u, UHM_ATOMIC_RELEASE);
        for (worker = 0u;
             worker < BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT; ++worker) {
#if defined(_WIN32)
            bench_expect(WaitForSingleObject(threads[worker], INFINITE) ==
                             WAIT_OBJECT_0,
                         "heap close mixed worker wait");
            CloseHandle(threads[worker]);
#else
            bench_expect(pthread_join(threads[worker], 0) == 0,
                         "heap close mixed worker join");
#endif
            if (!args[worker].ok) {
                ++run.worker_failures;
            }
        }
        for (context_index = 0u;
             context_index < BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT;
             ++context_index) {
            free(ctx_raws[context_index]);
        }
        if (!run.arena_owned) {
            uhm_result caller_result;

            ++run.caller_arena_probe_calls;
            caller_result = env.owner.vm.protect(
                env.owner.vm.user, caller_arena, env.owner.vm.page_size,
                UHM_PROT_READ | UHM_PROT_WRITE);
            if (caller_result != UHM_OK) {
                ++run.caller_arena_probe_failures;
            }
            bench_expect_ok(caller_result,
                            "heap close mixed caller arena remains mapped");
            ++run.caller_release_calls;
            caller_result = env.owner.vm.release(
                env.owner.vm.user, caller_arena, arena_size);
            if (caller_result != UHM_OK) {
                ++run.caller_release_failures;
            }
            bench_expect_ok(caller_result,
                            "heap close mixed caller arena release");
        }
        bench_heap_close_release_wrapper_storage(&env);
    }

    run.result = bench_result_make(result_name, 0u, total_close_ns,
                                   BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT);
    bench_result_set_latency(&run.result, latencies,
                             BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT);
    bench_result_set_latency_units(
        &run.result,
        BENCH_HEAP_CLOSE_MIXED_CONTEXT_COUNT +
            BENCH_HEAP_CLOSE_MIXED_WORKER_COUNT +
            class_queue_node_count,
        0u);
#if UHMAS_ENABLE_DIAGNOSTICS
    if (run.phase_breakdown_requested && run.phase_timing_available) {
        uint32_t phase;

        for (phase = 0u; phase < UHM_CLOSE_PHASE_COUNT; ++phase) {
            bench_heap_close_phase_run *phase_run = &run.phases[phase];
            uint64_t wall_values[BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT] = {0};
            uint64_t cpu_values[BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT] = {0};
            uint64_t wall_total = 0u;
            uint64_t cpu_total = 0u;
            size_t wall_count = 0u;
            size_t cpu_count = 0u;
            size_t index;

            for (index = 0u;
                 index < BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT; ++index) {
                if (phase_wall_sample_valid[phase][index]) {
                    wall_values[wall_count++] =
                        phase_wall_samples[phase][index];
                    wall_total += phase_wall_samples[phase][index];
                }
                if (phase_cpu_sample_valid[phase][index]) {
                    cpu_values[cpu_count++] =
                        phase_cpu_samples[phase][index];
                    cpu_total += phase_cpu_samples[phase][index];
                }
            }
            phase_run->wall_result =
                bench_result_make(0, 0u, wall_total, wall_count);
            if (wall_count != 0u) {
                bench_result_set_latency(&phase_run->wall_result,
                                         wall_values, wall_count);
            }
            phase_run->cpu_sample_count = cpu_count;
            if (cpu_count != 0u) {
                qsort(cpu_values, cpu_count, sizeof(cpu_values[0]),
                      bench_u64_compare);
                phase_run->cpu_min_work = cpu_values[0];
                phase_run->cpu_p50_work = bench_latency_percentile(
                    cpu_values, cpu_count, 50u);
                phase_run->cpu_p95_work = bench_latency_percentile(
                    cpu_values, cpu_count, 95u);
                phase_run->cpu_p99_work = bench_latency_percentile(
                    cpu_values, cpu_count, 99u);
                phase_run->cpu_max_work = cpu_values[cpu_count - 1u];
                phase_run->cpu_work_is_cycles =
                    phase_cpu_work_is_cycles[phase];
                phase_run->cpu_work_valid =
                    cpu_count == wall_count ? 1u : 0u;
                phase_run->wall_result.cpu_work = cpu_total;
                phase_run->wall_result.cpu_work_valid =
                    phase_run->cpu_work_valid;
                phase_run->wall_result.cpu_work_is_cycles =
                    phase_run->cpu_work_is_cycles;
            }
        }
    }
#endif
    return run;
}

static const char *bench_heap_close_phase_label(uint32_t phase) {
    static const char *const labels[UHM_CLOSE_PHASE_COUNT] = {
        "metadata epoch",
        "local registry drain",
        "fallback maintenance",
        "final class publish",
        "registry detach",
        "TLS key delete",
        "arena release",
    };

    return phase < UHM_CLOSE_PHASE_COUNT ? labels[phase] : "unknown";
}

static void bench_heap_close_mixed_print_phases(
    const bench_heap_close_mixed_run *run) {
    uint32_t phase;

    if (!run || !run->phase_breakdown_requested ||
        !run->phase_timing_available || !run->phase_scope) {
        return;
    }
    for (phase = 0u; phase < UHM_CLOSE_PHASE_COUNT; ++phase) {
        const bench_heap_close_phase_run *phase_run = &run->phases[phase];
        const char *label = bench_heap_close_phase_label(phase);
        bench_result wall_result;
        char result_name[128];

        bench_expect(phase_run->phase_calls ==
                         BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT,
                     "heap close phase exact call count");
        bench_expect(phase_run->wall_result.latency_sample_count ==
                         BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT &&
                         phase_run->cpu_sample_count ==
                             BENCH_HEAP_CLOSE_MIXED_SAMPLE_COUNT &&
                         phase_run->cpu_work_valid,
                     "heap close phase exact wall and CPU samples");
        snprintf(result_name, sizeof(result_name), "heap close %s phase %s",
                 run->phase_scope, label);
        wall_result = phase_run->wall_result;
        wall_result.name = result_name;
        bench_print(&wall_result);
        printf("metadata heap-close phase %s %s: "
               "phase_index=%u timing_available=1 phase_calls=%llu "
               "wall_samples=%llu cpu_samples=%llu cpu_work_valid=%u "
               "cpu_work_is_cycles=%u cpu_min_work=%llu "
               "cpu_p50_work=%llu cpu_p95_work=%llu cpu_p99_work=%llu "
               "cpu_max_work=%llu\n",
               run->phase_scope, label, phase,
               (unsigned long long)phase_run->phase_calls,
               (unsigned long long)
                   phase_run->wall_result.latency_sample_count,
               (unsigned long long)phase_run->cpu_sample_count,
               phase_run->cpu_work_valid,
               phase_run->cpu_work_is_cycles,
               (unsigned long long)phase_run->cpu_min_work,
               (unsigned long long)phase_run->cpu_p50_work,
               (unsigned long long)phase_run->cpu_p95_work,
               (unsigned long long)phase_run->cpu_p99_work,
               (unsigned long long)phase_run->cpu_max_work);
    }
}

static void bench_heap_close_mixed_print_run(
    const bench_heap_close_mixed_run *run, int diagnostics_enabled) {
    bench_expect(run != 0, "heap close mixed print run");
    bench_print(&run->result);
    if (diagnostics_enabled) {
        bench_diag_print_delta(run->result.name, &run->representative_before,
                               &run->representative_after);
    }
    bench_heap_close_mixed_print_phases(run);
    printf("metadata %s: "
           "latency_samples=%llu representative_close_count=1 "
           "contexts_per_close=%u tls_workers_per_close=%u "
           "tls_registry_slots_per_close=%u "
           "class_queue_nodes_per_close=%u lockless_remote_supported=%u "
           "vm_counters_available=%u arena_owned=%u closed_heaps=%llu "
           "total_contexts=%llu invalidated_contexts=%llu "
           "total_workers=%llu ready_caches=%llu cached_slots=%llu "
           "invalidated_caches=%llu class_queue_nodes=%llu "
           "class_queue_fallback_frees=%llu "
           "class_queue_multi_slab_closes=%llu worker_failures=%llu "
           "vm_release_calls=%llu vm_decommit_calls=%llu vm_failures=%llu "
           "vm_lock_held_calls=%llu vm_release_lock_held_calls=%llu "
           "active_ops_after=%llu registered_readers_after=%llu "
           "registry_leaks=%llu queue_leaks=%llu "
           "caller_arena_probe_calls=%llu "
           "caller_arena_probe_failures=%llu caller_release_calls=%llu "
           "caller_release_failures=%llu phase_count=%u "
           "phase_breakdown_requested=%u phase_timing_available=%u "
           "arena_release_phase_calls=%llu\n",
           run->metadata_name,
           (unsigned long long)run->result.latency_sample_count,
           (unsigned)run->contexts_per_close,
           (unsigned)run->workers_per_close,
           (unsigned)run->workers_per_close,
           (unsigned)run->class_queue_nodes_per_close,
           run->lockless_remote_supported, run->vm_counters_available,
           run->arena_owned,
           (unsigned long long)run->closed_heaps,
           (unsigned long long)run->total_contexts,
           (unsigned long long)run->invalidated_contexts,
           (unsigned long long)run->total_workers,
           (unsigned long long)run->ready_caches,
           (unsigned long long)run->cached_slots,
           (unsigned long long)run->invalidated_caches,
           (unsigned long long)run->class_queue_nodes,
           (unsigned long long)run->class_queue_fallback_frees,
           (unsigned long long)run->class_queue_multi_slab_closes,
           (unsigned long long)run->worker_failures,
           (unsigned long long)run->vm_release_calls,
           (unsigned long long)run->vm_decommit_calls,
           (unsigned long long)run->vm_failures,
           (unsigned long long)run->vm_lock_held_calls,
           (unsigned long long)run->vm_release_lock_held_calls,
           (unsigned long long)run->active_ops_after,
           (unsigned long long)run->registered_readers_after,
           (unsigned long long)run->registry_leaks,
           (unsigned long long)run->queue_leaks,
           (unsigned long long)run->caller_arena_probe_calls,
           (unsigned long long)run->caller_arena_probe_failures,
           (unsigned long long)run->caller_release_calls,
           (unsigned long long)run->caller_release_failures,
           (unsigned)UHM_CLOSE_PHASE_COUNT,
           run->phase_breakdown_requested,
           run->phase_timing_available,
           (unsigned long long)run->arena_release_phase_calls);
}
