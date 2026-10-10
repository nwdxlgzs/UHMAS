static bench_result bench_uhm_small(bench_heap_env *env, size_t iterations) {
    enum { slot_count = 128 };
    void *slots[slot_count];
    size_t i;
    uint64_t begin;
    uint64_t end;

    memset(slots, 0, sizeof(slots));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 17u) % slot_count;
        size_t size = 16u + ((i * 13u) % 240u);
        if (slots[slot]) {
            env->api->free(env->heap, slots[slot]);
        }
        slots[slot] = env->api->malloc(env->heap, size);
        bench_expect(slots[slot] != 0, "uhm small malloc");
        memset(slots[slot], (int)(i & 0xffu), size);
    }
    for (i = 0; i < slot_count; ++i) {
        if (slots[i]) {
            env->api->free(env->heap, slots[i]);
        }
    }
    end = bench_now_ns();
    return bench_result_make("uhm small malloc/free", begin, end,
                             iterations + slot_count);
}

static bench_result bench_system_small(size_t iterations) {
    enum { slot_count = 128 };
    void *slots[slot_count];
    size_t i;
    uint64_t begin;
    uint64_t end;

    memset(slots, 0, sizeof(slots));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 17u) % slot_count;
        size_t size = 16u + ((i * 13u) % 240u);
        free(slots[slot]);
        slots[slot] = malloc(size);
        bench_expect(slots[slot] != 0, "system small malloc");
        memset(slots[slot], (int)(i & 0xffu), size);
    }
    for (i = 0; i < slot_count; ++i) {
        free(slots[i]);
    }
    end = bench_now_ns();
    return bench_result_make("system malloc small", begin, end,
                             iterations + slot_count);
}

static int bench_thread_run(bench_thread_args *args) {
    enum { slot_count = 64 };
    void *slots[slot_count];
    size_t i;
    size_t completed_iterations = 0u;

    memset(slots, 0, sizeof(slots));
    if (args->pin_thread) {
        args->affinity_applied =
            bench_pin_current_thread(args->affinity_index);
        if (!args->affinity_applied) {
            return 0;
        }
    } else {
        args->affinity_applied = 1;
    }
    bench_start_gate_arrive_and_wait(args->start_gate);
    bench_worker_profile_begin(args->profile, args->iterations, 1u);
    args->ok = 0;
    for (i = 0; i < args->iterations; ++i) {
        size_t slot = (i * 7u) % slot_count;
        size_t size = 24u + ((i * 19u) % 384u);
        if (args->use_system) {
            free(slots[slot]);
            slots[slot] = malloc(size);
        } else {
            args->env->api->free(args->env->heap, slots[slot]);
            slots[slot] = args->env->api->malloc(args->env->heap, size);
        }
        if (!slots[slot]) {
            goto fail;
        }
        memset(slots[slot], (int)(size & 0xffu), size);
        completed_iterations = i + 1u;
        bench_worker_profile_sample(args->profile, completed_iterations,
                                    args->iterations);
    }
    args->ok = 1;

fail:
    for (i = 0; i < slot_count; ++i) {
        if (args->use_system) {
            free(slots[i]);
        } else {
            args->env->api->free(args->env->heap, slots[i]);
        }
    }
    bench_worker_profile_end(args->profile, completed_iterations);
    return args->ok;
}

#if defined(_WIN32)
static DWORD WINAPI bench_thread_entry(LPVOID user) {
    return bench_thread_run((bench_thread_args *)user) ? 0u : 1u;
}
#else
static void *bench_thread_entry(void *user) {
    (void)bench_thread_run((bench_thread_args *)user);
    return 0;
}
#endif

static bench_result bench_multithread_small_named_ex(bench_heap_env *env,
                                                     size_t iterations,
                                                     int use_system,
                                                     size_t thread_count,
                                                     int pin_threads,
                                                     const char *result_name,
                                                     int profile_workers,
                                                     long double worker_cycles_per_ns) {
    bench_thread_args *args;
    bench_worker_profile *profiles = 0;
    bench_result result;
    size_t i;
    uint64_t begin = 0;
    uint64_t end;
    bench_start_gate start_gate;
#if defined(_WIN32)
    HANDLE *threads;
#else
    pthread_t *threads;
#endif

    bench_expect(thread_count != 0u, "benchmark thread count");
    bench_expect(iterations <= (size_t)-1 / thread_count,
                 "benchmark multithread operations overflow");
    args = (bench_thread_args *)calloc(thread_count, sizeof(args[0]));
    bench_expect(args != 0, "benchmark thread args alloc");
    if (profile_workers) {
        profiles = (bench_worker_profile *)calloc(thread_count,
                                                  sizeof(profiles[0]));
        bench_expect(profiles != 0, "spinlock worker profiles alloc");
    }
#if defined(_WIN32)
    threads = (HANDLE *)calloc(thread_count, sizeof(threads[0]));
#else
    threads = (pthread_t *)calloc(thread_count, sizeof(threads[0]));
#endif
    bench_expect(threads != 0, "benchmark thread handles alloc");
    if (pin_threads) {
        bench_start_gate_init(&start_gate);
    } else {
        begin = bench_now_ns();
    }
    for (i = 0; i < thread_count; ++i) {
        args[i].env = env;
        args[i].start_gate = pin_threads ? &start_gate : 0;
        args[i].iterations = iterations;
        args[i].affinity_index = i;
        args[i].use_system = use_system;
        args[i].pin_thread = pin_threads;
        args[i].profile = profiles ? &profiles[i] : 0;
#if defined(_WIN32)
        threads[i] = CreateThread(0, 0, bench_thread_entry, &args[i], 0, 0);
        bench_expect(threads[i] != 0, "benchmark thread create");
#else
        bench_expect(pthread_create(&threads[i], 0, bench_thread_entry,
                                    &args[i]) == 0,
                     "benchmark thread create");
#endif
    }
    if (pin_threads) {
        bench_start_gate_wait_ready(&start_gate, thread_count);
        begin = bench_now_ns();
        bench_start_gate_release(&start_gate);
    }
    for (i = 0; i < thread_count; ++i) {
#if defined(_WIN32)
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], 0);
#endif
        bench_expect(args[i].ok, "benchmark thread result");
        bench_expect(args[i].affinity_applied,
                     "benchmark thread affinity applied");
    }
    end = bench_now_ns();
    result = bench_result_make(result_name, begin, end,
                               iterations * thread_count);
    if (profiles) {
        bench_result_set_worker_profiles(&result, profiles, thread_count,
                                         worker_cycles_per_ns);
    }
    free(threads);
    free(profiles);
    free(args);
    return result;
}

