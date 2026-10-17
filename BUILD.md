# Build (ClassicsCore.dll, Release_TSE107)

Reproducible **Core-only** build: all DS3D changes live in `ClassicsCore.dll`, so only
the `Core` solution project is built (other projects are not, see quirks).

## Prerequisites
- Visual Studio 2022 17.x with `Microsoft.Component.MSBuild` and
  `Microsoft.VisualStudio.Component.VC.Tools.x86.x64` (provides toolset v143).
- Windows SDK 10 with x86 um/ucrt libs + rc.exe (present: 10.0.19041.0).
- `vswhere.exe` (ships with VS).
- MFC is NOT needed for this Core-only scope; a full-solution build needs
  `Microsoft.VisualStudio.Component.VC.ATLMFC`.
- Submodules: `git submodule update --init --recursive` (API, Includes, Extras\XGizmo).

## Command
    MSBuild SamClassicPatch.sln /t:Core /nologo /m /p:Configuration=Release_TSE107 ^
        /p:Platform=x86 /p:PlatformToolset=v143
Output: `Bin\Release_TSE107\ClassicsCore.dll`.

## Usage
    .\build.ps1                 # build
    .\build.ps1 -Deploy         # build + copy outputs to ..\testbed\Bin
    .\build.ps1 -Config Release_TSE107
Exit code = MSBuild result; console shows only errors (max 40), status, elapsed, and
the SHA-256 + timestamp of ClassicsCore.dll.

## Deploy
Target `..\testbed\Bin` (junction to the Steam TSE install). `-Deploy` aborts unless
the path contains "testbed", and copies only outputs produced by this run, so the
testbed's other (API-compatible) binaries are never overwritten.

## Known quirks
- `.vcxproj` still says `PlatformToolset` v60/v120; build.ps1 passes v143 on the
  command line instead of editing the project.
- Other projects (`SeriousSam_Custom.exe`, `Game_Custom.dll`, ...) are NOT rebuilt:
  they already exist in the testbed and must stay API-compatible with Core.
- Full-solution builds currently fail on missing MFC (`afxres.h`).

## build.log
Full output goes to `build.log`; search with `Select-String -Path build.log -Pattern ': error'`.
