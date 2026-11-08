/*
 * What the boot shim needs from the boot loader's flattened device tree (the devicetree specification, v0.4, chapter 5):
 * the RAM, what of it is reserved, the command line, the root's compatible, a console UART and a framebuffer, if the tree
 * names them. One pass over the structure block; nothing is allocated. Built for the shim and, as it is, for the host
 * (fdt-test.c).
 */
#ifndef SHIM_FDT_H
#define SHIM_FDT_H

#include <stdint.h>

#define FDT_MAX_MEM     16
#define FDT_MAX_RSV     128

#define RSV_NOMAP       1       /* "no-map": not even to be mapped (a firewall may stand behind it) */
#define RSV_MEMRESERVE  2       /* from the /memreserve/ block */
#define RSV_FB          4       /* the boot loader's framebuffer (the shim's own reservation) */

enum { UART_NONE, UART_PL011, UART_16550 };

struct fdt_range {
	uint64_t base, size;
	uint32_t flags;
};

struct fdt_info {
	uint32_t total_size;
	struct fdt_range mem[FDT_MAX_MEM];
	int nmem;
	struct fdt_range rsv[FDT_MAX_RSV];
	int nrsv;
	int rsv_dropped;                /* more than FDT_MAX_RSV: the shim refuses to go on */
	const char *bootargs;
	const char *compatible;         /* the root's stringlist */
	uint32_t compatible_len;
	const char *model;
	int psci_smc;                   /* /psci's method is "smc" */
	struct {
		int kind;               /* UART_* */
		uint64_t base;
		uint32_t shift;         /* reg-shift (16550) */
	} uart;
	struct {
		uint64_t base, size;
		uint32_t width, height, stride;
		int ok;                 /* a "simple-framebuffer" in a8r8g8b8 or x8r8g8b8 */
	} fb;
};

uint32_t fdt_be32(const void *p);
int fdt_parse(const void *blob, struct fdt_info *out);         /* 0, or <0: not a device tree this understands */
int fdt_compat_has(const char *list, uint32_t len, const char *want);

#endif
