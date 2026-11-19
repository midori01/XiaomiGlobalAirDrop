#!/system/bin/sh
SUPPORTED_DEVICES="popsicle pandora nezha myron pudding"
KERNEL_BASE=6.12.69
STATE_DIR=/metadata/xiaomi_global_airdrop

KSUD=/data/adb/ksu/bin/ksud
MIDORISU_ONLY='! This module is for MidoriSU only; other KernelSU managers are not supported.'
[ "$KSU" = true ] && [ -x "$KSUD" ] || abort "$MIDORISU_ONLY"
[ "$("$KSUD" --help 2>/dev/null | head -1)" = 'MidoriSU userspace cli' ] || abort "$MIDORISU_ONLY"
preinit=$("$KSUD" module preinit status 2>&1) || abort "$MIDORISU_ONLY"
case "$(printf '%s\n' "$preinit" | head -1)" in
    *disabled*) abort '! Module preinit is disabled after failed boots; run `ksud module preinit reset` first.' ;;
esac
if printf '%s\n' "$preinit" | awk '$2 == "unsupported" {found = 1} END {exit !found}'; then
    abort '! Module preinit is unavailable on this kernel (late-load mode or preinit=0).'
fi
[ "$ARCH" = arm64 ] || abort '! arm64 only'
[ "$API" -ge 36 ] || abort '! Android 16 or newer is required'

device=$(getprop ro.product.device)
case " $SUPPORTED_DEVICES " in *" $device "*) ;; *) abort "! Unsupported device: $device" ;; esac
case "$(uname -r)" in "$KERNEL_BASE"|"$KERNEL_BASE"[!0-9]*) ;; *) abort "! Kernel $KERNEL_BASE is required" ;; esac
[ -f /vendor_dlkm/lib/modules/qca_cld3_peach_v2.ko ] || abort '! Peach v2 Wi-Fi driver not found'
for f in /system_ext/bin/mosey_server /system_ext/lib64/libmosey_daemon_ffi.so /system_ext/etc/init/mosey.rc; do
    [ -f "$f" ] || abort "! ROM lacks $f (Quick Share Extension native service)"
done
# GMS only binds Quick Share Extension when nearby flags 45802344 and 45704091 are true.
# The bundled Zygisk hook forces them unless GMS Tweaks is active, which must then do it.
[ -d /data/adb/modules/zygisksu ] || ui_print '! ZygiskNext not found: the GMS flags will not be set'
gmstweaks=$(sed -n 's/^versionCode=//p' /data/adb/modules/gmstweaks/module.prop 2>/dev/null)
if [ -n "$gmstweaks" ] && [ "$gmstweaks" -lt 220 ]; then
    ui_print '! GMS Tweaks older than v2.2.0 is installed: update it, or AirDrop stays hidden'
fi

for old in miairdrop_peach_fw miairdrop_xos; do
    if [ -d "/data/adb/modules/$old" ] && [ ! -e "/data/adb/modules/$old/remove" ]; then
        abort "! Remove $old first: it drives the same Wi-Fi firmware and driver"
    fi
done
# Earlier builds of this module used another id.
if [ -d /data/adb/modules/quickshare_airdrop ]; then
    touch /data/adb/modules/quickshare_airdrop/remove
    ui_print '- Old quickshare_airdrop module will be removed on reboot'
fi

(cd "$MODPATH" && sha256sum -c -s SHA256SUMS) || abort '! Payload checksum mismatch'

# A fresh install retries the patched firmware even after an earlier rollback.
rm -rf "$STATE_DIR"

set_perm_recursive "$MODPATH" 0 0 0755 0644
for f in service.sh post-fs-data.sh action.sh uninstall.sh preinit/bin/fwsel; do
    set_perm "$MODPATH/$f" 0 0 0755
done
set_perm_recursive "$MODPATH/firmware" 0 0 0755 0644 u:object_r:firmware_file:s0
ui_print '- Reboot to activate. Open the module action for status.'
