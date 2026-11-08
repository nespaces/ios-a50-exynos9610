/*
 * The phone's storage: one block device and one character device for every GPT partition of its
 * UFS (/dev/ufs_NAME and /dev/rufs_NAME, NAME as the partition table has it). Read only, but for
 * the one place the data of Darwin lives (ufs_write=1, below).
 *
 * There is no UFS driver here, and none is needed. UEFI never exits on this port: Qualcomm's own
 * UFS driver (UFSDxe) has brought up the controller, its PHY, its clocks and power, and stays
 * resident, and xnuloader keeps serving the kernel from core 0 while the kernel runs on another
 * core (it already does for the battery). So with ufs=2 on its command line the loader publishes a
 * page (ufs_shared_pa=) with the table of partitions, a request and an answer, and a buffer; this
 * driver writes a request -- which partition, which blocks -- and core 0 reads them with that
 * driver's ReadBlocks into the buffer (xnuloader/loader.c, block_poll). Nothing in this kernel
 * touches the storage controller.
 *
 * Read only wherever a write could do harm, by construction. The block devices never write: the
 * strategy routine refuses, so no filesystem mounted from one can write, and opening one for
 * writing is refused. With ufs_write=1 one character device alone takes writes -- /dev/rufs_darwin,
 * the port's own partition -- and in it only whole blocks from UFS_WRITE_FIRST on, past the Darwin
 * volume and both cryptexes that the build writes from the computer (tools/darwin-write.py); that
 * is where the data volume of Darwin is kept between runs (userland/src/var_store.c). The partition
 * has to be the one the port was set up with (its name, size, place and logical unit here; its
 * GUIDs as well on the loader's side), and the loader refuses the same things over again and reads
 * every write back before it answers. Without ufs_write=1 nothing writes at all.
 *
 * One request at a time (the page has one), so requests are serialized by a mutex and split into
 * pieces of the buffer's size. The kernel writes only the request's cache lines and core 0 only
 * the answer's (see the loader), both through a mapping that does not cache here, so neither side
 * ever overwrites the other's. The data buffer, which only core 0's reads fill, is mapped cacheable
 * and the part of it an answer covers is invalidated here before it is copied out.
 *
 * The data volume of Darwin (HFS Plus, built into this kernel: bsd/hfs) is /dev/ufs_darwin_data: darwin from
 * UFS_WRITE_FIRST to its end, a block device of its own that reads and writes, there with ufs_write=1 only. A port
 * with no darwin has its loader offer one partition of its own instead, named in the loader's platform (the Galaxy
 * S9's USERDATA, given up to Darwin; UFS_DATA_PART here): all of it is the data volume, /dev/ufs_USERDATA_data, from
 * its first block, and the same rules hold for it as for darwin (see ufs_write_first). Its
 * writes go the same way as /dev/rufs_darwin's -- through the loader, which flushes and reads every one back before it
 * answers -- so a write that has come back is on the flash, which is what the file system's journal counts on. The
 * one and the other are not used in the same run: once the data volume has been opened for writing, the packed store
 * (var_store) is refused, and while a saver holds the store the data volume does not open for writing.
 *
 * ufs_check=1 reads, once at start, the partitions there are images of in the ROM's
 * firmware-update/ (bluetooth, xbl, tz, ...) through the same path and prints an FNV-1a hash of
 * as many bytes as each image has: tools/ufs-survey.py compares those with the images and with
 * what the loader read itself before the kernel started.
 *
 * ufs_native=4 (own mode): no loader page at all. This kernel's own driver (hakios.ufshc.c) has
 * the controller for the run -- QEMU's -device ufs, or the SoC's ufs_native_soc= names -- and
 * this file reads each logical unit's GPT itself (checked as ShengUfs checks it: the header's
 * signature, revision, size and CRC, the entries' CRC, the usable range, every partition inside
 * it, none overlapping another, no name twice) and makes the same /dev/ufs_NAME and /dev/rufs_NAME
 * of every partition. The table is kept in a struct loader_block of this kernel's own, so that
 * everything below reads it as it reads the loader's. ufs_native=5 only looks (hakios.ufshc.c);
 * ufs_native_step=1..4 stops the driver at a step of its bring-up (look, the link, NOP OUT, READ
 * CAPACITY) with nothing read here, 5 (QEMU's and the POCO's default; the S9's is 1) goes on to
 * LBA 0 and the GPTs.
 *
 * Writes in own mode go to one partition only, the one ufs_native_write=NAME names, through its
 * character device /dev/rufs_NAME, in whole blocks, and only if NAME is not one that must never be
 * written (ufs_never_written below: the S9's EFS, SEC_EFS, BOOT, RECOVERY, PARAM and the rest of its
 * boot chain and device-unique data, the POCO's modem file systems and boot images, any name with
 * "efs" in it) and its unit is neither a boot unit nor write protected. The controller's driver is
 * told the partition's blocks (ufshc_open_window) and refuses any write outside them on its own.
 * Closing the device synchronizes the device's cache. ufs_native_test=1 tries all of it at start:
 * refusals that must happen, and a write, read back and put back as it was, at three places of
 * the partition.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/hakios.input.h>
#include <libkern/crc.h>
#include <stdarg.h>
#include <vm/memory_types.h>
#include "hakios.ufshc.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void IOSleep(unsigned int milliseconds);
extern void loader_return(void);        /* osfmk/arm64/loader_return.c */
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */
extern int sync_internal(void);          /* bsd/vfs/vfs_syscalls.c: a shutdown's sync, bounded */

/* the page the loader publishes: the same layout as struct loader_block in xnuloader/loader.c */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u
#define BLOCK_OP_WRITE_FAST 2u                         /* written only: the data volume's (the loader, BLOCK_OP_WRITE_FAST) */
#define BLOCK_OP_FLUSH     3u
#define BLOCK_CAN_FAST     1u

/*
 * Where a write may go, and nowhere else: darwin, the port's own 20 GiB at the end of logical
 * unit 0, and in it only past what the build puts there from the computer -- the Darwin volume
 * (blocks 0..1374207) and the two cryptexes behind it (..2042367). The loader keeps the same
 * bounds and checks the partition's type besides (xnuloader/loader.c, BLOCK_WRITE_FIRST). Where darwin
 * begins and its exact size are the phone's (tools/userdata-carve.py puts it after userdata, whatever
 * that is): at least the layout's 20 GiB.
 */
#define UFS_WRITE_FIRST    2042368ULL                  /* darwin's blocks of 4096 */
#define UFS_DARWIN_MIN     5242875ULL                  /* the first phone's darwin, in blocks of 4096 */
/*
 * The only other partition a write may reach: the Galaxy S9's USERDATA (Android's data, given up), when the loader
 * offers it (its platform names it: xnuloader/loader.c, data_part) and from its first block -- nothing of the
 * port's is in it. A partition, not a unit; the first unit; blocks of 4096; at least 1 GiB.
 */
#define UFS_DATA_PART      "USERDATA"
#define UFS_DATA_PART_MIN  262144ULL                   /* blocks of 4096: 1 GiB */

struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition the loader will take writes for this run (its index plus one; 0: none) and
	 * from which of its blocks. They sit in what was padding, so the page is the size and the
	 * version it was and one phone goes on being shared: a loader built before writes existed
	 * leaves them zero, which is no write, and a kernel built before them leaves ask.op zero,
	 * which is a read.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_caps;                   /* BLOCK_CAN_FAST; zero from a loader that has only BLOCK_OP_WRITE */
	uint64_t write_first;
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/* the loader's offer of an area for this kernel's own transfer request list, and UEFI's list */
	struct {
		uint64_t hc_pa;
		uint64_t area_pa;
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	struct {
		volatile uint32_t take_back;   /* the loader's: it takes the controller back, send nothing more */
	} loader_native __attribute__((aligned(128)));
	struct {
		volatile uint32_t list_in;     /* this kernel's: its list is in */
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when its list went in (for the loader's take-back) */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the loader's page must stay the size it was: what is before ask has to fit in its padding");

#define UFS_PAGE           0x4000UL
#define UFS_TIMEOUT_MS     20000        /* core 0 answers in well under a second; this is for a dead loader */

static volatile struct loader_block *ufs_page;
static uint8_t *ufs_buffer;             /* the loader's data buffer: filled by it for a read, here for a write */
static uint32_t ufs_buffer_size, ufs_parts, ufs_seq;
static uint32_t ufs_bsize[BLOCK_MAX_PARTS];   /* the block size each device is counted in (spec_open sets 512) */
static int ufs_bmajor = -1, ufs_cmajor = -1;
static bool ufs_ready, ufs_dead;
static uint64_t ufs_requests, ufs_errors, ufs_bytes, ufs_wait_us;
static uint64_t ufs_copy_us;                    /* copying out of the loader's buffer */
static uint64_t ufs_small, ufs_medium, ufs_large; /* requests of <= 4 KB, <= 64 KB, more */
static uint64_t ufs_sequential;                 /* requests that start where the one before ended */
static uint32_t ufs_last_part = ~0u;
static uint64_t ufs_last_end;

/*
 * This kernel's own UFS driver (hakios.ufshc.c), with ufs_native=1: each partition's logical
 * unit and where it begins there, in bytes; reads go that way unless it fails, and then through
 * the loader again. While its list is in the controller the loader is asked for nothing.
 */
static uint8_t ufs_lun[BLOCK_MAX_PARTS];         /* 0xff: not known, read through the loader */
static uint64_t ufs_lun_offset[BLOCK_MAX_PARTS];
static bool ufs_native_mapped;
static uint32_t ufs_native_luns;                /* how many units are mapped (LUN 0..n-1) */
static volatile bool ufs_native_on;
static uint64_t ufs_native_fallbacks, ufs_mailbox_refused;

/* ufs=3 (below: ufs_nufs_start) */
static bool ufs_nufs;                           /* ufs=3 asked for */
static volatile bool ufs_nufs_on;               /* switched: this kernel's own driver has the controller */
static bool ufs_nufs_ro;                        /* ufs_native_ro=1 */
static uint32_t ufs_nufs_verify;                /* ufs_native_verify=1: every write read back and compared */
static uint32_t ufs_nufs_soak;                  /* ufs_native_soak=N: N random reads of SYSTEM both ways before the switch */
static uint64_t ufs_nufs_unmapped, ufs_nufs_verify_bad;
static int ufs_nufs_failed(const char *name, uint64_t offset, int error);
static void nufs_say(const char *format, ...) __printflike(1, 2);
static int ufs_nufs_write(uint32_t part, uint64_t offset, const uint8_t *src, uint64_t len);

/*
 * ufs=3: what the run did, kept in memory and said as a summary (hakios_ufs_summary): when the USB console comes up,
 * on its "ufs", every 30 s with fps_log=1 or ufs_native_log=1, and at the end of the run (into the kernel's log, and
 * the last lines of the loader's). Only counted here: nothing of it decides anything.
 */
static struct {
	uint32_t step;                  /* the last step begun (1-5); 0: not started */
	int selftest, fences, soak;     /* 0 not run, 1 passed, -1 failed */
	uint32_t fences_tried;
	char why[240];                  /* why not native: the first reason given */
	uint64_t reads_native, reads_loader, read_errors, read_bytes, refused;
	char refusal[3][100];           /* the first three refusals */
} nufs_sum;
static void nufs_refused(const char *format, ...) __printflike(1, 2);
static void nufs_why(const char *why);
static const char *nufs_pname(uint32_t part);

/* own mode (ufs_native=4): the table is this kernel's, the loader serves nothing */
static bool ufs_own;
static int ufs_own_write_part = -1;             /* the partition ufs_native_write= names; -1: none */
static int ufs_own_write(uint32_t part, uint64_t offset, const uint8_t *src, uint64_t len);
static struct loader_block *ufs_own_table;

static LCK_GRP_DECLARE(ufs_lck_grp, "hakios-ufs");
static LCK_MTX_DECLARE(ufs_lock, &ufs_lck_grp);
static LCK_MTX_DECLARE(ufs_nufs_mtx, &ufs_lck_grp);      /* ufs=3: one giving back at a time */
/* One writer at a time; never held while the mailbox is, nor while user memory is touched. */
static LCK_MTX_DECLARE(ufs_write_mtx, &ufs_lck_grp);

/*
 * Saving the data volume without being asked (userland/src/var_store.c).
 *
 * The volume is packed and written by a program, not by this kernel, so the kernel's part is to
 * say when and to let only one program at a time do it:
 *
 *   kern.hakios.var_save   the handshake. The saver writes VAR_SAVE_IDLE when it starts and is
 *                           then the one that will be asked; on the orderly way back to the loader
 *                           (loader_return_after expired, or "xl stop") the kernel writes
 *                           VAR_SAVE_WANTED and waits, and the saver writes VAR_SAVE_DONE when the
 *                           volume is in the flash. Nobody registered: nothing is asked and
 *                           nothing waited for, so a run without a saver returns as it always did.
 *                           A panic does not come this way at all -- there is no program left to
 *                           ask -- and then the store simply keeps what it had.
 *   kern.hakios.var_busy   the claim. A write to /dev/rufs_darwin is refused unless the writer
 *                           holds it, so two savers cannot interleave into one slot. It is held by
 *                           process number, and a claimant may take it from a process that is gone
 *                           (a saver killed in the middle leaves no lock behind for the rest of the
 *                           run). What the one that was killed half-wrote is not lost either: the
 *                           slot it was writing is not the one holding the newest good archive.
 */
#define VAR_SAVE_NONE      0
#define VAR_SAVE_IDLE      1
#define VAR_SAVE_WANTED    2
#define VAR_SAVE_DONE      3
#define VAR_SAVE_MS        20000        /* at most this long on the way out (var_save_ms=N) */

static volatile int ufs_var_save = VAR_SAVE_NONE;
static int ufs_var_busy_pid;

/* ufs_write=1 and the one partition it is for: everything else answers EROFS as it always did */
static int ufs_write_part = -1;
static uint64_t ufs_write_first = UFS_WRITE_FIRST;     /* its first block a write may reach: darwin's, or 0 (USERDATA) */
static volatile bool ufs_write_busy;            /* a write of ours has this kernel's list out: not a failure */
static bool ufs_write_out, ufs_write_was_native;        /* the list is out for writing, and was in before */
static uint64_t ufs_writes, ufs_write_bytes, ufs_write_errors, ufs_write_refused;

/* /dev/ufs_darwin_data: the minor number past every partition's, and the block size it is counted in */
#define UFS_DATA_MINOR     BLOCK_MAX_PARTS
static uint32_t ufs_data_bsize = 4096;
static bool ufs_data_used;                      /* opened for writing this run: the packed store is not */
static LCK_MTX_DECLARE(ufs_data_mtx, &ufs_lck_grp);     /* one write of the data volume at a time */

SYSCTL_DECL(_kern_hakios);
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_requests,
    "requests the phone's storage has answered through the loader");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_errors,
    "requests the phone's storage has refused or not answered");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_bytes,
    "bytes read from the phone's storage");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_wait_us, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_wait_us,
    "microseconds spent waiting for core 0's answers");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_copy_us, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_copy_us,
    "microseconds spent copying out of the loader's buffer");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_writes, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_writes,
    "requests written into darwin and read back by the loader");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_write_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_write_bytes,
    "bytes written into darwin");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_write_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_write_errors,
    "writes the loader refused or did not finish");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_write_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_write_refused,
    "writes refused here: not darwin, or not past what the build writes");

/* The handshake: 1 to register as the saver, 3 to say the volume is in the flash. 2 is the kernel's. */
static int
ufs_sysctl_var_save(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = ufs_var_save, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	if (value == VAR_SAVE_IDLE && ufs_var_save != VAR_SAVE_WANTED) {
		ufs_var_save = VAR_SAVE_IDLE;           /* a saver is there; it is the one that will be asked */
	} else if (value == VAR_SAVE_DONE && ufs_var_save == VAR_SAVE_WANTED) {
		ufs_var_save = VAR_SAVE_DONE;           /* and it has finished: the way out may go on */
	} else if (value == VAR_SAVE_NONE) {
		ufs_var_save = VAR_SAVE_NONE;
	} else {
		return EINVAL;
	}
	return 0;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, var_save, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    ufs_sysctl_var_save, "I", "the data volume's saver: 1 registers, 2 is the kernel asking, 3 is done");

/* The claim on the store: 1 takes it, 0 gives it back; reading says which process holds it. */
static int
ufs_sysctl_var_busy(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = ufs_var_busy_pid, error, me = proc_selfpid();

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	lck_mtx_lock(&ufs_write_mtx);
	if (value == 1 && ufs_data_used) {
		error = EBUSY;          /* the data volume is mounted from darwin this run: no packed store into it */
	} else if (value == 1) {
		if (ufs_var_busy_pid != 0 && ufs_var_busy_pid != me) {
			proc_t held = proc_find(ufs_var_busy_pid);

			if (held != NULL) {
				proc_rele(held);
				lck_mtx_unlock(&ufs_write_mtx);
				return EBUSY;   /* another saver has it and is alive: one at a time */
			}
			printf("var_store: the claim on the store was left by process %d, which is gone\n",
			    ufs_var_busy_pid);
		}
		ufs_var_busy_pid = me;
	} else if (value == 0) {
		if (ufs_var_busy_pid == me || ufs_var_busy_pid == 0) {
			ufs_var_busy_pid = 0;
		} else {
			error = EPERM;
		}
	} else {
		error = EINVAL;
	}
	lck_mtx_unlock(&ufs_write_mtx);
	return error;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, var_busy, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    ufs_sysctl_var_busy, "I", "the claim a writer of /dev/rufs_darwin must hold: 1 takes it, 0 gives it back");

/*
 * Native blocks [lba, lba + count) of partition part, one request of at most the buffer's size:
 * read into dst, or, with src, written from there. Called with ufs_lock held.
 */
