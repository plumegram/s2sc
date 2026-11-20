#include "skin_db.h"

#include "json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

// Weapon ids: enum FFABDHFDBHCCHGB from the Standoff 2 IL2CPP dump
// (Dump_Standoff2_1.1.0, TypeDefIndex 6592). Underlying type is byte.
static const WeaponInfo kWeapons[] = {
    {11, "G22", "pistol"},        {12, "USP", "pistol"},        {13, "P350", "pistol"},
    {15, "Deagle", "pistol"},     {16, "Tec9", "pistol"},       {17, "FiveSeven", "pistol"},
    {18, "Berettas", "pistol"},   {19, "Watergun", "pistol"},
    {32, "UMP45", "smg"},         {33, "AkimboUzi", "smg"},     {34, "MP7", "smg"},
    {35, "P90", "smg"},           {36, "MP5", "smg"},           {37, "MAC10", "smg"},
    {42, "VAL", "rifle"},         {43, "M4A1", "rifle"},        {44, "AKR", "rifle"},
    {45, "AKR12", "rifle"},       {46, "M4", "rifle"},          {47, "M16", "rifle"},
    {48, "FAMAS", "rifle"},       {49, "FnFal", "rifle"},
    {51, "AWM", "sniper"},        {52, "M40", "sniper"},        {53, "M110", "sniper"},
    {54, "Mallard", "shotgun"},   {62, "SM1014", "shotgun"},    {63, "FabM", "shotgun"},
    {65, "SPAS", "shotgun"},      {64, "M60", "mg"},
    {66, "ScifiRailgun", "event"}, {68, "ScifiShotgun", "event"}, {69, "ScifiP90", "event"},
    {84, "CandyCane", "event"},
    {70, "Knife", "knife"},       {71, "KnifeBayonet", "knife"}, {72, "KnifeKarambit", "knife"},
    {73, "jKommando", "knife"},   {75, "KnifeButterfly", "knife"}, {77, "FlipKnife", "knife"},
    {78, "KunaiKnife", "knife"},  {79, "ScorpionKnife", "knife"}, {80, "KnifeTanto", "knife"},
    {81, "DaggerKnife", "knife"}, {82, "KnifeKukri", "knife"},   {83, "KnifeStilet", "knife"},
    {85, "KnifeMantis", "knife"}, {86, "KnifeFang", "knife"},    {88, "KnifeSting", "knife"},
    {74, "RoseHands", "glove"},   {76, "SurvivorHands", "glove"}, {89, "Hands", "glove"},
    {91, "GrenadeHE", "grenade"}, {92, "GrenadeSmoke", "grenade"}, {93, "GrenadeFlash", "grenade"},
    {94, "GrenadeMolotov", "grenade"}, {95, "GrenadeIncendiary", "grenade"},
    {96, "RoseGrenade", "grenade"}, {97, "EventGrenadeMolotov", "grenade"},
    {99, "GrenadeSnowball", "grenade"},
    {100, "Bomb", "other"},       {101, "Defuser", "other"},    {102, "DefuseKit", "other"},
    {110, "Vest", "other"},       {111, "VestAndHelmet", "other"},
};

// Enum name tokens that do not literally match the weapon enum name.
struct WeaponAlias { const char* token; const char* weapon; };
static const WeaponAlias kAliases[] = {
    {"Mantis", "KnifeMantis"}, {"Fang", "KnifeFang"},     {"Sting", "KnifeSting"},
    {"Kukri", "KnifeKukri"},   {"Stilet", "KnifeStilet"}, {"Tanto", "KnifeTanto"},
    {"Butterfly", "KnifeButterfly"}, {"Karambit", "KnifeKarambit"},
    {"Bayonet", "KnifeBayonet"}, {"Kunai", "KunaiKnife"}, {"Flip", "FlipKnife"},
    {"Scorpion", "ScorpionKnife"}, {"Dagger", "DaggerKnife"}, {"Kommando", "jKommando"},
    {"Survivor", "SurvivorHands"}, {"Rose", "RoseHands"},
};

const std::vector<uint32_t> SkinDb::s_empty;

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool StartsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string Humanize(const std::string& enum_name) {
    std::string out;
    out.reserve(enum_name.size());
    for (char c : enum_name) out.push_back(c == '_' ? ' ' : c);
    return out;
}

} // namespace

