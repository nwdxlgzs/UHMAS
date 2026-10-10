param()

$ErrorActionPreference = "Stop"
$runner = Join-Path $PSScriptRoot "run_benchmarks.ps1"

& powershell -NoProfile -ExecutionPolicy Bypass -File $runner `
    -ProvenanceSelfTest
if ($LASTEXITCODE -ne 0) {
    throw "benchmark provenance self-test failed"
}
