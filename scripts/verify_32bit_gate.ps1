param(
    [switch]$SkipWindows,
    [switch]$SkipWsl,
    [string]$WslDistro = "",
    [string]$WslProject = ""
)

$ErrorActionPreference = "Stop"

function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Body
    )
    Write-Host "==> $Name"
    & $Body
}

function Invoke-Native {
    param(
        [string]$File,
        [string[]]$Arguments
    )
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File exited with $LASTEXITCODE"
    }
}

function Assert-32BitAbiCapabilities {
    param(
        [object[]]$Output,
        [string]$Label,
        [ValidateSet("windows", "wsl")]
        [string]$HostPlatform
    )

    $line = @($Output | Where-Object {
            $_ -like "abi capabilities:*"
        } | Select-Object -First 1)
    if ($line.Count -eq 0) {
        $Output | Write-Host
        throw "32-bit ABI capability line missing for $Label"
    }

    $caps = @{}
    foreach ($part in (($line[0] -replace '^abi capabilities:\s*', '') -split '\s+')) {
        if ($part -match '^([^=]+)=([0-9]+)$') {
            $caps[$matches[1]] = [uint64]$matches[2]
        }
    }

    foreach ($key in @(
            "pointer_size",
            "size_t_size",
            "atomic_pointer_lock_free",
            "atomic_u32_lock_free",
            "atomic_u64_lock_free",
            "tagged_freelist_supported",
            "tagged_freelist_tag_bits",
            "lockless_remote_primitives_supported",
            "lockless_remote_config_supported",
            "malloc_min_alignment",
            "cacheline_size",
            "call_abi"
        )) {
        if (-not $caps.ContainsKey($key)) {
            $line | Write-Host
            throw "32-bit ABI capability '$key' missing for $Label"
        }
    }

    $expectedCallAbi = if ($HostPlatform -eq "windows") { 1 } else { 0 }
    if ($caps["pointer_size"] -ne 4 -or
        $caps["size_t_size"] -ne 4 -or
        $caps["atomic_u32_lock_free"] -ne 1 -or
        $caps["tagged_freelist_supported"] -ne 0 -or
        $caps["tagged_freelist_tag_bits"] -ne 0 -or
        $caps["lockless_remote_primitives_supported"] -ne 0 -or
        $caps["lockless_remote_config_supported"] -ne 0 -or
        $caps["malloc_min_alignment"] -ne 16 -or
        $caps["cacheline_size"] -ne 64 -or
        $caps["call_abi"] -ne $expectedCallAbi) {
        $line | Write-Host
        throw "32-bit ABI capability constraints failed for $Label"
    }

    Write-Host "32-bit ABI capabilities ($Label): $($line[0])"
}

