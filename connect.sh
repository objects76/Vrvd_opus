#!/bin/sh
# connect.sh - build (if stale) and connect the Linux viewer to the streamserver.
# Usage: ./connect.sh [host]   (host defaults to 10.1.110.27)
set -e
cd "$(dirname "$0")/simple/viewer"

# zstd is linked statically from the in-tree source (no libzstd-dev on this box)
ZSTD_LIB=../zstd-1.5.7/lib
[ -f "$ZSTD_LIB/libzstd.a" ] || make -C "$ZSTD_LIB" -j"$(nproc)" libzstd.a

# dav1d (AV1 decode): headers vendored in ../dav1d/include (extracted from the
# libdav1d-dev 1.2.1 deb — no sudo on this box); links the system runtime
# libdav1d.so.6 (1.2.1) already installed. Self-check: simple/test/av1_check.cpp
DAV1D_INC=../dav1d/include

# rebuild when any source the binary depends on is newer than it
if [ ! -x viewer ] || [ viewer_x11.cpp -nt viewer ] \
   || [ ../common/config.h -nt viewer ] \
   || [ ../common/scap_stream.h  -nt viewer ] \
   || [ ../common/scap_packet.h  -nt viewer ] \
   || [ ../common/zstd_stream.h  -nt viewer ] \
   || [ ../common/av1_decoder.h  -nt viewer ] \
   || [ ../common/scap_palette.h -nt viewer ] \
   || [ ../common/scap_332dither.h -nt viewer ] \
   || [ ../common/scap_256map.h -nt viewer ] \
   || [ "$ZSTD_LIB/libzstd.a" -nt viewer ]; then
    echo "rebuilding viewer..."
    # USE_AV1 (common/config.h) decodes with dav1d; headers are vendored (no
    # sudo on this box), runtime links the installed libdav1d.so.6 by exact
    # soname since there is no dev symlink. zstd 1.5.7 is static from in-tree.
    if [ ! -e "$DAV1D_INC/dav1d/dav1d.h" ]; then
        echo "missing vendored dav1d headers in $DAV1D_INC" >&2
        exit 1
    fi
    g++ -O2 -Wall -I"$DAV1D_INC" -I"$ZSTD_LIB" -o viewer viewer_x11.cpp \
        "$ZSTD_LIB/libzstd.a" -lX11 -l:libdav1d.so.6 -lpthread
    ./viewer --selftest
fi

# the server serves one client at a time: stop a stale instance first so the
# new connection doesn't sit in the listen backlog
pkill -x viewer 2>/dev/null && sleep 0.3 || true

exec ./viewer "${1:-10.1.110.27}"
