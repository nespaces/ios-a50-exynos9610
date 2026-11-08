/*
 * The GPU of the Galaxy S9 (starlte, Exynos 9810), a Mali-G72 MP18, brought up from the kernel step by step and then
 * handed to HakiosMali (drivers/gpu/hakios.mali.cpp), the IOKit service Mesa's panfrost draws through. Only with
 * boot-arg mali=N: every address below belongs to this phone, and nothing is touched without it.
 *
 * WHERE IT COMES FROM. This is HakiOS's Mali driver for the Infinix Hot 20 (X6826B: MediaTek MT6768, a Mali-G52 MC2),
 * kernel/bsd/dev/redwood_mali.c of github.com/oosexgang/hakios-mt6768 at commit 93ea250, under the Apple Public Source
 * License 2.0 as every file of that repository's kernel/ is (its NOTICE), and as this tree's drivers/ are. It is renamed
 * (redwood_ to hakios_, kern.redwood to kern.hakios) and MediaTek's power and clocks are replaced by the Exynos 9810's.
 * What is the original's: the GPU's reset and cores, address space 0 and the GPU's own page tables, the two first jobs,
 * Linux panfrost's parameters, the GPU addresses and what is remembered of each buffer, the queue of job chains, the
 * waits, the kept lines and the minute's report. On the X6826B it drew iOS's window server through Mesa's panfrost and a
 * Metal driver (that repository's README, "GPU: works"). Here it has not run.
 *
 * THE GPU. A Bifrost Mali needs no firmware: bringing it up is a supply, a clock and power, then its own registers,
 * which are Linux panfrost's (drivers/gpu/drm/panfrost/panfrost_regs.h) and the same on the G72 as on the G52 for all
 * that is used here.
 *   registers     0x17500000, 0x5000 bytes (the device tree's mali@17500000). GPU_ID >> 16 reads 0x6221: architecture
 *                 6.2, revision 2, product 1, the G72 ("tHEx"); Mesa's panfrost knows it by that number
 *   interrupts    SPI 282 job, 281 MMU, 283 GPU: GIC INTIDs 314, 313 and 315 on the GIC-400 (the S9+ port's device tree
 *                 has the same three). Not used: as the original did, the driver asks the registers (a moment of
 *                 polling, then asleep 200 us at a time)
 *   supply        the S2MPS18's BUCK6, vdd_g3d: B6CTRL (0x2A) [7:6] its on mode -- 10 on this phone, the G3D power
 *                 pin's (the device tree's g3d_en): the PMU's power-on of the domain turns it on -- and B6OUT (0x2B)
 *                 its voltage, 300 mV + 6.25 mV a step, 950 mV (0x68) the regulator's most. Reached only through ACPM
 *                 (drivers/core/hakios.exynos_acpm.c). S-BOOT LEAVES IT AT 644 mV WITH THE GPU AT 260 MHz, and on the
 *                 S9+ that is a combination the GPU does not work on: panfrost had DATA_INVALID_FAULTs every few seconds,
 *                 and two kernels whose panfrost reset and read the GPU before the rail was raised never came back (the
 *                 SoC hung). Step 2 raises B6OUT to mali_uv (950 mV by default), selected from the board's ASV table,
 *                 BEFORE THE GPU'S REGISTERS ARE FIRST TOUCHED, which is step 4
 *   power         the domain pd-embedded_g3d of the PMU (0x14060000): its configuration at 0x14064040, [3:0] = 0xF asks
 *                 for it on; its status at 0x14064044, [3:0] = 0xF when it is. A read of the GPU while it is off is a
 *                 bus hang, not an error
 *   clock         PLL_G3D of CMU_G3D (0x17400000, outside the domain: it keeps its settings when the domain is off).
 *                 PLL_CON0 at +0x140: S [2:0], P [13:8], M [25:16], MUX_SEL bit 4 (the G3D's clock from the PLL, not
 *                 the 26 MHz oscillator), MUX_BUSY bit 7, STABLE bit 29, ENABLE bit 31; 26 MHz x M / (P x 2^S). PLL_CON2
 *                 at +0x148 must hold 0x30000003 while the PLL locks (S-Boot's 0x10000001 keeps it from ever reaching
 *                 STABLE); PLL_LOCKTIME at +0x0, P x 150. The PLL is a PLL_1018X, and Samsung's CAL gives that kind
 *                 its limits (pll_spec.c): P 1-63, M 64-1023, S 0-6, 26 MHz / P from 2 to 8 MHz, the VCO (26 MHz x M
 *                 / P) from 600 to 1200 MHz. The rates this driver sets are three of the stock GPU table's six (the
 *                 phone's gpu_dvfs_table: 572, 546, 455, 338, 299, 260), the three the S9+ port runs: 260 MHz (M 160,
 *                 P 4, S 2: S-Boot's own, a VCO of 1040), 455 (M 140, P 4, S 1: 910) and 572 (M 176, P 4, S 1: 1144).
 *                 The S9+ port's own M/P/S for the two (455, 26, 0 and 286, 13, 0) put the VCO at 455 and 572 MHz,
 *                 below the PLL's 600, and 455's reference at 1 MHz, below its 2: the same rates, in the PLL's range
 *                 here. (The kernel's own PLL_G3D table, 860, 320 and 650 MHz, is in the range as well; the APM
 *                 firmware sets the GPU's rates under Android, and its table is not in the source.)
 *                 The G3D's own mux at +0x100 (bit 4: CMU's clock), and its bus dividers: MUX_CLK_G3D_BUSD +0x1000,
 *                 DIV_CLK_G3D_BUSD +0x1800, DIV_CLK_G3D_BUSP +0x1804 (bit 16 busy)
 *   coherency     none: the GPU does not look into the processors' caches. COHERENCY_ENABLE = 31 (none) after every
 *                 reset; the processor cleans its cache to memory around what the GPU reads and writes of the kernel's
 *                 (job pages, page tables), and the buffers of programs are write-combined, as in the original
 *
 * THE SEQUENCE, as the mainline Linux port to the S9+ runs it (panfrost from S-Boot's state, the same SoC and almost the
 * same board), and Samsung's CAL before it:
 *   the rail      B6OUT = mali_uv through ACPM, read back, 200 us; before anything else
 *   a rate        PLL_CON2 = 0x30000003; PLL_LOCKTIME = P x 150; PLL_CON0 MUX_SEL cleared and MUX_BUSY waited for (the
 *                 G3D on the oscillator first, as a step of its own: cleared together with ENABLE, the G3D is left with
 *                 no clock at all for a moment); ENABLE and the dividers cleared, then M/P/S, then ENABLE; STABLE (20
 *                 ms); MUX_SEL set and MUX_BUSY waited for again. Nothing, if the PLL already runs at the rate wanted
 *   power on      (only if the domain is off) PLL_CON2 = 0x30000003, PLL_CON0 |= MUX_SEL, +0x100 |= bit 4, the PMU's
 *                 configuration [3:0] = 0xF, its status waited for; then SMC 0x82000410 (1, 0x17410204, 0), the DTZPC
 *                 restore -- without it the first access to the GPU is refused by the bus, an SError -- 0 back for
 *                 success; then the bus dividers: +0x1000 = 0, +0x1800 = 0 (BUSD = the PLL's), +0x1804 = 3 (BUSP = BUSD
 *                 / 4), and the busy bit waited for. A domain the boot left on is used as it is: no SMC, no dividers
 *   power off     the five low-power settings of Samsung's pmucal_lpm_init (masked: PMU +0x4690 [1:0] = 2, +0x4680 bit
 *                 1 = 0, +0x4684 [11:0] = 0x363, +0x15A8 bit 0 = 1, +0x15AC bit 0 = 1) -- without them the S9+'s domain
 *                 stopped half off (STATUS 0x002F000E) and the next access was an SError --; SMC 0x82000410 (0,
 *                 0x17410204, 0), the DTZPC SAVE; the configuration [3:0] = 0 and the status waited for; then +0x100 and
 *                 PLL_CON0 MUX_SEL cleared and PLL_CON2 = 0x10000001. A domain that does not go off is switched on
 *                 again and its protection restored, and left on; one that never came on gets no save (its protection
 *                 controller is inside it), only the configuration [3:0] = 0
 * Under this port Carbon Hypervisor has EL2 and passes an SMC it does not know on to EL3.
 *
 * Sources. The S9+ port: github.com/SheroAbi/galaxy-s9plus-mainline-linux, src/soc/s9p-g3d.c (the rail, the rates, the
 * PLL's switch), src/soc/s9p-g3d-pd.c (the domain on and off, the SMCs, the dividers, the low-power settings),
 * src/soc/s9p-acpm.c (ACPM), src/soc/s9p-clkgate.c, src/mainline-dts/exynos9810-star2lte.dts (the GPU's, the domain's
 * and the clock's nodes) and its docs/04-drivers.md, 06-known-issues.md, 07-validation.md -- GPL-2.0 (the device tree
 * GPL-2.0 OR BSD-3-Clause): read for these facts, not one line copied. Samsung's GPL-2.0 kernel of the Exynos 9810
 * (github.com/ExyHyperBrick/android_kernel_samsung_exynos9810, branch lineage-24.0, forked to Scion-Project: the G3D's
 * CAL and power domain, cal-if.c, the S2MPS18's registers, the Mali's platform code) and Samsung's device tree (the
 * phone's own, and Silicium's starlte.dts), through a fact sheet. Linux panfrost (panfrost_gpu.c, panfrost_regs.h) for
 * what it does at a reset and at power-up.
 *
 * WHAT CHANGED FROM THE ORIGINAL, AND WHY
 *   - MediaTek's clock mux, SPM power domains, bus protection and MFG clock gate are gone; the Exynos 9810's sequence is
 *     steps 2 and 3, and step 1 looks before anything is written.
 *   - The original's steps ran all at once with mali=1; here mali=N makes the first N of nine, each said before it is
 *     made, and the domain is switched off again if the steps end short of serving and it was this driver that
 *     switched it on (with a soft reset first once the GPU has answered).
 *   - Its page tables assume 35 bits of GPU address (the G52's MMU_FEATURES, 0x2823). Step 4 reads the G72's and refuses
 *     to go on with fewer, or with fewer than 36 bits of physical address: the S9's memory reaches 0x8FFFFFFFF. On a GPU
 *     of 40 bits or more the walk is given a level 0 above them, as kbase and panfrost walk a G72 (the page tables).
 *   - Step 4 refuses a GPU that is not a G72, or whose shader cores do not fit the 32 bits of affinity the jobs are given.
 *   - COHERENCY_ENABLE = 31 after every reset, the one of a hung job's recovery included (a reset may clear it).
 *   - A reset is panfrost's: the reset interrupt cleared first, a hard reset when the soft one does not finish, and the
 *     one setting panfrost makes on a G72 after it: L2_MMU_CONFIG's limits on outstanding external reads and writes
 *     ([25:24], [27:26]) cleared. The cores come up as panfrost brings them: the L2 first and waited for, then the
 *     shader cores, then the tiler.
 *   - A physical address is checked to be memory the kernel manages (pmap_valid_address()) before the kernel's address
 *     of it is asked for and the cache cleaned over it -- a table of the GPU's, found through an entry -- and before a
 *     page is entered into the GPU's tables. phystokv() panics on an address outside, and PixOS, on another phone,
 *     panicked flushing the cache over an address it had not checked.
 *   - The page of the first jobs is taken out of the GPU's tables before it is freed (the original left its entry).
 *   - NR_CORE_GROUPS is the number of L2 caches present, as Linux panfrost answers it, not a fixed 1.
 *   - Programs' buffers get GPU addresses between 32 MB and 4 GB, given back when a buffer is freed, and a buffer of code
 *     never crosses a 16 MB line: Linux panfrost's range and rule, which Mesa's panfrost assumes (a Bifrost blend
 *     shader's address is 32 bits, its upper half the fragment shader's). The original gave 32 GB from 16 MB, never an
 *     address twice, whatever the buffer (hakios.mali_va.h, checked by tools/mali-va-test.sh).
 *   - The steps' lines go to the loader's log as they are said (say()), as this tree's drivers do: the end of a run
 *     that dies in a step is what is kept. The minute's report and the per-job lines stay in the kernel's log only.
 *   - Waits are bounded by the clock, not by a count of delay()s (which may each sleep far longer than asked).
 *
 * THE STEPS. mali=N makes the first N and stops; 0, or no mali= at all, leaves the GPU alone. A step says what it will
 * touch before it does, and one that does not find what it needs ends the sequence:
 *   1  reads   the PMU's G3D configuration and status; CMU_G3D (PLL_G3D and the rate it gives, the muxes, the bus
 *              dividers); through ACPM the S2MPS18's B6CTRL and B6OUT. Nothing is written; THE GPU IS NOT READ (the
 *              rail is as S-Boot left it)
 *   2  writes  B6OUT = mali_uv through ACPM, read back (or the steps end: the GPU is not touched at the boot's voltage);
 *              PLL_G3D to the rate (mali_mhz, 260 MHz unless said) by the switch above; then, if the domain is off,
 *              its power-on
 *   3  asks    if step 2 switched the domain on: EL3, through Carbon, for the DTZPC restore, SMC 0x82000410 (1,
 *              0x17410204, 0); then the bus dividers. A domain the boot left on: nothing
 *   4  reads   GPU_ID (a G72, or nothing more is done), MMU_FEATURES (35 bits of GPU address at least), the cores
 *   5  writes  a reset (soft; hard if that does not finish); COHERENCY_ENABLE = 31; L2_MMU_CONFIG's limits cleared
 *   6  writes  the L2, then the shader cores, then the tiler powered up
 *   7  runs    one job, the smallest there is (write a value into memory), through address space 0 in identity mode
 *   8  runs    the same job through the GPU's own page tables, at a GPU address of their making
 *   9  serves  HakiosMali started: programs can open the GPU (the original's "step 6 of 6" and RedwoodMaliStart())
 * sysctl kern.hakios.mali says the last step done: 9 is the GPU there for programs.
 *
 * Boot-args:
 *   mali=N           the first N of the GPU's nine steps of bring-up (9: all, then served); 0 or none: left alone
 *   mali_mhz=MHZ     the GPU's clock: 260 (S-Boot's, the default), 455 or 572, the S9+ port's three rates
 *   mali_pll=0       PLL_G3D left as the boot left it (the step stops if it does not run)
 *   mali_acpm=0      the rail not raised: the GPU is touched at the boot's voltage, which hung the S9+ (a test only)
 *   mali_uv=UV       BUCK6 target in microvolts, 300000..950000 in 6250-uV steps (default 950000).
 *                    Select from this phone's stock ASV table; our SM-G965F's maximum is 750000.
 *   mali_fastmap=0   the GPU walks its tables through its own caches, and every buffer entered is an UPDATE of AS 0
 *   mali_trace=1     a line for every job: "tl: j SEQ f|v q QUEUED b BEGAN e ENDED", in microseconds
 *   mali_gov=1       the GPU's clock follows its load (gov_window()): 260, 455 or 572 MHz, the lowest at which the
 *                    last 16 ms would have kept it busy half the time at most -- up at once, down after five such
 *                    windows in a row --, and no more than 455 from 85 C on the G3D's sensor, 260 from 95 (tmu=1 or
 *                    more; 5 C back). The rail stays at step 2's selected voltage for all three rates. Needs
 *                    mali_pll and mali_acpm (both on by default)
 *   mali_nap_us=US   a job not done after its first 200 us of polling is looked at every US microseconds (50 unless
 *                    said; 200 before): the GPU's interrupts are not used, and a job's end was seen 100 us late on
 *                    average, a frame of CoreAnimation's being a dozen jobs and more
 *
 * The user client is opened by the window server's process, backboardd, which does not run as root (PixOS learned that
 * on another phone): nothing here or in hakios.mali.cpp asks for root.
 *
 * Status: written, not yet run on the phone. The S9+ port's sequence has run panfrost on the same SoC from S-Boot's
 * state; what is ours (the steps, the jobs, the tables) has run only on the X6826B's G52.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

#include "hakios.mali_va.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);                       /* pmap_paddr_t */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */
extern vm_offset_t phystokv(uint64_t pa);
extern boolean_t pmap_valid_address(uint64_t addr);             /* memory the kernel manages */
extern int hakios_scanout_carved(uint64_t phys);                /* the DPU's pool (hakios.exynos_dpu.c; stubs.c) */
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern void HakiosMaliStart(void);                              /* drivers/gpu/hakios.mali.cpp */
extern uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;   /* hakios.mali.cpp */
extern void HakiosMaliCacheSecond(void);
extern struct mali_made { uint64_t address, size; uint32_t asked, flags; } gMaliMade[32];

