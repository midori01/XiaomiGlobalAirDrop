#!/data/data/com.termux/files/usr/bin/bash
# Build the static preinit helper with the same flags as the MidoriSU preinit runner.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NOLIBC=${NOLIBC:-$HOME/KernelSU/kernel/preinit/runner/nolibc}
# Installed kernel UAPI headers (Termux's copies are patched for bionic), from:
#   make -C <kernel> ARCH=arm64 LLVM=1 O=<objdir> INSTALL_HDR_PATH=<uapi> headers_install
UAPI=${UAPI:-$HOME/.cache/xga-uapi/include}
OUT=${1:-$HERE/fwsel}
trap 'rm -f "$OUT.o"' EXIT

clang --target=aarch64-linux-gnu -std=gnu11 -Os -Wall -Wextra \
    -nostdinc -isystem "$(clang -print-file-name=include)" \
    -I"$NOLIBC" -I"$UAPI" \
    -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
    -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections \
    -c -o "$OUT.o" "$HERE/fwsel.c"
ld.lld -static -nostdlib -e _start --gc-sections -s \
    -z max-page-size=0x10000 -z noseparate-code -o "$OUT" "$OUT.o"
echo "built $OUT"
