#requires -Version 5.1
<#
    Full-solution build for SamClassicPatch.sln (Release_TSE107, x86) using the
    native toolchain: MSVC 6.0 (with SP6) driven through Visual Studio 2010 with
    the Daffodil platform toolset "v60". See BUILD.md for prerequisites.

    The whole solution is built (not just Core) so every binary is produced by the
    same compiler as the release, matching the Steam engine/binaries' ABI.

    Usage:
        .\build.ps1                       # build the full solution -> build.log
        .\build.ps1 -Clean                # wipe Obj\ + Bin\ for this config first
        .\build.ps1 -Deploy               # build, then stage outputs into ..\deploy\Bin
        .\build.ps1 -Deploy -DeployTo D:\some\dir

    Notes:
      * The PlatformToolset is intentionally NOT overridden on the command line;
        each project's Release_TSE107 configuration resolves "v60" on its own.
      * -Deploy never writes into a Steam library: the target path is resolved
        through any junctions and rejected if it lands under "steamapps\common".
#>
[CmdletBinding()]
param(
    [string]$Config = 'Release_TSE107',
    [string]$Platform = 'x86',
    [switch]$Clean,
    [switch]$Deploy,
    [string]$DeployTo
)

$ErrorActionPreference = 'Stop'

$repo    = $PSScriptRoot                                      # SuperProject
$root    = Split-Path -Path $repo -Parent                     # workspace root
$sln     = Join-Path $repo 'SamClassicPatch.sln'
$srcBin  = Join-Path $repo "Bin\$Config"
$logPath = Join-Path $repo 'build.log'

if ([string]::IsNullOrEmpty($DeployTo)) { $DeployTo = Join-Path $root 'deploy\Bin' }

Write-Host "== build.ps1  Config=$Config  Platform=$Platform  Deploy=$([bool]$Deploy) =="

# ── 1. Locate the native (v60) toolchain ─────────────────────────────────────
$pf86      = ${env:ProgramFiles(x86)}
$vs10Dir   = Join-Path $pf86 'Microsoft Visual Studio 10.0'
$msbuild4  = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\MSBuild.exe'
$vctargets = Join-Path $pf86 'MSBuild\Microsoft.Cpp\v4.0'
$v60props  = Join-Path $vctargets 'Platforms\Win32\PlatformToolsets\v60\Microsoft.Cpp.Win32.v60.props'

$prereqs = "Native v60 build prerequisites (per SE1-ModSDK wiki 'Building'):`n" +
           "  1. Microsoft Visual C++ 6.0 with Service Pack 6;`n" +
           "  2. Visual Studio 2010 (Visual C++ installed);`n" +
           "  3. Daffodil (manusoft-daffodil R2.6.6) which adds the 'v60' toolset to VS2010."

if (-not (Test-Path -LiteralPath $vs10Dir)) {
    throw "Visual Studio 2010 not found at '$vs10Dir'.`n$prereqs"
}
if (-not (Test-Path -LiteralPath $vctargets)) {
    throw "VC build targets not found at '$vctargets'. Install Visual Studio 2010 with Visual C++.`n$prereqs"
}
if (-not (Test-Path -LiteralPath $v60props)) {
    throw "Platform toolset 'v60' not found ('$v60props' missing). Install Daffodil.`n$prereqs"
}
if (-not (Test-Path -LiteralPath $msbuild4)) {
    throw "MSBuild 4.0 not found at '$msbuild4'. Install Visual Studio 2010.`n$prereqs"
}
Write-Host "MSBuild: $msbuild4  (toolset v60 via Daffodil)"

# ── 2. Warn if a required submodule is empty ─────────────────────────────────
foreach ($sub in 'API', 'Includes', 'Extras\XGizmo') {
    $p = Join-Path $repo $sub
    if (-not (Test-Path -LiteralPath $p) -or -not (Get-ChildItem -LiteralPath $p -Force -ErrorAction SilentlyContinue)) {
        Write-Warning "submodule '$sub' looks empty - run: git submodule update --init --recursive"
    }
}

# ── 3. Optional clean ────────────────────────────────────────────────────────
if ($Clean) {
    foreach ($d in (Join-Path $repo "Obj\$Config"), (Join-Path $repo "Bin\$Config")) {
        if (Test-Path -LiteralPath $d) { Write-Host "Clean: $d"; Remove-Item -LiteralPath $d -Recurse -Force }
    }
}

# ── 4. Build the full solution (output -> build.log) ─────────────────────────
$msbuildArgs = @(
    $sln,
    '/nologo',
    '/m',
    "/p:Configuration=$Config",
    "/p:Platform=$Platform"
)
Write-Host "Command: MSBuild $($msbuildArgs -join ' ')"
Write-Host "Log:     $logPath"

$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $msbuild4 @msbuildArgs 2>&1 | Out-File -LiteralPath $logPath -Encoding utf8
$code = $LASTEXITCODE
$sw.Stop()

# ── 5. Parse the log: error lines + per-project pass/fail ────────────────────
$errPattern  = '(?i):\s*fatal error\b|:\s*error\s+[A-Z]|^\s*error\b|error MSB\d+|treated as error'
$projStart   = 'Project "([^"]+\.(?:vcxproj|csproj|sln))"\s+on node'
$projOutRe   = '->\s*([^\s]+\.(?:dll|exe|lib|pdb))\b'

$log         = @(Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue)
$errLines    = @()
$projErrors  = @{}
$projOutput  = @{}
$current     = '(solution)'