/* (every line of the steps to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
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

#define STARLTE_SOC_BASE        0x10000000UL    /* where the loader's platform "starlte" says the SoC's registers begin */

/*
 * The SoC's addresses: the Exynos 9810's (the S9) unless boot-arg mali_soc=9610 (the Galaxy A50: Mali-G72 MP3 at
 * 0x11500000, PMU 0x11860000 with pd-g3d at +0x4060/+0x4064, CMU_G3D 0x11430000 with PLL_G3D's CON0 at +0x120 in the
 * same field layout, the DTZPC at 0x11410204: Samsung's 4.14 kernel for it, exynos9610.dts, pmucal_local_exynos9610.h,
 * cmucal-sfr.c; the port's analysis notes). On the 9610 only step 1 is made so far: the rest (ACPM, the PMIC's BUCK5, the
 * PLL's 1052X rates) is the 9810's and not yet the A50's.
 */
static unsigned long mali_pmu_phys = 0x14060000UL, mali_cmu_phys = 0x17400000UL, mali_gpu_phys = 0x17500000UL;
static uint32_t mali_pmu_cfg = 0x4040, mali_pmu_status = 0x4044, mali_pll_con0 = 0x140, mali_pll_con2 = 0x148;
static uint32_t mali_dtzpc = 0x17410204u, mali_soc = 9810, mali_peek, mali_mux_busd = 0x1000;
#define PMU_PHYS                mali_pmu_phys   /* the PMU */
#define PMU_SIZE                0x5000
#define PMU_G3D_CONFIGURATION   mali_pmu_cfg    /* 0x14064040: [3:0] = 0xF asks for pd-embedded_g3d on, 0 off */
#define PMU_G3D_STATUS          mali_pmu_status /* 0x14064044: [3:0] = 0xF when it is on */
#define G3D_ON                  0xfu
#define G3D_WAIT_US             100000u         /* (the S9+ port waits 10 ms) */
/* the G3D's low-power settings of Samsung's pmucal_lpm_init, made before the domain is switched off: offset, mask, value */
static const struct { uint32_t at, mask, value; const char *name; } g3d_lpm[] = {
	{ 0x4690, 0x3u, 0x2u, "MEMORY_EMBEDDED_G3D_OPTION" },
	{ 0x4680, 0x2u, 0x0u, "PWR_EMBEDDED_G3D_OPTION" },
	{ 0x4684, 0xfffu, 0x363u, "PWR_EMBEDDED_G3D_DURATION" },
	{ 0x15a8, 0x1u, 0x1u, "SCI_CRPPORTPWRDN" },
	{ 0x15ac, 0x1u, 0x1u, "PWR_EMBEDDED_G3D" },
};

#define CMU_G3D_PHYS            mali_cmu_phys
#define CMU_G3D_SIZE            0x2000          /* the bus dividers are at +0x1800 */
#define PLL_LOCKTIME            0x000
#define MUX_G3D_USER            0x100           /* PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_USER: MUX_SEL set on, cleared off */
#define PLL_CON0                mali_pll_con0   /* PLL_G3D */
#define PLL_CON2                mali_pll_con2           /* PLL_CON2_LOCK while the PLL locks and the domain is on, _OFF after */
#define MUX_CLK_G3D_BUSD        mali_mux_busd   /* 0x1000 on the 9810, 0x1004 on the 9610 */
#define DIV_CLK_G3D_BUSD        0x1800
#define DIV_CLK_G3D_BUSP        0x1804
#define CLK_CON_BUSY            (1u << 16)
#define MUX_SEL                 (1u << 4)       /* PLL_CON0: the PLL's output, not the oscillator; +0x100: CMU's clock */
#define MUX_BUSY                (1u << 7)       /* PLL_CON0: the mux still moving */
#define PLL_ENABLE              (1u << 31)
#define PLL_STABLE              (1u << 29)
#define PLL_PMS                 ((0x3ffu << 16) | (0x3fu << 8) | 7u)
#define PLL_CON2_LOCK           0x30000003u
#define PLL_CON2_OFF            0x10000001u
#define PLL_FIN_KHZ             26000ull
#define PLL_LOCK_FACTOR         150u            /* PLL_LOCKTIME = P x 150 */
#define PLL_STABLE_US           20000u
#define MUX_BUSY_US             1000u
/*
 * the rates (mali_mhz): the S9+ port's three, M, P, S on the 26 MHz oscillator, each inside PLL_1018X's limits (26 / P
 * from 2 to 8 MHz, 26 x M / P from 600 to 1200 MHz: Samsung's pll_spec.c)
 */
static const struct g3d_rate { uint32_t mhz, m, p, s; } g3d_rates[] = {
	{ 260, 160, 4, 2 },                     /* S-Boot's own: the VCO at 1040 MHz */
	{ 455, 140, 4, 1 },                     /* 910 */
	{ 572, 176, 4, 1 },                     /* 1144 */
	/* the A50's (PLL_1052X, S 0 as Samsung's own pll_g3d rows: 26 / 13 = 2 MHz, VCO 764): its fvmap's 764 MHz, 775 mV */
	{ 764, 382, 13, 0 },
	/* its fvmap's 683 MHz, 737.5 mV: VCO 1366 MHz, S 1 as Samsung's 550 MHz row (M 550, P 13, S 1) */
	{ 683, 683, 13, 1 },
};
static uint32_t mali_mhz = 260;
static bool pll_quiet;                          /* the governor's switches: a line only when one fails */
static uint32_t mali_pll = 1;                   /* mali_pll=0: PLL_G3D as the boot left it */

#define DTZPC_SMC               0x82000410u     /* Samsung's SiP call, SMC_CMD_PREPARE_PD_ONOFF */
#define DTZPC_SAVE              0u              /* before the domain's power goes */
#define DTZPC_RESTORE           1u              /* after it has come */
#define DTZPC_G3D               mali_dtzpc

#define MALI_PHYS               mali_gpu_phys
#define MALI_SIZE               0x5000
#define MALI_G72                0x6221u         /* GPU_ID >> 16 */

SYSCTL_DECL(_kern_hakios);              /* the node is drivers/core/hakios.platform.c's */

static vm_offset_t pmu, cmu_g3d, mali;
static uint32_t reached, gpu_id, shader_mask, l2_mask, last_step, step;
static bool powered_here;               /* step 2 switched the domain on: off again if the steps end short */
static bool gpu_answered;               /* step 4 read a G72: a soft reset before the power goes */
/*
 * The supply, through ACPM (drivers/core/hakios.exynos_acpm.c): the S2MPS18's BUCK6 (vdd_g3d). Only B6OUT is ever
 * written, never above 0x68 (950 mV, the regulator's most in the phone's device tree), and read back; B6CTRL is read,
 * for the log. Put back as found when this driver switches the domain off again. Boot-arg mali_acpm=0: none of it.
 */
static uint8_t mali_rail_ctrl = 0x2a, mali_rail_out = 0x2b, mali_rail_most = 0x68;
#define B6CTRL                  mali_rail_ctrl  /* 9610: S2MPU09 BUCK5 CTRL 0x20 */ /* was 0x2a */
#define B6CTRL_9810             0x2a            /* [7:6]: 11 always on, 10 the G3D power pin's (g3d_en), 00 off */
#define B6OUT                   mali_rail_out   /* 9610: BUCK5 OUT 0x21 */ /* was 0x2b */
#define B6OUT_9810              0x2b            /* 300 mV + 6.25 mV a step */
#define B6OUT_950MV             0x68
#define B6OUT_MOST              mali_rail_most  /* 9610: 0x6f, 993.75 mV, the top of its fvmap */
#define RAIL_SETTLE_US          200
#define RAIL_TRIES              10              /* 100 ms apart: the APM may not answer at once */
static uint32_t mali_acpm = 1;
static uint32_t mali_uv = 950000;
static uint8_t b6out_target = B6OUT_950MV;
static bool acpm_ok, rail_changed, b6out_known;  /* b6out_known: B6OUT was read before it was written */
static uint8_t b6out_found;

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the Mali came up: 9 is the GPU there for programs (drivers/gpu/hakios.mali.c)");
/*
 * What a program said about the GPU (the original's userland/src/mali_test.c writes it): repeated in the log once a
 * minute, so that it lands in the end of a run's log, which is the part that is kept.
 */
static uint32_t user_said;
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_user, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &user_said, 0,
    "what user space found: 1 a job ran, 2 no service, 3 no client, 4 no buffer, 5 no mapping, 6 the job did nothing");

/*
 * What gl_test drew, for the log's end as well: who drew it (1 the Mali, 2 the processor) times 1000 plus the share of
 * the picture inside the triangle in percent; and the middle pixel, red in the low byte.
 */
/*
 * kern.hakios.say: a line from a program, into the kernel's log (a job's own output is kept nowhere):
 * sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line)).
 */
#define KEPT_LINES 96
static char kept_lines[KEPT_LINES][200];
static uint32_t kept_next;
static LCK_GRP_DECLARE(kept_lock_group, "hakios-said");
static LCK_MTX_DECLARE(kept_lock, &kept_lock_group);

/* A line from the kernel itself, kept and said again every minute as kern.hakios.say's "!" ones are. */
void
hakios_keep(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	lck_mtx_lock(&kept_lock);
	strlcpy(kept_lines[kept_next % KEPT_LINES], line, sizeof(kept_lines[0]));
	kept_next++;
	lck_mtx_unlock(&kept_lock);
}

static int
say_line(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char line[512];
	size_t length = req->newlen;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length > sizeof(line) - 1) {
		length = sizeof(line) - 1;
	}
	if (SYSCTL_IN(req, line, length) != 0) {
		return EINVAL;
	}
	line[length] = 0;
	printf("%s%s", line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
	if (line[0] == '!') {                   /* kept, and said again every minute (mali_thread) */
		lck_mtx_lock(&kept_lock);
		strlcpy(kept_lines[kept_next % KEPT_LINES], line + 1, sizeof(kept_lines[0]));
		kept_next++;
		lck_mtx_unlock(&kept_lock);
	}
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, say, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    say_line, "A", "");

static uint32_t gl_said, gl_middle;
/*
 * sysctl kern.hakios.bind_cpu=N: the calling thread runs on logical processor N from now on (-1: on any). The
 * scheduler of this kernel does not tell big cores from small ones, and on the X6826B CoreAnimation's render thread
 * spent 14 to 18 ms a frame wherever it happened to be (the original's run 147, 26 October). The Metal driver puts
 * that thread and its own worker on the big cores.
 */
extern void *cpu_to_processor(int cpu);
extern void *thread_bind(void *processor);
extern int thread_block(void *continuation);
extern unsigned int real_ncpus;

/* Kernel callers use the same validated placement operation as the existing
 * per-calling-thread sysctl. This keeps private scheduler declarations out of
 * the IOKit framebuffer's exported header environment. */
int
hakios_mali_bind_current_cpu(int cpu)
{
	if (cpu < -1 || cpu >= (int) real_ncpus) return EINVAL;
	void *processor = cpu < 0 ? NULL : cpu_to_processor(cpu);
	if (cpu >= 0 && processor == NULL) return EINVAL;
	(void) thread_bind(processor);
	(void) thread_block(NULL);
	return 0;
}

