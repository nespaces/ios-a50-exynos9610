/*
 * The phone's UFS host controller, driven from this kernel: one driver for a standard UFS host
 * controller (UFSHCI, JESD223), what each SoC does differently behind a few hooks (struct
 * ufshc_soc below), and two ways of having the controller. hakios.ufshc.h says what it is and
 * compares it with UniXNU's ShengUfs, item by item; what was adopted from that comparison is
 * marked "(ShengUfs)" where it is done here.
 *
 * ---- shared (ufs_native=1: the POCO) ---------------------------------------------------------
 *
 * Reading the storage through the loader (hakios.ufs.c) is one request at a time: Qualcomm's
 * UFSDxe reads synchronously on core 0, and its transfer request list has room for one request
 * only (UTRLBA points at a single descriptor, with UFSDxe's command descriptor right behind it,
 * where slots 8.. would be). The controller takes 32 at once. So for the length of a run this
 * kernel puts a list of its own into the controller: with nothing in flight, the list is stopped
 * (UTRLRSR 0), UTRLBA/UTRLBAU are pointed at the new one, the list is started again; at the end
 * (hakios_ufs_quiesce, from loader_return), what is in flight is waited for, the list stopped,
 * UEFI's address written back, the list started. The list and its command descriptors are in
 * memory the loader owns (it offers it in its page, with UEFI's address); the loader checks the
 * address on every way back from a run and puts UEFI's back itself if the kernel did not
 * (xnuloader/loader.c, hc_take_back), and neither reads nor writes for the kernel or for its log
 * while another list is in. While this kernel's list is in, it asks the loader for nothing.
 *
 * Nothing else of the controller is changed: no reset, no link startup, no UIC command, no
 * interrupt enable, aggregation, clock or power setting; the task management list is not
 * touched; IS and UTRLCNR are only read. UTRLCLR is written only to clear this kernel's own
 * slots when they do not finish at the end.
 *
 * Every slot of this list holds a whitelisted command at all times: TEST UNIT READY until it is
 * used for a read. Slot 0 is kept for the proof that the controller does read this list: after
 * every switch, before any read, slot 0's TEST UNIT READY is rung and must come back in this
 * list (its status written here). Were the controller still reading UEFI's list, the doorbell
 * would run UEFI's slot 0 instead -- so first UEFI's slot 0 must hold a read too (it does: the
 * loader's last use of the storage before a run is reading back the log it wrote), and the
 * proof costs nothing either way. Reads go in slots 1..31.
 *
 * The doorbell is rung only with interrupts off, after looking that this kernel's list is the
 * one in the controller and running, that the slot is free, and that the loader is not taking
 * the controller back (it says so in its page, then waits before it does). The list and the
 * command descriptors are mapped here without caching.
 *
 * ---- own (ufs_native=4; 5 looks only) -----------------------------------------------------------
 *
 * This kernel has the controller for the run, and nothing else uses it meanwhile: the loader's
 * block service (ufs=2) is refused with it. The controller is the SoC's (ufs_native_soc=, below)
 * or the first UFSHCI on the PCI bus (QEMU's -device ufs). It is taken as UEFI left it: enabled,
 * a device present and the list ready, nothing in flight, no fatal status; then the interrupt
 * enable is saved and masked, UEFI's list stopped, this kernel's put in, read back and started
 * (ShengUfs's takeover, with UEFI's list and IE kept to be given back). On QEMU alone, a
 * controller UEFI did not enable is enabled from reset here (HCE, DME_LINKSTARTUP, fDeviceInit):
 * on a phone the PHY, its clocks and power are UEFI's to set up, and that is never done here.
 * Then NOP OUT; the device's power mode (QUERY READ ATTRIBUTE bCurrentPowerMode), and START STOP
 * UNIT to the device's well-known unit if it was left in power-down (ShengUfs); each general
 * logical unit's descriptor (QUERY READ DESCRIPTOR: enabled, boot LU, write protect, block size
 * and count), TEST UNIT READY and READ CAPACITY(16). hakios.ufs.c reads the GPTs.
 *
 * At the end (hakios_ufs_quiesce) what is in flight is waited for, the device's cache
 * synchronized if anything was written, this kernel's list stopped and UEFI's put back with its
 * interrupt enable and the vendor's registers as they were (or, on QEMU, the controller disabled
 * again if it was found so).
 *
 * The memory is this kernel's, physically contiguous and below 4 GB where the controller cannot
 * reach above (CAP's 64-bit bit clear (ShengUfs), or the SoC's DMA mask). The descriptors are
 * cached memory with the caches cleaned before every doorbell and invalidated after: so that no
 * line of the list ever holds a descriptor this kernel is writing beside one the controller is
 * writing back, own mode uses only even slots (a descriptor is 32 bytes, a cache line 64): slot 0
 * for the control commands, 2, 4 ... 30 for reads and writes.
 *
 * ---- what may be sent: the whitelist ------------------------------------------------------------
 *
 * A command is made in a buffer of its own, and only one that passes ufshc_permitted() is ever
 * written into a slot; what the slot then holds is read back and must be the same and pass again
 * before the doorbell is rung. hc_refusal() checks the transaction type, the data direction in
 * the request descriptor, the UPIU's flags, the logical unit and the SCSI operation:
 *   always       READ(10)/READ(16) of a logical unit reads may go to now, with the CDB's length the
 *                UPIU's; TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY(10/16) of one
 *   own mode     NOP OUT; QUERY REQUESTs that read (descriptor, attribute, flag); START STOP UNIT
 *                with power condition ACTIVE, to the device's well-known unit (0xD0) only; the four
 *                above to any general unit; SET FLAG fDeviceInit only while enabling QEMU's
 *                controller from reset; WRITE(10)/WRITE(16) (FUA allowed) and SYNCHRONIZE CACHE(10)
 *                only into the one window ufshc_open_window() opened -- one logical unit, one range
 *                of blocks, the partition ufs_native_write= names -- and never anything else
 *   never        any other well-known unit (REPORT LUNS, BOOT, RPMB), task management, query writes,
 *                UNMAP, FORMAT UNIT, WRITE BUFFER, SECURITY PROTOCOL, MODE SELECT, data host to
 *                device outside the window, the W flag outside it
 * It is tried at the probe on made-up commands that must pass or be refused.
 *
 * ---- the SoCs (ufs_native_soc=) ------------------------------------------------------------------
 *
 * pci (the default)  QEMU's -device ufs: Red Hat 1b36:0013, class 01/09/01, BAR0 its registers. The
 *     PCI layer (bsd/dev/hakios.pci.c) enumerates bus 0 through the ECAM window the loader describes
 *     in /pci0 (QEMU virt: 0x4010000000) and keeps the BARs edk2's PCI bus driver assigned; the
 *     controller is the function of that class, its registers BAR0 (a 64-bit memory BAR), and the
 *     probe prints the BAR as it reads it in the function's configuration space. Standard UFSHCI:
 *     offsets in dwords, PRDT entries of 16 bytes, 256 KB each at most, UTRLCLR clears with zeros.
 * sm7325  the POCO X5 Pro's: ufshc@1d84000 ("ufs_mem" in its Android device tree). Standard as
 *     far as this driver goes: nothing of Qualcomm's vendor registers is needed for a controller
 *     UEFI's UFSDxe has brought up. Used in shared mode (ufs_native=1) as before.
 * exynos9810  the Galaxy S9's, from two GPL-2.0 sources, read for facts and nothing taken: [BSP]
 *     Samsung's kernel for the S9 (the Exynos 9810), and [ML] SheroAbi/galaxy-s9plus-mainline-linux
 *     at bcee6014fc2e44a4c4f1c34bca0a880d4b543ec3, mainline Linux 7.1 on the S9+ (the same SoC) with
 *     UFS read and written, src/ufs/ufs-exynos9810.c and src/mainline-dts/exynos9810-star2lte.dts:
 *     - the registers: ufs@11120000, reg = <0x11120000 0x200> (UFSHCI), <0x11121100 0x200> (the
 *       vendor's HCI registers), <0x11110000 0x8000> (UniPro), <0x11130000 0x100> (the UFS
 *       protector); SPI 242; "dma-coherent", and no iommus property (no SysMMU in front of it)
 *       -- [BSP] arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts, the ufs node; the
 *       same in [ML]'s ufs@11120000, which names them hci, vs_hci, unipro, ufsp, pma (0x11124000)
 *     - the request descriptor's response and PRDT offsets and lengths in bytes, not dwords
 *       (UFSHCD_QUIRK_PRDT_BYTE_GRAN: [BSP] drivers/scsi/ufs/ufs-exynos.c exynos_ufs_set_features,
 *       used in ufshcd.c ufshcd_host_memory_configure and ufshcd_map_sg; [ML] exynos_ufs_init)
 *     - command UPIU 1024 bytes, response UPIU 1024 bytes, then the PRDT (ALIGNED_UPIU_SIZE and
 *       struct utp_transfer_cmd_desc, drivers/scsi/ufs/ufshci.h)
 *     - a PRDT entry's size is the UFS protector's (FMP's) descriptor type, which only the secure
 *       world sets (SMC FMP_SECURITY): type 3 is 128 bytes -- the standard four words, then the
 *       inline encryption's IVs and keys -- type 0 the standard 16. S-Boot leaves type 3 ([ML], the
 *       comment at its SMC_CMD_FMP_SECURITY); [BSP] sets 3 too (exynos_fmp_sec_cfg, CFG_DESCTYPE_3,
 *       drivers/crypto/fmp/fmp.c; struct ufshcd_sg_entry with CONFIG_EXYNOS_FMP, which
 *       arch/arm64/configs/exynos9810-starlte_defconfig sets); [ML] sets 0. This kernel makes no
 *       SMC: it takes whichever UEFI's own reads use (below). The fourth word's bits 28-31 (FAS,
 *       DAS) 0 is no encryption (include/crypto/fmp.h; exynos_fmp_bypass)
 *     - at most 4 KB of data in one PRDT entry: the protector's data unit, in the data path even
 *       on type 0 -- [ML] measured larger segments wedging the doorbell with no error bit (its
 *       ufs_max_seg_size = 4096); [BSP] MAX_AES_XTS_TRANSFER_SIZE, include/crypto/fmp.h; the
 *       vendor's TXPRDT/RXPRDT_ENTRY_SIZE (0x00, 0x04 of its block) set to 12 by both drivers'
 *       host init (exynos_ufs_init_host) and checked here
 *     - HCI_UTRL_NEXUS_TYPE (vendor's 0x40): set to all ones at host init, then before each request
 *       the slot's bit set for a SCSI command and cleared for a query or NOP
 *       ([BSP] exynos_ufs_set_nexus_t_xfer_req, from ufshcd_queuecommand and ufshcd_exec_dev_cmd;
 *       [ML] exynos_ufs_setup_xfer_req)
 *     - UTRLCLR clears a slot with a one (BROKEN_REQ_LIST_CLR: [BSP] ufshcd_utrl_clear, [ML])
 *     - DMA below 4 GB: [BSP] exynos_ufs_dma_mask = DMA_BIT_MASK(32). [ML] runs with 64 bits (its
 *       dma_bits, "32 was a ruled-out experiment") once its entries were 4 KB; this driver keeps
 *       the BSP's 32 until the phone says otherwise. None of XNU's RAM is below 4 GB on the phone, so
 *       own mode's memory is 4 MB of the device tree's gap outside it (UFSHC_S9_MEM_PA, 0xDA000000)
 *     - an error of the device may come back as OCS 7 (UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR, [ML]):
 *       a unit's TEST UNIT READY is tried again on it while the units are looked for
 *     - interrupt aggregation not to be used (UFSHCI_QUIRK_SKIP_INTR_AGGR): not used here anyway
 *     - the Android partitions are in LUN 0's GPT: [ML]'s BOOT is /dev/sda10 and USERDATA
 *       /dev/sda25 (docs/03-installing.md); [BSP]'s FMP node names its block "sda". The general
 *       units are 0-7, the well-known ones REPORT LUNS 0x81, BOOT 0xB0, RPMB 0xC4 and the device
 *       0xD0 ([BSP] drivers/scsi/ufs/ufs.h); which general units are enabled, and which are boot
 *       units, is read from each one's unit descriptor on the phone, not assumed
 *     - for a controller UEFI has brought up nothing else of the vendor's is needed for transfers:
 *       the rest of host init (data reorder, AXI burst, clock gating, the 1 us counter, the
 *       protector's descriptor type and SMU, by SMC) is set up once and is UEFI's (and S-Boot's);
 *       HCI_SW_RST (vendor's 0x50) resets UniPro and the link, and is never written here. [ML]
 *       found transfers stalling with the controller's clock gating on and leaves it off; this
 *       driver writes neither HCI_CLKSTOP_CTRL nor HCI_FORCE_HCS, it prints what UEFI left
 *     Before its list goes out, UEFI's own descriptors are read: a read UEFI left in its list must be
 *     laid out as these facts allow (offsets in bytes; entries of 128 or 16 bytes whose lengths add
 *     up to the UPIU's expected length, the size then taken for this driver's), or the S9 is not
 *     taken. Only a slot holding a UFS storage command (command type 1, or 0 as Silicium's UfsDxe
 *     leaves it) is followed, and only to a command descriptor in DRAM below 4 GB: Silicium's
 *     UfsDxe uses two slots (UFS_NUTRS 2, its UfsDxe.h) and leaves the rest of its page as it found
 *     it, which is no descriptor to map and read. UEFI reuses its slots, so there may be no read of
 *     its left: then the entries are 16 bytes, the size that is harmless if wrong. Either way the
 *     layout is proven before anything else carrying data is sent: after NOP OUT, ONE READ(10) of
 *     LUN 0's blocks 0-1 (two 4 KB PRDT entries) through this driver's own descriptor, into a page
 *     filled with a pattern, must be byte for byte the loader's reads of the same blocks (ufs=3's
 *     references); otherwise UEFI's list is put back and the run goes through the loader.
 *     ufs_native=5 shows all of it and changes nothing.
 *   The blocks beside the controller, which this kernel reads at step 1 and 2 and never writes:
 *     - UniPro is at 0x11110000 (32 KB): the ufs node's third reg in [BSP]'s device tree (reg =
 *       <... 0x11110000 0x8000 ...>), mapped as reg_unipro by ufs-exynos.c exynos_ufs_probe
 *       (platform_get_resource index 2); 0x11130000 is the fourth, the protector (UFSP, 0x100).
 *       Silicium's PlatformUfsLib BoardInit.c puts UniPro at the controller's base + 0x10000, i.e.
 *       0x11130000, and its calibration writes the UNIPRO_DBG_APB rows (the L2 timers, 0x7888 ...
 *       0x78C0) there: this file takes [BSP]'s address, and reads only the results UniPro keeps
 *       of its last link startup, hibernate and power mode change (0x7854, 0x7868, 0x7878, 0x78EC:
 *       ufs-exynos.h; dumped the same way by ufs-exynos-dbg.c), with its version (0x0, 0x4)
 *     - the M-PHY's PMA at 0x11124000 (2 KB, the ufs-phy child), lanes 0x140 apart
 *       (ufs-cal-9810.c PHY_PMA_TRSV_ADDR): after a power mode change to HS-G3 on two lanes the
 *       calibration leaves TRSV 0xC8 = 0xBC, 0x120 = 0xC0, 0x128 = 0x00, 0x134 = 0x63 on each lane
 *       (calib_of_hs_rate_a/_b; PWM leaves 0xC8 = 0x40) and waits for bit 6 of 0x1FC, the CDR's
 *       lock (post_calib_of_hs_rate_*, ufs_cal_wait_cdr_afc_check). Silicium's UfsCal.c has the same
 *       tables. Read only if CLKSTOP_CTRL's MPHY_APBCLK_STOP (bit 3) is clear: the S9's kernel
 *       clears it around every PMA access (ufs-exynos.h phy_pma_readl/writel) and this one does not
 *     - the PHY's power: PMU UFS_PHY_CONTROL at 0x14060724 (the ufs-phy-sys child), bit 0 set is
 *       the isolation bypassed (exynos_ufs_ctrl_phy_pwr; Silicium's UfsDxe sets it the same way)
 *     - IO coherency: SYSREG_FSYS0 0x11010700, bits 8-9 (ufs-io-coherency, mask = bits = 0x300;
 *       exynos_ufs_init_system; Silicium sets them in BoardInit.c). This driver cleans and
 *       invalidates its descriptors and buffers either way
 *     - the device's reset: HCI_GPIO_OUT (vendor 0x70) bit 0, 1 out of reset (exynos_ufs_dev_hw_reset)
 *     - the clocks: GATE_UFS_EMBD and UFS_EMBD from the CMU ([BSP] device tree clocks), the UniPro
 *       clock 166 MHz on Silicium (MclkRate; HCI_1US_TO_CNT_VAL 166, vendor 0x0C), and the
 *       vendor's gating: CLKSTOP_CTRL (0xB0; UniPro PCLK bit 0, MCLK 1, REFCLK 2, MPHY APB 3,
 *       REFCLKOUT 4) and FORCE_HCS (0xB4, the gating enables; Silicium writes 0x0DE0). UniPro's
 *       main clock and the reference clock stopped is a refusal; the CMU itself is not read
 *     - the protector (FMP): bypassed by clearing FAS and DAS, bits 28-31 of a PRDT entry's
 *       fourth word (include/crypto/fmp.h SET_FAS/SET_DAS for the non-CMDQ table,
 *       drivers/crypto/fmp/fmp.c fmplib_bypass, used by ufs-exynos-fmp.c for every request without
 *       a key): this driver always does. Its SMU and descriptor type are the secure world's (SMC
 *       FMP_SECURITY, SMU_INIT, include/linux/smc.h) and are not touched
 *   Reused, not reset. UEFI has brought all of it up, and bringing it up again is not this kernel's
 *   to do: [BSP]'s host reset is HCI_SW_RST, then the vendor's host init, the device's reset line,
 *   and its link startup runs the calibration's pre-link table (UniPro debug MIBs, PCS and PMA
 *   writes), DME_LINKSTARTUP, the post-link table, then a power mode change to HS-G3 with its own
 *   tables and the CDR waits (ufs-exynos.c exynos_ufs_host_reset, link_startup_notify,
 *   pwr_change_notify; ufs-cal-9810.c) -- dozens of writes to UniPro and the PHY besides the
 *   controller, timing-dependent, none of which can be read back to prove it right before the
 *   flash is spoken to; Silicium's UfsDxe did exactly that sequence already, and the loader and
 *   UEFI go on using the controller after a run, so a reset here would leave UEFI a controller
 *   it no longer knows. The S9 is taken as UEFI left it, read back at steps 1 and 2 against the
 *   facts above, and given back as it was.
 *
 * ---- own mode in steps (ufs_native_step=) ---------------------------------------------------------
 *
 * Each step says what it found before the next is taken; each stops on its own, and a step that
 * stops gives the controller back as it was found:
 *   1  look: the registers, the vendor's, the blocks beside (the SoC's look), UEFI's descriptors;
 *      nothing written (as ufs_native=5)
 *   2  the link: the host's PA attributes by DME_GET (lanes, gears, power mode, series) and, on the
 *      S9, UniPro's results and the PHY's calibration read back; nothing written but DME_GET's
 *      own registers (UICCMD and its arguments, IS's UCCS)
 *   3  NOP OUT: this kernel's list in, NOP OUT answered by NOP IN, the list given back
 *   4  READ CAPACITY: the device's power mode, each unit's descriptor, TEST UNIT READY and READ
 *      CAPACITY(16), the list given back
 *   5  all: LBA 0 and each unit's GPT read (hakios.ufs.c), /dev/ufs_NAME made, reads served
 * The S9's default is 1; QEMU's and the POCO's 5.
 *
 * Checked: shared mode run on the POCO (as before this file was made generic). Own mode run in
 * QEMU 11.1 on -device ufs with two ufs-lu units. exynos9810: own mode run on the phone through
 * ufs=3 (hakios.ufs.c takes the controller after its self-test against the loader's reads; kernel
 * PR 155), the S9's default, with /private/var kept in USERDATA. ufs_native=4
 * ufs_native_soc=exynos9810 given by hand (no self-test) starts at step 1 unless ufs_native_step=
 * says otherwise; nothing uses it.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/hakios.pci.h>
#include <sys/hakios.exynos9810_mem.h>
#include <sys/hakios.physmap.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include "hakios.ufshc.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern boolean_t pmap_valid_address(uint64_t addr);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void IOSleep(unsigned int milliseconds);
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);

#define UFSHC_SIZE          0x3000

/* UFSHCI registers (JESD223). 0x38..0x48, the UIC error codes, clear when read: never read here. */
#define UFSHC_CAP           0x00
#define UFSHC_VER           0x08
#define UFSHC_HCDDID        0x10
#define UFSHC_HCPMID        0x14
#define UFSHC_AHIT          0x18
#define UFSHC_IS            0x20
#define UFSHC_IE            0x24
#define UFSHC_HCS           0x30
#define UFSHC_HCE           0x34
#define UFSHC_UTRIACR       0x4c
#define UFSHC_UTRLBA        0x50
#define UFSHC_UTRLBAU       0x54
#define UFSHC_UTRLDBR       0x58
#define UFSHC_UTRLCLR       0x5c
#define UFSHC_UTRLRSR       0x60
#define UFSHC_UTRLCNR       0x64
#define UFSHC_UTMRLBA       0x70
#define UFSHC_UTMRLBAU      0x74
#define UFSHC_UTMRLDBR      0x78
#define UFSHC_UTMRLRSR      0x80
#define UFSHC_UICCMD        0x90
#define UFSHC_UICCMDARG1    0x94
#define UFSHC_UICCMDARG2    0x98
#define UFSHC_UICCMDARG3    0x9c

#define CAP_NUTRS(c)        (((c) & 0x1f) + 1)
#define CAP_64AS            (1u << 24)
#define HCS_DP              (1u << 0)
#define HCS_UTRLRDY         (1u << 1)
#define HCS_UTMRLRDY        (1u << 2)
#define HCS_UCRDY           (1u << 3)
#define IS_UTRCS            (1u << 0)
#define IS_UE               (1u << 2)
#define IS_UCCS             (1u << 10)
/* ULLS, DFES, UTPES, HCFES, SBFES, CEFES (ShengUfs: link lost and the crypto engine are fatal, a UIC error not) */
#define IS_FATAL            ((1u << 7) | (1u << 11) | (1u << 12) | (1u << 16) | (1u << 17) | (1u << 18))
#define UIC_LINK_STARTUP    0x16
#define UIC_DME_GET         0x01                /* the one other UIC command sent here: a local attribute read */

/* the request descriptor's first word: command type, data direction */
#define UTRD_CT_MASK        0xf0000000u
#define UTRD_CT_UFS         (1u << 28)
#define UTRD_DD_MASK        (3u << 25)
#define UTRD_DD_NONE        (0u << 25)
#define UTRD_DD_OUT         (1u << 25)          /* host to device: own mode's window only */
#define UTRD_DD_IN          (2u << 25)
#define OCS_SUCCESS         0x0
#define OCS_INVALID         0xf

#define UPIU_NOP_OUT        0x00
#define UPIU_COMMAND        0x01
#define UPIU_QUERY          0x16
#define UPIU_NOP_IN         0x20
#define UPIU_RESPONSE       0x21
#define UPIU_QUERY_RSP      0x36
#define UPIU_FLAG_READ      0x40
#define UPIU_FLAG_WRITE     0x20
#define LUN_WELL_KNOWN      0x80
#define LUN_DEVICE          0xd0                /* the UFS device well-known unit */
#define QUERY_READ          0x01                /* query functions */
#define QUERY_WRITE         0x81
#define QOP_READ_DESC       0x01
#define QOP_READ_ATTR       0x03
#define QOP_READ_FLAG       0x05
#define QOP_SET_FLAG        0x06
#define QFLAG_DEVICE_INIT   0x01
#define QATTR_POWER_MODE    0x02
#define QDESC_UNIT          0x02
#define POWER_ACTIVE        0x11

#define SLOTS               32                  /* this kernel's list */
#define SLOT_CONTROL        0                   /* shared: the proof; own: the control commands */
#define SLOT_BUFFER_DEFAULT (256u << 10)
#define BLOCK               4096u               /* the logical block size of every unit here */

#define TIMEOUT_MS          10000

/* ---- the SoCs ---- */

/*
 * The S9's own-mode memory (the list, the command descriptors, the slots' buffers): none of XNU's RAM is below
 * 4 GB on the phone (the loader gives XNU the bank from 0x880000000), and the controller is kept to 32-bit
 * addresses (the BSP's DMA mask). So it is physical memory of the lower bank that XNU does not manage: 4 MB from
 * 0xDA000000, UFS's range in the table of that bank, sys/hakios.exynos9810_mem.h (the payload, the display's and the
 * camera's below it). Mapped once, write-combined, through IOKit (hakios_physmap_wc(), sys/hakios.physmap.h), never
 * ml_io_map() (io_submap is 8 MB).
 */
#define UFSHC_S9_MEM_PA     EXYNOS9810_MEM_UFS_PA
#define UFSHC_S9_MEM_BYTES  EXYNOS9810_MEM_SIZE(UFS)
/*
 * The A50's (Exynos 9610): XNU has the bank from 0x880000000 alone here too, and 4 MB at the same 0xDA000000 of the
 * lower bank is outside everything the loader keeps out (xnuloader/loader.c, platform_a50: the framebuffer at
 * 0xCA000000, the payload moved to 0xD0000000..0xD3FFFFFF, ABOX from 0xE9400000).
 */
