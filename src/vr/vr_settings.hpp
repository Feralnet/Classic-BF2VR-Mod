#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <d3d11_1.h>

#include <glm/glm.hpp>

// The player's VR options: kept in bf2vr_settings.txt next to d3d9.dll and changed in
// the VR panel (an ImGui window shown beside the menu screen, or in front of the player
// when opened in a match).
namespace sp::vr {

enum class Turn_mode : int { snap, smooth };

// What the left stick's forward is: where the head looks or where a controller points.
enum class Move_direction : int { head, right_hand, left_hand };

// How the character's height shows (see Settings::body_scale).
enum class Body_scale : int { off, height, full };

// How the player plays: standing (their head's height over the floor against their
// measured standing eye height moves the character's eyes, crouching in the room
// crouches the soldier) or seated (the character stands at its own eye height; the head
// moves it from where it was at Recenter / Measure height).
enum class Play_mode : int { standing, seated };

enum class Vehicle_steer_mode : int { position, speed };

struct Settings {
   // Comfort.
   bool vignette = true;          // darken the view's edges while moving or turning
   float vignette_strength = 0.6f; // 0..1
   Turn_mode turn_mode = Turn_mode::snap;
   float snap_turn_degrees = 30.0f;
   float smooth_turn_speed = 120.0f; // degrees per second
   bool sprint_toggle = true;        // a click sprints until the stick is let go
   Move_direction move_direction = Move_direction::right_hand;
   // Left-handed: the right stick moves, the left one turns (their clicks and the pads
   // swap too).
   bool swap_sticks = false;
   // A system menu opened over a match (the headset's own button) pauses the game.
   bool pause_on_focus_loss = true;
   bool head_sway = false;   // the view bobs with the soldier's steps (off: steadied)
   bool weapon_sway = false; // the gun bobs in the hand with the steps (off: steadied)
   bool screen_blur = false; // the game's full-screen blurs (jedi sprint, zoom)

   // View.
   float overscan_degrees = 2.0f; // extra field of view rendered at the edges
   // And more while the head turns fast. Off by default: the submitted field of view then
   // changes from frame to frame, and Steam Link (Steam Frame, Quest) showed a doubled
   // image while turning at 90 Hz and up (player reports 2026-11-02).
   bool overscan_by_speed = false;
   bool head_view = true;         // billboards, shadows and flares follow the head
   bool cull_from_eyes = true;    // a small character's view culled from its eyes

   // Body: the eyes stand on the soldier's feet at the character's eye height (a melee
   // character's from its head bone, anyone else's from the game camera over the feet
   // while it aims level), never lower than a hand's breadth over them. The player's
   // tracked motion (head, hands, the eyes' spacing) is scaled by the character's eye
   // height over the player's (full) or not (height); off = the eyes at the game camera.
   Play_mode play_mode = Play_mode::standing;
   Body_scale body_scale = Body_scale::full;
   // The player's standing eye height (metres): taken by the panel's Measure height
   // (then `player_height_measured`), else (standing) from the headset's floor as the
   // highest the head has been held, else this.
   float player_eye_height = 1.65f;
   bool player_height_measured = false;
   bool physical_crouch = true; // standing: crouching in the room crouches the soldier

   // Scope: a magnified view along the gun on a window over the right hand.
   bool scope_auto = true; // shown while the gun is raised to the eye
   // Three zooms a double click of the scope button (right grip) cycles through (up /
   // down on the right stick step through them while the scope is up); `scope_zoom` is
   // the current one's.
   std::array<float, 3> scope_zooms{2.0f, 4.0f, 8.0f};
   int scope_zoom_mode = 1;
   float scope_zoom = 4.0f;
   float scope_size = 0.11f;   // window width, metres
   float scope_height = 0.07f; // window above the controller, metres
   bool scope_locked = false;  // the window turns with the controller (else faces the eyes)

   // HUD and hands: the HUD in front of the eyes (head-locked, this fraction of the
   // game's) and / or in panels on the wrists. The crosshair is always drawn.
   float hud_size = 0.9f;
   bool head_hud = false;
   bool wrist_hud = true;

   // The first-person model's arms drawn in the hands (off: only the gun, for videos).
   bool show_left_arm = true;
   bool show_right_arm = true;

   // Controller smoothing, 0 (off) .. 1: slow, small movements (holding still, aiming)
   // are steadied, fast ones (swings, throws) pass with little lag (a One Euro filter).
   float hand_smoothing = 0.3f;

