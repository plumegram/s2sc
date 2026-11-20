#include "app.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>

namespace {

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

bool Exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Look for a file next to the exe, one and two levels up (build layouts), then cwd.
std::string FindFile(const std::string& relative) {
    std::string dir = ExeDir();
    std::string candidates[] = {
        JoinPath(dir, relative),
        JoinPath(JoinPath(dir, ".."), relative),
        JoinPath(JoinPath(JoinPath(dir, ".."), ".."), relative),
        JoinPath(JoinPath(JoinPath(JoinPath(dir, ".."), ".."), ".."), relative),
        relative,
    };
    for (const std::string& c : candidates)
        if (Exists(c)) return c;
    return JoinPath(dir, relative);
}

} // namespace

App::~App() {
    if (scan_thread.joinable()) {
        scan_cancel = true;
        scan_thread.join();
    }
    client.Disconnect();
    adb.RemoveForward(cfg.port);
    agent_running = false;
}

bool App::Init() {
    config_path = FindFile("config\\targets.json");

    std::string err;
    if (!cfg.Load(config_path, err)) {
        log.Warn("%s", err.c_str());
        log.Warn("using built-in defaults, config will be created on save");
    }

    // Relative data paths are resolved against the config directory first.
    std::string cfg_dir = config_path.substr(0, config_path.find_last_of("\\/"));
    auto resolve = [&](std::string& p) {
        if (p.empty()) return;
        if (Exists(p)) return;
        std::string alt = JoinPath(cfg_dir, p);
        if (Exists(alt)) { p = alt; return; }
        std::string alt2 = FindFile(p);
        if (Exists(alt2)) p = alt2;
    };
    resolve(cfg.skins_json);
    resolve(cfg.skins_enum);

    adb.SetPath(Adb::Resolve(cfg.adb_path));
    log.Info("adb: %s", adb.Path().c_str());

    if (!skins.Load(cfg.skins_json, cfg.skins_enum, err)) {
        log.Error("skin catalogue: %s", err.c_str());
    } else {
        log.Ok("skin catalogue: %zu entries", skins.Size());
    }

    mem.SetLayout(cfg);

    if (!cfg.LayoutComplete())
        log.Warn("layout incomplete, missing: %s", cfg.DescribeMissing().c_str());
    else
        log.Info("layout: wc.skin=0x%X wc.mc=0x%X wc.owner=0x%X mc.weapon=0x%X mc.skin=0x%X",
                 cfg.weapon_controller.skin_id.Get(), cfg.weapon_controller.material_controller.Get(),
                 cfg.weapon_controller.owner.Get(), cfg.material_controller.weapon_id.Get(),
                 cfg.material_controller.skin_id.Get());

    adb_agent_local = FindFile("agent\\build\\s2sc_agent");
    return true;
}

void App::Shutdown() {
    DetachGame();
    DisconnectDevice();
}

