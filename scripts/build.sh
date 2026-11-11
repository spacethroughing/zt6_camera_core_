#!/usr/bin/env bash
# 核心部署包：在当前机器编译 CLI / Web，不构建测试。
set -euo pipefail
workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cmake -S "$workspace" -B "$workspace/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$workspace/build" --parallel 2
"$workspace/bin/zt6" --help
"$workspace/bin/zt6-web" --help
