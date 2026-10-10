#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: verify_wsl_i386_gate_bench.sh <build-dir>" >&2
    exit 2
fi

cd "$1"
UHMAS_BENCH_32BIT_GATE=1 ./uhmas_benchmarks > gate_bench.log

for name in \
    'uhm 32-bit locked partial burst' \
    'uhm 32-bit low-lock partial burst' \
    'false-share remote free/reuse' \
    'uhm thread ctx cache reuse' \
    'uhm TLS cache reuse' \
    'uhm mprotect split/merge cost' \
    'uhm mremap resize cost' \
    'uhm mmap churn' \
    'uhm malloc/mmap collision pressure'
do
    if ! grep -q "^$name" gate_bench.log; then
        cat gate_bench.log
        echo "32-bit gate benchmark did not emit '$name'" >&2
        exit 1
    fi
done

UHMAS_BENCH_DIAGNOSTICS=1 \
UHMAS_BENCH_SLAB_EMPTY_CHURN=1 \
    ./uhmas_benchmarks > slab_empty_churn.log

for name in \
    'slab empty adaptive churn 64B' \
    'slab empty adaptive churn 512B' \
    'slab empty full-drain batch 512B'
do
    if ! grep -q "^$name" slab_empty_churn.log; then
        cat slab_empty_churn.log
        echo "32-bit focused benchmark did not emit '$name'" >&2
        exit 1
    fi
done

slab_diag_count=$(grep -c '^diag|slab empty adaptive churn ' slab_empty_churn.log || true)
if [ "$slab_diag_count" -lt 2 ]; then
    cat slab_empty_churn.log
    echo "32-bit slab-empty focused diagnostics missing result lines" >&2
    exit 1
fi

if grep '^diag|slab empty adaptive churn ' slab_empty_churn.log | \
    grep -E '(slab_empty_decommit_pressure_events|slab_empty_committed_cache_adaptive_grow_events|decommit_events|recommit_events|decommit_recommit_same_addr_events)=([2-9]|[1-9][0-9]+)'; then
    echo "32-bit slab-empty focused diagnostics exceeded count threshold" >&2
    exit 1
fi
if grep -E '(treap_health_flags|vm_lock_held_calls|vm_commit_lock_held_calls|vm_decommit_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|metadata_quarantine_drain_budget_exhausted)=[1-9][0-9]*' slab_empty_churn.log; then
    echo "32-bit slab-empty focused diagnostics exceeded zero threshold" >&2
    exit 1
fi

batch_diag=$(grep '^diag|slab empty full-drain batch 512B|' \
    slab_empty_churn.log || true)
if [ "$(printf '%s\n' "$batch_diag" | grep -c '^diag|' || true)" -ne 1 ]; then
    cat slab_empty_churn.log
    echo "32-bit slab-empty full-drain diagnostics missing result line" >&2
    exit 1
fi
for pattern in \
    'slab_empty_decommit_pressure_events=108( |$)' \
    'slab_empty_committed_cache_adaptive_grow_events=0( |$)' \
    'decommit_events=108( |$)' \
    'recommit_events=0( |$)' \
    'decommit_recommit_same_addr_events=0( |$)' \
    'page_mark_calls=108( |$)' \
    'page_mark_pages=108( |$)' \
    'vm_commit_calls=0( |$)' \
    'vm_decommit_calls=9( |$)' \
    'vm_decommit_failures=0( |$)' \
    'vm_lock_held_calls=0( |$)' \
    'vm_decommit_lock_held_calls=0( |$)'
do
    if ! printf '%s\n' "$batch_diag" | grep -E "$pattern" >/dev/null; then
        printf '%s\n' "$batch_diag"
        echo "32-bit slab-empty full-drain diagnostic invariant failed: $pattern" >&2
        exit 1
    fi
done
batch_attribution=$(grep \
    '^metadata attribution slab empty full-drain batch 512B:' \
    slab_empty_churn.log || true)
if [ "$(printf '%s\n' "$batch_attribution" | \
          grep -c '^metadata attribution ' || true)" -ne 1 ] || \
   ! printf '%s\n' "$batch_attribution" | \
       grep -E 'operations=9 .*vm_calls=9 vm_failures=0 .*vm_lock_held_calls=0 .*vm_commit_calls=0 vm_decommit_calls=9 .*decommit_events=108 .*recommit_events=0 .*same_addr_events=0 .*page_mark_pages=108 ' >/dev/null; then
    printf '%s\n' "$batch_attribution"
    echo "32-bit slab-empty full-drain attribution invariant failed" >&2
    exit 1
