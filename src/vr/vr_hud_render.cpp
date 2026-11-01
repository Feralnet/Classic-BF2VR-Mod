#include "vr_hud_render.hpp"
#include "../logger.hpp"

#include <cstring>
#include <string_view>

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace sp::vr {

namespace {

constexpr std::string_view shader_source = R"(
cbuffer Hud : register(b0)
{
   float4 view_projection[4];
   float4 center;
   float4 right;  // half extent
   float4 up;     // half extent
   float4 uv;     // u0, v0, u1, v1
   float4 mode;   // x: 1 = black backing, y: 1 = blended, z: 1 = solid colour
   float4 color;
};

Texture2D<float4> layer : register(t0);
SamplerState linear_clamp : register(s0);

struct Vs_output {
   float4 positionPS : SV_Position;
   float2 texcoords : TEXCOORD0;
};

Vs_output quad_vs(uint id : SV_VertexID)
{
   const float2 corner = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? -1.0 : 1.0);
   const float4 positionWS = float4(center.xyz + right.xyz * corner.x + up.xyz * corner.y, 1.0);

   Vs_output output;

   output.positionPS = float4(dot(view_projection[0], positionWS),
                              dot(view_projection[1], positionWS),
                              dot(view_projection[2], positionWS),
                              dot(view_projection[3], positionWS));
   output.texcoords = lerp(uv.xy, uv.zw, corner * float2(0.5, -0.5) + 0.5);

   return output;
}

float4 quad_ps(Vs_output input) : SV_Target
{
   if (mode.x > 0.5) return float4(0.0, 0.0, 0.0, 1.0);
   if (mode.z > 0.5) return color;

   const float4 color = layer.Sample(linear_clamp, input.texcoords);

   // Blended (premultiplied), or over black.
   return mode.y > 0.5 ? color : float4(color.rgb, 1.0);
}
)";

struct Constants {
   std::array<glm::vec4, 4> view_projection;
   glm::vec4 center;
   glm::vec4 right;
   glm::vec4 up;
   glm::vec4 uv;
   glm::vec4 mode;
   glm::vec4 color;
};

static_assert(sizeof(Constants) % 16 == 0);

struct Resources {
   bool tried = false;
   bool ready = false;
   ID3D11VertexShader* quad_vs = nullptr;
   ID3D11PixelShader* quad_ps = nullptr;
   ID3D11Buffer* constants = nullptr;
   ID3D11BlendState* opaque = nullptr;
   ID3D11BlendState* premultiplied = nullptr;
   ID3D11DepthStencilState* no_depth = nullptr;
   ID3D11RasterizerState* two_sided = nullptr;
   ID3D11SamplerState* linear_clamp = nullptr;
};

Resources resources;

