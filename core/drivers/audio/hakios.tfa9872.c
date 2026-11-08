/*
 * The Galaxy A50's speaker amplifier: an NXP TFA9872 (I2C 0x34) on I2C_CMGP00 (hsi2c@11D10000, alias hsi2c3; not a USI's
 * I2C but a block of CMGP's own), fed by ABOX's UAIF2 over I2S (hakios.exynos_abox.c with abox_soc=9610). Boot-arg
 * tfa9872=N:
 *   1  the bus mapped, its clocks and pins set; the chip's revision read (register 0x03)
 *   2  ... reset (I2CR), Samsung's/NXP's register patch for the revision (tfa_init.c tfa9872_specific), the device list
 *      of the phone's Tfa9872.cnt (TDM 2 slots x 16 bits, speaker data in slot 0, the gain TDMSPKG 6, the DC-DC's
 *      settings), left powered down
 *   3  ... handed to ABOX (hakios_abox_set_amp): UAIF2's start runs the "music" profile (48 kHz, AMPE and DCA on,
 *      powered up, MANSCONF: the PLL starts on BCLK), its stop the profile's stop list, then powered down
 *
 * Where the values come from: the A50 kernel's sound/soc/codecs/tfa9872 (GPL-2.0, read for facts, not copied):
 * tfa9872_tfafieldnames.h (each field's register, bit and width), tfa_dsp.c tfa_run_startup()/tfa98xx_init()/
 * tfa98xx_key2(), tfa_init.c tfa9872_specific(); the phone's Tfa9872.cnt (vendor/firmware), whose device list and
 * "music" profile are written here as they stand. The cnt's .msg/.vstep files are messages to Samsung's audio DSP
 * (calliope: SpeakerBoost, the speaker's protection), which does not run: hakios.guard.c holds the samples back instead,
 * and the gain stays at the cnt's (TDMSPKG 6), never more.
 */
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <sys/hakios.exynos_abox.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_line(const char *line);
extern void delay(int usec);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define TFA_ADDR                0x34

/* CMGP's CMU (cmucal-sfr.c: SFR_BLOCK(CMGP, 0x11c00000)) and I2C_CMGP00's clocks in it */
#define CMGP_CMU_PA             0x11C00000u
#define CMGP_MUX_I2C            0x1004          /* [0]: 1 CLKCMU_CMGP_BUS (cmucal-vclklut.c blk_cmgp: 1) */
#define CMGP_DIV_I2C            0x1804          /* [3:0]: 1, /2 (blk_cmgp: 1): the 200 MHz of the device tree's default-clk */
/* CMGP's GPIO (pinctrl@11C20000): SCL gpm2-0, SDA gpm3-0, function 3, no pull (the device tree's hsi2c3-bus) */
#define CMGP_GPIO_PA            0x11C20000u
#define GPM2                    0x40
#define GPM3                    0x60

static struct hakios_hsi2c bus = {
	.name = "tfa9872",
	.regs_pa = 0x11D10000,                  /* hsi2c@11D10000: I2C_CMGP00 */
	.usi_mode_pa = 0,                       /* none: not a USI */
	.cmu_pa = CMGP_CMU_PA, .cmu_size = 0x4000,
	.gates = { 0x2018, 0x201C, 0x2044 }, .ngates = 3,       /* I2C_CMGP00 IPCLK, PCLK; RSTNSYNC_CLK_CMGP_I2C */
	.qch = 0x3014, .div = CMGP_DIV_I2C, .mux = CMGP_MUX_I2C,
	.pins_pa = 0,                           /* two banks: set here, below */
	.ipclk_mhz = 200, .bus_hz = 400000,
};

static LCK_GRP_DECLARE(tfa9872_lock_group, "tfa9872");
static LCK_MTX_DECLARE(amp_lock_storage, &tfa9872_lock_group);
#define amp_lock (&amp_lock_storage)
static uint16_t revision;
static bool ready;              /* step 2 done: the chip is set up and powered down */
static bool running;

/* ---- registers: 16 bits, big-endian, at 8-bit addresses ---- */

static uint32_t
reg_read(uint8_t reg, uint16_t *value)
{
	uint8_t buf[2] = { 0, 0 };
	uint32_t error = 1;

	for (int tries = 0; tries < 3 && error != 0; tries++) {
		error = hakios_hsi2c_read_reg(&bus, TFA_ADDR, reg, buf, 2);
	}
	*value = (uint16_t)(buf[0] << 8 | buf[1]);
	return error;
}

