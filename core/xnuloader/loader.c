/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
#include "identity.h"         /* who Darwin shall think we are: from the iPhone's own device tree (mkidentity.py) */

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

/*
 * Where the boot shim (or QEMU's loader device, or U-Boot's hakios command) puts the payload, and where the firmware's
 * boot manager finds loader.efi in it: fixed in the UEFI image (hakios-uefi, XnuLoader.h) and the shim. A platform with a
 * payload_home moves it from there at the start (payload_move) and works from payload_base after that.
 */
#define PAYLOAD_BASE        0xF0000000ULL
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */
static uint64_t payload_base = PAYLOAD_BASE;   /* where the payload is read from: PAYLOAD_BASE, or its home once moved */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

#define LOG_PARTITION       u"logdump"
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (payload_base + PAYLOAD_MAX_SIZE - 0x100000ULL)
#define PAYLOAD_MAX_SIZE    0x04000000ULL         /* RAM reserved for the payload in the UEFI memory map (and taken at its home) */

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;
/*
 * And while the kernel runs, whatever started the loader: under HakiOS's U-Boot (the menu of an installed HakiOS) there
 * is no USB command loop, the text console is U-Boot's, drawn into the framebuffer the kernel shows Darwin in, and every
 * line core 0 printed during a run (the storage's, the battery's, the watchdog's) ran over the system's screen.
 * What it was before the run, to go back to.
 */
static int screen_quiet_before_run;

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && !screen_quiet) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d (64-bit values), %% */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

static efi_block_io *
find_log_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = LOG_PARTITION;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			/* (the handle array is pool memory the caller owns; this runs on every log write) */
			BS->free_pool(handles);
			return bio;
		}
	}
	BS->free_pool(handles);
	return NULL;
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
/* aligned for any firmware's block I/O: U-Boot's wants a buffer aligned to the block size (media->io_align) */
static char out_buf[2 * 1024 * 1024] __attribute__((aligned(0x2000)));
/* the storage's reads for the kernel land here (block_poll), and nothing else may be put in it */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
/*
 * Where the log is read back to compare it (write_logs). Not block_buffer: that is the kernel's
 * mailbox buffer, and a read-back landing in it while the kernel runs on the other core would
 * overwrite data the kernel is still copying out of it.
 */
static uint8_t log_verify[sizeof(out_buf) + 8192] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;
/*
 * The kernel's log of the last run, once it is back (write_logs): every later write of the log (flush_log(), the one
 * before the restart among them) keeps it behind the loader's. Before, the restart's flush put the loader's log alone
 * into the RAM log, over the kernel's. run_kernel() forgets it when the next kernel is loaded.
 */
static const char *last_klog;
static uint64_t last_klog_len;

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > sizeof(out_buf) - n - 8192) {
			klog_len = sizeof(out_buf) - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

/*
 * A phone with no log partition (the Galaxy S9: logdump is Qualcomm's) keeps the log in RAM that a restart leaves
 * alone, as one of Linux's persistent RAM buffers (fs/pstore/ram_core.c: "DBGC", the start, the size, the bytes), in a
 * place its own kernel's pstore reads back from: after the next start of Android or TWRP the log is a file in
 * /sys/fs/pstore. What fits: the head of the loader's log and the tail of everything after it.
 */
static uint64_t ram_log_base, ram_log_size;     /* the platform's; 0: none */

static void
write_ram_log(void)
{
	struct { uint32_t sig, start, size; } *h = (void *)(uintptr_t)ram_log_base;
	uint8_t *data = (uint8_t *)(uintptr_t)ram_log_base + sizeof(*h);
	uint64_t room = ram_log_size - sizeof(*h), head = 3072, n = staged_len;
	static const char cut[] = "\n[...]\n";

	if (ram_log_base == 0 || ram_log_size <= sizeof(*h) + head + sizeof(cut)) {
		return;
	}
	if (n <= room) {
		memcpy(data, staged_data, n);
	} else {
		memcpy(data, staged_data, head);
		memcpy(data + head, cut, sizeof(cut) - 1);
		memcpy(data + head + sizeof(cut) - 1, staged_data + n - (room - head - (sizeof(cut) - 1)),
		    room - head - (sizeof(cut) - 1));
		n = room;
	}
	h->sig = 0x43474244;    /* "DBGC" */
	h->start = 0;
	h->size = (uint32_t)n;
	clean_dcache(ram_log_base, ram_log_base + sizeof(*h) + n);
}

/*
 * The pstore's other half, where the platform has one (ram_dump): Linux's ramoops keeps there one record of the kind a
 * panic leaves (dmesg-ramoops-0 in /sys/fs/pstore), and it keeps it only behind its header line ("====sec.nsec-D",
 * fs/pstore/ram.c, ramoops_read_kmsg_hdr). It gets the kernel's log from its start, where ram_log has the end: twice
 * the log of a run that comes back.
 */
static uint64_t ram_dump_base, ram_dump_size;   /* the platform's; 0: none */

/* At the start: no record of a run before, which this one could be taken for if its kernel never comes back */
static void
clear_ram_dump(void)
{
	if (ram_dump_base != 0 && ram_dump_size >= sizeof(uint32_t)) {
		*(volatile uint32_t *)(uintptr_t)ram_dump_base = 0;
		clean_dcache(ram_dump_base, ram_dump_base + sizeof(uint32_t));
	}
}

static void
write_ram_dump(const char *klog, uint64_t klog_len)
{
	struct { uint32_t sig, start, size; } *h = (void *)(uintptr_t)ram_dump_base;
	uint8_t *data = (uint8_t *)(uintptr_t)ram_dump_base + sizeof(*h);
	static const char hdr[] = "====0.0-D\n== THE KERNEL'S LOG FROM ITS START (its end: pmsg-ramoops-0) ==\n";
	uint64_t room, n;

	/*
	 * Only a kernel's log is written: flush_log() comes here with none after it (round 3 on the S9 lost the record
	 * that way). Last run's record is taken away once, at the start (clear_ram_dump).
	 */
	if (ram_dump_base == 0 || ram_dump_size <= sizeof(*h) + sizeof(hdr) || klog == NULL || klog_len == 0) {
		return;
	}
	room = ram_dump_size - sizeof(*h) - (sizeof(hdr) - 1);
	n = klog_len < room ? klog_len : room;
	memcpy(data, hdr, sizeof(hdr) - 1);
	memcpy(data + sizeof(hdr) - 1, klog, n);
	h->sig = 0x43474244;    /* "DBGC" */
	h->start = 0;
	h->size = (uint32_t)(sizeof(hdr) - 1 + n);
	clean_dcache(ram_dump_base, ram_dump_base + sizeof(*h) + h->size);
}

static void wdt_pet(void);
static uint64_t timer_hz(void);

/*
 * The returned kernel's log on the console, for a machine with no log partition (QEMU, the Galaxy S9). Not into the
 * loader's own log (log_buf, 32 KB): the kernel's is in the staged log already, behind the loader's. On the S9 the
 * console is UEFI's text console on the panel, a character at a time: a run's log of a megabyte or two takes minutes
 * there. Meanwhile the watchdog is fed (it bit in the middle of the echo before: a reset that loses INFORM3, so the
 * phone came back into BOOT, not TWRP), and where the log is in a RAM log already the echo stops after ECHO_SECONDS.
 */
#define ECHO_SECONDS    20

static void
echo_kernel_log(const char *klog, uint64_t klog_len)
{
	static const char cut[] = "\n[... the rest is in the RAM log: not shown here ...]";
	uint64_t started = READ_SYSREG(CNTVCT_EL0), limit = ram_log_base != 0 ? ECHO_SECONDS * timer_hz() : 0;
	char16 w[3] = { 0, 0, 0 };

	print("\n== KERNEL LOG ==\n");
	if (ST->con_out != NULL && !screen_quiet) {
		for (uint64_t i = 0; i < klog_len; i++) {
			if ((i & 1023) == 0) {
				wdt_pet();
				if (limit != 0 && READ_SYSREG(CNTVCT_EL0) - started >= limit) {
					for (const char *c = cut; *c; c++) {
						w[0] = (char16)*c, w[1] = 0;
						ST->con_out->output_string(ST->con_out, w);
					}
					print("\n(the console's echo of the kernel's log stopped after %d s, at byte %d of %d)",
					    (uint64_t)ECHO_SECONDS, i, klog_len);
					break;
				}
			}
			if (klog[i] == '\n') {
				w[0] = '\r', w[1] = '\n';
			} else {
				w[0] = (char16)klog[i], w[1] = 0;
			}
			ST->con_out->output_string(ST->con_out, w);
		}
		wdt_pet();
	}
	print("\n== END OF LOG ==\n");
}

static void
write_logs_echo(const char *klog, uint64_t klog_len, int echo)
{
	efi_block_io *bio = find_log_partition();

	compose_logs(klog, klog_len);
	write_ram_log();
	write_ram_dump(klog, klog_len);
	if (bio == NULL) {
		/* No log partition (QEMU, the S9): echo the returned kernel log on the console instead. */
		if (klog != NULL && echo) {
			echo_kernel_log(klog, klog_len);
			/* and the log again with the lines the echo added, the kernel's still behind them */
			compose_logs(klog, klog_len);
			write_ram_log();
		}
		return;
	}
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
	efi_status ws, rs = 0;
	int same = 0;

	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to logdump: UEFI's transfer list is not the one in the controller\n");
		}
		compose_logs(klog, klog_len);
		return;
	}
	ws = bio->write_blocks(bio, bio->media->media_id, 0, len, out_buf);

	if (!EFI_ERROR(ws)) {
		bio->flush_blocks(bio);
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(log_verify)) {
			rs = bio->read_blocks(bio, bio->media->media_id, 0, len, log_verify);
			flush_dcache((uint64_t)(uintptr_t)log_verify, (uint64_t)(uintptr_t)log_verify + len);
			same = !EFI_ERROR(rs) && memcmp(log_verify, out_buf, len) == 0;
		}
	}
	print("log: %d bytes written to logdump (%x), read back %s (%x)\n", len, ws,
	    EFI_ERROR(ws) ? "not tried" : same ? "the same" : "DIFFERENT", rs);
	compose_logs(klog, klog_len);
}

/* the log, and a kernel's that came back (kept for the writes after it: last_klog) */
static void
write_logs(const char *klog, uint64_t klog_len)
{
	if (klog != NULL) {
		last_klog = klog;
		last_klog_len = klog_len;
	}
	write_logs_echo(klog, klog_len, 1);
}

/* the loader's log as it is now, the last run's kernel log (if there is one) behind it, not echoed again */
static void
flush_log(void)
{
	write_logs_echo(last_klog, last_klog_len, 0);
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer                                                 */
/*                                                                          */
/* node := u32 nProperties, u32 nChildren, props[], children[]              */
/* prop := char name[32], u32 length, value padded to 4 bytes               */

struct dt_writer {
	uint8_t *base, *cur;
	uint8_t *end;         /* the first byte after the room reserved for the tree */
	int overflow;         /* something did not fit: nothing more is written, run_kernel refuses the tree */
	uint32_t scratch[2];  /* the counts of a node that was not written (after an overflow) */
	uint32_t *counts[16]; /* [depth] -> {nProperties, nChildren} of the open node */
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	uint64_t padded = ((uint64_t)len + 3) & ~3ULL;

	/*
	 * Not past the room reserved for the tree (boot_args follows it), and not a name longer than the
	 * 32-byte field: either would overwrite what is next to it with no message.
	 */
	if (w->overflow || str_len(name) > 32 ||(uint64_t)(w->end - w->cur) < 36 + padded) {
		w->overflow = 1;
		return;
	}
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (size_t)padded);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + padded;
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

/* Seconds since 1970 by the firmware's real-time clock, or 0 when it has none or it is not set. */
static uint64_t
firmware_epoch(void)
{
	static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	struct efi_time t;
	uint64_t days;

	if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
	    t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
		return 0;
	}
	days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
	if (t.month > 2 && t.year % 4 == 0) {
		days++;
	}
	uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
	if (t.timezone != 0x7FF) {
		secs += (int64_t)t.timezone * 60;       /* local time = UTC - timezone */
	}
	return secs;
}

/* A string in a field of fixed size, the rest zeros. */
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

/*
 * Properties copied from the iPhone's device tree (identity.h), but one: the iPhone X's display is a
 * wide-gamut one ("artwork-display-gamut" P3), and with it Darwin takes the screen for one that shows
 * extended colour (MobileGestalt's HasExtendedColorDisplay): WebKit then draws a page's tiles into
 * surfaces of ten bits a colour ('w30r', 'b3a8'), which neither the kernel's surfaces nor the window
 * server's renderer here do (eight bits, BGRA), and every page comes out black. Here the screen is
 * what the framebuffer is: sRGB.
 */
static int gamut_p3;    /* boot-arg gamut=p3 (a test): the iPhone's own "P3" left in place */

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut") && !gamut_p3) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* One property of those copied from the iPhone's device tree, by name. */
static void
dt_identity_one(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, name)) {
			dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
		}
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	if (w->overflow || (uint64_t)(w->end - w->cur) < 8) {
		/* no room: the node is not written, the depth is kept so that dt_end still pairs with it */
		w->overflow = 1;
		w->counts[w->depth++] = w->scratch;
		return;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

#define PLATFORM_KEEP_OUT_MAX 16

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3 */
	uint64_t gic_cpu;                /* not 0: the GIC is a GICv2 (GIC-400), with its CPU interface here and no redistributors */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR: the phone numbers them in the second affinity field */
	const uint32_t *core_mpidr;      /* ... or this: core N's MPIDR, for a phone whose cores are in two clusters */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
	uint64_t ram_log, ram_log_size;  /* where the log is kept across a restart when there is no log partition (write_ram_log) */
	uint64_t ram_dump, ram_dump_size; /* ... and the start of the kernel's, in ramoops' dmesg record (write_ram_dump) */
	uint64_t ufs_hc;                 /* the UFS host controller the kernel's own driver may take over (ufs_native=); 0: none */
	uint64_t timer_hz;               /* the timer's frequency where CNTFRQ_EL0 reads 0 (the firmware never set it), 0: none */
	uint64_t recovery_pmu;           /* Samsung: the PMU whose INFORM registers send a restart into recovery (recovery_after_reset) */
	uint32_t pmu_swreset;            /* ... and its SWRESET register's offset: the restart itself (platform_restart), 0: none */
	uint64_t gic_redist_size;        /* the redistributors' region, where it is not 1 MB (GICv4 frames are 256 KB a core) */
	const char *compatible;          /* told apart by this in the device tree's root "compatible" instead (platform_from_fdt) */
	uint64_t exynos_wdt;             /* Samsung Exynos: the cluster watchdog armed for the run (exynos_wdt_start), 0: none */
	uint64_t exynos_wdt_other;       /* ... the other cluster's, stopped */
	uint32_t exynos_wdt_pmu_bit;     /* ... its bit in recovery_pmu's MASK_WDT_RESET_REQUEST and DISABLE_WDT */
	uint32_t parked_cores;           /* a bit for each core the boot loader leaves running in a loop of its own: never the kernel's */
	uint8_t affinity_unreliable;     /* PSCI AFFINITY_INFO says "on" of cores that are off: CPU_ON's answer is what counts */
	uint64_t payload_home;           /* not 0: the payload is moved here (or a 64 MiB step above) from PAYLOAD_BASE at the start */
	struct { uint64_t base, size; const char *what; } keep_out[PLATFORM_KEEP_OUT_MAX];
	const char *data_part;           /* a port with no darwin: the one partition, by name, ufs_write=1 may write (block_is_data_part) */
	const struct platform *sibling_of; /* not NULL: told apart from this one only by loader_platform= (platform_sibling) */
};

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55s (1.8 GHz at most), 4-7 Cortex-A78s (2.4 GHz): a big one is about
	 * 2.5 times as fast. The kernel uses the small ones only when the big ones are all busy
	 * (XNU's machine_routines.c, ml_choose_slow_core_partners()).
	 */
	.small_cores = 0x0f,
	.fb_base = 0xE1000000,
	.ufs_hc = 0x1d84000,             /* ufshc@1d84000, "ufs_mem", in this phone's Android device tree */
};

static const struct platform platform_starlte = {
	/*
	 * Samsung Galaxy S9 (SM-G960F), Exynos 9810, under Project Silicium's starlte UEFI. Every value is from
	 * the phone's own device tree (devices/starlte/README.md): the SoC's registers from 0x10000000, a GIC-400
	 * (distributor 0x10101000, CPU interface 0x10102000), PSCI by SMC, the panel's buffer at 0xCC000000.
	 */
	.name = "starlte", .io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x10101000, .gic_cpu = 0x10102000, .uart = 0,
	/*
	 * Cores 0-3 are Cortex-A55s (MPIDR 0x000-0x003, 1.8 GHz at most), 4-7 Samsung's own Exynos M3s (0x100-0x103,
	 * 2.7 GHz), started by Carbon's PSCI. The M3s implement ARMv8.0 where the A55s implement 8.2: a kernel that is
	 * to use both boots on an M3 (bootcpu=4), so that what it says the processor can do is what every core can
	 * (devices/starlte/README.md).
	 */
	.cores = 8, .core_mpidr = (const uint32_t[]){ 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 },
	.small_cores = 0x0f,
	/*
	 * The Exynos 9810's EL3 leaves CNTFRQ_EL0 at 0 (Silicium's S5E9810Pkg: HAS_CNTFRQ_EL0_EMPTY_ERRATA, the frequency
	 * given instead as 26 MHz; Samsung's device tree: the timer's clock-frequency 26000000), and only EL3 can write it.
	 */
	.timer_hz = 26000000,
	.fb_base = 0xCC000000,
	/*
	 * Samsung's pstore (exynos-snapshot's log_pstore, 0xFED10000 + 32 KB), its second half: the pmsg zone, where the
	 * WoA port for the S9+ kept its notes across restarts and read them back in TWRP. Reserved in this port's UEFI
	 * memory map (devices/starlte/uefi).
	 */
	.ram_log = 0xFED14000, .ram_log_size = 0x4000,
	/*
	 * Its first half: ramoops' one dmesg record (Samsung sets record_size and pmsg_size to half of log_pstore each,
	 * the dmesg records coming first). The start of the kernel's log goes there.
	 */
	.ram_dump = 0xFED10000, .ram_dump_size = 0x4000,
	/*
	 * With the kernel on a core of its own (bootcpu=, the S9's default), this core watches it and writes the log on the
	 * way, and when the kernel hangs: into the RAM log, which survives the restart that follows.
	 */
	.log_other_cpu = 1,
	/*
	 * The PMU (system-controller@14060000 in the phone's device tree): its INFORM registers tell S-Boot where a restart
	 * is to go, and a run's restart goes to recovery, where its log is read (recovery_after_reset).
	 */
	.recovery_pmu = 0x14060000,
	/*
	 * The data volume (/private/var kept between runs, ufs_write=1 var_hfs=ufs_USERDATA_data): this phone has no
	 * darwin, and Android's USERDATA, given up for it, is the only partition a write may reach, the whole of it.
	 */
	.data_part = "USERDATA",
	/*
	 * The restart: the PMU's SWRESET (+0x400), 1 written, as Samsung's own kernel restarts the phone (exynos-reboot.c,
	 * exynos_reboot(), after sec_reboot() has set INFORM2/3). Only that kind of reset keeps the INFORM registers: S-Boot
	 * reads them as 0 after the watchdog's (platform_restart).
	 */
	.pmu_swreset = 0x400,
	/*
	 * Cluster 1's watchdog (watchdog_cl1@10060000, PMU bit 23), armed for the whole run as the mainline Linux port to the
	 * S9+ arms it: a hang of the whole SoC becomes a restart (exynos_wdt_start; wdt=0: not), into BOOT on this S9, as its
	 * reset clears INFORM3 (platform_restart). Cluster 0's (watchdog_cl0@10050000), which S-Boot also leaves running, is
	 * stopped.
	 */
	.exynos_wdt = 0x10060000, .exynos_wdt_other = 0x10050000, .exynos_wdt_pmu_bit = 23,
	/*
	 * Seen on the phone (29 October 2026): S-Boot powers core 1 up and parks it before the hand-over (its log: "Powers up
	 * core(1)", CPU_SEQ_STATUS cpu0 and cpu1 on, cpu2-7 off). CPU_ON for it then answers 0 (not ALREADY_ON) and the core
	 * never comes; and AFFINITY_INFO answers 0 ("on") of cores 2-7, which are off. So core 1 is never the kernel's, and
	 * that answer is not taken as a no.
	 */
	.parked_cores = 0x2,
	.affinity_unreliable = 1,
	/*
	 * The Shannon CP's memory (Samsung's device tree, reserved-memory: modem_if 0xF0000000 + 0x9200000, cp_ram_logging
	 * 0xF9200000 + 0x2000000), whose window EL3 fixes at cold boot, and ABOX's (abox_rmem 0xFB200000 + 0x2800000, for
	 * calliope): taken from UEFI's allocator at the start and never in XNU's window (choose_window). The payload lands
	 * at 0xF0000000 (the UEFI image's boot manager finds loader.efi there), so it is moved out first, to 0xD0000000:
	 * between the panel's buffer (0xCC000000 + 0x1043000) and secure_camera (0xE1900000), where the device tree reserves
	 * nothing (docs: devices/starlte, docs/modem-plan.md). The CP is not running while the loader moves it: nothing
	 * starts the modem before XNU (S-Boot does not; Android's cbd would).
	 */
	.payload_home = 0xD0000000,
	.keep_out = { { 0xF0000000, 0x0B200000, "CP: modem_if, cp_ram_logging" }, { 0xFB200000, 0x02800000, "ABOX: abox_rmem" } },
};

