#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUTPUT=${OUTPUT:-build/GnuChanSound}
STAGE="$ROOT/build/install-smoke-$$"
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM

make -C "$ROOT" install OUTPUT="$OUTPUT" PREFIX=/usr DESTDIR="$STAGE"
test -x "$STAGE/usr/bin/GnuChanSound"
test -f "$STAGE/usr/share/applications/gnuchansound.desktop"

make -C "$ROOT" uninstall OUTPUT="$OUTPUT" PREFIX=/usr DESTDIR="$STAGE"
test ! -e "$STAGE/usr/bin/GnuChanSound"
test ! -e "$STAGE/usr/share/applications/gnuchansound.desktop"

echo "staged install/uninstall test passed"
