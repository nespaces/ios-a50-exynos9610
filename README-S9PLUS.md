# iOS research port for Galaxy S9+ Exynos

Samsung **SM-G965F / star2lte / Exynos 9810 / Mali-G72**, using the Scion fork of HakiOS and an iOS 16.5 userspace supplied by the user.

## Highlights of this S9+ build

Device-specific features and improvements over this port's earlier S9+ bring-up builds:

- **Installed on flash:** BOOT and iOS SYSTEM reside on the phone's UFS, and the loader reads SYSTEM directly from flash. A host upload is not required for each boot. Writable iOS data remains in RAM, and the current 600-second return-to-recovery timer is preserved.
- **Hardware-accelerated GLES on Mali-G72:** actual GPU rendering through Mesa/Panfrost, with local mapping, cache and advisory changes. This is the installed graphics path; Metal-to-Vulkan remains separate WIP.
- **Improved UI smoothness and presentation:** smoother interaction than the initial bring-up, based on on-device feedback, with display flicker and scrolling tearing resolved in the reported tests. Heavy SpringBoard widget/blur scenes still stall; sustained 60 FPS is not established.
- **UI adapted to the full S9+ panel:** a 1188 × 2442 render canvas scaled through the Exynos display path, adjusted proportions and status-bar insets for the curved edges. This is a scaled render resolution, not native-resolution rendering.
- **Touch and physical buttons:** working on-device input, including Bixby mapped to Home. Touch latency and keyboard/emoji defects remain under investigation.
- **Battery and charging indication:** battery level and charger state have been displayed in iOS.
- **Broadcom Wi-Fi bring-up:** association, DHCP, gateway traffic, DNS and SSH have been verified in development sessions. Reliable reconnection across boots remains unfinished.
- **Six CPUs enabled:** the installed configuration brings up six cores; using every CPU and all physical RAM remains unfinished.
- **TWRP-preserving, verified installation:** the release installer writes BOOT only, checks the matching SYSTEM and firmware, verifies the complete BOOT readback and confirms RECOVERY is unchanged.
- **Private device identity stays local:** the public BOOT template has its MAC field cleared. Installation supplies the target phone's own original MAC locally; no maintainer Wi-Fi password, personal MAC, saved network profile or partition backup is distributed.

**The default release carries the OpenGL ES/Panfrost build verified in the maintainer's phone flash, with each phone's own Wi-Fi MAC inserted locally. Metal → Vulkan is WIP and is not the default flash target.** This is a research build, with substantial animation and touch latency; sustained 60 FPS, working 3D WebGL and daily-phone readiness are not established.

## Installed build

The release identity is defined in [`profiles/installed-gles.json`](profiles/installed-gles.json). BOOT, SYSTEM and RECOVERY were read and hashed in TWRP on 6 November 2026. The installed BOOT matches the previous successful rollback exactly.

| Item | Verified state |
| --- | --- |
| Target | SM-G965F, `star2lte`; Snapdragon variants excluded |
| Default compositor | OpenGL ES through Mesa/Panfrost, with local mapping/advisory changes |
| UI | iOS 16.5 SpringBoard and Settings have run on hardware |
| Display | 1188 × 2442 render canvas, scaled by the Exynos display path; panel refresh is distinct from application FPS |
| Touch/buttons | Functional; latency and keyboard defects remain |
| Battery/charger indication | Observed; complete power management not established |
| Wi-Fi | Association, DHCP, gateway ping, DNS and SSH verified in development sessions |
| CPU/RAM | Six CPUs and the currently configured contiguous memory bank; full hardware-resource use is unfinished |
| UFS | SYSTEM is installed and read only; temporary `/private/var` remains volatile |
| Camera/audio/cellular/App Store | Not qualified by this release |
| Metal/Vulkan | Separate experiments; see [`metal-wip/`](metal-wip/) |

The original return timer is preserved: **this BOOT returns to recovery after approximately 600 seconds**. This publication does not silently replace it with a different long-running image.

## Installation and exactness

Use the **installed-gles** release and read [`INSTALL.md`](INSTALL.md). Its TWRP package contains a verified BOOT template, with the private MAC field cleared. It checks the template and original firmware tail, then inserts the installing phone's original MAC from its already-accessible EFS into recovery RAM. It refuses a different model, a different starting BOOT, a different SYSTEM, or an unavailable/invalid original MAC. Full write readback is verified.

Kernel, libraries, settings and firmware match the installed build. The only personalized field is the 18-byte `fw/mac.info` file. Reinserting the maintainer's MAC locally reconstructs the **exact complete BOOT currently in flash**, with the recorded hash; another phone correctly has its own MAC and a different final hash. The maintainer's MAC is never distributed. **Do not flash `boot-prefix.template` directly.**

The package writes **BOOT only**. It does not install iOS SYSTEM, alter RECOVERY, format DATA, or provide a universal Android-to-iOS conversion. Apple userspace and device firmware must come from the user's own inputs. No SYSTEM dump, Android backup, Apple IPSW, saved network profile, SSH key or personal data is published.

The installed GLES build is the release target, with device identity supplied locally. Recompiling the sources is a separate development workflow and is not claimed to produce a bit-identical binary automatically. The public assembler and installer must match the recorded template hashes before inserting the device-local MAC.

See [`TESTING.md`](TESTING.md) for the publication checks and their limits. Historical notes in `core/docs/` describe earlier development stages; this README and the release profile describe the current published target.

## Sources and attribution

This work builds on **HakiOS, Scion, Apple XNU, Mesa/Panfrost, Project Silicium, UniXNU/AdrenoMetal and metal2vulkan**. See [`SOURCES.md`](SOURCES.md), [`NOTICE`](NOTICE), and the original notices retained under [`core/`](core/). This is an independent integration, not an upstream release or an Apple product.

`core/` is the source snapshot associated with the installed kernel revision. Local GLES support changes and all new Metal/Vulkan experiments are identified separately, with their verification limits. The repository starts from a clean publication snapshot rather than importing the development workspace or its private history.
