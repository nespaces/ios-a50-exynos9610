/*
 * The Galaxy A50's temperatures: the Exynos 9610's TMU (one block, 0x10070000, 0x700 bytes: device tree exynos9610-tmu), read by
 * the kernel itself, said in the log, published, and a stop for a phone that gets too hot. Nothing without boot-arg tmu9610=N:
 *   1  every tmu_poll ms (2000): each sensor's code and degrees in the log every tmu_log s (10) and when the hottest passes a
 *      multiple of 5 from 60; reads only (the block's clock gate, in CMU_PERI, is read first and a closed one is left alone)
 *   2  also kern.hakios.temp.max_mc (millidegrees) and .sensors (a string)
 *   3  also: the hottest sensor at tmu_stop degrees (default 95) three reads in a row hands the phone back to the loader (TWRP)
 * The codes are 9 bits, three to a register (0x40, 0x44, 0x48: as the S9's TMU, hakios.exynos_tmu.h, the same block layout of
 * Samsung's BSP), made degrees with each sensor's fuses (TRIMINFO(n) at 4 n: [8:0] the code at 25 degrees, [17:9] at 85; bit 23
 * of TRIMINFO(0): two-point calibration), as code_to_temp_with_sensorinfo() does. The device tree's zone names (BIG sensors
 * 0x4, LITTLE 0x2, G3D 0x1) are not relied on: every sensor is said, the hottest decides. Android itself gets the temperature
 * from the APM firmware (CONFIG_EXYNOS_ACPM_THERMAL); this is the BSP's own all_temp path.
 * The clock: CMU_PERI (0x10030000) BUSIF_TMU PCLK gate +0x2024, Q-channel +0x3004 (cmucal-sfr.c). Reads only; nothing is written.
 */
#include <stdint.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern void thread_deallocate(thread_t thread);
extern void loader_log_line(const char *line);
extern void loader_return(void);

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

#define TMU_PA                  0x10070000UL
#define CMU_PERI_PA             0x10030000UL
#define GATE                    0x2024u
#define QCH                     0x3004u
#define NSENSORS                8u
#define NOT_READ_MC             (-273000)

static vm_offset_t tmu, cmu;
static uint32_t steps, poll_ms = 2000, log_s = 10, stop_c = 95;
static int temp_max_mc = NOT_READ_MC, temp_reads;
static char temp_sensors[200] = "";

SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern_hakios, OID_AUTO, temp, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Exynos 9610's TMU (drivers/power/hakios.a50_tmu.c, tmu9610=2)");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, max_mc, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_max_mc, 0, "the hottest sensor, millidegrees (-273000: none read)");
SYSCTL_STRING(_kern_hakios_temp, OID_AUTO, sensors, CTLFLAG_RD | CTLFLAG_LOCKED, temp_sensors, 0, "every sensor in degrees");
SYSCTL_INT(_kern_hakios_temp, OID_AUTO, reads, CTLFLAG_RD | CTLFLAG_LOCKED, &temp_reads, 0, "how many times the sensors were read");

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static uint32_t
temp_reg(unsigned s)
{
	return s < 2 ? 0x40u : 0x40u + ((s - 2) / 3 + 1) * 4;
}

static unsigned
temp_shift(unsigned s)
{
	return s < 2 ? s * 16 : ((s - 2) % 3) * 9;
}

/* a code in millidegrees: two points (25 and 85) or one (the code at 25 degrees) */
static int
code_mc(uint32_t code, uint32_t trim, bool two_point)
{
	int e1 = (int)(trim & 0x1ff), e2 = (int)((trim >> 9) & 0x1ff);

	if (two_point && e2 != e1) {
		return 25000 + (int)(((int64_t)((int)code - e1) * 60000) / (e2 - e1));
	}
	return 25000 + ((int)code - e1) * 1000;
}

static void
tmu_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t gate, qch, control, status, trim0, strikes = 0, tick = 0;
	bool two_point;
	int warned = 0;

	cmu = ml_io_map(CMU_PERI_PA, 0x4000);
	tmu = ml_io_map(TMU_PA, 0x700);
	if (cmu == 0 || tmu == 0) {
		say("a50_tmu: a block could not be mapped\n");
		return;
	}
	gate = rd(cmu, GATE);
	qch = rd(cmu, QCH);
	say("a50_tmu: CMU_PERI BUSIF_TMU PCLK gate %#x (bit 21: on, 20: manual), Q-channel %#x\n", gate, qch);
	if ((gate & ((1u << 20) | (1u << 21))) == (1u << 20)) {
		say("a50_tmu: the gate is forced shut: the block is not touched\n");
		return;
	}
	control = rd(tmu, 0x20);
	status = rd(tmu, 0x28);
	trim0 = rd(tmu, 0);
	two_point = (trim0 >> 23) & 1;
	say("a50_tmu: CONTROL %#x (CORE_EN bit 0) STATUS %#x TRIMINFO0 %#x (%s-point)\n", control, status, trim0, two_point ? "two" : "one");
	if (!(control & 1)) {
		say("a50_tmu: the block is not running (CORE_EN clear): no temperatures\n");
		return;
	}
	for (;;) {
		int max_mc = NOT_READ_MC, mc[NSENSORS];
		char text[200];
		unsigned used = 0;

		for (unsigned s = 0; s < NSENSORS; s++) {
			uint32_t code = (rd(tmu, temp_reg(s)) >> temp_shift(s)) & 0x1ffu, trim = rd(tmu, s * 4);

			mc[s] = code != 0 ? code_mc(code, trim, two_point) : NOT_READ_MC;
			/* a sensor that does not exist or is off reads 0 or something outside any phone's range */
			if (mc[s] > -20000 && mc[s] < 130000 && mc[s] > max_mc) {
				max_mc = mc[s];
			}
			used += (unsigned)snprintf(text + used, sizeof(text) - used, "%s%u=%d.%d", s ? " " : "", s, mc[s] / 1000, (mc[s] < 0 ? -mc[s] : mc[s]) / 100 % 10);
		}
		temp_reads++;
		if (steps >= 2) {
			temp_max_mc = max_mc;
			strlcpy(temp_sensors, text, sizeof(temp_sensors));
		}
		if (tick == 0 || (tick * poll_ms) % (log_s * 1000) < poll_ms || (max_mc >= 60000 && max_mc / 5000 != warned)) {
			warned = max_mc / 5000;
			say("a50_tmu: %s degrees; hottest %d.%d\n", text, max_mc / 1000, max_mc / 100 % 10);
		}
		tick++;
		if (steps >= 3 && max_mc >= (int)stop_c * 1000) {
			if (++strikes >= 3) {
				say("a50_tmu: TOO HOT (%d.%d degrees at or over %u): handing the phone back to the loader\n", max_mc / 1000, max_mc / 100 % 10, stop_c);
				loader_return();
			}
		} else {
			strikes = 0;
		}
		delay_for_interval(poll_ms, NSEC_PER_MSEC);
	}
}

static void
a50_tmu_init(void)
{
	uint32_t value = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("tmu9610", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (PE_parse_boot_argn("tmu_poll", &value, sizeof(value)) && value >= 500 && value <= 60000) {
		poll_ms = value;
	}
	if (PE_parse_boot_argn("tmu_log", &value, sizeof(value)) && value >= 1 && value <= 3600) {
		log_s = value;
	}
	if (PE_parse_boot_argn("tmu_stop", &value, sizeof(value)) && value >= 50 && value <= 110) {
		stop_c = value;
	}
	if (kernel_thread_start(tmu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, a50_tmu_init);
