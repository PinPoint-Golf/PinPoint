#pragma once
#include <QString>

// Reads/writes application secrets from (priority order):
//   1. Environment variable: UPPER_SNAKE_CASE form of the key name
//      e.g. key "assemblyaiApiKey" → env var "ASSEMBLYAI_API_KEY"
//   2. QSettings under "secrets/<key>"
//      stored in the platform-native config location (no extra dependencies):
//        Linux  : ~/.config/<OrgName>/<AppName>.conf
//        macOS  : ~/Library/Preferences/<bundle-id>.plist
//        Windows: HKCU\Software\<OrgName>\<AppName>
//
// Call initializeDefaults() once at startup to seed QSettings from any
// compile-time defaults (set via -DASSEMBLYAI_API_KEY=<value> at cmake time).
class SecretsManager {
public:
    static QString read(const QString& key);
    static void    write(const QString& key, const QString& value);
    static void    initializeDefaults();

private:
    static QString toEnvVarName(const QString& key);
};
