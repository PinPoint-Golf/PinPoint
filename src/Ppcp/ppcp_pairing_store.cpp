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

// RV 7.2c — "secrets at rest are held in the platform's protected storage where
// one exists", and 7.4b — persistence is opt-in, visible, individually
// revocable.
//
// ⚠ 7.2c IS A **SHOULD**, NOT A MUST — libppcp erratum E56, 25 August 2026.
// This file used to be the "where one exists" half, and it reached
// Security.framework directly on macOS and returned NOTHING everywhere else.
// That was a faithful reading of the clause and it was the wrong product:
//
//   ⛔ `makePlatformPairingStore()` returned nullptr off Apple, so `persist()`
//      refused and **Windows and Linux could persist no pairing at all**.
//      Reconnection could never work there, however good the discovery got —
//      and studio PCs are overwhelmingly Windows.  §7.4 and §7.5 were dead
//      letter on the platforms most of this application's users are on.
//   ⛔ On macOS the keychain also prompts.  The app is ad-hoc signed during
//      development, its CDHash changes on every relink, and macOS binds
//      keychain ACLs to the code signature — so a rebuilt binary looks like a
//      different app to an item the previous build wrote, and the user is asked
//      to authorise it again.  That much is a development artefact; the
//      standing point is that reaching a user's login keychain is a permission
//      a capture-and-review tool has to justify, and most users decline it.
//
// So the PRK now lives in the application's own settings, on every platform,
// through the canonical `ppSettings()` handle every other module uses.
//
// ⛔ **NOT `src/Secrets/SecretsManager`, and the distinction is not cosmetic.**
// That class layers an environment-variable override and a compile-time default
// on top of QSettings so an API key can be injected at build or run time.  An
// env var that can inject a pairing root key would be a new defect, not reuse.
// What is reused is the settings *handle*, not the secrets facade.
//
// ⚠ **What this gives up, recorded rather than assumed away.**  A PRK in the
// settings file is readable by anything running as that user.  Two consequences,
// both judged and accepted rather than overlooked:
//   • Possession of the file is a standing ability to complete a handshake as a
//     paired peer.  RV §7.4's own "possession of the device's storage is
//     possession of continuing access" was written when this was a keychain and
//     should now be read at its stronger meaning.  7.4b's opt-in, visible,
//     individually revocable persistence is what remains, and it does more for
//     the user than the storage mechanism did.
//   • §5.4 leaves forward secrecy best-effort and there is none at the TLS 1.2
//     PSK floor, so someone who captured LAN traffic at the time and *later*
//     reads this file decrypts those sessions retrospectively (Annex B12).
//     Judged thin: it needs passive capture then file access, and an attacker
//     with file access already has the swing videos.
//
// The unit of storage is still PRK and only PRK (5.1c).  psk never reaches this
// interface, which is why it cannot be persisted by accident.

#include "ppcp_rendezvous.h"

#include "../Core/pp_settings.h"

#include <QByteArray>
#include <QFile>
#include <QFileDevice>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstring>
#include <map>
#include <mutex>

namespace Ppcp {
namespace {

void wipeBytes(void *p, std::size_t n)
{
    volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
    while (n--) *v++ = 0;
}

// ── The in-memory store ─────────────────────────────────────────────────────
// Not protected storage, and describe() says so in as many words so that a
// diagnostic export never implies a guarantee the run did not have.  It is what
// the suite uses (RT-16 and the persistence rules are assertions about the
// DECISION to persist, not about the keychain) and what a run gets when the
// user has not opted in.
class EphemeralStore final : public PairingSecretStore {
public:
    bool put(const std::string &id, const std::uint8_t prk[PPCP_RV_KEY_BYTES]) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto &v = m_rows[id];
        v.assign(prk, prk + PPCP_RV_KEY_BYTES);
        return true;
    }
    bool get(const std::string &id, std::uint8_t prk[PPCP_RV_KEY_BYTES]) const override
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_rows.find(id);
        if (it == m_rows.end() || it->second.size() != PPCP_RV_KEY_BYTES) return false;
        std::memcpy(prk, it->second.data(), PPCP_RV_KEY_BYTES);
        return true;
    }
    bool erase(const std::string &id) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto it = m_rows.find(id);
        if (it == m_rows.end()) return false;
        wipeBytes(it->second.data(), it->second.size());
        m_rows.erase(it);
        return true;
    }
    std::vector<std::string> list() const override
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::vector<std::string> out;
        out.reserve(m_rows.size());
        for (const auto &kv : m_rows) out.push_back(kv.first);
        return out;
    }
    std::string describe() const override
    {
        std::lock_guard<std::mutex> g(m_mu);
        return "in-memory (NOT protected storage), " + std::to_string(m_rows.size()) +
               " pairing(s)";
    }
    ~EphemeralStore() override
    {
        std::lock_guard<std::mutex> g(m_mu);
        for (auto &kv : m_rows) wipeBytes(kv.second.data(), kv.second.size());
    }

private:
    mutable std::mutex m_mu;
    std::map<std::string, std::vector<std::uint8_t>> m_rows;
};