fi
batch_latency=$(grep '^latency|slab empty full-drain batch 512B|' \
    slab_empty_churn.log || true)
if [ "$(printf '%s\n' "$batch_latency" | grep -c '^latency|' || true)" -ne 1 ] || \
   ! printf '%s\n' "$batch_latency" | \
       grep -E 'samples=9 .*p95_ns=[1-9][0-9]* .*p99_ns=[1-9][0-9]* .*work_units_per_sample=12 .*pages_per_sample=12 ' >/dev/null; then
    printf '%s\n' "$batch_latency"
    echo "32-bit slab-empty full-drain latency metadata missing" >&2
    exit 1
fi

UHMAS_BENCH_DIAGNOSTICS=1 \
UHMAS_BENCH_MMAP_LOCALITY=1 \
    ./uhmas_benchmarks > mmap_locality.log

for name in \
    'mmap locality size-hint churn 2p' \
    'mmap locality size-hint churn 4p' \
    'mmap locality size-hint churn 8p' \
    'mmap locality same-bucket hint churn' \
    'mmap locality deep-bucket hint churn' \
    'mmap locality fragmented-ring hint churn' \
    'mmap locality exact-front hot-cache metadata' \
    'mmap locality sparse hot-cache maintenance' \
    'mmap locality adjacent-release recent lookup'; do
    if ! grep -q "^$name" mmap_locality.log; then
        cat mmap_locality.log
        echo "32-bit focused benchmark did not emit $name" >&2
        exit 1
    fi
