# CTest suite for the PPCP transport (work package H1).  Self-contained — NOT
# part of the app build: no Qt, no whisper, no FFmpeg.  OpenSSL is the one
# dependency, because the transport is a TLS-PSK transport and there is nothing
# to test without it.
#
#   Standalone (fast single-suite iteration):
#     cmake -S src/Ppcp/tests -B build/ppcp-tests
#     cmake --build build/ppcp-tests -j && ctest --test-dir build/ppcp-tests --output-on-failure
#
#   Under the umbrella (all suites at once):
#     cmake -S tests -B build/tests && cmake --build build/tests -j && ctest --test-dir build/tests
#
# The suite runs entirely on loopback with the PPCP-RV §10.1 K_tls vector.  It
# needs no device, no pairing code and no network.

if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_ppcp_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

set(PPCP ${CMAKE_CURRENT_SOURCE_DIR}/..)   # src/Ppcp

# CameraCapabilities is Qt-built, so the declaration suite needs Multimedia for
# the QVideoFrameFormat that video_input_base.h pulls in.
find_package(Qt6 REQUIRED COMPONENTS Multimedia)

# libppcp — the declaration suite needs the L4 entity vocabulary, and the
# transport suites below need the L1 frame/CBOR/envelope writers for link_bind.
pp_require_ppcp()

# ── CT-S3 / CT-I19 — the host's own declaration (work package H2) ────────────
# Qt IS linked here, unlike the transport suites: CameraCapabilities is the
# shape the three camera backends normalise onto and it is built out of QString.
# DeviceEnumerator is NOT linked — hostInventory() lives in its own translation
# unit precisely so that the builder is a pure function of an Inventory and the
# MSG 3.3d case (a host with no Sources) is reachable on any machine.
pp_add_test(ppcp_source_declaration_test
    SOURCES ppcp_source_declaration_test.cpp ${PPCP}/ppcp_source_declaration.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia
    DEFINES PP_PPCP_SRC_DIR="${PPCP}"
    STD 20
    GTEST)

# Which libppcp packages are here today. The suites below assert the DEFERRAL
# as loudly as they would assert the feature: a test that goes red when L6 lands
# is how a deferral gets noticed instead of quietly becoming permanent.
pp_ppcp_landed(peer.h   ppcp_peer_new           PP_PPCP_HAVE_PEER)
pp_ppcp_landed(bundle.h ppcp_bundle_reader_new  PP_PPCP_HAVE_BUNDLE)
message(STATUS "PinPointTests: libppcp L6 peer engine ${PP_PPCP_HAVE_PEER}, "
               "L8 bundle reader ${PP_PPCP_HAVE_BUNDLE}")

# ── H3 — bundle import as a file transport ──────────────────────────────────
# CT-I12, CT-I15, CT-I16 and CT-I34, the host column.
#
# ⚠ NO OpenSSL AND NO TRANSPORT IN THIS TARGET, AND THAT IS AN ASSERTION. Plan
# A10 says a bundle is a FILE TRANSPORT for the same engine; a suite that had to
# link the TLS socket layer to read a file would have disproved it at the link
# line. It is above the pp_find_openssl() gate for the same reason — this row
# must run on a box with no TLS at all.
pp_add_test(ppcp_bundle_import_test
    SOURCES ppcp_bundle_import_test.cpp
            ${PPCP}/ppcp_bundle_transport.cpp
            ${PPCP}/ppcp_import_ledger.cpp
            ${PPCP}/ppcp_import_sink.cpp
            ${PPCP}/ppcp_host_engine.cpp
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia
    DEFINES PP_PPCP_SRC_DIR="${PPCP}"
    STD 20
    GTEST)

