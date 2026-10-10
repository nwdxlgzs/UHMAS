#include "uhm/uhm.h"
#include "uhm/uhm_platform.h"
#include "../src/uhm_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_ok(uhm_result result, const char *message) {
    if (result != UHM_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static void expect_true(int value, const char *message) {
    if (!value) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

#if defined(_WIN32)
static void test_windows_mem_release_contract(void) {
    SYSTEM_INFO info;
    size_t reserve_size;
    void *region;
    void *inside;

    GetNativeSystemInfo(&info);
    reserve_size = (size_t)info.dwAllocationGranularity * 2u;
    region = VirtualAlloc(0, reserve_size, MEM_RESERVE, PAGE_NOACCESS);
    expect_true(region != 0, "windows mem_release reserve");

    expect_true(VirtualFree(region, (SIZE_T)info.dwPageSize, MEM_RELEASE) == 0,
                "windows native MEM_RELEASE rejects nonzero size");

    inside = (void *)((uintptr_t)region + (uintptr_t)info.dwPageSize);
    expect_true(uhm_platform_windows_release(0, inside, reserve_size - info.dwPageSize) ==
                    UHM_ERR_PLATFORM,
                "windows MEM_RELEASE rejects interior address");

    expect_ok(uhm_platform_windows_release(0, region, reserve_size),
              "windows wrapper releases base with abstract nonzero size");
}
#else
static void test_windows_mem_release_contract(void) {
}
#endif

static void test_platform_numa_backend(void) {
    uhm_vm_ops vm;
    void *region = 0;
    size_t reserve_size;
    volatile unsigned char *bytes;

    expect_ok(uhm_platform_vm_ops_init(&vm), "platform NUMA vm init");
    expect_true((vm.capabilities & UHM_VM_CAN_NUMA_FIRST_TOUCH) != 0u,
                "platform NUMA first-touch capability");
#if defined(_WIN32)
    expect_true((vm.capabilities & UHM_VM_CAN_NUMA_PREFERRED) != 0u,
                "windows NUMA preferred capability");
    expect_true((vm.capabilities & UHM_VM_CAN_NUMA_BIND) == 0u,
                "windows does not claim strict NUMA bind");
#elif defined(__linux__)
    expect_true((vm.capabilities & UHM_VM_CAN_NUMA_PREFERRED) != 0u,
                "linux NUMA preferred capability");
    expect_true((vm.capabilities & UHM_VM_CAN_NUMA_BIND) != 0u,
                "linux NUMA bind capability");
#else
    expect_true((vm.capabilities & (UHM_VM_CAN_NUMA_PREFERRED |
                                    UHM_VM_CAN_NUMA_BIND)) == 0u,
                "non-linux POSIX NUMA capability fallback");
#endif

    if ((vm.capabilities & UHM_VM_CAN_NUMA_PREFERRED) == 0u) {
        return;
    }
    reserve_size = vm.allocation_granularity;
    expect_ok(vm.reserve(vm.user, &region, reserve_size,
                         vm.allocation_granularity),
              "platform NUMA reserve");
    expect_ok(vm.commit(vm.user, region, vm.page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE),
              "platform NUMA commit");
    expect_ok(vm.numa_hint(vm.user, region, vm.page_size,
                           UHM_NUMA_POLICY_PREFERRED, 0, 0u),
              "platform NUMA preferred hint");
    bytes = (volatile unsigned char *)region;
    bytes[0] = 0x5au;
    expect_true(bytes[0] == 0x5au, "platform NUMA preferred memory usable");
    if ((vm.capabilities & UHM_VM_CAN_NUMA_BIND) != 0u) {
        expect_ok(vm.numa_hint(vm.user, region, vm.page_size,
                               UHM_NUMA_POLICY_BIND, 0, 0u),
                  "platform NUMA bind hint");
    }
    expect_true(vm.numa_hint(vm.user, region, vm.page_size,
                             UHM_NUMA_POLICY_PREFERRED, -1, 0u) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "platform NUMA rejects negative node");
    expect_ok(vm.decommit(vm.user, region, vm.page_size),
              "platform NUMA decommit");
    expect_ok(vm.release(vm.user, region, reserve_size),
              "platform NUMA release");
}

static void test_platform_heap_numa_backend(void) {
    uhm_vm_ops vm;
    uhm_platform_heap_config config;
    uhm_platform_heap owner;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_mmap_options options;
    uhm_stats stats;
    void *ptr;

    expect_ok(uhm_platform_vm_ops_init(&vm), "platform NUMA heap vm init");
    if ((vm.capabilities & UHM_VM_CAN_NUMA_PREFERRED) == 0u) {
        return;
    }
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = 4u * 1024u * 1024u;
    config.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    config.preferred_numa_node = 0;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform preferred NUMA heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0 && api->mmap_ex != 0,
                "platform preferred NUMA heap API");
    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "platform preferred NUMA malloc");
    memset(ptr, 0x6bu, 64u);
    api->free(heap, ptr);

    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.abi_minor = UHM_ABI_MINOR;
    options.map_size = owner.vm.page_size;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE;
    options.numa_policy = UHM_NUMA_POLICY_PREFERRED;
    options.numa_node = 0;
    expect_ok(api->mmap_ex(heap, &options),
              "platform preferred NUMA mmap");
    memset(options.addr, 0x7cu, owner.vm.page_size);
    expect_ok(api->munmap(heap, options.addr, options.map_size),
              "platform preferred NUMA munmap");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "platform NUMA stats");
    expect_true(stats.vm_numa_hint_calls >= 2u &&
                    stats.vm_numa_hint_failures == 0u,
                "platform NUMA malloc and mmap hints recorded");
    expect_ok(api->check(heap, 0), "platform preferred NUMA heap check");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform preferred NUMA heap close");
}

