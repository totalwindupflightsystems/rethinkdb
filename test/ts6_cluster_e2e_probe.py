#!/usr/bin/env python3
"""PHASE3-TS-6 live cluster E2E probe — time-series across a 3-node cluster.

Verifies (spec §6.4 cluster semantics + implementation-order item 8, the
remaining cluster half after the Raft-metadata serialization landed in
TS-1..TS-5):

- a 2-shard time-series table on a real 3-node cluster: chunk index is
  visible via the artificial table_config interface from ANY node
  (chunk_count / total_rows merged across shards)
- between()/count() reads across the cluster (chunk-index read pruning on
  the raw path)
- the downsample background job produces aggregate rows on the cluster;
  the planner's auto-selection returns them for wide windows, and the
  aggregates are REAL values: sum(cnt) == raw rows in the window and
  sum(avg_v * cnt) == the true sum of v over the window
- retention deletes expired chunks cluster-wide (old rows inserted 2h in
  the past with a 3600s retention window; chunk-granular expiry mirroring
  ts4_e2e_probe.py's timing model)
- FAILOVER: SIGKILL one node mid-reads → reads survive on the surviving
  nodes → the killed node rejoins (same data dir, --join) → consistent

Downsample config note: target interval (2s) <= chunk interval (4s), the
regime the spec's own examples use (chunk 1h → 1m). A target interval
LARGER than the chunk interval lets one bucket span two chunks and the
per-chunk merge would overwrite the first chunk's partial aggregate — the
config validator does not reject that combination (see TS-6 finding report).

Timing model: 60 live rows 1s apart starting at an EVEN second (so chunk
boundaries at base+4k align with 2s bucket boundaries base+2k and every
bucket is wholly inside one chunk), then 10 old rows at now-7200. The
window [base, base+40) covers only sealed chunks (rows 40..59 seal the
last window chunk; the active newest chunk is never merged).

Run with the vendored sync driver like ts2/ts3/ts4_e2e_probe.py.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'driver', 'python3'))
from rethinkdb import r  # noqa: E402

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'release', 'rethinkdb')
if not os.path.exists(BIN):
    raise RuntimeError(
        f"rethinkdb binary not found at {BIN}; run `make -j4` "
        "first (see AGENTS.md) before running this probe")
BASE_HTTP = 29615
BASE_DRIVER = 29715
BASE_CLUSTER = 29815

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


def wait_for(pred, timeout=120, interval=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


def server_count(conn):
    return r.db('rethinkdb').table('server_config').count().run(conn)


def table_config(conn, name):
    return (r.db('rethinkdb').table('table_config')
            .filter({'name': name}).nth(0).run(conn))


def chunk_stats(conn, name):
    ci = table_config(conn, name).get('time_series_chunks')
    if not ci:
        return None
    return (ci.get('chunk_count'), ci.get('total_rows'),
            [c['row_count'] for c in ci.get('chunks', [])])


def count_rows(conn, name, db='test'):
    return r.db(db).table(name).count().run(conn)


def boot_node(i, data_dir):
    cmd = [BIN, '-d', data_dir,
           '--http-port', str(BASE_HTTP + i),
           '--driver-port', str(BASE_DRIVER + i),
           '--cluster-port', str(BASE_CLUSTER + i),
           '--io-threads', '4', '--no-update-check']
    if i > 0:
        cmd.append('--join')
        cmd.append(f'127.0.0.1:{BASE_CLUSTER}')
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def main():
    print(f"== TS-6 cluster E2E probe ({time.strftime('%H:%M:%S')}) ==")
    dirs = []
    procs = []
    for i in range(3):
        d = tempfile.mkdtemp(prefix=f'ts6cl{i}_')
        dirs.append(d)
        procs.append(boot_node(i, d))

    conn = None
    try:
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                conn = r.connect(host='127.0.0.1', port=BASE_DRIVER, timeout=2)
                if server_count(conn) >= 3:
                    break
            except Exception:
                pass
            time.sleep(1)
        n_servers = server_count(conn) if conn else 0
        check(f"cluster formed ({n_servers} servers)", n_servers >= 3,
              n_servers)
        if conn is None or n_servers < 3:
            return 1

        db = f'ts6_{uuid.uuid4().hex[:6]}'
        r.db_create(db).run(conn)
        r.db(db).table_create(
            'sensors', shards=2, replicas=2,
            timeSeries={
                'field': 'ts',
                'chunk_interval': 4,
                'retention': 3600,
                'downsample': [{
                    'age': 5,
                    'to': 2,
                    'aggregate': {
                        'avg_v': lambda row: row['v'].avg(),
                        'cnt': lambda row: row.count(),
                    },
                }],
            }).run(conn)
        tc = table_config(conn, 'sensors')
        check('timeSeries config on cluster',
              tc.get('time_series') is not None
              and tc['time_series'].get('field') == 'ts'
              and tc['time_series'].get('chunk_interval') == 4,
              str(tc.get('time_series')))
        check('2 shards configured',
              len(tc.get('shards', [])) == 2, str(tc.get('shards')))

        # ── live rows: 60 rows, 1s apart, starting at an even second ──
        now_s = int(time.time())
        base = now_s - 60
        if base % 2:
            base -= 1
        live = [{'id': f'r{i:02d}', 'ts': r.epoch_time(base + i), 'v': i}
                for i in range(60)]
        res = r.db(db).table('sensors').insert(live).run(conn)
        check('insert 60 live rows across cluster', res.get('inserted') == 60,
              str(res))

        # ── chunk index visible via the artificial table_config ────────
        ci = chunk_stats(conn, 'sensors')
        check('time_series_chunks: total_rows == 60',
              ci is not None and ci[1] == 60,
              f"chunk_count={ci and ci[0]} total_rows={ci and ci[1]}")
        check('chunks formed across shards',
              ci is not None and ci[0] >= 1, f"chunk_count={ci and ci[0]}")
        # Visible from a DIFFERENT node's driver port (cluster interface).
        conn1 = r.connect(host='127.0.0.1', port=BASE_DRIVER + 1, timeout=5)
        ci1 = chunk_stats(conn1, 'sensors')
        check('chunk info visible via node 1', ci1 is not None and ci1[1] == 60,
              f"chunk_count={ci1 and ci1[0]} total_rows={ci1 and ci1[1]}")
        conn1.close()

        # ── raw-path reads across the cluster ──────────────────────────
        check('count() == 60', count_rows(conn, 'sensors', db) == 60)
        got = sorted(d['id'] for d in
                     r.db(db).table('sensors')
                     .between(r.epoch_time(base + 40), r.epoch_time(base + 44),
                              index='ts').run(conn))
        check('between(40,44) raw path == rows 40..43',
              got == [f'r{i:02d}' for i in range(40, 44)], f"n={len(got)}")
        cnt = (r.db(db).table('sensors')
               .between(r.epoch_time(base + 40), r.epoch_time(base + 44),
                        index='ts').count().run(conn))
        check('between().count() == 4', cnt == 4, f"count={cnt}")

        # ── downsample aggregate rows (wait for the merge job) ─────────
        print("  [waiting for downsample merge job across cluster...]")
        lo = r.epoch_time(base)
        hi = r.epoch_time(base + 40)

        def downsample_done():
            rows = list(r.db(db).table('sensors')
                        .between(lo, hi, index='ts').run(conn))
            if not rows or not all('avg_v' in d and 'cnt' in d for d in rows):
                return None
            return rows

        agg = None
        for _ in range(360):  # 180s
            agg = downsample_done()
            if agg is not None and len(agg) == 20 \
                    and sum(d['cnt'] for d in agg) == 40:
                break
            time.sleep(0.5)
        if agg is None:
            check('downsample: aggregate rows appear', False, 'no agg rows')
        else:
            n = len(agg)
            tot = sum(d['cnt'] for d in agg)
            weighted = sum(d['avg_v'] * d['cnt'] for d in agg)
            check('downsample: 20 bucket rows for [base, base+40)',
                  n == 20, f"n={n} sample={agg[0] if agg else None}")
            check('downsample: sum(cnt) == 40 raw rows',
                  tot == 40, f"sum(cnt)={tot}")
            check('downsample: sum(avg_v*cnt) == true sum 780',
                  abs(weighted - 780.0) < 1e-6, f"weighted={weighted}")

        # ── retention: old rows expire, live rows survive ──────────────
        old_base = now_s - 7200  # 2h in the past, beyond 3600s retention
        old = [{'id': f'o{i:02d}', 'ts': r.epoch_time(old_base + i), 'v': i}
               for i in range(10)]
        res = r.db(db).table('sensors').insert(old).run(conn)
        check('insert 10 old rows', res.get('inserted') == 10, str(res))
        ok = wait_for(lambda: count_rows(conn, 'sensors', db) == 60, timeout=120)
        check('retention deletes expired rows (70 -> 60)',
              ok, f"count={count_rows(conn, 'sensors', db)}")
        ci = chunk_stats(conn, 'sensors')
        check('chunk index shows 60 rows after retention',
              ci is not None and ci[1] == 60,
              f"chunk_count={ci and ci[0]} total_rows={ci and ci[1]}")
        gone = list(r.db(db).table('sensors')
                    .between(r.epoch_time(old_base + 2),
                             r.epoch_time(old_base + 6),
                             index='ts').run(conn))
        check('old rows gone from raw between()', len(gone) == 0,
              f"n={len(gone)}")
        check('live rows still readable',
              count_rows(conn, 'sensors', db) == 60)

        # ── FAILOVER: kill node 2, reads survive on node 1 ─────────────
        print("  [FAILOVER: killing node 2]")
        procs[2].kill()
        procs[2].wait(timeout=30)
        time.sleep(8)
        c2 = r.connect(host='127.0.0.1', port=BASE_DRIVER + 1, timeout=5)
        n = count_rows(c2, 'sensors', db)
        check(f"read after node 2 failure ({n} docs)", n == 60, n)
        got = sorted(d['id'] for d in
                     r.db(db).table('sensors')
                     .between(r.epoch_time(base + 40), r.epoch_time(base + 44),
                              index='ts').run(c2))
        check('between() works on surviving node', got == [f'r{i:02d}'
              for i in range(40, 44)], f"n={len(got)}")
        ci2 = chunk_stats(c2, 'sensors')
        check('chunk info via surviving node', ci2 is not None and ci2[1] == 60,
              f"chunk_count={ci2 and ci2[0]} total_rows={ci2 and ci2[1]}")
        c2.close()

        # ── restart node 2: rejoins and serves consistent reads ────────
        print("  [restarting node 2...]")
        procs[2] = boot_node(2, dirs[2])
        ok = wait_for(lambda: server_count(conn) == 3, timeout=120)
        check('node 2 rejoins cluster', ok,
              f"servers={server_count(conn)}")
        c3 = r.connect(host='127.0.0.1', port=BASE_DRIVER + 2, timeout=5)
        ok = wait_for(lambda: count_rows(c3, 'sensors', db) == 60, timeout=120)
        check('rejoined node serves consistent count', ok,
              f"count={count_rows(c3, 'sensors', db)}")
        got = sorted(d['id'] for d in
                     r.db(db).table('sensors')
                     .between(r.epoch_time(base + 40), r.epoch_time(base + 44),
                              index='ts').run(c3))
        check('rejoined node serves between()', got == [f'r{i:02d}'
              for i in range(40, 44)], f"n={len(got)}")
        c3.close()
    finally:
        for p in procs:
            try:
                p.kill()
            except Exception:
                pass
        for d in dirs:
            shutil.rmtree(d, ignore_errors=True)
        if conn:
            try:
                conn.close()
            except Exception:
                pass

    print(f"\n== TS-6 cluster probe: {passed} passed, {failed} failed ==")
    if errors:
        print("FAILURES:")
        for e in errors:
            print(f"  - {e}")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