#define UFSHC_A50_MEM_PA    0xDA000000ULL
#define UFSHC_A50_MEM_BYTES 0x400000ULL

struct ufshc_soc {
	const char *name;
	uint64_t pa, size;                      /* the UFSHCI registers; 0: the first UFSHCI on the PCI bus */
	uint64_t vs_pa, vs_size;                /* the vendor's registers beside them; 0: none used */
	bool may_enable;                        /* this kernel may enable it from reset (QEMU's alone) */
	bool prdt_bytes;                        /* the descriptor's offsets and lengths in bytes, not dwords */
	uint32_t prd_entry, prd_max;            /* bytes of a PRDT entry; the most data one describes */
	uint32_t rsp_offset, rsp_length, prdt_offset;   /* in the command descriptor */
	bool clear_with_ones;                   /* UTRLCLR: a one clears the slot */
	bool dma32;                             /* the controller reaches the first 4 GB only */
	uint32_t slot_buffer;                   /* own mode: bytes a slot reads or writes at a time */
	uint32_t default_step;                  /* own mode: ufs_native_step= when none is given (1 look ... 5 all) */
	uint64_t dma_lo, dma_hi;                /* where UEFI's descriptors can be (followed only there); 0, 0: anywhere */
	uint64_t carve_pa, carve_size;          /* own mode's memory: physical memory XNU does not manage; 0: XNU's own */
	uint32_t default_slots;                 /* own mode: requests in flight at once unless ufs_native_slots= says; 0: 15 */
	bool cnr;                               /* has UTRLCNR below UFSHCI 3.0 (cleared per slot as 3.0's) */
	const char *(*look)(bool link);         /* own mode: the blocks beside the controller, read only; why not */
	const char *(*adopt)(void);             /* own mode: the vendor's registers looked at and kept; why not */
	void (*nexus)(uint32_t slot, bool scsi);        /* before every doorbell */
	void (*give_back)(void);                /* at the end: what adopt kept */
	uint64_t unipro_pa, pma_pa;             /* exynos: UniPro and the M-PHY's PMA, read by look() */
	uint64_t pmu_phy_pa, coh_pa;            /* exynos: PMU UFS_PHY_CONTROL; SYSREG_FSYS's coherency bits 8-9 */
	uint32_t lanes;                         /* exynos: the lanes the device tree asks for (PMA read per lane) */
};

static const char *exynos_look(bool link);
static const char *exynos_adopt(void);
static void exynos_nexus(uint32_t slot, bool scsi);
static void exynos_give_back(void);

static const struct ufshc_soc hc_socs[] = {
	{
		.name = "pci", .may_enable = true, .prd_entry = 16, .prd_max = 256u << 10,
		.rsp_offset = 512, .rsp_length = 512, .prdt_offset = 1024, .slot_buffer = SLOT_BUFFER_DEFAULT,
		.default_step = 5,
	},
	{
		.name = "sm7325", .pa = 0x1d84000, .size = UFSHC_SIZE, .prd_entry = 16, .prd_max = 256u << 10,
		.rsp_offset = 512, .rsp_length = 512, .prdt_offset = 1024, .slot_buffer = SLOT_BUFFER_DEFAULT,
		.default_step = 5,
	},
	{
		.name = "exynos9810", .pa = 0x11120000, .size = 0x200, .vs_pa = 0x11121100, .vs_size = 0x200,
		.prdt_bytes = true, .prd_entry = 128, .prd_max = 4096, .rsp_offset = 1024, .rsp_length = 1024,
		.prdt_offset = 2048, .clear_with_ones = true, .dma32 = true, .slot_buffer = 64u << 10,
		.default_step = 1, .dma_lo = 0x80000000ULL, .dma_hi = 0x100000000ULL,
		.carve_pa = UFSHC_S9_MEM_PA, .carve_size = UFSHC_S9_MEM_BYTES,
		/*
		 * One request at a time, as Silicium's UfsDxe does (ufs-ro4 on the phone: 57 reads, two in flight at
		 * once, then slot 4's 64 KB read never cleared its doorbell bit, with IS's UTRCS set and its bit in
		 * 0x64 (UTRLCNR, which this 2.1 controller has: it read 00000014) set). UTRLCNR cleared per slot.
		 */
		.default_slots = 1, .cnr = true,
		.look = exynos_look, .adopt = exynos_adopt, .nexus = exynos_nexus, .give_back = exynos_give_back,
		.unipro_pa = 0x11110000ULL, .pma_pa = 0x11124000ULL, .pmu_phy_pa = 0x14060724ULL, .coh_pa = 0x11010700ULL,
		.lanes = 2,
	},
	{
		/*
		 * The Galaxy A50's (SM-A505FN): its device tree's ufs@0x13520000 (HCI 0x13520000, vendor 0x13521100,
		 * UniPro 0x13510000, ufs-phy 0x13524000; one lane at HS-G3), PMU 0x11860000 + 0x724 (ufs-exynos.c's
		 * default cxt_iso), SYSREG_FSYS 0x13410000 + 0x1010 (ufs-dma-coherency). PRDT entries of 16 bytes (FMP off on
		 * the A50: what ufs_native_prd=16 found on the phone) and step 5 by default. The same controller family as the
		 * 9810's in Samsung's 4.14 kernel for the A50 (drivers/scsi/ufs/ufs-exynos.c: PRDT entry size 12, byte
		 * granular, DMA mask 32 bits): taken as S-Boot left it.
		 */
		.name = "exynos9610", .pa = 0x13520000, .size = 0x200, .vs_pa = 0x13521100, .vs_size = 0x200,
		.prdt_bytes = true, .prd_entry = 16, .prd_max = 4096, .rsp_offset = 1024, .rsp_length = 1024,
		.prdt_offset = 2048, .clear_with_ones = true, .dma32 = true, .slot_buffer = 64u << 10,
		.default_step = 5, .dma_lo = 0x80000000ULL, .dma_hi = 0x100000000ULL,
		.carve_pa = UFSHC_A50_MEM_PA, .carve_size = UFSHC_A50_MEM_BYTES,
		.default_slots = 1, .cnr = true,
		.look = exynos_look, .adopt = exynos_adopt, .nexus = exynos_nexus, .give_back = exynos_give_back,
		.unipro_pa = 0x13510000ULL, .pma_pa = 0x13524000ULL, .pmu_phy_pa = 0x11860724ULL, .coh_pa = 0x13411010ULL,
		.lanes = 1,
	},
};
#define SOC_SHARED          (&hc_socs[1])       /* the one shared mode knows: the POCO's */

struct hc_cmd {
	uint8_t upiu[32];
	uint32_t dw0;
};

struct hc_slot {
	uint32_t number;                        /* in this kernel's list */
	volatile uint32_t *utrd;                /* its descriptor */
	volatile uint32_t *ucd;                 /* its command descriptor */
	uint64_t ucd_pa;
	uint8_t *data;                          /* this kernel's memory, cached */
	uint64_t data_pa;
	uint32_t data_size;
	uint64_t rung, done_at;                 /* mach_absolute_time at its doorbell, and when seen done */
	uint32_t cnr_rung[2];                   /* UTRLCNR before and after its bit was cleared at the doorbell */
};

/* what may be sent now (hc_refusal) */
struct hc_policy {
	uint32_t read_luns;                     /* the general units reads may go to */
	bool control;                           /* own mode: NOP, query reads, START STOP UNIT (ACTIVE) to the device */
	bool device_init;                       /* own mode, enabling QEMU's controller: SET FLAG fDeviceInit */
	bool window;                            /* own mode: writes into [w_first, w_first + w_blocks) of w_lun */
	uint8_t w_lun;
	uint64_t w_first, w_blocks;
	/*
	 * ufs=3 (hakios.ufs.c): checked on every write and SYNCHRONIZE CACHE after the window's own
	 * check, so that a window opened wrong can still not reach them. ro: no write at all
	 * (ufs_native_ro=1); deny_luns: whole logical units never written (every unit but the one
	 * USERDATA is on: the others hold the boot loaders); deny: ranges of blocks never written,
	 * whatever the window says (LBA 0-5, both GPTs, EFS, RADIO, BOOT and the rest, by name).
	 */
	bool ro;
	uint32_t deny_luns;
	const struct hc_deny *deny;
	uint32_t ndeny;
};

/* a range of blocks no write may touch (ufshc_deny) */
struct hc_deny {
	uint8_t lun;
	uint64_t first, blocks;
	char what[40];                          /* "protected: EFS", said in every refusal */
};

#define HC_DENY_MAX         96

enum { HC_NONE, HC_SHARED, HC_OWN };

static vm_offset_t hc;                          /* the registers */
static vm_offset_t hc_vs;                       /* the vendor's */
static const struct ufshc_soc *hc_soc = SOC_SHARED;
static struct ufshc_soc hc_soc_own;             /* own mode: the SoC's, with what UEFI's own read showed */
static bool hc_qemu_ufs;                        /* the controller is QEMU's -device ufs (1b36:0013): see hc_scsi_result */
static int hc_mode;
static struct ufshc_loader hc_loader;
static uint32_t hc_list_lo, hc_list_hi;         /* this kernel's list's address */
static volatile uint32_t *hc_uefi_list;         /* UEFI's list, not cached here, only read */
static uint64_t hc_uefi_map_pa, hc_uefi_map_size;
static vm_offset_t hc_uefi_map_va;
static struct hc_slot hc_slots[SLOTS];
static uint32_t hc_ucd_size;                    /* own mode: bytes of a command descriptor */
static uint32_t hc_nslots;                      /* the slots that carry reads (and writes) */
static uint32_t hc_free;                        /* bit i: hc_slots[i] free */
static uint32_t hc_slot_buffer = SLOT_BUFFER_DEFAULT;
static struct hc_policy hc_pol = { .read_luns = 1 };    /* LUN 0, until hakios.ufs.c says */
static struct hc_deny hc_denied[HC_DENY_MAX];           /* hc_pol.deny: only ever added to, under hc_lock */
static bool hc_direct;                                  /* ufs=3: every line said into the loader's log as well */
static volatile uint32_t *hc_own_list_in;               /* ufs=3: the loader's page's "the kernel's list is in" */
static bool hc_usable;
static uint32_t hc_leave;                       /* ufs_native_leave=1 (a test): the list left in at the end, for the loader */
static volatile bool hc_in, hc_closed, hc_dead;
static uint32_t hc_is_in, hc_cnr_in, hc_iacr_in; /* IS, UTRLCNR, UTRIACR when this kernel's list went in */
static uint64_t hc_requests, hc_errors, hc_bytes, hc_wait_us, hc_refused, hc_most_in_flight, hc_proofs;
static uint64_t hc_switch_in_count, hc_switch_out_count;
static uint32_t hc_in_flight;
/* where the time goes: doorbell reads while waiting, waits for a free slot, taking hc_lock, how many in flight */
static uint64_t hc_polls, hc_slot_waits, hc_slot_wait_ticks, hc_lock_ticks, hc_flight_hist[6];
/* own mode */
static uint32_t hc_nutrs, hc_ver;
static bool hc_enabled_here, hc_dirty;
static uint32_t hc_uefi_ie, hc_uefi_rsr;
static struct ufshc_unit hc_units[UFSHC_LUNS];
static uint32_t hc_step = 5, hc_stopped_at;     /* ufs_native_step=: how far own mode goes; where it stopped as asked */
static uint64_t hc_writes, hc_write_bytes, hc_protocol_errors;
static char hc_take_why[160];                   /* ufs=3: why own mode was not taken (ufshc_take_why) */
static const uint8_t *hc_layout_ref;            /* ufs=3: the loader's blocks 0.. of its unit 0 (ufshc_take) */
static uint32_t hc_layout_ref_blocks;
/*
 * ufs_native_ref_fnv=: the reference as a hash instead, FNV-1a (64 bits) of LUN 0's blocks 0-1 as read on the host
 * (the A50: no UEFI, so no loader that reads them; TWRP's dd of /dev/block/sda, 8192 bytes). ufs_native_prd=: the
 * PRDT entry size when UEFI left no read of its to tell it by (16, or 128 as the Exynos FMP's); the layout read
 * proves either.
 */
static uint64_t hc_layout_ref_fnv;
static uint32_t hc_prd_arg;

static LCK_GRP_DECLARE(hc_lck_grp, "hakios-ufshc");
static LCK_MTX_DECLARE(hc_lock, &hc_lck_grp);
static LCK_MTX_DECLARE(hc_ctl_lock, &hc_lck_grp);       /* own mode: slot 0, one control command at a time */

SYSCTL_DECL(_kern_hakios);
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_requests,
    "reads and writes this kernel's own UFS driver has had answered");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_errors,
    "reads and writes of this kernel's own UFS driver that failed");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_bytes,
    "bytes this kernel's own UFS driver has read and written");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_wait_us,
    "microseconds spent waiting for the controller, summed over the reads");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_refused,
    "commands the whitelist refused (there should never be one but the self test's)");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_most_in_flight, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_most_in_flight,
    "the most reads this kernel's own UFS driver has had in flight at once");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_writes, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_writes,
    "own mode: writes into the partition ufs_native_write= names");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_write_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &hc_write_bytes,
    "own mode: bytes written into the partition ufs_native_write= names");

/*
 * What this file says goes to the kernel's log, and the last lines are kept: the start of a long
 * run's log does not come back to the loader (the buffer wraps), so they are said again at the
 * end, into the loader's log directly (hakios_ufs_quiesce; printf is not for that moment).
 */
extern void loader_log_line(const char *line);
#define KEPT_LINES          64
#define LINE                320
static char hc_kept[KEPT_LINES][LINE];
static uint32_t hc_kept_count;

static void
hc_say(const char *format, ...) __printflike(1, 2);
static void
hc_say(const char *format, ...)
{
	char line[LINE];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	if (strlen(line) == sizeof(line) - 1) {
		line[sizeof(line) - 2] = '\n';        /* (cut, but still a line of its own) */
	}
	va_end(list);
	printf("%s", line);
	if (hc_direct) {
		loader_log_line(line);  /* ufs=3: into the loader's log as it happens (printf does not reach it) */
		return;
	}
	strlcpy(hc_kept[hc_kept_count++ % KEPT_LINES], line, sizeof(hc_kept[0]));
}

/* the same, at the end: into the loader's log only */
static void
hc_say_late(const char *format, ...) __printflike(1, 2);
static void
hc_say_late(const char *format, ...)
{
	char line[LINE];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	if (strlen(line) == sizeof(line) - 1) {
		line[sizeof(line) - 2] = '\n';
	}
	va_end(list);
	loader_log_line(line);
}

static inline uint32_t
hc_read(uint32_t reg)
{
	return *(volatile uint32_t *)(hc + reg);
}

static inline void
hc_write(uint32_t reg, uint32_t value)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(hc + reg) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
vs_read(uint32_t reg)
{
	return *(volatile uint32_t *)(hc_vs + reg);
}

static inline void
vs_write(uint32_t reg, uint32_t value)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(hc_vs + reg) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_us_since(uint64_t start)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return ns / 1000;
}

