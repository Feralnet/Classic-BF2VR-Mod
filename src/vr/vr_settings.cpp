
#include "vr_settings.hpp"
#include "../logger.hpp"

#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_dx11.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include <Windows.h>

namespace sp::vr {

namespace {

Settings current_settings;
bool settings_loaded = false;

std::map<std::string, Arm_tuning> arm_tunings;
bool arm_tunings_loaded = false;

auto directory() noexcept -> std::wstring
{
   HMODULE self = nullptr;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&directory), &self);

   std::wstring path(MAX_PATH, L'\0');
   path.resize(GetModuleFileNameW(self, path.data(), MAX_PATH));
   path.resize(path.find_last_of(L"\\/") + 1);

   return path;
}

// The settings file's entries: name, and how to read and write the value.
struct Entry {
   const char* name;
   float* number = nullptr;
   bool* flag = nullptr;
   int* choice = nullptr;
};

auto entries() noexcept -> std::array<Entry, 75>
{
   Settings& s = current_settings;

   return {Entry{"vignette", nullptr, &s.vignette},
           Entry{"vignette_strength", &s.vignette_strength},
           Entry{"turn_mode", nullptr, nullptr, reinterpret_cast<int*>(&s.turn_mode)},
           Entry{"snap_turn_degrees", &s.snap_turn_degrees},
           Entry{"smooth_turn_speed", &s.smooth_turn_speed},
           Entry{"sprint_toggle", nullptr, &s.sprint_toggle},
           Entry{"move_direction", nullptr, nullptr, reinterpret_cast<int*>(&s.move_direction)},
           Entry{"swap_sticks", nullptr, &s.swap_sticks},
           Entry{"pause_on_focus_loss", nullptr, &s.pause_on_focus_loss},
           Entry{"head_sway", nullptr, &s.head_sway},
           Entry{"weapon_sway", nullptr, &s.weapon_sway},
           Entry{"screen_blur", nullptr, &s.screen_blur},
           Entry{"overscan_degrees", &s.overscan_degrees},
           Entry{"overscan_by_speed", nullptr, &s.overscan_by_speed},
           Entry{"head_view", nullptr, &s.head_view},
           Entry{"cull_from_eyes", nullptr, &s.cull_from_eyes},
           Entry{"play_mode", nullptr, nullptr, reinterpret_cast<int*>(&s.play_mode)},
           Entry{"body_scale", nullptr, nullptr, reinterpret_cast<int*>(&s.body_scale)},
           Entry{"player_eye_height", &s.player_eye_height},
           Entry{"player_height_measured", nullptr, &s.player_height_measured},
           Entry{"physical_crouch", nullptr, &s.physical_crouch},
           Entry{"scope_auto", nullptr, &s.scope_auto},
           Entry{"scope_zoom_1", &s.scope_zooms[0]},
           Entry{"scope_zoom_2", &s.scope_zooms[1]},
           Entry{"scope_zoom_3", &s.scope_zooms[2]},
           Entry{"scope_zoom_mode", nullptr, nullptr, &s.scope_zoom_mode},
           Entry{"scope_size", &s.scope_size},
           Entry{"scope_height", &s.scope_height},
           Entry{"scope_locked", nullptr, &s.scope_locked},
           Entry{"hud_size", &s.hud_size},
           Entry{"head_hud", nullptr, &s.head_hud},
           Entry{"wrist_hud", nullptr, &s.wrist_hud},
           Entry{"show_left_arm", nullptr, &s.show_left_arm},
           Entry{"show_right_arm", nullptr, &s.show_right_arm},
           Entry{"hand_smoothing", &s.hand_smoothing},
           Entry{"fire_from_muzzle", nullptr, &s.fire_from_muzzle},
           Entry{"show_shot_line", nullptr, &s.show_shot_line},
           Entry{"turret_eye_right", &s.turret_eye_right},
           Entry{"turret_eye_up", &s.turret_eye_up},
           Entry{"turret_eye_forward", &s.turret_eye_forward},
           Entry{"turret_hide_radius", &s.turret_hide_radius},
           Entry{"turret_hand_yaw", &s.turret_hand_yaw},
           Entry{"vehicle_steering", nullptr, nullptr, // was vehicle_steer_mode (position)
                 reinterpret_cast<int*>(&s.vehicle_steer_mode)},
           Entry{"vehicle_stick_range", &s.vehicle_stick_range},
           Entry{"vehicle_steer_speed", &s.vehicle_steer_speed},
           Entry{"droideka_rolled_view", nullptr, &s.droideka_rolled_view},
           Entry{"droideka_first_person", nullptr, &s.droideka_first_person},
           Entry{"droideka_eye_height", &s.droideka_eye_height},
           Entry{"droideka_eye_forward", &s.droideka_eye_forward},
           Entry{"droideka_hide_radius", &s.droideka_hide_radius},
           Entry{"gesture_throw", nullptr, &s.gesture_throw},
           Entry{"reload_bar_crosshair", nullptr, &s.reload_bar_crosshair},
           Entry{"reload_bar_wrist", nullptr, &s.reload_bar_wrist},
           Entry{"shake_cooling", &s.shake_cooling},
           Entry{"cooling_haptics", nullptr, &s.cooling_haptics},
           Entry{"crosshair_distance", &s.crosshair_distance},
           Entry{"crosshair_shown", nullptr, &s.crosshair_shown},
           Entry{"crosshair_steady", nullptr, &s.crosshair_steady},
           Entry{"saber_swing_speed", &s.saber_swing_speed},
           Entry{"saber_swing_distance", &s.saber_swing_distance},
           Entry{"saber_sound", nullptr, &s.saber_sound},
           Entry{"saber_sound_volume", &s.saber_sound_volume},
           Entry{"saber_sound_speed", &s.saber_sound_speed},
           Entry{"saber_sound_interval", &s.saber_sound_interval},
           Entry{"saber_auto_block", nullptr, &s.saber_auto_block},
           Entry{"hand_swap", nullptr, &s.hand_swap},
           Entry{"hand_swap_keeps_rotation", nullptr, &s.hand_swap_keeps_rotation},
           Entry{"draw_hand_arrow", nullptr, &s.draw_hand_arrow},
           Entry{"weapon_bash", nullptr, &s.weapon_bash},
           Entry{"bash_speed", &s.bash_speed},
           Entry{"bash_damage", &s.bash_damage},
           Entry{"bash_push", &s.bash_push},
           Entry{"hero_always_available", nullptr, &s.hero_always_available},
           Entry{"special_units_always_available", nullptr,
                 &s.special_units_always_available},
           Entry{"render_scale_percent", &s.render_scale_percent}};
}