auto compile(const char* entry, const char* target) noexcept -> ID3DBlob*
{
   ID3DBlob* code = nullptr;
   ID3DBlob* errors = nullptr;

   const HRESULT result = D3DCompile(shader_source.data(), shader_source.size(), "bf2vr_hud",
                                     nullptr, nullptr, entry, target,
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

   if (FAILED(result)) {
      log_fmt(Log_level::error, "VR: HUD shader {} failed to compile: {}", entry,
              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
   }

   if (errors) errors->Release();

   return SUCCEEDED(result) ? code : nullptr;
}

bool create(ID3D11Device& device) noexcept
{
   ID3DBlob* const vs_code = compile("quad_vs", "vs_4_0");
   ID3DBlob* const ps_code = compile("quad_ps", "ps_4_0");

   bool ok = vs_code && ps_code &&
             SUCCEEDED(device.CreateVertexShader(vs_code->GetBufferPointer(),
                                                 vs_code->GetBufferSize(), nullptr,
                                                 &resources.quad_vs)) &&
             SUCCEEDED(device.CreatePixelShader(ps_code->GetBufferPointer(),
                                                ps_code->GetBufferSize(), nullptr,
                                                &resources.quad_ps));

   if (vs_code) vs_code->Release();
   if (ps_code) ps_code->Release();

   const D3D11_BUFFER_DESC buffer_desc{.ByteWidth = sizeof(Constants),
                                       .Usage = D3D11_USAGE_DYNAMIC,
                                       .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
                                       .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE};

   ok = ok && SUCCEEDED(device.CreateBuffer(&buffer_desc, nullptr, &resources.constants));

   D3D11_BLEND_DESC blend{};
   blend.RenderTarget[0].BlendEnable = false;
   blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
   blend.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
   blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
   blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
   blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
   blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
   blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

   ok = ok && SUCCEEDED(device.CreateBlendState(&blend, &resources.opaque));

   blend.RenderTarget[0].BlendEnable = true;
   blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
   blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;

   ok = ok && SUCCEEDED(device.CreateBlendState(&blend, &resources.premultiplied));

   D3D11_DEPTH_STENCIL_DESC depth{};
   depth.DepthEnable = false;
   depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
   depth.DepthFunc = D3D11_COMPARISON_ALWAYS;

   ok = ok && SUCCEEDED(device.CreateDepthStencilState(&depth, &resources.no_depth));

   D3D11_RASTERIZER_DESC raster{};
   raster.FillMode = D3D11_FILL_SOLID;
   raster.CullMode = D3D11_CULL_NONE;
   raster.DepthClipEnable = false;

   ok = ok && SUCCEEDED(device.CreateRasterizerState(&raster, &resources.two_sided));

   D3D11_SAMPLER_DESC sampler{};
   sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
   sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
   sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
   sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
   sampler.MaxLOD = D3D11_FLOAT32_MAX;

   ok = ok && SUCCEEDED(device.CreateSamplerState(&sampler, &resources.linear_clamp));

   return ok;
}

bool ready(ID3D11Device& device) noexcept
{
   if (!resources.tried) {
      resources.tried = true;
      resources.ready = create(device);

      log(resources.ready ? Log_level::info : Log_level::error,
          resources.ready ? "VR: HUD renderer ready." : "VR: HUD renderer failed.");
   }

   return resources.ready;
}

// Both eyes' quads with whatever targets and depth state are bound.
void draw_quads(ID3D11DeviceContext& dc, const glm::uvec2 target_size,
                const std::array<std::array<glm::vec4, 4>, 2>& eye_view_projection,
                std::span<const Hud_quad> quads) noexcept
{
   dc.RSSetState(resources.two_sided);
   dc.IASetInputLayout(nullptr);
   dc.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
   dc.VSSetShader(resources.quad_vs, nullptr, 0);
   dc.PSSetShader(resources.quad_ps, nullptr, 0);
   dc.GSSetShader(nullptr, nullptr, 0);
   dc.VSSetConstantBuffers(0, 1, &resources.constants);
   dc.PSSetConstantBuffers(0, 1, &resources.constants);
   dc.PSSetSamplers(0, 1, &resources.linear_clamp);

   const float eye_width = static_cast<float>(target_size.x) / 2.0f;

   for (int eye = 0; eye < 2; ++eye) {
      const D3D11_VIEWPORT viewport{.TopLeftX = eye * eye_width,
                                    .TopLeftY = 0.0f,
                                    .Width = eye_width,
                                    .Height = static_cast<float>(target_size.y),
                                    .MinDepth = 0.0f,
                                    .MaxDepth = 1.0f};

      dc.RSSetViewports(1, &viewport);

      for (const Hud_quad& quad : quads) {
         const Constants constants{.view_projection = eye_view_projection[eye],
                                   .center = {quad.center, 1.0f},
                                   .right = {quad.right, 0.0f},
                                   .up = {quad.up, 0.0f},
                                   .uv = quad.uv,
                                   .mode = {quad.backing ? 1.0f : 0.0f,
                                            quad.blended ? 1.0f : 0.0f,
                                            quad.solid ? 1.0f : 0.0f, 0.0f},
                                   .color = quad.color};

         dc.OMSetBlendState(quad.blended || quad.solid ? resources.premultiplied
                                                       : resources.opaque,
                            nullptr, 0xffffffff);

         D3D11_MAPPED_SUBRESOURCE mapped{};

         if (FAILED(dc.Map(resources.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            continue;
         }

         std::memcpy(mapped.pData, &constants, sizeof(constants));
         dc.Unmap(resources.constants, 0);

         dc.Draw(4, 0);
      }
   }
}

}

void draw_hud_quads(ID3D11Device& device, ID3D11DeviceContext& dc,
                    ID3D11RenderTargetView& rtv, const glm::uvec2 target_size,
                    const std::array<std::array<glm::vec4, 4>, 2>& eye_view_projection,
                    ID3D11ShaderResourceView& layer, std::span<const Hud_quad> quads) noexcept
{
   if (!ready(device) || quads.empty()) return;

   ID3D11RenderTargetView* const target = &rtv;
   ID3D11ShaderResourceView* const srv = &layer;

   dc.OMSetRenderTargets(1, &target, nullptr);
   dc.OMSetDepthStencilState(resources.no_depth, 0);
   dc.PSSetShaderResources(0, 1, &srv);

   draw_quads(dc, target_size, eye_view_projection, quads);
}

}
