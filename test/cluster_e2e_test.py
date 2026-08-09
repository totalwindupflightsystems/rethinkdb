#!/usr/bin/env python3
# ruff: noqa: BLE001  # test scripts intentionally catch broad exceptions
"""3-node cluster E2E — verifies the Phase-3 features survive real cluster
semantics: vector search, FTS, CDC publications/subscriptions/sinks, and
partition_info on a 3-node RethinkDB cluster, including a failover check."""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'driver', 'python3'))  # vendored upgraded driver
import rethinkdb as r  # noqa: E402

BIN = '/home/kara/rethinkdb/build/release/rethinkdb'

passed = 0
failed = 0
errors = []


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ✅ {name}")
    else:
        failed += 1
        errors.append(name)
        print(f"  ❌ {name}: {detail}")


# ── Boot 3 nodes ──


def main():

    dirs = []
    procs = []
    base_http = 29115
    base_driver = 29215
    base_cluster = 29315
    for i in range(3):
        d = tempfile.mkdtemp(prefix=f'cl{i}_')
        dirs.append(d)
        cmd = [BIN, '-d', d, '--http-port', str(base_http + i),
               '--driver-port', str(base_driver + i),
               '--cluster-port', str(base_cluster + i),
               '--io-threads', '4', '--no-update-check']
        if i > 0:
            cmd.append('--join')
            cmd.append(f'127.0.0.1:{base_cluster}')
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        procs.append(p)

    # Wait for all 3 to be reachable & cluster formed
    conn = None
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            conn = r.r.connect(host='127.0.0.1', port=base_driver, timeout=2)
            servers = r.r.db('rethinkdb').table('server_config').count().run(conn)
            if servers >= 2:
                break
        except Exception:
            pass
        time.sleep(1)

    try:
        servers = r.r.db('rethinkdb').table('server_config').count().run(conn)
        check(f"cluster formed ({servers} servers)", servers >= 2, servers)
    except Exception as e:
        check("cluster formed", False, str(e)[:100])
        # kill everything and bail
        for p in procs:
            p.kill()
        for d in dirs:
            shutil.rmtree(d, ignore_errors=True)
        sys.exit(1)

    db = f'cl_{uuid.uuid4().hex[:6]}'
    r.r.db_create(db).run(conn)

    # ── Vector across cluster ──
    print("[CLUSTER VECTOR]")
    r.r.db(db).table_create('vecs', shards=2, replicas=2).run(conn)
    docs = [{'id': 'v%d' % i, 'emb': r.r.vector([float(i), 0.0, 0.0])} for i in range(20)]
    r.r.db(db).table('vecs').insert(docs).run(conn)
    r.r.db(db).table('vecs').index_create('emb_idx', r.r.row['emb'],
        vector={'dim': 3, 'metric': 'l2'}).run(conn)
    r.r.db(db).table('vecs').index_wait('emb_idx').run(conn)
    near = list(r.r.db(db).table('vecs').vector_near(
        'emb_idx', r.r.vector([0.5, 0.0, 0.0]), k=3).run(conn))
    check("vector_near across 2-shard table", len(near) == 3, near)
    check("closest is v0 or v1", near[0]['doc']['id'] in ('v0', 'v1'), near)

    # ── FTS across cluster ──
    print("[CLUSTER FTS]")
    r.r.db(db).table_create('posts', shards=2, replicas=2).run(conn)
    r.r.db(db).table('posts').insert([
        {'id': 'p1', 'body': 'the quick brown fox'},
        {'id': 'p2', 'body': 'lazy dog sleeps'},
    ]).run(conn)
    r.r.db(db).table('posts').index_create('body_idx', r.r.row['body'],
        fts=True, multi=True).run(conn)
    r.r.db(db).table('posts').index_wait('body_idx').run(conn)
    hits = list(r.r.db(db).table('posts').fts_match('quick', index='body_idx').run(conn))
    check("fts_match across cluster", [d['id'] for d in hits] == ['p1'], hits)

    # ── CDC across cluster ──
    print("[CLUSTER CDC]")
    r.r.db(db).table_create('items', shards=2, replicas=2).run(conn)
    r.r.db(db).table('items').insert([{'id': 'i1', 'x': 1}]).run(conn)
    pub = r.r.db(db).table('items').publication_create(
        name='pub1', filter={}).run(conn)
    check("publication_create on cluster", isinstance(pub, dict) and 'created' in pub, pub)
    pl = r.r.db(db).table('items').publication_list().run(conn)
    check("publication_list reflects Raft metadata",
          any(p.get('name') == 'pub1' for p in pl), pl)

    r.r.db(db).table_create('sub_target').run(conn)
    sub = r.r.db(db).table('items').subscription_create(
        name='sub1', publication='pub1', targetTable='sub_target').run(conn)
    check("subscription_create on cluster", isinstance(sub, dict) and 'created' in sub, sub)
    sl = r.r.db(db).table('items').subscription_list().run(conn)
    check("subscription_list reflects Raft metadata",
          any(s.get('name') == 'sub1' for s in sl), sl)

    sk = r.r.db(db).table('items').cdc_sink_create(
        name='sink1', publication='pub1', type='file', path='/tmp/cl_sink.log').run(conn)
    check("cdc_sink_create on cluster", isinstance(sk, dict) and 'created' in sk, sk)
    skl = r.r.db(db).table('items').cdc_sink_list().run(conn)
    check("cdc_sink_list reflects Raft metadata",
          any(s.get('name') == 'sink1' for s in skl), skl)

    # ── Partition info ──
    print("[CLUSTER PARTITION]")
    pi = r.r.db(db).table('items').partition_info().run(conn)
    check("partition_info on cluster", isinstance(pi, dict) and 'partitioned' in pi, pi)

    # ── Failover: kill node 2 (if 3 servers), verify reads still work ──
    if servers >= 3:
        print("[CLUSTER FAILOVER]")
        procs[2].kill()
        time.sleep(8)
        try:
            c2 = r.r.connect(host='127.0.0.1', port=base_driver + 1, timeout=5)
            n = r.r.db(db).table('vecs').count().run(c2)
            check(f"read after node failure ({n} docs)", n == 20, n)
            c2.close()
        except Exception as e:
            check("read after node failure", False, str(e)[:100])

    # ── Cleanup ──
    for p in procs:
        try:
            p.kill()
        except Exception:
            pass
    for d in dirs:
        shutil.rmtree(d, ignore_errors=True)

    print(f"\n{'=' * 50}")
    print(f"RESULT: {passed} passed, {failed} failed")
    if errors:
        print("FAILURES:")
        for e in errors:
            print(f"  - {e}")
    sys.exit(1 if failed else 0)



if __name__ == "__main__":
    main()
