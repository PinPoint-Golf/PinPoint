/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

import QtQuick
import QtTest
import PinPointStudio

// The session diagnostics panel's body, loaded for real and pressed through its states.
//
// THE ASSERTIONS THAT EARN THIS FILE are the ones about RESTRAINT, because they are the
// ones a screenshot cannot make and a session cannot be made to produce on demand:
//
//   · Cold with an empty fault profile omits the "usually yours" block ENTIRELY. An
//     expectations heading over nothing reads as "the model looked and found none", which
//     is a different claim from "there is no profile yet".
//   · The not-assessable tick EXISTS, is shorter than a firing, and is never a gap. That
//     one is brief §3.1 and §5.2 drawn: a gap in the run reads as "nothing happened on
//     that swing", and the ledger has no such state.
//   · The bandwidth-quiet strip REPLACES the delta strip at the same height rather than
//     collapsing it, so a quiet panel can never be mistaken for a stalled one.
//
// It loads PpSessionDiagnosticsBody rather than PpSessionDiagnosticsPanel deliberately, and
// the split exists for this: the body takes a `source` and owns none of it, so the fixture
// below is the exact shape SessionDiagnosticsModel publishes and nothing here is a stand-in
// for the thing under test. The panel next door is the model, the session directory and the
// ingest connection — none of which a UI test should have to stand up to find out whether a
// tick is the right height.
Item {
    id: probe
    width: 1168; height: 560

    property var src: null

    // What the panel asked for, and never what it did about it — the screen flow is not
    // designed (brief §9), so the signal is the whole of the contract under test.
    property string lastScreenRef: ""
    property string lastScreenCondition: ""

    PpSessionDiagnosticsBody {
        id: body
        anchors.fill: parent
        source: probe.src
        readout: probe.readout
        onScreenRequested: (ref, cond) => {
            probe.lastScreenRef = ref
            probe.lastScreenCondition = cond
        }
    }

    // shotReadout() is an invokable, so the panel fetches it and hands it down — which means
    // the body takes it as a second input and a fixture supplies it the same way it supplies
    // `source`. Null is "no shot selected", which is a state under test in its own right.
    property var readout: null

    // The carousel's half of 13a, pressed on its own. PpShotCard needs a dozen proxy-model
    // roles and a ListView to exist at all; the pip row needs a list of states, which is the
    // whole reason it was extracted rather than left inline on the card.
    PpShotPipRow {
        id: pipProbe
        width: Theme.sp(127); height: Theme.sp(14)
        pips: probe.pipFixture
        firedCount: probe.pipFired
    }
    property var pipFixture: []
    property int pipFired: 0

    // ── fixtures, in the shape the model publishes ───────────────────────────

    function tick(state) { return { state: state, shotId: 0, selected: false } }

    function coldSource(expectations) {
        return {
            stage: "cold",
            headerInfo: {
                stage: "cold", stageLabel: "COLD", shotLabel: "shot 2",
                countLine: "0 patterns · counted over all 2 shots",
                cadenceNote: "", coldLine: "Too few swings to call a pattern.",
                formingLine: "", closingLine: ""
            },
            thisShot: [{ kind: "clean", name: "clean on every measurable condition" }],
            quiet: false,
            afterShotDelta: { headline: "0 of 7 conditions fired on this swing · 0 of them patterns",
                              note: "", surfaced: true },
            cards: [], watching: [], bookends: [],
            expectations: expectations,
            unchainedLine: "",
            coverageLine: "23 of 140 characteristics measurable with current capture"
        }
    }

    readonly property var expectationRows: [
        { id: "casting", name: "Casting",
          text: "pattern in 4 of your last 6 sessions", caveat: "an expectation to test, not a finding" },
        { id: "early_extension", name: "Early extension",
          text: "pattern in 3 of your last 6 sessions", caveat: "an expectation to test, not a finding" },
        { id: "over_the_top", name: "Over the top",
          text: "pattern in 2 of your last 6 sessions", caveat: "an expectation to test, not a finding" }
    ]

    // Two patterns, no authored edge between them — the Forming state's whole reason for
    // existing. The first card's run holds all three row states, including the one this
    // file is here to check.
    function formingSource(quiet) {
        return {
            stage: "forming",
            headerInfo: {
                stage: "forming", stageLabel: "FORMING", shotLabel: "shot 5",
                countLine: "2 patterns · counted over all 5 shots",
                cadenceNote: quiet ? "BANDWIDTH · QUIET" : "",
                coldLine: "",
                formingLine: "No chain is drawn: the model authors no edge between these patterns.",
                closingLine: ""
            },
            thisShot: [
                { id: "casting", name: "Casting", kind: "fired", tier: "pattern", focus: false },
                { id: "face_roll", name: "Face roll through impact", kind: "fired", tier: "watching", focus: false },
                { kind: "notAssessable", name: "2 not assessable on this capture" }
            ],
            quiet: quiet,
            afterShotDelta: { headline: "2 of 5 conditions fired on this swing · 1 of them pattern",
                              note: "2 measures not assessable on this capture", surfaced: !quiet },
            cards: [
                { id: "casting", name: "Casting", consequence: "", tier: "pattern",
                  recurrence: "4 of 5 measurable shots", fired: 4, assessable: 5,
                  fresh: true, resolving: false, thisShot: "fired", statePill: "FIRED",
                  directionClaimed: true,
                  directionText: "Consistent direction: the high side on 92% of its firings.",
                  trend: "worsening", trendArrow: "↑", trendText: "worsening",
                  recencyText: "fired on the last measurable shot",
                  evidence: "Casting on 4 of the 5 swings where rushed transition fired — on 0 of the 1 where it did not.",
                  ticks: [tick("fired"), tick("notAssessable"), tick("fired"),
                          tick("clean"), tick("fired")] },
                { id: "face_roll", name: "Face roll through impact", consequence: "", tier: "pattern",
                  recurrence: "3 of 5 measurable shots", fired: 3, assessable: 5,
                  fresh: false, resolving: false, thisShot: "clean", statePill: "CLEAN",
                  directionClaimed: false,
                  directionText: "Direction agreement 56%, below the 70% gate: dispersion, not a direction.",
                  trend: "stable", trendArrow: "", trendText: "trend after 6 measurable shots",
                  recencyText: "last fired 1 measurable shots ago",
                  evidence: "The face arrives at a different angle each time, which is why the miss has two sides.",
                  ticks: [tick("clean"), tick("fired"), tick("fired"),
                          tick("fired"), tick("clean")] }
            ],
            watching: [
                { id: "sway", name: "Lateral sway", recurrence: "1 of 5 measurable shots" },
                { id: "flat_shoulder", name: "Flat shoulder plane", recurrence: "1 of 4 measurable shots" }
            ],
            bookends: [],
            expectations: [],
            unchainedLine: "Face roll through impact · no authored edge to any other pattern this session",
            coverageLine: "23 of 140 characteristics measurable with current capture"
        }
    }

    // ── the rail ─────────────────────────────────────────────────────────────
    // Brief §4's two chains, because between them they are the whole honesty argument:
    // chain A is a fully live spine and chain B is mostly unmeasurable — a screened root, two
    // ghosts, one live card and the miss the golfer declared. All five link grades appear
    // across the two, which is the point of pressing both rather than one nice one.
    function chainNode(id, name, kind, mark, extra) {
        const n = {
            id: id, name: name, kind: kind, mark: mark,
            measure: "", phase: "", focused: false,
            recurrence: "", ticks: [], trend: "stable", trendArrow: "",
            state: "notAssessable", statePill: "NOT MEASURED", evidence: ""
        }
        for (const k in extra)
            n[k] = extra[k]
        return n
    }

    function liveNode(id, name, phase, measure, recurrence, state, pill, trend, arrow, evidence) {
        return chainNode(id, name, "live", "", {
            phase: phase, measure: measure, recurrence: recurrence,
            state: state, statePill: pill, trend: trend, trendArrow: arrow,
            evidence: evidence,
            ticks: [tick("fired"), tick("notAssessable"), tick("fired"),
                    tick("clean"), tick("fired"), tick("fired")]
        })
    }

    function chainLink(from, to, grade, word, stroke, width, arrow, opacity, note) {
        return { from: from, to: to, grade: grade, word: word, stroke: stroke,
                 width: width, arrow: arrow, opacity: opacity, note: note, pairs: 12 }
    }

    function establishedSource(eligible) {
        const chainA = {
            anyLive: true, liveCount: 4,
            nodes: [
                liveNode("rushed_transition", "Rushed transition", "transition", "transition tempo",
                         "5 of 6 measurable shots", "fired", "FIRED", "worsening", "↑",
                         "Rushed transition on 5 of the 6 swings measured."),
                liveNode("casting", "Casting", "downswing", "wrist release rate",
                         "4 of 6 measurable shots", "fired", "FIRED", "worsening", "↑",
                         "Casting on 4 of the 5 swings where rushed transition fired — on 0 of the 1 where it did not."),
                liveNode("shaft_lean", "Not enough shaft lean", "impact", "shaft lean at impact",
                         "4 of 6 measurable shots", "clean", "CLEAN", "stable", "",
                         "Shaft lean sits behind the ball on 4 of 6."),
                liveNode("scooping", "Scooping", "impact", "lead wrist extension",
                         "3 of 6 measurable shots", "fired", "FIRED", "improving", "↓",
                         "Scooping on 3 of the 6 swings measured.")
            ],
            links: [
                chainLink("rushed_transition", "casting", "movedTogether", "Moved together",
                          "solidAccent", 2.0, true, 1.0, "baseline 5 · 6 since focus"),
                chainLink("casting", "shaft_lean", "conditionallyDependent", "Conditionally dependent",
                          "solid", 1.5, true, 1.0, "Fisher exact · 12 paired shots"),
                chainLink("shaft_lean", "scooping", "coherent", "Coherent",
                          "dashed", 1.5, false, 1.0, "no variance to test")
            ]
        }
        const chainB = {
            anyLive: true, liveCount: 1,
            nodes: [
                chainNode("pelvic_disassociation", "Poor pelvic disassociation", "screenedRoot",
                          "screened root · needs a physical screen",
                          { evidence: "A thirty-second physical test would settle whether this is the root." }),
                chainNode("pelvis_slow", "Pelvis slow to start turning", "ghost",
                          "ghost · measure planned", {}),
                chainNode("over_the_top", "Over the top", "ghost",
                          "ghost · authored as asserted, no measure", {}),
                liveNode("path_out_to_in", "Path too far out-to-in", "impact", "club path",
                         "5 of 6 measurable shots", "fired", "FIRED", "worsening", "↑",
                         "Path sits left of neutral on 5 of the 6 swings the monitor read."),
                chainNode("slice", "Slice", "outcome", "declared miss · launch-monitor verified",
                          { recurrence: "5 of 6 shots", state: "fired", statePill: "FIRED" })
            ],
            links: [
                chainLink("pelvic_disassociation", "pelvis_slow", "unanchored", "Unanchored",
                          "dottedFaint", 1.0, false, 0.5, "screen would confirm"),
                chainLink("pelvis_slow", "over_the_top", "presentTogether", "Present together",
                          "dotted", 1.0, false, 1.0, "neither measured"),
                chainLink("over_the_top", "path_out_to_in", "coherent", "Coherent",
                          "dashed", 1.5, false, 1.0, "no variance to test"),
                chainLink("path_out_to_in", "slice", "conditionallyDependent", "Conditionally dependent",
                          "solid", 1.5, true, 1.0, "Fisher exact · 6 paired shots")
            ]
        }

        const driver = eligible
            ? { eligible: true,
                rootId: "rushed_transition", rootName: "Rushed transition", coverage: 3,
                whyText: "Rushed transition — would explain 3 of your patterns",
                explains: [{ id: "casting", name: "Casting" }],
                screenConditionId: "pelvic_disassociation",
                screenRef: "screen.pelvic_disassociation",
                screenLabel: "Seated trunk rotation",
                screenCta: "Seated trunk rotation would anchor this chain",
                screenReason: "it would explain 4 of your patterns",
                screenProtocol: "Sit, cross the arms, rotate.",
                rival: { childId: "casting", childName: "Casting",
                         chosenParentId: "rushed_transition", chosenName: "Rushed transition",
                         rivalParentId: "grip_strength", rivalName: "Grip too strong",
                         adjudicated: false,
                         text: "Grip too strong could also explain Casting — not adjudicated: it is not measurable on this capture." },
                offered: [] }
            : { eligible: false,
                waitingText: "the driver appears once the pattern set has held still for 3 shots" }

        return {
            stage: "established",
            headerInfo: {
                stage: "established", stageLabel: "ESTABLISHED", shotLabel: "shot 11",
                countLine: "4 patterns · counted over all 11 shots",
                cadenceNote: "", coldLine: "", formingLine: "", closingLine: ""
            },
            thisShot: [
                { id: "casting", name: "Casting", kind: "fired", tier: "pattern", focus: false },
                { kind: "notAssessable", name: "2 not assessable on this capture" }
            ],
            quiet: false,
            afterShotDelta: { headline: "3 of 9 conditions fired on this swing · 2 of them patterns",
                              note: "", surfaced: true },
            cards: [], expectations: [], bookends: [],
            chains: [chainA, chainB],
            unchained: [{ id: "face_roll", name: "Face roll through impact",
                          recurrence: "3 of 11 measurable shots" }],
            unchainedLine: "Face roll through impact is a pattern the model authors no edge for this session — reported on its own.",
            driver: driver,
            watching: [{ id: "sway", name: "Lateral sway", recurrence: "1 of 11 measurable shots" }],
            coverageLine: "23 of 140 characteristics measurable with current capture"
        }
    }

    function closingSource() {
        const s = formingSource(false)
        s.stage = "closing"
        s.closed = true
        s.headerInfo.stage = "closing"
        s.headerInfo.stageLabel = "CLOSING"
        s.headerInfo.closingLine = "Counts are session totals; this shot is the wide tick."
        s.bookends = [
            { id: "casting", name: "Casting", worstText: "worst · shot 3",
              bestText: "best · shot 11", representativeText: "most representative · shot 7" },
            { id: "face_roll", name: "Face roll through impact", worstText: "worst · shot 2",
              bestText: "best · shot 9", representativeText: "most representative · shot 6" }
        ]
        return s
    }

    // ── review (13a, brief §6) ───────────────────────────────────────────────
    //
    // A FINISHED SESSION THAT STILL HAS ITS RAIL, because that is the arrangement review is
    // for: session counts under every node, the reviewed shot as the one wide tick inside
    // each run, and each node saying what happened to it AFTER that swing. The stage is
    // Closing (the model freezes it there while reviewing) and the chains are the ones the
    // Established fixture publishes — the panel does not rewind, so they are the same chains.

    // The selected shot's tick, decided by the model and never by the delegate. `state` is
    // what that shot did to this condition, so the run and the state pill beside it are the
    // same fact — the model keeps those two in step and a fixture that did not would be
    // asserting a panel the model cannot produce.
    function selectTickAt(node, i, state) {
        const ts = []
        for (let j = 0; j < node.ticks.length; ++j)
            ts.push({ state: j === i ? state : node.ticks[j].state,
                      shotId: j, selected: j === i })
        node.ticks = ts
        return node
    }

    function reviewSource() {
        const s = establishedSource(true)
        s.stage = "closing"
        s.closed = true
        s.headerInfo.stage = "closing"
        s.headerInfo.stageLabel = "CLOSING"
        s.headerInfo.shotLabel = "shot 9 of 14"
        s.headerInfo.countLine = "4 patterns · counted over all 14 shots"
        s.headerInfo.closingLine = "Counts are session totals; this shot is the wide tick."
        s.headerInfo.reviewBadge = "REVIEWING · shot 9 of 14"
        s.headerInfo.reviewNote  = "final session state · this shot read inside the finished ledger"
        s.headerInfo.reviewFootLine =
            "The ledger stays at the finished session: counts under each node are the totals "
            + "over all 14 shots, and this shot is the wide tick inside each run. Recurrence "
            + "is not a rate at this n."

        // The review tense on the nodes: the pill says what happened HERE, the recency line
        // says what happened after, and the run carries the same answer as the wide tick.
        // chain A is rushed transition → casting → shaft lean → scooping, and all three
        // states appear across it so the pill is pressed at each of them.
        const here = [
            { pill: "FIRED HERE",   state: "fired",         after: "1 more firing after this shot" },
            { pill: "FIRED HERE",   state: "fired",         after: "3 more firings after this shot" },
            { pill: "CLEAN HERE",   state: "clean",         after: "no firings after this shot" },
            { pill: "NOT MEASURED", state: "notAssessable", after: "no firings after this shot" }
        ]
        for (let i = 0; i < s.chains[0].nodes.length; ++i) {
            s.chains[0].nodes[i].statePill        = here[i].pill
            s.chains[0].nodes[i].firingsAfterText = here[i].after
            selectTickAt(s.chains[0].nodes[i], 2, here[i].state)
        }
        return s
    }

    // The nine cells: fired, clean and the two the capture could not answer — which are the
    // ones this fixture exists for. Their value slot reads "not measurable" and their corridor
    // slot carries the REASON, and neither is ever blank.
    function reviewReadout() {
        function cell(id, name, kind, state, val, band, tier) {
            return { id: id, name: name, stateKind: kind, state: state,
                     valueText: val, corridorText: band,
                     reason: kind === "notAssessable" ? band : "",
                     tierTag: tier, recurrence: "", ticks: [], selectedIndex: 8,
                     firingsAfter: 0, firingsAfterText: "no firings after this shot" }
        }
        return {
            shotId: 9, shotIndex: 8, shotCount: 14, club: "7i",
            firedCount: 7, cleanCount: 2, notAssessableCount: 2,
            headline: "7 of 9 conditions fired on this swing · 4 of them patterns",
            note: "2 measures not assessable on this capture",
            conditions: [
                // The true minus (U+2212) is the model's, and it is what makes the reading
                // line up with the corridor beside it. Nothing in the QML reformats it.
                cell("casting", "Casting", "fired", "OUT",
                     "−4.4°", "pass −2.0 to +2.0°", "pattern"),
                cell("rushed_transition", "Rushed transition", "fired", "OUT",
                     "18 ms", "pass 30 to 70 ms", "pattern"),
                cell("shaft_lean", "Not enough shaft lean", "clean", "IN",
                     "+6.2°", "pass +4.0 to +12.0°", "watching"),
                cell("scooping", "Scooping", "fired", "OUT",
                     "12.4°", "pass 0.0 to 8.0°", "pattern"),
                cell("face_roll", "Face roll through impact", "clean", "IN",
                     "2.1°", "pass 0.0 to 4.0°", "clean all session"),
                cell("sway", "Lateral sway", "fired", "OUT",
                     "84 mm", "pass 20 to 55 mm", "watching"),
                cell("path_out_to_in", "Path too far out-to-in", "fired", "OUT",
                     "−5.1°", "pass −2.0 to +2.0°", "pattern"),
                cell("head_height", "Head height Δ", "notAssessable", "—",
                     "not measurable", "shaft occluded at P6", "clean all session"),
                cell("x_factor", "X-factor", "notAssessable", "—",
                     "not measurable", "ball not tracked", "watching")
            ]
        }
    }

    TestCase {
        id: tc
        name: "SessionDiagnostics"
        when: windowShown

        // ── tree walking ─────────────────────────────────────────────────────
        // findChild() finds one; most of what matters here is a COUNT — how many
        // expectation cards, how many ticks, how many of them are short.
        function findAll(item, name, out) {
            out = out || []
            if (!item)
                return out
            const kids = item.children
            for (let i = 0; i < kids.length; ++i) {
                if (kids[i].objectName === name)
                    out.push(kids[i])
                findAll(kids[i], name, out)
            }
            return out
        }

        function visibleAll(item, name) {
            return findAll(item, name).filter(function (i) { return i.visible })
        }

        function one(item, name) {
            const all = findAll(item, name)
            verify(all.length >= 1, "expected a '" + name + "', found none")
            return all[0]
        }

        // An item is only really on screen if every ancestor up to the body is too.
        function shown(item) {
            let i = item
            while (i && i !== body) {
                if (!i.visible)
                    return false
                i = i.parent
            }
            return !!i
        }

        function setSource(s) {
            probe.src = null
            wait(0)
            probe.src = s
            wait(0)
        }

        // Review is TWO inputs — the surface and the selected shot's readout — because
        // selection is what turns "the finished session" into "this shot inside it".
        function setReview(s, r) {
            setSource(s)
            probe.readout = r
            wait(0)
        }

        function init() {
            probe.width = 1168; probe.height = 560
            Theme.themeIndex = 5          // studio dark, the design's own frame
            Theme.fontScale = 1.0
            body.watchingExpanded = false
            body.expandedChain = -1
            probe.readout = null
            probe.pipFixture = []
            probe.pipFired = 0
            probe.lastScreenRef = ""
            probe.lastScreenCondition = ""
            wait(0)
        }

        // ── rail helpers ─────────────────────────────────────────────────────

        function linkByGrade(grade) {
            const all = visibleAll(body, "sdChainLink")
            for (let i = 0; i < all.length; ++i)
                if (all[i].grade === grade)
                    return all[i]
            return null
        }

        function nodeById(id) {
            const all = findAll(body, "sdChainNode")
            for (let i = 0; i < all.length; ++i)
                if (all[i].node && all[i].node.id === id && shown(all[i]))
                    return all[i]
            return null
        }

        // ── Cold ─────────────────────────────────────────────────────────────

        function test_01_coldSaysTheNIsTooSmall() {
            setSource(coldSource(probe.expectationRows))

            verify(shown(one(body, "sdColdBody")), "the Cold body is the one on screen")
            verify(!shown(one(body, "sdCardsBody")), "and the card row is not")

            compare(one(body, "sdColdHeadline").text, "Too few swings to call a pattern.")
            compare(one(body, "sdColdSubline").text, "0 patterns · counted over all 2 shots")
            compare(one(body, "sdStageChip").visible, true, "the stage chip is drawn")
        }

        function test_02_coldDrawsTheExpectationsDashed() {
            setSource(coldSource(probe.expectationRows))

            verify(shown(one(body, "sdExpectations")), "USUALLY YOURS is on screen")
            const cards = visibleAll(body, "sdExpectationCard")
            compare(cards.length, 3, "one dashed card per expectation")
            for (let i = 0; i < cards.length; ++i) {
                verify(cards[i].width > 0 && cards[i].height > 0,
                       "expectation card " + i + " has a size")
                verify(cards[i].width <= probe.width, "and fits the panel")
            }
        }

        // The one a screenshot never catches: no profile, so no heading either.
        function test_03_coldWithNoProfileOmitsTheBlockEntirely() {
            setSource(coldSource([]))

            verify(shown(one(body, "sdColdBody")), "still the Cold body")
            compare(one(body, "sdColdHeadline").text, "Too few swings to call a pattern.")
            verify(!shown(one(body, "sdExpectations")),
                   "an empty profile draws no 'expectations' heading at all")
            compare(findAll(body, "sdExpectationCard").length, 0, "and no cards")
        }

        // ── Forming ──────────────────────────────────────────────────────────

        function test_04_formingDrawsTheCardsAndTheirState() {
            setSource(formingSource(false))

            verify(shown(one(body, "sdCardsBody")), "the card row is the body")
            verify(!shown(one(body, "sdColdBody")), "and Cold is gone")

            const cards = visibleAll(body, "sdPatternCard")
            compare(cards.length, 2, "both patterns fit at the design size")

            compare(findAll(cards[0], "sdCardName")[0].text, "Casting")
            compare(findAll(cards[0], "sdStatePill")[0].children[0].text, "FIRED")
            compare(findAll(cards[0], "sdCardRecurrence")[0].text, "4 of 5 measurable shots")
            compare(findAll(cards[0], "sdCardFresh")[0].visible, true, "the NEW tag is on the fresh one")

            compare(findAll(cards[1], "sdStatePill")[0].children[0].text, "CLEAN")
            compare(findAll(cards[1], "sdCardFresh")[0].visible, false, "and not on the other")
            // Below the agreement gate the card reports dispersion, not a direction.
            verify(findAll(cards[1], "sdCardDirection")[0].text.indexOf("dispersion") >= 0,
                   "the withheld direction claim says so")

            // The forming note that stops the flat row reading as a failed chain.
            verify(one(body, "sdFormingNote").text.indexOf("no edge") >= 0,
                   "the no-authored-edge line is on screen")
        }

        // ── the run ──────────────────────────────────────────────────────────

        function test_05_notAssessableIsShortAndOutlined_neverAGap() {
            setSource(formingSource(false))

            const card = visibleAll(body, "sdPatternCard")[0]
            const run = findAll(card, "sdTickRun")[0]
            const ticks = findAll(run, "sdTick")

            // Five rows in, five ticks out. Nothing is skipped for being unmeasurable.
            compare(ticks.length, 5, "one tick per shot, including the unmeasurable one")

            const fired = ticks[0], na = ticks[1], clean = ticks[3]
            verify(na.height > 0, "the not-assessable tick is drawn")
            verify(na.height < fired.height,
                   "and is shorter than a firing (" + na.height + " vs " + fired.height + ")")
            verify(na.border.width >= 1, "outlined rather than filled")
            compare(na.color.a, 0, "with no fill")
            verify(fired.color !== clean.color, "fired and clean are different marks")
            compare(fired.color, Theme.colorError)
            compare(clean.color, Theme.colorGood)

            // Bottom-aligned: the short tick sits on the run's baseline, so the run reads as
            // a sequence with a weaker mark in it rather than one with a hole.
            compare(na.y + na.height, fired.y + fired.height)
        }

        // ── cadence ──────────────────────────────────────────────────────────

        function test_06_quietSwapsTheStripWithoutCollapsingIt() {
            setSource(formingSource(false))
            const strip = one(body, "sdThisShotStrip")
            const loudH = strip.height
            verify(shown(one(body, "sdChipState")), "the delta strip is up")
            verify(!shown(one(body, "sdQuietState")), "and the quiet one is not")
            compare(visibleAll(body, "sdChip").length, 3, "a chip per this-shot entry")

            setSource(formingSource(true))
            verify(shown(one(body, "sdQuietState")), "quiet takes the strip")
            verify(!shown(one(body, "sdChipState")), "and the chips stand down")
            compare(one(body, "sdQuietLabel").text, "BANDWIDTH · QUIET")
            verify(one(body, "sdQuietLine").text.length > 0,
                   "the quiet state still states what the shot did")
            // The strip must not shrink: a collapsed strip reads as a stalled panel.
            compare(one(body, "sdThisShotStrip").height, loudH,
                    "quiet is the same height as loud")
        }

        // ── Closing ──────────────────────────────────────────────────────────

        function test_07_closingReplacesTheStripWithBookends() {
            setSource(closingSource())

            verify(shown(one(body, "sdBookends")), "the bookends strip is up")
            verify(!shown(one(body, "sdThisShotStrip")),
                   "a closed session has no after-shot moment to report")
            compare(visibleAll(body, "sdBookend").length, 2, "one column per bookended pattern")
            // The body it had is the body it keeps.
            verify(shown(one(body, "sdCardsBody")), "the cards survive the close")
        }

        // ── watching + coverage ──────────────────────────────────────────────

        function test_08_watchingCollapsesAndExpandsIntoTheSameContent() {
            setSource(formingSource(false))

            const row = one(body, "sdWatchingRow")
            verify(row.visible, "two watched conditions put the row on screen")
            compare(one(body, "sdWatchingLabel").text.indexOf("WATCHING (2)") >= 0, true)
            verify(one(body, "sdWatchingLine").visible, "collapsed to one truncating line")
            compare(findAll(body, "sdWatchingItem").length, 0, "with no rows of its own")

            const collapsedH = row.height
            body.watchingExpanded = true
            wait(0)
            compare(findAll(body, "sdWatchingItem").length, 2,
                    "expanding opens the same two, one per line")
            // The row's new height reaches the panel through the layout, which settles on a
            // polish rather than on the binding — hence tryVerify and not verify.
            tryVerify(function () { return row.height > collapsedH }, 2000,
                      "the row grew to hold them")
        }

        function test_09_coverageAndUnchainedAreAlwaysStated() {
            setSource(formingSource(false))

            const cov = one(body, "sdCoverageLine")
            verify(cov.visible, "the coverage line is never omitted")
            verify(cov.text.indexOf("140") >= 0, "and states the whole pack, not the measured few")

            verify(shown(one(body, "sdUnchainedRow")),
                   "a pattern with no authored edge gets its own line")
        }

        // ── the fit ──────────────────────────────────────────────────────────

        function test_10_designSizeIsScaleOneAndAWiderStageScalesUp() {
            setSource(formingSource(false))
            fuzzyCompare(body.k, 1.0, 0.001, "1168 x 560 is the design size")
            compare(body.compact, false)

            probe.width = 2336; probe.height = 1120
            wait(0)
            verify(body.k > 1.9, "twice the stage is roughly twice the scale, got " + body.k)
            verify(body.tzBody > Theme.fontSzBody2, "and the type grew with it")

            probe.width = 1168; probe.height = 560
            wait(0)
        }

        // 12c: below the wide arrangement's floor the panel REDUCES rather than shrinking.
        function test_11_narrowReducesRatherThanShrinking() {
            setSource(formingSource(false))
            probe.width = 396; probe.height = 560
            wait(0)

            compare(body.k, 1.0, "never below the design's own type size")
            compare(body.compact, true, "the narrow arrangement's reductions are on")
            compare(one(body, "sdTitle").text, "SESSION DIAG.", "the title abbreviates, never elides")
            compare(one(body, "sdStageNote").visible, false, "the count line stands down")

            const cards = visibleAll(body, "sdPatternCard")
            compare(cards.length, 1, "one card fits")
            verify(cards[0].width <= probe.width, "and it fits the width it was given")
            // What did not fit is COUNTED, never silently dropped.
            verify(one(body, "sdMoreTail").visible, "the pattern that did not fit is counted")
            compare(one(body, "sdMoreTail").text, "+1 more")
        }

        // ── both Studio themes from one layout ───────────────────────────────
        // Theme reads appSettings once at completion and only writes back after that, so the
        // theme is set on Theme here. Setting appSettings.themeIndex would change nothing
        // and the test would assert the same theme twice.
        function test_12_bothStudioThemes() {
            setSource(formingSource(false))

            const geometry = []
            const marks = []
            // 4 = studio light, 5 = studio dark.
            for (let t = 4; t <= 5; ++t) {
                Theme.themeIndex = t
                wait(0)

                const cards = visibleAll(body, "sdPatternCard")
                compare(cards.length, 2, "theme " + t + ": both cards survive")
                geometry.push(cards[0].width + "x" + Math.round(cards[0].height))

                const ticks = findAll(findAll(cards[0], "sdTickRun")[0], "sdTick")
                compare(ticks.length, 5, "theme " + t + ": the run is intact")
                verify(ticks[1].height < ticks[0].height,
                       "theme " + t + ": not-assessable is still the short one")
                marks.push("" + ticks[0].color)

                compare(one(body, "sdColdBody").visible, false,
                        "theme " + t + ": still Forming")
                verify(one(body, "sdCoverageLine").visible, "theme " + t + ": coverage stated")
            }

            compare(geometry[0], geometry[1], "geometry does not depend on the theme")
            verify(marks[0] !== marks[1], "but the fired mark does — both themes are real")

            Theme.themeIndex = 5
            wait(0)
        }

        // ── Established: the chain rail ──────────────────────────────────────

        function test_14_establishedDrawsBothChainsAndOnlyTheEdgesTheModelAuthored() {
            setSource(establishedSource(true))

            verify(shown(one(body, "sdEstablishedBody")), "the rail is the body")
            verify(!shown(one(body, "sdCardsBody")), "and the flat card row is gone")

            const rails = visibleAll(body, "sdChainRail")
            compare(rails.length, 2, "both chains are drawn as rows")

            // Four nodes and three links, five nodes and four links. An edge is drawn ONLY
            // where the model published one, so a rail can never join two nodes because they
            // happened to be adjacent.
            compare(findAll(rails[0], "sdChainNode").length, 4)
            compare(findAll(rails[1], "sdChainNode").length, 5)
            compare(visibleAll(body, "sdChainLink").length, 7,
                    "three edges on chain A and four on chain B, and no others")
        }

        // §4.1's table, drawn. The word and the stroke are published together so a surface
        // cannot pick them independently; this is the half that checks the stroke arrives.
        function test_15_everyLinkGradeGetsItsOwnStroke() {
            setSource(establishedSource(true))

            const moved = linkByGrade("movedTogether")
            verify(moved, "the moved-together edge is on screen")
            compare(moved.strokeStyle, "solid")
            compare(moved.strokeColor, Theme.colorAccent, "accent, and only this grade is")
            fuzzyCompare(moved.strokeWidth, 2.0, 0.01)
            compare(moved.hasArrow, true, "a direction the evidence supports gets a head")
            compare(one(moved, "sdChainLinkWord").text, "Moved together")
            compare(one(moved, "sdChainLinkNote").text, "baseline 5 · 6 since focus")

            const dep = linkByGrade("conditionallyDependent")
            compare(dep.strokeStyle, "solid")
            compare(dep.strokeColor, Theme.colorText2)
            fuzzyCompare(dep.strokeWidth, 1.5, 0.01)
            compare(dep.hasArrow, true)
            verify(one(dep, "sdChainLinkNote").text.indexOf("Fisher exact") >= 0)

            // The three that cannot carry a direction do not get a head, and the stroke is
            // what says so — which is why 12c can drop the note and keep the claim.
            const coh = linkByGrade("coherent")
            compare(coh.strokeStyle, "dashed")
            compare(coh.strokeColor, Theme.colorText2)
            fuzzyCompare(coh.strokeWidth, 1.5, 0.01)
            compare(coh.hasArrow, false, "nothing varied, so nothing was tested")
            compare(one(coh, "sdChainLinkNote").text, "no variance to test")

            const pres = linkByGrade("presentTogether")
            compare(pres.strokeStyle, "dotted")
            compare(pres.strokeColor, Theme.colorText3)
            fuzzyCompare(pres.strokeWidth, 1.0, 0.01)
            compare(pres.hasArrow, false)
            compare(one(pres, "sdChainLinkNote").text, "neither measured")

            const unan = linkByGrade("unanchored")
            compare(unan.strokeStyle, "dotted")
            compare(unan.strokeColor, Theme.colorText3)
            compare(unan.hasArrow, false)
            fuzzyCompare(unan.opacity, 0.5, 0.01,
                         "a root nobody has screened is drawn as barely there")
            compare(one(unan, "sdChainLinkNote").text, "screen would confirm")
        }

        // Chain B is the reason the honesty devices exist (brief §4). Each of the three
        // non-live kinds says a different thing and none of them borrows the live card.
        function test_16_ghostScreenedRootAndOutcomeAreEachDrawnAsThemselves() {
            setSource(establishedSource(true))

            const live = nodeById("casting")
            compare(live.kind, "live")
            compare(one(live, "sdChainNodeName").text, "Casting")
            compare(one(live, "sdChainNodePill").children[0].text, "FIRED")
            // The line the Forming card has no data for and the chain card does.
            compare(one(live, "sdChainNodePhase").text, "downswing · wrist release rate")
            compare(one(live, "sdChainNodeRecurrence").text, "4 of 6 measurable shots")
            compare(findAll(one(live, "sdChainNodeRun"), "sdTick").length, 6,
                    "one tick per shot on the rail too")

            const ghost = nodeById("over_the_top")
            compare(ghost.kind, "ghost")
            verify(one(ghost, "sdChainGhostFrame").visible,
                   "dashed, because it is not evidence from this session")
            verify(ghost.opacity < 1.0, "and dimmed with it")
            compare(one(ghost, "sdChainNodeMark").text, "ghost · authored as asserted, no measure")
            verify(!shown(one(ghost, "sdChainNodeRecurrence")),
                   "a node nobody measured reports no recurrence")

            const screened = nodeById("pelvic_disassociation")
            compare(screened.kind, "screenedRoot")
            compare(one(screened, "sdChainNodeMark").text, "screened root · needs a physical screen")
            verify(shown(one(screened, "sdChainScreenCta")),
                   "a screened root is drawn as a request, never as a finding")

            const outcome = nodeById("slice")
            compare(outcome.kind, "outcome")
            compare(one(outcome, "sdChainNodeName").text, "Slice")
            compare(one(outcome, "sdChainNodeMark").text, "declared miss · launch-monitor verified")
            compare(one(outcome, "sdChainNodeRecurrence").text, "5 of 6 shots")
        }

        // ── the driver footer ────────────────────────────────────────────────

        function test_17_driverPrintsItsOwnFalsifiers() {
            setSource(establishedSource(true))

            verify(shown(one(body, "sdDriverFooter")), "the footer is up")
            compare(one(body, "sdDriverName").text, "Rushed transition")
            verify(one(body, "sdDriverWhy").text.indexOf("3 of your patterns") >= 0,
                   "a count of what it accounts for, never a score")
            verify(shown(one(body, "sdScreenCta")), "with the screen that would anchor it")
            verify(one(body, "sdScreenWhy").text.indexOf("4 of your patterns") >= 0)

            // The two disclosures that stop the driver reading as a verdict.
            verify(one(body, "sdDriverRival").text.indexOf("not adjudicated") >= 0,
                   "the rival parent is named and explicitly not adjudicated")
            verify(one(body, "sdDriverCoverage").text.indexOf("140") >= 0,
                   "and the coverage line rides beside it")
            compare(one(body, "sdCoverageLine").visible, false,
                    "so the panel does not state it twice")

            // The CTA asks; it does not run a screen. Brief §9 — that flow is not designed.
            mouseClick(one(body, "sdDriverFooter"))
            compare(probe.lastScreenRef, "screen.pelvic_disassociation")
            compare(probe.lastScreenCondition, "pelvic_disassociation")
        }

        function test_18_anUnstablePatternSetSaysSoRatherThanShowingADriver() {
            setSource(establishedSource(false))

            verify(shown(one(body, "sdDriverFooter")), "the footer keeps its place")
            verify(shown(one(body, "sdDriverWaiting")), "and says what it is waiting for")
            verify(one(body, "sdDriverWaiting").text.indexOf("held still") >= 0)
            verify(!shown(one(body, "sdDriverName")), "with no driver named")
            // The right-hand column is gone with the driver, so the coverage line goes back
            // to the bottom rather than disappearing with it.
            compare(one(body, "sdCoverageLine").visible, true)

            verify(shown(one(body, "sdUnchainedRow")),
                   "a pattern with no authored edge still gets its own line on the rail")
        }

        // ── 12c: the spine ───────────────────────────────────────────────────

        function test_19_narrowTurnsTheRailAndCollapsesEveryChainButTheFirst() {
            setSource(establishedSource(true))
            probe.width = 396; probe.height = 560
            wait(0)

            compare(body.compact, true)
            const rails = visibleAll(body, "sdChainRail")
            compare(rails.length, 1, "one chain open, the rest collapsed")
            compare(rails[0].vertical, true, "and it is the vertical form")
            compare(findAll(rails[0], "sdChainNodeDot").length, 4,
                    "one line per node, mark carried by the dot")
            // The grade survives the collapse because it is carried by the stroke.
            const moved = linkByGrade("movedTogether")
            verify(moved, "the graded link survives the turn")
            compare(moved.vertical, true)
            compare(moved.strokeColor, Theme.colorAccent)
            compare(moved.hasArrow, true)

            const collapsed = visibleAll(body, "sdChainCollapsed")
            compare(collapsed.length, 1, "chain B is one line")
            compare(one(collapsed[0], "sdChainCollapsedToggle").text, "OPEN ▸")
            verify(one(collapsed[0], "sdChainCollapsedSummary").text.indexOf("Slice") >= 0,
                   "the summary names what the chain ends in")

            // Expanding opens THE SAME rail, in place, not a different view (brief §8).
            mouseClick(collapsed[0])
            wait(0)
            compare(body.expandedChain, 1)
            const openRails = visibleAll(body, "sdChainRail")
            compare(openRails.length, 2, "the second rail opened where the summary was")
            compare(openRails[1].vertical, true)
            compare(findAll(openRails[1], "sdChainNodeDot").length, 5,
                    "with every node the model authored, and no fewer")
            compare(one(collapsed[0], "sdChainCollapsedToggle").text, "CLOSE ▴")

            // 12c's footer keeps the driver and the screen and drops the ranking sentence.
            verify(shown(one(body, "sdDriverName")), "the driver keeps its screen at 396")
            verify(shown(one(body, "sdScreenCtaCompact")))
            verify(!shown(one(body, "sdDriverWhy")))
            compare(one(body, "sdCoverageLine").visible, true,
                    "and the coverage line is stated at the bottom instead")
        }

        // ── review: 13a ──────────────────────────────────────────────────────

        function test_21_reviewStatesItsTenseInTheHeader() {
            setReview(reviewSource(), reviewReadout())

            verify(shown(one(body, "sdReviewBadge")), "the REVIEWING badge is up")
            compare(one(body, "sdReviewNote").text,
                    "final session state · this shot read inside the finished ledger")
            // The count line moves to the right-hand end and says what it counts over — it
            // is a SESSION total, not this shot's, and the panel does not rewind.
            compare(one(body, "sdCadenceNote").text, "4 patterns · counted over all 14 shots")
            verify(!shown(one(body, "sdStageNote")),
                   "so it is not also stated beside the stage chip")

            // ...and the footer says the same thing in words, exactly once.
            const foot = one(body, "sdTenseFooter")
            verify(shown(foot), "the tense footer is up")
            verify(foot.text.indexOf("wide tick") >= 0,
                   "and it names the device the panel is using to point at the shot")
            verify(foot.text.indexOf("not a rate at this n") >= 0)
        }

        function test_22_reviewWithNoShotSelectedIsStillTheFinishedSession() {
            // Brief §6: the panel holds the FINAL state, and selection is what enters
            // shot-reading. Reviewing without a pick is the session's own summary.
            setReview(reviewSource(), null)

            verify(shown(one(body, "sdReviewBadge")), "the tense is still stated")
            verify(!shown(one(body, "sdReviewStrip")),
                   "but there is no shot to read, so no shot strip")
            verify(shown(one(body, "sdBookends")), "the session's bookends hold the slot")

            // ...and picking one swaps that slot, and only that slot.
            probe.readout = reviewReadout()
            wait(0)
            verify(shown(one(body, "sdReviewStrip")), "the reviewed shot takes the slot")
            verify(!shown(one(body, "sdBookends")), "the bookends step aside for it")
            verify(!shown(one(body, "sdThisShotStrip")),
                   "and the live after-shot strip is not in review at all")
        }

        function test_23_reviewShowsEveryConditionAndNeverABlankSlot() {
            setReview(reviewSource(), reviewReadout())

            compare(one(body, "sdReviewHeadline").text,
                    "7 of 9 conditions fired on this swing · 4 of them patterns")
            // The not-assessable measures are stated SEPARATELY rather than folded into the
            // denominator, which would turn "we did not look" into "it was fine".
            compare(one(body, "sdReviewSubline").text,
                    "2 measures not assessable on this capture")

            const cells = visibleAll(body, "sdReviewCell")
            compare(cells.length, 9, "every condition is drawn — nothing is a '+3 more'")

            const marks  = visibleAll(body, "sdReviewCellMark").map(t => t.text)
            const values = visibleAll(body, "sdReviewCellValue").map(t => t.text)
            const bands  = visibleAll(body, "sdReviewCellCorridor").map(t => t.text)
            const tiers  = visibleAll(body, "sdReviewCellTier").map(t => t.text)

            compare(marks.filter(m => m === "OUT").length, 5)
            compare(marks.filter(m => m === "IN").length, 2)
            compare(marks.filter(m => m === "—").length, 2, "the third state has its own mark")

            // Neither slot is EVER blank on a cell the capture could not answer: the value
            // reads "not measurable" and the corridor slot carries the reason.
            compare(values.filter(v => v === "not measurable").length, 2)
            verify(bands.indexOf("shaft occluded at P6") >= 0, "the reason is in the band slot")
            verify(bands.indexOf("ball not tracked") >= 0)
            for (let i = 0; i < bands.length; ++i)
                verify(bands[i] !== "", "cell " + i + " states what it was tested against")

            // The true minus survives the trip: it is what makes the reading line up with
            // the corridor beside it.
            verify(values.indexOf("−4.4°") >= 0, "U+2212, not a hyphen")

            // The session tier sits beside this shot's read, which is the comparison the
            // cell exists to make: was this swing typical of the session or not.
            verify(tiers.indexOf("pattern") >= 0)
            verify(tiers.indexOf("watching") >= 0)
            verify(tiers.indexOf("clean all session") >= 0)

            // Dashed on the two, and only the two, the capture could not answer.
            compare(visibleAll(body, "sdReviewCellDash").length, 2)

            // The design's cell width, at k = 1 on a 1168 panel: five to a row, two rows.
            compare(cells[0].width, 220)
        }

        function test_24_theSelectedShotIsTheWideOutlinedTick() {
            setReview(reviewSource(), reviewReadout())

            const node = nodeById("casting")
            verify(node, "the reviewed session keeps its rail")
            // The node carries a wide form and a 12c slim one, both instantiated; only the
            // arrangement on screen is asked about.
            const run = findAll(node, "sdTick").filter(t => shown(t))
            compare(run.length, 6, "one tick per shot, and the selection adds none")

            const sel = run.filter(t => t.selected)
            compare(sel.length, 1, "exactly one selected tick")
            const other = run.filter(t => !t.selected && !t.notAssessable)[0]

            verify(sel[0].width > other.width, "wider than its neighbours")
            verify(sel[0].height > other.height, "and taller")
            compare(sel[0].border.width, 1, "outlined")
            compare(sel[0].border.color, Theme.colorText,
                    "in colorText, so it survives being drawn over a fired fill")
            // ...and NOT recoloured: the tick's colour already means fired/clean, and a
            // selection that overwrote it would delete the fact it is pointing at.
            compare(sel[0].color, Theme.colorError)
        }

        function test_25_nodesSayWhatHappenedHereAndAfter() {
            setReview(reviewSource(), reviewReadout())

            const node = nodeById("casting")
            compare(one(node, "sdChainNodePill").children[0].text, "FIRED HERE",
                    "the pill is in the review tense")
            compare(one(node, "sdChainNodeRecency").text, "3 more firings after this shot",
                    "and the recency line answers 'was this the end of it, or the middle'")
            // The session counts are untouched by the selection — the panel does not rewind.
            compare(one(node, "sdChainNodeRecurrence").text, "4 of 6 measurable shots")

            const clean = nodeById("shaft_lean")
            compare(one(clean, "sdChainNodePill").children[0].text, "CLEAN HERE")
            compare(one(clean, "sdChainNodeRecency").text, "no firings after this shot")
        }

        // ── the carousel's half (brief §6.1) ─────────────────────────────────

        function test_26_thePipRowIsTheSwingsWholeReadAtCarouselSize() {
            probe.pipFixture = [
                { id: "a", state: "fired" }, { id: "b", state: "clean" },
                { id: "c", state: "notAssessable" }, { id: "d", state: "fired" },
                { id: "e", state: "clean" }, { id: "f", state: "fired" },
                { id: "g", state: "clean" }, { id: "h", state: "notAssessable" },
                { id: "i", state: "fired" }
            ]
            probe.pipFired = 4
            wait(0)

            const pips = findAll(pipProbe, "sdPip")
            compare(pips.length, 9, "one pip per tracked condition, and never a gap")

            const fired = pips.filter(p => p.fired)
            const na    = pips.filter(p => p.notAssessable)
            compare(fired.length, 4)
            compare(na.length, 2)
            compare(fired[0].color, Theme.colorError)
            compare(pips[1].color, Theme.colorGood)

            // Same third state as the tick run: shorter and outlined, never left out — two
            // shots with different unmeasured sets must not read as the same swing.
            verify(na[0].height < fired[0].height, "the unmeasured pip is shorter")
            compare(na[0].border.width, 1, "and outlined rather than filled")

            // The count is the severity, in the app's three grading colours.
            const count = one(pipProbe, "sdPipCount")
            compare(count.text, "4 fired")
            compare(count.color, Theme.colorError)

            probe.pipFired = 0
            wait(0)
            compare(count.color, Theme.colorGood, "a swing that fired nothing says so in green")
        }

        // ── nothing escapes the panel ────────────────────────────────────────
        // The chrome is 1 px of border and a radius; a card or a strip that overhung it
        // would be clipped away rather than drawn, which is a finding gone missing.
        function test_20_nothingOverhangsTheChrome() {
            const sizes = [[1168, 560], [396, 560], [820, 420]]
            const sources = [coldSource(probe.expectationRows), formingSource(false),
                             closingSource(), establishedSource(true), reviewSource()]
            // The review arrangement is the only one with a second input; at 396 its cells
            // stack and the grid SCROLLS, which is what this test is checking has not turned
            // into a cell drawn outside the panel.
            const readouts = [null, null, null, null, reviewReadout()]

            for (let s = 0; s < sources.length; ++s) {
                setReview(sources[s], readouts[s])
                for (let i = 0; i < sizes.length; ++i) {
                    probe.width = sizes[i][0]; probe.height = sizes[i][1]
                    wait(0)

                    // In the 396 arrangement the rail lives in a clipped Flickable and
                    // SCROLLS (12c), so the things inside it are checked through the
                    // Flickable rather than against the panel — a node below the fold there
                    // is one scroll away, not one gone missing.
                    const names = ["sdPatternCard", "sdExpectationCard", "sdThisShotStrip",
                                   "sdBookends", "sdColdBody", "sdCardsBody",
                                   "sdChainsFlick", "sdDriverFooter",
                                   "sdReviewStrip", "sdReviewGrid", "sdTenseFooter"]
                        .concat(body.compact ? []
                                             : ["sdChainRail", "sdChainNode", "sdChainLink"])
                    for (let n = 0; n < names.length; ++n) {
                        const items = visibleAll(body, names[n])
                        for (let j = 0; j < items.length; ++j) {
                            if (!shown(items[j]) || items[j].width <= 0)
                                continue
                            const tl = items[j].mapToItem(body, 0, 0)
                            const br = items[j].mapToItem(body, items[j].width, items[j].height)
                            const label = "source " + s + " at " + sizes[i][0] + "x" + sizes[i][1]
                                        + ": " + names[n] + "[" + j + "]"
                            verify(tl.x >= -1 && tl.y >= -1, label + " starts inside the panel")
                            verify(br.x <= body.width + 1, label + " ends inside the panel")
                            verify(br.y <= body.height + 1, label + " fits the panel's height")
                        }
                    }
                }
            }
        }
    }
}
