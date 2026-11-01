
#include "backbuffer_resolver.hpp"
#include "../d3d11_helpers.hpp"

#include "../../vr/vr_system.hpp"

#include <DirectXTex.h>

#include <algorithm>

namespace sp::core::postprocessing {

namespace {

// Copies the input's middle into the swapchain's (the input may be larger: VR renders
// past the window's size, and the middle is between the eyes, left of the scope's view).
void copy_centred(ID3D11DeviceContext1& dc, ID3D11Resource& swapchain_texture,
                  const UINT swapchain_width, const UINT swapchain_height,
                  ID3D11Resource& input, const UINT input_width,
                  const UINT input_height) noexcept
{
   const UINT width = std::min(input_width, swapchain_width);
   const UINT height = std::min(input_height, swapchain_height);
   const UINT center = vr::mirror_center_x(input_width);
   const UINT source_left = std::min(center > width / 2 ? center - width / 2 : 0u,
                                     input_width - width);
   const UINT source_top = (input_height - height) / 2;
   const D3D11_BOX box{source_left, source_top, 0, source_left + width, source_top + height, 1};

   dc.CopySubresourceRegion(&swapchain_texture, 0, (swapchain_width - width) / 2,
                            (swapchain_height - height) / 2, 0, &input, 0, &box);
}

struct Resolve_constants {
   float width;
   float height;
   std::array<std::uint32_t, 2> randomness;
};

static_assert(sizeof(Resolve_constants) == 16);

}

Backbuffer_resolver::Backbuffer_resolver(Com_ptr<ID3D11Device5> device,
                                         shader::Database& shaders)
   : _resolve_vs{std::get<0>(
        shaders.vertex("late_backbuffer_resolve"sv).entrypoint("main_vs"sv))},
     _resolve_ps{shaders.pixel("late_backbuffer_resolve"sv).entrypoint("main_ps"sv)},
     _resolve_ps_x2{
        shaders.pixel("late_backbuffer_resolve"sv).entrypoint("main_x2_ps"sv)},
     _resolve_ps_x4{
        shaders.pixel("late_backbuffer_resolve"sv).entrypoint("main_x4_ps"sv)},
     _resolve_ps_x8{
        shaders.pixel("late_backbuffer_resolve"sv).entrypoint("main_x8_ps"sv)},
     _resolve_ps_linear{
        shaders.pixel("late_backbuffer_resolve"sv).entrypoint("main_linear_ps"sv)},
     _resolve_ps_linear_x2{
        shaders.pixel("late_backbuffer_resolve"sv).entrypoint("main_linear_x2_ps"sv)},
     _resolve_ps_linear_x4{
        shaders.pixel("late_backbuffer_resolve"sv).entrypoint("main_linear_x4_ps"sv)},
     _resolve_ps_linear_x8{
        shaders.pixel("late_backbuffer_resolve"sv).entrypoint("main_linear_x8_ps"sv)},
     _resolve_cb{create_dynamic_constant_buffer(*device, sizeof(Resolve_constants))}
{
}

void Backbuffer_resolver::apply(ID3D11DeviceContext1& dc, const Input input,
                                const Flags flags, Swapchain& swapchain,
                                const Interfaces interfaces) noexcept
{
   if (DirectX::MakeTypeless(input.format) == DirectX::MakeTypeless(Swapchain::format)) {
      apply_matching_format(dc, input, flags, swapchain, interfaces);
   }
   else {
      apply_mismatch_format(dc, input, flags, swapchain, interfaces);
   }
}

void Backbuffer_resolver::apply_matching_format(ID3D11DeviceContext1& dc,
                                                const Input& input, const Flags flags,
                                                Swapchain& swapchain,
                                                const Interfaces& interfaces) noexcept
{
   const bool msaa_input = input.sample_count > 1;
   const bool want_post_aa = flags.use_cmma2;
   const bool unscaled =
      input.width == swapchain.width() && input.height == swapchain.height();

   if (msaa_input && !want_post_aa) {
      if (unscaled) {
         dc.ResolveSubresource(swapchain.texture(), 0, &input.texture, 0, input.format);

         return;
      }
      else {
         resolve_input(dc, input, flags, interfaces, *swapchain.rtv(),
                       (swapchain.width() - input.width) / 2.0f,
                       (swapchain.height() - input.height) / 2.0f, input.width,
                       input.height);

         return;
      }
   }

   if (flags.use_cmma2) {
      interfaces.cmaa2.apply(dc, interfaces.profiler,
                             {.input = input.srv,
                              .output = *input.uav,
                              .width = input.width,
                              .height = input.height});
   }

   copy_centred(dc, *swapchain.texture(), swapchain.width(), swapchain.height(),
                input.texture, input.width, input.height);
}

void Backbuffer_resolver::apply_mismatch_format(ID3D11DeviceContext1& dc,
                                                const Input& input, const Flags flags,
                                                Swapchain& swapchain,
                                                const Interfaces& interfaces) noexcept
{
   [[maybe_unused]] const bool msaa_input = input.sample_count > 1;
   const bool want_post_aa = flags.use_cmma2;

   // Fast Path for no post AA.
   if (!want_post_aa) {
      resolve_input(dc, input, flags, interfaces, *swapchain.rtv(),
                    (swapchain.width() - input.width) / 2.0f,
                    (swapchain.height() - input.height) / 2.0f, input.width,
                    input.height);

      return;
   }

   if (flags.use_cmma2) {
      auto cmma_target = interfaces.rt_allocator.allocate(
         {.format = DXGI_FORMAT_R8G8B8A8_TYPELESS,
          .width = input.width,
          .height = input.height,
          .bind_flags = effects::rendertarget_bind_srv_rtv_uav,
          .srv_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
          .rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM,
          .uav_format = DXGI_FORMAT_R8G8B8A8_UNORM});

      resolve_input(dc, input, flags, interfaces, *cmma_target.rtv(), 0.0f,
                    0.0f, input.width, input.height);

      interfaces.cmaa2.apply(dc, interfaces.profiler,
                             {.input = *cmma_target.srv(),
                              .output = *cmma_target.uav(),
                              .width = input.width,
                              .height = input.height});

      copy_centred(dc, *swapchain.texture(), swapchain.width(), swapchain.height(),
                   cmma_target.texture(), input.width, input.height);

      return;
   }

   log_and_terminate("Failed to resolve backbuffer! This should never happen.");
}

void Backbuffer_resolver::resolve_input(
   ID3D11DeviceContext1& dc, const Input& input, const Flags flags,
   const Interfaces& interfaces, ID3D11RenderTargetView& target,
   const float target_offset_left, const float target_offset_top,
   const UINT target_width, const UINT target_height) noexcept
{
   effects::Profile profile{interfaces.profiler, dc, "Backbuffer Resolve"};

   const Resolve_constants constants{.width = static_cast<float>(input.width),
                                     .height = static_cast<float>(input.height),
                                     .randomness = {_resolve_rand_dist(_resolve_xorshift),
                                                    _resolve_rand_dist(_resolve_xorshift)}};

   update_dynamic_buffer(dc, *_resolve_cb, constants);

   auto* const cb = _resolve_cb.get();

   dc.ClearState();
   dc.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   dc.VSSetShader(_resolve_vs.get(), nullptr, 0);
   dc.VSSetConstantBuffers(0, 1, &cb);

   const D3D11_VIEWPORT viewport{.TopLeftX = target_offset_left,
                                 .TopLeftY = target_offset_top,
                                 .Width = static_cast<float>(target_width),
                                 .Height = static_cast<float>(target_height),
                                 .MinDepth = 0.0f,
                                 .MaxDepth = 1.0f};
   dc.RSSetViewports(1, &viewport);

   auto* const rtv = &target;
   dc.OMSetRenderTargets(1, &rtv, nullptr);

   const std::array srvs{&input.srv, get_blue_noise_texture(interfaces)};
   dc.PSSetShaderResources(0, srvs.size(), srvs.data());

   dc.PSSetConstantBuffers(0, 1, &cb);

   dc.PSSetShader(get_resolve_pixel_shader(input, flags), nullptr, 0);

   dc.Draw(3, 0);
}

auto Backbuffer_resolver::get_blue_noise_texture(const Interfaces& interfaces) noexcept
   -> ID3D11ShaderResourceView*
{
   if (_blue_noise_srvs[0] == nullptr) {
      for (int i = 0; i < 64; ++i) {
         _blue_noise_srvs[i] = interfaces.resources.at_if(
            "_SP_BUILTIN_blue_noise_rgb_"s + std::to_string(i));
      }
   }

   return _blue_noise_srvs[_resolve_rand_dist(_resolve_xorshift)].get();
}

auto Backbuffer_resolver::get_resolve_pixel_shader(const Input& input,
                                                   const Flags flags) noexcept
   -> ID3D11PixelShader*
{
   if (flags.linear_input) {
      switch (input.sample_count) {
      case 1:
         return _resolve_ps_linear.get();
      case 2:
         return _resolve_ps_linear_x2.get();
      case 4:
         return _resolve_ps_linear_x4.get();
      case 8:
         return _resolve_ps_linear_x8.get();
      }
   }

   switch (input.sample_count) {
   case 1:
      return _resolve_ps.get();
   case 2:
      return _resolve_ps_x2.get();
   case 4:
      return _resolve_ps_x4.get();
   case 8:
      return _resolve_ps_x8.get();
   default:
      log_and_terminate("Unsupported sample count!");
   }
}

}