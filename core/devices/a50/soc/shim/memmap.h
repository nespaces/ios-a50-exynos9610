/*
 * The RAM the boot loader's device tree describes, less what is reserved, in the 2 MB blocks the shim maps it in: what
 * the shim offers as UEFI's conventional memory (xnuloader picks XNU's window from it), and how each 2 MB is mapped.
 * Host-testable (fdt-test.c).
 */
#ifndef SHIM_MEMMAP_H
#define SHIM_MEMMAP_H

#include "fdt.h"

#define BLOCK           0x200000ULL     /* 2 MB: a level-2 block of the shim's 4 KB-granule tables */
#define MEMMAP_MAX      (FDT_MAX_RSV + FDT_MAX_MEM + 16)

struct memmap {
	struct fdt_range ram[FDT_MAX_MEM];      /* sorted, adjoining ones merged */
	int nram;
	struct fdt_range rsv[FDT_MAX_RSV + 16]; /* sorted, as given (flags: RSV_*) */
	int nrsv;
	struct fdt_range free[MEMMAP_MAX];      /* RAM less every reservation rounded out to 2 MB, sorted */
	int nfree;
};

enum { BLOCK_NONE, BLOCK_NORMAL, BLOCK_NOMAP, BLOCK_FB };

/* extra: what the shim reserves besides the tree's (itself, the tree, the payload's window) */
void memmap_build(struct memmap *m, const struct fdt_info *fi, const struct fdt_range *extra, int nextra);
int memmap_block(const struct memmap *m, uint64_t base);            /* BLOCK_*, of the 2 MB block at base */
int memmap_overlaps_reserved(const struct memmap *m, uint64_t base, uint64_t size);
int memmap_in_ram(const struct memmap *m, uint64_t base, uint64_t size);

#endif
