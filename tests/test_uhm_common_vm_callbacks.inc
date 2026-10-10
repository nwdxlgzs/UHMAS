static uhm_result UHM_CALL failing_vm_reserve(void *user, void **addr, size_t size, size_t alignment) {
    failing_vm_state *state = (failing_vm_state *)user;
    state->reserve_count++;
    state->reserve_size = size;
    state->reserve_alignment = alignment;
    if (state->fail_reserve) {
        test_set_platform_error(test_platform_error_code());
        return UHM_ERR_PLATFORM;
    }
    if (!test_vm_reserve_commit_alloc(size, alignment, &state->arena)) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    *addr = state->arena.aligned;
    return UHM_OK;
}

static uhm_result UHM_CALL failing_vm_commit(void *user, void *addr, size_t size, uint32_t prot) {
    failing_vm_state *state = (failing_vm_state *)user;
    (void)addr;
    (void)size;
    (void)prot;
    state->commit_count++;
    return UHM_ERR_PLATFORM;
}

static uhm_result UHM_CALL failing_vm_release(void *user, void *addr, size_t size) {
    failing_vm_state *state = (failing_vm_state *)user;
    (void)addr;
    (void)size;
    state->release_count++;
    test_vm_release_alloc(&state->arena);
    return UHM_OK;
}

