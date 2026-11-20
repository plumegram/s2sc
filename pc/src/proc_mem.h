#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agent_client.h"
#include "config.h"
#include "proto.h"

// One resolved PlayerAnimationData - the object that owns the local player's
// weapon id, skin id and ammo counters.
struct WeaponInstance {
    uint64_t obj     = 0; // PlayerAnimationData*
    uint64_t klass   = 0; // Il2CppClass* of the object, used to group hits
    uint32_t weapon  = 0; // WeaponId.Raw (byte)
    uint32_t skin    = 0; // WeaponSkinId (int)
    uint32_t mag     = 0; // MagazineCapacity (byte)
    uint32_t ammo    = 0; // Ammo (byte)
    uint32_t reserve = 0; // RestAmmo (ushort)
};

struct ModuleSpan {
    uint64_t base = 0;
    uint64_t size = 0;
    bool     valid = false;
};

class ProcMem {
public:
    explicit ProcMem(AgentClient& client) : m_client(client) {}

    void SetLayout(const Config& cfg) { m_cfg = cfg; }
    const Config& GetLayout() const { return m_cfg; }

    ModuleSpan FindModule(const std::string& name, std::string& err);

    bool ReadU32(uint64_t addr, uint32_t& out);
    bool ReadU64(uint64_t addr, uint64_t& out);
    bool ReadBytes(uint64_t addr, void* dst, uint32_t len);
    bool WriteU32(uint64_t addr, uint32_t value);
    bool WriteBytes(uint64_t addr, const void* src, uint32_t len);

    // Full structural validation of a candidate object address. `strict` adds the
    // ammo-triple checks; they are skipped when re-validating a live target,
    // because a melee weapon legitimately reports a zero magazine.
    bool Validate(uint64_t obj, WeaponInstance& out, std::string& why, bool strict = true);

    bool ReadSkin(const WeaponInstance& w, uint32_t& out);
    // Writes the skin id and reads it back; a silent no-op write is reported.
    bool WriteSkin(const WeaponInstance& w, uint32_t skin, std::string& err);

    // Filled after every scan: which structural checks rejected the hits.
    std::string last_scan_report;

    // Scans readable heap regions for the given skin id, then validates every hit
    // structurally. `progress` receives (bytes_done, bytes_total).
    void ScanForSkin(uint32_t skin,
                     std::vector<WeaponInstance>& out,
                     const std::function<void(uint64_t, uint64_t)>& progress,
                     std::atomic<bool>& cancel);

    // The exact path: the agent resolves the Il2CppClass by name on the device
    // and returns the objects that point at it. No heuristics, no gigabytes over
    // adb. `klass` is filled even when no object is currently alive.
    bool ScanByClass(const std::string& class_name, uint64_t& klass,
                     std::vector<WeaponInstance>& out, std::string& err);

    // The workhorse: the agent walks the heap once and keeps every 8-aligned
    // address whose weapon byte is sane and whose skin id belongs to the
    // catalogue. Needs no class metadata, so it works on any build.
    bool ScanBySkinSet(const std::vector<uint32_t>& ids,
                       std::vector<WeaponInstance>& out, std::string& err);

private:
    bool ReadRange(uint64_t addr, void* dst, uint32_t len);

    // Shared tail of both scans: chunk walk + report assembly.
    void ScanInternal(uint32_t seed,
                      std::vector<WeaponInstance>& out,
                      const std::function<void(uint64_t, uint64_t)>& progress,
                      std::atomic<bool>& cancel);

    AgentClient& m_client;
    Config       m_cfg;
    ModuleSpan   m_module;
};
