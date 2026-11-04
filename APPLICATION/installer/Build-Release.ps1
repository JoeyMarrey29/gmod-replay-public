[CmdletBinding()]
param(
    [ValidateSet('private', 'public')]
    [string]$Edition = 'private',
    [string]$CMake = 'cmake',
    [string]$Compiler = "$env:LOCALAPPDATA\Programs\GModReplayBuildTools\InnoSetup\ISCC.exe",
    [string]$CrtDirectory = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$build = Join-Path $root $(if ($Edition -eq 'public') { 'build\vbuild-public' } else { 'build\vbuild' })
& $CMake -S (Join-Path $root 'APPLICATION') -B $build -A x64 "-DGMOD_REPLAY_EDITION=$Edition"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed. Initialize submodules first.' }
& $CMake --build $build --config Release --target gmod-replay-application gmod-replay-viewing-policy-tests gmod-replay-combat-tests gmod-replay-vtf-tests gmod-replay-release-notes-tests gmod-replay-model-pose-tests gmod-replay-gpu-cache-tests gmod-replay-menu-settings-tests gmod-replay-audio-tests gmod-replay-npc-gui-tests gmod-replay-frame-text-tests gmod-replay-bank-baked-tests --parallel
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
& "$build\Release\gmod-replay-viewing-policy-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Viewing policy regression failed.' }
& "$build\Release\gmod-replay-combat-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Native combat regression failed.' }
& "$build\Release\gmod-replay-npc-gui-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'NPC GUI timeline regression failed.' }
& "$build\Release\gmod-replay-bank-baked-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Baked bank lighting regression failed.' }
& "$build\Release\gmod-replay-frame-text-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'In-frame ESP rendering regression failed.' }
& "$build\Release\gmod-replay-vtf-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Native texture regression failed.' }
& "$build\Release\gmod-replay-release-notes-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Release notes regression failed.' }
& "$build\Release\gmod-replay-gpu-cache-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'GPU asset cache regression failed.' }
& "$build\Release\gmod-replay-menu-settings-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Menu and audio settings regression failed.' }
& "$build\Release\gmod-replay-audio-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Audio playback regression failed.' }
if ($env:GMOD_DOOR_MODEL_PATH) {
    & "$build\Release\gmod-replay-model-pose-tests.exe" $env:GMOD_DOOR_MODEL_PATH
    if ($LASTEXITCODE -ne 0) { throw 'Door model-pose regression failed.' }
}
& "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$root\APPLICATION\tests\UpdateTests.ps1"
if ($LASTEXITCODE -ne 0) { throw 'Updater regression failed.' }
& dotnet test "$root\gmod-demo-parser-app\tests\GmodDemo.Parser.Tests\GmodDemo.Parser.Tests.csproj" -c Release
if ($LASTEXITCODE -ne 0) { throw 'Parser regression failed.' }
& "$PSScriptRoot\Build-Installer.ps1" -Edition $Edition -Compiler $Compiler -CrtDirectory $CrtDirectory
