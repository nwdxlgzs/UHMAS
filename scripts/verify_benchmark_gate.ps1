param(
    [ValidateSet("windows", "wsl", "both", "win32", "wsl-i386", "32bit-both")]
    [string]$Platform = "windows",
    [ValidateSet("quick", "stress", "false-sharing", "small-fast-path", "release-breakdown", "release-pressure-matrix", "slab-empty-churn", "heap-close", "mmap-locality", "control-allocator-churn", "live-block", "treap-adversarial", "mprotect-multi-vma", "munmap-multi-vma", "spinlock-thresholds", "spinlock-thresholds-quick", "spinlock-thresholds-stress", "metadata-footprint", "slab-slot-sharing", "slab-slot-ab", "mprotect-multi-vma-matrix", "mprotect-multi-vma-latency", "32bit-gate", "release-nodiag", "quick+focused", "all")]
    [string]$ProfileSet = "quick",
    [string]$BuildDir = "build-release",
    [string]$WslBuildDir = "build-wsl-release",
    [string]$WslDistro = "",
    [string]$WslProject = "",
    [string]$Configuration = "Release",
    [string]$OutputDir = "artifacts/benchmarks",
    [string]$BaselineDir = "artifacts/benchmarks/baselines",
    [string]$ThresholdConfigPath = "",
    [string]$DiagnosticThresholdConfigPath = "",
    [string]$LatencyThresholdConfigPath = "",
    [string]$MetadataThresholdConfigPath = "",
    [double]$RegressionThresholdPercent = 15.0,
    [int]$RegressionRetries = -1,
    [int]$Samples = 3,
    [ValidateSet("median", "min")]
    [string]$SampleAggregation = "median",
    [int]$TimeoutSeconds = 300,
    [switch]$SkipBuild,
    [switch]$WriteBaseline,
    [switch]$AllowMissingBaseline
)

$ErrorActionPreference = "Stop"

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

$script:BenchmarkGateBaselineSkips = @()
$script:BenchmarkGateRegressionRetries = @()

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

function ConvertTo-ProcessArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }

    $result = '"'
    $backslashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') {
            ++$backslashes
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
        [int]$TimeoutSeconds = 300
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $File
    $psi.Arguments = Join-ProcessArguments $Arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

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
    }
}

function Register-BenchmarkBaselineState {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Label,
        [Parameter(Mandatory = $true)]
        [string]$BaselinePath,
        [Parameter(Mandatory = $true)]
        [string]$RunnerScriptPath,
        [Parameter(Mandatory = $true)]
        [string[]]$RunnerArguments
    )

    if (Test-Path $BaselinePath) {
        return
    }
    if (-not $WriteBaseline -and -not $AllowMissingBaseline) {
        return
    }

    $bootstrapRunnerArguments = @($RunnerArguments | Where-Object {
        $_ -ne "-AllowMissingBaseline" -and $_ -ne "-WriteBaseline"
    })
    $bootstrapCommandArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $RunnerScriptPath
    ) + $bootstrapRunnerArguments + "-WriteBaseline"
    $bootstrapCommand = "powershell " + (
        Join-DisplayCommand $bootstrapCommandArgs
    )

    $script:BenchmarkGateBaselineSkips += [pscustomobject]@{
        label = $Label
        baseline_path = $BaselinePath
        write_baseline = [bool]$WriteBaseline
        allow_missing_baseline = [bool]$AllowMissingBaseline
        bootstrap_command = $bootstrapCommand
    }

    Write-Host "Benchmark regression baseline missing for ${Label}: $BaselinePath"
    if ($WriteBaseline) {
        Write-Host "Baseline bootstrap: this run will write a machine-local baseline; regression comparison is skipped for this profile."
    } else {
        Write-Host "Regression comparison skipped for this profile because -AllowMissingBaseline was supplied."
        Write-Host "Bootstrap with -WriteBaseline when this machine/run is approved:"
        Write-Host "  $bootstrapCommand"
    }
}

