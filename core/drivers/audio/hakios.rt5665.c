/*
 * The Galaxy A50's audio codec: a Realtek RT5665 at 0x1b on i2c_6 (0x13890000, samsung,s3c2440-i2c).
 * MAINMIC is IN3 (differential), MICBIAS2; capture goes to AIF1, ABOX UAIF0 (abox_mic=1, WDMA1).
 * Only with boot-arg rt5665=N. Addresses from the phone's device tree (rt5665@1b on i2c@13890000,
 * i2c6-bus gpp1-5 SCL / gpp1-4 SDA function 2). The I2C controller is the same as the touch's
 * (hakios.melfas_mss100.c). Device ID 0x00ff is 0x6451.
 *
 *   1  map the bus and pins, read the ID (nothing of the codec written)
 *   2  reset, analog power, MICBIAS2, IN3, ADC, I2S1 a slave of UAIF0
 *   3  the same, left on for iOS (with abox=8 abox_mic=1)
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/hakios.rt5665.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern void thread_deallocate(thread_t thread);
extern kern_return_t thread_terminate(thread_t thread);
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

#define I2C_PA                  0x13890000UL    /* i2c_6 */
#define TOP_PA                  0x139B0000UL
#define GPP1                    0x020
#define GPIO_CON                0x0
#define GPIO_DAT                0x4
#define GPIO_PUD                0x8
#define PIN_SDA                 4
#define PIN_SCL                 5
#define CHIP                    0x1b

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
#define SCALE                   3

#define RT5665_RESET            0x0000
#define RT5665_IN3_IN4          0x000d
#define RT5665_STO1_ADC_DIG_VOL 0x001c
#define RT5665_STO1_ADC_MIXER   0x0026
#define RT5665_REC1_L2_MIXER    0x003c
#define RT5665_PWR_DIG_1        0x0061
#define RT5665_PWR_DIG_2        0x0062
#define RT5665_PWR_ANLG_1       0x0063
#define RT5665_PWR_ANLG_2       0x0064
#define RT5665_PWR_ANLG_3       0x0065
#define RT5665_I2S1_SDP         0x0070
#define RT5665_ADDA_CLK_1       0x0073
#define RT5665_GLB_CLK          0x0080
#define RT5665_PLL_CTRL_1       0x0081
#define RT5665_PLL_CTRL_2       0x0082
#define RT5665_CLK_DET          0x006b
#define RT5665_DIG_MISC         0x00fa
#define RT5665_CHOP_ADC         0x013b
#define RT5665_DEVICE_ID        0x00ff
#define RT5665_ID_VALUE         0x6451

#define PWR_I2S1_1              (1u << 15)
#define PWR_ADC_L1              (1u << 4)
#define PWR_ADC_S1F             (1u << 15)
#define PWR_VREF1               (1u << 15)
#define PWR_VREF2               (1u << 13)
#define PWR_FV2                 (1u << 12)
#define PWR_LDO                 (1u << 8)       /* PWR_DIG_1 */
#define DIG_GATE_CTRL           (1u << 0)
#define CKGEN_ADC1              (1u << 12)
#define PWR_FV1                 (1u << 14)
#define PWR_MB                  (1u << 9)
#define PWR_BG                  (1u << 7)
#define PWR_LDO1_DVO_12         0x2
#define PWR_BST3                (1u << 13)
#define PWR_MB2                 (1u << 10)
#define PWR_BST3_P              (1u << 5)
#define PWR_RM1_L               (1u << 1)
#define PWR_PLL                 (1u << 6)
#define I2S_MS_S                (1u << 15)
#define IN3_DF                  (1u << 15)
#define SCLK_SRC_PLL1           (1u << 14)
#define PLL1_SRC_BCLK1          (1u << 8)
#define I2S_PD1_2               (1u << 12)

static vm_offset_t i2c, top;
static uint32_t scale = SCALE;
static bool ready;

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