/*
 * The Galaxy S9+ (SM-G965F, star2lte) and the Galaxy Note9 (SM-N960F, crownlte): the S9's SoC, board and memory map. Every
 * address above is the same in their device trees (Samsung's BSP, exynos9810-star2lte_eur_open_26.dts and
 * exynos9810-crownlte_eur_open_26.dts: the GIC, the timer, the PMU, both cluster watchdogs, reserved-memory's modem_if,
 * cp_ram_logging, abox_rmem, exynos_ss and log_pstore, secure_camera), and the panel is 1440 x 2960 on all three, its buffer
 * at 0xCC000000 (the S9+'s seen there by the WoA port for it, ntdevlabs/exynos9810-woa; the Note9's not seen by anyone).
 * Their RAM (6 GB; the Note9 6 or 8 GB) is UEFI's to report: Silicium's RamManagerLib reads its size from what S-Boot
 * left at NS_IRAM + 0x848, and the kernel's window is the largest free block, at most MAX_MEM_SIZE. So only the name
 * differs, and with it what the drivers that look at /arm-io's device_type do (the MAX77705's battery). The framebuffer
 * cannot tell them from the S9: the device's own image says which it is (loader_platform=, devices/<name>/mkimage.sh),
 * and without that they run as the S9. Written, not seen: nobody has run this on an S9+ or a Note9.
 */
static const struct platform platform_star2lte = {
	.name = "star2lte", .io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x10101000, .gic_cpu = 0x10102000, .uart = 0,
	.cores = 8, .core_mpidr = (const uint32_t[]){ 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 },
	.small_cores = 0x0f, .timer_hz = 26000000, .fb_base = 0xCC000000,
	.ram_log = 0xFED14000, .ram_log_size = 0x4000, .ram_dump = 0xFED10000, .ram_dump_size = 0x4000,
	.log_other_cpu = 1, .recovery_pmu = 0x14060000, .pmu_swreset = 0x400, .data_part = "USERDATA",
	.exynos_wdt = 0x10060000, .exynos_wdt_other = 0x10050000, .exynos_wdt_pmu_bit = 23,
	.parked_cores = 0x2, .affinity_unreliable = 1,
	.payload_home = 0xD0000000,
	/* SM-G965F live FDT plus the port's scanout allocation. Required before low
	 * RAM can be claimed; the former high-only XNU window never touched these. */
	.keep_out = {
		{ 0xF0000000, 0x0B200000, "CP: modem_if, cp_ram_logging" },
		{ 0xFB200000, 0x02800000, "ABOX: abox_rmem" },
		{ 0x80000000, 0x00001000, "sec_debug_magic" },
		{ 0xAF800000, 0x02000000, "RKP reserved" },
		{ 0xB8000000, 0x00200000, "TIMA reserved" },
		{ 0xBC800000, 0x03800000, "outside live FDT DRAM banks" },
		{ 0xC0000000, 0x00080000, "seclog_mem" },
		{ 0xCC000000, 0x01050000, "native boot framebuffer" },
		{ 0xD4000000, 0x03000000, "native scanout pool" },
		{ 0xE0000000, 0x01900000, "outside live FDT DRAM banks" },
		{ 0xE1900000, 0x01E00000, "secure_camera" },
		{ 0xFDA00000, 0x01C08000, "exynos snapshot and pstore" },
		{ 0xFF800000, 0x00800000, "fimc_is_lib" },
	},
	.sibling_of = &platform_starlte,
};

static const struct platform platform_crownlte = {
	.name = "crownlte", .io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x10101000, .gic_cpu = 0x10102000, .uart = 0,
	.cores = 8, .core_mpidr = (const uint32_t[]){ 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 },
	.small_cores = 0x0f, .timer_hz = 26000000, .fb_base = 0xCC000000,
	.ram_log = 0xFED14000, .ram_log_size = 0x4000, .ram_dump = 0xFED10000, .ram_dump_size = 0x4000,
	.log_other_cpu = 1, .recovery_pmu = 0x14060000, .pmu_swreset = 0x400, .data_part = "USERDATA",
	.exynos_wdt = 0x10060000, .exynos_wdt_other = 0x10050000, .exynos_wdt_pmu_bit = 23,
	.parked_cores = 0x2, .affinity_unreliable = 1,
	.payload_home = 0xD0000000,
	.keep_out = { { 0xF0000000, 0x0B200000, "CP: modem_if, cp_ram_logging" }, { 0xFB200000, 0x02800000, "ABOX: abox_rmem" } },
	.sibling_of = &platform_starlte,
};

static const struct platform platform_frankel = {
	/*
	 * Google Pixel 10 (frankel), Tensor G5 ("laguna", lga), bare metal. There is no UEFI for this phone: Google's boot
	 * loader (ABL) starts the SoC's boot shim (socs/lga/shim), which offers the few UEFI services this loader uses
	 * over the boot loader's device tree, and stays at EL2 to pass PSCI on to EL3. Its framebuffer has no fixed address
	 * either (the boot loader's own), so the phone is told apart by the root "compatible" of that device tree.
	 * Every value is from the mainline device tree (arch/arm64/boot/dts/google/lga.dtsi, Linux 7.4, GPL-2.0 OR
	 * BSD-3-Clause) and Google's kernel for the phone (private/devices/google/lga/dts/lga-base.dtsi, -rmem.dtsi);
	 * devices/frankel/README.md has each one's source.
	 */
	.name = "frankel", .compatible = "google,lga-frankel",
	/* the SoC's registers from the GIC (0x05880000) to the UFS host (0x3C400000) and the PWM (0x3BD90000) */
	.io_base = 0x04000000, .io_size = 0x3C000000,
	/* a GICv3 ("arm,gic-v3"): distributor 0x05880000, redistributors 0x05900000 + 2 MB, GICv4 frames of 256 KB */
	.gic_dist = 0x05880000, .gic_redist = 0x05900000, .gic_redist_size = 0x200000,
	/* the console UART (0x0DB62000) is a DesignWare 16550, not a PL011, and reaches the USB-C port only after `fastboot oem uart enable` */
	.uart = 0,
	/* PSCI by HVC: to the shim at EL2, which makes the SMC and starts a core at EL1 where the firmware would start it at EL2 */
	.psci_hvc = 1,
	/*
	 * Each core in an affinity field of its own (MPIDR 0x000-0x700, Aff1 = the core): 0-1 Cortex-A520, 2-6 Cortex-A725,
	 * 7 Cortex-X4. All ARMv9.2: any one of them may be the boot core.
	 */
	.cores = 8, .core_shift = 8,
	.small_cores = 0x03,
	/*
	 * Linux's ramoops (reserved-memory ramoops@95200000, 4 MB: 2 MB console, then 2 MB pmsg), its pmsg zone, which the
	 * boot loader leaves alone across a warm restart (the region is reserved "for access by the ABL following a
	 * crash-reset"): Android reads it back as /sys/fs/pstore/pmsg-ramoops-0.
	 */
	.ram_log = 0x95400000, .ram_log_size = 0x10000,
	/* CNTFRQ_EL0: set by the firmware (no clock-frequency in the timer's node) */
	.timer_hz = 0,
	/* the framebuffer is the boot loader's, wherever it is: not a way to tell this phone apart */
	.fb_base = 0,
};

/*
 * Not a phone: QEMU virt as close to the Pixel 10 as it goes (socs/lga/shim/qemu-test.sh): EL2 (virtualization=on) for
 * the boot shim, which starts this loader without UEFI and passes PSCI on; a GICv4 (gic-version=4: 256 KB redistributor
 * frames, 2 MB of them for 8 cores); eight cores, two of them small. QEMU numbers its cores in Aff0 whatever -smp says,
 * so the core numbering is QEMU's (core_shift 0); the Pixel's, Aff1, is redwood's. Told apart by the root "compatible"
 * of the test's device tree.
 */
static const struct platform platform_a50 = {
	/*
	 * Samsung Galaxy A50 (SM-A505FN, a50), Exynos 9610, bare metal: the device's own boot shim
	 * (devices/a50/soc/shim, the Tensor G5 shim's design) starts this loader, no UEFI. Every value is from
	 * the phone's live device tree, read in TWRP (devices/a50/README.md): the GIC-400 (distributor
	 * 0x12301000, CPU interface 0x12302000), the timer at 26 MHz, PSCI by SMC, the PMU at 0x11860000, both
	 * cluster watchdogs at the S9's addresses, the panel's buffer at 0xCA000000 (1080 x 2340,
	 * s3cfb.bootloaderfb= in the tree's bootargs). Told apart by the tree's "samsung,exynos9610".
	 */
	.name = "a50", .compatible = "samsung,exynos9610",
	.io_base = 0x10000000, .io_size = 0x10000000,
	.gic_dist = 0x12301000, .gic_cpu = 0x12302000, .uart = 0,
	/*
	 * Cores 0-3 are Cortex-A53s (MPIDR 0x000-0x003), 4-7 Cortex-A73s (0x100-0x103), all ARMv8.0: no PAN on
	 * any of them (the kernel's own check, xnu/patches/1509) and isa_floor=1 for user space (1510). The
	 * kernel runs on the A53s 2 and 3 (cpus=2 bootcpu=2), as on the S9; the A73s are untried.
	 */
	.cores = 8, .core_mpidr = (const uint32_t[]){ 0x000, 0x001, 0x002, 0x003, 0x100, 0x101, 0x102, 0x103 },
	.small_cores = 0x0f,
	/* CNTFRQ_EL0 may be 0 here as on the 9810; the tree's timer says 26 MHz. */
	.timer_hz = 26000000,
	.fb_base = 0xCA000000,
	/*
	 * Samsung's pstore (the live tree's debug-snapshot log_pstore, 0xF9D10000 + 2 MB): its first 256 KB for
	 * the start of the kernel's log, the second megabyte's start for the log kept across a restart.
	 */
	.ram_log = 0xF9E90000, .ram_log_size = 0x80000,
	.ram_dump = 0xF9D10000, .ram_dump_size = 0x80000,
	.log_other_cpu = 1,
	/*
	 * The PMU (system-controller@11860000): INFORM2/3 (+0x808/+0x80C) and SWRESET (+0x400), the same layout
	 * the S9's PMU has -- on this phone written, not yet seen to send a restart to recovery.
	 */
	.recovery_pmu = 0x11860000, .pmu_swreset = 0x400,
	.data_part = "USERDATA",
	/*
	 * The cluster watchdogs as on the S9 (watchdog_cl1@10060000 armed, watchdog_cl0@10050000 stopped); the
	 * PMU bit for the 9610 is the 9810's until checked (wdt=0 keeps everything off).
	 */
	.exynos_wdt = 0x10060000, .exynos_wdt_other = 0x10050000, .exynos_wdt_pmu_bit = 23,
	/*
	 * Not seen on this phone and guessed from the S9: S-Boot parks core 1 (parked_cores), and PSCI's
	 * AFFINITY_INFO is not believed (affinity_unreliable).
	 */
	.parked_cores = 0x2, .affinity_unreliable = 1,
	/*
	 * The payload lands at 0xF0000000, inside the modem's window (the CP is not running: S-Boot does not
	 * start it), so it is moved out first, to 0xD0000000: inside the 0xC0000000 bank, past the framebuffer,
	 * where the device tree reserves nothing.
	 */
	.payload_home = 0xD0000000,
	.keep_out = {
		{ 0x80000000, 0x00002000, "sec_debug_magic, kaslr" },
		{ 0x87000000, 0x00200000, "the EL2 hypervisor's (el2,uh)" },
		{ 0xB8000000, 0x00500000, "vipx_fw_code_rmem" },
		{ 0xC0000000, 0x00080000, "seclog_mem" },
		{ 0xCA000000, 0x01000000, "S-Boot framebuffer" },
		{ 0xE9400000, 0x02800000, "ABOX: abox_rmem" },
		{ 0xED000000, 0x08A00000, "CP: modem_if" },
		{ 0xF5A00000, 0x01E00000, "CP: cp_ram_logging" },
		{ 0xF8E00000, 0x00200000, "bcm_setup" },
		{ 0xF9000000, 0x02000000, "exynos snapshot and pstore" },
		{ 0xFB000000, 0x00800000, "gnss_if" },
		{ 0xFD000000, 0x00080000, "chub_rmem" },
		{ 0x8FE600000ULL, 0x01A00000, "the EL2 hypervisor's earlymem" },
	},
};

static const struct platform platform_qemu_lga = {
	.name = "qemu-lga", .compatible = "scion,qemu-lga",
	.io_base = 0x08000000, .io_size = 0x08000000,
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .gic_redist_size = 0x200000, .uart = 0x09000000, .psci_hvc = 1,
	.cores = 8, .core_shift = 0, .small_cores = 0x03,
	.timeout_scale = 20,
};

/*
 * The devices the loader knows, told apart by the framebuffer base their UEFI reports. A port to another
 * device adds its description here (devices/<name>/README.md says where each value comes from); a
 * display that matches none is QEMU's ramfb.
 */
static const struct platform *const platforms[] = {
	&platform_redwood,
	&platform_starlte,
	&platform_frankel,
	&platform_a50,
	&platform_qemu_lga,
};

/* ... and those the framebuffer does not tell apart from one of these, named on the payload's command line instead */
static const struct platform *const sibling_platforms[] = {
	&platform_star2lte,
	&platform_crownlte,
};

/* Core N's MPIDR (its affinity fields), and the number of the core with that MPIDR (-1: none of the platform's) */
static uint64_t
core_mpidr(const struct platform *plat, int core)
{
	return plat->core_mpidr != NULL ? plat->core_mpidr[core] : (uint64_t)core << plat->core_shift;
}

static int
core_number(const struct platform *plat, uint64_t mpidr)
{
	for (int core = 0; core < plat->cores; core++) {
		if (core_mpidr(plat, core) == (mpidr & 0xFFFFFF)) {
			return core;
		}
	}
	return -1;
}

static int starts_with(const char *s, const char *prefix);

/*
 * loader_platform=NAME on the payload's command line: the platform of that name in place of `plat`, if it is one of
 * plat's siblings (sibling_platforms: the same SoC, board and memory map, only the name differs), else `plat` as it was.
 * Without the argument nothing changes.
 */
static const struct platform *
platform_sibling(const struct platform *plat, const char *cmdline)
{
	for (const char *c = cmdline; c != NULL && *c; c++) {
		if ((c != cmdline && c[-1] != ' ') || !starts_with(c, "loader_platform=")) {
			continue;
		}
		const char *want = c + 16;
		for (unsigned i = 0; i < sizeof(sibling_platforms) / sizeof(sibling_platforms[0]); i++) {
			const char *n = sibling_platforms[i]->name;
			unsigned k = 0;

			while (n[k] != 0 && want[k] == n[k]) {
				k++;
			}
			if (n[k] == 0 && (want[k] == 0 || want[k] == ' ') && sibling_platforms[i]->sibling_of == plat) {
				return sibling_platforms[i];
			}
		}
		return plat;
	}
	return plat;
}

/* QEMU virt started with gic-version=2 (qemu/run.sh, QEMU_GIC=2): its GICv2's two frames */
static struct platform platform_qemu_virt_gicv2;

/* ------------------------------------------------------------------------ */
/* PCI host bridge -> device tree node /pci0 (XNU: bsd/dev/hakios.pci.c)    */
/*                                                                          */
/* From the flattened device tree when the firmware publishes one (UEFI     */
/* configuration table), else QEMU virt's layout. Either way the kernel      */
/* gets plain little-endian values with absolute addresses:                  */
/*   compatible, ecam = <base size>, bus-range = <first last>,              */
/*   pci-ranges = { u32 space, u32 prefetch, u64 pci, u64 cpu, u64 size }..., */
/*   intx-map = { u32 phys.hi, u32 pin, u32 GIC INTID, u32 edge }...,         */
/*   intx-mask = <phys.hi mask, pin mask>, probe-only, firmware-devfn.       */
/* firmware-devfn names the PCI function this loader was started from (its */
/* ESP under QEMU): UEFI keeps using it after the kernel returns, so the     */
/* kernel's drivers leave it alone.                                          */

#define EFI_DTB_TABLE_GUID      { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }
#define EFI_LOADED_IMAGE_GUID   { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define PCI_MAX_RANGES  4
#define PCI_MAX_INTX    64

struct pci_range_le {
	uint32_t space, prefetch;
	uint64_t pci, cpu, size;
};

struct pci_intx_le {
	uint32_t hi, pin, intid, edge;
};

static struct pci_host {
	int present;
	const char *compatible;
	uint64_t ecam[2];
	uint32_t bus_range[2];
	struct pci_range_le ranges[PCI_MAX_RANGES];
	uint32_t nranges;
	struct pci_intx_le intx[PCI_MAX_INTX];
	uint32_t nintx;
	uint32_t mask[2];
	uint32_t probe_only;
	uint32_t firmware_devfn[4];
	uint32_t nfirmware;
} PCI;