function Invoke-WindowsTreapDiagnostics {
    param(
        [string]$BenchmarkPath
    )

    $oldDiagnostics = $env:UHMAS_BENCH_DIAGNOSTICS
    $oldTreap = $env:UHMAS_BENCH_TREAP_ADVERSARIAL
    try {
        $env:UHMAS_BENCH_DIAGNOSTICS = "1"
        $env:UHMAS_BENCH_TREAP_ADVERSARIAL = "1"
        $output = & $BenchmarkPath 2>&1
        if ($LASTEXITCODE -ne 0) {
            $output | Write-Host
            throw "$BenchmarkPath exited with $LASTEXITCODE"
        }
    } finally {
        $env:UHMAS_BENCH_DIAGNOSTICS = $oldDiagnostics
        $env:UHMAS_BENCH_TREAP_ADVERSARIAL = $oldTreap
    }
    Assert-32BitAbiCapabilities $output "Win32 treap diagnostics" "windows"

    $requiredTreapResults = @(
        "treap malloc free-tree pressure",
        "treap same-size malloc free-tree pressure",
        "treap malloc free-tree remove pressure",
        "treap malloc free-tree two-child remove pressure",
        "treap vma live-tree pressure",
        "treap fixed sparse VMA build pressure",
        "treap fixed sparse VMA protect pressure",
        "treap mmap free-tree pressure"
    )
    foreach ($name in $requiredTreapResults) {
        if (-not @($output | Where-Object { $_ -like "$name*" })) {
            $output | Write-Host
            throw "treap diagnostics did not emit '$name'"
        }
    }

    $diagLines = @($output | Where-Object { $_ -like "diag|treap *" })
    $requiredTreapDiagnostics = @(
        "treap malloc free-tree pressure",
        "treap same-size malloc free-tree pressure",
        "treap malloc free-tree remove pressure",
        "treap malloc free-tree two-child remove pressure",
        "treap vma live-tree pressure",
        "treap fixed sparse VMA build pressure",
        "treap mmap free-tree pressure"
    )
    if ($diagLines.Count -lt $requiredTreapDiagnostics.Count) {
        $output | Write-Host
        throw "treap diagnostics did not emit all focused 32-bit cases"
    }
    foreach ($name in $requiredTreapDiagnostics) {
        if (-not @($diagLines | Where-Object { $_ -like "diag|$name|*" })) {
            $diagLines | Write-Host
            throw "treap diagnostics did not attribute '$name'"
        }
    }

    $badLines = @($diagLines | Where-Object {
            $_ -match "(priority_ties|depth_limit_exceeded|insert_budget_exceeded|insert_rotation_budget_exceeded|find_budget_exceeded|remove_budget_exceeded)=[1-9][0-9]*"
        })
    if ($badLines.Count -ne 0) {
        $badLines | Write-Host
        throw "32-bit treap diagnostics exceeded budget or reported priority ties"
    }

    $bucketLines = @($diagLines | Where-Object {
            $_ -match "priority_bucket_used=([4-9]|1[0-6])"
        })
    if ($bucketLines.Count -lt $requiredTreapDiagnostics.Count) {
        $diagLines | Write-Host
        throw "32-bit treap diagnostics priority bucket coverage is too narrow"
    }

    $requiredPositiveMetrics = @(
        @{ Name = "treap malloc free-tree pressure"; Key = "treap_priority_mix_ns" },
        @{ Name = "treap malloc free-tree pressure"; Key = "live_tree_priority_mix_ns" },
        @{ Name = "treap malloc free-tree pressure"; Key = "malloc_tree_priority_mix_ns" },
        @{ Name = "treap malloc free-tree pressure"; Key = "live_tree_remove_sink_steps" },
        @{ Name = "treap malloc free-tree pressure"; Key = "malloc_tree_remove_sink_steps" },
        @{ Name = "treap same-size malloc free-tree pressure"; Key = "treap_priority_mix_ns" },
        @{ Name = "treap same-size malloc free-tree pressure"; Key = "live_tree_priority_mix_ns" },
        @{ Name = "treap same-size malloc free-tree pressure"; Key = "malloc_tree_priority_mix_ns" },
        @{ Name = "treap same-size malloc free-tree pressure"; Key = "live_tree_remove_sink_steps" },
        @{ Name = "treap same-size malloc free-tree pressure"; Key = "malloc_tree_remove_calls" },
        @{ Name = "treap vma live-tree pressure"; Key = "treap_priority_mix_ns" },
        @{ Name = "treap vma live-tree pressure"; Key = "vma_tree_priority_mix_ns" },
        @{ Name = "treap fixed sparse VMA build pressure"; Key = "treap_priority_mix_ns" },
        @{ Name = "treap fixed sparse VMA build pressure"; Key = "vma_tree_priority_mix_ns" },
        @{ Name = "treap fixed sparse VMA build pressure"; Key = "mmap_free_tree_priority_mix_ns" },
        @{ Name = "treap fixed sparse VMA build pressure"; Key = "vma_tree_insertions" },
        @{ Name = "treap fixed sparse VMA build pressure"; Key = "mmap_free_tree_remove_calls" },
        @{ Name = "treap mmap free-tree pressure"; Key = "treap_priority_mix_ns" },
        @{ Name = "treap mmap free-tree pressure"; Key = "vma_tree_priority_mix_ns" },
        @{ Name = "treap mmap free-tree pressure"; Key = "mmap_free_tree_priority_mix_ns" },
        @{ Name = "treap mmap free-tree pressure"; Key = "vma_tree_remove_sink_steps" },
        @{ Name = "treap mmap free-tree pressure"; Key = "mmap_free_tree_remove_sink_steps" }
    )
    foreach ($metric in $requiredPositiveMetrics) {
        $line = @($diagLines | Where-Object { $_ -like "diag|$($metric.Name)|*" } | Select-Object -First 1)
        if ($line.Count -eq 0 -or
            $line[0] -notmatch "$([regex]::Escape($metric.Key))=([1-9][0-9]*)") {
            $diagLines | Write-Host
            throw "32-bit treap diagnostics missing positive $($metric.Key) for '$($metric.Name)'"
        }
    }
}

