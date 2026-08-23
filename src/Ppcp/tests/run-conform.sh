#!/bin/sh
# ── The H8 conformance row ──────────────────────────────────────────────────
#
# Starts the headless host on an ephemeral port, waits for it to say which one,
# runs `ppcp-conform` against it, and reports the tool's exit code as this
# test's.  Nothing is interpreted: exit 0 is "every applicable row passed",
# 1 is "a row failed", 2 is "bad invocation" and 3 is "no row applied" — and 3
# is deliberately not 0, because a claim naming profiles the tool has no row for
# has not been measured.
#
#   run-conform.sh <ppcp_conform_host> <ppcp-conform> <workdir>
#
# The profile set is the CLAIM (docs/ppcp-conformance.md §1): seven of the
# eight, with Mint withheld — this host parses `authority: device` and never
# originates it.  Passing it here and claiming it there would be two claims.
set -eu

HOST="$1"
CONFORM="$2"
WORK="$3"

PROFILES="core,capture,detect,arbitrate,live,offline,markup"
PORTFILE="$WORK/conform-host.port"
rm -f "$PORTFILE"

"$HOST" --port 0 --port-file "$PORTFILE" --run-ms 240000 >"$WORK/conform-host.log" 2>&1 &
HOSTPID=$!
trap 'kill $HOSTPID 2>/dev/null || true' EXIT INT TERM

# Wait for the port, but not for ever: a host that never listened is a failure
# of this row and not something to hang the suite on.
i=0
while [ ! -f "$PORTFILE" ]; do
    i=$((i + 1))
    if [ $i -gt 200 ]; then
        echo "the headless host never wrote a port file; its log follows" >&2
        cat "$WORK/conform-host.log" >&2 || true
        exit 1
    fi
    sleep 0.05
done
PORT=$(cat "$PORTFILE")
echo "peer under test: $HOST on 127.0.0.1:$PORT (PLAINTEXT harness socket)" >&2

set +e
"$CONFORM" --profiles "$PROFILES" --role host --connect "127.0.0.1:$PORT" \
           --column PinPointStudio \
           --json "$WORK/pps-conform.json" \
           --markdown "$WORK/pps-conform.md"
RC=$?
set -e

echo "── the host's own log ───────────────────────────────────────────────" >&2
cat "$WORK/conform-host.log" >&2 || true
echo "── the row table ────────────────────────────────────────────────────" >&2
cat "$WORK/pps-conform.md" >&2 || true

exit $RC
