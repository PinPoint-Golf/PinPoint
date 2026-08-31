/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "pp_debug.h"
#include "PpMessageLog.h"

#include <whisper.h>
#include <ggml.h>

#ifdef PP_HAS_DLSYM
#include <dlfcn.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

// FFmpeg av_log capture is available wherever we can reach into an
// already-loaded libavutil at runtime: dlsym on UNIX, GetModuleHandle on
// Windows (no CMake define needed there — windows.h is always available).
#if defined(PP_HAS_DLSYM) || defined(_WIN32)
#define PP_HAS_AVLOG_CAPTURE 1
#endif

#include <QMediaPlayer>
#include <QMessageLogContext>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <streambuf>
#include <string>

// ── PpLogStream lives in pp_log_stream.cpp ───────────────────────────────────
// ⛔ AND IT IS NOT COMING BACK HERE.  This file includes whisper.h and ggml.h to
// silence their loggers in `install()`; the log PRIMITIVE needs neither, and
// welding the two together is what made six test suites link a stub that
// discards every log line rather than pay for a speech model.  See the header of
// pp_log_stream.cpp.

// ── Qt message handler ────────────────────────────────────────────────────────
// PinPoint Studio messages bypass this via PpLogStream. This handler suppresses
// high-volume Qt framework categories and routes Qt warnings/errors into
// PpMessageLog so they appear in the resource monitor's application log.

static void ppMessageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (ctx.category) {
        const QLatin1StringView cat(ctx.category);

        // PINPOINT_BLE_TRACE=1: emit the full qt.bluetooth category to stderr
        // (enable with QT_LOGGING_RULES="qt.bluetooth*=true"). The category is
        // suppressed below by default, which also hides Qt-side BLE *errors* —
        // this is the only way to see where a connection stalls in the field.
        static const bool bleTrace = qEnvironmentVariableIsSet("PINPOINT_BLE_TRACE");
        const bool isBle = cat.startsWith(QLatin1StringView("qt.bluetooth"));
        if (bleTrace && isBle) {
            fprintf(stderr, "[%s] %s\n", ctx.category, msg.toLocal8Bit().constData());
            return;
        }

        if (cat.startsWith(QLatin1StringView("qt.multimedia.ffmpeg"))
         || cat.startsWith(QLatin1StringView("qt.multimedia.playbackengine"))
         || cat.startsWith(QLatin1StringView("qt.core.qfuture"))
         || isBle)
            return;

#if PINPOINT_DEBUG_LEVEL < 3
        if (type < QtWarningMsg && cat.startsWith(QLatin1StringView("qt.")))
            return;
#endif
    }

    // Capture warnings and above into the in-app log regardless of debug level.
    // Prefix the logging category when it's not the anonymous default.
    if (type >= QtWarningMsg) {
        QString logMsg = msg;
        if (ctx.category && QLatin1StringView(ctx.category) != QLatin1StringView("default"))
            logMsg = QStringLiteral("[") + QString::fromLatin1(ctx.category) + QStringLiteral("] ") + msg;
        PpMessageLog::instance()->append(type, logMsg);
    }

#if PINPOINT_DEBUG_LEVEL == 0
    if (type == QtFatalMsg) ::abort();
    return;
#else
    const QByteArray localMsg = msg.toLocal8Bit();
    switch (type) {
    case QtDebugMsg:
    case QtInfoMsg:     fprintf(stderr, "%s\n",           localMsg.constData()); break;
    case QtWarningMsg:  fprintf(stderr, "WARNING: %s\n",  localMsg.constData()); break;
    case QtCriticalMsg: fprintf(stderr, "CRITICAL: %s\n", localMsg.constData()); break;
    case QtFatalMsg:    fprintf(stderr, "FATAL: %s\n",    localMsg.constData()); ::abort();
    }
#endif
}

// ── std::cerr capture ─────────────────────────────────────────────────────────
// OpenCV's logger (CV_LOG_WARNING / CV_LOG_ERROR — e.g. videoio's
// "[ WARN:0@1.234] global cap_v4l.cpp ..." lines) writes straight to std::cerr;
// the writer-replacement hook only exists from OpenCV 4.11, so on 4.10 the only
// way to capture it is to tee the stream itself.  cv::redirectError in main.cpp
// covers the CV_Error path; this covers the logging path.  Safe against
// recursion: all PinPoint/Qt/FFmpeg console output uses stdio (fprintf), which
// bypasses iostream streambufs.  STTBackendWhisperCpp's temporary
// devnull-rdbuf swap during model load saves and restores this buffer — fine.