function Write-BenchmarkGateCompletion {
    $missingBaselineCount = @($script:BenchmarkGateBaselineSkips).Count
    $retryCount = @($script:BenchmarkGateRegressionRetries).Count
    if ($retryCount -gt 0) {
        Write-Host "Benchmark gate recovered from $retryCount transient regression comparison failure(s) after retry."
    }
    if ($missingBaselineCount -eq 0) {
        Write-Host "Benchmark gate passed."
        return
    }

    $writeBaselineCount = @($script:BenchmarkGateBaselineSkips | Where-Object {
        $_.write_baseline
    }).Count
    $allowMissingOnlyCount = $missingBaselineCount - $writeBaselineCount
    if ($allowMissingOnlyCount -gt 0) {
        Write-Host "Benchmark gate completed with regression comparison skipped for $allowMissingOnlyCount missing baseline(s)."
    }
    if ($writeBaselineCount -gt 0) {
        Write-Host "Benchmark baseline bootstrap completed for $writeBaselineCount missing baseline(s)."
    }
    Write-Host "This is not a full regression comparison pass until the gate is rerun with baselines present."
}

function Test-BenchmarkRegressionFailureOutput {
    param([AllowEmptyCollection()][object[]]$Output)

    $text = (@($Output) | ForEach-Object { $_.ToString() }) -join "`n"
    return $text -match "Benchmark regression threshold exceeded"
}

