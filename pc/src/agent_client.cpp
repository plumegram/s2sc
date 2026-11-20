#include "agent_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

using namespace s2sc;

namespace {

struct WinsockInit {
    WinsockInit() {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() { WSACleanup(); }
};
WinsockInit g_winsock;

bool RecvAll(SOCKET s, void* dst, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (n) {
        int r = ::recv(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool SendAll(SOCKET s, const void* src, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    while (n) {
        int r = ::send(s, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

} // namespace

AgentClient::~AgentClient() { Disconnect(); }

bool AgentClient::Connect(const std::string& host, int port, std::string& err) {
    Disconnect();

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "socket() failed"; return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<unsigned short>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::closesocket(s);
        err = "bad address: " + host;
        return false;
    }

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s);
        err = "connect failed (agent not running or forward not set)";
        return false;
    }

    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    DWORD tv = 4000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    m_sock = static_cast<uintptr_t>(s);
    return true;
}

void AgentClient::Disconnect() {
    if (m_sock) {
        ::closesocket(static_cast<SOCKET>(m_sock));
        m_sock = 0;
    }
}

bool AgentClient::Send(uint16_t op, const void* payload, uint32_t len) {
    if (!m_sock) { m_last_error = "not connected"; return false; }
    MsgHeader h{kMagic, op, 0, len};
    SOCKET s = static_cast<SOCKET>(m_sock);
    if (!SendAll(s, &h, sizeof(h))) { m_last_error = "send header failed"; return false; }
    if (len && !SendAll(s, payload, len)) { m_last_error = "send payload failed"; return false; }
    return true;
}

bool AgentClient::Recv(uint16_t expected_op, std::vector<uint8_t>& out, uint32_t max_len) {
    if (!m_sock) { m_last_error = "not connected"; return false; }
    SOCKET s = static_cast<SOCKET>(m_sock);

    MsgHeader h{};
    if (!RecvAll(s, &h, sizeof(h))) { m_last_error = "recv header failed"; return false; }
    if (h.magic != kMagic) { m_last_error = "bad magic"; return false; }
    if (h.op != expected_op) { m_last_error = "unexpected reply op"; return false; }
    if (h.len > max_len) { m_last_error = "reply too large"; return false; }

    out.resize(h.len);
    if (h.len && !RecvAll(s, out.data(), h.len)) { m_last_error = "recv payload failed"; return false; }
    return true;
}

bool AgentClient::Hello(HelloReply& out, std::string& err) {
    if (!Send(OpHello, nullptr, 0)) { err = m_last_error; return false; }
    std::vector<uint8_t> buf;
    if (!Recv(OpHello, buf) || buf.size() < sizeof(HelloReply)) { err = m_last_error; return false; }
    std::memcpy(&out, buf.data(), sizeof(out));
    return true;
}

bool AgentClient::ListProcs(std::vector<ProcEntry>& out, std::string& err) {
    if (!Send(OpListProc, nullptr, 0)) { err = m_last_error; return false; }
    std::vector<uint8_t> buf;
    if (!Recv(OpListProc, buf)) { err = m_last_error; return false; }
    size_t n = buf.size() / sizeof(ProcEntry);
    out.resize(n);
    if (n) std::memcpy(out.data(), buf.data(), n * sizeof(ProcEntry));
    return true;
}

bool AgentClient::Attach(uint32_t pid, std::string& err) {
    if (!Send(OpAttach, &pid, sizeof(pid))) { err = m_last_error; return false; }
    std::vector<uint8_t> buf;
    if (!Recv(OpAttach, buf) || buf.size() < sizeof(StatusReply)) { err = m_last_error; return false; }
    StatusReply st{};
    std::memcpy(&st, buf.data(), sizeof(st));
    if (st.status != 0) {
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "attach failed (errno %d)", -st.status);
        err = tmp;
        return false;
    }
    return true;
}

bool AgentClient::Regions(std::vector<RegionEntry>& out, std::string& err) {
    if (!Send(OpRegions, nullptr, 0)) { err = m_last_error; return false; }
    std::vector<uint8_t> buf;
    if (!Recv(OpRegions, buf)) { err = m_last_error; return false; }
    size_t n = buf.size() / sizeof(RegionEntry);
    out.resize(n);
    if (n) std::memcpy(out.data(), buf.data(), n * sizeof(RegionEntry));
    return true;
}

bool AgentClient::Read(uint64_t addr, void* dst, uint32_t len) {
    struct {
        uint64_t addr;
        uint32_t len;
    } req{addr, len};

    if (!Send(OpRead, &req, sizeof(req))) return false;
    std::vector<uint8_t> buf;
    if (!Recv(OpRead, buf, len + 64)) return false;
    if (buf.size() < sizeof(int32_t) + len) return false;
    int32_t status = 0;
    std::memcpy(&status, buf.data(), sizeof(status));
    if (status != 0) return false;
    std::memcpy(dst, buf.data() + sizeof(status), len);
    return true;
}

bool AgentClient::Write(uint64_t addr, const void* src, uint32_t len) {
    std::vector<uint8_t> payload(sizeof(uint64_t) + sizeof(uint32_t) + len);
    std::memcpy(payload.data(), &addr, sizeof(addr));
    std::memcpy(payload.data() + sizeof(addr), &len, sizeof(len));
    std::memcpy(payload.data() + sizeof(addr) + sizeof(len), src, len);

    if (!Send(OpWrite, payload.data(), static_cast<uint32_t>(payload.size()))) return false;
    std::vector<uint8_t> buf;
    if (!Recv(OpWrite, buf) || buf.size() < sizeof(StatusReply)) return false;
    StatusReply st{};
    std::memcpy(&st, buf.data(), sizeof(st));
    return st.status == 0;
}

bool AgentClient::Chain(const std::vector<ChainStep>& steps, ChainReply& out, std::string& err) {
    if (steps.empty()) { err = "empty chain"; return false; }
    std::vector<uint8_t> payload(sizeof(uint32_t) + steps.size() * sizeof(ChainStep));
    uint32_t count = static_cast<uint32_t>(steps.size());
    std::memcpy(payload.data(), &count, sizeof(count));
    std::memcpy(payload.data() + sizeof(count), steps.data(), steps.size() * sizeof(ChainStep));

    if (!Send(OpChain, payload.data(), static_cast<uint32_t>(payload.size()))) { err = m_last_error; return false; }
    std::vector<uint8_t> buf;
    if (!Recv(OpChain, buf)) { err = m_last_error; return false; }
    if (buf.size() < sizeof(ChainReply)) {
        err = "chain failed (pointer read)";
        return false;
    }
    std::memcpy(&out, buf.data(), sizeof(out));
    return true;
}

bool AgentClient::Ping(uint64_t token) {
    if (!Send(OpPing, &token, sizeof(token))) return false;
    std::vector<uint8_t> buf;
    if (!Recv(OpPing, buf) || buf.size() < sizeof(token)) return false;
    uint64_t back = 0;
    std::memcpy(&back, buf.data(), sizeof(back));
    return back == token;
}
