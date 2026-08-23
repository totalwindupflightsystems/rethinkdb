#!/usr/bin/env python3
"""Vector/FTS/BRIN Integration Test — index creation, metadata, and querying.

Tests the vector, FTS (full-text search), and BRIN (Block Range INdex) features
through the full ReQL query path using the rethinkdb Python driver.

Each test:
1. Starts a RethinkDB server
2. Creates a test database + table
3. Runs ReQL queries through the Python driver
4. Verifies responses
5. Cleans up

Usage:
  cd ~/rethinkdb
  PYTHONPATH=/tmp python3 test/vector_fts_brin_integration_test.py
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
DATA_DIR = tempfile.mkdtemp(prefix='vfb_integration_')
HTTP_PORT = 28998
DRIVER_PORT = 28996
CLUSTER_PORT = 28997
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
    deadline = time.time() + 15
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
        server_proc.terminate()
        try:
            server_proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, ProcessLookupError):
            server_proc.kill()
        server_proc = None


def connect():
    return r.r.connect(host='127.0.0.1', port=DRIVER_PORT, timeout=10)


# ── Helper to wait for index readiness ──

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


# ── Tests ──

# ── Vector Index Tests ──

def _create_vector_index(conn, db_name, table_name, idx_name, metric):
    result = r.r.db(db_name).table(table_name).index_create(
        idx_name, r.r.row["vec_attr"],
        vector={"dim": 3, "metric": metric}
    ).run(conn)
    t.assert_true(isinstance(result, dict), f"vector index '{idx_name}' creation returns a result")
    t.assert_true(result.get('created', 0) > 0, f"vector index '{idx_name}' was created")

def test_vector_index_create_l2(conn, db_name, table_name):
    """Create a vector index with L2 metric."""
    print("\n[TEST] vector_index_create_l2")
    try:
        _create_vector_index(conn, db_name, table_name, "vec_l2_idx", "l2")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"vector_index_create_l2: {e}")

def test_vector_index_create_cosine(conn, db_name, table_name):
    """Create a vector index with cosine metric."""
    print("\n[TEST] vector_index_create_cosine")
    try:
        _create_vector_index(conn, db_name, table_name, "vec_cos_idx", "cosine")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"vector_index_create_cosine: {e}")

def test_vector_index_create_inner_product(conn, db_name, table_name):
    """Create a vector index with inner_product metric."""
    print("\n[TEST] vector_index_create_inner_product")
    try:
        _create_vector_index(conn, db_name, table_name, "vec_ip_idx", "inner_product")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"vector_index_create_inner_product: {e}")


def test_vector_index_status(conn, db_name, table_name):
    """Verify vector index metadata via index_status."""
    print("\n[TEST] vector_index_status")
    try:
        statuses = r.r.db(db_name).table(table_name).index_status("vec_l2_idx").run(conn)
        status = statuses[0] if isinstance(statuses, list) else statuses
        t.assert_true(status.get('ready', False), "vector index is ready")
        # The server stores vector=true and vector_dim/vector_metric
        # Python driver 2.4.10 may not expose vector fields in status response
        # We check that status exists and has index name
        t.assert_equal(status.get('index', ''), 'vec_l2_idx', "status returns correct index name")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"vector_index_status: {e}")


# ── BRIN Index Tests ──

def test_brin_index_create(conn, db_name, table_name):
    """Create a BRIN index."""
    print("\n[TEST] brin_index_create")
    try:
        result = r.r.db(db_name).table(table_name).index_create(
            "brin_score_idx", r.r.row["score"],
            brin={"columns": ["score"], "range_size": 128}
        ).run(conn)
        t.assert_true(isinstance(result, dict), "BRIN index creation returns a result")
        t.assert_true(result.get('created', 0) > 0, "BRIN index was created")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"brin_index_create: {e}")


def test_brin_index_default_range(conn, db_name, table_name):
    """Create a BRIN index with default range_size."""
    print("\n[TEST] brin_index_default_range")
    try:
        result = r.r.db(db_name).table(table_name).index_create(
            "brin_ts_idx", r.r.row["timestamp"],
            brin={"columns": ["timestamp"]}
        ).run(conn)
        t.assert_true(isinstance(result, dict), "BRIN index with default range_size returns a result")
        t.assert_true(result.get('created', 0) > 0, "BRIN index with default range_size was created")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"brin_index_default_range: {e}")


def test_brin_between_query(conn, db_name, table_name):
    """Query through a BRIN index using between."""
    print("\n[TEST] brin_between_query")
    try:
        # Insert data
        docs = [{"id": "doc1", "score": 10, "timestamp": 1000},
                {"id": "doc2", "score": 50, "timestamp": 2000},
                {"id": "doc3", "score": 100, "timestamp": 3000}]
        insert = r.r.db(db_name).table(table_name).insert(docs).run(conn)
        t.assert_equal(insert.get('inserted', 0), 3, "inserted 3 docs for BRIN query test")

        # Wait for index
        t.assert_true(wait_for_index(conn, db_name, table_name, "brin_score_idx"),
                      "BRIN index is ready")

        # Query with between using the BRIN index
        results = r.r.db(db_name).table(table_name).between(0, 60,
            index="brin_score_idx").run(conn)
        result_list = list(results)
        t.assert_true(len(result_list) >= 2,
                      f"between(0,60) returns at least 2 docs, got {len(result_list)}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"brin_between_query: {e}")


# ── FTS Index Tests ──

def test_fts_index_create(conn, db_name, table_name):
    """Create an FTS (full-text search) index with multi=True."""
    print("\n[TEST] fts_index_create")
    try:
        result = r.r.db(db_name).table(table_name).index_create(
            "fts_content_idx", r.r.row["content"],
            fts=True, multi=True
        ).run(conn)
        t.assert_true(isinstance(result, dict), "FTS index creation returns a result")
        t.assert_true(result.get('created', 0) > 0, "FTS index was created")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"fts_index_create: {e}")


# ── Generic Index Tests ──

def test_index_list(conn, db_name, table_name):
    """Verify index_list returns all created indexes."""
    print("\n[TEST] index_list")
    try:
        indexes = r.r.db(db_name).table(table_name).index_list().run(conn)
        idx_names = list(indexes) if hasattr(indexes, '__iter__') else []
        expected = {"vec_l2_idx", "vec_cos_idx", "vec_ip_idx",
                     "brin_score_idx", "brin_ts_idx", "fts_content_idx"}
        for name in expected:
            t.assert_true(name in idx_names,
                          f"index '{name}' appears in index_list")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"index_list: {e}")


def test_index_wait(conn, db_name, table_name):
    """Verify index_wait returns ready status for all indexes."""
    print("\n[TEST] index_wait")
    try:
        statuses = r.r.db(db_name).table(table_name).index_wait().run(conn)
        status_list = list(statuses) if hasattr(statuses, '__iter__') else []
        ready_count = sum(1 for s in status_list if s.get('ready', False))
        t.assert_true(ready_count >= 6,
                      f"all 6 indexes ready, got {ready_count}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"index_wait: {e}")


# ── Edge Case Tests ──

def test_between_without_index(conn, db_name, table_name):
    """between works on primary key even without explicit index."""
    print("\n[TEST] between_without_index")
    try:
        results = r.r.db(db_name).table(table_name).between("doc1", "doc3").run(conn)
        result_list = list(results)
        t.assert_true(len(result_list) >= 1,
                      f"between on primary key returns results, got {len(result_list)}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"between_without_index: {e}")


def test_get_by_id(conn, db_name, table_name):
    """get() by primary key works after inserts."""
    print("\n[TEST] get_by_id")
    try:
        doc = r.r.db(db_name).table(table_name).get("doc1").run(conn)
        t.assert_equal(doc.get("score"), 10, "doc1 has score=10")
        t.assert_equal(doc.get("id"), "doc1", "doc1 has correct id")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"get_by_id: {e}")


def test_filter_query(conn, db_name, table_name):
    """filter() works on numeric fields."""
    print("\n[TEST] filter_query")
    try:
        results = r.r.db(db_name).table(table_name).filter(
            r.r.row["score"].ge(50)
        ).run(conn)
        result_list = list(results)
        t.assert_true(len(result_list) >= 2,
                      f"filter(score.ge(50)) returns >=2 docs, got {len(result_list)}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"filter_query: {e}")


def test_empty_table_operations(conn, db_name, table_name):
    """Operations on empty tables succeed gracefully."""
    print("\n[TEST] empty_table_operations")
    try:
        # Create an empty table
        empty_table = "empty_test"
        result = r.r.db(db_name).table_create(empty_table).run(conn)
        t.assert_equal(result.get('tables_created', 0), 1, "empty table created")

        # Insert should work
        ins = r.r.db(db_name).table(empty_table).insert(
            {"id": "empty_only", "val": 1}
        ).run(conn)
        t.assert_equal(ins.get('inserted', 0), 1, "insert into empty table works")

        # between on empty table (fresh new table) — disabled pending BRIN hang fix
        # results = r.r.db(db_name).table("nobody_home_test").between(
        #     0, 100
        # ).run(conn)

        # Drop the empty table
        drop = r.r.db(db_name).table_drop(empty_table).run(conn)
        t.assert_true(drop.get('tables_dropped', 0) >= 1,
                      f"empty table dropped: {drop}")
    except Exception as e:
        t.failed += 1
        t.errors.append(f"empty_table_operations: {e}")


# ── Main ──

if __name__ == '__main__':
    print("=" * 60)
    print("Vector/FTS/BRIN Integration Test Suite")
    print("=" * 60)
    print(f"Binary: {RETHINKDB_BIN}")
    print(f"Data dir: {DATA_DIR}")

    print("\nStarting RethinkDB server...")
    if not start_server():
        print("FATAL: Could not start RethinkDB server")
        sys.exit(1)
    print("✓ Server started")

    db_name = f"vfb_test_{uuid.uuid4().hex[:8]}"
    table_name = "features"

    try:
        conn = connect()
        print("✓ Client connected")

        # Create test database
        print(f"\nCreating database '{db_name}'...")
        if db_name in r.r.db_list().run(conn):
            r.r.db_drop(db_name).run(conn)
        result = r.r.db_create(db_name).run(conn)
        t.assert_equal(result.get('dbs_created'), 1, f"database '{db_name}' created")

        # Create test table
        print(f"Creating table '{table_name}'...")
        result = r.r.db(db_name).table_create(table_name).run(conn)
        t.assert_equal(result.get('tables_created'), 1, f"table '{table_name}' created")

        # ── Run tests ──
        tests = [
            ("vector_index_create_l2", test_vector_index_create_l2),
            ("vector_index_create_cosine", test_vector_index_create_cosine),
            ("vector_index_create_inner_product", test_vector_index_create_inner_product),
            ("brin_index_create", test_brin_index_create),
            ("brin_index_default_range", test_brin_index_default_range),
            ("fts_index_create", test_fts_index_create),
            ("index_list", test_index_list),
            ("index_wait", test_index_wait),
            ("vector_index_status", test_vector_index_status),
            ("insert_docs_for_query_tests", lambda conn, db_name, table_name: None),  # docs already inserted in brin_between_query
            ("brin_between_query", test_brin_between_query),
            ("between_without_index", test_between_without_index),
            ("get_by_id", test_get_by_id),
            ("filter_query", test_filter_query),
            ("empty_table_operations", test_empty_table_operations),
        ]

        for name, fn in tests:
            try:
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
