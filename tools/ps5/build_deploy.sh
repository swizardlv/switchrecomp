#!/usr/bin/env bash
# build_deploy.sh — 同步 253 代码、编译、部署 eboot 到 PS5
#
# 用法（在 Mac 终端执行）:
#   ./tools/ps5/build_deploy.sh            # 完整流程: git 同步 + 编译 + 部署
#   ./tools/ps5/build_deploy.sh --deploy-only   # 跳过编译, 仅部署已有 eboot (PS5 离线后补部署)
#
# 约定:
#   * 253 是编译/部署机, PS5($PS5_HOST, 见 <repo>/.env)需开机连网才能完成部署。
#   * 253 的 ps5 分支会与远端分叉, 不能用 git pull (non-fast-forward 失败),
#     故用 git fetch + git reset --hard origin/ps5 对齐, 符合项目约定。
#   * 默认 runtime-only 构建: 复用 libgame_code.a (游戏代码), 只重编 23 个 runtime 对象。
#     注意: cpu_helpers.c (SIMD 修复) 被 #include 进 game code, 不在本次构建内,
#           需全量 switchrecomp 重建才会生效 (另行处理, 不在本脚本)。
set -euo pipefail

# ---- 配置 (主机/端口统一从 <repo>/.env 读取, 见 .env.example) ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eval "$(python3 "$SCRIPT_DIR/ps5_env.py" --shell)"
REMOTE="$DEV_SSH"
CODE_DIR="$DEV_CODE_DIR"                                 # 253 上的代码根
BUILD_DIR="$DEV_WORK_ROOT/ps5o2_fixed"                   # 复用 0eb06fa 那次 build
MANIFEST="$BUILD_DIR/swr_bananza_ps5_o2_fixed_build/ps5_build_manifest.json"
EBOOT="$BUILD_DIR/$PS5_TITLE_ID/eboot.bin"
RUNS="$DEV_WORK_ROOT/runs"                               # 253 上的 run 日志目录

DEPLOY_ONLY=0
if [ "${1:-}" = "--deploy-only" ]; then DEPLOY_ONLY=1; fi

if [ "$DEPLOY_ONLY" -eq 0 ]; then
  echo "==> [1/4] 同步代码 (fetch + reset --hard origin/ps5 on 253)"
  ssh "$REMOTE" "cd $CODE_DIR && git fetch origin ps5 && git reset --hard origin/ps5"

  echo "==> [2/4] 编译 (ps5build.py, 复用 libgame_code.a, 重编 runtime 对象)"
  # ps5build 即便报告了非致命 issue (如历史遗留的 GAME_ARCHIVE_MISSING) 也可能已产出
  # 可用的 eboot.bin。只要 eboot 真正产出就继续部署; 未产出才视为编译失败。
  if ! ssh "$REMOTE" "cd $CODE_DIR && python3 tools/ps5/ps5build.py --manifest $MANIFEST"; then
    echo "    [警告] ps5build 退出码非 0 (上方报告可能有 ERROR/WARN)。若 eboot 已产出仍会继续部署。"
  fi
  if ! ssh "$REMOTE" "test -f '$EBOOT' && test \$(stat -c%s '$EBOOT') -gt 100000000"; then
    echo "!! eboot 未产出或过小 (<100MB), 编译失败。中止部署。"
    exit 1
  fi
else
  echo "==> [deploy-only] 跳过 git 同步与编译"
fi

echo "==> [3/4] 检查 PS5 FTP 在线 ($PS5_HOST:$PS5_FTP_PORT)"
if ! ssh "$REMOTE" "python3 -c \"import ftplib,sys; ftplib.FTP().connect('$PS5_HOST',$PS5_FTP_PORT,timeout=6); print('PS5 FTP OK')\""; then
  echo "!! PS5 离线 (No route to host)。请开机连网后执行: $0 --deploy-only"
  echo "   (eboot 已构建完成, 无需重新编译)"
  exit 1
fi

echo "==> [4/4] 部署 eboot (replace 模式)"
RUN_DIR="$RUNS/deploy_$(date +%Y%m%d_%H%M%S)"
ssh "$REMOTE" "cd $CODE_DIR && python3 tools/ps5/ps5_deploy_eboot.py --mode replace --local $EBOOT --run-dir $RUN_DIR"

echo "==> 部署完成。请到 PS5 上点一下游戏, 然后执行 fetch_log.sh 取日志。"
