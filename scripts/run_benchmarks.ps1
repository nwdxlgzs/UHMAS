param(
    [ValidateSet("windows", "wsl", "both", "win32", "wsl-i386", "32bit-both")]
    [string]$Platform = "windows",
    [string]$BuildDir = "build-release",
    [string]$WslBuildDir = "build-wsl-release",
    [string]$WslDistro = "",
    [string]$WslProject = "",
    [string]$Configuration = "Release",
    [string]$OutputDir = "artifacts/benchmarks",
    [string]$BaselinePath = "",
    [string]$ThresholdConfigPath = "",
    [string]$DiagnosticThresholdConfigPath = "",
    [string]$LatencyThresholdConfigPath = "",
    [string]$MetadataThresholdConfigPath = "",
    [string]$ProfileNameOverride = "",
    [double]$RegressionThresholdPercent = 15.0,
    [int]$Samples = 1,
    [ValidateSet("median", "min")]
    [string]$SampleAggregation = "median",
    [int]$TimeoutSeconds = 300,
    [switch]$Stress,
    [switch]$FalseSharing,
    [switch]$SmallFastPath,
    [switch]$ReleaseBreakdown,
    [switch]$ReleasePressureMatrix,
    [switch]$SlabEmptyChurn,
    [switch]$HeapClose,
    [switch]$MmapLocality,
    [switch]$ControlAllocatorChurn,
    [switch]$LiveBlockLookup,
    [switch]$TreapAdversarial,
    [switch]$MprotectMultiVma,
    [switch]$MunmapMultiVma,
    [switch]$SpinlockThresholds,
    [switch]$MetadataFootprint,
    [switch]$SlabSlotSharing,
    [switch]$SlabSlotAb,
    [switch]$Gate32Bit,
    [int]$MprotectStressPages = 0,
    [int]$MprotectLatencySamples = 0,
    [switch]$Diagnostics,
    [ValidateSet("on", "off")]
    [string]$DiagnosticsTiming = "on",
    [switch]$SkipBuild,
    [switch]$NoRegressionBaseline,
    [switch]$WriteBaseline,
    [switch]$AllowMissingBaseline,
    [switch]$RequireBaselineProvenance,
    [switch]$ProvenanceSelfTest
)

$ErrorActionPreference = "Stop"
$BenchmarkProvenanceSchema = 3
$BenchmarkWorkloadContractSchema = 1

if ($NoRegressionBaseline -and
    ($BaselinePath.Length -gt 0 -or $WriteBaseline -or $AllowMissingBaseline)) {
    throw "NoRegressionBaseline cannot be combined with BaselinePath, WriteBaseline, or AllowMissingBaseline."
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Body
    )
    Write-Host "==> $Name"
    & $Body
}

function Invoke-NativeChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$File,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File exited with $LASTEXITCODE"
    }
}

function ConvertTo-ProcessArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }

    $result = '"'
    $backslashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') {
            $backslashes += 1
            continue
        }
        if ($ch -eq '"') {
            $result += ('\' * (($backslashes * 2) + 1))
            $result += '"'
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            $result += ('\' * $backslashes)
            $backslashes = 0
        }
        $result += $ch
    }
    if ($backslashes -gt 0) {
        $result += ('\' * ($backslashes * 2))
    }
    $result += '"'
    return $result
}

function Join-ProcessArguments {
    param([AllowEmptyCollection()][string[]]$Arguments)

    return (($Arguments | ForEach-Object { ConvertTo-ProcessArgument $_ }) -join " ")
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$File,
        [AllowEmptyCollection()]
        [string[]]$Arguments,
        [hashtable]$Environment = @{},
        [int]$TimeoutSeconds = 300
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $File
    $psi.Arguments = Join-ProcessArguments $Arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($key in $Environment.Keys) {
        $psi.Environment[$key] = [string]$Environment[$key]
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    [void]$process.Start()

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        try {
            $process.Kill($true)
        } catch {
            $process.Kill()
        }
        throw "$File timed out after $TimeoutSeconds seconds"
    }

    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdoutTask.Result
        Stderr = $stderrTask.Result
        Command = ($File + " " + ($Arguments -join " "))
    }
}

function ConvertTo-ShSingleQuoted {
    param([Parameter(Mandatory = $true)][string]$Value)
    return "'" + ($Value -replace "'", "'\''") + "'"
}

function Get-ProfileName {
    param([switch]$Stress, [switch]$FalseSharing, [switch]$SmallFastPath, [switch]$ReleaseBreakdown, [switch]$ReleasePressureMatrix, [switch]$SlabEmptyChurn, [switch]$HeapClose, [switch]$MmapLocality, [switch]$ControlAllocatorChurn, [switch]$LiveBlockLookup, [switch]$TreapAdversarial, [switch]$MprotectMultiVma, [switch]$MunmapMultiVma, [switch]$SpinlockThresholds, [switch]$MetadataFootprint, [switch]$SlabSlotSharing, [switch]$SlabSlotAb, [switch]$Gate32Bit, [int]$MprotectStressPages = 0, [int]$MprotectLatencySamples = 0)
    $parts = @()
    if ($FalseSharing) {
        $parts += "false-sharing"
    }
    if ($SmallFastPath) {
        $parts += "small-fast-path"
    }
    if ($ReleaseBreakdown) {
        $parts += "release-breakdown"
    }
    if ($ReleasePressureMatrix) {
        $parts += "release-pressure-matrix"
    }
    if ($SlabEmptyChurn) {
        $parts += "slab-empty-churn"
    }
    if ($HeapClose) {
        $parts += "heap-close"
    }
    if ($MmapLocality) {
        $parts += "mmap-locality"
    }
    if ($ControlAllocatorChurn) {
        $parts += "control-allocator-churn"
    }
    if ($LiveBlockLookup) {
        $parts += "live-block"
    }
    if ($TreapAdversarial) {
        $parts += "treap-adversarial"
    }
    if ($MprotectMultiVma) {
        if ($MprotectStressPages -gt 0) {
            $mprotectPart = "mprotect-multi-vma-p$MprotectStressPages"
            if ($MprotectLatencySamples -gt 0) {
                $mprotectPart += "-lat$MprotectLatencySamples"
            }
            $parts += $mprotectPart
        } else {
            $parts += "mprotect-multi-vma"
        }
    }
    if ($MunmapMultiVma) {
        $parts += "munmap-multi-vma"
    }
    if ($SpinlockThresholds) {
        $parts += "spinlock-thresholds"
    }
    if ($MetadataFootprint) {
        $parts += "metadata-footprint"
    }
    if ($SlabSlotSharing) {
        $parts += "slab-slot-sharing"
    }
    if ($SlabSlotAb) {
        $parts += "slab-slot-ab"
    }
    if ($Gate32Bit) {
        $parts += "32bit-gate"
    }
    if ($Stress) {
        $parts += "stress"
    } else {
        $parts += "quick"
    }
    return ($parts -join "-")
}

function Get-BenchmarkEnvironment {
    param([switch]$Stress, [switch]$FalseSharing, [switch]$SmallFastPath, [switch]$ReleaseBreakdown, [switch]$ReleasePressureMatrix, [switch]$SlabEmptyChurn, [switch]$HeapClose, [switch]$MmapLocality, [switch]$ControlAllocatorChurn, [switch]$LiveBlockLookup, [switch]$TreapAdversarial, [switch]$MprotectMultiVma, [switch]$MunmapMultiVma, [switch]$SpinlockThresholds, [switch]$MetadataFootprint, [switch]$SlabSlotSharing, [switch]$SlabSlotAb, [switch]$Gate32Bit, [int]$MprotectStressPages = 0, [int]$MprotectLatencySamples = 0, [switch]$Diagnostics, [string]$DiagnosticsTiming = "on")
    $env = @{}
    if ($Stress) {
        $env["UHMAS_BENCH_STRESS"] = "1"
    }
    if ($FalseSharing) {
        $env["UHMAS_BENCH_FALSE_SHARING"] = "1"
    }
    if ($SmallFastPath) {
        $env["UHMAS_BENCH_SMALL_FAST_PATH"] = "1"
    }
    if ($ReleaseBreakdown) {
        $env["UHMAS_BENCH_RELEASE_BREAKDOWN"] = "1"
    }
    if ($ReleasePressureMatrix) {
        $env["UHMAS_BENCH_RELEASE_PRESSURE_MATRIX"] = "1"
    }
    if ($SlabEmptyChurn) {
        $env["UHMAS_BENCH_SLAB_EMPTY_CHURN"] = "1"
    }
    if ($HeapClose) {
        $env["UHMAS_BENCH_HEAP_CLOSE"] = "1"
    }
    if ($MmapLocality) {
        $env["UHMAS_BENCH_MMAP_LOCALITY"] = "1"
    }
    if ($ControlAllocatorChurn) {
        $env["UHMAS_BENCH_CONTROL_ALLOCATOR_CHURN"] = "1"
    }
    if ($LiveBlockLookup) {
        $env["UHMAS_BENCH_LIVE_BLOCK_LOOKUP"] = "1"
    }
    if ($TreapAdversarial) {
        $env["UHMAS_BENCH_TREAP_ADVERSARIAL"] = "1"
    }
    if ($MprotectMultiVma) {
        $env["UHMAS_BENCH_MPROTECT_MULTI_VMA"] = "1"
    }
    if ($MunmapMultiVma) {
        $env["UHMAS_BENCH_MUNMAP_MULTI_VMA"] = "1"
    }
    if ($SpinlockThresholds) {
        $env["UHMAS_BENCH_SPINLOCK_THRESHOLDS"] = "1"
    }
    if ($MetadataFootprint) {
        $env["UHMAS_BENCH_METADATA_FOOTPRINT"] = "1"
    }
    if ($SlabSlotSharing) {
        $env["UHMAS_BENCH_SLAB_SLOT_SHARING"] = "1"
    }
    if ($SlabSlotAb) {
        $env["UHMAS_BENCH_SLAB_SLOT_AB"] = "1"
    }
    if ($Gate32Bit) {
        $env["UHMAS_BENCH_32BIT_GATE"] = "1"
    }
    if ($MprotectStressPages -gt 0) {
        $env["UHMAS_BENCH_MPROTECT_MULTI_VMA_STRESS_PAGES"] = [string]$MprotectStressPages
    }
    if ($MprotectLatencySamples -gt 0) {
        $env["UHMAS_BENCH_MPROTECT_MULTI_VMA_LATENCY_SAMPLES"] =
            [string]$MprotectLatencySamples
    }
    if ($Diagnostics) {
        $env["UHMAS_BENCH_DIAGNOSTICS"] = "1"
        $env["UHMAS_BENCH_DIAGNOSTICS_TIMING"] =
            if ($DiagnosticsTiming -eq "off") { "0" } else { "1" }
    }
    return $env
}

function Find-WindowsBenchmarkExecutable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    $candidates = @(
        (Join-Path $BuildDir "$Configuration\uhmas_benchmarks.exe"),
        (Join-Path $BuildDir "tests\$Configuration\uhmas_benchmarks.exe"),
        (Join-Path $BuildDir "uhmas_benchmarks.exe"),
        (Join-Path $BuildDir "tests\uhmas_benchmarks.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }
    throw "Unable to find uhmas_benchmarks.exe under $BuildDir"
}

function Parse-BenchmarkOutput {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Platform,
        [Parameter(Mandatory = $true)]
        [string]$Profile,
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $records = @()
    $regex = '^(?<name>.+?)\s+(?<seconds>\d+(?:\.\d+)?)\s+s\s+(?<ops>\d+)\s+ops\s+(?<ns>\d+(?:\.\d+)?)\s+ns/op\s*$'
    foreach ($line in ($Text -split "`r?`n")) {
        $match = [regex]::Match($line, $regex)
        if (-not $match.Success) {
            continue
        }
        $records += [pscustomobject]@{
            platform = $Platform
            profile = $Profile
            name = $match.Groups["name"].Value.Trim()
            seconds = [double]::Parse($match.Groups["seconds"].Value, $culture)
            operations = [int64]::Parse($match.Groups["ops"].Value, $culture)
            ns_per_op = [double]::Parse($match.Groups["ns"].Value, $culture)
            raw = $line
            command = $Command
        }
    }
    return $records
}

function Parse-BenchmarkDiagnostics {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Platform,
        [Parameter(Mandatory = $true)]
        [string]$Profile
    )

    $records = @()
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -notmatch '^diag\|(?<name>[^|]+)\|(?<pairs>.*)$') {
            continue
        }
        $record = [ordered]@{
            platform = $Platform
            profile = $Profile
            name = $Matches["name"]
            raw = $line
        }
        foreach ($pair in ($Matches["pairs"] -split '\s+')) {
            if ($pair -match '^(?<key>[A-Za-z0-9_]+)=(?<value>\d+)$') {
                $record[$Matches["key"]] = [int64]$Matches["value"]
            }
        }
        $records += [pscustomobject]$record
    }
    return $records
}

function Parse-BenchmarkLatencies {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Platform,
        [Parameter(Mandatory = $true)]
        [string]$Profile
    )

    $records = @()
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -notmatch '^latency\|(?<name>[^|]+)\|(?<pairs>.*)$') {
            continue
        }
        $record = [ordered]@{
            platform = $Platform
            profile = $Profile
            name = $Matches["name"]
            raw = $line
        }
        foreach ($pair in ($Matches["pairs"] -split '\s+')) {
            if ($pair -match '^(?<key>[A-Za-z0-9_]+)=(?<value>\d+)$') {
                $record[$Matches["key"]] = [int64]$Matches["value"]
            }
        }
        $records += [pscustomobject]$record
    }
    return $records
}

function Parse-BenchmarkMetadata {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$Platform,
        [Parameter(Mandatory = $true)]
        [string]$Profile
    )

    $records = @()
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -notmatch '^metadata\s+(?<name>[^:]+):\s*(?<pairs>.*)$') {
            continue
        }
        $record = [ordered]@{
            platform = $Platform
            profile = $Profile
            name = $Matches["name"]
            raw = $line
        }
        foreach ($pair in ($Matches["pairs"] -split '\s+')) {
            if ($pair -match '^(?<key>[A-Za-z0-9_]+)=(?<value>\d+)$') {
                $record[$Matches["key"]] = [int64]$Matches["value"]
            }
        }
        $records += [pscustomobject]$record
    }
    return $records
}

function Get-BenchmarkResultKey {
    param([Parameter(Mandatory = $true)]$Record)
    return "$($Record.platform)|$($Record.profile)|$($Record.name)"
}

function Resolve-OptionalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Path
    )

    if ($Path.Length -eq 0) {
        return ""
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $Root $Path
}

function Test-ThresholdRuleMatchValue {
    param(
        $Expected,
        [Parameter(Mandatory = $true)]
        [string]$Actual
    )

    if ($null -eq $Expected) {
        return $true
    }
    $expectedText = [string]$Expected
    if ($expectedText.Contains("*")) {
        return $Actual -like $expectedText
    }
    return $expectedText -eq $Actual
}

