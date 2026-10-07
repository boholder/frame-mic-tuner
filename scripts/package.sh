#!/usr/bin/env bash
# 生成发布用的 tar.gz。在 Steam Frame 上运行（因为要链接本体的 cairo、FreeType、PipeWire 和 SteamVR 的 OpenVR）。
#   scripts/package.sh   → dist/frame-mic-tuner-<版本>.tar.gz, dist/SHA256SUMS
# 内容：可执行文件、install.sh（从 tar.gz 安装时不用构建，可以直接装）、vendor/frame-updater/frame-update.sh
# （install.sh 会放到 ~/.local/share/frame-mic-tuner/ 的更新脚本）、contrib（WirePlumber、.desktop、.service、
# 图标、设置示例）、README、CHANGELOG、LICENSE 等。不放入 CMakeLists.txt 和 src/
# （install.sh 发现没有它们时就当作“来自 tar.gz”，不进行构建直接安装）。
set -euo pipefail
cd "$(dirname "$0")/.."

name="frame-mic-tuner"
build_dir="build-release"

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "请在 Steam Frame（aarch64）上运行。本机是 $(uname -m)。" >&2
    exit 1
fi

# 同梱する vendor/frame-updater/ が、手で書き換えられていない・frame-updater 本体とずれていないかを確かめる
sh vendor/frame-updater/verify.sh

# 開発用の build/ とは別のフォルダで、Release でビルドし直す
cmake -G Ninja -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --clean-first

version="$("$build_dir/$name" --version | awk '{print $2}')"
if [[ -z "$version" || "$version" == "unknown" ]]; then
    echo "无法从 $build_dir/$name --version 读取版本" >&2
    exit 1
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
root="$stage/$name"
mkdir -p "$root/contrib/icons" "$root/contrib/wireplumber" "$root/vendor/frame-updater" "$root/third_party/openvr"
install -m755 "$build_dir/$name" "$root/$name"
strip "$root/$name"
install -m755 install.sh "$root/"
install -m644 LICENSE THIRD_PARTY_LICENSES.md README.md README.en.md README.ja.md CHANGELOG.md "$root/"
install -m644 "contrib/$name.service" "contrib/$name.desktop" contrib/config.example.json "$root/contrib/"
install -m644 contrib/icons/*.png "$root/contrib/icons/"
install -m644 contrib/wireplumber/* "$root/contrib/wireplumber/"
install -Dm644 third_party/openvr/LICENSE "$root/third_party/openvr/LICENSE"
# install.sh が ~/.local/share/frame-mic-tuner/ に入れる更新スクリプト（cpp/*・python/* は不要。パネルにはもう埋め込み済み）
install -m755 vendor/frame-updater/frame-update.sh "$root/vendor/frame-updater/"

mkdir -p dist
out="dist/$name-$version.tar.gz"
# 持ち主の名前は入れない（uid 0 にそろえる）
tar -C "$stage" --owner=0 --group=0 --numeric-owner --sort=name -czf "$out" "$name"
(cd dist && sha256sum "$(basename "$out")" > SHA256SUMS)
echo
tar -tzvf "$out"
echo
cat dist/SHA256SUMS
echo
echo "创建发布（标签 v$version 由 gh release create 生成。发布说明请写在仓库之外的文件里再传进来）："
echo "  gh release create v$version $out dist/SHA256SUMS --title v$version --notes-file <发布说明文件>"
echo "不要设为草稿（--draft）或预发布（--prerelease）：面板的更新看的是 /releases/latest，设了就会找不到。"
echo "SHA256SUMS 也一定要附上（没有它就无法从更新按钮安装，只能让用户手动更新）。"
