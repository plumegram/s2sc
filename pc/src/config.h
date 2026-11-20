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

struct LayoutWeaponController {
    Offset owner             = {false, 0}; // PlayerController*
    Offset material_controller = {false, 0}; // WeaponMaterialController*
    Offset skin_id           = {false, 0}; // GFGACECHEBHCGAG
};

struct LayoutMaterialController {
    Offset weapon_id = {false, 0}; // FFABDHFDBHCCHGB
    Offset skin_id   = {false, 0}; // GFGACECHEBHCGAG
};

struct Config {
    std::string build_tag;

    // device side
    std::string adb_path = "adb";
    std::string serial;
    std::string process_name = "com.axlebolt.standoff2";
    std::string module_name  = "libil2cpp.so";
    int         port         = 31337;
    std::string remote_agent = "/data/local/tmp/s2sc_agent";

    // data
    std::string skins_json;
    std::string skins_enum;

    // layout
    LayoutWeaponController   weapon_controller;
    LayoutMaterialController material_controller;

    // Il2CppClass instances may live on the managed heap rather than inside
    // libil2cpp; only enable this when the build is known to keep them in the
    // module image.
    bool klass_in_module = false;

    // ui
    bool auto_apply = false;

    bool Load(const std::string& path, std::string& err);
    bool Save(const std::string& path, std::string& err) const;

    // true when every offset needed for a skin write is present
    bool LayoutComplete() const {
        return weapon_controller.skin_id && material_controller.skin_id &&
               weapon_controller.material_controller && weapon_controller.owner;
    }

    std::string DescribeMissing() const;
};

std::string ExeDir();
