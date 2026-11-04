[CmdletBinding()]
param(
    [switch]$Automatic, [int]$ViewerPid = 0, [switch]$SelfTest, [switch]$LoadOnly,
    [ValidateSet('', 'status', 'automatic', 'link', 'unlink', 'enable', 'disable', 'check', 'install')]
    [string]$Operation = '',
    [string]$StatusFile = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'UpdateCore.psm1') -Force
Add-Type -AssemblyName System.Net.Http

$install = Split-Path $PSScriptRoot -Parent
# The public edition keeps its own state and never signs in; release.json says which edition this is.
$edition = 'private'
try {
    $probe = Get-Content -LiteralPath (Join-Path $install 'release.json') -Raw | ConvertFrom-Json
    if ($probe.PSObject.Properties.Name -contains 'edition' -and [string]$probe.edition -ceq 'public') { $edition = 'public' }
} catch { }
$stateFolder = if ($edition -eq 'public') { 'GModReplayPublic' } else { 'GModReplay' }
$state = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) "$stateFolder\updates"
$tokenFile = Join-Path $state 'token.dpapi'
$accessFile = Join-Path $state 'access.dpapi'
$preferences = Join-Path $state 'preferences.json'
$http = $null
$mutex = $null
$ownsMutex = $false
$accountFile = Join-Path $state 'account.json'
$installResult = Join-Path $state 'last-install-error.json'
$enabled = $false
$login = ''
$updateMutexName = if ($edition -eq 'public') { 'Local\GModReplayPublic.Updater' } else { 'Local\GModReplay.Updater' }
$privateRepository = 'JoeyMarrey29/gmod-replay'
$privateRepositoryId = '1428373505'
$publicRepository = 'JoeyMarrey29/gmod-replay-public'
$publicRepositoryId = '1446003494'
$accessEntropy = 'GModReplay.Access.v1'

function Write-UiStatus([string]$Status, [string]$Message, [hashtable]$Extra = @{}) {
    if (!$StatusFile) { return }
    $fields = @{ status = $Status; message = $Message; login = $login; enabled = [string]$enabled
        linked = [string](Test-Path -LiteralPath $tokenFile) }
    foreach ($key in $Extra.Keys) { $fields[$key] = $Extra[$key] }
    $lines = @($fields.GetEnumerator() | ForEach-Object {
        $_.Key + '=' + [uri]::EscapeDataString([string]$_.Value)
    })
    $temporary = $StatusFile + '.tmp'
    [IO.File]::WriteAllLines($temporary, [string[]]$lines, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $StatusFile -Force
}

function Test-UiCancelled {
    return $StatusFile -and (Test-Path -LiteralPath ($StatusFile + '.cancel'))
}

function Write-UpdateLog([string]$Text) {
    Add-Content -LiteralPath (Join-Path $state 'updates.log') -Value ("{0:o} {1}" -f [DateTime]::UtcNow, $Text)
}

function Invoke-GitHubJson([string]$Url, [string]$Token = '', [hashtable]$Form = $null) {
    $uri = [uri]$Url
    if ($uri.Scheme -ne 'https' -or $uri.Host -notin @('github.com', 'api.github.com')) {
        throw 'Refusing an unexpected GitHub request host.'
    }
    $method = if ($null -ne $Form) { [System.Net.Http.HttpMethod]::Post } else { [System.Net.Http.HttpMethod]::Get }
    $request = [System.Net.Http.HttpRequestMessage]::new($method, $uri)
    $response = $null
    try {
        $request.Headers.Accept.ParseAdd('application/json')
        if ($Token) {
            if ($uri.Host -ne 'api.github.com') { throw 'Credentials may only be sent to the GitHub API.' }
            $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $Token)
        }
        if ($null -ne $Form) {
            $dictionary = [System.Collections.Generic.Dictionary[string,string]]::new()
            foreach ($key in $Form.Keys) { $dictionary.Add($key, [string]$Form[$key]) }
            $request.Content = [System.Net.Http.FormUrlEncodedContent]::new($dictionary)
        }
        $response = $http.SendAsync($request).GetAwaiter().GetResult()
        if (!$response.IsSuccessStatusCode) {
            throw "GitHub returned HTTP $([int]$response.StatusCode). Check sign-in, repository invitation and network access."
        }
        return ($response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json)
    } finally {
        if ($null -ne $response) { $response.Dispose() }
        $request.Dispose()
    }
}

