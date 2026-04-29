# STT Audit — PinPoint

Audit date: 2026-04-29  
Branch: main (commit 7cccb49)

---

## 1. TranscriptionController — source files

| File | Path |
|------|------|
| Header | `Gui/transcription_controller.h` |
| Implementation | `Gui/transcription_controller.cpp` |

The class is named **TranscriptionController** (not TranscriptionClass).

### Public interface

```cpp
class TranscriptionController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString transcript READ transcript NOTIFY transcriptChanged)

public:
    explicit TranscriptionController(QObject *parent = nullptr);
    ~TranscriptionController() override;

    QString transcript() const;   // returns full accumulated transcript

signals:
    void transcriptChanged();     // fired every time new text is appended

private slots:
    void onTranscriptionReceived(const QString &text);
    void onAudioError(const QString &message);
    void onSTTError(const QString &message);
    void startAudio();
};
```

There are no other public methods beyond the constructor, destructor, and the `transcript()` accessor.

### How results are returned to callers

- The WhisperProcessor (in its own thread) emits `transcriptionReceived(QString)`.
- `TranscriptionController::onTranscriptionReceived` appends the text to `m_transcript` and emits `transcriptChanged()`.
- QML binds to the `transcript` Q_PROPERTY; nothing is returned by value or via callback.

---

## 2. Audio data path

### Capture

`AudioInput` wraps Qt6 `QAudioSource`. It runs on `m_audioThread` (a `QThread`). When the internal `QIODevice` receives data it emits:

```cpp
void audioDataReady(QByteArray data, QAudioFormat format);
```

### Dual consumer fan-out (queued connections)

| Consumer | Purpose |
|----------|---------|
| `AudioStreamSaver` | Writes a WAV file to the Desktop for debugging |
| `WhisperProcessor` | Buffers PCM and dispatches it to the in-process `WhisperWorker` |

### Silence gate

`WhisperProcessor` computes RMS amplitude over the chunk. If RMS < 0.01 (≈ −40 dBFS) the chunk is discarded.

### Chunking and conversion

PCM is accumulated until `m_chunkDurationMs` (default **3 000 ms**) of audio is buffered, then `AudioConverter::toWhisperFormat()` converts it to 16 kHz mono float32 before dispatch.

---

## 3. ~~Wire format — audio sent to port 5001~~ (removed)

> **Superseded.** The HTTP client path (`QNetworkAccessManager`, `localhost:5001`, WAV multipart POST, JSON response parsing) was removed after the in-process whisper.cpp backend was validated. `Qt6::Network` is no longer a project dependency. Audio is now processed entirely in-process via `WhisperBackendWhisperCpp::transcribe()` on a dedicated `QThread`.
>
> For the original wire-format description (useful if re-enabling a server backend), see git history.

---

## 4. Threading model

```
Main thread
 └─ TranscriptionController
     ├─ m_audioThread   (QThread)
     │    └─ AudioInput  ── QAudioSource (platform audio thread inside Qt)
     └─ m_processorThread (QThread)
          └─ WhisperProcessor
               ├─ QTimer (chunk timer, 3 s)
               └─ QNetworkAccessManager (async HTTP)
```

- All cross-thread communication uses **queued signal/slot connections**.
- HTTP calls are **non-blocking** (`QNetworkAccessManager::post()`); the reply `finished` signal arrives on `m_processorThread`.
- An `m_requestInFlight` flag prevents overlapping POST requests.
- On macOS, microphone permission is requested asynchronously; `startAudio()` is invoked via `QMetaObject::invokeMethod` after permission is granted.

---

## 5. CMakeLists.txt findings

File: `CMakeLists.txt` (single file at project root, no subdirectory CMakeLists).

| Aspect | Finding |
|--------|---------|
| Target name | `PinPoint` |
| `cmake_minimum_required` | 3.16 |
| C++ standard | Not set explicitly; `qt_standard_project_setup(REQUIRES 6.10)` governs it (Qt 6.10 default is **C++17**) |
| Dependency management | `find_package(Qt6 ...)` only — **no FetchContent, vcpkg, Conan, or vendor directories** |
| Platform blocks | `if(APPLE)` only: adds `macos_permissions.mm`, links `-framework AVFoundation`, copies `Info.plist` |
| WIN32 / UNIX blocks | None (WIN32_EXECUTABLE property is set, but no conditional CMake logic) |

Qt6 components in use: `Quick`, `QuickControls2`, `SerialPort`, `Bluetooth`, `Multimedia`, `Network`.

---

## 6. Audio resampling — existing support

**None found.**

There is no libsamplerate, Qt Multimedia resampler, or custom resampler in the codebase. The `AudioInput` constructor explicitly avoids forced resampling with the comment:

> *"Leave m_preferredFormat default (invalid) so start() uses the device's own preferred format and the FFmpeg backend does no conversion. Forced resampling (e.g. 44 100 Hz Float → 16 000 Hz Int16) in small chunks produces aliasing artefacts on the built-in MacBook microphone."*

---

## 7. Summary

| Topic | Key fact |
|-------|----------|
| Public API surface | Constructor/destructor + `transcript()` accessor + `transcriptChanged()` signal |
| Result delivery | Qt signal → Q_PROPERTY → QML binding |
| Audio wire format | WAV (RIFF), device-native rate/channels/depth, no resampling |
| Silence gating | RMS < 0.01 drops the chunk |
| Chunk duration | 3 000 ms default |
| Inference | In-process `whisper_full()` on a dedicated `QThread`; HTTP server path removed |
| Threading | Two worker QThreads (audio, processor); main thread receives via queued signals |
| Build system | CMake 3.16 + Qt6 `find_package`; single CMakeLists; C++17 via Qt 6.10 defaults |
| Resampling library | **Not present** — adding one would require new CMake plumbing (FetchContent or system find_package) |

---

## 8. GGML model files

### Recommended default

| Model | Size | Notes |
|-------|------|-------|
| `ggml-base.en.bin` | ~74 MB | Fastest; lower accuracy |
| **`ggml-small.en.bin`** | **~150 MB** | **Recommended for PinPoint — good balance of speed and accuracy** |
| `ggml-medium.en.bin` | ~460 MB | Highest accuracy; slow on CPU without GPU acceleration |

Download the recommended model:

```
https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin
```

Place the file in any of the locations searched by `WhisperProcessor::resolveModelPath()` (see below). Model files are excluded from version control via `.gitignore` (`models/`, `*.bin`).

### Runtime search order (`WhisperProcessor::resolveModelPath`)

| Priority | Location | Purpose |
|----------|----------|---------|
| 1 | QSettings key `stt/modelPath` | Absolute path override — for power users and CI |
| 2 | `QStandardPaths::AppDataLocation/models/` | Per-user install (recommended for end users) |
| 3 | `<executable dir>/models/` | Alongside the binary — good for dev builds and portable installs |

Platform-specific paths for priority 2:

| Platform | Path |
|----------|------|
| Linux | `~/.local/share/PinPoint/models/` |
| macOS | `~/Library/Application Support/PinPoint/models/` |
| Windows | `%APPDATA%\PinPoint\models\` |

If no path resolves to an existing file, `WhisperProcessor` emits `modelNotFound(QStringList searchedPaths)` so the UI can prompt the user to download or locate a model.