static int
ufs_request(uint32_t op, uint32_t part, uint64_t lba, uint64_t count, uint8_t *dst, const uint8_t *src)
{
	uint64_t bytes = count * ufs_page->part[part].block_size, started, waited_ms = 0;
	uint32_t seq = ++ufs_seq;
	int64_t status;

	if (ufs_dead) {
		return EIO;
	}
	if (op == BLOCK_OP_READ) {
		if (part == ufs_last_part && lba == ufs_last_end) {
			ufs_sequential++;
		}
		ufs_last_part = part;
		ufs_last_end = lba + count;
	} else if (op != BLOCK_OP_FLUSH) {
		/*
		 * The data into the buffer and out of this core's cache before the request is raised:
		 * core 0 drops its own lines of the buffer and hands it to UEFI's driver.
		 */
		memcpy(ufs_buffer, src, (size_t)bytes);
		FlushPoC_DcacheRegion((vm_offset_t)ufs_buffer, (size_t)bytes);
		ufs_last_part = ~0u;
	}
	ufs_page->ask.part = part;
	ufs_page->ask.lba = lba;
	ufs_page->ask.count = count;
	ufs_page->ask.op = op;
	__asm__ volatile ("dmb sy" ::: "memory");
	ufs_page->ask.request = seq;
	__asm__ volatile ("dmb sy" ::: "memory");

	started = mach_absolute_time();
	while (ufs_page->answer.answered != seq) {
		/* spin briefly for a quick answer, then sleep a millisecond at a time */
		uint64_t us;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &us);
		us /= 1000;
		if (us < 2000) {
			/* the answer to a small request comes in tens of microseconds: look often */
			__builtin_arm_yield();
			continue;
		}
		IOSleep(1);
		if (++waited_ms > UFS_TIMEOUT_MS) {
			/* no answer: the loader is gone or stuck; nothing more is asked of it */
			printf("ufs: no answer from the loader to request %u in %u s: the storage is off from now on\n",
			    seq, UFS_TIMEOUT_MS / 1000);
			ufs_dead = true;
			ufs_errors++;
			return EIO;
		}
	}
	__asm__ volatile ("dmb sy" ::: "memory");
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		ufs_wait_us += ns / 1000;
	}
	status = ufs_page->answer.status;
	if (status != 0) {
		printf("ufs: %s: blocks %llu..%llu: the loader's %s failed (status %llx)\n",
		    ufs_page->part[part].name, lba, lba + count - 1,
		    op == BLOCK_OP_READ ? "read" : op == BLOCK_OP_FLUSH ? "flush" : "write", (uint64_t)status);
		ufs_errors++;
		return EIO;
	}
	if (op != BLOCK_OP_READ) {
		/* BLOCK_OP_WRITE: the loader has flushed, read the blocks back and found them the same */
		if (op != BLOCK_OP_FLUSH) {
			ufs_writes++;
			ufs_write_bytes += bytes;
		}
		return 0;
	}
	started = mach_absolute_time();
	/*
	 * The buffer is mapped cacheable here (copying out of an uncached mapping went at 870 MB/s,
	 * as slow as the storage itself). Core 0 cleans and invalidates it after every read, and this
	 * side only ever fills it for a write of its own and cleans its lines then; whatever this
	 * core may still hold of an earlier request -- a line brought in by a prefetch, say -- goes
	 * before the copy.
	 */
	FlushPoC_DcacheRegion((vm_offset_t)ufs_buffer, (size_t)bytes);
	memcpy(dst, ufs_buffer, (size_t)bytes);
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		ufs_copy_us += ns / 1000;
	}
	ufs_requests++;
	ufs_bytes += bytes;
	if (bytes <= 4096) {
		ufs_small++;
	} else if (bytes <= 65536) {
		ufs_medium++;
	} else {
		ufs_large++;
	}
	return 0;
}

/* One of this driver's devices (block or character): a request to it costs tens of microseconds */
bool
hakios_ufs_dev(dev_t dev)
{
	return ufs_ready && ((ufs_bmajor >= 0 && major(dev) == ufs_bmajor) ||
	       (ufs_cmajor >= 0 && major(dev) == ufs_cmajor));
}

/*
 * Where the time of reading the storage went, so far: waiting for core 0's answer (of which
 * the loader's own clock says how long ReadBlocks itself took) and copying out of its buffer.
 */
void
hakios_ufs_report(const char *when)
{
	uint64_t loader_ns = 0;

	if (!ufs_ready) {
		return;
	}
	ufshc_report(when);
	if (ufs_own) {
		return;                 /* (no loader, no page: the driver's own report is all there is) */
	}
	absolutetime_to_nanoseconds(ufs_page->answer.ticks, &loader_ns);
	printf("ufs: %s: %llu requests (%llu of 4 KB or less, %llu to 64 KB, %llu more; %llu right after "
	    "the one before), %llu errors, %llu bytes; waiting %llu ms, of it in ReadBlocks %llu ms; "
	    "copying %llu ms\n", when, ufs_requests, ufs_small, ufs_medium, ufs_large, ufs_sequential,
	    ufs_errors, ufs_bytes, ufs_wait_us / 1000, loader_ns / 1000000, ufs_copy_us / 1000);
	if (ufs_write_part >= 0 || ufs_writes != 0 || ufs_write_refused != 0) {
		printf("ufs: %s: %llu writes into darwin, %llu bytes, read back by the loader; %llu did not "
		    "finish, %llu refused here\n", when, ufs_writes, ufs_write_bytes, ufs_write_errors,
		    ufs_write_refused);
	}
}

static uint64_t
ufs_part_bytes(uint32_t part)
{
	return ufs_page->part[part].blocks * ufs_page->part[part].block_size;
}

/*
 * Bytes [offset, offset + len) of partition part into dst, whatever their alignment, through the
 * loader: the native blocks that cover them are read a buffer at a time and the wanted bytes
 * copied out.
 */
static int
ufs_read_mailbox(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	uint64_t nbs = ufs_page->part[part].block_size, per = ufs_buffer_size / nbs;
	static uint8_t *bounce;
	int error = 0;

	if (ufs_own || ufs_buffer == NULL) {
		return ENXIO;           /* own mode: there is no loader to ask */
	}
	lck_mtx_lock(&ufs_lock);
	/*
	 * Allocated under the lock: two first readers at once would both allocate (one 4 MB leak),
	 * and one could copy out of a buffer the other has just replaced and never filled.
	 */
	if (bounce == NULL) {
		bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
		if (bounce == NULL) {
			lck_mtx_unlock(&ufs_lock);
			return ENOMEM;
		}
	}
	if (ufshc_list_in()) {
		/* this kernel's own transfer list is in the controller: UEFI's driver must not be asked to ring */
		if (ufs_mailbox_refused++ < 4) {
			printf("ufs: %s: not asked of the loader while this kernel's own transfer list is in\n",
			    ufs_page->part[part].name);
		}
		lck_mtx_unlock(&ufs_lock);
		return EIO;
	}
	while (len > 0 && error == 0) {
		uint64_t first = offset / nbs, skip = offset % nbs;
		uint64_t count = (skip + len + nbs - 1) / nbs, take;

		if (count > per) {
			count = per;
		}
		take = count * nbs - skip;
		if (take > len) {
			take = len;
		}
		if (skip == 0 && take == count * nbs) {
			error = ufs_request(BLOCK_OP_READ, part, first, count, dst, NULL);   /* whole blocks: straight in */
		} else {
			error = ufs_request(BLOCK_OP_READ, part, first, count, bounce, NULL);
			if (error == 0) {
				memcpy(dst, bounce + skip, (size_t)take);
			}
		}
		offset += take;
		dst += take;
		len -= take;
	}
	lck_mtx_unlock(&ufs_lock);
	return error;
}

/* The same, through this kernel's own driver: the partition's unit, from where it begins there */
static int
ufs_read_native(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	if (part >= ufs_parts || ufs_lun[part] == 0xff) {
		return ENXIO;
	}
	return ufshc_read(ufs_lun[part], ufs_lun_offset[part] + offset, dst, len);
}

/* The switches of lists, with the loader's mailbox held: nothing is asked of UEFI meanwhile */
static int
ufs_native_in(bool prove)
{
	int error;

	lck_mtx_lock(&ufs_lock);
	/* (a loader that did not answer may still be inside UEFI's driver) */
	error = ufs_dead ? EIO : ufshc_switch_in(prove);
	lck_mtx_unlock(&ufs_lock);
	return error;
}

static int
ufs_native_out(const char *why)
{
	int error;

	lck_mtx_lock(&ufs_lock);
	error = ufshc_switch_out(why);
	lck_mtx_unlock(&ufs_lock);
	return error;
}

/* ufs=3's summary: a read through the loader, counted */
static int
nufs_loader_read(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	int error = ufs_read_mailbox(part, offset, dst, len);

	if (error == 0) {
		os_atomic_inc(&nufs_sum.reads_loader, relaxed);
		os_atomic_add(&nufs_sum.read_bytes, len, relaxed);
	} else {
		static uint32_t said;

		os_atomic_inc(&nufs_sum.read_errors, relaxed);
		if (said < 8) {
			said++;
			/* (why the loader's path failed: refused here while this kernel's list is in, or UEFI's driver's error) */
			nufs_say("ufs: nufs: %s: a read through the loader at byte %llu, %llu bytes, failed (%d)%s\n",
			    (const char *)ufs_page->part[part].name, offset, len, error, ufshc_list_in() ?
			    " (this kernel's list is still in)" : "");
		}
	}
	return error;
}

/* The way chosen (ufs_native=, kern.hakios.ufs_native); if this kernel's own fails, the loader's */
static int
ufs_read(uint32_t part, uint64_t offset, uint8_t *dst, uint64_t len)
{
	/*
	 * No read leaves its partition on the native path either (the loader's path refuses it already):
	 * a short partition would otherwise be read together with the one that follows it. Written
	 * without a sum that can wrap.
	 */
	uint64_t part_size = ufs_part_bytes(part);

	if (offset > part_size || len > part_size - offset) {
		return EINVAL;
	}
	if (ufs_own) {
		return ufs_read_native(part, offset, dst, len);         /* the only way there is */
	}
	if (ufs_nufs_on) {
		/* ufs=3: this kernel's own driver has the controller; the loader is not asked while it does */
		int error = ufs_lun[part] == 0xff ? ENXIO : ufs_read_native(part, offset, dst, len);

		if (error == 0) {
			os_atomic_inc(&nufs_sum.reads_native, relaxed);
			os_atomic_add(&nufs_sum.read_bytes, len, relaxed);
			return 0;
		}
		if (ufs_lun[part] == 0xff) {
			nufs_refused("a read of %s: on a unit this kernel's driver does not read", nufs_pname(part));
			if (ufs_nufs_unmapped++ < 4) {
				nufs_say("ufs: nufs: %s: on a unit this kernel's driver does not read: not read\n",
				    (const char *)ufs_page->part[part].name);
			}
			return ENXIO;
		}
		os_atomic_inc(&nufs_sum.read_errors, relaxed);
		if (ufs_nufs_failed((const char *)ufs_page->part[part].name, offset, error) != 0) {
			return EIO;
		}
		return nufs_loader_read(part, offset, dst, len);
	}
	if (ufs_native_on && ufs_lun[part] != 0xff) {
		int error = ufs_read_native(part, offset, dst, len);

		if (error == 0) {
			return 0;
		}
		if (ufs_write_busy) {
			/* the list is out for a write of ours, not broken: this read goes the other way */
			return nufs_loader_read(part, offset, dst, len);
		}
		if (ufs_native_on) {
			ufs_native_on = false;
			ufs_native_fallbacks++;
			printf("ufs: %s: bytes %llu..%llu: this kernel's own read failed (%d); through the loader from now on\n",
			    ufs_page->part[part].name, offset, offset + len - 1, error);
		}
		ufs_native_out("a read failed");
	}
	return nufs_loader_read(part, offset, dst, len);
}

/*
 * Whole blocks [offset, offset + len) of the one partition that takes writes, from src, through
 * the loader. This kernel's own driver never writes -- its whitelist of commands has no write in
 * it -- so for the length of a write its transfer list goes out and UEFI's driver, which the
 * loader calls, has the controller again; reads meanwhile go through the loader too (ufs_read),
 * which is slower and right. Nothing of Darwin waits on this but the writer itself: the data volume
 * is in memory (ramfs) and only its copy for the next boot is written here.
 */
static int
ufs_write(uint32_t op, uint32_t part, uint64_t offset, const uint8_t *src, uint64_t len)
{
	uint64_t nbs, per;
	int error = 0;

	if (!ufs_ready || ufs_write_part < 0 || part != (uint32_t)ufs_write_part) {
		ufs_write_refused++;
		nufs_refused("a write to %s: %s", nufs_pname(part), ufs_write_part < 0 ? "nothing takes writes in this run" :
		    "not the partition that takes writes");
		return EROFS;
	}
	nbs = ufs_page->part[part].block_size;
	if (len == 0 || offset % nbs != 0 || len % nbs != 0) {
		return EINVAL;          /* whole blocks only: nothing is read before a write */
	}
	if (offset / nbs < ufs_write_first || offset > ufs_part_bytes(part) ||
	    len > ufs_part_bytes(part) - offset) {
		/* the Darwin volume, the cryptexes, and anything past the partition's end: never */
		ufs_write_refused++;
		nufs_refused("a write to %s at byte %llu: outside the blocks it takes writes in", nufs_pname(part), offset);
		return EROFS;
	}
	per = ufs_buffer_size / nbs;
	lck_mtx_lock(&ufs_write_mtx);
	if (ufs_own) {
		/* own mode (ufs_native_data=1): the data volume through this kernel's driver, inside the partition's window */
		error = ufs_own_write(part, offset, src, len);
		lck_mtx_unlock(&ufs_write_mtx);
		return error;
	}
	if (ufs_nufs_on) {
		/* ufs=3: through this kernel's own driver, into USERDATA's window only; never the loader's way as well */
		error = ufs_nufs_write(part, offset, src, len);
		lck_mtx_unlock(&ufs_write_mtx);
		return error;
	}
	if (ufs_nufs && !ufs_nufs_on && ufs_dead) {
		lck_mtx_unlock(&ufs_write_mtx);
		return EIO;
	}
	/*
	 * The list goes out once, at the first write, and comes back when the device is closed: a
	 * whole data volume is written a bufferful at a time, and switching the controller's list
	 * for each of those would say so a hundred times over and buy nothing. ufs_native_on is
	 * cleared first, so that a reader about to start goes the other way of its own accord; one
	 * already inside this kernel's driver gets an error and is sent the other way by
	 * ufs_write_busy.
	 */
	if (!ufs_write_out) {
		ufs_write_was_native = ufs_native_on;
		ufs_write_busy = true;
		ufs_write_out = true;
		if (ufs_write_was_native) {
			ufs_native_on = false;
			ufs_native_out("writing the data volume into darwin");
		}
	}
	while (len > 0 && error == 0) {
		uint64_t count = len / nbs;

		if (count > per) {
			count = per;
		}
		lck_mtx_lock(&ufs_lock);
		if (ufshc_list_in()) {
			/* it did not go out after all: UEFI's driver must not be asked to ring */
			error = EIO;
		} else {
			error = ufs_request(op, part, offset / nbs, count, NULL, src);
		}
		lck_mtx_unlock(&ufs_lock);
		offset += count * nbs;
		src += count * nbs;
		len -= count * nbs;
	}
	lck_mtx_unlock(&ufs_write_mtx);
	if (error != 0) {
		ufs_write_errors++;
	}
	return error;
}

/* The writing over: this kernel's own transfer list back in, and reads through it again. */
static void
ufs_write_restore(void)
{
	lck_mtx_lock(&ufs_write_mtx);
	if (ufs_write_out) {
		if (ufs_write_was_native && ufs_native_in(false) == 0) {
			ufs_native_on = true;
		}
		ufs_write_busy = false;
		ufs_write_out = false;
	}
	lck_mtx_unlock(&ufs_write_mtx);
}

/* ---- the data volume: darwin from UFS_WRITE_FIRST on, or USERDATA from its first block ---- */

static bool
ufs_is_data(dev_t dev)
{
	return ufs_ready && ufs_write_part >= 0 && (uint32_t)minor(dev) == UFS_DATA_MINOR;
}

static uint64_t
ufs_data_bytes(void)
{
	return ufs_part_bytes((uint32_t)ufs_write_part) - ufs_write_first * 4096;
}

/*
 * How the data volume writes: only written, when the loader can (BLOCK_CAN_FAST), the flash's cache flushed when the
 * file system asks (DKIOCSYNCHRONIZE: HFS Plus's journal, where the order of what reaches the flash matters); else as
 * /dev/rufs_darwin writes, each flushed and read back, which is safe and slow.
 */
static uint32_t
ufs_data_op(void)
{
	return (ufs_page->write_caps & BLOCK_CAN_FAST) ? BLOCK_OP_WRITE_FAST : BLOCK_OP_WRITE;
}

static int
ufs_data_flush(void)
{
	int error;

	if (ufs_nufs_on || ufs_own) {
		/* ufs=3 or own mode: the device's cache, through this kernel's own driver (SYNCHRONIZE CACHE to USERDATA's unit) */
		lck_mtx_lock(&ufs_write_mtx);
		error = ufshc_sync();
		lck_mtx_unlock(&ufs_write_mtx);
		return error;
	}
	if (ufs_data_op() != BLOCK_OP_WRITE_FAST) {
		return 0;               /* every write was flushed already */
	}
	lck_mtx_lock(&ufs_write_mtx);
	lck_mtx_lock(&ufs_lock);
	/* one block in bounds, for the loader's checks; nothing of it is touched */
	error = ufshc_list_in() ? EIO : ufs_request(BLOCK_OP_FLUSH, (uint32_t)ufs_write_part, ufs_write_first, 1, NULL, NULL);
	lck_mtx_unlock(&ufs_lock);
	lck_mtx_unlock(&ufs_write_mtx);
	return error;
}

/*
 * Bytes [offset, offset + len) of the data volume, read into p or written from it. A write that is not whole blocks
 * of 4096 (the device may be counted in 512-byte ones) is made whole here: the blocks around it read, patched,
 * written.
 */
static int
ufs_data_io(bool write, uint64_t offset, uint8_t *p, uint64_t len)
{
	uint64_t base = ufs_write_first * 4096, size = ufs_data_bytes();
	uint8_t *bounce;
	int error = 0;

	if (offset > size || len > size - offset) {
		return EINVAL;
	}
	if (!write) {
		return ufs_read((uint32_t)ufs_write_part, base + offset, p, len);
	}
	lck_mtx_lock(&ufs_data_mtx);
	if (offset % 4096 == 0 && len % 4096 == 0) {
		error = ufs_write(ufs_data_op(), (uint32_t)ufs_write_part, base + offset, p, len);
		lck_mtx_unlock(&ufs_data_mtx);
		return error;
	}
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		lck_mtx_unlock(&ufs_data_mtx);
		return ENOMEM;
	}
	while (len > 0 && error == 0) {
		uint64_t first = offset & ~4095ULL, skip = offset - first;
		uint64_t span = (skip + len + 4095) & ~4095ULL, take;

		if (span > ufs_buffer_size) {
			span = ufs_buffer_size;
		}
		take = MIN(span - skip, len);
		error = ufs_read((uint32_t)ufs_write_part, base + first, bounce, span);
		if (error == 0) {
			memcpy(bounce + skip, p, (size_t)take);
			error = ufs_write(ufs_data_op(), (uint32_t)ufs_write_part, base + first, bounce, span);
		}
		offset += take;
		p += take;
		len -= take;
	}
	kfree_data(bounce, ufs_buffer_size);
	lck_mtx_unlock(&ufs_data_mtx);
	return error;
}

static void
ufs_data_strategy(buf_t bp)
{
	uint64_t offset = (uint64_t)buf_blkno(bp) * ufs_data_bsize, len = buf_count(bp), size = ufs_data_bytes();
	caddr_t mapped;
	int error;

	if (offset >= size) {
		buf_seterror(bp, offset == size ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	len = MIN(len, size - offset);
	/* HFS Plus's cluster I/O hands over pages with no kernel address yet: buf_map() gives them one */
	if ((error = buf_map(bp, &mapped)) == 0) {
		error = ufs_data_io(!(buf_flags(bp) & B_READ), offset, (uint8_t *)mapped, len);
		buf_unmap(bp);
	}
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_data_ioctl(u_long cmd, caddr_t data)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = ufs_data_bsize;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (*(uint32_t *)data < 512 || *(uint32_t *)data > 4096 ||
		    (*(uint32_t *)data & (*(uint32_t *)data - 1)) != 0) {
			return EINVAL;
		}
		ufs_data_bsize = *(uint32_t *)data;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = 4096;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_data_bytes() / ufs_data_bsize;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 1;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = ufs_buffer_size / ufs_data_bsize;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE:
		return ufs_data_flush();
	default:
		return ENOTTY;
	}
}

/* ---- the devices ---- */

static int
ufs_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	if (ufs_is_data(dev)) {
		if (flags & FWRITE) {
			lck_mtx_lock(&ufs_write_mtx);
			if (ufs_var_busy_pid != 0) {
				lck_mtx_unlock(&ufs_write_mtx);
				return EBUSY;   /* a saver is writing the packed store into darwin */
			}
			ufs_data_used = true;
			lck_mtx_unlock(&ufs_write_mtx);
		}
		return 0;
	}
	if (!ufs_ready || (uint32_t)minor(dev) >= ufs_parts || (ufs_page->part[minor(dev)].flags & BLOCK_PART_UNIT)) {
		return ENXIO;
	}
	if (flags & FWRITE) {
		nufs_refused("/dev/ufs_%s opened for writing: a block device of this driver never writes",
		    nufs_pname((uint32_t)minor(dev)));
		return EROFS;           /* a block device of this driver never writes */
	}
	return 0;
}

