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

#include "ppcp_usbmux.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  define pp_close_socket closesocket
#  define pp_poll WSAPoll
#  define pp_last_error() WSAGetLastError()
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#  define pp_close_socket ::close
#  define pp_poll ::poll
#  define pp_last_error() errno
#endif

namespace Ppcp {
namespace Usbmux {
namespace {

// ── The wire format, §4.2 — VERIFIED live against /var/run/usbmuxd on the ──
// build Mac on 29 August 2026 with a phone attached, not recalled.
//
//   struct { uint32 length; uint32 version; uint32 message; uint32 tag; }
//
// little-endian, `length` INCLUSIVE of these 16 bytes, followed by an XML
// plist.  The reply echoes the request's tag.
constexpr std::size_t kHeaderSize = 16;
constexpr std::uint32_t kProtocolVersion = 1;      // observed on every reply
constexpr std::uint32_t kMessagePlist = 8;         // observed on every reply

// A sanity ceiling on one message.  usbmuxd's real DeviceList for a plausible
// number of attached devices is a few kilobytes; a megabyte means the socket is
// not usbmuxd, and we would rather say so than allocate on its say-so.
constexpr std::uint32_t kMaxMessageBytes = 1u << 20;

// usbmuxd's own client-library version gate.  3 is what everything current
// sends; a daemon that refuses it answers Result Number 5.
constexpr long long kLibUsbMuxVersion = 3;

constexpr const char *kProgName = "PinPointStudio";

// ── Little-endian codec, written out rather than memcpy'd over a struct ────
// The header is defined by the protocol as little-endian, not as host order.
std::uint32_t u32le(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void putU32le(unsigned char *p, std::uint32_t v)
{
    p[0] = static_cast<unsigned char>(v & 0xff);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xff);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xff);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xff);
}

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ── A minimal XML-plist scanner ────────────────────────────────────────────
//
// Not a general XML parser and deliberately so: it reads the dozen keys §4.2
// names and nothing else.  What it must be is UNSURPRISABLE — a malformed or
// hostile document has to come back as "no", never as a crash or an
// unbounded allocation — so it bounds nesting and never trusts a length.

constexpr int kMaxPlistDepth = 32;

struct PlistNode {
    enum class Type { Nil, Dict, Array, String, Integer, Boolean, Data, Real };

    Type type = Type::Nil;
    std::string text;          // String, Data (base64, undecoded), Real
    long long integer = 0;
    bool boolean = false;
    std::vector<std::pair<std::string, PlistNode>> dict;
    std::vector<PlistNode> array;

    const PlistNode *find(const char *key) const
    {
        if (type != Type::Dict) return nullptr;
        for (const auto &kv : dict)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    std::string stringOr(const char *key, const char *def = "") const
    {
        const PlistNode *n = find(key);
        return (n && n->type == Type::String) ? n->text : std::string(def);
    }

    long long intOr(const char *key, long long def = 0) const
    {
        const PlistNode *n = find(key);
        return (n && n->type == Type::Integer) ? n->integer : def;
    }
};

std::string unescape(const std::string &in)
{
    if (in.find('&') == std::string::npos) return in;
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] != '&') { out.push_back(in[i++]); continue; }
        const std::size_t sc = in.find(';', i);
        if (sc == std::string::npos || sc - i > 8) { out.push_back(in[i++]); continue; }
        const std::string ent = in.substr(i + 1, sc - i - 1);
        if      (ent == "amp")  out.push_back('&');
        else if (ent == "lt")   out.push_back('<');
        else if (ent == "gt")   out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') {
            const long v = std::strtol(ent.c_str() + 1, nullptr, ent.size() > 1 && ent[1] == 'x' ? 16 : 10);
            if (v > 0 && v < 128) out.push_back(static_cast<char>(v));
        } else {
            out.append(in, i, sc - i + 1);   // unknown entity: leave it alone
        }
        i = sc + 1;
    }
    return out;
}

