#include "proc_mem.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace s2sc;

namespace {

bool PlausiblePtr(uint64_t p) {
    // 4 KB aligned-down, inside the 47-bit user address space Android uses.
    return p >= 0x10000ull && p < 0x00007fffffffffffull && (p % 8) == 0;
}

bool ReadableName(const RegionEntry& r) {
    if (!(r.prot & kProtRead)) return false;
    const std::string name(r.name, strnlen(r.name, sizeof(r.name)));
    if (name == "[stack]" || name == "[vvar]" || name == "[vdso]" || name == "[vectors]") return false;
    if (name == "[heap]" || name.empty()) return true;
    if (name.rfind("[anon", 0) == 0) return true;
    if (name.find("libil2cpp") != std::string::npos) return true;
    if (name.find("/dev/ashmem") != std::string::npos) return true;
    return false;
}

} // namespace

bool ProcMem::ReadU32(uint64_t addr, uint32_t& out) { return m_client.Read(addr, &out, sizeof(out)); }
bool ProcMem::ReadU64(uint64_t addr, uint64_t& out) { return m_client.Read(addr, &out, sizeof(out)); }
bool ProcMem::ReadBytes(uint64_t addr, void* dst, uint32_t len) { return m_client.Read(addr, dst, len); }
bool ProcMem::WriteU32(uint64_t addr, uint32_t value) { return m_client.Write(addr, &value, sizeof(value)); }
bool ProcMem::WriteBytes(uint64_t addr, const void* src, uint32_t len) { return m_client.Write(addr, src, len); }

ModuleSpan ProcMem::FindModule(const std::string& name, std::string& err) {
    m_module = {};
    std::vector<RegionEntry> regions;
    if (!m_client.Regions(regions, err)) return {};

    uint64_t base = 0, end = 0;
    for (const RegionEntry& r : regions) {
        std::string rn(r.name, strnlen(r.name, sizeof(r.name)));
        if (rn.find(name) == std::string::npos) continue;
        if (r.offset == 0 && base == 0) base = r.start;
        end = std::max(end, r.end);
    }
    if (!base) {
        err = name + " is not mapped in the target process";
        return {};
    }
    m_module.base  = base;
    m_module.size  = end - base;
    m_module.valid = true;
    return m_module;
}

// Reads len bytes with an adaptive block size: shrink on a failed block, grow
// back after successes. A failed 4-byte block means the range is broken.
bool ProcMem::ReadRange(uint64_t addr, void* dst, uint32_t len) {
    constexpr uint32_t kMaxStep = 1u << 18; // 256 KB per round trip
    uint8_t* p = static_cast<uint8_t*>(dst);
    uint32_t done = 0;
    uint32_t step = kMaxStep;

    while (done < len) {
        uint32_t try_len = std::min(step, len - done);
        if (m_client.Read(addr + done, p + done, try_len)) {
            done += try_len;
            if (step < kMaxStep) step <<= 1;
            continue;
        }
        if (step > 4) {
            step >>= 1;
            step &= ~3u;
            continue;
        }
        return false;
    }
    return true;
}

bool ProcMem::Validate(uint64_t obj, WeaponInstance& out, std::string& why) {
    why.clear();
    if (!m_module.valid) { why = "module span unknown"; return false; }
    if (!PlausiblePtr(obj)) { why = "object pointer implausible"; return false; }
    if (!m_cfg.weapon_controller.skin_id || !m_cfg.weapon_controller.material_controller ||
        !m_cfg.weapon_controller.owner) {
        why = "layout incomplete";
        return false;
    }

    uint32_t span = std::max<uint32_t>(m_cfg.weapon_controller.skin_id.Get() + 8, 0x100);
    std::vector<uint8_t> buf(span, 0);
    if (!ReadRange(obj, buf.data(), span)) { why = "object header unreadable"; return false; }

    auto u64_at = [&](uint32_t off) -> uint64_t {
        uint64_t v = 0;
        std::memcpy(&v, buf.data() + off, sizeof(v));
        return v;
    };
    auto u32_at = [&](uint32_t off) -> uint32_t {
        uint32_t v = 0;
        std::memcpy(&v, buf.data() + off, sizeof(v));
        return v;
    };

    uint64_t klass = u64_at(0);
    if (!PlausiblePtr(klass)) { why = "klass pointer implausible"; return false; }
    if (m_cfg.klass_in_module && (klass < m_module.base || klass >= m_module.base + m_module.size)) {
        why = "klass pointer outside libil2cpp (klass_in_module is on)";
        return false;
    }

    uint64_t owner = u64_at(m_cfg.weapon_controller.owner.Get());
    if (!PlausiblePtr(owner)) { why = "owner pointer implausible"; return false; }

    uint64_t mc = u64_at(m_cfg.weapon_controller.material_controller.Get());
    if (!PlausiblePtr(mc)) { why = "material controller pointer implausible"; return false; }

    uint32_t skin = u32_at(m_cfg.weapon_controller.skin_id.Get());

    uint8_t mcbuf[0x40] = {0};
    if (!ReadRange(mc, mcbuf, sizeof(mcbuf))) { why = "material controller unreadable"; return false; }

    uint64_t klass2 = 0;
    std::memcpy(&klass2, mcbuf, sizeof(klass2));
    if (!PlausiblePtr(klass2)) { why = "material controller klass implausible"; return false; }

    uint32_t weapon = 0, skin2 = 0;
    if (m_cfg.material_controller.weapon_id)
        std::memcpy(&weapon, mcbuf + m_cfg.material_controller.weapon_id.Get(), sizeof(weapon));
    if (m_cfg.material_controller.skin_id)
        std::memcpy(&skin2, mcbuf + m_cfg.material_controller.skin_id.Get(), sizeof(skin2));

    if (weapon > 200) { why = "weapon id out of range"; return false; }
    if (m_cfg.material_controller.skin_id && skin2 != skin) { why = "skin id mismatch between objects"; return false; }

    out.obj    = obj;
    out.klass  = klass;
    out.mc     = mc;
    out.owner  = owner;
    out.weapon = weapon;
    out.skin   = skin;
    return true;
}

