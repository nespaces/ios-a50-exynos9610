/*
 * The battery of the Galaxy A50 (SM-A505FN, Exynos 9610): the fuel gauge and the cable detector (MUIC) of its Silicon
 * Mitus SM5713 (charger, fuel gauge, MUIC in one chip), read -- never written -- over i2c_1 (0x13840000) and handed to
 * hakios_battery_report() (drivers/power/hakios.battery.c), the power source iOS reads. Only with boot-arg sm5713=1:
 * every address below belongs to this phone.
 *
 * The bus is the older Samsung I2C ("samsung,s3c2440-i2c"), the touch screen's kind (hakios.melfas_mss100.c, whose
 * polled transfers this file repeats); its clock is under hardware auto clock gating (GATE_I2C_1_QCH).
 *   I2C_1              0x13840000   IICCON +0, IICSTAT +4, IICDS +0xC, IICLC +0x10
 *   GPIO TOP           0x139B0000   gpp0 +0x00: pin 3 SCL, pin 2 SDA, function 2 (i2c1_bus)
 * The chip answers at three addresses (sm5713-private.h): the MUIC 0x25, the charger 0x49, the fuel gauge 0x71. The
 * gauge's registers are 16 bits, little-endian (i2c_smbus_read_word_data), Samsung's sm5713_fuelgauge.c's scales:
 *   0x00 DEVICE_ID    0x05 SOC: [14:8] whole percent, [7:0] 1/256 of one; bit 15 set reads as 0
 *   0x07 VBAT         mV = (r & 0x3FFF) * 1000 >> 11
 *   0x08 CURRENT      mA = (r & 0x3FFF) * 1000 >> 11, bit 15 the sign (discharging)
 *   0x09 TEMPERATURE  0.1 C = (r & 0x7FF0) * 10 >> 8, bit 15 the sign
 *   0x0A SOC_CYCLE    cycles = r & 0x3FF
 * The MUIC's DEVICETYPE1 (0x07): bit 7 LO_TA, 6 QC20_TA, 5 AFC_TA, 4 U200, 3 CDP, 2 DCP, 1 SDP, 0 DCD_OUT_SDP;
 * DEVICETYPE2 (0x08) bit 4 OTG. Its interrupt registers (0x01, 0x02) clear when read: never read here.
 * Sources: the A50's device tree (i2c_1, sm5713@49, i2c1_bus), Samsung's 4.14 kernel for it (drivers/mfd/sm5713_core.c,
 * drivers/battery_v2/sm5713_fuelgauge.c, include/linux/mfd/sm5713-private.h, include/linux/muic/sm5713-muic.h), read
 * for these facts, not copied; the port's analysis notes.
 *
 * In steps, each said before it is made. Boot-arg sm5713_steps=N stops after the first N:
 *   1  map the blocks and read what they are set to          (nothing written)
 *   2  the bus's pins to I2C
 *   3  the bus set up: master, ack, the clock divided (sm5713_scale=, default 3)
 *   4  ask the chip once: the gauge's id, charge, voltage, current, temperature, cycles; the MUIC's cable types
 *   5  report every 5 s (the default)
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void delay(int usec);

/* (every line to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_line(const char *line);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

/* ---- where things are ---- */
#define I2C_PA                  0x13840000UL
#define TOP_PA                  0x139B0000UL
#define GPP0                    0x000
#define GPIO_CON                0x0
#define GPIO_DAT                0x4
#define GPIO_PUD                0x8
#define PIN_SDA                 2
#define PIN_SCL                 3
#define MUIC                    0x25
#define GAUGE                   0x71

/* ---- the bus (i2c-s3c2410.c's registers; the polled transfers of hakios.melfas_mss100.c) ---- */
#define IICCON                  0x00
#define IICSTAT                 0x04
#define IICDS                   0x0c
#define IICLC                   0x10
#define CON_ACKEN               (1u << 7)
#define CON_TXDIV_512           (1u << 6)
#define CON_IRQEN               (1u << 5)
#define CON_IRQPEND             (1u << 4)
#define STAT_MASTER_RX          (2u << 6)
#define STAT_MASTER_TX          (3u << 6)
#define STAT_START              (1u << 5)
#define STAT_TXRXEN             (1u << 4)
#define STAT_ARBITR             (1u << 3)
#define STAT_LASTBIT            (1u << 0)
#define LC_FILTER_ON            (1u << 2)
#define LC_SDA_DELAY5           (1u << 0)
#define BYTE_TIMEOUT_US         5000

