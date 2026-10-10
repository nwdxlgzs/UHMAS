static void *bench_aligned_host_alloc(size_t size, size_t alignment,
                                      void **raw_out) {
    void *raw;
    uintptr_t addr;
    uintptr_t aligned;
    size_t total;

    if (!raw_out || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1u)) != 0u) {
        return 0;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    if (size > (size_t)-1 - alignment) {
        return 0;
    }
    total = size + alignment - 1u;
    raw = malloc(total);
    if (!raw) {
        return 0;
    }
    addr = (uintptr_t)raw;
    aligned = (addr + (uintptr_t)alignment - 1u) &
              ~((uintptr_t)alignment - 1u);
    *raw_out = raw;
    return (void *)aligned;
}

static void bench_fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void bench_expect(int value, const char *message) {
    if (!value) {
        bench_fail(message);
    }
}

static void bench_expect_ok(uhm_result result, const char *message) {
    if (result != UHM_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static uint64_t bench_now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    uint64_t ticks;
    uint64_t freq;
    uint64_t seconds;
    uint64_t remainder;
    uint64_t fraction;

    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    if (counter.QuadPart < 0 || frequency.QuadPart <= 0) {
        return 0u;
    }
    ticks = (uint64_t)counter.QuadPart;
    freq = (uint64_t)frequency.QuadPart;
    seconds = ticks / freq;
    remainder = ticks % freq;
    if (seconds > UINT64_MAX / UINT64_C(1000000000)) {
        return UINT64_MAX;
    }
    if (remainder <= UINT64_MAX / UINT64_C(1000000000)) {
        fraction = (remainder * UINT64_C(1000000000)) / freq;
    } else {
        fraction = (uint64_t)(((long double)remainder *
                               1000000000.0L) /
                              (long double)freq);
    }
    return seconds * UINT64_C(1000000000) + fraction;
#else
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        timespec_get(&ts, TIME_UTC);
    }
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t bench_process_cpu_now_ns(void) {
#if defined(_WIN32)
    FILETIME create_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    ULARGE_INTEGER kernel_ticks;
    ULARGE_INTEGER user_ticks;

    if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time,
                         &kernel_time, &user_time)) {
        return 0;
    }
    kernel_ticks.LowPart = kernel_time.dwLowDateTime;
    kernel_ticks.HighPart = kernel_time.dwHighDateTime;
    user_ticks.LowPart = user_time.dwLowDateTime;
    user_ticks.HighPart = user_time.dwHighDateTime;
    return (kernel_ticks.QuadPart + user_ticks.QuadPart) * UINT64_C(100);
#else
#if defined(CLOCK_PROCESS_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
        return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
               (uint64_t)ts.tv_nsec;
    }
#endif
    {
        clock_t ticks = clock();
        if (ticks == (clock_t)-1) {
            return 0;
        }
        return (uint64_t)(((long double)ticks * 1000000000.0L) /
                          (long double)CLOCKS_PER_SEC);
    }
#endif
}

static uint64_t bench_process_cpu_work_now(uint32_t *is_cycles) {
#if defined(_WIN32)
    ULONG64 cycles = 0;
    if (is_cycles) {
        *is_cycles = 1u;
    }
    if (!QueryProcessCycleTime(GetCurrentProcess(), &cycles)) {
        return 0;
    }
    return (uint64_t)cycles;
#else
    if (is_cycles) {
        *is_cycles = 0u;
    }
    return bench_process_cpu_now_ns();
#endif
}

static uint64_t bench_thread_cpu_work_now(uint32_t *is_cycles,
                                          uint32_t *valid) {
#if defined(_WIN32)
    ULONG64 cycles = 0;

    if (is_cycles) {
        *is_cycles = 1u;
    }
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        if (valid) {
            *valid = 0u;
        }
        return 0;
    }
    if (valid) {
        *valid = 1u;
    }
    return (uint64_t)cycles;
#else
#if defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
        if (is_cycles) {
            *is_cycles = 0u;
        }
        if (valid) {
            *valid = 1u;
        }
        return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
               (uint64_t)ts.tv_nsec;
    }
#endif
    if (is_cycles) {
        *is_cycles = 0u;
    }
    if (valid) {
        *valid = 0u;
    }
    return 0;
#endif
}

