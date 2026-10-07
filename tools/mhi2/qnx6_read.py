#!/usr/bin/env python3
"""Minimal read-only fs-qnx6 reader: list directories and extract files.

On-disk format per the project's mkqnx6fs.py / validate_qnx6.py:
  [boot 0x2000][SB#1 0x1000][ data blocks 0.. ][SB#2 0x1000]
  data block B at partition byte (B + sblks_off)*BS, sblks_off=(0x2000+0x1000)/BS
Superblock @ part+0x2000, magic 0x68191122. 4 root nodes @ off 72,152,232,312:
  size(Q) ptr[16](16I) levels(B) mode(B). root[0]=inode tree, root[2]=longfile tree.
Inodes are 128 bytes: size(Q@0) mode(H@32) ptr[16](16I@36) levels(B@100) status(B@101).
Dir entries are 32 bytes: ino(I@0) namelen(B@4) name@5 ; namelen==0xff -> long name
  (lfile index I@8). Longfile block: len(H@0) name@2.
"""
import sys, struct

MAGIC = 0x68191122
UNUSED = 0xffffffff


class Qnx6:
    def __init__(self, path, part_base):
        self.f = open(path, "rb")
        self.base = part_base
        sb = self._raw(part_base + 0x2000, 512)
        if struct.unpack_from("<I", sb, 0)[0] != MAGIC:
            raise SystemExit("no QNX6 superblock at %#x" % (part_base + 0x2000))
        self.bs = struct.unpack_from("<I", sb, 48)[0]
        self.ppb = self.bs // 4
        self.num_inodes = struct.unpack_from("<I", sb, 52)[0]
        self.num_blocks = struct.unpack_from("<I", sb, 60)[0]
        self.sblks_off = (0x2000 // self.bs) + (0x1000 // self.bs)
        self.inode_root = self._rootnode(sb, 72)
        self.long_root = self._rootnode(sb, 232)
        # cache the whole inode table
        self.itable = self._read_tree(self.inode_root)

    def _raw(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _rootnode(self, sb, off):
        size = struct.unpack_from("<Q", sb, off)[0]
        ptr = list(struct.unpack_from("<16I", sb, off + 8))
        levels = sb[off + 72]
        return dict(size=size, ptr=ptr, levels=levels)

    def _block(self, phys):
        return self._raw(self.base + (phys + self.sblks_off) * self.bs, self.bs)

    def _bmap(self, node, logical):
        levels = node["levels"]
        ppb = self.ppb
        if levels == 0:
            cur = node["ptr"][logical]
        else:
            divs = [None, ppb, ppb * ppb, ppb * ppb * ppb]
            rem = logical
            top = rem // divs[levels]
            rem %= divs[levels]
            idx = [top]
            for lv in range(1, levels):
                d = divs[levels - lv]
                idx.append(rem // d)
                rem %= d
            idx.append(rem)
            cur = node["ptr"][idx[0]]
            for k in range(1, levels + 1):
                blk = self._block(cur)
                cur = struct.unpack_from("<I", blk, idx[k] * 4)[0]
        if cur == UNUSED:
            return None
        return cur

    def _read_tree(self, node):
        size = node["size"]
        nblk = (size + self.bs - 1) // self.bs
        out = bytearray()
        for L in range(nblk):
            phys = self._bmap(node, L)
            out += self._block(phys) if phys is not None else bytes(self.bs)
        return bytes(out[:size])

    def inode(self, ino):
        o = (ino - 1) * 128
        e = self.itable[o:o + 128]
        size = struct.unpack_from("<Q", e, 0)[0]
        mode = struct.unpack_from("<H", e, 32)[0]
        ptr = list(struct.unpack_from("<16I", e, 36))
        levels = e[100]
        return dict(size=size, mode=mode, ptr=ptr, levels=levels, ino=ino)

    def read_file(self, ino):
        return self._read_tree(self.inode(ino))

    def _longname(self, idx):
        blk = self._block(self.long_root["ptr"][idx] if self.long_root["levels"] == 0
                          else self._bmap(self.long_root, idx))
        n = struct.unpack_from("<H", blk, 0)[0]
        return blk[2:2 + n].decode("latin1")

    def listdir(self, ino):
        data = self.read_file(ino)
        out = []
        for o in range(0, len(data), 32):
            slot = data[o:o + 32]
            if len(slot) < 32:
                break
            tino = struct.unpack_from("<I", slot, 0)[0]
            nlen = slot[4]
            if tino == 0 or tino == UNUSED:
                continue
            if nlen == 0xff:
                lidx = struct.unpack_from("<I", slot, 8)[0]
                name = self._longname(lidx)
            else:
                name = slot[5:5 + nlen].decode("latin1")
            out.append((name, tino))
        return out

    def is_dir(self, ino):
        return (self.inode(ino)["mode"] & 0xf000) == 0x4000


def walk(fs, ino=1, path="", depth=0, maxdepth=3):
    for name, tino in fs.listdir(ino):
        if name in (".", ".."):
            continue
        ind = fs.inode(tino)
        kind = "d" if (ind["mode"] & 0xf000) == 0x4000 else "f"
        print("%-55s %s %10d" % (path + "/" + name, kind, ind["size"]))
        if kind == "d" and depth < maxdepth:
            walk(fs, tino, path + "/" + name, depth + 1, maxdepth)


if __name__ == "__main__":
    img = sys.argv[1]
    base = int(sys.argv[2], 0)
    fs = Qnx6(img, base)
    sys.stderr.write("bs=%d inodes=%d blocks=%d\n" % (fs.bs, fs.num_inodes, fs.num_blocks))
    if len(sys.argv) > 3 and sys.argv[3] == "extract":
        # extract path: argv[4]=qnx path, argv[5]=out
        parts = [p for p in sys.argv[4].split("/") if p]
        ino = 1
        for p in parts:
            ino = dict(fs.listdir(ino))[p]
        open(sys.argv[5], "wb").write(fs.read_file(ino))
        sys.stderr.write("extracted %s -> %s (%d bytes)\n" %
                         (sys.argv[4], sys.argv[5], fs.inode(ino)["size"]))
    else:
        walk(fs, 1, "", 0, int(sys.argv[3]) if len(sys.argv) > 3 else 2)
