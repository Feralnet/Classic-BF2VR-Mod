Star Wars Battlefront II (2005) VR mod
======================================

6DOF PC VR (head + both hands) for the Steam / GOG release of Star Wars
Battlefront II (2005, "Classic"), through OpenXR (SteamVR, Oculus/Meta,
Virtual Desktop, WMR - any PC headset).

This is an unofficial fan mod. It is not made, approved or supported by
Lucasfilm, Disney, EA or Aspyr. It contains no game files.

Requirements
------------
- Star Wars Battlefront II (Classic, 2005), Steam or GOG.
- Unofficial v1.3 Patch r130 (UOP), installed BEFORE this mod.
  This build of the mod is made against the game program that UOP r130
  installs; with the game's original BattlefrontII.exe parts of the VR
  (camera, hands, melee) switch themselves off.
- The game's BattlefrontII.exe marked Large Address Aware (the well known
  "4GB patch"). Without it the game can run out of memory in VR.
- An OpenXR runtime (SteamVR or your headset's PC app) set as the active one.

Install
-------
1. Unpack everything in this archive into the game's GameData folder
   (the folder that holds BattlefrontII.exe), replacing files when asked.
2. Put on the headset and start the game from Steam as usual.

Shader Patch is included: this mod is built on it. Do not install the
official Shader Patch over it (and if another mod ships its own d3d9.dll or
data\shaderpatch, unpack this mod again after that mod).

Settings: hold Y in a match, or use the panel next to the menu screen.
They are saved to bf2vr_settings.txt.

Reporting problems
------------------
Every start of the game writes a log to GameData\bf2vr_logs, named by the
date and time the game started (the last 10 are kept). If the game crashes,
a .dmp file with the same name is written next to that run's log.
To report a problem, open an issue on the mod's GitHub page and attach that
run's .log (and .dmp, if any). bf2vr_logs\README.txt says what they contain.
Nothing is ever sent anywhere by the mod.

Uninstall
---------
Delete these from GameData:
  d3d9.dll, d3dcompiler_47.dll, openxr_loader.dll, shader patch.yml,
  shader patch *.txt, openxr loader license.txt, bf2vr*.txt, bf2vr_logs,
  data\shaderpatch
or let Steam verify the game files.

Credits and licenses
--------------------
Built on Shader Patch by PrismaticFlower (MIT, see "shader patch
license.txt"; the libraries it uses are listed in "shader patch
acknowledgements.txt"). OpenXR loader by the Khronos Group (Apache 2.0, see
"openxr loader license.txt").
