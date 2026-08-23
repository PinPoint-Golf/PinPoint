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

# ⚠ EVERY ROW IS RUN AND EVERY ROW IS RECORDED.  `pps-conform.md` and
# `pps-conform.json` beside this script are the claim, verbatim, CT-I6's failure
# included, and docs/ppcp-conformance.md reproduces them unedited.
#
# ── THE ONE ROW THIS GATE DOES NOT FAIL ON, AND WHY (finding F-H8-6) ────────
#
# CT-I6 is a NEGATIVE row (CONF §1d): this host does not claim Mint, so the tool
# asserts it parses `shot` with `authority: device` and never originates one.
# The counterpart it picks for that is `reference-host.json` running the
# `reference-host` scenario — a peer declaring `role: host`.  Against a peer
# under test that is ALSO `role: host`, `PPCP-CORE` 5.2b and `PPCP-MSG` 3.2c
# require the responder to answer `error` / `role_conflict`, and `PPCP-MSG` §10
# marks that code **fatal**.  `ppcp-sim` counts a fatal error as a protocol
# violation, so the row asserts `violations=0` against a counterpart the
# specification requires this host to refuse.  It cannot pass, and a host that
# made it pass would be violating I20.
#
# So the exclusion is ONE NAMED ROW with a written reason, not a filter over
# whatever happened to be red.  Anything else failing fails this test.  If
# `ppcp-conform` ever gives CT-I6 a minting CAPTURE counterpart — which is what
# the row needs, since only a peer with Mint can send the `shot` this host must
# parse and not originate — this whole block goes.
python3 - "$WORK/pps-conform.json" >&2 <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))["rows"]
bad = [r for r in rows if r["verdict"] == "fail" and r["id"] != "CT-I6"]
ct_i6 = [r for r in rows if r["id"] == "CT-I6" and r["verdict"] == "fail"]
if ct_i6:
    print("F-H8-6: CT-I6 failed and this gate does not fail on it — "
          "its counterpart declares role: host and I20 requires this host to refuse it. "
          "Reason from the tool: " + ct_i6[0].get("reason", ""))
if bad:
    print("FAILED rows: " + ", ".join(r["id"] + " (" + r.get("reason", "") + ")" for r in bad))
    sys.exit(1)
sys.exit(0)
PY
GATE=$?

# A non-1 exit from the tool is never excused: 2 is a bad invocation and 3 is
# "no row applied", which is the failure mode `ppcp-conform` exists to avoid.
if [ "$RC" -gt 1 ]; then exit "$RC"; fi
exit $GATE