static long double bench_thread_cpu_cycles_per_ns(void) {
#if defined(_WIN32)
    ULONG64 begin_cycles = 0;
    ULONG64 end_cycles = 0;
    uint64_t begin_ns;
    uint64_t end_ns;
    volatile uint64_t sink = 0u;

    if (!QueryThreadCycleTime(GetCurrentThread(), &begin_cycles)) {
        return 0.0L;
    }
    begin_ns = bench_now_ns();
    while (bench_now_ns() - begin_ns < UINT64_C(5000000)) {
        sink = sink * UINT64_C(33) + UINT64_C(17);
    }
    end_ns = bench_now_ns();
    if (!QueryThreadCycleTime(GetCurrentThread(), &end_cycles) ||
        end_cycles <= begin_cycles || end_ns <= begin_ns || sink == 0u) {
        return 0.0L;
    }
    return (long double)(end_cycles - begin_cycles) /
           (long double)(end_ns - begin_ns);
#else
    return 0.0L;
#endif
}

static size_t bench_logical_cpu_count(void) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors != 0 ?
               (size_t)info.dwNumberOfProcessors : 1u;
#elif defined(_SC_NPROCESSORS_ONLN)
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (size_t)count : 1u;
#else
    return 1u;
#endif
}

static void bench_print_scheduler_state(void) {
    size_t logical_cpus = bench_logical_cpu_count();
    size_t affinity_cpus = 0;
    uint64_t process_priority_class = 0;
    uint64_t thread_priority = 0;
    uint64_t scheduler_policy = 0;
    uint64_t scheduler_priority = 0;
    uint32_t affinity_query_ok = 0u;
    uint32_t priority_query_ok = 0u;

#if defined(_WIN32)
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    DWORD priority_class;
    int priority;
    size_t bit;

    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask,
                               &system_mask)) {
        for (bit = 0; bit < sizeof(process_mask) * 8u; ++bit) {
            if ((process_mask & ((DWORD_PTR)1u << bit)) != 0u) {
                ++affinity_cpus;
            }
        }
        affinity_query_ok = 1u;
    }
    priority_class = GetPriorityClass(GetCurrentProcess());
    priority = GetThreadPriority(GetCurrentThread());
    if (priority_class != 0u && priority != THREAD_PRIORITY_ERROR_RETURN) {
        process_priority_class = (uint64_t)priority_class;
        thread_priority = (uint64_t)(priority < 0 ? 0 : priority);
        scheduler_priority = thread_priority;
        priority_query_ok = 1u;
    }
#else
    cpu_set_t set;
    int policy;
    struct sched_param param;

    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        affinity_cpus = (size_t)CPU_COUNT(&set);
        affinity_query_ok = 1u;
    }
    policy = sched_getscheduler(0);
    memset(&param, 0, sizeof(param));
    if (policy >= 0 && sched_getparam(0, &param) == 0) {
        scheduler_policy = (uint64_t)policy;
        scheduler_priority = (uint64_t)(param.sched_priority < 0
                                            ? 0
                                            : param.sched_priority);
        thread_priority = scheduler_priority;
        priority_query_ok = 1u;
    }
#endif

    printf("metadata spinlock-scheduler-state: "
           "logical_cpus=%llu affinity_cpus=%llu "
           "affinity_query_ok=%u priority_query_ok=%u "
           "scheduler_policy=%llu scheduler_priority=%llu "
           "process_priority_class=%llu thread_priority=%llu\n",
           (unsigned long long)logical_cpus,
           (unsigned long long)affinity_cpus,
           (unsigned)affinity_query_ok,
           (unsigned)priority_query_ok,
           (unsigned long long)scheduler_policy,
           (unsigned long long)scheduler_priority,
           (unsigned long long)process_priority_class,
           (unsigned long long)thread_priority);
}

static uint64_t bench_ratio_ppm(uint64_t numerator, uint64_t denominator) {
    long double ratio;

    if (denominator == 0) {
        return 0;
    }
    ratio = ((long double)numerator * 1000000.0L) /
            (long double)denominator;
    if (ratio >= (long double)UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint64_t)(ratio + 0.5L);
}

static uint64_t bench_ratio_ppm_zero_equal(uint64_t numerator,
                                           uint64_t denominator) {
    if (denominator == 0u) {
        return numerator == 0u ? UINT64_C(1000000)
                               : (uint64_t)INT64_MAX;
    }
    return bench_ratio_ppm(numerator, denominator);
}

