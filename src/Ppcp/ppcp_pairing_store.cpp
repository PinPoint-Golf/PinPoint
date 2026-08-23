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
// revocable.  This file is the "where one exists" half.
//
// ⚠ THE APPLICATION ALREADY HAS A SECRETS SUBSYSTEM AND IT IS NOT USABLE HERE.
// `src/Secrets/SecretsManager` keeps API keys in QSettings, which on macOS is
// `~/Library/Preferences/<bundle-id>.plist` — a plain file, world-readable by
// anything running as the user, and explicitly chosen for "no extra
// dependencies".  That is a perfectly reasonable place for an Azure key and is
// NOT protected storage in 7.2c's sense.  A PRK written there would give
// possession of a preferences file the standing ability to complete a handshake
// as a paired peer, which is exactly the exposure 7.4 asks to be weighed rather
// than assumed away.  So this reaches Security.framework directly, and on a
// platform with no keychain it returns NOTHING rather than falling back to the
// file — 7.2c says "where one exists", and offering no persistence is the
// conformant answer where one does not.

#include "ppcp_rendezvous.h"

#include <cstring>
#include <map>
#include <mutex>

#if defined(__APPLE__)
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

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

#if defined(__APPLE__)

// ── The macOS keychain ──────────────────────────────────────────────────────
// A generic-password item per pairing: service = the caller's service string,
// account = the pairingId, data = the 32 raw bytes of PRK.  Nothing else — not
// the sid, not the endpoints, not the display name.  7.4e says a new session
// derives a fresh `sid` inside the authenticated channel and the original is
// never transmitted again, so there is nothing else a persisted pairing needs.
class KeychainStore final : public PairingSecretStore {
public:
    explicit KeychainStore(std::string service) : m_service(std::move(service)) {}

