#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adb.h"
#include "agent_client.h"
#include "config.h"
#include "log.h"
#include "proc_mem.h"
#include "skin_db.h"

struct App {
    Config     cfg;
    Log        log;
    Adb        adb;
    AgentClient client;
    ProcMem    mem{client};
    SkinDb     skins;

    std::string config_path;
    std::string agent_binary; // agent picked for the attached device's ABI
    int         root_mode = 0; // 0 none, 1 su, 2 adbd already root

    // connection
    Adb::AgentProcess agentProc;
    std::vector<AdbDevice> devices;
    int         device_index = -1;
    bool        agent_running = false;
    bool        connected     = false;
    bool        attached      = false;
    uint32_t    pid           = 0;
    ModuleSpan  module;

    // scan job
    std::thread            scan_thread;
    std::atomic<bool>      scan_running{false};
    std::atomic<bool>      scan_cancel{false};
    std::atomic<uint64_t>  scan_done{0};
    std::atomic<uint64_t>  scan_total{1};
    std::mutex             scan_mutex;
    std::vector<WeaponInstance> scan_result;

    // selection
    std::vector<WeaponInstance> candidates;
    int          selected_candidate = -1;
    WeaponInstance target;
    bool         has_target = false;

    // skin ui
    std::string  weapon_filter = "rifle";
    std::string  selected_weapon = "AKR12";
    std::string  skin_search;
    uint32_t     selected_skin = 0;
    uint32_t     scan_value = 0;
    bool         scan_structural = false; // ignore the seed, match on the structure alone

    // post-write watchdog: proves the game does not rewrite the field next tick
    bool         watch_active = false;
    uint64_t     watch_obj = 0;
    uint32_t     watch_expected = 0;
    std::chrono::steady_clock::time_point watch_start{};
    std::chrono::steady_clock::time_point watch_next{};

    // misc ui
    char         manual_addr[32] = "0";
    uint32_t     manual_value = 0;
    std::string  status_line;

    ~App();

    bool Init();
    void Shutdown();

    bool ConnectDevice();
    void DisconnectDevice();
    bool AttachGame();
    void DetachGame();

    void StartScan();
    void PollScan();
    bool ApplySkin(uint32_t skin_id);
    bool RefreshTarget();

    // Re-reads the skin field every 250 ms for a second after a write and logs
    // whether the game kept the value. Called once per frame; never blocks.
    void PollWatch();

    void SaveConfig();
    std::string ProcessName() const { return cfg.process_name; }
};
