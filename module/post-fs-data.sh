#!/system/bin/sh
MODDIR=${0%/*}
FW_DIR=$MODDIR/firmware
FW_PARAM=/sys/module/firmware_class/parameters/path
HOOK=$MODDIR/zygisk/arm64-v8a.so

# v0.4/v0.5 parked the hook under this name; the hook now asks its root companion
# whether GMS Tweaks is active instead (ZygiskNext loads libraries before this script).
[ -f "$HOOK.off" ] && [ ! -f "$HOOK" ] && mv -f "$HOOK.off" "$HOOK"

# If preinit selected our firmware, make sure the files are intact and readable by
# the kernel loader; otherwise fall back to stock.
[ "$(cat "$FW_PARAM")" = "/data/adb/modules/xiaomi_global_airdrop/firmware" ] || exit 0
if [ -L "$FW_DIR" ] || [ -L "$FW_DIR/peach" ] ||
   ! (cd "$MODDIR" && sha256sum -c -s SHA256SUMS); then
    # Stock firmware for this boot; the unconfirmed pending marker rolls back the next one.
    printf '' > "$FW_PARAM"
    exit 0
fi
chcon -R u:object_r:firmware_file:s0 "$FW_DIR"