static uint32_t
be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
be64(const volatile uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

/* own mode's descriptors are cached memory: out to the controller before a doorbell, in again after */
static void
hc_dma_sync(const volatile void *p, size_t bytes)
{
	if (hc_mode == HC_OWN) {
		FlushPoC_DcacheRegion((vm_offset_t)p, bytes);
	}
}

/* ---- the Exynos 9810's vendor registers (the facts are in this file's head) ---- */

#define EXY_TXPRDT_ENTRY_SIZE   0x00
#define EXY_RXPRDT_ENTRY_SIZE   0x04
#define EXY_UTRL_NEXUS_TYPE     0x40
#define EXY_UTMRL_NEXUS_TYPE    0x44
#define EXY_SW_RST              0x50
#define EXY_DATA_REORDER        0x60
#define EXY_AXIDMA_BURST        0x6c
#define EXY_CLKSTOP_CTRL        0xb0
#define EXY_FORCE_HCS           0xb4
#define EXY_PRDT_SIZE(v)        ((v) & 0x1f)
#define EXY_1US_TO_CNT_VAL      0x0c
#define EXY_VENDOR_IS           0x38
#define EXY_GPIO_OUT            0x70            /* bit 0: the device's RST_n, 1 out of reset */
#define EXY_V2P1_CTRL           0x8c            /* bit 16: IA_TICK_SEL */
#define EXY_UFS_ACG_DISABLE     0xfc
#define EXY_IOP_ACG_DISABLE     0x100
#define EXY_MPHY_REFCLK_SEL     0x108
#define EXY_CLKSTOP_UNIPRO_PCLK (1u << 0)       /* CLKSTOP_CTRL: a one stops the clock */
#define EXY_CLKSTOP_UNIPRO_MCLK (1u << 1)
#define EXY_CLKSTOP_REFCLK      (1u << 2)
#define EXY_CLKSTOP_MPHY_APB    (1u << 3)

/* the blocks beside the controller, read and never written here */
#define EXY_UNIPRO_PA           (hc_soc->unipro_pa)     /* the ufs node's third reg, 32 KB */
#define EXY_UNIPRO_SIZE         0x8000
#define EXY_PMA_PA              (hc_soc->pma_pa)        /* ufs-phy, 2 KB: the M-PHY's PMA */
#define EXY_PMA_SIZE            0x800
#define EXY_PMU_PHY_PA          (hc_soc->pmu_phy_pa)    /* ufs-phy-sys: PMU UFS_PHY_CONTROL, bit 0 the isolation bypassed */
#define EXY_SYSREG_COH_PA       (hc_soc->coh_pa)        /* ufs-io-coherency: SYSREG_FSYS0, bits 8-9 */
#define EXY_SYSREG_COH_BITS     0x300u
#define UNIP_COMP_VERSION       0x0000
#define UNIP_COMP_INFO          0x0004
#define UNIP_LINKSTARTUP_CNF    0x7854
#define UNIP_H8_ENTER_IND       0x7868
#define UNIP_H8_EXIT_IND        0x7878
#define UNIP_PWR_IND            0x78ec
#define PMA_TRSV(reg, lane)     ((reg) + 0x140u * (lane))
#define PMA_CDR_AFC             0x1fc           /* bit 6: the lane's CDR has locked (the calibration's wait) */
#define PMA_CDR_AFC_DONE        0x40u

static uint32_t exy_nexus_at_start;

static uint32_t
exy_peek(uint64_t pa)
{
	vm_offset_t va = ml_io_map((vm_offset_t)(pa & ~(uint64_t)PAGE_MASK), PAGE_SIZE);

	return va == 0 ? 0xdeadbeef : *(volatile uint32_t *)(va + (pa & PAGE_MASK));
}

/*
 * Step 1 (and every later one): the power, reset, clocks and coherency the controller needs and
 * UEFI set up, read and compared with what the S9's kernel sets. Step 2 (link): UniPro's own
 * results of the last link startup and power mode change, and the M-PHY's PMA as the
 * calibration for HS-G3 on two lanes leaves it. Nothing is written: a block whose clock
 * CLKSTOP_CTRL says is stopped is not read at all (the S9's kernel ungates the PMA's APB clock
 * around every access to it, ufs-exynos.h phy_pma_readl; this kernel does not), it is said so.
 */
static const char *
exynos_look(bool link)
{
	uint32_t pmu = exy_peek(EXY_PMU_PHY_PA), coh = exy_peek(EXY_SYSREG_COH_PA);
	uint32_t gpio = vs_read(EXY_GPIO_OUT), stop = vs_read(EXY_CLKSTOP_CTRL), force = vs_read(EXY_FORCE_HCS);
	uint32_t cnt = vs_read(EXY_1US_TO_CNT_VAL), v2p1 = vs_read(EXY_V2P1_CTRL);

	if (!link) {
		hc_say("ufs: own: exynos: PMU UFS_PHY_CONTROL %08x (isolation %s), SYSREG_FSYS0 coherency %08x (bits 8-9 %s), "
		    "GPIO_OUT %08x (the device %s), CLKSTOP_CTRL %08x, FORCE_HCS %08x, UFS_ACG_DISABLE %08x, IOP_ACG_DISABLE %08x, "
		    "MPHY_REFCLK_SEL %08x, 1US_TO_CNT %u (%u MHz), V2P1_CTRL %08x, vendor IS %08x\n", pmu,
		    (pmu & 1) ? "bypassed: the PHY is on" : "ON: THE PHY IS OFF", coh, (coh & EXY_SYSREG_COH_BITS) ==
		    EXY_SYSREG_COH_BITS ? "set: coherent" : "NOT SET", gpio, (gpio & 1) ? "out of reset" : "HELD IN RESET", stop,
		    force, vs_read(EXY_UFS_ACG_DISABLE), vs_read(EXY_IOP_ACG_DISABLE), vs_read(EXY_MPHY_REFCLK_SEL),
		    cnt & 0x3ff, cnt & 0x3ff, v2p1, vs_read(EXY_VENDOR_IS));
		if ((pmu & 1) == 0) {
			return "the PHY is isolated (PMU UFS_PHY_CONTROL bit 0 clear): UEFI has not powered it";
		}
		if ((gpio & 1) == 0) {
			return "the UFS device is held in reset (HCI_GPIO_OUT bit 0 clear)";
		}
		if (stop & (EXY_CLKSTOP_UNIPRO_MCLK | EXY_CLKSTOP_REFCLK)) {
			return "UniPro's main clock or the reference clock is stopped (CLKSTOP_CTRL)";
		}
		return NULL;
	}

	if (stop & EXY_CLKSTOP_UNIPRO_PCLK) {
		hc_say("ufs: own: exynos: UniPro at %llx not read: its APB clock is stopped (CLKSTOP_CTRL %08x)\n", EXY_UNIPRO_PA,
		    stop);
	} else {
		vm_offset_t u = ml_io_map((vm_offset_t)EXY_UNIPRO_PA, EXY_UNIPRO_SIZE);

		if (u != 0) {
#define UNIP(r) (*(volatile uint32_t *)(u + (r)))
			hc_say("ufs: own: exynos: UniPro at %llx: COMP_VERSION %08x, COMP_INFO %08x; results of the last "
			    "DME_LINKSTARTUP %x (0: success), hibernate enter %x, exit %x, power mode change %x (1, PWR_LOCAL: "
			    "success)\n", EXY_UNIPRO_PA, UNIP(UNIP_COMP_VERSION), UNIP(UNIP_COMP_INFO),
			    UNIP(UNIP_LINKSTARTUP_CNF), UNIP(UNIP_H8_ENTER_IND), UNIP(UNIP_H8_EXIT_IND), UNIP(UNIP_PWR_IND));
#undef UNIP
		}
	}
	if (stop & EXY_CLKSTOP_MPHY_APB) {
		hc_say("ufs: own: exynos: the PHY's PMA at %llx not read: its APB clock is stopped (CLKSTOP_CTRL %08x)\n",
		    EXY_PMA_PA, stop);
	} else {
		vm_offset_t m = ml_io_map((vm_offset_t)EXY_PMA_PA, EXY_PMA_SIZE);

		for (uint32_t lane = 0; m != 0 && lane < hc_soc->lanes; lane++) {
#define PMA(r) (*(volatile uint32_t *)(m + PMA_TRSV(r, lane)) & 0xff)
			uint32_t c8 = PMA(0xc8), r120 = PMA(0x120), r128 = PMA(0x128), r134 = PMA(0x134), afc = PMA(PMA_CDR_AFC);
#undef PMA
			bool hs3 = c8 == 0xbc && r120 == 0xc0 && r128 == 0x00 && r134 == 0x63;

			hc_say("ufs: own: exynos: PMA lane %u: TRSV C8 %02x, 120 %02x, 128 %02x, 134 %02x (%s), CDR AFC %02x (%s)\n",
			    lane, c8, r120, r128, r134, hs3 ? "HS-G3's calibration on two lanes" : c8 == 0x40 ? "PWM's calibration" :
			    "NOT the calibration of HS-G3 on two lanes", afc, (afc & PMA_CDR_AFC_DONE) ? "locked" : "NOT LOCKED");
		}
	}
	return NULL;
}

static const char *
exynos_adopt(void)
{
	uint32_t tx = vs_read(EXY_TXPRDT_ENTRY_SIZE), rx = vs_read(EXY_RXPRDT_ENTRY_SIZE);
	uint32_t rst = vs_read(EXY_SW_RST);

	exy_nexus_at_start = vs_read(EXY_UTRL_NEXUS_TYPE);
	hc_say("ufs: own: exynos: TXPRDT_ENTRY_SIZE %08x, RXPRDT_ENTRY_SIZE %08x, UTRL_NEXUS_TYPE %08x, UTMRL_NEXUS_TYPE %08x, "
	    "SW_RST %08x, DATA_REORDER %08x, AXIDMA_RWDATA_BURST_LEN %08x, CLKSTOP_CTRL %08x, FORCE_HCS %08x\n", tx, rx,
	    exy_nexus_at_start, vs_read(EXY_UTMRL_NEXUS_TYPE), rst, vs_read(EXY_DATA_REORDER), vs_read(EXY_AXIDMA_BURST),
	    vs_read(EXY_CLKSTOP_CTRL), vs_read(EXY_FORCE_HCS));
	if (EXY_PRDT_SIZE(tx) != 12 || EXY_PRDT_SIZE(rx) != 12) {
		return "the PRDT entry size registers are not the 12 the S9's own kernel sets";
	}
	if (rst & 3) {
		return "UniPro or the link is held in software reset (HCI_SW_RST)";
	}
	return NULL;
}

/* the slot's nexus type: one for a SCSI command, zero for a query or NOP (hc_lock held, interrupts off) */
static void
exynos_nexus(uint32_t slot, bool scsi)
{
	uint32_t v = vs_read(EXY_UTRL_NEXUS_TYPE), bit = 1u << slot;

	if (((v & bit) != 0) != scsi) {
		vs_write(EXY_UTRL_NEXUS_TYPE, scsi ? v | bit : v & ~bit);
	}
}

static void
exynos_give_back(void)
{
	vs_write(EXY_UTRL_NEXUS_TYPE, exy_nexus_at_start);
}

/* ---- the whitelist: the one check every command passes before it goes into a slot ---- */

/*
 * ufs=3's second fence for a write (or SYNCHRONIZE CACHE, blocks 0): read only, a logical unit
 * never written, or any block of a range never written -- each refused with its name. Written
 * without sums that can wrap.
 */
static const char *
hc_protected(const struct hc_policy *p, uint8_t lun, uint64_t lba, uint64_t blocks)
{
	if (p->ro) {
		return "a write, and everything is read only (ufs_native_ro=1, or no write asked for)";
	}
	if (lun >= UFSHC_LUNS || (p->deny_luns & (1u << lun))) {
		return "a write to a logical unit that is never written (a boot unit, or not USERDATA's)";
	}
	for (uint32_t i = 0; i < p->ndeny; i++) {
		const struct hc_deny *d = &p->deny[i];

		if (d->lun != lun) {
			continue;
		}
		/* [lba, lba + blocks) and [first, first + blocks) meet; a write of no blocks still names its first */
		if (lba >= d->first ? lba - d->first < d->blocks : d->first - lba < (blocks == 0 ? 1 : blocks)) {
			return d->what;
		}
	}
	return NULL;
}

static const char *
hc_refusal(const uint8_t *upiu, uint32_t dw0, const struct hc_policy *p, bool any_command_type)
{
	const uint8_t *cdb = upiu + 16;
	uint32_t dd = dw0 & UTRD_DD_MASK;
	uint64_t expected = be32(upiu + 12);
	uint8_t type = upiu[0] & 0x3f, flags = upiu[1], lun = upiu[2];

	if (dd == UTRD_DD_MASK) {
		return "data direction 3";
	}
	if (p->ro && (dd == UTRD_DD_OUT || (type == UPIU_COMMAND && (flags & UPIU_FLAG_WRITE)))) {
		return "data to the device, and everything is read only (ufs_native_ro=1, or no write asked for)";
	}
	if (!any_command_type && (dw0 & UTRD_CT_MASK) != UTRD_CT_UFS) {
		return "not a UFS storage command type";
	}
	if (type == UPIU_NOP_OUT) {
		if (!p->control) {
			return "NOP OUT (own mode only)";
		}
		return dd == UTRD_DD_NONE && flags == 0 ? NULL : "NOP OUT with data or flags";
	}
	if (type == UPIU_QUERY) {
		if (!p->control) {
			return "a query (own mode only)";
		}
		if (dd != UTRD_DD_NONE || flags != 0 || upiu[10] != 0 || upiu[11] != 0) {
			return "a query with data";
		}
		if (upiu[5] == QUERY_READ && (upiu[12] == QOP_READ_DESC || upiu[12] == QOP_READ_ATTR ||
		    upiu[12] == QOP_READ_FLAG)) {
			return NULL;
		}
		if (upiu[5] == QUERY_WRITE && upiu[12] == QOP_SET_FLAG && upiu[13] == QFLAG_DEVICE_INIT && p->device_init) {
			return NULL;
		}
		return "a query that writes (only fDeviceInit, only while enabling QEMU's controller)";
	}
	if (type != UPIU_COMMAND) {
		return "not a SCSI command, query or NOP (task management, ...)";
	}
	if (upiu[4] & 0x0f) {
		return "not the SCSI command set";
	}
	if ((flags & UPIU_FLAG_READ) && (flags & UPIU_FLAG_WRITE)) {
		return "both the R and the W flag";
	}
	/* the device's well-known unit: START STOP UNIT to ACTIVE, nothing else; no other well-known unit */
	if (lun == LUN_DEVICE && p->control && cdb[0] == 0x1b) {
		return dd == UTRD_DD_NONE && flags == 0 && cdb[1] == 0 && cdb[2] == 0 && cdb[3] == 0 && cdb[4] == 0x10 &&
		       cdb[5] == 0 ? NULL : "START STOP UNIT other than to ACTIVE";
	}
	if (lun & LUN_WELL_KNOWN) {
		return "a well-known logical unit";
	}
	if (lun >= UFSHC_LUNS) {
		return "not a general logical unit";
	}
	switch (cdb[0]) {
	case 0x28:      /* READ(10) */
	case 0x88: {    /* READ(16) */
		uint64_t blocks = cdb[0] == 0x28 ? ((uint64_t)cdb[7] << 8 | cdb[8]) : be32(cdb + 10);

		if ((p->read_luns & (1u << lun)) == 0) {
			return "a logical unit reads do not go to now";
		}
		if (dd != UTRD_DD_IN || (flags & UPIU_FLAG_READ) == 0 || (flags & UPIU_FLAG_WRITE)) {
			return "a read without data from the device";
		}
		if (blocks == 0 || expected != blocks * BLOCK) {
			return "a read whose length is not what the UPIU expects";
		}
		return NULL;
	}
	case 0x2a:      /* WRITE(10) */
	case 0x8a: {    /* WRITE(16) */
		uint64_t lba = cdb[0] == 0x2a ? be32(cdb + 2) : be64(cdb + 2);
		uint64_t blocks = cdb[0] == 0x2a ? ((uint64_t)cdb[7] << 8 | cdb[8]) : be32(cdb + 10);

		if (!p->window) {
			return "a write, and no window is open";
		}
		if (lun != p->w_lun) {
			return "a write to a logical unit other than the window's";
		}
		if (dd != UTRD_DD_OUT || (flags & UPIU_FLAG_WRITE) == 0 || (flags & UPIU_FLAG_READ)) {
			return "a write without data to the device";
		}
		if ((cdb[1] & ~0x08) != 0) {
			return "a write with options other than FUA";
		}
		if (blocks == 0 || expected != blocks * BLOCK) {
			return "a write whose length is not what the UPIU expects";
		}
		if (lba < p->w_first || lba - p->w_first >= p->w_blocks || blocks > p->w_blocks - (lba - p->w_first)) {
			return "a write outside the window";
		}
		return hc_protected(p, lun, lba, blocks);
	}
	case 0x35:      /* SYNCHRONIZE CACHE(10) */
		if (!p->window || lun != p->w_lun) {
			return "SYNCHRONIZE CACHE outside the window's unit";
		}
		if (p->ro || (p->deny_luns & (1u << lun))) {
			return "SYNCHRONIZE CACHE, and nothing is written (read only, or a unit never written)";
		}
		return dd == UTRD_DD_NONE && flags == 0 && expected == 0 ? NULL : "SYNCHRONIZE CACHE with data";
	case 0x00:      /* TEST UNIT READY */
		if (!p->control && (p->read_luns & (1u << lun)) == 0) {
			return "a logical unit reads do not go to now";
		}
		return dd == UTRD_DD_NONE && expected == 0 && flags == 0 ? NULL : "TEST UNIT READY with data";
	case 0x03:      /* REQUEST SENSE */
	case 0x12:      /* INQUIRY */
	case 0x25:      /* READ CAPACITY(10) */
	case 0x9e:      /* SERVICE ACTION IN(16): READ CAPACITY(16) only */
		if (!p->control && (p->read_luns & (1u << lun)) == 0) {
			return "a logical unit reads do not go to now";
		}
		if (dd == UTRD_DD_OUT || (flags & UPIU_FLAG_WRITE)) {
			return "data to the device";
		}
		return cdb[0] != 0x9e || (cdb[1] & 0x1f) == 0x10 ? NULL : "a SERVICE ACTION IN other than READ CAPACITY(16)";
	default:
		return "a SCSI operation not on the list";
	}
}

static bool
ufshc_permitted(const uint8_t *upiu, uint32_t dw0)
{
	const char *why = hc_refusal(upiu, dw0, &hc_pol, false);

	if (why == NULL) {
		return true;
	}
	hc_refused++;
	hakios_ufs_refused(why);
	hc_say("ufs: native: REFUSED (%s): UPIU %02x %02x %02x %02x, CDB %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, "
	    "descriptor %08x; it goes into no slot\n", why, upiu[0], upiu[1], upiu[2], upiu[3], upiu[16], upiu[17], upiu[18],
	    upiu[19], upiu[20], upiu[21], upiu[22], upiu[23], upiu[24], upiu[25], dw0);
	return false;
}

static void
put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* READ or WRITE of blocks from lba: (10) while it fits, (16) beyond */
static void
hc_make_rw(struct hc_cmd *c, uint8_t lun, uint8_t tag, uint64_t lba, uint32_t blocks, bool write)
{
	uint8_t *cdb = c->upiu + 16;

	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[1] = write ? UPIU_FLAG_WRITE : UPIU_FLAG_READ;  /* simple task attribute */
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	put_be32(c->upiu + 12, blocks * BLOCK);
	if (lba + blocks <= 0x100000000ULL && blocks <= 0xffff) {
		cdb[0] = write ? 0x2a : 0x28;
		put_be32(cdb + 2, (uint32_t)lba);
		cdb[7] = (uint8_t)(blocks >> 8);
		cdb[8] = (uint8_t)blocks;
	} else {
		cdb[0] = write ? 0x8a : 0x88;
		put_be32(cdb + 2, (uint32_t)(lba >> 32));
		put_be32(cdb + 6, (uint32_t)lba);
		put_be32(cdb + 10, blocks);
	}
	c->dw0 = UTRD_CT_UFS | (write ? UTRD_DD_OUT : UTRD_DD_IN);      /* no interrupt, no inline encryption */
}

static void
hc_make_scsi(struct hc_cmd *c, uint8_t lun, uint8_t tag, const uint8_t *cdb, uint32_t cdb_len, uint32_t data_in)
{
	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_COMMAND;
	c->upiu[1] = data_in ? UPIU_FLAG_READ : 0;
	c->upiu[2] = lun;
	c->upiu[3] = tag;
	put_be32(c->upiu + 12, data_in);
	memcpy(c->upiu + 16, cdb, cdb_len);
	c->dw0 = UTRD_CT_UFS | (data_in ? UTRD_DD_IN : UTRD_DD_NONE);
}

static void
hc_make_tur(struct hc_cmd *c, uint8_t lun, uint8_t tag)
{
	static const uint8_t tur[6] = { 0x00 };

	hc_make_scsi(c, lun, tag, tur, sizeof(tur), 0);
}

static void
hc_make_query(struct hc_cmd *c, uint8_t tag, uint8_t function, uint8_t opcode, uint8_t idn, uint8_t index, uint16_t length)
{
	bzero(c, sizeof(*c));
	c->upiu[0] = UPIU_QUERY;
	c->upiu[3] = tag;
	c->upiu[5] = function;
	c->upiu[12] = opcode;
	c->upiu[13] = idn;
	c->upiu[14] = index;
	c->upiu[18] = (uint8_t)(length >> 8);
	c->upiu[19] = (uint8_t)length;
	c->dw0 = UTRD_CT_UFS | UTRD_DD_NONE;
}

/*
 * The whitelist tried on commands made up here and never sent: those that must be refused,
 * and those that must pass, under shared mode's policy (LUN 0 read, nothing else) and own
 * mode's (control, LUNs 0 and 1 read, a window of LUN 0's blocks 1000..1099). Pure bytes.
 */
static bool
hc_whitelist_selftest(void)
{
	static const struct {
		uint8_t pol;                    /* 0 shared, 1 own, 2 own enabling from reset */
		uint8_t type, flags, lun, func; /* UPIU bytes 0, 1, 2, 5 */
		uint8_t cdb[16];                /* (a query's opcode, idn, index for one) */
		uint32_t expected;
		uint32_t dd;
		bool pass;
		const char *what;
	} cases[] = {
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, true, "READ(10) of LUN 0" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x88, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, true, "READ(16) of LUN 0" },
		{ 0, UPIU_COMMAND, 0, 0, 0, { 0x00 }, 0, UTRD_DD_NONE, true, "TEST UNIT READY of LUN 0" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x25 }, 8, UTRD_DD_IN, true, "READ CAPACITY(10)" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10)" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x8a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(16)" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x2a, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "WRITE(10) dressed as a read" },
		{ 0, UPIU_COMMAND, 0, 0, 0, { 0x2a }, 0, UTRD_DD_NONE, false, "WRITE(10) with no data" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x42 }, 24, UTRD_DD_OUT, false, "UNMAP" },
		{ 0, UPIU_COMMAND, 0, 0, 0, { 0x42 }, 0, UTRD_DD_NONE, false, "UNMAP with no data" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "READ(10) with data host to device" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_MASK, false, "READ(10) with direction 3" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ | UPIU_FLAG_WRITE, 0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) with the W flag" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) with only the W flag" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 8192, UTRD_DD_IN, false, "READ(10) of a length other than expected" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0xc4, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) of the RPMB unit" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0xb0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) of the BOOT unit" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0xd0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) of the device unit" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0x81, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) of REPORT LUNS" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 3, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) of LUN 3 (not allowed now)" },
		{ 0, UPIU_COMMAND, 0, 0, 0, { 0x04 }, 0, UTRD_DD_NONE, false, "FORMAT UNIT" },
		{ 0, UPIU_COMMAND, 0, 0, 0, { 0x1b }, 0, UTRD_DD_NONE, false, "START STOP UNIT" },
		{ 0, UPIU_COMMAND, 0, 0, 0, { 0x35 }, 0, UTRD_DD_NONE, false, "SYNCHRONIZE CACHE(10)" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x3b }, 4096, UTRD_DD_OUT, false, "WRITE BUFFER" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0xb5 }, 512, UTRD_DD_OUT, false, "SECURITY PROTOCOL OUT" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0xa2 }, 512, UTRD_DD_IN, false, "SECURITY PROTOCOL IN" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x15 }, 24, UTRD_DD_OUT, false, "MODE SELECT" },
		{ 0, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x9e, 0x11 }, 32, UTRD_DD_IN, false, "SERVICE ACTION IN 0x11" },
		{ 0, UPIU_COMMAND, 0, 0, 0, { 0x00 }, 0, UTRD_DD_IN, false, "TEST UNIT READY with data in" },
		{ 0, UPIU_QUERY, 0, 0, QUERY_READ, { QOP_READ_ATTR, QATTR_POWER_MODE }, 0, UTRD_DD_NONE, false, "a QUERY REQUEST (shared)" },
		{ 0, 0x04, 0, 0, 0, { 0 }, 0, UTRD_DD_NONE, false, "TASK MANAGEMENT" },
		{ 0, UPIU_NOP_OUT, 0, 0, 0, { 0 }, 0, UTRD_DD_NONE, false, "NOP OUT (shared)" },
		/* own mode */
		{ 1, UPIU_NOP_OUT, 0, 0, 0, { 0 }, 0, UTRD_DD_NONE, true, "NOP OUT" },
		{ 1, UPIU_QUERY, 0, 0, QUERY_READ, { QOP_READ_ATTR, QATTR_POWER_MODE }, 0, UTRD_DD_NONE, true, "QUERY READ ATTRIBUTE" },
		{ 1, UPIU_QUERY, 0, 0, QUERY_READ, { QOP_READ_DESC, QDESC_UNIT, 2 }, 0, UTRD_DD_NONE, true, "QUERY READ DESCRIPTOR" },
		{ 1, UPIU_QUERY, 0, 0, QUERY_READ, { QOP_READ_FLAG, QFLAG_DEVICE_INIT }, 0, UTRD_DD_NONE, true, "QUERY READ FLAG" },
		{ 1, UPIU_QUERY, 0, 0, QUERY_WRITE, { QOP_SET_FLAG, QFLAG_DEVICE_INIT }, 0, UTRD_DD_NONE, false, "SET FLAG fDeviceInit (not enabling)" },
		{ 2, UPIU_QUERY, 0, 0, QUERY_WRITE, { QOP_SET_FLAG, QFLAG_DEVICE_INIT }, 0, UTRD_DD_NONE, true, "SET FLAG fDeviceInit (enabling)" },
		{ 2, UPIU_QUERY, 0, 0, QUERY_WRITE, { QOP_SET_FLAG, 0x02 }, 0, UTRD_DD_NONE, false, "SET FLAG fPermanentWPEn" },
		{ 1, UPIU_QUERY, 0, 0, QUERY_WRITE, { 0x04, 0x01 }, 0, UTRD_DD_NONE, false, "WRITE ATTRIBUTE" },
		{ 1, UPIU_QUERY, 0, 0, QUERY_WRITE, { 0x02, 0x01 }, 0, UTRD_DD_NONE, false, "WRITE DESCRIPTOR" },
		{ 1, UPIU_QUERY, 0, 0, QUERY_WRITE, { 0x07, 0x04 }, 0, UTRD_DD_NONE, false, "CLEAR FLAG" },
		{ 1, UPIU_QUERY, 0, 0, QUERY_READ, { 0x08, 0x04 }, 0, UTRD_DD_NONE, false, "TOGGLE FLAG as a read" },
		{ 1, UPIU_COMMAND, 0, 0xd0, 0, { 0x1b, 0, 0, 0, 0x10 }, 0, UTRD_DD_NONE, true, "START STOP UNIT to ACTIVE, device unit" },
		{ 1, UPIU_COMMAND, 0, 0xd0, 0, { 0x1b, 0, 0, 0, 0x30 }, 0, UTRD_DD_NONE, false, "START STOP UNIT to SLEEP" },
		{ 1, UPIU_COMMAND, 0, 0xd0, 0, { 0x1b, 0, 0, 0, 0x40 }, 0, UTRD_DD_NONE, false, "START STOP UNIT to POWERDOWN" },
		{ 1, UPIU_COMMAND, 0, 0, 0, { 0x1b, 0, 0, 0, 0x10 }, 0, UTRD_DD_NONE, false, "START STOP UNIT to LUN 0" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x03, 0xe8, 0, 0, 1 }, 4096, UTRD_DD_OUT, true, "WRITE(10) of block 1000" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0x08, 0, 0, 0x04, 0x4b, 0, 0, 1 }, 4096, UTRD_DD_OUT, true, "WRITE(10) FUA of block 1099" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x8a, 0, 0, 0, 0, 0, 0, 0, 0x04, 0x1a, 0, 0, 0, 8 }, 32768, UTRD_DD_OUT, true, "WRITE(16) of blocks 1050..1057" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x03, 0xe7, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10) of block 999 (before)" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x04, 0x4b, 0, 0, 2 }, 8192, UTRD_DD_OUT, false, "WRITE(10) of blocks 1099..1100 (across the end)" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x8a, 0, 0, 0, 0, 1, 0, 0, 0x03, 0xe8, 0, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(16) of block 2^32 + 1000" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 1, 0, { 0x2a, 0, 0, 0, 0x03, 0xe8, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10) of LUN 1" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x03, 0xe8, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "WRITE(10) with data from the device" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x2a, 0, 0, 0, 0x03, 0xe8, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10) with the R flag" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x03, 0xe8, 0, 0, 2 }, 4096, UTRD_DD_OUT, false, "WRITE(10) of a length other than expected" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0x10, 0, 0, 0x03, 0xe8, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10) with DPO" },
		{ 1, UPIU_COMMAND, 0, 0, 0, { 0x35 }, 0, UTRD_DD_NONE, true, "SYNCHRONIZE CACHE(10) of the window's unit" },
		{ 1, UPIU_COMMAND, 0, 1, 0, { 0x35 }, 0, UTRD_DD_NONE, false, "SYNCHRONIZE CACHE(10) of LUN 1" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x42 }, 24, UTRD_DD_OUT, false, "UNMAP (own)" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x3b }, 4096, UTRD_DD_OUT, false, "WRITE BUFFER (own)" },
		{ 1, UPIU_COMMAND, 0, 5, 0, { 0x00 }, 0, UTRD_DD_NONE, true, "TEST UNIT READY of LUN 5 (own)" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_READ, 5, 0, { 0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32 }, 32, UTRD_DD_IN, true, "READ CAPACITY(16) of LUN 5" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_READ, 1, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, true, "READ(10) of LUN 1 (own)" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_READ, 2, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) of LUN 2 (own, not allowed)" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_READ, 0xc4, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, false, "READ(10) of the RPMB unit (own)" },
		{ 1, UPIU_COMMAND, UPIU_FLAG_WRITE, 0xc4, 0, { 0xb5 }, 512, UTRD_DD_OUT, false, "SECURITY PROTOCOL OUT to RPMB (own)" },
		{ 1, 0x04, 0, 0, 0, { 0 }, 0, UTRD_DD_NONE, false, "TASK MANAGEMENT (own)" },
		/* ufs=3: the same window, blocks 1050..1059 of it protected, LUN 1 never written */
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x03, 0xe8, 0, 0, 1 }, 4096, UTRD_DD_OUT, true, "WRITE(10) of block 1000 (ufs=3)" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x04, 0x19, 0, 0, 1 }, 4096, UTRD_DD_OUT, true, "WRITE(10) of block 1049, beside the protected range" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x04, 0x1a, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10) of block 1050, protected" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x04, 0x23, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10) of block 1059, protected" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x04, 0x24, 0, 0, 1 }, 4096, UTRD_DD_OUT, true, "WRITE(10) of block 1060, beside the protected range" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x8a, 0, 0, 0, 0, 0, 0, 0, 0x04, 0x15, 0, 0, 0, 8 }, 32768, UTRD_DD_OUT, false, "WRITE(16) of blocks 1045..1052, into the protected range" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x04, 0x10, 0, 0, 40 }, 163840, UTRD_DD_OUT, false, "WRITE(10) of blocks 1040..1079, over the protected range" },
		{ 3, UPIU_COMMAND, 0, 0, 0, { 0x35 }, 0, UTRD_DD_NONE, true, "SYNCHRONIZE CACHE(10) of the window's unit (ufs=3)" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x42 }, 24, UTRD_DD_OUT, false, "UNMAP (ufs=3)" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x04 }, 0, UTRD_DD_OUT, false, "FORMAT UNIT (ufs=3)" },
		{ 3, UPIU_QUERY, 0, 0, QUERY_WRITE, { 0x02, 0x01 }, 0, UTRD_DD_NONE, false, "WRITE DESCRIPTOR (ufs=3)" },
		{ 3, UPIU_QUERY, 0, 0, QUERY_WRITE, { 0x04, 0x01 }, 0, UTRD_DD_NONE, false, "WRITE ATTRIBUTE (ufs=3)" },
		{ 3, UPIU_QUERY, 0, 0, QUERY_WRITE, { QOP_SET_FLAG, QFLAG_DEVICE_INIT }, 0, UTRD_DD_NONE, false, "SET FLAG fDeviceInit (ufs=3)" },
		{ 3, UPIU_QUERY, 0, 0, QUERY_WRITE, { 0x07, 0x04 }, 0, UTRD_DD_NONE, false, "CLEAR FLAG (ufs=3)" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x3b }, 4096, UTRD_DD_OUT, false, "WRITE BUFFER (ufs=3)" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_WRITE, 0xc4, 0, { 0xb5 }, 512, UTRD_DD_OUT, false, "SECURITY PROTOCOL OUT to RPMB (ufs=3)" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0xa2 }, 512, UTRD_DD_IN, false, "SECURITY PROTOCOL IN (ufs=3)" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0xc0 }, 512, UTRD_DD_IN, false, "a vendor command (0xC0, ufs=3)" },
		{ 3, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x28, 0, 0, 0, 0x04, 0x1a, 0, 0, 1 }, 4096, UTRD_DD_IN, true, "READ(10) of block 1050 (reads are not fenced)" },
		/* read only: the same window, and nothing written */
		{ 4, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 0, { 0x2a, 0, 0, 0, 0x03, 0xe8, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10) of block 1000 (read only)" },
		{ 4, UPIU_COMMAND, 0, 0, 0, { 0x35 }, 0, UTRD_DD_NONE, false, "SYNCHRONIZE CACHE(10) (read only)" },
		{ 4, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, { 0x28, 0, 0, 0, 0, 0, 0, 0, 1 }, 4096, UTRD_DD_IN, true, "READ(10) of LUN 0 (read only)" },
		{ 4, UPIU_QUERY, 0, 0, QUERY_READ, { QOP_READ_DESC, QDESC_UNIT, 0 }, 0, UTRD_DD_NONE, true, "QUERY READ DESCRIPTOR (read only)" },
		/* a window on a unit that is never written */
		{ 5, UPIU_COMMAND, UPIU_FLAG_WRITE, 1, 0, { 0x2a, 0, 0, 0, 0x03, 0xe8, 0, 0, 1 }, 4096, UTRD_DD_OUT, false, "WRITE(10) of LUN 1, a unit never written" },
		{ 5, UPIU_COMMAND, 0, 1, 0, { 0x35 }, 0, UTRD_DD_NONE, false, "SYNCHRONIZE CACHE(10) of LUN 1, a unit never written" },
	};
	static const struct hc_deny test_deny[1] = { { .lun = 0, .first = 1050, .blocks = 10, .what = "protected: TEST" } };
	struct hc_policy pol[6] = {
		{ .read_luns = 1 },
		{ .read_luns = 3, .control = true, .window = true, .w_lun = 0, .w_first = 1000, .w_blocks = 100 },
		{ .read_luns = 3, .control = true, .device_init = true },
		{ .read_luns = 3, .control = true, .window = true, .w_lun = 0, .w_first = 1000, .w_blocks = 100,
		  .deny_luns = 2, .deny = test_deny, .ndeny = 1 },
		{ .read_luns = 3, .control = true, .window = true, .w_lun = 0, .w_first = 1000, .w_blocks = 100, .ro = true },
		{ .read_luns = 3, .control = true, .window = true, .w_lun = 1, .w_first = 1000, .w_blocks = 100, .deny_luns = 2 },
	};
	bool right = true;
	unsigned passed_n = 0, refused_n = 0;

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t upiu[32] = { 0 };
		bool passed;

		upiu[0] = cases[i].type;
		upiu[1] = cases[i].flags;
		upiu[2] = cases[i].lun;
		upiu[5] = cases[i].func;
		if (cases[i].type == UPIU_QUERY) {
			upiu[12] = cases[i].cdb[0];
			upiu[13] = cases[i].cdb[1];
			upiu[14] = cases[i].cdb[2];
		} else {
			put_be32(upiu + 12, cases[i].expected);
			memcpy(upiu + 16, cases[i].cdb, 16);
		}
		passed = hc_refusal(upiu, UTRD_CT_UFS | cases[i].dd, &pol[cases[i].pol], false) == NULL;
		if (passed != cases[i].pass) {
			hc_say("ufs: native: the whitelist %s %s\n", passed ? "LETS THROUGH" : "refuses", cases[i].what);
			right = false;
		}
		passed ? passed_n++ : refused_n++;
	}
	/* and what this file makes */
	{
		struct hc_cmd c;

		hc_make_rw(&c, 0, 1, 12345, 64, false);
		right &= hc_refusal(c.upiu, c.dw0, &pol[0], false) == NULL;
		hc_make_rw(&c, 2, 1, 12345, 64, false);
		right &= hc_refusal(c.upiu, c.dw0, &pol[0], false) != NULL;
		hc_make_rw(&c, 0, 1, 1000, 100, true);
		right &= hc_refusal(c.upiu, c.dw0, &pol[1], false) == NULL;
		hc_make_rw(&c, 0, 1, 1001, 100, true);
		right &= hc_refusal(c.upiu, c.dw0, &pol[1], false) != NULL;
		hc_make_rw(&c, 0, 1, 1000, 1, true);
		right &= hc_refusal(c.upiu, c.dw0, &pol[0], false) != NULL;
		hc_make_tur(&c, 0, 0);
		right &= hc_refusal(c.upiu, c.dw0, &pol[0], false) == NULL;
		hc_make_query(&c, 0, QUERY_READ, QOP_READ_DESC, QDESC_UNIT, 0, 0x23);
		right &= hc_refusal(c.upiu, c.dw0, &pol[1], false) == NULL && hc_refusal(c.upiu, c.dw0, &pol[0], false) != NULL;
	}
	hc_say("ufs: native: whitelist tried on %u made-up commands, never sent (%u pass, %u refused; shared: reads of LUN 0; "
	    "own: + NOP, query reads, wake, writes in the window), and on 7 of this file's: %s\n",
	    (unsigned)(sizeof(cases) / sizeof(cases[0])), passed_n, refused_n, right ? "as it should" : "WRONG");
	return right;
}