class PlistScanner {
public:
    PlistScanner(const char *begin, std::size_t len) : m_p(begin), m_end(begin + len) {}

    // Finds <plist> and parses the single value inside it.  False on anything
    // it does not understand — there is no partial success.
    bool parse(PlistNode &out)
    {
        Tag t;
        while (readTag(t)) {
            if (t.name == "plist" && !t.closing) {
                if (t.selfClosing) return false;
                Tag v;
                if (!readTag(v) || v.closing) return false;
                return parseValue(v, out, 0);
            }
        }
        return false;
    }

private:
    struct Tag {
        std::string name;
        bool closing = false;
        bool selfClosing = false;
    };

    // Advances to the next element, skipping text, <?...?> and <!...>.
    bool readTag(Tag &t)
    {
        for (;;) {
            while (m_p < m_end && *m_p != '<') ++m_p;
            if (m_p >= m_end) return false;
            ++m_p;   // past '<'
            if (m_p < m_end && (*m_p == '?' || *m_p == '!')) {
                // <?xml ... ?> or <!DOCTYPE ...> / <!-- ... -->.  The DOCTYPE
                // usbmuxd emits carries a quoted URL with no '>' in it, so a
                // scan to the next '>' is sufficient and stays bounded.
                while (m_p < m_end && *m_p != '>') ++m_p;
                if (m_p < m_end) ++m_p;
                continue;
            }
            t = Tag{};
            if (m_p < m_end && *m_p == '/') { t.closing = true; ++m_p; }
            const char *s = m_p;
            while (m_p < m_end && *m_p != '>' && *m_p != ' ' && *m_p != '/' && *m_p != '\t' &&
                   *m_p != '\n' && *m_p != '\r')
                ++m_p;
            t.name.assign(s, static_cast<std::size_t>(m_p - s));
            while (m_p < m_end && *m_p != '>') {
                if (*m_p == '/') t.selfClosing = true;
                ++m_p;
            }
            if (m_p < m_end) ++m_p;   // past '>'
            return !t.name.empty();
        }
    }

    // Text up to the next '<', unescaped.
    std::string readText()
    {
        const char *s = m_p;
        while (m_p < m_end && *m_p != '<') ++m_p;
        return unescape(std::string(s, static_cast<std::size_t>(m_p - s)));
    }

    bool expectClose(const char *name)
    {
        Tag t;
        return readTag(t) && t.closing && t.name == name;
    }

    bool parseValue(const Tag &open, PlistNode &out, int depth)
    {
        if (depth > kMaxPlistDepth) return false;

        if (open.name == "dict") {
            out.type = PlistNode::Type::Dict;
            if (open.selfClosing) return true;
            for (;;) {
                Tag t;
                if (!readTag(t)) return false;
                if (t.closing && t.name == "dict") return true;
                if (t.closing || t.name != "key") return false;
                std::string key;
                if (!t.selfClosing) {
                    key = readText();
                    if (!expectClose("key")) return false;
                }
                Tag v;
                if (!readTag(v) || v.closing) return false;
                PlistNode child;
                if (!parseValue(v, child, depth + 1)) return false;
                out.dict.emplace_back(std::move(key), std::move(child));
            }
        }

        if (open.name == "array") {
            out.type = PlistNode::Type::Array;
            if (open.selfClosing) return true;
            for (;;) {
                Tag t;
                if (!readTag(t)) return false;
                if (t.closing && t.name == "array") return true;
                if (t.closing) return false;
                PlistNode child;
                if (!parseValue(t, child, depth + 1)) return false;
                out.array.push_back(std::move(child));
            }
        }

        if (open.name == "true" || open.name == "false") {
            out.type = PlistNode::Type::Boolean;
            out.boolean = (open.name == "true");
            if (!open.selfClosing) return expectClose(open.name.c_str());
            return true;
        }

        if (open.name == "string" || open.name == "integer" || open.name == "real" ||
            open.name == "data" || open.name == "date") {
            if (open.name == "string")       out.type = PlistNode::Type::String;
            else if (open.name == "integer") out.type = PlistNode::Type::Integer;
            else if (open.name == "real")    out.type = PlistNode::Type::Real;
            else                             out.type = PlistNode::Type::Data;
            if (open.selfClosing) return true;
            out.text = readText();
            if (out.type == PlistNode::Type::Integer) {
                errno = 0;
                out.integer = std::strtoll(out.text.c_str(), nullptr, 10);
            }
            return expectClose(open.name.c_str());
        }

        return false;   // an element type we do not read
    }