function Invoke-Windows32BitGateBenchmark {
    param(
        [string]$BenchmarkPath
    )

    $oldGate = $env:UHMAS_BENCH_32BIT_GATE
    try {
        $env:UHMAS_BENCH_32BIT_GATE = "1"
        $output = & $BenchmarkPath 2>&1
        if ($LASTEXITCODE -ne 0) {
            $output | Write-Host
            throw "$BenchmarkPath exited with $LASTEXITCODE"
        }
    } finally {
        $env:UHMAS_BENCH_32BIT_GATE = $oldGate
    }
    Assert-32BitAbiCapabilities $output "Win32 32-bit benchmark slice" "windows"

    foreach ($name in @(
            "uhm 32-bit locked partial burst",
            "uhm 32-bit low-lock partial burst",
            "false-share remote free/reuse",
            "uhm thread ctx cache reuse",
            "uhm TLS cache reuse",
            "uhm mprotect split/merge cost",
            "uhm mremap resize cost",
            "uhm mmap churn",
            "uhm malloc/mmap collision pressure"
        )) {
        if (-not @($output | Where-Object { $_ -like "$name*" })) {
            $output | Write-Host
            throw "32-bit gate benchmark did not emit '$name'"
        }
    }
}

function Invoke-Windows32BitFocusedBenchmarks {
    param(
        [string]$BenchmarkPath
    )

    $oldDiagnostics = $env:UHMAS_BENCH_DIAGNOSTICS
    $oldSlabEmptyChurn = $env:UHMAS_BENCH_SLAB_EMPTY_CHURN
    $oldMmapLocality = $env:UHMAS_BENCH_MMAP_LOCALITY
    try {
        $env:UHMAS_BENCH_DIAGNOSTICS = "1"
        $env:UHMAS_BENCH_SLAB_EMPTY_CHURN = "1"
        $env:UHMAS_BENCH_MMAP_LOCALITY = $null
        $slabOutput = & $BenchmarkPath 2>&1
        if ($LASTEXITCODE -ne 0) {
            $slabOutput | Write-Host
            throw "$BenchmarkPath slab-empty churn exited with $LASTEXITCODE"
        }

        $env:UHMAS_BENCH_SLAB_EMPTY_CHURN = $null
        $env:UHMAS_BENCH_MMAP_LOCALITY = "1"
        $mmapOutput = & $BenchmarkPath 2>&1
        if ($LASTEXITCODE -ne 0) {
            $mmapOutput | Write-Host
            throw "$BenchmarkPath mmap-locality exited with $LASTEXITCODE"
        }
    } finally {
        $env:UHMAS_BENCH_DIAGNOSTICS = $oldDiagnostics
        $env:UHMAS_BENCH_SLAB_EMPTY_CHURN = $oldSlabEmptyChurn
        $env:UHMAS_BENCH_MMAP_LOCALITY = $oldMmapLocality
    }

    foreach ($name in @(
            "slab empty adaptive churn 64B",
            "slab empty adaptive churn 512B",
            "slab empty full-drain batch 512B"
        )) {
        if (-not @($slabOutput | Where-Object { $_ -like "$name*" })) {
            $slabOutput | Write-Host
            throw "32-bit focused benchmark did not emit '$name'"
        }
    }
    $slabDiagLines = @($slabOutput | Where-Object {
            $_ -like "diag|slab empty adaptive churn *"
        })
    if ($slabDiagLines.Count -lt 2) {
        $slabOutput | Write-Host
        throw "32-bit slab-empty focused diagnostics missing result lines"
    }
    $badSlab = @($slabDiagLines | Where-Object {
            $_ -match "(slab_empty_decommit_pressure_events|slab_empty_committed_cache_adaptive_grow_events|decommit_events|recommit_events|decommit_recommit_same_addr_events)=([2-9]|[1-9][0-9]+)" -or
            $_ -match "(vm_lock_held_calls|vm_commit_lock_held_calls|vm_decommit_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|metadata_quarantine_drain_budget_exhausted)=[1-9][0-9]*"
        })
    if ($badSlab.Count -ne 0) {
        $badSlab | Write-Host
        throw "32-bit slab-empty focused diagnostics exceeded threshold"
    }
    $batchDiagLines = @($slabOutput | Where-Object {
            $_ -like "diag|slab empty full-drain batch 512B|*"
        })
    if ($batchDiagLines.Count -ne 1) {
        $slabOutput | Write-Host
        throw "32-bit slab-empty full-drain diagnostics missing result line"
    }
    $batchDiag = [string]$batchDiagLines[0]
    foreach ($pattern in @(
            "slab_empty_decommit_pressure_events=108( |$)",
            "slab_empty_committed_cache_adaptive_grow_events=0( |$)",
            "decommit_events=108( |$)",
            "recommit_events=0( |$)",
            "decommit_recommit_same_addr_events=0( |$)",
            "page_mark_calls=108( |$)",
            "page_mark_pages=108( |$)",
            "vm_commit_calls=0( |$)",
            "vm_decommit_calls=9( |$)",
            "vm_decommit_failures=0( |$)",
            "vm_lock_held_calls=0( |$)",
            "vm_decommit_lock_held_calls=0( |$)"
        )) {
        if ($batchDiag -notmatch $pattern) {
            $batchDiag | Write-Host
            throw "32-bit slab-empty full-drain diagnostic invariant failed: $pattern"
        }
    }
    $batchAttributionLines = @($slabOutput | Where-Object {
            $_ -like "metadata attribution slab empty full-drain batch 512B:*"
        })
    if ($batchAttributionLines.Count -ne 1 -or
        [string]$batchAttributionLines[0] -notmatch
            "operations=9 .*vm_calls=9 vm_failures=0 .*vm_lock_held_calls=0 .*vm_commit_calls=0 vm_decommit_calls=9 .*decommit_events=108 .*recommit_events=0 .*same_addr_events=0 .*page_mark_pages=108 ") {
        $slabOutput | Write-Host
        throw "32-bit slab-empty full-drain attribution invariant failed"
    }
    $batchLatencyLines = @($slabOutput | Where-Object {
            $_ -like "latency|slab empty full-drain batch 512B|*"
        })
    if ($batchLatencyLines.Count -ne 1 -or
        [string]$batchLatencyLines[0] -notmatch
            "samples=9 .*p95_ns=[1-9][0-9]* .*p99_ns=[1-9][0-9]* .*work_units_per_sample=12 .*pages_per_sample=12 ") {
        $slabOutput | Write-Host
        throw "32-bit slab-empty full-drain latency metadata missing"
    }

    foreach ($name in @(
            "mmap locality size-hint churn 2p",
            "mmap locality size-hint churn 4p",
            "mmap locality size-hint churn 8p",
            "mmap locality same-bucket hint churn",
            "mmap locality deep-bucket hint churn",
            "mmap locality fragmented-ring hint churn",
            "mmap locality exact-front hot-cache metadata",
            "mmap locality sparse hot-cache maintenance",
            "mmap locality adjacent-release recent lookup"
        )) {
        if (-not @($mmapOutput | Where-Object { $_ -like "$name*" })) {
            $mmapOutput | Write-Host
            throw "32-bit focused benchmark did not emit '$name'"
        }
    }
    $mmapMetadataLines = @($mmapOutput | Where-Object {
            $_ -like "metadata mmap locality *"
        })
    $mmapSparseMetadataLine = @($mmapMetadataLines | Where-Object {
            $_ -like "metadata mmap locality sparse hot-cache maintenance:*"
        })
    if ($mmapSparseMetadataLine.Count -lt 1 -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_samples=[1-9][0-9]*" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_exact_pages=1" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_recent_cache_slots=16" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_size_hint_slots=8" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_range_free_extents_sum=1" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_range_free_pages_sum=1" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_range_largest_pages_max=1" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_range_fragmented_pages_sum=0" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_range_fragmentation_ppm_max=0" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_exact_reuse_hits=[1-9][0-9]*" -or
        $mmapSparseMetadataLine[0] -notmatch "mmap_locality_reuse_distance_pages_max=0") {
        $mmapSparseMetadataLine | Write-Host
        throw "32-bit mmap-locality sparse metadata failed invariant check"
    }
    $mmapAdjacentMetadataLine = @($mmapMetadataLines | Where-Object {
            $_ -like "metadata mmap locality adjacent-release recent lookup:*"
        })
    if ($mmapAdjacentMetadataLine.Count -lt 1 -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_samples=[1-9][0-9]*" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_exact_pages=8" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_recent_cache_slots=16" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_size_hint_slots=8" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_range_free_extents_sum=72" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_range_free_pages_sum=72" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_range_largest_pages_max=1" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_range_fragmented_pages_sum=71" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_range_fragmentation_ppm_max=986111" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_exact_reuse_hits=[1-9][0-9]*" -or
        $mmapAdjacentMetadataLine[0] -notmatch "mmap_locality_reuse_distance_pages_max=0") {
        $mmapAdjacentMetadataLine | Write-Host
        throw "32-bit mmap-locality adjacent-release metadata failed invariant check"
    }
    foreach ($entry in @(
            @{ Name = "mmap locality size-hint churn 2p"; ExactPages = 2 },
            @{ Name = "mmap locality size-hint churn 4p"; ExactPages = 4 },
            @{ Name = "mmap locality size-hint churn 8p"; ExactPages = 8 },
            @{ Name = "mmap locality same-bucket hint churn"; ExactPages = 4 },
            @{ Name = "mmap locality deep-bucket hint churn"; ExactPages = 4 },
            @{ Name = "mmap locality fragmented-ring hint churn"; ExactPages = 4 },
            @{ Name = "mmap locality exact-front hot-cache metadata"; ExactPages = 2 },
            @{ Name = "mmap locality adjacent-release recent lookup"; ExactPages = 8 }
        )) {
        $line = @($mmapMetadataLines | Where-Object {
                $_ -like "metadata $($entry.Name):*"
            })
        if ($line.Count -lt 1) {
            $mmapOutput | Write-Host
            throw "32-bit mmap-locality metadata missing '$($entry.Name)'"
        }
        if ($line[0] -notmatch "mmap_locality_samples=[1-9][0-9]*" -or
            $line[0] -notmatch "mmap_locality_exact_pages=$($entry.ExactPages)" -or
            $line[0] -notmatch "mmap_locality_recent_cache_slots=16" -or
            $line[0] -notmatch "mmap_locality_size_hint_slots=8" -or
            $line[0] -notmatch "mmap_locality_range_fragmented_pages_sum=[1-9][0-9]*" -or
            $line[0] -notmatch "mmap_locality_range_fragmentation_ppm_max=[1-9][0-9]*" -or
            $line[0] -notmatch "mmap_locality_exact_reuse_hits=[1-9][0-9]*" -or
            $line[0] -notmatch "mmap_locality_reuse_distance_pages_max=0") {
            $line | Write-Host
            throw "32-bit mmap-locality metadata failed invariant check"
        }
    }
    $mmapDiagLines = @($mmapOutput | Where-Object {
            $_ -like "diag|mmap locality size-hint churn*"
    })
    $mmapSameBucketDiagLines = @($mmapOutput | Where-Object {
            $_ -like "diag|mmap locality same-bucket hint churn*"
        })
    $mmapDeepBucketDiagLines = @($mmapOutput | Where-Object {
            $_ -like "diag|mmap locality deep-bucket hint churn*"
        })
    $mmapFragmentedRingDiagLines = @($mmapOutput | Where-Object {
            $_ -like "diag|mmap locality fragmented-ring hint churn*"
        })
    $mmapExactFrontDiagLines = @($mmapOutput | Where-Object {
            $_ -like "diag|mmap locality exact-front hot-cache metadata*"
        })
    $mmapSparseDiagLines = @($mmapOutput | Where-Object {
            $_ -like "diag|mmap locality sparse hot-cache maintenance*"
        })
    $mmapAdjacentDiagLines = @($mmapOutput | Where-Object {
            $_ -like "diag|mmap locality adjacent-release recent lookup*"
        })
    if ($mmapDiagLines.Count -lt 3 -or $mmapSameBucketDiagLines.Count -lt 1 -or
        $mmapDeepBucketDiagLines.Count -lt 1 -or
        $mmapFragmentedRingDiagLines.Count -lt 1 -or
        $mmapExactFrontDiagLines.Count -lt 1 -or
        $mmapSparseDiagLines.Count -lt 1 -or
        $mmapAdjacentDiagLines.Count -lt 1) {
        $mmapOutput | Write-Host
        throw "32-bit mmap-locality focused diagnostics missing result lines"
    }
    $badMmap = @($mmapDiagLines | Where-Object {
            $_ -match "(mmap_size_hint_misses|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*" -or
            $_ -match "mmap_free_tree_find_calls=([2-9]|[1-9][0-9]+)"
        })
    $badMmap += @($mmapSameBucketDiagLines | Where-Object {
            $_ -match "(mmap_size_hint_misses|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*" -or
            $_ -match "mmap_free_tree_find_calls=([2-9]|[1-9][0-9]+)"
        })
    $badMmap += @($mmapDeepBucketDiagLines | Where-Object {
            $_ -match "(mmap_size_hint_misses|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*" -or
            $_ -match "mmap_free_tree_find_calls=([2-9]|[1-9][0-9]+)"
        })
    $badMmap += @($mmapFragmentedRingDiagLines | Where-Object {
            $_ -match "(mmap_size_hint_misses|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*" -or
            $_ -match "mmap_free_tree_find_calls=([2-9]|[1-9][0-9]+)"
        })
    $badMmap += @($mmapExactFrontDiagLines | Where-Object {
            $_ -match "(mmap_recent_free_too_small|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*" -or
            $_ -match "mmap_free_tree_find_calls=([2-9]|[1-9][0-9]+)"
        })
    $badMmap += @($mmapSparseDiagLines | Where-Object {
            $_ -match "(mmap_recent_free_too_small|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*" -or
            $_ -match "mmap_free_tree_find_calls=([2-9]|[1-9][0-9]+)"
        })
    $badMmap += @($mmapAdjacentDiagLines | Where-Object {
            $_ -match "(mmap_recent_free_too_small|mmap_size_hint_too_small|vm_lock_held_calls|page_mark_budget_exceeded|page_range_query_budget_exceeded|page_lock_range_budget_exceeded|mmap_free_tree_find_budget_exceeded|mmap_free_tree_insert_budget_exceeded|mmap_free_tree_remove_budget_exceeded)=[1-9][0-9]*" -or
            $_ -match "mmap_free_tree_find_calls=([2-9]|[1-9][0-9]+)"
        })
    if ($badMmap.Count -ne 0) {
        $badMmap | Write-Host
        throw "32-bit mmap-locality focused diagnostics exceeded threshold"
    }
}

