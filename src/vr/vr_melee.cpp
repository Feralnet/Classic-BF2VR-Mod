
#include "vr_melee.hpp"
#include "vr_settings.hpp"
#include "vr_system.hpp"
#include "../logger.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>

#include <Windows.h>
#include <intrin.h>

// How the game does melee, and what's changed (BattlefrontII.exe, 2017 Steam/GOG build;
// RE notes in the bf2vr README):
//
// - First person: the camera tracker asks the controlled entity whether it must be seen
//   in third person (trackable interface, slot 15). A soldier answers yes while either
//   of its current weapons is melee (FUN_004de390 calls the weapon's slot 21, which only
//   WeaponMelee answers with 1). In VR it answers no.
//
// - The saber in the hand: in first person the soldier's first-person renderable
//   (FUN_00520320) draws the weapon with WeaponMelee's render (slot 35, FUN_0068a050:
//   thiscall, matrix, skeleton, 3 more arguments, ret 0x14): the hilt at the hardpoint
//   (the skeleton's hand bone times the matrix), each blade edge from there, its length
//   x the weapon's own extension (+0x179 bit 1 lit, +0x1a0 seconds since it changed,
//   +0x1a4 the extend time), and the trail. For the player's weapon the call gets the
//   controller's hilt matrix and no skeleton. The weapon's lit bit is kept set (the game
//   extends the blade and plays its sound when the saber is taken up); while the saber
//   is thrown the game's own handling is left alone.
//
//   In third person the soldier's renderer (FUN_0048dc90) draws a melee weapon
//   with WeaponMeleeClass's render (FUN_0068e8f0, called at 0x48e346: thiscall, 5 stack
//   arguments, the blade length factor in XMM1). It places the hilt model at the weapon
//   hardpoint (the hand bone of the skeleton passed in, times the soldier's matrix) and
//   each blade edge (+0x2c8, 0x34 bytes: +0 hardpoint, +8 position, +0x14 direction in
//   the hilt's space, +0x20 length, +0x24 width, +0x30 colour) from there. For the
//   player's soldier the call gets the controller's hilt matrix instead (no skeleton) and
//   the ignition as the length factor: the game's own hilt and blade end up in the hand.
//   The first-person model (the same hilt, drawn at the camera) is then hidden.
//
// - WeaponMelee runs a combo state machine (+0x114 the combo: +0x10 its states, 0x34
//   bytes each, +0x21 their count; +0x180 the current state, +0x188 the time in it).
//   A state's attacks (+0x10, count in the top 4 bits of +0x1c, 16 bytes each) carry
//   damage samples: the blade's position and direction in the soldier's space at times
//   into the state, baked from the animation. The weapon's fire update (slot 41,
//   FUN_0068c230; the base weapon update only calls it while the trigger is held) takes
//   the samples of the time passed since the last frame, finds the objects near them and
//   ray-tests each sampled blade against their collision; a hit deals the attack's damage
//   and push (once per object and attack: hit records at +0x1a8, cleared by
//   FUN_0068bc00).
//
//   Here no combo attack is started (the trigger doesn't fire for melee). Instead, while
//   the lit blade is swung, the player's weapon update (slot 1, every frame) runs the
//   fire update with a stand-in state: one attack whose samples are the controller's
//   blade between the last update and this one, with the weapon's own strongest attack's
//   damage and push. Everything after that (collision, damage, push, effects) is the
//   game's.
//
// - Block stance: while blocking, the player's combo is put in its deflecting state
//   (FUN_0068bef0 leaves the current state, FUN_0068bc30 enters one; the update keeps it
//   there) and back to the idle state (the combo's +0x18) afterwards: the body plays the
//   block animation.
//
// - Saber throw: the soldier's WeaponMeleeThrow (pointer at the saber's +0x110; its +0x114
//   bit 1 set while the saber is thrown, bit 2 while it's away otherwise) spawns an
//   OrdnanceMeleeThrow, whose render (FUN_006034e0) draws the saber in flight with the
//   saber's slot 35 render: its own matrix, no skeleton. The hand's render (with a
//   skeleton) then leaves the hilt and first blade out. Only renders with a skeleton go
//   to the controller, and none while the saber is away. Both feed the weapon's blade
//   trails, so the player's saber is drawn without them (flags bit 0x100).
//
// - The fire update hits each object once per attack: a long swing (or one that turns
//   back without slowing down) would hit a target only once. A swing that turns back
//   starts a new one, and a target can be hit again after rehit_seconds.
//
// - Knockdown and stun: the soldier's state (EntitySoldier +0x744, set by FUN_004ee2c0;
//   the trackable interface is at +0x258) is 9 while thrown (a push over the threshold,
//   FUN_004e1690) and 14 while held (a push of type 2 - lightning, choke - from
//   FUN_0067d930 through FUN_004e20a0). Neither shows in first person: the saber leaves
//   the hand (isn't drawn, doesn't swing or block) until the soldier is back.
//
// - A bolt hitting a soldier deflects if its melee weapon says it's deflecting (slot 20,
//   FUN_00688b50: the current combo state has a deflect, i.e. DEFLECT) and the deflect
//   itself (slot 18, FUN_0068a550: cone test, bolt sent back) succeeds. Here the player's
//   weapon is deflecting while both grips are held with the blade upright in front of
//   the body, and the deflect runs with the combo's deflecting state made current for
//   its duration.
//
//   Auto block (settings): without the grips, a bolt is deflected the same way when it
//   hits near the blade (the deflect's point: where the bolt is) or, hitting from
//   further away, comes from the side the blade is on (seen from the head). The game's
//   deflecting query then answers yes while the lit saber is in the hand.
//
// - Death (soldier states 15 and up): the saber leaves the hand like a knockdown, until
//   the soldier is gone.

namespace sp::vr::melee {

namespace {

constexpr std::uintptr_t exe_image_base = 0x400000;

// Pointer to the per-player camera trackers (0x18 bytes each, +0x10 the controlled
// entity's trackable interface); player 0 is the local player.
constexpr std::uintptr_t exe_trackers = 0x007e9ee0;

// The local player's controls (FUN_0052a190: thiscall on 0x1ebdc50, player 0 = +0x428):
// +8 the control mode the player controller sets each frame (FUN_0061a2b0, 0x61a4ac),
// i.e. which of the profile's control sets the game reads its keys from: 0 soldier,
// 1 vehicle, 2 starfighter, 3 jedi, 4 turret.
constexpr std::uintptr_t exe_control_mode = 0x01ebe080;

// EntitySoldier's trackable interface: its vtable, and slot 15 "must be seen in third
// person" (FUN_004de390). Its current weapons: pointers at +0x4c8, indexed by the two
// signed bytes at +0x4e8 (primary and secondary channel, -1 = none).
constexpr std::uintptr_t exe_soldier_trackable_vtable = 0x0079d028;
// A droideka (EntityDroideka, RTTI): rolls up on the crouch key; rolled, it has no gun.
constexpr std::uintptr_t exe_droideka_trackable_vtable = 0x0079ae6c;
constexpr int force_third_person_slot = 15;
constexpr std::uintptr_t exe_force_third_person = 0x004de390;
constexpr std::ptrdiff_t trackable_weapons = 0x4c8;
constexpr std::ptrdiff_t trackable_weapon_channels = 0x4e8;
constexpr std::ptrdiff_t trackable_soldier_state = 0x4ec; // EntitySoldier +0x744

// Before asking slot 15, the camera's choice (FUN_00658830: thiscall(trackable, player),
// ret 4, 1 = FIRST person: slot 15 true gives 0) and the tracker's current mode
// (FUN_0044e3c0: fastcall tracker; +0x10 the trackable, +0x14 the mode it was set to,
// 1 = first person) both take a camera mode from the controlled entity's class
// (+0x14c: 1 third person only, 2 first person only).
// Mods' characters without first-person models (small ones: Mission Vao, a youngling
// wookiee) are third person only. In VR the player's soldier is first person anyway:
// both are entered through detours over their first whole instructions.
constexpr std::uintptr_t exe_camera_third_person = 0x00658830;
constexpr std::uint8_t camera_third_person_prologue[6] = {0x55, 0x8B, 0xEC,  // push ebp; mov
                                                          0x56, 0x8B, 0xF1}; // push esi; mov
constexpr std::uintptr_t exe_tracker_third_person = 0x0044e3c0;
constexpr std::uint8_t tracker_third_person_prologue[7] = {0x56, 0x57, 0x8B, 0xF9, // push x2; mov
                                                           0x8B, 0x77, 0x10};      // mov esi
constexpr std::ptrdiff_t tracker_trackable = 0x10;

// Stationary turrets: the controlled trackable is a MountedTurret's (vtable at the
// turret's +0x24; the turret's host entity at its +0x258, i.e. the trackable's +0x234),
// on a building (main vtables of the classes' "Entity..." name getters, slot 2).
constexpr std::uintptr_t exe_mounted_turret_trackable_vtable = 0x007aa5b8;
constexpr std::ptrdiff_t turret_trackable_host = 0x234;
// The turret's weapons and their aimers (its ctor FUN_005a5370: aimer FUN_0043cfa0 at
// turret +0x220 + 4i, the weapon its class makes at +0x230 + 4i, i < the class's +0x640;
// the class at turret +0x25c), as offsets from the trackable (turret +0x24).
constexpr std::ptrdiff_t turret_trackable_aimers = 0x220 - 0x24;
constexpr std::ptrdiff_t turret_trackable_weapons = 0x230 - 0x24;
constexpr std::uintptr_t exe_building_vtables[] = {
   0x00799d44, // EntityBuildingArmed
   0x00799ed4, // EntityBuildingArmedDynamic
   0x0079999c, // EntityBuildingAnimatedArmed
};

// WeaponGrenade's vtable (its class name getter is slot 7, like WeaponMelee's).
constexpr std::uintptr_t exe_grenade_vtable = 0x007b0ef4;

// A grenade's throw (FUN_006850d0 starts it, FUN_006851d0 runs it): fire pressed sets
// +0x110 and zeroes the timer +0xb4; the grenade leaves once the timer reaches the
// throw's delay (the class at +0x64: floats from +0x230, indexed by the throw type, signed
// byte +0x112; 0.5 / 0.36 / 0.58 s by default), unless fire was let go before (+0x111
// set: no throw). Then +0x110 clears and +0x111 sets.
constexpr std::ptrdiff_t grenade_throwing = 0x110;
constexpr std::ptrdiff_t grenade_released = 0x111;
constexpr std::ptrdiff_t grenade_throw_type = 0x112;
constexpr std::ptrdiff_t grenade_timer = 0xb4;
constexpr std::ptrdiff_t grenade_class_delays = 0x230;

// Soldiers are drawn (third person: every soldier but the player's own on foot) by
// SoldierElement's render (vtable 0x799064, slot 16, FUN_0048dc90: thiscall, the
// soldier's matrix (D3DX rows, position at floats 12-14) and one more pointer, ret 8).
// The renderer doesn't call it through that vtable slot (a hook there never ran): it's
// entered through a detour over its first six bytes, the same as the Weapon render's.
constexpr std::uintptr_t exe_soldier_render = 0x0048dc90;
constexpr std::uint8_t soldier_render_prologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};

// The game's interface screens (the Lua shell: pause menu, unit select, ...): the shell
// manager (pointer at 0x7e6664), the top of its screen stack at +0x178 (-1: none), and
// the shown screen's name: the first one set of +0xb4, +0x74, +0x34 (FUN_00528da0).
constexpr std::uintptr_t exe_shell = 0x007e6664;
constexpr std::ptrdiff_t shell_stack_top = 0x178;
constexpr std::ptrdiff_t shell_screen_names[] = {0xb4, 0x74, 0x34};

// Every weapon's (class Weapon) state machine, run by its update (FUN_006781b0): the
// state at +0xb0 (4 = reloading), the state's timer at +0xb4 and its length at +0xb8
// (a reload: the class's ReloadTime, class +0x9c). The reload ends once the timer
// reaches the length (slot 58, FUN_006792e0), at the weapon's next update, refilling
// the clip (slot 48). Weapon switches, death and the like leave the state as the game
// does: a weapon put away keeps its reload, paused.
constexpr std::ptrdiff_t weapon_machine_state = 0xb0;
constexpr std::ptrdiff_t weapon_state_length = 0xb8;
constexpr std::int32_t weapon_reloading = 4;

// A gun's heat: a meter at weapon +0x8c (FUN_00491690 gives back, FUN_004916c0 takes):
// +0 its limits (+0 full, +4 lowest, +8 the level it fires again at), +4 the level (1 =
// cool; a shot takes the class's HeatPerShot, class +0x80) and bit 0 of +8: overheated.
// The weapon's update (0x67832a) gives back the class's HeatRecoverRate (+0x84) x dt.
constexpr std::ptrdiff_t weapon_heat = 0x8c;
constexpr std::ptrdiff_t class_heat_per_shot = 0x80;
constexpr std::ptrdiff_t class_heat_recover_rate = 0x84;

// A gun's shot: WeaponCannon's (FUN_0067f320, called from its update at 0x67edf8) and
// the other shooting class's (FUN_00681460, called at 0x6812ca), both fastcall(weapon).
// Each takes the bolt's start from the weapon's aimer (weapon +0x70) at +0x88 and its
// direction from the aimer's +0x48 (also the muzzle flash's); for the player's gun those
// are the game's first-person gun's, in front of the camera.
constexpr std::uintptr_t exe_cannon_fire = 0x0067f320;
constexpr std::uintptr_t exe_cannon_fire_call = 0x0067edf8;
constexpr std::uintptr_t exe_laser_fire = 0x00681460;
constexpr std::uintptr_t exe_laser_fire_call = 0x006812ca;
constexpr std::ptrdiff_t weapon_aimer = 0x70;
constexpr std::ptrdiff_t aimer_fire_direction = 0x48;
constexpr std::ptrdiff_t aimer_fire_position = 0x88;

// A weapon's effects (the charge-up glow and sparks of a gun fired by holding the
// trigger, ...) are placed at its aimer's fire point (+0x88), in front of the game's
// camera at a full-size soldier's height. Three functions take it:
// - FUN_005384c0 starts an effect tied to a position (thiscall(effect handle; effect
//   class, position, other, flag), ret 0x10; the base Weapon's FUN_00678aa0 /
//   FUN_00679070 / FUN_00678c90 for the charge and the like),
// - FUN_005386b0 ties a running one again (thiscall(handle; class, position, other),
//   ret 0xc; WeaponCannon's update FUN_0067e620 every frame, weapon +0x13c/+0x148/+0x154),
// - FUN_00538010 plays one at a position (thiscall(effect class; position, other, ?,
//   flag), ret 0x10; the charge's end in FUN_00678e00).
// The tied ones keep the position's POINTER (effect instance +0x14) and read it while
// they live. For the player's gun they're all given a point that follows the gun in the
// hand instead (kept up to date every frame, see update).
constexpr std::uintptr_t exe_effect_start = 0x005384c0;
constexpr std::uint8_t effect_start_prologue[6] = {0x55, 0x8B, 0xEC,  // push ebp; mov ebp, esp
                                                   0x83, 0xE4, 0xF8}; // and esp, -8
constexpr std::uintptr_t exe_effect_attach = 0x005386b0;
constexpr std::uint8_t effect_attach_prologue[7] = {0x55, 0x8B, 0xEC,  // push ebp; mov ebp, esp
                                                    0x53,              // push ebx
                                                    0x8B, 0x5D, 0x08}; // mov ebx, [ebp + 8]
constexpr std::uintptr_t exe_effect_play = 0x00538010;
constexpr std::uint8_t effect_play_prologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
// An emitter ordnance's shot (OrdnanceEmitterClass; see emitter_fire_hook).
constexpr std::uintptr_t exe_emitter_fire = 0x005fce60;
constexpr std::uint8_t emitter_fire_prologue[6] = {0x55, 0x8B, 0xEC,  // push ebp; mov ebp, esp
                                                   0x83, 0xE4, 0xF0}; // and esp, -16
// Where a weapon keeps its effects' objects (the object FUN_005384c0 is called on, the
// first argument of FUN_005386b0): the base Weapon's three, WeaponCannon's three.
constexpr std::ptrdiff_t effect_objects[] = {0xd8, 0xe0, 0xe8, 0x13c, 0x148, 0x154};

// An animator's animation change (FUN_0072d430: thiscall(animation, blend), ret 8) walks
// the animation's bone table (*animation, 0x38 bytes a bone, count u16 at +10). A
// character whose class is third person only (an Ewok), kept in first person here, has
// no first-person animation for some actions: firing the Ewok's spear passed a pointer
// to no animation and crashed reading its bone table (FUN_0072f3f0). Such an animation
// is left out (the animator keeps its current one).
constexpr std::uintptr_t exe_animation_set = 0x0072d430;
constexpr std::uint8_t animation_set_prologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20};

// Soldier states (see above).
constexpr std::int32_t soldier_thrown = 9;
constexpr std::int32_t soldier_held = 14;
constexpr std::int32_t soldier_dying = 15; // and 16, dead

// WeaponMelee's vtable and the slots hooked: update (1: thiscall, float seconds),
// deflect (18: thiscall, 3 arguments), deflecting (20: thiscall), fire update (41:
// thiscall, float seconds); the hit records' release (fastcall).
constexpr std::uintptr_t exe_melee_vtable = 0x007b1578;
constexpr int update_slot = 1;
constexpr std::uintptr_t exe_update = 0x00689290;
constexpr int deflect_slot = 18;
constexpr std::uintptr_t exe_deflect = 0x0068a550;
constexpr int deflecting_slot = 20;
constexpr std::uintptr_t exe_deflecting = 0x00688b50;
constexpr int update_fire_slot = 41;
constexpr std::uintptr_t exe_update_fire = 0x0068c230;
constexpr std::uintptr_t exe_clear_hits = 0x0068bc00;
constexpr int render_slot = 35;
constexpr std::uintptr_t exe_render = 0x0068a050;
constexpr std::uintptr_t exe_leave_state = 0x0068bef0; // fastcall(weapon)
constexpr std::uintptr_t exe_enter_state = 0x0068bc30; // thiscall(weapon, state)

// The soldier renderer's call of WeaponMeleeClass's render.
constexpr std::uintptr_t exe_class_render = 0x0068e8f0;
constexpr std::uintptr_t exe_class_render_call = 0x0048e346;

// Any character's head bone (player_head_height) comes from a render given its body's
// skeleton: the soldier renderer's call of the other weapons' class render
// (FUN_0067bf00 at 0x48e36e: the melee one's arguments, no XMM1, ret 0x14), and the
// base Weapon render (slot 35 of most weapon vtables, FUN_00679350: thiscall, the body's
// matrix and skeleton, three more, ret 0x14), entered through a detour over its first
// six bytes (push ebp / mov ebp, esp / and esp, -16).
constexpr std::uintptr_t exe_gun_class_render = 0x0067bf00;
constexpr std::uintptr_t exe_gun_class_render_call = 0x0048e36e;
constexpr std::uintptr_t exe_weapon_render = 0x00679350;

// The muzzle flash: the base Weapon render (above) draws it while the weapon's flash
// timer runs, with FUN_0067bd30 (call at 0x6794d4; thiscall on the weapon's class, the
// flash's position - the weapon's +0x50, the first-person model's muzzle point - and
// direction - the aimer's +0x48 - its fade in XMM3, the weapon in EDI, ret 8).
constexpr std::uintptr_t exe_flash_draw = 0x0067bd30;
constexpr std::uintptr_t exe_flash_draw_call = 0x006794d4;
constexpr std::uint8_t weapon_render_prologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};

// Weapon bash (update_bash): the target's EntitySoldier (main object, vtable 0x79d114;
// SoldierElement +0x130 points at it) is hurt the way the saber's fire update and force
// lightning (FUN_0067d930) hurt it:
// - slot 53 (FUN_004e2180, thiscall(0, direction*, point*), ret 0xc): blocked (a saber);
// - a damage record: FUN_00488570's seven fields {owner +0xcc, attacker entity E =
//   (owner +0x18)->slot 8 (), E +0x204, E's team (E +0x234, low 4 bits signed),
//   E->slot 11 (), the weapon's class, 1.0} copied into FUN_00438a40 (thiscall(record,
//   the seven by value), damage in XMM2, ret 0x1c);
// - the target's Damageable (+0x140) slot 5 (record*, direction*, point*, -1, -1);
// - the push: slot 35 (FUN_004e1690, thiscall(direction*, float push, 0), ret 0xc).
constexpr std::uintptr_t exe_soldier_main_vtable = 0x0079d114;
constexpr std::ptrdiff_t element_soldier = 0x130;
constexpr std::ptrdiff_t entity_team = 0x234;
constexpr std::ptrdiff_t entity_id = 0x204;
constexpr std::ptrdiff_t entity_damageable = 0x140;
constexpr std::ptrdiff_t entity_soldier_state = 0x744;
constexpr std::ptrdiff_t owner_damage_source = 0xcc;
constexpr std::uintptr_t exe_damage_record = 0x00438a40;
constexpr int entity_block_slot = 53;
constexpr int entity_push_slot = 35;
constexpr int damageable_damage_slot = 5;