static int
bind_cpu(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int cpu = -1, error;

	error = SYSCTL_IN(req, &cpu, sizeof(cpu));
	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	return hakios_mali_bind_current_cpu(cpu);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, bind_cpu, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, NULL, 0,
    bind_cpu, "I", "bind the calling thread to a logical processor");

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gl, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_said, 0, "");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gl_middle, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, &gl_middle, 0, "");
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id, 0, "GPU_ID");

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

/* Until (register & mask) == want, for at most `us` microseconds by the clock (delay() may sleep far longer than asked). */
static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while ((rd(base, at) & mask) != want) {
		if (mach_absolute_time() >= deadline) {
			return (rd(base, at) & mask) == want;
		}
	}
	return true;
}

/* `us` microseconds, by the clock */
static void
pause_us(uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while (mach_absolute_time() < deadline) {
	}
}

/* ---- the GPU's registers (Linux drivers/gpu/drm/panfrost/panfrost_regs.h) ---- */

#define GPU_ID              0x000
#define GPU_L2_FEATURES     0x004
#define GPU_CORE_FEATURES   0x008
#define GPU_TILER_FEATURES  0x00c
#define GPU_MEM_FEATURES    0x010
#define GPU_MMU_FEATURES    0x014       /* [7:0] bits of GPU address, [15:8] bits of physical address */
#define GPU_AS_PRESENT      0x018
#define GPU_JS_PRESENT      0x01c
#define GPU_INT_RAWSTAT     0x020
#define GPU_INT_CLEAR       0x024
#define GPU_INT_MASK        0x028
#define GPU_CMD             0x030
#define THREAD_MAX_THREADS  0x0a0
#define SHADER_PRESENT      0x100
#define SHADER_PRESENT_HI   0x104
#define TILER_PRESENT       0x110
#define L2_PRESENT          0x120
#define SHADER_READY        0x140
#define TILER_READY         0x150
#define L2_READY            0x160
#define SHADER_PWRON        0x180
#define TILER_PWRON         0x190
#define L2_PWRON            0x1a0
#define COHERENCY_FEATURES  0x300
#define COHERENCY_ENABLE    0x304
#define COHERENCY_NONE      31u
#define THREAD_TLS_ALLOC    0x310
#define IRQ_RESET_COMPLETED (1u << 8)
#define GPU_CMD_SOFT_RESET  1u
#define GPU_CMD_HARD_RESET  2u
#define GPU_JM_CONFIG       0xf00
#define GPU_SHADER_CONFIG   0xf04
#define GPU_TILER_CONFIG    0xf08
#define GPU_L2_MMU_CONFIG   0xf0c
#define L2_MMU_LIMIT_EXTERNAL (0xfu << 24)    /* outstanding external reads [25:24], writes [27:26]: 0, no limit */
#define RESET_WAIT_US       100000
#define CORES_WAIT_US       50000

#define JOB_INT_RAWSTAT     0x1000
#define JOB_INT_CLEAR       0x1004
#define JOB_INT_MASK        0x1008
#define JS1                 0x1880      /* job slot 1, where compute-like jobs go */
#define JS_STATUS           0x24
#define JS_HEAD_NEXT_LO     0x40
#define JS_HEAD_NEXT_HI     0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT      0x58
#define JS_COMMAND_NEXT     0x60
#define JS_CONFIG           ((3u << 8) | (3u << 12) | (8u << 16))  /* AS 0, caches flushed both ends */

#define AS0                 0x2400
#define AS_TRANSTAB_LO      0x00
#define AS_TRANSTAB_HI      0x04
#define AS_MEMATTR_LO       0x08
#define AS_MEMATTR_HI       0x0c
#define AS_COMMAND          0x18
#define AS_FAULTSTATUS      0x1c
#define AS_STATUS           0x28
#define AS_ACTIVE           1u
#define AS_WAIT_US          50000
#define AS_TRANSCFG_LO      0x30
#define AS_TRANSCFG_HI      0x34

/*
 * A reset as Linux panfrost makes one (panfrost_gpu_soft_reset(), panfrost_gpu_init_quirks()): interrupts masked, the
 * reset's own interrupt cleared, a soft reset, and a hard one (GPU_CMD = 2) when that does not finish; everything
 * cleared. Then what a reset clears put back: COHERENCY_ENABLE, which says the GPU is to expect no coherency from the
 * rest of the chip (the original left it as the reset made it), and the one setting panfrost makes on a G72 after a
 * reset: L2_MMU_CONFIG's limits on outstanding external reads and writes cleared (the G72 has the two-bit fields).
 * False if no reset finished.
 */
static bool
gpu_reset(void)
{
	wr(mali, GPU_INT_MASK, 0);
	wr(mali, GPU_INT_CLEAR, IRQ_RESET_COMPLETED);
	wr(mali, GPU_CMD, GPU_CMD_SOFT_RESET);
	if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, RESET_WAIT_US)) {
		say("mali: the soft reset did not finish (GPU_INT_RAWSTAT %#x): a hard reset\n", rd(mali, GPU_INT_RAWSTAT));
		wr(mali, GPU_CMD, GPU_CMD_HARD_RESET);
		if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, RESET_WAIT_US)) {
			return false;
		}
	}
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, COHERENCY_ENABLE, COHERENCY_NONE);
	wr(mali, GPU_L2_MMU_CONFIG, rd(mali, GPU_L2_MMU_CONFIG) & ~L2_MMU_LIMIT_EXTERNAL);
	return true;
}

/*
 * The cores, as panfrost powers them (panfrost_gpu_power_on()): the L2 and waited for, then the shader cores, then the
 * tiler. False, and which did not come, if one did not.
 */
static bool
gpu_cores_on(const char **which)
{
	uint32_t tiler = rd(mali, TILER_PRESENT);

	wr(mali, L2_PWRON, l2_mask);
	if (!wait_for(mali, L2_READY, l2_mask, l2_mask, CORES_WAIT_US)) {
		*which = "the L2";
		return false;
	}
	wr(mali, SHADER_PWRON, shader_mask);
	if (!wait_for(mali, SHADER_READY, shader_mask, shader_mask, CORES_WAIT_US)) {
		*which = "the shader cores";
		return false;
	}
	wr(mali, TILER_PWRON, tiler);
	if (!wait_for(mali, TILER_READY, tiler, tiler, CORES_WAIT_US)) {
		*which = "the tiler";
		return false;
	}
	return true;
}

/* ---- one job ---- */

/*
 * The smallest job there is, in a page of the kernel's own: a job header saying "write value", and its payload --
 * where, what kind of value (a 32-bit immediate), and the value -- the "where" being the same page 0x800 further on.
 * `gpu` is the address the GPU knows the page by: its physical address in identity mode, or wherever the page tables
 * put it. The processor's caches are cleaned either side, since the GPU here does not look into them.
 */
static bool
run_write_job(vm_offset_t page, uint64_t gpu, const char *what)
{
	volatile uint32_t *job = (volatile uint32_t *)page, *target = (volatile uint32_t *)(page + 0x800);
	bool ok;

	bzero((void *)page, PAGE_SIZE);
	job[4] = (2u << 1) | 1u | (1u << 16);   /* write value; 64-bit pointers; job index 1 */
	job[8] = (uint32_t)(gpu + 0x800);
	job[9] = (uint32_t)((gpu + 0x800) >> 32);
	job[10] = 6;                            /* a 32-bit immediate */
	job[12] = 0xc0ffee01u;
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	wr(mali, JOB_INT_MASK, 0);
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	wr(mali, JS1 + JS_HEAD_NEXT_LO, (uint32_t)gpu);
	wr(mali, JS1 + JS_HEAD_NEXT_HI, (uint32_t)(gpu >> 32));
	wr(mali, JS1 + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, JS1 + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, JS1 + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, JS1 + JS_COMMAND_NEXT, 1);     /* start */
	(void)wait_for(mali, JOB_INT_RAWSTAT, (1u << 1) | (1u << 17), 1u << 1, 100000);

	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	ok = *target == 0xc0ffee01u;
	say("mali: %u: %s: interrupts %#x, slot status %#x, MMU fault %#x, job status %#x, the word %#x%s\n", step, what,
	    rd(mali, JOB_INT_RAWSTAT), rd(mali, JS1 + JS_STATUS), rd(mali, AS0 + AS_FAULTSTATUS), job[0], *target,
	    ok ? " -- the GPU ran it" : " -- NOT WRITTEN");
	wr(mali, JOB_INT_CLEAR, 0xffffffffu);
	return ok;
}

static void
as0_update(uint64_t transtab, uint32_t transcfg)
{
	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);   /* no command while one runs (panfrost) */
	wr(mali, AS0 + AS_TRANSTAB_LO, (uint32_t)transtab);
	wr(mali, AS0 + AS_TRANSTAB_HI, (uint32_t)(transtab >> 32));
	wr(mali, AS0 + AS_TRANSCFG_LO, transcfg);
	wr(mali, AS0 + AS_TRANSCFG_HI, 0);
	wr(mali, AS0 + AS_MEMATTR_LO, 0x88888888u);     /* every attribute: write-back, the GPU's own caches */
	wr(mali, AS0 + AS_MEMATTR_HI, 0x88888888u);
	wr(mali, AS0 + AS_COMMAND, 1);                  /* update */
	(void)wait_for(mali, AS0 + AS_STATUS, AS_ACTIVE, 0, AS_WAIT_US);
}

/* ---- the GPU's own page tables ---- */

/*
 * The format Linux's panfrost gives a Bifrost GPU: ordinary AArch64 stage-1 tables with 4 KB pages (AS_TRANSCFG mode
 * 6). They translate 35 bits of address (the G52's MMU_FEATURES were 0x2823; step 4 checks the G72 has at least as
 * many), which makes three levels: the top table (pt_top) is indexed by bits 34..30, then 29..21, then 20..12. A page's
 * entry is its address, the access flag, inner shareable, readable and writable (AP 01), memory attribute 0, and the
 * low bits 11 ("page").
 *
 * Where the GPU's walk starts. On the G52 the GPU itself had 35 bits of address, AS_TRANSCFG's INA_BITS [10:6] said 55 -
 * 35 = 20, and the walk began at pt_top. A G72 is expected to have 48 (the G71's and G72's MMU_FEATURES read 0x2830
 * elsewhere), and on it both of Linux's drivers walk four levels from level 0: Samsung's kbase always (its tables start
 * at MIDGARD_MMU_TOPLEVEL 0, INA_BITS left 0), panfrost with INA_BITS = 55 - 48. Neither has the GPU start at level 1 on
 * a GPU of 48 bits, so nothing says that a G72 honours INA_BITS = 20 that way. On a GPU of more than 39 bits, then, the
 * tables get the level 0 that both drivers give it: one more page (pt_root), its entry 0 pointing at pt_top (every GPU
 * address here is below 2^35, so its other 511 stay empty), AS_TRANSTAB at it, and INA_BITS = 55 - the GPU's bits, as
 * panfrost writes. pt_transtab is what AS_TRANSTAB holds, either way.
 */
#define PTE_TABLE           3ull
#define PTE_PAGE            3ull
#define PTE_AF              (1ull << 10)
#define PTE_SH_INNER        (3ull << 8)
#define PTE_AP_RW           (1ull << 6)
#define PTE_ADDRESS         0x0000fffffffff000ull
#define GPU_VA_BITS         35u
#define GPU_PA_BITS_NEEDED  36u         /* the S9's memory: 0x80000000-0xFFFFFFFF and 0x880000000-0x8FFFFFFFF */
#define TRANSCFG_AARCH64    6u                          /* [3:0]: AArch64 tables, 4 KB pages (2 is identity) */
#define TRANSCFG_INA(bits)  ((55u - (bits)) << 6)       /* [10:6]: 55 - the bits of input address */
#define TRANSCFG_WALK_WB    ((2u << 24) | (1u << 30))   /* the walk's reads write-back, read-allocate (kbase's) */
#define TRANSCFG_WALK_NC    (1u << 24)                  /* ... not cached */
#define PT_FOUR_LEVELS_BITS 40u                         /* from here on, a GPU's walk is given level 0 as well */
/*
 * Boot-arg mali_fastmap (1 unless =0): the GPU walks its tables without caching them, so an entry the processor has
 * written (and cleaned to memory) is what the next walk reads -- and a buffer newly entered needs no command to the MMU
 * at all, an empty entry being one no TLB keeps. Each new buffer used to be an UPDATE of address space 0 and a wait for
 * it: 2.4 ms a buffer, 320 buffers every five seconds in the window server (the original's run 139, 26 October). Taking
 * a buffer out still updates.
 */
static int mali_fastmap = 1;

static uint64_t *pt_top, *pt_root;      /* pt_root: level 0, on a GPU of more than 39 bits (above) */
static uint64_t pt_top_phys, pt_root_phys, pt_transtab;
static uint32_t pt_gpu_bits = GPU_VA_BITS;  /* the GPU's own, from step 4 (48 at most is used) */

static inline bool
pt_four_levels(void)
{
	return pt_gpu_bits >= PT_FOUR_LEVELS_BITS;
}

/* AS_TRANSCFG for the tables: AArch64, 4 KB pages, the input bits the walk starts from, how it reads */
static inline uint32_t
pt_transcfg(void)
{
	return TRANSCFG_AARCH64 | TRANSCFG_INA(pt_four_levels() ? pt_gpu_bits : GPU_VA_BITS) |
	       (mali_fastmap ? TRANSCFG_WALK_NC : TRANSCFG_WALK_WB);
}
static uint32_t pt_refused;             /* addresses not taken into, or not followed through, the tables */

/*
 * The kernel's address of the table an entry points to; NULL for an empty entry, and for one whose address is not
 * memory the kernel manages (said, the first few times). The check comes before phystokv(), which panics on an address
 * outside the kernel's memory, and before the cache is cleaned over the table's entries: PixOS, on another phone,
 * panicked cleaning the cache over an address it had not checked. The tables are all the kernel's own pages, so this
 * should never refuse; if it does, an entry has been overwritten.
 */