void clamp_settings() noexcept
{
   Settings& s = current_settings;

   s.vignette_strength = std::clamp(s.vignette_strength, 0.0f, 1.0f);
   s.turn_mode = s.turn_mode == Turn_mode::smooth ? Turn_mode::smooth : Turn_mode::snap;
   s.move_direction = static_cast<Move_direction>(
      std::clamp(static_cast<int>(s.move_direction), 0, 2));
   s.snap_turn_degrees = std::clamp(s.snap_turn_degrees, 10.0f, 90.0f);
   s.smooth_turn_speed = std::clamp(s.smooth_turn_speed, 30.0f, 360.0f);
   s.overscan_degrees = std::clamp(s.overscan_degrees, 0.0f, 10.0f);
   s.play_mode = s.play_mode == Play_mode::seated ? Play_mode::seated : Play_mode::standing;
   s.body_scale = static_cast<Body_scale>(std::clamp(static_cast<int>(s.body_scale), 0, 2));
   s.player_eye_height = std::clamp(s.player_eye_height, 0.8f, 2.1f);

   for (float& zoom : s.scope_zooms) zoom = std::clamp(zoom, 1.5f, 12.0f);

   s.scope_zoom_mode = std::clamp(s.scope_zoom_mode, 0, 2);
   s.scope_zoom = s.scope_zooms[s.scope_zoom_mode];
   s.scope_size = std::clamp(s.scope_size, 0.05f, 0.25f);
   s.scope_height = std::clamp(s.scope_height, -0.05f, 0.3f);
   s.hud_size = std::clamp(s.hud_size, 0.4f, 1.2f);
   s.hand_smoothing = std::clamp(s.hand_smoothing, 0.0f, 1.0f);
   s.turret_eye_right = std::clamp(s.turret_eye_right, -1.0f, 1.0f);
   s.turret_eye_up = std::clamp(s.turret_eye_up, -1.0f, 1.0f);
   s.turret_eye_forward = std::clamp(s.turret_eye_forward, -1.0f, 1.0f);
   s.turret_hide_radius = std::clamp(s.turret_hide_radius, 0.0f, 5.0f);
   s.droideka_eye_height = std::clamp(s.droideka_eye_height, 0.0f, 2.5f);
   s.droideka_eye_forward = std::clamp(s.droideka_eye_forward, -2.0f, 2.0f);
   s.droideka_hide_radius = std::clamp(s.droideka_hide_radius, 0.0f, 3.0f);
#ifdef BF2VR_PUBLIC
   // Not working yet: the public build keeps the rolled droideka on the game's camera.
   s.droideka_first_person = false;
#endif
   s.turret_hand_yaw = std::clamp(s.turret_hand_yaw, 0.0f, 60.0f);
   s.crosshair_distance = std::clamp(s.crosshair_distance, 1.0f, 50.0f);
   s.shake_cooling = std::clamp(s.shake_cooling, 1.0f, 10.0f);
   s.vehicle_steer_mode = s.vehicle_steer_mode == Vehicle_steer_mode::speed
                             ? Vehicle_steer_mode::speed
                             : Vehicle_steer_mode::position;
   s.vehicle_stick_range = std::clamp(s.vehicle_stick_range, 50.0f, 2000.0f);
   s.vehicle_steer_speed = std::clamp(s.vehicle_steer_speed, 200.0f, 6000.0f);
   s.saber_swing_speed = std::clamp(s.saber_swing_speed, 3.0f, 12.0f);
   s.saber_swing_distance = std::clamp(s.saber_swing_distance, 0.0f, 0.5f);
   s.saber_sound_volume = std::clamp(s.saber_sound_volume, 0.0f, 1.0f);
   s.saber_sound_speed = std::clamp(s.saber_sound_speed, 3.0f, 15.0f);
   s.saber_sound_interval = std::clamp(s.saber_sound_interval, 0.05f, 1.5f);
   s.bash_speed = std::clamp(s.bash_speed, 1.5f, 8.0f);
   s.bash_damage = std::clamp(s.bash_damage, 0.0f, 1000.0f);
   s.bash_push = std::clamp(s.bash_push, 0.0f, 12.0f);
   s.render_scale_percent = std::clamp(s.render_scale_percent, 40.0f, 150.0f);
}