function Save-ReplayCredential([object]$TokenResponse) {
    foreach ($name in @('access_token', 'refresh_token', 'expires_in', 'refresh_token_expires_in')) {
        if (!$TokenResponse.PSObject.Properties.Name.Contains($name) -or !$TokenResponse.$name) {
            throw 'GitHub did not return an expiring access token and refresh token. No account was linked.'
        }
    }
    $credential = @{
        accessToken = [string]$TokenResponse.access_token
        refreshToken = [string]$TokenResponse.refresh_token
        expiresAtUtc = [DateTime]::UtcNow.AddSeconds([int]$TokenResponse.expires_in).ToString('o')
    }
    $encrypted = (ConvertTo-Json -InputObject $credential -Compress) |
        ConvertTo-SecureString -AsPlainText -Force | ConvertFrom-SecureString
    $temporary = $tokenFile + '.tmp'
    try {
        [IO.File]::WriteAllText($temporary, $encrypted, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $tokenFile -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
    return [pscustomobject]$credential
}

function Read-ReplayCredential {
    if (!(Test-Path -LiteralPath $tokenFile)) { return $null }
    $secure = (Get-Content -LiteralPath $tokenFile -Raw).Trim() | ConvertTo-SecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
    try { $credential = $plain | ConvertFrom-Json }
    catch {
        throw 'This computer has an older broad OAuth sign-in. Unlink it, revoke the old GMod Replay Updates authorization in GitHub settings, then link again.'
    }
    if (!$credential.accessToken -or !$credential.refreshToken -or !$credential.expiresAtUtc) {
        throw 'The saved sign-in is not repository-limited. Unlink it, revoke the old GMod Replay Updates authorization in GitHub settings, then link again.'
    }
    return $credential
}

function Connect-ReplayGitHub([string]$ClientId, [string]$RepositoryId) {
    $device = Invoke-GitHubJson 'https://github.com/login/device/code' -Form @{
        client_id = $ClientId; scope = 'offline_access'
    }
    if ($device.verification_uri -ne 'https://github.com/login/device') { throw 'Unexpected device sign-in URL.' }
    $expires = [DateTime]::UtcNow.AddSeconds([int]$device.expires_in)
    Write-UiStatus 'linking' 'Open GitHub, enter this code, and authorize GMod Replay Updates. This panel updates automatically.' @{
        code = $device.user_code
    }
    $interval = [int]$device.interval
    while ([DateTime]::UtcNow -lt $expires) {
        for ($second = 0; $second -lt $interval; ++$second) {
            if (Test-UiCancelled) { return '' }
            Start-Sleep -Seconds 1
        }
        $reply = Invoke-GitHubJson 'https://github.com/login/oauth/access_token' -Form @{
            client_id = $ClientId; device_code = $device.device_code
            grant_type = 'urn:ietf:params:oauth:grant-type:device_code'
            repository_id = $RepositoryId
        }
        if ($reply.PSObject.Properties.Name -contains 'access_token') {
            if (Test-UiCancelled) { return '' }
            return Save-ReplayCredential $reply
        }
        switch ($reply.error) {
            'authorization_pending' { continue }
            'slow_down' { $interval += 5; continue }
            default { throw "GitHub sign-in failed: $($reply.error)" }
        }
    }
    throw 'GitHub sign-in expired. Select Link account to get a new code.'
}

function Read-ReplayToken {
    $credential = Read-ReplayCredential
    if ($null -eq $credential) { return '' }
    return [string]$credential.accessToken
}

function Refresh-ReplayCredential([object]$Credential, [string]$ClientId) {
    $reply = Invoke-GitHubJson 'https://github.com/login/oauth/access_token' -Form @{
        client_id = $ClientId
        grant_type = 'refresh_token'
        refresh_token = [string]$Credential.refreshToken
    }
    if ($reply.PSObject.Properties.Name -contains 'error') {
        throw "GitHub sign-in expired or could not be refreshed ($($reply.error)). Unlink and link your account again."
    }
    return Save-ReplayCredential $reply
}

function Save-UpdatePreference([bool]$Enabled) {
    @{ enabled = $Enabled } | ConvertTo-Json | Set-Content -LiteralPath $preferences
}

function Remove-ReplayAccess {
    if (Test-Path -LiteralPath $accessFile) { Remove-Item -LiteralPath $accessFile -Force }
}

# The private viewer only opens demos while this record says GitHub recently confirmed that the
# linked account can see the private repository. It is DPAPI-sealed to this Windows user.
function Save-ReplayAccess([string]$Login) {
    Add-Type -AssemblyName System.Security
    $record = @{ login = $Login; repositoryId = $privateRepositoryId
        verifiedAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() } | ConvertTo-Json -Compress
    $sealed = [Security.Cryptography.ProtectedData]::Protect([Text.Encoding]::UTF8.GetBytes($record),
        [Text.Encoding]::UTF8.GetBytes($accessEntropy), [Security.Cryptography.DataProtectionScope]::CurrentUser)
    $temporary = $accessFile + '.tmp'
    try {
        [IO.File]::WriteAllBytes($temporary, $sealed)
        Move-Item -LiteralPath $temporary -Destination $accessFile -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}

# Asks GitHub whether the linked account is a collaborator. A refusal removes access at once;
# a network failure keeps the last record so the viewer's offline grace period applies.
function Update-ReplayAccess([string]$Token) {
    if (!$Token) {
        Remove-ReplayAccess
        return $false
    }
    try {
        $repository = Invoke-GitHubJson "https://api.github.com/repos/$privateRepository" $Token
    } catch {
        if ($_.Exception.Message -match 'HTTP (401|403|404)\b') {
            Remove-ReplayAccess
            Write-UpdateLog 'GitHub refused access to the private repository; viewing access removed.'
            return $false
        }
        Write-UpdateLog "Could not confirm repository access: $($_.Exception.Message)"
        return (Test-Path -LiteralPath $accessFile)
    }
    if ([string]$repository.id -cne $privateRepositoryId) {
        Remove-ReplayAccess
        return $false
    }
    Save-ReplayAccess $login
    return $true
}

function Receive-ReplayInstaller([object]$Asset, [string]$Token, [string]$Destination) {
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get, $Asset.Url)
    $request.Headers.Accept.ParseAdd('application/octet-stream')
    if ($Token) { $request.Headers.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $Token) }
    $response = $null
    $stream = $null
    $deadline = [Threading.CancellationTokenSource]::new([TimeSpan]::FromMinutes(5))
    try {
        $response = $http.SendAsync($request, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead,
            $deadline.Token).GetAwaiter().GetResult()
        if ([int]$response.StatusCode -in @(301,302,303,307,308)) {
            $location = $response.Headers.Location
            if (!$location.IsAbsoluteUri -or $location.Scheme -ne 'https' -or
                $location.Host -ne 'release-assets.githubusercontent.com') {
                throw 'Refusing an unexpected release download redirect.'
            }
            $response.Dispose()
            $response = $null
            $request.Dispose()
            # Never forward the GitHub token to the release storage host.
            $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get, $location)
            $response = $http.SendAsync($request, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead,
                $deadline.Token).GetAwaiter().GetResult()
        }
        if (!$response.IsSuccessStatusCode) { throw "Installer download failed: HTTP $([int]$response.StatusCode)." }
        $inputStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $stream = [IO.File]::Create($Destination)
        $buffer = New-Object byte[] 65536
        [long]$total = 0
        while (($count = $inputStream.ReadAsync($buffer,0,$buffer.Length,$deadline.Token).GetAwaiter().GetResult()) -gt 0) {
            $total += $count
            if ($total -gt $Asset.Size) { throw 'Download exceeds the expected installer size.' }
            $stream.Write($buffer,0,$count)
        }
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        if ($null -ne $response) { $response.Dispose() }
        $request.Dispose()
        $deadline.Dispose()
    }
}

