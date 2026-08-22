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
    LINK    OpenSSL::SSL OpenSSL::Crypto Threads::Threads
    STD 20
    NO_QT
    GTEST)