static bench_result bench_multithread_small_named(bench_heap_env *env,
                                                  size_t iterations,
                                                  int use_system,
                                                  size_t thread_count,
                                                  int pin_threads,
                                                  const char *result_name) {
    return bench_multithread_small_named_ex(env, iterations, use_system,
                                            thread_count, pin_threads,
                                            result_name, 0, 0.0L);
}

static bench_result bench_multithread_small_profiled(bench_heap_env *env,
                                                     size_t iterations,
                                                     int use_system,
                                                     size_t thread_count,
                                                     int pin_threads,
                                                     const char *result_name,
                                                     long double cycles_per_ns) {
    return bench_multithread_small_named_ex(env, iterations, use_system,
                                            thread_count, pin_threads,
                                            result_name, 1, cycles_per_ns);
}

static bench_result bench_multithread_small(bench_heap_env *env,
                                            size_t iterations,
                                            int use_system) {
    return bench_multithread_small_named(
        env, iterations, use_system, 4u, 0,
        use_system ? "system malloc multithread"
                   : "uhm small malloc/free multithread");
}

static int bench_remote_free_run(bench_remote_free_args *args) {
    size_t i;

    args->ok = 0;
    args->affinity_applied = bench_pin_current_thread(args->affinity_index);
    if (!args->affinity_applied) {
        return 0;
    }
    bench_start_gate_arrive_and_wait(args->start_gate);
    bench_worker_profile_begin(args->profile, args->count, 1u);
    for (i = 0; i < args->count; ++i) {
        args->env->api->free(args->env->heap,
                             args->ptrs[args->begin_index + i]);
        args->ptrs[args->begin_index + i] = 0;
        bench_worker_profile_sample(args->profile, i + 1u, args->count);
    }
    bench_worker_profile_end(args->profile, args->count);
    args->ok = 1;
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI bench_remote_free_entry(LPVOID user) {
    return bench_remote_free_run((bench_remote_free_args *)user) ? 0u : 1u;
}
#else
static void *bench_remote_free_entry(void *user) {
    (void)bench_remote_free_run((bench_remote_free_args *)user);
    return 0;
}
#endif

static bench_result bench_false_remote_handoff_named_ex(
    bench_heap_env *env, size_t per_thread, size_t thread_count,
    int reuse_after_free, const char *result_name, int profile_workers,
    long double worker_cycles_per_ns) {
    size_t ptr_count;
    size_t operations;
    bench_remote_free_args *args;
    bench_worker_profile *profiles = 0;
    bench_result result;
    void **ptrs;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_start_gate start_gate;
#if defined(_WIN32)
    HANDLE *threads;
#else
    pthread_t *threads;
#endif

    bench_expect(thread_count != 0u, "false sharing remote thread count");
    bench_expect(per_thread <= (size_t)-1 / thread_count,
                 "false sharing remote ptr count overflow");
    ptr_count = per_thread * thread_count;
    bench_expect(ptr_count <= (size_t)-1 / 3u,
                 "false sharing remote operations overflow");
    operations = reuse_after_free ? ptr_count * 3u : ptr_count;
    args = (bench_remote_free_args *)calloc(thread_count, sizeof(args[0]));
    bench_expect(args != 0, "false sharing remote args alloc");
    if (profile_workers) {
        profiles = (bench_worker_profile *)calloc(thread_count,
                                                  sizeof(profiles[0]));
        bench_expect(profiles != 0, "spinlock remote profiles alloc");
    }
#if defined(_WIN32)
    threads = (HANDLE *)calloc(thread_count, sizeof(threads[0]));
#else
    threads = (pthread_t *)calloc(thread_count, sizeof(threads[0]));
#endif
    bench_expect(threads != 0, "false sharing remote handles alloc");
    ptrs = (void **)calloc(ptr_count, sizeof(ptrs[0]));
    bench_expect(ptrs != 0, "false sharing remote ptr array");
    for (i = 0; i < ptr_count; ++i) {
        ptrs[i] = env->api->malloc(env->heap, 64u + ((i & 3u) * 16u));
        bench_expect(ptrs[i] != 0, "false sharing remote source malloc");
        memset(ptrs[i], (int)(i & 0xffu), 64u);
    }

    bench_start_gate_init(&start_gate);
    for (i = 0; i < thread_count; ++i) {
        args[i].env = env;
        args[i].start_gate = &start_gate;
        args[i].ptrs = ptrs;
        args[i].begin_index = i * per_thread;
        args[i].count = per_thread;
        args[i].affinity_index = i;
        args[i].profile = profiles ? &profiles[i] : 0;
#if defined(_WIN32)
        threads[i] = CreateThread(0, 0, bench_remote_free_entry, &args[i], 0, 0);
        bench_expect(threads[i] != 0, "false sharing remote thread create");
#else
        bench_expect(pthread_create(&threads[i], 0, bench_remote_free_entry,
                                    &args[i]) == 0,
                     "false sharing remote thread create");
#endif
    }
    bench_start_gate_wait_ready(&start_gate, thread_count);
    begin = bench_now_ns();
    bench_start_gate_release(&start_gate);
    for (i = 0; i < thread_count; ++i) {
#if defined(_WIN32)
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], 0);
#endif
        bench_expect(args[i].ok, "false sharing remote thread result");
        bench_expect(args[i].affinity_applied,
                     "false sharing remote affinity applied");
    }
    if (reuse_after_free) {
        for (i = 0; i < ptr_count; ++i) {
            ptrs[i] = env->api->malloc(env->heap, 64u + ((i & 3u) * 16u));
            bench_expect(ptrs[i] != 0, "false sharing remote reuse malloc");
        }
        for (i = 0; i < ptr_count; ++i) {
            env->api->free(env->heap, ptrs[i]);
        }
    }
    end = bench_now_ns();
    result = bench_result_make(result_name, begin, end, operations);
    if (profiles) {
        bench_result_set_worker_profiles(&result, profiles, thread_count,
                                         worker_cycles_per_ns);
    }
    free(ptrs);
    free(threads);
    free(profiles);
    free(args);
    return result;
}

static bench_result bench_false_remote_handoff_named(bench_heap_env *env,
                                                     size_t per_thread,
                                                     size_t thread_count,
                                                     int reuse_after_free,
                                                     const char *result_name) {
    return bench_false_remote_handoff_named_ex(
        env, per_thread, thread_count, reuse_after_free, result_name, 0, 0.0L);
}

static bench_result bench_false_remote_handoff_profiled(
    bench_heap_env *env, size_t per_thread, size_t thread_count,
    int reuse_after_free, const char *result_name,
    long double cycles_per_ns) {
    return bench_false_remote_handoff_named_ex(
        env, per_thread, thread_count, reuse_after_free, result_name, 1,
        cycles_per_ns);
}

static bench_result bench_false_remote_handoff(bench_heap_env *env,
                                               size_t per_thread) {
    return bench_false_remote_handoff_named(
        env, per_thread, 4u, 1, "false-share remote free/reuse");
}