function Invoke-32BitDiagnosticThresholdGate {
    param(
        [ValidateSet("win32", "wsl-i386")]
        [string]$Platform
    )

    $scriptPath = Join-Path $PSScriptRoot "run_benchmarks.ps1"
    $args = @(
        "-Platform", $Platform,
        "-Gate32Bit",
        "-Diagnostics",
        "-SkipBuild",
        "-Samples", "1",
        "-AllowMissingBaseline",
        "-DiagnosticThresholdConfigPath", "scripts/diagnostic-thresholds-32bit-gate.json",
        "-OutputDir", "artifacts/benchmarks",
        "-TimeoutSeconds", "240"
    )
    if ($WslProject.Length -gt 0) {
        $args += @("-WslProject", $WslProject)
    }
    if ($WslDistro.Length -gt 0) {
        $args += @("-WslDistro", $WslDistro)
    }

    & powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath @args
    if ($LASTEXITCODE -ne 0) {
        throw "32-bit diagnostic threshold gate failed for $Platform"
    }
}

function Invoke-32BitMetadataFootprintGate {
    param(
        [ValidateSet("win32", "wsl-i386")]
        [string]$Platform
    )

    $scriptPath = Join-Path $PSScriptRoot "run_benchmarks.ps1"
    $args = @(
        "-Platform", $Platform,
        "-MetadataFootprint",
        "-SkipBuild",
        "-Samples", "1",
        "-AllowMissingBaseline",
        "-MetadataThresholdConfigPath", "scripts/metadata-thresholds-metadata-footprint.json",
        "-OutputDir", "artifacts/benchmarks",
        "-TimeoutSeconds", "240"
    )
    if ($WslProject.Length -gt 0) {
        $args += @("-WslProject", $WslProject)
    }
    if ($WslDistro.Length -gt 0) {
        $args += @("-WslDistro", $WslDistro)
    }

    & powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath @args
    if ($LASTEXITCODE -ne 0) {
        throw "32-bit metadata footprint gate failed for $Platform"
    }
}

