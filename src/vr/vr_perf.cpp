
#include "vr_perf.hpp"
#include "vr_settings.hpp"
#include "vr_system.hpp"
#include "../logger.hpp"
#include "com_ptr.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <Windows.h>
#include <dxgi.h>

#include <fmt/format.h>

namespace sp::vr::perf {

namespace {

constexpr int slot_count = 6;      // frames in flight for the GPU's answers
constexpr int max_marks = 96;      // timestamps per frame (group changes); more merge
constexpr ULONGLONG period_ms = 10'000;
constexpr int group_count = static_cast<int>(Gpu_group::count);
constexpr int part_count = static_cast<int>(Cpu_part::count);
constexpr std::uint8_t end_mark = 0xff;

constexpr std::array<const char*, group_count> group_names{
   "world", "full_screen", "offscreen", "hud", "effects", "vr_post", "desktop"};

struct Slot {
   Com_ptr<ID3D11Query> disjoint;
   std::array<Com_ptr<ID3D11Query>, max_marks + 1> stamps;
   std::array<std::uint8_t, max_marks + 1> groups{};
   int marks = 0;
   bool pending = false; // ended, waiting for the GPU
   bool stereo = false;
};

struct Period {
   std::uint32_t frames = 0;
   std::uint32_t stereo_frames = 0;
   std::uint32_t missed = 0; // frames longer than 1.5 display periods
   std::array<double, part_count> cpu_ms{};
   std::vector<float> frame_ms;
   std::uint32_t gpu_frames = 0;
   std::uint32_t gpu_dropped = 0;  // answers that never came in time
   std::uint32_t gpu_disjoint = 0;
   std::uint32_t capped = 0;       // frames with more group changes than marks
   std::array<double, group_count> gpu_ms{};
   std::vector<float> gpu_frame_ms;
   std::array<std::uint64_t, group_count> draws{};
   float refresh_sum = 0.0f;
   int views_max = 0;
};

struct State {
   bool tried = false;
   bool ready = false;
   std::array<Slot, slot_count> slots;
   int current = 0;
   bool open = false; // the current slot has had its first mark
   std::uint8_t last_group = end_mark;

   std::array<std::int64_t, part_count> frame_cpu{};
   std::array<std::uint32_t, group_count> frame_draws{};
   bool frame_stereo = false;
   float frame_refresh = 0.0f;
   int frame_views = 0;

   std::int64_t last_frame_end = 0;
   std::int64_t frequency = 1;
   ULONGLONG period_start = 0;
   Period period;

   std::string gpu_name;
   std::string cpu_name;
   bool csv_failed = false;
};

State state;

auto ms(const std::int64_t ticks) noexcept -> double
{
   return static_cast<double>(ticks) * 1000.0 / static_cast<double>(state.frequency);
}

auto percentile(std::vector<float>& values, const float p) noexcept -> float
{
   if (values.empty()) return 0.0f;

   const std::size_t index =
      std::min(values.size() - 1, static_cast<std::size_t>(p * static_cast<float>(values.size())));

   std::nth_element(values.begin(), values.begin() + index, values.end());

   return values[index];
}

auto read_cpu_name() -> std::string
{
   char name[128]{};
   DWORD size = sizeof(name);

   if (RegGetValueA(HKEY_LOCAL_MACHINE, R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)",
                    "ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) {
      return "unknown";
   }

   std::string text = name;

   while (!text.empty() && text.back() == ' ') text.pop_back();
   while (!text.empty() && text.front() == ' ') text.erase(text.begin());

   return text;
}

auto read_gpu_name(ID3D11Device& device) -> std::string
{
   Com_ptr<IDXGIDevice> dxgi_device;
   Com_ptr<IDXGIAdapter> adapter;
   DXGI_ADAPTER_DESC desc{};

   if (FAILED(device.QueryInterface(dxgi_device.clear_and_assign())) ||
       FAILED(dxgi_device->GetAdapter(adapter.clear_and_assign())) ||
       FAILED(adapter->GetDesc(&desc))) {
      return "unknown";
   }

   char name[256]{};
   WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);

