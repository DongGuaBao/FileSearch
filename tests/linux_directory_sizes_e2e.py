#!/usr/bin/env python3
"""Privileged, isolated FileSearch size integration. Never scans outside the fixture."""
import argparse,json,os,pathlib,socket,struct,subprocess,tempfile,time,shutil,sqlite3
parser=argparse.ArgumentParser();parser.add_argument('binary');parser.add_argument('--base-dir',required=True);args=parser.parse_args()
if os.geteuid()!=0:
 print('SKIP: fanotify integration requires root');raise SystemExit(77)
base=pathlib.Path(args.base_dir);base.mkdir(parents=True,exist_ok=True)
fixture=pathlib.Path(tempfile.mkdtemp(prefix='filesearch-sizes-',dir=base));root=fixture/'tree';state=fixture/'state';root.mkdir();state.mkdir(mode=0o700);process=None;log=None

def request(body):
 with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as connection:
  connection.settimeout(3);connection.connect(str(state/'search.sock'));payload=json.dumps(body).encode();connection.sendall(struct.pack('<I',len(payload))+payload)
  def receive(count):
   data=b''
   while len(data)<count:
    block=connection.recv(count-len(data))
    if not block:raise RuntimeError('closed IPC connection')
    data+=block
   return data
  length=struct.unpack('<I',receive(4))[0];assert length<=1048576
  result=json.loads(receive(length));assert result.get('ok'),result;return result

def wait(check,timeout=25):
 end=time.monotonic()+timeout;last=None
 while time.monotonic()<end:
  try:
   result=check()
   if result:return result
  except (OSError,AssertionError,KeyError,RuntimeError) as error:last=error
  if process.poll() is not None:raise AssertionError('service exited: '+(fixture/'service.log').read_text())
  time.sleep(.1)
 raise AssertionError('timed out: '+str(last))

def sizes(*paths):return request({'command':'directory_sizes','paths':[str(p) for p in paths]})['entries']
def expect_size(path,value):
 result=sizes(path)[0];return result['state']=='ready' and result.get('logical_bytes')==value and result['complete']
def start(periodic="2"):
 global process,log
 log=open(fixture/'service.log','ab');environment=dict(os.environ,FILESEARCH_SCAN_BATCH='2',FILESEARCH_SCAN_PAUSE_MS='2',FILESEARCH_RECONCILE_SECONDS=periodic)
 process=subprocess.Popen([str(pathlib.Path(args.binary).resolve()),str(root),str(state)],env=environment,stdout=log,stderr=log)
 wait(lambda:(state/'search.sock').exists())
def stop():
 global process,log
 if process and process.poll() is None:
  process.terminate()
  try:process.wait(timeout=10)
  except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
 if log:log.close()
try:
 (root/'a').mkdir();(root/'b').mkdir();(root/'a'/'sub').mkdir();(root/'a'/'sub'/'file').write_bytes(b'a'*100)
 (root/'a'/'zero').touch();os.symlink(root/'a',root/'link');start();wait(lambda:expect_size(root,100));assert expect_size(root/'b',0)
 # An open file must update before close (FAN_MODIFY / bounded calibration).
 with open(root/'a'/'sub'/'file','ab',buffering=0) as opened:
  opened.write(b'b'*200);wait(lambda:expect_size(root/'a',300))
 # Hardlinks count per pathname, and one inode write updates both aliases.
 os.link(root/'a'/'sub'/'file',root/'b'/'hardlink');wait(lambda:expect_size(root,600))
 with open(root/'b'/'hardlink','wb') as opened:opened.write(b'c'*50)
 wait(lambda:expect_size(root/'a',50) and expect_size(root/'b',50) and expect_size(root,100))
 (root/'a'/'sub').rename(root/'b'/'sub');wait(lambda:expect_size(root/'a',0) and expect_size(root/'b',100))
 # Atomic replace must carry pending metadata across the rename.
 (root/'b'/'temporary').write_bytes(b'd'*1234);os.replace(root/'b'/'temporary',root/'b'/'hardlink');wait(lambda:expect_size(root/'b',1284))
 (root/'b'/'sub'/'file').unlink();(root/'b'/'sub').rmdir();wait(lambda:expect_size(root,1234))
 # Search uses the same indexed namespace and supports scoped queries.
 def search_file():
  result=request({'command':'search','query':'hardlink','scope_path':str(root/'b'),'compact':True})
  return any(item['path']==str(root/'b'/'hardlink') for item in result['results'])
 wait(search_file)
 def durable_size():
  with sqlite3.connect(state/'directory-sizes.db') as database:
   row=database.execute('SELECT logical,complete FROM sizes WHERE path=?',(str(root).encode(),)).fetchone()
   return row==(1234,1)
 wait(durable_size)
 # Restart must load durable metadata and reconcile offline mutations.
 stop();(root/'b'/'hardlink').write_bytes(b'e'*444);start();wait(lambda:expect_size(root,444))
 excluded=sizes(state)[0];assert excluded['state']=='excluded' and excluded['logical_bytes'] is None
 wait(lambda:request({'command':'status'})['ready'])
 # Pause the catalog in a slow scan while > kernel queue capacity changes
 # happen elsewhere on the watched filesystem. The independent reader must
 # keep draining; unknown out-of-scope parents must not cause overflow loops.
 stop();seed=root/'seed';seed.mkdir()
 for i in range(3000):(seed/str(i)).touch()
 noise=fixture/'noise';noise.mkdir();start(periodic='0')
 wait(lambda:not request({'command':'status'}).get('initializing',False))
 # Replacements queued during the baseline must converge locally.
 for i in range(100):
  (root/'replace-temp').write_bytes(b'x'*(i+1));os.replace(root/'replace-temp',root/'replace-live')
 for i in range(20000):(noise/str(i)).touch()
 status=wait(lambda:(lambda result:result if result.get('ready') else None)(request({'command':'status'})),timeout=40)
 assert status['fanotify_overflows']==0 and status['scan_retries']==0,status
 wait(lambda:expect_size(root,544),timeout=40)
 print('PASS live directory sizes: open writes, hardlinks, moves, atomic replacement, delete, durable cache, scoped search, restart, scan-time replacements/backlog without global retries and exclusions')
finally:
 stop();shutil.rmtree(fixture)
