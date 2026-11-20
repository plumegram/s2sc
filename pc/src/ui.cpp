#include "ui.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

ImVec4 kAccent = ImVec4(0.36f, 0.80f, 0.72f, 1.00f);
ImVec4 kDim    = ImVec4(0.55f, 0.58f, 0.60f, 1.00f);
ImVec4 kOk     = ImVec4(0.42f, 0.82f, 0.48f, 1.00f);
ImVec4 kWarn   = ImVec4(0.92f, 0.72f, 0.30f, 1.00f);
ImVec4 kErr    = ImVec4(0.92f, 0.38f, 0.38f, 1.00f);

bool InputString(const char* label, std::string& s, size_t cap = 512) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    if (ImGui::InputText(label, buf, std::min(sizeof(buf), cap))) {
        s = buf;
        return true;
    }
    return false;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void Led(const char* label, bool on, const char* off_text = "offline") {
    ImGui::TextColored(on ? kOk : kErr, "%s", on ? "●" : "○");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", on ? label : off_text);
}

// --------------------------------------------------------------- connection

void DrawConnection(App& app) {
    ImGui::TextColored(kAccent, "S2SC");
    ImGui::SameLine();
    ImGui::TextDisabled("// standoff 2 structural");
    ImGui::Separator();

    InputString("adb path", app.cfg.adb_path);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        app.adb.SetPath(Adb::Resolve(app.cfg.adb_path));
        app.log.Info("adb: %s", app.adb.Path().c_str());
    }

    if (ImGui::Button("Refresh devices", ImVec2(-1, 0))) {
        std::string err;
        app.devices.clear();
        if (app.adb.ListDevices(app.devices, err) && !app.devices.empty()) {
            app.device_index = 0;
            app.log.Ok("%zu device(s)", app.devices.size());
        } else {
            app.device_index = -1;
            app.log.Warn("%s", err.empty() ? "no devices" : err.c_str());
        }
    }

    const char* current = (app.device_index >= 0 && app.device_index < static_cast<int>(app.devices.size()))
                              ? app.devices[app.device_index].serial.c_str()
                              : "<none>";
    if (ImGui::BeginCombo("device", current)) {
        for (int i = 0; i < static_cast<int>(app.devices.size()); ++i) {
            bool sel = (i == app.device_index);
            std::string label = app.devices[i].serial + "  [" + app.devices[i].state + "]";
            if (ImGui::Selectable(label.c_str(), sel)) app.device_index = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("port", &app.cfg.port);
    if (app.cfg.port < 1 || app.cfg.port > 65535) app.cfg.port = 31337;

    if (!app.connected) {
        if (ImGui::Button("Connect", ImVec2(-1, 0))) {
            if (app.ConnectDevice()) app.log.Ok("connected");
        }
    } else {
        if (ImGui::Button("Disconnect", ImVec2(-1, 0))) {
            app.DisconnectDevice();
            app.log.Info("disconnected");
        }
    }

    ImGui::Spacing();
    Led(app.connected ? "agent online" : "", app.connected);
    Led(app.attached ? "attached" : "", app.attached);

    ImGui::Separator();
    ImGui::TextDisabled("target process");
    InputString("##proc", app.cfg.process_name);
    if (ImGui::Button("Attach", ImVec2(-1, 0))) {
        if (app.AttachGame()) app.log.Ok("attached");
    }
    if (app.attached) {
        if (ImGui::Button("Detach", ImVec2(-1, 0))) {
            app.DetachGame();
            app.log.Info("detached");
        }
    }

    if (app.module.valid) {
        ImGui::TextDisabled("pid      0x%X (%u)", app.pid, app.pid);
        ImGui::TextDisabled("module   0x%llX", static_cast<unsigned long long>(app.module.base));
        ImGui::TextDisabled("size     0x%llX", static_cast<unsigned long long>(app.module.size));
    }

    ImGui::Separator();
    if (!app.cfg.LayoutComplete()) {
        ImGui::TextColored(kWarn, "layout incomplete");
        ImGui::TextWrapped("%s", app.cfg.DescribeMissing().c_str());
    } else {
        ImGui::TextColored(kOk, "layout loaded");
        ImGui::TextDisabled("%s", app.cfg.build_tag.c_str());
    }

    if (!app.status_line.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", app.status_line.c_str());
    }
}

// -------------------------------------------------------------- skinchanger

void DrawSkinchanger(App& app) {
    static const char* kCategories[] = {"pistol", "smg", "rifle", "sniper", "shotgun",
                                        "mg", "knife", "glove", "grenade", "event", "other"};

    ImGui::BeginChild("weapons", ImVec2(190, 0), ImGuiChildFlags_Border);
    for (const char* cat : kCategories) {
        bool any = false;
        for (const WeaponInfo& w : app.skins.Weapons())
            if (std::strcmp(w.category, cat) == 0 && !app.skins.SkinsForWeapon(w.name).empty()) { any = true; break; }
        if (!any) continue;

        if (ImGui::TreeNodeEx(cat, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const WeaponInfo& w : app.skins.Weapons()) {
                if (std::strcmp(w.category, cat) != 0) continue;
                const auto& list = app.skins.SkinsForWeapon(w.name);
                if (list.empty()) continue;
                char label[64];
                std::snprintf(label, sizeof(label), "%s (%zu)", w.name, list.size());
                if (ImGui::Selectable(label, app.selected_weapon == w.name))
                    app.selected_weapon = w.name;
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("skins", ImVec2(0, 0), ImGuiChildFlags_Border);

    ImGui::TextColored(kAccent, "%s", app.selected_weapon.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("// %zu skins", app.skins.SkinsForWeapon(app.selected_weapon).size());
    ImGui::SetNextItemWidth(-1);
    InputString("##search", app.skin_search);

    const auto& list = app.skins.SkinsForWeapon(app.selected_weapon);
    std::string needle = Lower(app.skin_search);

    ImGui::BeginChild("skinlist", ImVec2(0, -110), ImGuiChildFlags_Border);
    for (uint32_t idx : list) {
        const SkinEntry& s = app.skins.All()[idx];
        if (!needle.empty()) {
            if (Lower(s.display_name).find(needle) == std::string::npos &&
                Lower(s.enum_name).find(needle) == std::string::npos)
                continue;
        }
        char label[320];
        std::snprintf(label, sizeof(label), "%s##%u", s.display_name.c_str(), s.id);
        if (ImGui::Selectable(label, app.selected_skin == s.id)) {
            app.selected_skin = s.id;
            if (app.cfg.auto_apply && app.has_target) app.ApplySkin(s.id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("id %u\n%s\ncollection: %s", s.id, s.enum_name.c_str(),
                              s.collection.empty() ? "-" : s.collection.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    if (app.has_target) {
        ImGui::Text("obj 0x%llX  klass 0x%llX", static_cast<unsigned long long>(app.target.obj),
                    static_cast<unsigned long long>(app.target.klass));
        ImGui::Text("weapon %s   skin %u   ammo %u/%u   reserve %u",
                    app.skins.WeaponNameById(app.target.weapon), app.target.skin,
                    app.target.ammo, app.target.mag, app.target.reserve);
    } else {
        ImGui::TextColored(kDim, "target not selected");
        ImGui::SameLine();
        if (ImGui::SmallButton("scan structural")) {
            app.scan_structural = true;
            app.StartScan();
        }
    }

    bool can_apply = app.has_target && app.selected_skin != 0;
    if (!can_apply) ImGui::BeginDisabled();
    if (ImGui::Button("Apply", ImVec2(120, 0))) app.ApplySkin(app.selected_skin);
    ImGui::SameLine();
    if (ImGui::Button("Re-read", ImVec2(90, 0))) app.RefreshTarget();
    if (!can_apply) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Checkbox("auto", &app.cfg.auto_apply);

    if (app.watch_active) {
        ImGui::TextColored(kAccent, "watching the write for 1 s: if the game rewrites it, the log says so");
    } else if (app.has_target) {
        ImGui::TextColored(kDim, "write lands immediately, the view refreshes on the next weapon raise");
    }

    ImGui::EndChild();
}

// ------------------------------------------------------------------ targets

void DrawOffsetRow(const char* label, Offset& o) {
    if (ImGui::Checkbox(label, &o.set)) {
        if (!o.set) o.value = 0;
    }
    ImGui::SameLine(220);
    if (!o.set) {
        ImGui::TextColored(kErr, "UNSET");
    } else {
        ImGui::SetNextItemWidth(110);
        int v = static_cast<int>(o.value);
        if (ImGui::InputInt("##v", &v, 1, 16, ImGuiInputTextFlags_CharsHexadecimal)) {
            o.value = static_cast<uint32_t>(v);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("0x%X", o.value);
    }
}

void DrawTargets(App& app) {
    ImGui::TextColored(kAccent, "layout");
    ImGui::SameLine();
    ImGui::TextDisabled("// build %s", app.cfg.build_tag.c_str());

    DrawOffsetRow("player_animation.weapon_id", app.cfg.player_animation.weapon_id);
    DrawOffsetRow("player_animation.skin_id", app.cfg.player_animation.skin_id);
    DrawOffsetRow("player_animation.magazine_capacity", app.cfg.player_animation.magazine_capacity);
    DrawOffsetRow("player_animation.ammo", app.cfg.player_animation.ammo);
    DrawOffsetRow("player_animation.rest_ammo", app.cfg.player_animation.rest_ammo);

    if (ImGui::Button("Save config")) app.SaveConfig();
    ImGui::SameLine();
    if (ImGui::Button("Validate target") && app.has_target) {
        WeaponInstance w;
        std::string why;
        app.mem.SetLayout(app.cfg); // pick up offsets edited in this tab
        if (app.mem.Validate(app.target.obj, w, why))
            app.log.Ok("validation passed: weapon %s skin %u", app.skins.WeaponNameById(w.weapon), w.skin);
        else
            app.log.Error("validation failed: %s", why.c_str());
    }

    ImGui::Separator();
    ImGui::TextColored(kAccent, "scan");
    ImGui::SetNextItemWidth(140);
    int sv = static_cast<int>(app.scan_value);
    if (ImGui::InputInt("skin id to find", &sv)) app.scan_value = static_cast<uint32_t>(sv);
    ImGui::SameLine();
    ImGui::Checkbox("structural (ignore seed)", &app.scan_structural);
    ImGui::SameLine();
    if (!app.scan_running.load()) {
        if (ImGui::Button("Scan")) app.StartScan();
    } else {
        if (ImGui::Button("Cancel")) app.scan_cancel = true;
    }

    if (app.scan_running.load()) {
        float p = static_cast<float>(app.scan_done.load()) / static_cast<float>(app.scan_total.load() ? app.scan_total.load() : 1);
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.0f MB / %.0f MB", app.scan_done.load() / 1048576.0,
                      app.scan_total.load() / 1048576.0);
        ImGui::ProgressBar(p, ImVec2(-1, 0), overlay);
    }

    ImGui::Separator();
    ImGui::TextColored(kAccent, "candidates (%zu)", app.candidates.size());

    if (ImGui::BeginTable("cands", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("obj");
        ImGui::TableSetupColumn("weapon");
        ImGui::TableSetupColumn("skin");
        ImGui::TableSetupColumn("ammo");
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(app.candidates.size()); ++i) {
            const WeaponInstance& w = app.candidates[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "%d", i);
            if (ImGui::Selectable(lbl, app.selected_candidate == i,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                app.selected_candidate = i;
                app.target = w;
                app.has_target = true;
            }
            ImGui::TableNextColumn();
            ImGui::Text("0x%llX", static_cast<unsigned long long>(w.obj));
            ImGui::TableNextColumn();
            ImGui::Text("%s", app.skins.WeaponNameById(w.weapon));
            ImGui::TableNextColumn();
            ImGui::Text("%u", w.skin);
            ImGui::TableNextColumn();
            ImGui::Text("%u/%u", w.ammo, w.mag);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextColored(kAccent, "hex view");

    static std::vector<uint8_t> dump;
    static uint64_t dump_addr = 0;

    if (app.has_target) {
        if (ImGui::Button("Dump target")) {
            dump.assign(0x100, 0);
            if (app.mem.ReadBytes(app.target.obj, dump.data(), 0x100)) {
                dump_addr = app.target.obj;
                app.log.Info("dumped 0x%llX", static_cast<unsigned long long>(dump_addr));
            } else {
                app.log.Error("dump failed");
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("obj 0x%llX  klass 0x%llX",
                            static_cast<unsigned long long>(app.target.obj),
                            static_cast<unsigned long long>(app.target.klass));
    }

    if (!dump.empty()) {
        ImGui::BeginChild("hex", ImVec2(0, 220), ImGuiChildFlags_Border);
        for (size_t row = 0; row < dump.size(); row += 16) {
            char line[160];
            size_t n = 0;
            auto put = [&](const char* fmt, auto... args) {
                int w = std::snprintf(line + n, sizeof(line) - n, fmt, args...);
                if (w > 0) n += static_cast<size_t>(w);
            };
            put("%04zX  ", row);
            for (size_t k = 0; k < 16; ++k) {
                if (row + k < dump.size())
                    put("%02X ", dump[row + k]);
                else
                    put("   ");
                if (k == 7) put(" ");
            }
            put(" |");
            for (size_t k = 0; k < 16 && row + k < dump.size(); ++k) {
                unsigned char c = dump[row + k];
                put("%c", (c >= 32 && c < 127) ? static_cast<char>(c) : '.');
            }
            put("|");
            ImGui::TextUnformatted(line);
        }
        ImGui::EndChild();
    }

    ImGui::Separator();
    ImGui::TextColored(kAccent, "manual access");
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("address", app.manual_addr, sizeof(app.manual_addr),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SetNextItemWidth(120);
    int mv = static_cast<int>(app.manual_value);
    if (ImGui::InputInt("value", &mv, 1, 16, ImGuiInputTextFlags_CharsHexadecimal))
        app.manual_value = static_cast<uint32_t>(mv);
    if (ImGui::Button("Read u32")) {
        uint64_t addr = std::strtoull(app.manual_addr, nullptr, 16);
        uint32_t v = 0;
        if (app.mem.ReadU32(addr, v)) {
            app.manual_value = v;
            app.log.Info("read 0x%llX = %u (0x%X)", static_cast<unsigned long long>(addr), v, v);
        } else {
            app.log.Error("read 0x%llX failed", static_cast<unsigned long long>(addr));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Write u32")) {
        uint64_t addr = std::strtoull(app.manual_addr, nullptr, 16);
        if (app.mem.WriteU32(addr, app.manual_value))
            app.log.Ok("write 0x%llX = %u", static_cast<unsigned long long>(addr), app.manual_value);
        else
            app.log.Error("write 0x%llX failed", static_cast<unsigned long long>(addr));
    }
}

// ---------------------------------------------------------------------- log

void DrawLog(App& app) {
    if (ImGui::Button("Clear")) app.log.Clear();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", app.config_path.c_str());

    ImGui::BeginChild("loglines", ImVec2(0, 0), ImGuiChildFlags_Border);
    for (const LogLine& l : app.log.Snapshot()) {
        ImVec4 c = kDim;
        switch (l.level) {
        case LogLevel::Ok: c = kOk; break;
        case LogLevel::Warn: c = kWarn; break;
        case LogLevel::Error: c = kErr; break;
        case LogLevel::Info: c = ImVec4(0.78f, 0.80f, 0.82f, 1.0f); break;
        }
        ImGui::TextColored(c, "%s", l.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

} // namespace

void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 4.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(7, 3);
    s.ItemSpacing       = ImVec2(7, 5);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.075f, 0.082f, 0.094f, 1.00f);
    c[ImGuiCol_ChildBg]       = ImVec4(0.098f, 0.106f, 0.120f, 1.00f);
    c[ImGuiCol_PopupBg]       = ImVec4(0.098f, 0.106f, 0.120f, 1.00f);
    c[ImGuiCol_Border]        = ImVec4(0.180f, 0.196f, 0.216f, 1.00f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.137f, 0.149f, 0.168f, 1.00f);
    c[ImGuiCol_FrameBgHovered]= ImVec4(0.180f, 0.196f, 0.216f, 1.00f);
    c[ImGuiCol_Header]        = ImVec4(0.160f, 0.290f, 0.270f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.200f, 0.380f, 0.350f, 1.00f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.240f, 0.450f, 0.410f, 1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.137f, 0.149f, 0.168f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.200f, 0.380f, 0.350f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.240f, 0.450f, 0.410f, 1.00f);
    c[ImGuiCol_CheckMark]     = kAccent;
    c[ImGuiCol_SliderGrab]    = kAccent;
    c[ImGuiCol_Tab]           = ImVec4(0.110f, 0.120f, 0.136f, 1.00f);
    c[ImGuiCol_TabHovered]    = ImVec4(0.200f, 0.380f, 0.350f, 1.00f);
    c[ImGuiCol_TabActive]   = ImVec4(0.160f, 0.290f, 0.270f, 1.00f);
    c[ImGuiCol_TitleBg]       = ImVec4(0.075f, 0.082f, 0.094f, 1.00f);
    c[ImGuiCol_Text]          = ImVec4(0.880f, 0.890f, 0.900f, 1.00f);
    c[ImGuiCol_TextDisabled]  = ImVec4(0.480f, 0.500f, 0.520f, 1.00f);
}

void DrawUI(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;

    ImGui::Begin("##root", nullptr, flags);

    ImGui::BeginChild("left", ImVec2(330, 0), ImGuiChildFlags_Border);
    DrawConnection(app);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_Border);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Skinchanger")) {
            DrawSkinchanger(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Targets")) {
            DrawTargets(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            DrawLog(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::End();
}
