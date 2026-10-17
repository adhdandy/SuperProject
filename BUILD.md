# Build (SamClassicPatch.sln, Release_TSE107, x86)

Reproducible **full-solution** build with the **native** toolchain so every binary
matches the Steam release ABI (VC6 runtime, `MSVCP60`/`MSVCRT` imports).

## Prerequisites
Per the SE1-ModSDK wiki (`DreamyCecil/SE1-ModSDK`, page *Building*):
1. **Microsoft Visual C++ 6.0** with **Service Pack 6** (the real compiler).
2. **Visual Studio 2010** with the *Visual C++* workload (`vcxproj` build system).
3. **Daffodil** (`manusoft-daffodil` R2.6.6 MSI) — adds the `v60` platform toolset
   to VS2010:
   https://github.com/manusoft-gh/manusoft-daffodil/releases/download/R2.6.6/Daffodil.2.6.6.ENU.msi
4. Optional: a newer Visual Studio for the IDE (the script uses MSBuild 4.0).
5. Submodules: `git submodule update --init --recursive` (API, Includes, Extras\XGizmo).

The per-project `Release_TSE107` configurations already select `PlatformToolset`
`v60`; **do not** override the toolset on the command line.

## Command
    MSBuild SamClassicPatch.sln /nologo /m /p:Configuration=Release_TSE107 /p:Platform=x86

`build.ps1` locates MSBuild 4.0 (`%WINDIR%\Microsoft.NET\Framework\v4.0.30319`),
verifies VS2010 + the Daffodil `v60` toolset are installed, and throws a precise
prerequisite list if any is missing.

## Usage
    .\build.ps1                 # full solution -> build.log
    .\build.ps1 -Clean          # wipe Obj\ and Bin\ for this config first
    .\build.ps1 -Deploy         # build, then stage outputs into ..\deploy\Bin
    .\build.ps1 -Deploy -DeployTo D:\stage
Exit code = MSBuild result. Console shows per-project PASS/FAIL, errors (max 40),
status/elapsed, and the SHA-256 + timestamp of `ClassicsCore.dll`.

## Deploy
`-Deploy` stages only the `Bin` outputs listed in `DeployToBin - Release.bat`
(the mod `Mods` robocopy and resource packing are not staged).

The target path is **resolved through junctions** and the deploy is refused if it
lands under a Steam library (`steamapps\common`). `testbed\` is a junction into the
live Steam install, so the default staging target is `..\deploy\Bin`; copy the
staged files into the testbed yourself.

## Known quirks
- `.vcxproj` `PlatformToolset` for `Release_TSE107` is `v60` (other configs `v120`).
- Full-solution builds need the projects' MFC/ATL resources (e.g. `afxres.h`).
- `Core/Patches/SoundDS3D.*` and the rest of Core are C++98-clean (the only C++11
  shims are guarded by `_MSC_VER` in `Core/API/ISteam.h`).

## build.log
Full output goes to `build.log`; search with
`Select-String -Path build.log -Pattern ': error'`.
