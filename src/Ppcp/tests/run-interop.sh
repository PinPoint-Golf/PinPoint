#!/bin/sh
# ── The `PPCP-CONF` §5 interoperability rows, host side ─────────────────────
#
#   run-interop.sh ROW <ppcp_conform_host> <ppcp-sim> <scenarios-dir> <workdir>
#                      [device-bundle-dir] [libppcp-fixture-dir]
#
# One row per invocation.  Each starts the REAL PinPointStudio host headless on
# an ephemeral port, dials `ppcp-sim` at it as the counterpart CONF §5 names,
# and then asserts TWICE:
#
#   - the counterpart's view, as `ppcp-sim --expect` (its exit code);
#   - THIS host's view, as the JSON summary `--summary` writes.
#
# ⚠ BOTH, ALWAYS.  A pairing row that only read the simulator's counters would
# pass for a host that received everything and concluded nothing, and one that
# only read ours would pass for a host talking to itself.  The "principally
# proves" column of CONF §5 is a statement about the pair, so the row is too.
#
# Exit 0 is the row passing.  1 is the row failing — and a failing row is a
# finding against the host, the simulator, the library or the specification,
# which `docs/ppcp-conformance.md` §11 has to say which of.  2 is a bad
# invocation.
set -eu

if [ $# -lt 5 ]; then
    echo "usage: run-interop.sh ROW <host> <ppcp-sim> <scenarios> <workdir>" >&2
    exit 2
fi

ROW="$1"
HOST="$2"
SIM="$3"
SCEN="$4"
WORK="$5"
# IOP-3 and IOP-10 only.  The first is where the PinPointCapture agent checks in
# the bundles ITS device wrote, which is what makes IOP-3 an interoperability
# row rather than this host reading its own output; the second is libppcp's
# `tests/fixtures/`, which is a fallback and is recorded AS a fallback in
# docs/ppcp-conformance.md §11 rather than passed off as the real pairing.
DEVDIR="${6:-}"
FIXDIR="${7:-}"

mkdir -p "$WORK"
PORTFILE="$WORK/$ROW.port"
SUMMARY="$WORK/$ROW.host.json"
HOSTLOG="$WORK/$ROW.host.log"
SIMLOG="$WORK/$ROW.sim.log"
rm -f "$PORTFILE" "$SUMMARY"

# Defaults every row starts from.  `--run-ms` on the host is deliberately
# longer than the simulator's: the host is the party that does not decide when
# a Session is over, and a host that vanished first would be measuring the
# harness rather than the protocol.
HOST_RUN_MS=8000
SIM_RUN_MS=6000
HOST_EXTRA=""
SIM_ROLE="capture"
SIM_DECL=""
SIM_SCENARIO=""
SIM_EXPECT=""
HOST_ASSERT=""
WHAT=""

case "$ROW" in
# ── IOP-4 — reference host ↔ observer-only peer (Core + Live).  I24 ─────────
#
# The observer originates nothing past `hello`, `declare` and its acks, and the
# host must neither require it to nor treat the link as broken.  The transport
# staying open is asserted by the host having taken heartbeat acks off it.
IOP-4)
    WHAT="I24 — an observer-only peer participates fully and originates nothing"
    SIM_ROLE="observer"
    SIM_DECL="$SCEN/observer-core.json"
    SIM_SCENARIO="observer"
    SIM_EXPECT="violations=0,declares_rx=1,candidates_tx=0,shots_tx=0,errors_rx=0"
    HOST_ASSERT="link_up=true session_opened=true declares_rx=1 candidates_rx=0 shots_rx=0 issued=0 errors_fatal=0 heartbeat_acks>=1"
    ;;

# ── IOP-5 — reference host ↔ peer declaring `unrelated` timebases ───────────
#
# CONF 5b, and the reason the row exists: the host EXCLUDES and RETAINS every
# Candidate, issues no Shot, and never substitutes a zero offset.  The last of
# those is invisible on the wire — a fabricated mapping looks exactly like a
# measured one — so it is asked of the host directly: `counterpart_offsets` is
# how many of the peer's clocks this host claims a reading for, and it is 0.
IOP-5)
    WHAT="I3 / 8.2i1 — excluded AND retained, no Shot, and no zero substituted"
    SIM_DECL="$SCEN/unrelated-capture.json"
    SIM_SCENARIO="unrelated-capture"
    SIM_EXPECT="violations=0,minted=0,shots_rx=0,candidates_tx>=1"
    HOST_ASSERT="link_up=true session_opened=true candidates_rx>=1 issued=0 retained>=1 counterpart_offsets=0 errors_fatal=0"
    ;;

