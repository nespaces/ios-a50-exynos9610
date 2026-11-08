/*
 * The Galaxy S9's display controller (Exynos 9810 DPU) as the display driver's scan-out: MiniIOMobileFramebuffer.cpp
 * draws a frame into a buffer of its own and this file has the DECON show that buffer from its next refresh on, instead
 * of the frame being drawn into the very buffer the DECON is reading (which tears). Only with boot-arg fb_flip=1 or
 * fb_dpp=1 (read by MiniIOMobileFramebuffer.cpp, which calls in here); with neither, or fps_log=1 alone, the DECON is
 * only read.
 *
 * What the boot loader leaves (Samsung's S-Boot sets the display up; Silicium's UEFI and this kernel only write into
 * the buffer): DECON0 (0x16030000) in MIPI command mode, its frames started by the panel's TE (HW trigger), one window
 * on one DMA channel whose IDMA reads the 1440 x 2960 buffer at 0xCC000000 (the boot framebuffer), through the DPP and
 * DSC to DSIM0 and the S6E3HA8. The DECON reads that buffer once every refresh, while the processor writes the next
 * frame into it: what reaches the panel is part one frame, part the next.
 *
 * Two ways out, both off by default:
 *
 *   fb_flip=1   the same channel, the same scaling on the processor, but into one of three buffers of the panel's size
 *               (the boot framebuffer and two more): when a frame is drawn, its buffer's address is written to the
 *               channel's IDMA_IN_BASE_ADDR and a shadow update asked for (SHADOW_REG_UPDATE_REQ, as Samsung's driver
 *               does after every frame: all windows and the global bit). The DECON takes the new address at the start
 *               of its next frame (the next TE) and clears the request; only then is the next frame flipped, and the
 *               buffer that was shown before is drawn into again only after that. No buffer is ever written while the
 *               DECON reads it, and frames go to the panel at most once a refresh.
 *   fb_dpp=1    the scaling in hardware: the window is moved to the channel VGF0 (IDMA_VGF0, channel 1 in
 *               DATA_PATH_CONTROL_1), one of the two of the six whose DPP has a scaler (DPP_SCALED_IMG_SIZE, VGF0 and
 *               VGF1 only; VGF1 is the one with HDR). It reads the iPhone's 1125 x 2436 picture from three buffers of
 *               that size and scales it to the panel's 1440 x 2960 (8 taps across, 4 down, polyphase, 16 phases); the
 *               processor's part is a copy, not a scaling, and the flips are as above. Before the window is moved, VGF0
 *               is set up from scratch and its registers forced in (SFR_UPDATE_FORCE, it is idle); after, three refreshes
 *               are waited for and the configuration errors of its IDMA and DPP read: any error, or the DECON not taking
 *               the change, and the window goes back to the boot loader's channel and the boot buffer.
 *
 * The scaler's coefficients are not Samsung's: they are Catmull-Rom (a cubic through the four nearest pixels, sharp and
 * without ringing to speak of), worked out here at start for each of the nine phases the registers take (the other
 * seven are the mirror of these: the hardware mirrors them), in units of 1/512, each row summing to 512.
 *
 * Registers and fields (all from Samsung's dpu_9810, read for these facts):
 *   DECON0 0x16030000: GLOBAL_CONTROL 0x000 (bit 8 command mode, bit 4 running), SHADOW_REG_UPDATE_REQ 0x060 (bit 31
 *     global, bits 0-5 the windows; cleared by the hardware when taken), HW_SW_TRIG_CONTROL 0x070 (bit 0 HW trigger,
 *     bit 4 HW trigger masked, bit 8 SW trigger), DATA_PATH_CONTROL_0 0x214 (bit 4w window w on),
 *     DATA_PATH_CONTROL_1 0x218 (bits 4w-4w+2: window w's channel), FRAME_COUNT 0x2A0, WIN_START/END_POSITION
 *     0x1008/0x100C + 0x30 w.
 *   DMA channel numbers (DATA_PATH_CONTROL_1) to IDMA blocks: 0 VG0, 1 VGF0, 2 VGF1, 3 G1, 4 VG1, 5 G0; the blocks
 *     in the order G0, G1, VG0, VG1, VGF0, VGF1 at 0x1000 steps from 0x16071000 (IDMA) and 0x16021000 (DPP).
 *   IDMA: ENABLE 0x000 (bit 4 SFR update force, bit 2 busy), IRQ 0x004 (status bits 21 config error, 19 read slave
 *     error, 17 deadlock), IN_CON 0x008 (bits 11-15 format, 3 block, 6-7 AFBC, 8-10 rotation), OUT_CON 0x00C,
 *     SRC_SIZE 0x010 (width in pixels = the stride, height), SRC_OFFSET 0x014, IMG_SIZE 0x018, IN_BASE_ADDR_Y/C
 *     0x040/0x044 (32 bits: the buffers must be below 4 GB), DEADLOCK_NUM 0x050, BUS_CON 0x054, DYNAMIC_GATING_EN
 *     0x058, IN_REQ_DEST 0x068, CFG_ERR_STATE 0x870 (11 bits).
 *   DPP: ENABLE 0x000 (bit 4 SFR update force), IRQ 0x004 (bit 21 config error), IN_CON 0x008 (bits 0-2 format, 0 is
 *     ARGB8888; bit 3 per-pixel alpha), IMG_SIZE 0x018, SCALED_IMG_SIZE 0x02C, MAIN_H_RATIO/V_RATIO 0x044/0x048
 *     (source / destination in 12.20), the coefficients: vertical (4 taps) at 0x200 + 4 phase + 0x24 tap, horizontal
 *     (8 taps) at 0x290 + 4 phase + 0x24 tap, phases 0-8, 11 bits signed, the luma bank and +0x200 the chroma one (both
 *     written, as Samsung does), the start positions YH, YV, CH, CV 0x5B0-0x5BC ((source - destination) / (2
 *     destination) in 12.20, two's complement), DYNAMIC_GATING_EN 0xA54, CFG_ERR_STATE 0xD08 (5 bits).
 *   Limits (dpp.h, dpp_drv.c): sizes 16-4096, scaling from 1/2 to 8 times; 1125 -> 1440 and 2436 -> 2960 are within.
 *
 * Sources: Samsung's kernel for the S9, github.com/Scion-Project/android_kernel_samsung_exynos9810 (lineage-24.0),
 * drivers/video/fbdev/exynos/dpu_9810: regs-decon.h, regs-dpp.h, decon_reg.c (decon_reg_config_win_channel,
 * decon_reg_set_trigger, decon_reg_wait_for_update_timeout), dpp_reg.c (dpp_reg_set_h_coef/v_coef, the ratio and
 * initial-phase formulas, dpp_reg_set_format), dpp_drv.c (the ratio, dpp_check_scale_ratio), dpp.h, helper.c
 * (dpu_dma_type_to_channel); and the device tree of the same tree (exynos9810-starlte_eur_open_26.dts: decon_f@16030000,
 * dpp@16021000-16026000 with their IDMA at 16071000-16076000). GPL: read for these facts, nothing copied.
 *
 * Seen on the phone (run 33, 2 November): the probe as expected (window 5 on G0, reading 0xCC000000); the buffers were
 * then never had, the presenter asking IOKit for memory below 4 GB where this kernel has none (its RAM is the bank from
 * 0x880000000): hence hakios_scanout_memory. Every step says what it decided, in the loader's log.
 */
