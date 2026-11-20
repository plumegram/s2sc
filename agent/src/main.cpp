// s2sc device agent. Runs on the Android device, speaks the s2sc protocol over
// a loopback TCP socket that the PC reaches through `adb forward`.
//
// Build: aarch64-linux-android21-clang++ -O2 -std=c++17 -static-libstdc++ ...
// See agent/build.bat. Requires root on the device: /proc/<pid>/mem of another
// app is not readable by shell on a production build.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "proto.h"

using namespace s2sc;

namespace {

struct Region {
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t offset;
    char     name[128];
};

int  g_mem_fd = -1;
int  g_pid    = 0;
std::vector<Region> g_regions;

// ---------------------------------------------------------------- socket io

bool read_full(int fd, void* dst, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    while (n) {
        ssize_t r = ::recv(fd, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool write_full(int fd, const void* src, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    while (n) {
        ssize_t r = ::send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

bool send_msg(int fd, uint16_t op, const void* payload, uint32_t len) {
    MsgHeader h{kMagic, op, 0, len};
    if (!write_full(fd, &h, sizeof(h))) return false;
    if (len && !write_full(fd, payload, len)) return false;
    return true;
}

// ------------------------------------------------------------- proc parsing

std::string read_file(const char* path, size_t cap = 1 << 20) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    // /proc is seq_file backed: one read() returns only the first chunk, so a
    // single 16 MiB read of /proc/<pid>/maps yields ~5 KiB and silently loses
    // the rest of the map. Keep reading until EOF or the cap.
    std::string out;
    out.resize(cap);
    size_t done = 0;
    while (done < cap) {
        ssize_t n = ::read(fd, out.data() + done, cap - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        done += static_cast<size_t>(n);
    }
    ::close(fd);
    if (done == 0) return {};
    out.resize(done);
    return out;
}

bool parse_maps() {
    g_regions.clear();
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", g_pid);
    std::string maps = read_file(path, 16u << 20);
    if (maps.empty()) return false;

    size_t pos = 0;
    while (pos < maps.size()) {
        size_t eol = maps.find('\n', pos);
        if (eol == std::string::npos) eol = maps.size();
        std::string line = maps.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;

        Region r{};
        char perms[8] = {0};
        unsigned long long s = 0, e = 0, off = 0;
        int consumed = 0;
        // start-end perms offset dev inode path
        if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &s, &e, perms, &off, &consumed) < 4)
            continue;
        r.start  = s;
        r.end    = e;
        r.offset = static_cast<uint32_t>(off);
        r.prot   = (perms[0] == 'r' ? kProtRead : 0u) | (perms[1] == 'w' ? kProtWrite : 0u) |
                 (perms[2] == 'x' ? kProtExec : 0u);
        if (consumed > 0 && static_cast<size_t>(consumed) < line.size()) {
            std::string name = line.substr(static_cast<size_t>(consumed));
            while (!name.empty() && (name.back() == ' ' || name.back() == '\n')) name.pop_back();
            std::snprintf(r.name, sizeof(r.name), "%s", name.c_str());
        }
        g_regions.push_back(r);
    }
    return !g_regions.empty();
}

bool range_ok(uint64_t addr, uint64_t len) {
    if (len == 0 || addr + len < addr) return false;
    for (const Region& r : g_regions) {
        if (addr >= r.start && addr + len <= r.end && (r.prot & kProtRead)) return true;
    }
    return false;
}

bool mem_read(uint64_t addr, void* dst, size_t len) {
    if (g_mem_fd < 0) return false;
    if (!range_ok(addr, len)) return false;
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
        ssize_t r = ::pread(g_mem_fd, p + done, len - done, static_cast<off_t>(addr + done));
        if (r <= 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}

bool mem_write(uint64_t addr, const void* src, size_t len) {
    if (g_mem_fd < 0) return false;
    if (g_pid <= 0) return false;
    const uint8_t* p = static_cast<const uint8_t*>(src);
    size_t done = 0;
    while (done < len) {
        ssize_t r = ::pwrite(g_mem_fd, p + done, len - done, static_cast<off_t>(addr + done));
        if (r <= 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}

bool read_u64(uint64_t addr, uint64_t& out) { return mem_read(addr, &out, sizeof(out)); }

int find_pid(const char* want) {
    DIR* d = ::opendir("/proc");
    if (!d) return 0;
    int found = 0;
    while (dirent* de = ::readdir(d)) {
        char* endp = nullptr;
        long pid = std::strtol(de->d_name, &endp, 10);
        if (!endp || *endp || pid <= 0) continue;
        char p[64];
        std::snprintf(p, sizeof(p), "/proc/%ld/cmdline", pid);
        std::string cl = read_file(p, 512);
        if (!cl.empty()) {
            // cmdline is NUL separated; first token is the process name
            std::string first = cl.substr(0, cl.find('\0'));
            if (first == want) { found = static_cast<int>(pid); break; }
        }
        std::snprintf(p, sizeof(p), "/proc/%ld/comm", pid);
        std::string comm = read_file(p, 128);
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\0')) comm.pop_back();
        if (comm == want) { found = static_cast<int>(pid); break; }
    }
    ::closedir(d);
    return found;
}

void detach() {
    if (g_mem_fd >= 0) { ::close(g_mem_fd); g_mem_fd = -1; }
    g_pid = 0;
    g_regions.clear();
}

// ----------------------------------------------------------------- handlers

void on_hello(int fd) {
    HelloReply r{};
    r.proto     = kProtoVersion;
    r.pid_max   = 32768;
    r.page_size = static_cast<uint32_t>(::sysconf(_SC_PAGESIZE));
#if defined(__aarch64__)
    constexpr const char* kArch = "arm64";
#elif defined(__x86_64__)
    constexpr const char* kArch = "x86_64";
#elif defined(__arm__)
    constexpr const char* kArch = "arm32";
#elif defined(__i386__)
    constexpr const char* kArch = "x86";
#else
    constexpr const char* kArch = "unknown";
#endif
    std::snprintf(r.build, sizeof(r.build), "s2sc-agent/1 %s", kArch);
    send_msg(fd, OpHello, &r, sizeof(r));
}

void on_list_proc(int fd) {
    DIR* d = ::opendir("/proc");
    if (!d) { send_msg(fd, OpListProc, nullptr, 0); return; }
    std::vector<ProcEntry> out;
    while (dirent* de = ::readdir(d)) {
        char* endp = nullptr;
        long pid = std::strtol(de->d_name, &endp, 10);
        if (!endp || *endp || pid <= 0) continue;
        char p[64];
        std::snprintf(p, sizeof(p), "/proc/%ld/cmdline", pid);
        std::string cl = read_file(p, 512);
        ProcEntry e{};
        e.pid = static_cast<uint32_t>(pid);
        if (!cl.empty()) {
            std::string first = cl.substr(0, cl.find('\0'));
            std::snprintf(e.name, sizeof(e.name), "%s", first.c_str());
        } else {
            std::snprintf(p, sizeof(p), "/proc/%ld/comm", pid);
            std::string comm = read_file(p, 128);
            while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\0')) comm.pop_back();
            std::snprintf(e.name, sizeof(e.name), "%s", comm.c_str());
        }
        if (e.name[0]) out.push_back(e);
    }
    ::closedir(d);
    send_msg(fd, OpListProc, out.data(), static_cast<uint32_t>(out.size() * sizeof(ProcEntry)));
}

void on_attach(int fd, uint32_t pid) {
    detach();
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%u/mem", pid);
    int fdmem = ::open(path, O_RDWR | O_CLOEXEC);
    if (fdmem < 0) fdmem = ::open(path, O_RDONLY | O_CLOEXEC);
    StatusReply st{};
    if (fdmem < 0) {
        st.status = -errno;
        send_msg(fd, OpAttach, &st, sizeof(st));
        return;
    }
    g_mem_fd = fdmem;
    g_pid    = static_cast<int>(pid);
    if (!parse_maps()) {
        st.status = -EIO;
        detach();
        send_msg(fd, OpAttach, &st, sizeof(st));
        return;
    }
    st.status = 0;
    send_msg(fd, OpAttach, &st, sizeof(st));
}

void on_regions(int fd) {
    std::vector<RegionEntry> out;
    out.reserve(g_regions.size());
    for (const Region& r : g_regions) {
        RegionEntry e{};
        e.start  = r.start;
        e.end    = r.end;
        e.prot   = r.prot;
        e.offset = r.offset;
        std::snprintf(e.name, sizeof(e.name), "%s", r.name);
        out.push_back(e);
    }
    send_msg(fd, OpRegions, out.data(), static_cast<uint32_t>(out.size() * sizeof(RegionEntry)));
}

void on_read(int fd, uint64_t addr, uint32_t len) {
    if (len > (64u << 20)) {
        StatusReply st{-EMSGSIZE};
        send_msg(fd, OpRead, &st, sizeof(st));
        return;
    }
    std::vector<uint8_t> buf(len);
    if (!mem_read(addr, buf.data(), len)) {
        StatusReply st{-EIO};
        send_msg(fd, OpRead, &st, sizeof(st));
        return;
    }
    // payload: i32 status then raw bytes
    std::vector<uint8_t> out(sizeof(int32_t) + len);
    int32_t ok = 0;
    std::memcpy(out.data(), &ok, sizeof(ok));
    std::memcpy(out.data() + sizeof(ok), buf.data(), len);
    send_msg(fd, OpRead, out.data(), static_cast<uint32_t>(out.size()));
}

void on_write(int fd, uint64_t addr, const std::vector<uint8_t>& bytes) {
    StatusReply st{};
    st.status = mem_write(addr, bytes.data(), bytes.size()) ? 0 : -EIO;
    send_msg(fd, OpWrite, &st, sizeof(st));
}

void on_chain(int fd, const std::vector<uint8_t>& payload) {
    if (payload.size() < sizeof(uint32_t)) {
        StatusReply st{-EINVAL};
        send_msg(fd, OpChain, &st, sizeof(st));
        return;
    }
    uint32_t count = 0;
    std::memcpy(&count, payload.data(), sizeof(count));
    if (count == 0 || count > 16 || payload.size() < sizeof(uint32_t) + count * sizeof(ChainStep)) {
        StatusReply st{-EINVAL};
        send_msg(fd, OpChain, &st, sizeof(st));
        return;
    }
    std::vector<ChainStep> steps(count);
    std::memcpy(steps.data(), payload.data() + sizeof(uint32_t), count * sizeof(ChainStep));

    uint64_t cur = steps[0].base + static_cast<int64_t>(steps[0].off);
    for (uint32_t i = 1; i < count; ++i) {
        uint64_t ptr = 0;
        if (!read_u64(cur, ptr) || ptr == 0) {
            StatusReply st{-EFAULT};
            send_msg(fd, OpChain, &st, sizeof(st));
            return;
        }
        cur = ptr + static_cast<int64_t>(steps[i].off);
    }
    ChainReply rep{};
    rep.addr = cur;
    read_u64(cur, rep.value);
    send_msg(fd, OpChain, &rep, sizeof(rep));
}

// -------------------------------------------------------------------- main

void serve(int fd) {
    for (;;) {
        MsgHeader h{};
        if (!read_full(fd, &h, sizeof(h))) return;
        if (h.magic != kMagic || h.len > (64u << 20)) return;

        std::vector<uint8_t> payload(h.len);
        if (h.len && !read_full(fd, payload.data(), h.len)) return;

        switch (h.op) {
        case OpHello:
            on_hello(fd);
            break;
        case OpListProc:
            on_list_proc(fd);
            break;
        case OpAttach: {
            uint32_t pid = 0;
            if (payload.size() >= sizeof(pid)) std::memcpy(&pid, payload.data(), sizeof(pid));
            on_attach(fd, pid);
            break;
        }
        case OpDetach:
            detach();
            send_msg(fd, OpDetach, nullptr, 0);
            break;
        case OpRegions:
            on_regions(fd);
            break;
        case OpRead: {
            uint64_t addr = 0;
            uint32_t len  = 0;
            if (payload.size() < sizeof(addr) + sizeof(len)) return;
            std::memcpy(&addr, payload.data(), sizeof(addr));
            std::memcpy(&len, payload.data() + sizeof(addr), sizeof(len));
            on_read(fd, addr, len);
            break;
        }
        case OpWrite: {
            uint64_t addr = 0;
            uint32_t len  = 0;
            if (payload.size() < sizeof(addr) + sizeof(len)) return;
            std::memcpy(&addr, payload.data(), sizeof(addr));
            std::memcpy(&len, payload.data() + sizeof(addr), sizeof(len));
            if (payload.size() < sizeof(addr) + sizeof(len) + len) return;
            std::vector<uint8_t> bytes(len);
            std::memcpy(bytes.data(), payload.data() + sizeof(addr) + sizeof(len), len);
            on_write(fd, addr, bytes);
            break;
        }
        case OpChain:
            on_chain(fd, payload);
            break;
        case OpPing: {
            uint64_t t = 0;
            if (payload.size() >= sizeof(t)) std::memcpy(&t, payload.data(), sizeof(t));
            send_msg(fd, OpPing, &t, sizeof(t));
            break;
        }
        default:
            return;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    int port = kDefaultPort;
    if (argc > 1) port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) port = kDefaultPort;

    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::perror("socket"); return 1; }
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }
    if (::listen(srv, 4) < 0) {
        std::perror("listen");
        return 1;
    }

    std::printf("S2SC-AGENT READY port=%d pid=%d\n", port, static_cast<int>(::getpid()));
    std::fflush(stdout);

    for (;;) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int c = ::accept(srv, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        serve(c);
        ::close(c);
    }
    ::close(srv);
    detach();
    return 0;
}