// An older line's hand mode (with `n` parts: 0 nothing, 1..n part mode alone, n+1..2n-1
// the weapon and part mode - n + 1; -1 the defaults) as a part mask.
auto hand_mode_parts(const int mode, const int n) noexcept -> int
{
   if (mode < 0) return -1;
   if (mode == 0) return 0;
   if (mode <= n) return 1 << (mode - 1);

   return 1 | (1 << (mode - n));
}

void load_arm_tunings() noexcept
{
   if (std::exchange(arm_tunings_loaded, true)) return;

   FILE* file = nullptr;

   if (_wfopen_s(&file, (directory() + L"bf2vr_arms.txt").c_str(), L"r") != 0 || !file) return;

   char line[512]{};

   // "model rx ry rz lx ly lz [mirror axis sx sy sz scale [pR pL [mx my mz]]]", R and L
   // the hands' part masks (-1 = defaults), m the muzzle offset. Older lines: two hand
   // modes without the p (converted), or one offset of the mirror's plane along its
   // axis after the axis (a shift of twice that).
   while (std::fgets(line, sizeof(line), file)) {
      char model[256]{};
      Arm_tuning tuning;
      int mirror = 0;
      float scale = 0.0f;

      int right_parts = -1;
      int left_parts = -1;

      int fields = std::sscanf(
         line, "%255s %f %f %f %f %f %f %d %d %f %f %f %f p%d p%d m %f %f %f", model,
         &tuning.right_offset.x, &tuning.right_offset.y, &tuning.right_offset.z,
         &tuning.left_offset.x, &tuning.left_offset.y, &tuning.left_offset.z, &mirror,
         &tuning.mirror_axis, &tuning.mirror_shift.x, &tuning.mirror_shift.y,
         &tuning.mirror_shift.z, &scale, &right_parts, &left_parts, &tuning.muzzle_offset.x,
         &tuning.muzzle_offset.y, &tuning.muzzle_offset.z);

      if (fields == 13) {
         int right_mode = -1;
         int left_mode = -1;
         char skipped[256]{};

         if (std::sscanf(line, "%255s %*f %*f %*f %*f %*f %*f %*d %*d %*f %*f %*f %*f %d %d",
                         skipped, &right_mode, &left_mode) == 3) {
            const int n = 1 + static_cast<int>(std::count(model, model + std::strlen(model), '-'));

            right_parts = hand_mode_parts(right_mode, n);
            left_parts = hand_mode_parts(left_mode, n);
            fields = 15;
         }
      }

      if (fields < 7 || model[0] == '#') continue;

      tuning.mirror = mirror != 0;
      tuning.mirror_axis = std::clamp(tuning.mirror_axis, 0, 2);

      if (fields == 10) {
         const float plane_offset = tuning.mirror_shift.x;

         tuning.mirror_shift = {};
         tuning.mirror_shift[tuning.mirror_axis] = 2.0f * plane_offset;
      }

      if (fields >= 13) tuning.mirror_scale = std::clamp(scale, 0.8f, 1.05f);

      if (fields >= 15) {
         tuning.right_parts = right_parts;
         tuning.left_parts = left_parts;
      }

      // Older lines (no "m") held offsets from the game's fire point, not the barrel tip.
      if (fields < 18) tuning.muzzle_offset = {};

      arm_tunings[model] = tuning;
   }

   std::fclose(file);

   log_fmt(Log_level::info, "VR: {} first-person model corrections from bf2vr_arms.txt.",
           arm_tunings.size());
}

// --- The panel -----------------------------------------------------------------

struct Panel {
   bool tried = false;
   ImGuiContext* context = nullptr;
   bool mouse_down = false;
   bool settings_dirty = false; // saved once no slider is held
   bool arms_dirty = false;
};

Panel panel;

bool panel_ready(ID3D11Device& device, ID3D11DeviceContext& dc) noexcept
{
   if (panel.tried) return panel.context != nullptr;

   panel.tried = true;

   ImGuiContext* const previous = ImGui::GetCurrentContext();

   // Its own font atlas: the default font at twice its size (the panel is seen from
   // half a metre away).
   panel.context = ImGui::CreateContext();
   ImGui::SetCurrentContext(panel.context);

   ImGuiIO& io = ImGui::GetIO();
   io.IniFilename = nullptr;
   io.LogFilename = nullptr;
   io.MouseDrawCursor = true;

   ImFontConfig font{};
   font.SizePixels = 26.0f;
   io.Fonts->AddFontDefault(&font);

   ImGui::StyleColorsDark();
   ImGui::GetStyle().ScaleAllSizes(2.0f);
   ImGui::GetStyle().WindowRounding = 0.0f;
   ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f; // the texture around is clear

   const bool ok = ImGui_ImplDX11_Init(&device, &dc);

   ImGui::SetCurrentContext(previous);

   if (!ok) {
      ImGui::DestroyContext(panel.context);
      panel.context = nullptr;
   }

   log(ok ? Log_level::info : Log_level::error,
       ok ? "VR: settings panel ready." : "VR: settings panel failed.");

   return ok;
}

bool slider(const char* label, float& value, const float min, const float max,
            const char* format) noexcept
{
   return ImGui::SliderFloat(label, &value, min, max, format, ImGuiSliderFlags_AlwaysClamp);
}

}

