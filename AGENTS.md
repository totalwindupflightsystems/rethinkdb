# RethinkDB Project Overview

RethinkDB is an open-source, distributed NoSQL database designed for building realtime web applications. It stores schemaless JSON documents and provides features like automatic failover, robust fault tolerance, and continuous push of updated query results to applications without polling.

## Build and Test Commands

### Building
- Install dependencies (Ubuntu/Debian): `sudo apt-get install build-essential protobuf-compiler python3 python-is-python3 libprotobuf-dev libcurl4-openssl-dev libncurses5-dev libjemalloc-dev wget m4 g++ libssl-dev`
- Configure: `./configure --allow-fetch`
- Build: `make -j4`
- Install: `sudo make install`

### Testing
- Unit tests: `make unit`
- All tests: `make test`
- Run specific tests: `test/run --verbose --jobs <n> -H <test_type>`
  - Unit tests: `-H unit`
  - Integration tests: `-H all '!unit' '!cpplint' '!long' '!disabled'`

### Python driver integration tests (E2E probes in `test/`)

The python E2E probes (`test/merge_e2e_test.py`, `test/cluster_e2e_test.py`,
`test/ts2_e2e_probe.py`, `test/ts3_e2e_probe.py`, `test/ts4_e2e_probe.py`,
`test/ts6_chaos_probe.py`, `test/ts6_cluster_e2e_probe.py`) import the
vendored driver from `driver/python3/` rather than a pip-installed package.
The vendored path is wired in two places, both **repo-relative** (no
hardcoded `/home/kara/...` paths):

1. `conftest.py` (repo root) — `python3 -m pytest test/<file>.py -q`
   auto-adds `driver/python3` to `sys.path`. No `PYTHONPATH` needed.
2. The probes themselves — `python3 test/<file>.py` script-mode also
   works from any checkout via `os.path.dirname(__file__)`.

One-command setup (creates a venv, installs test deps, and verifies the
driver imports + pytest collection; live E2E runs still require a built
`build/release/rethinkdb` server):

```
python3 -m venv .venv \
  && .venv/bin/pip install -r test-requirements.txt \
  && .venv/bin/python -m pytest test/cdc_integration_test.py -q --collect-only
```

The collection check uses `test/cdc_integration_test.py` because it has
real pytest test functions (5 collected) and imports the vendored driver
at module level — a broken driver setup fails collection. The probe
files (`test/merge_e2e_test.py`, `test/cluster_e2e_test.py`, etc.) are
script-mode programs with no pytest test functions, so `--collect-only`
on them always reports "no tests collected" — run them directly instead
(see below).

**System-interpreter path: unsupported on this dev machine — use the venv path above.**
On machines where bare `python3`/`pip` resolve to tooling venvs (here: `pip` →
Hermes board venv python3.14, `python3 -m pip` → a gitreins-poc venv without pip,
and system `/usr/bin/python3.14` is PEP-668 externally-managed), the install lands
in the wrong interpreter's site-packages and collection fails with
`ModuleNotFoundError: No module named 'six'`. On a machine with a normal system
python (pip present, no PEP-668 restriction), the system path works with
interpreter-explicit commands:

```
python3 -m pip install --user -r test-requirements.txt   # system python deps (six, looseversion, pytest)
python3 -m pytest test/cdc_integration_test.py -q --collect-only
```

For the legacy `PYTHONPATH` form (e.g. running outside the repo root
or in CI without `conftest.py` on `PYTHONPATH`):

```
PYTHONPATH=driver/python3 python3 -m pytest test/cdc_integration_test.py -q
```

Running a probe in script mode (no pytest, no venv — the real check for
probe files, which have no pytest test functions):

```
python3 test/merge_e2e_test.py    # uses the repo-relative sys.path line
```

Note: the probes spawn `build/release/rethinkdb` as a child process, so
running them end-to-end still requires `./configure && make -j4` first.

## Code Style Guidelines

- Use braces for all control structures: `if (...) {`, `while (...) {`, etc.
- Include headers in order: parent .hpp, C system, C++ standard, Boost (with errors.hpp first), local headers.
- Avoid non-const lvalue references except in return values.
- Do not make fields whose type is a reference type.
- Use `DISABLE_COPYING` macro for non-copyable types.
- Run `../scripts/check_style.sh` to verify style compliance.
- Suppress bogus lints with `// NOLINT(specific/category)`.

## Testing Instructions

RethinkDB has unit tests (in `src/unittest/`) and integration tests. Integration tests use scenarios and workloads:
- Scenarios: Scripts that start RethinkDB clusters and run workloads (e.g., `test/scenarios/static_cluster.py`).
- Workloads: Commands that run queries against the database (memcached or RDB protocol).
- Run tests in a clean directory: `(rm -rf scratch; mkdir scratch; cd scratch; ../scenarios/<scenario> <args>)`

## Security Considerations

- Supports TLS encryption for driver, intracluster, and web UI connections.
- Includes user authentication and permissions system (since v2.5).
- Certificate verification for TLS connections.
- No known active security issues; follow standard database security practices.

## Project Architecture

### Core Components
- **Thread Pool and Event Loop**: Cooperative coroutines for IO operations (`arch/runtime/`).
- **Networking**: TCP connections between servers, mailboxes for communication (`rpc/`).
- **Query Execution**: Parses ReQL queries, executes on tables (`rdb_protocol/`).
- **Storage Engine**: B-tree storage on disk with page cache (`btree/`, `buffer_cache/`, `serializer/`).
- **Clustering**: Distributed operations, table management, Raft consensus (`clustering/`).

### Key Modules
- `arch/`: Runtime primitives, coroutines, IO.
- `concurrency/`: Concurrency utilities.
- `rpc/`: Networking and messaging.
- `rdb_protocol/`: Query language, terms, data types.
- `clustering/`: Administration, routing, table contracts.
- `btree/`: B-tree operations.
- `buffer_cache/`: Page caching.
- `serializer/`: Log-structured serialization.

### Server Startup
- `main()` in `main.cc` delegates to `clustering/administration/main/serve.cc`.
- `do_serve()` sets up all components in order.

### Query Flow
- Client queries parsed into `ql::term_t` tree.
- Executed via `read_t`/`write_t` objects routed through `table_query_client_t`.
- Reach `store_t` for B-tree operations.
- Results returned via the same path.

### Changefeeds
- Special handling for realtime updates, pushing data from shards to clients.

### Table Configuration
- Managed via Raft consensus (`raft_member_t`).
- `table_manager_t` and `contract_coordinator_t` handle metadata and execution.

## Development Conventions

- Use C++ with specific style guidelines (see STYLE.md).
- Build system uses GNU Make with autotools-like configure.
- Dependencies fetched automatically if `--allow-fetch` is used.
- Web UI assets pre-generated in `src/gen/web_assets.cc`.
- Drivers maintained in separate repositories (Python, Ruby, JavaScript, etc.).

## Deployment Processes

- Packages available for Ubuntu, Debian, CentOS, OS X, Windows (beta).
- Snap packages supported.
- AMI for AWS.
- Source distribution via `make dist`.

## Continuous Integration

- GitHub Actions workflow builds on Ubuntu, runs cpplint (changed files), unit tests, and Python driver E2E integration tests.
- Preflight checks style before building.