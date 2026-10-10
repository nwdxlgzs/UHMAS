static void bench_run_uhm_case(bench_heap_env *env, const bench_result *result,
                               const bench_diag_snapshot *before,
                               int diagnostics_enabled,
                               const char *check_message) {
    bench_diag_snapshot after_result;
    bench_diag_snapshot after;

    bench_print(result);
    bench_diag_capture(env, &after_result);
    bench_metadata_print_attribution_delta(result, before, &after_result,
                                           diagnostics_enabled);
    bench_check_heap(env, check_message);
    if (diagnostics_enabled) {
        bench_diag_capture(env, &after);
        after.heap_lock_max_wait_ns = bench_u64_max(
            after_result.heap_lock_max_wait_ns, after.heap_lock_max_wait_ns);
        after.heap_lock_max_hold_ns = bench_u64_max(
            after_result.heap_lock_max_hold_ns, after.heap_lock_max_hold_ns);
        after.slab_raw_lock_max_wait_ns = bench_u64_max(
            after_result.slab_raw_lock_max_wait_ns,
            after.slab_raw_lock_max_wait_ns);
        after.slab_raw_lock_max_hold_ns = bench_u64_max(
            after_result.slab_raw_lock_max_hold_ns,
            after.slab_raw_lock_max_hold_ns);
        after.vma_tree_priority_mix_max_ns = bench_u64_max(
            after_result.vma_tree_priority_mix_max_ns,
            after.vma_tree_priority_mix_max_ns);
        after.vma_tree_max_insert_depth = bench_u64_max(
            after_result.vma_tree_max_insert_depth,
            after.vma_tree_max_insert_depth);
        after.vma_tree_max_find_steps = bench_u64_max(
            after_result.vma_tree_max_find_steps,
            after.vma_tree_max_find_steps);
        after.vma_tree_max_remove_sink_steps = bench_u64_max(
            after_result.vma_tree_max_remove_sink_steps,
            after.vma_tree_max_remove_sink_steps);
        bench_diag_print_delta(result->name, before, &after);
        bench_diag_print_page_range_phase_delta(result->name, before, &after);
        bench_diag_print_tls_cache_delta(result->name, before, &after);
    }
}

static void bench_run_uhm_case_deferred_check(
    bench_heap_env *env, const bench_result *result,
    const bench_diag_snapshot *before, int diagnostics_enabled) {
    bench_diag_snapshot after_result;

    bench_print(result);
    bench_diag_capture(env, &after_result);
    bench_metadata_print_attribution_delta(result, before, &after_result,
                                           diagnostics_enabled);
    if (diagnostics_enabled) {
        bench_diag_print_delta(result->name, before, &after_result);
        bench_diag_print_page_range_phase_delta(result->name, before,
                                                &after_result);
        bench_diag_print_tls_cache_delta(result->name, before, &after_result);
    }
}

static void bench_heap_enable_diagnostics(bench_heap_env *env) {
    uint32_t timing_enabled;

    if (!env || !env->heap || !bench_diagnostics_enabled()) {
        return;
    }
    timing_enabled = bench_diagnostics_timing_enabled() ? 1u : 0u;
    uhm_spinlock_set_diagnostics_enabled(&env->heap->lock, 1u);
    uhm_spinlock_set_diagnostics_timing_enabled(&env->heap->lock,
                                                timing_enabled);
    uhm_heap_set_diagnostics_timing_enabled(env->heap, timing_enabled);
}

static void bench_heap_open_config_flags_slots_ex(
    bench_heap_env *env, size_t arena_size, int enable_tls_cache,
    size_t thread_registry_slots, uint32_t heap_flags,
    uint32_t spinlock_yield_after_spins) {
    uhm_platform_heap_config config;

    memset(env, 0, sizeof(*env));
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = arena_size;
    config.heap_flags = heap_flags;
    config.spinlock_yield_after_spins = spinlock_yield_after_spins;
    if (enable_tls_cache) {
        config.flags |= UHM_PLATFORM_HEAP_ENABLE_TLS_CACHE;
        config.thread_registry_slots = thread_registry_slots;
        config.tls_cache_min_pages = 1u;
        config.tls_cache_default_pages = 1u;
        config.tls_cache_max_pages = 3u;
    }
    bench_expect_ok(uhm_platform_heap_open(&config, &env->owner, &env->heap),
                    "benchmark platform heap open");
    env->api = uhm_get_api(env->heap);
    bench_expect(env->api != 0, "benchmark api");
    env->arena_size = arena_size;
    bench_heap_enable_diagnostics(env);
}