/* ---- the chip ---- */
#define FG_DEVICE_ID            0x00
#define FG_SOC                  0x05
#define FG_VBAT                 0x07
#define FG_CURRENT              0x08
#define FG_TEMPERATURE          0x09
#define FG_SOC_CYCLE            0x0A
#define MUIC_DEVICETYPE1        0x07
#define MUIC_DEVICETYPE2        0x08
#define DT1_COMPUTER            ((1u << 3) | (1u << 1) | (1u << 0))     /* CDP, SDP, DCD_OUT_SDP */
#define DT1_CHARGER             ((1u << 7) | (1u << 6) | (1u << 5) | (1u << 4) | (1u << 2))
#define DESIGN_UAH              4000000         /* the A50's 4000 mAh */
#define POLL_MS                 5000

static vm_offset_t i2c, top;
static lck_mtx_t *bus_lock;            /* the battery's thread and the flashlight share the bus */
static volatile bool bus_ready;

/* made once, by whichever of the battery and the flashlight starts first */
static void
bus_lock_make(void)
{
	static lck_grp_t *group;

	if (bus_lock == NULL) {
		group = lck_grp_alloc_init("hakios.sm5713", LCK_GRP_ATTR_NULL);
		bus_lock = lck_mtx_alloc_init(group, LCK_ATTR_NULL);
	}
}
static volatile int32_t last_temp_hundredths;
static volatile bool last_external = true;     /* (until the first read: no boost asked for) */
static uint32_t scale = 3;
static struct {
	uint64_t transfers, errors;
	uint32_t last_error;
} stats;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static void
pin_field(vm_offset_t bank, uint32_t reg, uint32_t pin, uint32_t v)
{
	uint32_t shift = pin * 4;

	wr(bank, reg, (rd(bank, reg) & ~(0xfu << shift)) | (v << shift));
}

static uint32_t
bus_con(void)
{
	return CON_ACKEN | CON_TXDIV_512 | CON_IRQEN | (scale & 0xf);
}

static int
bus_wait(void)
{
	for (uint32_t us = 0; us < BYTE_TIMEOUT_US; us++) {
		if (rd(i2c, IICCON) & CON_IRQPEND) {
			return 0;
		}
		delay(1);
	}
	return 1;
}

static void
bus_stop(uint32_t mode)
{
	wr(i2c, IICSTAT, mode | STAT_TXRXEN);
	wr(i2c, IICCON, bus_con());
	for (uint32_t us = 0; us < BYTE_TIMEOUT_US && (rd(i2c, IICSTAT) & STAT_START); us++) {
		delay(1);
	}
}

/*
 * One register read: its address written, a repeated start, `rlen` bytes read. 0 or the step that failed: 1 the bus
 * busy, 2 no ACK of the address, 3 of the register, 4 of the read address, 5 a timeout, 6 the arbitration lost.
 */