    bool put(const std::string &id, const std::uint8_t prk[PPCP_RV_KEY_BYTES]) override
    {
        // Replace rather than duplicate: a second SecItemAdd for the same
        // (service, account) returns errSecDuplicateItem and leaves the OLD key
        // in place, which would silently pin a revoked pairing.
        erase(id);

        CFRef<CFDictionaryRef> query(baseQuery(id));
        if (!query) return false;

        CFRef<CFMutableDictionaryRef> add(
            CFDictionaryCreateMutableCopy(nullptr, 0, query.get()));
        if (!add) return false;
        CFRef<CFDataRef> data(CFDataCreate(nullptr, prk, PPCP_RV_KEY_BYTES));
        if (!data) return false;
        CFDictionarySetValue(add.get(), kSecValueData, data.get());
        // The item is useless on another machine and must not travel: no
        // iCloud keychain, no backup restore onto a different device.  7.4c
        // scopes a pairing to a counterpart peer identity and calls it
        // non-transferable; this is the storage half of that.
        CFDictionarySetValue(add.get(), kSecAttrAccessible,
                             kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
        CFDictionarySetValue(add.get(), kSecAttrSynchronizable, kCFBooleanFalse);

        const OSStatus st = SecItemAdd(add.get(), nullptr);
        return st == errSecSuccess;
    }

    bool get(const std::string &id, std::uint8_t prk[PPCP_RV_KEY_BYTES]) const override
    {
        CFRef<CFDictionaryRef> base(baseQuery(id));
        if (!base) return false;
        CFRef<CFMutableDictionaryRef> q(CFDictionaryCreateMutableCopy(nullptr, 0, base.get()));
        if (!q) return false;
        CFDictionarySetValue(q.get(), kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(q.get(), kSecMatchLimit, kSecMatchLimitOne);

        CFTypeRef result = nullptr;
        if (SecItemCopyMatching(q.get(), &result) != errSecSuccess || !result) return false;
        CFRef<CFDataRef> data(static_cast<CFDataRef>(result));
        if (CFDataGetLength(data.get()) != PPCP_RV_KEY_BYTES) return false;
        std::memcpy(prk, CFDataGetBytePtr(data.get()), PPCP_RV_KEY_BYTES);
        return true;
    }

    bool erase(const std::string &id) override
    {
        CFRef<CFDictionaryRef> q(baseQuery(id));
        if (!q) return false;
        // 7.4d — honoured IMMEDIATELY by this side.  errSecItemNotFound is a
        // success for our purposes: the key is not there, which is the whole
        // requirement.
        const OSStatus st = SecItemDelete(q.get());
        return st == errSecSuccess || st == errSecItemNotFound;
    }

    std::vector<std::string> list() const override
    {
        CFRef<CFMutableDictionaryRef> q(
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
        if (!q) return {};
        CFDictionarySetValue(q.get(), kSecClass, kSecClassGenericPassword);
        CFRef<CFStringRef> svc(cfString(m_service));
        if (!svc) return {};
        CFDictionarySetValue(q.get(), kSecAttrService, svc.get());
        CFDictionarySetValue(q.get(), kSecReturnAttributes, kCFBooleanTrue);
        CFDictionarySetValue(q.get(), kSecMatchLimit, kSecMatchLimitAll);

        CFTypeRef result = nullptr;
        if (SecItemCopyMatching(q.get(), &result) != errSecSuccess || !result) return {};
        CFRef<CFArrayRef> arr(static_cast<CFArrayRef>(result));

        std::vector<std::string> out;
        const CFIndex n = CFArrayGetCount(arr.get());
        for (CFIndex i = 0; i < n; ++i) {
            auto d = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(arr.get(), i));
            auto acct = static_cast<CFStringRef>(CFDictionaryGetValue(d, kSecAttrAccount));
            if (!acct) continue;
            char buf[256] = {0};
            if (CFStringGetCString(acct, buf, sizeof buf, kCFStringEncodingUTF8))
                out.emplace_back(buf);
        }
        return out;
    }

    std::string describe() const override
    {
        // 7.2b: a name and a count.  Never an account list — a pairingId is not
        // a secret, but a diagnostic export is not the place to enumerate which
        // devices this host has ever been paired with either.
        return "macOS keychain (service " + m_service + "), " +
               std::to_string(list().size()) + " pairing(s)";
    }

private:
    template <typename T>
    class CFRef {
    public:
        explicit CFRef(T r = nullptr) : m_r(r) {}
        ~CFRef() { if (m_r) CFRelease(m_r); }
        CFRef(const CFRef &) = delete;
        CFRef &operator=(const CFRef &) = delete;
        T get() const { return m_r; }
        explicit operator bool() const { return m_r != nullptr; }

    private:
        T m_r;
    };

    static CFStringRef cfString(const std::string &s)
    {
        return CFStringCreateWithBytes(nullptr,
                                       reinterpret_cast<const UInt8 *>(s.data()),
                                       static_cast<CFIndex>(s.size()),
                                       kCFStringEncodingUTF8, false);
    }

    CFDictionaryRef baseQuery(const std::string &id) const
    {
        CFMutableDictionaryRef q =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        if (!q) return nullptr;
        CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
        CFRef<CFStringRef> svc(cfString(m_service));
        CFRef<CFStringRef> acct(cfString(id));
        if (!svc || !acct) { CFRelease(q); return nullptr; }
        CFDictionarySetValue(q, kSecAttrService, svc.get());
        CFDictionarySetValue(q, kSecAttrAccount, acct.get());
        return q;
    }

    std::string m_service;
};

#endif  // __APPLE__

}  // namespace

std::unique_ptr<PairingSecretStore> makeEphemeralPairingStore()
{
    return std::unique_ptr<PairingSecretStore>(new EphemeralStore);
}

std::unique_ptr<PairingSecretStore> makePlatformPairingStore(const std::string &service)
{
#if defined(__APPLE__)
    return std::unique_ptr<PairingSecretStore>(new KeychainStore(service));
#else
    // 7.2c — "where one exists".  Nothing here yet for Windows (DPAPI /
    // Credential Manager) or Linux (libsecret), so this host offers no
    // persistence there.  Returning null is what makes PpcpRendezvous::persist()
    // refuse; returning a file-backed store would be the failure this file
    // exists to avoid.
    (void)service;
    return nullptr;
#endif
}

}  // namespace Ppcp
