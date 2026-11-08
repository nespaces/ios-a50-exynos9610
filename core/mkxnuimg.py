#!/usr/bin/env python3
"""
Pack a phone's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image: for `fastboot boot` on redwood, to flash on starlte (Samsung has no fastboot).

  redwood: kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB
  starlte: kernel = BootShim + UEFI FD + ["XNUP" container], uncompressed and without a DTB (Project Silicium's
           starlte.toml: Samsung's boot loader takes no gzip, and its device tree from the phone's dtb partition)
  frankel: kernel = the Tensor G5's boot shim (socs/lga/shim/shim.bin, no UEFI: it starts the loader itself) +
           ["XNUP" container], a boot image of header v4 without a DTB (the boot loader keeps its own, from
           vendor_boot), with an AVB hash footer when avbtool is at hand (`fastboot boot` of an image without one
           does nothing on a Pixel 6a, PixOS found: devices/frankel/README.md)

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from devices/<device>/prebuilt/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]

The device and what it takes are set in the environment:
  UEFI_TARGET   Mu-Silicium device name: redwood (default) or starlte; or frankel    UEFI_FD, DTB   files to use instead
  FD_BASE, FD_SIZE, PAYLOAD_BASE    addresses from its UEFI port's memory map (defaults: the device's, in DEVICES below)
  frankel: SHIM, BOOT_CMDLINE (scion.fb=... and the like, for the shim), MKBOOTIMG, AVBTOOL, AVB_KEY,
           AVB_PARTITION_SIZE, OS_VERSION, OS_PATCH_LEVEL; --kernel-only writes the arm64 Image alone (QEMU's -kernel)
"""
import argparse, gzip, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
def env_int(name, default):
    v = os.environ.get(name, '')
    return int(v, 0) if v else default
# what each device's boot loader takes (its Mu-Silicium Resources/Configs/<device>.toml) and where its UEFI image lives
DEVICES = {
    'redwood': dict(fd_base=0x9FD00000, gzip=True, append_dtb=True, dtb_arg=False),
    # (Samsung's boot loader reads the device tree from the phone's own dtb partition: none goes in the image)
    # (... and the arm64 header's image size made the whole image's, the payload's included: a boot loader that puts
    # the device tree or anything else past the image it was told of would otherwise put it over the payload)
    'starlte': dict(fd_base=0x8F000000, gzip=False, append_dtb=False, dtb_arg=False, header_size=True),
    # (no UEFI: the shim is the SoC repository's, socs/lga, and starts the loader itself)
    'frankel': dict(fd_base=0, shim=os.path.join('socs', 'lga', 'shim', 'shim.bin')),
    # (no UEFI either: the device repository's own shim, devices/a50/soc/shim; S-Boot's packing: header v1,
    # uncompressed, the device tree from the phone's dtb partition, no AVB)
    'a50': dict(fd_base=0, shim=os.path.join('devices', 'a50', 'soc', 'shim', 'shim.bin'), samsung=True),
}
TARGET = os.environ.get('UEFI_TARGET') or 'redwood'
if TARGET not in DEVICES:
    sys.exit(f'mkxnuimg.py: no packing known for {TARGET} (DEVICES in this file: {", ".join(DEVICES)})')
DEV = DEVICES[TARGET]
PREBUILT = os.path.join(HERE, 'devices', TARGET, 'prebuilt')
FD_BASE, FD_SIZE = env_int('FD_BASE', DEV['fd_base']), env_int('FD_SIZE', 0x200000)
PAYLOAD_BASE, PAYLOAD_MAX = env_int('PAYLOAD_BASE', 0xF0000000), 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, f'xnu-{TARGET}.img'))
ap.add_argument('--mode', default='RELEASE')
ap.add_argument('--kernel-only', action='store_true', help='frankel: the arm64 Image (shim + payload) alone, for QEMU')
args = ap.parse_args()

def find_tool(env, names, paths):
    """a tool named in the environment, else the first of `paths` (relative to here) there is, else on PATH"""
    if os.environ.get(env):
        return os.environ[env]
    for p in paths:
        if os.path.isfile(os.path.join(HERE, p)):
            return os.path.join(HERE, p)
    for d in os.environ.get('PATH', '').split(os.pathsep):
        for n in names:
            if os.path.isfile(os.path.join(d, n)):
                return os.path.join(d, n)
    return None

