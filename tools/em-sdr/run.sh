#!/usr/bin/env bash
# EM-SDR 启动器
#
# 为什么用 em-studio 的 venv：
#   系统 python3 没装 pywebview，而 pip install 到系统会被 PEP 668 拦。
#   em-studio 的 venv 里已经有 pywebview + numpy 1.26.4 + 可用的 gnuradio，
#   正好满足需求（GNU Radio 是用 numpy 1.x 编的，numpy 2.x 会直接 import 失败）。
#
# 为什么要清 LD_LIBRARY_PATH：
#   Hermes 运行时把自己的 _internal 塞进去，里面有旧版 libstdc++，
#   会让 GTK/Qt 后端报 GLIBCXX 版本错误。
#
# 为什么要设 PYTHONPATH：
#   避免 ~/.local 里的 numpy 2.4.1 抢在系统 1.26.4 前面。

set -u

VENV_PY=/home/sun/EMlink/tools/em-studio/.venv/bin/python
APP=/home/sun/EMlink/tools/em-sdr/app.py

if [ ! -x "$VENV_PY" ]; then
    echo "找不到 venv: $VENV_PY" >&2
    echo "请先在 VSCode 里用 EM Studio 配置跑过一次，或手动创建 venv 并装 pywebview" >&2
    exit 1
fi

unset LD_LIBRARY_PATH
export PYTHONPATH=/usr/lib/python3/dist-packages:/usr/local/lib/python3.12/dist-packages

# rtl_sdr 独占 USB dongle，先把可能残留的占用者清掉
if pgrep -x sdrpp >/dev/null 2>&1; then
    echo "检测到 SDR++ 正在运行，它独占 dongle，先关掉它…"
    pkill -x sdrpp
    sleep 1.5
fi

exec "$VENV_PY" "$APP" "$@"