#include <stdarg.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <os/atomic_private.h>
#include <sys/systm.h>
#include <sys/hakios.exynos9810_mem.h>
#include <sys/hakios.viewport.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern void IOSleep(unsigned milliseconds);

/* the display driver's side (MiniIOMobileFramebuffer.cpp); stand-ins in drivers/core/hakios.stubs.c */
int hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes);
int hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t *phys);
int hakios_scanout_attach(int mode, const uint64_t *phys, unsigned count, uint32_t width, uint32_t height,
    uint32_t rowbytes, unsigned shown);
int hakios_scanout_flip(unsigned index);
int hakios_scanout_latched(void);
void hakios_scanout_detach(const char *why);
uint32_t hakios_scanout_refreshes(void);
int hakios_scanout_carve(uint64_t bytes, uint64_t *phys);
void hakios_scanout_uncarve(uint64_t phys);
int hakios_scanout_carved(uint64_t phys);
int hakios_scanout_show(uint64_t phys, uint32_t rowbytes);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define DECON_PA                0x16030000UL
#define DPP_PA                  0x16020000UL
#define DMA_PA                  0x16070000UL
#define BLOCK_SIZE              0x7000          /* the common block and the six channels' */

#define GLOBAL_CONTROL          0x000
#define  GC_CMD_MODE            (1u << 8)
#define  GC_RUN                 (1u << 4)
#define SHADOW_REQ              0x060
#define  SHADOW_ALL             (0x80000000u | 0x3fu)
#define TRIG_CONTROL            0x070
#define  HW_TRIG_EN             (1u << 0)
#define  HW_TRIG_MASKED         (1u << 4)
#define  SW_TRIG_EN             (1u << 8)
#define DATA_PATH_0             0x214
#define DATA_PATH_1             0x218
#define FRAME_COUNT             0x2A0
#define WIN_START(w)            (0x1008 + 0x30 * (w))
#define WIN_END(w)              (0x100C + 0x30 * (w))
#define WINDOWS                 6

#define IDMA_ENABLE             0x000
#define  SFR_UPDATE_FORCE       (1u << 4)
#define  OP_BUSY                (1u << 2)
#define IDMA_IRQ                0x004
#define  IDMA_IRQ_ERRORS        ((1u << 21) | (1u << 19) | (1u << 17))
#define IDMA_IN_CON             0x008
#define  IN_CON_KEEP            ((0xffu << 19) | (0x1fu << 11))     /* IC_MAX and the format */
#define  IN_CON_ODD             ((7u << 8) | (3u << 6) | (1u << 3)) /* rotation and flips, AFBC, blocking */
#define IDMA_OUT_CON            0x00C
#define IDMA_SRC_SIZE           0x010
#define IDMA_SRC_OFFSET         0x014
#define IDMA_IMG_SIZE           0x018
#define IDMA_BASE_Y             0x040
#define IDMA_BASE_C             0x044
#define IDMA_DEADLOCK           0x050
#define IDMA_BUS_CON            0x054
#define IDMA_GATING             0x058
#define IDMA_REQ_DEST           0x068
#define IDMA_CFG_ERR            0x870

#define DPP_ENABLE              0x000
#define DPP_IRQ                 0x004
#define  DPP_IRQ_ERRORS         (1u << 21)
#define DPP_IN_CON              0x008
#define  DPP_IN_KEEP            ((1u << 3) | 7u)                    /* per-pixel alpha, the format */
#define DPP_IMG_SIZE            0x018
#define DPP_SCALED_SIZE         0x02C
#define DPP_H_RATIO             0x044
#define DPP_V_RATIO             0x048
#define DPP_V_COEF(n, s, x)     (0x200 + 4 * (n) + 0x24 * (s) + 0x200 * (x))
#define DPP_H_COEF(n, s, x)     (0x290 + 4 * (n) + 0x24 * (s) + 0x200 * (x))
#define DPP_YH_POS              0x5B0
#define DPP_YV_POS              0x5B4
#define DPP_CH_POS              0x5B8
#define DPP_CV_POS              0x5BC
#define DPP_GATING              0xA54
#define DPP_CFG_ERR             0xD08

#define IDMA_VGF0               4               /* the blocks' order: G0 G1 VG0 VG1 VGF0 VGF1 */
#define CHANNEL_VGF0            1

static const int type_of_channel[8] = { 2, 4, 5, 1, 3, 0, -1, -1 };
static const char *const type_name[6] = { "G0", "G1", "VG0", "VG1", "VGF0", "VGF1" };

