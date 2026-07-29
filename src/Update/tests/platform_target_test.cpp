/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

// PlatformTarget arch-token mapping + compile-time current() resolution. The
// token table is the single place asset/feed arch strings are decided, so it is
// locked here; current() is a compile-time-constant sanity check.

#include "platform_target.h"

#include <gtest/gtest.h>

using namespace pp::update;

TEST(AssetArchToken, Linux)
{
    EXPECT_EQ((PlatformTarget{Os::Linux, Arch::X86_64}).assetArchToken(),
              QStringLiteral("x86_64"));
    EXPECT_EQ((PlatformTarget{Os::Linux, Arch::Arm64}).assetArchToken(),
              QStringLiteral("aarch64"));
}

TEST(AssetArchToken, MacOs)
{
    EXPECT_EQ((PlatformTarget{Os::MacOS, Arch::X86_64}).assetArchToken(),
              QStringLiteral("x86_64"));
    EXPECT_EQ((PlatformTarget{Os::MacOS, Arch::Arm64}).assetArchToken(),
              QStringLiteral("arm64"));
}

TEST(AssetArchToken, Windows)
{
    EXPECT_EQ((PlatformTarget{Os::Windows, Arch::X86_64}).assetArchToken(),
              QStringLiteral("x64"));
    EXPECT_EQ((PlatformTarget{Os::Windows, Arch::Arm64}).assetArchToken(),
              QStringLiteral("arm64"));
}

TEST(AssetArchToken, UnknownArchIsEmpty)
{
    EXPECT_TRUE((PlatformTarget{Os::Linux, Arch::Unknown}).assetArchToken().isEmpty());
}

// ── The macOS appcast filenames are a PUBLISHED CONTRACT ─────────────────────────
//
// "appcast-mac.xml" is baked into the Info.plist (SUFeedURL) of every Intel install
// already in the field, and Sparkle re-reads that URL from the INSTALLED bundle — not
// from anything we ship later. Renaming it, even for symmetry with the arm64 name,
// points every existing install at a URL that 404s: they would poll forever and
// silently never update again, with no way to fix them remotely.
//
// So: x86_64 keeps the unsuffixed name permanently, and arm64 got a NEW file rather
// than a rename of the old one.
//
// BE HONEST ABOUT WHAT THIS TEST IS. It is a tripwire, not enforcement. The real
// values live in three other places that this test cannot reach:
//   • CMakeLists.txt          — PP_SU_FEED_URL, baked into Info.plist at configure time
//   • tools/package_macos.sh  — the feed/arch cross-check before a DMG is named
//   • packaging/make_appcast_mac.sh — the output filename and its verification
// Changing this test does not change any of them; it exists so that anyone editing the
// arch tokens has to read the reason above first.
TEST(MacAppcastNames, AreFrozenByArch)
{
    const auto macToken = [](Arch a) {
        return (PlatformTarget{Os::MacOS, a}).assetArchToken();
    };

    // The x86_64 feed is the UNSUFFIXED name. Asserting that it does NOT carry its arch
    // token is the real content here: the obvious "tidy-up" is to rename it to
    // appcast-mac-x86_64.xml so the pair looks symmetric, and that is the one edit that
    // silently bricks updates for every Intel install in the field.
    EXPECT_FALSE(QStringLiteral("appcast-mac.xml").contains(macToken(Arch::X86_64)))
        << "the x86_64 feed must stay unsuffixed — renaming it orphans every existing "
           "Intel install (see the comment above)";

    // arm64, by contrast, is a NEW file and is suffixed with its token.
    EXPECT_EQ(QStringLiteral("appcast-mac-%1.xml").arg(macToken(Arch::Arm64)),
              QStringLiteral("appcast-mac-arm64.xml"));

    // The DMG asset names must carry these same tokens, since make_appcast_mac.sh
    // selects the enclosure by globbing on them.
    EXPECT_EQ(macToken(Arch::X86_64), QStringLiteral("x86_64"));
    EXPECT_EQ(macToken(Arch::Arm64), QStringLiteral("arm64"));

    // The two feeds must never collide — one item per feed is the whole design
    // (Sparkle has no arch attribute to negotiate with).
    EXPECT_NE(QStringLiteral("appcast-mac.xml"),
              QStringLiteral("appcast-mac-%1.xml").arg(macToken(Arch::Arm64)));
}

TEST(Current, MatchesCompiledPlatform)
{
    const PlatformTarget t = PlatformTarget::current();

#if defined(Q_OS_LINUX)
    EXPECT_EQ(t.os, Os::Linux);
#elif defined(Q_OS_WIN)
    EXPECT_EQ(t.os, Os::Windows);
#elif defined(Q_OS_MACOS)
    EXPECT_EQ(t.os, Os::MacOS);
#endif

    // Arch is one of the known enumerators on any platform we build/test on.
    EXPECT_TRUE(t.arch == Arch::X86_64 || t.arch == Arch::Arm64 || t.arch == Arch::Unknown);

    // On a supported build the token is non-empty (x86_64/aarch64/arm64/x64).
    if (t.arch != Arch::Unknown)
        EXPECT_FALSE(t.assetArchToken().isEmpty());
}
