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

// The launch monitor panel's graphics body, loaded for real and measured.
//
// THE ASSERTION THAT EARNS THIS FILE: no annotation escapes its card. Every label on the
// five schematics is positioned from its diagram container's top-left in design pixels,
// so a caption that wrapped, a font that grew with fontScale, or a value one character
// longer than the mock's all push a number over the card's edge — and review caught two
// of exactly that before there was a test to catch them. It is checked at the design
// size, at fontScale 1.25, in both themes, and in the reflowed narrow layout.
//
// This loads the REAL PpLmGraphicsBody. It can, because that component takes a plain map
// and no model: the fixture below is the shape LmSessionModel::buildGraphics() produces,
// so nothing here is a stand-in for the thing under test.
Item {
    id: probe
    width: 1148; height: 516      // a 1168 x 560 stage less the header and the sp(10) body padding

    // One shot, as the model hands it over — the design brief's reference row.
    // A field with no session spread yet — under the three-shot floor, so no region.
    function field(v, text, unit, abbrev, band) {
        return { value: v, text: text, unit: unit, abbrev: abbrev,
                 label: abbrev, bandIndex: band, has: true, mean: "", sd: "",
                 meanValue: v, sdValue: 0, hasSpread: false, n: 1 }
    }

    // A field the session HAS a spread for, which is what puts a shaded region behind
    // its vector.
    function spreadField(v, text, unit, abbrev, band, mean, sd) {
        return { value: v, text: text, unit: unit, abbrev: abbrev,
                 label: abbrev, bandIndex: band, has: true, mean: "", sd: "",
                 meanValue: mean, sdValue: sd, hasSpread: true, n: 18 }
    }

    readonly property var fixture: ({
        leftHanded: false,
        has: true,
        values: {
            "lm.clubheadSpeed":   spreadField(84.5, "84.5", "mph", "CLUB SPEED", 0, 83.9, 2.1),
            "lm.attackAngle":     spreadField(-3.5, "−3.5", "°", "ATTACK ANG.", 0, -3.2, 0.9),
            "lm.clubPath":        spreadField(-1.8, "−1.8", "°", "CLUB PATH", 0, -2.1, 1.4),
            "lm.faceAngle":       spreadField(-1.1, "−1.1", "°", "FACE ANGLE", 0, -0.9, 1.1),
            "lm.faceToPath":      field(1.6,   "1.6",   "°",     "FACE-PATH",   0),
            "lm.dynamicLoft":     spreadField(17.7, "17.7", "°", "DYN. LOFT", 0, 18.1, 1.3),
            "lm.spinLoft":        field(21.2,  "21.2",  "°",     "SPIN LOFT",   0),
            "lm.lieAngle":        field(60.7,  "60.7",  "°",     "LIE ANGLE",   0),
            "lm.closureRate":     field(1840,  "1840",  "°/s",   "CLOSURE",     0),
            "lm.smashFactor":     field(1.40,  "1.40",  "ratio", "SMASH FAC.",  1),
            "lm.strikeLocation":  field(3.0,   "3",     "mm",    "STRIKE LOC.", 1),
            "lm.strikeHeight":    field(-6.0,  "−6",    "mm",    "STRIKE HT.",  1),
            "lm.ballSpeed":       field(118.1, "118.1", "mph",   "BALL SPEED",  2),
            "lm.launchAngle":     spreadField(13.2, "13.2", "°", "LAUNCH ANG.", 2, 13.6, 1.0),
            "lm.launchDirection": spreadField(-1.3, "−1.3", "°", "START DIRECTION", 2, -0.8, 1.2),
            "lm.spinRate":        field(4686,  "4686",  "rpm",   "SPIN RATE",   3),
            "lm.backSpin":        field(4670,  "4670",  "rpm",   "BACK SPIN",   3),
            "lm.sideSpin":        field(384,   "384",   "rpm",   "SIDE SPIN",   3),
            "lm.spinAxis":        spreadField(4.7, "4.7", "°", "SPIN AXIS", 3, 3.9, 2.6),
            "lm.carryDistance":   spreadField(166.6, "166.6", "yd", "CARRY", 4, 164.2, 5.4),
            "lm.totalDistance":   field(181.9, "181.9", "yd",    "TOTAL",       4),
            "lm.offline":         spreadField(5.1, "5.1", "yd", "OFFLINE", 4, 2.8, 4.1),
            "lm.peakHeight":      field(63,    "63",    "ft",    "PEAK HT.",    4),
            "lm.descentAngle":    field(37.3,  "37.3",  "°",     "DESCENT",     4),
        },
        headline: [
            field(118.1, "118.1", "mph",   "BALL SPEED", 2),
            field(84.5,  "84.5",  "mph",   "CLUB SPEED", 0),
            field(1.40,  "1.40",  "ratio", "SMASH FAC.", 1),
            field(166.6, "166.6", "yd",    "CARRY",      4),
            field(181.9, "181.9", "yd",    "TOTAL",      4),
            field(4686,  "4686",  "rpm",   "SPIN RATE",  3),
        ],
        flight: {
            has: true,
            profile: [Qt.point(0, 0), Qt.point(0.29, 0.62), Qt.point(0.58, 1.0),
                      Qt.point(0.78, 0.72), Qt.point(0.9159, 0.0)],
            track:   [Qt.point(0, 0), Qt.point(0.29, -0.10), Qt.point(0.58, 0.24),
                      Qt.point(0.78, 0.58), Qt.point(0.9159, 0.889)],
            landing: { x: 0.9159, y: 0.0, z: 0.889 },
            finish:  { x: 1.0,    y: 0.0, z: 1.0 },
            launchTangent:  { x: 0.45, y: 0.89, z: 0 },
            landingTangent: { x: 0.15, y: -0.99, z: 0 },
            carryFraction: 0.9159, apexAtX: 0.5814,
            lateralExtentYd: 5.73, carryYd: 166.6, totalYd: 181.9,
            apexFt: 63.0, offlineYd: 5.1, residualOfflineYd: 3.47,
        },
        strikeEllipse: { has: true, n: 18, meanX: 1.2, meanY: -2.4,
                         majorSd: 5.1, minorSd: 2.2, tiltDeg: 28.0 },
        // No device reported low point, so this one is PinPoint's own optical estimate —
        // the case the IMPACT card has to badge rather than print bare.
        lowPoint: { has: true, value: 3.1, text: "3.1", unit: "in", source: "inferred" },
        shape:  { has: true, name: "Pull–fade", windowIdx: 0, curveIdx: 2,
                  evidence: "start 1.3° left · axis 4.7° right · curved 8.9 yd right" },
        strike: { has: true, name: "Low", evidence: "smash +0.01 vs μ" },
    })

    // An empty map: the state the panel is in before a monitor has said anything. Every
    // accessor in the body has to survive it, because a component that only works once
    // it has data is a component that crashes the first time a golfer opens the panel.
    readonly property var emptyFixture: ({})

    PpLmGraphicsBody {
        id: body
        anchors.fill: parent
        g: probe.fixture
    }

    TestCase {
        id: tc
        name: "LmGraphics"
        when: windowShown

        // Every item tagged "anno" under `item`, however deeply nested.
        function annotations(item, acc) {
            acc = acc || []
            if (!item || !item.children) return acc
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.objectName === "anno") acc.push(c)
                annotations(c, acc)
            }
            return acc
        }

        function cards(item, acc) {
            acc = acc || []
            if (!item || !item.children) return acc
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.objectName === "lmCard") acc.push(c)
                cards(c, acc)
            }
            return acc
        }

        // ONE CARD, BY NAME AND NOT BY INDEX. A Repeater whose model is replaced rebuilds
        // its delegates, and the order they land in the parent's `children` array is not
        // promised to match the model's — so an index-based lookup silently starts
        // measuring the card next door. It did: mirroring the fixture re-evaluated the
        // card list (the STRIKE caption names the handed side) and the assertion was then
        // comparing a mark on one card against the same mark mapped into another's frame.
        function cardTitled(t) {
            const cs = cards(body)
            for (let i = 0; i < cs.length; ++i)
                if (cs[i].title === t) return cs[i]
            return null
        }

        // Is `anno` fully inside the card that contains it? Measured in the CARD's own
        // coordinates, which is the frame a reader sees the edge of.
        function escapes(card, anno) {
            const tl = anno.mapToItem(card, 0, 0)
            const br = anno.mapToItem(card, anno.width, anno.height)
            // One pixel of slack: a rounded half-pixel from the scale is not an escape.
            const tol = 1.0
            return tl.x < -tol || tl.y < -tol
                || br.x > card.width + tol || br.y > card.height + tol
        }

        function checkContainment(label) {
            const cs = cards(body)
            verify(cs.length === 5, label + ": five cards, got " + cs.length)
            let checked = 0
            for (let i = 0; i < cs.length; ++i) {
                const as = annotations(cs[i])
                for (let j = 0; j < as.length; ++j) {
                    if (!as[j].visible || as[j].width <= 0) continue
                    ++checked
                    verify(!escapes(cs[i], as[j]),
                           label + ": annotation " + j + " escapes card " + i
                           + " (" + Math.round(as[j].mapToItem(cs[i], 0, 0).x) + ","
                           + Math.round(as[j].mapToItem(cs[i], 0, 0).y) + " "
                           + Math.round(as[j].width) + "x" + Math.round(as[j].height)
                           + " in " + Math.round(cs[i].width) + "x"
                           + Math.round(cs[i].height) + ")")
                }
            }
            // A containment check that found nothing to check would pass forever. The
            // floor is well below the ~24 actually drawn: it exists to catch a walk that
            // stopped finding things, not to pin the count, so removing a label is not
            // supposed to fail here.
            verify(checked >= 15, label + ": expected 15+ annotations, saw " + checked)
        }

        // ── the design size ─────────────────────────────────────────────────
        function test_01_widthLayoutAtDesignSize() {
            probe.width = 1148; probe.height = 516
            wait(0)
            compare(body.reflow, false, "the design size is the wide composition")
            compare(cards(body).length, 5, "five cards")
            // The design's proportions, as widths rather than as one scale factor: the
            // composition is a layout now, and this is what says the weights still decide
            // the share. PATH & FACE is 696/1148 of the top row, SPIN 444/1148.
            const ratio = cardTitled("Path & face").width / cardTitled("Spin").width
            fuzzyCompare(ratio, 696 / 444, 0.08,
                         "the top row keeps the design's proportions, got " + ratio)
            checkContainment("design size")
        }

        // ── fontScale 1.25 ──────────────────────────────────────────────────
        // Type on these cards comes from Theme.fontSzMicro, which carries fontScale,
        // while the geometry is pinned to the container. So a larger fontScale is
        // exactly the case where a label outgrows the space reserved for it.
        //
        // SET ON Theme, NOT ON appSettings. The arrow runs Main.qml → Theme.fontScale, and
        // Main.qml is not in this harness — so assigning appSettings here changed nothing
        // and this test had been asserting the default size twice. It matters more now
        // than it did: fixed type means fontScale is what decides how much room the chrome
        // needs, and therefore when the composition gives way to a column.
        function test_02_containsAtLargerFontScale() {
            const was = Theme.fontScale
            Theme.fontScale = 1.25
            wait(0)
            checkContainment("fontScale 1.25")
            Theme.fontScale = was
            wait(0)
        }

        // ── both themes ─────────────────────────────────────────────────────
        // Geometry should not depend on the theme, and this is what says so: if a token
        // ever picks up a size that differs light-to-dark, the containment breaks here
        // rather than in a screenshot someone happens to look at.
        function test_03_containsInBothThemes() {
            const was = appSettings.themeIndex
            for (let t = 0; t < 2; ++t) {
                appSettings.themeIndex = t
                wait(0)
                checkContainment("theme " + t)
            }
            appSettings.themeIndex = was
            wait(0)
        }

        // ── narrow: reflow, never a sideways scroll ─────────────────────────
        function test_04_reflowsWhenNarrow() {
            probe.width = 700; probe.height = 500
            wait(0)
            verify(probe.width < body.kMinWideW, "700 px is below the wide composition's floor")
            compare(body.reflow, true, "and the cards stack")

            const cs = cards(body)
            compare(cs.length, 5, "all five cards survive the reflow")
            for (let i = 0; i < cs.length; ++i) {
                verify(cs[i].width <= probe.width + 1,
                       "card " + i + " fits the width it was given")
                // The whole point of reflowing rather than shrinking on: in a column
                // each card is WIDER than its slot in the composition was.
                verify(cs[i].width > 0, "card " + i + " has a width")
            }
            checkContainment("reflowed")
        }

        function test_05_neverScrollsSideways() {
            probe.width = 500; probe.height = 400
            wait(0)
            compare(body.reflow, true, "500 px reflows")
            // Find the Flickable the column lives in and assert it cannot pan sideways.
            let flick = null
            function findFlick(item) {
                if (!item || !item.children) return
                for (let i = 0; i < item.children.length; ++i) {
                    const c = item.children[i]
                    if (c.contentWidth !== undefined && c.contentHeight !== undefined
                        && c.visible)
                        flick = c
                    findFlick(c)
                }
            }
            findFlick(body)
            verify(flick !== null, "the reflowed column is flickable")
            verify(flick.contentWidth <= flick.width + 1,
                   "content never exceeds the width, so there is nothing to pan to")
        }

        // ── the degenerate case ─────────────────────────────────────────────
        function test_06_survivesAnEmptyShot() {
            probe.width = 1148; probe.height = 516
            body.g = probe.emptyFixture
            wait(0)
            compare(body.reflow, false, "an empty map still lays out")
            const cs = cards(body)
            compare(cs.length, 5, "and still draws its five cards")
            // Nothing was reported, so nothing is claimed: no flight, no inferred reads.
            compare(body.flight.has, false, "no launch conditions, no flight curve")
            compare(body.shape.has, false, "no readings, no inferred flight shape")
            compare(body.strike.has, false, "no readings, no inferred strike")
            checkContainment("empty shot")
            body.g = probe.fixture
            wait(0)
        }

        // ── the mirror ──────────────────────────────────────────────────────
        // Toe left for a right-hander, toe right for a left-hander. This orientation was
        // wrong once in review, so it is asserted on the drawn position rather than on
        // the sign that produces it.
        function test_07_strikeMarkerMirrors() {
            probe.width = 1148; probe.height = 516
            body.g = probe.fixture
            wait(0)
            const right = strikeMarkerX()
            verify(right !== null, "the strike marker is drawn")

            const mirrored = Object.assign({}, probe.fixture, { leftHanded: true })
            body.g = mirrored
            wait(0)
            compare(body.leftHanded, true, "the body reads the handedness")
            const left = strikeMarkerX()
            verify(left !== null, "the mirrored marker is drawn")

            // 3 mm toward the toe: left of the crosshair for a right-hander, right of it
            // for a left-hander. Equal and opposite about the same centre.
            verify(right < left,
                   "a toe strike sits further left for a right-hander (" + right
                   + " vs " + left + ")")

            body.g = probe.fixture
            wait(0)
        }

        // ── the session's spread, shaded behind the shot ────────────────────
        // Every PpLmSpread under `item`, however deeply nested.
        function spreads(item, acc) {
            acc = acc || []
            if (!item || !item.children) return acc
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.restAlpha !== undefined && c.liveAlpha !== undefined) acc.push(c)
                spreads(c, acc)
            }
            return acc
        }

        function test_08_regionsAreSubtleAndBehind() {
            probe.width = 1148; probe.height = 516
            body.g = probe.fixture
            body.hoveredKey = ""
            wait(0)

            const rs = spreads(body).filter(r => r.visible)
            verify(rs.length >= 7, "the vectors with a session spread are shaded, saw " + rs.length)

            for (let i = 0; i < rs.length; ++i) {
                // Subtle: at rest the fill is a wash, not a colour. Checked on the alpha
                // rather than on a screenshot so it cannot drift unnoticed.
                verify(rs[i]._fill.a <= 0.09,
                       "region " + i + " is faint at rest (" + rs[i]._fill.a + ")")
                // ...but NOT invisible. This panel's stated home is a bay TV read from
                // across a hitting bay, where no pointer will ever hover: a region that
                // only exists on hover would not exist at all in the place it is for.
                verify(rs[i]._fill.a >= 0.03,
                       "region " + i + " is still visible at rest (" + rs[i]._fill.a + ")")
            }

            // Behind. Within a diagram, stacking is declaration order, so every region
            // must sit at a lower index than every line and label around it.
            const cs = cards(body)
            for (let c = 0; c < cs.length; ++c) {
                const mine = spreads(cs[c])
                if (mine.length === 0) continue
                const frame = mine[0].parent
                let lastRegion = -1, firstOther = frame.children.length
                for (let i = 0; i < frame.children.length; ++i) {
                    const ch = frame.children[i]
                    const isRegion = ch.restAlpha !== undefined
                    if (isRegion) lastRegion = Math.max(lastRegion, i)
                    else firstOther = Math.min(firstOther, i)
                }
                verify(lastRegion < firstOther,
                       "card " + c + ": every region stacks behind every mark")
            }
        }

        function test_09_hoverLiftsOneRegionOnly() {
            probe.width = 1148; probe.height = 516
            body.g = probe.fixture
            body.hoveredKey = ""
            wait(0)

            const rs = spreads(body).filter(r => r.visible)
            const rest = rs.map(r => r._fill.a)

            // Drive it through hoveredKey rather than a synthesised pointer: what is
            // being asserted is that ONE region answers a question about ONE metric,
            // which is the contract the HoverHandler feeds and not the handler itself.
            body.hoveredKey = "lm.clubPath"
            wait(Theme.durationFast + 60)

            let lifted = 0
            for (let i = 0; i < rs.length; ++i) {
                if (rs[i].active) {
                    ++lifted
                    verify(rs[i]._fill.a > rest[i],
                           "the asked-about region comes forward")
                    verify(rs[i]._fill.a <= 0.2,
                           "…without becoming a corridor (" + rs[i]._fill.a + ")")
                }
            }
            compare(lifted, 1, "exactly one region answers")

            body.hoveredKey = ""
            wait(Theme.durationFast + 60)
            for (let i = 0; i < rs.length; ++i)
                verify(!rs[i].active, "and they all settle back")
        }

        // Under three shots there is no spread to shade — the same floor that hides the
        // tiles board's dispersion strip, reaching the schematics.
        function test_10_noRegionBelowTheSpreadFloor() {
            probe.width = 1148; probe.height = 516
            const thin = JSON.parse(JSON.stringify({}))
            const noSpread = Object.assign({}, probe.fixture, {
                values: Object.assign({}, probe.fixture.values, {
                    "lm.clubPath": probe.field(-1.8, "−1.8", "°", "CLUB PATH", 0)
                }),
                strikeEllipse: { has: false }
            })
            body.g = noSpread
            wait(0)

            const rs = spreads(body)
            let clubPathShaded = false
            for (let i = 0; i < rs.length; ++i)
                if (rs[i].visible && rs[i].hue === body.hueClub && rs[i].radius === 170)
                    clubPathShaded = true
            verify(!clubPathShaded, "a field under the three-shot floor is not shaded")

            body.g = probe.fixture
            wait(0)
        }

        // ── hover, through the actual pointer ───────────────────────────────
        // test_09 drives hoveredKey directly, which asserts what the regions do with an
        // answer but NOT that anything ever produces one. This moves a real pointer over
        // a real label, which is the path a golfer takes.
        function test_11_pointingAtALabelAsksTheQuestion() {
            probe.width = 1148; probe.height = 516
            body.g = probe.fixture
            body.hoveredKey = ""
            wait(0)

            // The CLUB PATH annotation on the PATH & FACE card.
            const target = annotations(cardTitled("Path & face")).find(a => a.metricKey === "lm.clubPath")
            verify(target !== undefined, "the club path label is a hover target")
            verify(target.width > 0 && target.height > 0, "…with a real area to point at")

            const p = target.mapToItem(probe, target.width / 2, target.height / 2)
            mouseMove(probe, p.x, p.y)
            wait(60)
            compare(body.hoveredKey, "lm.clubPath",
                    "pointing at the label asks about that metric")

            // And leaving it puts the question down again.
            mouseMove(probe, 2, probe.height - 2)
            wait(60)
            compare(body.hoveredKey, "", "leaving the label clears it")
        }

        // ── pointing at the SHADING, which is the gesture people make ────────
        // The label is not the only target and never was the natural one: shown the
        // panel, the first thing a reader does is point at the shaded region itself.
        // test_11 covers the label; this covers the region, and it is the one that was
        // missing when the feature was first put in front of someone.
        function test_12_pointingAtTheShadingAsksTheQuestion() {
            probe.width = 1148; probe.height = 516
            body.g = probe.fixture
            body.hoveredKey = ""
            wait(0)

            const region = spreads(body).find(r => r.visible && r.metricKey === "lm.launchDirection")
            verify(region !== undefined, "the start direction spread is on screen")

            // Out along the wedge's own centreline, well clear of the pivot — where the
            // sector is widest and where a reader would actually point.
            const a = region.meanAngle * Math.PI / 180
            const rr = region.radius * region.s * 0.7
            const p = region.mapToItem(probe,
                                       region.pivotX * region.s + rr * Math.cos(a),
                                       region.pivotY * region.s + rr * Math.sin(a))
            mouseMove(probe, p.x, p.y)
            wait(60)
            compare(body.hoveredKey, "lm.launchDirection",
                    "pointing at the shading asks about that metric")
            verify(region.active, "…and that region is the one that answers")

            mouseMove(probe, 2, probe.height - 2)
            wait(60)
            compare(body.hoveredKey, "", "leaving it clears the question")
        }

        // Every region must be pointable, including the ones only a few pixels wide.
        // The 1° launch-angle wedge is 7 px across at its widest and the dynamic-loft one
        // is 3 px: without a floor on the TARGET (never on the drawing) they are shaded
        // for the eye and unreachable by the hand.
        function test_13_evenTheThinnestRegionIsPointable() {
            probe.width = 1148; probe.height = 516
            body.g = probe.fixture
            body.hoveredKey = ""
            wait(0)

            for (const key of ["lm.dynamicLoft", "lm.launchAngle", "lm.faceAngle"]) {
                const region = spreads(body).find(r => r.visible && r.metricKey === key)
                verify(region !== undefined, key + " is shaded")

                const a = region.meanAngle * Math.PI / 180
                const rr = region.radius * region.s * 0.6
                const p = region.mapToItem(probe,
                                           region.pivotX * region.s + rr * Math.cos(a),
                                           region.pivotY * region.s + rr * Math.sin(a))
                mouseMove(probe, p.x, p.y)
                wait(60)
                compare(body.hoveredKey, key, "a hairline region is still pointable: " + key)

                mouseMove(probe, 2, probe.height - 2)
                wait(60)
            }
        }

        // ── type is fixed; drawings are not ─────────────────────────────────
        // Every Text under `item`, however deeply nested.
        function texts(item, acc) {
            acc = acc || []
            if (!item || !item.children) return acc
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.font !== undefined && c.text !== undefined) acc.push(c)
                texts(c, acc)
            }
            return acc
        }

        // The DISTINCT pixel sizes in use across every visible reading on the board.
        //
        // A set, not a tally. An earlier version compared sorted multisets and broke the
        // moment a card changed how many readings it drew — which is a layout decision and
        // none of this test's business. What is this test's business is that the sizes
        // themselves do not move. Invisible items are skipped: the bands carry hidden
        // sizers to measure themselves against, and their text is not on screen.
        function readTypeSizes() {
            const cs = cards(body)
            const seen = []
            for (let i = 0; i < cs.length; ++i) {
                const as = annotations(cs[i])
                for (let j = 0; j < as.length; ++j) {
                    if (!as[j].visible) continue
                    const ts = texts(as[j])
                    for (let k = 0; k < ts.length; ++k) {
                        if (!ts[k].visible || ts[k].text === "") continue
                        const px = ts[k].font.pixelSize
                        if (seen.indexOf(px) < 0) seen.push(px)
                    }
                }
            }
            return seen.sort(function (a, b) { return a - b }).join(",")
        }

        // THE ASSERTION THIS WHOLE REWRITE EXISTS FOR. The graphics view used to multiply
        // one fit factor into every font size, so the same label was a different size in a
        // stage split and on a bay TV — it read as a different application at either end.
        // Type is now Theme tokens throughout, and this is what says so: the identical set
        // of sizes at a frame size where the drawings differ by nearly 2×.
        function test_14_typeDoesNotScaleWithTheFrame() {
            probe.width = 1148; probe.height = 516
            wait(0)
            compare(body.reflow, false, "the design size is the composition")
            const wide = readTypeSizes()
            verify(wide.length > 0, "there are readings to measure")

            probe.width = 700; probe.height = 500
            wait(0)
            compare(body.reflow, true, "700 px reflows")
            compare(readTypeSizes(), wide,
                    "the readings use the same sizes in both layouts")

            // …and those sizes are the app's tokens, not numbers of this view's own.
            const distinct = wide.split(",").map(Number)
            for (let i = 0; i < distinct.length; ++i)
                verify(distinct[i] === Theme.fontSzMicro || distinct[i] === Theme.fontSzData,
                       "size " + distinct[i] + " is a Theme token")

            probe.width = 1148; probe.height = 516
            wait(0)
        }

        // Every scaled design frame under `item`.
        function frames(item, acc) {
            acc = acc || []
            if (!item || !item.children) return acc
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.objectName === "diagramFrame") acc.push(c)
                frames(c, acc)
            }
            return acc
        }

        // One drawing, by the title of the card it is in.
        function frameOf(title) {
            const card = cardTitled(title)
            if (card === null) return null
            const fs = frames(card)
            return fs.length > 0 ? fs[0] : null
        }

        // The other half of the same change: the geometry DOES answer to the frame.
        // Reflowing to a column gives every card the full width, so a drawing there is at
        // least as big as its slot in the composition was — never smaller. That is the
        // point of reflowing rather than shrinking on, and it is only true because the
        // chrome around the drawing stopped growing with it.
        //
        // NOT "strictly bigger for all five". PATH & FACE already takes 696 of the design's
        // 1148, so a 700 px column is the width it nearly had — the two come out level to
        // within half a percent, and demanding an increase there would assert arithmetic
        // rather than behaviour. The gain belongs to the NARROW cards, and STRIKE, the
        // narrowest at 248, is where it has to show up. Modestly: a card in the column is
        // capped at sp(200) of drawing height, so the squarest schematic gains from the
        // extra width only as far as that cap lets it.
        function test_15_drawingsScaleWithTheirCard() {
            const titles = ["Path & face", "Spin", "Impact", "Strike", "Flight"]

            probe.width = 1148; probe.height = 516
            wait(0)
            const wide = {}
            for (let i = 0; i < titles.length; ++i) {
                const f = frameOf(titles[i])
                verify(f !== null && f.width > 0, titles[i] + " has a drawing")
                wide[titles[i]] = f.width
            }

            probe.width = 700; probe.height = 500
            wait(0)
            for (let j = 0; j < titles.length; ++j) {
                const f = frameOf(titles[j])
                verify(f !== null, titles[j] + " survives the reflow")
                // Half a percent of slack: the two layouts round independently, and a card
                // that is a wash between them must not read as a shrink.
                verify(f.width >= wide[titles[j]] * 0.99,
                       titles[j] + " is not smaller in the column (" + Math.round(f.width)
                       + " vs " + Math.round(wide[titles[j]]) + ")")
            }

            const strike = frameOf("Strike")
            verify(strike.width > wide["Strike"],
                   "the narrowest card gains from the column ("
                   + Math.round(strike.width) + " vs " + Math.round(wide["Strike"]) + ")")

            probe.width = 1148; probe.height = 516
            wait(0)
        }

        // Bigger type needs more room, so the same frame that composes at normal size must
        // reflow at a larger one. The old scale-based floor could not express this: it
        // asked how small the drawing would have to be, which fontScale never touched.
        function test_16_largerTypeReflowsEarlier() {
            const was = Theme.fontScale
            probe.width = 900; probe.height = 520
            Theme.fontScale = 1.0
            wait(0)
            compare(body.reflow, false, "900 × 520 composes at normal type")

            Theme.fontScale = 1.5
            wait(0)
            compare(body.reflow, true, "…and stacks once the type is half again bigger")

            Theme.fontScale = was
            probe.width = 1148; probe.height = 516
            wait(0)
        }

        // ── low point, and whose number it is ───────────────────────────────
        // The fixture's low point is PinPoint's optical estimate, not a device reading. An
        // estimate printed as a measurement is the one failure the `lm.` namespace exists
        // to prevent, so the card must say which it is showing.
        function test_17_lowPointIsDrawnAndBadgedWhenEstimated() {
            probe.width = 1148; probe.height = 516
            body.g = probe.fixture
            wait(0)

            const impact = cardTitled("Impact")
            verify(impact !== null, "the IMPACT card is on the view")
            const labels = annotations(impact).map(function (a) {
                return texts(a).map(function (t) { return t.text }).join(" ") })
            const low = labels.find(function (l) { return l.indexOf("LOW POINT") >= 0 })
            verify(low !== undefined, "the IMPACT card reports low point, saw " + labels)
            verify(low.indexOf("3.1") >= 0, "…with the value, got " + low)
            verify(low.indexOf("EST.") >= 0, "…badged as an estimate, got " + low)

            // The same reading, measured this time: no badge.
            const measured = Object.assign({}, probe.fixture, {
                lowPoint: { has: true, value: 3.1, text: "3.1", unit: "in",
                            source: "measured" } })
            body.g = measured
            wait(0)
            const after = annotations(cardTitled("Impact")).map(function (a) {
                return texts(a).map(function (t) { return t.text }).join(" ") })
            const lowMeasured = after.find(function (l) { return l.indexOf("LOW POINT") >= 0 })
            verify(lowMeasured !== undefined, "still reported when the device measured it")
            verify(lowMeasured.indexOf("EST.") < 0,
                   "…and NOT badged, got " + lowMeasured)

            body.g = probe.fixture
            wait(0)
            checkContainment("low point")
        }

        // ── where a reading lives, and what decides it ──────────────────────
        // The strip is the FALLBACK. A reading belongs beside the line it names, and only
        // moves into a row underneath when the drawing is too small to hold it — so the
        // test is on the DRAWING's size, not on which layout is showing.
        function test_18_readingsStayOnADrawingBigEnoughToHoldThem() {
            probe.width = 1148; probe.height = 516
            body.g = probe.fixture
            wait(0)

            const pf = cardTitled("Path & face")
            verify(pf !== null, "the path & face card is on the view")
            verify(pf.hosted, "at the design size the drawing carries its own readings"
                              + " (scale " + pf.hostScale.toFixed(3) + ")")
            compare(pf.reads.length, 0, "…so the strip under it is empty")
            const onDrawing = annotations(pf).filter(function (a) {
                return a.visible && a.width > 0 })
            verify(onDrawing.length >= 6,
                   "…and the readings are on the drawing, saw " + onDrawing.length)
            checkContainment("hosted on the drawing")

            // Small enough that the drawing can no longer hold them.
            probe.width = 600; probe.height = 420
            wait(0)
            const small = cardTitled("Path & face")
            verify(!small.hosted, "a small card cannot host them"
                                  + " (scale " + small.hostScale.toFixed(3) + ")")
            verify(small.reads.length >= 6,
                   "…so they fall back to the strip, saw " + small.reads.length)
            checkContainment("fallen back to the strip")

            probe.width = 1148; probe.height = 516
            wait(0)
        }

        // ── the seam between the two columns ────────────────────────────────
        // SPIN sits above FLIGHT and PATH & FACE above IMPACT + STRIKE, so the board has
        // one vertical seam down it. The brief's own figures did NOT give it one — SPIN was
        // 704…1148 against FLIGHT's 696…1148 — and eight pixels of step at full screen is
        // exactly the kind of thing that reads as sloppy without being nameable. The
        // composition splits both rows on one pair of weights now; this is what says so.
        function test_19_theTwoRowsShareOneColumnSeam() {
            probe.width = 1148; probe.height = 516
            wait(0)
            compare(body.reflow, false, "the composition is showing")

            const pf     = cardTitled("Path & face")
            const spin   = cardTitled("Spin")
            const impact = cardTitled("Impact")
            const strike = cardTitled("Strike")
            const flight = cardTitled("Flight")
            verify(pf && spin && impact && strike && flight, "all five cards are present")

            // The right column: same width, same left edge, in both rows.
            fuzzyCompare(spin.width, flight.width, 1.0,
                         "SPIN and FLIGHT are the same width ("
                         + Math.round(spin.width) + " vs " + Math.round(flight.width) + ")")
            fuzzyCompare(spin.mapToItem(body, 0, 0).x, flight.mapToItem(body, 0, 0).x, 1.0,
                         "…and begin at the same x")

            // The left column: PATH & FACE ends where STRIKE ends.
            fuzzyCompare(pf.mapToItem(body, pf.width, 0).x,
                         strike.mapToItem(body, strike.width, 0).x, 1.0,
                         "the left column ends at the same x in both rows")

            // And IMPACT starts where PATH & FACE does, so the outer edges agree too.
            fuzzyCompare(pf.mapToItem(body, 0, 0).x, impact.mapToItem(body, 0, 0).x, 1.0,
                         "both rows start at the same x")
        }

        // The strike marker's centre, in the STRIKE card's coordinates.
        function strikeMarkerX() {
            const card = cardTitled("Strike")
            if (card === null) return null
            let found = null
            function walk(item) {
                if (!item || !item.children) return
                for (let i = 0; i < item.children.length; ++i) {
                    const c = item.children[i]
                    // The marker is the only Item on that card carrying `mx`.
                    if (c.mx !== undefined && c.visible) found = c
                    walk(c)
                }
            }
            walk(card)
            return found ? found.mapToItem(card, 0, 0).x : null
        }
    }
}
