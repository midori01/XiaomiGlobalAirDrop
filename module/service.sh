#!/system/bin/sh
# Late boot: confirm the patched firmware, swap in the Wonder-capable driver, load Wonder,
# and grant Quick Share Extension its runtime permissions.
MODDIR=${0%/*}
STATE_DIR=/metadata/xiaomi_global_airdrop
FW_PARAM=/sys/module/firmware_class/parameters/path
FW_DIR=/data/adb/modules/xiaomi_global_airdrop/firmware
FW_EXPECTED='FW:2.0.0.1.46.17 HW:PEACH_V2'
DRIVER=qca_cld3_peach_v2
WONDER_PROVIDER=/soc/qcom,cnss-vendor-wlan-wonder
MOSEY=com.google.android.mosey
LOG=$MODDIR/service.log

exec >"$LOG" 2>&1
status() {
    printf '%s\n' "$*" >"$MODDIR/status.new" && mv "$MODDIR/status.new" "$MODDIR/status"
    echo "$(cut -d' ' -f1 /proc/uptime) $*"
}
settle() { sleep "${1:-1}"; }
wait_for() {
    # wait_for <tries> <command...>: poll once a second
    n=$1; shift
    while [ "$n" -gt 0 ]; do
        "$@" && return 0
        n=$((n - 1)); settle
    done
    return 1
}
# Android shell commands hand their stdio to system_server over binder, which cannot
# use this script's log file; give them /dev/null and pipes instead.
android() { "$@" </dev/null 2>&1 | cat; }
setting() { android settings get global "$1"; }
live_firmware() { cat /sys/kernel/peach_v2/wlan/fw/[0-9]*/version 2>/dev/null | head -1; }
# The version line continues with board details, so compare only its first two fields.
firmware_live() { [ "$(live_firmware | cut -d' ' -f1-2)" = "$FW_EXPECTED" ]; }
wlan_up() { [ -r /sys/class/net/wlan0/flags ] && [ $(( $(cat /sys/class/net/wlan0/flags) & 1 )) = 1 ]; }
driver_live() { [ "$(cat /sys/module/$DRIVER/initstate 2>/dev/null)" = live ]; }
driver_idle() { [ "$(cat /sys/module/$DRIVER/refcnt 2>/dev/null)" = 0 ]; }
boot_done() { [ "$(getprop sys.boot_completed)" = 1 ]; }
service_state() { [ "$(getprop init.svc.$1)" = "$2" ]; }
wonder_ready() { [ -r /proc/sys/net/wonder/wonder_enable ]; }
fail() {
    # Any failure after the stock driver is touched needs a clean boot; block future
    # patched boots until the module is reinstalled.
    printf '%s\n' "$*" >"$STATE_DIR/failed"
    status "Failed: $*. Patched Wi-Fi disabled; reinstall the module to retry."
    exit 1
}

wait_for 300 boot_done || exit 0

for p in BLUETOOTH_SCAN BLUETOOTH_ADVERTISE BLUETOOTH_CONNECT NEARBY_WIFI_DEVICES \
         ACCESS_FINE_LOCATION ACCESS_COARSE_LOCATION ACCESS_BACKGROUND_LOCATION \
         ACCESS_MEDIA_LOCATION ACCESS_LOCAL_NETWORK; do
    android pm grant "$MOSEY" "android.permission.$p" >/dev/null
done

if [ -f "$STATE_DIR/failed" ]; then
    status "Stock Wi-Fi: rolled back after a failed boot ($(cat "$STATE_DIR/failed")). Reinstall to retry."
    exit 0
fi
if [ "$(cat "$FW_PARAM")" != "$FW_DIR" ]; then
    status 'Stock Wi-Fi: patched firmware was not selected this boot.'
    exit 0
fi

# 1. Confirm the patched firmware is running. Wi-Fi must be on once for the chip to boot.
saved_wifi=$(setting wifi_on)
saved_scan=$(setting wifi_scan_always_enabled)
if ! firmware_live && [ "$saved_wifi" = 0 ]; then
    android cmd wifi set-wifi-enabled enabled
fi
wait_for 120 firmware_live || fail "firmware '$(live_firmware)' is not $FW_EXPECTED"
rm -f "$STATE_DIR/pending"
sync
status 'Patched firmware confirmed; loading the Wonder driver...'

