#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${BIN:-"$ROOT/build/GnuChanSound"}
SMOKE_CONFIG="$ROOT/build/smoke-config-$$"
trap 'rm -rf "$SMOKE_CONFIG"' EXIT HUP INT TERM

output=$(PATH=/nonexistent "$BIN" --check-backend 2>&1) && {
    echo "expected --check-backend to report the deliberately hidden wpctl" >&2
    exit 1
}
case "$output" in
    *wpctl*PipeWire*WirePlumber*) ;;
    *)
        echo "missing-backend diagnostic was not explicit: $output" >&2
        exit 1
        ;;
esac
echo "missing-backend diagnostic smoke test passed"

if command -v timeout >/dev/null 2>&1; then
    mkdir -p "$SMOKE_CONFIG/bin"
    printf '%s\n' '#!/bin/sh' 'exec sleep 30' > "$SMOKE_CONFIG/bin/wpctl"
    chmod 755 "$SMOKE_CONFIG/bin/wpctl"
    set +e
    slow=$(PATH="$SMOKE_CONFIG/bin:/usr/bin:/bin" timeout 9s \
        "$BIN" --check-backend 2>&1)
    result=$?
    set -e
    if [ "$result" -ne 1 ]; then
        echo "expected a bounded backend timeout, got status $result: $slow" >&2
        exit 1
    fi
    case "$slow" in
        *"timed out after 5 seconds"*) ;;
        *)
            echo "backend timeout diagnostic was missing: $slow" >&2
            exit 1
            ;;
    esac
    echo "hung-backend timeout smoke test passed"
fi

mkdir -p "$SMOKE_CONFIG/GnuChanSettings"
printf '%s\n' 'ApplicationLanguage = Turkish' \
    > "$SMOKE_CONFIG/GnuChanSettings/system.conf"
localized=$(PATH=/nonexistent XDG_CONFIG_HOME="$SMOKE_CONFIG" \
    "$BIN" --check-backend 2>&1) && {
    echo "expected localized backend check to report missing wpctl" >&2
    exit 1
}
case "$localized" in
    *"wpctl kurulu değil"*) ;;
    *)
        echo "ApplicationLanguage Turkish setting was not honored: $localized" >&2
        exit 1
        ;;
esac
echo "GnuChanSettings language smoke test passed"

if command -v xvfb-run >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
    set +e
    timeout 3s xvfb-run -a "$BIN" >/dev/null 2>&1
    result=$?
    set -e
    if [ "$result" -ne 124 ]; then
        echo "GUI smoke test exited unexpectedly with status $result" >&2
        exit 1
    fi
    echo "X11 startup smoke test passed"
else
    echo "X11 startup smoke test skipped (xvfb-run/timeout unavailable)"
fi
