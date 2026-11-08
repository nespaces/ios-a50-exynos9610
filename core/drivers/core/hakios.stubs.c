/*
 * What the rest of the kernel calls in a driver that is left out of it (CONFIG_HAKIOS_<NAME> not set: drivers/<subsystem>/conf,
 * docs/drivers-config.md), the way XNU keeps bsd/kern/bsd_stubs.c for its own parts that a configuration leaves out.
 *
 * The callers are the places that start the drivers (hakios.input.c, hakios.audio.c, bsd_init.c), that stop them on
 * the way back to the loader or out of a panic (loader_return.c, the thermal and battery guards, machine_routines.c)
 * and the few that ask a driver something on the way (is this device one of the storage's, what the battery's
 * temperature is). Each of them calls the driver unconditionally, as it did when every driver was always built; here,
 * for a driver that is not, is what each call then does: nothing, and the answer "no", "not known" or "not there".
 * A driver that is built gets none of this: every block below is for one driver, and empty when that one is in.
 *
 * What is not here, on purpose: a call from one driver to another that it cannot work without. That is a `requires`
 * of its conf (SOUND requires ADSP and SPEAKER, MODEM requires QMI and UFS, ...), and tools/hakios-config.py refuses
 * the configuration that would need a stand-in for it.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <mach/boolean.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.exynos_abox.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.exynos_pcie.h>
#include <sys/hakios.input.h>
#include <sys/hakios.max98512.h>
#include <sys/hakios.net.h>
#include <sys/hakios.sensors.h>
#include <sys/hakios.sta.h>
#include <sys/hakios.virtio_blk.h>

/* the ends of a run (loader_return.c, the guards), which have no header of their own */
void hakios_adsp_stop(void);
void hakios_adsp_quiesce(void);
void hakios_mpss_stop(void);
void hakios_mpss_quiesce(void);
void hakios_ddr_quiesce(void);
void hakios_ufs_quiesce(void);
void hakios_var_save_on_return(void);
void hakios_mic_quiesce(void);
void HakiosWLANStart(const char *interface, const uint8_t *address);
void hakios_exynos_halt_off(boolean_t reboot);          /* (machine_routines.c's halt_all_cpus(), patch 1510) */
bool hakios_s6e3ha8_blank(int (*say)(const char *, ...));

#ifndef HAKIOS_EXYNOS_ACPM
void hakios_exynos_dvfs_start(void);
void
hakios_exynos_dvfs_start(void)
{
}
/* (hakios.exynos_poweroff.c asks for the APM's stop with halt_off=2: no ACPM, nothing to stop) */
bool hakios_acpm_init(__unused int (*say)(const char *, ...)) { return false; }
int hakios_acpm_stop(void) { return ENXIO; }
#endif

#ifndef HAKIOS_S2MPS18_RTC
uint64_t hakios_rtc_boot_time(uint64_t epoch);
/* the time of day at start-up (GenericARMPE.cpp): no clock of the phone's own, what the loader or epoch= said */
uint64_t
hakios_rtc_boot_time(uint64_t epoch)
{
	return epoch;
}
#endif

#ifndef HAKIOS_EXYNOS_CPUFREQ
bool hakios_exynos_cpufreq_gov_start(void);
bool
hakios_exynos_cpufreq_gov_start(void)
{
	return false;
}
/* the governor's boosts (hakios.s6sy761.c, MiniIOMobileFramebuffer.cpp): no governor, nothing to boost */
void hakios_exynos_cpufreq_touch(void);
void hakios_exynos_cpufreq_touch(void) {}
void hakios_exynos_cpufreq_frame(void);
void hakios_exynos_cpufreq_frame(void) {}
#endif

#ifndef HAKIOS_EXYNOS_TMU
int32_t hakios_exynos_tmu_zone_mc(unsigned zone);
/* no TMU driver: no temperature ("not read", as the driver says before its first reading) */
int32_t
hakios_exynos_tmu_zone_mc(__unused unsigned zone)
{
	return -273000;
}
#endif

#ifndef HAKIOS_VIRTIO_INPUT
void hakios_virtio_input_init(void) {}
#endif

#ifndef HAKIOS_GOODIX
void hakios_goodix_init(void) {}
#endif