/* The same, for the character devices: the one partition that takes writes may be opened for them. */
static int
ufs_copen(dev_t dev, int flags, int devtype, proc_t p)
{
	if ((flags & FWRITE) && ufs_write_part >= 0 && (uint32_t)minor(dev) == (uint32_t)ufs_write_part) {
		if (ufs_data_used) {
			return EBUSY;   /* the data volume is in darwin this run */
		}
		flags &= ~FWRITE;
	}
	if ((flags & FWRITE) && ufs_own && ufs_own_write_part >= 0 && (uint32_t)minor(dev) == (uint32_t)ufs_own_write_part) {
		flags &= ~FWRITE;
	}
	return ufs_open(dev, flags, devtype, p);
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void ufs_write_restore(void);

/* The last close of /dev/rufs_darwin ends the writing: this kernel's own transfer list goes back in. */
static int
ufs_cclose(dev_t dev, int flags, int devtype, proc_t p)
{
	if (ufs_write_part >= 0 && (uint32_t)minor(dev) == (uint32_t)ufs_write_part) {
		ufs_write_restore();
	}
	if (ufs_own && ufs_own_write_part >= 0 && (uint32_t)minor(dev) == (uint32_t)ufs_own_write_part && (flags & FWRITE)) {
		int e = ufshc_sync();

		if (e != 0) {
			printf("ufs: own: %s: SYNCHRONIZE CACHE at close failed (%d)\n", ufs_page->part[minor(dev)].name, e);
		}
	}
	return ufs_close(dev, flags, devtype, p);
}

static void
ufs_strategy(buf_t bp)
{
	uint32_t part = (uint32_t)minor(buf_device(bp));
	uint64_t offset, len, size;
	int error;

	if (ufs_is_data(buf_device(bp))) {
		ufs_data_strategy(bp);
		return;
	}
	if (!ufs_ready || part >= ufs_parts) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (!(buf_flags(bp) & B_READ)) {
		nufs_refused("a write to /dev/ufs_%s: a block device of this driver never writes", nufs_pname(part));
		buf_seterror(bp, EROFS);         /* this device never writes */
		buf_biodone(bp);
		return;
	}
	size = ufs_part_bytes(part);
	offset = (uint64_t)buf_blkno(bp) * ufs_bsize[part];
	len = buf_count(bp);
	if (offset >= size) {
		buf_seterror(bp, offset == size ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (offset + len > size) {
		len = size - offset;
	}
	error = ufs_read(part, offset, (uint8_t *)buf_dataptr(bp), len);
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : len)));
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	uint32_t part = (uint32_t)minor(dev), native;

	if (ufs_is_data(dev)) {
		return ufs_data_ioctl(cmd, data);
	}
	if (!ufs_ready || part >= ufs_parts) {
		return ENXIO;
	}
	native = ufs_page->part[part].block_size;
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = ufs_bsize[part];
		return 0;
	case DKIOCSETBLOCKSIZE:
		/*
		 * spec_open() sets the block size to 512 to count the device in 512-byte blocks and
		 * then back, and treats a refusal as a broken device; reads are by byte here, so any
		 * power of two from 512 to the native block size will do.
		 */
		if (*(uint32_t *)data < 512 || *(uint32_t *)data > native ||
		    (*(uint32_t *)data & (*(uint32_t *)data - 1)) != 0) {
			return EINVAL;
		}
		ufs_bsize[part] = *(uint32_t *)data;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = native;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_part_bytes(part) / ufs_bsize[part];
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;  /* the block device, which is what a filesystem would ask */
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = ufs_buffer_size / ufs_bsize[part];
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_psize(dev_t dev)
{
	uint32_t part = (uint32_t)minor(dev);

	if (ufs_is_data(dev)) {
		return (int)ufs_data_bsize;
	}
	return ufs_ready && part < ufs_parts ? (int)ufs_bsize[part] : -1;
}

/*
 * A read of /dev/rufs_NAME: a bufferful at a time into memory of this kernel's and out to the
 * reader with uiomove. Not physio, which hands the driver the reader's own address to copy to --
 * and this kernel may not touch user memory itself (PAN): the copy has to go through uiomove.
 * Nothing of this driver is held while it does, for a fault in the reader's memory can come back
 * here as a read of the storage.
 */
static int
ufs_cread(dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t part = (uint32_t)minor(dev);
	uint64_t size;
	uint8_t *bounce;
	int error = 0;

	if (!ufs_ready || part >= ufs_parts) {
		return ENXIO;
	}
	size = ufs_part_bytes(part);
	if (uio_offset(uio) < 0) {
		return EINVAL;
	}
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		uint64_t at = (uint64_t)uio_offset(uio), want = (uint64_t)uio_resid(uio);

		if (want > ufs_buffer_size) {
			want = ufs_buffer_size;
		}
		if (want > size - at) {
			want = size - at;
		}
		error = ufs_read(part, at, bounce, want);
		if (error == 0) {
			error = uiomove((caddr_t)bounce, (int)want, uio);
		}
	}
	kfree_data(bounce, ufs_buffer_size);
	return error;
}

static int ufs_own_cwrite(uint32_t part, struct uio *uio);

/*
 * A write to /dev/rufs_darwin: whole blocks, out of user memory a bufferful at a time and into the
 * partition. Nothing of this driver is held while user memory is touched -- a fault there can come
 * back as a read of the storage -- so the bytes are taken first and written after.
 */
static int
ufs_cwrite(dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint32_t part = (uint32_t)minor(dev);
	uint64_t nbs;
	uint8_t *bounce;
	int error = 0;

	if (ufs_own) {
		return ufs_own_cwrite(part, uio);
	}
	if (!ufs_ready || part >= ufs_parts || ufs_write_part < 0 || part != (uint32_t)ufs_write_part) {
		nufs_refused("a write to /dev/rufs_%s: %s", nufs_pname(part), ufs_write_part < 0 ?
		    "nothing takes writes in this run" : "not the partition that takes writes");
		return EROFS;
	}
	if (ufs_var_busy_pid != proc_selfpid()) {
		/* one writer of the store at a time, and it says so first (kern.hakios.var_busy) */
		ufs_write_refused++;
		nufs_refused("a write to /dev/rufs_%s by pid %d: not the store's writer (kern.hakios.var_busy)",
		    nufs_pname(part), proc_selfpid());
		return EPERM;
	}
	nbs = ufs_page->part[part].block_size;
	if (uio_offset(uio) < 0 || (uint64_t)uio_offset(uio) % nbs != 0 || (uint64_t)uio_resid(uio) % nbs != 0) {
		return EINVAL;
	}
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0) {
		user_ssize_t want = uio_resid(uio);
		off_t at = uio_offset(uio);

		if (want > (user_ssize_t)ufs_buffer_size) {
			want = (user_ssize_t)ufs_buffer_size;
		}
		error = uiomove((caddr_t)bounce, (int)want, uio);
		if (error == 0) {
			error = ufs_write(BLOCK_OP_WRITE, part, (uint64_t)at, bounce, (uint64_t)want);
			if (error != 0) {
				/* these bytes did not go in: the write is to say so, not to count them */
				uio_setoffset(uio, at);
				uio_setresid(uio, uio_resid(uio) + want);
			}
		}
	}
	kfree_data(bounce, ufs_buffer_size);
	return error;
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_copen,
	.d_close      = ufs_cclose,
	.d_read       = ufs_cread,
	.d_write      = ufs_cwrite,
	.d_ioctl      = ufs_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

/*
 * The orderly end of a run, from osfmk/arm64/loader_return.c, in the thread that is about to hand
 * control back: ask the saver for the data volume and wait for it, so that a run need not have
 * been told to save. Bounded, and only when a saver registered -- the loader goes on feeding the
 * watchdog for a minute past the run's own budget, and the host waits a hundred seconds past it,
 * so twenty seconds here is well inside both. Nothing else of the way out has happened yet: the
 * GPU, the DSPs and this kernel's transfer list are still as the run had them.
 */
void
hakios_var_save_on_return(void)
{
	uint32_t ms = VAR_SAVE_MS;
	uint64_t waited = 0;

	hakios_ufs_summary("the run is ending", HAKIOS_UFS_SAY_LOG | HAKIOS_UFS_SAY_PRINTF | HAKIOS_UFS_SAY_NUFS_ONLY);

	/*
	 * The data volume mounted (/dev/ufs_NAME_data opened for writing): what HFS Plus still holds in memory goes
	 * into the flash before the run hands back, as a shutdown's sync would -- bounded by the kernel's own
	 * sync_timeout_seconds, and whatever it did not reach the journal makes whole at the next mount.
	 */
	if (ufs_ready && ufs_write_part >= 0 && ufs_data_used) {
		int e;

		printf("ufs: the run is ending: the file systems synced, the data volume's last writes into %s\n",
		    (const char *)ufs_page->part[ufs_write_part].name);
		e = sync_internal();
		printf("ufs: synced (%d); %llu writes, %llu MB this run\n", e, ufs_writes, ufs_write_bytes >> 20);
	}
	if (!ufs_ready || ufs_write_part < 0 || ufs_var_save != VAR_SAVE_IDLE) {
		return;
	}
	PE_parse_boot_argn("var_save_ms", &ms, sizeof(ms));
	printf("var_store: the run is ending: the data volume is asked for\n");
	ufs_var_save = VAR_SAVE_WANTED;
	while (ufs_var_save != VAR_SAVE_DONE && waited < ms) {
		IOSleep(20);
		waited += 20;
	}
	if (ufs_var_save == VAR_SAVE_DONE) {
		/* what it made of it is in its own lines: this only says it answered */
		printf("var_store: the saver has finished after %llu ms\n", waited);
	} else {
		printf("var_store: the saver did not answer in %u ms: the store keeps what it had\n", ms);
	}
}

/* ---- the check: the partitions there are images of, hashed as the loader hashes them ---- */

static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static bool
ufs_is_reference(const char *name, const char *ref)
{
	size_t n = strlen(ref);

	return strncmp(name, ref, n) == 0 &&
	       (name[n] == 0 || (name[n] == '_' && (name[n + 1] == 'a' || name[n + 1] == 'b') && name[n + 2] == 0));
}

static void
ufs_check(void *arg, __unused wait_result_t wr)
{
	uint8_t *chunk = kalloc_data(1 << 20, Z_WAITOK);

	/*
	 * Not at once: what the kernel prints in its first minute or so does not come back with the
	 * log on the phone (the first line that does is from long after this driver has started), so
	 * the check waits until it will be read -- unless it runs right at the start (ufs_check=2, then
	 * back to the loader: the log is short).
	 */
	if (arg == NULL) {
		IOSleep(40000);
	}
	printf("ufs: check: %u partitions of the phone's storage, read through %s\n", ufs_parts,
	    ufs_native_on ? "this kernel's own UFS driver" : "the loader's page");
	for (uint32_t part = 0; chunk != NULL && part < ufs_parts; part++) {
		for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
			uint64_t h = 0xcbf29ce484222325ULL, done = 0, started = mach_absolute_time(), ns;
			uint64_t bytes = ufs_references[r].bytes;
			int error = 0;

			if (!ufs_is_reference((const char *)ufs_page->part[part].name, ufs_references[r].name)) {
				continue;
			}
			while (done < bytes && error == 0) {
				uint64_t n = bytes - done < (1 << 20) ? bytes - done : (1 << 20);

				error = ufs_read(part, done, chunk, n);
				for (uint64_t i = 0; error == 0 && i < n; i++) {
					h = (h ^ chunk[i]) * 0x100000001b3ULL;
				}
				done += n;
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
			if (error) {
				printf("ufs: check %s: read failed (%d)\n", ufs_page->part[part].name, error);
			} else {
				printf("ufs: check %s: fnv %llx over %llu bytes, %llu ms\n", ufs_page->part[part].name,
				    h, bytes, ns / 1000000);
			}
		}
	}
	if (chunk != NULL) {
		kfree_data(chunk, 1 << 20);
	}
	printf("ufs: check done: %llu requests, %llu errors, %llu bytes, %llu ms waiting\n",
	    ufs_requests, ufs_errors, ufs_bytes, ufs_wait_us / 1000);
}

/*
 * ufs_bench=1: what one request through the loader costs, by size. The partition "darwin" is
 * read from the front, a different stretch for every size so that nothing the storage itself may
 * have kept is read twice; for each size, MB/s and microseconds a request, split into the time
 * core 0 spent in ReadBlocks (its clock in the page, read before and after), the rest of waiting
 * for its answer (noticing the request, answering), and copying out of its buffer.
 */
static void
ufs_bench(__unused void *arg, __unused wait_result_t wr)
{
	static const uint32_t sizes[] = { 4096, 16384, 65536, 262144, 1048576, 4194304 };
	uint8_t *dst = kalloc_data(ufs_buffer_size, Z_WAITOK);
	uint32_t part = BLOCK_MAX_PARTS;
	uint64_t base = 0;

	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (strcmp((const char *)ufs_page->part[i].name, "darwin") == 0) {
			part = i;
		}
	}
	if (dst == NULL || part == BLOCK_MAX_PARTS) {
		printf("ufs: bench: no partition darwin, or no memory\n");
		return;
	}
	for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
		uint64_t total = sizes[z] <= 16384 ? (16ULL << 20) : (128ULL << 20), done = 0, n = 0;
		uint64_t wait0 = ufs_wait_us, copy0 = ufs_copy_us, loader0 = ufs_page->answer.ticks;
		uint64_t started = mach_absolute_time(), ns, loader_ns;
		int error = 0;

		while (done < total && error == 0) {
			error = ufs_read(part, base + done, dst, sizes[z]);
			done += sizes[z];
			n++;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
		absolutetime_to_nanoseconds(ufs_page->answer.ticks - loader0, &loader_ns);
		printf("ufs: bench: %7u bytes a request: %4llu MB/s, %llu requests, %llu us each: "
		    "ReadBlocks %llu, the rest of waiting %llu, copying %llu%s\n", sizes[z],
		    ns ? (done * 1000ULL) / ns : 0, n, ns / 1000 / n, loader_ns / 1000 / n,
		    (ufs_wait_us - wait0 - loader_ns / 1000) / n, (ufs_copy_us - copy0) / n,
		    error ? " (an error)" : "");
		base += total;
	}
	kfree_data(dst, ufs_buffer_size);
	hakios_ufs_report("after the bench");
}

/* ---- this kernel's own driver: which unit is which, and whether it reads what the loader reads ---- */

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

static uint32_t
ufs_entry(const char *name)
{
	for (uint32_t i = 0; i < ufs_parts; i++) {
		if (strcmp((const char *)ufs_page->part[i].name, name) == 0) {
			return i;
		}
	}
	return BLOCK_MAX_PARTS;
}

/*
 * For the kernel's own drivers (the modem's file systems, drivers/modem/hakios.mpss.c): bytes
 * [offset, offset + len) of the partition of that name (or of its _a or _b slot), and how big it
 * is. ENXIO without the storage, ENOENT without the partition; nothing is written.
 */
int
hakios_ufs_read_partition(const char *name, uint64_t offset, uint8_t *dst, uint64_t len, uint64_t *bytes)
{
	uint32_t part;

	if (ufs_page == NULL) {
		return ENXIO;
	}
	part = ufs_entry(name);
	for (const char *slot = "_a"; part == BLOCK_MAX_PARTS && slot[0] != 0; slot = slot[1] == 'a' ? "_b" : "") {
		char slotted[48];

		snprintf(slotted, sizeof(slotted), "%s%s", name, slot);
		part = ufs_entry(slotted);
	}
	if (part == BLOCK_MAX_PARTS) {
		return ENOENT;
	}
	if (bytes != NULL) {
		*bytes = ufs_part_bytes(part);
	}
	if (len == 0) {
		return 0;
	}
	/* (written without a sum: offset + len wraps for an offset near 2^64) */
	if (offset > ufs_part_bytes(part) || len > ufs_part_bytes(part) - offset) {
		return EINVAL;
	}
	return ufs_read(part, offset, dst, len);
}

/*
 * Which logical unit each unit the loader lists is (their device paths do not say): unit k is
 * taken to be LUN k (ordinary logical units only: never a well-known one), and it is so only if
 * block 1 (the GPT header, with the unit's own GUID in it) read through the loader
 * is the same as block 1 of that LUN read here; no other LUN is read. The loader's reads are made
 * first, with UEFI's list in; then this kernel's list goes in (with its proof) and stays in.
 */
static bool
ufs_native_map(bool all_units)
{
	uint8_t *mine = kalloc_data(4096, Z_WAITOK), *theirs = kalloc_data(BLOCK_MAX_UNITS * 4096, Z_WAITOK);
	uint32_t units[BLOCK_MAX_UNITS], n = 0, mask = 0, darwin = ufs_entry("darwin");
	bool good = mine != NULL && theirs != NULL;

	memset(ufs_lun, 0xff, sizeof(ufs_lun));
	for (uint32_t i = 0; good && i < ufs_parts && n < BLOCK_MAX_UNITS; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];

		if ((e->flags & BLOCK_PART_UNIT) == 0 || e->unit >= 8) {
			continue;
		}
		if (!all_units && (darwin == BLOCK_MAX_PARTS || ufs_page->part[darwin].unit != e->unit)) {
			continue;
		}
		if (e->block_size != 4096 || ufs_read_mailbox(i, 4096, theirs + n * 4096, 4096) != 0 ||
		    memcmp(theirs + n * 4096, "EFI PART", 8) != 0) {
			printf("ufs: native: %s: no GPT header to know it by (block size %u)\n", e->name, e->block_size);
			good = false;
			break;
		}
		units[n++] = i;
		mask |= 1u << e->unit;
		ufs_native_luns = n;
	}
	if (good && !all_units && (n != 1 || ufs_page->part[units[0]].unit != 0)) {
		printf("ufs: native: darwin is not on the first unit the loader lists: not LUN 0, not read here\n");
		good = false;
	}
	ufshc_set_luns(mask);
	if (good && ufs_native_in(true) != 0) {
		good = false;
	}
	for (uint32_t k = 0; good && k < n; k++) {
		volatile struct loader_block_part *e = &ufs_page->part[units[k]];

		if (ufshc_read((uint8_t)e->unit, 4096, mine, 4096) != 0 || memcmp(mine, theirs + k * 4096, 4096) != 0) {
			printf("ufs: native: %s: LUN %u's block 1 is NOT what the loader reads as its block 1\n", e->name, e->unit);
			good = false;
		} else {
			printf("ufs: native: %s is LUN %u: its GPT header read both ways is the same\n", e->name, e->unit);
		}
	}
	if (!good) {
		ufs_native_out("the units could not be told");
		memset(ufs_lun, 0xff, sizeof(ufs_lun));
	} else {
		for (uint32_t i = 0; i < ufs_parts; i++) {
			volatile struct loader_block_part *e = &ufs_page->part[i];

			if (e->unit < 8 && (mask & (1u << e->unit)) && e->block_size == 4096) {
				ufs_lun[i] = (uint8_t)e->unit;
				ufs_lun_offset[i] = e->start_lba * 4096;
			}
		}
	}
	if (mine != NULL) {
		kfree_data(mine, 4096);
	}
	if (theirs != NULL) {
		kfree_data(theirs, BLOCK_MAX_UNITS * 4096);
	}
	return good;
}