static int bench_page_hotspot_run(bench_page_hotspot_args *args) {
    size_t i;

    args->ok = 0;
    args->affinity_applied = bench_pin_current_thread(args->affinity_index);
    if (!args->affinity_applied) {
        return 0;
    }
    bench_start_gate_arrive_and_wait(args->start_gate);
    bench_worker_profile_begin(args->profile, args->iterations, 2u);
    for (i = 0; i < args->iterations; ++i) {
        void *map = 0;
        if (args->env->api->mmap(args->env->heap, &map, args->page_size,
                                 UHM_PROT_READ | UHM_PROT_WRITE,
                                 UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT) !=
            UHM_OK) {
            return 0;
        }
        if (args->env->api->munmap(args->env->heap, map, args->page_size) !=
            UHM_OK) {
            return 0;
        }
        bench_worker_profile_sample(args->profile, i + 1u,
                                    args->iterations);
    }
    bench_worker_profile_end(args->profile, args->iterations);
    args->ok = 1;
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI bench_page_hotspot_entry(LPVOID user) {
    return bench_page_hotspot_run((bench_page_hotspot_args *)user) ? 0u : 1u;
}
#else
static void *bench_page_hotspot_entry(void *user) {
    (void)bench_page_hotspot_run((bench_page_hotspot_args *)user);
    return 0;
}
#endif

static bench_result bench_false_page_map_hotspot_ex(bench_heap_env *env,
                                                    size_t iterations,
                                                    size_t page_size,
                                                    int profile_workers,
                                                    long double worker_cycles_per_ns) {
    enum { thread_count = 4 };
    bench_page_hotspot_args args[thread_count];
    bench_worker_profile profiles[thread_count];
    bench_result result;
    size_t i;
    uint64_t begin;
    uint64_t end;
    bench_start_gate start_gate;
#if defined(_WIN32)
    HANDLE threads[thread_count];
#else
    pthread_t threads[thread_count];
#endif

    memset(args, 0, sizeof(args));
    memset(profiles, 0, sizeof(profiles));
    bench_start_gate_init(&start_gate);
    for (i = 0; i < thread_count; ++i) {
        args[i].env = env;
        args[i].start_gate = &start_gate;
        args[i].iterations = iterations;
        args[i].page_size = page_size;
        args[i].affinity_index = i;
        args[i].profile = profile_workers ? &profiles[i] : 0;
#if defined(_WIN32)
        threads[i] = CreateThread(0, 0, bench_page_hotspot_entry, &args[i], 0, 0);
        bench_expect(threads[i] != 0, "false sharing page thread create");
#else
        bench_expect(pthread_create(&threads[i], 0, bench_page_hotspot_entry,
                                    &args[i]) == 0,
                     "false sharing page thread create");
#endif
    }
    bench_start_gate_wait_ready(&start_gate, thread_count);
    begin = bench_now_ns();
    bench_start_gate_release(&start_gate);
    for (i = 0; i < thread_count; ++i) {
#if defined(_WIN32)
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], 0);
#endif
        bench_expect(args[i].ok, "false sharing page thread result");
        bench_expect(args[i].affinity_applied,
                     "false sharing page affinity applied");
    }
    end = bench_now_ns();
    result = bench_result_make("false-share page-map mmap/munmap",
                               begin, end, iterations * thread_count * 2u);
    if (profile_workers) {
        bench_result_set_worker_profiles(&result, profiles, thread_count,
                                         worker_cycles_per_ns);
    }
    return result;
}

static bench_result bench_false_page_map_hotspot(bench_heap_env *env,
                                                 size_t iterations,
                                                 size_t page_size) {
    return bench_false_page_map_hotspot_ex(env, iterations, page_size, 0,
                                           0.0L);
}

static bench_result bench_false_page_map_hotspot_profiled(
    bench_heap_env *env, size_t iterations, size_t page_size,
    long double cycles_per_ns) {
    return bench_false_page_map_hotspot_ex(env, iterations, page_size, 1,
                                           cycles_per_ns);
}

static bench_result bench_large_malloc(bench_heap_env *env, size_t iterations) {
    size_t i;
    void *ptr = 0;
    size_t page_size = env->owner.vm.page_size
                           ? env->owner.vm.page_size
                           : uhm_platform_system_page_size();
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = 2u * page_size + ((i % 8u) * page_size);
        ptr = env->api->malloc(env->heap, size);
        bench_expect(ptr != 0, "uhm large malloc");
        memset(ptr, (int)i, size);
        ptr = env->api->realloc(env->heap, ptr, size + page_size);
        bench_expect(ptr != 0, "uhm large realloc");
        memset(ptr, (int)(i + 1u), size + page_size);
        env->api->free(env->heap, ptr);
    }
    end = bench_now_ns();
    return bench_result_make("uhm large malloc/realloc/free", begin, end,
                             iterations * 3u);
}

static bench_result bench_system_large_malloc(size_t iterations) {
    size_t i;
    void *ptr = 0;
    size_t page_size = uhm_platform_system_page_size();
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = 2u * page_size + ((i % 8u) * page_size);
        ptr = malloc(size);
        bench_expect(ptr != 0, "system large malloc");
        memset(ptr, (int)i, size);
        ptr = realloc(ptr, size + page_size);
        bench_expect(ptr != 0, "system large realloc");
        memset(ptr, (int)(i + 1u), size + page_size);
        free(ptr);
    }
    end = bench_now_ns();
    return bench_result_make("system malloc large/realloc/free", begin, end,
                             iterations * 3u);
}

static bench_result bench_uhm_mmap(bench_heap_env *env, size_t iterations,
                                   size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = page_size * (4u + (i % 4u));
        void *map = 0;
        bench_expect_ok(env->api->mmap(env->heap, &map, size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "uhm mmap benchmark");
        memset(map, (int)i, size);
        bench_expect_ok(env->api->munmap(env->heap, map, size),
                        "uhm munmap benchmark");
    }
    end = bench_now_ns();
    return bench_result_make("uhm mmap/munmap large", begin, end,
                             iterations * 2u);
}

static bench_result bench_system_mmap(size_t iterations, size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = page_size * (4u + (i % 4u));
        void *map = bench_os_map(size);
        bench_expect(map != 0, "system map benchmark");
        memset(map, (int)i, size);
        bench_os_unmap(map, size);
    }
    end = bench_now_ns();
    return bench_result_make("system mmap/VirtualAlloc large", begin, end,
                             iterations * 2u);
}

