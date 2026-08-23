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


# ── H6 — the QR encoder (RV 4.1d) ───────────────────────────────────────────
# NO Qt and NO OpenSSL, and above the OpenSSL gate for the same reason the
# bundle suite is: a pairing code is a picture of a URI, and nothing about
# drawing one depends on there being a TLS stack on the box.
#
# The suite is where the evidence for a hand-written encoder lives — there is no
# QR decoder in this repository and no third-party symbol to diff against, so it
# proves the Reed-Solomon stage by the divisibility property, the two BCH stages
# against the published constants of ISO/IEC 18004, the module structure against
# the standard, and the whole pipeline by reading the finished symbol back out.
pp_add_test(ppcp_qr_test
    SOURCES ppcp_qr_test.cpp ${PPCP}/ppcp_qr.cpp
    INCLUDE ${PPCP}
    STD 20
    NO_QT
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
            ${PPCP}/ppcp_live_session.cpp
            ${PPCP}/ppcp_shot_bridge.cpp
            ${PPCP}/ppcp_annotation_store.cpp
            ${PPCP}/ppcp_host_engine.cpp
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
            ${PPCP}/ppcp_transport.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia OpenSSL::SSL OpenSSL::Crypto Threads::Threads
    DEFINES ${_hp_defines}
    STD 20
    GTEST)


# ── H6 — PPCP-RV rendezvous, host side ──────────────────────────────────────
# RT-5, RT-7 (browser half), RT-8, RT-9, RT-15 and RT-16.
#
# ⚠ IT NEEDS OpenSSL BECAUSE THE REFUSALS ARE REAL HANDSHAKES.  "The publisher
# invalidates the code" (7.3a) and "the publisher refuses a handshake past exp"
# (7.3e) are claims about what happens when somebody dials, and RV 5.2i is
# explicit that this class of assertion is settled by observed behaviour rather
# than through an API.  So the suite stands up H1's Listener on loopback,
# installs PpcpRendezvous::identityResolver() on it, and dials it with a scanner
# built entirely out of libppcp's ppcp_rv_* functions — the same four calls
# PinPointCapture will make.
#
# ⚠ AND Security.framework, because RV 7.2c is "protected storage where one
# exists" and this application's existing secrets subsystem (QSettings, a plain
# plist) is not one.  The keychain store is compiled here so the suite fails at
# the link line if it ever stops building; it is NOT exercised at runtime,
# because the login keychain cannot be unlocked from a non-Aqua session and a
# test that needed it would be red on every headless box.  RT-12 is a review
# row and that is where the keychain path is read.
set(_rv_link ppcp OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(APPLE)
    list(APPEND _rv_link "-framework Security" "-framework CoreFoundation")
endif()

pp_add_test(ppcp_rendezvous_test
    SOURCES ppcp_rendezvous_test.cpp
            ${PPCP}/ppcp_rendezvous.cpp
            ${PPCP}/ppcp_pairing_store.cpp
            ${PPCP}/ppcp_discovery.cpp
            ${PPCP}/ppcp_transport.cpp
    INCLUDE ${PPCP}
    LINK    ${_rv_link}
    STD 20
    NO_QT
    GTEST)


# ── H-compose (b) — every app-side PPCP translation unit, syntax-only ───────
#
# ⚠ WHY THIS ROW EXISTS.  On 22 Aug the application build broke on
# `ppcp_import_controller.cpp` — a positional `HostEngineConfig` had shifted the
# listener onto the health callback — and NOTHING IN ppcp-tests COMPILED THAT
# FILE.  Every suite above links the handful of translation units it needs; the
# app-only ones (the QML controllers, the composition, the device registry
# adapter) had no compiler pointed at them at all until Mark built the app.  A
# green ppcp-tests therefore said nothing whatever about whether the application
# still built, which is the worst property a gate can have.
#
# So this target compiles them — syntax only, with the app's own include paths
# and Qt, no linking, no Q_OBJECT moc pass and no application dependency chain.
# It is a compiler front end run over the sources, which is exactly the class of
# break it exists to catch and is a second or two of wall clock.
#
# ⚠ GLOBBED ON PURPOSE, AND IT IS THE ONE GLOB IN THIS FILE.  Every other list
# here is explicit, matching the repository's convention; this one must not be,
# because a translation unit somebody forgets to add is precisely the failure
# being guarded against.  CONFIGURE_DEPENDS re-runs it when the directory
# changes.
#
# ⚠ WHAT IS NOT IN IT, AND WHY.  `src/Gui/main.cpp` reaches whisper, ONNX
# Runtime, OpenCV, Sparkle and the whole model stack; syntax-checking it would
# mean reproducing the application's entire include graph in a suite whose whole
# point is not to need one.  The composition therefore lives in its own
# translation unit (`ppcp_host_service.cpp`) and main.cpp's share of it is four
# lines — construct, wire, expose — which is the smallest surface this row can
# leave uncovered and is stated here rather than left to be discovered.
file(GLOB _app_ppcp_tus CONFIGURE_DEPENDS ${PPCP}/*.cpp)
list(APPEND _app_ppcp_tus
    ${PP_SRC}/Video/VideoInputPpcp.cpp
    ${PP_SRC}/Video/VideoInputPpcp_inventory.cpp
    ${PP_SRC}/Gui/shot/shot_controller.cpp)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Multimedia Bluetooth Qml)

# EXCLUDE_FROM_ALL: `cmake --build` must not pay for it, and the ctest row below
# is what asks for it.  AUTOMOC OFF because -fsyntax-only produces no object and
# a moc pass would be checking a different thing anyway.
add_library(ppcp_app_tu_syntax_probe OBJECT EXCLUDE_FROM_ALL ${_app_ppcp_tus})
set_target_properties(ppcp_app_tu_syntax_probe PROPERTIES
    AUTOMOC OFF CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
# -Wno-unused-command-line-argument: CMake still passes -c and -o, and neither
# means anything under -fsyntax-only.  The warning is about the invocation, not
# the code, and leaving it in would train everyone to ignore this target's
# output — which is the one thing it cannot afford.
target_compile_options(ppcp_app_tu_syntax_probe PRIVATE
    -fsyntax-only -Wno-unused-command-line-argument)
target_compile_definitions(ppcp_app_tu_syntax_probe PRIVATE HAVE_PPCP HAVE_PPCP_TRANSPORT)
# The app's include list, trimmed to what a PPCP translation unit can reach:
# CMakeLists.txt lines 562-571, minus the subsystems none of these files touch.
target_include_directories(ppcp_app_tu_syntax_probe PRIVATE
    ${PPCP} ${PP_SRC}/Video ${PP_SRC}/Core ${PP_SRC}/Buffer ${PP_SRC}/IMU
    ${PP_SRC}/Gui ${PP_SRC}/Gui/shot ${PP_SRC}/Gui/session ${PP_SRC}/Gui/athlete)
target_link_libraries(ppcp_app_tu_syntax_probe PRIVATE
    Qt6::Core Qt6::Gui Qt6::Multimedia Qt6::Bluetooth Qt6::Qml ppcp)
if(PP_OPENSSL_FOUND)
    target_link_libraries(ppcp_app_tu_syntax_probe PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

add_test(NAME ppcp_app_tu_syntax
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                 --target ppcp_app_tu_syntax_probe)
