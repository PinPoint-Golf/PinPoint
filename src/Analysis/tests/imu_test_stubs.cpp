// Minimal symbol providers so the IMU driver/filter sources link in the standalone
// analyzer test harness WITHOUT pulling in the Buffer library or the
// whisper-dependent pp_debug.cpp (Track A, Phase 0.0 provisioning).
//
//   * EventBuffer::nowMicros() — referenced by ImuBase::fuseRawImu() (the per-sample
//     dt clock). The frame-parse / eulerToQuat goldens don't fuse, but the symbol is
//     needed at link time. A deterministic monotonic fake keeps any incidental fusion
//     reproducible.
//
// ⚠ IT NO LONGER PROVIDES PpLogStream, AND THAT IS THE POINT.  Swallowing the
// log was the price of avoiding pp_debug.cpp's whisper/ggml installer; ppWarn()
// now lives in src/Core/pp_log_stream.cpp, which costs Qt and PpMessageLog and
// nothing else, so the suites carry the REAL log and a test can assert on a line.

#include "event_buffer.h"

namespace pinpoint {
int64_t EventBuffer::nowMicros() noexcept
{
    static int64_t t = 0;
    t += 5000;          // 5 ms per call — deterministic, monotonic
    return t;
}
} // namespace pinpoint