/*
 * ufs_native_check=1: what this kernel's own driver reads is what the loader reads, byte for
 * byte. Spans are read a batch at a time, first all through the loader (UEFI's list in), then
 * all here (this kernel's list in, proved), and compared.
 */
struct ufs_span {
	uint32_t part;
	uint64_t offset, len;
};

#define UFS_CMP_BUFFER      (32u << 20)

static uint8_t *ufs_cmp_x, *ufs_cmp_y;
static uint64_t ufs_cmp_mailbox_ns, ufs_cmp_native_ns, ufs_cmp_bytes, ufs_cmp_spans, ufs_cmp_differ, ufs_cmp_failed;

static bool
ufs_compare_batch(const struct ufs_span *s, uint32_t n, const char *what, bool quiet)
{
	uint64_t at, t, ns, hx = 0xcbf29ce484222325ULL, hy = hx, bytes = 0, mailbox_ns, native_ns;
	bool same = true;
	int e = 0;

	t = mach_absolute_time();
	if (ufs_native_out("the loader reads, to compare") != 0) {
		e = EIO;
	}
	at = 0;
	for (uint32_t i = 0; e == 0 && i < n; at += s[i].len, i++) {
		e = ufs_read_mailbox(s[i].part, s[i].offset, ufs_cmp_x + at, s[i].len);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t, &mailbox_ns);
	t = mach_absolute_time();
	if (e == 0 && ufs_native_in(true) != 0) {
		e = EIO;
	}
	at = 0;
	for (uint32_t i = 0; e == 0 && i < n; at += s[i].len, i++) {
		e = ufs_read_native(s[i].part, s[i].offset, ufs_cmp_y + at, s[i].len);
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t, &native_ns);
	ufs_cmp_mailbox_ns += mailbox_ns;
	ufs_cmp_native_ns += native_ns;
	if (e != 0) {
		printf("ufs: native check: %s: a read failed (%d): STOP\n", what, e);
		ufs_cmp_failed++;
		return false;
	}
	at = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (memcmp(ufs_cmp_x + at, ufs_cmp_y + at, s[i].len) != 0) {
			uint64_t k = 0;

			while (ufs_cmp_x[at + k] == ufs_cmp_y[at + k]) {
				k++;
			}
			printf("ufs: native check: %s %s: DIFFERENT at byte %llu (%02x through the loader, %02x here): STOP\n",
			    ufs_page->part[s[i].part].name, what, s[i].offset + k, ufs_cmp_x[at + k], ufs_cmp_y[at + k]);
			same = false;
			ufs_cmp_differ++;
		}
		hx = ufs_fnv(hx, ufs_cmp_x + at, s[i].len);
		hy = ufs_fnv(hy, ufs_cmp_y + at, s[i].len);
		at += s[i].len;
		bytes += s[i].len;
	}
	ufs_cmp_spans += n;
	ufs_cmp_bytes += bytes;
	if (!quiet || !same) {
		printf("ufs: native check: %s %s: %u span(s), %llu bytes from %llu: %s (fnv %llx / %llx); loader %llu ms, here "
		    "%llu ms (switches included)\n", ufs_page->part[s[0].part].name, what, n, bytes, s[0].offset,
		    same ? "the same" : "NOT THE SAME", hx, hy, mailbox_ns / 1000000, native_ns / 1000000);
	}
	return same;
}

/* Bytes [offset, offset + len) of an entry, compared a buffer at a time; FNV-1a of the first ref bytes too */
static bool
ufs_compare(uint32_t part, uint64_t offset, uint64_t len, uint64_t ref, const char *what)
{
	uint64_t done = 0, href = 0xcbf29ce484222325ULL;
	bool same = true;

	while (done < len && same) {
		struct ufs_span s = { part, offset + done, len - done < UFS_CMP_BUFFER ? len - done : UFS_CMP_BUFFER };

		same = ufs_compare_batch(&s, 1, what, true);
		if (same && ref > done) {
			href = ufs_fnv(href, ufs_cmp_x, ref - done < s.len ? ref - done : s.len);
		}
		done += s.len;
	}
	printf("ufs: native check: %s %s: %llu bytes from %llu: %s%s", ufs_page->part[part].name, what, done, offset,
	    same ? "the same" : "NOT THE SAME", ref ? "" : "\n");
	if (ref) {
		printf("; fnv %llx over the first %llu bytes (the loader's own survey hashes them)\n", href, ref);
	}
	return same;
}

static void
ufs_native_check(void)
{
	uint8_t *h = kalloc_data(4096, Z_WAITOK);
	uint32_t part;
	bool going = true;

	ufs_cmp_x = kalloc_data(UFS_CMP_BUFFER, Z_WAITOK);
	ufs_cmp_y = kalloc_data(UFS_CMP_BUFFER, Z_WAITOK);
	if (h == NULL || ufs_cmp_x == NULL || ufs_cmp_y == NULL) {
		printf("ufs: native check: no memory\n");
		/* what was got is given back: the early return would leak it */
		if (h != NULL) {
			kfree_data(h, 4096);
		}
		if (ufs_cmp_x != NULL) {
			kfree_data(ufs_cmp_x, UFS_CMP_BUFFER);
			ufs_cmp_x = NULL;
		}
		if (ufs_cmp_y != NULL) {
			kfree_data(ufs_cmp_y, UFS_CMP_BUFFER);
			ufs_cmp_y = NULL;
		}
		return;
	}
	printf("ufs: native check: through the loader and through this kernel's own driver, compared\n");
	/* every unit's GPT: protective MBR, header, table; the backup table and header at the end */
	for (uint32_t i = 0; going && i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		uint64_t entries_lba, count, size, table, alt, alt_entries;
		uint32_t want, got;

		if ((e->flags & BLOCK_PART_UNIT) == 0 || ufs_lun[i] == 0xff) {
			continue;
		}
		if (ufs_native_out("the loader reads, to compare") != 0 || ufs_read_mailbox(i, 4096, h, 4096) != 0) {
			going = false;
			break;
		}
		entries_lba = *(uint64_t *)(h + 72);
		count = *(uint32_t *)(h + 80);
		size = *(uint32_t *)(h + 84);
		alt = *(uint64_t *)(h + 32);
		table = (count * size + 4095) / 4096;
		want = *(uint32_t *)(h + 16);
		*(uint32_t *)(h + 16) = 0;
		/* the header size comes from the flash: crc32 over more than the 4096 bytes read would run off the buffer */
		if (*(uint32_t *)(h + 12) >= 92 && *(uint32_t *)(h + 12) <= 4096) {
			got = crc32(0, h, *(uint32_t *)(h + 12));
		} else {
			got = ~want;                    /* (reported as WRONG below) */
		}
		printf("ufs: native check: %s (LUN %u): GPT header crc %s (%x), table at block %llu, %llu entries of %llu bytes, "
		    "backup header at block %llu\n", e->name, ufs_lun[i], got == want ? "right" : "WRONG", got, entries_lba, count,
		    size, alt);
		going = ufs_compare(i, 0, (entries_lba + table) * 4096, 0, "GPT (MBR, header, table)");
		if (!going || alt >= e->blocks || ufs_native_out("the loader reads, to compare") != 0 ||
		    ufs_read_mailbox(i, alt * 4096, h, 4096) != 0 || memcmp(h, "EFI PART", 8) != 0 ||
		    (alt_entries = *(uint64_t *)(h + 72)) > alt) {
			printf("ufs: native check: %s: no backup GPT header at block %llu: STOP\n", e->name, alt);
			going = false;
			break;
		}
		going = ufs_compare(i, alt_entries * 4096, (alt + 1 - alt_entries) * 4096, 0, "backup GPT (table, header)");
	}
	/* the partitions there are images of, whole */
	for (part = 0; going && part < ufs_parts; part++) {
		if ((ufs_page->part[part].flags & BLOCK_PART_UNIT) || ufs_lun[part] == 0xff) {
			continue;
		}
		for (unsigned r = 0; going && r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
			if (ufs_is_reference((const char *)ufs_page->part[part].name, ufs_references[r].name)) {
				going = ufs_compare(part, 0, ufs_part_bytes(part), ufs_references[r].bytes, "whole");
			}
		}
	}
	/* super's geometry and metadata */
	if (going && (part = ufs_entry("super")) < BLOCK_MAX_PARTS && ufs_lun[part] != 0xff) {
		going = ufs_compare(part, 0, 8 << 20, 0, "first 8 MB (geometry, metadata)");
	}
	/* darwin: 32 spread megabytes, 256 scattered blocks, 32 pieces that begin and end inside a block */
	if (going && (part = ufs_entry("darwin")) < BLOCK_MAX_PARTS && ufs_lun[part] != 0xff) {
		struct ufs_span s[256];
		uint64_t bytes = ufs_part_bytes(part), seed = 0x9e3779b97f4a7c15ULL, total = 0;

		for (uint32_t k = 0; k < 32; k++) {
			s[k] = (struct ufs_span){ part, (bytes - (1 << 20)) / 32 * k / 4096 * 4096, 1 << 20 };
		}
		going = ufs_compare_batch(s, 32, "spread megabytes", false);
		for (uint32_t k = 0; k < 256; k++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			s[k] = (struct ufs_span){ part, (seed >> 16) % (bytes / 4096) * 4096, 4096 };
		}
		going = going && ufs_compare_batch(s, 256, "scattered blocks", false);
		for (uint32_t k = 0; k < 32; k++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			s[k] = (struct ufs_span){ part, (seed >> 16) % (bytes - (2 << 20)), 1 + (seed >> 40) % (512 << 10) };
			total += s[k].len;
		}
		going = going && ufs_compare_batch(s, 32, "unaligned pieces", false);
	}
	kfree_data(h, 4096);
	printf("ufs: native check %s: %llu spans, %llu bytes, %llu NOT THE SAME, %llu failed reads; through the loader %llu ms, "
	    "through this kernel's own driver %llu ms (list switches included)\n", going ? "done" : "STOPPED", ufs_cmp_spans,
	    ufs_cmp_bytes, ufs_cmp_differ, ufs_cmp_failed, ufs_cmp_mailbox_ns / 1000000, ufs_cmp_native_ns / 1000000);
	ufs_native_out("after the check");
	ufshc_report("after the check");
}

/*
 * ufs_native_bench=1 (with ufs_native=1): what the flash gives with N reads in flight -- N
 * threads each reading at random places of darwin, a size at a time -- MB/s and microseconds a
 * read, for 1, 2, 4, 8, 16 and 31 threads. Then back to the loader.
 */
struct ufs_nb {
	uint32_t part, size, per_thread;
	uint64_t span, seed;
	volatile uint32_t left;
	int error;
};

static void
ufs_nb_thread(void *arg, __unused wait_result_t wr)
{
	struct ufs_nb *nb = arg;
	uint8_t *buf = kalloc_data(nb->size, Z_WAITOK);
	uint64_t seed = os_atomic_add(&nb->seed, 0x9e3779b97f4a7c15ULL, relaxed);

	for (uint32_t i = 0; buf != NULL && i < nb->per_thread; i++) {
		int e;

		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		e = ufs_read_native(nb->part, (seed >> 20) % (nb->span / nb->size) * nb->size, buf, nb->size);
		if (e != 0) {
			nb->error = e;
		}
	}
	if (buf != NULL) {
		kfree_data(buf, nb->size);
	}
	lck_mtx_lock(&ufs_lock);
	nb->left--;
	wakeup(nb);
	lck_mtx_unlock(&ufs_lock);
}

static void
ufs_native_bench(void)
{
	static const uint32_t sizes[] = { 4096, 32768, 131072, 524288 };
	static const uint32_t threads[] = { 1, 2, 4, 8, 16, 31 };
	uint32_t part = ufs_entry("darwin");

	if (part >= BLOCK_MAX_PARTS || ufs_lun[part] == 0xff) {
		printf("ufs: native bench: no darwin\n");
		return;
	}
	for (unsigned z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
		for (unsigned t = 0; t < sizeof(threads) / sizeof(threads[0]); t++) {
			struct ufs_nb nb = {
				.part = part, .size = sizes[z], .span = ufs_part_bytes(part),
				.per_thread = (sizes[z] <= 32768 ? 8192 : 2048) / threads[t] + 1, .seed = 12345 + z * 100 + t,
			};
			uint64_t started = mach_absolute_time(), ns, bytes;

			nb.left = threads[t];
			for (uint32_t i = 0; i < threads[t]; i++) {
				thread_t th;

				if (kernel_thread_start(ufs_nb_thread, &nb, &th) == KERN_SUCCESS) {
					thread_deallocate(th);
				} else {
					lck_mtx_lock(&ufs_lock);
					nb.left--;
					lck_mtx_unlock(&ufs_lock);
				}
			}
			lck_mtx_lock(&ufs_lock);
			while (nb.left > 0) {
				msleep(&nb, &ufs_lock, PRIBIO, "ufsbench", NULL);
			}
			lck_mtx_unlock(&ufs_lock);
			absolutetime_to_nanoseconds(mach_absolute_time() - started, &ns);
			bytes = (uint64_t)nb.per_thread * threads[t] * sizes[z];
			printf("ufs: native bench: %6u bytes, %2u in flight: %4llu MB/s, %6llu reads/s, %5llu us a read%s\n",
			    sizes[z], threads[t], ns ? bytes * 1000 / ns : 0, ns ? (bytes / sizes[z]) * 1000000000ULL / ns : 0,
			    ns / 1000 * threads[t] / (bytes / sizes[z]), nb.error ? " (ERRORS)" : "");
		}
	}
	ufshc_report("after the bench");
}

/* kern.hakios.ufs_native: 1 while reads go through this kernel's own driver; 0 or 1 may be set */
static int
ufs_sysctl_native(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = ufs_native_on, error, changed = 0;

	error = sysctl_io_number(req, value, sizeof(value), &value, &changed);
	if (error != 0 || !changed) {
		return error;
	}
	if (ufs_own) {
		return value != 0 ? 0 : EPERM;  /* own mode: there is no other way to the storage */
	}
	if (value != 0) {
		if (!(ufs_native_mapped && ufshc_available()) || ufs_native_in(true) != 0) {
			return ENXIO;
		}
		ufs_native_on = true;
	} else {
		ufs_native_on = false;
		ufs_native_out("kern.hakios.ufs_native=0");
	}
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, ufs_native, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    ufs_sysctl_native, "I", "1: the phone's storage is read by this kernel's own UFS driver, 0: through the loader");
SYSCTL_QUAD(_kern_hakios, OID_AUTO, ufs_native_fallbacks, CTLFLAG_RD | CTLFLAG_LOCKED, &ufs_native_fallbacks,
    "times a read of this kernel's own UFS driver failed and the loader's was used instead");

/* ---- own mode (ufs_native=4): the GPTs read here, one partition that takes writes ---- */

/*
 * Never written, whatever ufs_native_write= says. The Galaxy S9's: EFS (the IMEI and the radio's
 * calibration: losing it is permanent), SEC_EFS, BOOT, RECOVERY, PARAM -- and with them, as a
 * precaution, the other partitions of a Samsung phone's boot chain and device-unique data. The
 * POCO's: its modem file systems, persist and the boot chain's images. Compared without case; a
 * slot suffix (_a, _b) does not make a name another. Any name with "efs" in it is refused too.
 */
static const char *const ufs_never_written[] = {
	"EFS", "SEC_EFS", "CPEFS", "BOOT", "RECOVERY", "PARAM", "UP_PARAM", "BOTA0", "BOTA1", "RADIO", "CP_DEBUG",
	"PERSISTENT", "STEADY", "KEYSTORAGE", "KEYREFUGE", "DNT", "NAD_REFER", "DQMDBG", "VBMETA", "DTBO", "DTB",
	"modemst1", "modemst2", "fsg", "fsc", "persist", "modem", "xbl", "xbl_config", "abl", "aop", "tz", "hyp",
	"devcfg", "keymaster", "cmnlib", "cmnlib64", "qupfw", "uefisecapp", "imagefv", "shrm", "cpucp", "vendor_boot",
	"init_boot", "vbmeta_system", "frp", "devinfo", "secdata", "logdump",
};

static bool
ufs_name_is(const char *name, const char *ref)
{
	size_t n = strlen(ref);

	if (strncasecmp(name, ref, n) != 0) {
		return false;
	}
	return name[n] == 0 || (name[n] == '_' && (name[n + 1] == 'a' || name[n + 1] == 'b') && name[n + 2] == 0);
}

/* why the partition may not take writes; NULL: it may */
static const char *
ufs_own_unwritable(uint32_t part)
{
	const char *name = (const char *)ufs_page->part[part].name;
	const struct ufshc_unit *u = ufshc_unit((uint8_t)ufs_page->part[part].unit);

	for (unsigned i = 0; i < sizeof(ufs_never_written) / sizeof(ufs_never_written[0]); i++) {
		if (ufs_name_is(name, ufs_never_written[i])) {
			return "it is one that is never written";
		}
	}
	for (const char *c = name; *c != 0; c++) {
		if (strncasecmp(c, "efs", 3) == 0) {
			return "its name has \"efs\" in it";
		}
	}
	if (ufs_page->part[part].flags & BLOCK_PART_UNIT) {
		return "it is a whole logical unit";
	}
	if (u == NULL || u->boot_lun_id != 0 || u->write_protect != 0) {
		return "its logical unit is a boot unit, write protected, or not there";
	}
	if (ufs_page->part[part].block_size != 4096) {
		return "its blocks are not 4096 bytes";
	}
	return NULL;
}

/* Whole blocks [offset, offset + len) of the partition that takes writes, from src. */
static int
ufs_own_write(uint32_t part, uint64_t offset, const uint8_t *src, uint64_t len)
{
	uint64_t size;

	if (!ufs_own || ufs_own_write_part < 0 || part != (uint32_t)ufs_own_write_part) {
		ufs_write_refused++;
		return EROFS;
	}
	size = ufs_part_bytes(part);
	if (len == 0 || offset % 4096 != 0 || len % 4096 != 0) {
		return EINVAL;
	}
	if (offset > size || len > size - offset) {
		ufs_write_refused++;
		return EROFS;
	}
	if (ufshc_write(ufs_lun[part], ufs_lun_offset[part] + offset, src, len) != 0) {
		ufs_write_errors++;
		return EIO;
	}
	ufs_writes++;
	ufs_write_bytes += len;
	return 0;
}

/* A write to /dev/rufs_NAME in own mode: whole blocks, out of user memory a megabyte at a time. */
static int
ufs_own_cwrite(uint32_t part, struct uio *uio)
{
	uint8_t *bounce;
	int error = 0;

	if (!ufs_ready || part >= ufs_parts || ufs_own_write_part < 0 || part != (uint32_t)ufs_own_write_part) {
		ufs_write_refused++;
		return EROFS;
	}
	if (uio_offset(uio) < 0 || (uint64_t)uio_offset(uio) % 4096 != 0 || (uint64_t)uio_resid(uio) % 4096 != 0) {
		return EINVAL;
	}
	bounce = kalloc_data(ufs_buffer_size, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0) {
		user_ssize_t want = uio_resid(uio);
		off_t at = uio_offset(uio);

		if (want > (user_ssize_t)ufs_buffer_size) {
			want = (user_ssize_t)ufs_buffer_size;
		}
		error = uiomove((caddr_t)bounce, (int)want, uio);
		if (error == 0) {
			lck_mtx_lock(&ufs_write_mtx);
			error = ufs_own_write(part, (uint64_t)at, bounce, (uint64_t)want);
			lck_mtx_unlock(&ufs_write_mtx);
			if (error != 0) {
				uio_setoffset(uio, at);
				uio_setresid(uio, uio_resid(uio) + want);
			}
		}
	}
	kfree_data(bounce, ufs_buffer_size);
	return error;
}