function Get-ThresholdForRecord {
    param(
        [Parameter(Mandatory = $true)]
        $Record,
        [double]$DefaultThresholdPercent,
        [AllowEmptyCollection()]
        [object[]]$ThresholdRules = @()
    )

    $threshold = $DefaultThresholdPercent
    foreach ($rule in $ThresholdRules) {
        if (-not (Test-ThresholdRuleMatchValue $rule.platform $Record.platform)) {
            continue
        }
        if (-not (Test-ThresholdRuleMatchValue $rule.profile $Record.profile)) {
            continue
        }
        if (-not (Test-ThresholdRuleMatchValue $rule.name $Record.name)) {
            continue
        }
        if ($null -eq $rule.threshold_percent) {
            throw "Threshold rule for $($Record.platform)/$($Record.profile)/$($Record.name) is missing threshold_percent."
        }
        $threshold = [double]$rule.threshold_percent
    }
    return $threshold
}

function Compare-BenchmarkResults {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Current,
        [Parameter(Mandatory = $true)]
        [object[]]$Baseline,
        [double]$ThresholdPercent = 15.0,
        [AllowEmptyCollection()]
        [object[]]$ThresholdRules = @()
    )

    $baselineByKey = @{}
    foreach ($record in $Baseline) {
        $baselineByKey[(Get-BenchmarkResultKey $record)] = $record
    }

    $comparisons = @()
    foreach ($record in $Current) {
        $key = Get-BenchmarkResultKey $record
        $recordThreshold = Get-ThresholdForRecord `
            -Record $record `
            -DefaultThresholdPercent $ThresholdPercent `
            -ThresholdRules $ThresholdRules
        if (-not $baselineByKey.ContainsKey($key)) {
            $comparisons += [pscustomobject]@{
                platform = $record.platform
                profile = $record.profile
                name = $record.name
                status = "missing-baseline"
                baseline_ns_per_op = $null
                current_ns_per_op = $record.ns_per_op
                delta_percent = $null
                threshold_percent = $recordThreshold
            }
            continue
        }

        $base = $baselineByKey[$key]
        $delta = $null
        $status = "ok"
        if ([double]$base.ns_per_op -le 0.0) {
            $status = "invalid-baseline"
        } else {
            $delta = (([double]$record.ns_per_op - [double]$base.ns_per_op) /
                      [double]$base.ns_per_op) * 100.0
            if ($delta -gt $recordThreshold) {
                $status = "regression"
            }
        }

        $comparisons += [pscustomobject]@{
            platform = $record.platform
            profile = $record.profile
            name = $record.name
            status = $status
            baseline_ns_per_op = [double]$base.ns_per_op
            current_ns_per_op = [double]$record.ns_per_op
            delta_percent = $delta
            threshold_percent = $recordThreshold
        }
    }
    return $comparisons
}

function New-SameRunRelativeComparison {
    param(
        [Parameter(Mandatory = $true)]$Rule,
        $Record,
        $Reference
    )

    $minRatio = if ($null -ne $Rule.min_ratio) { [double]$Rule.min_ratio } else { $null }
    $maxRatio = if ($null -ne $Rule.max_ratio) { [double]$Rule.max_ratio } else { $null }
    $status = "ok"
    $ratio = $null
    $currentNs = $null
    $baselineNs = $null
    $platform = if ($null -ne $Record) { [string]$Record.platform } else { [string]$Rule.platform }
    $profile = if ($null -ne $Record) { [string]$Record.profile } else { [string]$Rule.profile }
    $name = [string]$Rule.name
    if ($null -ne $Record) {
        $name = [string]$Record.name
        $currentNs = [double]$Record.ns_per_op
    }

    if ($null -eq $Record) {
        $status = "missing-baseline"
    } elseif ($null -eq $Reference) {
        $status = "missing-baseline"
    } elseif ([double]$Reference.ns_per_op -le 0.0) {
        $status = "invalid-baseline"
        $baselineNs = [double]$Reference.ns_per_op
    } else {
        $baselineNs = [double]$Reference.ns_per_op
        $ratio = [double]$Record.ns_per_op / [double]$Reference.ns_per_op
        if (($null -ne $maxRatio -and $ratio -gt $maxRatio) -or
            ($null -ne $minRatio -and $ratio -lt $minRatio)) {
            $status = "regression"
        }
    }

    return [pscustomobject]@{
        platform = $platform
        profile = $profile
        name = $name
        status = $status
        comparison_kind = "same-run-relative"
        baseline_name = [string]$Rule.baseline_name
        baseline_ns_per_op = $baselineNs
        current_ns_per_op = $currentNs
        ratio = $ratio
        min_ratio = $minRatio
        max_ratio = $maxRatio
        delta_percent = if ($null -ne $ratio) { ($ratio - 1.0) * 100.0 } else { $null }
        threshold_percent = if ($null -ne $maxRatio) { ($maxRatio - 1.0) * 100.0 } else { $null }
    }
}

function Compare-SameRunRelativeResults {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Current,
        [AllowEmptyCollection()]
        [object[]]$Rules = @()
    )

    if (@($Rules).Count -eq 0) {
        return @()
    }

    $recordsByKey = @{}
    foreach ($record in $Current) {
        $recordsByKey[(Get-BenchmarkResultKey $record)] = $record
    }

    $comparisons = @()
    foreach ($rule in $Rules) {
        $platform = if ($null -ne $rule.platform) { [string]$rule.platform } else { "*" }
        $profile = if ($null -ne $rule.profile) { [string]$rule.profile } else { "*" }
        $targetName = [string]$rule.name
        $referenceName = [string]$rule.baseline_name
        if ($platform.Contains("*") -or $profile.Contains("*") -or
            $targetName.Contains("*") -or $referenceName.Contains("*")) {
            $matched = $false
            foreach ($record in $Current) {
                if (-not (Test-ThresholdRuleMatchValue $platform $record.platform)) {
                    continue
                }
                if (-not (Test-ThresholdRuleMatchValue $profile $record.profile)) {
                    continue
                }
                if (-not (Test-ThresholdRuleMatchValue $targetName $record.name)) {
                    continue
                }
                $matched = $true
                $reference = Get-BenchmarkRecordByName -Records @($Current) -Name $referenceName -Platform $record.platform -Profile $record.profile
                $comparisons += New-SameRunRelativeComparison -Rule $rule -Record $record -Reference $reference
            }
            if (-not $matched) {
                $comparisons += New-SameRunRelativeComparison -Rule $rule -Record $null -Reference $null
            }
            continue
        }

        $targetKey = "$platform|$profile|$targetName"
        $referenceKey = "$platform|$profile|$referenceName"
        $target = if ($recordsByKey.ContainsKey($targetKey)) { $recordsByKey[$targetKey] } else { $null }
        $reference = if ($recordsByKey.ContainsKey($referenceKey)) { $recordsByKey[$referenceKey] } else { $null }
        $comparisons += New-SameRunRelativeComparison -Rule $rule -Record $target -Reference $reference
    }
    return $comparisons
}

function Get-FailedComparisonSortValue {
    param([Parameter(Mandatory = $true)]$Comparison)

    if ($Comparison.status -eq "regression" -and
        $null -ne $Comparison.delta_percent) {
        return [double]$Comparison.delta_percent
    }
    if ($Comparison.status -eq "invalid-baseline") {
        return [double]::PositiveInfinity
    }
    if ($Comparison.status -eq "missing-baseline") {
        return [double]::MaxValue
    }
    return [double]::NegativeInfinity
}

function New-ComparisonSummary {
    param(
        [AllowEmptyCollection()]
        [object[]]$Comparisons,
        [AllowEmptyCollection()]
        [object[]]$FailedComparisons,
        [string]$Status = "compared",
        [bool]$Skipped = $false,
        [string]$SkipReason = "",
        [bool]$BaselineMissing = $false
    )

    $statusCounts = [ordered]@{
        ok = 0
        regression = 0
        invalid_baseline = 0
        missing_baseline = 0
        other = 0
    }
    $maxRegression = $null
    foreach ($comparison in $Comparisons) {
        switch ($comparison.status) {
        "ok" { ++$statusCounts.ok; break }
        "regression" {
            ++$statusCounts.regression
            if ($null -ne $comparison.delta_percent -and
                ($null -eq $maxRegression -or
                 [double]$comparison.delta_percent -gt $maxRegression)) {
                $maxRegression = [double]$comparison.delta_percent
            }
            break
        }
        "invalid-baseline" { ++$statusCounts.invalid_baseline; break }
        "missing-baseline" { ++$statusCounts.missing_baseline; break }
        default { ++$statusCounts.other; break }
        }
    }

    return [pscustomobject]@{
        status = $Status
        skipped = $Skipped
        skip_reason = $SkipReason
        baseline_missing = $BaselineMissing
        total = @($Comparisons).Count
        failed = @($FailedComparisons).Count
        ok = $statusCounts.ok
        regression = $statusCounts.regression
        invalid_baseline = $statusCounts.invalid_baseline
        missing_baseline = $statusCounts.missing_baseline
        other = $statusCounts.other
        max_regression_percent = $maxRegression
    }
}

function Get-DiagnosticNumericKeys {
    param([Parameter(Mandatory = $true)]$Record)

    $keys = @()
    foreach ($property in $Record.PSObject.Properties) {
        if ($property.Name -eq "platform" -or
            $property.Name -eq "profile" -or
            $property.Name -eq "name" -or
            $property.Name -eq "raw" -or
            $property.Name -eq "sample_index") {
            continue
        }
        if ($property.Value -is [byte] -or
            $property.Value -is [int16] -or
            $property.Value -is [int32] -or
            $property.Value -is [int64] -or
            $property.Value -is [uint16] -or
            $property.Value -is [uint32] -or
            $property.Value -is [uint64] -or
            $property.Value -is [double]) {
            $keys += $property.Name
        }
    }
    return $keys
}

function Get-DiagnosticRuleLimit {
    param(
        $Rule,
        [Parameter(Mandatory = $true)]
        [string]$PropertyName
    )

    $property = $Rule.PSObject.Properties[$PropertyName]
    if ($null -eq $property -or $null -eq $property.Value) {
        return $null
    }
    return [int64]$property.Value
}

function Test-DiagnosticValueAgainstRule {
    param(
        [int64]$Value,
        $MinValue,
        $MaxValue
    )

    if ($null -ne $MinValue -and $Value -lt [int64]$MinValue) {
        return $false
    }
    if ($null -ne $MaxValue -and $Value -gt [int64]$MaxValue) {
        return $false
    }
    return $true
}

function Compare-DiagnosticThresholds {
    param(
        [AllowEmptyCollection()]
        [object[]]$Records,
        [AllowEmptyCollection()]
        [object[]]$Rules
    )

    $comparisons = @()
    if (@($Rules).Count -eq 0) {
        return $comparisons
    }
    if (@($Records).Count -eq 0) {
        for ($i = 0; $i -lt @($Rules).Count; ++$i) {
            $rule = $Rules[$i]
            $minValue = Get-DiagnosticRuleLimit $rule "min_value"
            $maxValue = Get-DiagnosticRuleLimit $rule "max_value"
            $comparisons += [pscustomobject]@{
                platform = if ($null -eq $rule.platform) { "*" } else { [string]$rule.platform }
                profile = if ($null -eq $rule.profile) { "*" } else { [string]$rule.profile }
                name = if ($null -eq $rule.name) { "*" } else { [string]$rule.name }
                key = if ($null -eq $rule.key) { "*" } else { [string]$rule.key }
                status = "missing-diagnostics"
                current_value = $null
                min_value = $minValue
                max_value = $maxValue
                sample_index = $null
                rule_index = $i
            }
        }
        return $comparisons
    }

    for ($ruleIndex = 0; $ruleIndex -lt @($Rules).Count; ++$ruleIndex) {
        $rule = $Rules[$ruleIndex]
        $ruleMinValue = Get-DiagnosticRuleLimit $rule "min_value"
        $ruleMaxValue = Get-DiagnosticRuleLimit $rule "max_value"
        $matchedAny = $false
        foreach ($record in $Records) {
            if (-not (Test-ThresholdRuleMatchValue $rule.platform $record.platform)) {
                continue
            }
            if (-not (Test-ThresholdRuleMatchValue $rule.profile $record.profile)) {
                continue
            }
            if (-not (Test-ThresholdRuleMatchValue $rule.name $record.name)) {
                continue
            }
            $matchedAny = $true
            $keys = if ($null -eq $rule.key -or [string]$rule.key -eq "*") {
                Get-DiagnosticNumericKeys $record
            } else {
                @([string]$rule.key)
            }
            foreach ($key in $keys) {
                $property = $record.PSObject.Properties[$key]
                if ($null -eq $property) {
                    $comparisons += [pscustomobject]@{
                        platform = $record.platform
                        profile = $record.profile
                        name = $record.name
                        key = $key
                        status = "missing-diagnostic-key"
                        current_value = $null
                        min_value = $ruleMinValue
                        max_value = $ruleMaxValue
                        sample_index = $record.sample_index
                        rule_index = $ruleIndex
                    }
                    continue
                }
                $value = [int64]$property.Value
                $comparisons += [pscustomobject]@{
                    platform = $record.platform
                    profile = $record.profile
                    name = $record.name
                    key = $key
                    status = if (Test-DiagnosticValueAgainstRule $value $ruleMinValue $ruleMaxValue) { "ok" } else { "diagnostic-threshold" }
                    current_value = $value
                    min_value = $ruleMinValue
                    max_value = $ruleMaxValue
                    sample_index = $record.sample_index
                    rule_index = $ruleIndex
                }
            }
        }
        if (-not $matchedAny) {
            $scopePresent = $false
            foreach ($record in $Records) {
                if (-not (Test-ThresholdRuleMatchValue $rule.platform $record.platform)) {
                    continue
                }
                if (-not (Test-ThresholdRuleMatchValue $rule.profile $record.profile)) {
                    continue
                }
                $scopePresent = $true
                break
            }
            if (-not $scopePresent) {
                continue
            }
            $comparisons += [pscustomobject]@{
                platform = if ($null -eq $rule.platform) { "*" } else { [string]$rule.platform }
                profile = if ($null -eq $rule.profile) { "*" } else { [string]$rule.profile }
                name = if ($null -eq $rule.name) { "*" } else { [string]$rule.name }
                key = if ($null -eq $rule.key) { "*" } else { [string]$rule.key }
                status = "missing-diagnostic-record"
                current_value = $null
                min_value = $ruleMinValue
                max_value = $ruleMaxValue
                sample_index = $null
                rule_index = $ruleIndex
            }
        }
    }

    return $comparisons
}

function New-DiagnosticComparisonSummary {
    param(
        [AllowEmptyCollection()]
        [object[]]$Comparisons,
        [AllowEmptyCollection()]
        [object[]]$FailedComparisons
    )

    return [pscustomobject]@{
        total = @($Comparisons).Count
        failed = @($FailedComparisons).Count
        ok = @($Comparisons | Where-Object { $_.status -eq "ok" }).Count
        threshold = @($Comparisons | Where-Object { $_.status -eq "diagnostic-threshold" }).Count
        missing_diagnostics = @($Comparisons | Where-Object { $_.status -eq "missing-diagnostics" }).Count
        missing_record = @($Comparisons | Where-Object { $_.status -eq "missing-diagnostic-record" }).Count
        missing_key = @($Comparisons | Where-Object { $_.status -eq "missing-diagnostic-key" }).Count
    }
}

function Get-LatencyNumericKeys {
    param([Parameter(Mandatory = $true)]$Record)

    $keys = @()
    foreach ($property in $Record.PSObject.Properties) {
        if ($property.Name -eq "platform" -or
            $property.Name -eq "profile" -or
            $property.Name -eq "name" -or
            $property.Name -eq "raw" -or
            $property.Name -eq "sample_index" -or
            $property.Name -eq "sample_aggregation") {
            continue
        }
        if ($property.Value -is [byte] -or
            $property.Value -is [int16] -or
            $property.Value -is [int32] -or
            $property.Value -is [int64] -or
            $property.Value -is [uint16] -or
            $property.Value -is [uint32] -or
            $property.Value -is [uint64] -or
            $property.Value -is [double]) {
            $keys += $property.Name
        }
    }
    return $keys
}

function Compare-LatencyThresholds {
    param(
        [AllowEmptyCollection()]
        [object[]]$Records,
        [AllowEmptyCollection()]
        [object[]]$Rules
    )

    $comparisons = @()
    if (@($Rules).Count -eq 0) {
        return $comparisons
    }
    if (@($Records).Count -eq 0) {
        for ($i = 0; $i -lt @($Rules).Count; ++$i) {
            $rule = $Rules[$i]
            $minValue = Get-DiagnosticRuleLimit $rule "min_value"
            $maxValue = Get-DiagnosticRuleLimit $rule "max_value"
            $comparisons += [pscustomobject]@{
                platform = if ($null -eq $rule.platform) { "*" } else { [string]$rule.platform }
                profile = if ($null -eq $rule.profile) { "*" } else { [string]$rule.profile }
                name = if ($null -eq $rule.name) { "*" } else { [string]$rule.name }
                key = if ($null -eq $rule.key) { "*" } else { [string]$rule.key }
                status = "missing-latencies"
                current_value = $null
                min_value = $minValue
                max_value = $maxValue
                sample_index = $null
                rule_index = $i
            }
        }
        return $comparisons
    }

    for ($ruleIndex = 0; $ruleIndex -lt @($Rules).Count; ++$ruleIndex) {
        $rule = $Rules[$ruleIndex]
        $ruleMinValue = Get-DiagnosticRuleLimit $rule "min_value"
        $ruleMaxValue = Get-DiagnosticRuleLimit $rule "max_value"
        $matchedAny = $false
        foreach ($record in $Records) {
            if (-not (Test-ThresholdRuleMatchValue $rule.platform $record.platform)) {
                continue
            }
            if (-not (Test-ThresholdRuleMatchValue $rule.profile $record.profile)) {
                continue
            }
            if (-not (Test-ThresholdRuleMatchValue $rule.name $record.name)) {
                continue
            }
            $matchedAny = $true
            $keys = if ($null -eq $rule.key -or [string]$rule.key -eq "*") {
                Get-LatencyNumericKeys $record
            } else {
                @([string]$rule.key)
            }
            foreach ($key in $keys) {
                $property = $record.PSObject.Properties[$key]
                if ($null -eq $property) {
                    $comparisons += [pscustomobject]@{
                        platform = $record.platform
                        profile = $record.profile
                        name = $record.name
                        key = $key
                        status = "missing-latency-key"
                        current_value = $null
                        min_value = $ruleMinValue
                        max_value = $ruleMaxValue
                        sample_index = $record.sample_index
                        rule_index = $ruleIndex
                    }
                    continue
                }
                $value = [int64]$property.Value
                $comparisons += [pscustomobject]@{
                    platform = $record.platform
                    profile = $record.profile
                    name = $record.name
                    key = $key
                    status = if (Test-DiagnosticValueAgainstRule $value $ruleMinValue $ruleMaxValue) { "ok" } else { "latency-threshold" }
                    current_value = $value
                    min_value = $ruleMinValue
                    max_value = $ruleMaxValue
                    sample_index = $record.sample_index
                    rule_index = $ruleIndex
                }
            }
        }
        if (-not $matchedAny) {
            $scopePresent = $false
            foreach ($record in $Records) {
                if (-not (Test-ThresholdRuleMatchValue $rule.platform $record.platform)) {
                    continue
                }
                if (-not (Test-ThresholdRuleMatchValue $rule.profile $record.profile)) {
                    continue
                }
                $scopePresent = $true
                break
            }
            if (-not $scopePresent) {
                continue
            }
            $comparisons += [pscustomobject]@{
                platform = if ($null -eq $rule.platform) { "*" } else { [string]$rule.platform }
                profile = if ($null -eq $rule.profile) { "*" } else { [string]$rule.profile }
                name = if ($null -eq $rule.name) { "*" } else { [string]$rule.name }
                key = if ($null -eq $rule.key) { "*" } else { [string]$rule.key }
                status = "missing-latency-record"
                current_value = $null
                min_value = $ruleMinValue
                max_value = $ruleMaxValue
                sample_index = $null
                rule_index = $ruleIndex
            }
        }
    }
    return $comparisons
}

function New-LatencyComparisonSummary {
    param(
        [AllowEmptyCollection()]
        [object[]]$Comparisons,
        [AllowEmptyCollection()]
        [object[]]$FailedComparisons
    )

    return [pscustomobject]@{
        total = @($Comparisons).Count
        failed = @($FailedComparisons).Count
        ok = @($Comparisons | Where-Object { $_.status -eq "ok" }).Count
        threshold = @($Comparisons | Where-Object { $_.status -eq "latency-threshold" }).Count
        missing_latencies = @($Comparisons | Where-Object { $_.status -eq "missing-latencies" }).Count
        missing_record = @($Comparisons | Where-Object { $_.status -eq "missing-latency-record" }).Count
        missing_key = @($Comparisons | Where-Object { $_.status -eq "missing-latency-key" }).Count
    }
}

function Get-MetadataNumericKeys {
    param([Parameter(Mandatory = $true)]$Record)

    return Get-DiagnosticNumericKeys $Record
}

function Compare-MetadataThresholds {
    param(
        [AllowEmptyCollection()]
        [object[]]$Records,
        [AllowEmptyCollection()]
        [object[]]$Rules
    )

    $comparisons = @()
    if (@($Rules).Count -eq 0) {
        return $comparisons
    }
    if (@($Records).Count -eq 0) {
        for ($i = 0; $i -lt @($Rules).Count; ++$i) {
            $rule = $Rules[$i]
            $minValue = Get-DiagnosticRuleLimit $rule "min_value"
            $maxValue = Get-DiagnosticRuleLimit $rule "max_value"
            $comparisons += [pscustomobject]@{
                platform = if ($null -eq $rule.platform) { "*" } else { [string]$rule.platform }
                profile = if ($null -eq $rule.profile) { "*" } else { [string]$rule.profile }
                name = if ($null -eq $rule.name) { "*" } else { [string]$rule.name }
                key = if ($null -eq $rule.key) { "*" } else { [string]$rule.key }
                status = "missing-metadata"
                current_value = $null
                min_value = $minValue
                max_value = $maxValue
                sample_index = $null
                rule_index = $i
            }
        }
        return $comparisons
    }

    for ($ruleIndex = 0; $ruleIndex -lt @($Rules).Count; ++$ruleIndex) {
        $rule = $Rules[$ruleIndex]
        $ruleMinValue = Get-DiagnosticRuleLimit $rule "min_value"
        $ruleMaxValue = Get-DiagnosticRuleLimit $rule "max_value"
        $matchedAny = $false
        foreach ($record in $Records) {
            if (-not (Test-ThresholdRuleMatchValue $rule.platform $record.platform)) {
                continue
            }
            if (-not (Test-ThresholdRuleMatchValue $rule.profile $record.profile)) {
                continue
            }
            if (-not (Test-ThresholdRuleMatchValue $rule.name $record.name)) {
                continue
            }
            $matchedAny = $true
            $keys = if ($null -eq $rule.key -or [string]$rule.key -eq "*") {
                Get-MetadataNumericKeys $record
            } else {
                @([string]$rule.key)
            }
            foreach ($key in $keys) {
                $property = $record.PSObject.Properties[$key]
                if ($null -eq $property) {
                    $comparisons += [pscustomobject]@{
                        platform = $record.platform
                        profile = $record.profile
                        name = $record.name
                        key = $key
                        status = "missing-metadata-key"
                        current_value = $null
                        min_value = $ruleMinValue
                        max_value = $ruleMaxValue
                        sample_index = $record.sample_index
                        rule_index = $ruleIndex
                    }
                    continue
                }
                $value = [int64]$property.Value
                $comparisons += [pscustomobject]@{
                    platform = $record.platform
                    profile = $record.profile
                    name = $record.name
                    key = $key
                    status = if (Test-DiagnosticValueAgainstRule $value $ruleMinValue $ruleMaxValue) { "ok" } else { "metadata-threshold" }
                    current_value = $value
                    min_value = $ruleMinValue
                    max_value = $ruleMaxValue
                    sample_index = $record.sample_index
                    rule_index = $ruleIndex
                }
            }
        }
        if (-not $matchedAny) {
            $scopePresent = $false
            foreach ($record in $Records) {
                if (-not (Test-ThresholdRuleMatchValue $rule.platform $record.platform)) {
                    continue
                }
                if (-not (Test-ThresholdRuleMatchValue $rule.profile $record.profile)) {
                    continue
                }
                $scopePresent = $true
                break
            }
            if (-not $scopePresent) {
                continue
            }
            $comparisons += [pscustomobject]@{
                platform = if ($null -eq $rule.platform) { "*" } else { [string]$rule.platform }
                profile = if ($null -eq $rule.profile) { "*" } else { [string]$rule.profile }
                name = if ($null -eq $rule.name) { "*" } else { [string]$rule.name }
                key = if ($null -eq $rule.key) { "*" } else { [string]$rule.key }
                status = "missing-metadata-record"
                current_value = $null
                min_value = $ruleMinValue
                max_value = $ruleMaxValue
                sample_index = $null
                rule_index = $ruleIndex
            }
        }
    }
    # Scheduler preemption can produce an isolated outlier even when repeated
    # samples are stable. A rule may tolerate a bounded number of failures, but
    # only after collecting its configured minimum number of observations.
    for ($ruleIndex = 0; $ruleIndex -lt @($Rules).Count; ++$ruleIndex) {
        $rule = $Rules[$ruleIndex]
        $allowanceProperty = $rule.PSObject.Properties["max_failed_samples"]
        if ($null -eq $allowanceProperty) {
            continue
        }
        $allowance = [int]$allowanceProperty.Value
        $minSamplesProperty =
            $rule.PSObject.Properties["min_samples_for_outlier_tolerance"]
        $minSamples = if ($null -eq $minSamplesProperty) {
            1
        } else {
            [int]$minSamplesProperty.Value
        }
        $failedGroups = @($comparisons | Where-Object {
            $_.rule_index -eq $ruleIndex -and $_.status -eq "metadata-threshold"
        } | Group-Object platform, profile, name, key)
        foreach ($group in $failedGroups) {
            $first = $group.Group[0]
            $observationCount = @($comparisons | Where-Object {
                $_.rule_index -eq $ruleIndex -and
                $_.platform -eq $first.platform -and
                $_.profile -eq $first.profile -and
                $_.name -eq $first.name -and
                $_.key -eq $first.key
            }).Count
            if ($observationCount -ge $minSamples -and
                @($group.Group).Count -le $allowance) {
                foreach ($comparison in $group.Group) {
                    $comparison.status = "ok"
                    $comparison | Add-Member -NotePropertyName tolerated_sample_outlier `
                        -NotePropertyValue $true -Force
                    $comparison | Add-Member -NotePropertyName tolerated_sample_group_size `
                        -NotePropertyValue $observationCount -Force
                }
            }
        }
    }
    return $comparisons
}

function New-MetadataComparisonSummary {
    param(
        [AllowEmptyCollection()]
        [object[]]$Comparisons,
        [AllowEmptyCollection()]
        [object[]]$FailedComparisons
    )

    return [pscustomobject]@{
        total = @($Comparisons).Count
        failed = @($FailedComparisons).Count
        ok = @($Comparisons | Where-Object { $_.status -eq "ok" }).Count
        tolerated_sample_outliers = @($Comparisons | Where-Object {
                $_.tolerated_sample_outlier -eq $true
            }).Count
        threshold = @($Comparisons | Where-Object { $_.status -eq "metadata-threshold" }).Count
        missing_metadata = @($Comparisons | Where-Object { $_.status -eq "missing-metadata" }).Count
        missing_record = @($Comparisons | Where-Object { $_.status -eq "missing-metadata-record" }).Count
        missing_key = @($Comparisons | Where-Object { $_.status -eq "missing-metadata-key" }).Count
    }
}

function Format-NullableDouble {
    param(
        $Value,
        [string]$Format = "N1"
    )

    if ($null -eq $Value) {
        return "-"
    }
    return ([double]$Value).ToString($Format, [System.Globalization.CultureInfo]::InvariantCulture)
}

function Format-NullableInteger {
    param($Value)

    if ($null -eq $Value) {
        return "-"
    }
    return [string]$Value
}

function ConvertTo-DisplayArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ($Value.Length -gt 0 -and $Value -notmatch "[\s']") {
        return $Value
    }
    return "'" + ($Value -replace "'", "''") + "'"
}

function Join-DisplayCommand {
    param([AllowEmptyCollection()][string[]]$Arguments)

    return (($Arguments | ForEach-Object { ConvertTo-DisplayArgument $_ }) -join " ")
}

function Get-BenchmarkRecordByName {
    param(
        [AllowEmptyCollection()]
        [object[]]$Records,
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [string]$Platform = "",
        [string]$Profile = ""
    )

    foreach ($record in $Records) {
        if ($record.name -eq $Name -and
            ($Platform.Length -eq 0 -or $record.platform -eq $Platform) -and
            ($Profile.Length -eq 0 -or $record.profile -eq $Profile)) {
            return $record
        }
    }
    return $null
}

function New-QuickBenchmarkSummary {
    param(
        [AllowEmptyCollection()]
        [object[]]$Records
    )

    $items = @(
        [pscustomobject]@{
            key = "small_malloc"
            label = "small malloc"
            uhm_name = "uhm small malloc/free"
            baseline_name = "system malloc small"
            baseline_label = "system malloc"
        },
        [pscustomobject]@{
            key = "multithread_small"
            label = "multithread small"
            uhm_name = "uhm small malloc/free multithread"
            baseline_name = "system malloc multithread"
            baseline_label = "system malloc"
        },
        [pscustomobject]@{
            key = "large_realloc_free"
            label = "large realloc/free"
            uhm_name = "uhm large malloc/realloc/free"
            baseline_name = "system malloc large/realloc/free"
            baseline_label = "system malloc"
        },
        [pscustomobject]@{
            key = "mmap_munmap"
            label = "mmap/munmap"
            uhm_name = "uhm mmap/munmap large"
            baseline_name = "system mmap/VirtualAlloc large"
            baseline_label = "system mmap/VirtualAlloc"
        },
        [pscustomobject]@{
            key = "mprotect"
            label = "mprotect"
            uhm_name = "uhm mprotect split/merge cost"
            baseline_name = ""
            baseline_label = ""
        },
        [pscustomobject]@{
            key = "collision"
            label = "collision"
            uhm_name = "uhm malloc/mmap collision pressure"
            baseline_name = ""
            baseline_label = ""
        },
        [pscustomobject]@{
            key = "release_mixed"
            label = "release mixed"
            uhm_name = "uhm release mixed long pressure"
            baseline_name = ""
            baseline_label = ""
        }
    )

    $summary = @()
    $groups = @($Records | Group-Object { "$($_.platform)|$($_.profile)" })
    foreach ($group in $groups) {
        $first = $group.Group[0]
        $hasQuickItem = $false
        foreach ($item in $items) {
            if ($item.baseline_name.Length -gt 0 -and
                $null -ne (Get-BenchmarkRecordByName `
                    -Records @($group.Group) `
                    -Name $item.baseline_name)) {
                $hasQuickItem = $true
                break
            }
        }
        if (-not $hasQuickItem) {
            continue
        }
        foreach ($item in $items) {
            $uhm = Get-BenchmarkRecordByName `
                -Records @($group.Group) `
                -Name $item.uhm_name `
                -Platform $first.platform `
                -Profile $first.profile
            $baseline = $null
            if ($item.baseline_name.Length -gt 0) {
                $baseline = Get-BenchmarkRecordByName `
                    -Records @($group.Group) `
                    -Name $item.baseline_name `
                    -Platform $first.platform `
                    -Profile $first.profile
            }
            $ratio = $null
            if ($null -ne $uhm -and $null -ne $baseline -and
                [double]$baseline.ns_per_op -gt 0.0) {
                $ratio = [double]$uhm.ns_per_op / [double]$baseline.ns_per_op
            }

            $summary += [pscustomobject]@{
                platform = $first.platform
                profile = $first.profile
                key = $item.key
                label = $item.label
                uhm_name = $item.uhm_name
                uhm_ns_per_op = if ($null -ne $uhm) { [double]$uhm.ns_per_op } else { $null }
                baseline_name = $item.baseline_name
                baseline_label = $item.baseline_label
                baseline_ns_per_op = if ($null -ne $baseline) { [double]$baseline.ns_per_op } else { $null }
                uhm_to_baseline_ratio = $ratio
                status = if ($null -eq $uhm) {
                    "missing-uhm"
                } elseif ($item.baseline_name.Length -gt 0 -and $null -eq $baseline) {
                    "missing-baseline"
                } else {
                    "ok"
                }
            }
        }
    }
    return $summary
}

function Get-AggregatedDouble {
    param(
        [Parameter(Mandatory = $true)]
        [double[]]$Values,
        [Parameter(Mandatory = $true)]
        [string]$Method
    )

    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) {
        return 0.0
    }
    if ($Method -eq "min") {
        return [double]$sorted[0]
    }

    $middle = [int][Math]::Floor($sorted.Count / 2.0)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Add-SampleIndex {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$Records,
        [Parameter(Mandatory = $true)]
        [int]$SampleIndex
    )

    foreach ($record in $Records) {
        $record | Add-Member -NotePropertyName sample_index `
            -NotePropertyValue $SampleIndex -Force
    }
    return $Records
}

