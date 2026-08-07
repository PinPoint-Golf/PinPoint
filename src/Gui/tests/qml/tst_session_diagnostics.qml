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

    PpSessionDiagnosticsBody {
        id: body
        anchors.fill: parent
        source: probe.src
    }

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

        function init() {
            probe.width = 1168; probe.height = 560
            Theme.themeIndex = 5          // studio dark, the design's own frame
            Theme.fontScale = 1.0
            body.watchingExpanded = false
            wait(0)
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

        // ── nothing escapes the panel ────────────────────────────────────────
        // The chrome is 1 px of border and a radius; a card or a strip that overhung it
        // would be clipped away rather than drawn, which is a finding gone missing.
        function test_13_nothingOverhangsTheChrome() {
            const sizes = [[1168, 560], [396, 560], [820, 420]]
            const sources = [coldSource(probe.expectationRows), formingSource(false), closingSource()]

            for (let s = 0; s < sources.length; ++s) {
                setSource(sources[s])
                for (let i = 0; i < sizes.length; ++i) {
                    probe.width = sizes[i][0]; probe.height = sizes[i][1]
                    wait(0)

                    const names = ["sdPatternCard", "sdExpectationCard", "sdThisShotStrip",
                                   "sdBookends", "sdColdBody", "sdCardsBody"]
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