static void test_platform_heap_default_open(void) {
    uhm_platform_heap owner;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;
    void *map = 0;

    expect_ok(uhm_platform_heap_open_default(&owner, &heap),
              "default platform heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "default platform api");
    ptr = api->malloc(heap, 128u);
    expect_true(ptr != 0, "default platform malloc");
    memset(ptr, 0x2a, 128u);
    api->free(heap, ptr);
    expect_ok(api->mmap(heap, &map, owner.vm.page_size,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "default platform mmap");
    expect_true(map != 0, "default platform mmap address");
    memset(map, 0x3du, owner.vm.page_size);
    expect_ok(api->mprotect(heap, map, owner.vm.page_size, UHM_PROT_READ),
              "default platform mprotect");
    expect_ok(api->munmap(heap, map, owner.vm.page_size),
              "default platform munmap");
    expect_ok(api->check(heap, 0), "default platform check");
    expect_ok(uhm_platform_heap_close(&owner), "default platform heap close");
}

static void test_platform_heap_default_tls_policy(void) {
    uhm_platform_heap_config config;
    uhm_platform_heap owner;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *ptr;

    expect_ok(uhm_platform_heap_open_default(&owner, &heap),
              "platform default TLS heap open");
    expect_true((owner.flags & UHM_PLATFORM_HEAP_ENABLE_TLS_CACHE) != 0u,
                "platform default enables TLS flag");
    expect_true(heap->core.tls_cache_slot_count ==
                    uhm_platform_default_tls_registry_slots(),
                "platform default TLS registry slots");
    expect_true(uhm_platform_default_tls_registry_slots() ==
                        (sizeof(void *) >= 8u ? 8u : 4u) &&
                    uhm_platform_default_tls_max_pages() ==
                        (sizeof(void *) >= 8u ? 3u : 2u),
                "platform default TLS constants");
    expect_true(heap->core.tls_cache_min_pages == 1u &&
                    heap->core.tls_cache_default_pages == 1u &&
                    heap->core.tls_cache_max_pages ==
                        uhm_platform_default_tls_max_pages() &&
                    heap->core.tls_cache_max_retained_pages ==
                        uhm_platform_default_tls_max_pages() *
                            UHM_LOCAL_RETAINED_PAGE_FACTOR,
                "platform default TLS page range");
    api = uhm_get_api(heap);
    expect_true(api != 0, "platform default TLS api");
    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "platform default TLS malloc");
    api->free(heap, ptr);
    expect_true(heap->core.tls_cache_active_count == 1u &&
                    heap->core.tls_cache_head != 0 &&
                    heap->core.tls_cache_head->local_cache_count == 1u,
                "platform default TLS caches freed slot");
    expect_ok(api->check(heap, 0), "platform default TLS check");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform default TLS heap close");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = 2u * 1024u * 1024u;
    config.thread_registry_slots = 3u;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform custom TLS slots heap open");
    expect_true((owner.flags & UHM_PLATFORM_HEAP_ENABLE_TLS_CACHE) != 0u,
                "platform custom slots keeps default TLS enabled");
    expect_true(heap->core.tls_cache_slot_count == 3u &&
                    heap->core.tls_cache_max_pages ==
                        uhm_platform_default_tls_max_pages(),
                "platform custom slots with default TLS pages");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform custom TLS slots heap close");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = 2u * 1024u * 1024u;
    config.tls_cache_min_pages = 1u;
    config.tls_cache_default_pages = 2u;
    config.tls_cache_max_pages = 3u;
    config.tls_cache_max_retained_pages = 7u;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform explicit TLS retained ceiling open");
    expect_true(heap->core.tls_cache_max_retained_pages == 7u,
                "platform explicit TLS retained ceiling propagated");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform explicit TLS retained ceiling close");

    config.tls_cache_max_retained_pages = 2u;
    expect_true(uhm_platform_heap_open(&config, &owner, &heap) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "platform rejects retained ceiling below byte max");

    config.size = UHM_PLATFORM_HEAP_CONFIG_SIZE_ABI17;
    config.abi_minor = 17u;
    config.tls_cache_max_retained_pages = 7u;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform ABI17 ignores retained tail");
    expect_true(heap->core.tls_cache_max_retained_pages ==
                    3u * UHM_LOCAL_RETAINED_PAGE_FACTOR,
                "platform ABI17 uses compatible retained default");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform ABI17 retained close");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_DISABLE_TLS_CACHE;
    config.arena_size = 2u * 1024u * 1024u;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform disable TLS heap open");
    expect_true((owner.flags & UHM_PLATFORM_HEAP_DISABLE_TLS_CACHE) != 0u &&
                    (owner.flags & UHM_PLATFORM_HEAP_ENABLE_TLS_CACHE) == 0u,
                "platform disable TLS owner flags");
    expect_true(heap->core.tls_cache_max_pages == 0u,
                "platform disable TLS max pages");
    api = uhm_get_api(heap);
    expect_true(api != 0, "platform disable TLS api");
    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "platform disable TLS malloc");
    api->free(heap, ptr);
    expect_true(heap->core.tls_cache_active_count == 0u &&
                    heap->core.tls_cache_head == 0,
                "platform disable TLS does not cache slot");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform disable TLS heap close");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_ENABLE_TLS_CACHE |
                   UHM_PLATFORM_HEAP_DISABLE_TLS_CACHE;
    expect_true(uhm_platform_heap_open(&config, &owner, &heap) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "platform TLS rejects enable-disable conflict");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_DISABLE_TLS_CACHE;
    config.tls_cache_max_pages = 1u;
    expect_true(uhm_platform_heap_open(&config, &owner, &heap) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "platform TLS rejects disabled cache parameters");
}