static uint32_t
reg_write(uint8_t reg, uint16_t value)
{
	uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)value };
	uint32_t error = 1;

	for (int tries = 0; tries < 3 && error != 0; tries++) {
		error = hakios_hsi2c_message(&bus, TFA_ADDR, false, buf, sizeof(buf), true);
	}
	if (error != 0) {
		say("tfa9872: writing %#04x to register %#02x failed (%#x)\n", value, reg, error);
	}
	return error;
}

/* a field as NXP encodes it: register << 8 | bit << 4 | (width - 1) */
static uint32_t
field_set(uint16_t bf, uint16_t value)
{
	uint8_t reg = (uint8_t)(bf >> 8), pos = (bf >> 4) & 0xF, len = (bf & 0xF) + 1;
	uint16_t mask = (uint16_t)(((1u << len) - 1) << pos), now;
	uint32_t error = reg_read(reg, &now);

	if (error != 0) {
		return error;
	}
	return reg_write(reg, (uint16_t)((now & ~mask) | ((value << pos) & mask)));
}

static uint16_t
field_get(uint16_t bf)
{
	uint8_t reg = (uint8_t)(bf >> 8), pos = (bf >> 4) & 0xF, len = (bf & 0xF) + 1;
	uint16_t now = 0;

	(void)reg_read(reg, &now);
	return (uint16_t)((now >> pos) & ((1u << len) - 1));
}

#define BF_PWDN                 0x0000
#define BF_I2CR                 0x0010
#define BF_AMPE                 0x0030
#define BF_DCA                  0x0040
#define BF_MANSCONF             0x0120
#define BF_MANAOOSC             0x0140
#define BF_MANSTATE             0x1433
#define BF_MTPK                 0xA107
#define REG_STATUS              0x10            /* VDDS 0, PLLS 1, OTDS 2, OVDS 3, UVDS 4, CLKS 5, NOCLK 7, SWS 10, AMPS 12, AREFS 13 */
#define REG_REV                 0x03

struct bf_value {
	uint16_t bf, value;
};

/* Tfa9872.cnt's device list, in its order (the phone's: TDM, the source map, the DC-DC, the gain) */
static const struct bf_value device_list[] = {
	{ 0x20c3, 0 },  /* TDMNBCK */
	{ 0x2144, 15 }, /* TDMSLLN: 16-bit slots */
	{ 0x2224, 15 }, /* TDMSSIZE */
	{ 0x2103, 1 },  /* TDMSLOTS: 2 */
	{ 0x2330, 1 },  /* TDMCSE */
	{ 0x2340, 1 },  /* TDMVSE */
	{ 0x2603, 0 },  /* TDMSPKS: the speaker's data in slot 0 */
	{ 0x26c3, 0 },  /* TDMCSS */
	{ 0x2703, 1 },  /* TDMVSS */
	{ 0x6801, 2 },  /* TDMSRCMAP */
	{ 0x6821, 1 },  /* TDMSRCAS */
	{ 0x6841, 2 },  /* TDMSRCBS */
	{ 0x6110, 1 },  /* LPM0BYP */
	{ 0x6365, 27 },
	{ 0x6565, 0 },
	{ 0x7033, 12 }, /* DCMCC */
	{ 0x6163, 6 },  /* TDMSPKG: the gain, the cnt's (never more here) */
	{ 0x7444, 7 },  /* DCTRIP */
	{ 0x7534, 5 },  /* DCTRIP2 */
	{ 0x7402, 4 },  /* DCVOF */
	{ 0x7002, 5 },  /* DCVOS */
	{ 0x0580, 0 },  /* PGAE */
	{ 0x0560, 0 },  /* SSPDME */
	{ 0x5170, 0 },  /* DPSA */
	{ 0x52e0, 1 },  /* SLOPESET */
};

/* the "music" profile: what it sets to start, and its stop list */
static const struct bf_value music_start[] = {
	{ 0x0203, 8 },  /* AUDFS: 48 kHz */
	{ 0x0030, 1 },  /* AMPE */
	{ 0x64e1, 1 },  /* LPM1MODE */
	{ 0x62e1, 0 },  /* LNMODE */
	{ 0x0040, 1 },  /* DCA */
	{ 0x52e0, 1 },  /* SLOPESET */
};
static const struct bf_value music_stop[] = {
	{ 0x0030, 0 },  /* AMPE */
	{ 0x62e1, 0 },  /* LNMODE */
	{ 0x64e1, 0 },  /* LPM1MODE */
	{ 0x0040, 0 },  /* DCA */
};

static bool
write_list(const struct bf_value *list, unsigned count)
{
	bool ok = true;

	for (unsigned i = 0; i < count; i++) {
		ok = field_set(list[i].bf, list[i].value) == 0 && ok;
	}
	return ok;
}

