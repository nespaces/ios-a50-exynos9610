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
 * for these facts, not copied; KIMI-RESULT-POWER-WIFI.md.
 *
 * In steps, each said before it is made. Boot-arg sm5713_steps=N stops after the first N:
 *   1  map the blocks and read what they are set to          (nothing written)
 *   2  the bus's pins to I2C
 *   3  the bus set up: master, ack, the clock divided (sm5713_scale=, default 3)
 *   4  ask the chip once: the gauge's id, charge, voltage, current, temperature, cycles; the MUIC's cable types
 *   5  report every 5 s (the default)
 */
#include <kern/clock.h>
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
	return error;
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

	say("sm5713: step 5: reporting every %u s\n", POLL_MS / 1000);
	for (unsigned tick = 0;; tick++) {
		if (battery_read(&b, raw, dt)) {
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
	if (steps == 0 || scale > 15) {
		return;
	}
	if (kernel_thread_start(battery_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
