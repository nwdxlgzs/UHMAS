[CmdletBinding()]
param(
    [switch]$Apply,
    [ValidatePattern('^\d{8}$')]
    [string]$KeepRootBenchmarksOnOrAfter = '20260802',
    [ValidateRange(1, 10)]
    [int]$KeepLatestPerFamily = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$artifactsRoot = Join-Path $repoRoot 'artifacts'
$benchmarkRoot = Join-Path $artifactsRoot 'benchmarks'
$baselineRoot = Join-Path $benchmarkRoot 'baselines'
$repoPrefix = $repoRoot + '\'
$benchmarkPrefix = $benchmarkRoot + '\'

if (-not (Test-Path -LiteralPath (Join-Path $repoRoot '.git'))) {
    throw "Repository root is not a Git worktree: $repoRoot"
}
if (-not (Test-Path -LiteralPath $baselineRoot -PathType Container)) {
    throw "Protected benchmark baseline directory is missing: $baselineRoot"
}

# These directories were created for bounded A/B, smoke, profile, shared-library,
# superseded toolchain experiments, or obsolete generic build/worktree staging.
# The list is deliberately exact: adding a new directory never makes it eligible
# for deletion automatically. Canonical Windows/WSL Release, sanitizer, 32-bit,
# and release-nodiag directories must stay absent from this list.
$buildDeleteNames = @(
    'agent-worktree',
    'build',
    'build-ab-mmap-adjacent-off',
    'build-ab-mmap-adjacent-on',
    'build-ab-mmap-base',
    'build-ab-mmap-cache-bounded-off',
    'build-ab-mmap-cache-bounded-on',
    'build-ab-mmap-exact-off',
    'build-ab-mmap-exact-on',
    'build-ab-mmap-nodiag-base',
    'build-ab-mmap-nodiag-opt',
    'build-ab-mmap-opt',
    'build-ab-mmap-promote-nodiag-off',
    'build-ab-mmap-promote-nodiag-on',
    'build-ab-mmap-promote-off',
    'build-ab-mmap-promote-on',
    'build-ab-mmap-test-base',
    'build-ab-retained-proof-off',
    'build-ab-retained-proof-on',
    'build-hooks-off-tests',
    'build-nohooks',
    'build-profile-invalid',
    'build-profile-release',
    'build-profile-stress',
    'build-release-nodiag',
    'build-release-strict-owner',
    'build-shared',
    'build-windows-release-nodiag-smoke',
    'build-wsl',
    'build-wsl-shared',
    'build-wsl-tsan-gcc10'
)

# These artifact directories contain rejected candidates, superseded baselines,
# smoke runs, or one-off snapshots. The tracked baselines directory is absent by
# construction and is additionally protected below.
$artifactDeleteDirectoryNames = @(
    'baseline-archive',
    'baselines-treap-candidate-20260802',
    'baselines-v2-candidate',
    'candidates',
    'diag-release',
    'false-sharing-no-commit-smoke',
    'false-sharing-start-gate-smoke',
    'footprint-diag',
    'footprint-nodiag',
    'nodiag',
    'nodiag-release',
    'pass4-main-skip',
    'provenance-smoke',
    'slab-single-page-snapshot',
    'small-fast-path-audit',
    'small-fast-path-pop-elision',
    'small-fast-path-pop-one-probe',
    'small-fast-path-push-one-probe'
)

function Assert-DirectChild {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd('\')
    if ((Split-Path -Path $fullPath -Parent) -ne $fullParent) {
        throw "$Description is not a direct child of its approved parent: $fullPath"
    }
    return $fullPath
}

function Get-FilesRecursively {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [IO.DirectoryInfo[]]$Directories
    )

    if ($Directories.Count -eq 0) {
        return @()
    }
    return @($Directories | ForEach-Object {
        Get-ChildItem -LiteralPath $_.FullName -File -Recurse
    })
}

function Get-TotalFileLength {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$Files
    )

    if ($Files.Count -eq 0) {
        return [int64]0
    }
    return [int64](($Files | Measure-Object -Property Length -Sum).Sum)
}