static void
status(const char *when)
{
	uint16_t s = 0, c0 = 0, c1 = 0;

	(void)reg_read(REG_STATUS, &s);
	(void)reg_read(0x00, &c0);
	(void)reg_read(0x01, &c1);
	say("tfa9872: %s: STATUS %#06x (PLLS %u CLKS %u NOCLK %u SWS %u AMPS %u AREFS %u UVDS %u OTDS %u), MANSTATE %u, "
	    "CONTROL0 %#06x CONTROL1 %#06x\n", when, s, (s >> 1) & 1, (s >> 5) & 1, (s >> 7) & 1, (s >> 10) & 1, (s >> 12) & 1,
	    (s >> 13) & 1, (s >> 4) & 1, (s >> 2) & 1, field_get(BF_MANSTATE), c0, c1);
}

/* tfa98xx_init(): I2CR, MANSCONF back to 0; then tfa9872_specific() */
static bool
chip_init(void)
{
	uint16_t value = 0;
	uint32_t error;

	(void)reg_write(0x00, 1u << 1);         /* I2CR: every register to its reset value */
	delay(2000);
	(void)field_set(BF_MANSCONF, 0);
	/* the keys: 0x0F 0x5A6B, then 0xA0 = 0xFB ^ 0x5A (key 1); key 2 (MTPK 0x5A) with 0x0F open */
	(void)reg_write(0x0F, 0x5A6B);
	error = reg_read(0xFB, &value);
	(void)reg_write(0xA0, (uint16_t)(value ^ 0x005A));
	(void)reg_write(0x0F, 0x5A6B);
	(void)field_set(BF_MTPK, 0x5A);
	(void)reg_write(0x0F, 0);
	if (error != 0) {
		return false;
	}
	switch (revision) {
	case 0x1a72:
	case 0x2a72:
		(void)reg_write(0x00, 0x1801);
		(void)reg_write(0x02, 0x2dc8);
		(void)reg_write(0x20, 0x0890);
		(void)reg_write(0x22, 0x043c);
		(void)reg_write(0x51, 0x0000);
		(void)reg_write(0x52, 0x1a1c);
		(void)reg_write(0x58, 0x161c);
		(void)reg_write(0x61, 0x0198);
		(void)reg_write(0x65, 0x0a8b);
		(void)reg_write(0x70, 0x07f5);
		(void)reg_write(0x74, 0xcc84);
		(void)reg_write(0x82, 0x01ed);
		(void)reg_write(0x83, 0x0014);
		(void)reg_write(0x84, 0x0021);
		(void)reg_write(0x85, 0x0001);
		break;
	case 0x1b72:
	case 0x2b72:
	case 0x3b72:
		(void)reg_write(0x02, 0x2dc8);
		(void)reg_write(0x20, 0x0890);
		(void)reg_write(0x22, 0x043c);
		(void)reg_write(0x23, 0x0001);
		(void)reg_write(0x51, 0x0000);
		(void)reg_write(0x52, 0x5a1c);
		(void)reg_write(0x61, 0x0198);
		(void)reg_write(0x63, 0x0a9a);
		(void)reg_write(0x65, 0x0a82);
		(void)reg_write(0x6f, 0x01e3);
		(void)reg_write(0x70, 0x06fd);
		(void)reg_write(0x71, 0x307e);
		(void)reg_write(0x74, 0xcc84);
		(void)reg_write(0x75, 0x1132);
		(void)reg_write(0x82, 0x01ed);
		(void)reg_write(0x83, 0x001a);
		break;
	default:
		say("tfa9872: no register patch for revision %#06x: its reset values stand\n", revision);
		break;
	}
	(void)field_set(BF_MANAOOSC, 1);        /* the 1 MHz oscillator off (PLMA4928) */
	if (reg_read(0xB0, &value) == 0) {
		(void)reg_write(0xB0, (uint16_t)(value | 1u << 3));    /* bypass_ovp (PLMA5258) */
	}
	if (!write_list(device_list, sizeof(device_list) / sizeof(device_list[0]))) {
		say("tfa9872: the device list was not all written\n");
		return false;
	}
	(void)field_set(BF_PWDN, 1);
	return true;
}

/* ---- ABOX's hook ---- */

static int
amp_start(uint32_t rate, uint32_t slot_bits, uint32_t channels)
{
	int result = 0;

	if (rate != 48000 || slot_bits != 16 || channels != 2) {
		say("tfa9872: start: %u Hz, %u x %u bits: only 48000 Hz, 2 x 16 is set up\n", rate, channels, slot_bits);
		return EINVAL;
	}
	lck_mtx_lock(amp_lock);
	if (!ready) {
		result = ENXIO;
	} else if (!running) {
		/* tfa_run_startup(): the profile's registers, out of power-down, then MANSCONF (the PLL may start) */
		(void)write_list(music_start, sizeof(music_start) / sizeof(music_start[0]));
		(void)field_set(BF_PWDN, 0);
		(void)field_set(BF_MANSCONF, 1);
		running = true;
		for (int i = 0; i < 50; i++) {
			uint16_t s = 0;

			delay(1000);
			if (reg_read(REG_STATUS, &s) == 0 && (s & (1u << 12))) {
				break;          /* AMPS: the amplifier switches */
			}
		}
		status("started");
	}
	lck_mtx_unlock(amp_lock);
	return result;
}

