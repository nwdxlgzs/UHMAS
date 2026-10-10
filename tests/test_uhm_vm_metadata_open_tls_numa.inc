static void test_open_failure_releases_reserved_arena(const uhm_storage_info *heap_info) {
    test_alloc heap_mem = {0};
    failing_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const size_t page_size = uhm_platform_system_page_size();
    const size_t allocation_granularity =
        uhm_platform_system_allocation_granularity();

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size, heap_info->storage_alignment, &heap_mem) != 0,
                "failure heap storage alloc");

    memset(&vm, 0, sizeof(vm));
    vm.size = sizeof(vm);
    vm.abi_major = UHM_ABI_MAJOR;
    vm.abi_minor = UHM_ABI_MINOR;
    vm.user = &state;
    vm.page_size = page_size;
    vm.allocation_granularity = allocation_granularity;
    vm.capabilities = UHM_VM_CAN_RESERVE | UHM_VM_CAN_COMMIT | UHM_VM_CAN_RELEASE;
    vm.reserve = failing_vm_reserve;
    vm.commit = failing_vm_commit;
    vm.release = failing_vm_release;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_size = 1024u * 1024u;
    config.vm = vm;

    memset(heap_mem.aligned, 0, heap_info->storage_size);
    state.fail_reserve = 1;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_PLATFORM,
                "reserve failure returns platform error");
    expect_true(state.reserve_count == 1, "reserve failure calls reserve once");
    expect_true(state.commit_count == 0, "reserve failure does not commit");
    expect_true(state.release_count == 0, "reserve failure does not release");
    expect_true(uhm_get_api((uhm_heap *)heap_mem.aligned) == 0,
                "reserve failed heap has no api");

    memset(&state, 0, sizeof(state));
    vm.user = &state;
    config.vm = vm;
    heap = 0;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_PLATFORM, "open failure returns commit error");
    expect_true(state.reserve_count == 1, "reserve called once");
    expect_true(state.commit_count == 1, "commit called once");
    expect_true(state.release_count == 1, "release called after commit failure");
    expect_true(uhm_get_api((uhm_heap *)heap_mem.aligned) == 0, "failed heap has no api");

    test_vm_release_alloc(&heap_mem);
}