auto settings() noexcept -> Settings&
{
   return current_settings;
}

void load_settings() noexcept
{
   if (std::exchange(settings_loaded, true)) return;

   bool render_scale_read = false;
   FILE* file = nullptr;

   if (_wfopen_s(&file, (directory() + L"bf2vr_settings.txt").c_str(), L"r") == 0 && file) {
      char line[256]{};
      auto list = entries();

      while (std::fgets(line, sizeof(line), file)) {
         char name[64]{};
         float value = 0.0f;

         if (std::sscanf(line, "%63s %f", name, &value) != 2) continue;

         for (Entry& entry : list) {
            if (std::string_view{name} != entry.name) continue;

            if (entry.number) *entry.number = value;
            if (entry.flag) *entry.flag = value != 0.0f;
            if (entry.choice) *entry.choice = static_cast<int>(value);
            if (entry.number == &current_settings.render_scale_percent) {
               render_scale_read = true;
            }
         }
      }

      std::fclose(file);

      log(Log_level::info, "VR: settings from bf2vr_settings.txt.");
   }

   if (!render_scale_read &&
       _wfopen_s(&file, (directory() + L"bf2vr_render_scale.txt").c_str(), L"r") == 0 && file) {
      float percent = 0.0f;

      if (std::fscanf(file, "%f", &percent) == 1 && percent > 0.0f) {
         current_settings.render_scale_percent = percent;
      }

      std::fclose(file);
   }

   clamp_settings();

   std::string text;

   for (const Entry& entry : entries()) {
      const float value = entry.number   ? *entry.number
                          : entry.flag   ? (*entry.flag ? 1.0f : 0.0f)
                          : entry.choice ? static_cast<float>(*entry.choice)
                                         : 0.0f;

      text += fmt::format("{}{} {:g}", text.empty() ? "" : ", ", entry.name, value);
   }

   log_fmt(Log_level::info, "VR: settings in effect: {}.", text);
}

void save_settings() noexcept
{
   clamp_settings();

   FILE* file = nullptr;

   if (_wfopen_s(&file, (directory() + L"bf2vr_settings.txt").c_str(), L"w") != 0 || !file) {
      log(Log_level::warning, "VR: can't write bf2vr_settings.txt.");

      return;
   }

   std::fputs("# Star Wars Battlefront II VR settings (also changed in the VR panel).\n", file);

   for (const Entry& entry : entries()) {
      if (entry.number) std::fprintf(file, "%s %g\n", entry.name, *entry.number);
      if (entry.flag) std::fprintf(file, "%s %d\n", entry.name, *entry.flag ? 1 : 0);
      if (entry.choice) std::fprintf(file, "%s %d\n", entry.name, *entry.choice);
   }

   std::fclose(file);
}

auto arm_tuning(const std::string& model) noexcept -> Arm_tuning&
{
   load_arm_tunings();

   return arm_tunings[model];
}

void save_arm_tuning() noexcept
{
   FILE* file = nullptr;

   if (_wfopen_s(&file, (directory() + L"bf2vr_arms.txt").c_str(), L"w") != 0 || !file) {
      log(Log_level::warning, "VR: can't write bf2vr_arms.txt.");

      return;
   }

   std::fputs("# First-person model corrections: model (its parts' sizes), then the right and\n"
              "# left hand's grip offsets in the game camera's view space (metres, x right,\n"
              "# y up, -z forward), then whether the weapon is also drawn mirrored, the\n"
              "# mirror's axis (0 x, 1 y, 2 z of the part), the copy's shift (metres along\n"
              "# the part's x, y, z) and scale, then the parts the right and left hand hold\n"
              "# (p and a bit mask: 1 the weapon, 2 the second biggest part, 4 the third...;\n"
              "# p-1 = the defaults), then m and where the shots start moved from the\n"
              "# barrel tip (metres: right, up, back). Tuned in the VR panel.\n",
              file);

   for (const auto& [model, tuning] : arm_tunings) {
      if (tuning.right_offset == glm::vec3{} && tuning.left_offset == default_left_offset &&
          !tuning.mirror && tuning.right_parts < 0 && tuning.left_parts < 0 &&
          tuning.muzzle_offset == glm::vec3{}) {
         continue;
      }

      std::fprintf(file,
                   "%s %.3f %.3f %.3f %.3f %.3f %.3f %d %d %.3f %.3f %.3f %.3f p%d p%d m "
                   "%.3f %.3f %.3f\n",
                   model.c_str(), tuning.right_offset.x, tuning.right_offset.y,
                   tuning.right_offset.z, tuning.left_offset.x, tuning.left_offset.y,
                   tuning.left_offset.z, tuning.mirror ? 1 : 0, tuning.mirror_axis,
                   tuning.mirror_shift.x, tuning.mirror_shift.y, tuning.mirror_shift.z,
                   tuning.mirror_scale, tuning.right_parts, tuning.left_parts,
                   tuning.muzzle_offset.x, tuning.muzzle_offset.y, tuning.muzzle_offset.z);
   }

   std::fclose(file);
}

// The parts' sizes from a model's name ("3609-2025": elements, biggest first).
auto model_part_sizes(const std::string& model) -> std::vector<std::string>
{
   std::vector<std::string> sizes;
   std::size_t start = 0;

   while (start <= model.size()) {
      const std::size_t end = std::min(model.find('-', start), model.size());

      sizes.push_back(model.substr(start, end - start));
      start = end + 1;
   }

   return sizes;
}

