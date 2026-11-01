
#include "vr_sound.hpp"
#include "../logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include <Windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")

namespace sp::vr::sound {

namespace {

constexpr std::uint32_t sample_rate = 44100;
constexpr float swing_seconds = 0.42f;
constexpr int voice_count = 4;

// Swings: volume and pitch from the tip's speed, and the shortest gap between two.
constexpr float swing_quiet_speed = 4.0f;
constexpr float swing_loud_speed = 11.0f;
constexpr float swing_min_volume = 0.3f;
constexpr float swing_max_volume = 0.7f;
constexpr float swing_min_interval = 0.12f;

struct Audio {
   bool tried = false;
   bool ready = false;
   IXAudio2* xaudio = nullptr;
   IXAudio2MasteringVoice* master = nullptr;
   std::array<IXAudio2SourceVoice*, voice_count> voices{};
   int next_voice = 0;
   std::vector<std::int16_t> swing;
   std::vector<std::int16_t> thud;
   std::vector<std::int16_t> swish;
   LONGLONG last_swing = 0;
};

Audio audio;

// A saber swing: the hum's harmonics bent up and back down as the blade passes (like a
// Doppler shift), with a breath of filtered noise, under a quick swell and a long fade.
auto make_swing() -> std::vector<std::int16_t>
{
   const int count = static_cast<int>(swing_seconds * sample_rate);
   std::vector<float> samples(count);
   constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
   float phase = 0.0f;
   float noise = 0.0f;
   std::uint32_t seed = 0x2545f491u;
   float peak = 1e-6f;

   for (int i = 0; i < count; ++i) {
      const float t = static_cast<float>(i) / sample_rate;
      const float u = t / swing_seconds;
      const float envelope =
         u < 0.3f ? std::pow(u / 0.3f, 1.5f) : std::pow(1.0f - (u - 0.3f) / 0.7f, 1.8f);
      const float bend = (u - 0.35f) / 0.17f;
      const float frequency = 92.0f * (1.0f + 0.5f * std::exp(-bend * bend));

      phase += two_pi * frequency / sample_rate;

      if (phase > two_pi) phase -= two_pi;

      const float hum = std::sin(phase) + 0.55f * std::sin(2.0f * phase + 0.3f) +
                        0.35f * std::sin(3.0f * phase) + 0.2f * std::sin(4.0f * phase + 1.1f) +
                        0.12f * std::sin(6.0f * phase);
      const float wobble = 1.0f + 0.15f * std::sin(two_pi * 31.0f * t);

      seed = seed * 1664525u + 1013904223u;

      const float white = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
      const float cutoff = 0.04f + 0.12f * envelope; // one-pole low-pass

      noise += cutoff * (white - noise);

      const float sample = envelope * (0.6f * hum * wobble + 1.6f * noise);

      samples[i] = sample;
      peak = std::max(peak, std::abs(sample));
   }

   std::vector<std::int16_t> pcm(count);

   for (int i = 0; i < count; ++i) {
      pcm[i] = static_cast<std::int16_t>(samples[i] / peak * 0.9f * 32767.0f);
   }

   return pcm;
}

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
      audio.swing = make_swing();
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
          audio.ready ? "VR: swing sounds ready (XAudio2)."
                      : "VR: XAudio2 unavailable, saber swings are silent.");
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

void play_swing(const float tip_speed) noexcept
{
   if (!audio.tried) {
      audio.tried = true;
      audio.ready = init();

      log(audio.ready ? Log_level::info : Log_level::warning,
          audio.ready ? "VR: swing sounds ready (XAudio2)."
                      : "VR: XAudio2 unavailable, saber swings are silent.");
   }

   if (!audio.ready) return;

   if (audio.last_swing && seconds_since(audio.last_swing) < swing_min_interval) return;

   LARGE_INTEGER now{};
   QueryPerformanceCounter(&now);
   audio.last_swing = now.QuadPart;

   const float strength = std::clamp((tip_speed - swing_quiet_speed) /
                                        (swing_loud_speed - swing_quiet_speed),
                                     0.0f, 1.0f);
   IXAudio2SourceVoice* const voice = audio.voices[audio.next_voice];

   audio.next_voice = (audio.next_voice + 1) % voice_count;

   XAUDIO2_BUFFER buffer{};
   buffer.Flags = XAUDIO2_END_OF_STREAM;
   buffer.AudioBytes = static_cast<UINT32>(audio.swing.size() * sizeof(std::int16_t));
   buffer.pAudioData = reinterpret_cast<const BYTE*>(audio.swing.data());

   voice->Stop();
   voice->FlushSourceBuffers();
   voice->SubmitSourceBuffer(&buffer);
   voice->SetVolume(swing_min_volume + (swing_max_volume - swing_min_volume) * strength);
   voice->SetFrequencyRatio(0.9f + 0.35f * strength);
   voice->Start();
}

}
