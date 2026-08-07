# Verdict: RT-BUG-002

**Task:** Fix PartitionOpsTest.PkDirectoryInsertLookupExistsRemove full-suite hang (pk_directory init self-deadlock + superblock-before-commit)
**Evaluated:** 2026-08-07T01:39:32.268983
**Result:** ✗ FAIL

## Pipeline Stages

- ✗ **tier1**
  -   ✓ build:     [1/1] BUILD quickjs_0.15.1
make[1]: warning: undefined variable 'GNUMAKEFLAGS'

  ✓ secrets: 
    ○
    │╲
    │ ○
    ○ ░
    ░    gitleaks

[90m8:33PM[0m [32mINF[0m [1mscanned ~75534445 
  ✗ lint: Done processing ./src/clustering/generic/multi_client_client.cc
Done processing ./src/containers/bac
  ✓ tests: Note: Google Test filter = *Cdc*:*Vector*:*Fts*:*Brin*:*Sindex*:*Term*:*Btree*:*Reql*
[==========] R
- ✓ **tier2**
  - COMPLETE
  ✓ Isolated PartitionOpsTest.PkDirectoryInsertLookupExistsRemove completes with PASS (no hang, no EXIT=137) on fresh binary: Ran `./build/release/rethinkdb-unittest --gtest_filter='PartitionOpsTest.PkDirectoryInsertLookupExistsRemove'` on fresh binary (built 19:32:51 after source edits 19:25/19:32) → [PASSED] 1 test in 82ms, no hang, no EXIT=137. Worker log /tmp/rt-bug002-worker.log (a): 'COMPLETES in 41ms (was hanging >90s, EXIT=137)'.
  ✓ PartitionOpsTest.* 13/13 PASS: Ran `--gtest_filter='PartitionOpsTest.*'` → '[==========] 13 tests from 1 test case ran. [PASSED] 13 tests.' Also confirmed in /tmp/full_suite.log and /tmp/rt-full-unittest2.log (13 tests all OK).
  ✓ Regression+TS combined filter 255/255 PASS: /tmp/rt-t100-reg255.log: '[==========] 255 tests from 26 test cases ran. [PASSED] 255 tests. EXIT=0'.
  ✓ Full unit suite 798/798 completes with PASSED summary (passes test #404, no hang): /tmp/full_suite.log and /tmp/rt-full-unittest2.log: '[==========] 798 tests from 104 test cases ran. [PASSED] 798 tests.' Test #404 PartitionOpsTest.PkDirectoryInsertLookupExistsRemove OK (46ms/75ms), no hang.
  ✓ pk_directory_t::init() no longer self-deadlocks (no second write lock on same block_id while first held): src/btree/pk_directory.cc init() no longer calls save(). save() (line 112) creates `buf_lock_t dir_block(parent, dir_block_id, access_t::write)` — a second write lock on the same block_id while `block` from allocate_dir_block is still held. Fix removes the save() call, eliminating the self-deadlock.
  ✓ All partition_ops_test.cc TPTEST sites release superblock before txn->commit() (live_acqs_ == 0 invariant): All 11 txn->commit() sites (lines 231,258,298,339,373,396,423,447,465,490,556) each immediately preceded by superblock.reset(). Verified via awk context check on HEAD:src/unittest/partition_ops_test.cc.
  ✓ Pre-TS-2 A/B: hang proven predating PHASE3-TS-2 (pk_directory.cc diff empty PART-08 -> HEAD): `git diff 40b395cd03(PART-08) 1019010b79(pre-fix parent) -- src/btree/pk_directory.cc` is EMPTY; `git log 40b395cd03..1019010b79 -- pk_directory.cc` empty. Bug code byte-identical from PART-08 (07-16) through TS-2 (08-04) to pre-fix (08-06), proving hang predates PHASE3-TS-2.
  ✓ gitreins guard PASS: /tmp/guard2.log (19:51), /tmp/rt-t100-guard.log (20:23), /tmp/guard.log (20:27) all show 'Tier 1 Guards: PASS (secrets clean, lint ok, tests) GUARD_EXIT=0'. Worker log (e) confirms guard PASS.
  ✓ Commit f357ff4c4f scoped to src/btree/pk_directory.cc + src/unittest/partition_ops_test.cc with Co-authored-by trailer: `git show --name-only f357ff4c4f` → only src/btree/pk_directory.cc (+6/-3) and src/unittest/partition_ops_test.cc (+11/0). Commit body contains 'Co-authored-by: Hermes Agent <hermes-agent@nousresearch.com>'.
All 9 criteria verified PASS: the pk_directory init() self-deadlock fix (removed redundant save() second write-lock) plus superblock-before-commit resets in all 11 TPTEST sites resolve the full-suite hang, confirmed by isolated 1-test PASS, PartitionOpsTest 13/13, regression+TS 255/255, full suite 798/798 (test #404 passes), empty PART-08→pre-fix pk_directory.cc diff proving pre-TS-2 provenance, gitreins guard PASS, and a properly scoped commit f357ff4c4f with Co-authored-by trailer.
- ✓ **tier3**
  -   ✓ cppcheck: === cppcheck static analysis ===
cppcheck: error: could not find or open any of the paths given.
/bi
  ✓ clang-tidy: Command timed out

## Summary

Judge Result: RT-BUG-002

Stage tier1: FAIL
    ✓ build:     [1/1] BUILD quickjs_0.15.1
make[1]: warning: undefined variable 'GNUMAKEFLAGS'

  ✓ secrets: 
    ○
    │╲
    │ ○
    ○ ░
    ░    gitleaks

[90m8:33PM[0m [32mINF[0m [1mscanned ~75534445 
  ✗ lint: Done processing ./src/clustering/generic/multi_client_client.cc
Done processing ./src/containers/bac
  ✓ tests: Note: Google Test filter = *Cdc*:*Vector*:*Fts*:*Brin*:*Sindex*:*Term*:*Btree*:*Reql*
[==========] R

Stage tier2: PASS
  COMPLETE
  ✓ Isolated PartitionOpsTest.PkDirectoryInsertLookupExistsRemove completes with PASS (no hang, no EXIT=137) on fresh binary: Ran `./build/release/rethinkdb-unittest --gtest_filter='PartitionOpsTest.PkDirectoryInsertLookupExistsRemove'` on fresh binary (built 19:32:51 after source edits 19:25/19:32) → [PASSED] 1 test in 82ms, no hang, no EXIT=137. Worker log /tmp/rt-bug002-worker.log (a): 'COMPLETES in 41ms (was hanging >90s, EXIT=137)'.
  ✓ PartitionOpsTest.* 13/13 PASS: Ran `--gtest_filter='PartitionOpsTest.*'` → '[==========] 13 tests from 1 test case ran. [PASSED] 13 tests.' Also confirmed in /tmp/full_suite.log and /tmp/rt-full-unittest2.log (13 tests all OK).
  ✓ Regression+TS combined filter 255/255 PASS: /tmp/rt-t100-reg255.log: '[==========] 255 tests from 26 test cases ran. [PASSED] 255 tests. EXIT=0'.
  ✓ Full unit suite 798/798 completes with PASSED summary (passes test #404, no hang): /tmp/full_suite.log and /tmp/rt-full-unittest2.log: '[==========] 798 tests from 104 test cases ran. [PASSED] 798 tests.' Test #404 PartitionOpsTest.PkDirectoryInsertLookupExistsRemove OK (46ms/75ms), no hang.
  ✓ pk_directory_t::init() no longer self-deadlocks (no second write lock on same block_id while first held): src/btree/pk_directory.cc init() no longer calls save(). save() (line 112) creates `buf_lock_t dir_block(parent, dir_block_id, access_t::write)` — a second write lock on the same block_id while `block` from allocate_dir_block is still held. Fix removes the save() call, eliminating the self-deadlock.
  ✓ All partition_ops_test.cc TPTEST sites release superblock before txn->commit() (live_acqs_ == 0 invariant): All 11 txn->commit() sites (lines 231,258,298,339,373,396,423,447,465,490,556) each immediately preceded by superblock.reset(). Verified via awk context check on HEAD:src/unittest/partition_ops_test.cc.
  ✓ Pre-TS-2 A/B: hang proven predating PHASE3-TS-2 (pk_directory.cc diff empty PART-08 -> HEAD): `git diff 40b395cd03(PART-08) 1019010b79(pre-fix parent) -- src/btree/pk_directory.cc` is EMPTY; `git log 40b395cd03..1019010b79 -- pk_directory.cc` empty. Bug code byte-identical from PART-08 (07-16) through TS-2 (08-04) to pre-fix (08-06), proving hang predates PHASE3-TS-2.
  ✓ gitreins guard PASS: /tmp/guard2.log (19:51), /tmp/rt-t100-guard.log (20:23), /tmp/guard.log (20:27) all show 'Tier 1 Guards: PASS (secrets clean, lint ok, tests) GUARD_EXIT=0'. Worker log (e) confirms guard PASS.
  ✓ Commit f357ff4c4f scoped to src/btree/pk_directory.cc + src/unittest/partition_ops_test.cc with Co-authored-by trailer: `git show --name-only f357ff4c4f` → only src/btree/pk_directory.cc (+6/-3) and src/unittest/partition_ops_test.cc (+11/0). Commit body contains 'Co-authored-by: Hermes Agent <hermes-agent@nousresearch.com>'.
All 9 criteria verified PASS: the pk_directory init() self-deadlock fix (removed redundant save() second write-lock) plus superblock-before-commit resets in all 11 TPTEST sites resolve the full-suite hang, confirmed by isolated 1-test PASS, PartitionOpsTest 13/13, regression+TS 255/255, full suite 798/798 (test #404 passes), empty PART-08→pre-fix pk_directory.cc diff proving pre-TS-2 provenance, gitreins guard PASS, and a properly scoped commit f357ff4c4f with Co-authored-by trailer.

Stage tier3: PASS
    ✓ cppcheck: === cppcheck static analysis ===
cppcheck: error: could not find or open any of the paths given.
/bi
  ✓ clang-tidy: Command timed out

Overall: FAIL ✗