function Invoke-BenchmarkRunnerWithRetry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Label,
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,
        [Parameter(Mandatory = $true)]
        [string[]]$BenchmarkArgs,
        [int]$MaxRegressionRetries = 0,
        [int]$RunnerTimeoutSeconds = 300
    )

    $attempt = 0
    while ($true) {
        $processResult = Invoke-CapturedProcess `
            -File "powershell" `
            -Arguments (@(
                "-NoProfile",
                "-ExecutionPolicy", "Bypass",
                "-File", $ScriptPath
            ) + $BenchmarkArgs) `
            -TimeoutSeconds $RunnerTimeoutSeconds
        if ($processResult.Stdout.Length -gt 0) {
            Write-Host $processResult.Stdout.TrimEnd()
        }
        if ($processResult.Stderr.Length -gt 0) {
            Write-Host $processResult.Stderr.TrimEnd()
        }
        if ($processResult.ExitCode -eq 0) {
            return
        }
        $output = @($processResult.Stdout, $processResult.Stderr)
        if ($attempt -lt $MaxRegressionRetries -and
            (Test-BenchmarkRegressionFailureOutput -Output @($output))) {
            ++$attempt
            $script:BenchmarkGateRegressionRetries += [pscustomobject]@{
                label = $Label
                attempt = $attempt
                max_retries = $MaxRegressionRetries
            }
            Write-Host "Benchmark regression comparison failed for ${Label}; retrying ($attempt/$MaxRegressionRetries) to filter scheduler/VM noise."
            continue
        }
        throw "benchmark gate failed for $Label"
    }
}

function Invoke-GateBenchmark {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RunPlatform,
        [Parameter(Mandatory = $true)]
        [string]$ProfileName,
        [Parameter(Mandatory = $true)]
        [hashtable]$ProfileSwitches,
        [string]$DefaultThresholdConfigPath = "",
        [string]$DefaultDiagnosticThresholdConfigPath = "",
        [string]$DefaultLatencyThresholdConfigPath = "",
        [string]$DefaultMetadataThresholdConfigPath = "",
        [int]$DefaultRegressionRetries = 0,
        [switch]$RelativeOnlyRegression,
        [switch]$RequireBaselineProvenance
    )

    $root = Split-Path -Parent $PSScriptRoot
    $baselinePath = Join-Path $root (Join-Path $BaselineDir "baseline-$RunPlatform-$ProfileName.json")
    $scriptPath = Join-Path $PSScriptRoot "run_benchmarks.ps1"
    $effectiveThresholdConfigPath =
        if ($ThresholdConfigPath.Length -gt 0) {
            $ThresholdConfigPath
        } else {
            $DefaultThresholdConfigPath
        }
    $effectiveDiagnosticThresholdConfigPath =
        if ($DiagnosticThresholdConfigPath.Length -gt 0) {
            $DiagnosticThresholdConfigPath
        } else {
            $DefaultDiagnosticThresholdConfigPath
        }
    $effectiveLatencyThresholdConfigPath =
        if ($LatencyThresholdConfigPath.Length -gt 0) {
            $LatencyThresholdConfigPath
        } else {
            $DefaultLatencyThresholdConfigPath
        }
    $effectiveMetadataThresholdConfigPath =
        if ($MetadataThresholdConfigPath.Length -gt 0) {
            $MetadataThresholdConfigPath
        } else {
            $DefaultMetadataThresholdConfigPath
        }
    $effectiveRegressionRetries =
        if ($RegressionRetries -ge 0) {
            $RegressionRetries
        } else {
            $DefaultRegressionRetries
        }
    $benchmarkArgs = @(
        "-Platform", $RunPlatform,
        "-BuildDir", $BuildDir,
        "-WslBuildDir", $WslBuildDir,
        "-Configuration", $Configuration,
        "-OutputDir", $OutputDir,
        "-Samples", ([string]$Samples),
        "-SampleAggregation", $SampleAggregation,
        "-TimeoutSeconds", ([string]$TimeoutSeconds)
    )
    if ($WslProject.Length -gt 0) {
        $benchmarkArgs += @("-WslProject", $WslProject)
    }
    if ($RelativeOnlyRegression) {
        $benchmarkArgs += "-NoRegressionBaseline"
        $effectiveRegressionRetries = 0
    } else {
        $benchmarkArgs += @(
            "-BaselinePath", $baselinePath,
            "-RegressionThresholdPercent", ([string]$RegressionThresholdPercent)
        )
    }
    if ($effectiveThresholdConfigPath.Length -gt 0) {
        $benchmarkArgs += @("-ThresholdConfigPath", $effectiveThresholdConfigPath)
    }
    if ($WslDistro.Length -gt 0) {
        $benchmarkArgs += @("-WslDistro", $WslDistro)
    }
    if ($effectiveDiagnosticThresholdConfigPath.Length -gt 0) {
        $benchmarkArgs += @("-DiagnosticThresholdConfigPath", $effectiveDiagnosticThresholdConfigPath)
    }
    if ($effectiveLatencyThresholdConfigPath.Length -gt 0) {
        $benchmarkArgs += @("-LatencyThresholdConfigPath", $effectiveLatencyThresholdConfigPath)
    }
    if ($effectiveMetadataThresholdConfigPath.Length -gt 0) {
        $benchmarkArgs += @("-MetadataThresholdConfigPath", $effectiveMetadataThresholdConfigPath)
    }
    if ($SkipBuild) {
        $benchmarkArgs += "-SkipBuild"
    }
    if ($WriteBaseline -and -not $RelativeOnlyRegression) {
        $benchmarkArgs += "-WriteBaseline"
    }
    if ($AllowMissingBaseline -and -not $RelativeOnlyRegression) {
        $benchmarkArgs += "-AllowMissingBaseline"
    }
    if ($RequireBaselineProvenance -and -not $RelativeOnlyRegression) {
        $benchmarkArgs += "-RequireBaselineProvenance"
    }
    foreach ($name in $ProfileSwitches.Keys) {
        $value = $ProfileSwitches[$name]
        if ($value -is [bool] -or $value -is [switch]) {
            if ($value) {
                $benchmarkArgs += "-$name"
            }
        } elseif ($null -ne $value -and [string]$value -ne "") {
            $benchmarkArgs += "-$name"
            $benchmarkArgs += [string]$value
        }
    }

    if (-not $RelativeOnlyRegression) {
        Register-BenchmarkBaselineState `
            -Label "$RunPlatform/$ProfileName" `
            -BaselinePath $baselinePath `
            -RunnerScriptPath $scriptPath `
            -RunnerArguments $benchmarkArgs
    }

    Invoke-Step "Benchmark gate $RunPlatform/$ProfileName" {
        Invoke-BenchmarkRunnerWithRetry `
            -Label "$RunPlatform/$ProfileName" `
            -ScriptPath $scriptPath `
            -BenchmarkArgs $benchmarkArgs `
            -MaxRegressionRetries $effectiveRegressionRetries `
            -RunnerTimeoutSeconds ($TimeoutSeconds * ($Samples + 1) + 120)
    }
}

if ($Samples -le 0) {
    throw "Samples must be positive."
}
if ($RegressionThresholdPercent -lt 0.0) {
    throw "RegressionThresholdPercent must be non-negative."
}
if ($RegressionRetries -lt -1) {
    throw "RegressionRetries must be -1 or greater."
}

