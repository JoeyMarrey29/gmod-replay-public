Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertTo-ReplayVersion([string]$Version) {
    if ($Version -cnotmatch '^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-test\.(\d{8}))?$') {
        throw "Unsupported release version: $Version"
    }
    [pscustomobject]@{
        Number = [version]("$($Matches[1]).$($Matches[2]).$($Matches[3])")
        TestDate = if ($Matches[5]) { [int]$Matches[5] } else { [int]::MaxValue }
        IsTest = [bool]$Matches[4]
        Text = $Version.TrimStart('v')
    }
}

function Compare-ReplayVersion([string]$Left, [string]$Right) {
    $a = ConvertTo-ReplayVersion $Left
    $b = ConvertTo-ReplayVersion $Right
    $number = $a.Number.CompareTo($b.Number)
    if ($number -ne 0) { return $number }
    return $a.TestDate.CompareTo($b.TestDate)
}

function Find-ReplayUpdate([object[]]$Releases, [string]$CurrentVersion) {
    $current = ConvertTo-ReplayVersion $CurrentVersion
    $best = $null
    foreach ($release in $Releases) {
        if ($release.draft) { continue }
        $version = ConvertTo-ReplayVersion $release.tag_name
        if (!$current.IsTest -and ($release.prerelease -or $version.IsTest)) { continue }
        if ((Compare-ReplayVersion $version.Text $CurrentVersion) -le 0) { continue }
        if ($null -eq $best -or (Compare-ReplayVersion $version.Text $best.tag_name) -gt 0) {
            $best = $release
        }
    }
    return $best
}

function Get-ReplayInstaller([object]$Release, [string]$Repository, [string]$Prefix = 'GModReplay-Setup') {
    if ($Repository -cnotmatch '^[A-Za-z0-9-]+/[A-Za-z0-9_.-]+$') { throw 'Invalid release repository.' }
    if ($Prefix -cnotin @('GModReplay-Setup', 'GModReplay-Public-Setup')) { throw 'Invalid installer name.' }
    $version = ConvertTo-ReplayVersion $Release.tag_name
    $name = "$Prefix-$($version.Text)-win-x64.exe"
    $assets = @($Release.assets | Where-Object name -CEQ $name)
    if ($assets.Count -ne 1) { throw "Release must contain exactly one installer named $name." }
    $asset = $assets[0]
    if ($asset.url -cnotmatch ("^https://api\.github\.com/repos/" + [regex]::Escape($Repository) + "/releases/assets/[0-9]+$")) {
        throw 'Installer asset is not from the configured GitHub repository.'
    }
    if ($asset.digest -cnotmatch '^sha256:([a-f0-9]{64})$') { throw 'GitHub did not supply an installer SHA256 digest.' }
    $hash = $Matches[1]
    if ([long]$asset.size -le 0 -or [long]$asset.size -gt 536870912) { throw 'Installer size is outside the supported range.' }
    [pscustomobject]@{ Name = $name; Url = $asset.url; Hash = $hash; Size = [long]$asset.size }
}

function Test-ReplayInstaller([string]$Path, [object]$Asset) {
    if ((Get-Item -LiteralPath $Path).Length -ne $Asset.Size) { throw 'Installer download size does not match GitHub metadata.' }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Asset.Hash) {
        throw 'Installer SHA256 verification failed. Nothing will be installed.'
    }
}

function Get-ReplayViewerProcess([int]$ProcessId, [string]$ExpectedPath) {
    $process = $null
    try {
        $process = [Diagnostics.Process]::GetProcessById($ProcessId)
        if ($process.HasExited) { return $null }
        $path = $process.MainModule.FileName
    } catch [ArgumentException] {
        return $null
    } catch [InvalidOperationException] {
        if ($null -ne $process -and $process.HasExited) { return $null }
        throw
    }
    if ($path -ine $ExpectedPath) { throw 'The updater was not started by this installation of GMod Replay.' }
    return $process
}

Export-ModuleMember -Function ConvertTo-ReplayVersion, Compare-ReplayVersion, Find-ReplayUpdate, Get-ReplayInstaller, Test-ReplayInstaller, Get-ReplayViewerProcess