static uint32_t
ufs_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
ufs_le64(const uint8_t *p)
{
	return (uint64_t)ufs_le32(p) | (uint64_t)ufs_le32(p + 4) << 32;
}

/*
 * One logical unit's GPT, read with the controller's driver, into the table: its partitions,
 * then the unit itself. Refused whole (and the unit left out) on anything not right.
 */
static const char *
ufs_own_gpt(uint8_t lun, const struct ufshc_unit *u, uint32_t *added)
{
	uint8_t *h = kalloc_data(4096, Z_WAITOK), *t = NULL;
	uint32_t count, esize, hsize, crc, tblocks, first_new = ufs_parts;
	uint64_t elba, first, last, backup;
	const char *why = NULL;

	*added = 0;
	if (h == NULL) {
		return "no memory";
	}
	if (ufshc_read(lun, 4096, h, 4096) != 0) {
		why = "its block 1 could not be read";
		goto out;
	}
	hsize = ufs_le32(h + 12);
	count = ufs_le32(h + 80);
	esize = ufs_le32(h + 84);
	elba = ufs_le64(h + 72);
	first = ufs_le64(h + 40);
	last = ufs_le64(h + 48);
	backup = ufs_le64(h + 32);
	if (memcmp(h, "EFI PART", 8) != 0) {
		why = "no GPT header";
		goto out;
	}
	if (ufs_le32(h + 8) != 0x00010000 || hsize < 92 || hsize > 4096 || ufs_le64(h + 24) != 1) {
		why = "the header's revision, size or own LBA";
		goto out;
	}
	crc = ufs_le32(h + 16);
	*(uint32_t *)(h + 16) = 0;
	if (crc32(0, h, hsize) != crc) {
		why = "the header's CRC";
		goto out;
	}
	tblocks = (count * 128 + 4095) / 4096;
	if (esize != 128 || count == 0 || count > 256 || elba < 2 || first < elba + tblocks || last < first ||
	    last >= u->blocks || backup <= last || backup >= u->blocks) {
		why = "its geometry (entries, usable range, backup header)";
		goto out;
	}
	t = kalloc_data(tblocks * 4096, Z_WAITOK);
	if (t == NULL || ufshc_read(lun, elba * 4096, t, tblocks * 4096) != 0) {
		why = "its partition table could not be read";
		goto out;
	}
	if (crc32(0, t, count * 128) != ufs_le32(h + 88)) {
		why = "the partition table's CRC";
		goto out;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = t + 128 * i;
		struct loader_block_part *bp;
		uint64_t a = ufs_le64(e + 32), b = ufs_le64(e + 40);
		bool used = false;
		char name[36];
		uint32_t n = 0;

		for (int k = 0; k < 16; k++) {
			used |= e[k] != 0;
		}
		if (!used) {
			continue;
		}
		if (a < first || b < a || b > last) {
			why = "a partition outside the usable range";
			goto out;
		}
		for (uint32_t j = 0; j < i; j++) {
			const uint8_t *o = t + 128 * j;
			bool other = false;

			for (int k = 0; k < 16; k++) {
				other |= o[k] != 0;
			}
			if (other && a <= ufs_le64(o + 40) && ufs_le64(o + 32) <= b) {
				why = "two partitions overlap";
				goto out;
			}
		}
		for (; n < 35 && (e[56 + 2 * n] | e[57 + 2 * n]) != 0; n++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * n] | e[57 + 2 * n] << 8);

			name[n] = ch < 0x80 ? (char)ch : '?';
		}
		name[n] = 0;
		if (n == 0) {
			printf("ufs: own: LUN %u: a partition with no name (blocks %llu..%llu): left out\n", lun, a, b);
			continue;
		}
		if (ufs_entry(name) != BLOCK_MAX_PARTS) {
			printf("ufs: own: LUN %u: a second partition named %s: left out\n", lun, name);
			continue;
		}
		if (ufs_parts + 1 >= BLOCK_MAX_PARTS) {
			why = "more partitions than the table holds";
			goto out;
		}
		bp = &ufs_own_table->part[ufs_parts];
		strlcpy(bp->name, name, sizeof(bp->name));
		bp->block_size = 4096;
		bp->blocks = b - a + 1;
		bp->start_lba = a;
		bp->unit = lun;
		bp->lun = lun;
		ufs_lun[ufs_parts] = lun;
		ufs_lun_offset[ufs_parts] = a * 4096;
		ufs_parts++;
		(*added)++;
	}
	{
		struct loader_block_part *bp = &ufs_own_table->part[ufs_parts];

		snprintf(bp->name, sizeof(bp->name), "unit%u", lun);
		bp->block_size = 4096;
		bp->blocks = u->blocks;
		bp->unit = lun;
		bp->lun = lun;
		bp->flags = BLOCK_PART_UNIT;
		ufs_lun[ufs_parts] = lun;
		ufs_lun_offset[ufs_parts] = 0;
		ufs_parts++;
	}
	printf("ufs: own: LUN %u: GPT right (header and table CRCs, %u entries, usable %llu..%llu, backup at %llu): %u "
	    "partitions\n", lun, count, first, last, backup, *added);
out:
	if (why != NULL) {
		ufs_parts = first_new;          /* (none of this unit's is kept) */
		*added = 0;
	}
	if (t != NULL) {
		kfree_data(t, tblocks * 4096);
	}
	kfree_data(h, 4096);
	return why;
}

/*
 * ufs_native_test=1 (own mode): the refusals that must happen, then, if a partition takes
 * writes, a pattern written, read back, and what was there put back and read back, at its
 * first blocks, its middle and its last blocks.
 */
static void
ufs_own_test(uint32_t mode)
{
	uint8_t *saved = kalloc_data(64 << 10, Z_WAITOK), *pattern = kalloc_data(64 << 10, Z_WAITOK);
	uint8_t *back = kalloc_data(64 << 10, Z_WAITOK);
	uint32_t refused = 0, wrong = 0, places = 0;

	if (saved == NULL || pattern == NULL || back == NULL) {
		printf("ufs: own test: no memory\n");
		goto out;
	}
	bzero(pattern, 64 << 10);
	/* 1. every partition that is not the one: a write refused by the partition layer */
	for (uint32_t i = 0; i < ufs_parts; i++) {
		int e;

		if ((int)i == ufs_own_write_part || (ufs_page->part[i].flags & BLOCK_PART_UNIT)) {
			continue;
		}
		e = ufs_own_write(i, 0, pattern, 4096);
		printf("ufs: own test: a write to %s: %s (%d)\n", ufs_page->part[i].name, e == EROFS ? "refused, as it should be" :
		    "NOT REFUSED", e);
		e == EROFS ? refused++ : wrong++;
	}
	if (ufs_own_write_part >= 0) {
		uint32_t wp = (uint32_t)ufs_own_write_part;
		uint64_t size = ufs_part_bytes(wp), first = ufs_page->part[wp].start_lba, blocks = ufs_page->part[wp].blocks;
		uint8_t lun = ufs_lun[wp];
		const uint64_t at[3] = { 0, size / 2 / 4096 * 4096, size - (64 << 10) };
		int e;

		/* 2. past its end, by the partition layer; around it, by the controller's own whitelist */
		e = ufs_own_write(wp, size, pattern, 4096);
		printf("ufs: own test: a write past the end of %s: %s (%d)\n", ufs_page->part[wp].name, e != 0 ? "refused" :
		    "NOT REFUSED", e);
		e != 0 ? refused++ : wrong++;
		e = ufs_own_write(wp, size - 4096, pattern, 8192);
		printf("ufs: own test: a write across the end of %s: %s (%d)\n", ufs_page->part[wp].name, e != 0 ? "refused" :
		    "NOT REFUSED", e);
		e != 0 ? refused++ : wrong++;
		{
			const struct {
				uint8_t lun;
				uint64_t lba;
				const char *what;
			} probe[] = {
				{ lun, first - 1, "the block before it" }, { lun, first + blocks, "the block after it" },
				{ (uint8_t)(lun ^ 1), first, "its first block's number on the other unit" }, { lun, 1, "the GPT header" },
			};

			for (unsigned k = 0; k < sizeof(probe) / sizeof(probe[0]); k++) {
				e = ufshc_selftest_refusal(probe[k].lun, probe[k].lba);
				printf("ufs: own test: the controller's whitelist on a write of %s (LUN %u, block %llu): %s\n",
				    probe[k].what, probe[k].lun, probe[k].lba, e == EPERM ? "refused, as it should be" : "LET THROUGH");
				e == EPERM ? refused++ : wrong++;
			}
			e = ufshc_selftest_refusal(lun, first + blocks - 1);
			printf("ufs: own test: ... and on its own last block: %s\n", e == 0 ? "let through, as it should be" :
			    "REFUSED");
			e == 0 ? (void)0 : (void)wrong++;
		}
		/* 3. write, read back, put back, read back */
		for (unsigned k = 0; k < 3 && size >= (192 << 10); k++) {
			uint64_t seed = 0x5c10a + k * 7919;
			bool same;

			for (uint32_t i = 0; i < (64 << 10); i++) {
				seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
				pattern[i] = (uint8_t)(seed >> 33);
			}
			if (ufs_read(wp, at[k], saved, 64 << 10) != 0 || ufs_own_write(wp, at[k], pattern, 64 << 10) != 0 ||
			    ufs_read(wp, at[k], back, 64 << 10) != 0) {
				printf("ufs: own test: %s, 64 KB at %llu: a read or the write FAILED\n", ufs_page->part[wp].name, at[k]);
				wrong++;
				continue;
			}
			same = memcmp(back, pattern, 64 << 10) == 0;
			if (ufs_own_write(wp, at[k], saved, 64 << 10) != 0 || ufs_read(wp, at[k], back, 64 << 10) != 0) {
				printf("ufs: own test: %s, 64 KB at %llu: putting it back FAILED\n", ufs_page->part[wp].name, at[k]);
				wrong++;
				continue;
			}
			printf("ufs: own test: %s, 64 KB at byte %llu (LUN %u blocks %llu..%llu): written, read back %s; put back, "
			    "read back %s\n", ufs_page->part[wp].name, at[k], lun, first + at[k] / 4096, first + at[k] / 4096 + 15,
			    same ? "the same" : "DIFFERENT", memcmp(back, saved, 64 << 10) == 0 ? "as it was" : "NOT AS IT WAS");
			same && memcmp(back, saved, 64 << 10) == 0 ? places++ : wrong++;
		}
		if (mode == 2) {
			/* 4. one block left written, for the computer to find in the image after the run */
			bzero(pattern, 4096);
			snprintf((char *)pattern, 4096, "SCION-UFS-OWN-WRITE %s LUN %u block %llu", ufs_page->part[wp].name, lun, first);
			for (uint32_t i = 64; i < 4096; i++) {
				pattern[i] = (uint8_t)(i * 7 + 3);
			}
			e = ufs_own_write(wp, 0, pattern, 4096);
			printf("ufs: own test: ufs_native_test=2: the first block of %s left written (\"%s\", then i*7+3): %d\n",
			    ufs_page->part[wp].name, (const char *)pattern, e);
			e == 0 ? (void)0 : (void)wrong++;
		}
		e = ufshc_sync();
		printf("ufs: own test: SYNCHRONIZE CACHE: %d\n", e);
		e == 0 ? (void)0 : (void)wrong++;
	}
	printf("ufs: own test %s: %u refusals as they should be, %u places written and put back, %u wrong\n",
	    wrong == 0 ? "PASSED" : "FAILED", refused, places, wrong);
out:
	if (saved != NULL) {
		kfree_data(saved, 64 << 10);
	}
	if (pattern != NULL) {
		kfree_data(pattern, 64 << 10);
	}
	if (back != NULL) {
		kfree_data(back, 64 << 10);
	}
}

static uint32_t ufs_make_nodes(void);

/* ufs_native=4: this kernel's own UFS driver has the controller for the run, no loader (1: beside UEFI's; 5: only look) */
static void
ufs_own_init(uint32_t mode)
{
	char wname[40] = "";
	uint32_t test = 0, made, units = 0;

	if (!ufshc_own_probe()) {
		if (ufshc_own_stopped()) {
			printf("ufs: own: stopped at the step ufs_native_step= asked for: no storage from it in this run\n");
		} else if (mode == 4) {
			printf("ufs: own: this kernel's own UFS driver is not used: no storage\n");
		}
		return;
	}
	ufs_own_table = kalloc_data(sizeof(struct loader_block), Z_WAITOK);
	if (ufs_own_table == NULL) {
		return;
	}
	ufs_own_table->magic = BLOCK_MAGIC;
	ufs_own_table->version = BLOCK_VERSION;
	ufs_own_table->size = sizeof(struct loader_block);
	ufs_page = ufs_own_table;
	ufs_parts = 0;
	memset(ufs_lun, 0xff, sizeof(ufs_lun));
	for (uint8_t lun = 0; lun < UFSHC_LUNS; lun++) {
		const struct ufshc_unit *u = ufshc_unit(lun);
		const char *why;
		uint32_t added;

		if (u == NULL || u->block_size != 4096) {
			continue;
		}
		why = ufs_own_gpt(lun, u, &added);
		if (why != NULL) {
			printf("ufs: own: LUN %u: no partitions read: %s\n", lun, why);
		}
		units++;
	}
	ufs_own_table->parts = ufs_parts;
	ufs_buffer_size = 1u << 20;     /* (no loader buffer: the size reads and writes are cut into) */
	ufs_buffer = NULL;
	ufs_own = true;
	ufs_native_on = true;
	ufs_native_mapped = true;
	made = ufs_make_nodes();
	ufs_ready = true;
	printf("ufs: own: %u partitions on %u logical units as /dev/ufs_NAME and /dev/rufs_NAME, read by this kernel's own "
	    "UFS driver; nothing asked of the loader\n", made, units);
	for (uint32_t i = 0; i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];

		printf("ufs: own: %-12s LUN %u, blocks %llu..%llu (%llu MB)%s\n", e->name, e->unit, e->start_lba,
		    e->start_lba + e->blocks - 1, e->blocks * e->block_size >> 20, (e->flags & BLOCK_PART_UNIT) ? ", the whole unit" :
		    "");
	}

	/* ufs_native_write=NAME: own mode: the one partition that takes writes, through /dev/rufs_NAME (never EFS, BOOT, PARAM ...) */
	if (PE_parse_boot_arg_str("ufs_native_write", wname, sizeof(wname)) && wname[0] != 0) {
		uint32_t part = ufs_entry(wname);
		const char *why = part == BLOCK_MAX_PARTS ? "there is no partition of that name" : ufs_own_unwritable(part);

		if (why == NULL && ufshc_open_window(ufs_lun[part], ufs_page->part[part].start_lba, ufs_page->part[part].blocks) != 0) {
			why = "the controller's driver would not open the window";
		}
		if (why != NULL) {
			printf("ufs: own: ufs_native_write=%s REFUSED: %s; nothing takes writes\n", wname, why);
		} else {
			ufs_own_write_part = (int)part;
			printf("ufs: own: /dev/rufs_%s takes writes (whole blocks, LUN %u blocks %llu..%llu); everything else of the "
			    "storage is read only\n", wname, ufs_lun[part], ufs_page->part[part].start_lba,
			    ufs_page->part[part].start_lba + ufs_page->part[part].blocks - 1);
			/*
			 * ufs_native_data=1: that partition, all of it, is also the data volume's block device,
			 * /dev/ufs_NAME_data (init: var_hfs=ufs_NAME_data), its writes through ufs_own_write and
			 * the device's cache flushed when HFS Plus's journal asks (the Galaxy A50: CACHE)
			 */
			if (PE_parse_boot_argn("ufs_native_data", &test, sizeof(test)) && test != 0) {
				ufs_write_part = (int)part;
				ufs_write_first = 0;
				if (devfs_make_node(makedev(ufs_bmajor, UFS_DATA_MINOR), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640,
				    "ufs_%s_data", wname) != NULL) {
					printf("ufs: own: /dev/ufs_%s_data: the data volume's block device, %llu MB, flushed when the "
					    "file system asks\n", wname, ufs_data_bytes() >> 20);
				}
				test = 0;
			}
		}
	}
	/* ufs_native_test=1: own mode's test at start: the refusals, and writes put back (2: one block left written) */
	if (PE_parse_boot_argn("ufs_native_test", &test, sizeof(test)) && test != 0) {
		ufs_own_test(test);
	}
	test = 0;
	if (PE_parse_boot_argn("ufs_native_bench", &test, sizeof(test)) && test != 0) {
		/* (as with the loader: N threads at random places of darwin, then back to the loader) */
		ufs_native_bench();
		hakios_ufs_report("after the bench");
		loader_return();
	}
}

/*
 * ---- ufs=3: this kernel's own UFS driver takes the controller, the loader's service its reference and fallback ----
 *
 * The loader serves the storage as with ufs=2 (it takes ufs=3 for ufs=2, and refuses every request while the page
 * says this kernel's list is in: xnuloader/loader.c, block_poll). Here, before anything else of the run reads:
 *
 *   1. through the loader, the references: every unit's head (protective MBR, GPT header, table) and its backup GPT,
 *      and the first, middle and last block of every partition;
 *   2. own mode on the SoC (ufs_native_soc=, the S9's exynos9810 by default) to step 5, as UEFI left the controller
 *      (hakios.ufshc.c): looked at, this kernel's list in, NOP OUT, the units' descriptors and capacities;
 *   3. the self-test: each of the loader's units is the one logical unit whose capacity is the same and whose head
 *      reads the same here, byte for byte; its GPT read here is right (header and table CRCs) and holds every
 *      partition of the loader's table at the same blocks, of the same size; the backup GPT and every partition's
 *      three blocks read the same both ways;
 *   4. the fences, before any window: LBA 0-5 and both GPTs of every unit, every partition on the protected list
 *      (EFS, SEC_EFS, CPEFS, RADIO, CP_DEBUG, PERSISTENT, KEYSTORAGE, PARAM, UP_PARAM, STEADY, BOTA0-2, DTB, DTBO,
 *      BOOT, RECOVERY, NAD_REFER, OMR, HIDDEN and the rest of ufs_never_written, any name with "efs" in it), every
 *      logical unit but USERDATA's (the others hold the boot loaders), and everything of USERDATA's unit outside
 *      USERDATA. hakios.ufshc.c's whitelist refuses any write or SYNCHRONIZE CACHE that touches one, after its own
 *      window check, and says which; query writes, UNMAP, FORMAT UNIT, WRITE BUFFER, security protocols and vendor
 *      commands are not on its list at all;
 *   5. the window: USERDATA, and only with ufs_write=1 (the loader's offer, checked as for ufs=2), never with
 *      ufs_native_ro=1 (which also takes the loader's write path away for the run); otherwise everything is read
 *      only, in the whitelist itself;
 *   6. the switch: reads (and the data volume's writes) through this kernel's driver; the loader is asked nothing.
 *
 * Anything wrong before 6 gives the controller back (UEFI's list, interrupt enable and nexus types, read back) and
 * the run goes on through the loader as with ufs=2. A read that fails after 6 does the same, if the controller can
 * be given back; if it cannot, the storage is off for the run (nothing is asked of UEFI's driver behind a list that
 * is not its own). A write that fails is an error, never retried the other way. Every step is said into the
 * loader's log (loader_log_line), where the run's log is read.
 *
 * The run's outcome is kept in memory (nufs_sum) and said as a summary (hakios_ufs_summary): after the start, when
 * the USB console's host configures the link, on the console's "ufs", every 30 s with fps_log=1 or ufs_native_log=1,
 * when the run is ending (hakios_var_save_on_return: the kernel's log too, whose message buffer loader_return appends
 * last to the loader's log) and once more as the drivers are given back (hakios_ufs_quiesce, any way out, a panic's
 * too: the loader's log only).
 */