static uint32_t
bus_read(uint8_t chip, uint8_t reg, uint8_t *r, uint32_t rlen)
{
	uint32_t error = 0, con = bus_con();

	lck_mtx_lock(bus_lock);
	stats.transfers++;
	for (uint32_t us = 0; us < BYTE_TIMEOUT_US && (rd(i2c, IICSTAT) & STAT_START); us++) {
		delay(1);
	}
	if (rd(i2c, IICSTAT) & STAT_START) {
		error = 1;
		goto out;
	}
	wr(i2c, IICSTAT, STAT_MASTER_TX | STAT_TXRXEN);
	wr(i2c, IICDS, (uint32_t)chip << 1);
	delay(1);
	wr(i2c, IICCON, con);
	wr(i2c, IICSTAT, STAT_MASTER_TX | STAT_START | STAT_TXRXEN);
	if (bus_wait()) {
		error = 5;
		goto stop_tx;
	}
	if (rd(i2c, IICSTAT) & STAT_ARBITR) {
		error = 6;
		goto stop_tx;
	}
	if (rd(i2c, IICSTAT) & STAT_LASTBIT) {
		error = 2;
		goto stop_tx;
	}
	wr(i2c, IICDS, reg);
	delay(1);
	wr(i2c, IICCON, con);
	if (bus_wait()) {
		error = 5;
		goto stop_tx;
	}
	if (rd(i2c, IICSTAT) & STAT_LASTBIT) {
		error = 3;
		goto stop_tx;
	}
	wr(i2c, IICDS, (uint32_t)chip << 1 | 1);
	delay(1);
	wr(i2c, IICSTAT, STAT_MASTER_RX | STAT_START | STAT_TXRXEN);
	wr(i2c, IICCON, con);
	if (bus_wait()) {
		error = 5;
		goto stop_rx;
	}
	if (rd(i2c, IICSTAT) & STAT_LASTBIT) {
		error = 4;
		goto stop_rx;
	}
	for (uint32_t i = 0; i < rlen; i++) {
		wr(i2c, IICCON, i + 1 == rlen ? con & ~CON_ACKEN : con);
		if (bus_wait()) {
			error = 5;
			goto stop_rx;
		}
		r[i] = (uint8_t)rd(i2c, IICDS);
	}
stop_rx:
	bus_stop(STAT_MASTER_RX);
	goto out;
stop_tx:
	bus_stop(STAT_MASTER_TX);
out:
	if (error != 0) {
		stats.errors++;
		stats.last_error = error;
	}
	lck_mtx_unlock(bus_lock);
	return error;
}

/* One register written: the chip's address, the register, the value. 0 or the step that failed (as bus_read). */
static uint32_t
bus_write(uint8_t chip, uint8_t reg, uint8_t value)
{
	uint32_t error = 0, con = bus_con();
	const uint8_t bytes[2] = { reg, value };

	lck_mtx_lock(bus_lock);
	stats.transfers++;
	for (uint32_t us = 0; us < BYTE_TIMEOUT_US && (rd(i2c, IICSTAT) & STAT_START); us++) {
		delay(1);
	}
	if (rd(i2c, IICSTAT) & STAT_START) {
		error = 1;
		goto out;
	}
	wr(i2c, IICSTAT, STAT_MASTER_TX | STAT_TXRXEN);
	wr(i2c, IICDS, (uint32_t)chip << 1);
	delay(1);
	wr(i2c, IICCON, con);
	wr(i2c, IICSTAT, STAT_MASTER_TX | STAT_START | STAT_TXRXEN);
	if (bus_wait()) {
		error = 5;
		goto stop;
	}
	if (rd(i2c, IICSTAT) & (STAT_ARBITR | STAT_LASTBIT)) {
		error = (rd(i2c, IICSTAT) & STAT_ARBITR) ? 6 : 2;
		goto stop;
	}
	for (unsigned i = 0; i < 2; i++) {
		wr(i2c, IICDS, bytes[i]);
		delay(1);
		wr(i2c, IICCON, con);
		if (bus_wait()) {
			error = 5;
			goto stop;
		}
		if (rd(i2c, IICSTAT) & STAT_LASTBIT) {
			error = 3;
			goto stop;
		}
	}
stop:
	bus_stop(STAT_MASTER_TX);
out:
	if (error != 0) {
		stats.errors++;
		stats.last_error = error;
	}
	lck_mtx_unlock(bus_lock);
	return error;
}

/*
 * The flashlight (torch=1): the SM5713's flash block (leds-sm5713-fled.c, sm5713_charger_oper.c), at the charger's
 * address 0x49: FLED1CNTL2 [6:4] the torch current (the device tree's torch-brightness, 4), FLED1CNTL1 [1:0] = 3 (lit
 * by its pin, "external"), the pin torch-en = gpa1-3 (ALIVE 0x11850000 + 0x40) high. With no cable in, the charger
 * is put into its flash boost (CNTL2 [3:0] = 8, BSTCNTL1 [3:0] = 4.5 V) while lit, as Samsung's table says, and put
 * back after. /dev/hakios-torch takes a level of 0 to 1000 (a 32-bit number); any level above 0 is the one current.
 */
