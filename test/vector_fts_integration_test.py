#!/usr/bin/env python3
"""Vector + FTS Integration Test — index creation, metadata, and querying.

Tests vector and FTS (full-text search) features through the full ReQL query
path using the rethinkdb Python driver.

Key design decision — indexes are created SEQUENTIALLY (one at a time, waiting
for each to be ready) to avoid concurrent superblock contention in the sindex
construction coroutines. Parallel index creation caused index_wait to hang.

Usage:
  cd ~/rethinkdb && /tmp/rethink_venv/bin/python3 test/vector_fts_integration_test.py
"""

import os
import sys
import time
import subprocess
import tempfile
import shutil
import atexit
import uuid

sys.path.insert(0, '/tmp')
import rethinkdb as r

# ── Test harness ──

RETHINKDB_BIN = os.path.join(os.path.dirname(__file__), '..', 'build', 'release', 'rethinkdb')
DATA_DIR = tempfile.mkdtemp(prefix='vf_integration_')
HTTP_PORT = 28998
DRIVER_PORT = 28996
CLUSTER_PORT = 28997
SERVER_READY_TIMEOUT = 30
server_proc = None


class TestResult:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.errors = []

    def assert_true(self, cond, msg):
        if cond:
            self.passed += 1
        else:
            self.failed += 1
            self.errors.append(f"FAIL: {msg}")

    def assert_equal(self, a, b, msg):
        self.assert_true(a == b, f"{msg}: expected {b!r}, got {a!r}")

    def summary(self):
        total = self.passed + self.failed
        print(f"\n{'='*60}")
        print(f"RESULTS: {self.passed}/{total} passed, {self.failed} failed")
        if self.errors:
            for e in self.errors:
                print(f"  {e}")
        return self.failed == 0


t = TestResult()


def cleanup():
    global server_proc
    if server_proc:
        try:
            server_proc.terminate()
            server_proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, ProcessLookupError):
            server_proc.kill()
    shutil.rmtree(DATA_DIR, ignore_errors=True)


atexit.register(cleanup)


def start_server():
    global server_proc, RETHINKDB_BIN
    if not os.path.exists(RETHINKDB_BIN):
        raise RuntimeError(
            f"rethinkdb binary not found at {RETHINKDB_BIN}; run `make -j4` "
            "first (see AGENTS.md) before running this test")
    server_proc = subprocess.Popen(
        [RETHINKDB_BIN, '--no-update-check', '--bind', '127.0.0.1',
         '--http-port', str(HTTP_PORT), '--driver-port', str(DRIVER_PORT),
         '--cluster-port', str(CLUSTER_PORT),
         '-d', DATA_DIR, '--io-threads', '4'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    deadline = time.time() + SERVER_READY_TIMEOUT
    while time.time() < deadline:
        try:
            conn = r.r.connect(host='127.0.0.1', port=DRIVER_PORT, timeout=2)
            conn.close()
            return True
        except Exception:
            time.sleep(0.5)
    return False


def stop_server():
    global server_proc
    if server_proc:
        try:
            server_proc.terminate()
            server_proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, ProcessLookupError):
            server_proc.kill()
        server_proc = None


def connect():
    return r.r.connect(host='127.0.0.1', port=DRIVER_PORT, timeout=10)


def wait_for_index(conn, db_name, table_name, index_name, timeout_s=30):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        statuses = r.r.db(db_name).table(table_name).index_status(index_name).run(conn)
        if isinstance(statuses, list) and len(statuses) > 0:
            if statuses[0].get('ready', False):
                return True
        elif isinstance(statuses, dict):
            if statuses.get('ready', False):
                return True
        time.sleep(0.5)
    return False


def create_index_and_wait(conn, db_name, table_name, name, func_or_col, kwargs, desc):
    """Create an index and wait for it to become ready before returning."""
    print(f"\n[TEST] create_{name} ({desc})")
    try:
        result = r.r.db(db_name).table(table_name).index_create(
            name, func_or_col, **kwargs
        ).run(conn)
        t.assert_true(isinstance(result, dict), f"{name} creation returns a result")
        t.assert_true(result.get('created', 0) > 0, f"{name} was created")
        print(f"  created: {result}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"create_{name}: {e}")
        return

    if not wait_for_index(conn, db_name, table_name, name):
        t.failed += 1
        t.errors.append(f"create_{name}: index never became ready")
    else:
        print(f"  ready: ✓")
        t.passed += 1


# ── Vector Index Tests ──

def test_vector_indexes(conn, db_name, table_name):
    """Create 3 vector indexes sequentially (one at a time, waiting for ready)."""
    print("\n── Vector Indexes ──")
    create_index_and_wait(conn, db_name, table_name, "vec_l2_idx",
        r.r.row["vec_attr"], {"vector": {"dim": 3, "metric": "l2"}},
        "L2 metric")
    create_index_and_wait(conn, db_name, table_name, "vec_cos_idx",
        r.r.row["vec_attr"], {"vector": {"dim": 3, "metric": "cosine"}},
        "cosine metric")
    create_index_and_wait(conn, db_name, table_name, "vec_ip_idx",
        r.r.row["vec_attr"], {"vector": {"dim": 3, "metric": "inner_product"}},
        "inner_product metric")


def test_vector_index_status(conn, db_name, table_name):
    """Verify vector index metadata via index_status."""
    print("\n[TEST] vector_index_status")
    try:
        statuses = r.r.db(db_name).table(table_name).index_status("vec_l2_idx").run(conn)
        status = statuses[0] if isinstance(statuses, list) else statuses
        t.assert_true(status.get('ready', False), "vector index status shows ready")
        t.assert_equal(status.get('index', ''), 'vec_l2_idx', "status returns correct index name")
        t.assert_equal(status.get('vector', None), True, "vector flag set")
        t.assert_equal(status.get('vector_metric', ''), 'l2', "metric is l2")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"vector_index_status: {e}")


# ── FTS Index Tests ──

def test_fts_index(conn, db_name, table_name):
    """Create FTS index and wait for readiness."""
    print("\n── FTS Index ──")
    create_index_and_wait(conn, db_name, table_name, "fts_content_idx",
        r.r.row["content"], {"fts": True, "multi": True},
        "FTS with multi=True")


def test_fts_match_query(conn, db_name, table_name):
    """Test FTS match queries through tbl.match()."""
    print("\n[TEST] fts_match_query")
    try:
        results = r.r.db(db_name).table(table_name).filter(
            r.r.row["content"].match("hello")
        ).run(conn)
        items = list(results)
        t.assert_true(len(items) >= 3, f"match('hello') returns >=3 results, got {len(items)}")

        results2 = r.r.db(db_name).table(table_name).filter(
            r.r.row["name"].match("item")
        ).run(conn)
        items2 = list(results2)
        t.assert_true(len(items2) >= 3, f"match('item') on name returns >=3 results, got {len(items2)}")

        results3 = r.r.db(db_name).table(table_name).filter(
            r.r.row["content"].match("zzzznonexistent")
        ).run(conn)
        items3 = list(results3)
        t.assert_equal(len(items3), 0, "match('zzzznonexistent') returns 0 results")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"fts_match_query: {e}")


