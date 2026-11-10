#!/usr/bin/env python3
"""The Galaxy A50's WLBT keep-alive patch, from the builder's own copy of Samsung's kernel source.

Samsung's CFG_REQ handler for the Exynos 9610 (drivers/misc/samsung/scsc/platform_mif_9610.c, ka_patch[]) writes a
small program into the WLBT boot configuration block before it acknowledges the chip's configuration request. It is
Samsung's, so it is not in this tree: this reads it out of their GPL source and writes it as fw-scsc/scsc_ka.bin
(little-endian 32-bit words), where userland/build.sh puts it into fw/ for drivers/wifi/hakios.scsc.c.

    tools/a50-scsc-ka.py <kernel source>/drivers/misc/samsung/scsc/platform_mif_9610.c fw-scsc/scsc_ka.bin
"""
import re, struct, sys

src, out = sys.argv[1], sys.argv[2]
text = open(src, errors='replace').read()
m = re.search(r'uint32_t\s+ka_patch\[\]\s*=\s*\{(.*?)\};', text, re.S)
if not m:
    sys.exit('no ka_patch[] in ' + src)
body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)     # (its comment has numbers of its own)
words = [int(w, 16) for w in re.findall(r'0x[0-9a-fA-F]+', body)]
open(out, 'wb').write(struct.pack('<%dI' % len(words), *words))
print('%s: %d words' % (out, len(words)))