   return name;
}

bool ensure_ready(ID3D11DeviceContext& dc) noexcept
{
   if (state.tried) return state.ready;

   state.tried = true;

   LARGE_INTEGER frequency{};
   QueryPerformanceFrequency(&frequency);
   state.frequency = std::max<std::int64_t>(frequency.QuadPart, 1);

   Com_ptr<ID3D11Device> device;
   dc.GetDevice(device.clear_and_assign());

   if (!device) return false;

   for (Slot& slot : state.slots) {
      const D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
      const D3D11_QUERY_DESC stamp_desc{D3D11_QUERY_TIMESTAMP, 0};

      if (FAILED(device->CreateQuery(&disjoint_desc, slot.disjoint.clear_and_assign()))) {
         return false;
      }

      for (auto& stamp : slot.stamps) {
         if (FAILED(device->CreateQuery(&stamp_desc, stamp.clear_and_assign()))) return false;
      }
   }

   state.gpu_name = read_gpu_name(*device);
   state.cpu_name = read_cpu_name();
   state.ready = true;

   log_fmt(Log_level::info,
           "VR perf: profiling on ({}, {}): a \"VR perf\" line every 10 s and a row in "
           "bf2vr_logs\\perf.csv.",
           state.cpu_name, state.gpu_name);

   return true;
}

// Reads one ended slot back, false if the GPU hasn't got there yet.
bool read_slot(ID3D11DeviceContext& dc, Slot& slot) noexcept
{
   D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};