    const char *m_p;
    const char *m_end;
};

// ── The emitter ────────────────────────────────────────────────────────────
// Every request carries the four keys usbmuxd expects of a client (§4.2).

std::string escapeXml(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default:  out.push_back(c); break;
        }
    }
    return out;
}

std::string plistRequest(const char *messageType,
                         const std::vector<std::pair<const char *, long long>> &extraInts)
{
    std::string x;
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    x += "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    x += "<plist version=\"1.0\"><dict>";
    x += "<key>MessageType</key><string>" + escapeXml(messageType) + "</string>";
    x += "<key>ClientVersionString</key><string>" + escapeXml(kProgName) + "</string>";
    x += "<key>ProgName</key><string>" + escapeXml(kProgName) + "</string>";
    x += "<key>kLibUSBMuxVersion</key><integer>" + std::to_string(kLibUsbMuxVersion) +
         "</integer>";
    for (const auto &kv : extraInts)
        x += "<key>" + escapeXml(kv.first) + "</key><integer>" + std::to_string(kv.second) +
             "</integer>";
    x += "</dict></plist>\n";
    return x;
}

// ── Sockets ────────────────────────────────────────────────────────────────

void ensureSockets()
{
#ifdef _WIN32
    struct Once {
        Once() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    };
    static Once once;
    (void)once;
#endif
}

void setNonBlocking(pp_socket_t s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &on);
#else
    const int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

// The dial hands this fd straight to the Connector, whose peer walking away
// mid-session is an ordinary event (ENC 2.1c).  Writing to a socket the far end
// closed raises SIGPIPE and kills the process; a transport must not do that to
// its embedding, and must not fix it by changing the process's signal
// disposition either.  Same reasoning, same option, as applyOptions().
void setNoSigPipe(pp_socket_t s)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char *>(&on), sizeof on);
#else
    (void)s;
#endif
}

bool waitFor(pp_socket_t s, bool forRead, double deadlineMs)
{
    const double remain = deadlineMs - nowMs();
    if (remain <= 0) return false;
#ifdef _WIN32
    WSAPOLLFD p{};
    p.fd = static_cast<SOCKET>(s);
#else
    struct pollfd p{};
    p.fd = s;
#endif
    p.events = static_cast<short>(forRead ? POLLIN : POLLOUT);
    return pp_poll(&p, 1, static_cast<int>(remain)) > 0;
}

