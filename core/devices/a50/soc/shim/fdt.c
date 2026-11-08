/*
 * The boot loader's device tree, read once (fdt.h). The format is the devicetree specification's (v0.4, 5.1-5.4): a
 * header, the reservation block, a structure block of BEGIN_NODE/PROP/END_NODE tokens and a strings block.
 *
 * A node's "reg" is read with its parent's #address-cells/#size-cells (2 and 1 when the parent says nothing). A UART or
 * framebuffer below a bus is taken at the address its reg says: the buses of the Tensor G5's trees (soc@0) and QEMU's
 * map 1:1. A reserved-memory child is reserved whatever its status; one with no reg (a pool the kernel would place)
 * is nobody's here and is skipped.
 */
#include "fdt.h"

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9
#define MAX_DEPTH       16

struct node {
	const char *name;
	uint32_t addr_cells, size_cells;        /* what it declares for its children */
	const uint8_t *reg;
	uint32_t reg_len;
	const char *compat;
	uint32_t compat_len;
	const char *status;
	const char *device_type;
	const char *format;
	uint32_t width, height, stride, reg_shift;
	int nomap;
};

uint32_t
fdt_be32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint64_t
be_cells(const uint8_t *p, uint32_t cells)
{
	uint64_t v = 0;
	for (uint32_t i = 0; i < cells; i++) {
		v = v << 32 | fdt_be32(p + 4 * i);
	}
	return v;
}

