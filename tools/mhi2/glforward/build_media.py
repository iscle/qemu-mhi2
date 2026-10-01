from pathlib import Path
import ctypes,ctypes.util,json,struct,sys,subprocess,os
from ifs_resource import add_file
root=Path('/home/iscle/Downloads/mhi2-analysis/qemu')
raw=bytearray((root/'main-ifs/imagefs.bin').read_bytes())
for entry in json.loads((root/'main-ifs/files.json').read_text()):
 name=entry['name']
 if name in ('lib/libGLESv2.so','lib/libEGL.so','lib/libnvvsenc.so'):
  data=Path('/tmp/mhi2-'+name[4:]).read_bytes();off,n=entry['offset'],entry['size'];assert len(data)<=n
  raw[off:off+n]=data+bytes(n-len(data));print(name,len(data),n,flush=True)
raw=add_file(raw,'sbin/mhi2-pcm-endpoint',Path('/tmp/mhi2-pcm-endpoint').read_bytes())
lib=ctypes.CDLL(ctypes.util.find_library('lzo2'));work=ctypes.create_string_buffer(16*1024*1024)
block=2*1024*1024;out=bytearray(0x800);out[:8]=b'LZOZ'+struct.pack('<I',block)
for i,off in enumerate(range(0,len(raw),block)):
 d=bytes(raw[off:off+block]);buf=ctypes.create_string_buffer(len(d)+len(d)//16+128);n=ctypes.c_size_t(len(buf))
 r=lib.lzo1z_999_compress(d,ctypes.c_size_t(len(d)),buf,ctypes.byref(n),work);assert r==0
 check=ctypes.create_string_buffer(block);cn=ctypes.c_size_t(block)
 assert lib.lzo1z_decompress_safe(buf,n,check,ctypes.byref(cn),None)==0 and check.raw[:cn.value]==d
 struct.pack_into('<I',out,8+4*i,n.value);out+=buf.raw[:n.value];out+=bytes((-len(out))%512)
 print(i,n.value,flush=True)
(root/'main-ifs-gl.lzo').write_bytes(out)
nor=bytearray((root/'k3342-full-debug-nor.bin').read_bytes());assert len(out)<0x2a00000
nor[0xa60000:0x3460000]=out+b'\xff'*(0x2a00000-len(out))
# Preserve the guest-written English default across experimental media rebuilds.
# This is a QNX flash filesystem, not a patched language string in the HMI.
language_seed=root/'default-english-persist.bin'
navigation_seed=root/'navigation-enabled-persist.bin'
if navigation_seed.exists():
 language_seed=navigation_seed
navigation_defaults_seed=root/'navigation-defaults-persist.bin'
if navigation_defaults_seed.exists():
 language_seed=navigation_defaults_seed
if language_seed.exists():
 seed=language_seed.read_bytes();assert len(seed)==0x800000
 nor[0x3800000:0x4000000]=seed
(root/'k3342-gl-debug-nor.bin').write_bytes(nor)
disk=Path(os.environ.get('MHI2_EMMC',str(root/'emmc-gl.raw')))
if not disk.exists():subprocess.run(['cp','--reflink=auto','--sparse=always',str(root/'emmc-full.raw'),str(disk)],check=True)
sys.path.insert(0,'/home/iscle/Documents/mib_zr');from qnx6read import Qnx6
fs=Qnx6(str(disk),2048*512);ino=1
for name in ('img_restore','main_stage2.ifs.lzo'):ino=dict(fs.listdir(ino))[name]
node=fs.inode(ino);print('file',ino,node['size'],'new',len(out),flush=True);assert len(out)<=node['size']
out+=bytes(node['size']-len(out))
with disk.open('r+b') as f:
 for i,off in enumerate(range(0,len(out),fs.bs)):
  phys=fs._bmap(node,i);assert phys is not None
  f.seek(fs.base+(phys+fs.sblks_off)*fs.bs);f.write(out[off:off+fs.bs])
print('DONE',flush=True)