static int
guid_eq(const efi_guid *a, const efi_guid *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static uint32_t
fdt32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
fdt_cells(const uint8_t *p, uint32_t cells)
{
	uint64_t v = 0;
	for (uint32_t i = 0; i < cells; i++) {
		v = v << 32 | fdt32(p + 4 * i);
	}
	return v;
}

/* does a "compatible" list (len bytes of NUL-separated strings) contain want? */
static int
compat_has(const char *list, uint32_t len, const char *want)
{
	for (uint32_t at = 0; at < len;) {
		if (str_eq(list + at, want)) {
			return 1;
		}
		at += (uint32_t)str_len(list + at) + 1;
	}
	return 0;
}

#define FDT_MAX_CONTROLLERS 8

/*
 * The FDT, twice: first every interrupt controller (phandle -> #address-cells, #interrupt-cells),
 * then the host bridge and /chosen. Only what the bridge needs is understood.
 */
static int
pci_from_fdt(const uint8_t *fdt)
{
	struct { uint32_t phandle, addr_cells, intr_cells; } ctl[FDT_MAX_CONTROLLERS];
	uint32_t nctl = 0;

	if (fdt32(fdt) != 0xd00dfeed) {
		return 0;
	}
	const uint8_t *st = fdt + fdt32(fdt + 8);
	const char *strs = (const char *)fdt + fdt32(fdt + 12);
	uint32_t st_size = fdt32(fdt + 36);

	for (int pass = 0; pass < 2; pass++) {
		/* the node being read, depth by depth: only the innermost matters */
		uint32_t depth = 0, phandle = 0, addr_cells = 2, intr_cells = 0, is_ctl = 0, is_pci = 0, is_chosen = 0;
		const uint8_t *reg = 0, *ranges = 0, *imap = 0, *imask = 0, *busr = 0;
		uint32_t reg_len = 0, ranges_len = 0, imap_len = 0, imask_len = 0;
		const char *compat = 0;

		for (uint32_t at = 0; at + 4 <= st_size;) {
			uint32_t tok = fdt32(st + at);
			at += 4;
			if (tok == 1) {                 /* BEGIN_NODE name */
				const char *name = (const char *)st + at;
				at += ((uint32_t)str_len(name) + 1 + 3) & ~3u;
				depth++;
				phandle = 0, addr_cells = 2, intr_cells = 0, is_ctl = 0, is_pci = 0;
				is_chosen = depth == 2 && (str_eq(name, "chosen"));
				reg = ranges = imap = imask = busr = 0;
				compat = 0;
			} else if (tok == 3) {          /* PROP len nameoff value */
				uint32_t len = fdt32(st + at), nameoff = fdt32(st + at + 4);
				const uint8_t *v = st + at + 8;
				const char *pn = strs + nameoff;
				at += 8 + ((len + 3) & ~3u);
				if (str_eq(pn, "phandle") && len == 4) {
					phandle = fdt32(v);
				} else if (str_eq(pn, "#address-cells") && len == 4) {
					addr_cells = fdt32(v);
				} else if (str_eq(pn, "#interrupt-cells") && len == 4) {
					intr_cells = fdt32(v);
				} else if (str_eq(pn, "interrupt-controller")) {
					is_ctl = 1;
				} else if (str_eq(pn, "compatible")) {
					if (compat_has((const char *)v, len, "pci-host-ecam-generic")) {
						is_pci = 1, compat = "pci-host-ecam-generic";
					} else if (compat_has((const char *)v, len, "pci-host-cam-generic")) {
						is_pci = 1, compat = "pci-host-cam-generic";
					}
				} else if (str_eq(pn, "reg")) {
					reg = v, reg_len = len;
				} else if (str_eq(pn, "ranges")) {
					ranges = v, ranges_len = len;
				} else if (str_eq(pn, "interrupt-map")) {
					imap = v, imap_len = len;
				} else if (str_eq(pn, "interrupt-map-mask")) {
					imask = v, imask_len = len;
				} else if (str_eq(pn, "bus-range") && len == 8) {
					busr = v;
				} else if (pass == 1 && is_chosen && str_eq(pn, "linux,pci-probe-only") && len == 4) {
					PCI.probe_only = fdt32(v);
				}
			} else if (tok == 2) {          /* END_NODE: the node is complete */
				if (pass == 0 && is_ctl && phandle && nctl < FDT_MAX_CONTROLLERS) {
					ctl[nctl].phandle = phandle;
					ctl[nctl].addr_cells = addr_cells;
					ctl[nctl].intr_cells = intr_cells ? intr_cells : 3;
					nctl++;
				}
				if (pass == 1 && is_pci && !PCI.present && reg && reg_len >= 16) {
					/* the bridge sits under the root: 2 address and 2 size cells */
					PCI.present = 1;
					PCI.compatible = compat;
					PCI.ecam[0] = fdt_cells(reg, 2);
					PCI.ecam[1] = fdt_cells(reg + 8, 2);
					PCI.bus_range[0] = busr ? fdt32(busr) : 0;
					PCI.bus_range[1] = busr ? fdt32(busr + 4) : 0;
					/* ranges: child phys.hi phys.mid phys.lo, parent addr (2), size (2) */
					for (uint32_t o = 0; ranges && o + 28 <= ranges_len && PCI.nranges < PCI_MAX_RANGES; o += 28) {
						uint32_t hi = fdt32(ranges + o);
						struct pci_range_le *r = &PCI.ranges[PCI.nranges++];
						r->space = (hi >> 24) & 3;
						r->prefetch = (hi >> 30) & 1;
						r->pci = fdt_cells(ranges + o + 4, 2);
						r->cpu = fdt_cells(ranges + o + 12, 2);
						r->size = fdt_cells(ranges + o + 20, 2);
					}
					/* interrupt-map: phys.hi mid lo, pin, phandle, parent unit, parent specifier */
					for (uint32_t o = 0; imap && o + 20 <= imap_len && PCI.nintx < PCI_MAX_INTX;) {
						uint32_t ph = fdt32(imap + o + 16), pa = 2, pi = 3;
						for (uint32_t c = 0; c < nctl; c++) {
							if (ctl[c].phandle == ph) {
								pa = ctl[c].addr_cells, pi = ctl[c].intr_cells;
							}
						}
						uint32_t n = 5 + pa + pi;
						if (o + 4 * n > imap_len) {
							break;
						}
						const uint8_t *spec = imap + o + 4 * (5 + pa);
						if (pi >= 2) {
							struct pci_intx_le *e = &PCI.intx[PCI.nintx++];
							uint32_t type = fdt32(spec), num = fdt32(spec + 4);
							e->hi = fdt32(imap + o);
							e->pin = fdt32(imap + o + 12);
							e->intid = type == 1 ? 16 + num : 32 + num;
							e->edge = pi >= 3 ? (fdt32(spec + 8) & 3) != 0 : 0;
						}
						o += 4 * n;
					}
					PCI.mask[0] = imask && imask_len >= 16 ? fdt32(imask) : 0xffff00;
					PCI.mask[1] = imask && imask_len >= 16 ? fdt32(imask + 12) : 7;
				}
				depth--;
				is_chosen = 0;
			} else if (tok == 9) {          /* END */
				break;
			} else if (tok != 4) {          /* anything but NOP: give up */
				break;
			}
		}
	}
	return PCI.present;
}

/*
 * A GICv2 under the root of the FDT (QEMU virt with gic-version=2): its distributor and CPU interface, by the
 * root's #address-cells and #size-cells. 0: none, or a GICv3.
 */
static int
gic_v2_from_fdt(const uint8_t *fdt, uint64_t *dist, uint64_t *cpu)
{
	if (fdt32(fdt) != 0xd00dfeed) {
		return 0;
	}
	const uint8_t *st = fdt + fdt32(fdt + 8);
	const char *strs = (const char *)fdt + fdt32(fdt + 12);
	uint32_t st_size = fdt32(fdt + 36);
	uint32_t depth = 0, root_addr = 2, root_size = 1, is_v2 = 0, reg_len = 0;
	const uint8_t *reg = 0;

	for (uint32_t at = 0; at + 4 <= st_size;) {
		uint32_t tok = fdt32(st + at);
		at += 4;
		if (tok == 1) {                 /* BEGIN_NODE name */
			at += ((uint32_t)str_len((const char *)st + at) + 1 + 3) & ~3u;
			depth++;
			if (depth == 2) {       /* (a child of the GIC's node, QEMU's v2m@, must not forget the GIC's) */
				is_v2 = 0, reg = 0, reg_len = 0;
			}
		} else if (tok == 3) {          /* PROP len nameoff value */
			uint32_t len = fdt32(st + at), nameoff = fdt32(st + at + 4);
			const uint8_t *v = st + at + 8;
			const char *pn = strs + nameoff;
			at += 8 + ((len + 3) & ~3u);
			if (depth == 1 && str_eq(pn, "#address-cells") && len == 4) {
				root_addr = fdt32(v);
			} else if (depth == 1 && str_eq(pn, "#size-cells") && len == 4) {
				root_size = fdt32(v);
			} else if (depth == 2 && str_eq(pn, "compatible")) {
				is_v2 = compat_has((const char *)v, len, "arm,cortex-a15-gic") ||
				    compat_has((const char *)v, len, "arm,gic-400");
			} else if (depth == 2 && str_eq(pn, "reg")) {
				reg = v, reg_len = len;
			}
		} else if (tok == 2) {          /* END_NODE */
			uint32_t one = 4 * (root_addr + root_size);
			if (depth == 2 && is_v2 && reg != 0 && root_addr <= 2 && root_size <= 2 && reg_len >= 2 * one) {
				*dist = fdt_cells(reg, root_addr);
				*cpu = fdt_cells(reg + one, root_addr);
				return 1;
			}
			depth--;
		} else if (tok == 9) {          /* END */
			break;
		}
	}
	return 0;
}

/*
 * A platform with a .compatible (one whose framebuffer does not tell it apart: the Pixel 10's is wherever its boot loader
 * put it) found by the root node's "compatible" in the device tree the firmware publishes (UEFI configuration table;
 * socs/lga/shim publishes the boot loader's). `plat` if none matches.
 */
static const struct platform *
platform_from_fdt(const struct platform *plat)
{
	static const efi_guid dtb_guid = EFI_DTB_TABLE_GUID;

	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables; i++) {
		const uint8_t *fdt = ST->tables[i].table;

		if (!guid_eq(&ST->tables[i].guid, &dtb_guid) || fdt == NULL || fdt32(fdt) != 0xd00dfeed) {
			continue;
		}
		const uint8_t *st = fdt + fdt32(fdt + 8);
		const char *strs = (const char *)fdt + fdt32(fdt + 12);
		uint32_t st_size = fdt32(fdt + 36), at = 0;

		/* the root node: BEGIN_NODE "", then its properties, up to its first child */
		while (at + 4 <= st_size && fdt32(st + at) == 4) {      /* NOP */
			at += 4;
		}
		if (at + 8 > st_size || fdt32(st + at) != 1) {
			continue;
		}
		at += 4;
		at += ((uint32_t)str_len((const char *)st + at) + 1 + 3) & ~3u;
		while (at + 12 <= st_size && (fdt32(st + at) == 3 || fdt32(st + at) == 4)) {
			if (fdt32(st + at) == 4) {
				at += 4;
				continue;
			}
			uint32_t len = fdt32(st + at + 4), nameoff = fdt32(st + at + 8);
			const char *v = (const char *)st + at + 12;

			if (str_eq(strs + nameoff, "compatible")) {
				for (unsigned p = 0; p < sizeof(platforms) / sizeof(platforms[0]); p++) {
					if (platforms[p]->compatible != NULL && compat_has(v, len, platforms[p]->compatible)) {
						return platforms[p];
					}
				}
				return plat;
			}
			at += 12 + ((len + 3) & ~3u);
		}
	}
	return plat;
}

/* QEMU virt (gic-version=3, highmem): what its FDT says, for firmware that publishes ACPI instead */
static void
pci_qemu_virt(void)
{
	PCI.present = 1;
	PCI.compatible = "pci-host-ecam-generic";
	PCI.ecam[0] = 0x4010000000ULL;
	PCI.ecam[1] = 0x10000000;
	PCI.bus_range[0] = 0;
	PCI.bus_range[1] = 0xff;
	PCI.ranges[0] = (struct pci_range_le){ 2, 0, 0x10000000, 0x10000000, 0x2eff0000 };
	PCI.ranges[1] = (struct pci_range_le){ 3, 1, 0x8000000000ULL, 0x8000000000ULL, 0x8000000000ULL };
	PCI.nranges = 2;
	/* INTA..INTD of slot s: SPI 3 + (s + pin - 1) % 4, level */
	for (uint32_t slot = 0; slot < 4; slot++) {
		for (uint32_t pin = 1; pin <= 4; pin++) {
			PCI.intx[PCI.nintx++] = (struct pci_intx_le){ slot << 11, pin, 32 + 3 + (slot + pin - 1) % 4, 0 };
		}
	}
	PCI.mask[0] = 0x1800;
	PCI.mask[1] = 7;
}

static uint64_t device_path_bytes(const efi_device_path *dp);

/* The PCI function the loader came from (its device path's PCI node on the root bus), if any. */
static void
pci_firmware_devices(void)
{
	static const efi_guid li_guid = EFI_LOADED_IMAGE_GUID, dp_guid = EFI_DEVICE_PATH_GUID;
	struct { uint32_t revision; efi_handle parent; void *st; efi_handle device; } *li;
	const uint8_t *p;

	if (EFI_ERROR(BS->handle_protocol(IMAGE, &li_guid, (void **)&li)) || li->device == NULL ||
	    EFI_ERROR(BS->handle_protocol(li->device, &dp_guid, (void **)&p))) {
		return;
	}
	for (uint64_t n = 0, total = device_path_bytes((const efi_device_path *)p), pcis = 0; n < total;) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;
		if (p[n] == 1 && p[n + 1] == 1 && len >= 6) {   /* hardware, PCI: function, device */
			if (pcis++ == 0 && PCI.nfirmware < 4) {
				PCI.firmware_devfn[PCI.nfirmware++] = (uint32_t)p[n + 5] << 3 | p[n + 4];
			} else {
				PCI.nfirmware = 0;      /* behind a bridge: bus unknown, say nothing */
			}
		}
		n += len;
	}
}

static void
pci_probe(const struct platform *plat)
{
	static const efi_guid dtb_guid = EFI_DTB_TABLE_GUID;
	const char *from = "none";

	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables; i++) {
		if (guid_eq(&ST->tables[i].guid, &dtb_guid) && pci_from_fdt(ST->tables[i].table)) {
			from = "the firmware's device tree";
			break;
		}
	}
	if (!PCI.present && (plat == &platform_qemu_virt || plat == &platform_qemu_virt_gicv2)) {
		pci_qemu_virt();
		from = "QEMU virt's layout";
	}
	if (!PCI.present) {
		return;
	}
	pci_firmware_devices();
	print("pci: %s at %x size %x from %s, %d windows, %d INTx entries%s", PCI.compatible, PCI.ecam[0], PCI.ecam[1],
	    from, (uint64_t)PCI.nranges, (uint64_t)PCI.nintx, PCI.probe_only ? ", probe only" : "");
	for (uint32_t i = 0; i < PCI.nfirmware; i++) {
		print(", firmware's %x.%x", (uint64_t)(PCI.firmware_devfn[i] >> 3), (uint64_t)(PCI.firmware_devfn[i] & 7));
	}
	print("\n");
}

static void
dt_pci_node(struct dt_writer *w)
{
	if (!PCI.present) {
		return;
	}
	dt_begin(w, "pci0");
	dt_prop_str(w, "compatible", PCI.compatible);
	dt_prop(w, "ecam", PCI.ecam, sizeof(PCI.ecam));
	dt_prop(w, "bus-range", PCI.bus_range, sizeof(PCI.bus_range));
	dt_prop(w, "pci-ranges", PCI.ranges, PCI.nranges * sizeof(PCI.ranges[0]));
	dt_prop(w, "intx-map", PCI.intx, PCI.nintx * sizeof(PCI.intx[0]));
	dt_prop(w, "intx-mask", PCI.mask, sizeof(PCI.mask));
	dt_prop_u32(w, "probe-only", PCI.probe_only);
	if (PCI.nfirmware) {
		dt_prop(w, "firmware-devfn", PCI.firmware_devfn, PCI.nfirmware * sizeof(PCI.firmware_devfn[0]));
	}
	dt_end(w);
}

/* ------------------------------------------------------------------------ */

/*
 * A Samsung phone restarted from here goes to its recovery (TWRP), where the run's log is read, and not back into BOOT:
 * that is this image again, and its next run would write over the log in RAM. S-Boot reads why it was restarted from
 * two registers of the PMU, which a reset leaves alone, and the phone's own kernel sets them for "reboot recovery" so
 * (Samsung's sec_reboot.c, sec_reboot(); the offsets in include/linux/sec_ext.h): INFORM2 (+0x808) 0x12345678, a
 * restart and not a power-off; INFORM3 (+0x80C) 0x12345674, recovery. recovery=0 on the command line: back into BOOT.
 */
static const struct platform *PLAT;      /* (the device found, below) */
static int reset_to_recovery = 1;

static void
recovery_after_reset(void)
{
	if (PLAT == NULL || PLAT->recovery_pmu == 0 || !reset_to_recovery) {
		return;
	}
	*(volatile uint32_t *)(uintptr_t)(PLAT->recovery_pmu + 0x808) = 0x12345678;
	*(volatile uint32_t *)(uintptr_t)(PLAT->recovery_pmu + 0x80C) = 0x12345674;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * The restart, from anywhere in the loader (the end of a run, a hung kernel, an exception in the loader itself).
 *
 * On the Galaxy S9 the INFORM registers are written again right before it, and the reset is the PMU's software reset,
 * as Samsung's kernel does it (sec_reboot.c sets INFORM2/3, then exynos-reboot.c writes SWRESET): S-Boot's log after
 * such a reset reads "SW_RST (0x20000000)", "INF2: 12345678 / INF3: 1234567x" and goes where INFORM3 says. Not
 * UEFI's ResetSystem (PSCI SYSTEM_RESET, Silicium's ArmPsciResetSystemLib): no run's restart was ever seen to come
 * from it. And not the watchdog: every one of the S9's logs after its reset (WDT_RST2, RST_STAT 0x800000) has
 * "INF2: 0 / INF3: 0", the values written at the start gone, and S-Boot starts BOOT, the run again. Only the keys
 * held (S-Boot's "s5p_check_keypad: recovery mode") sent those to TWRP. Should SWRESET not take, the firmware's
 * ResetSystem is tried after a second, and after that the watchdog is what is left.
 *
 * recovery=0 on the command line: INFORM2/3 not written, a restart comes back into BOOT, as before.
 * SYSIP_DAT0 (+0x810), which exynos_reboot() also sets for "reboot recovery" (0xFF), is not written: whether S-Boot
 * ever clears it is not known, and a value left there could send every later start to recovery.
 */
static void
platform_restart(void)
{
	static int restarting;          /* an exception in the log's write below comes back here: then no more writing */
	int again = restarting++;

	recovery_after_reset();
	if (PLAT != NULL && PLAT->recovery_pmu != 0 && PLAT->pmu_swreset != 0) {
		volatile uint32_t *pmu = (volatile uint32_t *)(uintptr_t)PLAT->recovery_pmu;
		uint64_t t0, hz = timer_hz();

		wdt_pet();
		if (!again) {
			print("restart: INFORM2 %x, INFORM3 %x (%s); the PMU's software reset (SWRESET +%x)\n",
			    (uint64_t)pmu[0x808 / 4], (uint64_t)pmu[0x80C / 4],
			    reset_to_recovery ? "S-Boot starts recovery" : "recovery=0: BOOT again", (uint64_t)PLAT->pmu_swreset);
			flush_log();
		}
		recovery_after_reset();         /* (again: nothing may come between these and the reset) */
		pmu[PLAT->pmu_swreset / 4] = 1;
		__asm__ volatile ("dsb sy\nisb" ::: "memory");
		for (t0 = READ_SYSREG(CNTVCT_EL0); READ_SYSREG(CNTVCT_EL0) - t0 < hz;) {
		}
		if (!again) {
			print("restart: still running a second after SWRESET: the firmware's ResetSystem instead\n");
			flush_log();
		}
		recovery_after_reset();
	}
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

static void
reboot(void)
{
	hc_take_back("before the reset");
	flush_log();
	platform_restart();
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];
/* 0: the GIC is a GICv2, whose CPU interface is memory, not ICC_* registers (those are not there to read) */
uint64_t loader_gic_sysregs = 1;
int loader_save_ctx(void);
void loader_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mov x9, #0\n"
        "  mrs x11, id_aa64mmfr1_el1\n  ubfx x11, x11, #20, #4\n"
        "  cbz x11, 8f\n  mrs x9, s3_3_c4_c4_0\n8:\n"
        "  str x9, [x10, #328]\n"
        "  mov x9, #0\n"
        "  mrs x11, id_aa64mmfr2_el1\n  ubfx x11, x11, #4, #4\n"
        "  cbz x11, 8f\n  mrs x9, s3_3_c4_c4_1\n8:\n"
        "  str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts */
        "  adrp x11, loader_gic_sysregs\n"
        "  ldr  x11, [x11, :lo12:loader_gic_sysregs]\n"
        "  cbz  x11, 3f\n"
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
        "3:\n"
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults. */
        "  mrs x9, id_aa64mmfr1_el1\n  ubfx x9, x9, #20, #4\n"
        "  cbz x9, 8f\n  .inst 0xd500409f\n8:\n"
        "  mrs x9, id_aa64mmfr2_el1\n  ubfx x9, x9, #4, #4\n"
        "  cbz x9, 8f\n  .inst 0xd500407f\n8:\n"
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
        "  adrp x11, loader_gic_sysregs\n"   /* (x0-x7 are what the kernel returned: x11 is free) */
        "  ldr  x11, [x11, :lo12:loader_gic_sysregs]\n"
        "  cbz  x11, 3f\n"
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
        "3:\n"
        "  mrs x11, id_aa64mmfr1_el1\n  ubfx x11, x11, #20, #4\n"
        "  cbz x11, 8f\n  ldr x9, [x10, #328]\n"
        "  msr s3_3_c4_c4_0, x9\n8:\n"
        "  mrs x11, id_aa64mmfr2_el1\n  ubfx x11, x11, #4, #4\n"
        "  cbz x11, 8f\n  ldr x9, [x10, #336]\n"
        "  msr s3_3_c4_c4_1, x9\n8:\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and switches its own core off.                                         */

#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_NOT_SUPPORTED (-1)

/* each alone in a cache line: one core reads them with its caches off */
static struct {
	uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
uint64_t psci_by_hvc __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* x0 = &other_cpu_start; MMU and caches are off */
        /* Publish arrival before accessing any EL1 system register. */
        "  adrp x9, other_cpu_stage\n"
        "  add x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  dsb sy\n"

        "  msr daifset, #0xf\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        "  mrs x9, id_aa64mmfr1_el1\n  ubfx x9, x9, #20, #4\n"
        "  cbz x9, 8f\n  .inst 0xd500409f\n8:\n"
        "  mrs x9, id_aa64mmfr2_el1\n  ubfx x9, x9, #4, #4\n"
        "  cbz x9, 8f\n  .inst 0xd500407f\n8:\n"
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"                   /* PSCI CPU_OFF: does not return */
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;

/*
 * The architected timer's frequency: CNTFRQ_EL0, or where the firmware left that at 0 (the Galaxy S9), the platform's
 * own, else 26 MHz (before a platform is chosen). Everything here that turns ticks into time, and the device tree the
 * kernel takes its timebase from, goes through this: a 0 there divided every delay and timeout of the kernel by zero.
 */
static uint64_t
timer_hz(void)
{
	uint64_t hz = READ_SYSREG(CNTFRQ_EL0);

	if (hz != 0) {
		return hz;
	}
	return PLAT != NULL && PLAT->timer_hz != 0 ? PLAT->timer_hz : 26000000;
}
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
	uint64_t magic2;                /* "XNULIVE2": the kernel is new enough to have what follows */
	uint64_t sound;                 /* (a virtual address too) the same for a recording of the sound */
};

/*
 * Battery state shared with XNU.  Qualcomm's charger firmware remains a UEFI
 * boot-service client on core 0 while XNU runs on another core.  The loader is
 * therefore the one safe place to call it; XNU only reads this cache.
 */
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent;
	int32_t voltage_mv;
	int32_t current_ma;
	int32_t temperature_c;
	uint32_t charger_action;
	uint32_t polls;
	uint32_t failures;
	uint32_t reserved;
};

/*
 * Layout returned by QcomChargerDxeLA's GetChargingAction, word offsets confirmed by dumping the
 * raw buffer on this phone: [2] state of charge, [3] charge current (signed mA), [4] battery
 * voltage (mV), [5] temperature (C). `rest` is oversized on purpose so a firmware that writes
 * more than the fields we name cannot smash the stack.
 */
struct qcom_battery_status {
	uint32_t status0, status1;
	int32_t state_of_charge;
	int32_t charge_current_ma;
	int32_t battery_voltage_mv;
	int32_t battery_temperature_c;
	uint32_t rest[26];
};

static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
	.magic = LOADER_BATTERY_MAGIC,
	.version = LOADER_BATTERY_VERSION,
	.size = sizeof(struct loader_battery),
	.soc_percent = -1,
	.voltage_mv = -1,
	.current_ma = 0,
	.temperature_c = -273,
	.charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
/* ... and the recording of the sound ("oem sound"). */
static const char *sound_data;
static uint64_t sound_len;
#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define NOTE_SECONDS            24      /* how long the kernel's progress is written down as it happens */

static void wdt_pet(void);
static int wdt_armed;
static int exynos_wdt_owns(void);       /* (the Galaxy S9's watchdog, below) */

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static const struct kernel_live *running_live;
/*
 * Set for the whole of run_on_other_cpu(), whether the kernel has a live block or not.
 * running_live is NULL for a kernel without one, so it cannot say that a kernel is running.
 */
static int run_active;
static uint64_t running_slide;
static void usb_poll_command(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / timer_hz();
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
	uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
	uint64_t n, room = sizeof(out_buf) - 8192;

	compose_logs(NULL, 0);
	n = staged_len;
#define PUT(from, size) do { \
	uint64_t take = (size); \
	if (n + take > room) { \
	        take = room - n; \
	} \
	memcpy(out_buf + n, (from), take); \
	n += take; \
} while (0)
	PUT(sep, sizeof(sep) - 1);
	if (!wrapped || ring_at >= live->log_ring) {
		PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
	} else {
		PUT(log, live->log_head);
		PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
		PUT(log + live->log_head, ring_at);
	}
#undef PUT
	staged_len = n;
}