static uint64_t *
table_of(uint64_t entry)
{
	uint64_t phys = entry & PTE_ADDRESS;

	if (!(entry & 1)) {
		return NULL;
	}
	if (phys == 0 || !pmap_valid_address(phys)) {
		if (pt_refused++ < 4) {
			printf("mali: a table entry %#llx points outside the kernel's memory: not followed\n", entry);
		}
		return NULL;
	}
	return (uint64_t *)phystokv(phys);
}

static uint64_t *
pt_page(uint64_t *phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	if (*phys == 0 || !pmap_valid_address(*phys)) {
		kmem_free(kernel_map, page, PAGE_SIZE);
		return NULL;
	}
	FlushPoC_DcacheRegion(page, PAGE_SIZE);  /* its zeroes in memory, where the GPU's walk reads them */
	return (uint64_t *)page;
}

/* The table below `entry`, made if there is none yet. */
static uint64_t *
pt_below(uint64_t *entry)
{
	uint64_t phys;
	uint64_t *table;

	if (*entry & 1) {
		return table_of(*entry);
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* One 4 KB page of physical memory at GPU address `gpu` (with the lock held, or before anyone else runs). */
static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t *middle, *bottom, *entry;

	if (gpu >= (1ull << GPU_VA_BITS)) {
		return false;
	}
	if (phys == 0 || (!pmap_valid_address(phys) && !hakios_scanout_carved(phys))) {
		/*
		 * the GPU is given memory the kernel manages, and nothing else (a register block, memory of the firmware's) --
		 * but for the scan-out pool of the DPU driver (fb_zc: the display surfaces the window server draws into)
		 */
		if (pt_refused++ < 4) {
			printf("mali: physical address %#llx is not memory the kernel manages: not entered at %#llx\n", phys, gpu);
		}
		return false;
	}
	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	if (pt_four_levels() && pt_root == NULL) {
		/* level 0 above it: entry 0 covers GPU addresses 0 to 2^39, and every one here is below 2^35 */
		if ((pt_root = pt_page(&pt_root_phys)) == NULL) {
			return false;
		}
		pt_root[0] = pt_top_phys | PTE_TABLE;
		FlushPoC_DcacheRegion((vm_offset_t)pt_root, sizeof(pt_root[0]));
	}
	pt_transtab = pt_root != NULL ? pt_root_phys : pt_top_phys;
	if ((middle = pt_below(&pt_top[(gpu >> 30) & 31])) == NULL || (bottom = pt_below(&middle[(gpu >> 21) & 511])) == NULL) {
		return false;
	}
	entry = &bottom[(gpu >> 12) & 511];
	*entry = (phys & PTE_ADDRESS) | PTE_AF | PTE_SH_INNER | PTE_AP_RW | PTE_PAGE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return true;
}

/* The entries of `size` bytes from GPU address `gpu` emptied, as far as there are tables (with the lock held). */
static void
unmap_pages(uint64_t gpu, uint64_t size)
{
	if (pt_top == NULL) {
		return;
	}
	for (uint64_t at = gpu; at < gpu + size; at += 4096) {        /* the GPU's pages, not the kernel's 16 KB */
		uint64_t *table = table_of(pt_top[(at >> 30) & 31]), *entry;

		if (table == NULL) {
			continue;
		}
		table = table_of(table[(at >> 21) & 511]);
		if (table == NULL) {
			continue;
		}
		entry = &table[(at >> 12) & 511];
		*entry = 0;
		FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	}
}

/* ---- the Exynos 9810's side: the supply, the clock, power, the TrustZone protection controller ---- */

/* PLL_G3D as it is, said; its PLL_CON0 back. */
static uint32_t
pll_say(const char *when)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0);
	uint32_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
	uint32_t khz = p != 0 ? (uint32_t)(PLL_FIN_KHZ * m / ((uint64_t)p << s)) : 0;

	say("mali: %u: PLL_G3D %s: PLL_CON0 %#x: M %u, P %u, S %u, %u.%03u MHz from 26 MHz; %s, %s; the G3D on %s%s; "
	    "PLL_CON2 %#x\n", step, when, con0, m, p, s, khz / 1000, khz % 1000, (con0 & PLL_ENABLE) ? "enabled" :
	    "not enabled", (con0 & PLL_STABLE) ? "stable" : "NOT STABLE", (con0 & MUX_SEL) ? "the PLL" : "the oscillator",
	    (con0 & MUX_BUSY) ? " (the mux still moving)" : "", rd(cmu_g3d, PLL_CON2));
	return con0;
}

static bool
pll_is(const struct g3d_rate *r)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0);

	return ((con0 >> 16) & 0x3ff) == r->m && ((con0 >> 8) & 0x3f) == r->p && (con0 & 7) == r->s &&
	       (con0 & (PLL_ENABLE | PLL_STABLE | MUX_SEL)) == (PLL_ENABLE | PLL_STABLE | MUX_SEL);
}

/*
 * PLL_G3D to rate `r`, as the S9+ port switches it (s9p-g3d.c): PLL_CON2 = 0x30000003 (with S-Boot's 0x10000001 the PLL
 * never locks), the lock time; the G3D over to the oscillator and the mux waited for, as a step of its own; the PLL
 * stopped and its dividers cleared, then M/P/S, then ENABLE; STABLE waited for; the G3D back on the PLL and the mux
 * waited for. On a failure the PLL as it was found, on the oscillator if it does not lock again.
 */
static bool
pll_switch(const struct g3d_rate *r)
{
	uint32_t old_con0 = rd(cmu_g3d, PLL_CON0), old_locktime = rd(cmu_g3d, PLL_LOCKTIME), v;
	const char *failed;

	if (!pll_quiet) {
		say("mali: %u: PLL_G3D to %u MHz (M %u, P %u, S %u): PLL_CON2 = %#x, PLL_LOCKTIME = %u, the G3D on the "
		    "oscillator, the PLL relocked, the G3D back on it\n", step, r->mhz, r->m, r->p, r->s, PLL_CON2_LOCK,
		    r->p * PLL_LOCK_FACTOR);
	}
	wr(cmu_g3d, PLL_CON2, PLL_CON2_LOCK);
	wr(cmu_g3d, PLL_LOCKTIME, r->p * PLL_LOCK_FACTOR);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	if (!wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US)) {
		failed = "the mux did not move to the oscillator";
		goto restore;
	}
	v = rd(cmu_g3d, PLL_CON0) & ~(PLL_ENABLE | PLL_PMS);
	wr(cmu_g3d, PLL_CON0, v);
	v |= r->m << 16 | r->p << 8 | r->s;
	wr(cmu_g3d, PLL_CON0, v);
	wr(cmu_g3d, PLL_CON0, v | PLL_ENABLE);
	if (!wait_for(cmu_g3d, PLL_CON0, PLL_STABLE, PLL_STABLE, PLL_STABLE_US)) {
		failed = "the PLL did not lock";
		goto restore;
	}
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) | MUX_SEL);
	if (!wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US) || !pll_is(r)) {
		failed = "the mux did not move back to the PLL";
		goto restore;
	}
	if (!pll_quiet) {
		(void)pll_say("switched");
	}
	return true;

restore:
	(void)pll_say(failed);
	say("mali: %u: %s: PLL_G3D put back as it was found (PLL_CON0 %#x)\n", step, failed, old_con0);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	if (wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US)) {
		wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~PLL_ENABLE);
		wr(cmu_g3d, PLL_LOCKTIME, old_locktime);
		wr(cmu_g3d, PLL_CON0, old_con0 & ~MUX_SEL);
		if (!(old_con0 & PLL_ENABLE) || wait_for(cmu_g3d, PLL_CON0, PLL_STABLE, PLL_STABLE, PLL_STABLE_US)) {
			wr(cmu_g3d, PLL_CON0, old_con0);
			(void)wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US);
		}
	}
	(void)pll_say("put back");
	return false;
}

/* The rail's voltage, said; its B6OUT in `out`. False if ACPM did not answer. */
static bool
rail_read(const char *when, uint8_t *out)
{
	uint8_t ctrl = 0, vsel = 0;
	int error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, &ctrl);

	if (error == 0) {
		error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6OUT, &vsel);
	}
	if (error != 0) {
		say("mali: %u: ACPM: BUCK6 (vdd_g3d) %s: NOT READ, error %d\n", step, when, error);
		return false;
	}
	say("mali: %u: ACPM: BUCK6 (vdd_g3d) %s: B6CTRL %#x ([7:6] = %u: %s), B6OUT %#x = %u mV\n", step, when, ctrl,
	    (ctrl >> 6) & 3, (ctrl >> 6) == 3 ? "always on" : (ctrl >> 6) == 2 ? "the G3D power pin's" : (ctrl >> 6) == 1 ?
	    "on in standby" : "OFF", vsel, 300 + vsel * 25 / 4);
	*out = vsel;
	return true;
}

/*
 * B6OUT = `vsel`, and read back. The only PMIC register this driver writes, never above 950 mV: nothing else, ever.
 * Tried RAIL_TRIES times, 100 ms apart (the S9+ port found the APM not answering yet early in the boot).
 */
static bool
rail_set(uint8_t vsel)
{
	uint8_t now = 0;

	if (vsel > B6OUT_MOST) {
		return false;
	}
	for (int attempt = 1; attempt <= RAIL_TRIES; attempt++) {
		int error = hakios_acpm_pmic_write(HAKIOS_ACPM_PMIC_PMIC, B6OUT, vsel);

		if (error == 0) {
			error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6OUT, &now);
		}
		if (error == 0 && now == vsel) {
			pause_us(RAIL_SETTLE_US);
			return true;
		}
		say("mali: %u: ACPM: B6OUT = %#x, attempt %d of %d: %s %#x\n", step, vsel, attempt, RAIL_TRIES,
		    error ? "error" : "it reads", error ? (unsigned)error : now);
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	return false;
}

/* One SMC to EL3 (through Carbon at EL2): x0 back. */
static uint64_t
smc(uint64_t function, uint64_t first, uint64_t second, uint64_t third)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = first;
	register uint64_t x2 __asm__("x2") = second;
	register uint64_t x3 __asm__("x3") = third;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * The domain off again (see the header's "power off"): a GPU that has answered is reset first, so that nothing of it is
 * running when the power goes. A domain that does not go off is switched on again with its protection restored, and
 * left on: it is not one the GPU can be read in, half off. The rail is put back as found once the domain is off.
 */
static void
g3d_off(void)
{
	uint64_t answer;

	if (gpu_answered) {
		say("mali: before the power goes: a reset\n");
		if (!gpu_reset()) {
			say("mali: the reset did not finish (GPU_INT_RAWSTAT %#x): the power goes anyway\n", rd(mali, GPU_INT_RAWSTAT));
		}
		gpu_answered = false;
	}
	if ((rd(pmu, PMU_G3D_STATUS) & G3D_ON) != G3D_ON) {
		/* (never on: its protection controller is in it, and EL3 is not to save it from a block without power) */
		say("mali: the G3D domain is not on (status %#x): no DTZPC save; its configuration [3:0] = 0\n",
		    rd(pmu, PMU_G3D_STATUS));
		wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON);
		(void)wait_for(pmu, PMU_G3D_STATUS, G3D_ON, 0, G3D_WAIT_US);
		goto clocks;
	}
	say("mali: switching the G3D domain off again, as it was found: first Samsung's low-power settings for it\n");
	for (unsigned i = 0; i < sizeof(g3d_lpm) / sizeof(g3d_lpm[0]); i++) {
		uint32_t was = rd(pmu, g3d_lpm[i].at);

		wr(pmu, g3d_lpm[i].at, (was & ~g3d_lpm[i].mask) | g3d_lpm[i].value);
		say("mali: PMU +%#x %s: %#x -> %#x\n", g3d_lpm[i].at, g3d_lpm[i].name, was, rd(pmu, g3d_lpm[i].at));
	}
	answer = smc(DTZPC_SMC, DTZPC_SAVE, DTZPC_G3D, 0);
	say("mali: SMC %#x (%u, %#x): the DTZPC save, returned %#llx\n", DTZPC_SMC, DTZPC_SAVE, DTZPC_G3D, answer);
	if ((uint32_t)answer != 0) {
		say("mali: the protection was not saved: the domain is left ON\n");
		powered_here = false;
		return;
	}
	wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON);
	if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, 0, G3D_WAIT_US)) {
		say("mali: the domain did not go off: status %#x after %u ms; switching it on again\n", rd(pmu, PMU_G3D_STATUS),
		    G3D_WAIT_US / 1000);
		wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) | G3D_ON);
		if (wait_for(pmu, PMU_G3D_STATUS, G3D_ON, G3D_ON, G3D_WAIT_US)) {
			answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D, 0);
			say("mali: on again; the DTZPC restore returned %#llx; the domain is left ON\n", answer);
		} else {
			say("mali: NOT ON AGAIN EITHER: status %#x; nothing of the G3D is to be touched\n", rd(pmu, PMU_G3D_STATUS));
		}
		powered_here = false;
		return;
	}
clocks:
	wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) & ~MUX_SEL);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	wr(cmu_g3d, PLL_CON2, PLL_CON2_OFF);
	say("mali: the G3D domain: configuration %#x, status %#x; CMU_G3D +0x100 %#x, PLL_CON0 %#x, PLL_CON2 %#x\n",
	    rd(pmu, PMU_G3D_CONFIGURATION), rd(pmu, PMU_G3D_STATUS), rd(cmu_g3d, MUX_G3D_USER), rd(cmu_g3d, PLL_CON0),
	    rd(cmu_g3d, PLL_CON2));
	if (rail_changed) {
		say("mali: BUCK6 back as it was found: B6OUT %#x (%u mV)\n", b6out_found, 300 + b6out_found * 25 / 4);
		if (b6out_found <= B6OUT_MOST && rail_set(b6out_found)) {
			rail_changed = false;
		} else {
			say("mali: B6OUT not put back (it reads otherwise, or ACPM did not answer)\n");
		}
	}
	powered_here = false;
}

/* ---- the steps ---- */

