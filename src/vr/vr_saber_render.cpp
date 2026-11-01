
#include "vr_saber_render.hpp"
#include "../logger.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace sp::vr {

namespace {

constexpr std::string_view shader_source = R"(
cbuffer Saber : register(b0)
{
   float4 view_projection[4];
   float4 eye_position;
   float4 segment_a; // w: radius
   float4 segment_b; // w: core fraction, 0 = solid rod
   float4 color;     // a: intensity
};

struct Vs_output {
   float4 positionPS : SV_Position;
   float2 coords : TEXCOORD0; // x across (-1..1), y along (metres from a)
   float length : TEXCOORD1;
};

Vs_output main_vs(uint id : SV_VertexID)
{
   const float radius = segment_a.w;
   const float3 axis = segment_b.xyz - segment_a.xyz;
   const float len = length(axis);
   const float3 dir = len > 1e-5 ? axis / len : float3(0.0, 1.0, 0.0);
   const float3 to_middle = (segment_a.xyz + segment_b.xyz) * 0.5 - eye_position.xyz;

   float3 side = cross(dir, to_middle);

   if (length(side) < 1e-5) side = cross(dir, float3(0.577, 0.577, 0.577));

   side = normalize(side);

   const float across = (id & 1) ? 1.0 : -1.0;
   const float along = (id & 2) ? len + radius : -radius;
   const float4 positionWS = float4(segment_a.xyz + dir * along + side * (across * radius), 1.0);

   Vs_output output;

   output.positionPS = float4(dot(view_projection[0], positionWS),
                              dot(view_projection[1], positionWS),
                              dot(view_projection[2], positionWS),
                              dot(view_projection[3], positionWS));
   output.coords = float2(across, along);
   output.length = len;

   return output;
}

float4 main_ps(Vs_output input) : SV_Target
{
   const float radius = segment_a.w;
   const float beyond = max(0.0, max(-input.coords.y, input.coords.y - input.length)) / radius;
   const float dist = sqrt(input.coords.x * input.coords.x + beyond * beyond);

   if (segment_b.w <= 0.0) {
      // Solid rod: lit like a cylinder seen from the side, rounded ends.
      if (dist > 1.0) discard;

      const float facing = sqrt(saturate(1.0 - input.coords.x * input.coords.x));

      return float4(color.rgb * (0.25 + 0.6 * facing), 1.0);
   }

   const float core = segment_b.w;
   const float white = saturate((core - dist) / (core * 0.4));
   const float halo = pow(saturate(1.0 - dist), 2.0);

   return float4((color.rgb * (halo * 1.4 + white * 0.6) + white * 0.9) * color.a, 0.0);
}
)";

struct Constants {
   std::array<glm::vec4, 4> view_projection;
   glm::vec4 eye_position;
   glm::vec4 segment_a;
   glm::vec4 segment_b;
   glm::vec4 color;
};

static_assert(sizeof(Constants) % 16 == 0);

struct Resources {
   bool tried = false;
   bool ready = false;
   ID3D11VertexShader* vs = nullptr;
   ID3D11PixelShader* ps = nullptr;
   ID3D11Buffer* constants = nullptr;
   ID3D11BlendState* additive = nullptr;
   ID3D11BlendState* opaque = nullptr;
   ID3D11DepthStencilState* no_depth = nullptr;
   ID3D11RasterizerState* two_sided = nullptr;
};

Resources resources;