static void print(const char *fmt, ...);
static void flush_log(void);
static void put_char(char c);

/* A voltage-only fallback is deliberately conservative; the real QG SOC wins whenever available. */
static int
soc_from_voltage(uint32_t mv)
{
	if (mv <= 3300) return 0;
	if (mv < 3700) return (int)((mv - 3300) * 10 / 400);
	if (mv < 3900) return 10 + (int)((mv - 3700) * 40 / 200);
	if (mv < 4200) return 50 + (int)((mv - 3900) * 40 / 300);
	if (mv < 4400) return 90 + (int)((mv - 4200) * 10 / 200);
	return 100;
}

/* Read-only status query.  No TakeAction call and no charger-limit writes are made here. */
static void
charger_poll(void)
{
	struct qcom_battery_status status;
	uint32_t action = 0xffffffffu, flags = 0, voltage = 0;
	uint8_t present = 0, plugged = 0;
	efi_status s;
	int have_soc = 0, have_voltage = 0;

	if (!charger_enabled) {
		return;
	}
	memset(&status, 0, sizeof(status));

	charger_shared.sequence++;
	__asm__ volatile ("dmb ish" ::: "memory");
	if (CHARGER_EX != NULL) {
		s = CHARGER_EX->get_battery_presence(&present);
		if (!EFI_ERROR(s) && present) flags |= LOADER_BATTERY_PRESENT;
		s = CHARGER_EX->get_charger_presence(&plugged);
		if (!EFI_ERROR(s) && plugged) flags |= LOADER_BATTERY_CHARGER_PRESENT;
		s = CHARGER_EX->get_battery_voltage(&voltage);
		if (!EFI_ERROR(s) && voltage >= 2500 && voltage <= 5000) {
			have_voltage = 1;
			flags |= LOADER_BATTERY_VOLTAGE_VALID;
			charger_shared.voltage_mv = (int32_t)voltage;
		}
	}
	if (QCOM_CHARGER != NULL) {
		s = QCOM_CHARGER->get_charging_action(&action, &status, 0);
		if (!EFI_ERROR(s)) {
			charger_shared.charger_action = action;
			if (status.battery_voltage_mv >= 2500 && status.battery_voltage_mv <= 5000) {
				charger_shared.voltage_mv = status.battery_voltage_mv;
				have_voltage = 1;
				flags |= LOADER_BATTERY_VOLTAGE_VALID;
			}
			/*
			 * GetChargingAction returns the firmware's state of charge in word [2] of its
			 * output (confirmed by dumping the raw buffer on this device). We use it as-is
			 * EXCEPT when the fuel gauge is clearly not ready yet: right after initialize()
			 * it can report a near-empty SOC while the cell voltage says otherwise, which is
			 * where the old "shows 1% on a half-full battery" came from. A Li-ion cell above
			 * ~3.6 V is never under 5%, so in that case we distrust the gauge and fall back to
			 * the voltage estimate below until it settles.
			 */
			if (status.state_of_charge >= 0 && status.state_of_charge <= 100) {
				int vmv = have_voltage ? charger_shared.voltage_mv : 0;
				if (status.state_of_charge < 5 && vmv > 3600) {
					/* fuel gauge not settled; leave it to the voltage fallback */
				} else {
					charger_shared.soc_percent = status.state_of_charge;
					have_soc = 1;
					flags |= LOADER_BATTERY_VALID;
				}
			}
			charger_shared.current_ma = status.charge_current_ma;
			charger_shared.temperature_c = status.battery_temperature_c;
		}
	}
	if (!have_soc && have_voltage) {
		charger_shared.soc_percent = soc_from_voltage((uint32_t)charger_shared.voltage_mv);
		flags |= LOADER_BATTERY_VALID | LOADER_BATTERY_SOC_ESTIMATED;
	}
	if (plugged && charger_shared.soc_percent < 100) {
		flags |= LOADER_BATTERY_CHARGING;
	}
	charger_shared.flags = flags;
	charger_shared.polls++;
	if (!(flags & LOADER_BATTERY_VALID)) charger_shared.failures++;
	__asm__ volatile ("dmb ish" ::: "memory");
	charger_shared.sequence++;
	clean_dcache((uint64_t)(uintptr_t)&charger_shared,
	    (uint64_t)(uintptr_t)&charger_shared + sizeof(charger_shared));
}

/* ------------------------------------------------------------------------ */
/* UFS survey (ufs=1): what the phone's storage holds, read only            */

/*
 * The phone's storage as Qualcomm's own UFS driver in this UEFI sees it (UFSDxe): every raw
 * logical unit, every GPT partition with its name, place and size, and for the partitions there
 * are reference images of -- firmware-update/ in the ROM's fw.zip -- an FNV-1a hash of exactly
 * as many bytes as the image has, which tools/ufs-survey.py computes from the images on the host.
 * Where the two agree, reading through this driver gives the bytes that are on the flash, and that
 * is the ground a block device for the kernel is to stand on.
 *
 * Only read_blocks is ever called here: nothing is written, flushed or reset. It runs before the
 * kernel is started, with UEFI's boot services whole and interrupts on.
 */
static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static uint8_t ufs_buffer[1024 * 1024] __attribute__((aligned(4096)));

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/* the partition's name in ASCII (GPT names are UTF-16; these are all plain letters) */
static void
ufs_name(const efi_partition_info *info, char *out, unsigned size)
{
	unsigned i = 0;

	for (; i + 1 < size && i < 36 && info->name[i] != 0; i++) {
		out[i] = info->name[i] < 0x80 ? (char)info->name[i] : '?';
	}
	out[i] = 0;
}

/* "bluetooth_a" and "bluetooth_b" are both "bluetooth": the image is the same for either slot */
static int
ufs_reference_of(const char *name)
{
	for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
		const char *a = ufs_references[r].name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && (*b == 0 || ((b[0] == '_') && (b[1] == 'a' || b[1] == 'b') && b[2] == 0))) {
			return (int)r;
		}
	}
	return -1;
}

static void
ufs_hash_partition(efi_block_io *bio, const char *name, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, h = 0xcbf29ce484222325ULL, done = 0, started = READ_SYSREG(CNTVCT_EL0);
	uint64_t chunk = sizeof(ufs_buffer) / bs * bs, lba = 0;

	if (bs == 0 || bs > sizeof(ufs_buffer) || (bio->media->last_block + 1) * bs < bytes) {
		print("ufs: %s: %d bytes asked for, the partition is smaller\n", name, bytes);
		return;
	}
	while (done < bytes) {
		uint64_t want = bytes - done < chunk ? (bytes - done + bs - 1) / bs * bs : chunk;
		efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, want, ufs_buffer);

		if (EFI_ERROR(s)) {
			print("ufs: %s: reading at block %d: error %x\n", name, lba, (uint64_t)s);
			return;
		}
		if (done == 0) {
			print("ufs: %s: begins", name);
			for (int k = 0; k < 16; k++) {
				print(" %x", (uint64_t)ufs_buffer[k]);
			}
			print("\n");
		}
		h = ufs_fnv(h, ufs_buffer, bytes - done < want ? bytes - done : want);
		done += bytes - done < want ? bytes - done : want;
		lba += want / bs;
		if (wdt_armed) {
			wdt_pet();
		}
	}
	print("ufs: %s: fnv %x over %d bytes, %d ms\n", name, h, bytes,
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / timer_hz());
}

/* CRC-32 as GPT uses it (IEEE 802.3, reflected, 0xedb88320) */
static uint32_t
ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

/*
 * A unit's GPT checks itself: its header carries a CRC-32 of the header and one of the table of
 * partitions. Whatever firmware is on the phone, the bytes read are the bytes on the flash only if
 * both come out right -- the one check here that does not depend on having the same images.
 */
static void
ufs_check_gpt(efi_block_io *bio, uint64_t unit)
{
	uint64_t bs = bio->media->block_size, entries_lba, count, size, bytes, read;
	uint32_t header_size, header_crc, entries_crc, got_header, got_entries;
	uint8_t header[512];

	if (bs < 512 || bs > sizeof(ufs_buffer) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, ufs_buffer))) {
		print("ufs: unit %d: the GPT header could not be read\n", unit);
		return;
	}
	if (ufs_buffer[0] != 'E' || ufs_buffer[1] != 'F' || ufs_buffer[2] != 'I' || ufs_buffer[3] != ' ' ||
	    ufs_buffer[4] != 'P' || ufs_buffer[5] != 'A' || ufs_buffer[6] != 'R' || ufs_buffer[7] != 'T') {
		print("ufs: unit %d: no GPT header in block 1\n", unit);
		return;
	}
	memcpy(header, ufs_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	header_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header)) {
		print("ufs: unit %d: GPT header size %d\n", unit, (uint64_t)header_size);
		return;
	}
	*(uint32_t *)(header + 16) = 0;
	got_header = ufs_crc32(header, header_size);
	bytes = count * size;
	if (bytes == 0 || bytes > sizeof(ufs_buffer)) {
		print("ufs: unit %d: GPT header crc %s, a table of %d bytes not checked\n", unit,
		    got_header == header_crc ? "right" : "WRONG", bytes);
		return;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, ufs_buffer))) {
		print("ufs: unit %d: the GPT table could not be read\n", unit);
		return;
	}
	got_entries = ufs_crc32(ufs_buffer, bytes);
	print("ufs: unit %d: GPT header crc %s (%x), table of %d entries crc %s (%x)\n", unit,
	    got_header == header_crc ? "right" : "WRONG", (uint64_t)got_header, count,
	    got_entries == entries_crc ? "right" : "WRONG", (uint64_t)got_entries);
}

/* one line per 32 bytes: "ufs: PART WHAT +OFFSET: hex" */
static void
ufs_dump_of(const char *part, const char *what, const uint8_t *p, uint64_t n)
{
	for (uint64_t at = 0; at < n; at += 32) {
		char line[72];
		unsigned k = 0;

		for (uint64_t i = at; i < n && i < at + 32; i++) {
			line[k++] = "0123456789abcdef"[p[i] >> 4];
			line[k++] = "0123456789abcdef"[p[i] & 15];
		}
		line[k] = 0;
		print("ufs: %s %s +%x: %s\n", part, what, at, line);
	}
}

static void
ufs_dump(const char *what, const uint8_t *p, uint64_t n)
{
	ufs_dump_of("super", what, p, n);
}

/*
 * The start of userdata: where an f2fs (or ext4) of Android's would have its superblock, at 1024
 * and, for f2fs, its copy at 4096 + 1024. Plain text there says the filesystem itself lies on the
 * flash as it is; anything else says it lies under an encryption of the whole partition
 * (Android's metadata encryption, dm-default-key), and only Android can see into it.
 */
static void
ufs_dump_userdata(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 8192;

	if (bs == 0 || want % bs || EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: userdata: its start could not be read\n");
		return;
	}
	ufs_dump_of("userdata", "sb", ufs_buffer + 1024, 1024);
	ufs_dump_of("userdata", "sb2", ufs_buffer + 4096 + 1024, 64);
}

/*
 * Android's own system (system, vendor, product, ...) is not in GPT partitions but in logical ones
 * inside "super", which liblp's metadata at its start describes: a geometry at 4096 (and its copy
 * at 8192), then its metadata slots from 12288 on -- each a header and its tables of partitions,
 * extents, groups and block devices. Printed raw for tools/ufs-super.py, which checks their SHA-256 sums and
 * says where in super nothing lies: a place a volume of this port's own could have without
 * anything of Android's being moved.
 */
static void
ufs_dump_super(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 256 * 1024;
	const uint8_t *geometry = ufs_buffer + 4096;
	uint32_t max_size, slots;

	if (bs == 0 || want % bs || (bio->media->last_block + 1) * bs < want ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: super: its start could not be read\n");
		return;
	}
	if (*(const uint32_t *)geometry != 0x616c4467) {
		print("ufs: super: no liblp geometry (magic %x)\n", (uint64_t)*(const uint32_t *)geometry);
		return;
	}
	ufs_dump("geometry", geometry, 52);
	max_size = *(const uint32_t *)(geometry + 40);
	slots = *(const uint32_t *)(geometry + 44);
	/* a slot per A/B slot: the first describes super as slot _a sees it, the second as _b does */
	for (uint32_t slot = 0; slot < slots && slot < 2; slot++) {
		const uint8_t *header = ufs_buffer + 3 * 4096 + (uint64_t)slot * max_size;
		uint32_t header_size, tables_size;
		char what[8] = "header0", tables[8] = "tables0";

		if (3 * 4096 + (uint64_t)(slot + 1) * max_size > want || *(const uint32_t *)header != 0x414c5030) {
			print("ufs: super: slot %d: no metadata header within the first %d KB\n", (uint64_t)slot, want >> 10);
			continue;
		}
		header_size = *(const uint32_t *)(header + 8);
		tables_size = *(const uint32_t *)(header + 44);
		if (header_size < 128 || header_size > 256 || tables_size > max_size - header_size || tables_size > 16384) {
			print("ufs: super: slot %d: header of %d bytes, tables of %d: not dumped\n", (uint64_t)slot,
			    (uint64_t)header_size, (uint64_t)tables_size);
			continue;
		}
		what[6] = tables[6] = (char)('0' + slot);
		ufs_dump(what, header, header_size);
		ufs_dump(tables, header + header_size, tables_size);
	}
}

static void
ufs_survey(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count, units = 0, partitions = 0;

	/* the raw logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count; i++) {
			efi_block_io *bio;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
				continue;
			}
			print("ufs: unit %d: media %d, %d blocks of %d (%d MB), %s, align %d\n", units++,
			    (uint64_t)bio->media->media_id, bio->media->last_block + 1, (uint64_t)bio->media->block_size,
			    (bio->media->last_block + 1) * bio->media->block_size >> 20,
			    bio->media->read_only ? "read only" : "writable", (uint64_t)bio->media->io_align);
			ufs_check_gpt(bio, units - 1);
		}
		/* (the handle array is pool memory this function owns) */
		BS->free_pool(handles);
	}
	/* the GPT partitions, with the hash of those there are images of */
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information in this UEFI\n");
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		int r;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		partitions++;
		print("ufs: part %s: lba %d..%d, %d KB\n", name, info->start_lba, info->end_lba,
		    (bio->media->last_block + 1) * bio->media->block_size >> 10);
		if ((r = ufs_reference_of(name)) >= 0) {
			ufs_hash_partition(bio, name, ufs_references[r].bytes);
		}
		if (name[0] == 's' && name[1] == 'u' && name[2] == 'p' && name[3] == 'e' && name[4] == 'r' && name[5] == 0) {
			ufs_dump_super(bio);
		}
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 'd' &&
		    name[5] == 'a' && name[6] == 't' && name[7] == 'a' && name[8] == 0) {
			ufs_dump_userdata(bio);
		}
	}
	BS->free_pool(handles);
	print("ufs: %d raw units, %d partitions\n", units, partitions);
}

/* ------------------------------------------------------------------------ */
/* UFS block service (ufs=2): the kernel reads the phone's storage through  */
/* this loader, and this loader through Qualcomm's own UFS driver           */

/*
 * The kernel has no driver for the phone's storage, and does not need one: UEFI never exits here,
 * Qualcomm's UFS driver (UFSDxe) has the controller, its PHY, clocks and power up and stays
 * resident, and this loader already serves the kernel from core 0 while the kernel runs on another
 * (the battery). So the kernel asks for blocks through a page both can see, and core 0 reads them
 * with that driver's ReadBlocks into a buffer both can see. The kernel's side is
 * drivers/storage/hakios.ufs.c; it gets the page's address as ufs_shared_pa= on its command line.
 *
 * Reads, and -- with ufs_write=1 on the command line -- writes into one partition and nowhere
 * else: the port's own darwin, proved by its GPT entry (name, both GUIDs, first and last LBA, its
 * logical unit), and in it only past everything the build writes from the computer, so that no
 * answer to the kernel can reach Android's partitions, the Darwin volume or the cryptexes. The
 * refusals are here, in the kernel (drivers/storage/hakios.ufs.c) and in UEFI's own driver, which gives
 * out a partition's BlockIo and bounds it: a request has to pass all three.
 *
 * Each side writes its own cache lines only: the kernel the request, core 0 the answer (and the
 * table, once, before the kernel starts). A line one side cleans out of its cache therefore never
 * carries a stale copy of what the other side wrote into it. Into the data buffer core 0 writes
 * only what it has read back after a write of its own, so after the controller's DMA there is
 * nothing of its own in its cache to write back over the data; the kernel, which fills the buffer
 * for a write, cleans its own lines of it before it asks.
 */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_PART_UNIT    1u                          /* the entry is a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u
/*
 * The data volume's writes (HFS Plus on /dev/ufs_darwin_data): written and nothing more -- no flush, no read back --
 * because its journal asks for the flush itself (BLOCK_OP_FLUSH) where the order matters, and a flush and a read
 * back after every 4 KB of metadata made Darwin crawl. Offered with BLOCK_CAN_FAST in write_caps; the same bounds as
 * BLOCK_OP_WRITE. /dev/rufs_darwin's writes stay BLOCK_OP_WRITE, flushed and read back.
 */
#define BLOCK_OP_WRITE_FAST 2u
#define BLOCK_OP_FLUSH     3u                          /* the write partition's cache to the flash; one block in bounds */
#define BLOCK_CAN_FAST     1u

/*
 * The one partition a write may reach, and from which of its blocks. darwin is the port's own
 * 20 GiB at the end of logical unit 0 (README, "The port's partition"); what the build puts there
 * from the computer (tools/darwin-write.py) is the Darwin volume at its start and the two cryptexes
 * behind it, and nothing of this port reads or writes the rest -- so the data of Darwin lives there
 * and a write is refused before that block. The identity: the partition named darwin, of the APFS
 * container type, on logical unit 0, in blocks of 4096 and at least the 20 GiB the port lays out. Not
 * its place, its exact size or its unique GUID: tools/userdata-carve.py makes darwin at the end of
 * whatever userdata the phone has, with a GUID of its own (BLOCK_DARWIN_FIRST/LAST and _UNIQ were
 * the first phone's, and no write was ever served on another); they are printed instead.
 */
#define BLOCK_WRITE_FIRST  2042368ULL                  /* darwin's blocks of 4096: past both cryptexes */
#define BLOCK_DARWIN_MIN   5242875ULL                  /* blocks of 4096: the first phone's darwin, the layout's */
#define BLOCK_DARWIN_TYPE  { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } }

/*
 * The partitions first, then the logical units they are on (BLOCK_PART_UNIT, named unit0, unit1,
 * ... in the order UEFI lists them). Each partition says which of those it is on and where it
 * begins there, so that a reader of the units themselves -- the kernel's own UFS driver -- finds
 * it; a unit says its logical unit number if its device path does.
 */
struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition takes writes this run, as its index among part[] plus one (0: none), and
	 * from which of its blocks. They are here, in what was padding before ask, so that the page
	 * stays the size and the version it was: one phone is shared between working copies, and a
	 * kernel built before writes existed must go on reading through a loader built after them,
	 * and the other way about -- a kernel that does not know these fields leaves ask.op zero,
	 * which is a read, and a loader that does not know them leaves these zero, which is no write.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_caps;                   /* BLOCK_CAN_FAST; zero from a loader before it, which a kernel reads as none */
	uint64_t write_first;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/*
	 * Written by the loader before the kernel starts, with ufs_native= on the command line: where
	 * the kernel may put a transfer request list of its own into the UFS host controller for the
	 * run (hc_area, below), and UEFI's, which the loader puts back. hc_pa 0: it may not.
	 */
	struct {
		uint64_t hc_pa;                /* the controller's registers */
		uint64_t area_pa;              /* the list (1 KB, at the start) and command descriptors */
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
	} native __attribute__((aligned(128)));
	/* written by the loader during a run: it takes the controller back (a hung kernel); send nothing more */
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	/* written by the kernel: its list is in the controller (for the loader's log) */
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when the kernel's list went in */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the page must stay the size it was: the fields before ask have to fit in what was padding");

/* (8 KB is as far as this target aligns; the kernel maps whole 16 KB pages around both) */
static struct loader_block block_shared __attribute__((aligned(0x2000)));
static void block_io2_probe(efi_handle h, efi_block_io *bio);
static int block_probe_requested;       /* ufs_probe=1: block_io2_probe() at block_start() */
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
static int block_enabled;
static int block_write_requested;       /* ufs_write=1 on the command line */
static int block_write_part = -1;       /* the only partition a write may reach; -1: none, writes refused */
static uint64_t block_write_first = BLOCK_WRITE_FIRST;  /* ... and its first block a write may reach */
static uint64_t block_writes, block_writes_refused, block_write_bytes;

/* the bytes of a device path before its end node */
static uint64_t
device_path_bytes(const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0;

	for (int nodes = 0; nodes < 64; nodes++) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		if (p[n] == 0x7f || len < 4) {
			break;
		}
		n += len;
	}
	return n;
}