static void test_platform_heap_lockless_remote_policy(void) {
    uhm_platform_heap_config config;
    uhm_platform_heap owner;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_abi_info abi;
    uhm_stats stats;
    uint32_t expected_enabled;

    memset(&abi, 0, sizeof(abi));
    abi.size = sizeof(abi);
    expect_ok(uhm_abi_query(&abi), "platform lockless policy ABI query");
    expected_enabled = abi.lockless_remote_config_supported != 0u ? 1u : 0u;

    expect_ok(uhm_platform_heap_open_default(&owner, &heap),
              "platform default lockless policy open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "platform default lockless policy api");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "platform default lockless policy stats");
    expect_true((stats.config_flags_enabled &
                 UHM_CONFIG_LOCKLESS_REMOTE_FREE) == 0u &&
                    stats.lockless_remote_free_enabled == 0u &&
                    stats.lockless_remote_malloc_enabled == 0u &&
                    stats.lockless_remote_mode == UHM_LOCKLESS_REMOTE_MODE_NONE,
                "platform wrapper keeps lockless remote opt-in");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform default lockless policy close");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = 2u * 1024u * 1024u;
    config.heap_flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform explicit lockless policy open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "platform explicit lockless policy api");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "platform explicit lockless policy stats");
    expect_true(((stats.config_flags_enabled &
                  UHM_CONFIG_LOCKLESS_REMOTE_FREE) != 0u) ==
                    (expected_enabled != 0u) &&
                    stats.lockless_remote_free_enabled == expected_enabled &&
                    stats.lockless_remote_malloc_enabled == expected_enabled,
                "platform explicit lockless policy follows capability");
    expect_true((expected_enabled != 0u &&
                 stats.lockless_remote_mode != UHM_LOCKLESS_REMOTE_MODE_NONE) ||
                    (expected_enabled == 0u &&
                     stats.lockless_remote_mode == UHM_LOCKLESS_REMOTE_MODE_NONE),
                "platform explicit lockless policy reports effective mode");
    expect_ok(api->check(heap, 0), "platform explicit lockless policy check");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform explicit lockless policy close");
}

