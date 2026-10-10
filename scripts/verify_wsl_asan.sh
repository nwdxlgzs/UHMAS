#!/usr/bin/env bash
set -u

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="build-wsl-asan"
TEST_TIMEOUT_SECONDS=30
CLEAN_FIRST=0
USE_ASAN_SIGNAL_HANDLERS=0
NO_KILL_STALE=0

usage() {
    cat <<'EOF'
Usage: scripts/verify_wsl_asan.sh [options]

Options:
  --project-dir DIR              Project directory inside WSL.
  --build-dir DIR                Build directory relative to project dir.
  --timeout SECONDS              Per-test timeout, default 30.
  --clean-first                  Rebuild with --clean-first.
  --use-asan-signal-handlers     Let ASAN handle fatal signals for diagnosis.
  --no-kill-stale                Fail if stale uhmas test processes exist.
  -h, --help                     Show this help.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --project-dir)
            [ "$#" -ge 2 ] || { echo "--project-dir requires a value" >&2; exit 2; }
            PROJECT_DIR="$2"
            shift 2
            ;;
        --build-dir)
            [ "$#" -ge 2 ] || { echo "--build-dir requires a value" >&2; exit 2; }
            BUILD_DIR="$2"
            shift 2
            ;;
        --timeout)
            [ "$#" -ge 2 ] || { echo "--timeout requires a value" >&2; exit 2; }
            TEST_TIMEOUT_SECONDS="$2"
            shift 2
            ;;
        --clean-first)
            CLEAN_FIRST=1
            shift
            ;;
        --use-asan-signal-handlers)
            USE_ASAN_SIGNAL_HANDLERS=1
            shift
            ;;
        --no-kill-stale)
            NO_KILL_STALE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$TEST_TIMEOUT_SECONDS" in
    ''|*[!0-9]*)
        echo "--timeout must be a positive integer" >&2
        exit 2
        ;;
esac
if [ "$TEST_TIMEOUT_SECONDS" -le 0 ]; then
    echo "--timeout must be a positive integer" >&2
    exit 2
fi

run_checked() {
    local label="$1"
    shift
    "$@"
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "WSL ASAN $label failed with exit code $rc" >&2
        exit "$rc"
    fi
}

stale_processes() {
    local target_dir

    target_dir="$(cd "$PROJECT_DIR/$BUILD_DIR" && pwd -P)" || exit 1
    ps -eo pid,args |
        awk '/uhmas_(basic|platform)_tests/ && !/awk/ {print $1}' |
        while read -r pid; do
            local cwd
            cwd="$(readlink -f "/proc/$pid/cwd" 2>/dev/null || true)"
            if [ "$cwd" = "$target_dir" ]; then
                ps -p "$pid" -o pid,ppid,stat,comm,args --no-headers
            fi
        done
}

kill_stale_processes() {
    local pids

    pids="$(stale_processes | awk '{print $1}')"
    if [ -n "$pids" ]; then
        kill -TERM $pids 2>/dev/null || true
    fi
    sleep 1
    pids="$(stale_processes | awk '{print $1}')"
    if [ -n "$pids" ]; then
        kill -KILL $pids 2>/dev/null || true
    fi
}

run_asan_test() {
    local name="$1"
    local executable="$2"
    local log_name="asan-${name}.log"
    local rc=0

    cd "$PROJECT_DIR/$BUILD_DIR" || exit 1
    rm -f "$log_name"
    echo "== WSL ASAN: $name =="
    timeout --kill-after=2s "${TEST_TIMEOUT_SECONDS}s" \
        env $ASAN_ENV "./$executable" >"$log_name" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
            pkill -KILL -f "./$executable" 2>/dev/null || true
        fi
        echo "== WSL ASAN FAILED: $name =="
        echo "exit code: $rc"
        if [ "$rc" -eq 124 ]; then
            echo "timeout: test exceeded ${TEST_TIMEOUT_SECONDS}s"
        fi
        echo "--- last 200 log lines ---"
        tail -n 200 "$log_name" 2>/dev/null || true
        echo "--- matching processes ---"
        ps -eo pid,ppid,stat,comm,args | grep -E 'uhmas|asan' | grep -v grep || true
        exit "$rc"
    fi
    if [ -s "$log_name" ]; then
        cat "$log_name"
    fi
}

run_checked configure \
    cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS=-fsanitize=address,undefined\ -fno-omit-frame-pointer \
        -DUHMAS_ASAN_NO_PIE=ON

if [ "$CLEAN_FIRST" -ne 0 ]; then
    run_checked build cmake --build "$PROJECT_DIR/$BUILD_DIR" --clean-first
else
    run_checked build cmake --build "$PROJECT_DIR/$BUILD_DIR"
fi

cd "$PROJECT_DIR/$BUILD_DIR" || exit 1
file ./uhmas_basic_tests ./uhmas_platform_vm_tests
run_checked "basic non-PIE check" \
    bash -c "LC_ALL=C readelf -h ./uhmas_basic_tests | grep -q 'Type:.*EXEC'"
run_checked "platform non-PIE check" \
    bash -c "LC_ALL=C readelf -h ./uhmas_platform_vm_tests | grep -q 'Type:.*EXEC'"

STALE="$(stale_processes)"
if [ -n "$STALE" ]; then
    echo "Warning: matching test processes are still running before ASAN verification:"
    echo "$STALE"
    if [ "$NO_KILL_STALE" -ne 0 ]; then
        echo "Refusing to run WSL ASAN with stale test processes." >&2
        exit 1
    fi
    echo "Killing stale WSL ASAN test processes before verification."
    kill_stale_processes
    STALE="$(stale_processes)"
    if [ -n "$STALE" ]; then
        echo "Processes still present after kill attempt:" >&2
        echo "$STALE" >&2
        exit 1
    fi
fi

ASAN_SIGNAL_OPTIONS="handle_segv=0:handle_sigbus=0:handle_sigill=0:handle_sigfpe=0:handle_abort=0"
if [ "$USE_ASAN_SIGNAL_HANDLERS" -ne 0 ]; then
    ASAN_SIGNAL_OPTIONS="handle_segv=1:handle_sigbus=1:handle_sigill=1:handle_sigfpe=1:handle_abort=1"
fi
ASAN_ENV="ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:${ASAN_SIGNAL_OPTIONS} UBSAN_OPTIONS=print_stacktrace=1"

run_asan_test basic uhmas_basic_tests
run_asan_test platform-vm uhmas_platform_vm_tests

echo "WSL ASAN/UBSAN verification passed. ASAN executables are non-PIE EXEC binaries."
