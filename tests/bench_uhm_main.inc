static bench_result bench_release_pressure(bench_heap_env *env,
                                           size_t iterations,
                                           size_t page_size) {
    enum { slot_count = 96 };
    void *mallocs[slot_count];
    void *maps[slot_count];
    size_t map_sizes[slot_count];
    size_t i;
    uint64_t begin;
    uint64_t end;

    memset(mallocs, 0, sizeof(mallocs));
    memset(maps, 0, sizeof(maps));
    memset(map_sizes, 0, sizeof(map_sizes));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 13u) % slot_count;
        if (mallocs[slot]) {
            env->api->free(env->heap, mallocs[slot]);
            mallocs[slot] = 0;
        }
        if (maps[slot]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[slot],
                                             map_sizes[slot]),
                            "release pressure munmap");
            maps[slot] = 0;
            map_sizes[slot] = 0;
        }
        mallocs[slot] = env->api->malloc(env->heap,
                                         64u + ((i * 37u) % 8192u));
        bench_expect(mallocs[slot] != 0, "release pressure malloc");
        memset(mallocs[slot], (int)i, 64u);
        if ((i % 3u) == 0) {
            map_sizes[slot] = page_size * (1u + (i % 4u));
            bench_expect_ok(env->api->mmap(env->heap, &maps[slot],
                                           map_sizes[slot],
                                           UHM_PROT_READ | UHM_PROT_WRITE,
                                           UHM_MAP_PRIVATE),
                            "release pressure mmap");
            if ((i % 5u) == 0) {
                bench_expect_ok(env->api->mprotect(env->heap, maps[slot],
                                                   page_size, UHM_PROT_READ),
                                "release pressure mprotect");
            }
        }
    }
    for (i = 0; i < slot_count; ++i) {
        if (maps[i]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[i], map_sizes[i]),
                            "release pressure cleanup map");
        }
        env->api->free(env->heap, mallocs[i]);
    }
    end = bench_now_ns();
    return bench_result_make("uhm release mixed long pressure", begin, end,
                             iterations);
}

static bench_result bench_release_pressure_with_latency(bench_heap_env *env,
                                                        size_t iterations,
                                                        size_t page_size,
                                                        const char *result_name) {
    enum { slot_count = 96 };
    void *mallocs[slot_count];
    void *maps[slot_count];
    size_t map_sizes[slot_count];
    uint64_t *latencies;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_result result;

    latencies = (uint64_t *)calloc(iterations, sizeof(latencies[0]));
    bench_expect(latencies != 0, "release pressure full latency samples");
    memset(mallocs, 0, sizeof(mallocs));
    memset(maps, 0, sizeof(maps));
    memset(map_sizes, 0, sizeof(map_sizes));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 13u) % slot_count;
        uint64_t op_begin = bench_now_ns();
        if (mallocs[slot]) {
            env->api->free(env->heap, mallocs[slot]);
            mallocs[slot] = 0;
        }
        if (maps[slot]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[slot],
                                             map_sizes[slot]),
                            "release pressure latency munmap");
            maps[slot] = 0;
            map_sizes[slot] = 0;
        }
        mallocs[slot] = env->api->malloc(env->heap,
                                         64u + ((i * 37u) % 8192u));
        bench_expect(mallocs[slot] != 0, "release pressure latency malloc");
        memset(mallocs[slot], (int)i, 64u);
        if ((i % 3u) == 0) {
            map_sizes[slot] = page_size * (1u + (i % 4u));
            bench_expect_ok(env->api->mmap(env->heap, &maps[slot],
                                           map_sizes[slot],
                                           UHM_PROT_READ | UHM_PROT_WRITE,
                                           UHM_MAP_PRIVATE),
                            "release pressure latency mmap");
            if ((i % 5u) == 0) {
                bench_expect_ok(env->api->mprotect(env->heap, maps[slot],
                                                   page_size, UHM_PROT_READ),
                                "release pressure latency mprotect");
            }
        }
        latencies[i] = bench_now_ns() - op_begin;
    }
    for (i = 0; i < slot_count; ++i) {
        if (maps[i]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[i], map_sizes[i]),
                            "release pressure latency cleanup map");
        }
        env->api->free(env->heap, mallocs[i]);
    }
    end = bench_now_ns();
    result = bench_result_make(result_name, begin, end, iterations);
    bench_result_set_latency(&result, latencies, iterations);
    free(latencies);
    return result;
}

typedef struct bench_spinlock_metric {
    uint64_t wall_ns_per_op;
    uint64_t cpu_work_per_op;
    uint64_t p99_ns_per_op;
    uint64_t worker_cpu_ns_per_op;
    uint64_t worker_wait_ns_per_op;
} bench_spinlock_metric;

enum {
    BENCH_SPINLOCK_WORKLOAD_SMALL = 0,
    BENCH_SPINLOCK_WORKLOAD_REMOTE,
    BENCH_SPINLOCK_WORKLOAD_PAGE_MAP,
    BENCH_SPINLOCK_WORKLOAD_MIXED,
    BENCH_SPINLOCK_WORKLOAD_COUNT
};

static void bench_spinlock_metric_capture(bench_spinlock_metric *metric,
                                          const bench_result *result) {
    bench_expect(metric != 0 && result != 0 && result->operations != 0,
                 "spinlock metric capture arguments");
    metric->wall_ns_per_op =
        (result->wall_time_ns + result->operations / 2u) /
        result->operations;
    metric->cpu_work_per_op =
        (result->cpu_work + result->operations / 2u) /
        result->operations;
    metric->p99_ns_per_op = result->latency_p99_ns;
    metric->worker_cpu_ns_per_op =
        (result->worker_cpu_ns + result->operations / 2u) /
        result->operations;
    metric->worker_wait_ns_per_op =
        (result->worker_wait_ns + result->operations / 2u) /
        result->operations;
}

static void bench_spinlock_threshold_case(size_t arena_size,
                                          size_t thread_iters,
                                          size_t remote_iters,
                                          size_t page_iters,
                                          size_t mixed_iters,
                                          size_t page_size,
                                          uint32_t yield_after,
                                          const char *label,
                                          uint32_t heap_flags,
                                          int diagnostics,
                                          long double cycles_per_ns,
                                          bench_spinlock_metric *metrics) {
    bench_heap_env env;
    bench_diag_snapshot diag_before;
    bench_result result;
    uint64_t cpu_begin_work;
    uint64_t cpu_end_work;
    uint64_t process_cpu_begin_ns;
    uint64_t process_cpu_end_ns;
    uint64_t thread_cpu_begin_work;
    uint64_t thread_cpu_end_work;
    uint32_t cpu_work_is_cycles;
    uint32_t thread_cpu_work_is_cycles;
    uint32_t thread_cpu_begin_valid;
    uint32_t thread_cpu_end_valid;
    char name[96];

    bench_heap_open_no_tls_config_flags_ex(&env, arena_size, heap_flags,
                                           yield_after);
    if (diagnostics) {
        uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
    }
    bench_expect(env.heap->core.spinlock_mode ==
                     ((heap_flags & UHM_CONFIG_SPINLOCK_BURN) != 0u
                          ? UHM_SPINLOCK_MODE_BURN
                          : UHM_SPINLOCK_MODE_YIELD),
                 "spinlock threshold benchmark configured mode");
    bench_expect(env.heap->core.spinlock_yield_after_spins ==
                     (yield_after != 0u ? yield_after
                                        : UHM_SPINLOCK_DEFAULT_YIELD_AFTER),
                 "spinlock threshold benchmark configured threshold");

    bench_diag_capture(&env, &diag_before);
    cpu_begin_work = bench_process_cpu_work_now(&cpu_work_is_cycles);
    result = bench_multithread_small_profiled(
        &env, thread_iters, 0, 4u, 1,
        "spinlock profiled multithread small", cycles_per_ns);
    cpu_end_work = bench_process_cpu_work_now(&cpu_work_is_cycles);
    bench_result_set_cpu_work(
        &result, cpu_begin_work, cpu_end_work, cpu_work_is_cycles);
    bench_spinlock_metric_capture(
        &metrics[BENCH_SPINLOCK_WORKLOAD_SMALL], &result);
    snprintf(name, sizeof(name),
             "spinlock yield=%s multithread small", label);
    result.name = name;
    bench_run_uhm_case_deferred_check(&env, &result, &diag_before,
                                      diagnostics);

    bench_diag_capture(&env, &diag_before);
    cpu_begin_work = bench_process_cpu_work_now(&cpu_work_is_cycles);
    result = bench_false_remote_handoff_profiled(
        &env, remote_iters, 4u, 1, "spinlock profiled remote handoff",
        cycles_per_ns);
    cpu_end_work = bench_process_cpu_work_now(&cpu_work_is_cycles);
    bench_result_set_cpu_work(
        &result, cpu_begin_work, cpu_end_work, cpu_work_is_cycles);
    bench_spinlock_metric_capture(
        &metrics[BENCH_SPINLOCK_WORKLOAD_REMOTE], &result);
    snprintf(name, sizeof(name),
             "spinlock yield=%s remote handoff", label);
    result.name = name;
    bench_run_uhm_case_deferred_check(&env, &result, &diag_before,
                                      diagnostics);

    bench_diag_capture(&env, &diag_before);
    cpu_begin_work = bench_process_cpu_work_now(&cpu_work_is_cycles);
    result = bench_false_page_map_hotspot_profiled(&env, page_iters,
                                                   page_size,
                                                   cycles_per_ns);
    cpu_end_work = bench_process_cpu_work_now(&cpu_work_is_cycles);
    bench_result_set_cpu_work(
        &result, cpu_begin_work, cpu_end_work, cpu_work_is_cycles);
    bench_spinlock_metric_capture(
        &metrics[BENCH_SPINLOCK_WORKLOAD_PAGE_MAP], &result);
    snprintf(name, sizeof(name),
             "spinlock yield=%s page-map hotspot", label);
    result.name = name;
    bench_run_uhm_case_deferred_check(&env, &result, &diag_before,
                                      diagnostics);

    bench_diag_capture(&env, &diag_before);
    cpu_begin_work = bench_process_cpu_work_now(&cpu_work_is_cycles);
    process_cpu_begin_ns = bench_process_cpu_now_ns();
    thread_cpu_begin_work = bench_thread_cpu_work_now(
        &thread_cpu_work_is_cycles, &thread_cpu_begin_valid);
    result = bench_release_pressure_with_latency(
        &env, mixed_iters, page_size, "spinlock profiled mixed release");
    thread_cpu_end_work = bench_thread_cpu_work_now(
        &thread_cpu_work_is_cycles, &thread_cpu_end_valid);
    cpu_end_work = bench_process_cpu_work_now(&cpu_work_is_cycles);
    process_cpu_end_ns = bench_process_cpu_now_ns();
    bench_result_set_cpu_work(
        &result, cpu_begin_work, cpu_end_work, cpu_work_is_cycles);
    bench_result_set_single_thread_runtime(
        &result, thread_cpu_begin_work, thread_cpu_end_work,
        thread_cpu_work_is_cycles,
        thread_cpu_begin_valid && thread_cpu_end_valid);
    bench_result_rescale_worker_cpu(
        &result,
        process_cpu_end_ns >= process_cpu_begin_ns
            ? process_cpu_end_ns - process_cpu_begin_ns
            : 0u,
        result.cpu_work, result.cpu_work_is_cycles, cycles_per_ns);
    bench_spinlock_metric_capture(
        &metrics[BENCH_SPINLOCK_WORKLOAD_MIXED], &result);
    snprintf(name, sizeof(name),
             "spinlock yield=%s mixed release", label);
    result.name = name;
    bench_run_uhm_case_deferred_check(&env, &result, &diag_before,
                                      diagnostics);

    bench_heap_close(&env);
}

