#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROGRAM=GnuChanSound
OUTPUT=${OUTPUT:-build/$PROGRAM}
PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
BIN_DIR="$DESTDIR$PREFIX/bin"
APP_DIR="$DESTDIR$PREFIX/share/applications"
DESKTOP_FILE=gnuchansound.desktop
BIN="$ROOT/$OUTPUT"

action=${1:-install}
case "$action" in
    build)
        exec make -C "$ROOT" build
        ;;
    run)
        make -C "$ROOT" build
        exec "$ROOT/build/$PROGRAM"
        ;;
    check)
        exec make -C "$ROOT" check
        ;;
    install)
        make -C "$ROOT" build OUTPUT="$OUTPUT"
        if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
            if command -v sudo >/dev/null 2>&1; then
                exec sudo env PREFIX="$PREFIX" DESTDIR="$DESTDIR" \
                    sh "$ROOT/install.sh" install
            fi
            echo "error: installing to $BIN_DIR needs root (sudo unavailable)" >&2
            exit 1
        fi
        install -D -m 0755 "$BIN" "$BIN_DIR/$PROGRAM"
        install -D -m 0644 "$ROOT/$DESKTOP_FILE" "$APP_DIR/$DESKTOP_FILE"
        echo "Installed $PROGRAM to $BIN_DIR/$PROGRAM"
        echo "Desktop entry: $APP_DIR/$DESKTOP_FILE"
        ;;
    uninstall)
        if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
            if command -v sudo >/dev/null 2>&1; then
                exec sudo env PREFIX="$PREFIX" DESTDIR="$DESTDIR" \
                    sh "$ROOT/install.sh" uninstall
            fi
            echo "error: uninstalling from $BIN_DIR needs root (sudo unavailable)" >&2
            exit 1
        fi
        rm -f "$BIN_DIR/$PROGRAM" "$APP_DIR/$DESKTOP_FILE"
        echo "Removed $PROGRAM and its desktop entry"
        ;;
    *)
        echo "Usage: $0 [build|run|check|install|uninstall]" >&2
        exit 2
        ;;
esac