# ── IOP-6 — host owning its own acoustic Source ↔ nominating device.  I8 ────
#
# Two nominators of ONE `basis`, from two peers, both retained on one Shot.
# `--nominate-acoustic` is what makes this host the first of them: H5's
# `PpcpShotBridge::nominate()` over the microphone in this host's own
# declaration.  `max_shot_candidates >= 2` is the assertion the `ShotArbiter`
# this bridge replaced could not have met — its per-modality slot keeps one.
IOP-6)
    WHAT="I8 — two Candidates of one basis from two peers, both retained"
    HOST_EXTRA="--nominate-acoustic"
    SIM_DECL="$SCEN/reference-capture.json"
    SIM_SCENARIO="nominating-capture"
    SIM_EXPECT="violations=0,candidates_tx>=1,shot_candidates_max>=2,t0_revisions=0"
    HOST_ASSERT="link_up=true session_opened=true arbiter_started=true nominated>=1 candidates_rx>=1 issued>=1 max_shot_candidates>=2 errors_fatal=0"
    ;;

# ── IOP-7 — a host that never issues ↔ nominating peer.  I32 ────────────────
#
# The arbiter is built and 8.2h is never run, so the only thing that can fire is
# the DEVICE's own 8.2i deadline — and what it mints is what it would have
# promoted.  `shots_rx >= 1` is the host seeing that Shot arrive; `issued = 0`
# is the host not having raced it.
IOP-7)
    WHAT="I32 — the peer mints at its own deadline, and only what it would have promoted"
    HOST_EXTRA="--never-issue"
    SIM_DECL="$SCEN/reference-capture.json"
    SIM_SCENARIO="nominating-capture"
    SIM_EXPECT="violations=0,candidates_tx>=1,minted>=1,t0_revisions=0"
    HOST_ASSERT="link_up=true session_opened=true arbiter_started=true candidates_rx>=1 issued=0 shots_rx>=1 errors_fatal=0"
    ;;

# ── IOP-8 — a host delayed past the mint deadline ↔ nominating peer.  I35 ───
#
# ⚠ THE MARGIN IS THE ROW.  libppcp's own log records the trap: with the
# deadline at `issue_hold_ns + heartbeat_interval_ms` after the Candidate and
# the host delayed by only a little more, the two land close enough for the host
# to win the race, and the run looks like an ordinary arbitration while
# asserting nothing.  So both numbers are stated here — deadline 1.2 s, host
# delayed 3 s — and the assertion is 8.2k: the host ATTACHES to the device's
# Shot (`adopted >= 1`) rather than issuing a second one (`issued = 0`).
IOP-8)
    WHAT="I35 — the host attaches to the device's Shot rather than issuing a second"
    HOST_EXTRA="--issue-delay-ms 3000 --issue-hold-ms 200 --heartbeat-ms 1000"
    SIM_DECL="$SCEN/reference-capture.json"
    SIM_SCENARIO="nominating-capture"
    SIM_RUN_MS=8000
    HOST_RUN_MS=10500
    SIM_EXPECT="violations=0,minted>=1,t0_revisions=0"
    HOST_ASSERT="link_up=true session_opened=true arbiter_started=true candidates_rx>=1 shots_rx>=1 issued=0 adopted>=1 errors_fatal=0"
    ;;

