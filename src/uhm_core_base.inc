#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "uhm_internal.h"

#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static uint64_t uhm_diag_now_ns(uint64_t timer_frequency);
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_diag_atomic_add_u64(uhm_diag_counter_u64 *slot,
                                    uint64_t value) {
    uhm_diag_counter_add_relaxed(slot, value);
}

static uint64_t uhm_diag_atomic_load_u64(const uhm_diag_counter_u64 *slot) {
    return uhm_diag_counter_load_relaxed(slot);
}

static void uhm_diag_atomic_max_u64(uhm_diag_counter_u64 *slot,
                                    uint64_t value) {
    uhm_diag_counter_max_relaxed(slot, value);
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_diag_note_u64_inc(uint64_t *slot) {
    if (slot) {
        ++*slot;
    }
}

static void uhm_diag_note_u64_add(uint64_t *slot, uint64_t value) {
    if (slot) {
        *slot += value;
    }
}

static void uhm_diag_note_max_u64(uint64_t *slot, uint64_t value) {
    if (slot && *slot < value) {
        *slot = value;
    }
}
#else
#define uhm_diag_note_u64_inc(slot_) ((void)0)
#define uhm_diag_note_u64_add(slot_, value_) ((void)0)
#define uhm_diag_note_max_u64(slot_, value_) ((void)0)
#endif

static void uhm_spinlock_init_ex(uhm_spinlock *lock, uint32_t mode,
                                 uint32_t yield_after_spins) {
    if (!lock) {
        return;
    }
    lock->mode = mode;
    lock->yield_after_spins = yield_after_spins != 0u
                                   ? yield_after_spins
                                   : UHM_SPINLOCK_DEFAULT_YIELD_AFTER;
    uhm_spinlock_set_diagnostics_enabled(lock, 0u);
    uhm_spinlock_set_diagnostics_timing_enabled(lock, 0u);
    atomic_init(&lock->held, 0u);
#if UHMAS_ENABLE_DIAGNOSTICS
#if defined(_WIN32)
    {
        LARGE_INTEGER frequency;
        lock->diagnostics_timer_frequency =
            QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0
                ? (uint64_t)frequency.QuadPart
                : UINT64_C(0);
    }
#endif
    uhm_diag_counter_store_relaxed(&lock->lock_calls, UINT64_C(0));
    uhm_diag_counter_store_relaxed(&lock->try_calls, UINT64_C(0));
    uhm_diag_counter_store_relaxed(&lock->try_failures, UINT64_C(0));
    uhm_diag_counter_store_relaxed(&lock->contended_calls, UINT64_C(0));
    uhm_diag_counter_store_relaxed(&lock->wait_ns, UINT64_C(0));
    uhm_diag_counter_store_relaxed(&lock->max_wait_ns, UINT64_C(0));
    uhm_diag_counter_store_relaxed(&lock->hold_ns, UINT64_C(0));
    uhm_diag_counter_store_relaxed(&lock->max_hold_ns, UINT64_C(0));
    uhm_diag_counter_store_relaxed(&lock->acquired_ns, UINT64_C(0));
#endif
    atomic_flag_clear_explicit(&lock->flag, UHM_ATOMIC_RELAXED);
}

static void uhm_spinlock_yield(void) {
#if defined(_WIN32)
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static int uhm_transient_protecting_retry(unsigned *retries) {
    if (!retries || *retries >= UHM_TRANSIENT_PROTECTING_RETRY_LIMIT) {
        return 0;
    }
    ++*retries;
    uhm_spinlock_yield();
    return 1;
}

static void uhm_lock(uhm_spinlock *lock) {
    uint32_t spins = 0u;
    uint32_t yield_after;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t begin_ns;
    uint64_t end_ns;
    uint64_t wait_ns;
    int timing_enabled;
    int contended = 0;
#endif

    if (!lock) {
        return;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    if (!uhm_spinlock_diagnostics_enabled(lock)) {
#endif
        yield_after = lock->yield_after_spins != 0u
                          ? lock->yield_after_spins
                          : UHM_SPINLOCK_DEFAULT_YIELD_AFTER;
        while (atomic_flag_test_and_set_explicit(&lock->flag,
                                                 UHM_ATOMIC_ACQUIRE)) {
            /* held is only a read-spin hint; atomic_flag owns the lock. */
            do {
                if (lock->mode == UHM_SPINLOCK_MODE_YIELD &&
                    ++spins >= yield_after) {
                    spins = 0u;
                    uhm_spinlock_yield();
                }
            } while (atomic_load_explicit(&lock->held,
                                          UHM_ATOMIC_RELAXED) != 0u);
        }
        atomic_store_explicit(&lock->held, 1u, UHM_ATOMIC_RELEASE);
        return;
#if UHMAS_ENABLE_DIAGNOSTICS
    }
    timing_enabled = uhm_spinlock_diagnostics_timing_enabled(lock);
    begin_ns = timing_enabled
                   ? uhm_diag_now_ns(
                         uhm_spinlock_diagnostics_timer_frequency(lock))
                   : UINT64_C(0);
    uhm_diag_counter_add_relaxed(&lock->lock_calls, UINT64_C(1));
    yield_after = lock->yield_after_spins != 0u
                      ? lock->yield_after_spins
                      : UHM_SPINLOCK_DEFAULT_YIELD_AFTER;
    while (atomic_flag_test_and_set_explicit(&lock->flag,
                                             UHM_ATOMIC_ACQUIRE)) {
        contended = 1;
        /* held is only a read-spin hint; atomic_flag owns the lock. */
        do {
            if (lock->mode == UHM_SPINLOCK_MODE_YIELD &&
                ++spins >= yield_after) {
                spins = 0u;
                uhm_spinlock_yield();
            }
        } while (atomic_load_explicit(&lock->held,
                                      UHM_ATOMIC_RELAXED) != 0u);
    }
    end_ns = timing_enabled
                 ? uhm_diag_now_ns(
                       uhm_spinlock_diagnostics_timer_frequency(lock))
                 : UINT64_C(0);
    wait_ns = end_ns >= begin_ns ? end_ns - begin_ns : 0u;
    if (contended) {
        uhm_diag_counter_add_relaxed(&lock->contended_calls, UINT64_C(1));
    }
    uhm_diag_atomic_add_u64(&lock->wait_ns, wait_ns);
    uhm_diag_atomic_max_u64(&lock->max_wait_ns, wait_ns);
    atomic_store_explicit(&lock->held, 1u, UHM_ATOMIC_RELEASE);
    uhm_diag_counter_store_relaxed(&lock->acquired_ns, end_ns);
#endif
}

static int uhm_try_lock(uhm_spinlock *lock) {
    int acquired;
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t now_ns;
    int timing_enabled;
#endif

    if (!lock) {
        return 1;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    if (!uhm_spinlock_diagnostics_enabled(lock)) {
#endif
        acquired = !atomic_flag_test_and_set_explicit(&lock->flag,
                                                      UHM_ATOMIC_ACQUIRE);
        if (acquired) {
            atomic_store_explicit(&lock->held, 1u, UHM_ATOMIC_RELEASE);
        }
        return acquired;
#if UHMAS_ENABLE_DIAGNOSTICS
    }
    timing_enabled = uhm_spinlock_diagnostics_timing_enabled(lock);
    now_ns = timing_enabled
                 ? uhm_diag_now_ns(
                       uhm_spinlock_diagnostics_timer_frequency(lock))
                 : UINT64_C(0);
    uhm_diag_counter_add_relaxed(&lock->try_calls, UINT64_C(1));
    acquired = !atomic_flag_test_and_set_explicit(&lock->flag,
                                                  UHM_ATOMIC_ACQUIRE);
    if (acquired) {
        atomic_store_explicit(&lock->held, 1u, UHM_ATOMIC_RELEASE);
        uhm_diag_counter_store_relaxed(&lock->acquired_ns, now_ns);
    } else {
        uhm_diag_counter_add_relaxed(&lock->try_failures, UINT64_C(1));
    }
    return acquired;
#endif
}

static void uhm_unlock(uhm_spinlock *lock) {
#if UHMAS_ENABLE_DIAGNOSTICS
    uint64_t acquired_ns;
    uint64_t end_ns;
    uint64_t hold_ns;
    int timing_enabled;
#endif

    if (!lock) {
        return;
    }
#if UHMAS_ENABLE_DIAGNOSTICS
    if (!uhm_spinlock_diagnostics_enabled(lock)) {
#endif
        atomic_store_explicit(&lock->held, 0u, UHM_ATOMIC_RELEASE);
        atomic_flag_clear_explicit(&lock->flag, UHM_ATOMIC_RELEASE);
        return;
#if UHMAS_ENABLE_DIAGNOSTICS
    }
    timing_enabled = uhm_spinlock_diagnostics_timing_enabled(lock);
    end_ns = timing_enabled
                 ? uhm_diag_now_ns(
                       uhm_spinlock_diagnostics_timer_frequency(lock))
                 : UINT64_C(0);
    acquired_ns =
        uhm_diag_counter_exchange_relaxed(&lock->acquired_ns, UINT64_C(0));
    hold_ns = end_ns >= acquired_ns ? end_ns - acquired_ns : 0u;
    uhm_diag_atomic_add_u64(&lock->hold_ns, hold_ns);
    uhm_diag_atomic_max_u64(&lock->max_hold_ns, hold_ns);
    atomic_store_explicit(&lock->held, 0u, UHM_ATOMIC_RELEASE);
    atomic_flag_clear_explicit(&lock->flag, UHM_ATOMIC_RELEASE);
#endif
}

static int uhm_spinlock_is_held(const uhm_spinlock *lock) {
    return lock &&
           atomic_load_explicit(&lock->held, UHM_ATOMIC_ACQUIRE) != 0u;
}

#if UHMAS_ENABLE_DIAGNOSTICS
static uint64_t uhm_diag_now_ns(uint64_t timer_frequency) {
#if defined(_WIN32)
    LARGE_INTEGER counter;
    uint64_t ticks;
    uint64_t seconds;
    uint64_t remainder;

    QueryPerformanceCounter(&counter);
    if (timer_frequency == 0) {
        LARGE_INTEGER frequency;
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            return UINT64_C(0);
        }
        timer_frequency = (uint64_t)frequency.QuadPart;
    }
    ticks = (uint64_t)counter.QuadPart;
    seconds = ticks / timer_frequency;
    remainder = ticks % timer_frequency;
    return seconds * UINT64_C(1000000000) +
           (remainder * UINT64_C(1000000000)) / timer_frequency;
#else
    struct timespec ts;
    (void)timer_frequency;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static int uhm_diag_timing_enabled(const uhm_heap *heap) {
    return uhm_heap_diagnostics_timing_enabled(heap);
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static uint64_t uhm_diag_begin_ns(const uhm_heap *heap) {
    return uhm_diag_timing_enabled(heap)
               ? uhm_diag_now_ns(
                     uhm_spinlock_diagnostics_timer_frequency(&heap->lock))
               : UINT64_C(0);
}

static uint64_t uhm_diag_elapsed_ns(const uhm_heap *heap, uint64_t begin_ns) {
    uint64_t end_ns;

    if (begin_ns == 0) {
        return UINT64_C(0);
    }
    end_ns = uhm_diag_now_ns(
        uhm_spinlock_diagnostics_timer_frequency(&heap->lock));
    if (end_ns < begin_ns) {
        return UINT64_C(0);
    }
    return end_ns != begin_ns ? end_ns - begin_ns : UINT64_C(1);
}

static void uhm_diag_add_elapsed(const uhm_heap *heap, uint64_t *slot,
                                 uint64_t begin_ns) {
    if (!slot || begin_ns == 0) {
        return;
    }
    *slot += uhm_diag_elapsed_ns(heap, begin_ns);
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
typedef struct uhm_diag_close_phase_sample {
    uint64_t wall_begin_ns;
    uint64_t cpu_begin_work;
    uint32_t cpu_work_valid;
    uint32_t cpu_work_is_cycles;
    uint32_t enabled;
} uhm_diag_close_phase_sample;

static uint64_t uhm_diag_thread_cpu_work_now(uint32_t *is_cycles,
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
        return UINT64_C(0);
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
    return UINT64_C(0);
#endif
}

static uhm_diag_close_phase_sample
uhm_diag_close_phase_begin(const uhm_heap *heap) {
    uhm_diag_close_phase_sample sample;

    memset(&sample, 0, sizeof(sample));
    if (!uhm_diag_timing_enabled(heap)) {
        return sample;
    }
    sample.enabled = 1u;
    sample.wall_begin_ns = uhm_diag_now_ns(
        uhm_spinlock_diagnostics_timer_frequency(&heap->lock));
    sample.cpu_begin_work = uhm_diag_thread_cpu_work_now(
        &sample.cpu_work_is_cycles, &sample.cpu_work_valid);
    return sample;
}

static void uhm_diag_close_phase_end(
    uhm_heap *heap, uint32_t phase,
    const uhm_diag_close_phase_sample *sample) {
    uint64_t wall_end_ns;
    uint64_t cpu_end_work;
    uint64_t elapsed;
    uint32_t cpu_end_valid;
    uint32_t cpu_end_is_cycles;

    if (!heap || !sample || !sample->enabled ||
        phase >= UHM_CLOSE_PHASE_COUNT) {
        return;
    }
    wall_end_ns = uhm_diag_now_ns(
        uhm_spinlock_diagnostics_timer_frequency(&heap->lock));
    elapsed = wall_end_ns >= sample->wall_begin_ns
                  ? wall_end_ns - sample->wall_begin_ns
                  : UINT64_C(0);
    heap->core.close_phase_wall_ns[phase] =
        elapsed != 0u ? elapsed : UINT64_C(1);
    ++heap->core.close_phase_calls[phase];

    cpu_end_valid = 0u;
    cpu_end_is_cycles = sample->cpu_work_is_cycles;
    cpu_end_work = uhm_diag_thread_cpu_work_now(
        &cpu_end_is_cycles, &cpu_end_valid);
    heap->core.close_phase_cpu_work_valid[phase] = 0u;
    heap->core.close_phase_cpu_work_is_cycles[phase] =
        sample->cpu_work_is_cycles;
    heap->core.close_phase_cpu_work[phase] = UINT64_C(0);
    if (sample->cpu_work_valid && cpu_end_valid &&
        cpu_end_is_cycles == sample->cpu_work_is_cycles &&
        cpu_end_work >= sample->cpu_begin_work) {
        elapsed = cpu_end_work - sample->cpu_begin_work;
        heap->core.close_phase_cpu_work[phase] =
            elapsed != 0u ? elapsed : UINT64_C(1);
        heap->core.close_phase_cpu_work_valid[phase] = 1u;
    }
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_diag_note_vm_lock_held_call(uhm_heap *heap,
                                            uint64_t *op_lock_held_calls,
                                            uint64_t begin_ns,
                                            uhm_result result) {
    uint64_t end_ns;

    if (!heap || !uhm_spinlock_is_held(&heap->lock)) {
        return;
    }
    ++heap->core.vm_lock_held_calls;
    if (op_lock_held_calls) {
        ++*op_lock_held_calls;
    }
    if (result != UHM_OK) {
        ++heap->core.vm_lock_held_failures;
    }
    if (begin_ns != 0) {
        end_ns = uhm_diag_now_ns(
            uhm_spinlock_diagnostics_timer_frequency(&heap->lock));
        if (end_ns >= begin_ns) {
            heap->core.vm_lock_held_ns += end_ns - begin_ns;
        }
    }
}

static void uhm_diag_note_vm_call(const uhm_heap *heap, uint64_t *calls,
                                  uint64_t *failures, uint64_t *elapsed_ns,
                                  uint64_t begin_ns, uhm_result result) {
    if (calls) {
        ++*calls;
    }
    if (failures && result != UHM_OK) {
        ++*failures;
    }
    uhm_diag_add_elapsed(heap, elapsed_ns, begin_ns);
}
#endif

#if UHMAS_ENABLE_DIAGNOSTICS
static void uhm_diag_note_vm_call_elapsed(uint64_t *calls, uint64_t *failures,
                                          uint64_t *elapsed_ns,
                                          uint64_t elapsed,
                                          uhm_result result) {
    if (calls) {
        ++*calls;
    }
    if (failures && result != UHM_OK) {
        ++*failures;
    }
    if (elapsed_ns && elapsed != 0) {
        *elapsed_ns += elapsed;
    }
}
#endif

static int uhm_is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static int uhm_add_overflows_size(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) {
        return 1;
    }
    *out = a + b;
    return 0;
}

static int uhm_mul_overflows_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return 1;
    }
    *out = a * b;
    return 0;
}

static int uhm_align_up_uintptr_checked(uintptr_t value, size_t alignment,
                                        uintptr_t *out) {
    uintptr_t mask = (uintptr_t)alignment - 1u;
    if (!out || alignment == 0 || !uhm_is_power_of_two(alignment) ||
        value > UINTPTR_MAX - mask) {
        return 1;
    }
    *out = (value + mask) & ~mask;
    return 0;
}

static uintptr_t uhm_align_down_uintptr(uintptr_t value, size_t alignment) {
    uintptr_t mask = (uintptr_t)alignment - 1u;
    return value & ~mask;
}

static int uhm_align_up_size(size_t value, size_t alignment, size_t *out) {
    size_t mask = alignment - 1u;
    if (value > SIZE_MAX - mask) {
        return 1;
    }
    *out = (value + mask) & ~mask;
    return 0;
}

static size_t uhm_max_size(size_t a, size_t b) {
    return a > b ? a : b;
}

static size_t uhm_min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

static uint64_t uhm_current_thread_id(void) {
#if defined(_WIN32)
    return (uint64_t)GetCurrentThreadId();
#elif defined(__linux__) && defined(SYS_gettid)
    {
        long tid = syscall(SYS_gettid);
        return tid > 0 ? (uint64_t)tid : (uint64_t)getpid();
    }
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

static size_t uhm_active_op_shard_index(void) {
    uintptr_t value;

#if defined(_WIN32)
    value = (uintptr_t)GetCurrentThreadId();
#else
    value = (uintptr_t)pthread_self();
#endif
    value ^= value >> 16;
    value *= (uintptr_t)0x9e3779b1u;
    value ^= value >> 12;
    return (size_t)(value & (uintptr_t)(UHM_ACTIVE_OP_SHARDS - 1u));
}

static void uhm_tls_cache_destructor_common(void *value);

#if defined(_WIN32)
static VOID CALLBACK uhm_tls_cache_destructor(PVOID value) {
    uhm_tls_cache_destructor_common(value);
}
#else
static void uhm_tls_cache_destructor(void *value) {
    uhm_tls_cache_destructor_common(value);
}
#endif

static uhm_result uhm_tls_key_create(uhm_heap *heap) {
    if (!heap) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    {
        DWORD key = FlsAlloc(uhm_tls_cache_destructor);
        if (key == FLS_OUT_OF_INDEXES) {
            return UHM_ERR_PLATFORM;
        }
        heap->core.tls_cache_key = (uintptr_t)key;
        heap->core.tls_cache_key_valid = 1u;
        return UHM_OK;
    }
#else
    {
        pthread_key_t key;
        if (pthread_key_create(&key, uhm_tls_cache_destructor) != 0) {
            return UHM_ERR_PLATFORM;
        }
        heap->core.tls_cache_key = (uintptr_t)key;
        heap->core.tls_cache_key_valid = 1u;
        return UHM_OK;
    }
#endif
}

static void uhm_tls_key_delete(uhm_heap *heap) {
    if (!heap || !heap->core.tls_cache_key_valid) {
        return;
    }
#if defined(_WIN32)
    (void)FlsFree((DWORD)heap->core.tls_cache_key);
#else
    (void)pthread_key_delete((pthread_key_t)heap->core.tls_cache_key);
#endif
    heap->core.tls_cache_key = 0;
    heap->core.tls_cache_key_valid = 0u;
}

static void *uhm_tls_get_value(uhm_heap *heap) {
    if (!heap || !heap->core.tls_cache_key_valid) {
        return 0;
    }
#if defined(_WIN32)
    return FlsGetValue((DWORD)heap->core.tls_cache_key);
#else
    return pthread_getspecific((pthread_key_t)heap->core.tls_cache_key);
#endif
}

static int uhm_tls_set_value(uhm_heap *heap, void *value) {
    if (!heap || !heap->core.tls_cache_key_valid) {
        return 0;
    }
#if defined(_WIN32)
    return FlsSetValue((DWORD)heap->core.tls_cache_key, value) != 0;
#else
    return pthread_setspecific((pthread_key_t)heap->core.tls_cache_key,
                               value) == 0;
#endif
}

static size_t uhm_system_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    return info.dwPageSize != 0 ? (size_t)info.dwPageSize : (size_t)0u;
#elif defined(_SC_PAGESIZE)
    {
        long page_size = sysconf(_SC_PAGESIZE);
        return page_size > 0 ? (size_t)page_size : (size_t)0u;
    }
#else
    return (size_t)0u;
#endif
}

static size_t uhm_system_allocation_granularity(void) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    return info.dwAllocationGranularity != 0
               ? (size_t)info.dwAllocationGranularity
               : uhm_system_page_size();
#else
    return uhm_system_page_size();
#endif
}

static int uhm_range_overflows(uintptr_t start, size_t size, uintptr_t *end_out) {
    uintptr_t end = start + (uintptr_t)size;
    if (end < start) {
        return 1;
    }
    *end_out = end;
    return 0;
}

static size_t uhm_malloc_header_size(void);
static uhm_result uhm_prepare_alloc_size(size_t alignment, size_t size,
                                         size_t *out_total,
                                         size_t *out_payload,
                                         uint16_t *out_size_class);
static uhm_result uhm_thread_ctx_flush_local_locked(
    uhm_heap *heap, uhm_thread_ctx *ctx, int *maintenance_needed);
static uhm_result uhm_thread_ctx_flush_locked(uhm_heap *heap,
                                              uhm_thread_ctx *ctx);

static const size_t uhm_size_classes[] = {
    16u,   32u,   48u,   64u,   80u,   96u,   112u,  128u,
    160u,  192u,  224u,  256u,  320u,  384u,  448u,  512u,
    640u,  768u,  896u,  1024u, 1280u, 1536u, 1792u, 2048u,
    2560u, 3072u, 3584u, 4096u
};

_Static_assert((sizeof(uhm_size_classes) / sizeof(uhm_size_classes[0])) ==
                   UHM_SIZE_CLASS_COUNT,
               "size class table must match heap slab heads");

#define UHM_SLAB_MAX_SLOT_SIZE 512u
#define UHM_MALLOC_FREE_DECOMMIT_DEFAULT_RETENTION_PAGES 8u
#define UHM_MALLOC_FREE_DECOMMIT_RETENTION_HIGH_WATER_MULTIPLIER 8u
#define UHM_MALLOC_FREE_DECOMMIT_RETENTION_ADAPTIVE_MAX_MULTIPLIER 32u

static uint16_t uhm_size_class_for(size_t alignment, size_t size, size_t *payload_size) {
    size_t requested = size == 0 ? 1u : size;
    size_t i;

    if (alignment != UHM_MIN_ALIGNMENT) {
        return UHM_SIZE_CLASS_NONE;
    }
    for (i = 0; i < sizeof(uhm_size_classes) / sizeof(uhm_size_classes[0]); ++i) {
        if (requested <= uhm_size_classes[i]) {
            if (payload_size) {
                *payload_size = uhm_size_classes[i];
            }
            return (uint16_t)i;
        }
    }
    return UHM_SIZE_CLASS_NONE;
}

static size_t uhm_block_usable_size(const uhm_block *block) {
    size_t header = uhm_malloc_header_size();
    return block && block->total_size >= header ? block->total_size - header : 0;
}

static int uhm_heap_is_live(const uhm_heap *heap) {
    return heap &&
           atomic_load_explicit(&heap->lifecycle_state,
                                UHM_ATOMIC_ACQUIRE) == UHM_MAGIC &&
           heap->F.canary == UHM_HEAP_F_CANARY;
}

static int uhm_heap_is_accessible_pinned(const uhm_heap *heap) {
    uint32_t state;

    if (!heap || heap->F.canary != UHM_HEAP_F_CANARY) {
        return 0;
    }
    state = atomic_load_explicit(&heap->lifecycle_state, UHM_ATOMIC_ACQUIRE);
    return state == UHM_MAGIC || state == UHM_CLOSING_MAGIC;
}

static int uhm_heap_op_begin(uhm_heap *heap, size_t *out_shard) {
    size_t shard;

    if (!uhm_heap_is_live(heap)) {
        return 0;
    }
    shard = uhm_active_op_shard_index();
    (void)atomic_fetch_add_explicit(&heap->active_op_shards[shard].value, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    if (uhm_heap_is_live(heap)) {
        if (out_shard) {
            *out_shard = shard;
        }
        return 1;
    }
    (void)atomic_fetch_sub_explicit(&heap->active_op_shards[shard].value, 1u,
                                    UHM_ATOMIC_ACQ_REL);
    return 0;
}

static void uhm_heap_op_end(uhm_heap *heap, size_t shard) {
    if (heap && shard < UHM_ACTIVE_OP_SHARDS) {
        (void)atomic_fetch_sub_explicit(&heap->active_op_shards[shard].value,
                                        1u, UHM_ATOMIC_ACQ_REL);
    }
}

static void uhm_heap_wait_for_ops(uhm_heap *heap) {
    while (heap && uhm_heap_active_ops_load(heap) != 0u) {
    }
}

static void uhm_set_error(uhm_heap *heap, uhm_result result) {
    if (heap) {
        atomic_store_explicit(&heap->last_error, (int)result, UHM_ATOMIC_RELEASE);
    }
}

static uhm_result uhm_treap_rebuild_all_locked(uhm_heap *heap);

static int uhm_treap_health_runtime_failed(uhm_heap *heap) {
    int failed = 0;
    uhm_result result;

    if (!heap) {
        return 0;
    }
    if ((heap->state_flags & UHM_CONFIG_REPAIR_TREAP_HEALTH) != 0u &&
        atomic_load_explicit(&heap->core.treap_repair_pending_flags,
                             UHM_ATOMIC_ACQUIRE) != 0u) {
        uhm_lock(&heap->lock);
        result = atomic_load_explicit(
                     &heap->core.treap_repair_pending_flags,
                     UHM_ATOMIC_RELAXED) != 0u
                     ? uhm_treap_rebuild_all_locked(heap)
                     : UHM_OK;
        uhm_unlock(&heap->lock);
        if (result != UHM_OK) {
            uhm_set_error(heap, result);
            return 1;
        }
    }
    if ((heap->state_flags & UHM_CONFIG_FAIL_ON_TREAP_HEALTH) != 0u) {
        uhm_lock(&heap->lock);
        failed = heap->core.treap_health_flags != 0u;
        uhm_unlock(&heap->lock);
    }
    if ((heap->state_flags & UHM_CONFIG_FAIL_ON_LIVE_BLOCK_HEALTH) != 0u) {
        uhm_lock(&heap->lock);
        failed = failed || heap->core.live_block_health_flags != 0u;
        uhm_unlock(&heap->lock);
    }
    if (failed) {
        uhm_set_error(heap, UHM_ERR_CORRUPTION);
    }
    return failed;
}

static void uhm_debug_corruption_site(uhm_heap *heap, uint32_t site) {
    if (heap && site != 0) {
        uint32_t expected = 0;
        (void)atomic_compare_exchange_strong_explicit(
            &heap->core.debug_last_corruption_site, &expected, site,
            UHM_ATOMIC_ACQ_REL, UHM_ATOMIC_ACQUIRE);
    }
}

static void uhm_clear_platform_error(uhm_heap *heap) {
    if (heap) {
        atomic_store_explicit(&heap->platform_error, 0u, UHM_ATOMIC_RELEASE);
    }
}

static void uhm_platform_error_before_call(void) {
#if defined(_WIN32)
    SetLastError(0);
#else
    errno = 0;
#endif
}

static uint32_t uhm_platform_error_after_call(void) {
#if defined(_WIN32)
    return (uint32_t)GetLastError();
#else
    return (uint32_t)errno;
#endif
}

static uhm_result uhm_record_vm_result(uhm_heap *heap, uhm_result result) {
    if (result == UHM_ERR_PLATFORM) {
        if (heap) {
            atomic_store_explicit(&heap->platform_error, uhm_platform_error_after_call(),
                                  UHM_ATOMIC_RELEASE);
        }
    } else if (result != UHM_OK) {
        uhm_clear_platform_error(heap);
    }
    return result;
}

static uhm_result uhm_record_vm_result_captured(uhm_heap *heap,
                                                uhm_result result,
                                                uint32_t platform_error) {
    if (result == UHM_ERR_PLATFORM) {
        if (heap) {
            atomic_store_explicit(&heap->platform_error, platform_error,
                                  UHM_ATOMIC_RELEASE);
        }
    } else if (result != UHM_OK) {
        uhm_clear_platform_error(heap);
    }
    return result;
}

static void uhm_tx_begin(uhm_tx *tx, uhm_heap *heap, uint32_t op,
                         uintptr_t addr, size_t size, uint32_t prot,
                         uint32_t flags) {
    memset(tx, 0, sizeof(*tx));
    tx->heap = heap;
    tx->op = op;
    tx->stage = UHM_TX_STAGE_INIT;
    tx->result = UHM_OK;
    tx->addr = addr;
    tx->size = size;
    tx->flags = flags;
    tx->new_prot = prot;
    tx->old_commit_state = UHM_PAGE_COMMIT_NONE;
    tx->new_commit_state = UHM_PAGE_COMMIT_NONE;
    tx->begin_generation = heap ? uhm_heap_generation_load(heap) : 0;
    tx->old_mmap_lo = 0;
}

static int uhm_tx_stage_is_allowed(uint32_t current, uint32_t next) {
    if (current == next) {
        return current == UHM_TX_STAGE_PLATFORM;
    }
    if (next == UHM_TX_STAGE_ROLLBACK) {
        return current != UHM_TX_STAGE_INIT &&
               current != UHM_TX_STAGE_DONE;
    }
    switch (current) {
    case UHM_TX_STAGE_INIT:
        return next == UHM_TX_STAGE_VALIDATE;
    case UHM_TX_STAGE_VALIDATE:
        return next == UHM_TX_STAGE_CLAIMED ||
               next == UHM_TX_STAGE_PREPARED;
    case UHM_TX_STAGE_CLAIMED:
        return next == UHM_TX_STAGE_PREPARED;
    case UHM_TX_STAGE_PREPARED:
        return next == UHM_TX_STAGE_PLATFORM ||
               next == UHM_TX_STAGE_PUBLISH;
    case UHM_TX_STAGE_PLATFORM:
        return next == UHM_TX_STAGE_PUBLISH;
    default:
        return 0;
    }
}

static void uhm_tx_stage(uhm_tx *tx, uint32_t stage) {
    if (!tx || !uhm_tx_stage_is_allowed(tx->stage, stage)) {
        if (tx) {
            tx->stage_error = 1u;
        }
        return;
    }
    tx->stage = stage;
}

static void uhm_tx_record_old_mmap(uhm_tx *tx, uhm_vma *vma,
                                   uint32_t commit_state, uint32_t prot) {
    tx->old_vma = vma;
    tx->old_commit_state = commit_state;
    tx->old_prot = prot;
}

static uhm_result uhm_tx_finish(uhm_tx *tx, uhm_result result) {
    if (result == UHM_OK &&
        (!tx->published || tx->stage != UHM_TX_STAGE_PUBLISH)) {
        tx->stage_error = 1u;
    }
    if (tx->stage_error && result == UHM_OK) {
        result = UHM_ERR_CORRUPTION;
    }
    tx->result = result;
    tx->stage = UHM_TX_STAGE_DONE;
    if (result != UHM_OK) {
        uhm_set_error(tx->heap, result);
    }
    return result;
}

static uint64_t uhm_bump_failure_generation(uhm_heap *heap) {
    return heap ? uhm_heap_generation_next(heap) : 0;
}

static uint64_t uhm_extent_descriptor_id(const uhm_extent_descriptor *desc) {
    return (uint64_t)(uintptr_t)desc;
}

static void uhm_extent_descriptor_set(uhm_extent_descriptor *desc, uint32_t kind,
                                      uintptr_t addr, size_t size, uint32_t owner,
                                      uint32_t va_state, uint32_t commit_state,
                                      uint32_t prot, uint64_t generation,
                                      void *object) {
    if (!desc) {
        return;
    }
    desc->magic = UHM_EXTENT_DESCRIPTOR_MAGIC;
    desc->kind = kind;
    desc->addr = addr;
    desc->size = size;
    desc->owner = owner;
    desc->va_state = va_state;
    desc->commit_state = commit_state;
    desc->prot = prot;
    desc->id = uhm_extent_descriptor_id(desc);
    desc->generation = generation;
    desc->object = object;
}

static void uhm_extent_descriptor_retire(uhm_extent_descriptor *desc) {
    if (!desc) {
        return;
    }
    desc->magic = 0;
    desc->kind = UHM_EXTENT_KIND_NONE;
    desc->addr = 0;
    desc->size = 0;
    desc->owner = UHM_PAGE_OWNER_FREE;
    desc->va_state = UHM_PAGE_VA_FREE_RESERVED;
    desc->commit_state = UHM_PAGE_COMMIT_NONE;
    desc->prot = UHM_PROT_NONE;
    desc->id = 0;
    desc->generation = 0;
    desc->object = 0;
}

static int uhm_extent_descriptor_matches(const uhm_extent_descriptor *desc,
                                         uint32_t kind, uintptr_t addr,
                                         size_t size, uint32_t owner,
                                         uint32_t va_state,
                                         uint32_t commit_state,
                                         uint32_t prot, uint64_t generation,
                                         const void *object) {
    return desc &&
           desc->magic == UHM_EXTENT_DESCRIPTOR_MAGIC &&
           desc->kind == kind &&
           desc->addr == addr &&
           desc->size == size &&
           desc->owner == owner &&
           desc->va_state == va_state &&
           desc->commit_state == commit_state &&
           desc->prot == prot &&
           desc->id == uhm_extent_descriptor_id(desc) &&
           desc->generation == generation &&
           desc->object == object;
}

static uhm_result uhm_vm_validate_prot(uhm_heap *heap, uint32_t prot) {
    const uint32_t known_mask = UHM_PROT_READ | UHM_PROT_WRITE | UHM_PROT_EXEC;
    if ((prot & ~known_mask) != 0) {
        uhm_clear_platform_error(heap);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((prot & UHM_PROT_EXEC) != 0 &&
        (heap->F.vm.capabilities & UHM_VM_PROTECT_EXEC_SUPPORTED) == 0) {
        uhm_clear_platform_error(heap);
        return UHM_ERR_UNSUPPORTED;
    }
    if ((prot & (UHM_PROT_WRITE | UHM_PROT_EXEC)) == (UHM_PROT_WRITE | UHM_PROT_EXEC) &&
        (heap->F.vm.capabilities & UHM_VM_WX_POLICY_RESTRICTED) != 0) {
        uhm_clear_platform_error(heap);
        return UHM_ERR_PROTECTION;
    }
    return UHM_OK;
}

