#!/bin/sh
# ── S5 WAVE 2 — THE REAL PAIR, OVER THE REAL TRANSPORT ─────────────────────
#
#   run-tls-host.sh PORT PSK_HEX IDENTITY [RUN_MS] [SUMMARY_JSON]
#
# Runs the REAL PinPointStudio host, headless, on a REAL TLS 1.3 listener with
# an external PSK (`PPCP-RV` §5) — no harness socket, no plaintext option, no
# `ppcp-sim`.  It declares its Sources, accepts a session, arbitrates, and on
# exit writes a JSON summary of what it saw.  The PinPointCapture side dials it
# from the simulator; this end never dials.
#
# ⚠ WHY THIS SCRIPT EXISTS SEPARATELY FROM `run-interop.sh`.  Every row in that
# file runs over the PLAINTEXT harness socket, because `ppcp-sim` has no TLS
# transport and `PPCP-RV` erratum E4 says a harness socket is not a rendezvous
# path.  Wave 2's pairing is the one CONF 5a actually names — reference device ↔
# reference host — and it must not be measured over a socket neither product
# ships.  So this one takes the harness option OFF and stands up
# `Ppcp::Listener` exactly as `PpcpHostService` does.
#
# ── THE CONTRACT ───────────────────────────────────────────────────────────
#
#   PORT       the TCP port to listen on.  0 takes an ephemeral one and the
#              chosen port is written to $SUMMARY_JSON.port and printed.
#   PSK_HEX    K_tls (RV §5.1) as 64 hex characters — 32 bytes.  Both ends must
#              hold the same one; on the product path it is derived from the
#              pairing code, and here it is given because the two agents cannot
#              share a QR code.
#   IDENTITY   the PSK identity (RV §5.3, §10.2) this host will accept, as text.
#              The literal `any` accepts every identity, which is what to use
#              when the dialling side derives a per-connection identity with a
#              rotating `rid` — a script cannot know that value in advance, and
#              refusing it would be testing this script rather than the pair.
#              With anything else the match is exact, and a mismatch is refused
#              the same way an unknown identity is (RV 5.3d: the two failing
#              paths are indistinguishable).
#   RUN_MS     how long to listen before stopping.  Default 120000.
#   SUMMARY_JSON  where the summary lands.  Default ./pps-tls-host.json.
#
# ── WHAT THE SUMMARY CARRIES ───────────────────────────────────────────────
#
# One flat JSON object.  The fields the pairing is judged on:
#
#   link_up, session_opened, arbiter_started   did the handshake, the Session
#                                              and the arbiter each happen
#   declares_rx                                MSG 3.3 — the device declared
#   candidates_rx                              Candidates that arrived
#   nominated                                  this host's own nominations
#   issued, adopted, late, excluded, retained  8.2's outcomes
#   max_shot_candidates, shot_ids              what each Shot referenced (I8)
#   streams_rx, preview_streams_rx             the Streams the device opened
#   captures_rx, captures_absent,
#     captures_not_retained, payload_frames    5.11/5.14's Capture accounting
#   offers_rx, offers_accepted                 MSG §9's offer list
#   errors_rx, errors_fatal, error_codes       MSG §10 — and fatality is the
#                                              CODE, so a `profile_not_supported`
#                                              is not counted as a lost link
#   counterpart_timebases                      per clock: does this host claim a
#                                              reading for it, and what.  CONF 5b
#                                              lives here: `has_offset: false` is
#                                              the honest answer for a clock with
#                                              no measured relation, and a host
#                                              that substituted a zero would say
#                                              `true` with `0`
#
# Exit 0 means the run completed and the summary was written.  It does NOT mean
# the pairing passed: this script is an instrument, and the row is judged from
# the summary and from the dialling side's own report together.
set -eu

if [ $# -lt 3 ]; then
    echo "usage: run-tls-host.sh PORT PSK_HEX IDENTITY [RUN_MS] [SUMMARY_JSON]" >&2
    exit 2
fi

PORT="$1"
PSK="$2"
IDENTITY="$3"
RUN_MS="${4:-120000}"
SUMMARY="${5:-./pps-tls-host.json}"

# The binary is built by `src/Ppcp/tests` and is the same one every S5 row uses.
HOST="${PP_CONFORM_HOST:-}"
if [ -z "$HOST" ]; then
    for c in \
        "$(dirname "$0")/../../../build/ppcp-tests/ppcp_conform_host" \
        "$(dirname "$0")/../../../build/tests/ppcp_conform_host"; do
        if [ -x "$c" ]; then HOST="$c"; break; fi
    done
fi
if [ -z "$HOST" ] || [ ! -x "$HOST" ]; then
    echo "run-tls-host.sh: ppcp_conform_host not found; build it with" >&2
    echo "  cmake --build build/ppcp-tests --target ppcp_conform_host -j3" >&2
    echo "or set PP_CONFORM_HOST to its path" >&2
    exit 2
fi

case "$PSK" in
    *[!0-9a-fA-F]* | "") echo "run-tls-host.sh: PSK_HEX must be hex" >&2; exit 2 ;;
esac
if [ "${#PSK}" -ne 64 ]; then
    echo "run-tls-host.sh: PSK_HEX must be 64 hex characters (32 bytes of K_tls)" >&2
    exit 2
fi

PORTFILE="$SUMMARY.port"
rm -f "$PORTFILE" "$SUMMARY"

echo "run-tls-host.sh: listening on port $PORT, TLS 1.3 external PSK, identity '$IDENTITY'" >&2
echo "run-tls-host.sh: summary will be written to $SUMMARY after ${RUN_MS}ms" >&2

# ⚠ NO `--summary` FALLBACK AND NO `set +e`.  A run that dies without a summary
# must be visibly a dead run: a wave-2 row whose evidence is "the script exited
# 0" and nothing else is not evidence.
"$HOST" --row "wave2-tls-host" \
        --port "$PORT" --port-file "$PORTFILE" \
        --tls-psk "$PSK" --tls-identity "$IDENTITY" \
        --summary "$SUMMARY" --run-ms "$RUN_MS" &
HOSTPID=$!
trap 'kill $HOSTPID 2>/dev/null || true' INT TERM

i=0
while [ ! -f "$PORTFILE" ]; do
    i=$((i + 1))
    if [ $i -gt 200 ]; then
        echo "run-tls-host.sh: the host never bound a port" >&2
        kill $HOSTPID 2>/dev/null || true
        exit 1
    fi
    sleep 0.05
done
echo "run-tls-host.sh: PORT $(cat "$PORTFILE")" >&2

wait $HOSTPID
RC=$?
trap - INT TERM

if [ ! -s "$SUMMARY" ]; then
    echo "run-tls-host.sh: no summary at $SUMMARY — the run produced no evidence" >&2
    exit 1
fi
echo "run-tls-host.sh: summary at $SUMMARY" >&2
cat "$SUMMARY"
exit $RC
