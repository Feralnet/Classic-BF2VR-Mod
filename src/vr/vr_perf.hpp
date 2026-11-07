#pragma once

#include <cstdint>

#include <d3d11_1.h>

// Frame timing for optimising: GPU time per group of passes (timestamp queries, read back
// a few frames late without stalling) and CPU time per part of the frame. Every 10 s one
// line in the log ("VR perf: ...") and one row in bf2vr_logs\perf.csv, which is kept
// across runs (the log rotation only removes .log/.dmp files).
namespace sp::vr::perf {

// The public build (BF2VR_PUBLIC) leaves the profiler out: no perf.csv, no log lines.
#ifdef BF2VR_PUBLIC
inline constexpr bool enabled = false;
#else
inline constexpr bool enabled = true;
#endif

enum class Gpu_group : std::uint8_t {
   world,       // draws per eye into the side-by-side target (the world, the gun)
   full_screen, // full-screen passes across the side-by-side target (sky, bloom, ...)
   offscreen,   // draws into other targets / mono (shadow maps, refraction, menus)
   hud,         // HUD draws into the HUD layer
   effects,     // Shader Patch's own effects (postprocess, SSAO, game bloom/blur)
   vr_post,     // ours before submitting: HUD panels, CMAA2, vignette/scope, copy
   desktop,     // desktop mirror: resolve, ImGui, present
   count
};

enum class Cpu_part : std::uint8_t {
   draws,   // inside draw_for_each_eye (our per-eye loop + the driver's draw calls)
   vr,      // our per-frame VR work at submit (input, melee, logic), minus the below
   wait,    // xrWaitFrame + xrBeginFrame (paced by the headset)
   submit,  // xrEndFrame
   present, // desktop swapchain present
   count
};

// A GPU timestamp: what follows is `group` (only issued when the group changes).
void gpu_mark(ID3D11DeviceContext& dc, Gpu_group group) noexcept;

// CPU time spent in a part (QueryPerformanceCounter ticks).
void cpu_add(Cpu_part part, std::int64_t ticks) noexcept;

auto ticks_now() noexcept -> std::int64_t;

// Counts one draw of the group (draw_for_each_eye).
void count_draw(Gpu_group group) noexcept;

// From submit_frame: what this frame was.
void note_vr_frame(bool stereo, float refresh_hz, int views) noexcept;

// End of Shader_patch::present: closes the frame, reads finished frames back, logs every
// 10 s.
void frame_end(ID3D11DeviceContext& dc, std::uint32_t width, std::uint32_t height) noexcept;

// Scoped CPU timer.
class Cpu_scope {
public:
   explicit Cpu_scope(Cpu_part part) noexcept : _part{part}, _start{ticks_now()} {}
   ~Cpu_scope() { cpu_add(_part, ticks_now() - _start); }

   Cpu_scope(const Cpu_scope&) = delete;
   Cpu_scope& operator=(const Cpu_scope&) = delete;

private:
   Cpu_part _part;
   std::int64_t _start;
};

}