$protected = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$trackedArtifactPaths = @(git -C $repoRoot ls-files artifacts)
if ($LASTEXITCODE -ne 0) {
    throw 'git ls-files failed while collecting protected artifacts'
}
foreach ($relativePath in $trackedArtifactPaths) {
    $fullPath = [IO.Path]::GetFullPath((Join-Path $repoRoot ($relativePath -replace '/', '\')))
    if (Test-Path -LiteralPath $fullPath) {
        [void]$protected.Add($fullPath)
    }
}

$planPath = Join-Path $repoRoot 'PLAN.MD'
$todoPath = Join-Path $repoRoot 'TODO.MD'
$documents = (Get-Content -LiteralPath $planPath -Raw) + (Get-Content -LiteralPath $todoPath -Raw)
$documentReferences = @(
    [regex]::Matches(
        $documents,
        'artifacts[/\\][A-Za-z0-9_./\\-]+?\.(?:json|txt|log|md)',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase
    ) |
        ForEach-Object { $_.Value.Replace('/', '\') } |
        Sort-Object -Unique
)
foreach ($relativePath in $documentReferences) {
    $fullPath = [IO.Path]::GetFullPath((Join-Path $repoRoot $relativePath))
    if (-not $fullPath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Document artifact reference escaped the repository: $relativePath"
    }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Document artifact reference is missing before cleanup: $relativePath"
    }
    [void]$protected.Add($fullPath)
}

$rootJson = @(Get-ChildItem -LiteralPath $benchmarkRoot -File -Filter '*.json')
$recentRootJson = @($rootJson | Where-Object {
    $_.Name -match '^bench-(\d{8})-(\d{6})-' -and
    $matches[1] -ge $KeepRootBenchmarksOnOrAfter
})
foreach ($file in $recentRootJson) {
    [void]$protected.Add($file.FullName)
}

$parsedRootJson = @($rootJson | ForEach-Object {
    if ($_.Name -match '^bench-(\d{8})-(\d{6})-(.+)\.json$') {
        [pscustomobject]@{ File = $_; Family = $matches[3] }
    }
})
$latestRootJson = @($parsedRootJson |
    Group-Object -Property Family |
    ForEach-Object {
        $_.Group |
            Sort-Object -Property { $_.File.Name } -Descending |
            Select-Object -First $KeepLatestPerFamily
    })
foreach ($entry in $latestRootJson) {
    [void]$protected.Add($entry.File.FullName)
}

$protectedRootJson = @($rootJson | Where-Object { $protected.Contains($_.FullName) })
$protectedLogPrefixes = @($protectedRootJson | ForEach-Object {
    [IO.Path]::GetFileNameWithoutExtension($_.Name)
})

$deleteFiles = New-Object 'System.Collections.Generic.List[System.IO.FileInfo]'
foreach ($file in $rootJson) {
    if (-not $protected.Contains($file.FullName)) {
        $deleteFiles.Add($file)
    }
}
foreach ($file in @(Get-ChildItem -LiteralPath $benchmarkRoot -File -Filter '*.log')) {
    $keep = $false
    foreach ($prefix in $protectedLogPrefixes) {
        if ($file.Name.StartsWith($prefix + '-', [StringComparison]::OrdinalIgnoreCase)) {
            $keep = $true
            break
        }
    }
    if (-not $keep) {
        $deleteFiles.Add($file)
    }
}
foreach ($file in @(Get-ChildItem -LiteralPath $benchmarkRoot -File -Filter '*.txt')) {
    if (-not $protected.Contains($file.FullName)) {
        $deleteFiles.Add($file)
    }
}

$buildTargets = New-Object 'System.Collections.Generic.List[System.IO.DirectoryInfo]'
foreach ($name in $buildDeleteNames) {
    $path = Assert-DirectChild -Path (Join-Path $repoRoot $name) -Parent $repoRoot -Description 'Build deletion target'
    if (Test-Path -LiteralPath $path -PathType Container) {
        $buildTargets.Add((Get-Item -LiteralPath $path))
    }
}

$artifactDirectoryTargets = New-Object 'System.Collections.Generic.List[System.IO.DirectoryInfo]'
foreach ($name in $artifactDeleteDirectoryNames) {
    $path = Assert-DirectChild -Path (Join-Path $benchmarkRoot $name) -Parent $benchmarkRoot -Description 'Artifact deletion target'
    if ($path -eq $baselineRoot) {
        throw "Protected baseline directory selected for deletion: $path"
    }
    if (Test-Path -LiteralPath $path -PathType Container) {
        foreach ($protectedPath in $protected) {
            if ($protectedPath.StartsWith($path + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "Protected artifact is inside a deletion directory: $protectedPath"
            }
        }
        $artifactDirectoryTargets.Add((Get-Item -LiteralPath $path))
    }
}

foreach ($file in $deleteFiles) {
    $fullPath = [IO.Path]::GetFullPath($file.FullName)
    if (-not $fullPath.StartsWith($benchmarkPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Artifact file escaped the benchmark root: $fullPath"
    }
    if ((Split-Path -Path $fullPath -Parent) -ne $benchmarkRoot) {
        throw "Root artifact deletion target is not directly inside the benchmark root: $fullPath"
    }
    if ($protected.Contains($fullPath)) {
        throw "Protected artifact selected for deletion: $fullPath"
    }
}

$buildFiles = @(Get-FilesRecursively -Directories $buildTargets.ToArray())
$artifactDirectoryFiles = @(Get-FilesRecursively -Directories $artifactDirectoryTargets.ToArray())
$rootArtifactBytes = Get-TotalFileLength -Files @($deleteFiles)
$artifactDirectoryBytes = Get-TotalFileLength -Files $artifactDirectoryFiles
$buildBytes = Get-TotalFileLength -Files $buildFiles
$totalBytes = $rootArtifactBytes + $artifactDirectoryBytes + $buildBytes

$summary = [pscustomobject]@{
    Mode = if ($Apply) { 'apply' } else { 'dry-run' }
    BuildDirectories = $buildTargets.Count
    BuildFiles = $buildFiles.Count
    ArtifactDirectories = $artifactDirectoryTargets.Count
    RootArtifactFiles = $deleteFiles.Count
    NestedArtifactFiles = $artifactDirectoryFiles.Count
    TotalArtifactFiles = $deleteFiles.Count + $artifactDirectoryFiles.Count
    ProtectedTrackedArtifacts = $trackedArtifactPaths.Count
    ProtectedDocumentReferences = $documentReferences.Count
    ProtectedRootBenchmarkJson = $protectedRootJson.Count
    ReclaimMiB = [math]::Round($totalBytes / 1MB, 1)
}

$summary | Format-List

if (-not $Apply) {
    Write-Host 'Dry run only. Re-run with -Apply to delete the listed classes.'
    exit 0
}

foreach ($directory in $buildTargets) {
    Remove-Item -LiteralPath $directory.FullName -Recurse -Force
}
foreach ($directory in $artifactDirectoryTargets) {
    Remove-Item -LiteralPath $directory.FullName -Recurse -Force
}
foreach ($file in $deleteFiles) {
    if (Test-Path -LiteralPath $file.FullName -PathType Leaf) {
        Remove-Item -LiteralPath $file.FullName -Force
    }
}

Write-Host 'Workspace cleanup completed.'
