#!/usr/bin/env python3
# ruff: noqa: BLE001  # test scripts intentionally catch broad exceptions
"""PHASE3-MERGE E2E — UPSERT (216) + MERGE_DEEP (217) through the driver
against a live server."""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'driver', 'python3'))
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


def main():

    DATA = tempfile.mkdtemp(prefix='merge_')
    DP, CP, HP = 29915, 29916, 29917
    logf = open('/tmp/merge_e2e.log', 'w', buffering=1)
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

    db = f'm_{uuid.uuid4().hex[:6]}'
    r.r.db_create(db).run(conn)
    r.r.db(db).table_create('t').run(conn)

    # ── UPSERT ──
    print("[UPSERT]")
    ins = r.r.db(db).table('t').insert({'id': 'a', 'n': 1, 'tags': ['x']}).run(conn)
    check("initial insert", ins['inserted'] == 1, ins)

    up = r.r.db(db).table('t').upsert({'id': 'a', 'n': 2}).run(conn)
    check("upsert updates existing (not inserted)", up['replaced'] == 1, up)
    doc = r.r.db(db).table('t').get('a').run(conn)
    check("upsert merged fields (n=2, tags kept)", doc['n'] == 2 and 'tags' in doc, doc)

    up2 = r.r.db(db).table('t').upsert({'id': 'b', 'n': 5}).run(conn)
    check("upsert inserts new doc", up2['inserted'] == 1, up2)

    # UPSERT respects explicit conflict override
    up3 = r.r.db(db).table('t').upsert({'id': 'a', 'n': 99},
                                       conflict='error').run(conn)
    check("upsert honors explicit conflict='error'",
          up3['errors'] == 1 and up3['inserted'] == 0, up3)

    # ── MERGE_DEEP ──
    print("[MERGE_DEEP]")
    base = {'a': 1, 'nested': {'x': 1, 'y': 2}, 'arr': [{'p': 1, 'q': 2}, {'r': 3}]}
    over = {'nested': {'y': 20, 'z': 30}, 'b': 2,
            'arr': [{'q': 20, 's': 40}, {'r': 30}, {'t': 5}]}
    md = r.r.expr(base).merge_deep(over, deep=True).run(conn)
    check("deep merge recurses into nested",
          md['nested'] == {'x': 1, 'y': 20, 'z': 30}, md)
    check("deep merge keeps non-overlapping fields",
          md['a'] == 1 and md['b'] == 2, md)
    check("deep merge descends into arrays element-wise",
          md['arr'] == [{'p': 1, 'q': 20, 's': 40}, {'r': 30}, {'t': 5}], md)

    # Shallow merge (deep=False) replaces arrays wholesale (stock merge behavior)
    sh = r.r.expr(base).merge_deep(over, deep=False).run(conn)
    check("shallow mode replaces arrays wholesale",
          sh['arr'] == [{'q': 20, 's': 40}, {'r': 30}, {'t': 5}], sh)

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
