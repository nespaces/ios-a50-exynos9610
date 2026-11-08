#!/bin/bash
# The Pixel 10's route, without the phone: QEMU virt at EL2 (virtualization=on) with a GICv4 (256 KB redistributor
# frames, as the Tensor G5's) and eight cores, the boot shim as the kernel QEMU starts (-kernel, as Google's boot loader
# starts it: an arm64 Image, x0 = the device tree, the MMU off, EL2), xnuloader and an XNU kernel in its payload. The
# machine's own device tree, its root "compatible" made "scion,qemu-lga", tells the loader it is xnuloader's
# platform_qemu_lga: the shim, its UEFI services and its PSCI at EL2, the GIC and the cores as on the phone, at QEMU's
# addresses. No UEFI firmware is involved.
#
#   socs/lga/shim/qemu-test.sh [SECS] [boot-args...]       e.g. qemu-test.sh 90 cpus=8 bootcpu=2
#
# env: KERNEL (default: hakios-kernel at the top of the common repository; make frankel_defconfig && make kernel),
#      ROOT_IMG (userland/root.apfs), QEMU_CPU (cortex-a76), QEMU_SMP (8), QEMU_MEM (4G), OUT (out-qemu here),
#      QEMU_MACHINE (virt,virtualization=on,gic-version=4; virt,gic-version=3 enters the shim at EL1, QEMU answering PSCI),
#      SHIM_ARGS: the boot loader's command line as the shim reads it (/chosen/bootargs), e.g. a framebuffer in RAM:
#      SHIM_ARGS=scion.fb=0x100000000,1080,2424,4320 (nothing shows it, but the console, the GOP and XNU's display run;
#      QEMU_MONITOR=SOCKET, a Unix socket for the monitor, whose pmemsave takes a picture of it).
#      The run ends after SECS (loader_return_after=): the loader writes its log and resets, which ends QEMU (-no-reboot).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-$HERE/out-qemu}
SECS=${1:-60}
[ $# -gt 0 ] && shift
L=$TOP/xnuloader/loader.efi
K=${KERNEL:-$TOP/hakios-kernel}
RD=${ROOT_IMG:-$TOP/userland/root.apfs}
for f in "$L" "$K" "$RD"; do
	[ -f "$f" ] || { echo "qemu-test.sh: $f is missing (make loader; make frankel_defconfig && make kernel; userland/build.sh)" >&2; exit 1; }
done
make -s -C "$HERE" shim.bin
mkdir -p "$OUT"
M=${QEMU_MACHINE:-virt,virtualization=on,gic-version=4}
CPU=${QEMU_CPU:-cortex-a76}
SMP=${QEMU_SMP:-8}
MEM=${QEMU_MEM:-4G}

qemu-system-aarch64 -M "$M,dumpdtb=$OUT/virt.dtb" -cpu "$CPU" -smp "$SMP" -m "$MEM" -display none >/dev/null
dtc -q -I dtb -O dts "$OUT/virt.dtb" |
	sed 's/^\tcompatible = "linux,dummy-virt";/\tcompatible = "scion,qemu-lga", "linux,dummy-virt";/' |
	dtc -q -I dts -O dtb -o "$OUT/qemu-lga.dtb"

python3 "$TOP/xnuloader/mkpayload.py" -o "$OUT/payload.bin" loader.efi="$L" kernel="$K" ramdisk="$RD" \
	--cmdline "loader_return_after=$SECS epoch=$(date +%s) $*" >/dev/null
UEFI_TARGET=frankel python3 "$TOP/mkxnuimg.py" --kernel-only -p "$OUT/payload.bin" -o "$OUT/Image"

MON=(-monitor none)
[ -n "${QEMU_MONITOR:-}" ] && MON=(-monitor "unix:$QEMU_MONITOR,server=on,wait=off")
echo "qemu-test.sh: -M $M -cpu $CPU -smp $SMP -m $MEM; boot-args: loader_return_after=$SECS $*${SHIM_ARGS:+; for the shim: $SHIM_ARGS}"
timeout $((SECS + 90)) qemu-system-aarch64 -M "$M" -cpu "$CPU" -smp "$SMP" -m "$MEM" \
	-kernel "$OUT/Image" -dtb "$OUT/qemu-lga.dtb" -append "${SHIM_ARGS:-}" \
	-display none -serial stdio "${MON[@]}" -no-reboot 2>&1 || true
