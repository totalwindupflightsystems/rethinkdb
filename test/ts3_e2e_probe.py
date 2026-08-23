#!/usr/bin/env python3
"""PHASE3-TS-3 live E2E probe — time-series `between` read pruning.

Verifies (spec §4.2/§6.3, acceptance criteria):
- between() on the time field (explicit {index: 'ts'} AND the plain no-index
  default) returns exactly the rows whose ts is in the window
- chunk pruning: rows from chunks outside the window are absent
- unbounded / empty ranges, open/closed bounds, backfill rows
- pkey between, getAll and count unchanged (TS-2 regression)
- restart persistence

Chunk layout: chunk_interval=60 with rows at t=0..149 forms 3 chunks
[0,60) [60,120) [120,180); the probe verifies the actual layout from
table_config().time_series_chunks first and adapts expectations.

Run with the vendored sync driver like ts2_e2e_probe.py.
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'driver', 'python3'))
from rethinkdb import r  # noqa: E402

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'release', 'rethinkdb')
if not os.path.exists(BIN):
    raise RuntimeError(
        f"rethinkdb binary not found at {BIN}; run `make -j4` "
        "first (see AGENTS.md) before running this probe")
WORKDIR = tempfile.mkdtemp(prefix='ts3_e2e_')
PORT, CLUSTER_PORT, HTTP_PORT = 39115, 39116, 39117

BASE = 1750000000  # epoch seconds; rows at BASE + 0..149

results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(f"{'PASS' if cond else 'FAIL'}  {name}  {detail}")


def start_server():
    return subprocess.Popen(
        [BIN, 'serve', '-d', WORKDIR,
         '--driver-port', str(PORT), '--cluster-port', str(CLUSTER_PORT),
         '--http-port', str(HTTP_PORT), '--no-update-check'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def wait_conn():
    for _ in range(60):
        try:
            return r.connect(host='127.0.0.1', port=PORT, timeout=2)
        except Exception:
            time.sleep(1)
    return None


def table_config(conn, name='sensors'):
    return (r.db('rethinkdb').table('table_config')
            .filter({'name': name}).nth(0).run(conn))


def ts_between(conn, lo, hi, index=None, **kwargs):
    opts = dict(kwargs)
    if index is not None:
        opts['index'] = index
    q = r.table('sensors').between(lo, hi, **opts)
    return sorted(doc['id'] for doc in q.run(conn))


def epoch(t):
    return r.epoch_time(BASE + t)


def main():
    print(f"== TS-3 E2E probe ({time.strftime('%H:%M:%S')}) ==")
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
        r.table_create('sensors', timeSeries={
            'field': 'ts', 'chunk_interval': 60,
            'retention': 86400
        }).run(conn)
        # Non-TS control table: between must stay byte-identical (criterion 4)
        r.table_create('plain').run(conn)
        r.table('plain').insert(
            [{'id': i, 'v': i * 10} for i in range(10)]).run(conn)

        # 150 rows, t = BASE+0 .. BASE+149 → expect 3 chunks of 60/60/30
        batch = [{'id': f'r{i:03d}', 'ts': epoch(i), 'v': i}
                 for i in range(150)]
        res = r.table('sensors').insert(batch).run(conn)
        check('insert 150 rows', res.get('inserted') == 150, str(res))

        ci = table_config(conn).get('time_series_chunks')
        check('chunk info present', ci is not None, str(ci))
        chunk_rows = []
        if ci:
            chunk_rows = [c['row_count'] for c in ci.get('chunks', [])]
        check('>= 3 chunks formed', ci is not None
              and ci.get('chunk_count', 0) >= 3,
              f"chunk_count={ci and ci.get('chunk_count')} "
              f"rows_per_chunk={chunk_rows}")
        check('total_rows == 150', ci is not None
              and ci.get('total_rows') == 150,
              f"total_rows={ci and ci.get('total_rows')}")

        # --- between on the time field: explicit index --------------------
        got = ts_between(conn, epoch(40), epoch(80), index='ts')
        check('between(40,80,{index:ts}) == rows 40..79',
              got == [f'r{i:03d}' for i in range(40, 80)],
              f"n={len(got)} first={got[:2]} last={got[-2:]}")

        # --- plain between (no index): sensible default = time field ------
        got2 = ts_between(conn, epoch(40), epoch(80))
        check('plain between(40,80) == same rows', got2 == got,
              f"n={len(got2)}")

        # --- unbounded ends ------------------------------------------------
        got = ts_between(conn, r.minval, epoch(70), index='ts')
        check('between(minval,70,{index:ts}) == rows 0..69',
              got == [f'r{i:03d}' for i in range(0, 70)], f"n={len(got)}")
        got = ts_between(conn, epoch(130), r.maxval, index='ts')
        check('between(130,maxval,{index:ts}) == rows 130..149',
              got == [f'r{i:03d}' for i in range(130, 150)], f"n={len(got)}")
        got = ts_between(conn, r.minval, r.maxval, index='ts',
                         left_bound='closed')
        check('between(minval,maxval,{index:ts}) == all 150',
              len(got) == 150, f"n={len(got)}")

        # --- empty / inverted ranges --------------------------------------
        got = ts_between(conn, epoch(80), epoch(80), index='ts')
        check('empty range [80,80) == []', got == [], f"n={len(got)}")
        got = ts_between(conn, epoch(80), epoch(40), index='ts')
        check('inverted range == []', got == [], f"n={len(got)}")
        # Range covering no chunk (way after the data).
        got = ts_between(conn, epoch(5000), epoch(6000), index='ts')
        check('range over no chunk == []', got == [], f"n={len(got)}")

        # --- open/closed bound semantics -----------------------------------
        got = ts_between(conn, epoch(40), epoch(80), index='ts',
                         left_bound='closed', right_bound='closed')
        check('closed-closed [40,80] == rows 40..80',
              got == [f'r{i:03d}' for i in range(40, 81)], f"n={len(got)}")
        got = ts_between(conn, epoch(40), epoch(80), index='ts',
                         left_bound='open', right_bound='closed')
        check('open-closed (40,80] == rows 41..80',
              got == [f'r{i:03d}' for i in range(41, 81)], f"n={len(got)}")

        # --- backfill row (ts inside chunk 0, inserted after the fact) -----
        r.table('sensors').insert(
            {'id': 'r_bf', 'ts': epoch(5), 'v': -1}).run(conn)
        got = ts_between(conn, epoch(0), epoch(10), index='ts')
        expect = ['r_bf'] + [f'r{i:03d}' for i in range(0, 10)]
        check('backfill row returned by between(0,10)',
              got == sorted(expect), f"n={len(got)} got={got}")

        # --- TS-2 regression: count / get / getAll / pkey between ----------
        check('count == 151', r.table('sensors').count().run(conn) == 151)
        check('get works',
              r.table('sensors').get('r_bf').run(conn).get('v') == -1)
        got = sorted(r.table('sensors').get_all('r005', 'r010')
                     .run(conn), key=lambda d: d['id'])
        check('getAll works', [d['id'] for d in got] == ['r005', 'r010'],
              f"got={[d['id'] for d in got]}")
        got = sorted(d['id'] for d in
                     r.table('sensors').between('r000', 'r010').run(conn))
        check('pkey between unchanged (r000..r009)',
              got == [f'r{i:03d}' for i in range(0, 10)], f"n={len(got)}")
        # count() on a between slice (terminal through the pruned read)
        cnt = (r.table('sensors')
               .between(epoch(40), epoch(80), index='ts')
               .count().run(conn))
        check('between().count() == 40', cnt == 40, f"count={cnt}")

        # --- non-TS table regression (criterion 4) --------------------------
        got = sorted(d['id'] for d in
                     r.table('plain').between(1, 3).run(conn))
        check('plain table between unchanged', got == [1, 2], f"got={got}")
        # Time bounds on a plain table stay on the pkey path (config is
        # disabled → no time-series dispatch); ptype-encoded keys match
        # nothing, so the result is empty — unchanged pre-TS-3 behavior.
        got = sorted(d['id'] for d in
                     r.table('plain').between(epoch(1), epoch(3)).run(conn))
        check('plain table time-bounds between unchanged', got == [],
              f"got={got}")

        # --- restart persistence -------------------------------------------
        server.send_signal(signal.SIGTERM)
        server.wait(timeout=30)
        server = start_server()
        try:
            conn.close()
        except Exception:
            pass
        conn = wait_conn()
        check('server restarts', conn is not None)
        if conn is None:
            return 1
        cnt2 = None
        for _ in range(30):
            try:
                cnt2 = r.table('sensors').count().run(conn)
                break
            except r.ReqlOpFailedError:
                time.sleep(1)
        check('rows survive restart', cnt2 == 151, f'count={cnt2}')
        got = ts_between(conn, epoch(40), epoch(80), index='ts')
        check('between survives restart',
              got == [f'r{i:03d}' for i in range(40, 80)], f"n={len(got)}")
        got = ts_between(conn, epoch(0), epoch(10), index='ts')
        check('backfill row survives restart', 'r_bf' in got,
              f"n={len(got)}")
        ci3 = table_config(conn).get('time_series_chunks')
        check('chunk info survives restart',
              ci3 is not None and ci3.get('total_rows') == 151,
              f"total_rows={ci3 and ci3.get('total_rows')}")
    finally:
        if server and server.poll() is None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=15)
            except subprocess.TimeoutExpired:
                server.kill()
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
