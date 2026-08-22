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

// CT-I37 on the host path, and CORE 5.18's placement and supersession rules.
// Work package H7.
//
// ⚠ CT-I37 IS ASSERTED BY API SURFACE, NOT BEHAVIOUR (CONF §3), so the last
// test in this file greps `src/Analysis` for an include of a markup header and
// fails if one appears.  A grep is an odd-looking test and it is the right one:
// the invariant is that no analysis EVER reads an Annotation, and no runtime
// assertion can say that about code nobody wrote yet.

#include "ppcp_annotation_store.h"
#include "ppcp_host_engine.h"
#include "ppcp_test_peer.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace Ppcp;
using pptest::DevicePeer;
using pptest::idStr;

namespace fs = std::filesystem;

namespace {

constexpr const char *kSession = "sess:markup";
constexpr const char *kShot    = "shot:1";
constexpr const char *kStream  = "st:dev-1:src-cam:video";

// A body that is NOT text: an embedded NUL and high bytes, because 5.18a's
// "lossless round-trip" is only tested by something a text codec would break.
std::vector<std::uint8_t> awkwardBody()
{
    std::vector<std::uint8_t> b;
    for (int i = 0; i < 512; ++i) b.push_back(static_cast<std::uint8_t>((i * 37) & 0xff));
    b[10] = 0x00;
    b[11] = 0x00;
    b[12] = 0xff;
    return b;
}

struct TempRoot {
    fs::path p;
    TempRoot()
    {
        std::random_device rd;
        p = fs::temp_directory_path()
            / ("ppcp-annot-" + std::to_string(rd()) + "-" + std::to_string(rd()));
        fs::create_directories(p);
    }
    ~TempRoot() { std::error_code ec; fs::remove_all(p, ec); }
};

ppcp_annotation makeIncoming(const char *id, const char *author, std::uint64_t revision,
                             const std::vector<std::uint8_t> &body)
{
    ppcp_annotation a{};
    ppcp_instant at{}, created{};
    EXPECT_EQ(ppcp_instant_make_z(&at, "tb:dev", 1000), PPCP_OK);
    EXPECT_EQ(ppcp_instant_make_z(&created, "tb:dev", 2000), PPCP_OK);
    EXPECT_EQ(ppcp_annotation_make(&a, id, kSession, kShot, &at, author, PPCP_ANNOT_USER,
                                   "line", "application/json", body.data(), body.size(),
                                   &created, revision), PPCP_OK);
    EXPECT_EQ(ppcp_annotation_set_stream_id(&a, kStream), PPCP_OK);
    return a;
}

}  // namespace

// ── 5.18a — the body round-trips byte for byte, through the disk ──────────

TEST(PpcpAnnotations, ThePersistedBodyComesBackByteIdentical)
{
    TempRoot root;
    const std::vector<std::uint8_t> body = awkwardBody();

    {
        PpcpAnnotationStore s;
        std::string err;
        ASSERT_TRUE(s.setRoot(root.p.string(), &err)) << err;
        ppcp_annotation a = makeIncoming("an:1", "dev-1", 1, body);
        ASSERT_TRUE(s.observe(a, nullptr, &err)) << err;
        EXPECT_EQ(s.stats().persisted, 1u);
    }

    // A fresh store, a fresh process's worth of state, reading what was written.
    PpcpAnnotationStore s2;
    std::string err;
    ASSERT_TRUE(s2.setRoot(root.p.string(), &err)) << err;
    ASSERT_TRUE(s2.loadFromRoot(&err)) << err;
    ASSERT_EQ(s2.count(), 1u);
    const ppcp_annotation *got = s2.find("an:1");
    ASSERT_NE(got, nullptr);

    // 5.18a — "lossless round-tripping is the requirement and interpreting the
    // format explicitly is not one."  The embedded NULs are the assertion: a
    // store that had gone through a text encoder would have truncated at byte
    // ten and every other field would still look right.
    ASSERT_EQ(got->body_len, body.size());
    EXPECT_EQ(std::memcmp(got->body, body.data(), body.size()), 0);
    EXPECT_EQ(idStr(got->format), std::string("application/json"));
    EXPECT_EQ(idStr(got->kind), std::string("line"));
    ASSERT_TRUE(got->has_stream_id);
    EXPECT_EQ(idStr(got->stream_id), std::string(kStream));
    EXPECT_EQ(got->revision, 1u);
}

// ── 5.18e / 9.0c — supersession, and CONVERGENCE IN EITHER ORDER ─────────

