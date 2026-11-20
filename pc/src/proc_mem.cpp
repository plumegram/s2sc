#include "proc_mem.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace s2sc;

namespace {

// A structural scan of a live heap can surface thousands of look-alikes; the UI
// only needs enough of them to pick the right object.
constexpr size_t kMaxCandidates = 512;

bool PlausiblePtr(uint64_t p) {
    // 8-byte aligned, inside the 47-bit user address space Android uses.
    return p >= 0x10000ull && p < 0x00007fffffffffffull && (p % 8) == 0;
}

// Managed objects live on the GC heap, which shows up as anonymous mappings,
// [heap] or ashmem. App images are included because a few long-lived containers
// sit in static fields inside libunity itself.
bool ScannableRegion(const RegionEntry& r) {
    if (!(r.prot & kProtRead)) return false;
    if (r.prot & kProtExec) return false; // code, not data
    const std::string name(r.name, strnlen(r.name, sizeof(r.name)));
    if (name == "[stack]" || name == "[vvar]" || name == "[vdso]" || name == "[vectors]") return false;
    if (name.empty() || name == "[heap]") return true;
    if (name.rfind("[anon", 0) == 0) return true;
    if (name.find("/dev/ashmem") != std::string::npos) return true;
    if (name.find("libunity") != std::string::npos) return true;
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

bool ProcMem::Validate(uint64_t obj, WeaponInstance& out, std::string& why, bool strict) {
    why.clear();
    const LayoutPlayerAnimation& L = m_cfg.player_animation;
    if (!L.weapon_id || !L.skin_id) { why = "layout incomplete"; return false; }
    if (!PlausiblePtr(obj)) { why = "object pointer implausible"; return false; }

    uint32_t span = 0x100;
    span = std::max<uint32_t>(span, L.weapon_id.Get() + 4);
    span = std::max<uint32_t>(span, L.skin_id.Get() + 8);
    if (L.magazine_capacity) span = std::max<uint32_t>(span, L.magazine_capacity.Get() + 4);
    if (L.ammo)              span = std::max<uint32_t>(span, L.ammo.Get() + 4);
    if (L.rest_ammo)         span = std::max<uint32_t>(span, L.rest_ammo.Get() + 4);

    std::vector<uint8_t> buf(span, 0);
    if (!ReadRange(obj, buf.data(), span)) { why = "object header unreadable"; return false; }

    auto u8_at = [&](uint32_t off) -> uint32_t {
        return off < buf.size() ? buf[off] : 0u;
    };
    auto u16_at = [&](uint32_t off) -> uint32_t {
        uint16_t v = 0;
        if (off + sizeof(v) <= buf.size()) std::memcpy(&v, buf.data() + off, sizeof(v));
        return v;
    };
    auto u32_at = [&](uint32_t off) -> uint32_t {
        uint32_t v = 0;
        if (off + sizeof(v) <= buf.size()) std::memcpy(&v, buf.data() + off, sizeof(v));
        return v;
    };
    uint64_t klass = 0;
    std::memcpy(&klass, buf.data(), sizeof(klass));

    if (!PlausiblePtr(klass)) { why = "klass pointer implausible"; return false; }
    if (m_cfg.klass_in_module && m_module.valid &&
        (klass < m_module.base || klass >= m_module.base + m_module.size)) {
        why = "klass pointer outside module (klass_in_module is on)";
        return false;
    }

    // WeaponId is a struct wrapping a single byte, so the id is one byte wide.
    const uint32_t weapon = u8_at(L.weapon_id.Get());
    if (weapon == 0 || weapon > 80) { why = "weapon id out of range"; return false; }

    const uint32_t skin = u32_at(L.skin_id.Get());

    uint32_t mag = 0, ammo = 0, reserve = 0;
    if (L.magazine_capacity) mag = u8_at(L.magazine_capacity.Get());
    if (L.ammo)              ammo = u8_at(L.ammo.Get());
    if (L.rest_ammo)         reserve = u16_at(L.rest_ammo.Get());

    // A knife and a grenade have no magazine, so 0 is a legitimate capacity; it
    // only has to be consistent with the ammo counters.
    if (strict) {
        if (L.magazine_capacity && mag > 100) { why = "magazine capacity out of range"; return false; }
        if (L.ammo && L.magazine_capacity) {
            if (mag == 0 && ammo != 0) { why = "ammo without a magazine"; return false; }
            if (mag > 0 && ammo > mag) { why = "ammo exceeds magazine"; return false; }
        }
        if (L.rest_ammo && reserve > 999) { why = "reserve ammo out of range"; return false; }
    }

    out.obj     = obj;
    out.klass   = klass;
    out.weapon  = weapon;
    out.skin    = skin;
    out.mag     = mag;
    out.ammo    = ammo;
    out.reserve = reserve;
    return true;
}

bool ProcMem::ReadSkin(const WeaponInstance& w, uint32_t& out) {
    const LayoutPlayerAnimation& L = m_cfg.player_animation;
    if (!L.skin_id) return false;
    return ReadU32(w.obj + L.skin_id.Get(), out);
}

bool ProcMem::WriteSkin(const WeaponInstance& w, uint32_t skin, std::string& err) {
    const LayoutPlayerAnimation& L = m_cfg.player_animation;
    if (!L.skin_id) { err = "layout incomplete"; return false; }

    const uint64_t addr = w.obj + L.skin_id.Get();
    if (!WriteU32(addr, skin)) { err = "write to PlayerAnimationData failed"; return false; }

    // A write that lands on a copy, or gets overwritten by the next tick, must be
    // reported rather than shown as success.
    uint32_t back = 0;
    if (!ReadU32(addr, back)) { err = "read-back failed"; return false; }
    if (back != skin) { err = "write did not stick (still " + std::to_string(back) + ")"; return false; }
    return true;
}

void ProcMem::ScanInternal(uint32_t seed,
                           std::vector<WeaponInstance>& out,
                           const std::function<void(uint64_t, uint64_t)>& progress,
                           std::atomic<bool>& cancel) {
    out.clear();
    last_scan_report.clear();

    const LayoutPlayerAnimation& L = m_cfg.player_animation;
    if (!L.weapon_id || !L.skin_id) { last_scan_report = "layout incomplete"; return; }

    std::string err;
    std::vector<RegionEntry> regions;
    if (!m_client.Regions(regions, err)) { last_scan_report = "regions: " + err; return; }

    std::vector<RegionEntry> targets;
    uint64_t total = 0;
    for (const RegionEntry& r : regions) {
        if (!ScannableRegion(r)) continue;
        uint64_t size = r.end - r.start;
        // 2 GB: a single ART heap mapping can be large, and skipping it would
        // silently lose the object we are looking for.
        if (size == 0 || size > (2ull << 30)) continue;
        targets.push_back(r);
        total += size;
    }

    const uint32_t CHUNK   = 1u << 20;
    const uint32_t OVERLAP = 0x200; // so an object near the chunk edge is still whole
    std::vector<uint8_t> buf(CHUNK + OVERLAP);

    std::unordered_set<uint64_t> seen;
    std::unordered_map<std::string, int> rejects;
    uint64_t done = 0;

    const uint32_t skin_off   = L.skin_id.Get();
    const uint32_t weapon_off = L.weapon_id.Get();
    const uint32_t mag_off    = L.magazine_capacity ? L.magazine_capacity.Get() : 0;
    const uint32_t ammo_off   = L.ammo ? L.ammo.Get() : 0;
    const uint64_t mod_lo     = m_module.base;
    const uint64_t mod_hi     = m_module.base + m_module.size;
    const bool klass_module_only = m_cfg.klass_in_module && m_module.valid;

    for (const RegionEntry& r : targets) {
        const uint64_t rsize = r.end - r.start;
        for (uint64_t off = 0; off < rsize; off += CHUNK) {
            if (cancel.load()) return;

            const uint32_t iter_len = static_cast<uint32_t>(std::min<uint64_t>(CHUNK, rsize - off));
            const uint32_t read_len = static_cast<uint32_t>(std::min<uint64_t>(CHUNK + OVERLAP, rsize - off));
            const uint64_t addr_base = r.start + off;

            if (!ReadRange(addr_base, buf.data(), read_len)) {
                done += iter_len;
                if (progress) progress(done, total);
                continue;
            }

            // Reject a candidate using bytes that are already in the chunk, so the
            // expensive Validate() round trip only runs on survivors. The dedup set
            // is filled after these filters, not before: a structural scan walks
            // every 8-aligned address in the heap, and inserting all of them would
            // be billions of entries.
            auto consider = [&](uint64_t obj) {
                if (obj < addr_base) return;
                const uint64_t local = obj - addr_base;
                if (local + skin_off + 4 > read_len) return;

                uint64_t klass = 0;
                std::memcpy(&klass, buf.data() + local, sizeof(klass));
                if (klass_module_only) {
                    if (klass < mod_lo || klass >= mod_hi) return;
                } else if (!PlausiblePtr(klass)) {
                    return;
                }

                if (weapon_off < skin_off) {
                    const uint32_t w = buf[local + weapon_off];
                    if (w == 0 || w > 80) return;
                }
                if (mag_off > skin_off && local + mag_off < read_len) {
                    const uint32_t m = buf[local + mag_off];
                    if (m > 100) return;
                    if (ammo_off > mag_off && local + ammo_off < read_len) {
                        const uint32_t a = buf[local + ammo_off];
                        if (m == 0 ? a != 0 : a > m) return;
                    }
                }

                if (!seen.insert(obj).second) return;
                if (out.size() >= kMaxCandidates) return;

                WeaponInstance w;
                std::string why;
                if (Validate(obj, w, why))
                    out.push_back(w);
                else if (!why.empty())
                    rejects[why]++;
            };

            for (uint32_t i = 0; i + 4 <= iter_len; i += 4) {
                uint32_t v = 0;
                std::memcpy(&v, buf.data() + i, sizeof(v));
                if (v != seed) continue;
                if (i < skin_off) continue;
                consider(addr_base + i - skin_off);
            }

            done += iter_len;
            if (progress) progress(done, total);
        }
    }

    // Real instances share one Il2CppClass; sort so the dominant class group is
    // at the top and the right object is the obvious pick.
    std::unordered_map<uint64_t, int> freq;
    for (const WeaponInstance& w : out) freq[w.klass]++;
    std::stable_sort(out.begin(), out.end(), [&](const WeaponInstance& a, const WeaponInstance& b) {
        return freq[a.klass] > freq[b.klass];
    });

    std::vector<std::pair<int, std::string>> worst;
    worst.reserve(rejects.size());
    for (const auto& kv : rejects) worst.emplace_back(kv.second, kv.first);
    std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    char line[160];
    for (size_t i = 0; i < worst.size() && i < 4; ++i) {
        std::snprintf(line, sizeof(line), "%s (%d)", worst[i].second.c_str(), worst[i].first);
        if (!last_scan_report.empty()) last_scan_report += "; ";
        last_scan_report += line;
    }
    if (last_scan_report.empty()) last_scan_report = "no structural rejects";
}

void ProcMem::ScanForSkin(uint32_t skin,
                          std::vector<WeaponInstance>& out,
                          const std::function<void(uint64_t, uint64_t)>& progress,
                          std::atomic<bool>& cancel) {
    ScanInternal(skin, out, progress, cancel);
}

bool ProcMem::ScanBySkinSet(const std::vector<uint32_t>& ids,
                            std::vector<WeaponInstance>& out, std::string& err) {
    out.clear();
    last_scan_report.clear();

    const LayoutPlayerAnimation& L = m_cfg.player_animation;
    if (!L.weapon_id || !L.skin_id) { err = "layout incomplete"; return false; }

    std::vector<uint64_t> objs;
    if (!m_client.ScanObjects(L.skin_id.Get(), L.weapon_id.Get(),
                              L.magazine_capacity ? L.magazine_capacity.Get() : 0,
                              L.ammo ? L.ammo.Get() : 0,
                              L.rest_ammo ? L.rest_ammo.Get() : 0,
                              ids, kMaxCandidates, objs, err))
        return false;

    std::unordered_map<std::string, int> rejects;
    for (uint64_t obj : objs) {
        WeaponInstance w;
        std::string why;
        if (Validate(obj, w, why, /*strict=*/true))
            out.push_back(w);
        else if (!why.empty())
            rejects[why]++;
    }

    std::vector<std::pair<int, std::string>> worst;
    worst.reserve(rejects.size());
    for (const auto& kv : rejects) worst.emplace_back(kv.second, kv.first);
    std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    char line[160];
    for (size_t i = 0; i < worst.size() && i < 4; ++i) {
        std::snprintf(line, sizeof(line), "%s (%d)", worst[i].second.c_str(), worst[i].first);
        if (!last_scan_report.empty()) last_scan_report += "; ";
        last_scan_report += line;
    }
    if (last_scan_report.empty())
        last_scan_report = std::to_string(objs.size()) + " raw hits, all valid";
    return true;
}

bool ProcMem::ScanByClass(const std::string& class_name, uint64_t& klass,
                          std::vector<WeaponInstance>& out, std::string& err) {
    out.clear();
    last_scan_report.clear();
    klass = 0;

    std::vector<uint64_t> objs;
    if (!m_client.ScanClass(class_name, 256, klass, objs, err)) return false;

    // The klass pointer already proves what the object is, so only the cheap
    // sanity checks run here - a knife at rest is a valid target too.
    std::unordered_map<std::string, int> rejects;
    for (uint64_t obj : objs) {
        WeaponInstance w;
        std::string why;
        if (Validate(obj, w, why, /*strict=*/false))
            out.push_back(w);
        else if (!why.empty())
            rejects[why]++;
    }

    std::vector<std::pair<int, std::string>> worst;
    worst.reserve(rejects.size());
    for (const auto& kv : rejects) worst.emplace_back(kv.second, kv.first);
    std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    char line[160];
    for (size_t i = 0; i < worst.size() && i < 4; ++i) {
        std::snprintf(line, sizeof(line), "%s (%d)", worst[i].second.c_str(), worst[i].first);
        if (!last_scan_report.empty()) last_scan_report += "; ";
        last_scan_report += line;
    }
    if (last_scan_report.empty()) last_scan_report = "all objects valid";
    return true;
}
