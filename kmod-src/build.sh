#!/data/data/com.termux/files/usr/bin/bash
# Build xga_wonder.ko against an android16-6.12 tree configured with the device's
# /proc/config.gz (KBUILD_OUTPUT prepared once with olddefconfig + modules_prepare).
# Termux host-tool workarounds: bcmp, bionic <elf.h> gaps, and two host tools that
# external modules do not need (SELinux genheaders, asn1_compiler).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
KSRC=${KSRC:-$HOME/common_android_kernel_tree}
KOUT=${KOUT:-$HOME/.cache/xga-kbuild}
DEST=${1:-$HERE/../module/kmod/xga_wonder.ko}

make -s -C "$KSRC" O="$KOUT" M="$HERE/xga_wonder" ARCH=arm64 LLVM=1 \
    HOSTCC="clang -Dbcmp=memcmp -include $HERE/termux-elf-shim.h" \
    CONFIG_SECURITY_SELINUX= CONFIG_ASN1= CONFIG_MODULE_SIG_PROTECT= KBUILD_EXTRA_SYMBOLS=$HERE/crcs.symvers KBUILD_MODPOST_WARN=1 modules 2>&1 |
    grep -v -E 'WARNING: modpost: ".*" \[.*\] undefined!' || true
cp -f "$HERE/xga_wonder/xga_wonder.ko" "$DEST"
llvm-strip --strip-debug "$DEST"
echo "built $DEST ($(wc -c < "$DEST") bytes)"