// Targets come from the query the saber's fire update finds objects with (call 0x68cc1e,
// FUN_0045e210: ECX the centre (world), EDX the array of found entities, XMM1 the radius;
// stack: the array's size, an entity left out (the attacker), a category mask (0x3a like
// the saber; 0 = 0x1e); the caller pops them; returns how many). It tests the objects'
// bounds horizontally (FUN_0045ec60). A found entity's slot 12 is its collision (null:
// skipped, like the saber does).
// Soldiers drawn through the hooked render calls turned out to be only the player's own
// body and gun (log: every bash's "nearest body" was the player).
constexpr std::uintptr_t exe_find_objects = 0x0045e210;
constexpr std::uint32_t find_objects_saber_mask = 0x3a;
constexpr int entity_collision_slot = 12;

// WeaponMelee fields.
constexpr std::ptrdiff_t weapon_class = 0x64;
constexpr std::ptrdiff_t weapon_render_class = 0x68; // the class the slot 35 render reads
constexpr std::ptrdiff_t weapon_owner = 0x6c;
constexpr std::ptrdiff_t weapon_timer = 0xb4; // the fire update adds its seconds
constexpr std::ptrdiff_t weapon_combo = 0x114;
constexpr std::ptrdiff_t weapon_active_edges = 0x17c; // bits set by the fire update
constexpr std::ptrdiff_t weapon_state = 0x180;
constexpr std::ptrdiff_t weapon_state_time = 0x188;
constexpr std::ptrdiff_t weapon_hit_records = 0x1a8;
constexpr std::ptrdiff_t weapon_flags = 0x179;     // bit 1: lit
constexpr std::ptrdiff_t weapon_lit_time = 0x1a0;  // seconds since lit changed
constexpr std::ptrdiff_t weapon_thrower = 0x110;   // the WeaponMeleeThrow, or null
constexpr std::ptrdiff_t thrower_flags = 0x114;    // bits 1-2: the saber is away

// WeaponMeleeClass: blade edges and their count (see above).
constexpr std::ptrdiff_t class_edge_count = 0x2c4;
constexpr std::ptrdiff_t class_edges = 0x2c8;
constexpr std::ptrdiff_t edge_size = 0x34;

// WeaponMeleeClass: its hilts (hardpoints; FUN_0068a050 / FUN_0068e8f0): their count, and
// each one's bone (hash in the body's skeleton). Hilt 0's model is at +0x64, hilt i's at
// +0x2c8 + 4i; an edge's +0 is the hilt it's on. The renders draw hilt i at its bone x
// the matrix: without a skeleton only hilt 0 (at the matrix) and its edges, so dual
// sabers' second hilt (on the left hand's bone) wasn't drawn in the hand.
constexpr std::ptrdiff_t class_hardpoint_count = 0x2e8;
constexpr std::ptrdiff_t class_hardpoint_bones = 0x2d8;
constexpr int max_hardpoints = 4;

// Blades handled in the hands (the class's first edges): each swings, hits and blocks
// where it is - a double-bladed saber's second blade, dual sabers' left one.
constexpr int max_blades = 4;

// Combo and combo state fields.
constexpr std::ptrdiff_t combo_states = 0x10;
constexpr std::ptrdiff_t combo_idle_state = 0x18; // pointer to one of the states
constexpr std::ptrdiff_t combo_state_count = 0x21; // byte
constexpr std::ptrdiff_t state_size = 0x34;
constexpr std::ptrdiff_t state_attacks = 0x10;
constexpr std::ptrdiff_t state_deflect = 0x14;
constexpr std::ptrdiff_t state_attack_count = 0x1c; // top 4 bits
constexpr std::ptrdiff_t state_flags = 0x28;

// An attack (16 bytes), as the fire update reads it:
// - dword 0: bits 0-7 sample count, bit 8 "lasts to the state's end", bits 10-19 / 20-29
//   start / end time (x 0.01 s), bits 30-31 zero = push applies.
// - dword 1: bits 0-11 damage, bits 12+ attack id (0 = matched by address only).
// - dword 2: bits 0-3 edge, bit 4 length x edge length, bit 5 width x edge width,
//   bits 6-15 length (x 0.01), bits 16-23 width (x 0.01), bits 24-31 push (x 0.1).
// - dword 3: the samples, 7 floats each: time, position xyz, direction xyz (soldier
//   space). The blade runs from position - 0.2 x direction to position + length x
//   direction.
constexpr std::uint32_t attack_lasts_to_end = 1u << 8;
constexpr std::uint32_t attack_length_from_edge = 1u << 4;
constexpr std::uint32_t attack_width_from_edge = 1u << 5;
constexpr std::uint32_t attack_unit_width = 100u << 16; // 1.00 x the edge's width
constexpr float sample_back = 0.2f;

// Samples of the blade per update while swinging (interpolated between the last
// update's blade and this one's, so fast swings don't pass through targets).
constexpr int samples_per_update = 4;

// Swing: the blade's tip faster than the settings' swing speed for the settings' swing
// length starts one, slower than this fraction of that speed for a while ends it. Each
// swing hits a target once. Measured relative to the head, so walking and the camera's
// bobbing don't count.
constexpr float swing_end_speed_fraction = 0.5f;
constexpr float swing_end_time = 0.12f;

// A swing's tip moving against its last fast direction by more than this (cosine)
// starts a new swing; a target the swing hit can be hit again after rehit_seconds.
constexpr float swing_reverse_cos = -0.2f;
constexpr float rehit_seconds = 0.35f;

// Block (both grips held): the blade within this angle of upright, its middle in front
// of the head.
constexpr float block_max_degrees = 50.0f;
constexpr float block_min_forward = 0.1f; // metres in front of the head
constexpr float block_max_distance = 0.9f;

// Auto block: a bolt this close to the blade (metres) is deflected; one hitting further
// than auto_block_near from the soldier (horizontally) when it comes within this angle
// of the blade's side, seen from the head.
constexpr float auto_block_reach = 0.6f;
constexpr float auto_block_near = 1.5f;
constexpr float auto_block_degrees = 75.0f;

// A melee weapon whose edge has no length (mods' swords, staffs: LightSaberLength 0)
// draws no blade: its model is the blade, and its combo's attacks carry their own
// lengths. Such a weapon can't deflect bolts here. Its edge sits at the model's hp_fire,
// which is often left unrotated (edge along +z) while the model's blade points the way
// the sabre animations hold a hilt (the stock sabres' edges: up 60 deg from +z). The
// swing hits along the model's blade, this long and wide.
constexpr float mesh_blade_min_length = 0.05f;
constexpr glm::vec3 mesh_blade_direction{0.0f, 0.8660254f, 0.5f};
constexpr float mesh_blade_length = 1.0f;
constexpr float mesh_blade_width = 0.15f;

constexpr float ignite_seconds = 0.25f;

// The game's saber counts as drawn in the hand when its render was redirected this
// many frames ago or less (otherwise the fallback blade is drawn).
constexpr std::uint32_t game_saber_frames = 30;

struct Fake_attack {
   std::uint32_t dwords[4];
};

struct State {
   bool hooked = false;
   std::uintptr_t image_base = exe_image_base; // where the exe is loaded

   using Force_third_person_fn = bool(__fastcall*)(void* trackable, void* edx);
   using Camera_third_person_fn = bool(__fastcall*)(void* trackable, void* edx, int player);
   using Tracker_third_person_fn = bool(__fastcall*)(void* tracker);
   using Update_fn = std::uint32_t(__fastcall*)(void* weapon, void* edx, float seconds);
   using Deflect_fn = bool(__fastcall*)(void* weapon, void* edx, void* ordnance,
                                        void* point, void* arg);
   using Deflecting_fn = bool(__fastcall*)(void* weapon, void* edx);
   using Clear_hits_fn = void(__fastcall*)(void* weapon);
   using Render_fn = void(__fastcall*)(void* weapon, void* edx, const float* matrix,
                                       const void* skeleton, std::uint32_t a,
                                       std::uint32_t flags, std::uint32_t draw_hilt);
   using Leave_state_fn = void(__fastcall*)(void* weapon);
   using Soldier_render_fn = void(__fastcall*)(void* element, void* edx, const float* matrix,
                                               const void* arg);
   using Enter_state_fn = void(__fastcall*)(void* weapon, void* edx, int combo_state);

   Force_third_person_fn force_third_person = nullptr;
   Camera_third_person_fn camera_third_person = nullptr;   // detour trampolines
   Tracker_third_person_fn tracker_third_person = nullptr;
   std::uint32_t stat_class_third_person_refused = 0;
   bool class_third_person_logged = false;
   Update_fn update = nullptr;
   Deflect_fn deflect = nullptr;
   Deflecting_fn deflecting = nullptr;
   Update_fn update_fire = nullptr;
   Clear_hits_fn clear_hits = nullptr;
   Render_fn render = nullptr;
   Leave_state_fn leave_state = nullptr;
   Enter_state_fn enter_state = nullptr;
   bool render_hooked = false;
   int render_calls_logged = 0;

   // Shots of the player's gun moved to the gun in the hand (fire_hook).
   using Fire_fn = std::uint32_t(__fastcall*)(void* weapon);

   Fire_fn cannon_fire = nullptr;
   Fire_fn laser_fire = nullptr;
   int fire_logged = 0;

   // The player's gun's effects tied to the gun in the hand (exe_effect_attach): the
   // point they're given (lives as long as the effects read it).
   using Effect_attach_fn = void(__fastcall*)(void* handle, void* edx, void* effect_class,
                                               const float* position, void* other);
   using Effect_start_fn = void(__fastcall*)(void* handle, void* edx, void* effect_class,
                                              const float* position, void* other,
                                              std::uint32_t flag);
   using Effect_play_fn = void(__fastcall*)(void* effect_class, void* edx,
                                             const float* position, void* other, void* unknown,
                                             std::uint32_t flag);

   Effect_attach_fn effect_attach = nullptr;
   Effect_start_fn effect_start = nullptr;
   Effect_play_fn effect_play = nullptr;
   glm::vec3 effect_point{};
   const std::uint8_t* effect_aimer = nullptr; // whose fire point effect_point stands for
   bool effect_logged = false;
   std::uint32_t stat_effects_moved = 0;

   // Animations with no readable bone table left out (exe_animation_set).
   using Animation_set_fn = void(__fastcall*)(void* animator, void* edx, const void* animation,
                                               float blend);

   Animation_set_fn animation_set = nullptr;
   std::uint32_t stat_bad_animations = 0;

   std::uint32_t stat_shots = 0;          // through the fire hooks
   std::uint32_t stat_shots_moved = 0;    // ... moved there
   std::uint64_t last_shot_tick = 0;      // GetTickCount64 of the player's last shot
   std::uint8_t* firing_gun = nullptr;    // the player's gun while its shot is fired
   std::uint8_t* last_fired_gun = nullptr;

   // Emitter ordnance (an arc caster's: the Jawa's) fired (exe_emitter_fire).
   using Emitter_fire_fn = std::uint32_t(__fastcall*)(void* ordnance_class, void* edx,
                                                      float* shot);

   Emitter_fire_fn emitter_fire = nullptr;
   int emitter_logged = 0;
   std::uint32_t stat_emitter_shots = 0;   // the player's
   std::uint32_t stat_emitter_in_shot = 0; // ... inside a fire hook (already moved)
   std::uint32_t stat_emitter_moved = 0;   // ... moved here

   // Renders that give the player's body skeleton (see exe_gun_class_render).
   using Gun_render_fn = void(__fastcall*)(void* object, void* edx, const float* matrix,
                                           const void* skeleton, std::uint32_t a,
                                           std::uint32_t b, std::uint32_t c);

   Gun_render_fn gun_class_render = nullptr;
   Gun_render_fn weapon_render = nullptr; // the detour's trampoline
   glm::vec3 feet{};                      // the player's soldier this frame (feet_frame)
   bool feet_valid = false;
   std::uint32_t feet_frame = ~0u;

   // Weapon bash: soldiers drawn last frame (their EntitySoldier, or the weapon drawn in
   // their hands to find it by; feet), and the gesture.
   struct Bash_candidate {
      std::uint8_t* soldier;
      std::uint8_t* weapon;
      glm::vec3 feet;
   };

   std::array<Bash_candidate, 64> bash_candidates{};
   int bash_candidate_count = 0;
   std::uint32_t bash_candidates_frame = 0;
   bool bash_tracking = false;
   glm::vec3 bash_last_motion{};
   float bash_travel = 0.0f;
   float bash_cooldown = 0.0f;
   int bash_log_count = 0;
   glm::vec3 bash_near{}; // the gun's front (last update): candidates are kept near it
   bool bash_near_valid = false;
   float stat_bash_peak_speed = 0.0f;
   std::uint32_t stat_bash_checked_frames = 0;
   std::uint32_t stat_bash_off_frames = 0;
   std::uint32_t stat_bashes = 0;
   std::uint32_t stat_bash_hits = 0;
   std::uint32_t stat_bash_blocked = 0;

   // Soldiers near this point (the eyes on a turret) aren't drawn.
   Soldier_render_fn soldier_render = nullptr;
   bool hide_soldiers = false;
   glm::vec3 hide_soldiers_near{};
   float hide_soldiers_radius = 0.0f;
   std::uint32_t stat_hidden_soldiers = 0;
   std::uint32_t stat_turret_soldier_renders = 0; // soldier renders while hiding (turret)
   float stat_nearest_soldier = 0.0f;           // ... the nearest one's head from the eyes
   std::uint32_t stat_soldier_render_calls = 0; // every call of the hooked render (turret)

   // The player's soldier entity (its weapons' owner), kept from on foot: on a turret it
   // still stands at the gun, and its position (turret_body) tells its draws.
   std::uint8_t* body_owner = nullptr;
   std::uintptr_t body_owner_vtable = 0;
   bool turret_body_valid = false;
   glm::vec3 turret_body{};

   // The vtable of what the local player controls (diagnostics: soldiers are expected),
   // and the last one logged.
   std::uintptr_t player_vtable = 0;
   std::uintptr_t logged_player_vtable = 0;

   // The player's melee weapon (as of the last update) and what's read from it.
   std::uint8_t* weapon = nullptr;
   const std::uint8_t* weapon_class_ = nullptr;
   glm::vec3 color{0.2f, 0.4f, 1.0f};
   std::uint32_t raw_color = 0;
   float blade_length = 1.0f;
   float blade_width = 0.08f;
   glm::vec3 edge_position{};             // hilt space
   glm::vec3 edge_direction{0.0f, 1.0f, 0.0f}; // hilt space
   int edge_hardpoint = 0;
   int edges = 0;
   bool mesh_blade = false; // no energy blade: the model is the blade (see above)

   // The blades in the hands (max_blades): blade 0 is edge 0 (the fields above), the
   // others the class's next edges on hilt 0 (the right hand's) or, with dual sabers
   // (`dual`: hilt 1 has its own bone), on hilt 1 (the left hand's).
   struct Blade_edge {
      int index = 0;
      int hand = 0; // 0 right, 1 left
      glm::vec3 position{};
      glm::vec3 direction{0.0f, 1.0f, 0.0f};
      float length = 1.0f;
   };

   std::array<Blade_edge, max_blades> blade_edges{};
   int blade_count = 0;
   int hardpoints = 0;
   std::array<std::uint32_t, max_hardpoints> hardpoint_bones{};
   bool dual = false;
   glm::vec3 left_edge_direction{0.0f, 1.0f, 0.0f}; // hilt 1's first edge, hilt space
   std::uint32_t damage = 0;
   std::uint32_t push = 0; // x 0.1
   int deflect_state = -1;
   int idle_state = 0;
   bool in_block_stance = false; // the combo was put in the deflecting state

   // The saber. Positions are offsets from the soldier's position (its matrix at the
   // last update): the soldier moves on before the next frame's render and damage.
   bool lit = false;
   float ignition = 0.0f; // 0..1
   bool hand_valid = false;
   glm::vec3 soldier_position{};
   glm::vec3 hilt_offset{}; // the hilt's origin (the hardpoint: where the hand holds it)
   glm::mat3 hilt_rotation{1.0f}; // hilt space -> world (columns: hilt x, y, z)
   glm::vec3 base_offset{}; // where the blade starts
   glm::vec3 dir{0.0f, 1.0f, 0.0f};

   // The left hand's hilt (dual sabers), as the right's above; and every blade this
   // update (offsets from the soldier, directions, full lengths; inactive: not in a hand).
   bool left_hand_valid = false;
   glm::vec3 left_hilt_offset{};
   glm::mat3 left_hilt_rotation{1.0f};
   std::array<bool, max_blades> blade_active{};
   std::array<glm::vec3, max_blades> blade_bases{};
   std::array<glm::vec3, max_blades> blade_dirs{};
   std::array<float, max_blades> blade_lengths{};
   bool prev_left_tip_valid = false;
   glm::vec3 prev_left_tip{}; // relative to the head
   bool prev_tip_valid = false;
   glm::vec3 prev_tip{}; // relative to the head
   float tip_speed = 0.0f;
   bool swinging = false;
   float slow_time = 0.0f;
   float fast_travel = 0.0f; // metres the tip has moved fast since it was last slow
   bool swing_started = false; // hit records to clear at the next damage update
   int swing_hits = 0;         // objects the current swing has hit
   float hits_age = 0.0f;      // seconds since the hit records were cleared
   glm::vec3 swing_dir{};      // the tip's direction at its last fast update
   bool swing_dir_valid = false;
   bool blocking = false;
   bool thrown = false;       // the saber is away (thrown)
   bool control_lost = false; // knocked down, held or dead: the saber is hidden
   bool dead = false;
   glm::vec3 head_offset{};   // the head from the soldier's position

   // Where the game's saber was last put in the hand (the hilt matrix's origin, world).
   glm::vec3 drawn_hilt{};
   std::uint32_t drawn_hilt_frame = 0;
   bool drawn_hilt_valid = false;
   bool drawn_left_hilt_valid = false; // dual sabers: the left hand's, the same frame
   glm::vec3 drawn_left_hilt{};
   int bolts_logged = 0;

   // Saber throw diagnostics: the thrower's flags last logged (-1: no thrower), and the
   // secondary fire button.
   std::int64_t logged_thrower_flags = -2;
   bool secondary_fire_held = false;

   // The player mans a turret on a building; the player's secondary weapon is a grenade.
   bool in_turret = false;
   bool logged_in_turret = false;
   bool secondary_grenade = false;

   // The game's interface screen shown (empty: none) and the last one logged.
   char game_screen[32] = {};
   char logged_game_screen[32] = {};

   // The soldier's state (-1: not a soldier) and the last one logged.
   std::int32_t soldier_state = -1;
   std::int32_t logged_soldier_state = -1;

   // Since the last take_events().
   Events events{};

   // The blade at the last damage update of a swing, and the stand-in state's clock.
   bool damage_pose_valid = false;
   std::array<glm::vec3, max_blades> damage_bases{};
   std::array<glm::vec3, max_blades> damage_dirs{};
   float fake_time = 0.0f;

   // Stand-in combo, state and attack for the fire update.
   alignas(16) std::uint8_t fake_combo[0x40] = {};
   alignas(16) std::uint8_t fake_state[state_size] = {};
   Fake_attack fake_attacks[max_blades] = {}; // one per blade swung
   std::array<int, max_blades> attack_hands{}; // the hand each attack's blade is in
   int swing_left_hits = 0; // of swing_hits, by the left hand's blades
   float samples[max_blades][samples_per_update][7] = {};

   // The game's saber render redirected to the hand: last frame it happened, and the
   // frame counter.
   std::uint32_t frame = 0;
   std::uint32_t game_saber_frame = 0;
   bool game_saber_seen = false;
   bool render_distance_logged = false;
   bool stack_class_logged = false;

   // The body's head bone over the feet (player_head_height), from the body's render.
   float head_bone_height = 0.0f;
   std::uint32_t head_bone_frame = 0;
   bool head_bone_valid = false;
   bool head_bone_logged = false;
   bool head_bone_missing_logged = false;

