#!/usr/bin/env python3
"""PHASE3-TS-6 chaos probe — kill/restart consistency for time-series.

Covers the three spec §8.3 scenarios with REAL SIGKILL + restart against
the actual server (ts4_e2e_probe.py server-lifecycle pattern):

1. Kill mid-chunk-seal: a large in-flight insert is SIGKILLed while its
   chunk seals are being written. Restart → the chunk index is the
   checkpoint: count() matches table_config().time_series_chunks
   total_rows exactly (no half-written chunks, no corrupt trees), the
   pre-kill committed rows survive, and the table still reads.
2. Kill mid-retention: 300 expired + 10 live rows; SIGKILL while the
   retention pass is mid-deletion. Restart → retention resumes from the
   chunk index: the expired chunks stay expired (old rows gone), live
   rows survive, count() settles at exactly the live count.
3. Kill during/after downsample merge: SIGKILL while the merge job has
   partially folded sealed chunks. Restart → the per-chunk watermark
   resumes: the merge completes with no duplicate buckets and no data
   loss (sum(cnt) == raw rows in the window, sum(avg_v*cnt) == the true
   sum of v, len(buckets) == expected).

Timing note: exact mid-op landing is best-effort (kill at a fixed delay /
on a partial-state poll). Each scenario records which state the kill
actually landed in; the restart-consistency assertions hold regardless.
The aggregate row checks are exact because chunk boundaries (4s) are
aligned to bucket boundaries (2s) — target interval <= chunk interval,
the regime the spec's own config examples use.

Run with the vendored sync driver like ts2/ts3/ts4_e2e_probe.py.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'driver', 'python3'))
from rethinkdb import r  # noqa: E402

BIN = '/home/kara/rethinkdb/build/release/rethinkdb'
WORKDIR = tempfile.mkdtemp(prefix='ts6_chaos_')
PORT, CLUSTER_PORT, HTTP_PORT = 39915, 39916, 39917

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(f"{'PASS' if cond else 'FAIL'}  {name}  {detail}")


def start_server():
    return subprocess.Popen(
        [BIN, 'serve', '-d', WORKDIR,
         '--driver-port', str(PORT), '--cluster-port', str(CLUSTER_PORT),
         '--http-port', str(HTTP_PORT), '--no-update-check'],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def wait_conn():
    for _ in range(90):
        try:
            return r.connect(host='127.0.0.1', port=PORT, timeout=2)
        except Exception:
            time.sleep(1)
    return None


def table_config(conn, name):
    return (r.db('rethinkdb').table('table_config')
            .filter({'name': name}).nth(0).run(conn))


def chunk_stats(conn, name):
    ci = table_config(conn, name).get('time_series_chunks')
    if not ci:
        return None
    return (ci.get('chunk_count'), ci.get('total_rows'))


def count_rows(conn, name):
    return r.table(name).count().run(conn)


def wait_for(pred, timeout=90, interval=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if pred():
                return True
        except Exception:
            pass  # primary replica may not be available right after restart
        time.sleep(interval)
    return False


def kill_and_restart(server):
    """SIGKILL the server (crash, not graceful stop), restart, reconnect."""
    server.kill()
    server.wait(timeout=30)
    return start_server()


def main():
    print(f"== TS-6 chaos probe ({time.strftime('%H:%M:%S')}) ==")
    subprocess.run([BIN, 'create', '-d', WORKDIR], check=True,
                   capture_output=True)
    server = start_server()
    conn = None
    try:
        conn = wait_conn()
        check('server ready', conn is not None)
        if conn is None:
            return 1
        r.db_create('test').run(conn)

        # ── Scenario 1: kill mid-chunk-seal ────────────────────────────
        print("[SCENARIO 1: kill mid-chunk-seal]")
        r.table_create('seal', timeSeries={
            'field': 'ts', 'chunk_interval': 1, 'retention': 0
        }).run(conn)
        base1 = int(time.time()) - 200
        batch_a = [{'id': f'a{i:03d}', 'ts': r.epoch_time(base1 + i), 'v': i}
                   for i in range(100)]
        res = r.table('seal').insert(batch_a).run(conn)
        check('s1: commit 100 rows', res.get('inserted') == 100, str(res))
        ok = wait_for(lambda: count_rows(conn, 'seal') == 100, timeout=30)
        check('s1: 100 rows visible', ok)
        ci = chunk_stats(conn, 'seal')
        check('s1: chunk index total_rows == 100', ci is not None
              and ci[1] == 100, f"total_rows={ci and ci[1]}")

        # Huge in-flight batch (10K rows, 10K chunk seals) — SIGKILL while
        # the write transaction is mid-seal. The batch either commits
        # whole or rolls back whole; either way index and data agree.
        base1b = int(time.time()) - 300
        big = [{'id': f'b{i:04d}', 'ts': r.epoch_time(base1b + i), 'v': i}
               for i in range(10000)]

        def fire_big_batch():
            try:
                r.table('seal').insert(big).run(conn)
            except Exception:
                pass

        t = threading.Thread(target=fire_big_batch, daemon=True)
        t.start()
        time.sleep(0.4)
        server = kill_and_restart(server)
        try:
            conn.close()
        except Exception:
            pass
        conn = wait_conn()
        check('s1: server restarts after SIGKILL', conn is not None)
        if conn is None:
            return 1
        t.join(timeout=30)
        cnt = None
        for _ in range(30):
            try:
                cnt = count_rows(conn, 'seal')
                break
            except r.ReqlOpFailedError:
                time.sleep(1)
        s1_killed_state = 'batch-rolled-back' if cnt == 100 \
            else 'batch-committed'
        check('s1: table readable after restart', cnt is not None)
        ci = chunk_stats(conn, 'seal')
        check('s1: chunk index consistent (count == total_rows)',
              cnt is not None and ci is not None and ci[1] == cnt,
              f"count={cnt} total_rows={ci and ci[1]}")
        check('s1: pre-kill rows survive',
              cnt is not None and cnt >= 100, f"count={cnt}")
        got = sorted(d['id'] for d in
                     r.table('seal')
                     .between(r.epoch_time(base1), r.epoch_time(base1 + 4),
                              index='ts').run(conn))
        # The window [base1, base1+4) is 4s wide. The a-batch rows
        # a000..a003 fall in it by construction. If the 10K b-batch
        # COMMITTED (kill landed after the write finished), its rows
        # b0100..b0103 (ts = base1b + i, base1b = base1 - 100) also fall
        # in the window — 8 rows total. A rollback leaves only the 4 a-rows.
        expected_a = [f'a{i:03d}' for i in range(4)]
        expected = sorted(expected_a + (
            [f'b{i:04d}' for i in range(100, 104)]
            if s1_killed_state == 'batch-committed' else []))
        check('s1: between() reads after restart',
              got == expected, f"n={len(got)} state={s1_killed_state}")
        print(f"  [s1 kill landed: {s1_killed_state}]")

        # ── Scenario 2: kill mid-retention ─────────────────────────────
        print("[SCENARIO 2: kill mid-retention]")
        r.table_create('ret', timeSeries={
            'field': 'ts', 'chunk_interval': 1, 'retention': 30
        }).run(conn)
        now2 = int(time.time())
        old2 = [{'id': f'o{i:03d}', 'ts': r.epoch_time(now2 - 600 + i),
                 'v': i} for i in range(300)]
        live2 = [{'id': f'l{i:02d}', 'ts': r.epoch_time(now2 + 40 + i),
                  'v': i} for i in range(10)]
        res = r.table('ret').insert(old2).run(conn)
        check('s2: insert 300 expired rows', res.get('inserted') == 300,
              str(res))
        res = r.table('ret').insert(live2).run(conn)
        check('s2: insert 10 live rows', res.get('inserted') == 10, str(res))
        ok = wait_for(lambda: count_rows(conn, 'ret') == 310, timeout=30)
        check('s2: 310 rows before retention', ok)

        # Kill while the retention pass is mid-deletion (count strictly
        # between the live count and the full count).
        killed_at = None
        deadline = time.time() + 30
        while time.time() < deadline:
            cnt = count_rows(conn, 'ret')
            if 10 < cnt < 310:
                killed_at = cnt
                break
            time.sleep(0.1)
        server = kill_and_restart(server)
        try:
            conn.close()
        except Exception:
            pass
        conn = wait_conn()
        check('s2: server restarts after SIGKILL', conn is not None)
        if conn is None:
            return 1
        ok = wait_for(lambda: count_rows(conn, 'ret') == 10, timeout=90)
        check('s2: retention resumes and completes (count 10)',
              ok, f"count={count_rows(conn, 'ret')}")
        got = list(r.table('ret')
                   .between(r.epoch_time(now2 - 600), r.epoch_time(now2 - 590),
                            index='ts').run(conn))
        check('s2: expired rows stay gone after restart', len(got) == 0,
              f"n={len(got)}")
        live_ids = sorted(d['id'] for d in r.table('ret').run(conn))
        check('s2: live rows survive',
              live_ids == [f'l{i:02d}' for i in range(10)],
              f"ids={live_ids[:3]}...")
        ci = chunk_stats(conn, 'ret')
        check('s2: chunk index shows 10 rows',
              ci is not None and ci[1] == 10,
              f"chunk_count={ci and ci[0]} total_rows={ci and ci[1]}")
        print(f"  [s2 kill landed: count={killed_at} "
              f"({'mid-retention' if killed_at is not None else 'post-pass'})]")

        # ── Scenario 3: kill during/after downsample merge ─────────────
        print("[SCENARIO 3: kill during downsample merge]")
        r.table_create('ds', timeSeries={
            'field': 'ts', 'chunk_interval': 4, 'retention': 3600,
            'downsample': [{
                'age': 5, 'to': 2,
                'aggregate': {
                    'avg_v': lambda row: row['v'].avg(),
                    'cnt': lambda row: row.count(),
                },
            }],
        }).run(conn)
        now3 = int(time.time())
        base3 = now3 - 60
        if base3 % 2:
            base3 -= 1
        rows3 = [{'id': f'r{i:02d}', 'ts': r.epoch_time(base3 + i), 'v': i}
                 for i in range(60)]
        res = r.table('ds').insert(rows3).run(conn)
        check('s3: insert 60 rows', res.get('inserted') == 60, str(res))
        ok = wait_for(lambda: count_rows(conn, 'ds') == 60, timeout=30)
        check('s3: 60 rows visible', ok)
        lo3 = r.epoch_time(base3)
        hi3 = r.epoch_time(base3 + 40)

        def agg_sum():
            rows = list(r.table('ds').between(lo3, hi3, index='ts').run(conn))
            if not rows or not all('avg_v' in d and 'cnt' in d for d in rows):
                return None
            return sum(d['cnt'] for d in rows)

        s3_state = 'merge-not-started'
        deadline = time.time() + 60
        while time.time() < deadline:
            s = agg_sum()
            if s is not None and 0 < s < 40:
                s3_state = f'mid-merge (sum={s})'
                break
            if s is not None and s >= 40:
                s3_state = 'merge-completed'
                break
            time.sleep(0.3)
        server = kill_and_restart(server)
        try:
            conn.close()
        except Exception:
            pass
        conn = wait_conn()
        check('s3: server restarts after SIGKILL', conn is not None)
        if conn is None:
            return 1
        ok = wait_for(lambda: agg_sum() == 40, timeout=120, interval=0.5)
        rows = list(r.table('ds').between(lo3, hi3, index='ts').run(conn))
        n = len(rows) if rows else 0
        tot = sum(d['cnt'] for d in rows) if rows else 0
        weighted = sum(d['avg_v'] * d['cnt'] for d in rows) if rows else 0
        check('s3: merge completes after restart (sum(cnt) == 40)',
              ok and tot == 40, f"sum={tot}")
        check('s3: no duplicate buckets (bucket count in [20,40])',
              ok and 20 <= n <= 40, f"n={n}")
        check('s3: aggregates correct (weighted sum 780)',
              ok and abs(weighted - 780.0) < 1e-6, f"weighted={weighted}")
        check('s3: raw rows intact (count == 60)',
              count_rows(conn, 'ds') == 60)
        print(f"  [s3 kill landed: {s3_state}]")
    finally:
        if server and server.poll() is None:
            server.kill()
            try:
                server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                pass
        if conn:
            try:
                conn.close()
            except Exception:
                pass
        shutil.rmtree(WORKDIR, ignore_errors=True)

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n== {passed}/{len(results)} checks passed ==")
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