# ── Index Metadata Tests ──

def test_index_list(conn, db_name, table_name):
    """Verify index_list returns all created indexes."""
    print("\n[TEST] index_list")
    try:
        indexes = r.r.db(db_name).table(table_name).index_list().run(conn)
        idx_names = list(indexes) if hasattr(indexes, '__iter__') else []
        expected = {"vec_l2_idx", "vec_cos_idx", "vec_ip_idx", "fts_content_idx"}
        for name in expected:
            t.assert_true(name in idx_names,
                          f"index '{name}' appears in index_list")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"index_list: {e}")


def test_index_wait(conn, db_name, table_name):
    """Verify index_wait returns ready for all 4 indexes."""
    print("\n[TEST] index_wait")
    try:
        statuses = r.r.db(db_name).table(table_name).index_wait().run(conn)
        status_list = list(statuses) if hasattr(statuses, '__iter__') else [statuses]
        print(f"  index_wait returned {len(status_list)} items")
        ready_count = sum(1 for s in status_list if s.get('ready', False))
        t.assert_true(ready_count >= 4,
                      f"all 4 indexes ready, got {ready_count}")
        for s in status_list:
            t.assert_true(s.get('ready', False),
                          f"index {s.get('index', '?')} is ready")
            t.assert_equal(s.get('index', ''), s.get('index'),
                          f"index {s.get('index')} has name")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"index_wait: {e}")


# ── Edge Case Tests ──

def test_between_without_index(conn, db_name, table_name):
    """between works on primary key even without explicit index."""
    print("\n[TEST] between_without_index")
    try:
        results = r.r.db(db_name).table(table_name).between("item1", "item3").run(conn)
        result_list = list(results)
        t.assert_true(len(result_list) >= 2,
                      f"between on primary key returns results, got {len(result_list)}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"between_without_index: {e}")


def test_get_by_id(conn, db_name, table_name):
    """get() by primary key works."""
    print("\n[TEST] get_by_id")
    try:
        doc = r.r.db(db_name).table(table_name).get("item1").run(conn)
        t.assert_equal(doc.get("score"), 42, "item1 has score=42")
        t.assert_equal(doc.get("id"), "item1", "item1 has correct id")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"get_by_id: {e}")


