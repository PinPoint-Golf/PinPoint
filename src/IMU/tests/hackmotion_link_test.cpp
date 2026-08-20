// Build-dependency smoke test for libwrist (github.com/PinPoint-Golf/libwrist).
// Run via CTest (src/IMU/tests/CMakeLists.txt):
//   cmake -S src/IMU/tests -B build/imu-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.0/gcc_64
//   cmake --build build/imu-tests && ctest --test-dir build/imu-tests --output-on-failure
//
// WHY THIS EXISTS. Until the HackMotion integration lands, no PinPoint code
// references a single HackMotion symbol — so a library that configures and
// compiles but fails to LINK would go completely unnoticed on any platform, and
// the failure would surface mid-integration instead of at the point the
// dependency was added. This is the only thing standing between "we added the
// dependency" and knowing it actually works, so it deliberately calls across the
// C↔C++ boundary rather than merely including a header.
//
// It is also the guard against a class of failure specific to a dependency that
// tracks a branch and can be overridden by a local checkout: HEADERS FROM ONE
// BUILD AND AN ARCHIVE FROM ANOTHER. wr_abi_check() compares this compiler's
// view of every public struct against the sizes the library was built with,
// which is exactly that skew — and, incidentally, any MSVC/gcc/clang layout
// disagreement in the POD types PinPoint will be copying out of the ring.

#include <wrist/wrist.h>

#include <cstdio>
#include <cstring>

static int g_fail = 0;

#define CHECK(label, cond)                                                    \
    do {                                                                      \
        const bool ok = (cond);                                               \
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);              \
        if (!ok) ++g_fail;                                                    \
    } while (0)

