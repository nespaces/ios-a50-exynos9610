#!/bin/bash
# Build the user-space side of the bring-up.
#
#   hello            one raw-syscall program; as a RAM disk on its own it boots through mockfs
#   root.apfs        the small image: our init and test programs, etc/rc, and the launchd jobs
#                    (LaunchDaemons/), an APFS container (../tools/apfs/mkapfs.py). It travels in
#                    the payload next to the kernel (the second RAM disk, /dev/md1) and is mounted
#                    on /redwood. Rebuilt every time.
#                      ALONE=1      the small image is the whole root, with no system image beside it (the Galaxy
#                                   S9's BOOT image, which has room for nothing more): init is its /sbin/launchd,
#                                   /redwood points back at the image itself, and etc/rc is rc.alone unless RC= says
#                                   otherwise -- init's checks, then a wait while the kernel's drivers log.
#                                   With RC=rc.launchd and boot-arg system_apfs=ufs_NAME, init makes the system
#                                   image on that partition the root and the boot goes on as a RAM boot's
#                      VK_DIR=DIR   with the Vulkan library of the guest (gfxstream's Vulkan over /dev/vgpu3d,
#                                   tools/gfxstream/VULKAN.md) and its test, built by tools/gfxstream/mk-vk.sh ios
#                                   (which needs Xcode's clang): DIR/libhakios.vk.dylib into lib/, DIR/vk_test
#                                   into bin/ (rc.jobs runs it once; without it that line just says "not found")
#                      RC=<file>    etc/rc: rc.launchd (default) hands process 1 to the Darwin launchd
#                                   after init's checks; rc.darwin runs a few Darwin programs and halts
#                      JOBS="..."   labels of the stock Darwin launchd jobs to load (default: DARWIN_JOBS)
#                      REST=1       ... and the rest of them, started only when asked for (see mkjobs.py)
#                      WIFID=0      without wifid, Darwin's Wi-Fi daemon, which is in by default (see below)
#                      OSLOG="..."  labels of the jobs whose os_log output goes to the console ("all": every
#                                   one of JOBS; default: only the window server, which says little)
#                      INJECT="..." the programs started with a library of ours, "LIBRARY NAME..." per line
#                                   (default: src/inject.c in backboardd, src/inject_sb.c in SpringBoard,
#                                   src/inject_media.c in mediaserverd);
#                                   init hands the list to the kernel (etc/inject.conf -> sysctl kern.exec_inject)
#                      BT=1         Bluetooth on the Galaxy S9 (kernel bt=7, drivers/bluetooth/hakios.bcm4361_bt.c):
#                                   the jobs com.apple.bluetoothd (its transport BT_HCI_TRANSPORT=H4:POSIX:
#                                   com.apple.uart.bluetooth,4000000 -- the kernel's control at the chip's rate; give
#                                   BT_HCI_TRANSPORT= for another) and com.apple.BlueTool; BlueTool started with
#                                   lib/libhakios.bluetool.dylib (src/inject_bluetool.c), which has it run this
#                                   phone's own scripts, etc/bluetool/ (bluetool/ here), instead of Apple's firmware
#                                   loading. devices/starlte/BLUETOOTH.md, "iOS"
#                      EXTRA="..."  more programs for /redwood/bin (files on this machine): one-off
#                                   diagnostics, which a file given as RC_JOBS can then run
#                      EXTRA_SHARE="..." directories on this machine to copy whole into /redwood/share,
#                                   each under its own name: the data a one-off diagnostic reads, when it
#                                   needs more than a program (tools/air/intrincheck.py --emit writes such
#                                   a directory). This goes into the boot volume, which every recipe
#                                   carries and which the flash recipe puts in RAM, so its size is paid
#                                   for at every boot -- a few megabytes, not more
#                      SEED=<file>  a seed of the data volume, as share/var.seed: /private/var as an earlier
#                                   boot left it (tools/var-seed.py takes it out of a store's disk image), which
#                                   init unpacks over the template with boot-arg var_seed=1 (src/var_store.c).
#                                   It travels in this image, so its size is paid in RAM at every boot
#                      RC_JOBS=<file> what the job com.hakios.demo runs: rc.jobs (default), or
#                                   rc.jobs.unlock, which also swipes the lock screen away (for the phone,
#                                   where nothing else can touch the screen yet).
#                                   A job list cannot assume a shell. Darwin 16.5's own root has none at all
#                                   -- /bin is df and ps, and nothing else -- so a line beginning /bin/sh
#                                   fails with "No such file or directory" and status 127 on a flash-root
#                                   boot. The /var/jb/bin/sh that rc.jobs.jb and rc.jobs.sileo run is the
#                                   package manager's prefix, which only a JB=1 image carries. And init
#                                   runs these lines itself (src/init.c, run_command): words split on
#                                   blanks, no quoting, no variable assignment, no redirection beyond a
#                                   trailing ">> FILE" and "&". So a program that needs a value passed to
#                                   it needs an option for it, not an environment variable -- which is why
#                                   air_run grew --buffer.
#                      GL=1         with an OpenGL ES implementation for the window server: Mesa
#                                   (tools/mk-mesa.sh fetches and builds it) behind src/eagl.c, as
#                                   lib/libOSMesa.8.dylib (16 MB, freedreno and panfrost) and
#                                   lib/libhakios.gl.dylib. Where the kernel has started the phone's GPU,
#                                   CoreAnimation then draws with its OpenGL ES renderer, by that GPU
#                                   (boot-arg gpu=1); anywhere else -- QEMU, no boot-arg, no firmware in
#                                   ../fw-gpu -- the library is not even loaded and nothing changes
#                                   (src/inject.c, opengl_es()). The Galaxy S9's Mali counts as well: its
#                                   service, HakiosMali, is there once the kernel's mali=9 has brought it up.
#                                   NO_AIR=1 beside it: omit the AIR compiler library when testing GLES alone.
#                                   Leave it unset for the Mali Metal path (devices/starlte/GPU.md).
#                                   GL=gfx: the library draws with the HOST's GPU instead: gfxstream over
#                                   virtio-gpu (src/gfx, /dev/vgpu3d; AVF on the Pixel: the PowerVR), as
#                                   lib/libhakios.gl.dylib (no Mesa), plus bin/gfx_test. Boot-arg gl=1 loads
#                                   it into the window server.
#                                   GL=test: only the library and bin/gl_test (a triangle, src/gl_test.c;
#                                   RC_JOBS=rc.jobs.gltest runs it); the window server stays as it is.
#                      METAL=vk     the Metal plug-in (lib/HakiosMetal.bundle) is Metal on Vulkan instead of src/HakiosMetal.m:
#                                   AdrenoMetal's frontend (src/metalvk/) over lib/libmetal2vulkan.dylib, metal2vulkan's runtime
#                                   (tools/metal/mk-metal2vulkan.sh ios builds it; 18 MB), on the Vulkan driver at
#                                   /redwood/lib/libhakios.vk.dylib (gfxstream's, on the Pixel) or the one METAL_ICD=PATH names
#                                   (lib/HakiosMetal.icd). Plus bin/m2v_test and share/m2v_test.metallib: the same checks as on a
#                                   Mac, on the guest's driver (RC_JOBS=rc.jobs.metalvk). Still behind boot-arg metal=N like the
#                                   other plug-in; with no driver at that path there is simply no device. docs/metal-plan.md.
#                      ADSP=1       with the audio DSP's firmware from ../fw-audio in fw/ (20 MB; for the
#                                   kernel's adsp_step=N, drivers/audio/hakios.adsp.c, which runs before the root is
#                                   mounted). Not beside GL=1: the two do not fit the 64 MB the loader takes with
#                                   the kernel. adsp=1 needs none of it: the system image has the firmware in /fw.
#                      MODEM=1      stage checked local MPSS firmware from ../fw-modem in system.apfs /fw.
#                      DEVICE=NAME  with the firmware a device's own repository carries, devices/NAME/firmware/
#                                   (every file but its READMEs), in fw/: DEVICE=starlte for the Galaxy S9's audio
#                                   (ABOX's calliope images, DSM; devices/starlte/firmware/audio/README.md).
#                                   This does not start the modem; the XNU MPSS driver is still required.
#   system.apfs      the system image, the root filesystem (the resident RAM disk, /dev/md0), an
#                    APFS container: the part of Darwin userland 16.5 chosen by ../sysimage/select-system.py at
#                    its real paths, the dyld shared cache, and src/stage0.c as /sbin/launchd (the
#                    real one is /sbin/launchd.darwin). 2.5 GB; upload once per loader session with
#                    `xl ramdisk userland/system.apfs`. Rebuilt when missing, when its inputs
#                    (stage0, the manifest, the cryptex files) are newer, or with SYSTEM=1 (after
#                    changing the recipe below).
#   flashboot.apfs   FLASHBOOT=1: the RAM part of a boot whose root is the Darwin volume on the phone's
#                    flash (see below); with CRYPTEXES=flash the boot volume alone, the cryptexes being
#                    on the flash. FLASHBOOT=1 also decides what goes into root.apfs: the job list is
#                    then made for the whole Darwin volume (mkjobs.py --root-volume), so it holds daemons
#                    -- fairplayd, identityservicesd, passd ... -- that the system image has not got.
#                    That root.apfs is for a flash boot; a RAM boot wants one built without FLASHBOOT=1,
#                    or launchd will fail to start those few when something asks for them.
#
#   PREBUILT=1       do not compile anything: take our programs from ../prebuilt/userland (this is also
#                    what happens when the cross toolchain is not set up, see ../xnu-deps/setup.sh);
#                    the images are then made of them and of the Darwin files alone (python3)
#   CHECK=1          check every image (every container) as it is made: apfsck has to find it clean and
#                    the kernel's apfs_lib has to read every volume back as the directory it was made of
#                    (../tools/apfs/check-image.sh; the system image takes a few minutes more)
set -e
# a copy of a tree that shares its files' data: hard links (Linux), clones of the files (macOS's APFS, whose cp has no -l)
linkcopy() { if [ "$(uname)" = Darwin ]; then cp -cRp "$1" "$2"; else cp -al "$1" "$2"; fi; }
cd "$(dirname "$(readlink -f "$0")")"
source ../xnu-env.sh

CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
DYN="xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1"
DARWIN=../sysimage
CACHE=$DARWIN/20F66__iPhone10,3_6
# sysimage/rootfs and sysimage/cryptex must be this copy's own directories of hard links, the way
# tools/new-workspace.sh makes them. A symlink to another copy's tree makes sysroot a symlink too, because
# cp -a does not follow one, and then every rm -rf and mv below edits the shared extraction in place. That
# happened on 19 October 2026: 491 LaunchDaemons, NanoLaunchDaemons, LaunchAngels, xpc/launchd.plist and
# private/var went out of the main copy's tree, and only the hard links in the other copies still held them.
for d in rootfs cryptex cryptex-app; do
	[ -L $DARWIN/$d ] || continue
	echo "build.sh: sysimage/$d is a symlink to $(readlink $DARWIN/$d)" >&2
	echo "build.sh: it has to be a directory of this copy's own: rm it and cp -al the original over," >&2
	echo "build.sh: as tools/new-workspace.sh does. Building against a link damages the shared tree." >&2
	exit 1
done
# The stock Darwin jobs given to launchd, roughly in the order they were brought up.
DARWIN_JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard com.apple.chronod
"}
# com.apple.chronod is WidgetKit's host: it asks every widget extension for its timeline and hands
# SpringBoard the rendered views. Without the job SpringBoard asked launchd for it every second
# ("Could not find job with label com.apple.chronod", failed lookups of com.apple.chronoservices) and
# the home screen's Weather and Calendar widgets stayed blank white boxes. It used to abort in
# RenderBox for want of that framework's default.metallib; the system image carries the small
# shader libraries now (sysimage/select-system.py), and on QEMU's AVF layout (23 October 2026) it ran
# without a crash, started the extensions on demand and both widgets showed their content
# (Calendar the day, Weather "No weather data", there being no network and no location).
# Darwin's own Wi-Fi daemon, for Settings to have a Wi-Fi to show: the kernel answers it in
# drivers/wifi/hakios.apple80211.c. With the radio (wifi=1) Settings then shows Wi-Fi on, the network joined and the
# networks the radio hears. NOT FINISHED: a network cannot be joined from Settings yet.
# It is in by default now, because it costs nothing where there is no radio at all: in QEMU it comes up, talks
# to itself about country codes and sits there -- 114 processes started against 119 without it, no crash, never
# restarted -- and it answers what the rest of the system keeps asking for anyway. com.apple.wifi.manager, which
# wifid registers itself rather than declaring in its plist, fell from 30 failed lookups a session to 3,
# com.apple.private.corewifi-xpc from 1 to none, and the session's whole failed-lookup count from 331 to 305.
# (Count a QEMU run log with its duplicate lines removed: run-darwin.sh prints launchd's log twice.)
# WIFID=0 leaves it out.
[ "${WIFID-1}" != 0 ] && DARWIN_JOBS="$DARWIN_JOBS com.apple.wifid"
# Bluetooth (BT=1): bluetoothd at load, BlueTool when bluetoothd asks it (mkjobs.py gives bluetoothd BT_HCI_TRANSPORT)
if [ -n "$BT" ]; then
	DARWIN_JOBS="$DARWIN_JOBS com.apple.bluetoothd com.apple.BlueTool"
	export BT_HCI_TRANSPORT=${BT_HCI_TRANSPORT-H4:POSIX:com.apple.uart.bluetooth,4000000}
