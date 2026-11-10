#!/usr/bin/env bash
# ==============================================================================
# SwitchRecomp — 253 x86_64 Linux Headless + SWR_STRICT=1 极速定位与调试脚本
#
# 用法（在本机或 253 上均可运行）：
#   1. 从本机一键同步到 253 并编译运行（默认 SWR_STRICT=1，秒级出调用栈与 NVN 普查表）：
#      bash tools/ps5/run_linux_strict.sh
#      同步方式与 build_deploy.sh 一致：git fetch + git reset --hard origin/ps5
#      —— 因此**本机改动必须先 commit + push**，否则 253 拿到的是 origin 上的旧代码。
#      脚本会在同步前检查未提交改动 / 未推送提交并中止（SKIP_SYNC_CHECK=1 可跳过）。
#
#   2. 已在 253 上直接本地运行（自动识别，也可显式加 --local）：
#      bash tools/ps5/run_linux_strict.sh --local
#
#   2b. 只把 253 代码对齐到 origin/ps5，不编译不运行：
#      bash tools/ps5/run_linux_strict.sh --sync-only
#
#   3. 开启 ASan/UBSan 或切回非严格模式对比：
#      SWR_STRICT=0 bash tools/ps5/run_linux_strict.sh
#      SWR_ASAN=1   bash tools/ps5/run_linux_strict.sh
#
# 主机/路径统一从 <repo>/.env 读取（见 .env.example），不在脚本里硬编码。
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ---- 配置 (主机/端口统一从 <repo>/.env 读取, 见 .env.example) ----
eval "$(python3 "$SCRIPT_DIR/ps5_env.py" --shell)"

BUILD_HOST="${BUILD_HOST:-$DEV_SSH}"
REMOTE_REPO="${REMOTE_REPO:-$DEV_CODE_DIR}"          # 253 上的 git 仓库根
BUILD_DIR="${BUILD_DIR:-$DEV_WORK_ROOT/ps5o2_fixed/swr_bananza_ps5_o2_fixed_build}"
DATA_DIR="${DATA_DIR:-$DEV_WORK_ROOT/ps5o2_fixed/$PS5_TITLE_ID/swr_data}"
OUT_DIR="${OUT_DIR:-$DEV_WORK_ROOT/linux_strict}"
BIN_NAME="${BIN_NAME:-bananza_headless}"
SWR_STRICT="${SWR_STRICT:-1}"
SWR_ASAN="${SWR_ASAN:-0}"
RUN_SECONDS="${RUN_SECONDS:-25}"
GIT_BRANCH="${GIT_BRANCH:-ps5}"

# 运行在 253 的 git 仓库里就走本地分支（Mac 上仓库路径不同，走同步分支）。
if [[ "${1:-}" == "--local" ]] || [[ "$REPO_ROOT" == "$DEV_CODE_DIR" ]]; then
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

# ---- Step 1: git 同步（与 build_deploy.sh 一致：fetch + reset --hard，不用 rsync / git pull） ----
# git reset --hard 只能拿到 origin 上已有的提交。若本机有未提交或未推送的改动，
# 253 会静默跑旧代码，排查结论就不可信 —— 所以先检查并中止。
if [[ "${SKIP_SYNC_CHECK:-0}" != "1" ]]; then
    git -C "$REPO_ROOT" fetch origin "$GIT_BRANCH" --quiet 2>/dev/null || \
        echo "    [警告] 本机 git fetch 失败，未推送提交检查可能不准。"
    DIRTY="$(git -C "$REPO_ROOT" status --porcelain --untracked-files=normal || true)"
    AHEAD="$(git -C "$REPO_ROOT" rev-list --count "origin/$GIT_BRANCH..HEAD" 2>/dev/null || echo 1)"
    if [[ -n "$DIRTY" || "$AHEAD" != "0" ]]; then
        echo "!! 本机存在未提交/未跟踪改动或尚未推送的提交 (ahead=${AHEAD}):" >&2
        if [[ -n "$DIRTY" ]]; then printf '%s\n' "$DIRTY" | head -20 >&2; fi
        echo "   git reset --hard origin/$GIT_BRANCH 只会拿到 origin 上的代码，这些改动不会出现在 253。" >&2
        echo "   请先执行: git add -A && git commit && git push origin $GIT_BRANCH" >&2
        echo "   确认可以跳过检查时: SKIP_SYNC_CHECK=1 bash $0" >&2
        exit 1
    fi
fi

echo "=== [Step 1] Syncing code to ${BUILD_HOST}:${REMOTE_REPO} (git fetch + reset --hard origin/${GIT_BRANCH}) ==="
ssh "$BUILD_HOST" "cd $(printf %q "$REMOTE_REPO") && \
    git fetch origin $(printf %q "$GIT_BRANCH") && \
    git reset --hard origin/$(printf %q "$GIT_BRANCH") && \
    git log --oneline -1 && git status --short"

if [[ "${1:-}" == "--sync-only" ]]; then
    echo "=== [sync-only] 代码已对齐，跳过编译与运行 ==="
    exit 0
fi

echo "=== [Step 2] Running Linux Headless + SWR_STRICT=${SWR_STRICT} on ${BUILD_HOST} ==="
# 把本机设置原样传给远端，避免远端回落到默认值造成两边变量不一致。
REMOTE_ENV="$(printf 'SWR_STRICT=%q SWR_ASAN=%q RUN_SECONDS=%q BUILD_DIR=%q DATA_DIR=%q OUT_DIR=%q BIN_NAME=%q' \
    "$SWR_STRICT" "$SWR_ASAN" "$RUN_SECONDS" "$BUILD_DIR" "$DATA_DIR" "$OUT_DIR" "$BIN_NAME")"
ssh "$BUILD_HOST" "cd $(printf %q "$REMOTE_REPO") && $REMOTE_ENV bash $(printf %q "$REMOTE_REPO/tools/ps5/run_linux_strict.sh") --local"