function Invoke-32BitSlabSlotAbGate {
    param(
        [ValidateSet("win32", "wsl-i386")]
        [string]$Platform
    )

    $scriptPath = Join-Path $PSScriptRoot "run_benchmarks.ps1"
    $args = @(
        "-Platform", $Platform,
        "-SlabSlotAb",
        "-Diagnostics",
        "-SkipBuild",
        "-Samples", "1",
        "-AllowMissingBaseline",
        "-MetadataThresholdConfigPath", "scripts/metadata-thresholds-slab-slot-ab.json",
        "-OutputDir", "artifacts/benchmarks",
        "-TimeoutSeconds", "240"
    )
    if ($WslProject.Length -gt 0) {
        $args += @("-WslProject", $WslProject)
    }
    if ($WslDistro.Length -gt 0) {
        $args += @("-WslDistro", $WslDistro)
    }

    & powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath @args
    if ($LASTEXITCODE -ne 0) {
        throw "32-bit slab-slot A/B gate failed for $Platform"
    }
}

function Invoke-32BitReleaseBreakdownGate {
    param(
        [ValidateSet("win32", "wsl-i386")]
        [string]$Platform
    )

    $scriptPath = Join-Path $PSScriptRoot "run_benchmarks.ps1"
    $args = @(
        "-Platform", $Platform,
        "-ReleaseBreakdown",
        "-Diagnostics",
        "-SkipBuild",
        "-Samples", "1",
        "-AllowMissingBaseline",
        "-DiagnosticThresholdConfigPath", "scripts/diagnostic-thresholds-release-breakdown.json",
        "-LatencyThresholdConfigPath", "scripts/latency-thresholds-release-breakdown.json",
        "-MetadataThresholdConfigPath", "scripts/metadata-thresholds-release-breakdown.json",
        "-OutputDir", "artifacts/benchmarks",
        "-TimeoutSeconds", "300"
    )
    if ($WslProject.Length -gt 0) {
        $args += @("-WslProject", $WslProject)
    }
    if ($WslDistro.Length -gt 0) {
        $args += @("-WslDistro", $WslDistro)
    }

    & powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath @args
    if ($LASTEXITCODE -ne 0) {
        throw "32-bit release-breakdown gate failed for $Platform"
    }
}

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
if (-not $SkipWsl) {
    . (Join-Path $PSScriptRoot "wsl_project_path.ps1")
    $WslProject = Resolve-UhmWslProjectPath $WslProject $WslDistro $Root
}