extern void loader_log_line(const char *line);

static void
nufs_say(const char *format, ...) __printflike(1, 2);
static void
nufs_say(const char *format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (strlen(line) == sizeof(line) - 1) {
		line[sizeof(line) - 2] = '\n';
	}
	printf("%s", line);
	loader_log_line(line);
}

/* ---- ufs=3's summary (nufs_sum) ---- */

static const char *
nufs_pname(uint32_t part)
{
	return ufs_page != NULL && part < ufs_parts ? (const char *)ufs_page->part[part].name : "?";
}

/* a refusal: counted, the first three kept with their reasons */
static void
nufs_refused(const char *format, ...)
{
	uint64_t n = os_atomic_inc_orig(&nufs_sum.refused, relaxed);
	va_list list;

	if (n < 3) {
		va_start(list, format);
		vsnprintf(nufs_sum.refusal[n], sizeof(nufs_sum.refusal[0]), format, list);
		va_end(list);
	}
}

/* the controller's whitelist refused a command (hakios.ufshc.c) */
void
hakios_ufs_refused(const char *why)
{
	nufs_refused("the whitelist: %s", why);
}

/* why the run is not on this kernel's own driver: the first reason given is kept */
static void
nufs_why(const char *why)
{
	if (nufs_sum.why[0] == 0) {
		strlcpy(nufs_sum.why, why, sizeof(nufs_sum.why));
	}
}

static void
nufs_summary_line(int how, const char *format, ...) __printflike(2, 3);
static void
nufs_summary_line(int how, const char *format, ...)
{
	char line[600];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (strlen(line) == sizeof(line) - 1) {
		line[sizeof(line) - 2] = '\n';
	}
	if (how & HAKIOS_UFS_SAY_LOG) {
		loader_log_line(line);
	}
	if (how & HAKIOS_UFS_SAY_PRINTF) {
		printf("%s", line);
	}
}

/*
 * The summary of ufs=3, in two lines and then the first three refusals:
 *   ufs: nufs: summary (WHEN, up S s): step N of 5 (NAME); self-test R; fence test R; mode M
 *   ufs: nufs: summary: reads N native, N through the loader (N MB), N failed; writes N (N MB), N failed; refused N
 *   ufs: nufs: summary: refused #1: WHY
 * M: NATIVE (...), FELL BACK to ufs=2: WHY, NO STORAGE: WHY, not started: WHY, in progress, or ufs=3 not asked.
 * how: HAKIOS_UFS_SAY_LOG (the loader's log, and so the USB console), _PRINTF (the kernel's), _NUFS_ONLY (nothing
 * unless ufs=3 was asked). Takes no lock: from the USB console's poll, a thread, or the end of the run.
 */
void
hakios_ufs_summary(const char *when, int how)
{
	static const char *const steps[] = { "not started", "references through the loader", "the controller taken",
		                             "self-test", "the fences", "switched" };
	char mode[320], fence[64];
	uint64_t ns;
	uint32_t step = nufs_sum.step;

	if ((how & HAKIOS_UFS_SAY_NUFS_ONLY) && !ufs_nufs) {
		return;
	}
	if (step >= sizeof(steps) / sizeof(steps[0])) {
		step = 0;
	}
	if (!ufs_nufs) {
		strlcpy(mode, "ufs=3 not asked", sizeof(mode));
	} else if (ufs_nufs_on) {
		snprintf(mode, sizeof(mode), "NATIVE (every read through this kernel's own driver; %s)", ufs_nufs_ro ?
		    "read only, ufs_native_ro=1" : ufs_write_part >= 0 ? "USERDATA's writes too" : "read only");
	} else if (ufs_dead) {
		snprintf(mode, sizeof(mode), "NO STORAGE: %s", nufs_sum.why[0] ? nufs_sum.why : "?");
	} else if (nufs_sum.why[0] != 0) {
		snprintf(mode, sizeof(mode), "%s: %s; reads through the loader", nufs_sum.step == 0 ? "not started" :
		    "FELL BACK to ufs=2", nufs_sum.why);
	} else if (nufs_sum.step == 0) {
		strlcpy(mode, "not started yet: reads through the loader", sizeof(mode));
	} else {
		strlcpy(mode, "in progress", sizeof(mode));
	}
	if (nufs_sum.fences == 0) {
		strlcpy(fence, "not run", sizeof(fence));
	} else {
		snprintf(fence, sizeof(fence), "%s (%u writes judged, none sent)", nufs_sum.fences > 0 ? "passed" : "FAILED",
		    nufs_sum.fences_tried);
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	nufs_summary_line(how, "ufs: nufs: summary (%s, up %llu s): step %u of 5 (%s); self-test %s; soak %s; fence test %s; "
	    "mode %s\n", when, ns / NSEC_PER_SEC, step, steps[step], nufs_sum.selftest == 0 ? "not run" : nufs_sum.selftest > 0 ?
	    "passed" : "FAILED", nufs_sum.soak == 0 ? "not run" : nufs_sum.soak > 0 ? "passed" : "FAILED", fence, mode);
	nufs_summary_line(how, "ufs: nufs: summary: reads %llu native, %llu through the loader (%llu MB), %llu failed; "
	    "writes %llu (%llu MB), %llu failed; refused %llu\n", nufs_sum.reads_native, nufs_sum.reads_loader,
	    nufs_sum.read_bytes >> 20, nufs_sum.read_errors, ufs_writes, ufs_write_bytes >> 20, ufs_write_errors,
	    nufs_sum.refused);
	for (uint32_t i = 0; i < 3 && i < nufs_sum.refused; i++) {
		nufs_summary_line(how, "ufs: nufs: summary: refused #%u: %s\n", i + 1, nufs_sum.refusal[i]);
	}
	if (ufs_nufs_on) {
		char speed[320];

		if (ufshc_speed(speed, sizeof(speed))) {
			nufs_summary_line(how, "%s", speed);
		}
	}
}

/* fps_log=1 or ufs_native_log=1: the summary every 30 s for the run */
static void
nufs_summary_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		IOSleep(30 * 1000);
		hakios_ufs_summary("every 30 s", HAKIOS_UFS_SAY_LOG | HAKIOS_UFS_SAY_PRINTF);
	}
}

/* The partitions that are never written, whatever is asked (and ufs_never_written's besides) */
static const char *const ufs_nufs_protected[] = {
	"EFS", "SEC_EFS", "CPEFS", "RADIO", "CP_DEBUG", "PERSISTENT", "KEYSTORAGE", "PARAM", "UP_PARAM", "STEADY",
	"BOTA0", "BOTA1", "BOTA2", "DTB", "DTBO", "BOOT", "RECOVERY", "NAD_REFER", "OMR", "HIDDEN",
};

static bool
ufs_nufs_is_protected(const char *name)
{
	for (unsigned i = 0; i < sizeof(ufs_nufs_protected) / sizeof(ufs_nufs_protected[0]); i++) {
		if (ufs_name_is(name, ufs_nufs_protected[i])) {
			return true;
		}
	}
	for (unsigned i = 0; i < sizeof(ufs_never_written) / sizeof(ufs_never_written[0]); i++) {
		if (ufs_name_is(name, ufs_never_written[i])) {
			return true;
		}
	}
	for (const char *c = name; *c != 0; c++) {
		if (strncasecmp(c, "efs", 3) == 0) {
			return true;
		}
	}
	return false;
}

#define NUFS_HEAD_MAX      64u                  /* blocks of a unit's head kept: MBR, header, table */
#define NUFS_RAW_HEAD      6u                   /* a unit with no GPT: its blocks 0-5 */
#define NUFS_SPOTS         3u                   /* first, middle, last block of each partition */

struct nufs_unit {
	uint32_t part;                          /* the loader's entry for the whole unit */
	uint8_t lun;                            /* this kernel's logical unit; 0xff: none (not read, never written) */
	bool gpt;
	uint32_t head, backup;                  /* blocks kept: [0, head) and [alt_elba, alt] */
	uint64_t elba, tblocks, alt, alt_elba, first, last;
	uint32_t count;
	uint8_t *ref, *bref;                    /* through the loader */
};

static struct nufs_unit ufs_nufs_units[BLOCK_MAX_UNITS];
static uint32_t ufs_nufs_nunits;
static uint8_t *ufs_nufs_spots;                 /* each partition's NUFS_SPOTS blocks through the loader */
#define ufs_nufs_spot(part, s)  (ufs_nufs_spots + ((size_t)(part) * NUFS_SPOTS + (s)) * 4096)
#define NUFS_SPOTS_SIZE         ((vm_size_t)BLOCK_MAX_PARTS * NUFS_SPOTS * 4096)

/* the loader's unit (its index among the units) of a partition: ufs_nufs_units[] index, or -1 */
static int
nufs_unit_of(uint32_t part)
{
	for (uint32_t k = 0; k < ufs_nufs_nunits; k++) {
		if (ufs_page->part[ufs_nufs_units[k].part].unit == ufs_page->part[part].unit) {
			return (int)k;
		}
	}
	return -1;
}

static uint64_t
nufs_fnv(const uint8_t *p, uint64_t n)
{
	return ufs_fnv(0xcbf29ce484222325ULL, p, n);
}

static void
nufs_free_units(void)
{
	for (uint32_t k = 0; k < ufs_nufs_nunits; k++) {
		struct nufs_unit *u = &ufs_nufs_units[k];

		if (u->ref != NULL) {
			kfree_data(u->ref, (vm_size_t)u->head * 4096);
		}
		if (u->bref != NULL) {
			kfree_data(u->bref, (vm_size_t)u->backup * 4096);
		}
		u->ref = u->bref = NULL;
	}
}

/* Step 1: a unit's head and backup GPT through the loader (UEFI's list in). NULL: kept; else why not. */
static const char *
nufs_reference_unit(struct nufs_unit *u)
{
	volatile struct loader_block_part *e = &ufs_page->part[u->part];
	static uint8_t h[4096];         /* (bsd_init's thread alone: no 4 KB on its stack) */
	uint64_t count, esize;

	if (e->block_size != 4096 || e->blocks < 64) {
		return "not 4096-byte blocks, or too small";
	}
	if (ufs_read_mailbox(u->part, 4096, h, 4096) != 0) {
		return "its block 1 could not be read through the loader";
	}
	u->gpt = memcmp(h, "EFI PART", 8) == 0;
	if (!u->gpt) {
		u->head = NUFS_RAW_HEAD;
	} else {
		u->elba = ufs_le64(h + 72);
		count = ufs_le32(h + 80);
		esize = ufs_le32(h + 84);
		u->alt = ufs_le64(h + 32);
		u->first = ufs_le64(h + 40);
		u->last = ufs_le64(h + 48);
		u->count = (uint32_t)count;
		if (esize != 128 || count == 0 || count > 1024 || u->elba < 2 || u->alt >= e->blocks || u->alt < 3) {
			return "its GPT header's geometry (through the loader)";
		}
		u->tblocks = (count * esize + 4095) / 4096;
		if (u->elba + u->tblocks > NUFS_HEAD_MAX) {
			return "its GPT's table is further in than this keeps";
		}
		u->head = (uint32_t)(u->elba + u->tblocks);
		if (u->head < NUFS_RAW_HEAD) {
			u->head = NUFS_RAW_HEAD;
		}
	}
	u->ref = kalloc_data((vm_size_t)u->head * 4096, Z_WAITOK);
	if (u->ref == NULL || ufs_read_mailbox(u->part, 0, u->ref, (uint64_t)u->head * 4096) != 0) {
		return "its head could not be read through the loader";
	}
	if (u->gpt) {
		if (ufs_read_mailbox(u->part, u->alt * 4096, h, 4096) != 0 || memcmp(h, "EFI PART", 8) != 0) {
			return "no backup GPT header where the primary says";
		}
		u->alt_elba = ufs_le64(h + 72);
		if (u->alt_elba > u->alt || u->alt - u->alt_elba + 1 > NUFS_HEAD_MAX || u->alt_elba <= u->last) {
			return "the backup GPT's geometry";
		}
		u->backup = (uint32_t)(u->alt - u->alt_elba + 1);
		u->bref = kalloc_data((vm_size_t)u->backup * 4096, Z_WAITOK);
		if (u->bref == NULL || ufs_read_mailbox(u->part, u->alt_elba * 4096, u->bref, (uint64_t)u->backup * 4096) != 0) {
			return "the backup GPT could not be read through the loader";
		}
	}
	return NULL;
}

/*
 * Step 3, a unit: which of this kernel's logical units it is -- the one of the same capacity whose head reads
 * the same, and no other. 0xff: none, or more than one.
 */
static uint8_t
nufs_map_unit(struct nufs_unit *u, uint8_t *buf)
{
	uint8_t found = 0xff;
	uint32_t matches = 0;

	for (uint8_t lun = 0; lun < UFSHC_LUNS; lun++) {
		const struct ufshc_unit *n = ufshc_unit(lun);

		if (n == NULL || n->block_size != 4096 || n->blocks != ufs_page->part[u->part].blocks) {
			continue;
		}
		if (ufshc_read(lun, 0, buf, (uint64_t)u->head * 4096) != 0) {
			nufs_say("ufs: nufs: LUN %u: its head could not be read here\n", lun);
			continue;
		}
		if (memcmp(buf, u->ref, (size_t)u->head * 4096) == 0) {
			found = lun;
			matches++;
		}
	}
	return matches == 1 ? found : 0xff;
}

/* Step 3, a mapped unit with a GPT: the GPT read here is right and has the loader's partitions. NULL: it does */
static const char *
nufs_check_gpt(const struct nufs_unit *u, const uint8_t *head, const uint8_t *backup)
{
	uint8_t h[512];
	uint32_t hsize, crc;

	memcpy(h, head + 4096, sizeof(h));
	hsize = ufs_le32(h + 12);
	crc = ufs_le32(h + 16);
	if (memcmp(h, "EFI PART", 8) != 0 || ufs_le32(h + 8) != 0x00010000 || hsize < 92 || hsize > sizeof(h) ||
	    ufs_le64(h + 24) != 1) {
		return "the GPT header (signature, revision, size, own LBA)";
	}
	*(uint32_t *)(h + 16) = 0;
	if (crc32(0, h, hsize) != crc) {
		return "the GPT header's CRC";
	}
	if (crc32(0, head + u->elba * 4096, (size_t)u->count * 128) != ufs_le32(h + 88)) {
		return "the GPT table's CRC";
	}
	/* the backup: its header at alt, saying the same table */
	memcpy(h, backup + (u->alt - u->alt_elba) * 4096, sizeof(h));
	if (memcmp(h, "EFI PART", 8) != 0 || ufs_le64(h + 24) != u->alt || ufs_le64(h + 32) != 1 ||
	    crc32(0, backup, (size_t)u->count * 128) != ufs_le32(h + 88)) {
		return "the backup GPT (its header's own LBA, the primary's, its table's CRC)";
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		bool found = false;

		if ((e->flags & BLOCK_PART_UNIT) || e->unit != ufs_page->part[u->part].unit) {
			continue;
		}
		for (uint32_t j = 0; j < u->count && !found; j++) {
			const uint8_t *g = head + u->elba * 4096 + 128 * j;
			char name[36];
			uint32_t n = 0;

			for (; n < 35 && (g[56 + 2 * n] | g[57 + 2 * n]) != 0; n++) {
				uint16_t ch = (uint16_t)(g[56 + 2 * n] | g[57 + 2 * n] << 8);

				name[n] = ch < 0x80 ? (char)ch : '?';
			}
			name[n] = 0;
			if (strncmp(name, (const char *)e->name, sizeof(name)) != 0) {
				continue;
			}
			found = true;
			if (ufs_le64(g + 32) != e->start_lba || ufs_le64(g + 40) - ufs_le64(g + 32) + 1 != e->blocks) {
				nufs_say("ufs: nufs: %s: the GPT read here says blocks %llu..%llu, the loader's table %llu..%llu\n",
				    name, ufs_le64(g + 32), ufs_le64(g + 40), e->start_lba, e->start_lba + e->blocks - 1);
				return "a partition is not where the loader's table has it";
			}
		}
		if (!found) {
			nufs_say("ufs: nufs: %s: not in the GPT read here\n", (const char *)e->name);
			return "a partition of the loader's table is not in the GPT read here";
		}
	}
	return NULL;
}

/* Step 3: everything the loader read, read again here and compared. NULL: the same; else what differs */
static const char *
nufs_selftest(void)
{
	uint8_t *buf = kalloc_data(NUFS_HEAD_MAX * 4096, Z_WAITOK), *mine = NULL;
	uint64_t spots = 0, bytes = 0;
	const char *why = NULL;
	bool main_unit = false;

	if (buf == NULL) {
		return "no memory";
	}
	for (uint32_t k = 0; why == NULL && k < ufs_nufs_nunits; k++) {
		struct nufs_unit *u = &ufs_nufs_units[k];
		volatile struct loader_block_part *e = &ufs_page->part[u->part];

		u->lun = nufs_map_unit(u, buf);
		if (u->lun == 0xff) {
			nufs_say("ufs: nufs: the loader's unit %u (%llu blocks%s): no one logical unit here reads the same: not read, "
			    "never written\n", e->unit, e->blocks, u->gpt ? ", a GPT" : ", no GPT");
			if (e->unit == 0) {
				why = "the loader's first unit (SYSTEM's, USERDATA's) is no logical unit here";
			}
			continue;
		}
		main_unit |= e->unit == 0;
		/* (buf holds its head as read here, the same as the loader's) */
		nufs_say("ufs: nufs: the loader's unit %u is LUN %u: blocks 0..%u read both ways, the same (fnv %llx)\n",
		    e->unit, u->lun, u->head - 1, nufs_fnv(u->ref, (uint64_t)u->head * 4096));
		bytes += (uint64_t)u->head * 4096;
		if (!u->gpt) {
			continue;
		}
		if (ufshc_read(u->lun, u->alt_elba * 4096, buf, (uint64_t)u->backup * 4096) != 0) {
			why = "the backup GPT could not be read here";
			break;
		}
		if (memcmp(buf, u->bref, (size_t)u->backup * 4096) != 0) {
			why = "the backup GPT read here is not what the loader read";
			break;
		}
		bytes += (uint64_t)u->backup * 4096;
		nufs_say("ufs: nufs: LUN %u: backup GPT (blocks %llu..%llu) the same both ways\n", u->lun, u->alt_elba, u->alt);
		why = nufs_check_gpt(u, u->ref, u->bref);
		if (why == NULL) {
			nufs_say("ufs: nufs: LUN %u: GPT right (header and table CRCs, backup at %llu) and every partition of the "
			    "loader's table in it at the same blocks\n", u->lun, u->alt);
		}
	}
	if (why == NULL && !main_unit) {
		why = "the loader lists no first unit";
	}
	/* each partition of a unit found here: its logical unit, and where it begins there */
	for (uint32_t i = 0; why == NULL && i < ufs_parts; i++) {
		int k = nufs_unit_of(i);

		ufs_lun[i] = 0xff;
		if (k >= 0 && ufs_nufs_units[k].lun != 0xff && ufs_page->part[i].block_size == 4096) {
			ufs_lun[i] = ufs_nufs_units[k].lun;
			ufs_lun_offset[i] = (ufs_page->part[i].flags & BLOCK_PART_UNIT) ? 0 : ufs_page->part[i].start_lba * 4096;
		}
	}
	/* every partition's first, middle and last block, both ways */
	mine = kalloc_data(4096, Z_WAITOK);
	if (why == NULL && mine == NULL) {
		why = "no memory";
	}
	for (uint32_t i = 0; why == NULL && i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		int k = nufs_unit_of(i);
		uint64_t at[NUFS_SPOTS];

		if ((e->flags & BLOCK_PART_UNIT) || k < 0 || ufs_nufs_units[k].lun == 0xff || e->block_size != 4096 ||
		    e->blocks == 0) {
			continue;
		}
		at[0] = 0;
		at[1] = e->blocks / 2;
		at[2] = e->blocks - 1;
		for (uint32_t s = 0; why == NULL && s < NUFS_SPOTS; s++) {
			if (s > 0 && at[s] == at[s - 1]) {
				continue;
			}
			if (ufs_read_native(i, at[s] * 4096, mine, 4096) != 0) {
				why = "a partition's block could not be read here";
			} else if (memcmp(mine, ufs_nufs_spot(i, s), 4096) != 0) {
				nufs_say("ufs: nufs: %s block %llu (LUN %u block %llu): NOT what the loader read\n", (const char *)e->name,
				    at[s], ufs_nufs_units[k].lun, e->start_lba + at[s]);
				why = "a partition's block reads differently here";
			}
			spots++;
		}
	}
	if (why == NULL) {
		nufs_say("ufs: nufs: self-test passed: %u unit(s), %llu KB of heads and GPTs and %llu partition blocks read both "
		    "ways, all the same\n", ufs_nufs_nunits, bytes >> 10, spots);
	}
	kfree_data(buf, NUFS_HEAD_MAX * 4096);
	if (mine != NULL) {
		kfree_data(mine, 4096);
	}
	return why;
}

