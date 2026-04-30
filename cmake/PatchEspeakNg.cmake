# Patch espeak-ng 1.52.0 to fix include-path pollution on macOS/Linux.
#
# espeak-include originally exposes include/compat via INTERFACE_INCLUDE_DIRECTORIES.
# That directory shadows system headers (<wctype.h>, <endian.h>) in consumers like
# PinPoint, causing compilation failures.
#
# Strategy:
#   FILE1  src/CMakeLists.txt
#     Strip include/compat from espeak-include's public INTERFACE so it never
#     leaks to consumers.
#
#   FILE2  src/libespeak-ng/CMakeLists.txt
#     1. Keep the espeak-include link as PUBLIC (needed by espeak-ng-bin).
#     2. Re-add include/compat explicitly as PRIVATE to the espeak-ng library
#        itself — espeak-ng needs its own compat shims (endian.h, wctype.h …)
#        at compile time, but those shims must not propagate to consumers.
#
# Pass: -DFILE1=<abs_path_src_CMakeLists> -DFILE2=<abs_path_libespeak-ng_CMakeLists>

foreach(var FILE1 FILE2)
  if(NOT DEFINED "${var}")
    message(FATAL_ERROR "PatchEspeakNg.cmake: ${var} is not set")
  endif()
endforeach()

# --- src/CMakeLists.txt -------------------------------------------------
file(READ "${FILE1}" _c1)
string(REPLACE
    "target_include_directories(espeak-include INTERFACE include include/compat)"
    "target_include_directories(espeak-include INTERFACE include)"
    _c1 "${_c1}")
file(WRITE "${FILE1}" "${_c1}")

# --- src/libespeak-ng/CMakeLists.txt ------------------------------------
file(READ "${FILE2}" _c2)

# 1. Restore PUBLIC linkage (a prior workaround had set this to PRIVATE,
#    which broke espeak-ng-bin's access to public headers).
string(REPLACE
    "target_link_libraries(espeak-ng PRIVATE espeak-include)"
    "target_link_libraries(espeak-ng PUBLIC espeak-include)"
    _c2 "${_c2}")

# 2. Add include/compat as PRIVATE so espeak-ng's own sources can still find
#    their compat shims (endian.h, wctype.h, etc.) without exposing them.
string(REPLACE
    "target_include_directories(espeak-ng BEFORE PRIVATE \$<TARGET_PROPERTY:espeak-include,INTERFACE_INCLUDE_DIRECTORIES>)"
    "target_include_directories(espeak-ng BEFORE PRIVATE \$<TARGET_PROPERTY:espeak-include,INTERFACE_INCLUDE_DIRECTORIES> \${CMAKE_CURRENT_SOURCE_DIR}/../include/compat)"
    _c2 "${_c2}")

file(WRITE "${FILE2}" "${_c2}")
