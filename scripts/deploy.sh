#!/usr/bin/env bash
# 核心部署包：校验源码后传至新目录，在飞机本地编译。
# 不覆盖旧目录，不启动拍摄、运动或 Web。密码由 SSH 交互读取。
set -euo pipefail
workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-cat@100.64.0.56}
remote_dir=${2:-zt6_camera_ws_cpp}
if [[ ! "$remote_dir" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    echo '远端目录必须是主目录下的单个目录名（字母、数字、下划线、横线）。' >&2
    exit 2
fi
# One SSH login; credentials are read interactively by ssh, never saved here.
(cd "$workspace" && sha256sum -c SHA256SUMS)
tar -C "$workspace" -czf - bin cpp third_party web scripts CMakeLists.txt README.md SHA256SUMS |
ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new "$target" \
    "set -eu; umask 077; cd; test ! -e '$remote_dir' || { echo '目标已存在，未覆盖；请指定新目录名。' >&2; exit 2; }; mkdir '$remote_dir'; tar -xzf - -C '$remote_dir'; cd '$remote_dir'; sha256sum -c SHA256SUMS; bash scripts/build.sh"
