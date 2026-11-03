# bf2vr

6DOF PC VR (head + both hands) mod for **Star Wars Battlefront II (2005, "Classic")**,
Steam / GOG release, through OpenXR — SteamVR, Meta/Oculus, Virtual Desktop, WMR or any
other PC VR runtime.

The game is drawn in stereo with head tracking, the right controller aims and holds the
weapon, both controllers drive melee, and menus and the HUD are shown on panels in the
headset. Infantry first; vehicles and starfighters come later.

> This is an unofficial fan mod. It is not made, approved or supported by Lucasfilm,
> Disney, Electronic Arts or Aspyr, and it contains no game files. Star Wars and related
> names are trademarks of their owners.

## Playing

> [!WARNING]
> **Steam version only.** The mod supports **Star Wars Battlefront II (Classic, 2005) from Steam** only.
> Other copies (the old 2006 GOG installer, cracked or repacked games) use a different
> `BattlefrontII.exe`: the game will show an error message and close. **Steam Classic Battlefront 2 2005 is required.**

Download the zip from [Releases](../../releases) and follow `bf2vr readme.txt` inside it. In short:

- Star Wars Battlefront II (Classic, 2005), Steam as it installs — the 32-bit
  game, not the Classic Collection. No Unofficial 1.3 Patch or "4GB patch" needed (the
  mod **also** works on top of UOP r130).
