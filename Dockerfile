# syntax=docker/dockerfile:1
# ─────────────────────────────────────────────────────────────────────────────
# RethinkDB fork (totalwindupflightsystems) — single-node container image.
#
# Multi-stage build:
#   builder — compiles the server from source (ubuntu:24.04, distro g++).
#   runtime — minimal image: the server binary + the shared libraries it
#             links against. The web UI is compiled into the binary
#             (pre-generated web_assets.cc), so no extra assets are needed.
#
# Build locally:  docker compose up -d --build
# Pull prebuilt:  docker compose up -d   (ghcr.io/totalwindupflightsystems/rethinkdb:latest)
# ─────────────────────────────────────────────────────────────────────────────

# ── Builder stage ────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Same dependency recipe as the fork's CI workflow (.github/workflows/build.yml),
# plus cmake: the bundled quickjs dependency (fetched by --allow-fetch) builds
# via cmake, which is preinstalled on CI runners but absent from bare ubuntu.
# libboost-all-dev is REQUIRED: without system boost, configure queues a broken
# boost 1.85.0 fetch whose patch does not apply (proven CI-001).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        protobuf-compiler \
        python3 \
        python-is-python3 \
        libprotobuf-dev \
        libcurl4-openssl-dev \
        libncurses5-dev \
        libjemalloc-dev \
        libboost-all-dev \
        cmake \
        wget \
        m4 \
        g++ \
        libssl-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# NOTE: ./configure rejects CC=<compiler> ("Unknown variable argument: CC").
# Only CXX= is a valid variable arg; the distro default g++ is fine here.
RUN ./configure --allow-fetch \
    && make -j4

# ── Runtime stage ────────────────────────────────────────────────────────────
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Same base image as the builder stage ⇒ identical shared-library versions.
# The -dev packages pull in the runtime .so files the binary links against.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libprotobuf-dev \
        libcurl4-openssl-dev \
        libncurses5-dev \
        libjemalloc-dev \
        libssl-dev \
        libboost-all-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/release/rethinkdb /usr/bin/rethinkdb

# Data directory — backed by the named volume in docker-compose.yml.
RUN mkdir -p /data && chmod 0755 /data

VOLUME /data

# 28015 driver, 29015 intra-cluster, 8080 web UI
EXPOSE 28015 29015 8080

ENTRYPOINT ["rethinkdb"]
CMD ["--no-update-check", "--bind", "all", "--http-port", "8080", "--driver-port", "28015", "-d", "/data"]