# Quiesce Wi-Fi, (re)load the Wonder provider build of the driver, bring Wi-Fi up once
# (Wonder needs the WMI services set up by ndo_open), load Wonder, restore preferences.
load_wonder_stack() {
    wifi=$(setting wifi_on)
    scan=$(setting wifi_scan_always_enabled)
    # A forced country code lives in the framework only; a fresh driver instance boots
    # with its own default (e.g. the SIM's), so it has to be pushed again afterwards.
    forced_cc=$(dumpsys wifi 2>/dev/null </dev/null | sed -n 's/^mOverrideCountryCode: //p' | head -1)
    hal=$(getprop init.svc.vendor.wifi_hal_legacy)
    wpa=$(getprop init.svc.wpa_supplicant)
    android cmd wifi set-scan-always-available disabled
    android cmd wifi set-wifi-enabled disabled
    stop wpa_supplicant
    stop vendor.wifi_hal_legacy
    wait_for 25 service_state wpa_supplicant stopped || fail 'wpa_supplicant did not stop'
    wait_for 25 service_state vendor.wifi_hal_legacy stopped || fail 'Wi-Fi HAL did not stop'
    [ -d /sys/module/wonder ] && { rmmod wonder || fail 'rmmod of wonder.ko failed'; }
    wait_for 10 driver_idle || fail 'Wi-Fi driver still in use'

    rmmod "$DRIVER" || fail 'rmmod of the Wi-Fi driver failed'
    insmod "$MODDIR/kmod/$DRIVER.ko" wonder_exclusive_trial=1 wonder_idle_rearm=1 || fail 'insmod of the patched driver failed'
    wait_for 10 driver_live || fail 'patched driver did not go live'
    wait_for 30 test -d /sys/class/net/wlan0 || fail 'wlan0 did not appear'

    [ "$hal" = running ] && start vendor.wifi_hal_legacy
    [ "$wpa" = running ] && start wpa_supplicant
    android cmd wifi set-wifi-enabled enabled
    wait_for 45 wlan_up || fail 'wlan0 did not come up on the patched driver'
    case "$forced_cc" in
        [A-Z0-9][A-Z0-9]) android cmd wifi force-country-code enabled "$forced_cc" ;;
    esac

    insmod "$MODDIR/kmod/wonder.ko" provider_path="$WONDER_PROVIDER" runtime_stage=2 tx_enabled=1 || fail 'insmod of wonder.ko failed'
    wait_for 15 wonder_ready || fail 'Wonder sysctl did not appear'
    echo 1 > /proc/sys/net/wonder/wonder_enable || fail 'cannot enable Wonder'

    [ "$scan" = 1 ] && android cmd wifi set-scan-always-available enabled
    [ "$wifi" = 0 ] && android cmd wifi set-wifi-enabled disabled
}

# xga_wonder adds the TSFT that the provider's RX radiotap lacks (without it Quick Share
# Extension cannot sync to Apple peers or send to them) and rearms the provider in place
# when a session ends. It follows wonder.ko across reloads. Built out of tree, so it goes
# through ksud's loader (symbol binding, vermagic).
if [ ! -d /sys/module/xga_wonder ]; then
    /data/adb/ksu/bin/ksud insmod "$MODDIR/kmod/xga_wonder.ko" >/dev/null 2>&1 ||
        echo "$(cut -d' ' -f1 /proc/uptime) xga_wonder did not load; sending to Apple devices will not work"
fi

# 2. Swap the stock driver for the Wonder provider build.
android cmd wifi set-wifi-enabled "$([ "$saved_wifi" = 0 ] && echo disabled || echo enabled)"
load_wonder_stack
status 'Ready: Quick Share can reach AirDrop devices.'

# 3. The provider admits one Wonder session per WLAN start and rearms only after a full
# WLAN stop, which can take 10-20 s; Quick Share restarts discovery every few seconds, so
# sending to Apple devices fails in that gap. xga_wonder rearms it in place within about
# a second (the epoch moves); if that does not happen, reload the stack instead.
epoch() { cat /sys/module/$DRIVER/parameters/wonder_transport_epoch 2>/dev/null; }
epoch_moved() { [ "$(epoch)" != "$before" ]; }
last_rearm=0
/system/bin/dmesg -W | while read -r line; do
    case "$line" in *'passthrough TX pool unmapped'*) ;; *) continue ;; esac
    before=$(epoch)
    wait_for 5 epoch_moved
    now=$(cut -d. -f1 /proc/uptime)
    [ "$(epoch)" != "$before" ] && continue
    [ $((now - last_rearm)) -lt 10 ] && continue
    [ "$(setting wifi_on)" = 1 ] || continue
    last_rearm=$now
    echo "$(cut -d' ' -f1 /proc/uptime) Wonder session ended; reloading the Wi-Fi stack to rearm it"
    setprop ctl.stop mosey_server
    wait_for 10 service_state mosey_server stopped
    load_wonder_stack
    echo "$(cut -d' ' -f1 /proc/uptime) rearmed"
done
