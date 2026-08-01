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
DATA = tempfile.mkdtemp(prefix='ftsdiag_')
DP, CP, HP = 28815, 28816, 28817
logf = open('/tmp/ftsdiag.log', 'w', buffering=1)  # noqa: SIM115  # held open for subprocess
proc = subprocess.Popen([BIN, '-d', DATA, '--http-port', str(HP), '--driver-port', str(DP),
    '--cluster-port', str(CP), '--io-threads', '4', '--no-update-check'],
    stdout=logf, stderr=subprocess.STDOUT)
deadline = time.time() + 30
conn = None
while time.time() < deadline:
    try:
        conn = r.r.connect(host='127.0.0.1', port=DP, timeout=2); break
    except Exception: time.sleep(0.5)
db = f'f_{uuid.uuid4().hex[:6]}'
r.r.db_create(db).run(conn)
r.r.db(db).table_create('posts').run(conn)
docs = [
    {'id': 'p1', 'body': 'the quick brown fox'},
    {'id': 'p2', 'body': 'jumps over the lazy dog'},
    {'id': 'p3', 'body': 'quick silver and quick thoughts'},
]
r.r.db(db).table('posts').insert(docs).run(conn)
print('Inserted 3 docs')

# Create FTS index
r.r.db(db).table('posts').index_create('body_idx', r.r.row['body'],
    fts=True, multi=True).run(conn)
r.r.db(db).table('posts').index_wait('body_idx').run(conn)
st = r.r.db(db).table('posts').index_status('body_idx').run(conn)
print('Index status:', st)

# Direct get_all on the FTS index with a token
try:
    res = r.r.db(db).table('posts').get_all('quick', index='body_idx').run(conn)
    print('get_all(quick) via body_idx:', [d['id'] for d in res])
except Exception as e:
    print('get_all ERROR:', type(e).__name__, str(e)[:150])

# fts_match
try:
    res = r.r.db(db).table('posts').fts_match('quick', index='body_idx').run(conn)
    print('fts_match(quick):', [d['id'] for d in res])
except Exception as e:
    print('fts_match ERROR:', type(e).__name__, str(e)[:150])

time.sleep(0.5)
rc = proc.poll()
print(f'Server exit: {rc}')
if rc is not None:
    logf.close()
    with open('/tmp/ftsdiag.log') as f:
        print('--- LOG TAIL ---')
        print(''.join(f.readlines()[-15:]))
proc.kill(); shutil.rmtree(DATA, ignore_errors=True)
