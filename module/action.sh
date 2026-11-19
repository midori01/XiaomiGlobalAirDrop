#!/system/bin/sh
MODDIR=${0%/*}
PKG=com.google.android.mosey
SVC=com.google.android.moseyservice.IMoseyService/default

yesno() { if "$@" >/dev/null 2>&1; then echo yes; else echo NO; fi; }
has() { printf '%s\n' "$dump" | grep -q "$1"; }

echo "Status: $(cat "$MODDIR/status" 2>/dev/null || echo 'not started yet')"
dump=$(dumpsys package "$PKG" 2>/dev/null)
echo "Quick Share Extension: $(pm path "$PKG" 2>/dev/null | head -1 | cut -d: -f2)"
echo "  privileged: $(yesno has 'privateFlags=.*PRIVILEGED')"
for p in MANAGE_WIFI_INTERFACES BLUETOOTH_PRIVILEGED LOCATION_HARDWARE NEARBY_WIFI_DEVICES; do
    echo "  $p: $(yesno has "android.permission.$p: granted=true")"
done
# KernelSU runs this under busybox ash, whose ps lacks LABEL/NAME; use toybox ps.
label() { /system/bin/ps -A -o LABEL,NAME 2>/dev/null | awk -v p="$1" '$2 == p {print $1; exit}'; }
l=$(label "$PKG")
echo "  process: ${l:-not running (started on demand by Quick Share)}"
l=$(label mosey_server)
echo "mosey_server: ${l:-$(getprop init.svc.mosey_server) (lazy service, starts on demand)}"
if service check "$SVC" 2>/dev/null | grep -q ': found'; then
    echo "  service registered: yes"
elif [ -z "$l" ]; then
    echo "  service registered: no (normal while idle)"
else
    echo "  service registered: NO"
fi
G=/data/adb/modules/gmstweaks
if [ -d "$G" ] && [ ! -e "$G/disable" ] && [ ! -e "$G/remove" ]; then
    hook="GMS Tweaks"
    n=$(logcat -d -s QSUZygisk 2>/dev/null | grep -E 'Flag (45802344|45704091) hook active' | grep -o -E '4580234[4]|4570409[1]' | sort -u | wc -l)
else
    hook="bundled hook"
    n=$(logcat -d -s XGAirDrop 2>/dev/null | grep 'forced true' | grep -o -E '4580234[4]|4570409[1]' | sort -u | wc -l)
fi
if [ "$n" -gt 0 ]; then
    echo "GMS flags ($hook): $n of 2 forced (seen in logcat)"
else
    echo "GMS flags ($hook): not in logcat (the boot log may have rotated out; reboot and rerun to check)"
fi
echo "Wi-Fi firmware: $(cat /sys/kernel/peach_v2/wlan/fw/[0-9]*/version 2>/dev/null | head -1)"
echo "Wonder: loaded=$(yesno test -d /sys/module/wonder) enable=$(cat /proc/sys/net/wonder/wonder_enable 2>/dev/null)"
echo "  xga_wonder: loaded=$(yesno test -d /sys/module/xga_wonder) RX TSFT patched=$(cat /sys/module/xga_wonder/parameters/patched_frames 2>/dev/null) of $(cat /sys/module/xga_wonder/parameters/rx_frames 2>/dev/null) in-place rearms=$(cat /sys/module/xga_wonder/parameters/rearms 2>/dev/null)"
echo "  wiphys: $(ls /sys/class/ieee80211 2>/dev/null | tr '\n' ' ')"
echo "  ifaces: $(ls /sys/class/net 2>/dev/null | grep -E 'wonder|radiotap|awdl' | tr '\n' ' ')"
