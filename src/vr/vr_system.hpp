#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include <Windows.h>
#include <d3d11_1.h>

#include <glm/glm.hpp>

namespace sp::vr {

// Loads openxr_loader.dll (next to d3d9.dll) and creates the OpenXR instance and
// system. Must be called before the D3D11 device is created. Returns the LUID of
// the adapter the runtime renders on, or nullopt when VR is unavailable (no loader,
// SteamVR not running, no headset) — the game then runs as normal.
auto init_instance() noexcept -> std::optional<LUID>;

// Size of the stereo render target, or nullopt when VR is unavailable. Valid after
// init_instance. It holds the views side by side: the left eye, the right eye, and a
// column for the scope's view (see view_rect).
auto stereo_render_size(const glm::uvec2 window_size) noexcept -> std::optional<glm::uvec2>;

// Views drawn into the stereo target this frame: 2 (the eyes), or 3 while the scope's
// view is drawn too (view 2, magnified along the gun).
int view_count() noexcept;

// Where view `view` is in a stereo target of `target_size` (pixels).
struct View_rect {
   float x;
   float y;
   float width;
   float height;
};

auto view_rect(const int view, const glm::uvec2 target_size) noexcept -> View_rect;

// The part of a stereo target the two eyes take (left eye left, right eye right).
auto eyes_size(const glm::uvec2 target_size) noexcept -> glm::uvec2;

// The column the desktop mirror's crop is centred on, for an input `width` wide.
auto mirror_center_x(const std::uint32_t width) noexcept -> std::uint32_t;

// Creates the OpenXR session on the game's D3D11 device.
void init_session(ID3D11Device& device) noexcept;

// Logs who ends the process (called from DllMain on process detach; a process killed
// by TerminateProcess or a fast fail never gets there).
void log_process_exit() noexcept;

// True while the XR session is running (headset is showing our frames).
bool active() noexcept;

// True when the current game frame should be rendered in stereo: the session is
// running and this frame's eye poses are known.
bool stereo_frame() noexcept;

enum class Eye_draw {
   as_game,    // not a camera we handle: draw with the game's constants
   eye_camera, // draw with the eye's constants written to the outputs
   in_hand,    // as eye_camera, moved into the hand: draw without backface culling (the
               // first-person model is only built for the faces the game camera sees)
   skip        // don't draw at all
};

// Builds an eye's world->clip matrix (D3D9 constant register layout, as found in
// cb::Scene::projection_matrix) from the game's. Returns as_game if
// `game_view_projection` isn't a perspective camera (HUD, orthographic, mirrored
// reflection pass) - the draw should then be left as the game made it. `at_infinity`
// draws (the far scene: sky and distant backdrop, which the game keeps centred on its
// camera) get the eye's rotation only, so both eyes see them at the camera position,
// without parallax. The first-person weapon pass is placed at the right controller's
// aim pose; view-space draws (camera at the origin: the HUD) in front of the head along
// the game camera. `draw` tells the near scene's meshes apart: the first-person model
// (arms + gun, object origin at the camera) from nearby world objects, and its parts
// from each other; it's also logged for diagnostics.
struct Draw_info {
   UINT element_count = 0; // indices or vertices
   const char* shader_name = "";
   bool hard_skinned = false;
   bool skinned = false; // hard or soft (a soldier's body)
   int depthstencil = 0;
   glm::vec3 object_position{}; // translation of the draw's object->world matrix
   glm::mat3 object_rotation{1.0f}; // its 3x3 part (columns: the object's axes)
};

auto eye_view_projection(const std::array<glm::vec4, 4>& game_view_projection,
                         const int eye, const bool at_infinity, const Draw_info& draw,
                         std::array<glm::vec4, 4>& eye_view_projection,
                         glm::vec3& eye_positionWS) noexcept -> Eye_draw;

// Right after eye_view_projection gave Eye_draw::in_hand: for the weapon of a model set
// to be mirrored (bf2vr_arms.txt, the VR panel), turns `eye_view_projection` and
// `eye_positionWS` into its mirrored copy's (drawn as well, after the weapon). False
// when there's no copy to draw.
bool mirrored_in_hand(const Draw_info& draw, std::array<glm::vec4, 4>& eye_view_projection,
                      glm::vec3& eye_positionWS) noexcept;

// Called once per game frame, before the desktop Present, with the final frame.
// `side_by_side` frames carry both eyes (from stereo rendering); other frames are
// shown on a virtual screen. Ends the XR frame, then waits for and begins the next
// one so its eye poses are available while the game renders.
void submit_frame(ID3D11DeviceContext& dc, ID3D11Texture2D& frame,
                  const bool side_by_side) noexcept;

struct Input_state {
   bool fire = false;           // left mouse button, right trigger
   bool secondary_fire = false; // right mouse button, left trigger
   // DirectInput key codes held (one bit each): the keys the player's profile binds to
   // what the controllers do (see vr_system.cpp, "Game controls").
   std::array<std::uint32_t, 8> keys{};
   // Mouse buttons held besides fire (bit n: button n + 1), for controls the profile
   // binds to a mouse button.
   std::uint8_t mouse_buttons = 0;

