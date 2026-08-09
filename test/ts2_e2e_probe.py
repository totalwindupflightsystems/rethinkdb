#!/usr/bin/env python3
"""PHASE3-TS-2 live E2E probe — chunked write path through the real server.

Follows tick #91 E2E pitfalls:
- rethinkdb create -d <dir> first; serve with explicit ports
- vendored driver via sys.path.insert(0, 'driver/python3'), sync API
- r.db_create('test') needed (no default test DB)
- table_config is keyed by UUID — filter by name
- config()/update() returns result stats, never throws for field errors
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

BIN = '/home/kara/rethinkdb/build/release/rethinkdb'
WORKDIR = tempfile.mkdtemp(prefix='ts2_e2e_')
PORT, CLUSTER_PORT, HTTP_PORT = 39015, 39016, 39017

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


def table_config(conn):
    return (r.db('rethinkdb').table('table_config')
            .filter({'name': 'sensors'}).nth(0).run(conn))


def main():
    print(f"== TS-2 E2E probe ({time.strftime('%H:%M:%S')}) ==")
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
        cfg = table_config(conn)
        ts = cfg.get('time_series')
        check('tableCreate timeSeries accepted',
              ts is not None and ts.get('field') == 'ts', str(ts))
        check('time_series_chunks null before writes',
              cfg.get('time_series_chunks') is None,
              str(cfg.get('time_series_chunks')))

        # 3. insert 50 rows with time field (epoch seconds → TIME pseudo-type)
        base = 1750000000
        batch = [{'id': f'r{i:03d}', 'ts': r.epoch_time(base + i * 10), 'v': i}
                 for i in range(50)]
        res = r.table('sensors').insert(batch).run(conn)
        check('insert 50 rows', res.get('inserted') == 50, str(res))

        cnt = r.table('sensors').count().run(conn)
        check('count == 50', cnt == 50, f'count={cnt}')

        # 4. chunk info exposed after writes
        cfg2 = table_config(conn)
        ci = cfg2.get('time_series_chunks')
        check('time_series_chunks populated', ci is not None, str(ci))
        if ci:
            check('chunk_count >= 1', ci.get('chunk_count', 0) >= 1,
                  f"chunk_count={ci.get('chunk_count')} "
                  f"total_rows={ci.get('total_rows')}")
            check('total_rows == 50', ci.get('total_rows') == 50,
                  f"total_rows={ci.get('total_rows')}")

        # 5. backfill (out-of-order) insert
        res2 = r.table('sensors').insert(
            {'id': 'r_old', 'ts': r.epoch_time(base - 5000), 'v': -1}).run(conn)
        check('backfill insert ok', res2.get('inserted') == 1, str(res2))

        # 6. missing time field → per-row error, no crash
        res3 = r.table('sensors').insert({'id': 'r_bad', 'v': 999}).run(conn)
        check('missing time field errors (no crash)',
              res3.get('errors', 0) >= 1, str(res3))
        check('server alive after error',
              r.table('sensors').count().run(conn) == 51)

        # 7. restart persistence of catalog + rows
        server.send_signal(signal.SIGTERM)
        server.wait(timeout=30)
        server = start_server()
        try:
            conn.close()  # old conn died with the server
        except Exception:
            pass
        conn = wait_conn()
        check('server restarts', conn is not None)
        if conn is None:
            return 1
        # The table's primary replica needs a moment to come up after restart
        # (Raft/contract election); retry the first read until ready.
        cnt2 = None
        for _ in range(30):
            try:
                cnt2 = r.table('sensors').count().run(conn)
                break
            except r.ReqlOpFailedError:
                time.sleep(1)
        check('rows survive restart', cnt2 == 51, f'count={cnt2}')
        cfg3 = table_config(conn)
        ci3 = cfg3.get('time_series_chunks')
        check('chunk info survives restart', ci3 is not None, str(ci3))
        if ci3:
            check('chunk rows survive restart', ci3.get('total_rows') == 51,
                  f"total_rows={ci3.get('total_rows')}")
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
