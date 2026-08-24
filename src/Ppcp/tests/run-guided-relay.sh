#!/bin/sh
# ── H10's gate: a guided pairing against `ppcp-relay`, and my own 11.5c mirror ─
#
#   run-guided-relay.sh <ppcp_bootstrap_test> <ppcp-relay> [workdir]
#
# TWO runs, and they measure different things:
#
#   1. `--peer acceptor`  — an HONEST stand-in opens a window and this
#      application dials it, compares six digits and completes a pairing.  That
#      is the C2 gate's first half: "your application completes a guided pairing
#      against the relay".
#
#      ⛔ THE STAND-IN IS NOT A CONFORMANT PEER AND THE RELAY SAYS SO ITSELF:
#      "it affirms its own comparison in software, which is the one thing 11.1d
#      forbids."  So this run demonstrates that THIS side completes a real
#      exchange over a real socket.  It demonstrates nothing about the stand-in,
#      and it is not evidence about the security property.
#
#   2. `--probe order-initiator` — ⛔ MY OWN MIRROR, AND I RUN ONLY MINE (CA4).
#      The probe accepts, reads the `bs_offer`, asserts it carries `v` and `ct`
#      ONLY, and then NEVER REPLIES.  The row passes if and only if no
#      `bs_reveal` follows: `pk_i` goes out after `bs_accept` or it does not go
#      out at all (11.5b, 11.5d).  ⛔ NOTHING ON THE WIRE DISTINGUISHES A PEER
#      THAT GETS THIS RIGHT FROM ONE THAT DOES NOT, WHICH IS WHY THE INSTRUMENT
#      HAS TO WITHHOLD THE REPLY — and it is why the VERDICT here is the probe's
#      exit code and not the test's.
#
# ⚠ THE RELAY NEEDS `openssl` ON PATH (11.11: the private scalar lives in its
# helper and only `pk` and `Z` cross).  /opt/homebrew/bin on this machine.
#
# ⛔ AND THIS SCRIPT PRINTS NO RV-6 AGGREGATE.  RV 9g: a conformance claim names
# RT-20c and states its result, and RT-20c needs BOTH applications either side
# of the relay — it is unrun.  Two green rows here are two rows, not a pass.
#
# Exit 0 both rows passed.  1 a row failed.  2 a bad invocation or a missing
# tool, which is not a finding against anything.
set -eu

if [ $# -lt 2 ]; then
    echo "usage: run-guided-relay.sh <ppcp_bootstrap_test> <ppcp-relay> [workdir]" >&2
    exit 2
fi

TESTBIN="$1"
RELAY="$2"
WORK="${3:-$(mktemp -d)}"

[ -x "$TESTBIN" ] || { echo "no test binary at $TESTBIN" >&2; exit 2; }
[ -x "$RELAY" ]   || { echo "no relay at $RELAY" >&2; exit 2; }
command -v openssl >/dev/null 2>&1 || {
    echo "openssl is not on PATH — the relay's §11.11 helper needs it" >&2
    exit 2
}

mkdir -p "$WORK"
RC=0
KIDS=""

cleanup() {
    for k in $KIDS; do kill "$k" 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM

# An ephemeral port the relay can bind.  Asking the kernel is more reliable
# than picking a number, and a collision here would look exactly like a defect.
free_port() {
    python3 - <<'EOF'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
EOF
}

# ── Row 1 — a complete guided pairing ──────────────────────────────────────
P1=$(free_port)
echo "== the relay's honest acceptor stand-in on 127.0.0.1:$P1"
"$RELAY" --peer acceptor --listen "$P1" > "$WORK/peer.log" 2>&1 &
KIDS="$KIDS $!"
sleep 1

if PPCP_BS_RELAY="127.0.0.1:$P1" \
   "$TESTBIN" --gtest_filter=PpcpGuidedRelay.CompletesAPairingAgainstTheRelay \
   > "$WORK/pair.log" 2>&1; then
    echo "PASS  a guided pairing completed against the relay's stand-in"
    # ⛔ BOTH SETS OF DIGITS, SIDE BY SIDE, FOR A PERSON TO READ.  This script
    # does not compare them and must never learn how (11.1d, trap 8): the
    # comparison has value only because it crosses a channel an attacker is not
    # on, and a shell pipeline is not one.
    grep -iE 'SAS|digits' "$WORK/pair.log" "$WORK/peer.log" 2>/dev/null || true
else
    echo "FAIL  no guided pairing completed — $WORK/pair.log"
    tail -30 "$WORK/pair.log" || true
    RC=1
fi

# ── Row 2 — RT-20b(ii), the initiator half of 11.5c ────────────────────────
P2=$(free_port)
echo "== the ordering probe on 127.0.0.1:$P2 (it will never reply)"
"$RELAY" --probe order-initiator --listen "$P2" > "$WORK/probe.log" 2>&1 &
PROBE=$!
KIDS="$KIDS $PROBE"
sleep 1

PPCP_BS_PROBE="127.0.0.1:$P2" \
    "$TESTBIN" --gtest_filter=PpcpGuidedRelay.DialsForTheOrderingProbe \
    > "$WORK/dial.log" 2>&1 || true

# ⛔ THE PROBE'S EXIT CODE IS THE ROW.  Not the test's.
if wait "$PROBE"; then
    echo "PASS  RT-20b(ii)/initiator — bs_offer carried v and ct only, and no"
    echo "      bs_reveal followed an unanswered offer (11.5b, 11.5d)"
else
    echo "FAIL  RT-20b(ii)/initiator — see $WORK/probe.log"
    RC=1
fi
cat "$WORK/probe.log" || true
KIDS=$(echo "$KIDS" | sed "s/ $PROBE//")

echo
echo "⛔ These are two rows, not an RV-6 pass.  9g: RT-20c is unrun and needs"
echo "   both applications either side of the relay.  No aggregate is claimed."
exit $RC
