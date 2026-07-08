#!/bin/sh
# connect.sh - build (if stale) and connect the Linux viewer to the streamserver.
# Usage: ./connect.sh [host]   (host defaults to 10.1.110.27)
set -e
cd "$(dirname "$0")/simple/viewer"

# rebuild when any source the binary depends on is newer than it
if [ ! -x viewer ] || [ viewer_x11.cpp -nt viewer ] \
   || [ ../common/scap_stream.h  -nt viewer ] \
   || [ ../common/scap_packet.h  -nt viewer ] \
   || [ ../common/scap_palette.h -nt viewer ]; then
    echo "rebuilding viewer..."
    g++ -O2 -Wall -o viewer viewer_x11.cpp -lX11 -lz -lpthread
    ./viewer --selftest
fi

# the server serves one client at a time: stop a stale instance first so the
# new connection doesn't sit in the listen backlog
pkill -x viewer 2>/dev/null && sleep 0.3 || true

exec ./viewer "${1:-10.1.110.27}"
