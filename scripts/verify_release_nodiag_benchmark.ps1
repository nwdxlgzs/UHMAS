param(
    [ValidateSet("windows", "wsl", "both")]
    [string]$Platform = "windows",
    [string]$BuildDir = "build-windows-release-nodiag-x64",
    [string]$WslBuildDir = "build-wsl-release-nodiag",
    [string]$WslProject = "",
    [string]$WslDistro = "",
    [string]$Configuration = "Release",
    [string]$OutputDir = "artifacts/benchmarks",
    [string]$BaselinePath = "",
    [string]$ThresholdConfigPath = "",
    [double]$RegressionThresholdPercent = 15.0,
    [int]$Samples = 1,
    [ValidateSet("median", "min")]
    [string]$SampleAggregation = "median",
    [int]$TimeoutSeconds = 240,
    [switch]$SkipBuild,
    [switch]$WriteBaseline,
    [switch]$AllowMissingBaseline
)

$ErrorActionPreference = "Stop"

function Invoke-Native {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Invoke-WslCommand {
    param(
        [string]$Command
    )

    $args = @()
    if ($WslDistro.Length -gt 0) {
        $args += @("-d", $WslDistro)
    }
    $args += @("sh", "-lc", $Command)
    Invoke-Native "wsl" $args
}

function Join-WslPath {
    param(
        [string]$Base,
        [string]$Child
    )

    if ($Base.EndsWith("/")) {
        return "$Base$Child"
    }
    return "$Base/$Child"
}

function Resolve-WindowsDumpbinPath {
    $command = Get-Command "dumpbin.exe" -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $programFilesX86 = [Environment]::GetFolderPath("ProgramFilesX86")
    $vswhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installPath.Length -gt 0) {
            $dumpbin = Get-ChildItem -Path $installPath -Recurse -Filter "dumpbin.exe" -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match "\\Hostx64\\x64\\dumpbin\.exe$" } |
                Select-Object -First 1
            if ($dumpbin) {
                return $dumpbin.FullName
            }
        }
    }

    throw "Cannot find dumpbin.exe for release/no-diagnostics symbol audit"
}

function Get-ReleaseNodiagForbiddenSymbols {
    return @(
        "uhm_diag_now_ns",
        "uhm_diag_begin_ns",
        "uhm_diag_elapsed_ns",
        "uhm_diag_add_elapsed",
        "uhm_diag_add_elapsed_max",
        "uhm_diag_timing_enabled",
        "uhm_diag_thread_cpu_work_now",
        "uhm_diag_close_phase_begin",
        "uhm_diag_close_phase_end",
        "uhm_diag_note_u64_inc",
        "uhm_diag_note_u64_add",
        "uhm_diag_note_max_u64"
    )
}

function Assert-NoForbiddenSymbolText {
    param(
        [string]$Text,
        [string[]]$ForbiddenSymbols,
        [string]$Description
    )

    foreach ($symbol in $ForbiddenSymbols) {
        $match = Select-String -InputObject $Text -Pattern $symbol -SimpleMatch |
            Select-Object -First 1
        if ($match) {
            throw "$Description contains forbidden no-diagnostics symbol '$symbol'"
        }
    }
}

function Assert-WindowsReleaseNodiagSymbols {
    $buildRoot = Join-Path $Root $BuildDir
    $binaryPaths = @(
        (Join-Path $buildRoot "$Configuration\uhmas.lib"),
        (Join-Path $buildRoot "$Configuration\uhmas_benchmarks.exe")
    )
    foreach ($path in $binaryPaths) {
        if (-not (Test-Path $path)) {
            throw "release/no-diagnostics symbol audit missing Windows binary: $path"
        }
    }

    $dumpbin = Resolve-WindowsDumpbinPath
    $forbidden = Get-ReleaseNodiagForbiddenSymbols
    foreach ($path in $binaryPaths) {
        $symbols = & $dumpbin /symbols $path 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "dumpbin failed while auditing $path"
        }
        Assert-NoForbiddenSymbolText $symbols $forbidden "release/no-diagnostics Windows binary $path"
    }
}

function Assert-WslReleaseNodiagSymbols {
    $archivePath = Join-WslPath $WslBuildDir "libuhmas.a"
    $benchmarkPath = Join-WslPath $WslBuildDir "uhmas_benchmarks"
    $forbiddenPattern = (Get-ReleaseNodiagForbiddenSymbols) -join "|"
    $command = (
        "cd '$WslProject' && " +
        "test -f '$archivePath' && test -f '$benchmarkPath' && " +
        "if nm -a '$archivePath' '$benchmarkPath' 2>/dev/null | grep -E '$forbiddenPattern'; " +
        "then exit 1; else exit 0; fi"
    )
    Invoke-WslCommand $command
}

