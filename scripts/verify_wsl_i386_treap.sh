#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: verify_wsl_i386_treap.sh <build-dir>" >&2
    exit 2
fi

cd "$1"
UHMAS_BENCH_DIAGNOSTICS=1 \
UHMAS_BENCH_TREAP_ADVERSARIAL=1 \
    ./uhmas_benchmarks > treap_diag.log

abi_line=$(grep '^abi capabilities:' treap_diag.log | head -n 1 || true)
if [ -z "$abi_line" ]; then
    cat treap_diag.log
    echo "32-bit ABI capability line missing" >&2
    exit 1
fi
echo "32-bit ABI capabilities (WSL i386 treap diagnostics): $abi_line"
require_abi_metric() {
    key=$1
    expected=$2
    if ! echo "$abi_line" | grep -Eq "(^|[[:space:]])$key=$expected([[:space:]]|$)"; then
        echo "$abi_line" >&2
        echo "32-bit ABI capability $key expected $expected" >&2
        exit 1
    fi
}

require_abi_metric 'pointer_size' '4'
require_abi_metric 'size_t_size' '4'
require_abi_metric 'atomic_u32_lock_free' '1'
require_abi_metric 'tagged_freelist_supported' '0'
require_abi_metric 'tagged_freelist_tag_bits' '0'
require_abi_metric 'lockless_remote_primitives_supported' '0'
require_abi_metric 'lockless_remote_config_supported' '0'
require_abi_metric 'malloc_min_alignment' '16'
require_abi_metric 'cacheline_size' '64'
require_abi_metric 'call_abi' '0'

diag_count=$(grep -c '^diag|treap ' treap_diag.log || true)
for name in \
    'treap malloc free-tree pressure' \
    'treap same-size malloc free-tree pressure' \
    'treap malloc free-tree remove pressure' \
    'treap malloc free-tree two-child remove pressure' \
    'treap vma live-tree pressure' \
    'treap fixed sparse VMA build pressure' \
    'treap fixed sparse VMA protect pressure' \
    'treap mmap free-tree pressure'
do
    if ! grep -q "^$name" treap_diag.log; then
        cat treap_diag.log
        echo "treap diagnostics did not emit '$name'" >&2
        exit 1
    fi
done
if [ "$diag_count" -lt 7 ]; then
    cat treap_diag.log
    echo "treap diagnostics did not emit all focused 32-bit cases" >&2
    exit 1
fi

for name in \
    'treap malloc free-tree pressure' \
    'treap same-size malloc free-tree pressure' \
    'treap malloc free-tree remove pressure' \
    'treap malloc free-tree two-child remove pressure' \
    'treap vma live-tree pressure' \
    'treap fixed sparse VMA build pressure' \
    'treap mmap free-tree pressure'
do
    if ! grep -q "^diag|$name|" treap_diag.log; then
        cat treap_diag.log
        echo "treap diagnostics did not attribute '$name'" >&2
        exit 1
    fi
done

if grep -E '(treap_health_flags|priority_ties|depth_limit_exceeded|insert_budget_exceeded|insert_rotation_budget_exceeded|find_budget_exceeded|remove_budget_exceeded)=[1-9][0-9]*' treap_diag.log; then
    echo "32-bit treap diagnostics exceeded budget or reported priority ties" >&2
    exit 1
fi

bucket_count=$(grep -E -c 'priority_bucket_used=([4-9]|1[0-6])' treap_diag.log || true)
if [ "$bucket_count" -lt 7 ]; then
    cat treap_diag.log
    echo "32-bit treap diagnostics priority bucket coverage is too narrow" >&2
    exit 1
fi

require_positive_metric() {
    name=$1
    key=$2
    if ! grep "^diag|$name|" treap_diag.log | grep -Eq "$key=[1-9][0-9]*"; then
        cat treap_diag.log
        echo "32-bit treap diagnostics missing positive $key for '$name'" >&2
        exit 1
    fi
}

require_positive_metric 'treap malloc free-tree pressure' \
    'treap_priority_mix_ns'
require_positive_metric 'treap malloc free-tree pressure' \
    'live_tree_priority_mix_ns'
require_positive_metric 'treap malloc free-tree pressure' \
    'malloc_tree_priority_mix_ns'
require_positive_metric 'treap malloc free-tree pressure' \
    'live_tree_remove_sink_steps'
require_positive_metric 'treap malloc free-tree pressure' \
    'malloc_tree_remove_sink_steps'
require_positive_metric 'treap same-size malloc free-tree pressure' \
    'treap_priority_mix_ns'
require_positive_metric 'treap same-size malloc free-tree pressure' \
    'live_tree_priority_mix_ns'
require_positive_metric 'treap same-size malloc free-tree pressure' \
    'malloc_tree_priority_mix_ns'
require_positive_metric 'treap same-size malloc free-tree pressure' \
    'live_tree_remove_sink_steps'
require_positive_metric 'treap same-size malloc free-tree pressure' \
    'malloc_tree_remove_calls'
require_positive_metric 'treap vma live-tree pressure' \
    'treap_priority_mix_ns'
require_positive_metric 'treap vma live-tree pressure' \
    'vma_tree_priority_mix_ns'
require_positive_metric 'treap fixed sparse VMA build pressure' \
    'treap_priority_mix_ns'
require_positive_metric 'treap fixed sparse VMA build pressure' \
    'vma_tree_priority_mix_ns'
require_positive_metric 'treap fixed sparse VMA build pressure' \
    'mmap_free_tree_priority_mix_ns'
require_positive_metric 'treap fixed sparse VMA build pressure' \
    'vma_tree_insertions'
require_positive_metric 'treap fixed sparse VMA build pressure' \
    'mmap_free_tree_remove_calls'
require_positive_metric 'treap mmap free-tree pressure' \
    'treap_priority_mix_ns'
require_positive_metric 'treap mmap free-tree pressure' \
    'vma_tree_priority_mix_ns'
require_positive_metric 'treap mmap free-tree pressure' \
    'mmap_free_tree_priority_mix_ns'
require_positive_metric 'treap mmap free-tree pressure' \
    'vma_tree_remove_sink_steps'
require_positive_metric 'treap mmap free-tree pressure' \
    'mmap_free_tree_remove_sink_steps'
