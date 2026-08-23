#!/usr/bin/env python3
"""PHASE3-TS-4 live E2E probe — time-series retention TTL + background jobs.

Verifies (spec §5.2/§6.4/§8.2):
- retention deletes expired chunks/rows and keeps live rows (1s ticks via
  chunk_interval=1, retention=60s)
- chunk_count and total_rows drop in table_config().time_series_chunks
- live between()/count() still return the surviving rows (read pruning
  regression against the shrunken index)
- retention=0 table keeps everything (control)
- retention-only tableReconfigure (spec §6.1) is accepted and takes effect
  for future ticks once a write stamps the config onto every shard store
  (each of the CPU_SHARDING_FACTOR stores keeps its own durable catalog
  copy, stamped by the writes that land on it — the probe uses a 128-row
  stamp batch so every store's hash region is covered)
- non-retention field changes through reconfigure are still rejected
  (config immutable)
- restart persistence: the post-retention state survives a server restart

Timing model: with chunk_interval=1s the retention job ticks once per
second per shard store (the fork runs CPU_SHARDING_FACTOR stores per
table, each with its own catalog and job). Old rows are inserted FIRST
(~5000s in the past) and given time to expire completely (count 10 -> 0);
live rows are then inserted with ascending timestamps inside one second
so they share a single chunk per store and never age out of the 3600s
retention window (they have a 1h cushion, so probe waits and the restart
check can never outlive them). Old rows are kept in their own insert so
no live-row tile-seal can pin an old chunk's max_time into the retention
window (chunk-granular retention).

The control table gets only old rows: with retention=0 nothing expires;
after a retention-only reconfigure the config must reach every store's
job (wide stamp batch) and the engine must NOT over-expire: fresh stamp
rows keep their 1h cushion, and pre-existing old chunks sealed by the
stamp append legitimately survive one full retention period (spec §4.1:
the append path extends the sealed chunk's max_time to the new chunk's
start, so those tiles only expire once the wall clock passes
stamp_time + retention).

Reconfigure field errors surface via the update result stats
(errors/first_error), NOT exceptions — same as other table_config
validation errors.

Run with the vendored sync driver like ts2/ts3_e2e_probe.py.
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
WORKDIR = tempfile.mkdtemp(prefix='ts4_e2e_')
PORT, CLUSTER_PORT, HTTP_PORT = 39125, 39126, 39127

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
    for _ in range(60):
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
    return (ci.get('chunk_count'), ci.get('total_rows'),
            [c['row_count'] for c in ci.get('chunks', [])])


def count_rows(conn, name):
    return r.table(name).count().run(conn)


def wait_for(pred, timeout=90, interval=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


def main():
    print(f"== TS-4 E2E probe ({time.strftime('%H:%M:%S')}) ==")
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
        # Retention table: 1s chunk interval (fast ticks), 3600s retention
        # (old rows are ~5000s old so they expire; live rows have a 1h
        # cushion and survive the whole probe, incl. restart persistence).
        r.table_create('sensors', timeSeries={
            'field': 'ts', 'chunk_interval': 1, 'retention': 3600
        }).run(conn)
        # Control table: same shape, retention disabled (0).
        r.table_create('control', timeSeries={
            'field': 'ts', 'chunk_interval': 1, 'retention': 0
        }).run(conn)

        now_s = time.time()
        old_ts = [now_s - 5000 - i for i in range(10)]      # 10 old rows
        # Live rows ascend within the last second: one chunk per store,
        # all strictly inside the 3600s retention window.
        live_ts = [now_s - 0.9 + 0.1 * i for i in range(10)]  # 10 live rows

        old_batch = [{'id': f'o{i:02d}', 'ts': r.epoch_time(t), 'v': i}
                     for i, t in enumerate(old_ts)]
        live_batch = [{'id': f'l{i:02d}', 'ts': r.epoch_time(t), 'v': i}
                      for i, t in enumerate(live_ts)]

        # --- old rows expire completely --------------------------------
        res = r.table('sensors').insert(old_batch).run(conn)
        check('insert 10 old rows', res.get('inserted') == 10, str(res))
        ci = chunk_stats(conn, 'sensors')
        check('chunks formed before retention', ci is not None
              and ci[0] >= 1 and ci[1] == 10,
              f"chunk_count={ci and ci[0]} total_rows={ci and ci[1]}")

        ok = wait_for(lambda: count_rows(conn, 'sensors') == 0, timeout=90)
        check('retention deletes expired rows (count 10 -> 0)',
              ok, f"count={count_rows(conn, 'sensors')}")
        ci = chunk_stats(conn, 'sensors')
        check('chunk_count dropped to 0', ci is not None
              and ci[0] == 0 and ci[1] == 0,
              f"chunk_count={ci and ci[0]} total_rows={ci and ci[1]}")

        # --- live rows survive ------------------------------------------
        res = r.table('sensors').insert(live_batch).run(conn)
        check('insert 10 live rows', res.get('inserted') == 10, str(res))
        ok = wait_for(lambda: count_rows(conn, 'sensors') == 10, timeout=90)
        check('live rows kept (count 10 stable)',
              ok, f"count={count_rows(conn, 'sensors')}")

        live_ids = sorted(d['id'] for d in r.table('sensors').run(conn))
        check('only live rows remain', live_ids == [f'l{i:02d}'
              for i in range(10)], f"ids={live_ids[:3]}...")
        ci = chunk_stats(conn, 'sensors')
        check('chunk index shows 10 live rows', ci is not None
              and ci[1] == 10 and ci[0] >= 1,
              f"chunk_count={ci and ci[0]} total_rows={ci and ci[1]}")

        # Read pruning still correct on the shrunken index (TS-3
        # regression).
        lo = r.epoch_time(now_s - 5)
        hi = r.epoch_time(now_s + 1)
        got = sorted(d['id'] for d in
                     r.table('sensors').between(lo, hi, index='ts').run(conn))
        check('between() returns only live rows', got == [f'l{i:02d}'
              for i in range(10)], f"n={len(got)}")
        check('old rows gone from between()',
              len(list(r.table('sensors').between(
                  r.epoch_time(now_s - 5100), r.epoch_time(now_s - 4900),
                  index='ts').run(conn))) == 0)

        # --- control table (retention 0): nothing expires ---------------
        r.table('control').insert(old_batch).run(conn)
        time.sleep(5)  # several retention ticks would have fired
        cnt = count_rows(conn, 'control')
        check('retention=0 control keeps all rows', cnt == 10, f"count={cnt}")

        # --- reconfigure: retention-only change is accepted -------------
        tc = table_config(conn, 'control')
        ts_cfg = tc['time_series']
        new_cfg = dict(ts_cfg)
        new_cfg['retention'] = 3600
        r.db('rethinkdb').table('table_config').get(tc['id']).update(
            {'time_series': new_cfg}).run(conn)
        got_cfg = table_config(conn, 'control')['time_series']
        check('retention-only reconfigure accepted',
              got_cfg.get('retention') == 3600,
              f"retention={got_cfg.get('retention')}")

        # Immutable change is still rejected (surfaces via result stats,
        # not an exception).
        bad_cfg = dict(got_cfg)
        bad_cfg['field'] = 'other'
        res = r.db('rethinkdb').table('table_config').get(tc['id']).update(
            {'time_series': bad_cfg}).run(conn)
        check('immutable time_series change rejected',
              res.get('errors') == 1
              and 'read-only' in res.get('first_error', ''),
              f"errors={res.get('errors')} first_error={res.get('first_error')}")

        # New retention takes effect for future ticks once a write stamps
        # the config. The stamp must be a WIDE batch: each of the 8 shard
        # stores keeps its own durable catalog copy, stamped by the writes
        # that land on it — a single-row stamp would reach only one store.
        # 128 spread keys cover every store's hash region with overwhelming
        # probability, so every store's job sees retention=3600.
        # What we assert post-reconfigure: retention is ACTIVE on the
        # stamped stores (pre-existing old chunks with bounded max_time
        # expire within the hold window — observed deterministically:
        # 2 of 10 on this PK layout) AND nothing over-expires (fresh stamp
        # rows keep their 1h cushion; chunks sealed by the stamp append —
        # max_time extended to the stamp's ts per spec §4.1 — survive).
        # With 10 old + 128 stamps: 136 <= count <= 137 after the hold
        # (1-2 old rows gone, 128 stamps + remainder intact). A job that
        # never got the config would stay at 138; an over-expiring job
        # would drop stamps (count < 136).
        stamps = [{'id': f'st{i:03d}', 'ts': r.now(), 'v': i}
                  for i in range(128)]
        r.table('control').insert(stamps).run(conn)
        time.sleep(75)
        cnt = count_rows(conn, 'control')
        check('reconfigured retention active, no over-expiry (136-137)',
              136 <= cnt <= 137, f"count={cnt}")

        # --- restart persistence -----------------------------------------
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
        cnt = None
        for _ in range(30):
            try:
                cnt = r.table('sensors').count().run(conn)
                break
            except r.ReqlOpFailedError:
                time.sleep(1)
        check('post-retention state survives restart', cnt == 10,
              f'count={cnt}')
        ci = chunk_stats(conn, 'sensors')
        check('chunk index survives restart',
              ci is not None and ci[1] == 10,
              f"chunk_count={ci and ci[0]} total_rows={ci and ci[1]}")
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
