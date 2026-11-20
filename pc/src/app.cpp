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

std::string PickExisting(const std::vector<std::string>& relatives) {
    for (const std::string& rel : relatives) {
        std::string p = FindFile(rel);
        if (Exists(p)) return p;
    }
    return {};
}

// e_machine of a remote ELF header: 0x3e = x86-64, 0xb7 = AArch64, 0x28 = ARM32.
// Returns the raw value, or -1 when the header cannot be read.
int RemoteElfMachine(const Adb& adb, const std::string& path, std::string& why) {
    std::string out, err;
    if (!adb.Shell("od -An -tx1 -j18 -N2 " + path, out, err)) {
        why = err.empty() ? "od failed" : err;
        return -1;
    }
    unsigned lo = 0, hi = 0;
    if (std::sscanf(out.c_str(), "%x %x", &lo, &hi) != 2) {
        why = "unexpected od output: " + out;
        return -1;
    }
    return static_cast<int>(lo | (hi << 8));
}

const char* MachineName(int machine) {
    switch (machine) {
        case 0x3e: return "x86-64";
        case 0xb7: return "aarch64";
        case 0x28: return "arm32";
        default:   return "unknown";
    }
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
        log.Info("layout: weapon=0x%X skin=0x%X mag=0x%X ammo=0x%X reserve=0x%X",
                 cfg.player_animation.weapon_id.Get(), cfg.player_animation.skin_id.Get(),
                 cfg.player_animation.magazine_capacity.Get(), cfg.player_animation.ammo.Get(),
                 cfg.player_animation.rest_ammo.Get());

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

    // Emulators are x86_64, phones are arm64. Pick the matching agent binary.
    // Never fall back to the other ABI: the guest cannot exec a foreign ELF and
    // the only symptom is "not executable: 64-bit ELF file" much later.
    std::string abi, shell_err;
    if (adb.Shell("getprop ro.product.cpu.abi", abi, shell_err))
        log.Info("abi: %s", abi.c_str());
    else
        log.Warn("abi query failed: %s", shell_err.c_str());

    const bool want_x64 = abi.find("x86") != std::string::npos || abi.find("i686") != std::string::npos;
    agent_binary = want_x64
        ? PickExisting({"agent\\build\\s2sc_agent_x64", "agent\\build\\s2sc_agent"})
        : PickExisting({"agent\\build\\s2sc_agent_arm64", "agent\\build\\s2sc_agent"});

    root_mode = adb.ProbeRoot();
    const bool use_su = (root_mode == 1);
    if (root_mode == 0)
        log.Warn("no root: su is missing and adbd is not root, /proc/<pid>/mem will be denied");
    else
        log.Info("root via %s", root_mode == 1 ? "su" : "adbd (already root)");

    // Kill a stale agent from a previous run so the port is free.
    adb.Run(std::string("shell ") + (use_su ? "su -c \"pkill -f s2sc_agent\"" : "pkill -f s2sc_agent"), 8000);

    if (!adb.Forward(cfg.port, cfg.port, err)) {
        log.Error("%s", err.c_str());
        status_line = err;
        return false;
    }

    if (!agent_binary.empty()) {
        if (!adb.Push(agent_binary, cfg.remote_agent, err))
            log.Warn("push failed: %s", err.c_str());
        else
            log.Info("agent pushed: %s", agent_binary.c_str());
    } else {
        log.Warn("no local agent binary found under agent\\build, assuming it is already on the device");
    }

    // Verify the binary that actually sits on the device matches the guest ABI.
    // This catches a stale push and a wrong local build before anything is launched.
    {
        std::string why;
        const int machine = RemoteElfMachine(adb, cfg.remote_agent, why);
        const int want = want_x64 ? 0x3e : 0xb7;
        if (machine < 0) {
            log.Warn("agent ABI check skipped: %s", why.c_str());
        } else if (machine != want) {
            std::string msg = std::string("agent ABI mismatch: device is ") + (want_x64 ? "x86-64" : "aarch64") +
                              ", binary on device is " + MachineName(machine) +
                              " (" + cfg.remote_agent + ")";
            log.Error("%s", msg.c_str());
            status_line = msg;
            return false;
        } else {
            log.Ok("agent ABI ok: %s", MachineName(machine));
        }
    }

    std::string proc_err;
    if (!agentProc.Start(adb, cfg.remote_agent, cfg.port, use_su, proc_err)) {
        log.Error("agent start: %s", proc_err.c_str());
        status_line = proc_err;
        return false;
    }
    agent_running = true;

    // `adb forward` accepts the local TCP connection even when nothing is
    // listening on the device yet, so a successful Connect() proves nothing: the
    // agent may still be starting. Handshake inside the loop, otherwise the first
    // attempt races the agent and fails with "recv header failed".
    s2sc::HelloReply hello{};
    bool ok = false;
    for (int i = 0; i < 40; ++i) {
        if (client.Connect("127.0.0.1", cfg.port, err)) {
            std::string herr;
            if (client.Hello(hello, herr)) { ok = true; break; }
            err = herr;
            client.Disconnect();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!ok) {
        std::string out = agentProc.Output();
        log.Error("agent handshake: %s", err.c_str());
        if (!out.empty()) log.Error("agent output: %s", out.c_str());
        status_line = err;
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
    // TerminateProcess on adb.exe does not reliably reap the remote child.
    adb.Run(std::string("shell ") + (root_mode == 1 ? "su -c \"pkill -f s2sc_agent\"" : "pkill -f s2sc_agent"), 8000);
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
    // The targets tab edits cfg directly, so the offsets in the UI may be newer
    // than the copy the reader holds.
    mem.SetLayout(cfg);
    if (!cfg.player_animation.weapon_id || !cfg.player_animation.skin_id) {
        status_line = "layout incomplete";
        return;
    }

    if (scan_thread.joinable()) scan_thread.join();

    scan_cancel = false;
    scan_done    = 0;
    scan_total   = 1;
    scan_running = true;

    const uint32_t value = scan_value;
    const bool     by_class = scan_class_mode;
    const std::string class_name = cfg.class_name;
    scan_thread = std::thread([this, value, by_class, class_name]() {
        std::vector<WeaponInstance> hits;
        auto progress = [this](uint64_t done, uint64_t total) {
            scan_done  = done;
            scan_total = total ? total : 1;
        };

        std::string err;
        if (by_class) {
            // Exact, but depends on the build materialising Il2CppClass objects.
            uint64_t klass = 0;
            if (mem.ScanByClass(class_name, klass, hits, err)) {
                class_klass = klass;
                if (hits.empty())
                    log.Warn("class %s is at 0x%llX but has no live instance "
                             "(enter a match and scan again)", class_name.c_str(),
                             static_cast<unsigned long long>(klass));
            } else {
                log.Warn("class scan: %s", err.c_str());
            }
        } else {
            // Primary path: the agent walks the heap once and keeps every object
            // whose skin id is a real catalogue id. No class metadata involved, so
            // it does not depend on how this build lays out Il2CppClass.
            std::vector<uint32_t> ids;
            ids.reserve(skins.Size());
            for (const SkinEntry& s : skins.All()) ids.push_back(s.id);

            if (ids.empty())
                log.Warn("skin catalogue is empty, the field scan has nothing to match");
            else if (!mem.ScanBySkinSet(ids, hits, err))
                log.Warn("device scan: %s", err.c_str());
        }

        // Fallback for when the user knows the exact skin id that is equipped.
        if (hits.empty() && value != 0) {
            log.Info("falling back to a seeded scan for skin %u", value);
            mem.ScanForSkin(value, hits, progress, scan_cancel);
        }

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
        log.Warn("scan: no structurally valid objects");
        status_line = "scan: 0 hits";
    } else {
        log.Ok("scan: %zu candidate(s)", candidates.size());
        status_line = "scan: " + std::to_string(candidates.size()) + " hits";
    }
    log.Info("scan rejects: %s", mem.last_scan_report.c_str());
}

bool App::ApplySkin(uint32_t skin_id) {
    if (!has_target) { status_line = "no target"; return false; }
    mem.SetLayout(cfg);

    // The weapon view can be rebuilt on a weapon switch, leaving the cached
    // pointers dangling. Re-validate before every write so we never poke a freed
    // object.
    WeaponInstance fresh;
    std::string why;
    if (!mem.Validate(target.obj, fresh, why, /*strict=*/false)) {
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

    watch_active   = true;
    watch_obj      = target.obj;
    watch_expected = skin_id;
    watch_start    = std::chrono::steady_clock::now();
    watch_next     = watch_start;
    return true;
}

void App::PollWatch() {
    if (!watch_active) return;
    if (!attached) { watch_active = false; return; }
    if (!cfg.player_animation.skin_id) { watch_active = false; return; }

    const auto now = std::chrono::steady_clock::now();
    if (now < watch_next) return;
    watch_next = now + std::chrono::milliseconds(250);

    uint32_t v = 0;
    if (!mem.ReadU32(watch_obj + cfg.player_animation.skin_id.Get(), v)) {
        log.Warn("watch: the object stopped responding, it was freed or the weapon switched");
        watch_active = false;
        return;
    }

    const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(now - watch_start).count();

    if (v != watch_expected) {
        // The game recomputes this field from its own state, so a single poke is
        // not enough - this is the signal that the write path needs a hook.
        log.Warn("watch: value reverted to %u after %lld ms, the game rewrites +0x%X",
                 v, static_cast<long long>(held), cfg.player_animation.skin_id.Get());
        status_line = "reverted";
        watch_active = false;
        return;
    }

    if (held >= 1000) {
        log.Ok("watch: skin %u held for %lld ms at obj 0x%llX", v, static_cast<long long>(held),
               static_cast<unsigned long long>(watch_obj));
        watch_active = false;
    }
}

bool App::RefreshTarget() {
    if (!has_target) return false;
    mem.SetLayout(cfg);
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
