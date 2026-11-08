#!/usr/bin/env python3
"""Samsung's ACPM "fvp" plugin for the Galaxy A50 (Exynos 9610), out of the phone's own stock firmware.

usage: tools/a50-fvp.py STOCK [OUT]
    STOCK  the stock BOOT image of an SM-A505F* (boot.img from the AP tarball, or dd'd in TWRP),
           or Samsung's kernel source's firmware/exynos9610_acpm_fvp.fw.ihex
    OUT    default drivers/core/hakios.exynos9610_fvp.h

The plugin is Samsung's binary; it is not in this repository. Linux attaches it at boot and S-Boot does not, so
HakiOS loads it into the APM's SRAM itself (drivers/core/hakios.exynos_acpm.c, acpm9610=6). The repository carries a
placeholder header of size 0, with which the driver leaves the plugin alone. This finds the 5120 bytes in the kernel
built into the stock BOOT (they are there uncompressed), checks them by SHA-256 and writes the real header.
Keep the result out of commits (git update-index --skip-worktree drivers/core/hakios.exynos9610_fvp.h).
"""
import hashlib
import os
import sys

SIZE = 5120
SHA256 = '8f71dccbd234d6380951bfc0e84e9309054bf5acc97738a4f49279e320691219'
ANCHOR = bytes.fromhex('01229c21064b10b5')      # its entry's first instructions (0x9000), to look for


def from_ihex(text):
    out = bytearray()
    base = 0
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(':'):
            continue
        rec = bytes.fromhex(line[1:])
        n, addr, kind, data = rec[0], rec[1] << 8 | rec[2], rec[3], rec[4:4 + rec[0]]
        if kind == 0:
            at = base + addr
            if len(out) < at + n:
                out.extend(b'\0' * (at + n - len(out)))
            out[at:at + n] = data
        elif kind == 4:
            base = (data[0] << 8 | data[1]) << 16
        elif kind == 1:
            break
    return bytes(out)


def find(blob):
    at = blob.find(ANCHOR)
    while at >= 0:
        cand = blob[at:at + SIZE]
        if len(cand) == SIZE and hashlib.sha256(cand).hexdigest() == SHA256:
            return cand
        at = blob.find(ANCHOR, at + 1)
    return None


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'drivers', 'core',
                                                            'hakios.exynos9610_fvp.h')
    data = open(src, 'rb').read()
    plugin = find(from_ihex(data.decode('ascii', 'replace'))) if src.endswith('.ihex') else find(data)
    if plugin is None:
        sys.exit('a50-fvp.py: no fvp plugin with SHA-256 %s in %s' % (SHA256, src))
    rows = [',\n'.join('\t' + ', '.join('0x%02x' % b for b in plugin[i:i + 16]) for i in range(0, SIZE, 16))]
    with open(out, 'w') as f:
        f.write('/*\n * Samsung\'s ACPM "fvp" plugin for the Exynos 9610 (5120 bytes, linked at the APM\'s 0x9000), made by\n'
                ' * tools/a50-fvp.py from the phone\'s own stock firmware. SHA-256 %s.\n'
                ' * Samsung\'s binary: not in the repository.\n */\n' % SHA256)
        f.write('#define HAKIOS_FVP_9610_SIZE %d\nstatic const uint8_t hakios_fvp_9610[HAKIOS_FVP_9610_SIZE] = {\n' % SIZE)
        f.write(rows[0] + ',\n};\n')
    print('a50-fvp.py: %s: the plugin found and checked, %s written' % (src, out))


if __name__ == '__main__':
    main()