static void
amp_stop(void)
{
	lck_mtx_lock(amp_lock);
	if (running) {
		(void)field_set(BF_AMPE, 0);
		delay(10000);           /* the ramp down, on BCLK still running */
		(void)write_list(music_stop, sizeof(music_stop) / sizeof(music_stop[0]));
		(void)field_set(BF_PWDN, 1);
		(void)field_set(BF_MANSCONF, 0);
		running = false;
		status("stopped");
	}
	lck_mtx_unlock(amp_lock);
}

static void
amp_status(const char *when)
{
	lck_mtx_lock(amp_lock);
	status(when);
	lck_mtx_unlock(amp_lock);
}

static const struct hakios_abox_amp amp_hook = {
	.name = "tfa9872", .start = amp_start, .stop = amp_stop, .status = amp_status,
};

/* ---- the steps ---- */

static void
pins_set(void)
{
	vm_offset_t g = ml_io_map(CMGP_GPIO_PA, 0x1000);

	if (g == 0) {
		say("tfa9872: CMGP's GPIO could not be mapped\n");
		return;
	}
	for (int i = 0; i < 2; i++) {
		volatile uint32_t *con = (volatile uint32_t *)(g + (i == 0 ? GPM2 : GPM3));
		uint32_t was = con[0];

		con[0] = (was & ~0xFu) | 3u;    /* pin 0: function 3, the I2C */
		con[2] &= ~0xFu;                /* PUD: none */
		say("tfa9872: %s CON %#x -> %#x, PUD %#x, DAT %#x\n", i == 0 ? "gpm2 (SCL)" : "gpm3 (SDA)", was, con[0], con[2],
		    con[1]);
	}
}

static void
amp_thread(void *arg, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg;
	vm_offset_t cmu;

	say("tfa9872: 1: I2C_CMGP00 (0x11D10000): its clocks (CMGP), pins gpm2-0/gpm3-0; the chip at 0x34\n");
	if (!hakios_hsi2c_map(&bus, say)) {
		goto out;
	}
	cmu = bus.cmu;
	/* the IP clock: MUX 1 (CMGP's bus), DIV 1: 200 MHz, as Samsung's CAL leaves it */
	*(volatile uint32_t *)(cmu + CMGP_MUX_I2C) = (*(volatile uint32_t *)(cmu + CMGP_MUX_I2C) & ~1u) | 1u;
	*(volatile uint32_t *)(cmu + CMGP_DIV_I2C) = (*(volatile uint32_t *)(cmu + CMGP_DIV_I2C) & ~0xFu) | 1u;
	delay(100);
	say("tfa9872: 1: CMGP MUX_I2C %#x DIV_I2C %#x\n", *(volatile uint32_t *)(cmu + CMGP_MUX_I2C),
	    *(volatile uint32_t *)(cmu + CMGP_DIV_I2C));
	pins_set();
	lck_mtx_lock(amp_lock);
	hakios_hsi2c_power(&bus);
	hakios_hsi2c_reset(&bus);
	if (reg_read(REG_REV, &revision) != 0 || (revision & 0xFF) != 0x72) {
		say("tfa9872: 1: no TFA9872 answers at 0x34 (revision %#06x, bus errors %llu): nothing written\n", revision,
		    bus.errors);
		lck_mtx_unlock(amp_lock);
		goto out;
	}
	say("tfa9872: 1: revision %#06x\n", revision);
	status("as found");
	if (steps < 2) {
		lck_mtx_unlock(amp_lock);
		goto out;
	}
	say("tfa9872: 2: reset, the register patch, the device list (gain TDMSPKG 6); powered down\n");
	ready = chip_init();
	status(ready ? "set up" : "after a failure");
	lck_mtx_unlock(amp_lock);
	if (!ready || steps < 3) {
		goto out;
	}
	say("tfa9872: 3: handed to ABOX's UAIF2: its start turns the amplifier on, its stop off\n");
	hakios_abox_set_amp(&amp_hook);
out:
	thread_terminate(current_thread());
}

__startup_func
static void
tfa9872_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("tfa9872", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (kernel_thread_start(amp_thread, (void *)(uintptr_t)(steps > 3 ? 3 : steps), &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, tfa9872_init);
