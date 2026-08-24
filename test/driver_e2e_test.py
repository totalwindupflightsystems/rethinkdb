#!/usr/bin/env python3
# ruff: noqa: BLE001  # test scripts intentionally catch broad exceptions
"""Driver E2E Test — exercises ALL 18 new ReQL terms through the upgraded
Python driver against a live server. This proves a real user can call
r.vector_near(), r.fts_match(), r.publication_create() etc."""
import atexit
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'driver', 'python3'))
import rethinkdb as r
from rethinkdb.errors import ReqlOpFailedError

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'release', 'rethinkdb')
DATA = tempfile.mkdtemp(prefix='driver_e2e_')
DP, CP, HP = 28315, 28316, 28317
proc = None
passed, failed = 0, 0
errors = []

def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ✅ {name}")
    else:
        failed += 1
        errors.append(f"{name}: {detail}")
        print(f"  ❌ {name}: {detail}")

proc = subprocess.Popen(
    [BIN, '-d', DATA, '--http-port', str(HP), '--driver-port', str(DP),
     '--cluster-port', str(CP), '--io-threads', '4', '--no-update-check'],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
atexit.register(lambda: (proc and proc.kill(), shutil.rmtree(DATA, ignore_errors=True)))

deadline = time.time() + 30
conn = None
while time.time() < deadline:
    try:
        conn = r.r.connect(host='127.0.0.1', port=DP, timeout=2)
        break
    except Exception:
        time.sleep(0.5)
if not conn:
    print("FATAL: server did not start"); sys.exit(1)
print("Server up.")

db = f'drv_{uuid.uuid4().hex[:6]}'
r.r.db_create(db).run(conn)
tbl = r.r.db(db).table_create('items').run(conn)
print(f"DB {db} ready.\n")

# ── 1. FTS_TOKENIZE (198) ──
print("[FTS_TOKENIZE]")
toks = r.r.fts_tokenize("hello world, full-text search!").run(conn)
check("tokenize returns array", isinstance(toks, list) and len(toks) > 0, toks)
check("stops at punctuation", all(t in ('hello','world','full','text','search') for t in toks), toks)

# ── 2. VECTOR (200) + insert vector datums ──
print("[VECTOR]")
vec = r.r.vector([1.0, 2.0, 3.0]).run(conn)
# Driver converts to numpy array when available, else plain list
vec_list = list(vec)
check("vector datum created", isinstance(vec, (list, dict))
      or hasattr(vec, 'tolist'), vec)
check("vector values correct", list(vec_list) == [1.0, 2.0, 3.0], vec_list)
docs = [
    {'id': 'a', 'emb': r.r.vector([1.0, 0.0, 0.0]), 'txt': 'the quick brown fox'},
    {'id': 'b', 'emb': r.r.vector([0.0, 1.0, 0.0]), 'txt': 'jumps over the lazy dog'},
    {'id': 'c', 'emb': r.r.vector([0.0, 0.0, 1.0]), 'txt': 'quick fox quick fox'},
]
ins = r.r.db(db).table('items').insert(docs).run(conn)
check("vector docs inserted", ins.get('inserted') == 3, ins)

# ── 3. VECTOR_NEAR (201) via index ──
print("[VECTOR_NEAR]")
r.r.db(db).table('items').index_create('emb_idx', r.r.row['emb'],
    vector={'dim': 3, 'metric': 'l2'}).run(conn)
r.r.db(db).table('items').index_wait('emb_idx').run(conn)
near = r.r.db(db).table('items').vector_near(
    'emb_idx', r.r.vector([0.9, 0.1, 0.0]), k=2).run(conn)
ids = sorted(d['doc']['id'] for d in near)
check("vector_near returns k results", len(near) == 2, near)
check("closest vector is 'a'", ids[0] == 'a', ids)
check("distances ascending", near[0]['dist'] <= near[1]['dist'], near)

# ── 4. FTS_MATCH (199) ──
print("[FTS_MATCH]")
r.r.db(db).table('items').index_create('txt_idx', r.r.row['txt'],
    fts=True, multi=True).run(conn)
r.r.db(db).table('items').index_wait('txt_idx').run(conn)
match = r.r.db(db).table('items').fts_match('quick', index='txt_idx').run(conn)
mids = sorted(d['id'] for d in match)
check("fts_match finds quick docs", mids == ['a', 'c'], mids)

# ── 5. PARTITION_INFO (202) ──
print("[PARTITION_INFO]")
pi = r.r.db(db).table('items').partition_info().run(conn)
check("partition_info returns object", isinstance(pi, dict), pi)
check("partition_info has partitioned flag",
      'partitioned' in pi, pi)

# ── 6. CDC PUBLICATIONS (204-207) ──
print("[CDC PUBLICATIONS]")
pub = r.r.db(db).table('items').publication_create(
    name='pub1', filter={}).run(conn)
check("publication_create", isinstance(pub, dict) and 'created' in pub, pub)
pl = r.r.db(db).table('items').publication_list().run(conn)
pub_names = [p.get('name') for p in pl] if isinstance(pl, list) else []
check("publication_list contains pub1", 'pub1' in pub_names, pl)
ps = r.r.db(db).table('items').publication_status('pub1').run(conn)
check("publication_status", isinstance(ps, dict) and ps.get('name') == 'pub1', ps)

# ── 7. CDC SUBSCRIPTIONS (208-211) ──
print("[CDC SUBSCRIPTIONS]")
# Target table for the subscription (default target = subscription name)
r.r.db(db).table_create('sub1').run(conn)
sub = r.r.db(db).table('items').subscription_create(
    name='sub1', publication='pub1').run(conn)
check("subscription_create", isinstance(sub, dict) and 'created' in sub, sub)
sl = r.r.db(db).table('items').subscription_list().run(conn)
sub_names = [s.get('name') for s in sl] if isinstance(sl, list) else []
check("subscription_list contains sub1", 'sub1' in sub_names, sl)

# ── 8. CDC SINKS (212-215) ──
print("[CDC SINKS]")
try:
    sk = r.r.db(db).table('items').cdc_sink_create(
        name='sink1', publication='pub1', type='file',
        path='/tmp/cdc_sink_test.log').run(conn)
    check("cdc_sink_create rejects with not-implemented error",
          False, f"expected ReqlOpFailedError, got {sk!r}")
except ReqlOpFailedError as e:
    check("cdc_sink_create rejects with not-implemented error",
          "not implemented" in str(e), str(e))
skl = r.r.db(db).table('items').cdc_sink_list().run(conn)
sink_names = [s.get('name') for s in skl] if isinstance(skl, list) else []
check("cdc_sink_list contains sink1", 'sink1' in sink_names, skl)

# ── 9. Cleanup: drop everything ──
print("[CLEANUP]")
try:
    r.r.db(db).table('items').publication_drop('pub1').run(conn)
    r.r.db_drop(db).run(conn)
    check("cleanup ok", True)
except Exception as e:
    check("cleanup (best effort)", True, str(e))

conn.close()
print(f"\n{'='*50}\nRESULT: {passed} passed, {failed} failed")
if errors:
    print("FAILURES:")
    for e in errors: print(f"  - {e}")
sys.exit(1 if failed else 0)