/*
 * A unit's device path, a node at a time ("ufs: unit N: path TYPE.SUBTYPE[data] ..."), and its
 * logical unit number if a UFS node (messaging, subtype 0x19: target, then LUN) says it.
 */
static uint8_t
device_path_print(uint64_t unit, const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0, total = device_path_bytes(dp);
	uint8_t lun = 0xff;

	print("ufs: unit %d: path", unit);
	while (n < total) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		print(" %x.%x[", (uint64_t)p[n], (uint64_t)p[n + 1]);
		for (uint64_t i = 4; i < len && i < 4 + 24; i++) {
			print("%s%x", (uint64_t)p[n + i] < 16 ? "0" : "", (uint64_t)p[n + i]);
		}
		print("%s]", len > 4 + 24 ? "..." : "");
		if (p[n] == 3 && p[n + 1] == 0x19 && len >= 6) {
			lun = p[n + 5];
		}
		n += len;
	}
	print(lun == 0xff ? "\n" : " (LUN %d)\n", (uint64_t)lun);
	return lun;
}

/*
 * The UFS host controller, for the kernel's own driver (ufs_native= on the command line; without it
 * this loader never touches the controller's registers). The kernel may, for the length of a run,
 * stop the controller's transfer request list, point it at a list of its own in hc_area (memory of
 * this loader, so that putting UEFI's back never depends on the kernel's memory), and start it
 * again; UEFI's list is one slot and its command descriptor, with no room for another request.
 * This loader keeps UEFI's list address (UTRLBA/UTRLBAU) from before the first run and checks it,
 * and puts it back if need be, on every way back from a run (hc_take_back) before anything here
 * touches the storage; it reads for the kernel (block_poll) and writes the log (write_logs) only
 * while UEFI's list is the one in the controller.
 */
#define HC_PA              (PLAT->ufs_hc)      /* the platform's; 0: ufs_native= is refused */
#define HC_CAP             0x00
#define HC_VER             0x08
#define HC_IS              0x20
#define HC_IE              0x24
#define HC_HCS             0x30
#define HC_HCE             0x34
#define HC_UTRLBA          0x50
#define HC_UTRLBAU         0x54
#define HC_UTRLDBR         0x58
#define HC_UTRLCLR         0x5c
#define HC_UTRLRSR         0x60
#define HC_UTRLCNR         0x64
#define HC_UTMRLDBR        0x78
#define HC_SLOTS           32
#define HC_UCD_OFFSET      0x1000
#define HC_UCD_SIZE        0x800
#define HC_AREA_SIZE       (HC_UCD_OFFSET + HC_SLOTS * HC_UCD_SIZE)

static uint8_t hc_area[HC_AREA_SIZE] __attribute__((aligned(0x2000)));
static int hc_requested;                /* ufs_native= on the command line */
static int hc_known;                    /* UEFI's list address is kept here: check it, put it back */
static uint32_t hc_uefi_lo, hc_uefi_hi;
static int hc_refused_reads;

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(HC_PA + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)(HC_PA + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_ms_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) * 1000 / timer_hz();
}

/* UEFI's list in, and running: what UEFI's driver needs to read or write */
static int
hc_list_is_uefis(void)
{
	return !hc_known || (hc_rd(HC_UTRLBA) == hc_uefi_lo && hc_rd(HC_UTRLBAU) == hc_uefi_hi &&
	       (hc_rd(HC_UTRLRSR) & 1) != 0);
}

static void
hc_print_state(const char *when)
{
	print("ufs: %s: list %x:%x (UEFI's %x:%x), run %x, doorbell %x, HCS %x, IS %x, IE %x, completion %x, "
	    "task doorbell %x\n", when, (uint64_t)hc_rd(HC_UTRLBAU), (uint64_t)hc_rd(HC_UTRLBA), (uint64_t)hc_uefi_hi,
	    (uint64_t)hc_uefi_lo, (uint64_t)hc_rd(HC_UTRLRSR), (uint64_t)hc_rd(HC_UTRLDBR), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hc_rd(HC_UTRLCNR), (uint64_t)hc_rd(HC_UTMRLDBR));
}

/*
 * Whatever the kernel left: UEFI's list back in the controller. The kernel is told to send nothing
 * more (it looks before every doorbell, with interrupts off), given 50 ms, then what is in flight
 * is waited for (1 s), cleared if it does not finish (UTRLCLR: every slot of that list is the
 * kernel's), the list stopped, UEFI's address written back, the list started, all read back.
 */
static void
hc_take_back(const char *when)
{
	uint32_t lo, hi, rsr, db;
	uint64_t t;

	if (!hc_known) {
		return;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	rsr = hc_rd(HC_UTRLRSR);
	db = hc_rd(HC_UTRLDBR);
	if (lo == hc_uefi_lo && hi == hc_uefi_hi && (rsr & 1)) {
		print("ufs: %s: UEFI's transfer list is in the controller (%x), running, doorbell %x\n", when, (uint64_t)lo,
		    (uint64_t)db);
		return;
	}
	print("ufs: %s: the controller has list %x:%x (run %x, doorbell %x), not UEFI's %x:%x: taking it back\n", when,
	    (uint64_t)hi, (uint64_t)lo, (uint64_t)rsr, (uint64_t)db, (uint64_t)hc_uefi_hi, (uint64_t)hc_uefi_lo);
	block_shared.loader_native.take_back = 1;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.loader_native,
	    (uint64_t)(uintptr_t)&block_shared.loader_native + sizeof(block_shared.loader_native));
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
	}
	if (db != 0) {
		print("ufs: %s: doorbell %x still set after 1 s\n", when, (uint64_t)db);
		if (hc_rd(HC_UTRLBA) != hc_uefi_lo || hc_rd(HC_UTRLBAU) != hc_uefi_hi) {
			print("ufs: %s: clearing the kernel's slots (UTRLCLR)\n", when);
			hc_wr(HC_UTRLCLR, 0);
			for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
			}
			print("ufs: %s: doorbell %x after the clear\n", when, (uint64_t)db);
		}
	}
	/* the completion notifications the kernel's requests left (it says what there was when its list went in) */
	flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
	    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
	if (block_shared.kernel_native.switched_in != 0) {
		uint32_t cnr = hc_rd(HC_UTRLCNR), ours = cnr & ~block_shared.kernel_native.cnr_at_switch;

		if (ours != 0) {
			hc_wr(HC_UTRLCNR, ours);
		}
		print("ufs: %s: completion %x, at the kernel's switch %x: cleared %x, now %x\n", when, (uint64_t)cnr,
		    (uint64_t)block_shared.kernel_native.cnr_at_switch, (uint64_t)ours, (uint64_t)hc_rd(HC_UTRLCNR));
	}
	hc_wr(HC_UTRLRSR, 0);
	for (t = READ_SYSREG(CNTVCT_EL0); (hc_rd(HC_UTRLRSR) & 1) && hc_ms_since(t) < 100;) {
	}
	hc_wr(HC_UTRLBA, hc_uefi_lo);
	hc_wr(HC_UTRLBAU, hc_uefi_hi);
	hc_wr(HC_UTRLRSR, 1);
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	hc_print_state(when);
	print("ufs: %s: UEFI's list %s\n", when, hc_list_is_uefis() ? "put back" : "NOT PUT BACK: the storage is not used");
}

/*
 * Before a run (ufs_native=): what the controller looks like, and UEFI's list address kept -- the
 * first time; later it must be the same (or this loader's own area, which is then taken back).
 */
static int
hc_init(void)
{
	uint32_t lo, hi, area = (uint32_t)(uintptr_t)hc_area;

	if (!hc_requested) {
		return 0;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	print("ufs: controller at %x: CAP %x, VER %x, HCE %x, HCS %x, IS %x, IE %x, list %x:%x, run %x, doorbell %x\n",
	    HC_PA, (uint64_t)hc_rd(HC_CAP), (uint64_t)hc_rd(HC_VER), (uint64_t)hc_rd(HC_HCE), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hi, (uint64_t)lo, (uint64_t)hc_rd(HC_UTRLRSR),
	    (uint64_t)hc_rd(HC_UTRLDBR));
	if (!hc_known) {
		if (hi == 0 && lo == area) {
			print("ufs: the controller has this loader's own area for its list before any run: left alone\n");
			return 0;
		}
		hc_uefi_lo = lo;
		hc_uefi_hi = hi;
		hc_known = 1;
		print("ufs: UEFI's transfer list is at %x:%x; it is put back after every run\n", (uint64_t)hi, (uint64_t)lo);
	} else if (lo != hc_uefi_lo || hi != hc_uefi_hi) {
		if (hi == 0 && lo == area) {
			hc_take_back("before the run");
		} else {
			print("ufs: UEFI's list moved from %x:%x to %x:%x: that is UEFI's now\n", (uint64_t)hc_uefi_hi,
			    (uint64_t)hc_uefi_lo, (uint64_t)hi, (uint64_t)lo);
			hc_uefi_lo = lo;
			hc_uefi_hi = hi;
		}
	}
	return hc_list_is_uefis();
}

/*
 * The firmware's configuration tables, by GUID (an ACPI or device tree table among them would say
 * where the storage controller is).
 */
static void
block_tables(void)
{
	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables && i < 32; i++) {
		const efi_guid *g = &ST->tables[i].guid;

		print("ufs: firmware table %d: %x-%x-%x-%x%x-%x%x%x%x%x%x at %x\n", i, (uint64_t)g->a, (uint64_t)g->b,
		    (uint64_t)g->c, (uint64_t)g->d[0], (uint64_t)g->d[1], (uint64_t)g->d[2], (uint64_t)g->d[3],
		    (uint64_t)g->d[4], (uint64_t)g->d[5], (uint64_t)g->d[6], (uint64_t)g->d[7],
		    (uint64_t)(uintptr_t)ST->tables[i].table);
	}
}

/*
 * Is this partition the one a write may reach? Everything about it has to be what it was when the
 * port was set up; one field out of place and nothing is written this run. The name UEFI reports
 * is already in bp->name (ufs_name), in ASCII.
 */
static int
block_is_darwin(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	static const efi_guid type = BLOCK_DARWIN_TYPE;
	const uint8_t *u = (const uint8_t *)&info->unique_guid;
	static const char name[] = "darwin";
	const char *why = NULL;

	for (unsigned i = 0; i < sizeof(name); i++) {
		if (bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
	}
	if (memcmp(&info->type_guid, &type, sizeof(type)) != 0) {
		why = "its type GUID is not the one it had";
	} else if (bp->unit != 0) {
		/* the first unit UEFI lists: this firmware's device paths say no logical unit number
		 * (device_path_print finds no 3.19 node), so bp->lun is 0xff for every unit */
		why = "it is not on the first logical unit";
	} else if (bp->block_size != 4096 || bp->blocks < BLOCK_DARWIN_MIN ||
	    info->end_lba - info->start_lba + 1 != bp->blocks) {
		why = "it is not 20 GiB in blocks of 4096";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: darwin: no write will be served: %s\n", why);
		return 0;
	}
	print("ufs: darwin (LBA %d..%d of unit 0, unique GUID %x...%x): its blocks from %d on may be written this run\n",
	    info->start_lba, info->end_lba, (uint64_t)u[3] << 24 | (uint64_t)u[2] << 16 | (uint64_t)u[1] << 8 | u[0],
	    (uint64_t)u[12] << 24 | (uint64_t)u[13] << 16 | (uint64_t)u[14] << 8 | u[15], BLOCK_WRITE_FIRST);
	return 1;
}

/*
 * A port with no darwin (PLAT->data_part: the Galaxy S9's USERDATA, given up to Darwin): that one partition, by its
 * exact name, is the data volume from its first block (the kernel's /dev/ufs_NAME_data, HFS Plus made once from
 * TWRP). Nothing of the port's is in it, so there is nothing to keep out but the other partitions; it has to be a
 * partition (not a whole unit), on the first logical unit, in blocks of 4096, at least 1 GiB, whole as its GPT entry
 * says, and writable as UEFI gives it out. Anything else: nothing is written this run.
 */
#define BLOCK_DATA_MIN     262144ULL                   /* blocks of 4096: 1 GiB */

static int
block_is_data_part(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	const char *name = PLAT->data_part, *why = NULL;

	if (name == NULL) {
		return 0;
	}
	for (unsigned i = 0;; i++) {
		if (i >= sizeof(bp->name) || bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
		if (name[i] == 0) {
			break;
		}
	}
	if (bp->unit != 0) {
		why = "it is not on the first logical unit";
	} else if (bp->block_size != 4096 || bp->blocks < BLOCK_DATA_MIN ||
	    info->end_lba - info->start_lba + 1 != bp->blocks) {
		why = "it is not at least 1 GiB in blocks of 4096, whole";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: %s: no write will be served: %s\n", name, why);
		return 0;
	}
	print("ufs: %s (LBA %d..%d of unit 0): the data volume, all of its blocks may be written this run\n", name,
	    info->start_lba, info->end_lba);
	return 1;
}

static void
block_start(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	static const efi_guid dp_guid = EFI_DEVICE_PATH_GUID;
	static struct {
		efi_block_io *bio;
		const efi_device_path *path;
		uint64_t path_bytes;
		uint8_t lun;
	} units[BLOCK_MAX_UNITS];
	uint64_t nunits = 0;

	memset(&block_shared, 0, sizeof(block_shared));
	block_tables();
	/* the logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count && nunits < BLOCK_MAX_UNITS; i++) {
			efi_block_io *bio;
			efi_device_path *dp;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical ||
			    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE ||
			    EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
				continue;
			}
			units[nunits].bio = bio;
			units[nunits].path = dp;
			units[nunits].path_bytes = device_path_bytes(dp);
			units[nunits].lun = device_path_print(nunits, dp);
			nunits++;
		}
		/* (the handle array is pool memory this function owns; units[] holds the protocols, not the array) */
		BS->free_pool(handles);
	}
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information: the block service is off\n");
		return;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + nunits < BLOCK_MAX_PARTS; i++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		efi_partition_info *info;
		efi_block_io *bio;
		efi_device_path *dp;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) ||
		    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
			continue;
		}
		ufs_name(info, bp->name, sizeof(bp->name));
		bp->block_size = bio->media->block_size;
		bp->blocks = bio->media->last_block + 1;
		bp->start_lba = info->start_lba;
		bp->unit = 0xffff;
		bp->lun = 0xff;
		/* its unit: the one whose device path its own begins with */
		if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
			uint64_t bytes = device_path_bytes(dp);

			for (uint64_t u = 0; u < nunits; u++) {
				if (units[u].path_bytes < bytes &&
				    memcmp(units[u].path, dp, units[u].path_bytes) == 0) {
					bp->unit = (uint16_t)u;
					bp->lun = units[u].lun;
				}
			}
		}
		/* a port that names its data partition writes only that one; the others only darwin */
		if (block_write_requested &&
		    (PLAT->data_part != NULL ? block_is_data_part(info, bio, bp) : block_is_darwin(info, bio, bp))) {
			block_write_part = (int)block_shared.parts;
			block_write_first = PLAT->data_part != NULL ? 0 : BLOCK_WRITE_FIRST;
		}
		block_bios[block_shared.parts++] = bio;
		if (block_probe_requested && bp->name[0] == 'd' && bp->name[1] == 'a' && bp->name[2] == 'r' &&
		    bp->name[3] == 'w' && bp->name[4] == 'i' && bp->name[5] == 'n' && bp->name[6] == 0) {
			block_io2_probe(handles[i], bio);
		}
	}
	BS->free_pool(handles);
	for (uint64_t u = 0; u < nunits; u++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		char *n = bp->name;

		n[0] = 'u', n[1] = 'n', n[2] = 'i', n[3] = 't';
		n[4] = (char)('0' + u / 10 % 10), n[5] = (char)('0' + u % 10), n[6] = 0;
		if (u < 10) {
			n[4] = n[5], n[5] = 0;
		}
		bp->block_size = units[u].bio->media->block_size;
		bp->blocks = units[u].bio->media->last_block + 1;
		bp->unit = (uint16_t)u;
		bp->lun = units[u].lun;
		bp->flags = BLOCK_PART_UNIT;
		block_bios[block_shared.parts++] = units[u].bio;
	}
	/* the kernel's own transfer list: this loader's area, if the controller is as UEFI keeps it */
	if (hc_known && hc_list_is_uefis() && hc_rd(HC_UTRLDBR) == 0 && (hc_rd(HC_HCE) & 1)) {
		memset(hc_area, 0, sizeof(hc_area));
		flush_dcache((uint64_t)(uintptr_t)hc_area, (uint64_t)(uintptr_t)hc_area + sizeof(hc_area));
		block_shared.native.hc_pa = HC_PA;
		block_shared.native.area_pa = (uint64_t)(uintptr_t)hc_area;
		block_shared.native.area_size = sizeof(hc_area);
		block_shared.native.ucd_offset = HC_UCD_OFFSET;
		block_shared.native.ucd_size = HC_UCD_SIZE;
		block_shared.native.slots = HC_SLOTS;
		block_shared.native.uefi_utrlba = hc_uefi_lo;
		block_shared.native.uefi_utrlbau = hc_uefi_hi;
		print("ufs: the kernel may put its own transfer list in, at %x (this loader's), for the run\n",
		    (uint64_t)(uintptr_t)hc_area);
	} else if (hc_requested) {
		print("ufs: the kernel's own transfer list is not offered: the controller is not as UEFI keeps it\n");
	}
	hc_refused_reads = 0;
	hc_log_skips = 0;
	/* per run, like the two above: this loader stays resident across runs, and a tally carried
	 * over from the run before is read as this one's */
	block_writes = block_writes_refused = block_write_bytes = 0;
	if (block_write_part >= 0) {
		block_shared.write_part_plus_one = (uint32_t)block_write_part + 1;
		block_shared.write_first = block_write_first;
		block_shared.write_caps = BLOCK_CAN_FAST;
	}
	block_shared.magic = BLOCK_MAGIC;
	block_shared.version = BLOCK_VERSION;
	block_shared.size = sizeof(block_shared);
	block_shared.buffer_size = BLOCK_BUFFER_SIZE;
	block_shared.buffer_pa = (uint64_t)(uintptr_t)block_buffer;
	clean_dcache((uint64_t)(uintptr_t)&block_shared, (uint64_t)(uintptr_t)&block_shared + sizeof(block_shared));
	block_enabled = 1;
	print("ufs: block service: %d partitions and %d units, page %x, buffer %x of %d KB\n",
	    (uint64_t)block_shared.parts - nunits, nunits, (uint64_t)(uintptr_t)&block_shared, block_shared.buffer_pa,
	    (uint64_t)(BLOCK_BUFFER_SIZE >> 10));
}

/*
 * Whether the storage could have more than one request in flight (ufs_probe=1): Block I/O 2
 * on the partitions, and whether its reads come back before they are done.  Eight 4 KB
 * reads at places 64 MB apart in "darwin", one after another through Block I/O and then all
 * eight at once through Block I/O 2, each into its own part of the buffer.  Before the kernel
 * runs (the UEFI timer still ticks), and only to say what there is.
 *
 * What it says on this phone: Block I/O 2 is there and the calls come back at once (eight in
 * 160 us, none done), but the eight are done only after 61.5 ms, against 1.0 ms one at a time
 * through Block I/O -- Qualcomm's driver finishes them from the UEFI timer, which besides does
 * not tick on core 0 while the kernel runs.  So one request at a time, synchronously.
 */
static void
block_io2_probe(efi_handle h, efi_block_io *bio)
{
	static const efi_guid blk2_guid = EFI_BLOCK_IO2_GUID;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void *, void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void *, void *, void **))BS->create_event;
	efi_status (EFIAPI *check_event)(void *) = (efi_status (EFIAPI *)(void *))BS->check_event;
	static efi_block_io2_token tokens[8];
	efi_block_io2 *bio2 = NULL;
	uint64_t bs = bio->media->block_size, t0, t_sync, t_issue = 0, t_all, freq = timer_hz();
	uint64_t returned_done = 0, pending, polls = 0;
	efi_status s;

	if (EFI_ERROR(BS->handle_protocol(h, &blk2_guid, (void **)&bio2)) || bio2 == NULL) {
		print("ufs: darwin: no Block I/O 2: one request at a time it is\n");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		s = bio->read_blocks(bio, bio->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 7, bs,
		    block_buffer + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: Block I/O read failed %x\n", s);
			return;
		}
	}
	t_sync = READ_SYSREG(CNTVCT_EL0) - t0;
	for (int i = 0; i < 8; i++) {
		tokens[i].transaction_status = 0;
		if (EFI_ERROR(create_event(0, 8 /* TPL_CALLBACK */, NULL, NULL, &tokens[i].event))) {
			print("ufs: probe: no event\n");
			return;
		}
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		/* other places than the synchronous reads, so that nothing the storage kept is read again */
		s = bio2->read_blocks_ex(bio2, bio2->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 1031,
		    &tokens[i], bs, block_buffer + 1048576 + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: ReadBlocksEx %d failed %x\n", (uint64_t)i, s);
			return;
		}
		if (check_event(tokens[i].event) == 0) {
			returned_done++;        /* already signalled when the call came back */
		}
	}
	t_issue = READ_SYSREG(CNTVCT_EL0) - t0;
	pending = 8 - returned_done;
	while (pending > 0 && polls < 50000000) {
		pending = 0;
		for (int i = 0; i < 8; i++) {
			if (tokens[i].event != NULL && check_event(tokens[i].event) != 0) {
				pending++;
			} else if (tokens[i].event != NULL) {
				BS->close_event(tokens[i].event);
				tokens[i].event = NULL;
			}
		}
		polls++;
	}
	t_all = READ_SYSREG(CNTVCT_EL0) - t0;
	print("ufs: darwin: Block I/O 2 there. 8 random 4 KB reads: one at a time %d us; at once: the calls came back "
	    "after %d us (%d of them done by then), all done after %d us, %d still pending\n",
	    t_sync * 1000000 / freq, t_issue * 1000000 / freq, returned_done, t_all * 1000000 / freq, pending);
}

