[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Repository = 'JoeyMarrey29/gmod-replay-public',
    [string]$CommitMessage,
    [switch]$SkipBuild
)
# Moves the current private release (version A) to the public repository (version B):
# builds the public edition, replaces the public source with a snapshot of the pushed private
# commit (without the private release notes), and publishes the public installer.
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$version = (Get-Content -LiteralPath "$root\VERSION" -Raw).Trim()
Import-Module "$PSScriptRoot\UpdateCore.psm1" -Force
$parsed = ConvertTo-ReplayVersion $version
$tag = "v$version"
$notes = "$root\releases-public\$version.md"
if (!(Test-Path -LiteralPath $notes)) { throw "Write the public release notes first: $notes" }

$dirty = & git -C $root status --porcelain
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Commit the exact release source before publishing.' }
$commit = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve release commit.' }
$remote = & git -C $root ls-remote origin refs/heads/main
if ($LASTEXITCODE -ne 0 -or !($remote -match "^$commit\s")) { throw 'Push the release commit to origin/main before publishing.' }

if (!$SkipBuild) {
    & "$PSScriptRoot\Build-Release.ps1" -Edition public
    if (!$?) { throw 'Public build failed.' }
}
$installer = "$root\build\installer\GModReplay-Public-Setup-$version-win-x64.exe"
if (!(Test-Path -LiteralPath $installer)) { throw "Public installer missing: $installer" }
$sum = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
$checksum = "$root\build\installer\SHA256SUMS-Public-$version.txt"
"$sum  $([IO.Path]::GetFileName($installer))" | Set-Content -LiteralPath $checksum -Encoding ASCII

$work = Join-Path ([IO.Path]::GetTempPath()) ('gmod-replay-public-' + [guid]::NewGuid().ToString('N'))
try {
    & gh repo clone $Repository "$work\repo" -- --quiet
    if ($LASTEXITCODE -ne 0) { throw "Could not clone $Repository." }
    $clone = "$work\repo"
    & git -C $clone checkout -q -B main
    if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the public main branch.' }
    Get-ChildItem -LiteralPath $clone -Force | Where-Object Name -ne '.git' | Remove-Item -Recurse -Force

    # Keep both editions' historical notes in version A; B gets only this release's notes.
    $archive = "$work\source.tar"
    & git -C $root archive --format=tar -o $archive $commit -- . ':(exclude)releases' `
        ':(exclude)releases-public' ':(exclude)CHANGELOG.md' ':(exclude)README.md'
    if ($LASTEXITCODE -ne 0) { throw 'Could not export the release source.' }
    & tar -xf $archive -C $clone
    if ($LASTEXITCODE -ne 0) { throw 'Could not unpack the release source.' }
    New-Item -ItemType Directory -Force -Path "$clone\releases-public" | Out-Null
    Copy-Item -LiteralPath $notes -Destination "$clone\releases-public"
    Copy-Item -LiteralPath "$PSScriptRoot\PublicReadme.md" -Destination "$clone\README.md"

    & git -C $clone add -A
    foreach ($line in (& git -C $root ls-tree -r $commit | Where-Object { $_ -match '^160000 commit ([0-9a-f]{40})\t(.+)$' })) {
        $null = $line -match '^160000 commit ([0-9a-f]{40})\t(.+)$'
        & git -C $clone update-index --add --cacheinfo "160000,$($Matches[1]),$($Matches[2])"
        if ($LASTEXITCODE -ne 0) { throw "Could not record submodule $($Matches[2])." }
    }
    & git -C $clone diff --cached --quiet
    if ($LASTEXITCODE -ne 0) {
        $publicCommitMessage = if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
            "GMod Replay Public $version"
        } else {
            $CommitMessage
        }
        & git -C $clone commit -q -m $publicCommitMessage
        if ($LASTEXITCODE -ne 0) { throw 'Could not commit the public source.' }
    }
    $publicCommit = & git -C $clone rev-parse HEAD
    if ($PSCmdlet.ShouldProcess("$Repository/$tag", 'Push public source and publish GitHub release')) {
        & git -C $clone push -q origin main
        if ($LASTEXITCODE -ne 0) { throw 'Could not push the public source.' }
        $flags = @()
        if ($parsed.IsTest) { $flags += '--prerelease' }
        & gh release create $tag $installer $checksum --repo $Repository --target $publicCommit `
            --title "GMod Replay Public $version" --notes-file $notes @flags
        if ($LASTEXITCODE -ne 0) { throw 'Public release publication failed; inspect GitHub before retrying.' }
    } else {
        Write-Output "Prepared public snapshot $publicCommit in $clone (not pushed)."
    }
} finally {
    if (!$WhatIfPreference -and (Test-Path -LiteralPath $work)) { Remove-Item -LiteralPath $work -Recurse -Force }
}
