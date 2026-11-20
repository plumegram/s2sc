#include "adb.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

bool FileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

} // namespace

std::string Adb::Resolve(const std::string& hint) {
    if (!hint.empty() && hint != "adb" && FileExists(hint)) return hint;

    // PATH
    char found[MAX_PATH] = {0};
    if (SearchPathA(nullptr, "adb.exe", nullptr, MAX_PATH, found, nullptr)) return found;

    const char* envs[] = {"ANDROID_HOME", "ANDROID_SDK_ROOT"};
    for (const char* e : envs) {
        const char* v = std::getenv(e);
        if (!v || !*v) continue;
        std::string p = std::string(v) + "\\platform-tools\\adb.exe";
        if (FileExists(p)) return p;
    }

    const char* guesses[] = {
        "C:\\Android\\platform-tools\\adb.exe",
        "C:\\platform-tools\\adb.exe",
        "C:\\adb\\adb.exe",
        "C:\\Users\\Public\\platform-tools\\adb.exe",
    };
    for (const char* g : guesses)
        if (FileExists(g)) return g;

    return hint.empty() ? "adb" : hint;
}

std::string Adb::Prefix() const {
    std::string p = "\"" + m_path + "\"";
    if (!m_serial.empty()) p += " -s " + m_serial;
    return p;
}

ProcResult Adb::Run(const std::string& args, DWORD timeout_ms) const {
    ProcResult res;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return res;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::string cmdline = Prefix() + " " + args;

    std::vector<char> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        res.output = "failed to start adb";
        return res;
    }

    // Drain the pipe while the child runs, otherwise a full pipe buffer deadlocks it.
    char buf[4096];
    ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) break;
            buf[got] = 0;
            res.output += buf;
            continue;
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 20);
        if (w == WAIT_OBJECT_0) break;
        if (timeout_ms != INFINITE && GetTickCount64() - start > timeout_ms) {
            res.timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
    }
    DWORD code = 1;
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = 1;
    res.exit_code = code;

    // final drain
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(rd, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) break;
        buf[got] = 0;
        res.output += buf;
    }

    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return res;
}

bool Adb::ListDevices(std::vector<AdbDevice>& out, std::string& err) const {
    ProcResult r = Run("devices");
    if (r.output.empty()) { err = "adb produced no output (is it installed?)"; return false; }

    out.clear();
    std::istringstream ss(r.output);
    std::string line;
    bool in_list = false;
    while (std::getline(ss, line)) {
        line = Trim(line);
        if (line.rfind("List of devices", 0) == 0) { in_list = true; continue; }
        if (!in_list || line.empty()) continue;
        if (line.rfind("*", 0) == 0 || line.rfind("adb server", 0) == 0) continue;

        std::istringstream ls(line);
        AdbDevice d;
        ls >> d.serial >> d.state;
        if (!d.serial.empty()) out.push_back(d);
    }
    if (out.empty()) err = "no devices attached";
    return true;
}

bool Adb::Push(const std::string& local, const std::string& remote, std::string& err) const {
    ProcResult r = Run("push \"" + local + "\" \"" + remote + "\"", 60000);
    if (r.exit_code != 0) {
        err = "adb push failed: " + Trim(r.output);
        return false;
    }
    return true;
}

bool Adb::Forward(int local_port, int remote_port, std::string& err) const {
    char cmd[128];
    std::snprintf(cmd, sizeof(cmd), "forward tcp:%d tcp:%d", local_port, remote_port);
    ProcResult r = Run(cmd);
    if (r.exit_code != 0) {
        err = "adb forward failed: " + Trim(r.output);
        return false;
    }
    return true;
}

void Adb::RemoveForward(int local_port) const {
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "forward --remove tcp:%d", local_port);
    Run(cmd, 5000);
}

bool Adb::Shell(const std::string& cmd, std::string& out, std::string& err) const {
    ProcResult r = Run("shell " + cmd, 30000);
    out = Trim(r.output);
    if (r.exit_code != 0) {
        err = out.empty() ? "shell command failed" : out;
        return false;
    }
    return true;
}

int Adb::ProbeRoot() const {
    std::string out, err;
    if (Shell("su -c id", out, err) && out.find("uid=0") != std::string::npos) return 1;
    if (Shell("id", out, err) && out.find("uid=0") != std::string::npos) return 2;
    return 0;
}

// ---------------------------------------------------------------- AgentProcess

bool Adb::AgentProcess::Start(const Adb& adb, const std::string& remote_path, int port, bool use_su,
                              std::string& err) {
    Stop();

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { err = "CreatePipe failed"; return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = wr;
    si.hStdError   = wr;

    char inner[512];
    if (use_su) {
        std::snprintf(inner, sizeof(inner),
                      "su -c 'chmod 755 %s; %s %d'", remote_path.c_str(), remote_path.c_str(), port);
    } else {
        // adbd is already root (adb root / userdebug build): no su needed.
        std::snprintf(inner, sizeof(inner),
                      "chmod 755 %s; %s %d", remote_path.c_str(), remote_path.c_str(), port);
    }

    std::string cmdline = adb.Prefix() + " shell " + inner;
    std::vector<char> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back('\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        err = "failed to launch adb shell";
        return false;
    }

    m_proc    = pi.hProcess;
    m_pipe    = rd;
    m_running = true;
    m_stop    = false;
    CloseHandle(pi.hThread);
    m_reader = std::thread(&AgentProcess::ReaderLoop, this);
    return true;
}

void Adb::AgentProcess::ReaderLoop() {
    char buf[1024];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(m_pipe, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) break;
        buf[got] = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_output += buf;
            if (m_output.size() > 8192) m_output.erase(0, m_output.size() - 8192);
        }
        if (m_stop.load()) break;
    }
    m_running = false;
}

void Adb::AgentProcess::Stop() {
    m_stop = true;
    if (m_proc) {
        // Kill the adb shell; the remote su child is terminated with it.
        TerminateProcess(m_proc, 0);
        WaitForSingleObject(m_proc, 2000);
        CloseHandle(m_proc);
        m_proc = nullptr;
    }
    if (m_pipe) {
        CloseHandle(m_pipe);
        m_pipe = nullptr;
    }
    if (m_reader.joinable()) m_reader.join();
    m_running = false;
}

std::string Adb::AgentProcess::Output() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_output;
}
