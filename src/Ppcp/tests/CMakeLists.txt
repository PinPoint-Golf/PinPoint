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

# ══════════════════════════════════════════════════════════════════════════
# ⚠⚠⚠  THE PLAINTEXT HARNESS SOCKET, AND THE ONE PLACE IT IS TURNED ON  ⚠⚠⚠
#
# `PPCP-CONF` §2c requires the peer under test to be driven from OUTSIDE by the
# synthetic peer both implementations develop against.  `tools/ppcp-sim` has no
# TLS *transport* — its `--psk-ke-only` mode is a hand-built ClientHello for
# RT-4 and speaks no application data — so `ppcp-conform` cannot reach a
# TLS-only host at all.  `PPCP-RV` erratum E4 (RV 2c1) settles what that means:
# 2c constrains the rendezvous PATHS, and a conformance harness socket is not
# one of them; `PPCP-CORE` §3.2's `direct` transport is conformant plaintext.
#
# `PP_PPCP_PLAINTEXT_HARNESS` is DECLARED in the application's own
# CMakeLists.txt, default OFF, where a `PP_SHIPPING_BUILD` refuses to configure
# with it ON.  It is set ON HERE — in the test tree, and nowhere else in this
# repository.  A release build therefore compiles no plaintext code path at all,
# which is the same discipline `Connector::connect()` keeps for RV 5.2f.
#
# Turning it OFF must still leave every OTHER row in this file building: the
# guard is on the transport's plaintext branches and on the harness executable
# at the bottom of this file, never on anything the shipping code needs.
set(PP_PPCP_PLAINTEXT_HARNESS ON CACHE BOOL
    "Compile the PLAINTEXT conformance-harness listener (CONF 2c). NEVER in a release build."
    FORCE)
message(STATUS "PinPointTests: PPCP plaintext harness listener ON — this is a test tree")
#
# ⚠ AND IT IS SET ON ONE TARGET, NOT ON THE DIRECTORY.  `ppcp_conform_host` at
# the bottom of this file is the only thing that gets the macro.  Every other
# suite here compiles `ppcp_transport.cpp` WITHOUT it — which is what makes
# "the shipping build still has no plaintext code path" a fact this suite
# checks on every build rather than something somebody verified once by hand.

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