/* one request, if there is one: called from core 0's watch loop while the kernel runs */
static void
block_poll(void)
{
	uint64_t ask = (uint64_t)(uintptr_t)&block_shared.ask;
	uint32_t request, part, op;
	uint64_t lba, count, bytes, started;
	efi_status s = 0x8000000000000002ULL;     /* EFI_INVALID_PARAMETER: a request out of bounds */

	/* the kernel writes the request into memory past this core's cache: look at memory */
	flush_dcache(ask, ask + sizeof(block_shared.ask));
	request = block_shared.ask.request;
	if (request == block_shared.answer.answered) {
		return;
	}
	part = block_shared.ask.part;
	lba = block_shared.ask.lba;
	count = block_shared.ask.count;
	op = block_shared.ask.op;
	started = READ_SYSREG(CNTVCT_EL0);
	bytes = 0;
	if (part < block_shared.parts && count != 0 && count <= BLOCK_BUFFER_SIZE / block_shared.part[part].block_size &&
	    lba < block_shared.part[part].blocks && count <= block_shared.part[part].blocks - lba) {
		efi_block_io *bio = block_bios[part];

		bytes = count * block_shared.part[part].block_size;
		/* the kernel says its own list is in (ufs=3, or ufs_native=1): it should not ask, and is not served */
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		if (block_shared.kernel_native.list_in != 0 || !hc_list_is_uefis()) {
			/* the kernel's own list is in: UEFI's driver must not ring now (the kernel should not ask) */
			if (hc_refused_reads++ < 4) {
				print("ufs: a read asked for while the kernel's own transfer list is in the controller: refused\n");
			}
			s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
			bytes = 0;
		} else if (op == BLOCK_OP_WRITE || op == BLOCK_OP_WRITE_FAST || op == BLOCK_OP_FLUSH) {
			if (block_write_part < 0 || part != (uint32_t)block_write_part || lba < block_write_first) {
				/* not darwin (or the port's data partition), or not past what the build writes there: never */
				if (block_writes_refused++ < 8) {
					print("ufs: a write of %d blocks from %d of %s: REFUSED\n", count, lba,
					    block_shared.part[part].name);
				}
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
				bytes = 0;
			} else if (op == BLOCK_OP_FLUSH) {
				s = bio->flush_blocks(bio);
				bytes = 0;
			} else if (op == BLOCK_OP_WRITE_FAST) {
				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			} else {
				/*
				 * The kernel filled the buffer and cleaned its lines of it; drop whatever this
				 * core holds, write, and read back -- the answer says the blocks hold what the
				 * kernel gave, or the write is no good. The read back overwrites the buffer,
				 * which the kernel does not need: it has the data.
				 */
				uint32_t want;

				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				want = ufs_crc32(block_buffer, bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					bio->flush_blocks(bio);
					s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
					flush_dcache((uint64_t)(uintptr_t)block_buffer,
					    (uint64_t)(uintptr_t)block_buffer + bytes);
					if (!EFI_ERROR(s) && ufs_crc32(block_buffer, bytes) != want) {
						print("ufs: %d blocks from %d of %s read back DIFFERENT\n",
						    count, lba, block_shared.part[part].name);
						s = 0x800000000000000AULL;      /* EFI_VOLUME_CORRUPTED */
					}
				}
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			}
		} else if (op == BLOCK_OP_READ) {
			s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
			/* what the controller wrote is in memory; drop whatever this core held of the buffer */
			flush_dcache((uint64_t)(uintptr_t)block_buffer, (uint64_t)(uintptr_t)block_buffer + bytes);
		} else {
			bytes = 0;              /* an operation this loader does not know: nothing is touched */
		}
	}
	block_shared.answer.status = (int64_t)s;
	block_shared.answer.ticks += READ_SYSREG(CNTVCT_EL0) - started;
	if (EFI_ERROR(s)) {
		block_shared.answer.failed++;
	} else {
		block_shared.answer.served++;
		block_shared.answer.bytes += bytes;
	}
	__asm__ volatile ("dmb sy");
	block_shared.answer.answered = request;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.answer,
	    (uint64_t)(uintptr_t)&block_shared.answer + sizeof(block_shared.answer));
}

static void
charger_start(void)
{
	static const efi_guid charger_ex_guid = QCOM_CHARGER_EX_GUID;
	static const efi_guid qcom_charger_guid = QCOM_CHARGER_GUID;
	efi_status ex_status, qcom_status, init_status = 0;

	if (charger_enabled || PLAT != &platform_redwood) {
		return;
	}
	ex_status = BS->locate_protocol(&charger_ex_guid, NULL, (void **)&CHARGER_EX);
	qcom_status = BS->locate_protocol(&qcom_charger_guid, NULL, (void **)&QCOM_CHARGER);
	if (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL) {
		/* Re-open the driver's status path after QcomChargerApp handed off to the OS. */
		init_status = QCOM_CHARGER->initialize(1);
	}
	charger_enabled = (!EFI_ERROR(ex_status) && CHARGER_EX != NULL) ||
	    (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL);
	print("battery: ChargerEx=%x QcomCharger=%x init=%x shared=%x\n", ex_status, qcom_status, init_status,
	    (uint64_t)(uintptr_t)&charger_shared);
	charger_poll();
	print("battery: flags=%x SOC=%d%% V=%d mV I=%d mA T=%d C action=%d\n", (uint64_t)charger_shared.flags,
	    (uint64_t)(int64_t)charger_shared.soc_percent, (uint64_t)(int64_t)charger_shared.voltage_mv,
	    (uint64_t)(int64_t)charger_shared.current_ma, (uint64_t)(int64_t)charger_shared.temperature_c,
	    (uint64_t)charger_shared.charger_action);
}

/* For the record: how much the kernel on the other core has logged so far, and the end of it. */
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;

	if (live == NULL) {
		print("%d s\n", seconds);
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	from = len > 240 ? len - 240 : 0;
	flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
	print("%d s: the kernel has logged %d bytes, heartbeat %d; the last of it:\n", seconds, (uint64_t)len, live->heartbeat);
	for (uint32_t i = from; i < len; i++) {
		put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
	}
	print("\n");
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 */
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
	int64_t status;
	int arrived = 0, watching_only = 0;

	running_live = live;
	run_active = 1;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;
	other_cpu_stage = 0;
	other_cpu_done = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	/* the last argument arrives in that core's x0 */
	status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);

	started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
	print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
	if (PLAT->log_other_cpu) {
		flush_log();
	}
	while (status == 0) {
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		if (!watching_only && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			if (PLAT->log_other_cpu) {
				if (!arrived) {
					print("%d s: the core has not arrived\n", noted);
				} else {
					note_kernel_progress(live, slide, noted);
				}
				flush_log();
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			watching_only = 1;
			daif = READ_SYSREG(DAIF);
			__asm__ volatile ("msr daifset, #0xf");
			if (loader_gic_sysregs) {
				group1 = READ_SYSREG(S3_0_C12_C12_7);  /* ICC_IGRPEN1_EL1 */
				__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0));
			}
		}
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog. The Galaxy S9's (Exynos) watchdog
		 * is fed for as long as this core watches a kernel with signs of life to watch: a kernel that stops giving them
		 * is this core's to catch (OTHER_CPU_HUNG), and the watchdog is for a hang that takes this core too.
		 */
		if (wdt_armed && seconds_since(last_pet) >= 5 &&
		    (seconds_since(started) < budget + 60 || (exynos_wdt_owns() && live != NULL))) {
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side; keep XNU's cache live. */
		if (charger_enabled && seconds_since(last_battery) >= 5 && hc_list_is_uefis()) {
			/* (not while the kernel's own transfer list is in: Qualcomm's DXEs are not to touch the storage) */
			charger_poll();
			last_battery = READ_SYSREG(CNTVCT_EL0);
		}
		if (arrived && live != NULL) {
			if (live->heartbeat != last_beat) {
				last_beat = live->heartbeat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
		}
		if (block_enabled) {
			block_poll();
		}
		usb_poll_command();
		if (block_enabled) {
			/*
			 * The pause is spent looking at the kernel's mailbox: with one look a pass, a
			 * request waited 60-75 us on average before this core noticed it, more than a
			 * 4 KB ReadBlocks takes (27 us). Same length of pause as below.
			 */
			for (int k = 0; k < 64; k++) {
				block_poll();
				for (volatile int i = 0; i < 300; i++) {
				}
			}
		} else {
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}
	running_live = NULL;
	run_active = 0;

	if (watching_only) {
		if (loader_gic_sysregs) {
			__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
		}
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	return status;
}

/* ------------------------------------------------------------------------ */
/* Exynos cluster watchdog (the Galaxy S9: platform_starlte's exynos_wdt).   */
/* Armed from the loader's start to the end of the run and fed by this core  */
/* while it watches a kernel on another: when the whole SoC hangs, this core */
/* with it, the watchdog resets the SoC. S-Boot then finds INFORM3 cleared   */
/* and starts BOOT, not TWRP: a run that ends normally restarts with the     */
/* PMU's SWRESET instead (platform_restart), which keeps INFORM3.            */

/*
 * The facts are the mainline Linux port to the S9+'s, which turns every hang into a restart into TWRP this way
 * (github.com/SheroAbi/galaxy-s9plus-mainline-linux: its device tree, src/mainline-dts/exynos9810-star2lte.dts, GPL-2.0
 * OR BSD-3-Clause -- the clock-gates node's watchdog writes and the watchdog@10060000 node; docs/04-drivers.md and
 * 06-known-issues.md), and Samsung's kernel's (drivers/watchdog/s3c2410_wdt.c, and the phone's device tree's
 * watchdog_cl0 and watchdog_cl1; GPL, read for the facts):
 *   the watchdogs  cluster 0's at 0x10050000 ("samsung,exynos8-wdt": its PMU bit is 24), cluster 1's at 0x10060000
 *                  ("samsung,exynos7-wdt": bit 23; SPI 465). Both count the 26 MHz oscillator. WTCON +0x0 (bit 0 the
 *                  reset, bit 2 the interrupt, [4:3] the divider 16 << n, bit 5 on, [15:8] the prescaler less 1),
 *                  WTDAT +0x4 (the count reloaded), WTCNT +0x8 (the count; written to feed it), WTCLRINT +0xC. S-Boot
 *                  and the kernel before leave both running; the S9+ port stops cluster 0's and arms cluster 1's
 *   the PMU        (0x14060000) MASK_WDT_RESET_REQUEST +0x40C: with a watchdog's bit set its expiry resets nothing. S-Boot
 *                  leaves it all set (0xFFFFFFFF): on the S9+, arming the watchdog alone changed nothing until bit 23 was
 *                  cleared. DISABLE_WDT +0x408: with the bit set the PMU stops the watchdog by itself; Samsung's and
 *                  mainline's drivers clear it when they arm one. RST_STAT +0x404: why the SoC last reset (bit 23:
 *                  cluster 1's watchdog)
 *   the timeout    prescaler 255, divider 128 (WTCON 0xFF39: on, the reset, no interrupt): 26 MHz / 256 / 128 = 793.457
 *                  Hz in 16 bits of count, 82 s at most; 30 s is about 23805
 *   afterwards     S-Boot reads INFORM3 = 0x12345674 as "recovery" (and INFORM2 = 0x12345678: a restart, not a power-off).
 *                  The S9+ port writes them first thing and has its watchdog's resets land in TWRP; on this S9 they do
 *                  not: after every reset by this watchdog (S-Boot: "WDT_RST2 (0x800000)") its log reads "INF2: 0 /
 *                  INF3: 0", what was written here gone, and BOOT is started, the run again (only keys held send it to
 *                  TWRP). After the PMU's software reset (SWRESET, "SW_RST (0x20000000)") they are kept. So INFORM2/3 are
 *                  written here (recovery_after_reset()) before the watchdog is armed, and again right before the
 *                  loader's own restart, which is SWRESET (platform_restart()); the watchdog is the last resort, for a
 *                  hang of this core too, and that lands in BOOT. recovery=0: not written, and a restart is BOOT again
 * UEFI's page tables (devices/starlte/uefi, MemoryMapLib) do not map the watchdogs: their pages are mapped through the
 * CPU protocol's SetMemoryAttributes as device memory, and touched only if the processor then translates them (AT
 * S1E1R; Carbon at EL2 has no stage 2) to themselves as device memory.
 * Boot-arg:
 *   wdt=0  the Galaxy S9's hardware watchdog is not armed (a restart still goes to recovery: INFORM3 is written)
 */
#define EXYNOS_WTCON            0x0
#define EXYNOS_WTDAT            0x4
#define EXYNOS_WTCNT            0x8
#define EXYNOS_WTCLRINT         0xC
#define EXYNOS_WTCON_ARMED      0xFF39u         /* prescaler 255, divider 128, on, the reset; no interrupt */
#define EXYNOS_WTCON_RUN        0x21u           /* on (bit 5) and the reset (bit 0) */
#define EXYNOS_WDT_MILLIHZ      793457ull       /* 26 MHz / 256 / 128, in thousandths of a hertz */
#define EXYNOS_WDT_MS           30000u          /* while the loader runs; wdt_set_ms() says how long around the kernel */
#define EXYNOS_PMU_RST_STAT     0x404
#define EXYNOS_PMU_WDT_DISABLE  0x408
#define EXYNOS_PMU_WDT_MASK     0x40C
#define EXYNOS_PMU_INFORM2      0x808
#define EXYNOS_PMU_INFORM3      0x80C

static int exynos_wdt_on;               /* armed, and fed by wdt_pet() */
static int exynos_wdt_paused;           /* stopped while a kernel runs on this core; wdt_set_ms() arms it again */
static uint32_t exynos_wdt_count;
static int starts_with(const char *s, const char *prefix);

static inline uint32_t
exynos_rd(uint64_t at)
{
	return *(volatile uint32_t *)(uintptr_t)at;
}

static inline void
exynos_wr(uint64_t at, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)at = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* 1: the watchdog is this block's to feed (armed, or stopped for a while), not the hypervisor's */
static int
exynos_wdt_owns(void)
{
	return exynos_wdt_on || exynos_wdt_paused;
}

/* the page at `pa` as the processor translates it now: 1 if to itself, as device memory */
static int
exynos_page_ok(uint64_t pa)
{
	uint64_t par;

	__asm__ volatile ("at s1e1r, %0\nisb" : : "r" (pa) : "memory");
	par = READ_SYSREG(PAR_EL1);
	/* F (bit 0) clear; the address [47:12]; the attributes [63:56] of device memory, 0b0000xx00 */
	return !(par & 1) && (par & 0x0000FFFFFFFFF000ull) == (pa & ~0xFFFull) && (par >> 60) == 0;
}

static int
exynos_page_map(uint64_t pa)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;
	efi_status s;

	if (exynos_page_ok(pa)) {
		return 1;
	}
	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu)) || cpu->set_memory_attributes == NULL) {
		print("watchdog: %x is not mapped, and there is no CPU protocol to map it\n", pa);
		return 0;
	}
	s = cpu->set_memory_attributes(cpu, pa & ~0xFFFull, 0x1000, EFI_MEMORY_UC);
	print("watchdog: %x mapped as device memory by the firmware: status %x, %s\n", pa, (uint64_t)s,
	    exynos_page_ok(pa) ? "it translates" : "IT STILL DOES NOT TRANSLATE");
	return exynos_page_ok(pa);
}

static uint32_t
exynos_wdt_counts(uint64_t ms)
{
	uint64_t n = ms * EXYNOS_WDT_MILLIHZ / 1000000;

	return n == 0 ? 1 : n > 0xFFFF ? 0xFFFF : (uint32_t)n;
}

/* fed: its count back to the full time */
static void
exynos_wdt_feed(void)
{
	if (exynos_wdt_on) {
		exynos_wr(PLAT->exynos_wdt + EXYNOS_WTCNT, exynos_wdt_count);
	}
}

/* the time to the reset, from now; a watchdog stopped for a kernel on this core is armed again */
static void
exynos_wdt_set_ms(uint64_t ms)
{
	uint64_t wdt = PLAT->exynos_wdt;

	exynos_wdt_count = exynos_wdt_counts(ms);
	exynos_wr(wdt + EXYNOS_WTDAT, exynos_wdt_count);
	exynos_wr(wdt + EXYNOS_WTCNT, exynos_wdt_count);
	if (exynos_wdt_paused) {
		exynos_wr(wdt + EXYNOS_WTCON, EXYNOS_WTCON_ARMED);
		exynos_wdt_paused = 0;
		exynos_wdt_on = 1;
		print("watchdog: armed again, %d s\n", ms / 1000);
	}
}

/* stopped, for as long as nothing can feed it (a kernel on this core) */
static void
exynos_wdt_pause(const char *why)
{
	if (!exynos_wdt_on) {
		return;
	}
	exynos_wr(PLAT->exynos_wdt + EXYNOS_WTCON, exynos_rd(PLAT->exynos_wdt + EXYNOS_WTCON) & ~EXYNOS_WTCON_RUN);
	exynos_wdt_on = 0;
	exynos_wdt_paused = 1;
	print("watchdog: stopped: %s\n", why);
}

/*
 * From efi_main(), once the payload's command line is known: INFORM2/3 for recovery (unless recovery=0), then the
 * watchdog armed for EXYNOS_WDT_MS (unless wdt=0), the other cluster's stopped, and its reset unmasked in the PMU.
 */
static void
exynos_wdt_start(const char *cmdline)
{
	uint64_t pmu = PLAT->recovery_pmu, wdt = PLAT->exynos_wdt, other = PLAT->exynos_wdt_other;
	uint32_t bit = 1u << PLAT->exynos_wdt_pmu_bit, rst;
	int wanted = 1;

	if (wdt == 0 || pmu == 0) {
		return;
	}
	for (const char *c = cmdline; c != NULL && *c; c++) {
		if (c != cmdline && c[-1] != ' ') {
			continue;
		}
		if (starts_with(c, "recovery=0") && (c[10] == 0 || c[10] == ' ')) {
			reset_to_recovery = 0;
		}
		if (starts_with(c, "wdt=0") && (c[5] == 0 || c[5] == ' ')) {
			wanted = 0;
		}
	}
	/* first: a restart from here on goes to recovery, the watchdog's included */
	recovery_after_reset();
	rst = exynos_rd(pmu + EXYNOS_PMU_RST_STAT);
	print("watchdog: INFORM2 %x, INFORM3 %x: %s; the last reset: RST_STAT %x%s\n",
	    (uint64_t)exynos_rd(pmu + EXYNOS_PMU_INFORM2), (uint64_t)exynos_rd(pmu + EXYNOS_PMU_INFORM3),
	    reset_to_recovery ? "a restart goes to recovery" : "recovery=0, a restart comes back here", (uint64_t)rst,
	    (rst & bit) ? " (this watchdog's: S-Boot found INFORM2/3 cleared by it, and started BOOT unless keys were held)" :
	    (rst & 0x20000000u) ? " (a software reset)" : "");
	print("watchdog: PMU DISABLE_WDT %x, MASK_WDT_RESET_REQUEST %x as found\n",
	    (uint64_t)exynos_rd(pmu + EXYNOS_PMU_WDT_DISABLE), (uint64_t)exynos_rd(pmu + EXYNOS_PMU_WDT_MASK));
	if (!wanted) {
		print("watchdog: wdt=0: not armed (the boot's watchdogs left as they are, their resets masked)\n");
		return;
	}
	if (!exynos_page_map(wdt) || !exynos_page_map(other)) {
		print("watchdog: its registers cannot be reached: NOT ARMED\n");
		return;
	}
	print("watchdog: %x WTCON %x WTDAT %x WTCNT %x, the other cluster's (%x) WTCON %x, as found\n", wdt,
	    (uint64_t)exynos_rd(wdt + EXYNOS_WTCON), (uint64_t)exynos_rd(wdt + EXYNOS_WTDAT),
	    (uint64_t)exynos_rd(wdt + EXYNOS_WTCNT), other, (uint64_t)exynos_rd(other + EXYNOS_WTCON));
	exynos_wr(other + EXYNOS_WTCON, 0);
	exynos_wdt_count = exynos_wdt_counts(EXYNOS_WDT_MS);
	exynos_wr(wdt + EXYNOS_WTCON, exynos_rd(wdt + EXYNOS_WTCON) & ~EXYNOS_WTCON_RUN);
	exynos_wr(wdt + EXYNOS_WTDAT, exynos_wdt_count);
	exynos_wr(wdt + EXYNOS_WTCNT, exynos_wdt_count);
	exynos_wr(wdt + EXYNOS_WTCLRINT, 1);
	exynos_wr(wdt + EXYNOS_WTCON, EXYNOS_WTCON_ARMED);
	/* then its reset unmasked, and the PMU kept from stopping it */
	exynos_wr(pmu + EXYNOS_PMU_WDT_DISABLE, exynos_rd(pmu + EXYNOS_PMU_WDT_DISABLE) & ~bit);
	exynos_wr(pmu + EXYNOS_PMU_WDT_MASK, exynos_rd(pmu + EXYNOS_PMU_WDT_MASK) & ~bit);
	exynos_wdt_on = 1;
	wdt_armed = 1;
	print("watchdog: armed, %d s (WTCON %x, WTDAT %x); DISABLE_WDT %x, MASK_WDT_RESET_REQUEST %x; the other's WTCON %x\n",
	    (uint64_t)(EXYNOS_WDT_MS / 1000), (uint64_t)exynos_rd(wdt + EXYNOS_WTCON), (uint64_t)exynos_rd(wdt + EXYNOS_WTDAT),
	    (uint64_t)exynos_rd(pmu + EXYNOS_PMU_WDT_DISABLE), (uint64_t)exynos_rd(pmu + EXYNOS_PMU_WDT_MASK),
	    (uint64_t)exynos_rd(other + EXYNOS_WTCON));
}

