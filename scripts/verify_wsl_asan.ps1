param(
    [string]$WslProjectDir = "",
    [string]$BuildDir = "build-wsl-asan",
    [int]$TestTimeoutSeconds = 30,
    [switch]$CleanFirst,
    [switch]$UseAsanSignalHandlers,
    [switch]$KillStale,
    [switch]$NoKillStale
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

function Invoke-WslAsanTest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Executable
    )

    $logName = "asan-$Name.log"
    $command = @"
cd '$WslProjectDir/$BuildDir' &&
rm -f '$logName' &&
echo '== WSL ASAN: $Name ==' &&
timeout --kill-after=2s ${TestTimeoutSeconds}s env $asanEnv './$Executable' >'$logName' 2>&1
rc=\`$?
if [ \`$rc -ne 0 ]; then
    if [ \`$rc -eq 124 ] || [ \`$rc -eq 137 ]; then
        pkill -KILL -f './$Executable' 2>/dev/null || true
    fi
    echo '== WSL ASAN FAILED: $Name =='
    echo 'exit code:' \`$rc
    if [ \`$rc -eq 124 ]; then
        echo 'timeout: test exceeded ${TestTimeoutSeconds}s'
    fi
    echo '--- last 200 log lines ---'
    tail -n 200 '$logName' 2>/dev/null || true
    echo '--- matching processes ---'
    ps -eo pid,ppid,stat,comm,args | grep -E 'uhmas|asan' | grep -v grep || true
    exit \`$rc
fi
if [ -s '$logName' ]; then
    cat '$logName'
fi
"@

    Invoke-WslChecked $command "ASAN test $Name"
}

function Test-Wsl {
    & wsl.exe true
    if ($LASTEXITCODE -ne 0) {
        throw "WSL is not available or failed to start."
    }
}

Test-Wsl

if ($TestTimeoutSeconds -le 0) {
    throw "TestTimeoutSeconds must be positive."
}

$configure = "cd '$WslProjectDir' && cmake -S . -B '$BuildDir' -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' -DUHMAS_ASAN_NO_PIE=ON"

Invoke-WslChecked $configure "configure"

$buildCommand = "cd '$WslProjectDir' && cmake --build '$BuildDir'"
if ($CleanFirst) {
    $buildCommand = "$buildCommand --clean-first"
}
Invoke-WslChecked $buildCommand "build"

$elfCheck = "cd '$WslProjectDir/$BuildDir' && file ./uhmas_basic_tests ./uhmas_platform_vm_tests && readelf -h ./uhmas_basic_tests | grep -q 'EXEC' && readelf -h ./uhmas_platform_vm_tests | grep -q 'EXEC'"

Invoke-WslChecked $elfCheck "non-PIE ELF check"

$staleCheck = @"
target_dir=\`$(cd '$WslProjectDir/$BuildDir' && pwd -P) || exit 1
ps -eo pid,args |
    awk '/uhmas_(basic|platform)_tests/ && !/awk/ {print \`$1}' |
    while read -r pid; do
        cwd=\`$(readlink -f "/proc/\`$pid/cwd" 2>/dev/null || true)
        if [ "\`$cwd" = "\`$target_dir" ]; then
            ps -p "\`$pid" -o pid,ppid,stat,comm,args --no-headers
        fi
    done
"@
$stale = & wsl.exe bash -lc $staleCheck
if ($LASTEXITCODE -ne 0) {
    throw "WSL stale process check failed."
}
if ($stale) {
    Write-Host "Warning: matching test processes are still running before ASAN verification:"
    $stale | ForEach-Object { Write-Host $_ }
    if ($NoKillStale -and -not $KillStale) {
        throw "Refusing to run WSL ASAN with stale test processes. Re-run without -NoKillStale to clean them automatically."
    }
    if ($KillStale -or -not $NoKillStale) {
        Write-Host "Killing stale WSL ASAN test processes before verification."
        $killCommand = @"
target_dir=\`$(cd '$WslProjectDir/$BuildDir' && pwd -P) || exit 1
stale_pids() {
    ps -eo pid,args |
        awk '/uhmas_(basic|platform)_tests/ && !/awk/ {print \`$1}' |
        while read -r pid; do
            cwd=\`$(readlink -f "/proc/\`$pid/cwd" 2>/dev/null || true)
            if [ "\`$cwd" = "\`$target_dir" ]; then
                echo "\`$pid"
            fi
        done
}
pids=\`$(stale_pids)
if [ -n "\`$pids" ]; then
    kill -TERM \`$pids 2>/dev/null || true
fi
sleep 1
pids=\`$(stale_pids)
if [ -n "\`$pids" ]; then
    kill -KILL \`$pids 2>/dev/null || true
fi
"@
        Invoke-WslChecked $killCommand "kill stale ASAN test processes"
        $stale = & wsl.exe bash -lc $staleCheck
        if ($LASTEXITCODE -ne 0) {
            throw "WSL stale process recheck failed."
        }
        if ($stale) {
            Write-Host "Processes still present after kill attempt:"
            $stale | ForEach-Object { Write-Host $_ }
            throw "Unable to clear stale WSL ASAN test processes."
        }
    }
}

$asanSignalOptions = "handle_segv=0:handle_sigbus=0:handle_sigill=0:handle_sigfpe=0:handle_abort=0"
if ($UseAsanSignalHandlers) {
    $asanSignalOptions = "handle_segv=1:handle_sigbus=1:handle_sigill=1:handle_sigfpe=1:handle_abort=1"
}

$asanEnv = "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:$asanSignalOptions UBSAN_OPTIONS=print_stacktrace=1"
Invoke-WslAsanTest "basic" "uhmas_basic_tests"
Invoke-WslAsanTest "platform-vm" "uhmas_platform_vm_tests"

Write-Host "WSL ASAN/UBSAN verification passed. ASAN executables are non-PIE EXEC binaries."
