# Remove the `prefer` line from a staged copy of the PinPointStudio QML module.
#
# qt_add_qml_module writes `prefer :/qt/qml/PinPointStudio/` into the generated qmldir, which tells
# the engine to load the module's files from the Qt RESOURCE system rather than from disk. That is
# right for the app — the resources are compiled into its binary — and it is exactly wrong for any
# other executable: the test binary has no such resources, so every type in the module resolves to
# "No such file or directory" and the import fails whole.
#
# Stripping the line on a COPY leaves the app's own module untouched and points the test at the same
# .qml sources on disk. What changes is how they are executed: from disk they are interpreted rather
# than run as qmlcachegen's AOT output. That difference is not cosmetic and is worth stating —
# it is precisely why qml_reactivity_test exists as a source-reading test, because the interpreter
# keeps the dead property read that the compiler drops. This suite asserts BEHAVIOUR; that one
# guards the compiled/interpreted gap. Neither substitutes for the other.
#
#   cmake -DPP_QMLDIR=<path/to/qmldir> -P StripQmlPrefer.cmake

if(NOT DEFINED PP_QMLDIR)
    message(FATAL_ERROR "StripQmlPrefer: PP_QMLDIR is required")
endif()
if(NOT EXISTS "${PP_QMLDIR}")
    message(FATAL_ERROR "StripQmlPrefer: no qmldir at ${PP_QMLDIR}")
endif()

file(READ "${PP_QMLDIR}" _pp_qmldir_text)
string(REGEX REPLACE "(^|\n)[ \t]*prefer[ \t][^\n]*" "\\1" _pp_qmldir_text "${_pp_qmldir_text}")
file(WRITE "${PP_QMLDIR}" "${_pp_qmldir_text}")
