#include "config.h"

#include "json.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>

std::string ExeDir() {
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

namespace {

bool ReadOffset(const json::Value& parent, const char* key, Offset& out) {
    const json::Value* v = parent.Find(key);
    if (!v) { out = {false, 0}; return false; }
    if (v->IsNumber()) {
        out.set   = true;
        out.value = static_cast<uint32_t>(v->number);
        return true;
    }
    if (v->IsString()) {
        // accept "0xD8" / "216" / "unset"
        const std::string& s = v->string;
        if (s == "unset" || s.empty()) { out = {false, 0}; return false; }
        out.set   = true;
        out.value = static_cast<uint32_t>(std::strtoull(s.c_str(), nullptr, 0));
        return true;
    }
    out = {false, 0};
    return false;
}

std::string OffsetToJson(const Offset& o) {
    if (!o.set) return "null";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", o.value);
    return buf;
}

// Windows paths are full of backslashes; they must be escaped or the file we
// write back is not loadable by our own parser.
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace

bool Config::Load(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "config not found: " + path; return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    json::Value root;
    if (!json::Parser::Parse(text, root) || !root.IsObject()) {
        err = "config is not valid JSON: " + path;
        return false;
    }

    build_tag     = root.Str("build", build_tag);
    adb_path      = root.Str("adb", adb_path);
    serial        = root.Str("serial", serial);
    process_name  = root.Str("process", process_name);
    module_name   = root.Str("module", module_name);
    remote_agent  = root.Str("remote_agent", remote_agent);
    skins_json    = root.Str("skins_json", skins_json);
    skins_enum    = root.Str("skins_enum", skins_enum);
    port          = static_cast<int>(root.Int("port", port));
    auto_apply    = root.Bool("auto_apply", auto_apply);
    klass_in_module = root.Bool("klass_in_module", klass_in_module);

    if (const json::Value* layout = root.Find("layout")) {
        if (const json::Value* wc = layout->Find("weapon_controller")) {
            ReadOffset(*wc, "owner", weapon_controller.owner);
            ReadOffset(*wc, "material_controller", weapon_controller.material_controller);
            ReadOffset(*wc, "skin_id", weapon_controller.skin_id);
        }
        if (const json::Value* mc = layout->Find("material_controller")) {
            ReadOffset(*mc, "weapon_id", material_controller.weapon_id);
            ReadOffset(*mc, "skin_id", material_controller.skin_id);
        }
    }
    return true;
}

bool Config::Save(const std::string& path, std::string& err) const {
    std::ostringstream o;
    o << "{\n";
    o << "  \"build\": \"" << JsonEscape(build_tag) << "\",\n";
    o << "  \"adb\": \"" << JsonEscape(adb_path) << "\",\n";
    o << "  \"serial\": \"" << JsonEscape(serial) << "\",\n";
    o << "  \"process\": \"" << JsonEscape(process_name) << "\",\n";
    o << "  \"module\": \"" << JsonEscape(module_name) << "\",\n";
    o << "  \"port\": " << port << ",\n";
    o << "  \"remote_agent\": \"" << JsonEscape(remote_agent) << "\",\n";
    o << "  \"skins_json\": \"" << JsonEscape(skins_json) << "\",\n";
    o << "  \"skins_enum\": \"" << JsonEscape(skins_enum) << "\",\n";
    o << "  \"auto_apply\": " << (auto_apply ? "true" : "false") << ",\n";
    o << "  \"klass_in_module\": " << (klass_in_module ? "true" : "false") << ",\n";
    o << "  \"layout\": {\n";
    o << "    \"weapon_controller\": {\n";
    o << "      \"owner\": " << OffsetToJson(weapon_controller.owner) << ",\n";
    o << "      \"material_controller\": " << OffsetToJson(weapon_controller.material_controller) << ",\n";
    o << "      \"skin_id\": " << OffsetToJson(weapon_controller.skin_id) << "\n";
    o << "    },\n";
    o << "    \"material_controller\": {\n";
    o << "      \"weapon_id\": " << OffsetToJson(material_controller.weapon_id) << ",\n";
    o << "      \"skin_id\": " << OffsetToJson(material_controller.skin_id) << "\n";
    o << "    }\n";
    o << "  }\n";
    o << "}\n";

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot write config: " + path; return false; }
    out << o.str();
    return true;
}

std::string Config::DescribeMissing() const {
    std::string s;
    auto add = [&](const char* name, const Offset& o) {
        if (!o) { if (!s.empty()) s += ", "; s += name; }
    };
    add("weapon_controller.owner", weapon_controller.owner);
    add("weapon_controller.material_controller", weapon_controller.material_controller);
    add("weapon_controller.skin_id", weapon_controller.skin_id);
    add("material_controller.weapon_id", material_controller.weapon_id);
    add("material_controller.skin_id", material_controller.skin_id);
    return s;
}