/* 1: what the boot has left, read only; not the GPU itself. */
static bool
step_look(void)
{
	uint32_t status;

	say("mali: 1: mapping the PMU (%#lx), CMU_G3D (%#lx) and the GPU (%#lx); nothing is written in this step\n", PMU_PHYS,
	    CMU_G3D_PHYS, MALI_PHYS);
	pmu = ml_io_map(PMU_PHYS, PMU_SIZE);
	cmu_g3d = ml_io_map(CMU_G3D_PHYS, CMU_G3D_SIZE);
	mali = ml_io_map(MALI_PHYS, MALI_SIZE);
	if (pmu == 0 || cmu_g3d == 0 || mali == 0) {
		say("mali: 1: cannot map them\n");
		return false;
	}
	say("mali: 1: reading the PMU's G3D configuration (%#lx) and status (%#lx)\n", PMU_PHYS + PMU_G3D_CONFIGURATION,
	    PMU_PHYS + PMU_G3D_STATUS);
	status = rd(pmu, PMU_G3D_STATUS);
	say("mali: 1: G3D configuration %#x, status %#x: the domain is %s\n", rd(pmu, PMU_G3D_CONFIGURATION), status,
	    (status & G3D_ON) == G3D_ON ? "on" : (status & G3D_ON) == 0 ? "off" : "NEITHER ON NOR OFF");
	say("mali: 1: reading CMU_G3D (outside the domain: it answers with the domain off)\n");
	(void)pll_say("as found");
	say("mali: 1: CMU_G3D +0x100 %#x (the G3D on %s), MUX_CLK_G3D_BUSD %#x, DIV_CLK_G3D_BUSD %#x, DIV_CLK_G3D_BUSP %#x\n",
	    rd(cmu_g3d, MUX_G3D_USER), (rd(cmu_g3d, MUX_G3D_USER) & MUX_SEL) ? "CMU's clock" : "the oscillator",
	    rd(cmu_g3d, MUX_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSP));
	if (mali_soc == 9610 && mali_peek && (status & G3D_ON) == G3D_ON) {
		/* the A50 (mali_peek=1): the GPU's identity, read only, with the domain S-Boot left on */
		say("mali: 1: the 9610's domain is on: GPU_ID %#x, L2_FEATURES %#x, GPU_STATUS %#x, SHADER_PRESENT %#x, "
		    "TILER_PRESENT %#x, L2_PRESENT %#x\n", rd(mali, 0x0), rd(mali, 0x4), rd(mali, 0x34), rd(mali, 0x100),
		    rd(mali, 0x110), rd(mali, 0x120));
	} else {
		say("mali: 1: the GPU's own registers are not read before step 2 has raised its supply (S-Boot's 260 MHz at 644 mV "
		    "hung the S9+'s SoC when its GPU was read and reset)\n");
	}
	if (mali_acpm && (acpm_ok = hakios_acpm_init(say))) {
		b6out_known = rail_read("as found", &b6out_found);
	} else if (mali_acpm) {
		say("mali: 1: no ACPM: the rail cannot be raised, and step 2 will stop there\n");
	}
	return true;
}

/* 2: the supply, the clock, power. */
static bool
step_power(void)
{
	const struct g3d_rate *rate = NULL;
	uint32_t status = rd(pmu, PMU_G3D_STATUS) & G3D_ON;

	if (status != G3D_ON && status != 0) {
		say("mali: 2: the domain is neither on nor off (status %#x): left alone\n", rd(pmu, PMU_G3D_STATUS));
		return false;
	}
	/* the rail first, before the clock moves and before anything of the GPU is touched */
	if (!mali_acpm) {
		say("mali: 2: mali_acpm=0: BUCK6 left as found -- the GPU will be touched at the boot's voltage\n");
	} else if (!acpm_ok) {
		say("mali: 2: no ACPM: BUCK6 cannot be raised; the GPU is not touched at the boot's voltage\n");
		return false;
	} else if (!b6out_known && !(b6out_known = rail_read("as found, read again", &b6out_found))) {
		say("mali: 2: B6OUT cannot be read: not written either (ACPM does not answer); the GPU is not touched at the "
		    "boot's voltage\n");
		return false;
	} else if (b6out_found > b6out_target) {
		say("mali: 2: the boot's BUCK6 is above the requested %u uV: no lowering under a running GPU, and no GPU access\n",
		    mali_uv);
		return false;
	} else if (b6out_found == b6out_target) {
		say("mali: 2: B6OUT is %#x (%u mV) already: not written\n", b6out_found, 300 + b6out_found * 25 / 4);
	} else {
		uint8_t now = 0;

		say("mali: 2: ACPM: B6OUT = %#x (%u uV, was %#x = %u mV), read back, then %u us\n", b6out_target, mali_uv,
		    b6out_found, 300 + b6out_found * 25 / 4, RAIL_SETTLE_US);
		rail_changed = true;
		if (!rail_set(b6out_target)) {
			say("mali: 2: BUCK6 did not reach the selected %u uV: the GPU is not touched\n", mali_uv);
			(void)rail_read("now", &now);
			return false;
		}
		(void)rail_read("now", &now);
	}
	/* the clock: PLL_G3D at the rate asked for */
	for (unsigned i = 0; i < sizeof(g3d_rates) / sizeof(g3d_rates[0]); i++) {
		if (g3d_rates[i].mhz == mali_mhz) {
			rate = &g3d_rates[i];
		}
	}
	if (rate == NULL) {
		say("mali: 2: mali_mhz=%u is not one of 260, 455, 572, 764: 260\n", mali_mhz);
		rate = &g3d_rates[0];
	}
	if (pll_is(rate)) {
		say("mali: 2: PLL_G3D runs at %u MHz already, the G3D on it: not touched\n", rate->mhz);
	} else if (!mali_pll) {
		uint32_t con0 = pll_say("left as found (mali_pll=0)");

		if ((con0 & (PLL_ENABLE | PLL_STABLE)) != (PLL_ENABLE | PLL_STABLE)) {
			say("mali: 2: PLL_G3D does not run, and mali_pll=0 leaves it so: the GPU would have no clock\n");
			return false;
		}
	} else if (!pll_switch(rate)) {
		return false;
	}
	if (status == G3D_ON) {
		say("mali: 2: the domain is on already: no power-on; it is left on whatever happens\n");
		return true;
	}
	/* the power-on: PLL_CON2 and PLL_CON0's MUX_SEL as the switch left them (or the boot, with mali_pll=0) */
	say("mali: 2: the power-on: PLL_CON2 = %#x (was %#x), PLL_CON0 |= MUX_SEL, CMU_G3D +0x100 |= bit 4 (was %#x)\n",
	    PLL_CON2_LOCK, rd(cmu_g3d, PLL_CON2), rd(cmu_g3d, MUX_G3D_USER));
	powered_here = true;            /* from the first write on, the way back is g3d_off() */
	wr(cmu_g3d, PLL_CON2, PLL_CON2_LOCK);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) | MUX_SEL);
	wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) | MUX_SEL);
	say("mali: 2: writing the PMU's G3D configuration [3:0] = 0xf (was %#x), then waiting up to %u ms for its status\n",
	    rd(pmu, PMU_G3D_CONFIGURATION), G3D_WAIT_US / 1000);
	wr(pmu, PMU_G3D_CONFIGURATION, (rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON) | G3D_ON);
	if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, G3D_ON, G3D_WAIT_US)) {
		say("mali: 2: the domain did not come up: status %#x after %u ms\n", rd(pmu, PMU_G3D_STATUS), G3D_WAIT_US / 1000);
		return false;
	}
	say("mali: 2: the domain is on: status %#x\n", rd(pmu, PMU_G3D_STATUS));
	return true;
}

/* 3: after a power-on of ours, the G3D's TrustZone protection controller set up again by EL3, and the bus dividers. */
static bool
step_dtzpc(void)
{
	uint64_t answer;

	if (!powered_here) {
		say("mali: 3: the boot left the domain on: its protection and bus dividers stand (the S9+ port restores them only "
		    "after a power-on of its own); nothing done\n");
		return true;
	}
	say("mali: 3: about to make SMC %#x (%u, %#x, 0): the DTZPC restore, after the G3D's power-on; Carbon, at EL2, passes "
	    "it on to EL3\n", DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D);
	answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D, 0);
	say("mali: 3: SMC %#x returned %#llx\n", DTZPC_SMC, answer);
	if ((uint32_t)answer != 0) {
		say("mali: 3: not 0: %s; the GPU is not read (it would be an SError)\n", (uint32_t)answer == 0xffffffffu ?
		    "the call is not known to whoever answered it (SMCCC's NOT_SUPPORTED)" : "a refusal");
		return false;
	}
	say("mali: 3: the bus dividers: MUX_CLK_G3D_BUSD = 0, DIV_CLK_G3D_BUSD = 0 (the PLL's rate), DIV_CLK_G3D_BUSP = 3 (a "
	    "quarter of it); were %#x, %#x, %#x\n", rd(cmu_g3d, MUX_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSD),
	    rd(cmu_g3d, DIV_CLK_G3D_BUSP));
	wr(cmu_g3d, MUX_CLK_G3D_BUSD, 0);
	wr(cmu_g3d, DIV_CLK_G3D_BUSD, 0);
	wr(cmu_g3d, DIV_CLK_G3D_BUSP, 3);
	if (!wait_for(cmu_g3d, DIV_CLK_G3D_BUSP, CLK_CON_BUSY, 0, MUX_BUSY_US)) {
		say("mali: 3: DIV_CLK_G3D_BUSP still busy: %#x\n", rd(cmu_g3d, DIV_CLK_G3D_BUSP));
		return false;
	}
	return true;
}

/* 4: which GPU, and what it has. */
static bool
step_gpu(void)
{
	uint32_t mmu, va_bits, pa_bits, shaders_hi, as_present, js_present, tiler;

	say("mali: 4: reading GPU_ID at %#lx, the first read of the GPU: should the run end here, the domain says on and the "
	    "GPU does not answer (its supply, its clock, or its protection)\n", MALI_PHYS);
	gpu_id = rd(mali, GPU_ID);
	say("mali: 4: GPU_ID %#x: architecture %u.%u revision %u, product %u, version r%up%u, status %u\n", gpu_id,
	    gpu_id >> 28, (gpu_id >> 24) & 0xf, (gpu_id >> 20) & 0xf, (gpu_id >> 16) & 0xf, (gpu_id >> 12) & 0xf,
	    (gpu_id >> 4) & 0xff, gpu_id & 0xf);
	if ((gpu_id >> 16) != MALI_G72) {
		if (((gpu_id >> 16) & 0xf00f) != 0x6001) {
			say("mali: 4: not a Mali-G72 (GPU_ID >> 16 should be %#x): nothing more is done with it\n", MALI_G72);
			return false;
		}
		say("mali: 4: a G72 of another revision than the %#x expected: going on\n", MALI_G72);
	}
	mmu = rd(mali, GPU_MMU_FEATURES);
	va_bits = mmu & 0xff;
	pa_bits = (mmu >> 8) & 0xff;
	say("mali: 4: MMU_FEATURES %#x: %u bits of GPU address, %u of physical (the page tables want %u and %u; the G52 "
	    "the driver was written for had 0x2823)\n", mmu, va_bits, pa_bits, GPU_VA_BITS, GPU_PA_BITS_NEEDED);
	if (va_bits < GPU_VA_BITS) {
		say("mali: 4: fewer than %u bits of GPU address: the page tables here start at bit 34; not going on\n", GPU_VA_BITS);
		return false;
	}
	if (pa_bits < GPU_PA_BITS_NEEDED) {
		say("mali: 4: fewer than %u bits of physical address, and this phone's memory reaches 0x8FFFFFFFF: not going on\n",
		    GPU_PA_BITS_NEEDED);
		return false;
	}
	pt_gpu_bits = va_bits > 48 ? 48 : va_bits;
	say("mali: 4: the GPU's tables will be walked %s; AS_TRANSCFG %#x\n", pt_four_levels() ?
	    "from level 0, four levels, as kbase and panfrost have a GPU of this many bits walk them" :
	    "from level 1, three levels, as on the G52", pt_transcfg());
	shader_mask = rd(mali, SHADER_PRESENT);
	shaders_hi = rd(mali, SHADER_PRESENT_HI);
	tiler = rd(mali, TILER_PRESENT);
	l2_mask = rd(mali, L2_PRESENT);
	as_present = rd(mali, GPU_AS_PRESENT);
	js_present = rd(mali, GPU_JS_PRESENT);
	say("mali: 4: shader cores %#x (%u of them; above bit 31 %#x), tiler %#x, L2 %#x; address spaces %#x, job slots %#x\n",
	    shader_mask, __builtin_popcount(shader_mask), shaders_hi, tiler, l2_mask, as_present, js_present);
	say("mali: 4: L2_FEATURES %#x, CORE_FEATURES %#x, TILER_FEATURES %#x, MEM_FEATURES %#x, THREAD_MAX_THREADS %u, "
	    "THREAD_TLS_ALLOC %u, COHERENCY_FEATURES %#x\n", rd(mali, GPU_L2_FEATURES), rd(mali, GPU_CORE_FEATURES),
	    rd(mali, GPU_TILER_FEATURES), rd(mali, GPU_MEM_FEATURES), rd(mali, THREAD_MAX_THREADS), rd(mali, THREAD_TLS_ALLOC),
	    rd(mali, COHERENCY_FEATURES));
	if (shader_mask == 0 || shaders_hi != 0) {
		say("mali: 4: shader cores %#x:%08x: the jobs are given 32 bits of affinity; not going on\n", shaders_hi, shader_mask);
		return false;
	}
	if (!(as_present & 1) || (js_present & 3) != 3 || tiler == 0 || l2_mask == 0) {
		say("mali: 4: no address space 0, job slots 0 and 1, tiler or L2: not going on\n");
		return false;
	}
	gpu_answered = true;
	return true;
}