function Get-JsonArrayCount {
    param(
        [object]$Value
    )

    if ($null -eq $Value) {
        return 0
    }
    if ($Value -is [System.Management.Automation.PSCustomObject]) {
        return @($Value.PSObject.Properties).Count
    }
    return @($Value).Count
}

function Assert-JsonBooleanFalse {
    param(
        [object]$Object,
        [string]$PropertyName,
        [string]$Description
    )

    $property = $Object.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        throw "$Description is missing $PropertyName"
    }
    if ($property.Value -ne $false) {
        throw "$Description reports $PropertyName=$($property.Value)"
    }
}

function Assert-JsonBooleanTrue {
    param(
        [object]$Object,
        [string]$PropertyName,
        [string]$Description
    )

    $property = $Object.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        throw "$Description is missing $PropertyName"
    }
    if ($property.Value -ne $true) {
        throw "$Description reports $PropertyName=$($property.Value)"
    }
}

function Assert-JsonStringEquals {
    param(
        [object]$Object,
        [string]$PropertyName,
        [string]$Expected,
        [string]$Description
    )

    $property = $Object.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        throw "$Description is missing $PropertyName"
    }
    if ([string]$property.Value -ne $Expected) {
        throw "$Description has $PropertyName='$($property.Value)', expected '$Expected'"
    }
}

function Assert-JsonCountZero {
    param(
        [object]$Object,
        [string]$PropertyName,
        [string]$Description
    )

    $property = $Object.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        throw "$Description is missing $PropertyName"
    }
    if ([int64]$property.Value -ne 0) {
        throw "$Description reports $PropertyName=$($property.Value)"
    }
}

function Assert-JsonNumericRange {
    param(
        [object]$Object,
        [string]$PropertyName,
        [object]$MinValue,
        [object]$MaxValue,
        [string]$Description
    )

    $property = $Object.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        throw "$Description is missing $PropertyName"
    }
    $value = [int64]$property.Value
    if ($null -ne $MinValue) {
        $min = [int64]$MinValue
        if ($value -lt $min) {
            throw "$Description reports $PropertyName=$value, expected >= $min"
        }
    }
    if ($null -ne $MaxValue) {
        $max = [int64]$MaxValue
        if ($value -gt $max) {
            throw "$Description reports $PropertyName=$value, expected <= $max"
        }
    }
}

function Assert-JsonArrayEmpty {
    param(
        [object]$Object,
        [string]$PropertyName,
        [string]$Description
    )

    $property = $Object.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        throw "$Description is missing $PropertyName"
    }
    $count = Get-JsonArrayCount $property.Value
    if ($count -ne 0) {
        throw "$Description contains $count $PropertyName records"
    }
}

function Assert-JsonStringEmpty {
    param(
        [object]$Object,
        [string]$PropertyName,
        [string]$Description
    )

    $property = $Object.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        throw "$Description is missing $PropertyName"
    }
    if ([string]$property.Value -ne "") {
        throw "$Description has $PropertyName='$($property.Value)'"
    }
}

function Assert-ReleaseNodiagAttributionMetadata {
    param(
        [object]$Json,
        [string]$Description
    )

    $expectedNames = @(
        "attribution uhm small malloc/free",
        "attribution uhm small malloc/free multithread",
        "attribution uhm large malloc/realloc/free",
        "attribution uhm mmap/munmap large",
        "attribution uhm mmap fixed/auto mix",
        "attribution uhm mprotect split/merge cost",
        "attribution uhm malloc/mmap collision pressure",
        "attribution uhm release mixed long pressure"
    )
    $runCount = Get-JsonArrayCount $Json.runs
    if ($runCount -le 0) {
        throw "$Description cannot validate attribution metadata without runs"
    }
    $metadata = @($Json.metadata)
    foreach ($name in $expectedNames) {
        $records = @($metadata | Where-Object { [string]$_.name -eq $name })
        if ($records.Count -ne $runCount) {
            throw "$Description expected $runCount metadata records named '$name', found $($records.Count)"
        }
        foreach ($record in $records) {
            $recordDescription = "$Description metadata '$name' sample $($record.sample_index)"
            Assert-JsonNumericRange $record "operations" 1 $null `
                $recordDescription
            Assert-JsonNumericRange $record "diagnostics_enabled" 0 0 `
                $recordDescription
            Assert-JsonNumericRange $record "heap_check_excluded" 1 1 `
                $recordDescription
            Assert-JsonNumericRange $record "vm_failures" 0 0 `
                $recordDescription
            Assert-JsonNumericRange $record "page_budget_exceeded" 0 0 `
                $recordDescription
            Assert-JsonNumericRange $record "tree_budget_exceeded" 0 0 `
                $recordDescription
        }
    }
}