auto compile(const char* entry, const char* target) noexcept -> ID3DBlob*
{
   ID3DBlob* code = nullptr;
   ID3DBlob* errors = nullptr;

   const HRESULT result = D3DCompile(shader_source.data(), shader_source.size(), "bf2vr_saber",
                                     nullptr, nullptr, entry, target,
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

   if (FAILED(result)) {
      log_fmt(Log_level::error, "VR: saber shader {} failed to compile: {}", entry,
              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
   }

   if (errors) errors->Release();

   return SUCCEEDED(result) ? code : nullptr;
}

bool create(ID3D11Device& device) noexcept
{
   const auto shaders = [&](const char* vs_entry, const char* ps_entry,
                            ID3D11VertexShader*& vs, ID3D11PixelShader*& ps) {
      ID3DBlob* const vs_code = compile(vs_entry, "vs_4_0");
      ID3DBlob* const ps_code = compile(ps_entry, "ps_4_0");

      const bool created =
         vs_code && ps_code &&
         SUCCEEDED(device.CreateVertexShader(vs_code->GetBufferPointer(),
                                             vs_code->GetBufferSize(), nullptr, &vs)) &&
         SUCCEEDED(device.CreatePixelShader(ps_code->GetBufferPointer(),
                                            ps_code->GetBufferSize(), nullptr, &ps));

      if (vs_code) vs_code->Release();
      if (ps_code) ps_code->Release();

      return created;
   };

   bool ok = shaders("main_vs", "main_ps", resources.vs, resources.ps);

   const D3D11_BUFFER_DESC buffer_desc{.ByteWidth = sizeof(Constants),
                                       .Usage = D3D11_USAGE_DYNAMIC,
                                       .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
                                       .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE};

   ok = ok && SUCCEEDED(device.CreateBuffer(&buffer_desc, nullptr, &resources.constants));

   D3D11_BLEND_DESC blend{};
   blend.RenderTarget[0].BlendEnable = true;
   blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
   blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
   blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
   blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
   blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
   blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
   blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

   ok = ok && SUCCEEDED(device.CreateBlendState(&blend, &resources.additive));

   blend.RenderTarget[0].BlendEnable = false;

   ok = ok && SUCCEEDED(device.CreateBlendState(&blend, &resources.opaque));

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

   return ok;
}

bool ready(ID3D11Device& device) noexcept
{
   if (!resources.tried) {
      resources.tried = true;
      resources.ready = create(device);

      log(resources.ready ? Log_level::info : Log_level::error,
          resources.ready ? "VR: saber renderer ready." : "VR: saber renderer failed.");
   }

   return resources.ready;
}

}

void draw_sabers(ID3D11Device& device, ID3D11DeviceContext& dc, ID3D11RenderTargetView& rtv,
                 const glm::uvec2 target_size,
                 const std::array<std::array<glm::vec4, 4>, 2>& eye_view_projection,
                 const std::array<glm::vec3, 2>& eye_position,
                 std::span<const Saber_draw> draws) noexcept
{
   if (!ready(device) || draws.empty()) return;

   ID3D11RenderTargetView* const target = &rtv;

   dc.OMSetRenderTargets(1, &target, nullptr);
   dc.OMSetDepthStencilState(resources.no_depth, 0);
   dc.RSSetState(resources.two_sided);
   dc.IASetInputLayout(nullptr);
   dc.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
   dc.VSSetShader(resources.vs, nullptr, 0);
   dc.PSSetShader(resources.ps, nullptr, 0);
   dc.GSSetShader(nullptr, nullptr, 0);
   dc.VSSetConstantBuffers(0, 1, &resources.constants);
   dc.PSSetConstantBuffers(0, 1, &resources.constants);

   const float eye_width = static_cast<float>(target_size.x) / 2.0f;

   for (int eye = 0; eye < 2; ++eye) {
      const D3D11_VIEWPORT viewport{.TopLeftX = eye * eye_width,
                                    .TopLeftY = 0.0f,
                                    .Width = eye_width,
                                    .Height = static_cast<float>(target_size.y),
                                    .MinDepth = 0.0f,
                                    .MaxDepth = 1.0f};

      dc.RSSetViewports(1, &viewport);

      for (const Saber_draw& draw : draws) {
         const Constants constants{.view_projection = eye_view_projection[eye],
                                   .eye_position = {eye_position[eye], 1.0f},
                                   .segment_a = {draw.a, draw.radius},
                                   .segment_b = {draw.b, draw.core},
                                   .color = {draw.color, draw.intensity}};

         D3D11_MAPPED_SUBRESOURCE mapped{};

         if (FAILED(dc.Map(resources.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            continue;
         }

         std::memcpy(mapped.pData, &constants, sizeof(constants));
         dc.Unmap(resources.constants, 0);

         dc.OMSetBlendState(draw.core > 0.0f ? resources.additive : resources.opaque, nullptr,
                            0xffffffff);
         dc.Draw(4, 0);
      }
   }
}

}