/* 5: a reset, no coherency, and panfrost's setting after a reset. */
static bool
step_reset(void)
{
	say("mali: 5: JM_CONFIG %#x, SHADER_CONFIG %#x, TILER_CONFIG %#x, L2_MMU_CONFIG %#x before the reset\n",
	    rd(mali, GPU_JM_CONFIG), rd(mali, GPU_SHADER_CONFIG), rd(mali, GPU_TILER_CONFIG), rd(mali, GPU_L2_MMU_CONFIG));
	say("mali: 5: writing GPU_INT_MASK = 0, GPU_CMD = 1 (soft reset; 2, hard, if it does not finish), then "
	    "COHERENCY_ENABLE = %u (none) and L2_MMU_CONFIG &= ~%#x\n", COHERENCY_NONE, L2_MMU_LIMIT_EXTERNAL);
	if (!gpu_reset()) {
		say("mali: 5: GPU_ID %#x, but it did not finish a reset (GPU_INT_RAWSTAT %#x)\n", gpu_id, rd(mali, GPU_INT_RAWSTAT));
		return false;
	}
	say("mali: 5: reset; COHERENCY_ENABLE reads %#x, L2_MMU_CONFIG %#x\n", rd(mali, COHERENCY_ENABLE),
	    rd(mali, GPU_L2_MMU_CONFIG));
	return true;
}

/* 6: the cores. */
static bool
step_cores(void)
{
	uint32_t tiler = rd(mali, TILER_PRESENT);
	const char *which = NULL;

	say("mali: 6: writing L2_PWRON %#x, then SHADER_PWRON %#x, then TILER_PWRON %#x, each waited for\n", l2_mask,
	    shader_mask, tiler);
	if (!gpu_cores_on(&which)) {
		say("mali: 6: %s did not power up: L2 ready %#x, shaders %#x, tiler %#x\n", which, rd(mali, L2_READY),
		    rd(mali, SHADER_READY), rd(mali, TILER_READY));
		return false;
	}
	say("mali: 6: GPU_ID %#x (Mali-G72 r%up%u), %u shader cores %#x, tiler %#x, L2 %#x: all powered\n", gpu_id,
	    (gpu_id >> 12) & 0xf, (gpu_id >> 4) & 0xff, __builtin_popcount(shader_mask), shader_mask, tiler, l2_mask);
	return true;
}

/*
 * 7 and 8: two jobs, first in identity mode, which needs no tables and so says whether the job manager works at all;
 * then the same job at a GPU address the tables give the page, which says whether they do. Afterwards address space 0
 * stays on the tables, which is how anything that draws will need it, and the page's entry is taken out of them again.
 */
#define FIRST_JOB_GPU       0x00100000ull

static vm_offset_t job_page;

static bool
step_job_identity(void)
{
	if (kmem_alloc(kernel_map, &job_page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		job_page = 0;
		say("mali: 7: no page for the first jobs\n");
		return false;
	}
	say("mali: 7: address space 0 in identity mode (AS_TRANSCFG 2); one job on slot 1 at %#llx\n", kvtophys(job_page));
	as0_update(0, 2);                       /* identity: a GPU address is the physical one */
	return run_write_job(job_page, kvtophys(job_page), "first job, identity mode");
}

static bool
step_job_tables(void)
{
	if (!map_page(FIRST_JOB_GPU, kvtophys(job_page))) {
		say("mali: 8: no memory for page tables\n");
		return false;
	}
	say("mali: 8: the page at GPU address %#llx in the GPU's tables (AS_TRANSTAB %#llx, %s levels, AS_TRANSCFG %#x), "
	    "address space 0 on them; the same job again\n", FIRST_JOB_GPU, pt_transtab, pt_four_levels() ? "four" : "three",
	    pt_transcfg());
	as0_update(pt_transtab, pt_transcfg());
	return run_write_job(job_page, FIRST_JOB_GPU, "second job, through the GPU's page tables");
}

/* 9: there for programs. */
static bool
step_serve(void)
{
	say("mali: 9: HakiosMali started: programs can draw with the GPU (Mesa's panfrost; drivers/gpu/hakios.mali.cpp)\n");
	reached = 9;                            /* (the user client asks hakios_mali_ready()) */
	HakiosMaliStart();
	return true;
}

static const struct {
	const char *name;
	bool (*make)(void);
} steps[] = {
	{ "what the boot left, read only", step_look },
	{ "the supply, the clock, power", step_power },
	{ "after a power-on: the DTZPC restore, the bus dividers", step_dtzpc },
	{ "GPU_ID and what the GPU has", step_gpu },
	{ "a reset", step_reset },
	{ "the cores", step_cores },
	{ "a job in identity mode", step_job_identity },
	{ "a job through page tables", step_job_tables },
	{ "HakiosMali", step_serve },
};
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

/* ---- what the service that programs talk to uses (drivers/gpu/hakios.mali.cpp) ---- */

/*
 * Linux's panfrost interface, as far as Mesa's panfrost needs it to draw, is answered by the IOKit service HakiosMali;
 * this is its half in the hardware: parameters, GPU addresses, pages entered into and taken out of the tables, and
 * jobs. One address space (0) for everyone, and one job at a time -- simple, and enough to see Mesa draw; overlapping
 * jobs and per-client address spaces come after.
 */
static LCK_GRP_DECLARE(mali_lock_group, "hakios-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_lock_group);
/*
 * Held while a job runs, and by what must not change the address space under one (an UPDATE when entries are taken
 * out); taken before mali_lock, never after. mali_lock itself is no longer held for the length of a job: making a
 * buffer had to wait for the job running to finish (2.5 ms a buffer, 26 October).
 */
static LCK_MTX_DECLARE(mali_job_lock, &mali_lock_group);

/*
 * Until a job is done, without keeping a core busy: a moment of polling for the short ones, then asleep 200 us at a
 * time. The wait used to spin (delay(10) in a loop) for as long as the GPU worked -- a whole core of the four lost to
 * every frame's rendering.
 */
static int mali_wait_event;
static uint32_t mali_nap_us = 50;       /* boot-arg mali_nap_us (10 to 1000) */
static LCK_MTX_DECLARE(mali_wait_lock, &mali_lock_group);

static bool
wait_job(uint32_t at, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t deadline;

	for (int i = 0; i < 20; i++) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(mali, at) & mask) == want) {
			return true;
		}
		uint64_t nap;

		clock_interval_to_deadline(mali_nap_us, NSEC_PER_USEC, &nap);
		lck_mtx_lock(&mali_wait_lock);
		(void)lck_mtx_sleep_deadline(&mali_wait_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_wait_event, THREAD_UNINT, nap);
		lck_mtx_unlock(&mali_wait_lock);
	}
	return (rd(mali, at) & mask) == want;
}

#define MMU_INT_RAWSTAT     0x2000
#define MMU_INT_CLEAR       0x2004
#define AS_LOCKADDR_LO      0x10
#define AS_LOCKADDR_HI      0x14
#define AS_COMMAND_LOCK     2
#define AS_COMMAND_FLUSH_PT 4
#define JS0                 0x1800      /* fragment jobs; everything else goes to slot 1 */
/* The GPU addresses programs' buffers get: 32 MB to 4 GB, given out and taken back (hakios.mali_va.h says why). */
static struct mali_va mali_va;          /* with mali_lock held */

/*
 * The last buffers handed GPU addresses, and whether they have been taken out of the tables since: a translation fault
 * is a job writing into a buffer already freed or past the end of one, and this says which. An address freed is given
 * out again, but not soon (above): the newest buffer at an address is the one asked about. With mali_lock held.
 */
#define MALI_RECENT 4096
static struct {
	uint64_t address, size;
	uint32_t freed, kind;                   /* kind: 0 not said, 1 a panfrost BO, 2 an IOSurface */
	uint32_t width, height, row_bytes, format, offset;
} mali_recent[MALI_RECENT];
static uint32_t mali_recent_next;

/* What a buffer just made is, for describe_address. */
void
hakios_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == address && mali_recent[i].size != 0 && !mali_recent[i].freed) {
			mali_recent[i].kind = kind;
			mali_recent[i].width = width;
			mali_recent[i].height = height;
			mali_recent[i].row_bytes = row_bytes;
			mali_recent[i].format = format;
			mali_recent[i].offset = offset;
		}
	}
	lck_mtx_unlock(&mali_lock);
}

static void
say_kind(int i)
{
	if (mali_recent[i].kind == 2) {
		printf("mali:     an IOSurface %ux%u, %u bytes a row, format %#x, from byte %u\n", mali_recent[i].width,
		    mali_recent[i].height, mali_recent[i].row_bytes, mali_recent[i].format, mali_recent[i].offset);
	} else {
		printf("mali:     %s\n", mali_recent[i].kind == 1 ? "a panfrost BO" : "made by nothing that said");
	}
}

static void
describe_address(uint64_t address)
{
	int below = -1;

	/* newest first: an address given out again belongs to its last buffer */
	for (uint32_t n = 0; n < MALI_RECENT; n++) {
		int i = (int)((mali_recent_next - 1 - n) % MALI_RECENT);

		if (mali_recent[i].size == 0) {
			continue;
		}
		if (address >= mali_recent[i].address && address < mali_recent[i].address + mali_recent[i].size) {
			printf("mali:   the fault is %#llx into a buffer of %#llx bytes at %#llx, %s\n",
			    address - mali_recent[i].address, mali_recent[i].size, mali_recent[i].address,
			    mali_recent[i].freed ? "FREED already" : "still there (a page not entered?)");
			say_kind(i);
			return;
		}
		if (mali_recent[i].address <= address && (below < 0 || mali_recent[i].address > mali_recent[below].address ||
		    (mali_recent[i].address == mali_recent[below].address && !mali_recent[i].freed))) {
			below = i;
		}
	}
	if (below >= 0) {
		printf("mali:   the fault is %#llx past the end of a buffer of %#llx bytes at %#llx (%s)\n",
		    address - (mali_recent[below].address + mali_recent[below].size), mali_recent[below].size,
		    mali_recent[below].address, mali_recent[below].freed ? "freed" : "live");
		say_kind(below);
	} else {
		printf("mali:   the fault is in no buffer of the last %u\n", MALI_RECENT);
	}
}

/* The last job that did not finish, to be repeated where the end of the log keeps it. */
static struct {
	uint32_t count, slot, raw, status, mmu, fault, fault_address;
	uint64_t chain, transtab, walk[3];      /* the tables the GPU had, and their entries for the fault's address */
} last_failure;

bool
hakios_mali_map(uint64_t gpu, uint64_t phys)
{
	bool done;

	lck_mtx_lock(&mali_lock);
	done = map_page(gpu, phys);
	lck_mtx_unlock(&mali_lock);
	return done;
}

bool
hakios_mali_ready(void)
{
	return reached >= STEPS;
}