# ── H4 — a connected peer's camera Source behind the camera factory ─────────
# CT-S1 on the HOST path (the canonical-instant conversion, applied where the
# host applies it) and CT-I36a with the host as CONSUMER (preview is live-only).
#
# ⚠ NO OpenSSL AND NO TRANSPORT HERE EITHER, AND FOR THE SAME REASON THE BUNDLE
# SUITE HAS NONE. A Source declared over a socket and a Source declared in a
# bundle are the same Source; a camera backend that had to link TLS to describe
# one would have said otherwise at the link line. Two real ppcp_peers in one
# process, with the bytes moved between them a frame at a time.
#
# ⚠ AND NO DeviceEnumerator. VideoInputPpcp_inventory.cpp — the one translation
# unit that registers a peer's Sources with the app's device registry — is
# deliberately NOT in this target: device_enumerator.h reaches Bluetooth through
# imu_base.h, and everything worth asserting about this backend is a pure
# function of a declaration.
pp_add_test(ppcp_video_input_test
    SOURCES ppcp_video_input_test.cpp
            ${PP_SRC}/Video/VideoInputPpcp.cpp
            ${PP_SRC}/Video/video_input_base.cpp
            ${PP_SRC}/Video/raw_video_frame.cpp
            ${PPCP}/ppcp_host_engine.cpp
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
    INCLUDE ${PPCP} ${PP_SRC}/Video
    LINK    ppcp Qt6::Multimedia
    DEFINES PP_PPCP_SRC_DIR="${PPCP}"
    STD 20
    AUTOMOC
    GTEST)

# ── H5 — the live Session: sync, heartbeat, arm ─────────────────────────────
# CT-I21 (one probe sequence per LOCAL timebase), CT-I18's negative half (a
# conversion with no direct relation is REFUSED, never assumed zero), 7.4c's
# three missed intervals, and 5.10e's two arbitration parameters on the wire.
#
# ⚠ NO OpenSSL AND NO TRANSPORT, for the third time in this file and for the
# same reason: a Session is opened over a link, and NOTHING about what it
# carries depends on the link being TLS.  Two real ppcp_peers in one process,
# both on ppcp_sim_clocks with a deliberate offset and skew between them, so
# §6.3's exchange is asserted to RECOVER numbers it was never handed.
#
# ⚠ AND tools/ppcp-sim (libppcp L13) DOES NOT EXIST YET.  When it does these
# rows move from a same-process pair to a real independent implementation,
# which is what CONF §5's pairings ask for.  Until then this is the strongest
# available evidence and docs/ppcp-conformance.md says so.
pp_add_test(ppcp_live_session_test
    SOURCES ppcp_live_session_test.cpp
            ${PPCP}/ppcp_live_session.cpp
            ${PPCP}/ppcp_host_engine.cpp
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia
    DEFINES PP_PPCP_SRC_DIR="${PPCP}"
    STD 20
    GTEST)

# ── H5 — the arbitration bridge ─────────────────────────────────────────────
# CT-I7, CT-I8, CT-I20 and 8.1's three shapes.  The host declares a microphone
# of its OWN here, because CT-I8's second half — "two Candidates of the same
# `basis` from DIFFERENT peers" — is unreachable without one, and that is
# exactly the assertion a per-modality arbiter passes by never being asked.
pp_add_test(ppcp_arbitration_test
    SOURCES ppcp_arbitration_test.cpp
            ${PPCP}/ppcp_shot_bridge.cpp
            ${PPCP}/ppcp_live_session.cpp
            ${PPCP}/ppcp_host_engine.cpp
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia
    DEFINES PP_PPCP_SRC_DIR="${PPCP}"
    STD 20
    GTEST)

# ── H5 — the offer list, at the CONTROLLER level ────────────────────────────
# MSG 9.1–9.2.  The deliverable is a model and a QML delegate; the delegate is
# presentation and the model is the protocol obligation, so the evidence is
# stated over the model and survives a redesign of the screen.
#
# GTEST_NO_MAIN because the suite supplies its own: QLocale and QDateTime format
# the row labels and neither works without a QCoreApplication.
# AUTOMOC because PpcpOfferController is a QAbstractListModel.
pp_add_test(ppcp_offer_test
    SOURCES ppcp_offer_test.cpp
            ${PPCP}/ppcp_offer_controller.cpp
            ${PPCP}/ppcp_import_ledger.cpp
            ${PPCP}/ppcp_host_engine.cpp
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia
    DEFINES PP_PPCP_SRC_DIR="${PPCP}"
    STD 20
    AUTOMOC
    GTEST_NO_MAIN)