function New-AggregatedBenchmarkResults {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Records,
        [Parameter(Mandatory = $true)]
        [string]$Method
    )

    $aggregated = @()
    $groups = $Records | Group-Object { Get-BenchmarkResultKey $_ }
    foreach ($group in $groups) {
        $first = $group.Group[0]
        $seconds = Get-AggregatedDouble `
            -Values @($group.Group | ForEach-Object { [double]$_.seconds }) `
            -Method $Method
        $nsPerOp = Get-AggregatedDouble `
            -Values @($group.Group | ForEach-Object { [double]$_.ns_per_op }) `
            -Method $Method
        $operations = @($group.Group | ForEach-Object {
                [int64]$_.operations
            } | Sort-Object -Descending)[0]

        $aggregated += [pscustomobject]@{
            platform = $first.platform
            profile = $first.profile
            name = $first.name
            seconds = $seconds
            operations = $operations
            ns_per_op = $nsPerOp
            raw = "aggregated:$Method samples=$($group.Group.Count)"
            command = $first.command
            sample_count = $group.Group.Count
            sample_aggregation = $Method
        }
    }
    return $aggregated
}

function New-AggregatedBenchmarkLatencies {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Records,
        [Parameter(Mandatory = $true)]
        [string]$Method
    )

    $aggregated = @()
    $groups = $Records | Group-Object { Get-BenchmarkResultKey $_ }
    foreach ($group in $groups) {
        $first = $group.Group[0]
        $sampleCount = @($group.Group | ForEach-Object {
                [int64]$_.samples
            } | Sort-Object -Descending)[0]
        $minNs = [int64][Math]::Round((Get-AggregatedDouble `
                    -Values @($group.Group | ForEach-Object { [double]$_.min_ns }) `
                    -Method $Method))
        $p50Ns = [int64][Math]::Round((Get-AggregatedDouble `
                    -Values @($group.Group | ForEach-Object { [double]$_.p50_ns }) `
                    -Method $Method))
        $p95Ns = [int64][Math]::Round((Get-AggregatedDouble `
                    -Values @($group.Group | ForEach-Object { [double]$_.p95_ns }) `
                    -Method $Method))
        $p99Ns = [int64][Math]::Round((Get-AggregatedDouble `
                    -Values @($group.Group | ForEach-Object { [double]$_.p99_ns }) `
                    -Method $Method))
        $maxNs = [int64][Math]::Round((Get-AggregatedDouble `
                    -Values @($group.Group | ForEach-Object { [double]$_.max_ns }) `
                    -Method $Method))

        $record = [ordered]@{
            platform = $first.platform
            profile = $first.profile
            name = $first.name
            samples = $sampleCount
            min_ns = $minNs
            p50_ns = $p50Ns
            p95_ns = $p95Ns
            p99_ns = $p99Ns
            max_ns = $maxNs
            raw = "aggregated:$Method samples=$($group.Group.Count)"
            sample_count = $group.Group.Count
            sample_aggregation = $Method
        }
        foreach ($key in @(
                "work_units_per_sample",
                "p95_per_work_unit_ns",
                "p99_per_work_unit_ns",
                "max_per_work_unit_ns",
                "pages_per_sample",
                "p95_per_page_ns",
                "p99_per_page_ns",
                "max_per_page_ns")) {
            $values = @($group.Group | Where-Object {
                    $null -ne $_.PSObject.Properties[$key]
                } | ForEach-Object {
                    [double]$_.PSObject.Properties[$key].Value
                })
            if (@($values).Count -eq @($group.Group).Count) {
                $record[$key] = [int64][Math]::Round((Get-AggregatedDouble `
                            -Values $values `
                            -Method $Method))
            }
        }
        $aggregated += [pscustomobject]$record
    }
    return $aggregated
}