// Opens a connection to the usbmux provider.  The fd comes back NON-BLOCKING
// and SIGPIPE-safe, because both `Connect` and `Listen` leave the caller
// holding this very socket afterwards.
pp_socket_t openProvider(const Provider &prov, double deadlineMs, Result &r)
{
    ensureSockets();

    if (prov.kind == Provider::Kind::Tcp) {
        // Phase 2.  Named here rather than silently unsupported so the log line
        // is true on the day somebody runs this on Windows.
        r.status = Status::NoProvider;
        return kInvalidSocket;
    }

#ifdef _WIN32
    // No AF_UNIX path on the Windows provider; see above.
    r.status = Status::NoProvider;
    return kInvalidSocket;
#else
    if (prov.path.empty() || prov.path.size() + 1 > sizeof(sockaddr_un::sun_path)) {
        r.status = Status::NoProvider;
        return kInvalidSocket;
    }

    const pp_socket_t s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
        r.status = Status::NoProvider;
        r.systemError = pp_last_error();
        return kInvalidSocket;
    }
    setNonBlocking(s);
    setNoSigPipe(s);

    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, prov.path.c_str(), prov.path.size() + 1);

    if (::connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
        const int e = pp_last_error();
        bool pending = (e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN);
        bool connected = false;
        if (pending && waitFor(s, /*forRead=*/false, deadlineMs)) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len);
            connected = (err == 0);
            if (!connected) r.systemError = err;
        } else if (!pending) {
            r.systemError = e;
        }
        if (!connected) {
            pp_close_socket(s);
            // ⛔ Absence is not an error (RV 3.6a): "no usbmuxd here" and "the
            // daemon refused us" are the same row of §6.2 and neither is a
            // banner.  systemError carries ENOENT vs EACCES for the log line,
            // which is what tells a Linux user their group membership is wrong.
            // A connect that neither failed nor completed inside the deadline is
            // the only Timeout case here.
            r.status = (r.systemError != 0) ? Status::NoProvider : Status::Timeout;
            return kInvalidSocket;
        }
    }
    return s;
#endif
}

bool sendAll(pp_socket_t s, const unsigned char *p, std::size_t n, double deadlineMs)
{
    std::size_t sent = 0;
    while (sent < n) {
#ifdef _WIN32
        const int w = ::send(static_cast<SOCKET>(s), reinterpret_cast<const char *>(p + sent),
                             static_cast<int>(n - sent), 0);
#else
        const ssize_t w = ::send(s, p + sent, n - sent, 0);
#endif
        if (w > 0) { sent += static_cast<std::size_t>(w); continue; }
        const int e = pp_last_error();
#ifdef _WIN32
        const bool again = (e == WSAEWOULDBLOCK);
#else
        const bool again = (e == EAGAIN || e == EWOULDBLOCK || e == EINTR);
#endif
        if (!again) return false;
        if (!waitFor(s, /*forRead=*/false, deadlineMs)) return false;
    }
    return true;
}

enum class ReadOutcome { Ok, Closed, TimedOut, Failed };

// ⛔ Reads EXACTLY n bytes and not one more.  This is not fastidiousness: after
// a successful `Connect` the very same socket IS the tunnel (§4.2), so a reader
// that over-read into a scratch buffer would silently swallow the first bytes
// the device sent.
ReadOutcome recvExact(pp_socket_t s, unsigned char *p, std::size_t n, double deadlineMs)
{
    std::size_t got = 0;
    while (got < n) {
#ifdef _WIN32
        const int rd = ::recv(static_cast<SOCKET>(s), reinterpret_cast<char *>(p + got),
                              static_cast<int>(n - got), 0);
#else
        const ssize_t rd = ::recv(s, p + got, n - got, 0);
#endif
        if (rd > 0) { got += static_cast<std::size_t>(rd); continue; }
        if (rd == 0) return ReadOutcome::Closed;
        const int e = pp_last_error();
#ifdef _WIN32
        const bool again = (e == WSAEWOULDBLOCK);
#else
        const bool again = (e == EAGAIN || e == EWOULDBLOCK || e == EINTR);
#endif
        if (!again) return ReadOutcome::Failed;
        if (!waitFor(s, /*forRead=*/true, deadlineMs)) return ReadOutcome::TimedOut;
    }
    return ReadOutcome::Ok;
}

bool writeMessage(pp_socket_t s, std::uint32_t tag, const std::string &xml, double deadlineMs)
{
    if (xml.size() > kMaxMessageBytes - kHeaderSize) return false;
    std::vector<unsigned char> out(kHeaderSize + xml.size());
    putU32le(&out[0], static_cast<std::uint32_t>(out.size()));   // length INCLUDES the header
    putU32le(&out[4], kProtocolVersion);
    putU32le(&out[8], kMessagePlist);
    putU32le(&out[12], tag);
    std::memcpy(&out[kHeaderSize], xml.data(), xml.size());
    return sendAll(s, out.data(), out.size(), deadlineMs);
}