static void bench_spinlock_threshold_matrix(size_t arena_size,
                                            size_t thread_iters,
                                            size_t remote_iters,
                                            size_t page_iters,
                                            size_t mixed_iters,
                                            size_t page_size,
                                            int diagnostics) {
    static const struct {
        uint32_t yield_after;
        const char *label;
        uint32_t heap_flags;
    } cases[] = {
        {0u, "burn", UHM_CONFIG_SPINLOCK_BURN},
        {0u, "default", 0u},
        {1u, "1", UHM_CONFIG_SPINLOCK_YIELD},
        {16u, "16", UHM_CONFIG_SPINLOCK_YIELD},
        {256u, "256", UHM_CONFIG_SPINLOCK_YIELD},
        {1024u, "1024", UHM_CONFIG_SPINLOCK_YIELD},
    };
    static const char *workload_labels[BENCH_SPINLOCK_WORKLOAD_COUNT] = {
        "multithread-small",
        "remote-handoff",
        "page-map-hotspot",
        "mixed-release",
    };
    bench_spinlock_metric
        metrics[sizeof(cases) / sizeof(cases[0])]
               [BENCH_SPINLOCK_WORKLOAD_COUNT];
    const size_t default_index = 1u;
    uint64_t matrix_wall_begin_ns;
    uint64_t matrix_wall_end_ns;
    uint64_t matrix_cpu_begin_ns;
    uint64_t matrix_cpu_end_ns;
    uint64_t matrix_wall_ns;
    uint64_t matrix_cpu_ns;
    uint64_t capacity_denominator;
    uint64_t parallelism_ppm;
    uint64_t capacity_ppm;
    size_t logical_cpus;
    size_t case_count;
    size_t i;
    size_t run_slot;
    size_t sample_index;
    size_t rotation;
    int reverse_order;
    size_t workload;
    long double cycles_per_ns;

    memset(metrics, 0, sizeof(metrics));
    bench_expect(strcmp(cases[default_index].label, "default") == 0,
                 "spinlock threshold default index");
    logical_cpus = bench_logical_cpu_count();
    bench_print_scheduler_state();
    cycles_per_ns = bench_thread_cpu_cycles_per_ns();
    printf("metadata spinlock-cpu-cycle-calibration: cycles_per_ns_ppm=%llu\n",
           (unsigned long long)(cycles_per_ns > 0.0L
                                    ? cycles_per_ns * 1000000.0L + 0.5L
                                    : 0u));
    matrix_wall_begin_ns = bench_now_ns();
    matrix_cpu_begin_ns = bench_process_cpu_now_ns();

    sample_index = bench_sample_index();
    case_count = sizeof(cases) / sizeof(cases[0]);
    rotation = sample_index == 0u
                   ? 0u
                   : (sample_index - 1u) % case_count;
    reverse_order =
        sample_index != 0u &&
        (((sample_index - 1u) / case_count) % 2u) != 0u;
    printf("metadata spinlock-matrix-order: sample_index=%llu rotation=%llu "
           "reverse=%u\n",
           (unsigned long long)sample_index,
           (unsigned long long)rotation,
           reverse_order ? 1u : 0u);

    for (run_slot = 0; run_slot < case_count; ++run_slot) {
        i = reverse_order
                ? (rotation + case_count - run_slot) % case_count
                : (rotation + run_slot) % case_count;
        bench_spinlock_threshold_case(arena_size, thread_iters, remote_iters,
                                      page_iters, mixed_iters, page_size,
                                      cases[i].yield_after, cases[i].label,
                                      cases[i].heap_flags, diagnostics,
                                      cycles_per_ns,
                                      metrics[i]);
    }
    matrix_cpu_end_ns = bench_process_cpu_now_ns();
    matrix_wall_end_ns = bench_now_ns();
    matrix_wall_ns = matrix_wall_end_ns - matrix_wall_begin_ns;
    matrix_cpu_ns = matrix_cpu_end_ns >= matrix_cpu_begin_ns ?
                        matrix_cpu_end_ns - matrix_cpu_begin_ns : 0u;
    parallelism_ppm = bench_ratio_ppm(matrix_cpu_ns, matrix_wall_ns);
    capacity_denominator = matrix_wall_ns;
    if (logical_cpus != 0 &&
        capacity_denominator <= UINT64_MAX / logical_cpus) {
        capacity_denominator *= logical_cpus;
    } else {
        capacity_denominator = UINT64_MAX;
    }
    capacity_ppm = bench_ratio_ppm(matrix_cpu_ns, capacity_denominator);
    printf("metadata spinlock-matrix-runtime: wall_ns=%llu cpu_ns=%llu "
           "logical_cpus=%llu cpu_parallelism_ppm=%llu "
           "cpu_capacity_ppm=%llu\n",
           (unsigned long long)matrix_wall_ns,
           (unsigned long long)matrix_cpu_ns,
           (unsigned long long)logical_cpus,
           (unsigned long long)parallelism_ppm,
           (unsigned long long)capacity_ppm);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        for (workload = 0; workload < BENCH_SPINLOCK_WORKLOAD_COUNT;
             ++workload) {
            printf("metadata spinlock-relative yield=%s %s: "
                   "wall_ratio_ppm=%llu cpu_work_ratio_ppm=%llu "
                   "p99_ratio_ppm=%llu worker_cpu_ratio_ppm=%llu "
                   "worker_wait_ratio_ppm=%llu "
                   "wall_ns_per_op=%llu default_wall_ns_per_op=%llu "
                   "cpu_work_per_op=%llu default_cpu_work_per_op=%llu "
                   "p99_ns_per_op=%llu default_p99_ns_per_op=%llu "
                   "worker_cpu_ns_per_op=%llu "
                   "default_worker_cpu_ns_per_op=%llu "
                   "worker_wait_ns_per_op=%llu "
                   "default_worker_wait_ns_per_op=%llu\n",
                   cases[i].label, workload_labels[workload],
                   (unsigned long long)bench_ratio_ppm(
                       metrics[i][workload].wall_ns_per_op,
                       metrics[default_index][workload].wall_ns_per_op),
                   (unsigned long long)bench_ratio_ppm(
                       metrics[i][workload].cpu_work_per_op,
                       metrics[default_index][workload].cpu_work_per_op),
                   (unsigned long long)bench_ratio_ppm(
                       metrics[i][workload].p99_ns_per_op,
                       metrics[default_index][workload].p99_ns_per_op),
                   (unsigned long long)bench_ratio_ppm(
                       metrics[i][workload].worker_cpu_ns_per_op,
                       metrics[default_index][workload]
                           .worker_cpu_ns_per_op),
                   (unsigned long long)bench_ratio_ppm_zero_equal(
                       metrics[i][workload].worker_wait_ns_per_op,
                       metrics[default_index][workload]
                           .worker_wait_ns_per_op),
                   (unsigned long long)metrics[i][workload].wall_ns_per_op,
                   (unsigned long long)
                       metrics[default_index][workload].wall_ns_per_op,
                   (unsigned long long)
                       metrics[i][workload].cpu_work_per_op,
                   (unsigned long long)
                       metrics[default_index][workload].cpu_work_per_op,
                   (unsigned long long)metrics[i][workload].p99_ns_per_op,
                   (unsigned long long)
                       metrics[default_index][workload].p99_ns_per_op,
                   (unsigned long long)
                       metrics[i][workload].worker_cpu_ns_per_op,
                   (unsigned long long)
                       metrics[default_index][workload]
                           .worker_cpu_ns_per_op,
                   (unsigned long long)
                       metrics[i][workload].worker_wait_ns_per_op,
                   (unsigned long long)
                       metrics[default_index][workload]
                           .worker_wait_ns_per_op);
        }
    }
}