static uint32_t bench_next_random(uint64_t *state) {
    uint64_t value = *state;
    if (value == 0) {
        value = UINT64_C(0x9e3779b97f4a7c15);
    }
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return (uint32_t)((value * UINT64_C(2685821657736338717)) >> 32);
}

static bench_result bench_result_make(const char *name, uint64_t begin_ns,
                                      uint64_t end_ns, size_t operations) {
    bench_result result;
    memset(&result, 0, sizeof(result));
    result.name = name;
    result.wall_time_ns = end_ns - begin_ns;
    result.seconds = (double)(end_ns - begin_ns) / 1000000000.0;
    result.operations = operations;
    return result;
}

static void bench_result_set_cpu_work(bench_result *result,
                                      uint64_t begin_work,
                                      uint64_t end_work,
                                      uint32_t is_cycles) {
    if (!result || end_work < begin_work) {
        return;
    }
    result->cpu_work = end_work - begin_work;
    result->cpu_work_valid = 1u;
    result->cpu_work_is_cycles = is_cycles ? 1u : 0u;
}

static int bench_pin_current_thread(size_t affinity_index) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    DWORD cpu_count;
    DWORD_PTR mask;

    GetSystemInfo(&info);
    cpu_count = info.dwNumberOfProcessors;
    if (cpu_count == 0 || cpu_count > sizeof(DWORD_PTR) * 8u) {
        return 0;
    }
    mask = (DWORD_PTR)1u << (affinity_index % cpu_count);
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t set;

    if (cpu_count <= 0) {
        return 0;
    }
    CPU_ZERO(&set);
    CPU_SET((int)(affinity_index % (size_t)cpu_count), &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void)affinity_index;
    return 1;
#endif
}

static void bench_thread_yield(void) {
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}

static void bench_start_gate_init(bench_start_gate *gate) {
    atomic_init(&gate->ready, 0u);
    atomic_init(&gate->start, 0u);
}

static void bench_start_gate_arrive_and_wait(bench_start_gate *gate) {
    if (!gate) {
        return;
    }
    atomic_fetch_add_explicit(&gate->ready, 1u, UHM_ATOMIC_ACQ_REL);
    while (atomic_load_explicit(&gate->start, UHM_ATOMIC_ACQUIRE) == 0u) {
        bench_thread_yield();
    }
}

static void bench_start_gate_wait_ready(bench_start_gate *gate,
                                        size_t thread_count) {
    while ((size_t)atomic_load_explicit(&gate->ready, UHM_ATOMIC_ACQUIRE) !=
           thread_count) {
        bench_thread_yield();
    }
}

static void bench_start_gate_release(bench_start_gate *gate) {
    atomic_store_explicit(&gate->start, 1u, UHM_ATOMIC_RELEASE);
}

static int bench_u64_compare(const void *a, const void *b) {
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;

    if (av < bv) {
        return -1;
    }
    if (av > bv) {
        return 1;
    }
    return 0;
}

static uint64_t bench_latency_percentile(const uint64_t *sorted,
                                         size_t count,
                                         unsigned percentile) {
    size_t index;

    if (!sorted || count == 0) {
        return 0;
    }
    if (percentile >= 100u) {
        return sorted[count - 1u];
    }
    index = ((count - 1u) * (size_t)percentile + 99u) / 100u;
    if (index >= count) {
        index = count - 1u;
    }
    return sorted[index];
}

static uint64_t bench_div_ceil_u64(uint64_t value, uint64_t divisor) {
    if (divisor == 0) {
        return 0;
    }
    return (value / divisor) + ((value % divisor) != 0u ? 1u : 0u);
}

static void bench_result_set_latency(bench_result *result,
                                     uint64_t *samples,
                                     size_t sample_count) {
    if (!result || !samples || sample_count == 0) {
        return;
    }
    qsort(samples, sample_count, sizeof(samples[0]), bench_u64_compare);
    result->latency_sample_count = (uint64_t)sample_count;
    result->latency_min_ns = samples[0];
    result->latency_p50_ns = bench_latency_percentile(samples, sample_count, 50u);
    result->latency_p95_ns = bench_latency_percentile(samples, sample_count, 95u);
    result->latency_p99_ns = bench_latency_percentile(samples, sample_count, 99u);
    result->latency_max_ns = samples[sample_count - 1u];
}