function Get-FileSha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LiteralPath
    )

    $stream = [System.IO.File]::OpenRead($LiteralPath)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash($stream)
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
    return (($digest | ForEach-Object { $_.ToString("x2") }) -join "")
}

function Invoke-WindowsBenchmark {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Configuration,
        [switch]$Stress,
        [switch]$FalseSharing,
        [switch]$SmallFastPath,
        [switch]$ReleaseBreakdown,
        [switch]$ReleasePressureMatrix,
        [switch]$SlabEmptyChurn,
        [switch]$HeapClose,
        [switch]$MmapLocality,
        [switch]$ControlAllocatorChurn,
        [switch]$LiveBlockLookup,
        [switch]$TreapAdversarial,
        [switch]$MprotectMultiVma,
        [switch]$MunmapMultiVma,
        [switch]$SpinlockThresholds,
        [switch]$MetadataFootprint,
        [switch]$SlabSlotSharing,
        [switch]$SlabSlotAb,
        [switch]$Gate32Bit,
        [switch]$Win32,
        [int]$MprotectStressPages = 0,
        [int]$MprotectLatencySamples = 0,
        [switch]$Diagnostics,
        [string]$DiagnosticsTiming = "on",
        [switch]$SkipBuild,
        [int]$SampleIndex = 0,
        [int]$TimeoutSeconds
    )

    $resolvedBuild = Join-Path $Root $BuildDir
    if (-not $SkipBuild) {
        if (-not (Test-Path (Join-Path $resolvedBuild "CMakeCache.txt"))) {
            $configureArgs = @(
                "-S", $Root,
                "-B", $resolvedBuild
            )
            if ($Win32) {
                $configureArgs += @(
                    "-G", "Visual Studio 17 2022",
                    "-A", "Win32"
                )
            }
            $configureArgs += @(
                "-DCMAKE_BUILD_TYPE=$Configuration",
                "-DUHMAS_BUILD_TESTS=ON",
                "-DUHMAS_BUILD_BENCHMARKS=ON"
            )
            if ($Win32) {
                $configureArgs += "-DUHMAS_EXPECT_32BIT=ON"
            }
            Invoke-Step "Configure Windows benchmark build" {
                Invoke-NativeChecked "cmake" $configureArgs
            }
        }
        Invoke-Step "Build Windows uhmas_benchmarks" {
            Invoke-NativeChecked "cmake" @(
                "--build", $resolvedBuild,
                "--config", $Configuration,
                "--target", "uhmas_benchmarks"
            )
        }
    }

    $exe = Find-WindowsBenchmarkExecutable $resolvedBuild $Configuration
    $binarySha256 = Get-FileSha256Hex -LiteralPath $exe
    $env = Get-BenchmarkEnvironment -Stress:$Stress -FalseSharing:$FalseSharing -SmallFastPath:$SmallFastPath -ReleaseBreakdown:$ReleaseBreakdown -ReleasePressureMatrix:$ReleasePressureMatrix -SlabEmptyChurn:$SlabEmptyChurn -HeapClose:$HeapClose -MmapLocality:$MmapLocality -ControlAllocatorChurn:$ControlAllocatorChurn -LiveBlockLookup:$LiveBlockLookup -TreapAdversarial:$TreapAdversarial -MprotectMultiVma:$MprotectMultiVma -MunmapMultiVma:$MunmapMultiVma -SpinlockThresholds:$SpinlockThresholds -MetadataFootprint:$MetadataFootprint -SlabSlotSharing:$SlabSlotSharing -SlabSlotAb:$SlabSlotAb -Gate32Bit:$Gate32Bit -MprotectStressPages $MprotectStressPages -MprotectLatencySamples $MprotectLatencySamples -Diagnostics:$Diagnostics -DiagnosticsTiming $DiagnosticsTiming
    if ($SampleIndex -gt 0) {
        $env["UHMAS_BENCH_SAMPLE_INDEX"] = [string]$SampleIndex
    }
    Invoke-Step "Run Windows benchmark" {
        $script:WindowsRunResult = Invoke-CapturedProcess $exe @() $env $TimeoutSeconds
    }
    if ($script:WindowsRunResult.ExitCode -ne 0) {
        Write-Host $script:WindowsRunResult.Stdout
        Write-Host $script:WindowsRunResult.Stderr
        throw "Windows benchmark exited with $($script:WindowsRunResult.ExitCode)"
    }
    $script:WindowsRunResult | Add-Member -NotePropertyName binary_sha256 `
        -NotePropertyValue $binarySha256 -Force
    return $script:WindowsRunResult
}

function Invoke-WslBenchmark {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WslProject,
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Configuration,
        [string]$WslDistro = "",
        [switch]$Stress,
        [switch]$FalseSharing,
        [switch]$SmallFastPath,
        [switch]$ReleaseBreakdown,
        [switch]$ReleasePressureMatrix,
        [switch]$SlabEmptyChurn,
        [switch]$HeapClose,
        [switch]$MmapLocality,
        [switch]$ControlAllocatorChurn,
        [switch]$LiveBlockLookup,
        [switch]$TreapAdversarial,
        [switch]$MprotectMultiVma,
        [switch]$MunmapMultiVma,
        [switch]$SpinlockThresholds,
        [switch]$MetadataFootprint,
        [switch]$SlabSlotSharing,
        [switch]$SlabSlotAb,
        [switch]$Gate32Bit,
        [switch]$WslI386,
        [int]$MprotectStressPages = 0,
        [int]$MprotectLatencySamples = 0,
        [switch]$Diagnostics,
        [string]$DiagnosticsTiming = "on",
        [switch]$SkipBuild,
        [int]$SampleIndex = 0,
        [int]$TimeoutSeconds
    )

    $quotedProject = ConvertTo-ShSingleQuoted $WslProject
    $quotedBuild = ConvertTo-ShSingleQuoted $BuildDir
    $envParts = @()
    if ($SampleIndex -gt 0) {
        $envParts += "UHMAS_BENCH_SAMPLE_INDEX=$SampleIndex"
    }
    if ($Stress) {
        $envParts += "UHMAS_BENCH_STRESS=1"
    }
    if ($FalseSharing) {
        $envParts += "UHMAS_BENCH_FALSE_SHARING=1"
    }
    if ($SmallFastPath) {
        $envParts += "UHMAS_BENCH_SMALL_FAST_PATH=1"
    }
    if ($ReleaseBreakdown) {
        $envParts += "UHMAS_BENCH_RELEASE_BREAKDOWN=1"
    }
    if ($ReleasePressureMatrix) {
        $envParts += "UHMAS_BENCH_RELEASE_PRESSURE_MATRIX=1"
    }
    if ($SlabEmptyChurn) {
        $envParts += "UHMAS_BENCH_SLAB_EMPTY_CHURN=1"
    }
    if ($HeapClose) {
        $envParts += "UHMAS_BENCH_HEAP_CLOSE=1"
    }
    if ($MmapLocality) {
        $envParts += "UHMAS_BENCH_MMAP_LOCALITY=1"
    }
    if ($ControlAllocatorChurn) {
        $envParts += "UHMAS_BENCH_CONTROL_ALLOCATOR_CHURN=1"
    }
    if ($LiveBlockLookup) {
        $envParts += "UHMAS_BENCH_LIVE_BLOCK_LOOKUP=1"
    }
    if ($TreapAdversarial) {
        $envParts += "UHMAS_BENCH_TREAP_ADVERSARIAL=1"
    }
    if ($MprotectMultiVma) {
        $envParts += "UHMAS_BENCH_MPROTECT_MULTI_VMA=1"
    }
    if ($MunmapMultiVma) {
        $envParts += "UHMAS_BENCH_MUNMAP_MULTI_VMA=1"
    }
    if ($SpinlockThresholds) {
        $envParts += "UHMAS_BENCH_SPINLOCK_THRESHOLDS=1"
    }
    if ($MetadataFootprint) {
        $envParts += "UHMAS_BENCH_METADATA_FOOTPRINT=1"
    }
    if ($SlabSlotSharing) {
        $envParts += "UHMAS_BENCH_SLAB_SLOT_SHARING=1"
    }
    if ($SlabSlotAb) {
        $envParts += "UHMAS_BENCH_SLAB_SLOT_AB=1"
    }
    if ($Gate32Bit) {
        $envParts += "UHMAS_BENCH_32BIT_GATE=1"
    }
    if ($MprotectStressPages -gt 0) {
        $envParts += "UHMAS_BENCH_MPROTECT_MULTI_VMA_STRESS_PAGES=$MprotectStressPages"
    }
    if ($MprotectLatencySamples -gt 0) {
        $envParts += "UHMAS_BENCH_MPROTECT_MULTI_VMA_LATENCY_SAMPLES=$MprotectLatencySamples"
    }
    if ($Diagnostics) {
        $envParts += "UHMAS_BENCH_DIAGNOSTICS=1"
        $envParts += if ($DiagnosticsTiming -eq "off") {
            "UHMAS_BENCH_DIAGNOSTICS_TIMING=0"
        } else {
            "UHMAS_BENCH_DIAGNOSTICS_TIMING=1"
        }
    }
    $envPrefix = ""
    if ($envParts.Count -gt 0) {
        $envPrefix = "env " + ($envParts -join " ") + " "
    }

    $buildScript = ""
    if (-not $SkipBuild) {
        $extraConfigureArgs = ""
        if ($WslI386) {
            $extraConfigureArgs = " -DCMAKE_C_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 -DUHMAS_EXPECT_32BIT=ON"
        }
        $buildScript = @"
if [ ! -f "$BuildDir/CMakeCache.txt" ]; then
    cmake -S . -B $quotedBuild -DCMAKE_BUILD_TYPE=$Configuration -DUHMAS_BUILD_TESTS=ON -DUHMAS_BUILD_BENCHMARKS=ON$extraConfigureArgs
fi
cmake --build $quotedBuild --target uhmas_benchmarks
"@
    }
    $script = @"
set -eu
cd $quotedProject
$buildScript
    cd $quotedBuild
    sha256sum ./uhmas_benchmarks
    $envPrefix./uhmas_benchmarks
"@
    $script = $script -replace "`r`n", "`n"
    $script = $script -replace "`r", "`n"
    Invoke-Step "Run WSL benchmark" {
        $wslArgs = @()
        if ($WslDistro.Length -gt 0) {
            $wslArgs += @("-d", $WslDistro)
        }
        $wslArgs += @("bash", "-lc", $script)
        $script:WslRunResult = Invoke-CapturedProcess "wsl.exe" $wslArgs @{} $TimeoutSeconds
    }
    if ($script:WslRunResult.ExitCode -ne 0) {
        Write-Host $script:WslRunResult.Stdout
        Write-Host $script:WslRunResult.Stderr
        throw "WSL benchmark exited with $($script:WslRunResult.ExitCode)"
    }
    $binaryMatch = [regex]::Match(
        $script:WslRunResult.Stdout,
        '(?m)^([0-9a-fA-F]{64})\s+\*?\./uhmas_benchmarks\s*$')
    if (-not $binaryMatch.Success) {
        throw "WSL benchmark binary SHA256 was not reported."
    }
    $script:WslRunResult | Add-Member -NotePropertyName binary_sha256 `
        -NotePropertyValue $binaryMatch.Groups[1].Value.ToLowerInvariant() -Force
    return $script:WslRunResult
}

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$runWindows = $Platform -eq "windows" -or $Platform -eq "both" -or
              $Platform -eq "win32" -or $Platform -eq "32bit-both"
$runWsl = $Platform -eq "wsl" -or $Platform -eq "both" -or
          $Platform -eq "wsl-i386" -or $Platform -eq "32bit-both"
if ($runWsl) {
    . (Join-Path $PSScriptRoot "wsl_project_path.ps1")
    $WslProject = Resolve-UhmWslProjectPath $WslProject $WslDistro $Root
}
$windowsIsWin32 = $Platform -eq "win32" -or $Platform -eq "32bit-both"
$wslIsI386 = $Platform -eq "wsl-i386" -or $Platform -eq "32bit-both"
$windowsPlatformLabel = if ($windowsIsWin32) { "win32" } else { "windows" }
$wslPlatformLabel = if ($wslIsI386) { "wsl-i386" } else { "wsl" }

if ($windowsIsWin32 -and $BuildDir -eq "build-release") {
    $BuildDir = "build-win32-msvc"
}
if ($wslIsI386 -and $WslBuildDir -eq "build-wsl-release") {
    $WslBuildDir = "build-wsl-i386-release"
}

if ($TimeoutSeconds -le 0) {
    throw "TimeoutSeconds must be positive."
}
if ($RegressionThresholdPercent -lt 0.0) {
    throw "RegressionThresholdPercent must be non-negative."
}
if ($Samples -le 0) {
    throw "Samples must be positive."
}
if ($MprotectLatencySamples -lt 0) {
    throw "MprotectLatencySamples must be non-negative."
}
if ($MprotectLatencySamples -gt 0 -and
    (-not $MprotectMultiVma -or $MprotectStressPages -le 0)) {
    throw "MprotectLatencySamples requires -MprotectMultiVma and -MprotectStressPages."
}
if ($ProfileNameOverride.Length -gt 0 -and
    $ProfileNameOverride -notmatch '^[A-Za-z0-9_.+-]+$') {
    throw "ProfileNameOverride may contain only letters, digits, '_', '.', '+', or '-'."
}

function Get-CompositeFileSha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][object[]]$Files
    )

    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/') +
                [System.IO.Path]::DirectorySeparatorChar
    $entries = @()
    foreach ($file in @($Files | Sort-Object FullName -Unique)) {
        $full = [System.IO.Path]::GetFullPath($file.FullName)
        if (-not $full.StartsWith($rootFull,
                                 [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Fingerprint file is outside the project root: $full"
        }
        $relative = $full.Substring($rootFull.Length).Replace('\', '/')
        $hash = Get-FileSha256Hex -LiteralPath $full
        $entries += "$relative`t$hash"
    }
    $payload = [System.Text.Encoding]::UTF8.GetBytes(($entries -join "`n"))
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash($payload)
    } finally {
        $sha.Dispose()
    }
    return (($digest | ForEach-Object { $_.ToString("x2") }) -join "")
}