#ifndef HAKIOS_BUTTONS
void hakios_buttons_init(void) {}
#endif
#ifndef HAKIOS_EXYNOS_KEYS
void hakios_exynos_keys_init(void) {}
#endif
#ifndef HAKIOS_S6SY761
void hakios_s6sy761_init(void) {}
#endif
#ifndef HAKIOS_MELFAS_MSS100
void hakios_melfas_mss100_init(void) {}
#endif
#ifndef HAKIOS_MAX77705
void hakios_max77705_init(void) {}
#endif
#ifndef HAKIOS_SM5713
void hakios_sm5713_init(void) {}
#endif
#ifndef HAKIOS_EXYNOS_CAMERA
void hakios_exynos_camera_init(void) {}
void hakios_exynos_camera_quiesce(void) {}
/* the USB console's camshot (hakios.dwc3.c): no camera here */
const char *hakios_exynos_camera_shot(uint32_t n, int pattern);
const char *hakios_exynos_camera_shot(__unused uint32_t n, __unused int pattern) { return "this kernel has no Galaxy S9 camera driver (CONFIG_HAKIOS_EXYNOS_CAMERA)"; }
#endif
#ifndef HAKIOS_S2MPB02_TORCH
void hakios_s2mpb02_torch_init(void) {}
void hakios_s2mpb02_torch_quiesce(void) {}
#endif
#ifndef HAKIOS_BCM4361_BT
void hakios_bcm4361_bt_init(void) {}
extern void loader_log_line(const char *line);
void hakios_bt_console(__unused const char *arg) { loader_log_line("usb: bt: this kernel has no Bluetooth driver (BCM4361_BT)\n"); }
#endif
#ifndef HAKIOS_SSP
void hakios_ssp_init(void) {}
void hakios_ssp_console(__unused const char *arg) {}  /* the USB console's "sensors": no hub driver */
#endif
#ifndef HAKIOS_EXYNOS_PCIE
void hakios_exynos_pcie_init(void) {}
void hakios_exynos_pcie_quiesce(void) {}
#endif
#ifndef HAKIOS_BRCMFMAC
void hakios_brcmfmac_start(void) {}     /* wifi_pcie=6 in a kernel without the S9's radio driver: the link stays as it is */
const char *hakios_bcm4361_module(void) { return NULL; }       /* nobody reads the module's OTP */
#endif
#if !defined(HAKIOS_WIFI) && !defined(HAKIOS_BRCMFMAC)
extern int vprintf(const char *format, va_list ap) __printflike(1, 0);
/* the station layer's log (WIFI_STA) where no radio's driver, whose log it is, is built: the console */
int
hakios_wifi_tell(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vprintf(format, ap);
	va_end(ap);
	return n;
}
/* what hakios.lx.c (always built) asks of a radio, in a kernel that has none: nothing, and it says so */
int
hakios_wifi_say(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vprintf(format, ap);
	va_end(ap);
	return n;
}
uint32_t hakios_wifi_option(__unused const char *name) { return 0; }
bool hakios_wifi_dsp_is_up(void) { return false; }
const uint8_t *hakios_wifi_driver_file(__unused const char *name, __unused uint64_t *size) { return NULL; }
bool hakios_wifi_radio_map(__unused uint64_t physical, __unused uint64_t size, __unused unsigned long long *address) { return false; }
bool hakios_wifi_radio_msi(__unused unsigned long long *address, __unused uint32_t *data, __unused uint32_t *first_spi) { return false; }
bool hakios_wifi_radio_address(__unused uint8_t *address) { return false; }
bool hakios_wifi_network(__unused uint8_t *name, __unused unsigned *name_length, __unused uint8_t *passphrase,
    __unused unsigned *passphrase_length) { return false; }
#endif
#ifndef HAKIOS_EXYNOS_ABOX
void hakios_exynos_abox_init(void) {}
void hakios_exynos_abox_quiesce(void) {}
void hakios_abox_set_amp(__unused const struct hakios_abox_amp *amp) {}   /* an amplifiers' driver built without ABOX's */
void hakios_abox_console(__unused const char *arg) {}   /* the USB console's "abox": nothing to drive */
bool hakios_abox_headphones(__unused bool on) { return false; }      /* the codec's headphones: no stream to route */
#endif
#ifndef HAKIOS_CS47L93
void hakios_cs47l93_console(__unused const char *arg) {}        /* the USB console's "codec": nothing to drive */
void hakios_cs47l93_quiesce(void) {}
#endif
#ifndef HAKIOS_MAX98512
void hakios_max98512_quiesce(void) {}
#endif
#ifndef HAKIOS_S6E3HA8
void hakios_s6e3ha8_init(void) {}
bool hakios_s6e3ha8_blank(__unused int (*say)(const char *, ...)) { return false; }      /* "not blanked" */
#endif

