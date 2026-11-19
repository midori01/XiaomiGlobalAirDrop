#!/data/data/com.termux/files/usr/bin/python3
"""Relax the exclusive-trial admission in MiAirDrop's qca_cld3_peach_v2.ko.

vdevmgr_mlme_ext_hdl_create() only admits the Wonder passthrough vdev (opmode 17)
when it is the only vdev on the psoc (psoc->soc_objmgr.wlan_vdev_count == 1). With
Wi-Fi on, wlan0/p2p0 already exist, so every Wonder session fails with -EINVAL; with
Wi-Fi off the driver idles down and Wonder has no WMA. Skip only that count check;
the trial flag, WMA and Wonder WMI service checks stay in place.

Usage: patch-driver.py <original.ko> <patched.ko>
"""
import hashlib
import struct
import sys

ORIGINAL_SHA256 = "39c9e666dd5cb5fea67b695f3bdb8d1aeac27f16397ba7168c983f2b04df25cc"
PATCHED_SHA256 = "e1edd4cb64ef578bb2ee011fc2fc82676163f15531671ba7f6d240e53300a0f4"
FUNCTION = b"vdevmgr_mlme_ext_hdl_create"

# Offsets inside FUNCTION: (original instruction, replacement)
PATCHES = {
    0xdc: ("e1210054", "1f2003d5"),   # b.ne <reject>      -> nop
    0x514: ("60deff54", "f3feff17"),  # b.eq <admit>       -> b <admit>
}


def sections(elf):
    shoff = struct.unpack_from("<Q", elf, 0x28)[0]
    shentsize, shnum = struct.unpack_from("<HH", elf, 0x3A)
    return [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize) for i in range(shnum)]


def find_function(elf, secs):
    for sec in secs:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[sec[6]]
        for off in range(sec[4], sec[4] + sec[5], 24):
            st_name, st_info, _, st_shndx, st_value, _ = struct.unpack_from("<IBBHQQ", elf, off)
            start = strtab[4] + st_name
            if elf[start:start + len(FUNCTION) + 1] == FUNCTION + b"\0":
                return st_shndx, st_value
    raise SystemExit("symbol not found")


def main():
    src, dst = sys.argv[1:3]
    elf = bytearray(open(src, "rb").read())
    if hashlib.sha256(elf).hexdigest() != ORIGINAL_SHA256:
        raise SystemExit("unexpected input driver (sha256 mismatch)")
    secs = sections(elf)
    shndx, value = find_function(elf, secs)
    base = secs[shndx][4] + value
    for rel, (old, new) in PATCHES.items():
        pos = base + rel
        if elf[pos:pos + 4] != bytes.fromhex(old):
            raise SystemExit(f"unexpected instruction at +{rel:#x}")
        elf[pos:pos + 4] = bytes.fromhex(new)
    if hashlib.sha256(elf).hexdigest() != PATCHED_SHA256:
        raise SystemExit("patched driver sha256 mismatch")
    open(dst, "wb").write(elf)
    print(f"patched {dst}")


if __name__ == "__main__":
    main()