/* Linux's DRM_PANFROST_PARAM_*, in its order, from the registers they come from. */
bool
hakios_mali_parameter(uint32_t which, uint64_t *value)
{
	static const uint16_t simple[] = {
		[5] = 0xe00,    /* STACK_PRESENT (none on Bifrost: reads 0) */
		[6] = 0x018, [7] = 0x01c, [8] = 0x004, [9] = 0x008, [10] = 0x00c, [11] = 0x010, [12] = 0x014,
		[13] = 0x0ac, [14] = 0x0a0, [15] = 0x0a4, [16] = 0x0a8, [17] = 0x300,
		[18] = 0x0b0, [19] = 0x0b4, [20] = 0x0b8, [21] = 0x0bc,
	};

	if (!hakios_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:
		*value = gpu_id >> 16;
		return true;
	case 1:
		*value = gpu_id & 0xffff;
		return true;
	case 2:
	case 3:
	case 4:
		*value = rd(mali, 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali, 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 38:                                /* NR_CORE_GROUPS: as Linux panfrost counts them, one per L2 */
		*value = __builtin_popcount(l2_mask) != 0 ? (uint64_t)__builtin_popcount(l2_mask) : 1;
		return true;
	case 39:
		/*
		 * THREAD_TLS_ALLOC (0x310). Answered 0 on the G52, panfrost sized the stack of a spilling shader by
		 * THREAD_MAX_THREADS instead, and the GPU, which gives each core more stacks than that, wrote past the end of
		 * it (25 October: every write fault of the uber shaders, up to 0x15870 past a 0x60000 buffer). Never less than
		 * that fallback, whatever the register says.
		 */
		*value = rd(mali, THREAD_TLS_ALLOC);
		if (*value < rd(mali, THREAD_MAX_THREADS)) {
			*value = rd(mali, THREAD_MAX_THREADS);
		}
		{
			static int said;
			if (!said++) {
				printf("mali: THREAD_TLS_ALLOC %#x, THREAD_MAX_THREADS %#x, SHADER_PRESENT %#x\n", rd(mali, THREAD_TLS_ALLOC),
				    rd(mali, THREAD_MAX_THREADS), rd(mali, SHADER_PRESENT));
			}
		}
		return true;
	case 40:
		/*
		 * AFBC_FEATURES: 0. Mesa reads 0 as AFBC SUPPORTED, not as none (Mesa 25.0.7 pan_props.c,
		 * panfrost_query_afbc(): architecture 5 or later and the register 0), as Linux panfrost answers
		 * on a GPU without the register. So panfrost renders into AFBC textures here (a G72 has AFBC;
		 * seen under drm-shim as the G72, tools/mali-drmshim-check.sh: the triangle's render target is
		 * written AFBC). PAN_MESA_DEBUG=noafbc in a program's environment turns it off, for telling
		 * an AFBC fault from any other on the phone.
		 */
		*value = 0;
		return true;
	default:
		if (which >= 22 && which <= 37) {
			*value = rd(mali, 0x0c0 + (which - 22) * 4);
			return true;
		}
		if (which < sizeof(simple) / sizeof(simple[0]) && simple[which] != 0) {
			*value = rd(mali, simple[which]);
			return true;
		}
		return false;
	}
}

/*
 * `size` bytes of GPU addresses between 32 MB and 4 GB (hakios.mali_va.h). `executable`: a buffer the GPU may run code
 * from (Linux's flags without PANFROST_BO_NOEXEC), kept off 16 MB lines. 0: none left, or a size of nothing.
 */
uint64_t
hakios_mali_address(uint64_t size, bool executable)
{
	uint64_t address;

	lck_mtx_lock(&mali_lock);
	address = mali_va_alloc(&mali_va, size, executable);
	if (address != 0) {
		mali_recent[mali_recent_next % MALI_RECENT].address = address;
		mali_recent[mali_recent_next % MALI_RECENT].size = size;
		mali_recent[mali_recent_next % MALI_RECENT].freed = 0;
		mali_recent[mali_recent_next % MALI_RECENT].kind = 0;
		mali_recent_next++;
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

void
hakios_mali_unmap(uint64_t gpu, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == gpu && mali_recent[i].size != 0) {
			mali_recent[i].freed = 1;
		}
	}
	unmap_pages(gpu, size);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_lock(&mali_job_lock);                   /* an entry taken out: the TLB may hold it */
	lck_mtx_lock(&mali_lock);
	as0_update(pt_transtab, pt_transcfg());
	/* only now, with nothing of the buffer left in the tables or the MMU, are its addresses free to be given again */
	(void)mali_va_free(&mali_va, gpu, size);
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/*
 * After entries change: the GPU's MMU forgets what it remembered of the tables. The address space is set up again from
 * the top -- an UPDATE drops everything the MMU had cached for it -- rather than a region locked and its tables
 * flushed: with the region flush a fragment job faulted on a page of a buffer whose entry was there, written and
 * pushed out to memory. Jobs are run one at a time and to their end, so the GPU is idle whenever this runs.
 */
void
hakios_mali_mapped(void)
{
	if (mali_fastmap) {
		return;         /* see TRANSCFG_WALK_NC */
	}
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update(pt_transtab, pt_transcfg());
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

/* The entries of each level for GPU address `gpu`, as far as there are tables (with the lock held). */
static void
walk_tables(uint64_t gpu, uint64_t walk[3])
{
	uint64_t *table = pt_top;
	static const unsigned shift[3] = { 30, 21, 12 };

	walk[0] = walk[1] = walk[2] = 0;
	for (int level = 0; level < 3 && table != NULL; level++) {
		walk[level] = table[(gpu >> shift[level]) & (level == 0 ? 31 : 511)];
		table = level < 2 ? table_of(walk[level]) : NULL;
	}
}

/*
 * One job chain, from GPU address `chain`, run to its end before this returns: fragment jobs (`fragment`) on slot 0,
 * the rest on slot 1. 0 when it finished; otherwise the slot's status, a fault or 0xff for no answer within a second --
 * and then the GPU is reset, so the next one finds it working.
 */
uint32_t
hakios_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0 : 1, js = fragment ? JS0 : JS1, raw, status = 0;
	bool finished;

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	wr(mali, js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(mali, js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(mali, js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(mali, js + JS_AFFINITY_NEXT_HI, 0);
	wr(mali, js + JS_CONFIG_NEXT, JS_CONFIG);
	wr(mali, js + JS_COMMAND_NEXT, 1);
	lck_mtx_unlock(&mali_lock);
	finished = wait_job(JOB_INT_RAWSTAT, (1u << slot) | (1u << (slot + 16)), 1u << slot, 1000);
	lck_mtx_lock(&mali_lock);
	if (!finished) {
		raw = rd(mali, JOB_INT_RAWSTAT);
		status = (raw & (1u << (slot + 16))) ? rd(mali, js + JS_STATUS) : 0xff;
		last_failure.count++;
		last_failure.slot = slot;
		last_failure.raw = raw;
		last_failure.status = rd(mali, js + JS_STATUS);
		last_failure.mmu = rd(mali, MMU_INT_RAWSTAT);
		last_failure.fault = rd(mali, AS0 + AS_FAULTSTATUS);
		last_failure.fault_address = rd(mali, AS0 + 0x20);
		last_failure.chain = chain;
		last_failure.transtab = rd(mali, AS0 + AS_TRANSTAB_LO) | ((uint64_t)rd(mali, AS0 + AS_TRANSTAB_HI) << 32);
		walk_tables(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32), last_failure.walk);
		printf("mali: a job on slot %u did not finish: interrupts %#x, status %#x, MMU %#x fault %#x at %#x\n", slot, raw,
		    last_failure.status, last_failure.mmu, last_failure.fault, last_failure.fault_address);
		describe_address(last_failure.fault_address | ((uint64_t)rd(mali, AS0 + 0x24) << 32));
		const char *which = NULL;

		(void)gpu_reset();                      /* a reset, then everything back as it was */
		if (!gpu_cores_on(&which)) {
			printf("mali: after the reset, %s did not power up again\n", which);
		}
		as0_update(pt_transtab, pt_transcfg());
		wr(mali, MMU_INT_CLEAR, 0xffffffffu);
	}
	wr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
	return status;
}

/*
 * The jobs in a queue, run by a thread of their own: a submission returns at once with the job's number, and whoever
 * needs a job done waits for that number (hakios_mali_wait). Run one by one in the order given, which is the order
 * panfrost needs -- a pass's vertex and tiler chain before its fragment chain, one pass before the next that reads it.
 * Before this every submission ran its job to the end before returning: a frame of CoreAnimation's with its blur passes
 * had the window server waiting for the GPU at every one of them, the processor and the GPU taking turns (24 October,
 * 11 to 17 ms a frame of the processor's).
 */
#define MALI_QUEUE      64
static struct {
	uint64_t chain, seq, queued;
	bool fragment;
} mali_queue[MALI_QUEUE];
/* boot-arg mali_trace=1: a line for every job, "tl: j SEQ f|v q QUEUED b BEGAN e ENDED" in us of mach_absolute_time */
static uint32_t mali_trace;
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;
static LCK_MTX_DECLARE(mali_queue_lock, &mali_lock_group);

/*
 * The last second, for the display's line (fps_log, MiniIOMobileFramebuffer.cpp): how long the GPU ran jobs, how many,
 * how long they waited in the queue; with mali_queue_lock held.
 */
static uint64_t sec_busy, sec_queued;
static uint32_t sec_jobs;

/* PLL_G3D's rate as its registers say, in MHz; 26 when the G3D is on the oscillator */
static uint32_t
pll_mhz(void)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0), m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;

	if (!(con0 & MUX_SEL)) {
		return (uint32_t)(PLL_FIN_KHZ / 1000);
	}
	return p != 0 ? (uint32_t)(PLL_FIN_KHZ * m / ((uint64_t)p << s) / 1000) : 0;
}

/*
 * mali_gov=1: the GPU's clock after its load, as drivers/power/hakios.exynos_cpufreq.c sets the cores': every window of
 * GOV_WINDOW_MS, the share of it the GPU ran jobs; the lowest of the three rates at which that would have been
 * GOV_TARGET% at most, at once if higher, after GOV_DOWN windows in a row if lower; never above what the G3D's
 * temperature allows. The switch is pll_switch(), between two jobs (this thread runs them, one at a time) and under
 * mali_job_lock, so that nothing touches the GPU while its clock moves (the G3D is on the 26 MHz oscillator for some
 * microseconds). Three failed switches in a row and the governor stops, the clock left where it is.
 */
#define GOV_WINDOW_MS   16
#define GOV_TARGET      50              /* low: a frame waits for its jobs, the sooner done the better */
#define GOV_DOWN        5
#define GOV_IDLE_MS     50              /* asleep with no jobs: a window ends anyway, the clock may come down */
#define GOV_CAP1_MC     85000           /* the G3D's sensor: 455 MHz at most from here */
#define GOV_CAP2_MC     95000           /* 260 MHz from here */
#define GOV_HYST_MC     5000
#define GOV_NOT_READ_MC (-273000)
#define GOV_LEVELS      ((unsigned)(sizeof(g3d_rates) / sizeof(g3d_rates[0])))
extern int32_t hakios_exynos_tmu_zone_mc(unsigned zone);       /* 2: the G3D's (hakios.exynos_tmu.c; stubs.c) */
static uint32_t mali_gov;
/*
 * mali_gov_frame=<MHz> (kern.hakios.mali_gov_frame while it runs): the GPU's clock while frames come -- one given to the
 * display (hakios_mali_frame(), MiniIOMobileFramebuffer.cpp present()) in the last GOV_FRAME_MS -- at least the lowest
 * rate of the three that is as fast, whatever the load says (still under the temperature's cap). A frame's jobs are on
 * the way of the next frame: CoreAnimation's render thread waits for them, so a GPU busy a third of the time at 260 MHz
 * is a frame later than one at 572, though the load alone would keep it at 260. 0: none (as mali_gov first was).
 */
#define GOV_FRAME_MS    100
static uint32_t gov_frame_mhz = 572;
static uint64_t gov_frame_last;
SYSCTL_UINT(_kern_hakios, OID_AUTO, mali_gov_frame, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gov_frame_mhz, 0,
    "the GPU's clock while frames come, MHz at least (mali_gov=1; 0: none)");

void hakios_mali_frame(void);
void
hakios_mali_frame(void)
{
	__atomic_store_n(&gov_frame_last, mach_absolute_time(), __ATOMIC_RELAXED);
}
static bool gov_on;
static unsigned gov_level, gov_cap = GOV_LEVELS - 1, gov_down, gov_failed;
static uint64_t gov_since, gov_busy;
static uint32_t gov_switches, gov_ups, gov_downs, gov_capped;

/* which of the rates PLL_G3D runs at; GOV_LEVELS when none */
static unsigned
gov_level_now(void)
{
	for (unsigned i = 0; i < GOV_LEVELS; i++) {
		if (pll_is(&g3d_rates[i])) {
			return i;
		}
	}
	return GOV_LEVELS;
}

static void
gov_start(void)
{
	if (!mali_gov) {
		return;
	}
	if (!mali_pll || !mali_acpm || !acpm_ok || (gov_level = gov_level_now()) >= GOV_LEVELS) {
		say("mali: mali_gov=1: not started (mali_pll=%u, mali_acpm=%u, ACPM %s, PLL_G3D %s one of the three rates)\n",
		    mali_pll, mali_acpm, acpm_ok ? "answers" : "does not answer", gov_level < GOV_LEVELS ? "at" : "NOT at");
		return;
	}
	(void)PE_parse_boot_argn("mali_gov_frame", &gov_frame_mhz, sizeof(gov_frame_mhz));
	gov_on = true;
	gov_since = mach_absolute_time();
	say("mali: mali_gov=1: the GPU's clock follows its load from %u MHz: %u%% of a %u ms window wanted at most; no more "
	    "than %u MHz from %d C on the G3D's sensor, %u MHz from %d C\n", g3d_rates[gov_level].mhz, GOV_TARGET, GOV_WINDOW_MS,
	    g3d_rates[1].mhz, GOV_CAP1_MC / 1000, g3d_rates[0].mhz, GOV_CAP2_MC / 1000);
}

static void
gov_set(unsigned level)
{
	bool switched;

	if (level == gov_level || level >= GOV_LEVELS) {
		return;
	}
	lck_mtx_lock(&mali_job_lock);
	pll_quiet = true;
	switched = pll_switch(&g3d_rates[level]);
	pll_quiet = false;
	lck_mtx_unlock(&mali_job_lock);
	if (switched) {
		gov_ups += level > gov_level;
		gov_downs += level < gov_level;
		gov_level = level;
		gov_switches++;
		gov_failed = 0;
		return;
	}
	if ((gov_level = gov_level_now()) >= GOV_LEVELS || ++gov_failed >= 3) {
		gov_on = false;
		say("mali: mali_gov: PLL_G3D did not switch to %u MHz (%u times in a row; it runs at %u MHz): the governor stops\n",
		    g3d_rates[level].mhz, gov_failed, pll_mhz());
	}
}

/* `busy` more of the GPU's time at `now` (mach_absolute_time); a window that has ended decides. No locks held. */
static void
gov_window(uint64_t now, uint64_t busy)
{
	uint64_t window, elapsed;
	unsigned want, cap;
	uint32_t load;
	int32_t mc;

	if (!gov_on) {
		return;
	}
	gov_busy += busy;
	nanoseconds_to_absolutetime((uint64_t)GOV_WINDOW_MS * NSEC_PER_MSEC, &window);
	elapsed = now - gov_since;
	if (elapsed < window) {
		return;
	}
	load = (uint32_t)(gov_busy >= elapsed ? 100 : gov_busy * 100 / elapsed);
	gov_since = now;
	gov_busy = 0;
	mc = hakios_exynos_tmu_zone_mc(2);
	cap = GOV_LEVELS - 1;
	if (mc != GOV_NOT_READ_MC) {
		if (mc >= GOV_CAP1_MC || (gov_cap <= 1 && mc >= GOV_CAP1_MC - GOV_HYST_MC)) {
			cap = 1;
		}
		if (mc >= GOV_CAP2_MC || (gov_cap == 0 && mc >= GOV_CAP2_MC - GOV_HYST_MC)) {
			cap = 0;
		}
	}
	if (cap != gov_cap) {
		say("mali: mali_gov: the G3D at %d.%d C: %u MHz at most now\n", mc / 1000, mc % 1000 / 100, g3d_rates[cap].mhz);
		gov_capped += cap < gov_cap;
		gov_cap = cap;
	}
	for (want = 0; want + 1 < GOV_LEVELS &&
	    (uint64_t)load * g3d_rates[gov_level].mhz > (uint64_t)GOV_TARGET * g3d_rates[want].mhz; want++) {
	}
	/* (mali_gov_frame: frames coming, the floor) */
	if (gov_frame_mhz != 0) {
		uint64_t last = __atomic_load_n(&gov_frame_last, __ATOMIC_RELAXED), span;

		nanoseconds_to_absolutetime((uint64_t)GOV_FRAME_MS * NSEC_PER_MSEC, &span);
		if (last != 0 && now - last < span) {
			unsigned floor = 0;

			while (floor + 1 < GOV_LEVELS && g3d_rates[floor].mhz < gov_frame_mhz) {
				floor++;
			}
			want = want < floor ? floor : want;
		}
	}
	want = want > cap ? cap : want;
	if (want > gov_level || gov_level > cap) {
		gov_down = 0;
		gov_set(want);
	} else if (want < gov_level) {
		if (++gov_down >= GOV_DOWN) {
			gov_down = 0;
			gov_set(want);
		}
	} else {
		gov_down = 0;
	}
}

/*
 * fps_log (MiniIOMobileFramebuffer.cpp), once a second: the GPU's time running jobs since the last call, the jobs, their
 * time in the queue before they ran (ns), the clock (MHz) and the governor's switches; the G3D's temperature
 * (millidegrees). 0 when the GPU is not there for programs.
 */
int hakios_mali_second(uint64_t *busy_ns, uint32_t *jobs, uint64_t *queued_ns, uint32_t *mhz, uint32_t *switches,
    int32_t *g3d_mc);

int
hakios_mali_second(uint64_t *busy_ns, uint32_t *jobs, uint64_t *queued_ns, uint32_t *mhz, uint32_t *switches,
    int32_t *g3d_mc)
{
	uint64_t busy, queued;

	if (reached < STEPS) {
		return 0;
	}
	lck_mtx_lock(&mali_queue_lock);
	busy = sec_busy;
	queued = sec_queued;
	*jobs = sec_jobs;
	sec_busy = sec_queued = 0;
	sec_jobs = 0;
	lck_mtx_unlock(&mali_queue_lock);
	absolutetime_to_nanoseconds(busy, busy_ns);
	absolutetime_to_nanoseconds(queued, queued_ns);
	*mhz = pll_mhz();
	*switches = gov_on ? gov_switches : 0;
	*g3d_mc = hakios_exynos_tmu_zone_mc(2);
	HakiosMaliCacheSecond();
	return gov_on ? 2 : 1;
}

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t chain, seq;
		bool fragment;

		lck_mtx_lock(&mali_queue_lock);
		while (mali_queue_count == 0) {
			uint64_t deadline;

			if (!gov_on || gov_level == 0) {
				lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT);
				continue;
			}
			clock_interval_to_deadline(GOV_IDLE_MS, NSEC_PER_MSEC, &deadline);
			if (lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT,
			    deadline) == THREAD_TIMED_OUT && mali_queue_count == 0) {
				lck_mtx_unlock(&mali_queue_lock);
				gov_window(mach_absolute_time(), 0);    /* (idle: the clock may come down) */
				lck_mtx_lock(&mali_queue_lock);
			}
		}
		chain = mali_queue[mali_queue_head].chain;
		seq = mali_queue[mali_queue_head].seq;
		uint64_t queued = mali_queue[mali_queue_head].queued, job_began = mach_absolute_time();
		fragment = mali_queue[mali_queue_head].fragment;
		lck_mtx_unlock(&mali_queue_lock);

		{
			/* how busy the GPU is: every five seconds, its share of the time and by which slot */
			static uint64_t busy[2], since, jobs[2];
			uint64_t began = mach_absolute_time(), now, ns;

			if (hakios_mali_submit(chain, fragment) != 0) {
				gMaliFailed++;
			}
			now = mach_absolute_time();
			if (mali_trace) {
				uint64_t q, b, e;

				absolutetime_to_nanoseconds(queued, &q);
				absolutetime_to_nanoseconds(began, &b);
				absolutetime_to_nanoseconds(now, &e);
				printf("tl: j %llu %c q %llu b %llu e %llu\n", seq, fragment ? 'f' : 'v', q / 1000, b / 1000, e / 1000);
			}
			absolutetime_to_nanoseconds(now - began, &ns);
			busy[fragment] += ns;
			jobs[fragment]++;
			if (since == 0) {
				since = now;
			} else {
				absolutetime_to_nanoseconds(now - since, &ns);
				if (ns >= 5ull * NSEC_PER_SEC) {
					printf("mali: GPU busy %llu%% of %llu s: vertex and tiler %llu jobs %llu ms (%llu us each), "
					    "fragment %llu jobs %llu ms (%llu us each)\n", (busy[0] + busy[1]) * 100 / ns,
					    ns / NSEC_PER_SEC, jobs[0], busy[0] / 1000000, jobs[0] ? busy[0] / 1000 / jobs[0] : 0,
					    jobs[1], busy[1] / 1000000, jobs[1] ? busy[1] / 1000 / jobs[1] : 0);
					busy[0] = busy[1] = jobs[0] = jobs[1] = 0;
					since = now;
				}
			}
		}

		uint64_t ended = mach_absolute_time();

		lck_mtx_lock(&mali_queue_lock);
		mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
		mali_queue_count--;
		mali_done_seq = seq;
		sec_jobs++;
		sec_busy += ended - job_began;
		sec_queued += job_began - queued;
		lck_mtx_unlock(&mali_queue_lock);
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);       /* (room in the queue) */
		gov_window(ended, ended - job_began);
	}
}