bool App::ConnectDevice() {
    std::string err;

    if (device_index < 0 || device_index >= static_cast<int>(devices.size())) {
        devices.clear();
        if (!adb.ListDevices(devices, err) || devices.empty()) {
            status_line = err.empty() ? "no devices" : err;
            log.Error("devices: %s", status_line.c_str());
            return false;
        }
        device_index = 0;
    }

    adb.SetSerial(devices[device_index].serial);
    log.Info("device: %s (%s)", adb.Serial().c_str(), devices[device_index].state.c_str());

    // Kill a stale agent from a previous run so the port is free.
    adb.Run("shell su -c \"pkill -f s2sc_agent\"", 8000);

    if (!adb.Forward(cfg.port, cfg.port, err)) {
        log.Error("%s", err.c_str());
        status_line = err;
        return false;
    }

    if (!adb_agent_local.empty() && Exists(adb_agent_local)) {
        if (!adb.Push(adb_agent_local, cfg.remote_agent, err))
            log.Warn("push failed: %s", err.c_str());
        else
            log.Info("agent pushed to %s", cfg.remote_agent.c_str());
    } else {
        log.Warn("local agent binary not found at agent\\build\\s2sc_agent, assuming it is already on the device");
    }

    std::string proc_err;
    if (!agentProc.Start(adb, cfg.remote_agent, cfg.port, proc_err)) {
        log.Error("agent start: %s", proc_err.c_str());
        status_line = proc_err;
        return false;
    }
    agent_running = true;

    bool ok = false;
    for (int i = 0; i < 24; ++i) {
        if (client.Connect("127.0.0.1", cfg.port, err)) { ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!ok) {
        std::string out = agentProc.Output();
        log.Error("agent connect: %s", err.c_str());
        if (!out.empty()) log.Error("agent output: %s", out.c_str());
        status_line = err;
        return false;
    }

    s2sc::HelloReply hello{};
    if (!client.Hello(hello, err)) {
        log.Error("handshake: %s", err.c_str());
        status_line = err;
        client.Disconnect();
        return false;
    }

    connected = true;
    status_line = "agent online";
    log.Ok("agent online, proto %u, page %u", hello.proto, hello.page_size);
    return true;
}

void App::DisconnectDevice() {
    DetachGame();
    client.Disconnect();
    connected = false;
    agentProc.Stop();
    // TerminateProcess on adb.exe does not reliably reap the remote su child.
    adb.Run("shell su -c \"pkill -f s2sc_agent\"", 8000);
    agent_running = false;
    adb.RemoveForward(cfg.port);
}

bool App::AttachGame() {
    if (!connected) { status_line = "agent offline"; return false; }

    std::string err;
    std::vector<s2sc::ProcEntry> procs;
    if (!client.ListProcs(procs, err)) {
        log.Error("process list: %s", err.c_str());
        status_line = err;
        return false;
    }

    uint32_t found = 0;
    for (const auto& p : procs) {
        if (cfg.process_name == p.name) { found = p.pid; break; }
    }
    if (!found) {
        status_line = "process not running: " + cfg.process_name;
        log.Error("%s", status_line.c_str());
        return false;
    }

    if (!client.Attach(found, err)) {
        log.Error("attach %u: %s", found, err.c_str());
        status_line = err;
        return false;
    }

    pid = found;
    attached = true;

    mem.SetLayout(cfg);
    module = mem.FindModule(cfg.module_name, err);
    if (!module.valid) {
        log.Error("module: %s", err.c_str());
        status_line = err;
        return false;
    }

    log.Ok("attached pid %u, %s base 0x%llX size 0x%llX", pid, cfg.module_name.c_str(),
           static_cast<unsigned long long>(module.base), static_cast<unsigned long long>(module.size));
    status_line = "attached";
    return true;
}

void App::DetachGame() {
    if (scan_thread.joinable()) {
        scan_cancel = true;
        scan_thread.join();
    }
    scan_running = false;
    attached = false;
    has_target = false;
    candidates.clear();
    selected_candidate = -1;
}

void App::StartScan() {
    if (!attached) { status_line = "not attached"; return; }
    if (scan_running.load()) return;
    if (!cfg.weapon_controller.skin_id) { status_line = "layout incomplete"; return; }

    if (scan_thread.joinable()) scan_thread.join();

    scan_cancel = false;
    scan_done    = 0;
    scan_total   = 1;
    scan_running = true;

    uint32_t value = scan_value;
    scan_thread = std::thread([this, value]() {
        std::vector<WeaponInstance> hits;
        mem.ScanForSkin(
            value, hits,
            [this](uint64_t done, uint64_t total) {
                scan_done  = done;
                scan_total = total ? total : 1;
            },
            scan_cancel);

        {
            std::lock_guard<std::mutex> lock(scan_mutex);
            scan_result = std::move(hits);
        }
        scan_running = false;
    });
}

void App::PollScan() {
    if (scan_running.load() || !scan_thread.joinable()) return;

    scan_thread.join();
    std::lock_guard<std::mutex> lock(scan_mutex);
    candidates = scan_result;
    selected_candidate = candidates.empty() ? -1 : 0;
    has_target = false;

    if (candidates.empty()) {
        log.Warn("scan: no structurally valid weapon objects for skin %u", scan_value);
        status_line = "scan: 0 hits";
    } else {
        log.Ok("scan: %zu candidate(s)", candidates.size());
        status_line = "scan: " + std::to_string(candidates.size()) + " hits";
    }
    log.Info("scan rejects: %s", mem.last_scan_report.c_str());
}

bool App::ApplySkin(uint32_t skin_id) {
    if (!has_target) { status_line = "no target"; return false; }

    // The weapon view can be rebuilt on a weapon switch, leaving the cached
    // pointers dangling. Re-validate before every write so we never poke a freed
    // object.
    WeaponInstance fresh;
    std::string why;
    if (!mem.Validate(target.obj, fresh, why)) {
        log.Error("target is stale (%s), re-scan needed", why.c_str());
        has_target = false;
        status_line = "target stale";
        return false;
    }
    target = fresh;

    std::string err;
    if (!mem.WriteSkin(target, skin_id, err)) {
        log.Error("apply: %s", err.c_str());
        status_line = err;
        return false;
    }

    target.skin = skin_id;
    log.Ok("skin %u written to weapon %s (obj 0x%llX)", skin_id,
           skins.WeaponNameById(target.weapon), static_cast<unsigned long long>(target.obj));
    status_line = "applied";
    return true;
}

bool App::RefreshTarget() {
    if (!has_target) return false;
    uint32_t v = 0;
    if (!mem.ReadSkin(target, v)) {
        log.Error("read skin failed");
        return false;
    }
    target.skin = v;
    return true;
}

void App::SaveConfig() {
    std::string err;
    if (!cfg.Save(config_path, err))
        log.Error("%s", err.c_str());
    else
        log.Info("config saved: %s", config_path.c_str());
    mem.SetLayout(cfg);
}
