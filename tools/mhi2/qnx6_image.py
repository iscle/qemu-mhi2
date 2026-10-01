#!/usr/bin/env python3
"""
Host-side mkqnx6fs: build a QNX6 (fs-qnx6) filesystem image from a directory tree.
On-disk format per Linux fs/qnx6 + include/linux/qnx6_fs.h (Kai Bankett) and the
QNX fs-qnx6 driver. Read-capable image; superblock checksum = crc32_be(0, sb[8:512]).

Layout (blocksize BS, default 4096):
  [bootblock 0x2000][SB#1 area 0x1000][ data blocks 0..num_blocks-1 ][SB#2 area 0x1000]
  s_blks_off = (0x2000>>bits)+(0x1000>>bits)  -> data block B at disk byte (B+s_blks_off)*BS
  SB#1 at disk byte 0x2000 ; SB#2 at disk block (num_blocks + s_blks_off)
"""
import os, sys, struct, mmap

BS = int(os.environ.get("QNX6_BS", "4096"))
PPB = BS // 4                       # pointers per indirect block
BOOT = 0x2000
SBAREA = 0x1000
BITS = BS.bit_length() - 1          # log2(BS)
SBLKS_OFF = (BOOT >> BITS) + (SBAREA >> BITS)   # =3 for BS=4096
MAGIC = 0x68191122
UNUSED = 0xffffffff
QNX6_FILE_DIRECTORY = 0x01
QNX6_FILE_NORMAL = 0x03

def crc32_be(crc, data):
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xffffffff if (crc & 0x80000000) else (crc << 1) & 0xffffffff
    return crc

def lfile_checksum(name: bytes):
    crc = 0
    for b in name:
        crc = (((crc >> 1) + b) ^ (0x80000000 if (crc & 1) else 0)) & 0xffffffff
    return crc

def total_blocks_for(D):
    """Total blocks (data + indirect tree) to store D data blocks."""
    if D == 0: return 0
    total = D; level = D
    while level > 16:
        ind = (level + PPB - 1) // PPB
        total += ind; level = ind
    return total

class Qnx6Builder:
    """Two-region allocator: a preallocated power-safe SYSTEM area (inode-tree +
    bitmap-tree blocks live here, split into two halves for the 2 superblock
    generations) followed by USERSPACE (file data + longfile)."""
    def __init__(self, outpath, slack_blocks=512):
        self.f = open(outpath, "wb+")
        self.blocks = {}             # idx -> bytes (<=BS)
        self.slack = slack_blocks
        self.long_names = []
        # allocators (set in build() once SYS size is known)
        self.sys_next = 0            # system area half-1 cursor (0..)
        self.user_next = 0           # userspace cursor (2*SYS ..)
        self.sys_limit = 0           # == SYS (half size)

    def alloc(self, region):
        if region == 'sys':
            b = self.sys_next; self.sys_next += 1
            return b
        b = self.user_next; self.user_next += 1
        return b

    def put(self, idx, data):
        assert len(data) <= BS
        self.f.seek((idx + SBLKS_OFF) * BS)
        self.f.write(data + b"\x00" * (BS - len(data)))

    def write_file_blocks(self, data: bytes, region='user'):
        size = len(data)
        nblk = (size + BS - 1) // BS
        if nblk == 0:
            return [UNUSED]*16, 0, 0
        idxs = []
        for i in range(nblk):
            b = self.alloc(region)
            self.put(b, data[i*BS:(i+1)*BS])
            idxs.append(b)
        ptr, levels = self.build_tree(idxs, region)
        return ptr, levels, size

    def build_tree(self, idxs, region='user'):
        levels = 0
        level = idxs
        while len(level) > 16:
            nxt = []
            for i in range(0, len(level), PPB):
                chunk = level[i:i+PPB]
                payload = b"".join(struct.pack("<I", p) for p in chunk)
                payload += b"\xff\xff\xff\xff" * (PPB - len(chunk))
                b = self.alloc(region)
                self.put(b, payload)
                nxt.append(b)
            level = nxt
            levels += 1
        ptr = list(level) + [UNUSED]*(16-len(level))
        return ptr, levels

    def add_long_name(self, name: bytes):
        k = len(self.long_names)
        self.long_names.append(name)
        return k