static uint32_t
slen(const char *s)
{
	uint32_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
seq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/* `name` is `want` or `want@...` */
static int
named(const char *name, const char *want)
{
	while (*want && *name == *want) {
		name++, want++;
	}
	return *want == 0 && (*name == 0 || *name == '@');
}

int
fdt_compat_has(const char *list, uint32_t len, const char *want)
{
	for (uint32_t at = 0; list != 0 && at < len;) {
		if (seq(list + at, want)) {
			return 1;
		}
		at += slen(list + at) + 1;
	}
	return 0;
}

static int
okay(const struct node *n)
{
	return n->status == 0 || seq(n->status, "okay") || seq(n->status, "ok");
}

static void
add_range(struct fdt_range *r, int *count, int max, uint64_t base, uint64_t size, uint32_t flags, int *dropped)
{
	if (size == 0) {
		return;
	}
	if (*count >= max) {
		if (dropped) {
			(*dropped)++;
		}
		return;
	}
	r[*count].base = base;
	r[*count].size = size;
	r[*count].flags = flags;
	(*count)++;
}

/* each (address, size) of a node's reg, by its parent's cells */
static void
add_reg(const struct node *n, const struct node *parent, struct fdt_range *r, int *count, int max, uint32_t flags,
    int *dropped)
{
	uint32_t ac = parent->addr_cells, sc = parent->size_cells, one = 4 * (ac + sc);

	if (n->reg == 0 || ac > 2 || sc > 2 || one == 0) {
		return;
	}
	for (uint32_t at = 0; at + one <= n->reg_len; at += one) {
		add_range(r, count, max, be_cells(n->reg + at, ac), be_cells(n->reg + at + 4 * ac, sc), flags, dropped);
	}
}

static void
end_node(struct node *stack, int depth, struct fdt_info *out)
{
	struct node *n = &stack[depth];
	struct node *parent = depth > 0 ? &stack[depth - 1] : 0;

	if (depth == 0) {
		out->compatible = n->compat;
		out->compatible_len = n->compat_len;
		return;
	}
	if (depth == 1 && (named(n->name, "memory") || (n->device_type && seq(n->device_type, "memory"))) && okay(n)) {
		add_reg(n, parent, out->mem, &out->nmem, FDT_MAX_MEM, 0, 0);
	}
	if (depth == 2 && named(stack[1].name, "reserved-memory")) {
		add_reg(n, parent, out->rsv, &out->nrsv, FDT_MAX_RSV, n->nomap ? RSV_NOMAP : 0, &out->rsv_dropped);
	}
	if (fdt_compat_has(n->compat, n->compat_len, "simple-framebuffer") && okay(n) && !out->fb.ok && n->reg != 0 &&
	    n->format != 0 && (seq(n->format, "a8r8g8b8") || seq(n->format, "x8r8g8b8")) && n->width && n->height) {
		struct fdt_range r[1];
		int c = 0;
		add_reg(n, parent, r, &c, 1, 0, 0);
		if (c == 1) {
			out->fb.base = r[0].base;
			out->fb.size = r[0].size;
			out->fb.width = n->width;
			out->fb.height = n->height;
			out->fb.stride = n->stride ? n->stride : n->width * 4;
			out->fb.ok = 1;
		}
	}
	if (out->uart.kind == UART_NONE && okay(n) && n->reg != 0) {
		int kind = UART_NONE;
		if (fdt_compat_has(n->compat, n->compat_len, "arm,pl011")) {
			kind = UART_PL011;
		} else if (fdt_compat_has(n->compat, n->compat_len, "snps,dw-apb-uart") ||
		    fdt_compat_has(n->compat, n->compat_len, "google,lga-uart") ||
		    fdt_compat_has(n->compat, n->compat_len, "goog,goog-dw-apb-uart") ||
		    fdt_compat_has(n->compat, n->compat_len, "ns16550a")) {
			kind = UART_16550;
		}
		if (kind != UART_NONE) {
			struct fdt_range r[1];
			int c = 0;
			add_reg(n, parent, r, &c, 1, 0, 0);
			if (c == 1) {
				out->uart.kind = kind;
				out->uart.base = r[0].base;
				out->uart.shift = n->reg_shift;
			}
		}
	}
}

int
fdt_parse(const void *blob, struct fdt_info *out)
{
	const uint8_t *b = blob;
	struct node stack[MAX_DEPTH];
	int depth = -1;

	for (uint32_t i = 0; i < sizeof(*out); i++) {
		((uint8_t *)out)[i] = 0;
	}
	if (b == 0 || fdt_be32(b) != FDT_MAGIC || fdt_be32(b + 20) < 16) {
		return -1;
	}
	uint32_t total = fdt_be32(b + 4), off_st = fdt_be32(b + 8), off_str = fdt_be32(b + 12), off_rsv = fdt_be32(b + 16);
	uint32_t size_str = fdt_be32(b + 32), size_st = fdt_be32(b + 36);
	if (total < 40 || off_st > total || size_st > total - off_st || off_str > total || size_str > total - off_str ||
	    off_rsv > total) {
		return -2;
	}
	out->total_size = total;

	/* the reservation block: (address, size) pairs up to one of zeros */
	for (uint32_t at = off_rsv; at + 16 <= total; at += 16) {
		uint64_t base = be_cells(b + at, 2), size = be_cells(b + at + 8, 2);
		if (base == 0 && size == 0) {
			break;
		}
		add_range(out->rsv, &out->nrsv, FDT_MAX_RSV, base, size, RSV_MEMRESERVE, &out->rsv_dropped);
	}

	const uint8_t *st = b + off_st;
	const char *strs = (const char *)b + off_str;
	for (uint32_t at = 0; at + 4 <= size_st;) {
		uint32_t tok = fdt_be32(st + at);
		at += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *name = (const char *)st + at;
			at += (slen(name) + 1 + 3) & ~3u;
			if (++depth >= MAX_DEPTH) {
				return -3;
			}
			struct node *n = &stack[depth];
			for (uint32_t i = 0; i < sizeof(*n); i++) {
				((uint8_t *)n)[i] = 0;
			}
			n->name = name;
			n->addr_cells = 2;
			n->size_cells = 1;
		} else if (tok == FDT_PROP) {
			if (depth < 0 || at + 8 > size_st) {
				return -4;
			}
			uint32_t len = fdt_be32(st + at), nameoff = fdt_be32(st + at + 4);
			const uint8_t *v = st + at + 8;
			at += 8 + ((len + 3) & ~3u);
			if (at > size_st || nameoff >= size_str) {
				return -5;
			}
			const char *pn = strs + nameoff;
			struct node *n = &stack[depth];
			if (seq(pn, "#address-cells") && len == 4) {
				n->addr_cells = fdt_be32(v);
			} else if (seq(pn, "#size-cells") && len == 4) {
				n->size_cells = fdt_be32(v);
			} else if (seq(pn, "reg")) {
				n->reg = v, n->reg_len = len;
			} else if (seq(pn, "compatible")) {
				n->compat = (const char *)v, n->compat_len = len;
			} else if (seq(pn, "status")) {
				n->status = (const char *)v;
			} else if (seq(pn, "device_type")) {
				n->device_type = (const char *)v;
			} else if (seq(pn, "no-map")) {
				n->nomap = 1;
			} else if (seq(pn, "format")) {
				n->format = (const char *)v;
			} else if (seq(pn, "width") && len == 4) {
				n->width = fdt_be32(v);
			} else if (seq(pn, "height") && len == 4) {
				n->height = fdt_be32(v);
			} else if (seq(pn, "stride") && len == 4) {
				n->stride = fdt_be32(v);
			} else if (seq(pn, "reg-shift") && len == 4) {
				n->reg_shift = fdt_be32(v);
			} else if (seq(pn, "bootargs") && depth == 1 && named(n->name, "chosen") && len > 0) {
				out->bootargs = (const char *)v;
			} else if (seq(pn, "model") && depth == 0) {
				out->model = (const char *)v;
			} else if (seq(pn, "method") && depth == 1 && named(n->name, "psci")) {
				out->psci_smc = seq((const char *)v, "smc");
			}
		} else if (tok == FDT_END_NODE) {
			if (depth < 0) {
				return -6;
			}
			end_node(stack, depth, out);
			depth--;
		} else if (tok == FDT_NOP) {
			continue;
		} else if (tok == FDT_END) {
			break;
		} else {
			return -7;
		}
	}
	return depth == -1 ? 0 : -8;
}