/* Step 1: the references, through the loader (the caller holds nothing; UEFI's list is in) */
static const char *
nufs_references(void)
{
	uint64_t spots = 0;

	ufs_nufs_nunits = 0;
	for (uint32_t i = 0; i < ufs_parts && ufs_nufs_nunits < BLOCK_MAX_UNITS; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		struct nufs_unit *u;
		const char *why;

		if ((e->flags & BLOCK_PART_UNIT) == 0 || e->unit >= UFSHC_LUNS) {
			continue;
		}
		u = &ufs_nufs_units[ufs_nufs_nunits++];
		bzero(u, sizeof(*u));
		u->part = i;
		u->lun = 0xff;
		why = nufs_reference_unit(u);
		if (why != NULL) {
			nufs_say("ufs: nufs: the loader's unit %u (%s): %s\n", e->unit, (const char *)e->name, why);
			if (e->unit == 0) {
				return "the first unit's head or GPT could not be read through the loader";
			}
			/* (another unit: kept out; it is not read here and never written) */
			if (u->ref != NULL) {
				kfree_data(u->ref, (vm_size_t)u->head * 4096);
			}
			if (u->bref != NULL) {
				kfree_data(u->bref, (vm_size_t)u->backup * 4096);
			}
			ufs_nufs_nunits--;
			continue;
		}
		nufs_say("ufs: nufs: the loader's unit %u (%s, %llu blocks): %s; blocks 0..%u read through the loader (fnv %llx)%s\n",
		    e->unit, (const char *)e->name, e->blocks, u->gpt ? "a GPT" : "no GPT", u->head - 1,
		    nufs_fnv(u->ref, (uint64_t)u->head * 4096), u->gpt ? ", and its backup GPT" : "");
	}
	ufs_nufs_spots = kalloc_data(NUFS_SPOTS_SIZE, Z_WAITOK);
	if (ufs_nufs_spots == NULL) {
		return "no memory";
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		uint64_t at[NUFS_SPOTS];

		if ((e->flags & BLOCK_PART_UNIT) || nufs_unit_of(i) < 0 || e->block_size != 4096 || e->blocks == 0) {
			continue;
		}
		at[0] = 0;
		at[1] = e->blocks / 2;
		at[2] = e->blocks - 1;
		for (uint32_t s = 0; s < NUFS_SPOTS; s++) {
			if (s > 0 && at[s] == at[s - 1]) {
				continue;
			}
			if (ufs_read_mailbox(i, at[s] * 4096, ufs_nufs_spot(i, s), 4096) != 0) {
				nufs_say("ufs: nufs: %s block %llu could not be read through the loader\n", (const char *)e->name, at[s]);
				return "a partition's block could not be read through the loader";
			}
			spots++;
		}
	}
	nufs_say("ufs: nufs: references through the loader: %u unit(s), %llu partition blocks\n", ufs_nufs_nunits, spots);
	return NULL;
}

/* Step 4: the fences (before any window). false: one could not be set, and then everything stays read only. */
static bool
nufs_fences(int data_unit_lun)
{
	uint32_t named = 0;
	bool all = true;

	/* every logical unit but USERDATA's, the ones this kernel cannot tell (boot units) among them */
	for (uint8_t lun = 0; lun < UFSHC_LUNS; lun++) {
		if ((int)lun != data_unit_lun) {
			ufshc_deny_lun(lun);
		}
	}
	for (uint32_t k = 0; k < ufs_nufs_nunits; k++) {
		const struct nufs_unit *u = &ufs_nufs_units[k];

		if (u->lun == 0xff) {
			continue;
		}
		all &= ufshc_deny(u->lun, 0, 6, "LBA 0-5") == 0;
		if (u->gpt) {
			all &= ufshc_deny(u->lun, 0, u->elba + u->tblocks, "the GPT") == 0;
			all &= ufshc_deny(u->lun, u->alt_elba, u->alt - u->alt_elba + 1, "the backup GPT") == 0;
		}
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];

		if ((e->flags & BLOCK_PART_UNIT) || ufs_lun[i] == 0xff || !ufs_nufs_is_protected((const char *)e->name)) {
			continue;
		}
		all &= ufshc_deny(ufs_lun[i], e->start_lba, e->blocks, (const char *)e->name) == 0;
		named++;
	}
	/* and on USERDATA's unit everything outside USERDATA */
	if (data_unit_lun >= 0 && ufs_write_part >= 0) {
		volatile struct loader_block_part *d = &ufs_page->part[ufs_write_part];
		const struct ufshc_unit *n = ufshc_unit((uint8_t)data_unit_lun);

		all &= ufshc_deny((uint8_t)data_unit_lun, 0, d->start_lba, "before USERDATA") == 0;
		if (n != NULL && d->start_lba + d->blocks < n->blocks) {
			all &= ufshc_deny((uint8_t)data_unit_lun, d->start_lba + d->blocks, n->blocks - d->start_lba - d->blocks,
			    "after USERDATA") == 0;
		}
	}
	nufs_say("ufs: nufs: fences set: every logical unit but %s never written; LBA 0-5 and both GPTs of every unit; %u "
	    "protected partitions by name (EFS, RADIO, BOOT, PARAM ...); %s\n", data_unit_lun >= 0 ? "USERDATA's" : "none",
	    named, all ? "all of them in the whitelist" : "ONE COULD NOT BE SET: everything read only");
	for (uint32_t i = 0; i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];

		if ((e->flags & BLOCK_PART_UNIT) == 0 && ufs_lun[i] != 0xff && ufs_nufs_is_protected((const char *)e->name)) {
			nufs_say("ufs: nufs: protected: %-12s LUN %u blocks %llu..%llu\n", (const char *)e->name, ufs_lun[i],
			    e->start_lba, e->start_lba + e->blocks - 1);
		}
	}
	if (!all) {
		ufshc_force_ro();
	}
	return all;
}

/*
 * Step 4 again, proved: writes the whitelist must refuse, made and judged, never sent -- LBA 0, the GPTs, every
 * protected partition's first and last block, another unit, past USERDATA -- and USERDATA's first block, which
 * passes only with the window open. false: one was not refused, or USERDATA's did not do what it should.
 */
static bool
nufs_prove_fences(int data_unit_lun, bool window)
{
	uint32_t tried = 0;
	bool right = true;

	for (uint32_t k = 0; k < ufs_nufs_nunits; k++) {
		const struct nufs_unit *u = &ufs_nufs_units[k];
		const uint64_t lbas[4] = { 0, 1, u->elba, u->alt };

		for (uint32_t j = 0; u->lun != 0xff && j < (u->gpt ? 4u : 1u); j++) {
			const char *why = ufshc_write_refusal(u->lun, lbas[j], 1);

			tried++;
			if (why == NULL) {
				nufs_say("ufs: nufs: fence test: a write of LUN %u block %llu WOULD PASS\n", u->lun, lbas[j]);
				right = false;
			}
		}
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		volatile struct loader_block_part *e = &ufs_page->part[i];
		const char *why0, *why1;

		if ((e->flags & BLOCK_PART_UNIT) || ufs_lun[i] == 0xff || (int)i == ufs_write_part) {
			continue;
		}
		why0 = ufshc_write_refusal(ufs_lun[i], e->start_lba, 1);
		why1 = ufshc_write_refusal(ufs_lun[i], e->start_lba + e->blocks - 1, 1);
		tried += 2;
		if (why0 == NULL || why1 == NULL) {
			nufs_say("ufs: nufs: fence test: a write into %s WOULD PASS\n", (const char *)e->name);
			right = false;
		} else if (ufs_nufs_is_protected((const char *)e->name)) {
			nufs_say("ufs: nufs: fence test: %s refused (%s)\n", (const char *)e->name, why0);
		}
	}
	for (uint8_t lun = 0; lun < UFSHC_LUNS; lun++) {
		if ((int)lun != data_unit_lun && ufshc_unit(lun) != NULL) {
			tried++;
			if (ufshc_write_refusal(lun, 100, 1) == NULL) {
				nufs_say("ufs: nufs: fence test: a write of LUN %u WOULD PASS\n", lun);
				right = false;
			}
		}
	}
	if (ufs_write_part >= 0 && data_unit_lun >= 0) {
		volatile struct loader_block_part *d = &ufs_page->part[ufs_write_part];
		const char *first = ufshc_write_refusal((uint8_t)data_unit_lun, d->start_lba, 1);
		const char *past = ufshc_write_refusal((uint8_t)data_unit_lun, d->start_lba + d->blocks - 1, 2);
		const char *before = d->start_lba == 0 ? "(it begins at block 0)" :
		    ufshc_write_refusal((uint8_t)data_unit_lun, d->start_lba - 1, 2);

		tried += 3;
		nufs_say("ufs: nufs: fence test: USERDATA's first block %s (%s); its last and the next refused (%s); the block "
		    "before it and its first refused (%s)\n", first == NULL ? "would pass" : "refused", first == NULL ? "the window" :
		    first, past == NULL ? "WOULD PASS" : past, before == NULL ? "WOULD PASS" : before);
		right &= past != NULL && before != NULL && ((first == NULL) == window);
	}
	nufs_say("ufs: nufs: fence test: %u writes made and judged, none sent: %s\n", tried, right ? "every one as it should be" :
	    "NOT AS IT SHOULD BE");
	nufs_sum.fences_tried = tried;
	nufs_sum.fences = right ? 1 : -1;
	return right;
}

/* The controller back to UEFI, and the run on through the loader if that worked; said either way */
static void
nufs_abort(const char *why)
{
	bool back;

	nufs_say("ufs: nufs: ABORTED: %s\n", why);
	nufs_why(why);
	ufs_nufs_on = false;
	ufs_native_on = false;
	memset(ufs_lun, 0xff, sizeof(ufs_lun));
	back = ufshc_release("ufs=3 aborted");
	if (back) {
		nufs_say("ufs: nufs: UEFI's list is back as it was: the storage goes through the loader for this run (ufs=2)\n");
	} else {
		ufs_dead = true;
		nufs_say("ufs: nufs: THE CONTROLLER COULD NOT BE GIVEN BACK: no storage in this run (the loader refuses while "
		    "the page says this kernel's list is in, and it is not asked)\n");
	}
}

/*
 * ufs_native_soak=N (at most NUFS_SOAK_MAX): N reads of SYSTEM at random blocks, 1 to 16 blocks each (so 1 to 16
 * PRDT entries), first through the loader at step 1 (each one's FNV-1a kept), then through this kernel's own
 * driver after the self-test, each compared with the loader's. Any read failing or different is a refusal: the
 * controller given back, the run through the loader. Read only: nothing is written.
 */
#define NUFS_SOAK_MAX       2048u
#define NUFS_SOAK_BLOCKS    16u

struct nufs_soak {
	uint64_t block;
	uint32_t count;
	uint64_t fnv;
};

static struct nufs_soak *nufs_soaks;
static uint32_t nufs_soak_part = BLOCK_MAX_PARTS;

static uint64_t
nufs_soak_rand(uint64_t *x)
{
	*x ^= *x << 13;
	*x ^= *x >> 7;
	*x ^= *x << 17;
	return *x;
}

static void
nufs_soak_free(void)
{
	if (nufs_soaks != NULL) {
		kfree_data(nufs_soaks, (vm_size_t)ufs_nufs_soak * sizeof(struct nufs_soak));
		nufs_soaks = NULL;
	}
}

/* step 1: the loader's reads. NULL, or why ufs=3 is not taken */
static const char *
nufs_soak_references(void)
{
	uint32_t part = ufs_entry("SYSTEM");
	uint64_t seed = mach_absolute_time() | 1, x, blocks;
	uint8_t *buf;

	if (ufs_nufs_soak > NUFS_SOAK_MAX) {
		ufs_nufs_soak = NUFS_SOAK_MAX;
	}
	if (part >= ufs_parts || nufs_unit_of(part) < 0 || ufs_page->part[part].block_size != 4096 ||
	    (blocks = ufs_page->part[part].blocks) < NUFS_SOAK_BLOCKS) {
		return "ufs_native_soak: no SYSTEM partition of 4 KB blocks on a unit of the loader's";
	}
	nufs_soaks = kalloc_data((vm_size_t)ufs_nufs_soak * sizeof(struct nufs_soak), Z_WAITOK);
	buf = kalloc_data(NUFS_SOAK_BLOCKS * 4096, Z_WAITOK);
	if (nufs_soaks == NULL || buf == NULL) {
		if (buf != NULL) {
			kfree_data(buf, NUFS_SOAK_BLOCKS * 4096);
		}
		nufs_soak_free();
		return "ufs_native_soak: no memory";
	}
	nufs_soak_part = part;
	x = seed;
	for (uint32_t i = 0; i < ufs_nufs_soak; i++) {
		struct nufs_soak *k = &nufs_soaks[i];

		k->count = 1 + (uint32_t)(nufs_soak_rand(&x) % NUFS_SOAK_BLOCKS);
		k->block = nufs_soak_rand(&x) % (blocks - k->count + 1);
		if (ufs_read_mailbox(part, k->block * 4096, buf, (uint64_t)k->count * 4096) != 0) {
			nufs_say("ufs: nufs: soak: SYSTEM blocks %llu..%llu could not be read through the loader\n", k->block,
			    k->block + k->count - 1);
			kfree_data(buf, NUFS_SOAK_BLOCKS * 4096);
			nufs_soak_free();
			return "ufs_native_soak: a read through the loader failed";
		}
		k->fnv = nufs_fnv(buf, (uint64_t)k->count * 4096);
	}
	kfree_data(buf, NUFS_SOAK_BLOCKS * 4096);
	nufs_say("ufs: nufs: soak: %u random reads of SYSTEM (%llu blocks; 1 to %u blocks each, seed %llx) through the loader, "
	    "kept to compare\n", ufs_nufs_soak, blocks, NUFS_SOAK_BLOCKS, seed);
	return NULL;
}

/* after the self-test: the same reads through this kernel's own driver. NULL, or why not */
static const char *
nufs_soak_run(void)
{
	static char why[160];
	uint64_t t0 = mach_absolute_time(), ns, bytes = 0;
	uint8_t *buf = kalloc_data(NUFS_SOAK_BLOCKS * 4096, Z_WAITOK);

	if (buf == NULL) {
		return "ufs_native_soak: no memory";
	}
	why[0] = 0;
	for (uint32_t i = 0; i < ufs_nufs_soak && why[0] == 0; i++) {
		struct nufs_soak *k = &nufs_soaks[i];
		uint64_t len = (uint64_t)k->count * 4096;
		int e;

		memset(buf, 0xa5, (size_t)len);
		e = ufs_read_native(nufs_soak_part, k->block * 4096, buf, len);
		if (e != 0) {
			snprintf(why, sizeof(why), "soak: read %u of %u (SYSTEM blocks %llu..%llu) failed here (%d)", i + 1,
			    ufs_nufs_soak, k->block, k->block + k->count - 1, e);
		} else if (nufs_fnv(buf, len) != k->fnv) {
			snprintf(why, sizeof(why), "soak: read %u of %u (SYSTEM blocks %llu..%llu) NOT the loader's (fnv %llx / %llx)",
			    i + 1, ufs_nufs_soak, k->block, k->block + k->count - 1, nufs_fnv(buf, len), k->fnv);
		}
		bytes += len;
	}
	kfree_data(buf, NUFS_SOAK_BLOCKS * 4096);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	if (why[0] != 0) {
		nufs_say("ufs: nufs: %s\n", why);
		return why;
	}
	nufs_say("ufs: nufs: soak passed: %u random reads of SYSTEM (%llu KB) through this driver, every one the loader's, "
	    "%llu ms\n", ufs_nufs_soak, bytes >> 10, ns / 1000000);
	return NULL;
}