function Invoke-ReplayUpdates([string]$RequestedOperation = $Operation) {
    $Operation = $RequestedOperation
    $installAttempted = $false
    $mutex = $null
    $http = $null
    $ownsMutex = $false
    $enabled = $false
    $login = ''
    try {
        New-Item -ItemType Directory -Force -Path $state | Out-Null
        $config = Get-Content -LiteralPath (Join-Path $install 'release.json') -Raw | ConvertFrom-Json
        $null = ConvertTo-ReplayVersion $config.version
        $public = $config.PSObject.Properties.Name -contains 'edition' -and [string]$config.edition -ceq 'public'
        if ($public) {
            if ($config.repository -cne $publicRepository -or
                [string]$config.repositoryId -cne $publicRepositoryId) { throw 'Invalid installed release configuration.' }
        } elseif ($config.repository -cnotmatch '^[A-Za-z0-9-]+/[A-Za-z0-9_.-]+$' -or
            $config.appClientId -cnotmatch '^[A-Za-z0-9]{20}$' -or
            [string]$config.repositoryId -cnotmatch '^[1-9][0-9]*$' -or
            $config.repository -cne $privateRepository -or
            [string]$config.repositoryId -cne $privateRepositoryId) { throw 'Invalid installed release configuration.' }
        if ($SelfTest) {
            Write-Output "PASS: installed updater loads, validates configuration and imports version policy."
            return
        }
        $viewer = $null
        if ($ViewerPid -gt 0) {
            $viewer = Get-ReplayViewerProcess $ViewerPid (Join-Path $install 'GModReplay.exe')
            if ($null -eq $viewer -and ($Automatic -or $Operation -eq 'automatic')) {
                Write-UpdateLog 'Viewer exited before automatic checking started; deferred until next menu startup.'
                return
            }
        }
        $mutex = [Threading.Mutex]::new($false, $updateMutexName)
        try { $ownsMutex = $mutex.WaitOne(0) }
        catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
        if (!$ownsMutex) {
            Write-UiStatus 'busy' 'Another update task is already running. Wait for it to finish, then try again.'
            return
        }
        if (!$Operation) { $Operation = if ($Automatic) { 'automatic' } else { 'status' } }
        if (Test-Path -LiteralPath $preferences) {
            $enabled = [bool](Get-Content -LiteralPath $preferences -Raw | ConvertFrom-Json).enabled
        }
        if (Test-Path -LiteralPath $accountFile) {
            $login = [string](Get-Content -LiteralPath $accountFile -Raw | ConvertFrom-Json).login
        }
        if (!(Test-Path -LiteralPath $tokenFile)) { $login = '' }
        if ($Operation -in @('automatic', 'status') -and (Test-Path -LiteralPath $installResult)) {
            $previous = Get-Content -LiteralPath $installResult -Raw | ConvertFrom-Json
            Write-UiStatus 'error' ([string]$previous.message)
            Remove-Item -LiteralPath $installResult
            return
        }
        if ($Operation -in @('link', 'unlink') -and $public) {
            Write-UiStatus 'idle' 'The public edition needs no GitHub account. Updates come from public releases.'
            return
        }
        if ($Operation -eq 'unlink') {
            foreach ($file in @($tokenFile, $accountFile, $accessFile)) {
                if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
            }
            $login = ''
            Write-UpdateLog 'GitHub account unlinked; local saved credentials removed.'
            Write-UiStatus 'idle' 'Account unlinked on this computer. You can also revoke access in GitHub settings.'
            return
        }
        if ($Operation -in @('enable', 'disable')) {
            $enabled = $Operation -eq 'enable'
            Save-UpdatePreference $enabled
            Write-UpdateLog "Legacy automatic-check preference set: $enabled."
            Write-UiStatus 'idle' 'Update checks run on launch when a GitHub account is linked.'
            return
        }
        $handler = [System.Net.Http.HttpClientHandler]::new()
        $handler.AllowAutoRedirect = $false
        $http = [System.Net.Http.HttpClient]::new($handler)
        $http.Timeout = [TimeSpan]::FromMinutes(5)
        $http.DefaultRequestHeaders.UserAgent.ParseAdd("GModReplay-Updater/$($config.version)")
        Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
        if ($Operation -eq 'link') {
            $credential = Connect-ReplayGitHub $config.appClientId $config.repositoryId
            if (!$credential) {
                Write-UiStatus 'idle' 'GitHub sign-in cancelled. No new account was linked.'
                return
            }
            $account = Invoke-GitHubJson 'https://api.github.com/user' $credential.accessToken
            $login = [string]$account.login
            @{ login = $login } | ConvertTo-Json | Set-Content -LiteralPath $accountFile -Encoding UTF8
            Write-UpdateLog "GitHub account linked: $login."
            if (Update-ReplayAccess $credential.accessToken) {
                Write-UiStatus 'idle' 'GitHub account linked. You can now open demos and check private releases.'
            } else {
                Write-UiStatus 'idle' 'GitHub account linked, but it is not a collaborator on GMod Replay. Use the public version, or ask Joey for access.'
            }
            return
        }
        $credential = if ($public) { $null } else { Read-ReplayCredential }
        if ($credential -and [DateTime]::Parse([string]$credential.expiresAtUtc).ToUniversalTime() -le
            [DateTime]::UtcNow.AddMinutes(2)) {
            $credential = Refresh-ReplayCredential $credential $config.appClientId
        }
        $token = if ($credential) { [string]$credential.accessToken } else { '' }
        if ($token -and !$login) {
            $account = Invoke-GitHubJson 'https://api.github.com/user' $token
            $login = [string]$account.login
            @{ login = $login } | ConvertTo-Json | Set-Content -LiteralPath $accountFile -Encoding UTF8
        }
        $access = if ($public) { $true } else { Update-ReplayAccess $token }
        if ($Operation -eq 'status') {
            Write-UiStatus 'idle' $(if ($public) { 'Updates come from public GitHub releases. No sign-in is needed.' }
                elseif ($login -and !$access) { 'This GitHub account is not a collaborator on GMod Replay. Use the public version, or ask Joey for access.' }
                elseif ($login) { 'Your GitHub account is linked. Check for updates whenever you like.' }
                else { 'Link your GitHub account to access private releases. No shared token is used.' })
            return
        }
        if (!$token -and !$public) {
            Write-UiStatus 'unlinked' 'Link your GitHub account before checking private releases.'
            return
        }
        Write-UiStatus 'checking' 'Checking GitHub Releases for a newer version...'
        $releases = @(Invoke-GitHubJson ("https://api.github.com/repos/$($config.repository)/releases?per_page=100") $token)
        $update = Find-ReplayUpdate $releases $config.version
        if ($null -eq $update) {
            Write-UpdateLog "No newer eligible release than $($config.version)."
            Write-UiStatus 'uptodate' "You are up to date ($($config.version))."
            return
        }
        $asset = Get-ReplayInstaller $update $config.repository $(if ($public) { 'GModReplay-Public-Setup' } else { 'GModReplay-Setup' })
        $download = Join-Path $state $asset.Name
        $partial = $download + '.part'
        Write-UiStatus 'downloading' "Downloading and verifying $($update.tag_name)..." @{ release = $update.tag_name }
        $cached = $false
        if (Test-Path -LiteralPath $download) {
            $cached = (Get-Item -LiteralPath $download).Length -eq $asset.Size -and
                (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ieq $asset.Hash
            if (!$cached) { Write-UpdateLog 'Cached installer did not match release metadata; downloading again.' }
        }
        if (!$cached) {
            try {
                Receive-ReplayInstaller $asset $token $partial
                Test-ReplayInstaller $partial $asset
                Move-Item -LiteralPath $partial -Destination $download -Force
            } finally {
                if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial }
            }
        }
        Write-UpdateLog "Downloaded and SHA256-verified $($asset.Name)."
        if ($Operation -ne 'install') {
            Write-UiStatus 'ready' "Update $($update.tag_name) is downloaded and verified. Install when ready; your settings and cached content will be kept." @{
                release = $update.tag_name
            }
            return
        }
        if ($null -eq $viewer) { throw 'The viewer must be running to approve in-app installation.' }
        Write-UiStatus 'waiting' 'Update approved. Close all GMod Replay windows to install. Your demo will not be closed automatically.' @{
            release = $update.tag_name
        }
        while (!$viewer.WaitForExit(1000)) {
            if (Test-UiCancelled) {
                Write-UiStatus 'ready' 'Installation cancelled. The verified update is still downloaded.' @{
                    release = $update.tag_name
                }
                return
            }
        }
        $installAttempted = $true
        Test-ReplayInstaller $download $asset
        $log = Join-Path $state 'installer.log'
        $process = Start-Process -FilePath $download -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES',
            '/NORESTART','/NOCLOSEAPPLICATIONS','/NORESTARTAPPLICATIONS',
            ('/DIR="' + $install + '"'),('/LOG="' + $log + '"')) -PassThru -Wait
        if ($process.ExitCode -ne 0) { throw "Installer exited with code $($process.ExitCode). See $log. Close other viewer instances before retrying." }
        $installed = Get-Content -LiteralPath (Join-Path $install 'release.json') -Raw | ConvertFrom-Json
        if ((Compare-ReplayVersion $installed.version $update.tag_name) -ne 0) { throw 'Installed version does not match the downloaded update.' }
        Write-UpdateLog "Installed $($installed.version)."
        Write-UiStatus 'installed' "Installed $($installed.version). Restarting..."
        Start-Process -FilePath (Join-Path $install 'GModReplay.exe') -WorkingDirectory $install
    } catch {
        Write-UpdateLog ($_.Exception.Message + "`r`n" + $_.ScriptStackTrace)
        $errorMessage = $_.Exception.Message +
            $(if ($edition -eq 'public') { "`nNo unverified installer was run. Retry later." }
              else { "`nNo unverified installer was run. Retry, or unlink and link your account again if access was revoked." })
        Write-UiStatus 'error' $errorMessage
        if ($installAttempted) {
            @{ message = $errorMessage } | ConvertTo-Json | Set-Content -LiteralPath $installResult -Encoding UTF8
            Write-UpdateLog 'Installation failed after exit; reopening the viewer to display the saved error.'
            Start-Process -FilePath (Join-Path $install 'GModReplay.exe') -WorkingDirectory $install
        }
        throw
    } finally {
        if ($null -ne $http) { $http.Dispose() }
        if ($ownsMutex) { $mutex.ReleaseMutex() }
        if ($null -ne $mutex) { $mutex.Dispose() }
    }
}

if ($LoadOnly) { return }
try { Invoke-ReplayUpdates }
catch { exit 1 }