if (-not $SkipWindows) {
    $VsGenerator = "Visual Studio 17 2022"
    $Win32Build = Join-Path $Root "build-win32-msvc"

    Invoke-Step "Configure Win32 MSVC" {
        Invoke-Native "cmake" @(
            "-S", $Root,
            "-B", $Win32Build,
            "-G", $VsGenerator,
            "-A", "Win32",
            "-DUHMAS_BUILD_TESTS=ON",
            "-DUHMAS_BUILD_BENCHMARKS=ON",
            "-DUHMAS_EXPECT_32BIT=ON"
        )
    }
    Invoke-Step "Build Win32 Debug" {
        Invoke-Native "cmake" @("--build", $Win32Build, "--config", "Debug")
    }
    Invoke-Step "Run Win32 Debug tests" {
        Invoke-Native (Join-Path $Win32Build "Debug\uhmas_basic_tests.exe") @()
        Invoke-Native (Join-Path $Win32Build "Debug\uhmas_platform_vm_tests.exe") @()
    }
    Invoke-Step "Build Win32 Release" {
        Invoke-Native "cmake" @("--build", $Win32Build, "--config", "Release")
    }
    Invoke-Step "Run Win32 Release tests" {
        Invoke-Native (Join-Path $Win32Build "Release\uhmas_basic_tests.exe") @()
        Invoke-Native (Join-Path $Win32Build "Release\uhmas_platform_vm_tests.exe") @()
    }
    Invoke-Step "Run Win32 Release treap diagnostics" {
        Invoke-WindowsTreapDiagnostics (Join-Path $Win32Build "Release\uhmas_benchmarks.exe")
    }
    Invoke-Step "Run Win32 Release 32-bit benchmark slice" {
        Invoke-Windows32BitGateBenchmark (Join-Path $Win32Build "Release\uhmas_benchmarks.exe")
    }
    Invoke-Step "Run Win32 Release focused diagnostics" {
        Invoke-Windows32BitFocusedBenchmarks (Join-Path $Win32Build "Release\uhmas_benchmarks.exe")
    }
    Invoke-Step "Run Win32 Release 32-bit diagnostic threshold gate" {
        Invoke-32BitDiagnosticThresholdGate "win32"
    }
    Invoke-Step "Run Win32 Release release-breakdown gate" {
        Invoke-32BitReleaseBreakdownGate "win32"
    }
    Invoke-Step "Run Win32 Release metadata footprint gate" {
        Invoke-32BitMetadataFootprintGate "win32"
    }
    Invoke-Step "Run Win32 Release slab-slot A/B gate" {
        Invoke-32BitSlabSlotAbGate "win32"
    }
}