   // Bolts leave the gun in the hand (its muzzle, along its barrel) instead of the
   // game's first-person gun in front of the camera.
   bool fire_from_muzzle = true;

   // A thin red line from where the shots start, along their direction, while a gun is
   // held (for tuning a model's muzzle in the VR panel's hands window).
   bool show_shot_line = false;

   // Turret: the eyes moved from the point its camera circles (metres, along the
   // turret's heading).
   float turret_eye_right = 0.0f;
   float turret_eye_up = 0.0f;
   float turret_eye_forward = 0.0f;
   float turret_hide_radius = 2.5f; // soldiers this close to the eyes aren't drawn (metres)
   // The turret turns left / right with the head, and the right hand turns it on from
   // there by up to this much (degrees) as it points away from where the head looks.
   float turret_hand_yaw = 20.0f;

   // Vehicles (on the virtual screen): the right stick steers with mouse movement, either
   // as the mouse's position (the stick held out = the mouse moved out that far, let go =
   // moved back: the game's mouse steering keeps a moved mouse's turn going) or as its
   // speed (mouse counts per second at full deflection).
   Vehicle_steer_mode vehicle_steer_mode = Vehicle_steer_mode::speed;
   float vehicle_stick_range = 400.0f; // mouse counts at full deflection (position)
   float vehicle_steer_speed = 1500.0f;
   // A rolled-up droideka is seen in the headset from the game's camera over it (the view
   // turning with the droideka), not on the virtual screen.
   bool droideka_rolled_view = true;
   // ... from inside it (first person: the eyes this high over the droideka's origin,
   // turning only with its heading, never rolling with the ball), else from the game's
   // camera behind it.
   bool droideka_first_person = true;
   float droideka_eye_height = 0.8f;   // metres over the droideka's origin
   float droideka_eye_forward = 0.0f;  // metres along its heading
   float droideka_hide_radius = 1.0f;  // its own draws this close to its origin aren't drawn
   bool gesture_throw = true; // grenades: hold the left trigger, swing, let go

   // Quick reload: a bar of the reload's progress with a blue window; reload pressed
   // again inside it finishes the reload at once (once per reload). Shown under the
   // crosshair and / or on the left wrist; off in both places = no minigame.
   bool reload_bar_crosshair = true;
   bool reload_bar_wrist = false; // the right wrist

   // Guns that overheat (no reload: a clone trooper's pistol) cool this many times as
   // fast while shaken hard; 1 = only as the game cools them.
   float shake_cooling = 4.0f;

   // A light vibration in the gun hand while an overheating gun cools down, stronger while
   // it's shaken (shake_cooling).
   bool cooling_haptics = true;

   // The game's crosshair: this far along the shots' line from where they start (the
   // gun's muzzle, a turret's barrel), metres.
   float crosshair_distance = 8.0f;
   bool crosshair_shown = true;  // off: no crosshair at all
   bool crosshair_steady = true; // the gun's recoil, reload and steps don't sway it

   // Saber swings: the blade's tip (relative to the head) faster than this starts a
   // swing once it has moved this far that fast (a flick of the wrist doesn't).
   float saber_swing_speed = 6.0f;     // m/s
   float saber_swing_distance = 0.15f; // metres

   // Saber swing sound (the game's own "saber_swing", at the blade): swings with the tip
   // slower than saber_sound_speed are silent, faster ones louder up to
   // saber_sound_volume (of the game's loudness); none sooner than saber_sound_interval
   // after the last, and a quick run of swings gets quieter.
   bool saber_sound = true;
   float saber_sound_volume = 0.8f;   // 0..1
   float saber_sound_speed = 7.0f;    // m/s
   float saber_sound_interval = 0.3f; // seconds

   // Saber block: a bolt that hits near the blade is deflected without holding the grips
   // (both grips with the blade upright still block everything in front).
   bool saber_auto_block = true;

   // Hand swap: the free hand brought to the weapon hand (both vibrate) with its grip
   // pressed takes the weapon. Keeping the rotation, the weapon stays turned as it was
   // relative to the new hand (a double click of that hand's grip turns it back to the
   // default); else it takes the default rotation.
   bool hand_swap = true;
   bool hand_swap_keeps_rotation = true;