function Get-BenchmarkProvenanceInputs {
    param([Parameter(Mandatory = $true)][string]$Root)

    $sourceFiles = @(
        Get-ChildItem -LiteralPath (Join-Path $Root "src") -File -Recurse
        Get-ChildItem -LiteralPath (Join-Path $Root "include") -File -Recurse
        Get-Item -LiteralPath (Join-Path $Root "CMakeLists.txt")
    )
    $workloadFiles = @(
        Get-ChildItem -LiteralPath (Join-Path $Root "tests") -File |
            Where-Object { $_.Name -like "bench_uhm*" }
    )
    return [pscustomobject]@{
        source_sha256 = Get-CompositeFileSha256 $Root $sourceFiles
        workload_sha256 = Get-CompositeFileSha256 $Root $workloadFiles
        source_file_count = @($sourceFiles).Count
        workload_file_count = @($workloadFiles).Count
    }
}

function Get-BenchmarkWorkloadContractVersion {
    param([Parameter(Mandatory = $true)][string]$Profile)

    switch -Wildcard ($Profile) {
        # v2 added affinity, v3 synchronized starts, v4 isolates page metadata.
        "false-sharing-*" { return 4 }
        # v2 adds the real thread_ctx retained-hint collision workload.
        "small-fast-path-*" { return 2 }
        # v2 adds per-workload CPU attribution and same-run relative ratios;
        # v3 adds scheduler state, thread CPU/wait attribution and chunk P99;
        # v4 balances policy execution order across independent samples;
        # v5 defers full-heap checks to each policy boundary.
        "spinlock-thresholds-*" { return 5 }
        default { return 1 }
    }
}

function New-BenchmarkWorkloadContract {
    param([Parameter(Mandatory = $true)][string]$Profile)

    return [pscustomobject][ordered]@{
        schema = $BenchmarkWorkloadContractSchema
        id = $Profile
        version = Get-BenchmarkWorkloadContractVersion -Profile $Profile
    }
}

function Get-BenchmarkWorkloadContractMismatchReasons {
    param(
        [Parameter(Mandatory = $true)]$Current,
        [Parameter(Mandatory = $true)]$Baseline
    )

    $reasons = @()
    $baselineSchemaProperty = $Baseline.PSObject.Properties["schema"]
    $baselineSchema = if ($null -ne $baselineSchemaProperty) {
        [int]$baselineSchemaProperty.Value
    } else {
        0
    }
    $baselineContractProperty = $Baseline.PSObject.Properties["workload_contract"]
    $baselineContract = if ($null -ne $baselineContractProperty) {
        $baselineContractProperty.Value
    } elseif ($baselineSchema -eq 2) {
        # Schema 2 had one global workload SHA. Treat each existing profile as
        # contract v1 so unrelated profile edits do not invalidate it.
        [pscustomobject]@{
            schema = $BenchmarkWorkloadContractSchema
            id = [string]$Baseline.profile
            version = 1
        }
    } else {
        $null
    }

    if ($null -eq $baselineContract) {
        return @("baseline workload contract is missing")
    }
    foreach ($field in @("schema", "id", "version")) {
        $baselineProperty = $baselineContract.PSObject.Properties[$field]
        $currentProperty = $Current.workload_contract.PSObject.Properties[$field]
        if ($null -eq $baselineProperty) {
            $reasons += "baseline workload contract field '$field' is missing"
        } elseif ([string]$baselineProperty.Value -cne
                  [string]$currentProperty.Value) {
            $reasons += "workload contract field '$field' differs"
        }
    }
    return @($reasons)
}

function Get-ProvenanceMismatchReasons {
    param(
        [Parameter(Mandatory = $true)]$Current,
        $Baseline
    )

    $reasons = @()
    if ($null -eq $Baseline) {
        return @("baseline provenance is missing")
    }
    $baselineSchemaProperty = $Baseline.PSObject.Properties["schema"]
    if ($null -eq $baselineSchemaProperty) {
        $reasons += "baseline provenance field 'schema' is missing"
    } elseif ([int]$baselineSchemaProperty.Value -ne 2 -and
              [int]$baselineSchemaProperty.Value -ne $BenchmarkProvenanceSchema) {
        $reasons += "baseline provenance schema is unsupported"
    }
    foreach ($field in @("source_sha256", "workload_sha256",
                          "configuration", "profile",
                          "diagnostics_enabled",
                          "diagnostics_timing_enabled")) {
        $baselineProperty = $Baseline.PSObject.Properties[$field]
        $currentProperty = $Current.PSObject.Properties[$field]
        if ($null -eq $baselineProperty) {
            $reasons += "baseline provenance field '$field' is missing"
        } elseif ($field -notin @("source_sha256", "workload_sha256") -and
                  [string]$baselineProperty.Value -cne
                  [string]$currentProperty.Value) {
            $reasons += "provenance field '$field' differs"
        }
    }
    $reasons += @(Get-BenchmarkWorkloadContractMismatchReasons `
        -Current $Current -Baseline $Baseline)
    $baselineBinaries = $Baseline.PSObject.Properties["binaries"]
    if ($null -eq $baselineBinaries) {
        $reasons += "baseline binary fingerprints are missing"
    } else {
        foreach ($property in $Current.binaries.PSObject.Properties) {
            $baselineBinary = $baselineBinaries.Value.PSObject.Properties[$property.Name]
            if ($null -eq $baselineBinary) {
                $reasons += "baseline binary fingerprint '$($property.Name)' is missing"
            }
        }
    }
    return @($reasons)
}

function New-ProvenanceSelfTestRecord {
    param(
        [Parameter(Mandatory = $true)][int]$Schema,
        [Parameter(Mandatory = $true)][string]$Profile,
        [Parameter(Mandatory = $true)][string]$SourceSha,
        [Parameter(Mandatory = $true)][string]$WorkloadSha,
        [int]$ContractVersion = 0
    )

    $record = [pscustomobject][ordered]@{
        schema = $Schema
        source_sha256 = $SourceSha
        workload_sha256 = $WorkloadSha
        configuration = "Release"
        profile = $Profile
        diagnostics_enabled = $Profile -ne "quick"
        diagnostics_timing_enabled = $Profile -ne "quick"
        binaries = [pscustomobject]@{ windows = "binary-a" }
    }
    if ($Schema -ge 3) {
        $version = if ($ContractVersion -gt 0) {
            $ContractVersion
        } else {
            Get-BenchmarkWorkloadContractVersion -Profile $Profile
        }
        Add-Member -InputObject $record -NotePropertyName workload_contract `
            -NotePropertyValue ([pscustomobject]@{
                schema = $BenchmarkWorkloadContractSchema
                id = $Profile
                version = $version
            })
    }
    return $record
}

function Assert-ProvenanceSelfTest {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "Benchmark provenance self-test failed: $Message"
    }
}

function Invoke-BenchmarkProvenanceSelfTest {
    $quickCurrent = New-ProvenanceSelfTestRecord `
        -Schema 3 -Profile "quick" -SourceSha "source-new" `
        -WorkloadSha "workload-new"
    $quickSchema2Baseline = New-ProvenanceSelfTestRecord `
        -Schema 2 -Profile "quick" -SourceSha "source-old" `
        -WorkloadSha "workload-old"
    $quickReasons = @(Get-ProvenanceMismatchReasons `
        -Current $quickCurrent -Baseline $quickSchema2Baseline)
    Assert-ProvenanceSelfTest ($quickReasons.Count -eq 0) `
        "an unrelated global source/workload hash change invalidated quick"

    $falseSharingCurrent = New-ProvenanceSelfTestRecord `
        -Schema 3 -Profile "false-sharing-quick" -SourceSha "source-new" `
        -WorkloadSha "workload-new"
    $falseSharingSchema2Baseline = New-ProvenanceSelfTestRecord `
        -Schema 2 -Profile "false-sharing-quick" -SourceSha "source-old" `
        -WorkloadSha "workload-old"
    $falseSharingLegacyReasons = @(Get-ProvenanceMismatchReasons `
        -Current $falseSharingCurrent -Baseline $falseSharingSchema2Baseline)
    Assert-ProvenanceSelfTest `
        (@($falseSharingLegacyReasons | Where-Object {
            $_ -eq "workload contract field 'version' differs"
        }).Count -eq 1) `
        "false-sharing workload v4 did not invalidate its schema-2/v1 baseline"

    $falseSharingSameContract = New-ProvenanceSelfTestRecord `
        -Schema 3 -Profile "false-sharing-quick" -SourceSha "source-old" `
        -WorkloadSha "workload-old"
    $sameContractReasons = @(Get-ProvenanceMismatchReasons `
        -Current $falseSharingCurrent -Baseline $falseSharingSameContract)
    Assert-ProvenanceSelfTest ($sameContractReasons.Count -eq 0) `
        "trace-only source/workload hashes rejected the same profile contract"

    $falseSharingOldContract = New-ProvenanceSelfTestRecord `
        -Schema 3 -Profile "false-sharing-quick" -SourceSha "source-new" `
        -WorkloadSha "workload-new" -ContractVersion 1
    $oldContractReasons = @(Get-ProvenanceMismatchReasons `
        -Current $falseSharingCurrent -Baseline $falseSharingOldContract)
    Assert-ProvenanceSelfTest `
        (@($oldContractReasons | Where-Object {
            $_ -eq "workload contract field 'version' differs"
        }).Count -eq 1) `
        "a same-profile workload contract change was not rejected"

    Write-Host "Benchmark provenance self-test passed."
}