# ---- inode entry (128 bytes) ----
def pack_inode(size, mode, status, ptr, levels, links=1):
    b = bytearray(128)
    struct.pack_into("<Q", b, 0, size)          # di_size
    struct.pack_into("<I", b, 8, 0)             # uid
    struct.pack_into("<I", b, 12, 0)            # gid
    struct.pack_into("<I", b, 16, 0)            # ftime
    struct.pack_into("<I", b, 20, 0)            # mtime
    struct.pack_into("<I", b, 24, 0)            # atime
    struct.pack_into("<I", b, 28, 0)            # ctime
    struct.pack_into("<H", b, 32, mode)         # di_mode
    struct.pack_into("<H", b, 34, links)            # di_ext_mode
    for i in range(16):
        struct.pack_into("<I", b, 36+i*4, ptr[i])
    b[100] = levels                              # di_filelevels
    b[101] = status                              # di_status
    return bytes(b)

def pack_rootnode(size, ptr, levels, mode=0):
    b = bytearray(8 + 16*4 + 1 + 1 + 6)          # 80 bytes
    struct.pack_into("<Q", b, 0, size)
    for i in range(16):
        struct.pack_into("<I", b, 8+i*4, ptr[i])
    b[72] = levels
    b[73] = mode
    return bytes(b)

SB_VERSION1 = int(os.environ.get("QNX6_VER1", "4"))  # fs-qnx6 sb_version1 (off 0x1c): 4 (confirmed)
SB_VERSION2 = int(os.environ.get("QNX6_VER2", "0"))  # sb_version2 (off 0x1e)
BITMAP_FREE_IS_SET = int(os.environ.get("QNX6_BITMAP_FREE_SET", "0"))  # 1 => free blocks bit=1
SB_ALLOCGROUP = int(os.environ.get("QNX6_ALLOCGROUP", "0"))
RNODE_MODE = int(os.environ.get("QNX6_RNODE_MODE", "1"))  # real mkqnx6fs filler @0x10230c writes root mode byte (+0x49)=1 for ALL roots; mode=0 -> driver slot allocator @0xb858 (tst mode&1) rejects "Corrupted file system"
FREE_INODES = os.environ.get("QNX6_FREE_INODES", "")   # "" => 0 ; else int
SB1_ACTIVE = int(os.environ.get("QNX6_SB1_ACTIVE", "0"))  # 1 => SB#1 serial higher
VOLID = int(os.environ.get("QNX6_VOLID", "0"))
INODE_SLACK = int(os.environ.get("QNX6_INODE_SLACK", "0"))

