import subprocess,time,os,selectors,signal
from wireless import from_environment as wireless_arguments
def stop_requested(signum, frame):
 raise KeyboardInterrupt
signal.signal(signal.SIGTERM, stop_requested)
from pathlib import Path
root=os.environ.get('MHI2_MEDIA_ROOT', '/home/iscle/Downloads/mhi2-analysis/qemu').rstrip('/')+'/'
Path(root+'tmp').mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env['TMPDIR']=root+'tmp'
cmd=[os.environ.get('QEMU_SYSTEM_ARM','/home/iscle/qemu-mhi2/build/qemu-system-arm'),'-M','mhi2-harman,iram='+os.environ.get('MHI2_IRAM',root+'k3342-70/iram.bin'),'-bios',os.environ.get('MHI2_DEBUG_NOR',root+'k3342-debug-nor.bin'),'-drive','if=sd,format=raw,snapshot='+os.environ.get('MHI2_SNAPSHOT','on')+',file='+os.environ.get('MHI2_EMMC',root+'k3342-70/emmc.raw'),'-display','none','-monitor','none','-qmp','pipe:/tmp/mhi2-debug-qmp','-serial','null','-serial','null','-serial','null','-serial','stdio']
cmd += wireless_arguments()
if os.environ.get('MHI2_OSCILLATOR_12MHZ') == '1':
 cmd += ['-global', 'tegra30-clk.oscillator-12mhz=on']
import json,stat
for name in ['/tmp/mhi2-debug-qmp.in','/tmp/mhi2-debug-qmp.out']:
 if not os.path.exists(name):os.mkfifo(name)
 assert stat.S_ISFIFO(os.stat(name).st_mode)
qin=os.open('/tmp/mhi2-debug-qmp.in',os.O_RDWR|os.O_NONBLOCK)
qout=os.open('/tmp/mhi2-debug-qmp.out',os.O_RDWR|os.O_NONBLOCK)
try:os.read(qout,65536)
except BlockingIOError:pass
glhost = None
if os.environ.get('MHI2_GL'):
 for name in ['/tmp/mhi2-gl.in','/tmp/mhi2-gl.out']:
  if not os.path.exists(name):os.mkfifo(name)
  assert stat.S_ISFIFO(os.stat(name).st_mode)
 glin=os.open('/tmp/mhi2-gl.in',os.O_RDWR)
 glout=os.open('/tmp/mhi2-gl.out',os.O_RDWR)
 glhost=subprocess.Popen([os.environ.get('MHI2_GLHOST', '/tmp/mhi2-glhost')],stdin=glout,stdout=glin,stderr=open('/tmp/mhi2-glhost.log','w'))
 cmd += ['-chardev','pipe,id=glbridge,path=/tmp/mhi2-gl']
rcc_peer = None
pass_fds = ()
if os.environ.get('MHI2_RCC'):
 for name in ['/tmp/mhi2-rcc.in','/tmp/mhi2-rcc.out']:
  if not os.path.exists(name): os.mkfifo(name, 0o600)
  assert stat.S_ISFIFO(os.stat(name).st_mode)
 rcc_in = os.open('/tmp/mhi2-rcc.in', os.O_RDWR)
 rcc_out = os.open('/tmp/mhi2-rcc.out', os.O_RDWR)
 rcc_args = ['python3', str(Path(__file__).with_name('rcc_peer.py'))]
 if os.environ.get('MHI2_RCC_RELOAD') == '1': rcc_args.append('--reload')
 rcc_peer = subprocess.Popen(rcc_args, stdin=rcc_out, stdout=rcc_in, stderr=open('/tmp/mhi2-rcc-peer.log','w'))
 cmd += ['-chardev', 'pipe,id=rcceth,path=/tmp/mhi2-rcc']
