enum {
    BENCH_HEAP_CLOSE_TLS_SAMPLE_COUNT = 9u,
    BENCH_HEAP_CLOSE_TLS_MAX_WORKER_COUNT = 9u
};

typedef struct bench_heap_close_tls_worker_args {
    const uhm_api *api;
    uhm_heap *heap;
    _Atomic(uint32_t) *ready_count;
    _Atomic(uint32_t) *release;
    int ok;
} bench_heap_close_tls_worker_args;

typedef struct bench_heap_close_tls_run {
    bench_result result;
    bench_diag_snapshot representative_before;
    bench_diag_snapshot representative_after;
    uint64_t closed_heaps;
    uint64_t total_workers;
    uint64_t ready_caches;
    uint64_t cached_slots;
    uint64_t invalidated_caches;
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
    size_t workers_per_close;
    const char *metadata_name;
    uint32_t vm_counters_available;
} bench_heap_close_tls_run;

static int bench_heap_close_tls_worker_run(
    bench_heap_close_tls_worker_args *args) {
    unsigned char *ptr;

    args->ok = 0;
    ptr = (unsigned char *)args->api->malloc(args->heap, 64u);
    if (ptr) {
        memset(ptr, 0x6du, 64u);
        args->api->free(args->heap, ptr);
        args->ok = 1;
    }
    (void)atomic_fetch_add_explicit(args->ready_count, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    while (atomic_load_explicit(args->release, UHM_ATOMIC_ACQUIRE) == 0u) {
        bench_thread_yield();
    }
    return args->ok;
}

#if defined(_WIN32)
static DWORD WINAPI bench_heap_close_tls_worker_entry(LPVOID user) {
    return bench_heap_close_tls_worker_run(
               (bench_heap_close_tls_worker_args *)user)
               ? 0u
               : 1u;
}
#else
static void *bench_heap_close_tls_worker_entry(void *user) {
    (void)bench_heap_close_tls_worker_run(
        (bench_heap_close_tls_worker_args *)user);
    return 0;
}
#endif

static size_t bench_heap_close_tls_snapshot_caches(
    uhm_heap *heap, uhm_tls_cache **caches, size_t cache_capacity,
    size_t *cached_slots) {
    uhm_tls_cache *cache;
    size_t count = 0u;
    size_t slots = 0u;

    bench_expect(heap != 0 && caches != 0 && cached_slots != 0,
                 "heap close TLS cache snapshot arguments");
    for (cache = heap->core.tls_cache_head; cache;
         cache = cache->next_active) {
        bench_expect(count < cache_capacity,
                     "heap close TLS cache snapshot bounded");
        bench_expect(cache->magic == UHM_TLS_CACHE_MAGIC &&
                         atomic_load_explicit(&cache->lifecycle_state,
                                              UHM_ATOMIC_ACQUIRE) ==
                             UHM_TLS_CACHE_MAGIC &&
                         cache->owner == heap &&
                         atomic_load_explicit(&cache->owner_ref,
                                              UHM_ATOMIC_ACQUIRE) ==
                             (uintptr_t)heap,
                     "heap close TLS cache snapshot live ownership");
        bench_expect(cache->local_cache_count == 1u,
                     "heap close TLS worker retains one cached slot");
        caches[count++] = cache;
        slots += cache->local_cache_count;
    }
    *cached_slots = slots;
    return count;
}

static bench_heap_close_tls_run bench_heap_close_owned_tls_workers(
    size_t arena_size, size_t worker_count, const char *result_name,
    const char *metadata_name) {
    bench_heap_close_tls_run run;
    uint64_t latencies[BENCH_HEAP_CLOSE_TLS_SAMPLE_COUNT] = {0};
    uint64_t total_close_ns = 0u;
    size_t sample;

    memset(&run, 0, sizeof(run));
    bench_expect(worker_count != 0u &&
                     worker_count <= BENCH_HEAP_CLOSE_TLS_MAX_WORKER_COUNT &&
                     result_name != 0 && metadata_name != 0,
                 "heap close TLS benchmark configuration");
    run.workers_per_close = worker_count;
    run.metadata_name = metadata_name;
    run.vm_counters_available = UHMAS_ENABLE_DIAGNOSTICS ? 1u : 0u;

    for (sample = 0u; sample < BENCH_HEAP_CLOSE_TLS_SAMPLE_COUNT; ++sample) {
        bench_heap_env env;
        bench_heap_close_tls_worker_args
            args[BENCH_HEAP_CLOSE_TLS_MAX_WORKER_COUNT];
        uhm_tls_cache *caches[BENCH_HEAP_CLOSE_TLS_MAX_WORKER_COUNT] = {0};
        _Atomic(uint32_t) ready_count;
        _Atomic(uint32_t) release;
        size_t ready_caches;
        size_t cached_slots;
        size_t invalidated_caches = 0u;
        size_t size_class;
        size_t worker;
        uint64_t vm_release_before;
        uint64_t vm_decommit_before;
        uint64_t vm_failures_before;
        uint64_t vm_lock_held_before;
        uint64_t vm_release_lock_held_before;
        uint64_t begin;
        uint64_t end;
#if defined(_WIN32)
        HANDLE threads[BENCH_HEAP_CLOSE_TLS_MAX_WORKER_COUNT] = {0};
#else
        pthread_t threads[BENCH_HEAP_CLOSE_TLS_MAX_WORKER_COUNT];
#endif

        bench_heap_open_tls_slots(&env, arena_size, worker_count);
        bench_expect(env.heap->core.owns_arena != 0u,
                     "heap close TLS benchmark owns arena");
        bench_expect(env.heap->core.tls_cache_slot_count ==
                         worker_count &&
                         env.heap->core.tls_cache_key_valid != 0u,
                     "heap close TLS benchmark registry configured");

        memset(args, 0, sizeof(args));
        atomic_init(&ready_count, 0u);
        atomic_init(&release, 0u);
        for (worker = 0u; worker < worker_count;
             ++worker) {
            args[worker].api = env.api;
            args[worker].heap = env.heap;
            args[worker].ready_count = &ready_count;
            args[worker].release = &release;
#if defined(_WIN32)
            threads[worker] = CreateThread(
                0, 0, bench_heap_close_tls_worker_entry, &args[worker], 0, 0);
            bench_expect(threads[worker] != 0,
                         "heap close TLS benchmark thread create");
#else
            bench_expect(pthread_create(&threads[worker], 0,
                                        bench_heap_close_tls_worker_entry,
                                        &args[worker]) == 0,
                         "heap close TLS benchmark thread create");
#endif
        }
        while (atomic_load_explicit(&ready_count, UHM_ATOMIC_ACQUIRE) !=
               worker_count) {
            bench_thread_yield();
        }
        for (worker = 0u; worker < worker_count;
             ++worker) {
            bench_expect(args[worker].ok,
                         "heap close TLS benchmark worker setup");
        }
        bench_expect(env.heap->core.tls_cache_active_count ==
                         worker_count,
                     "heap close TLS benchmark all caches active");
        ready_caches = bench_heap_close_tls_snapshot_caches(
            env.heap, caches, worker_count, &cached_slots);
        bench_expect(ready_caches == worker_count &&
                         cached_slots == worker_count,
                     "heap close TLS benchmark exact cache setup");

        for (size_class = 0u; size_class < UHM_SIZE_CLASS_COUNT;
             ++size_class) {
            env.heap->core.slab_empty_committed_cache_high_water[size_class] =
                uhm_slab_empty_committed_cache_state_make(
                    UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER,
                    0u);
        }
        bench_check_heap(&env, "heap close TLS benchmark pre-close check");
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
                        "heap close TLS benchmark API close");
        end = bench_now_ns();
        latencies[sample] = end - begin;
        total_close_ns += latencies[sample];

        bench_expect(atomic_load_explicit(&env.heap->lifecycle_state,
                                          UHM_ATOMIC_ACQUIRE) ==
                             UHM_CLOSED_MAGIC &&
                         env.heap->magic == UHM_CLOSED_MAGIC,
                     "heap close TLS benchmark closed lifecycle");
        bench_expect(env.heap->core.tls_cache_head == 0 &&
                         env.heap->core.tls_cache_active_count == 0u &&
                         env.heap->core.thread_ctx_head == 0 &&
                         env.heap->core.tls_cache_key_valid == 0u,
                     "heap close TLS benchmark registry detached");
        bench_expect(uhm_heap_active_ops_load(env.heap) == 0u,
                     "heap close TLS benchmark active ops drained");
        bench_expect(uhm_metadata_registered_readers_load(env.heap) == 0u,
                     "heap close TLS benchmark readers drained");
        bench_expect(bench_heap_remote_queues_empty(env.heap),
                     "heap close TLS benchmark queues drained");
        for (worker = 0u; worker < worker_count;
             ++worker) {
            if (atomic_load_explicit(&caches[worker]->lifecycle_state,
                                     UHM_ATOMIC_ACQUIRE) == 0u &&
                atomic_load_explicit(&caches[worker]->owner_ref,
                                     UHM_ATOMIC_ACQUIRE) == 0u &&
                caches[worker]->magic == 0u &&
                caches[worker]->owner == 0) {
                ++invalidated_caches;
            }
        }
        bench_expect(invalidated_caches ==
                         worker_count,
                     "heap close TLS benchmark invalidates every cache");
        if (run.vm_counters_available) {
            bench_expect(
                env.heap->core.vm_decommit_calls == vm_decommit_before,
                "heap close TLS benchmark no decommit");
            bench_expect(env.heap->core.vm_release_calls ==
                             vm_release_before + 1u,
                         "heap close TLS benchmark one arena release");
            bench_expect(env.heap->core.vm_release_failures +
                                 env.heap->core.vm_decommit_failures ==
                             vm_failures_before,
                         "heap close TLS benchmark no VM failure");
            bench_expect(env.heap->core.vm_lock_held_calls ==
                             vm_lock_held_before + 1u &&
                             env.heap->core.vm_release_lock_held_calls ==
                                 vm_release_lock_held_before + 1u,
                         "heap close TLS terminal release under close lock");
        }
        if (sample == 0u) {
            bench_heap_close_capture_safe_after(
                &env, &run.representative_before,
                &run.representative_after);
        }

        ++run.closed_heaps;
        run.total_workers += worker_count;
        run.ready_caches += ready_caches;
        run.cached_slots += cached_slots;
        run.invalidated_caches += invalidated_caches;
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
        if (env.heap->core.tls_cache_head != 0 ||
            env.heap->core.tls_cache_active_count != 0u ||
            env.heap->core.thread_ctx_head != 0) {
            ++run.registry_leaks;
        }
        if (!bench_heap_remote_queues_empty(env.heap)) {
            ++run.queue_leaks;
        }

        atomic_store_explicit(&release, 1u, UHM_ATOMIC_RELEASE);
        for (worker = 0u; worker < worker_count;
             ++worker) {
#if defined(_WIN32)
            bench_expect(WaitForSingleObject(threads[worker], INFINITE) ==
                             WAIT_OBJECT_0,
                         "heap close TLS benchmark thread wait");
            CloseHandle(threads[worker]);
#else
            bench_expect(pthread_join(threads[worker], 0) == 0,
                         "heap close TLS benchmark thread join");
#endif
            if (!args[worker].ok) {
                ++run.worker_failures;
            }
        }
        bench_heap_close_release_wrapper_storage(&env);
    }

    run.result = bench_result_make(result_name, 0u, total_close_ns,
                                   BENCH_HEAP_CLOSE_TLS_SAMPLE_COUNT);
    bench_result_set_latency(&run.result, latencies,
                             BENCH_HEAP_CLOSE_TLS_SAMPLE_COUNT);
    bench_result_set_latency_units(&run.result,
                                   worker_count, 0u);
    return run;
}

