[CmdletBinding(SupportsShouldProcess)]
param([string]$Repository = 'JoeyMarrey29/gmod-replay')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$version = (Get-Content -LiteralPath "$root\VERSION" -Raw).Trim()
Import-Module "$PSScriptRoot\UpdateCore.psm1" -Force
$parsed = ConvertTo-ReplayVersion $version
$tag = "v$version"
$installer = "$root\build\installer\GModReplay-Setup-$version-win-x64.exe"
$notes = "$root\releases\$version.md"
foreach ($file in @($installer,$notes)) {
    if (!(Test-Path -LiteralPath $file)) { throw "Release input missing: $file" }
}
$dirty = & git -C $root status --porcelain
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Commit the exact release source before publishing.' }
$commit = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve release commit.' }
$remote = & git -C $root ls-remote origin refs/heads/main
if ($LASTEXITCODE -ne 0 -or !($remote -match "^$commit\s")) { throw 'Push the release commit to origin/main before publishing.' }
$sum = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
$checksum = "$root\build\installer\SHA256SUMS-$version.txt"
"$sum  $([IO.Path]::GetFileName($installer))" | Set-Content -LiteralPath $checksum -Encoding ASCII
if ($PSCmdlet.ShouldProcess("$Repository/$tag", 'Publish GitHub release and installer')) {
    $flags = @()
    if ($parsed.IsTest) { $flags += '--prerelease' }
    & gh release create $tag $installer $checksum --repo $Repository --target $commit --title "GMod Replay $version" --notes-file $notes @flags
    if ($LASTEXITCODE -ne 0) { throw 'Release publication failed; inspect GitHub before retrying.' }
    & git -C $root fetch origin "refs/tags/${tag}:refs/tags/$tag"
    if ($LASTEXITCODE -ne 0) { throw 'Release published, but the local tag could not be fetched.' }
}