most_sink = None
if os.environ.get('MHI2_MOST'):
 Path('/tmp/mhi2-most-blocks.bin').write_bytes(b'')
 Path('/tmp/mhi2-cluster.ppm').unlink(missing_ok=True)
 cmd += ['-chardev','file,id=mostvideo,path=/tmp/mhi2-most-blocks.bin']
 most_sink=subprocess.Popen(['python3',str(Path(__file__).with_name('most_sink.py'))],stderr=open('/tmp/mhi2-most.log','w'),stdout=open('/tmp/mhi2-most-status.log','w'))
p=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=open('/tmp/mhi2-debug-host.log','w'),env=env,bufsize=0,pass_fds=pass_fds)
os.write(qin,b'{"execute":"qmp_capabilities"}\n');nextshot=time.monotonic()+10
sel=selectors.DefaultSelector();sel.register(p.stdout,selectors.EVENT_READ);buf=b'';sent=False
serial_pending=bytearray();serial_next=0.0;failed=False
command_fd = None
command_path = os.environ.get('MHI2_COMMAND_FIFO')
if command_path:
 if not os.path.exists(command_path): os.mkfifo(command_path, 0o600)
 assert stat.S_ISFIFO(os.stat(command_path).st_mode)
 command_fd = os.open(command_path, os.O_RDWR | os.O_NONBLOCK)
with open('/tmp/mhi2-debug-console.log','wb') as log:
 try:
  deadline=time.monotonic()+float(os.environ.get('MHI2_TIMEOUT','100'))
  while time.monotonic()<deadline:
   for name,helper in [('graphics bridge',glhost),('RCC peer',rcc_peer),('MOST receiver',most_sink)]:
    if helper is not None and helper.poll() is not None:
     print(name+' exited unexpectedly with status '+str(helper.returncode),flush=True)
     failed=True;break
   if failed:break
   if command_fd is not None and sent:
    try:
     command = os.read(command_fd, 4096)
     if command: serial_pending.extend(command)
    except BlockingIOError: pass
   if time.monotonic()>nextshot:
    os.write(qin,(json.dumps({'execute':'screendump','arguments':{'filename':'/tmp/mhi2-frame.ppm'}})+'\n').encode());nextshot=time.monotonic()+10
   try:os.read(qout,65536)
   except BlockingIOError:pass
   if serial_pending and time.monotonic() >= serial_next:
    p.stdin.write(serial_pending[:16]);del serial_pending[:16]
    serial_next=time.monotonic()+0.005
   if not sel.select(0.005 if serial_pending else 0.05):continue
   data=os.read(p.stdout.fileno(),65536)
   if not data:break
   log.write(data);log.flush();buf+=data
   if b'MHI2_DIAGNOSTIC_SHELL' in buf and not sent:
    serial_pending.extend(Path('/tmp/mhi2-diag-commands.txt').read_bytes()+b'\n');sent=True
   if sent and b'END_DIAG\r\n# ' in buf:break
 except KeyboardInterrupt:
  pass
 finally:
  p.terminate()
  try:p.wait(timeout=5)
  except subprocess.TimeoutExpired:p.kill();p.wait()
for helper in (most_sink,glhost,rcc_peer):
 if helper is not None:
  helper.terminate()
  try:helper.wait(timeout=8)
  except subprocess.TimeoutExpired:helper.kill();helper.wait()
print(buf.decode(errors='replace')[-16000:])

import shutil
saved=Path(root)/'runs'/time.strftime('%Y%m%d-%H%M%S')
saved.mkdir(parents=True,exist_ok=True)
for f in ['mhi2-debug-console.log','mhi2-debug-host.log','mhi2-glhost.log', 'mhi2-rcc-peer.log', 'mhi2-rcc.pcap', 'mhi2-most.log', 'mhi2-most-status.log', 'mhi2-most-blocks.bin']:
 if Path('/tmp',f).exists():shutil.copy2(Path('/tmp',f),saved/f)
print('Full logs:',saved)

if failed:raise SystemExit(1)
