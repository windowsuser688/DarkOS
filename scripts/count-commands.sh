#!/bin/sh
set -eu

ROOT=${1:-build/rootfs}

find "$ROOT/bin" "$ROOT/sbin" "$ROOT/usr/bin" "$ROOT/usr/sbin" \
	-type f -o -type l 2>/dev/null |
	sed 's#.*/##' |
	sort -u |
	wc -l