if (-not $SkipWsl) {
    $WslPrefix = @()
    if ($WslDistro.Length -gt 0) {
        $WslPrefix = @("-d", $WslDistro)
    }
    $Script = @"
set -eu
cd "$WslProject"
cmake -S . -B build-wsl-i386 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 -DUHMAS_BUILD_TESTS=ON -DUHMAS_BUILD_BENCHMARKS=ON -DUHMAS_EXPECT_32BIT=ON
cmake --build build-wsl-i386
(cd build-wsl-i386 && ./uhmas_basic_tests && ./uhmas_platform_vm_tests)
cmake -S . -B build-wsl-i386-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 -DUHMAS_BUILD_TESTS=ON -DUHMAS_BUILD_BENCHMARKS=ON -DUHMAS_EXPECT_32BIT=ON
cmake --build build-wsl-i386-release
(cd build-wsl-i386-release && ./uhmas_basic_tests && ./uhmas_platform_vm_tests)
tr -d '\r' < "$WslProject/scripts/verify_wsl_i386_treap.sh" | bash -s "$WslProject/build-wsl-i386-release"
tr -d '\r' < "$WslProject/scripts/verify_wsl_i386_gate_bench.sh" | bash -s "$WslProject/build-wsl-i386-release"
"@
    $Script = $Script -replace "`r", ""
    Invoke-Step "Run WSL i386 Debug/Release tests" {
        Invoke-Native "wsl" ($WslPrefix + @("bash", "-lc", $Script))
    }
    Invoke-Step "Run WSL i386 32-bit diagnostic threshold gate" {
        Invoke-32BitDiagnosticThresholdGate "wsl-i386"
    }
    Invoke-Step "Run WSL i386 release-breakdown gate" {
        Invoke-32BitReleaseBreakdownGate "wsl-i386"
    }
    Invoke-Step "Run WSL i386 metadata footprint gate" {
        Invoke-32BitMetadataFootprintGate "wsl-i386"
    }
    Invoke-Step "Run WSL i386 slab-slot A/B gate" {
        Invoke-32BitSlabSlotAbGate "wsl-i386"
    }
}

Write-Host "32-bit gate passed."