# ── IOP-9 — reference host ↔ capture peer with `continuous` + `preview` ─────
#
# I36's coverage shape: a stream-anchored SEGMENT on the continuous Stream, and
# the discarded preview announced `absent` / `not_retained` (5.11c3) rather than
# as a gap.  "Preview absent from the bundle" is 5.11j, and the measurable half
# of it on this side is that no preview Capture ever carried a payload frame —
# a measured zero rather than an assumption about what the device stored.
IOP-9)
    WHAT="I36 — coverage across the session, and a preview that is absent and never transferred"
    SIM_DECL="$SCEN/preview-capture.json"
    SIM_SCENARIO="preview-capture"
    SIM_EXPECT="violations=0,captures_rx>=0"
    HOST_ASSERT="link_up=true session_opened=true streams_rx>=2 preview_streams_rx>=1 captures_rx>=2 captures_absent>=1 captures_not_retained>=1 preview_payload_frames=0 errors_fatal=0"
    ;;

# ── IOP-3 (import half) and IOP-10 (read direction) ────────────────────────
#
# A bundle the DEVICE wrote, through this host's real offline path.  No socket
# is opened: CORE §9's offline path needs no rendezvous, no TLS and no listener,
# and that is the whole argument for shipping it first.
#
# ⚠ READ TWICE, AGAINST ONE LEDGER.  I34: the second read admits nothing new and
# duplicates nothing.  Nothing on the wire distinguishes an importer that
# de-duplicated from one that imported twice, which is why this is a bundle row
# and not a paired one — and why the assertion is made of the ledger.
IOP-3)
    WHAT="I20, I23, I16, I9 — a hostless device's bundle imported, and idempotent on re-import"
    IMPORT_ROW=1
    ;;

# ── IOP-10, the write direction ────────────────────────────────────────────
#
# ENC 7a — "live and file are one format".  Phase 1 runs a real Session (the
# IOP-6 pairing, so the bundle has Shots in it) with `--write-bundle`; phase 2
# reads that file back through the same offline path IOP-3 uses.  A bundle this
# host cannot read is not one the device will read either, and finding that out
# here costs a second rather than a round trip through another repository.
IOP-10)
    WHAT="ENC 7a — this host's own Session record, written and read back"
    IMPORT_ROW=1
    WRITE_FIRST=1
    HOST_EXTRA="--nominate-acoustic"
    SIM_DECL="$SCEN/reference-capture.json"
    SIM_SCENARIO="nominating-capture"
    ;;

*)
    echo "run-interop.sh: no such row: $ROW" >&2
    exit 2
    ;;
esac

echo "── $ROW — $WHAT ───────────────────────────────────────────────" >&2