   bool key(const std::uint32_t code) const noexcept
   {
      return code < 256 && ((keys[code >> 5] >> (code & 31)) & 1) != 0;
   }

   bool any_key() const noexcept
   {
      for (const std::uint32_t bits : keys) {
         if (bits) return true;
      }

      return false;
   }
};

// Latest controller input, updated by submit_frame.
auto input() noexcept -> Input_state;

// Mouse wheel notches (positive = up) for controls bound to the wheel. Consumed by the
// read.
auto take_mouse_wheel() noexcept -> LONG;

// Mouse movement (DirectInput counts) that turns the game camera towards the right
// hand's aim (or the head), or moves the menu cursor. Consumed by the read.
auto take_mouse_delta() noexcept -> std::array<LONG, 2>;

// Where the right controller points on the virtual screen (menus), 0..1 from the
// top-left corner, or nullopt when the screen isn't shown. For the GetCursorPos hook.
auto screen_pointer() noexcept -> std::optional<glm::vec2>;

// A shot of the player's gun (settings fire_from_muzzle): `position`, a point on the
// game's first-person gun (world, in front of the game camera: its fire point), moved to
// the same point of the gun in the right hand, and `direction` turned along that gun's
// barrel (its length kept). False (both unchanged) when the gun isn't in the hand.
bool fire_from_hand(glm::vec3& position, glm::vec3& direction) noexcept;

// The player's gun's muzzle flash (settings fire_from_muzzle): `position`, where the game
// draws it (its first-person model's muzzle, in front of the game camera), moved to where
// the shots start in that same first-person frame (the barrel tip plus the model's
// muzzle offset); its draws are then put in the hand the way the shots are. False
// (unchanged) when the gun isn't in the hand.
bool flash_from_hand(glm::vec3& position) noexcept;

// The weapon model's barrel tip comes from its geometry: once per model the VR side asks
// (during eye_view_projection of a draw of the weapon part or its muzzle flash) for that
// draw's positions. True once, right after such a draw; then the caller reads the draw's
// object-space positions back and hands them over (empty, with why, when it can't).
bool take_model_positions_request() noexcept;

void give_model_positions(std::span<const glm::vec3> positions, const char* failure) noexcept;

// The game's IDirect3DDevice9::BeginScene: the frame's game update is done, culling and
// drawing follow. The gameplay camera is culled looking where the head looks from here
// until submit_frame.
void begin_scene() noexcept;

// Eye camera draws (not the far scene) should be drawn without near clipping (depth
// clamped instead): a saber blade swung at the face isn't cut off, nor a turret's model
// around the eyes. True while the player holds a melee weapon or mans a turret.
bool near_draws_unclipped() noexcept;

// The game's full-screen blurs (scene blur: the jedi sprint; zoom blur) are left out of
// gameplay stereo frames unless the settings want them (they make the headset blurry).
// Logs the first one left out.
bool drop_screen_blur() noexcept;

// True for a HUD draw (view-space camera: health, ammo, minimap, crosshair) in a
// gameplay stereo frame. It's drawn into the HUD layer only, with the game's own HUD
// projection (a target of the frame's size, cleared to transparent black, blended with
// premultiplied alpha); draw_hud puts the layer in front of the eyes.
bool hud_to_layer(const std::array<glm::vec4, 4>& game_view_projection) noexcept;

// Draws the HUD layer into the side-by-side target before it's submitted: in front of
// the eyes (turned with the head), its crosshair along the gun, and pieces of it on
// panels at the wrists. Leaves the context's state changed: the caller restores the
// game's.
void draw_hud(ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
              const glm::uvec2 target_size, ID3D11ShaderResourceView& layer,
              const glm::uvec2 layer_size) noexcept;

// VR overlays drawn into the stereo frame (the saber in the hand): true once per frame,
// for the frame's first HUD draw, which they go before. The caller then calls
// draw_overlay and restores its own state.
bool overlay_due(const std::array<glm::vec4, 4>& game_view_projection) noexcept;

void draw_overlay(ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                  const glm::uvec2 target_size) noexcept;

// Last touches to the stereo frame before it's submitted (after anti-aliasing): the
// comfort vignette in the eyes and the scope view's round window and crosshair. Same
// state caveat as draw_hud.
void draw_post(ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
               const glm::uvec2 target_size) noexcept;

}