def shim_image():
    """frankel: shim + payload, its arm64 header's image_size the whole; a boot image of header v4 around it, AVB-footed"""
    path = os.environ.get('SHIM') or os.path.join(HERE, DEV['shim'])
    if not os.path.isfile(path):
        sys.exit(f'mkxnuimg.py: {path} not found: make -C socs/lga/shim (tools/hakios-device.py sync lga first)')
    shim = open(path, 'rb').read()
    assert shim[56:60] == b'ARMd', f'{path}: not an arm64 Image'
    data = open(args.payload, 'rb').read() if args.payload else b''
    if data:
        assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX, 'bad or oversized payload'
        print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')
    blob = bytearray(shim + data)
    blob += b'\0' * (-len(blob) % 16)
    struct.pack_into('<Q', blob, 16, (len(blob) + 0xFFFF) & ~0xFFFF)     # image_size: the boot loader puts nothing over it
    if args.kernel_only:
        open(args.output, 'wb').write(blob)
        print(f'{args.output}: {len(blob)} bytes (an arm64 Image: shim {len(shim)}, payload {len(data)})')
        return
    mkbootimg = find_tool('MKBOOTIMG', ['mkbootimg', 'mkbootimg.py'],
                          ['Mu-Silicium/Resources/Scripts/mkbootimg.py', 'devices/starlte/prebuilt/mkbootimg.py'])
    if mkbootimg is None:
        sys.exit('mkxnuimg.py: no mkbootimg (AOSP system/tools/mkbootimg): MKBOOTIMG=... or on PATH')
    # an empty ramdisk (a newc cpio of its trailer alone): the boot loader expects one, the shim takes none
    name = b'TRAILER!!!\0'
    cpio = b'070701' + b''.join(b'%08X' % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0)) + name
    cpio += b'\0' * (-len(cpio) % 4)
    cpio += b'\0' * (-len(cpio) % 512)
    kernel, ramdisk = args.output + '.kernel~', args.output + '.ramdisk~'
    open(kernel, 'wb').write(blob)
    open(ramdisk, 'wb').write(gzip.compress(cpio, 9, mtime=0))
    try:
        cmd = [mkbootimg] if not mkbootimg.endswith('.py') else [sys.executable, mkbootimg]
        if DEV.get('samsung'):
            # S-Boot takes a header v1 image, the kernel uncompressed; its device tree is the phone's own
            subprocess.check_call(cmd + ['--header_version', '1', '--kernel', kernel, '--ramdisk', ramdisk,
                                         '-o', args.output])
        else:
            subprocess.check_call(cmd + ['--header_version', '4', '--kernel', kernel, '--ramdisk', ramdisk,
                                         '--cmdline', os.environ.get('BOOT_CMDLINE', ''),
                                         '--os_version', os.environ.get('OS_VERSION', '16.0.0'),
                                         '--os_patch_level', os.environ.get('OS_PATCH_LEVEL', '2026-10'), '-o', args.output])
    finally:
        os.unlink(kernel)
        os.unlink(ramdisk)
    size = os.path.getsize(args.output)
    if DEV.get('samsung'):
        print(f'{args.output}: {size} bytes (a50: boot image v1 of shim {len(shim)} + payload, no AVB)')
        return
    avbtool = find_tool('AVBTOOL', ['avbtool', 'avbtool.py'], [])
    if avbtool is None:
        print(f'{args.output}: {size} bytes, WITHOUT an AVB footer (no avbtool: AVBTOOL=...); a Pixel may refuse it')
        return
    key = os.environ.get('AVB_KEY') or args.output + '.testkey.pem'
    if not os.path.isfile(key):
        subprocess.check_call(['openssl', 'genrsa', '-out', key, '2048'], stderr=subprocess.DEVNULL)
    part = env_int('AVB_PARTITION_SIZE', max(64 << 20, (size + (2 << 20) + 0xFFFFF) & ~0xFFFFF))
    cmd = [avbtool] if not avbtool.endswith('.py') else [sys.executable, avbtool]
    subprocess.check_call(cmd + ['add_hash_footer', '--image', args.output, '--partition_name', 'boot',
                                 '--partition_size', str(part), '--key', key, '--algorithm', 'SHA256_RSA2048'])
    print(f'{args.output}: {os.path.getsize(args.output)} bytes (boot image v4 of {size}, AVB footer, test key {key})')

if DEV.get('shim'):
    shim_image()
    sys.exit(0)

fd_path = os.environ.get('UEFI_FD') or os.path.join(MU, f'Build/{TARGET}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path) and os.path.isdir(os.path.join(MU, 'BootShim')):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{TARGET}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
    if os.environ.get('UEFI_FD'): parts['fd'] = os.environ['UEFI_FD']
if not (DEV['append_dtb'] or DEV['dtb_arg']):
    del parts['dtb']
for name, path in parts.items():
    if not os.path.isfile(path):
        hint = ''
        if name == 'fd':
            hint = ("\n  The UEFI image is not in the repository: it carries the phone vendor's own drivers, which are not\n"
                    "  redistributed here. Build it from your own phone's firmware: uefi/README.md")
        sys.exit(f'mkxnuimg.py: {path} not found{hint}')
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if DEV.get('header_size') and shim[56:60] == b'ARMd':
    size = len(blob) + (os.path.getsize(args.payload) + 0x1000 if args.payload else 0)
    shim = shim[:16] + struct.pack('<Q', (size + 0xFFFF) & ~0xFFFF) + shim[24:]
    blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

if os.environ.get('DTB'): parts['dtb'] = os.environ['DTB']
dtb = open(parts['dtb'], 'rb').read() if 'dtb' in parts else b''
kernel = args.output + '.kernel~'
open(kernel, 'wb').write((gzip.compress(blob, 6) if DEV['gzip'] else blob) + (dtb if DEV['append_dtb'] else b''))
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '-o', args.output] +
                          (['--dtb', parts['dtb']] if DEV['dtb_arg'] else []))
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