# ── H8 — the conformance claim ───────────────────────────────────────────────
#
# `PPCP-CONF` §2c: an implementation tested only by its own unit tests is the
# single-implementation trap.  `libppcp/tools/ppcp-conform` (L14) is the
# instrument all three implementations are measured by; it drives the peer under
# test from outside, through its real transport, with `ppcp-sim` as the
# counterpart, and every assertion is made on the wire.
#
# `ppcp_conform_host` is the peer it dials: the REAL `PpcpHostPeer`, the REAL
# `makeHostEngine()`, the REAL ingest policy and the REAL declaration builder,
# with no Qt GUI and no device enumeration.  See the note at the top of the .cpp
# for what it deliberately does NOT link and why.
#
# ⚠ IT IS BUILT ONLY WITH THE PLAINTEXT GUARD ON, because without it there is no
# listener it can reach.  That is the guard doing its job, not a gap.
if(PP_PPCP_PLAINTEXT_HARNESS)
    add_executable(ppcp_conform_host
        ppcp_conform_host.cpp
        ${PPCP}/ppcp_bundle_transport.cpp
        ${PPCP}/ppcp_import_sink.cpp
        ${PPCP}/ppcp_host_peer.cpp
        ${PPCP}/ppcp_live_session.cpp
        ${PPCP}/ppcp_shot_bridge.cpp
        ${PPCP}/ppcp_annotation_store.cpp
        ${PPCP}/ppcp_offer_controller.cpp
        ${PPCP}/ppcp_import_ledger.cpp
        ${PPCP}/ppcp_host_engine.cpp
        ${PPCP}/ppcp_ingest_policy.cpp
        ${PPCP}/ppcp_source_declaration.cpp
        ${PPCP}/ppcp_transport.cpp)
    set_target_properties(ppcp_conform_host PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON AUTOMOC ON)
    target_include_directories(ppcp_conform_host PRIVATE ${PPCP} ${PP_BUFFER})
    target_link_libraries(ppcp_conform_host PRIVATE
        ppcp Qt6::Core Qt6::Multimedia OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    # THE ONLY TARGET IN THIS REPOSITORY THAT GETS THE MACRO.  See the banner
    # at the top of this file, and `Listener::setPlaintextHarness()`.
    target_compile_definitions(ppcp_conform_host PRIVATE
        PP_PPCP_SRC_DIR="${PPCP}" PP_PPCP_PLAINTEXT_HARNESS)
    pp_apply_sanitizers(ppcp_conform_host)

    # ── Where `ppcp-conform` is ─────────────────────────────────────────────
    #
    # NOT built here.  `pp_require_ppcp()` forces `PPCP_BUILD_TOOLS` OFF when the
    # library is embedded — correctly, because this repository must not start
    # building another repository's command line tools — so the instrument comes
    # from libppcp's OWN build tree.  Ground rule 1: no code crosses repos; a
    # binary that measures us is not code we carry.
    if(NOT PP_PPCP_CONFORM)
        find_program(PP_PPCP_CONFORM ppcp-conform
            HINTS "${ppcp_SOURCE_DIR}/build/dev/tools/ppcp-conform"
                  "${ppcp_SOURCE_DIR}/build/tools/ppcp-conform"
            NO_DEFAULT_PATH)
    endif()

    if(PP_PPCP_CONFORM)
        message(STATUS "PinPointTests: ppcp-conform at ${PP_PPCP_CONFORM}")
        add_test(NAME ppcp_conformance
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-conform.sh
                         $<TARGET_FILE:ppcp_conform_host>
                         ${PP_PPCP_CONFORM}
                         ${CMAKE_CURRENT_BINARY_DIR})
        # The claim run drives twelve rows over real sockets, one link each.
        set_tests_properties(ppcp_conformance PROPERTIES TIMEOUT 300)
    else()
        # LOUD, and not a silent skip: a conformance row that quietly does not
        # run is how a claim stops being evidence.
        message(WARNING
            "ppcp-conform NOT FOUND — the H8 conformance row will not run. Build it in the "
            "libppcp checkout (cmake --build --preset dev --target ppcp-conform ppcp-sim) or "
            "pass -DPP_PPCP_CONFORM=/path/to/ppcp-conform.")
    endif()

    # ── S5 — the `PPCP-CONF` §5 interoperability rows ────────────────────────
    #
    # A different claim again from `ppcp_conformance`.  That row asks "does this
    # host pass every CONF §3/§4 test the instrument has for the profiles it
    # claims"; these ask CONF §5's question, which is a different one: does this
    # host interoperate with a peer of a STATED SHAPE — an observer, an
    # `unrelated` clock, a nominating device, a device with a preview.  §5 opens
    # by saying why both are needed: "two implementations that each pass §3 and
    # §4 alone can still fail to interoperate."
    #
    # Each row is `run-interop.sh`, which asserts on BOTH ends — `ppcp-sim`'s
    # `--expect` and the host's own `--summary` JSON.  The declarations and
    # scenarios come from libppcp's `tools/scenarios/`, never from a copy here:
    # ground rule 1, no code crosses repos.
    if(PP_PPCP_SIM AND EXISTS "${PP_PPCP_SIM}")
        set(_pp_sim "${PP_PPCP_SIM}")
    else()
        find_program(PP_PPCP_SIM ppcp-sim
            HINTS "${ppcp_SOURCE_DIR}/build/dev/tools/ppcp-sim"
                  "${ppcp_SOURCE_DIR}/build/tools/ppcp-sim"
            NO_DEFAULT_PATH)
        set(_pp_sim "${PP_PPCP_SIM}")
    endif()

    set(_pp_scenarios "${ppcp_SOURCE_DIR}/tools/scenarios")
    # Where the PinPointCapture agent checks in the bundles ITS device wrote.
    # IOP-3 is an interoperability row and this directory is what makes it one;
    # `run-interop.sh` says so out loud when it is missing and falls back to
    # libppcp's fixtures, which measure the reader and not the pairing.
    set(_pp_device_bundles "${CMAKE_CURRENT_SOURCE_DIR}/../../../../PinPointCapture/docs/conformance/bundles")
    set(_pp_fixtures "${ppcp_SOURCE_DIR}/tests/fixtures")

    if(_pp_sim AND EXISTS "${_pp_sim}" AND EXISTS "${_pp_scenarios}")
        message(STATUS "PinPointTests: ppcp-sim at ${_pp_sim}")
        foreach(_row IOP-3 IOP-4 IOP-5 IOP-6 IOP-7 IOP-8 IOP-9 IOP-10)
            string(REPLACE "-" "_" _rowname "${_row}")
            string(TOLOWER "${_rowname}" _rowname)
            add_test(NAME ppcp_interop_${_rowname}
                     COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-interop.sh
                             ${_row}
                             $<TARGET_FILE:ppcp_conform_host>
                             ${_pp_sim}
                             ${_pp_scenarios}
                             ${CMAKE_CURRENT_BINARY_DIR}/interop
                             ${_pp_device_bundles}
                             ${_pp_fixtures})
            # IOP-8 waits out a 3 s arbitration delay on purpose (I35's margin);
            # everything else is under 15 s.  60 is slack, not a budget.
            set_tests_properties(ppcp_interop_${_rowname} PROPERTIES TIMEOUT 60)
        endforeach()
    else()
        message(WARNING
            "ppcp-sim NOT FOUND — the S5 interoperability rows will not run. Build it in the "
            "libppcp checkout (cmake --build --preset dev --target ppcp-conform ppcp-sim) or "
            "pass -DPP_PPCP_SIM=/path/to/ppcp-sim.")
    endif()
endif()