#define CHG                     0x49
#define CHG_CNTL2               0x14
#define CHG_BSTCNTL1            0x23
#define FLED1CNTL1              0x41
#define FLED1CNTL2              0x42
#define TORCH_CURRENT           4
#define ALIVE_PA                0x11850000UL
#define GPA1                    0x040
#define TORCH_PIN               3
#define TORCH_HOT_C             45

static vm_offset_t alive;
static uint32_t torch_lit;
static bool torch_boost;
static uint8_t saved_cntl2, saved_bst;
static lck_mtx_t *torch_lock;

static uint32_t
chg_update(uint8_t reg, uint8_t value, uint8_t mask)
{
	uint8_t v = 0;
	uint32_t e = bus_read(CHG, reg, &v, 1);

	return e != 0 ? e : bus_write(CHG, reg, (uint8_t)((v & ~mask) | (value & mask)));
}

static bool
torch_set(uint32_t level)
{
	volatile uint32_t *dat = (volatile uint32_t *)(alive + GPA1 + 4);
	uint32_t e = 0;

	if (level != 0 && torch_lit == 0) {
		if (!last_external) {
			uint8_t c = 0, b = 0;

			if (bus_read(CHG, CHG_CNTL2, &c, 1) == 0 && bus_read(CHG, CHG_BSTCNTL1, &b, 1) == 0) {
				saved_cntl2 = c;
				saved_bst = b;
				torch_boost = true;
				e |= chg_update(CHG_BSTCNTL1, 0x1, 0xf);
				e |= chg_update(CHG_CNTL2, 0x8, 0xf);
			}
		}
		e |= chg_update(FLED1CNTL2, TORCH_CURRENT << 4, 0x7 << 4);
		e |= chg_update(FLED1CNTL1, 0x3, 0x3);
		*dat |= 1u << TORCH_PIN;
	} else if (level == 0 && torch_lit != 0) {
		*dat &= ~(1u << TORCH_PIN);
		e |= chg_update(FLED1CNTL1, 0x0, 0x3);
		if (torch_boost) {
			e |= chg_update(CHG_CNTL2, saved_cntl2, 0xf);
			e |= chg_update(CHG_BSTCNTL1, saved_bst, 0xf);
			torch_boost = false;
		}
	}
	torch_lit = level;
	if (e != 0) {
		say("sm5713: torch %u: an I2C step failed (%u)\n", level, e);
	}
	return e == 0;
}

static int
torch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t level;
	int error = uiomove((caddr_t)&level, sizeof(level), uio);

	if (error != 0) {
		return error;
	}
	if (!bus_ready) {
		return EBUSY;
	}
	level = level > 1000 ? 1000 : level;
	lck_mtx_lock(torch_lock);
	if (level != 0 && last_temp_hundredths > TORCH_HOT_C * 100) {
		say("sm5713: torch: the battery is above %d C: the light stays off\n", TORCH_HOT_C);
		(void)torch_set(0);
		error = EBUSY;
	} else if (!torch_set(level)) {
		error = EIO;
	}
	lck_mtx_unlock(torch_lock);
	return error;
}

static int
torch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t level = torch_lit;

	return uiomove((caddr_t)&level, sizeof(level), uio);
}

