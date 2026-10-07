#!/usr/bin/env bash
# 在 Steam Frame 上安装或更新 Frame Mic Tuner。请在头显上以普通用户身份运行（不需要 sudo），
# 可以从下载好的发布包安装（先解压 tar.gz），也可以从克隆下来的仓库安装
# （后者会进行构建）：
#   ./install.sh                     需要时构建，然后安装或更新
#   ./install.sh --autostart         同上，并从此随 SteamVR 一起启动
#   ./install.sh --uninstall         卸载应用和 WirePlumber 脚本（之后请重启）
#   ./install.sh --uninstall --purge 还会删除应用的设置和保存的 frame-mic.* 值
#
# 本脚本从不重启 PipeWire 或 WirePlumber（游戏过程中这样做会让 SteamVR 和 Steam Link 失去声音），
# 也从不改动 amixer、/etc 或 root。
# 新的 WirePlumber 脚本会在下次重启后生效。
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
bin="$HOME/.local/bin/frame-mic-tuner"
desktop="$data_home/applications/frame-mic-tuner.desktop"
icons="$data_home/icons/hicolor"
unit_dir="$config_home/systemd/user"
unit="frame-mic-tuner.service"
app_config="$config_home/frame-mic-tuner"
wp_script="$data_home/wireplumber/scripts/frame-mic-tracker.lua"
wp_conf="$config_home/wireplumber/wireplumber.conf.d/90-frame-mic.conf"
update_script="$data_home/frame-mic-tuner/frame-update.sh"

# A release tar.gz has the built binary next to this script and no CMakeLists.txt; a git checkout has
# the source and no binary here yet (or an outdated one from a previous build).
if [[ -f "$here/CMakeLists.txt" ]]; then
    from_source=1
else
    from_source=0
fi

usage() {
    sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'
}

autostart=0
uninstall=0
purge=0
for arg in "$@"; do
    case "$arg" in
        --autostart) autostart=1 ;;
        --uninstall) uninstall=1 ;;
        --purge) purge=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知选项: $arg" >&2; usage >&2; exit 2 ;;
    esac
done
if [[ $purge -eq 1 && $uninstall -eq 0 ]]; then
    echo "--purge 只能与 --uninstall 一起使用。" >&2
    exit 2
fi

if [[ $uninstall -eq 1 ]]; then
    # Stop and disable the service, and quit an instance started from the dashboard (+), if any.
    systemctl --user disable --now "$unit" 2>/dev/null || true
    pkill -TERM -x frame-mic-tuner 2>/dev/null || true
    rm -f "$bin" "$desktop" "$unit_dir/$unit" "$wp_script" "$wp_conf" "$update_script"
    rmdir "$(dirname "$update_script")" 2>/dev/null || true
    for size in 48 128 256; do
        rm -f "$icons/${size}x${size}/apps/frame-mic-tuner.png"
    done
    systemctl --user daemon-reload
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$(dirname "$desktop")" || true
    fi
    if [[ $purge -eq 1 ]]; then
        rm -rf "$app_config"
        # Forget the saved switch values. Only these two keys; nothing else in WirePlumber changes.
        wpctl settings --delete frame-mic.echo-cancel >/dev/null 2>&1 || true
        wpctl settings --delete frame-mic.noise-suppression >/dev/null 2>&1 || true
        echo "已删除 Frame Mic Tuner、它的设置以及保存的 frame-mic.* 值。"
    else
        echo "已删除 Frame Mic Tuner。它的设置仍保留在 $app_config（加上 --purge 可一并删除）。"
    fi
    cat <<EOF

WirePlumber 脚本已删除，但在 WirePlumber 下次启动之前仍处于加载状态。
请在没在游戏时重启头显：之后就会恢复 Valve 原本的麦克风行为
（应用使用麦克风时，回声消除和噪声抑制都会打开）。
游戏过程中不要手动重启 PipeWire 或 WirePlumber：SteamVR 和 Steam Link 会失去声音
（重启 SteamVR 即可恢复）。
EOF
    exit 0
fi

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "这是给 Steam Frame（aarch64）用的，但本机是 $(uname -m)。" >&2
    exit 1