static uint32_t
bus_xfer(const uint8_t *w, uint32_t wlen, uint8_t *r, uint32_t rlen)
{
	uint32_t error = 0, con = bus_con();

	for (uint32_t us = 0; us < BYTE_TIMEOUT_US && (rd(i2c, IICSTAT) & STAT_START); us++) {
		delay(1);
	}
	if (rd(i2c, IICSTAT) & STAT_START) {
		return 1;
	}
	wr(i2c, IICSTAT, STAT_MASTER_TX | STAT_TXRXEN);
	wr(i2c, IICDS, CHIP << 1);
	delay(1);
	wr(i2c, IICCON, con);
	wr(i2c, IICSTAT, STAT_MASTER_TX | STAT_START | STAT_TXRXEN);
	if (bus_wait()) {
		error = 5;
		goto stop_tx;
	}
	if (rd(i2c, IICSTAT) & STAT_LASTBIT) {
		error = 2;
		goto stop_tx;
	}
	for (uint32_t i = 0; i < wlen; i++) {
		wr(i2c, IICDS, w[i]);
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
	}
	if (rlen == 0) {
		goto stop_tx;
	}
	wr(i2c, IICDS, CHIP << 1 | 1);
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
	return error;
stop_tx:
	bus_stop(STAT_MASTER_TX);
	return error;
}

static uint32_t
reg_read(uint16_t reg, uint16_t *value)
{
	uint8_t w[2] = { (uint8_t)(reg >> 8), (uint8_t)reg }, r[2] = { 0, 0 };
	uint32_t error = bus_xfer(w, 2, r, 2);

	if (error != 0) {
		error = bus_xfer(w, 2, r, 2);
	}
	*value = (uint16_t)(r[0] << 8 | r[1]);
	return error;
}

