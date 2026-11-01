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

- Star Wars Battlefront II (Classic, 2005), Steam or GOG, as it installs — the 32-bit
  game, not the Classic Collection. No Unofficial 1.3 Patch or "4GB patch" needed (the
  mod also works on top of UOP r130).
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
