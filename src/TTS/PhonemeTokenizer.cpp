#include "PhonemeTokenizer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

#ifdef HAVE_ESPEAK_NG
#  include <espeak-ng/speak_lib.h>
#  include <cstdio>
#  include <cstdlib>
#  ifdef Q_OS_WIN
#    include <io.h>
#    include <fcntl.h>
#  endif
#endif

static QMutex s_espeakMutex;

// ---------------------------------------------------------------------------

PhonemeTokenizer::~PhonemeTokenizer()
{
#ifdef HAVE_ESPEAK_NG
    if (m_initialised)
        espeak_Terminate();
#endif
}

bool PhonemeTokenizer::initialise(const QString &tokensJsonPath)
{
    m_initialised = false;

    // ---- Vocabulary ---------------------------------------------------------
    QFile f(tokensJsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("Cannot open tokens file: ") + tokensJsonPath;
        return false;
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (doc.isNull()) {
        m_lastError = QStringLiteral("Failed to parse tokenizer.json: ") + parseErr.errorString();
        return false;
    }

    // HuggingFace fast-tokenizer: { "model": { "vocab": { "<sym>": id } } }
    const QJsonObject root = doc.object();
    const QJsonObject vocabObj = root.contains(QStringLiteral("model"))
        ? root[QStringLiteral("model")].toObject()[QStringLiteral("vocab")].toObject()
        : root;

    if (vocabObj.isEmpty()) {
        m_lastError = QStringLiteral("tokenizer.json: vocab is empty or not found");
        return false;
    }

    m_vocab.reserve(vocabObj.size());
    for (auto it = vocabObj.constBegin(); it != vocabObj.constEnd(); ++it)
        m_vocab.insert(it.key(), static_cast<int64_t>(it.value().toInt()));

    qDebug() << "[Tokenizer] vocab loaded:" << m_vocab.size() << "entries";

    // ---- Initialise espeak-ng -----------------------------------------------
#ifdef HAVE_ESPEAK_NG
    // On macOS the data directory is bundled in Contents/Resources.
    // On Linux with a system espeak-ng, nullptr uses the compiled-in path.
    // espeakINITIALIZE_DONT_EXIT prevents espeak-ng from calling exit(1) on
    // failure (which would crash the whole app from a worker thread).
#ifdef Q_OS_MACOS
    const QByteArray espeakData =
        (QCoreApplication::applicationDirPath() + "/../Resources").toLocal8Bit();
    const char *espeakDataPath = espeakData.constData();
#elif defined(ESPEAK_DATA_PATH)
    // Set by CMake when espeak-ng is built from source; the system library
    // (ESPEAK_FOUND) knows its own data path so nullptr is correct for that case.
    const char *espeakDataPath = ESPEAK_DATA_PATH;
#else
    const char *espeakDataPath = nullptr;
#endif
    if (espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0,
                          espeakDataPath, espeakINITIALIZE_DONT_EXIT) < 0) {
        m_lastError = QStringLiteral("espeak_Initialize failed: data not found");
        return false;
    }
    espeak_SetVoiceByName("en-us");
#else
    m_lastError = QStringLiteral("Built without espeak-ng");
    return false;
#endif

    m_initialised = true;
    return true;
}

QVector<int64_t> PhonemeTokenizer::tokenise(const QString &text) const
{
    if (!m_initialised)
        return {};

    const QString phonemes = textToPhonemes(text);
    if (phonemes.isEmpty())
        return {};

    return phonemesToIds(phonemes);
}

QString PhonemeTokenizer::textToPhonemes(const QString &text) const
{
#ifndef HAVE_ESPEAK_NG
    Q_UNUSED(text)
    return {};
#else
    QMutexLocker lock(&s_espeakMutex);

    // espeak_SetPhonemeTrace writes IPA phoneme names to a FILE* stream.
    // Mode bit 1 = output phoneme names; bit 2 = use IPA.
    // We capture them into a memory buffer via open_memstream (POSIX) or a
    // temporary file (Windows).

#  ifndef Q_OS_WIN
    char  *buf     = nullptr;
    size_t bufSize = 0;
    FILE  *stream  = open_memstream(&buf, &bufSize);
    if (!stream) {
        qWarning() << "[Tokenizer] open_memstream failed";
        return {};
    }
#  else
    // Windows: write to a temp file and read it back.
    char tmpPath[L_tmpnam];
    std::tmpnam(tmpPath);
    FILE *stream = std::fopen(tmpPath, "w+");
    if (!stream) {
        qWarning() << "[Tokenizer] failed to open temp file for phoneme trace";
        return {};
    }
#  endif

    espeak_SetPhonemeTrace(3, stream);   // 3 = phoneme names (bit1) + IPA (bit2)

    const QByteArray utf8 = text.toUtf8();
    espeak_Synth(utf8.constData(),
                 static_cast<size_t>(utf8.size()) + 1,
                 0, POS_CHARACTER, 0,
                 espeakCHARS_UTF8, nullptr, nullptr);
    espeak_Synchronize();

    espeak_SetPhonemeTrace(0, nullptr);  // disable trace

#  ifndef Q_OS_WIN
    std::fclose(stream);
    QString result = QString::fromUtf8(buf, static_cast<int>(bufSize)).trimmed();
    std::free(buf);
#  else
    std::rewind(stream);
    QByteArray data;
    char ch;
    while (std::fread(&ch, 1, 1, stream) == 1)
        data.append(ch);
    std::fclose(stream);
    std::remove(tmpPath);
    QString result = QString::fromUtf8(data).trimmed();
#  endif

    qDebug() << "[Tokenizer] raw phoneme trace:" << result;
    return result;
#endif // HAVE_ESPEAK_NG
}

QVector<int64_t> PhonemeTokenizer::phonemesToIds(const QString &phonemes) const
{
    QVector<int64_t> ids;
    ids.reserve(phonemes.size());

    const QList<uint> codepoints = phonemes.toUcs4();
    int pos = 0;
    const int len = static_cast<int>(codepoints.size());

    while (pos < len && ids.size() < kMaxTokens) {
        bool matched = false;
        for (int n = qMin(2, len - pos); n >= 1; --n) {
            const QString sym = QString::fromUcs4(
                reinterpret_cast<const char32_t *>(codepoints.constData() + pos), n);
            auto it = m_vocab.constFind(sym);
            if (it != m_vocab.constEnd()) {
                ids.append(it.value());
                pos += n;
                matched = true;
                break;
            }
        }
        if (!matched)
            ++pos;
    }

    qDebug() << "[Tokenizer]" << ids.size() << "token IDs for:" << phonemes;
    return ids;
}