if ($ProfileSet -eq "release-nodiag") {
    if ($Platform -ne "windows" -and $Platform -ne "wsl" -and
        $Platform -ne "both") {
        throw "release-nodiag supports only windows, wsl, or both."
    }

    $releaseNodiagBuildDir = if ($PSBoundParameters.ContainsKey("BuildDir")) {
        $BuildDir
    } else {
        "build-windows-release-nodiag-x64"
    }
    $releaseNodiagWslBuildDir =
        if ($PSBoundParameters.ContainsKey("WslBuildDir")) {
            $WslBuildDir
        } else {
            "build-wsl-release-nodiag"
    }
    $scriptPath = Join-Path $PSScriptRoot "verify_release_nodiag_benchmark.ps1"
    $root = Split-Path -Parent $PSScriptRoot
    $releaseNodiagPlatforms = if ($Platform -eq "both") {
        @("windows", "wsl")
    } else {
        @($Platform)
    }

    foreach ($runPlatform in $releaseNodiagPlatforms) {
        $baselinePath = Join-Path $root (
            Join-Path $BaselineDir "baseline-$runPlatform-release-nodiag-quick.json"
        )
        $releaseNodiagArgs = @(
            "-Platform", $runPlatform,
            "-BuildDir", $releaseNodiagBuildDir,
            "-WslBuildDir", $releaseNodiagWslBuildDir,
            "-Configuration", $Configuration,
            "-OutputDir", $OutputDir,
            "-BaselinePath", $baselinePath,
            "-RegressionThresholdPercent", ([string]$RegressionThresholdPercent),
            "-Samples", ([string]$Samples),
            "-SampleAggregation", $SampleAggregation,
            "-TimeoutSeconds", ([string]$TimeoutSeconds)
        )
        if ($WslProject.Length -gt 0) {
            $releaseNodiagArgs += @("-WslProject", $WslProject)
        }
        if ($ThresholdConfigPath.Length -gt 0) {
            $releaseNodiagArgs += @("-ThresholdConfigPath", $ThresholdConfigPath)
        }
        if ($WslDistro.Length -gt 0) {
            $releaseNodiagArgs += @("-WslDistro", $WslDistro)
        }
        if ($SkipBuild) {
            $releaseNodiagArgs += "-SkipBuild"
        }
        if ($WriteBaseline) {
            $releaseNodiagArgs += "-WriteBaseline"
        }
        if ($AllowMissingBaseline) {
            $releaseNodiagArgs += "-AllowMissingBaseline"
        }

        Register-BenchmarkBaselineState `
            -Label "$runPlatform/release-nodiag-quick" `
            -BaselinePath $baselinePath `
            -RunnerScriptPath $scriptPath `
            -RunnerArguments $releaseNodiagArgs

        Invoke-Step "Benchmark gate $runPlatform/release-nodiag-quick" {
            & powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath @releaseNodiagArgs
            if ($LASTEXITCODE -ne 0) {
                throw "release/no-diagnostics benchmark gate failed for $runPlatform"
            }
        }
    }
    Write-BenchmarkGateCompletion
    return
}

$platforms = if ($Platform -eq "both") {
    @("windows", "wsl")
} elseif ($Platform -eq "32bit-both") {
    @("win32", "wsl-i386")
} else {
    @($Platform)
}