/* ------------------------------------------------------------------------ */
/* Hypervisor watchdog (Qualcomm "Haven" virtual watchdog, SMC interface as  */
/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (exynos_wdt_owns()) {
		exynos_wdt_feed();      /* (the Galaxy S9's: see above) */
		return;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (exynos_wdt_owns()) {
		exynos_wdt_set_ms(ms);  /* (the Galaxy S9's: see above) */
		return;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

	if (PLAT != &platform_redwood) {
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
	wdt_armed = (r_pet == 0);
}

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
	flush_log();
	platform_restart();
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	uint32_t kernel_size;           /* the kernel entry's size: what its Mach-O header may point into */
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	const uint8_t *resident;        /* ... or one the payload carries, which the loader puts there itself */
	uint32_t resident_size;
	const char *kernel_version;     /* kernel.ver: the version of the kernel in this payload (tools/kernel-version.py) */
	int usb_mode;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

/* How many bytes after the header the walk over a payload's entries may cover: the header's size, but never more than fits */
static uint64_t
payload_walk_size(uint32_t header_size)
{
	uint64_t most = PAYLOAD_MAX_SIZE - 0x100000ULL - sizeof(struct payload_header);

	return header_size < most ? header_size : most;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)payload_base;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", payload_base, (uint64_t)ph->magic);
		return 0;
	}
	/*
	 * The size in the header is not trusted past what the payload region can hold (the file's size is not
	 * known here), so a wild value cannot send the walk through memory that is not the payload.
	 */
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + payload_walk_size(ph->size);
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		/*
		 * An entry that claims more bytes than are left is a torn or made-up payload: the ramdisk copy would read
		 * past it, and the 32-bit rounding below would wrap. Not booted, as with a payload of the wrong magic.
		 */
		if ((uint64_t)e->size > (uint64_t)(pend - (p + sizeof(*e)))) {
			print("payload entry is larger than the payload (size %x): not used\n", (uint64_t)e->size);
			return 0;
		}
		if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
			pi->kernel_size = e->size;
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "resident")) {
			pi->resident = p + sizeof(*e);            /* the image xl ramdisk would send: a boot with no host */
			pi->resident_size = e->size;
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "kernel.ver")) {
			pi->kernel_version = text;                /* printed under the banner; the kernel gets it as hakios_kver */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

static int
choose_window(const struct payload_info *pi)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	/* what XNU's window must stay clear of: the payload where it landed and where it is now, and the platform's keep-outs */
	uint64_t avoid[2 + PLATFORM_KEEP_OUT_MAX][2] = {
		{ PAYLOAD_BASE, PAYLOAD_BASE + PAYLOAD_MAX_SIZE }, { payload_base, payload_base + PAYLOAD_MAX_SIZE },
	};
	for (int k = 0; k < PLATFORM_KEEP_OUT_MAX; ++k) {
		avoid[2+k][0] = PLAT->keep_out[k].base;
		avoid[2+k][1] = PLAT->keep_out[k].base + PLAT->keep_out[k].size;
	}

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
		/* the payload and the keep-outs may sit in RAM that UEFI considers free: stay clear of them (the larger side) */
		for (int a = 0; a < 2 + PLATFORM_KEEP_OUT_MAX; a++) {
			uint64_t pay_lo = avoid[a][0], pay_hi = avoid[a][1];

			if (pay_hi > pay_lo && lo < pay_hi && hi > pay_lo) {
				uint64_t left = pay_lo > lo ? pay_lo - lo : 0;
				uint64_t right = hi > pay_hi ? hi - pay_hi : 0;
				if (left > right) {
					hi = pay_lo;
				} else {
					lo = pay_hi;
				}
			}
		}
		if (hi <= lo) {
			continue;
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	for (int k = 0; k < PLATFORM_KEEP_OUT_MAX; k++) {
		uint64_t ko = PLAT->keep_out[k].base, ke = ko + PLAT->keep_out[k].size;

		if (ke > ko && best_base < ke && best_base + best_size > ko) {
			print("phys window %x size %x would cover %x..%x (%s): refused\n", best_base, best_size, ko, ke - 1,
			    PLAT->keep_out[k].what);
			return 0;
		}
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	return 1;
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int starts_with(const char *s, const char *prefix);

static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
	static uint8_t map_buf[65536];
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	last_klog = NULL;               /* (the last run's kernel log is in memory this run reuses) */
	last_klog_len = 0;
	if (!parse_payload(&pi, 1) || pi.kernel == NULL) {
		print("no kernel in payload\n");
		return 0;
	}
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}
	const uint8_t *kernel = pi.kernel;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (pi.kernel_size < sizeof(*mh) || mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}
	/* the load commands must lie inside the kernel entry, or the walk below reads whatever follows it */
	if (mh->sizeofcmds > pi.kernel_size - sizeof(*mh)) {
		print("kernel: the load commands are longer than the kernel\n");
		return 0;
	}

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		/*
		 * Every command must fit in the commands the header counts, and be at least its own header long: a
		 * cmdsize of 0 would keep the walk on one command for up to 2^32 rounds, a huge one would leave the kernel.
		 */
		uint64_t at = (uint64_t)((const uint8_t *)lc - (const uint8_t *)(mh + 1));

		if (at > mh->sizeofcmds || sizeof(*lc) > mh->sizeofcmds - at || lc->cmdsize < sizeof(*lc) ||
		    lc->cmdsize > mh->sizeofcmds - at) {
			print("kernel: load command %d does not fit in the load commands\n", (uint64_t)i);
			return 0;
		}
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (lc->cmdsize < sizeof(*sg)) {
				print("kernel: segment command %d is too short\n", (uint64_t)i);
				return 0;
			}
			if (sg->vmsize == 0) {
				continue;
			}
			/*
			 * What is loaded must be inside the kernel (file) and inside its own extent: a vmaddr + vmsize that
			 * wraps, a filesize above vmsize, or a file range past the entry would write over, or read from, memory
			 * that is not the kernel's.
			 */
			if (sg->vmaddr + sg->vmsize < sg->vmaddr || sg->filesize > sg->vmsize || sg->fileoff > pi.kernel_size ||
			    sg->filesize > pi.kernel_size - sg->fileoff) {
				print("kernel: segment %d is outside the kernel or wraps\n", (uint64_t)i);
				return 0;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD && lc->cmdsize >= 16 + 33 * 8) {
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc (a command too short to hold pc gives no entry) */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	/* (a kernel that reaches more than the largest window past the virtual base is no kernel of this loader's) */
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0 || vm_hi < vm_lo ||
	    vm_hi - KERNEL_VIRT_BASE > MAX_MEM_SIZE) {
		print("unexpected kernel layout\n");
		return 0;
	}
	if (!choose_window(&pi)) {
		return 0;
	}

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;

	if (pi.resident_rd != 0) {
		/*
		 * The size is the payload's word: one that does not even fit above the resident address would wrap
		 * the page rounding or the device tree address below, and the room check after them would pass.
		 * (Whatever fits is still held to the room check as before.)
		 */
		if (win_size <= RESIDENT_RD_OFFSET || pi.resident_rd > win_size - RESIDENT_RD_OFFSET) {
			print("rdres %x does not fit in the memory window\n", pi.resident_rd);
			return 0;
		}
		resident_rd_size = pi.resident_rd;
	}
	/*
	 * The payload's own resident image (a boot from the phone's boot partition, where no host sends one): put where
	 * "oem ramdisk" puts it, once per run, and taken as if it had been sent.
	 */
	if (pi.resident != NULL) {
		if (win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE ||
		    pi.resident_size > win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE) {
			print("resident %x does not fit in the memory window\n", (uint64_t)pi.resident_size);
			return 0;
		}
		memcpy((void *)(phys_base + RESIDENT_RD_OFFSET), pi.resident, pi.resident_size);
		resident_rd_size = pi.resident_size;
		print("resident image from the payload: %x bytes\n", (uint64_t)pi.resident_size);
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
	}
	if (rd1_size != 0) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}
	/* the whole layout in two lines, so that one run's log proves where everything went */
	print("layout: payload %x..%x%s, kernel %x..%x, ramdisk %x..%x, device tree %x..%x, boot_args %x, panic log %x\n",
	    payload_base, payload_base + PAYLOAD_MAX_SIZE - 1, payload_base != PAYLOAD_BASE ? " (moved)" : "", kern_lo,
	    kern_hi - 1, rd_pa, rd_pa + rd_size - 1, dt_pa, dt_pa + dt_max - 1, ba_pa, PANIC_LOG_BASE);
	print("layout: XNU's RAM %x..%x (memSize %x); kept from XNU and UEFI:", win_base, win_base + win_size - 1, win_size);
	for (int k = 0; k < PLATFORM_KEEP_OUT_MAX; k++) {
		if (PLAT->keep_out[k].size != 0) {
			print(" %x..%x (%s)", PLAT->keep_out[k].base, PLAT->keep_out[k].base + PLAT->keep_out[k].size - 1,
			    PLAT->keep_out[k].what);
		}
	}
	print("%s\n", PLAT->keep_out[0].size != 0 ? "" : " nothing");

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0, battery_requested = 0, ufs_requested = 0;
	/* "torch=N" (not 0) there too: the phone's flashlight is the kernel's (drivers/camera/hakios.torch.c) */
	int torch_requested = 0;
	/* "cam=5" there too: the phone's wide camera is Darwin's (drivers/camera/hakios.camera.c) */
	int camera_requested = 0;
	/* "unpark=1": core 1 is offered to the kernel although S-Boot is believed to park it (guessed from the S9 on the A50) */
	int unpark_core1 = 0;
	/* (the loader stays for run after run: what the last run's arguments said must not hold for this one) */
	gamut_p3 = 0;
	/* a run that does not ask for writes does not get them, whatever an earlier run of this
	 * loader asked for (the loader stays resident across runs) */
	block_write_requested = 0;
	block_write_part = -1;
	block_write_first = BLOCK_WRITE_FIRST;
	/*
	 * the same for the other storage requests (they are only ever OR-ed in below): without this a run
	 * that did not ask for ufs=2 is still offered a stale page and given its ufs_shared_pa
	 */
	hc_requested = 0;
	block_probe_requested = 0;
	block_enabled = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			int want = c[8] - '0';

			/* a core the boot loader keeps (parked_cores) never comes when asked: the next one instead */
			while (want < PLAT->cores && ((PLAT->parked_cores >> want) & 1)) {
				print("bootcpu=%d: that core is the boot loader's (parked); core %d instead\n", want, want + 1);
				want++;
			}
			if (want < PLAT->cores) {
				kernel_mpidr = core_mpidr(PLAT, want);
			}
		}
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
		battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
		unpark_core1 |= starts_with(c, "unpark=1") && (c == pi.cmdline || c[-1] == ' ');
		camera_requested |= starts_with(c, "cam=5") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		gamut_p3 |= starts_with(c, "gamut=p3") && (c == pi.cmdline || c[-1] == ' ');
		/*
		 * ufs=3: served as ufs=2. The kernel reads its references through this service, then its own driver takes
		 * the controller (drivers/storage/hakios.ufs.c, ufs_nufs_start); while the page says its list is in, nothing
		 * is asked of UEFI's driver here (block_poll).
		 */
		if (starts_with(c, "ufs=") && (c == pi.cmdline || c[-1] == ' ') && c[4] >= '1' && c[4] <= '3' &&
		    (c[5] == 0 || c[5] == ' ')) {
			ufs_requested = c[4] - '0';
		}
		if (starts_with(c, "recovery=0") && (c == pi.cmdline || c[-1] == ' ') && (c[10] == 0 || c[10] == ' ')) {
			reset_to_recovery = 0;
		}
		block_probe_requested |= starts_with(c, "ufs_probe=1") && (c == pi.cmdline || c[-1] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
		/* 1-3: the kernel's list beside UEFI's, in this loader's area; 4 and 5 are the kernel's alone (it has the
		 * controller for the run, and this loader serves no storage then: drivers/storage/hakios.ufshc.c, own mode) */
		hc_requested |= starts_with(c, "ufs_native=") && (c == pi.cmdline || c[-1] == ' ') && c[11] >= '1' &&
		    c[11] <= '3';
	}
	if (hc_requested && HC_PA == 0) {
		print("ufs: ufs_native= refused: platform %s has no UFS host controller for the kernel's own driver\n", PLAT->name);
		hc_requested = 0;
	}
	if (battery_requested) {
		charger_start();
	}
	if (ufs_requested >= 2) {
		hc_init();              /* (ufs_native=: UEFI's transfer list kept, before anything reads) */
	}
	if (ufs_requested == 3) {
		print("ufs: ufs=3: the block service as ufs=2, for the kernel's own driver to check itself against and to fall "
		    "back to; nothing asked of UEFI's driver while the kernel's list is in\n");
	}
	if (ufs_requested >= 1) {
		ufs_survey();
	}
	if (ufs_requested >= 2) {
		block_start();
	}
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		ba->video.v_baseAddr = SCREEN.base;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
	}
	const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (wdt_armed && !exynos_wdt_owns()) {       /* (the Exynos watchdog is fed by the loader's core, not the kernel) */
		for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (block_enabled) {
		static const char prefix[] = "ufs_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&block_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	if (battery_requested) {
		static const char prefix[] = "battery_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&charger_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	/* the version of this kernel, for sysctl kern.hakios.version and the kernel's log (no spaces in it) */
	if (pi.kernel_version != NULL && pi.kernel_version[0] != 0) {
		static const char prefix[] = "hakios_kver=";
		uint32_t before = n, need = (uint32_t)(sizeof(prefix) - 1 + str_len(pi.kernel_version) + 1);

		if (n + need < BOOT_LINE_LENGTH - 1) {
			for (const char *c = prefix; *c; c++) {
				ba->commandLine[n++] = *c;
			}
			for (const char *c = pi.kernel_version; *c && *c != ' '; c++) {
				ba->commandLine[n++] = *c;
			}
			ba->commandLine[n++] = ' ';
		} else {
			print("WARNING: hakios_kver= does not fit in the kernel command line (%d of %d used)\n", (uint64_t)before,
			    (uint64_t)(BOOT_LINE_LENGTH - 1));
		}
	}
	/*
	 * The payload's own command line, last. The kernel's boot_args holds BOOT_LINE_LENGTH characters and the
	 * copy used to stop at the end without a word: a long line lost its tail (the last argument arrived
	 * shortened and the kernel took it as that). Now it is said, with how much was lost.
	 */
	const char *rest = pi.cmdline;
	for (; *rest && n < BOOT_LINE_LENGTH - 1; rest++) {
		ba->commandLine[n++] = *rest;
	}
	if (*rest != 0) {
		print("WARNING: the kernel command line is TRUNCATED: %d characters of the payload's cmdline did not fit in the "
		    "%d the kernel has; the last argument is cut\n", (uint64_t)str_len(rest), (uint64_t)(BOOT_LINE_LENGTH - 1));
	}
	print("cmdline: %d of %d characters\n", (uint64_t)n, (uint64_t)(BOOT_LINE_LENGTH - 1));

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .end = (uint8_t *)(dt_pa + dt_max), .depth = 0 };
	uint64_t cntfrq = timer_hz();
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): Darwin user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	dt_prop_padded(&w, "serial-number", "RDW00POCOX5P", 32);
	dt_prop_padded(&w, "mlb-serial-number", "RDW00POCOX5PMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk.
	 */
	if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	/*
	 * and its haptics: "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics) makes Darwin play
	 * its vibrations and feedbacks as waveforms on the "Actuator" audio device (the audio plug-in
	 * hands them to the kernel's motor driver); without it Darwin looks for an IOKit vibration motor
	 * that the iPhone X does not have, and nothing vibrates.
	 */
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	/*
	 * and of its camera what is here: with torch=N the flash (MobileGestalt's camera-flash; without
	 * it Control Center has no flashlight); with cam=5 the node itself, for a camera there is (with it
	 * MobileGestalt's video-camera). The rest of the iPhone's camera is not here, nor said to be.
	 */
	if (torch_requested || camera_requested) {
		dt_begin(&w, "camera");
		if (camera_requested) {
			dt_identity(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data);
		} else {
			dt_identity_one(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data, "flash");
		}
		dt_end(&w);
	}
	dt_end(&w);

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/* the PCI host bridge, if there is one (virtio-pci devices under QEMU or crosvm) */
	dt_pci_node(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = core_number(plat, kernel_mpidr);

	if (boot_core < 0) {
		boot_core = 0;          /* (a boot core the platform does not number: the others counted from 0) */
	}

	if (more_cores) {
		/*
		 * The cores of the boot core's own kind first (both small or both not), then the others: the kernel takes the
		 * first cpus=N, and on the Galaxy S9 a fourth core by nearness alone was an Exynos M3, an ARMv8.0 core under a
		 * kernel that had said ARMv8.2 from the A55 it boots on.
		 */
		int boot_small = (int)((plat->small_cores >> boot_core) & 1);

		for (int pass = 0; pass < 2; pass++)
		for (int distance = 1; distance < plat->cores; distance++) {
			for (int side = 0; side < 2; side++) {
				int core = side == 0 ? boot_core + distance : boot_core - distance;
				uint64_t mpidr;
				int64_t state;

				if (core < 0 || core >= plat->cores || (mpidr = core_mpidr(plat, core)) == this_mpidr) {
					continue;
				}
				if (((int)((plat->small_cores >> core) & 1) == boot_small) != (pass == 0)) {
					continue;
				}
				if (((plat->parked_cores >> core) & 1) && !(unpark_core1 && core == 1)) {
					print("core %x is the boot loader's (parked): not the kernel's\n", mpidr);
					continue;
				}
				/* (a 32-bit answer: the A50's PSCI 0.1 gives NOT_SUPPORTED as 0xffffffff in x0, not -1) */
				state = (int32_t)psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED || (state == 0 && plat->affinity_unreliable)) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (core_number(plat, cpu_mpidr[i]) >= 0 && (plat->small_cores & (1u << core_number(plat, cpu_mpidr[i])))) {
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	/*
	 * With cam=5, where MobileGestalt looks for the cameras (IODeviceTree:/arm-io/isp, its still-camera and
	 * rear-facing-camera, by which SpringBoard shows Camera.app): the iPhone's ISP node, of its
	 * properties only the camera at the back. Neither a compatible nor a device_type: no driver matches it.
	 */
	if (camera_requested || torch_requested) {
		dt_begin(&w, "isp");
		dt_prop_u32(&w, "camera-rear", 1);
		dt_end(&w);
	}
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	if (plat->gic_cpu != 0) {
		/* GICv2: the distributor, then the CPU interface (osfmk/arm64/gic_v3_irq.c tells the two apart by this) */
		dt_prop_str(&w, "compatible", "arm,gic-400");
		uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x1000, plat->gic_cpu - plat->io_base, 0x2000 };
		dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	} else {
		dt_prop_str(&w, "compatible", "arm,gic-v3");
		uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base,
		    plat->gic_redist_size != 0 ? plat->gic_redist_size : 0x100000 };
		dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	}
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	/* a tree that did not fit in the room before boot_args was cut short by dt_prop: not handed to the kernel */
	if (w.overflow) {
		print("device tree does not fit in %x bytes: not started\n", dt_max);
		return 0;
	}
	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
	print("cmdline: %s\n", ba->commandLine);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	print("jumping to %x\n", entry_va + slide);
	flush_log();
	screen_quiet_before_run = screen_quiet;
	screen_quiet = 1;               /* the screen is the kernel's now; the log still takes every line */
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	if (loader_save_ctx() == 0) {
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = sizeof(map_buf);
		if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
			}
		}
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
		if (checkpoint == CHECKPOINT_SELFTEST) {
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			screen_quiet = screen_quiet_before_run;
			wdt_set_ms(WDT_BITE_MS);
			if (status != 0) {
				/* a kernel that hung may have left its own transfer list in the controller */
				hc_take_back(status == OTHER_CPU_HUNG ? "the kernel hung" : "the kernel did not start");
			}
			if (status == OTHER_CPU_HUNG) {
				/* the kernel still has that core; what it logged can be had all the same */
				print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
				    (uint64_t)SILENCE_SECONDS);
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				return 1;
			}
		} else {
			exynos_wdt_pause("the kernel runs on this core, and nothing would feed it");
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

	/* --- the kernel came back ------------------------------------------- */
	screen_quiet = screen_quiet_before_run;
	wdt_set_ms(WDT_BITE_MS);
	hc_take_back("the kernel came back");   /* before anything here touches the storage */
	if (block_enabled) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		if (block_shared.kernel_native.list_in != 0) {
			/* (ufs=3) the kernel did not say UEFI's list is back: the storage is not used again until a restart */
			print("ufs: the kernel left word that its own transfer list is in: the block service stays refused\n");
		}
	}
	if (hc_known) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		print("ufs: the kernel put its own list in %d times and took it out %d times\n",
		    (uint64_t)block_shared.kernel_native.switched_in, (uint64_t)block_shared.kernel_native.switched_out);
	}
	if (block_write_requested) {
		print("ufs: %d writes into %s, %d bytes, read back the same; %d refused\n", block_writes,
		    PLAT->data_part != NULL ? PLAT->data_part : "darwin", block_write_bytes, block_writes_refused);
	}
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	for (int i = 1; i < cpu_count; i++) {
		int64_t state = (int32_t)psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);

		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	sound_data = NULL;
	sound_len = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && kernel_live->sound != 0 &&
	    checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *sound = (const uint64_t *)(kernel_live->sound + slide);

		if (sound[0] != 0 && sound[1] > 24 && sound[1] <= (1ULL << 30)) {
			sound_data = (const char *)sound[0];
			sound_len = sound[1];
			print("the kernel left a sound recording: %d bytes at %x\n", sound_len, sound[0]);
		}
	}
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: log at %x, %d bytes\n", (uint64_t)(uintptr_t)klog, klog_len);
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static int
usb_start(void)
{
	static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	static usb_device_descriptor_set set;
	void *event;

	/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
	if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
	    &init_guid, &event))) {
		BS->signal_event(event);
		BS->close_event(event);
	}
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		return 0;
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
}

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
/*
 * Seconds a download may go without a byte before it is given up (0: wait for ever). A host that is stopped half-way
 * through a download (a fastboot killed by its timeout) left the loader waiting for the rest for good: every command
 * after it was taken for data, and only the power button brought the phone back.
 */
