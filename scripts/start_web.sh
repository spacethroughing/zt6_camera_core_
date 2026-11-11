#!/usr/bin/env bash
# 本机通过 SSH 隧道访问飞机上的 C++ Web；部署完成后使用。
# local_port 是电脑浏览器访问端口；18765 是飞机回环 HTTP 端口。
set -euo pipefail
target=${1:-cat@100.64.0.56}
local_port=${ZT6_WEB_LOCAL_PORT:-18766}
remote_dir=${ZT6_REMOTE_DIR:-zt6_camera_ws_cpp}
if [[ ! "$remote_dir" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    echo '远端目录必须是主目录下的单个目录名。' >&2; exit 2
fi
if [[ ! "$local_port" =~ ^[0-9]+$ ]] || (( local_port < 1024 || local_port > 65535 )); then
    echo '本机端口无效。' >&2; exit 2
fi
echo "登录飞机后，用本机浏览器打开：http://127.0.0.1:$local_port"
echo '保留本终端；Ctrl+C 关闭网页服务和隧道。录像需要在界面或 CLI 中单独停止。'
exec ssh -tt -o ExitOnForwardFailure=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=3 \
    -o ConnectTimeout=8 -L "127.0.0.1:$local_port:127.0.0.1:18765" "$target" \
    "cd ~/'$remote_dir' && exec ./bin/zt6-web --port 18765"