// Reads one framed plist.  `tagOut` is the tag the daemon echoed.
bool readMessage(pp_socket_t s, double deadlineMs, std::uint32_t &tagOut, PlistNode &out,
                 Result &r)
{
    unsigned char hdr[kHeaderSize];
    switch (recvExact(s, hdr, kHeaderSize, deadlineMs)) {
    case ReadOutcome::Ok: break;
    case ReadOutcome::TimedOut: r.status = Status::Timeout; return false;
    default: r.status = Status::ProviderProtocol; r.systemError = pp_last_error(); return false;
    }

    const std::uint32_t length = u32le(hdr);
    const std::uint32_t version = u32le(hdr + 4);
    const std::uint32_t message = u32le(hdr + 8);
    tagOut = u32le(hdr + 12);

    if (version != kProtocolVersion || message != kMessagePlist || length < kHeaderSize ||
        length > kMaxMessageBytes) {
        r.status = Status::ProviderProtocol;
        return false;
    }

    std::string body(length - kHeaderSize, '\0');
    if (!body.empty()) {
        switch (recvExact(s, reinterpret_cast<unsigned char *>(&body[0]), body.size(),
                          deadlineMs)) {
        case ReadOutcome::Ok: break;
        case ReadOutcome::TimedOut: r.status = Status::Timeout; return false;
        default: r.status = Status::ProviderProtocol; return false;
        }
    }

    PlistScanner sc(body.data(), body.size());
    if (!sc.parse(out) || out.type != PlistNode::Type::Dict) {
        r.status = Status::ProviderProtocol;
        return false;
    }
    return true;
}

Status statusForResultNumber(long long n)
{
    switch (n) {
    case 0:  return Status::Ok;
    case 2:  return Status::UnknownDevice;
    case 3:  return Status::ConnectRefused;
    case 5:  return Status::BadVersion;
    default: return Status::UnexpectedResult;
    }
}

Device deviceFromProperties(DeviceId id, const PlistNode &props)
{
    Device d;
    d.deviceId = id;
    d.serialNumber = props.stringOr("SerialNumber");
    d.connectionType = props.stringOr("ConnectionType");
    d.connectionSpeed = static_cast<std::uint64_t>(props.intOr("ConnectionSpeed"));
    d.productId = static_cast<std::uint32_t>(props.intOr("ProductID"));
    d.locationId = static_cast<std::uint64_t>(props.intOr("LocationID"));
    return d;
}

// One counter for every connection this process opens.  The tag only has to be
// unique within a connection; monotonic across the process is simply cheaper
// than per-connection state and is just as correct.
std::uint32_t nextTag()
{
    static std::uint32_t tag = 0;
    return ++tag;
}

}  // namespace

// ── Public surface ─────────────────────────────────────────────────────────

const char *describe(Status s)
{
    switch (s) {
    case Status::Ok:               return "ok";
    case Status::NoProvider:       return "no usbmux provider (macOS: /var/run/usbmuxd; "
                                          "Windows: install Apple Devices; Linux: usbmuxd "
                                          "not running or not permitted)";
    case Status::ProviderProtocol: return "the usbmux socket did not speak the usbmux protocol";
    case Status::Timeout:          return "the usbmux provider did not answer in time";
    case Status::NoDevices:        return "no device attached (or a charge-only cable)";
    case Status::NoWiredDevices:   return "device is WiFi-paired, not cabled — not treated as wired";
    case Status::UnknownDevice:    return "the device is no longer attached";
    case Status::ConnectRefused:   return "connection refused inside the device — the capture "
                                          "app is not running or not in the foreground (or "
                                          "this computer is not trusted; M5 pending)";
    case Status::BadVersion:       return "the usbmux provider refused our client version";
    case Status::UnexpectedResult: return "the usbmux provider returned an unrecognised result";
    }
    return "unknown";
}

