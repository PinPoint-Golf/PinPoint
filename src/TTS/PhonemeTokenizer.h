#pragma once
#include <QHash>
#include <QString>
#include <QVector>

// Converts plain text to Kokoro token IDs via espeak-ng phonemisation.
//
// Pipeline:
//   text  ──▶  espeak_SetPhonemeTrace (IPA mode)  ──▶  IPA string  ──▶  token IDs
//
// Requires libespeak-ng (linked at build time via CMake) and its runtime data.

class PhonemeTokenizer
{
public:
    static constexpr int kMaxTokens = 512;

    PhonemeTokenizer() = default;
    ~PhonemeTokenizer();

    bool    initialise(const QString &tokensJsonPath);
    bool    isInitialised() const { return m_initialised; }
    QString lastError()     const { return m_lastError; }

    QVector<int64_t> tokenise(const QString &text) const;

private:
    QString          textToPhonemes(const QString &text) const;
    QVector<int64_t> phonemesToIds(const QString &phonemes) const;

    QHash<QString, int64_t> m_vocab;
    bool                    m_initialised = false;
    QString                 m_lastError;
};
