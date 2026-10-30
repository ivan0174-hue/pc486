#!/usr/bin/env python3
"""
pc486 - create a FAT12 floppy image (1.44 MB by default) containing files.

    mkfloppy.py out.img file1 [file2 ...] [--size 1440] [--boot bootsector.bin]

Files are stored in the root directory using their 8.3 upper-case names.
The image is not bootable unless a boot sector is given.
"""
import os
import struct
import sys

GEOM = {  # KB: (sectors per track, heads, tracks, sectors/cluster, root entries, media, fat sectors)
    360: (9, 2, 40, 2, 112, 0xFD, 2),
    720: (9, 2, 80, 2, 112, 0xF9, 3),
    1200: (15, 2, 80, 1, 224, 0xF9, 7),
    1440: (18, 2, 80, 1, 224, 0xF0, 9),
    2880: (36, 2, 80, 2, 240, 0xF0, 9),
}


def name83(path):
    base = os.path.basename(path).upper()
    if '.' in base:
        n, e = base.rsplit('.', 1)
    else:
        n, e = base, ''
    return (n[:8].ljust(8) + e[:3].ljust(3)).encode('ascii')


def build(out, files, size_kb=1440, boot=None):
    spt, heads, tracks, spc, rootents, media, fatsecs = GEOM[size_kb]
    total = spt * heads * tracks
    img = bytearray(total * 512)
    bs = bytearray(512)
    if boot:
        bs[:] = open(boot, 'rb').read()[:512].ljust(512, b'\0')
    else:
        bs[0:3] = b'\xeb\x3c\x90'
        # tiny "not bootable" message loop
        code = b'\xfa\xeb\xfe'
        bs[0x3e:0x3e + len(code)] = code
    bs[3:11] = b'PC486   '
    struct.pack_into('<HBHBHHBHHHII', bs, 11, 512, spc, 1, 2, rootents, total,
                     media, fatsecs, spt, heads, 0, 0)
    struct.pack_into('<BBBI11s8s', bs, 36, 0, 0, 0x29, 0x12345678,
                     b'PC486 DISK ', b'FAT12   ')
    bs[510:512] = b'\x55\xaa'
    img[0:512] = bs

    fat = bytearray(fatsecs * 512)
    fat[0:3] = bytes([media, 0xff, 0xff])

    def set_fat(n, v):
        off = n * 3 // 2
        if n & 1:
            fat[off] = (fat[off] & 0x0f) | ((v << 4) & 0xf0)
            fat[off + 1] = (v >> 4) & 0xff
        else:
            fat[off] = v & 0xff
            fat[off + 1] = (fat[off + 1] & 0xf0) | ((v >> 8) & 0x0f)

    root_start = 1 + 2 * fatsecs
    root_secs = rootents * 32 // 512
    data_start = root_start + root_secs
    nclusters = (total - data_start) // spc
    root = bytearray(root_secs * 512)
    cl = 2
    for i, f in enumerate(files):
        data = open(f, 'rb').read()
        ncl = (len(data) + spc * 512 - 1) // (spc * 512)
        first = cl if ncl else 0
        if cl + ncl - 2 > nclusters:
            raise SystemExit('disk full')
        for k in range(ncl):
            set_fat(cl + k, cl + k + 1 if k < ncl - 1 else 0xfff)
            off = (data_start + (cl + k - 2) * spc) * 512
            chunk = data[k * spc * 512:(k + 1) * spc * 512]
            img[off:off + len(chunk)] = chunk
        ent = struct.pack('<11sB10sHHHI', name83(f), 0x20, b'\0' * 10, 0, 0x2821,
                          first, len(data))
        root[i * 32:(i + 1) * 32] = ent
        cl += ncl
    for k in range(2):
        img[(1 + k * fatsecs) * 512:(1 + (k + 1) * fatsecs) * 512] = fat
    img[root_start * 512:data_start * 512] = root
    open(out, 'wb').write(img)


if __name__ == '__main__':
    args = sys.argv[1:]
    size = 1440
    boot = None
    if '--size' in args:
        i = args.index('--size')
        size = int(args[i + 1])
        del args[i:i + 2]
    if '--boot' in args:
        i = args.index('--boot')
        boot = args[i + 1]
        del args[i:i + 2]
    if len(args) < 1:
        print(__doc__)
        sys.exit(1)
    build(args[0], args[1:], size, boot)
