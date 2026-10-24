#!/usr/bin/env bash
# 把 EM Studio / EM-SDR 的图标与桌面快捷方式装到用户目录。
# 幂等：可以反复运行，会覆盖旧文件。
set -eu

ICONS_SRC=/home/sun/EMlink/design/icons
ICON_ROOT="$HOME/.local/share/icons/hicolor"
APPS="$HOME/.local/share/applications"
DESKTOP_DIR=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")

echo "===== 1. 安装图标 ====="
for size in 512 256 128 64 48; do
    dir="$ICON_ROOT/${size}x${size}/apps"
    mkdir -p "$dir"
    for name in em-studio em-sdr; do
        src="$ICONS_SRC/${name}-${size}.png"
        [ -f "$src" ] || { echo "  缺 $src"; continue; }
        cp -f "$src" "$dir/${name}.png"
        echo "  $dir/${name}.png"
    done
done

# 矢量源也装一份，高分屏/缩放时优先用
SCAL="$ICON_ROOT/scalable/apps"
mkdir -p "$SCAL"
for name in em-studio em-sdr; do
    [ -f "$ICONS_SRC/${name}.svg" ] && cp -f "$ICONS_SRC/${name}.svg" "$SCAL/${name}.svg" \
        && echo "  $SCAL/${name}.svg"
done

echo
echo "===== 2. 写 .desktop ====="
mkdir -p "$APPS"

cat > "$APPS/em-studio.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=EM Studio
Name[zh_CN]=EM Studio
GenericName=EMlink 配置工具
Comment=配置 EMlink 数传节点、查看设备状态、烧录固件
Exec=/home/sun/EMlink/tools/em-studio/run.sh
Icon=em-studio
Terminal=false
Categories=Development;
Keywords=EMlink;LoRa;Mesh;串口;烧录;
StartupNotify=true
EOF

cat > "$APPS/em-sdr.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=EM-SDR
Name[zh_CN]=EM-SDR
GenericName=空口监视器
Comment=监听 915 MHz 上 EMlink 节点的频谱、瀑布与帧活动
Exec=/home/sun/EMlink/tools/em-sdr/run.sh --autostart
Icon=em-sdr
Terminal=false
Categories=Development;
Keywords=EMlink;LoRa;SDR;频谱;瀑布;
StartupNotify=true
EOF

chmod 755 "$APPS/em-studio.desktop" "$APPS/em-sdr.desktop"
chmod 755 /home/sun/EMlink/tools/em-studio/run.sh /home/sun/EMlink/tools/em-sdr/run.sh
ls -l "$APPS"/em-studio.desktop "$APPS"/em-sdr.desktop

echo
echo "===== 3. 放桌面（$DESKTOP_DIR）====="
mkdir -p "$DESKTOP_DIR"
for f in em-studio em-sdr; do
    cp -f "$APPS/$f.desktop" "$DESKTOP_DIR/$f.desktop"
    chmod 755 "$DESKTOP_DIR/$f.desktop"
    # GNOME/Nemo 需要显式标记为可信才能直接双击运行
    gio set "$DESKTOP_DIR/$f.desktop" metadata::trusted true 2>/dev/null || true
    echo "  $DESKTOP_DIR/$f.desktop"
done

echo
echo "===== 4. 刷新缓存 ====="
gtk-update-icon-cache -f -t "$ICON_ROOT" 2>/dev/null && echo "  图标缓存已刷新" || echo "  (图标缓存跳过)"
update-desktop-database "$APPS" 2>/dev/null && echo "  应用数据库已刷新" || echo "  (应用数据库跳过)"

echo
echo "===== 5. 校验 ====="
for f in "$APPS/em-studio.desktop" "$APPS/em-sdr.desktop"; do
    echo "--- $f"
    desktop-file-validate "$f" 2>&1 | head -5 || true
done

echo
echo "完成。桌面上的两个图标可能需要登出/重进桌面才刷新。"
