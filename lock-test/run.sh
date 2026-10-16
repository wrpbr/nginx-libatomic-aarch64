#!/bin/sh
# Build the three lock variants and run each one. Run it on an aarch64 machine with at least 2 cores:
#
#   docker run --rm --init -v "$PWD/lock-test:/src:ro" debian:bookworm-slim sh /src/run.sh
#
# The container installs gcc and libatomic-ops-dev (7.6.14 on Debian 12), builds static binaries in /tmp and
# prints one line per variant. The test takes about 10 seconds per variant on a Neoverse-N1.
set -eu
ROUNDS="${1:-20000000}"
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends gcc libc6-dev libatomic-ops-dev >/dev/null
for variant in CAS_GCC_SYNC CAS_AO CAS_AO_FULL; do
    gcc -O2 -static -D"$variant" -o "/tmp/$variant" /src/counter.c /src/lock.c
done
uname -m
for variant in CAS_GCC_SYNC CAS_AO CAS_AO_FULL; do
    printf '%-14s ' "$variant"
    "/tmp/$variant" "$ROUNDS" || true
done