static uhm_result UHM_CALL fault_vm_commit(void *user, void *addr, size_t size, uint32_t prot) {
    fault_vm_state *state = (fault_vm_state *)user;
    (void)prot;
    state->commit_count++;
    state->last_commit_addr = addr;
    state->last_commit_size = size;
    if (state->fail_commit_once) {
        state->fail_commit_once = 0;
        (void)state->partial_commit_before_failure;
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    if (state->fail_commit) {
        (void)state->partial_commit_before_failure;
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    if (fault_vm_should_fail(state, FAULT_VM_OP_COMMIT, addr, size)) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    return UHM_OK;
}

static uhm_result UHM_CALL close_during_mmap_vm_commit(void *user, void *addr,
                                                       size_t size,
                                                       uint32_t prot) {
    close_during_mmap_vm_state *state =
        (close_during_mmap_vm_state *)user;
    (void)prot;
    state->commit_count++;
    state->last_commit_addr = addr;
    state->last_commit_size = size;
    if (atomic_load_explicit(&state->block_commit,
                             UHM_ATOMIC_ACQUIRE) != 0u) {
        atomic_store_explicit(&state->commit_entered, 1u,
                              UHM_ATOMIC_RELEASE);
        while (atomic_load_explicit(&state->release_commit,
                                    UHM_ATOMIC_ACQUIRE) == 0u) {
            test_thread_pause();
        }
    }
    return UHM_OK;
}

static uhm_result UHM_CALL fault_vm_decommit(void *user, void *addr, size_t size) {
    fault_vm_state *state = (fault_vm_state *)user;
    state->decommit_count++;
    state->last_decommit_addr = addr;
    state->last_decommit_size = size;
    if (state->fail_decommit) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    if (fault_vm_should_fail(state, FAULT_VM_OP_DECOMMIT, addr, size)) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    return UHM_OK;
}

static uhm_result UHM_CALL fault_vm_protect(void *user, void *addr, size_t size, uint32_t prot) {
    fault_vm_state *state = (fault_vm_state *)user;
    (void)addr;
    (void)size;
    (void)prot;
    state->protect_count++;
    if (state->fail_protect) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    if (fault_vm_should_fail(state, FAULT_VM_OP_PROTECT, addr, size)) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    return UHM_OK;
}

static uhm_result UHM_CALL reentrant_check_vm_protect(void *user, void *addr,
                                                      size_t size,
                                                      uint32_t prot) {
    reentrant_check_vm_state *state = (reentrant_check_vm_state *)user;
    (void)addr;
    (void)size;
    (void)prot;
    state->protect_count++;
    if (state->run_check && state->api && state->heap) {
        state->check_calls++;
        state->check_result = state->api->check(state->heap, 0);
        if (state->check_result != UHM_OK) {
            state->check_failures++;
        }
    }
    return UHM_OK;
}

static uhm_result UHM_CALL reentrant_check_vm_commit(void *user, void *addr,
                                                     size_t size,
                                                     uint32_t prot) {
    reentrant_check_vm_state *state = (reentrant_check_vm_state *)user;
    (void)prot;
    state->commit_count++;
    state->last_commit_addr = addr;
    state->last_commit_size = size;
    if (state->decommit_revalidate_slab) {
        state->decommit_revalidate_slab->ctx_cached_slots = 0u;
    }
    if (state->run_mprotect_probe && state->api && state->heap) {
        state->mprotect_probe_calls++;
        state->mprotect_probe_result = state->api->mprotect(
            state->heap, state->mprotect_probe_addr,
            state->mprotect_probe_size, state->mprotect_probe_prot);
    }
    if (state->run_check && state->api && state->heap) {
        state->check_calls++;
        state->check_result = state->api->check(state->heap, 0);
        if (state->check_result != UHM_OK) {
            state->check_failures++;
        }
    }
    return UHM_OK;
}

static uhm_result UHM_CALL reentrant_check_vm_decommit(void *user, void *addr,
                                                       size_t size) {
    reentrant_check_vm_state *state = (reentrant_check_vm_state *)user;
    state->decommit_count++;
    state->last_decommit_addr = addr;
    state->last_decommit_size = size;
    if (state->run_mprotect_probe && state->api && state->heap) {
        state->mprotect_probe_calls++;
        state->mprotect_probe_result = state->api->mprotect(
            state->heap, state->mprotect_probe_addr,
            state->mprotect_probe_size, state->mprotect_probe_prot);
    }
    if (state->run_check && state->api && state->heap) {
        state->check_calls++;
        state->check_result = state->api->check(state->heap, 0);
        if (state->check_result != UHM_OK) {
            state->check_failures++;
        }
    }
    if (state->fail_decommit) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    if (state->zero_decommit_range) {
        memset(addr, 0, size);
    }
    if (state->decommit_revalidate_slab) {
        state->decommit_revalidate_slab->ctx_cached_slots = 1u;
    }
    return UHM_OK;
}

static uhm_result UHM_CALL reentrant_check_vm_numa_hint(
    void *user, void *addr, size_t size, uint32_t policy, int32_t node,
    uint32_t flags) {
    reentrant_check_vm_state *state = (reentrant_check_vm_state *)user;
    (void)addr;
    (void)size;
    (void)policy;
    (void)node;
    (void)flags;
    state->numa_hint_count++;
    if (state->run_check && state->api && state->heap) {
        state->check_calls++;
        state->check_result = state->api->check(state->heap, 0);
        if (state->check_result != UHM_OK) {
            state->check_failures++;
        }
    }
    return UHM_OK;
}

static uhm_result UHM_CALL fault_vm_fixed_remap(void *user, void *addr, size_t size,
                                                uint32_t prot) {
    fault_vm_state *state = (fault_vm_state *)user;
    (void)addr;
    (void)size;
    (void)prot;
    state->fixed_remap_count++;
    if (state->fail_fixed_remap) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    if (fault_vm_should_fail(state, FAULT_VM_OP_FIXED_REMAP, addr, size)) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    return UHM_OK;
}

static uhm_result UHM_CALL fault_vm_numa_hint(void *user, void *addr, size_t size,
                                              uint32_t policy, int32_t node,
                                              uint32_t flags) {
    fault_vm_state *state = (fault_vm_state *)user;
    (void)flags;
    state->numa_hint_count++;
    state->last_numa_hint_addr = addr;
    state->last_numa_hint_size = size;
    state->last_numa_hint_policy = policy;
    state->last_numa_hint_node = node;
    if (state->fail_numa_hint) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    if (fault_vm_should_fail(state, FAULT_VM_OP_NUMA_HINT, addr, size)) {
        test_set_platform_error(state->platform_error);
        return UHM_ERR_PLATFORM;
    }
    return UHM_OK;
}

