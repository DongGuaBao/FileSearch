#!/usr/bin/env python3
"""Queries must stay responsive while the catalog database is locked at startup."""
import argparse,json,pathlib,socket,sqlite3,struct,subprocess,tempfile,time,shutil
parser=argparse.ArgumentParser();parser.add_argument('binary');parser.add_argument('--base-dir',required=True);args=parser.parse_args()
base=pathlib.Path(args.base_dir);base.mkdir(parents=True,exist_ok=True)
fixture=pathlib.Path(tempfile.mkdtemp(prefix='fsboot-',dir=base));root=fixture/'tree';root.mkdir();state=fixture/'state';state.mkdir(mode=0o700)
connection=sqlite3.connect(state/'catalog.db');connection.execute('CREATE TABLE entries(path BLOB PRIMARY KEY,name BLOB,inode INTEGER,device INTEGER,directory INTEGER,symlink INTEGER) WITHOUT ROWID');connection.commit();connection.execute('BEGIN EXCLUSIVE')
cache=sqlite3.connect(state/'directory-sizes.db');cache.executescript('CREATE TABLE metadata(root BLOB); CREATE TABLE sizes(path BLOB PRIMARY KEY,logical INTEGER,allocated INTEGER,files INTEGER,unknown INTEGER,checked INTEGER,complete INTEGER) WITHOUT ROWID;');cache.execute('INSERT INTO metadata VALUES(?)',(str(root).encode(),));cache.execute('INSERT INTO sizes VALUES(?,?,?,?,?,?,?)',(str(root).encode(),777,4096,1,0,1234,1));cache.commit();cache.close()
process=None;log=open(fixture/'service.log','wb')
def request(body):
 with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as client:
  client.settimeout(1);client.connect(str(state/'search.sock'));payload=json.dumps(body).encode();client.sendall(struct.pack('<I',len(payload))+payload)
  def read(n):
   data=b''
   while len(data)<n:
    part=client.recv(n-len(data))
    if not part:raise AssertionError('IPC closed')
    data+=part
   return data
  size=struct.unpack('<I',read(4))[0];assert size<=1048576;return json.loads(read(size))
try:
 start=time.monotonic();process=subprocess.Popen([str(pathlib.Path(args.binary).resolve()),str(root),str(state)],stdout=log,stderr=log)
 while not (state/'search.sock').exists():
  assert time.monotonic()-start<1.5,'IPC blocked by catalog initialization';assert process.poll() is None,'service exited';time.sleep(.01)
 status=request({'command':'status'});assert status['initializing'] and not status['ready'],status
 sizes=request({'command':'directory_sizes','paths':[str(root)]});entry=sizes['entries'][0];assert entry['logical_bytes']==777 and entry['state']=='stale' and entry['complete'],entry
 invalid=request({'command':'directory_sizes','paths':[str(root/'../outside')]});assert not invalid['ok'],invalid
 result=request({'command':'search','query':'report','scope_path':str(root),'compact':True});assert result['ok'] and result['initializing'] and result['partial'],result
 print('PASS IPC and persistent directory sizes available while catalog initialization is blocked; bounded startup search')
finally:
 if process and process.poll() is None:process.terminate()
 connection.rollback();connection.close()
 if process:
  try:process.wait(timeout=10)
  except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
 log.close();shutil.rmtree(fixture)