$profiles = @()
if ($ProfileSet -eq "quick" -or $ProfileSet -eq "quick+focused" -or
    $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "quick"
        Switches = @{}
        ThresholdConfigPath = "scripts/regression-thresholds-quick.json"
        DiagnosticThresholdConfigPath = ""
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-quick-attribution.json"
        DefaultRegressionRetries = 1
        RequireBaselineProvenance = $true
    }
}
if ($ProfileSet -eq "stress" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "stress"
        Switches = @{ Stress = $true }
    }
}
if ($ProfileSet -eq "false-sharing" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "false-sharing-quick"
        Switches = @{ FalseSharing = $true; Diagnostics = $true }
        ThresholdConfigPath = "scripts/regression-thresholds-false-sharing.json"
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-false-sharing.json"
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-false-sharing.json"
        DefaultRegressionRetries = 1
        RequireBaselineProvenance = $true
    }
}
if ($ProfileSet -eq "small-fast-path" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "small-fast-path-quick"
        Switches = @{ SmallFastPath = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-small-fast-path.json"
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-small-fast-path.json"
    }
}
if ($ProfileSet -eq "release-breakdown" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "release-breakdown-quick"
        Switches = @{ ReleaseBreakdown = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-release-breakdown.json"
        LatencyThresholdConfigPath = "scripts/latency-thresholds-release-breakdown.json"
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-release-breakdown.json"
        DefaultRegressionRetries = 1
        RequireBaselineProvenance = $true
    }
}
if ($ProfileSet -eq "release-pressure-matrix" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "release-pressure-matrix-quick"
        Switches = @{ ReleasePressureMatrix = $true; Diagnostics = $true }
        ThresholdConfigPath = "scripts/regression-thresholds-release-pressure-matrix.json"
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-release-pressure-matrix.json"
        LatencyThresholdConfigPath = "scripts/latency-thresholds-release-pressure-matrix.json"
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-release-pressure-matrix.json"
        DefaultRegressionRetries = 1
        RequireBaselineProvenance = $true
    }
}
if ($ProfileSet -eq "slab-empty-churn" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "slab-empty-churn-quick"
        Switches = @{ SlabEmptyChurn = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-slab-empty-churn.json"
        LatencyThresholdConfigPath = "scripts/latency-thresholds-slab-empty-churn.json"
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-slab-empty-churn.json"
    }
}
if ($ProfileSet -eq "heap-close" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "heap-close-quick"
        Switches = @{ HeapClose = $true; Diagnostics = $true }
        ThresholdConfigPath = "scripts/regression-thresholds-heap-close.json"
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-heap-close.json"
        LatencyThresholdConfigPath = "scripts/latency-thresholds-heap-close.json"
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-heap-close.json"
        DefaultRegressionRetries = 0
        RelativeOnlyRegression = $true
    }
}
if ($ProfileSet -eq "mmap-locality" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "mmap-locality-quick"
        Switches = @{ MmapLocality = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-mmap-locality.json"
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-mmap-locality.json"
    }
}
if ($ProfileSet -eq "control-allocator-churn" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "control-allocator-churn-quick"
        Switches = @{ ControlAllocatorChurn = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-control-allocator-churn.json"
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = ""
    }
}
if ($ProfileSet -eq "live-block" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "live-block-quick"
        Switches = @{ LiveBlockLookup = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-live-block.json"
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = ""
    }
}
if ($ProfileSet -eq "treap-adversarial" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "treap-adversarial-quick"
        Switches = @{ TreapAdversarial = $true; Diagnostics = $true }
        ThresholdConfigPath = "scripts/regression-thresholds-treap-adversarial.json"
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-treap-adversarial.json"
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = ""
        DefaultRegressionRetries = 0
        RelativeOnlyRegression = $true
    }
}
if ($ProfileSet -eq "mprotect-multi-vma" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "mprotect-multi-vma-quick"
        Switches = @{ MprotectMultiVma = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-mprotect-matrix.json"
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-mprotect-matrix.json"
    }
}
if ($ProfileSet -eq "munmap-multi-vma" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "munmap-multi-vma-quick"
        Switches = @{ MunmapMultiVma = $true; Diagnostics = $true }
        ThresholdConfigPath = "scripts/regression-thresholds-munmap-multi-vma.json"
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-munmap-multi-vma.json"
        LatencyThresholdConfigPath = "scripts/latency-thresholds-munmap-multi-vma.json"
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-munmap-multi-vma.json"
        DefaultRegressionRetries = 1
        RequireBaselineProvenance = $true
    }
}
if ($ProfileSet -eq "spinlock-thresholds" -or
    $ProfileSet -eq "spinlock-thresholds-quick" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "spinlock-thresholds-quick"
        Switches = @{ SpinlockThresholds = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-spinlock-thresholds.json"
        LatencyThresholdConfigPath = "scripts/latency-thresholds-spinlock-thresholds.json"
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-spinlock-thresholds.json"
        DefaultRegressionRetries = 0
        RelativeOnlyRegression = $true
    }
}
if ($ProfileSet -eq "spinlock-thresholds" -or
    $ProfileSet -eq "spinlock-thresholds-stress" -or
    $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "spinlock-thresholds-stress"
        Switches = @{
            SpinlockThresholds = $true
            Stress = $true
            Diagnostics = $true
        }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-spinlock-thresholds.json"
        LatencyThresholdConfigPath = "scripts/latency-thresholds-spinlock-thresholds.json"
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-spinlock-thresholds.json"
        DefaultRegressionRetries = 0
        RelativeOnlyRegression = $true
    }
}
if ($ProfileSet -eq "metadata-footprint" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "metadata-footprint-quick"
        Switches = @{ MetadataFootprint = $true }
        DiagnosticThresholdConfigPath = ""
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-metadata-footprint.json"
    }
}
if ($ProfileSet -eq "slab-slot-sharing" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "slab-slot-sharing-quick"
        Switches = @{ SlabSlotSharing = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = ""
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-slab-slot-sharing.json"
    }
}
if ($ProfileSet -eq "slab-slot-ab" -or
    $ProfileSet -eq "quick+focused" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "slab-slot-ab-quick"
        Switches = @{ SlabSlotAb = $true; Diagnostics = $true }
        DiagnosticThresholdConfigPath = ""
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-slab-slot-ab.json"
    }
}
if ($ProfileSet -eq "32bit-gate" -or $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "32bit-gate-quick"
        Switches = @{
            Gate32Bit = $true
            Diagnostics = $true
        }
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-32bit-gate.json"
        LatencyThresholdConfigPath = ""
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-32bit-gate.json"
    }
}
if ($ProfileSet -eq "mprotect-multi-vma-matrix" -or
    $ProfileSet -eq "all") {
    foreach ($pageCount in @(64, 128, 256)) {
        $profiles += [pscustomobject]@{
            Name = "mprotect-multi-vma-p$pageCount-quick"
            Switches = @{
                MprotectMultiVma = $true
                MprotectStressPages = $pageCount
                Diagnostics = $true
            }
            ThresholdConfigPath = "scripts/regression-thresholds-mprotect-matrix.json"
            DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-mprotect-matrix.json"
            LatencyThresholdConfigPath = "scripts/latency-thresholds-mprotect-matrix.json"
            MetadataThresholdConfigPath = "scripts/metadata-thresholds-mprotect-matrix.json"
            DefaultRegressionRetries = 1
            RequireBaselineProvenance = $true
        }
    }
}
if ($ProfileSet -eq "mprotect-multi-vma-latency" -or
    $ProfileSet -eq "all") {
    $profiles += [pscustomobject]@{
        Name = "mprotect-multi-vma-p256-lat128-quick"
        Switches = @{
            MprotectMultiVma = $true
            MprotectStressPages = 256
            MprotectLatencySamples = 128
            Diagnostics = $true
        }
        ThresholdConfigPath = "scripts/regression-thresholds-mprotect-matrix.json"
        DiagnosticThresholdConfigPath = "scripts/diagnostic-thresholds-mprotect-matrix.json"
        LatencyThresholdConfigPath = "scripts/latency-thresholds-mprotect-matrix.json"
        MetadataThresholdConfigPath = "scripts/metadata-thresholds-mprotect-matrix.json"
        DefaultRegressionRetries = 1
        RequireBaselineProvenance = $true
    }
}

