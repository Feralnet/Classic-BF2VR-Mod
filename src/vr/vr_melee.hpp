#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

// Melee weapons (lightsabers, and mod units built on the same WeaponMelee class) held in
// the right hand: the saber is drawn at the controller with its blade always lit, both
// grips held with the blade upright in front of the body block (the game's deflect),
// and swinging it deals the game's melee damage where the blade actually goes.
//
// The game's own melee is driven by combo animations: its attacks come from damage
// samples baked into the combo, and the soldier is forced into third person while a
// melee weapon is held. Here the camera stays first person, no combo attack is started,
// and the blade's samples come from the controller instead (see vr_melee.cpp).
namespace sp::vr::melee {

// Patches the engine (BattlefrontII.exe, 2017 Steam/GOG build). Logs and leaves the game
// alone on any other exe.
void install_hooks() noexcept;

// The right hand in the game world, OpenXR axes (columns right, up, back).
struct Hand {
   glm::vec3 position;
   glm::mat3 rotation;
};

// The gun in the right hand (not with a melee weapon), for the weapon bash: its front
// and barrel direction in the game world, and its front relative to the head in the
// tracking space (metres; the gesture's speed, untouched by turning and walking).
struct Gun {
   glm::vec3 front;
   glm::vec3 forward;
   glm::vec3 motion;
};

struct Frame_input {
   std::optional<Hand> hand;
   std::optional<Hand> left_hand; // a second saber's (dual sabers)
   std::optional<Gun> gun;
   glm::vec3 head_position{};
   glm::vec3 head_forward{}; // horizontal
   bool block = false;          // both grips are held
   bool secondary_fire = false; // diagnostics (saber throw)
   // Soldiers whose origin is near this aren't drawn (the player's own on a turret,
   // around the eyes).
   std::optional<glm::vec3> hide_soldiers_near;
   float hide_soldiers_radius = 0.0f; // metres
   float scale = 1.0f; // the body's (vr_system's tracking scale): the saber's size
   float dt = 0.0f;          // seconds since the last update
};

// Once per game frame (at Present): reads the player's soldier, updates the blade.
void update(const Frame_input& input) noexcept;

// What happened since the last call: objects the saber hit, bolts it deflected, and the
// tip's speed (m/s) if a swing started (0 otherwise).
struct Events {
   int hits = 0;
   int deflects = 0;
   float swing_speed = 0.0f;
   int bashes = 0;        // weapon bashes swung
   int bash_hits = 0;     // ... that hit a soldier
   int bash_blocked = 0;  // ... that a saber blocked
   int left_hits = 0;     // of `hits`, by a saber in the left hand (dual sabers)
   bool swing_left = false; // the swing was started by the left hand's saber
};

auto take_events() noexcept -> Events;

// The player's soldier isn't under the player's control: knocked down (thrown), or held
// (lightning, choke). As of the last update, any weapon. The saber isn't drawn then.
enum class Control_loss { none, thrown, held };

auto player_control_loss() noexcept -> Control_loss;

// The local player's soldier holds a melee weapon (as of the last update).
bool player_has_melee() noexcept;

// Where the player's gun (primary weapon, not melee) shoots from now and in which
// direction, as the game has it (its first-person gun, in front of the game camera).
// Read now; false without such a gun.
bool player_gun_fire_point(glm::vec3& position, glm::vec3& direction) noexcept;

// The player's gun shot within the last `seconds` (through the fire hooks).
bool player_shot_within(float seconds) noexcept;

// The local player mans a stationary turret (a building's; as of the last update).
bool player_in_turret() noexcept;

// The local player controls a droideka (EntityDroideka; as of the last update).
bool player_on_droideka() noexcept;

// The game's control mode for the local player, read now: which of the profile's control
// sets its keys come from (0 soldier, 1 vehicle, 2 starfighter, 3 jedi, 4 turret); -1
// unknown.
int player_control_mode() noexcept;

// On that turret, where the player's own soldier entity stands (its draws are left out
// within player_body_radius of it); none when its entity isn't known.
auto player_body_on_turret() noexcept -> std::optional<glm::vec3>;

constexpr float player_body_radius = 0.75f;

// Where the player's turret's (first) weapon shoots from and in which direction, as its
// aimer has it. Read now; false off a turret.
bool turret_fire_line(glm::vec3& position, glm::vec3& direction) noexcept;

// The player's secondary weapon is a grenade (as of the last update).
bool player_secondary_is_grenade() noexcept;

// The game's scope overlay texture (an IDirect3DTexture9 of the d3d9 layer) for the
// player's primary weapon (its class's ScopeTexture, else the game's default one), or
// null when it isn't loaded. Read now.
auto player_scope_texture() noexcept -> void*;

// The player's soldier reloads the weapon in hand (primary or secondary): the reload's
// seconds so far and its whole length (the weapon's ReloadTime). Read now.
struct Reload {
   const void* weapon = nullptr; // tells one weapon's reload from another's
   float elapsed = 0.0f;
   float length = 0.0f;
};

auto player_reload() noexcept -> std::optional<Reload>;

// Ends that weapon's reload now if it's still reloading in the player's hand (the game
// refills the clip at its next update). True if it did.
bool finish_player_reload(const void* weapon) noexcept;

// The weapon in the player's hand overheats (its class has HeatPerShot) and isn't fully
// cool: gives it `seconds` more of the game's own cooling (HeatRecoverRate x seconds),
// ending an overheat the way the game does. True if the weapon took it.
bool cool_player_weapon(float seconds) noexcept;

// While the game winds up a throw of the player's grenade (fire held), skips the rest of
// the wind-up: the grenade leaves at the game's next update. True while winding up.
bool hurry_grenade_throw() noexcept;

// The local player controls something that's neither a soldier nor a building's
// turret: a vehicle (as of the last update).
bool player_in_vehicle() noexcept;

// One of the game's interface screens is up over the match (pause menu, unit select;
// as of the last update).
bool game_screen_shown() noexcept;

// That screen's name, empty without one (for diagnostics).
auto game_screen_name() noexcept -> const char*;

// The local player controls an entity: a soldier, vehicle or turret (as of the last
// update). Not on the galaxy map, in menus, or while dead. True with an unknown exe.
bool player_controls_entity() noexcept;

// The local player controls a soldier in an ordinary state: not knocked down, thrown,
// held, stunned or dead (soldier states 9 and up), as of the last update.
bool player_soldier_on_foot() noexcept;

// The local player's soldier read now (not as of the last update): its world position
// (the entity's origin, at the feet) and state (EntitySoldier +0x744). None while the
// player controls anything else, or with an unknown exe.
struct Player_soldier {
   glm::vec3 position;
   int state;
};

auto player_soldier() noexcept -> std::optional<Player_soldier>;

// How high the player's head bone (bone_head) is over the soldier's feet, from the body's
// render this frame or the last: only while a melee weapon is held (its render gets the
// body's skeleton).
auto player_head_height() noexcept -> std::optional<float>;

// The player's saber is thrown (away from the hand, as of the last update).
bool player_saber_thrown() noexcept;

// The galactic conquest screens (the shell's ifs_freeform_*: the galaxy map, the fleet)
// are up (as of the last update).
bool galactic_conquest_shown() noexcept;

// The player's saber blocks (as of the last update).
bool player_blocking() noexcept;

// The player's saber is a pair (dual sabers, one in each hand; as of the last update).
bool player_dual_sabers() noexcept;

// Where a saber's blade points from a hand of this rotation (OpenXR axes, any space): along
// its aim, tilted up by the tuning's pitch.
auto blade_direction(const glm::mat3& hand_rotation) noexcept -> glm::vec3;

// The game draws the player's saber (hilt and blade) in the hand: the first-person
// model (the same hilt, at the camera) is hidden, and blade() gives nothing.
bool game_saber_in_hand() noexcept;

// Where the game's saber was put in the hand this frame or the last (the hilt's origin,
// world): its draws are found by it however far the hand is from the game's camera.
auto player_saber_hilt() noexcept -> std::optional<glm::vec3>;


// The same for dual sabers' second hilt, in the left hand.
auto player_left_saber_hilt() noexcept -> std::optional<glm::vec3>;

struct Blade {
   glm::vec3 hilt_bottom;
   glm::vec3 hilt_top; // the blade starts here
   glm::vec3 tip;      // equals hilt_top while extinguished
   glm::vec3 color;
   float radius;       // the blade's core radius (metres)
   float glow;         // 0..1, ignition
};

// The saber to draw this frame when the game's can't be put in the hand, or nullopt.
auto blade() noexcept -> std::optional<Blade>;

// Tuning from bf2vr_saber.txt (see vr_melee.cpp).
void load_tuning(const wchar_t* path) noexcept;

// Logged with the other VR stats.
void log_stats() noexcept;

}
