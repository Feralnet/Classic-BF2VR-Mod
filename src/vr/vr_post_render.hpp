#pragma once

#include <array>

#include <d3d11_1.h>

#include <glm/glm.hpp>

namespace sp::vr {

// Darkens each eye's view towards its edges: nothing within `inner_degrees` of the
// eye's forward direction, `strength` (0..1) black from `outer_degrees` on. `tangents`
// are each eye's field of view (left, right, up, down). Eyes in the target's halves
// (`eyes_size`: the part the two eyes take). Leaves the context's state changed: the
// caller restores the game's.
void draw_vignette(ID3D11Device& device, ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                   const glm::uvec2 eyes_size, const std::array<glm::vec4, 2>& tangents,
                   const float inner_degrees, const float outer_degrees,
                   const float strength) noexcept;

// The scope view's window in the rectangle (x, y, width, height; pixels): alpha 1 in
// a centred circle and 0 outside it (the window is shown with its alpha), then the
// game's scope overlay `reticle` over it (its texture is the overlay's top left quarter,
// mirrored into the others, as the game's ScopeDisplay draws it), or without one a dark
// rim and a crosshair of our own. Same state caveat.
void draw_scope_mask(ID3D11Device& device, ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                     const glm::vec4 rect, ID3D11ShaderResourceView* reticle) noexcept;

// The shader view of one of the game's 2D textures (an IDirect3DTexture9 the d3d9
// layer made), or null (another kind of texture, or none).
auto game_texture_view(void* d3d9_texture) noexcept -> ID3D11ShaderResourceView*;

}