int main()
{
    std::printf("libwrist link/ABI smoke test\n");
    std::printf("  header version: %s (ABI %u)\n", WR_VERSION_STRING,
                (unsigned)WR_ABI_VERSION);

    // ── The command allowlist is live in the linked archive ──────────────────
    // ⛔ 0xf0 reboots the sensor into firmware-update mode, and it reaches that
    // mode through the ORDINARY data characteristic — so avoiding the OTA
    // service is not sufficient on its own. The library refuses it by
    // construction and there is no sendRaw(). This is the one property that
    // must never regress silently, in any build, on any platform.
    {
        std::printf("\nCommand allowlist\n");
        CHECK("0xf0 (firmware-update reboot) is refused",
              wr_command_is_allowed(0xF0) == false);

        uint8_t allow[32] = {0};
        const size_t n = wr_command_allowlist(allow, sizeof(allow));
        CHECK("allowlist is non-empty", n > 0);
        CHECK("allowlist fits the probe buffer", n <= sizeof(allow));

        bool sawF0 = false;
        bool allSelfConsistent = true;
        for (size_t i = 0; i < n && i < sizeof(allow); ++i) {
            if (allow[i] == 0xF0) sawF0 = true;
            if (!wr_command_is_allowed(allow[i])) allSelfConsistent = false;
        }
        CHECK("0xf0 is absent from the enumerated allowlist", !sawF0);
        CHECK("every enumerated byte passes the gate", allSelfConsistent);
    }

    // ── Header/library agreement ─────────────────────────────────────────────
    // The compiled string comes from the archive; the macro comes from the
    // headers on our include path. They disagree when a stale _deps copy is
    // linked against fresh headers, or vice versa.
    {
        std::printf("\nHeader/library agreement\n");
        const char *runtime = wr_version_string();
        CHECK("wr_version_string() is non-null", runtime != nullptr);
        CHECK("compiled version matches the header macro",
              runtime && std::strcmp(runtime, WR_VERSION_STRING) == 0);
        CHECK("wr_abi_version() matches WR_ABI_VERSION",
              wr_abi_version() == (uint32_t)WR_ABI_VERSION);

        // Filled from OUR headers, as the contract asks, and compared against
        // the sizes the library recorded at its own build time.
        wr_abi_sizes expected;
        std::memset(&expected, 0, sizeof(expected));
        expected.abi_version           = (uint32_t)WR_ABI_VERSION;
        expected.sample                = (uint32_t)sizeof(wr_sample);
        expected.unit_sample           = (uint32_t)sizeof(wr_unit_sample);
        expected.event                 = (uint32_t)sizeof(wr_event);
        expected.clock_snapshot        = (uint32_t)sizeof(wr_clock_snapshot);
        expected.history_block         = (uint32_t)sizeof(wr_history_block);
        expected.write_request         = (uint32_t)sizeof(wr_write_request);
        expected.wire_chunk            = (uint32_t)sizeof(wr_wire_chunk);
        expected.session_config        = (uint32_t)sizeof(wr_session_config);
        expected.sample_layout_version = WR_SAMPLE_LAYOUT_VERSION;

        const wr_status abi = wr_abi_check(&expected);
        if (abi < WR_OK) {
            wr_abi_sizes actual;
            std::memset(&actual, 0, sizeof(actual));
            wr_abi_sizes_get(&actual);
            std::printf("    ABI mismatch — header vs library struct sizes:\n"
                        "      sample %u/%u  unit_sample %u/%u  event %u/%u\n"
                        "      clock_snapshot %u/%u  history_block %u/%u\n"
                        "      write_request %u/%u  wire_chunk %u/%u\n"
                        "      session_config %u/%u  layout_version %u/%u\n",
                        expected.sample, actual.sample,
                        expected.unit_sample, actual.unit_sample,
                        expected.event, actual.event,
                        expected.clock_snapshot, actual.clock_snapshot,
                        expected.history_block, actual.history_block,
                        expected.write_request, actual.write_request,
                        expected.wire_chunk, actual.wire_chunk,
                        expected.session_config, actual.session_config,
                        expected.sample_layout_version, actual.sample_layout_version);
        }
        CHECK("wr_abi_check() accepts this compiler's struct layout", abi >= WR_OK);
    }

    // ── Real symbol resolution, not just a header include ────────────────────
    {
        std::printf("\nSymbol resolution\n");
        const char *s = wr_status_str(WR_ERR_MTU_TOO_SMALL);
        CHECK("wr_status_str() returns a non-empty string", s && s[0] != '\0');
        CHECK("wr_status_str() is stable across calls",
              s == wr_status_str(WR_ERR_MTU_TOO_SMALL));
        // WR_PENDING is a SUCCESS code — the sign test is the documented one,
        // and getting it wrong turns "poll again later" into an error path.
        CHECK("WR_PENDING is not an error", WR_PENDING >= WR_OK);
        CHECK("WR_ERR_LINK_DOWN is an error", WR_ERR_LINK_DOWN < WR_OK);
    }

    // ── POD round-trip across the C/C++ boundary ─────────────────────────────
    {
        std::printf("\nPOD round-trip\n");
        // The wG3's advertised name is a library constant, not a PinPoint one —
        // the enumerator will match on it, so prove it is reachable from here.
        CHECK("WR_ADVERTISED_LOCAL_NAME is reachable",
              WR_ADVERTISED_LOCAL_NAME[0] != '\0');

        wr_uuid uuid;
        std::memset(&uuid, 0, sizeof(uuid));
        const char *canonical = "0000fe80-8e22-4541-9d4c-21edae82ed19";
        CHECK("wr_uuid_parse() accepts the canonical form",
              wr_uuid_parse(canonical, &uuid) == WR_OK);

        char out[WR_UUID_STRING_SIZE] = {0};
        CHECK("wr_uuid_format() returns its output buffer",
              wr_uuid_format(&uuid, out, sizeof(out)) == out);
        CHECK("uuid round-trips byte-identically",
              std::strcmp(out, canonical) == 0);

        wr_uuid again;
        std::memset(&again, 0, sizeof(again));
        CHECK("re-parsing the formatted uuid yields an equal value",
              wr_uuid_parse(out, &again) == WR_OK && wr_uuid_equal(&uuid, &again));
        CHECK("wr_uuid_parse() rejects a malformed string",
              wr_uuid_parse("not-a-uuid", &again) == WR_ERR_INVALID_ARG);
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "OK",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