fi
# The programs (file names) started with a library of ours inside them: "LIBRARY NAME..." per line
# (src/inject.c is libhakios.dylib, src/inject_sb.c is libhakios.sb.dylib, src/inject_media.c is
# libhakios.media.dylib).
# What Sileo runs from /bin (see the package manager's prefix below): the system image's /bin, or the
# boot volume's bin, bound over the Darwin volume's /bin when that is the root (src/init.c, flash_root).
SILEO_BIN="chmod cp mkdir mv rm"
# (libhakios.tweaks is the tweak loader of the package manager's prefix, src/inject_tweaks.c: in
# SpringBoard the library above opens it, here it is named for the programs that have no library of
# ours -- Settings, where a tweak's own pane belongs. Add a program's name to have tweaks in it.)
INJECT=${INJECT-"/redwood/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system
/redwood/lib/libhakios.sb.dylib SpringBoard
/redwood/lib/libhakios.media.dylib mediaserverd
/redwood/lib/libhakios.app.dylib Sileo MobileNotes
/redwood/lib/libhakios.tweaks.dylib Preferences"}
[ -n "$BT" ] && INJECT="$INJECT
/redwood/lib/libhakios.bluetool.dylib BlueTool"
mkdir -p obj

if [ -z "$PREBUILT" ] && ! command -v ld64 >/dev/null; then PREBUILT=1; fi
if [ -n "$PREBUILT" ]; then
	echo "build.sh: not compiling, using the programs in prebuilt/userland"
	for f in ../prebuilt/userland/*; do
		# keep a file that did not change (and its time stamp): system.apfs depends on stage0
		cmp -s "$f" "obj/$(basename "$f")" || cp "$f" obj/
	done
	cp obj/hello hello
else
	$CC $LD -o hello src/hello.c
	$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
	$CC -c libmini/start.S -o obj/start.o
	for prog in init child stage0 var_store; do
		$CC -Ilibmini $LD -o obj/$prog.new src/$prog.c obj/mini.o obj/start.o
		# keep the old file (and its time stamp) when nothing changed: system.apfs depends on stage0
		cmp -s obj/$prog.new obj/$prog && rm obj/$prog.new || mv obj/$prog.new obj/$prog
	done
fi

# --- the small image ------------------------------------------------------------------------------
rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
cp obj/child root/bin/child
if [ -n "$VK_DIR" ]; then
	[ -f "$VK_DIR/libhakios.vk.dylib" ] && [ -f "$VK_DIR/vk_test" ] ||
		{ echo "build.sh: VK_DIR=$VK_DIR has no libhakios.vk.dylib and vk_test (tools/gfxstream/mk-vk.sh ios)" >&2; exit 1; }
	mkdir -p root/lib
	install -m 0755 "$VK_DIR/libhakios.vk.dylib" root/lib/libhakios.vk.dylib
	install -m 0755 "$VK_DIR/vk_test" root/bin/vk_test
	echo "build.sh: the guest's Vulkan (libhakios.vk.dylib, vk_test) from $VK_DIR"
fi
# ALONE=1: the kernel starts this image's /sbin/launchd as process 1 (rd=md0), with no stage0 to mount it on /redwood:
# so init is that file too, and finds its image at / (src/init.c, base); /redwood is the image again for the paths
# that name it (the jobs' plists, etc/rc lines)
if [ -n "$ALONE" ]; then
	cp obj/init root/sbin/launchd
	ln -s / root/redwood
	ln -s private/var/tmp root/tmp          # as iOS's own root has it (the system image's, otherwise)
	mkdir -p root/flash                     # where init mounts the system image off the flash (system_apfs=)
fi
# The data volume into the flash and back (src/var_store.c), and the job that saves it when a run
# ends: the program and its job together, away from the Darwin job list below, which several working
# copies are editing at once.
# Only with the program: a PREBUILT tree has no var_store, and its job then made xpcproxy exit(78)
# (EX_CONFIG, nothing to run) at every boot.
if [ -f obj/var_store ]; then
	cp obj/var_store root/bin/
	cp com.hakios.varstore.plist root/LaunchDaemons/
fi
echo "Darwin on hakios: image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
# 300000 bytes, byte i = (i*31 + (i>>8)) & 0xff; init verifies it through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > root/share/pattern.bin
# EXTRA_SHARE: whole directories of data for a one-off diagnostic, each under its own name. Here and
# not in root/bin, because what goes in is not a program: a generated test suite is modules and the
# bytes they read, and the job list runs it by its own run.sh. -L so that a directory assembled out
# of symbolic links arrives as files, since the image is made from this tree and not from the links'
# targets.
for d in $EXTRA_SHARE; do
	[ -d "$d" ] || { echo "build.sh: EXTRA_SHARE: $d is not a directory"; exit 1; }
	cp -rL "$d" root/share/
done
if [ -n "$SEED" ]; then
	[ -f "$SEED" ] || { echo "build.sh: SEED: $SEED is not a file"; exit 1; }
	cp "$SEED" root/share/var.seed
	echo "build.sh: share/var.seed: a seed of the data volume, $(( $(wc -c < "$SEED") / 1048576 )) MB (boot-arg var_seed=1)"
fi
if [ -d $DARWIN/rootfs/usr/lib ]; then
	if [ -z "$PREBUILT" ]; then
		# ordinary dynamically linked programs of our own, built against the Darwin SDK
		$DYN -o obj/hello_dyn src/hello_dyn.c
		$DYN -framework CoreFoundation -o obj/svc_test src/svc_test.c
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface -o obj/surf_test src/surf_test.c
		# an IOSurface of ours imported as a texture of the GPU, written through one view
		# and read through the other (src/drawable_test.c): the check that tells a texture
		# over the surface's own pages from a texture over a copy of them. It goes through
		# src/hakiosdrawable.m, which is the bridge under test, and dlopens the compiler
		# library the way the driver does.
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface \
			-framework IOKit -o obj/drawable_test src/drawable_test.c src/hakiosdrawable.m
		$DYN -o obj/rwtouch src/rwtouch.c
		# the kernel's thermal pressure passed on to iOS (src/thermald.c)
		$DYN -o obj/thermald src/thermald.c
		$DYN -framework CoreFoundation -framework AudioToolbox -framework AVFoundation -lobjc -o obj/sound_test src/sound_test.c
		$DYN -framework CoreFoundation -lMobileGestalt -o obj/mg_test src/mg_test.c
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/torch_test src/torch_test.c
		$DYN -O2 -o obj/camera_test src/camera_test.c src/camera_isp.c
		$DYN -fobjc-arc -framework Foundation -framework AVFoundation -o obj/camera_avtest src/camera_avtest.m
		# (the microphone as an application records: allowed to, by an entitlement, if ldid is there)
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/record_test src/record_test.c &&
			{ ${LDID:-ldid} -Ssrc/record_test.entitlements obj/record_test 2>/dev/null ||
			  echo "build.sh: no ldid: record_test is unsigned, and the system may not let it record"; }
		$DYN -o obj/cpu_test src/cpu_test.c
		# the Galaxy S9's GPU through HakiosMali, without Mesa (src/mali_test.c)
		$DYN -framework IOKit -o obj/mali_test src/mali_test.c
		# which processes take the processors, every few seconds (src/cputop.c; rc.jobs.cputop)
		$DYN -o obj/cputop src/cputop.c
		# the wallpaper's second chance on a phone too slow for launchd's 5 s check-in (src/poster_retry.c; rc.jobs)
		$DYN -o obj/poster_retry src/poster_retry.c
		$DYN -framework CoreFoundation -framework IOSurface -o obj/cow_test src/cow_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework IOSurface -o obj/cg_test src/cg_test.c
		# what Darwin here believes about Metal, and what CoreImage does without it (src/metal_test.m);
		# OpenGLES for the CIContext made over an EAGLContext, which is deprecated and the point
		$DYN -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics \
			-framework CoreImage -framework IOKit -framework Metal -framework OpenGLES \
			-lMobileGestalt -o obj/metal_test src/metal_test.m
		# the first question of subsystem (2): can a subclass of the private _MTLDevice be
		# made here, and what does its initWithAcceleratorPort: do (src/metal_dev.m). No ARC:
		# it calls objc_msgSend by hand and allocates a class at runtime.
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_dev src/metal_dev.m
		# a device object that implements nothing and logs what Apple's own init asks it
		# for (src/metal_stubdev.m); same reasons for no ARC and for -lobjc
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_stubdev src/metal_stubdev.m
		# the device object of the driver itself, as the library a bundle would hold
		# (src/hakiosmetal_air.m). It links against Metal's private _OBJC_CLASS_$__MTLDevice,
		# which is bindable because IOGPU.framework imports the same symbol; -lobjc for
		# class_addProtocol and no ARC because it calls objc_msgSend by hand.
		# (with src/hakiosdrawable.m beside it: the IOSurface a CAMetalLayer hands a
		# device, as a texture. A file of its own because it answers CoreAnimation and
		# not Metal, and the two meet in one method -- docs/metal.md 10.46.)
		$DYN -dynamiclib -install_name /redwood/lib/libhakiosmetal.dylib \
			-framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/libhakiosmetal.dylib src/hakiosmetal_air.m src/hakiosdrawable.m
		# and the program that walks Metal's own steps 4 and 5 against it
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_drv_test src/metal_drv_test.m
		# An application of this port's own: a window, a CAMetalLayer, a drawable and a
		# triangle in it (src/triangle_app.m). UIKit because that is the point -- every
		# Metal result so far is from inside one of Apple's daemons, and this is an
		# ordinary bundle in /Applications that SpringBoard launches; IOSurface because
		# it reads the drawable's own surface back to say whether the texture WAS those
		# pages; no ARC and -lobjc because it sends -initWithAcceleratorPort: by hand,
		# the way src/metal_drv_test.m does. The bundle is assembled further down, where
		# a PREBUILT tree can assemble it too.
		# (signed, if ldid is there, for the entitlements an application has and a
		# /redwood/bin program does not: see src/triangle_app.entitlements, which says
		# why none of them is needed to RUN. Signed here rather than in the bundle, so
		# that the copy in /redwood/bin carries the same signature.)
		$DYN -framework Foundation -framework UIKit -framework QuartzCore \
			-framework Metal -framework IOSurface -framework IOKit -lobjc \
			-o obj/HakiosTriangle src/triangle_app.m &&
			{ ${LDID:-ldid} -Ssrc/triangle_app.entitlements obj/HakiosTriangle 2>/dev/null ||
			  echo "build.sh: no ldid: HakiosTriangle is unsigned, which still runs (AMFI here accepts everything) but holds no entitlements"; }
		# what the device ANSWERS when an application asks what it supports
		# (src/metal_caps.m). It takes the device from MTLCreateSystemDefaultDevice() so
		# that the ivars behind Apple's inherited answers are filled by the registration
		# Metal itself ran; no ARC and -lobjc because most of what it asks is MTLDeviceSPI,
		# whose header this port has not got, so every send is an objc_msgSend by hand. MetalFX
		# because the acceptance test's own capability gate is +supportsDevice: on its two
		# scaler descriptors, and that gate can be measured without the game.
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-framework MetalFX -o obj/metal_caps src/metal_caps.m
		# ... and the switch that steps 1 and 2 need: it puts MetalPluginName and
		# MetalPluginClassName on the IOAcceleratorES service (src/metal_props.c). IOKit and
		# CoreFoundation only -- a program that flips this switch must not itself ask Metal,
		# whose conditions are asserts. It needs the kernel booted with metal=1.
		$DYN -framework CoreFoundation -framework IOKit -o obj/metal_props src/metal_props.c
		# what CoreAnimation and UIKit believe the screen's frame rate is (src/fps_info.m)
		$DYN -fobjc-arc -framework Foundation -framework QuartzCore -o obj/fps_info src/fps_info.m
		$DYN -o obj/shm_test src/shm_test.c
		$DYN -o obj/apfs_mmap_test src/apfs_mmap_test.c
		$DYN -o obj/vol_ls src/vol_ls.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o obj/snap_test src/snap_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework CoreText -o obj/font_test src/font_test.c
		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m
		# (with an entitlement for "battery_test open URL", if ldid is there: src/battery_test.c)
		$DYN -framework CoreFoundation -framework IOKit -lsqlite3 -o obj/battery_test src/battery_test.c &&
			{ ${LDID:-ldid} -Ssrc/battery_test.entitlements obj/battery_test 2>/dev/null ||
			  echo "build.sh: no ldid: battery_test is unsigned, and SpringBoard will not open URLs for it"; }
		# (the keychain answers only a program that has a keychain access group: signed with one, if ldid is there)
		$DYN -framework CoreFoundation -framework Security -o obj/keychain_test src/keychain_test.c &&
			{ ${LDID:-ldid} -Ssrc/keychain_test.entitlements obj/keychain_test 2>/dev/null ||
			  echo "build.sh: no ldid: keychain_test is unsigned, and the keychain will refuse it (-34018)"; }
		# IOAESAccelerator round trips, the way iOS's own clients call it (src/aes_test.c; the kernel's
		# MiniIOAESAccelerator.cpp, UniXNU's SoftwareAES)
		$DYN -framework CoreFoundation -framework IOKit -o obj/aes_test src/aes_test.c
		# (and wifid's way of keeping a network's password, by wifid's name: src/wifipw_test.c)
		$DYN -framework CoreFoundation -framework Security -o obj/wifipw_test src/wifipw_test.c &&
			{ ${LDID:-ldid} -Ssrc/wifipw_test.entitlements obj/wifipw_test 2>/dev/null ||
			  echo "build.sh: no ldid: wifipw_test is unsigned, and the keychain will refuse it (-34018)"; }
		# our library for inside stock Darwin programs (the jobs in INJECT are started with it)
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libhakios.dylib src/inject.c
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libhakios.sb.dylib src/inject_sb.c
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.display.dylib -framework CoreFoundation -lobjc \
			-o obj/libhakios.display.dylib src/display_native.c
		# ... the tweak loader of the package manager's prefix (src/inject_tweaks.c): it goes into a
		# program either as the one library the kernel inserts or opened by a library of ours
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.tweaks.dylib -framework CoreFoundation -lobjc \
			-o obj/libhakios.tweaks.dylib src/inject_tweaks.c
		# ... and inside an application, to say what killed it (src/inject_app.c)
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.app.dylib -lobjc \
			-o obj/libhakios.app.dylib src/inject_app.c
		# ... and inside BlueTool, so that it runs the Galaxy S9's Bluetooth scripts (BT=1, src/inject_bluetool.c)
		$DYN -dynamiclib -install_name /redwood/lib/libhakios.bluetool.dylib -framework CoreFoundation -lMobileGestalt \
			-o obj/libhakios.bluetool.dylib src/inject_bluetool.c
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libhakios.media.dylib -framework CoreFoundation -framework CoreMedia \
			-framework CoreVideo -framework IOSurface -lobjc \
			-o obj/libhakios.media.dylib src/inject_media.c src/camera_device.c src/camera_isp.c
		# the output device of the audio server: a plug-in for CoreAudio's HAL
		$DYN -bundle -framework CoreFoundation -o obj/HakiosAudio src/audio_plugin.c
		# the plug-in Metal loads when the kernel names it (boot-arg metal=1): a device, its buffers and the libraries it reads
		# (src/HakiosMetal.m, src/hakios_metallib.c; tools/metal-plugin-test.sh tries the same objects on a Mac)
		# With hakios-metal (github.com/oosexgang/hakios-metal, `make darwin` in it, beside this tree or at HAKIOS_METAL=): buffers in
		# the GPU's memory, command buffers, and blits done by the GPU when the kernel has one (gpu=1). Without it, the plug-in as before.
		# (The earlier driver over Mesa's AIR compiler, src/hakiosmetal_air.m, is still built above as libhakiosmetal.dylib.)
		HM=${HAKIOS_METAL:-../../hakios-metal}
		METAL_GPU=
		if [ -f $HM/out/darwin/libhakios-metal.a ]; then
			METAL_GPU="-DHAKIOS_METAL_GPU -I$HM/include $HM/out/darwin/libhakios-metal.a -lc++ -framework IOKit"
			echo "build.sh: HakiosMetal with hakios-metal ($HM)"
		fi
		# the plug-in itself is hakios-metal's now (plugin/, where it is tested); the copy in src/ is for a tree without it.
		# hakios-metal's plug-in decides by itself which processes are shown the device; the copy in src/ does it with
		# src/hakios_metal_shown.c (and HakiosMetal.clients beside the bundle)
		METAL_SRC=src
		METAL_SHOWN=src/hakios_metal_shown.c
		if [ -f $HM/plugin/HakiosMetal.m ]; then
			METAL_SRC=$HM/plugin
			METAL_SHOWN=
			echo "build.sh: HakiosMetal from $HM/plugin"
		fi
		if [ "$METAL" = vk ]; then
			# METAL=vk: Metal on Vulkan instead (src/metalvk/HakiosMetalVK.mm says how its device is made). The library is built
			# again when it is missing or tools/metal/ has changed since (its pins, the bridge, the shaders: the same hash
			# tools/metal/mk-metal2vulkan.sh writes beside it). ARC and C++17 for AdrenoMetal's frontend; -fuse-cxa-atexit
			# undoes the kernel's -fno-use-cxa-atexit that xnu-clang++ adds, as tools/mk-mesa.sh does.
			V=../xnu-deps/build/metal2vulkan/out/ios
			[ -f $V/libmetal2vulkan.dylib ] && [ "$(cat $V/built-from-tree 2>/dev/null)" = "$(cat ../tools/metal/mk-metal2vulkan.sh \
				../tools/metal/bridge/* ../tools/metal/*.metal ../tools/metal/m2v_test.c ../tools/metal/m2v/*.patch 2>/dev/null |
				sha256sum)" ] || ../tools/metal/mk-metal2vulkan.sh ios
			$DYN -c -o obj/hakios_metal_shown.o src/hakios_metal_shown.c
			xnu-clang++ -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1 -fuse-cxa-atexit -std=c++17 -fobjc-arc \
				-I../tools/metal -Isrc/metalvk -bundle -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
				-framework CoreFoundation -lobjc -o obj/HakiosMetal src/metalvk/HakiosMetalVK.mm \
				src/metalvk/NativeBridgeMetalFrontend.mm obj/hakios_metal_shown.o $V/libmetal2vulkan.dylib
			cp $V/libmetal2vulkan.dylib $V/m2v_test $V/m2v_test.metallib obj/
			echo "build.sh: HakiosMetal is Metal on Vulkan (libmetal2vulkan.dylib, $(cat $V/built-from))"
		else
			$DYN -bundle -framework Foundation -framework Metal -lobjc -I$METAL_SRC -o obj/HakiosMetal $METAL_SRC/HakiosMetal.m \
				$METAL_SRC/hakios_metallib.c $METAL_SHOWN $METAL_GPU
			rm -f obj/libmetal2vulkan.dylib obj/m2v_test obj/m2v_test.metallib
		fi
		if [ "$GL" = gfx ]; then
			# the OpenGL ES library that draws with the host's GPU: gfxstream over virtio-gpu
			# (/dev/vgpu3d), behind the same eagl.c (tools/gfxstream/mk-gfx.sh)
			OUT=$PWD/obj ../tools/gfxstream/mk-gfx.sh
		elif [ -n "$GL" ]; then
			# the OpenGL ES library, and the table of functions that hands CoreAnimation's calls to it
			# (Mesa says beside the library what it was built from: one from other patches is built again)
			M=../xnu-deps/build/mesa/out
			[ "$(cat $M/built-from 2>/dev/null)" = "$(cat ../tools/mesa/*.patch ../tools/mesa/hakios-drm/* | sha256sum)" ] ||
				../tools/mk-mesa.sh
			# ... and the compiler library separately, because the hash above covers the PATCHES
			# and the library is built from tools/mesa/air/, which they do not mention. Two boots
			# of 22 October carried a libhakiosair.dylib three hours older than the driver beside
			# it while rwa_render_shape had grown IN THE MIDDLE -- the image was the mismatch the
			# contract warns about, and nothing in it said so. Silence is what made it expensive:
			# the library is stripped, so the driver found the symbols it wanted and read a stride
			# where a pixel format was.
			# NO_AIR=1 omits this library for a GLES-only image. The Mali Metal path needs it,
			# just as Freedreno does (devices/starlte/GPU.md).
			[ -n "$NO_AIR" ] || [ "$(cat $M/built-from-air 2>/dev/null)" = "$(cat ../tools/mesa/air/* | sha256sum)" ] || {
				echo "build.sh: tools/mesa/air/ has changed since libhakiosair.dylib was built"
				AIR=1 ../tools/mk-mesa.sh
			}
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
			$DYN -O2 -c -o obj/surface_checksum.o src/surface_checksum.c
			$DYN -dynamiclib -install_name /redwood/lib/libhakios.gl.dylib -Iobj -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libhakios.gl.dylib \
				src/eagl.c src/gl_osmesa.c obj/surface_checksum.o
			$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/gl_test.c
			# ... and what this GPU makes of an unsigned integer turned into a float (src/u2f_test.c)
			$DYN $M/libOSMesa.8.dylib -o obj/u2f_test src/u2f_test.c
			cp $M/libOSMesa.8.dylib obj/
			# air_run draws with a Metal shader translated by the AIR frontend
			# and reads the pixels back (docs/air.md). It is built inside the
			# Mesa build, because it needs Gallium itself and not the gl*
			# entry points this library exports; AIR=1 tools/mk-mesa.sh makes
			# it, and it is copied in when it is there.
			[ -f $M/air_run ] && cp $M/air_run obj/air_run
			[ -f $M/air_metal_test ] && cp $M/air_metal_test obj/air_metal_test
			# The compiler the Metal driver asks for: AIR in, a dispatch on the
			# Adreno out (tools/mesa/air/air_metal.h). It is copied whether or not
			# GL is wanted, because src/hakiosmetal_air.m dlopens it by that path --
			# and its absence is a sentence the driver prints, not a crash.
			[ -z "$NO_AIR" ] && [ -f $M/libhakiosair.dylib ] && cp $M/libhakiosair.dylib obj/
		fi
	fi
	# (drawable_test is built in the same block as surf_test, so it is copied with it)
	for f in hello_dyn svc_test surf_test drawable_test rwtouch mg_test; do
		[ -f obj/$f ] && cp obj/$f root/bin/
	done
	[ -f obj/cpu_test ] && cp obj/cpu_test root/bin/        # (a prebuilt/ from before this program has none)
	[ -f obj/mali_test ] && cp obj/mali_test root/bin/      # (likewise)
	[ -f obj/thermald ] && cp obj/thermald root/bin/
	[ -f obj/cputop ] && cp obj/cputop root/bin/
	[ -f obj/poster_retry ] && cp obj/poster_retry root/bin/
	[ -f obj/cow_test ] && cp obj/cow_test root/bin/
	[ -f obj/cg_test ] && cp obj/cg_test root/bin/
	[ -f obj/metal_dev ] && cp obj/metal_dev root/bin/
	[ -f obj/metal_stubdev ] && cp obj/metal_stubdev root/bin/
	[ -f obj/metal_drv_test ] && cp obj/metal_drv_test root/bin/
	[ -f obj/metal_caps ] && cp obj/metal_caps root/bin/
	[ -f obj/metal_props ] && cp obj/metal_props root/bin/
	[ -f obj/shm_test ] && cp obj/shm_test root/bin/
	[ -f obj/apfs_mmap_test ] && cp obj/apfs_mmap_test root/bin/
	[ -f obj/vol_ls ] && cp obj/vol_ls root/bin/
	[ -f obj/snap_test ] && cp obj/snap_test root/bin/
	[ -f obj/font_test ] && cp obj/font_test root/bin/
	[ -f obj/metal_test ] && cp obj/metal_test root/bin/
	[ -f obj/fps_info ] && cp obj/fps_info root/bin/
	[ -f obj/net_test ] && cp obj/net_test root/bin/
	[ -f obj/battery_test ] && cp obj/battery_test root/bin/
	[ -f obj/keychain_test ] && cp obj/keychain_test root/bin/
	[ -f obj/wifipw_test ] && cp obj/wifipw_test root/bin/
	[ -f obj/aes_test ] && cp obj/aes_test root/bin/
	[ -f obj/sound_test ] && cp obj/sound_test root/bin/
	[ -f obj/torch_test ] && cp obj/torch_test root/bin/
	[ -f obj/camera_test ] && cp obj/camera_test root/bin/
	[ -f obj/camera_avtest ] && cp obj/camera_avtest root/bin/
	[ -f obj/record_test ] && cp obj/record_test root/bin/
	# The same binary as the application below, under /redwood/bin, for its --layer mode:
	# a CAMetalLayer with no window and no UIKit, driven once. That mode cannot say whether
	# a frame reaches the panel, and it answers the two questions that come before it -- does
	# the driver work outside Apple's daemons, and does nextDrawable give a texture -- in a
	# process whose output is certainly in the log, because the job list runs it. An
	# application launched by SpringBoard is a process this file cannot promise that about.
	[ -f obj/HakiosTriangle ] && cp obj/HakiosTriangle root/bin/triangle_app
	for f in $EXTRA; do cp "$f" root/bin/; done
	#
	# ... and the application bundle itself, staged in obj/ and copied into
	# sysroot/Applications by the system image below.
	#
	# A PLAIN DIRECTORY OF REAL FILES, and that is not a style: a bundle made of symbolic
	# links is refused by LaunchServices ("Unable to register"), and the registrar that
	# walks /Applications at boot passes over mount points. So the icons are generated here
	# rather than linked from anywhere, and the plist and the executable are copied.
	if [ -f obj/HakiosTriangle ]; then
		rm -rf obj/HakiosTriangle.app
		mkdir -p obj/HakiosTriangle.app
		cp obj/HakiosTriangle obj/HakiosTriangle.app/
		cp src/triangle_app.plist obj/HakiosTriangle.app/Info.plist
		printf 'APPL????' > obj/HakiosTriangle.app/PkgInfo
		# The icon is the picture the application draws: an orange triangle over the
		# lower left half of a blue square, by the same rule the shader covers a pixel
		# by ((x+0.5)/w < (y+0.5)/h). So the home screen states the prediction, and an
		# icon that comes out with its halves the other way round says the arithmetic in
		# this one line disagrees with the arithmetic in src/triangle_app.m. No alpha
		# channel: iOS masks an icon itself and an icon with alpha comes out wrong.
		python3 - obj/HakiosTriangle.app <<-'PY'
		import struct, sys, zlib
		out = sys.argv[1]
		for size, scale in ((120, 2), (180, 3)):
		    rows = bytearray()
		    for y in range(size):
		        rows.append(0)                        # PNG filter type: none
		        for x in range(size):
		            inside = (2 * x + 1) * size < (2 * y + 1) * size
		            rows += bytes((255, 128, 0) if inside else (0, 0, 255))
		    def chunk(kind, body):
		        return (struct.pack('>I', len(body)) + kind + body +
		                struct.pack('>I', zlib.crc32(kind + body)))
		    png = (b'\x89PNG\r\n\x1a\n' +
		           chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
		           chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))
		    open('%s/AppIcon60x60@%dx.png' % (out, scale), 'wb').write(png)
		PY
		# Unsigned, which is enough: AMFI in this kernel accepts every signature and
		# holds no trust cache (libkern/amfi/miniamfi.c), and the kernel is built without
		# config_enforce_signed_code. What a signature would still buy is entitlements,
		# and this application asks the system for nothing that needs one.
		echo "  HakiosTriangle.app: $(du -sk obj/HakiosTriangle.app | cut -f1) KB for /Applications"
	fi
	# /dev and /private/var exist so that this image can also be the root on its own:
	# devfs is mounted on /dev, and init puts a ramfs on /private/var
	mkdir -p root/dev root/private/var
	mkdir -p root/lib
	cp obj/libhakios.dylib obj/libhakios.sb.dylib root/lib/
	[ -f obj/libhakios.display.dylib ] && cp obj/libhakios.display.dylib root/lib/
	[ -f obj/libhakios.media.dylib ] && cp obj/libhakios.media.dylib root/lib/     # (as with cpu_test)
	[ -f obj/libhakios.app.dylib ] && cp obj/libhakios.app.dylib root/lib/
	[ -f obj/libhakios.tweaks.dylib ] && cp obj/libhakios.tweaks.dylib root/lib/
	if [ -n "$BT" ]; then
		[ -f obj/libhakios.bluetool.dylib ] && cp obj/libhakios.bluetool.dylib root/lib/
		mkdir -p root/etc/bluetool
		cp bluetool/*.script root/etc/bluetool/
	fi
	[ -f obj/libhakiosmetal.dylib ] && cp obj/libhakiosmetal.dylib root/lib/
	[ -f obj/libhakiosair.dylib ] && cp obj/libhakiosair.dylib root/lib/
	if [ -f obj/HakiosAudio ]; then
		mkdir -p root/lib/HakiosAudio.driver
		cp obj/HakiosAudio root/lib/HakiosAudio.driver/
		cp src/audio_plugin.plist root/lib/HakiosAudio.driver/Info.plist
	fi
	if [ -f obj/HakiosMetal ]; then
		mkdir -p root/lib/HakiosMetal.bundle
		cp obj/HakiosMetal root/lib/HakiosMetal.bundle/
		cp src/HakiosMetal.plist root/lib/HakiosMetal.bundle/Info.plist
		# the processes shown the device without a boot-arg (src/hakios_metal_shown.c): Notes, whose PencilKit takes
		# Metal for granted -- with no device it builds a compute pipeline of nil and aborts at the first touch
		printf 'MobileNotes\n' > root/lib/HakiosMetal.bundle/HakiosMetal.clients
	fi
	# METAL=vk: the runtime the plug-in links, the guest's test of it, and the Vulkan driver it opens if not the default
	if [ -f obj/libmetal2vulkan.dylib ]; then
		cp obj/libmetal2vulkan.dylib root/lib/
		cp obj/m2v_test root/bin/
		cp obj/m2v_test.metallib root/share/
		[ -z "$METAL_ICD" ] || echo "$METAL_ICD" > root/lib/HakiosMetal.icd
	fi
	if [ "$GL" = gfx ]; then
		cp obj/libhakios.gl.dylib root/lib/
		cp obj/gfx_test root/bin/
	elif [ -n "$GL" ]; then
		cp obj/libOSMesa.8.dylib root/lib/
		cp obj/gl_test root/bin/
		if [ -f obj/u2f_test ]; then cp obj/u2f_test root/bin/; fi   # an "if": see the inject.conf loop below
		[ -f obj/air_run ] && cp obj/air_run root/bin/
		[ -f obj/air_metal_test ] && cp obj/air_metal_test root/bin/
		[ "$GL" = test ] || cp obj/libhakios.gl.dylib root/lib/
	fi
	# a program is not started with a library that is not there: it would not start at all
	echo "$INJECT" | while read -r library names; do
		# a /redwood/lib library only if it was actually built; any other path (e.g. a tweak loader
		# in /var/jb) as it is, since it lives on the prefix and is there at run time
		case "$library" in
		"") ;;
		# an "if" and not "&& echo": a last line whose library is absent would make the loop,
		# and with it the pipeline and the whole build under set -e, fail -- which is what a tree
		# with only prebuilt/userland in it does, since not every injection library is shipped there
		/redwood/lib/*) if [ -f "root/lib/${library#/redwood/lib/}" ]; then echo "$library $names"; fi ;;
		*) echo "$library $names" ;;
		esac
	done > root/etc/inject.conf
	cp $DARWIN/var.skel root/etc/var.skel              # the directories init creates in /private/var
	# time zones: the template's links go in a circle until the time zone daemon has run (/usr/share/zoneinfo
	# points back into /var); point them at the data that ships with the system, and use this machine's zone
	zone=$(timedatectl show -p Timezone --value 2>/dev/null || readlink /etc/localtime | sed 's#.*/zoneinfo/##' 2>/dev/null || true); zone=${zone:-UTC}
	sed -i.bak -e 's|^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$|l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo|' \
		-e "s|^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime\$|l\t/var/db/timezone/zoneinfo/$zone\t/private/var/db/timezone/localtime|" root/etc/var.skel
	rm -f root/etc/var.skel.bak
	# ... and the files of ours it puts there (var/): those under mobile/ belong to the user mobile
	cp -r var root/var
	(cd var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
		printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
	done >> root/etc/var.skel
	# mobile's own temporary directory, $HOME/tmp, which Darwin's template does not have: an application of
	# mobile's without a data container of its own keeps its temporary files there -- Sileo, registered from
	# the prefix with the root on the flash, downloads every package and index there, and without it each
	# download ended in "No such file or directory"
	printf 'd\t700\t501\t501\t/private/var/mobile/tmp\n' >> root/etc/var.skel
	# A key of ours for root, when there is one: ssh/authorized_keys, never in the repository (like
	# fw-*). dropbear from the package manager's prefix reads it and lets us in over the phone's
	# Wi-Fi, which saves rebuilding an image for every question one wants to ask a running Darwin. It
	# refuses a key whose directory anyone but its owner may write, hence the 700.
	# SHARE=1: no keys in an image that is going to somebody else. ssh_host_ed25519_key is a
	# private key, and it would be leaving this machine inside a 2.5 GB container
	if [ -z "$SHARE" ] && [ -f ../ssh/authorized_keys ]; then
		mkdir -p root/var/root/.ssh
		printf 'd\t700\t0\t0\t/private/var/root/.ssh\n' >> root/etc/var.skel
		# the key that lets us in, and the host's own key beside it: made here and not on the
		# phone, because ssh-keygen asks who uid 0 is and gets "No user exists for uid 0" --
		# it asks through libiosexec, which reads a database the bootstrap does not ship (see
		# init.c, where it is now built at every boot); the key is made here all the same,
		# since nothing should depend on the order of those two
		for k in authorized_keys ssh_host_ed25519_key ssh_host_ed25519_key.pub; do
			[ -f ../ssh/$k ] || continue
			cp ../ssh/$k root/var/root/.ssh/$k
			printf 'f\t600\t0\t0\t/private/var/root/.ssh/%s\t/var/root/.ssh/%s\n' $k $k >> root/etc/var.skel
		done
	fi
	if [ -n "$ALONE" ]; then cp ${RC:-rc.alone} root/etc/rc; else cp ${RC:-rc.launchd} root/etc/rc; fi
	cp ${RC_JOBS:-rc.jobs} root/etc/rc.jobs
	# QUIET=1: launchd's own standard error goes nowhere (src/init.c). It says a few thousand
	# lines about its own business in the first minutes and pushes everything else out of the
	# kernel's log; the daemons keep their own standard error on the console all the same.
	rm -f root/etc/quiet
	[ -n "$QUIET" ] && : > root/etc/quiet
	# the jobs the Darwin launchd will see (/System/Library/LaunchDaemons is a symlink to this directory).
	# With FLASHBOOT=1 the root will be Apple's whole volume on the flash, not the part of it that fits in
	# the system image, so mkjobs.py judges "is this daemon's program there" by the listing of the volume
	# and not by what select-system.py extracted here (--root-volume; seven jobs turn on the difference,
	# fairplayd among them).
	cp com.hakios.demo.plist com.hakios.wifi.plist com.hakios.thermald.plist root/LaunchDaemons/
	VOLUME=""
	[ -n "$FLASHBOOT" ] && [ -f $DARWIN/rootfs.slt ] && VOLUME="--root-volume $DARWIN/rootfs.slt"
	[ -n "$DARWIN_JOBS" ] && OSLOG=${OSLOG-com.apple.backboardd} ./mkjobs.py ${REST:+--rest} $VOLUME $DARWIN/rootfs root/LaunchDaemons $DARWIN_JOBS
fi
# firmware of the phone's own hardware, where somebody has pulled it from the phone's Android into ../fw-gpu
# (it is the vendor's and never in the repository): the kernel's GPU bring-up, drivers/gpu/hakios.adreno.c, finds it in fw/
if [ -d ../fw-gpu ]; then
	mkdir -p root/fw
	cp ../fw-gpu/* root/fw/
fi
# ... and the Galaxy A50's Wi-Fi/Bluetooth chip: mx140.bin from /vendor/etc/wifi and the keep-alive patch made by
# tools/a50-scsc-ka.py, in ../fw-scsc (drivers/wifi/hakios.scsc.c reads them from fw/ of this image)
if [ -d ../fw-scsc ]; then
	mkdir -p root/fw
	for f in mx140.bin scsc_ka.bin mx140_wlan.hcf mx140_wlan_sw.hcf scsc_country.bin; do
		[ -f ../fw-scsc/$f ] && install -m 0644 ../fw-scsc/$f root/fw/
	done
fi
# The A50's own Wi-Fi address (hakios.scsc.c: ADD_VIF / en0). Same file as brcmfmac: the phone's
# /efs/wifi/.mac.info, copied here as fw-wifi/mac.info. Left out with SHARE=1; the driver also reads EFS at run time.
if [ -z "$SHARE" ] && [ -f ../fw-wifi/mac.info ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-wifi/mac.info root/fw/mac.info
fi
# ... and the image of the Wi-Fi DSP, from /vendor/firmware_mnt/image of the same Android into ../fw-wifi
# (drivers/wifi/hakios.wifi.c; 7.6 MB), with what the radio's firmware is sent once it runs: this phone's
# board data and the regulatory database (image/qca6750 there; 60 and 24 KB)
if [ -f ../fw-wifi/wpss.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-wifi/wpss.mdt ../fw-wifi/wpss.b0[2-9] root/fw/
	for f in bd_m20gl.elf regdb_xiaomi.bin; do
		[ -f ../fw-wifi/qca6750/$f ] && install -m 0644 ../fw-wifi/qca6750/$f root/fw/
	done
	# the network the radio is to join: its name on the first line, its passphrase on the second. Yours, and
	# so not in the repository; it goes where the firmware goes and the kernel reads it there (wifi_join=1)
	rm -f root/fw/network
	# SHARE=1: leave it out, for an image that is going to somebody else -- it is a passphrase
	if [ -z "$SHARE" ] && [ -f ../fw-wifi/network ]; then install -m 0600 ../fw-wifi/network root/fw/network; fi
fi
# ... and the Galaxy S9's (drivers/wifi/hakios.brcmfmac.c, brcmfmac=2 and up): the BCM4361's firmware, NVRAM and CLM,
# pulled from the phone's own Android into ../fw-wifi under the names brcmfmac asks for (brcmfmac4361b2-pcie.bin, .txt,
# .clm_blob; devices/starlte/WIFI.md says which of the phone's files each is). The vendor's: never in the repository
for f in ../fw-wifi/brcmfmac4361*-pcie.*; do
	[ -f "$f" ] || continue
	mkdir -p root/fw
	install -m 0644 "$f" root/fw/
done
# Samsung names let the kernel choose the module's NVRAM from its OTP instead
# of requiring a guessed generic brcmfmac*.txt. Also usable with a RAM root and
# wifi_vendor=0, without mounting or reading VENDOR at run time.
for f in ../fw-wifi/bcmdhd_sta.bin_b[02] ../fw-wifi/bcmdhd_clm.blob ../fw-wifi/nvram.txt_*; do
	[ -f "$f" ] || continue
	mkdir -p root/fw
	install -m 0644 "$f" root/fw/
done
# ... with the phone's own address (brcmfmac=6: its /efs/wifi/.mac.info, as fw-wifi/mac.info) and the network to join
# (brcmfmac=8: fw-wifi/network, as for the POCO above), both left out with SHARE=1
if [ -f ../fw-wifi/bcmdhd_sta.bin_b2 ] || [ -f ../fw-wifi/bcmdhd_sta.bin_b0 ] || \
    ls ../fw-wifi/brcmfmac4361*-pcie.bin >/dev/null 2>&1; then
	rm -f root/fw/mac.info
	if [ -z "$SHARE" ] && [ -f ../fw-wifi/mac.info ]; then install -m 0644 ../fw-wifi/mac.info root/fw/mac.info; fi
	if [ -z "$SHARE" ] && [ -f ../fw-wifi/network ] && [ ! -f root/fw/network ]; then
		install -m 0600 ../fw-wifi/network root/fw/network
	fi
fi
# ... and the Galaxy S9's Bluetooth patch (drivers/bluetooth/hakios.bcm4361_bt.c, bt=6): the phone's own
# /vendor/firmware/bcm4361B2_murata.hcd or _semco.hcd, whichever module the phone has, pulled into ../fw-bt as
# bcm4361B2.hcd (devices/starlte/BLUETOOTH.md; 106 KB). The vendor's: never in the repository
for f in ../fw-bt/*.hcd; do
	[ -f "$f" ] || continue
	mkdir -p root/fw
	install -m 0644 "$f" root/fw/
done
# ... and the vibration motor's waveforms, the phone's /vendor/firmware/aw8697_haptic.bin, from ../fw-haptic
# (drivers/haptic/hakios.haptic.c, haptic=3; 3.6 KB, the vendor's and not in the repository either)
if [ -f ../fw-haptic/aw8697_haptic.bin ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-haptic/aw8697_haptic.bin root/fw/
fi
# ... and the cameras' register tables (drivers/camera/hakios.camera.c, cam=2 and up), made by tools/camsensor.py out of
# the sensors' module files of Android's camera stack, the phone's /vendor/lib64/camera/com.qti.sensormodule.
# hakios_<maker>_<sensor>_<camera>.bin, in ../fw-camera (the vendor's: neither they nor the tables are in the
# repository): camera-<sensor>.tab
for module in ../fw-camera/com.qti.sensormodule.hakios_*.bin; do
	[ -f "$module" ] || continue
	sensor=$(basename "$module" .bin | sed 's/^com\.qti\.sensormodule\.hakios_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camsensor.py "$module" "root/fw/camera-$sensor.tab" >/dev/null
done
# ... and their colour tuning (libhakios.media's processor, src/camera_isp.c), made by tools/camtuning.py out of the
# cameras' tuning files beside them, com.qti.tuned.hakios_<maker>_<sensor>_<camera>.bin: camera-<sensor>.isp
for tuned in ../fw-camera/com.qti.tuned.hakios_*.bin; do
	[ -f "$tuned" ] || continue
	sensor=$(basename "$tuned" .bin | sed 's/^com\.qti\.tuned\.hakios_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camtuning.py "$tuned" "root/fw/camera-$sensor.isp" >/dev/null
done
# ... and with ADSP=1 the image of the audio DSP, from the same place into ../fw-audio (drivers/audio/hakios.adsp.c):
# adsp.mdt and adsp.b02 to adsp.b36, 20 MB. Only when asked for: beside GL=1 the payload would be more than the 64 MB
# the loader takes.
if [ -n "$ADSP" ] && [ -f ../fw-audio/adsp.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] root/fw/
fi
# ... and with DEVICE=NAME what a device's repository carries itself, devices/NAME/firmware/: there the project keeps
# the vendor's files in the repository (the S9's ABOX firmware), so nothing has to be pulled from the phone first
if [ -n "$DEVICE" ]; then
	[ -d ../devices/$DEVICE/firmware ] || { echo "build.sh: DEVICE=$DEVICE: no devices/$DEVICE/firmware (python3 tools/hakios-device.py sync $DEVICE)" >&2; exit 1; }
	mkdir -p root/fw
	find ../devices/$DEVICE/firmware -type f ! -name 'README*' -exec install -m 0644 {} root/fw/ \;
	echo "build.sh: fw/: $(find ../devices/$DEVICE/firmware -type f ! -name 'README*' | wc -l | tr -d ' ') files from devices/$DEVICE/firmware"
fi
# Firmware that is not there is otherwise silent at build time and speaks only much later, deep in a
# device log -- "adreno: THE GPU IS NOT STARTED: the small image has no fw/a660_gmu.bin" cost a phone
# slot on 19 October 2026. It is the vendor's and never in the repository, so a copy made with bare
# git worktree add has none of it, and neither has one whose tools/new-workspace.sh predates the fw-*
# loop there. Say what went in and what was not there to go in; what each one is for is above.
fwhave= fwnone=
for d in gpu wifi haptic camera audio; do
	if [ -d ../fw-$d ]; then fwhave="$fwhave $d"; else fwnone="$fwnone $d"; fi
done
echo "build.sh: fw/: $(ls -1 root/fw 2>/dev/null | wc -l) files from ../fw-*:${fwhave:- none present}${fwnone:+, absent:$fwnone}"
# The images are what somebody would be handed to run this on a second phone, and two of the things
# build.sh puts in them are secrets: the Wi-Fi network's name and passphrase, and the host's own
# private key for ssh. Both are one small file inside 2.5 GB, and nobody would notice either leave.
secrets=
[ -f root/fw/network ] && secrets="$secrets fw/network (the name and passphrase of your Wi-Fi)"
[ -f root/var/root/.ssh/ssh_host_ed25519_key ] &&
	secrets="$secrets /var/root/.ssh/ssh_host_ed25519_key (a private key)"
if [ -n "$secrets" ]; then
	echo "build.sh: this image carries:$secrets"
	echo "build.sh: do not pass it on as it is; SHARE=1 builds the same image without them."
fi
# When the Darwin volume on the phone's flash is to become the root (boot-arg root_apfs=ufs_darwin,
# with flashboot.apfs resident; src/init.c, flash_root): an empty directory to hide Darwin's
# NanoLaunchDaemons and LaunchAngels with,
# xpc without launchd.plist (launchd would read that list of all of Darwin's jobs instead of
# LaunchDaemons) and the audio server's plug-in directory with only our device in it.
mkdir -p root/empty root/xpc root/hal
for f in extensions.plist extensions.plist.sig; do
	[ -f $DARWIN/rootfs/System/Library/xpc/$f ] && cp $DARWIN/rootfs/System/Library/xpc/$f root/xpc/
done
[ -d root/lib/HakiosAudio.driver ] && ln -s /redwood/lib/HakiosAudio.driver root/hal/HakiosAudio.driver
# the image: zlib where it saves room (a small file whole in its attribute, as Apple keeps them); fw/
# as it is, which the kernel's drivers use where it lies in memory (drivers/storage/hakios.image.c)
python3 ../tools/apfs/mkapfs.py -z --plain fw -o root.apfs redwood=root | tail -1
[ -z "$CHECK" ] || ../tools/apfs/check-image.sh root.apfs redwood=root

# --- the RAM part of a boot off the flash: FLASHBOOT=1 -> flashboot.apfs --------------------------------
# With the Darwin volume on the phone's flash as the root, all that has to come from RAM is what is not
# on it -- the two cryptexes, OS with the dyld shared cache and App -- and what starts the boot: three
# APFS containers back to back in the one resident image. The first, the boot volume, is the root the
# kernel starts from (rd=md0): stage0 as /sbin/launchd mounts the small image on /redwood and runs its
# init, which with boot-arg root_apfs=DEV mounts the volume on /flash, makes it the root and moves the
# boot volume to the volume's /private/preboot, then mounts the second container -- the OS cryptex -- on
# the boot volume's empty Cryptexes/OS and the third -- Apple's App cryptex as it is -- on Cryptexes/App:
# /private/preboot/Cryptexes/OS and App, where the volume's own /System/Cryptexes/OS and App point and
# where Darwin grafts the cryptexes (src/init.c, flash_root; the kernel's apfs takes "/dev/md0,c=1" and
# "c=2", and bsd/vm/vm_unix.c takes a shared cache from Cryptexes/OS). CRYPTEX_IMAGE=FILE:
# Apple's own image of the cryptex as it is instead of this one remade (2.6 GB against 1 GB: too much
# for the phone's memory beside Darwin, not for QEMU's). Upload it with `xl ramdisk userland/flashboot.apfs`.
# CRYPTEXES=flash: the boot volume alone (20 MB) -- the cryptexes are Apple's images as they are on the
# flash, in darwin right after the Darwin volume (../tools/darwin-write.py put them there), and init mounts
# them from there, the same containers 1 and 2 of the root's own device.
if [ -n "$FLASHBOOT" ] && [ -f $CACHE/dyld_shared_cache_arm64 ]; then
	rm -rf flashboot flashboot.os
	mkdir -p flashboot/sbin flashboot/redwood flashboot/dev flashboot/flash flashboot/Cryptexes/OS flashboot/Cryptexes/App
	cp obj/stage0 flashboot/sbin/launchd
	# the OS cryptex, and the shared cache in it, only where this image will carry it:
	# with CRYPTEXES=flash the cryptexes are read from the phone's flash and this tree is
	# built and thrown away unused. The guard ends with the cache: everything after it goes
	# into the boot volume, which every recipe carries, flash cryptexes included
	if [ "$CRYPTEXES" != flash ]; then
		if [ -d $DARWIN/cryptex ]; then linkcopy $DARWIN/cryptex flashboot.os; else mkdir -p flashboot.os; fi
		D=flashboot.os/System/Library/Caches/com.apple.dyld
		mkdir -p $D
		for f in $CACHE/dyld_shared_cache_arm64*; do
			case "$f" in *.symbols) continue ;; esac
			# -f: a cryptex fetched with the cache already in it would otherwise stop the build
			ln -f "$f" $D/
		done
	fi
	# the audio DSP's firmware and the speaker amplifiers' configuration, as in the system image's /fw: the kernel
	# reads the firmware as soon as there is a root (this image, adsp=1), the configuration maybe later -- by then
	# from the Darwin volume's /fw, which src/init.c (flash_root) binds to this
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] flashboot/fw/
	fi
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/aw882xx_acf.bin flashboot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: /jb of the boot volume, which is
	# /private/preboot/jb once the Darwin volume is the root -- the volume where a rootless jailbreak keeps its
	# Procursus on an iPhone as well; init grafts it onto /var/jb (src/init.c, mount_writable)
	if [ -d ../jb/root/var/jb ]; then
		cp -a ../jb/root/var/jb flashboot/jb
		mkdir -p flashboot/jb-rw        # where init mounts the prefix's writable copy (src/init.c)
		# and what Sileo runs from /bin, for init to put over the Darwin volume's /bin (see the system image below)
		mkdir -p flashboot/jb-bin
		for c in $SILEO_BIN; do
			ln -s /var/jb/usr/bin/$c flashboot/jb-bin/$c
		done
		echo "  the package manager's prefix: $(du -sh flashboot/jb | cut -f1) as /jb of the boot volume"
	fi
	python3 ../tools/apfs/mkapfs.py -z --plain fw -o flashboot.boot.apfs Boot=flashboot | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.boot.apfs Boot=flashboot
	# the third container: Apple's App cryptex as it is (10 MB; unpack.sh), for the boot volume's Cryptexes/App
	APP=""
	[ -f $DARWIN/cryptex-app.dmg ] && APP=$DARWIN/cryptex-app.dmg
	if [ "$CRYPTEXES" = flash ]; then
		mv flashboot.boot.apfs flashboot.apfs
	elif [ -n "$CRYPTEX_IMAGE" ]; then
		cat flashboot.boot.apfs "$CRYPTEX_IMAGE" $APP > flashboot.apfs
	else
		python3 ../tools/apfs/mkapfs.py -z -o flashboot.os.apfs OS=flashboot.os | tail -1
		[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.os.apfs OS=flashboot.os
		cat flashboot.boot.apfs flashboot.os.apfs $APP > flashboot.apfs
		rm -f flashboot.os.apfs
	fi
	rm -rf flashboot flashboot.os flashboot.boot.apfs
	echo "flashboot.apfs: $(( $(wc -c < flashboot.apfs) / 1048576 )) MB"
fi

# --- the system image -----------------------------------------------------------------------------
if [ -f $DARWIN/system.manifest ] && [ -f $CACHE/dyld_shared_cache_arm64 ] &&
	{ [ ! -f system.apfs ] || [ obj/stage0 -nt system.apfs ] || [ $DARWIN/system.manifest -nt system.apfs ] ||
	  [ $DARWIN/cryptex -nt system.apfs ] || [ $DARWIN/cryptex-app -nt system.apfs ] || [ -n "$SYSTEM" ]; }; then
	# what sysimage/select-system.py has chosen since sysimage/rootfs was extracted is not in it, and would not be
	# in the image either (so it was once with the audio plug-ins and the system's sounds: Darwin found no
	# speaker and played nothing, though our device was there and the kernel's side worked)
	if [ -f $DARWIN/.done-rootfs ] && [ $DARWIN/select-system.py -nt $DARWIN/.done-rootfs ]; then
		echo "build.sh: sysimage/select-system.py has changed since sysimage/rootfs was extracted: run KEEP=1 sysimage/unpack.sh" >&2
		exit 1
	fi
	rm -rf sysroot
	linkcopy $DARWIN/rootfs sysroot                   # hard links or clones: no copies of the 0.5 GB
	# launchd must load our choice of jobs, not all 500 that Darwin has
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	ln -s /redwood/LaunchDaemons sysroot/System/Library/LaunchDaemons
	mkdir -p sysroot/redwood sysroot/sbin
	# where the kernel mounts the Darwin root container off the phone's flash (boot-arg apfs_volume=):
	# Darwin has no /System/Volumes, and nothing can make a directory in this image once it is built
	mkdir -p sysroot/System/Volumes/Darwin
	# the applications Darwin installs on its first boot are system applications here (see select-system.py)
	if [ -d sysroot/private/var/staged_system_apps ]; then
		mv sysroot/private/var/staged_system_apps/* sysroot/Applications/
		rm -rf sysroot/private/var
	fi
	mv sysroot/sbin/launchd sysroot/sbin/launchd.darwin
	cp obj/stage0 sysroot/sbin/launchd
	# the applications' frame rate: on a phone CoreAnimation holds every process to 60 frames a second unless its
	# bundle says CADisableMinimumFrameDurationOnPhone, which no application of an iPhone X's system does; the panel
	# here runs at 120 (IOMFBDisplayRefresh, MiniIOMobileFramebuffer.cpp). New files, not edits: sysroot's are links
	python3 mkhighrate.py sysroot
	# the audio DSP's firmware, the phone's own from ../fw-audio (never in the repository), where the kernel reads
	# it once the root is mounted (drivers/audio/hakios.adsp.c, adsp=1): 20 MB that do not fit beside GL=1 in the small image
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	# MPSS firmware is much too large for the small boot image. Keep it in the system APFS,
	# only when explicitly requested, after checking every loadable segment against this DTB.
	# This stages the files; a separate XNU MPSS driver is still required to start the modem.
	if [ -n "$MODEM" ]; then
		[ -f ../fw-modem/modem.mdt ] || { echo "build.sh: MODEM=1 needs ../fw-modem/modem.mdt" >&2; exit 1; }
		../devices/redwood/tools/modem_firmware.py --dtb ../devices/redwood/prebuilt/device-tree.dtb --firmware-dir ../fw-modem \
			--node /soc/qcom,mss@4080000 >/dev/null
		mkdir -p sysroot/fw
		cp ../fw-modem/modem.mdt ../fw-modem/modem.b[0-9][0-9] sysroot/fw/
		# ... and the firmware partition's image/ (its mcfg configurations and the registry's *.jsn, pulled from
		# the phone's /vendor/firmware_mnt/image into ../fw-modem/image), which the modem reads over TFTP (hakios.mpss.c)
		if [ -d ../fw-modem/image ]; then
			rm -rf sysroot/fw/image && cp -R ../fw-modem/image sysroot/fw/image
		fi
	fi
	# ... and the speaker amplifiers' configuration, the phone's /vendor/etc/aw882xx_acf.bin (drivers/audio/hakios.speaker.c,
	# for sound=3): Android's own register values, which are not in the repository either
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: the Procursus bootstrap with
	# Sileo in it, as /jb. It cannot live at /var/jb in the image -- /private/var is a memory
	# filesystem made afresh at every boot -- so init copies it there (src/init.c, copy_tree).
	if [ -d ../jb/root/var/jb ]; then
		rm -rf sysroot/jb
		cp -a ../jb/root/var/jb sysroot/jb
		# the applications of the prefix are system applications here: LaunchServices builds its
		# database afresh at every boot by walking /Applications, which is how their icons appear
		for app in sysroot/jb/Applications/*.app; do
			[ -d "$app" ] && cp -a "$app" sysroot/Applications/
		done
		# Sileo moves, copies and removes files as root with /bin/mv, /bin/cp, /bin/rm, /bin/mkdir and
		# /bin/chmod -- where a jailbreak with a writable root keeps them -- and Darwin's /bin has df and ps
		# alone: every one of those spawns failed with nothing said, and the package it had downloaded never
		# reached apt's archives ("Unable to fetch some archives"). The prefix's own programs, by those names.
		for c in $SILEO_BIN; do
			ln -sf /var/jb/usr/bin/$c sysroot/bin/$c
		done
		echo "  the package manager's prefix: $(du -sh sysroot/jb | cut -f1) as /jb (init puts it at /var/jb)"
	fi
	# An application of this port's own, registered the same free way the prefix's are: a plain
	# directory in /Applications, which the LaunchServices database is rebuilt from at every boot
	# by walking that directory -- no installd, no container, no uicache. cp -a and not a link,
	# because a bundle of symbolic links is refused ("Unable to register") and the boot-time
	# registrar passes over mount points.
	#
	# This is in the SYSTEM image and not the small one, so a change to it costs a 2.5 GB rebuild
	# and a re-upload: SYSTEM=1 is what makes this line run again.
	if [ -d obj/HakiosTriangle.app ]; then
		cp -a obj/HakiosTriangle.app sysroot/Applications/
		echo "  HakiosTriangle.app in /Applications (com.hakios.triangle, scheme hakiostriangle:)"
	fi
	# Applications from outside this tree: every .app in ../apps, which is kept out of the
	# repository the way ../fw-* and ../jb are, so that nothing of somebody else's package can
	# reach it. They go into /Applications beside the port's own on exactly the same terms --
	# a plain directory, which LaunchServices finds by walking that folder at every boot.
	#
	# cp -aL and not cp -a: a bundle made of symbolic links is refused ("Unable to register"),
	# so whatever a link points at is copied as the file itself.
	for app in ../apps/*.app; do
		[ -d "$app" ] || continue
		cp -aL "$app" sysroot/Applications/
		echo "  ${app##*/} in /Applications ($(du -sm "$app" | cut -f1) MB from ../apps)"
	done
	# our audio device, where the audio server looks for such plug-ins (and for no others: Apple's are left out)
	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -s /redwood/lib/HakiosAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver
	# the plug-in Metal opens when the accelerator names it (boot-arg metal=1; without it nothing names this bundle)
	mkdir -p sysroot/System/Library/Extensions
	[ -d root/lib/HakiosMetal.bundle ] && ln -sfn /redwood/lib/HakiosMetal.bundle sysroot/System/Library/Extensions/HakiosMetal.bundle
	# METAL_ICD's file is read "beside the bundle", which is either directory depending on the path the bundle was loaded by
	if [ -f root/lib/HakiosMetal.icd ]; then ln -sfn /redwood/lib/HakiosMetal.icd sysroot/System/Library/Extensions/HakiosMetal.icd; fi
	# the OS cryptex: on a device a separate volume grafted at /System/Cryptexes/OS. It holds the dyld
	# shared cache (dyld looks there first) and a few libraries and resources outside the cache.
	mkdir -p sysroot/System/Cryptexes
	if [ -d $DARWIN/cryptex ]; then linkcopy $DARWIN/cryptex sysroot/System/Cryptexes/OS; fi
	# the App cryptex, the same way: Safari's daemons (eight launchd jobs run them from there), its settings and
	# the bundles around it, which the root's links reach (System/Library/PreferenceBundles/MobileSafariSettings.bundle...)
	if [ -d $DARWIN/cryptex-app ]; then linkcopy $DARWIN/cryptex-app sysroot/System/Cryptexes/App; fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac     # symbolication data, never mapped
		# -f: a cryptex fetched with the cache already in it would otherwise stop the build
		ln -f "$f" $D/
	done
	# zlib where it saves room, as Apple keeps the root (a small file whole in its attribute): 5.4 GB of
	# files in 2.5 GB, what fits into the phone's RAM beside Darwin
	python3 ../tools/apfs/mkapfs.py -z -o system.apfs System=sysroot,manifest=$DARWIN/system.manifest | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh system.apfs System=sysroot,manifest=$DARWIN/system.manifest
	rm -rf sysroot
fi