static vm_offset_t decon, dpp, dma;
static int probed;                      /* 1: the DECON is as expected; -1: it is not, nothing is written */
static unsigned win, boot_type;         /* the boot loader's window and its channel's block */
static uint32_t boot_base, boot_data_path_1;
static uint32_t boot_win_start, boot_win_end;
static bool aspect_window_changed;
static bool aspect_active;
static struct hakios_viewport active_viewport;
static uint32_t aspect_panel_width, aspect_panel_height;

int
hakios_scanout_viewport(struct hakios_viewport *v, uint32_t *pw, uint32_t *ph)
{
	if (!os_atomic_load(&aspect_active, acquire)) return 0;
	*v = active_viewport;
	*pw = aspect_panel_width;
	*ph = aspect_panel_height;
	return 1;
}
static int mode;                        /* 0 the boot loader's, 1 flip on its channel, 2 VGF0's scaler */
static unsigned channel_type;           /* the block whose base address the flips write */
static uint64_t buffers[4];
static unsigned nbuffers;

static inline uint32_t rd(vm_offset_t base, uint32_t offset) { return *(volatile uint32_t *)(base + offset); }
static inline void wr(vm_offset_t base, uint32_t offset, uint32_t value) { *(volatile uint32_t *)(base + offset) = value; }
/*
 * The Galaxy A50 (Exynos 9610, dpu20): DECON_F at 0x148B0000 with the same DECON registers (dpu20/cal_9610/regs-decon.h:
 * SHADOW_REG_UPDATE_REQ 0x60, DATA_PATH_CONTROL_1 0x218, FRAME_COUNT ...) and IDMA registers (regs-dpp.h: IN_BASE_ADDR_Y
 * 0x40, SRC_SIZE 0x10, IMG_SIZE 0x18), but four DMA blocks at other places (the device tree's dpp nodes: 0x14891000,
 * 0x14892000, 0x14896000, 0x14895000). S-Boot's framebuffer 0xCA000000, 1080 x 2340. Only fb_flip here: the block that
 * reads S-Boot's buffer is found by its base address, and flips write that block alone; no scaler, no fb_zc pool (the
 * S9's pool, 0xDEC00000 up, runs past the end of this phone's bank at 0xE0000000).
 */
static bool soc9610;
static vm_offset_t idma9610;
static const uint64_t dma9610_pa[4] = { 0x14891000ULL, 0x14892000ULL, 0x14896000ULL, 0x14895000ULL };
static inline vm_offset_t idma(unsigned type) { return soc9610 ? idma9610 : dma + 0x1000 * (type + 1); }
static inline vm_offset_t dppc(unsigned type) { return dpp + 0x1000 * (type + 1); }

/* the shadow update taken (all of it: the DECON's request register reads 0); up to `ms` milliseconds */
static int
wait_taken(unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if ((rd(decon, SHADOW_REQ) & SHADOW_ALL) == 0) {
			return 0;
		}
		IOSleep(1);
	}
	return -1;
}

static void
dump_channel(const char *what, unsigned type)
{
	vm_offset_t i = idma(type), d = dppc(type);

	if (soc9610) {
		say("exynos_dpu: %s DMA%u: IDMA enable %x irq %x in %x out %x src %x offset %x img %x base %08x\n", what, type,
		    rd(i, IDMA_ENABLE), rd(i, IDMA_IRQ), rd(i, IDMA_IN_CON), rd(i, IDMA_OUT_CON), rd(i, IDMA_SRC_SIZE),
		    rd(i, IDMA_SRC_OFFSET), rd(i, IDMA_IMG_SIZE), rd(i, IDMA_BASE_Y));
		return;
	}
	say("exynos_dpu: %s %s: IDMA enable %x irq %x in %x out %x src %x offset %x img %x base %08x/%08x deadlock %x "
	    "bus %x gating %x dest %x cfg-err %x; DPP enable %x irq %x in %x img %x scaled %x ratio %x/%x cfg-err %x\n",
	    what, type_name[type], rd(i, IDMA_ENABLE), rd(i, IDMA_IRQ), rd(i, IDMA_IN_CON), rd(i, IDMA_OUT_CON),
	    rd(i, IDMA_SRC_SIZE), rd(i, IDMA_SRC_OFFSET), rd(i, IDMA_IMG_SIZE), rd(i, IDMA_BASE_Y), rd(i, IDMA_BASE_C),
	    rd(i, IDMA_DEADLOCK), rd(i, IDMA_BUS_CON), rd(i, IDMA_GATING), rd(i, IDMA_REQ_DEST), rd(i, IDMA_CFG_ERR),
	    rd(d, DPP_ENABLE), rd(d, DPP_IRQ), rd(d, DPP_IN_CON), rd(d, DPP_IMG_SIZE),
	    type >= IDMA_VGF0 ? rd(d, DPP_SCALED_SIZE) : 0, type >= 2 ? rd(d, DPP_H_RATIO) : 0,
	    type >= 2 ? rd(d, DPP_V_RATIO) : 0, rd(d, DPP_CFG_ERR));
}