static void test_platform_heap_merged_vma_alternating_mprotect(void) {
    enum { map_count = 512 };
    uhm_platform_heap_config config;
    uhm_platform_heap owner;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *maps[map_count];
    size_t sizes[map_count];
    size_t page_size;
    size_t i;

    memset(maps, 0, sizeof(maps));
    memset(sizes, 0, sizeof(sizes));
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = 32u * 1024u * 1024u;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform merged-vma alternating heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "platform merged-vma alternating api");
    page_size = owner.vm.page_size;
    expect_true(page_size != 0, "platform merged-vma alternating page size");

    for (i = 0; i < map_count; ++i) {
        sizes[i] = page_size * (1u + (i & 3u));
        expect_ok(api->mmap(heap, &maps[i], sizes[i],
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE),
                  "platform merged-vma alternating mmap");
        memset(maps[i], (int)i, page_size);
    }
    expect_ok(api->check(heap, 0),
              "platform merged-vma alternating after mmap check");

    for (i = 0; i < map_count; ++i) {
        uint32_t prot = (i & 1u) ? (UHM_PROT_READ | UHM_PROT_WRITE)
                                 : UHM_PROT_READ;
        uhm_result protect_result = api->mprotect(heap, maps[i], sizes[i], prot);
        if (protect_result != UHM_OK) {
            fprintf(stderr,
                    "platform merged-vma alternating failed index=%zu size=%zu prot=%u result=%d\n",
                    i, sizes[i], prot, (int)protect_result);
        }
        expect_ok(protect_result, "platform merged-vma alternating mprotect");
        expect_ok(api->check(heap, 0),
                  "platform merged-vma alternating mprotect check");
    }

    for (i = 0; i < map_count; ++i) {
        expect_ok(api->munmap(heap, maps[i], sizes[i]),
                  "platform merged-vma alternating cleanup munmap");
    }
    expect_ok(api->check(heap, 0),
              "platform merged-vma alternating final check");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform merged-vma alternating heap close");
}

static void test_platform_heap_decommit_retention_config(void) {
    uhm_platform_heap_config config;
    uhm_platform_heap owner;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_stats before;
    uhm_stats after;
    void *ptr;
    size_t page_size;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = 2u * 1024u * 1024u;
    config.malloc_free_decommit_retention_pages = 1u;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform retention config heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "platform retention config api");
    page_size = owner.vm.page_size;
    expect_true(page_size != 0, "platform retention config page size");

    ptr = api->malloc(heap, page_size * 4u);
    expect_true(ptr != 0, "platform retention config malloc");
    memset(ptr, 0x8cu, page_size * 4u);
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before),
              "platform retention config stats before free");
    api->free(heap, ptr);
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after),
              "platform retention config stats after free");
    expect_true(after.decommitted_bytes > before.decommitted_bytes,
                "platform retention config decommits small large extent");
    expect_ok(api->check(heap, 0), "platform retention config check");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform retention config heap close");
}

