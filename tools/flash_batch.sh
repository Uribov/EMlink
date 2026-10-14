#!/usr/bin/env bash
# ============================================================
# EMlink 批量烧录脚本
#   合并镜像 -> 逐块板：检测芯片 -> 整片擦除 -> 写入（带校验）-> 结果表
#
# 用法：
#   bash tools/flash_batch.sh --port /dev/ttyUSB0        # 单块板
#   bash tools/flash_batch.sh --all                      # 自动找 /dev/ttyUSB* /dev/ttyACM*
#   bash tools/flash_batch.sh --all -e <env> --no-build  # 指定 PlatformIO env / 跳过构建
#   bash tools/flash_batch.sh --merge-only               # 只生成合并镜像并打印 sha256
#
# 依赖：bash + python3 + esptool + pio（不引入其它依赖）
# ============================================================
set -euo pipefail

# 本机环境坑：Hermes 会把自己的旧 libstdc++ 塞进 LD_LIBRARY_PATH，
# 导致 pio / 平台工具报 GLIBCXX 找不到。这里必须先清掉。
unset LD_LIBRARY_PATH

# ---------------------------- 默认值 ----------------------------
DEFAULT_ENV="esp32s3-supermini-e220-900m22s"
CHIP="esp32s3"
ENV="$DEFAULT_ENV"
PORT=""
USE_ALL=0
MERGE_ONLY=0
DO_BUILD=1
ASSUME_YES=0
BAUD="${BAUD:-460800}"
CMD_TIMEOUT="${CMD_TIMEOUT:-60}"
ESPTOOL="${ESPTOOL:-esptool}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
FIRMWARE_DIR="$REPO_ROOT/firmware"
BUILD_DIR="$FIRMWARE_DIR/.pio/build/$ENV"
MERGED_IMAGE="$BUILD_DIR/emlink_factory_${ENV}.bin"

# ---------------------------- 帮助 ----------------------------
usage() {
    cat <<'EOF'
EMlink 批量烧录脚本

用法:
  bash tools/flash_batch.sh --port <串口>              # 单块板
  bash tools/flash_batch.sh --all                     # 自动扫描所有串口
  bash tools/flash_batch.sh --all -e <env> --no-build # 指定 env / 跳过构建
  bash tools/flash_batch.sh --merge-only              # 只生成合并镜像并打印 sha256

参数:
  --port <串口>   指定串口，如 /dev/ttyUSB0
  --all           自动查找 /dev/ttyUSB* 与 /dev/ttyACM*
  --merge-only    只生成合并镜像（不碰任何板子），打印路径/大小/sha256
  -e, --env <名>  PlatformIO 环境名（默认: esp32s3-supermini-e220-900m22s）
  --no-build      跳过构建，直接用已有产物
  --yes, -y       跳过每块板烧写前的 5 秒等待
  -h, --help      显示本帮助

环境变量:
  ESPTOOL=<命令>  指定 esptool 命令（默认: PATH 里的 esptool）
  BAUD=<波特率>   烧写波特率（默认: 460800）
  CMD_TIMEOUT=<秒>  单步（合并/擦除/写入）超时，默认 60；大 flash 或低波特率可调大
EOF
}

# ---------------------------- 参数解析 ----------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)
            [[ $# -ge 2 ]] || { echo "错误：--port 缺少参数" >&2; usage; exit 2; }
            PORT="$2"; shift 2 ;;
        --all)
            USE_ALL=1; shift ;;
        --merge-only|--merge)
            MERGE_ONLY=1; shift ;;
        -e|--env)
            [[ $# -ge 2 ]] || { echo "错误：-e 缺少参数" >&2; usage; exit 2; }
            ENV="$2"; shift 2 ;;
        --no-build)
            DO_BUILD=0; shift ;;
        --yes|-y)
            ASSUME_YES=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "错误：未知参数 '$1'" >&2
            echo "" >&2
            usage
            exit 2 ;;
    esac
done

# env 改变后重算 build 目录与输出镜像路径
BUILD_DIR="$FIRMWARE_DIR/.pio/build/$ENV"
MERGED_IMAGE="$BUILD_DIR/emlink_factory_${ENV}.bin"