if ($ProvenanceSelfTest) {
    Invoke-BenchmarkProvenanceSelfTest
    return
}
if (-not $Diagnostics -and $DiagnosticsTiming -eq "off") {
    throw "DiagnosticsTiming=off requires -Diagnostics."
}

$resolvedThresholdConfigPath = Resolve-OptionalPath $Root $ThresholdConfigPath
$thresholdRules = @()
$sameRunThresholdRules = @()
if ($resolvedThresholdConfigPath.Length -gt 0) {
    if (-not (Test-Path $resolvedThresholdConfigPath)) {
        throw "ThresholdConfigPath does not exist: $resolvedThresholdConfigPath"
    }
    $thresholdConfig = Get-Content $resolvedThresholdConfigPath -Raw | ConvertFrom-Json
    if ($null -ne $thresholdConfig.default_threshold_percent) {
        $RegressionThresholdPercent = [double]$thresholdConfig.default_threshold_percent
    }
    if ($RegressionThresholdPercent -lt 0.0) {
        throw "Configured default threshold must be non-negative."
    }
    if ($null -ne $thresholdConfig.rules) {
        $thresholdRules = @($thresholdConfig.rules)
    }
    if ($null -ne $thresholdConfig.same_run_rules) {
        $sameRunThresholdRules = @($thresholdConfig.same_run_rules)
    }
    foreach ($rule in $thresholdRules) {
        if ($null -eq $rule.threshold_percent) {
            throw "Every threshold rule must provide threshold_percent."
        }
        if ([double]$rule.threshold_percent -lt 0.0) {
            throw "Threshold rule values must be non-negative."
        }
    }
    foreach ($rule in $sameRunThresholdRules) {
        if ($null -eq $rule.platform -or $null -eq $rule.profile -or
            $null -eq $rule.name -or $null -eq $rule.baseline_name) {
            throw "Every same-run threshold rule must provide platform, profile, name, and baseline_name."
        }
        if ($null -eq $rule.min_ratio -and $null -eq $rule.max_ratio) {
            throw "Every same-run threshold rule must provide min_ratio, max_ratio, or both."
        }
        if ($null -ne $rule.min_ratio -and [double]$rule.min_ratio -lt 0.0) {
            throw "Same-run threshold min_ratio must be non-negative."
        }
        if ($null -ne $rule.max_ratio -and [double]$rule.max_ratio -lt 0.0) {
            throw "Same-run threshold max_ratio must be non-negative."
        }
        if ($null -ne $rule.min_ratio -and $null -ne $rule.max_ratio -and
            [double]$rule.min_ratio -gt [double]$rule.max_ratio) {
            throw "Same-run threshold min_ratio must be less than or equal to max_ratio."
        }
    }
}

$resolvedDiagnosticThresholdConfigPath =
    Resolve-OptionalPath $Root $DiagnosticThresholdConfigPath
$diagnosticThresholdRules = @()
if ($resolvedDiagnosticThresholdConfigPath.Length -gt 0) {
    if (-not (Test-Path $resolvedDiagnosticThresholdConfigPath)) {
        throw "DiagnosticThresholdConfigPath does not exist: $resolvedDiagnosticThresholdConfigPath"
    }
    $diagnosticThresholdConfig =
        Get-Content $resolvedDiagnosticThresholdConfigPath -Raw | ConvertFrom-Json
    if ($null -ne $diagnosticThresholdConfig.rules) {
        $diagnosticThresholdRules = @($diagnosticThresholdConfig.rules)
    }
    foreach ($rule in $diagnosticThresholdRules) {
        $minValue = Get-DiagnosticRuleLimit $rule "min_value"
        $maxValue = Get-DiagnosticRuleLimit $rule "max_value"
        if ($null -eq $minValue -and $null -eq $maxValue) {
            throw "Every diagnostic threshold rule must provide min_value, max_value, or both."
        }
        if ($null -ne $minValue -and [int64]$minValue -lt 0) {
            throw "Diagnostic threshold min_value must be non-negative."
        }
        if ($null -ne $maxValue -and [int64]$maxValue -lt 0) {
            throw "Diagnostic threshold max_value must be non-negative."
        }
        if ($null -ne $minValue -and $null -ne $maxValue -and
            [int64]$minValue -gt [int64]$maxValue) {
            throw "Diagnostic threshold min_value must be less than or equal to max_value."
        }
    }
}

$resolvedLatencyThresholdConfigPath =
    Resolve-OptionalPath $Root $LatencyThresholdConfigPath
$latencyThresholdRules = @()
if ($resolvedLatencyThresholdConfigPath.Length -gt 0) {
    if (-not (Test-Path $resolvedLatencyThresholdConfigPath)) {
        throw "LatencyThresholdConfigPath does not exist: $resolvedLatencyThresholdConfigPath"
    }
    $latencyThresholdConfig =
        Get-Content $resolvedLatencyThresholdConfigPath -Raw | ConvertFrom-Json
    if ($null -ne $latencyThresholdConfig.rules) {
        $latencyThresholdRules = @($latencyThresholdConfig.rules)
    }
    foreach ($rule in $latencyThresholdRules) {
        $minValue = Get-DiagnosticRuleLimit $rule "min_value"
        $maxValue = Get-DiagnosticRuleLimit $rule "max_value"
        if ($null -eq $minValue -and $null -eq $maxValue) {
            throw "Every latency threshold rule must provide min_value, max_value, or both."
        }
        if ($null -ne $minValue -and [int64]$minValue -lt 0) {
            throw "Latency threshold min_value must be non-negative."
        }
        if ($null -ne $maxValue -and [int64]$maxValue -lt 0) {
            throw "Latency threshold max_value must be non-negative."
        }
        if ($null -ne $minValue -and $null -ne $maxValue -and
            [int64]$minValue -gt [int64]$maxValue) {
            throw "Latency threshold min_value must be less than or equal to max_value."
        }
    }
}

$resolvedMetadataThresholdConfigPath =
    Resolve-OptionalPath $Root $MetadataThresholdConfigPath
$metadataThresholdRules = @()
if ($resolvedMetadataThresholdConfigPath.Length -gt 0) {
    if (-not (Test-Path $resolvedMetadataThresholdConfigPath)) {
        throw "MetadataThresholdConfigPath does not exist: $resolvedMetadataThresholdConfigPath"
    }
    $metadataThresholdConfig =
        Get-Content $resolvedMetadataThresholdConfigPath -Raw | ConvertFrom-Json
    if ($null -ne $metadataThresholdConfig.rules) {
        $metadataThresholdRules = @($metadataThresholdConfig.rules)
    }
    foreach ($rule in $metadataThresholdRules) {
        $minValue = Get-DiagnosticRuleLimit $rule "min_value"
        $maxValue = Get-DiagnosticRuleLimit $rule "max_value"
        if ($null -eq $minValue -and $null -eq $maxValue) {
            throw "Every metadata threshold rule must provide min_value, max_value, or both."
        }
        if ($null -ne $minValue -and [int64]$minValue -lt 0) {
            throw "Metadata threshold min_value must be non-negative."
        }
        if ($null -ne $maxValue -and [int64]$maxValue -lt 0) {
            throw "Metadata threshold max_value must be non-negative."
        }
        if ($null -ne $minValue -and $null -ne $maxValue -and
            [int64]$minValue -gt [int64]$maxValue) {
            throw "Metadata threshold min_value must be less than or equal to max_value."
        }
        $allowanceProperty = $rule.PSObject.Properties["max_failed_samples"]
        if ($null -ne $allowanceProperty -and [int]$allowanceProperty.Value -lt 0) {
            throw "Metadata threshold max_failed_samples must be non-negative."
        }
        $minSamplesProperty =
            $rule.PSObject.Properties["min_samples_for_outlier_tolerance"]
        if ($null -ne $minSamplesProperty) {
            if ($null -eq $allowanceProperty) {
                throw "Metadata threshold min_samples_for_outlier_tolerance requires max_failed_samples."
            }
            if ([int]$minSamplesProperty.Value -le [int]$allowanceProperty.Value) {
                throw "Metadata threshold min_samples_for_outlier_tolerance must exceed max_failed_samples."
            }
        }
    }
}

$timestamp = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
$profile = Get-ProfileName -Stress:$Stress -FalseSharing:$FalseSharing -SmallFastPath:$SmallFastPath -ReleaseBreakdown:$ReleaseBreakdown -ReleasePressureMatrix:$ReleasePressureMatrix -SlabEmptyChurn:$SlabEmptyChurn -HeapClose:$HeapClose -MmapLocality:$MmapLocality -ControlAllocatorChurn:$ControlAllocatorChurn -LiveBlockLookup:$LiveBlockLookup -TreapAdversarial:$TreapAdversarial -MprotectMultiVma:$MprotectMultiVma -MunmapMultiVma:$MunmapMultiVma -SpinlockThresholds:$SpinlockThresholds -MetadataFootprint:$MetadataFootprint -SlabSlotSharing:$SlabSlotSharing -SlabSlotAb:$SlabSlotAb -Gate32Bit:$Gate32Bit -MprotectStressPages $MprotectStressPages -MprotectLatencySamples $MprotectLatencySamples
if ($ProfileNameOverride.Length -gt 0) {
    $profile = $ProfileNameOverride
} elseif ($Diagnostics -and $DiagnosticsTiming -eq "off") {
    $profile = $profile -replace '-quick$', '-counter-only-quick'
    if ($profile -notmatch '-counter-only-quick$') {
        $profile += '-counter-only'
    }
}
$resolvedOutputDir = Join-Path $Root $OutputDir
New-Item -ItemType Directory -Force -Path $resolvedOutputDir | Out-Null

$runs = @()
$sampleRecords = @()
$sampleLatencyRecords = @()
$sampleMetadataRecords = @()
$records = @()
$latencyRecords = @()
$diagnosticRecords = @()
$metadataRecords = @()
$comparisons = @()

if ($runWindows) {
    for ($sampleIndex = 1; $sampleIndex -le $Samples; ++$sampleIndex) {
        $run = Invoke-WindowsBenchmark $Root $BuildDir $Configuration -Stress:$Stress -FalseSharing:$FalseSharing -SmallFastPath:$SmallFastPath -ReleaseBreakdown:$ReleaseBreakdown -ReleasePressureMatrix:$ReleasePressureMatrix -SlabEmptyChurn:$SlabEmptyChurn -HeapClose:$HeapClose -MmapLocality:$MmapLocality -ControlAllocatorChurn:$ControlAllocatorChurn -LiveBlockLookup:$LiveBlockLookup -TreapAdversarial:$TreapAdversarial -MprotectMultiVma:$MprotectMultiVma -MunmapMultiVma:$MunmapMultiVma -SpinlockThresholds:$SpinlockThresholds -MetadataFootprint:$MetadataFootprint -SlabSlotSharing:$SlabSlotSharing -SlabSlotAb:$SlabSlotAb -Gate32Bit:$Gate32Bit -Win32:$windowsIsWin32 -MprotectStressPages $MprotectStressPages -MprotectLatencySamples $MprotectLatencySamples -Diagnostics:$Diagnostics -DiagnosticsTiming $DiagnosticsTiming -SkipBuild:($SkipBuild -or $sampleIndex -gt 1) -SampleIndex $sampleIndex -TimeoutSeconds $TimeoutSeconds
        $sampleSuffix = if ($Samples -gt 1) { "-sample$sampleIndex" } else { "" }
        $logPath = Join-Path $resolvedOutputDir "bench-$timestamp-$windowsPlatformLabel-$profile$sampleSuffix.log"
        Set-Content -Path $logPath -Value $run.Stdout -Encoding UTF8
        if ($run.Stderr.Length -gt 0) {
            Set-Content -Path (Join-Path $resolvedOutputDir "bench-$timestamp-$windowsPlatformLabel-$profile$sampleSuffix.err.log") -Value $run.Stderr -Encoding UTF8
        }
        $parsed = Add-SampleIndex `
            -Records @(Parse-BenchmarkOutput $run.Stdout $windowsPlatformLabel $profile $run.Command) `
            -SampleIndex $sampleIndex
        $diagParsed = Add-SampleIndex `
            -Records @(Parse-BenchmarkDiagnostics $run.Stdout $windowsPlatformLabel $profile) `
            -SampleIndex $sampleIndex
        $latencyParsed = Add-SampleIndex `
            -Records @(Parse-BenchmarkLatencies $run.Stdout $windowsPlatformLabel $profile) `
            -SampleIndex $sampleIndex
        $metadataParsed = Add-SampleIndex `
            -Records @(Parse-BenchmarkMetadata $run.Stdout $windowsPlatformLabel $profile) `
            -SampleIndex $sampleIndex
        $sampleRecords += $parsed
        $sampleLatencyRecords += $latencyParsed
        $sampleMetadataRecords += $metadataParsed
        $diagnosticRecords += $diagParsed
        $metadataRecords += $metadataParsed
        $runs += [pscustomobject]@{
            platform = $windowsPlatformLabel
            profile = $profile
            sample_index = $sampleIndex
            exit_code = $run.ExitCode
            command = $run.Command
            stdout_log = $logPath
            benchmark_count = @($parsed).Count
            latency_count = @($latencyParsed).Count
            diagnostic_count = @($diagParsed).Count
            metadata_count = @($metadataParsed).Count
            binary_sha256 = $run.binary_sha256
        }
    }
}