/* the A50's: as below, with the DMA block found by the buffer it reads */
static int
probe9610(uint64_t fb_phys, uint32_t width, uint32_t height)
{
	uint32_t gc, path0, path1, on = 0;
	int found = -1;

	soc9610 = true;
	decon = ml_io_map(0x148B0000UL, 0x2000);
	if (decon == 0) {
		say("exynos_dpu: 9610: cannot map DECON_F\n");
		return -1;
	}
	gc = rd(decon, GLOBAL_CONTROL);
	path0 = rd(decon, DATA_PATH_0);
	path1 = rd(decon, DATA_PATH_1);
	for (unsigned w = 0; w < WINDOWS; w++) {
		if (path0 & (1u << (4 * w))) {
			on++;
			win = w;
		}
	}
	say("exynos_dpu: 9610: DECON_F control %x (%s mode, %s), trigger %x, shadow request %x, windows %x (%u on), "
	    "channels %x, frame count %u\n", gc, (gc & GC_CMD_MODE) ? "command" : "video", (gc & GC_RUN) ? "running" :
	    "NOT running", rd(decon, TRIG_CONTROL), rd(decon, SHADOW_REQ), path0, on, path1, rd(decon, FRAME_COUNT));
	if (on != 1 || !(gc & GC_CMD_MODE) || !(gc & GC_RUN)) {
		say("exynos_dpu: 9610: not one window in command mode, running: the DECON is left alone\n");
		return -1;
	}
	for (unsigned k = 0; k < 4; k++) {
		vm_offset_t m = ml_io_map(dma9610_pa[k], 0x1000);
		uint32_t base, img;

		if (m == 0) {
			continue;
		}
		base = rd(m, IDMA_BASE_Y);
		img = rd(m, IDMA_IMG_SIZE);
		say("exynos_dpu: 9610: DMA%u at %#llx: enable %x base %08x img %x src %x\n", k, dma9610_pa[k],
		    rd(m, IDMA_ENABLE), base, img, rd(m, IDMA_SRC_SIZE));
		/* (on the A50 IN_BASE_ADDR_Y and SRC_SIZE read 0 though the block scans S-Boot's buffer: matched by its size) */
		if (found < 0 && (base == (uint32_t)fb_phys || base == 0) && img == (height << 16 | width)) {
			found = (int)k;
			idma9610 = m;
		}
	}
	if (found < 0) {
		say("exynos_dpu: 9610: no DMA block reads S-Boot's buffer: the DECON is left alone\n");
		return -1;
	}
	{
		/* the block's first 0x100 bytes and a write of IN_BASE_ADDR_Y read back (the same address: nothing changes) */
		uint32_t w[64];

		for (unsigned k = 0; k < 64; k++) {
			w[k] = rd(idma9610, 4 * k);
		}
		for (unsigned k = 0; k < 64; k += 8) {
			say("exynos_dpu: 9610: DMA%d +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", found, 4 * k, w[k], w[k + 1],
			    w[k + 2], w[k + 3], w[k + 4], w[k + 5], w[k + 6], w[k + 7]);
		}
		wr(idma9610, IDMA_BASE_Y, (uint32_t)fb_phys);
		__asm__ volatile ("dsb sy" ::: "memory");
		say("exynos_dpu: 9610: IN_BASE_ADDR_Y <- %#llx: reads %08x; SRC_SIZE %08x\n", fb_phys, rd(idma9610, IDMA_BASE_Y),
		    rd(idma9610, IDMA_SRC_SIZE));
	}
	boot_type = channel_type = (unsigned)found;
	boot_base = (uint32_t)fb_phys;
	boot_data_path_1 = path1;
	boot_win_start = rd(decon, WIN_START(win));
	boot_win_end = rd(decon, WIN_END(win));
	probed = 1;
	say("exynos_dpu: 9610: probe: window %u on DMA%u reading %#llx: fb_flip may be asked for (no scaler, no pool)\n",
	    win, boot_type, fb_phys);
	return 0;
}

/*
 * Is the display what this file expects: the boot framebuffer at 0xCC000000, 1440 x 2960, and a DECON0 running in
 * command mode with one window on, whose channel reads that buffer at that size? Reads only. 0 if it is.
 */
int
hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes)
{
	uint32_t gc, trig, path0, path1, frames, base, img, on = 0;
	int type;

	if (probed != 0) {
		return probed > 0 ? 0 : -1;
	}
	probed = -1;
	if (fb_phys == 0xCA000000ULL && width == 1080 && height == 2340 && rowbytes == 1080 * 4) {
		return probe9610(fb_phys, width, height);
	}
	if (fb_phys != 0xCC000000ULL || width != 1440 || height != 2960 || rowbytes != 1440 * 4) {
		say("exynos_dpu: the boot framebuffer is at %#llx, %ux%u, %u bytes a row: not the S9's (0xcc000000, "
		    "1440x2960): the DECON is left alone\n", fb_phys, width, height, rowbytes);
		return -1;
	}
	decon = ml_io_map(DECON_PA, 0x2000);
	dpp = ml_io_map(DPP_PA, BLOCK_SIZE);
	dma = ml_io_map(DMA_PA, BLOCK_SIZE);
	if (decon == 0 || dpp == 0 || dma == 0) {
		say("exynos_dpu: cannot map the DECON, the DPP or the DMA\n");
		return -1;
	}
	gc = rd(decon, GLOBAL_CONTROL);
	trig = rd(decon, TRIG_CONTROL);
	path0 = rd(decon, DATA_PATH_0);
	path1 = rd(decon, DATA_PATH_1);
	frames = rd(decon, FRAME_COUNT);
	for (unsigned w = 0; w < WINDOWS; w++) {
		if (path0 & (1u << (4 * w))) {
			on++;
			win = w;
		}
	}
	say("exynos_dpu: DECON0: control %x (%s mode, %s), trigger %x (HW %s%s, SW %s), shadow request %x, windows %x "
	    "(%u on), channels %x, frame count %u\n", gc, (gc & GC_CMD_MODE) ? "command" : "video",
	    (gc & GC_RUN) ? "running" : "NOT running", trig, (trig & HW_TRIG_EN) ? "on" : "off",
	    (trig & HW_TRIG_MASKED) ? " but masked" : "", (trig & SW_TRIG_EN) ? "on" : "off", rd(decon, SHADOW_REQ),
	    path0, on, path1, frames);
	if (on != 1) {
		say("exynos_dpu: %u windows on, not one: the DECON is left alone\n", on);
		return -1;
	}
	type = type_of_channel[(path1 >> (4 * win)) & 7];
	if (type < 0) {
		say("exynos_dpu: window %u is on channel %u, which is no IDMA's: the DECON is left alone\n", win,
		    (path1 >> (4 * win)) & 7);
		return -1;
	}
	boot_type = (unsigned)type;
	say("exynos_dpu: window %u: %x to %x, on channel %u (%s)\n", win, rd(decon, WIN_START(win)),
	    rd(decon, WIN_END(win)), (path1 >> (4 * win)) & 7, type_name[boot_type]);
	dump_channel("the boot loader's", boot_type);
	base = rd(idma(boot_type), IDMA_BASE_Y);
	img = rd(idma(boot_type), IDMA_IMG_SIZE);
	if (base != (uint32_t)fb_phys || img != (height << 16 | width)) {
		say("exynos_dpu: the channel reads %08x, %ux%u, not the boot framebuffer: the DECON is left alone\n", base,
		    img & 0x1fff, img >> 16 & 0x1fff);
		return -1;
	}
	if (!(gc & GC_CMD_MODE) || !(gc & GC_RUN)) {
		say("exynos_dpu: the DECON is not running in command mode: left alone\n");
		return -1;
	}
	boot_base = base;
	boot_data_path_1 = path1;
	boot_win_start = rd(decon, WIN_START(win));
	boot_win_end = rd(decon, WIN_END(win));
	channel_type = boot_type;
	probed = 1;
	say("exynos_dpu: probe: as expected (window %u on %s reading the boot framebuffer, command mode, running; the "
	    "boot channel's IDMA enable reads %x, not checked): flips and the scaler may be asked for\n", win,
	    type_name[boot_type], rd(idma(boot_type), IDMA_ENABLE));
	/* (sys/hakios.exynos9810_mem.h; SCAN_SLOTS below is the same count) */
	say("exynos_dpu: memory: scan-out buffers %#llx-%#llx, fb_zc's pool %#llx-%#llx (%llu slots of 12 MB)\n",
	    EXYNOS9810_MEM_DPU_SCAN_PA, EXYNOS9810_MEM_DPU_SCAN_END - 1, EXYNOS9810_MEM_DPU_POOL_PA,
	    EXYNOS9810_MEM_DPU_POOL_END - 1, EXYNOS9810_MEM_SIZE(DPU_POOL) / (12ULL << 20));
	return 0;
}

