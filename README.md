# Quick Share AirDrop for Xiaomi Global

A MidoriSU module that lets Google Quick Share exchange files with Apple AirDrop on
Xiaomi global ROMs, the way Pixel 10 does.

## Requirements

- MidoriSU. Other KernelSU managers are refused by choice, not for a technical reason; the
  firmware selector is a KernelSU module preinit hook (`preinit/preinit.conf`)
- ZygiskNext, for the GMS flag hook (or GMS Tweaks v2.2.1+, which then sets the flags instead)
- Xiaomi global ROM, Android 16+, kernel 6.12.69, Peach v2 Wi-Fi
  (`popsicle`, `pandora`, `nezha`, `myron`, `pudding`; tested on Xiaomi 17 Ultra / `nezha`)
- The ROM must already ship Google's Quick Share Extension native service
  (`/system_ext/bin/mosey_server`, `libmosey_daemon_ffi.so`, `mosey.rc`)

## How it works

Pixel's AirDrop interop chain is:

    GMS Quick Share -> com.google.android.mosey (Quick Share Extension)
      -> IMoseyService -> mosey_server -> libmosey_daemon_ffi.so (userspace AWDL)
      -> nl80211 vendor commands + radiotap frames -> wonder.ko -> Wi-Fi driver/firmware

Xiaomi global ROMs ship the Mosey native service and SELinux policy, but miss three pieces:

| Missing piece | Provided by |
|---|---|
| Mosey is a plain Play Store app, so it gets no privileged permissions or `mosey_app` domain | this module: `system/product/priv-app/MoseyApp` plus its privapp/default permissions, hidden API allowlist and `com.google.android.moseylib` |
| GMS ignores external sharing providers unless nearby flags `45802344` (provider check) and `45704091` (provider binding) are true; both are only true on Pixel | `zygisk/`: an LSPlant hook in the main GMS process (never `gms.unstable`). Before loading, it asks its root companion whether GMS Tweaks is active and stands down if so, because two LSPlant engines in GMS make it hang; GMS Tweaks v2.2.1+ forces the same flags. Nearby builds its provider list from these flags while GMS starts, so the hook goes in from `Instrumentation.newApplication()`, before any GMS code runs, using a scan result cached per GMS build in GMS's `no_backup` directory. A hook placed later (GMS Tweaks before v2.2.1 polled for the app ClassLoader) loses that race whenever GMS restarts, and Quick Share then never asks Mosey |
| No Wonder support in the stock Wi-Fi driver and firmware | this module: `firmware/`, `kmod/`, `preinit/`, `service.sh` |
| The provider's RX radiotap has no TSFT, so Mosey drops Apple action frames and never syncs to Apple peers: they show up but cannot be sent to | `kmod-src/xga_wonder` (see below) |

### Wonder driver patch

The MiAirDrop driver only admits the Wonder passthrough vdev (opmode 17) when it is the
only vdev on the chip (`psoc->soc_objmgr.wlan_vdev_count == 1` in
`vdevmgr_mlme_ext_hdl_create`). With Wi-Fi on, `wlan0` and `p2p0` already exist, so every
session failed with `Legacy vdev object creation failed` / `-EINVAL`; with Wi-Fi off the
driver idles down and Wonder has no WMA context. `tools/patch-driver.py` turns the two
branches of that count check into a fall-through to the admit path and keeps the trial
flag, WMA and Wonder WMI service checks. Tested on Xiaomi 17 Ultra: AWDL runs on 5745 MHz
while `wlan0` stays connected.

### One Wonder session per WLAN start

The provider admits one Wonder session per WLAN start. A finished session leaves three
things behind that only `hdd_wlan_stop_modules` clears:

- `wt_vdev_trial_claimed`: the next passthrough vdev is rejected (`Legacy vdev object
  creation failed`, `-EINVAL`);
- the "used" flag of the Wonder control (`wt_controls` entry): binding peer events for the
  next session fails, so `Vendor init failed: -16`;
- possibly a closed peer owner (`wt_peer_owners`): the next owner lookup fails with `-108`.

Quick Share Extension ends and restarts its session while it discovers, so after the first
one every restart failed and Quick Share stopped retrying: Apple devices showed up once and
then not again until Quick Share was reopened. `xga_wonder.ko` watches
`hdd_passthrough_unmap_tx_pool` (the session is gone) and clears the three the way the
driver does on WLAN stop, after the old session has unbound its events. The next session
starts about 0.5 s later, and Wi-Fi stays up. `service.sh` falls back to reloading the
driver and Wonder (about 4 s, Wi-Fi drops) if the transport epoch has not moved within 5 s.