if ($runWsl) {
    for ($sampleIndex = 1; $sampleIndex -le $Samples; ++$sampleIndex) {
        $run = Invoke-WslBenchmark $WslProject $WslBuildDir $Configuration -WslDistro $WslDistro -Stress:$Stress -FalseSharing:$FalseSharing -SmallFastPath:$SmallFastPath -ReleaseBreakdown:$ReleaseBreakdown -ReleasePressureMatrix:$ReleasePressureMatrix -SlabEmptyChurn:$SlabEmptyChurn -HeapClose:$HeapClose -MmapLocality:$MmapLocality -ControlAllocatorChurn:$ControlAllocatorChurn -LiveBlockLookup:$LiveBlockLookup -TreapAdversarial:$TreapAdversarial -MprotectMultiVma:$MprotectMultiVma -MunmapMultiVma:$MunmapMultiVma -SpinlockThresholds:$SpinlockThresholds -MetadataFootprint:$MetadataFootprint -SlabSlotSharing:$SlabSlotSharing -SlabSlotAb:$SlabSlotAb -Gate32Bit:$Gate32Bit -WslI386:$wslIsI386 -MprotectStressPages $MprotectStressPages -MprotectLatencySamples $MprotectLatencySamples -Diagnostics:$Diagnostics -DiagnosticsTiming $DiagnosticsTiming -SkipBuild:($SkipBuild -or $sampleIndex -gt 1) -SampleIndex $sampleIndex -TimeoutSeconds $TimeoutSeconds
        $sampleSuffix = if ($Samples -gt 1) { "-sample$sampleIndex" } else { "" }
        $logPath = Join-Path $resolvedOutputDir "bench-$timestamp-$wslPlatformLabel-$profile$sampleSuffix.log"
        Set-Content -Path $logPath -Value $run.Stdout -Encoding UTF8
        if ($run.Stderr.Length -gt 0) {
            Set-Content -Path (Join-Path $resolvedOutputDir "bench-$timestamp-$wslPlatformLabel-$profile$sampleSuffix.err.log") -Value $run.Stderr -Encoding UTF8
        }
        $parsed = Add-SampleIndex `
            -Records @(Parse-BenchmarkOutput $run.Stdout $wslPlatformLabel $profile $run.Command) `
            -SampleIndex $sampleIndex
        $diagParsed = Add-SampleIndex `
            -Records @(Parse-BenchmarkDiagnostics $run.Stdout $wslPlatformLabel $profile) `
            -SampleIndex $sampleIndex
        $latencyParsed = Add-SampleIndex `
            -Records @(Parse-BenchmarkLatencies $run.Stdout $wslPlatformLabel $profile) `
            -SampleIndex $sampleIndex
        $metadataParsed = Add-SampleIndex `
            -Records @(Parse-BenchmarkMetadata $run.Stdout $wslPlatformLabel $profile) `
            -SampleIndex $sampleIndex
        $sampleRecords += $parsed
        $sampleLatencyRecords += $latencyParsed
        $sampleMetadataRecords += $metadataParsed
        $diagnosticRecords += $diagParsed
        $metadataRecords += $metadataParsed
        $runs += [pscustomobject]@{
            platform = $wslPlatformLabel
            profile = $profile
            sample_index = $sampleIndex
            exit_code = $run.ExitCode
            command = $run.Command
            stdout_log = $logPath
            benchmark_count = @($parsed).Count
            latency_count = @($latencyParsed).Count
            diagnostic_count = @($diagParsed).Count
            metadata_count = @($metadataParsed).Count
            binary_sha256 = $run.binary_sha256
        }
    }
}

$records = if (@($sampleRecords).Count -eq 0) {
    @()
} elseif ($Samples -gt 1) {
    New-AggregatedBenchmarkResults -Records @($sampleRecords) `
        -Method $SampleAggregation
} else {
    $sampleRecords
}

$latencyRecords = if (@($sampleLatencyRecords).Count -eq 0) {
    @()
} elseif ($Samples -gt 1) {
    New-AggregatedBenchmarkLatencies -Records @($sampleLatencyRecords) `
        -Method $SampleAggregation
} else {
    $sampleLatencyRecords
}

if (@($records).Count -eq 0 -and
    (-not $MetadataFootprint -or @($metadataRecords).Count -eq 0)) {
    throw "No benchmark result lines were parsed."
}

$provenanceInputs = Get-BenchmarkProvenanceInputs $Root
$binaryFingerprints = [ordered]@{}
foreach ($group in @($runs | Group-Object platform)) {
    $hashes = @($group.Group.binary_sha256 | Sort-Object -Unique)
    if ($hashes.Count -ne 1) {
        throw "Benchmark binary changed between samples for platform '$($group.Name)'."
    }
    $binaryFingerprints[$group.Name] = $hashes[0]
}
$currentProvenance = [pscustomobject][ordered]@{
    schema = $BenchmarkProvenanceSchema
    source_sha256 = $provenanceInputs.source_sha256
    workload_sha256 = $provenanceInputs.workload_sha256
    source_file_count = $provenanceInputs.source_file_count
    workload_file_count = $provenanceInputs.workload_file_count
    configuration = $Configuration
    profile = $profile
    diagnostics_enabled = [bool]$Diagnostics
    diagnostics_timing_enabled = [bool]($Diagnostics -and
                                         $DiagnosticsTiming -eq "on")
    workload_contract = New-BenchmarkWorkloadContract -Profile $profile
    binaries = [pscustomobject]$binaryFingerprints
}

$resolvedBaselinePath = ""
$baselineExists = $false
$baselineMissing = $false
$baselineIncompatible = $false
$baselineLegacy = $false
$baselineSourceChanged = $false
$baselineWorkloadSourceChanged = $false
$baselineBinaryChanged = $false
$baselineProvenanceMigrated = $false
$baselineIncompatibilityReasons = @()
$comparisonStatus = if ($NoRegressionBaseline) {
    "same-run-relative"
} else {
    "not-requested"
}
$comparisonSkipped = $true
$comparisonSkipReason = if ($NoRegressionBaseline) {
    "Cross-run absolute comparison is disabled; profile-local same-run relative thresholds are authoritative."
} else {
    "No BaselinePath was provided."
}
$metadataOnlyResults = @($records).Count -eq 0 -and @($metadataRecords).Count -ne 0
if ($BaselinePath.Length -gt 0) {
    if ([System.IO.Path]::IsPathRooted($BaselinePath)) {
        $resolvedBaselinePath = $BaselinePath
    } else {
        $resolvedBaselinePath = Join-Path $Root $BaselinePath
    }

    if ($metadataOnlyResults -and -not (Test-Path $resolvedBaselinePath) -and
        -not $WriteBaseline) {
        $comparisonStatus = "skipped-metadata-only"
        $comparisonSkipped = $true
        $comparisonSkipReason =
            "No benchmark result records were produced; metadata thresholds are checked without a regression baseline."
    } elseif (Test-Path $resolvedBaselinePath) {
        $baselineExists = $true
        $baseline = Get-Content $resolvedBaselinePath -Raw | ConvertFrom-Json
        $baselineProvenanceProperty = $baseline.PSObject.Properties["provenance"]
        if ($null -eq $baselineProvenanceProperty -and
            -not $RequireBaselineProvenance) {
            $baselineLegacy = $true
            $baselineIncompatibilityReasons = if ($WriteBaseline) {
                @("legacy baseline without provenance cannot be overwritten")
            } else {
                @()
            }
        } else {
            $baselineIncompatibilityReasons = @(
                Get-ProvenanceMismatchReasons `
                    -Current $currentProvenance `
                    -Baseline $baselineProvenanceProperty.Value)
            $baselineSourceChanged =
                [string]$baselineProvenanceProperty.Value.source_sha256 -cne
                [string]$currentProvenance.source_sha256
            $baselineWorkloadSourceChanged =
                [string]$baselineProvenanceProperty.Value.workload_sha256 -cne
                [string]$currentProvenance.workload_sha256
            $baselineProvenanceMigrated =
                [int]$baselineProvenanceProperty.Value.schema -eq 2 -and
                $baselineIncompatibilityReasons.Count -eq 0
            foreach ($property in $currentProvenance.binaries.PSObject.Properties) {
                $baselineBinary =
                    $baselineProvenanceProperty.Value.binaries.PSObject.Properties[$property.Name]
                if ($null -ne $baselineBinary -and
                    [string]$baselineBinary.Value -cne [string]$property.Value) {
                    $baselineBinaryChanged = $true
                }
            }
        }
        if ($baselineIncompatibilityReasons.Count -ne 0) {
            $baselineIncompatible = $true
            $comparisonStatus = "incompatible-baseline"
            $comparisonSkipped = $true
            $comparisonSkipReason =
                "Baseline provenance does not match the current build/workload."
        } else {
            $comparisonStatus = "compared"
            $comparisonSkipped = $false
            $comparisonSkipReason = ""
            $comparisons = Compare-BenchmarkResults `
                -Current @($records) `
                -Baseline @($baseline.results) `
                -ThresholdPercent $RegressionThresholdPercent `
                -ThresholdRules @($thresholdRules)
        }
    } elseif (-not $WriteBaseline -and -not $AllowMissingBaseline) {
        throw "BaselinePath does not exist: $resolvedBaselinePath"
    } else {
        $baselineMissing = $true
        $comparisonStatus = "skipped-baseline-missing"
        $comparisonSkipped = $true
        $comparisonSkipReason = if ($WriteBaseline) {
            "Baseline was missing before this WriteBaseline run; no previous regression comparison was possible."
        } else {
            "Baseline missing and AllowMissingBaseline was supplied; regression comparison was skipped."
        }
    }
}
if ($NoRegressionBaseline -and @($sameRunThresholdRules).Count -gt 0) {
    $comparisons = Compare-SameRunRelativeResults `
        -Current @($records) `
        -Rules @($sameRunThresholdRules)
    $comparisonStatus = "same-run-relative"
    $comparisonSkipped = $false
    $comparisonSkipReason = ""
}

$jsonPath = Join-Path $resolvedOutputDir "bench-$timestamp-$Platform-$profile.json"
$baselineBootstrapCommand = ""
$baselineBootstrap = $null
if ($baselineMissing) {
    $bootstrapArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $PSScriptRoot "run_benchmarks.ps1"),
        "-Platform", $Platform,
        "-BuildDir", $BuildDir,
        "-WslBuildDir", $WslBuildDir,
        "-WslProject", $WslProject,
        "-Configuration", $Configuration,
        "-OutputDir", $OutputDir,
        "-BaselinePath", $resolvedBaselinePath,
        "-RegressionThresholdPercent", ([string]$RegressionThresholdPercent),
        "-Samples", ([string]$Samples),
        "-SampleAggregation", $SampleAggregation,
        "-TimeoutSeconds", ([string]$TimeoutSeconds)
    )
    if ($WslDistro.Length -gt 0) {
        $bootstrapArgs += @("-WslDistro", $WslDistro)
    }
    if ($ThresholdConfigPath.Length -gt 0) {
        $bootstrapArgs += @("-ThresholdConfigPath", $ThresholdConfigPath)
    }
    if ($DiagnosticThresholdConfigPath.Length -gt 0) {
        $bootstrapArgs += @(
            "-DiagnosticThresholdConfigPath",
            $DiagnosticThresholdConfigPath
        )
    }
    if ($LatencyThresholdConfigPath.Length -gt 0) {
        $bootstrapArgs += @("-LatencyThresholdConfigPath", $LatencyThresholdConfigPath)
    }
    if ($MetadataThresholdConfigPath.Length -gt 0) {
        $bootstrapArgs += @(
            "-MetadataThresholdConfigPath",
            $MetadataThresholdConfigPath
        )
    }
    if ($ProfileNameOverride.Length -gt 0) {
        $bootstrapArgs += @("-ProfileNameOverride", $ProfileNameOverride)
    }
    if ($Stress) { $bootstrapArgs += "-Stress" }
    if ($FalseSharing) { $bootstrapArgs += "-FalseSharing" }
    if ($SmallFastPath) { $bootstrapArgs += "-SmallFastPath" }
    if ($ReleaseBreakdown) { $bootstrapArgs += "-ReleaseBreakdown" }
    if ($ReleasePressureMatrix) { $bootstrapArgs += "-ReleasePressureMatrix" }
    if ($SlabEmptyChurn) { $bootstrapArgs += "-SlabEmptyChurn" }
    if ($HeapClose) { $bootstrapArgs += "-HeapClose" }
    if ($MmapLocality) { $bootstrapArgs += "-MmapLocality" }
    if ($ControlAllocatorChurn) { $bootstrapArgs += "-ControlAllocatorChurn" }
    if ($LiveBlockLookup) { $bootstrapArgs += "-LiveBlockLookup" }
    if ($TreapAdversarial) { $bootstrapArgs += "-TreapAdversarial" }
    if ($MprotectMultiVma) { $bootstrapArgs += "-MprotectMultiVma" }
    if ($MunmapMultiVma) { $bootstrapArgs += "-MunmapMultiVma" }
    if ($SpinlockThresholds) { $bootstrapArgs += "-SpinlockThresholds" }
    if ($MetadataFootprint) { $bootstrapArgs += "-MetadataFootprint" }
    if ($SlabSlotSharing) { $bootstrapArgs += "-SlabSlotSharing" }
    if ($SlabSlotAb) { $bootstrapArgs += "-SlabSlotAb" }
    if ($Gate32Bit) { $bootstrapArgs += "-Gate32Bit" }
    if ($MprotectStressPages -gt 0) {
        $bootstrapArgs += @(
            "-MprotectStressPages",
            ([string]$MprotectStressPages)
        )
    }
    if ($MprotectLatencySamples -gt 0) {
        $bootstrapArgs += @(
            "-MprotectLatencySamples",
            ([string]$MprotectLatencySamples)
        )
    }
    if ($Diagnostics) { $bootstrapArgs += "-Diagnostics" }
    if ($DiagnosticsTiming -ne "on") {
        $bootstrapArgs += @("-DiagnosticsTiming", $DiagnosticsTiming)
    }
    if ($RequireBaselineProvenance) {
        $bootstrapArgs += "-RequireBaselineProvenance"
    }
    if ($SkipBuild) { $bootstrapArgs += "-SkipBuild" }
    $bootstrapArgs += "-WriteBaseline"
    $baselineBootstrapCommand = "powershell " + (Join-DisplayCommand $bootstrapArgs)
    $baselineBootstrap = [pscustomobject]@{
        baseline_path = $resolvedBaselinePath
        artifact_path = $jsonPath
        write_baseline = [bool]$WriteBaseline
        allow_missing_baseline = [bool]$AllowMissingBaseline
        command = $baselineBootstrapCommand
        note = "Benchmark baselines under artifacts are machine-local unless maintainers intentionally publish a calibrated baseline."
    }
}