std::string Result::message() const
{
    std::string m = std::string("usbmux: ") + describe(status);
    if (muxResult >= 0) m += " (Number=" + std::to_string(muxResult) + ")";
    if (systemError != 0) m += " (errno=" + std::to_string(systemError) + ")";
    return m;
}

Provider Provider::platformDefault()
{
    Provider p;
#ifdef _WIN32
    // Apple Mobile Device Service.  ⚠ Phase 2 — openProvider() still refuses it.
    p.kind = Kind::Tcp;
    p.host = "127.0.0.1";
    p.port = 27015;
#else
    p.kind = Kind::Unix;
    p.path = "/var/run/usbmuxd";
#endif
    return p;
}

Provider Provider::unixSocket(std::string socketPath)
{
    Provider p;
    p.kind = Kind::Unix;
    p.path = std::move(socketPath);
    return p;
}

std::vector<Device> wiredOnly(const std::vector<Device> &all)
{
    std::vector<Device> out;
    out.reserve(all.size());
    for (const Device &d : all)
        if (d.isWired()) out.push_back(d);
    return out;
}

Client::Client(Provider provider) : m_provider(std::move(provider)) {}

Result Client::listDevices(std::vector<Device> &out, int timeoutMs)
{
    out.clear();
    Result r;
    const double deadline = nowMs() + timeoutMs;

    const pp_socket_t s = openProvider(m_provider, deadline, r);
    if (s == kInvalidSocket) return r;

    const std::uint32_t tag = nextTag();
    if (!writeMessage(s, tag, plistRequest("ListDevices", {}), deadline)) {
        pp_close_socket(s);
        r.status = Status::ProviderProtocol;
        return r;
    }

    std::uint32_t replyTag = 0;
    PlistNode reply;
    if (!readMessage(s, deadline, replyTag, reply, r)) {
        pp_close_socket(s);
        return r;
    }
    pp_close_socket(s);

    // §4.2: "Replies echo the request tag."  A reply that does not is a daemon
    // we are not in step with, and reading it would be guessing.
    if (replyTag != tag) {
        r.status = Status::ProviderProtocol;
        return r;
    }

    const PlistNode *list = reply.find("DeviceList");
    if (!list || list->type != PlistNode::Type::Array) {
        // A `Result` came back instead — that is the version gate answering.
        const PlistNode *num = reply.find("Number");
        if (num && num->type == PlistNode::Type::Integer) {
            r.muxResult = static_cast<int>(num->integer);
            r.status = statusForResultNumber(num->integer);
            if (r.status == Status::Ok) r.status = Status::ProviderProtocol;
            return r;
        }
        r.status = Status::ProviderProtocol;
        return r;
    }

    for (const PlistNode &entry : list->array) {
        if (entry.type != PlistNode::Type::Dict) continue;
        const PlistNode *props = entry.find("Properties");
        if (!props || props->type != PlistNode::Type::Dict) continue;
        out.push_back(deviceFromProperties(static_cast<DeviceId>(entry.intOr("DeviceID")), *props));
    }

    // Two DIFFERENT rows of the §6.2 table, and the caller gets to tell them
    // apart: nothing attached at all, versus attached but only over WiFi.
    if (out.empty())                          r.status = Status::NoDevices;
    else if (wiredOnly(out).empty())          r.status = Status::NoWiredDevices;
    else                                      r.status = Status::Ok;
    return r;
}