// ── The settings store ──────────────────────────────────────────────────────
// One key per pairing under `ppcp/pairings/<pairingId>`, holding the 32 raw
// bytes of PRK as hex.  Nothing else — not the sid, not the endpoints, not the
// display name.  7.4e says a new session derives a fresh `sid` inside the
// authenticated channel and the original is never transmitted again, so there
// is nothing else a persisted pairing needs.  The nickname a user sees lives in
// `ppcp/phoneNames`, deliberately apart from the key material.
//
// ⚠ Works on macOS, Windows and Linux alike, which is the whole point of E56:
// there is no platform here that gets no persistence.
class SettingsStore final : public PairingSecretStore {
public:
    bool put(const std::string &id, const std::uint8_t prk[PPCP_RV_KEY_BYTES]) override
    {
        if (id.empty()) return false;
        std::lock_guard<std::mutex> g(m_mu);
        QByteArray raw(reinterpret_cast<const char *>(prk), PPCP_RV_KEY_BYTES);
        auto s = ppSettings();
        s.setValue(keyFor(id), QString::fromLatin1(raw.toHex()));
        s.sync();
        wipeBytes(raw.data(), static_cast<std::size_t>(raw.size()));
        if (s.status() != QSettings::NoError) return false;
        tightenPermissions(s.fileName());
        return true;
    }

    bool get(const std::string &id, std::uint8_t prk[PPCP_RV_KEY_BYTES]) const override
    {
        if (id.empty()) return false;
        std::lock_guard<std::mutex> g(m_mu);
        auto s = ppSettings();
        const QVariant v = s.value(keyFor(id));
        if (!v.isValid()) return false;
        QByteArray raw = QByteArray::fromHex(v.toString().toLatin1());
        // ⛔ A short or corrupt row is a failure, never a zero key.  A silently
        // zeroed PRK would complete a handshake with nobody and look like a
        // revocation at the far end.
        if (raw.size() != PPCP_RV_KEY_BYTES) return false;
        std::memcpy(prk, raw.constData(), PPCP_RV_KEY_BYTES);
        wipeBytes(raw.data(), static_cast<std::size_t>(raw.size()));
        return true;
    }

    bool erase(const std::string &id) override
    {
        if (id.empty()) return false;
        std::lock_guard<std::mutex> g(m_mu);
        auto s = ppSettings();
        const QString k = keyFor(id);
        if (!s.contains(k)) return false;
        // 7.4d — revocation is honoured immediately by this side.  There is no
        // soft delete: the row goes and the file is synced before we return.
        s.remove(k);
        s.sync();
        return true;
    }

    std::vector<std::string> list() const override
    {
        std::lock_guard<std::mutex> g(m_mu);
        auto s = ppSettings();
        s.beginGroup(QStringLiteral("ppcp/pairings"));
        const QStringList keys = s.childKeys();
        s.endGroup();
        std::vector<std::string> out;
        out.reserve(static_cast<std::size_t>(keys.size()));
        for (const QString &k : keys) out.push_back(k.toStdString());
        return out;
    }

    std::string describe() const override
    {
        // RT-9 — a name and a count, never a byte of a key.  ⛔ It says "not
        // protected storage" in as many words so a diagnostic export never
        // implies a guarantee the run did not have.
        std::lock_guard<std::mutex> g(m_mu);
        auto s = ppSettings();
        s.beginGroup(QStringLiteral("ppcp/pairings"));
        const int n = s.childKeys().size();
        s.endGroup();
        return "application settings (NOT protected storage; RV 7.2c is a SHOULD, "
               "erratum E56), " + std::to_string(n) + " pairing(s)";
    }

private:
    // ⛔ The settings file is created rw-r--r--, i.e. readable by every account
    // on the machine, and it now holds a PRK.  ⚠ This is not merely about the
    // PRK: the file ALREADY held this application's API keys in plain text
    // under `secrets/*`, so the exposure predates erratum E56 and is only made
    // worse by it.  Narrowed to the owner on the first pairing write.
    //
    // ⚠ It does not encrypt anything and does not pretend to.  An attacker
    // running AS THIS USER reads it either way — RV §7.4's "possession of the
    // device's storage is possession of continuing access", which 7.2c's
    // relaxation makes the operative sentence.  What it stops is the OTHER
    // accounts on a shared machine, which is worth having and costs nothing.
    static void tightenPermissions(const QString &path)
    {
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.exists()) return;
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    static QString keyFor(const std::string &id)
    {
        return QStringLiteral("ppcp/pairings/") + QString::fromStdString(id);
    }

    mutable std::mutex m_mu;
};

}  // namespace

std::unique_ptr<PairingSecretStore> makeEphemeralPairingStore()
{
    return std::unique_ptr<PairingSecretStore>(new EphemeralStore);
}

std::unique_ptr<PairingSecretStore> makePlatformPairingStore(const std::string &service)
{
    // ⚠ `service` is no longer used: it named the keychain service, and the
    // settings store keys on the pairingId under one group.  Kept in the
    // signature so the call site and its comment stay meaningful, and because
    // a future protected-storage backend would want it again.
    (void)service;
    // ⛔ Every platform, and that is the point of E56.  This returned nullptr
    // off Apple until 25 August 2026, so Windows and Linux persisted nothing
    // and reconnection could never work there.
    return std::unique_ptr<PairingSecretStore>(new SettingsStore);
}


}  // namespace Ppcp