$failedComparisons = @($comparisons | Where-Object {
    $_.status -eq "regression" -or
    $_.status -eq "invalid-baseline" -or
    $_.status -eq "missing-baseline"
})
$failedComparisons = @($failedComparisons | Sort-Object `
    @{ Expression = { Get-FailedComparisonSortValue $_ }; Descending = $true },
    platform, profile, name)
$comparisonSummary = New-ComparisonSummary `
    -Comparisons @($comparisons) `
    -FailedComparisons @($failedComparisons) `
    -Status $comparisonStatus `
    -Skipped $comparisonSkipped `
    -SkipReason $comparisonSkipReason `
    -BaselineMissing $baselineMissing
$diagnosticComparisons = Compare-DiagnosticThresholds `
    -Records @($diagnosticRecords) `
    -Rules @($diagnosticThresholdRules)
$failedDiagnosticComparisons = @($diagnosticComparisons | Where-Object {
    $_.status -ne "ok"
})
$failedDiagnosticComparisons = @($failedDiagnosticComparisons | Sort-Object `
    status, platform, profile, name, key, sample_index)
$diagnosticComparisonSummary = New-DiagnosticComparisonSummary `
    -Comparisons @($diagnosticComparisons) `
    -FailedComparisons @($failedDiagnosticComparisons)
$latencyComparisons = Compare-LatencyThresholds `
    -Records @($latencyRecords) `
    -Rules @($latencyThresholdRules)
$failedLatencyComparisons = @($latencyComparisons | Where-Object {
    $_.status -ne "ok"
})
$failedLatencyComparisons = @($failedLatencyComparisons | Sort-Object `
    status, platform, profile, name, key, sample_index)
$latencyComparisonSummary = New-LatencyComparisonSummary `
    -Comparisons @($latencyComparisons) `
    -FailedComparisons @($failedLatencyComparisons)
$metadataComparisons = Compare-MetadataThresholds `
    -Records @($metadataRecords) `
    -Rules @($metadataThresholdRules)
$failedMetadataComparisons = @($metadataComparisons | Where-Object {
    $_.status -ne "ok"
})
$failedMetadataComparisons = @($failedMetadataComparisons | Sort-Object `
    status, platform, profile, name, key, sample_index)
$metadataComparisonSummary = New-MetadataComparisonSummary `
    -Comparisons @($metadataComparisons) `
    -FailedComparisons @($failedMetadataComparisons)
$quickSummary = New-QuickBenchmarkSummary -Records @($records)
$baselineValidationPassed =
    @($failedComparisons).Count -eq 0 -and
    @($failedDiagnosticComparisons).Count -eq 0 -and
    @($failedLatencyComparisons).Count -eq 0 -and
    @($failedMetadataComparisons).Count -eq 0
$baselineWillWrite = [bool]($WriteBaseline -and -not $baselineIncompatible -and
                            $baselineValidationPassed)

$result = [pscustomobject]@{
    schema = "uhmas.benchmark.v1"
    timestamp_utc = (Get-Date).ToUniversalTime().ToString("o")
    root = $Root
    configuration = $Configuration
    stress = [bool]$Stress
    false_sharing = [bool]$FalseSharing
    small_fast_path = [bool]$SmallFastPath
    release_breakdown = [bool]$ReleaseBreakdown
    release_pressure_matrix = [bool]$ReleasePressureMatrix
    slab_empty_churn = [bool]$SlabEmptyChurn
    heap_close = [bool]$HeapClose
    mmap_locality = [bool]$MmapLocality
    control_allocator_churn = [bool]$ControlAllocatorChurn
    live_block_lookup = [bool]$LiveBlockLookup
    treap_adversarial = [bool]$TreapAdversarial
    mprotect_multi_vma = [bool]$MprotectMultiVma
    munmap_multi_vma = [bool]$MunmapMultiVma
    spinlock_thresholds = [bool]$SpinlockThresholds
    metadata_footprint = [bool]$MetadataFootprint
    slab_slot_sharing = [bool]$SlabSlotSharing
    slab_slot_ab = [bool]$SlabSlotAb
    gate_32bit = [bool]$Gate32Bit
    mprotect_stress_pages = $MprotectStressPages
    mprotect_latency_samples = $MprotectLatencySamples
    diagnostics_enabled = [bool]$Diagnostics
    diagnostics_timing_enabled = [bool]($Diagnostics -and
                                         $DiagnosticsTiming -eq "on")
    samples = $Samples
    sample_aggregation = $SampleAggregation
    profile = $profile
    baseline_path = $resolvedBaselinePath
    baseline_exists = $baselineExists
    baseline_missing = $baselineMissing
    baseline_incompatible = $baselineIncompatible
    baseline_legacy = $baselineLegacy
    baseline_source_changed = $baselineSourceChanged
    baseline_workload_source_changed = $baselineWorkloadSourceChanged
    baseline_binary_changed = $baselineBinaryChanged
    baseline_provenance_migrated = $baselineProvenanceMigrated
    require_baseline_provenance = [bool]$RequireBaselineProvenance
    baseline_incompatibility_reasons = [object[]]@($baselineIncompatibilityReasons)
    write_baseline = [bool]$WriteBaseline
    baseline_validation_passed = [bool]$baselineValidationPassed
    baseline_written = [bool]$baselineWillWrite
    allow_missing_baseline = [bool]$AllowMissingBaseline
    threshold_config_path = $resolvedThresholdConfigPath
    threshold_rule_count = @($thresholdRules).Count
    same_run_threshold_rule_count = @($sameRunThresholdRules).Count
    diagnostic_threshold_config_path = $resolvedDiagnosticThresholdConfigPath
    diagnostic_threshold_rule_count = @($diagnosticThresholdRules).Count
    latency_threshold_config_path = $resolvedLatencyThresholdConfigPath
    latency_threshold_rule_count = @($latencyThresholdRules).Count
    metadata_threshold_config_path = $resolvedMetadataThresholdConfigPath
    metadata_threshold_rule_count = @($metadataThresholdRules).Count
    regression_threshold_percent = $RegressionThresholdPercent
    regression_mode = if ($NoRegressionBaseline) {
        "same-run-relative"
    } elseif ($BaselinePath.Length -gt 0) {
        "cross-run-baseline"
    } else {
        "not-requested"
    }
    comparison_status = $comparisonStatus
    comparison_skipped = $comparisonSkipped
    comparison_skip_reason = $comparisonSkipReason
    comparison_failed = (@($failedComparisons).Count -gt 0)
    comparison_summary = $comparisonSummary
    baseline_bootstrap = $baselineBootstrap
    comparisons = $comparisons
    failed_comparisons = $failedComparisons
    diagnostic_comparison_failed = (@($failedDiagnosticComparisons).Count -gt 0)
    diagnostic_comparison_summary = $diagnosticComparisonSummary
    diagnostic_comparisons = $diagnosticComparisons
    failed_diagnostic_comparisons = $failedDiagnosticComparisons
    latency_comparison_failed = (@($failedLatencyComparisons).Count -gt 0)
    latency_comparison_summary = $latencyComparisonSummary
    latency_comparisons = $latencyComparisons
    failed_latency_comparisons = $failedLatencyComparisons
    metadata_comparison_failed = (@($failedMetadataComparisons).Count -gt 0)
    metadata_comparison_summary = $metadataComparisonSummary
    metadata_comparisons = $metadataComparisons
    failed_metadata_comparisons = $failedMetadataComparisons
    provenance = $currentProvenance
    runs = $runs
    quick_summary = [object[]]@($quickSummary)
    results = [object[]]@($records)
    sample_results = [object[]]@($sampleRecords)
    latencies = [object[]]@($latencyRecords)
    sample_latencies = [object[]]@($sampleLatencyRecords)
    diagnostics = $diagnosticRecords
    metadata = [object[]]@($metadataRecords)
    sample_metadata = [object[]]@($sampleMetadataRecords)
}

$result | ConvertTo-Json -Depth 8 | Set-Content -Path $jsonPath -Encoding UTF8
if ($baselineWillWrite) {
    if ($resolvedBaselinePath.Length -eq 0) {
        throw "WriteBaseline requires BaselinePath."
    }
    $baselineDir = Split-Path -Parent $resolvedBaselinePath
    if ($baselineDir -and -not (Test-Path $baselineDir)) {
        New-Item -ItemType Directory -Force -Path $baselineDir | Out-Null
    }
    # A regression baseline only needs the aggregated measurements and the
    # provenance needed to prove comparability. Keep full samples,
    # diagnostics, latency and threshold evidence in the immutable run
    # artifact so formal baselines stay reviewable and small.
    $baselineDocument = [pscustomobject][ordered]@{
        schema = "uhmas.benchmark-baseline.v1"
        timestamp_utc = $result.timestamp_utc
        platform = $Platform
        configuration = $Configuration
        profile = $profile
        stress = [bool]$Stress
        samples = $Samples
        sample_aggregation = $SampleAggregation
        source_artifact = $jsonPath
        validation_passed = $true
        provenance = $currentProvenance
        results = [object[]]@($records)
    }
    $baselineDocument | ConvertTo-Json -Depth 8 |
        Set-Content -Path $resolvedBaselinePath -Encoding UTF8
}

Write-Host "Benchmark JSON: $jsonPath"
if ($WriteBaseline) {
    if ($baselineIncompatible) {
        Write-Host "Benchmark baseline was not overwritten: $resolvedBaselinePath"
    } elseif (-not $baselineValidationPassed) {
        Write-Host "Benchmark baseline was not written: validation failed: $resolvedBaselinePath"
    } else {
        Write-Host "Benchmark baseline: $resolvedBaselinePath"
    }
}
if ($baselineMissing) {
    Write-Host "Benchmark regression comparison skipped: baseline missing."
    Write-Host "Baseline path: $resolvedBaselinePath"
    Write-Host "Artifact path: $jsonPath"
    if ($WriteBaseline) {
        if ($baselineWillWrite) {
            Write-Host "Baseline bootstrap: wrote the current machine-local measurements as the baseline."
            Write-Host "Rerun the gate without -AllowMissingBaseline to enforce regression comparison."
        } else {
            Write-Host "Baseline bootstrap rejected: validation failed; the baseline was not written."
        }
    } else {
        Write-Host "Bootstrap with -WriteBaseline when this machine/run is approved:"
        Write-Host "  $baselineBootstrapCommand"
    }
}
if ($baselineIncompatible) {
    Write-Host "Benchmark regression comparison rejected: incompatible baseline provenance."
    Write-Host "Baseline path: $resolvedBaselinePath"
    Write-Host "Artifact path: $jsonPath"
    foreach ($reason in $baselineIncompatibilityReasons) {
        Write-Host "  - $reason"
    }
    Write-Host "Use a new BaselinePath for calibration; the existing baseline will not be overwritten."
}
if ($baselineLegacy) {
    Write-Host "Benchmark warning: legacy baseline has no provenance; comparison was allowed for migration compatibility."
}
foreach ($record in $records) {
    "{0,-10} {1,-22} {2,-38} {3,10:N1} ns/op" -f `
        $record.platform, $record.profile, $record.name, $record.ns_per_op |
        Write-Host
}
if (@($quickSummary).Count -gt 0) {
    Write-Host "Quick benchmark summary:"
    foreach ($item in $quickSummary) {
        $uhmText = Format-NullableDouble $item.uhm_ns_per_op
        if ($null -ne $item.baseline_ns_per_op) {
            $baselineText = Format-NullableDouble $item.baseline_ns_per_op
            $ratioText = Format-NullableDouble $item.uhm_to_baseline_ratio "N2"
            "{0,-10} {1,-22} uhm={2,10} ns/op {3}={4,10} ns/op ratio={5,8}x" -f `
                $item.platform, $item.label, $uhmText, $item.baseline_label,
                $baselineText, $ratioText |
                Write-Host
        } else {
            "{0,-10} {1,-22} uhm={2,10} ns/op" -f `
                $item.platform, $item.label, $uhmText |
                Write-Host
        }
    }
}
if ($baselineIncompatible) {
    throw "Benchmark baseline provenance is incompatible with the current build/workload."
}
if (@($failedComparisons).Count -gt 0) {
    Write-Host "Benchmark comparison summary: $($comparisonSummary.failed) failed / $($comparisonSummary.total) compared."
    foreach ($comparison in $failedComparisons) {
        "{0,-10} {1,-22} {2,-38} {3,-16} base={4,10} current={5,10} delta={6,8}% threshold={7,6}%" -f `
            $comparison.platform, $comparison.profile, $comparison.name,
            $comparison.status,
            (Format-NullableDouble $comparison.baseline_ns_per_op),
            (Format-NullableDouble $comparison.current_ns_per_op),
            (Format-NullableDouble $comparison.delta_percent),
            (Format-NullableDouble $comparison.threshold_percent) |
            Write-Host
    }
    throw "Benchmark regression threshold exceeded for $(@($failedComparisons).Count) result(s)."
}
if (@($failedDiagnosticComparisons).Count -gt 0) {
    Write-Host "Benchmark diagnostic summary: $($diagnosticComparisonSummary.failed) failed / $($diagnosticComparisonSummary.total) checked."
    foreach ($comparison in $failedDiagnosticComparisons) {
        "{0,-10} {1,-22} {2,-38} {3,-30} {4,-24} value={5,8} min={6,8} max={7,8} sample={8}" -f `
            $comparison.platform, $comparison.profile, $comparison.name,
            $comparison.key, $comparison.status,
            (Format-NullableInteger $comparison.current_value),
            (Format-NullableInteger $comparison.min_value),
            (Format-NullableInteger $comparison.max_value),
            (Format-NullableInteger $comparison.sample_index) |
            Write-Host
    }
    throw "Benchmark diagnostic threshold exceeded for $(@($failedDiagnosticComparisons).Count) result(s)."
}
if (@($failedLatencyComparisons).Count -gt 0) {
    Write-Host "Benchmark latency summary: $($latencyComparisonSummary.failed) failed / $($latencyComparisonSummary.total) checked."
    foreach ($comparison in $failedLatencyComparisons) {
        "{0,-10} {1,-22} {2,-38} {3,-16} {4,-20} value={5,8} min={6,8} max={7,8} sample={8}" -f `
            $comparison.platform, $comparison.profile, $comparison.name,
            $comparison.key, $comparison.status,
            (Format-NullableInteger $comparison.current_value),
            (Format-NullableInteger $comparison.min_value),
            (Format-NullableInteger $comparison.max_value),
            (Format-NullableInteger $comparison.sample_index) |
            Write-Host
    }
    throw "Benchmark latency threshold exceeded for $(@($failedLatencyComparisons).Count) result(s)."
}
if (@($failedMetadataComparisons).Count -gt 0) {
    Write-Host "Benchmark metadata summary: $($metadataComparisonSummary.failed) failed / $($metadataComparisonSummary.total) checked."
    foreach ($comparison in $failedMetadataComparisons) {
        "{0,-10} {1,-22} {2,-46} {3,-36} {4,-20} value={5,8} min={6,8} max={7,8} sample={8}" -f `
            $comparison.platform, $comparison.profile, $comparison.name,
            $comparison.key, $comparison.status,
            (Format-NullableInteger $comparison.current_value),
            (Format-NullableInteger $comparison.min_value),
            (Format-NullableInteger $comparison.max_value),
            (Format-NullableInteger $comparison.sample_index) |
            Write-Host
    }
    throw "Benchmark metadata threshold exceeded for $(@($failedMetadataComparisons).Count) result(s)."
}