   // Tuning (bf2vr_saber.txt).
   float pitch_degrees = 0.0f; // the blade tilted up from the controller's aim
   float grip_offset = 0.0f;   // the hilt moved along the blade from the aim pose (m)
   float weapon_scale = 1.0f;  // the body's scale (Frame_input::scale)
   float scaled_blade_length = 1.0f; // the blade's length in the world (the game's)
   bool edges_writable = false; // the class's edge lengths can be set for the hand render

   // Diagnostics, per stats period.
   std::uint32_t stat_third_person_refused = 0;
   std::uint32_t stat_weapon_updates = 0;
   std::uint32_t stat_swing_updates = 0;
   std::uint32_t stat_swing_starts = 0;
   std::uint32_t stat_hits = 0;
   std::uint32_t stat_deflecting_queries = 0;
   std::uint32_t stat_deflect_calls = 0;
   std::uint32_t stat_deflected = 0;
   std::uint32_t stat_auto_deflects = 0; // deflect tried by the auto block
   std::uint32_t stat_rehits = 0;
   std::uint32_t stat_reversals = 0;
   std::uint32_t stat_block_frames = 0;
   std::uint32_t stat_renders = 0;
   std::uint32_t stat_renders_in_hand = 0;
   std::uint32_t stat_block_stances = 0;
   std::uint32_t stat_flight_renders = 0;
   std::uint32_t stat_hidden_renders = 0;
   std::uint32_t stat_thrown_hand_renders = 0;
   std::uint32_t stat_frames = 0;
};

State state;

// XMM1 at the render call (the blade length factor), and the render itself.
float render_length_factor = 1.0f;

// EDI at the soldier renderer's weapon class render calls (0x48e346, 0x48e36e): the
// SoldierElement being drawn (+0x130 its EntitySoldier). The renderer is called
// directly, not through the vtable soldier_render_hook patches (that hook saw nothing
// while the player held a gun), so the weapon bash finds its targets here.
const void* render_element = nullptr;
std::uintptr_t class_render = 0;

template<typename T>
auto engine(const std::uintptr_t image_address) noexcept -> T
{
   return (T)(image_address - exe_image_base + state.image_base);
}

template<typename T>
auto field(const std::uint8_t* object, const std::ptrdiff_t offset) noexcept -> T
{
   T value;
   std::memcpy(&value, object + offset, sizeof(T));

   return value;
}

template<typename T>
void set_field(std::uint8_t* object, const std::ptrdiff_t offset, const T value) noexcept
{
   std::memcpy(object + offset, &value, sizeof(T));
}

auto vtable_of(const std::uint8_t* object) noexcept -> std::uintptr_t
{
   return field<std::uintptr_t>(object, 0);
}

// Replaces a vtable slot that must hold `expected`. Returns the old function or 0.
auto patch_vtable(const std::uintptr_t vtable, const int slot, const std::uintptr_t expected,
                  const void* hook) noexcept -> std::uintptr_t
{
   auto* const entry = engine<std::uintptr_t*>(vtable) + slot;
   const std::uintptr_t target = expected - exe_image_base + state.image_base;

   if (*entry != target) return 0;

   DWORD protection = 0;
   VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &protection);
   *entry = reinterpret_cast<std::uintptr_t>(hook);
   VirtualProtect(entry, sizeof(*entry), protection, &protection);

   return target;
}

// Redirects a call (E8 rel32) that must go to `expected`. Returns the old target or 0.
auto patch_call(const std::uintptr_t site, const std::uintptr_t expected,
                const void* hook) noexcept -> std::uintptr_t
{
   const std::uintptr_t call = site - exe_image_base + state.image_base;
   const std::uintptr_t target = expected - exe_image_base + state.image_base;
   std::int32_t rel;

   std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), sizeof(rel));

   if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8 || call + 5 + rel != target) {
      return 0;
   }

   rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(hook) - (call + 5));

   DWORD protection = 0;
   VirtualProtect(reinterpret_cast<void*>(call + 1), sizeof(rel), PAGE_EXECUTE_READWRITE,
                  &protection);
   std::memcpy(reinterpret_cast<void*>(call + 1), &rel, sizeof(rel));
   VirtualProtect(reinterpret_cast<void*>(call + 1), sizeof(rel), protection, &protection);
   FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

   return target;
}

// --- Reading the player's soldier ----------------------------------------------
//
// Engine pointers can go stale between frames (a soldier dies, a map unloads); reads
// are guarded. These functions hold no objects with destructors (SEH).

auto read_player_melee_weapon() noexcept -> std::uint8_t*
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers) return nullptr;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) {
         return nullptr;
      }

      // The engine's own check for a freed entity.
      if (field<std::uint32_t>(trackable, 0x1c) == 0xddddddddu) return nullptr;

      state.player_vtable = vtable_of(trackable);

      if (state.player_vtable != engine<std::uintptr_t>(exe_soldier_trackable_vtable)) {
         return nullptr;
      }

      state.soldier_state = field<std::int32_t>(trackable, trackable_soldier_state);

      for (int channel = 0; channel < 2; ++channel) {
         const auto index = field<std::int8_t>(trackable, trackable_weapon_channels + channel);

         if (index < 0 || index >= 8) continue;

         auto* const weapon =
            field<std::uint8_t*>(trackable, trackable_weapons + index * sizeof(void*));

         if (weapon && vtable_of(weapon) == engine<std::uintptr_t>(exe_melee_vtable)) {
            return weapon;
         }
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   return nullptr;
}

struct Weapon_info {
   const std::uint8_t* weapon_class;
   std::uint32_t raw_color;
   float length;
   float width;
   float edge_position[3];
   float edge_direction[3];
   int edge_hardpoint;
   int edges;

   struct Edge {
      int hardpoint;
      float position[3];
      float direction[3];
      float length;
   };

   Edge all_edges[max_blades];
   int hardpoints;
   std::uint32_t hardpoint_bones[max_hardpoints];
   const std::uint8_t* render_class; // where the hilts were read (the render's class)
   std::uint32_t damage;
   std::uint32_t push;
   int deflect_state;
   int idle_state;
};

bool read_weapon_info(const std::uint8_t* weapon, Weapon_info& info) noexcept
{
   __try {
      info = {};
      info.deflect_state = -1;
      info.weapon_class = field<const std::uint8_t*>(weapon, weapon_class);
      info.edges = std::clamp(field<int>(info.weapon_class, class_edge_count), 0, 8);

      if (info.edges > 0) {
         const auto* const edge = field<const std::uint8_t*>(info.weapon_class, class_edges);

         info.edge_hardpoint = field<int>(edge, 0);
         std::memcpy(info.edge_position, edge + 0x8, sizeof(info.edge_position));
         std::memcpy(info.edge_direction, edge + 0x14, sizeof(info.edge_direction));
         info.length = field<float>(edge, 0x20);
         info.width = field<float>(edge, 0x24);
         info.raw_color = field<std::uint32_t>(edge, 0x30);

         for (int i = 0; i < std::min(info.edges, max_blades); ++i) {
            const auto* const e = edge + i * edge_size;
            Weapon_info::Edge& out = info.all_edges[i];

            out.hardpoint = field<int>(e, 0);
            std::memcpy(out.position, e + 0x8, sizeof(out.position));
            std::memcpy(out.direction, e + 0x14, sizeof(out.direction));
            out.length = field<float>(e, 0x20);
         }
      }

      // The hilts as the first-person render reads them (its class pointer).
      info.render_class = field<const std::uint8_t*>(weapon, weapon_render_class);

      if (!info.render_class) info.render_class = info.weapon_class;

      info.hardpoints =
         std::clamp(field<int>(info.render_class, class_hardpoint_count), 0, max_hardpoints);

      for (int i = 0; i < info.hardpoints; ++i) {
         info.hardpoint_bones[i] = field<std::uint32_t>(
            info.render_class, class_hardpoint_bones + i * sizeof(std::uint32_t));
      }

      const auto* const combo = field<const std::uint8_t*>(weapon, weapon_combo);

      if (!combo) return true;

      const auto* const states = field<const std::uint8_t*>(combo, combo_states);
      const int state_count = field<std::uint8_t>(combo, combo_state_count);
      const auto* const idle = field<const std::uint8_t*>(combo, combo_idle_state);

      if (states && idle >= states && idle < states + state_count * state_size) {
         info.idle_state = static_cast<int>((idle - states) / state_size);
      }

      for (int i = 0; i < state_count && states; ++i) {
         const auto* const combo_state = states + i * state_size;

         if (field<const void*>(combo_state, state_deflect) && info.deflect_state < 0) {
            info.deflect_state = i;
         }

         const auto* const attacks = field<const std::uint32_t*>(combo_state, state_attacks);
         const int attack_count = field<std::uint32_t>(combo_state, state_attack_count) >> 28;

         for (int a = 0; a < attack_count && attacks; ++a) {
            const std::uint32_t* const attack = attacks + a * 4;

            info.damage = std::max(info.damage, attack[1] & 0xfffu);

            if ((attack[0] >> 30) == 0) info.push = std::max(info.push, attack[2] >> 24);
         }
      }

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// The owner's (soldier's) world matrix as the fire update uses it: D3DX rows right, up,
// forward, position.
bool read_owner_matrix(std::uint8_t* weapon, float (&matrix)[16]) noexcept
{
   __try {
      auto* const owner = field<std::uint8_t*>(weapon, weapon_owner);

      if (!owner) return false;

      using Matrix_fn = const float*(__fastcall*)(void* self, void* edx);
      const auto get_matrix = reinterpret_cast<Matrix_fn>(
         field<const std::uintptr_t*>(owner, 0)[0x28 / sizeof(std::uintptr_t)]);
      const float* const m = get_matrix(owner, nullptr);

      if (!m) return false;

      std::memcpy(matrix, m, sizeof(matrix));

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

bool read_soldier_position(glm::vec3& position) noexcept
{
   float matrix[16];

   if (!state.weapon || !read_owner_matrix(state.weapon, matrix)) return false;

   position = {matrix[12], matrix[13], matrix[14]};

   return true;
}

// The player's soldier through its current weapons' owner (any weapon, read now); the
// owner (the soldier entity) into `owner_out` if given.
bool read_player_soldier(glm::vec3& position, std::int32_t& soldier_state,
                         std::uint8_t** owner_out = nullptr) noexcept
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers) return false;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) {
         return false;
      }

      if (field<std::uint32_t>(trackable, 0x1c) == 0xddddddddu) return false;

      if (vtable_of(trackable) != engine<std::uintptr_t>(exe_soldier_trackable_vtable)) {
         return false;
      }

      soldier_state = field<std::int32_t>(trackable, trackable_soldier_state);

      for (int channel = 0; channel < 2; ++channel) {
         const auto index = field<std::int8_t>(trackable, trackable_weapon_channels + channel);

         if (index < 0 || index >= 8) continue;

         auto* const weapon =
            field<std::uint8_t*>(trackable, trackable_weapons + index * sizeof(void*));
         float matrix[16];

         if (weapon && read_owner_matrix(weapon, matrix)) {
            position = {matrix[12], matrix[13], matrix[14]};

            if (owner_out) *owner_out = field<std::uint8_t*>(weapon, weapon_owner);

            return true;
         }
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   return false;
}

// World vector -> the owner's space (rows orthonormal).
auto to_owner(const float (&m)[16], const glm::vec3 v) noexcept -> glm::vec3
{
   return {glm::dot(v, glm::vec3{m[0], m[1], m[2]}), glm::dot(v, glm::vec3{m[4], m[5], m[6]}),
           glm::dot(v, glm::vec3{m[8], m[9], m[10]})};
}

// The weapon's lit bit; a change restarts its extend / retract time.
void set_weapon_lit(std::uint8_t* weapon, const bool lit) noexcept
{
   __try {
      const auto flags = field<std::uint8_t>(weapon, weapon_flags);
      const bool weapon_lit = (flags & 2) != 0;

      if (weapon_lit == lit) return;

      set_field<std::uint8_t>(weapon, weapon_flags,
                              static_cast<std::uint8_t>(lit ? flags | 2 : flags & ~2));
      set_field<float>(weapon, weapon_lit_time, 0.0f);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

bool read_saber_thrown(const std::uint8_t* weapon) noexcept
{
   __try {
      const auto* const thrower = field<const std::uint8_t*>(weapon, weapon_thrower);

      return thrower && (field<std::uint32_t>(thrower, thrower_flags) & 6) != 0;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// The thrower's flags (bit 0 fired, 1 in flight, 2 back: being caught), -1 without one.
auto read_thrower_flags(const std::uint8_t* weapon) noexcept -> std::int64_t
{
   __try {
      const auto* const thrower = field<const std::uint8_t*>(weapon, weapon_thrower);

      return thrower ? static_cast<std::int64_t>(field<std::uint32_t>(thrower, thrower_flags))
                     : -1;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return -1;
   }
}

// The player's soldier's secondary weapon, or null.
auto read_secondary_weapon() noexcept -> std::uint8_t*
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers) return nullptr;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) return nullptr;
      if (vtable_of(trackable) != engine<std::uintptr_t>(exe_soldier_trackable_vtable)) {
         return nullptr;
      }

      const auto index = field<std::int8_t>(trackable, trackable_weapon_channels + 1);

      if (index < 0 || index >= 8) return nullptr;

      return field<std::uint8_t*>(trackable, trackable_weapons + index * sizeof(void*));
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return nullptr;
   }
}

// The game's textures by name hash (FUN_0040f780: FUN_00726e00 on 0x8eed8c with size
// 0x2000, i.e. 0x1000 keys then the texture objects); a texture object keeps its
// IDirect3DTexture9 at +0x20 (created by FUN_006baa30, released by FUN_006ba140).
// A weapon class keeps its scope overlay's texture hash at +0x74 (ScopeDisplay,
// FUN_00633b90; the class constructor FUN_00679700 sets 0xf6bcfa8e, the default).
constexpr std::uintptr_t exe_texture_table = 0x008eed8c;
constexpr std::uint32_t texture_table_slots = 0x1000;
constexpr std::ptrdiff_t texture_d3d9_texture = 0x20;
constexpr std::ptrdiff_t class_scope_texture = 0x74;
constexpr std::uint32_t default_scope_texture = 0xf6bcfa8eu;

auto find_game_texture(const std::uint32_t hash) noexcept -> void*
{
   if (hash == 0) return nullptr;

   __try {
      const auto* const keys = engine<const std::uint32_t*>(exe_texture_table);
      const std::uint32_t* slot = keys + ((texture_table_slots - 1) & hash);
      std::uint32_t key = *slot;

      for (std::uint32_t probes = 0; probes < texture_table_slots; ++probes) {
         if (key == hash) {
            const auto* const texture =
               reinterpret_cast<const std::uint8_t*>(slot[texture_table_slots]);

            return texture ? field<void*>(texture, texture_d3d9_texture) : nullptr;
         }

         if (key == 0) break;
         if (slot <= keys) slot += texture_table_slots;

         key = slot[-1];
         slot -= 1;
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   return nullptr;
}

// The scope texture hash of the player's soldier's primary weapon, 0 without one.
auto read_primary_scope_texture() noexcept -> std::uint32_t
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers) return 0;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) return 0;
      if (field<std::uint32_t>(trackable, 0x1c) == 0xddddddddu) return 0;
      if (vtable_of(trackable) != engine<std::uintptr_t>(exe_soldier_trackable_vtable)) {
         return 0;
      }

      const auto index = field<std::int8_t>(trackable, trackable_weapon_channels);

      if (index < 0 || index >= 8) return 0;

      const auto* const weapon =
         field<const std::uint8_t*>(trackable, trackable_weapons + index * sizeof(void*));
      const auto* const weapon_class_ = weapon ? field<const std::uint8_t*>(weapon, weapon_class)
                                               : nullptr;

      return weapon_class_ ? field<std::uint32_t>(weapon_class_, class_scope_texture) : 0;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return 0;
   }
}

// The player's soldier's weapon in hand that's reloading (primary first), or null.
// Not for melee units: their force powers are weapons whose recharge runs as a reload.
// Grenades re-arm the same way: not a reload either.
auto read_reloading_weapon() noexcept -> std::uint8_t*
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers) return nullptr;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) return nullptr;
      if (field<std::uint32_t>(trackable, 0x1c) == 0xddddddddu) return nullptr;
      if (vtable_of(trackable) != engine<std::uintptr_t>(exe_soldier_trackable_vtable)) {
         return nullptr;
      }

      std::uint8_t* weapons[2] = {};

      for (int channel = 0; channel < 2; ++channel) {
         const auto index = field<std::int8_t>(trackable, trackable_weapon_channels + channel);

         if (index < 0 || index >= 8) continue;

         weapons[channel] =
            field<std::uint8_t*>(trackable, trackable_weapons + index * sizeof(void*));

         if (weapons[channel] &&
             vtable_of(weapons[channel]) == engine<std::uintptr_t>(exe_melee_vtable)) {
            return nullptr;
         }
      }

      for (std::uint8_t* const weapon : weapons) {
         if (weapon && vtable_of(weapon) != engine<std::uintptr_t>(exe_grenade_vtable) &&
             field<std::int32_t>(weapon, weapon_machine_state) == weapon_reloading) {
            return weapon;
         }
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   return nullptr;
}

