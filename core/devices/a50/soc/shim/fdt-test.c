/*
 * Host test of the shim's device-tree reader and memory map (fdt.c, memmap.c): `make test`.
 *
 *   fdt-test FILE.dtb      prints what the shim would see, and checks it:
 *                          - every free range is in whole 2 MB blocks, in RAM, and meets no reservation;
 *                          - a 2 MB block with a no-map region in it is not mapped cacheable;
 *                          - 0xF0000000 + 64 MB, where the loader takes its payload, is RAM and reserved by nobody else.
 *
 * With tests/frankel-like.dtb it also checks what that tree says (the Tensor G5's reservations, scion.fb=, no UART).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdt.h"
#include "memmap.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: fdt-test FILE.dtb\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	static unsigned char blob[1 << 20];
	size_t n = fread(blob, 1, sizeof(blob), f);
	fclose(f);

	struct fdt_info fi;
	int r = fdt_parse(blob, &fi);
	printf("fdt_parse: %d, %zu bytes (header says %u)\n", r, n, fi.total_size);
	if (r != 0) {
		return 1;
	}
	printf("model %s; compatible %s; bootargs %s\n", fi.model ? fi.model : "-", fi.compatible ? fi.compatible : "-",
	    fi.bootargs ? fi.bootargs : "-");
	printf("uart: kind %d at %#llx shift %u; fb: %d at %#llx %ux%u stride %u\n", fi.uart.kind,
	    (unsigned long long)fi.uart.base, fi.uart.shift, fi.fb.ok, (unsigned long long)fi.fb.base, fi.fb.width,
	    fi.fb.height, fi.fb.stride);
	for (int i = 0; i < fi.nmem; i++) {
		printf("memory %#llx + %#llx\n", (unsigned long long)fi.mem[i].base, (unsigned long long)fi.mem[i].size);
	}
	for (int i = 0; i < fi.nrsv; i++) {
		printf("reserved %#llx + %#llx%s%s\n", (unsigned long long)fi.rsv[i].base, (unsigned long long)fi.rsv[i].size,
		    fi.rsv[i].flags & RSV_NOMAP ? " no-map" : "", fi.rsv[i].flags & RSV_MEMRESERVE ? " (memreserve)" : "");
	}

	/* the shim's own reservations, as on a phone that put it at 0x80000000 + 2 MB and the tree at 0x8a000000 */
	struct fdt_range extra[3] = {
		{ 0x80200000, 0x02000000, 0 },
		{ 0x8a000000, fi.total_size, 0 },
		{ 0xF0000000, 0x04000000, 0 },
	};
	static struct memmap mm;
	memmap_build(&mm, &fi, extra, 3);
	unsigned long long total = 0, biggest = 0;
	for (int i = 0; i < mm.nfree; i++) {
		struct fdt_range *fr = &mm.free[i];
		printf("free %#llx + %#llx (%llu MB)\n", (unsigned long long)fr->base, (unsigned long long)fr->size,
		    (unsigned long long)(fr->size >> 20));
		total += fr->size;
		if (fr->size > biggest) {
			biggest = fr->size;
		}
		CHECK((fr->base & (BLOCK - 1)) == 0 && (fr->size & (BLOCK - 1)) == 0 && fr->size != 0, "free %#llx + %#llx not whole 2 MB blocks",
		    (unsigned long long)fr->base, (unsigned long long)fr->size);
		CHECK(memmap_in_ram(&mm, fr->base, fr->size), "free %#llx not in RAM", (unsigned long long)fr->base);
		CHECK(!memmap_overlaps_reserved(&mm, fr->base, fr->size), "free %#llx + %#llx meets a reservation",
		    (unsigned long long)fr->base, (unsigned long long)fr->size);
		for (uint64_t b = fr->base; b < fr->base + fr->size; b += BLOCK) {
			CHECK(memmap_block(&mm, b) == BLOCK_NORMAL, "free block %#llx not mapped cacheable", (unsigned long long)b);
		}
		if (i > 0) {
			CHECK(fr->base >= mm.free[i - 1].base + mm.free[i - 1].size, "free ranges out of order or overlapping");
		}
	}
	printf("free: %llu MB in %d ranges, the largest %llu MB\n", total >> 20, mm.nfree, biggest >> 20);
	for (int i = 0; i < mm.nrsv; i++) {
		if (!(mm.rsv[i].flags & RSV_NOMAP)) {
			continue;
		}
		for (uint64_t b = mm.rsv[i].base & ~(BLOCK - 1); b < mm.rsv[i].base + mm.rsv[i].size; b += BLOCK) {
			CHECK(memmap_block(&mm, b) != BLOCK_NORMAL, "no-map %#llx's block %#llx mapped cacheable",
			    (unsigned long long)mm.rsv[i].base, (unsigned long long)b);
		}
	}
	CHECK(memmap_in_ram(&mm, 0xF0000000, 0x04000000), "0xF0000000 + 64 MB is not RAM");
	/* nobody but the shim's own payload reservation may claim the loader's payload window */
	for (int i = 0; i < fi.nrsv; i++) {
		CHECK(!(fi.rsv[i].base < 0xF4000000 && 0xF0000000 < fi.rsv[i].base + fi.rsv[i].size),
		    "the tree reserves %#llx + %#llx, inside the payload's window", (unsigned long long)fi.rsv[i].base,
		    (unsigned long long)fi.rsv[i].size);
	}

	if (fi.compatible && fdt_compat_has(fi.compatible, fi.compatible_len, "google,lga-frankel")) {
		CHECK(fi.nmem == 2, "two memory ranges, got %d", fi.nmem);
		CHECK(mm.nram == 2, "two RAM banks after merging, got %d", mm.nram);
		CHECK(fi.nrsv == 19, "19 reservations (18 nodes with reg, 1 memreserve), got %d", fi.nrsv);
		CHECK(fi.uart.kind == UART_NONE, "the disabled UART taken");
		CHECK(fi.bootargs && strstr(fi.bootargs, "scion.fb=0xbe400000"), "bootargs");
		CHECK(memmap_block(&mm, 0x8d200000) == BLOCK_NOMAP, "the AOC's region mapped cacheable");
		CHECK(memmap_block(&mm, 0x95200000) == BLOCK_NORMAL, "ramoops (mapped, not no-map) not cacheable");
		CHECK(memmap_block(&mm, 0x880000000ULL) == BLOCK_NORMAL, "the high bank not cacheable");
		CHECK(memmap_block(&mm, 0x05800000) == BLOCK_NONE, "the GIC's block taken for RAM");
		CHECK(biggest == 0x280000000ULL, "the whole high bank (10 GB) free for XNU, the largest is %#llx", biggest);
	}
	printf("%s\n", failures ? "FAILED" : "all checks passed");
	return failures != 0;
}