# 参数合法性：既不 --port 也不 --all 也不 --merge-only => 用法错误
if [[ -z "$PORT" && $USE_ALL -eq 0 && $MERGE_ONLY -eq 0 ]]; then
    echo "错误：必须指定 --port <串口>、--all 或 --merge-only 之一" >&2
    echo "" >&2
    usage
    exit 2
fi
if [[ -n "$PORT" && $USE_ALL -eq 1 ]]; then
    echo "错误：--port 与 --all 不能同时使用" >&2
    exit 2
fi

# ---------------------------- esptool 定位 ----------------------------
# 优先用 PATH 里的 esptool；找不到再回退 python3 -m esptool。
# 只做 command -v 检查，绝不探测仓库之外的目录。
resolve_esptool() {
    if command -v "$ESPTOOL" >/dev/null 2>&1; then
        ESPTOOL_CMD=("$ESPTOOL")
        return 0
    fi
    if python3 -c 'import esptool' >/dev/null 2>&1; then
        ESPTOOL_CMD=(python3 -m esptool)
        return 0
    fi
    return 1
}

if ! resolve_esptool; then
    echo "错误：找不到 esptool。请确认 PATH 里有 'esptool'，或设置 ESPTOOL=<命令>。" >&2
    exit 1
fi
echo "使用 esptool: ${ESPTOOL_CMD[*]}"

# ---------------------------- 步骤 1：构建 ----------------------------
if [[ $DO_BUILD -eq 1 ]]; then
    echo "[1/3] 构建固件（env=$ENV）……"
    ( cd "$FIRMWARE_DIR" && pio run -e "$ENV" )
    echo "构建完成。"
else
    echo "[1/3] 跳过构建（--no-build）。"
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "错误：找不到构建目录 $BUILD_DIR（先构建或去掉 --no-build）。" >&2
    exit 1
fi

# ---------------------------- 步骤 2：生成合并镜像 ----------------------------
echo "[2/3] 生成合并镜像……"

FLASH_ARGS_FILE="$BUILD_DIR/flash_args"
declare -a MERGE_ARGS=()

if [[ -f "$FLASH_ARGS_FILE" ]]; then
    # PlatformIO 生成了 flash_args：直接解析（首行是 --flash_* 参数，其余是 "偏移 文件"）。
    echo "  读取 PlatformIO flash_args: $FLASH_ARGS_FILE"
    mapfile -d '' -t MERGE_ARGS < <(python3 - "$FLASH_ARGS_FILE" <<'PY'
import sys
with open(sys.argv[1]) as fh:
    for raw in fh:
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        toks = line.split()
        if toks[0].startswith('--'):
            for t in toks:
                sys.stdout.write(t + '\0')
        elif toks[0].lower().startswith('0x'):
            sys.stdout.write(toks[0] + '\0')
            sys.stdout.write(' '.join(toks[1:]) + '\0')
PY
    )
else
    # 本平台（pioarduino espressif32 7.x）不生成 flash_args，
    # 回退到标准 ESP32-S3 Arduino 布局；只读仓库内的文件，绝不读仓库之外。
    echo "  未找到 flash_args，按标准 ESP32-S3 布局合成（boot_app0 优先用仓库内 tools/boot_app0.bin）"
    MERGE_ARGS=(--flash_mode keep --flash_freq keep --flash_size keep)
    if [[ -f "$BUILD_DIR/bootloader.bin" ]]; then MERGE_ARGS+=(0x0 bootloader.bin); fi
    if [[ -f "$BUILD_DIR/partitions.bin" ]]; then MERGE_ARGS+=(0x8000 partitions.bin); fi
    # boot_app0.bin：优先用仓库内 vendor 的标准文件（tools/boot_app0.bin），
    # 与 PlatformIO 原生 upload 写入的内容保持一致（显式填充 otadata 区，
    # 不依赖"otadata 空白 → 默认启动 ota_0"）。次选 build 目录内的产物。
    # 注意：merge_bin 是在 build 目录内执行的，故这里一律用绝对路径，
    # 否则相对路径会被按 build 目录解析而找不到。
    BOOT_APP0=""
    if [[ -f "$REPO_ROOT/tools/boot_app0.bin" ]]; then
        BOOT_APP0="$REPO_ROOT/tools/boot_app0.bin"
    elif [[ -f "$BUILD_DIR/boot_app0.bin" ]]; then
        BOOT_APP0="$BUILD_DIR/boot_app0.bin"
    fi
    if [[ -n "$BOOT_APP0" ]]; then MERGE_ARGS+=(0xe000 "$BOOT_APP0"); fi
    MERGE_ARGS+=(0x10000 firmware.bin)