static void bench_heap_open_pre_reserved_flags_slots_ex(
    bench_heap_env *env, size_t arena_size, size_t thread_registry_slots,
    uint32_t heap_flags, void **out_arena_base) {
    uhm_storage_info heap_info;
    uhm_config config;
    uhm_vm_ops vm;
    void *heap_storage = 0;
    void *arena_base = 0;
    size_t heap_storage_reserved_size = 0u;
    uhm_result result;

    bench_expect(env != 0 && arena_size != 0u &&
                     thread_registry_slots != 0u && out_arena_base != 0,
                 "benchmark pre-reserved heap open arguments");
    *out_arena_base = 0;
    memset(env, 0, sizeof(*env));
    memset(&heap_info, 0, sizeof(heap_info));
    heap_info.size = sizeof(heap_info);
    bench_expect_ok(uhm_heap_storage_query(&heap_info, 0),
                    "benchmark pre-reserved heap storage query");
    bench_expect_ok(uhm_platform_vm_ops_init(&vm),
                    "benchmark pre-reserved platform VM init");
    bench_expect((vm.capabilities &
                  (UHM_VM_CAN_RESERVE | UHM_VM_CAN_COMMIT |
                   UHM_VM_CAN_RELEASE | UHM_VM_CAN_PROTECT)) ==
                     (UHM_VM_CAN_RESERVE | UHM_VM_CAN_COMMIT |
                      UHM_VM_CAN_RELEASE | UHM_VM_CAN_PROTECT) &&
                     vm.reserve != 0 && vm.commit != 0 && vm.release != 0 &&
                     vm.protect != 0,
                 "benchmark pre-reserved platform VM capabilities");
    bench_expect(uhm_platform_size_align_up(
                     heap_info.storage_size, vm.page_size,
                     &heap_storage_reserved_size),
                 "benchmark pre-reserved heap storage alignment");
    bench_expect_ok(vm.reserve(vm.user, &heap_storage,
                               heap_storage_reserved_size,
                               vm.allocation_granularity),
                    "benchmark pre-reserved heap storage reserve");
    result = vm.commit(vm.user, heap_storage, heap_storage_reserved_size,
                       UHM_PROT_READ | UHM_PROT_WRITE);
    if (result != UHM_OK) {
        (void)vm.release(vm.user, heap_storage,
                         heap_storage_reserved_size);
        bench_expect_ok(result,
                        "benchmark pre-reserved heap storage commit");
    }
    result = vm.reserve(vm.user, &arena_base, arena_size,
                        vm.allocation_granularity);
    if (result != UHM_OK) {
        (void)vm.release(vm.user, heap_storage,
                         heap_storage_reserved_size);
        bench_expect_ok(result, "benchmark pre-reserved arena reserve");
    }

    uhm_platform_heap_zero(&env->owner);
    env->owner.flags = UHM_PLATFORM_HEAP_ENABLE_TLS_CACHE;
    env->owner.vm = vm;
    env->owner.heap_storage = heap_storage;
    env->owner.heap_storage_size = heap_info.storage_size;
    env->owner.heap_storage_reserved_size = heap_storage_reserved_size;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = heap_flags;
    config.heap_storage = heap_storage;
    config.heap_storage_size = heap_info.storage_size;
    config.arena_base = arena_base;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.thread_registry_slots = thread_registry_slots;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 3u;
    result = uhm_open(&config, &env->owner.heap);
    if (result != UHM_OK) {
        (void)vm.release(vm.user, arena_base, arena_size);
        (void)vm.release(vm.user, heap_storage,
                         heap_storage_reserved_size);
        uhm_platform_heap_zero(&env->owner);
        bench_expect_ok(result, "benchmark pre-reserved heap open");
    }
    env->heap = env->owner.heap;
    env->api = uhm_get_api(env->heap);
    bench_expect(env->api != 0, "benchmark pre-reserved API");
    env->arena_size = arena_size;
    *out_arena_base = arena_base;
    bench_heap_enable_diagnostics(env);
}

static void bench_heap_open_config_flags_ex(bench_heap_env *env,
                                            size_t arena_size,
                                            int enable_tls_cache,
                                            uint32_t heap_flags,
                                            uint32_t spinlock_yield_after_spins) {
    bench_heap_open_config_flags_slots_ex(
        env, arena_size, enable_tls_cache, 8u, heap_flags,
        spinlock_yield_after_spins);
}