   if (dc.GetData(slot.disjoint.get(), &disjoint, sizeof(disjoint),
                  D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
      return false;
   }

   std::array<UINT64, max_marks + 1> stamps{};

   for (int i = 0; i < slot.marks; ++i) {
      if (dc.GetData(slot.stamps[i].get(), &stamps[i], sizeof(UINT64),
                     D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
         return false;
      }
   }

   slot.pending = false;

   if (disjoint.Disjoint || disjoint.Frequency == 0 || slot.marks < 2) {
      state.period.gpu_disjoint += 1;

      return true;
   }

   const double to_ms = 1000.0 / static_cast<double>(disjoint.Frequency);

   for (int i = 0; i + 1 < slot.marks; ++i) {
      if (slot.groups[i] >= group_count || stamps[i + 1] < stamps[i]) continue;

      state.period.gpu_ms[slot.groups[i]] += static_cast<double>(stamps[i + 1] - stamps[i]) * to_ms;
   }

   if (stamps[slot.marks - 1] >= stamps[0]) {
      state.period.gpu_frame_ms.push_back(
         static_cast<float>(static_cast<double>(stamps[slot.marks - 1] - stamps[0]) * to_ms));
   }

   state.period.gpu_frames += 1;

   return true;
}

void stamp(ID3D11DeviceContext& dc, Slot& slot, const std::uint8_t group) noexcept
{
   dc.End(slot.stamps[slot.marks].get());
   slot.groups[slot.marks] = group;
   slot.marks += 1;
}

void open_slot(ID3D11DeviceContext& dc) noexcept
{
   Slot& slot = state.slots[state.current];

   // Its answer never came: dropped.
   if (slot.pending) {
      slot.pending = false;
      state.period.gpu_dropped += 1;
   }

   slot.marks = 0;
   dc.Begin(slot.disjoint.get());
   state.open = true;
   state.last_group = end_mark;
}

void write_csv(const Period& p, const double seconds, const std::array<double, part_count>& cpu,
               const double cpu_game, const float cpu_p50, const float cpu_p95,
               const std::array<double, group_count>& gpu, const float gpu_p50,
               const float gpu_p95, const std::uint32_t width, const std::uint32_t height) noexcept
{
   if (state.csv_failed) return;

   namespace fs = std::filesystem;

   const fs::path path = fs::path{log_folder} / "perf.csv";
   std::error_code error;
   const bool header = !fs::exists(path, error);

   std::ofstream file{path, std::ios::app};

   if (!file) {
      state.csv_failed = true;
      log(Log_level::warning, "VR perf: can't write bf2vr_logs\\perf.csv.");

      return;
   }

   if (header) {
      file << "time,run,build,cpu_name,gpu_name,render_scale,width,height,views,refresh_hz,"
              "seconds,frames,stereo_frames,fps,missed_pct,"
              "cpu_frame_avg,cpu_frame_p50,cpu_frame_p95,cpu_game,cpu_draws,cpu_vr,cpu_wait,"
              "cpu_submit,cpu_present,"
              "gpu_frames,gpu_dropped,gpu_frame_avg,gpu_frame_p50,gpu_frame_p95";

      for (const char* name : group_names) file << ",gpu_" << name;
      for (const char* name : group_names) file << ",draws_" << name;

      file << "\n";
   }

   const auto now = std::time(nullptr);
   char time_text[32]{};
   std::strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

   const std::string run = fs::path{log_run_path("")}.filename().string();
   const double frames = std::max<double>(p.frames, 1.0);
   const double gpu_frames = std::max<double>(p.gpu_frames, 1.0);
   double gpu_sum = 0.0;

   for (const double value : p.gpu_ms) gpu_sum += value;

   std::string row = fmt::format(
      "{},{},{} {},\"{}\",\"{}\",{:.0f},{},{},{},{:.1f},{:.1f},{},{},{:.2f},{:.2f},"
      "{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},"
      "{},{},{:.3f},{:.3f},{:.3f}",
      time_text, run, __DATE__, __TIME__, state.cpu_name, state.gpu_name,
      settings().render_scale_percent, width, height, p.views_max,
      p.refresh_sum / static_cast<float>(frames), seconds, p.frames, p.stereo_frames,
      p.frames / seconds, 100.0 * p.missed / frames,
      seconds * 1000.0 / frames, cpu_p50, cpu_p95, cpu_game, cpu[0], cpu[1], cpu[2], cpu[3],
      cpu[4], p.gpu_frames, p.gpu_dropped, gpu_sum / gpu_frames, gpu_p50, gpu_p95);

   for (const double value : gpu) row += fmt::format(",{:.3f}", value);
   for (const std::uint64_t value : p.draws) row += fmt::format(",{:.0f}", value / frames);

   file << row << "\n";
}

void log_period(const std::uint32_t width, const std::uint32_t height) noexcept
{
   Period& p = state.period;
   const double seconds = static_cast<double>(GetTickCount64() - state.period_start) / 1000.0;

   if (p.frames == 0 || seconds <= 0.0) return;

   const double frames = p.frames;
   const double gpu_frames = std::max<double>(p.gpu_frames, 1.0);

   std::array<double, part_count> cpu{};

   for (int i = 0; i < part_count; ++i) cpu[i] = p.cpu_ms[i] / frames;

   // The VR part is all of submit_frame: its waits and xrEndFrame are inside it.
   cpu[static_cast<int>(Cpu_part::vr)] =
      std::max(0.0, cpu[static_cast<int>(Cpu_part::vr)] - cpu[static_cast<int>(Cpu_part::wait)] -
                       cpu[static_cast<int>(Cpu_part::submit)]);

   const double frame_avg = seconds * 1000.0 / frames;
   double cpu_known = 0.0;

   for (const double value : cpu) cpu_known += value;

   const double cpu_game = std::max(0.0, frame_avg - cpu_known);
   const float cpu_p50 = percentile(p.frame_ms, 0.5f);
   const float cpu_p95 = percentile(p.frame_ms, 0.95f);

   std::array<double, group_count> gpu{};
   double gpu_sum = 0.0;

   for (int i = 0; i < group_count; ++i) {
      gpu[i] = p.gpu_ms[i] / gpu_frames;
      gpu_sum += gpu[i];
   }

   const float gpu_p50 = percentile(p.gpu_frame_ms, 0.5f);
   const float gpu_p95 = percentile(p.gpu_frame_ms, 0.95f);

   std::string groups;

   for (int i = 0; i < group_count; ++i) {
      groups += fmt::format("{}{} {:.2f}", i ? ", " : "", group_names[i], gpu[i]);
   }

   std::string draws;

   for (int i = 0; i < group_count; ++i) {
      if (p.draws[i] == 0) continue;

      draws += fmt::format("{}{} {:.0f}", draws.empty() ? "" : ", ", group_names[i],
                           p.draws[i] / frames);
   }

   log_fmt(Log_level::info,
           "VR perf: {:.1f} fps at {:.0f} Hz ({} frames, {} stereo, {:.1f}% missed), {}x{} "
           "{} views. CPU ms/frame: frame {:.2f} (median {:.2f}, 95% {:.2f}) = game {:.2f} + "
           "draws {:.2f} + vr {:.2f} + wait {:.2f} + submit {:.2f} + present {:.2f}. GPU "
           "ms/frame: {:.2f} (median {:.2f}, 95% {:.2f}; {} frames read, {} dropped, {} "
           "disjoint, {} over {} marks) = {}. Draws/frame: {}.",
           frames / seconds, p.refresh_sum / static_cast<float>(frames), p.frames,
           p.stereo_frames, 100.0 * p.missed / frames, width, height, p.views_max,
           frame_avg, cpu_p50, cpu_p95, cpu_game, cpu[0], cpu[1], cpu[2], cpu[3], cpu[4],
           gpu_sum, gpu_p50, gpu_p95, p.gpu_frames, p.gpu_dropped, p.gpu_disjoint,
           p.capped, max_marks, groups, draws.empty() ? "none" : draws);

   write_csv(p, seconds, cpu, cpu_game, cpu_p50, cpu_p95, gpu, gpu_p50, gpu_p95, width,
             height);
}

}

auto ticks_now() noexcept -> std::int64_t
{
   if (!enabled) return 0;

   LARGE_INTEGER now{};
   QueryPerformanceCounter(&now);

   return now.QuadPart;
}

void gpu_mark(ID3D11DeviceContext& dc, const Gpu_group group) noexcept
{
   if (!enabled) return;

   if (!vr::active() || !ensure_ready(dc)) return;

   const auto value = static_cast<std::uint8_t>(group);

   if (state.open && state.last_group == value) return;
   if (!state.open) open_slot(dc);

   Slot& slot = state.slots[state.current];

   // One left for the frame's end.
   if (slot.marks >= max_marks) return;

   stamp(dc, slot, value);
   state.last_group = value;
}

void cpu_add(const Cpu_part part, const std::int64_t ticks) noexcept
{
   state.frame_cpu[static_cast<int>(part)] += ticks;
}

void count_draw(const Gpu_group group) noexcept
{
   state.frame_draws[static_cast<int>(group)] += 1;
}

void note_vr_frame(const bool stereo, const float refresh_hz, const int views) noexcept
{
   state.frame_stereo = stereo;
   state.frame_refresh = refresh_hz;
   state.frame_views = views;
}

void frame_end(ID3D11DeviceContext& dc, const std::uint32_t width,
               const std::uint32_t height) noexcept
{
   if (!enabled) return;

   if (!vr::active() || !ensure_ready(dc)) return;

   const std::int64_t now = ticks_now();
   Period& p = state.period;

   if (state.period_start == 0) state.period_start = GetTickCount64();

   // CPU: present to present.
   if (state.last_frame_end != 0) {
      const double frame_ms = ms(now - state.last_frame_end);

      p.frames += 1;
      p.stereo_frames += state.frame_stereo ? 1 : 0;
      p.frame_ms.push_back(static_cast<float>(frame_ms));
      p.refresh_sum += state.frame_refresh;
      p.views_max = std::max(p.views_max, state.frame_views);

      if (state.frame_refresh > 0.0f && frame_ms > 1.5 * 1000.0 / state.frame_refresh) {
         p.missed += 1;
      }

      for (int i = 0; i < part_count; ++i) p.cpu_ms[i] += ms(state.frame_cpu[i]);
      for (int i = 0; i < group_count; ++i) p.draws[i] += state.frame_draws[i];
   }

   state.last_frame_end = now;
   state.frame_cpu = {};
   state.frame_draws = {};

   // GPU: close this frame's slot.
   if (state.open) {
      Slot& slot = state.slots[state.current];

      if (slot.marks >= max_marks) p.capped += 1;

      stamp(dc, slot, end_mark);
      dc.End(slot.disjoint.get());
      slot.pending = true;
      slot.stereo = state.frame_stereo;
      state.open = false;
      state.current = (state.current + 1) % slot_count;
   }

   // Oldest first; stop at the first the GPU hasn't finished.
   for (int i = 0; i < slot_count; ++i) {
      Slot& slot = state.slots[(state.current + i) % slot_count];

      if (!slot.pending) continue;
      if (!read_slot(dc, slot)) break;
   }

   if (GetTickCount64() - state.period_start >= period_ms) {
      log_period(width, height);

      state.period = {};
      state.period.frame_ms.reserve(1500);
      state.period.gpu_frame_ms.reserve(1500);
      state.period_start = GetTickCount64();
   }
}

}