# ── The two bundle rows, which open no socket ───────────────────────────────
if [ -n "${IMPORT_ROW:-}" ]; then
    IMPORT_ROOT="$WORK/$ROW-import"
    rm -rf "$IMPORT_ROOT"
    RC=0

    if [ -n "${WRITE_FIRST:-}" ]; then
        # Phase 1 — write.  A real Session against the IOP-6 counterpart, so
        # the bundle carries `session_open` with both arbitration parameters
        # (5.10e: the statement that this Session HAS a host), this host's own
        # `declare`, and the Shots it actually issued.
        BUNDLE="$WORK/pinpointstudio-host-session.ppcpbndl"
        rm -f "$BUNDLE"
        # shellcheck disable=SC2086
        "$HOST" --row "$ROW-write" --port 0 --port-file "$PORTFILE" \
                --summary "$WORK/$ROW.write.json" --write-bundle "$BUNDLE" \
                --run-ms "$HOST_RUN_MS" $HOST_EXTRA >"$HOSTLOG" 2>&1 &
        HOSTPID=$!
        trap 'kill $HOSTPID 2>/dev/null || true' EXIT INT TERM
        i=0
        while [ ! -f "$PORTFILE" ]; do
            i=$((i + 1))
            if [ $i -gt 200 ]; then
                echo "$ROW: the host never wrote a port file" >&2
                cat "$HOSTLOG" >&2 || true
                exit 1
            fi
            sleep 0.05
        done
        PORT=$(cat "$PORTFILE")
        set +e
        "$SIM" --role "$SIM_ROLE" --connect "127.0.0.1:$PORT" \
               --declaration "$SIM_DECL" --scenario "$SIM_SCENARIO" \
               --run-ms "$SIM_RUN_MS" --log-prefix "$ROW-sim" \
               --expect "violations=0" >"$SIMLOG" 2>&1
        set -e
        wait $HOSTPID 2>/dev/null || true
        trap - EXIT INT TERM
        tail -20 "$HOSTLOG" >&2 || true
        if [ ! -s "$BUNDLE" ]; then
            echo "$ROW: FAIL — the host wrote no bundle at $BUNDLE" >&2
            exit 1
        fi
        echo "$ROW: wrote $(wc -c <"$BUNDLE" | tr -d ' ') bytes to $BUNDLE" >&2
        BUNDLES="$BUNDLE"
    else
        # ⚠ EVERY BUNDLE THE DEVICE WROTE, NOT THE FIRST ONE.  IOP-3 is an
        # interoperability row — a bundle written by PinPointCapture and read by
        # PinPointStudio — and the device checks in more than one shape on
        # purpose (one Shot and two).  Reading only the first would leave the
        # other unread and the row would still be green, which is how a
        # conformance file stops being evidence.
        #
        # The fallback to libppcp's fixtures is LOUD: it is still evidence about
        # the reader, but about ONE writer, and that is a different and much
        # weaker claim.
        BUNDLES=""
        if [ -n "$DEVDIR" ] && [ -d "$DEVDIR" ]; then
            BUNDLES=$(ls -1 "$DEVDIR"/*.ppcpbndl "$DEVDIR"/*.ppcpb 2>/dev/null || true)
        fi
        if [ -n "$BUNDLES" ]; then
            echo "$ROW: reading the bundles the DEVICE wrote:" >&2
            echo "$BUNDLES" | sed "s/^/$ROW:   /" >&2
        elif [ -n "$FIXDIR" ] && [ -d "$FIXDIR" ]; then
            BUNDLES=$(ls -1 "$FIXDIR"/ct-i12-video.ppcpb 2>/dev/null || true)
            echo "$ROW: ⚠ NO DEVICE BUNDLE at ${DEVDIR:-<unset>} — falling back to libppcp's" >&2
            echo "$ROW: ⚠ fixture $BUNDLES.  This measures the READER only; the pairing" >&2
            echo "$ROW: ⚠ is not demonstrated until the device checks a bundle in." >&2
        fi
        if [ -z "$BUNDLES" ]; then
            echo "$ROW: BLOCKED — no bundle to read (device dir '$DEVDIR', fixtures '$FIXDIR')" >&2
            exit 1
        fi
    fi

    # ── The checker, written out once and run per bundle ───────────────────
    #
    # A file rather than a heredoc inside the loop, because the loop reads more
    # than one bundle and a heredoc belongs to one command.
    CHECK="$WORK/$ROW-check.py"
    cat >"$CHECK" <<'PY'
import json, sys

path, row = sys.argv[1], sys.argv[2]
try:
    d = json.load(open(path))
except Exception as e:                                     # noqa: BLE001
    print(f"{row}: FAIL — no host summary at {path}: {e}")
    sys.exit(1)

# CORE 4.4a / I10 — three states, not two, and `unknown` is not `complete`:
# completeness is ASSERTED by the owner and never inferred.
NAMES = {0: "complete", 1: "partial", 2: "absent", 3: "unknown"}

p = d.get("import_passes", [])
bad = []
if len(p) != 2:
    bad.append(f"expected two import passes, got {len(p)}")
else:
    first, second = p
    # ENC §7 — the container parsed, the frame walk completed, and 7c's
    # manifest ordering held.  A reader that got past a misordered manifest
    # would be reading a bundle no conformant writer produced.
    if not first["ok"]:
        bad.append("the first read failed: " + first["error"])
    if first["frames"] < 1:
        bad.append("the first read walked no frames")
    if not first["manifest_ordered"]:
        bad.append("ENC 7c: the manifest was not ordered before the payload")
    # MSG 3.3c / I34 — a Capture announced before the walk knew who minted it
    # is unattributable, and its identity is then unresolvable.  Zero, or the
    # ledger is keying on nothing.
    if first["captures_unattributable"]:
        bad.append(f"{first['captures_unattributable']} Captures arrived before any declare")
    # I34 — the second read admits nothing new and duplicates nothing.  Stated
    # over the LEDGER because nothing on the wire could say it.
    if second["captures_new"] != 0:
        bad.append(f"I34: the second read admitted {second['captures_new']} new Captures")
    if second["captures_already_held"] != first["captures"]:
        bad.append("I34: the second read held "
                   f"{second['captures_already_held']} of {first['captures']} again")
    if first["digest_conflicts"] or second["digest_conflicts"]:
        bad.append("a digest conflict: the same identity with different content")
    if first["captures"] and not first["session_held"]:
        bad.append("the ledger does not hold the Session it just imported")
    # ENC 7d / I10 — what the OWNER asserted is what is recorded.  An
    # untruncated bundle asserting `partial` stays `partial`: 7d resolves the
    # assertion and the observation in exactly one direction, and an
    # observation may never UPGRADE a session the owner called incomplete.
    if first["asserted_completeness"] and first["truncated"]:
        bad.append("the bundle asserted a completeness AND was truncated; "
                   "ENC 7d only ever downgrades, so this needs reading by hand")
    # ⚠ CORE 5.14h — `capture_committed` is owed for a payload DURABLY HELD, and
    # for nothing else.  The invariant is one commit per clip written, which is
    # 0 == 0 for a Session whose every Capture is `absent`: committing one would
    # confirm bytes that were never sent, and 8.4b puts `confirmed` outside the
    # owner's own authority precisely so that cannot happen.
    if first["commits_queued"] != first["clips_written"]:
        bad.append(f"5.14h: {first['commits_queued']} commits queued for "
                   f"{first['clips_written']} clips written")
    if second["commits_queued"] != 0:
        bad.append("5.14h: the second read queued a commit for a clip it did not write")

if bad:
    print(f"{row}: FAIL — the import path:")
    for b in bad:
        print("   " + b)
    print("   summary: " + json.dumps(d, indent=2))
    sys.exit(1)
f = p[0]
print(f"{row}: {f['frames']} frames, {f['streams']} Streams, {f['captures']} Captures "
      f"({f['captures_new']} new, {p[1]['captures_already_held']} already held on the second), "
      f"completeness {NAMES.get(f['completeness'], f['completeness'])}"
      f"{' (asserted)' if f['asserted_completeness'] else ' (nothing asserted)'}, "
      f"{f['clips_written']} clips, {f['commits_queued']} commits owed to {f['owner_peer_id']}")
sys.exit(0)
PY

    # ⚠ ONE LEDGER AND ONE ROOT PER BUNDLE.  I34's claim is that a SECOND read
    # of the SAME bundle admits nothing new.  It is not a claim about two
    # different Sessions sharing a ledger, and running them together would let a
    # miscount in one hide behind the other's totals.
    READ=0
    CAPTURED=0
    for BUNDLE in $BUNDLES; do
        [ -s "$BUNDLE" ] || continue
        BASE=$(basename "$BUNDLE")
        BSUM="$WORK/$ROW.$BASE.json"
        "$HOST" --row "$ROW" --summary "$BSUM" --import-bundle "$BUNDLE" \
                --import-twice --import-root "$IMPORT_ROOT/$BASE" >>"$HOSTLOG" 2>&1 || RC=1
        cp "$BSUM" "$SUMMARY"
        READ=$((READ + 1))
        if python3 "$CHECK" "$BSUM" "$ROW $BASE" >&2; then
            N=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['import_passes'][0]['captures'])" "$BSUM")
            CAPTURED=$((CAPTURED + N))
        else
            RC=1
        fi
    done
    tail -40 "$HOSTLOG" >&2 || true

    if [ "$READ" -eq 0 ]; then
        echo "$ROW: FAIL — no bundle was readable" >&2
        RC=1
    fi
    # A bundle carrying no Capture at all is a legal Session (I12) and a useless
    # IOP-3: it would pass every assertion above without ever exercising the
    # identity rule the row exists for.
    #
    # ⚠ IOP-3 ONLY.  This host's own Session record (IOP-10) carries Shots and
    # no Captures, because this host owns no capture Stream — it arbitrates over
    # a device's.  A Session of Shots with no Captures is exactly what 5.10e's
    # hosted Session looks like from the arbitrating end, and requiring a
    # Capture there would be requiring the host to own a camera.
    if [ -z "${WRITE_FIRST:-}" ] && [ "$CAPTURED" -eq 0 ]; then
        echo "$ROW: FAIL — $READ bundle(s) read and not one Capture between them" >&2
        RC=1
    fi
    echo "$ROW: $READ bundle(s), $CAPTURED Captures imported in total" >&2

    if [ "$RC" -eq 0 ]; then echo "$ROW: PASS — $WHAT" >&2; fi
    exit $RC
fi

# shellcheck disable=SC2086
"$HOST" --row "$ROW" --port 0 --port-file "$PORTFILE" --summary "$SUMMARY" \
        --run-ms "$HOST_RUN_MS" $HOST_EXTRA >"$HOSTLOG" 2>&1 &
HOSTPID=$!
trap 'kill $HOSTPID 2>/dev/null || true' EXIT INT TERM

i=0
while [ ! -f "$PORTFILE" ]; do
    i=$((i + 1))
    if [ $i -gt 200 ]; then
        echo "$ROW: the host never wrote a port file; its log follows" >&2
        cat "$HOSTLOG" >&2 || true
        exit 1
    fi
    sleep 0.05
done
PORT=$(cat "$PORTFILE")
echo "$ROW: host on 127.0.0.1:$PORT (PLAINTEXT harness socket)" >&2

set +e
"$SIM" --role "$SIM_ROLE" --connect "127.0.0.1:$PORT" \
       --declaration "$SIM_DECL" --scenario "$SIM_SCENARIO" \
       --run-ms "$SIM_RUN_MS" --log-prefix "$ROW-sim" \
       --expect "$SIM_EXPECT" >"$SIMLOG" 2>&1
SIMRC=$?
set -e

# The host is given time to finish the link and write its summary; it stops on
# its own `--run-ms`, which is longer than the simulator's on purpose.
wait $HOSTPID 2>/dev/null || true
trap - EXIT INT TERM

echo "── the counterpart's view ($SIM_SCENARIO over $(basename "$SIM_DECL")) ──" >&2
tail -40 "$SIMLOG" >&2 || true
echo "── this host's log ──────────────────────────────────────────────" >&2
tail -40 "$HOSTLOG" >&2 || true

RC=0
if [ "$SIMRC" -ne 0 ]; then
    echo "$ROW: FAIL — the counterpart exited $SIMRC (a violation or an unmet --expect)" >&2
    RC=1
fi

python3 - "$SUMMARY" "$ROW" $HOST_ASSERT >&2 <<'PY' || RC=1
import json, sys

path, row = sys.argv[1], sys.argv[2]
try:
    d = json.load(open(path))
except Exception as e:                                     # noqa: BLE001
    print(f"{row}: FAIL — no host summary at {path}: {e}")
    sys.exit(1)

# ⚠ DERIVED, AND THE ONE ASSERTION THAT IS NOT A COUNTER.  CONF 5b asks that a
# host never substitutes a zero offset for a relation it does not have, and a
# substituted zero is invisible everywhere else: it is a number that looks like
# every other number.  So the host is asked, for each of the counterpart's
# declared clocks, whether it claims a reading — and for an `unrelated` peer the
# answer is no, for all of them.
d["counterpart_offsets"] = sum(1 for t in d.get("counterpart_timebases", [])
                               if t.get("has_offset"))

bad = []
for a in sys.argv[3:]:
    for op in (">=", "<=", "="):
        if op in a:
            k, v = a.split(op, 1)
            break
    else:
        bad.append(f"unparseable assertion {a!r}")
        continue
    if k not in d:
        bad.append(f"{k} is not in the summary")
        continue
    got = d[k]
    if v in ("true", "false"):
        want = (v == "true")
        ok = (bool(got) == want)
    else:
        want = int(v)
        ok = {">=": got >= want, "<=": got <= want, "=": got == want}[op]
    if not ok:
        bad.append(f"{k} is {got!r}, expected {op} {v}")

if bad:
    print(f"{row}: FAIL — this host's own view:")
    for b in bad:
        print("   " + b)
    print("   summary: " + json.dumps(d, indent=2))
    sys.exit(1)
print(f"{row}: the host's own view holds ({len(sys.argv) - 3} assertions)")
sys.exit(0)
PY

if [ "$RC" -eq 0 ]; then echo "$ROW: PASS — $WHAT" >&2; fi
exit $RC
