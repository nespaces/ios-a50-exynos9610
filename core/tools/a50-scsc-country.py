#!/usr/bin/env python3
"""The regulatory rules of one country for the Galaxy A50's Wi-Fi firmware, from the phone's own database.

Samsung's driver sends MLME_SET_COUNTRY_REQ after the WLAN service starts (mlme.c slsi_mlme_set_country()): the
country's code, its DFS region and its rules, 7 octets each (start and end in half-MHz, 16 bits little-endian; the
widest channel in MHz; the power in dBm; the flags), read from /vendor/etc/wifi/slsi_reg_database.bin
(reg_info.c slsi_read_regulatory()). Until it has them the firmware only listens: it scans passively and cannot join.
This writes fw-scsc/scsc_country.bin for drivers/wifi/hakios.scsc.c: alpha2 (2), the DFS region (1), the number of
rules (1), then the rules.

    tools/a50-scsc-country.py slsi_reg_database.bin RU fw-scsc/scsc_country.bin
"""
import struct, sys

db, alpha2, out = open(sys.argv[1], 'rb').read(), sys.argv[2].encode(), sys.argv[3]
off = 0
def u32():
    global off
    v = struct.unpack_from('<I', db, off)[0]; off += 4
    return v
script, major, minor = u32(), u32(), u32()
ranges = [(u32(), u32(), u32()) for _ in range(u32())]
rules = [(u32(), u32(), u32()) for _ in range(u32())]              # range index, eirp, flags
collections = []
for _ in range(u32()):
    collections.append([u32() for _ in range(u32())])
countries = {}
for _ in range(u32()):
    a, dfs = db[off:off + 2], db[off + 3]; off += 4
    countries[a] = (dfs, u32())
if alpha2 not in countries:
    sys.exit('%s is not in the database (%d countries, version %d.%d)' % (alpha2.decode(), len(countries), major, minor))
dfs, c = countries[alpha2]
body = b''
n = 0
for r in collections[c]:
    start, end, bw = ranges[rules[r][0]]
    if start >= 57000:
        continue
    body += struct.pack('<HHBBB', (start * 2) & 0xFFFF, (end * 2) & 0xFFFF, bw & 0xFF, rules[r][1] & 0xFF, rules[r][2] & 0xFF)
    n += 1
    print('  %u-%u MHz, %u MHz wide, %u dBm, flags %#x' % (start, end, bw, rules[r][1], rules[r][2]))
open(out, 'wb').write(alpha2 + bytes([dfs, n]) + body)
print('%s: %s, DFS region %u, %u rules (database %d.%d)' % (out, alpha2.decode(), dfs, n, major, minor))
