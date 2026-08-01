#!/usr/bin/env python3
# ruff: noqa: BLE001  # test scripts intentionally catch broad exceptions
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, '/tmp')
import rethinkdb as r

BIN = '/home/kara/rethinkdb/build/release/rethinkdb'
DATA = tempfile.mkdtemp(prefix='partdiag_')
DP, CP, HP = 29015, 29016, 29017
logf = open('/tmp/partdiag.log', 'w', buffering=1)  # noqa: SIM115  # held open for subprocess
proc = subprocess.Popen([BIN, '-d', DATA, '--http-port', str(HP), '--driver-port', str(DP),
    '--cluster-port', str(CP), '--io-threads', '4', '--no-update-check'],
    stdout=logf, stderr=subprocess.STDOUT)
deadline = time.time() + 30
conn = None
while time.time() < deadline:
    try:
        conn = r.r.connect(host='127.0.0.1', port=DP, timeout=2); break
    except Exception: time.sleep(0.5)
db = f'p_{uuid.uuid4().hex[:6]}'
r.r.db_create(db).run(conn)
r.r.db(db).table_create('t').run(conn)
print('Table created. Calling partition_info...')
try:
    pi = r.r.db(db).table('t').partition_info().run(conn)
    print('partition_info:', pi)
except Exception as e:
    print('QUERY ERROR:', type(e).__name__, str(e)[:150])
time.sleep(1)
rc = proc.poll()
print(f'Server exit: {rc}')
if rc is not None:
    logf.close()
    with open('/tmp/partdiag.log') as f:
        lines = f.readlines()
        print('--- LOG TAIL ---')
        print(''.join(lines[-20:]))
proc.kill(); shutil.rmtree(DATA, ignore_errors=True)
