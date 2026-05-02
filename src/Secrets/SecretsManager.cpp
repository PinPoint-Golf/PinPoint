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

// Seeds QSettings from compile-time defaults on first run.
// Compile with -DASSEMBLYAI_API_KEY=<value> to embed a default key that
// is written to QSettings the first time the app starts, then read from
// QSettings on subsequent runs (no recompilation needed after initial seed).
void SecretsManager::initializeDefaults()
{
#ifdef ASSEMBLYAI_API_KEY_DEFAULT
    QSettings settings;
    const QString k = QStringLiteral("secrets/assemblyaiApiKey");
    if (settings.value(k).toString().isEmpty())
        settings.setValue(k, QStringLiteral(ASSEMBLYAI_API_KEY_DEFAULT));
#endif
}