static uint64_t usb_data_timeout;

static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = timer_hz(), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (run_active && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (usb_data_timeout != 0 && READ_SYSREG(CNTVCT_EL0) - began > usb_data_timeout * freq) {
			USB->abort_xfer(endpoint);
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
static void
fb_command(char *cmd)
{
	/* a kernel runs on another core, with a live block or without one (running_live alone is NULL without) */
	int busy = run_active;

	{
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem sound") || starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy && running_live != NULL) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "0.5");
			} else if (str_eq(var, "max-download-size")) {
				fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : "0x03f00000");
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row */
				uint32_t mark = log_len;
				print("%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, (uint64_t)SCREEN.stride);
				log_buf[log_len] = 0;
				fb_reply("OKAY", log_buf + mark);
				log_len = mark;
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				/* (an "oem ramdisk" before this one must not leave the next download aimed at the resident area) */
				download_to_ramdisk = 0;
				fb_reply("OKAY", "");
				return;
			}
			parse_payload(&pi, 0);
			log_len = 0;
			if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			uint64_t total = parse_number(cmd + 9, 16), got = 0;
			uint64_t limit = PAYLOAD_MAX_SIZE - 0x100000;
			uint8_t *dest = (uint8_t *)payload_base;
			int to_ramdisk = download_to_ramdisk;

			download_to_ramdisk = 0;
			if (to_ramdisk) {
				limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
				/* the answer to "download:" carries the size in 32 bits: a larger one would be answered as another size */
				if (limit > 0xffffffffULL) {
					limit = 0xffffffffULL;
				}
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
				resident_rd_size = 0;
			}
			if (total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");
				return;
			}
			/*
			 * A download over the payload makes the payload no payload until it is whole: the XNUP magic
			 * is taken out when it arrives and put back at the end, so that a transfer that stops half-way
			 * leaves something "oem run" refuses, not a mix of the new and the old.
			 */
			if (!to_ramdisk) {
				*(volatile uint32_t *)dest = 0;
			}
			uint32_t magic_held = 0;
			int magic_taken = 0;
			fb_reply_data(total);
			usb_data_timeout = 20;
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				/* (a controller that reports more than was asked for must not carry the copy past the end) */
				if (r > (int64_t)want) {
					r = (int64_t)want;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				if (!to_ramdisk && !magic_taken && got + (uint64_t)r >= 4) {
					magic_held = *(volatile uint32_t *)dest;
					*(volatile uint32_t *)dest = 0;
					magic_taken = 1;
				}
				got += (uint64_t)r;
			}
			usb_data_timeout = 0;
			if (got == total) {
				if (magic_taken) {
					*(volatile uint32_t *)dest = magic_held;
				}
				if (to_ramdisk) {
					resident_rd_size = total;
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem sound")) {
			/* stage what the kernel recorded of the sound (boot-arg rec_sound) */
			if (sound_data == NULL) {
				fb_reply("FAIL", "no sound recording");
				return;
			}
			staged_data = sound_data;
			staged_len = sound_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || (*arg >= '0' && *arg <= '9');
			uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);

			set_result("running", cp, 0);
			fb_reply("OKAY", "");
			log_len = 0;
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem log")) {
			if (busy && running_live != NULL) {
				compose_live_log(running_live, running_slide);
			} else {
				compose_logs(NULL, 0);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
			fb_reply("OKAY", "");
			BS->stall(200000);
			platform_restart();
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL) {
		return;         /* this run was not started over USB */
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
	}
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	for (;;) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what Darwin expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	/*
	 * volatile: QEMU writes `control' back (to 0) when it has done the request, behind the compiler's back. Without the
	 * qualifier a build is free to keep the value written a few lines below and never look again, and the loader then
	 * says the request failed, leaves ramfb at the firmware's 800x600 and Darwin draws its 1125x2436 into a corner: a black window
	 * (it did, with a newer build of this very file; the older one happened to reload).
	 */
	static volatile struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * PHONE_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || EFI_ERROR(BS->allocate_pages(AllocateAnyPages, EfiLoaderData, (bytes + 0xFFF) >> 12, &fb))) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(PHONE_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	__asm__ volatile ("dsb sy" ::: "memory");       /* (cfg and dma are in memory before the device is told to read them) */
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	/* the device does it at once; allow it some time all the same instead of reading once */
	for (uint32_t spin = 0; dma.control != 0 && spin < 1000000; spin++) {
		__asm__ volatile ("" ::: "memory");
	}
	if (dma.control != 0) {
		print("ramfb: control %x after the request\n", (uint64_t)be32(dma.control));
		return;
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = PHONE_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

/*
 * UEFI-native boot: a payload from the volume the loader was started from.
 *
 * On the phone the payload (the kernel, the small root image, the command line) is put in memory at PAYLOAD_BASE
 * before the loader runs: by the boot shim behind Project Silicium's image (fastboot boot), or by QEMU's loader
 * device. This is the other way, the one every UEFI boot loader has: when there is no payload in memory, look for
 * \EFI\hakios\payload.bin on the volume this image was loaded from (the EFI system partition, where
 * \EFI\BOOT\BOOTAA64.EFI is the loader itself), read it to PAYLOAD_BASE and go on as if it had been there all along.
 * That is what lets the kernel start from a boot menu, a USB stick or an SD card, without fastboot.
 *
 * It prints nothing (the banner has not been drawn yet): volume_payload_bytes and volume_payload_note say what
 * happened, and efi_main says it under the banner. The file has to fit in the payload region (less the last MiB,
 * which holds the panic log), and the region has to be free: where a firmware keeps it reserved, the allocation is
 * refused and the loader goes on without a payload, as it always did.
 * Only the kernel and a root image that fits in the payload come this way; the big Darwin system image does not
 * (docs/uefi-boot.md says what does).
 */
static uint64_t volume_payload_bytes;           /* 0: nothing was read; else the payload's size */
static const char *volume_payload_note;         /* why not, if it was looked for */

static int
load_payload_from_volume(void)
{
	static const efi_guid image_guid = EFI_LOADED_IMAGE_GUID, fs_guid = EFI_SIMPLE_FILE_SYSTEM_GUID,
	    info_guid = EFI_FILE_INFO_GUID;
	static const char16 path[] = u"\\EFI\\hakios\\payload.bin";
	efi_loaded_image *image = NULL;
	efi_simple_fs *fs = NULL;
	efi_file *root = NULL, *file = NULL;
	uint8_t info[512] __attribute__((aligned(8)));
	uint64_t info_size = sizeof(info), size, done = 0, pages;
	uint64_t address = PAYLOAD_BASE;

	if (EFI_ERROR(BS->handle_protocol(IMAGE, &image_guid, (void **)&image)) || image == NULL || image->device == NULL ||
	    EFI_ERROR(BS->handle_protocol(image->device, &fs_guid, (void **)&fs)) || fs == NULL) {
		volume_payload_note = "the loader was not started from a volume with a file system";
		return 0;
	}
	if (EFI_ERROR(fs->open_volume(fs, &root)) || root == NULL) {
		volume_payload_note = "the volume cannot be opened";
		return 0;
	}
	if (EFI_ERROR(root->open(root, &file, path, EFI_FILE_MODE_READ, 0)) || file == NULL) {
		volume_payload_note = "there is no \\EFI\\hakios\\payload.bin on the volume";
		root->close(root);
		return 0;
	}
	if (EFI_ERROR(file->get_info(file, &info_guid, &info_size, info))) {
		volume_payload_note = "the size of \\EFI\\hakios\\payload.bin cannot be read";
		goto close;
	}
	size = *(uint64_t *)(info + EFI_FILE_INFO_FILE_SIZE_OFFSET);
	if (size < sizeof(struct payload_header) || size > PAYLOAD_MAX_SIZE - 0x100000ULL) {
		volume_payload_note = "\\EFI\\hakios\\payload.bin is empty or does not fit in the payload region (63 MiB)";
		goto close;
	}
	/*
	 * Take the whole payload region, not only the pages the file fills: the rest of it is written later (a payload
	 * sent over USB with `download:` goes up to the 63 MiB limit, and the panic log lives in the last MiB), and
	 * memory the firmware has not given to us must not be written.
	 */
	pages = PAYLOAD_MAX_SIZE / 0x1000;
	if (EFI_ERROR(BS->allocate_pages(2 /* AllocateAddress */, 2 /* EfiLoaderData */, pages, &address))) {
		volume_payload_note = "the payload region is not free (the firmware keeps it reserved)";
		goto close;
	}
	while (done < size) {
		uint64_t chunk = size - done < 0x100000ULL ? size - done : 0x100000ULL;       /* 1 MiB at a time */

		if (EFI_ERROR(file->read(file, &chunk, (void *)(uintptr_t)(PAYLOAD_BASE + done))) || chunk == 0) {
			volume_payload_note = "\\EFI\\hakios\\payload.bin could not be read to its end";
			/* what was read stays in memory after the pages are given back: without this, the first MiB's XNUP
			 * header would still be there and a half-read payload would be taken for a whole one */
			((volatile struct payload_header *)PAYLOAD_BASE)->magic = 0;
			BS->free_pages(PAYLOAD_BASE, pages);
			goto close;
		}
		done += chunk;
	}
	if (((const struct payload_header *)PAYLOAD_BASE)->magic != PAYLOAD_MAGIC) {
		volume_payload_note = "\\EFI\\hakios\\payload.bin is not a payload (no XNUP at its start)";
		BS->free_pages(PAYLOAD_BASE, pages);
		goto close;
	}
	volume_payload_bytes = size;
close:
	file->close(file);
	root->close(root);
	return volume_payload_bytes != 0;
}

/*
 * The platform's keep-outs (struct platform keep_out) taken from UEFI's allocator: every free piece of [lo, hi) becomes
 * EfiReservedMemoryType, so that nothing UEFI hands out later (pool, the loader's own pages, a driver's buffer) lands
 * there while it stays alive beside the kernel, and choose_window never sees it as free. Pieces the firmware already has
 * are said, not taken.
 */
static void
keep_out_reserve(uint64_t lo, uint64_t hi, const char *what)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, taken = 0, held = 0, failed = 0;
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("keep-out %x..%x (%s): GetMemoryMap failed, NOT reserved\n", lo, hi - 1, what);
		return;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		uint64_t a = d->phys_start, b = d->phys_start + (d->pages << 12);

		if (b <= lo || a >= hi) {
			continue;
		}
		a = a < lo ? lo : a;
		b = b > hi ? hi : b;
		if (d->type == EfiConventionalMemory) {
			uint64_t at = a;

			if (EFI_ERROR(BS->allocate_pages(AllocateAddress, 0 /* EfiReservedMemoryType */, (b - a) >> 12, &at))) {
				failed += b - a;
				print("  keep-out %x..%x free but NOT taken\n", a, b - 1);
			} else {
				taken += b - a;
			}
		} else {
			held += b - a;
			print("  keep-out %x..%x is the firmware's already: type %d\n", a, b - 1, (uint64_t)d->type);
		}
	}
	print("keep-out %x..%x (%s): %d KB reserved now, %d KB the firmware's already, %d KB not taken\n", lo, hi - 1, what,
	    taken >> 10, held >> 10, failed >> 10);
}

/*
 * A platform whose payload landing place is memory that is not ours to keep (the Galaxy S9: the CP's region, struct
 * platform payload_home) has it moved out at the start, before anything else reads it: a 64 MiB block at payload_home
 * (or a step of 64 MiB above, up to three) is taken from UEFI, the payload copied there and checked, and the copy at
 * PAYLOAD_BASE made no payload (its magic cleared). If no block can be had, or the copy does not read back the same,
 * the payload stays where it landed and the run goes on as before the move: a run is never lost to it.
 * Keep-outs are reserved first, so a fallback payload placement cannot use the
 * display's scanout pool or another peripheral's reserved memory.
 */
static void
payload_move(void)
{
	const struct payload_header *ph = (const struct payload_header *)PAYLOAD_BASE;
	uint64_t home = 0, bytes;
	for (int k = 0; k < PLATFORM_KEEP_OUT_MAX; k++) {
		if (PLAT->keep_out[k].size != 0) {
			keep_out_reserve(PLAT->keep_out[k].base, PLAT->keep_out[k].base + PLAT->keep_out[k].size,
			    PLAT->keep_out[k].what);
		}
	}

	if (PLAT->payload_home != 0 && ph->magic == PAYLOAD_MAGIC) {
		bytes = (sizeof(*ph) + payload_walk_size(ph->size) + 15) & ~15ULL;
		for (int i = 0; i < 4 && home == 0; i++) {
			uint64_t at = PLAT->payload_home + (uint64_t)i * PAYLOAD_MAX_SIZE;

			if (!EFI_ERROR(BS->allocate_pages(AllocateAddress, EfiLoaderData, PAYLOAD_MAX_SIZE >> 12, &at))) {
				home = at;
			} else {
				print("payload: %x..%x is not free\n", at, at + PAYLOAD_MAX_SIZE - 1);
			}
		}
		if (home == 0) {
			print("payload: NOT moved (no 64 MiB free from %x): it stays at %x, in memory the platform keeps out\n",
			    PLAT->payload_home, PAYLOAD_BASE);
		} else {
			memcpy((void *)(uintptr_t)home, (const void *)(uintptr_t)PAYLOAD_BASE, bytes);
			if (memcmp((const void *)(uintptr_t)home, (const void *)(uintptr_t)PAYLOAD_BASE, bytes) != 0) {
				print("payload: the copy at %x does not read back the same: NOT moved, it stays at %x\n", home,
				    PAYLOAD_BASE);
				BS->free_pages(home, PAYLOAD_MAX_SIZE >> 12);
			} else {
				clean_dcache(home, home + bytes);
				payload_base = home;
				((volatile struct payload_header *)(uintptr_t)PAYLOAD_BASE)->magic = 0;
				clean_dcache(PAYLOAD_BASE, PAYLOAD_BASE + 64);
				print("payload: moved from %x to %x (%x bytes; its region %x..%x, the panic log at %x)\n", PAYLOAD_BASE,
				    home, bytes, home, home + PAYLOAD_MAX_SIZE - 1, PANIC_LOG_BASE);
			}
		}
	} else if (PLAT->payload_home != 0) {
		print("payload: none at %x to move (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
	}
}

/*
 * The version of the kernel in the payload (the entry kernel.ver, made by xnuloader/mkpayload.py from
 * tools/kernel-version.py), or NULL if the payload has none. A look at the payload that changes
 * nothing and prints nothing, so that the banner can use it before the rest of the loader runs.
 */
static const char *
payload_kernel_version(void)
{
	const struct payload_header *ph = (const struct payload_header *)payload_base;

	if (ph->magic != PAYLOAD_MAGIC) {
		return NULL;
	}
	/* (the same limits as parse_payload: a header size or an entry size that reaches past the payload ends the walk) */
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + payload_walk_size(ph->size);
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;

		if (e->name[0] == 0) {
			break;
		}
		if ((uint64_t)e->size > (uint64_t)(pend - (p + sizeof(*e)))) {
			break;
		}
		if (str_eq(e->name, "kernel.ver")) {
			return (const char *)(p + sizeof(*e));
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return NULL;
}

/*
 * The greeting. Printed from efi_main, while the UEFI text console is still ours (on the phone that is
 * Project Silicium's UEFI): the USB command loop that follows sets screen_quiet and leaves the screen
 * alone, so what is on it now stays there. It also goes into the log, which "xl log" fetches.
 * The two lines under the letters are the ones the project's banner has: a greeting from the kernel,
 * and its version, kernel.ver=, as tools/kernel-version.py makes it.
 */
static void
print_banner(void)
{
	const char *version = payload_kernel_version();

	print("\n");
	print("   #   #   ###   #   #  #####   ###    ####\n");
	print("   #   #  #   #  #  #     #    #   #  #\n");
	print("   #####  #####  ###      #    #   #   ###\n");
	print("   #   #  #   #  #  #     #    #   #      #\n");
	print("   #   #  #   #  #   #  #####   ###   ####\n");
	print("\n");
	print("   hello hakios-kernel!\n");
	print("   kernel.ver=%s\n", version ? version : "unknown");
	print("\n");
}

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	/* no payload in memory (nobody put one there): the volume this loader came from may have one, as a UEFI boot loader's does */
	if (((const struct payload_header *)PAYLOAD_BASE)->magic != PAYLOAD_MAGIC) {
		load_payload_from_volume();
	}
	print("== XNULOADER v3 ==\n");
	print_banner();
	if (volume_payload_bytes != 0) {
		print("payload: %d bytes from \\EFI\\hakios\\payload.bin on the volume this loader was started from\n",
		    volume_payload_bytes);
	} else if (volume_payload_note != NULL) {
		print("payload: none in memory, and %s\n", volume_payload_note);
	}
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), READ_SYSREG(CNTFRQ_EL0) /* the register itself */, (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = platform_from_fdt(&platform_qemu_virt);  /* (the Pixel 10: by its device tree, before the framebuffer) */
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
			if (platforms[i]->fb_base == SCREEN.base)
				PLAT = platforms[i];
		if (PLAT == &platform_qemu_virt)
			ramfb_phone_screen();
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	if (PLAT == &platform_qemu_virt) {
		/*
		 * A GICv2: the FDT says where, when the firmware publishes one; else the processor says there is no
		 * GICv3 register interface (ID_AA64PFR0_EL1.GIC, bits 27:24, 0) and it is QEMU virt's, at its usual place.
		 */
		static const efi_guid dtb_guid = EFI_DTB_TABLE_GUID;
		uint64_t dist = 0, cpu = 0;

		for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables; i++) {
			if (guid_eq(&ST->tables[i].guid, &dtb_guid) && gic_v2_from_fdt(ST->tables[i].table, &dist, &cpu)) {
				break;
			}
		}
		if (cpu == 0 && ((READ_SYSREG(ID_AA64PFR0_EL1) >> 24) & 0xf) == 0) {
			dist = 0x08000000, cpu = 0x08010000;
		}
		if (cpu != 0) {
			platform_qemu_virt_gicv2 = platform_qemu_virt;
			platform_qemu_virt_gicv2.name = "qemu-virt-gicv2";
			platform_qemu_virt_gicv2.gic_dist = dist;
			platform_qemu_virt_gicv2.gic_redist = 0;
			platform_qemu_virt_gicv2.gic_cpu = cpu;
			PLAT = &platform_qemu_virt_gicv2;
		}
	}
	if (PLAT->gic_cpu == 0 && ((READ_SYSREG(ID_AA64PFR0_EL1) >> 24) & 0xf) == 0) {
		print("platform %s: a GICv3 by its description, but this processor has no GICv3 registers\n", PLAT->name);
	}
	loader_gic_sysregs = PLAT->gic_cpu == 0;
	ram_log_base = PLAT->ram_log, ram_log_size = PLAT->ram_log_size;
	ram_dump_base = PLAT->ram_dump, ram_dump_size = PLAT->ram_dump_size;
	clear_ram_dump();
	print("platform %s, %s\n", PLAT->name, PLAT->gic_cpu != 0 ? "GICv2" : "GICv3");
	pci_probe(PLAT);

	install_exception_hook();
	wdt_arm();

	int have_payload = parse_payload(&pi, 0);
	if (have_payload && platform_sibling(PLAT, pi.cmdline) != PLAT) {
		/* the S9+ or the Note9 (loader_platform=): the S9's description in all but the name, so nothing above changes */
		print("platform %s (loader_platform=), the description of %s with its own name\n",
		    platform_sibling(PLAT, pi.cmdline)->name, PLAT->name);
		PLAT = platform_sibling(PLAT, pi.cmdline);
	}
	exynos_wdt_start(have_payload ? pi.cmdline : "");   /* the Galaxy S9's hardware watchdog, and INFORM3 */
	/*
	 * The Galaxy S9: the payload out of the CP's region, which is then kept from UEFI and XNU. Here, under the watchdog
	 * just armed, so that a hang in it is a restart into recovery; and read again from where it is now.
	 */
	if (PLAT->payload_home != 0 || PLAT->keep_out[0].size != 0) {
		payload_move();
		have_payload = parse_payload(&pi, 0);
	}
	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/* One-shot mode: run the kernel that came with the image, then reset. */
	if (have_payload) {
		run_kernel(0, 0);
	}
	reboot();
	return 0;
}