namespace {

class CerrCaptureBuf : public std::streambuf
{
public:
    explicit CerrCaptureBuf(std::streambuf *fwd) : m_fwd(fwd) {}

protected:
    int overflow(int ch) override
    {
        if (ch == traits_type::eof())
            return 0;
        const char c = traits_type::to_char_type(ch);
        ingest(&c, 1);
        return ch;
    }

    std::streamsize xsputn(const char *s, std::streamsize n) override
    {
        ingest(s, n);
        return n;
    }

    int sync() override { return m_fwd ? m_fwd->pubsync() : 0; }

private:
    void ingest(const char *s, std::streamsize n)
    {
#if PINPOINT_DEBUG_LEVEL > 0
        if (m_fwd)
            m_fwd->sputn(s, n);
#endif
        // Writers may emit partial lines — accumulate per thread (OpenCV logs
        // from capture threads) and flush one PpMessageLog entry per full line.
        thread_local std::string pending;
        pending.append(s, size_t(n));

        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            emitLine(QString::fromUtf8(pending.data(), qsizetype(nl)).trimmed());
            pending.erase(0, nl + 1);
        }
    }

    static void emitLine(const QString &line)
    {
        if (line.isEmpty())
            return;

        // OpenCV log lines carry their severity as "[LEVEL:thread@time] ..."
        // (level padded to 5 chars: "[ WARN", "[ERROR", "[FATAL", "[ INFO").
        QtMsgType type = QtWarningMsg;
        QString   tag  = QStringLiteral("[stderr] ");
        if (line.startsWith(QLatin1StringView("[ERROR:"))
         || line.startsWith(QLatin1StringView("[FATAL:"))) {
            type = QtCriticalMsg;
            tag  = QStringLiteral("[OpenCV] ");
        } else if (line.startsWith(QLatin1StringView("[ WARN:"))) {
            tag  = QStringLiteral("[OpenCV] ");
        } else if (line.startsWith(QLatin1StringView("[ INFO:"))) {
            type = QtInfoMsg;
            tag  = QStringLiteral("[OpenCV] ");
        } else if (line.startsWith(QLatin1StringView("[DEBUG:"))
                || line.startsWith(QLatin1StringView("[VERB:"))) {
            return;
        }
        PpMessageLog::instance()->append(type, tag + line);
    }

    std::streambuf *m_fwd;
};

} // namespace

static void installCerrCapture()
{
    static CerrCaptureBuf s_buf(std::cerr.rdbuf());
    std::cerr.rdbuf(&s_buf);
}

// ── Dependency log suppressors ────────────────────────────────────────────────

static void silentLogCallback(ggml_log_level, const char *, void *) {}

#ifdef PP_HAS_AVLOG_CAPTURE
// ── FFmpeg av_log capture ─────────────────────────────────────────────────────
// Routes libavutil warnings/errors into PpMessageLog instead of stderr.
// Anything below warning (the "Input #0, mov,mp4,..." stream dumps, codec
// banners, etc.) is swallowed — neither logged nor printed.  Numeric
// constants mirror libavutil/log.h — no header dependency.

static constexpr int kAvLogError   = 16;
static constexpr int kAvLogWarning = 24;