/* ---- one command in one slot ---- */

static void
hc_put(volatile uint32_t *to, const void *from, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes / 4; i++) {
		uint32_t w;

		memcpy(&w, (const uint8_t *)from + 4 * i, 4);
		to[i] = w;
	}
}

/*
 * Into the slot: the command UPIU, an empty response, the PRD table for bytes of the slot's
 * buffer, the descriptor (offsets and lengths as the SoC counts them) -- only a command the
 * whitelist passes, and only if what the slot then holds is the same and passes again.
 */
static int
hc_place(struct hc_slot *s, const struct hc_cmd *c, uint32_t bytes)
{
	const struct ufshc_soc *soc = hc_soc;
	uint32_t n = 0, back[8], entries = (bytes + soc->prd_max - 1) / soc->prd_max;

	if (!ufshc_permitted(c->upiu, c->dw0)) {
		return EPERM;
	}
	if (bytes > s->data_size || soc->prdt_offset + entries * soc->prd_entry > hc_ucd_size) {
		hc_say("ufs: native: slot %u: %u bytes do not fit its buffer or its PRD table\n", s->number, bytes);
		return EINVAL;
	}
	hc_put(s->ucd, c->upiu, sizeof(c->upiu));
	for (uint32_t i = sizeof(c->upiu) / 4; i < soc->prdt_offset / 4; i++) {
		s->ucd[i] = 0;
	}
	for (uint32_t at = 0; at < bytes; at += soc->prd_max, n++) {
		volatile uint32_t *prd = s->ucd + (soc->prdt_offset + soc->prd_entry * n) / 4;
		uint64_t pa = s->data_pa + at;
		uint32_t len = bytes - at < soc->prd_max ? bytes - at : soc->prd_max;

		prd[0] = (uint32_t)pa;
		prd[1] = (uint32_t)(pa >> 32);
		prd[2] = 0;
		prd[3] = len - 1;                       /* (Exynos: bits 26-31, key length and encryption, 0: none) */
		for (uint32_t w = 4; w < soc->prd_entry / 4; w++) {
			prd[w] = 0;                     /* (Exynos: the encryption's IVs and keys, unused) */
		}
	}
	s->utrd[0] = c->dw0;
	s->utrd[1] = 0;
	s->utrd[2] = OCS_INVALID;
	s->utrd[3] = 0;
	s->utrd[4] = (uint32_t)s->ucd_pa;
	s->utrd[5] = (uint32_t)(s->ucd_pa >> 32);
	if (soc->prdt_bytes) {
		s->utrd[6] = (soc->rsp_offset << 16) | soc->rsp_length;
		s->utrd[7] = (soc->prdt_offset << 16) | (n * soc->prd_entry);
	} else {
		s->utrd[6] = ((soc->rsp_offset / 4) << 16) | (soc->rsp_length / 4);     /* response: offset, length in dwords */
		s->utrd[7] = ((soc->prdt_offset / 4) << 16) | n;                         /* PRD table: offset in dwords, entries */
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int i = 0; i < 8; i++) {
		back[i] = s->ucd[i];
	}
	if (memcmp(back, c->upiu, sizeof(back)) != 0 || s->utrd[0] != c->dw0 ||
	    !ufshc_permitted((const uint8_t *)back, s->utrd[0])) {
		hc_say("ufs: native: slot %u does not hold what was put in it: not rung\n", s->number);
		hc_refused++;
		return EPERM;
	}
	hc_dma_sync(s->utrd, 32);
	hc_dma_sync(s->ucd, soc->prdt_offset + n * soc->prd_entry);
	return 0;
}

/*
 * Own mode's completion notification (UTRLCNR, 0x64): UFSHCI 3.0's, and the Exynos 9810's 2.1
 * controller has it too. A slot's bit is set when its request is done; a one written clears it.
 * The slot's bit is cleared before its doorbell is rung (UEFI leaves bits set: 00000014), so a set
 * bit after it is this request's: one of hc_done_sign's three signs. A UTRLCNR bit that does not
 * clear when written turns that sign off (a stale bit must never be taken for a completion).
 */
static bool hc_cnr_broken;                      /* a written one did not clear a UTRLCNR bit */

static bool
hc_cnr_on(void)
{
	return hc_mode == HC_OWN && !hc_cnr_broken && (hc_ver >= 0x300 || hc_soc->cnr);
}

/*
 * The doorbell: only while this kernel's list is the one in, running, and (shared mode) the
 * loader not taking it back, or (own mode) no fatal status. Before it, the SoC's nexus hook.
 */
static int
hc_ring(struct hc_slot *s, bool scsi)
{
	uint32_t bit = 1u << s->number;
	boolean_t istate;
	bool ok;

	lck_mtx_lock(&hc_lock);
	istate = ml_set_interrupts_enabled(FALSE);
	ok = hc_in && !hc_closed && !hc_dead &&
	    hc_read(UFSHC_UTRLBA) == hc_list_lo && hc_read(UFSHC_UTRLBAU) == hc_list_hi &&
	    (hc_read(UFSHC_UTRLRSR) & 1) != 0 && (hc_read(UFSHC_UTRLDBR) & bit) == 0;
	if (hc_mode == HC_SHARED) {
		ok = ok && *hc_loader.take_back == 0;
	} else if (ok && (hc_read(UFSHC_IS) & IS_FATAL)) {
		ok = false;
	}
	if (ok) {
		if (hc_mode == HC_OWN) {
			hc_write(UFSHC_IS, IS_UTRCS);   /* completions acknowledged; error bits stay as evidence (ShengUfs) */
		}
		if (hc_cnr_on()) {
			s->cnr_rung[0] = hc_read(UFSHC_UTRLCNR);
			hc_write(UFSHC_UTRLCNR, bit);   /* no stale notification: a set bit after the doorbell is this request's */
			s->cnr_rung[1] = hc_read(UFSHC_UTRLCNR);
			if (s->cnr_rung[1] & bit) {
				hc_cnr_broken = true;
				hc_say("ufs: native: slot %u: its UTRLCNR bit did not clear when written (UTRLCNR %08x): "
				    "completions from the doorbell alone from now on\n", s->number, hc_read(UFSHC_UTRLCNR));
			}
		}
		if (hc_soc->nexus != NULL) {
			hc_soc->nexus(s->number, scsi);
		}
		s->rung = mach_absolute_time();
		hc_write(UFSHC_UTRLDBR, bit);
	}
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	return ok ? 0 : ENXIO;
}

/*
 * ufs_native_hang=N (a test of the loader's side): once N reads are done, the thread that has just
 * rung a doorbell stops the kernel -- every processor spinning with interrupts off, this one
 * included -- with reads in flight and this kernel's list in. The heartbeat stops, and the
 * loader (or the watchdog) must take it from there.
 */
extern unsigned int cpu_broadcast_xcall(uint32_t *synch, boolean_t self_xcall, void (*func)(void *), void *parm);
static uint32_t hc_hang_after;
static volatile uint32_t hc_hanging;

static void
hc_spin_forever(__unused void *parm)
{
	(void)ml_set_interrupts_enabled(FALSE);
	for (;;) {
		__builtin_arm_yield();
	}
}

static void
hc_hang(uint32_t slot)
{
	static int parm;

	/* (into the loader's log directly as well: a hung kernel's own log never comes back to it) */
	hc_say_late("ufs: native: ufs_native_hang=%u: the kernel stops HERE, every processor spinning with interrupts "
	    "off; slot %u just rung, doorbell %08x, %u reads in flight, this kernel's list in\n", hc_hang_after, slot,
	    hc_read(UFSHC_UTRLDBR), hc_in_flight);
	cpu_broadcast_xcall(NULL, TRUE, hc_spin_forever, &parm);
}

/* Nothing more is sent from here on; whoever waits for a slot is told. */
static void
hc_off(void)
{
	hc_dead = true;
	lck_mtx_lock(&hc_lock);
	wakeup(&hc_free);
	/* those who sleep on a slot's answer look at hc_dead now, not after their own 1 s tick */
	for (uint32_t i = 0; i < SLOTS; i++) {
		wakeup(&hc_slots[i]);
	}
	lck_mtx_unlock(&hc_lock);
}

/*
 * Waiting for the controller. Its interrupt stays off (UEFI's setting; own mode masks it), so a
 * waiter looks for itself. A request is done as soon as any of three signs is there:
 *   - its doorbell bit clear;
 *   - (own mode) its UTRLCNR bit set (hc_cnr_on: cleared before the doorbell, so a set bit is this request's);
 *   - (own mode) its descriptor written back (OCS not f) with a response UPIU of its task tag in its command
 *     descriptor (both made invalid by hc_place before the doorbell).
 * Seen on the phone (ufs-rw8, 4375 requests): the doorbell bit dropped by itself on about two in three; on the
 * rest it stayed, the descriptor written back (OCS 0), the response there (21, status 00) and IS's UTRCS set,
 * with UTRLCNR 0 as long as the poller looked (100 ms) -- 00000004 only read after UTRLCLR. So the descriptor
 * is the sign that holds on this controller, and the clean-up after it (hc_finish) is: IS acknowledged
 * (written with ones), the UTRLCNR bit written, and, if the doorbell bit is still set, that slot cleared with
 * UTRLCLR (clear_with_ones: on the Exynos a one clears, as the safety net did in rw7 and rw8; UFSHCI's own
 * sense is a zero) and the bit confirmed clear. A waiter spins (hc_spin_us, 2 ms: a 4 KB read is done in well under that) and then sleeps
 * in 1 ms steps; no poller thread, no 100 ms tick.
 */
static void hc_stuck(struct hc_slot *s);
static uint32_t hc_spin_us = 2000;              /* boot-arg ufs_native_spin */
static uint64_t hc_sleeps, hc_cleared, hc_finish_failed;
enum { HC_BY_DB, HC_BY_CNR, HC_BY_OCS, HC_BY_N };
static const char *const hc_by_name[HC_BY_N] = { "its doorbell bit clear", "its UTRLCNR bit", "its descriptor and response" };
static uint64_t hc_done_by[HC_BY_N];
static uint32_t hc_decisions_said;
#define HC_DECISIONS_SAID   8                   /* the first few completions said in detail */

/* Which sign says the slot's request is done (HC_BY_* + 1), or 0: not yet. Reads only. */
static int
hc_done_sign(struct hc_slot *s)
{
	uint32_t bit = 1u << s->number;
	volatile const uint8_t *r;
	uint8_t type;

	if ((hc_read(UFSHC_UTRLDBR) & bit) == 0) {
		return HC_BY_DB + 1;
	}
	if (hc_mode != HC_OWN) {
		return 0;
	}
	if (hc_cnr_on() && (hc_read(UFSHC_UTRLCNR) & bit)) {
		return HC_BY_CNR + 1;
	}
	hc_dma_sync(s->utrd, 32);
	if ((s->utrd[2] & 0xff) == OCS_INVALID) {
		return 0;
	}
	r = (volatile const uint8_t *)s->ucd + hc_soc->rsp_offset;
	hc_dma_sync(r, 32);
	type = r[0] & 0x3f;
	if (r[3] == s->number && (type == UPIU_RESPONSE || type == UPIU_NOP_IN || type == UPIU_QUERY_RSP)) {
		return HC_BY_OCS + 1;
	}
	return 0;
}

/* The descriptor written back and the response there (as hc_done_sign's third sign) */
static bool
hc_written_back(struct hc_slot *s)
{
	volatile const uint8_t *r = (volatile const uint8_t *)s->ucd + hc_soc->rsp_offset;
	uint8_t type;

	hc_dma_sync(s->utrd, 32);
	hc_dma_sync(r, 32);
	type = r[0] & 0x3f;
	return (s->utrd[2] & 0xff) != OCS_INVALID && r[3] == s->number &&
	       (type == UPIU_RESPONSE || type == UPIU_NOP_IN || type == UPIU_QUERY_RSP);
}

/*
 * The slot's request is done (by: the sign seen). Own mode's clean-up: IS acknowledged, the UTRLCNR bit
 * written, and a doorbell bit still set cleared with UTRLCLR -- only once the descriptor is written back and
 * the response there (a UTRLCNR bit ahead of them is waited for up to 2 ms), and only while this kernel's list
 * is in. true: the doorbell bit is clear. hc_lock held.
 */
static bool
hc_finish(struct hc_slot *s, int by)
{
	uint32_t bit = 1u << s->number, db0, cnr0, is0, cnr1 = 0, db1;
	uint64_t t;
	bool cleared = false;

	db0 = hc_read(UFSHC_UTRLDBR);
	cnr0 = hc_read(UFSHC_UTRLCNR);
	is0 = hc_read(UFSHC_IS);
	if (hc_mode == HC_OWN) {
		hc_write(UFSHC_IS, IS_UTRCS);
		if (hc_ver >= 0x300 || hc_soc->cnr) {
			hc_write(UFSHC_UTRLCNR, bit);
			cnr1 = hc_read(UFSHC_UTRLCNR);
		}
		if (hc_read(UFSHC_UTRLDBR) & bit) {
			for (t = mach_absolute_time(); !hc_written_back(s) && hc_us_since(t) < 2000;) {
				__builtin_arm_yield();
			}
			if (!hc_written_back(s) || hc_read(UFSHC_UTRLBA) != hc_list_lo || hc_read(UFSHC_UTRLBAU) != hc_list_hi) {
				hc_finish_failed++;
				if (hc_finish_failed <= 4) {
					hc_say("ufs: native: slot %u: done by %s, but its descriptor is not written back (OCS %x) or the "
					    "list is not this kernel's: its doorbell bit left set, waited for again\n", s->number,
					    hc_by_name[by], s->utrd[2] & 0xff);
				}
				return false;
			}
			if ((hc_read(UFSHC_UTRLDBR) & bit) != 0) {
				hc_write(UFSHC_UTRLCLR, hc_soc->clear_with_ones ? bit : ~bit);
				cleared = true;
				for (t = mach_absolute_time(); (hc_read(UFSHC_UTRLDBR) & bit) && hc_us_since(t) < 1000;) {
					__builtin_arm_yield();
				}
			}
		}
	}
	db1 = hc_read(UFSHC_UTRLDBR);
	if (hc_decisions_said < HC_DECISIONS_SAID) {
		volatile const uint8_t *r = (volatile const uint8_t *)s->ucd + hc_soc->rsp_offset;

		hc_decisions_said++;
		hc_say("ufs: native: slot %u: done by %s, %llu us after its doorbell (UTRLCNR at the doorbell %08x, then %08x; "
		    "now doorbell %08x, UTRLCNR %08x, IS %08x, OCS %x, response %02x tag %u status %02x); IS acknowledged, "
		    "UTRLCNR written (then %08x), %s: doorbell %08x%s\n", s->number, hc_by_name[by], hc_us_since(s->rung),
		    s->cnr_rung[0], s->cnr_rung[1], db0, cnr0, is0, s->utrd[2] & 0xff, r[0], r[3], r[7], cnr1,
		    cleared ? "its bit cleared with UTRLCLR" : "UTRLCLR not needed", db1, (db1 & bit) ? ": STILL SET" : "");
	}
	if (db1 & bit) {
		hc_finish_failed++;
		if (hc_finish_failed <= 4) {
			hc_say("ufs: native: slot %u: done (OCS %x) but its doorbell bit stays, and UTRLCLR did not clear it\n",
			    s->number, s->utrd[2] & 0xff);
		}
		return false;
	}
	if (cleared) {
		hc_cleared++;
	}
	hc_done_by[by]++;
	return true;
}

/*
 * Until the controller is done with the slot's command (hc_done_sign, then hc_finish), and (ShengUfs)
 * its descriptor's OCS written back -- a cleared bit alone is not taken for a completion.
 */
static int
hc_wait(struct hc_slot *s, const char *what, bool count)
{
	uint64_t started = mach_absolute_time(), us, polls = 1;
	uint32_t bit = 1u << s->number, ocs;
	bool slept = false;
	int by;

	for (;;) {
		while ((by = hc_done_sign(s)) == 0 && hc_us_since(started) < hc_spin_us) {
			polls++;
			__builtin_arm_yield();
		}
		if (by != 0) {
			bool ok;

			if (by == HC_BY_DB + 1 && hc_mode != HC_OWN) {
				break;          /* (shared mode: the doorbell alone, nothing to clean up) */
			}
			lck_mtx_lock(&hc_lock);
			ok = hc_finish(s, by - 1);
			lck_mtx_unlock(&hc_lock);
			if (ok) {
				break;
			}
		}
		/*
		 * The path went off while this one waited (another slot of the same read timed out):
		 * do not wait out a fresh 10 s per slot (31 slots would hold the read for 310 s).
		 * A slot whose doorbell bit is already clear did finish, so it is read as done.
		 */
		if (hc_dead) {
			if ((hc_read(UFSHC_UTRLDBR) & bit) == 0) {
				break;
			}
			hc_say("ufs: native: slot %u (%s) not waited for: this path is already off\n", s->number, what);
			return ETIMEDOUT;
		}
		us = hc_us_since(started);
		if (us > TIMEOUT_MS * 1000ULL) {
			hc_say("ufs: native: slot %u (%s) not done in %u s (doorbell %08x, IS %08x, HCS %08x): "
			    "this path is off from now on\n", s->number, what, TIMEOUT_MS / 1000, hc_read(UFSHC_UTRLDBR),
			    hc_read(UFSHC_IS), hc_read(UFSHC_HCS));
			hc_stuck(s);
			hc_off();
			return ETIMEDOUT;
		}
		if (!slept) {
			slept = true;
			os_atomic_inc(&hc_sleeps, relaxed);
		}
		IOSleep(1);
		polls++;
	}
	s->done_at = mach_absolute_time();
	__asm__ volatile ("dsb sy" ::: "memory");
	hc_dma_sync(s->utrd, 32);
	hc_dma_sync(s->ucd, hc_soc->rsp_offset + hc_soc->rsp_length);
	ocs = s->utrd[2] & 0xff;
	for (uint64_t t = mach_absolute_time(); ocs == OCS_INVALID && hc_us_since(t) < 2000;) {
		__builtin_arm_yield();
		hc_dma_sync(s->utrd, 32);
		ocs = s->utrd[2] & 0xff;
	}
	os_atomic_add(&hc_polls, polls, relaxed);
	if (count) {
		os_atomic_add(&hc_wait_us, hc_us_since(started), relaxed);
	}
	if (hc_mode == HC_OWN) {
		uint32_t is = hc_read(UFSHC_IS);

		if (is & IS_FATAL) {
			hc_say("ufs: own: slot %u (%s): fatal status, IS %08x, HCS %08x: this path is off from now on\n",
			    s->number, what, is, hc_read(UFSHC_HCS));
			hc_off();
			return EIO;
		}
	}
	if (ocs == OCS_INVALID) {
		hc_say("ufs: native: slot %u (%s): the doorbell bit cleared, the descriptor not written back (OCS f)\n",
		    s->number, what);
		return EIO;
	}
	return 0;
}

/*
 * Throughput and latency (hc_speed_add, from hc_rw): bytes read and written per second for the last
 * HC_WIN seconds, and the time from the doorbell to the request seen done, over the run.
 */
#define HC_WIN 32
static struct {
	uint64_t sec, rd, wr;
} hc_win[HC_WIN];
static uint64_t hc_lat_n, hc_lat_sum_us, hc_lat_max_us, hc_lat4k_n, hc_lat4k_sum_us, hc_lat4k_max_us;

static uint64_t
hc_now_s(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_SEC;
}

static void
hc_speed_add(struct hc_slot *s, bool write, uint64_t bytes)
{
	uint64_t ns, us, sec = hc_now_s();

	absolutetime_to_nanoseconds(s->done_at - s->rung, &ns);
	us = ns / 1000;
	lck_mtx_lock(&hc_lock);
	if (hc_win[sec % HC_WIN].sec != sec) {
		hc_win[sec % HC_WIN].sec = sec;
		hc_win[sec % HC_WIN].rd = 0;
		hc_win[sec % HC_WIN].wr = 0;
	}
	if (write) {
		hc_win[sec % HC_WIN].wr += bytes;
	} else {
		hc_win[sec % HC_WIN].rd += bytes;
	}
	hc_lat_n++;
	hc_lat_sum_us += us;
	if (us > hc_lat_max_us) {
		hc_lat_max_us = us;
	}
	if (!write && bytes == BLOCK) {
		hc_lat4k_n++;
		hc_lat4k_sum_us += us;
		if (us > hc_lat4k_max_us) {
			hc_lat4k_max_us = us;
		}
	}
	lck_mtx_unlock(&hc_lock);
}

/*
 * One line: MB/s read and written over the last 30 s, the requests' average and longest latency. No lock
 * (hakios_ufs_summary takes none: the USB console's poll, the end of the run); numbers a request apart at worst.
 */
static void
hc_speed_line(char *buf, size_t size)
{
	uint64_t now = hc_now_s(), rd = 0, wr = 0, n, sum, max, n4, sum4, max4, rkb, wkb;

	for (uint32_t i = 0; i < HC_WIN; i++) {
		if (hc_win[i].sec + 30 > now && hc_win[i].sec <= now) {
			rd += hc_win[i].rd;
			wr += hc_win[i].wr;
		}
	}
	n = hc_lat_n;
	sum = hc_lat_sum_us;
	max = hc_lat_max_us;
	n4 = hc_lat4k_n;
	sum4 = hc_lat4k_sum_us;
	max4 = hc_lat4k_max_us;
	rkb = rd / 30 / 1000;           /* (kB/s: MB/s with three decimals, said with one) */
	wkb = wr / 30 / 1000;
	snprintf(buf, size, "ufs: native: speed: the last 30 s: read %llu.%llu MB/s (%llu MB), written %llu.%llu MB/s "
	    "(%llu MB); doorbell to done: %llu requests, average %llu us, at most %llu us; 4 KB reads: %llu, average %llu "
	    "us, at most %llu us\n", rkb / 1000, rkb % 1000 / 100, rd >> 20, wkb / 1000, wkb % 1000 / 100, wr >> 20, n,
	    n ? sum / n : 0, max, n4, n4 ? sum4 / n4 : 0, max4);
}

