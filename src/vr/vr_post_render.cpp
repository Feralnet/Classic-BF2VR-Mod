#include "vr_post_render.hpp"
#include "../direct3d/base_texture.hpp"
#include "../logger.hpp"

#include <cstring>
#include <string_view>

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace sp::vr {

namespace {

constexpr std::string_view shader_source = R"(
cbuffer Post : register(b0)
{
   float4 tangents; // the view's field of view: left, right, up, down (tan)
   float4 params;   // vignette: inner, outer (radians), strength
};

struct Vs_output {
   float4 positionPS : SV_Position;
   float2 texcoords : TEXCOORD0;
};

Vs_output fullscreen_vs(uint id : SV_VertexID)
{
   const float2 uv = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);

   Vs_output output;

   output.positionPS = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
   output.texcoords = uv;

   return output;
}

float4 vignette_ps(Vs_output input) : SV_Target
{
   const float2 tan_xy = float2(lerp(tangents.x, tangents.y, input.texcoords.x),
                                lerp(tangents.z, tangents.w, input.texcoords.y));
   const float angle = atan(length(tan_xy));
   const float dark = smoothstep(params.x, params.y, angle) * params.z;

   return float4(0.0, 0.0, 0.0, dark);
}

// Pass 1 writes only alpha: the round window. Pass 2 blends black over the colour: the
// rim and the crosshair.
float4 scope_alpha_ps(Vs_output input) : SV_Target
{
   const float r = length(input.texcoords - 0.5);

   return float4(0.0, 0.0, 0.0, r < 0.49 ? 1.0 : 0.0);
}

float4 scope_reticle_ps(Vs_output input) : SV_Target
{
   const float2 p = input.texcoords - 0.5;
   const float r = length(p);
   const float rim = smoothstep(0.36, 0.49, r);

   const float line_width = 0.0035;
   const float gap = 0.025;
   const bool on_line = (abs(p.x) < line_width && abs(p.y) > gap && abs(p.y) < 0.45) ||
                        (abs(p.y) < line_width && abs(p.x) > gap && abs(p.x) < 0.45);
   const bool dot = r < 0.004;

   return float4(0.0, 0.0, 0.0, max(rim * 0.9, (on_line || dot) ? 0.9 : 0.0));
}

// The game's scope overlay: its texture holds the top left quarter (the centre at its
// bottom right corner), mirrored into the other three.
Texture2D<float4> overlay : register(t0);
SamplerState overlay_sampler : register(s0);

float4 scope_texture_ps(Vs_output input) : SV_Target
{
   return overlay.Sample(overlay_sampler, 1.0 - abs(input.texcoords * 2.0 - 1.0));
}
)";

struct Constants {
   glm::vec4 tangents;
   glm::vec4 params;
};

struct Resources {
   bool tried = false;
   bool ready = false;
   ID3D11VertexShader* fullscreen_vs = nullptr;
   ID3D11PixelShader* vignette_ps = nullptr;
   ID3D11PixelShader* scope_alpha_ps = nullptr;
   ID3D11PixelShader* scope_reticle_ps = nullptr;
   ID3D11PixelShader* scope_texture_ps = nullptr;
   ID3D11SamplerState* linear_clamp = nullptr;
   ID3D11Buffer* constants = nullptr;
   ID3D11BlendState* darken = nullptr;      // black over the colour, alpha kept
   ID3D11BlendState* alpha_only = nullptr;  // replaces alpha, colour kept
   ID3D11DepthStencilState* no_depth = nullptr;
   ID3D11RasterizerState* no_cull = nullptr;
};

Resources resources;