Separately, Quick Share Extension itself disconnects Wi-Fi while it runs when the access
point is on 5 or 6 GHz (`Disconnect Wi-Fi because STA and mosey are running on the same
band`), as on Pixel; on 2.4 GHz Wi-Fi stays connected.

### RX TSFT

The provider tags received frames with a radiotap header that carries the channel at most.
Mosey's AWDL stack needs TSFT to sync to an Apple master and drops the frame otherwise
(`Ignoring management frame without required radiotap fields`). Receiving still works, since
the iPhone syncs to us, but sending does not. `xga_wonder.ko` puts a kprobe on
`wonder_rx_handler` and rewrites that header with TSFT and an antenna signal (`signal_dbm`,
default -50). Mosey never reads the local TSF, so the timestamp only has to be monotonic
and in microseconds: the monotonic clock is used, unless something polls
`wondertap_get_mac_tsf` (a kretprobe pairs each reading with the monotonic clock and the
receive time is extrapolated from it). A module notifier re-places the probes whenever
`wonder.ko` is reloaded. `rx_frames`, `patched_frames` and `tsf_samples` under
`/sys/module/xga_wonder/parameters` count its work.
`service.sh` loads it once per boot with `ksud insmod`.
The driver offsets it relies on are checked against `tools/driver/` by its hashes.

### Wi-Fi bring-up and rollback

1. `preinit/bin/fwsel` (static, runs before SELinux) writes `/metadata/xiaomi_global_airdrop/pending`
   and points `firmware_class.path` at `firmware/`.
2. `post-fs-data.sh` verifies the payload checksums and labels the firmware `firmware_file`.
3. `service.sh` waits for the patched firmware (`FW:2.0.0.1.46.17 HW:PEACH_V2`) and clears `pending`,
   then swaps in the Wonder provider build of `qca_cld3_peach_v2.ko`, brings Wi-Fi up once,
   loads `wonder.ko` and enables it.
4. If a patched boot is never confirmed, the next boot falls back to stock firmware and writes
   `failed`. Any later failure in `service.sh` writes `failed` too. Reinstall the module to retry.

The patched driver, `wonder.ko` and firmware come from the MiAirDrop project. Its MiShare bridge
(`xos_bridge`, `libxos.so`) is not used: Google's Mosey stack implements AWDL instead.

## Third-party files

These are not in the repository. Put them at the paths below; `package.sh` checks them
against `blobs.sha256`.

| Path | Source |
|---|---|
| `module/firmware/peach/amss20.bin` | MiAirDrop 2026.11.06.1 zip: `firmware/71fc5c2eb2df048b/peach/amss20.bin` |
| `module/firmware/peach/regdb_xiaomi.bin` | MiAirDrop zip: `firmware/71fc5c2eb2df048b/peach/regdb_xiaomi.bin` |
| `module/kmod/wonder.ko` | MiAirDrop zip: `kmod/wonder.ko` |
| `tools/driver/qca_cld3_peach_v2.orig.ko` | MiAirDrop zip: `kmod/qca_cld3_peach_v2.ko` |
| `module/system/product/priv-app/MoseyApp/MoseyApp.apk` | Pixel 10 series, Android 17: `/product/priv-app/MoseyApp/MoseyApp.apk` (Quick Share Extension 1.0.980159239) |
| `module/system/system_ext/framework/com.google.android.moseylib.jar` | Same Pixel: `/system_ext/framework/com.google.android.moseylib.jar` |

## Build

    ./package.sh        # -> dist/XiaomiGlobalAirDrop-<version>.zip

- `tools/fwsel/build.sh` needs nolibc from a KernelSU source tree (`~/KernelSU/kernel/preinit/runner/nolibc`) and
  installed kernel UAPI headers (`make headers_install`).
- `zygisk/build.sh` links the prebuilt LSPlant, ShadowHook and NDK libc++ static libraries from a
  GMSTweaks checkout (`DEPS`, default `~/GMSTweaks`).
- `tools/patch-driver.py` builds `module/kmod/qca_cld3_peach_v2.ko` from the original MiAirDrop
  driver in `tools/driver/`, checking both input and output hashes.
- `kmod-src/build.sh` builds `xga_wonder.ko` out of tree against an android16-6.12 kernel tree
  (`KSRC`) configured with the device's `/proc/config.gz` (`KOUT`, prepared once with
  `olddefconfig` and `modules_prepare`). `crcs.symvers` holds the device's CRCs for the
  symbols it uses, taken from the stock modules.

## Status

Open the module action in the manager, or run `sh /data/adb/modules/xiaomi_global_airdrop/action.sh`.
The boot log is `service.log` in the module directory.