/*
 * Where the scan-out's buffers can be: the DECON's DMA takes 32-bit addresses, and none of this kernel's own memory is
 * below 4 GB on the S9 (the loader gives XNU the bank from 0x880000000 on; IOKit's "below 4 GB" never succeeds there).
 * In the lower bank, the gap of the phone's device tree between the moved payload and panic log (0xD0000000 -
 * 0xD3FFFFFF, kernel PR 144) and secure_camera (0xE1900000): no reserved-memory node, outside XNU's RAM, used by
 * neither payload layout (the memory table of hakios-starlte's README, starlte PR 27). Not 0xF0000000 and above: the
 * Shannon modem's region (modem_if, cp_ram_logging), then ABOX's. `count` buffers of `bytes` each from 0xD4000000,
 * 64 KB apart at least; physical addresses, the caller maps them (write-combined). 0 if they fit.
 */
#define SCAN_MEMORY_PA          EXYNOS9810_MEM_DPU_SCAN_PA      /* 0xD4000000 (sys/hakios.exynos9810_mem.h) */
#define SCAN_MEMORY_END         EXYNOS9810_MEM_DPU_SCAN_END     /* 0xD7000000 */
/*
 * The buffers above take 34 MB at most (fb_flip's of the panel's size) of their 48 MB. fb_zc has a pool of its own, the
 * top of the gap above the camera's, UFS's and Wi-Fi's memory (sys/hakios.exynos9810_mem.h): the window server's own
 * display surfaces are made of it (MiniIOSurface.cpp), so that VGF0 scans out the very surface CoreAnimation's frame was
 * drawn into, and nothing is copied (MiniIOMobileFramebuffer.cpp). Slots of 12 MB, one surface each: the iPhone's
 * 1125 x 2436 at 4544 or 4608 bytes a row is 11 MB. Three slots; CoreAnimation keeps two display surfaces.
 */
#define SCAN_POOL_PA            EXYNOS9810_MEM_DPU_POOL_PA      /* 0xDEC00000, above the camera's, UFS's and Wi-Fi's */
#define SCAN_POOL_END           EXYNOS9810_MEM_DPU_POOL_END     /* 0xE1900000, secure_camera */
#define SCAN_SLOT               (12ULL << 20)
#define SCAN_SLOTS              ((unsigned)((SCAN_POOL_END - SCAN_POOL_PA) / SCAN_SLOT))
_Static_assert(SCAN_SLOTS >= 2 && SCAN_SLOTS <= 32, "exynos_dpu: the pool holds CoreAnimation's two surfaces, and pool_used's bits");
static uint32_t pool_used;              /* a bit for every slot given out (SCAN_SLOTS <= 32) */
static uint32_t scan_rowbytes;          /* the buffers' row length as attached: a flip back to them restores it */
static uint32_t shown_rowbytes;         /* what the IDMA reads now */

int
hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t *phys)
{
	uint64_t step = (bytes + 0xFFFFULL) & ~0xFFFFULL;

	if (probed <= 0) {
		say("exynos_dpu: memory: not the S9's DECON as expected: no buffers of its own\n");
		return -1;
	}
	if (SCAN_MEMORY_PA + step * count > SCAN_MEMORY_END) {
		say("exynos_dpu: memory: %u buffers of %llu bytes do not fit between %#llx and %#llx\n", count, bytes,
		    SCAN_MEMORY_PA, SCAN_MEMORY_END);
		return -1;
	}
	for (unsigned k = 0; k < count; k++) {
		phys[k] = SCAN_MEMORY_PA + step * k;
	}
	say("exynos_dpu: memory: %u buffers of %llu bytes at %#llx to %#llx (of %#llx to %#llx)\n", count, bytes,
	    SCAN_MEMORY_PA, SCAN_MEMORY_PA + step * count, SCAN_MEMORY_PA, SCAN_MEMORY_END);
	return 0;
}

/*
 * fb_zc: a slot of the pool for a surface of `bytes` (physically contiguous, below 4 GB, outside XNU's RAM: the caller
 * maps it write-combined). 0 and its address in `phys`; -1 when the DPU is not the S9's or no slot is free. Called by
 * MiniIOSurface with its own lock held: nothing here sleeps.
 */
