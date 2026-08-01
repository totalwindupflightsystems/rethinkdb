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
DATA = tempfile.mkdtemp(prefix='bfill_')
DP, CP, HP = 28915, 28916, 28917
proc = subprocess.Popen([BIN, '-d', DATA, '--http-port', str(HP), '--driver-port', str(DP),
    '--cluster-port', str(CP), '--io-threads', '4', '--no-update-check'],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
deadline = time.time() + 30
conn = None
while time.time() < deadline:
    try:
        conn = r.r.connect(host='127.0.0.1', port=DP, timeout=2); break
    except Exception: time.sleep(0.5)
db = f'b_{uuid.uuid4().hex[:6]}'
r.r.db_create(db).run(conn)
r.r.db(db).table_create('t').run(conn)
docs = [{'id': f'd{i}', 'val': i % 5, 'tag': f't{i % 3}'} for i in range(20)]
r.r.db(db).table('t').insert(docs).run(conn)
print('Inserted 20 docs BEFORE index creation')

# PLAIN index on existing data
r.r.db(db).table('t').index_create('val_idx', r.r.row['val']).run(conn)
r.r.db(db).table('t').index_wait('val_idx').run(conn)
try:
    res = list(r.r.db(db).table('t').get_all(3, index='val_idx').run(conn))
    print('PLAIN index get_all(3):', len(res), 'docs (expect 4)')
except Exception as e:
    print('PLAIN ERROR:', type(e).__name__, str(e)[:120])

# MULTI index on existing data
r.r.db(db).table('t').index_create('tag_idx', r.r.row['tag'], multi=True).run(conn)
r.r.db(db).table('t').index_wait('tag_idx').run(conn)
try:
    res = list(r.r.db(db).table('t').get_all('t1', index='tag_idx').run(conn))
    print('MULTI index get_all(t1):', len(res), 'docs (expect ~7)')
except Exception as e:
    print('MULTI ERROR:', type(e).__name__, str(e)[:120])

# Sanity: table scan works
res = r.r.db(db).table('t').count().run(conn)
print('Table count:', res)

proc.kill(); shutil.rmtree(DATA, ignore_errors=True)
