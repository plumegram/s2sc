#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agent_client.h"
#include "config.h"
#include "proto.h"

// One resolved WeaponController and its WeaponMaterialController.
struct WeaponInstance {
    uint64_t obj     = 0; // WeaponController*
    uint64_t klass   = 0; // Il2CppClass* of the object, used to group hits
    uint64_t mc      = 0; // WeaponMaterialController*
    uint64_t owner   = 0; // PlayerController*
    uint32_t weapon  = 0; // weapon id (FFABDHFDBHCCHGB)
    uint32_t skin    = 0; // current skin id (GFGACECHEBHCGAG)
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

    // Full structural validation of a candidate object address.
    bool Validate(uint64_t obj, WeaponInstance& out, std::string& why);

    bool ReadSkin(const WeaponInstance& w, uint32_t& out);
    bool WriteSkin(const WeaponInstance& w, uint32_t skin, std::string& err);

    // Filled after every scan: which structural checks rejected the hits.
    std::string last_scan_report;

    // Scans readable heap regions for the given skin id, then validates every hit
    // structurally. `progress` receives (bytes_done, bytes_total).
    void ScanForSkin(uint32_t skin,
                     std::vector<WeaponInstance>& out,
                     const std::function<void(uint64_t, uint64_t)>& progress,
                     std::atomic<bool>& cancel);

private:
    bool ReadRange(uint64_t addr, void* dst, uint32_t len);

    AgentClient& m_client;
    Config       m_cfg;
    ModuleSpan   m_module;
};
