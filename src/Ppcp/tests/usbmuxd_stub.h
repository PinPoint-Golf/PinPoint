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

// A STUB usbmuxd — the plist protocol of design §4.2 over an AF_UNIX socket,
// with scripted device lists, scripted `Connect` results and scripted
// attach/detach.
//
// WHY IT EXISTS.  Design §12 names "both halves of the wired path are unreached
// code" as the largest single estimate risk in Phase 1.  Everything above the
// cable — the header framing, the plist scan, the ⛔ PortNumber byte order, the
// ConnectionType filter, the Listen event stream and every row of the §6.2
// diagnostics table the mux layer can see — is testable with no phone, no
// cable and no daemon, and this is the thing that makes it so.
//
// ⚠ IT IS DELIBERATELY NOT WRITTEN AGAINST ppcp_usbmux.cpp's HELPERS.  It emits
// its own headers and its own XML so that a test asserting "the client put the
// port on the wire big-endian" is an assertion about two independent pieces of
// code agreeing, not about one piece of code agreeing with itself.
//
// Phase 1 contract C7: this file belongs to the PPS transport agent.  The
// host-service agent drives it through ppcp_usbmux.h rather than copying it.

#include "ppcp_usbmux.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace UsbmuxStub {

// One device as usbmuxd would describe it in `DeviceList` / `Attached`.
struct Entry {
    std::uint32_t deviceId = 0;
    std::string udid;
    std::string connectionType = "USB";      // or "Network" for a WiFi pairing
    std::uint64_t connectionSpeed = 480000000;
    std::uint32_t productId = 4776;
    std::uint64_t locationId = 337641472;
};

// What the last request looked like on the wire, header and all.
struct RequestRecord {
    bool seen = false;
    std::uint32_t length = 0;      // as sent — INCLUSIVE of the 16-byte header
    std::uint32_t version = 0;
    std::uint32_t message = 0;
    std::uint32_t tag = 0;
    std::size_t bodyBytes = 0;
    std::string messageType;
    std::string body;

    // ⛔ The raw plist value of `PortNumber`, exactly as it arrived.  The test
    // that matters asserts THIS is htons(port) and not port.
    long long portNumberOnWire = -1;
    long long deviceId = -1;
};

class Daemon {
public:
    Daemon()
    {
        static std::atomic<int> counter{0};
        m_path = "/tmp/ppcp-muxstub-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter.fetch_add(1)) + ".sock";
        ::unlink(m_path.c_str());

        m_listen = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_listen < 0) return;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::memcpy(a.sun_path, m_path.c_str(), m_path.size() + 1);
        if (::bind(m_listen, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
            ::close(m_listen);
            m_listen = -1;
            return;
        }
        if (::listen(m_listen, 8) != 0) {
            ::close(m_listen);
            m_listen = -1;
            return;
        }
        m_ok = true;
        m_acceptor = std::thread([this] { acceptLoop(); });
    }

    ~Daemon() { stop(); }

    Daemon(const Daemon &) = delete;
    Daemon &operator=(const Daemon &) = delete;

    bool ok() const { return m_ok; }
    const std::string &path() const { return m_path; }
    Ppcp::Usbmux::Provider provider() const { return Ppcp::Usbmux::Provider::unixSocket(m_path); }

    // ── The script ─────────────────────────────────────────────────────────
    void setDevices(std::vector<Entry> d)
    {
        std::lock_guard<std::mutex> g(m_m);
        m_devices = std::move(d);
    }
    void setConnectResult(int number)
    {
        std::lock_guard<std::mutex> g(m_m);
        m_connectResult = number;
    }
    void setListenResult(int number)
    {
        std::lock_guard<std::mutex> g(m_m);
        m_listenResult = number;
    }
    // false = reply with tag+1, i.e. a daemon out of step with its client.
    void setEchoTag(bool echo)
    {
        std::lock_guard<std::mutex> g(m_m);
        m_echoTag = echo;
    }
    // false = do not announce the current device list when Listen succeeds, so
    // a test can drive attach ordering by hand.
    void setAnnounceOnListen(bool on)
    {
        std::lock_guard<std::mutex> g(m_m);
        m_announceOnListen = on;
    }