static void bench_heap_close_tls_print_run(
    const bench_heap_close_tls_run *run, int diagnostics_enabled) {
    bench_expect(run != 0, "heap close TLS print run");
    bench_print(&run->result);
    if (diagnostics_enabled) {
        bench_diag_print_delta(
            run->result.name, &run->representative_before,
            &run->representative_after);
    }
    printf("metadata %s: "
           "latency_samples=%llu representative_close_count=1 "
           "tls_workers_per_close=%u tls_registry_slots_per_close=%u "
           "vm_counters_available=%u closed_heaps=%llu "
           "total_workers=%llu ready_caches=%llu cached_slots=%llu "
           "invalidated_caches=%llu worker_failures=%llu "
           "vm_release_calls=%llu vm_decommit_calls=%llu vm_failures=%llu "
           "vm_lock_held_calls=%llu vm_release_lock_held_calls=%llu "
           "active_ops_after=%llu registered_readers_after=%llu "
           "registry_leaks=%llu queue_leaks=%llu\n",
           run->metadata_name,
           (unsigned long long)run->result.latency_sample_count,
           (unsigned)run->workers_per_close,
           (unsigned)run->workers_per_close,
           run->vm_counters_available,
           (unsigned long long)run->closed_heaps,
           (unsigned long long)run->total_workers,
           (unsigned long long)run->ready_caches,
           (unsigned long long)run->cached_slots,
           (unsigned long long)run->invalidated_caches,
           (unsigned long long)run->worker_failures,
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
