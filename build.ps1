#requires -Version 5.1
<#
    Reproducible build for the DS3D spatial work.

    Scope: Core project only -> ClassicsCore.dll (Release_TSE107, x86).
    All DS3D code lives in Core, so the other solution projects are not built.
    See BUILD.md for prerequisites and the full command.

    Usage:
        .\build.ps1                 # build ClassicsCore.dll
        .\build.ps1 -Deploy         # build and copy outputs into ..\testbed\Bin
        .\build.ps1 -Config Release_TSE107 -Deploy
#>
[CmdletBinding()]
param(
    [string]$Config = 'Release_TSE107',
    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'

$repo    = $PSScriptRoot                                   # SuperProject
$sln     = Join-Path $repo 'SamClassicPatch.sln'
$srcBin  = Join-Path $repo "Bin\$Config"
$dllPath = Join-Path $srcBin 'ClassicsCore.dll'
$logPath = Join-Path $repo 'build.log'
$toolset = 'v143'

Write-Host "== build.ps1  Config=$Config  Deploy=$([bool]$Deploy) =="

# ── 1. Locate MSBuild via vswhere (VS 2022 or newer) ─────────────────────────
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found at '$vswhere'. Install Visual Studio 2022 (or Build Tools) with the 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' component."
}
$msbuild = & $vswhere -latest -version '[17.0,)' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild for VS 2022+ not found. Install the 'Microsoft.VisualStudio.Component.MSBuild' + 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' components."
}
Write-Host "MSBuild: $msbuild"

# ── 2. Warn if a required submodule is empty ─────────────────────────────────
foreach ($sub in 'API', 'Includes', 'Extras\XGizmo') {
    $p = Join-Path $repo $sub
    if (-not (Test-Path -LiteralPath $p) -or -not (Get-ChildItem -LiteralPath $p -Force -ErrorAction SilentlyContinue)) {
        Write-Warning "submodule '$sub' looks empty - run: git submodule update --init --recursive"
    }
}

# ── 3. Force a fresh Core artifact (leaves zlib.lib etc. in place) ───────────
$objCore = Join-Path $repo "Obj\$Config\Core"
if (Test-Path -LiteralPath $objCore) { Remove-Item -LiteralPath $objCore -Recurse -Force }
Get-ChildItem -LiteralPath $srcBin -Filter 'ClassicsCore.*' -ErrorAction SilentlyContinue | Remove-Item -Force

$buildStart = Get-Date

# ── 4. Build (full output -> build.log; console stays quiet) ─────────────────
$msbuildArgs = @(
    $sln,
    '/t:Core',
    '/nologo',
    '/m',
    "/p:Configuration=$Config",
    '/p:Platform=x86',
    "/p:PlatformToolset=$toolset"
)
Write-Host "Command: MSBuild $($msbuildArgs -join ' ')"
Write-Host "Log:     $logPath"

$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $msbuild @msbuildArgs 2>&1 | Out-File -LiteralPath $logPath -Encoding utf8
$code = $LASTEXITCODE
$sw.Stop()

# ── 5. Console summary: errors only ──────────────────────────────────────────
$errPattern = '(?i)(:\s*fatal error\b|:\s*error\s+[A-Z]|^\s*error\b|error MSB\d+|treated as error)'
$errLines = @(Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue | Where-Object { $_ -match $errPattern })
if ($errLines.Count -gt 0) {
    Write-Host ""
    Write-Host "--- errors / warnings-as-errors (first 40 of $($errLines.Count)) ---"
    $errLines | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
    if ($errLines.Count -gt 40) { Write-Host "... ($($errLines.Count - 40) more, see build.log)" }
}

$status = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
Write-Host ""
Write-Host ("Result:  {0} (exit {1})" -f $status, $code)
Write-Host ("Elapsed: {0:hh\:mm\:ss\.fff}" -f $sw.Elapsed)

if (Test-Path -LiteralPath $dllPath) {
    $h = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash
    $t = (Get-Item -LiteralPath $dllPath).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
    Write-Host "ClassicsCore.dll  SHA-256: $h"
    Write-Host "ClassicsCore.dll  built:   $t"
} else {
    Write-Warning "ClassicsCore.dll not found at $dllPath"
}

# ── 6. Deploy to testbed (Bin only) ──────────────────────────────────────────
if ($Deploy) {
    $root       = Split-Path -Path $repo -Parent          # workspace root
    $testbedBin = Join-Path $root 'testbed\Bin'

    if ($testbedBin -notmatch 'testbed') {
        throw "Refusing to deploy: target path does not contain 'testbed': $testbedBin"
    }
    if (-not (Test-Path -LiteralPath $testbedBin)) {
        throw "Refusing to deploy: testbed Bin not found: $testbedBin"
    }

    Write-Host ""
    Write-Host "Deploy -> $testbedBin"

    # Bin-targeted file list from 'DeployToBin - Release.bat'
    $binList = @(
        'ClassicsCore.dll', 'ClassicsExtras_Custom.dll', 'DedicatedServer_Custom.exe',
        'SeriousSam_Custom.exe', 'SeriousEditor_Custom.exe', 'Shaders.dll', 'EngineGUI.dll',
        'Entities_Custom.dll', 'Game_Custom.dll', 'GameGUI_Custom.dll'
    )

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

    foreach ($f in $binList) { Deploy-File -srcPath (Join-Path $srcBin $f) -destDir $testbedBin }

    $pluginsSrc = Join-Path $srcBin 'Plugins'
    if (Test-Path -LiteralPath $pluginsSrc) {
        Get-ChildItem -LiteralPath $pluginsSrc -Filter '*.dll' | ForEach-Object {
            Deploy-File -srcPath $_.FullName -destDir (Join-Path $testbedBin 'Plugins')
        }
    }

    Deploy-File -srcPath (Join-Path $repo 'Extras\SDL\lib\x86\SDL3.dll') -destDir $testbedBin

    Write-Host "Deploy:  $copied copied, $skipped skipped"
}

exit $code
