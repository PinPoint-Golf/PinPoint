# PinPointQtPrefix.cmake — point CMAKE_PREFIX_PATH at the newest installed Qt.
#
# Included by the app's top-level CMakeLists.txt and by tests/cmake/PinPointTests.cmake,
# so a developer gets the same Qt whether they configure the app or a test suite.
#
# ⚠ NEVER replace this with a hardcoded version. Getting a hardcoded prefix wrong
# is SILENT: a path that does not exist is not appended, no error is raised, and
# find_package(Qt6) walks CMake's ordinary search path instead. On macOS that
# lands on the Homebrew Qt, which carries no Quick3D, so the configure dies
# claiming a required COMPONENT is missing. Nothing in that message points at the
# prefix, which is the actual fault. That is a day lost to the wrong question.
#
# ⚠ Compared with VERSION_GREATER, not sorted. list(SORT) and glob order are both
# lexical, which ranks 6.9 above 6.10 — wrong at exactly the minor rollover this
# exists to survive. COMPARE NATURAL would do it but wants CMake 3.18, and the
# standalone suites under src/*/tests still declare 3.16.
#
# Stands aside when the caller already said which Qt to use: an explicit
# -DCMAKE_PREFIX_PATH or a CMAKE_PREFIX_PATH in the environment. CI always passes
# one of those, so it never reaches the search below.
include_guard(GLOBAL)

if(CMAKE_PREFIX_PATH OR DEFINED ENV{CMAKE_PREFIX_PATH})
    return()
endif()

# QT_ROOT_DIR is exported by install-qt-action and by Qt's own env scripts. It
# names a Qt, but nothing in CMake consumes it, so honour it rather than merely
# deferring to it — skipping here would leave find_package(Qt6) with no prefix at
# all, which is the failure this file exists to prevent.
if(DEFINED ENV{QT_ROOT_DIR} AND EXISTS "$ENV{QT_ROOT_DIR}/lib/cmake/Qt6/Qt6Config.cmake")
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT_DIR}")
    message(STATUS "PinPoint: using Qt prefix $ENV{QT_ROOT_DIR} (QT_ROOT_DIR)")
    return()
endif()

if(APPLE)
    set(_pp_qt_root "$ENV{HOME}/Qt")
    set(_pp_qt_abis macos)
elseif(WIN32)
    set(_pp_qt_root "$ENV{HOMEDRIVE}$ENV{HOMEPATH}/Qt")
    set(_pp_qt_abis msvc2022_64)
else()
    set(_pp_qt_root "$ENV{HOME}/Qt")
    set(_pp_qt_abis gcc_64)
endif()

set(_pp_qt "")
set(_pp_qt_ver "0")
file(GLOB _pp_qt_dirs "${_pp_qt_root}/*")
foreach(_pp_d IN LISTS _pp_qt_dirs)
    get_filename_component(_pp_v "${_pp_d}" NAME)
    # The installer tree also holds Tools/, Docs/, Licenses/, MaintenanceTool.
    if(_pp_v MATCHES "^[0-9]+\\.[0-9]+")
        foreach(_pp_abi IN LISTS _pp_qt_abis)
            # Qt6Config.cmake, not the directory: an ABI dir can survive an
            # uninstall, and find_package needs the config either way.
            if(EXISTS "${_pp_d}/${_pp_abi}/lib/cmake/Qt6/Qt6Config.cmake"
               AND _pp_v VERSION_GREATER _pp_qt_ver)
                set(_pp_qt_ver "${_pp_v}")
                set(_pp_qt "${_pp_d}/${_pp_abi}")
            endif()
        endforeach()
    endif()
endforeach()

if(_pp_qt)
    list(APPEND CMAKE_PREFIX_PATH "${_pp_qt}")
    message(STATUS "PinPoint: using Qt prefix ${_pp_qt}")
else()
    # Say so. Silence is what made the old failure so hard to read.
    message(STATUS "PinPoint: no Qt6 under ${_pp_qt_root}/<version>/<abi>"
                   " — falling back to CMake's search path. If a COMPONENT"
                   " turns up missing, pass -DCMAKE_PREFIX_PATH explicitly.")
endif()

unset(_pp_qt_root)
unset(_pp_qt_abis)
unset(_pp_qt_ver)
unset(_pp_qt_dirs)
unset(_pp_d)
unset(_pp_v)
unset(_pp_abi)