int
hakios_scanout_carve(uint64_t bytes, uint64_t *phys)
{
	if (probed <= 0 || soc9610 || bytes == 0 || bytes > SCAN_SLOT) {
		return -1;
	}
	for (unsigned k = 0; k < SCAN_SLOTS; k++) {
		uint32_t used = __atomic_load_n(&pool_used, __ATOMIC_RELAXED);

		while ((used & (1u << k)) == 0) {
			if (__atomic_compare_exchange_n(&pool_used, &used, used | (1u << k), 0, __ATOMIC_SEQ_CST,
			    __ATOMIC_SEQ_CST)) {
				*phys = SCAN_POOL_PA + SCAN_SLOT * k;
				return 0;
			}
		}
	}
	return -1;
}

void
hakios_scanout_uncarve(uint64_t phys)
{
	if (hakios_scanout_carved(phys)) {
		unsigned k = (unsigned)((phys - SCAN_POOL_PA) / SCAN_SLOT);

		__atomic_fetch_and(&pool_used, ~(1u << k), __ATOMIC_SEQ_CST);
	}
}

/* Is `phys` in the pool? (The Mali's tables take it, though the kernel does not manage it: drivers/gpu/hakios.mali.c.) */
int
hakios_scanout_carved(uint64_t phys)
{
	return probed > 0 && !soc9610 && phys >= SCAN_POOL_PA && phys < SCAN_POOL_PA + SCAN_SLOT * SCAN_SLOTS;
}

/*
 * fb_zc: VGF0 to scan out a surface of the pool from the next refresh on, `rowbytes` a row (its size stays the
 * display's, as attached). As hakios_scanout_flip(): the caller has waited for the flip before to be taken.
 */
int
hakios_scanout_show(uint64_t phys, uint32_t rowbytes)
{
	vm_offset_t i;

	if (mode != 2 || (phys & 0xff) || (rowbytes & 3) || !hakios_scanout_carved(phys) ||
	    rowbytes / 4 < (rd(idma(channel_type), IDMA_IMG_SIZE) & 0x3fff) || rowbytes / 4 > 0xffff) {
		return -1;
	}
	i = idma(channel_type);
	if (rowbytes != shown_rowbytes) {
		wr(i, IDMA_SRC_SIZE, (rd(i, IDMA_SRC_SIZE) & 0xffff0000u) | (rowbytes / 4));
		shown_rowbytes = rowbytes;
	}
	wr(i, IDMA_BASE_Y, (uint32_t)phys);
	wr(i, IDMA_BASE_C, (uint32_t)phys);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	return 0;
}

uint32_t
hakios_scanout_refreshes(void)
{
	return probed > 0 ? rd(decon, FRAME_COUNT) : 0;
}

int
hakios_scanout_latched(void)
{
	return probed > 0 && (rd(decon, SHADOW_REQ) & SHADOW_ALL) == 0;
}

int
hakios_scanout_flip(unsigned index)
{
	vm_offset_t i;

	if (mode == 0 || index >= nbuffers) {
		return -1;
	}
	i = idma(channel_type);
	if (mode == 2 && shown_rowbytes != scan_rowbytes) {
		/* (back from a surface of fb_zc's, whose rows may be shorter) */
		wr(i, IDMA_SRC_SIZE, (rd(i, IDMA_SRC_SIZE) & 0xffff0000u) | (scan_rowbytes / 4));
		shown_rowbytes = scan_rowbytes;
	}
	wr(i, IDMA_BASE_Y, (uint32_t)buffers[index]);
	wr(i, IDMA_BASE_C, (uint32_t)buffers[index]);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	return 0;
}

/* Catmull-Rom's weight of the pixel `k` (-1 .. 2) for a point `t` / 16 past pixel 0, in 1/512 */
static int
cubic(int k, int t)
{
	/* in 1/(2 * 16^3) units: w(x) for |x| <= 1: 3|x|^3 - 5|x|^2 + 2; 1 < |x| < 2: -|x|^3 + 5|x|^2 - 8|x| + 4 */
	int x = k * 16 - t, a = x < 0 ? -x : x;
	int64_t w;

	if (a <= 16) {
		w = 3LL * a * a * a - 5LL * 16 * a * a + 2LL * 16 * 16 * 16;
	} else if (a < 32) {
		w = -1LL * a * a * a + 5LL * 16 * a * a - 8LL * 16 * 16 * a + 4LL * 16 * 16 * 16;
	} else {
		w = 0;
	}
	/* (w / (2 * 4096)) * 512, rounded */
	return (int)((w * 512 + (w >= 0 ? 4096 : -4096)) / 8192);
}

/* the nine phases of the four taps (pixels -1 .. 2), each row 512 */
static void
coefficients(int16_t table[9][4])
{
	for (int t = 0; t < 9; t++) {
		int sum = 0;

		for (int k = 0; k < 4; k++) {
			table[t][k] = (int16_t)cubic(k - 1, t);
			sum += table[t][k];
		}
		/* what rounding left over goes to the nearer of the middle two */
		table[t][t < 8 ? 1 : 2] += (int16_t)(512 - sum);
	}
}