- An OpenXR runtime set as active (SteamVR or your headset's PC app).

## Controls (Meta Quest 3 / Touch controllers)

The buttons press the keys bound in your game profile, so rebinding in the game's own
Controls screen still works.

### On foot

| Controller | Action |
|---|---|
| Right trigger | Fire |
| Left trigger | Secondary fire. Grenades: hold, swing the left hand, let go to throw |
| Left stick | Move |
| Left stick click | Sprint (until the stick is let go; can be set to "hold") |
| Right stick left / right | Snap turn (smooth turn in the VR panel) |
| Right stick up / down | Next primary / secondary weapon; with the scope up: change scope zoom |
| Right stick click | Crouch (crouch while strafing to roll) |
| A | Reload; hold 0.5 s: lock target |
| B | Jump |
| X | Use / enter vehicle / change class at a command post; Enter in menus |
| Y | Pause; hold 0.5 s in a match: VR settings panel |
| Left menu button (≡) | Squad command |
| Left grip | Hold the weapon with both hands |
| Right grip | Scope (hold; double press: next zoom) |
| Both grips (lightsaber) | Block |
| Raise the gun to your eye | Scope comes up by itself |
| Fast jab or swing with the gun | Melee hit (weapon bash) |
| Crouch in your room | Soldier crouches (standing mode) |
| Free hand's grip at the weapon | Swap the weapon to that hand |

### Vehicles and starfighters

| Controller | Ground vehicle | Starfighter |
|---|---|---|
| Right / left trigger | Primary / secondary fire | Primary / secondary fire |
| Left stick | Drive | Throttle and roll |
| Left stick click | Boost | Boost |
| Right stick | Steer / aim | Pitch and turn |
| Right stick click | Droideka: roll up / deploy | Trick |
| Right grip (hold) | Zoom | Roll lock |
| A | Reload; hold: lock target | Reload; hold: lock target |
| B | Jump | Land / take off |
| X | Exit | Exit |
| Y | Pause; hold: VR panel | Pause; hold: VR panel |
| Left grip + right stick up / down | Next / previous seat | Next / previous seat |
| Left grip + right stick click | First / third person view | First / third person view |
| Left grip + B | — | Immelmann |
| Left grip + Y | Map (while held) | Map (while held) |

## Feedback

Feel free to share bugs/errors on a [bug report](../../issues/new?template=bug_report.yml) and attach the
log from `GameData\bf2vr_logs`. The mod never sends anything anywhere.

Drop in Discord for chat: https://discord.gg/jmeSQ5SVjb

Oh, and buy me a Ko-Fi! https://ko-fi.com/addvans

## Known Issues

So far there are recognizable issues:

- Lightsaber sound is annoying. While not technically a bug, it is on a to-do list.
- Big maps (Hoth XL mode, Kashyyk XL mode) can cause rendering issues and crashes.
- B2 Battle Droid arm can fire off crosshair sometimes.
- Small characters (Ewoks, Jawas, Yoda) can cause units and/or stationary droids to render incorrectly.
- Some characters hands have wrong positioning. See "Setting up hands" for more.
- Droidekas and vehicles have theater mode type rendering. This is not an issue, but something to be implemented later.
- There could be many unnoticed issues yet, as this is a new field for me. Feedback is greatly appreciated.

## Setting up hands

Most units have hands set up relatively correct. That being said, mod automatically get weapon model and uses "left" and "right" hands for rendering. With that in mind, some characters have their hands calculated wrong, like Fusion Cutters, Leia Organa's hands (for some reason), and many more.

Mod has built-in hands editor. While holding the weapon that needs to be adjusted, hold Pause Button to open up VR Settings menu mid-match. Editor offers the ability to switch which parts of the model renders on the left & right hands separately, as well as the ability to add XYZ offsets to models to better match controller positions. There's also function to mirror weapon model to render Clones weapons properly, as some of them are only one-side rendered.

## How it is built

bf2vr is a fork of [Shader Patch](https://github.com/PrismaticFlower/shaderpatch) by
PrismaticFlower (v1.11.2), which already replaces the game's Direct3D 9 renderer with
Direct3D 11. The VR layer is added on top of it:

| Path | What |
|---|---|
| `src/vr/` | the VR layer: OpenXR session, stereo rendering, head and hand tracking, controls, melee, HUD/menu panels, settings |
| `src/vr/third_party/openxr/` | Khronos OpenXR headers |
| `src/core/`, `src/direct3d/`, `src/*_hooks.cpp`, `assets/core/src/` | Shader Patch code with VR hooks added |
| `assets/`, `shared/`, `src/` (rest) | Shader Patch, unchanged |
| `tools/lvl_pack`, `tools/texture_munge` | Shader Patch tools needed to build `core.lvl` |
| `vcpkg-overlay/` | a fixed `sol2` port (the 3.5.0 port has a typo that breaks the build) |
| `release/` | the readme shipped in the release zip |

Shader Patch's other tools (installer, material/effects munging, texture array maker,
shader cache primer) are not needed by the mod and are left out; get them from upstream.

## Building

Requirements:

- Visual Studio 2022 with **Desktop development with C++** (MSVC v143) and Windows SDK
  10.0.26100. VS 2026 is not supported by the pinned vcpkg.
- [vcpkg](https://github.com/microsoft/vcpkg) checked out at commit
  `501e463505cdd76b425e145208dc815d99443c69` (2025-08-27) — the last one where `abseil`
  still has the `cxx17` feature `vcpkg.json` asks for — and bootstrapped:

  ```powershell
  git clone https://github.com/microsoft/vcpkg C:\vcpkg
  git -C C:\vcpkg checkout 501e463505cdd76b425e145208dc815d99443c69
  C:\vcpkg\bootstrap-vcpkg.bat
  ```

Build the mod (`bin\Release\d3d9.dll`; the first build also builds every dependency,
which takes a while):

```powershell
.\scripts\build.ps1 -VcpkgRoot C:\vcpkg
```

`vcpkg-configuration.json` points vcpkg at the `sol2` overlay automatically.

Then pack the release zip (`packages\CBF2VR.zip`, unpacked by players into `GameData`):

```powershell
.\scripts\package.ps1 -OpenXrLoader <path>\openxr_loader.dll -TexturesLvl <path>\textures.lvl
```

- `openxr_loader.dll` — the 32-bit (`Win32`) loader from the
  [OpenXR SDK releases](https://github.com/KhronosGroup/OpenXR-SDK/releases) (1.1.65 is used).
- `textures.lvl` — `data\shaderpatch\textures.lvl`, unchanged from Shader Patch v1.11.2:
  take it from the official release, or build the `core` project of `shader_patch.sln`
  (it builds the x64 `lvl_pack` and `texture_munge` tools first).
- `d3dcompiler_47.dll` is taken from the Windows SDK's `Redist\D3D\x86`.

Everything else comes from the repository: `release\` (readme, default HUD layout,
license of the loader), `assets\shader patch.yml`, `assets\core\definitions` and
`assets\core\src` (shaders, compiled by the mod at first start) and `assets\scripts`.

## License

MIT — see [LICENSE](LICENSE). It covers Shader Patch (© PrismaticFlower) and the bf2vr
changes. Third-party libraries and their licenses are listed in
[third_party.md](third_party.md); ship it with releases together with `LICENSE` and the
OpenXR loader's license.