static void bench_small_fast_path_case(size_t arena_size,
                                       size_t small_iters,
                                       size_t thread_iters,
                                       const char *label,
                                       int enable_tls_cache,
                                       uint32_t heap_flags,
                                       int diagnostics) {
    bench_heap_env env;
    bench_diag_snapshot diag_before;
    bench_result result;
    char name[96];
    char check_message[128];

    if (enable_tls_cache) {
        bench_heap_open_config_flags_ex(&env, arena_size, 1, heap_flags, 0u);
    } else {
        bench_heap_open_no_tls_config_flags_ex(&env, arena_size, heap_flags,
                                               0u);
    }
    if (diagnostics) {
        uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
    }

    bench_diag_capture(&env, &diag_before);
    result = bench_uhm_small(&env, small_iters);
    snprintf(name, sizeof(name), "small fastpath %s small", label);
    result.name = name;
    snprintf(check_message, sizeof(check_message),
             "check after small fastpath %s small", label);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       check_message);

    bench_diag_capture(&env, &diag_before);
    result = bench_multithread_small(&env, thread_iters, 0);
    snprintf(name, sizeof(name), "small fastpath %s multithread", label);
    result.name = name;
    snprintf(check_message, sizeof(check_message),
             "check after small fastpath %s multithread", label);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       check_message);

    bench_heap_close(&env);
}

static void bench_small_fast_path_matrix(size_t arena_size,
                                         size_t small_iters,
                                         size_t thread_iters,
                                         size_t page_size,
                                         int diagnostics) {
    static const struct {
        const char *label;
        int enable_tls_cache;
        uint32_t heap_flags;
    } cases[] = {
        {"default", 0, 0u},
        {"tls", 1, 0u},
        {"lockless", 0, UHM_CONFIG_LOCKLESS_REMOTE_FREE},
        {"tls-lockless", 1, UHM_CONFIG_LOCKLESS_REMOTE_FREE},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        bench_small_fast_path_case(arena_size, small_iters, thread_iters,
                                   cases[i].label,
                                   cases[i].enable_tls_cache,
                                   cases[i].heap_flags, diagnostics);
    }
    {
        bench_heap_env env;
        bench_diag_snapshot diag_before;
        bench_result result;

        bench_heap_open_no_tls_config_flags_ex(&env, arena_size, 0u, 0u);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_thread_ctx_retained_hint_collision(
            &env, small_iters, page_size);
        bench_run_uhm_case(
            &env, &result, &diag_before, diagnostics,
            "check after ctx retained-hint collision benchmark");
        bench_heap_close(&env);
    }
}

static void bench_platform_named_comparisons(void) {
#if defined(_WIN32)
    printf("platform comparison: Windows HeapAlloc and VirtualAlloc covered\n");
#elif defined(__linux__)
    printf("platform comparison: glibc malloc and mmap covered\n");
#elif defined(__APPLE__)
    printf("platform comparison: libmalloc and mmap compatible path compiled\n");
#elif defined(__ANDROID__)
    printf("platform comparison: bionic malloc and mmap compatible path compiled\n");
#else
    printf("platform comparison: generic malloc and virtual memory path compiled\n");
#endif
}

static void bench_print_abi_capabilities(void) {
    uhm_abi_info abi;

    memset(&abi, 0, sizeof(abi));
    abi.size = sizeof(abi);
    bench_expect_ok(uhm_abi_query(&abi), "benchmark abi query");
    printf("abi capabilities: pointer_size=%u size_t_size=%u "
           "atomic_pointer_lock_free=%u atomic_u32_lock_free=%u "
           "atomic_u64_lock_free=%u tagged_freelist_supported=%u "
           "tagged_freelist_tag_bits=%u "
           "lockless_remote_primitives_supported=%u "
           "lockless_remote_config_supported=%u "
           "malloc_min_alignment=%u cacheline_size=%u call_abi=%u\n",
           abi.pointer_size, abi.size_t_size, abi.atomic_pointer_lock_free,
           abi.atomic_u32_lock_free, abi.atomic_u64_lock_free,
           abi.tagged_freelist_supported, abi.tagged_freelist_tag_bits,
           abi.lockless_remote_primitives_supported,
           abi.lockless_remote_config_supported, abi.malloc_min_alignment,
           abi.cacheline_size, abi.call_abi);
}

static size_t bench_model_align_up(size_t value, size_t alignment) {
    size_t mask;

    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
        return 0u;
    }
    mask = alignment - 1u;
    if (value > (size_t)-1 - mask) {
        return 0u;
    }
    return (value + mask) & ~mask;
}

static size_t bench_model_percent(size_t numerator, size_t denominator) {
    if (denominator == 0u) {
        return 0u;
    }
    return (size_t)(((uint64_t)numerator * UINT64_C(100)) /
                    (uint64_t)denominator);
}

static void bench_print_slab_slot_model(const char *model_name,
                                        size_t request,
                                        size_t page_size,
                                        size_t current_effective,
                                        size_t usable_payload,
                                        size_t inline_header,
                                        size_t side_bytes,
                                        size_t slot_stride,
                                        unsigned metadata_in_payload,
                                        unsigned cacheline_padded) {
    size_t effective_bytes = slot_stride + side_bytes;
    size_t slots_per_page = effective_bytes != 0u
                                ? page_size / effective_bytes
                                : 0u;
    size_t overhead = effective_bytes > request ? effective_bytes - request
                                                : 0u;
    size_t bytes_saved =
        current_effective > effective_bytes ? current_effective - effective_bytes
                                            : 0u;
    size_t bytes_extra =
        effective_bytes > current_effective ? effective_bytes - current_effective
                                            : 0u;
    size_t current_slots =
        current_effective != 0u ? page_size / current_effective : 0u;
    size_t slot_gain =
        slots_per_page > current_slots ? slots_per_page - current_slots : 0u;
    size_t slot_loss =
        current_slots > slots_per_page ? current_slots - slots_per_page : 0u;

    printf("metadata slab-slot-model-%s: request=%zu "
           "model_estimate=1 usable_payload_bytes=%zu inline_header=%zu "
           "side_bytes=%zu slot_stride=%zu effective_bytes=%zu "
           "slots_per_page=%zu overhead_bytes=%zu overhead_percent=%zu "
           "request_density_percent=%zu usable_density_percent=%zu "
           "stride_mod_cacheline=%zu metadata_in_payload=%u "
           "cacheline_padded=%u bytes_saved_vs_current=%zu "
           "bytes_extra_vs_current=%zu space_saving_percent_vs_current=%zu "
           "space_extra_percent_vs_current=%zu slot_gain_vs_current=%zu "
           "slot_loss_vs_current=%zu slot_gain_percent_vs_current=%zu "
           "slot_loss_percent_vs_current=%zu\n",
           model_name, request, usable_payload, inline_header, side_bytes,
           slot_stride, effective_bytes, slots_per_page, overhead,
           bench_model_percent(overhead, request),
           bench_model_percent(request, effective_bytes),
           bench_model_percent(usable_payload, effective_bytes),
           slot_stride % UHM_CACHELINE_SIZE, metadata_in_payload,
           cacheline_padded, bytes_saved, bytes_extra,
           bench_model_percent(bytes_saved, current_effective),
           bench_model_percent(bytes_extra, current_effective), slot_gain,
           slot_loss, bench_model_percent(slot_gain, current_slots),
           bench_model_percent(slot_loss, current_slots));
}

static void bench_print_slab_slot_layout_models(size_t request,
                                                size_t page_size,
                                                const uhm_slab *slab,
                                                size_t current_header) {
    size_t payload = slab->slot_size;
    size_t current_stride = slab->slot_stride;
    size_t compact16_stride =
        bench_model_align_up(payload + 16u, UHM_MIN_ALIGNMENT);
    size_t compact32_stride =
        bench_model_align_up(payload + 32u, UHM_MIN_ALIGNMENT);
    size_t side16_stride = payload;
    size_t padded64_stride =
        bench_model_align_up(current_stride, UHM_CACHELINE_SIZE);

    bench_print_slab_slot_model("current", request, page_size, current_stride,
                                payload, current_header, 0u, current_stride,
                                1u, 0u);
    bench_print_slab_slot_model("compact16", request, page_size,
                                current_stride, payload, 16u, 0u,
                                compact16_stride, 1u, 0u);
    bench_print_slab_slot_model("compact32", request, page_size,
                                current_stride, payload, 32u, 0u,
                                compact32_stride, 1u, 0u);
    bench_print_slab_slot_model("side16", request, page_size, current_stride,
                                payload, 0u, 16u, side16_stride, 0u, 0u);
    bench_print_slab_slot_model("padded64", request, page_size,
                                current_stride, payload, current_header, 0u,
                                padded64_stride, 1u, 1u);
}

