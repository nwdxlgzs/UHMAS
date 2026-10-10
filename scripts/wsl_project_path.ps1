function Resolve-UhmWslProjectPath {
    param(
        [string]$ProjectPath = "",
        [string]$Distro = "",
        [string]$WindowsRoot = (Split-Path -Parent $PSScriptRoot)
    )

    if ($ProjectPath.Length -gt 0) {
        return $ProjectPath
    }
    $wslArgs = @()
    if ($Distro.Length -gt 0) {
        $wslArgs += @("-d", $Distro)
    }
    # Execute directly: the default WSL shell can consume Windows backslashes.
    $wslArgs += @("--exec", "wslpath", "-a", "-u", $WindowsRoot.Replace('\', '/'))
    $converted = @(& wsl.exe @wslArgs)
    if ($LASTEXITCODE -ne 0 -or $converted.Count -ne 1 -or
        [string]::IsNullOrWhiteSpace([string]$converted[0])) {
        throw "Cannot derive the WSL checkout path. Set -WslProject or -WslProjectDir explicitly."
    }
    return ([string]$converted[0]).Trim()
}