bool ProcMem::ReadSkin(const WeaponInstance& w, uint32_t& out) {
    if (!m_cfg.weapon_controller.skin_id) return false;
    return ReadU32(w.obj + m_cfg.weapon_controller.skin_id.Get(), out);
}

bool ProcMem::WriteSkin(const WeaponInstance& w, uint32_t skin, std::string& err) {
    if (!m_cfg.weapon_controller.skin_id || !m_cfg.material_controller.skin_id) {
        err = "layout incomplete";
        return false;
    }
    if (!WriteU32(w.obj + m_cfg.weapon_controller.skin_id.Get(), skin)) {
        err = "write to WeaponController failed";
        return false;
    }
    if (!WriteU32(w.mc + m_cfg.material_controller.skin_id.Get(), skin)) {
        err = "write to WeaponMaterialController failed";
        return false;
    }
    return true;
}

void ProcMem::ScanForSkin(uint32_t skin,
                          std::vector<WeaponInstance>& out,
                          const std::function<void(uint64_t, uint64_t)>& progress,
                          std::atomic<bool>& cancel) {
    out.clear();
    if (!m_cfg.weapon_controller.skin_id) return;

    std::string err;
    std::vector<RegionEntry> regions;
    if (!m_client.Regions(regions, err)) return;

    std::vector<RegionEntry> targets;
    uint64_t total = 0;
    for (const RegionEntry& r : regions) {
        if (!ReadableName(r)) continue;
        uint64_t size = r.end - r.start;
        if (size == 0 || size > (512ull << 20)) continue;
        targets.push_back(r);
        total += size;
    }

    const uint32_t CHUNK = 1u << 20;
    std::vector<uint8_t> buf(CHUNK + 8);
    std::unordered_set<uint64_t> seen;
    std::unordered_map<std::string, int> rejects;
    uint64_t done = 0;

    for (const RegionEntry& r : targets) {
        for (uint64_t off = 0; off < r.end - r.start; off += CHUNK) {
            if (cancel.load()) return;

            uint32_t len = static_cast<uint32_t>(std::min<uint64_t>(CHUNK, (r.end - r.start) - off));
            uint64_t addr_base = r.start + off;

            if (!ReadRange(addr_base, buf.data(), len)) {
                done += len;
                if (progress) progress(done, total);
                continue;
            }

            const uint32_t skin_off = m_cfg.weapon_controller.skin_id.Get();
            const uint32_t mc_off   = m_cfg.weapon_controller.material_controller.Get();
            const uint64_t mod_lo   = m_module.base;
            const uint64_t mod_hi   = m_module.base + m_module.size;
            const bool     klass_module_only = m_cfg.klass_in_module;

            for (uint32_t i = 0; i + 4 <= len; i += 4) {
                uint32_t v = 0;
                std::memcpy(&v, buf.data() + i, sizeof(v));
                if (v != skin) continue;

                uint64_t addr = addr_base + i;
                if (addr < skin_off) continue;
                uint64_t obj = addr - skin_off;
                if (!seen.insert(obj).second) continue;

                // Free pre-filters, evaluated against the chunk already in memory:
                // the klass pointer at obj+0 and the material controller pointer at
                // obj+mc_off both sit before the hit, so no extra round trip is
                // needed to reject the overwhelming majority of value matches.
                if (i >= skin_off) {
                    uint64_t klass = 0;
                    std::memcpy(&klass, buf.data() + i - skin_off, sizeof(klass));
                    if (klass_module_only) {
                        if (klass < mod_lo || klass >= mod_hi) continue;
                    } else if (!PlausiblePtr(klass)) {
                        continue;
                    }
                }
                if (mc_off <= skin_off && i >= skin_off - mc_off) {
                    uint64_t mcptr = 0;
                    std::memcpy(&mcptr, buf.data() + i - (skin_off - mc_off), sizeof(mcptr));
                    if (!PlausiblePtr(mcptr)) continue;
                }

                WeaponInstance w;
                std::string why;
                if (Validate(obj, w, why))
                    out.push_back(w);
                else if (!why.empty())
                    rejects[why]++;
            }
            done += len;
            if (progress) progress(done, total);
        }
    }

    // Real weapon controllers share one Il2CppClass; sort so the dominant class
    // group is at the top and the user does not have to hunt for it.
    std::unordered_map<uint64_t, int> freq;
    for (const WeaponInstance& w : out) freq[w.klass]++;
    std::stable_sort(out.begin(), out.end(), [&](const WeaponInstance& a, const WeaponInstance& b) {
        return freq[a.klass] > freq[b.klass];
    });

    std::vector<std::pair<int, std::string>> worst;
    worst.reserve(rejects.size());
    for (const auto& kv : rejects) worst.emplace_back(kv.second, kv.first);
    std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    last_scan_report.clear();
    char line[160];
    for (size_t i = 0; i < worst.size() && i < 4; ++i) {
        std::snprintf(line, sizeof(line), "%s (%d)", worst[i].second.c_str(), worst[i].first);
        if (!last_scan_report.empty()) last_scan_report += "; ";
        last_scan_report += line;
    }
    if (last_scan_report.empty()) last_scan_report = "no structural rejects";
}