static void bench_print_metadata_footprint(bench_heap_env *env,
                                           size_t page_size) {
    static const size_t sample_requests[] = {
        16u, 32u, 64u, 128u, 256u, 512u,
    };
    void *ptrs[sizeof(sample_requests) / sizeof(sample_requests[0])];
    size_t i;
    size_t header = bench_slab_slot_header_size();
    size_t raw_directory_bytes = 0u;

#if UINTPTR_MAX == UINT32_MAX
    raw_directory_bytes = sizeof(env->heap->core.slab_remote_ready_raw);
#endif

    memset(ptrs, 0, sizeof(ptrs));
    printf("metadata footprint: pointer_size=%zu cacheline_size=%u "
           "page_size=%zu heap=%zu heap_core=%zu thread_ctx=%zu "
           "tls_cache=%zu stats=%zu slab=%zu slab_slot=%zu "
           "slab_slot_header=%zu tagged_freelist=%zu metadata_reader=%zu "
           "spinlock=%zu spinlock_cell=%zu "
           "page_leaf=%zu block=%zu vma=%zu free_extent=%zu "
           "control_extent=%zu\n",
           sizeof(void *), (unsigned)UHM_CACHELINE_SIZE, page_size,
           sizeof(uhm_heap), sizeof(uhm_heap_core), sizeof(uhm_thread_ctx),
           sizeof(uhm_tls_cache), sizeof(uhm_heap_stats_internal),
           sizeof(uhm_slab), sizeof(uhm_slab_slot), header,
           sizeof(uhm_tagged_freelist), sizeof(uhm_metadata_reader_state),
           sizeof(uhm_spinlock), sizeof(uhm_spinlock_cell),
           sizeof(uhm_page_leaf), sizeof(uhm_block), sizeof(uhm_vma),
           sizeof(uhm_free_extent), sizeof(uhm_control_extent));
    printf("metadata raw-ready-layout: pointer_size=%zu directory_bytes=%zu "
           "class_count=%u ready_seq_end=%zu pending_mask_offset=%zu\n",
           sizeof(void *), raw_directory_bytes, (unsigned)UHM_SIZE_CLASS_COUNT,
           offsetof(uhm_heap_core, slab_remote_ready_seq) +
               sizeof(env->heap->core.slab_remote_ready_seq),
           offsetof(uhm_heap_core, slab_remote_pending_masks));
    printf("metadata slab-slot-layout: magic=%zu flags=%zu size_class=%zu "
           "state=%zu requested_size=%zu state_word=%zu slab=%zu "
           "remote_link=%zu next_free=%zu\n",
           offsetof(uhm_slab_slot, magic), offsetof(uhm_slab_slot, flags),
           offsetof(uhm_slab_slot, size_class),
           offsetof(uhm_slab_slot, state),
           offsetof(uhm_slab_slot, requested_size),
           offsetof(uhm_slab_slot, state_word),
           offsetof(uhm_slab_slot, slab),
           offsetof(uhm_slab_slot, remote_link),
           offsetof(uhm_slab_slot, next_free));
    printf("metadata stats-cache-delta: thread_ctx_cached_requested=%zu "
           "tls_cache_cached_requested=%zu malloc_live_stat_shards=%u\n",
           offsetof(uhm_thread_ctx, cached_requested_bytes),
           offsetof(uhm_tls_cache, cached_requested_bytes),
           (unsigned)UHM_MALLOC_LIVE_STAT_SHARDS);

    for (i = 0; i < sizeof(sample_requests) / sizeof(sample_requests[0]); ++i) {
        size_t request = sample_requests[i];
        size_t overhead;
        size_t overhead_percent;
        size_t density_percent;
        uhm_slab *slab;

        ptrs[i] = env->api->malloc(env->heap, request);
        bench_expect(ptrs[i] != 0, "metadata footprint sample malloc");
        slab = bench_find_slab_by_payload(env, ptrs[i]);
        bench_expect(slab != 0, "metadata footprint sample slab");
        overhead = slab->slot_stride > request ? slab->slot_stride - request : 0u;
        overhead_percent = request != 0 ? (overhead * 100u) / request : 0u;
        density_percent = slab->slot_stride != 0
                              ? (slab->slot_size * 100u) / slab->slot_stride
                              : 0u;
        printf("metadata slab-slot-sample: request=%zu slot_size=%zu "
               "slot_stride=%zu header=%zu overhead_bytes=%zu "
               "overhead_percent=%zu payload_density_percent=%zu "
               "total_slots=%zu stride_mod_cacheline=%zu "
               "state_word_mod_cacheline=%zu remote_link_mod_cacheline=%zu "
               "next_free_mod_cacheline=%zu\n",
               request, slab->slot_size, slab->slot_stride, header,
               overhead, overhead_percent, density_percent, slab->total_slots,
               slab->slot_stride % UHM_CACHELINE_SIZE,
               offsetof(uhm_slab_slot, state_word) % UHM_CACHELINE_SIZE,
               offsetof(uhm_slab_slot, remote_link) % UHM_CACHELINE_SIZE,
               offsetof(uhm_slab_slot, next_free) % UHM_CACHELINE_SIZE);
        bench_print_slab_slot_layout_models(request, page_size, slab, header);
    }

    for (i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); ++i) {
        env->api->free(env->heap, ptrs[i]);
    }
    bench_check_heap(env, "check after metadata footprint report");
}

typedef struct bench_slot_ab_writer_args {
    _Atomic(uint64_t) *word;
    size_t iterations;
    _Atomic(uint32_t) *ready;
    _Atomic(uint32_t) *start;
    int ok;
} bench_slot_ab_writer_args;