    RequestRecord lastRequest() const
    {
        std::lock_guard<std::mutex> g(m_m);
        return m_lastRequest;
    }
    RequestRecord lastConnect() const
    {
        std::lock_guard<std::mutex> g(m_m);
        return m_lastConnect;
    }

    // ── Asynchronous events on whichever Listen connection is open ─────────
    void pushAttached(const Entry &e)
    {
        std::lock_guard<std::mutex> g(m_m);
        m_devices.push_back(e);
        sendToListenersLocked(attachedPlist(e));
    }
    void pushDetached(std::uint32_t deviceId)
    {
        std::lock_guard<std::mutex> g(m_m);
        for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
            if (it->deviceId == deviceId) { m_devices.erase(it); break; }
        sendToListenersLocked(detachedPlist(deviceId));
    }
    // Something usbmuxd really does send and we really do have to ignore.
    void pushPaired(std::uint32_t deviceId)
    {
        std::lock_guard<std::mutex> g(m_m);
        sendToListenersLocked(simplePlist("Paired", deviceId));
    }

    bool waitForListener(int ms)
    {
        std::unique_lock<std::mutex> g(m_m);
        return m_listenerCv.wait_for(g, std::chrono::milliseconds(ms),
                                     [this] { return !m_listeners.empty(); });
    }

    void stop()
    {
        if (m_stopped.exchange(true)) return;

        // ⚠ close() does NOT reliably wake a thread parked in accept() on macOS,
        // so the acceptor is woken the way anything else would wake it: with a
        // connection.  It sees m_stopped and returns.
        if (m_listen >= 0) {
            const int w = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (w >= 0) {
                sockaddr_un a{};
                a.sun_family = AF_UNIX;
                std::memcpy(a.sun_path, m_path.c_str(), m_path.size() + 1);
                ::connect(w, reinterpret_cast<sockaddr *>(&a), sizeof a);
                ::close(w);
            }
        }
        if (m_acceptor.joinable()) m_acceptor.join();
        if (m_listen >= 0) { ::close(m_listen); m_listen = -1; }

        // Every handler is parked in recv() — on a held-open Listen connection
        // or on a tunnel echo.  shutdown() is what ends those.
        {
            std::lock_guard<std::mutex> g(m_m);
            for (int fd : m_conns) ::shutdown(fd, SHUT_RDWR);
        }
        for (auto &t : m_handlers)
            if (t.joinable()) t.join();
        m_handlers.clear();
        ::unlink(m_path.c_str());
    }

private:
    // ── Framing, written out independently of the client under test ────────
    static void putLe(std::string &s, std::uint32_t v)
    {
        s.push_back(static_cast<char>(v & 0xff));
        s.push_back(static_cast<char>((v >> 8) & 0xff));
        s.push_back(static_cast<char>((v >> 16) & 0xff));
        s.push_back(static_cast<char>((v >> 24) & 0xff));
    }
    static std::uint32_t getLe(const unsigned char *p)
    {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }

    static std::string frame(std::uint32_t tag, const std::string &xml)
    {
        std::string out;
        putLe(out, static_cast<std::uint32_t>(16 + xml.size()));   // length INCLUDES the header
        putLe(out, 1);      // version
        putLe(out, 8);      // message = plist
        putLe(out, tag);
        out += xml;
        return out;
    }