static void ffmpegLogCallback(void *, int level, const char *fmt, va_list vl)
{
    // Custom callbacks receive every message regardless of av_log_set_level —
    // filter here.  Drop info/verbose/debug/trace: FFmpeg's INFO (stream
    // dumps, codec banners) is far too chatty for the app log; only
    // warnings and errors are worth surfacing.
    if (level > kAvLogWarning)
        return;

    char chunk[1024];
    vsnprintf(chunk, sizeof chunk, fmt, vl);

    // av_log emits partial lines (the stream dump is built from many calls) —
    // accumulate per thread and flush one PpMessageLog entry per full line.
    thread_local QString pending;
    pending += QString::fromUtf8(chunk);

    qsizetype nl;
    while ((nl = pending.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = pending.first(nl).trimmed();
        pending.remove(0, nl + 1);
        if (line.isEmpty())
            continue;

        const QtMsgType type = (level <= kAvLogError) ? QtCriticalMsg : QtWarningMsg;
        PpMessageLog::instance()->append(type, QStringLiteral("[FFmpeg] ") + line);
    }
}

// Install ffmpegLogCallback on every loaded libavutil instance.  On UNIX, TWO
// instances coexist: the app binary links the system copy (.so.60, via the
// swing-export encoder) which is loaded before main(), while Qt Multimedia's
// FFmpeg plugin links the Qt-bundled copy (.so.59).  The callback pointer
// lives in static storage inside each instance, so both must be set — do not
// stop at the first one found.  On Windows the loader dedupes unqualified
// module names, so usually one instance serves both, but probe every
// plausible name anyway.
static void installAvLogCallbackOnAllInstances()
{
    using SetCb = void(*)(void(*)(void *, int, const char *, va_list));
#ifdef _WIN32
    static const char * const kLibs[] = {
        "avutil-59.dll",        // FFmpeg 7.x (Qt 6.8–6.11 bundle + exporter)
        "avutil-58.dll",        // FFmpeg 6.x
        "avutil-60.dll",        // FFmpeg 8.x
        "avutil-57.dll",        // FFmpeg 5.x
        "avutil.dll",
        nullptr
    };
    for (int i = 0; kLibs[i]; ++i) {
        // Only instances that are already loaded — GetModuleHandle never loads.
        HMODULE h = GetModuleHandleA(kLibs[i]);
        if (!h)
            continue;
        if (auto fn = reinterpret_cast<SetCb>(GetProcAddress(h, "av_log_set_callback")))
            fn(&ffmpegLogCallback);
    }
#else
    static const char * const kLibs[] = {
        "libavutil.so.59",      // Qt 6.7–6.11
        "libavutil.so.58",      // Qt 6.4–6.6
        "libavutil.so.60",      // system
        "libavutil.so",
        "libavutil.59.dylib",   // macOS Qt bundle
        "libavutil.58.dylib",
        "libavutil.dylib",
        nullptr
    };

    for (int i = 0; kLibs[i]; ++i) {
        // Only instances that are already loaded — RTLD_NOLOAD never loads.
        void *h = dlopen(kLibs[i], RTLD_LAZY | RTLD_NOLOAD);
        if (!h)
            continue;
        if (auto fn = reinterpret_cast<SetCb>(dlsym(h, "av_log_set_callback")))
            fn(&ffmpegLogCallback);
        dlclose(h);   // balance the RTLD_NOLOAD refcount; instance stays loaded
    }
#endif
}
#endif // PP_HAS_AVLOG_CAPTURE

void PinPointDebug::install()
{
    qInstallMessageHandler(ppMessageHandler);

    // ⚠ THE GUI'S LOG IS OTHERWISE ONLY READABLE BY HAND.  Everything routed
    // here lands in PpMessageLog's ring buffer, which surfaces in Resource
    // Monitor -> Message Log and its Export button — fine for a person, useless
    // for anything driving the application from a terminal, which sees a silent
    // process.  `swinglab_run` was the only caller that ever turned the echo on.
    //
    // Opt-in and off by default, because the echo is unconditional once set and
    // a shipped app should not be writing to a stream nobody reads.
    if (!qEnvironmentVariableIsEmpty("PINPOINT_LOG_STDERR"))
        PpMessageLog::instance()->setEchoToStderr(true);

    // Capture iostream stderr writers (OpenCV's logger) into PpMessageLog.
    installCerrCapture();

    whisper_log_set(silentLogCallback, nullptr);
    ggml_log_set(silentLogCallback, nullptr);

    // Capture FFmpeg's av_log output into PpMessageLog.  This early pass only
    // reaches instances already loaded before main() (the libavutil linked by
    // the swing-export encoder).  The Qt-bundled instance is handled by
    // installFfmpegLogCapture() once QGuiApplication exists.
#ifdef PP_HAS_AVLOG_CAPTURE
    installAvLogCallbackOnAllInstances();
#endif
}

void PinPointDebug::installFfmpegLogCapture()
{
#ifdef PP_HAS_AVLOG_CAPTURE
    // Force the Qt Multimedia FFmpeg plugin to load NOW.  Its integration
    // constructor (setupFFmpegLogger in qffmpegmediaintegration.cpp)
    // unconditionally calls av_log_set_callback — without QT_FFMPEG_DEBUG set,
    // its callback delegates to av_log_default_callback, i.e. raw stderr.
    // Constructing a throwaway QMediaPlayer creates the integration
    // deterministically; afterwards our capture callback wins the race.
    { QMediaPlayer probe; }

    installAvLogCallbackOnAllInstances();
#endif
}
