param(
    [string]$WslProjectDir = "",
    [string]$BuildDir = "build-wsl-tsan",
    [string]$Compiler = "/usr/bin/gcc-10",
    [int]$TestTimeoutSeconds = 60,
    [switch]$CleanFirst
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "wsl_project_path.ps1")
$WslProjectDir = Resolve-UhmWslProjectPath $WslProjectDir

function Invoke-WslChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [string]$Label = "command"
    )

    & wsl.exe bash -lc $Command
    if ($LASTEXITCODE -ne 0) {
        throw "WSL $Label failed with exit code $LASTEXITCODE`: $Command"
    }
}

& wsl.exe true
if ($LASTEXITCODE -ne 0) {
    throw "WSL is not available or failed to start."
}

if ($TestTimeoutSeconds -le 0) {
    throw "TestTimeoutSeconds must be positive."
}

$args = @(
    "--project-dir '$WslProjectDir'",
    "--build-dir '$BuildDir'",
    "--cc '$Compiler'",
    "--timeout $TestTimeoutSeconds"
)
if ($CleanFirst) {
    $args += "--clean-first"
}

$command = "cd '$WslProjectDir' && bash scripts/verify_wsl_tsan.sh $($args -join ' ')"
Invoke-WslChecked $command "TSAN verification"