fi

if [[ ${#MERGE_ARGS[@]} -eq 0 ]]; then
    echo "错误：flash_args 解析结果为空，无法合成镜像。" >&2
    exit 1
fi

# flash_args 里的路径是相对 build 目录的，因此在 build 目录内执行。
( cd "$BUILD_DIR" && timeout "$CMD_TIMEOUT" "${ESPTOOL_CMD[@]}" --chip "$CHIP" \
    merge_bin -o "$MERGED_IMAGE" "${MERGE_ARGS[@]}" )

if [[ ! -f "$MERGED_IMAGE" ]]; then
    echo "错误：合并镜像生成失败：$MERGED_IMAGE" >&2
    exit 1
fi

# ---------------------------- 步骤 3：--merge-only 收尾 ----------------------------
if [[ $MERGE_ONLY -eq 1 ]]; then
    MERGED_SIZE="$(stat -c '%s' "$MERGED_IMAGE")"
    MERGED_SHA="$(sha256sum "$MERGED_IMAGE" | awk '{print $1}')"
    echo ""
    echo "==================== 合并镜像 ===================="
    echo "镜像路径: $MERGED_IMAGE"
    echo "镜像大小: $MERGED_SIZE 字节"
    echo "sha256  : $MERGED_SHA"
    echo "================================================="
    exit 0
fi

# ---------------------------- 收集待烧写串口 ----------------------------
declare -a PORTS=()
if [[ $USE_ALL -eq 1 ]]; then
    shopt -s nullglob
    PORTS=(/dev/ttyUSB* /dev/ttyACM*)
    shopt -u nullglob
    if [[ ${#PORTS[@]} -gt 0 ]]; then
        mapfile -t PORTS < <(printf '%s\n' "${PORTS[@]}" | sort)
    fi
else
    PORTS=("$PORT")
fi

if [[ ${#PORTS[@]} -eq 0 ]]; then
    echo ""
    echo "错误：--all 没有找到任何串口（/dev/ttyUSB* 或 /dev/ttyACM*）。" >&2
    echo "提示：确认已插好 USB 数据线、装好驱动、且当前用户在 dialout 组。" >&2
    exit 1
fi

echo ""
echo "[3/3] 共 ${#PORTS[@]} 块板待烧写。"

# ---------------------------- 逐块板烧写 ----------------------------
declare -a R_PORT=()
declare -a R_CHIP=()
declare -a R_STATUS=()
declare -a R_TIME=()
declare -a R_DETAIL=()

FAILED=0

detect_chip() {
    local port="$1" out chip
    out="$(timeout "$CMD_TIMEOUT" "${ESPTOOL_CMD[@]}" --chip "$CHIP" --port "$port" \
        --baud "$BAUD" flash_id 2>&1)" || true
    chip="$(printf '%s\n' "$out" | grep -m1 'Chip is' | sed 's/^Chip is //' || true)"
    [[ -z "$chip" ]] && chip="未知"
    printf '%s' "$chip"
}

flash_one() {
    local port="$1"
    local start_ms end_ms elapsed chip

    if [[ ! -e "$port" ]]; then
        R_PORT+=("$port"); R_CHIP+=("-"); R_STATUS+=("失败"); R_TIME+=("0.0s")
        R_DETAIL+=("端口不存在")
        echo "  ✗ $port：端口不存在，跳过。"
        return 1
    fi

    if [[ $ASSUME_YES -eq 0 ]]; then
        echo "即将擦除并烧写 $port，5 秒后开始（Ctrl-C 取消）"
        local i
        for i in 5 4 3 2 1; do printf '  %d...\n' "$i"; sleep 1; done
    else
        echo "即将擦除并烧写 $port（--yes，跳过等待）"
    fi

    start_ms="$(date +%s%3N)"
    chip="$(detect_chip "$port")"
    echo "  检测到芯片: $chip"

    echo "  [1/2] 整片擦除 erase_flash ……"
    if ! timeout "$CMD_TIMEOUT" "${ESPTOOL_CMD[@]}" --chip "$CHIP" --port "$port" \
            --baud "$BAUD" erase_flash; then
        end_ms="$(date +%s%3N)"
        elapsed="$(awk "BEGIN{printf \"%.1f\", ($end_ms-$start_ms)/1000}")"
        R_PORT+=("$port"); R_CHIP+=("$chip"); R_STATUS+=("失败"); R_TIME+=("${elapsed}s")
        R_DETAIL+=("整片擦除失败")
        echo "  ✗ $port：整片擦除失败。"
        return 1
    fi
    # erase_flash 结束后板子会硬复位，USB 桥（CP2102/CH34x）可能短暂重枚举；
    # 等 1 秒让它回来，避免紧接着的 write_flash 偶发连接失败。
    sleep 1

    echo "  [2/2] 写入合并镜像（0x0，带校验）……"
    if ! timeout "$CMD_TIMEOUT" "${ESPTOOL_CMD[@]}" --chip "$CHIP" --port "$port" \
            --baud "$BAUD" write_flash 0x0 "$MERGED_IMAGE"; then
        echo "  写入失败，1 秒后重试一次……"
        sleep 1
        if ! timeout "$CMD_TIMEOUT" "${ESPTOOL_CMD[@]}" --chip "$CHIP" --port "$port" \
                --baud "$BAUD" write_flash 0x0 "$MERGED_IMAGE"; then
            end_ms="$(date +%s%3N)"
            elapsed="$(awk "BEGIN{printf \"%.1f\", ($end_ms-$start_ms)/1000}")"
            R_PORT+=("$port"); R_CHIP+=("$chip"); R_STATUS+=("失败"); R_TIME+=("${elapsed}s")
            R_DETAIL+=("写入失败（已重试）")
            echo "  ✗ $port：写入失败（已重试）。"
            return 1
        fi
    fi

    end_ms="$(date +%s%3N)"
    elapsed="$(awk "BEGIN{printf \"%.1f\", ($end_ms-$start_ms)/1000}")"
    R_PORT+=("$port"); R_CHIP+=("$chip"); R_STATUS+=("成功"); R_TIME+=("${elapsed}s")
    R_DETAIL+=("OK")
    echo "  ✓ $port：烧写完成，用时 ${elapsed}s。"
    return 0
}

idx=0
for p in "${PORTS[@]}"; do
    idx=$((idx + 1))
    echo ""
    echo "---------- 板 $idx/${#PORTS[@]} : $p ----------"
    if ! flash_one "$p"; then
        FAILED=$((FAILED + 1))
    fi
done

# ---------------------------- 汇总表 ----------------------------
echo ""
echo "==================== 烧录结果汇总 ===================="
printf '%-18s %-36s %-6s %-9s\n' "端口" "芯片" "结果" "耗时"
printf '%-18s %-36s %-6s %-9s\n' "------------------" "------------------------------------" "------" "---------"
for i in "${!R_PORT[@]}"; do
    printf '%-18s %-36s %-6s %-9s\n' "${R_PORT[$i]}" "${R_CHIP[$i]}" "${R_STATUS[$i]}" "${R_TIME[$i]}"
    if [[ "${R_STATUS[$i]}" != "成功" ]]; then
        printf '   ↳ 原因: %s\n' "${R_DETAIL[$i]}"
    fi
done
echo "-----------------------------------------------------"
echo "成功: $(( ${#R_PORT[@]} - FAILED ))  失败: $FAILED  合计: ${#R_PORT[@]}"
echo "====================================================="

if [[ $FAILED -gt 0 ]]; then
    exit 1
fi
exit 0