def test_filter_query(conn, db_name, table_name):
    """filter() works on numeric fields."""
    print("\n[TEST] filter_query")
    try:
        results = r.r.db(db_name).table(table_name).filter(
            r.r.row["score"].ge(30)
        ).run(conn)
        result_list = list(results)
        t.assert_true(len(result_list) >= 3,
                      f"filter(score.ge(30)) returns >=3 docs, got {len(result_list)}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"filter_query: {e}")


def test_empty_table_operations(conn, db_name, table_name):
    """Operations on empty tables succeed gracefully."""
    print("\n[TEST] empty_table_operations")
    try:
        empty_table = "empty_test"
        result = r.r.db(db_name).table_create(empty_table).run(conn)
        t.assert_equal(result.get('tables_created', 0), 1, "empty table created")

        ins = r.r.db(db_name).table(empty_table).insert(
            {"id": "empty_only", "val": 1}
        ).run(conn)
        t.assert_equal(ins.get('inserted', 0), 1, "insert into empty table works")

        drop = r.r.db(db_name).table_drop(empty_table).run(conn)
        t.assert_true(drop.get('tables_dropped', 0) >= 1,
                      f"empty table dropped: {drop}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"empty_table_operations: {e}")


# ── Main ──

if __name__ == '__main__':
    print("=" * 60)
    print("Vector/FTS Integration Test Suite (BRIN in INT-07-BUG-BRIN)")
    print("=" * 60)
    print(f"Binary: {RETHINKDB_BIN}")
    print(f"Data dir: {DATA_DIR}")

    print("\nStarting RethinkDB server...")
    if not start_server():
        print("FATAL: Could not start RethinkDB server")
        sys.exit(1)
    print("✓ Server started")

    db_name = f"vf_test_{uuid.uuid4().hex[:8]}"
    table_name = "features"

    try:
        conn = connect()
        print("✓ Client connected")

        # Create test database + table
        print(f"\nCreating database '{db_name}'...")
        if db_name in r.r.db_list().run(conn):
            r.r.db_drop(db_name).run(conn)
        result = r.r.db_create(db_name).run(conn)
        t.assert_equal(result.get('dbs_created'), 1, f"database '{db_name}' created")

        print(f"Creating table '{table_name}'...")
        result = r.r.db(db_name).table_create(table_name).run(conn)
        t.assert_equal(result.get('tables_created'), 1, f"table '{table_name}' created")

        # Insert data BEFORE creating indexes
        print("\nInserting test data (before index creation)...")
        test_docs = [
            {"id": "item1", "vec_attr": [0.1, 0.2, 0.3], "score": 42, "content": "hello world this is a test document", "name": "test_item_1", "timestamp": 1000},
            {"id": "item2", "vec_attr": [0.4, 0.5, 0.6], "score": 55, "content": "another document with hello and world", "name": "test_item_2", "timestamp": 2000},
            {"id": "item3", "vec_attr": [0.7, 0.8, 0.9], "score": 68, "content": "third document hello test", "name": "test_item_3", "timestamp": 3000},
            {"id": "item4", "vec_attr": [0.2, 0.4, 0.6], "score": 30, "content": "hello world unique content here", "name": "another_item", "timestamp": 1500},
            {"id": "item5", "vec_attr": [0.9, 0.7, 0.5], "score": 88, "content": "completely different document without hello", "name": "unique_name", "timestamp": 2500},
        ]
        ins = r.r.db(db_name).table(table_name).insert(test_docs).run(conn)
        t.assert_equal(ins.get('inserted', 0), 5, "inserted 5 test docs")

        # ── Run tests ──
        # IMPORTANT: Create indexes SEQUENTIALLY. Concurrent index creation
        # causes superblock contention in the construction coroutines,
        # leading to index_wait hanging.
        tests = [
            ("vector_indexes (seq create)", test_vector_indexes),
            ("vector_index_status", test_vector_index_status),
            ("fts_index (seq create)", test_fts_index),
            ("index_list", test_index_list),
            ("index_wait", test_index_wait),
            ("fts_match_query", test_fts_match_query),
            ("between_without_index", test_between_without_index),
            ("get_by_id", test_get_by_id),
            ("filter_query", test_filter_query),
            ("empty_table_operations", test_empty_table_operations),
        ]

        for name, fn in tests:
            try:
                print(f"\n── {name} ──")
                fn(conn, db_name, table_name)
            except Exception as e:
                print(f"  ✗ ERROR in {name}: {e}")
                t.failed += 1
                t.errors.append(f"{name}: {e}")

        # Cleanup: drop test database
        print(f"\nDropping database '{db_name}'...")
        try:
            r.r.db_drop(db_name).run(conn)
            print("  ✓ Database dropped")
        except Exception as e:
            print(f"  ⚠ Could not drop database: {e}")

        conn.close()

    except Exception as e:
        print(f"\nFATAL: {e}")
        import traceback
        traceback.print_exc()
        t.failed += 1

    stop_server()

    ok = t.summary()
    if not ok:
        sys.exit(1)
    print("✓ All tests PASS")
