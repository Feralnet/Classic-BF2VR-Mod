
#include "vr_sound.hpp"
#include "vr_melee.hpp"
#include "vr_settings.hpp"
#include "../logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

#include <Windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")

namespace sp::vr::sound {

namespace {

constexpr std::uint32_t sample_rate = 44100;
constexpr int voice_count = 4;

// Swings: the tip this much faster than the threshold is a full-loudness swing; the
// quietest heard swing is this fraction of full. Each sound adds 1 to a run count that
// fades this much per second; a run of n sounds plays at 1 / (1 + run_quieting x n).
constexpr float swing_full_range = 6.0f; // m/s
constexpr float swing_min_loudness = 0.4f;
constexpr float swing_run_fade = 1.5f;
constexpr float swing_run_quieting = 0.35f;

struct Audio {
   bool tried = false;
   bool ready = false;
   IXAudio2* xaudio = nullptr;
   IXAudio2MasteringVoice* master = nullptr;
   std::array<IXAudio2SourceVoice*, voice_count> voices{};
   int next_voice = 0;
   std::vector<std::int16_t> thud;
   std::vector<std::int16_t> swish;
   LONGLONG last_swing = 0;
   float swing_run = 0.0f;
   bool swing_missing_logged = false;
};

Audio audio;

// A weapon bash: a thud (a falling low tone with a crack of noise at its start), or a
// short swish of filtered noise.
auto make_bash(const bool hit) -> std::vector<std::int16_t>
{
   const float seconds = hit ? 0.28f : 0.16f;
   const int count = static_cast<int>(seconds * sample_rate);
   std::vector<float> samples(count);
   constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
   float phase = 0.0f;
   float noise = 0.0f;
   std::uint32_t seed = 0x6c8e9cf5u;
   float peak = 1e-6f;

   for (int i = 0; i < count; ++i) {
      const float t = static_cast<float>(i) / sample_rate;
      const float u = t / seconds;

      seed = seed * 1664525u + 1013904223u;

      const float white = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
      float sample = 0.0f;

      if (hit) {
         phase += two_pi * (110.0f - 60.0f * u) / sample_rate;
         noise += 0.3f * (white - noise);
         sample = std::sin(phase) * std::exp(-t * 14.0f) + noise * std::exp(-t * 60.0f) * 1.5f;
      }
      else {
         const float envelope = std::sin(std::numbers::pi_v<float> * u);

         noise += (0.05f + 0.1f * envelope) * (white - noise);
         sample = noise * envelope;
      }

      samples[i] = sample;
      peak = std::max(peak, std::abs(sample));
   }

   std::vector<std::int16_t> pcm(count);

   for (int i = 0; i < count; ++i) {
      pcm[i] = static_cast<std::int16_t>(samples[i] / peak * 0.9f * 32767.0f);
   }

   return pcm;
}

bool init() noexcept
{
   // XAudio2 wants COM; the game's thread may already have it in another mode.
   const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

   if (FAILED(com) && com != RPC_E_CHANGED_MODE) return false;

   if (FAILED(XAudio2Create(&audio.xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;

   if (FAILED(audio.xaudio->CreateMasteringVoice(&audio.master))) return false;

   WAVEFORMATEX format{};
   format.wFormatTag = WAVE_FORMAT_PCM;
   format.nChannels = 1;
   format.nSamplesPerSec = sample_rate;
   format.wBitsPerSample = 16;
   format.nBlockAlign = 2;
   format.nAvgBytesPerSec = sample_rate * 2;

   for (IXAudio2SourceVoice*& voice : audio.voices) {
      if (FAILED(audio.xaudio->CreateSourceVoice(&voice, &format, 0, 2.0f))) return false;
   }

   try {
      audio.thud = make_bash(true);
      audio.swish = make_bash(false);
   }
   catch (...) {
      return false;
   }

   return true;
}

auto seconds_since(const LONGLONG then) noexcept -> float
{
   LARGE_INTEGER now{}, frequency{};
   QueryPerformanceCounter(&now);
   QueryPerformanceFrequency(&frequency);

   return static_cast<float>(now.QuadPart - then) / static_cast<float>(frequency.QuadPart);
}

}

bool ready() noexcept
{
   if (!audio.tried) {
      audio.tried = true;
      audio.ready = init();

      log(audio.ready ? Log_level::info : Log_level::warning,
          audio.ready ? "VR: weapon bash sounds ready (XAudio2)."
                      : "VR: XAudio2 unavailable, weapon bashes are silent.");
   }

   return audio.ready;
}

void play(const std::vector<std::int16_t>& pcm, const float volume) noexcept
{
   IXAudio2SourceVoice* const voice = audio.voices[audio.next_voice];

   audio.next_voice = (audio.next_voice + 1) % voice_count;

   XAUDIO2_BUFFER buffer{};
   buffer.Flags = XAUDIO2_END_OF_STREAM;
   buffer.AudioBytes = static_cast<UINT32>(pcm.size() * sizeof(std::int16_t));
   buffer.pAudioData = reinterpret_cast<const BYTE*>(pcm.data());

   voice->Stop();
   voice->FlushSourceBuffers();
   voice->SubmitSourceBuffer(&buffer);
   voice->SetVolume(volume);
   voice->SetFrequencyRatio(1.0f);
   voice->Start();
}

void play_bash(const bool hit) noexcept
{
   if (!ready()) return;

   play(hit ? audio.thud : audio.swish, hit ? 0.8f : 0.4f);
}

void play_swing(const float tip_speed, const glm::vec3& position) noexcept
{
   const Settings& s = settings();

   if (!s.saber_sound || s.saber_sound_volume <= 0.0f) return;
   if (tip_speed < s.saber_sound_speed) return;

   const float since = audio.last_swing ? seconds_since(audio.last_swing) : 1e6f;

   if (since < s.saber_sound_interval) return;

   audio.swing_run = std::max(audio.swing_run - since * swing_run_fade, 0.0f);

   const float strength =
      std::clamp((tip_speed - s.saber_sound_speed) / swing_full_range, 0.0f, 1.0f);
   const float loudness = swing_min_loudness + (1.0f - swing_min_loudness) * strength;
   const float volume =
      s.saber_sound_volume * loudness / (1.0f + swing_run_quieting * audio.swing_run);

   // The game's sounds tried, first loaded wins: sabers', then the Wampa's (the stock
   // melee sides' combo files).
   if (!melee::play_game_sound({"saber_swing", "com_weap_melee_swing"}, position, volume)) {
      if (!std::exchange(audio.swing_missing_logged, true)) {
         log(Log_level::warning, "VR: no swing sound of the game is loaded (saber_swing, "
                                 "com_weap_melee_swing): saber swings are silent.");
      }

      return;
   }

   LARGE_INTEGER now{};
   QueryPerformanceCounter(&now);
   audio.last_swing = now.QuadPart;
   audio.swing_run += 1.0f;
}

}
