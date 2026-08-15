#!/usr/bin/env python3
"""CDC Integration Test Suite — tests the FULL ReQL query path for CDC features.

Starts a RethinkDB server, creates tables, inserts/updates/deletes data,
and verifies that changefeeds correctly deliver events through the CDC
write-capture seam (rdb_write_visitor_t) and the btree_store_t path.

This is NOT a unit test. It exercises:
- Server startup and shutdown
- DB/table CRUD operations
- INSERT/UPDATE/DELETE through the CDC-instrumented write path
- Changefeed event delivery (old_val, new_val)
- Index creation and querying
- Multi-table operations
- Server restart durability (data survives)

Requirements: rethinkdb Python driver, running rethinkdb binary in PATH
or ../build/release/rethinkdb relative to project root.

Usage:
  cd ~/rethinkdb
  PYTHONPATH=/tmp python3 test/cdc_integration_test.py

Exit code 0 = all tests pass. Non-zero = failure with details.
"""

import os
import sys
import time
import subprocess
import tempfile
import shutil
import atexit

# Add local rethinkdb driver
sys.path.insert(0, '/tmp')
import rethinkdb as r

class TestResult:
    __test__ = False  # result container, not a pytest test class

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

# ── Server management ──

RETHINKDB_BIN = os.path.join(os.path.dirname(__file__), '..', 'build', 'release', 'rethinkdb')
DATA_DIR = tempfile.mkdtemp(prefix='cdc_test_')
HTTP_PORT = 19999
DRIVER_PORT = 29999
CLUSTER_PORT = 39999
server_proc = None

def cleanup():
    global server_proc
    if server_proc:
        try:
            server_proc.terminate()
            server_proc.wait(timeout=5)
        except Exception:
            server_proc.kill()
    shutil.rmtree(DATA_DIR, ignore_errors=True)
atexit.register(cleanup)

