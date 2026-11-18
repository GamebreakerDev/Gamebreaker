# What the launcher's "Copy logs" button hands over. Dot-sourced by host.ps1.
#
# A report gets pasted into public places, so the text leaves out where the
# player's files are: the Windows profile folder (it holds the user name) and
# the name of a synced folder (a work OneDrive's name holds the employer's).
# The log files on disk are not changed.

function ConvertTo-GbPathPattern([string]$Path) {
    # Either slash, single or doubled: CMake writes C:/Users/..., JSON C:\\Users\\...
    # CMake's object files also spell the path C_/Users/..., with _ for : and for a space.
    $parts = @($Path.TrimEnd('\', '/') -split '[\\/]+' | ForEach-Object { [regex]::Escape($_).Replace('\ ', '[ _]').Replace(':', '[:_]') })
    '(?i)' + ($parts -join '[\\/]+') + '(?![\w.\-])'
}

function Hide-GbUserPaths {
    param(
        [string]$Text,
        [string]$UserProfile = $env:USERPROFILE,
        [string[]]$SyncedFolders = @($env:OneDriveCommercial, $env:OneDriveConsumer, $env:OneDrive)
    )
    if (-not $Text) { return $Text }
    $synced = '%USERPROFILE%\SyncedFolder'
    # Longest first: one synced folder's path can begin with another's.
    foreach ($s in @($SyncedFolders | Where-Object { $_ } | Sort-Object Length -Descending)) {
        $Text = [regex]::Replace($Text, (ConvertTo-GbPathPattern $s), $synced.Replace('$', '$$'))
    }
    if ($UserProfile) { $Text = [regex]::Replace($Text, (ConvertTo-GbPathPattern $UserProfile), '%USERPROFILE%') }
    # The same folder in another spelling, such as a short name (C:\Users\JOSMIT~1).
    $Text = [regex]::Replace($Text, '(?i)\b[A-Z][:_][\\/]+Users[\\/]+[^\\/\s:*?"<>|]+', '%USERPROFILE%')
    # A synced folder that no environment variable named.
    [regex]::Replace($Text, '(?im)%USERPROFILE%[\\/]+(?:OneDrive|Dropbox|Box|Google[ _]Drive|iCloudDrive)(?:[ _]-[ _][^\\/\r\n"]*|[ _]\([^\\/\r\n"]*\))?(?=[\\/"'']|\s*$)', $synced)
}

# The latest setup log, the end of the game's own log and the end of the
# launcher's log. Nothing to report (no setup log and no game log): $null.
function Get-GbLogReport([string]$Folder, [string]$LauncherLog) {
    $logs = Join-Path $Folder 'logs'
    $parts = @()
    $l = Get-ChildItem $logs -Filter 'setup_*.txt' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($l) { $parts += "===== $($l.Name)`r`n" + (Get-Content $l.FullName -Raw) }
    $g = Join-Path $logs 'game.log'
    if (Test-Path $g) { $parts += "===== game.log (last 300 lines)`r`n" + ((Get-Content $g -Tail 300) -join "`r`n") }
    if (-not $parts) { return $null }
    if (Test-Path $LauncherLog) { $parts += "===== launcher`r`n" + ((Get-Content $LauncherLog -Tail 100) -join "`r`n") }
    Hide-GbUserPaths ($parts -join "`r`n`r`n")
}