# ── H7 — Markup and annotations ─────────────────────────────────────────────
# CT-I37 (host), and CORE 5.18's placement and supersession rules.
#
# ⚠ THE LAST TEST IN THAT FILE IS A GREP OVER src/Analysis, and that is the
# method CONF §3 names for CT-I37: "asserted by API surface, not behaviour".  No
# runtime assertion can say "no analysis ever reads an Annotation" about code
# nobody has written yet; a check on what src/Analysis is allowed to INCLUDE
# can.  PP_PPCP_SRC_DIR is how it finds the tree.
pp_add_test(ppcp_annotation_test
    SOURCES ppcp_annotation_test.cpp
            ${PPCP}/ppcp_annotation_store.cpp
            ${PPCP}/ppcp_host_engine.cpp
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia
    DEFINES PP_PPCP_SRC_DIR="${PPCP}"
    STD 20
    GTEST)


# ⚠ EVERYTHING BELOW THIS LINE NEEDS OpenSSL, AND THE DECLARATION SUITE ABOVE
# DOES NOT.  A box without OpenSSL headers still runs CT-S3 and CT-I19; only the
# TLS-PSK transport rows go missing, and they say so.
pp_find_openssl()
if(NOT PP_OPENSSL_FOUND)
    # Not fatal: the umbrella must still configure on a box without OpenSSL
    # headers.  Loud, though — a silently missing PPCP suite is how RT-4, RT-11
    # and RT-14 stop being evidence.
    message(WARNING
        "PPCP transport tests SKIPPED — OpenSSL development headers not found. "
        "macOS: brew install openssl@3.  Debian/Ubuntu: apt install libssl-dev. "
        "Windows: vcpkg install openssl, or -DOPENSSL_ROOT_DIR=<path>.")
    return()
endif()

# ── Loopback TLS-PSK: handshake, failure modes, channel independence ─────────
# Qt-free by design (NO_QT): the transport owns no event loop, so the suite can
# run headless in the conformance harness of H8.  Threads: the listener accepts
# on a worker while the connector dials from the test thread.
pp_add_test(ppcp_transport_test
    SOURCES ppcp_transport_test.cpp ${PPCP}/ppcp_transport.cpp
    INCLUDE ${PPCP}
    LINK    OpenSSL::SSL OpenSSL::Crypto Threads::Threads ppcp
    STD 20
    NO_QT
    GTEST)

# ── ENC §2.1 link binding (erratum E1) ───────────────────────────────────────
# Concurrent dials, an abandoned dial, a late third channel and a bad first
# frame — the four cases the implicit rules of session 1 got wrong.
pp_add_test(ppcp_link_bind_test
    SOURCES ppcp_link_bind_test.cpp ${PPCP}/ppcp_transport.cpp
    INCLUDE ${PPCP}
    LINK    OpenSSL::SSL OpenSSL::Crypto Threads::Threads ppcp
    STD 20
    NO_QT
    GTEST)


# ── CT-I14 and the host peer adapter (work package H2, second half) ──────────
# Needs OpenSSL because ppcp_host_peer.h owns a PeerConnection from H1's
# transport. PPCP_HAVE_PEER is set only when libppcp's L6 header is real — see
# pp_ppcp_landed() in tests/cmake/PinPointTests.cmake.
set(_hp_defines PP_PPCP_SRC_DIR="${PPCP}")
if(PP_PPCP_HAVE_PEER)
    list(APPEND _hp_defines PPCP_HAVE_PEER)
endif()

pp_add_test(ppcp_host_peer_test
    SOURCES ppcp_host_peer_test.cpp
            ${PPCP}/ppcp_host_peer.cpp
            ${PPCP}/ppcp_host_engine.cpp
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
            ${PPCP}/ppcp_transport.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia OpenSSL::SSL OpenSSL::Crypto Threads::Threads
    DEFINES ${_hp_defines}
    STD 20
    GTEST)
