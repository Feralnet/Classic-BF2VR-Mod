#pragma once

// Sounds the game doesn't make in VR. The saber's swing: the game's melee sounds come
// with its combo attacks, which the controller's swings don't start (see vr_melee.cpp),
// so a swing's whoosh is made here, through XAudio2 next to the game's own audio.
namespace sp::vr::sound {

// A saber swing whose blade tip moves at `tip_speed` (m/s): louder and higher when
// faster. Swings closer together than a short interval play once.
void play_swing(float tip_speed) noexcept;

// A weapon bash: a low thud when it hit something (`hit`), else a short swish.
void play_bash(bool hit) noexcept;

}
