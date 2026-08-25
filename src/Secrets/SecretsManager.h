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

#pragma once
#include <QString>

// Reads/writes application secrets from (priority order):
//   1. Environment variable: UPPER_SNAKE_CASE form of the key name
//      e.g. key "assemblyaiApiKey" → env var "ASSEMBLYAI_API_KEY"
//   2. QSettings under "secrets/<key>", via ppSettings()
//      ⚠ CORRECTED 25 Aug 2026.  This said the values land in the
//      platform-native location — a plist on macOS, HKCU on Windows.  They do
//      not: ppSettings() pins QSettings::IniFormat, so it is ONE INI file on
//      every platform, ~/.config/PinPointStudio/PinPointStudio.ini on macOS and
//      Linux alike.  The stale note mattered: ppcp_pairing_store.cpp cited the
//      "plain plist" as its reason not to reuse this, and the reason was wrong
//      even though the conclusion (do not put a PRK behind an env-var override)
//      was right.
//      ⛔ The file is plain text and holds these keys unencrypted.
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