static void bench_result_set_latency_units(bench_result *result,
                                           uint64_t work_units_per_sample,
                                           uint64_t pages_per_sample) {
    if (!result || result->latency_sample_count == 0) {
        return;
    }
    result->latency_work_units_per_sample = work_units_per_sample;
    if (work_units_per_sample != 0) {
        result->latency_p95_per_work_unit_ns =
            bench_div_ceil_u64(result->latency_p95_ns, work_units_per_sample);
        result->latency_p99_per_work_unit_ns =
            bench_div_ceil_u64(result->latency_p99_ns, work_units_per_sample);
        result->latency_max_per_work_unit_ns =
            bench_div_ceil_u64(result->latency_max_ns, work_units_per_sample);
    }
    result->latency_pages_per_sample = pages_per_sample;
    if (pages_per_sample != 0) {
        result->latency_p95_per_page_ns =
            bench_div_ceil_u64(result->latency_p95_ns, pages_per_sample);
        result->latency_p99_per_page_ns =
            bench_div_ceil_u64(result->latency_p99_ns, pages_per_sample);
        result->latency_max_per_page_ns =
            bench_div_ceil_u64(result->latency_max_ns, pages_per_sample);
    }
}

static void bench_print(const bench_result *result) {
    double ns_per_op = 0.0;
    if (result->operations != 0 && result->seconds > 0.0) {
        ns_per_op = (result->seconds * 1000000000.0) /
                    (double)result->operations;
    }
    printf("%-38s %10.6f s  %10zu ops  %10.1f ns/op\n",
           result->name, result->seconds, result->operations, ns_per_op);
    if (result->cpu_work_valid || result->worker_runtime_valid) {
        printf("metadata runtime %s: operations=%llu wall_ns=%llu "
               "cpu_work=%llu cpu_work_is_cycles=%u",
               result->name,
               (unsigned long long)result->operations,
               (unsigned long long)result->wall_time_ns,
               (unsigned long long)result->cpu_work,
               result->cpu_work_is_cycles);
        if (result->worker_runtime_valid) {
            printf(" worker_thread_count=%llu worker_cpu_work=%llu "
                   "worker_cpu_work_is_cycles=%u worker_cpu_ns=%llu "
                   "worker_wall_ns=%llu worker_wait_ns=%llu "
                   "worker_max_wait_ns=%llu worker_cpu_ratio_ppm=%llu "
                   "worker_wait_ratio_ppm=%llu",
                   (unsigned long long)result->worker_thread_count,
                   (unsigned long long)result->worker_cpu_work,
                   result->worker_cpu_work_is_cycles,
                   (unsigned long long)result->worker_cpu_ns,
                   (unsigned long long)result->worker_wall_ns,
                   (unsigned long long)result->worker_wait_ns,
                   (unsigned long long)result->worker_max_wait_ns,
                   (unsigned long long)result->worker_cpu_ratio_ppm,
                   (unsigned long long)result->worker_wait_ratio_ppm);
        }
        printf("\n");
    }
    if (result->latency_sample_count != 0) {
        printf("latency|%s|samples=%llu min_ns=%llu p50_ns=%llu "
               "p95_ns=%llu p99_ns=%llu max_ns=%llu",
               result->name,
               (unsigned long long)result->latency_sample_count,
               (unsigned long long)result->latency_min_ns,
               (unsigned long long)result->latency_p50_ns,
               (unsigned long long)result->latency_p95_ns,
               (unsigned long long)result->latency_p99_ns,
               (unsigned long long)result->latency_max_ns);
        if (result->latency_work_units_per_sample != 0) {
            printf(" work_units_per_sample=%llu "
                   "p95_per_work_unit_ns=%llu "
                   "p99_per_work_unit_ns=%llu "
                   "max_per_work_unit_ns=%llu",
                   (unsigned long long)result->latency_work_units_per_sample,
                   (unsigned long long)
                       result->latency_p95_per_work_unit_ns,
                   (unsigned long long)
                       result->latency_p99_per_work_unit_ns,
                   (unsigned long long)
                       result->latency_max_per_work_unit_ns);
        }
        if (result->latency_pages_per_sample != 0) {
            printf(" pages_per_sample=%llu "
                   "p95_per_page_ns=%llu "
                   "p99_per_page_ns=%llu "
                   "max_per_page_ns=%llu",
                   (unsigned long long)result->latency_pages_per_sample,
                   (unsigned long long)result->latency_p95_per_page_ns,
                   (unsigned long long)result->latency_p99_per_page_ns,
                   (unsigned long long)result->latency_max_per_page_ns);
        }
        printf("\n");
    }
}

