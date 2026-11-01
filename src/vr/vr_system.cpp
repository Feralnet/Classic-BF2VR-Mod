
#include "vr_system.hpp"
#include "vr_hud_render.hpp"
#include "vr_melee.hpp"
#include "vr_post_render.hpp"
#include "vr_saber_render.hpp"
#include "vr_settings.hpp"
#include "vr_sound.hpp"
#include "../logger.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <intrin.h>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <DbgHelp.h>

#include <glm/gtc/quaternion.hpp>

#pragma comment(lib, "dbghelp.lib")

namespace sp::vr {

namespace {

// Distance and width of the virtual screen (menus, loading), in metres.
constexpr float screen_distance = 2.0f;
constexpr float screen_width = 2.4f;

// Fraction of the runtime's recommended per-eye resolution to render at (settings).
constexpr float min_render_scale = 0.4f;
constexpr float max_render_scale = 1.5f;

// The scope's view: a square column right of the eyes, this fraction of their height,
// shown on a window over the right hand. Its field of view at 1x zoom.
constexpr float scope_column_fraction = 0.5f;
constexpr float scope_base_half_fov_degrees = 30.0f;

// The scope comes up by itself while the gun is held to the eye: the aim ray passes
// this close to the eyes, within this angle of the head's forward and this near.
constexpr float scope_raise_ray_distance = 0.09f;
constexpr float scope_raise_degrees = 20.0f;
constexpr float scope_raise_hand_distance = 0.4f;
constexpr float scope_lower_ray_distance = 0.13f; // and goes away beyond these
constexpr float scope_lower_degrees = 30.0f;

// The scope window over the right hand: in front of the aim pose (metres); its height
// above it is a setting.
constexpr float scope_window_forward = 0.06f;

// The VR panel (settings, and the hands' window right of it: panel_texture_width_px):
// its texture's height, and one window's width in the world.
constexpr std::uint32_t panel_height_px = 1100;
constexpr float panel_width = 0.55f;
constexpr float panel_distance = 0.7f; // in front of the head, opened in a match

// The scope button pressed again within this long after a press: the next zoom.
constexpr float scope_double_click_seconds = 0.35f;

// The weapon bash's point on the gun: this far along the barrel from the aim pose.
constexpr float bash_gun_front = 0.3f;

// The panel's Measure height takes the head's height this long after the press (the
// player stands up straight and looks ahead meanwhile).
constexpr float measure_countdown_seconds = 3.0f;

// Buttons held at least this long do their second action (A: lock target, Y: panel).
constexpr float long_press_seconds = 0.5f;

// Game keys tapped (reload on a short A press, lock target) stay down this many frames,
// so the game's once-a-frame read sees them.
constexpr std::uint32_t key_tap_frames = 4;

// Grenade throw: the left hand's speed over the last samples (m/s) above which the
// swing's direction is the throw's, and how long the game camera may take to turn to
// it before the grenade goes anyway.
constexpr float throw_min_speed = 1.2f;
constexpr float throw_velocity_window = 0.06f; // seconds
constexpr float throw_aim_tolerance_degrees = 3.0f;
constexpr std::uint32_t throw_max_wait_frames = 20;
constexpr std::uint32_t throw_hold_frames = 8; // the camera stays on the throw after

// Secondary fire is held through the game's wind-up of the throw (cut short, see
// melee::hurry_grenade_throw), at most this long: the longest wind-up the game has.
constexpr float throw_max_hold_seconds = 0.9f;

// Hand swap: the hands' grip poses this close are together (apart again past the second
// distance), and buzz lightly while they are.
constexpr float hands_together_distance = 0.13f; // metres
constexpr float hands_apart_distance = 0.17f;
constexpr float hands_near_distance = 0.35f; // near: a double click of the grip swaps
constexpr float hands_together_amplitude = 0.12f;
constexpr float hands_together_buzz_seconds = 0.08f;

// The ghost arrow (settings draw_hand_arrow) while the saber's rotation is held.
constexpr float hand_arrow_length = 0.35f; // metres in the room

// Head culling culls from the eyes when they're at most this far from the game's camera
// (metres; further is taken as a bad anchor and left to the camera).
constexpr float cull_eyes_max_offset = 2.0f;
// ... and only for a body scaled under this (a small character: Yoda, a Jawa, an Ewok).
// Anyone else's eyes are within centimetres of the game's camera, which culls as it
// always did (moving it there made buildings such as Endor's health droids flicker).
constexpr float cull_eyes_max_scale = 0.85f;

// Edge margin by head speed: extra degrees per degree per second turned, up to a cap,
// easing back over this many seconds.
constexpr float overscan_per_speed = 0.035f;
constexpr float overscan_max_degrees = 12.0f;
constexpr float overscan_release_seconds = 0.3f;

// Comfort vignette: clear within / dark from these angles off each eye's forward.
constexpr float vignette_inner_degrees = 30.0f;
constexpr float vignette_outer_degrees = 52.0f;

// Melee characters: either trigger uses the ability, aimed by that trigger's hand while
// it's held and this long after; otherwise the head aims (the soldier faces where the
// player looks: the game's deflect only sends back bolts from in front of the soldier).
// When the aiming hand changes, the press waits this many frames for the camera to turn.
constexpr int melee_aim_settle_frames = 4;
constexpr float melee_aim_linger_seconds = 0.5f;

// Game camera chases the head: mouse counts per radian of error, and a per-frame cap.
constexpr float mouse_follow_gain = 250.0f;
constexpr float mouse_follow_max_counts = 40.0f;

// A turret answers the mouse with a lag: at the full gain it swung round the aim. Gentler
// there, and still within this of it.
constexpr float turret_follow_gain = 60.0f;
constexpr float turret_follow_max_counts = 8.0f;
constexpr float turret_follow_dead_zone = 0.006f; // radians

// Menu cursor: DirectInput counts per pixel of the game window (unverified: 1:1 assumed).
constexpr float menu_counts_per_pixel = 1.0f;

// Frames without the first-person weapon pass before the world is treated as a menu
// (unit select). Hysteresis for passes the game skips briefly.
constexpr std::uint32_t menu_after_frames_without_weapon = 30;

// Frames an interface screen or a view-space-only frame must last before gameplay is
// left for the virtual screen (a single odd frame mustn't flip the view).
constexpr std::uint32_t menu_after_frames_with_screen = 3;

// Times "the weapon pass wasn't recognised" is logged per run.
constexpr std::uint32_t max_logged_weapon_gaps = 20;

// Vehicles (on the virtual screen): the right stick steers with mouse counts (as the
// mouse's position or speed: settings; eased in: deflection squared), from this
// deflection.
constexpr float vehicle_steer_dead_zone = 0.15f;

// Quick reload (see update_quick_reload): the blue window starts at a random point in
// this part of the reload (fractions), and lasts this long (seconds, kept within these
// fractions of the reload). After a hit the bar shows full for a moment.
constexpr float quick_reload_earliest = 0.3f;
constexpr float quick_reload_latest = 0.75f;
constexpr float quick_reload_window_seconds = 0.3f;
constexpr float quick_reload_min_window = 0.1f;
constexpr float quick_reload_max_window = 0.2f;
constexpr float quick_reload_flash_seconds = 0.35f;

// Quick reload's vibration (right hand): a light buzz while the marker is in the window
// (pulses this long, back to back), a light tap for a hit, two hard low knocks (this
// far apart) for a miss.
constexpr float quick_reload_buzz_amplitude = 0.12f;
constexpr float quick_reload_buzz_interval = 0.05f;
constexpr float quick_reload_hit_amplitude = 0.35f;
constexpr float quick_reload_hit_seconds = 0.04f;
constexpr float quick_reload_miss_amplitude = 1.0f;
constexpr float quick_reload_miss_seconds = 0.1f;
constexpr float quick_reload_miss_frequency = 40.0f; // Hz, where the runtime takes one
constexpr float quick_reload_miss_gap = 0.16f;

// Left stick -> movement keys (the game's movement is digital on the keyboard): pushed
// this far, in eight directions (a key is held while the stick's direction is within
// 67.5 degrees of it).
constexpr float stick_key_threshold = 0.5f;
constexpr float stick_key_direction = 0.383f; // sin 22.5 degrees

// Head sway off: the eyes' height follows the game camera's through a tracking filter
// (no lag on slopes and stairs) that leaves out the steps' bobbing (2-3 Hz), never more
// than this far off the camera's.
constexpr float head_sway_filter_hz = 0.25f;
constexpr float head_sway_filter_damping = 0.7f;
constexpr float head_sway_max_offset = 0.15f;

// Body height (settings body_scale): the eyes are as high over the soldier's feet, as a
// fraction of the character's standing eye height, as the player's are over the floor
// as a fraction of their standing eye height (crouching to half height in the room puts
// the eyes at half the character's height). The character's standing eye height is the
// game camera's over the feet (or a melee character's head bone) while it aims within
// about 6 degrees of level and the soldier stands still: the median of
// body_lock_samples, then kept for the life (the camera bobs a few cm, which moved the
// view). Other states' heights (the game's crouch) are averaged over
// body_measure_seconds. Changes of the character's height ease over body_ease_seconds;
// the eyes never go lower than min_eye_above_feet.
constexpr int body_states = 9;
constexpr float body_level_aim_sine = 0.1f;
constexpr float body_measure_seconds = 0.3f;
constexpr float body_ease_seconds = 0.25f;
constexpr float body_default_eye_height = 1.6f; // until the character is measured
constexpr int body_lock_samples = 45;
constexpr float body_max_camera_distance = 3.0f; // the soldier's farther: not the player's body
constexpr float min_eye_above_feet = 0.15f;
constexpr float min_body_fraction = 0.1f;
constexpr float max_body_fraction = 1.15f;

// Crouched with the game's button while standing in the room: the eyes drop by the
// character's crouch (standing minus crouched height, measured, or this fraction of the
// standing one), the head's own motion kept on top. Crouching in the room as well never
// takes them under body_crouch_lowest_fraction of the standing height (not into the
// floor). A press of the button is watched for crouch_answer_seconds: the soldier state
// it leads to from standing is taken as the crouch's.
constexpr float body_crouch_default_fraction = 0.62f;
constexpr float body_crouch_lowest_fraction = 0.35f;

// Physical crouch (settings physical_crouch): the soldier crouches once the player's
// head is under crouch_enter_fraction of their standing height and stands up over
// crouch_leave_fraction (a tap of the game's crouch, a toggle). The soldier state the
// first tap leads to is taken as the crouch's (1 until then); a tap that changes nothing
// within crouch_answer_seconds is a miss, and after crouch_max_misses it's given up.
constexpr float crouch_enter_fraction = 0.72f;
constexpr float crouch_leave_fraction = 0.82f;
constexpr float crouch_answer_seconds = 1.0f;
constexpr int crouch_max_misses = 2; // even: a toggle the state missed ends as it was
// The player's motion scaled by the character's eye height over theirs, within these:
// a Jawa or Yoda makes the world big around a small player, Grievous or a Wampa small
// around a big one. (A seated player once felt a head taller than everyone: their eye
// height had been taken sitting; it's measured standing now.)
constexpr float min_body_scale = 0.3f;
constexpr float max_body_scale = 1.5f;

// A character's eyes over its feet: its head bone's height (bone_head sits at the
// skull's base) times this. Calibrated on a clone trooper (head bone 1.50 m) to the
// game's own first-person camera height, 1.72 m: a standard soldier is where the flat
// game puts the eyes, and every other character in proportion (Yoda ~0.7 m, Grievous
// ~1.9 m). The game's camera is as high for every character; the skeleton isn't.
constexpr float head_bone_eye_factor = 1.72f / 1.50f;

// The player's eye height over the floor rises to a head held higher for about this
// long (the first one may have been taken sitting or with the headset off); it never
// drops by itself (crouching), only at the panel's Recenter.
constexpr float player_eye_rise_seconds = 1.0f;
constexpr float player_eye_max_height = 2.3f;

// While the left stick moves the soldier, the head's offset in the room (leaning,
// stepping) eases back over the soldier: the game's body (what enemies shoot at and
// collide with) stays under the eyes. Standing still it's left alone: that would move
// the view by itself.
constexpr float body_follow_seconds = 0.6f;

// Controller smoothing (settings hand_smoothing, s 0..1): a One Euro filter per hand.
// Its cutoff is hand_filter_min_hz * hand_filter_range^s at rest and rises with the
// hand's speed (beta per m/s and per rad/s): held still, the tracking's jitter and the
// hand's tremor are steadied; a swing passes with little lag. The speeds themselves are
// filtered at hand_filter_speed_hz.
constexpr float hand_filter_min_hz = 12.0f;
constexpr float hand_filter_range = 0.03f; // s = 1: 0.36 Hz at rest
constexpr float hand_filter_beta_position = 30.0f;
constexpr float hand_filter_beta_rotation = 8.0f;
constexpr float hand_filter_speed_hz = 1.0f;
constexpr float hand_filter_max_gap = 0.1f; // seconds; a longer one restarts the filter

// Shots from the gun in the hand (fire_from_hand): the game's fire point is on its
// first-person gun, this close to the camera at most.
constexpr float fire_point_max_distance = 1.5f;

// The shot line's length (settings show_shot_line), metres.
constexpr float shot_line_length = 3.0f;

// Weapon sway off: the first-person model's pose relative to the camera is eased over
// this many seconds; a jump bigger than these (weapon change) is taken at once.
constexpr float weapon_sway_seconds = 0.4f;
constexpr float weapon_sway_snap_distance = 0.15f;
constexpr float weapon_sway_snap_degrees = 30.0f;

// Stationary turrets: the game's camera circles the turret behind it; the eyes sit at
// the point it circles (where its view lines meet, least squares) once the lines have
// turned enough to tell it, at most this far from the camera. The camera's positions are
// exact (it circles the point rigidly), so a turn of a degree or two tells it; the point
// keeps being refined as the turret turns, and the eyes ease to the refined one.
constexpr std::uint32_t turret_min_lines = 12;
constexpr float turret_min_spread = 0.001f; // determinant of the lines' mean projector
constexpr float turret_pivot_ease_rate = 6.0f; // per second
constexpr float turret_max_pivot_distance = 15.0f;

// The HUD in front of the eyes (head-locked), at this distance; the crosshair's piece of
// it goes along the gun instead.
constexpr float hud_overlay_distance = 1.0f;

// Steady crosshair (settings crosshair_steady): the gun's model counts as kicked by
// recoil this long after a shot.
constexpr float crosshair_recoil_seconds = 0.5f;
constexpr int crosshair_calm_frames = 20;
// ... and the game camera no further than this from its aim (it catches up with the gun
// after a snap turn: the game sways its model meanwhile).
constexpr float crosshair_calm_camera_radians = 0.05f;

// Shaking an overheated gun (settings shake_cooling): strokes of the gun hand faster
// than this, each turning back from the last one by more than ~120 deg, raise the shake
// (by one step each, full after three), which fades over shake_fade_seconds.
constexpr float shake_stroke_speed = 0.8f;
constexpr float shake_turn_back_cos = -0.5f;
constexpr float shake_step = 0.35f;
constexpr float shake_fade_seconds = 0.6f;
constexpr float shake_velocity_seconds = 0.03f;

// Vehicle steering brake (vehicle_brake_counts): at most this long after letting go,
// until the turn is slower than the stop rate (rad/s); the share of the turn countered
// per second of mouse counts.
constexpr float vehicle_brake_seconds = 2.0f;
constexpr float vehicle_brake_stop_rate = 0.05f;
constexpr float vehicle_brake_gain = 0.6f;

// Skinned draws remembered on foot for telling a turret's own from its gunner's.
constexpr std::uint64_t turret_seen_frames = 300;
constexpr float turret_seen_distance = 6.0f;
constexpr float turret_seen_match = 0.5f;
constexpr float turret_gunner_radius = 3.0f;
constexpr std::uint64_t turret_gunner_frames = 45;
constexpr std::size_t turret_seen_max = 1024;
constexpr float player_feet_radius = 0.3f;
constexpr std::size_t player_body_max = 32;

// Far scene draws whose object is this close to the game camera move with it (the sky
// dome): seen from the camera. The rest (distant buildings) stays put in the world: on a
// turret seen from where the eyes are, not from the camera circling it.
constexpr float far_scene_camera_centred = 2.0f;

// Two-handed hold: the hands must be at least this far apart (metres) to steer the gun.
constexpr float two_hand_min_distance = 0.1f;

// Right stick turning (snap by default: smooth turning is a setting). The stick's
// deflection that turns, and the one below which it's released.
constexpr float snap_turn_trigger = 0.7f;
constexpr float snap_turn_release = 0.3f;
constexpr float smooth_turn_dead_zone = 0.2f;

// A game camera moving further than this between frames was cut to (spawn, map or
// menu change): recentre the head on it instead of swinging the camera round.
constexpr float camera_cut_distance = 5.0f;

// Tolerances when recognising a perspective camera in the game's view-projection.
constexpr float camera_epsilon = 0.02f;

// Stun / knockdown: the held buzz's interval (seconds).
constexpr float stun_buzz_interval = 0.12f;

// Saber block: a light buzz in both hands while blocking (re-applied every interval, each
// lasting longer than that: continuous), a strong one for each deflected bolt.
constexpr float block_buzz_amplitude = 0.12f;
constexpr float block_buzz_interval = 0.05f;
constexpr float block_buzz_seconds = 0.1f;
constexpr float block_deflect_amplitude = 0.85f;
constexpr float block_deflect_seconds = 0.12f;

#define SP_XR_INSTANCE_FUNCTIONS(X)                                            \
   X(xrDestroyInstance)                                                        \
   X(xrGetInstanceProperties)                                                  \
   X(xrGetSystem)                                                              \
   X(xrGetD3D11GraphicsRequirementsKHR)                                        \
   X(xrEnumerateViewConfigurationViews)                                        \
   X(xrCreateSession)                                                          \
   X(xrDestroySession)                                                         \
   X(xrPollEvent)                                                              \
   X(xrBeginSession)                                                           \
   X(xrEndSession)                                                             \
   X(xrWaitFrame)                                                              \
   X(xrBeginFrame)                                                             \
   X(xrEndFrame)                                                               \
   X(xrLocateViews)                                                            \
   X(xrCreateReferenceSpace)                                                   \
   X(xrEnumerateSwapchainFormats)                                              \
   X(xrCreateSwapchain)                                                        \
   X(xrDestroySwapchain)                                                       \
   X(xrEnumerateSwapchainImages)                                               \
   X(xrAcquireSwapchainImage)                                                  \
   X(xrWaitSwapchainImage)                                                     \
   X(xrReleaseSwapchainImage)                                                  \
   X(xrStringToPath)                                                           \
   X(xrPathToString)                                                           \
   X(xrGetCurrentInteractionProfile)                                           \
   X(xrCreateActionSet)                                                        \
   X(xrCreateAction)                                                           \
   X(xrSuggestInteractionProfileBindings)                                      \
   X(xrAttachSessionActionSets)                                                \
   X(xrSyncActions)                                                            \
   X(xrGetActionStateBoolean)                                                  \
   X(xrGetActionStateVector2f)                                                 \
   X(xrGetActionStatePose)                                                     \
   X(xrApplyHapticFeedback)                                                    \
   X(xrCreateActionSpace)                                                      \
   X(xrLocateSpace)

struct Xr_functions {
   PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
   PFN_xrCreateInstance xrCreateInstance = nullptr;
   PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;

#define SP_XR_DECLARE(name) PFN_##name name = nullptr;
   SP_XR_INSTANCE_FUNCTIONS(SP_XR_DECLARE)
#undef SP_XR_DECLARE
};

// A perspective camera recovered from the game's view-projection matrix.
struct Game_camera {
   glm::vec3 position;
   glm::vec3 right;
   glm::vec3 up;
   glm::vec3 forward;
   float depth_scale;  // projection z row: z_clip = depth_scale * z_view + depth_offset
   float depth_offset;
   float handedness;   // sign of dot(cross(right, up), forward)
   float tan_half_fov_x; // from the projection's x scale (symmetric projection)
};

// An engine camera's rotation: world matrix rows right, up, back.
struct Camera_rows {
   glm::vec3 right;
   glm::vec3 up;
   glm::vec3 back;
};

// Distance along the view axis where depth is 0: z_ndc = A + B / d, 0 at d = -B/A.
auto near_plane(const Game_camera& camera) noexcept -> float
{
   return -camera.depth_offset / camera.depth_scale;
}

// The camera with its near plane moved to `near_distance` (far plane kept).
auto with_near_plane(Game_camera camera, const float near_distance) noexcept -> Game_camera
{
   const float far_distance = camera.depth_offset / (1.0f - camera.depth_scale);

   camera.depth_scale = far_distance / (far_distance - near_distance);
   camera.depth_offset = -near_distance * far_distance / (far_distance - near_distance);

   return camera;
}

// A smaller body (body scale under 1: Yoda) sees the world closer up: the near plane
// comes as much nearer, or the world is cut off at a distance that looks too far.
auto body_near_plane(const Game_camera& camera, const float body_scale) noexcept -> Game_camera
{
   return body_scale < 0.99f ? with_near_plane(camera, near_plane(camera) * body_scale) : camera;
}

// The first-person model in the hand comes much closer to the eyes than the game's
// weapon pass near plane (0.3 m) allows: it gets a nearer one, and its draws don't clip
// at the near plane at all (see Eye_draw::in_hand).
constexpr float in_hand_near_plane = 0.01f;

// True for a camera at the origin looking down -Z: the draw's vertices are already in
// the game camera's view space. That's the HUD (crosshair, markers; hfov 60, near 1),
// not the weapon: the first-person weapon is drawn in world space with the world
// camera's pose and a nearer near plane (0.3 vs 0.5).
bool is_view_space_camera(const Game_camera& camera) noexcept
{
   return glm::length(camera.position) < 0.01f &&
          glm::distance(camera.right, glm::vec3{1.0f, 0.0f, 0.0f}) < 0.01f &&
          glm::distance(camera.up, glm::vec3{0.0f, 1.0f, 0.0f}) < 0.01f &&
          glm::distance(camera.forward, glm::vec3{0.0f, 0.0f, -1.0f}) < 0.01f;
}

// Why a view-projection wasn't taken for a perspective camera (for diagnostics).
enum class Reject { none, no_w_row, degenerate, not_orthogonal };

// One object of the first-person model (arms, gun, ...) in this frame's near scene.
struct Model_part {
   glm::vec3 origin_view; // object origin in the model's frame (see Weapon_frame)
   std::uint32_t draws;
   std::uint32_t elements;
   glm::vec3 origin_other; // the same in the other candidate frame
};

// The frame the game keeps the first-person model in front of: the camera's own
// rotation (the aim) or the rotation the camera is drawn with (the head's). Whichever
// keeps the gun's origin still while head and aim move apart is it; checked as the
// game runs.
enum class Weapon_frame { aim, render };

// Frames per check of the weapon's frame, and how much stiller the other frame must
// keep the gun (summed movement, metres) to switch to it.
constexpr std::uint32_t weapon_frame_check_frames = 90;
constexpr float weapon_frame_switch_ratio = 0.5f;
constexpr float weapon_frame_min_movement = 0.05f;

// Near scene draws whose object origin is this close to the camera are the
// first-person model; nearby world objects have their origins further away.
constexpr float first_person_model_radius = 1.0f;

// A near scene draw this close to where the player's saber hilt was put (metres) is the
// saber.
constexpr float saber_hilt_match = 0.25f;
constexpr std::size_t max_model_parts = 8;

// Frames a new part count of the first-person model must last to be taken.
constexpr int part_count_change_min_frames = 30;

// ... and a new name (its parts' sizes, same count).
constexpr int signature_change_min_frames = 45;

// A draw of the weapon part shows the muzzle flash only this soon after a shot; the
// gun's own draws are learnt further from shots (muzzle_flash_draw).
constexpr float flash_after_shot_seconds = 0.4f;

// The barrel tip (finish_gun_tip): the vertices this close (metres) behind the frontmost
// one; and how many frames a model's read-back is tried.
constexpr float gun_tip_depth = 0.015f;
constexpr int gun_tip_max_tries = 5;

// The parts a hand holds: bit `rank` set for each part (ranked by size, rank 0 = the
// gun), any combination.
bool hand_parts_have(const std::uint32_t parts, const int rank) noexcept
{
   return rank >= 0 && rank < 32 && ((parts >> rank) & 1u) != 0;
}

// The part whose origin is the hand's grip point (its biggest part), or -1.
auto hand_parts_grip_rank(const std::uint32_t parts, const int n) noexcept -> int
{
   // No parts ranked yet (first weapon frame after a load): no grip; indexing the
   // empty ranked_parts here crashed on entering a match.
   const std::uint32_t held = n >= 32 ? parts : parts & ((1u << n) - 1u);

   return held == 0 ? -1 : std::countr_zero(held);
}
constexpr std::uint32_t max_logged_weapon_draws = 40;

// A part is matched to last frame's part with the nearest origin within this (parts
// sway a little between frames; they sit 10+ cm from each other).
constexpr float part_match_tolerance = 0.05f;

// A hand aim pose (the right one by default) in the game world, anchored like the eyes
// at the world camera.
struct World_pose {
   glm::vec3 position;
   glm::mat3 rotation; // columns: hand right, up, back (OpenXR axes) in world space
};

struct Eye_view {
   glm::vec3 position; // world space
   glm::vec3 right;
   glm::vec3 up;
   glm::vec3 forward;
   std::array<float, 4> tangents; // left, right, up, down
};

struct State {
   Xr_functions xr;

   XrInstance instance = XR_NULL_HANDLE;
   XrSystemId system = XR_NULL_SYSTEM_ID;
   XrSession session = XR_NULL_HANDLE;
   XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
   bool session_running = false;

   glm::uvec2 recommended_eye_size{0, 0};
   glm::uvec2 eye_size{0, 0};

   XrSpace local_space = XR_NULL_HANDLE;
   XrSpace stage_space = XR_NULL_HANDLE; // the floor's height; null without one

   XrSwapchain swapchain = XR_NULL_HANDLE;
   std::vector<ID3D11Texture2D*> swapchain_images;
   UINT swapchain_width = 0;
   UINT swapchain_height = 0;
   DXGI_FORMAT swapchain_format = DXGI_FORMAT_UNKNOWN;

   XrActionSet action_set = XR_NULL_HANDLE;
   XrAction fire_action = XR_NULL_HANDLE;
   XrAction aim_action = XR_NULL_HANDLE;      // right hand aim pose
   XrAction left_aim_action = XR_NULL_HANDLE; // left hand aim pose
   XrAction reload_action = XR_NULL_HANDLE;         // A
   XrAction jump_action = XR_NULL_HANDLE;           // B
   XrAction class_menu_action = XR_NULL_HANDLE;     // X
   XrAction pause_action = XR_NULL_HANDLE;          // Y
   XrAction secondary_fire_action = XR_NULL_HANDLE; // left trigger
   XrAction two_hand_action = XR_NULL_HANDLE;       // left grip
   XrAction block_action = XR_NULL_HANDLE;          // right grip (saber block)
   XrAction grip_action = XR_NULL_HANDLE;           // right hand grip pose (the saber)
   XrAction left_grip_action = XR_NULL_HANDLE;      // left hand grip pose (a second saber)
   XrAction haptic_action = XR_NULL_HANDLE;         // both hands' vibration
   XrAction sprint_action = XR_NULL_HANDLE;         // left stick click
   XrAction crouch_action = XR_NULL_HANDLE;         // right stick click (+ strafe: roll)
   XrAction squad_action = XR_NULL_HANDLE;          // squad command
   XrAction lock_action = XR_NULL_HANDLE;           // lock target (Touch: A held)
   XrAction left_pad_click_action = XR_NULL_HANDLE;  // Vive, WMR: the pads are clicked in
   XrAction right_pad_click_action = XR_NULL_HANDLE; // directions (see update_input)
   XrAction left_pad_action = XR_NULL_HANDLE;        // where the pads are touched
   XrAction right_pad_action = XR_NULL_HANDLE;
   // The right hand's controller has no stick: its pad is the stick (Vive wands), from
   // the runtime's current interaction profile.
   bool right_stick_is_pad = false;
   bool hp_controller_extension = false;   // XR_EXT_hp_mixed_reality_controller
   bool pico_controller_extension = false; // XR_BD_controller_interaction
   XrSpace aim_space = XR_NULL_HANDLE;
   XrSpace left_aim_space = XR_NULL_HANDLE;
   XrSpace grip_space = XR_NULL_HANDLE;
   XrSpace left_grip_space = XR_NULL_HANDLE;
   XrPath hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH}; // left, right

   // Left grip held with both hands tracked: the gun's grip stays in the right hand and
   // its barrel points at the left hand, rolled like the right hand. Tracking space,
   // columns right, up, back.
   bool two_handed = false;
   glm::mat3 weapon_rotation{1.0f};

   // Hand aim poses (LOCAL space) for the frame being rendered.
   bool aim_valid = false;
   XrPosef aim_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
   bool left_aim_valid = false;
   XrPosef left_aim_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
   // Right hand grip pose: -Z runs through the fist from the little finger to the thumb,
   // the way a held hilt points.
   bool grip_valid = false;
   XrPosef grip_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
   bool left_grip_valid = false; // the same for the left hand
   XrPosef left_grip_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

   // Melee characters: the hand aiming the ability (-1 none: the head; 0 left, 1 right),
   // seconds it keeps aiming after its trigger, frames left before the press goes through.
   int melee_aim_hand = -1;
   float melee_aim_linger = 0.0f;
   int melee_aim_settle = 0;

   // Steady crosshair: the shots' line in the frame of the hand holding the gun, taken
   // while the gun is calm (its recoil, reload, steps and the game camera turning after
   // the gun move the model, not the hand): no shot, no reload, not walking, the game
   // camera on its aim, for crosshair_calm_frames frames; per gun model. The shots take
   // it too (fire_from_hand), so they go where the crosshair is.
   int crosshair_calm = 0;
   float camera_aim_error = 0.0f; // radians, the larger of yaw and pitch (mouse-follow)
   std::string crosshair_line_model;
   bool crosshair_line_valid = false;
   glm::vec3 crosshair_line_origin{};
   glm::vec3 crosshair_line_direction{};

   // Controller smoothing (smooth_hands): the right grip's and the left aim's filters.
   struct Pose_filter {
      bool valid = false;
      glm::vec3 position{};
      glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
      float speed = 0.0f;         // m/s, filtered
      float angular_speed = 0.0f; // rad/s, filtered
   };

   Pose_filter right_filter;
   Pose_filter left_filter;
   glm::vec3 raw_aim_position{}; // the right aim pose's position before smoothing

   // Which first-person model parts each hand holds (see hand_parts_have); set to the
   // defaults whenever the model's part count changes (see default_hand_parts). A hand's
   // grip point is the origin of its biggest part (the right hand's comes from
   // bf2vr_weapon.txt if set).
   std::uint32_t right_hand_parts = 1;
   std::uint32_t left_hand_parts = 0;
   int part_count_change_frames = 0; // frames the model's part count has differed
   int signature_change_frames = 0;  // ... and its name (parts' sizes)

   // The weapon part's own draws' sizes (muzzle_flash_draw), for the model named.
   // Where the gun's muzzle flash was drawn this frame (flash_from_hand; the game's
   // first-person frame), told apart from the model's parts by it.
   glm::vec3 flash_origin{};
   std::uint64_t flash_origin_frame = ~0ull;

   std::string gun_draw_sizes_model;
   std::array<std::uint32_t, 16> gun_draw_sizes{};
   std::size_t gun_draw_size_count = 0;
   bool flash_logged = false;

   // Each weapon model's barrel tip and muzzle flash centre in its weapon part's object
   // space, from the parts' geometry (give_model_positions).
   struct Gun_tip {
      bool tip_known = false;
      glm::vec3 tip_object{};
      bool flash_known = false;
      glm::vec3 flash_object{};
      int tries = 0;
      bool failed = false;
   };

   std::map<std::string, Gun_tip> gun_tips;

   // A read-back asked for (take_model_positions_request): of which, and the draw's
   // object matrix and camera then.
   bool positions_request = false;
   bool positions_for_flash = false;
   glm::mat3 positions_rotation{1.0f};
   glm::vec3 positions_position{};

   // The weapon part's draws read back in one frame, in the weapon camera's view space,
   // finished into a tip after the frame (finish_gun_tip).
   std::string tip_capture_model;
   std::uint64_t tip_capture_frame = 0;
   std::vector<glm::vec3> tip_capture_view;
   Game_camera tip_capture_camera{};
   glm::mat3 tip_capture_rotation{1.0f};
   glm::vec3 tip_capture_position{};

   // The weapon part's object matrix as last drawn (where the tip is now).
   std::uint64_t gun_object_frame = 0;
   glm::mat3 gun_object_rotation{1.0f};
   glm::vec3 gun_object_position{};
   bool weapon_grip_from_file = false;

   // The HUD layer this frame (see hud_to_layer): the HUD camera's view-projection.
   bool hud_layer_vp_valid = false;
   std::array<glm::vec4, 4> hud_layer_vp{};
   std::uint32_t hud_layer_draws = 0;
   bool hud_layer_vp_mismatch_logged = false;
   bool hud_layer_logged = false;

   // Last frame's first-person model parts, biggest first (the gun).
   std::vector<Model_part> ranked_parts;

   XrAction move_action = XR_NULL_HANDLE; // left stick: movement keys
   XrAction turn_action = XR_NULL_HANDLE; // right stick: snap turn, up / down weapons
   bool turn_latched = false;             // stick pushed, waiting for it to centre

   // Buttons with a second action when held (see long_press_seconds) and taps.
   float reload_held = -1.0f; // seconds A is held, -1 when up
   bool lock_sent = false;    // this press
   float panel_held = -1.0f;  // Y
   bool panel_toggled = false;
   std::array<std::uint32_t, 32> tap_frames{}; // frames left per Game_control
   bool sprint_latched = false;
   bool sprint_click_down = false;
   bool left_pad_down = false;

   // Quick reload (see update_quick_reload).
   const void* reload_weapon = nullptr; // the reload shown, null without one
   float reload_progress = 0.0f;        // 0..1
   float reload_last_elapsed = 0.0f;
   glm::vec2 reload_window{};       // start, end (fractions of the reload)
   bool reload_window_gone = false; // pressed once in this reload: hit or missed
   bool reload_press_taken = false; // this A press was the minigame's
   float reload_flash = 0.0f;       // seconds left of a hit's full bar
   float reload_buzz_due = 0.0f;    // the window's buzz: seconds to its next pulse
   float reload_miss_pulse_in = 0.0f; // a miss's second knock: seconds to it (0 = none)
   std::uint32_t reload_random = 0;

   // Shaking an overheated gun (see update_shake_cooling).
   glm::vec3 shake_last_position{};
   glm::vec3 shake_velocity{};
   glm::vec3 shake_stroke{}; // the last fast stroke's direction (zero: none yet)
   bool shake_position_valid = false;
   float shake_level = 0.0f; // 0..1
   bool shake_cooling_logged = false;

   // Comfort vignette: how strong it is now (eased), and a snap turn's flash.
   float vignette = 0.0f;
   float snap_flash = 0.0f;
   float move_amount = 0.0f; // stick movement or smooth turning, 0..1 this frame

   // Edge margin (see update_overscan): last head orientation and the margin now.
   bool overscan_orientation_valid = false;
   XrQuaternionf overscan_orientation{0.0f, 0.0f, 0.0f, 1.0f};
   float overscan_degrees = 0.0f;

   // The menu screen's pose (LOCAL space), placed in front of the head whenever the
   // screen comes up.
   XrPosef screen_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -2.0f}};
   bool screen_place_pending = true;

   // Grenade throw (see update_throw): held, the left hand's recent positions (tracking
   // space) with their times, and a throw waiting for the game camera to turn to it.
   bool grenade_held = false;
   std::array<glm::vec4, 16> throw_samples{}; // xyz, time
   std::uint32_t throw_sample_count = 0;
   float input_clock = 0.0f;
   bool throw_pending = false;
   glm::vec3 throw_direction{}; // tracking space
   std::uint32_t throw_wait_frames = 0;
   bool throw_holding = false;  // secondary fire held for the throw
   bool throw_wound_up = false; // the game's wind-up was seen
   float throw_hold_time = 0.0f;
   std::uint32_t throw_hold_left = 0;

   // The eyes' and hands' anchor: the game camera's position plus this (see
   // update_anchor), for the frame anchor_frame.
   glm::vec3 anchor_offset{};
   std::uint64_t anchor_frame = ~0ull;
   float anchor_dt = 0.0f;

   // Head sway off: the steadied height (world), its rate, the camera's last height.
   bool sway_valid = false;
   float sway_height = 0.0f;
   float sway_rate = 0.0f;
   float sway_camera_height = 0.0f;

   // Turret: the camera's view lines summed (see update_anchor), the camera's position
   // when the player took the turret, and the point the lines meet.
   bool turret_anchor = false;

   // On a turret: where the player's own soldier stands (its skinned draws there are left
   // out of the eyes), as of this frame; how many were, and the nearest skinned draw.
   std::optional<glm::vec3> turret_body;

   // Skinned draws seen near the head on foot (the last turret_seen_frames frames): on
   // entering a turret, those at the player's soldier are the turret's own (the soldier
   // is drawn from the same origin), their element counts kept in turret_own_elements;
   // other skinned draws there are the soldier's.
   struct Seen_skinned {
      glm::vec3 origin;
      UINT elements;
      std::uint64_t frame;
   };

   std::vector<Seen_skinned> seen_skinned;
   std::uint64_t seen_skinned_frame = ~0ull;
   glm::vec3 seen_skinned_head{};
   std::vector<UINT> turret_own_elements;
   bool turret_own_taken = false;

   // The gunner: skinned draws near the turret in its first turret_gunner_frames frames
   // manned that weren't there before (element counts); hidden near it from then on.
   std::vector<UINT> turret_gunner_elements;
   std::uint64_t turret_manned_frame = 0;

   // The player's own body's skinned draws (element counts) seen at its feet on foot
   // (drawn for shadows even in first person): on a turret they're the gunner's, never
   // the turret's own. The feet read once a frame.
   std::vector<UINT> player_body_elements;
   std::optional<glm::vec3> seen_skinned_feet;
   std::uint32_t stat_turret_body_draws = 0;
   std::uint32_t stat_turret_skinned_draws = 0;
   float stat_turret_nearest_skinned = 1e9f;
   glm::mat3 turret_projectors{0.0f};
   glm::vec3 turret_projected{};
   std::uint32_t turret_lines = 0;
   glm::vec3 turret_entry{};
   bool turret_pivot_valid = false;
   glm::vec3 turret_pivot{};

   // Weapon sway off: each first-person model part's (by rank) eased pose in the
   // camera's view space, and the frame it was last eased in.
   struct Steady_part {
      bool valid = false;
      glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
      glm::vec3 origin{};
      std::uint64_t frame = ~0ull;
   };

   std::array<Steady_part, 8> steady_parts{};

   // The scope: shown this frame (its view drawn as view 2), raised by holding the gun
   // to the eye, its window's pose (LOCAL space).
   bool scope_shown = false;
   bool scope_raised = false;
   XrPosef scope_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {}};
   bool scope_zoom_latched = false;

   // The scope button (right grip): down last frame, seconds since its last press (a
   // second press within scope_double_click_seconds is a double click).
   bool scope_button_down = false;
   float scope_press_age = 1e9f;

   // The VR panel: open in a match, its pose (LOCAL space), where the right hand points
   // on it, its texture and swapchain.
   bool panel_open = false;
   XrPosef panel_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {}};
   bool panel_shown = false; // this frame
   bool panel_hit = false;
   glm::vec2 panel_pointer{-1.0f, -1.0f};
   bool panel_click = false;
   ID3D11Texture2D* panel_texture = nullptr;
   ID3D11RenderTargetView* panel_rtv = nullptr;
   XrSwapchain panel_swapchain = XR_NULL_HANDLE;
   std::vector<ID3D11Texture2D*> panel_swapchain_images;
   LONGLONG panel_last_tick = 0;
   bool panel_failed = false;

   // Frame rate (frames submitted per second, over half-second windows) and the
   // headset's refresh rate (from the runtime's display period), for the panel.
   LONGLONG perf_window_start = 0;
   std::uint32_t perf_window_frames = 0;
   float perf_fps = 0.0f;
   float perf_refresh_hz = 0.0f;

   // Focus lost (a system menu over the game) in a match: the game is paused (Esc held
   // for this many more input frames, sent while unfocused).
   bool input_gameplay = false; // the last focused input frame was gameplay
   std::uint32_t focus_pause_frames = 0;
   float panel_size_m = panel_width; // one window's width in the world

   // The game controls held last frame (Game_control bits).
   std::uint32_t previous_controls = 0;

   // The first-person model's name for its corrections (its parts' sizes).
   std::string model_signature;

   // The player is in a vehicle, shown on the virtual screen: the match's controls stay
   // on (see update_input).
   bool vehicle_controls = false;
   glm::vec2 vehicle_steer_remainder{};

   // The game's control mode (melee::player_control_mode): the profile's control set the
   // controls press keys from. In a vehicle the left grip held is a second layer (see
   // update_input): the right stick up / down switches position (latched until it
   // centres), Y pressed with it shows the map until let go.
   int control_mode = 0;
   int logged_control_mode = -1;
   bool position_latched = false;
   bool map_press = false;

   // Steering brake (see update_vehicle_brake): the game camera's heading and turn rate,
   // how far it turns per mouse count (learnt while steering; 0 unknown), and how long
   // the brake has been on since the stick was let go.
   // Per axis: x yaw, y pitch.
   bool vehicle_yaw_valid = false;
   glm::vec2 vehicle_yaw{};      // heading, pitch (rad)
   glm::vec2 vehicle_yaw_rate{}; // rad/s
   std::uint64_t vehicle_yaw_frame = 0;
   glm::vec2 vehicle_turn_per_count{};
   glm::vec2 vehicle_brake_time{};
   std::array<bool, 2> vehicle_brake_logged{};
   glm::vec2 vehicle_stick_sent{}; // mouse counts out (steering by position)

   // The draw eye_view_projection last put into the right hand is the weapon (the
   // model's largest part; see mirrored_in_hand).
   bool last_draw_weapon = false;

   // The game's cursor (see update_menu_pointer): written directly when the exe is the
   // known one.
   bool cursor_checked = false;
   std::int32_t* cursor_position = nullptr;      // x, y
   const std::int32_t* cursor_limits = nullptr; // max x, max y

   // The gameplay camera whose field of view was last widened, and the game's own then
   // (put back when the menu screen comes up: the death camera keeps the camera).
   std::uint8_t* widened_camera = nullptr;
   glm::vec2 widened_game_tan{};
   glm::vec2 widened_written_tan{};

   // This frame's first-person model parts, in draw order (eye 0).
   std::vector<Model_part> model_parts;

   // The first-person model's frame, and the gun's movement in it and in the other
   // candidate frame since the last check.
   Weapon_frame weapon_frame = Weapon_frame::aim;
   bool gun_origin_known = false;
   glm::vec3 gun_origin{};
   glm::vec3 gun_origin_other{};
   float gun_movement = 0.0f;
   float gun_movement_other = 0.0f;
   std::uint32_t weapon_frame_frames = 0;

   // Near scene camera (turned to the model's frame), and its eye cameras moved into
   // each hand (and not moved yet, with the hands' poses and grips: for steadied parts).
   Game_camera cached_weapon_camera{};
   std::array<std::array<glm::vec4, 4>, 2> cached_near_eye_vp{};
   World_pose cached_right_hand{};

   // The game's first-person gun (world, in front of the game camera) -> the gun in the
   // hand that holds the weapon part (the right one if both), p' = r p + t, as last
   // drawn (fire_from_hand). With weapon sway off the gun is drawn steadied
   // (steady_in_hand): that draw's own r, t (the bobbing the game's fire point has is
   // taken out with the model's).
   bool fire_map_valid = false;
   bool fire_map_left = false;
   std::uint64_t fire_map_frame = 0;
   glm::mat3 fire_map_r{1.0f};
   glm::vec3 fire_map_t{};
   glm::vec3 fire_map_forward{};
   glm::vec3 fire_map_camera{};
   glm::mat3 fire_map_camera_rotation{1.0f}; // view -> world
   glm::vec3 fire_map_muzzle_offset{};       // the model's tuning (view space)
   bool steady_gun_valid = false;
   std::uint64_t steady_gun_frame = 0;
   glm::mat3 steady_gun_r{1.0f};
   glm::vec3 steady_gun_t{};
   World_pose cached_left_hand{};
   int cached_right_grip = -1;
   int cached_left_grip = -1;
   glm::vec3 cached_right_grip_tuning{};
   glm::vec3 cached_left_grip_tuning{};
   Game_camera cached_weapon_render_camera{}; // as drawn
   std::array<std::array<glm::vec4, 4>, 2> cached_hand_vp{};
   std::array<glm::vec3, 2> cached_hand_position{};
   bool cached_left_in_hand = false;
   std::array<std::array<glm::vec4, 4>, 2> cached_left_hand_vp{};
   std::array<glm::vec3, 2> cached_left_hand_position{};

   // Weapon pass bookkeeping: menus (and the unit select screen) have none.
   bool weapon_seen_this_frame = false;
   std::uint32_t frames_without_weapon = 0;
   // Frames since the player last controlled an entity (soldier, vehicle, turret).
   std::uint32_t frames_without_player = 0;
   std::uint32_t frames_with_screen = 0; // see menu_after_frames_with_screen
   std::uint32_t weapon_gaps_logged = 0;
   std::uint32_t weapon_pass_draws = 0; // this frame, eye 0
   bool weapon_draws_log_pending = true; // log the next weapon pass draw by draw
   bool weapon_draws_logged_this_frame = false;

   // Right hand grip from bf2vr_weapon.txt: the point of the game camera's view space
   // (x right, y up, -z forward, metres) put at the controller's aim pose.
   glm::vec3 weapon_grip_view{0.12f, -0.15f, -0.25f};

   // World camera of this frame (last one eye cameras were built for).
   Game_camera frame_world_camera{};
   bool frame_world_camera_valid = false;

   // The XR frame the game is currently rendering.
   bool frame_begun = false;
   XrFrameState frame_state{XR_TYPE_FRAME_STATE};
   std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
   bool views_valid = false;

   // Mapping from the tracking space to the game world, set on first use: the head's
   // horizontal heading at recentre (tracking space) maps to the game camera's heading
   // (world space). Built from the camera's own axes, so it holds whatever the
   // handedness of the game's world is.
   bool recentered = false;
   glm::vec3 tracking_right{1.0f, 0.0f, 0.0f};
   glm::vec3 tracking_forward{0.0f, 0.0f, -1.0f};
   glm::vec3 world_right{1.0f, 0.0f, 0.0f};
   glm::vec3 world_up{0.0f, 1.0f, 0.0f};
   glm::vec3 world_forward{0.0f, 0.0f, 1.0f};
   float world_handedness = 1.0f; // cameras of the other handedness are reflections
   glm::vec3 tracking_origin{};  // head position (tracking space) that maps to the game camera

   // Tracked points map to the world relative to tracking_reference, scaled by
   // tracking_scale: the head's position at recentre and 1, or the body's (update_anchor).
   glm::vec3 tracking_reference{};
   float tracking_scale = 1.0f;

   // Body height (anchor_body): the character's eye height per soldier state, known from
   // any life and valid in this one (an unknown standing one is body_default_eye_height),
   // and the heights the eyes are eased to.
   struct Character_height {
      float height = 0.0f;
      bool known = false;
      bool valid = false;
   };

   std::array<Character_height, body_states> character_heights{};
   bool character_from_head = false; // this life's heights are the head bone's
   std::array<float, body_lock_samples> standing_samples{};
   int standing_sample_count = 0;
   bool standing_locked = false; // this life's standing height is final
   bool body_anchored = false;
   float body_eye_height = 0.0f;
   float body_standing_height = 0.0f;
   float body_player_eye_height = 0.0f;
   bool body_player_from_floor = false;
   float body_fraction = 1.0f;  // the player's head over the floor / their standing height
   float body_crouch_drop = 0.0f; // eased: the game's crouch (button) lowers the eyes this much
   std::int32_t body_soldier_state = -1;

   // Physical crouch (update_body_crouch): the soldier state taken as the crouch, the
   // crouch is ours (a tap of ours crouched the soldier), and a tap awaiting its answer.
   std::int32_t crouch_state = 1;
   bool crouch_state_learned = false;
   bool crouch_by_body = false;
   int crouch_pending = 0; // +1 a tap to crouch, -1 to stand up
   std::int32_t crouch_pending_from = -1;
   float crouch_pending_time = 0.0f;
   int crouch_misses = 0;
   bool crouch_given_up = false;
   bool crouch_button_down = false;
   float crouch_button_watch = -1.0f; // seconds since a press from standing, < 0 none
   bool crouch_button_owns = false;   // the button chose: no taps of ours till the player stands
   bool sway_on_feet = false;

   // The floor (tracking y, from the stage space at recentre), and the player's eye
   // height over it, taken at the session's first recentre and at the panel's.
   bool floor_valid = false;
   float floor_height = 0.0f;
   float player_eye_measured = 0.0f;
   float player_eye_logged = 0.0f;
   bool measure_player = true;

   // Seated: the head's tracking height the character's standing eyes match (taken at
   // the session's first recentre, the panel's Recenter and Measure height).
   float seated_head_y = 0.0f;
   bool seated_head_valid = false;

   // The panel's Measure height: seconds until it's taken, and what it found.
   float measure_countdown = 0.0f;
   std::string measure_text;

   // The left stick moves the soldier this frame (body_follow_seconds).
   bool stick_moving = false;

   // The head's horizontal distance from the soldier, per stats period.
   float stat_body_offset_sum = 0.0f;
   float stat_body_offset_max = 0.0f;
   std::uint32_t stat_body_offset_frames = 0;

   // Diagnostics: per-second counts of stereo camera matches and logged rejections.
   std::uint32_t stat_frames = 0;
   std::uint32_t stat_frames_with_camera = 0;
   std::uint32_t stat_rejects = 0;
   std::uint32_t stat_should_render = 0;
   std::uint32_t stat_views_valid = 0;
   std::uint32_t stat_projection_layers = 0;
   std::uint32_t stat_quad_layers = 0;
   std::uint32_t stat_mirrored_frames = 0;
   std::uint32_t stat_far_draws = 0;
   std::uint32_t stat_aim_valid = 0;
   std::uint32_t stat_weapon_draws = 0;

   // Game camera frustum widening (see update_frustum_hook).
   std::uintptr_t update_frustum = 0; // the engine's function, 0 = not hooked
   std::uintptr_t build_view = 0;     // the engine's function, 0 = not hooked
   bool head_camera_disabled = false; // no bf2vr_head_camera.txt
   std::uint32_t stat_head_camera_builds = 0;

   // The gameplay camera's own rotation (the gun's aim) as the game set it, and the
   // frame it was taken in (the camera is drawn with the head's rotation instead).
   Camera_rows engine_aim{};
   std::uint64_t engine_aim_frame = ~0ull;

   // The camera whose rows hold the head's rotation until the frame ends, what was
   // written (rows right, up, back with w) and the game's rows to put back.
   std::uint8_t* head_camera = nullptr;
   std::array<float, 12> head_rows_written{};
   std::array<float, 12> aim_rows_saved{};

   // Head culling (see begin_scene): this frame's gameplay cameras, and the ones whose
   // rotation and field of view are the head's until Present, with the game's values.
   struct Head_cull {
      std::uint8_t* camera;
      std::array<float, 12> game_rows;
      std::array<float, 12> head_rows;
      std::array<float, 16> game_view; // the view matrix (+0x70), with head_view
      std::array<float, 16> head_view;
      bool view_written;
      glm::vec3 game_position; // +0x60, with position_written
      glm::vec3 head_position;
      bool position_written;
      glm::vec2 game_tan;
      bool restore; // false for temporary copies (stack or heap: gone by Present)
   };

   std::vector<std::uint8_t*> cull_cameras;
   std::vector<Head_cull> head_culls;
   bool head_cull_disabled = false; // bf2vr_no_head_cull.txt
   bool head_cull_done = false;     // this frame
   std::uint32_t stat_head_culls = 0;

   // Render cameras (see set_render_camera_hook): the engine's function, gameplay
   // cameras made current while the frame is drawn, how many of those still had the
   // game's rotation (culled with the head's from there on), gameplay cameras made
   // current before head culling started (outside the renderer: left alone), and frames
   // whose head culling the renderer's first camera started (before BeginScene).
   std::uintptr_t set_render_camera = 0;
   std::uint32_t stat_render_cameras = 0;
   std::uint32_t stat_late_head_culls = 0;
   std::uint32_t stat_frustum_head_culls = 0; // frustum rebuilt while drawing
   std::uint32_t stat_render_cameras_early = 0;
   std::uint32_t stat_head_cull_started_early = 0;
   std::vector<std::uintptr_t> late_cull_sites; // logged once each

   // Sector / portal visibility (see pvs_camera_hook): the engine's function, whether
   // the map uses it, updates (with the head's view) and the portal test's half-angle.
   std::uintptr_t pvs_camera = 0;
   bool stat_pvs_on = false;
   std::uint32_t stat_pvs_updates = 0;
   std::uint32_t stat_pvs_head_updates = 0;
   float stat_pvs_half_angle = 0.0f;

   std::uint32_t stat_frustum_widened = 0;
   std::uint32_t stat_frustum_other = 0; // frustum updates of other cameras
   float stat_widest_tan_x = 0.0f;
   float stat_widest_tan_y = 0.0f;

   std::uint32_t frustum_logged = 0;

   std::uint32_t rejects_logged = 0;
   std::uint32_t mirrored_logged = 0;
   ULONGLONG stat_start_ms = 0;
   bool mirrored_seen_this_frame = false;

   // Cameras drawn with this frame and how many stereo draws each got. The one with
   // the most draws is the world camera; others are e.g. the first-person weapon.
   struct Frame_camera {
      std::array<glm::vec4, 4> game_vp;
      Game_camera camera;
      std::uint32_t draws;
      std::uint32_t first_draw; // stereo draws of the frame before its first one
      int depthstencil;
      bool far_scene = false; // the far scene (sky, distant backdrop)
      bool weapon = false;    // the first-person weapon pass (near scene)
   };

   std::uint32_t frame_draws = 0;
   bool weapon_draw_logged = false;
   bool hud_draw_logged = false;

   // Menus: this frame is drawn as the game made it and shown on the virtual screen,
   // where the right hand points the game's cursor.
   bool menu_frame = false;
   bool screen_blur_logged = false;
   bool screen_shown = false; // the last submitted frame went to the virtual screen
   glm::vec2 screen_size{};   // metres
   glm::uvec2 window_size{};
   bool pointer_valid = false;
   glm::vec2 pointer_uv{}; // 0..1 across the screen from the top-left
   bool pointer_homed = false;
   glm::vec2 pointer_last_px{};
   bool pointer_logged = false;
   std::uint32_t stat_cursor_queries = 0;

   std::vector<Frame_camera> frame_cameras;
   int current_frame_camera = -1;
   bool main_camera_valid = false;
   Game_camera main_camera{};      // world camera of the last frame that had one
   bool cameras_logged = false;

   // Position of the world camera in the previous frame that had one.
   std::optional<glm::vec3> previous_frame_camera_position;

   // Per-frame derived data.
   std::array<Eye_view, 2> eyes{};
   bool camera_seen_this_frame = false;
   bool camera_seen_last_frame = false;

   // Cache for eye_view_projection (views: the eyes, then the scope).
   std::array<glm::vec4, 4> cached_game_vp{};
   std::array<std::array<glm::vec4, 4>, 3> cached_eye_vp{};
   std::array<glm::vec3, 3> cached_eye_position{};
   std::array<std::array<glm::vec4, 4>, 3> cached_eye_vp_at_infinity{};
   std::array<std::array<glm::vec4, 4>, 2> cached_eye_vp_far_anchored{};
   glm::vec3 cached_far_anchored_position{};
   bool cached_far_anchored = false;
   bool cached_hud = false;
   glm::vec3 cached_camera_position{};
   bool cached_valid = false;
   bool cached_in_hand = false;
   bool cached_weapon_pass = false;
   bool cache_filled = false;
   std::uint64_t frame_index = 0;
   std::uint64_t cached_frame_index = ~0ull;

   glm::vec2 mouse_remainder{};

   ID3D11Device* device = nullptr;
   bool device_removal_logged = false;
   bool stereo_disabled = false;

   // Melee (vr_melee.cpp): both grips held with a melee weapon (block), the world
   // camera position the saber was placed at, the last update's time, and the saber
   // overlay's bookkeeping.
   bool saber_block_grips = false;

   // A trigger pressed on a menu (the unit select's spawn click) and still held in the
   // match: ignored there until let go.
   bool fire_held_from_menu = false;
   bool secondary_held_from_menu = false;

   // Hand swap (update_hand_swap): the weapon in the left hand. The hands' spaces,
   // triggers, grips and vibration are swapped (handles_swapped) while it's on and the
   // player isn't in a vehicle. The hands together: the free hand's grip takes the
   // weapon (a double click with a gun: a single press is the two-handed hold).
   bool hands_swapped = false;
   bool handles_swapped = false;
   bool hands_together = false;
   float hands_together_buzz_due = 0.0f;
   bool swap_grip_down = false;
   float swap_press_age = 1e9f;

   // Saber rotation (update_saber_rotation), per hand (0 the weapon's, right; 1 the left,
   // dual sabers): the saber's rotation relative to its hand's grip pose, and while that
   // hand's grip is held, the saber's rotation kept (both in the tracking space). pose is
   // the grip pose turned by it, for vr_melee.
   struct Saber_turn {
      glm::quat offset{1.0f, 0.0f, 0.0f, 0.0f};
      bool held_still = false;
      glm::quat still_rotation{1.0f, 0.0f, 0.0f, 0.0f};
      bool grip_down = false;
      bool grip_ignored = false; // till let go: no offset taken (double click, block)
      float press_age = 1e9f;
      XrPosef pose{};
   };

   std::array<Saber_turn, 2> saber_turns{};
   bool had_melee = false;
   glm::vec3 melee_anchor{};
   LONGLONG melee_last_tick = 0;
   bool overlay_drawn = false; // this frame
   bool overlay_vp_valid = false;
   std::array<glm::vec4, 4> overlay_vp{}; // last camera checked for the HUD

   // Stun / knockdown (see update_stun): the vibration's clock.
   bool stun_active = false;
   bool stun_held = false;
   float stun_buzz_time = 0.0f;

   // Vibration: seconds since start (melee updates), until when a strong pulse runs
   // (the block's buzz waits for it), and when the block's buzz is due again.
   float haptic_clock = 0.0f;
   float haptic_strong_until = 0.0f;
   float block_buzz_due = 0.0f;
   bool block_logged = false;

   // Render scale (see stereo_render_size) and the scope column's width (pixels).
   bool render_scale_loaded = false;
   float render_scale = 0.85f;
   std::uint32_t scope_column = 0;
};

State state;
// The exe's preferred base: addresses below are as in the unrelocated exe (it runs with
// ASLR, so they're rebased on the module handle).
constexpr std::uintptr_t exe_image_base = 0x400000;

std::uint32_t prop_pool_overruns = 0; // see patch_prop_pool (bumped from asm)
std::uintptr_t prop_pool_count = 0;    // the game's pool size (int), see patch_prop_pool
std::uint32_t stale_shader_draws = 0; // see patch_stale_shaders (bumped from asm)
std::uintptr_t stale_shader_vtable = 0; // the last one's object's vtable (set from asm)
std::uint32_t dropped_queued_draws = 0; // see patch_queue_push
const std::uint32_t* draw_count = nullptr; // see enlarge_draw_array
std::uint32_t draw_array_capacity = 0x128c;
std::uint32_t peak_draw_count = 0; // the most draws a frame had (sampled at queue pushes)
std::atomic<bool> fire_pressed = false;
std::atomic<bool> secondary_fire_pressed = false;
std::atomic<LONG> mouse_dx = 0;
std::atomic<LONG> mouse_dy = 0;
std::atomic<LONG> mouse_wheel = 0;

// The game keys held for the controllers (read by the DirectInput hooks on the game's
// thread, written at Present on the same thread).
Input_state held_input;

// --- Game controls -------------------------------------------------------------
//
// What the controllers do is pressed as the game's own controls, on the keys the
// player's profile binds them to (GameData\SaveGames\<name>.profile, the most recently
// saved one). A profile keeps the soldier's bindings first: records of three 2-byte
// slots from 0x4e, each slot (DIK key code, mouse code); mouse codes 0x43-0x46 are
// buttons 1-4, 0x4b / 0x4c the wheel up / down. The records follow the PC controls
// screen: fire, secondary fire, sprint, jump, crouch, zoom, view, reload, enter / use,
// squad command, accept, decline, lock target, next primary, (previous primary), next
// secondary, ..., map (28), roll (29), and last right, left, forward, back (35-38).
// Four more control sets of 43 records follow the soldier's: vehicle, starfighter,
// jedi, turret (the game's control modes; FUN_005876f0 names each record per mode). The
// game reads the current mode's set, where the same records are that mode's own
// controls: in a vehicle crouch = droideka deploy, next / previous primary = next /
// previous position; in a starfighter sprint = boost, jump = land / take off, crouch =
// trick, zoom = roll lock, roll = Immelmann, left / right = roll, forward / back =
// throttle.
enum Game_control : int {
   control_sprint,
   control_jump,
   control_crouch,
   control_reload,
   control_use,
   control_squad,
   control_lock,
   control_next_primary,
   control_next_secondary,
   control_move_forward,
   control_move_back,
   control_move_left,
   control_move_right,
   control_zoom,
   control_view,
   control_previous_primary,
   control_roll,
   control_map,
   control_menu_accept, // Enter (menus)
   control_pause,       // Esc
   control_count
};

static_assert(control_count <= 32); // bits of a std::uint32_t

struct Game_binding {
   std::uint8_t key = 0;   // DIK code, or 0
   std::uint8_t mouse = 0; // profile mouse code, or 0
};

constexpr std::array<const char*, control_count> control_names{
   "sprint", "jump", "crouch", "reload", "use", "squad command", "lock target",
   "next primary", "next secondary", "forward", "back", "left", "right", "zoom", "view",
   "previous primary", "roll", "map", "enter", "pause"};

// Profile record per control (-1: a fixed key).
constexpr std::array<int, control_count> control_records{
   2, 3, 4, 7, 8, 9, 12, 13, 15, 37, 38, 36, 35, 5, 6, 14, 29, 28, -1, -1};

// The game's defaults.
constexpr std::array<Game_binding, control_count> default_bindings{
   Game_binding{0x2a}, // Left Shift
   Game_binding{0x39}, // Space
   Game_binding{0x1d}, // Left Ctrl
   Game_binding{0x13}, // R
   Game_binding{0x12}, // E
   Game_binding{0x3e}, // F4
   Game_binding{0x10}, // Q
   Game_binding{0x02}, // 1
   Game_binding{0x03}, // 2
   Game_binding{0x11}, // W
   Game_binding{0x1f}, // S
   Game_binding{0x1e}, // A
   Game_binding{0x20}, // D
   Game_binding{0x2c}, // Z
   Game_binding{0x2f}, // V
   Game_binding{},     // (previous primary: unbound)
   Game_binding{0x38}, // Left Alt
   Game_binding{0x32}, // M
   Game_binding{0x1c}, // Enter
   Game_binding{0x01}, // Esc
};

std::array<Game_binding, control_count> game_bindings = default_bindings;

// Every control set (see Game_control), per record; set 0 is the soldier's.
constexpr int control_mode_count = 5;
constexpr int profile_record_count = 43;
constexpr std::size_t profile_section_size = profile_record_count * 6;

using Control_set = std::array<Game_binding, profile_record_count>;

std::array<Control_set, control_mode_count> mode_bindings{};

constexpr std::array<const char*, control_mode_count> control_mode_names{
   "soldier", "vehicle", "starfighter", "jedi", "turret"};

std::string bindings_text;
FILETIME bindings_profile_time{};
bool bindings_loaded = false;

constexpr std::size_t profile_records_offset = 0x4e;
constexpr std::size_t profile_record_size = 6;

bool failed(const XrResult result, const char* what) noexcept
{
   if (XR_SUCCEEDED(result)) return false;

   log_fmt(Log_level::warning, "VR: {} failed ({})", what, static_cast<int>(result));

   return true;
}

// --- Math helpers -----------------------------------------------------------

auto rotate(const XrQuaternionf& q, const glm::vec3 v) noexcept -> glm::vec3
{
   const glm::vec3 u{q.x, q.y, q.z};
   const glm::vec3 t = 2.0f * glm::cross(u, v);

   return v + q.w * t + glm::cross(u, t);
}

// The game world is Y-up.
constexpr glm::vec3 world_up_axis{0.0f, 1.0f, 0.0f};

// The rotation whose columns are `x`, `y`, `z` (orthonormal) as an OpenXR quaternion.
auto to_quaternion(const glm::vec3 x, const glm::vec3 y, const glm::vec3 z) noexcept
   -> XrQuaternionf
{
   const glm::quat q = glm::quat_cast(glm::mat3{x, y, z});

   return {q.x, q.y, q.z, q.w};
}

auto conjugate(const XrQuaternionf& q) noexcept -> XrQuaternionf
{
   return {-q.x, -q.y, -q.z, q.w};
}

auto to_vec3(const XrVector3f& v) noexcept -> glm::vec3
{
   return {v.x, v.y, v.z};
}

auto horizontal(const glm::vec3 v) noexcept -> glm::vec3
{
   const glm::vec3 h{v.x, 0.0f, v.z};
   const float length = glm::length(h);

   return length > 1e-4f ? h / length : glm::vec3{};
}

// OpenXR tracking space (right-handed, -Z forward) -> game world, through the frames
// captured at recentre: tracking right/up/forward -> the camera's right/up/forward.
auto tracking_to_world(const glm::vec3 v) noexcept -> glm::vec3
{
   return glm::dot(v, state.tracking_right) * state.world_right +
          v.y * state.world_up + glm::dot(v, state.tracking_forward) * state.world_forward;
}

// A hand aim pose (the right one by default) in the game world, anchored like the eyes
// at the world camera.
auto aim_pose_world(const Game_camera& world_camera,
                    const XrPosef& pose = state.aim_pose) noexcept -> World_pose
{
   const glm::vec3 position{pose.position.x, pose.position.y, pose.position.z};

   return {world_camera.position + state.anchor_offset +
              state.tracking_scale * tracking_to_world(position - state.tracking_reference),
           glm::mat3{tracking_to_world(rotate(pose.orientation, {1.0f, 0.0f, 0.0f})),
                     tracking_to_world(rotate(pose.orientation, {0.0f, 1.0f, 0.0f})),
                     tracking_to_world(rotate(pose.orientation, {0.0f, 0.0f, 1.0f}))}};
}

// The gun in the game world: in the right hand, turned by both hands while held with
// two (state.weapon_rotation, see update_weapon_rotation).
auto weapon_pose_world(const Game_camera& world_camera) noexcept -> World_pose
{
   World_pose pose = aim_pose_world(world_camera);

   if (state.two_handed) {
      for (int i = 0; i < 3; ++i) pose.rotation[i] = tracking_to_world(state.weapon_rotation[i]);
   }

   return pose;
}

// The game's constants hold the rows of clip = P * V * p (column vectors). For a
// symmetric perspective P, row 3 is V's forward row, rows 0/1 are scaled right/up rows.
// Clip coordinates are homogeneous, so the whole matrix may carry any positive scale.
auto decompose_camera(const std::array<glm::vec4, 4>& game_vp, Game_camera& camera) noexcept
   -> Reject
{
   const float w_scale = glm::length(glm::vec3{game_vp[3]});

   // No view-dependent w: orthographic / screen-space (HUD, post-processing).
   if (w_scale < 1e-6f) return Reject::no_w_row;

   std::array<glm::vec4, 4> vp;

   for (int i = 0; i < 4; ++i) vp[i] = game_vp[i] / w_scale;

   const float scale_x = glm::length(glm::vec3{vp[0]});
   const float scale_y = glm::length(glm::vec3{vp[1]});

   if (scale_x < 1e-4f || scale_y < 1e-4f) return Reject::degenerate;

   const glm::vec4 right = vp[0] / scale_x;
   const glm::vec4 up = vp[1] / scale_y;
   const glm::vec4 forward = vp[3];

   const glm::vec3 r{right}, u{up}, f{forward};

   if (std::abs(glm::dot(r, u)) > camera_epsilon ||
       std::abs(glm::dot(r, f)) > camera_epsilon ||
       std::abs(glm::dot(u, f)) > camera_epsilon) {
      return Reject::not_orthogonal;
   }

   camera.right = r;
   camera.up = u;
   camera.forward = f;
   camera.position = -(right.w * r + up.w * u + forward.w * f);
   camera.depth_scale = glm::dot(glm::vec3{vp[2]}, f);
   camera.depth_offset = vp[2].w - camera.depth_scale * forward.w;
   camera.handedness = glm::dot(glm::cross(r, u), f) < 0.0f ? -1.0f : 1.0f;
   camera.tan_half_fov_x = 1.0f / scale_x;

   return Reject::none;
}

void log_rejected_camera(const Reject reason, const std::array<glm::vec4, 4>& vp) noexcept
{
   state.stat_rejects += 1;

   // Screen-space draws (HUD) are expected; log only the surprising ones, a few times.
   if (reason == Reject::no_w_row || state.rejects_logged >= 6) return;

   state.rejects_logged += 1;

   log_fmt(Log_level::info,
           "VR: view-projection not taken as a camera ({}): "
           "[{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}] "
           "[{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}]",
           reason == Reject::degenerate ? "degenerate" : "axes not orthogonal",
           vp[0].x, vp[0].y, vp[0].z, vp[0].w, vp[1].x, vp[1].y, vp[1].z, vp[1].w,
           vp[2].x, vp[2].y, vp[2].z, vp[2].w, vp[3].x, vp[3].y, vp[3].z, vp[3].w);
}

void log_stats() noexcept
{
   const ULONGLONG now = GetTickCount64();

   if (state.stat_start_ms == 0) state.stat_start_ms = now;
   if (now - state.stat_start_ms < 10'000) return;

   log_fmt(Log_level::info,
           "VR: last {} frames: shouldRender {}, views valid {}, eye cameras {}, "
           "only mirrored cameras {}, rejected matrices {}, far-scene draws {}; "
           "layers: projection {}, quad {}; session state {}.",
           state.stat_frames, state.stat_should_render, state.stat_views_valid,
           state.stat_frames_with_camera, state.stat_mirrored_frames,
           state.stat_rejects, state.stat_far_draws, state.stat_projection_layers,
           state.stat_quad_layers, static_cast<int>(state.session_state));

   state.stat_start_ms = now;
   state.stat_frames = 0;
   state.stat_frames_with_camera = 0;
   state.stat_rejects = 0;
   state.stat_should_render = 0;
   state.stat_views_valid = 0;
   state.stat_projection_layers = 0;
   state.stat_quad_layers = 0;
   state.stat_mirrored_frames = 0;
   state.stat_far_draws = 0;

   melee::log_stats();

   if (state.recentered && state.main_camera_valid) {
      const XrVector3f& p = state.aim_pose.position;
      const World_pose hand = aim_pose_world(state.main_camera);
      const glm::vec3 aim_forward = -hand.rotation[2];

      log_fmt(Log_level::info,
              "VR: right aim pose valid in {} frames, last at tracking ({:.2f}, {:.2f}, "
              "{:.2f}) -> world ({:.2f}, {:.2f}, {:.2f}) pointing ({:.2f}, {:.2f}, {:.2f}); "
              "weapon draws in hand {}.",
              state.stat_aim_valid, p.x, p.y, p.z, hand.position.x, hand.position.y,
              hand.position.z, aim_forward.x, aim_forward.y, aim_forward.z,
              state.stat_weapon_draws);
   }

   if (state.body_anchored) {
      log_fmt(Log_level::info,
              "VR: body: character's eyes {:.2f} m (standing {:.2f} m{}, from the {}), the "
              "player's head at {:.0f}% of {:.2f} m ({}){}, scale {:.2f}; head off the soldier "
              "{:.2f} m on average, {:.2f} m at most.",
              state.body_eye_height, state.body_standing_height,
              state.standing_locked ? " locked" : "",
              state.character_from_head ? "head bone" : "game camera",
              state.body_fraction * 100.0f, state.body_player_eye_height,
              state.body_player_from_floor ? "over the floor" : "setting",
              state.crouch_by_body ? ", soldier crouched by the body" : "", state.tracking_scale,
              state.stat_body_offset_frames
                 ? state.stat_body_offset_sum / static_cast<float>(state.stat_body_offset_frames)
                 : 0.0f,
              state.stat_body_offset_max);
   }

   state.stat_body_offset_sum = 0.0f;
   state.stat_body_offset_max = 0.0f;
   state.stat_body_offset_frames = 0;

   if (state.pointer_valid || state.stat_cursor_queries > 0) {
      log_fmt(Log_level::info,
              "VR: menu pointer {} at ({:.2f}, {:.2f}); game called GetCursorPos {} times.",
              state.pointer_valid ? "on screen" : "off", state.pointer_uv.x,
              state.pointer_uv.y, state.stat_cursor_queries);
   }

   state.stat_aim_valid = 0;
   state.stat_weapon_draws = 0;

   if (state.update_frustum) {
      log_fmt(Log_level::info,
              "VR: game camera frustum widened {} times (widest half-angles {:.0f} x {:.0f} "
              "deg), other cameras' frustum updates {}, drawn with the head's rotation {}, culled with the head's rotation {}.",
              state.stat_frustum_widened,
              std::atan(state.stat_widest_tan_x) * 180.0f / std::numbers::pi_v<float>,
              std::atan(state.stat_widest_tan_y) * 180.0f / std::numbers::pi_v<float>,
              state.stat_frustum_other, state.stat_head_camera_builds,
              state.stat_head_culls);
   }

   if (state.set_render_camera) {
      log_fmt(Log_level::info,
              "VR: gameplay cameras made current for drawing {} (still with the game's "
              "rotation, culled with the head's from there: {}), frustums rebuilt while "
              "drawing culled with the head's rotation {}, gameplay cameras made current "
              "outside the renderer before head culling {}; head culling started by the "
              "renderer's first camera (before BeginScene) in {} frames.",
              state.stat_render_cameras, state.stat_late_head_culls,
              state.stat_frustum_head_culls, state.stat_render_cameras_early,
              state.stat_head_cull_started_early);
   }

   if (state.pvs_camera) {
      log_fmt(Log_level::info,
              "VR: sector/portal visibility (PVS) {} on this map: updated {} times, {} with "
              "the head's view (portal test opened to {:.0f} deg half-angle).",
              state.stat_pvs_on ? "is on" : "is off", state.stat_pvs_updates,
              state.stat_pvs_head_updates, state.stat_pvs_half_angle);
   }

   if (const std::uint32_t overruns = std::exchange(prop_pool_overruns, 0)) {
      log_fmt(Log_level::info,
              "VR: prop cluster pool ({} cells) ran out mid-cell {} times (game crash "
              "averted).",
              prop_pool_count ? *reinterpret_cast<const int*>(prop_pool_count) : -1,
              overruns);
   }

   // Address space: a 32-bit game runs out of it long before the PC runs out of memory.
   {
      std::uintptr_t largest_free = 0;
      std::uintptr_t total_free = 0;
      MEMORY_BASIC_INFORMATION info{};

      for (auto address = reinterpret_cast<const std::uint8_t*>(0x10000);
           VirtualQuery(address, &info, sizeof(info)) == sizeof(info);
           address = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize) {
         if (info.State == MEM_FREE) {
            total_free += info.RegionSize;
            largest_free = std::max<std::uintptr_t>(largest_free, info.RegionSize);
         }
      }

      MEMORYSTATUSEX status{sizeof(status)};
      GlobalMemoryStatusEx(&status);

      log_fmt(Log_level::info,
              "VR: address space: {} MB free of {} MB, largest free block {} MB; PC memory "
              "{}% in use.",
              total_free >> 20, status.ullTotalVirtual >> 20, largest_free >> 20,
              status.dwMemoryLoad);
   }

   if (const std::uint32_t peak = std::exchange(peak_draw_count, 0)) {
      log_fmt(Log_level::info, "VR: most draws in a frame {} of {}{}.", peak,
              draw_array_capacity - 2,
              peak + 2 >= draw_array_capacity ? " (FULL: later draws of those frames skipped)"
                                              : "");
   }

   if (const std::uint32_t draws = std::exchange(dropped_queued_draws, 0)) {
      log_fmt(Log_level::info,
              "VR: {} draws didn't fit the renderer's sorted queues (not drawn; their "
              "references given back).",
              draws);
   }

   if (const std::uint32_t draws = std::exchange(stale_shader_draws, 0)) {
      const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

      log_fmt(Log_level::info,
              "VR: skipped {} queued draws whose shader was already destroyed (game crash "
              "averted; last object's vtable 0x{:08x}).",
              draws, stale_shader_vtable - base + exe_image_base);
   }

   state.stat_head_cull_started_early = 0;
   state.stat_pvs_updates = 0;
   state.stat_pvs_head_updates = 0;
   state.stat_pvs_half_angle = 0.0f;

   state.stat_frustum_widened = 0;
   state.stat_frustum_other = 0;
   state.stat_head_camera_builds = 0;
   state.stat_head_culls = 0;
   state.stat_render_cameras = 0;
   state.stat_late_head_culls = 0;
   state.stat_frustum_head_culls = 0;
   state.stat_render_cameras_early = 0;
   state.stat_widest_tan_x = 0.0f;
   state.stat_widest_tan_y = 0.0f;
   state.stat_cursor_queries = 0;
}

void build_eye_view_projection(const Eye_view& eye, const Game_camera& camera,
                               std::array<glm::vec4, 4>& out) noexcept
{
   const glm::vec4 view_right{eye.right, -glm::dot(eye.right, eye.position)};
   const glm::vec4 view_up{eye.up, -glm::dot(eye.up, eye.position)};
   const glm::vec4 view_forward{eye.forward, -glm::dot(eye.forward, eye.position)};

   const auto [tan_left, tan_right, tan_up, tan_down] = eye.tangents;

   const float a = 2.0f / (tan_right - tan_left);
   const float b = -(tan_right + tan_left) / (tan_right - tan_left);
   const float c = 2.0f / (tan_up - tan_down);
   const float d = -(tan_up + tan_down) / (tan_up - tan_down);

   out[0] = a * view_right + b * view_forward;
   out[1] = c * view_up + d * view_forward;
   out[2] = camera.depth_scale * view_forward + glm::vec4{0.0f, 0.0f, 0.0f, camera.depth_offset};
   out[3] = view_forward;
}

// The floor under the tracking space (the stage space's), and with `measure_player` the
// player's eye height over it: the head's at recentre.
void update_floor(const bool measure_player) noexcept
{
   if (!state.stage_space || !state.local_space) return;

   XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};

   if (XR_FAILED(state.xr.xrLocateSpace(state.local_space, state.stage_space,
                                        state.frame_state.predictedDisplayTime, &location)) ||
       !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
      return;
   }

   // Both spaces are level: the local space's origin is this high over the floor.
   state.floor_height = -location.pose.position.y;
   state.floor_valid = true;

   if (!measure_player) return;

   const float eye_height = state.tracking_origin.y - state.floor_height;

   if (eye_height < 0.5f || eye_height > 2.3f) {
      log_fmt(Log_level::warning,
              "VR: the head is {:.2f} m over the headset's floor: not taken as the player's "
              "eye height.",
              eye_height);

      return;
   }

   state.player_eye_measured = eye_height;
   state.player_eye_logged = eye_height;

   log_fmt(Log_level::info, "VR: the player's eyes are {:.2f} m over the floor.", eye_height);
}

// Maps the head's current heading onto the world camera's. False when it can't be
// done yet (head or camera looking straight up/down).
bool recenter(const Game_camera& camera) noexcept
{
   if (!state.views_valid) return false;

   const glm::vec3 head_forward = horizontal(
      rotate(state.views[0].pose.orientation, glm::vec3{0.0f, 0.0f, -1.0f}));
   const glm::vec3 camera_right = horizontal(camera.right);

   if (head_forward == glm::vec3{} || camera_right == glm::vec3{}) return false;

   const auto& a = state.views[0].pose.position;
   const auto& b = state.views[1].pose.position;

   // Tracking space is right-handed: right = forward x up.
   state.tracking_forward = head_forward;
   state.tracking_right = glm::cross(head_forward, world_up_axis);
   state.world_right = camera_right;
   state.world_up = world_up_axis;
   state.world_forward = camera.handedness * glm::cross(camera_right, world_up_axis);
   state.world_handedness = camera.handedness;
   state.tracking_origin = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
   state.tracking_reference = state.tracking_origin;
   state.tracking_scale = 1.0f;
   state.body_anchored = false;

   if (state.measure_player || !state.seated_head_valid) {
      state.seated_head_y = state.tracking_origin.y;
      state.seated_head_valid = true;
   }

   update_floor(std::exchange(state.measure_player, false));
   state.recentered = true;
   state.mouse_remainder = {};
   mouse_dx = 0;
   mouse_dy = 0;

   log_fmt(Log_level::info,
           "VR: recentered on the game camera at ({:.1f}, {:.1f}, {:.1f}), "
           "forward ({:.2f}, {:.2f}, {:.2f}), {}-handed world.",
           camera.position.x, camera.position.y, camera.position.z, camera.forward.x,
           camera.forward.y, camera.forward.z, camera.handedness > 0.0f ? "left" : "right");

   return true;
}

// `camera` (one drawn with the gameplay camera's pose) turned to this frame's aim: the
// game's own rotation of its camera, which is drawn with the head's (build_view_hook).
// Unchanged when the head didn't take the camera this frame.
auto aim_camera(Game_camera camera) noexcept -> Game_camera
{
   if (state.engine_aim_frame != state.frame_index) return camera;

   camera.right = glm::normalize(state.engine_aim.right);
   camera.up = glm::normalize(state.engine_aim.up);
   camera.forward = -glm::normalize(state.engine_aim.back);

   return camera;
}

// Picks this frame's world camera (most stereo draws), recentres on cuts, and resets
// the per-frame camera list. Cameras in the world win over the ones at the origin (the
// HUD, menus) and the weapon pass: looking at the sky, the HUD can outnumber the
// world's draws, which made the frame look like a menu. The far scene is left out too:
// its near plane is what the weapon pass is told apart from the world pass by.
void finish_frame_cameras() noexcept
{
   const auto in_world = [](const State::Frame_camera& c) {
      return !c.weapon && !c.far_scene && !is_view_space_camera(c.camera) &&
             glm::length(c.camera.position) > 1.0f;
   };

   const auto main = std::max_element(state.frame_cameras.begin(),
                                      state.frame_cameras.end(),
                                      [&](const auto& a, const auto& b) {
                                         const bool a_world = in_world(a);
                                         const bool b_world = in_world(b);

                                         if (a_world != b_world) return b_world;

                                         return a.draws < b.draws;
                                      });

   if (main != state.frame_cameras.end()) {
      // Drawn looking where the head looks; the game's own rotation is the aim.
      const Game_camera camera = aim_camera(main->camera);

      if (state.recentered && state.frame_cameras.size() > 1 && !state.cameras_logged) {
         state.cameras_logged = true;

         for (const auto& entry : state.frame_cameras) {
            const Game_camera& c = entry.camera;
            // Depth z_ndc = A + B / d along the view axis: 0 at d = -B/A, 1 at B/(1-A).
            const float near_plane = -c.depth_offset / c.depth_scale;
            const float far_plane = c.depth_offset / (1.0f - c.depth_scale);

            log_fmt(Log_level::info,
                    "VR: frame camera slot {} at ({:.1f}, {:.1f}, {:.1f}) forward ({:.2f}, "
                    "{:.2f}, {:.2f}), {} draws from draw {}, depthstencil {}, "
                    "hfov {:.1f} deg, depth 0 at {:.3f} / 1 at {:.1f}{}{}.",
                    &entry - state.frame_cameras.data() + 1, c.position.x, c.position.y,
                    c.position.z, c.forward.x, c.forward.y, c.forward.z, entry.draws,
                    entry.first_draw, entry.depthstencil,
                    2.0f * std::atan(c.tan_half_fov_x) * 180.0f / std::numbers::pi_v<float>,
                    near_plane, far_plane, &entry == &*main ? " (world camera)" : "",
                    is_view_space_camera(c) ? " (view space)" : "");
         }
      }

      const bool cut = state.previous_frame_camera_position &&
                       glm::distance(*state.previous_frame_camera_position,
                                     camera.position) > camera_cut_distance;

      if (state.recentered && cut) {
         log(Log_level::info, "VR: game camera cut, recentering.");

         // Log the next frame's cameras once per cut into gameplay (not the menu).
         if (glm::length(camera.position) > 1.0f) state.cameras_logged = false;
      }

      if (!state.recentered || cut || camera.handedness != state.world_handedness) {
         state.recentered = false;
         recenter(camera);
      }

      state.main_camera = camera;
      state.main_camera_valid = true;
      state.previous_frame_camera_position = camera.position;
   }
   else {
      state.main_camera_valid = false;
   }

   state.frame_cameras.clear();
   state.current_frame_camera = -1;
   state.frame_draws = 0;
   state.frame_world_camera_valid = false;
}

// Head pose -> eye views in the game world, anchored at the game camera.
void update_eyes(const Game_camera& camera) noexcept
{
   for (int i = 0; i < 2; ++i) {
      const XrPosef& pose = state.views[i].pose;
      const glm::vec3 position{pose.position.x, pose.position.y, pose.position.z};

      auto& eye = state.eyes[i];
      eye.right = tracking_to_world(rotate(pose.orientation, {1.0f, 0.0f, 0.0f}));
      eye.up = tracking_to_world(rotate(pose.orientation, {0.0f, 1.0f, 0.0f}));
      eye.forward = tracking_to_world(rotate(pose.orientation, {0.0f, 0.0f, -1.0f}));
      eye.position = camera.position + state.anchor_offset +
                     state.tracking_scale * tracking_to_world(position - state.tracking_reference);

      const XrFovf& fov = state.views[i].fov;
      eye.tangents = {std::tan(fov.angleLeft), std::tan(fov.angleRight),
                      std::tan(fov.angleUp), std::tan(fov.angleDown)};
   }
}

// The game camera's view space (x right, y up, z back) -> world.
auto camera_rotation(const Game_camera& camera) noexcept -> glm::mat3
{
   return glm::mat3{camera.right, camera.up, -camera.forward};
}

// Moves a draw's geometry by p' = r * p + t (r a rotation, maybe scaled): folds that into a
// world->clip matrix (rows) and maps the eye position back.
void transform_draw(const glm::mat3& r, const glm::vec3 t, std::array<glm::vec4, 4>& vp,
                    glm::vec3& eye_position) noexcept
{
   for (auto& row : vp) {
      const glm::vec3 axis{row};

      row = glm::vec4{glm::transpose(r) * axis, glm::dot(axis, t) + row.w};
   }

   eye_position = glm::inverse(r) * (eye_position - t);
}

// The first-person model (world space, in front of `camera`) moved into a hand: its
// view space position taken relative to the hand, with `grip_view` at the aim pose.
void place_in_hand(const World_pose& hand, const Game_camera& camera,
                   const glm::vec3 grip_view, std::array<glm::vec4, 4>& vp,
                   glm::vec3& eye_position) noexcept
{
   const glm::mat3 r = hand.rotation * glm::transpose(camera_rotation(camera));
   const glm::vec3 t = hand.position - hand.rotation * grip_view - r * camera.position;

   transform_draw(r, t, vp, eye_position);
}

// Midpoint of the eyes in the game world.
auto head_position_world(const Game_camera& world_camera) noexcept -> glm::vec3
{
   const auto& a = state.views[0].pose.position;
   const auto& b = state.views[1].pose.position;
   const glm::vec3 head{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};

   return world_camera.position + state.anchor_offset +
          state.tracking_scale * tracking_to_world(head - state.tracking_reference);
}

// The eyes and hands at the game camera, tracked as they are.
void plain_anchor() noexcept
{
   state.anchor_offset = {};
   state.tracking_reference = state.tracking_origin;
   state.tracking_scale = 1.0f;
}

// Head sway off: `height` (the camera's or the feet's) through a tracking filter that
// leaves out the steps' bobbing; as it is with head sway on.
auto steadied(const float height, const float dt) noexcept -> float
{
   if (settings().head_sway || dt <= 0.0f) {
      state.sway_valid = false;

      return height;
   }

   if (!state.sway_valid || std::abs(height - state.sway_height) > 1.0f) {
      state.sway_valid = true;
      state.sway_height = height;
      state.sway_rate = 0.0f;
      state.sway_camera_height = height;

      return height;
   }

   // A critically damped tracking filter: follows a steady climb without lag.
   const float omega = 2.0f * std::numbers::pi_v<float> * head_sway_filter_hz;
   const float error = height - state.sway_height;

   state.sway_rate += omega * omega * error * dt;
   state.sway_height += (state.sway_rate + 2.0f * head_sway_filter_damping * omega * error) * dt;

   // Jumps and falls: kept close, at the camera's own rate.
   if (std::abs(height - state.sway_height) > head_sway_max_offset) {
      state.sway_height = height - std::copysign(head_sway_max_offset, height - state.sway_height);
      state.sway_rate = (height - state.sway_camera_height) / dt;
   }

   state.sway_camera_height = height;

   return state.sway_height;
}

// The eyes on the soldier's feet (`feet_height` steadied) at the character's eye height,
// the player's motion relative to their own eye height, scaled to the character's with
// body_scale full. The game camera swings with the aim; the feet don't.
void anchor_body(const Game_camera& camera, const melee::Player_soldier& soldier,
                 const float feet_height, const float dt) noexcept
{
   const Settings& s = settings();
   const bool on_foot = soldier.state >= 0 && soldier.state < body_states;

   // The head's position in the tracking space (the eyes' midpoint).
   const glm::vec3 head{(state.views[0].pose.position.x + state.views[1].pose.position.x) * 0.5f,
                        (state.views[0].pose.position.y + state.views[1].pose.position.y) * 0.5f,
                        (state.views[0].pose.position.z + state.views[1].pose.position.z) * 0.5f};

   const bool seated = s.play_mode == Play_mode::seated;

   // The player's standing eye height, until Measure height takes it: risen to a head
   // held higher (see player_eye_rise_seconds).
   if (!seated && !s.player_height_measured && state.views_valid && state.floor_valid &&
       dt > 0.0f) {
      const float height = head.y - state.floor_height;

      if (height > 0.5f && height < player_eye_max_height) {
         if (state.player_eye_measured <= 0.0f) {
            state.player_eye_measured = height;
         }
         else if (height > state.player_eye_measured) {
            state.player_eye_measured += (height - state.player_eye_measured) *
                                         (1.0f - std::exp(-dt / player_eye_rise_seconds));
         }

         if (std::abs(state.player_eye_measured - state.player_eye_logged) > 0.05f) {
            state.player_eye_logged = state.player_eye_measured;

            log_fmt(Log_level::info, "VR: the player's eyes are now {:.2f} m over the floor.",
                    state.player_eye_measured);
         }
      }
   }

   // Moving with the stick: the head's offset in the room eases back over the soldier.
   if (state.views_valid && state.stick_moving && on_foot && dt > 0.0f) {
      const float follow = 1.0f - std::exp(-dt / body_follow_seconds);

      state.tracking_origin.x += (head.x - state.tracking_origin.x) * follow;
      state.tracking_origin.z += (head.z - state.tracking_origin.z) * follow;
   }

   // The character's eye height: from its head bone (the body's skeleton, whenever a
   // render of it is seen: the game's camera is as high for Yoda as for a clone), else
   // from the game's camera.
   const std::optional<float> head_bone = melee::player_head_height();

   if (head_bone) state.character_from_head = true;

   if (on_foot && std::abs(aim_camera(camera).forward.y) < body_level_aim_sine &&
       (head_bone || !state.character_from_head)) {
      const float sample =
         head_bone ? *head_bone * head_bone_eye_factor : camera.position.y - soldier.position.y;

      auto& measured = state.character_heights[soldier.state];

      if (sample > 0.2f && sample < 3.0f && !(soldier.state == 0 && state.standing_locked)) {
         if (!measured.valid) {
            log_fmt(Log_level::info,
                    "VR: the character's eyes are {:.2f} m over its feet in soldier state {} "
                    "(from the {}).",
                    sample, soldier.state, head_bone ? "head bone" : "game camera");

            measured = {.height = sample, .known = true, .valid = true};
         }
         else {
            measured.height +=
               (sample - measured.height) * (1.0f - std::exp(-dt / body_measure_seconds));
         }

         // Standing still: the standing height's samples, locked at their median.
         if (soldier.state == 0 && !state.stick_moving) {
            state.standing_samples[state.standing_sample_count++] = sample;

            if (state.standing_sample_count == body_lock_samples) {
               std::nth_element(state.standing_samples.begin(),
                                state.standing_samples.begin() + body_lock_samples / 2,
                                state.standing_samples.end());

               measured.height = state.standing_samples[body_lock_samples / 2];
               state.standing_locked = true;

               log_fmt(Log_level::info,
                       "VR: the character's standing eye height is {:.2f} m (from the {}), "
                       "kept for this life.",
                       measured.height, head_bone ? "head bone" : "game camera");
            }
         }
      }
   }

   const auto& standing = state.character_heights[0];
   const float standing_height = standing.known ? standing.height : body_default_eye_height;
   const float ease =
      state.body_anchored && dt > 0.0f ? 1.0f - std::exp(-dt / body_ease_seconds) : 1.0f;

   state.body_standing_height += (standing_height - state.body_standing_height) * ease;

   // Standing: the head over the headset's floor against the player's standing eye
   // height (measured, else the highest held, else the setting's). Seated: the head
   // where it was at Recenter / Measure height stands for that height.
   state.body_player_from_floor = !seated && state.floor_valid;
   state.body_player_eye_height =
      !seated && !s.player_height_measured && state.floor_valid && state.player_eye_measured > 0.0f
         ? state.player_eye_measured
         : s.player_eye_height;

   const float floor =
      seated ? state.seated_head_y - state.body_player_eye_height
      : state.body_player_from_floor ? state.floor_height
                                     : state.tracking_origin.y - state.body_player_eye_height;

   // How low the player's head is: the character's eyes go as low, in proportion.
   state.body_fraction = state.views_valid
                            ? std::clamp((head.y - floor) / state.body_player_eye_height,
                                         min_body_fraction, max_body_fraction)
                            : 1.0f;

   // Crouched with the game's button (not by crouching in the room): the eyes drop by
   // the character's crouch, never under body_crouch_lowest_fraction.
   float crouch_drop = 0.0f;

   state.body_soldier_state = soldier.state;

   if (on_foot && soldier.state == state.crouch_state && !state.crouch_by_body) {
      const auto& crouched = state.character_heights[state.crouch_state];

      crouch_drop =
         1.0f - (crouched.valid && state.body_standing_height > 0.0f
                    ? std::clamp(crouched.height / state.body_standing_height, 0.3f, 1.0f)
                    : body_crouch_default_fraction);
   }

   state.body_crouch_drop += (crouch_drop - state.body_crouch_drop) * ease;

   const float eye_fraction =
      state.body_crouch_drop > 0.001f
         ? std::max(state.body_fraction - state.body_crouch_drop,
                    std::min(state.body_fraction, body_crouch_lowest_fraction))
         : state.body_fraction;

   state.body_eye_height = state.body_standing_height * eye_fraction;
   state.body_anchored = true;

   // The head lands on the eyes; the eyes and hands keep their offsets from it, scaled
   // with body_scale full.
   state.tracking_reference = {state.tracking_origin.x, head.y, state.tracking_origin.z};
   state.tracking_scale =
      s.body_scale == Body_scale::full
         ? std::clamp(state.body_standing_height / state.body_player_eye_height, min_body_scale,
                      max_body_scale)
         : 1.0f;

   const glm::vec3 eyes{soldier.position.x, feet_height + state.body_eye_height,
                        soldier.position.z};

   state.anchor_offset = eyes - camera.position;

   // Never under the ground: the head kept a little over the feet.
   const float lowest = soldier.position.y + min_eye_above_feet;
   const glm::vec3 eyes_world = head_position_world(camera);

   if (eyes_world.y < lowest) state.anchor_offset.y += lowest - eyes_world.y;

   const float offset = glm::length(glm::vec2{eyes_world.x - soldier.position.x,
                                              eyes_world.z - soldier.position.z});

   state.stat_body_offset_sum += offset;
   state.stat_body_offset_max = std::max(state.stat_body_offset_max, offset);
   state.stat_body_offset_frames += 1;
}

void reset_body_measure() noexcept;

// Where the eyes and hands sit in this frame, as an offset from the game camera's
// position, from the frame's world camera (once per frame):
// - on a stationary turret, at the point the game's camera circles (the turret): turning
//   the turret swings the camera round it, which swung the player round with it;
// - on foot (body_scale), on the soldier's feet at the character's eye height;
// - with head sway off, at a height that leaves out the steps' bobbing.
void update_anchor(const Game_camera& camera) noexcept
{
   if (state.anchor_frame == state.frame_index) return;

   state.anchor_frame = state.frame_index;
   plain_anchor();

   const float dt = state.anchor_dt;

   if (melee::player_in_turret()) {
      state.sway_valid = false;
      state.body_anchored = false;
      state.turret_body = melee::player_body_on_turret();

      if (!std::exchange(state.turret_anchor, true)) {
         state.turret_projectors = glm::mat3{0.0f};
         state.turret_projected = {};
         state.turret_lines = 0;
         state.turret_entry = camera.position;
         state.turret_pivot_valid = false;
      }

      // The turret's aim (the game's own camera rotation), not the head's it's drawn with.
      const glm::vec3 forward = glm::normalize(aim_camera(camera).forward);
      const glm::mat3 projector = glm::mat3{1.0f} - glm::outerProduct(forward, forward);

      state.turret_projectors += projector;
      state.turret_projected += projector * camera.position;
      state.turret_lines += 1;

      const glm::mat3 mean = state.turret_projectors / static_cast<float>(state.turret_lines);

      if (state.turret_lines >= turret_min_lines && glm::determinant(mean) > turret_min_spread) {
         const glm::vec3 pivot = glm::inverse(state.turret_projectors) * state.turret_projected;

         if (glm::distance(pivot, camera.position) < turret_max_pivot_distance) {
            if (!state.turret_pivot_valid) {
               log_fmt(Log_level::info,
                       "VR: the turret's camera circles ({:.1f}, {:.1f}, {:.1f}), {:.1f} m from "
                       "it: the eyes go there.",
                       pivot.x, pivot.y, pivot.z, glm::distance(pivot, camera.position));
            }

            state.turret_pivot = std::exchange(state.turret_pivot_valid, true)
                                    ? state.turret_pivot + (pivot - state.turret_pivot) *
                                                              std::min(1.0f, dt * turret_pivot_ease_rate)
                                    : pivot;
         }
      }

      // Moved by the player's offset (VR panel), along the turret's heading.
      const Settings& s = settings();
      const glm::vec3 turret_right = horizontal(aim_camera(camera).right);
      const glm::vec3 turret_forward =
         camera.handedness * glm::cross(turret_right, world_up_axis);

      state.anchor_offset =
         (state.turret_pivot_valid ? state.turret_pivot : state.turret_entry) - camera.position +
         turret_right * s.turret_eye_right + world_up_axis * s.turret_eye_up +
         turret_forward * s.turret_eye_forward;

      return;
   }

   if (std::exchange(state.turret_anchor, false)) {
      log_fmt(Log_level::info,
              "VR: the player's soldier on the turret: {} skinned draws left out of {} "
              "skinned draws (nearest {:.2f} m from it).",
              std::exchange(state.stat_turret_body_draws, 0),
              std::exchange(state.stat_turret_skinned_draws, 0),
              std::exchange(state.stat_turret_nearest_skinned, 1e9f));
   }

   state.turret_body.reset();
   state.turret_own_taken = false;

   const std::optional<melee::Player_soldier> soldier =
      settings().body_scale != Body_scale::off ? melee::player_soldier() : std::nullopt;
   const bool body = soldier && glm::distance(soldier->position, camera.position) <
                                   body_max_camera_distance;

   if (!soldier) reset_body_measure();

   if (body != state.sway_on_feet) {
      state.sway_on_feet = body;
      state.sway_valid = false;
   }

   if (!body) {
      state.body_anchored = false;
      state.anchor_offset.y = steadied(camera.position.y, dt) - camera.position.y;

      return;
   }

   anchor_body(camera, *soldier, steadied(soldier->position.y, dt), dt);
}

// Turn the game camera towards the right hand's aim (controller aim) or the head with
// injected mouse movement. The game fires and culls along its camera; the eyes only
// take the camera's position, so turning it doesn't move what the player sees.
void update_mouse_follow() noexcept
{
   if (!state.camera_seen_last_frame || !state.main_camera_valid || !state.recentered ||
       !state.views_valid) {
      return;
   }

   // A vehicle is steered with the right stick alone (see update_input): turning its
   // camera towards the hand would steer it too. A droideka on its legs aims like a
   // soldier (rolled up, it's steered).
   if (melee::player_in_vehicle() && !(melee::player_on_droideka() && !state.vehicle_controls)) {
      state.mouse_remainder = {};

      return;
   }

   // With a melee weapon the hand using the ability aims (force powers), along its
   // controller; otherwise the head (see melee_aim_linger_seconds).
   const bool has_melee = melee::player_has_melee();
   const bool left_aims = has_melee && state.melee_aim_hand == 0 && state.left_aim_valid;
   const bool right_hand_aims = has_melee && state.melee_aim_hand == 1 && state.aim_valid;
   const glm::vec3 head_forward =
      tracking_to_world(rotate(state.views[0].pose.orientation, glm::vec3{0.0f, 0.0f, -1.0f}));
   glm::vec3 target_forward =
      left_aims         ? -aim_pose_world(state.main_camera, state.left_aim_pose).rotation[2]
      : right_hand_aims ? -aim_pose_world(state.main_camera, state.aim_pose).rotation[2]
      : has_melee       ? head_forward
      : state.aim_valid ? -weapon_pose_world(state.main_camera).rotation[2]
                        : head_forward;

   // A grenade thrown: the camera turns to the throw until the grenade has gone.
   if (state.throw_pending || state.throw_holding || state.throw_hold_left > 0) {
      target_forward = glm::normalize(tracking_to_world(state.throw_direction));
   }
   // A turret turns left and right with the head and up and down with the right hand;
   // the hand pointing aside from the head turns it on by up to turret_hand_yaw.
   else if (melee::player_in_turret() && state.aim_valid) {
      const glm::vec3 hand_forward = -weapon_pose_world(state.main_camera).rotation[2];
      const glm::vec3 head_heading = horizontal(head_forward);
      const glm::vec3 hand_heading = horizontal(hand_forward);
      const float pitch_sin = std::clamp(glm::dot(hand_forward, world_up_axis), -1.0f, 1.0f);
      glm::vec3 heading = head_heading;

      if (head_heading != glm::vec3{} && hand_heading != glm::vec3{}) {
         const glm::vec3 aside =
            hand_heading - glm::dot(hand_heading, head_heading) * head_heading;

         if (glm::length(aside) > 1e-4f) {
            const float apart =
               std::acos(std::clamp(glm::dot(head_heading, hand_heading), -1.0f, 1.0f));
            const float turn = std::min(apart, glm::radians(settings().turret_hand_yaw));

            heading = head_heading * std::cos(turn) + glm::normalize(aside) * std::sin(turn);
         }
      }

      if (heading != glm::vec3{}) {
         target_forward = heading * std::sqrt(1.0f - pitch_sin * pitch_sin) +
                          world_up_axis * pitch_sin;
      }
   }

   const Game_camera& camera = state.main_camera;
   const glm::vec3 camera_right = horizontal(camera.right);
   const glm::vec3 camera_forward = camera.handedness * glm::cross(camera_right, world_up_axis);

   if (camera_right == glm::vec3{}) return;

   // Measured in the camera's own frame: positive yaw = target is to the camera's right.
   const float yaw_error = std::atan2(glm::dot(target_forward, camera_right),
                                      glm::dot(target_forward, camera_forward));
   const float pitch_error =
      std::asin(std::clamp(glm::dot(target_forward, world_up_axis), -1.0f, 1.0f)) -
      std::asin(std::clamp(glm::dot(camera.forward, world_up_axis), -1.0f, 1.0f));

   state.camera_aim_error = std::max(std::abs(yaw_error), std::abs(pitch_error));

   glm::vec2 counts =
      glm::clamp(glm::vec2{yaw_error, -pitch_error} * mouse_follow_gain +
                    state.mouse_remainder,
                 -mouse_follow_max_counts, mouse_follow_max_counts);

   if (melee::player_in_turret() && !state.throw_pending && !state.throw_holding) {
      const auto gentle = [](const float error) {
         return std::abs(error) < turret_follow_dead_zone ? 0.0f : error;
      };

      counts = glm::clamp(glm::vec2{gentle(yaw_error), gentle(-pitch_error)} * turret_follow_gain +
                             state.mouse_remainder,
                          -turret_follow_max_counts, turret_follow_max_counts);
   }
   const glm::vec2 whole = glm::trunc(counts);

   state.mouse_remainder = counts - whole;
   mouse_dx += static_cast<LONG>(whole.x);
   mouse_dy += static_cast<LONG>(whole.y);
}

// --- Game camera frustum ------------------------------------------------------
//
// The game culls (and builds its projection) with its own camera, which follows the
// right hand, so the world the head looks at is missing where it's outside the gun's
// view. The frustum of the engine's camera (RedCamera, a pvs::BaseCamera) is widened to
// also hold both eyes' views. From BattlefrontII.exe (2017 Steam/GOG build):
//
// - +0x30 / +0x50 / +0x60: world matrix rows right, back (the camera looks down -back)
//   and position.
// - +0x138 / +0x13c: tan of the half field of view, horizontal / vertical; +0x140: zoom.
// - FUN_006cc090 (this in ECX) derives the effective tangents (+0x144 = +0x138 / zoom,
//   +0x148) and the frustum planes from them, then rebuilds the projection and the
//   culling frustum (FUN_006cc160). Called from the five call sites below whenever the
//   field of view, zoom or clip planes change: every frame for the gameplay camera.
//
// The hook widens +0x138 / +0x13c before it runs, and they stay widened: the projection
// is rebuilt from them (FUN_006cbcd0) on every later pose change too, so putting the
// game's back made the projection - and with it the culling - narrow again. The game
// eases its field of view from what it reads back towards its target, so once the
// head looks along the gun again it narrows back by itself. The widened projection
// reaches the eye cameras only through the depth planes, which don't change.
//
// Widening alone can't cover a head turned more than ~80 degrees from the gun (one
// perspective camera sees less than 180 degrees), so the gameplay camera is also drawn
// and culled looking where the head looks:
//
// - FUN_006cc160 (this in ECX) builds the view-projection (+0xf0 = view +0x70 *
//   projection +0xb0) and the culling frustum from the world matrix rows. It's called
//   from FUN_006ccc10 (pose set: view +0x70 rebuilt from the rows first) and from
//   FUN_006cc090 (field of view set), at the two call sites below.
// - Its hook writes the head's rotation into the rows (position kept) and the view
//   matrix. The rows keep it until the frame is presented: the culling reads the rows
//   themselves when it runs (putting the game's back right after the call changed
//   nothing). At Present the game's rotation goes back (restore_aim_rows), so the next
//   frame's game code (aim, fire) sees the gun's direction. The game's rotation is kept
//   as this frame's aim (engine_aim) for mouse-follow, recentring, the HUD and the
//   weapon.

constexpr std::uintptr_t exe_update_frustum = 0x006cc090;
constexpr std::uintptr_t exe_update_frustum_calls[] = {0x006cbe96, 0x006cbee6, 0x006cbf4d,
                                                       0x006cbfb3, 0x006cbfd0};
constexpr std::uintptr_t exe_build_view = 0x006cc160;
constexpr std::uintptr_t exe_build_view_calls[] = {0x006cc148, 0x006ccc74};

// The frustum must hold the eyes' views a little beyond their edges (head turns between
// the game's update and the display) and can't open wider than this.
constexpr float frustum_margin_degrees = 12.0f;
constexpr float frustum_max_half_angle_degrees = 80.0f;

// The engine camera is the gameplay one when it's this close to the last world camera.
constexpr float frustum_camera_match_distance = 3.0f;

auto engine_vec3(const std::uint8_t* camera, const int offset) noexcept -> glm::vec3
{
   const auto* f = reinterpret_cast<const float*>(camera + offset);

   return {f[0], f[1], f[2]};
}

auto module_directory() noexcept -> std::wstring;

auto engine_rows(const std::uint8_t* camera) noexcept -> Camera_rows
{
   return {engine_vec3(camera, 0x30), engine_vec3(camera, 0x40), engine_vec3(camera, 0x50)};
}

// The rotation the gameplay camera is drawn and culled with: the head's (or the game's
// own with the head camera off), or nullopt for any other camera / outside gameplay.
auto gameplay_camera_rows(const std::uint8_t* camera,
                          const bool head = !state.head_camera_disabled) noexcept
   -> std::optional<Camera_rows>
{
   if (!stereo_frame() || !state.recentered || state.menu_frame ||
       !state.main_camera_valid) {
      return std::nullopt;
   }

   if (glm::distance(engine_vec3(camera, 0x60), state.main_camera.position) >
       frustum_camera_match_distance) {
      return std::nullopt;
   }

   const Camera_rows game = engine_rows(camera);

   if (glm::length(game.right) < 1e-3f || glm::length(game.back) < 1e-3f) {
      return std::nullopt;
   }

   if (!head) return game;

   // Midway between the eyes' orientations is the left eye's for a headset (both eyes
   // share their rotation).
   const XrQuaternionf& q = state.views[0].pose.orientation;
   const glm::vec3 right = glm::normalize(tracking_to_world(rotate(q, {1.0f, 0.0f, 0.0f})));
   const glm::vec3 back = -glm::normalize(tracking_to_world(rotate(q, {0.0f, 0.0f, -1.0f})));

   // Same handedness as the game's rows.
   const float handedness =
      glm::dot(glm::cross(game.right, game.up), game.back) < 0.0f ? -1.0f : 1.0f;

   return Camera_rows{right, handedness * glm::cross(back, right), back};
}

// Tangents of the half field of view (horizontal, vertical) the engine camera needs to
// hold both eyes' views as well as its own (`game_tan`), or nullopt to leave it.
auto culling_tangents(const std::uint8_t* camera, const glm::vec2 game_tan,
                      const bool head = !state.head_camera_disabled) noexcept
   -> std::optional<glm::vec2>
{
   const std::optional<Camera_rows> rows = gameplay_camera_rows(camera, head);

   if (!rows) return std::nullopt;

   // Only the magnitudes of x / z and y / z matter: the up axis' sign doesn't.
   const glm::vec3 x_axis = glm::normalize(rows->right);
   const glm::vec3 forward = -glm::normalize(rows->back);
   const glm::vec3 y_axis = glm::normalize(glm::cross(forward, x_axis));

   const float max_angle =
      frustum_max_half_angle_degrees * std::numbers::pi_v<float> / 180.0f;
   const float margin = frustum_margin_degrees * std::numbers::pi_v<float> / 180.0f;
   const float max_tan = std::tan(max_angle);

   glm::vec2 needed{0.0f, 0.0f};

   // The view frustum is the cone of its corner rays: |x / z| and |y / z| peak at them.
   for (const XrView& view : state.views) {
      const glm::vec3 eye_right = tracking_to_world(rotate(view.pose.orientation, {1.0f, 0.0f, 0.0f}));
      const glm::vec3 eye_up = tracking_to_world(rotate(view.pose.orientation, {0.0f, 1.0f, 0.0f}));
      const glm::vec3 eye_forward =
         tracking_to_world(rotate(view.pose.orientation, {0.0f, 0.0f, -1.0f}));

      for (const float tx : {std::tan(view.fov.angleLeft), std::tan(view.fov.angleRight)}) {
         for (const float ty : {std::tan(view.fov.angleDown), std::tan(view.fov.angleUp)}) {
            const glm::vec3 corner = glm::normalize(eye_forward + tx * eye_right + ty * eye_up);
            const float z = glm::dot(corner, forward);

            // Behind or beside the camera: open as wide as the engine allows.
            if (z < std::cos(max_angle)) return glm::vec2{max_tan, max_tan};

            needed = glm::max(needed, glm::vec2{std::abs(glm::dot(corner, x_axis)),
                                                std::abs(glm::dot(corner, y_axis))} /
                                         z);
         }
      }
   }

   const auto widen = [&](const float tan) {
      return std::tan(std::min(std::atan(tan) + margin, max_angle));
   };

   return glm::max(game_tan, glm::vec2{widen(needed.x), widen(needed.y)});
}

// True for memory on this thread's stack (the engine's temporary camera copies).
bool on_stack(const void* const address) noexcept
{
   const auto* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());

   return address >= tib->StackLimit && address < tib->StackBase;
}

// Gives a gameplay camera the head's rotation and a field of view around the head that
// holds both eyes' views, and rebuilds its frustum (projection, culling planes and
// bounds; FUN_006cc090). The engine culls with these until Present puts the game's back
// (end_head_cull); the view matrix it draws with stays the game's. `rebuild` redoes the
// frustum of a camera that has the head's rotation already (the game changed its clip
// planes). False for other cameras and when there was nothing to do.
bool head_cull_camera(std::uint8_t* const camera, const bool rebuild) noexcept
{
   const std::optional<Camera_rows> rows = gameplay_camera_rows(camera, true);
   const float zoom = *reinterpret_cast<const float*>(camera + 0x140);

   if (!rows || zoom <= 0.0f) return false;

   auto* const m = reinterpret_cast<float*>(camera + 0x30);
   float& tan_x = *reinterpret_cast<float*>(camera + 0x138);
   float& tan_y = *reinterpret_cast<float*>(camera + 0x13c);

   const bool culled = std::any_of(state.head_culls.begin(), state.head_culls.end(),
                                   [&](const State::Head_cull& cull) {
                                      return cull.camera == camera &&
                                             std::memcmp(m, cull.head_rows.data(),
                                                         sizeof(cull.head_rows)) == 0;
                                   });

   if (culled && !rebuild) return false;

   State::Head_cull cull{camera};

   if (!culled) {
      std::memcpy(cull.game_rows.data(), m, sizeof(cull.game_rows));
      cull.game_tan = {tan_x, tan_y};
      // Only the gameplay cameras the game set up before drawing are its own lasting
      // ones; others are temporary copies (stack or heap), likely freed by Present.
      cull.restore = std::find(state.cull_cameras.begin(), state.cull_cameras.end(),
                               camera) != state.cull_cameras.end();

      // The game's rotation is this frame's aim (the camera is drawn with the head's
      // view from here on, see below).
      if (cull.restore && state.engine_aim_frame != state.frame_index) {
         state.engine_aim = engine_rows(camera);
         state.engine_aim_frame = state.frame_index;
      }

      const std::array<glm::vec3, 3> axes{rows->right, rows->up, rows->back};

      for (int row = 0; row < 3; ++row) {
         m[row * 4 + 0] = axes[row].x;
         m[row * 4 + 1] = axes[row].y;
         m[row * 4 + 2] = axes[row].z;
      }

      // Head view: the view matrix the frame is drawn with looks where the head looks
      // too. What the game orients to its camera while drawing (camera-facing
      // billboards, flares, the sun, shadow cascades) then follows the head instead of
      // the gun. The eye cameras don't depend on it (they take the camera's position
      // only). Built as FUN_00411dd0 does (D3DX row vectors). Only the game's own
      // gameplay cameras: copies get their matrices from their rows, and a reflection's
      // mirrored copy must keep its own.
      cull.view_written = settings().head_view && cull.restore;

      if (cull.view_written) {
         auto* const view = reinterpret_cast<float*>(camera + 0x70);
         const glm::vec3 position = engine_vec3(camera, 0x60);

         std::memcpy(cull.game_view.data(), view, sizeof(cull.game_view));

         for (int column = 0; column < 3; ++column) {
            view[0 * 4 + column] = axes[column].x;
            view[1 * 4 + column] = axes[column].y;
            view[2 * 4 + column] = axes[column].z;
            view[3 * 4 + column] = -glm::dot(position, axes[column]);
         }

         view[3] = view[7] = view[11] = 0.0f;
         view[15] = 1.0f;

         std::memcpy(cull.head_view.data(), view, sizeof(cull.head_view));
      }

      // Culled from the eyes: the game's camera is as high over the feet for every
      // character (1.72 m, Yoda's too), and the eyes of a smaller body are far under it,
      // so what was close to them (Dagobah's roots, the saber in the hand) fell outside
      // the frustum from up there. The position moves only after the view matrix took
      // the game's (it's drawn with that; the eye cameras are placed from it), and the
      // frustum's planes and bounds are rebuilt from it below.
      cull.position_written = false;

      // Already moved this frame (the game put its rows back, not its position): kept
      // where it is. The eyes are placed from the camera's position, so working them
      // out again from the moved one would move it once more.
      const auto moved = std::find_if(state.head_culls.begin(), state.head_culls.end(),
                                      [&](const State::Head_cull& earlier) {
                                         return earlier.camera == camera &&
                                                earlier.position_written &&
                                                engine_vec3(camera, 0x60) ==
                                                   earlier.head_position;
                                      });

      if (moved != state.head_culls.end()) {
         cull.game_position = moved->game_position;
         cull.head_position = moved->head_position;
         cull.position_written = true;
      }
      else if (cull.restore && settings().cull_from_eyes && state.body_anchored &&
               state.views_valid && state.tracking_scale < cull_eyes_max_scale) {
         auto* const position = reinterpret_cast<float*>(camera + 0x60);
         Game_camera at_engine{};

         at_engine.position = engine_vec3(camera, 0x60);

         const glm::vec3 head = head_position_world(at_engine);

         if (glm::distance(head, at_engine.position) < cull_eyes_max_offset) {
            cull.game_position = at_engine.position;
            cull.head_position = head;
            cull.position_written = true;

            position[0] = head.x;
            position[1] = head.y;
            position[2] = head.z;
         }
      }
   }

   // The eyes' field of view around the head (not the gun's widened one).
   if (const std::optional<glm::vec2> wide =
          culling_tangents(camera, glm::vec2{0.0f, 0.0f}, true)) {
      tan_x = wide->x * zoom;
      tan_y = wide->y * zoom;
   }

   reinterpret_cast<void(__fastcall*)(std::uint8_t*)>(state.update_frustum)(camera);

   if (!culled) {
      std::memcpy(cull.head_rows.data(), m, sizeof(cull.head_rows));
      state.head_culls.push_back(cull);
   }

   return true;
}

void __fastcall update_frustum_hook(std::uint8_t* const camera) noexcept
{
   const auto original =
      reinterpret_cast<void(__fastcall*)(std::uint8_t*)>(state.update_frustum);

   // While the frame is drawn, a gameplay camera's frustum the game rebuilds (the far
   // scene's clip planes, copies of the camera) looks where the head looks too.
   if (state.head_cull_done && !state.head_cull_disabled && head_cull_camera(camera, true)) {
      state.stat_frustum_head_culls += 1;

      return;
   }

   float& tan_x = *reinterpret_cast<float*>(camera + 0x138);
   float& tan_y = *reinterpret_cast<float*>(camera + 0x13c);
   const float zoom = *reinterpret_cast<const float*>(camera + 0x140);

   const std::optional<glm::vec2> wide =
      zoom > 0.0f ? culling_tangents(camera, glm::vec2{tan_x, tan_y} / zoom) : std::nullopt;

   if (!wide) {
      if (state.session_running) state.stat_frustum_other += 1;

      original(camera);

      return;
   }

   const float game_tan_x = tan_x;
   const float game_tan_y = tan_y;

   tan_x = wide->x * zoom;
   tan_y = wide->y * zoom;

   original(camera);

   state.stat_frustum_widened += 1;
   state.widened_camera = camera;
   state.widened_game_tan = {game_tan_x, game_tan_y};
   state.widened_written_tan = {tan_x, tan_y};

   // A gameplay camera: culled with the head's rotation while the frame is drawn.
   if (std::find(state.cull_cameras.begin(), state.cull_cameras.end(), camera) ==
          state.cull_cameras.end() &&
       state.cull_cameras.size() < 4) {
      state.cull_cameras.push_back(camera);
   }
   state.stat_widest_tan_x = std::max(state.stat_widest_tan_x, wide->x);
   state.stat_widest_tan_y = std::max(state.stat_widest_tan_y, wide->y);

   if (state.frustum_logged < 3) {
      state.frustum_logged += 1;

      log_fmt(Log_level::info,
              "VR: game camera frustum widened from half-angles {:.0f} x {:.0f} to {:.0f} x "
              "{:.0f} deg (zoom {:.2f}).",
              std::atan(game_tan_x / zoom) * 180.0f / std::numbers::pi_v<float>,
              std::atan(game_tan_y / zoom) * 180.0f / std::numbers::pi_v<float>,
              std::atan(wide->x) * 180.0f / std::numbers::pi_v<float>,
              std::atan(wide->y) * 180.0f / std::numbers::pi_v<float>, zoom);
   }
}

void __fastcall build_view_hook(std::uint8_t* const camera) noexcept
{
   const auto original = reinterpret_cast<void(__fastcall*)(std::uint8_t*)>(state.build_view);

   const std::optional<Camera_rows> rows =
      state.head_camera_disabled ? std::nullopt : gameplay_camera_rows(camera);

   if (!rows) {
      original(camera);

      return;
   }

   auto* const m = reinterpret_cast<float*>(camera + 0x30); // rows right, up, back, position
   auto* const view = reinterpret_cast<float*>(camera + 0x70);

   // The game's rotation is this frame's aim - unless the rows still hold the head's
   // from an earlier call this frame (a field of view change without a pose change).
   const bool rows_are_head =
      camera == state.head_camera &&
      std::memcmp(m, state.head_rows_written.data(), sizeof(state.head_rows_written)) == 0;

   if (!rows_are_head) {
      std::memcpy(state.aim_rows_saved.data(), m, sizeof(state.aim_rows_saved));
      state.engine_aim = engine_rows(camera);
   }

   state.engine_aim_frame = state.frame_index;

   const glm::vec3 position = engine_vec3(camera, 0x60);
   const std::array<glm::vec3, 3> axes{rows->right, rows->up, rows->back};

   for (int row = 0; row < 3; ++row) {
      m[row * 4 + 0] = axes[row].x;
      m[row * 4 + 1] = axes[row].y;
      m[row * 4 + 2] = axes[row].z;
   }

   // The view matrix as FUN_00411dd0 builds it (D3DX row vectors): the rotation
   // transposed, then the position taken into view space and negated.
   for (int column = 0; column < 3; ++column) {
      view[0 * 4 + column] = axes[column].x;
      view[1 * 4 + column] = axes[column].y;
      view[2 * 4 + column] = axes[column].z;
      view[3 * 4 + column] = -glm::dot(position, axes[column]);
   }

   view[3] = view[7] = view[11] = 0.0f;
   view[15] = 1.0f;

   original(camera);

   // The head's rotation stays in the rows while the frame is culled and drawn (the
   // culling reads them, not just the matrices built here); restore_aim_rows puts the
   // game's back before the next frame's game code runs.
   std::memcpy(state.head_rows_written.data(), m, sizeof(state.head_rows_written));
   state.head_camera = camera;

   state.stat_head_camera_builds += 1;
}

// Head culling: the game culls with its camera's rows (and field of view) as they are
// when it culls, and reads the same rows for aiming. So the gameplay cameras take the
// head's rotation (and a field of view around it holding both eyes) only from the
// frame's first BeginScene - the game's update is done by then - until Present. The
// view-projection the game draws with keeps its own view (FUN_006cc090 rebuilds the
// projection and frustum from the field of view and rows, not the view matrix), so
// the eye cameras are unaffected.
//
// The engine has no single culling function to patch: the visibility tests are inlined
// in every renderer (objects, terrain, foliage, particles, ...) and read the current
// render camera's rows, tangents and bounds directly. So the camera data is what
// changes. Cameras the game sets up while drawing get the same: ones made current for
// a pass (set_render_camera_hook) and frustums rebuilt then (update_frustum_hook).
void apply_head_cull() noexcept
{
   if (state.head_cull_done || state.head_cull_disabled || !state.update_frustum) return;

   state.head_cull_done = true;

   for (std::uint8_t* const camera : state.cull_cameras) {
      if (head_cull_camera(camera, false)) state.stat_head_culls += 1;
   }
}

// FUN_006b3420 (cdecl): makes `camera` the one the renderer draws and culls the next
// passes with (the global at 0x7f58e0). Called for every pass: world, far scene (its
// own near plane), first-person model, and with temporary copies of the camera.
constexpr std::uintptr_t exe_set_render_camera = 0x006b3420;
constexpr std::uintptr_t exe_set_render_camera_calls[] = {
   0x00576f98, 0x005771a6, 0x006c10bc, 0x006c144e, 0x0044e94b, 0x006bd382,
   0x006bd5c7, 0x006bd6cf, 0x006bdb67, 0x006bdf48, 0x006be9f8};

// The call sites in the frame's renderer (FUN_006bd080, its far scene FUN_006bd940 and
// reflections FUN_006bdc90). The game's update is done once they run, and the renderer
// culls before its first BeginScene already (sector visibility, reflections), so the
// first of them starts head culling.
constexpr std::uintptr_t exe_renderer_set_render_camera_calls[] = {
   0x006bd382, 0x006bd5c7, 0x006bd6cf, 0x006bdb67, 0x006bdf48, 0x006be9f8};

void __cdecl set_render_camera_hook(std::uint8_t* const camera) noexcept
{
   if (camera && !state.head_cull_disabled && gameplay_camera_rows(camera, true)) {
      const auto site = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - 5 -
                        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) +
                        exe_image_base;

      if (!state.head_cull_done &&
          std::find(std::begin(exe_renderer_set_render_camera_calls),
                    std::end(exe_renderer_set_render_camera_calls),
                    site) != std::end(exe_renderer_set_render_camera_calls)) {
         apply_head_cull();

         if (state.head_cull_done) state.stat_head_cull_started_early += 1;
      }

      if (!state.head_cull_done) {
         state.stat_render_cameras_early += 1;
      }
      else {
         state.stat_render_cameras += 1;

         if (head_cull_camera(camera, false)) {
            state.stat_late_head_culls += 1;

            // Where the game draws with a camera head culling hadn't reached yet.

            if (std::find(state.late_cull_sites.begin(), state.late_cull_sites.end(),
                          site) == state.late_cull_sites.end() &&
                state.late_cull_sites.size() < 16) {
               state.late_cull_sites.push_back(site);

               log_fmt(Log_level::info,
                       "VR: render camera {} at 0x{:08x} (made current from 0x{:08x}) "
                       "culled with the head's rotation there.",
                       on_stack(camera) ? "copy" : "object",
                       reinterpret_cast<std::uintptr_t>(camera), site);
            }
         }
      }
   }

   reinterpret_cast<void(__cdecl*)(std::uint8_t*)>(state.set_render_camera)(camera);
}

// FUN_006e42f0 (stdcall: ret 4), called once per frame from the renderer (0x6bd39c) right after
// the world camera is made current, before the first BeginScene: it hands the camera to
// pvs::Manager (vtable 0x78365c, entry FUN_006e1d40 -> FUN_006e19f0), which walks the
// map's sectors through their portals and marks what is visible. Objects in sectors it
// doesn't reach aren't drawn at all, wherever the render camera looks.
// - Its frustum (FUN_006e0670) is a half-space in front of the camera, from the
//   camera's world rows (pvs::BaseCamera at RedCamera+0x24, vtable 0x772e54: [0]
//   position +0x60, [2] rows +0x30).
// - Each portal (FUN_006dfa40) is moved into view space with the camera's VIEW matrix
//   (+0x70, vtable [1]), projected as (x / -z, y / -z) and clipped to a rectangle that
//   starts at [-1, 1]: a 45 degree half-angle around the view direction.
// VR keeps the view matrix the game's (aim), so the portals were tested along the gun.
// Here the camera gets the head's rows and a view matrix looking where the head looks
// for the call only, its x and y scaled so [-1, 1] holds both eyes' field of view.
constexpr std::uintptr_t exe_pvs_camera = 0x006e42f0;
constexpr std::uintptr_t exe_pvs_camera_calls[] = {0x006bd39c};
constexpr std::uintptr_t exe_pvs_enabled = 0x007df864; // byte, FUN_006e1d40 checks it

void __stdcall pvs_camera_hook(std::uint8_t* const camera) noexcept
{
   const auto original = reinterpret_cast<void(__stdcall*)(std::uint8_t*)>(state.pvs_camera);

   state.stat_pvs_updates += 1;
   state.stat_pvs_on =
      *reinterpret_cast<const std::uint8_t*>(
         exe_pvs_enabled - exe_image_base +
         reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))) != 0;

   const std::optional<Camera_rows> rows =
      camera && !state.head_cull_disabled ? gameplay_camera_rows(camera, true) : std::nullopt;

   if (!rows) {
      original(camera);

      return;
   }

   auto* const m = reinterpret_cast<float*>(camera + 0x30); // rows right, up, back, position
   auto* const view = reinterpret_cast<float*>(camera + 0x70);

   std::array<float, 12> saved_rows;
   std::array<float, 16> saved_view;

   std::memcpy(saved_rows.data(), m, sizeof(saved_rows));
   std::memcpy(saved_view.data(), view, sizeof(saved_view));

   // The sectors are walked from the game's camera even where head culling moved it to
   // a small character's eyes (head_cull_camera): which sector holds the camera decides
   // what's drawn at all, and the eyes may stand in another one.
   const auto moved = std::find_if(state.head_culls.begin(), state.head_culls.end(),
                                   [&](const State::Head_cull& cull) {
                                      return cull.camera == camera && cull.position_written &&
                                             engine_vec3(camera, 0x60) == cull.head_position;
                                   });
   auto* const camera_position = reinterpret_cast<float*>(camera + 0x60);
   // Copied: the call may head-cull more cameras (the list grows).
   const std::optional<glm::vec3> eyes =
      moved != state.head_culls.end() ? std::optional{moved->head_position} : std::nullopt;

   if (eyes) {
      camera_position[0] = moved->game_position.x;
      camera_position[1] = moved->game_position.y;
      camera_position[2] = moved->game_position.z;
   }

   const glm::vec3 position = engine_vec3(camera, 0x60);
   const std::array<glm::vec3, 3> axes{rows->right, rows->up, rows->back};

   // The eyes' field of view around the head; [-1, 1] after scaling covers it.
   const std::optional<glm::vec2> tan = culling_tangents(camera, glm::vec2{1.0f, 1.0f}, true);
   const float widest = tan ? std::max(tan->x, tan->y) : 1.0f;
   const float scale = 1.0f / std::max(widest, 1.0f);

   for (int row = 0; row < 3; ++row) {
      m[row * 4 + 0] = axes[row].x;
      m[row * 4 + 1] = axes[row].y;
      m[row * 4 + 2] = axes[row].z;
   }

   // As FUN_00411dd0 builds it (D3DX row vectors), view-space x and y scaled.
   for (int column = 0; column < 3; ++column) {
      const float s = column < 2 ? scale : 1.0f;

      view[0 * 4 + column] = axes[column].x * s;
      view[1 * 4 + column] = axes[column].y * s;
      view[2 * 4 + column] = axes[column].z * s;
      view[3 * 4 + column] = -glm::dot(position, axes[column]) * s;
   }

   view[3] = view[7] = view[11] = 0.0f;
   view[15] = 1.0f;

   original(camera);

   std::memcpy(m, saved_rows.data(), sizeof(saved_rows));
   std::memcpy(view, saved_view.data(), sizeof(saved_view));

   if (eyes) {
      camera_position[0] = eyes->x;
      camera_position[1] = eyes->y;
      camera_position[2] = eyes->z;
   }

   state.stat_pvs_head_updates += 1;
   state.stat_pvs_half_angle = std::max(state.stat_pvs_half_angle,
                                        std::atan(1.0f / scale) * 180.0f /
                                           std::numbers::pi_v<float>);
}

// Where the engine camera at `game_position` stands while the frame is drawn: the eyes
// when head culling moved it there.
auto sky_centre(const glm::vec3& game_position) noexcept -> glm::vec3
{
   for (const State::Head_cull& cull : state.head_culls) {
      if (cull.position_written && glm::distance(cull.game_position, game_position) < 0.01f) {
         return cull.head_position;
      }
   }

   return game_position;
}

// At Present: the game's rotation and field of view back (where the game hasn't set
// its own since), latest first. Temporary copies are left alone (see head_cull_camera).
void end_head_cull() noexcept
{
   const auto update_frustum =
      reinterpret_cast<void(__fastcall*)(std::uint8_t*)>(state.update_frustum);

   for (auto it = state.head_culls.rbegin(); it != state.head_culls.rend(); ++it) {
      const State::Head_cull& cull = *it;
      auto* const m = reinterpret_cast<float*>(cull.camera + 0x30);

      if (!cull.restore) continue;

      // The game's position back even where it has set its own rows since: left at the
      // eyes, the next frame's game code would start from there.
      if (cull.position_written && engine_vec3(cull.camera, 0x60) == cull.head_position) {
         auto* const position = reinterpret_cast<float*>(cull.camera + 0x60);

         position[0] = cull.game_position.x;
         position[1] = cull.game_position.y;
         position[2] = cull.game_position.z;
      }

      if (std::memcmp(m, cull.head_rows.data(), sizeof(cull.head_rows)) != 0) continue;

      std::memcpy(m, cull.game_rows.data(), sizeof(cull.game_rows));

      auto* const view = reinterpret_cast<float*>(cull.camera + 0x70);

      if (cull.view_written &&
          std::memcmp(view, cull.head_view.data(), sizeof(cull.head_view)) == 0) {
         std::memcpy(view, cull.game_view.data(), sizeof(cull.game_view));
      }
      *reinterpret_cast<float*>(cull.camera + 0x138) = cull.game_tan.x;
      *reinterpret_cast<float*>(cull.camera + 0x13c) = cull.game_tan.y;

      update_frustum(cull.camera);
   }

   state.head_culls.clear();
   state.cull_cameras.clear();
   state.head_cull_done = false;
}

// The menu screen came up (death, unit select): the gameplay camera the game goes on
// showing there (the death camera) gets the game's own field of view back, if the
// widened one is still in it. The camera may be gone (map change): reads are guarded.
void restore_widened_fov() noexcept
{
   std::uint8_t* const camera = std::exchange(state.widened_camera, nullptr);

   if (!camera || !state.update_frustum) return;

   __try {
      float& tan_x = *reinterpret_cast<float*>(camera + 0x138);
      float& tan_y = *reinterpret_cast<float*>(camera + 0x13c);

      if (tan_x != state.widened_written_tan.x || tan_y != state.widened_written_tan.y) return;

      tan_x = state.widened_game_tan.x;
      tan_y = state.widened_game_tan.y;

      reinterpret_cast<void(__fastcall*)(std::uint8_t*)>(state.update_frustum)(camera);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// At the end of the frame: the gameplay camera's rows back to the game's rotation, so
// the game aims and fires along the gun. Left alone if the game has set them since.
void restore_aim_rows() noexcept
{
   if (!state.head_camera) return;

   auto* const m = reinterpret_cast<float*>(state.head_camera + 0x30);

   if (std::memcmp(m, state.head_rows_written.data(), sizeof(state.head_rows_written)) == 0) {
      std::memcpy(m, state.aim_rows_saved.data(), sizeof(state.aim_rows_saved));
   }

   state.head_camera = nullptr;
}

// Redirects the engine's calls at `sites` of the function at `function` (image address)
// to `hook`. Every call site is checked first: a different exe is left alone. Returns
// the function's address, or 0.
auto patch_calls(const std::uintptr_t function, std::span<const std::uintptr_t> sites,
                 const void* hook) noexcept -> std::uintptr_t
{
   const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
   const std::uintptr_t target = function - exe_image_base + base;

   const auto call_target = [](const std::uintptr_t call) {
      std::int32_t rel;
      std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), sizeof(rel));

      return call + 5 + rel;
   };

   for (const std::uintptr_t site : sites) {
      const std::uintptr_t call = site - exe_image_base + base;

      if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8 ||
          call_target(call) != target) {
         return 0;
      }
   }

   for (const std::uintptr_t site : sites) {
      const std::uintptr_t call = site - exe_image_base + base;
      const std::int32_t rel =
         static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(hook) - (call + 5));
      DWORD protection = 0;

      VirtualProtect(reinterpret_cast<void*>(call + 1), sizeof(rel), PAGE_EXECUTE_READWRITE,
                     &protection);
      std::memcpy(reinterpret_cast<void*>(call + 1), &rel, sizeof(rel));
      VirtualProtect(reinterpret_cast<void*>(call + 1), sizeof(rel), protection, &protection);
   }

   FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

   return target;
}

// Debug cheat "hero always available": the class select's "may the local player pick
// the hero" (FUN_0042b590) starts with a test of a byte the game never sets (a leftover
// debug switch): set, it answers yes, so the team's hero class (the last one, team +0x4c
// counts it) is listed whatever the unlock rules say. Maps without a hero class have
// nothing more to list. Single player doesn't check the rules again at spawn.
constexpr std::uintptr_t exe_hero_check = 0x0042b590;
constexpr std::uintptr_t exe_hero_debug_flag = 0x01e2fd12;

// Debug cheat "special units always available": "may this player spawn as class N"
// (FUN_0061f380, also behind the class select's "needs N points" text) compares the
// player's points (FUN_0061dad0) with the class's points to unlock (class +0x368) and
// refuses when they're short: its jge there (0x61f3ff) becomes a jmp. The class's
// player limit after it still applies.
constexpr std::uintptr_t exe_unlock_points_jump = 0x0061f3ff;
constexpr std::uint8_t unlock_points_bytes[] = {0x3b, 0xc2, 0x7d, 0x04, 0x85, 0xd2, 0x75, 0x69};

void apply_debug_cheats() noexcept
{
   static std::uint8_t* flag = nullptr;
   static bool checked = false;

   if (!std::exchange(checked, true)) {
      const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
      const auto* const site =
         reinterpret_cast<const std::uint8_t*>(exe_hero_check - exe_image_base + base);
      const std::uintptr_t address = exe_hero_debug_flag - exe_image_base + base;
      std::uint32_t operand = 0;

      std::memcpy(&operand, site + 2, sizeof(operand));

      // cmp byte ptr [flag],0
      if (site[0] == 0x80 && site[1] == 0x3d && operand == address && site[6] == 0x00) {
         flag = reinterpret_cast<std::uint8_t*>(address);
      }
      else {
         log(Log_level::warning, "VR: the hero availability check isn't as expected, the "
                                 "\"hero always available\" cheat does nothing.");
      }
   }

   static std::uint8_t* unlock_jump = nullptr;
   static bool unlock_checked = false;

   if (!std::exchange(unlock_checked, true)) {
      const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
      auto* const site =
         reinterpret_cast<std::uint8_t*>(exe_unlock_points_jump - 2 - exe_image_base + base);

      if (std::memcmp(site, unlock_points_bytes, sizeof(unlock_points_bytes)) == 0) {
         unlock_jump = site + 2;
      }
      else {
         log(Log_level::warning, "VR: the class unlock check isn't as expected, the \"special "
                                 "units always available\" cheat does nothing.");
      }
   }

   if (unlock_jump) {
      const std::uint8_t wanted = settings().special_units_always_available ? 0xeb : 0x7d;

      if (*unlock_jump != wanted) {
         DWORD protection = 0;

         VirtualProtect(unlock_jump, 1, PAGE_EXECUTE_READWRITE, &protection);
         *unlock_jump = wanted;
         VirtualProtect(unlock_jump, 1, protection, &protection);
         FlushInstructionCache(GetCurrentProcess(), unlock_jump, 1);

         log_fmt(Log_level::info, "VR: special units always available {}.",
                 wanted == 0xeb ? "on" : "off");
      }
   }

   if (!flag) return;

   const std::uint8_t wanted = settings().hero_always_available ? 1 : 0;

   if (*flag != wanted) {
      *flag = wanted;
      log_fmt(Log_level::info, "VR: hero always available {}.", wanted ? "on" : "off");
   }
}

// Prop clusters (terrain props/foliage, FUN_0062a3c0) walk each camera's frustum over a
// grid of cells and hand the cells PropCluster objects from a fixed pool (count at
// 0x1eaefe0, ~66). The game returns when the pool runs out (0x62af22), but the path that
// skips a cell failing the height test (jb 0x62af44) jumps past that check after taking
// a pool index, and the next cell reads past the pool's end (a null cluster: crash).
// With the widened frustum many more cells pass, so it happens on dense maps (Kashyyyk).
// The index load at 0x62ae62 goes through a stub that returns like the game does when
// the pool is used up.
constexpr std::uintptr_t exe_prop_pool_load = 0x0062ae62;
constexpr std::uint8_t exe_prop_pool_load_bytes[] = {0x8b, 0x3c, 0x88,        // mov edi,[eax+ecx*4]
                                                     0x41,                    // inc ecx
                                                     0x89, 0x4c, 0x24, 0x08}; // mov [esp+8],ecx
constexpr std::uintptr_t exe_prop_pool_count = 0x01eaefe0;
constexpr std::uintptr_t exe_prop_pool_full_return = 0x0062aff7;

std::uintptr_t prop_pool_resume = 0;
std::uintptr_t prop_pool_full_return = 0;

__declspec(naked) void prop_pool_load_stub()
{
   __asm {
      push edx
      mov edx, prop_pool_count
      cmp ecx, dword ptr [edx]
      pop edx
      jge pool_used_up
      mov edi, dword ptr [eax+ecx*4]
      inc ecx
      mov dword ptr [esp+8], ecx
      jmp dword ptr [prop_pool_resume]
   pool_used_up:
      inc dword ptr [prop_pool_overruns]
      jmp dword ptr [prop_pool_full_return]
   }
}

void patch_prop_pool() noexcept
{
   if (prop_pool_resume) return;

   const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
   auto* const site = reinterpret_cast<std::uint8_t*>(exe_prop_pool_load - exe_image_base + base);

   if (std::memcmp(site, exe_prop_pool_load_bytes, sizeof(exe_prop_pool_load_bytes)) != 0) {
      log(Log_level::warning, "VR: the prop cluster pool code isn't as expected, dense props "
                              "may crash the game with the widened frustum.");

      return;
   }

   prop_pool_count = exe_prop_pool_count - exe_image_base + base;
   prop_pool_resume =
      reinterpret_cast<std::uintptr_t>(site) + sizeof(exe_prop_pool_load_bytes);
   prop_pool_full_return = exe_prop_pool_full_return - exe_image_base + base;

   std::uint8_t jump[sizeof(exe_prop_pool_load_bytes)];
   std::memset(jump, 0x90, sizeof(jump));
   jump[0] = 0xE9;

   const std::int32_t rel = static_cast<std::int32_t>(
      reinterpret_cast<std::uintptr_t>(&prop_pool_load_stub) -
      (reinterpret_cast<std::uintptr_t>(site) + 5));
   std::memcpy(jump + 1, &rel, sizeof(rel));

   DWORD protection = 0;
   VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &protection);
   std::memcpy(site, jump, sizeof(jump));
   VirtualProtect(site, sizeof(jump), protection, &protection);
   FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

   log(Log_level::info, "VR: prop cluster pool overrun guarded.");
}

// The renderer's queued draws (FUN_006af430, flushed by FUN_006b81e0: on Hoth, Kamino,
// Mustafar, ...) each name their current shader among the draw's (+8, index u16 +0x16)
// and ask it for its pass table (slot 0x44, 0x6af536 and 0x6af6b3). Sometimes a draw
// outlives its shader: the object has its abstract base's vtable again, slot 0x44 is
// _purecall and the CRT aborts the game (fast fail, no exception to catch). Both calls go
// through a stub that drops such a draw (the epilogue at 0x6af502; the draw was already
// taken off the queue, ebx/edi/esi are pushed on both paths).
constexpr std::uintptr_t exe_shader_pass_calls[] = {0x006af536, 0x006af6b3};
constexpr std::uint8_t exe_shader_pass_call_bytes[] = {0x8b, 0x01,        // mov eax,[ecx]
                                                       0xff, 0x50, 0x44}; // call [eax+44h]
constexpr std::uintptr_t exe_shader_pass_skip = 0x006af502;
constexpr std::uintptr_t exe_purecall_thunk = 0x00752998; // jmp [_purecall's IAT slot]

std::uintptr_t purecall_thunk = 0;
std::uintptr_t purecall_function = 0;
std::uintptr_t shader_pass_skip = 0;
std::uintptr_t shader_pass_resume[std::size(exe_shader_pass_calls)]{};

#define VR_SHADER_PASS_STUB(index)                                                         \
   __asm mov eax, dword ptr [ecx]                                                          \
   __asm mov edx, dword ptr [eax+0x44]                                                     \
   __asm cmp edx, dword ptr [purecall_thunk]                                               \
   __asm je stale                                                                          \
   __asm cmp edx, dword ptr [purecall_function]                                            \
   __asm je stale                                                                          \
   __asm call edx                                                                          \
   __asm jmp dword ptr [shader_pass_resume + index * 4]                                    \
   __asm stale:                                                                            \
   __asm mov dword ptr [stale_shader_vtable], eax                                          \
   __asm inc dword ptr [stale_shader_draws]                                                \
   __asm jmp dword ptr [shader_pass_skip]

// edx is free at both sites: thiscall, and the code after the calls reloads it.
__declspec(naked) void shader_pass_stub_0()
{
   VR_SHADER_PASS_STUB(0)
}

__declspec(naked) void shader_pass_stub_1()
{
   VR_SHADER_PASS_STUB(1)
}

#undef VR_SHADER_PASS_STUB

void patch_stale_shaders() noexcept
{
   if (shader_pass_skip) return;

   const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
   const void* const stubs[] = {reinterpret_cast<const void*>(&shader_pass_stub_0),
                                reinterpret_cast<const void*>(&shader_pass_stub_1)};
   auto* const thunk = reinterpret_cast<const std::uint8_t*>(exe_purecall_thunk -
                                                             exe_image_base + base);
   const HMODULE crt = GetModuleHandleW(L"msvcr120.dll");

   purecall_function =
      crt ? reinterpret_cast<std::uintptr_t>(GetProcAddress(crt, "_purecall")) : 0;

   bool expected = purecall_function && thunk[0] == 0xff && thunk[1] == 0x25 &&
                   **reinterpret_cast<const std::uintptr_t* const*>(thunk + 2) ==
                      purecall_function;

   for (const std::uintptr_t call : exe_shader_pass_calls) {
      expected &= std::memcmp(reinterpret_cast<const void*>(call - exe_image_base + base),
                              exe_shader_pass_call_bytes,
                              sizeof(exe_shader_pass_call_bytes)) == 0;
   }

   if (!expected) {
      log(Log_level::warning, "VR: the renderer's draw queue code isn't as expected, a "
                              "destroyed shader may still crash the game.");

      return;
   }

   purecall_thunk = reinterpret_cast<std::uintptr_t>(thunk);

   for (std::size_t i = 0; i < std::size(exe_shader_pass_calls); ++i) {
      auto* const site = reinterpret_cast<std::uint8_t*>(exe_shader_pass_calls[i] -
                                                         exe_image_base + base);

      shader_pass_resume[i] =
         reinterpret_cast<std::uintptr_t>(site) + sizeof(exe_shader_pass_call_bytes);

      std::uint8_t jump[sizeof(exe_shader_pass_call_bytes)]{0xE9};
      const std::int32_t rel =
         static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(stubs[i]) -
                                   (reinterpret_cast<std::uintptr_t>(site) + 5));
      std::memcpy(jump + 1, &rel, sizeof(rel));

      DWORD protection = 0;
      VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &protection);
      std::memcpy(site, jump, sizeof(jump));
      VirtualProtect(site, sizeof(jump), protection, &protection);
   }

   shader_pass_skip = exe_shader_pass_skip - exe_image_base + base;

   FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

   log(Log_level::info, "VR: queued draws of destroyed shaders guarded.");
}

// Why shaders get destroyed while in use: a queued draw (the per-frame array at 0x8174f0)
// takes a reference on its mesh, shader and passes (u16 at +4, count << 2) when it's
// made, and gives them back (FUN_006b0ed0) once it's drawn. The sorted queues' push
// (FUN_006b2820, thiscall(queue, &draw, key)) silently drops the draw when the queue's
// node pool is used up (+8: free list; 800 nodes for the main one), and the array is
// just reset next frame: the references leak. After 16384 leaks the 14-bit count wraps,
// a release takes it to 0 and the shader is deleted under the draws still using it
// (use after free: _purecall, or a call through a vtable that is now something else).
// The widened frustum overfills the queues on dense maps (Hoth: minutes in). All four
// push calls go through a hook that gives a dropped draw's references back.
constexpr std::uintptr_t exe_queue_push = 0x006b2820;
constexpr std::uintptr_t exe_queue_push_calls[] = {0x006af5bf, 0x006af658, 0x006b90d2,
                                                   0x006b91fd};
constexpr std::uintptr_t exe_release_draw = 0x006b0ed0; // fastcall(draw)
constexpr std::ptrdiff_t queue_free_nodes = 0x8;

using Queue_push_fn = void(__fastcall*)(void* queue, void*, void* const* draw,
                                        std::uint32_t key);
using Release_draw_fn = void(__fastcall*)(void* draw);

Queue_push_fn queue_push = nullptr;
Release_draw_fn release_draw = nullptr;

void __fastcall queue_push_hook(void* const queue, void*, void* const* const draw,
                                const std::uint32_t key) noexcept
{
   if (draw_count) peak_draw_count = std::max(peak_draw_count, *draw_count);

   if (*reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(queue) +
                                             queue_free_nodes) == nullptr) {
      dropped_queued_draws += 1;

      if (*draw) release_draw(*draw);

      return;
   }

   queue_push(queue, nullptr, draw, key);
}

// A dropped draw is simply missing from the frame: with the widened frustum the queues
// fill up when the head looks over a dense scene (Hoth's AT-ATs vanished at some head
// angles). The three sorted queues are built by static initializers (FUN_006b21f0,
// thiscall(queue, capacity): sets the capacity at +0x14, frees the old pool at +4,
// allocates capacity * 12-byte nodes and links them into the free list at +8). Called
// again while a queue is empty (head +0xc null, count +0x10 zero) it just swaps in a
// bigger pool. Pushes walk the sorted list, so the queues grow only 4x.
constexpr std::uintptr_t exe_queue_init = 0x006b21f0;

struct Draw_queue {
   std::uintptr_t address;
   std::uint32_t capacity;
};

constexpr Draw_queue exe_draw_queues[] = {{0x0086ac50, 800},
                                          {0x0086ac74, 512},
                                          {0x007fbec8, 100}};
constexpr std::uint32_t draw_queue_growth = 4;

using Queue_init_fn = void(__fastcall*)(void* queue, void*, std::uint32_t capacity);

// Returns whether every queue was empty (nothing refers to the draw array then).
bool enlarge_draw_queues(const std::uintptr_t base) noexcept
{
   const auto init = reinterpret_cast<Queue_init_fn>(exe_queue_init - exe_image_base + base);
   bool all_empty = true;

   for (const Draw_queue& queue : exe_draw_queues) {
      auto* const q = reinterpret_cast<std::uint8_t*>(queue.address - exe_image_base + base);
      const auto capacity = *reinterpret_cast<const std::uint32_t*>(q + 0x14);
      const auto head = *reinterpret_cast<const std::uint32_t*>(q + 0xc);
      const auto count = *reinterpret_cast<const std::uint32_t*>(q + 0x10);

      if (head != 0 || count != 0) all_empty = false;

      if (capacity != queue.capacity || head != 0 || count != 0) {
         log_fmt(Log_level::warning,
                 "VR: draw queue at {:#x} not enlarged (capacity {}, head {:#x}, count {}).",
                 queue.address, capacity, head, count);

         continue;
      }

      init(q, nullptr, queue.capacity * draw_queue_growth);
   }

   log_fmt(Log_level::info, "VR: the renderer's draw queues enlarged {}x.",
           draw_queue_growth);

   return all_empty;
}

// The per-frame draw array (0x8174f0, 0x128c draws of 0x48 bytes, count at 0x7fa050)
// ends right where the main queue's header starts (0x86ac50). FUN_006b88f0 checks the
// count once (cmp ecx,0x128c at 0x6b8925), but its copy path (flag 0x8000000, 0x6b8fcd)
// and FUN_006b0d70 (0x6b0da9) take another draw unchecked: a full array writes a draw
// over the queue headers (pools, free lists: random crashes later). A full array also
// skips every later draw of the frame (characters, turrets, the gun, buildings vanish
// together on Hoth). The array is reached only through those three lea's, so it moves to
// a 4x bigger one, and the limit keeps two draws of headroom for the unchecked paths.
constexpr std::uintptr_t exe_draw_array = 0x008174f0;
constexpr std::uint32_t exe_draw_array_capacity = 0x128c;
constexpr std::uint32_t draw_record_size = 0x48;
constexpr std::uintptr_t exe_draw_array_refs[] = {0x006b0da9, 0x006b8945, 0x006b8fcd};
constexpr std::uintptr_t exe_draw_array_limit = 0x006b8928; // imm32 of cmp ecx,0x128c
constexpr std::uintptr_t exe_draw_count = 0x007fa050;
constexpr std::uint32_t draw_array_growth = 4;
constexpr std::uint32_t draw_array_headroom = 2;

void write_code_dword(const std::uintptr_t address, const std::uint32_t value) noexcept
{
   auto* const site = reinterpret_cast<void*>(address);
   DWORD protection = 0;

   VirtualProtect(site, sizeof(value), PAGE_EXECUTE_READWRITE, &protection);
   std::memcpy(site, &value, sizeof(value));
   VirtualProtect(site, sizeof(value), protection, &protection);
}

void enlarge_draw_array(const std::uintptr_t base, const bool queues_empty) noexcept
{
   const auto rebase = [base](const std::uintptr_t address) {
      return address - exe_image_base + base;
   };
   const auto read = [](const std::uintptr_t address) {
      return *reinterpret_cast<const std::uint32_t*>(address);
   };

   bool expected = read(rebase(exe_draw_array_limit)) == exe_draw_array_capacity;

   for (const std::uintptr_t ref : exe_draw_array_refs) {
      expected &= read(rebase(ref)) == rebase(exe_draw_array);
   }

   if (!expected) {
      log(Log_level::warning, "VR: the renderer's draw array code isn't as expected, a full "
                              "array may still overwrite the draw queues.");

      return;
   }

   draw_count = reinterpret_cast<const std::uint32_t*>(rebase(exe_draw_count));

   // Draws live only within a frame (the queues are empty here), and their constructor
   // does nothing: zeroed memory is a fresh array.
   void* const array =
      queues_empty ? VirtualAlloc(nullptr,
                                  std::size_t{exe_draw_array_capacity} * draw_array_growth *
                                     draw_record_size,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
                   : nullptr;

   if (array) {
      draw_array_capacity = exe_draw_array_capacity * draw_array_growth;

      for (const std::uintptr_t ref : exe_draw_array_refs) {
         write_code_dword(rebase(ref),
                          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(array)));
      }
   }

   write_code_dword(rebase(exe_draw_array_limit), draw_array_capacity - draw_array_headroom);

   FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

   log_fmt(Log_level::info, "VR: the renderer's draw array holds {} draws{}.",
           draw_array_capacity - draw_array_headroom,
           array ? "" : " (not moved: the queues weren't empty), overrun guarded");
}

void patch_queue_push() noexcept
{
   if (queue_push) return;

   const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

   release_draw = reinterpret_cast<Release_draw_fn>(exe_release_draw - exe_image_base + base);
   queue_push = reinterpret_cast<Queue_push_fn>(
      patch_calls(exe_queue_push, exe_queue_push_calls,
                  reinterpret_cast<const void*>(&queue_push_hook)));

   if (!queue_push) {
      log(Log_level::warning, "VR: the renderer's draw queue push isn't as expected, full "
                              "queues may leak shader references (crashes minutes in).");

      return;
   }

   log(Log_level::info, "VR: full draw queues give dropped draws' references back.");

   enlarge_draw_array(base, enlarge_draw_queues(base));
}

void hook_game_camera() noexcept
{
   if (state.update_frustum) return;

   // Experiment, off by default: a bf2vr_head_camera.txt next to d3d9.dll draws the
   // gameplay camera with the head's rotation. It breaks aiming (the game reads the
   // head's rows back: the hands spin), so normally the camera looks along the gun and
   // only its field of view is widened.
   state.head_camera_disabled =
      GetFileAttributesW((module_directory() + L"bf2vr_head_camera.txt").c_str()) ==
      INVALID_FILE_ATTRIBUTES;

   // Debug switch: a bf2vr_no_head_cull.txt next to d3d9.dll culls with the gun's
   // camera only (see apply_head_cull).
   state.head_cull_disabled =
      GetFileAttributesW((module_directory() + L"bf2vr_no_head_cull.txt").c_str()) !=
      INVALID_FILE_ATTRIBUTES;

   state.update_frustum = patch_calls(exe_update_frustum, exe_update_frustum_calls,
                                      reinterpret_cast<const void*>(&update_frustum_hook));

   if (!state.update_frustum) {
      log(Log_level::warning, "VR: unknown game executable, the game camera isn't changed "
                              "(the world may be missing where the head looks away from "
                              "the gun).");

      return;
   }

   patch_prop_pool();
   patch_stale_shaders();
   patch_queue_push();

   if (!state.head_camera_disabled) {
      state.build_view = patch_calls(exe_build_view, exe_build_view_calls,
                                     reinterpret_cast<const void*>(&build_view_hook));
   }

   if (!state.head_cull_disabled) {
      state.set_render_camera =
         patch_calls(exe_set_render_camera, exe_set_render_camera_calls,
                     reinterpret_cast<const void*>(&set_render_camera_hook));

      if (!state.set_render_camera) {
         log(Log_level::warning, "VR: the render camera calls aren't as expected, cameras "
                                 "set up while drawing are culled with the game's rotation.");
      }

      state.pvs_camera = patch_calls(exe_pvs_camera, exe_pvs_camera_calls,
                                     reinterpret_cast<const void*>(&pvs_camera_hook));

      if (!state.pvs_camera) {
         log(Log_level::warning, "VR: the sector visibility call isn't as expected, sectors "
                                 "are found along the gun's view.");
      }
   }

   log_fmt(Log_level::info, "VR: game camera hooked: drawn and culled {}.",
           state.build_view ? "looking where the head looks, aimed by the gun"
                            : "along the gun with a widened field of view");
}

// --- Loader -----------------------------------------------------------------

auto module_directory() noexcept -> std::wstring
{
   HMODULE self = nullptr;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&module_directory), &self);

   std::wstring path(MAX_PATH, L'\0');
   path.resize(GetModuleFileNameW(self, path.data(), MAX_PATH));
   path.resize(path.find_last_of(L"\\/") + 1);

   return path;
}

// The loader reports why it can't find/load a runtime only on stderr. The game has no
// console, so point stderr at a file before the loader's CRT initialises.
void capture_loader_diagnostics(const std::wstring& directory) noexcept
{
   const HANDLE file =
      CreateFileW((directory + L"openxr_loader.log").c_str(), GENERIC_WRITE,
                  FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

   if (file == INVALID_HANDLE_VALUE) return;

   SetStdHandle(STD_ERROR_HANDLE, file);
   SetEnvironmentVariableW(L"XR_LOADER_DEBUG", L"error");
}

// Steam launches games with XR_RUNTIME_JSON pointing at SteamVR's 64-bit manifest
// (steamxr_win64.json -> vrclient_x64.dll), which a 32-bit process can't load. It takes
// precedence over the registry, so swap it for the 32-bit manifest next to it.
void fix_runtime_json_bitness() noexcept
{
   std::wstring value(MAX_PATH * 2, L'\0');
   const DWORD length =
      GetEnvironmentVariableW(L"XR_RUNTIME_JSON", value.data(), static_cast<DWORD>(value.size()));

   if (length == 0 || length >= value.size()) return;

   value.resize(length);

   const auto pos = value.rfind(L"_win64.json");

   if (sizeof(void*) != 4 || pos == std::wstring::npos) return;

   std::wstring win32_manifest = value;
   win32_manifest.replace(pos, std::wcslen(L"_win64.json"), L"_win32.json");

   if (GetFileAttributesW(win32_manifest.c_str()) == INVALID_FILE_ATTRIBUTES) {
      log(Log_level::warning, "VR: XR_RUNTIME_JSON names a 64-bit runtime and no 32-bit "
                              "manifest exists next to it.");

      return;
   }

   SetEnvironmentVariableW(L"XR_RUNTIME_JSON", win32_manifest.c_str());

   log(Log_level::info, "VR: switched XR_RUNTIME_JSON to the 32-bit runtime manifest.");
}

// The first lines of a log: what a bug report needs to know about the player's setup
// (the graphics card and the OpenXR runtime are logged where they're found).
void log_system_info(const std::wstring& directory) noexcept
{
   const auto ascii = [](const std::wstring_view text) {
      std::string result;

      for (const wchar_t c : text) result += c < 128 ? static_cast<char>(c) : '?';

      return result;
   };

   log_fmt(Log_level::info, "VR: mod built {} {}.", __DATE__, __TIME__);

   using Rtl_get_version_fn = LONG(WINAPI*)(OSVERSIONINFOW*);
   OSVERSIONINFOW version{sizeof(version)};

   if (const auto rtl_get_version = reinterpret_cast<Rtl_get_version_fn>(
          GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
       rtl_get_version && rtl_get_version(&version) == 0) {
      log_fmt(Log_level::info, "VR: Windows {}.{} build {}.", version.dwMajorVersion,
              version.dwMinorVersion, version.dwBuildNumber);
   }

   MEMORYSTATUSEX memory{sizeof(memory)};
   GlobalMemoryStatusEx(&memory);

   wchar_t exe_path[MAX_PATH]{};
   GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

   WIN32_FILE_ATTRIBUTE_DATA exe_file{};
   GetFileAttributesExW(exe_path, GetFileExInfoStandard, &exe_file);

   const auto exe = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
   const auto& nt_headers = *reinterpret_cast<const IMAGE_NT_HEADERS*>(
      exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
   const bool large_address_aware =
      (nt_headers.FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
   const DWORD exe_size = exe_file.nFileSizeLow;

   log_fmt(Log_level::info,
           "VR: game program {} bytes ({}), {}; RAM {} GB, game address space {} MB.",
           exe_size,
           exe_size == 4363776   ? "Unofficial Patch r130"
           : exe_size == 4357632 ? "the original Steam/GOG one"
                                 : "unknown build",
           large_address_aware ? "Large Address Aware" : "NOT Large Address Aware",
           (memory.ullTotalPhys + (1ull << 29)) >> 30, memory.ullTotalVirtual >> 20);

   std::string addons;
   WIN32_FIND_DATAW found{};

   if (const HANDLE find = FindFirstFileW((directory + L"addon\\*").c_str(), &found);
       find != INVALID_HANDLE_VALUE) {
      do {
         if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
             found.cFileName[0] == L'.') {
            continue;
         }

         addons += (addons.empty() ? "" : ", ") + ascii(found.cFileName);
      } while (FindNextFileW(find, &found));

      FindClose(find);
   }

   log_fmt(Log_level::info, "VR: GameData\\addon folders: {}.",
           addons.empty() ? "none" : addons);
}

bool load_loader() noexcept
{
   const std::wstring directory = module_directory();

   log_system_info(directory);

   capture_loader_diagnostics(directory);
   fix_runtime_json_bitness();

   const HMODULE loader = LoadLibraryW((directory + L"openxr_loader.dll").c_str());

   if (!loader) {
      log(Log_level::info, "VR: openxr_loader.dll not found, VR disabled.");

      return false;
   }

   state.xr.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
      GetProcAddress(loader, "xrGetInstanceProcAddr"));

   if (!state.xr.xrGetInstanceProcAddr) return false;

   state.xr.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                  reinterpret_cast<PFN_xrVoidFunction*>(
                                     &state.xr.xrEnumerateInstanceExtensionProperties));

   return !failed(state.xr.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance",
                                                 reinterpret_cast<PFN_xrVoidFunction*>(
                                                    &state.xr.xrCreateInstance)),
                  "xrGetInstanceProcAddr(xrCreateInstance)");
}

bool load_instance_functions() noexcept
{
#define SP_XR_LOAD(name)                                                            \
   if (failed(state.xr.xrGetInstanceProcAddr(state.instance, #name,                 \
                                             reinterpret_cast<PFN_xrVoidFunction*>( \
                                                &state.xr.name)),                   \
              "xrGetInstanceProcAddr(" #name ")"))                                  \
      return false;

   SP_XR_INSTANCE_FUNCTIONS(SP_XR_LOAD)
#undef SP_XR_LOAD

   return true;
}

bool instance_extension_available(const char* name) noexcept
{
   if (!state.xr.xrEnumerateInstanceExtensionProperties) return false;

   uint32_t count = 0;
   state.xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
   std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
   state.xr.xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());

   for (const auto& extension : extensions) {
      if (std::strcmp(extension.extensionName, name) == 0) return true;
   }

   return false;
}

// Routes OpenXR runtime diagnostics to our log.
XrBool32 XRAPI_PTR debug_message(XrDebugUtilsMessageSeverityFlagsEXT severity,
                                 XrDebugUtilsMessageTypeFlagsEXT,
                                 const XrDebugUtilsMessengerCallbackDataEXT* data,
                                 void*) noexcept
{
   const Log_level level = (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                              ? Log_level::warning
                              : Log_level::info;

   log_fmt(level, "VR (OpenXR): {}: {}", data->functionName ? data->functionName : "",
           data->message ? data->message : "");

   return XR_FALSE;
}

// --- Session ----------------------------------------------------------------

// Optional bf2vr_weapon.txt next to d3d9.dll: "x y z", the weapon's grip point in the
// game camera's view space (metres, x right, y up, -z forward).
void load_weapon_tuning() noexcept
{
   const std::wstring path = module_directory() + L"bf2vr_weapon.txt";
   FILE* file = nullptr;

   if (_wfopen_s(&file, path.c_str(), L"r") == 0 && file) {
      glm::vec3 grip;

      if (std::fscanf(file, "%f %f %f", &grip.x, &grip.y, &grip.z) == 3) {
         state.weapon_grip_view = grip;
         state.weapon_grip_from_file = true;
      }

      std::fclose(file);
   }

   if (state.weapon_grip_from_file) {
      log_fmt(Log_level::info,
              "VR: weapon grip point in view space ({:.3f}, {:.3f}, {:.3f}) from "
              "bf2vr_weapon.txt.",
              state.weapon_grip_view.x, state.weapon_grip_view.y, state.weapon_grip_view.z);
   }
   else {
      log(Log_level::info, "VR: weapon grip point: the gun's own origin.");
   }
}

// A binding's key or button name ("Left Shift"), for the log and the panel.
auto key_name(const Game_binding binding) noexcept -> std::string
{
   if (binding.key) {
      // DirectInput codes are scan codes; 0x80 and up are the extended keys.
      const LONG scan = (binding.key & 0x7f) | ((binding.key & 0x80) ? 0x100 : 0);
      char name[64]{};

      if (GetKeyNameTextA(scan << 16, name, sizeof(name)) > 0) return name;

      return fmt::format("key 0x{:02x}", binding.key);
   }

   if (binding.mouse >= 0x43 && binding.mouse <= 0x46) {
      return fmt::format("mouse button {}", binding.mouse - 0x42);
   }

   if (binding.mouse == 0x4b) return "wheel up";
   if (binding.mouse == 0x4c) return "wheel down";

   return "unbound";
}

// The game's own default control sets (without a profile, or for sets it lacks): the
// soldier's keys where the vehicle sets share them.
void set_default_mode_bindings() noexcept
{
   for (Control_set& set : mode_bindings) {
      set = {};

      for (int control = 0; control < control_count; ++control) {
         if (control_records[control] >= 0) {
            set[control_records[control]] = default_bindings[control];
         }
      }
   }

   Control_set& vehicle = mode_bindings[1];

   vehicle[4] = {0x2e};  // C: droideka deploy
   vehicle[13] = {0x02}; // 1: next position
   vehicle[14] = {0x03}; // 2: previous position
   vehicle[15] = {};
   vehicle[29] = {};

   Control_set& starfighter = mode_bindings[2];

   starfighter = vehicle;
   starfighter[4] = {0x38};  // Left Alt: trick
   starfighter[5] = {0x1d};  // Left Ctrl: roll lock
   starfighter[29] = {0x2e}; // C: Immelmann
}

// The most recently saved profile's bindings (see "Game controls"), read again when a
// newer one has been saved since.
void load_bindings() noexcept
{
   const std::wstring directory = module_directory() + L"SaveGames\\";
   WIN32_FIND_DATAW found{};
   const HANDLE find = FindFirstFileW((directory + L"*.profile").c_str(), &found);

   if (find == INVALID_HANDLE_VALUE) {
      if (!std::exchange(bindings_loaded, true)) {
         set_default_mode_bindings();
         log(Log_level::info, "VR: no player profile found, the controls press the game's "
                              "default keys.");
      }

      return;
   }

   std::wstring newest;
   FILETIME newest_time{};

   do {
      if (CompareFileTime(&found.ftLastWriteTime, &newest_time) > 0) {
         newest_time = found.ftLastWriteTime;
         newest = found.cFileName;
      }
   } while (FindNextFileW(find, &found));

   FindClose(find);

   if (bindings_loaded && CompareFileTime(&newest_time, &bindings_profile_time) == 0) return;

   bindings_loaded = true;
   bindings_profile_time = newest_time;
   game_bindings = default_bindings;
   set_default_mode_bindings();

   std::vector<std::uint8_t> profile(0x4000);
   FILE* file = nullptr;

   if (_wfopen_s(&file, (directory + newest).c_str(), L"rb") == 0 && file) {
      profile.resize(std::fread(profile.data(), 1, profile.size(), file));
      std::fclose(file);
   }
   else {
      profile.clear();
   }

   for (int control = 0; control < control_count; ++control) {
      const int record = control_records[control];
      const std::size_t offset = profile_records_offset + record * profile_record_size;

      if (record < 0 || offset + profile_record_size > profile.size()) continue;

      // The first slot holding a key or a mouse button.
      for (std::size_t slot = 0; slot < 3; ++slot) {
         const Game_binding binding{profile[offset + slot * 2], profile[offset + slot * 2 + 1]};

         if (binding.key || binding.mouse) {
            game_bindings[control] = binding;

            break;
         }
      }
   }

   // Each control set the profile has, whole (a record left empty there is unbound).
   for (int mode = 0; mode < control_mode_count; ++mode) {
      const std::size_t section = profile_records_offset + mode * profile_section_size;

      if (section + profile_section_size > profile.size()) break;

      for (int record = 0; record < profile_record_count; ++record) {
         const std::size_t offset = section + record * profile_record_size;
         Game_binding first{};

         for (std::size_t slot = 0; slot < 3; ++slot) {
            const Game_binding binding{profile[offset + slot * 2], profile[offset + slot * 2 + 1]};

            if (binding.key || binding.mouse) {
               first = binding;

               break;
            }
         }

         mode_bindings[mode][record] = first;
      }
   }

   bindings_text.clear();

   for (int control = 0; control < control_count; ++control) {
      bindings_text += fmt::format("{}{}: {}", control ? ", " : "", control_names[control],
                                   key_name(game_bindings[control]));
   }

   std::string name;

   for (const wchar_t c : newest) name += c < 128 ? static_cast<char>(c) : '?';

   log_fmt(Log_level::info, "VR: game controls from the newest profile: {}.", bindings_text);

   // The vehicle sets' controls the controllers press (see update_input).
   for (const int mode : {1, 2}) {
      std::string text;

      for (const Game_control control :
           {control_sprint, control_jump, control_crouch, control_zoom, control_view,
            control_reload, control_use, control_lock, control_next_primary,
            control_previous_primary, control_roll, control_map}) {
         text += fmt::format("{}{}: {}", text.empty() ? "" : ", ", control_names[control],
                             key_name(mode_bindings[mode][control_records[control]]));
      }

      log_fmt(Log_level::info, "VR: {} controls (named as the soldier's): {}.",
              control_mode_names[mode], text);
   }
}

auto make_path(const char* str) noexcept -> XrPath
{
   XrPath path = XR_NULL_PATH;
   state.xr.xrStringToPath(state.instance, str, &path);

   return path;
}

void create_actions() noexcept
{
   XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
   strcpy_s(set_info.actionSetName, "gameplay");
   strcpy_s(set_info.localizedActionSetName, "Gameplay");

   if (failed(state.xr.xrCreateActionSet(state.instance, &set_info, &state.action_set),
              "xrCreateActionSet"))
      return;

   const auto create = [](const XrActionType type, const char* name,
                          const char* localized_name, XrAction& action) {
      XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
      action_info.actionType = type;
      strcpy_s(action_info.actionName, name);
      strcpy_s(action_info.localizedActionName, localized_name);

      return !failed(state.xr.xrCreateAction(state.action_set, &action_info, &action),
                     name);
   };

   if (!create(XR_ACTION_TYPE_BOOLEAN_INPUT, "fire", "Fire", state.fire_action)) return;

   create(XR_ACTION_TYPE_POSE_INPUT, "aim", "Weapon aim", state.aim_action);
   create(XR_ACTION_TYPE_POSE_INPUT, "left_aim", "Left hand", state.left_aim_action);
   create(XR_ACTION_TYPE_VECTOR2F_INPUT, "move", "Move", state.move_action);
   create(XR_ACTION_TYPE_VECTOR2F_INPUT, "turn", "Snap turn / change weapon",
          state.turn_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "reload", "Reload", state.reload_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "jump", "Jump", state.jump_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "class_menu", "Use / enter vehicle / change class",
          state.class_menu_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "pause", "Pause", state.pause_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary_fire", "Ability / grenade",
          state.secondary_fire_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "two_hand", "Hold weapon with both hands",
          state.two_hand_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "block", "Scope (guns) / saber block (both grips)",
          state.block_action);
   create(XR_ACTION_TYPE_POSE_INPUT, "grip", "Saber grip", state.grip_action);
   create(XR_ACTION_TYPE_POSE_INPUT, "left_grip", "Left saber grip", state.left_grip_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "sprint", "Sprint", state.sprint_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "crouch", "Crouch (roll while strafing)",
          state.crouch_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "squad", "Squad command", state.squad_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "lock_target", "Lock target", state.lock_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "left_pad_click", "Left pad click (Vive)",
          state.left_pad_click_action);
   create(XR_ACTION_TYPE_BOOLEAN_INPUT, "right_pad_click", "Right pad click (Vive)",
          state.right_pad_click_action);
   create(XR_ACTION_TYPE_VECTOR2F_INPUT, "left_pad", "Left pad (Vive, WMR)",
          state.left_pad_action);
   create(XR_ACTION_TYPE_VECTOR2F_INPUT, "right_pad", "Right pad (Vive, WMR)",
          state.right_pad_action);

   // Vibration, per hand.
   state.hand_paths[0] = make_path("/user/hand/left");
   state.hand_paths[1] = make_path("/user/hand/right");

   {
      XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
      action_info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
      strcpy_s(action_info.actionName, "haptic");
      strcpy_s(action_info.localizedActionName, "Vibration");
      action_info.countSubactionPaths = 2;
      action_info.subactionPaths = state.hand_paths;

      if (failed(state.xr.xrCreateAction(state.action_set, &action_info, &state.haptic_action),
                 "haptic")) {
         state.haptic_action = XR_NULL_HANDLE;
      }
   }

   struct Binding {
      XrAction action;
      const char* path;
   };

   const auto suggest = [](const char* profile, std::initializer_list<Binding> bindings) {
      std::vector<XrActionSuggestedBinding> suggested_bindings;

      for (const Binding& binding : bindings) {
         if (binding.action) {
            suggested_bindings.push_back({binding.action, make_path(binding.path)});
         }
      }

      XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
      suggested.interactionProfile = make_path(profile);
      suggested.countSuggestedBindings = static_cast<uint32_t>(suggested_bindings.size());
      suggested.suggestedBindings = suggested_bindings.data();

      failed(state.xr.xrSuggestInteractionProfileBindings(state.instance, &suggested),
             profile);
   };

   constexpr const char* aim_pose = "/user/hand/right/input/aim/pose";
   constexpr const char* left_aim_pose = "/user/hand/left/input/aim/pose";
   constexpr const char* grip_pose = "/user/hand/right/input/grip/pose";
   constexpr const char* left_grip_pose = "/user/hand/left/input/grip/pose";

   suggest("/interaction_profiles/oculus/touch_controller",
           {{state.fire_action, "/user/hand/right/input/trigger/value"},
            {state.aim_action, aim_pose},
            {state.left_aim_action, left_aim_pose},
            {state.move_action, "/user/hand/left/input/thumbstick"},
            {state.turn_action, "/user/hand/right/input/thumbstick"},
            {state.reload_action, "/user/hand/right/input/a/click"},
            {state.jump_action, "/user/hand/right/input/b/click"},
            {state.class_menu_action, "/user/hand/left/input/x/click"},
            {state.pause_action, "/user/hand/left/input/y/click"},
            {state.secondary_fire_action, "/user/hand/left/input/trigger/value"},
            {state.grip_action, grip_pose},
            {state.left_grip_action, left_grip_pose},
            {state.haptic_action, "/user/hand/left/output/haptic"},
            {state.haptic_action, "/user/hand/right/output/haptic"},
            {state.two_hand_action, "/user/hand/left/input/squeeze/value"},
            {state.block_action, "/user/hand/right/input/squeeze/value"},
            {state.sprint_action, "/user/hand/left/input/thumbstick/click"},
            {state.crouch_action, "/user/hand/right/input/thumbstick/click"},
            {state.squad_action, "/user/hand/left/input/menu/click"}});
   suggest("/interaction_profiles/valve/index_controller",
           {{state.fire_action, "/user/hand/right/input/trigger/click"},
            {state.aim_action, aim_pose},
            {state.left_aim_action, left_aim_pose},
            {state.move_action, "/user/hand/left/input/thumbstick"},
            {state.turn_action, "/user/hand/right/input/thumbstick"},
            {state.reload_action, "/user/hand/right/input/a/click"},
            {state.jump_action, "/user/hand/right/input/b/click"},
            {state.class_menu_action, "/user/hand/left/input/a/click"},
            {state.pause_action, "/user/hand/left/input/b/click"},
            {state.secondary_fire_action, "/user/hand/left/input/trigger/click"},
            {state.grip_action, grip_pose},
            {state.left_grip_action, left_grip_pose},
            {state.haptic_action, "/user/hand/left/output/haptic"},
            {state.haptic_action, "/user/hand/right/output/haptic"},
            {state.two_hand_action, "/user/hand/left/input/squeeze/value"},
            {state.block_action, "/user/hand/right/input/squeeze/value"},
            {state.sprint_action, "/user/hand/left/input/thumbstick/click"},
            {state.crouch_action, "/user/hand/right/input/thumbstick/click"},
            {state.squad_action, "/user/hand/left/input/trackpad/force"},
            {state.lock_action, "/user/hand/right/input/trackpad/force"}});
   suggest("/interaction_profiles/htc/vive_controller",
           {{state.fire_action, "/user/hand/right/input/trigger/click"},
            {state.aim_action, aim_pose},
            {state.left_aim_action, left_aim_pose},
            {state.move_action, "/user/hand/left/input/trackpad"},
            {state.turn_action, "/user/hand/right/input/trackpad"},
            {state.left_pad_click_action, "/user/hand/left/input/trackpad/click"},
            {state.right_pad_click_action, "/user/hand/right/input/trackpad/click"},
            {state.left_pad_action, "/user/hand/left/input/trackpad"},
            {state.right_pad_action, "/user/hand/right/input/trackpad"},
            {state.pause_action, "/user/hand/left/input/menu/click"},
            {state.class_menu_action, "/user/hand/right/input/menu/click"},
            {state.secondary_fire_action, "/user/hand/left/input/trigger/click"},
            {state.grip_action, grip_pose},
            {state.left_grip_action, left_grip_pose},
            {state.haptic_action, "/user/hand/left/output/haptic"},
            {state.haptic_action, "/user/hand/right/output/haptic"},
            {state.two_hand_action, "/user/hand/left/input/squeeze/click"},
            {state.block_action, "/user/hand/right/input/squeeze/click"}});
   // Windows Mixed Reality: sticks as Touch, the pads clicked in directions as the Vive's
   // (no face buttons: the menu buttons pause and use).
   suggest("/interaction_profiles/microsoft/motion_controller",
           {{state.fire_action, "/user/hand/right/input/trigger/value"},
            {state.aim_action, aim_pose},
            {state.left_aim_action, left_aim_pose},
            {state.move_action, "/user/hand/left/input/thumbstick"},
            {state.turn_action, "/user/hand/right/input/thumbstick"},
            {state.left_pad_click_action, "/user/hand/left/input/trackpad/click"},
            {state.right_pad_click_action, "/user/hand/right/input/trackpad/click"},
            {state.left_pad_action, "/user/hand/left/input/trackpad"},
            {state.right_pad_action, "/user/hand/right/input/trackpad"},
            {state.pause_action, "/user/hand/left/input/menu/click"},
            {state.class_menu_action, "/user/hand/right/input/menu/click"},
            {state.secondary_fire_action, "/user/hand/left/input/trigger/value"},
            {state.grip_action, grip_pose},
            {state.left_grip_action, left_grip_pose},
            {state.haptic_action, "/user/hand/left/output/haptic"},
            {state.haptic_action, "/user/hand/right/output/haptic"},
            {state.two_hand_action, "/user/hand/left/input/squeeze/click"},
            {state.block_action, "/user/hand/right/input/squeeze/click"},
            {state.sprint_action, "/user/hand/left/input/thumbstick/click"},
            {state.crouch_action, "/user/hand/right/input/thumbstick/click"}});

   // HP Reverb G2: Touch's layout, the right menu button locks the target.
   if (state.hp_controller_extension) {
      suggest("/interaction_profiles/hp/mixed_reality_controller",
              {{state.fire_action, "/user/hand/right/input/trigger/value"},
               {state.aim_action, aim_pose},
               {state.left_aim_action, left_aim_pose},
               {state.move_action, "/user/hand/left/input/thumbstick"},
               {state.turn_action, "/user/hand/right/input/thumbstick"},
               {state.reload_action, "/user/hand/right/input/a/click"},
               {state.jump_action, "/user/hand/right/input/b/click"},
               {state.class_menu_action, "/user/hand/left/input/x/click"},
               {state.pause_action, "/user/hand/left/input/y/click"},
               {state.secondary_fire_action, "/user/hand/left/input/trigger/value"},
               {state.grip_action, grip_pose},
               {state.left_grip_action, left_grip_pose},
               {state.haptic_action, "/user/hand/left/output/haptic"},
               {state.haptic_action, "/user/hand/right/output/haptic"},
               {state.two_hand_action, "/user/hand/left/input/squeeze/value"},
               {state.block_action, "/user/hand/right/input/squeeze/value"},
               {state.sprint_action, "/user/hand/left/input/thumbstick/click"},
               {state.crouch_action, "/user/hand/right/input/thumbstick/click"},
               {state.squad_action, "/user/hand/left/input/menu/click"},
               {state.lock_action, "/user/hand/right/input/menu/click"}});
   }

   // Pico 4 (its own PC runtime; through SteamVR it's remapped to one of the above).
   if (state.pico_controller_extension) {
      suggest("/interaction_profiles/bytedance/pico4_controller",
              {{state.fire_action, "/user/hand/right/input/trigger/value"},
               {state.aim_action, aim_pose},
               {state.left_aim_action, left_aim_pose},
               {state.move_action, "/user/hand/left/input/thumbstick"},
               {state.turn_action, "/user/hand/right/input/thumbstick"},
               {state.reload_action, "/user/hand/right/input/a/click"},
               {state.jump_action, "/user/hand/right/input/b/click"},
               {state.class_menu_action, "/user/hand/left/input/x/click"},
               {state.pause_action, "/user/hand/left/input/y/click"},
               {state.secondary_fire_action, "/user/hand/left/input/trigger/value"},
               {state.grip_action, grip_pose},
               {state.left_grip_action, left_grip_pose},
               {state.haptic_action, "/user/hand/left/output/haptic"},
               {state.haptic_action, "/user/hand/right/output/haptic"},
               {state.two_hand_action, "/user/hand/left/input/squeeze/value"},
               {state.block_action, "/user/hand/right/input/squeeze/value"},
               {state.sprint_action, "/user/hand/left/input/thumbstick/click"},
               {state.crouch_action, "/user/hand/right/input/thumbstick/click"},
               {state.squad_action, "/user/hand/left/input/menu/click"}});
   }
   suggest("/interaction_profiles/khr/simple_controller",
           {{state.fire_action, "/user/hand/right/input/select/click"},
            {state.aim_action, aim_pose},
            {state.grip_action, grip_pose},
            {state.haptic_action, "/user/hand/right/output/haptic"}});
}

auto pick_swapchain_format() noexcept -> DXGI_FORMAT
{
   uint32_t count = 0;
   state.xr.xrEnumerateSwapchainFormats(state.session, 0, &count, nullptr);
   std::vector<int64_t> formats(count);
   state.xr.xrEnumerateSwapchainFormats(state.session, count, &count, formats.data());

   // The game's frame holds sRGB-encoded values in an R8G8B8A8_UNORM texture. Copying
   // the raw bytes into an _SRGB swapchain makes the compositor decode them correctly.
   for (const DXGI_FORMAT preferred :
        {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM}) {
      for (const int64_t format : formats) {
         if (format == preferred) return preferred;
      }
   }

   return DXGI_FORMAT_UNKNOWN;
}

void destroy_swapchain() noexcept
{
   if (state.swapchain) state.xr.xrDestroySwapchain(state.swapchain);

   state.swapchain = XR_NULL_HANDLE;
   state.swapchain_images.clear();
   state.swapchain_width = 0;
   state.swapchain_height = 0;
}

bool ensure_swapchain(const UINT width, const UINT height) noexcept
{
   if (state.swapchain && state.swapchain_width == width &&
       state.swapchain_height == height) {
      return true;
   }

   destroy_swapchain();

   if (state.swapchain_format == DXGI_FORMAT_UNKNOWN) {
      state.swapchain_format = pick_swapchain_format();

      if (state.swapchain_format == DXGI_FORMAT_UNKNOWN) {
         log(Log_level::warning, "VR: runtime offers no R8G8B8A8 swapchain format.");

         return false;
      }
   }

   XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
   info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
   info.format = state.swapchain_format;
   info.sampleCount = 1;
   info.width = width;
   info.height = height;
   info.faceCount = 1;
   info.arraySize = 1;
   info.mipCount = 1;

   if (failed(state.xr.xrCreateSwapchain(state.session, &info, &state.swapchain),
              "xrCreateSwapchain")) {
      return false;
   }

   uint32_t count = 0;
   state.xr.xrEnumerateSwapchainImages(state.swapchain, 0, &count, nullptr);
   std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
   state.xr.xrEnumerateSwapchainImages(state.swapchain, count, &count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(
                                          images.data()));

   for (const auto& image : images) state.swapchain_images.push_back(image.texture);

   state.swapchain_width = width;
   state.swapchain_height = height;

   log_fmt(Log_level::info, "VR: swapchain {}x{}", width, height);

   return true;
}

void place_panel() noexcept;

// Recenter (the panel's button, or the runtime's own recenter): the next gameplay camera
// takes the head's pose anew (seated: its height too).
void request_recenter() noexcept
{
   state.recentered = false;
   state.measure_player = true;
   state.screen_place_pending = true;

   if (state.panel_open) place_panel();
}

// Whether the right hand's controller is one whose pad is its stick (Vive wands).
void update_interaction_profile() noexcept
{
   if (!state.session || !state.xr.xrGetCurrentInteractionProfile) return;

   XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};

   if (XR_FAILED(state.xr.xrGetCurrentInteractionProfile(
          state.session, make_path("/user/hand/right"), &profile))) {
      return;
   }

   std::array<char, XR_MAX_PATH_LENGTH> name{};
   uint32_t length = 0;

   if (profile.interactionProfile != XR_NULL_PATH) {
      state.xr.xrPathToString(state.instance, profile.interactionProfile,
                              static_cast<uint32_t>(name.size()), &length, name.data());
   }

   state.right_stick_is_pad =
      std::strcmp(name.data(), "/interaction_profiles/htc/vive_controller") == 0;

   log_fmt(Log_level::info, "VR: the right controller is {}.",
           name[0] ? name.data() : "not known yet");
}

void handle_session_state(const XrSessionState new_state) noexcept
{
   // A system menu over the match (focus lost): pause the game (update_input).
   if (state.session_state == XR_SESSION_STATE_FOCUSED &&
       new_state != XR_SESSION_STATE_FOCUSED && state.input_gameplay &&
       settings().pause_on_focus_loss) {
      state.focus_pause_frames = key_tap_frames;

      log(Log_level::info, "VR: focus lost in a match, pausing the game.");
   }

   state.session_state = new_state;

   log_fmt(Log_level::info, "VR: session state {}.", static_cast<int>(new_state));

   switch (new_state) {
   case XR_SESSION_STATE_READY: {
      XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
      begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

      if (!failed(state.xr.xrBeginSession(state.session, &begin_info), "xrBeginSession")) {
         state.session_running = true;
         log(Log_level::info, "VR: session running.");
      }
   } break;
   case XR_SESSION_STATE_STOPPING:
      state.session_running = false;
      state.frame_begun = false;
      state.views_valid = false;
      state.xr.xrEndSession(state.session);
      log(Log_level::info, "VR: session stopped.");
      break;
   case XR_SESSION_STATE_EXITING:
   case XR_SESSION_STATE_LOSS_PENDING:
      state.session_running = false;
      state.frame_begun = false;
      state.views_valid = false;
      destroy_swapchain();
      state.xr.xrDestroySession(state.session);
      state.session = XR_NULL_HANDLE;
      log(Log_level::info, "VR: session ended by the runtime.");
      break;
   default:
      break;
   }
}

void poll_events() noexcept
{
   XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};

   while (state.instance && state.xr.xrPollEvent(state.instance, &event) == XR_SUCCESS) {
      if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
         handle_session_state(
            reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
      }
      else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
         // The runtime's own recenter (a long press of the Oculus button, SteamVR's Reset
         // seated position) moves LOCAL space: recenter the game on it as our button does.
         const auto& change =
            reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);

         if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
            log(Log_level::info, "VR: the runtime recentered, recentering the game.");

            request_recenter();
         }
      }
      else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
         update_interaction_profile();
      }

      event = {XR_TYPE_EVENT_DATA_BUFFER};
   }
}

// Where a ray (LOCAL space) meets a quad layer's front (`size` metres): 0..1 from its
// top left (outside that when it misses), or nullopt when the ray points away from it.
auto ray_on_quad(const XrPosef& pose, const glm::vec2 size, const glm::vec3 origin,
                 const glm::vec3 direction) noexcept -> std::optional<glm::vec2>
{
   const XrQuaternionf inverse = conjugate(pose.orientation);
   const glm::vec3 local_origin = rotate(inverse, origin - to_vec3(pose.position));
   const glm::vec3 local_direction = rotate(inverse, direction);

   if (local_direction.z > -1e-3f) return std::nullopt;

   const float t = -local_origin.z / local_direction.z;

   if (t <= 0.0f) return std::nullopt;

   const glm::vec3 hit = local_origin + t * local_direction;

   return glm::vec2{hit.x / size.x + 0.5f, 0.5f - hit.y / size.y};
}

auto panel_size() noexcept -> glm::vec2
{
   const float metres_per_pixel = state.panel_size_m / static_cast<float>(panel_window_px);

   return {metres_per_pixel * static_cast<float>(panel_texture_width_px),
           metres_per_pixel * static_cast<float>(panel_height_px)};
}

// A yaw-only rotation whose -Z is `forward` (horizontal): a quad facing back along it.
auto yaw_rotation(const glm::vec3 forward) noexcept -> XrQuaternionf
{
   const float angle = std::atan2(-forward.x, -forward.z);

   return {0.0f, std::sin(angle / 2.0f), 0.0f, std::cos(angle / 2.0f)};
}

// Midpoint of the eyes and the head's horizontal forward, in tracking space.
auto head_tracking() noexcept -> std::pair<glm::vec3, glm::vec3>
{
   const glm::vec3 head =
      (to_vec3(state.views[0].pose.position) + to_vec3(state.views[1].pose.position)) * 0.5f;
   const glm::vec3 forward =
      horizontal(rotate(state.views[0].pose.orientation, {0.0f, 0.0f, -1.0f}));

   return {head, forward != glm::vec3{} ? forward : glm::vec3{0.0f, 0.0f, -1.0f}};
}

// The menu screen in front of the head (it stays there while it's shown), the VR panel
// to its right, turned to the head.
void place_screen() noexcept
{
   if (!state.views_valid) return;

   const auto [head, forward] = head_tracking();
   const glm::vec3 right = glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f});
   const glm::vec3 center = head + forward * screen_distance;

   state.screen_pose = {yaw_rotation(forward), {center.x, center.y, center.z}};
   state.screen_place_pending = false;

   state.panel_size_m = 0.9f;

   const glm::vec3 panel_center =
      head + forward * 1.3f + right * (screen_width * 0.5f + 0.1f + panel_size().x * 0.5f);
   const glm::vec3 to_panel = horizontal(panel_center - head);

   state.panel_pose = {yaw_rotation(to_panel != glm::vec3{} ? to_panel : forward),
                       {panel_center.x, panel_center.y, panel_center.z}};
}

// The VR panel's settings window in front of the head, a little below the eyes (opened
// in a match), the hands' window right of it.
void place_panel() noexcept
{
   if (!state.views_valid) return;

   const auto [head, forward] = head_tracking();
   const glm::vec3 right = glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f});

   state.panel_size_m = panel_width;

   const glm::vec3 center = head + forward * panel_distance - glm::vec3{0.0f, 0.12f, 0.0f} +
                            right * ((panel_size().x - state.panel_size_m) * 0.5f);

   state.panel_pose = {yaw_rotation(forward), {center.x, center.y, center.z}};
}

void toggle_panel() noexcept
{
   state.panel_open = !state.panel_open;

   if (state.panel_open) place_panel();

   log(Log_level::info, state.panel_open ? "VR: panel opened." : "VR: panel closed.");
}

// The game's mouse (0x99d138, updated by FUN_006da560 from the mouse's DIMOUSESTATE)
// keeps its cursor at +0x14 / +0x18, clamped to [0, the screen's size] (0x93e4a4 /
// 0x93e4a8) and moved 1:1 by the mouse's counts. Moved by counts towards where the
// hand points, it drifted from the pointer (and stopped at "invisible edges"): it's
// written directly instead.
constexpr std::uintptr_t exe_mouse = 0x0099d138;
constexpr std::uintptr_t exe_mouse_update_this = 0x006dc20b; // MOV ECX, exe_mouse
constexpr std::uintptr_t exe_screen_limits = 0x0093e4a4;

bool game_cursor_ready() noexcept
{
   if (!std::exchange(state.cursor_checked, true)) {
      const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
      const auto mouse = static_cast<std::uint32_t>(exe_mouse - exe_image_base + base);
      std::uint8_t expected[5] = {0xB9};

      std::memcpy(expected + 1, &mouse, sizeof(mouse));

      if (std::memcmp(reinterpret_cast<const void*>(exe_mouse_update_this - exe_image_base + base),
                      expected, sizeof(expected)) == 0) {
         state.cursor_position = reinterpret_cast<std::int32_t*>(mouse + 0x14);
         state.cursor_limits = reinterpret_cast<const std::int32_t*>(exe_screen_limits -
                                                                    exe_image_base + base);
      }

      log(Log_level::info, state.cursor_position
                              ? "VR: the menu cursor goes where the right hand points."
                              : "VR: unknown game executable, the menu cursor is moved with "
                                "mouse counts.");
   }

   return state.cursor_position != nullptr;
}

// Where the right hand's aim ray points: on the VR panel, or on the menu screen. On
// the screen the game's cursor is put there (or, on an unknown exe, moved there with
// mouse counts: pinned to the top-left corner once, then moved by the change of the
// pointed-at pixel). Windows cursor readers get it from GetCursorPos.
void update_menu_pointer() noexcept
{
   state.pointer_valid = false;
   state.panel_hit = false;
   state.panel_pointer = {-1.0f, -1.0f};

   if (state.aim_valid) {
      const glm::vec3 origin = to_vec3(state.aim_pose.position);
      const glm::vec3 direction = rotate(state.aim_pose.orientation, {0.0f, 0.0f, -1.0f});

      if (state.panel_shown) {
         const auto uv = ray_on_quad(state.panel_pose, panel_size(), origin, direction);

         if (uv && uv->x >= 0.0f && uv->x <= 1.0f && uv->y >= 0.0f && uv->y <= 1.0f) {
            state.panel_hit = true;
            state.panel_pointer = *uv;
         }
      }

      if (state.screen_shown && !state.panel_hit && !state.vehicle_controls &&
          state.screen_size.x > 0.0f) {
         if (const auto uv = ray_on_quad(state.screen_pose, state.screen_size, origin, direction)) {
            state.pointer_uv = glm::clamp(*uv, 0.0f, 1.0f);
            state.pointer_valid = true;
         }
      }
   }

   if (!state.pointer_valid || state.window_size.x == 0) {
      state.pointer_homed = false;

      return;
   }

   if (!std::exchange(state.pointer_logged, true)) {
      log_fmt(Log_level::info, "VR: right hand points at the virtual screen ({:.2f}, {:.2f}).",
              state.pointer_uv.x, state.pointer_uv.y);
   }

   if (game_cursor_ready()) {
      state.cursor_position[0] = static_cast<std::int32_t>(
         state.pointer_uv.x * static_cast<float>(state.cursor_limits[0]) + 0.5f);
      state.cursor_position[1] = static_cast<std::int32_t>(
         state.pointer_uv.y * static_cast<float>(state.cursor_limits[1]) + 0.5f);

      return;
   }

   const glm::vec2 window{state.window_size};
   const glm::vec2 target = state.pointer_uv * window;

   if (!state.pointer_homed) {
      state.pointer_homed = true;
      state.pointer_last_px = {};
      state.mouse_remainder = {};
      mouse_dx -= static_cast<LONG>(window.x * 2.0f);
      mouse_dy -= static_cast<LONG>(window.y * 2.0f);
   }

   const glm::vec2 counts =
      (target - state.pointer_last_px) * menu_counts_per_pixel + state.mouse_remainder;
   const glm::vec2 whole = glm::trunc(counts);

   state.mouse_remainder = counts - whole;
   state.pointer_last_px = target;
   mouse_dx += static_cast<LONG>(whole.x);
   mouse_dy += static_cast<LONG>(whole.y);
}

// Turns the player: rotates the tracking -> world mapping about the up axis (positive =
// to the right). The view snaps; mouse-follow then turns the game camera after it.
void snap_turn(const float degrees) noexcept
{
   const float angle = degrees * std::numbers::pi_v<float> / 180.0f;
   const glm::vec3 right = state.world_right;
   const glm::vec3 forward = state.world_forward;

   state.world_forward = std::cos(angle) * forward + std::sin(angle) * right;
   state.world_right = std::cos(angle) * right - std::sin(angle) * forward;
}

// Rank (by size, last frame) of the model part with this origin, or -1.
auto ranked_part_index(const glm::vec3 origin_view) noexcept -> int
{
   int best = -1;
   float best_distance = part_match_tolerance;

   for (int i = 0; i < static_cast<int>(state.ranked_parts.size()); ++i) {
      const float distance = glm::distance(state.ranked_parts[i].origin_view, origin_view);

      if (distance < best_distance) {
         best = i;
         best_distance = distance;
      }
   }

   return best;
}

// The hand models the player picked (2026-10-19, rifle with 5 parts: the right hand the
// gun and part 2, the left hand part 3), kept by meaning for models with other part
// counts.
auto default_right_hand_parts(const int n) noexcept -> std::uint32_t
{
   return n >= 2 ? 0b011u : 0b001u;
}

auto default_left_hand_parts(const int n) noexcept -> std::uint32_t
{
   return n >= 3 ? 0b100u : 0u;
}

void default_hand_parts(const int n) noexcept
{
   state.right_hand_parts = default_right_hand_parts(n);
   state.left_hand_parts = default_left_hand_parts(n);

   log_fmt(Log_level::info, "VR: {} first-person model parts, default hand models: right "
                            "parts 0x{:x}, left parts 0x{:x}.",
           n, state.right_hand_parts, state.left_hand_parts);
}

// The game's 2D HUD (drawn into the HUD layer) is shown in front of the eyes, turned
// with the head, except its crosshair region, which goes along the gun (see draw_hud).
// With the wrist HUD, pieces of it (regions of the screen, u0 v0 u1 v1 from the top
// left) are also shown on black panels on the wrists, laid out in rows. bf2vr_hud.txt
// overrides the regions ("name u0 v0 u1 v1") and panels ("name width back up side":
// metres, the panel's centre from the hand's aim pose).
enum Hud_region {
   status,
   score_blue,
   score_red,
   messages,
   map,
   capture,
   crosshair,
   hud_region_count
};

constexpr std::array<const char*, hud_region_count> hud_region_names{
   "status", "score_blue", "score_red", "messages", "map", "capture", "crosshair"};

enum Hud_panel { left_up, right_up, hud_panel_count };

constexpr std::array<const char*, hud_panel_count> hud_panel_names{"left_up", "right_up"};

struct Hud_panel_config {
   float width;
   glm::vec3 offset; // back, up, side (the hand's +Z, +Y, +X)
};

struct Hud_config {
   bool loaded = false;

   std::array<glm::vec4, hud_region_count> regions{
      glm::vec4{0.000f, 0.760f, 0.230f, 0.960f}, // health, stamina, weapons, ammo
      glm::vec4{0.250f, 0.060f, 0.400f, 0.170f}, // our reinforcements
      glm::vec4{0.600f, 0.060f, 0.750f, 0.170f}, // theirs
      glm::vec4{0.000f, 0.560f, 0.320f, 0.760f}, // messages (a guess)
      glm::vec4{0.845f, 0.050f, 0.995f, 0.285f}, // minimap
      glm::vec4{0.845f, 0.290f, 0.995f, 0.370f}, // command post capture (a guess)
      glm::vec4{0.440f, 0.410f, 0.560f, 0.590f}, // crosshair (the screen's centre)
   };

   std::array<Hud_panel_config, hud_panel_count> panels{
      Hud_panel_config{0.12f, {0.16f, 0.03f, 0.0f}},
      Hud_panel_config{0.08f, {0.16f, 0.03f, 0.0f}},
   };
};

Hud_config hud_config;

// Each panel's rows, top first; a row's regions side by side, scaled to the panel width.
const std::array<std::vector<std::vector<Hud_region>>, hud_panel_count> hud_panel_rows{
   std::vector<std::vector<Hud_region>>{{score_blue, score_red}, {messages}, {status}},
   std::vector<std::vector<Hud_region>>{{map}, {capture}},
};

constexpr float hud_row_gap = 0.004f;
constexpr float hud_panel_margin = 0.006f;

void load_hud_config() noexcept
{
   if (std::exchange(hud_config.loaded, true)) return;

   FILE* file = nullptr;

   if (_wfopen_s(&file, (module_directory() + L"bf2vr_hud.txt").c_str(), L"r") != 0 || !file) {
      log(Log_level::info, "VR: wrist HUD with the default layout (no bf2vr_hud.txt).");

      return;
   }

   char line[256]{};
   int read = 0;

   while (std::fgets(line, sizeof(line), file)) {
      char name[64]{};
      float v[4]{};

      if (std::sscanf(line, "%63s %f %f %f %f", name, &v[0], &v[1], &v[2], &v[3]) != 5) {
         continue;
      }

      const std::string_view key = name;

      for (int i = 0; i < hud_region_count; ++i) {
         if (key == hud_region_names[i]) {
            hud_config.regions[i] = {v[0], v[1], v[2], v[3]};
            read += 1;
         }
      }

      for (int i = 0; i < hud_panel_count; ++i) {
         if (key == hud_panel_names[i]) {
            hud_config.panels[i] = {v[0], {v[1], v[2], v[3]}};
            read += 1;
         }
      }
   }

   std::fclose(file);

   log_fmt(Log_level::info, "VR: wrist HUD layout: {} entries from bf2vr_hud.txt.", read);
}

// A panel's quads (black backing first) at the hand. `layer_size`: the HUD's size in
// the layer, which spans its left `u_scale` of it.
// A wrist panel's centre and text axes, its face towards the eyes when the wrist is
// turned to them like a watch (on top of the wrist).
struct Hud_panel_frame {
   glm::vec3 center;
   glm::vec3 text_right;
   glm::vec3 text_up;
};

auto hud_panel_frame(const Hud_panel panel, const World_pose& hand) noexcept -> Hud_panel_frame
{
   const glm::vec3 hand_right = hand.rotation[0];
   const glm::vec3 hand_up = hand.rotation[1];
   const glm::vec3 hand_back = hand.rotation[2];
   const Hud_panel_config& config = hud_config.panels[panel];

   return {.center = hand.position + hand_back * config.offset.x + hand_up * config.offset.y +
                     hand_right * config.offset.z,
           .text_right = panel == left_up ? -hand_back : hand_back,
           .text_up = panel == left_up ? -hand_right : hand_right};
}

// Returns the panel's half height (metres, with its margin).
auto add_hud_panel(const Hud_panel panel, const World_pose& hand,
                   const glm::uvec2 layer_size, const float u_scale,
                   std::vector<Hud_quad>& quads) noexcept -> float
{
   const auto [center, text_right, text_up] = hud_panel_frame(panel, hand);
   const Hud_panel_config& config = hud_config.panels[panel];
   const glm::vec2 layer{layer_size};
   const auto& rows = hud_panel_rows[panel];

   // Row heights at the panel width.
   std::vector<float> scales;
   std::vector<float> heights;
   float total_height = 0.0f;

   for (const auto& row : rows) {
      float width_px = 0.0f;
      float height_px = 0.0f;

      for (const Hud_region region : row) {
         const glm::vec4 r = hud_config.regions[region];

         width_px += std::max(r.z - r.x, 0.0f) * layer.x;
         height_px = std::max(height_px, std::max(r.w - r.y, 0.0f) * layer.y);
      }

      const float scale = width_px > 0.0f ? config.width / width_px : 0.0f;

      scales.push_back(scale);
      heights.push_back(height_px * scale);
      total_height += height_px * scale;
   }

   total_height += hud_row_gap * static_cast<float>(rows.size() - 1);

   quads.push_back({.center = center,
                    .right = text_right * (config.width / 2.0f + hud_panel_margin),
                    .up = text_up * (total_height / 2.0f + hud_panel_margin),
                    .uv = {},
                    .backing = true});

   float top = total_height / 2.0f;

   for (std::size_t i = 0; i < rows.size(); ++i) {
      const float row_y = top - heights[i] / 2.0f;
      float left = -config.width / 2.0f;

      for (const Hud_region region : rows[i]) {
         const glm::vec4 r = hud_config.regions[region];
         const float width = std::max(r.z - r.x, 0.0f) * layer.x * scales[i];
         const float height = std::max(r.w - r.y, 0.0f) * layer.y * scales[i];

         quads.push_back({.center = center + text_right * (left + width / 2.0f) + text_up * row_y,
                          .right = text_right * (width / 2.0f),
                          .up = text_up * (height / 2.0f),
                          .uv = {r.x * u_scale, r.y, r.z * u_scale, r.w},
                          .backing = false});

         left += width;
      }

      top -= heights[i] + hud_row_gap;
   }

   return total_height / 2.0f + hud_panel_margin;
}

// The quick reload bar (see update_quick_reload), `width` long along `right` from
// `center`, its face spanned by `right` and `up`: the reload's progress as a marker
// over a grey bar, the blue window while it's still to try, all green after a hit.
void add_reload_bar(const glm::vec3 center, const glm::vec3 right, const glm::vec3 up,
                    const float width, std::vector<Hud_quad>& quads) noexcept
{
   const bool hit = state.reload_flash > 0.0f;

   if (!state.reload_weapon && !hit) return;

   const float height = width * 0.07f;

   // Colours premultiplied.
   const auto bar = [&](const float from, const float to, const float thickness,
                        const glm::vec4 color) {
      quads.push_back({.center = center + right * (((from + to) * 0.5f - 0.5f) * width),
                       .right = right * ((to - from) * 0.5f * width),
                       .up = up * (thickness * 0.5f),
                       .uv = {},
                       .backing = false,
                       .solid = true,
                       .color = color});
   };

   bar(-0.03f, 1.03f, height * 2.0f, {0.0f, 0.0f, 0.0f, 0.6f});

   if (hit) {
      bar(0.0f, 1.0f, height, {0.25f, 0.9f, 0.4f, 1.0f});

      return;
   }

   bar(0.0f, 1.0f, height, {0.3f, 0.3f, 0.32f, 1.0f});

   if (!state.reload_window_gone) {
      bar(state.reload_window.x, state.reload_window.y, height, {0.15f, 0.6f, 1.0f, 1.0f});
   }

   const float marker = state.reload_progress;

   bar(marker - 0.01f, marker + 0.01f, height * 2.4f, {1.0f, 1.0f, 1.0f, 1.0f});
}

// Checks which frame the first-person model follows (see Weapon_frame) from how much the
// gun's origin moves in each, and switches to the other when it keeps the gun stiller.
void update_weapon_frame(const Model_part& gun) noexcept
{
   if (state.gun_origin_known) {
      state.gun_movement += glm::distance(gun.origin_view, state.gun_origin);
      state.gun_movement_other += glm::distance(gun.origin_other, state.gun_origin_other);
   }

   state.gun_origin = gun.origin_view;
   state.gun_origin_other = gun.origin_other;
   state.gun_origin_known = true;

   if (++state.weapon_frame_frames < weapon_frame_check_frames) return;

   if (state.gun_movement > weapon_frame_min_movement &&
       state.gun_movement_other < state.gun_movement * weapon_frame_switch_ratio) {
      state.weapon_frame = state.weapon_frame == Weapon_frame::aim ? Weapon_frame::render
                                                                   : Weapon_frame::aim;

      // The parts' origins are now taken in the other frame.
      for (Model_part& part : state.ranked_parts) {
         std::swap(part.origin_view, part.origin_other);
      }

      std::swap(state.gun_origin, state.gun_origin_other);

      log_fmt(Log_level::info,
              "VR: the first-person model follows the {} (gun moved {:.2f} m in the other "
              "frame, {:.2f} m in this one).",
              state.weapon_frame == Weapon_frame::aim ? "aim" : "camera as drawn",
              state.gun_movement, state.gun_movement_other);
   }

   state.gun_movement = 0.0f;
   state.gun_movement_other = 0.0f;
   state.weapon_frame_frames = 0;
}

void haptic_pulse(const int hand, const float amplitude, const float seconds,
                  const float frequency = 0.0f) noexcept;

// A game control pressed for a few frames.
// One step of a One Euro filter on `pose` (see hand_filter_min_hz), in place.
void filter_pose(State::Pose_filter& filter, XrPosef& pose, const float dt,
                 const float smoothing) noexcept
{
   const glm::vec3 position{pose.position.x, pose.position.y, pose.position.z};
   const glm::quat rotation{pose.orientation.w, pose.orientation.x, pose.orientation.y,
                            pose.orientation.z};

   if (!filter.valid || dt <= 0.0f || dt > hand_filter_max_gap) {
      filter = {.valid = true, .position = position, .rotation = rotation};

      return;
   }

   const auto alpha = [dt](const float hz) {
      const float tau = 1.0f / (2.0f * std::numbers::pi_v<float> * hz);

      return 1.0f / (1.0f + tau / dt);
   };

   // The speeds from the raw pose against the filtered one.
   const float speed = glm::distance(position, filter.position) / dt;
   const float cos_half = std::min(std::abs(glm::dot(rotation, filter.rotation)), 1.0f);
   const float angular_speed = 2.0f * std::acos(cos_half) / dt;
   const float speed_alpha = alpha(hand_filter_speed_hz);

   filter.speed += (speed - filter.speed) * speed_alpha;
   filter.angular_speed += (angular_speed - filter.angular_speed) * speed_alpha;

   const float rest_hz = hand_filter_min_hz * std::pow(hand_filter_range, smoothing);

   filter.position +=
      (position - filter.position) * alpha(rest_hz + hand_filter_beta_position * filter.speed);
   filter.rotation = glm::normalize(glm::slerp(
      filter.rotation, rotation, alpha(rest_hz + hand_filter_beta_rotation * filter.angular_speed)));

   pose.position = {filter.position.x, filter.position.y, filter.position.z};
   pose.orientation = {filter.rotation.x, filter.rotation.y, filter.rotation.z,
                       filter.rotation.w};
}

// Controller smoothing (settings hand_smoothing): the right grip and the left aim are
// filtered; the right aim keeps its offset from the grip (a rigid part of the same
// controller), so the gun and the saber move as one.
void smooth_hands(const float dt) noexcept
{
   const float smoothing = settings().hand_smoothing;

   if (smoothing <= 0.0f) {
      state.right_filter.valid = false;
      state.left_filter.valid = false;

      return;
   }

   if (state.grip_valid) {
      const XrPosef raw_grip = state.grip_pose;

      filter_pose(state.right_filter, state.grip_pose, dt, smoothing);

      if (state.aim_valid) {
         // The aim relative to the grip, put on the filtered grip.
         const XrQuaternionf inverse = conjugate(raw_grip.orientation);
         const glm::vec3 offset = rotate(inverse, to_vec3(state.aim_pose.position) -
                                                     to_vec3(raw_grip.position));
         const glm::quat grip_raw{raw_grip.orientation.w, raw_grip.orientation.x,
                                  raw_grip.orientation.y, raw_grip.orientation.z};
         const glm::quat aim_raw{state.aim_pose.orientation.w, state.aim_pose.orientation.x,
                                 state.aim_pose.orientation.y, state.aim_pose.orientation.z};
         const glm::quat grip{state.grip_pose.orientation.w, state.grip_pose.orientation.x,
                              state.grip_pose.orientation.y, state.grip_pose.orientation.z};
         const glm::quat aim = glm::normalize(grip * (glm::inverse(grip_raw) * aim_raw));
         const glm::vec3 position =
            to_vec3(state.grip_pose.position) + rotate(state.grip_pose.orientation, offset);

         state.aim_pose.position = {position.x, position.y, position.z};
         state.aim_pose.orientation = {aim.x, aim.y, aim.z, aim.w};
      }
   }
   else if (state.aim_valid) {
      filter_pose(state.right_filter, state.aim_pose, dt, smoothing);
   }
   else {
      state.right_filter.valid = false;
   }

   if (state.left_aim_valid) filter_pose(state.left_filter, state.left_aim_pose, dt, smoothing);
   else state.left_filter.valid = false;
}

void tap(const Game_control control) noexcept
{
   state.tap_frames[control] = key_tap_frames;
}

auto to_quat(const XrQuaternionf& q) noexcept -> glm::quat
{
   return glm::quat{q.w, q.x, q.y, q.z};
}

auto to_xr(const glm::quat& q) noexcept -> XrQuaternionf
{
   return {q.x, q.y, q.z, q.w};
}

// A saber's hand pose (0 right, 1 left): the grip's (through the fist), the aim's
// without one.
auto saber_hand_pose(const int hand = 0) noexcept -> const XrPosef&
{
   if (hand == 1) return state.left_grip_valid ? state.left_grip_pose : state.left_aim_pose;

   return state.grip_valid ? state.grip_pose : state.aim_pose;
}

// The saber's rotation in that hand now (tracking space).
auto saber_rotation(const int hand) noexcept -> glm::quat
{
   const State::Saber_turn& turn = state.saber_turns[hand];

   return turn.held_still
             ? turn.still_rotation
             : glm::normalize(to_quat(saber_hand_pose(hand).orientation) * turn.offset);
}

// Hand swap: the left controller as the right (aim, grip, trigger, grip button,
// vibration) and the other way round. This frame's poses are swapped too.
void set_handles_swapped(const bool swapped) noexcept
{
   if (swapped == state.handles_swapped) return;

   state.handles_swapped = swapped;

   std::swap(state.aim_space, state.left_aim_space);
   std::swap(state.grip_space, state.left_grip_space);
   std::swap(state.fire_action, state.secondary_fire_action);
   std::swap(state.block_action, state.two_hand_action);
   std::swap(state.hand_paths[0], state.hand_paths[1]);
   std::swap(state.aim_pose, state.left_aim_pose);
   std::swap(state.aim_valid, state.left_aim_valid);
   std::swap(state.grip_pose, state.left_grip_pose);
   std::swap(state.grip_valid, state.left_grip_valid);

   state.right_filter.valid = false;
   state.left_filter.valid = false;
}

// Hand swap (settings hand_swap): the hands brought together (both vibrate lightly)
// and the free hand's grip pressed, or near each other and that grip clicked twice, the
// weapon goes to the free hand (dual sabers: each saber to the other hand). The sabers
// keep their turn relative to the hands (hand_swap_keeps_rotation) or take the default.
// Not in vehicles (the swap waits).
void update_hand_swap(const bool on_foot, const bool melee_in_hand, const bool weapon_grip,
                      const bool free_grip, const float dt) noexcept
{
   const Settings& s = settings();
   const bool pressed = free_grip && !state.swap_grip_down;

   state.swap_grip_down = free_grip;
   state.swap_press_age += dt;

   const bool allowed =
      s.hand_swap && on_foot && !state.panel_open && state.grip_valid && state.left_grip_valid;
   const float apart = glm::distance(to_vec3(state.grip_pose.position),
                                     to_vec3(state.left_grip_pose.position));

   state.hands_together =
      allowed && apart < (state.hands_together ? hands_apart_distance : hands_together_distance);

   // (Not through a gun's two-handed hold: the hands may be as close as that on a pistol.)
   if (state.hands_together && !state.two_handed &&
       state.input_clock >= state.hands_together_buzz_due) {
      haptic_pulse(0, hands_together_amplitude, hands_together_buzz_seconds);
      haptic_pulse(1, hands_together_amplitude, hands_together_buzz_seconds);
      state.hands_together_buzz_due = state.input_clock + hands_together_buzz_seconds;
   }

   if (!pressed) return;

   const bool double_click = state.swap_press_age < scope_double_click_seconds;

   state.swap_press_age = double_click ? 1e9f : 0.0f;

   const bool hands_near = allowed && apart < hands_near_distance;

   if (weapon_grip || !(state.hands_together || (hands_near && double_click))) return;

   // The sabers' rotations now (tracking space), for the new hands to keep.
   const std::array<glm::quat, 2> sabers{saber_rotation(0), saber_rotation(1)};
   const bool keep = melee_in_hand && s.hand_swap_keeps_rotation;

   state.hands_swapped = !state.hands_swapped;
   set_handles_swapped(state.hands_swapped);

   // The saber that was in hand i is in the other one now (dual sabers: both).
   for (int i = 0; i < 2; ++i) {
      State::Saber_turn& turn = state.saber_turns[i];

      turn.offset = keep ? glm::normalize(glm::inverse(to_quat(saber_hand_pose(i).orientation)) *
                                          sabers[1 - i])
                         : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
      turn.held_still = false;
      turn.press_age = 1e9f;
      // The grip that took the weapon is held: it doesn't start a turn.
      turn.grip_ignored = true;
   }

   state.saber_turns[0].grip_down = true;
   state.swap_grip_down = false;

   haptic_pulse(0, 0.6f, 0.06f);
   haptic_pulse(1, 0.6f, 0.06f);

   log_fmt(Log_level::info, "VR: the weapon is in the {} hand now ({}, {}).",
           state.hands_swapped ? "left" : "right",
           state.hands_together ? "hands together" : "a double click near",
           keep ? "its rotation kept" : "default rotation");
}

// Saber rotation, per hand (the left with dual sabers): with its grip held (and not the
// other's: both block) the saber keeps its rotation, still moving with the hand; let go,
// the hand's turn since is kept as the saber's offset from the hand. A double click of
// the grip clears the offset. Gives each hand's saber_turns[].pose each frame.
void update_saber_turn(const int hand_index, const bool active, const bool grip,
                       const bool other_grip, const float dt) noexcept
{
   State::Saber_turn& turn = state.saber_turns[hand_index];
   const bool pressed = grip && !turn.grip_down;
   const bool released = !grip && turn.grip_down;
   const glm::quat hand = to_quat(saber_hand_pose(hand_index).orientation);
   const char* const name = hand_index == 0 ? "weapon hand's" : "other hand's";

   turn.grip_down = grip;
   turn.press_age += dt;

   if (!active) {
      turn.held_still = false;

      if (!grip) turn.grip_ignored = false;
   }
   else {
      if (pressed) {
         if (turn.press_age < scope_double_click_seconds) {
            turn.offset = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            turn.held_still = false;
            turn.grip_ignored = true;
            turn.press_age = 1e9f;

            haptic_pulse(hand_index == 0 ? 1 : 0, 0.4f, 0.04f);
            log_fmt(Log_level::info, "VR: the {} saber's rotation is the default again.", name);
         }
         else {
            turn.press_age = 0.0f;

            if (!other_grip && !turn.grip_ignored) {
               turn.held_still = true;
               turn.still_rotation = glm::normalize(hand * turn.offset);
            }
         }
      }

      // Both grips: the block, with the saber as it was turned before.
      if (turn.held_still && other_grip) {
         turn.held_still = false;
         turn.grip_ignored = true;
      }

      if (released) {
         if (turn.held_still && !turn.grip_ignored) {
            turn.offset = glm::normalize(glm::inverse(hand) * turn.still_rotation);

            log_fmt(Log_level::info, "VR: the {} saber keeps a turn from the hand.", name);
         }

         turn.held_still = false;
         turn.grip_ignored = false;
      }
   }

   turn.pose = saber_hand_pose(hand_index);
   turn.pose.orientation =
      to_xr(turn.held_still ? turn.still_rotation : glm::normalize(hand * turn.offset));
}

void update_saber_rotation(const bool active, const bool dual, const bool weapon_grip,
                           const bool other_grip, const float dt) noexcept
{
   update_saber_turn(0, active, weapon_grip, other_grip, dt);
   update_saber_turn(1, active && dual, other_grip, weapon_grip, dt);
}

// Physical crouch (settings physical_crouch, see crouch_enter_fraction): the player
// crouching in the room crouches the soldier, standing up stands it up again (only a
// crouch of ours: one from the button stays). Never with the stick sideways: crouching
// while strafing rolls. The button works in both play modes: its press from standing
// teaches the crouch's soldier state (the eyes drop by it, see
// body_crouch_default_fraction), and after a press no tap of ours fights it until the
// player stands up straight in the room.
void update_body_crouch(const float dt, const bool gameplay, const glm::vec2 move,
                        const bool button) noexcept
{
   const std::int32_t soldier_state = state.body_soldier_state;
   const bool on_foot = state.body_anchored && soldier_state >= 0 && soldier_state < body_states;
   const bool button_pressed = button && !state.crouch_button_down;

   state.crouch_button_down = button;

   if (gameplay && on_foot && button_pressed) {
      state.crouch_button_owns = true;

      if (soldier_state == 0 && !state.crouch_state_learned) state.crouch_button_watch = 0.0f;
   }

   if (state.crouch_button_watch >= 0.0f) {
      state.crouch_button_watch += dt;

      if (!on_foot || state.crouch_state_learned ||
          state.crouch_button_watch > crouch_answer_seconds) {
         state.crouch_button_watch = -1.0f;
      }
      else if (soldier_state != 0 && soldier_state != 3 && soldier_state != 4) { // not sprint, jump
         state.crouch_state_learned = true;
         state.crouch_state = soldier_state;
         state.crouch_button_watch = -1.0f;

         log_fmt(Log_level::info, "VR: the soldier's crouch is soldier state {} (button).",
                 soldier_state);
      }
   }

   if (state.crouch_button_owns && state.body_fraction > crouch_leave_fraction) {
      state.crouch_button_owns = false;
   }

   if (!settings().physical_crouch || settings().play_mode != Play_mode::standing || !gameplay ||
       !on_foot || state.crouch_given_up) {
      state.crouch_by_body = false;
      state.crouch_pending = 0;

      return;
   }

   const bool crouched = soldier_state == state.crouch_state;

   if (state.crouch_pending != 0) {
      state.crouch_pending_time += dt;

      if (soldier_state != state.crouch_pending_from) {
         if (state.crouch_pending > 0 && !state.crouch_state_learned) {
            state.crouch_state_learned = true;
            state.crouch_state = soldier_state;

            log_fmt(Log_level::info, "VR: the soldier's crouch is soldier state {}.",
                    soldier_state);
         }

         state.crouch_by_body = state.crouch_pending > 0;
         state.crouch_pending = 0;
         state.crouch_misses = 0;
      }
      else if (state.crouch_pending_time > crouch_answer_seconds) {
         state.crouch_pending = 0;

         if (++state.crouch_misses >= crouch_max_misses) {
            state.crouch_given_up = true;

            log(Log_level::warning, "VR: the game's crouch didn't answer the player crouching; "
                                    "physical crouch given up for this life.");
         }
      }

      return;
   }

   // Stood up some other way (a jump, sprint, the button).
   if (!crouched) state.crouch_by_body = false;

   if (std::abs(move.x) > 0.3f || state.crouch_button_owns) return;

   const auto press = [&](const int direction) {
      tap(control_crouch);
      state.crouch_pending = direction;
      state.crouch_pending_from = soldier_state;
      state.crouch_pending_time = 0.0f;
   };

   if (!crouched && soldier_state == 0 && state.body_fraction < crouch_enter_fraction) {
      press(1);
   }
   else if (crouched && state.crouch_by_body && state.body_fraction > crouch_leave_fraction) {
      press(-1);
   }
}

// Quick reload, a minigame on the game's reload: a bar shows the reload's progress
// with a blue window at a random point; reload pressed again while the bar's marker is
// in the window ends the reload at once. One try per reload: pressed outside it, the
// window goes (the bar stays). The game's own rules end or pause reloads (a weapon
// switch, death, ...): the bar shows only while the weapon in hand reloads.
bool quick_reload_enabled() noexcept
{
   const Settings& s = settings();

   return s.reload_bar_crosshair || s.reload_bar_wrist;
}

void update_quick_reload(const float dt) noexcept
{
   state.reload_flash = std::max(state.reload_flash - dt, 0.0f);
   state.reload_buzz_due -= dt;

   if (state.reload_miss_pulse_in > 0.0f) {
      state.reload_miss_pulse_in -= dt;

      if (state.reload_miss_pulse_in <= 0.0f) {
         haptic_pulse(1, quick_reload_miss_amplitude, quick_reload_miss_seconds,
                      quick_reload_miss_frequency);
      }
   }

   const auto reload = quick_reload_enabled() && !state.menu_frame && state.recentered
                          ? melee::player_reload()
                          : std::nullopt;

   if (!reload) {
      state.reload_weapon = nullptr;

      return;
   }

   const float progress = std::clamp(reload->elapsed / reload->length, 0.0f, 1.0f);

   if (reload->weapon != state.reload_weapon ||
       reload->elapsed + 1e-4f < state.reload_last_elapsed) {
      if (state.reload_random == 0) state.reload_random = GetTickCount() | 1u;

      state.reload_random = state.reload_random * 1664525u + 1013904223u;

      const float random = static_cast<float>(state.reload_random >> 8) / 16777216.0f;
      const float length = std::clamp(quick_reload_window_seconds / reload->length,
                                      quick_reload_min_window, quick_reload_max_window);
      const float earliest = std::max(quick_reload_earliest, progress + 0.1f);
      const float start =
         std::min(earliest + random * std::max(quick_reload_latest - earliest, 0.0f),
                  1.0f - length);

      state.reload_window = {start, start + length};
      state.reload_window_gone = start <= progress; // seen too late for a window
      state.reload_flash = 0.0f;
   }

   state.reload_weapon = reload->weapon;
   state.reload_last_elapsed = reload->elapsed;
   state.reload_progress = progress;

   // While the marker is in the window (and it's still on): a light buzz, "now".
   const bool in_window = !state.reload_window_gone && progress >= state.reload_window.x &&
                          progress <= state.reload_window.y;

   if (in_window && state.reload_buzz_due <= 0.0f) {
      haptic_pulse(1, quick_reload_buzz_amplitude, quick_reload_buzz_interval);
      state.reload_buzz_due = quick_reload_buzz_interval;
   }
}

// Guns that overheat have no reload (no minigame): shaking one cools it faster, on top
// of the game's own cooling, with a light tick in the hand per stroke while it cools.
void update_shake_cooling(const float dt, const bool active) noexcept
{
   state.shake_level = std::max(state.shake_level - dt / shake_fade_seconds, 0.0f);

   if (!active || !state.aim_valid || dt <= 0.0f) {
      state.shake_position_valid = false;
      state.shake_stroke = {};

      return;
   }

   const glm::vec3 position = state.raw_aim_position;

   if (!std::exchange(state.shake_position_valid, true)) {
      state.shake_last_position = position;
      state.shake_velocity = {};
   }

   const glm::vec3 velocity = (position - state.shake_last_position) / dt;
   const float t = 1.0f - std::exp(-dt / shake_velocity_seconds);

   state.shake_last_position = position;
   state.shake_velocity = glm::mix(state.shake_velocity, velocity, t);

   const float speed = glm::length(state.shake_velocity);
   bool stroke = false;

   if (speed > shake_stroke_speed) {
      const glm::vec3 direction = state.shake_velocity / speed;

      if (state.shake_stroke != glm::vec3{} &&
          glm::dot(direction, state.shake_stroke) < shake_turn_back_cos) {
         state.shake_level = std::min(state.shake_level + shake_step, 1.0f);
         stroke = true;
      }

      state.shake_stroke = direction;
   }

   const float extra = settings().shake_cooling - 1.0f;

   if (extra <= 0.0f || state.shake_level <= 0.0f) return;

   if (melee::cool_player_weapon(dt * extra * state.shake_level)) {
      if (stroke) haptic_pulse(1, 0.2f, 0.02f);

      if (!std::exchange(state.shake_cooling_logged, true)) {
         log_fmt(Log_level::info, "VR: an overheated gun cooled by shaking ({:.1f}x).",
                 settings().shake_cooling);
      }
   }
}

// Reload pressed. While a reload is shown the press is the minigame's (not the game's):
// true then.
bool quick_reload_press() noexcept
{
   if (!state.reload_weapon) return false;

   if (!std::exchange(state.reload_window_gone, true)) {
      const bool in_window = state.reload_progress >= state.reload_window.x &&
                             state.reload_progress <= state.reload_window.y;

      if (in_window && melee::finish_player_reload(state.reload_weapon)) {
         state.reload_flash = quick_reload_flash_seconds;
         haptic_pulse(1, quick_reload_hit_amplitude, quick_reload_hit_seconds);
      }
      else {
         // A miss: two hard, low knocks.
         haptic_pulse(1, quick_reload_miss_amplitude, quick_reload_miss_seconds,
                      quick_reload_miss_frequency);
         state.reload_miss_pulse_in = quick_reload_miss_gap;
      }
   }

   return true;
}

// The scope's zoom: one of the three (Settings::scope_zooms), with a tick in the hand.
void set_scope_zoom_mode(const int mode) noexcept
{
   Settings& s = settings();

   s.scope_zoom_mode = std::clamp(mode, 0, 2);
   s.scope_zoom = s.scope_zooms[s.scope_zoom_mode];
   save_settings();
   haptic_pulse(1, 0.25f, 0.02f);

   log_fmt(Log_level::info, "VR: scope zoom {} ({:.1f}x).", s.scope_zoom_mode + 1, s.scope_zoom);
}

// The scope (see State::scope_shown): held up with the right grip, or raised by
// holding the gun to the eye; its window over the right hand, facing the eyes.
void update_scope(const bool allowed, const bool held) noexcept
{
   if (!allowed) {
      state.scope_shown = false;
      state.scope_raised = false;

      return;
   }

   // Along the gun: turned by both hands while held with two (state.weapon_rotation).
   const glm::vec3 hand = to_vec3(state.aim_pose.position);
   const glm::vec3 forward = state.two_handed
                                ? -state.weapon_rotation[2]
                                : rotate(state.aim_pose.orientation, {0.0f, 0.0f, -1.0f});
   const glm::vec3 up = state.two_handed ? state.weapon_rotation[1]
                                         : rotate(state.aim_pose.orientation, {0.0f, 1.0f, 0.0f});
   const glm::vec3 head =
      (to_vec3(state.views[0].pose.position) + to_vec3(state.views[1].pose.position)) * 0.5f;
   const glm::vec3 head_forward = rotate(state.views[0].pose.orientation, {0.0f, 0.0f, -1.0f});

   // The aim ray passes close to the eyes, pointing roughly where the head looks.
   const glm::vec3 to_head = head - hand;
   const float ray_distance = glm::length(to_head - glm::dot(to_head, forward) * forward);
   const float degrees = std::acos(std::clamp(glm::dot(forward, head_forward), -1.0f, 1.0f)) *
                         180.0f / std::numbers::pi_v<float>;
   const float hand_distance = glm::length(to_head);

   if (!settings().scope_auto) {
      state.scope_raised = false;
   }
   else if (!state.scope_raised) {
      state.scope_raised = ray_distance < scope_raise_ray_distance &&
                           degrees < scope_raise_degrees &&
                           hand_distance < scope_raise_hand_distance;
   }
   else {
      state.scope_raised = ray_distance < scope_lower_ray_distance &&
                           degrees < scope_lower_degrees &&
                           hand_distance < scope_raise_hand_distance * 1.3f;
   }

   const bool shown = held || state.scope_raised;

   if (shown != state.scope_shown) {
      log_fmt(Log_level::info, "VR: scope {}.",
              !shown ? "down" : held ? "held up" : "raised to the eye");
   }

   state.scope_shown = shown;

   const glm::vec3 center =
      hand + up * settings().scope_height + forward * scope_window_forward;

   // Facing the eyes (kept upright), or locked: square to the gun, rolled with it.
   glm::vec3 z = -forward;
   glm::vec3 y = up;

   if (!settings().scope_locked) {
      const glm::vec3 to_eyes = head - center;

      z = glm::length(to_eyes) > 1e-3f ? glm::normalize(to_eyes) : -forward;
      y = glm::vec3{0.0f, 1.0f, 0.0f} - z.y * z;
      y = glm::length(y) > 1e-3f ? glm::normalize(y) : up;
   }

   state.scope_pose = {to_quaternion(glm::cross(y, z), y, z), {center.x, center.y, center.z}};
}

// Grenades: the left trigger takes one in the hand, letting it go throws it where the
// hand swung (or points, without a swing). The game throws along its camera, so the
// camera turns to the throw first (update_mouse_follow), then secondary fire is pressed
// and held: the game throws only once fire was held through its wind-up (half a second;
// a short press was taken back). The wind-up is cut short so the grenade leaves at once.
// Returns whether the game's secondary fire is held this frame.
bool update_throw(const bool trigger, const bool grenade, const float dt) noexcept
{
   if (!grenade) {
      state.grenade_held = false;
      state.throw_pending = false;
      state.throw_holding = false;
      state.throw_hold_left = 0;

      return trigger;
   }

   if (state.left_aim_valid) {
      const glm::vec3 position = to_vec3(state.left_aim_pose.position);

      std::move(state.throw_samples.begin() + 1, state.throw_samples.end(),
                state.throw_samples.begin());
      state.throw_samples.back() = {position, state.input_clock};
      state.throw_sample_count = std::min<std::uint32_t>(
         state.throw_sample_count + 1, static_cast<std::uint32_t>(state.throw_samples.size()));
   }

   const bool busy = state.throw_pending || state.throw_holding;

   if (trigger && !busy) {
      state.grenade_held = true;

      return false;
   }

   if (!trigger && state.grenade_held) {
      state.grenade_held = false;

      glm::vec3 direction = rotate(state.left_aim_pose.orientation, {0.0f, 0.0f, -1.0f});
      float speed = 0.0f;

      // The hand's velocity over the last throw_velocity_window seconds.
      if (state.throw_sample_count >= 2) {
         const glm::vec4 newest = state.throw_samples.back();
         glm::vec4 oldest = newest;

         for (std::uint32_t i = 1; i < state.throw_sample_count; ++i) {
            const glm::vec4 sample = state.throw_samples[state.throw_samples.size() - 1 - i];

            if (newest.w - sample.w > throw_velocity_window) break;

            oldest = sample;
         }

         if (newest.w - oldest.w > 1e-3f) {
            const glm::vec3 velocity = (glm::vec3{newest} - glm::vec3{oldest}) / (newest.w - oldest.w);

            speed = glm::length(velocity);

            if (speed > throw_min_speed) direction = velocity / speed;
         }
      }

      state.throw_direction = glm::normalize(direction);
      state.throw_pending = true;
      state.throw_wait_frames = 0;

      log_fmt(Log_level::info, "VR: grenade thrown {} ({:.1f} m/s).",
              speed > throw_min_speed ? "along the swing" : "where the hand points", speed);
   }

   if (state.throw_pending) {
      state.throw_wait_frames += 1;

      const glm::vec3 target = glm::normalize(tracking_to_world(state.throw_direction));
      const float error =
         std::acos(std::clamp(glm::dot(glm::normalize(state.main_camera.forward), target),
                              -1.0f, 1.0f)) *
         180.0f / std::numbers::pi_v<float>;

      if (error < throw_aim_tolerance_degrees ||
          state.throw_wait_frames >= throw_max_wait_frames || !state.main_camera_valid) {
         state.throw_pending = false;
         state.throw_holding = true;
         state.throw_wound_up = false;
         state.throw_hold_time = 0.0f;
         haptic_pulse(0, 0.5f, 0.05f);
      }
   }

   if (state.throw_holding) {
      // Held until the grenade has left (its wind-up seen and over), or as long as the
      // game's own wind-up takes when it can't be seen.
      const bool winding_up = melee::hurry_grenade_throw();

      state.throw_wound_up |= winding_up;
      state.throw_hold_time += dt;

      if ((state.throw_wound_up && !winding_up) ||
          state.throw_hold_time >= throw_max_hold_seconds) {
         state.throw_holding = false;
         state.throw_hold_left = throw_hold_frames;

         log_fmt(Log_level::info, "VR: secondary fire let go after {:.2f} s ({}).",
                 state.throw_hold_time,
                 state.throw_wound_up ? "the grenade left" : "no wind-up seen");
      }
      else {
         return true;
      }
   }

   if (state.throw_hold_left > 0) state.throw_hold_left -= 1;

   return false;
}

// The left stick turned from the movement direction's frame (the head's or a
// controller's heading) into the game camera's, which the movement keys move along.
auto camera_relative_move(const glm::vec2 move) noexcept -> glm::vec2
{
   if (move == glm::vec2{} || !state.main_camera_valid || !state.recentered) return move;

   const XrPosef* pose = nullptr;

   switch (settings().move_direction) {
   case Move_direction::head:
      if (state.views_valid) pose = &state.views[0].pose;
      break;
   case Move_direction::right_hand:
      if (state.aim_valid) pose = &state.aim_pose;
      break;
   case Move_direction::left_hand:
      if (state.left_aim_valid) pose = &state.left_aim_pose;
      break;
   }

   if (!pose) return move;

   const glm::vec3 forward =
      horizontal(tracking_to_world(rotate(pose->orientation, {0.0f, 0.0f, -1.0f})));
   const glm::vec3 camera_right = horizontal(state.main_camera.right);
   const glm::vec3 camera_forward =
      state.main_camera.handedness * glm::cross(camera_right, world_up_axis);

   if (forward == glm::vec3{} || camera_right == glm::vec3{}) return move;

   // How far the reference is turned to the camera's right.
   const float angle = std::atan2(glm::dot(forward, camera_right), glm::dot(forward, camera_forward));
   const float c = std::cos(angle);
   const float s = std::sin(angle);

   return {move.x * c + move.y * s, move.y * c - move.x * s};
}

auto vehicle_brake_counts(glm::vec2 deflection, float steer_speed, float dt) noexcept -> glm::vec2;

// A new life (or character) is measured anew (the body's heights, the crouch learnt).
void reset_body_measure() noexcept
{
   for (auto& measured : state.character_heights) measured.valid = false;

   state.character_from_head = false;
   state.standing_sample_count = 0;
   state.standing_locked = false;
   state.crouch_by_body = false;
   state.body_soldier_state = -1;
   state.crouch_given_up = false;
   state.crouch_misses = 0;
   state.crouch_pending = 0;
   state.crouch_button_watch = -1.0f;
   state.crouch_button_owns = false;
}

void press_controls(std::uint32_t controls) noexcept;

// Settings swap_sticks: the sticks (pads), their clicks and pad clicks change hands for
// as long as this lives (update_input), the poses and buttons stay.
struct Sticks_swapped {
   explicit Sticks_swapped(const bool swap) noexcept : swapped{swap}
   {
      if (swapped) swap_handles();
   }

   ~Sticks_swapped()
   {
      if (swapped) swap_handles();
   }

   Sticks_swapped(const Sticks_swapped&) = delete;
   auto operator=(const Sticks_swapped&) -> Sticks_swapped& = delete;

private:
   static void swap_handles() noexcept
   {
      std::swap(state.move_action, state.turn_action);
      std::swap(state.sprint_action, state.crouch_action);
      std::swap(state.left_pad_click_action, state.right_pad_click_action);
      std::swap(state.left_pad_action, state.right_pad_action);
   }

   bool swapped;
};

void update_input() noexcept
{
   held_input = {};

   if (state.session_state != XR_SESSION_STATE_FOCUSED || !state.action_set) {
      fire_pressed = false;
      secondary_fire_pressed = false;
      state.two_handed = false;
      state.saber_block_grips = false;
      state.scope_shown = false;
      state.panel_click = false;

      // Focus lost in a match: Esc for a few frames (handle_session_state).
      std::uint32_t controls = 0;

      if (state.focus_pause_frames > 0) {
         state.focus_pause_frames -= 1;
         controls |= 1u << control_pause;
      }

      press_controls(controls);

      return;
   }

   state.focus_pause_frames = 0;

   const Sticks_swapped sticks_swapped{settings().swap_sticks};

   const XrActiveActionSet active_set{state.action_set, XR_NULL_PATH};
   XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
   sync_info.countActiveActionSets = 1;
   sync_info.activeActionSets = &active_set;

   if (XR_FAILED(state.xr.xrSyncActions(state.session, &sync_info))) return;

   const float dt =
      std::clamp(static_cast<float>(state.frame_state.predictedDisplayPeriod) * 1e-9f, 0.001f, 0.1f);

   state.input_clock += dt;
   state.anchor_dt = dt;

   XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};

   const auto boolean = [&](const XrAction action) {
      XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};

      if (action) {
         get_info.action = action;
         state.xr.xrGetActionStateBoolean(state.session, &get_info, &button);
      }

      return button;
   };

   const auto down = [&](const XrAction action) {
      const XrActionStateBoolean button = boolean(action);

      return button.isActive && button.currentState;
   };

   // Whether an action has a binding on the controllers in use.
   const auto bound = [&](const XrAction action) { return boolean(action).isActive == XR_TRUE; };

   const auto stick = [&](const XrAction action) {
      XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};

      if (action) {
         get_info.action = action;
         state.xr.xrGetActionStateVector2f(state.session, &get_info, &value);
      }

      return value.isActive ? glm::vec2{value.currentState.x, value.currentState.y}
                            : glm::vec2{};
   };

   const auto bound_stick = [&](const XrAction action) {
      XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};

      if (action) {
         get_info.action = action;
         state.xr.xrGetActionStateVector2f(state.session, &get_info, &value);
      }

      return value.isActive == XR_TRUE;
   };

   const auto locate = [](const XrSpace space, bool& valid, XrPosef& pose) {
      valid = false;

      if (!space || !state.local_space) return;

      XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};

      if (XR_SUCCEEDED(state.xr.xrLocateSpace(space, state.local_space,
                                              state.frame_state.predictedDisplayTime,
                                              &location))) {
         constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                                   XR_SPACE_LOCATION_POSITION_VALID_BIT;

         valid = (location.locationFlags & required) == required;

         if (valid) pose = location.pose;
      }
   };

   // The weapon's hand: swapped hands are the right ones again in a vehicle.
   set_handles_swapped(state.hands_swapped && !state.vehicle_controls);

   locate(state.aim_space, state.aim_valid, state.aim_pose);
   locate(state.left_aim_space, state.left_aim_valid, state.left_aim_pose);
   locate(state.grip_space, state.grip_valid, state.grip_pose);
   locate(state.left_grip_space, state.left_grip_valid, state.left_grip_pose);

   // The weapon bash's gesture reads the controller unsmoothed (smoothing eats a jab's
   // peak speed).
   state.raw_aim_position = to_vec3(state.aim_pose.position);

   smooth_hands(dt);

   if (state.aim_valid) state.stat_aim_valid += 1;

   state.vehicle_controls =
      state.menu_frame && state.recentered && melee::player_in_vehicle() &&
      !melee::game_screen_shown();

   // Without a soldier (dead, the unit select, a new map from the pause menu) the next
   // one is measured anew: update_anchor only sees gameplay frames, and a quit to a new
   // map left the last soldier's standing samples to lock a hero's height (Yoda 1.89 m).
   if (!melee::player_soldier()) reset_body_measure();

   // The control set the game reads now (when it can't be read: a vehicle's in one, else
   // the soldier's).
   state.control_mode = melee::player_control_mode();

   if (state.control_mode < 0) state.control_mode = melee::player_in_vehicle() ? 1 : 0;

   if (state.control_mode != std::exchange(state.logged_control_mode, state.control_mode)) {
      log_fmt(Log_level::info, "VR: the game reads the {} controls now.",
              control_mode_names[state.control_mode]);
   }

   update_menu_pointer();

   // A melee weapon doesn't fire: its blade does the damage (see vr_melee.cpp); on the
   // virtual screen (pause menu, unit select over the match) the trigger still clicks.
   // The trigger clicks the VR panel while the hand points at it.
   const bool melee_in_hand = melee::player_has_melee();
   const bool fire_down = down(state.fire_action);

   // Movement and turning are for gameplay only (menus use the pointer); in a vehicle
   // (on the screen) the controls are the match's, the right stick steering.
   const bool vehicle = state.vehicle_controls;
   const bool gameplay = (!state.menu_frame && state.recentered) || vehicle;
   const Settings& s = settings();

   state.input_gameplay = gameplay;

   // A trigger still held from a menu (the unit select's spawn click) does nothing in
   // the match until it's let go: a saber was thrown (the ability) at once.
   const bool secondary_down = down(state.secondary_fire_action);

   state.fire_held_from_menu = fire_down && (state.fire_held_from_menu || !gameplay);
   state.secondary_held_from_menu =
      secondary_down && (state.secondary_held_from_menu || !gameplay);

   state.panel_click = state.panel_hit && fire_down;
   fire_pressed = fire_down && !state.panel_hit &&
                  (state.menu_frame || (!melee_in_hand && !state.fire_held_from_menu));

   update_hand_swap(gameplay && !vehicle, melee_in_hand, down(state.block_action),
                    down(state.two_hand_action), dt);
   update_saber_rotation(gameplay && !vehicle && melee_in_hand, melee::player_dual_sabers(),
                         down(state.block_action), down(state.two_hand_action), dt);

   std::uint32_t controls = 0; // Game_control bits held this frame

   const auto hold = [&](const Game_control control) { controls |= 1u << control; };

   for (int control = 0; control < control_count; ++control) {
      if (state.tap_frames[control] == 0) continue;

      state.tap_frames[control] -= 1;
      hold(static_cast<Game_control>(control));
   }

   const glm::vec2 move = gameplay ? stick(state.move_action) : glm::vec2{};

   if (glm::length(move) > stick_key_threshold) {
      const glm::vec2 direction = glm::normalize(vehicle ? move : camera_relative_move(move));

      if (direction.y > stick_key_direction) hold(control_move_forward);
      if (direction.y < -stick_key_direction) hold(control_move_back);
      if (direction.x < -stick_key_direction) hold(control_move_left);
      if (direction.x > stick_key_direction) hold(control_move_right);
   }

   state.move_amount = glm::length(move) > stick_key_threshold ? 1.0f : 0.0f;
   state.stick_moving = !vehicle && glm::length(move) > stick_key_threshold;

   // Galactic conquest: the left stick presses W / A / S / D themselves (the galaxy
   // map's keys), in eight directions as above.
   // (The galaxy map may have no screen name: no screen and no entity is taken as it.)
   if (!gameplay && (melee::galactic_conquest_shown() ||
                     (!melee::game_screen_shown() && !melee::player_controls_entity()))) {
      const glm::vec2 gc_move = stick(state.move_action);

      if (glm::length(gc_move) > stick_key_threshold) {
         const glm::vec2 direction = glm::normalize(gc_move);
         const auto press = [](const std::uint8_t key) {
            held_input.keys[key >> 5] |= 1u << (key & 31);
         };

         if (direction.y > stick_key_direction) press(0x11);  // W
         if (direction.y < -stick_key_direction) press(0x1f); // S
         if (direction.x < -stick_key_direction) press(0x1e); // A
         if (direction.x > stick_key_direction) press(0x20);  // D
      }
   }

   // Vive wands, WMR: the left pad (Vive: touched, movement) clicked up sprints, down /
   // sideways change the secondary / primary weapon, in the middle the squad command.
   bool sprint_click = gameplay && down(state.sprint_action);

   if (bound(state.left_pad_click_action)) {
      const glm::vec2 pad =
         bound_stick(state.left_pad_action) ? stick(state.left_pad_action) : move;
      const bool click = gameplay && down(state.left_pad_click_action);
      const bool pressed = click && !state.left_pad_down;

      state.left_pad_down = click;

      if (click && glm::length(pad) < 0.35f) hold(control_squad);
      else if (click && pad.y > 0.3f) sprint_click = true;
      else if (pressed && pad.y < -0.3f) tap(control_next_secondary);
      else if (pressed && std::abs(pad.x) > 0.3f) tap(control_next_primary);
   }

   // The right pad (Vive, WMR) is read here as the reload button needs its middle click
   // (below: a short click reloads, a long one locks the target, as A on Touch).
   const bool right_pad = !vehicle && bound(state.right_pad_click_action);
   const glm::vec2 right_pad_position =
      !right_pad                            ? glm::vec2{}
      : bound_stick(state.right_pad_action) ? stick(state.right_pad_action)
                                            : stick(state.turn_action);
   const bool right_pad_click = right_pad && gameplay && down(state.right_pad_click_action);
   const bool right_pad_middle = right_pad_click && glm::length(right_pad_position) < 0.5f;

   // Sprint: a click sprints until the stick is let go (or while held, see settings).
   if (s.sprint_toggle) {
      if (sprint_click && !state.sprint_click_down) state.sprint_latched = !state.sprint_latched;
      if (glm::length(move) < 0.2f) state.sprint_latched = false;
      if (state.sprint_latched) hold(control_sprint);
   }
   else if (sprint_click) {
      hold(control_sprint);
   }

   state.sprint_click_down = sprint_click;

   // In a vehicle the left grip held gives some buttons a second control (see below).
   const bool vehicle_layer = vehicle && down(state.two_hand_action);

   // Crouch (crouching while strafing rolls; a droideka rolls up, a starfighter does a
   // trick: the vehicle sets' crouch record), with the vehicle layer the view (first /
   // third person); jump (a starfighter lands / takes off), with the vehicle layer roll
   // (a starfighter's Immelmann); squad command, lock target.
   const bool crouch_button = gameplay && down(state.crouch_action);
   const bool droideka = melee::player_on_droideka();

   if (crouch_button) hold(vehicle_layer ? control_view : control_crouch);

   update_body_crouch(dt, gameplay && !vehicle, move, crouch_button);

   if (gameplay && down(state.jump_action)) hold(vehicle_layer ? control_roll : control_jump);
   if (gameplay && down(state.squad_action)) hold(control_squad);
   if (gameplay && down(state.lock_action)) hold(control_lock);

   // A reloads (a melee weapon has nothing to reload: its blade is always lit). Without a
   // lock target button (Touch), a short press reloads and a long one locks the target.
   const bool lock_bound = bound(state.lock_action);

   update_quick_reload(dt);
   update_shake_cooling(dt, gameplay && !melee_in_hand && !vehicle && !state.menu_frame);

   if (gameplay && !melee_in_hand && (down(state.reload_action) || right_pad_middle)) {
      if (state.reload_held < 0.0f) {
         state.reload_held = 0.0f;
         state.lock_sent = false;
         state.reload_press_taken = quick_reload_press();
      }
      else {
         state.reload_held += dt;
      }

      if (lock_bound) {
         if (!state.reload_press_taken) hold(control_reload);
      }
      else if (!state.lock_sent && state.reload_held >= long_press_seconds) {
         state.lock_sent = true;
         tap(control_lock);
      }
   }
   else {
      if (state.reload_held >= 0.0f && !lock_bound && !state.lock_sent &&
          !state.reload_press_taken) {
         tap(control_reload);
      }

      state.reload_held = -1.0f;
   }

   // X: use, enter a vehicle, change class at a command post; Enter in menus.
   if (down(state.class_menu_action)) hold(gameplay ? control_use : control_menu_accept);

   // Y: pause (Esc); held in a match, the VR panel. Pressed with the vehicle layer: the
   // map, while held.
   const bool pause_button = down(state.pause_action);

   if (!pause_button) state.map_press = false;
   else if (state.panel_held < 0.0f && vehicle_layer) state.map_press = true;

   if (state.map_press) {
      hold(control_map);
   }
   else if (pause_button) {
      if (state.panel_held < 0.0f) {
         state.panel_held = 0.0f;
         state.panel_toggled = false;
      }
      else {
         state.panel_held += dt;
      }

      if (!gameplay) {
         hold(control_pause);
      }
      else if (!state.panel_toggled && state.panel_held >= long_press_seconds) {
         state.panel_toggled = true;
         toggle_panel();
      }
   }
   else {
      if (state.panel_held >= 0.0f && gameplay && !state.panel_toggled) tap(control_pause);

      state.panel_held = -1.0f;
   }

   // With a melee weapon both grips held block (with the blade raised, vr_melee.cpp);
   // with a gun the right grip holds the scope up.
   const bool right_grip = down(state.block_action);

   state.saber_block_grips =
      gameplay && melee_in_hand && right_grip && down(state.two_hand_action);

   // In a vehicle the right grip zooms (a starfighter: roll lock, the same record).
   if (vehicle && right_grip) hold(control_zoom);

   // Right stick: sideways turns, up / down change weapon (the scope's zoom while it's
   // up). Vive: the right pad clicked sideways turns, up jumps, down crouches, in the
   // middle reloads.
   glm::vec2 right_stick = gameplay ? stick(state.turn_action) : glm::vec2{};

   if (!vehicle) state.vehicle_stick_sent = {};

   if (vehicle) {
      // Steering (up = nose up): the game's mouse steering keeps turning while the mouse
      // stays moved out, so by position the mouse is held out as far as the stick and
      // moved back as it centres; by speed, mouse counts at a rate set by the stick.
      const auto shape = [](const float deflection) {
         const float amount =
            std::max(std::abs(deflection) - vehicle_steer_dead_zone, 0.0f) /
            (1.0f - vehicle_steer_dead_zone);

         return std::copysign(amount * amount, deflection);
      };

      // With the vehicle layer the stick switches position instead (up next, down
      // previous), the vehicle's steering held still.
      if (vehicle_layer) {
         if (std::abs(right_stick.y) > snap_turn_trigger && !state.position_latched) {
            state.position_latched = true;
            tap(right_stick.y > 0.0f ? control_next_primary : control_previous_primary);
         }

         if (std::abs(right_stick.y) < snap_turn_release) state.position_latched = false;

         right_stick = {};
      }

      const glm::vec2 deflection{shape(right_stick.x), -shape(right_stick.y)};
      glm::vec2 whole{};

      // A droideka turns like a soldier (by the mouse's movement, not held out): by speed.
      if (s.vehicle_steer_mode == Vehicle_steer_mode::position && !droideka) {
         const glm::vec2 target = glm::round(deflection * s.vehicle_stick_range);

         whole = target - state.vehicle_stick_sent;
         state.vehicle_stick_sent = target;
         state.vehicle_steer_remainder = {};
      }
      else {
         glm::vec2 counts =
            deflection * s.vehicle_steer_speed * dt + state.vehicle_steer_remainder;

         counts += vehicle_brake_counts(deflection, s.vehicle_steer_speed, dt);

         whole = glm::trunc(counts);
         state.vehicle_steer_remainder = counts - whole;
         state.vehicle_stick_sent = {};
      }

      mouse_dx += static_cast<LONG>(whole.x);
      mouse_dy += static_cast<LONG>(whole.y);
      right_stick = {};
   }
   else if (right_pad && state.right_stick_is_pad) {
      // Vive: the right pad clicked sideways turns, up jumps, down crouches (in the
      // middle: reload / lock target, above); only touched it does nothing.
      right_stick = {};

      if (right_pad_click && !right_pad_middle) {
         const glm::vec2 pad = right_pad_position;

         if (std::abs(pad.x) > std::abs(pad.y)) right_stick.x = pad.x > 0.0f ? 1.0f : -1.0f;
         else if (pad.y > 0.0f) hold(control_jump);
         else hold(control_crouch);
      }
   }
   else {
      // WMR: the right pad next to the stick, clicked up jumps, down crouches, sideways
      // turns while the stick is let go.
      if (right_pad_click && !right_pad_middle) {
         const glm::vec2 pad = right_pad_position;

         if (std::abs(pad.x) > std::abs(pad.y)) {
            if (glm::length(right_stick) < snap_turn_release) {
               right_stick = {pad.x > 0.0f ? 1.0f : -1.0f, 0.0f};
            }
         }
         else if (pad.y > 0.0f) {
            hold(control_jump);
         }
         else {
            hold(control_crouch);
         }
      }

      const bool upright = std::abs(right_stick.y) > std::abs(right_stick.x);

      if (upright && state.scope_shown) {
         if (std::abs(right_stick.y) > snap_turn_trigger && !state.scope_zoom_latched) {
            state.scope_zoom_latched = true;

            const int mode = settings().scope_zoom_mode + (right_stick.y > 0.0f ? 1 : -1);

            if (mode >= 0 && mode <= 2) set_scope_zoom_mode(mode);
         }
      }
      else if (upright && right_stick.y > snap_turn_trigger) {
         hold(control_next_primary);
      }
      else if (upright && right_stick.y < -snap_turn_trigger) {
         hold(control_next_secondary);
      }

      if (std::abs(right_stick.y) < snap_turn_release) state.scope_zoom_latched = false;
      if (upright) right_stick.x = 0.0f;
   }

   // Turning: snap (with a short vignette flash) or smooth.
   const float turn = right_stick.x;

   if (s.turn_mode == Turn_mode::smooth) {
      if (std::abs(turn) > smooth_turn_dead_zone) {
         const float amount =
            (std::abs(turn) - smooth_turn_dead_zone) / (1.0f - smooth_turn_dead_zone);

         snap_turn((turn > 0.0f ? 1.0f : -1.0f) * amount * s.smooth_turn_speed * dt);
         state.move_amount = std::max(state.move_amount, amount);
      }
   }
   else if (std::abs(turn) < snap_turn_release) {
      state.turn_latched = false;
   }
   else if (std::abs(turn) > snap_turn_trigger && !state.turn_latched) {
      state.turn_latched = true;
      snap_turn(turn > 0.0f ? s.snap_turn_degrees : -s.snap_turn_degrees);
      state.snap_flash = 1.0f;
   }

   // Comfort vignette: while moving or turning with the sticks, eased in and out.
   state.snap_flash = std::max(0.0f, state.snap_flash - dt / 0.25f);

   const float vignette_wanted =
      s.vignette && gameplay
         ? std::max(state.move_amount, state.snap_flash) * s.vignette_strength
         : 0.0f;

   state.vignette += std::clamp(vignette_wanted - state.vignette, -dt * 2.5f, dt * 6.0f);

   // Secondary fire; grenades are thrown with the left hand (see update_throw).
   const bool grenade = gameplay && !melee_in_hand && s.gesture_throw &&
                        melee::player_secondary_is_grenade();

   // Melee characters: either trigger uses the selected ability (secondary fire), aimed
   // by its hand (update_mouse_follow); after a change of hand the press waits for the
   // camera to turn to it.
   const bool left_trigger =
      gameplay && down(state.secondary_fire_action) && !state.secondary_held_from_menu;
   bool ability = left_trigger;

   if (melee_in_hand) {
      const bool right_trigger =
         gameplay && fire_down && !state.panel_hit && !state.fire_held_from_menu;
      int hand = state.melee_aim_hand;

      if (left_trigger != right_trigger) {
         hand = right_trigger ? 1 : 0;
      }
      else if (!left_trigger) {
         state.melee_aim_linger = std::max(0.0f, state.melee_aim_linger - dt);

         if (state.melee_aim_linger <= 0.0f) hand = -1;
      }

      if (left_trigger || right_trigger) state.melee_aim_linger = melee_aim_linger_seconds;

      if (hand != state.melee_aim_hand) {
         state.melee_aim_hand = hand;

         if (hand >= 0) state.melee_aim_settle = melee_aim_settle_frames;
      }

      ability = left_trigger || right_trigger;

      if (state.melee_aim_settle > 0) {
         state.melee_aim_settle -= 1;
         ability = false;
      }
   }
   else {
      state.melee_aim_hand = -1;
   }

   secondary_fire_pressed = update_throw(ability, grenade, dt);

   // Two-handed hold: a frame along the line between the hands, rolled like the right
   // hand; nullopt with the hands too close together to tell a direction.
   const auto two_hand_frame = [](const glm::mat3& right_hand) -> std::optional<glm::mat3> {
      const XrVector3f& r = state.aim_pose.position;
      const XrVector3f& l = state.left_aim_pose.position;
      const glm::vec3 line{l.x - r.x, l.y - r.y, l.z - r.z};

      if (glm::length(line) < two_hand_min_distance) return std::nullopt;

      const glm::vec3 back = -glm::normalize(line);
      const glm::vec3 up_hint = right_hand[1] - glm::dot(right_hand[1], back) * back;

      if (glm::length(up_hint) < 1e-3f) return std::nullopt;

      const glm::vec3 up = glm::normalize(up_hint);

      return glm::mat3{glm::cross(up, back), up, back};
   };

   const glm::mat3 right_hand{rotate(state.aim_pose.orientation, {1.0f, 0.0f, 0.0f}),
                              rotate(state.aim_pose.orientation, {0.0f, 1.0f, 0.0f}),
                              rotate(state.aim_pose.orientation, {0.0f, 0.0f, 1.0f})};
   const std::optional<glm::mat3> frame =
      state.aim_valid && state.left_aim_valid ? two_hand_frame(right_hand) : std::nullopt;

   const bool two_handed =
      gameplay && !vehicle && !melee_in_hand && frame && down(state.two_hand_action);

   if (two_handed) state.weapon_rotation = *frame;

   if (two_handed != std::exchange(state.two_handed, two_handed)) {
      log_fmt(Log_level::info, "VR: weapon held with {}.", two_handed ? "both hands" : "one hand");
   }

   // The scope button clicked twice quickly: the next of the three zooms (a single click
   // keeps the zoom).
   const bool scope_button = gameplay && !vehicle && right_grip && !melee_in_hand;

   state.scope_press_age += dt;

   if (scope_button && !state.scope_button_down) {
      if (state.scope_press_age < scope_double_click_seconds) {
         set_scope_zoom_mode((settings().scope_zoom_mode + 1) % 3);
         state.scope_press_age = 1e9f;
      }
      else {
         state.scope_press_age = 0.0f;
      }
   }

   state.scope_button_down = scope_button;

   update_scope(gameplay && !vehicle && !melee_in_hand && !melee::player_in_turret() &&
                   state.aim_valid &&
                   state.views_valid && !state.panel_open,
                right_grip && !melee_in_hand);

   press_controls(controls);
}

// The profile's keys (or mouse buttons, wheel notches) for the controls held (Game_control
// bits), from the control set the game reads now.
void press_controls(const std::uint32_t controls) noexcept
{
   for (int control = 0; control < control_count; ++control) {
      if (!(controls & (1u << control))) continue;

      const int record = control_records[control];
      const Game_binding binding =
         record >= 0 ? mode_bindings[state.control_mode][record] : game_bindings[control];

      if (binding.key) {
         held_input.keys[binding.key >> 5] |= 1u << (binding.key & 31);
      }
      else if (binding.mouse >= 0x43 && binding.mouse <= 0x46) {
         held_input.mouse_buttons |= static_cast<std::uint8_t>(1u << (binding.mouse - 0x43));
      }
      else if ((binding.mouse == 0x4b || binding.mouse == 0x4c) &&
               !(state.previous_controls & (1u << control))) {
         mouse_wheel += binding.mouse == 0x4b ? 1 : -1;
      }
   }

   state.previous_controls = controls;
}

// Vehicle steering brake: the game keeps turning a vehicle (or a rolled droideka, a
// starfighter) after the mouse stops (it chases where the mouse aimed, through its turn
// filter). While the stick steers, how far the game camera turns per mouse count is
// learnt, per axis (yaw, pitch); once the stick is let go on an axis, mouse counts
// against the camera's remaining turn stop it, as letting go of the left stick stops
// walking. Returns this frame's counts.
auto vehicle_brake_counts(const glm::vec2 deflection, const float steer_speed,
                          const float dt) noexcept -> glm::vec2
{
   if (!state.main_camera_valid || dt <= 0.0f) return {};

   // The camera's turn rates, once per game frame.
   if (state.vehicle_yaw_frame != state.frame_index) {
      const glm::vec3 forward = glm::normalize(state.main_camera.forward);
      const glm::vec3 level = horizontal(forward);

      state.vehicle_yaw_frame = state.frame_index;

      if (level != glm::vec3{}) {
         const glm::vec2 angles{std::atan2(level.x, level.z),
                                std::asin(std::clamp(forward.y, -1.0f, 1.0f))};

         if (state.vehicle_yaw_valid) {
            const glm::vec2 turned{
               std::remainder(angles.x - state.vehicle_yaw.x, glm::two_pi<float>()),
               angles.y - state.vehicle_yaw.y};

            state.vehicle_yaw_rate += (turned / dt - state.vehicle_yaw_rate) * 0.5f;
         }

         state.vehicle_yaw = angles;
         state.vehicle_yaw_valid = true;
      }
   }

   glm::vec2 result{};

   for (int axis = 0; axis < 2; ++axis) {
      const float rate = state.vehicle_yaw_rate[axis];
      float& per_count = state.vehicle_turn_per_count[axis];

      // Steering: learn the turn per count from a steady turn.
      if (deflection[axis] != 0.0f) {
         state.vehicle_brake_time[axis] = 0.0f;

         const float sent_rate = deflection[axis] * steer_speed; // counts/s

         if (std::abs(deflection[axis]) > 0.3f && std::abs(rate) > 0.1f) {
            const float measured = rate / sent_rate;

            per_count = per_count == 0.0f
                           ? measured
                           : per_count + (measured - per_count) * std::min(1.0f, dt * 2.0f);
         }

         continue;
      }

      // Let go: counter the remaining turn for a while.
      state.vehicle_brake_time[axis] += dt;

      if (per_count == 0.0f || state.vehicle_brake_time[axis] > vehicle_brake_seconds ||
          std::abs(rate) < vehicle_brake_stop_rate) {
         continue;
      }

      if (!std::exchange(state.vehicle_brake_logged[axis], true)) {
         log_fmt(Log_level::info,
                 "VR: vehicle steering brake ({}): {:.5f} rad per mouse count, braking a "
                 "{:.2f} rad/s turn.",
                 axis == 0 ? "yaw" : "pitch", per_count, rate);
      }

      const float counts = -rate / per_count * vehicle_brake_gain * dt;
      const float most = steer_speed * dt;

      result[axis] = std::clamp(counts, -most, most);
   }

   return result;
}

// The panel's Measure height, its countdown over: standing, the head's height over the
// headset's floor becomes the player's standing eye height (kept in the settings);
// seated, the head's height now stands for the character's standing eyes.
void measure_player_height() noexcept
{
   if (!state.views_valid) {
      state.measure_text = "No head tracking.";

      return;
   }

   const float head_y = (state.views[0].pose.position.y + state.views[1].pose.position.y) * 0.5f;
   Settings& s = settings();

   if (s.play_mode == Play_mode::seated) {
      state.seated_head_y = head_y;
      state.seated_head_valid = true;
      state.measure_text = "Done.";

      log(Log_level::info, "VR: seated height measured.");

      return;
   }

   update_floor(false);

   if (!state.floor_valid) {
      state.measure_text = "The headset has no floor: set it under Advanced.";

      return;
   }

   const float height = head_y - state.floor_height;

   if (height < 0.8f || height > 2.1f) {
      state.measure_text = fmt::format("{:.2f} m over the floor? Not taken.", height);

      return;
   }

   s.player_eye_height = height;
   s.player_height_measured = true;
   save_settings();

   state.player_eye_measured = height;
   state.player_eye_logged = height;
   state.measure_text = fmt::format("Your eyes: {:.2f} m.", height);

   log_fmt(Log_level::info, "VR: the player's standing eye height measured: {:.2f} m.", height);
}

// The VR panel drawn into its swapchain. False when it can't be shown.
bool draw_panel_layer(ID3D11DeviceContext& dc) noexcept
{
   if (!state.panel_swapchain) {
      if (state.panel_failed || !state.device || state.swapchain_format == DXGI_FORMAT_UNKNOWN) {
         return false;
      }

      state.panel_failed = true;

      const CD3D11_TEXTURE2D_DESC desc{DXGI_FORMAT_R8G8B8A8_UNORM, panel_texture_width_px,
                                       panel_height_px, 1, 1,
                                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};

      if (FAILED(state.device->CreateTexture2D(&desc, nullptr, &state.panel_texture)) ||
          FAILED(state.device->CreateRenderTargetView(state.panel_texture, nullptr,
                                                      &state.panel_rtv))) {
         log(Log_level::warning, "VR: can't create the panel's texture.");

         return false;
      }

      XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
      info.format = state.swapchain_format;
      info.sampleCount = 1;
      info.width = panel_texture_width_px;
      info.height = panel_height_px;
      info.faceCount = 1;
      info.arraySize = 1;
      info.mipCount = 1;

      if (failed(state.xr.xrCreateSwapchain(state.session, &info, &state.panel_swapchain),
                 "xrCreateSwapchain(panel)")) {
         state.panel_swapchain = XR_NULL_HANDLE;

         return false;
      }

      uint32_t count = 0;
      state.xr.xrEnumerateSwapchainImages(state.panel_swapchain, 0, &count, nullptr);
      std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
      state.xr.xrEnumerateSwapchainImages(state.panel_swapchain, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(
                                             images.data()));

      for (const auto& image : images) state.panel_swapchain_images.push_back(image.texture);

      state.panel_failed = false;
   }

   LARGE_INTEGER now{}, frequency{};
   QueryPerformanceCounter(&now);
   QueryPerformanceFrequency(&frequency);

   const float dt = state.panel_last_tick
                       ? static_cast<float>(now.QuadPart - state.panel_last_tick) /
                            static_cast<float>(frequency.QuadPart)
                       : 1.0f / 90.0f;

   state.panel_last_tick = now.QuadPart;

   Panel_status status;

   if (!state.menu_frame) {
      status.model = state.model_signature;
      status.model_parts = static_cast<int>(state.ranked_parts.size());
      status.right_hand_parts = state.right_hand_parts;
      status.left_hand_parts = state.left_hand_parts;
   }

   status.bindings = bindings_text;
   status.fps = state.perf_fps;
   status.refresh_hz = state.perf_refresh_hz;

   if (state.body_anchored) {
      const Settings& s = settings();

      status.body = fmt::format(
         "Character's eyes {:.2f} m (standing {:.2f} m{}). Your standing eyes {:.2f} m ({}), "
         "head now at {:.0f}%. Your motion scaled {:.2f}x.",
         state.body_eye_height, state.body_standing_height,
         state.character_from_head ? ", head bone" : "", state.body_player_eye_height,
         s.player_height_measured                ? "measured"
         : s.play_mode == Play_mode::standing &&
               state.body_player_eye_height != s.player_eye_height ? "highest held"
                                                                     : "default",
         state.body_fraction * 100.0f, state.tracking_scale);
   }

   if (state.measure_countdown > 0.0f) {
      state.measure_countdown -= dt;

      if (state.measure_countdown <= 0.0f) measure_player_height();
   }

   status.measure = state.measure_countdown > 0.0f
                       ? fmt::format("Hold still: {:.0f}", std::ceil(state.measure_countdown))
                       : state.measure_text;

   const Panel_requests requests =
      draw_panel(*state.device, dc, *state.panel_rtv, {panel_texture_width_px, panel_height_px},
                 state.panel_pointer, state.panel_click, dt, status);

   if (requests.close) state.panel_open = false;
   if (requests.measure) state.measure_countdown = measure_countdown_seconds;

   if (requests.recenter) request_recenter();

   uint32_t index = 0;

   if (failed(state.xr.xrAcquireSwapchainImage(state.panel_swapchain, nullptr, &index),
              "xrAcquireSwapchainImage(panel)")) {
      return false;
   }

   XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
   wait_info.timeout = XR_INFINITE_DURATION;

   const bool ok = XR_SUCCEEDED(state.xr.xrWaitSwapchainImage(state.panel_swapchain, &wait_info));

   if (ok) dc.CopyResource(state.panel_swapchain_images[index], state.panel_texture);

   state.xr.xrReleaseSwapchainImage(state.panel_swapchain, nullptr);

   return ok;
}

void copy_to_swapchain(ID3D11DeviceContext& dc, ID3D11Texture2D& frame) noexcept
{
   uint32_t index = 0;

   if (failed(state.xr.xrAcquireSwapchainImage(state.swapchain, nullptr, &index),
              "xrAcquireSwapchainImage")) {
      return;
   }

   XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
   wait_info.timeout = XR_INFINITE_DURATION;

   if (XR_SUCCEEDED(state.xr.xrWaitSwapchainImage(state.swapchain, &wait_info))) {
      dc.CopyResource(state.swapchain_images[index], &frame);
   }

   state.xr.xrReleaseSwapchainImage(state.swapchain, nullptr);
}

// The panel's frame rate and refresh rate (State::perf_fps).
void count_frame() noexcept
{
   LARGE_INTEGER now{};
   LARGE_INTEGER frequency{};
   QueryPerformanceCounter(&now);
   QueryPerformanceFrequency(&frequency);

   if (state.frame_state.predictedDisplayPeriod > 0) {
      state.perf_refresh_hz =
         1e9f / static_cast<float>(state.frame_state.predictedDisplayPeriod);
   }

   state.perf_window_frames += 1;

   if (state.perf_window_start == 0) {
      state.perf_window_start = now.QuadPart;
      state.perf_window_frames = 0;

      return;
   }

   const float seconds = static_cast<float>(now.QuadPart - state.perf_window_start) /
                         static_cast<float>(frequency.QuadPart);

   if (seconds < 0.5f) return;

   state.perf_fps = static_cast<float>(state.perf_window_frames) / seconds;
   state.perf_window_start = now.QuadPart;
   state.perf_window_frames = 0;
}

void end_frame(ID3D11DeviceContext& dc, ID3D11Texture2D& frame, const bool side_by_side) noexcept
{
   count_frame();

   D3D11_TEXTURE2D_DESC desc{};
   frame.GetDesc(&desc);

   XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
   std::array<XrCompositionLayerProjectionView, 2> projection_views{
      XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
      XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
   XrCompositionLayerQuad screen{XR_TYPE_COMPOSITION_LAYER_QUAD};
   XrCompositionLayerQuad scope{XR_TYPE_COMPOSITION_LAYER_QUAD};
   XrCompositionLayerQuad panel{XR_TYPE_COMPOSITION_LAYER_QUAD};

   std::array<const XrCompositionLayerBaseHeader*, 3> layers{};
   std::uint32_t layer_count = 0;
   bool screen_shown = false;

   if (state.frame_state.shouldRender && state.local_space &&
       ensure_swapchain(desc.Width, desc.Height)) {
      copy_to_swapchain(dc, frame);

      const glm::uvec2 size{desc.Width, desc.Height};
      const glm::uvec2 eyes = side_by_side ? eyes_size(size) : size;
      const int32_t half_width = static_cast<int32_t>(eyes.x / 2);
      const int32_t height = static_cast<int32_t>(desc.Height);

      // Stereo only when this frame actually drew the world with our eye cameras.
      if (side_by_side && state.views_valid && state.camera_seen_this_frame) {
         for (int i = 0; i < 2; ++i) {
            projection_views[i].pose = state.views[i].pose;
            projection_views[i].fov = state.views[i].fov;
            projection_views[i].subImage.swapchain = state.swapchain;
            projection_views[i].subImage.imageRect = {{i * half_width, 0},
                                                      {half_width, height}};
         }

         projection.space = state.local_space;
         projection.viewCount = 2;
         projection.views = projection_views.data();
         layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
         state.stat_projection_layers += 1;

         // The scope's view on its window over the right hand (round: its alpha).
         if (state.scope_shown && state.scope_column > 0 && view_count() == 3) {
            const View_rect rect = view_rect(2, size);

            scope.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            scope.space = state.local_space;
            scope.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            scope.subImage.swapchain = state.swapchain;
            scope.subImage.imageRect = {{static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y)},
                                        {static_cast<int32_t>(rect.width),
                                         static_cast<int32_t>(rect.height)}};
            scope.pose = state.scope_pose;
            scope.size = {settings().scope_size, settings().scope_size};
            layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scope);
         }
      }
      else {
         // Brought up in front of the head (death, menus, loading) and left there.
         if (!state.screen_shown || state.screen_place_pending) place_screen();

         const int32_t width = side_by_side ? half_width : static_cast<int32_t>(desc.Width);

         // A side-by-side half holds the whole game frame squeezed: keep the window's
         // aspect.
         const float aspect = state.window_size.x > 0
                                 ? static_cast<float>(state.window_size.y) /
                                      static_cast<float>(state.window_size.x)
                                 : static_cast<float>(height) / static_cast<float>(width);

         screen.space = state.local_space;
         screen.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
         screen.subImage.swapchain = state.swapchain;
         screen.subImage.imageRect = {{0, 0}, {width, height}};
         screen.pose = state.screen_pose;
         screen.size = {screen_width, screen_width * aspect};
         layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&screen);
         screen_shown = true;
         state.stat_quad_layers += 1;
      }
   }

   state.screen_shown = screen_shown;

   if (screen_shown) state.screen_size = {screen.size.width, screen.size.height};

   // The VR panel: beside the menu screen, or where it was opened in a match.
   state.panel_shown = layer_count > 0 && (screen_shown || state.panel_open);

   if (state.panel_shown && draw_panel_layer(dc)) {
      panel.space = state.local_space;
      panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      panel.subImage.swapchain = state.panel_swapchain;
      panel.subImage.imageRect = {{0, 0},
                                  {static_cast<int32_t>(panel_texture_width_px),
                                   static_cast<int32_t>(panel_height_px)}};
      panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // the gap
      panel.pose = state.panel_pose;
      panel.size = {panel_size().x, panel_size().y};
      layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel);
   }

   XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
   end_info.displayTime = state.frame_state.predictedDisplayTime;
   end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
   end_info.layerCount = layer_count;
   end_info.layers = layers.data();

   failed(state.xr.xrEndFrame(state.session, &end_info), "xrEndFrame");

   state.frame_begun = false;
}

// Edge margin: each eye renders a little past its field of view (more while the head
// turns fast), so the compositor's late reprojection to a newer head pose doesn't
// reveal unrendered edges. The views' field of view is widened (also what's submitted).
void update_overscan() noexcept
{
   const Settings& s = settings();
   const XrQuaternionf q = state.views[0].pose.orientation;
   const float period = std::max(static_cast<float>(state.frame_state.predictedDisplayPeriod) * 1e-9f,
                                 0.001f);
   float speed = 0.0f; // degrees per second

   if (state.overscan_orientation_valid) {
      const XrQuaternionf& r = state.overscan_orientation;
      const float dot = std::abs(q.x * r.x + q.y * r.y + q.z * r.z + q.w * r.w);

      speed = 2.0f * std::acos(std::min(dot, 1.0f)) * 180.0f / std::numbers::pi_v<float> / period;
   }

   state.overscan_orientation = q;
   state.overscan_orientation_valid = true;

   const float wanted =
      s.overscan_by_speed ? std::min(speed * overscan_per_speed, overscan_max_degrees) : 0.0f;

   state.overscan_degrees =
      wanted >= state.overscan_degrees
         ? wanted
         : std::max(wanted, state.overscan_degrees -
                               overscan_max_degrees * period / overscan_release_seconds);

   const float margin =
      (s.overscan_degrees + state.overscan_degrees) * std::numbers::pi_v<float> / 180.0f;

   for (XrView& view : state.views) {
      view.fov.angleLeft -= margin;
      view.fov.angleRight += margin;
      view.fov.angleUp += margin;
      view.fov.angleDown -= margin;
   }
}

void begin_frame() noexcept
{
   state.frame_state = {XR_TYPE_FRAME_STATE};

   if (failed(state.xr.xrWaitFrame(state.session, nullptr, &state.frame_state), "xrWaitFrame"))
      return;

   if (failed(state.xr.xrBeginFrame(state.session, nullptr), "xrBeginFrame")) return;

   state.frame_begun = true;
   state.views_valid = false;

   if (state.frame_state.shouldRender && state.local_space) {
      XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
      locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      locate_info.displayTime = state.frame_state.predictedDisplayTime;
      locate_info.space = state.local_space;

      XrViewState view_state{XR_TYPE_VIEW_STATE};
      uint32_t count = 0;

      state.views = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};

      if (XR_SUCCEEDED(state.xr.xrLocateViews(state.session, &locate_info, &view_state,
                                              2, &count, state.views.data())) &&
          count == 2) {
         constexpr XrViewStateFlags required = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                                               XR_VIEW_STATE_POSITION_VALID_BIT;

         state.views_valid = (view_state.viewStateFlags & required) == required;
      }

      if (state.views_valid) {
         state.stat_views_valid += 1;
         update_overscan();
      }
   }

   if (state.frame_state.shouldRender) state.stat_should_render += 1;

   update_input();
   update_mouse_follow();
   apply_debug_cheats();
}

// A vibration of `seconds` on one hand (0 left, 1 right), at `frequency` (Hz; 0 = the
// runtime's own).
void haptic_pulse(const int hand, const float amplitude, const float seconds,
                  const float frequency) noexcept
{
   if (!state.haptic_action || state.session_state != XR_SESSION_STATE_FOCUSED) return;

   XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
   info.action = state.haptic_action;
   info.subactionPath = state.hand_paths[hand];

   XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
   vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
   vibration.duration = static_cast<XrDuration>(seconds * 1e9f);
   vibration.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;

   state.xr.xrApplyHapticFeedback(state.session, &info,
                                  reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

// The player's soldier knocked down (thrown) or held (lightning, choke): the controls
// don't answer and first person shows nothing of it. The hands vibrate (and the saber
// leaves the hand, vr_melee.cpp).
void update_stun(const float dt) noexcept
{
   const melee::Control_loss loss = melee::player_control_loss();
   const bool lost = loss != melee::Control_loss::none;

   if (lost) {
      state.stun_held = loss == melee::Control_loss::held;

      if (!state.stun_active) {
         state.stun_active = true;
         state.stun_buzz_time = 0.0f;
         haptic_pulse(0, 0.8f, 0.15f);
         haptic_pulse(1, 0.8f, 0.15f);
      }

      state.stun_buzz_time += dt;

      if (state.stun_held && state.stun_buzz_time >= stun_buzz_interval) {
         state.stun_buzz_time = 0.0f;
         haptic_pulse(0, 0.35f, 0.06f);
         haptic_pulse(1, 0.35f, 0.06f);
      }
   }
   else {
      state.stun_active = false;
   }
}

// The saber (vr_melee.cpp): right hand, head and block grips, once per game frame.
void update_melee() noexcept
{
   LARGE_INTEGER now{}, frequency{};
   QueryPerformanceCounter(&now);
   QueryPerformanceFrequency(&frequency);

   melee::Frame_input input;

   input.dt = state.melee_last_tick
                 ? static_cast<float>(now.QuadPart - state.melee_last_tick) /
                      static_cast<float>(frequency.QuadPart)
                 : 0.0f;
   input.block = state.saber_block_grips;
   input.scale = state.tracking_scale;
   input.secondary_fire = secondary_fire_pressed;
   state.melee_last_tick = now.QuadPart;

   if (state.recentered && state.main_camera_valid && state.views_valid) {
      // The saber follows the grip pose (through the fist), the aim pose without one.
      // A single saber turned in the hand (update_saber_rotation).
      if (state.grip_valid || state.aim_valid) {
         const World_pose hand = aim_pose_world(
            state.main_camera,
            state.saber_turns[0].pose);

         input.hand = melee::Hand{hand.position, hand.rotation};
      }

      // A second saber (dual sabers) in the left hand, the same way.
      if (state.left_grip_valid || state.left_aim_valid) {
         const World_pose hand = aim_pose_world(
            state.main_camera, state.saber_turns[1].pose);

         input.left_hand = melee::Hand{hand.position, hand.rotation};
      }

      // The gun's front (weapon bash): along the barrel from the aim pose (the two hands'
      // direction while held with both).
      if (state.aim_valid && !melee::player_has_melee()) {
         const World_pose aim = aim_pose_world(state.main_camera, state.aim_pose);
         const glm::vec3 barrel =
            state.two_handed ? -state.weapon_rotation[2]
                             : rotate(state.aim_pose.orientation, {0.0f, 0.0f, -1.0f});
         const glm::vec3 forward = glm::normalize(tracking_to_world(barrel));
         const glm::vec3 head =
            (to_vec3(state.views[0].pose.position) + to_vec3(state.views[1].pose.position)) *
            0.5f;

         input.gun = melee::Gun{
            .front = aim.position + forward * bash_gun_front,
            .forward = forward,
            .motion = state.raw_aim_position + barrel * bash_gun_front - head};
      }

      input.head_position = head_position_world(state.main_camera);
      input.head_forward = horizontal(tracking_to_world(
         rotate(state.views[0].pose.orientation, glm::vec3{0.0f, 0.0f, -1.0f})));
      state.melee_anchor = state.main_camera.position;

      if (state.turret_anchor) {
         input.hide_soldiers_near = state.main_camera.position + state.anchor_offset;
         input.hide_soldiers_radius = settings().turret_hide_radius;
      }
   }

   melee::update(input);

   const melee::Events events = melee::take_events();
   const float haptic_time = state.haptic_clock += input.dt;
   const auto strong_pulse = [&](const bool both_hands, const float amplitude,
                                 const float seconds) {
      if (both_hands) haptic_pulse(0, amplitude, seconds);

      haptic_pulse(1, amplitude, seconds);
      state.haptic_strong_until = haptic_time + seconds;
   };

   // Vibration: a deflected bolt in both hands (they hold the block), a hit in the saber
   // hand, a whoosh's start lightly. While blocking, a light buzz in both hands.
   if (events.deflects > 0) strong_pulse(true, block_deflect_amplitude, block_deflect_seconds);
   else if (events.hits > 0) {
      // Each hand feels its own saber's hits (dual sabers).
      if (events.hits > events.left_hits) haptic_pulse(1, 0.9f, 0.07f);
      if (events.left_hits > 0) haptic_pulse(0, 0.9f, 0.07f);
   }
   else if (events.bash_hits > 0) strong_pulse(state.two_handed, 1.0f, 0.12f);
   else if (events.bash_blocked > 0) strong_pulse(state.two_handed, 0.6f, 0.05f);
   else if (events.bashes > 0) haptic_pulse(1, 0.2f, 0.03f);
   else if (events.swing_speed > 0.0f) haptic_pulse(events.swing_left ? 0 : 1, 0.15f, 0.02f);

   const bool blocking = melee::player_blocking();

   if (blocking && haptic_time >= state.haptic_strong_until && haptic_time >= state.block_buzz_due) {
      haptic_pulse(0, block_buzz_amplitude, block_buzz_seconds);
      haptic_pulse(1, block_buzz_amplitude, block_buzz_seconds);
      state.block_buzz_due = haptic_time + block_buzz_interval;
   }

   if (blocking != state.block_logged) {
      state.block_logged = blocking;
      log(Log_level::info, blocking ? "VR: saber block on." : "VR: saber block off.");
   }

   // A melee weapon taken up: log the next near scene draw by draw (where its blade is
   // drawn).
   if (melee::player_has_melee() && !std::exchange(state.had_melee, true)) {
      state.weapon_draws_log_pending = true;
   }
   else if (!melee::player_has_melee()) {
      state.had_melee = false;
   }

   if (events.swing_speed > 0.0f) sound::play_swing(events.swing_speed);
   // Only a bash that hit something is heard (a swish for every miss was a nuisance).
   if (events.bash_hits + events.bash_blocked > 0) sound::play_bash(true);

   update_stun(input.dt);
}

}

auto init_instance() noexcept -> std::optional<LUID>
{
   if (state.instance) return std::nullopt;
   if (!load_loader()) return std::nullopt;

   std::vector<const char*> extensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};

   XrDebugUtilsMessengerCreateInfoEXT messenger_info{XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
   messenger_info.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                      XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
   messenger_info.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
   messenger_info.userCallback = debug_message;

   const bool debug_utils = instance_extension_available(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);

   if (debug_utils) extensions.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);

   // Controllers whose interaction profiles come with extensions (bindings in
   // create_actions).
   state.hp_controller_extension =
      instance_extension_available(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);
   state.pico_controller_extension =
      instance_extension_available(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);

   if (state.hp_controller_extension) {
      extensions.push_back(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);
   }

   if (state.pico_controller_extension) {
      extensions.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
   }

   XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
   create_info.next = debug_utils ? &messenger_info : nullptr;
   strcpy_s(create_info.applicationInfo.applicationName, "Star Wars Battlefront II VR");
   create_info.applicationInfo.applicationVersion = 1;
   strcpy_s(create_info.applicationInfo.engineName, "Shader Patch VR");
   create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
   create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
   create_info.enabledExtensionNames = extensions.data();

   if (failed(state.xr.xrCreateInstance(&create_info, &state.instance), "xrCreateInstance")) {
      log(Log_level::info, "VR: no OpenXR runtime available (is SteamVR running?), VR disabled.");
      state.instance = XR_NULL_HANDLE;

      return std::nullopt;
   }

   if (!load_instance_functions()) return std::nullopt;

   XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
   state.xr.xrGetInstanceProperties(state.instance, &properties);

   XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
   system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

   if (failed(state.xr.xrGetSystem(state.instance, &system_info, &state.system),
              "xrGetSystem (is the headset connected?)")) {
      state.xr.xrDestroyInstance(state.instance);
      state.instance = XR_NULL_HANDLE;

      return std::nullopt;
   }

   XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};

   if (failed(state.xr.xrGetD3D11GraphicsRequirementsKHR(state.instance, state.system,
                                                         &requirements),
              "xrGetD3D11GraphicsRequirementsKHR")) {
      state.xr.xrDestroyInstance(state.instance);
      state.instance = XR_NULL_HANDLE;

      return std::nullopt;
   }

   uint32_t view_count = 0;
   std::array<XrViewConfigurationView, 2> config_views{
      XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
      XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW}};

   if (!failed(state.xr.xrEnumerateViewConfigurationViews(
                  state.instance, state.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                  2, &view_count, config_views.data()),
               "xrEnumerateViewConfigurationViews")) {
      state.recommended_eye_size = {config_views[0].recommendedImageRectWidth,
                                    config_views[0].recommendedImageRectHeight};
   }

   log_fmt(Log_level::info, "VR: OpenXR runtime '{}' ready, recommended eye size {}x{}.",
           properties.runtimeName, state.recommended_eye_size.x,
           state.recommended_eye_size.y);

   create_actions();

   return requirements.adapterLuid;
}

auto stereo_render_size(const glm::uvec2 window_size) noexcept -> std::optional<glm::uvec2>
{
   if (!state.instance || state.recommended_eye_size.x == 0) return std::nullopt;

   if (!std::exchange(state.render_scale_loaded, true)) {
      load_settings();

      state.render_scale = std::clamp(settings().render_scale_percent / 100.0f,
                                      min_render_scale, max_render_scale);

      log_fmt(Log_level::info, "VR: render scale {:.0f}% of the recommended eye size.",
              state.render_scale * 100.0f);
   }

   // The side-by-side target may be larger than the window: the desktop mirror then
   // shows its middle (see Backbuffer_resolver).
   const glm::vec2 recommended{state.recommended_eye_size};
   const float scale = state.render_scale;

   const glm::uvec2 eye_size{static_cast<UINT>(recommended.x * scale) & ~1u,
                             static_cast<UINT>(recommended.y * scale) & ~1u};

   state.window_size = window_size;
   state.scope_column = static_cast<std::uint32_t>(static_cast<float>(eye_size.y) *
                                                   scope_column_fraction) &
                        ~1u;

   if (std::exchange(state.eye_size, eye_size) != eye_size) {
      log_fmt(Log_level::info, "VR: eye render size {}x{} ({:.0f}% of recommended), scope view "
                               "{}x{}.",
              eye_size.x, eye_size.y, scale * 100.0f, state.scope_column, state.scope_column);
   }

   return glm::uvec2{state.eye_size.x * 2 + state.scope_column, state.eye_size.y};
}

namespace {

// Writes this run's crash dump next to its log (bf2vr_logs, info: null outside an
// exception handler). Small enough to attach to a bug report; a bf2vr_full_dump.txt next
// to d3d9.dll adds the game's global data (tens of MB) for debugging.
// Written from a thread of its own: the crashed thread's stack may be used up (a stack
// overflow) or broken.
void write_crash_dump(EXCEPTION_POINTERS* const info) noexcept
{
   struct Request {
      EXCEPTION_POINTERS* info;
      DWORD thread;
   };

   Request request{info, GetCurrentThreadId()};

   const auto write = [](void* const parameter) noexcept -> DWORD {
      const Request& request = *static_cast<const Request*>(parameter);
      const HANDLE file = CreateFileA(log_run_path(".dmp").c_str(), GENERIC_WRITE, 0,
                                      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

      if (file == INVALID_HANDLE_VALUE) return 1;

      const bool full = GetFileAttributesW((module_directory() + L"bf2vr_full_dump.txt")
                                              .c_str()) != INVALID_FILE_ATTRIBUTES;
      MINIDUMP_EXCEPTION_INFORMATION exception{request.thread, request.info, FALSE};

      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                        static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                   MiniDumpWithThreadInfo |
                                                   MiniDumpWithUnloadedModules |
                                                   (full ? MiniDumpWithDataSegs : 0)),
                        request.info ? &exception : nullptr, nullptr, nullptr);
      CloseHandle(file);

      return 0;
   };

   const HANDLE thread = CreateThread(nullptr, 0, write, &request, 0, nullptr);

   if (!thread) return;

   WaitForSingleObject(thread, INFINITE);
   CloseHandle(thread);
}

LPTOP_LEVEL_EXCEPTION_FILTER previous_crash_filter = nullptr;

// Crash diagnostics: an exception nobody handled (the game is about to close) is logged
// with code, module and offset and written to a crash dump next to the run's log; then
// the filter before ours (Windows Error Reporting by default) runs. Exceptions caught
// on the way (our reads of game memory in __try, the driver's, the game's own) never
// get here.
LONG WINAPI crash_filter(EXCEPTION_POINTERS* const info) noexcept
{
   static std::atomic<int> handled = 0;

   if (handled.fetch_add(1) == 0) {
      const DWORD code = info->ExceptionRecord->ExceptionCode;
      const void* const address = info->ExceptionRecord->ExceptionAddress;
      HMODULE module = nullptr;
      wchar_t module_path[MAX_PATH]{};

      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module);
      if (module) GetModuleFileNameW(module, module_path, MAX_PATH);

      const std::wstring path{module_path};
      const std::string name{path.begin() + (path.find_last_of(L"\\/") + 1), path.end()};

      log_fmt(Log_level::error,
              "VR: unhandled exception 0x{:08X} at 0x{:08x} ({}+0x{:x}), {} 0x{:08x}; "
              "stats this frame: head culls {}, late {}, rebuilt {}.",
              code, reinterpret_cast<std::uintptr_t>(address), name.empty() ? "?" : name,
              reinterpret_cast<std::uintptr_t>(address) -
                 reinterpret_cast<std::uintptr_t>(module),
              info->ExceptionRecord->NumberParameters >= 2 &&
                    info->ExceptionRecord->ExceptionInformation[0] == 1
                 ? "writing"
                 : "reading",
              info->ExceptionRecord->NumberParameters >= 2
                 ? static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1])
                 : 0,
              state.head_culls.size(), state.stat_late_head_culls,
              state.stat_frustum_head_culls);

      write_crash_dump(info);
   }

   return previous_crash_filter ? previous_crash_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// The game's CRT (msvcr120) ends the process on a pure virtual call or abort() with a
// fast fail the exception handler never sees; log who called and write the dump first.
auto call_stack_text() noexcept -> std::string
{
   void* frames[24]{};
   const USHORT count = CaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)),
                                              frames, nullptr);
   const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
   std::string stack;

   for (USHORT i = 0; i < count; ++i) {
      const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
      HMODULE module = nullptr;
      wchar_t module_path[MAX_PATH]{};

      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(frames[i]), &module);

      if (reinterpret_cast<std::uintptr_t>(module) == exe) {
         stack += fmt::format(" 0x{:08x}", address - exe + exe_image_base);

         continue;
      }

      if (module) GetModuleFileNameW(module, module_path, MAX_PATH);

      const std::wstring path{module_path};
      const std::string name{path.begin() + (path.find_last_of(L"\\/") + 1), path.end()};

      stack += fmt::format(" {}+0x{:x}", name.empty() ? "?" : name,
                           address - reinterpret_cast<std::uintptr_t>(module));
   }

   return stack;
}

void log_abort(const char* what) noexcept
{
   static std::atomic<int> handled = 0;

   if (handled.fetch_add(1) >= 2) return;

   log_fmt(Log_level::error, "VR: {}, the game will close; call stack (exe addresses as in "
                             "the unrelocated exe):{}",
           what, call_stack_text());

   write_crash_dump(nullptr);
}

void __cdecl purecall_handler() noexcept
{
   log_abort("pure virtual function call");
}

void __cdecl abort_signal_handler(int) noexcept
{
   log_abort("abort()");
}

void watch_crt_aborts() noexcept
{
   const HMODULE crt = GetModuleHandleW(L"msvcr120.dll");

   if (!crt) return;

   using Set_purecall_handler_fn = void*(__cdecl*)(void(__cdecl*)());
   using Signal_fn = void*(__cdecl*)(int, void(__cdecl*)(int));

   if (const auto set_purecall_handler = reinterpret_cast<Set_purecall_handler_fn>(
          GetProcAddress(crt, "_set_purecall_handler"))) {
      set_purecall_handler(&purecall_handler);
   }

   if (const auto signal = reinterpret_cast<Signal_fn>(GetProcAddress(crt, "signal"))) {
      signal(22 /* SIGABRT */, &abort_signal_handler);
   }
}

}

void log_process_exit() noexcept
{
   if (!state.session) return;

   log_fmt(Log_level::info, "VR: the game process is ending (ExitProcess / exit); called "
                            "from (exe addresses as in the unrelocated exe):{}",
           call_stack_text());
}

void init_session(ID3D11Device& device) noexcept
{
   if (!state.instance || state.session) return;

   XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
   binding.device = &device;

   XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
   create_info.next = &binding;
   create_info.systemId = state.system;

   if (failed(state.xr.xrCreateSession(state.instance, &create_info, &state.session),
              "xrCreateSession")) {
      state.session = XR_NULL_HANDLE;

      return;
   }

   state.device = &device;

   previous_crash_filter = SetUnhandledExceptionFilter(crash_filter);
   watch_crt_aborts();

   // Debug switch: a bf2vr_mono.txt next to d3d9.dll keeps the side-by-side layout
   // but draws once per draw call.
   if (GetFileAttributesW((module_directory() + L"bf2vr_mono.txt").c_str()) !=
       INVALID_FILE_ATTRIBUTES) {
      state.stereo_disabled = true;
      log(Log_level::info, "VR: stereo draws disabled by bf2vr_mono.txt.");
   }

   XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
   space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
   space_info.poseInReferenceSpace.orientation.w = 1.0f;
   failed(state.xr.xrCreateReferenceSpace(state.session, &space_info, &state.local_space),
          "xrCreateReferenceSpace(local)");

   space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;

   if (XR_FAILED(state.xr.xrCreateReferenceSpace(state.session, &space_info,
                                                 &state.stage_space))) {
      state.stage_space = XR_NULL_HANDLE;

      log(Log_level::info,
          "VR: no stage space: the player's eye height comes from the settings.");
   }

   if (state.action_set) {
      XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
      attach_info.countActionSets = 1;
      attach_info.actionSets = &state.action_set;
      failed(state.xr.xrAttachSessionActionSets(state.session, &attach_info),
             "xrAttachSessionActionSets");
   }

   // The hands' spaces are made for the hands they're named for.
   set_handles_swapped(false);

   if (state.aim_action) {
      XrActionSpaceCreateInfo aim_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
      aim_info.action = state.aim_action;
      aim_info.poseInActionSpace.orientation.w = 1.0f;
      failed(state.xr.xrCreateActionSpace(state.session, &aim_info, &state.aim_space),
             "xrCreateActionSpace(aim)");
   }

   if (state.left_aim_action) {
      XrActionSpaceCreateInfo aim_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
      aim_info.action = state.left_aim_action;
      aim_info.poseInActionSpace.orientation.w = 1.0f;
      failed(state.xr.xrCreateActionSpace(state.session, &aim_info, &state.left_aim_space),
             "xrCreateActionSpace(left_aim)");
   }

   if (state.grip_action) {
      XrActionSpaceCreateInfo grip_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
      grip_info.action = state.grip_action;
      grip_info.poseInActionSpace.orientation.w = 1.0f;
      failed(state.xr.xrCreateActionSpace(state.session, &grip_info, &state.grip_space),
             "xrCreateActionSpace(grip)");
   }

   if (state.left_grip_action) {
      XrActionSpaceCreateInfo grip_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
      grip_info.action = state.left_grip_action;
      grip_info.poseInActionSpace.orientation.w = 1.0f;
      failed(state.xr.xrCreateActionSpace(state.session, &grip_info, &state.left_grip_space),
             "xrCreateActionSpace(left_grip)");
   }

   load_settings();
   load_weapon_tuning();
   load_bindings();
   hook_game_camera();
   melee::install_hooks();
   melee::load_tuning((module_directory() + L"bf2vr_saber.txt").c_str());

   log(Log_level::info, "VR: session created.");
}

bool active() noexcept
{
   return state.session_running;
}

bool stereo_frame() noexcept
{
   return state.session_running && state.frame_begun && state.views_valid &&
          !state.stereo_disabled;
}

namespace {

// The scope's view (view 2): from the game camera along its aim (where the shots go),
// narrowed by the zoom, with `camera`'s depth planes.
void build_scope_view_projection(const Game_camera& camera,
                                 std::array<glm::vec4, 4>& view_projection) noexcept
{
   const Game_camera aim = aim_camera(camera);
   const float tan = std::tan(scope_base_half_fov_degrees * std::numbers::pi_v<float> / 180.0f) /
                     std::max(settings().scope_zoom, 1.0f);
   const Eye_view view{.position = camera.position,
                       .right = aim.right,
                       .up = aim.up,
                       .forward = aim.forward,
                       .tangents = {-tan, tan, tan, -tan}};

   build_eye_view_projection(view, camera, view_projection);
}

// Weapon sway off: a first-person model part's draw moved into a hand with the part's
// eased pose in the camera's view space instead of this frame's (the run's bobbing left
// out), the hand's grip at its grip part's eased origin. False when that can't be done
// (the part isn't known by its rank, or the grip part not yet seen).
bool steady_in_hand(const Draw_info& draw, const int rank, const int eye, const bool left,
                    std::array<glm::vec4, 4>& eye_vp, glm::vec3& eye_position) noexcept
{
   if (rank < 0 || rank >= static_cast<int>(state.steady_parts.size())) return false;

   const Game_camera& camera = state.cached_weapon_camera;
   const glm::mat3 to_view = glm::transpose(camera_rotation(camera));

   // The part's pose in view space (its object matrix may scale: taken out).
   glm::mat3 rotation = to_view * draw.object_rotation;

   for (int i = 0; i < 3; ++i) {
      const float length = glm::length(rotation[i]);

      if (length < 1e-4f) return false;

      rotation[i] /= length;
   }

   const glm::quat current = glm::normalize(glm::quat_cast(rotation));
   const glm::vec3 origin = to_view * (draw.object_position - camera.position);

   State::Steady_part& part = state.steady_parts[rank];

   // Eased once per frame, by its first draw.
   if (part.frame != state.frame_index) {
      const float cos_half = std::min(std::abs(glm::dot(part.rotation, current)), 1.0f);
      const float degrees = 2.0f * std::acos(cos_half) * 180.0f / std::numbers::pi_v<float>;

      if (!part.valid || glm::distance(part.origin, origin) > weapon_sway_snap_distance ||
          degrees > weapon_sway_snap_degrees) {
         part.rotation = current;
         part.origin = origin;
      }
      else {
         const float t = 1.0f - std::exp(-std::max(state.anchor_dt, 0.0f) / weapon_sway_seconds);

         part.rotation = glm::normalize(glm::slerp(part.rotation, current, t));
         part.origin = glm::mix(part.origin, origin, t);
      }

      part.valid = true;
      part.frame = state.frame_index;
   }

   const int grip = left ? state.cached_left_grip : state.cached_right_grip;
   glm::vec3 grip_view;

   if (!left && grip < 0) {
      grip_view = state.weapon_grip_view; // bf2vr_weapon.txt, or no part ranked yet
   }
   else if (grip >= 0 && grip < static_cast<int>(state.steady_parts.size()) &&
            state.steady_parts[grip].valid) {
      grip_view = state.steady_parts[grip].origin;
   }
   else {
      return false;
   }

   grip_view += left ? state.cached_left_grip_tuning : state.cached_right_grip_tuning;

   // View space: v' = steady * (v - origin) + eased origin; then into the hand as
   // place_in_hand does: p' = hand * (v' - grip) + hand position.
   const World_pose& hand = left ? state.cached_left_hand : state.cached_right_hand;
   const glm::mat3 steady = glm::mat3_cast(part.rotation) * glm::transpose(rotation);
   const glm::mat3 r = hand.rotation * steady * to_view;
   const glm::vec3 t =
      hand.position +
      hand.rotation * (part.origin - grip_view - steady * origin - steady * (to_view * camera.position));

   eye_vp = state.cached_near_eye_vp[eye];
   eye_position = state.cached_eye_position[eye];

   // The weapon part in the hand the shots come from (fire_from_hand).
   if (rank == 0 && left == state.fire_map_left) {
      state.steady_gun_r = r;
      state.steady_gun_t = t;
      state.steady_gun_frame = state.frame_index;
      state.steady_gun_valid = true;
   }

   transform_draw(r, t, eye_vp, eye_position);

   return true;
}

}

namespace {

// A draw of the weapon part (rank 0) that is the shot's muzzle flash: drawn with the
// gun's object matrix (it joins the gun's part), so it went into the hand where the
// game's model has its muzzle, not where the shots start. Told by its size: the gun's
// own draws' sizes are learnt while the player isn't shooting; another size shortly
// after a shot is the flash.
bool muzzle_flash_draw(const Draw_info& draw, const int eye) noexcept
{
   if (state.gun_draw_sizes_model != state.model_signature) {
      state.gun_draw_sizes_model = state.model_signature;
      state.gun_draw_size_count = 0;
   }

   const auto sizes_end = state.gun_draw_sizes.begin() + state.gun_draw_size_count;
   const bool known = std::find(state.gun_draw_sizes.begin(), sizes_end, draw.element_count) !=
                      sizes_end;

   if (!melee::player_shot_within(flash_after_shot_seconds)) {
      if (eye == 0 && !known && state.gun_draw_size_count < state.gun_draw_sizes.size()) {
         state.gun_draw_sizes[state.gun_draw_size_count++] = draw.element_count;
      }

      return false;
   }

   if (known || state.gun_draw_size_count == 0) return false;

   if (eye == 0 && !std::exchange(state.flash_logged, true)) {
      log_fmt(Log_level::info,
              "VR: a muzzle flash drawn with the gun ({} elements, shader '{}'): moved to "
              "where the shots start.",
              draw.element_count, draw.shader_name);
   }

   return true;
}

// The current model's tip record (null while there's no model).
auto current_gun_tip() noexcept -> State::Gun_tip*
{
   if (state.model_signature.empty()) return nullptr;

   return &state.gun_tips[state.model_signature];
}

// Where the barrel tip is now (world, the game's first-person frame like the game's fire
// point), or nullopt without a tip for the model or a fresh draw of its weapon part.
auto gun_tip_world() noexcept -> std::optional<glm::vec3>
{
   const auto found = state.gun_tips.find(state.model_signature);

   if (found == state.gun_tips.end() || !found->second.tip_known ||
       state.frame_index - state.gun_object_frame > 2) {
      return std::nullopt;
   }

   return state.gun_object_rotation * found->second.tip_object + state.gun_object_position;
}

void finish_gun_tip() noexcept;

// The weapon part's draw (eye 0): remembers its matrix and, while the model has no tip
// yet, asks for its positions (not right after a shot: the flash isn't the barrel).
void note_gun_draw(const Draw_info& draw) noexcept
{
   state.gun_object_frame = state.frame_index;
   state.gun_object_rotation = draw.object_rotation;
   state.gun_object_position = draw.object_position;

   State::Gun_tip* const tip = current_gun_tip();

   // A frame's read-back is finished once its frame is over.
   if (state.tip_capture_frame != 0 && state.tip_capture_frame != state.frame_index) {
      finish_gun_tip();
   }

   if (!tip || tip->tip_known || tip->failed || tip->tries >= gun_tip_max_tries ||
       melee::player_shot_within(flash_after_shot_seconds)) {
      return;
   }

   if (state.tip_capture_model != state.model_signature ||
       state.tip_capture_frame != state.frame_index) {
      state.tip_capture_model = state.model_signature;
      state.tip_capture_frame = state.frame_index;
      state.tip_capture_view.clear();
      state.tip_capture_camera = state.cached_weapon_camera;
      state.tip_capture_rotation = draw.object_rotation;
      state.tip_capture_position = draw.object_position;
   }

   state.positions_request = true;
   state.positions_for_flash = false;
   state.positions_rotation = draw.object_rotation;
   state.positions_position = draw.object_position;
}

// The flash's draw (eye 0): asks once per model for its positions (its centre).
void note_flash_draw(const Draw_info& draw) noexcept
{
   State::Gun_tip* const tip = current_gun_tip();

   if (!tip || !tip->tip_known || tip->flash_known ||
       state.frame_index - state.gun_object_frame > 2) {
      return;
   }

   state.positions_request = true;
   state.positions_for_flash = true;
   state.positions_rotation = draw.object_rotation;
   state.positions_position = draw.object_position;
}

// After a frame that read the weapon part back: the tip is the middle of the vertices
// within gun_tip_depth of the frontmost one (along the game camera's forward, which the
// first-person gun points along).
void finish_gun_tip() noexcept
{
   if (state.tip_capture_frame == 0 || state.tip_capture_frame == state.frame_index) return;

   const std::uint64_t frame = std::exchange(state.tip_capture_frame, 0);
   State::Gun_tip& tip = state.gun_tips[state.tip_capture_model];

   if (tip.tip_known || tip.failed) return;

   tip.tries += 1;

   if (state.tip_capture_view.size() < 3) {
      log_fmt(Log_level::info, "VR: the barrel tip of {}: too few vertices read (frame {}).",
              state.tip_capture_model, frame);

      return;
   }

   float front = 0.0f;

   for (const glm::vec3& v : state.tip_capture_view) front = std::min(front, v.z);

   glm::vec3 sum{};
   int count = 0;

   for (const glm::vec3& v : state.tip_capture_view) {
      if (v.z <= front + gun_tip_depth) {
         sum += v;
         count += 1;
      }
   }

   const glm::vec3 tip_view = sum / static_cast<float>(count);
   const Game_camera& camera = state.tip_capture_camera;
   const glm::vec3 tip_world = camera.position + camera_rotation(camera) * tip_view;

   tip.tip_object = glm::inverse(state.tip_capture_rotation) *
                    (tip_world - state.tip_capture_position);
   tip.tip_known = true;

   log_fmt(Log_level::info,
           "VR: the barrel tip of {}: ({:.3f}, {:.3f}, {:.3f}) in the game camera's view "
           "space, from {} of {} vertices.",
           state.tip_capture_model, tip_view.x, tip_view.y, tip_view.z, count,
           state.tip_capture_view.size());
}

// The flash's draw moved like the gun in the hand that shoots, then from where the game
// put it to the barrel tip, then by the model's muzzle offset (see fire_from_hand).
// False when there's no such gun this frame.
// `at_shot_start`: the draw is already where the shots start (flash_from_hand), only
// moved like the gun.
bool flash_in_hand(const int eye, std::array<glm::vec4, 4>& eye_vp, glm::vec3& eye_position,
                   const bool at_shot_start = false) noexcept
{
   if (!state.fire_map_valid || state.frame_index - state.fire_map_frame > 2) return false;

   const bool steady = !settings().weapon_sway && state.steady_gun_valid &&
                       state.frame_index - state.steady_gun_frame <= 2;
   const glm::mat3& r = steady ? state.steady_gun_r : state.fire_map_r;
   glm::vec3 shift{};

   if (!at_shot_start) {
      shift = state.fire_map_camera_rotation * state.fire_map_muzzle_offset;

      if (const State::Gun_tip* const tip = current_gun_tip();
          tip && tip->tip_known && tip->flash_known) {
         shift += state.gun_object_rotation * (tip->tip_object - tip->flash_object);
      }
   }

   const glm::vec3 t = (steady ? state.steady_gun_t : state.fire_map_t) + r * shift;

   eye_vp = state.cached_near_eye_vp[eye];
   eye_position = state.cached_eye_position[eye];

   transform_draw(r, t, eye_vp, eye_position);

   if (eye == 0) state.stat_weapon_draws += 1;

   return true;
}

}

auto choose_eye_draw(const std::array<glm::vec4, 4>& game_view_projection, const int eye,
                     const bool at_infinity, const Draw_info& draw,
                     std::array<glm::vec4, 4>& eye_vp, glm::vec3& eye_positionWS) noexcept
   -> Eye_draw
{
   state.last_draw_weapon = false;

   if (!stereo_frame()) return Eye_draw::as_game;
   if (eye >= 2 && view_count() < 3) return Eye_draw::skip;

   // Draws come in long runs with the same camera; only redo the maths when it changes.
   if (!state.cache_filled || state.cached_frame_index != state.frame_index ||
       std::memcmp(&state.cached_game_vp, &game_view_projection,
                   sizeof(game_view_projection)) != 0) {
      state.cache_filled = true;
      state.cached_frame_index = state.frame_index;
      state.cached_game_vp = game_view_projection;

      Game_camera camera;
      const Reject reject = decompose_camera(game_view_projection, camera);

      state.cached_valid = false;
      state.cached_in_hand = false;
      state.cached_left_in_hand = false;
      state.cached_weapon_pass = false;
      state.cached_hud = false;
      state.cached_far_anchored = false;
      state.current_frame_camera = -1;

      if (reject != Reject::none) {
         log_rejected_camera(reject, game_view_projection);

         return eye >= 2 ? Eye_draw::skip : Eye_draw::as_game;
      }

      // Remember the camera for picking the frame's world camera.
      const auto entry = std::find_if(state.frame_cameras.begin(),
                                      state.frame_cameras.end(), [&](const auto& c) {
                                         return std::memcmp(&c.game_vp,
                                                            &game_view_projection,
                                                            sizeof(game_view_projection)) == 0;
                                      });

      if (entry != state.frame_cameras.end()) {
         state.current_frame_camera =
            static_cast<int>(entry - state.frame_cameras.begin());
      }
      else if (state.frame_cameras.size() < 16) {
         state.frame_cameras.push_back(
            {game_view_projection, camera, 0, state.frame_draws, draw.depthstencil,
             at_infinity});
         state.current_frame_camera = static_cast<int>(state.frame_cameras.size() - 1);
      }

      const bool view_space = is_view_space_camera(camera);
      const bool world_known = state.frame_world_camera_valid &&
                               glm::length(state.frame_world_camera.position) > 1.0f;
      const Game_camera& world = state.frame_world_camera;

      // HUD: view-space draws while the world camera is elsewhere.
      const bool hud = view_space && world_known;

      // First-person weapon: the world camera's pose with a nearer near plane than the
      // world camera's (taken from the last frame: 0.5 m; the weapon has 0.3 m).
      const bool weapon = !view_space && world_known && state.main_camera_valid &&
                          glm::distance(camera.position, world.position) < 0.05f &&
                          near_plane(camera) < near_plane(state.main_camera) - 0.05f;

      state.cached_weapon_pass = weapon;

      if (weapon) state.weapon_seen_this_frame = true;

      if (weapon && state.current_frame_camera >= 0) {
         state.frame_cameras[state.current_frame_camera].weapon = true;
      }

      if (!view_space && !weapon && camera.handedness == state.world_handedness) {
         state.frame_world_camera = camera;
         state.frame_world_camera_valid = true;

         if (state.recentered && !state.menu_frame) {
            update_anchor(camera);
         }
         else {
            plain_anchor();
            state.body_anchored = false;
         }
      }

      if (!state.recentered) {
         // Drawn as the game made it until the frame's world camera is known.
      }
      else if (state.menu_frame) {
         // Drawn as the game made it, for the virtual screen.
      }
      else if (camera.handedness != state.world_handedness) {
         state.mirrored_seen_this_frame = true;

         if (state.mirrored_logged++ == 0) {
            log(Log_level::info, "VR: skipping a mirrored camera (reflection pass).");
         }
      }
      else if (hud || weapon) {
         // Eyes at the world camera; the geometry is moved instead: the weapon into the
         // hand, the HUD in front of the head, turned with it (when it isn't drawn from
         // the HUD layer, see draw_hud).
         update_eyes(world);
         state.cached_valid = true;
         state.cached_hud = hud;
         // Each hand's grip: the origin of its first model part (last frame's), moved
         // by the model's corrections (bf2vr_arms.txt, tuned in the VR panel).
         const int n = static_cast<int>(state.ranked_parts.size());
         const int right_grip = hand_parts_grip_rank(state.right_hand_parts, n);
         const int left_grip = hand_parts_grip_rank(state.left_hand_parts, n);

         state.cached_in_hand = weapon && state.aim_valid && right_grip >= 0;
         state.cached_left_in_hand = weapon && state.left_aim_valid && left_grip >= 0;

         const Arm_tuning& tuning = arm_tuning(state.model_signature);
         const glm::vec3 right_grip_view =
            (state.weapon_grip_from_file || right_grip < 0
                ? state.weapon_grip_view
                : state.ranked_parts[right_grip].origin_view) +
            tuning.right_offset;
         const glm::vec3 left_grip_view =
            (left_grip < 0 ? glm::vec3{} : state.ranked_parts[left_grip].origin_view) +
            tuning.left_offset;

         World_pose hand = weapon_pose_world(world);
         World_pose left_hand = aim_pose_world(world, state.left_aim_pose);

         // Hands swapped: the model mirrored (its right arm and gun into the left hand as
         // a left arm, the left arm into the right). A hand's x axis flipped mirrors all
         // that's placed by it: the parts, steadied parts, the shots' start. In-hand draws
         // are two-sided, so the flipped winding doesn't matter.
         if (state.handles_swapped) {
            hand.rotation[0] = -hand.rotation[0];
            left_hand.rotation[0] = -left_hand.rotation[0];
         }

         // The first-person model sized with the body (a Jawa's gun is a Jawa's): its
         // hand frames scaled, so the model shrinks about the hand, and with it all that's
         // placed by them (parts, steadied parts, the shots' start, the flash).
         hand.rotation *= state.tracking_scale;
         left_hand.rotation *= state.tracking_scale;
         const glm::mat3 hud_rotation{state.eyes[0].right, state.eyes[0].up,
                                      -state.eyes[0].forward};
         const glm::vec3 hud_position = head_position_world(world);

         // For steadied parts (see steady_in_hand).
         state.cached_right_hand = hand;
         state.cached_left_hand = left_hand;
         state.cached_right_grip = state.weapon_grip_from_file ? -1 : right_grip;
         state.cached_left_grip = left_grip;
         state.cached_right_grip_tuning = tuning.right_offset;
         state.cached_left_grip_tuning = tuning.left_offset;

         // The near scene also holds nearby world objects: those keep the plain eye
         // cameras, only the first-person model's draws take the hand ones. The model
         // is placed in the frame it follows (see Weapon_frame).
         state.cached_weapon_render_camera = camera;
         state.cached_weapon_camera =
            state.weapon_frame == Weapon_frame::aim ? aim_camera(camera) : camera;

         // For the shots (fire_from_hand): how the gun goes from the camera to the hand
         // that holds the weapon part.
         const bool gun_right = state.cached_in_hand && hand_parts_have(state.right_hand_parts, 0);
         const bool gun_left = !gun_right && state.cached_left_in_hand &&
                               hand_parts_have(state.left_hand_parts, 0);

         if (gun_right || gun_left) {
            const Game_camera& gun_camera = state.cached_weapon_camera;
            const World_pose& gun_hand = gun_right ? hand : left_hand;
            const glm::vec3& gun_grip = gun_right ? right_grip_view : left_grip_view;

            state.fire_map_camera_rotation = camera_rotation(gun_camera);
            state.fire_map_r = gun_hand.rotation * glm::transpose(state.fire_map_camera_rotation);
            state.fire_map_t = gun_hand.position - gun_hand.rotation * gun_grip -
                               state.fire_map_r * gun_camera.position;
            state.fire_map_forward = glm::normalize(state.fire_map_r * gun_camera.forward);
            state.fire_map_camera = gun_camera.position;
            state.fire_map_muzzle_offset = tuning.muzzle_offset;
            state.fire_map_left = gun_left;
            state.fire_map_frame = state.frame_index;
            state.fire_map_valid = true;
         }

         const Game_camera hand_camera = with_near_plane(camera, in_hand_near_plane);
         const Game_camera eye_camera = body_near_plane(camera, state.tracking_scale);

         for (int i = 0; i < 2; ++i) {
            build_eye_view_projection(state.eyes[i], eye_camera, state.cached_eye_vp[i]);
            state.cached_eye_position[i] = state.eyes[i].position;

            if (hud) {
               transform_draw(hud_rotation, hud_position, state.cached_eye_vp[i],
                              state.cached_eye_position[i]);
            }

            std::array<glm::vec4, 4> near_eye_vp{};

            if (state.cached_in_hand || state.cached_left_in_hand) {
               build_eye_view_projection(state.eyes[i], hand_camera, near_eye_vp);
               state.cached_near_eye_vp[i] = near_eye_vp;
            }

            if (state.cached_in_hand) {
               state.cached_hand_vp[i] = near_eye_vp;
               state.cached_hand_position[i] = state.cached_eye_position[i];
               place_in_hand(hand, state.cached_weapon_camera, right_grip_view,
                             state.cached_hand_vp[i],
                             state.cached_hand_position[i]);
            }

            if (state.cached_left_in_hand) {
               state.cached_left_hand_vp[i] = near_eye_vp;
               state.cached_left_hand_position[i] = state.cached_eye_position[i];
               place_in_hand(left_hand, state.cached_weapon_camera, left_grip_view,
                             state.cached_left_hand_vp[i],
                             state.cached_left_hand_position[i]);
            }

            state.cached_eye_vp_at_infinity[i] = state.cached_eye_vp[i];
         }

         // The scope sees what's near the gun (not the first-person model, nor the HUD).
         if (view_count() == 3) {
            build_scope_view_projection(camera, state.cached_eye_vp[2]);
            state.cached_eye_vp_at_infinity[2] = state.cached_eye_vp[2];
            state.cached_eye_position[2] = camera.position;
         }

         state.cached_camera_position = state.cached_eye_position[0];

         if (weapon && !std::exchange(state.weapon_draw_logged, true)) {
            log_fmt(Log_level::info,
                    "VR: first-person weapon pass found (near plane {:.2f} m, world "
                    "camera's {:.2f} m).",
                    near_plane(camera), near_plane(state.main_camera));
         }

         if (hud && !std::exchange(state.hud_draw_logged, true)) {
            log(Log_level::info, "VR: drawing the view-space HUD along the game camera.");
         }
      }
      else {
         // The world camera (and anything else drawn with plain eye cameras).
         update_eyes(camera);
         state.cached_valid = true;

         const Game_camera eye_camera = body_near_plane(camera, state.tracking_scale);

         for (int i = 0; i < 2; ++i) {
            build_eye_view_projection(state.eyes[i], eye_camera, state.cached_eye_vp[i]);
            state.cached_eye_position[i] = state.eyes[i].position;

            // Without the head's own movement: the sky from the camera, and on a turret
            // the rest from where the eyes are anchored (not the camera circling it: the
            // distant buildings swung round with it).
            // The game centres the sky dome where the camera stands while it draws:
            // the eyes when head culling moved it there (see head_cull_camera), else
            // the dome steps with the game camera's bobbing against the eyes.
            Eye_view eye_at_camera = state.eyes[i];
            eye_at_camera.position = sky_centre(camera.position);

            build_eye_view_projection(eye_at_camera, eye_camera,
                                      state.cached_eye_vp_at_infinity[i]);
            state.cached_camera_position = eye_at_camera.position;

            eye_at_camera.position = camera.position + state.anchor_offset;

            build_eye_view_projection(eye_at_camera, eye_camera,
                                      state.cached_eye_vp_far_anchored[i]);
            state.cached_far_anchored_position = eye_at_camera.position;
            state.cached_far_anchored = state.turret_anchor;
         }

         if (view_count() == 3) {
            build_scope_view_projection(camera, state.cached_eye_vp[2]);
            state.cached_eye_vp_at_infinity[2] = state.cached_eye_vp[2];
            state.cached_eye_position[2] = camera.position;
         }

         state.camera_seen_this_frame = true;
      }
   }

   if (eye == 0) {
      state.frame_draws += 1;

      if (state.current_frame_camera >= 0) {
         state.frame_cameras[state.current_frame_camera].draws += 1;
      }

   }

   // Near scene: the first-person model (object origin at the camera) goes into the hand
   // in parts (one per object), nearby world objects stay where they are.
   if (state.cached_weapon_pass) {
      const Game_camera& camera = state.cached_weapon_camera;
      const glm::vec3 offset = draw.object_position - camera.position;
      const glm::vec3 origin_view = glm::transpose(camera_rotation(camera)) * offset;
      int model_part = -1;

      // The same origin in the other candidate frame (see Weapon_frame).
      const Game_camera other_camera =
         state.weapon_frame == Weapon_frame::aim ? state.cached_weapon_render_camera
                                                 : aim_camera(state.cached_weapon_render_camera);
      const glm::vec3 origin_other = glm::transpose(camera_rotation(other_camera)) * offset;

      // The gun's muzzle flash, drawn by the game where the shots start
      // (flash_from_hand): into the hand the way the shots go, not as a model part.
      if (state.flash_origin_frame == state.frame_index &&
          glm::distance(draw.object_position, state.flash_origin) < 0.01f) {
         if (eye >= 2) return Eye_draw::skip;
         if (flash_in_hand(eye, eye_vp, eye_positionWS, true)) return Eye_draw::in_hand;
      }

      // The player's saber, put in the hand by vr_melee.cpp, is found by where its hilt
      // went: with the arm stretched or hanging it can be further from the game's camera
      // than the model radius (then it was drawn as a world object, near clipped).
      const std::optional<glm::vec3> saber_hilt = melee::player_saber_hilt();
      const std::optional<glm::vec3> left_saber_hilt = melee::player_left_saber_hilt();
      const bool saber_draw =
         (saber_hilt && glm::distance(draw.object_position, *saber_hilt) < saber_hilt_match) ||
         (left_saber_hilt &&
          glm::distance(draw.object_position, *left_saber_hilt) < saber_hilt_match);

      if (glm::length(offset) < first_person_model_radius || saber_draw) {
         auto& parts = state.model_parts;
         auto part = std::find_if(parts.begin(), parts.end(), [&](const Model_part& p) {
            return glm::distance(p.origin_view, origin_view) < 0.001f;
         });

         if (part == parts.end() && parts.size() < max_model_parts) {
            part = parts.insert(parts.end(), Model_part{origin_view, 0, 0, origin_other});
         }

         model_part = part == parts.end() ? static_cast<int>(max_model_parts)
                                          : static_cast<int>(part - parts.begin());

         if (eye == 0 && part != parts.end()) {
            part->draws += 1;
            part->elements += draw.element_count;
         }
      }

      if (eye == 0 && state.weapon_draws_log_pending &&
          state.weapon_pass_draws < max_logged_weapon_draws) {
         state.weapon_draws_logged_this_frame = true;

         log_fmt(Log_level::info,
                 "VR: near scene draw {}: {} elements, shader '{}', object {:.2f} m from "
                 "the camera{}.",
                 state.weapon_pass_draws, draw.element_count, draw.shader_name,
                 glm::length(offset),
                 model_part >= 0 ? fmt::format(", first-person model part {}", model_part + 1)
                                 : std::string{});
      }

      if (eye == 0) state.weapon_pass_draws += 1;

      // Each part goes to the hand whose mode holds it (ranked by size last frame);
      // parts no hand holds aren't drawn. Without hand tracking the model stays in
      // front of the head as the game drew it.
      // The scope's view doesn't show the first-person model.
      if (eye >= 2 && model_part >= 0) return Eye_draw::skip;

      // The saber is away (thrown): nothing of the first-person model in the hand.
      if (model_part >= 0 && melee::player_saber_thrown()) return Eye_draw::skip;

      // The player's saber is drawn by the game at the controller already (see
      // vr_melee.cpp): drawn with the eyes (near the face: no near clipping). Anything
      // else of the first-person model (at the camera) isn't drawn.
      if (model_part >= 0 && melee::game_saber_in_hand()) {
         if (glm::length(offset) < 0.05f) return Eye_draw::skip;

         if (eye == 0) state.stat_weapon_draws += 1;

         build_eye_view_projection(state.eyes[eye],
                                   with_near_plane(state.cached_weapon_render_camera,
                                                   in_hand_near_plane),
                                   eye_vp);
         eye_positionWS = state.eyes[eye].position;

         return Eye_draw::in_hand;
      }

      if (model_part >= 0 && state.aim_valid) {
         const int n = static_cast<int>(state.ranked_parts.size());
         const int rank = ranked_part_index(origin_view);

         if (rank == 0) {
            const bool flash = muzzle_flash_draw(draw, eye);

            if (eye == 0) flash ? note_flash_draw(draw) : note_gun_draw(draw);

            if (flash && flash_in_hand(eye, eye_vp, eye_positionWS)) return Eye_draw::in_hand;
         }

         if (state.cached_in_hand && hand_parts_have(state.right_hand_parts, rank) &&
             (settings().show_right_arm || rank == 0)) {
            if (eye == 0) state.stat_weapon_draws += 1;

            state.last_draw_weapon = rank == 0;

            if (!settings().weapon_sway &&
                steady_in_hand(draw, rank, eye, false, eye_vp, eye_positionWS)) {
               return Eye_draw::in_hand;
            }

            eye_vp = state.cached_hand_vp[eye];
            eye_positionWS = state.cached_hand_position[eye];

            return Eye_draw::in_hand;
         }

         if (state.cached_left_in_hand && hand_parts_have(state.left_hand_parts, rank) &&
             (settings().show_left_arm || rank == 0)) {
            if (!settings().weapon_sway &&
                steady_in_hand(draw, rank, eye, true, eye_vp, eye_positionWS)) {
               return Eye_draw::in_hand;
            }

            eye_vp = state.cached_left_hand_vp[eye];
            eye_positionWS = state.cached_left_hand_position[eye];

            return Eye_draw::in_hand;
         }

         if (n > 0) return Eye_draw::skip;
      }
   }

   if (!state.cached_valid) return eye >= 2 ? Eye_draw::skip : Eye_draw::as_game;
   if (eye >= 2 && state.cached_hud) return Eye_draw::skip;

   if (at_infinity && eye == 0) state.stat_far_draws += 1;

   if (at_infinity && eye < 2 && state.cached_far_anchored && !state.cached_hud &&
       glm::distance(draw.object_position, state.cached_camera_position) >
          far_scene_camera_centred) {
      eye_vp = state.cached_eye_vp_far_anchored[eye];
      eye_positionWS = state.cached_far_anchored_position;

      return Eye_draw::eye_camera;
   }

   eye_vp = at_infinity ? state.cached_eye_vp_at_infinity[eye] : state.cached_eye_vp[eye];
   eye_positionWS = at_infinity ? state.cached_camera_position : state.cached_eye_position[eye];

   return Eye_draw::eye_camera;
}

// See State::seen_skinned.
void note_skinned_draw(const Draw_info& draw) noexcept
{
   if (state.seen_skinned_frame != state.frame_index) {
      state.seen_skinned_frame = state.frame_index;
      state.seen_skinned_head = head_position_world(state.main_camera);

      const std::optional<melee::Player_soldier> soldier = melee::player_soldier();

      state.seen_skinned_feet = soldier ? std::optional{soldier->position} : std::nullopt;

      std::erase_if(state.seen_skinned, [](const State::Seen_skinned& seen) {
         return state.frame_index - seen.frame > turret_seen_frames;
      });
   }

   if (glm::distance(draw.object_position, state.seen_skinned_head) > turret_seen_distance) {
      return;
   }

   // The player's own body: not something else's.
   if (state.seen_skinned_feet &&
       glm::distance(draw.object_position, *state.seen_skinned_feet) < player_feet_radius) {
      auto& body = state.player_body_elements;

      if (std::find(body.begin(), body.end(), draw.element_count) == body.end()) {
         if (body.size() >= player_body_max) body.erase(body.begin());

         body.push_back(draw.element_count);
      }

      return;
   }

   for (State::Seen_skinned& seen : state.seen_skinned) {
      if (seen.elements == draw.element_count &&
          glm::distance(seen.origin, draw.object_position) < turret_seen_match) {
         seen.origin = draw.object_position;
         seen.frame = state.frame_index;

         return;
      }
   }

   if (state.seen_skinned.size() < turret_seen_max) {
      state.seen_skinned.push_back({draw.object_position, draw.element_count, state.frame_index});
   }
}

void take_turret_own_draws() noexcept
{
   state.turret_own_taken = true;
   state.turret_own_elements.clear();

   const auto& body = state.player_body_elements;
   const auto is_body = [&](const UINT elements) {
      return std::find(body.begin(), body.end(), elements) != body.end();
   };

   state.turret_gunner_elements.clear();
   state.turret_manned_frame = state.frame_index;

   for (const State::Seen_skinned& seen : state.seen_skinned) {
      if (glm::distance(seen.origin, *state.turret_body) < turret_gunner_radius &&
          !is_body(seen.elements)) {
         state.turret_own_elements.push_back(seen.elements);
      }
   }

   std::string body_text;

   for (const UINT elements : body) body_text += fmt::format(" {}", elements);

   log_fmt(Log_level::info,
           "VR: the turret's own skinned draws (seen before it was manned): {}; the player's "
           "body draws (seen at its feet):{}; any other skinned draw at the player's soldier "
           "is left out.",
           state.turret_own_elements.size(), body_text.empty() ? " none" : body_text);
}

auto eye_view_projection(const std::array<glm::vec4, 4>& game_view_projection,
                         const int eye, const bool at_infinity, const Draw_info& draw,
                         std::array<glm::vec4, 4>& eye_vp,
                         glm::vec3& eye_positionWS) noexcept -> Eye_draw
{
   if (eye == 0 && draw.skinned && !state.turret_anchor && stereo_frame() &&
       !state.menu_frame) {
      note_skinned_draw(draw);
   }

   // On a turret the player's own soldier (skinned, at its entity, not one of the
   // turret's own draws) isn't drawn.
   if (state.turret_anchor && state.turret_body && draw.skinned) {
      const float distance = glm::distance(draw.object_position, *state.turret_body);

      if (eye == 0) {
         state.stat_turret_skinned_draws += 1;
         state.stat_turret_nearest_skinned =
            std::min(state.stat_turret_nearest_skinned, distance);
      }

      if (!state.turret_own_taken) take_turret_own_draws();

      const auto& own = state.turret_own_elements;
      const auto& body = state.player_body_elements;
      auto& gunner = state.turret_gunner_elements;
      const auto has = [&](const std::vector<UINT>& list) {
         return std::find(list.begin(), list.end(), draw.element_count) != list.end();
      };

      // New near the turret just after manning it: the gunner.
      if (distance < turret_gunner_radius && !has(own) && !has(gunner) &&
          state.frame_index - state.turret_manned_frame < turret_gunner_frames &&
          gunner.size() < 16) {
         gunner.push_back(draw.element_count);

         log_fmt(Log_level::info,
                 "VR: the turret's gunner: a skinned draw of {} elements, {:.2f} m from the "
                 "player's soldier entity, left out.",
                 draw.element_count, distance);
      }

      if (distance < turret_gunner_radius && (has(body) || has(gunner))) {
         if (eye == 0) state.stat_turret_body_draws += 1;

         return Eye_draw::skip;
      }
   }

   return choose_eye_draw(game_view_projection, eye, at_infinity, draw, eye_vp, eye_positionWS);
}

bool mirrored_in_hand(const Draw_info& draw, std::array<glm::vec4, 4>& eye_vp,
                      glm::vec3& eye_positionWS) noexcept
{
   if (!state.last_draw_weapon) return false;

   const Arm_tuning& tuning = arm_tuning(state.model_signature);

   if (!tuning.mirror) return false;

   // The part's axis in the world (its object matrix may scale).
   const glm::vec3 axis = draw.object_rotation[std::clamp(tuning.mirror_axis, 0, 2)];
   const float length = glm::length(axis);

   if (length < 1e-4f) return false;

   const glm::vec3 normal = axis / length;
   const glm::mat3 reflection = glm::mat3{1.0f} - 2.0f * glm::outerProduct(normal, normal);
   const float scale = std::clamp(tuning.mirror_scale, 0.8f, 1.05f);
   const glm::vec3 origin = draw.object_position;
   glm::vec3 shift{};

   for (int i = 0; i < 3; ++i) {
      const float axis_length = glm::length(draw.object_rotation[i]);

      if (axis_length > 1e-4f) {
         shift += draw.object_rotation[i] / axis_length * tuning.mirror_shift[i];
      }
   }

   // p -> scale * reflection * (p - origin) + origin + shift, before the hand's placement
   // (transform_draw maps the eye back as for a rotation: done here for the scale).
   const glm::mat3 r = reflection * scale;
   const glm::vec3 t = origin + shift - r * origin;
   const glm::vec3 eye = reflection * (eye_positionWS - t) / scale;

   transform_draw(r, t, eye_vp, eye_positionWS);
   eye_positionWS = eye;

   return true;
}

void submit_frame(ID3D11DeviceContext& dc, ID3D11Texture2D& frame,
                  const bool side_by_side) noexcept
{
   if (!state.session) return;

   if (state.device && !state.device_removal_logged) {
      if (const HRESULT reason = state.device->GetDeviceRemovedReason(); FAILED(reason)) {
         state.device_removal_logged = true;
         log_fmt(Log_level::error, "VR: D3D11 device removed, reason 0x{:08X} (frame {}).",
                 static_cast<std::uint32_t>(reason), state.frame_index);
      }
   }

   if (state.frame_begun) end_frame(dc, frame, side_by_side);

   state.stat_frames += 1;
   if (state.camera_seen_this_frame) state.stat_frames_with_camera += 1;

   if (!state.camera_seen_this_frame && state.mirrored_seen_this_frame) {
      state.stat_mirrored_frames += 1;
   }

   state.mirrored_seen_this_frame = false;
   state.hud_layer_vp_valid = false;
   state.hud_layer_draws = 0;
   restore_aim_rows();
   end_head_cull();
   finish_frame_cameras();
   log_stats();
   finish_gun_tip();

   // Rank the first-person model's parts by size: the gun is the biggest, the rest are
   // arm pieces. Kept from the last frame that drew the model.
   // A different part count is only taken once it lasts: a shot's muzzle flash is an
   // extra part for a few frames, and a part can miss a frame or two during fast swings
   // (log 2026-10-26: 3609-2025 flickered to 2025 and to three parts).
   if (!state.model_parts.empty() && !state.ranked_parts.empty() &&
       state.model_parts.size() != state.ranked_parts.size() &&
       ++state.part_count_change_frames < part_count_change_min_frames) {
      state.model_parts.clear();
   }

   if (!state.model_parts.empty()) {
      state.part_count_change_frames = 0;

      std::stable_sort(state.model_parts.begin(), state.model_parts.end(),
                       [](const Model_part& a, const Model_part& b) {
                          return a.elements > b.elements;
                       });

      if (state.model_parts.size() != state.ranked_parts.size()) {
         default_hand_parts(static_cast<int>(state.model_parts.size()));
      }

      state.ranked_parts = state.model_parts;

      update_weapon_frame(state.ranked_parts[0]);

      // The model's name for its hand corrections: its parts' sizes, biggest first.
      std::string signature;

      for (const Model_part& part : state.ranked_parts) {
         signature += fmt::format("{}{}", signature.empty() ? "" : "-", part.elements);
      }

      // A new name is only taken once it lasts (or the part count changed: another
      // weapon): a shot's muzzle flash is drawn with the gun and adds its elements to it
      // for a few frames (log 2026-10-26: 3248-951-... and 4872-951-... in turns), which
      // lost the model's muzzle offset for exactly the shots, and its steadying.
      if (signature == state.model_signature) {
         state.signature_change_frames = 0;
      }
      else if (state.model_signature.empty() ||
               std::ranges::count(signature, '-') !=
                  std::ranges::count(state.model_signature, '-') ||
               ++state.signature_change_frames >= signature_change_min_frames) {
         log_fmt(Log_level::info, "VR: first-person model {}.", signature);

         state.model_signature = std::move(signature);
         state.steady_parts = {};
         state.signature_change_frames = 0;
      }

      // The parts the hands hold: the model's own (switched in the VR panel), else the
      // defaults.
      const Arm_tuning& tuning = arm_tuning(state.model_signature);
      const int n = static_cast<int>(state.ranked_parts.size());

      const std::uint32_t all_parts = (1u << n) - 1u;

      state.right_hand_parts = tuning.right_parts >= 0
                                  ? static_cast<std::uint32_t>(tuning.right_parts) & all_parts
                                  : default_right_hand_parts(n);
      state.left_hand_parts = tuning.left_parts >= 0
                                 ? static_cast<std::uint32_t>(tuning.left_parts) & all_parts
                                 : default_left_hand_parts(n);
   }

   if (std::exchange(state.weapon_draws_logged_this_frame, false)) {
      state.weapon_draws_log_pending = false;

      for (const Model_part& part : state.ranked_parts) {
         log_fmt(Log_level::info,
                 "VR: first-person model part {}: origin in view space ({:.3f}, {:.3f}, "
                 "{:.3f}), {} draws, {} elements.",
                 &part - state.ranked_parts.data() + 1, part.origin_view.x,
                 part.origin_view.y, part.origin_view.z, part.draws, part.elements);
      }
   }

   state.model_parts.clear();

   // A soldier with a melee weapon has no first-person model, nor has a turret: still
   // gameplay, not a menu.
   update_melee();

   if (melee::player_has_melee() || melee::player_in_turret()) {
      state.weapon_seen_this_frame = true;
   }

   state.overlay_drawn = false;
   state.overlay_vp_valid = false;

   state.weapon_pass_draws = 0;
   state.frames_without_weapon =
      std::exchange(state.weapon_seen_this_frame, false) ? 0 : state.frames_without_weapon + 1;

   // Menus draw with only the view-space camera at the origin; the unit select screen
   // (a fly-over camera plus a view-space UI) and other screens without the
   // first-person weapon for a while are menus too. The next frame is then drawn as the
   // game made it and shown on the virtual screen, where the right hand points.
   const bool was_menu = state.menu_frame;

   // Without an entity the player controls (the galactic conquest map, the fly-over
   // before spawning, death) it isn't gameplay either: the galaxy map draws its planets
   // with a world camera and passes the weapon pass test (a nearer near plane), which
   // took it for gameplay - planets around the head, its interface head-locked, the
   // sticks driving a soldier.
   state.frames_without_player =
      melee::player_controls_entity() ? 0 : state.frames_without_player + 1;

   // The pause menu and the unit select opened in a match (the world and the weapon
   // still drawn behind them) are menus too.
   const bool view_space = state.main_camera_valid && is_view_space_camera(state.main_camera);
   const bool no_player = state.frames_without_player >= menu_after_frames_without_weapon;
   const bool screen = melee::game_screen_shown();

   // A soldier on foot is gameplay even when the weapon pass isn't recognised for a
   // while (the Death Star reactor room flipped to the virtual screen now and then).
   // Without the weapon pass it's still the unit select fly-over (no entity yet), death,
   // knockdowns and vehicles: those aren't a soldier on foot.
   const bool on_foot = melee::player_soldier_on_foot();
   const bool no_weapon =
      !on_foot && state.frames_without_weapon >= menu_after_frames_without_weapon;

   if (on_foot && state.frames_without_weapon == menu_after_frames_without_weapon &&
       state.weapon_gaps_logged < max_logged_weapon_gaps) {
      state.weapon_gaps_logged += 1;
      // The next frame's cameras show why.
      state.cameras_logged = false;

      log_fmt(Log_level::info,
              "VR: no first-person weapon pass recognised for {} frames while the player "
              "is a soldier on foot: kept in gameplay (world camera hfov {:.1f} deg, depth "
              "0 at {:.3f}).",
              menu_after_frames_without_weapon,
              2.0f * std::atan(state.main_camera.tan_half_fov_x) * 180.0f /
                 std::numbers::pi_v<float>,
              -state.main_camera.depth_offset / state.main_camera.depth_scale);
   }

   // Interface screens and view-space-only frames count once they last.
   state.frames_with_screen =
      state.main_camera_valid && (view_space || screen) ? state.frames_with_screen + 1 : 0;
   const bool screen_lasts =
      state.frames_with_screen >= (was_menu ? 1u : menu_after_frames_with_screen);

   state.menu_frame = state.main_camera_valid && (screen_lasts || no_weapon || no_player);

   if (was_menu != state.menu_frame) {
      log_fmt(Log_level::info,
              "VR: {} the virtual screen (menu; world camera {}, view space {}, no weapon "
              "{}, no player entity {}, interface screen '{}', soldier on foot {}).",
              state.menu_frame ? "switching to" : "leaving", state.main_camera_valid,
              view_space, no_weapon, no_player, melee::game_screen_name(), on_foot);

      // Into a menu: the death camera shows the game's own field of view. Out of one:
      // the profile's keys may have changed.
      if (state.menu_frame) restore_widened_fov();
      else load_bindings();
   }

   state.camera_seen_last_frame = std::exchange(state.camera_seen_this_frame, false);
   state.frame_index += 1;

   poll_events();

   if (!state.session_running) return;

   begin_frame();
}

auto input() noexcept -> Input_state
{
   Input_state result = held_input;

   result.fire = fire_pressed;
   result.secondary_fire = secondary_fire_pressed;

   return result;
}

auto take_mouse_wheel() noexcept -> LONG
{
   return mouse_wheel.exchange(0);
}

int view_count() noexcept
{
   return state.scope_shown && state.scope_column > 0 && stereo_frame() && !state.menu_frame
             ? 3
             : 2;
}

auto eyes_size(const glm::uvec2 target_size) noexcept -> glm::uvec2
{
   // The scope's column is in a target of the stereo target's width.
   const bool column = state.scope_column > 0 &&
                       target_size.x == state.eye_size.x * 2 + state.scope_column;

   return {target_size.x - (column ? state.scope_column : 0), target_size.y};
}

auto view_rect(const int view, const glm::uvec2 target_size) noexcept -> View_rect
{
   const glm::uvec2 eyes = eyes_size(target_size);
   const float eye_width = static_cast<float>(eyes.x) / 2.0f;

   if (view < 2) {
      return {static_cast<float>(view) * eye_width, 0.0f, eye_width,
              static_cast<float>(target_size.y)};
   }

   const float column = static_cast<float>(target_size.x - eyes.x);

   return {static_cast<float>(eyes.x), 0.0f, column,
           std::min(column, static_cast<float>(target_size.y))};
}

auto mirror_center_x(const std::uint32_t width) noexcept -> std::uint32_t
{
   return eyes_size({width, 1}).x / 2;
}

auto take_mouse_delta() noexcept -> std::array<LONG, 2>
{
   return {mouse_dx.exchange(0), mouse_dy.exchange(0)};
}

void begin_scene() noexcept
{
   apply_head_cull();
}

bool take_model_positions_request() noexcept
{
   return std::exchange(state.positions_request, false);
}

void give_model_positions(const std::span<const glm::vec3> positions,
                          const char* const failure) noexcept
{
   State::Gun_tip* const tip = current_gun_tip();

   if (!tip) return;

   if (failure) {
      if (!std::exchange(tip->failed, true)) {
         log_fmt(Log_level::info,
                 "VR: the barrel tip of {} can't be read from its model ({}): shots start "
                 "where the game has them (plus the model's muzzle offset).",
                 state.model_signature, failure);
      }

      return;
   }

   if (state.positions_for_flash) {
      if (positions.empty()) return;

      glm::vec3 sum{};

      for (const glm::vec3& p : positions) {
         sum += state.positions_rotation * p + state.positions_position;
      }

      const glm::vec3 centre = sum / static_cast<float>(positions.size());

      tip->flash_object =
         glm::inverse(state.gun_object_rotation) * (centre - state.gun_object_position);
      tip->flash_known = true;

      log_fmt(Log_level::info, "VR: the muzzle flash of {}: {:.3f} m from the barrel tip.",
              state.model_signature,
              glm::length(state.gun_object_rotation * (tip->tip_object - tip->flash_object)));

      return;
   }

   // Into the weapon camera's view space, gathered for finish_gun_tip.
   const Game_camera& camera = state.tip_capture_camera;
   const glm::mat3 to_view = glm::transpose(camera_rotation(camera));

   for (const glm::vec3& p : positions) {
      state.tip_capture_view.push_back(
         to_view * (state.positions_rotation * p + state.positions_position - camera.position));
   }
}

namespace {

// The hand holding the gun as this frame's weapon pass placed it (see fire_from_gun).
auto gun_hand() noexcept -> const World_pose&
{
   return state.fire_map_left ? state.cached_left_hand : state.cached_right_hand;
}

// The game's shot moved to the gun as drawn in the hand (its barrel tip, along the
// barrel as the game's camera had it), from this frame's model.
bool fire_from_gun(glm::vec3& position, glm::vec3& direction) noexcept
{
   if (!settings().fire_from_muzzle || !state.fire_map_valid || state.menu_frame ||
       state.frame_index - state.fire_map_frame > 2) {
      return false;
   }

   // Not the first-person gun's point (third person, a vehicle's).
   if (glm::distance(position, state.fire_map_camera) > fire_point_max_distance) return false;

   // The gun as drawn: steadied (weapon sway off), else moved rigidly.
   const bool steady = !settings().weapon_sway && state.steady_gun_valid &&
                       state.frame_index - state.steady_gun_frame <= 2;
   const glm::mat3& r = steady ? state.steady_gun_r : state.fire_map_r;
   const glm::vec3& t = steady ? state.steady_gun_t : state.fire_map_t;

   // From the barrel tip when the model's geometry gave one (else the game's point).
   if (const std::optional<glm::vec3> tip = gun_tip_world()) position = *tip;

   position = r * (position + state.fire_map_camera_rotation * state.fire_map_muzzle_offset) + t;
   direction = glm::normalize(r * state.fire_map_camera_rotation * glm::vec3{0.0f, 0.0f, -1.0f}) *
               glm::length(direction);

   return true;
}

}

bool fire_from_hand(glm::vec3& position, glm::vec3& direction) noexcept
{
   if (!fire_from_gun(position, direction)) return false;

   // Steady crosshair: along its line, held in the hand (see State::crosshair_calm).
   if (settings().crosshair_steady && state.crosshair_line_valid &&
       state.crosshair_line_model == state.model_signature) {
      const World_pose& hand = gun_hand();

      position = hand.position + hand.rotation * state.crosshair_line_origin;
      direction = glm::normalize(hand.rotation * state.crosshair_line_direction) *
                  glm::length(direction);
   }

   return true;
}

bool flash_from_hand(glm::vec3& position) noexcept
{
   if (!settings().fire_from_muzzle || !state.fire_map_valid || state.menu_frame ||
       state.frame_index - state.fire_map_frame > 2) {
      return false;
   }

   if (glm::distance(position, state.fire_map_camera) > fire_point_max_distance) return false;

   if (const std::optional<glm::vec3> tip = gun_tip_world()) position = *tip;

   position += state.fire_map_camera_rotation * state.fire_map_muzzle_offset;
   state.flash_origin = position;
   state.flash_origin_frame = state.frame_index;

   return true;
}

auto screen_pointer() noexcept -> std::optional<glm::vec2>
{
   state.stat_cursor_queries += 1;

   if (!state.pointer_valid) return std::nullopt;

   return state.pointer_uv;
}

bool drop_screen_blur() noexcept
{
   if (!stereo_frame() || state.menu_frame || settings().screen_blur) return false;

   if (!std::exchange(state.screen_blur_logged, true)) {
      log(Log_level::info, "VR: the game's screen blur left out (settings: screen_blur).");
   }

   return true;
}

bool near_draws_unclipped() noexcept
{
   return stereo_frame() && !state.menu_frame &&
          (melee::player_has_melee() || melee::player_in_turret());
}

// The shot line (settings show_shot_line): where the next shot starts and goes, as
// fire_from_hand would move it.
auto shot_line() noexcept -> std::optional<std::pair<glm::vec3, glm::vec3>>
{
   if (!settings().show_shot_line) return std::nullopt;

   glm::vec3 position{};
   glm::vec3 direction{};

   if (!melee::player_gun_fire_point(position, direction)) return std::nullopt;

   fire_from_hand(position, direction);

   return std::pair{position, glm::normalize(direction)};
}

// The ghost arrow (settings draw_hand_arrow): while the saber's rotation is held, where
// the hand points (the way the blade would go without the turn).
bool hand_arrow_shown(const int hand) noexcept
{
   const bool valid = hand == 0 ? state.grip_valid || state.aim_valid
                                : state.left_grip_valid || state.left_aim_valid;

   return state.saber_turns[hand].held_still && settings().draw_hand_arrow &&
          melee::player_has_melee() && valid;
}

bool hand_arrow_shown() noexcept
{
   return hand_arrow_shown(0) || hand_arrow_shown(1);
}

bool overlay_due(const std::array<glm::vec4, 4>& game_view_projection) noexcept
{
   if (state.overlay_drawn || !stereo_frame() || state.menu_frame || !state.recentered ||
       !state.main_camera_valid || (!melee::blade() && !shot_line() && !hand_arrow_shown())) {
      return false;
   }

   // Draws come in long runs with the same camera: check each camera once.
   if (state.overlay_vp_valid && std::memcmp(&state.overlay_vp, &game_view_projection,
                                             sizeof(game_view_projection)) == 0) {
      return false;
   }

   state.overlay_vp = game_view_projection;
   state.overlay_vp_valid = true;

   Game_camera camera;

   if (decompose_camera(game_view_projection, camera) != Reject::none) return false;

   if (!is_view_space_camera(camera) || !state.frame_world_camera_valid ||
       glm::length(state.frame_world_camera.position) <= 1.0f) {
      return false;
   }

   state.overlay_drawn = true;

   return true;
}

void draw_overlay(ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                  const glm::uvec2 target_size) noexcept
{
   const std::optional<melee::Blade> blade = melee::blade();
   const auto line = blade ? std::nullopt : shot_line();
   const bool arrow = hand_arrow_shown();

   if ((!blade && !line && !arrow) || !state.device) return;

   const Game_camera& world = state.frame_world_camera;

   // The saber was placed at the last frame's camera; the camera has moved with the
   // soldier since.
   const glm::vec3 shift = world.position - state.melee_anchor;

   update_eyes(world);

   // Close to the eyes: a near plane like the weapon's in the hand.
   const Game_camera near_camera = with_near_plane(world, in_hand_near_plane);
   std::array<std::array<glm::vec4, 4>, 2> view_projection{};
   std::array<glm::vec3, 2> positions{};

   for (int i = 0; i < 2; ++i) {
      build_eye_view_projection(state.eyes[i], near_camera, view_projection[i]);
      positions[i] = state.eyes[i].position;
   }

   for (int i = 0; i < 2; ++i) {
      if (!hand_arrow_shown(i)) continue;

      const World_pose hand = aim_pose_world(world, saber_hand_pose(i));
      const glm::vec3 dir = melee::blade_direction(hand.rotation);
      const float length = hand_arrow_length * state.tracking_scale;
      const glm::vec3 tip = hand.position + dir * length;
      const glm::vec3 side = glm::normalize(glm::cross(dir, hand.rotation[0])) * (length * 0.12f);
      const glm::vec3 back = -dir * (length * 0.22f);
      const auto ghost = [](const glm::vec3 a, const glm::vec3 b) {
         return Saber_draw{.a = a,
                           .b = b,
                           .radius = 0.008f,
                           .color = {0.55f, 0.75f, 1.0f},
                           .intensity = 0.45f,
                           .core = 0.3f};
      };
      const std::array<Saber_draw, 3> draws{ghost(hand.position, tip),
                                            ghost(tip, tip + back + side),
                                            ghost(tip, tip + back - side)};

      draw_sabers(*state.device, dc, rtv, eyes_size(target_size), view_projection, positions,
                  std::span{draws.data(), draws.size()});
   }

   if (!blade && !line) return;

   if (line) {
      const Saber_draw draw{.a = line->first,
                            .b = line->first + line->second * shot_line_length,
                            .radius = 0.004f,
                            .color = {1.0f, 0.1f, 0.05f},
                            .intensity = 1.0f,
                            .core = 0.0f};

      draw_sabers(*state.device, dc, rtv, eyes_size(target_size), view_projection, positions,
                  std::span{&draw, 1u});

      return;
   }

   const std::array<Saber_draw, 2> draws{
      Saber_draw{.a = blade->hilt_bottom + shift,
                 .b = blade->hilt_top + shift,
                 .radius = 0.018f,
                 .color = {0.55f, 0.56f, 0.6f},
                 .intensity = 1.0f,
                 .core = 0.0f},
      Saber_draw{.a = blade->hilt_top + shift,
                 .b = blade->tip + shift,
                 .radius = blade->radius * 4.0f,
                 .color = blade->color,
                 .intensity = blade->glow,
                 .core = 0.25f}};

   draw_sabers(*state.device, dc, rtv, eyes_size(target_size), view_projection, positions,
               std::span{draws.data(), blade->glow > 0.01f ? 2u : 1u});
}

void draw_post(ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
               const glm::uvec2 target_size) noexcept
{
   if (!state.device || !stereo_frame() || state.menu_frame || !state.camera_seen_this_frame) {
      return;
   }

   if (state.vignette > 0.01f) {
      std::array<glm::vec4, 2> tangents;

      for (int i = 0; i < 2; ++i) {
         const XrFovf& fov = state.views[i].fov;

         tangents[i] = {std::tan(fov.angleLeft), std::tan(fov.angleRight),
                        std::tan(fov.angleUp), std::tan(fov.angleDown)};
      }

      draw_vignette(*state.device, dc, rtv, eyes_size(target_size), tangents,
                    vignette_inner_degrees, vignette_outer_degrees, state.vignette);
   }

   if (view_count() == 3) {
      const View_rect rect = view_rect(2, target_size);

      draw_scope_mask(*state.device, dc, rtv, {rect.x, rect.y, rect.width, rect.height},
                      game_texture_view(melee::player_scope_texture()));
   }
}

bool hud_to_layer(const std::array<glm::vec4, 4>& game_view_projection) noexcept
{
   if (!stereo_frame() || state.menu_frame || !state.recentered ||
       !state.frame_world_camera_valid ||
       glm::length(state.frame_world_camera.position) <= 1.0f) {
      return false;
   }

   // The HUD's camera: at the origin of the game camera's view space (see
   // is_view_space_camera).
   Game_camera camera;

   if (decompose_camera(game_view_projection, camera) != Reject::none ||
       !is_view_space_camera(camera)) {
      return false;
   }

   if (!state.hud_layer_vp_valid) {
      state.hud_layer_vp_valid = true;
      state.hud_layer_vp = game_view_projection;
   }
   else if (std::memcmp(&state.hud_layer_vp, &game_view_projection,
                        sizeof(game_view_projection)) != 0 &&
            !std::exchange(state.hud_layer_vp_mismatch_logged, true)) {
      log(Log_level::warning, "VR: HUD draws with more than one projection in a frame; "
                              "the HUD layer maps them all with the first.");
   }

   state.hud_layer_draws += 1;

   return true;
}

// Each eye's world->clip matrix (rows) for drawing close to the eyes.
auto hud_eye_view_projection(const Game_camera& world) noexcept
   -> std::array<std::array<glm::vec4, 4>, 2>
{
   update_eyes(world);

   const Game_camera near_camera = with_near_plane(world, in_hand_near_plane);
   std::array<std::array<glm::vec4, 4>, 2> view_projection{};

   for (int i = 0; i < 2; ++i) {
      build_eye_view_projection(state.eyes[i], near_camera, view_projection[i]);
   }

   return view_projection;
}

// A piece of the HUD (u0 v0 u1 v1 of the screen, from the top left) on a plane: its
// centre at the screen's centre, right / up the half extents of the whole screen.
// `u_scale`: the HUD's width in the layer, which spans its left part.
auto hud_piece(const glm::vec4 piece, const glm::vec3 center, const glm::vec3 right,
               const glm::vec3 up, const float u_scale) noexcept -> Hud_quad
{
   return {.center = center + right * (piece.x + piece.z - 1.0f) + up * (1.0f - piece.y - piece.w),
           .right = right * (piece.z - piece.x),
           .up = up * (piece.w - piece.y),
           .uv = {piece.x * u_scale, piece.y, piece.z * u_scale, piece.w},
           .backing = false,
           .blended = true};
}

void draw_hud(ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
              const glm::uvec2 target_size, ID3D11ShaderResourceView& layer,
              const glm::uvec2 layer_size) noexcept
{
   const bool vp_valid = std::exchange(state.hud_layer_vp_valid, false);
   const std::uint32_t draws = std::exchange(state.hud_layer_draws, 0);

   if (!state.device || !vp_valid || !state.frame_world_camera_valid || !state.views_valid) {
      return;
   }

   if (!std::exchange(state.hud_layer_logged, true)) {
      log_fmt(Log_level::info, "VR: the HUD goes to the HUD layer ({} draws in the first "
                               "frame), in front of the eyes, the crosshair along the gun.",
              draws);
   }

   load_hud_config();

   const Settings& s = settings();
   const Game_camera& world = state.frame_world_camera;
   const auto eye_view_projection = hud_eye_view_projection(world);

   // The HUD was drawn over the eyes' part of the layer (see draw_into_vr_hud_layer).
   const glm::uvec2 hud_size = eyes_size(layer_size);
   const float u_scale = static_cast<float>(hud_size.x) / static_cast<float>(layer_size.x);

   // The HUD camera's field of view (the layer holds its whole screen).
   const std::array<glm::vec4, 4>& hud_vp = state.hud_layer_vp;
   const float w_scale = glm::length(glm::vec3{hud_vp[3]});
   const glm::vec2 hud_tan =
      w_scale > 1e-6f ? glm::vec2{w_scale / std::max(glm::length(glm::vec3{hud_vp[0]}), 1e-6f),
                                  w_scale / std::max(glm::length(glm::vec3{hud_vp[1]}), 1e-6f)}
                      : glm::vec2{0.577f, 0.325f};

   std::vector<Hud_quad> quads;
   float right_panel_half_height = 0.0f;

   // The wrist panels are as big as the body is (body scale: a small hero's hands are
   // closer together, a full-size panel on them looked huge), scaled about the hand.
   const auto body_scaled = [&](const std::size_t first, const glm::vec3 hand) {
      const float scale = state.tracking_scale;

      if (std::abs(scale - 1.0f) < 0.01f) return;

      for (std::size_t i = first; i < quads.size(); ++i) {
         quads[i].center = hand + (quads[i].center - hand) * scale;
         quads[i].right *= scale;
         quads[i].up *= scale;
      }
   };

   if (s.wrist_hud) {
      if (state.left_aim_valid) {
         const World_pose hand = aim_pose_world(world, state.left_aim_pose);
         const std::size_t first = quads.size();

         add_hud_panel(left_up, hand, hud_size, u_scale, quads);
         body_scaled(first, hand.position);
      }

      if (state.aim_valid) {
         const World_pose hand = aim_pose_world(world);
         const std::size_t first = quads.size();

         right_panel_half_height = add_hud_panel(right_up, hand, hud_size, u_scale, quads);
         body_scaled(first, hand.position);
      }
   }

   // The crosshair: on the shots' line (from the gun's muzzle along its barrel, as
   // fire_from_hand moves them; a turret's from its barrel), settings crosshair_distance
   // from where they start, facing the eyes; the same size to the eye at any distance.
   // Melee weapons have none.
   const glm::vec3 head = head_position_world(world);
   const glm::vec3 head_right = state.eyes[0].right;
   const glm::vec3 head_up = state.eyes[0].up;
   const glm::vec3 head_forward = state.eyes[0].forward;
   const glm::vec4 cross = hud_config.regions[crosshair];
   const bool turret = melee::player_in_turret();
   const bool crosshair_moved = !melee::player_has_melee() && (turret || state.aim_valid);

   if (crosshair_moved) {
      glm::vec3 center;
      glm::vec3 right = head_right;
      glm::vec3 up = head_up;

      // The game camera's aim when the shots' line can't be read.
      glm::vec3 shot_origin = world.position;
      glm::vec3 shot_direction = glm::normalize(aim_camera(world).forward);
      glm::vec3 position{};
      glm::vec3 direction{};

      if (turret ? melee::turret_fire_line(position, direction)
                 : melee::player_gun_fire_point(position, direction)) {
         const bool from_gun = !turret && fire_from_gun(position, direction);

         shot_origin = position;
         shot_direction = glm::normalize(direction);

         // Steady: the line kept in the gun hand's frame from when the gun was last calm,
         // so the model's recoil, reload, steps and the game camera catching up with the
         // gun (a snap turn) don't shake it. The shots take the same line.
         if (from_gun && s.crosshair_steady && state.aim_valid) {
            const World_pose& hand = gun_hand();
            const glm::mat3 to_hand = glm::inverse(hand.rotation); // scaled with the body
            const bool calm = !melee::player_shot_within(crosshair_recoil_seconds) &&
                              !melee::player_reload() && !state.stick_moving &&
                              state.camera_aim_error < crosshair_calm_camera_radians;

            state.crosshair_calm = calm ? state.crosshair_calm + 1 : 0;

            if (state.crosshair_line_model != state.model_signature) {
               state.crosshair_line_model = state.model_signature;
               state.crosshair_line_valid = false;
            }

            if (state.crosshair_calm >= crosshair_calm_frames || !state.crosshair_line_valid) {
               state.crosshair_line_origin = to_hand * (shot_origin - hand.position);
               state.crosshair_line_direction = to_hand * shot_direction;
               state.crosshair_line_valid = true;
            }
            else if (state.crosshair_line_valid) {
               shot_origin = hand.position + hand.rotation * state.crosshair_line_origin;
               shot_direction = glm::normalize(hand.rotation * state.crosshair_line_direction);
            }
         }
      }

      center = shot_origin + shot_direction * s.crosshair_distance;

      // Never behind or right at the eyes.
      if (glm::dot(center - head, head_forward) < 0.3f) {
         center = head + glm::normalize(center - head + head_forward * 0.3f) * 0.3f;
      }

      {
         // Kept upright.
         const glm::vec3 to_eyes = glm::normalize(head - center);
         const glm::vec3 upright = world_up_axis - glm::dot(world_up_axis, to_eyes) * to_eyes;

         if (glm::length(upright) > 1e-3f) {
            up = glm::normalize(upright);
            right = glm::normalize(glm::cross(up, to_eyes));

            if (glm::dot(right, head_right) < 0.0f) right = -right;
         }
      }

      const float distance = glm::distance(head, center);
      const glm::vec3 screen_center =
         center - right * (cross.x + cross.z - 1.0f) * hud_tan.x * distance -
         up * (1.0f - cross.y - cross.w) * hud_tan.y * distance;

      if (s.crosshair_shown) {
         quads.push_back(hud_piece(cross, screen_center, right * hud_tan.x * distance,
                                   up * hud_tan.y * distance, u_scale));
      }

      if (s.reload_bar_crosshair) {
         add_reload_bar(center - up * (0.075f * distance), right, up, 0.12f * distance, quads);
      }
   }

   // The quick reload bar on the right wrist (the gun's): under the wrist HUD's panel
   // (the map), or in its place.
   if (s.reload_bar_wrist && state.aim_valid) {
      const Hud_panel_frame frame = hud_panel_frame(right_up, aim_pose_world(world));
      const float width = hud_config.panels[right_up].width;
      const float below = right_panel_half_height > 0.0f
                             ? right_panel_half_height + width * 0.1f
                             : 0.0f;

      const std::size_t first = quads.size();

      add_reload_bar(frame.center - frame.text_up * below, frame.text_right, frame.text_up,
                     width * 0.9f, quads);
      body_scaled(first, aim_pose_world(world).position);
   }

   // The rest in front of the eyes (if wanted there), around the crosshair's region.
   if (!s.head_hud) {
      draw_hud_quads(*state.device, dc, rtv, eyes_size(target_size), eye_view_projection,
                     layer, quads);

      return;
   }

   const glm::vec3 center = head + head_forward * hud_overlay_distance;
   const glm::vec3 right = head_right * (hud_tan.x * hud_overlay_distance * s.hud_size);
   const glm::vec3 up = head_up * (hud_tan.y * hud_overlay_distance * s.hud_size);

   if (crosshair_moved) {
      quads.push_back(hud_piece({0.0f, 0.0f, 1.0f, cross.y}, center, right, up, u_scale));
      quads.push_back(hud_piece({0.0f, cross.w, 1.0f, 1.0f}, center, right, up, u_scale));
      quads.push_back(hud_piece({0.0f, cross.y, cross.x, cross.w}, center, right, up, u_scale));
      quads.push_back(hud_piece({cross.z, cross.y, 1.0f, cross.w}, center, right, up, u_scale));
   }
   else {
      quads.push_back(hud_piece({0.0f, 0.0f, 1.0f, 1.0f}, center, right, up, u_scale));
   }

   draw_hud_quads(*state.device, dc, rtv, eyes_size(target_size), eye_view_projection,
                  layer, quads);
}

}
