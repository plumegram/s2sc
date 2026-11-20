#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AdbDevice {
    std::string serial;
    std::string state;
};

struct ProcResult {
    std::string output;
    DWORD       exit_code = 1;
    bool        timed_out = false;
};

class Adb {
public:
    // Resolve the adb executable: explicit path, then PATH, then common SDK dirs.
    static std::string Resolve(const std::string& hint);

    bool ListDevices(std::vector<AdbDevice>& out, std::string& err) const;

    ProcResult Run(const std::string& args, DWORD timeout_ms = 20000) const;
    bool       Push(const std::string& local, const std::string& remote, std::string& err) const;
    bool       Forward(int local_port, int remote_port, std::string& err) const;
    void       RemoveForward(int local_port) const;
    bool       Shell(const std::string& cmd, std::string& out, std::string& err) const;

    // How this device can reach uid 0:
    //   0 = no root, 1 = su binary works, 2 = adbd itself already runs as root
    int ProbeRoot() const;

    void SetPath(std::string p) { m_path = std::move(p); }
    const std::string& Path() const { return m_path; }
    void SetSerial(std::string s) { m_serial = std::move(s); }
    const std::string& Serial() const { return m_serial; }

    // Launches the agent under `su` and keeps the adb shell alive for as long as
    // the process object exists. Output is captured for diagnostics.
    class AgentProcess {
    public:
        ~AgentProcess() { Stop(); }
        bool Start(const Adb& adb, const std::string& remote_path, int port, bool use_su, std::string& err);
        void Stop();
        bool Running() const { return m_running.load(); }
        std::string Output() const;

    private:
        void ReaderLoop();

        HANDLE                   m_proc = nullptr;
        HANDLE                   m_pipe = nullptr;
        std::thread              m_reader;
        std::atomic<bool>        m_running{false};
        std::atomic<bool>        m_stop{false};
        mutable std::mutex       m_mutex;
        std::string              m_output;
    };

private:
    std::string m_path = "adb";
    std::string m_serial;
    std::string Prefix() const;
};