static void
setup_scaler(vm_offset_t d, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
	int16_t c[9][4];

	coefficients(c);
	for (unsigned x = 0; x < 2; x++) {
		for (unsigned n = 0; n < 9; n++) {
			/* 8 taps across: pixel 0 is tap 3, so the four are taps 2 to 5; 4 down: pixel 0 is tap 1 */
			for (unsigned s = 0; s < 8; s++) {
				int v = s >= 2 && s <= 5 ? c[n][s - 2] : 0;

				wr(d, DPP_H_COEF(n, s, x), (uint32_t)v & 0x7ff);
			}
			for (unsigned s = 0; s < 4; s++) {
				wr(d, DPP_V_COEF(n, s, x), (uint32_t)c[n][s] & 0x7ff);
			}
		}
	}
	wr(d, DPP_H_RATIO, (uint32_t)(((uint64_t)sw << 20) / dw));
	wr(d, DPP_V_RATIO, (uint32_t)(((uint64_t)sh << 20) / dh));
	wr(d, DPP_YH_POS, (uint32_t)(int32_t)((((int64_t)sw - dw) << 20) / (2 * (int64_t)dw)));
	wr(d, DPP_YV_POS, (uint32_t)(int32_t)((((int64_t)sh - dh) << 20) / (2 * (int64_t)dh)));
	wr(d, DPP_CH_POS, (uint32_t)(int32_t)((((int64_t)sw - dw) << 20) / (2 * (int64_t)dw)));
	wr(d, DPP_CV_POS, (uint32_t)(int32_t)((((int64_t)sh - dh) << 20) / (2 * (int64_t)dh)));
	say("exynos_dpu: VGF0 scaler: %ux%u to %ux%u, ratio %x/%x, phases %d/%d, Catmull-Rom taps at half a pixel "
	    "%d %d %d %d\n", sw, sh, dw, dh, rd(d, DPP_H_RATIO), rd(d, DPP_V_RATIO), (int32_t)rd(d, DPP_YH_POS),
	    (int32_t)rd(d, DPP_YV_POS), c[8][0], c[8][1], c[8][2], c[8][3]);
}

/* back to the boot loader's channel and buffer */
void
hakios_scanout_detach(const char *why)
{
	if (probed <= 0 || mode == 0) {
		return;
	}
	say("exynos_dpu: back to the boot loader's window (%s)\n", why);
	if (mode == 2) {
		os_atomic_store(&aspect_active, false, release);
		wr(decon, DATA_PATH_1, boot_data_path_1);
		if (aspect_window_changed) {
			wr(decon, WIN_START(win), boot_win_start);
			wr(decon, WIN_END(win), boot_win_end);
			aspect_window_changed = false;
		}
	}
	wr(idma(boot_type), IDMA_BASE_Y, boot_base);
	wr(idma(boot_type), IDMA_BASE_C, boot_base);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	if (wait_taken(100) != 0) {
		say("exynos_dpu: the DECON has not taken it after 100 ms (shadow request %x)\n", rd(decon, SHADOW_REQ));
	}
	mode = 0;
	channel_type = boot_type;
}

/* VGF0 set up to read `phys` (width x height, rowbytes a row) and scale it to the boot window's size; then the window moved */
static int
attach_vgf0(uint64_t phys, uint32_t width, uint32_t height, uint32_t rowbytes)
{
	vm_offset_t bi = idma(boot_type), bd = dppc(boot_type), i = idma(IDMA_VGF0), d = dppc(IDMA_VGF0);
	uint32_t path1 = boot_data_path_1, in_con = rd(bi, IDMA_IN_CON), dpp_in = rd(bd, DPP_IN_CON);
	uint32_t pw = rd(decon, WIN_END(win)) & 0x3fff, ph = rd(decon, WIN_END(win)) >> 16 & 0x3fff;
	uint32_t frames, errors_i, errors_d, irq_i, irq_d;
	uint32_t keep_aspect = 0;
	struct hakios_viewport viewport;

	pw += 1;
	ph += 1;
	PE_parse_boot_argn("fb_aspect", &keep_aspect, sizeof(keep_aspect));
	viewport = (struct hakios_viewport){ 0, 0, pw, ph };
	if (keep_aspect && (boot_win_start != 0 || !hakios_viewport_fit(width, height, pw, ph, &viewport))) {
		say("exynos_dpu: aspect viewport cannot fit the boot window: not taken\n");
		return -1;
	}
	for (unsigned w = 0; w < WINDOWS; w++) {
		if (((path1 >> (4 * w)) & 7) == CHANNEL_VGF0 && (rd(decon, DATA_PATH_0) & (1u << (4 * w)))) {
			say("exynos_dpu: VGF0 is window %u's already: not taken\n", w);
			return -1;
		}
	}
	if (boot_type == IDMA_VGF0 || (in_con & IN_CON_ODD) || (dpp_in & 7) != 0 || (rowbytes & 3) || rowbytes / 4 < width ||
	    width < 16 || height < 16 || pw > 4096 || ph > 4096 || width > 2 * pw || height > 2 * ph ||
	    ph > 8 * height || pw > 8 * width || phys >> 32 || (phys & 0xff)) {
		say("exynos_dpu: VGF0 cannot take it: in %x, DPP in %x, %ux%u (%u bytes a row) at %#llx to %ux%u\n", in_con,
		    dpp_in, width, height, rowbytes, phys, pw, ph);
		return -1;
	}
	if (rd(i, IDMA_ENABLE) & OP_BUSY) {
		say("exynos_dpu: VGF0's IDMA is busy: not taken\n");
		return -1;
	}
	dump_channel("before,", IDMA_VGF0);
	/* the IDMA: the boot channel's format and bus settings, our buffer and sizes, nothing else of its */
	wr(i, IDMA_IN_CON, in_con & IN_CON_KEEP);
	wr(i, IDMA_OUT_CON, rd(bi, IDMA_OUT_CON));
	wr(i, IDMA_SRC_SIZE, height << 16 | (rowbytes / 4));
	wr(i, IDMA_SRC_OFFSET, 0);
	wr(i, IDMA_IMG_SIZE, height << 16 | width);
	wr(i, IDMA_BASE_Y, (uint32_t)phys);
	wr(i, IDMA_BASE_C, (uint32_t)phys);
	wr(i, IDMA_DEADLOCK, rd(bi, IDMA_DEADLOCK));
	wr(i, IDMA_BUS_CON, rd(bi, IDMA_BUS_CON));
	wr(i, IDMA_GATING, rd(bi, IDMA_GATING));
	wr(i, IDMA_REQ_DEST, rd(bi, IDMA_REQ_DEST));
	/* the DPP: ARGB as the boot channel's, the sizes, the scaler */
	wr(d, DPP_IN_CON, (rd(d, DPP_IN_CON) & ~DPP_IN_KEEP) | (dpp_in & DPP_IN_KEEP));
	wr(d, DPP_IMG_SIZE, height << 16 | width);
	wr(d, DPP_SCALED_SIZE, viewport.height << 16 | viewport.width);
	setup_scaler(d, width, height, viewport.width, viewport.height);
	wr(d, DPP_GATING, rd(bd, DPP_GATING));
	/* VGF0 is idle: its registers taken now, not at a frame of its own */
	wr(i, IDMA_ENABLE, rd(i, IDMA_ENABLE) | SFR_UPDATE_FORCE);
	wr(d, DPP_ENABLE, rd(d, DPP_ENABLE) | SFR_UPDATE_FORCE);
	dump_channel("set up,", IDMA_VGF0);
	if (wait_taken(100) != 0) {
		say("exynos_dpu: a shadow update is still pending (%x): not taken\n", rd(decon, SHADOW_REQ));
		return -1;
	}
	/* the window to VGF0 */
	frames = rd(decon, FRAME_COUNT);
	mode = 2;
	channel_type = IDMA_VGF0;
	if (keep_aspect) {
		active_viewport = viewport;
		aspect_panel_width = pw;
		aspect_panel_height = ph;
		aspect_window_changed = true;
		wr(decon, WIN_START(win), viewport.y << 16 | viewport.x);
		wr(decon, WIN_END(win), (viewport.y + viewport.height - 1) << 16 | (viewport.x + viewport.width - 1));
		say("exynos_dpu: aspect viewport %ux%u at %u,%u on %ux%u\n", viewport.width, viewport.height,
		    viewport.x, viewport.y, pw, ph);
	}
	wr(decon, DATA_PATH_1, (path1 & ~(7u << (4 * win))) | (uint32_t)CHANNEL_VGF0 << (4 * win));
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_ALL);
	if (wait_taken(100) != 0) {
		hakios_scanout_detach("the DECON did not take the window's new channel in 100 ms");
		return -1;
	}
	/* three refreshes on it, then what VGF0 says of them */
	for (unsigned ms = 0; ms < 100 && rd(decon, FRAME_COUNT) - frames < 3; ms++) {
		IOSleep(1);
	}
	errors_i = rd(i, IDMA_CFG_ERR) & 0x7ff;
	errors_d = rd(d, DPP_CFG_ERR) & 0x1f;
	irq_i = rd(i, IDMA_IRQ) & IDMA_IRQ_ERRORS;
	irq_d = rd(d, DPP_IRQ) & DPP_IRQ_ERRORS;
	dump_channel("after the switch,", IDMA_VGF0);
	say("exynos_dpu: VGF0 after %u refreshes: IDMA configuration errors %x, DPP's %x, IDMA error status %x, DPP's %x\n",
	    rd(decon, FRAME_COUNT) - frames, errors_i, errors_d, irq_i, irq_d);
	if (errors_i || errors_d || irq_i || irq_d) {
		hakios_scanout_detach("VGF0 reports errors");
		return -1;
	}
	os_atomic_store(&aspect_active, keep_aspect != 0, release);
	return 0;
}