done
for spec in \
    'mmap locality size-hint churn 2p|2' \
    'mmap locality size-hint churn 4p|4' \
    'mmap locality size-hint churn 8p|8' \
    'mmap locality same-bucket hint churn|4' \
    'mmap locality deep-bucket hint churn|4' \
    'mmap locality fragmented-ring hint churn|4' \
    'mmap locality exact-front hot-cache metadata|2' \
    'mmap locality adjacent-release recent lookup|8'; do
    name=${spec%|*}
    exact_pages=${spec#*|}
    line=$(grep "^metadata $name:" mmap_locality.log || true)
    if [ -z "$line" ]; then
        cat mmap_locality.log
        echo "32-bit mmap-locality metadata missing $name" >&2
        exit 1
    fi
    if ! printf '%s\n' "$line" | \
        grep -E "mmap_locality_samples=[1-9][0-9]*" >/dev/null || \
       ! printf '%s\n' "$line" | \
        grep -E "mmap_locality_exact_pages=$exact_pages" >/dev/null || \
       ! printf '%s\n' "$line" | \
        grep -E "mmap_locality_recent_cache_slots=16" >/dev/null || \
       ! printf '%s\n' "$line" | \
        grep -E "mmap_locality_size_hint_slots=8" >/dev/null || \
       ! printf '%s\n' "$line" | \
        grep -E "mmap_locality_range_fragmented_pages_sum=[1-9][0-9]*" >/dev/null || \
       ! printf '%s\n' "$line" | \
        grep -E "mmap_locality_range_fragmentation_ppm_max=[1-9][0-9]*" >/dev/null || \
       ! printf '%s\n' "$line" | \
        grep -E "mmap_locality_exact_reuse_hits=[1-9][0-9]*" >/dev/null || \
       ! printf '%s\n' "$line" | \
        grep -E "mmap_locality_reuse_distance_pages_max=0" >/dev/null; then
        printf '%s\n' "$line"
        echo "32-bit mmap-locality metadata failed invariant check" >&2
        exit 1
    fi
done
sparse_line=$(grep '^metadata mmap locality sparse hot-cache maintenance:' mmap_locality.log || true)
if [ -z "$sparse_line" ] || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_samples=[1-9][0-9]*' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_exact_pages=1' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_recent_cache_slots=16' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_size_hint_slots=8' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_range_free_extents_sum=1' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_range_free_pages_sum=1' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_range_largest_pages_max=1' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_range_fragmented_pages_sum=0' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_range_fragmentation_ppm_max=0' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_exact_reuse_hits=[1-9][0-9]*' >/dev/null || \
   ! printf '%s\n' "$sparse_line" | grep -E 'mmap_locality_reuse_distance_pages_max=0' >/dev/null; then
    printf '%s\n' "$sparse_line"
    echo "32-bit mmap-locality sparse metadata failed invariant check" >&2
    exit 1
fi
adjacent_line=$(grep '^metadata mmap locality adjacent-release recent lookup:' mmap_locality.log || true)
if [ -z "$adjacent_line" ] || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_samples=[1-9][0-9]*' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_exact_pages=8' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_recent_cache_slots=16' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_size_hint_slots=8' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_range_free_extents_sum=72' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_range_free_pages_sum=72' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_range_largest_pages_max=1' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_range_fragmented_pages_sum=71' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_range_fragmentation_ppm_max=986111' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_exact_reuse_hits=[1-9][0-9]*' >/dev/null || \
   ! printf '%s\n' "$adjacent_line" | grep -E 'mmap_locality_reuse_distance_pages_max=0' >/dev/null; then
    printf '%s\n' "$adjacent_line"
    echo "32-bit mmap-locality adjacent-release metadata failed invariant check" >&2
    exit 1
fi
if [ "$(grep -c '^diag|mmap locality size-hint churn' mmap_locality.log)" -lt 3 ] || \
   [ "$(grep -c '^diag|mmap locality same-bucket hint churn' mmap_locality.log)" -lt 1 ] || \
   [ "$(grep -c '^diag|mmap locality deep-bucket hint churn' mmap_locality.log)" -lt 1 ] || \
   [ "$(grep -c '^diag|mmap locality fragmented-ring hint churn' mmap_locality.log)" -lt 1 ] || \
   [ "$(grep -c '^diag|mmap locality exact-front hot-cache metadata' mmap_locality.log)" -lt 1 ] || \
   [ "$(grep -c '^diag|mmap locality sparse hot-cache maintenance' mmap_locality.log)" -lt 1 ] || \
   [ "$(grep -c '^diag|mmap locality adjacent-release recent lookup' mmap_locality.log)" -lt 1 ]; then
    cat mmap_locality.log
    echo "32-bit mmap-locality focused diagnostics missing result lines" >&2
    exit 1
fi
if grep '^diag|mmap locality size-hint churn' mmap_locality.log | \
    grep -E '(treap_health_flags|mmap_size_hint_misses|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*'; then
    echo "32-bit mmap-locality focused diagnostics exceeded zero threshold" >&2
    exit 1
fi
if grep '^diag|mmap locality same-bucket hint churn' mmap_locality.log | \
    grep -E '(treap_health_flags|mmap_size_hint_misses|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*'; then
    echo "32-bit mmap-locality same-bucket diagnostics exceeded zero threshold" >&2
    exit 1
fi
if grep '^diag|mmap locality deep-bucket hint churn' mmap_locality.log | \
    grep -E '(treap_health_flags|mmap_size_hint_misses|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*'; then
    echo "32-bit mmap-locality deep-bucket diagnostics exceeded zero threshold" >&2
    exit 1
fi
if grep '^diag|mmap locality fragmented-ring hint churn' mmap_locality.log | \
    grep -E '(treap_health_flags|mmap_size_hint_misses|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*'; then
    echo "32-bit mmap-locality fragmented-ring diagnostics exceeded zero threshold" >&2
    exit 1
fi
if grep '^diag|mmap locality exact-front hot-cache metadata' mmap_locality.log | \
    grep -E '(mmap_recent_free_too_small|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*'; then
    echo "32-bit mmap-locality exact-front diagnostics exceeded zero threshold" >&2
    exit 1
fi
if grep '^diag|mmap locality sparse hot-cache maintenance' mmap_locality.log | \
    grep -E '(mmap_recent_free_too_small|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*'; then
    echo "32-bit mmap-locality sparse diagnostics exceeded zero threshold" >&2
    exit 1
fi
if grep '^diag|mmap locality adjacent-release recent lookup' mmap_locality.log | \
    grep -E '(mmap_recent_free_too_small|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*'; then
    echo "32-bit mmap-locality adjacent-release diagnostics exceeded zero threshold" >&2
    exit 1
fi
if grep -E 'mmap_free_tree_find_calls=([2-9]|[1-9][0-9]+)' mmap_locality.log; then
    echo "32-bit mmap-locality focused diagnostics exceeded tree-find threshold" >&2
    exit 1
fi
