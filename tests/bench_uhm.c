#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

/*
 * UHMAS benchmark unity translation unit.
 *
 * Bench helpers and scenario groups are split into include-only blocks.
 */
#include "bench_uhm_common.inc"
#include "bench_uhm_env.inc"
#include "bench_uhm_core_cases.inc"
#include "bench_uhm_raw_remote.inc"
#include "bench_uhm_pressure_cases.inc"
#include "bench_uhm_main.inc"
