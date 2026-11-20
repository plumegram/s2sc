#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct WeaponInfo {
    uint8_t     id = 0;
    const char* name = "";
    const char* category = "";
};

struct SkinEntry {
    uint32_t    id = 0;
    std::string enum_name;
    std::string display_name;
    std::string weapon;      // canonical weapon name, "Unknown" when unmapped
    std::string collection;
    bool        stattrack = false;
};

class SkinDb {
public:
    bool Load(const std::string& json_path, const std::string& enum_path, std::string& err);

    const std::vector<SkinEntry>& All() const { return m_skins; }
    const std::vector<WeaponInfo>& Weapons() const { return m_weapons; }

    const WeaponInfo* FindWeapon(const std::string& name) const;
    const WeaponInfo* FindWeaponById(uint8_t id) const;
    const char* WeaponNameById(uint32_t id) const;

    // indices into All() for one weapon, sorted by display name
    const std::vector<uint32_t>& SkinsForWeapon(const std::string& weapon) const;

    size_t Size() const { return m_skins.size(); }

private:
    void BuildIndex();

    std::vector<SkinEntry>   m_skins;
    std::vector<WeaponInfo>  m_weapons;
    std::unordered_map<std::string, std::vector<uint32_t>> m_by_weapon;
    static const std::vector<uint32_t> s_empty;
};