auto compile(const char* entry, const char* target) noexcept -> ID3DBlob*
{
   ID3DBlob* code = nullptr;
   ID3DBlob* errors = nullptr;

   const HRESULT result = D3DCompile(shader_source.data(), shader_source.size(), "bf2vr_post",
                                     nullptr, nullptr, entry, target,
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

   if (FAILED(result)) {
      log_fmt(Log_level::error, "VR: post shader {} failed to compile: {}", entry,
              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
   }

   if (errors) errors->Release();

   return SUCCEEDED(result) ? code : nullptr;
}

bool create_pixel_shader(ID3D11Device& device, const char* entry,
                         ID3D11PixelShader*& shader) noexcept
{
   ID3DBlob* const code = compile(entry, "ps_4_0");

   const bool ok = code && SUCCEEDED(device.CreatePixelShader(code->GetBufferPointer(),
                                                              code->GetBufferSize(), nullptr,
                                                              &shader));

   if (code) code->Release();

   return ok;
}

bool create(ID3D11Device& device) noexcept
{
   ID3DBlob* const vs_code = compile("fullscreen_vs", "vs_4_0");

   bool ok = vs_code && SUCCEEDED(device.CreateVertexShader(vs_code->GetBufferPointer(),
                                                            vs_code->GetBufferSize(), nullptr,
                                                            &resources.fullscreen_vs));

   if (vs_code) vs_code->Release();

   ok = ok && create_pixel_shader(device, "vignette_ps", resources.vignette_ps);
   ok = ok && create_pixel_shader(device, "scope_alpha_ps", resources.scope_alpha_ps);
   ok = ok && create_pixel_shader(device, "scope_reticle_ps", resources.scope_reticle_ps);
   ok = ok && create_pixel_shader(device, "scope_texture_ps", resources.scope_texture_ps);

   D3D11_SAMPLER_DESC sampler{};
   sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
   sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
   sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
   sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
   sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
   sampler.MaxLOD = D3D11_FLOAT32_MAX;

   ok = ok && SUCCEEDED(device.CreateSamplerState(&sampler, &resources.linear_clamp));

   const D3D11_BUFFER_DESC buffer_desc{.ByteWidth = sizeof(Constants),
                                       .Usage = D3D11_USAGE_DYNAMIC,
                                       .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
                                       .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE};

   ok = ok && SUCCEEDED(device.CreateBuffer(&buffer_desc, nullptr, &resources.constants));

   D3D11_BLEND_DESC blend{};
   auto& target = blend.RenderTarget[0];

   target.BlendEnable = true;
   target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
   target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
   target.BlendOp = D3D11_BLEND_OP_ADD;
   target.SrcBlendAlpha = D3D11_BLEND_ZERO;
   target.DestBlendAlpha = D3D11_BLEND_ONE;
   target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
   target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                  D3D11_COLOR_WRITE_ENABLE_BLUE;

   ok = ok && SUCCEEDED(device.CreateBlendState(&blend, &resources.darken));

   target.BlendEnable = false;
   target.SrcBlend = D3D11_BLEND_ONE;
   target.DestBlend = D3D11_BLEND_ZERO;
   target.SrcBlendAlpha = D3D11_BLEND_ONE;
   target.DestBlendAlpha = D3D11_BLEND_ZERO;
   target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;

   ok = ok && SUCCEEDED(device.CreateBlendState(&blend, &resources.alpha_only));

   D3D11_DEPTH_STENCIL_DESC depth{};
   depth.DepthEnable = false;
   depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
   depth.DepthFunc = D3D11_COMPARISON_ALWAYS;

   ok = ok && SUCCEEDED(device.CreateDepthStencilState(&depth, &resources.no_depth));

   D3D11_RASTERIZER_DESC raster{};
   raster.FillMode = D3D11_FILL_SOLID;
   raster.CullMode = D3D11_CULL_NONE;
   raster.DepthClipEnable = true;

   ok = ok && SUCCEEDED(device.CreateRasterizerState(&raster, &resources.no_cull));

   return ok;
}

bool ready(ID3D11Device& device) noexcept
{
   if (!resources.tried) {
      resources.tried = true;
      resources.ready = create(device);

      log(resources.ready ? Log_level::info : Log_level::error,
          resources.ready ? "VR: post renderer ready." : "VR: post renderer failed.");
   }

   return resources.ready;
}

void bind_common(ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv) noexcept
{
   ID3D11RenderTargetView* const target = &rtv;

   dc.OMSetRenderTargets(1, &target, nullptr);
   dc.OMSetDepthStencilState(resources.no_depth, 0);
   dc.RSSetState(resources.no_cull);
   dc.IASetInputLayout(nullptr);
   dc.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
   dc.VSSetShader(resources.fullscreen_vs, nullptr, 0);
   dc.GSSetShader(nullptr, nullptr, 0);
   dc.PSSetConstantBuffers(0, 1, &resources.constants);
}

void set_constants(ID3D11DeviceContext& dc, const Constants& constants) noexcept
{
   D3D11_MAPPED_SUBRESOURCE mapped{};

   if (FAILED(dc.Map(resources.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

   std::memcpy(mapped.pData, &constants, sizeof(constants));
   dc.Unmap(resources.constants, 0);
}

}

void draw_vignette(ID3D11Device& device, ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                   const glm::uvec2 eyes_size, const std::array<glm::vec4, 2>& tangents,
                   const float inner_degrees, const float outer_degrees,
                   const float strength) noexcept
{
   if (strength <= 0.001f || !ready(device)) return;

   bind_common(dc, rtv);
   dc.OMSetBlendState(resources.darken, nullptr, 0xffffffff);
   dc.PSSetShader(resources.vignette_ps, nullptr, 0);

   constexpr float to_radians = 3.14159265f / 180.0f;
   const float eye_width = static_cast<float>(eyes_size.x) / 2.0f;

   for (int eye = 0; eye < 2; ++eye) {
      const D3D11_VIEWPORT viewport{eye * eye_width, 0.0f, eye_width,
                                    static_cast<float>(eyes_size.y), 0.0f, 1.0f};

      dc.RSSetViewports(1, &viewport);
      set_constants(dc, {.tangents = tangents[eye],
                         .params = {inner_degrees * to_radians, outer_degrees * to_radians,
                                    strength, 0.0f}});
      dc.Draw(4, 0);
   }
}

void draw_scope_mask(ID3D11Device& device, ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                     const glm::vec4 rect, ID3D11ShaderResourceView* reticle) noexcept
{
   if (!ready(device)) return;

   bind_common(dc, rtv);

   const D3D11_VIEWPORT viewport{rect.x, rect.y, rect.z, rect.w, 0.0f, 1.0f};

   dc.RSSetViewports(1, &viewport);
   set_constants(dc, {});

   dc.OMSetBlendState(resources.alpha_only, nullptr, 0xffffffff);
   dc.PSSetShader(resources.scope_alpha_ps, nullptr, 0);
   dc.Draw(4, 0);

   // The overlay blends over the colour with its alpha (the window's alpha is kept).
   dc.OMSetBlendState(resources.darken, nullptr, 0xffffffff);

   if (reticle) {
      dc.PSSetShader(resources.scope_texture_ps, nullptr, 0);
      dc.PSSetShaderResources(0, 1, &reticle);
      dc.PSSetSamplers(0, 1, &resources.linear_clamp);
      dc.Draw(4, 0);

      ID3D11ShaderResourceView* const none = nullptr;

      dc.PSSetShaderResources(0, 1, &none);

      return;
   }

   dc.PSSetShader(resources.scope_reticle_ps, nullptr, 0);
   dc.Draw(4, 0);
}

auto game_texture_view(void* d3d9_texture) noexcept -> ID3D11ShaderResourceView*
{
   if (!d3d9_texture) return nullptr;

   const auto& texture = *static_cast<const d3d9::Base_texture*>(d3d9_texture);

   if (texture.texture_type != d3d9::Texture_type::texture2d) return nullptr;

   const core::Game_texture* const game_texture = texture.get_if<core::Game_texture>();

   return game_texture ? game_texture->srv.get() : nullptr;
}

}
