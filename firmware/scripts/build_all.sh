#!/usr/bin/env bash
# ============================================================
# Build all firmware variants (each variant = one board + one radio)
# Usage: bash scripts/build_all.sh
# Output: .pio/build/<variant>/firmware.bin
# ============================================================
set -e

# Locate firmware dir (this script lives in firmware/scripts/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(dirname "$SCRIPT_DIR")"
cd "$FIRMWARE_DIR"

# Variant list (must match [env:xxx] in platformio.ini)
VARIANTS=(
    "esp32s3-supermini-e220-900m22s"
    # add new variants here
)

FAILED=()
for v in "${VARIANTS[@]}"; do
    echo ""
    echo "==================== Building $v ===================="
    if ! pio run -e "$v"; then
        echo "!!!! Build FAILED: $v"
        FAILED+=("$v")
    fi
done

echo ""
if [ ${#FAILED[@]} -eq 0 ]; then
    echo "==================== All variants built OK ===================="
else
    echo "==================== FAILED variants ===================="
    for f in "${FAILED[@]}"; do echo "  $f"; done
    exit 1
fi
