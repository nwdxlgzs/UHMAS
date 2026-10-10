#!/usr/bin/env bash
set -u

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="build-wsl-tsan"
TEST_TIMEOUT_SECONDS=60
C_COMPILER="/usr/bin/gcc-10"
CLEAN_FIRST=0

usage() {
    cat <<'EOF'
Usage: scripts/verify_wsl_tsan.sh [options]

Options:
  --project-dir DIR      Project directory inside WSL.
  --build-dir DIR        Build directory relative to project dir.
  --cc PATH              C compiler, default /usr/bin/gcc-10.
  --timeout SECONDS      Per-test timeout, default 60.
  --clean-first          Rebuild with --clean-first.
  -h, --help             Show this help.
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
        --cc)
            [ "$#" -ge 2 ] || { echo "--cc requires a value" >&2; exit 2; }
            C_COMPILER="$2"
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

if [ ! -x "$C_COMPILER" ]; then
    echo "TSAN compiler not found or not executable: $C_COMPILER" >&2
    echo "Install gcc-10/libtsan0 or pass --cc PATH." >&2
    exit 2
fi

run_checked() {
    local label="$1"
    shift
    "$@"
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "WSL TSAN $label failed with exit code $rc" >&2
        exit "$rc"
    fi
}

run_tsan_test() {
    local name="$1"
    local executable="$2"
    local log_name="tsan-${name}.log"
    local rc=0

    cd "$PROJECT_DIR/$BUILD_DIR" || exit 1
    rm -f "$log_name"
    echo "== WSL TSAN: $name =="
    setarch x86_64 -R timeout --kill-after=2s "${TEST_TIMEOUT_SECONDS}s" \
        env TSAN_OPTIONS=halt_on_error=1 "./$executable" >"$log_name" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "== WSL TSAN FAILED: $name =="
        echo "exit code: $rc"
        if [ "$rc" -eq 124 ]; then
            echo "timeout: test exceeded ${TEST_TIMEOUT_SECONDS}s"
        fi
        echo "--- last 200 log lines ---"
        tail -n 200 "$log_name" 2>/dev/null || true
        echo "--- matching processes ---"
        ps -eo pid,ppid,stat,comm,args | grep -E 'uhmas|tsan' | grep -v grep || true
        exit "$rc"
    fi
    if [ -s "$log_name" ]; then
        cat "$log_name"
    fi
}

run_checked configure \
    cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER="$C_COMPILER" \
        -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -fno-pie -pthread" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -no-pie -pthread"

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

run_tsan_test basic uhmas_basic_tests
run_tsan_test platform-vm uhmas_platform_vm_tests

echo "WSL TSAN verification passed. TSAN executables are non-PIE EXEC binaries and ran under setarch x86_64 -R."