static bench_result bench_fixed_mix(bench_heap_env *env, size_t iterations,
                                    size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *fixed;
        bench_expect_ok(env->api->mmap(env->heap, &map, page_size * 8u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "fixed mix source mmap");
        fixed = (void *)((uintptr_t)map + page_size * 2u);
        bench_expect_ok(env->api->mmap(env->heap, &fixed, page_size * 2u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE | UHM_MAP_FIXED),
                        "fixed mix replace");
        bench_expect_ok(env->api->munmap(env->heap, map, page_size * 8u),
                        "fixed mix cleanup");
    }
    end = bench_now_ns();
    return bench_result_make("uhm mmap fixed/auto mix", begin, end,
                             iterations * 3u);
}

static bench_result bench_mprotect_split(bench_heap_env *env, size_t iterations,
                                         size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *middle;
        bench_expect_ok(env->api->mmap(env->heap, &map, page_size * 6u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mprotect split mmap");
        middle = (void *)((uintptr_t)map + page_size * 2u);
        bench_expect_ok(env->api->mprotect(env->heap, middle, page_size * 2u,
                                           UHM_PROT_READ),
                        "mprotect split read");
        bench_expect_ok(env->api->mprotect(env->heap, middle, page_size * 2u,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "mprotect split write");
        bench_expect_ok(env->api->munmap(env->heap, map, page_size * 6u),
                        "mprotect split cleanup");
    }
    end = bench_now_ns();
    return bench_result_make("uhm mprotect split/merge cost", begin, end,
                             iterations * 4u);
}

static bench_result bench_page_private_restore_free(bench_heap_env *env,
                                                     size_t iterations,
                                                     size_t page_size) {
    const size_t allocation_size = page_size * 2u;
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *ptr = env->api->aligned_alloc(env->heap, page_size,
                                            allocation_size);
        bench_expect(ptr != 0, "page-private restore allocation");
        bench_expect_ok(env->api->mprotect(env->heap, ptr, page_size,
                                           UHM_PROT_READ),
                        "page-private restore protect read");
        env->api->free(env->heap, ptr);
    }
    end = bench_now_ns();
    return bench_result_make("page-private malloc restore/free", begin, end,
                             iterations * 3u);
}

static bench_result bench_mremap_resize(bench_heap_env *env, size_t iterations,
                                        size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    bench_expect(env->api->mremap != 0, "mremap api available");
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *same;

        bench_expect_ok(env->api->mmap(env->heap, &map, page_size * 2u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mremap resize mmap");
        memset(map, (int)i, page_size);
        same = map;
        bench_expect_ok(env->api->mremap(env->heap, &same, page_size + 1u,
                                         page_size * 2u, 0u),
                        "mremap resize same rounded span");
        bench_expect(same == map, "mremap resize no-op keeps address");
        bench_expect_ok(env->api->mremap(env->heap, &same, page_size + 1u,
                                         page_size * 3u,
                                         UHM_REMAP_MAYMOVE),
                        "mremap resize maymove");
        bench_expect(same != map, "mremap resize maymove moves address");
        bench_expect(((unsigned char *)same)[0] == (unsigned char)(i & 0xffu),
                     "mremap resize maymove copies payload");
        bench_expect(env->api->munmap(env->heap, map, page_size) ==
                         UHM_ERR_NOT_MAPPED,
                     "mremap resize maymove old address unmapped");
        bench_expect_ok(env->api->munmap(env->heap, same, page_size * 3u),
                        "mremap resize maymove cleanup");

        map = 0;
        bench_expect_ok(env->api->mmap(env->heap, &map, page_size * 2u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mremap resize second mmap");
        memset(map, (int)(i + 17u), page_size);
        same = map;
        bench_expect_ok(env->api->mremap(env->heap, &same, page_size + 1u,
                                         page_size, 0u),
                        "mremap resize shrink");
        bench_expect(same == map, "mremap resize shrink keeps address");
        bench_expect_ok(env->api->mremap(env->heap, &same, page_size,
                                         page_size + 1u, 0u),
                        "mremap resize grow");
        bench_expect(same == map, "mremap resize grow keeps address");
        bench_expect_ok(env->api->munmap(env->heap, map, page_size + 1u),
                        "mremap resize cleanup");
    }
    end = bench_now_ns();
    return bench_result_make("uhm mremap resize cost", begin, end,
                             iterations * 10u);
}

static bench_result bench_thread_ctx_cache_reuse(bench_heap_env *env,
                                                 size_t iterations,
                                                 size_t page_size) {
    uhm_storage_info ctx_info;
    uhm_thread_config ctx_config;
    uhm_thread_ctx *ctx = 0;
    void *ctx_raw = 0;
    void *ctx_storage;
    size_t i;
    uint64_t begin;
    uint64_t end;

    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.size = sizeof(ctx_info);
    bench_expect_ok(uhm_heap_storage_query(0, &ctx_info),
                    "thread ctx cache storage query");
    ctx_storage = bench_aligned_host_alloc(ctx_info.storage_size,
                                           ctx_info.storage_alignment,
                                           &ctx_raw);
    bench_expect(ctx_storage != 0, "thread ctx cache storage alloc");

    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_storage;
    ctx_config.storage_size = ctx_info.storage_size;
    ctx_config.local_cache_bytes = page_size;
    bench_expect(env->api->thread_ctx_open != 0 &&
                     env->api->thread_ctx_close != 0 &&
                     env->api->malloc_ctx != 0 &&
                     env->api->free_ctx != 0,
                 "thread ctx cache api available");
    bench_expect_ok(env->api->thread_ctx_open(env->heap, &ctx_config, &ctx),
                    "thread ctx cache open");

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *ptr = env->api->malloc_ctx(env->heap, ctx, 64u);
        bench_expect(ptr != 0, "thread ctx cache malloc");
        memset(ptr, (int)(i & 0xffu), 64u);
        env->api->free_ctx(env->heap, ctx, ptr);
    }
    end = bench_now_ns();

    if (env->api->thread_ctx_flush) {
        bench_expect_ok(env->api->thread_ctx_flush(env->heap, ctx),
                        "thread ctx cache flush");
    }
    env->api->thread_ctx_close(env->heap, ctx);
    free(ctx_raw);
    return bench_result_make("uhm thread ctx cache reuse", begin, end,
                             iterations * 2u);
}

static uhm_slab_slot *bench_slab_slot_from_payload(void *ptr) {
    const size_t header_size =
        (sizeof(uhm_slab_slot) + UHM_MIN_ALIGNMENT - 1u) &
        ~(UHM_MIN_ALIGNMENT - 1u);

    return ptr ? (uhm_slab_slot *)((uintptr_t)ptr - header_size) : 0;
}

static size_t bench_local_retained_hint_set_base(const uhm_slab *slab) {
    uintptr_t key;

    bench_expect(slab != 0, "retained-hint collision slab");
    key = (uintptr_t)slab / sizeof(*slab);
    key ^= key >> 7u;
    key ^= (uintptr_t)slab->size_class;
    return (size_t)(key & (UHM_LOCAL_RETAINED_HINT_SET_COUNT - 1u)) *
           UHM_LOCAL_RETAINED_HINT_WAYS;
}

static size_t bench_local_cache_slab_count(uhm_slab_slot *head,
                                           const uhm_slab *slab) {
    size_t count = 0;

    for (; head; head = head->next_free) {
        if (head->slab == slab) {
            ++count;
        }
    }
    return count;
}

static bench_result bench_thread_ctx_retained_hint_collision(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    enum {
        max_allocations = 4096,
        max_slabs = 64,
        required_complete_slabs = UHM_LOCAL_RETAINED_HINT_SET_COUNT + 1u,
        tracked_slots_per_slab = 6
    };
    const size_t request_size = 512u;
    const size_t repetitions = iterations * 16u;
    uhm_storage_info ctx_info;
    uhm_thread_config ctx_config;
    uhm_thread_ctx *ctx = 0;
    void *ctx_raw = 0;
    void *ctx_storage;
    void *allocs[max_allocations] = {0};
    uhm_slab *slabs[max_slabs] = {0};
    size_t slab_counts[max_slabs] = {0};
    size_t slab_indices[max_slabs][tracked_slots_per_slab] = {{0}};
    size_t allocation_count = 0;
    size_t complete_slab_count = 0;
    size_t slab_count = 0;
    size_t anchor_index = SIZE_MAX;
    size_t target_index = SIZE_MAX;
    size_t set_base;
    size_t target_cached_slots;
    size_t i;
    size_t j;
    uint64_t begin;
    uint64_t end;

    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.size = sizeof(ctx_info);
    bench_expect_ok(uhm_heap_storage_query(0, &ctx_info),
                    "retained-hint collision ctx storage query");
    ctx_storage = bench_aligned_host_alloc(ctx_info.storage_size,
                                           ctx_info.storage_alignment,
                                           &ctx_raw);
    bench_expect(ctx_storage != 0,
                 "retained-hint collision ctx storage alloc");
    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_storage;
    ctx_config.storage_size = ctx_info.storage_size;
    ctx_config.local_cache_bytes = page_size * 16u;
    bench_expect_ok(env->api->thread_ctx_open(env->heap, &ctx_config, &ctx),
                    "retained-hint collision ctx open");

    while (allocation_count < max_allocations &&
           complete_slab_count < required_complete_slabs) {
        uhm_slab_slot *slot;
        uhm_slab *slab;
        size_t slab_index;

        allocs[allocation_count] = env->api->malloc(env->heap, request_size);
        bench_expect(allocs[allocation_count] != 0,
                     "retained-hint collision seed malloc");
        slot = bench_slab_slot_from_payload(allocs[allocation_count]);
        slab = slot ? slot->slab : 0;
        bench_expect(slab != 0 && slab->size_class < UHM_SIZE_CLASS_COUNT,
                     "retained-hint collision seed slot");
        for (slab_index = 0; slab_index < slab_count; ++slab_index) {
            if (slabs[slab_index] == slab) {
                break;
            }
        }
        if (slab_index == slab_count) {
            bench_expect(slab_count < max_slabs,
                         "retained-hint collision slab capacity");
            slabs[slab_count++] = slab;
        }
        if (slab_counts[slab_index] < tracked_slots_per_slab) {
            slab_indices[slab_index][slab_counts[slab_index]] =
                allocation_count;
        }
        ++slab_counts[slab_index];
        if (slab_counts[slab_index] == tracked_slots_per_slab) {
            ++complete_slab_count;
        }
        ++allocation_count;
    }
    bench_expect(complete_slab_count >= required_complete_slabs,
                 "retained-hint collision complete slabs");
    for (i = 0; i < slab_count && anchor_index == SIZE_MAX; ++i) {
        if (slab_counts[i] < tracked_slots_per_slab) {
            continue;
        }
        for (j = i + 1u; j < slab_count; ++j) {
            if (slab_counts[j] >= tracked_slots_per_slab &&
                bench_local_retained_hint_set_base(slabs[i]) ==
                    bench_local_retained_hint_set_base(slabs[j])) {
                anchor_index = i;
                target_index = j;
                break;
            }
        }
    }
    bench_expect(anchor_index != SIZE_MAX && target_index != SIZE_MAX,
                 "retained-hint collision pair");
    set_base = bench_local_retained_hint_set_base(slabs[anchor_index]);

    env->api->free_ctx(
        env->heap, ctx,
        allocs[slab_indices[anchor_index][0]]);
    allocs[slab_indices[anchor_index][0]] = 0;
    for (j = 0; j < tracked_slots_per_slab; ++j) {
        env->api->free_ctx(
            env->heap, ctx,
            allocs[slab_indices[target_index][j]]);
        allocs[slab_indices[target_index][j]] = 0;
    }
    bench_expect(UHM_LOCAL_RETAINED_HINT_WAYS == 1u &&
                     ctx->local_retained_hints[set_base].slab ==
                         slabs[anchor_index] &&
                     ctx->local_retained_hints[set_base].slot_count == 1u,
                 "retained-hint collision anchor remains hinted");
    target_cached_slots = bench_local_cache_slab_count(
        ctx->local_cache_heads[slabs[target_index]->size_class],
        slabs[target_index]);
    bench_expect(target_cached_slots == tracked_slots_per_slab,
                 "retained-hint collision target seed count");

    {
        void *warmup = env->api->malloc_ctx(env->heap, ctx, request_size);
        bench_expect(warmup != 0 &&
                         bench_slab_slot_from_payload(warmup)->slab ==
                             slabs[target_index],
                     "retained-hint collision warmup target");
        env->api->free_ctx(env->heap, ctx, warmup);
    }
    begin = bench_now_ns();
    for (i = 0; i < repetitions; ++i) {
        void *ptr = env->api->malloc_ctx(env->heap, ctx, request_size);
        bench_expect(ptr != 0, "retained-hint collision malloc");
        *(volatile unsigned char *)ptr = (unsigned char)i;
        env->api->free_ctx(env->heap, ctx, ptr);
    }
    end = bench_now_ns();

    target_cached_slots = bench_local_cache_slab_count(
        ctx->local_cache_heads[slabs[target_index]->size_class],
        slabs[target_index]);
    bench_expect(ctx->local_retained_hints[set_base].slab ==
                         slabs[anchor_index] &&
                     ctx->local_retained_hints[set_base].slot_count == 1u &&
                     target_cached_slots == tracked_slots_per_slab,
                 "retained-hint collision remains stable");
    printf("metadata retained-hint collision: prepared_proof=%u "
           "hint_count=%u hint_ways=%u collision_set=%zu "
           "anchor_hint_present=1 target_hint_present=0 "
           "target_cached_slots=%zu samples=%zu\n",
           (unsigned)UHMAS_LOCAL_RETAINED_PREPARED_PROOF,
           (unsigned)UHM_LOCAL_RETAINED_HINT_COUNT,
           (unsigned)UHM_LOCAL_RETAINED_HINT_WAYS, set_base,
           target_cached_slots, repetitions);

    bench_expect_ok(env->api->thread_ctx_flush(env->heap, ctx),
                    "retained-hint collision ctx flush");
    for (i = 0; i < allocation_count; ++i) {
        if (allocs[i]) {
            env->api->free(env->heap, allocs[i]);
        }
    }
    env->api->thread_ctx_close(env->heap, ctx);
    free(ctx_raw);
    return bench_result_make(
        "small fastpath ctx retained-hint collision", begin, end,
        repetitions * 2u);
}

static bench_result bench_tls_cache_reuse(bench_heap_env *env,
                                          size_t iterations) {
    void *warmup;
    size_t i;
    uint64_t begin;
    uint64_t end;

    bench_expect(env && env->heap && env->api, "tls cache benchmark heap");
    warmup = env->api->malloc(env->heap, 64u);
    bench_expect(warmup != 0, "tls cache warmup malloc");
    memset(warmup, 0x5a, 64u);
    env->api->free(env->heap, warmup);
    bench_expect(env->heap->core.tls_cache_active_count == 1u &&
                     env->heap->core.tls_cache_head != 0 &&
                     env->heap->core.tls_cache_head->local_cache_count == 1u,
                 "tls cache warmup registered local slot");

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *ptr = env->api->malloc(env->heap, 64u);
        bench_expect(ptr != 0, "tls cache malloc");
        memset(ptr, (int)(i & 0xffu), 64u);
        env->api->free(env->heap, ptr);
    }
    end = bench_now_ns();

    bench_expect(env->heap->core.tls_cache_active_count == 1u &&
                     env->heap->core.tls_cache_head != 0 &&
                     env->heap->core.tls_cache_head->local_cache_count != 0u,
                 "tls cache keeps local slot after reuse");
    return bench_result_make("uhm TLS cache reuse", begin, end,
                             iterations * 2u);
}

static uhm_block *bench_find_block_by_payload(bench_heap_env *env,
                                               const void *ptr) {
    uhm_block *block;
    size_t header =
        (sizeof(uhm_block) + (UHM_MIN_ALIGNMENT - 1u)) &
        ~(size_t)(UHM_MIN_ALIGNMENT - 1u);

    if (!env || !env->heap || !ptr) {
        return 0;
    }
    for (block = env->heap->core.first_block; block; block = block->next_phys) {
        if (block->magic == UHM_BLOCK_MAGIC &&
            (void *)((uintptr_t)block + header) == ptr) {
            return block;
        }
    }
    return 0;
}

static size_t bench_slab_slot_header_size(void) {
    return (sizeof(uhm_slab_slot) + (UHM_MIN_ALIGNMENT - 1u)) &
           ~(size_t)(UHM_MIN_ALIGNMENT - 1u);
}

static uhm_slab *bench_find_slab_by_payload(bench_heap_env *env,
                                             const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    size_t header = bench_slab_slot_header_size();
    uhm_slab *slab;

    if (!env || !env->heap || !ptr) {
        return 0;
    }
    for (slab = env->heap->core.slab_all_head; slab; slab = slab->next_all) {
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

typedef enum bench_slab_slot_hot_field {
    BENCH_SLAB_SLOT_FIELD_STATE = 0,
    BENCH_SLAB_SLOT_FIELD_REMOTE_NEXT = 1,
    BENCH_SLAB_SLOT_FIELD_NEXT_FREE = 2
} bench_slab_slot_hot_field;

typedef struct bench_slab_slot_writer_args {
    uhm_slab_slot *slot;
    bench_slab_slot_hot_field field;
    size_t iterations;
    _Atomic(uint32_t) *ready;
    _Atomic(uint32_t) *start;
    int ok;
} bench_slab_slot_writer_args;

static uhm_slab_slot *bench_slab_slot_from_index(uhm_slab *slab,
                                                 size_t index) {
    if (!slab || index >= slab->total_slots) {
        return 0;
    }
    return (uhm_slab_slot *)(slab->base + index * slab->slot_stride);
}

static uintptr_t bench_slab_slot_field_addr(
    const uhm_slab_slot *slot,
    bench_slab_slot_hot_field field) {
    if (!slot) {
        return 0u;
    }
    switch (field) {
    case BENCH_SLAB_SLOT_FIELD_STATE:
        return (uintptr_t)&slot->state_word;
    case BENCH_SLAB_SLOT_FIELD_REMOTE_NEXT:
        return (uintptr_t)&slot->remote_link.next;
    case BENCH_SLAB_SLOT_FIELD_NEXT_FREE:
        return (uintptr_t)&slot->next_free;
    default:
        return 0u;
    }
}

static uint64_t bench_slab_slot_field_line(
    const uhm_slab_slot *slot,
    bench_slab_slot_hot_field field) {
    return (uint64_t)(bench_slab_slot_field_addr(slot, field) /
                      (uintptr_t)UHM_CACHELINE_SIZE);
}

static int bench_slab_slot_fields_share_line(
    const uhm_slab_slot *left,
    bench_slab_slot_hot_field left_field,
    const uhm_slab_slot *right,
    bench_slab_slot_hot_field right_field) {
    if (!left || !right) {
        return 0;
    }
    return bench_slab_slot_field_line(left, left_field) ==
           bench_slab_slot_field_line(right, right_field);
}

static int bench_slab_slot_find_hot_overlap(
    const uhm_slab_slot *left,
    const uhm_slab_slot *right,
    bench_slab_slot_hot_field *left_field,
    bench_slab_slot_hot_field *right_field) {
    static const bench_slab_slot_hot_field fields[] = {
        BENCH_SLAB_SLOT_FIELD_STATE,
        BENCH_SLAB_SLOT_FIELD_REMOTE_NEXT,
        BENCH_SLAB_SLOT_FIELD_NEXT_FREE,
    };
    size_t i;
    size_t j;

    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        for (j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j) {
            if (bench_slab_slot_fields_share_line(left, fields[i], right,
                                                  fields[j])) {
                *left_field = fields[i];
                *right_field = fields[j];
                return 1;
            }
        }
    }
    *left_field = BENCH_SLAB_SLOT_FIELD_STATE;
    *right_field = BENCH_SLAB_SLOT_FIELD_STATE;
    return 0;
}

static void bench_slab_slot_field_store_same(
    uhm_slab_slot *slot,
    bench_slab_slot_hot_field field) {
    uint64_t state_word;
    uhm_tagged_freelist_node *remote_next;
    uhm_slab_slot *next_free;

    switch (field) {
    case BENCH_SLAB_SLOT_FIELD_STATE:
        state_word = atomic_load_explicit(&slot->state_word,
                                          UHM_ATOMIC_RELAXED);
        atomic_store_explicit(&slot->state_word, state_word,
                              UHM_ATOMIC_RELAXED);
        break;
    case BENCH_SLAB_SLOT_FIELD_REMOTE_NEXT:
        remote_next = atomic_load_explicit(&slot->remote_link.next,
                                           UHM_ATOMIC_RELAXED);
        atomic_store_explicit(&slot->remote_link.next, remote_next,
                              UHM_ATOMIC_RELAXED);
        break;
    case BENCH_SLAB_SLOT_FIELD_NEXT_FREE:
        next_free = slot->next_free;
        *((uhm_slab_slot * volatile *)&slot->next_free) = next_free;
        break;
    default:
        break;
    }
}

static int bench_slab_slot_writer_run(bench_slab_slot_writer_args *args) {
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
        bench_slab_slot_field_store_same(args->slot, args->field);
    }
    args->ok = 1;
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI bench_slab_slot_writer_entry(LPVOID user) {
    return bench_slab_slot_writer_run((bench_slab_slot_writer_args *)user) ? 0u
                                                                          : 1u;
}
#else
static void *bench_slab_slot_writer_entry(void *user) {
    (void)bench_slab_slot_writer_run((bench_slab_slot_writer_args *)user);
    return 0;
}
#endif

static bench_result bench_slab_slot_writer_pair(
    const char *name,
    uhm_slab_slot *left,
    bench_slab_slot_hot_field left_field,
    uhm_slab_slot *right,
    bench_slab_slot_hot_field right_field,
    size_t iterations) {
    bench_slab_slot_writer_args args[2];
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
    args[0].slot = left;
    args[0].field = left_field;
    args[0].iterations = iterations;
    args[0].ready = &ready;
    args[0].start = &start;
    args[1].slot = right;
    args[1].field = right_field;
    args[1].iterations = iterations;
    args[1].ready = &ready;
    args[1].start = &start;

#if defined(_WIN32)
    threads[0] = CreateThread(0, 0, bench_slab_slot_writer_entry, &args[0], 0,
                              0);
    threads[1] = CreateThread(0, 0, bench_slab_slot_writer_entry, &args[1], 0,
                              0);
    bench_expect(threads[0] != 0 && threads[1] != 0,
                 "slab slot sharing thread create");
#else
    bench_expect(pthread_create(&threads[0], 0, bench_slab_slot_writer_entry,
                                &args[0]) == 0,
                 "slab slot sharing thread create");
    bench_expect(pthread_create(&threads[1], 0, bench_slab_slot_writer_entry,
                                &args[1]) == 0,
                 "slab slot sharing thread create");
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
    bench_expect(args[0].ok && args[1].ok, "slab slot sharing thread result");
    return bench_result_make(name, begin, end, iterations * 2u);
}

static int bench_slab_slot_find_far_index(
    uhm_slab *slab,
    size_t adjacent_left_index,
    bench_slab_slot_hot_field left_field,
    bench_slab_slot_hot_field right_field,
    size_t *far_index) {
    uhm_slab_slot *left = bench_slab_slot_from_index(slab,
                                                     adjacent_left_index);
    size_t i;

    for (i = 0; i < slab->total_slots; ++i) {
        uhm_slab_slot *candidate;
        if (i == adjacent_left_index || i == adjacent_left_index + 1u) {
            continue;
        }
        candidate = bench_slab_slot_from_index(slab, i);
        if (!bench_slab_slot_fields_share_line(left, left_field, candidate,
                                               right_field)) {
            *far_index = i;
            return 1;
        }
    }
    return 0;
}

static void bench_slab_slot_sharing_case(bench_heap_env *env,
                                         size_t request,
                                         size_t iterations,
                                         int diagnostics) {
    enum { max_ptrs = 1024 };
    void **ptrs;
    uhm_slab *slab;
    size_t ptr_count = 0;
    size_t same_slab_count = 0;
    size_t i;
    size_t adjacent_left_index = 0;
    size_t far_index = 0;
    int hot_overlap = 0;
    int far_available;
    bench_slab_slot_hot_field left_field =
        BENCH_SLAB_SLOT_FIELD_STATE;
    bench_slab_slot_hot_field right_field =
        BENCH_SLAB_SLOT_FIELD_STATE;
    uhm_slab_slot *left;
    uhm_slab_slot *adjacent_right;
    uhm_slab_slot *far_right;
    char adjacent_name[96];
    char far_name[96];
    char check_message[128];
    bench_result result;
    bench_diag_snapshot diag_before;

    ptrs = (void **)calloc(max_ptrs, sizeof(ptrs[0]));
    bench_expect(ptrs != 0, "slab slot sharing ptr array");
    ptrs[ptr_count] = env->api->malloc(env->heap, request);
    bench_expect(ptrs[ptr_count] != 0, "slab slot sharing first malloc");
    slab = bench_find_slab_by_payload(env, ptrs[ptr_count]);
    bench_expect(slab != 0 && slab->total_slots >= 2u,
                 "slab slot sharing slab");
    ++ptr_count;
    same_slab_count = 1u;

    while (ptr_count < max_ptrs && same_slab_count < slab->total_slots) {
        uhm_slab *current_slab;
        ptrs[ptr_count] = env->api->malloc(env->heap, request);
        bench_expect(ptrs[ptr_count] != 0, "slab slot sharing malloc");
        current_slab = bench_find_slab_by_payload(env, ptrs[ptr_count]);
        if (current_slab == slab) {
            ++same_slab_count;
        }
        ++ptr_count;
    }
    bench_expect(same_slab_count == slab->total_slots,
                 "slab slot sharing filled first slab");

    for (i = 0; i + 1u < slab->total_slots; ++i) {
        uhm_slab_slot *candidate_left = bench_slab_slot_from_index(slab, i);
        uhm_slab_slot *candidate_right =
            bench_slab_slot_from_index(slab, i + 1u);
        if (bench_slab_slot_find_hot_overlap(candidate_left, candidate_right,
                                             &left_field, &right_field)) {
            adjacent_left_index = i;
            hot_overlap = 1;
            break;
        }
    }
    if (!hot_overlap) {
        (void)bench_slab_slot_find_hot_overlap(
            bench_slab_slot_from_index(slab, 0u),
            bench_slab_slot_from_index(slab, 1u), &left_field, &right_field);
        adjacent_left_index = 0u;
    }

    far_available = bench_slab_slot_find_far_index(
        slab, adjacent_left_index, left_field, right_field, &far_index);
    if (!far_available) {
        far_index = adjacent_left_index + 1u;
    }
    left = bench_slab_slot_from_index(slab, adjacent_left_index);
    adjacent_right = bench_slab_slot_from_index(slab,
                                               adjacent_left_index + 1u);
    far_right = bench_slab_slot_from_index(slab, far_index);

    printf("metadata slab-slot-sharing: request=%zu slot_stride=%zu "
           "slot_size=%zu total_slots=%zu adjacent_left_index=%zu "
           "adjacent_right_index=%zu far_index=%zu left_field=%u "
           "right_field=%u hot_line_overlap=%u far_available=%u "
           "adjacent_left_line=%llu adjacent_right_line=%llu "
           "far_right_line=%llu state_word_mod_cacheline=%zu "
           "remote_link_mod_cacheline=%zu next_free_mod_cacheline=%zu\n",
           request, slab->slot_stride, slab->slot_size, slab->total_slots,
           adjacent_left_index, adjacent_left_index + 1u, far_index,
           (unsigned)left_field, (unsigned)right_field,
           (unsigned)hot_overlap, (unsigned)far_available,
           (unsigned long long)bench_slab_slot_field_line(left, left_field),
           (unsigned long long)bench_slab_slot_field_line(adjacent_right,
                                                          right_field),
           (unsigned long long)bench_slab_slot_field_line(far_right,
                                                          right_field),
           offsetof(uhm_slab_slot, state_word) % UHM_CACHELINE_SIZE,
           offsetof(uhm_slab_slot, remote_link) % UHM_CACHELINE_SIZE,
           offsetof(uhm_slab_slot, next_free) % UHM_CACHELINE_SIZE);

    snprintf(adjacent_name, sizeof(adjacent_name),
             "slab-slot hot-field adjacent req=%zu", request);
    bench_diag_capture(env, &diag_before);
    result = bench_slab_slot_writer_pair(adjacent_name, left, left_field,
                                         adjacent_right, right_field,
                                         iterations);
    snprintf(check_message, sizeof(check_message),
             "check after slab slot adjacent req=%zu", request);
    bench_run_uhm_case(env, &result, &diag_before, diagnostics,
                       check_message);

    if (far_available) {
        snprintf(far_name, sizeof(far_name),
                 "slab-slot hot-field far req=%zu", request);
        bench_diag_capture(env, &diag_before);
        result = bench_slab_slot_writer_pair(far_name, left, left_field,
                                             far_right, right_field,
                                             iterations);
        snprintf(check_message, sizeof(check_message),
                 "check after slab slot far req=%zu", request);
        bench_run_uhm_case(env, &result, &diag_before, diagnostics,
                           check_message);
    }

    for (i = 0; i < ptr_count; ++i) {
        env->api->free(env->heap, ptrs[i]);
    }
    free(ptrs);
    bench_check_heap(env, "check after slab slot sharing cleanup");
}

static void bench_slab_slot_sharing_matrix(bench_heap_env *env,
                                           size_t iterations,
                                           int diagnostics) {
    static const size_t requests[] = {
        16u, 32u, 64u, 128u, 256u, 512u,
    };
    size_t i;

    for (i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        bench_slab_slot_sharing_case(env, requests[i], iterations,
                                     diagnostics);
    }
}

static bench_result bench_live_block_fast_lookup(bench_heap_env *env,
                                                 size_t iterations,
                                                 size_t page_size) {
    void *ptr;
    size_t size = page_size * 2u + 128u;
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    ptr = env->api->malloc(env->heap, size);
    bench_expect(ptr != 0, "live-block fast lookup malloc");
    for (i = 0; i < iterations; ++i) {
        bench_expect(env->api->malloc_usable_size(env->heap, ptr) >= size,
                     "live-block fast lookup usable");
    }
    env->api->free(env->heap, ptr);
    end = bench_now_ns();
    return bench_result_make("live-block valid fast lookup", begin, end,
                             iterations + 2u);
}

static bench_result bench_live_block_descriptor_slow(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    void *ptr;
    uhm_block *block;
    uhm_extent_descriptor saved_desc;
    size_t usable;
    size_t size = page_size * 2u + 128u;
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    ptr = env->api->malloc(env->heap, size);
    bench_expect(ptr != 0, "live-block descriptor slow malloc");
    block = bench_find_block_by_payload(env, ptr);
    bench_expect(block != 0, "live-block descriptor slow block");
    for (i = 0; i < iterations; ++i) {
        saved_desc = block->desc;
        block->desc.magic ^= 0x5a5a5a5au;
        usable = env->api->malloc_usable_size(env->heap, ptr);
        block->desc = saved_desc;
        bench_expect(usable >= size, "live-block descriptor slow usable");
    }
    env->api->free(env->heap, ptr);
    end = bench_now_ns();
    return bench_result_make("live-block descriptor slow lookup", begin, end,
                             iterations + 2u);
}

static bench_result bench_live_block_invalid_lookup(bench_heap_env *env,
                                                    size_t iterations) {
    size_t i;
    uint64_t begin;
    uint64_t end;

    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        uintptr_t fake = (uintptr_t)&i + (uintptr_t)((i & 7u) + 1u);
        bench_expect(env->api->malloc_usable_size(env->heap,
                                                  (const void *)fake) == 0,
                     "live-block invalid lookup rejected");
    }
    end = bench_now_ns();
    return bench_result_make("live-block invalid pointer miss", begin, end,
                             iterations);
}

static bench_result bench_live_block_fragmented_slow(
    bench_heap_env *env, size_t iterations, size_t page_size) {
    const size_t block_count = iterations > 12000u ? 1024u : 256u;
    void **ptrs;
    uhm_block *target_block;
    uhm_extent_descriptor saved_desc;
    size_t size = page_size * 2u + 128u;
    size_t i;
    uint64_t begin;
    uint64_t end;

    ptrs = (void **)calloc(block_count, sizeof(ptrs[0]));
    bench_expect(ptrs != 0, "live-block fragmented ptrs");
    begin = bench_now_ns();
    for (i = 0; i < block_count; ++i) {
        ptrs[i] = env->api->malloc(env->heap, size + ((i & 3u) * 64u));
        bench_expect(ptrs[i] != 0, "live-block fragmented malloc");
        memset(ptrs[i], (int)i, 64u);
    }
    target_block = bench_find_block_by_payload(env, ptrs[block_count - 1u]);
    bench_expect(target_block != 0, "live-block fragmented target block");
    for (i = 0; i < iterations; ++i) {
        saved_desc = target_block->desc;
        target_block->desc.magic ^= 0x5a5a5a5au;
        bench_expect(env->api->malloc_usable_size(env->heap,
                                                  ptrs[block_count - 1u]) >=
                         size,
                     "live-block fragmented slow usable");
        target_block->desc = saved_desc;
    }
    for (i = 0; i < block_count; ++i) {
        env->api->free(env->heap, ptrs[i]);
    }
    end = bench_now_ns();
    free(ptrs);
    return bench_result_make("live-block fragmented slow lifecycle",
                             begin, end, iterations + (block_count * 2u));
}

