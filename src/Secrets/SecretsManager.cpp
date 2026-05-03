#include "SecretsManager.h"
#include <QProcessEnvironment>
#include <QSettings>

// Converts camelCase key to UPPER_SNAKE_CASE for env var lookup.
// "assemblyaiApiKey" → "ASSEMBLYAI_API_KEY"
QString SecretsManager::toEnvVarName(const QString& key)
{
    QString result;
    result.reserve(key.size() + 4);
    for (QChar c : key) {
        if (c.isUpper() && !result.isEmpty())
            result += QLatin1Char('_');
        result += c.toUpper();
    }
    return result;
}

QString SecretsManager::read(const QString& key)
{
    const QString envVal =
        QProcessEnvironment::systemEnvironment().value(toEnvVarName(key));
    if (!envVal.isEmpty())
        return envVal;

    return QSettings().value(QStringLiteral("secrets/") + key).toString();
}

void SecretsManager::write(const QString& key, const QString& value)
{
    QSettings().setValue(QStringLiteral("secrets/") + key, value);
}

// Persists keys into QSettings on first run so they survive without the
// original source on subsequent launches.
//
// Two seeding routes:
//   - Compile-time: -DASSEMBLYAI_API_KEY=<val> → ASSEMBLYAI_API_KEY_DEFAULT define
//   - Runtime env var: set AZURE_TTS_API_KEY (or any key in kRuntimeEnvKeys)
//     before the first launch; value is saved to QSettings and the env var is
//     not required again after that.
void SecretsManager::initializeDefaults()
{
    QSettings settings;

#ifdef ASSEMBLYAI_API_KEY_DEFAULT
    {
        const QString k = QStringLiteral("secrets/assemblyaiApiKey");
        if (settings.value(k).toString().isEmpty())
            settings.setValue(k, QStringLiteral(ASSEMBLYAI_API_KEY_DEFAULT));
    }
#endif

    // Keys whose values come from runtime env vars rather than compile-time defines.
    static const char * const kRuntimeEnvKeys[] = { "azureTtsApiKey", "azureSttApiKey" };
    const auto env = QProcessEnvironment::systemEnvironment();
    for (const char *key : kRuntimeEnvKeys) {
        const QString qkey       = QString::fromLatin1(key);
        const QString settingKey = QStringLiteral("secrets/") + qkey;
        if (settings.value(settingKey).toString().isEmpty()) {
            const QString val = env.value(toEnvVarName(qkey));
            if (!val.isEmpty())
                settings.setValue(settingKey, val);
        }
    }
}