pp_socket_t Client::dial(DeviceId deviceId, std::uint16_t port, Result *diag, int timeoutMs)
{
    Result r;
    const double deadline = nowMs() + timeoutMs;
    const auto fail = [&](pp_socket_t s) {
        if (s != kInvalidSocket) pp_close_socket(s);
        if (diag) *diag = r;
        return kInvalidSocket;
    };

    const pp_socket_t s = openProvider(m_provider, deadline, r);
    if (s == kInvalidSocket) { if (diag) *diag = r; return kInvalidSocket; }

    // ⛔⛔ THE ONE THAT COSTS A DAY.  `PortNumber` goes on the wire BIG-ENDIAN —
    // usbmuxd reads the field as already network-order and applies ntohs — while
    // every other integer in the plist is native.  MEASURED against Apple's
    // daemon, twice, on 29 Aug 2026:
    //
    //     PortNumber 32498 (= htons(62078))  ->  Result Number = 0, connected
    //     PortNumber 62078 (native)          ->  Result Number = 3, refused
    //
    // ⚠ And read the failure mode, not just the failure: the native value did
    // not error at the mux, it DIALLED 32498.  On a busy device the wrong byte
    // order connects you to something else entirely and everything downstream
    // looks like a protocol bug.  htons() is the portable spelling — identity on
    // a big-endian host, which is exactly right because ntohs is identity there
    // too.  Callers pass a host-order port; this is the only place it is swapped.
    const long long wirePort = static_cast<long long>(htons(port));

    if (!writeMessage(s, nextTag(),
                      plistRequest("Connect", {{"DeviceID", static_cast<long long>(deviceId)},
                                               {"PortNumber", wirePort}}),
                      deadline)) {
        r.status = Status::ProviderProtocol;
        return fail(s);
    }

    std::uint32_t replyTag = 0;
    PlistNode reply;
    if (!readMessage(s, deadline, replyTag, reply, r)) return fail(s);

    const PlistNode *num = reply.find("Number");
    if (reply.stringOr("MessageType") != "Result" || !num ||
        num->type != PlistNode::Type::Integer) {
        r.status = Status::ProviderProtocol;
        return fail(s);
    }

    r.muxResult = static_cast<int>(num->integer);
    r.status = statusForResultNumber(num->integer);
    if (r.status != Status::Ok) return fail(s);

    // ✅ From here the socket IS the tunnel and carries no more plist (§4.2).
    // It is already non-blocking and SIGPIPE-safe from openProvider(), which is
    // what ConnectorConfig::dial's contract (C1) requires of what it returns.
    if (diag) *diag = r;
    return s;
}

// ── Watch ──────────────────────────────────────────────────────────────────

Watch::Watch() = default;

Watch::~Watch() { stop(); }

bool Watch::active() const { return m_fd != kInvalidSocket; }

pp_socket_t Watch::fd() const { return m_fd; }

Result Watch::start(Provider provider, int timeoutMs)
{
    stop();

    Result r;
    const double deadline = nowMs() + timeoutMs;

    const pp_socket_t s = openProvider(provider, deadline, r);
    if (s == kInvalidSocket) { m_lastError = r; return r; }

    const std::uint32_t tag = nextTag();
    if (!writeMessage(s, tag, plistRequest("Listen", {}), deadline)) {
        pp_close_socket(s);
        r.status = Status::ProviderProtocol;
        m_lastError = r;
        return r;
    }

    // ⛔ Read EXACTLY the Result and stop.  usbmuxd sends an `Attached` for every
    // already-present device immediately afterwards; recvExact() never over-reads,
    // so those are still in the socket buffer for the first poll() and no device
    // is missed between start() and the notifier being installed.
    std::uint32_t replyTag = 0;
    PlistNode reply;
    if (!readMessage(s, deadline, replyTag, reply, r)) {
        pp_close_socket(s);
        m_lastError = r;
        return r;
    }

    const PlistNode *num = reply.find("Number");
    if (!num || num->type != PlistNode::Type::Integer) {
        pp_close_socket(s);
        r.status = Status::ProviderProtocol;
        m_lastError = r;
        return r;
    }
    r.muxResult = static_cast<int>(num->integer);
    r.status = statusForResultNumber(num->integer);
    if (r.status != Status::Ok) {
        pp_close_socket(s);
        m_lastError = r;
        return r;
    }

    m_fd = s;
    m_buf.clear();
    m_lastError = Result{};
    return r;
}

