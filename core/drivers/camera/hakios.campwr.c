/*
 * The Galaxy A50's cameras: first the supplies. Only with boot-arg campwr=N (see campwr_thread()).
 * Sources: the phone's device tree (s2mpb03@56 on i2c@13860000, i2c3-bus gpp0-7 SCL / gpp0-6 SDA function 2) and
 * Samsung's s2mpb03 driver for the registers.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.physmap.h>
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

#define TOP_PA                  0x139B0000UL
#define GPP0                    0x000
#define GPIO_CON                0x0
#define GPIO_PUD                0x8
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
#define STAT_LASTBIT            (1u << 0)
#define LC_FILTER_ON            (1u << 2)
#define LC_SDA_DELAY5           (1u << 0)
#define BYTE_TIMEOUT_US         5000
#define SCALE                   3

#define PMIC_I2C_PA             0x13860000UL    /* i2c_3: gpp0-7 SCL, gpp0-6 SDA, function 2 */
#define PMIC_ADDR               0x56            /* S2MPB03 */
#define REG_ID                  0x00
#define REG_LDO(n)              (0x02 + (n))    /* LDO1_CTRL 3 ... LDO7_CTRL 9 (include/linux/mfd/s2mpb03.h) */
#define LDO_ON                  0x80

static vm_offset_t i2c, top;
static uint32_t scale = SCALE;
static uint32_t chip;

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
	wr(i2c, IICDS, chip << 1);
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
	wr(i2c, IICDS, chip << 1 | 1);
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
pmic_read(uint8_t reg, uint8_t *value)
{
	uint8_t w = reg, r = 0;
	uint32_t error = bus_xfer(&w, 1, &r, 1);

	if (error != 0) {
		error = bus_xfer(&w, 1, &r, 1);
	}
	*value = r;
	return error;
}

static uint32_t
pmic_write(uint8_t reg, uint8_t value)
{
	uint8_t w[2] = { reg, value };

	return bus_xfer(w, 2, NULL, 0);
}

