// The two reference registries a characteristic points at: the physical screens behind `screenRef`
// and the drills behind `drills`.
//
// Both joins are EXACT STRING MATCHES into a separate file, which is the failure mode worth a test
// of its own: a typo or a renamed row does not throw, does not warn at load, and does not stop the
// condition rendering. It just leaves the panel that was meant to tell a coach how to run the test
// blank, with nothing anywhere to say why. So this gates the shipped content in BOTH directions and
// each validator in both — firing when it should, and silent when it should not, since half the
// value of a validator test is the negative case.
//
//   cmake --build build/analyzer-tests --target reference_sets_test
//   ctest --test-dir build/analyzer-tests -R reference_sets --output-on-failure

#include "../characteristic_pack.h"
#include "../drill_pack.h"
#include "../reference_pack.h"
#include "../screen_pack.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static bool hasCode(const ValidationReport &r, const char *code)
{
    return !r.withCode(QString::fromLatin1(code)).empty();
}

static CharacteristicPack shippedPack()
{
    QFile f(QString::fromLocal8Bit(qgetenv("PINPOINT_CORE_PACK")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return loadPack(f.readAll(), QStringLiteral("core")).pack;
}

int main()
{
    std::printf("=== the shipped registries load ===\n");
    const ScreenSet    &screens = sharedScreenSet();
    const DrillSet     &drills  = sharedDrillSet();
    const ReferenceSet &refs    = sharedReferenceSet();

    check(!screens.screens.empty(), "the screen set loaded");
    check(!drills.drills.empty(), "the drill set loaded");
    check(validateScreenSet(screens).ok(), "the shipped screen set has no errors");
    check(validateDrillSet(drills).ok(), "the shipped drill set has no errors");
    std::printf("      (%d screens, %d drills)\n",
                int(screens.screens.size()), int(drills.drills.size()));

    std::printf("=== every reference resolves ===\n");
    {
        const CharacteristicPack p = shippedPack();
        check(!p.conditions.empty(), "the shipped pack loaded");

        int danglingScreens = 0, danglingDrills = 0, withScreen = 0, withDrill = 0;
        for (const Condition &c : p.conditions) {
            if (!c.screenRef.isEmpty()) {
                ++withScreen;
                if (!screens.screen(c.screenRef)) {
                    ++danglingScreens;
                    std::printf("        '%s' -> unknown screen '%s'\n",
                                qPrintable(c.id), qPrintable(c.screenRef));
                }
            }
            for (const QString &d : c.drills) {
                ++withDrill;
                if (!drills.drill(d)) {
                    ++danglingDrills;
                    std::printf("        '%s' -> unknown drill '%s'\n",
                                qPrintable(c.id), qPrintable(d));
                }
            }
        }
        check(danglingScreens == 0, "every screenRef in the pack names a real screen");
        check(danglingDrills == 0, "every drill reference in the pack names a real drill");
        check(withScreen > 0, "…and screens are actually referenced, so the check can fail");
        check(withDrill > 0, "…and so are drills");
        std::printf("        (%d screen references, %d drill references)\n", withScreen, withDrill);
    }

    std::printf("=== no branded screening system reached the content ===\n");
    {
        // The same rule the pack is held to, checked against the raw bytes so a note or a stray
        // field cannot slip past. The MOVEMENTS are common property of the clinical literature; the
        // packaging of them into a named system is somebody's product and must not enter the repo
        // in any form — not a citation, not an author, not an explanatory aside.
        const char *forbidden[] = { "titleist", "tpi", "trackman", "flightscope",
                                    "performance institute", "hackmotion", "k-vest", "kvest" };
        int hits = 0;
        for (const QByteArray &blob : { saveScreenSet(screens), saveDrillSet(drills) }) {
            const QString lower = QString::fromUtf8(blob).toLower();
            for (const char *needle : forbidden)
                if (lower.contains(QLatin1String(needle))) {
                    ++hits;
                    std::printf("        brand token '%s'\n", needle);
                }
        }
        check(hits == 0, "no commercial screening system is named anywhere in the registries");
    }

    std::printf("=== a screen says how to run it and what passing looks like ===\n");
    {
        int noProtocol = 0, noPass = 0, numericNoUnit = 0;
        for (const Screen &s : screens.screens) {
            if (s.protocol.trimmed().isEmpty()) ++noProtocol;
            if (s.passCriterion.trimmed().isEmpty()) ++noPass;
            if (s.passAtLeast.has_value() && s.unit.isEmpty()) ++numericNoUnit;
        }
        check(noProtocol == 0, "every shipped screen states a protocol");
        check(noPass == 0, "every shipped screen states what passing looks like");
        check(numericNoUnit == 0, "every numeric pass floor carries its unit");

        // Several screens are deliberately QUALITATIVE — "achieves neutral extension", "smooth
        // independent rotation". Inventing a number for those would read as more certain than the
        // words, so the absence is intentional and asserted rather than tolerated.
        const int qualitative = int(std::count_if(
            screens.screens.begin(), screens.screens.end(),
            [](const Screen &s) { return !s.passAtLeast.has_value(); }));
        check(qualitative > 0, "some screens are qualitative on purpose, not merely unfinished");
    }

    std::printf("=== a drill says what to do and what it is for ===\n");
    {
        int noInstruction = 0, noTarget = 0;
        for (const Drill &d : drills.drills) {
            if (d.instruction.trimmed().isEmpty()) ++noInstruction;
            if (d.targets.trimmed().isEmpty()) ++noTarget;
        }
        check(noInstruction == 0, "every shipped drill says what the golfer does");
        check(noTarget == 0, "every shipped drill says what it is trying to change");
    }

    std::printf("=== the validators fire, and stay quiet ===\n");
    {
        ScreenSet s;
        s.screens.push_back(Screen{ QStringLiteral("screen.a"), QStringLiteral("A"), {},
                                    QStringLiteral("do it"), QStringLiteral("pass"), {}, {}, {}, {} });
        check(validateScreenSet(s).ok(), "a well-formed screen set passes");

        ScreenSet dup = s;
        dup.screens.push_back(dup.screens.front());
        check(hasCode(validateScreenSet(dup), "duplicateId"), "a duplicate screen id is refused");

        ScreenSet ns = s;
        ns.screens.front().id = QStringLiteral("a");
        check(hasCode(validateScreenSet(ns), "screenIdNamespace"),
              "an id outside the screen. namespace is refused — nothing could point at it");

        ScreenSet nu = s;
        nu.screens.front().passAtLeast = 30.0;
        check(hasCode(validateScreenSet(nu), "screenUnitMissing"),
              "a numeric pass floor with no unit is refused");

        ScreenSet np = s;
        np.screens.front().protocol.clear();
        check(hasCode(validateScreenSet(np), "screenNoProtocol"), "a screen nobody could run warns");

        DrillSet d;
        d.drills.push_back(Drill{ QStringLiteral("drill.a"), QStringLiteral("A"),
                                  QStringLiteral("do it"), QStringLiteral("for this"), {}, {} });
        check(validateDrillSet(d).ok(), "a well-formed drill set passes");

        DrillSet dn = d;
        dn.drills.front().id = QStringLiteral("a");
        check(hasCode(validateDrillSet(dn), "drillIdNamespace"),
              "a drill id outside the drill. namespace is refused");

        DrillSet dt = d;
        dt.drills.front().targets.clear();
        check(hasCode(validateDrillSet(dt), "drillNoTarget"),
              "a drill that does not say what it changes warns");
    }

    std::printf("=== round-trip ===\n");
    {
        const ScreenLoadResult rs = loadScreenSet(saveScreenSet(screens), QStringLiteral("rt"));
        check(rs.loaded && rs.pack.screens.size() == screens.screens.size(),
              "a screen set survives save and load");
        const Screen *before = screens.screen(QStringLiteral("screen.ankleDorsiflexion"));
        const Screen *after  = rs.pack.screen(QStringLiteral("screen.ankleDorsiflexion"));
        check(before && after && after->passAtLeast.has_value()
                  && *after->passAtLeast == *before->passAtLeast && after->unit == before->unit,
              "…including the optional numeric floor and its unit");

        const DrillLoadResult rd = loadDrillSet(saveDrillSet(drills), QStringLiteral("rt"));
        check(rd.loaded && rd.pack.drills.size() == drills.drills.size(),
              "a drill set survives save and load");
    }

    // ── The schema gate ─────────────────────────────────────────────────────
    //
    // All three registries parsed a `schemaVersion` and then ignored what it said, which is the
    // worst of the three possible behaviours: a set from a newer build was read as far as this one
    // understood it, and the keys it had never heard of were dropped in silence. Round-trip that
    // through an editing panel — which is the NORMAL way these files get written — and the newer
    // build's content is gone, with nothing on any screen to say so.
    //
    // `parsed` must be false too, and that is the load-bearing half. A layer that merely failed
    // validation is still a layer; a file this build could not read is not one, and a merge that
    // took it for one would write the truncated result back over the original.
    {
        const auto tooNew = [](const char *arrayKey) {
            return QByteArray("{\"id\":\"x\",\"version\":\"1\",\"schemaVersion\":99,\"")
                   + arrayKey + "\":[]}";
        };

        const ScreenLoadResult s = loadScreenSet(tooNew("screens"), QStringLiteral("future"));
        check(!s.parsed && !s.loaded && hasCode(s.report, "schemaTooNew"),
              "a screen set from a newer build is refused, not partially read");

        const DrillLoadResult d = loadDrillSet(tooNew("drills"), QStringLiteral("future"));
        check(!d.parsed && !d.loaded && hasCode(d.report, "schemaTooNew"),
              "a drill set from a newer build is refused, not partially read");

        const ReferenceLoadResult r = loadReferenceSet(tooNew("references"),
                                                       QStringLiteral("future"));
        check(!r.parsed && !r.loaded && hasCode(r.report, "schemaTooNew"),
              "a reference set from a newer build is refused, not partially read");

        // The version this build writes is still readable by this build — the gate has to admit
        // its own output, and a k-constant bumped without the writer following it would not.
        check(loadScreenSet(saveScreenSet(screens), QStringLiteral("rt")).parsed
                  && loadDrillSet(saveDrillSet(drills), QStringLiteral("rt")).parsed
                  && loadReferenceSet(saveReferenceSet(refs), QStringLiteral("rt")).parsed,
              "…and what this build writes still loads through the gate");

        // Unparseable bytes are `parse` in all three. The reference loader reported this as
        // `schemaVersion` — a code naming a fault the file had not committed, on the one registry
        // where a reader chasing "which version is wrong?" would find nothing to fix.
        check(hasCode(loadScreenSet(QByteArray("{ not json"), QStringLiteral("broken")).report,
                      "parse")
                  && hasCode(loadDrillSet(QByteArray("{ not json"),
                                          QStringLiteral("broken")).report, "parse")
                  && hasCode(loadReferenceSet(QByteArray("{ not json"),
                                              QStringLiteral("broken")).report, "parse"),
              "and unparseable bytes are `parse` in all three, one word for one fault");
    }

    // ── The bibliography ────────────────────────────────────────────────────
    //
    // The join runs Provenance::citation -> Reference::doi OR ::pmid OR ::isbn and is an EXACT
    // STRING MATCH into a separate file, exactly like screenRef and drills. A citation whose
    // identifier is not in the registry fails silently and in the worst possible way: the edge
    // still loads, still validates, still renders its tier chip — and the References view simply
    // never mentions the claim, so the library looks better sourced than it is.
    std::printf("=== references ===\n");
    const CharacteristicPack pack = shippedPack();
    {
        check(!refs.references.empty(), "the shipped reference set loads");
        check(validateReferenceSet(refs).ok(), "and validates clean");

        QSet<QString> known;
        for (const Reference &r : refs.references) {
            if (!r.doi.isEmpty())  known.insert(r.doi);
            if (!r.pmid.isEmpty()) known.insert(r.pmid);
            if (!r.isbn.isEmpty()) known.insert(r.isbn);
        }

        int dangling = 0, cited = 0;
        for (const Edge &e : pack.edges) {
            if (e.provenance.citation.isEmpty()) continue;
            ++cited;
            if (!known.contains(e.provenance.citation)) {
                ++dangling;
                std::printf("        edge '%s' -> '%s' cites '%s', which is not in the registry\n",
                            qPrintable(e.from), qPrintable(e.to),
                            qPrintable(e.provenance.citation));
            }
        }
        for (const Condition &c : pack.conditions) {
            if (c.provenance.citation.isEmpty()) continue;
            ++cited;
            if (!known.contains(c.provenance.citation)) {
                ++dangling;
                std::printf("        condition '%s' cites '%s', which is not in the registry\n",
                            qPrintable(c.id), qPrintable(c.provenance.citation));
            }
        }
        std::printf("        (%d citations over %d references, %d dangling)\n",
                    cited, int(refs.references.size()), dangling);
        check(cited > 0, "the pack cites something");
        check(dangling == 0, "every citation in the pack resolves to a reference");

        // Every reference must be openable and identifiable. A row that renders as a bare
        // identifier is one a reader cannot judge, and the identifier is also the only route to
        // the source itself. Any of the three kinds counts — some journals issue no DOI at all,
        // and a book never had one.
        bool complete = true;
        for (const Reference &r : refs.references)
            if ((r.doi.isEmpty() && r.pmid.isEmpty() && r.isbn.isEmpty()) || r.title.isEmpty()
                || r.authors.isEmpty() || r.year <= 0)
                complete = false;
        check(complete, "every reference carries an identifier, title, authors and year");

        bool urls = true;
        for (const Reference &r : refs.references)
            if (!r.url().startsWith(QStringLiteral("https://doi.org/"))
                && !r.url().startsWith(QStringLiteral("https://pubmed.ncbi.nlm.nih.gov/"))
                && !r.url().startsWith(QStringLiteral("https://openlibrary.org/isbn/")))
                urls = false;
        check(urls, "and resolves to a doi.org, PubMed or Open Library URL the view can open");

        // A DOI that is cited by nothing is legitimate and must NOT be pruned: one of them is the
        // paper that contradicts claims the pack does make, and dropping it would leave the
        // bibliography agreeing with itself.
        int uncited = 0;
        for (const Reference &r : refs.references) {
            const auto cites = [&r](const QString &c) {
                if (c.isEmpty()) return false;
                return (!r.doi.isEmpty() && c == r.doi) || (!r.pmid.isEmpty() && c == r.pmid)
                    || (!r.isbn.isEmpty() && c == r.isbn);
            };
            bool used = false;
            for (const Edge &e : pack.edges)
                if (cites(e.provenance.citation)) used = true;
            for (const Condition &c : pack.conditions)
                if (cites(c.provenance.citation)) used = true;
            if (!used) ++uncited;
        }
        std::printf("        (%d references cited by nothing — kept deliberately)\n", uncited);

        const ReferenceLoadResult rr = loadReferenceSet(saveReferenceSet(refs), QStringLiteral("rt"));
        check(rr.loaded && rr.pack.references.size() == refs.references.size(),
              "a reference set survives save and load");

        // A journal that issues no DOI is not a reason to drop a source. The PMID is the same join
        // key and reaches the same reader, so it must validate clean and produce a live URL — a
        // PMID pushed through the doi.org template yields a link that is well-formed and dead.
        // Fields are named rather than positional: this fixture is where a new field on Reference
        // would otherwise shift every value one slot to the right and be caught, if at all, by
        // whichever assertion happened to look wrong first.
        {
            Reference r;
            r.id      = QStringLiteral("ref.pmidOnly");
            r.pmid    = QStringLiteral("30479527");
            r.title   = QStringLiteral("T");
            r.authors = QStringLiteral("A");
            r.journal = QStringLiteral("J");
            r.year    = 2018;

            ReferenceSet pm;
            pm.references.push_back(r);
            check(validateReferenceSet(pm).ok(),
                  "a reference identified only by PMID validates — some journals issue no DOI");
            check(pm.references.front().url()
                      == QStringLiteral("https://pubmed.ncbi.nlm.nih.gov/30479527/"),
                  "…and opens at PubMed rather than a well-formed but dead doi.org link");
            check(pm.byCitation(QStringLiteral("30479527")) != nullptr,
                  "…and a citation carrying that PMID joins to it");
        }

        // ── The third identifier ────────────────────────────────────────────
        //
        // Most golf coaching doctrine was never published in a journal: it lives in books and in
        // chapters of edited volumes, whose identifier is an ISBN. Everything the PMID case asserts
        // has to hold for it too, plus one thing that does not arise for a PMID — a book must not
        // be able to claim a peer-reviewed tier (gated in core_pack_test and characteristic_pack).
        {
            Reference r;
            r.id        = QStringLiteral("ref.isbnOnly");
            r.isbn      = QStringLiteral("9781875378371");
            r.title     = QStringLiteral("T");
            r.authors   = QStringLiteral("A");
            r.publisher = QStringLiteral("P");
            r.year      = 2001;

            ReferenceSet bk;
            bk.references.push_back(r);
            const ValidationReport rep = validateReferenceSet(bk);
            check(rep.ok(),
                  "a reference identified only by ISBN validates — a book never had a DOI");
            check(!hasCode(rep, "referenceNoDoi"),
                  "…and referenceNoDoi does not fire on it, which is the whole point of the field");
            check(bk.references.front().url()
                      == QStringLiteral("https://openlibrary.org/isbn/9781875378371"),
                  "…and opens at a library catalogue, not a bookseller and not a dead doi.org link");
            check(bk.references.front().identifierLabel()
                      == QStringLiteral("ISBN 9781875378371"),
                  "…and its identifier is labelled — thirteen bare digits say nothing on their own");
            check(bk.byCitation(QStringLiteral("9781875378371")) != nullptr,
                  "…and a citation carrying that ISBN joins to it");

            // Hyphens are presentation, not identity: the stored form is as authored, and only the
            // URL normalises. A reader who copies "978-1-875378-37-1" out of a title page and an
            // author who typed it bare must reach the same catalogue page.
            Reference h = r;
            h.isbn = QStringLiteral("978-1-875378-37-1");
            check(h.url() == QStringLiteral("https://openlibrary.org/isbn/9781875378371"),
                  "…and a hyphenated ISBN strips to the same URL");
            check(h.identifierLabel() == QStringLiteral("ISBN 978-1-875378-37-1"),
                  "…while the LABEL keeps the hyphens the author wrote");

            // The null this whole session exists to distinguish from: none of the three.
            ReferenceSet none = bk;
            none.references.front().isbn.clear();
            check(hasCode(validateReferenceSet(none), "referenceNoDoi"),
                  "a reference with none of the three is still refused");

            ReferenceSet dupIsbn = bk;
            dupIsbn.references.push_back(dupIsbn.references.front());
            dupIsbn.references.back().id = QStringLiteral("ref.otherIsbn");
            check(hasCode(validateReferenceSet(dupIsbn), "duplicateIsbn"),
                  "two references sharing an ISBN are refused — the second is unreachable");
            check(!hasCode(rep, "duplicateIsbn"),
                  "…and the code stays silent when the ISBNs differ");
        }

        // How an identifier READS. A DOI says what it is; a bare PubMed id is eight digits that
        // could be a year, a count or an internal key, so it is labelled everywhere it surfaces —
        // the provenance block on a characteristic, not just the bibliography.
        {
            check(citationLabel(QStringLiteral("30479527"))
                      == QStringLiteral("PMID 30479527"),
                  "a PMID citation renders labelled, so the number means something");
            check(citationLabel(QStringLiteral("10.1123/jab.27.3.242"))
                      == QStringLiteral("10.1123/jab.27.3.242"),
                  "a DOI renders as itself — it already announces what it is");
            check(citationLabel(QString()).isEmpty(), "and an absent citation stays absent");
            // Unresolvable citations still have to render: a user-layer pack may name a source the
            // shipped registry has never heard of.
            check(citationLabel(QStringLiteral("99999999")) == QStringLiteral("PMID 99999999"),
                  "an unregistered all-digit citation falls back to the PMID label by shape");

            // THE REGRESSION THAT MATTERS. An ISBN-13 is thirteen digits, so the all-digits rule
            // above renders an unresolved one as "PMID 9780646407548" — well-formed, confident and
            // wrong, and wrong in the direction that sends a reader to PubMed for a book. The shape
            // test has to discriminate BEFORE it falls through, and this is the case that says so.
            check(citationLabel(QStringLiteral("9780646407548"))
                      == QStringLiteral("ISBN 9780646407548"),
                  "an unregistered 13-digit 978 citation renders as an ISBN, NOT as a PMID");
            check(citationLabel(QStringLiteral("9791234567896"))
                      == QStringLiteral("ISBN 9791234567896"),
                  "…and so does the 979 prefix, which is the one nobody remembers exists");
            // ISBN-10's check digit is mod 11, so its eleventh value is written 'X'. A reader will
            // assume that branch is a typo; it is not, and this is the case that proves it.
            check(citationLabel(QStringLiteral("080442957X")) == QStringLiteral("ISBN 080442957X"),
                  "…and an ISBN-10 ending in the X check digit is an ISBN, not a stray DOI");
            check(citationLabel(QStringLiteral("978-0-646-40754-8"))
                      == QStringLiteral("ISBN 978-0-646-40754-8"),
                  "…and the hyphenated form is recognised while rendering as authored");
            // The discrimination has to cut both ways or it has just moved the bug: a PubMed id is
            // eight digits and must not acquire an ISBN label on its way past the new branch.
            check(citationLabel(QStringLiteral("12345678")) == QStringLiteral("PMID 12345678"),
                  "…and an 8-digit citation is still a PMID — the new branch did not swallow it");
        }

        // Validators, in both directions.
        ReferenceSet bad = refs;
        bad.references.front().doi.clear();
        bad.references.front().pmid.clear();
        bad.references.front().isbn.clear();   // all THREE, or the check quietly stops firing
        check(hasCode(validateReferenceSet(bad), "referenceNoDoi"),
              "a reference with no identifier at all is refused — nothing could cite or open it");

        ReferenceSet dup = refs;
        dup.references.push_back(dup.references.front());
        dup.references.back().id = QStringLiteral("ref.other");
        check(hasCode(validateReferenceSet(dup), "duplicateDoi"),
              "two references sharing a DOI are refused — the second is unreachable");

        ReferenceSet dupPmid = refs;
        dupPmid.references.front().doi.clear();
        dupPmid.references.front().pmid = QStringLiteral("12345678");
        dupPmid.references.push_back(dupPmid.references.front());
        dupPmid.references.back().id = QStringLiteral("ref.otherPmid");
        check(hasCode(validateReferenceSet(dupPmid), "duplicatePmid"),
              "and so are two sharing a PMID — the join is by identifier, not by DOI specifically");

        ReferenceSet ns = refs;
        ns.references.front().id = QStringLiteral("vad2004");
        check(hasCode(validateReferenceSet(ns), "referenceIdNamespace"),
              "an id outside the ref. namespace is refused");

        // ── General reading ─────────────────────────────────────────────────
        //
        // The registry holds two kinds of record: the sources behind a citation, and the field's
        // standard works. The flag is ADDITIVE — it says a record earns its place on its own, and
        // says nothing about whether anything cites it — so the two populations overlap and the
        // round trip has to carry the flag without carrying an implied category with it.
        std::printf("=== general reading ===\n");
        {
            Reference r;
            r.id             = QStringLiteral("ref.reading");
            r.doi            = QStringLiteral("10.1000/reading");
            r.title          = QStringLiteral("T");
            r.authors        = QStringLiteral("A");
            r.journal        = QStringLiteral("J");
            r.year           = 2020;
            r.generalReading = true;

            ReferenceSet gr;
            gr.references.push_back(r);

            const ReferenceLoadResult rt = loadReferenceSet(saveReferenceSet(gr),
                                                            QStringLiteral("rt"));
            check(rt.loaded && rt.pack.references.size() == 1
                      && rt.pack.references.front().generalReading,
                  "generalReading survives save and load");

            // The absence of the flag and a false flag are the same fact, which is the entire
            // reason this needed no schemaVersion bump. If `false` were written out, an older
            // reader would meet a key it does not know on every record rather than on none.
            ReferenceSet plain = gr;
            plain.references.front().generalReading = false;
            check(!QString::fromUtf8(saveReferenceSet(plain))
                       .contains(QStringLiteral("generalReading")),
                  "…and a false flag is omitted on save, so absent and false stay indistinguishable");
            check(!loadReferenceSet(saveReferenceSet(plain), QStringLiteral("rt"))
                       .pack.references.front().generalReading,
                  "…and an absent flag parses back as false");

            // THE FLAG IS NOT AN EXEMPTION. The failure mode worth a test rather than a comment is
            // somebody reading "general reading" as "held to a lower standard": a record here for
            // its own sake still has to be reachable and still has to be judgeable.
            ReferenceSet noId = gr;
            noId.references.front().doi.clear();
            check(hasCode(validateReferenceSet(noId), "referenceNoDoi"),
                  "a general-reading record with no identifier is still refused");

            ReferenceSet noTitle = gr;
            noTitle.references.front().title.clear();
            check(hasCode(validateReferenceSet(noTitle), "referenceNoTitle"),
                  "…and one with no title still warns, exactly as a cited record would");

            ReferenceSet noYear = gr;
            noYear.references.front().year = 0;
            check(hasCode(validateReferenceSet(noYear), "referenceNoYear"),
                  "…and one with no year still warns");

            ReferenceSet nsGr = gr;
            nsGr.references.front().id = QStringLiteral("reading2020");
            check(hasCode(validateReferenceSet(nsGr), "referenceIdNamespace"),
                  "…and the ref. namespace still applies to it");
        }

        // The shipped general-reading records are held to the same openable-URL rule as every other
        // record — asserted through the SAME `urls` check above, which spans the whole registry, so
        // there is one rule here and not two. What is worth stating separately is that the flag has
        // actually been used: a check over an empty population passes without meaning anything.
        {
            int flagged = 0;
            for (const Reference &r : refs.references)
                if (r.generalReading) ++flagged;
            std::printf("        (%d of %d references carry generalReading)\n",
                        flagged, int(refs.references.size()));
            check(flagged > 0, "the shipped registry actually uses the flag");
        }
    }

    // ── CSL-JSON export ─────────────────────────────────────────────────────
    //
    // A bibliography nobody can get out of the app is an appendix. CSL-JSON is what Zotero imports
    // and what pandoc consumes, so one export renders in every citation style without us writing
    // one — which is exactly why the mapping has to be right: a consumer that half-understands the
    // file produces a plausible citation with the wrong parts missing, and says nothing.
    //
    // There is no importer, so these gate SHAPE rather than a round trip: what a consumer is handed,
    // not what we can read back. The two things easiest to get wrong and hardest to see are the
    // uppercase identifier fields and the nesting of `issued`, and both have a check of their own.
    std::printf("=== CSL-JSON export ===\n");
    {
        auto itemsOf = [](const ReferenceSet &s) {
            return QJsonDocument::fromJson(exportReferenceSetCsl(s)).array();
        };
        auto oneItem = [&itemsOf](const Reference &r) {
            ReferenceSet s;
            s.references.push_back(r);
            const QJsonArray a = itemsOf(s);
            return a.isEmpty() ? QJsonObject() : a.first().toObject();
        };

        // Named rather than positional, for the reason given above the PMID fixture: a new field on
        // Reference would otherwise shift every value one slot right and be caught, if at all, by
        // whichever assertion happened to look wrong first.
        Reference art;
        art.id          = QStringLiteral("ref.art");
        art.doi         = QStringLiteral("10.1000/art");
        art.title       = QStringLiteral("T");
        art.authors     = QStringLiteral("Brown, Selbie, Wallace");
        art.journal     = QStringLiteral("Journal of Sports Sciences");
        art.year        = 2005;
        art.volume      = QStringLiteral("23");
        art.issue       = QStringLiteral("Suppl 1");   // not a number, and that is the point
        art.pages       = QStringLiteral("1156-1163");
        art.establishes = QStringLiteral("E");

        Reference chap;
        chap.id             = QStringLiteral("ref.chap");
        chap.isbn           = QStringLiteral("9781875378371");
        chap.title          = QStringLiteral("C");
        chap.authors        = QStringLiteral("Cheetham");
        chap.publisher      = QStringLiteral("P");
        chap.containerTitle = QStringLiteral("Optimising Performance in Golf");
        chap.editor         = QStringLiteral("Thomas");
        chap.year           = 2001;
        chap.pages          = QStringLiteral("192-199");

        Reference bk;
        bk.id        = QStringLiteral("ref.bk");
        bk.isbn      = QStringLiteral("9780387986913");
        bk.title     = QStringLiteral("B");
        bk.authors   = QStringLiteral("Jorgensen");
        bk.publisher = QStringLiteral("S");
        bk.year      = 1994;

        const QJsonObject ai = oneItem(art);
        const QJsonObject ci = oneItem(chap);
        const QJsonObject bi = oneItem(bk);

        // ── The outer shape ─────────────────────────────────────────────────
        {
            const QJsonDocument doc = QJsonDocument::fromJson(exportReferenceSetCsl(refs));
            check(doc.isArray() && !doc.isObject(),
                  "the export is a bare JSON array — a consumer handed an object wrapping one "
                  "fails with an error that says nothing about which end is wrong");
            check(doc.array().size() == int(refs.references.size()),
                  "…carrying one item per record, with none dropped");
        }

        // ── Type, inferred from what the record already carries ─────────────
        check(ai.value(QStringLiteral("type")).toString() == QStringLiteral("article-journal"),
              "a record with no ISBN types as an article — the identifier decides, nothing else");
        check(ci.value(QStringLiteral("type")).toString() == QStringLiteral("chapter"),
              "…an ISBN with a containerTitle is a chapter of that volume");
        check(bi.value(QStringLiteral("type")).toString() == QStringLiteral("book"),
              "…and an ISBN on its own is the book");

        // ── issued: the nesting everybody gets wrong ─────────────────────────
        {
            const QJsonArray parts = ai.value(QStringLiteral("issued")).toObject()
                                       .value(QStringLiteral("date-parts")).toArray();
            check(parts.size() == 1 && parts.first().toArray().size() == 1
                      && parts.first().toArray().first().isDouble()
                      && parts.first().toArray().first().toInt() == 2005,
                  "issued is date-parts: an array of arrays holding the year as an INTEGER");

            Reference undated = art;
            undated.year = 0;
            check(!oneItem(undated).contains(QStringLiteral("issued")),
                  "…and an undated record omits it rather than claiming the year 0");
        }

        // ── The identifiers are the only uppercase names in the format ──────
        check(ai.contains(QStringLiteral("DOI")) && !ai.contains(QStringLiteral("doi")),
              "the DOI key is uppercase — CSL names everything lowercase EXCEPT the identifiers");
        check(bi.contains(QStringLiteral("ISBN")) && !bi.contains(QStringLiteral("isbn")),
              "…and so is the ISBN");
        check(ai.value(QStringLiteral("URL")).toString() == art.url()
                  && !ai.contains(QStringLiteral("url")),
              "…and the URL, which comes from the accessor so doi->pmid->isbn stays in one place");
        {
            Reference pm = art;
            pm.doi.clear();
            pm.pmid = QStringLiteral("30479527");
            const QJsonObject pi = oneItem(pm);
            check(pi.contains(QStringLiteral("PMID")) && !pi.contains(QStringLiteral("pmid")),
                  "…and the PMID, which is a legal CSL-JSON field rather than an extension of ours");
        }

        // ── Locators ────────────────────────────────────────────────────────
        check(ai.value(QStringLiteral("page")).toString() == QStringLiteral("1156-1163")
                  && !ai.contains(QStringLiteral("pages")),
              "the locator is CSL `page`, SINGULAR — `pages` is our spelling, not the format's");
        check(ai.value(QStringLiteral("issue")).toString() == QStringLiteral("Suppl 1"),
              "…and an issue designator that was never a number survives as written");

        // ── Authors, and why they are literal ───────────────────────────────
        {
            const QJsonArray au = ai.value(QStringLiteral("author")).toArray();
            check(au.size() == 1
                      && au.first().toObject().value(QStringLiteral("literal")).toString()
                             == art.authors,
                  "authors are ONE CSL `literal` name — deliberately: the field is a single string "
                  "and structuring it is a schemaVersion bump, not a serialiser change");
            check(ci.value(QStringLiteral("editor")).toArray().first().toObject()
                      .value(QStringLiteral("literal")).toString() == QStringLiteral("Thomas"),
                  "…and a volume's editor travels the same way");
            check(!ai.contains(QStringLiteral("editor")),
                  "…and a record with no editor omits the field rather than emitting an empty name");
        }

        // ── note: our words, said to be ours ────────────────────────────────
        check(ai.value(QStringLiteral("note")).toString()
                  == QStringLiteral("PinPoint diagnostics: E"),
              "establishes exports as a note, PREFIXED — it is our editorial judgement, and it "
              "travels into somebody else's library where nothing else says so");
        {
            Reference bare = art;
            bare.establishes.clear();
            check(!oneItem(bare).contains(QStringLiteral("note")),
                  "…and a record with nothing to say omits the note rather than prefixing silence");
        }

        // ── One container, and the record that claims two ───────────────────
        check(ai.value(QStringLiteral("container-title")).toString() == art.journal,
              "an article's container-title is its journal");
        check(ci.value(QStringLiteral("container-title")).toString() == chap.containerTitle,
              "…and a chapter's is the volume it appears in");
        {
            ReferenceSet clash;
            Reference    both  = art;
            both.containerTitle = QStringLiteral("A Book");
            clash.references.push_back(both);
            check(hasCode(validateReferenceSet(clash), "referenceContainerConflict"),
                  "a record carrying BOTH a journal and a containerTitle is refused — they are one "
                  "field in a citation, so it claims to be a paper and a chapter at once");

            ReferenceSet onlyJournal;
            onlyJournal.references.push_back(art);
            check(!hasCode(validateReferenceSet(onlyJournal), "referenceContainerConflict"),
                  "…and one carrying only a journal is not");

            ReferenceSet onlyContainer;
            onlyContainer.references.push_back(chap);
            check(!hasCode(validateReferenceSet(onlyContainer), "referenceContainerConflict"),
                  "…nor one carrying only a containerTitle");
        }

        // ── categories ──────────────────────────────────────────────────────
        {
            Reference gr = art;
            gr.generalReading = true;
            const QJsonArray cats = oneItem(gr).value(QStringLiteral("categories")).toArray();
            check(cats.size() == 1
                      && cats.first().toString() == QStringLiteral("general reading"),
                  "a general-reading record says so in CSL `categories`");
            check(!ai.contains(QStringLiteral("categories")),
                  "…and an unflagged one omits it, exactly as the stored flag does");
        }

        // ── THE SHIPPED REGISTRY, not a fixture ─────────────────────────────
        //
        // A check that cannot fire on real content is a check nobody notices is broken. Every
        // assertion above would pass over an empty registry; these cannot.
        {
            const QJsonArray live = itemsOf(refs);
            int missing = 0, blanks = 0;
            int nArticle = 0, nBook = 0, nChapter = 0, noLocator = 0;

            for (const QJsonValue &v : live) {
                const QJsonObject o = v.toObject();

                if (o.value(QStringLiteral("id")).toString().isEmpty()
                    || o.value(QStringLiteral("type")).toString().isEmpty()
                    || o.value(QStringLiteral("title")).toString().isEmpty()) ++missing;

                // Top level only, which is sufficient: the two nested shapes are `author`/`editor`
                // (omitted entirely when the source string is empty) and `categories` (a constant).
                for (auto it = o.begin(); it != o.end(); ++it) {
                    const QJsonValue val = it.value();
                    if (val.isString() && val.toString().isEmpty()) ++blanks;
                }

                const QString t = o.value(QStringLiteral("type")).toString();
                if (t == QLatin1String("article-journal")) {
                    ++nArticle;
                    if (!o.contains(QStringLiteral("volume"))
                        && !o.contains(QStringLiteral("page"))) ++noLocator;
                } else if (t == QLatin1String("book"))    ++nBook;
                else if   (t == QLatin1String("chapter")) ++nChapter;
            }

            check(missing == 0,
                  "every shipped record exports with an id, a type and a title");
            check(blanks == 0,
                  "…and no field anywhere in the export is an empty string, which a consumer would "
                  "render as present-but-blank and punctuate around");
            check(nArticle > 0 && nBook > 0,
                  "…and the export actually spans both kinds of source");

            std::printf("        (%d article-journal, %d book, %d chapter"
                        " — %d articles carry no locator)\n",
                        nArticle, nBook, nChapter, noLocator);

            // Printed, never asserted. A CSL-JSON error is almost always visible on inspection and
            // almost never visible in a passing shape test, so one item of each kind goes to the
            // log for a human. Pinning the content here would be the mistake the file warns about.
            auto printFirst = [&live](const char *want) {
                for (const QJsonValue &v : live) {
                    if (v.toObject().value(QStringLiteral("type")).toString()
                            != QLatin1String(want)) continue;
                    std::printf("      ── first %s ──\n%s\n", want,
                                QJsonDocument(v.toObject())
                                    .toJson(QJsonDocument::Indented).constData());
                    return;
                }
            };
            printFirst("article-journal");
            printFirst("chapter");
            printFirst("book");
        }
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
