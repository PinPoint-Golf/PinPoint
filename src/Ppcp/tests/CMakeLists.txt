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
            ${PPCP}/ppcp_ingest_policy.cpp
            ${PPCP}/ppcp_source_declaration.cpp
            ${PPCP}/ppcp_transport.cpp
    INCLUDE ${PPCP}
    LINK    ppcp Qt6::Multimedia OpenSSL::SSL OpenSSL::Crypto Threads::Threads
    DEFINES ${_hp_defines}
    STD 20
    GTEST)