function Assert-ReleaseNodiagBuildConfigMetadata {
    param(
        [object]$Json,
        [string]$Description
    )

    $runCount = Get-JsonArrayCount $Json.runs
    if ($runCount -le 0) {
        throw "$Description cannot validate build config metadata without runs"
    }
    $metadata = @($Json.metadata)
    $records = @($metadata | Where-Object {
        [string]$_.name -eq "release-build-config"
    })
    if ($records.Count -ne $runCount) {
        throw "$Description expected $runCount metadata records named 'release-build-config', found $($records.Count)"
    }
    foreach ($record in $records) {
        $recordDescription = "$Description metadata 'release-build-config' sample $($record.sample_index)"
        Assert-JsonNumericRange $record "diagnostics_compiled" 0 0 `
            $recordDescription
        Assert-JsonNumericRange $record "test_hooks_compiled" 0 0 `
            $recordDescription
        Assert-JsonNumericRange $record "diagnostics_timing_compiled" 0 0 `
            $recordDescription
    }
}

function Resolve-ArtifactPath {
    param(
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

function Assert-ReleaseNodiagLog {
    param(
        [string]$Path,
        [string]$RunLabel
    )

    $resolvedPath = Resolve-ArtifactPath $Path
    if ($resolvedPath.Length -eq 0) {
        throw "release/no-diagnostics run $RunLabel is missing stdout_log"
    }
    if (-not (Test-Path $resolvedPath)) {
        throw "release/no-diagnostics run $RunLabel stdout_log does not exist: $resolvedPath"
    }

    $diagnosticLine = Select-String -Path $resolvedPath -Pattern '^diag\|' |
        Select-Object -First 1
    if ($diagnosticLine) {
        throw "release/no-diagnostics run $RunLabel emitted raw diagnostic line in $resolvedPath line $($diagnosticLine.LineNumber)"
    }

    $runtimeConfigLine = Select-String -Path $resolvedPath -Pattern '^diagnostics_enabled=0 diagnostics_timing_enabled=0$' | Select-Object -First 1
    if (-not $runtimeConfigLine) {
        throw "release/no-diagnostics run $RunLabel did not report runtime diagnostics disabled in $resolvedPath"
    }

    $buildConfigLine = Select-String -Path $resolvedPath -Pattern '^metadata release-build-config: .*diagnostics_compiled=0 .*test_hooks_compiled=0 .*diagnostics_timing_compiled=0' | Select-Object -First 1
    if (-not $buildConfigLine) {
        throw "release/no-diagnostics run $RunLabel did not report compile-time diagnostics/test-hooks removal in $resolvedPath"
    }

    $statsZeroLine = Select-String -Path $resolvedPath `
        -Pattern '^metadata release-nodiag-stats-zero: .*page_stats_zero=1' |
        Select-Object -First 1
    if (-not $statsZeroLine) {
        throw "release/no-diagnostics run $RunLabel did not report page diagnostic stats zero self-check in $resolvedPath"
    }
}

function Assert-ReleaseNodiagArtifact {
    param(
        [string]$PlatformLabel,
        [datetime]$StartedAt,
        [bool]$BaselineWasMissingAtStart = $false
    )

    $resolvedOutputDir = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
        $OutputDir
    } else {
        Join-Path $Root $OutputDir
    }
    $pattern = "bench-*-$PlatformLabel-release-nodiag-quick.json"
    $artifact = Get-ChildItem -Path $resolvedOutputDir -Filter $pattern -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $StartedAt.AddSeconds(-5) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $artifact) {
        throw "release/no-diagnostics benchmark JSON was not produced for $PlatformLabel"
    }

    $json = Get-Content -Raw -Path $artifact.FullName | ConvertFrom-Json
    if ($json.profile -ne "release-nodiag-quick") {
        throw "release/no-diagnostics JSON has unexpected profile '$($json.profile)'"
    }
    foreach ($profileFlag in @(
        "stress",
        "false_sharing",
        "small_fast_path",
        "release_breakdown",
        "release_pressure_matrix",
        "slab_empty_churn",
        "heap_close",
        "mmap_locality",
        "control_allocator_churn",
        "live_block_lookup",
        "treap_adversarial",
        "mprotect_multi_vma",
        "munmap_multi_vma",
        "spinlock_thresholds",
        "metadata_footprint",
        "slab_slot_sharing",
        "slab_slot_ab",
        "gate_32bit"
    )) {
        Assert-JsonBooleanFalse $json $profileFlag `
            "release/no-diagnostics JSON"
    }
    Assert-JsonBooleanFalse $json "diagnostics_enabled" `
        "release/no-diagnostics JSON"
    Assert-JsonBooleanFalse $json "diagnostics_timing_enabled" `
        "release/no-diagnostics JSON"
    Assert-JsonCountZero $json "diagnostic_threshold_rule_count" `
        "release/no-diagnostics JSON"
    Assert-JsonStringEmpty $json "diagnostic_threshold_config_path" `
        "release/no-diagnostics JSON"
    Assert-JsonBooleanFalse $json "diagnostic_comparison_failed" `
        "release/no-diagnostics JSON"
    Assert-JsonArrayEmpty $json "diagnostics" `
        "release/no-diagnostics JSON"
    Assert-JsonArrayEmpty $json "diagnostic_comparisons" `
        "release/no-diagnostics JSON"
    Assert-JsonArrayEmpty $json "failed_diagnostic_comparisons" `
        "release/no-diagnostics JSON"

    if ($null -eq $json.diagnostic_comparison_summary) {
        throw "release/no-diagnostics JSON is missing diagnostic_comparison_summary"
    }
    Assert-JsonCountZero $json.diagnostic_comparison_summary "total" `
        "release/no-diagnostics diagnostic_comparison_summary"
    Assert-JsonCountZero $json.diagnostic_comparison_summary "failed" `
        "release/no-diagnostics diagnostic_comparison_summary"

    Assert-JsonCountZero $json "metadata_threshold_rule_count" `
        "release/no-diagnostics JSON"
    Assert-JsonStringEmpty $json "metadata_threshold_config_path" `
        "release/no-diagnostics JSON"
    Assert-JsonBooleanFalse $json "metadata_comparison_failed" `
        "release/no-diagnostics JSON"
    if ($null -eq $json.metadata_comparison_summary) {
        throw "release/no-diagnostics JSON is missing metadata_comparison_summary"
    }
    Assert-JsonCountZero $json.metadata_comparison_summary "total" `
        "release/no-diagnostics metadata_comparison_summary"
    Assert-JsonCountZero $json.metadata_comparison_summary "failed" `
        "release/no-diagnostics metadata_comparison_summary"

    if ($BaselinePath.Length -gt 0) {
        if ($BaselineWasMissingAtStart) {
            Assert-JsonStringEquals $json "comparison_status" `
                "skipped-baseline-missing" `
                "release/no-diagnostics JSON"
            Assert-JsonBooleanTrue $json "comparison_skipped" `
                "release/no-diagnostics JSON"
            Assert-JsonBooleanTrue $json "baseline_missing" `
                "release/no-diagnostics JSON"
            if ($null -eq $json.baseline_bootstrap) {
                throw "release/no-diagnostics JSON is missing baseline_bootstrap"
            }
            Assert-JsonStringEquals $json.comparison_summary "status" `
                "skipped-baseline-missing" `
                "release/no-diagnostics comparison_summary"
            Assert-JsonBooleanTrue $json.comparison_summary "skipped" `
                "release/no-diagnostics comparison_summary"
        } else {
            Assert-JsonStringEquals $json "comparison_status" "compared" `
                "release/no-diagnostics JSON"
            Assert-JsonBooleanFalse $json "comparison_skipped" `
                "release/no-diagnostics JSON"
            Assert-JsonBooleanFalse $json "baseline_missing" `
                "release/no-diagnostics JSON"
        }
    }

    if ((Get-JsonArrayCount $json.runs) -eq 0) {
        throw "release/no-diagnostics JSON contains no runs"
    }
    foreach ($run in @($json.runs)) {
        if ($run.profile -ne "release-nodiag-quick") {
            throw "release/no-diagnostics run has unexpected profile '$($run.profile)'"
        }
        if ($run.diagnostic_count -ne 0) {
            throw "release/no-diagnostics run emitted $($run.diagnostic_count) diagnostic records"
        }
        Assert-ReleaseNodiagLog $run.stdout_log `
            "$($run.platform)/sample$($run.sample_index)"
    }
    Assert-ReleaseNodiagAttributionMetadata $json `
        "release/no-diagnostics JSON"
    Assert-ReleaseNodiagBuildConfigMetadata $json `
        "release/no-diagnostics JSON"

    Write-Host "Verified no-diagnostics benchmark JSON: $($artifact.FullName)"
}

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
if ($Platform -eq "wsl" -or $Platform -eq "both") {
    . (Join-Path $PSScriptRoot "wsl_project_path.ps1")
    $WslProject = Resolve-UhmWslProjectPath $WslProject $WslDistro $Root
}

$runner = Join-Path $PSScriptRoot "run_benchmarks.ps1"

if (($Platform -eq "windows" -or $Platform -eq "both") -and -not $SkipBuild) {
    Invoke-Native "cmake" @(
        "-S", $Root,
        "-B", (Join-Path $Root $BuildDir),
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-DUHMAS_BUILD_PROFILE=release",
        "-DUHMAS_BUILD_TESTS=OFF",
        "-DUHMAS_BUILD_BENCHMARKS=ON",
        "-DUHMAS_ENABLE_TEST_HOOKS=OFF",
        "-DUHMAS_ENABLE_DIAGNOSTICS=OFF"
    )
    Invoke-Native "cmake" @(
        "--build", (Join-Path $Root $BuildDir),
        "--config", $Configuration,
        "--target", "uhmas_benchmarks"
    )
}

if (($Platform -eq "wsl" -or $Platform -eq "both") -and -not $SkipBuild) {
    Invoke-WslCommand (
        "cd '$WslProject' && " +
        "cmake -S . -B '$WslBuildDir' " +
        "-DCMAKE_BUILD_TYPE=Release " +
        "-DUHMAS_BUILD_PROFILE=release " +
        "-DUHMAS_BUILD_TESTS=OFF " +
        "-DUHMAS_BUILD_BENCHMARKS=ON " +
        "-DUHMAS_ENABLE_TEST_HOOKS=OFF " +
        "-DUHMAS_ENABLE_DIAGNOSTICS=OFF && " +
        "cmake --build '$WslBuildDir' --target uhmas_benchmarks"
    )
}

if ($Platform -eq "windows" -or $Platform -eq "both") {
    Assert-WindowsReleaseNodiagSymbols
}

if ($Platform -eq "wsl" -or $Platform -eq "both") {
    Assert-WslReleaseNodiagSymbols
}

$benchArgs = @(
    "-Platform", $Platform,
    "-Configuration", $Configuration,
    "-OutputDir", $OutputDir,
    "-ProfileNameOverride", "release-nodiag-quick",
    "-SkipBuild",
    "-Samples", "$Samples",
    "-SampleAggregation", $SampleAggregation,
    "-TimeoutSeconds", "$TimeoutSeconds"
)

if ($Platform -eq "windows" -or $Platform -eq "both") {
    $benchArgs += @("-BuildDir", $BuildDir)
}
if ($Platform -eq "wsl" -or $Platform -eq "both") {
    $benchArgs += @("-WslBuildDir", $WslBuildDir, "-WslProject", $WslProject)
    if ($WslDistro.Length -gt 0) {
        $benchArgs += @("-WslDistro", $WslDistro)
    }
}
if ($BaselinePath.Length -gt 0) {
    $benchArgs += @("-BaselinePath", $BaselinePath)
}
if ($ThresholdConfigPath.Length -gt 0) {
    $benchArgs += @("-ThresholdConfigPath", $ThresholdConfigPath)
}
if ($WriteBaseline) {
    $benchArgs += "-WriteBaseline"
}
if ($AllowMissingBaseline) {
    $benchArgs += "-AllowMissingBaseline"
}
$benchArgs += @("-RegressionThresholdPercent", ([string]$RegressionThresholdPercent))

$runnerArgs = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", $runner
) + $benchArgs

$baselineWasMissingAtStart = $false
if ($BaselinePath.Length -gt 0) {
    $baselineWasMissingAtStart = -not (Test-Path (Resolve-ArtifactPath $BaselinePath))
}

$benchmarkStartedAt = Get-Date
Invoke-Native "powershell" $runnerArgs
Assert-ReleaseNodiagArtifact `
    -PlatformLabel $Platform `
    -StartedAt $benchmarkStartedAt `
    -BaselineWasMissingAtStart $baselineWasMissingAtStart
