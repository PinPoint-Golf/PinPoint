# DevCodesign.cmake — sign the dev .app with a stable self-signed identity.
#
# Run via `cmake -DBUNDLE=<app> -DIDENTITY=<name> -P cmake/DevCodesign.cmake`.
#
# macOS keys camera / microphone / speech / Bluetooth permission grants (TCC) to
# the running binary's code signature, not just its bundle id.  An unsigned
# build has no durable identity to match, and an ad-hoc-signed one gets a fresh
# cdhash every rebuild — so either way a grant made in System Settings does not
# apply to the next run and every entitled API returns Denied, even though the
# app shows as "enabled" there.  Signing the finished bundle with a stable
# self-signed certificate gives TCC something constant to match, so a grant made
# once survives all subsequent rebuilds.
#
# Must run AFTER the bundle is otherwise final (the Info.plist force-copy in
# particular): any write into the bundle after codesign invalidates the seal.
# The release pipeline re-signs with Developer ID + notarises separately; this is
# strictly the local dev loop.

# The identity lives in a DEDICATED keychain, not the login one, and this block
# unlocks it before we go looking.  The login keychain can only be unlocked by
# PROMPTING, so any build not driven from a GUI desktop session -- an agent or CI
# job over SSH, which launchd reports as session `Background` rather than `Aqua`
# -- cannot use it: codesign fails `errSecInternalComponent` with no way to ask.
# A keychain whose password sits on disk unlocks with no interaction at all,
# which is the same reason fastlane's create_keychain exists.  Set up once by
# tools/setup_dev_signing.sh.
#
# The keychain must also be on the SEARCH LIST, and ahead of login.keychain-db:
# codesign resolves `-s <name>` through the search list in order (its own
# --keychain flag does NOT affect identity lookup -- it reports "no identity
# found"), so a stale copy of the same identity in an earlier keychain is picked
# first and fails.  The setup script puts this one first and removes the login
# copy; nothing here needs to re-check that.
#
# Absent either file this is a silent no-op and we fall through to whatever the
# search list already offers -- a GUI-session build with the cert still in the
# login keychain keeps working exactly as before.
#
# The password reaches `security` as an argv element, so it is briefly visible in
# `ps` to this user.  Accepted: it guards a self-signed key whose only power is
# making local TCC grants stick, it is a throwaway that can be regenerated in one
# command, and the file itself is 0600.  Nothing here touches the Developer ID
# path in tools/package_macos.sh, which still signs from the login keychain in a
# real desktop session.
set(_pp_kc     "$ENV{HOME}/Library/Keychains/pinpoint-dev.keychain-db")
set(_pp_kcpass "$ENV{HOME}/.config/pinpoint/dev-keychain.pass")
if(EXISTS "${_pp_kc}" AND EXISTS "${_pp_kcpass}")
    file(READ "${_pp_kcpass}" _pp_pw)
    string(STRIP "${_pp_pw}" _pp_pw)
    execute_process(
        COMMAND security unlock-keychain -p "${_pp_pw}" "${_pp_kc}"
        RESULT_VARIABLE _pp_unlock_rc
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT _pp_unlock_rc EQUAL 0)
        # Not fatal on its own: say so and let the codesign below produce the real
        # error, which names the actual failure better than a guess here would.
        message(WARNING "Could not unlock ${_pp_kc} (exit ${_pp_unlock_rc}) — signing may fail.")
    endif()
endif()

# security find-identity lists signing certs as e.g. `1) <hash> "Name"`.  We do
# NOT pass -v ("valid only"): a self-signed dev cert is an untrusted root, so -v
# hides it (it lists as `"Name" (CSSMERR_TP_NOT_TRUSTED)`).  codesign signs with
# it regardless of trust, and TCC keys the grant to the signature either way, so
# trust is irrelevant here — we only need the identity to exist.
execute_process(
    COMMAND security find-identity -p codesigning
    OUTPUT_VARIABLE _identities
    ERROR_QUIET)

if(_identities MATCHES "\"${IDENTITY}\"")
    execute_process(
        COMMAND codesign --force --deep --sign "${IDENTITY}" "${BUNDLE}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "codesign failed (exit ${_rc}) signing ${BUNDLE}")
    endif()
    message(STATUS "Signed ${BUNDLE} with '${IDENTITY}' (TCC-stable dev identity)")
else()
    message(WARNING
        "Dev signing identity '${IDENTITY}' not found in keychain — bundle left unsigned.\n"
        "  Camera / microphone / speech / Bluetooth grants will NOT persist across\n"
        "  rebuilds until a stable signing certificate exists.  Create one once:\n"
        "    Keychain Access ▸ Certificate Assistant ▸ Create a Certificate…\n"
        "      Name: ${IDENTITY}\n"
        "      Identity Type: Self Signed Root\n"
        "      Certificate Type: Code Signing\n"
        "  Then rebuild and grant each permission once; the grants then stick.\n"
        "  Override the name with -DPINPOINT_DEV_CODESIGN_IDENTITY=<name>.")
endif()
