#!/bin/bash
# A boot image for one run on the Galaxy A50 (a50): the SoC's boot shim (devices/a50/soc/shim -- the Tensor G5 shim's
# design, no UEFI) plus the payload, as an Android boot image of header v1 for S-Boot (mkxnuimg.py, UEFI_TARGET=a50).
# In TWRP: Install, Install Image to Boot; power on. A restart goes back to recovery (the loader's
# recovery_after_reset), where the run's log is read from Samsung's pstore RAM.
#
#   devices/a50/mkimage.sh [-o xnu-a50.img] [-k kernel] [-r ramdisk] [-s SECS] [-q] [boot-args...]
#
# The kernel is this phone's configuration (make a50_defconfig && make kernel: hakios-kernel); the boot-arguments
# default to that configuration's (tools/hakios-config.py args) plus isa_floor=1 (every core is ARMv8.0), with -v
# unless -q. The loader is xnuloader/loader.efi (make loader-open: the placeholder identity is enough for the hello
# payload): the image is yours, never to be published.
set -e
START=$PWD
NAME=$(basename "$(cd "$(dirname "$0")" && pwd)")
[ "$NAME" = a50 ] || { echo "mkimage.sh: devices/$NAME: this script is the Galaxy A50's (devices/a50)" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
OUT=$ROOT/xnu-a50.img K=$ROOT/hakios-kernel RD=$ROOT/userland/hello SECS=120 VERBOSE=-v
while getopts o:k:r:s:q opt; do
	case $opt in
	o) OUT=$OPTARG ;; k) K=$OPTARG ;; r) RD=$OPTARG ;; s) SECS=$OPTARG ;; q) VERBOSE= ;;
	*) sed -n 2,14p "$0"; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
abs() { case "$1" in /*) echo "$1" ;; *) echo "$START/$1" ;; esac; }
OUT=$(abs "$OUT") K=$(abs "$K") RD=$(abs "$RD")
L=xnuloader/loader.efi
for f in "$L" "$K" "$RD"; do
	[ -f "$f" ] || { echo "mkimage.sh: $f is missing (make kernel, make loader-open hello)" >&2; exit 1; }
done
[ -f devices/a50/soc/shim/shim.bin ] || make -C devices/a50/soc/shim
ARGS="$*"
if [ -z "$ARGS" ]; then
	grep -q '^CONFIG_DEVICE="a50"' .config 2>/dev/null || {
		echo "mkimage.sh: .config is not the a50's: make a50_defconfig first, or give the boot-arguments" >&2; exit 1; }
	ARGS=$(python3 tools/hakios-config.py args)
fi
case " $ARGS " in
*" isa_floor="*) ;;
*) ARGS="isa_floor=1 $ARGS" ;;
esac
ARGS="${VERBOSE:+$VERBOSE }$ARGS"
P=$(mktemp "${TMPDIR:-/tmp}/a50-payload.XXXXXX")
trap 'rm -f "$P"' EXIT
python3 xnuloader/mkpayload.py -o "$P" loader.efi="$L" kernel="$K" ramdisk="$RD" \
	--cmdline "loader_return_after=$SECS epoch=$(date +%s) $ARGS" >/dev/null
UEFI_TARGET=a50 python3 mkxnuimg.py -p "$P" -o "$OUT"
echo "mkimage.sh: $OUT (a50, the boot shim, no UEFI): in TWRP, Install Image to Boot; power on"
if [ "$SECS" = 0 ]; then LIMIT="no time limit"; else LIMIT="the run returns after $SECS s"; fi
echo "mkimage.sh: boot-args: $ARGS; $LIMIT"