/* A job chain into the queue: its number. */
uint64_t
hakios_mali_enqueue(uint64_t chain, bool fragment)
{
	uint64_t seq;

	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		thread_t thread;

		if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "the Mali's jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	while (mali_queue_count == MALI_QUEUE) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_head, THREAD_UNINT);
	}
	seq = ++mali_queued_seq;
	unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

	mali_queue[at].chain = chain;
	mali_queue[at].fragment = fragment;
	mali_queue[at].seq = seq;
	mali_queue[at].queued = mach_absolute_time();
	mali_queue_count++;
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

/* Wait until job `seq` is done: true when it is; with timeout 0 only a look. */
bool
hakios_mali_wait(uint64_t seq, uint64_t timeout_ns)
{
	uint64_t deadline;
	bool done;

	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;     /* (an "infinite" one, INT64_MAX, would wrap) */
	}
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();
	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && timeout_ns != 0 && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
	done = mali_done_seq >= seq;
	lck_mtx_unlock(&mali_queue_lock);
	return done;
}

/* ---- the run ---- */

/* The steps, then, if they all went, once a minute what the GPU has done. */
static void
mali_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("mali: the Galaxy S9's Mali-G72: steps 1 to %u of %u (mali=%u)\n", last_step, STEPS, last_step);
	for (step = 1; step <= last_step; step++) {
		say("mali: step %u: %s\n", step, steps[step - 1].name);
		if (!steps[step - 1].make()) {
			say("mali: step %u: FAILED; the steps after it are not made\n", step);
			break;
		}
		reached = step;
		say("mali: step %u: done\n", step);
	}
	if (job_page != 0) {
		/* (after step 9 programs may be at the GPU already: under the locks, as any change of the tables) */
		lck_mtx_lock(&mali_job_lock);
		lck_mtx_lock(&mali_lock);
		if (pt_top != NULL) {
			unmap_pages(FIRST_JOB_GPU, PAGE_SIZE);  /* nothing of the GPU's to point at the page once it is freed */
			as0_update(pt_transtab, pt_transcfg());
		}
		lck_mtx_unlock(&mali_lock);
		lck_mtx_unlock(&mali_job_lock);
		kmem_free(kernel_map, job_page, PAGE_SIZE);
		job_page = 0;
	}
	if (reached < STEPS) {
		if (powered_here) {
			g3d_off();
		} else if (reached >= 2) {
			say("mali: the G3D domain was on when found: left on%s\n", rail_changed ? ", and BUCK6 at the selected voltage" : "");
		} else if (rail_changed && (rd(pmu, PMU_G3D_STATUS) & G3D_ON) == 0) {
			say("mali: BUCK6 back as it was found: B6OUT %#x\n", b6out_found);
			if (rail_set(b6out_found)) {
				rail_changed = false;
			}
		} else if (rail_changed) {
			/* (the boot's domain, on, and step 2 stopped at the clock: never a lower voltage under a running GPU) */
			say("mali: the G3D domain is on: BUCK6 left at the selected %u uV under it\n", mali_uv);
		}
		say("mali: came up to step %u of %u: not there for programs\n", reached, STEPS);
		return;
	}
	say("mali: came up to step %u of %u: the GPU is there for programs (HakiosMali)\n", reached, STEPS);
	gov_start();
	for (;;) {
		delay(60 * 1000 * 1000);
		printf("mali: step %u of %u; user space says %u%s; %u clients, %u buffers, %u job chains (%u failed), "
		    "%u pictures shown; %llu MB of the GPU's 4 GB of addresses in use\n", reached, STEPS, user_said,
		    user_said == 1 ? " -- a program ran a job on the GPU" : "", gMaliClients, gMaliBuffers, gMaliSubmits,
		    gMaliFailed, gMaliShown, mali_va.taken * MALI_VA_GRANULE >> 20);
		lck_mtx_lock(&kept_lock);
		for (uint32_t i = kept_next > KEPT_LINES ? kept_next - KEPT_LINES : 0; i < kept_next; i++) {
			printf("kept: %s\n", kept_lines[i % KEPT_LINES]);
		}
		lck_mtx_unlock(&kept_lock);
		if (gl_said != 0) {
			printf("mali: gl_test drew on %s; %u%% of the picture inside the triangle (26%% by geometry); "
			    "the middle pixel red %u green %u blue %u alpha %u\n", gl_said / 1000 == 1 ? "THE MALI" : "the processor",
			    gl_said % 1000, gl_middle & 0xff, (gl_middle >> 8) & 0xff, (gl_middle >> 16) & 0xff, gl_middle >> 24);
		}
		if (last_failure.count != 0) {
			printf("mali: the last of %u unfinished: slot %u, chain at %#llx, interrupts %#x, slot status %#x, "
			    "MMU interrupts %#x, fault status %#x at address %#x\n", last_failure.count, last_failure.slot,
			    last_failure.chain, last_failure.raw, last_failure.status, last_failure.mmu, last_failure.fault,
			    last_failure.fault_address);
			printf("mali: the GPU's tables at %#llx (ours at %#llx); for the fault's address: %#llx, %#llx, %#llx\n",
			    last_failure.transtab, pt_transtab, last_failure.walk[0], last_failure.walk[1], last_failure.walk[2]);
			for (uint32_t i = 0; i < gMaliBuffers && i < 32; i++) {
				printf("mali:   buffer %u at %#llx..%#llx (asked %#x, flags %#x)%s\n", i, gMaliMade[i].address,
				    gMaliMade[i].address + gMaliMade[i].size, gMaliMade[i].asked, gMaliMade[i].flags,
				    last_failure.fault_address >= gMaliMade[i].address &&
				    last_failure.fault_address < gMaliMade[i].address + gMaliMade[i].size ? "  <- the fault" : "");
			}
		}
	}
}

/* From hakios_input_init() (drivers/input/hakios.input.c), while the system starts: the steps go on in a thread. */
void
hakios_mali_init(void)
{
	thread_t thread;

	(void)PE_parse_boot_argn("mali_fastmap", &mali_fastmap, sizeof(mali_fastmap));
	(void)PE_parse_boot_argn("mali_trace", &mali_trace, sizeof(mali_trace));
	(void)PE_parse_boot_argn("mali_pll", &mali_pll, sizeof(mali_pll));
	(void)PE_parse_boot_argn("mali_mhz", &mali_mhz, sizeof(mali_mhz));
	(void)PE_parse_boot_argn("mali_acpm", &mali_acpm, sizeof(mali_acpm));
	(void)PE_parse_boot_argn("mali_soc", &mali_soc, sizeof(mali_soc));
	(void)PE_parse_boot_argn("mali_peek", &mali_peek, sizeof(mali_peek));
	if (mali_soc == 9610) {
		mali_pmu_phys = 0x11860000UL;
		mali_pmu_cfg = 0x4060;
		mali_pmu_status = 0x4064;
		mali_cmu_phys = 0x11430000UL;
		mali_pll_con0 = 0x120;
		mali_pll_con2 = 0x128;
		mali_mux_busd = 0x1004;
		mali_dtzpc = 0x11410204u;
		mali_gpu_phys = 0x11500000UL;
		/* the rail: S2MPU09's BUCK5 (vdd_g3d) through ACPM's PMIC channel (hakios.exynos_acpm.c with acpm_soc=9610),
		 * at most the 993.75 mV of the top level of the A50's own fvmap (1053 MHz) */
		mali_rail_ctrl = 0x20;
		mali_rail_out = 0x21;
		mali_rail_most = 0x6f;
	}
	(void)PE_parse_boot_argn("mali_gov", &mali_gov, sizeof(mali_gov));
	(void)PE_parse_boot_argn("mali_nap_us", &mali_nap_us, sizeof(mali_nap_us));
	mali_nap_us = mali_nap_us < 10 ? 10 : mali_nap_us > 1000 ? 1000 : mali_nap_us;
	if (!PE_parse_boot_argn("mali", &last_step, sizeof(last_step)) || last_step == 0) {
		return;
	}
	(void)PE_parse_boot_argn("mali_uv", &mali_uv, sizeof(mali_uv));
	if (mali_uv < 300000 || mali_uv > 950000 || (mali_uv - 300000) % 6250 != 0) {
		say("mali: mali_uv=%u is not a BUCK6 setting (300000..950000, steps of 6250 uV): nothing touched\n", mali_uv);
		return;
	}
	b6out_target = (uint8_t)((mali_uv - 300000) / 6250);
	if (pe_arm_get_soc_base_phys() != STARLTE_SOC_BASE) {
		say("mali: mali= is for the Galaxy S9 (the loader's platform starlte, registers from %#lx), and this is not it "
		    "(%#lx): nothing touched\n", STARLTE_SOC_BASE, (unsigned long)pe_arm_get_soc_base_phys());
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	if (mali_soc == 9610 && last_step > 1 && mali_pll != 0 && !mali_acpm) {
		/* past step 1 only with the boot's clock and supply (mali_pll=0; ACPM is off on the 9610): the 1052X rates and
		 * S2MPU09's BUCK5 are not this file's yet */
		say("mali: mali_soc=9610: past step 1 only with mali_pll=0 (the boot's 260 MHz): mali=%u made 1\n", last_step);
		last_step = 1;
	}
	if (kernel_thread_start(mali_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