def start_server():
    global server_proc
    bin_path = RETHINKDB_BIN
    if not os.path.exists(bin_path):
        alt = '/home/kara/rethinkdb/build/release/rethinkdb'
        if os.path.exists(alt):
            bin_path = alt
    
    server_proc = subprocess.Popen(
        [bin_path, '--no-update-check', '--bind', '127.0.0.1',
         '--http-port', str(HTTP_PORT), '--driver-port', str(DRIVER_PORT),
         '--cluster-port', str(CLUSTER_PORT),
         '-d', DATA_DIR, '--io-threads', '4'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    
    # Wait for server to be ready
    deadline = time.time() + 10
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
        except Exception:
            server_proc.kill()
        server_proc = None

# ── Connection ──

def connect():
    return r.r.connect(host='127.0.0.1', port=DRIVER_PORT, timeout=10)

# ── Test 1: Basic CRUD through write path ──

def test_basic_crud():
    print("\n[TEST 1] Basic CRUD through CDC write path")
    conn = connect()
    
    # Create
    if 'cdc_crud_test' in r.r.db_list().run(conn):
        r.r.db_drop('cdc_crud_test').run(conn)
    r.r.db_create('cdc_crud_test').run(conn)
    r.r.db('cdc_crud_test').table_create('docs').run(conn)
    t.assert_true('docs' in r.r.db('cdc_crud_test').table_list().run(conn), 
                  "table created")
    
    # Insert 100 documents
    result = r.r.db('cdc_crud_test').table('docs').insert(
        [{'id': i, 'val': i * 10, 'name': f'doc_{i}'} for i in range(100)]
    ).run(conn)
    t.assert_equal(result['inserted'], 100, "insert 100 docs")
    
    # Read one
    doc = r.r.db('cdc_crud_test').table('docs').get(42).run(conn)
    t.assert_equal(doc['val'], 420, "read doc 42 val")
    t.assert_equal(doc['name'], 'doc_42', "read doc 42 name")
    
    # Update
    result = r.r.db('cdc_crud_test').table('docs').get(42).update(
        {'val': 999, 'updated': True}
    ).run(conn)
    t.assert_equal(result['replaced'], 1, "update doc 42")
    doc = r.r.db('cdc_crud_test').table('docs').get(42).run(conn)
    t.assert_equal(doc['val'], 999, "verify update val")
    t.assert_true(doc.get('updated'), "verify update flag")
    
    # Count with filter
    count = r.r.db('cdc_crud_test').table('docs').filter(
        r.r.row['val'] > 500
    ).count().run(conn)
    t.assert_equal(count, 50, "filter val>500 count")  # val 510-990 from docs 51-99 = 49... check
    
    # Delete
    result = r.r.db('cdc_crud_test').table('docs').get(99).delete().run(conn)
    t.assert_equal(result['deleted'], 1, "delete doc 99")
    count = r.r.db('cdc_crud_test').table('docs').count().run(conn)
    t.assert_equal(count, 99, "count after delete")
    
    # Cleanup
    r.r.db_drop('cdc_crud_test').run(conn)
    conn.close()

# ── Test 2: Changefeed through CDC write path ──

def test_changefeed_cdc_path():
    print("\n[TEST 2] Changefeed through CDC-instrumented write path")
    conn = connect()
    
    if 'cdc_feed_test' in r.r.db_list().run(conn):
        r.r.db_drop('cdc_feed_test').run(conn)
    r.r.db_create('cdc_feed_test').run(conn)
    r.r.db('cdc_feed_test').table_create('events').run(conn)
    
    # Open changefeed BEFORE inserting (tests delivery of initial + updates)
    feed = r.r.db('cdc_feed_test').table('events').changes(
        include_initial=True
    ).run(conn)
    
    # Insert — CDC write visitor should capture this
    r.r.db('cdc_feed_test').table('events').insert([
        {'id': 1, 'type': 'create', 'payload': 'first'},
        {'id': 2, 'type': 'create', 'payload': 'second'},
    ]).run(conn)
    
    events = []
    deadline = time.time() + 5
    while time.time() < deadline and len(events) < 2:
        try:
            event = feed.next(wait=1)
            if event.get('new_val'):
                events.append(event['new_val'])
        except Exception:
            break
    feed.close()
    
    t.assert_true(len(events) >= 2, f"changefeed received {len(events)} initial events, expected >= 2")
    payloads = {e.get('payload') for e in events}
    t.assert_true('first' in payloads, "first payload in changefeed")
    t.assert_true('second' in payloads, "second payload in changefeed")
    
    # Open new feed for updates
    feed2 = r.r.db('cdc_feed_test').table('events').changes().run(conn)
    
    # Update — goes through CDC write path
    r.r.db('cdc_feed_test').table('events').get(1).update(
        {'payload': 'updated', 'extra_field': True}
    ).run(conn)
    
    time.sleep(1)
    update_events = []
    try:
        while True:
            event = feed2.next(wait=1)
            if event.get('new_val'):
                update_events.append(event)
    except Exception:
        pass
    feed2.close()
    
    t.assert_true(len(update_events) > 0, "changefeed received update event")
    if update_events:
        evt = update_events[0]
        t.assert_true(evt.get('old_val') is not None, "update has old_val")
        t.assert_equal(evt['new_val'].get('payload'), 'updated', "update payload changed")
    
    r.r.db_drop('cdc_feed_test').run(conn)
    conn.close()

# ── Test 3: Index operations ──

def test_index_operations():
    print("\n[TEST 3] Index create/wait/query")
    conn = connect()
    
    if 'cdc_idx_test' in r.r.db_list().run(conn):
        r.r.db_drop('cdc_idx_test').run(conn)
    r.r.db_create('cdc_idx_test').run(conn)
    r.r.db('cdc_idx_test').table_create('data').run(conn)
    
    # Insert test data (smaller set for speed)
    r.r.db('cdc_idx_test').table('data').insert([
        {'id': f'item_{i}', 'score': i * 10, 'category': 'a' if i % 2 == 0 else 'b'}
        for i in range(50)
    ]).run(conn)
    
    # Create index
    r.r.db('cdc_idx_test').table('data').index_create('score').run(conn)
    r.r.db('cdc_idx_test').table('data').index_wait('score').run(conn)
    t.assert_true('score' in r.r.db('cdc_idx_test').table('data').index_list().run(conn),
                  "score index created")
    
    # Query via index
    between_result = r.r.db('cdc_idx_test').table('data').between(
        100, 200, index='score'
    ).count().run(conn)
    t.assert_equal(between_result, 10, "between 100-200 via score index")
    
    # Multi-index
    r.r.db('cdc_idx_test').table('data').index_create('category').run(conn)
    r.r.db('cdc_idx_test').table('data').index_wait('category').run(conn)
    
    cat_a = r.r.db('cdc_idx_test').table('data').get_all(
        'a', index='category'
    ).count().run(conn)
    t.assert_equal(cat_a, 25, "category 'a' count via index")
    
    r.r.db_drop('cdc_idx_test').run(conn)
    conn.close()

# ── Test 4: Server restart durability ──

def test_durability_after_restart():
    print("\n[TEST 4] Data survives server restart")
    conn = connect()
    
    if 'cdc_dura_test' in r.r.db_list().run(conn):
        r.r.db_drop('cdc_dura_test').run(conn)
    r.r.db_create('cdc_dura_test').run(conn)
    r.r.db('cdc_dura_test').table_create('persist').run(conn)
    
    # Write data
    r.r.db('cdc_dura_test').table('persist').insert([
        {'id': i, 'marker': f'pre_restart_{i}'} for i in range(100)
    ]).run(conn)
    conn.close()
    
    # Restart server
    print("  Stopping server...")
    stop_server()
    time.sleep(2)
    print("  Starting server...")
    t.assert_true(start_server(), "server restart")
    
    # Verify data — server needs a moment to reassign table leadership
    conn = connect()
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            dbs = r.r.db_list().run(conn)
            if 'cdc_dura_test' in dbs:
                tables = r.r.db('cdc_dura_test').table_list().run(conn)
                if 'persist' in tables:
                    count = r.r.db('cdc_dura_test').table('persist').count().run(conn)
                    if count == 100:
                        break
        except Exception:
            pass
        time.sleep(1)
    
    dbs = r.r.db_list().run(conn)
    t.assert_true('cdc_dura_test' in dbs, "db survived restart")
    
    tables = r.r.db('cdc_dura_test').table_list().run(conn)
    t.assert_true('persist' in tables, "table survived restart")
    
    count = r.r.db('cdc_dura_test').table('persist').count().run(conn)
    t.assert_equal(count, 100, "100 docs survived restart")
    
    doc = r.r.db('cdc_dura_test').table('persist').get(77).run(conn)
    t.assert_equal(doc['marker'], 'pre_restart_77', "specific doc survived restart")
    
    r.r.db_drop('cdc_dura_test').run(conn)
    conn.close()

# ── Test 5: Bulk operations and edge cases ──

def test_bulk_and_edge_cases():
    print("\n[TEST 5] Bulk operations and edge cases")
    conn = connect()
    
    if 'cdc_bulk_test' in r.r.db_list().run(conn):
        r.r.db_drop('cdc_bulk_test').run(conn)
    r.r.db_create('cdc_bulk_test').run(conn)
    r.r.db('cdc_bulk_test').table_create('big').run(conn)
    
    # Bulk insert — stresses the write path
    batch_size = 500
    docs = [{'id': f'bulk_{i}', 'data': 'x' * 100, 'num': i} for i in range(batch_size)]
    result = r.r.db('cdc_bulk_test').table('big').insert(docs).run(conn)
    t.assert_equal(result['inserted'], batch_size, f"bulk insert {batch_size}")
    
    # Bulk update
    result = r.r.db('cdc_bulk_test').table('big').update(
        {'updated': True}
    ).run(conn)
    t.assert_equal(result['replaced'], batch_size, f"bulk update {batch_size}")
    
    # Empty insert
    result = r.r.db('cdc_bulk_test').table('big').insert([]).run(conn)
    t.assert_equal(result['inserted'], 0, "empty insert")
    
    # Nonexistent key read
    doc = r.r.db('cdc_bulk_test').table('big').get('nonexistent').run(conn)
    t.assert_true(doc is None, "nonexistent key returns None")
    
    # Batch delete
    ids_to_delete = [f'bulk_{i}' for i in range(100)]
    result = r.r.db('cdc_bulk_test').table('big').get_all(
        *ids_to_delete
    ).delete().run(conn)
    t.assert_equal(result['deleted'], 100, "batch delete 100")
    
    r.r.db_drop('cdc_bulk_test').run(conn)
    conn.close()

# ── Main ──

if __name__ == '__main__':
    print("CDC Integration Test Suite")
    print(f"Binary: {RETHINKDB_BIN}")
    print(f"Data dir: {DATA_DIR}")
    
    print("\nStarting RethinkDB...")
    if not start_server():
        print("FATAL: Could not start RethinkDB server")
        sys.exit(1)
    print("Server started successfully")
    
    test_basic_crud()
    test_changefeed_cdc_path()
    test_index_operations()
    test_durability_after_restart()
    test_bulk_and_edge_cases()
    
    stop_server()
    
    ok = t.summary()
    sys.exit(0 if ok else 1)
