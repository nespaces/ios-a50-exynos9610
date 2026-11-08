/*
 * The shim's view of RAM (memmap.h). Every reservation is rounded out to 2 MB before it is taken from the free RAM, so
 * that a free 2 MB block can be mapped cacheable as a whole and no page the loader is given shares a block with a
 * region behind which a firewall may stand (the Tensor G5's tree reserves dozens, some of 4 KB). What that costs is a
 * few MB of the low 2 GB; XNU's window is in the high bank, which the tree leaves whole.
 */
#include "memmap.h"

static void
sort(struct fdt_range *r, int n)
{
	for (int i = 1; i < n; i++) {
		struct fdt_range t = r[i];
		int j = i - 1;
		while (j >= 0 && r[j].base > t.base) {
			r[j + 1] = r[j];
			j--;
		}
		r[j + 1] = t;
	}
}

static uint64_t
down(uint64_t v)
{
	return v & ~(BLOCK - 1);
}

static uint64_t
up(uint64_t v)
{
	return (v + BLOCK - 1) & ~(BLOCK - 1);
}

void
memmap_build(struct memmap *m, const struct fdt_info *fi, const struct fdt_range *extra, int nextra)
{
	m->nram = m->nrsv = m->nfree = 0;
	for (int i = 0; i < fi->nmem && m->nram < FDT_MAX_MEM; i++) {
		m->ram[m->nram++] = fi->mem[i];
	}
	sort(m->ram, m->nram);
	/* merge adjoining or overlapping banks */
	int k = 0;
	for (int i = 0; i < m->nram; i++) {
		if (k > 0 && m->ram[i].base <= m->ram[k - 1].base + m->ram[k - 1].size) {
			uint64_t end = m->ram[i].base + m->ram[i].size;
			if (end > m->ram[k - 1].base + m->ram[k - 1].size) {
				m->ram[k - 1].size = end - m->ram[k - 1].base;
			}
		} else {
			m->ram[k++] = m->ram[i];
		}
	}
	m->nram = k;

	for (int i = 0; i < fi->nrsv && m->nrsv < FDT_MAX_RSV + 16; i++) {
		m->rsv[m->nrsv++] = fi->rsv[i];
	}
	for (int i = 0; i < nextra && m->nrsv < FDT_MAX_RSV + 16; i++) {
		m->rsv[m->nrsv++] = extra[i];
	}
	sort(m->rsv, m->nrsv);

	/* free = RAM (in whole 2 MB blocks) less the reservations (rounded out to 2 MB) */
	for (int i = 0; i < m->nram; i++) {
		uint64_t at = up(m->ram[i].base), end = down(m->ram[i].base + m->ram[i].size);
		for (int j = 0; j < m->nrsv && at < end; j++) {
			uint64_t rb = down(m->rsv[j].base), re = up(m->rsv[j].base + m->rsv[j].size);
			if (re <= at || rb >= end) {
				continue;
			}
			if (rb > at && m->nfree < MEMMAP_MAX) {
				m->free[m->nfree].base = at;
				m->free[m->nfree].size = rb - at;
				m->free[m->nfree].flags = 0;
				m->nfree++;
			}
			if (re > at) {
				at = re;
			}
		}
		if (at < end && m->nfree < MEMMAP_MAX) {
			m->free[m->nfree].base = at;
			m->free[m->nfree].size = end - at;
			m->free[m->nfree].flags = 0;
			m->nfree++;
		}
	}
}

int
memmap_in_ram(const struct memmap *m, uint64_t base, uint64_t size)
{
	for (int i = 0; i < m->nram; i++) {
		if (base >= m->ram[i].base && base + size <= m->ram[i].base + m->ram[i].size) {
			return 1;
		}
	}
	return 0;
}

int
memmap_overlaps_reserved(const struct memmap *m, uint64_t base, uint64_t size)
{
	for (int i = 0; i < m->nrsv; i++) {
		if (base < m->rsv[i].base + m->rsv[i].size && m->rsv[i].base < base + size) {
			return 1;
		}
	}
	return 0;
}

int
memmap_block(const struct memmap *m, uint64_t base)
{
	/* the framebuffer first: written by the CPU and read by the display, not cached, whatever else is in its blocks */
	for (int i = 0; i < m->nrsv; i++) {
		if ((m->rsv[i].flags & RSV_FB) && base < m->rsv[i].base + m->rsv[i].size && m->rsv[i].base < base + BLOCK) {
			return BLOCK_FB;
		}
	}
	if (!memmap_in_ram(m, base, BLOCK)) {
		/* a block only partly RAM is mapped as a device's: never fetched ahead, never cached */
		for (int i = 0; i < m->nram; i++) {
			if (base < m->ram[i].base + m->ram[i].size && m->ram[i].base < base + BLOCK) {
				return BLOCK_NOMAP;
			}
		}
		return BLOCK_NONE;
	}
	for (int i = 0; i < m->nrsv; i++) {
		if ((m->rsv[i].flags & RSV_NOMAP) && base < m->rsv[i].base + m->rsv[i].size && m->rsv[i].base < base + BLOCK) {
			return BLOCK_NOMAP;
		}
	}
	return BLOCK_NORMAL;
}
