#pragma once

#include <array>
#include <span>

#include <d3d11_1.h>

#include <glm/glm.hpp>

namespace sp::vr {

// A capsule drawn as a camera-facing quad between `a` and `b`: either a glowing blade
// (additive, white core of `core` x radius) or, with core 0, a solid shaded rod (the
// hilt).
struct Saber_draw {
   glm::vec3 a;
   glm::vec3 b;
   float radius;
   glm::vec3 color;
   float intensity;
   float core;
};

// Draws into both halves of the side-by-side target (left eye left), with each eye's
// world->clip matrix (rows) and position. Leaves the context's state changed: the
// caller restores the game's.
void draw_sabers(ID3D11Device& device, ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                 const glm::uvec2 target_size,
                 const std::array<std::array<glm::vec4, 4>, 2>& eye_view_projection,
                 const std::array<glm::vec3, 2>& eye_position,
                 std::span<const Saber_draw> draws) noexcept;

}
