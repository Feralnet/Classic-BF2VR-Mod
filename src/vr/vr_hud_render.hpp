#pragma once

#include <array>
#include <span>

#include <d3d11_1.h>

#include <glm/glm.hpp>

namespace sp::vr {

// A flat quad in the world: `center` plus/minus the half extents `right` and `up`.
// Either a black backing, a `solid` colour (premultiplied, blended), or a piece (`uv`:
// u0, v0, u1, v1) of the HUD layer, drawn opaque over black, or `blended` over what's
// there (the layer is premultiplied).
struct Hud_quad {
   glm::vec3 center;
   glm::vec3 right;
   glm::vec3 up;
   glm::vec4 uv;
   bool backing;
   bool blended = false;
   bool solid = false;
   glm::vec4 color{};
};

// World-space quads into both halves of the side-by-side target (left eye left), with
// each eye's world->clip matrix (rows). Leaves the context's state changed: the caller
// restores the game's.
void draw_hud_quads(ID3D11Device& device, ID3D11DeviceContext& dc,
                    ID3D11RenderTargetView& rtv, const glm::uvec2 target_size,
                    const std::array<std::array<glm::vec4, 4>, 2>& eye_view_projection,
                    ID3D11ShaderResourceView& layer, std::span<const Hud_quad> quads) noexcept;

}