fi
tools="wpctl systemctl"
if [[ $from_source -eq 1 ]]; then
    tools="cmake ninja g++ pkg-config $tools"
fi
for tool in $tools; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "缺少工具: $tool" >&2
        exit 1
    fi
done

cd "$here"
if [[ $from_source -eq 1 ]]; then
    # Build (or bring an existing build up to date after a git pull)
    if [[ ! -f build/build.ninja ]]; then
        cmake -G Ninja -S . -B build
    fi
    ninja -C build
    built_bin="build/frame-mic-tuner"
else
    # A release tar.gz: the binary is already built, right next to this script
    built_bin="$here/frame-mic-tuner"
fi

# The app: binary, launcher entry for the dashboard's "+" list, icons, and the systemd unit
install -Dm755 "$built_bin" "$bin"
# The shared update script (vendor/frame-updater), so the panel's update card can check and install updates
install -Dm755 vendor/frame-updater/frame-update.sh "$update_script"
for size in 48 128 256; do
    install -Dm644 "contrib/icons/frame-mic-tuner-$size.png" "$icons/${size}x${size}/apps/frame-mic-tuner.png"
done
mkdir -p "$(dirname "$desktop")"
sed "s|@BINARY@|$bin|" contrib/frame-mic-tuner.desktop > "$desktop.tmp"
chmod 644 "$desktop.tmp"
mv "$desktop.tmp" "$desktop"
install -Dm644 contrib/frame-mic-tuner.service "$unit_dir/$unit"
systemctl --user daemon-reload
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$desktop" || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$(dirname "$desktop")" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [[ -f "$icons/index.theme" ]]; then
    gtk-update-icon-cache -q "$icons" || true
fi

# The WirePlumber script that lets the app switch the filters. Only the user's own folders are used
# (Valve's files in /etc stay as they are). A file is left alone when it already has the same content;
# the "SPDX-License-Identifier" first line is ignored in that comparison.
wp_changed=0
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

# Print a file without its SPDX first line.
strip_spdx() {
    sed '1{/SPDX-License-Identifier/d}' "$1"
}

# Install $1 (already rendered) as $2 unless $2 has the same content.
install_if_changed() {
    local source="$1" target="$2"
    if [[ -f "$target" ]] && cmp -s <(strip_spdx "$source") <(strip_spdx "$target"); then
        echo "WirePlumber: $target 已是最新。"
        return
    fi
    install -Dm644 "$source" "$target"
    echo "WirePlumber: 已安装 $target"
    wp_changed=1
}

cp contrib/wireplumber/frame-mic-tracker.lua "$tmp"
install_if_changed "$tmp" "$wp_script"
sed "s|@SCRIPT_PATH@|$wp_script|" contrib/wireplumber/90-frame-mic.conf > "$tmp"
install_if_changed "$tmp" "$wp_conf"

if [[ $autostart -eq 1 ]]; then
    systemctl --user enable "$unit"
fi
autostart_state="$(systemctl --user is-enabled "$unit" 2>/dev/null || true)"

# Remember this run's options, so an update started from the panel (which runs this same script with no
# terminal to ask) reinstalls with the same options. Only --autostart matters here.
mkdir -p "$app_config"
: > "$app_config/install-args"
if [[ $autostart -eq 1 ]]; then
    echo --autostart >> "$app_config/install-args"
fi

cat <<EOF

Frame Mic Tuner 已安装：
  $bin
  $desktop
  $unit_dir/$unit（随 SteamVR 启动：${autostart_state:-unknown}；可在面板里切换）
从 SteamVR 仪表盘打开：+（启动程序）> Frame Mic Tuner，然后点“Mic”图标。
EOF
if pgrep -x frame-mic-tuner >/dev/null 2>&1; then
    echo "正在运行的 Frame Mic Tuner 会继续使用旧版本，直到你退出它（仪表盘 > Mic > 退出）或重启 SteamVR。"
fi
if [[ $wp_changed -eq 1 ]]; then
    cat <<EOF

WirePlumber 脚本已安装或已更新。重启头显即可启用。
游戏过程中不要重启 PipeWire 或 WirePlumber：SteamVR 和 Steam Link 会失去声音
（重启 SteamVR 即可恢复）。
EOF
fi