static void test_vm_zero_page_size_auto_detects_system(
    const uhm_storage_info *heap_info) {
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    size_t system_page;
    size_t system_granularity;
    void *ptr;

    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, 0u, 0u, UHM_VM_DECOMMIT_ZERO_FILLS),
              "zero page vm init auto detects");
    system_page = vm.page_size;
    system_granularity = vm.allocation_granularity;
    expect_true(system_page != 0 && (system_page & (system_page - 1u)) == 0,
                "zero page system page power of two");
    expect_true(system_granularity != 0 &&
                    (system_granularity & (system_granularity - 1u)) == 0,
                "zero page system granularity power of two");

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "zero page heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, system_granularity,
                                    &arena_mem) != 0,
                "zero page arena alloc");

    vm.page_size = 0;
    vm.allocation_granularity = 0;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    expect_ok(uhm_open(&config, &heap), "zero page heap open");
    expect_true(heap->F.vm.page_size == system_page,
                "zero page normalized page size");
    expect_true(heap->F.vm.allocation_granularity == system_granularity,
                "zero page normalized granularity");
    api = uhm_get_api(heap);
    expect_true(api != 0, "zero page api");
    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "zero page malloc works");
    expect_true(((uintptr_t)ptr % UHM_MIN_ALIGNMENT) == 0,
                "zero page malloc alignment");
    api->free(heap, ptr);
    expect_ok(api->check(heap, 0), "zero page heap check");
    expect_ok(api->close(heap), "zero page close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_tls_config_and_mmap_ex_numa(
    const uhm_storage_info *heap_info) {
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_mmap_options options;
    uhm_stats before;
    uhm_stats after;
    const size_t page_size = uhm_platform_system_page_size();
    void *map;
    void *small_malloc;
    void *large_malloc;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "tls/numa heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "tls/numa arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "tls/numa usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;

    config.tls_cache_min_pages = 3u;
    config.tls_cache_default_pages = 2u;
    config.tls_cache_max_pages = 4u;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "tls config rejects default below min");
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 2u;
    config.tls_cache_max_pages = 0u;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "tls config rejects nonzero range with disabled max");
    config.tls_cache_min_pages = 0u;
    config.tls_cache_default_pages = 0u;
    config.tls_cache_max_pages = 0u;
    config.tls_cache_max_retained_pages = 0u;
    config.tls_cache_max_retained_pages = 1u;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "tls config rejects retained pages while disabled");
    config.tls_cache_max_retained_pages = 0u;
    config.numa_policy = 99u;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "numa config rejects unknown policy");
    config.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_UNSUPPORTED,
                "numa config rejects unsupported preferred default");

    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 2u;
    config.tls_cache_max_pages = 4u;
    config.tls_cache_max_retained_pages = 3u;
    config.numa_policy = UHM_NUMA_POLICY_FIRST_TOUCH;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "tls config rejects retained ceiling below byte max");

    config.size = UHM_CONFIG_SIZE_ABI17;
    config.abi_minor = 17u;
    config.tls_cache_max_retained_pages = 9u;
    expect_ok(uhm_open(&config, &heap), "tls abi17 retained tail ignored");
    expect_true(heap->core.tls_cache_max_retained_pages ==
                    4u * UHM_LOCAL_RETAINED_PAGE_FACTOR,
                "tls abi17 uses compatible retained default");
    expect_ok(uhm_get_api(heap)->close(heap), "tls abi17 retained close");

    config.size = sizeof(config);
    config.abi_minor = UHM_ABI_MINOR;
    config.tls_cache_max_retained_pages = 9u;
    config.numa_policy = UHM_NUMA_POLICY_FIRST_TOUCH;
    config.preferred_numa_node = 3;
    expect_ok(uhm_open(&config, &heap), "tls/numa first-touch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mmap_ex != 0, "mmap_ex api slot");
    expect_true(heap->F.vm.numa_hint == vm.numa_hint,
                "numa hint bound into heap F");
    expect_true(heap->core.tls_cache_min_pages == 1u &&
                    heap->core.tls_cache_default_pages == 2u &&
                    heap->core.tls_cache_max_pages == 4u &&
                    heap->core.tls_cache_max_retained_pages == 9u,
                "tls cache config stored");
    expect_true(heap->core.numa_policy == UHM_NUMA_POLICY_FIRST_TOUCH &&
                    heap->core.preferred_numa_node == 3,
                "numa config stored");

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.abi_minor = UHM_ABI_MINOR;
    options.map_size = page_size;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE;
    expect_ok(api->mmap_ex(heap, &options), "mmap_ex default first-touch");
    expect_true(options.addr != 0, "mmap_ex default address out");
    expect_ok(api->munmap(heap, options.addr, page_size),
              "mmap_ex default cleanup");

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.map_size = page_size;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = 1;
    expect_true(api->mmap_ex(heap, &options) == UHM_ERR_UNSUPPORTED,
                "mmap_ex unsupported preferred fails");
    expect_true(options.addr == 0, "failed mmap_ex does not publish address");
    options.flags = 1u;
    expect_true(api->mmap_ex(heap, &options) == UHM_ERR_INVALID_ARGUMENT,
                "mmap_ex rejects reserved option flags");
    expect_ok(api->check(heap, 0), "tls/numa first heap check");
    expect_ok(api->close(heap), "tls/numa first heap close");

    memset(&state, 0, sizeof(state));
    state.platform_error = test_platform_error_code();
    vm.user = &state;
    vm.capabilities |= UHM_VM_CAN_NUMA_PREFERRED;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    vm.protect = fault_vm_protect;
    vm.fixed_remap = fault_vm_fixed_remap;
    vm.numa_hint = fault_vm_numa_hint;
    config.vm = vm;
    config.tls_cache_min_pages = 0u;
    config.tls_cache_default_pages = 0u;
    config.tls_cache_max_pages = 0u;
    config.tls_cache_max_retained_pages = 0u;
    config.preferred_numa_node = -1;
    config.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    expect_true(uhm_open(&config, &heap) == UHM_ERR_INVALID_ARGUMENT,
                "numa preferred config rejects negative node");
    config.numa_policy = UHM_NUMA_POLICY_DEFAULT;
    heap = 0;
    expect_ok(uhm_open(&config, &heap), "numa preferred heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mmap_ex != 0, "numa preferred api");

    state.commit_count = 0;
    state.numa_hint_count = 0;
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.map_size = page_size;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = -1;
    expect_true(api->mmap_ex(heap, &options) == UHM_ERR_INVALID_ARGUMENT,
                "mmap_ex preferred rejects negative node");
    expect_true(options.addr == 0 && state.commit_count == 0 &&
                    state.numa_hint_count == 0,
                "negative NUMA node rejected before platform work");

    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before), "numa stats before failure");
    state.commit_count = 0;
    state.decommit_count = 0;
    state.numa_hint_count = 0;
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.map_size = page_size;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = 5;
    state.fail_numa_hint = 1;
    expect_true(api->mmap_ex(heap, &options) == UHM_ERR_PLATFORM,
                "mmap_ex numa hint failure propagates");
    expect_true(state.commit_count == 1 && state.numa_hint_count == 1 &&
                    state.decommit_count == 1,
                "mmap_ex numa failure rolled back commit");
    expect_last_error(api, heap, UHM_ERR_PLATFORM, state.platform_error,
                      "mmap_ex numa failure last error");
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after), "numa stats after failure");
    expect_true(after.committed_bytes == before.committed_bytes,
                "mmap_ex numa failure does not leak committed bytes");
    expect_ok(api->check(heap, 0), "numa failure heap check");

    state.fail_numa_hint = 0;
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.map_size = page_size;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = 6;
    expect_ok(api->mmap_ex(heap, &options), "mmap_ex preferred succeeds");
    map = options.addr;
    expect_true(map != 0 && state.last_numa_hint_addr == map &&
                    state.last_numa_hint_size == page_size &&
                    state.last_numa_hint_policy == UHM_NUMA_POLICY_PREFERRED &&
                    state.last_numa_hint_node == 6,
                "mmap_ex preferred calls hint with range and node");

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.map_size = page_size;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE | UHM_MAP_NO_COMMIT;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = 6;
    expect_true(api->mmap_ex(heap, &options) == UHM_ERR_UNSUPPORTED,
                "mmap_ex no-commit preferred requires committed pages");
    expect_ok(api->munmap(heap, map, page_size), "mmap_ex preferred cleanup");
    expect_ok(api->check(heap, 0), "numa preferred heap check");
    expect_ok(api->close(heap), "numa preferred heap close");

    config.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    config.preferred_numa_node = 2;
    state.commit_count = 0;
    state.numa_hint_count = 0;
    state.fail_numa_hint = 1;
    heap = 0;
    expect_ok(uhm_open(&config, &heap),
              "malloc preferred numa heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "malloc preferred numa api");
    small_malloc = api->malloc(heap, 64u);
    large_malloc = api->malloc(heap, page_size * 3u);
    expect_true(small_malloc != 0 && large_malloc != 0,
                "malloc preferred numa ignores hint failure");
    expect_true(state.numa_hint_count >= 2 &&
                    state.last_numa_hint_policy ==
                        UHM_NUMA_POLICY_PREFERRED &&
                    state.last_numa_hint_node == 2,
                "malloc preferred numa uses heap policy as recommendation");
    expect_last_error(api, heap, UHM_OK, 0u,
                      "malloc preferred hint failure is non-fatal");
    api->free(heap, small_malloc);
    api->free(heap, large_malloc);
    expect_ok(api->check(heap, 0), "malloc preferred numa heap check");
    expect_ok(api->close(heap), "malloc preferred numa heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_tls_cache_reuse_adapt_and_heap_close_flush(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *reuse;
    uhm_slab_slot *slot;
    uhm_slab *slab;
    uhm_tls_cache *cache;
    uhm_stats stats;
    size_t i;
    void *retained_allocs[256];
    size_t retained_count = 0;
    size_t second_slab_index = SIZE_MAX;
    uint64_t epoch_after_malloc;
    uint64_t epoch_after_free;
    uint64_t overflow_before;
    uint64_t page_pressure_before;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "tls cache heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "tls cache arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "tls cache usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.thread_registry_slots = 2u;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 3u;

    expect_ok(uhm_open(&config, &heap), "tls cache heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "tls cache api");

    first = api->malloc(heap, 64u);
    expect_true(first != 0, "tls cache first malloc");
    memset(first, 0xa7, 64u);
    slot = test_slab_slot_from_payload(first);
    slab = slot->slab;
    epoch_after_malloc = heap->core.metadata_epoch;
    expect_true(slab != 0 && slab->magic == UHM_SLAB_MAGIC,
                "tls cache slab metadata");
    api->free(heap, first);
    epoch_after_free = heap->core.metadata_epoch;
    expect_true(epoch_after_free == epoch_after_malloc,
                "tls cache push does not advance metadata epoch");
    cache = heap->core.tls_cache_head;
    expect_true(heap->core.tls_cache_active_count == 1u &&
                    cache != 0 && cache->local_cache_count == 1u &&
                    cache->local_cache_head == slot,
                "tls cache stores freed slot");
    expect_true(slab->ctx_cached_slots == 1u &&
                    (slot->flags & UHM_SLAB_SLOT_FLAG_CTX_CACHE) != 0,
                "tls cache marks slab cached slot");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "tls cache stats after free");
    expect_true(stats.malloc_live_bytes == 0, "tls cache free removes live bytes");
    expect_ok(api->check(heap, 0), "tls cache check with cached slot");
    {
        uintptr_t saved_owner_ref =
            atomic_load_explicit(&cache->owner_ref, UHM_ATOMIC_ACQUIRE);
        atomic_store_explicit(&cache->owner_ref, (uintptr_t)0,
                              UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "tls cache owner ref corruption detected");
        atomic_store_explicit(&cache->owner_ref, saved_owner_ref,
                              UHM_ATOMIC_RELEASE);
    }
    expect_ok(api->check(heap, 0), "tls cache owner ref restored");
    {
        uint64_t valid_epoch;

        atomic_store_explicit(&cache->metadata_reader.epoch,
                              heap->core.metadata_epoch, UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "tls cache reader inactive dirty epoch");
        atomic_store_explicit(&cache->metadata_reader.epoch, 0u,
                              UHM_ATOMIC_RELEASE);
        expect_ok(api->check(heap, 0),
                  "tls cache reader inactive epoch restored");

        atomic_store_explicit(&cache->metadata_reader.hazards[0],
                              (uintptr_t)cache, UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "tls cache reader inactive dirty hazard");
        atomic_store_explicit(&cache->metadata_reader.hazards[0],
                              (uintptr_t)0, UHM_ATOMIC_RELEASE);
        expect_ok(api->check(heap, 0),
                  "tls cache reader inactive hazard restored");

        expect_true(uhm_metadata_reader_begin(&cache->metadata_reader, heap),
                    "tls cache reader begin active");
        valid_epoch = atomic_load_explicit(&cache->metadata_reader.epoch,
                                           UHM_ATOMIC_ACQUIRE);
        atomic_store_explicit(&cache->metadata_reader.epoch,
                              heap->core.metadata_epoch + 1u,
                              UHM_ATOMIC_RELEASE);
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "tls cache reader future active epoch");
        atomic_store_explicit(&cache->metadata_reader.epoch, valid_epoch,
                              UHM_ATOMIC_RELEASE);
        expect_ok(api->check(heap, 0),
                  "tls cache reader active epoch restored");

        expect_true(uhm_metadata_reader_hazard_set(&cache->metadata_reader, 0u,
                                                   slab),
                    "tls cache reader set slab hazard");
        expect_true(uhm_metadata_reader_hazard_set(&cache->metadata_reader, 1u,
                                                   slot),
                    "tls cache reader set slot hazard");
        expect_ok(api->check(heap, 0),
                  "tls cache reader slab slot hazards known");
        expect_true(uhm_metadata_reader_hazard_set(
                        &cache->metadata_reader, 1u,
                        (void *)((uintptr_t)slot + 1u)),
                    "tls cache reader set misaligned slot hazard");
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "tls cache reader misaligned slot hazard");
        expect_true(uhm_metadata_reader_hazard_set(&cache->metadata_reader,
                                                   1u, slot),
                    "tls cache reader restore slot hazard");
        expect_ok(api->check(heap, 0),
                  "tls cache reader restored slot hazard");

        expect_true(uhm_metadata_reader_hazard_set(&cache->metadata_reader, 0u,
                                                   cache),
                    "tls cache reader set unknown hazard");
        expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                    "tls cache reader unknown active hazard");
        expect_true(uhm_metadata_reader_hazard_set(&cache->metadata_reader, 0u,
                                                   0),
                    "tls cache reader clear unknown hazard");
        expect_true(uhm_metadata_reader_hazard_set(&cache->metadata_reader, 1u,
                                                   0),
                    "tls cache reader clear slot hazard");
        expect_true(uhm_metadata_reader_end(&cache->metadata_reader),
                    "tls cache reader end active");
    }
    expect_ok(api->check(heap, 0), "tls cache reader diagnostics restored");

    reuse = api->malloc(heap, 48u);
    expect_true(reuse == first, "tls cache malloc reuses local slot");
    expect_true(heap->core.metadata_epoch == epoch_after_free,
                "tls cache pop does not advance metadata epoch");
    expect_true(cache->local_cache_count == 0u &&
                    slab->ctx_cached_slots == 0u,
                "tls cache pop clears cached counters");
    api->free(heap, reuse);
    for (i = 0; i < 70u; ++i) {
        void *ptr = api->malloc(heap, 64u);
        expect_true(ptr == first, "tls cache repeated local reuse");
        api->free(heap, ptr);
    }
    expect_true(cache->hit_count >= 64u &&
                    cache->adaptive_pages >= 2u &&
                    cache->adaptive_pages <= config.tls_cache_max_pages,
                "tls cache adapts within configured page range");
    expect_ok(api->check(heap, 0), "tls cache check after adaptation");

    retained_allocs[retained_count++] = api->malloc(heap, 64u);
    expect_true(retained_allocs[0] == first,
                "tls retained-page setup consumes cached slot");
    for (; retained_count < sizeof(retained_allocs) /
                                sizeof(retained_allocs[0]);
         ++retained_count) {
        retained_allocs[retained_count] = api->malloc(heap, 64u);
        expect_true(retained_allocs[retained_count] != 0,
                    "tls retained-page setup malloc");
        if (test_slab_slot_from_payload(retained_allocs[retained_count])
                ->slab != slab) {
            second_slab_index = retained_count;
            ++retained_count;
            break;
        }
    }
    expect_true(second_slab_index != SIZE_MAX,
                "tls retained-page setup reaches second slab");
    cache->adaptive_pages = 1u;
    cache->local_cache_limit = page_size;
    heap->core.tls_cache_max_pages = 1u;
    cache->retained_slab_page_limit = 1u;
    overflow_before = cache->overflow_count;
    page_pressure_before = cache->retained_page_pressure_count;
    api->free(heap, retained_allocs[0]);
    api->free(heap, retained_allocs[second_slab_index]);
    expect_true(cache->retained_slab_pages == 1u &&
                    cache->local_cache_count == 1u &&
                    cache->local_cache_head ==
                        test_slab_slot_from_payload(
                            retained_allocs[second_slab_index]) &&
                    (test_slab_slot_from_payload(retained_allocs[0])->flags &
                     UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0u,
                "tls retained-page pressure evicts the cold slab");
    expect_true(cache->overflow_count == overflow_before &&
                    cache->retained_page_pressure_count >
                        page_pressure_before,
                "tls retained-page pressure is distinct from byte overflow");
    heap->core.tls_cache_max_pages = config.tls_cache_max_pages;
    cache->retained_slab_page_limit =
        config.tls_cache_max_pages * UHM_LOCAL_RETAINED_PAGE_FACTOR;
    for (i = 1u; i < retained_count; ++i) {
        if (i != second_slab_index) {
            api->free(heap, retained_allocs[i]);
        }
    }
    expect_true(cache->retained_slab_pages == 2u &&
                    cache->retained_slab_pages <=
                        cache->retained_slab_page_limit,
                "tls retained-page cap expands again after limit restore");
    expect_true(cache->local_cache_bytes <= page_size,
                "tls retained-page rotation respects byte budget");
    for (i = 0; i < UHM_LOCAL_CACHE_IDLE_MISS_THRESHOLD; ++i) {
        void *idle_ptr = api->malloc(heap, 1024u);
        expect_true(idle_ptr != 0, "tls retained idle miss malloc");
        api->free(heap, idle_ptr);
    }
    expect_true(cache->retained_slab_pages == 1u &&
                    (atomic_load_explicit(&cache->idle_miss_streak,
                                          UHM_ATOMIC_ACQUIRE) &
                     UHM_LOCAL_CACHE_IDLE_STREAK_MASK) == 0u &&
                    (test_slab_slot_from_payload(
                         retained_allocs[second_slab_index])->flags &
                     UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0u,
                "tls retained idle misses evict one excess slab");
    expect_ok(api->check(heap, 0), "tls retained-page cap check");

    expect_ok(api->close(heap), "tls cache heap close flush");
    expect_true(uhm_get_api(heap) == 0, "tls cache closed api rejected");
    expect_true(heap->core.tls_cache_head == 0 &&
                    heap->core.tls_cache_active_count == 0u,
                "tls cache close clears active registry");
    expect_true(slab->ctx_cached_slots == 0u &&
                    (slot->flags & UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0,
                "tls cache close flush returns slot to slab");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_tls_cache_page_hint_set_associativity(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    enum { target_slabs = UHM_LOCAL_PAGE_HINT_SET_COUNT + 1u };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *allocs[512] = {0};
    size_t first_index[target_slabs];
    size_t probe_index[target_slabs];
    uhm_slab *slabs[target_slabs];
    size_t alloc_count = 0;
    size_t slab_count = 0;
    size_t pair_a = SIZE_MAX;
    size_t pair_b = SIZE_MAX;
    size_t warmup = SIZE_MAX;
    size_t set_base;
    uint64_t page_span_before;
    uint64_t page_lock_before;
    uint64_t reader_seq_before;
    size_t i;
    size_t j;
    uhm_tls_cache *cache;
    uhm_local_page_hint saved0;
    uhm_local_page_hint saved1;

    memset(first_index, 0xff, sizeof(first_index));
    memset(probe_index, 0xff, sizeof(probe_index));
    memset(slabs, 0, sizeof(slabs));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "tls set-assoc heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(2u * 1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "tls set-assoc arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "tls set-assoc usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 2u * 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.thread_registry_slots = 2u;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 3u;

    expect_ok(uhm_open(&config, &heap), "tls set-assoc heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "tls set-assoc api");

    while (alloc_count < sizeof(allocs) / sizeof(allocs[0]) &&
           (slab_count < target_slabs ||
            probe_index[target_slabs - 1u] == SIZE_MAX)) {
        uhm_slab_slot *slot;
        uhm_slab *slab;
        size_t slab_index = SIZE_MAX;

        allocs[alloc_count] = api->malloc(heap, 512u);
        expect_true(allocs[alloc_count] != 0,
                    "tls set-assoc seed allocation");
        slot = test_slab_slot_from_payload(allocs[alloc_count]);
        slab = slot ? slot->slab : 0;
        for (i = 0u; i < slab_count; ++i) {
            if (slabs[i] == slab) {
                slab_index = i;
                break;
            }
        }
        if (slab_index == SIZE_MAX && slab_count < target_slabs) {
            slab_index = slab_count++;
            slabs[slab_index] = slab;
            first_index[slab_index] = alloc_count;
        } else if (slab_index != SIZE_MAX &&
                   probe_index[slab_index] == SIZE_MAX) {
            probe_index[slab_index] = alloc_count;
        }
        ++alloc_count;
    }
    expect_true(slab_count == target_slabs &&
                    probe_index[target_slabs - 1u] != SIZE_MAX,
                "tls set-assoc reaches enough two-slot slabs");

    for (i = 0u; i < slab_count && pair_a == SIZE_MAX; ++i) {
        size_t page_i =
            (size_t)(slabs[i]->base - heap->core.arena_base) / page_size;
        for (j = i + 1u; j < slab_count; ++j) {
            size_t page_j =
                (size_t)(slabs[j]->base - heap->core.arena_base) / page_size;
            if (test_local_page_hint_set_base(page_i) ==
                test_local_page_hint_set_base(page_j)) {
                pair_a = i;
                pair_b = j;
                break;
            }
        }
    }
    expect_true(pair_a != SIZE_MAX && pair_b != SIZE_MAX,
                "tls set-assoc finds colliding slabs");
    for (i = 0u; i < slab_count; ++i) {
        if (i != pair_a && i != pair_b) {
            warmup = i;
            break;
        }
    }
    expect_true(warmup != SIZE_MAX, "tls set-assoc warmup slab");

    api->free(heap, allocs[first_index[warmup]]);
    api->free(heap, allocs[first_index[pair_a]]);
    api->free(heap, allocs[first_index[pair_b]]);
    cache = heap->core.tls_cache_head;
    set_base = test_local_page_hint_set_base(
        (size_t)(slabs[pair_a]->base - heap->core.arena_base) / page_size);
    expect_true(cache != 0 &&
                    cache->local_page_hints[set_base].anchor != 0 &&
                    cache->local_page_hints[set_base].anchor->slab ==
                        slabs[pair_b] &&
                    cache->local_page_hints[set_base + 1u].anchor != 0 &&
                    cache->local_page_hints[set_base + 1u].anchor->slab ==
                        slabs[pair_a],
                "tls set-assoc retains both colliding slab anchors");
    expect_ok(api->check(heap, 0), "tls set-assoc initial check");

    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    reader_seq_before = uhm_metadata_registered_reader_seq_load(heap);
    api->free(heap, allocs[probe_index[pair_a]]);
    expect_true(cache->local_page_hints[set_base].anchor != 0 &&
                    cache->local_page_hints[set_base].anchor->slab ==
                    slabs[pair_a],
                "tls set-assoc second-way free promotes its slab");
    api->free(heap, allocs[probe_index[pair_b]]);
    expect_true(cache->local_page_hints[set_base].anchor != 0 &&
                    cache->local_page_hints[set_base].anchor->slab ==
                        slabs[pair_b] &&
                    cache->local_page_hints[set_base + 1u].anchor != 0 &&
                    cache->local_page_hints[set_base + 1u].anchor->slab ==
                        slabs[pair_a] &&
                    test_atomic_u64_load(&heap->core.page_span_calls) ==
                        page_span_before &&
                    test_atomic_u64_load(
                        &heap->core.page_lock_range_shards) ==
                        page_lock_before &&
                    uhm_metadata_registered_reader_seq_load(heap) ==
                        reader_seq_before,
                "tls set-assoc both ways avoid page lookup and reader");

    saved0 = cache->local_page_hints[set_base];
    saved1 = cache->local_page_hints[set_base + 1u];
    cache->local_page_hints[set_base].anchor = 0;
    cache->local_page_hints[set_base].record = 0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "tls set-assoc gap is corruption");
    cache->local_page_hints[set_base] = saved0;
    cache->local_page_hints[set_base + 1u] = saved0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "tls set-assoc duplicate slab is corruption");
    cache->local_page_hints[set_base] = saved0;
    cache->local_page_hints[set_base + 1u] = saved1;
    expect_ok(api->check(heap, 0), "tls set-assoc restored check");

    for (i = 0u; i < alloc_count; ++i) {
        if (i != first_index[warmup] && i != first_index[pair_a] &&
            i != first_index[pair_b] && i != probe_index[pair_a] &&
            i != probe_index[pair_b]) {
            api->free(heap, allocs[i]);
        }
    }
    expect_ok(api->close(heap), "tls set-assoc heap close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_tls_cache_partial_lowlock(const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *first;
    void *second;
    void *third;
    void *reuse;
    void *empty_cache_alloc;
    void *drift_alloc;
    uhm_slab_slot *first_slot;
    uhm_slab_slot *second_slot;
    uhm_slab *slab;
    uhm_tls_cache *cache;
    uint64_t lock_before;
    uint64_t raw_lock_before;
    uint64_t page_span_before;
    uint64_t page_lock_calls_before;
    uint64_t page_lock_shards_before;
    uint64_t reader_seq_before;
    size_t readers_before;
    uintptr_t free_head_ref_before;
    uint32_t state_mirror_before;
    size_t live_count_before;
    size_t free_count_before;
    size_t cached_count_before;
    size_t cached_page;
    size_t cached_page_count;
    size_t cached_hint_base;
    uhm_page_record *cached_record;
    uint64_t cached_generation;
    uint32_t cached_prot;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "tls lowlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                             &arena_mem) != 0,
                "tls lowlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "tls lowlock usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.thread_registry_slots = 2u;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 2u;

    expect_ok(uhm_open(&config, &heap), "tls lowlock heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "tls lowlock api");

    first = api->malloc(heap, 64u);
    second = api->malloc(heap, 64u);
    third = api->malloc(heap, 64u);
    expect_true(first != 0 && second != 0 && third != 0,
                "tls lowlock seed allocations");
    first_slot = test_slab_slot_from_payload(first);
    second_slot = test_slab_slot_from_payload(second);
    slab = first_slot ? first_slot->slab : 0;
    expect_true(slab != 0 &&
                    second_slot != 0 &&
                    second_slot->slab == slab &&
                    test_slab_slot_from_payload(third)->slab == slab &&
                    slab->state == UHM_SLAB_STATE_PARTIAL,
                "tls lowlock partial slab seeded");

    api->free(heap, first);
    cache = heap->core.tls_cache_head;
    expect_true(cache != 0 && cache->local_cache_head == first_slot &&
                    cache->nonempty_class_mask ==
                        (UINT32_C(1) << first_slot->size_class),
                "tls lowlock warmup creates cache");
    cache->nonempty_class_mask = 0u;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "tls check catches missing nonempty class bit");
    cache->nonempty_class_mask = UINT32_C(1) << first_slot->size_class;
    expect_ok(api->check(heap, 0), "tls nonempty class mask restore check");
    expect_true(test_shadow_page_span(heap, (uintptr_t)second,
                                      sizeof(*second_slot), &cached_page,
                                      &cached_page_count) &&
                    cached_page_count == 1u,
                "tls lowlock live slot page span");
    cached_record = test_page_record_at(heap, cached_page);
    expect_true(cached_record != 0,
                "tls lowlock live slot page record");
    cached_prot = cached_record->prot;
    cached_record->prot = UHM_PROT_READ;
    api->free(heap, second);
    expect_true(cache->local_cache_head == first_slot &&
                    cache->local_cache_count == 1u &&
                    cache->nonempty_class_mask ==
                        (UINT32_C(1) << first_slot->size_class) &&
                    atomic_load_explicit(&cache->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u &&
                    (second_slot->flags & UHM_SLAB_SLOT_FLAG_FREE) == 0u,
                "tls lowlock rejected push releases reader and keeps slot live");
    cached_record->prot = cached_prot;

    lock_before = test_atomic_u64_load(&heap->lock.lock_calls);
    readers_before = uhm_metadata_registered_readers_load(heap);
    reader_seq_before = uhm_metadata_registered_reader_seq_load(heap);
    state_mirror_before = atomic_load_explicit(&slab->state_mirror,
                                               UHM_ATOMIC_ACQUIRE);
    free_head_ref_before = atomic_load_explicit(&slab->free_head_ref,
                                                UHM_ATOMIC_ACQUIRE);
    live_count_before = atomic_load_explicit(&slab->live_slots_count,
                                             UHM_ATOMIC_ACQUIRE);
    free_count_before = atomic_load_explicit(&slab->free_slots_count,
                                             UHM_ATOMIC_ACQUIRE);
    cached_count_before = atomic_load_explicit(&slab->ctx_cached_slots,
                                               UHM_ATOMIC_ACQUIRE);
    raw_lock_before = test_atomic_u64_load(&slab->raw_lock.lock_calls);
    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_calls_before =
        test_atomic_u64_load(&heap->core.page_lock_range_calls);
    page_lock_shards_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    api->free(heap, second);
    expect_true(test_atomic_u64_load(&heap->core.page_span_calls) ==
                    page_span_before + 1u,
                "tls lowlock cache push uses one page owner probe");
    expect_true(test_atomic_u64_load(&heap->core.page_lock_range_calls) ==
                         page_lock_calls_before + 1u &&
                    test_atomic_u64_load(&heap->core.page_lock_range_shards) ==
                        page_lock_shards_before + 1u,
                "tls lowlock cache push locks exactly one page shard");
    expect_true(test_atomic_u64_load(&heap->lock.lock_calls) == lock_before,
                "tls lowlock cache push avoids heap lock");
    expect_true(test_atomic_u64_load(&slab->raw_lock.lock_calls) ==
                    raw_lock_before,
                "tls reservation push avoids slab raw lock");
    expect_true(uhm_metadata_registered_readers_load(heap) == readers_before,
                "tls lowlock cache push avoids registered reader count");
    expect_true(uhm_metadata_registered_reader_seq_load(heap) ==
                    reader_seq_before,
                "tls lowlock cache push avoids registered reader seq");
    expect_true(cache->local_cache_head == second_slot &&
                    cache->local_cache_count == 2u &&
                    atomic_load_explicit(&cache->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "tls lowlock push stores slot and releases reader");
    expect_true(atomic_load_explicit(&slab->state_mirror,
                                     UHM_ATOMIC_ACQUIRE) ==
                    state_mirror_before &&
                    state_mirror_before == UHM_SLAB_STATE_PARTIAL,
                "tls lowlock push keeps state mirror unchanged");
    expect_true(atomic_load_explicit(&slab->free_head_ref,
                                     UHM_ATOMIC_ACQUIRE) ==
                    free_head_ref_before,
                "tls lowlock push keeps free head mirror unchanged");
    expect_true(atomic_load_explicit(&slab->live_slots_count,
                                     UHM_ATOMIC_ACQUIRE) ==
                    live_count_before &&
                    atomic_load_explicit(&slab->live_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                        slab->live_slots,
                "tls reservation push keeps live capacity");
    expect_true(atomic_load_explicit(&slab->free_slots_count,
                                     UHM_ATOMIC_ACQUIRE) ==
                    free_count_before &&
                    atomic_load_explicit(&slab->free_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                        slab->free_slots,
                "tls reservation push keeps slab free capacity");
    expect_true(atomic_load_explicit(&slab->ctx_cached_slots,
                                     UHM_ATOMIC_ACQUIRE) ==
                    cached_count_before + 1u,
                "tls lowlock push updates authoritative cached count");
    cached_hint_base = test_local_page_hint_set_base(cached_page);
    expect_true(cache->local_page_hints[cached_hint_base].anchor ==
                        second_slot &&
                    cache->local_page_hints[cached_hint_base].record ==
                        cached_record,
                "tls first anchored push publishes page hint");
    expect_ok(api->check(heap, 0), "tls lowlock check after push");
    cache->local_page_hints[cached_hint_base].record = 0;
    expect_true(api->check(heap, 0) == UHM_ERR_CORRUPTION,
                "tls page hint missing record check catches corruption");
    cache->local_page_hints[cached_hint_base].record = cached_record;
    expect_ok(api->check(heap, 0), "tls page hint restored check");

    expect_true(test_shadow_page_span(heap, (uintptr_t)second_slot,
                                      sizeof(*second_slot), &cached_page,
                                      &cached_page_count) &&
                    cached_page_count == 1u,
                "tls lowlock cached slot page span");
    cached_record = test_page_record_at(heap, cached_page);
    expect_true(cached_record != 0,
                "tls lowlock cached slot page record");
    cached_generation = cached_record->generation;
    cached_record->generation = cached_generation + 1u;
    drift_alloc = api->malloc(heap, 64u);
    expect_true(drift_alloc == second,
                "tls private cache pop ignores diagnostic page drift");
    expect_true(cache->local_cache_head == first_slot &&
                    cache->local_cache_count == 1u &&
                    atomic_load_explicit(&cache->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "tls private cache pop avoids reader under page drift");
    expect_true(cache->local_page_hints[
                        test_local_page_hint_set_base(cached_page)].anchor ==
                    first_slot &&
                    cache->local_page_hints[
                        test_local_page_hint_set_base(cached_page)].record ==
                    cached_record,
                "tls private cache pop transfers page hint anchor");
    cached_record->generation = cached_generation;
    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_calls_before =
        test_atomic_u64_load(&heap->core.page_lock_range_calls);
    page_lock_shards_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    reader_seq_before = uhm_metadata_registered_reader_seq_load(heap);
    api->free(heap, drift_alloc);
    expect_true(test_atomic_u64_load(&heap->core.page_span_calls) ==
                    page_span_before &&
                    test_atomic_u64_load(&heap->core.page_lock_range_calls) ==
                        page_lock_calls_before &&
                    test_atomic_u64_load(&heap->core.page_lock_range_shards) ==
                        page_lock_shards_before,
                "tls anchored cache push skips page owner lock");
    expect_true(uhm_metadata_registered_reader_seq_load(heap) ==
                    reader_seq_before &&
                    atomic_load_explicit(&cache->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "tls anchored cache push skips metadata reader");
    expect_true(cache->local_cache_head == second_slot &&
                    cache->local_cache_count == 2u &&
                    cache->nonempty_class_mask ==
                        (UINT32_C(1) << second_slot->size_class) &&
                    cache->local_page_hints[cached_hint_base].anchor ==
                        first_slot &&
                    cache->local_page_hints[cached_hint_base].record ==
                        cached_record,
                "tls repeated anchored push preserves older cache anchor");

    lock_before = test_atomic_u64_load(&heap->lock.lock_calls);
    readers_before = uhm_metadata_registered_readers_load(heap);
    reader_seq_before = uhm_metadata_registered_reader_seq_load(heap);

    state_mirror_before = atomic_load_explicit(&slab->state_mirror,
                                               UHM_ATOMIC_ACQUIRE);
    free_head_ref_before = atomic_load_explicit(&slab->free_head_ref,
                                                UHM_ATOMIC_ACQUIRE);
    live_count_before = atomic_load_explicit(&slab->live_slots_count,
                                             UHM_ATOMIC_ACQUIRE);
    free_count_before = atomic_load_explicit(&slab->free_slots_count,
                                             UHM_ATOMIC_ACQUIRE);
    cached_count_before = atomic_load_explicit(&slab->ctx_cached_slots,
                                               UHM_ATOMIC_ACQUIRE);
    raw_lock_before = test_atomic_u64_load(&slab->raw_lock.lock_calls);
    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_calls_before =
        test_atomic_u64_load(&heap->core.page_lock_range_calls);
    page_lock_shards_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    atomic_store_explicit(&slab->free_slots_count, 0u,
                          UHM_ATOMIC_RELEASE);
    reuse = api->malloc(heap, 64u);
    atomic_store_explicit(&slab->free_slots_count, free_count_before,
                          UHM_ATOMIC_RELEASE);
    expect_true(reuse == second, "tls lowlock pop reuses expected slot");
    expect_true(test_atomic_u64_load(&heap->core.page_span_calls) ==
                    page_span_before,
                "tls private cache pop skips page owner probe");
    expect_true(test_atomic_u64_load(&heap->core.page_lock_range_calls) ==
                        page_lock_calls_before &&
                    test_atomic_u64_load(&heap->core.page_lock_range_shards) ==
                        page_lock_shards_before,
                "tls private cache pop skips page shard lock");
    expect_true(test_atomic_u64_load(&heap->lock.lock_calls) == lock_before,
                "tls pop ignores unrelated free mirror without heap lock");
    expect_true(test_atomic_u64_load(&slab->raw_lock.lock_calls) ==
                    raw_lock_before,
                "tls reservation pop avoids slab raw lock");
    expect_true(uhm_metadata_registered_readers_load(heap) == readers_before,
                "tls lowlock cache pop avoids registered reader count");
    expect_true(uhm_metadata_registered_reader_seq_load(heap) ==
                    reader_seq_before,
                "tls lowlock cache pop avoids registered reader seq");
    expect_true(atomic_load_explicit(&slab->state_mirror,
                                     UHM_ATOMIC_ACQUIRE) ==
                    state_mirror_before &&
                    state_mirror_before == UHM_SLAB_STATE_PARTIAL,
                "tls lowlock pop keeps state mirror unchanged");
    expect_true(atomic_load_explicit(&slab->free_head_ref,
                                     UHM_ATOMIC_ACQUIRE) ==
                    free_head_ref_before,
                "tls lowlock pop keeps free head mirror unchanged");
    expect_true(atomic_load_explicit(&slab->live_slots_count,
                                     UHM_ATOMIC_ACQUIRE) ==
                    live_count_before &&
                    atomic_load_explicit(&slab->live_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                        slab->live_slots,
                "tls reservation pop keeps live capacity");
    expect_true(atomic_load_explicit(&slab->free_slots_count,
                                     UHM_ATOMIC_ACQUIRE) ==
                    free_count_before &&
                    atomic_load_explicit(&slab->free_slots_count,
                                         UHM_ATOMIC_ACQUIRE) ==
                        slab->free_slots,
                "tls reservation pop keeps slab free capacity");
    expect_true(atomic_load_explicit(&slab->ctx_cached_slots,
                                     UHM_ATOMIC_ACQUIRE) ==
                    cached_count_before - 1u,
                "tls lowlock pop updates authoritative cached count");
    expect_ok(api->check(heap, 0), "tls lowlock check after pop");

    empty_cache_alloc = api->malloc(heap, 64u);
    expect_true(empty_cache_alloc == first && cache->local_cache_count == 0u &&
                    cache->local_cache_head == 0 &&
                    cache->nonempty_class_mask == 0u,
                "tls lowlock last cache slot pop reaches empty cache");
    lock_before = test_atomic_u64_load(&heap->lock.lock_calls);
    page_span_before = test_atomic_u64_load(&heap->core.page_span_calls);
    page_lock_calls_before =
        test_atomic_u64_load(&heap->core.page_lock_range_calls);
    page_lock_shards_before =
        test_atomic_u64_load(&heap->core.page_lock_range_shards);
    api->free(heap, empty_cache_alloc);
    expect_true(test_atomic_u64_load(&heap->core.page_span_calls) ==
                    page_span_before + 1u &&
                    test_atomic_u64_load(&heap->core.page_lock_range_calls) ==
                        page_lock_calls_before + 1u &&
                    test_atomic_u64_load(&heap->core.page_lock_range_shards) ==
                        page_lock_shards_before + 1u &&
                    test_atomic_u64_load(&heap->lock.lock_calls) == lock_before &&
                    cache->local_cache_count == 1u &&
                    cache->local_cache_head == first_slot &&
                    atomic_load_explicit(&cache->metadata_reader.depth,
                                         UHM_ATOMIC_ACQUIRE) == 0u,
                "tls lowlock empty cache push stays outside heap lock");
    expect_ok(api->check(heap, 0), "tls lowlock empty cache check");

    api->free(heap, reuse);
    api->free(heap, third);
    expect_ok(api->check(heap, 0), "tls lowlock final check");
    expect_ok(api->close(heap), "tls lowlock heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_tls_cache_thread_exit_flush_and_registry_pressure(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    enum { churn_thread_count = 16 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    tls_exit_args exit_args;
    tls_churn_args churn_args[churn_thread_count];
    void *main_ptr;
    void *reuse;
    uhm_slab_slot *main_slot;
    uhm_slab *slab;
    size_t i;
    _Atomic(uint32_t) churn_ready;
    _Atomic(uint32_t) churn_release;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "tls pressure heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "tls pressure arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "tls pressure usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.thread_registry_slots = 4u;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 2u;
    config.tls_cache_max_pages = 2u;

    expect_ok(uhm_open(&config, &heap), "tls pressure heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "tls pressure api");

    memset(&exit_args, 0, sizeof(exit_args));
    exit_args.api = api;
    exit_args.heap = heap;
#if defined(_WIN32)
    {
        HANDLE thread = CreateThread(0, 0, tls_exit_worker, &exit_args, 0, 0);
        expect_true(thread != 0, "tls exit create thread");
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "tls exit wait thread");
        CloseHandle(thread);
    }
#else
    {
        pthread_t thread;
        expect_true(pthread_create(&thread, 0, tls_exit_worker,
                                   &exit_args) == 0,
                    "tls exit create thread");
        expect_true(pthread_join(thread, 0) == 0, "tls exit join thread");
    }
#endif
    expect_true(exit_args.ok && exit_args.ptr != 0 && exit_args.slot != 0,
                "tls exit worker completed");
    slab = exit_args.slot->slab;
    expect_true(heap->core.tls_cache_head == 0 &&
                    heap->core.tls_cache_active_count == 0u,
                "tls exit destructor detaches cache");
    expect_true(slab->ctx_cached_slots == 0u &&
                    (exit_args.slot->flags & UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0,
                "tls exit destructor flushes slot");
    expect_ok(api->check(heap, 0), "tls exit heap check");
    reuse = api->malloc(heap, 64u);
    expect_true(reuse == exit_args.ptr,
                "tls exit flushed slot can be reused by heap");
    api->free(heap, reuse);

    main_ptr = api->malloc(heap, 96u);
    expect_true(main_ptr != 0, "tls pressure main malloc");
    main_slot = test_slab_slot_from_payload(main_ptr);
    api->free(heap, main_ptr);
    expect_true(heap->core.tls_cache_active_count == 1u &&
                    heap->core.tls_cache_head != 0 &&
                    heap->core.tls_cache_head->local_cache_head == main_slot,
                "tls pressure main cache occupies only registry slot");

    memset(churn_args, 0, sizeof(churn_args));
    atomic_init(&churn_ready, 0u);
    atomic_init(&churn_release, 0u);
    for (i = 0; i < churn_thread_count; ++i) {
        churn_args[i].api = api;
        churn_args[i].heap = heap;
        churn_args[i].id = (unsigned)i;
        churn_args[i].iterations = 240u;
        churn_args[i].ready_count = &churn_ready;
        churn_args[i].release = &churn_release;
    }
#if defined(_WIN32)
    {
        HANDLE threads[churn_thread_count];
        for (i = 0; i < churn_thread_count; ++i) {
            threads[i] = CreateThread(0, 0, tls_churn_worker,
                                      &churn_args[i], 0, 0);
            expect_true(threads[i] != 0, "tls churn create thread");
        }
        while (atomic_load_explicit(&churn_ready,
                                    UHM_ATOMIC_ACQUIRE) !=
               churn_thread_count) {
            test_thread_pause();
        }
        expect_true(heap->core.tls_cache_active_count == 4u,
                    "tls registry pressure fills all four cache slots");
        atomic_store_explicit(&churn_release, 1u, UHM_ATOMIC_RELEASE);
        for (i = 0; i < churn_thread_count; ++i) {
            expect_true(WaitForSingleObject(threads[i], INFINITE) ==
                            WAIT_OBJECT_0,
                        "tls churn wait thread");
            CloseHandle(threads[i]);
        }
    }
#else
    {
        pthread_t threads[churn_thread_count];
        for (i = 0; i < churn_thread_count; ++i) {
            expect_true(pthread_create(&threads[i], 0, tls_churn_worker,
                                       &churn_args[i]) == 0,
                        "tls churn create thread");
        }
        while (atomic_load_explicit(&churn_ready,
                                    UHM_ATOMIC_ACQUIRE) !=
               churn_thread_count) {
            test_thread_pause();
        }
        expect_true(heap->core.tls_cache_active_count == 4u,
                    "tls registry pressure fills all four cache slots");
        atomic_store_explicit(&churn_release, 1u, UHM_ATOMIC_RELEASE);
        for (i = 0; i < churn_thread_count; ++i) {
            expect_true(pthread_join(threads[i], 0) == 0,
                        "tls churn join thread");
        }
    }
#endif
    for (i = 0; i < churn_thread_count; ++i) {
        if (!churn_args[i].ok) {
            uint32_t corruption_site = atomic_load_explicit(
                &heap->core.debug_last_corruption_site,
                UHM_ATOMIC_ACQUIRE);
            fprintf(stderr,
                    "tls churn worker %u failed: stage=%u iteration=%zu "
                    "last_error=%d platform_error=%u corruption_site=%u\n",
                    churn_args[i].id, churn_args[i].fail_stage,
                    churn_args[i].fail_iteration,
                    (int)churn_args[i].last_error,
                    churn_args[i].platform_error, corruption_site);
        }
        expect_true(churn_args[i].ok, "tls churn worker completed");
    }
    expect_true(heap->core.tls_cache_active_count == 1u,
                "tls worker destructors leave only main cache active");
    expect_ok(api->check(heap, 0), "tls pressure heap check");
    expect_ok(api->close(heap), "tls pressure heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_tls_cache_heap_close_flushes_foreign_thread_cache(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    tls_hold_args args;
    uhm_slab *slab;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "tls foreign close heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(1024u * 1024u, page_size,
                                    &arena_mem) != 0,
                "tls foreign close arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "tls foreign close usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.thread_registry_slots = 2u;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 2u;

    expect_ok(uhm_open(&config, &heap), "tls foreign close heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "tls foreign close api");

    memset(&args, 0, sizeof(args));
    args.api = api;
    args.heap = heap;
    atomic_init(&args.ready, 0u);
    atomic_init(&args.release, 0u);
#if defined(_WIN32)
    {
        HANDLE thread = CreateThread(0, 0, tls_hold_worker, &args, 0, 0);
        expect_true(thread != 0, "tls foreign close create thread");
        while (atomic_load_explicit(&args.ready, UHM_ATOMIC_ACQUIRE) == 0u) {
            test_thread_pause();
        }
        expect_true(args.ok && args.ptr != 0 && args.slot != 0,
                    "tls foreign close worker ready");
        slab = args.slot->slab;
        expect_true(heap->core.tls_cache_active_count == 1u &&
                        slab->ctx_cached_slots == 1u,
                    "tls foreign close cache active before close");
        expect_ok(api->close(heap), "tls foreign close heap close");
        expect_true(heap->core.tls_cache_active_count == 0u &&
                        heap->core.tls_cache_head == 0,
                    "tls foreign close active registry cleared");
        expect_true(slab->ctx_cached_slots == 0u &&
                        (args.slot->flags &
                         UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0,
                    "tls foreign close slot flushed");
        atomic_store_explicit(&args.release, 1u, UHM_ATOMIC_RELEASE);
        expect_true(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0,
                    "tls foreign close wait thread");
        CloseHandle(thread);
    }
#else
    {
        pthread_t thread;
        expect_true(pthread_create(&thread, 0, tls_hold_worker, &args) == 0,
                    "tls foreign close create thread");
        while (atomic_load_explicit(&args.ready, UHM_ATOMIC_ACQUIRE) == 0u) {
            test_thread_pause();
        }
        expect_true(args.ok && args.ptr != 0 && args.slot != 0,
                    "tls foreign close worker ready");
        slab = args.slot->slab;
        expect_true(heap->core.tls_cache_active_count == 1u &&
                        slab->ctx_cached_slots == 1u,
                    "tls foreign close cache active before close");
        expect_ok(api->close(heap), "tls foreign close heap close");
        expect_true(heap->core.tls_cache_active_count == 0u &&
                        heap->core.tls_cache_head == 0,
                    "tls foreign close active registry cleared");
        expect_true(slab->ctx_cached_slots == 0u &&
                        (args.slot->flags &
                         UHM_SLAB_SLOT_FLAG_CTX_CACHE) == 0,
                    "tls foreign close slot flushed");
        atomic_store_explicit(&args.release, 1u, UHM_ATOMIC_RELEASE);
        expect_true(pthread_join(thread, 0) == 0,
                    "tls foreign close join thread");
    }
#endif

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_heap_close_batches_tls_cache_global_maintenance(
    const uhm_storage_info *heap_info) {
    enum { CACHE_COUNT = 9u };
    const size_t page_size = uhm_platform_system_page_size();
    const uint16_t size_class = 0u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    tls_churn_args args[CACHE_COUNT];
    _Atomic(uint32_t) ready_count;
    _Atomic(uint32_t) release;
    size_t state;
    size_t i;
#if defined(_WIN32)
    HANDLE threads[CACHE_COUNT];
#else
    pthread_t threads[CACHE_COUNT];
#endif

    expect_true(test_vm_reserve_commit_alloc(
                    heap_info->storage_size, heap_info->storage_alignment,
                    &heap_mem) != 0,
                "close tls batch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(
                    1024u * 1024u, page_size, &arena_mem) != 0,
                "close tls batch arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "close tls batch usermode vm init");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = 1024u * 1024u;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.thread_registry_slots = CACHE_COUNT;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 1u;
    config.tls_cache_max_pages = 1u;
    expect_ok(uhm_open(&config, &heap), "close tls batch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "close tls batch api");

    memset(args, 0, sizeof(args));
    atomic_init(&ready_count, 0u);
    atomic_init(&release, 0u);
    for (i = 0u; i < CACHE_COUNT; ++i) {
        args[i].api = api;
        args[i].heap = heap;
        args[i].id = (unsigned)i;
        args[i].iterations = 1u;
        args[i].ready_count = &ready_count;
        args[i].release = &release;
#if defined(_WIN32)
        threads[i] = CreateThread(0, 0, tls_churn_worker, &args[i], 0, 0);
        expect_true(threads[i] != 0, "close tls batch create thread");
#else
        expect_true(pthread_create(&threads[i], 0, tls_churn_worker,
                                   &args[i]) == 0,
                    "close tls batch create thread");
#endif
    }
    while (atomic_load_explicit(&ready_count, UHM_ATOMIC_ACQUIRE) !=
           CACHE_COUNT) {
        test_thread_pause();
    }
    expect_true(heap->core.tls_cache_active_count == CACHE_COUNT,
                "close tls batch all caches active");
    heap->core.slab_empty_committed_cache_high_water[size_class] =
        uhm_slab_empty_committed_cache_state_make(
            UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER, 0u);

    expect_ok(api->close(heap), "close tls batch heap close");
    state = heap->core.slab_empty_committed_cache_high_water[size_class];
    expect_true(
        uhm_slab_empty_committed_cache_state_high_water(state) ==
                UHM_SLAB_EMPTY_COMMITTED_CACHE_ADAPTIVE_MAX_HIGH_WATER &&
            uhm_slab_empty_committed_cache_state_idle_drains(state) == 1u,
        "close tls batch advances global maintenance once");
    expect_true(heap->core.tls_cache_active_count == 0u &&
                    heap->core.tls_cache_head == 0,
                "close tls batch detaches all caches");

    atomic_store_explicit(&release, 1u, UHM_ATOMIC_RELEASE);
    for (i = 0u; i < CACHE_COUNT; ++i) {
#if defined(_WIN32)
        expect_true(WaitForSingleObject(threads[i], INFINITE) ==
                        WAIT_OBJECT_0,
                    "close tls batch wait thread");
        CloseHandle(threads[i]);
#else
        expect_true(pthread_join(threads[i], 0) == 0,
                    "close tls batch join thread");
#endif
        expect_true(args[i].ok, "close tls batch worker completed");
    }

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_numa_first_touch_preserves_uninitialized_payload(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 2u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    unsigned char *small;
    unsigned char *large;
    unsigned char *zeroed;
    void *map = 0;

    memset(&state, 0, sizeof(state));
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "first-touch heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "first-touch arena alloc");
    memset(arena_mem.aligned, 0xcc, arena_size);
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "first-touch usermode vm init");
    vm.user = &state;
    vm.numa_hint = fault_vm_numa_hint;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.numa_policy = UHM_NUMA_POLICY_FIRST_TOUCH;

    expect_ok(uhm_open(&config, &heap), "first-touch heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "first-touch api");

    small = (unsigned char *)api->malloc(heap, 64u);
    large = (unsigned char *)api->malloc(heap, page_size * 2u);
    expect_true(small != 0 && large != 0, "first-touch mallocs");
    expect_true(test_bytes_equal(small, 64u, 0xcc),
                "first-touch small malloc payload not prewritten");
    expect_true(test_bytes_equal(large, 128u, 0xcc),
                "first-touch large malloc payload not prewritten");

    expect_ok(api->mmap(heap, &map, page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "first-touch mmap");
    expect_true(test_bytes_equal(map, page_size, 0xcc),
                "first-touch mmap payload not prewritten");

    zeroed = (unsigned char *)api->calloc(heap, 16u, 4u);
    expect_true(zeroed != 0, "first-touch calloc");
    expect_true(test_bytes_equal(zeroed, 64u, 0u),
                "first-touch calloc only explicit zero range");
    expect_true(state.numa_hint_count == 0,
                "first-touch policy does not issue explicit numa hint");

    api->free(heap, zeroed);
    expect_ok(api->munmap(heap, map, page_size), "first-touch munmap");
    api->free(heap, small);
    api->free(heap, large);
    expect_ok(api->check(heap, 0), "first-touch heap check");
    expect_ok(api->close(heap), "first-touch heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_numa_policy_flag_fuzz_long_run(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    const size_t fixed_pages = 16u;
    enum { temp_capacity = 48, iterations = 512 };
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uintptr_t fixed_base;
    void *fixed_addr;
    void *temps[temp_capacity];
    size_t temp_sizes[temp_capacity];
    size_t temp_count = 0;
    uint64_t rng = 0x714f4d41534e5548ull;
    size_t i;

    static const uint32_t policies[] = {
        UHM_NUMA_POLICY_DEFAULT,
        UHM_NUMA_POLICY_FIRST_TOUCH,
        UHM_NUMA_POLICY_PREFERRED,
        UHM_NUMA_POLICY_BIND
    };

    memset(&state, 0, sizeof(state));
    state.platform_error = test_platform_error_code();
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                    heap_info->storage_alignment,
                                    &heap_mem) != 0,
                "numa fuzz heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size, &arena_mem) != 0,
                "numa fuzz arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(
                  &vm, page_size, page_size, UHM_VM_DECOMMIT_ZERO_FILLS),
              "numa fuzz usermode vm init");
    vm.user = &state;
    vm.capabilities |= UHM_VM_CAN_NUMA_PREFERRED | UHM_VM_CAN_NUMA_BIND;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    vm.protect = fault_vm_protect;
    vm.fixed_remap = fault_vm_fixed_remap;
    vm.numa_hint = fault_vm_numa_hint;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.heap_storage = heap_mem.aligned;
    config.heap_storage_size = heap_info->storage_size;
    config.arena_base = arena_mem.aligned;
    config.arena_size = arena_size;
    config.arena_flags = UHM_ARENA_PRE_RESERVED;
    config.vm = vm;
    config.numa_policy = UHM_NUMA_POLICY_FIRST_TOUCH;
    config.preferred_numa_node = -1;

    expect_ok(uhm_open(&config, &heap), "numa fuzz heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mmap_ex != 0, "numa fuzz api");

    fixed_base = test_align_up_uintptr(
        (uintptr_t)arena_mem.aligned + arena_size / 2u, page_size);
    fixed_addr = (void *)fixed_base;
    expect_ok(api->mmap(heap, &fixed_addr, fixed_pages * page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
              "numa fuzz fixed seed map");
    expect_true(fixed_addr == (void *)fixed_base,
                "numa fuzz fixed seed address");

    memset(temps, 0, sizeof(temps));
    memset(temp_sizes, 0, sizeof(temp_sizes));
    for (i = 0; i < iterations; ++i) {
        uint64_t r = test_next_u64(&rng);
        uint32_t policy = policies[r & 3u];
        int32_t node = (int32_t)((r >> 8) & 7u);
        uint32_t prot = ((r >> 16) & 1u) != 0
                            ? UHM_PROT_READ
                            : (UHM_PROT_READ | UHM_PROT_WRITE);
        size_t offset_pages = (size_t)((r >> 20) % fixed_pages);
        size_t span_pages =
            1u + (size_t)((r >> 28) % (fixed_pages - offset_pages));
        if (span_pages > 4u) {
            span_pages = 4u;
        }

        switch ((unsigned)((r >> 36) % 4u)) {
        case 0: {
            uhm_result mprotect_result = api->mprotect(
                heap, (void *)(fixed_base + offset_pages * page_size),
                span_pages * page_size, prot);
            expect_ok(mprotect_result, "numa fuzz mprotect live subrange");
            break;
        }
        case 1: {
            uhm_mmap_options options;
            memset(&options, 0, sizeof(options));
            options.size = sizeof(options);
            options.abi_major = UHM_ABI_MAJOR;
            options.abi_minor = UHM_ABI_MINOR;
            options.addr = (void *)(fixed_base + offset_pages * page_size);
            options.map_size = span_pages * page_size;
            options.prot = prot;
            options.map_flags = UHM_MAP_PRIVATE | UHM_MAP_FIXED;
            options.numa_policy = policy;
            options.numa_node = node;
            expect_ok(api->mmap_ex(heap, &options),
                      "numa fuzz fixed replace live subrange");
            expect_true(options.addr ==
                            (void *)(fixed_base + offset_pages * page_size),
                        "numa fuzz fixed replace keeps address");
            break;
        }
        case 2: {
            uhm_mmap_options options;
            memset(&options, 0, sizeof(options));
            options.size = sizeof(options);
            options.abi_major = UHM_ABI_MAJOR;
            options.abi_minor = UHM_ABI_MINOR;
            options.addr = (void *)(fixed_base + offset_pages * page_size);
            options.map_size = span_pages * page_size;
            options.prot = prot;
            options.map_flags = UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE;
            options.numa_policy = policy;
            options.numa_node = node;
            expect_true(api->mmap_ex(heap, &options) == UHM_ERR_ALREADY_MAPPED,
                        "numa fuzz noreplace rejects live subrange");
            break;
        }
        default: {
            uhm_mmap_options options;
            uhm_result result;
            int strong_no_commit =
                ((r >> 44) & 1u) != 0 &&
                (policy == UHM_NUMA_POLICY_PREFERRED ||
                 policy == UHM_NUMA_POLICY_BIND);
            if (temp_count == temp_capacity) {
                size_t j;
                for (j = 0; j < temp_count; ++j) {
                    expect_ok(api->munmap(heap, temps[j], temp_sizes[j]),
                              "numa fuzz temp drain");
                }
                temp_count = 0;
            }
            memset(&options, 0, sizeof(options));
            options.size = sizeof(options);
            options.abi_major = UHM_ABI_MAJOR;
            options.abi_minor = UHM_ABI_MINOR;
            options.map_size = (1u + (size_t)((r >> 48) & 1u)) * page_size;
            options.prot = prot;
            options.map_flags = UHM_MAP_PRIVATE;
            if (((r >> 44) & 1u) != 0) {
                options.map_flags |= UHM_MAP_NO_COMMIT;
            }
            options.numa_policy = policy;
            options.numa_node = node;
            result = api->mmap_ex(heap, &options);
            if (strong_no_commit) {
                expect_true(result == UHM_ERR_UNSUPPORTED,
                            "numa fuzz strong no-commit unsupported");
                expect_true(options.addr == 0,
                            "numa fuzz no-commit failure does not publish");
            } else {
                expect_ok(result, "numa fuzz auto mmap");
                expect_true(options.addr != 0, "numa fuzz auto mmap address");
                temps[temp_count] = options.addr;
                temp_sizes[temp_count] = options.map_size;
                ++temp_count;
            }
            break;
        }
        }

        if ((i % 32u) == 31u) {
            expect_ok(api->check(heap, 0), "numa fuzz periodic check");
        }
    }

    expect_true(state.numa_hint_count != 0,
                "numa fuzz exercised strong numa hints");
    expect_ok(api->check(heap, 0), "numa fuzz final check before cleanup");
    for (i = 0; i < temp_count; ++i) {
        expect_ok(api->munmap(heap, temps[i], temp_sizes[i]),
                  "numa fuzz temp cleanup");
    }
    expect_ok(api->munmap(heap, (void *)fixed_base,
                          fixed_pages * page_size),
              "numa fuzz fixed cleanup");
    expect_ok(api->check(heap, 0), "numa fuzz final check");
    expect_ok(api->close(heap), "numa fuzz heap close");

    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