auto draw_panel(ID3D11Device& device, ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                const glm::uvec2 size, const glm::vec2 pointer, const bool click,
                const float dt, const Panel_status& status) noexcept -> Panel_requests
{
   Panel_requests requests;

   if (!panel_ready(device, dc)) return requests;

   ImGuiContext* const previous = ImGui::GetCurrentContext();
   ImGui::SetCurrentContext(panel.context);

   ImGuiIO& io = ImGui::GetIO();
   io.DisplaySize = {static_cast<float>(size.x), static_cast<float>(size.y)};
   io.DeltaTime = std::clamp(dt, 1.0f / 1000.0f, 0.25f);

   const bool on_panel = pointer.x >= 0.0f && pointer.y >= 0.0f;

   io.AddMousePosEvent(on_panel ? pointer.x * io.DisplaySize.x : -FLT_MAX,
                       on_panel ? pointer.y * io.DisplaySize.y : -FLT_MAX);

   if (click != panel.mouse_down) {
      panel.mouse_down = click;
      io.AddMouseButtonEvent(0, click);
   }

   ImGui_ImplDX11_NewFrame();
   ImGui::NewFrame();

   const ImVec2 window_size{static_cast<float>(panel_window_px), io.DisplaySize.y};

   ImGui::SetNextWindowPos({0.0f, 0.0f});
   ImGui::SetNextWindowSize(window_size);

   Settings& s = current_settings;
   bool changed = false;

   if (ImGui::Begin("VR settings", nullptr,
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoCollapse)) {
      // Performance: the game's frame rate against the headset's refresh rate.
      if (status.fps > 0.0f) {
         const bool behind = status.refresh_hz > 0.0f && status.fps < status.refresh_hz * 0.95f;

         if (status.refresh_hz > 0.0f) {
            ImGui::TextColored(behind ? ImVec4{1.0f, 0.6f, 0.3f, 1.0f}
                                      : ImVec4{0.5f, 1.0f, 0.5f, 1.0f},
                               "%.0f fps, headset %.0f Hz", status.fps, status.refresh_hz);
         }
         else {
            ImGui::Text("%.0f fps", status.fps);
         }

         if (behind) {
            ImGui::SameLine();
            ImGui::TextUnformatted("(below the headset's rate: lower Render scale in View)");
         }
      }

      if (ImGui::CollapsingHeader("Comfort", ImGuiTreeNodeFlags_DefaultOpen)) {
         changed |= ImGui::Checkbox("Vignette while moving", &s.vignette);

         if (s.vignette) {
            changed |= slider("Vignette strength", s.vignette_strength, 0.0f, 1.0f, "%.2f");
         }

         int mode = static_cast<int>(s.turn_mode);

         changed |= ImGui::RadioButton("Snap turn", &mode, 0);
         ImGui::SameLine();
         changed |= ImGui::RadioButton("Smooth turn", &mode, 1);
         s.turn_mode = static_cast<Turn_mode>(mode);

         if (s.turn_mode == Turn_mode::snap) {
            changed |= slider("Snap angle", s.snap_turn_degrees, 10.0f, 90.0f, "%.0f deg");
         }
         else {
            changed |= slider("Turn speed", s.smooth_turn_speed, 30.0f, 360.0f, "%.0f deg/s");
         }

         changed |= ImGui::Checkbox("Sprint: click to toggle (else hold)", &s.sprint_toggle);

         int direction = static_cast<int>(s.move_direction);

         ImGui::TextUnformatted("Movement direction");
         changed |= ImGui::RadioButton("HMD", &direction, 0);
         ImGui::SameLine();
         changed |= ImGui::RadioButton("Right controller", &direction, 1);
         ImGui::SameLine();
         changed |= ImGui::RadioButton("Left controller", &direction, 2);
         s.move_direction = static_cast<Move_direction>(direction);

         changed |= ImGui::Checkbox("Left-handed sticks (right stick moves, left turns)",
                                    &s.swap_sticks);
         changed |= ImGui::Checkbox("Pause when the headset's system menu opens",
                                    &s.pause_on_focus_loss);

         changed |= ImGui::Checkbox("Head sway while running", &s.head_sway);
         changed |= ImGui::Checkbox("Weapon sway while running", &s.weapon_sway);
         changed |= ImGui::Checkbox("Screen blur (jedi sprint, zoom)", &s.screen_blur);
      }

      if (ImGui::CollapsingHeader("View")) {
         changed |= slider("Edge margin", s.overscan_degrees, 0.0f, 10.0f, "%.1f deg");
         changed |= ImGui::Checkbox("Wider margin while turning the head", &s.overscan_by_speed);
         changed |= ImGui::Checkbox("Billboards and shadows follow the head", &s.head_view);
         changed |= ImGui::Checkbox("Small characters see from their eyes (culling)",
                                    &s.cull_from_eyes);
         changed |= slider("Render scale (restart)", s.render_scale_percent, 40.0f, 150.0f,
                           "%.0f%%");
      }

      if (ImGui::CollapsingHeader("Height", ImGuiTreeNodeFlags_DefaultOpen)) {
         int mode = static_cast<int>(s.play_mode);

         changed |= ImGui::RadioButton("Standing", &mode, 0);
         ImGui::SameLine();
         changed |= ImGui::RadioButton("Seated", &mode, 1);
         s.play_mode = static_cast<Play_mode>(mode);

         ImGui::TextWrapped(s.play_mode == Play_mode::standing
                               ? "Stand up straight, look ahead and press Measure height "
                                 "(once: it's kept). Crouch for real or with the right stick."
                               : "Sit as you play, look ahead and press Measure height "
                                 "(Recenter does it too). Crouch with the right stick.");

         if (ImGui::Button("Measure height")) requests.measure = true;

         if (!status.measure.empty()) {
            ImGui::SameLine();
            ImGui::TextUnformatted(status.measure.c_str());
         }

         if (!status.body.empty()) ImGui::TextWrapped("%s", status.body.c_str());

         if (ImGui::TreeNode("Advanced")) {
            int body_scale = static_cast<int>(s.body_scale);

            ImGui::TextUnformatted("Character's height");
            changed |= ImGui::RadioButton("Off", &body_scale, 0);
            ImGui::SameLine();
            changed |= ImGui::RadioButton("Eye height", &body_scale, 1);
            ImGui::SameLine();
            changed |= ImGui::RadioButton("Eye height and size", &body_scale, 2);
            s.body_scale = static_cast<Body_scale>(body_scale);

            if (slider("My standing eye height", s.player_eye_height, 0.8f, 2.1f, "%.2f m")) {
               s.player_height_measured = true;
               changed = true;
            }

            if (s.play_mode == Play_mode::standing) {
               changed |= ImGui::Checkbox("Crouching in the room crouches the soldier",
                                          &s.physical_crouch);
            }

            ImGui::TreePop();
         }
      }

      if (ImGui::CollapsingHeader("Weapons", ImGuiTreeNodeFlags_DefaultOpen)) {
         changed |= slider("Controller smoothing", s.hand_smoothing, 0.0f, 1.0f, "%.2f");
         changed |= ImGui::Checkbox("Shots leave the gun's muzzle", &s.fire_from_muzzle);
         changed |= ImGui::Checkbox("Scope when the gun is raised to the eye", &s.scope_auto);
         ImGui::TextWrapped("Scope zooms: double-click the scope button (right grip) for the "
                            "next one (now zoom %d).",
                            s.scope_zoom_mode + 1);
         changed |= slider("Zoom 1", s.scope_zooms[0], 1.5f, 12.0f, "%.1fx");
         changed |= slider("Zoom 2", s.scope_zooms[1], 1.5f, 12.0f, "%.1fx");
         changed |= slider("Zoom 3", s.scope_zooms[2], 1.5f, 12.0f, "%.1fx");
         s.scope_zoom = s.scope_zooms[std::clamp(s.scope_zoom_mode, 0, 2)];
         changed |= slider("Scope window", s.scope_size, 0.05f, 0.25f, "%.2f m");
         changed |= slider("Scope height", s.scope_height, -0.05f, 0.3f, "%.2f m");
         changed |= ImGui::Checkbox("Lock scope rotation (turns with the controller)",
                                    &s.scope_locked);
         changed |= ImGui::Checkbox("Throw grenades with the left hand's swing", &s.gesture_throw);
         changed |= ImGui::Checkbox("Crosshair", &s.crosshair_shown);
         changed |= ImGui::Checkbox("Steady crosshair (no recoil, reload or step sway)", &s.crosshair_steady);
         changed |= slider("Crosshair distance", s.crosshair_distance, 1.0f, 50.0f, "%.1f m");
         changed |= ImGui::Checkbox("Quick reload bar under the crosshair",
                                    &s.reload_bar_crosshair);
         changed |= ImGui::Checkbox("Quick reload bar on the right wrist", &s.reload_bar_wrist);
         changed |= slider("Overheated gun: cooling while shaken", s.shake_cooling, 1.0f,
                           10.0f, "%.1fx");
         changed |= ImGui::Checkbox("Overheated gun: vibrates while it cools",
                                    &s.cooling_haptics);
      }

      if (ImGui::CollapsingHeader("Weapon bash")) {
         changed |= ImGui::Checkbox("Hit with the gun (a fast jab or swing)", &s.weapon_bash);

         if (s.weapon_bash) {
            changed |= slider("Swing speed##bash", s.bash_speed, 1.5f, 8.0f, "%.1f m/s");
            changed |= slider("Damage##bash", s.bash_damage, 0.0f, 1000.0f, "%.0f");
            changed |= slider("Push##bash", s.bash_push, 0.0f, 12.0f, "%.1f");
         }
      }

      if (ImGui::CollapsingHeader("Lightsaber")) {
         changed |= slider("Swing speed", s.saber_swing_speed, 3.0f, 12.0f, "%.1f m/s");
         changed |= slider("Swing length", s.saber_swing_distance, 0.0f, 0.5f, "%.2f m");
         changed |= ImGui::Checkbox("Auto block (bolts near the blade)", &s.saber_auto_block);
         changed |= ImGui::Checkbox("Swing sound (the game's)", &s.saber_sound);

         if (s.saber_sound) {
            changed |= slider("Volume##saber_sound", s.saber_sound_volume, 0.0f, 1.0f, "%.2f");
            changed |= slider("Sound from speed", s.saber_sound_speed, 3.0f, 15.0f, "%.1f m/s");
            changed |= slider("Sound gap", s.saber_sound_interval, 0.05f, 1.5f, "%.2f s");
            ImGui::TextWrapped("Slower swings are silent, faster ones louder. No sound sooner "
                               "than the gap after the last; quick runs get quieter.");
         }
         ImGui::TextWrapped("A swing hits once the blade's tip moves faster than the speed "
                            "for the length. Higher: fewer accidental hits.");
         changed |= ImGui::Checkbox("Draw hand arrow when rotating sword", &s.draw_hand_arrow);
         ImGui::TextWrapped("The saber hand's grip held keeps the saber's rotation; let go, "
                            "it stays turned. A double click of that grip turns it back.");
      }

      if (ImGui::CollapsingHeader("Hand swap")) {
         changed |= ImGui::Checkbox("Swap hands (free hand's grip at the weapon)", &s.hand_swap);
         changed |= ImGui::Checkbox("The weapon keeps its rotation in the new hand",
                                    &s.hand_swap_keeps_rotation);
         ImGui::TextWrapped("Hands together (both vibrate): press the free hand's grip. Near "
                            "each other: click it twice. The weapon goes to that hand (mirrored), "
                            "with the triggers and grips.");
      }

      if (ImGui::CollapsingHeader("HUD", ImGuiTreeNodeFlags_DefaultOpen)) {
         changed |= ImGui::Checkbox("Draw HUD on the wrists", &s.wrist_hud);
         changed |= ImGui::Checkbox("Draw HUD on the head", &s.head_hud);

         if (s.head_hud) changed |= slider("HUD size", s.hud_size, 0.4f, 1.2f, "%.2f");
      }

      if (ImGui::CollapsingHeader("Turret")) {
         changed |= slider("Eyes right", s.turret_eye_right, -1.0f, 1.0f, "%.2f m");
         changed |= slider("Eyes up", s.turret_eye_up, -1.0f, 1.0f, "%.2f m");
         changed |= slider("Eyes forward", s.turret_eye_forward, -1.0f, 1.0f, "%.2f m");
         changed |= slider("Hide soldiers within", s.turret_hide_radius, 0.0f, 5.0f, "%.1f m");
         changed |= slider("Turn with the hand (left / right)", s.turret_hand_yaw, 0.0f, 60.0f,
                           "%.0f deg");
      }

      if (ImGui::CollapsingHeader("Vehicles")) {
         int steer = static_cast<int>(s.vehicle_steer_mode);

         ImGui::TextUnformatted("Right stick steers like");
         changed |= ImGui::RadioButton("Joystick (position)", &steer, 0);
         ImGui::SameLine();
         changed |= ImGui::RadioButton("Mouse (speed)", &steer, 1);
         s.vehicle_steer_mode = static_cast<Vehicle_steer_mode>(steer);

         if (s.vehicle_steer_mode == Vehicle_steer_mode::position) {
            changed |= slider("Stick range", s.vehicle_stick_range, 50.0f, 2000.0f, "%.0f");
         }
         else {
            changed |= slider("Steering speed", s.vehicle_steer_speed, 200.0f, 6000.0f, "%.0f");
         }

         changed |= ImGui::Checkbox("Rolled-up droideka in the headset (not on the screen)",
                                    &s.droideka_rolled_view);

#ifndef BF2VR_PUBLIC
         if (s.droideka_rolled_view) {
            changed |= ImGui::Checkbox("Rolled droideka: first-person view",
                                       &s.droideka_first_person);

            if (s.droideka_first_person) {
               changed |= slider("Droideka eye height", s.droideka_eye_height, 0.0f, 2.5f,
                                 "%.2f m");
               changed |= slider("Droideka eye forward", s.droideka_eye_forward, -2.0f, 2.0f,
                                 "%.2f m");
               changed |= slider("Hide own droideka within", s.droideka_hide_radius, 0.0f,
                                 3.0f, "%.2f m");
            }
         }
#endif
      }

      if (!status.bindings.empty() && ImGui::CollapsingHeader("Game keys")) {
         ImGui::TextWrapped("%s", status.bindings.c_str());
      }

      if (ImGui::CollapsingHeader("Debug Cheat")) {
         changed |= ImGui::Checkbox("Hero always available", &s.hero_always_available);
         ImGui::TextWrapped("On maps with a hero, the hero is in the class select without "
                            "unlocking it.");
         changed |= ImGui::Checkbox("Special units always available",
                                    &s.special_units_always_available);
         ImGui::TextWrapped("Classes that need points to unlock (droideka, officer, ...) can "
                            "be picked without them; the map's limit on how many may play "
                            "them at once still holds.");
      }

      ImGui::Separator();

      if (ImGui::Button("Recenter")) requests.recenter = true;

      ImGui::SameLine();

      if (ImGui::Button("Close")) requests.close = true;
   }

   ImGui::End();

   // The hands' window, right of the settings.
   ImGui::SetNextWindowPos({static_cast<float>(panel_window_px + panel_gap_px), 0.0f});
   ImGui::SetNextWindowSize(window_size);

   if (ImGui::Begin("Hands", nullptr,
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoCollapse)) {
      ImGui::TextUnformatted("Arm models in the hands (the gun always shows)");
      changed |= ImGui::Checkbox("Left arm", &s.show_left_arm);
      ImGui::SameLine();
      changed |= ImGui::Checkbox("Right arm", &s.show_right_arm);
      ImGui::Separator();

      if (status.model.empty()) {
         ImGui::TextWrapped("Hold a weapon in a match to move the hands on its model.");
      }
      else {
         Arm_tuning& tuning = arm_tuning(status.model);
         bool arms_changed = false;

         ImGui::TextWrapped("Model %s (offsets in metres)", status.model.c_str());

         // Which of the model's parts each hand holds.
         if (status.model_parts > 0) {
            const int n = std::min(status.model_parts, 8);
            const std::vector<std::string> sizes = model_part_sizes(status.model);

            // One checkbox per part: any part in either hand, or both, or neither.
            const auto parts = [&](const char* hand, const std::uint32_t held, int& saved) {
               ImGui::PushID(hand);
               ImGui::TextUnformatted(hand);

               for (int i = 0; i < n; ++i) {
                  bool checked = ((held >> i) & 1u) != 0;
                  const std::string label = fmt::format(
                     "{}{} ({})", i == 0 ? "weapon" : "part ", i == 0 ? "" : std::to_string(i + 1),
                     i < static_cast<int>(sizes.size()) ? sizes[i] : "?");

                  if (i > 0) ImGui::SameLine();

                  if (ImGui::Checkbox(label.c_str(), &checked)) {
                     saved = static_cast<int>(checked ? held | (1u << i) : held & ~(1u << i));
                     arms_changed = true;
                  }
               }

               ImGui::PopID();
            };

            parts("Right hand holds", status.right_hand_parts, tuning.right_parts);
            parts("Left hand holds", status.left_hand_parts, tuning.left_parts);
            ImGui::TextWrapped("Parts are ranked by size (numbers: their triangles' counts). "
                               "A hand's grip is its biggest part.");
            ImGui::Separator();
         }

         const auto offsets = [&](const char* hand, glm::vec3& offset, const float range) {
            ImGui::PushID(hand);
            ImGui::TextUnformatted(hand);
            arms_changed |= slider("Right (+) / left (-)", offset.x, -range, range, "%.3f");
            arms_changed |= slider("Up (+) / down (-)", offset.y, -range, range, "%.3f");
            arms_changed |= slider("Back (+) / forward (-)", offset.z, -range, range, "%.3f");
            ImGui::PopID();
         };

         offsets("Right hand", tuning.right_offset, 0.15f);
         offsets("Left hand", tuning.left_offset, 0.3f);

         ImGui::Separator();
         arms_changed |= ImGui::Checkbox("Mirror the weapon model", &tuning.mirror);

         if (tuning.mirror) {
            ImGui::TextUnformatted("Mirror axis");
            ImGui::SameLine();
            arms_changed |= ImGui::RadioButton("X", &tuning.mirror_axis, 0);
            ImGui::SameLine();
            arms_changed |= ImGui::RadioButton("Y", &tuning.mirror_axis, 1);
            ImGui::SameLine();
            arms_changed |= ImGui::RadioButton("Z", &tuning.mirror_axis, 2);
            arms_changed |= ImGui::SliderFloat3("Mirror shift (m)", &tuning.mirror_shift.x,
                                                -0.1f, 0.1f, "%.3f");
            arms_changed |= ImGui::SliderFloat("Mirror scale", &tuning.mirror_scale, 0.8f,
                                               1.05f, "%.3f");
         }

         ImGui::Separator();
         ImGui::TextUnformatted("Where the shots start (moved from the barrel tip)");
         changed |= ImGui::Checkbox("Show the shot line", &s.show_shot_line);
         ImGui::PushID("muzzle");
         arms_changed |= slider("Right (+) / left (-)", tuning.muzzle_offset.x, -0.3f, 0.3f, "%.3f");
         arms_changed |= slider("Up (+) / down (-)", tuning.muzzle_offset.y, -0.3f, 0.3f, "%.3f");
         arms_changed |= slider("Back (+) / forward (-)", tuning.muzzle_offset.z, -0.6f, 0.6f, "%.3f");
         ImGui::PopID();

         if (ImGui::Button("Reset this model's hands")) {
            tuning = {};
            arms_changed = true;
         }

         if (arms_changed) panel.arms_dirty = true;
      }
   }

   ImGui::End();

   // Saved once the slider being dragged is let go.
   panel.settings_dirty |= changed;

   if (!ImGui::IsAnyItemActive()) {
      if (std::exchange(panel.settings_dirty, false)) save_settings();
      if (std::exchange(panel.arms_dirty, false)) save_arm_tuning();
   }

   ImGui::Render();

   const std::array<float, 4> background{0.0f, 0.0f, 0.0f, 0.0f}; // the gap stays clear
   ID3D11RenderTargetView* previous_rtv = nullptr;
   ID3D11DepthStencilView* previous_dsv = nullptr;

   dc.OMGetRenderTargets(1, &previous_rtv, &previous_dsv);

   ID3D11RenderTargetView* const target = &rtv;

   dc.ClearRenderTargetView(target, background.data());
   dc.OMSetRenderTargets(1, &target, nullptr);

   const D3D11_VIEWPORT viewport{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y, 0.0f, 1.0f};
   dc.RSSetViewports(1, &viewport);

   ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

   dc.OMSetRenderTargets(1, &previous_rtv, previous_dsv);

   if (previous_rtv) previous_rtv->Release();
   if (previous_dsv) previous_dsv->Release();

   ImGui::SetCurrentContext(previous);

   return requests;
}

}