    static bool writeAll(int fd, const std::string &b)
    {
        std::size_t sent = 0;
        while (sent < b.size()) {
            const ssize_t w = ::send(fd, b.data() + sent, b.size() - sent, 0);
            if (w > 0) { sent += static_cast<std::size_t>(w); continue; }
            if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        return true;
    }

    static bool readExact(int fd, unsigned char *p, std::size_t n)
    {
        std::size_t got = 0;
        while (got < n) {
            const ssize_t r = ::recv(fd, p + got, n - got, 0);
            if (r > 0) { got += static_cast<std::size_t>(r); continue; }
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    // ── A crude but independent plist reader ───────────────────────────────
    static std::string valueFor(const std::string &xml, const std::string &key,
                                const char *tag)
    {
        const std::string k = "<key>" + key + "</key>";
        const std::size_t at = xml.find(k);
        if (at == std::string::npos) return {};
        const std::string open = std::string("<") + tag + ">";
        const std::string close = std::string("</") + tag + ">";
        const std::size_t o = xml.find(open, at + k.size());
        if (o == std::string::npos) return {};
        const std::size_t c = xml.find(close, o + open.size());
        if (c == std::string::npos) return {};
        return xml.substr(o + open.size(), c - o - open.size());
    }

    // ── Emitters ───────────────────────────────────────────────────────────
    static std::string propertiesXml(const Entry &e)
    {
        std::string x;
        x += "<key>Properties</key><dict>";
        x += "<key>ConnectionSpeed</key><integer>" + std::to_string(e.connectionSpeed) +
             "</integer>";
        x += "<key>ConnectionType</key><string>" + e.connectionType + "</string>";
        x += "<key>DeviceID</key><integer>" + std::to_string(e.deviceId) + "</integer>";
        x += "<key>LocationID</key><integer>" + std::to_string(e.locationId) + "</integer>";
        x += "<key>ProductID</key><integer>" + std::to_string(e.productId) + "</integer>";
        x += "<key>SerialNumber</key><string>" + e.udid + "</string>";
        x += "</dict>";
        return x;
    }

    static std::string head()
    {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\"><dict>";
    }

    static std::string resultPlist(int number)
    {
        return head() + "<key>MessageType</key><string>Result</string>" +
               "<key>Number</key><integer>" + std::to_string(number) + "</integer>" +
               "</dict></plist>";
    }

    std::string deviceListPlistLocked() const
    {
        std::string x = head() + "<key>DeviceList</key><array>";
        for (const Entry &e : m_devices) {
            x += "<dict><key>DeviceID</key><integer>" + std::to_string(e.deviceId) + "</integer>";
            x += "<key>MessageType</key><string>Attached</string>";
            x += propertiesXml(e);
            x += "</dict>";
        }
        x += "</array></dict></plist>";
        return x;
    }

    static std::string attachedPlist(const Entry &e)
    {
        return head() + "<key>MessageType</key><string>Attached</string>" +
               "<key>DeviceID</key><integer>" + std::to_string(e.deviceId) + "</integer>" +
               propertiesXml(e) + "</dict></plist>";
    }

    static std::string detachedPlist(std::uint32_t id)
    {
        // ⚠ Exactly what the real daemon sends: an id and nothing else.
        return head() + "<key>MessageType</key><string>Detached</string>" +
               "<key>DeviceID</key><integer>" + std::to_string(id) + "</integer>" +
               "</dict></plist>";
    }

    static std::string simplePlist(const char *type, std::uint32_t id)
    {
        return head() + "<key>MessageType</key><string>" + type + "</string>" +
               "<key>DeviceID</key><integer>" + std::to_string(id) + "</integer>" +
               "</dict></plist>";
    }

    void sendToListenersLocked(const std::string &xml)
    {
        for (int fd : m_listeners) writeAll(fd, frame(0, xml));
    }

    // ── Connection handling ────────────────────────────────────────────────
    void acceptLoop()
    {
        for (;;) {
            const int c = ::accept(m_listen, nullptr, nullptr);
            if (m_stopped.load()) {
                if (c >= 0) ::close(c);
                return;
            }
            if (c < 0) {
                if (errno == EINTR) continue;
                return;   // listener gone
            }
            {
                std::lock_guard<std::mutex> g(m_m);
                m_conns.push_back(c);
            }
            m_handlers.emplace_back([this, c] { serve(c); });
        }
    }

    void forgetConn(int fd)
    {
        std::lock_guard<std::mutex> g(m_m);
        for (auto it = m_conns.begin(); it != m_conns.end(); ++it)
            if (*it == fd) { m_conns.erase(it); break; }
        for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it)
            if (*it == fd) { m_listeners.erase(it); break; }
    }

    void serve(int fd)
    {
        struct Closer {
            Daemon *d; int fd;
            ~Closer() { d->forgetConn(fd); ::close(fd); }
        } closer{this, fd};

        unsigned char hdr[16];
        if (!readExact(fd, hdr, sizeof hdr)) return;

        const std::uint32_t length = getLe(hdr);
        const std::uint32_t version = getLe(hdr + 4);
        const std::uint32_t message = getLe(hdr + 8);
        const std::uint32_t tag = getLe(hdr + 12);
        if (length < 16 || length > (1u << 20)) return;

        std::string body(length - 16, '\0');
        if (!body.empty() && !readExact(fd, reinterpret_cast<unsigned char *>(&body[0]),
                                        body.size()))
            return;

        RequestRecord rec;
        rec.seen = true;
        rec.length = length;
        rec.version = version;
        rec.message = message;
        rec.tag = tag;
        rec.bodyBytes = body.size();
        rec.body = body;
        rec.messageType = valueFor(body, "MessageType", "string");
        const std::string port = valueFor(body, "PortNumber", "integer");
        const std::string dev = valueFor(body, "DeviceID", "integer");
        if (!port.empty()) rec.portNumberOnWire = std::stoll(port);
        if (!dev.empty()) rec.deviceId = std::stoll(dev);

        int connectResult = 0;
        int listenResult = 0;
        bool echoTag = true;
        bool announce = true;
        std::string listReply;
        {
            std::lock_guard<std::mutex> g(m_m);
            m_lastRequest = rec;
            if (rec.messageType == "Connect") m_lastConnect = rec;
            connectResult = m_connectResult;
            listenResult = m_listenResult;
            echoTag = m_echoTag;
            announce = m_announceOnListen;
            listReply = deviceListPlistLocked();
        }
        const std::uint32_t replyTag = echoTag ? tag : tag + 1;

        if (rec.messageType == "ListDevices") {
            writeAll(fd, frame(replyTag, listReply));
            return;
        }

        if (rec.messageType == "Listen") {
            writeAll(fd, frame(replyTag, resultPlist(listenResult)));
            if (listenResult != 0) return;
            {
                std::lock_guard<std::mutex> g(m_m);
                if (announce)
                    for (const Entry &e : m_devices) writeAll(fd, frame(0, attachedPlist(e)));
                m_listeners.push_back(fd);
                m_listenerCv.notify_all();
            }
            // Hold the socket open forever, exactly as the daemon does; stop()
            // shuts it down.  Nothing more is read from a Listen connection.
            for (;;) {
                unsigned char sink[64];
                const ssize_t r = ::recv(fd, sink, sizeof sink, 0);
                if (r == 0 || (r < 0 && errno != EINTR && errno != EAGAIN)) break;
                if (r < 0) continue;
            }
            return;
        }

        if (rec.messageType == "Connect") {
            writeAll(fd, frame(replyTag, resultPlist(connectResult)));
            if (connectResult != 0) return;
            // ✅ §4.2 — after Number=0 THE SOCKET IS THE TUNNEL.  A byte echo is
            // the cheapest thing that proves the client handed back a usable,
            // connected fd rather than a number.
            for (;;) {
                unsigned char buf[512];
                const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
                if (r > 0) {
                    if (::send(fd, buf, static_cast<std::size_t>(r), 0) < 0) break;
                    continue;
                }
                if (r == 0) break;
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
            return;
        }
    }

    std::string m_path;
    int m_listen = -1;
    bool m_ok = false;
    std::atomic<bool> m_stopped{false};
    std::thread m_acceptor;
    std::vector<std::thread> m_handlers;

    mutable std::mutex m_m;
    std::condition_variable m_listenerCv;
    std::vector<Entry> m_devices;
    std::vector<int> m_listeners;   // Listen connections, for event fan-out
    std::vector<int> m_conns;       // every accepted fd, so stop() can wake them
    int m_connectResult = 0;
    int m_listenResult = 0;
    bool m_echoTag = true;
    bool m_announceOnListen = true;
    RequestRecord m_lastRequest;
    RequestRecord m_lastConnect;
};

}  // namespace UsbmuxStub
