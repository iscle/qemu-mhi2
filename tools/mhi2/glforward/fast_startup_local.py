import sys
from pathlib import Path
sys.path.insert(0,'/home/iscle/Documents/mib_zr');from qnx6read import Qnx6
p=Path('/home/iscle/Downloads/mhi2-analysis/qemu/emmc-gl.raw');fs=Qnx6(str(p),1048576);ino=1
for name in ('eso','hmi','lsd','lsd.sh'):ino=dict(fs.listdir(ino))[name]
node=fs.inode(ino);s=fs.read_file(ino).replace(b'-DdsiTimeout=20000 -DdomainTimeout=120000',b'-DdsiTimeout=1000 -DdomainTimeout=5000')
assert len(s)<=node['size'];s+=b' '*(node['size']-len(s))
with p.open('r+b') as f:
 for i,off in enumerate(range(0,len(s),fs.bs)):
  f.seek(fs.base+(fs._bmap(node,i)+fs.sblks_off)*fs.bs);f.write(s[off:off+fs.bs])
