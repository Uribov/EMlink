#!/usr/bin/env bash
# EM Studio 启动器（供桌面快捷方式调用）
#
# 为什么要单独一个脚本：
#   1. .desktop 的 Exec 字段不解析 shell 语法，写 `bash -c '... && ...'`
#      会被 desktop-file-validate 判为非法（保留字符 ' 与 & 未转义）。
#   2. 启动 GUI 前必须清掉 LD_LIBRARY_PATH，否则 pywebview 的 GTK 后端
#      加载宿主注入的旧 libstdc++，报 GLIBCXX_3.4.31 not found，窗口不出现。

set -u

REPO=/home/sun/EMlink
VENV_PY="$REPO/tools/em-studio/.venv/bin/python"
APP="$REPO/tools/em-studio/app.py"

if [ ! -x "$VENV_PY" ]; then
    echo "找不到 venv 解释器: $VENV_PY" >&2
    exit 1
fi

unset LD_LIBRARY_PATH
# 让系统 numpy 1.26.4 压住 ~/.local 里可能的 numpy 2.x
export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages

cd "$REPO" || exit 1
exec "$VENV_PY" "$APP" "$@"
