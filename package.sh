#!/data/data/com.termux/files/usr/bin/bash
# Build the native parts and package the flashable module zip into dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
M=$HERE/module
VERSION=$(sed -n 's/^version=//p' "$M/module.prop")
ZIP=$HERE/dist/XiaomiGlobalAirDrop-$VERSION.zip

# Third-party binaries are not in the repository; see "Third-party files" in README.md.
(cd "$HERE" && sha256sum --quiet -c blobs.sha256) || {
    echo "missing or different third-party files; see \"Third-party files\" in README.md" >&2
    exit 1
}

bash "$HERE/tools/fwsel/build.sh" "$M/preinit/bin/fwsel"
bash "$HERE/zygisk/build.sh" "$M/zygisk/arm64-v8a.so"
bash "$HERE/kmod-src/build.sh" "$M/kmod/xga_wonder.ko"
python3 -I "$HERE/tools/patch-driver.py" "$HERE/tools/driver/qca_cld3_peach_v2.orig.ko" "$M/kmod/qca_cld3_peach_v2.ko"

# Only paths the installer never moves; system/product and system/system_ext are
# relocated to the module root after customize.sh, and post-fs-data.sh renames zygisk/.
(cd "$M" && find firmware kmod preinit -type f | sort | xargs sha256sum) > "$M/SHA256SUMS"

mkdir -p "$HERE/dist"
rm -f "$ZIP"
(cd "$M" && zip -q -r -X "$ZIP" .)
echo "packaged $ZIP ($(wc -c < "$ZIP") bytes)"