bool Watch::poll(std::vector<Event> &out)
{
    if (m_fd == kInvalidSocket) return false;

    // Drain whatever is pending.  Never blocks: the fd is non-blocking and the
    // loop stops at EAGAIN.  A slow-but-alive daemon simply yields no events.
    for (;;) {
        unsigned char chunk[4096];
#ifdef _WIN32
        const int rd = ::recv(static_cast<SOCKET>(m_fd), reinterpret_cast<char *>(chunk),
                              static_cast<int>(sizeof chunk), 0);
#else
        const ssize_t rd = ::recv(m_fd, chunk, sizeof chunk, 0);
#endif
        if (rd > 0) {
            if (m_buf.size() + static_cast<std::size_t>(rd) > kMaxMessageBytes * 2) {
                m_lastError.status = Status::ProviderProtocol;
                return false;
            }
            m_buf.insert(m_buf.end(), chunk, chunk + rd);
            continue;
        }
        if (rd == 0) {
            // usbmuxd hung up.  Not an error state in the RV 3.6a sense — the
            // caller drops its notifier, calls stop(), and may retry later.
            m_lastError.status = Status::NoProvider;
            return false;
        }
        const int e = pp_last_error();
#ifdef _WIN32
        const bool again = (e == WSAEWOULDBLOCK);
#else
        const bool again = (e == EAGAIN || e == EWOULDBLOCK);
#endif
        if (e == EINTR) continue;
        if (again) break;
        m_lastError.status = Status::ProviderProtocol;
        m_lastError.systemError = e;
        return false;
    }

    std::size_t off = 0;
    while (m_buf.size() - off >= kHeaderSize) {
        const unsigned char *h = m_buf.data() + off;
        const std::uint32_t length = u32le(h);
        const std::uint32_t version = u32le(h + 4);
        const std::uint32_t message = u32le(h + 8);
        if (version != kProtocolVersion || message != kMessagePlist || length < kHeaderSize ||
            length > kMaxMessageBytes) {
            m_lastError.status = Status::ProviderProtocol;
            return false;
        }
        if (m_buf.size() - off < length) break;   // partial: wait for more

        PlistNode node;
        PlistScanner sc(reinterpret_cast<const char *>(h + kHeaderSize), length - kHeaderSize);
        if (!sc.parse(node) || node.type != PlistNode::Type::Dict) {
            m_lastError.status = Status::ProviderProtocol;
            return false;
        }
        off += length;

        const std::string type = node.stringOr("MessageType");
        if (type == "Attached") {
            const PlistNode *props = node.find("Properties");
            Event ev;
            ev.kind = EventKind::Attached;
            ev.deviceId = static_cast<DeviceId>(node.intOr("DeviceID"));
            if (props && props->type == PlistNode::Type::Dict)
                ev.device = deviceFromProperties(ev.deviceId, *props);
            else
                ev.device.deviceId = ev.deviceId;
            out.push_back(std::move(ev));
        } else if (type == "Detached") {
            Event ev;
            ev.kind = EventKind::Detached;
            ev.deviceId = static_cast<DeviceId>(node.intOr("DeviceID"));
            // ⚠ Detached carries the DeviceID and nothing else — no UDID.  The
            // caller must have remembered which udid that attachment was.
            ev.device.deviceId = ev.deviceId;
            out.push_back(std::move(ev));
        }
        // Anything else (`Paired`, and whatever a future daemon adds) is not our
        // business and is skipped rather than treated as a protocol error.
    }

    if (off > 0) m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(off));
    return true;
}

void Watch::stop()
{
    // ⛔ The caller's QSocketNotifier must already be gone; this closes the fd
    // the notifier was watching (ppcp_host_service.cpp:1205).
    if (m_fd != kInvalidSocket) {
        pp_close_socket(m_fd);
        m_fd = kInvalidSocket;
    }
    m_buf.clear();
}

}  // namespace Usbmux
}  // namespace Ppcp
