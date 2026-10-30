#!/usr/bin/env python3
"""
pc486 - create a FAT16 hard disk image and copy a folder into it.

    mkhdd.py disco.img SIZE_MB [folder]

The image has a standard MBR with one primary FAT16 partition and the
geometry used by pc486 (16 heads, 63 sectors per track). Sub-folders are
copied too; long names are shortened to 8.3 (e.g. "Programmi vari" ->
"PROGRA~1"). The disk is a data disk (not bootable): attach it as hdb in
pc486.ini and it appears as D: in DOS.

Max size for the 1993 AMI BIOS: 504 MB.
"""
import os
import struct
import sys

HEADS, SPT = 16, 63
CYL_BYTES = HEADS * SPT * 512


def short_name(name, used):
    if '.' in name and not name.startswith('.'):
        base, ext = name.rsplit('.', 1)
    else:
        base, ext = name, ''
    valid = lambda t: ''.join(c for c in t.upper() if c.isalnum() or c in "!#$%&'()-@^_`{}~")
    b, e = valid(base) or 'FILE', valid(ext)[:3]
    exact = len(b) <= 8 and b == base.upper() and e == ext.upper()
    key = b.ljust(8) + e.ljust(3)
    if not exact or key in used:
        n = 1
        while True:
            t = '~%d' % n
            key = (b[:8 - len(t)] + t).ljust(8) + e.ljust(3)
            if key not in used:
                break
            n += 1
    used.add(key)
    return key.encode('ascii')


def build(out, size_mb, folder=None):
    cyls = max(2, (size_mb * 1024 * 1024) // CYL_BYTES)
    if cyls > 1024:
        print('warning: more than 1024 cylinders, the 1993 BIOS cannot use all of it')
    total = cyls * CYL_BYTES // 512
    start = SPT
    psecs = total - start
    if psecs * 512 <= 128 << 20:
        spc = 4
    elif psecs * 512 <= 256 << 20:
        spc = 8
    elif psecs * 512 <= 512 << 20:
        spc = 16
    elif psecs * 512 <= 1024 << 20:
        spc = 32
    else:
        spc = 64
    rootents = 512
    reserved = 1
    # FAT size: 2 bytes per cluster
    fatsecs = 1
    while True:
        data = psecs - reserved - 2 * fatsecs - rootents * 32 // 512
        nclus = data // spc
        need = (nclus + 2) * 2 // 512 + 1
        if need <= fatsecs:
            break
        fatsecs = need
    img = bytearray(total * 512)

    # MBR: copy pc486's MBR code if available
    mbrcode = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mbr.bin')
    if os.path.exists(mbrcode):
        img[0:446] = open(mbrcode, 'rb').read()[:446].ljust(446, b'\0')

    def chs(lba):
        c = lba // (HEADS * SPT)
        r = lba % (HEADS * SPT)
        h, s = r // SPT, r % SPT + 1
        if c > 1023:
            c, h, s = 1023, HEADS - 1, SPT
        return bytes([h, ((c >> 2) & 0xc0) | s, c & 0xff])

    ptype = 0x04 if psecs < 65536 else 0x06
    pe = bytes([0x80]) + chs(start) + bytes([ptype]) + chs(total - 1) + struct.pack('<II', start, psecs)
    img[446:462] = pe
    img[510:512] = b'\x55\xaa'

    # boot sector (not bootable: prints nothing and halts)
    bs = bytearray(512)
    bs[0:3] = b'\xeb\x3c\x90'
    bs[3:11] = b'PC486   '
    struct.pack_into('<HBHBHHBHHHII', bs, 11, 512, spc, reserved, 2, rootents,
                     psecs if psecs < 65536 else 0, 0xf8, fatsecs, SPT, HEADS, start,
                     psecs if psecs >= 65536 else 0)
    struct.pack_into('<BBBI11s8s', bs, 36, 0x80, 0, 0x29, 0x486486, b'PC486 DATA ', b'FAT16   ')
    bs[0x3e:0x41] = b'\xfa\xeb\xfe'
    bs[510:512] = b'\x55\xaa'
    off = start * 512
    img[off:off + 512] = bs

    fat = bytearray(fatsecs * 512)
    struct.pack_into('<HH', fat, 0, 0xfff8, 0xffff)
    root_start = start + reserved + 2 * fatsecs
    data_start = root_start + rootents * 32 // 512
    nclus = (total - data_start) // spc
    clus_bytes = spc * 512
    next_cl = [2]

    def alloc(nbytes):
        n = max(1, (nbytes + clus_bytes - 1) // clus_bytes)
        first = next_cl[0]
        if first + n - 2 > nclus:
            raise SystemExit('disco pieno: aumenta la dimensione')
        for k in range(n):
            struct.pack_into('<H', fat, (first + k) * 2, first + k + 1 if k < n - 1 else 0xffff)
        next_cl[0] += n
        return first

    def cl_off(c):
        return (data_start + (c - 2) * spc) * 512

    def dirent(name, attr, cl, size):
        return struct.pack('<11sB10sHHHI', name, attr, b'\0' * 10, 0, 0x2821, cl, size)

    def write_dir(path, cluster, parent):
        # returns list of 32-byte entries for this directory's content
        entries = []
        used = set()
        names = sorted(os.listdir(path)) if path else []
        for n in names:
            full = os.path.join(path, n)
            sn = short_name(n, used)
            if os.path.isdir(full):
                sub_entries_count = len(os.listdir(full)) + 2
                size = max(clus_bytes, ((sub_entries_count * 32 + clus_bytes - 1) // clus_bytes) * clus_bytes)
                cl = alloc(size)
                entries.append(dirent(sn, 0x10, cl, 0))
                sub = [dirent(b'.          ', 0x10, cl, 0), dirent(b'..         ', 0x10, cluster, 0)]
                sub += write_dir(full, cl, cluster)
                blob = b''.join(sub)
                # directory clusters are contiguous
                o = cl_off(cl)
                img[o:o + len(blob)] = blob
            else:
                data = open(full, 'rb').read()
                cl = alloc(len(data)) if data else 0
                if data:
                    o = cl_off(cl)
                    img[o:o + len(data)] = data
                entries.append(dirent(sn, 0x20, cl, len(data)))
        return entries

    root = [dirent(b'PC486 DATA ', 0x08, 0, 0)]
    if folder:
        root += write_dir(folder, 0, 0)
    if len(root) > rootents:
        raise SystemExit('troppi file nella cartella principale (max %d)' % (rootents - 1))
    blob = b''.join(root)
    img[root_start * 512:root_start * 512 + len(blob)] = blob
    for k in range(2):
        o = (start + reserved + k * fatsecs) * 512
        img[o:o + len(fat)] = fat
    open(out, 'wb').write(img)
    print('%s: %d MB, %d cilindri, FAT16, %d byte per cluster' % (out, total * 512 >> 20, cyls, clus_bytes))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    build(sys.argv[1], int(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else None)