static int bench_slot_ab_writer_run(bench_slot_ab_writer_args *args) {
    size_t i;

    args->ok = 0;
    atomic_fetch_add_explicit(args->ready, 1u, UHM_ATOMIC_ACQ_REL);
    while (atomic_load_explicit(args->start, UHM_ATOMIC_ACQUIRE) == 0u) {
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    for (i = 0; i < args->iterations; ++i) {
        atomic_fetch_add_explicit(args->word, UINT64_C(1),
                                  UHM_ATOMIC_RELAXED);
    }
    args->ok = 1;
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI bench_slot_ab_writer_entry(LPVOID user) {
    return bench_slot_ab_writer_run((bench_slot_ab_writer_args *)user) ? 0u
                                                                      : 1u;
}
#else
static void *bench_slot_ab_writer_entry(void *user) {
    (void)bench_slot_ab_writer_run((bench_slot_ab_writer_args *)user);
    return 0;
}
#endif

static bench_result bench_slot_ab_writer_pair(
    const char *name,
    _Atomic(uint64_t) *left,
    _Atomic(uint64_t) *right,
    size_t iterations) {
    bench_slot_ab_writer_args args[2];
    _Atomic(uint32_t) ready;
    _Atomic(uint32_t) start;
    uint64_t begin;
    uint64_t end;
#if defined(_WIN32)
    HANDLE threads[2];
#else
    pthread_t threads[2];
#endif

    memset(args, 0, sizeof(args));
    atomic_init(&ready, 0u);
    atomic_init(&start, 0u);
    args[0].word = left;
    args[0].iterations = iterations;
    args[0].ready = &ready;
    args[0].start = &start;
    args[1].word = right;
    args[1].iterations = iterations;
    args[1].ready = &ready;
    args[1].start = &start;

#if defined(_WIN32)
    threads[0] = CreateThread(0, 0, bench_slot_ab_writer_entry, &args[0], 0,
                              0);
    threads[1] = CreateThread(0, 0, bench_slot_ab_writer_entry, &args[1], 0,
                              0);
    bench_expect(threads[0] != 0 && threads[1] != 0,
                 "slab slot A/B thread create");
#else
    bench_expect(pthread_create(&threads[0], 0, bench_slot_ab_writer_entry,
                                &args[0]) == 0,
                 "slab slot A/B thread create");
    bench_expect(pthread_create(&threads[1], 0, bench_slot_ab_writer_entry,
                                &args[1]) == 0,
                 "slab slot A/B thread create");
#endif
    while (atomic_load_explicit(&ready, UHM_ATOMIC_ACQUIRE) != 2u) {
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
    begin = bench_now_ns();
    atomic_store_explicit(&start, 1u, UHM_ATOMIC_RELEASE);
#if defined(_WIN32)
    WaitForSingleObject(threads[0], INFINITE);
    WaitForSingleObject(threads[1], INFINITE);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
#else
    pthread_join(threads[0], 0);
    pthread_join(threads[1], 0);
#endif
    end = bench_now_ns();
    bench_expect(args[0].ok && args[1].ok, "slab slot A/B thread result");
    return bench_result_make(name, begin, end, iterations * 2u);
}

static void bench_slab_slot_ab_case(bench_heap_env *env,
                                    const char *model_name,
                                    unsigned model_id,
                                    size_t request,
                                    size_t payload,
                                    size_t current_effective,
                                    size_t inline_header,
                                    size_t side_bytes,
                                    size_t metadata_stride,
                                    size_t effective_bytes,
                                    unsigned metadata_in_payload,
                                    unsigned cacheline_padded,
                                    size_t iterations,
                                    int diagnostics) {
    size_t alloc_bytes;
    void *raw;
    uintptr_t raw_addr;
    uintptr_t base;
    _Atomic(uint64_t) *left;
    _Atomic(uint64_t) *right;
    uint64_t left_line;
    uint64_t right_line;
    int line_overlap;
    char name[96];
    char check_message[128];
    bench_result result;
    bench_diag_snapshot diag_before;

    bench_expect(metadata_stride >= sizeof(uint64_t),
                 "slab slot A/B metadata stride");
    bench_expect(effective_bytes >= request,
                 "slab slot A/B effective size");
    bench_expect(metadata_stride <= (size_t)-1 / 2u,
                 "slab slot A/B allocation overflow");
    alloc_bytes = metadata_stride * 2u + UHM_CACHELINE_SIZE + sizeof(uint64_t);
    raw = malloc(alloc_bytes);
    bench_expect(raw != 0, "slab slot A/B allocation");
    memset(raw, 0, alloc_bytes);
    raw_addr = (uintptr_t)raw;
    base = (raw_addr + (uintptr_t)(UHM_CACHELINE_SIZE - 1u)) &
           ~(uintptr_t)(UHM_CACHELINE_SIZE - 1u);
    left = (_Atomic(uint64_t) *)base;
    right = (_Atomic(uint64_t) *)(base + metadata_stride);
    atomic_init(left, UINT64_C(0));
    atomic_init(right, UINT64_C(0));
    left_line = (uint64_t)(base / (uintptr_t)UHM_CACHELINE_SIZE);
    right_line =
        (uint64_t)((base + metadata_stride) / (uintptr_t)UHM_CACHELINE_SIZE);
    line_overlap = left_line == right_line;

    printf("metadata slab-slot-layout-ab-%s: request=%zu model_id=%u "
           "payload_bytes=%zu current_effective_bytes=%zu "
           "inline_header=%zu side_bytes=%zu metadata_stride=%zu "
           "effective_bytes=%zu metadata_in_payload=%u cacheline_padded=%u "
           "line_overlap=%u left_line=%llu right_line=%llu "
           "stride_mod_cacheline=%zu bytes_saved_vs_current=%zu "
           "bytes_extra_vs_current=%zu\n",
           model_name, request, model_id, payload, current_effective,
           inline_header, side_bytes, metadata_stride, effective_bytes,
           metadata_in_payload, cacheline_padded, (unsigned)line_overlap,
           (unsigned long long)left_line, (unsigned long long)right_line,
           metadata_stride % UHM_CACHELINE_SIZE,
           current_effective > effective_bytes ? current_effective - effective_bytes
                                               : 0u,
           effective_bytes > current_effective ? effective_bytes - current_effective
                                               : 0u);

    snprintf(name, sizeof(name), "slab-slot layout %s req=%zu", model_name,
             request);
    snprintf(check_message, sizeof(check_message),
             "check after slab slot layout %s req=%zu", model_name, request);
    bench_diag_capture(env, &diag_before);
    result = bench_slot_ab_writer_pair(name, left, right, iterations);
    bench_run_uhm_case(env, &result, &diag_before, diagnostics,
                       check_message);
    free(raw);
}

static void bench_slab_slot_ab_request(bench_heap_env *env,
                                       size_t request,
                                       size_t page_size,
                                       size_t iterations,
                                       int diagnostics) {
    void *ptr;
    uhm_slab *slab;
    size_t payload;
    size_t current_stride;
    size_t compact16_stride;
    size_t compact32_stride;
    size_t side16_stride;
    size_t padded64_stride;

    ptr = env->api->malloc(env->heap, request);
    bench_expect(ptr != 0, "slab slot A/B sample malloc");
    slab = bench_find_slab_by_payload(env, ptr);
    bench_expect(slab != 0, "slab slot A/B sample slab");
    payload = slab->slot_size;
    current_stride = slab->slot_stride;
    compact16_stride = bench_model_align_up(payload + 16u, UHM_MIN_ALIGNMENT);
    compact32_stride = bench_model_align_up(payload + 32u, UHM_MIN_ALIGNMENT);
    side16_stride = bench_model_align_up(16u, sizeof(uint64_t));
    padded64_stride = bench_model_align_up(current_stride, UHM_CACHELINE_SIZE);
    (void)page_size;

    env->api->free(env->heap, ptr);
    bench_check_heap(env, "check before slab slot A/B model run");

    bench_slab_slot_ab_case(env, "current", 0u, request, payload,
                            current_stride, bench_slab_slot_header_size(), 0u,
                            current_stride, current_stride, 1u, 0u,
                            iterations, diagnostics);
    bench_slab_slot_ab_case(env, "compact16", 1u, request, payload,
                            current_stride, 16u, 0u, compact16_stride,
                            compact16_stride, 1u, 0u, iterations,
                            diagnostics);
    bench_slab_slot_ab_case(env, "compact32", 2u, request, payload,
                            current_stride, 32u, 0u, compact32_stride,
                            compact32_stride, 1u, 0u, iterations,
                            diagnostics);
    bench_slab_slot_ab_case(env, "side16", 3u, request, payload,
                            current_stride, 0u, 16u, side16_stride,
                            payload + 16u, 0u, 0u, iterations, diagnostics);
    bench_slab_slot_ab_case(env, "padded64", 4u, request, payload,
                            current_stride, bench_slab_slot_header_size(), 0u,
                            padded64_stride, padded64_stride, 1u, 1u,
                            iterations, diagnostics);
}

static void bench_slab_slot_ab_matrix(bench_heap_env *env,
                                      size_t page_size,
                                      size_t iterations,
                                      int diagnostics) {
    static const size_t sample_requests[] = {
        16u, 32u, 64u, 128u, 256u, 512u,
    };
    size_t i;

    for (i = 0; i < sizeof(sample_requests) / sizeof(sample_requests[0]); ++i) {
        bench_slab_slot_ab_request(env, sample_requests[i], page_size,
                                   iterations, diagnostics);
    }
    bench_check_heap(env, "check after slab slot A/B matrix");
}

static void bench_release_nodiag_page_stats_zero_check(bench_heap_env *env) {
#if !UHMAS_ENABLE_DIAGNOSTICS
    uhm_stats stats;
    uint64_t page_stat_total;

    bench_expect(env && env->heap && env->api, "release-nodiag stats heap");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    bench_expect_ok(env->api->get_stats(env->heap, &stats),
                    "release-nodiag stats query");
    page_stat_total = stats.page_span_calls + stats.page_span_pages +
                      stats.page_mark_calls + stats.page_mark_pages +
                      stats.page_mark_max_pages +
                      stats.page_mark_budget_exceeded +
                      stats.page_mark_leaf_spans +
                      stats.page_mark_max_leaf_spans +
                      stats.page_range_query_calls +
                      stats.page_range_query_pages +
                      stats.page_range_query_max_pages +
                      stats.page_range_query_budget_exceeded +
                      stats.page_range_query_leaf_spans +
                      stats.page_range_query_max_leaf_spans +
                      stats.page_lock_range_calls +
                      stats.page_lock_range_shards +
                      stats.page_lock_range_max_shards +
                      stats.page_lock_range_budget_exceeded;
    bench_expect(page_stat_total == UINT64_C(0),
                 "release-nodiag page diagnostic stats are zero");
    printf("metadata release-nodiag-stats-zero: page_stats_zero=1 "
           "page_span_calls=%llu page_mark_calls=%llu "
           "page_range_query_calls=%llu page_lock_range_calls=%llu "
           "page_mark_budget_exceeded=%llu "
           "page_range_query_budget_exceeded=%llu "
           "page_lock_range_budget_exceeded=%llu\n",
           (unsigned long long)stats.page_span_calls,
           (unsigned long long)stats.page_mark_calls,
           (unsigned long long)stats.page_range_query_calls,
           (unsigned long long)stats.page_lock_range_calls,
           (unsigned long long)stats.page_mark_budget_exceeded,
           (unsigned long long)stats.page_range_query_budget_exceeded,
           (unsigned long long)stats.page_lock_range_budget_exceeded);
#else
    (void)env;
#endif
}

static void bench_print_release_build_config(void) {
    printf("metadata release-build-config: diagnostics_compiled=%u "
           "test_hooks_compiled=%u diagnostics_timing_compiled=%u\n",
           (unsigned)UHMAS_ENABLE_DIAGNOSTICS,
           (unsigned)UHMAS_ENABLE_TEST_HOOKS,
           (unsigned)UHMAS_ENABLE_DIAGNOSTICS);
}

int main(void) {
    const int stress = bench_stress_enabled();
    const int false_sharing = bench_false_sharing_enabled();
    const int small_fast_path = bench_small_fast_path_enabled();
    const int release_breakdown = bench_release_breakdown_enabled();
    const int release_pressure_matrix =
        bench_release_pressure_matrix_enabled();
    const int slab_empty_churn = bench_slab_empty_churn_enabled();
    const int heap_close = bench_heap_close_enabled();
    const int mmap_locality = bench_mmap_locality_enabled();
    const int control_allocator_churn =
        bench_control_allocator_churn_enabled();
    const int live_block_lookup = bench_live_block_lookup_enabled();
    const int treap_adversarial = bench_treap_adversarial_enabled();
    const int mprotect_multi_vma = bench_mprotect_multi_vma_enabled();
    const int munmap_multi_vma = bench_munmap_multi_vma_enabled();
    const int spinlock_thresholds = bench_spinlock_thresholds_enabled();
    const int metadata_footprint = bench_metadata_footprint_enabled();
    const int slab_slot_sharing = bench_slab_slot_sharing_enabled();
    const int slab_slot_ab = bench_slab_slot_ab_enabled();
    const int gate_32bit = bench_32bit_gate_enabled();
    const int diagnostics = bench_diagnostics_enabled();
    const int diagnostics_timing =
        diagnostics && bench_diagnostics_timing_enabled();
    const size_t mprotect_multi_vma_stress_pages =
        bench_mprotect_multi_vma_stress_pages();
    const size_t mprotect_multi_vma_latency_samples =
        bench_mprotect_multi_vma_latency_samples();
    const size_t arena_size = stress ? 256u * 1024u * 1024u
                                     : 96u * 1024u * 1024u;
    const size_t small_iters = stress ? 240000u : 24000u;
    const size_t thread_iters = stress ? 70000u : 7000u;
    const size_t false_remote_iters = stress ? 20000u : 4000u;
    const size_t false_page_iters = stress ? 2000u : 300u;
    const size_t large_iters = stress ? 9000u : 900u;
    const size_t mmap_iters = stress ? 5000u : 500u;
    const size_t control_allocator_iters = stress ? 384u : 96u;
    const size_t fixed_iters = stress ? 2500u : 250u;
    const size_t release_iters = stress ? 90000u : 9000u;
    const size_t live_lookup_iters = stress ? 24000u : 3000u;
    const size_t treap_iters = stress ? 4096u : 512u;
    const size_t mprotect_multi_vma_iters = stress ? 1500u : 200u;
    const size_t mprotect_multi_vma_wide_iters = stress ? 375u : 50u;
    const size_t munmap_multi_vma_iters = stress ? 1500u : 200u;
    const size_t gate_32bit_remote_iters = stress ? 8000u : 1200u;
    const size_t gate_32bit_ctx_iters = stress ? 16000u : 2400u;
    const size_t gate_32bit_tls_iters = stress ? 16000u : 2400u;
    const size_t gate_32bit_mmap_churn_iters = stress ? 12000u : 1600u;
    const size_t gate_32bit_mprotect_iters = stress ? 1200u : 160u;
    const size_t gate_32bit_mremap_iters = stress ? 1200u : 160u;
    const size_t slab_slot_sharing_iters = stress ? 1500000u : 250000u;
    static const size_t false_sharing_thread_counts[] = {
        16u, 32u, 64u,
    };
    bench_heap_env env;
    uhm_vm_ops vm;
    size_t page_size;
    bench_result result;
    bench_diag_snapshot diag_before;
    size_t false_sharing_index;
    size_t false_sharing_thread_count;
    size_t false_sharing_scaled_iters;
    size_t false_sharing_scaled_remote_iters;
    char false_sharing_name[96];
    char false_sharing_check_message[128];

    bench_expect_ok(uhm_platform_vm_ops_init(&vm), "benchmark page size init");
    page_size = vm.page_size ? vm.page_size : uhm_platform_system_page_size();
    bench_heap_open(&env, arena_size);
    bench_diag_interval_max_self_check(&env);
    if (diagnostics) {
        uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
    }

    printf("UHMAS benchmark mode: %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
           false_sharing ? "false-sharing " : "",
           small_fast_path ? "small-fast-path " : "",
           release_breakdown ? "release-breakdown " : "",
           release_pressure_matrix ? "release-pressure-matrix " : "",
           slab_empty_churn ? "slab-empty-churn " : "",
           heap_close ? "heap-close " : "",
           mmap_locality ? "mmap-locality " : "",
           control_allocator_churn ? "control-allocator-churn " : "",
           live_block_lookup ? "live-block-lookup " : "",
           treap_adversarial ? "treap-adversarial " : "",
           mprotect_multi_vma ? "mprotect-multi-vma " : "",
           munmap_multi_vma ? "munmap-multi-vma " : "",
           spinlock_thresholds ? "spinlock-thresholds " : "",
           metadata_footprint ? "metadata-footprint " : "",
           slab_slot_sharing ? "slab-slot-sharing " : "",
           slab_slot_ab ? "slab-slot-ab " : "",
           gate_32bit ? "32bit-gate " : "",
           stress ? "stress" : "quick");
    printf("arena_size=%zu page_size=%zu\n", arena_size, page_size);
    printf("diagnostics_enabled=%d diagnostics_timing_enabled=%d\n",
           diagnostics, diagnostics_timing);
    bench_print_release_build_config();
    bench_print_abi_capabilities();
    bench_platform_named_comparisons();

    if (metadata_footprint) {
        bench_print_metadata_footprint(&env, page_size);
        bench_heap_close(&env);
        return 0;
    }

    if (slab_slot_sharing) {
        bench_slab_slot_sharing_matrix(&env, slab_slot_sharing_iters,
                                       diagnostics);
        bench_heap_close(&env);
        return 0;
    }

    if (slab_slot_ab) {
        bench_slab_slot_ab_matrix(&env, page_size, slab_slot_sharing_iters,
                                  diagnostics);
        bench_heap_close(&env);
        return 0;
    }

    if (small_fast_path) {
        bench_heap_close(&env);
        bench_small_fast_path_matrix(arena_size, small_iters, thread_iters,
                                     page_size, diagnostics);
        return 0;
    }

    if (false_sharing) {
        bench_diag_capture(&env, &diag_before);
        result = bench_multithread_small_named(
            &env, thread_iters, 0, 4u, 1,
            "uhm small malloc/free multithread");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after false sharing small benchmark");
        bench_diag_capture(&env, &diag_before);
        result = bench_false_remote_handoff(&env, false_remote_iters);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after false sharing remote benchmark");
        for (false_sharing_index = 0;
             false_sharing_index < sizeof(false_sharing_thread_counts) /
                                       sizeof(false_sharing_thread_counts[0]);
             ++false_sharing_index) {
            false_sharing_thread_count =
                false_sharing_thread_counts[false_sharing_index];
            false_sharing_scaled_iters =
                (thread_iters * 4u + false_sharing_thread_count - 1u) /
                false_sharing_thread_count;
            snprintf(false_sharing_name, sizeof(false_sharing_name),
                     "false-share small malloc/free threads=%zu",
                     false_sharing_thread_count);
            snprintf(false_sharing_check_message,
                     sizeof(false_sharing_check_message),
                     "check after false sharing small threads=%zu",
                     false_sharing_thread_count);
            bench_diag_capture(&env, &diag_before);
            result = bench_multithread_small_named(
                &env, false_sharing_scaled_iters, 0,
                false_sharing_thread_count, 1,
                false_sharing_name);
            bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                               false_sharing_check_message);
        }
        for (false_sharing_index = 0;
             false_sharing_index < sizeof(false_sharing_thread_counts) /
                                       sizeof(false_sharing_thread_counts[0]);
             ++false_sharing_index) {
            false_sharing_thread_count =
                false_sharing_thread_counts[false_sharing_index];
            false_sharing_scaled_remote_iters =
                (false_remote_iters * 4u + false_sharing_thread_count - 1u) /
                false_sharing_thread_count;
            snprintf(false_sharing_name, sizeof(false_sharing_name),
                     "false-share remote free only threads=%zu",
                     false_sharing_thread_count);
            snprintf(false_sharing_check_message,
                     sizeof(false_sharing_check_message),
                     "check after false sharing remote free only threads=%zu",
                     false_sharing_thread_count);
            bench_diag_capture(&env, &diag_before);
            result = bench_false_remote_handoff_named(
                &env, false_sharing_scaled_remote_iters,
                false_sharing_thread_count, 0, false_sharing_name);
            bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                               false_sharing_check_message);
        }
        for (false_sharing_index = 0;
             false_sharing_index < sizeof(false_sharing_thread_counts) /
                                       sizeof(false_sharing_thread_counts[0]);
             ++false_sharing_index) {
            false_sharing_thread_count =
                false_sharing_thread_counts[false_sharing_index];
            false_sharing_scaled_remote_iters =
                (false_remote_iters * 4u + false_sharing_thread_count - 1u) /
                false_sharing_thread_count;
            snprintf(false_sharing_name, sizeof(false_sharing_name),
                     "false-share remote free/reuse threads=%zu",
                     false_sharing_thread_count);
            snprintf(false_sharing_check_message,
                     sizeof(false_sharing_check_message),
                     "check after false sharing remote free/reuse threads=%zu",
                     false_sharing_thread_count);
            bench_diag_capture(&env, &diag_before);
            result = bench_false_remote_handoff_named(
                &env, false_sharing_scaled_remote_iters,
                false_sharing_thread_count, 1, false_sharing_name);
            bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                               false_sharing_check_message);
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_false_page_map_hotspot(&env, false_page_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after false sharing page benchmark");
        bench_heap_close(&env);
        return 0;
    }

    if (live_block_lookup) {
        bench_diag_capture(&env, &diag_before);
        result = bench_live_block_fast_lookup(&env, live_lookup_iters,
                                              page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after live-block fast lookup");
        bench_diag_capture(&env, &diag_before);
        result = bench_live_block_descriptor_slow(&env, live_lookup_iters,
                                                  page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after live-block descriptor slow lookup");
        bench_diag_capture(&env, &diag_before);
        result = bench_live_block_invalid_lookup(&env, live_lookup_iters);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after live-block invalid lookup");
        bench_diag_capture(&env, &diag_before);
        result = bench_live_block_fragmented_slow(&env, live_lookup_iters,
                                                  page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after live-block fragmented slow lookup");
        bench_heap_close(&env);
        return 0;
    }

    if (treap_adversarial) {
        bench_diag_capture(&env, &diag_before);
        result = bench_treap_malloc_free_pressure(&env, treap_iters,
                                                  page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after treap malloc pressure");
        bench_heap_close(&env);

        bench_heap_open(&env, arena_size);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_treap_malloc_same_size_pressure(&env, treap_iters,
                                                       page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after treap same-size malloc pressure");
        bench_heap_close(&env);

        bench_heap_open(&env, arena_size);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_treap_malloc_remove_pressure(&env, treap_iters,
                                                    page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after treap malloc remove pressure");
        bench_heap_close(&env);

        bench_heap_open_malloc_retention(&env, arena_size, 1024u);
        bench_diag_capture(&env, &diag_before);
        result = bench_treap_malloc_two_child_remove_pressure(
            &env, treap_iters, page_size);
        bench_run_uhm_case(
            &env, &result, &diag_before, diagnostics,
            "check after treap malloc two-child remove pressure");
        bench_heap_close(&env);

        bench_heap_open(&env, arena_size);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_treap_vma_pressure(&env, treap_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after treap vma pressure");
        bench_heap_close(&env);

        bench_heap_open(&env, arena_size);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        {
            bench_treap_fixed_sparse_vma_state sparse_state;
            bench_treap_fixed_sparse_vma_prepare(&env, &sparse_state,
                                                 treap_iters, page_size);
            bench_diag_capture(&env, &diag_before);
            result = bench_treap_fixed_sparse_vma_build_pressure(
                &env, &sparse_state);
            bench_run_uhm_case(
                &env, &result, &diag_before, diagnostics,
                "check after treap fixed sparse VMA build pressure");
            result = bench_treap_fixed_sparse_vma_protect_pressure(
                &env, &sparse_state);
            bench_print(&result);
            bench_check_heap(
                &env, "check after treap fixed sparse VMA protect pressure");
            bench_treap_fixed_sparse_vma_cleanup(&sparse_state);
        }
        bench_heap_close(&env);

        bench_heap_open(&env, arena_size);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_treap_mmap_free_pressure(&env, treap_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after treap mmap free pressure");
        bench_heap_close(&env);
        return 0;
    }

    if (mprotect_multi_vma) {
        if (mprotect_multi_vma_stress_pages == 0) {
            bench_diag_capture(&env, &diag_before);
            result = bench_page_private_restore_free(
                &env, mprotect_multi_vma_iters, page_size);
            bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                               "check after page-private restore/free");
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_multi_vma_pass(&env, mprotect_multi_vma_iters,
                                               page_size, 8u,
                                               "multi-VMA mprotect pass pressure");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after multi-VMA mprotect benchmark");
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_multi_vma_rollback(
            &env, mprotect_multi_vma_iters, page_size, 8u,
            "multi-VMA mprotect rollback pressure");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after multi-VMA mprotect rollback");
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_multi_vma_pass(
            &env, mprotect_multi_vma_wide_iters, page_size, 32u,
            "multi-VMA mprotect wide pass pressure");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after wide multi-VMA mprotect benchmark");
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_multi_vma_rollback(
            &env, mprotect_multi_vma_wide_iters, page_size, 32u,
            "multi-VMA mprotect wide rollback pressure");
        bench_run_uhm_case(
            &env, &result, &diag_before, diagnostics,
            "check after wide multi-VMA mprotect rollback");
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_multi_vma_merge(&env, mprotect_multi_vma_iters,
                                                page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after multi-VMA mprotect merge");
        if (mprotect_multi_vma_stress_pages == 0) {
            bench_diag_capture(&env, &diag_before);
            result = bench_mprotect_exact_neighbor_merge(
                &env, mprotect_multi_vma_iters, page_size);
            bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                               "check after exact-neighbor mprotect merge");
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_partial_edge_merge(
            &env, mprotect_multi_vma_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after partial-edge mprotect merge");
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_multi_vma_merge_rollback(
            &env, mprotect_multi_vma_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after multi-VMA mprotect merge rollback");
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_multi_vma_random_fault(
            &env, mprotect_multi_vma_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after multi-VMA mprotect random fault");
        if (mprotect_multi_vma_stress_pages != 0) {
            size_t stress_iters = mprotect_multi_vma_latency_samples != 0
                                      ? mprotect_multi_vma_latency_samples
                                      : (stress ? 64u : 16u);
            bench_diag_capture(&env, &diag_before);
            result = bench_mprotect_multi_vma_pass(
                &env, stress_iters, page_size,
                mprotect_multi_vma_stress_pages,
                "multi-VMA mprotect configurable stress pass");
            bench_run_uhm_case(
                &env, &result, &diag_before, diagnostics,
                "check after configurable multi-VMA mprotect pass");
            bench_diag_capture(&env, &diag_before);
            result = bench_mprotect_multi_vma_rollback(
                &env, stress_iters, page_size,
                mprotect_multi_vma_stress_pages,
                "multi-VMA mprotect configurable stress rollback");
            bench_run_uhm_case(
                &env, &result, &diag_before, diagnostics,
                "check after configurable multi-VMA mprotect rollback");
        }
        bench_heap_close(&env);
        return 0;
    }

    if (munmap_multi_vma) {
        bench_diag_capture(&env, &diag_before);
        result = bench_munmap_exact_vma(&env, munmap_multi_vma_iters,
                                        page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after munmap exact VMA");
        bench_diag_capture(&env, &diag_before);
        result = bench_munmap_head_tail_middle(&env,
                                               munmap_multi_vma_iters,
                                               page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after munmap head/tail/middle");
        bench_diag_capture(&env, &diag_before);
        result = bench_munmap_right_small_middle(&env,
                                                 munmap_multi_vma_iters,
                                                 page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after munmap right-small middle");
        bench_diag_capture(&env, &diag_before);
        result = bench_munmap_multi_vma_full(&env, munmap_multi_vma_iters,
                                             page_size, 8u,
                                             "multi-VMA munmap 8-page full");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after multi-VMA munmap full");
        bench_diag_capture(&env, &diag_before);
        result = bench_munmap_multi_vma_partial(&env,
                                                munmap_multi_vma_iters,
                                                page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after multi-VMA munmap partial");
        bench_diag_capture(&env, &diag_before);
        result = bench_munmap_multi_vma_full(&env,
                                             munmap_multi_vma_iters / 4u,
                                             page_size, 32u,
                                             "multi-VMA munmap 32-page full");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after wide multi-VMA munmap full");
        bench_heap_close(&env);
        return 0;
    }

    if (spinlock_thresholds) {
        bench_heap_close(&env);
        bench_spinlock_threshold_matrix(96u * 1024u * 1024u, thread_iters,
                                        false_remote_iters, false_page_iters,
                                        release_iters / 3u, page_size,
                                        diagnostics);
        return 0;
    }

    if (release_pressure_matrix) {
        bench_heap_close(&env);
        bench_release_pressure_matrix_run(arena_size, release_iters,
                                          page_size, diagnostics);
        return 0;
    }

    if (slab_empty_churn) {
        uhm_thread_ctx *batch_contexts
            [BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT] = {0};
        void *batch_ctx_raws[BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT] = {0};
        size_t batch_index;

        bench_heap_close(&env);
        bench_heap_open_no_tls_config_flags_ex(&env, arena_size, 0u, 0u);
        bench_diag_capture(&env, &diag_before);
        result = bench_slab_empty_churn(&env, 64u,
                                        "slab empty adaptive churn 64B");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after slab empty 64B churn benchmark");
        bench_diag_capture(&env, &diag_before);
        result = bench_slab_empty_churn(&env, 512u,
                                        "slab empty adaptive churn 512B");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after slab empty 512B churn benchmark");
        bench_heap_close(&env);
        /*
         * Keep control metadata growth out of the physical-run benchmark.
         * The allocator still handles real control-band gaps, but this case
         * intentionally measures one contiguous 12-page excess per sample
         * on both 32-bit and 64-bit layouts.
         */
        bench_heap_open_no_tls_control_config_flags_ex(
            &env, arena_size, 0u, 0u, 256u * 1024u);
        result = bench_slab_empty_full_drain_batch(
            &env, &diag_before, batch_contexts, batch_ctx_raws);
        bench_run_uhm_case(
            &env, &result, &diag_before, diagnostics,
            "check after slab empty full-drain batch benchmark");
        for (batch_index = 0u;
             batch_index < BENCH_SLAB_EMPTY_BATCH_SAMPLE_COUNT;
             ++batch_index) {
            env.api->thread_ctx_close(env.heap,
                                      batch_contexts[batch_index]);
            free(batch_ctx_raws[batch_index]);
        }
        bench_heap_close(&env);
        return 0;
    }

    if (heap_close) {
        bench_heap_close_run close_run;
        bench_heap_close_tls_run tls_run;
        bench_heap_close_mixed_run mixed_run;

        bench_heap_close(&env);
        close_run = bench_heap_close_owned_contexts(
            arena_size, 9u, 2u, "heap close owned ctx/class queue",
            "heap-close owned ctx-class-queue");
        bench_heap_close_print_run(&close_run, diagnostics);
        close_run = bench_heap_close_owned_contexts(
            arena_size, 1u, 0u, "heap close owned ctx 1",
            "heap-close owned ctx-1");
        bench_heap_close_print_run(&close_run, diagnostics);
        close_run = bench_heap_close_owned_contexts(
            arena_size, 3u, 0u, "heap close owned ctx 3",
            "heap-close owned ctx-3");
        bench_heap_close_print_run(&close_run, diagnostics);
        close_run = bench_heap_close_owned_contexts(
            arena_size, 9u, 0u, "heap close owned ctx 9",
            "heap-close owned ctx-9");
        bench_heap_close_print_run(&close_run, diagnostics);
        tls_run = bench_heap_close_owned_tls_workers(
            arena_size, 1u, "heap close owned TLS workers 1",
            "heap-close owned tls-workers-1");
        bench_heap_close_tls_print_run(&tls_run, diagnostics);
        tls_run = bench_heap_close_owned_tls_workers(
            arena_size, 3u, "heap close owned TLS workers 3",
            "heap-close owned tls-workers-3");
        bench_heap_close_tls_print_run(&tls_run, diagnostics);
        tls_run = bench_heap_close_owned_tls_workers(
            arena_size, 9u, "heap close owned TLS workers",
            "heap-close owned tls-workers");
        bench_heap_close_tls_print_run(&tls_run, diagnostics);
        mixed_run = bench_heap_close_mixed(
            arena_size, 1u, 1u,
            "heap close owned mixed ctx/TLS/class queue q1",
            "heap-close owned mixed-ctx-tls-class-queue-q1", "owned q1");
        bench_heap_close_mixed_print_run(&mixed_run, diagnostics);
        mixed_run = bench_heap_close_mixed(
            arena_size, 1u, 2u,
            "heap close owned mixed ctx/TLS/class queue",
            "heap-close owned mixed-ctx-tls-class-queue", 0);
        bench_heap_close_mixed_print_run(&mixed_run, diagnostics);
        mixed_run = bench_heap_close_mixed(
            arena_size, 1u, 8u,
            "heap close owned mixed ctx/TLS/class queue q8",
            "heap-close owned mixed-ctx-tls-class-queue-q8", 0);
        bench_heap_close_mixed_print_run(&mixed_run, diagnostics);
        mixed_run = bench_heap_close_mixed(
            arena_size, 1u, 32u,
            "heap close owned mixed ctx/TLS/class queue q32",
            "heap-close owned mixed-ctx-tls-class-queue-q32", "owned q32");
        bench_heap_close_mixed_print_run(&mixed_run, diagnostics);
        mixed_run = bench_heap_close_mixed(
            arena_size, 0u, 2u,
            "heap close pre-reserved mixed ctx/TLS/class queue",
            "heap-close pre-reserved mixed-ctx-tls-class-queue", 0);
        bench_heap_close_mixed_print_run(&mixed_run, diagnostics);
        return 0;
    }

    if (mmap_locality) {
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_locality_size_hint(
            &env, mmap_iters, page_size, 2u,
            "mmap locality size-hint churn 2p");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after mmap locality 2p benchmark");
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_locality_size_hint(
            &env, mmap_iters, page_size, 4u,
            "mmap locality size-hint churn 4p");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after mmap locality 4p benchmark");
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_locality_size_hint(
            &env, mmap_iters, page_size, 8u,
            "mmap locality size-hint churn 8p");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after mmap locality 8p benchmark");
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result =
            bench_mmap_locality_same_bucket_hint(&env, mmap_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after mmap locality same-bucket benchmark");
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result =
            bench_mmap_locality_deep_bucket_hint(&env, mmap_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after mmap locality deep-bucket benchmark");
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_locality_fragmented_ring_hint(&env, mmap_iters,
                                                          page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after mmap locality fragmented-ring "
                           "benchmark");
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_locality_exact_front_hot_cache(
            &env, mmap_iters, page_size);
        bench_run_uhm_case(
            &env, &result, &diag_before, diagnostics,
            "check after mmap locality exact-front hot-cache benchmark");
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_locality_sparse_cache_maintenance(
            &env, mmap_iters, page_size);
        bench_run_uhm_case(
            &env, &result, &diag_before, diagnostics,
            "check after mmap locality sparse hot-cache benchmark");
        bench_heap_close(&env);
        bench_heap_open(&env, arena_size);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        bench_mmap_hint_reset(&env);
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_locality_adjacent_release_lookup(
            &env, mmap_iters, page_size);
        bench_run_uhm_case(
            &env, &result, &diag_before, diagnostics,
            "check after mmap locality adjacent-release benchmark");
        bench_heap_close(&env);
        return 0;
    }

    if (control_allocator_churn) {
        bench_heap_close(&env);
        bench_heap_open_control_band(&env, arena_size, 16u * 1024u);
        bench_control_allocator_hint_window_prepare(&env, page_size);
        bench_diag_capture(&env, &diag_before);
        result = bench_control_allocator_hint_window_churn(
            &env, control_allocator_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after control allocator hint-window churn");
        bench_diag_capture(&env, &diag_before);
        result = bench_control_allocator_recent_extent_churn(
            &env, control_allocator_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after control allocator recent-extent churn");
        bench_heap_close(&env);
        return 0;
    }

    if (gate_32bit) {
#if UINTPTR_MAX == UINT32_MAX
        bench_heap_close(&env);
        bench_heap_open_no_tls_config_flags_ex(&env, arena_size, 0u, 0u);
        bench_diag_capture(&env, &diag_before);
        result = bench_raw_remote_partial_burst(
            &env, gate_32bit_remote_iters, "uhm 32-bit locked partial burst");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after locked partial burst");
        bench_heap_close(&env);
        bench_heap_open_no_tls_config_flags_ex(
            &env, arena_size, UHM_CONFIG_LOCKLESS_REMOTE_FREE, 0u);
        bench_expect(env.heap->core.low_lock_remote_free_enabled != 0u,
                     "32-bit burst low-lock mode enabled");
        bench_diag_capture(&env, &diag_before);
        result = bench_raw_remote_partial_burst(
            &env, gate_32bit_remote_iters, "uhm 32-bit low-lock partial burst");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after low-lock partial burst");
        bench_heap_close(&env);
        bench_heap_open(&env, arena_size);
#endif
        bench_diag_capture(&env, &diag_before);
        result = bench_false_remote_handoff(&env, gate_32bit_remote_iters);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after 32-bit gate remote benchmark");
        bench_diag_capture(&env, &diag_before);
        result = bench_thread_ctx_cache_reuse(&env, gate_32bit_ctx_iters,
                                              page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after 32-bit gate ctx cache benchmark");
        bench_heap_close(&env);

        bench_heap_open_tls(&env, arena_size);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_tls_cache_reuse(&env, gate_32bit_tls_iters);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after 32-bit gate TLS cache benchmark");
        bench_heap_close(&env);

        bench_heap_open(&env, arena_size);
        if (diagnostics) {
            uhm_spinlock_set_diagnostics_enabled(&env.heap->lock, 1u);
        }
        bench_diag_capture(&env, &diag_before);
        result = bench_mprotect_split(&env, gate_32bit_mprotect_iters,
                                      page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after 32-bit gate mprotect benchmark");
        bench_diag_capture(&env, &diag_before);
        result = bench_mremap_resize(&env, gate_32bit_mremap_iters,
                                     page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after 32-bit gate mremap benchmark");
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_churn_named(&env, gate_32bit_mmap_churn_iters,
                                        page_size, "uhm mmap churn");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after 32-bit gate mmap churn benchmark");
        bench_diag_capture(&env, &diag_before);
        result = bench_collision_pressure(&env, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after 32-bit gate collision benchmark");
        bench_heap_close(&env);
        return 0;
    }

    if (release_breakdown) {
        bench_diag_capture(&env, &diag_before);
        result = bench_release_malloc_churn(&env, release_iters);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after release breakdown malloc churn");
        bench_diag_capture(&env, &diag_before);
        result = bench_release_free_burst(&env, release_iters);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after release breakdown free burst");
        bench_diag_capture(&env, &diag_before);
        result = bench_mmap_churn_named(&env, release_iters, page_size,
                                        "release breakdown mmap churn");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after release breakdown mmap churn");
        bench_diag_capture(&env, &diag_before);
        result = bench_release_munmap_burst(&env, release_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after release breakdown munmap burst");
        bench_diag_capture(&env, &diag_before);
        result = bench_release_mprotect_toggle(&env, release_iters, page_size);
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after release breakdown mprotect toggle");
        bench_diag_capture(&env, &diag_before);
        result = bench_release_pressure_with_latency(
            &env, release_iters, page_size, "uhm release mixed long pressure");
        bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                           "check after release breakdown mixed pressure");
        bench_heap_close(&env);
        return 0;
    }

    bench_diag_capture(&env, &diag_before);
    result = bench_uhm_small(&env, small_iters);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after uhm small benchmark");
    result = bench_system_small(small_iters);
    bench_print(&result);
    bench_diag_capture(&env, &diag_before);
    result = bench_multithread_small(&env, thread_iters, 0);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after uhm multithread small benchmark");
    result = bench_multithread_small(&env, thread_iters, 1);
    bench_print(&result);
    bench_diag_capture(&env, &diag_before);
    result = bench_large_malloc(&env, large_iters);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after uhm large benchmark");
    result = bench_system_large_malloc(large_iters);
    bench_print(&result);
    bench_diag_capture(&env, &diag_before);
    result = bench_uhm_mmap(&env, mmap_iters, page_size);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after uhm mmap benchmark");
    result = bench_system_mmap(mmap_iters, page_size);
    bench_print(&result);
    bench_diag_capture(&env, &diag_before);
    result = bench_fixed_mix(&env, fixed_iters, page_size);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after uhm fixed mix benchmark");
    bench_diag_capture(&env, &diag_before);
    result = bench_mprotect_split(&env, fixed_iters, page_size);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after uhm mprotect benchmark");
    bench_diag_capture(&env, &diag_before);
    result = bench_collision_pressure(&env, page_size);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after uhm collision benchmark");
    bench_diag_capture(&env, &diag_before);
    result = bench_release_pressure(&env, release_iters, page_size);
    bench_run_uhm_case(&env, &result, &diag_before, diagnostics,
                       "check after uhm release pressure benchmark");

    bench_release_nodiag_page_stats_zero_check(&env);
    bench_heap_close(&env);
    return 0;
}
