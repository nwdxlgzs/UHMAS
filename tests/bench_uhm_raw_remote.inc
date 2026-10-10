#if UINTPTR_MAX == UINT32_MAX
static bench_result bench_raw_remote_partial_burst(
    bench_heap_env *env, size_t iterations, const char *name) {
    void *slots[34];
    uhm_slab *slab;
    size_t burst;
    size_t i;
    size_t round;
    uint64_t begin;
    uint64_t end;

    slots[0] = env->api->malloc(env->heap, 64u);
    bench_expect(slots[0] != 0, "raw remote burst first allocation");
    slab = ((uhm_slab_slot *)((uintptr_t)slots[0] -
                              bench_slab_slot_header_size()))->slab;
    bench_expect(slab && slab->total_slots > 3u,
                 "raw remote burst slab geometry");
    burst = slab->total_slots - 3u;
    if (burst > 32u) {
        burst = 32u;
    }
    for (i = 1u; i < burst + 2u; ++i) {
        slots[i] = env->api->malloc(env->heap, 64u);
        bench_expect(slots[i] != 0, "raw remote burst allocation");
    }
    begin = bench_now_ns();
    for (round = 0u; round < iterations; ++round) {
        for (i = 2u; i < burst + 2u; ++i) {
            env->api->free(env->heap, slots[i]);
        }
        for (i = 2u; i < burst + 2u; ++i) {
            slots[i] = env->api->malloc(env->heap, 64u);
            bench_expect(slots[i] != 0, "raw remote burst reuse");
        }
    }
    end = bench_now_ns();
    for (i = 0u; i < burst + 2u; ++i) {
        env->api->free(env->heap, slots[i]);
    }
    return bench_result_make(name, begin, end, iterations * burst * 2u);
}
#endif