/*
 * The display driver hands over its buffers (physical addresses; `count` of them, `shown` the one already drawn) and
 * asks for mode 1 (flips on the boot loader's channel; buffers of the panel's size) or 2 (VGF0 scales). 0 if taken.
 */
int
hakios_scanout_attach(int want, const uint64_t *phys, unsigned count, uint32_t width, uint32_t height, uint32_t rowbytes,
    unsigned shown)
{
	say("exynos_dpu: attach: %s asked for, %u buffers of %ux%u (%u bytes a row), the first at %#llx, %u drawn\n",
	    want == 2 ? "VGF0's scaler (fb_dpp)" : want == 1 ? "flips (fb_flip)" : "nothing", count, width, height, rowbytes,
	    count ? phys[0] : 0, shown);
	if (probed <= 0 || mode != 0 || count < 2 || count > 4 || shown >= count) {
		say("exynos_dpu: attach: refused (probe %d, mode %d, %u buffers, buffer %u drawn)\n", probed, mode, count, shown);
		return -1;
	}
	for (unsigned k = 0; k < count; k++) {
		if (phys[k] >> 32 || (phys[k] & 0xff)) {
			say("exynos_dpu: buffer %u at %#llx: the IDMA takes 32-bit addresses (aligned): not taken\n", k, phys[k]);
			return -1;
		}
		buffers[k] = phys[k];
	}
	nbuffers = count;
	if (want == 1) {
		/* (the A50's SRC_SIZE reads 0: its rows are taken as the boot framebuffer's, width x 4) */
		if (width != (rd(idma(boot_type), IDMA_IMG_SIZE) & 0x1fff) || rowbytes != (soc9610 ? width * 4 :
		    (rd(idma(boot_type), IDMA_SRC_SIZE) & 0xffff) * 4)) {
			say("exynos_dpu: flips need buffers like the boot one (%u wide, %u bytes a row; given %u, %u)\n",
			    rd(idma(boot_type), IDMA_IMG_SIZE) & 0x1fff, (rd(idma(boot_type), IDMA_SRC_SIZE) & 0xffff) * 4, width,
			    rowbytes);
			return -1;
		}
		mode = 1;
		channel_type = boot_type;
		say("exynos_dpu: flips on %s%u between %u buffers (%08llx ...), each taken at a refresh\n",
		    soc9610 ? "DMA" : type_name[boot_type], soc9610 ? boot_type : 0, count, phys[0]);
		if (buffers[shown] != boot_base) {
			hakios_scanout_flip(shown);
		}
		return 0;
	}
	if (want == 2 && !soc9610 && attach_vgf0(phys[shown], width, height, rowbytes) == 0) {
		scan_rowbytes = shown_rowbytes = rowbytes;
		say("exynos_dpu: VGF0 scales %ux%u to the panel; flips between %u buffers\n", width, height, count);
		return 0;
	}
	say("exynos_dpu: attach: %s not taken: the boot loader's window and buffer stay\n",
	    want == 2 ? "VGF0's scaler" : "this mode");
	nbuffers = 0;
	return -1;
}
