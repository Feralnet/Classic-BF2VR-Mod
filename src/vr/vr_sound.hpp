#pragma once

#include <glm/glm.hpp>

// Sounds the game doesn't make in VR. The saber's swing: the game's swing sound comes
// with its combo attacks, which the controller's swings don't start (see vr_melee.cpp),
// so the game's own sound ("saber_swing") is played here at the blade. A weapon bash's
// thud is made through XAudio2 next to the game's own audio.
namespace sp::vr::sound {

// A saber swing whose blade tip moves at `tip_speed` (m/s, the player's), the blade's
// middle at `position` (world). Slower than the setting saber_sound_speed is silent;
// faster is louder; a swing sooner than saber_sound_interval after the last sound is
// silent, and a quick run of sounds gets quieter (a waved blade doesn't drone).
void play_swing(float tip_speed, const glm::vec3& position) noexcept;

// A weapon bash: a low thud when it hit something (`hit`), else a short swish.
void play_bash(bool hit) noexcept;

}