#ifndef HAKIOS_EXYNOS_POWEROFF
void hakios_exynos_halt_off(__unused boolean_t reboot) {}  /* a halt hands the phone to the loader, as it always did */
#endif
#ifndef HAKIOS_EXYNOS_DPU
#include <sys/hakios.viewport.h>
int hakios_scanout_viewport(__unused struct hakios_viewport *v, __unused uint32_t *w, __unused uint32_t *h) { return 0; }
/* the display's own scan-out (MiniIOMobileFramebuffer.cpp, fb_flip/fb_dpp/fps_log): none, frames go into the boot framebuffer */
int hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes);
int hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t *phys);
int hakios_scanout_attach(int mode, const uint64_t *phys, unsigned count, uint32_t width, uint32_t height,
    uint32_t rowbytes, unsigned shown);
int hakios_scanout_flip(unsigned index);
int hakios_scanout_latched(void);
void hakios_scanout_detach(const char *why);
uint32_t hakios_scanout_refreshes(void);
int hakios_scanout_probe(__unused uint64_t fb_phys, __unused uint32_t width, __unused uint32_t height, __unused uint32_t rowbytes) { return -1; }
int hakios_scanout_memory(__unused uint64_t bytes, __unused unsigned count, __unused uint64_t *phys) { return -1; }
int hakios_scanout_attach(__unused int mode, __unused const uint64_t *phys, __unused unsigned count, __unused uint32_t width,
    __unused uint32_t height, __unused uint32_t rowbytes, __unused unsigned shown) { return -1; }
int hakios_scanout_flip(__unused unsigned index) { return -1; }
int hakios_scanout_latched(void) { return 1; }
void hakios_scanout_detach(__unused const char *why) {}
uint32_t hakios_scanout_refreshes(void) { return 0; }
int hakios_scanout_carve(uint64_t bytes, uint64_t *phys);
void hakios_scanout_uncarve(uint64_t phys);
int hakios_scanout_carved(uint64_t phys);
int hakios_scanout_show(uint64_t phys, uint32_t rowbytes);
int hakios_scanout_carve(__unused uint64_t bytes, __unused uint64_t *phys) { return -1; }
void hakios_scanout_uncarve(__unused uint64_t phys) {}
int hakios_scanout_carved(__unused uint64_t phys) { return 0; }
int hakios_scanout_show(__unused uint64_t phys, __unused uint32_t rowbytes) { return -1; }
#endif

#ifndef HAKIOS_USB_GADGET
void hakios_usb_gadget_quiesce(void) {}
/* a picture for the USB console (hakios.exynos_camera.c's camshot): no USB link here */
int hakios_usb_picture(uintptr_t rgb, uint32_t width, uint32_t height, const char *what);
int hakios_usb_picture(__unused uintptr_t rgb, __unused uint32_t width, __unused uint32_t height, __unused const char *what) { return -1; }
#endif
#ifndef HAKIOS_EXYNOS_M3
int hakios_psci_cpu_on_prepare(int cpu, uint32_t mpidr);
/* before CPU_ON (psci_smp.c, patch 1508): go ahead, nothing to prepare */
int hakios_psci_cpu_on_prepare(__unused int cpu, __unused uint32_t mpidr) { return 0; }
#endif
#ifndef HAKIOS_EXYNOS_USB
struct hakios_usb_glue;
const struct hakios_usb_glue *hakios_usb_glue_exynos9810(void);
const struct hakios_usb_glue *hakios_usb_glue_exynos9810(void) { return NULL; }     /* usb_gadget= then says there is no glue */
#endif

#ifndef HAKIOS_UFS
void hakios_ufs_init(void) {}
void hakios_ufs_report(__unused const char *when) {}
bool hakios_ufs_dev(__unused dev_t dev) { return false; }
void hakios_ufs_quiesce(void) {}
void hakios_ufs_summary(const char *when, int how);
void hakios_ufs_summary(__unused const char *when, __unused int how) {}      /* no flash driver: nothing to say */
void hakios_var_save_on_return(void) {}         /* no flash, so no data volume to save into it */
int hakios_partition_file(__unused const char *partition, __unused const char *mount, __unused const char *path,
    __unused uint64_t max, const uint8_t **data, uint64_t *size)
{
	*data = NULL;
	*size = 0;
	return ENXIO;                           /* no flash: no partition's file (the drivers say so and go on without) */
}
#endif

