#pragma once

#include <cstdint>
#include <string>

// Every build-dependent value lives here. "Unset" is a distinct state, not a
// zero: a field with set == false must never be used for a memory access.
struct Offset {
    bool     set   = false;
    uint32_t value = 0;

    uint32_t Get() const { return set ? value : 0u; }
    explicit operator bool() const { return set; }
};

// PlayerAnimationData (TypeDefIndex 22204 in the 1.1.0 x64 dump) holds the
// local player's weapon state in one object - there is no second controller to
// chase through a pointer, unlike the 2022 build.
struct LayoutPlayerAnimation {
    Offset weapon_id         = {false, 0}; // byte  WeaponId.Raw  (struct wraps one byte)
    Offset skin_id           = {false, 0}; // int   WeaponSkinId
    Offset magazine_capacity = {false, 0}; // byte  MagazineCapacity
    Offset ammo              = {false, 0}; // byte  Ammo
    Offset rest_ammo         = {false, 0}; // ushort RestAmmo
};

struct Config {
    std::string build_tag;

    // device side
    std::string adb_path = "adb";
    std::string serial;
    std::string process_name = "com.axlebolt.standoff2";
    std::string module_name  = "libunity.so";
    int         port         = 31337;
    std::string remote_agent = "/data/local/tmp/s2sc_agent";

    // data
    std::string skins_json;
    std::string skins_enum;

    // layout
    LayoutPlayerAnimation player_animation;
    // Il2CppClass the skinchanger targets; the agent resolves it by this name.
    std::string class_name = "PlayerAnimationData";

    // Il2CppClass instances may live on the managed heap rather than inside the
    // module image; only enable this when the build is known to keep them there.
    bool klass_in_module = false;

    // ui
    bool auto_apply = false;

    bool Load(const std::string& path, std::string& err);
    bool Save(const std::string& path, std::string& err) const;

    // true when every offset needed for a skin write is present
    bool LayoutComplete() const {
        return player_animation.weapon_id && player_animation.skin_id;
    }

    std::string DescribeMissing() const;
};

std::string ExeDir();
