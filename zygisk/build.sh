#!/data/data/com.termux/files/usr/bin/bash
# Build the Zygisk flag hook: Java payload (Entry + DexScanner) embedded into an
# arm64 shared library that links LSPlant and ShadowHook statically.
# The prebuilt static libraries are reused from a GMSTweaks checkout (see its DESIGN.md).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DEPS=${DEPS:-$HOME/GMSTweaks}
OUT=$HERE/out
DEST=${1:-$HERE/../module/zygisk/arm64-v8a.so}
PKG=com/midori/xga

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$(dirname "$DEST")"

javac -d "$OUT/classes" "$HERE/java/src/$PKG/DexScanner.java" "$HERE/java/src/$PKG/Entry.java"
d8 --min-api 26 --output "$OUT/dex" "$OUT/classes/$PKG"/*.class
python3 -I - "$OUT/dex/classes.dex" "$HERE/src/payload_dex.h" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('#pragma once\n#include <cstddef>\nstatic const unsigned char kPayloadDex[] = {\n')
    for i in range(0, len(data), 16):
        f.write('  ' + ','.join(str(b) for b in data[i:i + 16]) + ',\n')
    f.write('};\nstatic const size_t kPayloadDexLen = %d;\n' % len(data))
EOF

CXX="clang++ --target=aarch64-linux-android24 -std=c++20 -O2 -fPIC -Wall -Wextra"
$CXX -I"$HERE/src" \
    -I"$DEPS/vendor/shadowhook/shadowhook/src/main/cpp/include" \
    -I"$DEPS/vendor/lsplant/lsplant/src/main/jni/include" \
    -c "$HERE/src/module.cpp" -o "$OUT/module.o"
$CXX -c "$HERE/src/libcxx_shim.cpp" -o "$OUT/libcxx_shim.o"

CRTDIR=$(dirname "$(clang++ --target=aarch64-linux-android24 -print-file-name=crtbegin_so.o)")
BUILTINS=$(clang++ --target=aarch64-linux-android24 -print-libgcc-file-name)
ld.lld -shared --no-undefined -soname libxgairdrop.so \
    --hash-style=both -z noexecstack -z relro -z now \
    -o "$OUT/libxgairdrop.so" \
    "$CRTDIR/crtbegin_so.o" "$OUT/module.o" "$OUT/libcxx_shim.o" \
    "$DEPS/vendor/lsplant/build64/liblsplant.a" \
    "$DEPS/vendor/lsplant/build64/external/dex_builder/libdex_builder_static.a" \
    "$DEPS/vendor/shadowhook/build/libshadowhook.a" \
    "$DEPS/ndklibs/libc++_static.a" "$DEPS/ndklibs/libc++abi.a" \
    -L/system/lib64 -llog -lc -ldl -lm -lz \
    "$BUILTINS" "$CRTDIR/crtend_so.o"

if llvm-readelf -d "$OUT/libxgairdrop.so" | grep -qiE 'RUNPATH|RPATH'; then
    echo "RUNPATH/RPATH found, refusing to ship" >&2
    exit 1
fi
cp -f "$OUT/libxgairdrop.so" "$DEST"
llvm-strip "$DEST"
echo "built $DEST ($(wc -c < "$DEST") bytes)"
