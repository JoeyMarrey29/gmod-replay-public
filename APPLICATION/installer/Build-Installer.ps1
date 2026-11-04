[CmdletBinding()]
param(
    [ValidateSet('private', 'public')]
    [string]$Edition = 'private',
    [string]$Version = '',
    [string]$GitHubAppClientId = 'Iv23liWStteaaJocHwVu',
    [string]$ViewerPath = '',
    [string]$Compiler = "$env:LOCALAPPDATA\Programs\GModReplayBuildTools\InnoSetup\ISCC.exe",
    [string]$CrtDirectory = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT"
)

$ErrorActionPreference = 'Stop'
$application = Split-Path $PSScriptRoot -Parent
$workspace = Split-Path $application -Parent
if (!$Version) { $Version = (Get-Content -LiteralPath (Join-Path $workspace 'VERSION') -Raw).Trim() }
Import-Module (Join-Path $PSScriptRoot 'UpdateCore.psm1') -Force
$null = ConvertTo-ReplayVersion $Version
$public = $Edition -eq 'public'
if (!$public -and $GitHubAppClientId -cnotmatch '^[A-Za-z0-9]{20}$') {
    throw 'Set GMOD_REPLAY_GITHUB_APP_CLIENT_ID to the 20-character GitHub App Client ID before packaging.'
}
$viewer = if ($ViewerPath) { (Resolve-Path -LiteralPath $ViewerPath).Path } else {
    Join-Path $workspace $(if ($public) { 'build\vbuild-public\Release\gmod-replay-application.exe' } else { 'build\vbuild\Release\gmod-replay-application.exe' })
}
$parserRoot = Join-Path $workspace 'gmod-demo-parser-app'
$nuget = Join-Path $env:USERPROFILE '.nuget\packages'
foreach ($required in @($viewer, $Compiler, $CrtDirectory)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Required packaging input is missing: $required" }
}
if ($Version -notmatch '^[0-9A-Za-z][0-9A-Za-z.-]*$') { throw 'Invalid version string.' }

$output = Join-Path $workspace 'build\installer'
$stage = Join-Path $output ("stage-" + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage 'payload'
$publish = Join-Path $stage 'parser-publish'
$source = Join-Path $stage 'parser-source'
New-Item -ItemType Directory -Force -Path $output, $payload, "$payload\tools", "$payload\licenses", "$payload\source", $source | Out-Null

& dotnet publish "$parserRoot\src\GmodDemo.Cli\GmodDemo.Cli.csproj" -c Release -r win-x64 -o $publish
if ($LASTEXITCODE -ne 0) { throw 'Parser publish failed.' }
Copy-Item -LiteralPath $viewer -Destination "$payload\GModReplay.exe"
Copy-Item -LiteralPath "$publish\gmoddem.exe" -Destination "$payload\tools\gmoddem.exe"
Copy-Item -Path "$CrtDirectory\*.dll" -Destination $payload
$guide = if ($public) { 'PublicGuide.txt' } else { 'TesterGuide.txt' }
Copy-Item -LiteralPath "$PSScriptRoot\$guide" -Destination $payload
Copy-Item -LiteralPath "$PSScriptRoot\Update-GModReplay.ps1","$PSScriptRoot\UpdateCore.psm1" -Destination "$payload\tools"
$release = if ($public) {
    @{ version = $Version; edition = 'public'; repository = 'JoeyMarrey29/gmod-replay-public'; repositoryId = '1446003494' }
} else {
    @{ version = $Version; repository = 'JoeyMarrey29/gmod-replay'; repositoryId = '1428373505'
        appClientId = $GitHubAppClientId }
}
$release | ConvertTo-Json | Set-Content -LiteralPath "$payload\release.json" -Encoding UTF8
New-Item -ItemType Directory -Force -Path "$payload\assets\ui" | Out-Null
Copy-Item -LiteralPath "$application\assets\ui\menu_background.png" -Destination "$payload\assets\ui"

Copy-Item -LiteralPath "$parserRoot\LICENSE" -Destination "$payload\licenses\gmoddem-GPL-3.0.txt"
Copy-Item -LiteralPath "$application\third_party\directxtk12\LICENSE" -Destination "$payload\licenses\DirectXTK12.txt"
Copy-Item -LiteralPath "$nuget\concentus\2.2.2\LICENSE" -Destination "$payload\licenses\Concentus.txt"
Copy-Item -LiteralPath "$PSScriptRoot\MoonSharp-LICENSE.txt" -Destination "$payload\licenses\MoonSharp.txt"
Copy-Item -LiteralPath "$parserRoot\src\GmodDemo.Cli\LzmaSdk\LICENSE.txt" -Destination "$payload\licenses\LZMA.txt"

$assets = Get-Content -LiteralPath "$parserRoot\src\GmodDemo.Cli\obj\project.assets.json" -Raw | ConvertFrom-Json
$runtimeVersion = $assets.project.frameworks.'net10.0'.downloadDependencies |
    Where-Object name -eq 'Microsoft.NETCore.App.Runtime.win-x64' |
    Select-Object -ExpandProperty version -Unique
if (!$runtimeVersion) { throw 'Could not determine the self-contained .NET runtime version.' }
$runtimeVersion = ($runtimeVersion -split ',')[0].Trim('[', ']', ' ')
$runtime = Join-Path $nuget "microsoft.netcore.app.runtime.win-x64\$runtimeVersion"
Copy-Item -LiteralPath "$runtime\LICENSE.TXT" -Destination "$payload\licenses\dotnet-LICENSE.txt"
Copy-Item -LiteralPath "$runtime\THIRD-PARTY-NOTICES.TXT" -Destination "$payload\licenses\dotnet-NOTICES.txt"

# Include the exact modified parser sources, not demo caches or build intermediates.
foreach ($folder in @('src', 'tests')) {
    Get-ChildItem -LiteralPath "$parserRoot\$folder" -Recurse -File |
        Where-Object { $_.FullName -notmatch '\\(bin|obj|TestResults)\\' } |
        ForEach-Object {
            $relative = $_.FullName.Substring($parserRoot.Length + 1)
            $destination = Join-Path $source $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $destination
        }
}
foreach ($file in @('LICENSE', 'README.md', 'DOCUMENTATION.md', 'THIRD_PARTY.md', 'GmodDemo.sln')) {
    Copy-Item -LiteralPath "$parserRoot\$file" -Destination $source
}
Compress-Archive -Path "$source\*" -DestinationPath "$payload\source\gmoddem-source.zip"

& $Compiler "/DPayloadDir=$payload" "/DReleaseDir=$output" "/DAppVersion=$Version" "/DEdition=$Edition" "$PSScriptRoot\GModReplay.iss"
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
$prefix = if ($public) { 'GModReplay-Public-Setup' } else { 'GModReplay-Setup' }
$installer = Join-Path $output "$prefix-$Version-win-x64.exe"
Get-Item -LiteralPath $installer
Get-FileHash -LiteralPath $installer -Algorithm SHA256
