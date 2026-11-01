# Packs packages\CBF2VR.zip: the files a player unpacks into GameData.
# Run scripts\build.ps1 first.
# Usage: .\scripts\package.ps1 -OpenXrLoader <path>\openxr_loader.dll -TexturesLvl <path>\textures.lvl
param([Parameter(Mandatory)][string]$OpenXrLoader,
      [Parameter(Mandatory)][string]$TexturesLvl,
      [string]$D3DCompiler = "${env:ProgramFiles(x86)}\Windows Kits\10\Redist\D3D\x86\d3dcompiler_47.dll")

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dll = "$root\bin\Release\d3d9.dll"
foreach ($file in $dll, $OpenXrLoader, $TexturesLvl, $D3DCompiler) {
  if (-not (Test-Path $file)) { throw "Missing $file" }
}

$out = "$root\packages\CBF2VR"
if (Test-Path "$root\packages") { Remove-Item "$root\packages" -Recurse -Force }
$sp = "$out\data\shaderpatch"
New-Item -ItemType Directory -Force "$sp\shaders" | Out-Null

Copy-Item $dll, $OpenXrLoader, $D3DCompiler $out
Copy-Item "$root\release\*" $out
Copy-Item "$root\assets\shader patch.yml" $out
Copy-Item "$root\assets\shader patch user readme.txt" "$out\shader patch readme.txt"
Copy-Item "$root\LICENSE" "$out\shader patch license.txt"
Copy-Item "$root\third_party.md" "$out\shader patch acknowledgements.txt"

Copy-Item $TexturesLvl "$sp\textures.lvl"
Copy-Item "$root\assets\core\definitions" "$sp\shaders\definitions" -Recurse
Copy-Item "$root\assets\core\src" "$sp\shaders\src" -Recurse
Copy-Item "$root\assets\scripts" "$sp\scripts" -Recurse

# Windows' tar.exe, not Compress-Archive: PowerShell 5.1 writes '\' into zip entry names.
& "$env:SystemRoot\System32\tar.exe" -a -c -f "$root\packages\CBF2VR.zip" -C $out *
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Packed $root\packages\CBF2VR.zip"
