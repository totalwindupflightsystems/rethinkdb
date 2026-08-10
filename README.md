<img style="width:100%;" src="/github-banner.png">

[RethinkDB](https://www.rethinkdb.com)
======================================

[![CI](https://github.com/totalwindupflightsystems/rethinkdb/actions/workflows/build.yml/badge.svg)](https://github.com/totalwindupflightsystems/rethinkdb/actions/workflows/build.yml)

What is RethinkDB?
------------------

* **Open-source** database for building realtime web applications
* **NoSQL** database that stores schemaless JSON documents
* **Distributed** database that is easy to scale
* **High availability** database with automatic failover and robust fault tolerance

RethinkDB is the first open-source scalable database built for
realtime applications. It exposes a new database access model, in
which the developer can tell the database to continuously push updated
query results to applications without polling for changes.  RethinkDB
allows developers to build scalable realtime apps in a fraction of the
time with less effort.

To learn more, check out [rethinkdb.com](https://rethinkdb.com).

Not sure what types of projects RethinkDB can help you build? Here are a few examples:

* Build a [realtime liveblog](https://rethinkdb.com/blog/rethinkdb-pubnub/) with RethinkDB and PubNub
* Create a [collaborative photo sharing whiteboard](https://www.youtube.com/watch?v=pdPRp3UxL_s)
* Build an [IRC bot in Go](https://rethinkdb.com/blog/go-irc-bot/) with RethinkDB changefeeds
* Look at [cats on Instagram in realtime](https://rethinkdb.com/blog/cats-of-instagram/)
* Watch [how Platzi uses RethinkDB](https://www.youtube.com/watch?v=Nb_UzRYDB40) to educate


Quickstart
----------

For a thirty-second RethinkDB quickstart, check out
[rethinkdb.com/docs/quickstart](https://www.rethinkdb.com/docs/quickstart).

Or, get started right away with our ten-minute guide in these languages:

* [**JavaScript**](https://rethinkdb.com/docs/guide/javascript/)
* [**Python**](https://rethinkdb.com/docs/guide/python/)
* [**Ruby**](https://rethinkdb.com/docs/guide/ruby/)
* [**Java**](https://rethinkdb.com/docs/guide/java/)

Besides our four official drivers, we also have many [third-party drivers](https://rethinkdb.com/docs/install-drivers/) supported by the RethinkDB community. Here are a few of them:

* **C#/.NET:** [RethinkDb.Driver](https://github.com/bchavez/RethinkDb.Driver), [rethinkdb-net](https://github.com/mfenniak/rethinkdb-net)
* **C++:** [librethinkdbxx](https://github.com/AtnNn/librethinkdbxx)
* **Clojure:** [clj-rethinkdb](https://github.com/apa512/clj-rethinkdb)
* **Elixir:** [rethinkdb-elixir](https://github.com/rethinkdb/rethinkdb-elixir)
* **Go:** [GoRethink](https://github.com/dancannon/gorethink)
* **Haskell:** [haskell-rethinkdb](https://github.com/atnnn/haskell-rethinkdb)
* **PHP:** [php-rql](https://github.com/danielmewes/php-rql)
* **Rust:** [reql](https://github.com/rust-rethinkdb/reql)
* **Scala:** [rethink-scala](https://github.com/kclay/rethink-scala)

Looking to explore what else RethinkDB offers or the specifics of
ReQL? Check out [our RethinkDB docs](https://rethinkdb.com/docs/) and
[ReQL API](https://rethinkdb.com/api/).

Web Admin UI
------------

The bundled administrative web UI is served by the server on port
`8080` by default. Start a server (see [Building](#building) below),
then open <http://localhost:8080> in a browser — or, with the Python
driver, connect to the driver port (default `28015`):

    rethinkdb serve

To change the HTTP port (for example when `8080` is already taken by
another service on the host, or when running multiple instances), pass
`--http-port`:

    rethinkdb serve --http-port 8081

The UI's static assets are pre-generated into
`src/gen/web_assets.cc` (see `CONTRIBUTING.md` — "Building the admin
UI" — for how to regenerate them from the [`old_admin`][old_admin]
branch). The ReQL driver remains the primary supported interface for
programmatic access.

[old_admin]: https://github.com/rethinkdb/rethinkdb/tree/old_admin

Building
--------

First install some dependencies.  For example, on Ubuntu or Debian:

    sudo apt-get install build-essential protobuf-compiler \
        # python \  # for older distros
        python3 python-is-python3 \
        libprotobuf-dev libcurl4-openssl-dev \
        libncurses5-dev libjemalloc-dev wget m4 g++ libssl-dev

Generally, you will need

* GCC or Clang
* Protocol Buffers
* jemalloc
* Ncurses
* Python 2 or Python 3
* libcurl
* libcrypto (OpenSSL)
* libssl-dev

Then, to build:

    ./configure --allow-fetch
    # or run ./configure --allow-fetch CXX=clang++

    make -j4
    # or run make -j4 DEBUG=1

    sudo make install
    # or run ./build/debug_clang/rethinkdb

See WINDOWS.md and mk/README.md for build instructions for Windows and
FreeBSD.

Need help?
----------

A great place to start is
[rethinkdb.com/community](https://rethinkdb.com/community). Here you
can find out how to ask us questions, reach out to us, or [report an
issue](https://github.com/rethinkdb/rethinkdb/issues). You'll be able
to find all the places we frequent online and at which conference or
meetups you might be able to meet us next.

If you need help right now, you can also find us [on
Slack](https://join.slack.com/t/rethinkdb/shared_invite/enQtNzAxOTUzNTk1NzMzLWY5ZTA0OTNmMWJiOWFmOGVhNTUxZjQzODQyZjIzNjgzZjdjZDFjNDg1NDY3MjFhYmNhOTY1MDVkNDgzMWZiZWM),
[Twitter](https://twitter.com/rethinkdb), or IRC at
[#rethinkdb](irc://chat.freenode.net/#rethinkdb) on Freenode.

Contributing
------------

RethinkDB was built by a dedicated team, but it wouldn't have been
possible without the support and contributions of hundreds of people
from all over the world. We could use your help too! Check out our
[contributing guidelines](CONTRIBUTING.md) to get started.

Fork extensions
---------------

This fork adds PHASE3 extensions on top of upstream RethinkDB. The
table below summarizes the implemented features and the planned ones
(design specs only, not yet in `src/`):

| Feature | Status | Documentation / Links |
|---------|--------|-----------------------|
| Time-series | Implemented | [docs/time-series.md](docs/time-series.md) — chunked time-ordered storage, `between()` chunk pruning, retention TTL, downsampling. Tests: `test/ts2_e2e_probe.py`, `test/ts3_e2e_probe.py`, `test/ts4_e2e_probe.py`, `test/ts6_chaos_probe.py` |
| CDC streaming | Implemented | Publications, subscriptions, Kafka/Webhook/File-S3 sinks. Tests: `test/cdc_e2e_test.py`, `test/cdc_integration_test.py` |
| Vector / FTS / BRIN indexes | Implemented | Vector similarity search, full-text search, BRIN block-range indexes. Tests: `test/vector_fts_integration_test.py`, `test/vector_fts_brin_integration_test.py` |
| MERGE / UPSERT | Implemented | Deep merge and upsert terms. Test: `test/merge_e2e_test.py` |
| Generated / virtual columns | Implemented | `SET_GENERATED_COLUMNS` / `GET_GENERATED_COLUMNS` terms. Source: `src/rdb_protocol/terms/generated_columns.cc` |
| Partitioning | Implemented | Table partitioning. Test: `test/ts6_cluster_e2e_probe.py` |
| Parallel query execution | Implemented | Source: `src/rdb_protocol/parallel_executor.hpp` |
| JSON | Implemented | Source: `src/rdb_protocol/terms/json.cc` |
| JSONB / JSONPath | Planned (spec) | [.coding-hermes/specs/phase3-jsonb-jsonpath.md](.coding-hermes/specs/phase3-jsonb-jsonpath.md) |
| Async I/O | Planned (spec) | [.coding-hermes/specs/phase3-async-io.md](.coding-hermes/specs/phase3-async-io.md) |
| FDW | Planned (spec) | [.coding-hermes/specs/phase3-fdw.md](.coding-hermes/specs/phase3-fdw.md) |
| WASM UDF | Planned (spec) | [.coding-hermes/specs/phase3-wasm-udf.md](.coding-hermes/specs/phase3-wasm-udf.md) |

Docker quickstart
-----------------

No C++ toolchain needed — run a single-node instance with Docker:

    docker compose up -d --build    # build the image from source (first build ~8-10 min)
    docker compose up -d            # use the prebuilt image (ghcr.io — anonymous pulls
                                    # work after the package is set public; until then
                                    # compose falls back to building from source)

Connect any ReQL driver to `localhost:28015`, open the web UI at
http://localhost:8080, and data persists in the `rethinkdb-data` Docker
volume. The prebuilt image is published at
`ghcr.io/totalwindupflightsystems/rethinkdb:latest` (multi-arch amd64).

Donors
------

* [CNCF](https://www.cncf.io/)
* [Digital Ocean](https://www.digitalocean.com/) provides infrastructure and servers needed for serving mission-critical sites like download.rethinkdb.com or update.rethinkdb.com
* [Atlassian](https://www.atlassian.com/) provides OSS license to be able to handle internal tickets like vulnerability issues
* [Netlify](https://www.netlify.com/) OSS license to be able to migrate rethinkdb.com
* [DNSimple](https://dnsimple.com) provides DNS services for the RethinkDB project
* [ZeroTier](https://www.zerotier.com) sponsored the development of per-table configurable write aggregation including the ability to set write delay to infinite to create a memory-only table ([PR #6392](https://github.com/rethinkdb/rethinkdb/pull/6392))

Licensing
---------

RethinkDB is licensed by the Linux Foundation under the open-source
Apache 2.0 license. Portions of the software are licensed by Google
and others and used with permission or subject to their respective
license agreements.

Where's the changelog?
----------------------
We keep [a list of changes and feature explanations here](NOTES.md).