bool read_reload(const std::uint8_t* weapon, float& elapsed, float& length) noexcept
{
   __try {
      if (field<std::int32_t>(weapon, weapon_machine_state) != weapon_reloading) return false;

      elapsed = field<float>(weapon, weapon_timer);
      length = field<float>(weapon, weapon_state_length);

      return std::isfinite(elapsed) && std::isfinite(length) && length > 0.0f;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

bool end_reload(std::uint8_t* weapon) noexcept
{
   __try {
      if (field<std::int32_t>(weapon, weapon_machine_state) != weapon_reloading) return false;

      *reinterpret_cast<float*>(weapon + weapon_timer) = field<float>(weapon, weapon_state_length);

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// The vtable of the player's secondary weapon (exe addresses), 0 without one.
auto read_secondary_weapon_vtable() noexcept -> std::uintptr_t
{
   __try {
      const auto* const weapon = read_secondary_weapon();

      return weapon ? vtable_of(weapon) - state.image_base + exe_image_base : 0;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return 0;
   }
}

// The game's interface screen shown into `name` (empty without one).
void read_game_screen(char (&name)[32]) noexcept
{
   name[0] = '\0';

   __try {
      const auto* const shell = *engine<std::uint8_t* const*>(exe_shell);

      if (!shell || field<std::int32_t>(shell, shell_stack_top) < 0) return;

      for (const std::ptrdiff_t offset : shell_screen_names) {
         if (shell[offset] == '\0') continue;

         std::memcpy(name, shell + offset, sizeof(name) - 1);
         name[sizeof(name) - 1] = '\0';

         return;
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      name[0] = '\0';
   }
}

// The controlled entity is a MountedTurret on a building.
bool read_player_in_turret() noexcept
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers) return false;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) return false;
      if (vtable_of(trackable) != engine<std::uintptr_t>(exe_mounted_turret_trackable_vtable)) {
         return false;
      }

      const auto* const host = field<const std::uint8_t*>(trackable, turret_trackable_host);

      if (!host) return false;

      const std::uintptr_t host_vtable = vtable_of(host);

      for (const std::uintptr_t building : exe_building_vtables) {
         if (host_vtable == engine<std::uintptr_t>(building)) return true;
      }

      return false;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

int read_control_mode() noexcept
{
   __try {
      return *engine<const std::int32_t*>(exe_control_mode);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return -1;
   }
}

bool read_weapon_lit(const std::uint8_t* weapon) noexcept
{
   __try {
      return (field<std::uint8_t>(weapon, weapon_flags) & 2) != 0;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// --- Hooks ---------------------------------------------------------------------

// The weapon is in the local player's soldier's hands (either channel).
bool is_player_gun(const std::uint8_t* weapon) noexcept
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers) return false;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) return false;
      if (field<std::uint32_t>(trackable, 0x1c) == 0xddddddddu) return false;
      if (vtable_of(trackable) != engine<std::uintptr_t>(exe_soldier_trackable_vtable)) {
         return false;
      }

      for (int channel = 0; channel < 2; ++channel) {
         const auto index = field<std::int8_t>(trackable, trackable_weapon_channels + channel);

         if (index < 0 || index >= 8) continue;

         if (field<const std::uint8_t*>(trackable, trackable_weapons + index * sizeof(void*)) ==
             weapon) {
            return true;
         }
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   return false;
}

auto read_vec3(const std::uint8_t* object, const std::ptrdiff_t offset) noexcept -> glm::vec3
{
   return {field<float>(object, offset), field<float>(object, offset + 4),
           field<float>(object, offset + 8)};
}

void write_vec3(std::uint8_t* object, const std::ptrdiff_t offset, const glm::vec3 v) noexcept
{
   set_field(object, offset, v.x);
   set_field(object, offset + 4, v.y);
   set_field(object, offset + 8, v.z);
}

// Moves the aimer's fire point and direction (see aim_from_hand). False: unchanged.
bool move_aim(std::uint8_t* weapon, glm::vec3& old_position, glm::vec3& old_direction,
              glm::vec3& position, glm::vec3& direction) noexcept
{
   __try {
      auto* const aimer = field<std::uint8_t*>(weapon, weapon_aimer);

      if (!aimer) return false;

      old_position = read_vec3(aimer, aimer_fire_position);
      old_direction = read_vec3(aimer, aimer_fire_direction);
      position = old_position;
      direction = old_direction;

      if (!vr::fire_from_hand(position, direction)) return false;

      write_vec3(aimer, aimer_fire_position, position);
      write_vec3(aimer, aimer_fire_direction, direction);

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   return false;
}

// The aimer's fire point and direction for the player's gun, moved to the gun in the
// hand (vr::fire_from_hand), around the shot; put back after it. False: left as it is.
bool aim_from_hand(std::uint8_t* weapon, glm::vec3& old_position,
                   glm::vec3& old_direction) noexcept
{
   if (!vr::active() || !is_player_gun(weapon)) return false;

   glm::vec3 position{};
   glm::vec3 direction{};

   if (!move_aim(weapon, old_position, old_direction, position, direction)) return false;

   if (state.fire_logged < 3) {
      state.fire_logged += 1;

      log_fmt(Log_level::info,
              "VR: a shot moved to the gun in the hand: from ({:.2f}, {:.2f}, {:.2f}) to "
              "({:.2f}, {:.2f}, {:.2f}), {:.2f} m; direction ({:.2f}, {:.2f}, {:.2f}) -> "
              "({:.2f}, {:.2f}, {:.2f}).",
              old_position.x, old_position.y, old_position.z, position.x, position.y,
              position.z, glm::distance(old_position, position), old_direction.x,
              old_direction.y, old_direction.z, direction.x, direction.y, direction.z);
   }

   return true;
}

void restore_aim(std::uint8_t* weapon, const glm::vec3 position, const glm::vec3 direction) noexcept
{
   __try {
      auto* const aimer = field<std::uint8_t*>(weapon, weapon_aimer);

      write_vec3(aimer, aimer_fire_position, position);
      write_vec3(aimer, aimer_fire_direction, direction);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

// `aimer` is the aimer of a weapon in the player's soldier's hands.
bool is_player_aimer(const void* aimer) noexcept
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers || !aimer) return false;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) return false;
      if (vtable_of(trackable) != engine<std::uintptr_t>(exe_soldier_trackable_vtable)) {
         return false;
      }

      for (int channel = 0; channel < 2; ++channel) {
         const auto index = field<std::int8_t>(trackable, trackable_weapon_channels + channel);

         if (index < 0 || index >= 8) continue;

         const auto* const weapon =
            field<const std::uint8_t*>(trackable, trackable_weapons + index * sizeof(void*));

         if (weapon && field<const void*>(weapon, weapon_aimer) == aimer) return true;
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   return false;
}

// The player's aimer's fire point moved to the gun in the hand, into effect_point. False:
// the gun isn't in a hand.
bool move_effect_point(const std::uint8_t* aimer) noexcept
{
   glm::vec3 point{};
   glm::vec3 direction{};

   __try {
      point = read_vec3(aimer, aimer_fire_position);
      direction = read_vec3(aimer, aimer_fire_direction);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }

   const glm::vec3 game_point = point;

   if (!vr::fire_from_hand(point, direction)) return false;

   state.effect_point = point;

   if (!std::exchange(state.effect_logged, true)) {
      log_fmt(Log_level::info,
              "VR: an effect of the player's gun put at the gun in the hand: {:.2f} m from "
              "the game's fire point.",
              glm::distance(game_point, point));
   }

   return true;
}

// The position an effect is given: for the player's gun the point at the gun in the
// hand. The gun is told by the position (its aimer's fire point) or by `object`, the
// effect's object inside the weapon (effect_objects): the base Weapon's update ties its
// effects again every frame to the soldier's aim matrix (at the game camera's height),
// which isn't the aimer's.
auto effect_position(const float* position, const void* object) noexcept -> const float*
{
   // Not a melee weapon's (its sparks and the like are where the game puts them): with
   // a saber the effects went to a stale gun point (log 2026-10-28 22:56).
   if (!vr::active() || !position || state.weapon) return position;

   const auto* aimer = reinterpret_cast<const std::uint8_t*>(position) - aimer_fire_position;

   if (!is_player_aimer(aimer)) {
      aimer = nullptr;

      for (const std::ptrdiff_t offset : effect_objects) {
         const auto* const weapon = static_cast<const std::uint8_t*>(object) - offset;

         if (object && is_player_gun(weapon)) {
            aimer = field<const std::uint8_t*>(weapon, weapon_aimer);
            break;
         }
      }
   }

   if (!aimer || !move_effect_point(aimer)) return position;

   state.effect_aimer = aimer;
   state.stat_effects_moved += 1;

   return &state.effect_point.x;
}

bool readable_memory(const void* address, std::size_t size) noexcept;

void __fastcall effect_attach_hook(void* handle, void* edx, void* effect_class,
                                   const float* position, void* other) noexcept
{
   const float* const moved = effect_position(position, effect_class);

   state.effect_attach(handle, edx, effect_class, moved, other);
}

void __fastcall effect_start_hook(void* handle, void* edx, void* effect_class,
                                  const float* position, void* other,
                                  const std::uint32_t flag) noexcept
{
   const float* const moved = effect_position(position, handle);

   state.effect_start(handle, edx, effect_class, moved, other, flag);
}

void __fastcall effect_play_hook(void* effect_class, void* edx, const float* position,
                                 void* other, void* unknown, const std::uint32_t flag) noexcept
{
   const float* const moved = effect_position(position, nullptr);

   state.effect_play(effect_class, edx, moved, other,
                     unknown, flag);
}

// `size` bytes from `address` are committed, readable memory. Asked, not tried: a
// faulting read would be caught, but the crash logger writes a dump for it first (a
// freeze of up to a second).
bool readable_memory(const void* const address, const std::size_t size) noexcept
{
   auto at = reinterpret_cast<std::uintptr_t>(address);
   const std::uintptr_t end = at + size;

   if (!address || end < at) return false;

   while (at < end) {
      MEMORY_BASIC_INFORMATION info{};

      if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) == 0) {
         return false;
      }

      constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                 PAGE_EXECUTE_WRITECOPY;

      if (info.State != MEM_COMMIT || (info.Protect & readable) == 0 ||
          (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
         return false;
      }

      at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
   }

   return true;
}

// An animation whose bone table can be read whole.
// The animator's per-bone arrays (FUN_0072f3f0: +0 0x28 bytes, +0x500 0x20, +0x900 2 a
// bone) hold animation_max_bones: one with more (the Ewok's spear throw, 66) writes past
// them over the animation pointer (+0x980) and crashes.
constexpr std::uint16_t animation_max_bones = 32;

bool readable_animation(const std::uint8_t* animation) noexcept
{
   if (!readable_memory(animation, 12)) return false;

   const auto* const bones = field<const std::uint8_t*>(animation, 0);
   const auto count = field<std::uint16_t>(animation, 10);

   if (count > animation_max_bones) return false;

   return count == 0 || readable_memory(bones, std::size_t{count} * 0x38);
}

void __fastcall animation_set_hook(void* animator, void* edx, const void* animation,
                                   const float blend) noexcept
{
   if (animation && !readable_animation(static_cast<const std::uint8_t*>(animation))) {
      if (state.stat_bad_animations++ < 5) {
         log_fmt(Log_level::warning,
                 "VR: an animation with too many bones ({}) or no readable bone table ({}) "
                 "left out (animator {}, called from 0x{:08x}).",
                 readable_memory(animation, 12)
                    ? field<std::uint16_t>(static_cast<const std::uint8_t*>(animation), 10)
                    : 0,
                 animation, animator,
                 reinterpret_cast<std::uintptr_t>(_ReturnAddress()) -
                    reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + exe_image_base);
      }

      return;
   }

   state.animation_set(animator, edx, animation, blend);
}

auto fire_from_hand_hook(void* weapon, const State::Fire_fn fire) noexcept -> std::uint32_t
{
   auto* const gun = static_cast<std::uint8_t*>(weapon);
   glm::vec3 position{};
   glm::vec3 direction{};

   if (!vr::active() || !is_player_gun(gun)) return fire(weapon);

   state.stat_shots += 1;
   state.last_shot_tick = GetTickCount64();

   state.last_fired_gun = gun;

   if (!aim_from_hand(gun, position, direction)) return fire(weapon);

   state.stat_shots_moved += 1;
   state.firing_gun = gun;

   const std::uint32_t result = fire(weapon);

   state.firing_gun = nullptr;
   restore_aim(gun, position, direction);

   return result;
}

// An emitter ordnance's shot (OrdnanceEmitterClass, FUN_005fce60: thiscall(class, shot),
// ret 4; shot +0 position, +0xc direction, +0x1c the shooter): arcs searched and drawn
// from the shot's position, and an effect placed at the shooter's aimer (FUN_005fc2e0,
// aimer +0x88 / +0x48) tied to the shooter. The Jawa's arc caster fires one. Fired from
// a hooked fire function it has the gun's point already; otherwise the player's (its
// position at the gun's aimer's fire point) gets the gun in the hand, and so does the
// aimer for the call.
std::uint32_t __fastcall emitter_fire_hook(void* ordnance_class, void* edx, float* shot) noexcept
{
   if (!vr::active() || !shot || state.weapon) {
      return state.emitter_fire(ordnance_class, edx, shot);
   }

   std::uint8_t* const gun = state.last_fired_gun;

   if (state.firing_gun) {
      state.stat_emitter_shots += 1;
      state.stat_emitter_in_shot += 1;

      return state.emitter_fire(ordnance_class, edx, shot);
   }

   const glm::vec3 shot_position{shot[0], shot[1], shot[2]};
   glm::vec3 old_position{};
   glm::vec3 old_direction{};
   glm::vec3 position{};
   glm::vec3 direction{};

   if (!gun || GetTickCount64() - state.last_shot_tick > 2000 || !is_player_gun(gun) ||
       !readable_memory(field<const std::uint8_t*>(gun, weapon_aimer), 0x94)) {
      return state.emitter_fire(ordnance_class, edx, shot);
   }

   // The player's: fired from its gun's aimer's (game) fire point.
   const glm::vec3 game_point =
      read_vec3(field<const std::uint8_t*>(gun, weapon_aimer), aimer_fire_position);

   if (glm::distance(game_point, shot_position) > 0.5f ||
       !move_aim(gun, old_position, old_direction, position, direction)) {
      if (state.emitter_logged < 6) {
         state.emitter_logged += 1;

         log_fmt(Log_level::info,
                 "VR: an emitter shot (arc) after the player's shot left as it is: {:.2f} m "
                 "from the gun's game fire point.",
                 glm::distance(game_point, shot_position));
      }

      return state.emitter_fire(ordnance_class, edx, shot);
   }

   state.stat_emitter_shots += 1;
   state.stat_emitter_moved += 1;

   shot[0] = position.x;
   shot[1] = position.y;
   shot[2] = position.z;
   shot[3] = direction.x;
   shot[4] = direction.y;
   shot[5] = direction.z;

   if (state.emitter_logged < 6) {
      state.emitter_logged += 1;

      log_fmt(Log_level::info,
              "VR: an emitter shot (arc) of the player's gun moved to the gun in the hand: "
              "from ({:.2f}, {:.2f}, {:.2f}) to ({:.2f}, {:.2f}, {:.2f}), {:.2f} m.",
              shot_position.x, shot_position.y, shot_position.z, position.x, position.y,
              position.z, glm::distance(shot_position, position));
   }

   const std::uint32_t result = state.emitter_fire(ordnance_class, edx, shot);

   restore_aim(gun, old_position, old_direction);

   return result;
}

std::uint32_t __fastcall cannon_fire_hook(void* weapon) noexcept
{
   return fire_from_hand_hook(weapon, state.cannon_fire);
}

std::uint32_t __fastcall laser_fire_hook(void* weapon) noexcept
{
   return fire_from_hand_hook(weapon, state.laser_fire);
}

// The muzzle flash's draw (exe_flash_draw) and what its call site had in EDI / XMM3.
std::uintptr_t flash_draw = 0;
const std::uint8_t* flash_weapon = nullptr;
float flash_fade = 0.0f;
std::uint32_t stat_flashes_moved = 0;
bool flash_logged = false;

// Calls the flash's draw (thiscall, 2 stack arguments, ret 8) with XMM3 set.
__declspec(naked) void __fastcall call_flash_draw(void* /*weapon_class*/, void* /*edx*/,
                                                  const float* /*position*/,
                                                  const float* /*direction*/)
{
   __asm {
      movss xmm3, dword ptr [flash_fade]
      push dword ptr [esp + 8]
      push dword ptr [esp + 8]
      call dword ptr [flash_draw]
      ret 8
   }
}

// The player's gun's flash goes where its shots start (vr::flash_from_hand), not to
// the game's first-person model's muzzle in front of the camera.
void __fastcall flash_draw_hook(void* flash_class, void*, const float* position,
                                const float* direction) noexcept
{
   float moved[3];

   if (vr::active() && position && is_player_gun(flash_weapon)) {
      glm::vec3 p{position[0], position[1], position[2]};

      if (vr::flash_from_hand(p)) {
         moved[0] = p.x;
         moved[1] = p.y;
         moved[2] = p.z;
         position = moved;
         stat_flashes_moved += 1;

         if (!std::exchange(flash_logged, true)) {
            log(Log_level::info, "VR: the gun's muzzle flash drawn where its shots start.");
         }
      }
   }

   call_flash_draw(flash_class, nullptr, position, direction);
}

// The call site's entry: keeps the weapon (EDI) and the fade (XMM3).
__declspec(naked) void flash_draw_stub()
{
   __asm {
      mov dword ptr [flash_weapon], edi
      movss dword ptr [flash_fade], xmm3
      jmp flash_draw_hook
   }
}

void note_bash_candidate(const void* element, const float* matrix) noexcept;

auto read_vtable(const std::uint8_t* object) noexcept -> std::uintptr_t
{
   __try {
      return vtable_of(object);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return 0;
   }
}

// The player's soldier (remembered from on foot) where it stands on the turret.
bool read_turret_body(glm::vec3& position) noexcept
{
   if (!state.body_owner || !state.body_owner_vtable) return false;

   __try {
      if (vtable_of(state.body_owner) != state.body_owner_vtable) return false;

      using Matrix_fn = const float*(__fastcall*)(void* self, void* edx);
      const auto get_matrix = reinterpret_cast<Matrix_fn>(
         field<const std::uintptr_t*>(state.body_owner, 0)[0x28 / sizeof(std::uintptr_t)]);
      const float* const m = get_matrix(state.body_owner, nullptr);

      if (!m) return false;

      position = {m[12], m[13], m[14]};

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

void __fastcall soldier_render_hook(void* element, void*, const float* matrix,
                                    const void* arg) noexcept
{
   if (vr::active()) note_bash_candidate(element, matrix);

   if (state.in_turret) state.stat_soldier_render_calls += 1;

   // On a turret the player's own soldier (at the entity's position) isn't drawn.
   if (state.turret_body_valid && matrix &&
       glm::distance(glm::vec3{matrix[12], matrix[13], matrix[14]}, state.turret_body) <
          player_body_radius) {
      state.stat_hidden_soldiers += 1;

      return;
   }

   // The soldier's origin is at its feet: its head (up the matrix's up row) can be at
   // the eyes while the feet are further than the radius.
   if (state.hide_soldiers && matrix) {
      const glm::vec3 feet{matrix[12], matrix[13], matrix[14]};
      const glm::vec3 up{matrix[4], matrix[5], matrix[6]};
      const glm::vec3 head = feet + (glm::length(up) > 1e-3f ? glm::normalize(up) : glm::vec3{0.0f, 1.0f, 0.0f}) * 1.6f;
      const float nearest = std::min({glm::distance(feet, state.hide_soldiers_near),
                                      glm::distance(head, state.hide_soldiers_near),
                                      glm::distance((feet + head) * 0.5f, state.hide_soldiers_near)});

      if (state.stat_turret_soldier_renders == 0 || nearest < state.stat_nearest_soldier) {
         state.stat_nearest_soldier = nearest;
      }

      state.stat_turret_soldier_renders += 1;

      if (nearest < state.hide_soldiers_radius) {
         state.stat_hidden_soldiers += 1;

         return;
      }
   }

   state.soldier_render(element, nullptr, matrix, arg);
}

// --- Weapon bash ------------------------------------------------------------------
//
// A fast jab or swing of the gun's front (update_bash) hits the nearest soldier drawn
// last frame whose body (a capsule over its feet) the gun passes close to: the game's
// own damage and push, credited to the player's gun (see exe_soldier_main_vtable).

// The gesture: the gun's front faster than Settings::bash_speed (relative to the head,
// not pulled back towards it) for this far, at most once per cooldown.
constexpr float bash_min_travel = 0.08f;
constexpr float bash_cooldown_seconds = 0.6f;

// Soldiers drawn further than this from the gun aren't kept as targets (the list would
// fill with the far ones on a big map).
constexpr float bash_candidate_range = 8.0f;

// The first bashes are logged one by one (why each hit or missed).
constexpr int bash_logged_max = 40;

// The game's search (see exe_find_objects) is centred this far back from the gun's
// front and reaches this far (plus each object's own radius, horizontally).
constexpr float bash_query_back = 0.15f;
constexpr float bash_radius = 0.3f;

std::uintptr_t damage_record_fn = 0;

// FUN_00438a40 on `record` with the seven fields and `damage` in XMM2.
__declspec(naked) void __cdecl make_damage_record(void* /*record*/,
                                                  const std::uint32_t* /*fields*/,
                                                  float /*damage*/)
{
   __asm {
      push ebp
      mov ebp, esp
      push esi
      push edi
      movss xmm2, dword ptr [ebp + 16]
      sub esp, 0x1c
      mov esi, dword ptr [ebp + 12]
      mov edi, esp
      mov ecx, 7
      cld
      rep movsd
      mov ecx, dword ptr [ebp + 8]
      call dword ptr [damage_record_fn]
      lea esp, [ebp - 8]
      pop edi
      pop esi
      pop ebp
      ret
   }
}

std::uintptr_t find_objects_fn = 0;

// FUN_0045e210 (see exe_find_objects): how many entities went into `found`.
__declspec(naked) int __cdecl find_objects(const float* /*centre*/, void** /*found*/,
                                           int /*size*/, const void* /*left_out*/,
                                           std::uint32_t /*mask*/, float /*radius*/)
{
   __asm {
      push ebp
      mov ebp, esp
      movss xmm1, dword ptr [ebp + 28]
      push dword ptr [ebp + 24]
      push dword ptr [ebp + 20]
      push dword ptr [ebp + 16]
      mov edx, dword ptr [ebp + 12]
      mov ecx, dword ptr [ebp + 8]
      call dword ptr [find_objects_fn]
      add esp, 12
      pop ebp
      ret
   }
}

auto virtual_slot(const void* object, const int slot) noexcept -> void*
{
   return (*static_cast<void* const* const*>(object))[slot];
}

auto entity_team_of(const std::uint8_t* entity) noexcept -> std::int32_t
{
   return static_cast<std::int32_t>(field<std::uint32_t>(entity, entity_team) << 28) >> 28;
}

// The attacker entity behind the player's gun (owner +0x18, slot 8), or null.
auto bash_attacker(std::uint8_t* weapon) noexcept -> std::uint8_t*
{
   using Get_fn = std::uint8_t*(__thiscall*)(void*);

   __try {
      auto* const source = field<std::uint8_t*>(weapon, weapon_owner) + 0x18;

      return reinterpret_cast<Get_fn>(virtual_slot(source, 8))(source);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return nullptr;
   }
}

// Why `target` isn't worth hitting (null: worth it). Teams are left to the game's damage
// (friendly fire, neutral droids); only soldiers have a state to be dead in.
auto bash_target_problem(const std::uint8_t* target, const std::uint8_t* attacker) noexcept
   -> const char*
{
   using Collision_fn = void*(__thiscall*)(const void*);

   __try {
      if (target == attacker) return "the player";
      if (field<std::uint32_t>(target, 0x1c) == 0xddddddddu) return "deleted";
      if (!reinterpret_cast<Collision_fn>(virtual_slot(target, entity_collision_slot))(target)) {
         return "no collision";
      }
      if (vtable_of(target) != engine<std::uintptr_t>(exe_soldier_main_vtable)) return nullptr;

      const auto soldier_state = field<std::int32_t>(target, entity_soldier_state);

      if (soldier_state < 0 || soldier_state >= soldier_dying) return "dead";

      return nullptr;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return "unreadable";
   }
}

enum class Bash_result { missed, hit, blocked, failed };

// The game's damage and push on `target` from the player's gun (see the constants).
auto hurt_soldier(std::uint8_t* weapon, std::uint8_t* attacker, std::uint8_t* target,
                  const glm::vec3 direction, const glm::vec3 point, const float damage,
                  const float push) noexcept -> Bash_result
{
   using Id_fn = std::uint32_t(__thiscall*)(void*);
   using Block_fn = bool(__thiscall*)(void*, int, const float*, const float*);
   using Damage_fn = void(__thiscall*)(void*, void*, const float*, const float*, int, int);
   using Push_fn = void(__thiscall*)(void*, const float*, float, int);

   __try {
      const float direction_xyz[3] = {direction.x, direction.y, direction.z};
      const float point_xyz[3] = {point.x, point.y, point.z};

      if (reinterpret_cast<Block_fn>(virtual_slot(target, entity_block_slot))(
             target, 0, direction_xyz, point_xyz)) {
         return Bash_result::blocked;
      }

      const auto* const owner = field<const std::uint8_t*>(weapon, weapon_owner);
      const std::uint32_t fields[7] = {
         field<std::uint32_t>(owner, owner_damage_source),
         static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(attacker)),
         field<std::uint32_t>(attacker, entity_id),
         static_cast<std::uint32_t>(entity_team_of(attacker)),
         reinterpret_cast<Id_fn>(virtual_slot(attacker, 11))(attacker),
         field<std::uint32_t>(weapon, weapon_class),
         std::bit_cast<std::uint32_t>(1.0f)};

      alignas(16) std::uint8_t record[0x80]{};

      make_damage_record(record, fields, damage);

      auto* const damageable = target + entity_damageable;

      reinterpret_cast<Damage_fn>(virtual_slot(damageable, damageable_damage_slot))(
         damageable, record, direction_xyz, point_xyz, -1, -1);

      if (push > 0.0f) {
         reinterpret_cast<Push_fn>(virtual_slot(target, entity_push_slot))(target, direction_xyz,
                                                                          push, 0);
      }

      return Bash_result::hit;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return Bash_result::failed;
   }
}

// The player's soldier's primary weapon (the gun in hand), or null.
auto read_primary_weapon() noexcept -> std::uint8_t*
{
   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);

      if (!trackers) return nullptr;

      const auto* const trackable = field<const std::uint8_t*>(trackers, 0x10);

      if (!trackable || trackable == reinterpret_cast<const std::uint8_t*>(-1)) return nullptr;
      if (vtable_of(trackable) != engine<std::uintptr_t>(exe_soldier_trackable_vtable)) {
         return nullptr;
      }

      const auto index = field<std::int8_t>(trackable, trackable_weapon_channels);

      if (index < 0 || index >= 8) return nullptr;

      return field<std::uint8_t*>(trackable, trackable_weapons + index * sizeof(void*));
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return nullptr;
   }
}

// find_objects with the saber's mask and bash_radius; -1 if it crashed.
auto find_objects_safely(const float* centre, void** found, const int size,
                         const void* attacker) noexcept -> int
{
   __try {
      return find_objects(centre, found, size, attacker, find_objects_saber_mask, bash_radius);
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return -1;
   }
}

void bash(const Gun& gun, const float speed) noexcept
{
   const Settings& s = settings();
   std::uint8_t* const weapon = read_primary_weapon();
   std::uint8_t* const attacker = weapon ? bash_attacker(weapon) : nullptr;
   const bool logged = state.bash_log_count < bash_logged_max;

   state.stat_bashes += 1;
   state.events.bashes += 1;

   if (logged) state.bash_log_count += 1;

   if (!attacker) {
      if (logged) {
         log_fmt(Log_level::info,
                 "VR: weapon bash {} ({:.1f} m/s): no attacker (gun 0x{:08x}).",
                 state.bash_log_count, speed, reinterpret_cast<std::uintptr_t>(weapon));
      }

      return;
   }

   // The game's own search around the gun's front (see exe_find_objects); every entity
   // found is hit, the game deciding (team, friendly fire) what the damage does.
   const glm::vec3 centre = gun.front - gun.forward * bash_query_back;
   const float centre_xyz[3] = {centre.x, centre.y, centre.z};
   std::array<void*, 8> found{};
   const int found_count =
      find_objects_safely(centre_xyz, found.data(), static_cast<int>(found.size()), attacker);

   glm::vec3 direction{gun.forward.x, 0.0f, gun.forward.z};

   direction = glm::length(direction) > 1e-3f ? glm::normalize(direction)
                                              : glm::vec3{1.0f, 0.0f, 0.0f};

   int hits = 0;
   int blocked = 0;
   int failed = 0;
   const char* rejected = nullptr;

   for (int i = 0; i < std::clamp(found_count, 0, static_cast<int>(found.size())); ++i) {
      auto* const target = static_cast<std::uint8_t*>(found[i]);

      if (!target) continue;

      if (const char* const problem = bash_target_problem(target, attacker)) {
         rejected = problem;

         continue;
      }

      const Bash_result result =
         hurt_soldier(weapon, attacker, target, direction, centre, s.bash_damage, s.bash_push);

      if (logged) {
         log_fmt(Log_level::info,
                 "VR: weapon bash {} ({:.1f} m/s): entity vtable 0x{:08X}, team {}{}: {}.",
                 state.bash_log_count, speed,
                 vtable_of(target) - state.image_base + exe_image_base, entity_team_of(target),
                 vtable_of(target) == engine<std::uintptr_t>(exe_soldier_main_vtable)
                    ? fmt::format(", soldier state {}",
                                  field<std::int32_t>(target, entity_soldier_state))
                    : std::string{},
                 result == Bash_result::hit       ? "hit"
                 : result == Bash_result::blocked ? "blocked"
                                                  : "the game's damage call failed");
      }

      if (result == Bash_result::hit) hits += 1;
      else if (result == Bash_result::blocked) blocked += 1;
      else failed += 1;
   }

   if (logged && hits + blocked + failed == 0) {
      log_fmt(Log_level::info,
              "VR: weapon bash {} ({:.1f} m/s): the game's search found {} near the gun{}{}.",
              state.bash_log_count, speed,
              found_count < 0 ? std::string{"nothing (it crashed)"}
                              : fmt::format("{} entities", found_count),
              rejected ? ", the last left out: " : "", rejected ? rejected : "");
   }

   if (hits > 0) {
      state.stat_bash_hits += 1;
      state.events.bash_hits += 1;
   }
   else if (blocked > 0) {
      state.stat_bash_blocked += 1;
      state.events.bash_blocked += 1;
   }
}

// The gesture (see bash_min_travel), once per game frame.
void update_bash(const Frame_input& input, const bool melee_in_hand) noexcept
{
   const Settings& s = settings();

   state.bash_cooldown = std::max(state.bash_cooldown - input.dt, 0.0f);

   state.bash_near_valid = input.gun.has_value();

   if (input.gun) state.bash_near = input.gun->front;

   if (!s.weapon_bash || !input.gun || melee_in_hand || state.in_turret || input.dt <= 0.0f ||
       !damage_record_fn || !player_soldier_on_foot()) {
      if (s.weapon_bash && input.gun && !melee_in_hand) state.stat_bash_off_frames += 1;

      state.bash_tracking = false;

      return;
   }

   state.stat_bash_checked_frames += 1;

   const glm::vec3 motion = input.gun->motion;

   if (!std::exchange(state.bash_tracking, true)) {
      state.bash_last_motion = motion;
      state.bash_travel = 0.0f;

      return;
   }

   const glm::vec3 velocity = (motion - state.bash_last_motion) / input.dt;
   const float speed = glm::length(velocity);
   const glm::vec3 outward = glm::length(motion) > 1e-3f ? glm::normalize(motion) : glm::vec3{};

   state.bash_last_motion = motion;
   state.stat_bash_peak_speed = std::max(state.stat_bash_peak_speed, speed);

   const bool striking = speed > s.bash_speed && glm::dot(velocity, outward) > -0.3f * speed;

   state.bash_travel = striking ? state.bash_travel + speed * input.dt : 0.0f;

   if (state.bash_travel < bash_min_travel || state.bash_cooldown > 0.0f) return;

   state.bash_travel = 0.0f;
   state.bash_cooldown = bash_cooldown_seconds;

   bash(*input.gun, speed);
}

void add_bash_candidate(std::uint8_t* soldier, std::uint8_t* weapon,
                        const glm::vec3 feet) noexcept
{
   if (state.bash_candidates_frame != state.frame) {
      state.bash_candidates_frame = state.frame;
      state.bash_candidate_count = 0;
   }

   if (state.bash_near_valid && glm::distance(feet, state.bash_near) > bash_candidate_range) {
      return;
   }

   if (state.bash_candidate_count < static_cast<int>(state.bash_candidates.size())) {
      state.bash_candidates[state.bash_candidate_count++] = {soldier, weapon, feet};
   }
}

// A weapon drawn with its holder's body (skeleton given, the body's matrix at its
// feet), for the weapon bash's targets: soldiers' own renders aren't always hooked
// (none were seen while the player held a gun).
void note_bash_weapon(void* weapon, const float* matrix, const void* skeleton) noexcept
{
   if (!weapon || !matrix || !skeleton) return;

   add_bash_candidate(nullptr, static_cast<std::uint8_t*>(weapon),
                      {matrix[12], matrix[13], matrix[14]});
}

// The soldier being drawn, for the weapon bash's targets (frame-stamped).
void note_bash_candidate(const void* element, const float* matrix) noexcept
{
   if (!element || !matrix) return;

   __try {
      auto* const soldier =
         field<std::uint8_t*>(static_cast<const std::uint8_t*>(element), element_soldier);

      if (!soldier || vtable_of(soldier) != engine<std::uintptr_t>(exe_soldier_main_vtable)) {
         return;
      }

      add_bash_candidate(soldier, nullptr, {matrix[12], matrix[13], matrix[14]});
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }
}

void note_head_bone(const float* matrix, const void* skeleton) noexcept;

void __fastcall gun_class_render_hook(void* class_object, void*, const float* matrix,
                                      const void* skeleton, std::uint32_t a, std::uint32_t b,
                                      std::uint32_t c) noexcept
{
   if (vr::active()) {
      note_head_bone(matrix, skeleton);
      note_bash_candidate(std::exchange(render_element, nullptr), matrix);
   }

   state.gun_class_render(class_object, nullptr, matrix, skeleton, a, b, c);
}

void __fastcall weapon_render_hook(void* weapon, void*, const float* matrix, const void* skeleton,
                                   std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept
{
   if (vr::active()) {
      note_head_bone(matrix, skeleton);
      note_bash_weapon(weapon, matrix, skeleton);
   }

   state.weapon_render(weapon, nullptr, matrix, skeleton, a, b, c);
}

// Puts a jump to `hook` over the function's first bytes (`prologue`, whole
// instructions); the returned trampoline runs them and jumps back. Null on another exe.
template<std::size_t N>
auto detour(const std::uintptr_t function, const std::uint8_t (&prologue)[N],
            const void* hook) noexcept -> void*
{
   static_assert(N >= 5 && N <= 11);

   auto* const code = engine<std::uint8_t*>(function);

   if (std::memcmp(code, prologue, N) != 0) return nullptr;

   auto* const trampoline = static_cast<std::uint8_t*>(
      VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

   if (!trampoline) return nullptr;

   std::memcpy(trampoline, prologue, N);
   trampoline[N] = 0xE9;

   const auto back = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(code + N) -
                                                reinterpret_cast<std::uintptr_t>(trampoline + N + 5));

   std::memcpy(trampoline + N + 1, &back, sizeof(back));

   std::uint8_t jump[N];
   std::memset(jump, 0x90, N);
   jump[0] = 0xE9;
   const auto to_hook = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(hook) -
                                                   reinterpret_cast<std::uintptr_t>(code + 5));

   std::memcpy(jump + 1, &to_hook, sizeof(to_hook));

   DWORD protection = 0;
   VirtualProtect(code, sizeof(jump), PAGE_EXECUTE_READWRITE, &protection);
   std::memcpy(code, jump, sizeof(jump));
   VirtualProtect(code, sizeof(jump), protection, &protection);
   FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

   return trampoline;
}

bool __fastcall force_third_person_hook(void* trackable, void*) noexcept
{
   if (vr::active()) {
      state.stat_third_person_refused += 1;

      return false;
   }

   return state.force_third_person(trackable, nullptr);
}

bool is_soldier_trackable(const void* trackable) noexcept
{
   return trackable &&
          vtable_of(static_cast<const std::uint8_t*>(trackable)) ==
             engine<std::uintptr_t>(exe_soldier_trackable_vtable);
}

void log_class_third_person_refused() noexcept
{
   state.stat_class_third_person_refused += 1;

   if (!std::exchange(state.class_third_person_logged, true)) {
      log(Log_level::info, "VR: the character's class asks for third person only: "
                           "kept in first person.");
   }
}

// A soldier in VR is first person, whatever its class says (see above).
bool __fastcall camera_third_person_hook(void* trackable, void*, int player) noexcept
{
   if (!vr::active() || !is_soldier_trackable(trackable)) {
      return state.camera_third_person(trackable, nullptr, player);
   }

   if (!state.camera_third_person(trackable, nullptr, player)) log_class_third_person_refused();

   return true; // first person
}

bool __fastcall tracker_third_person_hook(void* tracker) noexcept
{
   const bool first = state.tracker_third_person(tracker); // 1 = first person

   if (first || !vr::active()) return first;

   __try {
      const auto* const t = static_cast<const std::uint8_t*>(tracker);

      if (!is_soldier_trackable(field<const void*>(t, tracker_trackable))) return first;

      // First person even when the mode it was set to is third: a session's first
      // character being third person only (a Jawa, an Ewok) leaves the mode at third.
      log_class_third_person_refused();

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return first;
   }
}

// The lit saber is in the hand, ready to deflect by itself (auto block).
bool auto_block_ready() noexcept
{
   return settings().saber_auto_block && state.lit && !state.mesh_blade && !state.thrown &&
          !state.control_lost && state.hand_valid && state.ignition >= 1.0f;
}

// Auto block: the bolt (at `point`, world) hits near the blade or comes from its side.
bool bolt_at_blade(const float* point) noexcept
{
   glm::vec3 soldier{};

   if (!point || !read_soldier_position(soldier)) return false;

   const glm::vec3 bolt{point[0], point[1], point[2]};
   const glm::vec3 head = soldier + state.head_offset;

   // The nearest blade in a hand.
   float to_blade = std::numeric_limits<float>::max();
   glm::vec3 base = soldier + state.base_offset;
   glm::vec3 tip = base + state.dir * state.scaled_blade_length;

   for (int b = 0; b < max_blades; ++b) {
      if (!state.blade_active[b]) continue;

      const glm::vec3 blade_base = soldier + state.blade_bases[b];
      const glm::vec3 dir = state.blade_dirs[b];
      const float length = state.blade_lengths[b];
      const float along = std::clamp(glm::dot(bolt - blade_base, dir), 0.0f, length);
      const float distance = glm::distance(bolt, blade_base + dir * along);

      if (distance < to_blade) {
         to_blade = distance;
         base = blade_base;
         tip = blade_base + dir * length;
      }
   }

   const auto flat = [](const glm::vec3 v) { return glm::vec3{v.x, 0.0f, v.z}; };
   const float from_soldier = glm::length(flat(bolt - soldier));
   const glm::vec3 to_bolt = flat(bolt - head);
   const glm::vec3 to_middle = flat((base + tip) * 0.5f - head);
   float degrees = 180.0f;

   if (glm::length(to_bolt) > 1e-3f && glm::length(to_middle) > 1e-3f) {
      degrees = glm::degrees(std::acos(std::clamp(
         glm::dot(glm::normalize(to_bolt), glm::normalize(to_middle)), -1.0f, 1.0f)));
   }

   const bool at_blade = from_soldier < auto_block_near ? to_blade < auto_block_reach
                                                        : degrees < auto_block_degrees;

   if (state.bolts_logged < 12) {
      state.bolts_logged += 1;

      log_fmt(Log_level::info,
              "VR: bolt at the saber: {:.2f} m from the soldier, {:.2f} m from the blade, "
              "{:.0f} deg from its side: {}.",
              from_soldier, to_blade, degrees, at_blade ? "auto block" : "not blocked");
   }

   return at_blade;
}

bool __fastcall deflecting_hook(void* weapon, void*) noexcept
{
   if (!vr::active() || weapon != state.weapon) return state.deflecting(weapon, nullptr);

   state.stat_deflecting_queries += 1;

   return (state.blocking || auto_block_ready()) && state.deflect_state >= 0 &&
          !state.mesh_blade;
}

bool __fastcall deflect_hook(void* weapon, void*, void* ordnance, void* point,
                             void* arg) noexcept
{
   if (!vr::active() || weapon != state.weapon) {
      return state.deflect(weapon, nullptr, ordnance, point, arg);
   }

   state.stat_deflect_calls += 1;

   if (state.deflect_state < 0 || state.mesh_blade) return false;

   if (!state.blocking) {
      if (!auto_block_ready() || !bolt_at_blade(static_cast<const float*>(point))) {
         return false;
      }

      state.stat_auto_deflects += 1;
   }

   auto* const w = static_cast<std::uint8_t*>(weapon);
   const auto saved_state = field<std::int32_t>(w, weapon_state);

   set_field<std::int32_t>(w, weapon_state, state.deflect_state);

   const bool deflected = state.deflect(weapon, nullptr, ordnance, point, arg);

   set_field<std::int32_t>(w, weapon_state, saved_state);

   if (deflected) {
      state.stat_deflected += 1;
      state.events.deflects += 1;
   }

   return deflected;
}

// Runs the game's fire update with the stand-in state holding this update's blade
// samples. Engine state the update reads is swapped in and put back afterwards.
void swing_update(std::uint8_t* weapon, const float seconds) noexcept
{
   float matrix[16];

   if (!read_owner_matrix(weapon, matrix)) return;

   if (!state.damage_pose_valid) {
      state.damage_bases = state.blade_bases;
      state.damage_dirs = state.blade_dirs;
      state.damage_pose_valid = true;
   }

   const float dt = std::max(seconds, 1e-4f);

   state.fake_time += dt;

   // One attack per blade in a hand, on that blade's edge.
   std::uint32_t attacks = 0;

   for (int b = 0; b < max_blades; ++b) {
      if (!state.blade_active[b]) continue;

      // The blade's offsets are from the soldier's position, as the samples are: only
      // the rotation into the soldier's space is left.
      for (int i = 0; i < samples_per_update; ++i) {
         const float t = static_cast<float>(i + 1) / samples_per_update;
         const glm::vec3 base = glm::mix(state.damage_bases[b], state.blade_bases[b], t);
         const glm::vec3 dir =
            glm::normalize(glm::mix(state.damage_dirs[b], state.blade_dirs[b], t));
         const glm::vec3 position = to_owner(matrix, base + sample_back * dir);
         const glm::vec3 direction = to_owner(matrix, dir);
         float* const sample = state.samples[attacks][i];

         sample[0] = state.fake_time - dt + dt * t;
         sample[1] = position.x;
         sample[2] = position.y;
         sample[3] = position.z;
         sample[4] = direction.x;
         sample[5] = direction.y;
         sample[6] = direction.z;
      }

      // The swing's length: the lit blade's, past the 0.2 m the samples start behind
      // it, as a fraction of the edge's (100 = 1.00). Width: the edge's. Push and
      // damage: the weapon's strongest attack.
      const float full = state.blade_lengths[b];
      const float length = full * state.ignition - sample_back;
      const auto length_scale = static_cast<std::uint32_t>(
         std::clamp(length / full, 0.0f, 10.0f) * 100.0f + 0.5f);

      // A weapon without an energy blade has an edge of no length: its swing's length
      // and width are given in metres instead (x 0.01).
      const auto mesh_length =
         static_cast<std::uint32_t>(std::clamp(length, 0.0f, 10.0f) * 100.0f + 0.5f);
      const auto mesh_width = static_cast<std::uint32_t>(
         std::clamp(state.blade_width, 0.0f, 2.55f) * 100.0f + 0.5f);
      const std::uint32_t size =
         state.mesh_blade ? (std::min(mesh_length, 0x3ffu) << 6) | (mesh_width << 16)
                          : attack_length_from_edge | attack_width_from_edge |
                               (std::min(length_scale, 0x3ffu) << 6) | attack_unit_width;
      const auto edge = static_cast<std::uint32_t>(state.blade_edges[b].index) & 0xfu;
      Fake_attack& attack = state.fake_attacks[attacks];

      state.attack_hands[attacks] = state.blade_edges[b].hand;

      attack.dwords[0] = samples_per_update | attack_lasts_to_end;
      attack.dwords[1] = std::min(state.damage, 0xfffu);
      attack.dwords[2] = size | edge | (std::min(state.push, 0xffu) << 24);
      attack.dwords[3] = static_cast<std::uint32_t>(
         reinterpret_cast<std::uintptr_t>(&state.samples[attacks][0][0]));
      attacks += 1;
   }

   state.damage_bases = state.blade_bases;
   state.damage_dirs = state.blade_dirs;

   if (attacks == 0) return;

   std::memset(state.fake_state, 0, sizeof(state.fake_state));
   set_field<const void*>(state.fake_state, state_attacks, &state.fake_attacks[0]);
   set_field<std::uint32_t>(state.fake_state, state_attack_count, attacks << 28);
   set_field<std::uint32_t>(state.fake_state, state_flags, 0);

   std::memset(state.fake_combo, 0, sizeof(state.fake_combo));
   set_field<const void*>(state.fake_combo, combo_states, state.fake_state);
   set_field<std::uint8_t>(state.fake_combo, combo_state_count, 1);

   state.hits_age += dt;

   // A new swing hits everything again; a long one hits a target again after a while.
   if (std::exchange(state.swing_started, false)) {
      state.clear_hits(weapon);
      state.swing_hits = 0;
      state.swing_left_hits = 0;
      state.hits_age = 0.0f;
   }
   else if (state.swing_hits > 0 && state.hits_age > rehit_seconds) {
      state.clear_hits(weapon);
      state.swing_hits = 0;
      state.swing_left_hits = 0;
      state.hits_age = 0.0f;
      state.stat_rehits += 1;
   }

   const auto saved_combo = field<std::uint8_t*>(weapon, weapon_combo);
   const auto saved_state = field<std::int32_t>(weapon, weapon_state);
   const auto saved_time = field<float>(weapon, weapon_state_time);
   const auto saved_timer = field<float>(weapon, weapon_timer);
   const auto saved_edges = field<std::uint8_t>(weapon, weapon_active_edges);

   set_field<std::uint8_t*>(weapon, weapon_combo, state.fake_combo);
   set_field<std::int32_t>(weapon, weapon_state, 0);
   set_field<float>(weapon, weapon_state_time, state.fake_time);

   state.update_fire(weapon, nullptr, dt);

   set_field<std::uint8_t*>(weapon, weapon_combo, saved_combo);
   set_field<std::int32_t>(weapon, weapon_state, saved_state);
   set_field<float>(weapon, weapon_state_time, saved_time);
   set_field<float>(weapon, weapon_timer, saved_timer);
   set_field<std::uint8_t>(weapon, weapon_active_edges, saved_edges);

   // Objects this swing has hit: the stand-in attacks' hit records keep them (+4 the
   // attack, +8 the count; the next record at +0x2c, FUN_0068bc00).
   int hits = 0;
   int left_hits = 0;
   int records = 0;

   for (const auto* record = field<const std::uint8_t*>(weapon, weapon_hit_records);
        record && records < 64; record = field<const std::uint8_t*>(record, 0x2c), ++records) {
      const auto* const attack = field<const Fake_attack*>(record, 4);

      if (attack >= &state.fake_attacks[0] && attack < &state.fake_attacks[max_blades]) {
         const int count = field<int>(record, 8);

         hits += count;

         if (state.attack_hands[attack - &state.fake_attacks[0]] == 1) left_hits += count;
      }
   }

   if (hits > state.swing_hits) {
      state.stat_hits += hits - state.swing_hits;
      state.events.hits += hits - state.swing_hits;
   }

   if (left_hits > state.swing_left_hits) {
      state.events.left_hits += left_hits - state.swing_left_hits;
   }

   state.swing_hits = hits;
   state.swing_left_hits = left_hits;
}

std::uint32_t __fastcall update_hook(void* weapon, void*, float seconds) noexcept
{
   const std::uint32_t result = state.update(weapon, nullptr, seconds);

   if (vr::active() && weapon == state.weapon) {
      state.stat_weapon_updates += 1;

      // Block stance: into the deflecting state (again, if the combo left it) while
      // blocking, back to idle when done.
      auto* const w = static_cast<std::uint8_t*>(weapon);
      const int current = field<std::int32_t>(w, weapon_state);

      if (state.blocking && state.deflect_state >= 0 && state.leave_state) {
         if (current != state.deflect_state) {
            state.leave_state(weapon);
            state.enter_state(weapon, nullptr, state.deflect_state);
            state.stat_block_stances += 1;
         }

         state.in_block_stance = true;
      }
      else if (state.in_block_stance) {
         state.in_block_stance = false;

         if (current == state.deflect_state && state.leave_state) {
            state.leave_state(weapon);
            state.enter_state(weapon, nullptr, state.idle_state);
         }
      }

      if (state.lit && !state.thrown && state.swinging && state.hand_valid &&
          state.damage > 0 && state.edges > 0) {
         state.stat_swing_updates += 1;
         swing_update(static_cast<std::uint8_t*>(weapon), seconds);
      }
      else {
         state.damage_pose_valid = false;
      }
   }

   return result;
}

// The player's saber starts no combo attack: its damage is the blade's (update_hook).
std::uint32_t __fastcall update_fire_hook(void* weapon, void*, float seconds) noexcept
{
   if (!vr::active() || weapon != state.weapon) {
      return state.update_fire(weapon, nullptr, seconds);
   }

   auto* const w = static_cast<std::uint8_t*>(weapon);
   set_field<float>(w, weapon_timer, field<float>(w, weapon_timer) + seconds);

   return 1;
}

// The render's flags bit that leaves the blade trails alone (FUN_0068a410 not called).
constexpr std::uint32_t render_no_trail = 0x100;

// A skeleton (the render's second argument) keeps its bones' matrices in a hash table
// at +4 keyed by the bone name's hash (FUN_00726e00 with size 0x100: 0x80 keys, then
// their matrix pointers; probed downwards, wrapping). The hash is the munged models'
// CRC-32 of the name lowercased (MSB first, polynomial 0x04c11db7, inverted at both
// ends; the exe looks up 0x8afdf5e4 = "bone_head" itself). FNV-1 (FUN_00726e50, the
// one names elsewhere use) never matched: "the player's skeleton has no bone_head".
constexpr std::uint32_t skeleton_bone_slots = 0x80;

constexpr auto bone_hash(const char* name) noexcept -> std::uint32_t
{
   std::uint32_t crc = 0xffffffffu;

   for (; *name; ++name) {
      const auto c = static_cast<std::uint32_t>(*name);

      crc ^= (c >= 'A' && c <= 'Z' ? c | 0x20u : c) << 24;

      for (int bit = 0; bit < 8; ++bit) {
         crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
      }
   }

   return ~crc;
}

constexpr std::uint32_t head_bone = bone_hash("bone_head");

static_assert(head_bone == 0x8afdf5e4u);

// The bone's matrix (D3DX, the skeleton's model space), or null.
auto skeleton_bone(const void* skeleton, const std::uint32_t hash) noexcept -> const float*
{
   __try {
      const auto* const keys =
         reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(skeleton) + 4);
      const std::uint32_t* slot = keys + ((skeleton_bone_slots - 1) & hash);
      std::uint32_t key = *slot;

      for (std::uint32_t probes = 0; probes < skeleton_bone_slots; ++probes) {
         if (key == hash) return reinterpret_cast<const float*>(slot[skeleton_bone_slots]);
         if (key == 0) break;
         if (slot <= keys) slot += skeleton_bone_slots;

         key = slot[-1];
         slot -= 1;
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
   }

   return nullptr;
}

// The body's render (matrix at the soldier's feet, its own skeleton; the first-person
// render's matrix is at the camera): how high the head bone is over the feet.
void note_head_bone(const float* matrix, const void* skeleton) noexcept
{
   // Every render of every soldier comes here: the player's position read once a frame.
   if (state.feet_frame != state.frame) {
      std::int32_t soldier_state = -1;

      state.feet_frame = state.frame;
      state.feet_valid = read_player_soldier(state.feet, soldier_state);
   }

   if (!state.feet_valid || !matrix || !skeleton) return;

   const glm::vec3 soldier = state.feet;

   const glm::vec3 origin{matrix[12], matrix[13], matrix[14]};

   if (glm::distance(origin, soldier) > 0.3f) return;

   const float* const bone = skeleton_bone(skeleton, head_bone);

   if (!bone) {
      if (!std::exchange(state.head_bone_missing_logged, true)) {
         log(Log_level::info, "VR: the player's skeleton has no bone_head.");
      }

      return;
   }

   __try {
      // D3DX rows: the bone's origin times the soldier's matrix.
      const float y = bone[12] * matrix[1] + bone[13] * matrix[5] + bone[14] * matrix[9] +
                      matrix[13];
      const float height = y - soldier.y;

      if (height < 0.2f || height > 3.0f) return;

      state.head_bone_height = height;
      state.head_bone_frame = state.frame;
      state.head_bone_valid = true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return;
   }

   if (!std::exchange(state.head_bone_logged, true)) {
      log_fmt(Log_level::info, "VR: the player's head bone is {:.2f} m over its feet.",
              state.head_bone_height);
   }
}

// A stand-in skeleton (see skeleton_bone) for the player's saber render with dual
// sabers: the hilts' bones (the class's hardpoint bones) at the hands' hilt matrices
// (world), drawn with an identity matrix: the render draws hilt i at its bone x the
// matrix, and only hilt 0 without a skeleton.
struct Hand_skeleton {
   std::uint32_t header = 0;
   std::uint32_t keys[skeleton_bone_slots] = {};
   const float* matrices[skeleton_bone_slots] = {};
};

Hand_skeleton hand_skeleton;
float hand_hilts[2][16] = {};
constexpr float identity_matrix[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                       0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

// Probed the way skeleton_bone looks a bone up.
void add_bone(Hand_skeleton& skeleton, const std::uint32_t hash, const float* matrix) noexcept
{
   std::uint32_t slot = hash & (skeleton_bone_slots - 1);

   for (std::uint32_t probes = 0; probes < skeleton_bone_slots; ++probes) {
      if (skeleton.keys[slot] == 0 || skeleton.keys[slot] == hash) {
         skeleton.keys[slot] = hash;
         skeleton.matrices[slot] = matrix;

         return;
      }

      slot = slot == 0 ? skeleton_bone_slots - 1 : slot - 1;
   }
}

// D3DX rows: the images of the hilt's x, y, z axes, then the origin.
void hilt_matrix(const glm::mat3& r, const glm::vec3 origin, float (&m)[16]) noexcept
{
   const float rows[16] = {r[0].x, r[0].y, r[0].z, 0.0f, r[1].x, r[1].y, r[1].z, 0.0f,
                           r[2].x, r[2].y, r[2].z, 0.0f, origin.x, origin.y, origin.z, 1.0f};

   std::memcpy(m, rows, sizeof(m));
}

// The matrix and skeleton the player's saber is drawn with in the hands: the right
// hilt's matrix alone, or with dual sabers in both hands the stand-in skeleton.
void hand_render_args(const glm::vec3 soldier, const float*& matrix,
                      const void*& skeleton) noexcept
{
   hilt_matrix(state.hilt_rotation * state.weapon_scale, soldier + state.hilt_offset, hand_hilts[0]);
   matrix = hand_hilts[0];
   skeleton = nullptr;
   state.drawn_left_hilt_valid = false;

   if (!state.dual || !state.left_hand_valid) return;

   const glm::vec3 left = soldier + state.left_hilt_offset;

   hilt_matrix(state.left_hilt_rotation * state.weapon_scale, left, hand_hilts[1]);
   hand_skeleton = {};
   add_bone(hand_skeleton, state.hardpoint_bones[0], hand_hilts[0]);
   add_bone(hand_skeleton, state.hardpoint_bones[1], hand_hilts[1]);
   matrix = identity_matrix;
   skeleton = &hand_skeleton;
   state.drawn_left_hilt = left;
   state.drawn_left_hilt_valid = true;
}

// While the saber is drawn in the hand: the hilt matrix is scaled with the body, and the
// render draws each blade as long as its edge x that scale; the class's edge lengths are
// divided by the scale for the call (put back after), so the blades keep the game's
// lengths, as their hits do (scaled_blade_length).
class Hand_blade_lengths {
public:
   Hand_blade_lengths() noexcept
   {
      if (!state.edges_writable || !state.weapon_class_ ||
          std::abs(state.weapon_scale - 1.0f) < 1e-3f) {
         return;
      }

      edges = field<std::uint8_t*>(state.weapon_class_, class_edges);
      count = std::min(state.edges, max_blades);

      for (int i = 0; i < count; ++i) {
         auto* const length = reinterpret_cast<float*>(edges + i * edge_size + 0x20);

         saved[i] = *length;
         *length = saved[i] / state.weapon_scale;
      }
   }

   ~Hand_blade_lengths()
   {
      for (int i = 0; i < count; ++i) {
         *reinterpret_cast<float*>(edges + i * edge_size + 0x20) = saved[i];
      }
   }

   Hand_blade_lengths(const Hand_blade_lengths&) = delete;
   Hand_blade_lengths& operator=(const Hand_blade_lengths&) = delete;

private:
   std::uint8_t* edges = nullptr;
   int count = 0;
   float saved[max_blades]{};
};

// The player's weapon drawn at the controller: the hilt matrix, no skeleton (dual
// sabers: both hands' hilts through a stand-in skeleton).
void __fastcall render_hook(void* weapon, void*, const float* matrix, const void* skeleton,
                            std::uint32_t a, std::uint32_t flags, std::uint32_t draw_hilt) noexcept
{
   if (!vr::active() || weapon != state.weapon) {
      return state.render(weapon, nullptr, matrix, skeleton, a, flags, draw_hilt);
   }

   if (skeleton) note_head_bone(matrix, skeleton);

   // No trail for the player's saber: the weapon keeps one trail per blade edge, fed by
   // every render. The hand's (at the controller) and the thrown saber's (from where the
   // body's hand is, then along its path) fed the same one: a wide ribbon from the
   // controller along the throw.
   flags |= render_no_trail;

   // Without a skeleton: the thrown saber in flight (OrdnanceMeleeThrow), as the game
   // draws it.
   if (!skeleton) {
      state.stat_flight_renders += 1;

      return state.render(weapon, nullptr, matrix, skeleton, a, flags, draw_hilt);
   }

   // Knocked down or held: out of the hand (the first-person model isn't drawn either).
   if (state.control_lost && !read_saber_thrown(static_cast<const std::uint8_t*>(weapon))) {
      state.stat_hidden_renders += 1;
      state.game_saber_frame = state.frame;
      state.game_saber_seen = true;

      return;
   }

   if (!state.hand_valid) {
      return state.render(weapon, nullptr, matrix, skeleton, a, flags, draw_hilt);
   }

   // The saber is away: nothing in the hand. (The game's hand render is meant to leave
   // the hilt out then, but on the first throw of a life the hilt stayed in the hand.)
   if (read_saber_thrown(static_cast<const std::uint8_t*>(weapon))) {
      state.game_saber_frame = state.frame;
      state.game_saber_seen = true;
      state.stat_thrown_hand_renders += 1;

      return;
   }

   glm::vec3 soldier{};

   if (!read_soldier_position(soldier)) {
      return state.render(weapon, nullptr, matrix, skeleton, a, flags, draw_hilt);
   }

   if (state.render_calls_logged < 4) {
      state.render_calls_logged += 1;

      log_fmt(Log_level::info,
              "VR: the player's saber render: matrix at ({:.2f}, {:.2f}, {:.2f}) from the "
              "soldier, skeleton {}, flags 0x{:08X}, hilt {}.",
              matrix[12] - soldier.x, matrix[13] - soldier.y, matrix[14] - soldier.z,
              skeleton ? "given" : "none", flags, draw_hilt & 0xff);
   }

   const glm::vec3 origin = soldier + state.hilt_offset;
   const float* hilt = nullptr;
   const void* hilts = nullptr;

   hand_render_args(soldier, hilt, hilts);

   state.stat_renders_in_hand += 1;
   state.game_saber_frame = state.frame;
   state.game_saber_seen = true;
   state.drawn_hilt = origin;
   state.drawn_hilt_frame = state.frame;
   state.drawn_hilt_valid = true;

   const Hand_blade_lengths blade_lengths;

   state.render(weapon, nullptr, hilt, hilts, a, flags, draw_hilt);
}

// Calls WeaponMeleeClass's render (thiscall, 5 stack arguments, ret 0x14) with XMM1 set.
__declspec(naked) void __fastcall call_class_render(void* /*weapon_class*/, void* /*edx*/,
                                                     const float* /*matrix*/,
                                                     const void* /*skeleton*/,
                                                     std::uint32_t, std::uint32_t,
                                                     std::uint32_t, float /*length_factor*/)
{
   __asm {
      movss xmm1, dword ptr [esp + 24]
      push dword ptr [esp + 20]
      push dword ptr [esp + 20]
      push dword ptr [esp + 20]
      push dword ptr [esp + 20]
      push dword ptr [esp + 20]
      call dword ptr [class_render]
      ret 0x18
   }
}

// The player's saber: drawn at the controller (hilt matrix, no skeleton), as long as
// it's lit. Everyone else's as the game does.
void __fastcall class_render_hook(void* weapon_class, void*, const float* matrix,
                                  const void* skeleton, std::uint32_t a, std::uint32_t b,
                                  std::uint32_t c) noexcept
{
   const float length_factor = render_length_factor;

   state.stat_renders += 1;

   // Seen once as the player died (Mos Eisley hero assault): the class pointer was a
   // stack address and the render's edge loop ran off the stack (likely memory
   // corrupted elsewhere: an overfull draw array overwrote the draw queues). Skip it.
   ULONG_PTR stack_low = 0;
   ULONG_PTR stack_high = 0;

   GetCurrentThreadStackLimits(&stack_low, &stack_high);

   if (const auto address = reinterpret_cast<ULONG_PTR>(weapon_class);
       address >= stack_low && address < stack_high) {
      if (!std::exchange(state.stack_class_logged, true)) {
         log_fmt(Log_level::warning,
                 "VR: a melee weapon render was given a class on the stack (0x{:08x}); "
                 "not drawn.",
                 address);
      }

      return;
   }

   glm::vec3 soldier{};

   if (vr::active()) note_bash_candidate(std::exchange(render_element, nullptr), matrix);

   if (vr::active() && state.weapon && skeleton && weapon_class == state.weapon_class_) {
      note_head_bone(matrix, skeleton);
   }

   if (vr::active() && state.weapon && state.hand_valid &&
       weapon_class == state.weapon_class_ && read_soldier_position(soldier)) {
      const float distance = glm::distance(soldier, glm::vec3{matrix[12], matrix[13], matrix[14]});

      if (!std::exchange(state.render_distance_logged, true)) {
         log_fmt(Log_level::info,
                 "VR: the player's saber is drawn {:.2f} m from the soldier's position.",
                 distance);
      }

      if (distance < 1.0f) {
         const glm::vec3 origin = soldier + state.hilt_offset;
         const float* hilt = nullptr;
         const void* hilts = nullptr;

         hand_render_args(soldier, hilt, hilts);

         state.stat_renders_in_hand += 1;
         state.game_saber_frame = state.frame;
         state.game_saber_seen = true;
         state.drawn_hilt = origin;
         state.drawn_hilt_frame = state.frame;
         state.drawn_hilt_valid = true;

         const Hand_blade_lengths blade_lengths;

         call_class_render(weapon_class, nullptr, hilt, hilts, a, b, c, state.ignition);

         return;
      }
   }

   call_class_render(weapon_class, nullptr, matrix, skeleton, a, b, c, length_factor);
}

// The call site's entry: keeps XMM1 (the blade length factor) and goes on to the hook.
__declspec(naked) void class_render_stub()
{
   __asm {
      movss dword ptr [render_length_factor], xmm1
      mov dword ptr [render_element], edi
      jmp class_render_hook
   }
}

// The other weapons' call site's entry: keeps the element (EDI), goes on to the hook.
__declspec(naked) void gun_class_render_stub()
{
   __asm {
      mov dword ptr [render_element], edi
      jmp gun_class_render_hook
   }
}

auto decode_color(const std::uint32_t argb) noexcept -> glm::vec3
{
   const glm::vec3 rgb{static_cast<float>((argb >> 16) & 0xff),
                       static_cast<float>((argb >> 8) & 0xff),
                       static_cast<float>(argb & 0xff)};
   const float peak = std::max({rgb.r, rgb.g, rgb.b});

   if (peak <= 0.0f) return {0.2f, 0.4f, 1.0f};

   return rgb / peak;
}

// A rotation taking `from` (unit) to `to` (unit) with the second axes as close as they
// can be: hilt space -> world.
auto basis(const glm::vec3 axis, const glm::vec3 hint) noexcept -> glm::mat3
{
   glm::vec3 second = hint - glm::dot(hint, axis) * axis;

   if (glm::length(second) < 1e-4f) {
      second = std::abs(axis.x) < 0.9f ? glm::vec3{1.0f, 0.0f, 0.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
      second -= glm::dot(second, axis) * axis;
   }

   second = glm::normalize(second);

   return glm::mat3{axis, second, glm::cross(axis, second)};
}

}

void install_hooks() noexcept
{
   if (state.hooked) return;

   state.hooked = true;
   state.image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

   const std::uintptr_t force_third_person =
      patch_vtable(exe_soldier_trackable_vtable, force_third_person_slot,
                   exe_force_third_person, reinterpret_cast<const void*>(&force_third_person_hook));

   if (!force_third_person) {
      log(Log_level::warning, "VR: unknown game executable, melee weapons keep the game's "
                              "third-person camera.");

      return;
   }

   state.force_third_person =
      reinterpret_cast<State::Force_third_person_fn>(force_third_person);

   state.camera_third_person = reinterpret_cast<State::Camera_third_person_fn>(
      detour(exe_camera_third_person, camera_third_person_prologue,
             reinterpret_cast<const void*>(&camera_third_person_hook)));

   if (state.camera_third_person) {
      state.tracker_third_person = reinterpret_cast<State::Tracker_third_person_fn>(
         detour(exe_tracker_third_person, tracker_third_person_prologue,
                reinterpret_cast<const void*>(&tracker_third_person_hook)));
   }

   log_fmt(Log_level::info, "VR: third-person-only characters: camera {}, tracker {}.",
           state.camera_third_person ? "hooked" : "NOT hooked",
           state.tracker_third_person ? "hooked" : "NOT hooked");

   // The clear function is called directly: check its first bytes (mov edx, ecx).
   const auto* const clear = engine<const std::uint8_t*>(exe_clear_hits);

   if (clear[0] == 0x8b && clear[1] == 0xd1) {
      state.clear_hits =
         reinterpret_cast<State::Clear_hits_fn>(engine<std::uintptr_t>(exe_clear_hits));
      state.update_fire = reinterpret_cast<State::Update_fn>(
         patch_vtable(exe_melee_vtable, update_fire_slot, exe_update_fire,
                      reinterpret_cast<const void*>(&update_fire_hook)));

      if (state.update_fire) {
         state.update = reinterpret_cast<State::Update_fn>(
            patch_vtable(exe_melee_vtable, update_slot, exe_update,
                         reinterpret_cast<const void*>(&update_hook)));
      }
   }

   state.deflect = reinterpret_cast<State::Deflect_fn>(
      patch_vtable(exe_melee_vtable, deflect_slot, exe_deflect,
                   reinterpret_cast<const void*>(&deflect_hook)));

   // Entered directly: check their first bytes (push esi / push ebp).
   const auto* const leave = engine<const std::uint8_t*>(exe_leave_state);
   const auto* const enter = engine<const std::uint8_t*>(exe_enter_state);

   if (leave[0] == 0x56 && enter[0] == 0x55) {
      state.leave_state =
         reinterpret_cast<State::Leave_state_fn>(engine<std::uintptr_t>(exe_leave_state));
      state.enter_state =
         reinterpret_cast<State::Enter_state_fn>(engine<std::uintptr_t>(exe_enter_state));
   }

   state.render = reinterpret_cast<State::Render_fn>(
      patch_vtable(exe_melee_vtable, render_slot, exe_render,
                   reinterpret_cast<const void*>(&render_hook)));

   if (state.deflect) {
      state.deflecting = reinterpret_cast<State::Deflecting_fn>(
         patch_vtable(exe_melee_vtable, deflecting_slot, exe_deflecting,
                      reinterpret_cast<const void*>(&deflecting_hook)));
   }

   class_render = patch_call(exe_class_render_call, exe_class_render,
                             reinterpret_cast<const void*>(&class_render_stub));

   damage_record_fn = engine<std::uintptr_t>(exe_damage_record);
   find_objects_fn = engine<std::uintptr_t>(exe_find_objects);

   state.gun_class_render = reinterpret_cast<State::Gun_render_fn>(
      patch_call(exe_gun_class_render_call, exe_gun_class_render,
                 reinterpret_cast<const void*>(&gun_class_render_stub)));
   state.weapon_render = reinterpret_cast<State::Gun_render_fn>(detour(
      exe_weapon_render, weapon_render_prologue, reinterpret_cast<const void*>(&weapon_render_hook)));

   log_fmt(Log_level::info, "VR: body skeletons for the head's height: gun class render {}, "
                            "weapon render {}.",
           state.gun_class_render ? "hooked" : "NOT hooked",
           state.weapon_render ? "hooked" : "NOT hooked");

   state.soldier_render = reinterpret_cast<State::Soldier_render_fn>(
      detour(exe_soldier_render, soldier_render_prologue,
             reinterpret_cast<const void*>(&soldier_render_hook)));

   if (!state.soldier_render) {
      log(Log_level::warning, "VR: soldiers' render not hooked: the player's own soldier "
                              "stays visible on turrets.");
   }

   state.cannon_fire = reinterpret_cast<State::Fire_fn>(patch_call(
      exe_cannon_fire_call, exe_cannon_fire, reinterpret_cast<const void*>(&cannon_fire_hook)));
   state.laser_fire = reinterpret_cast<State::Fire_fn>(patch_call(
      exe_laser_fire_call, exe_laser_fire, reinterpret_cast<const void*>(&laser_fire_hook)));
   flash_draw = patch_call(exe_flash_draw_call, exe_flash_draw,
                           reinterpret_cast<const void*>(&flash_draw_stub));

   log_fmt(Log_level::info, "VR: the gun's muzzle flash {}.",
           flash_draw ? "hooked" : "NOT hooked");

   state.effect_attach = reinterpret_cast<State::Effect_attach_fn>(
      detour(exe_effect_attach, effect_attach_prologue,
             reinterpret_cast<const void*>(&effect_attach_hook)));
   state.effect_start = reinterpret_cast<State::Effect_start_fn>(
      detour(exe_effect_start, effect_start_prologue,
             reinterpret_cast<const void*>(&effect_start_hook)));
   state.effect_play = reinterpret_cast<State::Effect_play_fn>(
      detour(exe_effect_play, effect_play_prologue,
             reinterpret_cast<const void*>(&effect_play_hook)));
   state.animation_set = reinterpret_cast<State::Animation_set_fn>(
      detour(exe_animation_set, animation_set_prologue,
             reinterpret_cast<const void*>(&animation_set_hook)));
   state.emitter_fire = reinterpret_cast<State::Emitter_fire_fn>(
      detour(exe_emitter_fire, emitter_fire_prologue,
             reinterpret_cast<const void*>(&emitter_fire_hook)));

   log_fmt(Log_level::info,
           "VR: the gun's effects (charge-up): tied {}, started {}, played {}; emitter "
           "shots (arcs) {}; animation changes {}.",
           state.effect_attach ? "hooked" : "NOT hooked",
           state.effect_start ? "hooked" : "NOT hooked",
           state.effect_play ? "hooked" : "NOT hooked",
           state.emitter_fire ? "hooked" : "NOT hooked",
           state.animation_set ? "guarded" : "NOT guarded");

   log_fmt(Log_level::info,
           "VR: shots from the gun in the hand: cannons {}, the other guns {}.",
           state.cannon_fire ? "hooked" : "NOT hooked", state.laser_fire ? "hooked" : "NOT hooked");
   state.render_hooked = class_render != 0 || state.render != nullptr;

   log_fmt(Log_level::info, "VR: melee hooked: first person with melee weapons{}{}{}.",
           state.deflecting ? ", saber blocks" : "", state.update ? ", saber swings" : "",
           state.render_hooked ? ", the game's saber in the hand" : "");
}

void load_tuning(const wchar_t* path) noexcept
{
   FILE* file = nullptr;

   if (_wfopen_s(&file, path, L"r") != 0 || !file) return;

   float pitch = 0.0f, grip = 0.0f;
   const int read = std::fscanf(file, "%f %f", &pitch, &grip);

   std::fclose(file);

   if (read >= 1) state.pitch_degrees = pitch;
   if (read >= 2) state.grip_offset = std::clamp(grip, -0.5f, 0.5f);

   log_fmt(Log_level::info,
           "VR: saber tuning from bf2vr_saber.txt: blade tilted {:.0f} degrees up from the "
           "aim, hilt moved {:.2f} m along it.",
           state.pitch_degrees, state.grip_offset);
}

void update(const Frame_input& input) noexcept
{
   // Effects tied to the player's gun read effect_point while they live: kept at the gun.
   if (state.effect_aimer && (!vr::active() || !is_player_aimer(state.effect_aimer) ||
                              !move_effect_point(state.effect_aimer))) {
      state.effect_aimer = nullptr;
   }

   state.stat_frames += 1;
   state.frame += 1;
   state.player_vtable = 0;
   state.soldier_state = -1;

   std::uint8_t* const weapon = state.hooked ? read_player_melee_weapon() : nullptr;

   state.in_turret = state.hooked && read_player_in_turret();
   state.hide_soldiers = state.soldier_render && input.hide_soldiers_near.has_value();

   // The player's soldier entity, remembered on foot, read on a turret (the same entity,
   // its vtable unchanged; anything else and it's forgotten).
   if (state.hooked && !state.in_turret) {
      glm::vec3 feet{};
      std::int32_t soldier_state = -1;
      std::uint8_t* owner = nullptr;

      if (read_player_soldier(feet, soldier_state, &owner) && owner) {
         state.body_owner = owner;
         state.body_owner_vtable = read_vtable(owner);
      }
   }

   state.turret_body_valid = state.in_turret && read_turret_body(state.turret_body);

   if (state.hide_soldiers) {
      state.hide_soldiers_near = *input.hide_soldiers_near;
      state.hide_soldiers_radius = input.hide_soldiers_radius;
   }

   if (state.hooked) read_game_screen(state.game_screen);

   if (std::strcmp(state.game_screen, state.logged_game_screen) != 0) {
      std::memcpy(state.logged_game_screen, state.game_screen, sizeof(state.game_screen));

      log_fmt(Log_level::info, "VR: game interface screen '{}'.", state.game_screen);
   }
   state.secondary_grenade = state.hooked && !weapon &&
                             read_secondary_weapon_vtable() == exe_grenade_vtable;

   if (state.hooked) update_bash(input, weapon != nullptr);

   if (state.in_turret != state.logged_in_turret) {
      state.logged_in_turret = state.in_turret;

      log(Log_level::info, state.in_turret ? "VR: the player mans a stationary turret."
                                           : "VR: the player left the turret.");

      if (!state.in_turret) {
         log_fmt(Log_level::info,
                 "VR: soldier draws left out on the turret: {} of {} soldier renders "
                 "({} render calls in all, nearest {:.2f} m from the eyes); the player's "
                 "soldier entity {}.",
                 std::exchange(state.stat_hidden_soldiers, 0),
                 std::exchange(state.stat_turret_soldier_renders, 0),
                 std::exchange(state.stat_soldier_render_calls, 0),
                 state.stat_nearest_soldier,
                 state.body_owner ? "known" : "NOT known");
      }
   }

   if (state.player_vtable != state.logged_player_vtable) {
      state.logged_player_vtable = state.player_vtable;

      log_fmt(Log_level::info, "VR: the player controls an entity with vtable 0x{:08X}{}.",
              state.player_vtable ? state.player_vtable - state.image_base + exe_image_base : 0,
              state.player_vtable == engine<std::uintptr_t>(exe_soldier_trackable_vtable)
                 ? " (a soldier)"
                 : "");
   }

   // Logged for telling knockdowns and stuns apart.
   if (state.soldier_state != state.logged_soldier_state) {
      log_fmt(Log_level::info, "VR: the player's soldier state {} -> {}.",
              state.logged_soldier_state, state.soldier_state);
      state.logged_soldier_state = state.soldier_state;
   }

   if (weapon != state.weapon) {
      state.weapon = weapon;
      state.weapon_class_ = nullptr;
      state.lit = weapon != nullptr;
      state.ignition = weapon && read_weapon_lit(weapon) ? 1.0f : 0.0f;
      state.swinging = false;
      state.swing_dir_valid = false;
      state.prev_tip_valid = false;
      state.damage_pose_valid = false;
      state.render_distance_logged = false;
      state.render_calls_logged = 0;
      state.in_block_stance = false;

      if (weapon) {
         Weapon_info info{};

         if (read_weapon_info(weapon, info)) {
            state.weapon_class_ = info.weapon_class;
            state.raw_color = info.raw_color;
            state.color = decode_color(info.raw_color);
            state.mesh_blade = info.edges > 0 && info.length < mesh_blade_min_length;
            state.blade_length = info.length > 0.05f ? info.length : 1.0f;
            state.blade_width = info.width > 0.005f ? info.width : 0.08f;
            state.edge_position = {info.edge_position[0], info.edge_position[1],
                                   info.edge_position[2]};
            state.edge_direction = {info.edge_direction[0], info.edge_direction[1],
                                    info.edge_direction[2]};
            state.edge_hardpoint = info.edge_hardpoint;

            if (glm::length(state.edge_direction) < 1e-3f) state.edge_direction = {0.0f, 1.0f, 0.0f};

            state.edge_direction = glm::normalize(state.edge_direction);

            if (state.mesh_blade) {
               state.blade_length = mesh_blade_length;
               state.blade_width = mesh_blade_width;

               if (glm::length(state.edge_position) < 1e-3f &&
                   state.edge_direction.z > 0.999f) {
                  state.edge_direction = mesh_blade_direction;
               }
            }
            state.edges = info.edges;
            state.edges_writable = false;

            if (info.edges > 0 && !state.mesh_blade) {
               MEMORY_BASIC_INFORMATION memory{};
               const auto* const edges =
                  field<const std::uint8_t*>(info.weapon_class, class_edges);
               constexpr DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                                          PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;

               state.edges_writable =
                  VirtualQuery(edges, &memory, sizeof(memory)) == sizeof(memory) &&
                  memory.State == MEM_COMMIT && (memory.Protect & writable) != 0 &&
                  (memory.Protect & PAGE_GUARD) == 0 &&
                  edges + std::min(info.edges, max_blades) * edge_size <=
                     static_cast<const std::uint8_t*>(memory.BaseAddress) + memory.RegionSize;
            }

            state.damage = info.damage;
            state.push = info.push;

            // The blades in the hands: edge 0 (the right hand's), the next edges on
            // hilt 0, and with dual sabers (hilt 1 on a bone of its own) hilt 1's.
            state.hardpoints = info.hardpoints;

            for (int i = 0; i < max_hardpoints; ++i) {
               state.hardpoint_bones[i] = info.hardpoint_bones[i];
            }

            state.dual = info.hardpoints >= 2 && info.hardpoint_bones[0] != 0 &&
                         info.hardpoint_bones[1] != 0 &&
                         info.hardpoint_bones[1] != info.hardpoint_bones[0];
            state.blade_count = 0;

            bool left_blade = false;

            for (int i = 0; i < std::min(info.edges, max_blades); ++i) {
               const Weapon_info::Edge& e = info.all_edges[i];
               const int hand = i == 0              ? 0
                                : e.hardpoint == 0  ? 0
                                : e.hardpoint == 1 && state.dual ? 1
                                                                 : -1;

               if (i > 0 && (hand < 0 || state.mesh_blade || e.length < mesh_blade_min_length)) {
                  continue;
               }

               glm::vec3 direction{e.direction[0], e.direction[1], e.direction[2]};

               direction = glm::length(direction) < 1e-3f ? glm::vec3{0.0f, 1.0f, 0.0f}
                                                          : glm::normalize(direction);

               if (hand == 1 && !std::exchange(left_blade, true)) {
                  state.left_edge_direction = direction;
               }

               state.blade_edges[state.blade_count++] = State::Blade_edge{
                  .index = i,
                  .hand = hand,
                  .position = {e.position[0], e.position[1], e.position[2]},
                  .direction = direction,
                  .length = e.length};
            }

            if (!left_blade) state.dual = false;

            log_fmt(Log_level::info,
                    "VR: melee weapon hilts: {} (bones 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}), "
                    "{} blade(s) in the hands{}{}.",
                    info.hardpoints, info.hardpoint_bones[0], info.hardpoint_bones[1],
                    info.hardpoint_bones[2], info.hardpoint_bones[3], state.blade_count,
                    state.dual ? ", dual: hilt 1 in the left hand" : "",
                    info.render_class != info.weapon_class
                       ? " (the render's class differs from the weapon's: hilts from the render's)"
                       : "");

            for (int i = 0; i < std::min(info.edges, max_blades); ++i) {
               const Weapon_info::Edge& e = info.all_edges[i];

               log_fmt(Log_level::info,
                       "VR: melee weapon edge {}: hilt {}, {:.2f} m from ({:.3f}, {:.3f}, "
                       "{:.3f}) along ({:.3f}, {:.3f}, {:.3f}).",
                       i, e.hardpoint, e.length, e.position[0], e.position[1], e.position[2],
                       e.direction[0], e.direction[1], e.direction[2]);
            }
            state.deflect_state = info.deflect_state;
            state.idle_state = info.idle_state;
         }
         else {
            state.edges = 0;
            state.mesh_blade = false;
            state.damage = 0;
            state.deflect_state = -1;
            state.blade_count = 0;
            state.dual = false;
         }

         log_fmt(Log_level::info,
                 "VR: melee weapon in hand: {} edge(s), blade {:.2f} m x {:.3f} m from "
                 "({:.3f}, {:.3f}, {:.3f}) along ({:.3f}, {:.3f}, {:.3f}) at hardpoint {}, "
                 "colour 0x{:08X}, strongest attack {} damage / {:.1f} push, deflecting "
                 "state {}, idle state {}{}.",
                 state.edges, state.blade_length, state.blade_width, state.edge_position.x,
                 state.edge_position.y, state.edge_position.z, state.edge_direction.x,
                 state.edge_direction.y, state.edge_direction.z, state.edge_hardpoint,
                 state.raw_color, state.damage, state.push * 0.1f, state.deflect_state,
                 state.idle_state,
                 state.mesh_blade ? "; no energy blade: the model is the blade, no blocks"
                                  : "");
      }
      else {
         log(Log_level::info, "VR: no melee weapon in hand.");
      }
   }

   if (!weapon) {
      state.hand_valid = false;
      state.blocking = false;
      state.thrown = false;
      state.control_lost = false;
      state.dead = false;
      state.secondary_fire_held = false;

      return;
   }

   state.thrown = read_saber_thrown(weapon);
   state.dead = state.soldier_state >= soldier_dying;
   state.control_lost = player_control_loss() != Control_loss::none || state.dead;

   // Saber throw diagnostics: the thrower's state changes, and what the secondary fire
   // button finds.
   if (const std::int64_t thrower = read_thrower_flags(weapon);
       thrower != state.logged_thrower_flags) {
      log_fmt(Log_level::info, "VR: saber thrower flags {} -> {}{}.",
              state.logged_thrower_flags, thrower, state.thrown ? " (thrown)" : "");
      state.logged_thrower_flags = thrower;
   }

   if (input.secondary_fire && !state.secondary_fire_held) {
      log_fmt(Log_level::info,
              "VR: secondary fire with the saber: secondary weapon vtable 0x{:08X}, "
              "thrower flags {}.",
              read_secondary_weapon_vtable(), state.logged_thrower_flags);
   }

   state.secondary_fire_held = input.secondary_fire;

   // The blade is always lit (the weapon's own lit bit follows: the game extends the
   // blade and plays its sound); a thrown saber is the game's.
   if (!state.thrown) set_weapon_lit(weapon, true);

   const float dt = std::clamp(input.dt, 0.0f, 0.1f);

   state.ignition = std::clamp(state.ignition + (state.lit ? dt : -dt) / ignite_seconds,
                               0.0f, 1.0f);

   glm::vec3 soldier{};

   state.hand_valid = input.hand.has_value() && read_soldier_position(soldier);

   if (!state.hand_valid) {
      state.prev_tip_valid = false;
      state.swinging = false;
      state.blocking = false;

      return;
   }

   // The blade along the controller's aim (-back), tilted up about the hand's right axis;
   // the hilt turned so its blade edge points that way, rolled with the hand.
   // The saber is as big as the body is (body scale: a small hero's is small).
   const Hand& hand = *input.hand;
   const glm::vec3 dir = blade_direction(hand.rotation);
   const float scale = std::clamp(input.scale, 0.1f, 2.0f);

   // Only the hilt: the blades keep the game's lengths (it sizes them per character
   // already: Yoda's is 0.7 m). Scaled again with the body (x 0.44) Yoda's reached 0.3 m
   // and hit no one (log 2026-10-28 23:02). See Hand_blade_lengths.
   state.weapon_scale = scale;
   state.scaled_blade_length = state.blade_length;
   const glm::mat3 hilt_basis = basis(state.edge_direction, glm::vec3{1.0f, 0.0f, 0.0f});
   const glm::mat3 world_basis = basis(dir, hand.rotation[0]);

   state.hilt_rotation = world_basis * glm::transpose(hilt_basis);
   state.soldier_position = soldier;
   state.head_offset = input.head_position - soldier;
   state.hilt_offset = hand.position + dir * (state.grip_offset * scale) - soldier;
   state.base_offset = state.hilt_offset + state.hilt_rotation * (state.edge_position * scale);
   state.dir = dir;

   // Dual sabers: the left hilt the same way from the left hand.
   state.left_hand_valid = state.dual && input.left_hand.has_value();

   if (state.left_hand_valid) {
      const Hand& left = *input.left_hand;
      const glm::vec3 left_dir = blade_direction(left.rotation);

      state.left_hilt_rotation =
         basis(left_dir, left.rotation[0]) *
         glm::transpose(basis(state.left_edge_direction, glm::vec3{1.0f, 0.0f, 0.0f}));
      state.left_hilt_offset = left.position + left_dir * (state.grip_offset * scale) - soldier;
   }

   // Every blade in a hand: blade 0 as above, the others from their hilt.
   state.blade_active = {};

   for (int b = 0; b < state.blade_count; ++b) {
      const State::Blade_edge& edge = state.blade_edges[b];

      if (b == 0) {
         state.blade_bases[0] = state.base_offset;
         state.blade_dirs[0] = dir;
         state.blade_lengths[0] = state.scaled_blade_length;
         state.blade_active[0] = true;

         continue;
      }

      if (edge.hand == 1 && !state.left_hand_valid) continue;

      const glm::mat3& r = edge.hand == 0 ? state.hilt_rotation : state.left_hilt_rotation;
      const glm::vec3 hilt = edge.hand == 0 ? state.hilt_offset : state.left_hilt_offset;

      state.blade_bases[b] = hilt + r * (edge.position * scale);
      state.blade_dirs[b] = glm::normalize(r * edge.direction);
      state.blade_lengths[b] = edge.length;
      state.blade_active[b] = true;
   }

   const glm::vec3 tip =
      soldier + state.base_offset + dir * state.scaled_blade_length - input.head_position;

   // Swings: the full blade's tip speed (also while lighting up).
   glm::vec3 tip_velocity{};

   if (state.prev_tip_valid && dt > 0.0f) {
      tip_velocity = (tip - state.prev_tip) / dt;
      state.tip_speed = glm::length(tip_velocity);
   }

   state.prev_tip = tip;
   state.prev_tip_valid = true;

   // Dual sabers: the left blade's tip too; the faster of the two swings.
   int left_blade = -1;
   bool left_faster = false;

   for (int b = 0; b < state.blade_count && left_blade < 0; ++b) {
      if (state.blade_active[b] && state.blade_edges[b].hand == 1) left_blade = b;
   }

   if (left_blade >= 0) {
      const glm::vec3 left_tip = soldier + state.blade_bases[left_blade] +
                                 state.blade_dirs[left_blade] * state.blade_lengths[left_blade] -
                                 input.head_position;

      if (state.prev_left_tip_valid && dt > 0.0f) {
         const glm::vec3 velocity = (left_tip - state.prev_left_tip) / dt;

         if (glm::length(velocity) > state.tip_speed) {
            state.tip_speed = glm::length(velocity);
            tip_velocity = velocity;
            left_faster = true;
         }
      }

      state.prev_left_tip = left_tip;
      state.prev_left_tip_valid = true;
   }
   else {
      state.prev_left_tip_valid = false;
   }

   // The swing's thresholds are the player's (metres in the room): a small hero's blade
   // moves as much less as the body is smaller.
   const Settings& s = settings();

   state.tip_speed /= scale;
   tip_velocity /= scale;

   const bool fast = state.tip_speed > s.saber_swing_speed;
   const glm::vec3 tip_dir = fast ? tip_velocity / state.tip_speed : glm::vec3{};

   state.fast_travel = fast ? state.fast_travel + state.tip_speed * dt : 0.0f;

   const bool swing_ready = fast && state.fast_travel >= s.saber_swing_distance;
   const auto start_swing = [&] {
      state.swinging = true;
      state.swing_started = true;
      state.slow_time = 0.0f;
      state.stat_swing_starts += 1;
      state.events.swing_speed = std::max(state.events.swing_speed, state.tip_speed);
      state.events.swing_left = left_faster;
   };

   if (!state.lit || state.thrown || state.control_lost) {
      state.swinging = false;
   }
   else if (!state.swinging) {
      if (swing_ready) start_swing();
   }
   else {
      state.slow_time = state.tip_speed < s.saber_swing_speed * swing_end_speed_fraction
                           ? state.slow_time + dt
                           : 0.0f;

      if (state.slow_time > swing_end_time) {
         state.swinging = false;
      }
      else if (fast && state.swing_dir_valid &&
               glm::dot(tip_dir, state.swing_dir) < swing_reverse_cos) {
         // Turned back: a new swing once it has gone the swing length the other way.
         state.fast_travel = state.tip_speed * dt;

         if (state.fast_travel >= s.saber_swing_distance) start_swing();
         else state.swinging = false;

         state.stat_reversals += 1;
      }
   }

   if (fast) {
      state.swing_dir = tip_dir;
      state.swing_dir_valid = true;
   }
   else if (!state.swinging) {
      state.swing_dir_valid = false;
   }

   // Block: both grips held, lit, a blade upright, in front of the head.
   const auto up_in_front = [&](const glm::vec3 base, const glm::vec3 blade_dir,
                                const float length) {
      const glm::vec3 to_middle = soldier + base + blade_dir * (length * 0.5f) -
                                  input.head_position;
      const float upright = glm::dot(blade_dir, glm::vec3{0.0f, 1.0f, 0.0f});

      return upright > std::cos(glm::radians(block_max_degrees)) &&
             glm::dot(to_middle, input.head_forward) > block_min_forward &&
             glm::length(to_middle) < block_max_distance;
   };

   bool blade_up = up_in_front(state.base_offset, dir, state.scaled_blade_length);

   for (int b = 1; b < state.blade_count && !blade_up; ++b) {
      if (state.blade_active[b]) {
         blade_up = up_in_front(state.blade_bases[b], state.blade_dirs[b], state.blade_lengths[b]);
      }
   }

   state.blocking = input.block && state.lit && !state.mesh_blade && !state.thrown &&
                    !state.control_lost && state.ignition >= 1.0f && blade_up;

   if (state.blocking) state.stat_block_frames += 1;
}

auto take_events() noexcept -> Events
{
   return std::exchange(state.events, Events{});
}

auto player_control_loss() noexcept -> Control_loss
{
   switch (state.soldier_state) {
   case soldier_thrown:
      return Control_loss::thrown;
   case soldier_held:
      return Control_loss::held;
   default:
      return Control_loss::none;
   }
}

bool player_has_melee() noexcept
{
   return state.weapon != nullptr;
}

bool player_shot_within(const float seconds) noexcept
{
   return state.last_shot_tick != 0 &&
          GetTickCount64() - state.last_shot_tick <= static_cast<std::uint64_t>(seconds * 1000.0f);
}

bool player_gun_fire_point(glm::vec3& position, glm::vec3& direction) noexcept
{
   if (state.weapon || state.in_turret) return false;

   __try {
      const auto* const weapon = read_primary_weapon();
      const auto* const aimer = weapon ? field<const std::uint8_t*>(weapon, weapon_aimer) : nullptr;

      if (!aimer) return false;

      position = read_vec3(aimer, aimer_fire_position);
      direction = read_vec3(aimer, aimer_fire_direction);

      return glm::length(direction) > 1e-3f;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

bool player_in_turret() noexcept
{
   return state.in_turret;
}

bool player_on_droideka() noexcept
{
   return state.hooked && state.player_vtable != 0 &&
          state.player_vtable == engine<std::uintptr_t>(exe_droideka_trackable_vtable);
}

int player_control_mode() noexcept
{
   if (!state.hooked) return -1;

   const int mode = read_control_mode();

   return mode >= 0 && mode <= 4 ? mode : -1;
}

auto player_body_on_turret() noexcept -> std::optional<glm::vec3>
{
   if (!state.turret_body_valid) return std::nullopt;

   return state.turret_body;
}

bool turret_fire_line(glm::vec3& position, glm::vec3& direction) noexcept
{
   if (!state.in_turret) return false;

   __try {
      const auto* const trackers = *engine<std::uint8_t* const*>(exe_trackers);
      const auto* const trackable = trackers ? field<const std::uint8_t*>(trackers, 0x10) : nullptr;

      if (!trackable || vtable_of(trackable) !=
                           engine<std::uintptr_t>(exe_mounted_turret_trackable_vtable)) {
         return false;
      }

      const auto* const weapon = field<const std::uint8_t*>(trackable, turret_trackable_weapons);
      const auto* const aimer = field<const std::uint8_t*>(trackable, turret_trackable_aimers);

      // The first weapon, with its own aimer (else the layout isn't the one expected).
      if (!weapon || !aimer || field<const std::uint8_t*>(weapon, weapon_aimer) != aimer) {
         return false;
      }

      position = read_vec3(aimer, aimer_fire_position);
      direction = read_vec3(aimer, aimer_fire_direction);

      return glm::length(direction) > 1e-3f;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

bool player_secondary_is_grenade() noexcept
{
   return state.secondary_grenade;
}

auto player_scope_texture() noexcept -> void*
{
   if (!state.hooked) return nullptr;

   const std::uint32_t hash = read_primary_scope_texture();
   void* texture = find_game_texture(hash);

   if (!texture && hash != default_scope_texture) {
      texture = find_game_texture(default_scope_texture);
   }

   static std::uint32_t logged_hash = 1;

   if (hash != logged_hash) {
      logged_hash = hash;

      log_fmt(Log_level::info, "VR: the gun's scope texture 0x{:08X}: {}.", hash,
              texture ? "the game's" : "not loaded, the VR reticle instead");
   }

   return texture;
}

auto player_reload() noexcept -> std::optional<Reload>
{
   if (!state.hooked) return std::nullopt;

   const auto* const weapon = read_reloading_weapon();
   Reload reload{.weapon = weapon};

   if (!weapon || !read_reload(weapon, reload.elapsed, reload.length)) return std::nullopt;

   return reload;
}

bool finish_player_reload(const void* weapon) noexcept
{
   auto* const reloading = read_reloading_weapon();

   return reloading && reloading == weapon && end_reload(reloading);
}

bool cool_player_weapon(const float seconds) noexcept
{
   if (!state.hooked || seconds <= 0.0f) return false;

   __try {
      std::uint8_t* const weapon = read_primary_weapon();

      if (!weapon || vtable_of(weapon) == engine<std::uintptr_t>(exe_melee_vtable) ||
          vtable_of(weapon) == engine<std::uintptr_t>(exe_grenade_vtable)) {
         return false;
      }

      const auto* const type = field<const std::uint8_t*>(weapon, weapon_class);
      auto* const heat = field<std::uint8_t*>(weapon, weapon_heat);

      if (!type || !heat || !(field<float>(type, class_heat_per_shot) > 0.0f)) {
         return false;
      }

      const float rate = field<float>(type, class_heat_recover_rate);
      const auto* const limits = field<const float*>(heat, 0);
      auto& level = *reinterpret_cast<float*>(heat + 4);
      auto& flags = *reinterpret_cast<std::uint32_t*>(heat + 8);

      if (!limits || !(rate > 0.0f) || !std::isfinite(level) || level >= limits[0]) return false;

      // As FUN_00491690 does.
      level = std::min(level + rate * seconds, limits[0]);

      if (level >= limits[2]) flags &= ~1u;

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

bool player_in_vehicle() noexcept
{
   return state.hooked && state.player_vtable != 0 &&
          state.player_vtable != engine<std::uintptr_t>(exe_soldier_trackable_vtable) &&
          !state.in_turret;
}

bool game_screen_shown() noexcept
{
   return state.game_screen[0] != '\0';
}

auto game_screen_name() noexcept -> const char*
{
   return state.game_screen;
}

bool player_controls_entity() noexcept
{
   return !state.hooked || state.player_vtable != 0;
}

bool player_soldier_on_foot() noexcept
{
   return state.hooked &&
          state.player_vtable == engine<std::uintptr_t>(exe_soldier_trackable_vtable) &&
          state.soldier_state >= 0 && state.soldier_state < soldier_thrown;
}

auto player_head_height() noexcept -> std::optional<float>
{
   if (!state.head_bone_valid || state.frame - state.head_bone_frame > 2) {
      return std::nullopt;
   }

   return state.head_bone_height;
}

bool player_saber_thrown() noexcept
{
   return state.weapon && state.thrown;
}

auto player_soldier() noexcept -> std::optional<Player_soldier>
{
   glm::vec3 position{};
   std::int32_t soldier_state = -1;

   if (!state.hooked || !read_player_soldier(position, soldier_state)) return std::nullopt;

   return Player_soldier{position, soldier_state};
}

bool galactic_conquest_shown() noexcept
{
   return std::strncmp(state.game_screen, "ifs_freeform", 12) == 0;
}

bool hurry_grenade_throw() noexcept
{
   if (!state.hooked) return false;

   __try {
      std::uint8_t* const weapon = read_secondary_weapon();

      if (!weapon || vtable_of(weapon) != engine<std::uintptr_t>(exe_grenade_vtable)) {
         return false;
      }

      if (field<std::uint8_t>(weapon, grenade_throwing) == 0 ||
          field<std::uint8_t>(weapon, grenade_released) != 0) {
         return false;
      }

      const auto* const weapon_class_ = field<const std::uint8_t*>(weapon, weapon_class);
      const int type = std::clamp<int>(field<std::int8_t>(weapon, grenade_throw_type), 0, 2);
      const float delay = field<float>(weapon_class_, grenade_class_delays + type * 4);

      if (field<float>(weapon, grenade_timer) < delay) {
         set_field<float>(weapon, grenade_timer, delay);
      }

      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

bool player_blocking() noexcept
{
   return state.weapon && state.blocking;
}

bool player_dual_sabers() noexcept
{
   return state.weapon && state.dual;
}

auto blade_direction(const glm::mat3& hand_rotation) noexcept -> glm::vec3
{
   const float pitch = glm::radians(state.pitch_degrees);

   return glm::normalize(std::cos(pitch) * -hand_rotation[2] +
                         std::sin(pitch) * hand_rotation[1]);
}

auto player_saber_hilt() noexcept -> std::optional<glm::vec3>
{
   if (!state.weapon || !state.drawn_hilt_valid || state.frame - state.drawn_hilt_frame > 1) {
      return std::nullopt;
   }

   return state.drawn_hilt;
}

auto player_left_saber_hilt() noexcept -> std::optional<glm::vec3>
{
   if (!state.weapon || !state.drawn_hilt_valid || !state.drawn_left_hilt_valid ||
       state.frame - state.drawn_hilt_frame > 1) {
      return std::nullopt;
   }

   return state.drawn_left_hilt;
}

bool game_saber_in_hand() noexcept
{
   return state.weapon && state.game_saber_seen &&
          state.frame - state.game_saber_frame <= game_saber_frames;
}

auto blade() noexcept -> std::optional<Blade>
{
   // The game's saber is put in the hand whenever its render is hooked (none drawn while
   // it's hidden): this one is only for an exe where that failed.
   if (!state.weapon || !state.hand_valid || state.control_lost || state.render_hooked ||
       game_saber_in_hand()) {
      return std::nullopt;
   }

   const glm::vec3 base = state.soldier_position + state.base_offset;

   return Blade{.hilt_bottom = state.soldier_position + state.hilt_offset,
                .hilt_top = base,
                .tip = base + state.dir * (state.scaled_blade_length * state.ignition),
                .color = state.color,
                .radius = state.blade_width * 0.25f,
                .glow = state.ignition};
}

void log_stats() noexcept
{
   if (!state.hooked) return;

   if (state.stat_bash_checked_frames || state.stat_bash_off_frames) {
      log_fmt(Log_level::info,
              "VR: weapon bash: {} bashes, {} hit, {} blocked; gesture checked in {} frames "
              "(off in {}: not on foot / no hooks), fastest gun front {:.1f} m/s (needs "
              "{:.1f}), {} soldiers drawn near in the last frame.",
              state.stat_bashes, state.stat_bash_hits, state.stat_bash_blocked,
              state.stat_bash_checked_frames, state.stat_bash_off_frames,
              state.stat_bash_peak_speed, settings().bash_speed, state.bash_candidate_count);

      state.stat_bashes = 0;
      state.stat_bash_hits = 0;
      state.stat_bash_blocked = 0;
      state.stat_bash_checked_frames = 0;
      state.stat_bash_off_frames = 0;
      state.stat_bash_peak_speed = 0.0f;
   }

   if (state.stat_shots) {
      log_fmt(Log_level::info,
              "VR: shots through the cannon/other hooks {} ({} moved to the gun in the hand).",
              state.stat_shots, state.stat_shots_moved);

      state.stat_shots = 0;
      state.stat_shots_moved = 0;
   }

   if (state.stat_effects_moved) {
      log_fmt(Log_level::info, "VR: effects of the player's gun put at the gun in the hand: {}.",
              state.stat_effects_moved);

      state.stat_effects_moved = 0;
   }

   if (state.stat_emitter_shots) {
      log_fmt(Log_level::info,
              "VR: the player's emitter shots (arcs): {} ({} from a moved shot, {} moved "
              "here).",
              state.stat_emitter_shots, state.stat_emitter_in_shot, state.stat_emitter_moved);

      state.stat_emitter_shots = 0;
      state.stat_emitter_in_shot = 0;
      state.stat_emitter_moved = 0;
   }

   if (state.weapon || state.stat_deflect_calls || state.stat_weapon_updates) {
      log_fmt(Log_level::info,
              "VR: melee: third person refused {} times, {} weapon updates ({} swinging), "
              "{} swings ({} turned back), {} hits ({} re-hit windows), deflecting asked {} "
              "times, {} bolts at the saber ({} deflected, {} by the auto block), blocking {} of {} frames (block "
              "stance entered {} times), saber renders {} ({} in the hand, {} in flight, {} "
              "hidden while not in control, {} left out of the hand while thrown).",
              state.stat_third_person_refused, state.stat_weapon_updates,
              state.stat_swing_updates, state.stat_swing_starts, state.stat_reversals,
              state.stat_hits, state.stat_rehits,
              state.stat_deflecting_queries, state.stat_deflect_calls, state.stat_deflected,
              state.stat_auto_deflects,
              state.stat_block_frames, state.stat_frames, state.stat_block_stances,
              state.stat_renders, state.stat_renders_in_hand, state.stat_flight_renders,
              state.stat_hidden_renders, state.stat_thrown_hand_renders);
   }

   state.stat_third_person_refused = 0;
   state.stat_weapon_updates = 0;
   state.stat_swing_updates = 0;
   state.stat_swing_starts = 0;
   state.stat_hits = 0;
   state.stat_deflecting_queries = 0;
   state.stat_deflect_calls = 0;
   state.stat_deflected = 0;
   state.stat_auto_deflects = 0;
   state.stat_rehits = 0;
   state.stat_reversals = 0;
   state.stat_block_frames = 0;
   state.stat_renders = 0;
   state.stat_renders_in_hand = 0;
   state.stat_block_stances = 0;
   state.stat_flight_renders = 0;
   state.stat_hidden_renders = 0;
   state.stat_thrown_hand_renders = 0;
   state.stat_frames = 0;
}

}