static void bench_check_heap(bench_heap_env *env, const char *message) {
    bench_expect_ok(env->api->check(env->heap, 0), message);
}

static int bench_stress_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_STRESS");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_false_sharing_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_FALSE_SHARING");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_small_fast_path_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_SMALL_FAST_PATH");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_release_breakdown_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_RELEASE_BREAKDOWN");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_release_pressure_matrix_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_RELEASE_PRESSURE_MATRIX");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_slab_empty_churn_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_SLAB_EMPTY_CHURN");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_heap_close_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_HEAP_CLOSE");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_mmap_locality_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_MMAP_LOCALITY");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_control_allocator_churn_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_CONTROL_ALLOCATOR_CHURN");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_live_block_lookup_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_LIVE_BLOCK_LOOKUP");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_treap_adversarial_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_TREAP_ADVERSARIAL");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_mprotect_multi_vma_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_MPROTECT_MULTI_VMA");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_munmap_multi_vma_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_MUNMAP_MULTI_VMA");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_spinlock_thresholds_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_SPINLOCK_THRESHOLDS");
    return value && value[0] != '\0' && value[0] != '0';
}

static size_t bench_sample_index(void) {
    const char *value = getenv("UHMAS_BENCH_SAMPLE_INDEX");
    char *end = 0;
    unsigned long long parsed;

    if (!value || value[0] == '\0' || value[0] == '0') {
        return 0u;
    }
    parsed = strtoull(value, &end, 10);
    if (end == value || (end && *end != '\0') || parsed > (size_t)-1) {
        return 0u;
    }
    return (size_t)parsed;
}

static int bench_metadata_footprint_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_METADATA_FOOTPRINT");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_slab_slot_sharing_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_SLAB_SLOT_SHARING");
    return value && value[0] != '\0' && value[0] != '0';
}

static int bench_slab_slot_ab_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_SLAB_SLOT_AB");
    return value && value[0] != '\0' && value[0] != '0';
}

static size_t bench_mprotect_multi_vma_stress_pages(void) {
    const char *value = getenv("UHMAS_BENCH_MPROTECT_MULTI_VMA_STRESS_PAGES");
    char *end = 0;
    unsigned long long parsed;

    if (!value || value[0] == '\0' || value[0] == '0') {
        return 0;
    }
    parsed = strtoull(value, &end, 10);
    if (end == value || (end && *end != '\0') || parsed < 2ull) {
        return 0;
    }
    if (parsed > 512ull) {
        parsed = 512ull;
    }
    return (size_t)parsed;
}

static size_t bench_mprotect_multi_vma_latency_samples(void) {
    const char *value =
        getenv("UHMAS_BENCH_MPROTECT_MULTI_VMA_LATENCY_SAMPLES");
    char *end = 0;
    unsigned long long parsed;

    if (!value || value[0] == '\0' || value[0] == '0') {
        return 0;
    }
    parsed = strtoull(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        return 0;
    }
    if (parsed > 4096ull) {
        parsed = 4096ull;
    }
    return (size_t)parsed;
}

static int bench_32bit_gate_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_32BIT_GATE");
    return value && value[0] != '\0' && value[0] != '0';
}

static uint64_t bench_diag_popcount64(uint64_t value) {
    uint64_t count = 0;

    while (value != 0) {
        count += value & UINT64_C(1);
        value >>= 1;
    }
    return count;
}

static int bench_diagnostics_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_DIAGNOSTICS");
    return value && value[0] != '\0' && value[0] != '0';
}