/* A slot whose doorbell bit did not clear: everything that tells what the controller made of it */
static void
hc_stuck(struct hc_slot *s)
{
	volatile const uint8_t *r;

	if (hc_mode != HC_OWN) {
		return;
	}
	hc_dma_sync(s->utrd, 32);
	hc_dma_sync(s->ucd, hc_soc->rsp_offset + hc_soc->rsp_length);
	r = (volatile const uint8_t *)s->ucd + hc_soc->rsp_offset;
	hc_say("ufs: native: slot %u stuck: its descriptor %08x %08x %08x %08x %08x %08x %08x %08x (OCS %x%s); response "
	    "%02x %02x %02x %02x %02x %02x %02x %02x; UTRLCNR %08x, UTRLRSR %08x, task doorbell %08x, in flight %u\n",
	    s->number, s->utrd[0], s->utrd[1], s->utrd[2], s->utrd[3], s->utrd[4], s->utrd[5], s->utrd[6], s->utrd[7],
	    s->utrd[2] & 0xff, (s->utrd[2] & 0xff) == OCS_INVALID ? ": not written back" : ": WRITTEN BACK", r[0], r[1], r[2],
	    r[3], r[4], r[5], r[6], r[7], hc_read(UFSHC_UTRLCNR), hc_read(UFSHC_UTRLRSR), hc_read(UFSHC_UTMRLDBR), hc_in_flight);
	if (hc_vs != 0 && hc_soc->nexus == exynos_nexus) {
		hc_say("ufs: native: slot %u stuck: exynos: UTRL_NEXUS_TYPE %08x\n", s->number, vs_read(EXY_UTRL_NEXUS_TYPE));
	}
}

