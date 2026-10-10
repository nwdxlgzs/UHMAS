static void test_mremap_grow_commit_numa_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *same;
    uint64_t vm_lock_held_before;
    uint64_t vm_commit_lock_held_before;
    uint64_t vm_numa_hint_lock_held_before;
    uint64_t vm_commit_calls_before;
    uint64_t vm_numa_hint_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mremap grow unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mremap grow unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mremap grow unlock vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.numa_hint = reentrant_check_vm_numa_hint;
    vm.capabilities |= UHM_VM_CAN_NUMA_PREFERRED;

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
    config.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    config.preferred_numa_node = 3;

    expect_ok(uhm_open(&config, &heap), "mremap grow unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mremap != 0, "mremap grow unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mremap grow unlock initial mmap");
    expect_true(map != 0, "mremap grow unlock initial address");
    same = map;
    expect_ok(api->mremap(heap, &same, page_size * 2u, page_size, 0u),
              "mremap grow unlock shrink setup");
    expect_true(same == map, "mremap grow unlock shrink keeps address");

    state.check_result = UHM_ERR_CORRUPTION;
    state.commit_count = 0;
    state.numa_hint_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_commit_lock_held_before = heap->core.vm_commit_lock_held_calls;
    vm_numa_hint_lock_held_before = heap->core.vm_numa_hint_lock_held_calls;
    vm_commit_calls_before = heap->core.vm_commit_calls;
    vm_numa_hint_calls_before = heap->core.vm_numa_hint_calls;

    state.run_check = 1;
    same = map;
    expect_ok(api->mremap(heap, &same, page_size, page_size + 1u, 0u),
              "mremap grow unlock grow");
    state.run_check = 0;
    expect_true(same == map, "mremap grow unlock grow keeps address");
    expect_true(state.commit_count == 1,
                "mremap grow unlock commit callback called");
    expect_true(state.numa_hint_count == 1,
                "mremap grow unlock numa callback called");
    expect_true(state.check_calls == 2 && state.check_failures == 0,
                "mremap grow unlock reentrant checks ok");
    expect_ok(state.check_result, "mremap grow unlock last check ok");
    expect_true(heap->core.vm_commit_calls == vm_commit_calls_before + 1u,
                "mremap grow unlock commit call counted");
    expect_true(heap->core.vm_numa_hint_calls ==
                    vm_numa_hint_calls_before + 1u,
                "mremap grow unlock numa call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_commit_lock_held_calls ==
                        vm_commit_lock_held_before &&
                    heap->core.vm_numa_hint_lock_held_calls ==
                        vm_numa_hint_lock_held_before,
                "mremap grow unlock platform calls not counted lock-held");
    memset((unsigned char *)map + page_size, 0x42, page_size);
    expect_ok(api->check(heap, 0), "mremap grow unlock check after");

    expect_ok(api->munmap(heap, map, page_size * 2u),
              "mremap grow unlock munmap");
    expect_ok(api->close(heap), "mremap grow unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mremap_shrink_decommit_platform_unlock(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *same;
    uint64_t vm_lock_held_before;
    uint64_t vm_decommit_lock_held_before;
    uint64_t vm_decommit_calls_before;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mremap shrink unlock heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mremap shrink unlock arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mremap shrink unlock vm init");
    vm.user = &state;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "mremap shrink unlock open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mremap != 0, "mremap shrink unlock api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, page_size * 2u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mremap shrink unlock initial mmap");
    expect_true(map != 0, "mremap shrink unlock initial address");
    memset(map, 0x33, page_size * 2u);

    state.check_result = UHM_ERR_CORRUPTION;
    state.decommit_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    vm_lock_held_before = heap->core.vm_lock_held_calls;
    vm_decommit_lock_held_before = heap->core.vm_decommit_lock_held_calls;
    vm_decommit_calls_before = heap->core.vm_decommit_calls;

    state.run_check = 1;
    same = map;
    expect_ok(api->mremap(heap, &same, page_size * 2u, page_size, 0u),
              "mremap shrink unlock shrink");
    state.run_check = 0;
    expect_true(same == map, "mremap shrink unlock keeps address");
    expect_true(state.decommit_count == 1,
                "mremap shrink unlock decommit callback called");
    expect_true(state.check_calls == 1 && state.check_failures == 0,
                "mremap shrink unlock reentrant check ok");
    expect_ok(state.check_result, "mremap shrink unlock last check ok");
    expect_true(heap->core.vm_decommit_calls == vm_decommit_calls_before + 1u,
                "mremap shrink unlock decommit call counted");
    expect_true(heap->core.vm_lock_held_calls == vm_lock_held_before &&
                    heap->core.vm_decommit_lock_held_calls ==
                        vm_decommit_lock_held_before,
                "mremap shrink unlock decommit not counted lock-held");
    expect_ok(api->check(heap, 0), "mremap shrink unlock check after");

    expect_ok(api->munmap(heap, map, page_size),
              "mremap shrink unlock cleanup munmap");
    expect_ok(api->close(heap), "mremap shrink unlock close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mremap_shrink_extends_adjacent_free_extent(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *same;
    void *right_page;
    uhm_free_extent *extent;

    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mremap shrink adjacent heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mremap shrink adjacent arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mremap shrink adjacent vm init");

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

    expect_ok(uhm_open(&config, &heap), "mremap shrink adjacent open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mremap != 0,
                "mremap shrink adjacent api");

    expect_ok(api->mmap(heap, &map, page_size * 3u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mremap shrink adjacent initial mmap");
    expect_true(map != 0, "mremap shrink adjacent initial address");
    right_page = (void *)((uintptr_t)map + 2u * page_size);
    expect_ok(api->munmap(heap, right_page, page_size),
              "mremap shrink adjacent setup right free");
    extent = test_find_free_extent_containing(heap, right_page);
    expect_true(extent != 0 && extent->addr == (uintptr_t)right_page,
                "mremap shrink adjacent right extent exists");
    expect_ok(api->check(heap, 0), "mremap shrink adjacent check before");

    heap->core.fault_mmap_free_insert_countdown = 1u;
    same = map;
    expect_ok(api->mremap(heap, &same, page_size * 2u, page_size, 0u),
              "mremap shrink adjacent shrink");
    expect_true(same == map, "mremap shrink adjacent keeps address");
    expect_true(heap->core.fault_mmap_free_insert_countdown == 1u,
                "mremap shrink adjacent skips free insert");
    heap->core.fault_mmap_free_insert_countdown = 0;

    extent = test_find_free_extent_containing(
        heap, (void *)((uintptr_t)map + page_size));
    expect_true(extent != 0 && extent->addr == (uintptr_t)map + page_size &&
                    extent->size >= page_size * 2u,
                "mremap shrink adjacent extended right extent");
    expect_ok(api->check(heap, 0), "mremap shrink adjacent check after");
    expect_ok(api->munmap(heap, map, page_size),
              "mremap shrink adjacent cleanup");
    expect_ok(api->close(heap), "mremap shrink adjacent close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mremap_maymove_source_claim(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    reentrant_check_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *moved;
    uhm_vma *moved_vma;

    memset(&state, 0, sizeof(state));
    state.check_result = UHM_ERR_CORRUPTION;
    state.mprotect_probe_result = UHM_OK;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mremap maymove claim heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mremap maymove claim arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mremap maymove claim vm init");
    vm.user = &state;
    vm.commit = reentrant_check_vm_commit;
    vm.decommit = reentrant_check_vm_decommit;

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

    expect_ok(uhm_open(&config, &heap), "mremap maymove claim open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mremap != 0,
                "mremap maymove claim api");
    state.heap = heap;
    state.api = api;

    expect_ok(api->mmap(heap, &map, page_size + 1u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mremap maymove claim mmap");
    expect_true(map != 0, "mremap maymove claim address");
    memset(map, 0x6du, page_size * 2u);
    expect_ok(api->check(heap, 0), "mremap maymove claim check before");

    state.check_result = UHM_ERR_CORRUPTION;
    state.commit_count = 0;
    state.decommit_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    state.mprotect_probe_calls = 0;
    state.mprotect_probe_result = UHM_OK;
    state.mprotect_probe_addr = map;
    state.mprotect_probe_size = page_size;
    state.mprotect_probe_prot = UHM_PROT_READ;
    state.run_check = 1;
    state.run_mprotect_probe = 1;

    moved = map;
    expect_ok(api->mremap(heap, &moved, page_size + 1u, page_size * 3u,
                          UHM_REMAP_MAYMOVE),
              "mremap maymove claim relocate");
    state.run_mprotect_probe = 0;
    state.run_check = 0;

    expect_true(moved != map, "mremap maymove claim moved");
    expect_true(((unsigned char *)moved)[0] == 0x6du &&
                    ((unsigned char *)moved)[page_size * 2u - 1u] == 0x6du,
                "mremap maymove claim copied payload");
    expect_true(state.commit_count == 1,
                "mremap maymove claim commit callback called");
    expect_true(state.decommit_count == 1,
                "mremap maymove claim decommit callback called");
    expect_true(state.mprotect_probe_calls == 2,
                "mremap maymove claim source probe called");
    expect_true(state.mprotect_probe_result == UHM_ERR_PROTECTION,
                "mremap maymove claim source probe blocked");
    expect_true(state.check_calls == 2 && state.check_failures == 0,
                "mremap maymove claim reentrant checks ok");
    expect_ok(state.check_result, "mremap maymove claim last check ok");
    expect_true(api->munmap(heap, map, page_size) == UHM_ERR_NOT_MAPPED,
                "mremap maymove claim old address unmapped");
    expect_ok(api->check(heap, 0), "mremap maymove claim check after");

    expect_ok(api->munmap(heap, moved, page_size * 3u),
              "mremap maymove claim cleanup");

    expect_ok(api->mmap(heap, &map, page_size + 1u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mremap maymove claim readonly mmap");
    expect_true(map != 0, "mremap maymove claim readonly address");
    memset(map, 0x72u, page_size * 2u);
    expect_ok(api->mprotect(heap, map, page_size * 2u, UHM_PROT_READ),
              "mremap maymove claim readonly setup");

    state.check_result = UHM_ERR_CORRUPTION;
    state.commit_count = 0;
    state.decommit_count = 0;
    state.check_calls = 0;
    state.check_failures = 0;
    state.mprotect_probe_calls = 0;
    state.mprotect_probe_result = UHM_OK;
    state.mprotect_probe_addr = map;
    state.mprotect_probe_size = page_size;
    state.mprotect_probe_prot = UHM_PROT_READ | UHM_PROT_WRITE;
    state.run_check = 1;
    state.run_mprotect_probe = 1;

    moved = map;
    expect_ok(api->mremap(heap, &moved, page_size + 1u, page_size * 3u,
                          UHM_REMAP_MAYMOVE),
              "mremap maymove claim readonly relocate");
    state.run_mprotect_probe = 0;
    state.run_check = 0;

    expect_true(moved != map, "mremap maymove claim readonly moved");
    expect_true(((unsigned char *)moved)[0] == 0x72u &&
                    ((unsigned char *)moved)[page_size * 2u - 1u] == 0x72u,
                "mremap maymove claim readonly copied payload");
    moved_vma = test_find_vma_containing(heap, moved);
    expect_true(moved_vma != 0 && moved_vma->addr == (uintptr_t)moved &&
                    moved_vma->size == page_size * 3u &&
                    moved_vma->prot == UHM_PROT_READ,
                "mremap maymove claim readonly restores target prot");
    expect_true(state.commit_count == 1,
                "mremap maymove claim readonly commit callback called");
    expect_true(state.decommit_count == 1,
                "mremap maymove claim readonly decommit callback called");
    expect_true(state.mprotect_probe_calls == 2,
                "mremap maymove claim readonly source probe called");
    expect_true(state.mprotect_probe_result == UHM_ERR_PROTECTION,
                "mremap maymove claim readonly source probe blocked");
    expect_true(state.check_calls == 2 && state.check_failures == 0,
                "mremap maymove claim readonly reentrant checks ok");
    expect_ok(state.check_result,
              "mremap maymove claim readonly last check ok");
    expect_true(api->munmap(heap, map, page_size) == UHM_ERR_NOT_MAPPED,
                "mremap maymove claim readonly old address unmapped");
    expect_ok(api->check(heap, 0),
              "mremap maymove claim readonly check after");

    expect_ok(api->munmap(heap, moved, page_size * 3u),
              "mremap maymove claim readonly cleanup");
    expect_ok(api->close(heap), "mremap maymove claim close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

static void test_mremap_maymove_restore_failure_rollback(
    const uhm_storage_info *heap_info) {
    const size_t page_size = uhm_platform_system_page_size();
    const size_t arena_size = 4u * 1024u * 1024u;
    test_alloc heap_mem = {0};
    test_alloc arena_mem = {0};
    fault_vm_state state;
    uhm_vm_ops vm;
    uhm_config config;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *map = 0;
    void *moved;
    uhm_stats before;
    uhm_stats after;
    uint32_t platform_error;
    uint32_t source_platform_error;
    uint32_t commit_platform_error;
    uint64_t generation_before;

    memset(&state, 0, sizeof(state));
    platform_error = test_platform_error_code();
    source_platform_error = platform_error + 97u;
    commit_platform_error = platform_error + 193u;
    state.platform_error = platform_error;
    expect_true(test_vm_reserve_commit_alloc(heap_info->storage_size,
                                             heap_info->storage_alignment,
                                             &heap_mem) != 0,
                "mremap maymove rollback heap storage alloc");
    expect_true(test_vm_reserve_commit_alloc(arena_size, page_size,
                                             &arena_mem) != 0,
                "mremap maymove rollback arena alloc");
    expect_ok(uhm_platform_usermode_vm_ops_init(&vm, page_size, page_size,
                                                UHM_VM_DECOMMIT_ZERO_FILLS),
              "mremap maymove rollback vm init");
    vm.user = &state;
    vm.commit = fault_vm_commit;
    vm.decommit = fault_vm_decommit;
    vm.protect = fault_vm_protect;
    vm.fixed_remap = fault_vm_fixed_remap;

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

    expect_ok(uhm_open(&config, &heap), "mremap maymove rollback open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mremap != 0,
                "mremap maymove rollback api");

    expect_ok(api->mmap(heap, &map, page_size + 1u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "mremap maymove rollback mmap");
    expect_true(map != 0, "mremap maymove rollback address");
    memset(map, 0x4du, page_size * 2u);
    expect_ok(api->mprotect(heap, map, page_size * 2u, UHM_PROT_READ),
              "mremap maymove rollback readonly setup");
    expect_ok(api->check(heap, 0), "mremap maymove rollback check before");

    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before),
              "mremap maymove rollback stats before");
    state.commit_count = 0;
    state.decommit_count = 0;
    state.protect_count = 0;
    state.fail_protect = 1;
    moved = map;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mremap(heap, &moved, page_size + 1u, page_size * 3u,
                            UHM_REMAP_MAYMOVE) == UHM_ERR_PLATFORM,
                "mremap maymove rollback restore protect failure");
    expect_true(moved == map, "mremap maymove rollback keeps old address");
    expect_generation_advanced(heap, generation_before,
                               "mremap maymove rollback advances generation");
    test_set_platform_error(platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, platform_error,
                      "mremap maymove rollback captures platform error");
    state.fail_protect = 0;
    expect_true(state.commit_count == 1,
                "mremap maymove rollback committed temp target");
    expect_true(state.protect_count == 1,
                "mremap maymove rollback failed target protect");
    expect_true(state.decommit_count == 1,
                "mremap maymove rollback decommitted temp target");

    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after),
              "mremap maymove rollback stats after");
    expect_true(after.mmap_live_bytes == before.mmap_live_bytes,
                "mremap maymove rollback keeps live mmap bytes");
    expect_true(after.committed_bytes == before.committed_bytes,
                "mremap maymove rollback removes temp commit stats");
    expect_true(after.decommitted_bytes == before.decommitted_bytes,
                "mremap maymove rollback hides temp decommit stats");
    expect_true(after.logical_decommitted_bytes ==
                    before.logical_decommitted_bytes,
                "mremap maymove rollback hides logical decommit stats");
    expect_true(after.physical_released_bytes ==
                    before.physical_released_bytes,
                "mremap maymove rollback hides physical decommit stats");
    expect_true(test_has_exact_vma(heap, (uintptr_t)map, page_size * 2u,
                                   UHM_PROT_READ),
                "mremap maymove rollback restores source VMA");
    expect_true(((unsigned char *)map)[0] == 0x4du &&
                    ((unsigned char *)map)[page_size * 2u - 1u] == 0x4du,
                "mremap maymove rollback preserves source payload");
    expect_ok(api->check(heap, 0), "mremap maymove rollback check after");

    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before),
              "mremap maymove source rollback stats before");
    fault_vm_clear_rules(&state);
    state.platform_error = source_platform_error;
    state.commit_count = 0;
    state.decommit_count = 0;
    state.protect_count = 0;
    state.fail_ops = FAULT_VM_OP_DECOMMIT;
    state.fail_on_match = 1u;
    moved = map;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mremap(heap, &moved, page_size + 1u, page_size * 3u,
                            UHM_REMAP_MAYMOVE) == UHM_ERR_PLATFORM,
                "mremap maymove rollback source decommit failure");
    expect_true(moved == map,
                "mremap maymove source rollback keeps old address");
    expect_generation_advanced(
        heap, generation_before,
        "mremap maymove source rollback advances generation");
    test_set_platform_error(source_platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, source_platform_error,
                      "mremap maymove source rollback captures platform error");
    expect_true(state.commit_count == 1,
                "mremap maymove source rollback committed temp target");
    expect_true(state.protect_count == 1,
                "mremap maymove source rollback restored target prot");
    expect_true(state.decommit_count == 2,
                "mremap maymove source rollback failed source and temp decommit");
    expect_true(state.fault_match_count == 2u,
                "mremap maymove source rollback faults only first decommit");

    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after),
              "mremap maymove source rollback stats after");
    expect_true(after.mmap_live_bytes == before.mmap_live_bytes,
                "mremap maymove source rollback keeps live mmap bytes");
    expect_true(after.committed_bytes == before.committed_bytes,
                "mremap maymove source rollback removes temp commit stats");
    expect_true(after.decommitted_bytes == before.decommitted_bytes,
                "mremap maymove source rollback hides temp decommit stats");
    expect_true(after.logical_decommitted_bytes ==
                    before.logical_decommitted_bytes,
                "mremap maymove source rollback hides logical decommit stats");
    expect_true(after.physical_released_bytes ==
                    before.physical_released_bytes,
                "mremap maymove source rollback hides physical decommit stats");
    expect_true(test_has_exact_vma(heap, (uintptr_t)map, page_size * 2u,
                                   UHM_PROT_READ),
                "mremap maymove source rollback restores source VMA");
    expect_true(((unsigned char *)map)[0] == 0x4du &&
                    ((unsigned char *)map)[page_size * 2u - 1u] == 0x4du,
                "mremap maymove source rollback preserves source payload");
    expect_ok(api->check(heap, 0),
              "mremap maymove source rollback check after");

    fault_vm_clear_rules(&state);
    state.platform_error = commit_platform_error;
    state.commit_count = 0;
    state.decommit_count = 0;
    state.protect_count = 0;
    state.fail_ops = FAULT_VM_OP_COMMIT;
    state.fail_on_match = 1u;
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before),
              "mremap maymove commit rollback stats before");
    moved = map;
    generation_before = uhm_heap_generation_load(heap);
    expect_true(api->mremap(heap, &moved, page_size + 1u, page_size * 3u,
                            UHM_REMAP_MAYMOVE) == UHM_ERR_PLATFORM,
                "mremap maymove rollback new target commit failure");
    expect_true(moved == map,
                "mremap maymove commit rollback keeps old address");
    expect_generation_advanced(
        heap, generation_before,
        "mremap maymove commit rollback advances generation");
    test_set_platform_error(commit_platform_error + 1u);
    expect_last_error(api, heap, UHM_ERR_PLATFORM, commit_platform_error,
                      "mremap maymove commit rollback captures platform error");
    expect_true(state.commit_count == 1,
                "mremap maymove commit rollback attempted target commit");
    expect_true(state.decommit_count == 1,
                "mremap maymove commit rollback decommitted partial target");
    expect_true(state.protect_count == 0,
                "mremap maymove commit rollback did not protect target");

    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after),
              "mremap maymove commit rollback stats after");
    expect_true(after.mmap_live_bytes == before.mmap_live_bytes,
                "mremap maymove commit rollback keeps live mmap bytes");
    expect_true(after.committed_bytes == before.committed_bytes,
                "mremap maymove commit rollback keeps committed bytes");
    expect_true(after.decommitted_bytes == before.decommitted_bytes,
                "mremap maymove commit rollback hides rollback decommit stats");
    expect_true(after.logical_decommitted_bytes ==
                    before.logical_decommitted_bytes,
                "mremap maymove commit rollback hides logical decommit stats");
    expect_true(after.physical_released_bytes ==
                    before.physical_released_bytes,
                "mremap maymove commit rollback hides physical decommit stats");
    expect_true(test_has_exact_vma(heap, (uintptr_t)map, page_size * 2u,
                                   UHM_PROT_READ),
                "mremap maymove commit rollback restores source VMA");
    expect_true(((unsigned char *)map)[0] == 0x4du &&
                    ((unsigned char *)map)[page_size * 2u - 1u] == 0x4du,
                "mremap maymove commit rollback preserves source payload");
    expect_ok(api->check(heap, 0),
              "mremap maymove commit rollback check after");

    fault_vm_clear_rules(&state);
    state.commit_count = 0;
    state.decommit_count = 0;
    state.protect_count = 0;
    heap->core.fault_mmap_free_insert_countdown = 1u;
    moved = map;
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before),
              "mremap maymove free publish degrade stats before");
    expect_ok(api->mremap(heap, &moved, page_size + 1u, page_size * 3u,
                          UHM_REMAP_MAYMOVE),
              "mremap maymove free publish degrade move");
    expect_true(moved != map, "mremap maymove free publish degrade moved");
    expect_true(heap->core.fault_mmap_free_insert_countdown == 0,
                "mremap maymove free publish degrade hook consumed");
    expect_true(((unsigned char *)moved)[0] == 0x4du &&
                    ((unsigned char *)moved)[page_size * 2u - 1u] == 0x4du,
                "mremap maymove free publish degrade copied payload");
    expect_true(state.commit_count == 1,
                "mremap maymove free publish degrade target commit");
    expect_true(state.protect_count == 1,
                "mremap maymove free publish degrade target protect");
    expect_true(state.decommit_count == 1,
                "mremap maymove free publish degrade source decommit");
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after),
              "mremap maymove free publish degrade stats after");
    expect_true(after.mmap_live_bytes == before.mmap_live_bytes + page_size,
                "mremap maymove free publish degrade live size");
    expect_true(api->munmap(heap, map, page_size) == UHM_ERR_NOT_MAPPED,
                "mremap maymove free publish degrade old address unmapped");
    expect_true(test_find_free_extent_containing(heap, map) == 0,
                "mremap maymove free publish degrade old address isolated");
    expect_ok(api->check(heap, 0),
              "mremap maymove free publish degrade check after");
    expect_ok(api->munmap(heap, moved, page_size * 3u),
              "mremap maymove rollback cleanup");
    expect_ok(api->close(heap), "mremap maymove rollback close");
    test_vm_release_alloc(&arena_mem);
    test_vm_release_alloc(&heap_mem);
}