foreach ($runPlatform in $platforms) {
    foreach ($profile in $profiles) {
        $defaultDiagnostic = if ($null -ne $profile.PSObject.Properties["DiagnosticThresholdConfigPath"]) {
            [string]$profile.DiagnosticThresholdConfigPath
        } else {
            ""
        }
        $defaultThreshold = if ($null -ne $profile.PSObject.Properties["ThresholdConfigPath"]) {
            [string]$profile.ThresholdConfigPath
        } else {
            ""
        }
        $defaultLatency = if ($null -ne $profile.PSObject.Properties["LatencyThresholdConfigPath"]) {
            [string]$profile.LatencyThresholdConfigPath
        } else {
            ""
        }
        $defaultMetadata = if ($null -ne $profile.PSObject.Properties["MetadataThresholdConfigPath"]) {
            [string]$profile.MetadataThresholdConfigPath
        } else {
            ""
        }
        $defaultRegressionRetries = if ($null -ne $profile.PSObject.Properties["DefaultRegressionRetries"]) {
            [int]$profile.DefaultRegressionRetries
        } else {
            0
        }
        $requireBaselineProvenance =
            $null -ne $profile.PSObject.Properties["RequireBaselineProvenance"] -and
            [bool]$profile.RequireBaselineProvenance
        $relativeOnlyRegression =
            $null -ne $profile.PSObject.Properties["RelativeOnlyRegression"] -and
            [bool]$profile.RelativeOnlyRegression
        Invoke-GateBenchmark `
            $runPlatform `
            $profile.Name `
            $profile.Switches `
            -DefaultThresholdConfigPath $defaultThreshold `
            -DefaultDiagnosticThresholdConfigPath $defaultDiagnostic `
            -DefaultLatencyThresholdConfigPath $defaultLatency `
            -DefaultMetadataThresholdConfigPath $defaultMetadata `
            -DefaultRegressionRetries $defaultRegressionRetries `
            -RelativeOnlyRegression:$relativeOnlyRegression `
            -RequireBaselineProvenance:$requireBaselineProvenance
    }
}

Write-BenchmarkGateCompletion
