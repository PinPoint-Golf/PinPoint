# Silence deprecation warnings in the libsamplerate 0.2.2 CMakeLists.txt.
# Invoked at configure time via FetchContent PATCH_COMMAND.
# FILE must be passed as -DFILE=<absolute_path>.

if(NOT DEFINED FILE)
  message(FATAL_ERROR "PatchLibsamplerate.cmake: FILE variable not set")
endif()

file(READ "${FILE}" content)

# cmake_minimum_required VERSION 3.1 triggers "Compatibility with CMake < 3.5"
# deprecation in CMake 3.30+.  Raising the min to 3.5 silences it while keeping
# the existing 3.18 upper bound.
string(REPLACE
    "cmake_minimum_required(VERSION 3.1..3.18)"
    "cmake_minimum_required(VERSION 3.5..3.18)"
    content "${content}")

# The CMP0091 OLD branch fires on non-MSVC (where CMAKE_MSVC_RUNTIME_LIBRARY is
# not defined), producing a "OLD behavior deprecated" warning.  NEW is correct
# for all platforms we target.
string(REPLACE
    "cmake_policy(SET CMP0091 OLD)"
    "cmake_policy(SET CMP0091 NEW)"
    content "${content}")

file(WRITE "${FILE}" "${content}")