static void test_platform_heap_config_abi7_tail_compat(void) {
    uhm_platform_heap_config config;
    uhm_platform_heap owner;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_stats before;
    uhm_stats after;
    void *ptr;
    size_t page_size;

    memset(&config, 0, sizeof(config));
    config.size = UHM_PLATFORM_HEAP_CONFIG_SIZE_ABI7;
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = 7u;
    config.arena_size = 2u * 1024u * 1024u;
    config.malloc_free_decommit_retention_pages = 1u;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform abi7 tail compat heap open");
    api = uhm_get_api(heap);
    expect_true(api != 0, "platform abi7 tail compat api");
    page_size = owner.vm.page_size;
    expect_true(page_size != 0, "platform abi7 tail compat page size");

    ptr = api->malloc(heap, page_size * 4u);
    expect_true(ptr != 0, "platform abi7 tail compat malloc");
    memset(ptr, 0x91u, page_size * 4u);
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(api->get_stats(heap, &before),
              "platform abi7 tail compat stats before");
    api->free(heap, ptr);
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(api->get_stats(heap, &after),
              "platform abi7 tail compat stats after");
    expect_true(after.decommitted_bytes == before.decommitted_bytes,
                "platform abi7 tail compat ignores abi8 retention field");
    expect_ok(api->check(heap, 0), "platform abi7 tail compat check");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform abi7 tail compat heap close");
}

static void test_platform_heap_auto_treap_repair_policy(void) {
    uhm_platform_heap_config config;
    uhm_platform_heap owner;
    uhm_heap *heap = 0;
    const uhm_api *api;
    uhm_stats stats;
    void *ptr;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_AUTO_TREAP_REPAIR;
    config.arena_size = 2u * 1024u * 1024u;
    expect_ok(uhm_platform_heap_open(&config, &owner, &heap),
              "platform auto treap repair heap open");
    expect_true((owner.flags & UHM_PLATFORM_HEAP_AUTO_TREAP_REPAIR) != 0u &&
                    (owner.flags & UHM_PLATFORM_HEAP_ENABLE_TLS_CACHE) != 0u,
                "platform auto treap repair owner flags");
    api = uhm_get_api(heap);
    expect_true(api != 0, "platform auto treap repair api");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats),
              "platform auto treap repair stats");
    expect_true((stats.config_flags_enabled &
                 UHM_CONFIG_REPAIR_TREAP_HEALTH) != 0u,
                "platform auto treap repair enables core flag");
#if UHMAS_ENABLE_DIAGNOSTICS
    ptr = api->malloc(heap, 64u);
    expect_true(ptr != 0, "platform auto treap repair TLS warmup malloc");
    api->free(heap, ptr);
    {
        size_t i;
        for (i = 0; i < 1024u; ++i) {
            ptr = api->malloc(heap, 64u);
            expect_true(ptr != 0,
                        "platform auto treap repair TLS steady warmup");
            api->free(heap, ptr);
        }
    }
    uhm_spinlock_set_diagnostics_enabled(&heap->lock, 1u);
    {
        uint64_t lock_calls_before =
            uhm_diag_counter_load_relaxed(&heap->lock.lock_calls);
        size_t i;
        for (i = 0; i < 128u; ++i) {
            ptr = api->malloc(heap, 64u);
            expect_true(ptr != 0,
                        "platform auto treap repair healthy TLS malloc");
            api->free(heap, ptr);
        }
        expect_true(
            uhm_diag_counter_load_relaxed(&heap->lock.lock_calls) -
                    lock_calls_before <=
                128u,
            "platform auto treap repair healthy TLS path avoids per-op heap lock");
    }
    uhm_spinlock_set_diagnostics_enabled(&heap->lock, 0u);