bool SkinDb::Load(const std::string& json_path, const std::string& enum_path, std::string& err) {
    m_skins.clear();
    m_weapons.assign(std::begin(kWeapons), std::end(kWeapons));
    m_by_weapon.clear();

    // ---- enum: name -> id
    std::unordered_map<uint32_t, std::string> enum_by_id;
    {
        std::ifstream in(enum_path, std::ios::binary);
        if (!in) { err = "skins enum not found: " + enum_path; return false; }
        std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            size_t semi = line.find(';', eq);
            if (semi == std::string::npos) continue;

            std::string left  = line.substr(0, eq);
            std::string right = line.substr(eq + 1, semi - eq - 1);

            // "public const wr NAME = 123;" -> drop the trailing blank before '='
            while (!left.empty() && (left.back() == ' ' || left.back() == '\t' || left.back() == '\r')) left.pop_back();
            size_t eqpos = left.find_last_of(" \t");
            std::string name = (eqpos == std::string::npos) ? left : left.substr(eqpos + 1);
            if (name.empty() || !std::isalpha(static_cast<unsigned char>(name[0]))) continue;

            unsigned long long id = std::strtoull(right.c_str(), nullptr, 10);
            if (id == 0) continue;
            enum_by_id[static_cast<uint32_t>(id)] = name;
        }
        if (enum_by_id.empty()) { err = "skins enum parsed empty: " + enum_path; return false; }
    }

    // ---- json: id -> display name, collection
    std::unordered_map<uint32_t, std::string> name_by_id;
    std::unordered_map<uint32_t, std::string> collection_by_id;
    std::unordered_map<uint32_t, bool>        stattrack_by_id;
    {
        std::ifstream in(json_path, std::ios::binary);
        if (!in) { err = "skins json not found: " + json_path; return false; }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();

        json::Value root;
        if (!json::Parser::Parse(text, root) || !root.IsObject()) {
            err = "skins json is not valid JSON: " + json_path;
            return false;
        }
        const json::Value* items = root.Find("inventoryItemDefinitions");
        if (!items || !items->IsArray()) {
            err = "skins json has no inventoryItemDefinitions array";
            return false;
        }
        for (const json::Value& item : items->array) {
            uint32_t id = static_cast<uint32_t>(item.Int("id", 0));
            if (!id) continue;
            name_by_id[id]       = item.Str("displayName");
            collection_by_id[id] = item.Str("properties.collection");
            const json::Value* props = item.Find("properties");
            if (props) {
                collection_by_id[id] = props->Str("collection");
                stattrack_by_id[id]  = props->Bool("stattrack", false);
            }
        }
    }

    // ---- join
    m_skins.reserve(enum_by_id.size());
    for (const auto& kv : enum_by_id) {
        SkinEntry e;
        e.id        = kv.first;
        e.enum_name = kv.second;
        auto it = name_by_id.find(e.id);
        e.display_name = (it != name_by_id.end() && !it->second.empty()) ? it->second : Humanize(e.enum_name);
        auto ct = collection_by_id.find(e.id);
        if (ct != collection_by_id.end()) e.collection = ct->second;
        auto st = stattrack_by_id.find(e.id);
        e.stattrack = (st != stattrack_by_id.end() && st->second) ||
                      e.enum_name.size() >= 10 && e.enum_name.compare(e.enum_name.size() - 10, 10, "_StatTrack") == 0;
        e.weapon = "Unknown";
        m_skins.push_back(std::move(e));
    }

    BuildIndex();
    if (m_skins.empty()) { err = "skin catalogue is empty"; return false; }
    return true;
}

void SkinDb::BuildIndex() {
    // Weapon mapping: enum names look like S3_AKR12_YearOfTheHorse[_StatTrack].
    // Strip the season prefix, then take the longest weapon name that starts the
    // remaining token run.
    for (SkinEntry& e : m_skins) {
        std::string body = e.enum_name;
        if (body.size() > 3 && body[0] == 'S' && std::isdigit(static_cast<unsigned char>(body[1])) && body[2] == '_')
            body = body.substr(3);

        std::string lower = ToLower(body);
        const WeaponInfo* best = nullptr;
        size_t best_len = 0;

        // Longest prefix wins: AKR12Railgun -> AKR12 (not AKR), M40 -> M40 (not M4).
        for (const WeaponInfo& w : m_weapons) {
            std::string wn = ToLower(w.name);
            if (StartsWith(lower, wn) && wn.size() > best_len) {
                best = &w;
                best_len = wn.size();
            }
        }
        if (!best) {
            for (const WeaponAlias& a : kAliases) {
                std::string tok = ToLower(a.token);
                if (StartsWith(lower, tok) && tok.size() > best_len) {
                    best = FindWeapon(a.weapon);
                    best_len = tok.size();
                }
            }
        }
        if (best) e.weapon = best->name;
    }

    for (uint32_t i = 0; i < m_skins.size(); ++i) {
        m_by_weapon[m_skins[i].weapon].push_back(i);
    }
    for (auto& kv : m_by_weapon) {
        std::sort(kv.second.begin(), kv.second.end(), [&](uint32_t a, uint32_t b) {
            return m_skins[a].display_name < m_skins[b].display_name;
        });
    }
}

const WeaponInfo* SkinDb::FindWeapon(const std::string& name) const {
    for (const WeaponInfo& w : m_weapons)
        if (name == w.name) return &w;
    return nullptr;
}

const WeaponInfo* SkinDb::FindWeaponById(uint8_t id) const {
    for (const WeaponInfo& w : m_weapons)
        if (w.id == id) return &w;
    return nullptr;
}

const char* SkinDb::WeaponNameById(uint32_t id) const {
    const WeaponInfo* w = FindWeaponById(static_cast<uint8_t>(id));
    return w ? w->name : "?";
}

const std::vector<uint32_t>& SkinDb::SkinsForWeapon(const std::string& weapon) const {
    auto it = m_by_weapon.find(weapon);
    return it == m_by_weapon.end() ? s_empty : it->second;
}
