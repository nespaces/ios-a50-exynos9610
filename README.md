# HakiOS on the Samsung Galaxy A50 (SM-A505F*, Exynos 9610)

A port of [HakiOS](core/README.md) — Apple's open-source XNU kernel with the Darwin/iOS 16.5 user space — to the
Galaxy A50, built on the Galaxy S9+ work in [danstronics/ios-s9plus-exynos](https://github.com/danstronics/ios-s9plus-exynos)
(its README is kept as [README-S9PLUS.md](README-S9PLUS.md); its history is this repository's history).

This is an independent hobby integration. It is not affiliated with or endorsed by Apple or Samsung, and it ships
**no Apple software and no Samsung firmware**: see [What is not here](#what-is-not-here).

## Status (November 2026)

| Area | State |
|---|---|
| Boot | S-Boot → `devices/a50/soc/shim` (EL1, no UEFI) → xnuloader → XNU 22.5 → launchd → SpringBoard |
| Display | S-Boot's framebuffer (1080×2340, DECON_F), TE-driven, triple-buffered by the DPU (no tearing); iOS's 1125×2436 scaled on 6 cores, ~60 fps in the UI |
| Storage | own UFS driver for the 9610 (`ufs_native=4`): iOS system on SYSTEM, `/private/var` (HFS+) on USERDATA, kept between boots |
| CPU | all 8 cores (`cpus=8 unpark=1`: core 1 was only believed to be parked, as on the S9, and starts fine), PSCI via a low trampoline (EL3 keeps 32 bits of the entry) |
| GPU | Mali-G72 MP3 with Mesa/Panfrost (accelerated CoreAnimation), 683 MHz via ACPM-driven rail |
| Touch | Melfas MSS100 (`drivers/input/hakios.melfas_mss100.c`) |
| Keys | power / volume (`exynos_keys=2`) |
| Battery | SM5713 fuel gauge + MUIC, read only (`drivers/power/hakios.sm5713.c`) |
| ACPM | PMIC channel works; the `fvp` plugin is loaded and attached by the kernel; CPU clusters set to 2080/1534 MHz |
| Sound | speaker: ABOX (CPU off) → UAIF2 → NXP TFA9872 (`abox=8 tfa9872=3`), volume from iOS |
| Brightness | EA8076 panel via DSIM0 DCS 51, from Settings and Control Center |
| Vibration | the motor via its GPIO-switched LDO (`vib=1`) |
| Wi-Fi | Samsung SCSC S620 (Maxwell `mx140`), a driver of its own (`drivers/wifi/hakios.scsc.c`, `scsc=6`): the chip powered and booted, HIP4, scan, WPA2 join from Settings, DHCP, TCP over it with a send queue and long receive chains handled (YouTube-class traffic holds with Bluetooth off); with Bluetooth on, the firmware faults and the link can drop (open); Settings does not show the address yet |
| Flashlight | SM5713 FLED1, on/off and brightness from Control Center (`torch=1 sm5713=1`). Tested on the A50 with `/var` in RAM. |
| Microphone | RT5665 codec (MAINMIC on IN3) → ABOX UAIF0 → `/dev/hakios-mic`: Voice Memos records a voice (`rt5665=3 abox_mic=1`, `drivers/audio/hakios.rt5665.c`) |
| Camera | in progress: the rear sensor (Sony IMX576) is powered (S2MPB03), clocked and reset, answers on its I2C bus (model id 0576) and streams (`campwr=5`, drivers/camera/hakios.campwr.c; its register tables come from the stock kernel and are not in this repository); the MIPI receiver (CSIS v5.1) is readable; no frame is captured yet | (the CSIS DMA test pattern into memory, `campwr=8`, is set up as the S9 port does it and does not produce a frame yet)
| Power off | "slide to power off" ends in TWRP (the shutdown no longer waits on the system unmount); holding the power key for 6 s goes back to TWRP too |
| Screen rotation | by hand: hold both volume keys for 0.7 s (`kern.hakios.orientation`); the accelerometer is behind the CHUB sensor hub, whose firmware is not loaded yet |
| Temperature | the 9610's TMU read by the kernel (`tmu9610=N`, drivers/power/hakios.a50_tmu.c): in the log, `kern.hakios.temp.*`, and with `tmu9610=3` the run goes back to the loader at 95 degrees (`tmu_stop=`); two sensors answer (0 and 2), the rest are not wired here |
| Speed | GPU on its 764 MHz level (775 mV, the A50's own ACPM table: 1053/949/839/764/683 MHz at 993.75/900/818.75/775/737.5 mV; `mali_mhz=`, `mali_uv=`), the CPUs on the table's stock top levels with `acpm_cl0=1742000 acpm_cl1=2314000` (A53/A73; hot: there is no thermal driver for the 9610 yet) |
| Bluetooth | works in part, on the Wi-Fi chip (`scsc=8`): iOS's `bluetoothd` talks HCI to the controller over a kernel control; Settings shows it, other phones see this one, AirPods pair and their controls work. Audio does not go to Bluetooth yet (no route in the audio driver), and the AirPods settings (noise control) do nothing | (A2DP: `bluetoothd` already negotiates the stream with AirPods; the sound still stays on the speaker because the audio server does not load Apple's `BTAudioHALPlugin` here, which has to be taken from your own IPSW into `apple-hal/`)
| Not yet | camera capture, ringtones (AAC), Metal in apps (Notes' drawing), modem |

## Building

1. Set up the HakiOS build as described in [core/README.md](core/README.md) (XNU sources from Apple's open-source
   repository via `core/xnu/apply.sh`, Mesa via `core/tools/mk-mesa.sh`).
2. From your own IPSW: `core/xnuloader/mkidentity.sh` (the loader's device identity) and the user space images
   (`core/userland/build.sh`). Nothing of the IPSW is in this repository.
3. From your own phone's stock firmware: `core/tools/a50-fvp.py boot.img` (Samsung's ACPM `fvp` plugin; without it
   the kernel leaves CPU DVFS alone).
4. `make a50_defconfig && make kernel`, then `devices/a50/mkimage.sh` (see [core/devices/a50/README.md](core/devices/a50/README.md)
   for the boot-arguments and the per-phone values).

Flashing is done from TWRP (`dd` to BOOT; SYSTEM and USERDATA are replaced). **Back up your phone's partitions
first** — this erases Android.

## What is not here

- Apple: no IPSW, no iOS/Darwin binaries, no device trees, no firmware. Every builder uses their own IPSW;
  generated files (`identity.h`, `*.apfs`) are git-ignored.
- Samsung: no S-Boot, no ACPM plugins, no vendor firmware (audio DSP, Wi-Fi, camera). The `fvp` plugin header in
  the tree is a placeholder; `tools/a50-fvp.py` makes the real one from the builder's own stock BOOT.
- Nothing of any one phone: no serial numbers, IMEI, MAC addresses, partition dumps or logs. The UFS layout check
  (`ufs_native_ref_fnv`) is computed by each builder from their own phone.

## Licenses

As upstream: the kernel parts (`core/xnu/`, `core/drivers/`) are APSL 2.0 derivative work of Apple's XNU; the rest is
MIT. See [core/LICENSE](core/LICENSE), [NOTICE](NOTICE) and [SOURCES.md](SOURCES.md). Apple, iOS and iPhone are
trademarks of Apple Inc.; Samsung, Galaxy and Exynos are trademarks of Samsung Electronics. They are used here only to
identify hardware and the original software.
