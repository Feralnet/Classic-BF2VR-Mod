# Builds bin\Release\d3d9.dll (the mod) with VS 2022 and the pinned vcpkg.
# Usage: .\scripts\build.ps1 -VcpkgRoot C:\path\to\vcpkg
param([string]$VcpkgRoot = $env:VCPKG_ROOT,
      [string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'

if (-not $VcpkgRoot -or -not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
  throw 'Pass -VcpkgRoot (or set VCPKG_ROOT) to a bootstrapped vcpkg checkout.'
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 with the C++ workload was not found.' }

# Newer abseil warns in its headers and fmt 11 requires /utf-8.
$env:_CL_ = '/WX- /utf-8'

$root = Split-Path -Parent $PSScriptRoot
& "$vs\MSBuild\Current\Bin\MSBuild.exe" "$root\shader_patch.vcxproj" /m `
   /p:Configuration=$Configuration /p:Platform=Win32 "/p:VcpkgRoot=$VcpkgRoot\"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