TEST(PpcpAnnotations, TwoDeliveryOrdersConvergeOnTheSameRevision)
{
    const std::vector<std::uint8_t> b1{ 1, 2, 3 };
    const std::vector<std::uint8_t> b2{ 4, 5, 6, 7 };

    ppcp_annotation r1 = makeIncoming("an:1", "dev-1", 1, b1);
    ppcp_annotation r2 = makeIncoming("an:1", "dev-1", 2, b2);

    PpcpAnnotationStore forward, backward;
    bool replaced = false;

    ASSERT_TRUE(forward.observe(r1, &replaced));
    EXPECT_TRUE(replaced);
    ASSERT_TRUE(forward.observe(r2, &replaced));
    EXPECT_TRUE(replaced);

    ASSERT_TRUE(backward.observe(r2, &replaced));
    EXPECT_TRUE(replaced);
    ASSERT_TRUE(backward.observe(r1, &replaced));
    // 9.0c — "lower is ignored".  The store did not change, and that is
    // reported rather than silently absorbed: a caller that persisted on every
    // observe() would write the losing revision to disk.
    EXPECT_FALSE(replaced);
    EXPECT_EQ(backward.stats().superseded, 1u);

    ASSERT_EQ(forward.count(), 1u);
    ASSERT_EQ(backward.count(), 1u);
    const ppcp_annotation *f = forward.find("an:1");
    const ppcp_annotation *b = backward.find("an:1");
    ASSERT_NE(f, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(f->revision, 2u);
    EXPECT_EQ(b->revision, 2u);
    ASSERT_EQ(f->body_len, b->body_len);
    EXPECT_EQ(std::memcmp(f->body, b->body, f->body_len), 0);
}

TEST(PpcpAnnotations, EqualRevisionsFromTwoAuthorsAreBrokenBytewiseAndNotIgnored)
{
    // The case the tiebreak exists for: a coach at the host and a golfer at the
    // device edit the same annotation concurrently.  Both hold revision 1, both
    // produce revision 2.  WITHOUT the `author_peer_id` tiebreak each receives
    // an equal revision, ignores it, and the two ends diverge permanently while
    // each believes it converged.
    const std::vector<std::uint8_t> ba{ 0xAA };
    const std::vector<std::uint8_t> bb{ 0xBB };
    ppcp_annotation fromA = makeIncoming("an:1", "aaa-peer", 2, ba);
    ppcp_annotation fromB = makeIncoming("an:1", "zzz-peer", 2, bb);

    PpcpAnnotationStore forward, backward;
    ASSERT_TRUE(forward.observe(fromA));
    ASSERT_TRUE(forward.observe(fromB));
    ASSERT_TRUE(backward.observe(fromB));
    ASSERT_TRUE(backward.observe(fromA));

    const ppcp_annotation *f = forward.find("an:1");
    const ppcp_annotation *b = backward.find("an:1");
    ASSERT_NE(f, nullptr);
    ASSERT_NE(b, nullptr);
    // Both ends agree on WHICH one won, whatever order they saw them in.
    EXPECT_EQ(idStr(f->author_peer_id), idStr(b->author_peer_id));
    ASSERT_EQ(f->body_len, b->body_len);
    EXPECT_EQ(std::memcmp(f->body, b->body, f->body_len), 0);
    EXPECT_NE(ppcp_annotation_supersedes(&fromA, &fromB), 0)
        << "an equal revision from a different author is NOT a tie";
}

// ── 5.18j / 5.18g — placement ────────────────────────────────────────────

TEST(PpcpAnnotations, AViewSpecificKindMustNameTheStreamAndIsAnchoredInItsTimebase)
{
    DevicePeer dev;
    ASSERT_NO_FATAL_FAILURE(dev.build());
    HostEngineConfig cfg;
    cfg.peerId = "host-1";
    cfg.listener = true;
    std::string why;
    std::unique_ptr<PpcpEngine> host = makeHostEngine(std::move(cfg), &why);
    ASSERT_NE(host, nullptr) << why;
    ASSERT_EQ(ppcp_peer_declare(dev.p, &dev.desc), PPCP_OK);
    pptest::pipe(dev.p, host->peer(), PPCP_CHANNEL_CONTROL);
    pptest::drainEvents(host->peer(), [](const ppcp_event &) {});

    // A Stream on the DEVICE's clock, which is the view a host-authored line is
    // drawn on: the coach draws on the phone's video, not on ours.
    ppcp_stream st{};
    ppcp_instant opened{};
    ASSERT_EQ(ppcp_instant_make_z(&opened, dev.tb.c_str(), 5000), PPCP_OK);
    ASSERT_EQ(ppcp_stream_make(&st, kStream, kSession, "src-cam", PPCP_STREAM_KIND_VIDEO,
                               "p-cap", dev.tb.c_str(), PPCP_SHOT_WINDOWED, &opened),
              PPCP_OK);

    PpcpAnnotationStore s;
    s.attach(host->peer(), "host-1");
    std::string err;

    // 5.18j — `line` is view-specific, so omitting `stream_id` is refused
    // BEFORE origination rather than rendered nowhere at the far end.
    EXPECT_FALSE(s.author("an:bad", kSession, kShot, "line", "application/json",
                          { 1, 2, 3 }, /*streamId=*/"", /*stream=*/nullptr,
                          kHostTimebaseId, 6000, 7000, 1, &err));
    EXPECT_EQ(s.stats().placementRefused, 1u);

    // 5.18g — with a `stream_id`, `at` is in THAT STREAM'S timebase.  Putting
    // the host's own clock reading under the device's timebase id would be I1's
    // defect written into the wire.
    ASSERT_TRUE(s.author("an:line", kSession, kShot, "line", "application/json",
                         { 1, 2, 3 }, kStream, &st, kHostTimebaseId, 6000, 7000, 1, &err))
        << err;
    const ppcp_annotation *a = s.find("an:line");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(idStr(a->at.tb), dev.tb);
    ASSERT_TRUE(a->has_stream_id);

    // 5.18j again — `text` is NOT view-specific, carries no `stream_id`, and
    // 5.18g then anchors it in `Session.timebase_ref`.
    ASSERT_TRUE(s.author("an:text", kSession, kShot, "text", "text/plain",
                         { 'h', 'i' }, /*streamId=*/"", /*stream=*/nullptr,
                         kHostTimebaseId, 8000, 9000, 1, &err)) << err;
    const ppcp_annotation *t = s.find("an:text");
    ASSERT_NE(t, nullptr);
    EXPECT_FALSE(t->has_stream_id);
    EXPECT_EQ(idStr(t->at.tb), std::string(kHostTimebaseId));

    // MSG 9.0a — and both reached the wire.  `annotation` is the only content
    // in PPCP that travels either direction, and Markup confers it.
    EXPECT_EQ(s.stats().sent, 2u);
    std::size_t seen = 0;
    pptest::pipe(host->peer(), dev.p, PPCP_CHANNEL_CONTROL);
    pptest::drainEvents(dev.p, [&](const ppcp_event &e) {
        if (e.kind == PPCP_EVENT_ANNOTATION) ++seen;
    });
    EXPECT_EQ(seen, 2u);
}

TEST(PpcpAnnotations, DeletionIsARevisionAndNotARemoval)
{
    PpcpAnnotationStore s;
    s.attach(nullptr, "host-1");
    std::string err;
    ASSERT_TRUE(s.author("an:1", kSession, kShot, "text", "text/plain", { 'x' }, "",
                         nullptr, kHostTimebaseId, 100, 200, 1, &err)) << err;
    ASSERT_TRUE(s.markDeleted("an:1", 2, 300, &err)) << err;

    const ppcp_annotation *a = s.find("an:1");
    // Still held, and marked.  A store that ERASED it would accept the old
    // revision straight back from the far end and the deletion would undo
    // itself on the next sync.
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->revision, 2u);
    EXPECT_TRUE(a->has_deleted);
    EXPECT_TRUE(a->deleted);
    EXPECT_EQ(s.count(), 1u);
}