#else
    (void)ptr;
#endif
    expect_ok(api->check(heap, 0), "platform auto treap repair check");
    expect_ok(uhm_platform_heap_close(&owner),
              "platform auto treap repair heap close");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_AUTO_TREAP_REPAIR;
    config.heap_flags = UHM_CONFIG_FAIL_ON_TREAP_HEALTH;
    expect_true(uhm_platform_heap_open(&config, &owner, &heap) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "platform auto treap repair rejects fail-fast conflict");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.flags = UHM_PLATFORM_HEAP_DISABLE_TLS_CACHE << 1u;
    expect_true(uhm_platform_heap_open(&config, &owner, &heap) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "platform heap rejects unknown wrapper flag");
}

int main(void) {
    uhm_platform_heap_config heap_config;
    uhm_platform_heap heap_owner;
    uhm_vm_ops vm;
    uhm_heap *heap = 0;
    const uhm_api *api;
    void *p;
    void *map = 0;
    void *split_map = 0;
    void *trim_map = 0;
    void *replace_map = 0;
    void *page_private = 0;
    void *large_malloc = 0;
    void *large_reuse = 0;
    unsigned char *page_reuse;
    uhm_stats stats;
    uhm_stats large_before;
    uhm_stats large_after;

    test_windows_mem_release_contract();
    test_platform_numa_backend();
    test_platform_heap_numa_backend();
    test_platform_heap_default_open();
    test_platform_heap_default_tls_policy();
    test_platform_heap_lockless_remote_policy();
    test_platform_heap_merged_vma_alternating_mprotect();
    test_platform_heap_decommit_retention_config();
    test_platform_heap_config_abi7_tail_compat();
    test_platform_heap_auto_treap_repair_policy();

    expect_ok(uhm_platform_vm_ops_init(&vm), "platform vm init");
    expect_true(vm.page_size != 0, "page size");
    expect_true((vm.capabilities & UHM_VM_CAN_RESERVE) != 0, "reserve capability");
    expect_true((vm.capabilities & UHM_VM_CAN_COMMIT) != 0, "commit capability");

    memset(&heap_config, 0, sizeof(heap_config));
    heap_config.size = sizeof(heap_config);
    heap_config.abi_major = UHM_ABI_MAJOR;
    heap_config.abi_minor = UHM_ABI_MINOR;
    heap_config.arena_size = 2u * 1024u * 1024u + 1u;
    expect_ok(uhm_platform_heap_open(&heap_config, &heap_owner, &heap),
              "open with platform heap wrapper");

    api = uhm_get_api(heap);
    expect_true(api != 0, "api");
    vm = heap_owner.vm;
    expect_true(vm.page_size != 0, "owner page size");
    {
        size_t rounded_arena_size = 0;
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_true(uhm_platform_size_align_up(heap_config.arena_size,
                                               vm.page_size,
                                               &rounded_arena_size),
                    "platform wrapper arena round helper");
        expect_ok(api->get_stats(heap, &stats), "platform wrapper arena stats");
        expect_true(stats.arena_size == rounded_arena_size,
                    "platform wrapper rounds arena size to page multiple");
    }

    p = api->malloc(heap, 256u);
    expect_true(p != 0, "malloc");
    memset(p, 0x5a, 256u);
    {
        void *malloc_page_conflict =
            (void *)((uintptr_t)p & ~((uintptr_t)vm.page_size - 1u));
        expect_true(api->mmap(heap, &malloc_page_conflict, 1u,
                              UHM_PROT_READ | UHM_PROT_WRITE,
                              UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                        UHM_ERR_ALREADY_MAPPED,
                    "platform mmap rejects malloc-owned page");
    }

    large_malloc = api->malloc(heap, vm.page_size * 12u);
    expect_true(large_malloc != 0, "platform large malloc extent");
    memset(large_malloc, 0x6d, vm.page_size * 12u);
    memset(&large_before, 0, sizeof(large_before));
    large_before.size = sizeof(large_before);
    expect_ok(api->get_stats(heap, &large_before), "platform large stats before free");
    api->free(heap, large_malloc);
    memset(&large_after, 0, sizeof(large_after));
    large_after.size = sizeof(large_after);
    expect_ok(api->get_stats(heap, &large_after), "platform large stats after free");
    expect_true(large_after.decommitted_bytes > large_before.decommitted_bytes,
                "platform large free decommits pages");
    large_reuse = api->malloc(heap, vm.page_size * 12u);
    expect_true(large_reuse == large_malloc, "platform large malloc reuses extent");
    memset(large_reuse, 0x7e, vm.page_size * 12u);
    api->free(heap, large_reuse);

    expect_ok(api->mmap(heap, &map, vm.page_size + 1u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "platform mmap rounds request");
    expect_true(map != 0, "mmap address");
    memset(map, 0xa5, vm.page_size * 2u);
    {
        void *fixed_conflict = map;
        expect_true(api->mmap(heap, &fixed_conflict, vm.page_size,
                              UHM_PROT_READ | UHM_PROT_WRITE,
                              UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) ==
                        UHM_ERR_ALREADY_MAPPED,
                    "fixed noreplace rejects live range");
    }
    expect_ok(api->mprotect(heap, map, vm.page_size, UHM_PROT_READ), "platform mprotect");
    expect_ok(api->munmap(heap, map, vm.page_size + 1u),
              "platform munmap rounds request");
    {
        void *fixed_reuse = map;
        expect_ok(api->mmap(heap, &fixed_reuse, vm.page_size * 2u,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
                  "fixed noreplace reuses unmapped range");
        expect_true(fixed_reuse == map, "fixed noreplace same address");
        memset(fixed_reuse, 0x3c, vm.page_size * 2u);
        expect_ok(api->munmap(heap, fixed_reuse, vm.page_size * 2u), "fixed reuse munmap");
    }

    {
        void *resize_map = 0;
        void *same;
        uintptr_t resize_addr;

        expect_ok(api->mmap(heap, &resize_map, vm.page_size + 1u,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE),
                  "platform mremap initial mmap");
        resize_addr = (uintptr_t)resize_map;
        memset(resize_map, 0x4d, vm.page_size * 2u);
        same = resize_map;
        expect_ok(api->mremap(heap, &same, vm.page_size + 1u,
                              vm.page_size, 0u),
                  "platform mremap shrink");
        expect_true(same == resize_map, "platform mremap shrink address");
        expect_true(api->munmap(heap, (void *)(resize_addr + vm.page_size),
                                1u) == UHM_ERR_NOT_MAPPED,
                    "platform mremap shrink rounded tail");
        same = resize_map;
        expect_ok(api->mremap(heap, &same, vm.page_size,
                              vm.page_size + 1u, 0u),
                  "platform mremap grow");
        expect_true(same == resize_map, "platform mremap grow address");
        memset((void *)(resize_addr + vm.page_size), 0x5e, vm.page_size);
        expect_ok(api->munmap(heap, resize_map, vm.page_size + 1u),
                  "platform mremap cleanup");
    }

    expect_ok(api->mmap(heap, &split_map, vm.page_size * 4u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "split mmap");
    expect_ok(api->mprotect(heap, (void *)((uintptr_t)split_map + vm.page_size), vm.page_size,
                            UHM_PROT_READ),
              "middle page mprotect");
    memset(split_map, 0x11, vm.page_size);
    memset((void *)((uintptr_t)split_map + vm.page_size * 2u), 0x22, vm.page_size);
    expect_ok(api->munmap(heap, (void *)((uintptr_t)split_map + vm.page_size), vm.page_size),
              "middle page munmap");
    {
        void *middle = (void *)((uintptr_t)split_map + vm.page_size);
        if (vm.allocation_granularity > vm.page_size) {
            expect_true(((uintptr_t)middle % vm.allocation_granularity) != 0,
                        "fixed reuse target is not allocation-granularity aligned");
        }
        expect_ok(api->mmap(heap, &middle, vm.page_size, UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE),
                  "middle page fixed reuse");
        expect_true(middle == (void *)((uintptr_t)split_map + vm.page_size),
                    "middle page same address");
        memset(middle, 0x33, vm.page_size);
        expect_ok(api->munmap(heap, middle, vm.page_size), "middle page final munmap");
    }
    expect_ok(api->munmap(heap, split_map, vm.page_size), "left page munmap");
    expect_ok(api->munmap(heap, (void *)((uintptr_t)split_map + vm.page_size * 2u),
                          vm.page_size * 2u),
              "right tail munmap");

    expect_ok(api->mmap(heap, &trim_map, vm.page_size * 3u, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "trim mmap");
    memset(trim_map, 0x44, vm.page_size * 3u);
    expect_ok(api->munmap(heap, trim_map, vm.page_size), "head munmap");
    memset((void *)((uintptr_t)trim_map + vm.page_size), 0x55, vm.page_size);
    memset((void *)((uintptr_t)trim_map + vm.page_size * 2u), 0x66, vm.page_size);
    expect_ok(api->munmap(heap, (void *)((uintptr_t)trim_map + vm.page_size * 2u), vm.page_size),
              "tail munmap");
    memset((void *)((uintptr_t)trim_map + vm.page_size), 0x77, vm.page_size);
    expect_ok(api->munmap(heap, (void *)((uintptr_t)trim_map + vm.page_size), vm.page_size),
              "remaining middle munmap");

    expect_ok(api->mmap(heap, &replace_map, vm.page_size * 3u,
                        UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE),
              "fixed replace platform source mmap");
    memset(replace_map, 0x88, vm.page_size * 3u);
    {
        void *replace_middle = (void *)((uintptr_t)replace_map + vm.page_size);
        expect_ok(api->mmap(heap, &replace_middle, vm.page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE,
                            UHM_MAP_PRIVATE | UHM_MAP_FIXED),
                  "fixed replace platform middle page");
        expect_true(replace_middle == (void *)((uintptr_t)replace_map + vm.page_size),
                    "fixed replace platform same address");
        memset(replace_middle, 0x99, vm.page_size);
    }
    memset(replace_map, 0xaa, vm.page_size);
    memset((void *)((uintptr_t)replace_map + vm.page_size * 2u), 0xbb, vm.page_size);
    expect_ok(api->munmap(heap, replace_map, vm.page_size * 3u),
              "fixed replace platform cleanup");

    page_private = api->aligned_alloc(heap, vm.page_size, vm.page_size * 2u);
    expect_true(page_private != 0, "platform page-private malloc");
    expect_true(((uintptr_t)page_private % vm.page_size) == 0,
                "platform page-private alignment");
    memset(page_private, 0x12, vm.page_size * 2u);
    expect_ok(api->mprotect(heap, page_private, vm.page_size, UHM_PROT_READ),
              "platform page-private malloc mprotect read");
    expect_ok(api->mprotect(heap, page_private, vm.page_size, UHM_PROT_READ),
              "platform page-private malloc mprotect read no-op");
    expect_ok(api->mprotect(heap, page_private, vm.page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "platform page-private malloc mprotect write");
    expect_ok(api->mprotect(heap, page_private, vm.page_size,
                            UHM_PROT_READ | UHM_PROT_WRITE),
              "platform page-private malloc mprotect write no-op");
    memset(page_private, 0x34, vm.page_size);
    expect_ok(api->mprotect(heap, page_private, vm.page_size, UHM_PROT_READ),
              "platform page-private malloc mprotect before free");
    api->free(heap, page_private);
    page_reuse = (unsigned char *)api->malloc(heap, 1024u);
    expect_true(page_reuse == page_private, "platform page-private malloc reuse");
    memset(page_reuse, 0x56, 1024u);
    api->free(heap, page_reuse);

    api->free(heap, p);
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "stats");
    expect_true(stats.physical_released_bytes >= vm.page_size,
                "platform decommit counted as physical release");
    expect_ok(api->check(heap, 0), "check");
    expect_ok(uhm_platform_heap_close(&heap_owner), "platform heap close");
    return 0;
}