static uint32_t
reg_write(uint16_t reg, uint16_t value)
{
	uint8_t w[4] = { (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(value >> 8), (uint8_t)value };

	return bus_xfer(w, 4, NULL, 0);
}

static uint32_t
reg_set(uint16_t reg, uint16_t bits)
{
	uint16_t v = 0;
	uint32_t error = reg_read(reg, &v);

	return error != 0 ? error : reg_write(reg, v | bits);
}

/* IN3 -> BST3 -> RECMIX1L -> ADC1 L -> Stereo1 ADC (source ADC, not DD Mux) -> I2S1 slave.
 * PLL1 from UAIF0 BCLK (1.536 MHz) to 24.576 MHz, as exynos9610-rt5665.c hw_params. */
static void
mic_path(bool settle)
{
	uint16_t mix = 0, glb = 0, rec = 0;

	/*
	 * What Samsung's driver switches on outside the capture route, by the codec's bias level (rt5665.c
	 * rt5665_set_bias_level()): STANDBY sets PWR_DIG_1's LDO (the digital core's supply) and PREPARE sets DIG_MISC's
	 * gate (the digital blocks' clock). Without them the ADC's filter and the I2S never run: WDMA1 recorded at the
	 * right rate and every sample was 0 (10 November). And of the route itself: ADC1 L needs "ADC1 clock" (CHOP_ADC
	 * bit 12) and Vref2 as well as Vref1 (rt5665_dapm_routes).
	 */
	(void)reg_set(RT5665_PWR_DIG_1, PWR_LDO);
	(void)reg_set(RT5665_PWR_ANLG_1, PWR_VREF1 | PWR_VREF2 | PWR_MB | PWR_BG | PWR_LDO1_DVO_12);
	if (settle) {
		delay(15000);
	}
	(void)reg_set(RT5665_PWR_ANLG_1, PWR_FV1 | PWR_FV2);
	(void)reg_set(RT5665_DIG_MISC, DIG_GATE_CTRL);
	(void)reg_set(RT5665_CHOP_ADC, CKGEN_ADC1);
	(void)reg_set(RT5665_PWR_ANLG_2, PWR_BST3 | PWR_MB2 | PWR_BST3_P | PWR_RM1_L);
	(void)reg_set(RT5665_PWR_ANLG_3, PWR_PLL);
	(void)reg_set(RT5665_IN3_IN4, IN3_DF | (0x2c << 8));
	(void)reg_write(RT5665_REC1_L2_MIXER, 0x00f7);          /* BST3 unmute (bit 3) */
	(void)reg_set(RT5665_PWR_DIG_1, PWR_I2S1_1 | PWR_ADC_L1);
	(void)reg_set(RT5665_PWR_DIG_2, PWR_ADC_S1F);
	(void)reg_write(RT5665_STO1_ADC_MIXER, 0x6060);         /* ADC1 L/R unmute, src = ADC */
	(void)reg_write(RT5665_STO1_ADC_DIG_VOL, 0x2f2f);
	(void)reg_write(RT5665_I2S1_SDP, I2S_MS_S);
	(void)reg_write(RT5665_PLL_CTRL_1, 0x1f02);             /* N=62 K=2: 1.536 MHz -> 24.576 MHz */
	(void)reg_write(RT5665_PLL_CTRL_2, 0x0800);             /* M bypass */
	(void)reg_write(RT5665_GLB_CLK, SCLK_SRC_PLL1 | PLL1_SRC_BCLK1);
	(void)reg_write(RT5665_ADDA_CLK_1, I2S_PD1_2);          /* PD1 /2: 512fs / 256 */
	(void)reg_write(RT5665_CLK_DET, 0x0001);
	(void)reg_read(RT5665_STO1_ADC_MIXER, &mix);
	(void)reg_read(RT5665_GLB_CLK, &glb);
	(void)reg_read(RT5665_REC1_L2_MIXER, &rec);
	say("rt5665: path REC1_L2 %#06x STO1 %#06x GLB %#06x\n", rec, mix, glb);
	{
		uint16_t d1 = 0, d2 = 0, a1 = 0, a2 = 0, a3 = 0, misc = 0, chop = 0;

		(void)reg_read(RT5665_PWR_DIG_1, &d1);
		(void)reg_read(RT5665_PWR_DIG_2, &d2);
		(void)reg_read(RT5665_PWR_ANLG_1, &a1);
		(void)reg_read(RT5665_PWR_ANLG_2, &a2);
		(void)reg_read(RT5665_PWR_ANLG_3, &a3);
		(void)reg_read(RT5665_DIG_MISC, &misc);
		(void)reg_read(RT5665_CHOP_ADC, &chop);
		say("rt5665: power DIG %#06x %#06x ANLG %#06x %#06x %#06x DIG_MISC %#06x CHOP_ADC %#06x\n", d1, d2, a1, a2, a3,
		    misc, chop);
	}
}

static int
codec_bringup(void)
{
	uint16_t id = 0;

	if (ready) {
		return 0;
	}
	say("rt5665: I2C_6 0x13890000 gpp1-5/4, chip 0x1b\n");
	if (i2c == 0) {
		i2c = ml_io_map(I2C_PA, 0x1000);
	}
	if (top == 0) {
		top = ml_io_map(TOP_PA, 0x1000);
	}
	if (i2c == 0 || top == 0) {
		say("rt5665: a block could not be mapped\n");
		return 1;
	}
	pin_field(top + GPP1, GPIO_CON, PIN_SDA, 2);
	pin_field(top + GPP1, GPIO_CON, PIN_SCL, 2);
	pin_field(top + GPP1, GPIO_PUD, PIN_SDA, 0);
	pin_field(top + GPP1, GPIO_PUD, PIN_SCL, 0);
	wr(i2c, IICSTAT, 0);
	wr(i2c, IICLC, LC_FILTER_ON | LC_SDA_DELAY5);
	wr(i2c, IICCON, bus_con());
	wr(i2c, IICSTAT, STAT_TXRXEN);
	if (reg_read(RT5665_DEVICE_ID, &id) != 0 || id != RT5665_ID_VALUE) {
		say("rt5665: no chip at 0x1b (id %#06x)\n", id);
		return 1;
	}
	say("rt5665: DEVICE_ID %#06x\n", id);
	(void)reg_write(RT5665_RESET, 0);
	delay(20000);
	if (reg_read(RT5665_DEVICE_ID, &id) != 0 || id != RT5665_ID_VALUE) {
		say("rt5665: ID gone after reset (%#06x)\n", id);
		return 1;
	}
	mic_path(true);
	ready = true;
	say("rt5665: MAINMIC IN3 on, I2S1 slave, PLL from BCLK1\n");
	return 0;
}

void
hakios_rt5665_capture(bool on)
{
	uint16_t id = 0;

	if (!on) {
		return;
	}
	if (codec_bringup() != 0) {
		return;
	}
	(void)reg_read(RT5665_DEVICE_ID, &id);
	/* UAIF0 BCLK is running: PLL/mixers only, no VREF wait (this can run beside the speaker feeder). */
	mic_path(false);
	say("rt5665: capture: ID %#06x mixers rewritten\n", id);
}

static void
codec_thread(void *arg, __unused wait_result_t result)
{
	(void)arg;
	(void)codec_bringup();
	(void)thread_terminate(current_thread());
}

__startup_func
static void
rt5665_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	(void)PE_parse_boot_argn("rt5665", &steps, sizeof(steps));
	say("rt5665: init steps %u\n", steps);
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(codec_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, rt5665_init);