def build(srcdir, outpath, slack_blocks=512, partition_blocks=None):
    qb = Qnx6Builder(outpath, slack_blocks)

    # ---- phase 1: assign inode numbers (DFS), root = 1 ----
    nodes = []   # (ino, path, parent_ino, kind)  kind: 'd','f','l'
    ino_counter = [1]
    def kind_of(p):
        if os.path.islink(p): return 'l'
        if os.path.isdir(p): return 'd'
        return 'f'
    root_ino = 1
    nodes.append([1, srcdir, 1, 'd'])
    ino_counter[0] = 1
    # BFS so inode numbers are contiguous & parents known
    queue = [(1, srcdir)]
    # need children mapping for dirs
    children = {1: []}
    while queue:
        pino, ppath = queue.pop(0)
        try:
            entries = sorted(os.listdir(ppath))
        except OSError:
            entries = []
        for name in entries:
            full = os.path.join(ppath, name)
            k = kind_of(full)
            ino_counter[0] += 1
            cino = ino_counter[0]
            nodes.append([cino, full, pino, k])
            children.setdefault(pino, []).append((name, cino, k))
            children.setdefault(cino, [])
            if k == 'd':
                queue.append((cino, full))
    num_inodes = ino_counter[0]

    # ---- phase 2a: build per-inode DATA bytes (registers long names), count blocks ----
    inode_data = {}      # ino -> (data_bytes, mode, status)
    for ino, path, pino, k in nodes:
        if k == 'd':
            data = bytearray()
            def add_entry(nm, target_ino):
                nonlocal data
                nb = nm.encode("utf-8", "surrogateescape") if isinstance(nm, str) else nm
                slot = bytearray(32)
                struct.pack_into("<I", slot, 0, target_ino)
                if len(nb) <= 27:
                    slot[4] = len(nb); slot[5:5+len(nb)] = nb
                else:
                    lk = qb.add_long_name(nb)
                    slot[4] = 0xff
                    struct.pack_into("<I", slot, 8, lk)
                    struct.pack_into("<I", slot, 12, lfile_checksum(nb))
                data += slot
            add_entry(".", ino); add_entry("..", pino)
            for nm, cino, ck in children.get(ino, []):
                add_entry(nm, cino)
            data += bytes((-len(data)) % 1024)
            inode_data[ino] = (bytes(data), 0o040755, QNX6_FILE_DIRECTORY)
        elif k == 'l':
            tgt = os.readlink(path).encode("utf-8", "surrogateescape")
            inode_data[ino] = (tgt, 0o120777, QNX6_FILE_NORMAL)
        else:
            with open(path, "rb") as fh:
                inode_data[ino] = (mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ, trackfd=False) if os.fstat(fh.fileno()).st_size else b"", 0o100755, QNX6_FILE_NORMAL)

    # ---- size precompute -> power-safe SYSTEM area size (2 halves) ----
    file_tree_total = sum(total_blocks_for((len(inode_data[i][0]) + BS - 1)//BS)
                          for i in range(1, num_inodes+1))
    num_long = len(qb.long_names)
    long_tree_total = total_blocks_for(num_long)
    U = file_tree_total + long_tree_total + slack_blocks          # userspace blocks
    inode_capacity = num_inodes + INODE_SLACK
    inode_data_blocks = (inode_capacity*128 + BS - 1)//BS
    I = total_blocks_for(inode_data_blocks)                       # inode-tree blocks (system)
    B = 1
    for _ in range(12):                                           # converge bitmap size
        SYS = I + B
        num_blocks = 2*SYS + U
        bm_data = (((num_blocks + 7)//8) + BS - 1)//BS
        nB = total_blocks_for(bm_data)
        if nB == B: break
        B = nB
    SYS = I + B
    num_blocks = 2*SYS + U
    user_base = 2*SYS
    qb.sys_next = 0
    qb.user_next = user_base
    qb.sys_limit = SYS

    # ---- phase 2b: assign userspace blocks (file data + longfile) ----
    inode_entries = {}
    for ino in range(1, num_inodes+1):
        data, mode, status = inode_data[ino]
        ptr, levels, size = qb.write_file_blocks(data, 'user')
        if isinstance(data, mmap.mmap): data.close()
        inode_entries[ino] = pack_inode(size, mode, status, ptr, levels,
            2 + sum(k == "d" for _, _, k in children[ino]) if mode & 0o170000 == 0o040000 else 1)
    if qb.long_names:
        long_blocks = []
        for nm in qb.long_names:
            blk = bytearray(BS)
            struct.pack_into("<H", blk, 0, len(nm)); blk[2:2+len(nm)] = nm[:510]
            b = qb.alloc('user'); qb.put(b, bytes(blk)); long_blocks.append(b)
        long_ptr, long_levels = qb.build_tree(long_blocks, 'user')
        long_size = num_long * BS
    else:
        long_ptr, long_levels, long_size = [UNUSED]*16, 0, 0
    user_used_end = qb.user_next                                  # first free userspace block

    # ---- phase 3: SYSTEM area (inode tree then bitmap tree), half-1 ----
    itab = bytearray(inode_capacity * 128)
    for ino in range(1, num_inodes+1):
        itab[(ino-1)*128:(ino)*128] = inode_entries[ino]
    inode_ptr, inode_levels, inode_size = qb.write_file_blocks(bytes(itab), 'sys')

    # Reserve both metadata generations. Bits beyond the filesystem's final
    # block must also be allocated, through the bitmap's last physical block;
    # native chkqnx6fs checks this padding even beyond the root node's byte size.
    bm_bytes = (num_blocks + 7)//8
    bitmap = bytearray(((bm_bytes + BS - 1)//BS)*BS)
    def setbit(i): bitmap[i>>3] |= (1 << (i & 7))
    for i in range(0, 2*SYS):                # both metadata generations reserved
        setbit(i)
    for i in range(user_base, user_used_end): # userspace actually used
        setbit(i)
    for i in range(num_blocks, len(bitmap)*8):
        setbit(i)
    if BITMAP_FREE_IS_SET:
        inv = bytearray(b'\xff')*len(bitmap)
        for i in range(num_blocks):
            if not (bitmap[i>>3] >> (i&7)) & 1: continue
            inv[i>>3] &= ~(1 << (i&7)) & 0xff
        # clear used bits in inv where bitmap set -> inv already cleared; pad beyond num_blocks=0
        for i in range(num_blocks, len(bitmap)*8): inv[i>>3] &= ~(1 << (i&7)) & 0xff
        bitmap = inv
    bm_ptr, bm_levels, bm_size = qb.write_file_blocks(bytes(bitmap), 'sys')
    bm_size = bm_bytes
    assert qb.sys_next <= SYS, (qb.sys_next, SYS, I, B)
    used_total = 2*SYS + (user_used_end - user_base)
    free_blocks = num_blocks - used_total
    SYSTEM_AREA = 2*SYS

    # ---- assemble & write image ----
    if partition_blocks is not None:
        num_blocks = max(num_blocks, partition_blocks - SBLKS_OFF - 1)
        free_blocks = num_blocks - used_total
    total_disk_blocks = num_blocks + SBLKS_OFF + 1   # +1 for SB#2 area
    f = qb.f
    f.truncate(total_disk_blocks * BS)
    # bootblock zeros already (sparse). write data blocks.
    for idx, data in qb.blocks.items():
        f.seek((idx + SBLKS_OFF) * BS)
        f.write(data + b"\x00"*(BS-len(data)))

    def make_sb(serial):
        sb = bytearray(512)
        struct.pack_into("<I", sb, 0, MAGIC)
        # checksum at 4 filled later
        struct.pack_into("<Q", sb, 8, serial)
        struct.pack_into("<I", sb, 16, 0)        # ctime
        struct.pack_into("<I", sb, 20, 0)        # atime
        struct.pack_into("<I", sb, 24, 0)        # flags
        struct.pack_into("<H", sb, 28, SB_VERSION1)   # version1
        struct.pack_into("<H", sb, 30, SB_VERSION2)   # version2
        # volumeid[16] 32..47 = 0
        if VOLID: struct.pack_into("<I", sb, 32, VOLID)
        struct.pack_into("<I", sb, 48, BS)       # blocksize
        struct.pack_into("<I", sb, 52, num_inodes + INODE_SLACK)
        struct.pack_into("<I", sb, 56, (int(FREE_INODES) if FREE_INODES else INODE_SLACK))  # free_inodes
        struct.pack_into("<I", sb, 60, num_blocks)
        struct.pack_into("<I", sb, 64, free_blocks)
        ag = 1 if SB_ALLOCGROUP == 0 else (num_blocks if SB_ALLOCGROUP < 0 else SB_ALLOCGROUP)
        struct.pack_into("<I", sb, 68, ag)       # allocgroup = system-area size (2*SYS)
        off = 72
        sb[off:off+80]   = pack_rootnode(inode_size, inode_ptr, inode_levels, RNODE_MODE); off+=80
        sb[off:off+80]   = pack_rootnode(bm_size,    bm_ptr,    bm_levels, RNODE_MODE);    off+=80
        sb[off:off+80]   = pack_rootnode(long_size,  long_ptr,  long_levels, RNODE_MODE);  off+=80
        sb[off:off+80]   = pack_rootnode(0, [UNUSED]*16, 0, RNODE_MODE); off+=80  # Empty inode-reclaim tree
        ck = crc32_be(0, bytes(sb[8:512]))
        struct.pack_into("<I", sb, 4, ck)
        return bytes(sb)

    f.seek(BOOT);                              f.write(make_sb(2 if SB1_ACTIVE else 1))  # SB#1 @0x2000
    f.seek((num_blocks + SBLKS_OFF) * BS);     f.write(make_sb(1 if SB1_ACTIVE else 2))  # SB#2
    f.flush(); f.close()
    print(f"[mkqnx6fs] {outpath}: inodes={num_inodes} data_blocks={num_blocks} "
          f"used={used_total} free={free_blocks} long_names={len(qb.long_names)} "
          f"size={total_disk_blocks*BS} ({total_disk_blocks*BS/1024/1024:.1f}MB)")
    return total_disk_blocks * BS

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: mkqnx6fs.py <srcdir> <out.img> [slack_blocks] [partition_blocks]")
        sys.exit(1)
    src, out = sys.argv[1], sys.argv[2]
    slack = int(sys.argv[3]) if len(sys.argv) > 3 else 512
    pblk = int(sys.argv[4]) if len(sys.argv) > 4 else None
    build(src, out, slack, pblk)
