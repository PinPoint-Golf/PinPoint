# The bundled font faces, and the ONE list of them.
#
# ⚠ THESE ARE NOT DECORATION — THEY ARE THE METRICS EVERY LAYOUT IS MEASURED IN.
# Theme.qml names families ("Geist Mono", "Hanken Grotesk", …) and says it "falls back to
# the system default if the font file is not installed". That fallback is not a graceful
# degradation for a LAYOUT TEST: a fallback face has different advance widths, so a test
# measuring whether a label fits inside a card is measuring a font the application never
# renders with.
#
# That is not hypothetical. `qml_ui_test` did not load these, and neither this Mac nor the
# studio PC has Geist Mono installed system-wide — so the suite was measuring two DIFFERENT
# fallback faces on the two platforms. macOS fell back to something narrow and passed;
# Windows fell back to something wider and failed four containment assertions in
# tst_lm_graphics. Neither result described the shipping application.
#
# So this list is consumed twice, and must stay one list:
#
#   the app   — qt_add_resources(PinPointStudio "app_fonts" ...) in the root CMakeLists,
#               loaded at startup by src/Gui/main.cpp via QFontDatabase::addApplicationFont
#   the tests — the same resource compiled into qml_ui_test, loaded by its setup object
#
# The C++ side deliberately does NOT carry a matching hardcoded list: it enumerates
# `:/fonts` at run time, so a face added here reaches both consumers with no third place to
# update. (main.cpp still spells its own list out; that duplication predates this file and
# is worth collapsing the same way when someone is next in there.)
#
# Paths are relative to the repo root; each consumer prefixes them.

set(PP_FONT_FILES
    src/Resources/fonts/Georgia.ttf
    src/Resources/fonts/Georgiab.ttf
    src/Resources/fonts/Georgiai.ttf
    src/Resources/fonts/Georgiaz.ttf
    src/Resources/fonts/DMSans-Variable.ttf
    src/Resources/fonts/DMSans-Italic-Variable.ttf
    src/Resources/fonts/DMMono-Regular.ttf
    src/Resources/fonts/DMMono-Medium.ttf
    src/Resources/fonts/DMSerifDisplay-Regular.ttf
    src/Resources/fonts/Fraunces-Variable.ttf
    src/Resources/fonts/Fraunces-Italic-Variable.ttf
    src/Resources/fonts/Fraunces-Regular.ttf
    src/Resources/fonts/Fraunces-SemiBold.ttf
    src/Resources/fonts/SourceSerif4-Variable.ttf
    src/Resources/fonts/SourceSerif4-Italic-Variable.ttf
    src/Resources/fonts/HankenGrotesk-Variable.ttf
    src/Resources/fonts/HankenGrotesk-Italic-Variable.ttf
    src/Resources/fonts/Literata-Variable.ttf
    src/Resources/fonts/Literata-Italic-Variable.ttf
    src/Resources/fonts/Literata-Regular.ttf
    src/Resources/fonts/Literata-Medium.ttf
    src/Resources/fonts/InstrumentSans-Variable.ttf
    src/Resources/fonts/JetBrainsMono-Variable.ttf
    src/Resources/fonts/PlayfairDisplay-Variable.ttf
    src/Resources/fonts/Geist-Variable.ttf
    src/Resources/fonts/GeistMono-Variable.ttf
    src/Resources/fonts/SpaceGrotesk-Variable.ttf
    src/Resources/fonts/SpaceMono-Regular.ttf
    src/Resources/fonts/SpaceMono-Bold.ttf
)