#ifndef HAKIOS_VIRTIO_BLK
void hakios_virtio_blk_init(void) {}
bool hakios_virtio_blk_dev(__unused dev_t dev) { return false; }
/* (IOFindBSDRoot asks these whatever the configuration: xnu/patches/1100, 1101) */
int hakios_virtio_blk_rootdev(__unused const char *name, __unused dev_t *dev) { return ENXIO; }
int hakios_virtio_blk_alias(__unused const char *disk, __unused const char *name) { return ENXIO; }
#endif

#ifndef HAKIOS_BATTERY
void hakios_battery_init(void) {}
int hakios_battery_temperature(void) { return -273; }      /* "not known", as the driver says before its first reading */
void hakios_battery_report(__unused const struct hakios_battery_state *state) {}
#endif

#ifndef HAKIOS_CPUFREQ
void hakios_cpufreq_init(void) {}
void hakios_ddr_quiesce(void) {}
#endif

#ifndef HAKIOS_ADRENO
void hakios_adreno_init(void) {}
void hakios_adreno_quiesce(void) {}
void hakios_adreno_stop(void) {}
void hakios_adreno_minute(void) {}
#endif

#ifndef HAKIOS_MALI
void hakios_mali_init(void) {}
/* the display's fps_log line (MiniIOMobileFramebuffer.cpp): no Mali, nothing to say of it */
int hakios_mali_second(uint64_t *busy_ns, uint32_t *jobs, uint64_t *queued_ns, uint32_t *mhz, uint32_t *switches,
    int32_t *g3d_mc);
int hakios_mali_second(__unused uint64_t *busy_ns, __unused uint32_t *jobs, __unused uint64_t *queued_ns,
    __unused uint32_t *mhz, __unused uint32_t *switches, __unused int32_t *g3d_mc) { return 0; }
/* a frame given to the display (MiniIOMobileFramebuffer.cpp present()), for mali_gov_frame: no Mali, no floor */
void hakios_mali_frame(void);
void hakios_mali_frame(void) {}
#endif

#ifndef HAKIOS_WIFI
void hakios_wifi_init(void) {}
#ifndef HAKIOS_BRCMFMAC                         /* (the S9's radio driver has its own: the radio unpowered on the way back) */
void hakios_wifi_stop(void) {}
void hakios_wifi_quiesce(void) {}
#endif
#endif

#ifndef HAKIOS_WIFI_STA
errno_t hakios_apple80211_ioctl(__unused u_long command, __unused void *data) { return EOPNOTSUPP; }
void HakiosWLANStart(__unused const char *interface, __unused const uint8_t *address) {}
void hakios_sta_attach(__unused const struct hakios_sta_ops *ops) {}   /* (hakios.lx.c: a radio's station, nobody to give it to) */
void hakios_sta_lost(__unused int error) {}
extern void loader_log_line(const char *line);
void hakios_sta_console_scan(void) { loader_log_line("usb: wifi scan: this kernel has no station (WIFI_STA)\n"); }
#endif

#ifndef HAKIOS_ADSP
void hakios_adsp_init(void) {}
void hakios_adsp_stop(void) {}
void hakios_adsp_quiesce(void) {}
#endif

#ifndef HAKIOS_MODEM
void hakios_mpss_init(void) {}
void hakios_mpss_stop(void) {}
void hakios_mpss_quiesce(void) {}
#endif

#ifndef HAKIOS_VIRTIO_NET
void hakios_virtio_net_init(void) {}
#endif

#ifndef HAKIOS_VIRTIO_SOUND
void hakios_virtio_sound_init(void) {}
#endif

#ifndef HAKIOS_SPEAKER
void hakios_speaker_init(void) {}
#endif

#ifndef HAKIOS_SOUND
void hakios_sound_dsp_up(void) {}
void hakios_sound_dsp_down(void) {}
void hakios_sound_quiesce(void) {}
#endif

#ifndef HAKIOS_MIC
void hakios_mic_dsp_up(void) {}
void hakios_mic_dsp_down(void) {}
void hakios_mic_quiesce(void) {}
#endif

#ifndef HAKIOS_HAPTIC
void hakios_haptic_init(void) {}
void hakios_haptic_quiesce(void) {}
void hakios_haptic_pretend(__unused uint32_t peak) {}
#endif

#ifndef HAKIOS_TORCH
void hakios_torch_init(void) {}
void hakios_torch_quiesce(void) {}
#endif

#ifndef HAKIOS_CAMERA
void hakios_camera_init(void) {}
void hakios_camera_quiesce(void) {}
#endif