static int
torch_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw torch_cdevsw = {
	.d_open = torch_open, .d_close = torch_open, .d_read = torch_read, .d_write = torch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

void
hakios_torch_init(void)
{
	uint32_t on = 0;
	int major;

	uint32_t battery = 0;

	if (!PE_parse_boot_argn("torch", &on, sizeof(on)) || on == 0 ||
	    !PE_parse_boot_argn("sm5713", &battery, sizeof(battery)) || battery == 0) {
		return;          /* (the bus is the battery driver's: sm5713=1) */
	}
	bus_lock_make();
	alive = ml_io_map(ALIVE_PA, 0x1000);
	if (alive == 0) {
		return;
	}
	torch_lock = lck_mtx_alloc_init(lck_grp_alloc_init("hakios.torch", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	{
		volatile uint32_t *con = (volatile uint32_t *)(alive + GPA1);

		con[1] &= ~(1u << TORCH_PIN);
		con[0] = (con[0] & ~(0xfu << (4 * TORCH_PIN))) | (1u << (4 * TORCH_PIN));
	}
	major = cdevsw_add(-1, &torch_cdevsw);
	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-torch") == NULL) {
		say("sm5713: /dev/hakios-torch cannot be made\n");
		return;
	}
	say("sm5713: /dev/hakios-torch: the flashlight for iOS (SM5713 FLED1, gpa1-3)\n");
}

void
hakios_torch_quiesce(void)
{
	if (alive != 0 && torch_lit != 0 && bus_ready) {
		(void)torch_set(0);
	}
}

/* a 16-bit register of the gauge, little-endian; one retry */
static bool
gauge_word(uint8_t reg, uint32_t *v)
{
	uint8_t b[2] = { 0 };

	if (bus_read(GAUGE, reg, b, 2) != 0 && bus_read(GAUGE, reg, b, 2) != 0) {
		return false;
	}
	*v = (uint32_t)b[0] | (uint32_t)b[1] << 8;
	return true;
}

static bool
muic_byte(uint8_t reg, uint8_t *v)
{
	return bus_read(MUIC, reg, v, 1) == 0 || bus_read(MUIC, reg, v, 1) == 0;
}

/* everything the power source wants, from the gauge and the MUIC */
static bool
battery_read(struct hakios_battery_state *b, uint32_t raw[6], uint8_t dt[2])
{
	uint32_t soc, vbat, cur, temp, cyc, pct_x256;
	int32_t ma, dc;

	if (!gauge_word(FG_SOC, &soc) || !gauge_word(FG_VBAT, &vbat) || !gauge_word(FG_CURRENT, &cur) ||
	    !gauge_word(FG_TEMPERATURE, &temp) || !gauge_word(FG_SOC_CYCLE, &cyc)) {
		return false;
	}
	if (!muic_byte(MUIC_DEVICETYPE1, &dt[0]) || !muic_byte(MUIC_DEVICETYPE2, &dt[1])) {
		dt[0] = dt[1] = 0;
	}
	raw[0] = soc, raw[1] = vbat, raw[2] = cur, raw[3] = temp, raw[4] = cyc;
	memset(b, 0, sizeof(*b));
	b->size = sizeof(*b);
	pct_x256 = (soc & 0x8000) ? 0 : ((soc & 0x7F00) >> 8) * 256 + (soc & 0xFF);     /* percent, in 1/256 */
	b->percent_hundredths = (int32_t)(pct_x256 * 100 / 256);
	if (b->percent_hundredths > 10000) {
		b->percent_hundredths = 10000;
	}
	b->voltage_uv = (int32_t)(((vbat & 0x3FFF) * 1000) >> 11) * 1000;
	ma = (int32_t)(((cur & 0x3FFF) * 1000) >> 11);
	b->current_ua = ((cur & 0x8000) ? -ma : ma) * 1000;
	dc = (int32_t)(((temp & 0x7FF0) * 10) >> 8);
	b->temperature_hundredths = ((temp & 0x8000) ? -dc : dc) * 10;
	b->present = 1;
	b->external = (dt[0] & (DT1_COMPUTER | DT1_CHARGER)) != 0;
	b->usb_type = !b->external ? 0 : (dt[0] & DT1_CHARGER) ? 2 : 1;
	b->status = !b->external ? 2 : b->percent_hundredths >= 10000 ? 4 : b->current_ua > 0 ? 1 : 3;
	b->design_uah = DESIGN_UAH;
	b->full_uah = DESIGN_UAH;
	b->charge_uah = (int32_t)((int64_t)DESIGN_UAH * b->percent_hundredths / 10000);
	b->cycles = (int32_t)(cyc & 0x3FF);
	return true;
}

static void
say_state(const char *when, const struct hakios_battery_state *b, const uint32_t raw[6], const uint8_t dt[2])
{
	say("sm5713: %s: %d.%02d%%, %d mV, %d mA, %d.%d C, %d cycles; cable %s (DEVICETYPE1 %#x, 2 %#x), %s; raw SOC %#x "
	    "VBAT %#x CURRENT %#x TEMP %#x CYCLE %#x\n", when, b->percent_hundredths / 100, b->percent_hundredths % 100,
	    b->voltage_uv / 1000, b->current_ua / 1000, b->temperature_hundredths / 100,
	    (b->temperature_hundredths < 0 ? -b->temperature_hundredths : b->temperature_hundredths) % 100 / 10, b->cycles,
	    !b->external ? "none" : b->usb_type == 2 ? "a charger" : "a computer", dt[0], dt[1],
	    b->status == 1 ? "charging" : b->status == 2 ? "discharging" : b->status == 4 ? "full" : "not charging",
	    raw[0], raw[1], raw[2], raw[3], raw[4]);
}

static void
battery_thread(void *arg, __unused wait_result_t wr_)
{
	uint32_t steps = (uint32_t)(uintptr_t)arg, step = 0, raw[6] = { 0 }, id = 0;
	uint8_t dt[2] = { 0 };
	struct hakios_battery_state b;

	say("sm5713: step 1: map I2C_1 and the GPIO block; read what they are set to\n");
	i2c = ml_io_map(I2C_PA, 0x1000);
	top = ml_io_map(TOP_PA, 0x1000);
	if (!i2c || !top) {
		say("sm5713: a block could not be mapped\n");
		goto out;
	}
	say("sm5713: I2C_1 IICCON %x IICSTAT %x IICLC %x; gpp0 CON %x DAT %x PUD %x\n", rd(i2c, IICCON), rd(i2c, IICSTAT),
	    rd(i2c, IICLC), rd(top + GPP0, GPIO_CON), rd(top + GPP0, GPIO_DAT), rd(top + GPP0, GPIO_PUD));
	if (++step >= steps) {
		goto out;
	}

	say("sm5713: step 2: the pins to I2C (gpp0-2 SDA, gpp0-3 SCL)\n");
	pin_field(top + GPP0, GPIO_CON, PIN_SDA, 2);
	pin_field(top + GPP0, GPIO_CON, PIN_SCL, 2);
	pin_field(top + GPP0, GPIO_PUD, PIN_SDA, 0);
	pin_field(top + GPP0, GPIO_PUD, PIN_SCL, 0);
	say("sm5713: gpp0 CON now %x\n", rd(top + GPP0, GPIO_CON));
	if (++step >= steps) {
		goto out;
	}

	say("sm5713: step 3: the bus: master, ACK, PERI / 512 / %u, filter on\n", scale + 1);
	wr(i2c, IICSTAT, 0);
	wr(i2c, IICLC, LC_FILTER_ON | LC_SDA_DELAY5);
	wr(i2c, IICCON, bus_con());
	wr(i2c, IICSTAT, STAT_TXRXEN);
	if (++step >= steps) {
		goto out;
	}

	say("sm5713: step 4: ask the gauge (0x%x) and the MUIC (0x%x)\n", GAUGE, MUIC);
	if (!gauge_word(FG_DEVICE_ID, &id)) {
		say("sm5713: the gauge does not answer (step %u failed; IICSTAT %x)\n", stats.last_error, rd(i2c, IICSTAT));
		goto out;
	}
	say("sm5713: the gauge's DEVICE_ID %#x\n", id);
	if (!battery_read(&b, raw, dt)) {
		say("sm5713: a register of the gauge did not read (%u)\n", stats.last_error);
		goto out;
	}
	say_state("now", &b, raw, dt);
	if (++step >= steps) {
		goto out;
	}

	bus_ready = true;
	say("sm5713: step 5: reporting every %u s\n", POLL_MS / 1000);
	for (unsigned tick = 0;; tick++) {
		if (battery_read(&b, raw, dt)) {
			last_temp_hundredths = b.temperature_hundredths;
			last_external = b.external;
			hakios_battery_report(&b);
			if (tick % 60 == 0) {
				say_state("every 5 minutes", &b, raw, dt);
			}
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
out:
	if (steps < 5) {
		say("sm5713: stopping here (sm5713_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
hakios_sm5713_init(void)
{
	uint32_t enabled = 0, steps = 5;
	thread_t thread;

	if (!PE_parse_boot_argn("sm5713", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	PE_parse_boot_argn("sm5713_steps", &steps, sizeof(steps));
	PE_parse_boot_argn("sm5713_scale", &scale, sizeof(scale));
	bus_lock_make();
	if (steps == 0 || scale > 15) {
		return;
	}
	if (kernel_thread_start(battery_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