/* ufs=3: all of it, from hakios_ufs_init, before the devices answer anything (ufs_ready false) */
static void
ufs_nufs_start(void)
{
	char soc[32] = "exynos9810";
	uint32_t ro = 0, step = 5;
	const char *why;
	int data_lun = -1;
	bool window = false;

	ufshc_log_direct(true);
	if (!PE_parse_boot_arg_str("ufs_native_soc", soc, sizeof(soc))) {
		uint32_t acpm_soc = 0;

		/* the A50: acpm_soc=9610 says the SoC (its boot-args are near the command line's limit) */
		if (PE_parse_boot_argn("acpm_soc", &acpm_soc, sizeof(acpm_soc)) && acpm_soc == 9610) {
			strlcpy(soc, "exynos9610", sizeof(soc));
		}
	}
	PE_parse_boot_argn("ufs_native_ro", &ro, sizeof(ro));
	PE_parse_boot_argn("ufs_native_verify", &ufs_nufs_verify, sizeof(ufs_nufs_verify));
	PE_parse_boot_argn("ufs_native_soak", &ufs_nufs_soak, sizeof(ufs_nufs_soak));
	ufs_nufs_ro = ro != 0;
	if (ufs_nufs_ro && ufs_write_part >= 0) {
		/* read only is read only: the loader's write path is not used either */
		ufs_write_part = -1;
		nufs_say("ufs: nufs: ufs_native_ro=1: nothing takes writes this run, through either way\n");
	}
	nufs_say("ufs: nufs: ufs=3: this kernel's own UFS driver (%s) takes the controller after a self-test against the "
	    "loader's reads; %s; the loader's block service is the reference and the fallback\n", soc, ufs_nufs_ro ?
	    "READ ONLY (ufs_native_ro=1)" : ufs_write_part >= 0 ? "USERDATA writable (ufs_write=1)" : "read only (no ufs_write=1)");

	nufs_say("ufs: nufs: step 1: references through the loader\n");
	nufs_sum.step = 1;
	why = nufs_references();
	if (why == NULL && ufs_nufs_soak != 0) {
		why = nufs_soak_references();
	}
	if (why != NULL) {
		nufs_say("ufs: nufs: not taken: %s; through the loader (ufs=2), nothing changed\n", why);
		nufs_why(why);
		goto out;
	}
	nufs_say("ufs: nufs: step 2: the controller taken as UEFI left it (own mode, step %u)\n", step);
	nufs_sum.step = 2;
	if (ufs_nufs_ro || ufs_write_part < 0) {
		ufshc_force_ro();       /* (the whitelist's own fence, from before this kernel's list is in) */
	}
	lck_mtx_lock(&ufs_lock);        /* no request of the loader's in flight, nor any started meanwhile */
	/*
	 * The loader's blocks 0.. of its first unit (LUN 0 on the S9): the reference own mode's layout read
	 * (READ(10) of LUN 0's blocks 0-1 through its own descriptor) must match byte for byte.
	 */
	if (!ufshc_take(soc, step, &ufs_page->kernel_native.list_in, ufs_nufs_nunits > 0 ? ufs_nufs_units[0].ref : NULL,
	    ufs_nufs_nunits > 0 ? ufs_nufs_units[0].head : 0)) {
		const char *taken = ufshc_take_why();
		char reason[sizeof(nufs_sum.why)];

		lck_mtx_unlock(&ufs_lock);
		if (ufshc_release("not taken")) {
			snprintf(reason, sizeof(reason), "the controller could not be taken (step 2: %s)", taken != NULL ? taken :
			    "the lines before say why");
			nufs_say("ufs: nufs: not taken (%s); UEFI's list is in: through the loader (ufs=2)\n", taken != NULL ?
			    taken : "above");
			nufs_why(reason);
		} else {
			ufs_dead = true;
			snprintf(reason, sizeof(reason), "the controller could not be taken (%s), and UEFI's list is NOT back",
			    taken != NULL ? taken : "above");
			nufs_say("ufs: nufs: not taken, and UEFI's list is NOT back: no storage in this run\n");
			nufs_why(reason);
		}
		goto out;
	}
	lck_mtx_unlock(&ufs_lock);
	nufs_say("ufs: nufs: step 3: self-test\n");
	nufs_sum.step = 3;
	why = nufs_selftest();
	nufs_sum.selftest = why == NULL ? 1 : -1;
	if (why != NULL) {
		nufs_abort(why);
		goto out;
	}
	if (ufs_nufs_soak != 0) {
		nufs_say("ufs: nufs: step 3: soak (ufs_native_soak=%u)\n", ufs_nufs_soak);
		why = nufs_soak_run();
		nufs_sum.soak = why == NULL ? 1 : -1;
		if (why != NULL) {
			nufs_abort(why);
			goto out;
		}
	}
	if (ufs_write_part >= 0) {
		int k = nufs_unit_of((uint32_t)ufs_write_part);

		data_lun = k >= 0 ? ufs_nufs_units[k].lun : 0xff;
		if (data_lun == 0xff || strncmp((const char *)ufs_page->part[ufs_write_part].name, UFS_DATA_PART,
		    sizeof(UFS_DATA_PART)) != 0) {
			data_lun = -1;
		}
	}
	nufs_say("ufs: nufs: step 4: the fences\n");
	nufs_sum.step = 4;
	if (!nufs_fences(ufs_nufs_ro ? -1 : data_lun)) {
		ufs_write_part = -1;
	}
	if (!ufs_nufs_ro && ufs_write_part >= 0 && data_lun >= 0) {
		volatile struct loader_block_part *d = &ufs_page->part[ufs_write_part];

		window = ufshc_open_window((uint8_t)data_lun, d->start_lba, d->blocks) == 0;
		if (!window) {
			nufs_say("ufs: nufs: USERDATA's window could not be opened: everything read only\n");
			ufshc_force_ro();
			ufs_write_part = -1;
		}
	} else {
		ufshc_force_ro();
		if (ufs_write_part >= 0) {
			nufs_say("ufs: nufs: USERDATA is not where it should be: everything read only\n");
			ufs_write_part = -1;
		}
	}
	if (!nufs_prove_fences(data_lun, window)) {
		nufs_abort("the fences did not refuse what they must");
		goto out;
	}
	nufs_say("ufs: nufs: step 5: switched: every read%s through this kernel's own driver from now on; the loader is asked "
	    "for nothing (core 0 stays the loader's)\n", window ? ", and USERDATA's writes," : "");
	nufs_sum.step = 5;
	ufs_native_mapped = true;
	ufs_native_on = true;
	ufs_nufs_on = true;
out:
	nufs_soak_free();
	nufs_free_units();
	if (ufs_nufs_spots != NULL) {
		kfree_data(ufs_nufs_spots, NUFS_SPOTS_SIZE);
		ufs_nufs_spots = NULL;
	}
}

/*
 * A read through this kernel's own driver failed after the switch: the controller back to UEFI and the loader from
 * now on, if it can be given back; no storage otherwise. 0: the loader may be asked.
 */
static int
ufs_nufs_failed(const char *name, uint64_t offset, int error)
{
	int e = 0;

	lck_mtx_lock(&ufs_nufs_mtx);
	if (ufs_nufs_on) {
		nufs_say("ufs: nufs: %s: a read at byte %llu failed here (%d): giving the controller back\n", name, offset, error);
		lck_mtx_lock(&ufs_write_mtx);           /* no write of ours in the middle */
		nufs_abort("a read failed after the switch");
		lck_mtx_unlock(&ufs_write_mtx);
	}
	if (ufs_dead) {
		e = EIO;
	}
	lck_mtx_unlock(&ufs_nufs_mtx);
	return e;
}

/* USERDATA's whole blocks, through the window (the whitelist refuses anything else again); ufs_write_mtx held */
static int
ufs_nufs_write(uint32_t part, uint64_t offset, const uint8_t *src, uint64_t len)
{
	int e;

	if (ufs_lun[part] == 0xff) {
		nufs_refused("a write to %s: on a unit this kernel's driver does not write", nufs_pname(part));
		return EROFS;
	}
	e = ufshc_write(ufs_lun[part], ufs_lun_offset[part] + offset, src, len);
	if (e == 0 && ufs_nufs_verify != 0) {
		uint8_t *back = kalloc_data((vm_size_t)len, Z_WAITOK);

		if (back == NULL || ufshc_read(ufs_lun[part], ufs_lun_offset[part] + offset, back, len) != 0) {
			e = EIO;
		} else if (memcmp(back, src, (size_t)len) != 0) {
			if (ufs_nufs_verify_bad++ < 8) {
				nufs_say("ufs: nufs: %s bytes %llu..%llu read back DIFFERENT\n", (const char *)ufs_page->part[part].name,
				    offset, offset + len - 1);
			}
			e = EIO;
		}
		if (back != NULL) {
			kfree_data(back, (vm_size_t)len);
		}
	}
	if (e != 0) {
		ufs_write_errors++;
		if (ufs_write_errors <= 8) {
			nufs_say("ufs: nufs: %s: a write of bytes %llu..%llu failed (%d); not tried the other way\n",
			    (const char *)ufs_page->part[part].name, offset, offset + len - 1, e);
		}
		return e;
	}
	ufs_writes++;
	ufs_write_bytes += len;
	return 0;
}

/* ---- bringing it up ---- */

/* the block and character device of every partition (not of the units): how many were made */
static uint32_t
ufs_make_nodes(void)
{
	uint32_t made = 0;

	ufs_bmajor = bdevsw_add(-1, &ufs_bdevsw);
	ufs_cmajor = ufs_bmajor < 0 ? -1 : cdevsw_add_with_bdev(-1, &ufs_cdevsw, ufs_bmajor);
	if (ufs_cmajor < 0) {
		printf("ufs: no device numbers to be had\n");
		return 0;
	}
	for (uint32_t i = 0; i < ufs_parts; i++) {
		char name[48];

		ufs_bsize[i] = ufs_page->part[i].block_size;
		if (ufs_page->part[i].flags & BLOCK_PART_UNIT) {
			continue;               /* the units are for this kernel's own driver to be checked against */
		}
		snprintf(name, sizeof(name), "ufs_%s", ufs_page->part[i].name);
		if (devfs_make_node(makedev(ufs_bmajor, i), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "%s", name) == NULL) {
			printf("ufs: no node for %s (the same name twice?)\n", name);
			continue;
		}
		snprintf(name, sizeof(name), "rufs_%s", ufs_page->part[i].name);
		(void)devfs_make_node(makedev(ufs_cmajor, i), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "%s", name);
		made++;
	}
	return made;
}

void
hakios_ufs_init(void)
{
	uint64_t pa = 0, buffer_pa;
	uint32_t check = 0, made = 0, native = 0;
	vm_offset_t base;

	PE_parse_boot_argn("ufs_native", &native, sizeof(native));
	/* ufs=3: the loader serves as with ufs=2, and this kernel's own driver takes over after its self-test */
	check = 0;
	ufs_nufs = PE_parse_boot_argn("ufs", &check, sizeof(check)) && check == 3;
	check = 0;
	ufs_nufs_ro = PE_parse_boot_argn("ufs_native_ro", &check, sizeof(check)) && check != 0;
	if (!PE_parse_boot_argn("ufs_shared_pa", &pa, sizeof(pa)) || pa == 0) {
		if (ufs_nufs) {
			nufs_say("ufs: nufs: ufs=3, but the loader published no page (a loader older than ufs=3?): no storage\n");
			nufs_why("the loader published no page (ufs_shared_pa): no storage");
			return;
		}
		/* the loader was not asked for ufs=2: nothing to read through, unless this kernel has the controller */
		if (native == 4 || native == 5) {
			ufs_own_init(native);
		}
		return;
	}
	if (native == 4 || native == 5) {
		printf("ufs: ufs_native=%u refused: the loader serves the storage (ufs=2), and the two would drive one "
		    "controller; through the loader\n", native);
		native = 0;
	}
	if (ufs_nufs && native != 0) {
		nufs_say("ufs: nufs: ufs_native=%u ignored: ufs=3 is its own way of taking the controller\n", native);
		native = 0;
	}
	base = ml_io_map_unmappable((vm_offset_t)(pa & ~(UFS_PAGE - 1)),
	    ((pa & (UFS_PAGE - 1)) + sizeof(struct loader_block) + UFS_PAGE - 1) & ~(UFS_PAGE - 1), VM_WIMG_WCOMB);
	ufs_page = (volatile struct loader_block *)(base + (pa & (UFS_PAGE - 1)));
	if (ufs_page->magic != BLOCK_MAGIC || ufs_page->version != BLOCK_VERSION ||
	    ufs_page->size != sizeof(struct loader_block) || ufs_page->parts > BLOCK_MAX_PARTS) {
		printf("ufs: the loader's page at %llx is not one this kernel knows (magic %llx, version %u, size %u)\n",
		    pa, ufs_page->magic, ufs_page->version, ufs_page->size);
		return;
	}
	ufs_parts = ufs_page->parts;
	ufs_buffer_size = ufs_page->buffer_size;
	buffer_pa = ufs_page->buffer_pa;
	ufs_buffer = (const uint8_t *)(ml_io_map_unmappable((vm_offset_t)(buffer_pa & ~(UFS_PAGE - 1)),
	    ((buffer_pa & (UFS_PAGE - 1)) + ufs_buffer_size + UFS_PAGE - 1) & ~(UFS_PAGE - 1), VM_WIMG_DEFAULT) +
	    (buffer_pa & (UFS_PAGE - 1)));
	ufs_seq = ufs_page->answer.answered;      /* a loader that served an earlier kernel counts on from there */

	made = ufs_make_nodes();
	if (ufs_cmajor < 0) {
		return;
	}
	ufs_ready = true;
	printf("ufs: %u partitions of the phone's storage as /dev/ufs_NAME and /dev/rufs_NAME, read only, "
	    "through the loader's page at %llx and its %u KB buffer\n", made, pa, ufs_buffer_size >> 10);

	/*
	 * ufs_write=1: /dev/rufs_darwin takes writes, past the Darwin volume and the cryptexes. darwin
	 * has to be the partition the port was set up with, in everything the loader's table says of
	 * it; the loader looks at its GUIDs besides, and refuses the same bounds of its own accord.
	 * Or, on a port with no darwin, the partition its loader offers by name (UFS_DATA_PART), all
	 * of it: the loader's offer has to say block 0 and this name, and nothing else is taken.
	 */
	check = 0;
	if (PE_parse_boot_argn("ufs_write", &check, sizeof(check)) && check == 1 && ufs_nufs && ufs_nufs_ro) {
		nufs_say("ufs: nufs: ufs_write=1 ignored: ufs_native_ro=1 makes the whole run read only\n");
		check = 0;
	}
	if (check == 1) {
		uint32_t offered = ufs_page->write_part_plus_one;

		if (offered == 0 || offered > ufs_parts) {
			printf("ufs: the loader offers no partition to write into (ufs_write=1 on its command "
			    "line too, and a loader that knows about writes)\n");
		}
		for (uint32_t i = 0; offered != 0 && i < ufs_parts; i++) {
			const char *name = (const char *)ufs_page->part[i].name, *why = NULL;
			bool darwin = strncmp(name, "darwin", 7) == 0;

			if (!darwin && strncmp(name, UFS_DATA_PART, sizeof(UFS_DATA_PART)) != 0) {
				continue;
			}
			if (i != offered - 1) {
				why = "the loader offers another partition";
			} else if (ufs_page->write_first != (darwin ? UFS_WRITE_FIRST : 0)) {
				why = "the loader would write it from another block";
			} else if (ufs_page->part[i].flags & BLOCK_PART_UNIT) {
				why = "it is a whole logical unit, not the partition";
			} else if (ufs_page->part[i].unit != 0) {
				/* the first unit UEFI lists, which this kernel's own driver reads as LUN 0 and
				 * has compared with the loader's reads; the device path this firmware gives
				 * says no logical unit number at all, so part[].lun is 0xff for every unit */
				why = "it is not on the first logical unit";
			} else if (ufs_page->part[i].blocks < (darwin ? UFS_DARWIN_MIN : UFS_DATA_PART_MIN)) {
				why = darwin ? "it is smaller than the layout's 20 GiB" : "it is smaller than 1 GiB";
			} else if (ufs_page->part[i].block_size != 4096) {
				why = "its blocks are not 4096 bytes";
			}
			if (why != NULL) {
				printf("ufs: %s: no write will be asked for: %s\n", name, why);
				continue;
			}
			ufs_write_part = (int)i;
			ufs_write_first = darwin ? UFS_WRITE_FIRST : 0;
		}
		if (ufs_write_part >= 0) {
			const char *name = (const char *)ufs_page->part[ufs_write_part].name;

			printf("ufs: /dev/rufs_%s takes writes, whole blocks from %llu on (%llu MB of it); "
			    "everything else of the storage is read only\n", name, ufs_write_first,
			    (ufs_page->part[ufs_write_part].blocks - ufs_write_first) * 4096 >> 20);
			if (devfs_make_node(makedev(ufs_bmajor, UFS_DATA_MINOR), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640,
			    "ufs_%s_data", name) != NULL) {
				printf("ufs: /dev/ufs_%s_data: the same %llu MB as a block device, for the data volume; "
				    "%s\n", name, ufs_data_bytes() >> 20, ufs_data_op() == BLOCK_OP_WRITE_FAST ?
				    "writes flushed when the file system asks" : "every write flushed and read back (an older loader)");
			}
		} else {
			printf("ufs: nothing takes writes: neither darwin nor " UFS_DATA_PART " is offered as it should be\n");
		}
	}

	if (ufs_nufs) {
		/* (nothing answers from the devices meanwhile: the loader's mailbox and the controller change hands here) */
		uint32_t every = 0, fps = 0;
		thread_t thread;

		ufs_ready = false;
		ufs_nufs_start();
		ufs_ready = true;
		hakios_ufs_summary("after the start", HAKIOS_UFS_SAY_LOG | HAKIOS_UFS_SAY_PRINTF);
		PE_parse_boot_argn("ufs_native_log", &every, sizeof(every));
		PE_parse_boot_argn("fps_log", &fps, sizeof(fps));
		if ((every != 0 || fps != 0) && kernel_thread_start(nufs_summary_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}

	/*
	 * ufs_native=1: through this kernel's own driver instead, its own transfer list in the
	 * controller for the run, if all is as expected; 2: only look; 3: a dry run -- the list in and
	 * out again, and in once more for the end of the run to take out, no command sent.
	 * (Not with ufs=3: ufs_nufs_start has mapped the units, and its map is the one the reads use.)
	 */
	check = native;
	if (!ufs_nufs) {
		memset(ufs_lun, 0xff, sizeof(ufs_lun));
	}
	if (check >= 1 && check <= 3) {
		struct ufshc_loader offer = {
			.hc_pa = ufs_page->native.hc_pa, .area_pa = ufs_page->native.area_pa,
			.area_size = ufs_page->native.area_size, .ucd_offset = ufs_page->native.ucd_offset,
			.ucd_size = ufs_page->native.ucd_size, .slots = ufs_page->native.slots,
			.uefi_lo = ufs_page->native.uefi_utrlba, .uefi_hi = ufs_page->native.uefi_utrlbau,
			.take_back = &ufs_page->loader_native.take_back, .list_in = &ufs_page->kernel_native.list_in,
			.switched_in = (uint32_t *)&ufs_page->kernel_native.switched_in,
			.switched_out = (uint32_t *)&ufs_page->kernel_native.switched_out,
			.cnr_at_switch = &ufs_page->kernel_native.cnr_at_switch,
		};
		uint32_t mode = check, compare = 0;

		PE_parse_boot_argn("ufs_native_check", &compare, sizeof(compare));
		if (!ufshc_probe(&offer, mode == 2)) {
			if (mode != 2) {
				printf("ufs: this kernel's own UFS driver is not used: through the loader\n");
			}
		} else if (mode == 3) {
			printf("ufs: native: dry run: this kernel's list in and out, no command\n");
			if (ufs_native_in(false) == 0) {
				IOSleep(1000);
				ufs_native_out("dry run");
				IOSleep(500);
				ufs_native_in(false);
				printf("ufs: native: dry run: the list stays in for the end of the run\n");
			}
		} else if (ufs_native_map(true)) {
			ufs_native_mapped = true;
			if (compare != 0) {
				/* right here, in bsd_init's thread, and then back to the loader */
				ufs_native_check();
				hakios_ufs_report("after the check");
				loader_return();
			}
			ufs_native_on = true;
			compare = 0;
			if (PE_parse_boot_argn("ufs_native_bench", &compare, sizeof(compare)) && compare != 0) {
				ufs_native_bench();
				hakios_ufs_report("after the bench");
				loader_return();
			}
			printf("ufs: the partitions (LUN 0-%u) are read by this kernel's own UFS driver, its list in the "
			    "controller (kern.hakios.ufs_native=0: through the loader)\n", ufs_native_luns - 1);
		} else {
			printf("ufs: this kernel's own UFS driver is not used: through the loader\n");
		}
	}

	if (PE_parse_boot_argn("ufs_bench", &check, sizeof(check)) && check != 0) {
		/* right here, in bsd_init's thread, and then back to the loader: nothing else is wanted */
		ufs_bench(NULL, THREAD_AWAKENED);
		loader_return();
	}
	check = 0;
	if (PE_parse_boot_argn("ufs_check", &check, sizeof(check)) && check == 2) {
		/* right here, whichever way the partitions are read (ufs_native=), and back to the loader */
		ufs_check((void *)1, THREAD_AWAKENED);
		hakios_ufs_report("after the check");
		loader_return();
	}
	if (check == 1) {
		thread_t thread;

		if (kernel_thread_start(ufs_check, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