static void bench_heap_open_tls_slots(bench_heap_env *env, size_t arena_size,
                                      size_t thread_registry_slots) {
    bench_heap_open_config_flags_slots_ex(
        env, arena_size, 1, thread_registry_slots, 0u, 0u);
}

static void bench_heap_open_config_ex(bench_heap_env *env, size_t arena_size,
                                      int enable_tls_cache,
                                      uint32_t spinlock_yield_after_spins) {
    bench_heap_open_config_flags_ex(env, arena_size, enable_tls_cache, 0u,
                                    spinlock_yield_after_spins);
}

static void bench_heap_open_config(bench_heap_env *env, size_t arena_size,
                                   int enable_tls_cache) {
    bench_heap_open_config_ex(env, arena_size, enable_tls_cache, 0u);
}

static void bench_heap_open(bench_heap_env *env, size_t arena_size) {
    bench_heap_open_config(env, arena_size, 0);
}

static void bench_heap_open_malloc_retention(bench_heap_env *env,
                                             size_t arena_size,
                                             size_t retention_pages) {
    uhm_platform_heap_config config;

    memset(env, 0, sizeof(*env));
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_DISABLE_TLS_CACHE;
    config.arena_size = arena_size;
    config.malloc_free_decommit_retention_pages = retention_pages;
    bench_expect_ok(uhm_platform_heap_open(&config, &env->owner, &env->heap),
                    "benchmark malloc-retention platform heap open");
    env->api = uhm_get_api(env->heap);
    bench_expect(env->api != 0, "benchmark malloc-retention api");
    env->arena_size = arena_size;
    bench_heap_enable_diagnostics(env);
}

static void bench_heap_open_control_band(bench_heap_env *env,
                                         size_t arena_size,
                                         size_t control_band_min_size) {
    uhm_platform_heap_config config;

    memset(env, 0, sizeof(*env));
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_DISABLE_TLS_CACHE;
    config.arena_size = arena_size;
    config.control_band_min_size = control_band_min_size;
    bench_expect_ok(uhm_platform_heap_open(&config, &env->owner, &env->heap),
                    "benchmark control-band platform heap open");
    env->api = uhm_get_api(env->heap);
    bench_expect(env->api != 0, "benchmark control-band api");
    env->arena_size = arena_size;
    bench_heap_enable_diagnostics(env);
}

static void bench_heap_open_tls(bench_heap_env *env, size_t arena_size) {
    bench_heap_open_config(env, arena_size, 1);
}

static void bench_heap_open_no_tls_control_config_flags_ex(
    bench_heap_env *env, size_t arena_size, uint32_t heap_flags,
    uint32_t spinlock_yield_after_spins,
    size_t control_band_min_size) {
    uhm_platform_heap_config config;

    memset(env, 0, sizeof(*env));
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_DISABLE_TLS_CACHE;
    config.arena_size = arena_size;
    config.heap_flags = heap_flags;
    config.spinlock_yield_after_spins = spinlock_yield_after_spins;
    config.control_band_min_size = control_band_min_size;
    bench_expect_ok(uhm_platform_heap_open(&config, &env->owner, &env->heap),
                    "benchmark no-TLS platform heap open");
    env->api = uhm_get_api(env->heap);
    bench_expect(env->api != 0, "benchmark no-TLS api");
    env->arena_size = arena_size;
    bench_heap_enable_diagnostics(env);
}

static void bench_heap_open_no_tls_config_flags_ex(
    bench_heap_env *env, size_t arena_size, uint32_t heap_flags,
    uint32_t spinlock_yield_after_spins) {
    bench_heap_open_no_tls_control_config_flags_ex(
        env, arena_size, heap_flags, spinlock_yield_after_spins, 0u);
}

static void bench_heap_close(bench_heap_env *env) {
    if (env->heap && env->api) {
        bench_expect_ok(env->api->check(env->heap, 0), "benchmark heap check");
        bench_expect_ok(uhm_platform_heap_close(&env->owner),
                        "benchmark platform heap close");
    }
    memset(env, 0, sizeof(*env));
}

static void *bench_os_map(size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *ptr = mmap(0, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return ptr == MAP_FAILED ? 0 : ptr;
#endif
}

static void bench_os_unmap(void *ptr, size_t size) {
#if defined(_WIN32)
    (void)size;
    (void)VirtualFree(ptr, 0, MEM_RELEASE);
#else
    (void)munmap(ptr, size);
#endif
}