foreach ($line in $log) {
    if ($line -match $projStart) { $current = [System.IO.Path]::GetFileName($Matches[1]) }
    if ($line -match $errPattern) {
        $errLines += $line
        if ($current) { $projErrors[$current] = 1 + [int]($projErrors[$current]) }
    }
    if ($line -match $projOutRe -and $current) { $projOutput[$current] = $Matches[1] }
}

if ($projErrors.Count -gt 0 -or $projOutput.Count -gt 0) {
    $names = @($projErrors.Keys + $projOutput.Keys | Sort-Object -Unique)
    Write-Host ""
    Write-Host "--- per-project result ---"
    foreach ($n in $names) {
        $e = [int]($projErrors[$n]); $o = $projOutput[$n]
        $verdict = if ($e -gt 0) { 'FAIL' } else { 'PASS' }
        Write-Host ("  {0,-6} {1}{2}" -f $verdict, $n, $(if ($o) { "  -> $o" } else { "" }))
    }
}

if ($errLines.Count -gt 0) {
    Write-Host ""
    Write-Host "--- errors / warnings-as-errors (first 40 of $($errLines.Count)) ---"
    $errLines | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
    if ($errLines.Count -gt 40) { Write-Host "... ($($errLines.Count - 40) more, see build.log)" }
}

$status = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
Write-Host ""
Write-Host ("Result:  {0} (exit {1})  errors: {2}" -f $status, $code, $errLines.Count)
Write-Host ("Elapsed: {0:hh\:mm\:ss\.fff}" -f $sw.Elapsed)

$coreDll = Join-Path $srcBin 'ClassicsCore.dll'
if (Test-Path -LiteralPath $coreDll) {
    $h = (Get-FileHash -LiteralPath $coreDll -Algorithm SHA256).Hash
    $t = (Get-Item -LiteralPath $coreDll).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
    Write-Host "ClassicsCore.dll  SHA-256: $h"
    Write-Host "ClassicsCore.dll  built:   $t"
} else {
    Write-Warning "ClassicsCore.dll not found at $coreDll"
}

# ── 6. Deploy (stage only; never into a Steam library) ───────────────────────
function Get-RealPath {
    param([string]$Path)
    $full  = [System.IO.Path]::GetFullPath($Path)
    $parts = $full -split '[\\/]'
    $acc   = $parts[0] + '\'
    for ($k = 1; $k -lt $parts.Count; $k++) {
        if ([string]::IsNullOrEmpty($parts[$k])) { continue }
        $acc = Join-Path $acc $parts[$k]
        if (Test-Path -LiteralPath $acc) {
            $it = Get-Item -LiteralPath $acc -Force -ErrorAction SilentlyContinue
            if ($it) { $t = $it.ResolveLinkTarget($true); if ($t) { $acc = $t.FullName } }
        }
    }
    return [System.IO.Path]::GetFullPath($acc)
}

if ($Deploy) {
    $realTarget = Get-RealPath $DeployTo
    if ($realTarget -match '(?i)steamapps[\\/]common[\\/]') {
        throw "Refusing to deploy: '$DeployTo' resolves into a Steam library ('$realTarget'). Pass -DeployTo to a non-Steam staging directory."
    }
    if ($code -ne 0) { throw "Refusing to deploy: build failed (exit $code)." }

    Write-Host ""
    Write-Host "Deploy -> $DeployTo  (real: $realTarget)"
    if (-not (Test-Path -LiteralPath $DeployTo)) { New-Item -ItemType Directory -Path $DeployTo -Force | Out-Null }

    # Bin-targeted file list from 'DeployToBin - Release.bat' (Mods robocopy +
    # resource packing are outside Bin and are not staged here).
    $binList = @(
        'ClassicsCore.dll', 'ClassicsExtras_Custom.dll', 'DedicatedServer_Custom.exe',
        'SeriousSam_Custom.exe', 'SeriousEditor_Custom.exe', 'Shaders.dll', 'EngineGUI.dll',
        'Entities_Custom.dll', 'Game_Custom.dll', 'GameGUI_Custom.dll'
    )

    $buildStart = $sw.StartTime
    $copied = 0; $skipped = 0

    function Deploy-File {
        param([string]$srcPath, [string]$destDir)
        $name = Split-Path -Leaf $srcPath
        if (-not (Test-Path -LiteralPath $srcPath)) { Write-Host "  skip  $name (absent)"; $script:skipped++; return }
        if ((Get-Item -LiteralPath $srcPath).LastWriteTime -lt $buildStart) { Write-Host "  skip  $name (not rebuilt)"; $script:skipped++; return }
        if (-not (Test-Path -LiteralPath $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        Copy-Item -LiteralPath $srcPath -Destination $destDir -Force
        Write-Host "  copy  $name"
        $script:copied++
    }

    foreach ($f in $binList) { Deploy-File -srcPath (Join-Path $srcBin $f) -destDir $DeployTo }

    $pluginsSrc = Join-Path $srcBin 'Plugins'
    if (Test-Path -LiteralPath $pluginsSrc) {
        Get-ChildItem -LiteralPath $pluginsSrc -Filter '*.dll' | ForEach-Object {
            Deploy-File -srcPath $_.FullName -destDir (Join-Path $DeployTo 'Plugins')
        }
    }

    Deploy-File -srcPath (Join-Path $repo 'Extras\SDL\lib\x86\SDL3.dll') -destDir $DeployTo

    Write-Host "Deploy:  $copied copied, $skipped skipped"
    Write-Host "Note: copy the staged files into the testbed yourself (testbed is a Steam junction)."
}

exit $code