   // Saber rotation: with its hand's grip held the saber keeps its rotation (it still
   // moves with the hand); let go, the hand's turn since is kept as the saber's offset.
   // While held, a ghost arrow shows where the hand points.
   bool draw_hand_arrow = true;

   // Weapon bash: a fast jab or swing of the gun's front (faster than this, relative to
   // the head) hits a soldier it passes near: this much damage and push (the game's own,
   // as the saber's; a saber held by the target may block it).
   bool weapon_bash = true;
   float bash_speed = 2.0f; // m/s
   float bash_damage = 300.0f;
   float bash_push = 4.0f;

   // Debug cheats: the hero is in the class select on maps that have one, unlocked or not.
   bool hero_always_available = false;
   bool special_units_always_available = false; // no points needed to unlock a class

   float render_scale_percent = 85.0f; // applies after a restart
};

auto settings() noexcept -> Settings&;

// Reads bf2vr_settings.txt (once; later calls do nothing). Also takes the render scale
// from the older bf2vr_render_scale.txt if the settings file has none.
void load_settings() noexcept;

// Writes bf2vr_settings.txt.
void save_settings() noexcept;

// Per first-person model corrections (bf2vr_arms.txt): where each hand holds the model,
// as offsets in the game camera's view space (metres, x right, y up, -z forward) added
// to the automatic grip points. A model is known by its parts' sizes.
// Some models are only modelled on the side the flat game shows: `mirror` also draws the
// weapon (the model's largest part) reflected across its own plane through its origin
// whose normal is the part's `mirror_axis` (0 x, 1 y, 2 z), scaled by `mirror_scale`
// about its origin and moved by `mirror_shift` (metres along the part's x, y, z), to
// cover the missing side (a slightly smaller copy doesn't fight the original's surfaces).
// `right_parts` / `left_parts` pick the parts each hand holds, any combination: bit i
// for the i-th biggest part (bit 0 the weapon; vr_system.cpp hand_parts_have, the
// panel's checkboxes), -1 = the defaults.
// `muzzle_offset` moves where the shots start (metres in the gun's frame: x right,
// y up, -z forward, as the gun is held) from the barrel tip found in the model (or the
// game's fire point when it can't be found).
// The left hand's grip offset a model starts with: 0.1 m right (the clone rifles were
// tuned to +0.05..+0.11 by hand).
inline constexpr glm::vec3 default_left_offset{0.1f, 0.0f, 0.0f};

struct Arm_tuning {
   glm::vec3 right_offset{};
   glm::vec3 left_offset = default_left_offset;
   bool mirror = false;
   int mirror_axis = 0;
   glm::vec3 mirror_shift{};
   float mirror_scale = 0.98f;
   int right_parts = -1;
   int left_parts = -1;
   glm::vec3 muzzle_offset{};
};

auto arm_tuning(const std::string& model) noexcept -> Arm_tuning&;

void save_arm_tuning() noexcept;

// The VR panel: what it shows besides the settings, and what it asks for.
struct Panel_status {
   std::string model;           // the current first-person model, empty without one
   int model_parts = 0;         // its parts, and the parts each hand holds (bit masks)
   std::uint32_t right_hand_parts = 0;
   std::uint32_t left_hand_parts = 0;
   std::string bindings;        // the game keys the controls press
   std::string body;            // the measured eye heights, empty while not on foot
   std::string measure;         // Measure height's countdown or result, empty before
   float scope_zoom = 0.0f;
   float fps = 0.0f;        // frames the game sends to the headset per second
   float refresh_hz = 0.0f; // the headset's refresh rate, 0 while unknown
};

struct Panel_requests {
   bool recenter = false;
   bool close = false;
   bool measure = false; // Measure height pressed
};

// The panel's two windows side by side in its texture: the settings, then (after a
// gap left transparent) the hands'.
constexpr std::uint32_t panel_window_px = 900;
constexpr std::uint32_t panel_gap_px = 60;
constexpr std::uint32_t panel_texture_width_px = panel_window_px * 2 + panel_gap_px;

// Draws the panel into `rtv` (size `size`) with the pointer at `pointer` (0..1 from the
// top left, or negative when it's off the panel). Leaves the D3D state as it found it.
auto draw_panel(ID3D11Device& device, ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                const glm::uvec2 size, const glm::vec2 pointer, const bool click,
                const float dt, const Panel_status& status) noexcept -> Panel_requests;

}