static int
hc_protocol(struct hc_slot *s, const char *what, const char *why)
{
	volatile const uint8_t *r = (volatile const uint8_t *)s->ucd + hc_soc->rsp_offset;

	hc_protocol_errors++;
	hc_say("ufs: native: slot %u (%s): the response is not right (%s): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
	    "%02x %02x\n", s->number, what, why, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
	return EIO;
}

/*
 * The response of the SCSI command in the slot (ShengUfs's checks): its type, task tag and
 * LUN, no extra header segment, a data segment that fits, the sense data in fixed or
 * descriptor format, no residual with GOOD status. 0; EAGAIN for CHECK CONDITION (the caller
 * may try again: a unit attention, a unit not ready yet); EIO.
 */
static int
hc_scsi_result(struct hc_slot *s, uint8_t lun, const char *what, bool quiet)
{
	volatile const uint8_t *r = (volatile const uint8_t *)s->ucd + hc_soc->rsp_offset;
	uint32_t ocs = s->utrd[2] & 0xff, seg = (uint32_t)r[10] << 8 | r[11], residual = be32(r + 12);
	uint32_t key = 0, asc = 0, ascq = 0;

	if (ocs != OCS_SUCCESS) {
		if (!quiet) {
			hc_say("ufs: native: slot %u (%s): OCS %x\n", s->number, what, ocs);
		}
		return EIO;
	}
	if ((r[0] & 0x3f) != UPIU_RESPONSE) {
		return hc_protocol(s, what, "transaction type");
	}
	if (r[3] != s->number) {
		return hc_protocol(s, what, "task tag");
	}
	if (r[2] != lun) {
		return hc_protocol(s, what, "logical unit");
	}
	if (r[8] != 0) {
		return hc_protocol(s, what, "extra header segment");
	}
	if (seg > hc_soc->rsp_length - 32) {
		/*
		 * QEMU's UFS model (hw/ufs/lu.c ufs_build_scsi_response_upiu, through 10.1 at least) swaps the data
		 * segment length to big-endian twice, so it arrives little-endian: 2 reads as 0x0200, 2 + 18 bytes
		 * of sense as 0x1400. Linux's ufshcd never reads this field (it takes the sense length from the
		 * segment itself), so nothing else notices; QEMU upstream fixed it in ed621cc8. Only on QEMU's
		 * controller (1b36:0013) is a length that only fits swapped taken swapped, and said once: on a phone it
		 * is a protocol error like any other.
		 */
		uint32_t swapped = (seg >> 8 | seg << 8) & 0xffff;
		static bool said_swapped;

		if (!hc_qemu_ufs || swapped > hc_soc->rsp_length - 32) {
			return hc_protocol(s, what, "data segment length");
		}
		if (!said_swapped) {
			said_swapped = true;
			hc_say("ufs: native: the response's data segment length %u only fits byte-swapped (%u): taken so "
			    "(QEMU's model writes it little-endian)\n", seg, swapped);
		}
		seg = swapped;
	}
	if (seg >= 2) {
		uint32_t sense_len = (uint32_t)r[32] << 8 | r[33];
		volatile const uint8_t *sense = r + 34;

		if (sense_len > seg - 2) {
			return hc_protocol(s, what, "sense length");
		}
		if (sense_len >= 14 && ((sense[0] & 0x7f) == 0x70 || (sense[0] & 0x7f) == 0x71)) {
			key = sense[2] & 0xf, asc = sense[12], ascq = sense[13];
		} else if (sense_len >= 4 && ((sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73)) {
			key = sense[1] & 0xf, asc = sense[2], ascq = sense[3];
		}
	}
	/*
	 * The status decides, as in Linux's ufshcd: a CHECK CONDITION comes with "target failure" in the
	 * response field from some devices (QEMU's), with "target success" from others.
	 */
	if (r[7] == 0x02) {
		if (!quiet) {
			hc_say("ufs: native: slot %u (%s): CHECK CONDITION, sense key %x asc %02x ascq %02x\n", s->number, what,
			    key, asc, ascq);
		}
		return EAGAIN;
	}
	if (r[6] != 0) {
		hc_say("ufs: native: slot %u (%s): response %02x (target failure), status %02x\n", s->number, what, r[6], r[7]);
		return EIO;
	}
	if (r[7] == 0 && ((r[1] & 0x60) || residual != 0)) {
		return hc_protocol(s, what, "GOOD with a residual");
	}
	if (r[7] != 0) {
		hc_say("ufs: native: slot %u (%s): status %02x, sense key %x asc %02x ascq %02x\n", s->number, what, r[7], key,
		    asc, ascq);
		return EIO;
	}
	return 0;
}

/* Until the controller is done with the slot's command; then whether it went well. */
static int
hc_complete(struct hc_slot *s, uint8_t lun, const char *what, bool tur)
{
	int e = hc_wait(s, what, !tur);

	if (e == 0) {
		e = hc_scsi_result(s, lun, what, tur);
		if (e == EAGAIN && tur) {
			e = 0;                          /* (the proof: TEST UNIT READY came back, whatever it says) */
		}
	}
	if (e != 0 && !tur) {
		os_atomic_inc(&hc_errors, relaxed);
	}
	return e;
}

static struct hc_slot *
hc_take(bool wait)
{
	struct hc_slot *s = NULL;
	uint64_t t0 = mach_absolute_time();

	lck_mtx_lock(&hc_lock);
	hc_lock_ticks += mach_absolute_time() - t0;
	if (hc_free == 0 && wait && !hc_dead && hc_in) {
		uint64_t w0 = mach_absolute_time();

		hc_slot_waits++;
		while (hc_free == 0 && wait && !hc_dead && hc_in) {
			msleep(&hc_free, &hc_lock, PRIBIO, "ufshc", NULL);
		}
		hc_slot_wait_ticks += mach_absolute_time() - w0;
	}
	if (hc_free != 0 && hc_in && !hc_dead) {
		uint32_t i = (uint32_t)__builtin_ctz(hc_free), f;

		hc_free &= ~(1u << i);
		s = &hc_slots[i];
		f = ++hc_in_flight;
		if (f > hc_most_in_flight) {
			hc_most_in_flight = f;
		}
		hc_flight_hist[f <= 1 ? 0 : f <= 2 ? 1 : f <= 4 ? 2 : f <= 8 ? 3 : f <= 16 ? 4 : 5]++;
	}
	lck_mtx_unlock(&hc_lock);
	return s;
}

static void
hc_give(struct hc_slot *s)
{
	lck_mtx_lock(&hc_lock);
	hc_free |= 1u << (s - hc_slots);
	hc_in_flight--;
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
}

/*
 * Bytes [offset, offset + len) of logical unit lun into dst (or, write, from src: whole blocks
 * only): the blocks that cover them, a slot's buffer at a time, with as many slots in flight at
 * once as are free (at least one), the wanted bytes copied out of each as it is done.
 */
static int
hc_rw(uint8_t lun, uint64_t offset, uint8_t *buf, uint64_t len, bool write)
{
	struct {
		struct hc_slot *slot;
		uint64_t lba;
		uint32_t blocks, skip, take;
		uint8_t *to;
	} q[SLOTS];
	uint32_t head = 0, tail = 0, per = hc_slot_buffer / BLOCK;
	int error = 0;

	if (!hc_usable || !hc_in || hc_dead || hc_closed) {
		return ENXIO;
	}
	if (write && (offset % BLOCK || len % BLOCK)) {
		return EINVAL;
	}
	while ((len > 0 && error == 0) || head != tail) {
		/* as many more as there are free slots (and one, waiting for it, if none is in flight) */
		while (len > 0 && error == 0 && tail - head < SLOTS) {
			uint64_t first = offset / BLOCK, skip = offset % BLOCK;
			uint64_t blocks = (skip + len + BLOCK - 1) / BLOCK, take;
			struct hc_slot *s = hc_take(head == tail);
			struct hc_cmd c;
			int e;

			if (s == NULL) {
				if (head == tail) {
					error = ENXIO;                  /* the path went off while waiting */
				}
				break;
			}
			if (blocks > per) {
				blocks = per;
			}
			take = blocks * BLOCK - skip;
			if (take > len) {
				take = len;
			}
			hc_make_rw(&c, lun, (uint8_t)s->number, first, (uint32_t)blocks, write);
			if (write) {
				memcpy(s->data, buf, (size_t)take);
			}
			/* nothing of the buffer is left in a cache to be written back over what the controller writes */
			FlushPoC_DcacheRegion((vm_offset_t)s->data, (size_t)blocks * BLOCK);
			e = hc_place(s, &c, (uint32_t)blocks * BLOCK);
			if (e == 0) {
				e = hc_ring(s, true);
			}
			if (e == 0 && hc_hang_after != 0 && hc_requests >= hc_hang_after &&
			    os_atomic_cmpxchg(&hc_hanging, 0, 1, relaxed)) {
				hc_hang(s->number);
			}
			if (e != 0) {
				hc_give(s);
				error = e;
				break;
			}
			q[tail % SLOTS].slot = s;
			q[tail % SLOTS].lba = first;
			q[tail % SLOTS].blocks = (uint32_t)blocks;
			q[tail % SLOTS].skip = (uint32_t)skip;
			q[tail % SLOTS].take = (uint32_t)take;
			q[tail % SLOTS].to = buf;
			tail++;
			offset += take;
			buf += take;
			len -= take;
		}
		/* the oldest one done, copied out, its slot free again */
		if (head != tail) {
			typeof(q[0]) *e = &q[head % SLOTS];
			char what[64];
			int r;

			snprintf(what, sizeof(what), "%s LUN %u, blocks %llu..%llu", write ? "write" : "read", lun, e->lba,
			    e->lba + e->blocks - 1);
			r = hc_complete(e->slot, lun, what, false);
			if (r == 0) {
				FlushPoC_DcacheRegion((vm_offset_t)e->slot->data, (size_t)e->blocks * BLOCK);
				os_atomic_inc(&hc_requests, relaxed);
				os_atomic_add(&hc_bytes, (uint64_t)e->blocks * BLOCK, relaxed);
				hc_speed_add(e->slot, write, (uint64_t)e->blocks * BLOCK);
				if (write) {
					os_atomic_inc(&hc_writes, relaxed);
					os_atomic_add(&hc_write_bytes, (uint64_t)e->blocks * BLOCK, relaxed);
				} else if (error == 0) {
					memcpy(e->to, e->slot->data + e->skip, e->take);
				}
			} else if (error == 0) {
				error = r == EAGAIN ? EIO : r;
			}
			if (r != ETIMEDOUT) {
				hc_give(e->slot);                       /* (a slot that never came back is not used again) */
			}
			head++;
		}
	}
	return error;
}

int
ufshc_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len)
{
	return hc_rw(lun, offset, dst, len, false);
}

/* own mode: whole blocks into the window's unit (the whitelist refuses anything outside the window) */
int
ufshc_write(uint8_t lun, uint64_t offset, const uint8_t *src, uint64_t len)
{
	int e;

	if (hc_mode != HC_OWN || !hc_pol.window || lun != hc_pol.w_lun) {
		return EROFS;
	}
	hc_dirty = true;
	e = hc_rw(lun, offset, (uint8_t *)(uintptr_t)src, len, true);
	return e;
}

/* ---- own mode's control commands, in slot 0 (hc_ctl_lock held) ---- */

static int
hc_run0(const struct hc_cmd *c, uint32_t data_in, const char *what)
{
	struct hc_slot *s = &hc_slots[SLOT_CONTROL];
	int e;

	if (data_in) {
		FlushPoC_DcacheRegion((vm_offset_t)s->data, data_in);
	}
	e = hc_place(s, c, data_in);
	if (e == 0) {
		e = hc_ring(s, c->upiu[0] == UPIU_COMMAND);
	}
	if (e == 0) {
		e = hc_wait(s, what, false);
	}
	if (e == 0 && data_in) {
		FlushPoC_DcacheRegion((vm_offset_t)s->data, data_in);
	}
	return e;
}

static int
hc_nop(void)
{
	volatile const uint8_t *r = (volatile const uint8_t *)hc_slots[SLOT_CONTROL].ucd + hc_soc->rsp_offset;
	struct hc_cmd c;
	int e;

	bzero(&c, sizeof(c));
	c.upiu[0] = UPIU_NOP_OUT;
	c.upiu[3] = SLOT_CONTROL;
	c.dw0 = UTRD_CT_UFS | UTRD_DD_NONE;
	lck_mtx_lock(&hc_ctl_lock);
	e = hc_run0(&c, 0, "NOP OUT");
	if (e == 0 && (hc_slots[SLOT_CONTROL].utrd[2] & 0xff) != OCS_SUCCESS) {
		e = EIO;
	} else if (e == 0 && ((r[0] & 0x3f) != UPIU_NOP_IN || r[3] != SLOT_CONTROL || r[6] != 0 || r[10] || r[11])) {
		e = hc_protocol(&hc_slots[SLOT_CONTROL], "NOP OUT", "not a NOP IN that says success");
	}
	lck_mtx_unlock(&hc_ctl_lock);
	return e;
}

/* a query: the 32-bit value it answers (attributes; a flag in the low byte), and a descriptor's bytes */
static bool hc_quiet;                           /* looking for units: a unit that is not there is no news */

static int
hc_query(uint8_t function, uint8_t opcode, uint8_t idn, uint8_t index, uint32_t *value, uint8_t *desc, uint32_t *desc_len)
{
	struct hc_slot *s = &hc_slots[SLOT_CONTROL];
	volatile const uint8_t *r = (volatile const uint8_t *)s->ucd + hc_soc->rsp_offset;
	uint16_t want = opcode == QOP_READ_DESC ? (uint16_t)*desc_len : 0;
	struct hc_cmd c;
	char what[48];
	int e;

	snprintf(what, sizeof(what), "query %02x/%02x idn %02x index %u", function, opcode, idn, index);
	hc_make_query(&c, SLOT_CONTROL, function, opcode, idn, index, want);
	lck_mtx_lock(&hc_ctl_lock);
	e = hc_run0(&c, 0, what);
	if (e == 0 && (s->utrd[2] & 0xff) != OCS_SUCCESS) {
		if (!hc_quiet) {
			hc_say("ufs: own: %s: OCS %x\n", what, s->utrd[2] & 0xff);
		}
		e = EIO;
	}
	if (e == 0) {
		uint32_t seg = (uint32_t)r[10] << 8 | r[11];

		if ((r[0] & 0x3f) != UPIU_QUERY_RSP || r[3] != SLOT_CONTROL || r[8] != 0 || seg > hc_soc->rsp_length - 32 ||
		    r[12] != opcode || r[13] != idn || r[14] != index) {
			e = hc_protocol(s, what, "not the answer to this query");
		} else if (r[6] != 0) {
			if (!hc_quiet) {
				hc_say("ufs: own: %s: the device answers %02x\n", what, r[6]);
			}
			e = EIO;
		} else {
			if (value != NULL) {
				*value = be32(r + 20);
			}
			if (desc != NULL) {
				uint32_t n = seg < *desc_len ? seg : *desc_len;

				for (uint32_t i = 0; i < n; i++) {
					desc[i] = r[32 + i];
				}
				*desc_len = n;
			}
		}
	}
	lck_mtx_unlock(&hc_ctl_lock);
	return e;
}

/* a SCSI command of no data or data_in bytes into out, in slot 0 */
static int
hc_scsi0(uint8_t lun, const uint8_t *cdb, uint32_t cdb_len, uint32_t data_in, uint8_t *out, const char *what, bool quiet)
{
	struct hc_slot *s = &hc_slots[SLOT_CONTROL];
	struct hc_cmd c;
	int e;

	hc_make_scsi(&c, lun, SLOT_CONTROL, cdb, cdb_len, data_in);
	lck_mtx_lock(&hc_ctl_lock);
	e = hc_run0(&c, data_in, what);
	if (e == 0) {
		e = hc_scsi_result(s, lun, what, quiet);
	}
	if (e == 0 && out != NULL) {
		memcpy(out, s->data, data_in);
	}
	lck_mtx_unlock(&hc_ctl_lock);
	return e;
}

/* SYNCHRONIZE CACHE(10) of the window's unit: what was written is on the medium */
int
ufshc_sync(void)
{
	static const uint8_t sync10[10] = { 0x35 };
	int e;

	if (hc_mode != HC_OWN || !hc_pol.window) {
		return 0;
	}
	if (!hc_dirty) {
		return 0;
	}
	e = hc_scsi0(hc_pol.w_lun, sync10, sizeof(sync10), 0, NULL, "SYNCHRONIZE CACHE", false);
	if (e == 0) {
		hc_dirty = false;
	}
	return e;
}

/* ---- shared mode: the proof, and the switches ---- */

/* UEFI's slot 0 holds a read (what the controller would run were it to take no notice of this list) */
static const char *
hc_uefi_slot0(uint8_t *upiu, uint32_t *dw0)
{
	static const struct hc_policy any_read = { .read_luns = 0xff };
	uint64_t ucd = (uint64_t)hc_uefi_list[5] << 32 | hc_uefi_list[4];
	volatile const uint8_t *u;

	*dw0 = hc_uefi_list[0];
	if (ucd < hc_uefi_map_pa || ucd + 32 > hc_uefi_map_pa + hc_uefi_map_size) {
		return "UEFI's command descriptor is not where it was";
	}
	u = (volatile const uint8_t *)(hc_uefi_map_va + (ucd - hc_uefi_map_pa));
	for (int i = 0; i < 32; i++) {
		upiu[i] = u[i];
	}
	return hc_refusal(upiu, *dw0, &any_read, true) == NULL ? NULL : "UEFI's slot 0 does not hold a read";
}

/* This kernel's slot 0: TEST UNIT READY rung, and its status must come back in this list */
static int
hc_prove(void)
{
	struct hc_slot *s = &hc_slots[SLOT_CONTROL];
	uint8_t lun = (uint8_t)__builtin_ctz(hc_pol.read_luns);
	struct hc_cmd c;
	uint32_t ocs;
	int e;

	hc_make_tur(&c, lun, SLOT_CONTROL);
	e = hc_place(s, &c, 0);
	if (e == 0) {
		e = hc_ring(s, true);
	}
	if (e == 0) {
		e = hc_complete(s, lun, "the proof's TEST UNIT READY", true);
	}
	ocs = s->utrd[2] & 0xff;
	hc_proofs++;
	if (e != 0 || ocs != OCS_SUCCESS) {
		hc_say("ufs: native: proof FAILED (%d): this list's slot 0 has OCS %x after its doorbell -- the controller "
		    "did not run it from this list\n", e, ocs);
		return e != 0 ? e : EIO;
	}
	hc_say("ufs: native: proof: TEST UNIT READY in this list's slot 0 came back here (OCS 0, response %02x, status %02x)\n",
	    ((volatile const uint8_t *)s->ucd)[hc_soc->rsp_offset + 6], ((volatile const uint8_t *)s->ucd)[hc_soc->rsp_offset + 7]);
	return 0;
}

struct hc_put_back_result {
	uint32_t db_before, db_after_clear, lo, hi, rsr, db, is, cnr, cnr_ours, is_ours, iacr;
	bool cleared, ok;
};

/*
 * UEFI's list back into the controller: what is in flight waited for (1 s), cleared if it does
 * not finish (every slot of this list is this kernel's), the list stopped, UEFI's address
 * written, the list started (if UEFI had it running), read back. Interrupts off, no locks, no
 * printing (the quiesce). Own mode: UEFI's interrupt enable and the vendor's registers too.
 */
static void
hc_put_back(struct hc_put_back_result *r)
{
	uint64_t t;
	bool run = hc_mode == HC_SHARED || (hc_uefi_rsr & 1);

	bzero(r, sizeof(*r));
	for (t = mach_absolute_time(); (r->db_before = hc_read(UFSHC_UTRLDBR)) != 0 && hc_us_since(t) < 1000000;) {
		__builtin_arm_yield();
	}
	if (r->db_before != 0 && hc_read(UFSHC_UTRLBA) == hc_list_lo && hc_read(UFSHC_UTRLBAU) == hc_list_hi) {
		r->cleared = true;
		hc_write(UFSHC_UTRLCLR, hc_soc->clear_with_ones ? r->db_before : 0);
		for (t = mach_absolute_time(); (r->db_after_clear = hc_read(UFSHC_UTRLDBR)) != 0 && hc_us_since(t) < 1000000;) {
			__builtin_arm_yield();
		}
	}
	/* the completion notifications this kernel's requests left: exactly those bits, written with ones */
	r->cnr_ours = hc_read(UFSHC_UTRLCNR) & ~hc_cnr_in;
	if (r->cnr_ours != 0) {
		hc_write(UFSHC_UTRLCNR, r->cnr_ours);
	}
	hc_write(UFSHC_UTRLRSR, 0);
	for (t = mach_absolute_time(); (hc_read(UFSHC_UTRLRSR) & 1) && hc_us_since(t) < 5000;) {
		__builtin_arm_yield();
	}
	hc_write(UFSHC_UTRLBA, hc_loader.uefi_lo);
	hc_write(UFSHC_UTRLBAU, hc_loader.uefi_hi);
	if (hc_mode == HC_OWN) {
		if (hc_soc->give_back != NULL) {
			hc_soc->give_back();
		}
		/* the completion status this kernel's requests left, cleared (UEFI found none); error bits stay as evidence */
		r->is_ours = hc_read(UFSHC_IS) & ~hc_is_in & IS_UTRCS;
		if (r->is_ours != 0) {
			hc_write(UFSHC_IS, r->is_ours);
		}
		hc_write(UFSHC_IE, hc_uefi_ie);
	}
	if (run) {
		hc_write(UFSHC_UTRLRSR, 1);
	}
	r->lo = hc_read(UFSHC_UTRLBA);
	r->hi = hc_read(UFSHC_UTRLBAU);
	r->rsr = hc_read(UFSHC_UTRLRSR);
	r->db = hc_read(UFSHC_UTRLDBR);
	r->is = hc_read(UFSHC_IS);
	r->cnr = hc_read(UFSHC_UTRLCNR);
	r->iacr = hc_read(UFSHC_UTRIACR);
	r->ok = r->lo == hc_loader.uefi_lo && r->hi == hc_loader.uefi_hi && (!run || (r->rsr & 1));
	if (r->ok && hc_mode == HC_SHARED) {
		*hc_loader.list_in = 0;
		(*hc_loader.switched_out)++;
	}
	if (r->ok && r->db == 0 && hc_mode == HC_OWN && hc_own_list_in != NULL) {
		*hc_own_list_in = 0;            /* (ufs=3) the loader may use UEFI's list again */
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	if (r->ok) {
		hc_switch_out_count++;
	}
}

static void
hc_put_back_print(const char *why, const struct hc_put_back_result *r, bool late)
{
	void (*say)(const char *, ...) = late ? hc_say_late : hc_say;

	if (r->cleared) {
		say("ufs: native: %s: doorbell %08x did not clear in 1 s: this kernel's slots cleared (UTRLCLR), doorbell "
		    "then %08x\n", why, r->db_before, r->db_after_clear);
	}
	say("ufs: native: %s: transfer list %s UEFI's %08x:%08x (run %u, doorbell %08x; IS %08x, at the switch %08x; "
	    "completion %08x after clearing this kernel's %08x, at the switch %08x%s; IS's UTRCS this kernel left %s; "
	    "aggregation %08x, at the switch %08x; %llu done slots cleared with UTRLCLR)\n", why, r->ok ? "back to" :
	    "NOT BACK TO", r->hi, r->lo, r->rsr, r->db, r->is, hc_is_in, r->cnr, r->cnr_ours, hc_cnr_in,
	    r->cnr == hc_cnr_in ? ", as it was" : ", NOT AS IT WAS", r->is_ours ? "cleared" : "none", r->iacr, hc_iacr_in,
	    hc_cleared);
}

/*
 * This kernel's list into the controller (the caller holds the loader's mailbox: nothing is asked
 * of UEFI meanwhile). prove: then slot 0's TEST UNIT READY, before anything is read.
 */
int
ufshc_switch_in(bool prove)
{
	uint32_t lo, hi, db, rsr, hcs, uefi_dw0 = 0;
	uint8_t uefi_upiu[32] = { 0 };
	const char *no = NULL;
	struct hc_put_back_result undo;
	boolean_t istate;
	bool undone = false;

	if (!hc_usable || hc_dead || hc_closed || hc_mode != HC_SHARED) {
		return ENXIO;
	}
	lck_mtx_lock(&hc_lock);
	if (hc_in) {
		lck_mtx_unlock(&hc_lock);
		return 0;
	}
	istate = ml_set_interrupts_enabled(FALSE);
	if (*hc_loader.take_back != 0) {
		no = "the loader has taken the controller back";
	} else {
		no = hc_uefi_slot0(uefi_upiu, &uefi_dw0);
	}
	lo = hc_read(UFSHC_UTRLBA);
	hi = hc_read(UFSHC_UTRLBAU);
	db = hc_read(UFSHC_UTRLDBR);
	rsr = hc_read(UFSHC_UTRLRSR);
	hcs = hc_read(UFSHC_HCS);
	if (no == NULL) {
		if (lo != hc_loader.uefi_lo || hi != hc_loader.uefi_hi) {
			no = "the controller's list is not UEFI's";
		} else if (db != 0) {
			no = "a request in flight";
		} else if ((rsr & 1) == 0 || (hcs & HCS_UTRLRDY) == 0) {
			no = "the list is not running, or not ready";
		}
	}
	if (no == NULL) {
		uint64_t t;

		hc_is_in = hc_read(UFSHC_IS);
		hc_cnr_in = hc_read(UFSHC_UTRLCNR);
		hc_iacr_in = hc_read(UFSHC_UTRIACR);
		hc_write(UFSHC_UTRLRSR, 0);
		for (t = mach_absolute_time(); (hc_read(UFSHC_UTRLRSR) & 1) && hc_us_since(t) < 5000;) {
			__builtin_arm_yield();
		}
		if (hc_read(UFSHC_UTRLRSR) & 1) {
			no = "the list did not stop";
			hc_write(UFSHC_UTRLRSR, 1);     /* (UEFI's list, running, as it was) */
		} else {
			hc_write(UFSHC_UTRLBA, hc_list_lo);
			hc_write(UFSHC_UTRLBAU, hc_list_hi);
			hc_write(UFSHC_UTRLRSR, 1);
			lo = hc_read(UFSHC_UTRLBA);
			hi = hc_read(UFSHC_UTRLBAU);
			rsr = hc_read(UFSHC_UTRLRSR);
			if (lo != hc_list_lo || hi != hc_list_hi || (rsr & 1) == 0) {
				no = "the controller did not take this list";
				hc_put_back(&undo);
				undone = true;
			} else {
				hc_in = true;
				*hc_loader.cnr_at_switch = hc_cnr_in;
				*hc_loader.list_in = 1;
				(*hc_loader.switched_in)++;
				hc_switch_in_count++;
			}
		}
	}
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	if (undone) {
		hc_put_back_print("undoing", &undo, false);
	}
	if (no != NULL) {
		hc_say("ufs: native: this kernel's list not put in: %s (list %08x:%08x, doorbell %08x, run %08x, HCS %08x; UEFI's "
		    "slot 0 %08x, CDB %02x)\n", no, hi, lo, db, rsr, hcs, uefi_dw0, uefi_upiu[16]);
		return EIO;
	}
	hc_say("ufs: native: transfer list switched from UEFI's %08x:%08x to this kernel's %08x:%08x (doorbell 0, run %u; "
	    "IS %08x, completion %08x; UEFI's slot 0 holds a read: %08x, CDB %02x)\n", hc_loader.uefi_hi,
	    hc_loader.uefi_lo, hi, lo, rsr, hc_is_in, hc_cnr_in, uefi_dw0, uefi_upiu[16]);
	if (prove) {
		int e = hc_prove();

		if (e != 0) {
			ufshc_switch_out("after the failed proof");
			hc_off();
			return e;
		}
	}
	return 0;
}

/* UEFI's list back (the caller holds the loader's mailbox) */
int
ufshc_switch_out(const char *why)
{
	struct hc_put_back_result r;
	uint64_t started;
	boolean_t istate;

	if (hc_mode != HC_SHARED) {
		return 0;
	}
	lck_mtx_lock(&hc_lock);
	if (!hc_in) {
		lck_mtx_unlock(&hc_lock);
		return 0;
	}
	hc_in = false;                          /* nothing more is rung */
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
	/* what is in flight finishes (whoever waits for it sees it done) */
	for (started = mach_absolute_time(); hc_read(UFSHC_UTRLDBR) != 0 && hc_us_since(started) < 2000000;) {
		IOSleep(1);
	}
	lck_mtx_lock(&hc_lock);
	istate = ml_set_interrupts_enabled(FALSE);
	hc_put_back(&r);
	ml_set_interrupts_enabled(istate);
	lck_mtx_unlock(&hc_lock);
	hc_put_back_print(why, &r, false);
	if (!r.ok) {
		hc_off();
		return EIO;
	}
	return 0;
}

bool
ufshc_list_in(void)
{
	return hc_in;
}

bool
ufshc_available(void)
{
	return hc_usable && !hc_dead && !hc_closed;
}

bool
ufshc_own(void)
{
	return hc_mode == HC_OWN && hc_usable;
}

void
ufshc_set_luns(uint32_t mask)
{
	hc_pol.read_luns = mask & 0xff;
}

const struct ufshc_unit *
ufshc_unit(uint8_t lun)
{
	return lun < UFSHC_LUNS && hc_units[lun].present ? &hc_units[lun] : NULL;
}

/*
 * Own mode: the one window writes may go into from now on -- blocks [first_lba, first_lba +
 * blocks) of logical unit lun. Once a run; never a boot unit or a write-protected one.
 */
int
ufshc_open_window(uint8_t lun, uint64_t first_lba, uint64_t blocks)
{
	const struct ufshc_unit *u = ufshc_unit(lun);

	if (hc_mode != HC_OWN || !hc_usable) {
		return ENXIO;
	}
	if (hc_pol.window) {
		return EBUSY;
	}
	if (u == NULL || u->block_size != BLOCK || blocks == 0 || first_lba >= u->blocks || blocks > u->blocks - first_lba) {
		return EINVAL;
	}
	if (u->boot_lun_id != 0 || u->write_protect != 0) {
		hc_say("ufs: own: no window on LUN %u: it is %s\n", lun, u->boot_lun_id ? "a boot unit" : "write protected");
		return EROFS;
	}
	{
		/* (ufs=3) not over anything protected, nor while everything is read only: refused whole */
		const char *why = hc_protected(&hc_pol, lun, first_lba, blocks);

		if (why != NULL) {
			hc_say("ufs: own: no window on LUN %u blocks %llu..%llu: %s\n", lun, first_lba, first_lba + blocks - 1, why);
			return EROFS;
		}
	}
	lck_mtx_lock(&hc_lock);
	hc_pol.w_lun = lun;
	hc_pol.w_first = first_lba;
	hc_pol.w_blocks = blocks;
	hc_pol.window = true;
	lck_mtx_unlock(&hc_lock);
	hc_say("ufs: own: writes may go to LUN %u, blocks %llu..%llu, and nowhere else\n", lun, first_lba,
	    first_lba + blocks - 1);
	return 0;
}

/*
 * The second layer, tried: a write of one block at lba of lun made and put to the whitelist
 * alone (no slot, no doorbell). EPERM when refused -- which is what a block outside the window
 * must be -- 0 when it would have passed.
 */
int
ufshc_selftest_refusal(uint8_t lun, uint64_t lba)
{
	struct hc_cmd c;

	hc_make_rw(&c, lun, 2, lba, 1, true);
	return ufshc_permitted(c.upiu, c.dw0) ? 0 : EPERM;
}

/* ---- ufs=3 (hakios.ufs.c): the fences, said into the loader's log ---- */

void
ufshc_log_direct(bool on)
{
	hc_direct = on;
}

/*
 * Blocks [first, first + blocks) of lun never written from now on, whatever the window says.
 * Ranges are only ever added; once the window is open none is (the fence is set before it).
 */
int
ufshc_deny(uint8_t lun, uint64_t first, uint64_t blocks, const char *what)
{
	struct hc_deny *d;

	if (lun >= UFSHC_LUNS || blocks == 0) {
		return EINVAL;
	}
	lck_mtx_lock(&hc_lock);
	if (hc_pol.window || hc_pol.ndeny >= HC_DENY_MAX) {
		lck_mtx_unlock(&hc_lock);
		/* no room: then nothing at all may be written */
		hc_pol.ro = true;
		hc_say("ufs: nufs: protection list full or set too late (%s): EVERYTHING READ ONLY\n", what);
		return ENOSPC;
	}
	d = &hc_denied[hc_pol.ndeny];
	d->lun = lun;
	d->first = first;
	d->blocks = blocks;
	snprintf(d->what, sizeof(d->what), "protected: %s", what);
	hc_pol.deny = hc_denied;
	__asm__ volatile ("dmb ish" ::: "memory");
	hc_pol.ndeny++;
	lck_mtx_unlock(&hc_lock);
	return 0;
}

void
ufshc_deny_lun(uint8_t lun)
{
	if (lun < UFSHC_LUNS) {
		lck_mtx_lock(&hc_lock);
		hc_pol.deny_luns |= 1u << lun;
		lck_mtx_unlock(&hc_lock);
	}
}

/* nothing at all written for the rest of the run (there is no way back from it) */
void
ufshc_force_ro(void)
{
	lck_mtx_lock(&hc_lock);
	hc_pol.ro = true;
	lck_mtx_unlock(&hc_lock);
}

/*
 * What the whitelist would say of a write of blocks at lba of lun: NULL if it would pass, the
 * reason it is refused otherwise. Made and judged only, never placed in a slot, not counted.
 */
const char *
ufshc_write_refusal(uint8_t lun, uint64_t lba, uint32_t blocks)
{
	struct hc_cmd c;

	hc_make_rw(&c, lun, 2, lba, blocks, true);
	return hc_refusal(c.upiu, c.dw0, &hc_pol, false);
}

/*
 * ufs=3: own mode on the SoC named, as far as step (5: all), with the loader's page told
 * (list_in) before this kernel's list goes into the controller and after UEFI's is back.
 */
static bool hc_own_probe(const char *name, uint32_t mode, uint32_t step);

bool
ufshc_take(const char *soc, uint32_t step, volatile uint32_t *list_in, const uint8_t *ref, uint32_t ref_blocks)
{
	hc_own_list_in = list_in;
	hc_layout_ref = ref;
	hc_layout_ref_blocks = ref_blocks;
	hc_take_why[0] = 0;
	return hc_own_probe(soc, 4, step);
}

/*
 * ufs=3, the way back to the loader within a run: nothing more sent, what is in flight waited
 * for (or cleared), UEFI's list, interrupt enable and nexus types put back and read back. true
 * only if UEFI's list is in again and running as it was: only then may the loader be asked.
 */
bool
ufshc_release(const char *why)
{
	struct hc_put_back_result r;

	if (hc_mode != HC_OWN) {
		return hc_own_list_in == NULL || *hc_own_list_in == 0;
	}
	lck_mtx_lock(&hc_lock);
	hc_closed = true;               /* nothing more taken from here on */
	wakeup(&hc_free);
	lck_mtx_unlock(&hc_lock);
	for (uint64_t t = mach_absolute_time(); hc_in_flight != 0 && hc_us_since(t) < 2000000;) {
		IOSleep(1);
	}
	hc_put_back(&r);
	hc_put_back_print(why, &r, false);
	hc_in = false;
	hc_usable = false;
	hc_mode = HC_NONE;
	hc_off();
	if (!r.ok || r.db != 0) {
		hc_say("ufs: nufs: %s: UEFI's list NOT back as it was: the loader must not be asked (it refuses anyway)\n", why);
		return false;
	}
	return true;
}

static void
hc_report(const char *when, bool late)
{
	(late ? hc_say_late : hc_say)("ufs: native: %s: %llu requests (%llu writes, %llu bytes written), %llu errors, %llu "
	    "bytes, %llu ms waiting (summed over reads), at most %llu in flight, %llu refused, %llu protocol errors, list in "
	    "%llu times and out %llu times, %llu proofs%s\n", when, hc_requests, hc_writes, hc_write_bytes, hc_errors,
	    hc_bytes, hc_wait_us / 1000, hc_most_in_flight, hc_refused, hc_protocol_errors, hc_switch_in_count,
	    hc_switch_out_count, hc_proofs, hc_dead ? "; THE PATH IS OFF" : "");
	{
		uint64_t wait_ns, lock_ns;

		absolutetime_to_nanoseconds(hc_slot_wait_ticks, &wait_ns);
		absolutetime_to_nanoseconds(hc_lock_ticks, &lock_ns);
		(late ? hc_say_late : hc_say)("ufs: native: %s: %llu doorbell reads while waiting (%llu a request), %llu waits "
		    "for a free slot (%llu ms), %llu ms taking the slots' lock; requests put in with 1 in flight %llu, 2 %llu, "
		    "3-4 %llu, 5-8 %llu, 9-16 %llu, more %llu\n", when, hc_polls, hc_requests ? hc_polls / hc_requests : 0,
		    hc_slot_waits, wait_ns / 1000000, lock_ns / 1000000, hc_flight_hist[0], hc_flight_hist[1],
		    hc_flight_hist[2], hc_flight_hist[3], hc_flight_hist[4], hc_flight_hist[5]);
		(late ? hc_say_late : hc_say)("ufs: native: %s: waiters spin %u us, then sleep in 1 ms steps: %llu slept\n",
		    when, hc_spin_us, hc_sleeps);
		(late ? hc_say_late : hc_say)("ufs: native: %s: completion: done by the doorbell bit %llu, by UTRLCNR %llu "
		    "(%s), by the descriptor and response %llu; doorbell bits cleared with UTRLCLR %llu; not finished at once "
		    "%llu\n", when, hc_done_by[HC_BY_DB], hc_done_by[HC_BY_CNR], hc_cnr_broken ? "NOT USED: a bit did not clear" :
		    hc_cnr_on() ? "used" : "not used", hc_done_by[HC_BY_OCS], hc_cleared, hc_finish_failed);
		{
			char line[320];

			hc_speed_line(line, sizeof(line));
			(late ? hc_say_late : hc_say)("%s", line);
		}
	}
}

/* The speed line of the summary (hakios_ufs_summary): false while this kernel's driver is not in use */
bool
ufshc_speed(char *buf, size_t size)
{
	if (!hc_usable || hc_mode != HC_OWN) {
		return false;
	}
	hc_speed_line(buf, size);
	return true;
}

void
ufshc_report(const char *when)
{
	if (hc_usable) {
		hc_report(when, false);
	}
}

static void
hc_boot_args(void)
{
	PE_parse_boot_argn("ufs_native_buffer", &hc_slot_buffer, sizeof(hc_slot_buffer));
	if (hc_slot_buffer < BLOCK || hc_slot_buffer > (4u << 20) || hc_slot_buffer % BLOCK) {
		hc_slot_buffer = hc_soc->slot_buffer;
	}
	PE_parse_boot_argn("ufs_native_leave", &hc_leave, sizeof(hc_leave));
	PE_parse_boot_argn("ufs_native_ref_fnv", &hc_layout_ref_fnv, sizeof(hc_layout_ref_fnv));
	PE_parse_boot_argn("ufs_native_prd", &hc_prd_arg, sizeof(hc_prd_arg));
	if (hc_prd_arg != 16 && hc_prd_arg != 128) {
		hc_prd_arg = 0;
	}
	PE_parse_boot_argn("ufs_native_hang", &hc_hang_after, sizeof(hc_hang_after));
	PE_parse_boot_argn("ufs_native_spin", &hc_spin_us, sizeof(hc_spin_us));
	if (hc_spin_us > 5000) {
		hc_spin_us = 5000;
	}
}

/* ---- shared mode's probe ---- */

/*
 * What the controller looks like as UEFI left it, and whether this kernel may put a list of its
 * own in (only == true: say so and do not use it).
 */
bool
ufshc_probe(const struct ufshc_loader *loader, bool only)
{
	uint32_t cap, ver, hcs, hce, is, ie, iacr, ahit, base, baseu, db, rsr, cnr, tm_base, tm_baseu, tm_db, tm_rsr;
	uint32_t uefi_dw0 = 0, slots = 31;
	uint8_t uefi_upiu[32] = { 0 };
	uint64_t uefi_pa;
	const char *no = NULL;
	vm_offset_t area;

	if (!hc_whitelist_selftest()) {
		return false;
	}
	hc_soc = SOC_SHARED;
	hc_loader = *loader;
	hc_slot_buffer = SLOT_BUFFER_DEFAULT;
	hc_boot_args();
	PE_parse_boot_argn("ufs_native_slots", &slots, sizeof(slots));
	if (slots < 1 || slots > SLOTS - 1) {
		slots = SLOTS - 1;
	}
	if (loader->hc_pa != hc_soc->pa || loader->area_pa == 0 || loader->slots != SLOTS || (loader->area_pa & 0x3ff) ||
	    loader->ucd_size < hc_soc->prdt_offset + hc_soc->prd_entry * (hc_slot_buffer / hc_soc->prd_max + 1) ||
	    (loader->ucd_offset & 0x7f) || (loader->ucd_size & 0x7f) || loader->ucd_offset < SLOTS * 32 ||
	    loader->area_size < loader->ucd_offset + SLOTS * loader->ucd_size) {
		hc_say("ufs: native: not used: the loader offers no area for a list (or not one as expected: controller %llx, "
		    "area %llx of %u, command descriptors at %u of %u)\n", loader->hc_pa, loader->area_pa, loader->area_size,
		    loader->ucd_offset, loader->ucd_size);
		return false;
	}
	if (pmap_valid_address(loader->area_pa) || pmap_valid_address(loader->area_pa + loader->area_size - 1)) {
		hc_say("ufs: native: not used: the loader's area %llx is memory this kernel manages\n", loader->area_pa);
		return false;
	}
	hc = ml_io_map(hc_soc->pa, hc_soc->size);
	cap = hc_read(UFSHC_CAP);
	ver = hc_read(UFSHC_VER);
	hcs = hc_read(UFSHC_HCS);
	hce = hc_read(UFSHC_HCE);
	is = hc_read(UFSHC_IS);
	ie = hc_read(UFSHC_IE);
	iacr = hc_read(UFSHC_UTRIACR);
	ahit = hc_read(UFSHC_AHIT);
	base = hc_read(UFSHC_UTRLBA);
	baseu = hc_read(UFSHC_UTRLBAU);
	db = hc_read(UFSHC_UTRLDBR);
	rsr = hc_read(UFSHC_UTRLRSR);
	cnr = hc_read(UFSHC_UTRLCNR);
	tm_base = hc_read(UFSHC_UTMRLBA);
	tm_baseu = hc_read(UFSHC_UTMRLBAU);
	tm_db = hc_read(UFSHC_UTMRLDBR);
	tm_rsr = hc_read(UFSHC_UTMRLRSR);
	hc_say("ufs: native: controller at %llx: CAP %08x (%u slots, 64-bit %s), VER %08x, HCDDID %08x, HCPMID %08x\n",
	    hc_soc->pa, cap, CAP_NUTRS(cap), (cap & CAP_64AS) ? "yes" : "no", ver, hc_read(UFSHC_HCDDID),
	    hc_read(UFSHC_HCPMID));
	hc_say("ufs: native: HCE %08x, HCS %08x, IS %08x, IE %08x, UTRIACR %08x, AHIT %08x\n", hce, hcs, is, ie, iacr, ahit);
	hc_say("ufs: native: transfer list %08x:%08x (the loader's note of UEFI's: %08x:%08x), doorbell %08x, run %08x, "
	    "completion %08x; task management list %08x:%08x, doorbell %08x, run %08x\n", baseu, base, loader->uefi_hi,
	    loader->uefi_lo, db, rsr, cnr, tm_baseu, tm_base, tm_db, tm_rsr);
	uefi_pa = (uint64_t)baseu << 32 | base;

	if ((hce & 1) == 0) {
		no = "the controller is not enabled";
	} else if ((hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY)) {
		no = "no device present or the transfer list not ready";
	} else if ((rsr & 1) == 0) {
		no = "the transfer list is not running";
	} else if (ver < 0x200 || ver >= 0x400) {
		no = "a controller version other than 2.x or 3.x";
	} else if (CAP_NUTRS(cap) < SLOTS) {
		no = "fewer than 32 slots";
	} else if ((cap & CAP_64AS) == 0) {
		no = "no 64-bit addressing";
	} else if (is & (IS_FATAL | IS_UE)) {
		no = "an error status";
	} else if (base != loader->uefi_lo || baseu != loader->uefi_hi) {
		no = "the transfer list is not the one the loader noted as UEFI's";
	} else if (db != 0) {
		no = "a request in flight";
	} else if (pmap_valid_address(uefi_pa)) {
		no = "UEFI's transfer list is in memory this kernel manages";
	}
	if (no == NULL) {
		/* UEFI's list and command descriptor (the page they are in), not cached here, only read */
		hc_uefi_map_pa = uefi_pa & ~(uint64_t)PAGE_MASK;
		hc_uefi_map_size = PAGE_SIZE;
		hc_uefi_map_va = ml_io_map_unmappable((vm_offset_t)hc_uefi_map_pa, PAGE_SIZE, VM_WIMG_WCOMB);
		hc_uefi_list = (volatile uint32_t *)(hc_uefi_map_va + (uefi_pa - hc_uefi_map_pa));
		hc_say("ufs: native: UEFI's slot 0: %08x %08x %08x %08x %08x %08x %08x %08x\n", hc_uefi_list[0], hc_uefi_list[1],
		    hc_uefi_list[2], hc_uefi_list[3], hc_uefi_list[4], hc_uefi_list[5], hc_uefi_list[6], hc_uefi_list[7]);
		no = hc_uefi_slot0(uefi_upiu, &uefi_dw0);
		hc_say("ufs: native: UEFI's last command: UPIU %02x %02x %02x %02x, CDB %02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x\n", uefi_upiu[0], uefi_upiu[1], uefi_upiu[2], uefi_upiu[3], uefi_upiu[16], uefi_upiu[17],
		    uefi_upiu[18], uefi_upiu[19], uefi_upiu[20], uefi_upiu[21], uefi_upiu[22], uefi_upiu[23], uefi_upiu[24],
		    uefi_upiu[25]);
	}
	if (no != NULL) {
		hc_say("ufs: native: not used: %s; the storage is read through the loader\n", no);
		return false;
	}
	if (only) {
		hc_say("ufs: native: as expected; ufs_native=2 asks for no more than this look\n");
		return false;
	}

	/* this kernel's list and command descriptors, in the loader's area; every slot TEST UNIT READY to begin with */
	hc_mode = HC_SHARED;
	hc_ucd_size = loader->ucd_size;
	area = ml_io_map_unmappable((vm_offset_t)(loader->area_pa & ~(uint64_t)PAGE_MASK),
	    (vm_size_t)round_page((loader->area_pa & PAGE_MASK) + loader->area_size), VM_WIMG_WCOMB) +
	    (vm_offset_t)(loader->area_pa & PAGE_MASK);
	hc_list_lo = (uint32_t)loader->area_pa;
	hc_list_hi = (uint32_t)(loader->area_pa >> 32);
	for (uint32_t i = 0; i < SLOTS; i++) {
		struct hc_slot *s = &hc_slots[i];
		struct hc_cmd c;

		s->number = i;
		s->utrd = (volatile uint32_t *)(area + 32 * i);
		s->ucd = (volatile uint32_t *)(area + loader->ucd_offset + i * loader->ucd_size);
		s->ucd_pa = loader->area_pa + loader->ucd_offset + i * loader->ucd_size;
		if (i >= 1 && i <= slots) {
			vm_offset_t data = 0;

			if (kmem_alloc_contig(kernel_map, &data, round_page(hc_slot_buffer), PAGE_MASK, 0, 0,
			    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				hc_say("ufs: native: not used: no memory for the slots' buffers\n");
				return false;
			}
			s->data = (uint8_t *)data;
			s->data_pa = kvtophys(data);
			s->data_size = hc_slot_buffer;
			FlushPoC_DcacheRegion(data, hc_slot_buffer);
		}
		hc_make_tur(&c, 0, (uint8_t)i);
		if (hc_place(s, &c, 0) != 0) {
			hc_say("ufs: native: not used: slot %u would not take its TEST UNIT READY\n", i);
			return false;
		}
	}
	hc_nslots = slots;
	hc_free = (uint32_t)(((1ULL << (slots + 1)) - 1) & ~1ULL);       /* slots 1..n */
	hc_usable = true;
	hc_say("ufs: native: usable: a list of %u slots at %llx (the loader's), reads in %u of them, %u KB each (the logical units reads may go to are set by hakios.ufs.c)%s\n",
	    SLOTS, loader->area_pa, slots, hc_slot_buffer >> 10, hc_leave ? "; ufs_native_leave=1: the list is left in "
	    "at the end, for the loader to take back" : "");
	return true;
}

/* ---- own mode ---- */

/* the first UFSHCI on the PCI bus: its registers (BAR0), mapped, memory decoding and bus mastering on */
static uint64_t
hc_pci_find(void)
{
	if (hakios_pci_init() != 0) {
		hc_say("ufs: own: no PCI bus\n");
		return 0;
	}
	for (unsigned i = 0; i < hakios_pci_count(); i++) {
		struct hakios_pci_dev *d = hakios_pci_device(i);
		uint32_t bar0, bar1;
		uint16_t cmd;

		if ((d->class_rev >> 8) != 0x010901 && !(d->vendor == 0x1b36 && d->device == 0x0013)) {
			continue;
		}
		hc_qemu_ufs = (d->vendor == 0x1b36 && d->device == 0x0013);
		bar0 = hakios_pci_cfg_read32(d, 0x10);
		bar1 = hakios_pci_cfg_read32(d, 0x14);
		cmd = hakios_pci_cfg_read16(d, 0x04);
		hc_say("ufs: own: PCI %02x:%02x.%x, %04x:%04x, class %06x: BAR0 %llx, %llu KB (its configuration space, at ECAM "
		    "offset %x: BAR0 %08x BAR1 %08x, command %04x)%s\n", d->bus, d->slot, d->func, d->vendor, d->device,
		    d->class_rev >> 8, d->bar_pa[0], d->bar_size[0] >> 10, (uint32_t)d->bus << 20 | (uint32_t)d->slot << 15 |
		    (uint32_t)d->func << 12, bar0, bar1, cmd, d->firmware ? "; the firmware's: left alone" : "");
		if (d->firmware || d->bar_pa[0] == 0 || (d->bar_flags[0] & HAKIOS_PCI_BAR_IO) || d->bar_size[0] < 0x200) {
			continue;
		}
		hakios_pci_enable(d, false);
		hc = hakios_pci_map_bar(d, 0, 0, d->bar_size[0] < UFSHC_SIZE ? d->bar_size[0] : UFSHC_SIZE);
		return hc != 0 ? d->bar_pa[0] : 0;
	}
	hc_say("ufs: own: no UFS host controller on the PCI bus (class 01/09/01)\n");
	return 0;
}

static bool
hc_poll(uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
	for (uint64_t t = mach_absolute_time(); hc_us_since(t) < ms * 1000ULL;) {
		if ((hc_read(reg) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return (hc_read(reg) & mask) == want;
}

/*
 * A local UniPro/M-PHY attribute, read with DME_GET (UFSHCI 5.6.4): the only UIC command this
 * driver sends to a controller it did not enable itself, and it reads. 0: read, the value in *v.
 */
static int
hc_dme_get(uint16_t attr, uint16_t sel, uint32_t *v)
{
	uint32_t result;

	if ((hc_read(UFSHC_HCS) & HCS_UCRDY) == 0) {
		return EBUSY;
	}
	hc_write(UFSHC_IS, IS_UCCS);
	hc_write(UFSHC_UICCMDARG1, (uint32_t)attr << 16 | sel);
	hc_write(UFSHC_UICCMDARG2, 0);
	hc_write(UFSHC_UICCMDARG3, 0);
	hc_write(UFSHC_UICCMD, UIC_DME_GET);
	if (!hc_poll(UFSHC_IS, IS_UCCS, IS_UCCS, 100)) {
		return ETIMEDOUT;
	}
	result = hc_read(UFSHC_UICCMDARG2) & 0xff;
	*v = hc_read(UFSHC_UICCMDARG3);
	hc_write(UFSHC_IS, IS_UCCS);
	return result == 0 ? 0 : EIO;
}

/*
 * Step 2: the link as UEFI left it, from the host's own attributes (PA layer, UniPro 1.6): the
 * lanes there are, connected and active, the gears, the power mode and the HS series. The S9's
 * device tree asks for two lanes at HS-G3 (ufs,pmd-attr-lane, ufs,pmd-attr-gear) and Silicium's
 * UFS driver sets FAST mode, rate B (UfsDxe.h UFS_POWER_MODE, UFS_RATE). Only a link that is up
 * and has a lane each way is taken on; the rest is said.
 */
static const char *
hc_link_look(void)
{
	static const struct {
		uint16_t attr;
		const char *name;
	} pa[] = {
		{ 0x1520, "PA_AvailTxDataLanes" }, { 0x1540, "PA_AvailRxDataLanes" }, { 0x1561, "PA_ConnectedTxDataLanes" },
		{ 0x1581, "PA_ConnectedRxDataLanes" }, { 0x1560, "PA_ActiveTxDataLanes" }, { 0x1580, "PA_ActiveRxDataLanes" },
		{ 0x1568, "PA_TxGear" }, { 0x1583, "PA_RxGear" }, { 0x1571, "PA_PWRMode" }, { 0x156a, "PA_HSSeries" },
		{ 0x1587, "PA_MaxRxHSGear" },
	};
	uint32_t v[sizeof(pa) / sizeof(pa[0])] = { 0 }, hcs = hc_read(UFSHC_HCS), failed = 0;

	for (unsigned i = 0; i < sizeof(pa) / sizeof(pa[0]); i++) {
		int e = hc_dme_get(pa[i].attr, 0, &v[i]);

		if (e != 0) {
			failed |= 1u << i;
		}
		hc_say("ufs: own: link: %-24s %04x: %s%u\n", pa[i].name, pa[i].attr, e == 0 ? "" : "NOT READ, ", e == 0 ? v[i] :
		    (uint32_t)e);
	}
	hc_say("ufs: own: link: HCS %08x (device %s, UIC %s); %u lanes each way connected (%u, %u), gear %u/%u, power mode "
	    "%02x (%s), series %s\n", hcs, (hcs & HCS_DP) ? "present" : "ABSENT", (hcs & HCS_UCRDY) ? "ready" : "NOT READY",
	    v[2] < v[3] ? v[2] : v[3], v[2], v[3], v[6], v[7], v[8], v[8] == 0x11 ? "FAST both ways" : v[8] == 0x22 ?
	    "SLOW both ways" : v[8] == 0x44 ? "FASTAUTO" : v[8] == 0x55 ? "SLOWAUTO" : "other", v[9] == 2 ? "B" : v[9] == 1 ?
	    "A" : "?");
	if ((hcs & HCS_DP) == 0) {
		return "no device present";
	}
	if (failed & (3u << 2)) {
		return hc_soc->may_enable ? NULL : "the connected lanes could not be read (DME_GET)";
	}
	if (v[2] == 0 || v[3] == 0) {
		return "no lane connected one way or the other: the link is down";
	}
	return NULL;
}

/* QEMU's alone: the controller enabled, the link started, a device present */
static const char *
hc_enable_from_reset(void)
{
	hc_write(UFSHC_HCE, 1);
	if (!hc_poll(UFSHC_HCE, 1, 1, 100)) {
		return "HCE did not read 1";
	}
	if (!hc_poll(UFSHC_HCS, HCS_UCRDY, HCS_UCRDY, 100)) {
		return "not ready for a UIC command";
	}
	for (int tries = 1; tries <= 3; tries++) {
		uint32_t result;

		hc_write(UFSHC_IS, hc_read(UFSHC_IS));
		hc_write(UFSHC_UICCMDARG1, 0);
		hc_write(UFSHC_UICCMDARG2, 0);
		hc_write(UFSHC_UICCMDARG3, 0);
		hc_write(UFSHC_UICCMD, UIC_LINK_STARTUP);
		if (!hc_poll(UFSHC_IS, IS_UCCS, IS_UCCS, 500)) {
			hc_say("ufs: own: DME_LINKSTARTUP %d: no completion\n", tries);
			continue;
		}
		result = hc_read(UFSHC_UICCMDARG2) & 0xff;
		hc_write(UFSHC_IS, IS_UCCS);
		hc_say("ufs: own: DME_LINKSTARTUP %d: result %u, HCS %08x\n", tries, result, hc_read(UFSHC_HCS));
		if (result == 0 && hc_poll(UFSHC_HCS, HCS_DP, HCS_DP, 500)) {
			return NULL;
		}
	}
	return "the link did not start, or no device came up";
}

/*
 * UEFI's own descriptors, read: each slot of its list that holds one, its UPIU, and for a read
 * whether its PRD table is laid out the way the SoC's facts say this driver must lay out its own
 * (offsets in bytes or dwords; entries of prd_entry bytes whose lengths add up to the UPIU's).
 * 1: a read of UEFI's agrees; 0: none to tell by; -1: one disagrees.
 */
static int
hc_look_uefi(uint64_t list_pa, uint32_t nutrs)
{
	const struct ufshc_soc *soc = hc_soc;
	vm_offset_t list;
	int verdict = 0, shown = 0;

	if (list_pa == 0 || (list_pa & 0x3ff) || pmap_valid_address(list_pa) ||
	    (soc->dma_hi != 0 && (list_pa < soc->dma_lo || list_pa + 32 * nutrs > soc->dma_hi))) {
		hc_say("ufs: own: UEFI's list %llx: none, or not where it can be read here\n", list_pa);
		return 0;
	}
	list = ml_io_map_unmappable((vm_offset_t)(list_pa & ~(uint64_t)PAGE_MASK), PAGE_SIZE, VM_WIMG_WCOMB) +
	    (vm_offset_t)(list_pa & PAGE_MASK);
	for (uint32_t i = 0; i < nutrs && shown < 6; i++) {
		volatile const uint32_t *d = (volatile const uint32_t *)(list + 32 * i);
		uint64_t ucd = (uint64_t)d[5] << 32 | d[4];
		uint32_t prd_off, prd_len, rsp_off, entries, stride, sum = 0, expected, used;
		volatile const uint8_t *u;
		vm_offset_t map;

		if (d[0] == 0 && ucd == 0) {
			continue;
		}
		shown++;
		hc_say("ufs: own: UEFI's slot %u: %08x %08x %08x %08x %08x %08x %08x %08x\n", i, d[0], d[1], d[2], d[3], d[4],
		    d[5], d[6], d[7]);
		/*
		 * Only a descriptor of UEFI's own is followed: a UFS storage command, its command descriptor
		 * where the SoC's DMA reaches (the S9's: DRAM below 4 GB). UEFI may use fewer slots than the
		 * controller has (Silicium's UfsDxe: two), and what lies in the rest of its page is not
		 * a descriptor: an address there must not be mapped and read.
		 */
		/*
		 * The command type: UFS storage (1) as UFSHCI 2.1 has it, or 0 -- Silicium's UfsDxe leaves the
		 * field 0 (its slot 0 on the S9: 05000000, interrupt and device-to-host only), and the Exynos
		 * controller runs it. The UPIU's own transaction type is checked below either way.
		 */
		if (ucd == 0 || (d[0] & UTRD_CT_MASK) == UTRD_CT_MASK) {
			continue;
		}
		if ((ucd & 0x7f) || pmap_valid_address(ucd) || ((d[0] & UTRD_CT_MASK) != UTRD_CT_UFS &&
		    (d[0] & UTRD_CT_MASK) != 0) || (soc->dma_hi != 0 && (ucd < soc->dma_lo || ucd + 2 * PAGE_SIZE > soc->dma_hi))) {
			hc_say("ufs: own: UEFI's slot %u: not followed (%s)\n", i, (ucd & 0x7f) ? "its command descriptor is "
			    "not aligned" : pmap_valid_address(ucd) ? "its command descriptor is memory this kernel manages" :
			    ((d[0] & UTRD_CT_MASK) != UTRD_CT_UFS && (d[0] & UTRD_CT_MASK) != 0) ? "not a UFS storage command" :
			    "its command descriptor is not where the SoC's DMA reaches");
			continue;
		}
		map = ml_io_map_unmappable((vm_offset_t)(ucd & ~(uint64_t)PAGE_MASK), 2 * PAGE_SIZE, VM_WIMG_WCOMB);
		u = (volatile const uint8_t *)(map + (ucd & PAGE_MASK));
		expected = be32(u + 12);
		hc_say("ufs: own: UEFI's slot %u: UPIU %02x %02x %02x %02x, expects %u bytes, CDB %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x\n", i, u[0], u[1], u[2], u[3], expected, u[16], u[17], u[18], u[19], u[20], u[21],
		    u[22], u[23], u[24], u[25]);
		if ((d[2] & 0xff) != OCS_SUCCESS || (d[0] & UTRD_DD_MASK) != UTRD_DD_IN || (u[0] & 0x3f) != UPIU_COMMAND ||
		    expected == 0) {
			hc_say("ufs: own: UEFI's slot %u: not a completed read (OCS %x, direction %u, UPIU %02x, %u bytes)\n", i,
			    d[2] & 0xff, (d[0] & UTRD_DD_MASK) >> 25, u[0], expected);
			continue;
		}
		rsp_off = d[6] >> 16;
		prd_off = d[7] >> 16;
		prd_len = d[7] & 0xffff;
		/*
		 * In bytes (Exynos), the entry's size is the protector's descriptor type, which an SMC sets and
		 * this kernel never does: 128 bytes on type 3 (as S-Boot leaves it and the S9's kernel sets it),
		 * 16 on type 0. Whichever makes UEFI's own entries add up to its UPIU's length is the one.
		 */
		/*
		 * 16-byte entries first (what UEFI's reads on the S9 have shown), then the protector's 128. An entry
		 * all zero is one UEFI left unused (ro6: a table of 256 bytes, one entry used, fifteen zero) and is
		 * not counted. A single-entry read cannot tell 16 from 128 apart; the layout read (two entries) does.
		 */
		for (unsigned c = 0; c < (soc->prdt_bytes ? 2u : 1u); c++) {
			stride = soc->prdt_bytes ? (c == 0 ? 16 : soc->prd_entry) : 16;
			entries = soc->prdt_bytes ? (prd_len % stride == 0 ? prd_len / stride : 0) : prd_len;
			sum = 0;
			used = 0;
			for (uint32_t k = 0; k < entries; k++) {
				uint32_t at = (soc->prdt_bytes ? prd_off : prd_off * 4) + k * stride, w[4];

				if (at + 16 > PAGE_SIZE + PAGE_SIZE - (ucd & PAGE_MASK)) {
					break;
				}
				for (int b = 0; b < 4; b++) {
					w[b] = (uint32_t)u[at + 4 * b] | (uint32_t)u[at + 4 * b + 1] << 8 |
					    (uint32_t)u[at + 4 * b + 2] << 16 | (uint32_t)u[at + 4 * b + 3] << 24;
				}
				if (w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 0) {
					continue;               /* (unused) */
				}
				used++;
				sum += (w[3] & (soc->prdt_bytes ? 0x3ffffff : 0x3ffff)) + 1;
			}
			entries = used;
			if (entries != 0 && sum == expected) {
				break;
			}
		}
		hc_say("ufs: own: UEFI's slot %u: response at %u, PRDT at %u, %u (%s): %u entries of %u bytes describe %u bytes "
		    "of the %u expected: %s\n", i, rsp_off, prd_off, prd_len, soc->prdt_bytes ? "bytes" : "entries", entries,
		    stride, sum, expected, entries != 0 && sum == expected ? "a layout this driver can make" : "NOT a layout "
		    "this driver knows");
		if (entries != 0 && sum == expected && soc->prdt_bytes) {
			if (verdict == 1 && hc_soc_own.prd_entry != stride) {
				verdict = -1;           /* (two reads of UEFI's that disagree) */
				continue;
			}
			hc_soc_own.prd_entry = stride;
		}
		verdict = entries != 0 && sum == expected ? (verdict < 0 ? -1 : 1) : -1;
	}
	return verdict;
}

/* this kernel's memory the controller reaches: physically contiguous, below 4 GB if it must be */
static vm_offset_t
hc_dma_alloc(vm_size_t size, uint64_t *pa, bool dma32)
{
	vm_offset_t va = 0;
	ppnum_t max = dma32 ? (ppnum_t)((4ULL << 30) >> PAGE_SHIFT) - 1 : 0;

	size = round_page(size);
	if (hc_soc->carve_size != 0) {
		/* the SoC's memory outside XNU's RAM (the S9's: see UFSHC_S9_MEM_PA), handed out in order, never given back */
		static vm_offset_t carve_va;
		static uint64_t carve_used;
		uint64_t lo = hc_soc->carve_pa, n = hc_soc->carve_size;

		if (carve_va == 0) {
			if (pmap_valid_address(lo) || pmap_valid_address(lo + n - 1) || (dma32 && lo + n > (4ULL << 30))) {
				hc_say("ufs: own: memory %llx-%llx: XNU's RAM, or not below 4 GB: not used\n", lo, lo + n - 1);
				return 0;
			}
			carve_va = hakios_physmap_wc(lo, n);
			if (carve_va == 0) {
				hc_say("ufs: own: memory %llx-%llx: could not be mapped\n", lo, lo + n - 1);
				return 0;
			}
			hc_say("ufs: own: memory: %llu KB at %llx, physical memory outside XNU's RAM, mapped write-combined\n",
			    n >> 10, lo);
		}
		if (carve_used + size > n) {
			return 0;
		}
		va = carve_va + (vm_offset_t)carve_used;
		*pa = lo + carve_used;
		carve_used += size;
		bzero((void *)va, size);
		__asm__ volatile ("dsb sy" ::: "memory");
		return va;
	}
	if (kmem_alloc_contig(kernel_map, &va, size, PAGE_MASK, max, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return 0;
	}
	*pa = kvtophys(va);
	if (dma32 && *pa + size > (4ULL << 30)) {
		kmem_free(kernel_map, va, size);
		return 0;
	}
	FlushPoC_DcacheRegion(va, size);
	return va;
}

/*
 * The general logical units: each one's descriptor, then TEST UNIT READY (a unit attention or
 * not-ready answered by trying again) and READ CAPACITY(16), whose answer is what is used.
 */
static void
hc_find_units(void)
{
	static const uint8_t rc16[16] = { 0x9e, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0 };
	static const uint8_t tur[6] = { 0 };

	for (uint8_t lun = 0; lun < UFSHC_LUNS; lun++) {
		struct ufshc_unit *u = &hc_units[lun];
		uint8_t desc[0x23], cap[32];
		uint32_t len = sizeof(desc);
		bool described;
		char what[32];
		int e = EIO;

		hc_quiet = true;
		described = hc_query(QUERY_READ, QOP_READ_DESC, QDESC_UNIT, lun, NULL, desc, &len) == 0 && len >= 0x13;
		hc_quiet = false;
		bzero(u, sizeof(*u));
		if (described) {
			hc_say("ufs: own: LUN %u: descriptor: enabled %u, boot LU %u, write protect %u, block size 2^%u, %llu "
			    "blocks\n", lun, desc[3], desc[4], desc[5], desc[0x0a], be64(desc + 0x0b));
			if (desc[3] == 0) {
				continue;
			}
			u->boot_lun_id = desc[4];
			u->write_protect = desc[5];
		}
		snprintf(what, sizeof(what), "LUN %u's TEST UNIT READY", lun);
		for (int tries = 0; tries < 20 && e != 0 && !hc_dead; tries++) {
			e = hc_scsi0(lun, tur, sizeof(tur), 0, NULL, what, true);
			/*
			 * A unit attention or a unit not ready yet: again. For a unit its descriptor says is enabled,
			 * an error OCS too -- the Exynos 9810's controller can report a device's error as OCS 7
			 * (the mainline port sets UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR for it).
			 */
			if (e == EAGAIN || (e == EIO && described)) {
				IOSleep(10);
			} else if (e != 0) {
				break;
			}
		}
		if (e != 0) {
			hc_say("ufs: own: LUN %u: %s (%d)\n", lun, described ? "enabled, but not ready" : "none", e);
			continue;
		}
		snprintf(what, sizeof(what), "LUN %u's READ CAPACITY(16)", lun);
		if (hc_scsi0(lun, rc16, sizeof(rc16), sizeof(cap), cap, what, false) != 0) {
			continue;
		}
		u->blocks = be64(cap) + 1;
		u->block_size = be32(cap + 8);
		u->present = true;
		hc_say("ufs: own: LUN %u: %llu blocks of %u (%llu MB)%s%s%s\n", lun, u->blocks, u->block_size,
		    u->blocks * u->block_size >> 20, u->boot_lun_id ? ", a boot unit" : "",
		    u->write_protect ? ", write protected" : "", u->block_size != BLOCK ? ": not 4096-byte blocks, NOT USED" : "");
	}
}

/*
 * Steps 3 and 4: as far as was asked, and no further. Nothing more is sent, and the controller is
 * given back as it was found (UEFI's list, its interrupt enable, the vendor's nexus types; on
 * QEMU, disabled again if it was found so), as at the end of a run.
 */
static bool
hc_stop_here(uint32_t step, const char *next)
{
	struct hc_put_back_result r;

	for (uint64_t t = mach_absolute_time(); hc_read(UFSHC_UTRLDBR) != 0 && hc_us_since(t) < 1000000;) {
		__builtin_arm_yield();
	}
	if (hc_enabled_here) {
		hc_write(UFSHC_UTRLRSR, 0);
		hc_write(UFSHC_IE, hc_uefi_ie);
		hc_write(UFSHC_HCE, 0);
		hc_say("ufs: own: the controller disabled again, as it was found (HCE %08x)\n", hc_read(UFSHC_HCE));
	} else {
		hc_put_back(&r);
		hc_put_back_print("stopping", &r, false);
	}
	hc_in = false;
	hc_usable = false;
	hc_mode = HC_NONE;
	hc_stopped_at = step;
	hc_report("stopped", false);
	hc_off();
	hc_say("ufs: own: step %u done, as asked: this kernel's own driver is not used further in this run; %s\n", step, next);
	return false;
}

bool
ufshc_own_stopped(void)
{
	return hc_stopped_at != 0;
}

/*
 * Own mode: find the controller, take it (from UEFI, or on QEMU from reset), put this kernel's
 * list in, wake the device and find its units. ufs_native=5: look, say, change nothing.
 */
bool
ufshc_own_probe(void)
{
	uint32_t mode = 4;
	char name[32] = "pci";

	PE_parse_boot_argn("ufs_native", &mode, sizeof(mode));
	/* ufs_native_soc=NAME: the controller own mode takes: pci (QEMU's, the default), sm7325, exynos9810, exynos9610 */
	if (!PE_parse_boot_arg_str("ufs_native_soc", name, sizeof(name))) {
		uint32_t acpm_soc = 0;

		/* the A50: acpm_soc=9610 says the SoC (its boot-args are near the command line's limit) */
		if (PE_parse_boot_argn("acpm_soc", &acpm_soc, sizeof(acpm_soc)) && acpm_soc == 9610) {
			strlcpy(name, "exynos9610", sizeof(name));
		}
	}
	return hc_own_probe(name, mode, 0);
}

/* own mode not taken: said, and kept for ufs=3's summary (the first reason) */
static void
hc_not_used(const char *why)
{
	hc_say("ufs: own: not used: %s\n", why);
	if (hc_take_why[0] == 0) {
		strlcpy(hc_take_why, why, sizeof(hc_take_why));
	}
}

const char *
ufshc_take_why(void)
{
	return hc_take_why[0] != 0 ? hc_take_why : NULL;
}

/*
 * The layout read (SoCs whose layout is taken from their kernel's source, the S9's): ONE READ(10)
 * of blocks 0 and 1 of LUN 0 through this kernel's own descriptor -- two blocks, so two PRDT
 * entries of 4 KB, the second where the entry size says -- into slot 0's page, filled with a
 * pattern first; then byte for byte against the loader's reads of the same blocks (ufs=3's
 * step 1). TEST UNIT READY before it (a unit attention is answered by trying again). Only a
 * read that comes back GOOD and the same lets own mode go on; anything else is a refusal and
 * the caller gives the controller back. Nothing else carrying data is sent before it.
 */
static const char *
hc_layout_read(void)
{
	static const uint8_t tur[6] = { 0 };
	static const uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };    /* LBA 0, 2 blocks */
	static uint8_t got[2 * BLOCK];
	struct hc_slot *s = &hc_slots[SLOT_CONTROL];
	uint32_t saved = hc_pol.read_luns, first = 0, diff = 0;
	int e = EIO;

	if ((hc_layout_ref == NULL || hc_layout_ref_blocks < 2) && hc_layout_ref_fnv == 0) {
		return "no reference read of the loader's to check this driver's layout against";
	}
	if (s->data_size < sizeof(got)) {
		return "slot 0's page is too small for the layout read";
	}
	hc_pol.read_luns = 1;                   /* LUN 0 only, for this one read */
	for (int tries = 0; tries < 20 && e != 0 && !hc_dead; tries++) {
		e = hc_scsi0(0, tur, sizeof(tur), 0, NULL, "LUN 0's TEST UNIT READY (layout read)", true);
		if (e == EAGAIN || e == EIO) {
			IOSleep(10);
		} else if (e != 0) {
			break;
		}
	}
	if (e != 0) {
		hc_pol.read_luns = saved;
		return "LUN 0 does not answer TEST UNIT READY: no layout read";
	}
	memset(s->data, 0xa5, sizeof(got));     /* (what a read that wrote nothing would leave: never the reference) */
	memset(got, 0x5a, sizeof(got));
	e = hc_scsi0(0, read10, sizeof(read10), sizeof(got), got, "the layout read (READ(10), LUN 0, LBA 0-1)", false);
	hc_pol.read_luns = saved;
	if (e != 0) {
		hc_say("ufs: own: layout read: READ(10) of LUN 0's blocks 0-1 failed (%d; OCS %x, doorbell %08x, IS %08x)\n", e,
		    hc_slots[SLOT_CONTROL].utrd[2] & 0xff, hc_read(UFSHC_UTRLDBR), hc_read(UFSHC_IS));
		return "the layout read (READ(10) of LUN 0, LBA 0-1, this driver's descriptor) failed";
	}
	if (hc_layout_ref != NULL && hc_layout_ref_blocks >= 2) {
		for (uint32_t i = 0; i < sizeof(got); i++) {
			if (got[i] != hc_layout_ref[i]) {
				if (diff++ == 0) {
					first = i;
				}
			}
		}
	} else {
		uint64_t h = 0xcbf29ce484222325ULL;

		for (uint32_t i = 0; i < sizeof(got); i++) {
			h = (h ^ got[i]) * 0x100000001b3ULL;
		}
		hc_say("ufs: own: layout read: FNV-1a %016llx, the host's ufs_native_ref_fnv= %016llx\n", h, hc_layout_ref_fnv);
		diff = h == hc_layout_ref_fnv ? 0 : 1;
	}
	hc_say("ufs: own: layout read: LUN 0's blocks 0-1 through this driver's descriptor (PRDT entries of %u bytes, "
	    "offsets in %s): %s the loader's (%u of %u bytes differ%s%u; here %02x %02x %02x %02x ... %02x %02x at 510)\n",
	    hc_soc->prd_entry, hc_soc->prdt_bytes ? "bytes" : "dwords", diff == 0 ? "THE SAME as" : "NOT THE SAME as", diff,
	    (uint32_t)sizeof(got), diff ? ", the first at " : "", diff ? first : 0, got[0], got[1], got[2], got[3], got[510],
	    got[511]);
	return diff == 0 ? NULL : "the layout read (LUN 0, LBA 0-1) through this driver's descriptor is not the loader's";
}

/* step: 0, the controller's default unless ufs_native_step= says; 1-5, that unless ufs_native_step= says */
static bool
hc_own_probe(const char *name, uint32_t mode, uint32_t step)
{
	uint32_t cap, hcs, hce, is, ie, base, baseu, db, rsr, tm_db, slots = 15, nfree = 0;
	const char *no = NULL;
	uint64_t pa, list_pa, uefi_pa;
	vm_offset_t list;
	bool dma32;

	if (!hc_whitelist_selftest()) {
		return false;
	}
	hc_soc = NULL;
	for (unsigned i = 0; i < sizeof(hc_socs) / sizeof(hc_socs[0]); i++) {
		if (strcmp(name, hc_socs[i].name) == 0) {
			hc_soc_own = hc_socs[i];
			hc_soc = &hc_soc_own;
		}
	}
	if (hc_soc == NULL) {
		hc_soc = SOC_SHARED;
		hc_say("ufs: own: ufs_native_soc=%s: no such controller (pci, sm7325, exynos9810, exynos9610)\n", name);
		return false;
	}
	hc_slot_buffer = hc_soc->slot_buffer;
	hc_boot_args();
	/* ufs_native_step=N: own mode goes as far as step N: 1 look, 2 the link, 3 NOP OUT, 4 READ CAPACITY, 5 all (the GPTs) */
	hc_step = step != 0 ? step : hc_soc->default_step;
	PE_parse_boot_argn("ufs_native_step", &hc_step, sizeof(hc_step));
	if (mode == 5 || hc_step < 1) {
		hc_step = 1;
	} else if (hc_step > 5) {
		hc_step = 5;
	}
	hc_say("ufs: own: %s: step %u of 5 (1 look, 2 the link, 3 NOP OUT, 4 READ CAPACITY, 5 the GPTs and the devices)%s\n",
	    hc_soc->name, hc_step, hc_step == hc_soc->default_step ? ", this controller's default" : "");
	if (hc_slot_buffer > hc_soc->prd_max * 64) {
		hc_slot_buffer = hc_soc->prd_max * 64;
	}
	if (hc_soc->pa == 0) {
		pa = hc_pci_find();
	} else {
		pa = hc_soc->pa;
		hc = ml_io_map(hc_soc->pa, hc_soc->size);
		if (hc_soc->vs_pa != 0) {
			hc_vs = ml_io_map(hc_soc->vs_pa, hc_soc->vs_size);
		}
	}
	if (pa == 0 || hc == 0) {
		return false;
	}
	cap = hc_read(UFSHC_CAP);
	hc_ver = hc_read(UFSHC_VER);
	hcs = hc_read(UFSHC_HCS);
	hce = hc_read(UFSHC_HCE);
	is = hc_read(UFSHC_IS);
	ie = hc_read(UFSHC_IE);
	base = hc_read(UFSHC_UTRLBA);
	baseu = hc_read(UFSHC_UTRLBAU);
	db = hc_read(UFSHC_UTRLDBR);
	rsr = hc_read(UFSHC_UTRLRSR);
	tm_db = hc_read(UFSHC_UTMRLDBR);
	hc_nutrs = CAP_NUTRS(cap);
	dma32 = hc_soc->dma32 || (cap & CAP_64AS) == 0;
	hc_say("ufs: own: %s controller at %llx: CAP %08x (%u slots, 64-bit %s), VER %08x, HCE %08x, HCS %08x, IS %08x, "
	    "IE %08x; transfer list %08x:%08x, doorbell %08x, run %08x; task doorbell %08x\n", hc_soc->name, pa, cap, hc_nutrs,
	    (cap & CAP_64AS) ? "yes" : "no", hc_ver, hce, hcs, is, ie, baseu, base, db, rsr, tm_db);
	uefi_pa = (uint64_t)baseu << 32 | base;

	if (hc_ver < 0x200 || hc_ver >= 0x500) {
		no = "a controller version other than 2.x, 3.x or 4.x";
	} else if (hc_nutrs < 4) {
		no = "fewer than 4 slots";
	} else if (hce & 1) {
		/* as UEFI left it (ShengUfs's checks, the task list's doorbell among them) */
		if ((hcs & (HCS_DP | HCS_UTRLRDY)) != (HCS_DP | HCS_UTRLRDY)) {
			no = "enabled, but no device present or the transfer list not ready";
		} else if (db != 0 || tm_db != 0) {
			no = "a request of UEFI's in flight";
		} else if (is & IS_FATAL) {
			no = "a fatal status";
		} else if (hc_soc->look != NULL && (no = hc_soc->look(false)) != NULL) {
			/* (said) */
		} else if (hc_soc->adopt != NULL) {
			no = hc_soc->adopt();
		}
		if (no == NULL) {
			int verdict = hc_look_uefi(uefi_pa, hc_nutrs);

			if (hc_soc->prdt_bytes && verdict < 0) {
				/*
				 * UEFI's leftovers do not decide (ro6: a read of its whose table this file misread): said, and
				 * the layout read -- this driver's own READ of LUN 0's blocks 0-1 against the loader's -- does.
				 */
				hc_soc_own.prd_entry = hc_prd_arg != 0 ? hc_prd_arg : 16;
				if (hc_layout_ref == NULL && hc_layout_ref_fnv == 0 && hc_step >= 4) {
					no = "UEFI's own read is not laid out as this driver would lay out its own, and no reference to "
					    "read against";
				} else {
					hc_say("ufs: own: UEFI's leftover read is not laid out as this driver reads it (above): not "
					    "decisive; PRDT entries of %u bytes, to be proven by the layout read before anything else\n",
					    hc_soc_own.prd_entry);
				}
			} else if (hc_soc->prdt_bytes && verdict == 0) {
				/*
				 * No read of UEFI's left to tell the entry size by (UEFI reuses its slots): 16 bytes,
				 * the one that is harmless if wrong -- two 16-byte entries (32 bytes) are no whole
				 * 128-byte entry, so a controller on 128 has no entry to write into and the layout
				 * read fails; one on 16 reads. The layout read before anything else decides.
				 */
				hc_soc_own.prd_entry = hc_prd_arg != 0 ? hc_prd_arg : 16;
				if (hc_layout_ref == NULL && hc_layout_ref_fnv == 0 && hc_step >= 4) {
					no = "no read of UEFI's to check this driver's layout against, and no reference to read against";
				} else {
					hc_say("ufs: own: no read of UEFI's to check this driver's layout against: PRDT entries of %u "
					    "bytes, to be proven by the layout read before anything else is read\n", hc_soc_own.prd_entry);
				}
			} else if (hc_soc->prdt_bytes) {
				hc_say("ufs: own: UEFI's own read agrees with this driver's layout (PRDT entries of %u bytes): the layout "
				    "read checks it once more\n", hc_soc->prd_entry);
			}
		}
	} else if (!hc_soc->may_enable) {
		no = "not enabled, and on this SoC its PHY, clocks and power are UEFI's to set up, not this kernel's";
	}
	if (no != NULL) {
		hc_not_used(no);
		return false;
	}
	if (hc_step == 1) {
		hc_stopped_at = 1;
		hc_say("ufs: own: step 1 done: looked, nothing changed%s; ufs_native_step=2 looks at the link\n", (hce & 1) ? "" :
		    " (it would be enabled here)");
		return false;
	}
	if (hc_step == 2) {
		hc_stopped_at = 2;
		if ((hce & 1) == 0) {
			hc_say("ufs: own: step 2 done: not enabled, so no link to look at, nothing changed; ufs_native_step=3 enables it "
			    "here, starts the link and sends NOP OUT\n");
			return false;
		}
		no = hc_link_look();
		if (no == NULL && hc_soc->look != NULL) {
			no = hc_soc->look(true);
		}
		hc_say("ufs: own: step 2 done: %s%s; nothing changed but DME_GET's own registers (UICCMD, its arguments, IS's UCCS)%s\n",
		    no == NULL ? "the link is up" : "THE LINK IS NOT USABLE: ", no == NULL ? "" : no,
		    no == NULL ? "; ufs_native_step=3 puts this kernel's list in and sends NOP OUT" : "");
		return false;
	}
	if ((hce & 1) == 0) {
		hc_enabled_here = true;
		hc_say("ufs: own: the controller is not enabled (UEFI has no driver for it): enabling it here\n");
		no = hc_enable_from_reset();
		if (no != NULL) {
			hc_not_used(no);
			hc_write(UFSHC_HCE, 0);
			return false;
		}
		uefi_pa = 0;
		rsr = 0;
	}
	hc_loader.uefi_lo = (uint32_t)uefi_pa;
	hc_loader.uefi_hi = (uint32_t)(uefi_pa >> 32);
	hc_uefi_rsr = rsr;
	hc_uefi_ie = hc_read(UFSHC_IE);

	/* the list (a page), the command descriptors, a page for slot 0's data; the slots' buffers */
	hc_ucd_size = (hc_soc->prdt_offset + hc_soc->prd_entry * (hc_slot_buffer / hc_soc->prd_max + 1) + 127) & ~127u;
	list = hc_dma_alloc(PAGE_SIZE + SLOTS * hc_ucd_size + PAGE_SIZE, &list_pa, dma32);
	if (list == 0) {
		hc_not_used(dma32 ? "no memory for the list below 4 GB" : "no memory for the list");
		return false;
	}
	if (hc_soc->default_slots != 0) {
		slots = hc_soc->default_slots;
	}
	PE_parse_boot_argn("ufs_native_slots", &slots, sizeof(slots));
	if (slots < 1 || slots > 15) {
		slots = 15;
	}
	hc_mode = HC_OWN;
	hc_list_lo = (uint32_t)list_pa;
	hc_list_hi = (uint32_t)(list_pa >> 32);
	hc_free = 0;
	for (uint32_t i = 0; i < SLOTS; i++) {
		struct hc_slot *s = &hc_slots[i];
		vm_offset_t ucd = list + PAGE_SIZE + i * hc_ucd_size;

		s->number = i;
		s->utrd = (volatile uint32_t *)(list + 32 * i);
		s->ucd = (volatile uint32_t *)ucd;
		s->ucd_pa = list_pa + (ucd - list);
		if (i == SLOT_CONTROL) {
			s->data = (uint8_t *)(list + PAGE_SIZE + SLOTS * hc_ucd_size);
			s->data_pa = list_pa + PAGE_SIZE + SLOTS * hc_ucd_size;
			s->data_size = PAGE_SIZE;
		} else if (i % 2 == 0 && i < hc_nutrs && nfree < slots) {
			vm_offset_t data = hc_dma_alloc(hc_slot_buffer, &s->data_pa, dma32);

			if (data == 0) {
				hc_say("ufs: own: no memory for slot %u's buffer: %u slots it is\n", i, nfree);
				break;
			}
			s->data = (uint8_t *)data;
			s->data_size = hc_slot_buffer;
			hc_free |= 1u << i;
			nfree++;
		}
	}
	hc_nslots = nfree;
	if (nfree == 0) {
		hc_not_used("no slots");
		hc_mode = HC_NONE;
		return false;
	}
	FlushPoC_DcacheRegion(list, PAGE_SIZE + SLOTS * hc_ucd_size + PAGE_SIZE);

	/* UEFI's list out, this kernel's in (interrupts masked: completions are polled) */
	hc_is_in = hc_read(UFSHC_IS);
	hc_cnr_in = hc_read(UFSHC_UTRLCNR);
	hc_iacr_in = hc_read(UFSHC_UTRIACR);
	if (hc_own_list_in != NULL) {
		/* (ufs=3) the loader's page says so before the controller does: the loader asks UEFI for nothing now */
		*hc_own_list_in = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	hc_write(UFSHC_IE, 0);
	if (rsr & 1) {
		hc_write(UFSHC_UTRLRSR, 0);
		if (!hc_poll(UFSHC_UTRLRSR, 1, 0, 100)) {
			hc_write(UFSHC_IE, hc_uefi_ie);
			hc_write(UFSHC_UTRLRSR, 1);
			strlcpy(hc_take_why, "UEFI's list did not stop", sizeof(hc_take_why));
			hc_say("ufs: own: not used: UEFI's list did not stop (run %08x, list %08x:%08x now)\n",
			    hc_read(UFSHC_UTRLRSR), hc_read(UFSHC_UTRLBAU), hc_read(UFSHC_UTRLBA));
			if (hc_own_list_in != NULL && hc_read(UFSHC_UTRLBA) == (uint32_t)uefi_pa &&
			    hc_read(UFSHC_UTRLBAU) == (uint32_t)(uefi_pa >> 32) && (hc_read(UFSHC_UTRLRSR) & 1)) {
				*hc_own_list_in = 0;    /* never left UEFI's: the loader may go on */
			}
			hc_mode = HC_NONE;
			return false;
		}
	}
	hc_write(UFSHC_UTRLBA, hc_list_lo);
	hc_write(UFSHC_UTRLBAU, hc_list_hi);
	if (hc_read(UFSHC_UTRLBA) != hc_list_lo || hc_read(UFSHC_UTRLBAU) != hc_list_hi) {
		no = "the controller did not take this list's address";
	} else {
		hc_write(UFSHC_IS, hc_read(UFSHC_IS) & ~IS_FATAL);      /* (fatal bits, were there any, would stay as evidence) */
		hc_write(UFSHC_UTRLRSR, 1);
		if (!hc_poll(UFSHC_UTRLRSR, 1, 1, 100) || !hc_poll(UFSHC_HCS, HCS_UTRLRDY, HCS_UTRLRDY, 100)) {
			no = "this list did not start";
		}
	}
	if (no != NULL) {
		struct hc_put_back_result r;

		hc_not_used(no);
		hc_put_back(&r);
		hc_put_back_print("undoing", &r, false);
		hc_mode = HC_NONE;
		return false;
	}
	hc_in = true;
	hc_usable = true;
	hc_switch_in_count++;
	hc_say("ufs: own: list %08x:%08x in (UEFI's %08x:%08x %s), IE %08x masked\n", hc_list_hi, hc_list_lo,
	    hc_loader.uefi_hi, hc_loader.uefi_lo, hc_enabled_here ? "none: enabled here" : "kept, to be put back", hc_uefi_ie);
	hc_say("ufs: own: %u slots of %u KB, command descriptors of %u bytes, PRDT entries of %u bytes and %u KB at most, "
	    "offsets in %s%s\n", nfree, hc_slot_buffer >> 10, hc_ucd_size, hc_soc->prd_entry, hc_soc->prd_max >> 10,
	    hc_soc->prdt_bytes ? "bytes" : "dwords", dma32 ? ", all below 4 GB" : "");

	/* the device: NOP OUT first (ShengUfs), then, if the controller was enabled here, fDeviceInit */
	hc_pol.control = true;
	hc_pol.read_luns = 0;
	{
		int e = EIO;

		for (int tries = 0; tries < 20 && e != 0 && !hc_dead; tries++) {
			e = hc_nop();
			if (e != 0) {
				IOSleep(10);
			}
		}
		if (e != 0) {
			hc_not_used("the device does not answer NOP OUT");
			hc_off();
			return false;
		}
		hc_say("ufs: own: NOP IN: the device answers through this kernel's list\n");
	}
	if (hc_step == 3) {
		return hc_stop_here(3, "ufs_native_step=4 asks the device's units their capacity");
	}
	if (hc_enabled_here) {
		uint32_t flag = 1;

		hc_pol.device_init = true;
		(void)hc_query(QUERY_WRITE, QOP_SET_FLAG, QFLAG_DEVICE_INIT, 0, NULL, NULL, NULL);
		hc_pol.device_init = false;
		for (int tries = 0; tries < 1000 && (flag & 0xff) != 0; tries++) {
			if (hc_query(QUERY_READ, QOP_READ_FLAG, QFLAG_DEVICE_INIT, 0, &flag, NULL, NULL) != 0) {
				break;
			}
			if ((flag & 0xff) != 0) {
				IOSleep(1);
			}
		}
		hc_say("ufs: own: fDeviceInit set, %s\n", (flag & 0xff) == 0 ? "and the device has finished" : "NOT FINISHED");
	}
	{
		uint32_t power = 0;

		if (hc_query(QUERY_READ, QOP_READ_ATTR, QATTR_POWER_MODE, 0, &power, NULL, NULL) == 0) {
			hc_say("ufs: own: bCurrentPowerMode %02x%s\n", power & 0xff, (power & 0xff) == POWER_ACTIVE ? " (active)" :
			    ": waking it (START STOP UNIT, ACTIVE, to the device's unit)");
			if ((power & 0xff) != POWER_ACTIVE) {
				static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };

				(void)hc_scsi0(LUN_DEVICE, ssu, sizeof(ssu), 0, NULL, "START STOP UNIT", false);
				IOSleep(50);
			}
		}
	}
	if (hc_soc->prdt_bytes && !hc_enabled_here) {
		const char *why = hc_layout_read();

		if (why != NULL) {
			hc_not_used(why);
			hc_off();
			return false;           /* (hc_mode HC_OWN: the caller gives UEFI's list back, ufshc_release) */
		}
	}
	hc_find_units();
	{
		uint32_t mask = 0;

		for (uint8_t lun = 0; lun < UFSHC_LUNS; lun++) {
			if (hc_units[lun].present && hc_units[lun].block_size == BLOCK) {
				mask |= 1u << lun;
			}
		}
		hc_pol.read_luns = mask;
		if (mask == 0) {
			hc_say("ufs: own: no logical unit to read\n");
		}
	}
	if (hc_step == 4 && !hc_dead) {
		return hc_stop_here(4, "ufs_native_step=5 reads LBA 0 and each unit's GPT and makes the devices");
	}
	return !hc_dead;
}

/* own mode, at the end: in flight waited for, the cache synchronized if written, UEFI's list back */
static void
hc_own_give_back(void)
{
	struct hc_put_back_result r;
	uint64_t t;

	for (t = mach_absolute_time(); hc_read(UFSHC_UTRLDBR) != 0 && hc_us_since(t) < 1000000;) {
		__builtin_arm_yield();
	}
	if (hc_dirty && hc_pol.window && hc_read(UFSHC_UTRLDBR) == 0 && !hc_dead) {
		/* SYNCHRONIZE CACHE in slot 0, no locks (the other processors are stopped) */
		static const uint8_t sync10[10] = { 0x35 };
		struct hc_slot *s = &hc_slots[SLOT_CONTROL];
		struct hc_cmd c;

		hc_make_scsi(&c, hc_pol.w_lun, SLOT_CONTROL, sync10, sizeof(sync10), 0);
		if (hc_refusal(c.upiu, c.dw0, &hc_pol, false) == NULL) {
			hc_put(s->ucd, c.upiu, sizeof(c.upiu));
			s->utrd[0] = c.dw0;
			s->utrd[2] = OCS_INVALID;
			s->utrd[7] = 0;
			FlushPoC_DcacheRegion((vm_offset_t)s->utrd, 32);
			FlushPoC_DcacheRegion((vm_offset_t)s->ucd, hc_ucd_size);
			if (hc_soc->nexus != NULL) {
				hc_soc->nexus(SLOT_CONTROL, true);
			}
			if (hc_cnr_on()) {
				hc_write(UFSHC_UTRLCNR, 1u << SLOT_CONTROL);
			}
			hc_write(UFSHC_UTRLDBR, 1u << SLOT_CONTROL);
			for (t = mach_absolute_time(); (hc_read(UFSHC_UTRLDBR) & 1) && hc_us_since(t) < 2000000;) {
				if (hc_cnr_on() && (hc_read(UFSHC_UTRLCNR) & (1u << SLOT_CONTROL))) {
					hc_write(UFSHC_IS, IS_UTRCS);   /* done: acknowledged and cleared, as hc_finish */
					hc_write(UFSHC_UTRLCNR, 1u << SLOT_CONTROL);
				}
				__builtin_arm_yield();
			}
			FlushPoC_DcacheRegion((vm_offset_t)s->utrd, 32);
			hc_say_late("ufs: own: at the end: SYNCHRONIZE CACHE: doorbell %08x, OCS %x\n", hc_read(UFSHC_UTRLDBR),
			    s->utrd[2] & 0xff);
		}
	}
	if (hc_enabled_here) {
		hc_write(UFSHC_UTRLRSR, 0);
		hc_write(UFSHC_IE, hc_uefi_ie);
		hc_write(UFSHC_HCE, 0);
		hc_say_late("ufs: own: at the end: the controller disabled again, as it was found (HCE %08x)\n", hc_read(UFSHC_HCE));
		return;
	}
	hc_put_back(&r);
	hc_put_back_print("at the end", &r, true);
}

/*
 * Before the kernel gives the phone back to the loader (loader_return, the other processors
 * already stopped): nothing more is sent, what is in flight is waited for, UEFI's list put back.
 * No locks (a stopped processor may hold one).
 */
static void hc_quiesce(void);

void
hakios_ufs_quiesce(void)
{
	hc_quiesce();
	/* ufs=3: what the run did, once more, after the controller's own last lines (no printf here: no locks) */
	hakios_ufs_summary("the drivers given back", HAKIOS_UFS_SAY_LOG | HAKIOS_UFS_SAY_NUFS_ONLY);
}

static void
hc_quiesce(void)
{
	struct hc_put_back_result r;

	if (hc_kept_count != 0) {
		hc_say_late("ufs: native: what happened in this run, again:\n");
		for (uint32_t i = hc_kept_count > KEPT_LINES ? hc_kept_count - KEPT_LINES : 0; i < hc_kept_count; i++) {
			const char *line = hc_kept[i % KEPT_LINES];

			hc_say_late("> %s%s", line, line[0] != 0 && line[strlen(line) - 1] == '\n' ? "" : "...\n");
		}
	}
	if (!hc_usable) {
		return;
	}
	hc_closed = true;
	__asm__ volatile ("dsb sy" ::: "memory");
	hc_say_late("ufs: native: at the end: entered with doorbell %08x, %u requests of this kernel's in flight, its list %s\n",
	    hc_read(UFSHC_UTRLDBR), hc_in_flight, hc_in ? "in" : "out");
	if (hc_mode == HC_OWN) {
		hc_own_give_back();
		hc_report("at the end", true);
		return;
	}
	if (!hc_in && hc_read(UFSHC_UTRLBA) == hc_loader.uefi_lo && hc_read(UFSHC_UTRLBAU) == hc_loader.uefi_hi &&
	    (hc_read(UFSHC_UTRLRSR) & 1)) {
		hc_say_late("ufs: native: at the end: UEFI's list is in (doorbell %08x)\n", hc_read(UFSHC_UTRLDBR));
		hc_report("at the end", true);
		return;
	}
	hc_in = false;
	if (hc_leave) {
		hc_say_late("ufs: native: at the end: this kernel's list LEFT IN for the loader to take back "
		    "(ufs_native_leave=1; doorbell %08x)\n", hc_read(UFSHC_UTRLDBR));
		hc_report("at the end", true);
		return;
	}
	hc_put_back(&r);
	hc_put_back_print("at the end", &r, true);
	hc_report("at the end", true);
}