static void bench_worker_profile_begin(bench_worker_profile *profile,
                                       size_t iterations,
                                       size_t operations_per_iteration) {
    size_t sample_iterations;
    uint32_t cpu_valid = 0u;
    uint32_t cpu_work_is_cycles = 0u;

    if (!profile || iterations == 0u || operations_per_iteration == 0u) {
        return;
    }
    memset(profile, 0, sizeof(*profile));
    sample_iterations =
        (iterations + BENCH_WORKER_LATENCY_CAPACITY - 1u) /
        BENCH_WORKER_LATENCY_CAPACITY;
    if (sample_iterations == 0u) {
        sample_iterations = 1u;
    }
    profile->enabled = 1u;
    profile->sample_iterations = sample_iterations;
    profile->operations_per_iteration = operations_per_iteration;
    profile->wall_begin_ns = bench_now_ns();
    profile->sample_begin_ns = profile->wall_begin_ns;
    profile->cpu_begin_work =
        bench_thread_cpu_work_now(&cpu_work_is_cycles, &cpu_valid);
    profile->cpu_valid = cpu_valid;
    profile->cpu_work_is_cycles = cpu_work_is_cycles;
}

static void bench_worker_profile_sample(bench_worker_profile *profile,
                                        size_t completed_iterations,
                                        size_t total_iterations) {
    size_t sample_iterations;
    size_t operations;
    uint64_t now;
    uint64_t elapsed;

    if (!profile || !profile->enabled || completed_iterations == 0u ||
        completed_iterations > total_iterations ||
        profile->sample_count >= BENCH_WORKER_LATENCY_CAPACITY) {
        return;
    }
    sample_iterations = completed_iterations - profile->sample_begin_iteration;
    if (completed_iterations != total_iterations &&
        sample_iterations < profile->sample_iterations) {
        return;
    }
    operations = sample_iterations * profile->operations_per_iteration;
    if (operations == 0u) {
        return;
    }
    now = bench_now_ns();
    elapsed = now >= profile->sample_begin_ns ?
                  now - profile->sample_begin_ns : 0u;
    profile->latency_ns_per_op[profile->sample_count++] =
        (elapsed + operations / 2u) / operations;
    profile->sample_begin_iteration = completed_iterations;
    profile->sample_begin_ns = now;
}

static void bench_worker_profile_end(bench_worker_profile *profile,
                                     size_t completed_iterations) {
    uint64_t wall_end;
    uint64_t cpu_end_work;
    uint32_t cpu_valid = 0u;
    uint32_t cpu_work_is_cycles = 0u;

    if (!profile || !profile->enabled) {
        return;
    }
    bench_worker_profile_sample(profile, completed_iterations,
                                completed_iterations);
    wall_end = bench_now_ns();
    profile->wall_ns = wall_end >= profile->wall_begin_ns ?
                           wall_end - profile->wall_begin_ns : 0u;
    cpu_end_work = bench_thread_cpu_work_now(&cpu_work_is_cycles,
                                             &cpu_valid);
    if (profile->cpu_valid && cpu_valid &&
        cpu_work_is_cycles == profile->cpu_work_is_cycles &&
        cpu_end_work >= profile->cpu_begin_work) {
        profile->cpu_work = cpu_end_work - profile->cpu_begin_work;
        if (!profile->cpu_work_is_cycles) {
            profile->cpu_ns = profile->cpu_work;
        }
    } else {
        profile->cpu_valid = 0u;
        profile->cpu_work = 0u;
        profile->cpu_ns = 0u;
    }
    profile->wait_ns = profile->cpu_work_is_cycles ? profile->wall_ns :
                                                       (profile->wall_ns >=
                                                                profile->cpu_ns
                                                            ? profile->wall_ns -
                                                                  profile->cpu_ns
                                                            : 0u);
}

