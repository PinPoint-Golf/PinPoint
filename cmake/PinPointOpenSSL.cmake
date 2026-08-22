# PinPointOpenSSL.cmake — locate OpenSSL for the PPCP transport (src/Ppcp).
#
# Shared by the app's top-level CMakeLists.txt and by the standalone test suite
# in src/Ppcp/tests, the same way cmake/PinPointQtPrefix.cmake is shared, so a
# developer gets the same OpenSSL whichever they configure.
#
# WHY OpenSSL AT ALL, when Qt Network is already linked.  PPCP-RV §5 needs a TLS
# EXTERNAL pre-shared key, and RV §8 says in as many words that the PSK
# interfaces in common toolkits — identity hints, RFC 4279 ciphersuite selection
# — do not reach TLS 1.3 external PSKs.  Qt's QSslPreSharedKeyAuthenticator is
# one of those interfaces.  The transport therefore drives OpenSSL's external-PSK
# session callbacks directly, which means the headers, not just the runtime Qt
# already loads.
#
# A missing OpenSSL is NOT fatal here.  It leaves PP_OPENSSL_FOUND false and the
# caller decides — the app excludes the transport with a warning, the test suite
# skips itself.  That keeps a Linux or Windows box without libssl-dev configuring
# as it always did, which the alternative (a hard find_package REQUIRED) would
# not.  It is a build-time absence, not a runtime downgrade: nothing in this
# project can fall back to an unencrypted connection (RV 5.2f) because no such
# code path exists.

include_guard(GLOBAL)

# Runs its find_package on EVERY configure, not once per build directory. A
# cached "yes" is not enough: find_package is what creates the OpenSSL::SSL
# imported target, and a target does not survive into the next configure. Caching
# the answer instead of repeating the question is how a reconfigure ends up
# linking to a target that was never defined.
macro(pp_find_openssl)
    if(NOT TARGET OpenSSL::SSL)
        # Homebrew's openssl@3 is keg-only, so CMake will not find it on macOS
        # without a hint — and what it WOULD find otherwise is LibreSSL, whose
        # SSL_CTX_set_psk_use_session_callback does not exist.  Probe the prefix
        # the same way the espeak-ng block in the top-level file does.
        if(APPLE AND NOT OPENSSL_ROOT_DIR)
            execute_process(
                COMMAND brew --prefix openssl@3
                OUTPUT_VARIABLE _pp_brew_openssl
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _pp_brew_result)
            if(_pp_brew_result EQUAL 0 AND EXISTS "${_pp_brew_openssl}/include/openssl/ssl.h")
                set(OPENSSL_ROOT_DIR "${_pp_brew_openssl}")
            endif()
            unset(_pp_brew_openssl)
            unset(_pp_brew_result)
        endif()

        # 1.1.1 is the floor: SSL_CTX_set_psk_use_session_callback and
        # SSL_CTX_set_psk_find_session_callback — the external-PSK session
        # callbacks RV §8 points at — arrived with TLS 1.3 support in 1.1.1.
        find_package(OpenSSL 1.1.1 QUIET COMPONENTS SSL Crypto)

        if(OpenSSL_FOUND)
            message(STATUS "OpenSSL ${OPENSSL_VERSION}: ${OPENSSL_SSL_LIBRARY}")
        endif()
    endif()

    if(TARGET OpenSSL::SSL)
        set(PP_OPENSSL_FOUND TRUE)
        set(PP_OPENSSL_VERSION "${OPENSSL_VERSION}")
    else()
        set(PP_OPENSSL_FOUND FALSE)
        set(PP_OPENSSL_VERSION "")
    endif()
endmacro()
