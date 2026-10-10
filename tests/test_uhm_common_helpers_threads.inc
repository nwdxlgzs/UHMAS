static void test_fill_bytes(void *ptr, size_t size, unsigned value) {
    memset(ptr, (int)(value & 0xffu), size);
}

static int test_bytes_equal(const void *ptr, size_t size, unsigned value) {
    const unsigned char *bytes = (const unsigned char *)ptr;
    unsigned char expected = (unsigned char)(value & 0xffu);
    size_t i;
    for (i = 0; i < size; ++i) {
        if (bytes[i] != expected) {
            return 0;
        }
    }
    return 1;
}

static uint64_t test_next_u64(uint64_t *state) {
    *state = (*state * 6364136223846793005ull) + 1442695040888963407ull;
    return *state;
}

static void test_set_platform_error(uint32_t code) {
#if defined(_WIN32)
    SetLastError((DWORD)code);
#else
    errno = (int)code;
#endif
}

static uint32_t test_platform_error_code(void) {
#if defined(_WIN32)
    return 1234u;
#else
    return (uint32_t)ENOMEM;
#endif
}

static void test_thread_pause(void) {
#if defined(_WIN32)
    Sleep(0);
#else
    (void)sched_yield();
#endif
}

static void test_spin_lock(uhm_spinlock *lock) {
    while (atomic_flag_test_and_set_explicit(&lock->flag,
                                             UHM_ATOMIC_ACQUIRE)) {
    }
    atomic_store_explicit(&lock->held, 1u, UHM_ATOMIC_RELEASE);
}

static void test_spin_unlock(uhm_spinlock *lock) {
    atomic_store_explicit(&lock->held, 0u, UHM_ATOMIC_RELEASE);
    atomic_flag_clear_explicit(&lock->flag, UHM_ATOMIC_RELEASE);
}

static int test_wait_atomic_u32(_Atomic(uint32_t) *value, uint32_t expected) {
    size_t i;
    for (i = 0; i < 1000000u; ++i) {
        if (atomic_load_explicit(value, UHM_ATOMIC_ACQUIRE) == expected) {
            return 1;
        }
        test_thread_pause();
    }
    return 0;
}

static int test_wait_heap_lifecycle(uhm_heap *heap, uint32_t expected) {
    size_t i;
    for (i = 0; i < 1000000u; ++i) {
        if (atomic_load_explicit(&heap->lifecycle_state,
                                 UHM_ATOMIC_ACQUIRE) == expected) {
            return 1;
        }
        test_thread_pause();
    }
    return 0;
}

static uint32_t fault_vm_next_random(fault_vm_state *state) {
    uint64_t value = state->fault_rng_state;
    if (value == 0) {
        value = state->fault_seed != 0 ? state->fault_seed
                                       : UINT64_C(0x9e3779b97f4a7c15);
    }
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    state->fault_rng_state = value;
    return (uint32_t)((value * UINT64_C(2685821657736338717)) >> 32);
}

static int fault_vm_range_matches(const fault_vm_state *state, void *addr, size_t size) {
    uintptr_t start;
    uintptr_t end;

    if (!state->fail_addr_range_enabled) {
        return 1;
    }
    start = (uintptr_t)addr;
    end = start + size;
    if (end < start || state->fail_addr_end <= state->fail_addr_begin) {
        return 0;
    }
    return start < state->fail_addr_end && state->fail_addr_begin < end;
}

static int fault_vm_should_fail(fault_vm_state *state, uint32_t op,
                                void *addr, size_t size) {
    if ((state->fail_ops & op) == 0 || !fault_vm_range_matches(state, addr, size)) {
        return 0;
    }
    ++state->fault_match_count;
    if (state->fail_on_match != 0 && state->fault_match_count == state->fail_on_match) {
        return 1;
    }
    if (state->fail_probability_ppm != 0) {
        uint32_t threshold = state->fail_probability_ppm;
        if (threshold > FAULT_VM_PROB_DENOM) {
            threshold = FAULT_VM_PROB_DENOM;
        }
        return (fault_vm_next_random(state) % FAULT_VM_PROB_DENOM) < threshold;
    }
    return 0;
}

static void fault_vm_clear_rules(fault_vm_state *state) {
    state->fail_ops = 0;
    state->fail_probability_ppm = 0;
    state->fault_seed = 0;
    state->fault_rng_state = 0;
    state->fault_match_count = 0;
    state->fail_on_match = 0;
    state->fail_addr_range_enabled = 0;
    state->fail_addr_begin = 0;
    state->fail_addr_end = 0;
}

static void expect_last_error(const uhm_api *api, uhm_heap *heap,
                              uhm_result expected_error,
                              uint32_t expected_platform_error,
                              const char *message) {
    uhm_result actual_error = UHM_OK;
    uint32_t actual_platform_error = 0;
    expect_true(api->get_last_error != 0, "get_last_error api slot");
    expect_ok(api->get_last_error(heap, &actual_error, &actual_platform_error), message);
    expect_true(actual_error == expected_error, "last error result");
    expect_true(actual_platform_error == expected_platform_error, "last platform error");
}

static uhm_result UHM_CALL collect_leak(void *user, const uhm_leak_record *record) {
    leak_collect_state *state = (leak_collect_state *)user;
    expect_true(record != 0, "leak callback record");
    expect_true(record->size >= sizeof(*record), "leak record size");
    expect_true(record->abi_major == UHM_ABI_MAJOR, "leak record abi major");
    ++state->total_count;
    if (record->kind == UHM_LEAK_KIND_MALLOC) {
        ++state->malloc_count;
        state->seen_malloc = record->addr;
        expect_true(record->requested_size != 0, "malloc leak requested size");
        expect_true(record->allocated_size >= record->requested_size,
                    "malloc leak allocated size");
        if ((record->flags & UHM_LEAK_FLAG_PAGE_PRIVATE) != 0) {
            ++state->page_private_count;
        }
    } else if (record->kind == UHM_LEAK_KIND_MMAP) {
        ++state->mmap_count;
        state->seen_mmap = record->addr;
        expect_true(record->requested_size == record->allocated_size,
                    "mmap leak size matches");
    } else {
        expect_true(0, "unknown leak kind");
    }
    if (state->callback_limit != 0 && state->total_count >= state->callback_limit) {
        return UHM_ERR_UNSUPPORTED;
    }
    return UHM_OK;
}