static void bench_result_set_worker_profiles(bench_result *result,
                                              const bench_worker_profile *profiles,
                                              size_t profile_count,
                                              long double cycles_per_ns) {
    uint64_t *samples;
    size_t sample_capacity;
    size_t sample_count = 0u;
    size_t i;
    size_t j;
    uint64_t wall_ns = 0u;
    uint64_t cpu_work = 0u;
    uint64_t cpu_ns = 0u;
    uint64_t wait_ns = 0u;
    uint64_t max_wait_ns = 0u;
    uint32_t cpu_work_is_cycles;

    if (!result || !profiles || profile_count == 0u ||
        profile_count > (size_t)-1 / BENCH_WORKER_LATENCY_CAPACITY) {
        return;
    }
    sample_capacity = profile_count * BENCH_WORKER_LATENCY_CAPACITY;
    samples = (uint64_t *)calloc(sample_capacity, sizeof(samples[0]));
    bench_expect(samples != 0, "spinlock worker latency samples alloc");
    bench_expect(profiles[0].enabled && profiles[0].cpu_valid,
                 "spinlock worker profile runtime valid");
    cpu_work_is_cycles = profiles[0].cpu_work_is_cycles;
    if (cpu_work_is_cycles) {
        bench_expect(cycles_per_ns > 0.0L,
                     "spinlock worker cycle calibration valid");
    }
    for (i = 0; i < profile_count; ++i) {
        uint64_t profile_cpu_ns;
        uint64_t profile_wait_ns;

        bench_expect(profiles[i].enabled && profiles[i].cpu_valid,
                     "spinlock worker profile runtime valid");
        bench_expect(profiles[i].cpu_work_is_cycles == cpu_work_is_cycles,
                     "spinlock worker profile CPU units match");
        if (cpu_work_is_cycles) {
            long double converted =
                (long double)profiles[i].cpu_work / cycles_per_ns;
            profile_cpu_ns = converted >= (long double)UINT64_MAX
                                 ? UINT64_MAX
                                 : (uint64_t)(converted + 0.5L);
        } else {
            profile_cpu_ns = profiles[i].cpu_work;
        }
        if (profile_cpu_ns > profiles[i].wall_ns) {
            profile_cpu_ns = profiles[i].wall_ns;
        }
        profile_wait_ns = profiles[i].wall_ns - profile_cpu_ns;
        wall_ns += profiles[i].wall_ns;
        cpu_work += profiles[i].cpu_work;
        cpu_ns += profile_cpu_ns;
        wait_ns += profile_wait_ns;
        if (profile_wait_ns > max_wait_ns) {
            max_wait_ns = profile_wait_ns;
        }
        for (j = 0; j < profiles[i].sample_count; ++j) {
            samples[sample_count++] = profiles[i].latency_ns_per_op[j];
        }
    }
    result->worker_thread_count = (uint64_t)profile_count;
    result->worker_cpu_work = cpu_work;
    result->worker_cpu_work_is_cycles = cpu_work_is_cycles;
    result->worker_cpu_ns = cpu_ns;
    result->worker_wall_ns = wall_ns;
    result->worker_wait_ns = wait_ns;
    result->worker_max_wait_ns = max_wait_ns;
    bench_expect(result->worker_max_wait_ns <= result->worker_wait_ns,
                 "spinlock worker max wait bounded by aggregate wait");
    result->worker_cpu_ratio_ppm = bench_ratio_ppm(cpu_ns, wall_ns);
    result->worker_wait_ratio_ppm = bench_ratio_ppm(wait_ns, wall_ns);
    result->worker_runtime_valid = 1u;
    if (sample_count != 0u) {
        bench_result_set_latency(result, samples, sample_count);
    }
    free(samples);
}

static void bench_result_rescale_worker_cpu(bench_result *result,
                                            uint64_t process_cpu_ns,
                                            uint64_t process_cpu_work,
                                            uint32_t process_work_is_cycles,
                                            long double cycles_per_ns) {
    uint64_t cpu_ns;

    if (!result || !result->worker_runtime_valid) {
        return;
    }
    if (result->worker_cpu_work_is_cycles) {
        if (cycles_per_ns > 0.0L) {
            cpu_ns = (uint64_t)((long double)result->worker_cpu_work /
                                cycles_per_ns +
                                0.5L);
        } else {
            if (!process_work_is_cycles || process_cpu_work == 0u ||
                process_cpu_ns == 0u) {
                return;
            }
            cpu_ns = (uint64_t)(((long double)result->worker_cpu_work *
                                 (long double)process_cpu_ns) /
                                (long double)process_cpu_work +
                                0.5L);
        }
    } else {
        cpu_ns = result->worker_cpu_work;
    }
    if (cpu_ns > result->worker_wall_ns) {
        cpu_ns = result->worker_wall_ns;
    }
    result->worker_cpu_ns = cpu_ns;
    result->worker_wait_ns = result->worker_wall_ns - cpu_ns;
    if (result->worker_thread_count <= 1u) {
        result->worker_max_wait_ns = result->worker_wait_ns;
    }
    result->worker_cpu_ratio_ppm =
        bench_ratio_ppm(cpu_ns, result->worker_wall_ns);
    result->worker_wait_ratio_ppm =
        bench_ratio_ppm(result->worker_wait_ns, result->worker_wall_ns);
}

