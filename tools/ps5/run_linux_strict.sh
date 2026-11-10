#!/usr/bin/env bash
# ==============================================================================
# SwitchRecomp — 253 x86_64 Linux Headless + SWR_STRICT=1 极速定位与调试脚本
#
# 用法（在本机或 253 上均可运行）：
#   1. 从本机一键同步到 253 并编译运行（默认 SWR_STRICT=1，秒级出调用栈与 NVN 普查表）：
#      bash tools/ps5/run_linux_strict.sh
#
#   2. 已在 253 上直接本地运行：
#      bash tools/ps5/run_linux_strict.sh --local
#
#   3. 开启 ASan/UBSan 或切回非严格模式对比：
#      SWR_STRICT=0 bash tools/ps5/run_linux_strict.sh
#      SWR_ASAN=1   bash tools/ps5/run_linux_strict.sh
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD_HOST="${BUILD_HOST:-swizard@192.168.1.253}"
REMOTE_REPO="${REMOTE_REPO:-/data/switchrecomp/repo}"
BUILD_DIR="${BUILD_DIR:-/data/switchrecomp/ps5o2_fixed/swr_bananza_ps5_o2_fixed_build}"
DATA_DIR="${DATA_DIR:-/data/switchrecomp/ps5o2_fixed/PPSA99201/swr_data}"
OUT_DIR="${OUT_DIR:-/data/switchrecomp/linux_strict}"
BIN_NAME="${BIN_NAME:-bananza_headless}"
SWR_STRICT="${SWR_STRICT:-1}"
SWR_ASAN="${SWR_ASAN:-0}"
RUN_SECONDS="${RUN_SECONDS:-25}"

if [[ "${1:-}" == "--local" ]] || [[ -d "/data/switchrecomp/ps5o2_fixed" ]]; then
    echo "=== [253 Native Linux Strict Runner] (SWR_STRICT=${SWR_STRICT}, ASAN=${SWR_ASAN}, timeout=${RUN_SECONDS}s) ==="
    ASAN_FLAG=()
    if [[ "${SWR_ASAN}" == "1" ]]; then
        ASAN_FLAG=(--asan)
    fi
    python3 "${REPO_ROOT}/tools/ps5/linux_headless_build.py" \
        --build-dir "${BUILD_DIR}" \
        --data-dir  "${DATA_DIR}" \
        --out-dir   "${OUT_DIR}" \
        --name      "${BIN_NAME}" \
        "${ASAN_FLAG[@]}"

    EXE="${OUT_DIR}/${BIN_NAME}"
    LOG="${OUT_DIR}/${BIN_NAME}.log"
    GDB_LOG="${OUT_DIR}/gdb_strict_bt.log"
    rm -f "${LOG}" "${GDB_LOG}" "${OUT_DIR}/stage.txt"

    echo "=== Running ${EXE} --strict=${SWR_STRICT} --seconds=${RUN_SECONDS} ==="
    set +e
    if command -v gdb >/dev/null 2>&1; then
        SWR_STRICT="${SWR_STRICT}" timeout --signal=SIGINT "$((RUN_SECONDS + 5))" \
            gdb -batch \
                -ex "set pagination off" \
                -ex "handle SIGUSR1 nostop noprint pass" \
                -ex "run --strict=${SWR_STRICT} --seconds=${RUN_SECONDS}" \
                -ex "echo \n=== GDB CRASH/STOP BACKTRACE ===\n" \
                -ex "bt 24" \
                -ex "info registers" \
                -ex "thread apply all bt 8" \
                --args "${EXE}" > "${GDB_LOG}" 2>&1
        RC=$?
    else
        SWR_STRICT="${SWR_STRICT}" timeout "${RUN_SECONDS}" \
            "${EXE}" "--strict=${SWR_STRICT}" "--seconds=${RUN_SECONDS}" > "${GDB_LOG}" 2>&1
        RC=$?
    fi
    set -e

    echo "=== Run finished (rc=${RC}) ==="
    if [[ -f "${OUT_DIR}/stage.txt" ]]; then
        echo "--- stage.txt ---"
        cat "${OUT_DIR}/stage.txt"
    fi
    if [[ -f "${LOG}" ]]; then
        echo "--- Key Diagnostics from ${LOG} ---"
        grep -E "SwitchRecomp -|stage:|STRICT|nvn: census|nvn\[|Crash|svcBreak|badcall|unresolved" "${LOG}" | tail -n 120 || true
    fi
    if [[ -f "${GDB_LOG}" ]]; then
        echo "--- Tail of ${GDB_LOG} ---"
        tail -n 60 "${GDB_LOG}"
    fi
    exit 0
fi

echo "=== [Step 1] Syncing workspace to ${BUILD_HOST}:${REMOTE_REPO} ==="
rsync -az --delete --exclude='.git' --exclude='runs/' \
    "${REPO_ROOT}/" "${BUILD_HOST}:${REMOTE_REPO}/"

echo "=== [Step 2] Running Linux Headless + SWR_STRICT=${SWR_STRICT} on ${BUILD_HOST} ==="
ssh "${BUILD_HOST}" "SWR_STRICT=${SWR_STRICT} SWR_ASAN=${SWR_ASAN} RUN_SECONDS=${RUN_SECONDS} bash ${REMOTE_REPO}/tools/ps5/run_linux_strict.sh --local"
