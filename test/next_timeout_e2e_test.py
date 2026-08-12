#!/usr/bin/env python3
# ruff: noqa: BLE001  # test scripts intentionally catch broad exceptions
"""RT-GAP-016 E2E — Cursor.next(timeout=...) bounded-wait support against a live server."""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'driver', 'python3'))
import rethinkdb as r  # noqa: E402
from rethinkdb.errors import ReqlDriverError, ReqlTimeoutError  # noqa: E402

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'release', 'rethinkdb')
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


def main():

    DATA = tempfile.mkdtemp(prefix='next_timeout_')
    DP, CP, HP = 30016, 30017, 30018
    logf = open('/tmp/next_timeout_e2e.log', 'w', buffering=1)
    proc = subprocess.Popen([BIN, '-d', DATA, '--http-port', str(HP),
                             '--driver-port', str(DP), '--cluster-port', str(CP),
                             '--io-threads', '4', '--no-update-check'],
                            stdout=logf, stderr=subprocess.STDOUT)
    deadline = time.time() + 30
    conn = None
    while time.time() < deadline:
        try:
            conn = r.r.connect(host='127.0.0.1', port=DP, timeout=2)
            break
        except Exception:
            time.sleep(0.5)
    print("Server up.")

    db = f'n_{uuid.uuid4().hex[:6]}'
    r.r.db_create(db).run(conn)
    r.r.db(db).table_create('t').run(conn)
    r.r.db(db).table('t').insert({'id': 1, 'v': 'a'}).run(conn)

    # ── AC1: next(timeout=5.0) returns a doc without TypeError ──
    print("[AC1: next(timeout=5.0)]")
    feed = r.r.db(db).table('t').changes().run(conn)
    r.r.db(db).table('t').insert({'id': 2, 'v': 'b'}).run(conn)
    try:
        change = feed.next(timeout=5.0)
        check("next(timeout=5.0) returns a row, no TypeError", change['new_val']['id'] == 2, change)
    except TypeError as e:
        check("next(timeout=5.0) returns a row, no TypeError", False, f"TypeError: {e}")
    except Exception as e:
        check("next(timeout=5.0) returns a row, no TypeError", False, f"{type(e).__name__}: {e}")
    feed.close()

    # ── AC2: wait-only calls unchanged ──
    print("[AC2: wait-only backward compat]")
    feed = r.r.db(db).table('t').changes().run(conn)
    r.r.db(db).table('t').insert({'id': 3, 'v': 'c'}).run(conn)
    try:
        d1 = feed.next()
        check("next() returns first row", d1['new_val']['id'] == 3, d1)
    except Exception as e:
        check("next() returns first row", False, f"{type(e).__name__}: {e}")

    r.r.db(db).table('t').insert({'id': 4, 'v': 'd'}).run(conn)
    try:
        d2 = feed.next(wait=True)
        check("next(wait=True) returns second row", d2['new_val']['id'] == 4, d2)
    except Exception as e:
        check("next(wait=True) returns second row", False, f"{type(e).__name__}: {e}")

    try:
        feed.next(wait=False)
        check("next(wait=False) on empty cursor raises ReqlTimeoutError", False, "no error raised")
    except ReqlTimeoutError:
        check("next(wait=False) on empty cursor raises ReqlTimeoutError", True)
    except Exception as e:
        check("next(wait=False) on empty cursor raises ReqlTimeoutError", False, f"{type(e).__name__}: {e}")
    feed.close()

    # next(wait=5) numeric wait still works
    feed = r.r.db(db).table('t').changes().run(conn)
    r.r.db(db).table('t').insert({'id': 5, 'v': 'e'}).run(conn)
    try:
        d3 = feed.next(wait=5)
        check("next(wait=5) returns a row", d3['new_val']['id'] == 5, d3)
    except Exception as e:
        check("next(wait=5) returns a row", False, f"{type(e).__name__}: {e}")
    feed.close()

    # ── AC1b: timeout=0 behaves like wait=False (no wait) ──
    print("[AC1b: timeout=0 no-wait]")
    feed = r.r.db(db).table('t').changes().run(conn)
    try:
        feed.next(timeout=0)
        check("next(timeout=0) on idle changefeed raises ReqlTimeoutError", False, "no error raised")
    except ReqlTimeoutError:
        check("next(timeout=0) on idle changefeed raises ReqlTimeoutError", True)
    except Exception as e:
        check("next(timeout=0) on idle changefeed raises ReqlTimeoutError", False, f"{type(e).__name__}: {e}")
    feed.close()

    # timeout=5.0 on an idle changefeed: waits ~5s, then raises ReqlTimeoutError
    feed = r.r.db(db).table('t').changes().run(conn)
    t0 = time.time()
    try:
        feed.next(timeout=5.0)
        check("next(timeout=5.0) on idle changefeed times out", False, "no error raised")
    except ReqlTimeoutError:
        elapsed = time.time() - t0
        check("next(timeout=5.0) on idle changefeed times out", 4.0 <= elapsed < 10.0, f"elapsed={elapsed:.2f}s")
    except Exception as e:
        check("next(timeout=5.0) on idle changefeed times out", False, f"{type(e).__name__}: {e}")
    feed.close()

    # ── AC3: timeout validation ──
    print("[AC3: timeout validation]")
    feed = r.r.db(db).table('t').changes().run(conn)
    try:
        feed.next(timeout=-1)
        check("next(timeout=-1) raises ReqlDriverError", False, "no error raised")
    except ReqlDriverError as e:
        check("next(timeout=-1) raises ReqlDriverError", "Invalid wait timeout" in str(e), str(e))
    except Exception as e:
        check("next(timeout=-1) raises ReqlDriverError", False, f"{type(e).__name__}: {e}")

    try:
        feed.next(timeout="x")
        check("next(timeout='x') raises ReqlDriverError", False, "no error raised")
    except ReqlDriverError as e:
        check("next(timeout='x') raises ReqlDriverError", "Invalid wait timeout" in str(e), str(e))
    except Exception as e:
        check("next(timeout='x') raises ReqlDriverError", False, f"{type(e).__name__}: {e}")
    feed.close()

    # ── Cleanup ──
    proc.kill()
    shutil.rmtree(DATA, ignore_errors=True)
    print(f"\n{'=' * 50}")
    print(f"RESULT: {passed} passed, {failed} failed")
    if errors:
        print("FAILURES:")
        for e in errors:
            print(f"  - {e}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