static void
dump(const char *what)
{
	uint8_t v[10];

	for (unsigned i = 0; i < 10; i++) {
		(void)pmic_read((uint8_t)i, &v[i]);
	}
	say("campwr: %s: S2MPB03 regs 0..9: %02x %02x %02x | LDO1 %02x LDO2 %02x LDO3 %02x LDO4 %02x LDO5 %02x LDO6 %02x LDO7 %02x\n",
	    what, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
}


/*
 * The rear camera's sensor, a Sony IMX576 at 0x1a on hsi2c_13 (0x138B0000, a camera I2C of PERI: pins gpc0-2/3 function 2),
 * its reset gpg1-1 (RCAM1_RST_N), its clock CIS_CLK0 (CMU_TOP: mux 0x1008 0 = OSCCLK 26 MHz, divider 0x1810, gate
 * 0x2010) on gpc2-0 function 2. Banks of the pinctrl at 0x139B0000: gpc0 0x60, gpc2 0xa0, gpg1 0xe0 (their order in the
 * device tree, 0x20 each).
 *   campwr=3: say what the clocks and pins are, write nothing
 *   campwr=4: the three supplies, the clock, the reset released, the chip's model id (registers 0x0016/0x0017: 0x0576)
 */
#define PINCTRL_PA              0x139B0000UL
#define GPC0                    0x060
#define GPC2                    0x0a0
#define GPG1                    0x0e0
#define TOP_CMU_PA              0x12100000UL
#define CIS0_MUX                0x1008
#define CIS0_DIV                0x1810
#define CIS0_GATE               0x2010
#define GATE_MANUAL             (1u << 20)
#define GATE_ON                 (1u << 21)

static struct hakios_hsi2c cam = {
	.name = "hsi2c_13 (rear camera)",
	.regs_pa = 0x138B0000, .usi_mode_pa = 0,
	.cmu_pa = 0x10030000, .cmu_size = 0x3000,
	.gates = { 0x2008, 0x2030, 0x2034 }, .ngates = 3,       /* GATE_CLK_PERI_I2C, CAMI2C_1's IPCLK and PCLK */
	.qch = 0, .div = 0x1800, .mux = 0x120,
	.pins_pa = PINCTRL_PA + GPC0, .pins_mask = 0xff00, .pins_want = 0x2200,
	.ipclk_mhz = 200, .bus_hz = 400000,
};


extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static uint32_t
sensor_write(uint16_t reg, uint8_t value)
{
	uint8_t w[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, value };

	return hakios_hsi2c_message(&cam, 0x1a, false, w, 3, true);
}

static uint32_t
sensor_read(uint16_t reg, uint8_t *value)
{
	uint8_t w[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
	uint32_t error = hakios_hsi2c_message(&cam, 0x1a, false, w, 2, false);

	if (error == 0) {
		error = hakios_hsi2c_message(&cam, 0x1a, true, value, 1, true);
	}
	return error;
}


/*
 * The camera block (CAM): its power domain is the PMU's CAM_CONFIGURATION (0x11860000 + 0x4020, low nibble 0xf on) and
 * CAM_STATUS (+0x4024); the clock block CMU_CAM is at 0x14500000 (BUS_USER mux PLL_CON0 at 0x100, bit 4 the select), and
 * the MIPI receiver CSIS0 at 0x14400000 (CSIS_VERSION at +0). campwr=6: the domain on, the CMU's mux said and set, the
 * receiver's version read (Samsung: pmucal_local_exynos9610.h cam_on; fimc-is2/sensor/csi2/fimc-is-hw-csi-v4_0.h).
 */
static void
cam_block_stage(void)
{
	vm_offset_t pmu = ml_io_map(0x11860000UL, 0x5000), cmu = ml_io_map(0x14500000UL, 0x4000), csis;
	uint32_t status;

	if (pmu == 0 || cmu == 0) {
		say("campwr: PMU or CMU_CAM could not be mapped\n");
		return;
	}
	say("campwr: PMU CAM_CONFIGURATION %#x CAM_STATUS %#x\n", rd(pmu, 0x4020), rd(pmu, 0x4024));
	wr(pmu, 0x4020, (rd(pmu, 0x4020) & ~0xfu) | 0xf);
	for (unsigned us = 0; us < 100000 && (rd(pmu, 0x4024) & 0xf) != 0xf; us += 100) {
		delay(100);
	}
	status = rd(pmu, 0x4024);
	say("campwr: CAM domain: CONFIGURATION %#x STATUS %#x\n", rd(pmu, 0x4020), status);
	if ((status & 0xf) != 0xf) {
		return;
	}
	say("campwr: CMU_CAM BUS_USER PLL_CON0 %#x, DIV_CLK_CAM_BUSP %#x, QCH SYSREG_CAM %#x\n", rd(cmu, 0x100), rd(cmu, 0x1800), rd(cmu, 0x3050));
	if (!(rd(cmu, 0x100) & (1u << 4))) {
		wr(cmu, 0x100, rd(cmu, 0x100) | (1u << 4));
		delay(1000);
		say("campwr: BUS_USER mux set: PLL_CON0 %#x\n", rd(cmu, 0x100));
	}
	csis = ml_io_map(0x14400000UL, 0x1000);
	if (csis == 0) {
		return;
	}
	say("campwr: CSIS0 CSIS_VERSION %#x CMN_CTRL %#x DPHY_STATUS %#x\n", rd(csis, 0x0), rd(csis, 0x4), rd(csis, 0x20));
}

/*
 * The MIPI receiver for the IMX576's mode 0 (2880x2156, RAW10, 4 lanes, 2300 Mbps a lane, settle 0): Samsung's
 * csi_hw_* of fimc-is2/sensor/csi2/fimc-is-hw-csi-v5_1.c in the order fimc-is-device-csi.c calls them. D-PHY: PMU
 * 0x70c bit 0 isolation bypass, then reset bit 4 of the CAM sysreg word at 0x14510800 low and high (phy-exynos-mipi.c,
 * dphy_m4s4s4_csis). CSIS0 0x14400000: CMN_CTRL +4 (CSI_EN b0, SW_RESET b1, LANE_NUMBER b8-9, PHY_SEL b21, UPDATE_SHADOW
 * b16-19), ISP_CONFIG_CH0 +0x40 (VIRTUAL_CHANNEL b0-1, DATAFORMAT b2-7, PIXEL_MODE b12-13), ISP_RESOL_CH0 +0x44, PHY at
 * +0x700: STATUS, CMN_CTRL +0x704 (ENABLE_CLK b0, ENABLE_DAT b1-4, S_BYTE_CLK_ENABLE b7, S_CLKSETTLECTL b16-23, HSSETTLE
 * b24-31), BCTRL_0..3 +0x708.., SCTRL_0..3 +0x718..., SCTRL_4..6 +0x748....
 */
static vm_offset_t csis_base;

static void
csis_setup(vm_offset_t pmu)
{
	vm_offset_t sysreg = ml_io_map(0x14510000UL, 0x1000), cmu = ml_io_map(0x14500000UL, 0x4000);
	uint32_t v;

	csis_base = ml_io_map(0x14400000UL, 0x1000);
	if (sysreg == 0 || csis_base == 0 || cmu == 0) {
		say("campwr: CSIS blocks could not be mapped\n");
		csis_base = 0;
		return;
	}
	say("campwr: before: PMU 0x70c %#x, sysreg 0x800 %#x, CSIS CMN_CTRL %#x PHY_STATUS %#x\n", rd(pmu, 0x70c), rd(sysreg, 0x800),
	    rd(csis_base, 0x4), rd(csis_base, 0x700));
	wr(pmu, 0x70c, rd(pmu, 0x70c) | 1u);                    /* isolation bypass */
	wr(sysreg, 0x800, rd(sysreg, 0x800) & ~(1u << 4));      /* reset */
	delay(100);
	wr(sysreg, 0x800, rd(sysreg, 0x800) | (1u << 4));       /* released */
	delay(100);
	say("campwr: D-PHY isolation bypassed, reset released: PMU 0x70c %#x, sysreg 0x800 %#x, PHY_STATUS %#x\n", rd(pmu, 0x70c),
	    rd(sysreg, 0x800), rd(csis_base, 0x700));
	/* csi_hw_reset */
	wr(csis_base, 0x4, rd(csis_base, 0x4) | (1u << 1));
	for (unsigned us = 0; us < 1000 && (rd(csis_base, 0x4) & (1u << 1)); us += 10) {
		delay(10);
	}
	/* csi_hw_s_phy_default_value */
	wr(csis_base, 0x708, 0x1f4);
	wr(csis_base, 0x70c, 0x800);
	wr(csis_base, 0x710, 0x10001249);
	wr(csis_base, 0x714, 0x500002);
	wr(csis_base, 0x718, 0x9e003e00);
	wr(csis_base, 0x71c, 0x46);
	wr(csis_base, 0x720, 0x2c);
	wr(csis_base, 0x724, 0x200);
	wr(csis_base, 0x748, 0x3fea);
	wr(csis_base, 0x74c, 0xc0000);
	wr(csis_base, 0x750, 0x140);
	/* csi_hw_s_settle: HSSETTLE 0, S_CLKSETTLECTL 1 */
	v = rd(csis_base, 0x704);
	v = (v & ~(0xffu << 24)) | 0;
	v = (v & ~(0xffu << 16)) | (1u << 16);
	wr(csis_base, 0x704, v);
	/* csi_hw_s_phy_config: 2300 Mbps: deskew, coarse delay for 2-3 Gbps, byte clock */
	v = rd(csis_base, 0x718) | 1u;
	v &= ~(1u << 12);
	wr(csis_base, 0x718, v);
	wr(csis_base, 0x704, rd(csis_base, 0x704) | (1u << 7));
	/* csi_hw_s_lane: 4 lanes */
	wr(csis_base, 0x4, (rd(csis_base, 0x4) & ~(3u << 8)) | (3u << 8));
	wr(csis_base, 0x704, (rd(csis_base, 0x704) & ~(0xfu << 1)) | (0xfu << 1));
	/* csi_hw_s_config, VC0: virtual channel 0, RAW10 (0x2b), four pixels a clock (the DMA is not used yet); 2880 x 2156 */
	v = rd(csis_base, 0x40);
	v = (v & ~0x3u) | 0;
	v = (v & ~(0x3fu << 2)) | (0x2bu << 2);
	v = (v & ~(3u << 12)) | (2u << 12);
	wr(csis_base, 0x40, v);
	wr(csis_base, 0x44, (2156u << 16) | 2880u);
	/* csi_hw_enable: update shadow, PHY select 0, PHY clock on, CSI on */
	wr(csis_base, 0x4, rd(csis_base, 0x4) | (0xfu << 16));
	wr(csis_base, 0x4, rd(csis_base, 0x4) & ~(1u << 21));
	wr(csis_base, 0x704, rd(csis_base, 0x704) | 1u);
	wr(csis_base, 0x4, rd(csis_base, 0x4) | 1u);
	say("campwr: CSIS0 set up: CMN_CTRL %#x PHY_CMN_CTRL %#x ISP_CONFIG_CH0 %#x RESOL %#x PHY_STATUS %#x\n", rd(csis_base, 0x4),
	    rd(csis_base, 0x704), rd(csis_base, 0x40), rd(csis_base, 0x44), rd(csis_base, 0x700));
}

static void
csis_report(const char *when)
{
	if (csis_base == 0) {
		return;
	}
	say("campwr: CSIS0 %s: frame count VC0 %u VC1 %u, INT_SRC0 %#x INT_SRC1 %#x, PHY_STATUS %#x, CMN_CTRL %#x\n", when,
	    rd(csis_base, 0x100), rd(csis_base, 0x104), rd(csis_base, 0x14), rd(csis_base, 0x1c), rd(csis_base, 0x700), rd(csis_base, 0x4));
}

/*
 * campwr=8: CSIS0's test pattern into memory, as the Galaxy S9 port's cam_s9=4 does for CSIS1 (devices/starlte/CAMERA.md,
 * hakios.exynos_camera.c: the same CSIS v5.1 registers at the same offsets). The A50's blocks, from its device tree:
 * CSIS0 0x14400000, VC0's WDMA 0x14450000 with its DMA common at +0x400, the four CSISs' DMA common (arbitration, SRAM
 * split, test pattern generator) 0x14490000, the CAM SysMMU 0x14550000 (must be off: the WDMA writes physical addresses).
 * The buffer: 4 MB at 0xD8000000, outside XNU's RAM, below 4 GB (the S9's place; the display's scan-out buffers end below it,
 * UFS's own memory starts at 0xDA000000). No sensor, no D-PHY lanes: the generator feeds the DMA.
 */
#define PAT_W                   1440
#define PAT_H                   1080
#define PAT_PA                  0xD8000000ULL
#define PAT_BYTES               (4u << 20)
#define WDMA_PA                 0x14450000UL
#define CDMA_PA                 0x14490000UL
#define SYSMMU_PA               0x14550000UL
#define V_DMA_CTRL              0x000
#define V_DMA_FMT               0x004
#define V_DMA_ADDR(n)           (0x010 + 4 * (n))
#define V_DMA_ACT_CTRL          0x030
#define V_DMA_RESOL             0x040
#define VCMN                    0x400
#define VCMN_DATA_CTRL          0x00
#define VCMN_INT_MASK           0x04
#define VCMN_INT_SRC            0x08
#define VCMN_CMN_CTRL           0x14
#define VCMN_OTF_FORMAT         0x1C
#define VCMN_IRQ_MASK           0x00F11F00u
#define FRM_START0              (1u << 4)
#define FRM_END0                (1u << 8)
#define CD_ARB_PRI              0x00
#define CD_CTRL                 0x08
#define CD_SRAM_SPLIT           0x0C
#define CD_MATRIX               0x10
#define CD_CLK_CTRL             0x14
#define TP_ENABLE               0x20
#define TP_CTRL                 0x24
#define TP_RESOL                0x28
#define TP_ON                   (1u << 31)
#define TP_DUAL                 (1u << 30)
#define TP_VBLANK               100000u

static bool
wait_for(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t++) {
		if ((rd(base, off) & mask) == want) {
			return true;
		}
		delay(1000);
	}
	return (rd(base, off) & mask) == want;
}

static vm_offset_t other_wdma[2];

static void
pattern_stage(void)
{
	vm_offset_t pmu = ml_io_map(0x11860000UL, 0x5000), wdma = ml_io_map(WDMA_PA, 0x1000), cdma = ml_io_map(CDMA_PA, 0x1000);
	vm_offset_t mmu = ml_io_map(SYSMMU_PA, 0x1000), sysreg = ml_io_map(0x14510000UL, 0x1000), cmu = ml_io_map(0x14500000UL, 0x4000);
	vm_offset_t buf;
	uint32_t v, changed = 0, last = 0;
	bool ended;

	csis_base = ml_io_map(0x14400000UL, 0x1000);
	if (pmu == 0 || wdma == 0 || cdma == 0 || mmu == 0 || sysreg == 0 || csis_base == 0 || cmu == 0) {
		say("campwr: 8: a block could not be mapped\n");
		return;
	}
	say("campwr: 8: CAM domain %#x; BUS_USER %#x; CSIS0 version %#x CMN_CTRL %#x; WDMA CTRL %#x ACT_CTRL %#x; DMA common CTRL %#x "
	    "CLK_CTRL %#x; SysMMU MMU_CTRL %#x\n", rd(pmu, 0x4024), rd(cmu, 0x100), rd(csis_base, 0), rd(csis_base, 4), rd(wdma, V_DMA_CTRL),
	    rd(wdma, V_DMA_ACT_CTRL), rd(cdma, CD_CTRL), rd(cdma, CD_CLK_CTRL), rd(mmu, 0));
	{
		/* CMU_CAM's gates and Q-channels, as the boot left them: each said before it is read (a read that never comes back
		 * ends the log at its offset) */
		static const unsigned offs[] = { 0x2014, 0x2018, 0x201c, 0x2020, 0x2024, 0x2028, 0x202c, 0x2030, 0x2034, 0x2038, 0x203c, 0x2040,
		    0x2044, 0x300c, 0x3010, 0x3014, 0x3018, 0x301c, 0x3020, 0x3024, 0x3028, 0x302c, 0x3030, 0x3034, 0x3038, 0x303c,
		    0x3038, 0x303c };

		for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
			say("campwr: 8: CMU_CAM %#x: ", offs[i]);
			say("%#x\n", rd(cmu, offs[i]));
		}
	}
	if (rd(mmu, 0) & 1) {
		say("campwr: 8: the SysMMU translates (MMU_CTRL %#x): the WDMA's addresses would need a table; no DMA\n", rd(mmu, 0));
		return;
	}
	buf = hakios_physmap_wc(PAT_PA, PAT_BYTES);
	if (buf == 0) {
		say("campwr: 8: the buffer (%#llx, %u bytes) cannot be mapped\n", PAT_PA, PAT_BYTES);
		return;
	}
	memset((void *)buf, 0xA5, PAT_BYTES);
	for (vm_offset_t line = buf; line < buf + PAT_BYTES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
	/* the D-PHY as in csis_setup (isolation, reset) is not needed for the generator: the link is only reset and set up */
	wr(csis_base, 0x4, rd(csis_base, 0x4) | (1u << 1));
	if (!wait_for(csis_base, 0x4, 1u << 1, 0, 2)) {
		say("campwr: 8: CSIS0's reset does not finish (CMN_CTRL %#x)\n", rd(csis_base, 0x4));
		return;
	}
	wr(csis_base, 0x10, 0x011FFF1Fu);
	wr(csis_base, 0x18, 0x0000FFF0u);
	wr(csis_base, 0x14, rd(csis_base, 0x14));
	wr(csis_base, 0x1c, rd(csis_base, 0x1c));
	v = rd(csis_base, 0x4) & ~((3u << 8) | (3u << 10));
	wr(csis_base, 0x4, v | (3u << 8));
	wr(csis_base, 0x704, (rd(csis_base, 0x704) & ~(0xfu << 1)) | (0xfu << 1));
	/* VC0: RAW10, 2880x2156 would be the mode; PAT_W x PAT_H here, two pixels a clock; the other channels nothing */
	wr(csis_base, 0x40, (rd(csis_base, 0x40) & ~0x33ffu) | (1u << 12) | (0x2bu << 2) | 0);
	wr(csis_base, 0x44, ((uint32_t)PAT_H << 16) | PAT_W);
	for (unsigned c = 1; c < 4; c++) {
		wr(csis_base, 0x40 + 0x10 * c, (rd(csis_base, 0x40 + 0x10 * c) & ~0x33ffu) | (1u << 12) | (0x3fu << 2) | c);
		wr(csis_base, 0x44 + 0x10 * c, 0);
	}
	/* VC0's WDMA: 2D, RAW10 not packed, the width; its common: OTF in, RAW10 */
	wr(wdma, V_DMA_FMT, rd(wdma, V_DMA_FMT) & ~0x3fu);
	wr(wdma, V_DMA_RESOL, (rd(wdma, V_DMA_RESOL) & ~0x3fffu) | PAT_W);
	wr(wdma, VCMN + VCMN_OTF_FORMAT, rd(wdma, VCMN + VCMN_OTF_FORMAT) & ~3u);
	wr(wdma, VCMN + VCMN_DATA_CTRL, rd(wdma, VCMN + VCMN_DATA_CTRL) & ~1u);
	wr(wdma, VCMN + VCMN_INT_MASK, VCMN_IRQ_MASK);
	wr(wdma, VCMN + VCMN_INT_SRC, rd(wdma, VCMN + VCMN_INT_SRC));
	/* the DMA common as csi_hw_s_dma_common_dynamic leaves it for CSIS0 alone on the 9610 */
	wr(cdma, CD_ARB_PRI, (rd(cdma, CD_ARB_PRI) & ~0xfu) | (1u << 2) | 1u);
	wr(cdma, CD_CTRL, rd(cdma, CD_CTRL) | (1u << 1));
	wr(cdma, CD_SRAM_SPLIT, (rd(cdma, CD_SRAM_SPLIT) & ~0x03ff03ffu) | (0x280u << 16) | 0x280u);
	wr(cdma, CD_MATRIX, rd(cdma, CD_MATRIX) & ~0x1fu);
	say("campwr: 8: CSIS0 set up: CMN_CTRL %#x CH0 %#x %#x; WDMA FMT %#x RESOL %#x; DMA common ARB %#x CTRL %#x SPLIT %#x MATRIX %#x\n",
	    rd(csis_base, 4), rd(csis_base, 0x40), rd(csis_base, 0x44), rd(wdma, V_DMA_FMT), rd(wdma, V_DMA_RESOL), rd(cdma, CD_ARB_PRI),
	    rd(cdma, CD_CTRL), rd(cdma, CD_SRAM_SPLIT), rd(cdma, CD_MATRIX));
	v = rd(wdma, VCMN + VCMN_CMN_CTRL);
	if (v & 1) {
		wr(wdma, VCMN + VCMN_CMN_CTRL, v & ~1u);
	}
	/* the buffer to all eight frame pointers, the DMA on, the generator on for ONE frame */
	wr(wdma, V_DMA_CTRL, rd(wdma, V_DMA_CTRL) & ~(7u << 2));
	for (uint32_t i = 0; i < 8; i++) {
		wr(wdma, V_DMA_ADDR(i), (uint32_t)PAT_PA);
	}
	wr(wdma, V_DMA_CTRL, rd(wdma, V_DMA_CTRL) | (1u << 0) | (1u << 1));
	{
		/* the generator may feed another CSIS's DMA: CSIS1's and CSIS2's VC0 get the buffer too (their interrupt sources are looked at) */
		static const unsigned long others[2] = { 0x14460000UL, 0x14470000UL };

		for (unsigned o = 0; o < 2; o++) {
			vm_offset_t w = ml_io_map(others[o], 0x1000);

			if (w == 0) { continue; }
			wr(w, V_DMA_FMT, rd(w, V_DMA_FMT) & ~0x3fu);
			wr(w, V_DMA_RESOL, (rd(w, V_DMA_RESOL) & ~0x3fffu) | PAT_W);
			wr(w, VCMN + VCMN_OTF_FORMAT, rd(w, VCMN + VCMN_OTF_FORMAT) & ~3u);
			wr(w, VCMN + VCMN_DATA_CTRL, rd(w, VCMN + VCMN_DATA_CTRL) & ~1u);
			wr(w, VCMN + VCMN_INT_MASK, VCMN_IRQ_MASK);
			wr(w, VCMN + VCMN_INT_SRC, rd(w, VCMN + VCMN_INT_SRC));
			wr(w, V_DMA_CTRL, rd(w, V_DMA_CTRL) & ~(7u << 2));
			for (uint32_t i = 0; i < 8; i++) { wr(w, V_DMA_ADDR(i), (uint32_t)PAT_PA); }
			wr(w, V_DMA_CTRL, rd(w, V_DMA_CTRL) | (1u << 0) | (1u << 1));
			other_wdma[o] = w;
		}
	}
	wr(cdma, TP_CTRL, (rd(cdma, TP_CTRL) & ~0xffffffu) | (16u << 16) | (8u << 8) | 2u);
	wr(cdma, TP_RESOL, ((uint32_t)PAT_H << 16) | PAT_W);
	wr(cdma, TP_ENABLE, TP_DUAL | TP_VBLANK);
	wr(cdma, TP_ENABLE, rd(cdma, TP_ENABLE) | TP_ON);
	ended = wait_for(wdma, VCMN + VCMN_INT_SRC, FRM_END0, FRM_END0, 500);
	if (!ended && !(rd(wdma, VCMN + VCMN_INT_SRC) & FRM_START0)) {
		say("campwr: 8: no frame start with the link off: the pattern off, the link enabled (UPDATE_SHADOW, PHY_SEL 0, CSI_EN), on again\n");
		wr(cdma, TP_ENABLE, rd(cdma, TP_ENABLE) & ~TP_ON);
		delay(5000);
		wr(csis_base, 0x4, rd(csis_base, 0x4) | (0xfu << 16));
		wr(csis_base, 0x4, rd(csis_base, 0x4) & ~((1u << 21) | (1u << 20)));
		wr(csis_base, 0x4, rd(csis_base, 0x4) | 1u);
		wr(wdma, VCMN + VCMN_INT_SRC, rd(wdma, VCMN + VCMN_INT_SRC));
		wr(cdma, TP_ENABLE, rd(cdma, TP_ENABLE) | TP_ON);
		ended = wait_for(wdma, VCMN + VCMN_INT_SRC, FRM_END0, FRM_END0, 1500);
	} else if (!ended) {
		ended = wait_for(wdma, VCMN + VCMN_INT_SRC, FRM_END0, FRM_END0, 1500);
	}
	for (unsigned o = 0; o < 2; o++) {
		if (other_wdma[o] != 0) {
			say("campwr: 8: CSIS%u's WDMA INT_SRC %#x ACT_CTRL %#x\n", o + 1, rd(other_wdma[o], VCMN + VCMN_INT_SRC), rd(other_wdma[o], V_DMA_ACT_CTRL));
			wr(other_wdma[o], V_DMA_CTRL, rd(other_wdma[o], V_DMA_CTRL) & ~((1u << 0) | (1u << 1)));
		}
	}
	say("campwr: 8: %s: WDMA INT_SRC %#x ACT_CTRL %#x CTRL %#x; CSIS0 INT_SRC0 %#x INT_SRC1 %#x frames %u\n", ended ? "THE FRAME ENDED" : "no frame end",
	    rd(wdma, VCMN + VCMN_INT_SRC), rd(wdma, V_DMA_ACT_CTRL), rd(wdma, V_DMA_CTRL), rd(csis_base, 0x14), rd(csis_base, 0x1c), rd(csis_base, 0x100));
	wr(wdma, V_DMA_CTRL, rd(wdma, V_DMA_CTRL) & ~((1u << 0) | (1u << 1)));
	wr(cdma, TP_ENABLE, rd(cdma, TP_ENABLE) & ~TP_ON);
	delay(50000);
	for (vm_offset_t line = buf; line < buf + PAT_BYTES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
	for (uint32_t i = 0; i < PAT_BYTES; i += 8) {
		uint64_t w = *(const volatile uint64_t *)(buf + i);

		if (w == 0xA5A5A5A5A5A5A5A5ULL) {
			continue;
		}
		for (uint32_t k = 0; k < 8; k++) {
			if (((w >> (8 * k)) & 0xff) != 0xA5) {
				changed++;
				last = i + k + 1;
			}
		}
	}
	say("campwr: 8: the buffer: %u bytes changed, the last at %u (%u a line over %u lines)\n", changed, last, last / PAT_H, PAT_H);
	{
		const uint8_t *b = (const uint8_t *)buf;

		say("campwr: 8: first bytes: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x; at %u: %02x %02x %02x %02x\n",
		    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], PAT_W * 2,
		    b[PAT_W * 2], b[PAT_W * 2 + 1], b[PAT_W * 2 + 2], b[PAT_W * 2 + 3]);
	}
	if (last != 0) {
		say("campwr: 8: THE PATTERN REACHED MEMORY\n");
	}
}

static void
sensor_stage(uint32_t steps)
{
	vm_offset_t topcmu = ml_io_map(TOP_CMU_PA, 0x3000);
	uint8_t w[2] = { 0x00, 0x16 }, r[2] = { 0, 0 };
	uint32_t error;

	if (topcmu == 0) {
		say("campwr: CMU_TOP could not be mapped\n");
		return;
	}
	say("campwr: CMU_TOP CIS_CLK0: mux %#x div %#x gate %#x; pins: gpc0 CON %#x gpc2 CON %#x gpg1 CON %#x DAT %#x\n",
	    rd(topcmu, CIS0_MUX), rd(topcmu, CIS0_DIV), rd(topcmu, CIS0_GATE), rd(top, GPC0 + GPIO_CON),
	    rd(top, GPC2 + GPIO_CON), rd(top, GPG1 + GPIO_CON), rd(top, GPG1 + 4));
	if (!hakios_hsi2c_map(&cam, say)) {
		return;
	}
	if (steps < 4) {
		return;
	}
	(void)pmic_write(REG_LDO(3), LDO_ON | 0x2c);
	delay(2000);
	(void)pmic_write(REG_LDO(1), LDO_ON | 0x1c);
	delay(2000);
	(void)pmic_write(REG_LDO(6), LDO_ON | 0x28);
	delay(5000);
	/* the clock: OSCCLK, not divided, gate forced on; the pin to the clock */
	wr(topcmu, CIS0_MUX, rd(topcmu, CIS0_MUX) & ~1u);
	wr(topcmu, CIS0_DIV, rd(topcmu, CIS0_DIV) & ~0x1fu);
	wr(topcmu, CIS0_GATE, rd(topcmu, CIS0_GATE) | GATE_MANUAL | GATE_ON);
	pin_field(top + GPC2, GPIO_CON, 0, 2);
	delay(2000);
	/* the reset: an output, low, then high */
	pin_field(top + GPG1, GPIO_CON, 1, 1);
	wr(top, GPG1 + 4, rd(top, GPG1 + 4) & ~(1u << 1));
	delay(2000);
	wr(top, GPG1 + 4, rd(top, GPG1 + 4) | (1u << 1));
	delay(10000);
	say("campwr: supplies, clock and reset up: CIS_CLK0 mux %#x div %#x gate %#x\n", rd(topcmu, CIS0_MUX), rd(topcmu, CIS0_DIV),
	    rd(topcmu, CIS0_GATE));
	hakios_hsi2c_power(&cam);
	hakios_hsi2c_reset(&cam);
	error = hakios_hsi2c_message(&cam, 0x1a, false, w, 2, false);
	if (error == 0) {
		error = hakios_hsi2c_message(&cam, 0x1a, true, r, 2, true);
	}
	say("campwr: IMX576 at 0x1a: model id %02x%02x (0576 expected), error %#x\n", r[0], r[1], error);
	if (error == 0 && steps >= 5) {
		/* the vendor's tables (fw/imx576_2880x2156.regs: u16 register, u8 value; 289 of the global part, 84 of the mode's):
		 * written, streaming on, and the sensor's frame counter (register 0x0005) read twice */
		uint64_t size = 0;
		const uint8_t *regs = NULL;
		unsigned failed = 0, count;
		uint8_t f1 = 0, f2 = 0, mode = 0;

		for (unsigned wait = 0; wait < 600 && (regs = hakios_image_file("fw", "imx576_2880x2156.regs", &size, NULL)) == NULL; wait++) {
			delay(100000);
		}
		if (regs == NULL) {
			say("campwr: fw/imx576_2880x2156.regs is not in the image\n");
		} else {
			if (steps >= 7) {
				vm_offset_t pmu = ml_io_map(0x11860000UL, 0x5000);

				if (pmu != 0) { csis_setup(pmu); }
			}
			count = (unsigned)(size / 3);
			for (unsigned i = 0; i < count; i++) {
				uint16_t reg = (uint16_t)(regs[3 * i] | regs[3 * i + 1] << 8);

				if (sensor_write(reg, regs[3 * i + 2]) != 0) {
					if (failed++ < 4) { say("campwr: write of %#06x failed\n", reg); }
				}
			}
			say("campwr: %u registers written, %u failed\n", count, failed);
			csis_report("before streaming");
			/* Samsung's sensor_imx576_cis_stream_on(): group hold, 0x3040 and 0x3F71, mode_select, release */
			(void)sensor_write(0x0104, 1);
			(void)sensor_write(0x3040, 1);
			(void)sensor_write(0x3f71, 1);
			error = sensor_write(0x0100, 1);
			(void)sensor_write(0x0104, 0);
			delay(100000);
			csis_report("100 ms after streaming on");
			(void)sensor_read(0x0005, &f1);
			(void)sensor_read(0x0100, &mode);
			delay(50000);
			(void)sensor_read(0x0005, &f2);
			csis_report("150 ms after streaming on");
			{
				static const uint16_t regs_to_read[] = { 0x0114, 0x0112, 0x0113, 0x0340, 0x0341, 0x0342, 0x0343, 0x034c, 0x034d,
				    0x034e, 0x034f, 0x0306, 0x0307, 0x030e, 0x030f, 0x0820, 0x0821, 0x0822, 0x0823, 0x0136, 0x0137, 0x3040, 0x3f71 };
				char line[240];
				unsigned used = 0;

				for (unsigned i = 0; i < sizeof(regs_to_read) / sizeof(regs_to_read[0]); i++) {
					uint8_t b = 0xee;

					(void)sensor_read(regs_to_read[i], &b);
					used += (unsigned)snprintf(line + used, sizeof(line) - used, " %04x=%02x", regs_to_read[i], b);
				}
				say("campwr: sensor readback:%s\n", line);
				if (csis_base != 0) {
					vm_offset_t cmu = ml_io_map(0x14500000UL, 0x4000);

					say("campwr: CMU_CAM gates: ACLK_CSIS0 %#x GLUE_CSIS0 %#x QCH_CSIS0 %#x; CSIS CLK_CTRL %#x\n", rd(cmu, 0x2018),
					    rd(cmu, 0x202c), rd(cmu, 0x3020), rd(csis_base, 0x8));
				}
			}
			say("campwr: streaming on (error %#x): mode_select %#x, frame counter %u then %u (50 ms later)\n", error, mode, f1, f2);
			(void)sensor_write(0x0100, 0);
		}
	}
	if (error != 0) {
		for (uint8_t a = 0x08; a < 0x78; a++) {
			uint8_t b[2] = { 0, 0 };

			if (hakios_hsi2c_message(&cam, a, true, b, 1, true) == 0) {
				say("campwr: hsi2c_13: something answers at %#x\n", a);
			}
		}
	}
	/* back off */
	wr(top, GPG1 + 4, rd(top, GPG1 + 4) & ~(1u << 1));
	wr(topcmu, CIS0_GATE, rd(topcmu, CIS0_GATE) & ~(GATE_MANUAL | GATE_ON));
	(void)pmic_write(REG_LDO(6), 0x28);
	(void)pmic_write(REG_LDO(1), 0x1c);
	(void)pmic_write(REG_LDO(3), 0x2c);
	delay(5000);
	dump("sensor stage over");
}

/*
 * The Galaxy A50's camera supplies, from the S2MPB03 (0x56 on i2c_3): LDO1 RCAM1_DVDD 1.05 V, LDO2 FCAM_DVDD 1.05 V,
 * LDO3 CAM_VDDIO 1.8 V, LDO4 RCAM3_DVDD 1.2 V, LDO5 RCAM2_AVDD 2.8 V, LDO6 RCAM1_AVDD 2.8 V, LDO7 FCAM_AVDD 2.8 V.
 * The select field is 6 bits: LDO1/2/4 700 mV + 12.5 mV x n, LDO3 700 mV + 25 mV x n, LDO5-7 1800 mV + 25 mV x n
 * (drivers/regulator/s2mpb03.c). campwr=N: 1 read the chip and say the registers; 2 the rear camera's three supplies on
 * for two seconds (the sensor's own reset and clock are not touched), then off.
 */
static void
campwr_thread(__unused void *arg, __unused wait_result_t wr_)
{
	uint8_t id = 0;
	uint32_t steps = 0;

	(void)PE_parse_boot_argn("campwr", &steps, sizeof(steps));
	i2c = ml_io_map(PMIC_I2C_PA, 0x1000);
	top = ml_io_map(TOP_PA, 0x1000);
	if (i2c == 0 || top == 0) {
		say("campwr: a block could not be mapped\n");
		thread_terminate(current_thread());
		return;
	}
	chip = PMIC_ADDR;
	pin_field(top + GPP0, GPIO_CON, 6, 2);
	pin_field(top + GPP0, GPIO_CON, 7, 2);
	pin_field(top + GPP0, GPIO_PUD, 6, 0);
	pin_field(top + GPP0, GPIO_PUD, 7, 0);
	wr(i2c, IICSTAT, 0);
	wr(i2c, IICLC, LC_FILTER_ON | LC_SDA_DELAY5);
	wr(i2c, IICCON, bus_con());
	wr(i2c, IICSTAT, STAT_TXRXEN);
	{
		uint32_t error = pmic_read(REG_ID, &id);

		say("campwr: i2c_3 IICCON %#x IICSTAT %#x IICLC %#x; read of PMIC_ID: error %u\n", rd(i2c, IICCON), rd(i2c, IICSTAT),
		    rd(i2c, IICLC), error);
		if (error != 0) {
			/* who is on the bus: a one-byte read of register 0 from every address */
			for (uint32_t a = 0x08; a < 0x78; a++) {
				uint8_t b = 0;

				chip = a;
				if (bus_xfer(&b, 1, &b, 1) == 0) {
					say("campwr: i2c_3: a device answers at %#x (register 0 = %#x)\n", a, b);
				}
			}
			say("campwr: i2c_3: scan done\n");
			thread_terminate(current_thread());
			return;
		}
	}
	say("campwr: S2MPB03 PMIC_ID %#04x\n", id);
	dump("as found");
	if (steps == 8) {
		pattern_stage();
	} else if (steps == 6) {
		cam_block_stage();
	} else if (steps >= 3) {
		sensor_stage(steps);
	} else if (steps >= 2) {
		(void)pmic_write(REG_LDO(3), LDO_ON | 0x2c);    /* CAM_VDDIO 1.8 V */
		delay(2000);
		(void)pmic_write(REG_LDO(1), LDO_ON | 0x1c);    /* RCAM1_DVDD 1.05 V */
		delay(2000);
		(void)pmic_write(REG_LDO(6), LDO_ON | 0x28);    /* RCAM1_AVDD 2.8 V */
		delay(5000);
		dump("rear supplies on");
		delay_for_interval(2000, NSEC_PER_MSEC);
		(void)pmic_write(REG_LDO(6), 0x28);
		(void)pmic_write(REG_LDO(1), 0x1c);
		(void)pmic_write(REG_LDO(3), 0x2c);
		delay(5000);
		dump("rear supplies off again");
	}
	thread_terminate(current_thread());
}

static void
campwr_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("campwr", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (kernel_thread_start(campwr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, campwr_init);
