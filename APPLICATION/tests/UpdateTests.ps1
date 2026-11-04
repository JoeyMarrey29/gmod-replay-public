$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\installer\UpdateCore.psm1') -Force

function Assert([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}
function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $caught = $false
    try { & $Action | Out-Null } catch { $caught = $true }
    Assert $caught $Message
}
function Release([string]$Tag, [bool]$Draft = $false, [bool]$Prerelease = $true) {
    [pscustomobject]@{ tag_name = $Tag; draft = $Draft; prerelease = $Prerelease; assets = @() }
}
Assert ((Compare-ReplayVersion '0.1.10-test.20261021' '0.1.9-test.20261021') -gt 0) 'Versions must be numeric, not lexical.'
Assert ((Compare-ReplayVersion 'v0.1.11' '0.1.11-test.20261021') -gt 0) 'Stable must follow its prerelease.'
Assert ((Compare-ReplayVersion '0.1.11-test.20261022' '0.1.11-test.20261021') -gt 0) 'Test dates must sort.'
Assert ((Compare-ReplayVersion 'v0.1.11-test.20261021' '0.1.11-test.20261021') -eq 0) 'Tags must normalize.'
Assert-Throws { ConvertTo-ReplayVersion '1.2.3;calc.exe' } 'Shell-shaped versions must be rejected.'
Assert-Throws { ConvertTo-ReplayVersion '../1.2.3' } 'Path-shaped versions must be rejected.'
Assert-Throws { ConvertTo-ReplayVersion '01.2.3' } 'Noncanonical versions must be rejected.'
$releases = @((Release 'v0.1.12-test.20261022'), (Release 'v0.1.13-test.20261022' $true), (Release 'v0.1.11' $false $false))
Assert ((Find-ReplayUpdate $releases '0.1.10-test.20261021').tag_name -eq 'v0.1.12-test.20261022') 'Draft must not be selected.'
Assert ((Find-ReplayUpdate $releases '0.1.10').tag_name -eq 'v0.1.11') 'Stable users must not get test releases.'
Assert ($null -eq (Find-ReplayUpdate $releases '0.1.14')) 'Updates must not downgrade.'
Assert ($null -eq (Get-ReplayViewerProcess ([int]::MaxValue) 'unused.exe')) 'Already-exited viewer must not fail the updater.'
$ownPath = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
Assert ((Get-ReplayViewerProcess $PID $ownPath).Id -eq $PID) 'Live parent must resolve.'
Assert-Throws { Get-ReplayViewerProcess $PID 'C:\wrong-install\GModReplay.exe' } 'Reused PID from a different executable must be rejected.'
$release = Release 'v0.1.11-test.20261021'
$asset = [pscustomobject]@{
    name = 'GModReplay-Setup-0.1.11-test.20261021-win-x64.exe'
    url = 'https://api.github.com/repos/JoeyMarrey29/gmod-replay/releases/assets/123'
    digest = 'sha256:' + ('a' * 64)
    size = 10
}
$release.assets = @($asset)
$selected = Get-ReplayInstaller $release 'JoeyMarrey29/gmod-replay'
Assert ($selected.Hash -eq ('a' * 64)) 'Digest must be parsed.'
$asset.url = 'https://api.github.com/repos/other/repo/releases/assets/123'
Assert-Throws { Get-ReplayInstaller $release 'JoeyMarrey29/gmod-replay' } 'Other repository assets must be rejected.'
$asset.url = 'http://api.github.com/repos/JoeyMarrey29/gmod-replay/releases/assets/123'
Assert-Throws { Get-ReplayInstaller $release 'JoeyMarrey29/gmod-replay' } 'Plain HTTP must be rejected.'
$asset.url = 'https://api.github.com/repos/JoeyMarrey29/gmod-replay/releases/assets/123'
$asset.digest = ''
Assert-Throws { Get-ReplayInstaller $release 'JoeyMarrey29/gmod-replay' } 'Missing digest must fail closed.'
$asset.digest = 'sha256:' + ('a' * 64)
$release.assets = @($asset, $asset)
Assert-Throws { Get-ReplayInstaller $release 'JoeyMarrey29/gmod-replay' } 'Duplicate installers must be rejected.'
$file = [IO.Path]::GetTempFileName()
try {
    [IO.File]::WriteAllBytes($file, [byte[]](1,2,3))
    $expected = [pscustomobject]@{ Size = 3; Hash = (Get-FileHash $file -Algorithm SHA256).Hash }
    Test-ReplayInstaller $file $expected
    $expected.Hash = 'a' * 64
    Assert-Throws { Test-ReplayInstaller $file $expected } 'Hash mismatch must block installation.'
    $expected.Size = 4
    Assert-Throws { Test-ReplayInstaller $file $expected } 'Truncation must block installation.'
} finally {
    Remove-Item -LiteralPath $file
}
foreach ($script in @('Update-GModReplay.ps1','UpdateCore.psm1','Build-Installer.ps1',
    'Build-Release.ps1','Publish-Release.ps1','Publish-Public.ps1')) {
    $tokens = $null
    $errors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot "..\installer\$script"), [ref]$tokens, [ref]$errors)
    Assert ($errors.Count -eq 0) "$script has PowerShell syntax errors: $errors"
}
$workerText = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\installer\Update-GModReplay.ps1') -Raw
Assert ($workerText -notmatch 'MessageBox|System.Windows.Forms|Show-UpdateMessage') 'Update operations must not display Windows dialogs.'
$helpersWork = & {
    . (Join-Path $PSScriptRoot '..\installer\Update-GModReplay.ps1') -LoadOnly
    $tokenFile = [IO.Path]::GetTempFileName()
    try {
        (@{ accessToken = 'test-token'; refreshToken = 'test-refresh'
            expiresAtUtc = [DateTime]::UtcNow.AddHours(8).ToString('o') } | ConvertTo-Json -Compress) |
            ConvertTo-SecureString -AsPlainText -Force |
            ConvertFrom-SecureString | Set-Content -LiteralPath $tokenFile
        Assert ((Read-ReplayToken) -eq 'test-token') 'Saved DPAPI credentials must survive the trailing file newline.'
        'legacy-token' | ConvertTo-SecureString -AsPlainText -Force |
            ConvertFrom-SecureString | Set-Content -LiteralPath $tokenFile
        $legacyMessage = ''
        try { $null = Read-ReplayToken } catch { $legacyMessage = $_.Exception.Message }
        Assert ($legacyMessage -match 'older broad OAuth sign-in') 'Old broad OAuth tokens must require explicit unlink and revocation.'
    } finally {
        Remove-Item -LiteralPath $tokenFile
    }
    $StatusFile = [IO.Path]::GetTempFileName()
    try {
        $login = 'test-user'
        $enabled = $true
        Write-UiStatus 'linking' "Sign in`nwith code" @{ code = 'ABCD-1234' }
        $statusText = [IO.File]::ReadAllText($StatusFile)
        Assert ($statusText -match 'login=test-user' -and $statusText -match 'enabled=True') 'Account/preference status must reach the app.'
        Assert ($statusText -match 'message=Sign%20in%0Awith%20code' -and $statusText -match 'code=ABCD-1234') 'Status must encode multiline text and sign-in code.'
        Assert (!(Test-Path -LiteralPath ($StatusFile + '.tmp'))) 'Status file replacement must be atomic.'
    } finally {
        Remove-Item -LiteralPath $StatusFile
    }
    & {
        $fixture = Join-Path ([IO.Path]::GetTempPath()) ('replay-update-ui-test-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $fixture | Out-Null
        $install = $fixture
        $state = $fixture
        $tokenFile = Join-Path $fixture 'token.dpapi'
        $accessFile = Join-Path $fixture 'access.dpapi'
        $denyRepository = $false
        $preferences = Join-Path $fixture 'preferences.json'
        $accountFile = Join-Path $fixture 'account.json'
        $installResult = Join-Path $fixture 'last-install-error.json'
        $StatusFile = Join-Path $fixture 'ui.status'
        $updateMutexName = 'Local\GModReplay.Test.' + [guid]::NewGuid().ToString('N')
        $ViewerPid = 0
        $received = [Collections.Generic.List[string]]::new()
        $formRequests = [Collections.Generic.List[object]]::new()
        $apiTokens = [Collections.Generic.List[string]]::new()
        @{ version = '0.1.10-test.20261021'; repository = 'JoeyMarrey29/gmod-replay'
            repositoryId = '1428373505'; appClientId = '12345678901234567890' } |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture 'release.json')
        $binary = Join-Path $fixture 'expected.bin'
        [IO.File]::WriteAllBytes($binary, [byte[]](1,2,3))
        $digest = (Get-FileHash $binary -Algorithm SHA256).Hash.ToLowerInvariant()
        function Invoke-GitHubJson([string]$Url, [string]$Token = '', [hashtable]$Form = $null) {
            if ($Url -like '*/login/device/code') {
                $formRequests.Add($Form)
                return [pscustomobject]@{ verification_uri = 'https://github.com/login/device'; user_code = 'ABCD-1234'
                    device_code = 'fixture'; expires_in = 30; interval = 0 }
            }
            if ($Url -like '*/login/oauth/access_token') {
                $formRequests.Add($Form)
                if ($Form.grant_type -eq 'refresh_token') {
                    return [pscustomobject]@{ access_token = 'fixture-refreshed-token'; refresh_token = 'fixture-next-refresh'
                        expires_in = 28800; refresh_token_expires_in = 15897600 }
                }
                return [pscustomobject]@{ access_token = 'fixture-token'; refresh_token = 'fixture-refresh'
                    expires_in = 28800; refresh_token_expires_in = 15897600 }
            }
            $apiTokens.Add($Token)
            Assert ($Token -in @('fixture-token', 'fixture-refreshed-token')) 'Private update operations must use the current linked token.'
            if ($Url -eq 'https://api.github.com/user') { return [pscustomobject]@{ login = 'fixture-user' } }
            if ($Url -eq 'https://api.github.com/repos/JoeyMarrey29/gmod-replay') {
                if ($denyRepository) { throw 'GitHub returned HTTP 404. Check sign-in, repository invitation and network access.' }
                return [pscustomobject]@{ id = 1428373505 }
            }
            return [pscustomobject]@{ tag_name = 'v0.1.11-test.20261021'; draft = $false; prerelease = $true
                assets = @([pscustomobject]@{ name = 'GModReplay-Setup-0.1.11-test.20261021-win-x64.exe'
                    url = 'https://api.github.com/repos/JoeyMarrey29/gmod-replay/releases/assets/123'
                    size = 3; digest = "sha256:$digest" }) }
        }
        function Receive-ReplayInstaller([object]$Asset, [string]$Token, [string]$Destination) {
            $received.Add($Destination)
            [IO.File]::WriteAllBytes($Destination, [byte[]](1,2,3))
        }
        try {
            Invoke-ReplayUpdates 'automatic'
            Assert ((Get-Content $StatusFile -Raw) -match 'status=unlinked') 'Launch checks without a linked account must request sign-in.'
            Invoke-ReplayUpdates 'check'
            Assert ((Get-Content $StatusFile -Raw) -match 'status=unlinked') 'Unlinked checks must request account linking in-app.'
            Invoke-ReplayUpdates 'link'
            Assert ((Get-Content $StatusFile -Raw) -match 'login=fixture-user') 'Account identity must appear after device authorization.'
            Assert ((Read-ReplayToken) -eq 'fixture-token') 'Device authorization must persist the linked token.'
            Assert (Test-Path $accessFile) 'A confirmed collaborator must get a viewing access record.'
            Add-Type -AssemblyName System.Security
            $record = [Text.Encoding]::UTF8.GetString([Security.Cryptography.ProtectedData]::Unprotect(
                [IO.File]::ReadAllBytes($accessFile), [Text.Encoding]::UTF8.GetBytes('GModReplay.Access.v1'),
                [Security.Cryptography.DataProtectionScope]::CurrentUser)) | ConvertFrom-Json
            Assert ($record.repositoryId -eq '1428373505' -and $record.login -eq 'fixture-user' -and
                [Math]::Abs([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() - [long]$record.verifiedAt) -lt 60) 'The access record must name the private repository and when it was verified.'
            $denyRepository = $true
            Invoke-ReplayUpdates 'status'
            Assert (!(Test-Path $accessFile)) 'Losing collaborator access must remove the viewing access record.'
            Assert ((Get-Content $StatusFile -Raw) -match 'not%20a%20collaborator') 'Non-collaborators must be told to use the public version.'
            $denyRepository = $false
            Invoke-ReplayUpdates 'status'
            Assert (Test-Path $accessFile) 'Regaining access must restore the viewing access record.'
            Assert ($formRequests[0].scope -eq 'offline_access') 'GitHub App device flow must request expiring tokens without broad OAuth scopes.'
            Assert ($formRequests[1].repository_id -eq '1428373505') 'The device token exchange must restrict access to the GMod Replay repository.'
            Invoke-ReplayUpdates 'enable'
            Assert ((Get-Content $preferences -Raw | ConvertFrom-Json).enabled) 'Automatic updates must enable.'
            Invoke-ReplayUpdates 'disable'
            Assert (!(Get-Content $preferences -Raw | ConvertFrom-Json).enabled) 'Automatic updates must disable.'
            Assert ((Read-ReplayToken) -eq 'fixture-token') 'Turning updates off must not unlink the account.'
            Invoke-ReplayUpdates 'status'
            Assert ((Get-Content $StatusFile -Raw) -match 'login=fixture-user') 'Status must retain the linked identity.'
            $expired = @{ accessToken = 'fixture-token'; refreshToken = 'fixture-refresh'
                expiresAtUtc = [DateTime]::UtcNow.AddMinutes(-1).ToString('o') }
            (ConvertTo-Json -InputObject $expired -Compress) |
                ConvertTo-SecureString -AsPlainText -Force | ConvertFrom-SecureString |
                Set-Content -LiteralPath $tokenFile
            Invoke-ReplayUpdates 'automatic'
            Assert ((Get-Content $StatusFile -Raw) -match 'status=ready') 'Launch checks must offer a verified downloaded update even when the legacy automatic-check preference is off.'
            Assert ($formRequests[2].grant_type -eq 'refresh_token' -and !$formRequests[2].ContainsKey('repository_id')) `
                'Token refresh must rotate credentials while retaining GitHub’s original repository restriction.'
            Assert ((Read-ReplayToken) -eq 'fixture-refreshed-token') 'An expired access token must be refreshed before update requests.'
            Assert ($apiTokens.Contains('fixture-refreshed-token')) 'Update requests must use the refreshed access token.'
            Assert ($received.Count -eq 1) 'Manual checking must download the installer once.'
            Invoke-ReplayUpdates 'check'
            Assert ($received.Count -eq 1) 'Verified cached installers must be reused.'
            Assert-Throws { Invoke-ReplayUpdates 'install' } 'Installation without viewer approval must be rejected.'
            Assert ((Get-Content $StatusFile -Raw) -match 'status=error') 'Installer rejection must be shown in-app.'
            & {
                $ViewerPid = 123
                $starts = [Collections.Generic.List[string]]::new()
                function Get-ReplayViewerProcess([int]$ProcessId, [string]$ExpectedPath) {
                    $parent = [pscustomobject]@{}
                    $parent | Add-Member ScriptMethod WaitForExit { param($timeout) return $true }
                    return $parent
                }
                function Start-Process {
                    param([string]$FilePath, [object[]]$ArgumentList, [string]$WorkingDirectory,
                        [switch]$PassThru, [switch]$Wait)
                    $starts.Add($FilePath)
                    if ($FilePath -like '*Setup*') {
                        $config = Get-Content -LiteralPath (Join-Path $install 'release.json') -Raw | ConvertFrom-Json
                        $config.version = '0.1.11-test.20261021'
                        $config | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $install 'release.json')
                        return [pscustomobject]@{ ExitCode = 0 }
                    }
                }
                Invoke-ReplayUpdates 'install'
                Assert ($starts.Count -eq 2 -and $starts[0] -like '*Setup*' -and
                    $starts[1] -like '*GModReplay.exe') 'A successful install must relaunch the viewer after the installer exits.'
                Assert ((Get-Content $StatusFile -Raw) -match 'status=installed') 'Successful install must report completion.'
                $config = @{ version = '0.1.10-test.20261021'; repository = 'JoeyMarrey29/gmod-replay'
                    repositoryId = '1428373505'; appClientId = '12345678901234567890' }
                $config | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $install 'release.json')
            }
            & {
                $ViewerPid = 123
                function Get-ReplayViewerProcess([int]$ProcessId, [string]$ExpectedPath) {
                    $parent = [pscustomobject]@{}
                    $parent | Add-Member ScriptMethod WaitForExit {
                        param($timeout)
                        Set-Content -LiteralPath ($StatusFile + '.cancel') -Value 'cancel'
                        return $false
                    }
                    return $parent
                }
                function Start-Process { throw 'Cancelled installation must never run an installer.' }
                Invoke-ReplayUpdates 'install'
                Assert ((Get-Content $StatusFile -Raw) -match 'status=ready') 'Cancelling while waiting for exit must retain the downloaded update.'
                Remove-Item -LiteralPath ($StatusFile + '.cancel')
            }
            & {
                $ViewerPid = 123
                $starts = [Collections.Generic.List[string]]::new()
                function Get-ReplayViewerProcess([int]$ProcessId, [string]$ExpectedPath) {
                    $parent = [pscustomobject]@{}
                    $parent | Add-Member ScriptMethod WaitForExit { param($timeout) return $true }
                    return $parent
                }
                function Start-Process {
                    param([string]$FilePath, [object[]]$ArgumentList, [string]$WorkingDirectory,
                        [switch]$PassThru, [switch]$Wait)
                    $starts.Add($FilePath)
                    if ($FilePath -like '*Setup*') { return [pscustomobject]@{ ExitCode = 2 } }
                }
                Assert-Throws { Invoke-ReplayUpdates 'install' } 'Failed installation must report failure.'
                Assert ($starts.Count -eq 2 -and $starts[1] -like '*GModReplay.exe') 'An install failure after exit must reopen the viewer to show its error.'
                Assert (Test-Path $installResult) 'An install failure must persist its message for the new viewer.'
                Invoke-ReplayUpdates 'automatic'
                Assert ((Get-Content $StatusFile -Raw) -match 'status=error') 'Install errors must be surfaced inside the next viewer.'
                Assert (!(Test-Path $installResult)) 'Displayed previous-install errors must be acknowledged once.'
            }
            Invoke-ReplayUpdates 'unlink'
            Assert (!(Test-Path $tokenFile) -and !(Test-Path $accountFile) -and !(Test-Path $accessFile)) 'Unlink must remove saved credential, identity and viewing access.'
            Assert ((Get-Content $StatusFile -Raw) -match 'linked=False') 'Unlinked status must be explicit.'
            Set-Content -LiteralPath $tokenFile -Value 'damaged token'
            Assert-Throws { Invoke-ReplayUpdates 'status' } 'Unreadable credentials must report an error.'
            Assert ((Get-Content $StatusFile -Raw) -match 'linked=True') 'Unreadable saved accounts must still expose Unlink.'
            Invoke-ReplayUpdates 'unlink'
            Set-Content -LiteralPath ($StatusFile + '.cancel') -Value 'cancel'
            Invoke-ReplayUpdates 'link'
            Assert (!(Test-Path $tokenFile)) 'Cancelled device sign-in must not save a token.'
            Assert ((Get-Content $StatusFile -Raw) -match 'status=idle') 'Cancelled sign-in must return to in-app idle state.'
        } finally {
            Remove-Item -LiteralPath $fixture -Recurse -Force
        }
    }
    & {
        $fixture = Join-Path ([IO.Path]::GetTempPath()) ('replay-update-public-test-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $fixture | Out-Null
        $install = $fixture
        $state = $fixture
        $edition = 'public'
        $tokenFile = Join-Path $fixture 'token.dpapi'
        $accessFile = Join-Path $fixture 'access.dpapi'
        $preferences = Join-Path $fixture 'preferences.json'
        $accountFile = Join-Path $fixture 'account.json'
        $installResult = Join-Path $fixture 'last-install-error.json'
        $StatusFile = Join-Path $fixture 'ui.status'
        $updateMutexName = 'Local\GModReplayPublic.Test.' + [guid]::NewGuid().ToString('N')
        $ViewerPid = 0
        $received = [Collections.Generic.List[string]]::new()
        @{ version = '0.1.10-test.20261021'; edition = 'public'; repository = 'JoeyMarrey29/gmod-replay-public'
            repositoryId = '1446003494' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture 'release.json')
        $binary = Join-Path $fixture 'expected.bin'
        [IO.File]::WriteAllBytes($binary, [byte[]](1,2,3))
        $digest = (Get-FileHash $binary -Algorithm SHA256).Hash.ToLowerInvariant()
        function Invoke-GitHubJson([string]$Url, [string]$Token = '', [hashtable]$Form = $null) {
            Assert (!$Token -and $null -eq $Form) 'The public edition must never sign in or send credentials.'
            Assert ($Url -eq 'https://api.github.com/repos/JoeyMarrey29/gmod-replay-public/releases?per_page=100') 'The public edition must only read public releases.'
            return [pscustomobject]@{ tag_name = 'v0.1.11-test.20261021'; draft = $false; prerelease = $true
                assets = @([pscustomobject]@{ name = 'GModReplay-Public-Setup-0.1.11-test.20261021-win-x64.exe'
                    url = 'https://api.github.com/repos/JoeyMarrey29/gmod-replay-public/releases/assets/123'
                    size = 3; digest = "sha256:$digest" }) }
        }
        function Receive-ReplayInstaller([object]$Asset, [string]$Token, [string]$Destination) {
            Assert (!$Token) 'Public downloads must be anonymous.'
            $received.Add($Destination)
            [IO.File]::WriteAllBytes($Destination, [byte[]](1,2,3))
        }
        try {
            Invoke-ReplayUpdates 'link'
            Assert ((Get-Content $StatusFile -Raw) -match 'needs%20no%20GitHub%20account') 'The public edition must refuse to link an account.'
            Assert (!(Test-Path $tokenFile)) 'The public edition must never save a token.'
            Invoke-ReplayUpdates 'status'
            Assert ((Get-Content $StatusFile -Raw) -match 'No%20sign-in') 'Public status must not ask for sign-in.'
            Invoke-ReplayUpdates 'automatic'
            Assert ((Get-Content $StatusFile -Raw) -match 'status=ready') 'Public launch checks must download updates without signing in.'
            Assert ($received.Count -eq 1 -and $received[0] -like '*GModReplay-Public-Setup-*') 'The public installer must be downloaded.'
            Assert (!(Test-Path $accessFile)) 'The public edition never writes private viewing access.'
            @{ version = '0.1.10-test.20261021'; edition = 'public'; repository = 'JoeyMarrey29/gmod-replay'
                repositoryId = '1428373505' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture 'release.json')
            Assert-Throws { Invoke-ReplayUpdates 'check' } 'The public edition must not update from the private repository.'
        } finally {
            Remove-Item -LiteralPath $fixture -Recurse -Force
        }
    }
    $http = [pscustomobject]@{}
    $http | Add-Member ScriptMethod SendAsync {
        param($request)
        Assert ($request.Headers.Authorization.Parameter -eq 'test-token') 'API request must authenticate.'
        $response = [System.Net.Http.HttpResponseMessage]::new([System.Net.HttpStatusCode]::OK)
        $response.Content = [System.Net.Http.StringContent]::new('{"ok":true}')
        $task = [Threading.Tasks.TaskCompletionSource[System.Net.Http.HttpResponseMessage]]::new()
        $task.SetResult($response)
        return $task.Task
    }
    return (Invoke-GitHubJson 'https://api.github.com/repos/owner/repo/releases' 'test-token').ok
}
Assert $helpersWork 'LoadOnly must load working HTTP helpers without executing the updater.'
Write-Output 'PASS: updater versions, editions, collaborator access, channels, lifecycle, saved credentials, in-app account/toggle/check/unlink/cancel operations, verified cache, HTTP helpers, digest, tampering and script syntax.'