static void bench_result_set_single_thread_runtime(bench_result *result,
                                                    uint64_t cpu_begin_work,
                                                    uint64_t cpu_end_work,
                                                    uint32_t cpu_work_is_cycles,
                                                    uint32_t cpu_valid) {
    uint64_t cpu_work;

    if (!result || !cpu_valid || cpu_end_work < cpu_begin_work) {
        return;
    }
    cpu_work = cpu_end_work - cpu_begin_work;
    result->worker_thread_count = 1u;
    result->worker_cpu_work = cpu_work;
    result->worker_cpu_work_is_cycles = cpu_work_is_cycles;
    result->worker_cpu_ns = cpu_work_is_cycles ? 0u : cpu_work;
    result->worker_wall_ns = result->wall_time_ns;
    result->worker_wait_ns = cpu_work_is_cycles ? result->wall_time_ns :
                                                     (result->wall_time_ns >=
                                                              cpu_work
                                                          ? result->wall_time_ns -
                                                                cpu_work
                                                          : 0u);
    result->worker_max_wait_ns = result->worker_wait_ns;
    result->worker_cpu_ratio_ppm =
        cpu_work_is_cycles ? 0u
                           : bench_ratio_ppm(result->worker_cpu_ns,
                                             result->worker_wall_ns);
    result->worker_wait_ratio_ppm =
        cpu_work_is_cycles ? 1000000u
                           : bench_ratio_ppm(result->worker_wait_ns,
                                             result->worker_wall_ns);
    result->worker_runtime_valid = 1u;
}

static int bench_diagnostics_timing_enabled(void) {
    const char *value = getenv("UHMAS_BENCH_DIAGNOSTICS_TIMING");
    return !value || value[0] == '\0' || value[0] != '0';
}

static uint64_t bench_u64_max(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

static uint64_t bench_malloc_free_tree_count(const uhm_block *node) {
    if (!node) {
        return UINT64_C(0);
    }
    return UINT64_C(1) +
           bench_malloc_free_tree_count(node->free_tree_left) +
           bench_malloc_free_tree_count(node->free_tree_right);
}

static uint64_t bench_malloc_free_tree_depth(const uhm_block *node) {
    if (!node) {
        return UINT64_C(0);
    }
    return UINT64_C(1) +
           bench_u64_max(bench_malloc_free_tree_depth(node->free_tree_left),
                         bench_malloc_free_tree_depth(node->free_tree_right));
}

static uint64_t bench_vma_tree_count(const uhm_vma *node) {
    if (!node) {
        return UINT64_C(0);
    }
    return UINT64_C(1) + bench_vma_tree_count(node->tree_left) +
           bench_vma_tree_count(node->tree_right);
}

static uint64_t bench_vma_tree_depth(const uhm_vma *node) {
    if (!node) {
        return UINT64_C(0);
    }
    return UINT64_C(1) +
           bench_u64_max(bench_vma_tree_depth(node->tree_left),
                         bench_vma_tree_depth(node->tree_right));
}

static uint64_t bench_mmap_free_tree_count(const uhm_free_extent *node) {
    if (!node) {
        return UINT64_C(0);
    }
    return UINT64_C(1) + bench_mmap_free_tree_count(node->tree_left) +
           bench_mmap_free_tree_count(node->tree_right);
}

static uint64_t bench_mmap_free_tree_depth(const uhm_free_extent *node) {
    if (!node) {
        return UINT64_C(0);
    }
    return UINT64_C(1) +
           bench_u64_max(bench_mmap_free_tree_depth(node->tree_left),
                         bench_mmap_free_tree_depth(node->tree_right));
}

static uint64_t bench_vma_list_count(const uhm_vma *node) {
    uint64_t count = UINT64_C(0);
    while (node) {
        ++count;
        node = node->next;
    }
    return count;
}

static uint64_t bench_mmap_free_list_count(const uhm_free_extent *node) {
    uint64_t count = UINT64_C(0);
    while (node) {
        ++count;
        node = node->next;
    }
    return count;
}

