// Standalone test for the shared frame decode (src/Export/frame_decode) — the
// ONE payload→cv::Mat path used by both SwingExporter and the shot analyzer.
// Synthetic NV12 / YUV420P / BGRA32 / Bayer8 payloads with known pixel values;
// golden-pixel checks, geometry/stride handling, false-on-short/unsupported.
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure

#include "../../Export/frame_decode.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pinpoint;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static CameraFormat makeFormat(PixelFormat pf, int w, int h, uint32_t stride0 = 0)
{
    CameraFormat fmt;
    fmt.pixel_format     = pf;
    fmt.width            = static_cast<uint32_t>(w);
    fmt.height           = static_cast<uint32_t>(h);
    fmt.plane_strides[0] = stride0;
    return fmt;
}

static const std::byte *bytesOf(const std::vector<unsigned char> &v)
{
    return reinterpret_cast<const std::byte *>(v.data());
}

int main()
{
    // ── BGRA32: exact passthrough goldens + stride padding ──────────────────
    std::printf("=== BGRA32 ===\n");
    {
        // 2×2, B/G/R/A per pixel.
        const std::vector<unsigned char> payload = {
            10, 20, 30, 255,   40, 50, 60, 255,    // row 0
            70, 80, 90, 255,  200, 150, 100, 255,  // row 1
        };
        const CameraFormat fmt = makeFormat(PixelFormat::BGRA32, 2, 2);

        size_t stride = 0, minBytes = 0;
        check(frameGeometry(fmt, stride, minBytes), "frameGeometry ok");
        check(stride == 8 && minBytes == 16, "stride 8, minBytes 16");

        cv::Mat bgr;
        check(decodeToBgr(fmt, bytesOf(payload), payload.size(), bgr), "decodeToBgr ok");
        check(bgr.type() == CV_8UC3 && bgr.cols == 2 && bgr.rows == 2, "BGR 2x2 CV_8UC3");
        check(bgr.at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 20, 30),    "pixel (0,0) == 10,20,30");
        check(bgr.at<cv::Vec3b>(0, 1) == cv::Vec3b(40, 50, 60),    "pixel (0,1) == 40,50,60");
        check(bgr.at<cv::Vec3b>(1, 1) == cv::Vec3b(200, 150, 100), "pixel (1,1) == 200,150,100");

        // Luma — must equal OpenCV's own BGRA→GRAY of the same pixels.
        cv::Mat luma, ref;
        cv::cvtColor(cv::Mat(2, 2, CV_8UC4, const_cast<unsigned char *>(payload.data())),
                     ref, cv::COLOR_BGRA2GRAY);
        check(decodeToLuma(fmt, bytesOf(payload), payload.size(), luma), "decodeToLuma ok");
        check(luma.type() == CV_8UC1 && cv::countNonZero(luma != ref) == 0,
              "luma == BGRA2GRAY reference");

        // Padded rows: same pixels at stride 12 (4 pad bytes per row).
        const std::vector<unsigned char> padded = {
            10, 20, 30, 255,   40, 50, 60, 255,   0, 0, 0, 0,
            70, 80, 90, 255,  200, 150, 100, 255, 0, 0, 0, 0,
        };
        const CameraFormat pfmt = makeFormat(PixelFormat::BGRA32, 2, 2, 12);
        cv::Mat pbgr;
        check(decodeToBgr(pfmt, bytesOf(padded), padded.size(), pbgr), "padded decode ok");
        check(pbgr.at<cv::Vec3b>(1, 1) == cv::Vec3b(200, 150, 100), "padded pixel (1,1) correct");

        check(!decodeToBgr(fmt, bytesOf(payload), payload.size() - 1, bgr),
              "short payload rejected");
        check(!decodeToBgr(fmt, nullptr, payload.size(), bgr), "null payload rejected");
    }

    // ── NV12: Y gradient + strong-red chroma; planar 3/2-rows geometry ──────
    std::printf("=== NV12 ===\n");
    {
        // 4×4 Y plane (i*16 gradient) + one 2-row interleaved UV plane.
        std::vector<unsigned char> payload(4 * 4 + 4 * 2);
        for (int i = 0; i < 16; ++i)
            payload[i] = static_cast<unsigned char>(i * 16);
        for (int i = 16; i < 24; i += 2) { payload[i] = 90; payload[i + 1] = 240; }  // U=90, V=240
        const CameraFormat fmt = makeFormat(PixelFormat::NV12, 4, 4);

        size_t stride = 0, minBytes = 0;
        check(frameGeometry(fmt, stride, minBytes), "frameGeometry ok");
        check(stride == 4 && minBytes == 24, "stride 4, minBytes 24 (3/2 rows)");

        cv::Mat bgr;
        check(decodeToBgr(fmt, bytesOf(payload), payload.size(), bgr), "decodeToBgr ok");
        check(bgr.type() == CV_8UC3 && bgr.cols == 4 && bgr.rows == 4, "BGR 4x4 CV_8UC3");

        // Hand golden at (row 2, col 0): Y=128, U=90, V=240 (BT.601 studio swing)
        //   R = 1.164·112 + 1.596·112 ≈ 309 → 255 (clamped)
        //   G = 1.164·112 − 0.813·112 − 0.391·(−38) ≈ 54
        //   B = 1.164·112 + 2.018·(−38) ≈ 54
        const cv::Vec3b px = bgr.at<cv::Vec3b>(2, 0);
        check(px[2] == 255, "golden pixel R clamps to 255");
        check(std::abs(px[1] - 54) <= 8 && std::abs(px[0] - 54) <= 8,
              "golden pixel G/B ~= 54");
        check(px[2] > px[0] + 100, "red chroma dominates (UV plane order correct)");

        // Whole-frame agreement with OpenCV's NV12 conversion of the same bytes.
        cv::Mat ref;
        cv::cvtColor(cv::Mat(6, 4, CV_8UC1, payload.data()), ref, cv::COLOR_YUV2BGR_NV12);
        std::vector<cv::Mat> diff(3);
        cv::split(bgr != ref, diff);
        check(cv::countNonZero(diff[0]) + cv::countNonZero(diff[1])
                  + cv::countNonZero(diff[2]) == 0,
              "matches COLOR_YUV2BGR_NV12 reference");

        // Luma fast path: exact Y-plane bytes, zero-copy alias of the payload.
        cv::Mat luma;
        check(decodeToLuma(fmt, bytesOf(payload), payload.size(), luma), "decodeToLuma ok");
        check(luma.type() == CV_8UC1 && luma.cols == 4 && luma.rows == 4, "luma 4x4 CV_8UC1");
        check(std::memcmp(luma.data, payload.data(), 16) == 0, "luma == Y plane bytes");
        check(luma.data == payload.data(), "luma wraps payload (zero-copy)");

        check(!decodeToBgr(fmt, bytesOf(payload), 23, bgr), "short payload rejected");
        check(!decodeToLuma(fmt, bytesOf(payload), 23, luma), "short payload rejected (luma)");
    }

    // ── YUV420P (I420): same Y, planar U then V ─────────────────────────────
    std::printf("=== YUV420P ===\n");
    {
        std::vector<unsigned char> payload(4 * 4 + 4 + 4);
        for (int i = 0; i < 16; ++i)
            payload[i] = static_cast<unsigned char>(i * 16);
        for (int i = 16; i < 20; ++i) payload[i] = 90;    // U plane
        for (int i = 20; i < 24; ++i) payload[i] = 240;   // V plane
        const CameraFormat fmt = makeFormat(PixelFormat::YUV420P, 4, 4);

        cv::Mat bgr;
        check(decodeToBgr(fmt, bytesOf(payload), payload.size(), bgr), "decodeToBgr ok");
        const cv::Vec3b px = bgr.at<cv::Vec3b>(2, 0);   // same hand golden as NV12
        check(px[2] == 255 && std::abs(px[1] - 54) <= 8 && std::abs(px[0] - 54) <= 8,
              "golden pixel ~= (54, 54, 255)");
        check(px[2] > px[0] + 100, "red chroma dominates (U/V plane order correct)");

        cv::Mat luma;
        check(decodeToLuma(fmt, bytesOf(payload), payload.size(), luma), "decodeToLuma ok");
        check(std::memcmp(luma.data, payload.data(), 16) == 0, "luma == Y plane bytes");
        check(luma.data == payload.data(), "luma wraps payload (zero-copy)");
    }

    // ── 8-bit Bayer: constant mosaic demosaics to the constant colour ───────
    std::printf("=== BayerRG8 ===\n");
    {
        const int w = 8, h = 8;
        std::vector<unsigned char> payload(static_cast<size_t>(w) * h, 100);
        const CameraFormat fmt = makeFormat(PixelFormat::BayerRG8, w, h);

        cv::Mat bgr;
        check(decodeToBgr(fmt, bytesOf(payload), payload.size(), bgr), "decodeToBgr ok");
        check(bgr.type() == CV_8UC3 && bgr.cols == w && bgr.rows == h, "BGR 8x8 CV_8UC3");
        check(bgr.at<cv::Vec3b>(3, 3) == cv::Vec3b(100, 100, 100)
                  && bgr.at<cv::Vec3b>(0, 0) == cv::Vec3b(100, 100, 100),
              "constant mosaic -> constant (100,100,100)");

        // Luma = raw mosaic wrap (documented grey approximation), zero-copy.
        cv::Mat luma;
        check(decodeToLuma(fmt, bytesOf(payload), payload.size(), luma), "decodeToLuma ok");
        check(luma.type() == CV_8UC1 && luma.at<unsigned char>(4, 4) == 100,
              "luma == raw mosaic value");
        check(luma.data == payload.data(), "luma wraps payload (zero-copy)");

        check(!decodeToBgr(fmt, bytesOf(payload), payload.size() - 1, bgr),
              "short payload rejected");
    }

    // ── Unsupported formats / degenerate geometry ───────────────────────────
    std::printf("=== unsupported ===\n");
    {
        const std::vector<unsigned char> payload(64, 0);
        cv::Mat out;
        for (PixelFormat pf : { PixelFormat::MJPEG, PixelFormat::H264_NAL,
                                PixelFormat::BayerRG12, PixelFormat::Mono16,
                                PixelFormat::Unknown }) {
            const CameraFormat fmt = makeFormat(pf, 4, 4);
            size_t s = 0, m = 0;
            if (decodeToBgr(fmt, bytesOf(payload), payload.size(), out)
                || decodeToLuma(fmt, bytesOf(payload), payload.size(), out)
                || frameGeometry(fmt, s, m)) {
                check(false, "unsupported format rejected");
            }
        }
        check(true, "MJPEG/H264/Bayer12/Mono16/Unknown all rejected");

        const CameraFormat zero = makeFormat(PixelFormat::NV12, 0, 0);
        size_t s = 0, m = 0;
        check(!frameGeometry(zero, s, m) && !decodeToBgr(zero, bytesOf(payload), payload.size(), out),
              "degenerate dimensions rejected");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
