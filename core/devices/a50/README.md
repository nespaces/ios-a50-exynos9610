# devices/a50: the Samsung Galaxy A50 (SM-A505FN, Exynos 9610)

First bring-up: the common HakiOS kernel and a raw-syscall PID 1, booted through the SoC's own boot shim
(soc/shim, the Tensor G5 shim's design: socs/lga) instead of a UEFI -- there is none for this phone.

## Where each fact comes from

- The phone's live device tree, read in TWRP (`a50-fdt.dtb`): the GIC-400 (distributor 0x12301000, CPU interface
  0x12302000), the arch timer at 26 MHz, PSCI by SMC, the PMU (`system-controller@11860000`), both cluster
  watchdogs (0x10050000/0x10060000, as on the S9), the four memory banks (0x80000000 + 0x3AB00000, 0xC0000000 +
  0x20000000, 0xE1900000 + 0x1E700000, 0x880000000 + 0x80000000) and every reserved region (the modem's, ABOX's,
  exynos-snapshot's, the EL2 hypervisor "uh"'s).
- The boot arguments S-Boot leaves in that tree: `s3cfb.bootloaderfb=0xca000000` (the panel's buffer, 1080 x 2340,
  the EA8076), `androidboot.bootloader=A505FNPUS9CVJ2`.
- The CPU order: four Cortex-A53s at MPIDR 0x000-0x003, four Cortex-A73s at 0x100-0x103 (the device tree's cpus).
  All eight are ARMv8.0: no PAN anywhere (the kernel's own check, xnu/patches/1509), `isa_floor=1` for user space
  (xnu/patches/1510).
- The storage is UFS (0x13520000; in TWRP: /dev/block/platform/13520000.ufs, userdata on sda32), not eMMC.

## The boot

S-Boot loads the image's "kernel" (the shim plus the XNUP payload, uncompressed, header v1, mkxnuimg.py's a50
packing) and jumps to it at EL1 with the device tree in x0 (the "uh" hypervisor keeps EL2). The shim maps RAM,
stops the cluster watchdogs S-Boot may have left, finds the framebuffer (s3cfb.bootloaderfb=), and starts
xnuloader as a UEFI application. The loader knows this phone as platform "a50" (by the tree's
"samsung,exynos9610", and by the framebuffer at 0xCA000000). A restart is the PMU's SWRESET with INFORM2/3 set
for recovery: the run returns to TWRP, the log stays in Samsung's pstore RAM (log_pstore 0xF9D10000).

## What runs (November 2026)

iOS 16.5's SpringBoard with touch, keys, battery, the Mali GPU through Mesa/Panfrost and seven cores; the system
volume from SYSTEM and `/private/var` on USERDATA, both through the kernel's own UFS driver. See the top-level
README for the table.

## The boot-arguments that work

    -q debug=0x8 rd=md0 cpus=7 small_cores_after=0 psci_low=0xDA400000 bootcpu=0 esslog=2
    exynos_keys=2 melfas=1 sm5713=1 acpm_soc=9610
    mali=9 mali_soc=9610 mali_mhz=683 mali_uv=750000
    ufs_native=4 ufs_native_soc=exynos9610 ufs_native_step=5 ufs_native_prd=16 ufs_native_ref_fnv=<YOURS>
    ufs_native_ro=0 ufs_native_write=userdata ufs_native_data=1 var_hfs=ufs_userdata_data var_save_ms=0
    system_apfs=ufs_system fb_threads=6 fb_rt=1 fb_ca_hz=120

- `ufs_native_ref_fnv`: the driver checks its first reads against the host's: FNV-1a (64 bits) of the first 8192
  bytes of LUN 0, which holds the partition table and so differs from phone to phone. In TWRP:
  `adb exec-out "dd if=/dev/block/sda bs=4096 count=2 2>/dev/null" > lun0.bin`, then
  `python3 -c "import sys;h=0xcbf29ce484222325
for b in open(sys.argv[1],'rb').read(8192): h=((h^b)*0x100000001b3)&(2**64-1)
print(hex(h))" lun0.bin`.
- `psci_low=0xDA400000`: Samsung's EL3 keeps only 32 bits of CPU_ON's entry, so secondary cores start at a
  trampoline below 4 GB (xnu/patches/1520).
- `mali_mhz=683 mali_uv=750000`: the phone's own fvmap gives 737.5 mV for 683 MHz; 764 MHz showed artefacts.
- `fb_threads=6 fb_rt=1 fb_ca_hz=120`: the scaling shared over six cores, the window server's render thread
  real-time, CoreAnimation told 120 Hz -- about 60 frames a second in the UI (10 without).
- `acpm9610=6` (optional): loads and attaches Samsung's `fvp` plugin (tools/a50-fvp.py) before anything asks the
  DVFS channel; asking it first leaves the APM firmware dead until the next boot.

## Installing (erases Android)

In TWRP, back up every partition you care about (at least BOOT, SYSTEM, EFS) to a computer first.

1. SYSTEM: the iOS system volume (`system.apfs` from `userland/build.sh`), `dd` to `/dev/block/by-name/system`.
2. USERDATA: an empty HFS+ volume of its size: `tools/mkdatavol.py --userdata BLOCKS OUTDIR` (BLOCKS from
   `blockdev --getsize64 /dev/block/by-name/userdata` / 4096), then the head and the tail as `userdata.txt` says
   (`dd ... conv=notrunc`).
3. BOOT: `devices/a50/mkimage.sh -o xnu-a50.img -r userland/root.apfs -s 0 <boot-arguments>`, `dd` to
   `/dev/block/by-name/boot`.

`-s N` makes a run return to TWRP after N seconds (the log is then in `/sys/fs/pstore`); `-s 0` runs for good.