TEST(PpcpAnnotations, AnnotationsAreScopedToTheShotTheyAnchorTo)
{
    PpcpAnnotationStore s;
    s.attach(nullptr, "host-1");
    std::string err;
    ASSERT_TRUE(s.author("an:1", kSession, "shot:1", "text", "text/plain", { 'a' }, "",
                         nullptr, kHostTimebaseId, 1, 2, 1, &err)) << err;
    ASSERT_TRUE(s.author("an:2", kSession, "shot:2", "text", "text/plain", { 'b' }, "",
                         nullptr, kHostTimebaseId, 1, 2, 1, &err)) << err;

    EXPECT_EQ(s.forShot(kSession, "shot:1").size(), 1u);
    EXPECT_EQ(s.forShot(kSession, "shot:2").size(), 1u);
    EXPECT_EQ(s.forShot(kSession, "shot:3").size(), 0u);
}

// ── CT-I37 — asserted by the ABSENCE of an include ───────────────────────

TEST(PpcpAnnotations, NothingInAnalysisReadsAnAnnotation)
{
    // I37 / 5.18c: "there is no path from an Annotation to a Shot, a Candidate,
    // a Calibration or any computed quantity."  CONF §3 makes this a check on
    // API SURFACE, and the surface that matters on this side of the boundary is
    // what `src/Analysis` is allowed to include.
    //
    // ⚠ THE FAILURE THIS CATCHES IS A PLAUSIBLE ONE, not a hypothetical.  A
    // `kind: nav_anchor` annotation looks exactly like phase data — a labelled
    // instant on a shot — and the analysis ladder is full of code that wants
    // labelled instants.  Reading one would turn a user's drawing into an
    // observation and every metric downstream would inherit it.
    const fs::path analysis = fs::path(PP_PPCP_SRC_DIR) / ".." / "Analysis";
    ASSERT_TRUE(fs::exists(analysis)) << analysis.string();

    std::vector<std::string> offenders;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(analysis, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".h" && ext != ".cpp" && ext != ".hpp") continue;
        std::ifstream f(it->path());
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("#include") == std::string::npos) continue;
            if (line.find("ppcp/markup.h") != std::string::npos
                || line.find("ppcp_annotation_store.h") != std::string::npos
                || line.find("markup_truth.h") != std::string::npos
                || line.find("markup_controller.h") != std::string::npos) {
                offenders.push_back(it->path().string() + ": " + line);
            }
        }
    }
    EXPECT_TRUE(offenders.empty())
        << "src/Analysis must not read an Annotation (I37). Found:\n"
        << [&] { std::string s; for (const auto &o : offenders) s += "  " + o + "\n"; return s; }();
}
