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

Download the zip from [Releases](../../releases) and follow `bf2vr readme.txt` inside it
(the same text as [`release/bf2vr readme.txt`](release/bf2vr%20readme.txt)). In short:

- Star Wars Battlefront II (Classic, 2005), Steam or GOG — the 32-bit game, not the
  Classic Collection.
- The Unofficial v1.3 Patch r130 (UOP), installed before this mod.
- `BattlefrontII.exe` marked Large Address Aware ("4GB patch").
- An OpenXR runtime set as active (SteamVR or your headset's PC app).

Problems: open a [bug report](../../issues/new?template=bug_report.yml) and attach the
log from `GameData\bf2vr_logs`. The mod never sends anything anywhere.

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

`data\shaderpatch\core.lvl` is unchanged from Shader Patch v1.11.2. Take it from the
official release, or build the `core` project of `shader_patch.sln` (it builds the x64
`lvl_pack` and `texture_munge` tools first).

A release zip holds, relative to `GameData`: `d3d9.dll`, `d3dcompiler_47.dll`,
`openxr_loader.dll` (32-bit, from the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK/releases)),
`shader patch.yml`, `data\shaderpatch\core.lvl`, `data\shaderpatch\shaders\` (from
`assets\core\definitions` and `assets\core\src`), `data\shaderpatch\scripts\` (from
`assets\scripts`), and the license files below.

## License

MIT — see [LICENSE](LICENSE). It covers Shader Patch (© PrismaticFlower) and the bf2vr
changes. Third-party libraries and their licenses are listed in
[third_party.md](third_party.md); ship it with releases together with `LICENSE` and the
OpenXR loader's license.
